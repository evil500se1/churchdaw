#include "../../Source/TimbresProcessor.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#elif JUCE_MAC
 #include <mach/mach.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

static_assert (CHURCHDAW_STREAM_SAMPLES == 1,
               "Builds do produto nao podem voltar a decodificar pianos inteiros");
static_assert (TimbresProcessor::polyphonyHeadroom >= 0.62f
                && TimbresProcessor::polyphonyHeadroom <= 0.64f,
               "0 dB precisa manter a reserva calibrada de aproximadamente -4 dB");
static_assert (TimbresProcessor::organVelocityGain == 1.0f,
               "orgao nativo nao pode voltar a perder nivel pela velocity");
static_assert (TimbresProcessor::nativePadAmbienceTrim > 1.67f
                 && TimbresProcessor::nativePadAmbienceTrim < 1.69f,
               "PAD/AMBIENCE nativo precisa manter o trim calibrado de +4,5 dB");
static_assert (TimbresProcessor::churchOrganFamilyCount == 4,
               "O catalogo precisa expor exatamente quatro familias Church Organ");

namespace
{
size_t privateBytes() noexcept
{
   #if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof (counters);

    if (::GetProcessMemoryInfo (
            ::GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&counters),
            sizeof (counters)) != FALSE)
        return (size_t) counters.PrivateUsage;
   #elif JUCE_MAC
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO,
                   reinterpret_cast<task_info_t> (&info), &count) == KERN_SUCCESS)
        return (size_t) info.resident_size;
   #endif

    return 0;
}

void recordPeak (std::atomic<size_t>& peak) noexcept
{
    const auto value = privateBytes();
    auto previous = peak.load (std::memory_order_relaxed);

    while (value > previous
           && ! peak.compare_exchange_weak (
               previous, value, std::memory_order_relaxed))
    {
    }
}

bool dispatchUntilLoaded (TimbresProcessor& processor,
                          std::atomic<size_t>& peak,
                          int timeoutSeconds)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds (timeoutSeconds);

    while (processor.isLoading()
           && std::chrono::steady_clock::now() < deadline)
    {
        recordPeak (peak);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }

    // Entrega a ultima callAsync, caso loadingCount tenha mudado no mesmo tick.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    recordPeak (peak);
    return ! processor.isLoading();
}

juce::Array<juce::File> collectSamples (const juce::File& directory,
                                        const juce::String& prefix)
{
    juce::Array<juce::File> files;

    for (const auto& file : directory.findChildFiles (
             juce::File::findFiles, false, "*.aif;*.aiff;*.wav;*.flac"))
        if (file.getFileNameWithoutExtension().startsWithIgnoreCase (prefix))
            files.add (file);

    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });

    return files;
}

bool runStreamerWindowContinuityProbe (
    SampleStreamer& streamer,
    SampleStreamer::VoiceStream& voice,
    const std::shared_ptr<const SampleStreamer::Source>& source)
{
    constexpr juce::int64 preloadEnd = 48000;
    constexpr int wideSpan = 12000;

    if (source == nullptr
         || source->totalSamples < preloadEnd + SampleStreamer::windowSamples)
        return false;

    const auto waitForAcquire = [&] (juce::int64 pos, int needed)
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds (5);

        while (std::chrono::steady_clock::now() < deadline)
        {
            const float* left = nullptr;
            const float* right = nullptr;
            int offset = 0;
            SampleStreamer::VoiceStream::ReadAccess access;

            if (voice.acquire (pos, needed, left, right, offset, access))
                return true;

            streamer.wake();
            juce::Thread::sleep (1);
        }

        return false;
    };

    const auto hasWindowAt = [&] (juce::int64 start)
    {
        const int generation = voice.generation.load (std::memory_order_acquire);

        for (int i = 0; i < 2; ++i)
        {
            int expected = 0;

            if (! voice.windowAccess[i].compare_exchange_strong (
                    expected, 1, std::memory_order_acquire,
                    std::memory_order_relaxed))
                continue;

            const bool matches = voice.winGen[i] == generation
                              && voice.winValid[i] > 0
                              && voice.winStart[i] == start;
            voice.releaseWindow (i);

            if (matches)
                return true;
        }

        return false;
    };

    const auto waitForWindowAt = [&] (juce::int64 start)
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds (5);

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (hasWindowAt (start))
                return true;

            streamer.wake();
            juce::Thread::sleep (1);
        }

        return false;
    };

    // A primeira janela deve recuar pelo span REAL. O codigo antigo recuava
    // sempre 8196 e nao cobria [preloadEnd-wideSpan, preloadEnd).
    voice.restart (source.get());

    if (! voice.requestFirstWindow (preloadEnd, wideSpan))
        return false;

    streamer.wake();

    if (! waitForAcquire (preloadEnd - wideSpan, wideSpan))
        return false;

    // Reproduz a sobrescrita entre callbacks: A esta tocando, B ja contem a
    // continuacao com overlap antigo, e um bloco maior pede outra B. O worker
    // deve substituir B, nunca A, mesmo depois de ReadAccess liberar A.
    voice.restart (source.get());

    if (! voice.request (0))
        return false;

    streamer.wake();

    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds (5);
        bool requestedContinuation = false;

        while (std::chrono::steady_clock::now() < deadline)
        {
            const float* left = nullptr;
            const float* right = nullptr;
            int offset = 0;
            SampleStreamer::VoiceStream::ReadAccess access;

            if (voice.acquire (1000, 512, left, right, offset, access))
            {
                voice.prefetch (1000, access);
                requestedContinuation = true;
                break;
            }

            streamer.wake();
            juce::Thread::sleep (1);
        }

        if (! requestedContinuation)
            return false;
    }

    constexpr juce::int64 oldContinuation =
        SampleStreamer::windowSamples - SampleStreamer::windowOverlap;

    streamer.wake();

    if (! waitForWindowAt (oldContinuation))
        return false;

    {
        const float* left = nullptr;
        const float* right = nullptr;
        int offset = 0;
        SampleStreamer::VoiceStream::ReadAccess access;

        if (! voice.acquire (1000, wideSpan,
                             left, right, offset, access))
            return false;

        voice.prefetch (1000, access);
    }

    constexpr juce::int64 widenedContinuation =
        SampleStreamer::windowSamples - wideSpan;

    streamer.wake();

    if (! waitForWindowAt (widenedContinuation))
        return false;

    constexpr juce::int64 nextBlockPosition = 1000 + wideSpan - 4;
    const bool preserved = waitForAcquire (nextBlockPosition, wideSpan);
    voice.stop();
    return preserved;
}

bool runStreamerGenerationStress (const juce::Array<juce::File>& files)
{
    if (files.size() < 2)
        return false;

    auto makeSource = [] (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            formats.createReaderFor (file));

        if (reader == nullptr || reader->lengthInSamples < 8192)
            return std::shared_ptr<const SampleStreamer::Source>();

        auto source = std::make_shared<SampleStreamer::Source>();
        source->file = file;
        source->totalSamples = reader->lengthInSamples;
        source->numChannels = juce::jlimit (1, 2, (int) reader->numChannels);
        return std::shared_ptr<const SampleStreamer::Source> (std::move (source));
    };

    const auto sourceA = makeSource (files.getFirst());
    const auto sourceB = makeSource (files.getLast());

    if (sourceA == nullptr || sourceB == nullptr)
        return false;

    constexpr juce::int64 testStart = 4096;
    constexpr int testSamples = 256;
    juce::AudioBuffer<float> expected (2, testSamples);
    expected.clear();

    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            formats.createReaderFor (sourceB->file));

        if (reader == nullptr
             || ! reader->read (&expected, 0, testSamples,
                                testStart, true, true))
            return false;
    }

    SampleStreamer streamer;
    streamer.prepare (1);
    auto& voice = streamer.stream (0);

    // Forca restart enquanto o worker pode estar abrindo/lendo a fonte anterior.
    // O ultimo pedido e sempre sourceB e seu conteudo e conferido byte-a-byte
    // (com tolerancia float) para detectar janela velha com geracao nova.
    for (int i = 0; i < 100; ++i)
    {
        voice.restart (((i & 1) == 0 ? sourceA : sourceB).get());
        voice.request (testStart);
        streamer.wake();

        if ((i & 3) == 0)
            juce::Thread::yield();
    }

    voice.restart (sourceB.get());
    voice.request (testStart);
    streamer.wake();

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds (5);

    while (std::chrono::steady_clock::now() < deadline)
    {
        const float* left = nullptr;
        const float* right = nullptr;
        int offset = 0;
        SampleStreamer::VoiceStream::ReadAccess access;

        if (voice.acquire (testStart, testSamples,
                           left, right, offset, access))
        {
            const auto* expectedLeft = expected.getReadPointer (0);
            const auto* expectedRight = expected.getReadPointer (1);

            for (int i = 0; i < testSamples; ++i)
                if (std::abs (left[offset + i] - expectedLeft[i]) > 1.0e-6f
                     || std::abs (right[offset + i] - expectedRight[i]) > 1.0e-6f)
                    return false;

            access.reset();
            const bool continuityOk = runStreamerWindowContinuityProbe (
                streamer, voice, sourceB);
            voice.stop();
            return continuityOk;
        }

        streamer.wake();
        juce::Thread::sleep (1);
    }

    return false;
}

struct ProgrammaticToneListener final
    : juce::AudioProcessorListener
{
    explicit ProgrammaticToneListener (TimbresProcessor& ownerIn)
        : owner (ownerIn)
    {
    }

    void audioProcessorParameterChanged (
        juce::AudioProcessor*, int, float) override
    {
        ++callbacks;

        if (! owner.isProgrammaticToneChange())
            ++unguardedCallbacks;
    }

    void audioProcessorChanged (
        juce::AudioProcessor*,
        const juce::AudioProcessorListener::ChangeDetails&) override
    {
    }

    TimbresProcessor& owner;
    int callbacks = 0;
    int unguardedCallbacks = 0;
};

bool runChannelControlStateProbe()
{
    // Matriz pura de roteamento para os oito canais de palco. O predicado de
    // SOLO e o mesmo usado por prepareToPlay/renderVoices; o restante explicita
    // a precedencia de ON, MUTE e SOLO sem depender de timbre ou hardware.
    struct RoutingMaskCase
    {
        juce::uint32 enabled;
        juce::uint32 muted;
        juce::uint32 solo;
        juce::uint32 expectedAudible;
    };

    constexpr juce::uint32 loadedEight = 0x00ffu;
    constexpr juce::uint32 allLayerBits =
        (juce::uint32 (1) << TimbresProcessor::numLayers) - 1u;
    constexpr std::array<RoutingMaskCase, 8> routingMatrix {{
        { 0x00ffu, 0x0000u, 0x0000u, 0x00ffu }, // baseline
        { 0x00ffu, 0x0000u, 0x0001u, 0x0001u }, // SOLO C1
        { 0x00ffu, 0x0000u, 0x0005u, 0x0005u }, // SOLO C1+C3
        { 0x00ffu, 0x0001u, 0x0005u, 0x0004u }, // MUTE vence SOLO C1
        { 0x00fdu, 0x0000u, 0x0000u, 0x00fdu }, // C2 desligado
        { 0x00ffu, 0x0000u, 0x2000u, 0x00ffu }, // SOLO fantasma C14
        { 0x00ffu, 0x0000u, 0x2001u, 0x0001u }, // SOLO C1 + fantasma
        { 0x00ffu, 0x0000u, 0x0000u, 0x00ffu }  // limpar todos os SOLOS
    }};

    for (const auto& state : routingMatrix)
    {
        const bool hasAnySolo =
            TimbresProcessor::hasAnySoloInMasks (
                loadedEight, state.solo);
        const auto audible = loadedEight
                           & state.enabled
                           & ~state.muted
                           & (hasAnySolo
                                  ? state.solo
                                  : allLayerBits);

        if (audible != state.expectedAudible)
            return false;
    }

    // TimbresProcessor is a production AudioProcessor owned on the heap by the
    // graph. Keep the probe faithful to that ownership: several simultaneous
    // stack instances can exceed the default Windows test-process stack now
    // that the realtime MIDI ledger tracks overlapping note-ons.
    auto source = std::make_unique<TimbresProcessor>();

    const auto findFloatParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterFloat*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterFloat*> (parameter);

        return nullptr;
    };

    const auto findIntParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterInt*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterInt*> (parameter);

        return nullptr;
    };

    const auto findBoolParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterBool*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterBool*> (parameter);

        return nullptr;
    };

    // Nenhum controle novo pode deslocar os IDs/indices usados pelas versoes
    // ja instaladas, hosts e mapas MIDI.
    const juce::StringArray legacyLayerZeroOrder {
        "vol0", "transp0", "loop0", "mute0", "solo0", "on0",
        "comp0", "dly0", "lcut0", "hcut0", "aeq0", "rvb0"
    };

    const auto& parameters = source->getParameters();

    if (parameters.size() < legacyLayerZeroOrder.size())
        return false;

    for (int i = 0; i < legacyLayerZeroOrder.size(); ++i)
    {
        const auto* identified =
            dynamic_cast<const juce::AudioProcessorParameterWithID*> (
                parameters.getUnchecked (i));

        if (identified == nullptr
             || identified->paramID != legacyLayerZeroOrder[i])
            return false;
    }

    *source->getLayerAttackParam (0) = 0.8f;
    *source->getLayerReleaseParam (0) = 8.0f;
    *source->getLayerLowEqParam (0) = 8.0f;
    *source->getLayerMidEqParam (0) = -8.0f;
    *source->getLayerHighEqParam (0) = 8.0f;
    ProgrammaticToneListener toneListener (*source);
    source->addListener (&toneListener);
    source->applyFactoryTimbreShape (
        0, "Felt Upright", "PIANOS");
    source->removeListener (&toneListener);

    if (toneListener.callbacks < 5
         || toneListener.unguardedCallbacks != 0)
        return false;

    *source->getLayerAttackParam (0) = 0.137f;
    *source->getLayerReleaseParam (0) = 3.25f;
    *source->getLayerLowEqParam (0) = 2.5f;
    *source->getLayerMidEqParam (0) = -3.5f;
    *source->getLayerHighEqParam (0) = 1.75f;
    *source->getLayerRotaryModeParam (0) =
        ChurchDAWRotary::Speaker::fastMode;
    *source->getLayerRotaryAmountParam (0) = 0.77f;
    *source->getLayerNordFilterOnParam (0) = true;
    *source->getLayerNordFilterTypeParam (0) =
        ChurchDAWNordSample::bandPass;
    *source->getLayerNordFilterCutoffParam (0) = 1700.0f;
    *source->getLayerNordFilterResonanceParam (0) = 0.42f;
    *source->getLayerNordFilterKeyTrackParam (0) = 2;
    *source->getLayerNordFilterDriveParam (0) = 3;
    *source->getLayerNordFilterBrightParam (0) = true;
    *source->getLayerNordFilterEnvAmountParam (0) = -0.37f;
    *source->getLayerNordFilterEnvAttackParam (0) = 0.123f;
    *source->getLayerNordFilterEnvDecayParam (0) = 2.45f;
    *source->getLayerNordFilterEnvReleaseParam (0) = 3.67f;
    *source->getLayerNordFilterVelocityParam (0) = true;
    *source->getLayerNordAmpEqOnParam (0) = true;
    *source->getLayerNordAmpEqBassParam (0) = -12.5f;
    *source->getLayerNordAmpEqMidParam (0) = 7.25f;
    *source->getLayerNordAmpEqMidHzParam (0) = 2440.0f;
    *source->getLayerNordAmpEqTrebleParam (0) = 4.5f;
    auto* sourceChannelTranspose = findIntParameter (*source, "transp0");
    auto* sourceGlobalTranspose = dynamic_cast<juce::AudioParameterInt*> (
        source->getGlobalTranspParam());

    if (sourceGlobalTranspose == nullptr || sourceChannelTranspose == nullptr)
        return false;

    *sourceChannelTranspose = -4;
    *sourceGlobalTranspose = 5;
    auto* sourceSolo = findBoolParameter (*source, "solo0");
    auto* sourceLoop = findBoolParameter (*source, "loop0");

    if (sourceSolo == nullptr || sourceLoop == nullptr)
        return false;

    *sourceSolo = true;
    *sourceLoop = true;   // tombstone legado deve ser salvo/restaurado como false

    juce::MemoryBlock state;
    source->getStateInformation (state);

    if (state.isEmpty())
        return false;

    // SOLO e uma escuta temporaria: nao pode atravessar save/load nem
    // sobreviver quando o canal que o possuia e removido.
    auto soloStateXml = juce::AudioProcessor::getXmlFromBinary (
        state.getData(), (int) state.getSize());

    if (soloStateXml == nullptr)
        return false;

    bool serializedSoloWasOff = false;
    bool serializedLoopWasOff = false;

    forEachXmlChildElementWithTagName (
        *soloStateXml, layer, "LAYER")
        if (layer->getIntAttribute ("index", -1) == 0)
        {
            serializedSoloWasOff =
                ! layer->getBoolAttribute ("solo", true);
            serializedLoopWasOff =
                ! layer->getBoolAttribute ("loop", true);
            // Simula setup legado/macOS salvo com o bit temporario ligado.
            layer->setAttribute ("solo", true);
            layer->setAttribute ("loop", true);
            break;
        }

    if (! serializedSoloWasOff || ! serializedLoopWasOff)
        return false;

    juce::MemoryBlock legacySoloState;
    juce::AudioProcessor::copyXmlToBinary (
        *soloStateXml, legacySoloState);
    auto legacySoloRestored = std::make_unique<TimbresProcessor>();
    legacySoloRestored->setStateInformation (
        legacySoloState.getData(), (int) legacySoloState.getSize());

    if (auto* restoredSolo = findBoolParameter (
            *legacySoloRestored, "solo0");
        restoredSolo == nullptr || restoredSolo->get())
        return false;

    if (auto* restoredLoop = findBoolParameter (
            *legacySoloRestored, "loop0");
        restoredLoop == nullptr || restoredLoop->get())
        return false;

    *sourceSolo = true;
    source->clearLayer (0);

    if (sourceSolo->get())
        return false;

    auto restored = std::make_unique<TimbresProcessor>();
    restored->setStateInformation (state.getData(), (int) state.getSize());

    const auto close = [] (float a, float b)
    {
        return std::abs (a - b) < 0.0001f;
    };

    const bool currentStateOk =
           close (restored->getLayerAttackParam (0)->get(), 0.137f)
        && close (restored->getLayerReleaseParam (0)->get(), 3.25f)
        && close (restored->getLayerLowEqParam (0)->get(), 2.5f)
        && close (restored->getLayerMidEqParam (0)->get(), -3.5f)
        && close (restored->getLayerHighEqParam (0)->get(), 1.75f)
        && restored->getLayerRotaryModeParam (0)->getIndex()
              == ChurchDAWRotary::Speaker::fastMode
        && close (restored->getLayerRotaryAmountParam (0)->get(), 0.77f)
        && dynamic_cast<juce::AudioParameterInt*> (
               restored->getGlobalTranspParam()) != nullptr
        && dynamic_cast<juce::AudioParameterInt*> (
               restored->getGlobalTranspParam())->get() == 5
        && findIntParameter (*restored, "transp0") != nullptr
        && findIntParameter (*restored, "transp0")->get() == -4
        && findBoolParameter (*restored, "loop0") != nullptr
        && ! findBoolParameter (*restored, "loop0")->get()
        && restored->getLayerNordFilterOnParam (0)->get()
        && restored->getLayerNordFilterTypeParam (0)->getIndex()
              == ChurchDAWNordSample::bandPass
        && close (restored->getLayerNordFilterCutoffParam (0)->get(), 1700.0f)
        && close (restored->getLayerNordFilterResonanceParam (0)->get(), 0.42f)
        && restored->getLayerNordFilterKeyTrackParam (0)->getIndex() == 2
        && restored->getLayerNordFilterDriveParam (0)->getIndex() == 3
        && restored->getLayerNordFilterBrightParam (0)->get()
        && close (restored->getLayerNordFilterEnvAmountParam (0)->get(), -0.37f)
        && close (restored->getLayerNordFilterEnvAttackParam (0)->get(), 0.123f)
        && close (restored->getLayerNordFilterEnvDecayParam (0)->get(), 2.45f)
        && close (restored->getLayerNordFilterEnvReleaseParam (0)->get(), 3.67f)
        && restored->getLayerNordFilterVelocityParam (0)->get()
        && restored->getLayerNordAmpEqOnParam (0)->get()
        && close (restored->getLayerNordAmpEqBassParam (0)->get(), -12.5f)
        && close (restored->getLayerNordAmpEqMidParam (0)->get(), 7.25f)
        && close (restored->getLayerNordAmpEqMidHzParam (0)->get(), 2440.0f)
        && close (restored->getLayerNordAmpEqTrebleParam (0)->get(), 4.5f);

    if (! currentStateOk)
        return false;

    // Uma pagina de cena que referencia plugin ainda indisponivel nao pode ser
    // declarada pronta pela metade. O alvo fica pendente para autocura; uma
    // limpeza/substituicao explicita encerra corretamente essa pendencia.
    auto missingPluginXml = juce::AudioProcessor::getXmlFromBinary (
        state.getData(), (int) state.getSize());

    if (missingPluginXml == nullptr)
        return false;

    forEachXmlChildElementWithTagName (
        *missingPluginXml, layer, "LAYER")
        if (layer->getIntAttribute ("index", -1) == 0)
        {
            layer->setAttribute (
                "vstId", "missing-scene-plugin-identifier");
            layer->setAttribute ("vstState", "");
            layer->setAttribute ("timbre", "");
            break;
        }

    juce::MemoryBlock missingPluginState;
    juce::AudioProcessor::copyXmlToBinary (
        *missingPluginXml, missingPluginState);
    auto pendingScenePage = std::make_unique<TimbresProcessor>();
    pendingScenePage->setStateInformation (
        missingPluginState.getData(),
        (int) missingPluginState.getSize());

    if (! pendingScenePage->hasPendingStateRestore())
        return false;

    pendingScenePage->clearLayer (0);

    if (pendingScenePage->hasPendingStateRestore())
        return false;

    // Uma LAYER ausente representa canal vazio, nao "deixe os controles do
    // projeto anterior". Suje o canal 2, remova-o do XML e confira o reset
    // substitutivo de volume/ENV/EQ.
    auto partialXml = juce::AudioProcessor::getXmlFromBinary (
        state.getData(), (int) state.getSize());

    if (partialXml == nullptr)
        return false;

    juce::XmlElement* omittedLayer = nullptr;
    forEachXmlChildElementWithTagName (
        *partialXml, candidate, "LAYER")
        if (candidate->getIntAttribute ("index", -1) == 1)
        {
            omittedLayer = candidate;
            break;
        }

    if (omittedLayer == nullptr)
        return false;

    partialXml->removeChildElement (omittedLayer, true);
    juce::MemoryBlock partialState;
    juce::AudioProcessor::copyXmlToBinary (*partialXml, partialState);
    auto partialRestored = std::make_unique<TimbresProcessor>();
    *partialRestored->getLayerAttackParam (1) = 1.5f;
    *partialRestored->getLayerReleaseParam (1) = 10.0f;
    *partialRestored->getLayerLowEqParam (1) = 9.0f;
    *partialRestored->getLayerMidEqParam (1) = -7.0f;
    *partialRestored->getLayerHighEqParam (1) = 6.0f;
    auto* partialVolume = dynamic_cast<juce::AudioParameterFloat*> (
        partialRestored->getLayerVolParam (1));

    if (partialVolume == nullptr)
        return false;

    *partialVolume = 0.2f;
    partialRestored->setStateInformation (
        partialState.getData(), (int) partialState.getSize());

    if (! close (partialRestored->getLayerAttackParam (1)->get(), 0.003f)
         || ! close (partialRestored->getLayerReleaseParam (1)->get(), 0.55f)
         || ! close (partialRestored->getLayerLowEqParam (1)->get(), 0.0f)
         || ! close (partialRestored->getLayerMidEqParam (1)->get(), 0.0f)
         || ! close (partialRestored->getLayerHighEqParam (1)->get(), 0.0f)
         || ! close (partialVolume->get(), 1.0f))
        return false;

    // O banco A->B->A precisa atravessar o estado real do processador, nao
    // apenas a classe isolada. Injeta um perfil como faria uma sessao salva,
    // restaura e salva novamente; builds antigos ignoram o filho TONE.
    auto profileXml = juce::AudioProcessor::getXmlFromBinary (
        state.getData(), (int) state.getSize());

    if (profileXml == nullptr)
        return false;

    auto* profile = profileXml->createNewChildElement ("TONE");
    profile->setAttribute ("layer", 0);
    profile->setAttribute ("timbre", "Stored Piano");
    profile->setAttribute ("lib", "Church Live");
    profile->setAttribute ("attack", 0.241);
    profile->setAttribute ("release", 4.75);
    profile->setAttribute ("eqLow", 3.0);
    profile->setAttribute ("eqMid", -2.0);
    profile->setAttribute ("eqHigh", 1.25);
    profile->setAttribute ("lcut", 0.0);
    profile->setAttribute ("hcut", 0.0);
    profile->setAttribute ("aeq", false);
    profile->setAttribute (
        "rotMode", ChurchDAWRotary::Speaker::stopMode);
    profile->setAttribute ("rotAmt", 0.0);
    profile->setAttribute ("nfOn", true);
    profile->setAttribute ("nfType", ChurchDAWNordSample::lpM);
    profile->setAttribute ("nfCut", 1320.0);
    profile->setAttribute ("nfRes", 0.58);
    profile->setAttribute ("nfTrack", 3);
    profile->setAttribute ("nfDrive", 2);
    profile->setAttribute ("nfBright", true);
    profile->setAttribute ("nfEnvAmt", 0.44);
    profile->setAttribute ("nfEnvA", 0.082);
    profile->setAttribute ("nfEnvD", 1.75);
    profile->setAttribute ("nfEnvR", 2.25);
    profile->setAttribute ("nfVel", true);
    profile->setAttribute ("nEqOn", true);
    profile->setAttribute ("nEqBass", -9.0);
    profile->setAttribute ("nEqMid", 6.5);
    profile->setAttribute ("nEqMidHz", 1777.0);
    profile->setAttribute ("nEqTreble", 3.25);

    juce::MemoryBlock profileState;
    juce::AudioProcessor::copyXmlToBinary (*profileXml, profileState);
    auto profileRestored = std::make_unique<TimbresProcessor>();
    profileRestored->setStateInformation (
        profileState.getData(), (int) profileState.getSize());

    juce::MemoryBlock profileRoundTrip;
    profileRestored->getStateInformation (profileRoundTrip);
    auto roundTripXml = juce::AudioProcessor::getXmlFromBinary (
        profileRoundTrip.getData(), (int) profileRoundTrip.getSize());

    if (roundTripXml == nullptr
         || roundTripXml->getIntAttribute ("toneSchema", 0) != 4)
        return false;

    const juce::XmlElement* roundTripProfile = nullptr;

    forEachXmlChildElementWithTagName (
        *roundTripXml, candidate, "TONE")
        if (candidate->getIntAttribute ("layer", -1) == 0
             && candidate->getStringAttribute ("timbre")
                    == "Stored Piano"
             && candidate->getStringAttribute ("lib")
                    == "Church Live")
        {
            roundTripProfile = candidate;
            break;
        }

    if (roundTripProfile == nullptr
         || ! close (
                (float) roundTripProfile->getDoubleAttribute (
                    "attack"), 0.241f)
         || ! close (
                (float) roundTripProfile->getDoubleAttribute (
                    "release"), 4.75f)
         || ! close (
                (float) roundTripProfile->getDoubleAttribute (
                    "eqLow"), 3.0f)
         || ! close (
                (float) roundTripProfile->getDoubleAttribute (
                    "eqMid"), -2.0f)
         || ! close (
                (float) roundTripProfile->getDoubleAttribute (
                    "eqHigh"), 1.25f)
         || ! roundTripProfile->getBoolAttribute ("nfOn", false)
         || roundTripProfile->getIntAttribute ("nfType", -1)
                != ChurchDAWNordSample::lpM
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfCut"), 1320.0f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfRes"), 0.58f)
         || roundTripProfile->getIntAttribute ("nfTrack", -1) != 3
         || roundTripProfile->getIntAttribute ("nfDrive", -1) != 2
         || ! roundTripProfile->getBoolAttribute ("nfBright", false)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfEnvAmt"), 0.44f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfEnvA"), 0.082f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfEnvD"), 1.75f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nfEnvR"), 2.25f)
         || ! roundTripProfile->getBoolAttribute ("nfVel", false)
         || ! roundTripProfile->getBoolAttribute ("nEqOn", false)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nEqBass"), -9.0f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nEqMid"), 6.5f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nEqMidHz"), 1777.0f)
         || ! close ((float) roundTripProfile->getDoubleAttribute (
                "nEqTreble"), 3.25f))
        return false;

    // Sessao criada antes do Rotary nao pode ligar Leslie so porque o app foi
    // atualizado. A migracao precisa permanecer integralmente em bypass.
    auto legacyXml = juce::AudioProcessor::getXmlFromBinary (
        state.getData(), (int) state.getSize());

    if (legacyXml == nullptr)
        return false;

    forEachXmlChildElementWithTagName (*legacyXml, layer, "LAYER")
    {
        layer->setAttribute ("lcut", 0.32);
        layer->setAttribute ("hcut", 0.25);
        layer->removeAttribute ("attack");
        layer->removeAttribute ("release");
        layer->removeAttribute ("eqLow");
        layer->removeAttribute ("eqMid");
        layer->removeAttribute ("eqHigh");
        layer->removeAttribute ("mid");
        layer->removeAttribute ("rotMode");
        layer->removeAttribute ("rotAmt");
        layer->removeAttribute ("nfOn");
        layer->removeAttribute ("nfType");
        layer->removeAttribute ("nfCut");
        layer->removeAttribute ("nfRes");
        layer->removeAttribute ("nfTrack");
        layer->removeAttribute ("nfDrive");
        layer->removeAttribute ("nfBright");
        layer->removeAttribute ("nfEnvAmt");
        layer->removeAttribute ("nfEnvA");
        layer->removeAttribute ("nfEnvD");
        layer->removeAttribute ("nfEnvR");
        layer->removeAttribute ("nfVel");
        layer->removeAttribute ("nEqOn");
        layer->removeAttribute ("nEqBass");
        layer->removeAttribute ("nEqMid");
        layer->removeAttribute ("nEqMidHz");
        layer->removeAttribute ("nEqTreble");
        layer->setAttribute ("transp", -9);
    }
    legacyXml->removeAttribute ("toneSchema");
    legacyXml->removeAttribute ("transposeSchema");
    legacyXml->setAttribute ("gtransp", 7);

    juce::MemoryBlock legacyState;
    juce::AudioProcessor::copyXmlToBinary (*legacyXml, legacyState);
    auto legacyRestored = std::make_unique<TimbresProcessor>();
    legacyRestored->setStateInformation (
        legacyState.getData(), (int) legacyState.getSize());

    if (legacyRestored->getLayerRotaryModeParam (0)->getIndex()
            != ChurchDAWRotary::Speaker::stopMode
         || ! close (legacyRestored->getLayerRotaryAmountParam (0)->get(), 0.0f)
         || ! close (legacyRestored->getLayerAttackParam (0)->get(), 0.003f)
         || ! close (legacyRestored->getLayerReleaseParam (0)->get(), 0.55f)
         || ! close (legacyRestored->getLayerLowEqParam (0)->get(), 0.0f)
         || ! close (legacyRestored->getLayerMidEqParam (0)->get(), 0.0f)
         || ! close (legacyRestored->getLayerHighEqParam (0)->get(), 0.0f)
         || legacyRestored->getLayerNordFilterOnParam (0)->get()
         || legacyRestored->getLayerNordFilterTypeParam (0)->getIndex()
                != ChurchDAWNordSample::lp24
         || ! close (legacyRestored->getLayerNordFilterCutoffParam (0)->get(), 18000.0f)
         || legacyRestored->getLayerNordAmpEqOnParam (0)->get()
         || dynamic_cast<juce::AudioParameterInt*> (
                legacyRestored->getGlobalTranspParam()) == nullptr
         || dynamic_cast<juce::AudioParameterInt*> (
                legacyRestored->getGlobalTranspParam())->get() != 0
         || findIntParameter (*legacyRestored, "transp0") == nullptr
         || findIntParameter (*legacyRestored, "transp0")->get() != 0)
        return false;

    auto* legacyLowCut = findFloatParameter (*legacyRestored, "lcut0");
    auto* legacyHighCut = findFloatParameter (*legacyRestored, "hcut0");

    if (legacyLowCut == nullptr || legacyHighCut == nullptr
         || ! close (legacyLowCut->get(), 0.32f)
         || ! close (legacyHighCut->get(), 0.25f))
        return false;

    // O formato ainda mais antigo <TIMBRES> tambem precisa nascer com Rotary
    // integralmente em bypass.
    juce::XmlElement oldestXml ("TIMBRES");
    oldestXml.setAttribute ("transpose", 0);
    oldestXml.setAttribute ("gain", 1.0);
    oldestXml.setAttribute ("loop", true);
    oldestXml.setAttribute ("timbre", "");
    juce::MemoryBlock oldestState;
    juce::AudioProcessor::copyXmlToBinary (oldestXml, oldestState);
    auto oldestRestored = std::make_unique<TimbresProcessor>();
    *oldestRestored->getLayerAttackParam (1) = 1.25f;
    *oldestRestored->getLayerReleaseParam (1) = 9.0f;
    *oldestRestored->getLayerLowEqParam (1) = 6.0f;
    *oldestRestored->getLayerMidEqParam (1) = -4.0f;
    *oldestRestored->getLayerHighEqParam (1) = 3.0f;
    oldestRestored->setStateInformation (
        oldestState.getData(), (int) oldestState.getSize());

    if (oldestRestored->getLayerRotaryModeParam (0)->getIndex()
            != ChurchDAWRotary::Speaker::stopMode
         || ! close (oldestRestored->getLayerRotaryAmountParam (0)->get(), 0.0f)
         || ! close (oldestRestored->getLayerAttackParam (1)->get(), 0.003f)
         || ! close (oldestRestored->getLayerReleaseParam (1)->get(), 0.55f)
         || ! close (oldestRestored->getLayerLowEqParam (1)->get(), 0.0f)
         || ! close (oldestRestored->getLayerMidEqParam (1)->get(), 0.0f)
         || ! close (oldestRestored->getLayerHighEqParam (1)->get(), 0.0f)
         || findBoolParameter (*oldestRestored, "loop0") == nullptr
         || findBoolParameter (*oldestRestored, "loop0")->get())
        return false;

    // Um timbre novo preserva o preset historico de pad (HPF 0,32), mas deixa
    // o novo EQ em neutro ate o cliente decidir mexer.
    auto factory = std::make_unique<TimbresProcessor>();
    factory->applyFactoryTimbreShape (0, "Airy Pad", "PADS");
    auto* factoryLowCut = findFloatParameter (*factory, "lcut0");

    if (factoryLowCut == nullptr
         || ! close (factoryLowCut->get(), 0.32f)
         || ! close (factory->getLayerLowEqParam (0)->get(), 0.0f)
         || ! close (factory->getLayerMidEqParam (0)->get(), 0.0f)
         || ! close (factory->getLayerHighEqParam (0)->get(), 0.0f))
        return false;

    // Orgao nativo recebe o Leslie worship; VST/AU externo parte em bypass,
    // pois B-3/VB3/Hammond normalmente ja traz Rotary proprio.
    factory->applyFactoryTimbreShape (
        1, "Church Hammond Organ", "ORGANS");
    factory->applyFactoryTimbreShape (
        2, "VB3 Organ", "ORGANS", false);

    if (factory->getLayerRotaryModeParam (1)->getIndex()
            != ChurchDAWRotary::Speaker::slowMode
         || ! close (factory->getLayerRotaryAmountParam (1)->get(), 0.65f)
         || factory->getLayerRotaryModeParam (2)->getIndex()
            != ChurchDAWRotary::Speaker::stopMode
         || ! close (factory->getLayerRotaryAmountParam (2)->get(), 0.0f))
        return false;

    // Estado corrompido/adversarial nunca ultrapassa os limites publicos.
    if (auto* layer = legacyXml->getChildByName ("LAYER"))
    {
        layer->setAttribute ("rotMode", 999);
        layer->setAttribute ("rotAmt", 99.0);
    }

    juce::MemoryBlock invalidState;
    juce::AudioProcessor::copyXmlToBinary (*legacyXml, invalidState);
    auto invalidRestored = std::make_unique<TimbresProcessor>();
    invalidRestored->setStateInformation (
        invalidState.getData(), (int) invalidState.getSize());

    if (invalidRestored->getLayerRotaryModeParam (0)->getIndex()
            != ChurchDAWRotary::Speaker::fastMode
         || ! close (
                invalidRestored->getLayerRotaryAmountParam (0)->get(),
                1.0f))
        return false;

    // Apagar a cena ATIVA precisa produzir uma pagina realmente vazia em
    // todos os canais. clearLayer() sozinho preserva controles de proposito;
    // a operacao explicita de cena deve, alem de remover o timbre, deixar o
    // fader em -inf, canal OFF e ENV/FX/transpose neutros.
    auto emptyScene = std::make_unique<TimbresProcessor>();

    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
    {
        emptyScene->getLayerVolParam (layer)->setValueNotifyingHost (0.83f);
        emptyScene->getLayerOnParam (layer)->setValueNotifyingHost (1.0f);
        *emptyScene->getLayerMuteParam (layer) = true;
        *emptyScene->getLayerSoloParam (layer) = true;
        *emptyScene->getLayerTransposeParam (layer) = 7;
        *emptyScene->getLayerOctaveParam (layer) = -2;
        emptyScene->getLayerCompParam (layer)->setValueNotifyingHost (0.74f);
        *emptyScene->getLayerDelayParam (layer) = 0.68f;
        *emptyScene->getLayerAttackParam (layer) = 1.2f;
        *emptyScene->getLayerReleaseParam (layer) = 9.0f;
        *emptyScene->getLayerLowCutParam (layer) = 0.45f;
        *emptyScene->getLayerHighCutParam (layer) = 0.35f;
        *emptyScene->getLayerLowEqParam (layer) = 6.0f;
        *emptyScene->getLayerMidEqParam (layer) = -5.0f;
        *emptyScene->getLayerHighEqParam (layer) = 4.0f;
        *emptyScene->getLayerReverbSendParam (layer) = 0.91f;
        *emptyScene->getLayerRotaryModeParam (layer) =
            ChurchDAWRotary::Speaker::fastMode;
        *emptyScene->getLayerRotaryAmountParam (layer) = 0.88f;
        *emptyScene->getLayerNordFilterOnParam (layer) = true;
        *emptyScene->getLayerNordFilterTypeParam (layer) =
            ChurchDAWNordSample::highPass;
        *emptyScene->getLayerNordFilterCutoffParam (layer) = 3200.0f;
        *emptyScene->getLayerNordFilterResonanceParam (layer) = 0.91f;
        *emptyScene->getLayerNordFilterKeyTrackParam (layer) = 3;
        *emptyScene->getLayerNordFilterDriveParam (layer) = 3;
        *emptyScene->getLayerNordFilterBrightParam (layer) = true;
        *emptyScene->getLayerNordFilterEnvAmountParam (layer) = 0.8f;
        *emptyScene->getLayerNordFilterEnvAttackParam (layer) = 1.2f;
        *emptyScene->getLayerNordFilterEnvDecayParam (layer) = 2.3f;
        *emptyScene->getLayerNordFilterEnvReleaseParam (layer) = 4.5f;
        *emptyScene->getLayerNordFilterVelocityParam (layer) = true;
        *emptyScene->getLayerNordAmpEqOnParam (layer) = true;
        *emptyScene->getLayerNordAmpEqBassParam (layer) = 12.0f;
        *emptyScene->getLayerNordAmpEqMidParam (layer) = -11.0f;
        *emptyScene->getLayerNordAmpEqMidHzParam (layer) = 6400.0f;
        *emptyScene->getLayerNordAmpEqTrebleParam (layer) = 9.0f;

        emptyScene->clearLayer (layer);
        emptyScene->resetClearedLayerForEmptyScene (layer);

        if (! close (emptyScene->getLayerVolParam (layer)->getValue(), 0.0f)
             || emptyScene->getLayerOnParam (layer)->getValue() > 0.5f
             || emptyScene->getLayerMuteParam (layer)->get()
             || emptyScene->getLayerSoloParam (layer)->get()
             || emptyScene->getLayerTransposeParam (layer)->get() != 0
             || emptyScene->getLayerOctaveParam (layer)->get() != 0
             || ! close (emptyScene->getLayerCompParam (layer)->getValue(), 0.0f)
             || ! close (emptyScene->getLayerDelayParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerAttackParam (layer)->get(), 0.003f)
             || ! close (emptyScene->getLayerReleaseParam (layer)->get(), 0.55f)
             || ! close (emptyScene->getLayerLowCutParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerHighCutParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerLowEqParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerMidEqParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerHighEqParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerReverbSendParam (layer)->get(), 0.0f)
             || emptyScene->getLayerRotaryModeParam (layer)->getIndex()
                    != ChurchDAWRotary::Speaker::stopMode
             || ! close (emptyScene->getLayerRotaryAmountParam (layer)->get(), 0.0f)
             || emptyScene->getLayerNordFilterOnParam (layer)->get()
             || emptyScene->getLayerNordFilterTypeParam (layer)->getIndex()
                    != ChurchDAWNordSample::lp24
             || ! close (emptyScene->getLayerNordFilterCutoffParam (layer)->get(), 18000.0f)
             || ! close (emptyScene->getLayerNordFilterResonanceParam (layer)->get(), 0.0f)
             || emptyScene->getLayerNordFilterKeyTrackParam (layer)->getIndex() != 0
             || emptyScene->getLayerNordFilterDriveParam (layer)->getIndex() != 0
             || emptyScene->getLayerNordFilterBrightParam (layer)->get()
             || ! close (emptyScene->getLayerNordFilterEnvAmountParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerNordFilterEnvAttackParam (layer)->get(), 0.001f)
             || ! close (emptyScene->getLayerNordFilterEnvDecayParam (layer)->get(), 10.0f)
             || ! close (emptyScene->getLayerNordFilterEnvReleaseParam (layer)->get(), 0.55f)
             || emptyScene->getLayerNordFilterVelocityParam (layer)->get()
             || emptyScene->getLayerNordAmpEqOnParam (layer)->get()
             || ! close (emptyScene->getLayerNordAmpEqBassParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerNordAmpEqMidParam (layer)->get(), 0.0f)
             || ! close (emptyScene->getLayerNordAmpEqMidHzParam (layer)->get(), 850.0f)
             || ! close (emptyScene->getLayerNordAmpEqTrebleParam (layer)->get(), 0.0f)
             || emptyScene->getLayerTimbre (layer).isNotEmpty())
            return false;
    }

    // Autosave concorrente com ajustes programaticos nao pode deadlockar nem
    // produzir XML truncado. O lock transacional cobre identidade + ENV/EQ.
    auto concurrent = std::make_unique<TimbresProcessor>();
    std::atomic<bool> snapshotsValid { true };
    std::thread saver ([&]
    {
        for (int i = 0; i < 250; ++i)
        {
            juce::MemoryBlock snapshot;
            concurrent->getStateInformation (snapshot);
            const auto snapshotXml =
                juce::AudioProcessor::getXmlFromBinary (
                    snapshot.getData(), (int) snapshot.getSize());

            if (snapshotXml == nullptr
                 || ! snapshotXml->hasTagName ("TIMBRES2"))
            {
                snapshotsValid.store (
                    false, std::memory_order_release);
                return;
            }
        }
    });

    for (int i = 0; i < 250; ++i)
        concurrent->applyFactoryTimbreShape (
            0, (i & 1) == 0 ? "Felt Upright" : "Airy Pad",
            (i & 1) == 0 ? "PIANOS" : "PADS");

    saver.join();
    return snapshotsValid.load (std::memory_order_acquire);
}

bool writeProbeWav (const juce::File& file, float frequency)
{
    auto output = file.createOutputStream();

    if (output == nullptr)
        return false;

    auto* rawOutput = output.release();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (
            rawOutput, 48000.0, 1, 16, {}, 0));

    if (writer == nullptr)
    {
        delete rawOutput;
        return false;
    }

    juce::AudioBuffer<float> audio (1, 8192);

    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (
            0, i,
            0.2f * std::sin (
                juce::MathConstants<float>::twoPi
                * frequency * (float) i / 48000.0f));

    return writer->writeFromAudioSampleBuffer (
        audio, 0, audio.getNumSamples());
}

bool runParallelNativeRenderParityProbe()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int notesPerLayer = 16;

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-parallel-render-parity", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeProbeWav (sample, 733.0f))
        return false;

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Parallel Native Parity__";
    entry.library = "Probe Pack";
    entry.category = "PADS";
    entry.files.add (sample);

    const auto prepare = [&] (TimbresProcessor& processor, bool parallel)
    {
        processor.setParallelVoiceRenderingForTest (parallel);
        processor.prepareToPlay (sampleRate, blockSize);
        std::atomic<size_t> memoryPeak { 0 };

        for (int layer = 0; layer < 2; ++layer)
        {
            processor.loadTimbreIntoLayer (layer, entry, false);

            if (! dispatchUntilLoaded (processor, memoryPeak, 10)
                 || processor.getLayerTimbre (layer) != entry.label)
                return false;

            *processor.getLayerAttackParam (layer) = 0.001f;
            *processor.getLayerReleaseParam (layer) = 0.08f;
            *processor.getLayerLowEqParam (layer) = 0.0f;
            *processor.getLayerMidEqParam (layer) = 0.0f;
            *processor.getLayerHighEqParam (layer) = 0.0f;
            *processor.getLayerRotaryModeParam (layer) =
                ChurchDAWRotary::Speaker::stopMode;
            *processor.getLayerRotaryAmountParam (layer) = 0.0f;
            *processor.getLayerReverbSendParam (layer) = 0.0f;

            auto* volume = dynamic_cast<juce::AudioParameterFloat*> (
                processor.getLayerVolParam (layer));
            auto* compressor = dynamic_cast<juce::AudioParameterFloat*> (
                processor.getLayerCompParam (layer));

            if (volume == nullptr || compressor == nullptr)
                return false;

            *volume = 1.0f;
            *compressor = 0.0f;
        }

        juce::AudioBuffer<float> settle (2, blockSize);
        juce::MidiBuffer emptyMidi;

        for (int block = 0; block < 8; ++block)
        {
            settle.clear();
            processor.processBlock (settle, emptyMidi);
        }

        return true;
    };

    auto serial = std::make_unique<TimbresProcessor>();
    auto parallel = std::make_unique<TimbresProcessor>();

    if (! prepare (*serial, false) || ! prepare (*parallel, true))
        return false;

    juce::AudioBuffer<float> serialAudio (2, blockSize);
    juce::AudioBuffer<float> parallelAudio (2, blockSize);

    for (int block = 0; block < 18; ++block)
    {
        juce::MidiBuffer serialMidi;
        juce::MidiBuffer parallelMidi;

        if (block == 0)
            for (int note = 0; note < notesPerLayer; ++note)
            {
                const auto message = juce::MidiMessage::noteOn (
                    1, 36 + note * 3, (juce::uint8) (96 + note % 24));
                serialMidi.addEvent (message, 0);
                parallelMidi.addEvent (message, 0);
            }
        else if (block == 8)
            for (int note = 0; note < notesPerLayer; ++note)
            {
                const auto message = juce::MidiMessage::noteOff (
                    1, 36 + note * 3);
                serialMidi.addEvent (message, 73);
                parallelMidi.addEvent (message, 73);
            }

        serialAudio.clear();
        parallelAudio.clear();
        serial->processBlock (serialAudio, serialMidi);
        parallel->processBlock (parallelAudio, parallelMidi);

        if (block == 0
             && parallel->getParallelVoiceRenderCountForTest() == 0)
            std::cerr << "PARALLEL_PARITY_BLOCK0 worker_ready="
                      << parallel->isParallelVoiceRenderWorkerReadyForTest()
                      << " layer0="
                      << parallel->getActiveVoiceCountInLayerForTest (0)
                      << " layer1="
                      << parallel->getActiveVoiceCountInLayerForTest (1)
                      << '\n';

        for (int channel = 0; channel < 2; ++channel)
            if (std::memcmp (
                    serialAudio.getReadPointer (channel),
                    parallelAudio.getReadPointer (channel),
                    (std::size_t) blockSize * sizeof (float)) != 0)
            {
                std::cerr << "PARALLEL_PARITY_DIFF block=" << block
                          << " channel=" << channel
                          << " serial_count="
                          << serial->getParallelVoiceRenderCountForTest()
                          << " parallel_count="
                          << parallel->getParallelVoiceRenderCountForTest()
                          << '\n';
                return false;
            }
    }

    const auto serialCount = serial->getParallelVoiceRenderCountForTest();
    const auto parallelCount = parallel->getParallelVoiceRenderCountForTest();

    if (serialCount != 0 || parallelCount == 0)
        std::cerr << "PARALLEL_PARITY_NOT_EXERCISED serial_count="
                  << serialCount << " parallel_count=" << parallelCount << '\n';

    return serialCount == 0 && parallelCount > 0;
}

struct NativeAntiAliasMetrics
{
    double aliasBeforeDb = 0.0;
    double aliasAfterDb = 0.0;
    double aliasReductionDb = 0.0;
    double passbandChangeDb = 0.0;
    juce::uint32 streamMisses = 0;
};

bool writeAntiAliasProbeWav (const juce::File& file,
                             juce::AudioBuffer<float>& reference)
{
    constexpr int sourceSamples = 180000;
    constexpr double sampleRate = 48000.0;
    constexpr double passFrequency = 3000.0;
    constexpr double rejectedFrequency = 18000.0;

    reference.setSize (1, sourceSamples);

    for (int i = 0; i < sourceSamples; ++i)
    {
        const double time = (double) i / sampleRate;
        reference.setSample (
            0, i,
            (float) (0.18 * std::sin (
                         juce::MathConstants<double>::twoPi
                         * passFrequency * time)
                     + 0.18 * std::sin (
                         juce::MathConstants<double>::twoPi
                         * rejectedFrequency * time)));
    }

    auto output = file.createOutputStream();

    if (output == nullptr)
        return false;

    auto* rawOutput = output.release();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (
            rawOutput, sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
    {
        delete rawOutput;
        return false;
    }

    return writer->writeFromAudioSampleBuffer (
        reference, 0, reference.getNumSamples());
}

double measuredToneAmplitude (const float* signal, int startSample,
                              int numSamples, double frequency,
                              double sampleRate)
{
    double real = 0.0;
    double imaginary = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double phase =
            juce::MathConstants<double>::twoPi * frequency
            * (double) (startSample + i) / sampleRate;
        const double sample = signal[startSample + i];
        real += sample * std::cos (phase);
        imaginary -= sample * std::sin (phase);
    }

    return 2.0 * std::hypot (real, imaginary) / (double) numSamples;
}

bool runNativeAntiAliasProbe (NativeAntiAliasMetrics& metrics)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int analysisStart = 52000;
    constexpr int analysisSamples = 16384;
    constexpr int renderSamples = analysisStart + analysisSamples;

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-native-antialias-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    const auto sample = temp.getChildFile (
        "AntiAlias-C3-V127.wav");
    juce::AudioBuffer<float> source;

    if (! writeAntiAliasProbeWav (sample, source))
        return false;

    // Nao dependa da convencao textual de oitavas do parser nesta sonda: fixa
    // C central em MIDI 60 para que MIDI 72 seja exatamente ratio=2.
    const auto map = temp.getChildFile ("churchdaw-timbre.json");
    const juce::String mapJson = R"json({
  "schema": 1,
  "zones": [
    { "file": "AntiAlias-C3-V127.wav", "root": 60,
      "keyLo": 0, "keyHi": 127 }
  ]
})json";

    if (! map.replaceWithText (mapJson))
        return false;

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Native Anti Alias__";
    entry.library = "Probe Pack";
    entry.category = "TEST";
    entry.files.add (sample);

    const auto renderNote = [&] (int midiNote,
                                 juce::AudioBuffer<float>& captured,
                                 juce::uint32& streamMisses)
    {
        auto processor = std::make_unique<TimbresProcessor>();
        processor->prepareToPlay (sampleRate, blockSize);
        processor->loadTimbreIntoLayer (0, entry, true);
        std::atomic<size_t> memoryPeak { 0 };

        if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
             || processor->getLayerTimbre (0) != entry.label)
            return false;

        // Cadeia neutra: a sonda deve medir o resampler, nao EQ/rotary/delay.
        processor->applyFactoryTimbreShape (
            0, entry.label, entry.category, false);
        *processor->getLayerAttackParam (0) = 0.001f;
        *processor->getLayerReleaseParam (0) = 0.05f;
        *processor->getLayerLowEqParam (0) = 0.0f;
        *processor->getLayerMidEqParam (0) = 0.0f;
        *processor->getLayerHighEqParam (0) = 0.0f;
        *processor->getLayerRotaryModeParam (0) =
            ChurchDAWRotary::Speaker::stopMode;
        *processor->getLayerRotaryAmountParam (0) = 0.0f;
        *processor->getLayerReverbSendParam (0) = 0.0f;

        if (auto* volume = dynamic_cast<juce::AudioParameterFloat*> (
                processor->getLayerVolParam (0)))
            *volume = 1.0f;
        else
            return false;

        if (auto* compressor = dynamic_cast<juce::AudioParameterFloat*> (
                processor->getLayerCompParam (0)))
            *compressor = 0.0f;
        else
            return false;

        for (auto* parameter : processor->getParameters())
        {
            auto* identified =
                dynamic_cast<juce::AudioProcessorParameterWithID*> (
                    parameter);

            if (identified == nullptr)
                continue;

            if (identified->paramID == "lcut0"
                 || identified->paramID == "hcut0"
                 || identified->paramID == "dly0")
            {
                if (auto* value =
                        dynamic_cast<juce::AudioParameterFloat*> (
                            parameter))
                    *value = 0.0f;
            }
            else if (identified->paramID == "aeq0")
            {
                if (auto* value =
                        dynamic_cast<juce::AudioParameterBool*> (
                            parameter))
                    *value = false;
            }
        }

        juce::AudioBuffer<float> audio (2, blockSize);
        juce::MidiBuffer midi;

        // Deixa todos os smoothers da cadeia neutra chegarem ao alvo.
        for (int block = 0; block < 16; ++block)
        {
            audio.clear();
            midi.clear();
            processor->processBlock (audio, midi);
        }

        captured.setSize (1, renderSamples);
        captured.clear();

        for (int rendered = 0; rendered < renderSamples;
             rendered += blockSize)
        {
            audio.clear();
            midi.clear();

            if (rendered == 0)
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, midiNote, (juce::uint8) 127), 0);

            processor->processBlock (audio, midi);

            const int valid = juce::jmin (
                blockSize, renderSamples - rendered);
            captured.copyFrom (
                0, rendered, audio, 0, 0, valid);

            // O ratio=2 cruza o preload e exercita a emenda das duas janelas.
            std::this_thread::sleep_for (
                std::chrono::milliseconds (3));
        }

        streamMisses = processor->getStreamMissCount();
        return true;
    };

    juce::AudioBuffer<float> unityOutput;
    juce::AudioBuffer<float> downsampledOutput;
    juce::uint32 unityMisses = 0;
    juce::uint32 downsampledMisses = 0;

    if (! renderNote (60, unityOutput, unityMisses)
         || ! renderNote (72, downsampledOutput, downsampledMisses))
        return false;

    // Referencia BEFORE: o renderer antigo em ratio=2 lia exatamente cada
    // segunda amostra. A senoide fonte de 18 kHz reaparece em 12 kHz com a
    // mesma energia, enquanto 3 kHz vira o sinal util de 6 kHz.
    juce::AudioBuffer<float> legacyDownsampled (1, renderSamples);

    for (int i = 0; i < renderSamples; ++i)
        legacyDownsampled.setSample (
            0, i, source.getSample (0, i * 2));

    const double legacyPass = measuredToneAmplitude (
        legacyDownsampled.getReadPointer (0), analysisStart,
        analysisSamples, 6000.0, sampleRate);
    const double legacyAlias = measuredToneAmplitude (
        legacyDownsampled.getReadPointer (0), analysisStart,
        analysisSamples, 12000.0, sampleRate);
    const double afterPass = measuredToneAmplitude (
        downsampledOutput.getReadPointer (0), analysisStart,
        analysisSamples, 6000.0, sampleRate);
    const double afterAlias = measuredToneAmplitude (
        downsampledOutput.getReadPointer (0), analysisStart,
        analysisSamples, 12000.0, sampleRate);
    const double unityPass = measuredToneAmplitude (
        unityOutput.getReadPointer (0), analysisStart,
        analysisSamples, 3000.0, sampleRate);

    constexpr double floorEnergy = 1.0e-30;
    const double beforeRatio =
        (legacyAlias * legacyAlias + floorEnergy)
        / (legacyPass * legacyPass + floorEnergy);
    const double afterRatio =
        (afterAlias * afterAlias + floorEnergy)
        / (afterPass * afterPass + floorEnergy);

    metrics.aliasBeforeDb = 10.0 * std::log10 (beforeRatio);
    metrics.aliasAfterDb = 10.0 * std::log10 (afterRatio);
    metrics.aliasReductionDb =
        metrics.aliasBeforeDb - metrics.aliasAfterDb;
    metrics.passbandChangeDb = 20.0 * std::log10 (
        (afterPass + 1.0e-15) / (unityPass + 1.0e-15));
    metrics.streamMisses = unityMisses + downsampledMisses;

    return std::isfinite (metrics.aliasBeforeDb)
        && std::isfinite (metrics.aliasAfterDb)
        && std::isfinite (metrics.passbandChangeDb)
        && metrics.aliasBeforeDb > -1.0
        && metrics.aliasBeforeDb < 1.0
        && metrics.aliasReductionDb >= 24.0
        && metrics.aliasAfterDb <= -24.0
        && metrics.passbandChangeDb >= -1.5
        && metrics.passbandChangeDb <= 1.0
        && afterPass > 0.01
        && metrics.streamMisses == 0;
}

bool writeShortDelayBurstWav (const juce::File& file)
{
    auto output = file.createOutputStream();

    if (output == nullptr)
        return false;

    auto* rawOutput = output.release();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (
            rawOutput, 48000.0, 1, 16, {}, 0));

    if (writer == nullptr)
    {
        delete rawOutput;
        return false;
    }

    juce::AudioBuffer<float> audio (1, 512);

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float envelope = std::sin (
            juce::MathConstants<float>::pi
            * (float) i / (float) (audio.getNumSamples() - 1));
        audio.setSample (
            0, i,
            0.45f * envelope
                * std::sin (
                    juce::MathConstants<float>::twoPi
                    * 997.0f * (float) i / 48000.0f));
    }

    return writer->writeFromAudioSampleBuffer (
        audio, 0, audio.getNumSamples());
}

bool runNativeKeyDepthProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-native-logical-press-probe",
                              {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sampleA = temp.getChildFile ("Press-A-C3.wav");
    const auto sampleB = temp.getChildFile ("Press-B-C3.wav");

    if (! writeProbeWav (sampleA, 440.0f)
         || ! writeProbeWav (sampleB, 660.0f))
        return false;

    TimbresProcessor::TimbreEntry entryA;
    entryA.label = "__Native Logical Press A__";
    entryA.library = "Probe Pack";
    entryA.category = "PIANOS";
    entryA.files.add (sampleA);

    auto entryB = entryA;
    entryB.label = "__Native Logical Press B__";
    entryB.files.clear();
    entryB.files.add (sampleB);

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 256);
    processor->loadTimbreIntoLayer (0, entryA, true);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entryA.label)
        return false;

    juce::AudioBuffer<float> audio (2, 256);
    juce::MidiBuffer midi;

    const auto process = [&]
    {
        audio.clear();
        processor->processBlock (audio, midi);
        midi.clear();
    };

    // Dois note-ons iguais no mesmo canal representam duas pressoes logicas e
    // duas geracoes de voz. O primeiro NoteOff consome a geracao FIFO mais
    // antiga sem soltar a segunda.
    midi.addEvent (juce::MidiMessage::noteOn (
                       1, 60, (juce::uint8) 100), 0);
    midi.addEvent (juce::MidiMessage::noteOn (
                       1, 60, (juce::uint8) 110), 8);
    process();

    const auto first = processor->getNativeLogicalPressIdForTest (60, 1, 0);
    const auto second = processor->getNativeLogicalPressIdForTest (60, 1, 1);

    if (first == 0 || second <= first
         || processor->getPhysicalKeyDepthForTest (60, 1) != 2
         || processor->getNativeLogicalPressCountForTest (60, 1) != 2
         || processor->getNativePressedVoiceCountForTest (first) != 1
         || processor->getNativePressedVoiceCountForTest (second) != 1)
        return false;

    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 64);
    process();

    if (processor->getPhysicalKeyDepthForTest (60, 1) != 1
         || processor->getNativeLogicalPressCountForTest (60, 1) != 1
         || processor->getNativeLogicalPressIdForTest (60, 1, 0) != second
         || processor->getNativePressedVoiceCountForTest (first) != 0
         || processor->getNativeReleasedVoiceCountForTest (first) != 1
         || processor->getNativePressedVoiceCountForTest (second) != 1)
        return false;

    // Outro canal e independente; o segundo Off do canal 1 nao apaga o canal 2.
    midi.addEvent (juce::MidiMessage::noteOn (
                       2, 60, (juce::uint8) 100), 0);
    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 32);
    process();

    if (processor->getPhysicalKeyDepthForTest (60, 1) != 0
         || processor->getPhysicalKeyDepthForTest (60, 2) != 1
         || processor->getNativeLogicalPressCountForTest (60, 1) != 0
         || processor->getNativeLogicalPressCountForTest (60, 2) != 1
         || processor->getNativeReleasedVoiceCountForTest (second) != 1)
        return false;

    midi.addEvent (juce::MidiMessage::allSoundOff (2), 0);
    process();

    if (processor->getPhysicalKeyDepthForTest (60, 1) != 0
         || processor->getPhysicalKeyDepthForTest (60, 2) != 0
         || processor->getNativeLogicalPressCountForTest (60, 2) != 0)
        return false;

    // Com sustain, cada Off move somente sua propria geracao para pedal-held;
    // levantar o pedal libera ambas, ainda identificaveis separadamente.
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
    midi.addEvent (juce::MidiMessage::noteOn (
                       1, 60, (juce::uint8) 96), 8);
    midi.addEvent (juce::MidiMessage::noteOn (
                       1, 60, (juce::uint8) 112), 16);
    process();

    const auto pedalFirst =
        processor->getNativeLogicalPressIdForTest (60, 1, 0);
    const auto pedalSecond =
        processor->getNativeLogicalPressIdForTest (60, 1, 1);

    if (pedalFirst == 0 || pedalSecond <= pedalFirst)
        return false;

    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    process();

    if (processor->getNativePedalHeldVoiceCountForTest (pedalFirst) != 1
         || processor->getNativePressedVoiceCountForTest (pedalSecond) != 1
         || processor->getNativeLogicalPressCountForTest (60, 1) != 1)
        return false;

    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    process();

    if (processor->getNativePedalHeldVoiceCountForTest (pedalFirst) != 1
         || processor->getNativePedalHeldVoiceCountForTest (pedalSecond) != 1
         || processor->getNativeLogicalPressCountForTest (60, 1) != 0
         || processor->getPhysicalKeyDepthForTest (60, 1) != 0)
        return false;

    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    process();

    if (processor->getNativePedalHeldVoiceCountForTest (pedalFirst) != 0
         || processor->getNativePedalHeldVoiceCountForTest (pedalSecond) != 0
         || processor->getNativeReleasedVoiceCountForTest (pedalFirst) != 1
         || processor->getNativeReleasedVoiceCountForTest (pedalSecond) != 1)
        return false;

    midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
    process();

    // A identidade da pressao deve sobreviver ao hot-reload. Um NoteOff antigo
    // libera a voz recriada, nunca uma geracao posterior por coincidencia.
    midi.addEvent (juce::MidiMessage::noteOn (
                       1, 60, (juce::uint8) 105), 0);
    process();
    const auto reloadPress =
        processor->getNativeLogicalPressIdForTest (60, 1, 0);

    if (reloadPress == 0
         || processor->getNativePressedVoiceCountForTest (reloadPress) != 1)
        return false;

    processor->loadTimbreIntoLayer (0, entryB, false);

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entryB.label
         || processor->getNativeLogicalPressIdForTest (60, 1, 0)
                != reloadPress
         || processor->getNativePressedVoiceCountForTest (reloadPress) != 1)
        return false;

    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    process();

    if (processor->getPhysicalKeyDepthForTest (60, 1) != 0
         || processor->getNativeLogicalPressCountForTest (60, 1) != 0
         || processor->getNativePressedVoiceCountForTest (reloadPress) != 0
         || processor->getNativeReleasedVoiceCountForTest (reloadPress) < 1)
        return false;

    // Defesa contra notas presas: Note-Off duplicado ou orfão não pode
    // corromper depth nem prender vozes
    midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    process();

    if (processor->getPhysicalKeyDepthForTest (60, 1) != 0
         || processor->getNativeLogicalPressCountForTest (60, 1) != 0)
        return false;

    // Defesa contra notas presas: pedal de sustain com tecla já solta
    // garante liberação completa ao soltar o pedal
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 0);
    midi.addEvent (juce::MidiMessage::noteOn (1, 65, (juce::uint8) 100), 10);
    midi.addEvent (juce::MidiMessage::noteOff (1, 65), 20);
    process();

    if (processor->getPhysicalKeyDepthForTest (65, 1) != 0
         || processor->getNativeLogicalPressCountForTest (65, 1) != 0)
        return false;

    // Ao soltar o pedal de sustain, todas as vozes remanescentes devem ser liberadas
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 0);
    process();

    return processor->getPhysicalKeyDepthForTest (65, 1) == 0
        && processor->getNativeLogicalPressCountForTest (65, 1) == 0;
}

bool runNativeSampleAccurateMidiProbe()
{
    constexpr int blockSize = 256;
    constexpr int noteOnOffset = 40;
    constexpr int noteOffOffset = 170;
    constexpr int pedalOnOffset = 80;
    constexpr int pedalNoteOffOffset = 130;
    constexpr int pedalOffOffset = 200;
    constexpr int allOffOffset = 220;

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-native-midi-offset-probe",
                              {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeProbeWav (sample, 997.0f))
        return false;

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Native Sample Accurate MIDI__";
    entry.library = "Probe Pack";
    entry.category = "PADS";
    entry.files.add (sample);

    enum class Case
    {
        held,
        noteOff,
        pedal,
        allOff,
        sameSampleAllOff
    };

    const auto render = [&] (Case which,
                             juce::AudioBuffer<float>& result)
    {
        auto processor = std::make_unique<TimbresProcessor>();
        processor->prepareToPlay (48000.0, blockSize);
        processor->loadTimbreIntoLayer (0, entry, true);
        std::atomic<size_t> memoryPeak { 0 };

        if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
             || processor->getLayerTimbre (0) != entry.label)
            return false;

        *processor->getLayerAttackParam (0) = 0.001f;
        *processor->getLayerReleaseParam (0) = 0.05f;

        juce::AudioBuffer<float> settle (2, blockSize);
        juce::MidiBuffer midi;

        // Iguala os smoothers dos canais antes da comparacao; nenhum evento
        // MIDI e usado nesse pre-roll e portanto nenhuma voz e criada.
        for (int block = 0; block < 6; ++block)
        {
            settle.clear();
            midi.clear();
            processor->processBlock (settle, midi);
        }

        result.setSize (2, blockSize);
        result.clear();
        midi.clear();

        const int onOffset = which == Case::sameSampleAllOff
                                 ? 100 : noteOnOffset;
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, (juce::uint8) 127),
            onOffset);

        if (which == Case::noteOff)
            midi.addEvent (
                juce::MidiMessage::noteOff (1, 60),
                noteOffOffset);
        else if (which == Case::pedal)
        {
            midi.addEvent (
                juce::MidiMessage::controllerEvent (
                    1, 64, 127),
                pedalOnOffset);
            midi.addEvent (
                juce::MidiMessage::noteOff (1, 60),
                pedalNoteOffOffset);
            midi.addEvent (
                juce::MidiMessage::controllerEvent (
                    1, 64, 0),
                pedalOffOffset);
        }
        else if (which == Case::allOff)
            midi.addEvent (
                juce::MidiMessage::allSoundOff (1),
                allOffOffset);
        else if (which == Case::sameSampleAllOff)
            midi.addEvent (
                juce::MidiMessage::allSoundOff (1),
                onOffset);

        processor->processBlock (result, midi);
        return true;
    };

    juce::AudioBuffer<float> held;
    juce::AudioBuffer<float> noteReleased;
    juce::AudioBuffer<float> pedalReleased;
    juce::AudioBuffer<float> allOff;
    juce::AudioBuffer<float> sameSampleAllOff;

    if (! render (Case::held, held)
         || ! render (Case::noteOff, noteReleased)
         || ! render (Case::pedal, pedalReleased)
         || ! render (Case::allOff, allOff)
         || ! render (Case::sameSampleAllOff, sameSampleAllOff))
        return false;

    const auto maxAbs = [] (const juce::AudioBuffer<float>& audio,
                            int start, int end)
    {
        float peak = 0.0f;

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sampleIndex = start; sampleIndex < end; ++sampleIndex)
                peak = juce::jmax (
                    peak,
                    std::abs (audio.getSample (channel, sampleIndex)));

        return peak;
    };

    const auto maxDifference = [] (
        const juce::AudioBuffer<float>& a,
        const juce::AudioBuffer<float>& b,
        int start, int end)
    {
        float peak = 0.0f;

        for (int channel = 0; channel < a.getNumChannels(); ++channel)
            for (int sampleIndex = start; sampleIndex < end; ++sampleIndex)
                peak = juce::jmax (
                    peak,
                    std::abs (a.getSample (channel, sampleIndex)
                              - b.getSample (channel, sampleIndex)));

        return peak;
    };

    const auto firstDifference = [] (
        const juce::AudioBuffer<float>& a,
        const juce::AudioBuffer<float>& b,
        float epsilon)
    {
        for (int sampleIndex = 0;
             sampleIndex < a.getNumSamples();
             ++sampleIndex)
            for (int channel = 0;
                 channel < a.getNumChannels();
                 ++channel)
                if (std::abs (a.getSample (channel, sampleIndex)
                              - b.getSample (channel, sampleIndex))
                     > epsilon)
                    return sampleIndex;

        return -1;
    };

    // A nota nao pode existir antes do offset e precisa produzir sinal real
    // durante o intervalo em que a tecla esta fisicamente pressionada.
    if (maxAbs (noteReleased, 0, noteOnOffset) > 1.0e-8f
         || maxAbs (noteReleased, noteOnOffset + 48,
                    noteOffOffset) < 1.0e-4f)
        return false;

    // NOTE OFF nao pode alterar uma unica amostra anterior a sua fronteira.
    // Depois dela, a versao liberada precisa divergir da nota ainda segurada.
    const int noteOffFirstDifference = firstDifference (
        held, noteReleased, 1.0e-7f);

    if (maxDifference (held, noteReleased, 0, noteOffOffset)
            > 1.0e-7f
         || noteOffFirstDifference < noteOffOffset
         || maxDifference (held, noteReleased,
                           noteOffOffset, blockSize) < 1.0e-5f)
        return false;

    // O note-off sob CC64 permanece identico a tecla segurada; a cauda so
    // comeca quando o pedal sobe no offset 200.
    const int pedalFirstDifference = firstDifference (
        held, pedalReleased, 1.0e-7f);

    if (maxDifference (held, pedalReleased, 0, pedalOffOffset)
            > 1.0e-7f
         || pedalFirstDifference < pedalOffOffset
         || maxDifference (held, pedalReleased,
                           pedalOffOffset, blockSize) < 1.0e-5f)
        return false;

    // ALL SOUND OFF tambem e uma fronteira temporal: o audio anterior fica
    // intacto e apenas o declique posterior pode divergir.
    const int allOffFirstDifference = firstDifference (
        held, allOff, 1.0e-7f);

    if (maxDifference (held, allOff, 0, allOffOffset)
            > 1.0e-7f
         || allOffFirstDifference < allOffOffset
         || maxDifference (held, allOff,
                           allOffOffset, blockSize) < 1.0e-5f)
        return false;

    // O guard central ordena eventos comuns antes de ALL OFF no mesmo sample.
    // Como nao houve intervalo renderizavel entre eles, o bloco deve ser mudo.
    return maxAbs (sameSampleAllOff, 0, blockSize) <= 1.0e-8f;
}

bool writeIgnoredLoopMarkerWav (const juce::File& file,
                                int postMarkerStart = 30000,
                                bool hardTail = false)
{
    auto output = file.createOutputStream();

    if (output == nullptr)
        return false;

    juce::StringPairArray metadata;
    metadata.set ("MidiUnityNote", "60");
    metadata.set ("NumSampleLoops", "1");
    metadata.set ("Loop0Identifier", "0");
    metadata.set ("Loop0Type", "0");
    metadata.set ("Loop0Start", "12000");
    metadata.set ("Loop0End", "24000");
    metadata.set ("Loop0Fraction", "0");
    metadata.set ("Loop0PlayCount", "0");

    auto* rawOutput = output.release();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (
            rawOutput, 48000.0, 1, 16, metadata, 0));

    if (writer == nullptr)
    {
        delete rawOutput;
        return false;
    }

    constexpr int totalSamples = 72000;   // 1,5 s: maior que o preload de 0,75 s
    juce::AudioBuffer<float> audio (1, totalSamples);

    for (int i = 0; i < totalSamples; ++i)
    {
        const float frequency = i < postMarkerStart ? 440.0f : 997.0f;
        const float fadeIn = juce::jlimit (0.0f, 1.0f, (float) i / 64.0f);
        audio.setSample (
            0, i,
            hardTail && i >= postMarkerStart
                ? 0.8f
                : 0.2f * fadeIn * std::sin (
                    juce::MathConstants<float>::twoPi
                    * frequency * (float) i / 48000.0f));
    }

    const bool wrote = writer->writeFromAudioSampleBuffer (
        audio, 0, audio.getNumSamples());
    writer.reset();

    if (! wrote)
        return false;

    auto input = file.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (input.release(), true));

    return reader != nullptr
        && reader->metadataValues.getValue (
               "NumSampleLoops", "0").getIntValue() == 1
        && reader->metadataValues.getValue (
               "Loop0Start", "-1").getIntValue() == 12000
        && reader->metadataValues.getValue (
               "Loop0End", "-1").getIntValue() == 24000;
}

bool runIgnoredLoopMarkerPlaybackProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-no-timbre-loop-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeIgnoredLoopMarkerWav (sample))
        return false;

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 256);

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Ignored Loop Marker__";
    entry.library = "Probe Pack";
    entry.category = "PADS";
    entry.files.add (sample);
    processor->loadTimbreIntoLayer (0, entry, true);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entry.label
         || ! processor->isFirstZoneStreamedForTest (0)
         || processor->getFirstZoneResidentSamplesForTest (0) > 37000
         || processor->getFirstZoneTotalSamplesForTest (0) < 71000)
        return false;

    const auto parameterById = [&processor] (const juce::String& id)
        -> juce::AudioProcessorParameterWithID*
    {
        for (auto* parameter : processor->getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == id)
                return identified;

        return nullptr;
    };

    auto* legacyLoop = parameterById ("loop0");
    auto* reverb = parameterById ("rvb0");
    auto* compressor = parameterById ("comp0");
    auto* delay = parameterById ("dly0");
    auto* lowCut = parameterById ("lcut0");

    if (legacyLoop == nullptr || reverb == nullptr
         || compressor == nullptr || delay == nullptr || lowCut == nullptr
         || std::abs (reverb->getValue()) > 1.0e-6f
         || std::abs (compressor->getValue()) > 1.0e-6f
         || std::abs (delay->getValue()) > 1.0e-6f)
        return false;

    // Prova forte: mesmo uma automacao antiga tentando ligar o tombstone nao
    // pode alterar playback. Os demais ajustes deixam a analise espectral seca.
    legacyLoop->setValueNotifyingHost (1.0f);
    reverb->setValueNotifyingHost (0.0f);
    compressor->setValueNotifyingHost (0.0f);
    lowCut->setValueNotifyingHost (0.0f);

    constexpr int blockSize = 256;
    constexpr int totalRenderSamples = 86400;   // 1,8 s
    constexpr int analysisStart = 40800;        // 0,85 s
    constexpr int analysisEnd = 55200;          // 1,15 s
    constexpr int tailStart = 79200;             // 1,65 s
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    double cos440 = 0.0, sin440 = 0.0;
    double cos997 = 0.0, sin997 = 0.0;
    double tailSquares = 0.0;
    std::uint64_t tailCount = 0;

    for (int rendered = 0; rendered < totalRenderSamples;
         rendered += blockSize)
    {
        audio.clear();
        midi.clear();

        if (rendered == 0)
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, (juce::uint8) 110), 0);

        processor->processBlock (audio, midi);

        const int valid = juce::jmin (
            blockSize, totalRenderSamples - rendered);

        for (int i = 0; i < valid; ++i)
        {
            const int sampleIndex = rendered + i;
            const double value = audio.getSample (0, i);

            if (! std::isfinite (value))
                return false;

            if (sampleIndex >= analysisStart
                 && sampleIndex < analysisEnd)
            {
                const double phase440 =
                    juce::MathConstants<double>::twoPi
                    * 440.0 * (double) sampleIndex / 48000.0;
                const double phase997 =
                    juce::MathConstants<double>::twoPi
                    * 997.0 * (double) sampleIndex / 48000.0;
                cos440 += value * std::cos (phase440);
                sin440 += value * std::sin (phase440);
                cos997 += value * std::cos (phase997);
                sin997 += value * std::sin (phase997);
            }

            if (sampleIndex >= tailStart)
            {
                tailSquares += value * value;
                ++tailCount;
            }
        }

        // A thread de streaming precisa receber o mesmo tempo que teria no app;
        // assim um miss de disco e defeito real, nao artefato do render offline.
        std::this_thread::sleep_for (std::chrono::milliseconds (6));
    }

    const double energy440 = cos440 * cos440 + sin440 * sin440;
    const double energy997 = cos997 * cos997 + sin997 * sin997;
    const double tailRms = tailCount > 0
                             ? std::sqrt (
                                   tailSquares / (double) tailCount)
                             : 1.0;

    return legacyLoop->getValue() > 0.5f
        && energy997 > 1.0e-4
        && energy997 > energy440 * 16.0
        && processor->getActiveVoiceCount() == 0
        && processor->getStreamMissCount() == 0
        && tailRms < 1.0e-7;
}

bool runExplicitSidecarLoopPlaybackProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-explicit-loop-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    // A cauda fisica comeca exatamente em loopEnd e e deliberadamente muito
    // diferente do loop. Assim o teste tambem detecta a Hermite consultando
    // loopEnd/loopEnd+1 em vez dos dois guardas copiados de loopStart.
    if (! writeIgnoredLoopMarkerWav (sample, 24000, true))
        return false;

    const auto map = temp.getChildFile ("churchdaw-timbre.json");
    const juce::String mapJson = R"json({
  "schema": 1,
  "tone": {
    "profileRevision": 1,
    "volumeLinear": 1.0,
    "octave": 0,
    "attackSeconds": 0.001,
    "releaseSeconds": 0.05,
    "lowCut": 0.0,
    "highCut": 0.0,
    "eqLowDb": 0.0,
    "eqMidDb": 0.0,
    "eqHighDb": 0.0,
    "compressor": 0.0,
    "delay": 0.0,
    "reverbSend": 0.0,
    "autoEq": false,
    "rotaryMode": "stop",
    "rotaryAmount": 0.0
  },
  "zones": [
    {
      "file": "C3.wav",
      "root": 60,
      "keyLo": 0,
      "keyHi": 127,
      "sourceLoop": {
        "sourceLoops": true,
        "startFrame": 12000,
        "endFrame": 24000,
        "playback": true
      }
    }
  ]
})json";

    if (! map.replaceWithText (mapJson))
        return false;

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 256);

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Explicit Sidecar Loop__";
    entry.library = "Probe Pack";
    entry.category = "PADS";
    entry.files.add (sample);
    processor->loadTimbreIntoLayer (0, entry, true);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entry.label
         || processor->isFirstZoneStreamedForTest (0)
         || processor->getFirstZoneResidentSamplesForTest (0) < 71000)
        return false;

    const int loopStart = processor->getZoneLoopStartForTest (0, 61);
    const int loopEnd = processor->getZoneLoopEndForTest (0, 61);

    if (loopStart < 11900 || loopStart > 12000
         || loopEnd - loopStart != 12000)
        return false;

    constexpr int blockSize = 256;
    constexpr int heldSamples = 67200; // 1,4 s: varias voltas e alem do WAV
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    double lateSquares = 0.0;
    std::uint64_t lateCount = 0;
    float previous = 0.0f;
    float maxAdjacent = 0.0f;
    bool havePrevious = false;

    for (int rendered = 0; rendered < heldSamples; rendered += blockSize)
    {
        audio.clear();
        midi.clear();

        // Um semitom acima da root obriga a Hermite a atravessar a fronteira.
        if (rendered == 0)
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 61, (juce::uint8) 110), 0);

        processor->processBlock (audio, midi);
        const int valid = juce::jmin (
            blockSize, heldSamples - rendered);

        for (int i = 0; i < valid; ++i)
        {
            const float value = audio.getSample (0, i);

            if (! std::isfinite (value))
                return false;

            if (havePrevious)
                maxAdjacent = juce::jmax (
                    maxAdjacent, std::abs (value - previous));

            previous = value;
            havePrevious = true;

            if (rendered + i >= 57600)
            {
                lateSquares += (double) value * (double) value;
                ++lateCount;
            }
        }
    }

    const double lateRms = lateCount > 0
                             ? std::sqrt (lateSquares / (double) lateCount)
                             : 0.0;

    if (processor->getActiveVoiceCount() == 0
         || processor->getStreamMissCount() != 0
         || lateRms < 1.0e-3
         || maxAdjacent > 0.04f)
        return false;

    // A voz loopada continua obedecendo ao note-off/release normal.
    for (int rendered = 0; rendered < 48000; rendered += blockSize)
    {
        audio.clear();
        midi.clear();

        if (rendered == 0)
            midi.addEvent (
                juce::MidiMessage::noteOff (1, 61), 0);

        processor->processBlock (audio, midi);
    }

    return processor->getActiveVoiceCount() == 0
        && processor->getStreamMissCount() == 0;
}

bool runEvenRootBoundaryProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-even-root-boundary-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    constexpr std::array<const char*, 9> names {
        "Map-C1-V127-0008.wav", "Map-F#1-V127-0007.wav",
        "Map-C2-V127-0006.wav", "Map-F#2-V127-0005.wav",
        "Map-C3-V127-0004.wav", "Map-F#3-V127-0003.wav",
        "Map-C4-V127-0002.wav", "Map-F#4-V127-0001.wav",
        "Map-C5-V127-0000.wav"
    };

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Even Root Boundary__";
    entry.library = "Probe Pack";
    entry.category = "PADS";

    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const auto sample = temp.getChildFile (names[i]);

        if (! writeProbeWav (sample, 180.0f + (float) i * 37.0f))
            return false;

        entry.files.add (sample);
    }

    const auto map = temp.getChildFile ("churchdaw-timbre.json");
    const juce::String mapJson = R"json({
  "schema": 1,
  "tone": {
    "profileRevision": 7,
    "volumeLinear": 0.61,
    "octave": -2,
    "attackSeconds": 0.021,
    "releaseSeconds": 2.34,
    "lowCut": 0.73,
    "highCut": 0.41,
    "eqLowDb": -6.0,
    "eqMidDb": -4.0,
    "eqHighDb": 2.0,
    "compressor": 0.27,
    "delay": 0.18,
    "reverbSend": 0.36,
    "autoEq": false,
    "rotaryMode": "stop",
    "rotaryAmount": 0.0
  },
  "zones": [
    { "file": "Map-C1-V127-0008.wav",  "root": 36, "keyLo": 17, "keyHi": 38 },
    { "file": "Map-F#1-V127-0007.wav", "root": 42, "keyLo": 39, "keyHi": 44 },
    { "file": "Map-C2-V127-0006.wav",  "root": 48, "keyLo": 45, "keyHi": 50 },
    { "file": "Map-F#2-V127-0005.wav", "root": 54, "keyLo": 51, "keyHi": 56 },
    { "file": "Map-C3-V127-0004.wav",  "root": 60, "keyLo": 57, "keyHi": 62 },
    { "file": "Map-F#3-V127-0003.wav", "root": 66, "keyLo": 63, "keyHi": 68 },
    { "file": "Map-C4-V127-0002.wav",  "root": 72, "keyLo": 69, "keyHi": 74 },
    { "file": "Map-F#4-V127-0001.wav", "root": 78, "keyLo": 75, "keyHi": 80 },
    { "file": "Map-C5-V127-0000.wav",  "root": 84, "keyLo": 81, "keyHi": 108 }
  ]
})json";

    if (! map.replaceWithText (mapJson))
        return false;

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 256);
    processor->loadTimbreIntoLayer (0, entry, true);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entry.label)
        return false;

    // A tone sidecar may describe envelope/EQ, but never owns the channel
    // octave. Legacy pad sidecars with octave=-2 must open at the neutral
    // channel setting; the musician changes this knob explicitly if wanted.
    if (processor->getLayerOctaveParam (0) == nullptr
         || processor->getLayerOctaveParam (0)->get() != 0)
        return false;

    // O Nord entrega a metade exata ao root de cima quando a distancia entre
    // roots e par. Antes, MIDI 57 ainda escolhia F#2 (root 54); o arquivo era
    // transposto +3 em vez de C3 (root 60) transposto -3, mudando o timbre.
    constexpr std::array<std::pair<int, int>, 10> expected {{
        { 38, 36 }, { 39, 42 }, { 44, 42 }, { 45, 48 },
        { 56, 54 }, { 57, 60 }, { 62, 60 }, { 63, 66 },
        { 80, 78 }, { 81, 84 }
    }};

    for (const auto [note, expectedRoot] : expected)
        if (processor->getZoneRootNoteForTest (0, note) != expectedRoot)
        {
            return false;
        }

    // Varre o MIDI inteiro, nao apenas as fronteiras exemplares. Isto protege
    // buracos, sobreposicoes e uma zona esticada acidentalmente alem do Nord.
    constexpr std::array<std::array<int, 3>, 9> expectedZones {{
        {{ 17, 38, 36 }}, {{ 39, 44, 42 }}, {{ 45, 50, 48 }},
        {{ 51, 56, 54 }}, {{ 57, 62, 60 }}, {{ 63, 68, 66 }},
        {{ 69, 74, 72 }}, {{ 75, 80, 78 }}, {{ 81, 108, 84 }}
    }};

    for (int note = 0; note < 128; ++note)
    {
        int expectedRoot = -1;

        for (const auto& zone : expectedZones)
            if (note >= zone[0] && note <= zone[1])
            {
                expectedRoot = zone[2];
                break;
            }

        if (processor->getZoneRootNoteForTest (0, note) != expectedRoot)
        {
            return false;
        }
    }

    // As pontas tambem pertencem ao mapa autoral. Sem o sidecar C5 era
    // esticada ate 127 (mais de 3,5 oitavas nesta taxa), fonte de aliasing.
    if (processor->getZoneRootNoteForTest (0, 16) != -1
         || processor->getZoneRootNoteForTest (0, 17) != 36
         || processor->getZoneRootNoteForTest (0, 108) != 84
         || processor->getZoneRootNoteForTest (0, 109) != -1)
        return false;


    const auto findFloatParameter = [] (
        TimbresProcessor& owner,
        const juce::String& parameterId) -> juce::AudioParameterFloat*
    {
        for (auto* parameter : owner.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterFloat*> (parameter);

        return nullptr;
    };

    const auto close = [] (float actual, float expected)
    {
        return std::isfinite (actual)
            && std::abs (actual - expected) <= 0.0001f;
    };

    const auto* lowCut = findFloatParameter (*processor, "lcut0");
    const auto* highCut = findFloatParameter (*processor, "hcut0");
    const auto* compressor = findFloatParameter (*processor, "comp0");
    const auto* delay = findFloatParameter (*processor, "dly0");

    if (lowCut == nullptr || highCut == nullptr
         || compressor == nullptr || delay == nullptr
         || ! close (dynamic_cast<juce::AudioParameterFloat*> (
                         processor->getLayerVolParam (0))->get(), 0.61f)
         || processor->getLayerOctaveParam (0)->get() != 0
         || ! close (processor->getLayerAttackParam (0)->get(), 0.021f)
         || ! close (processor->getLayerReleaseParam (0)->get(), 2.34f)
         || ! close (lowCut->get(), 0.73f)
         || ! close (highCut->get(), 0.41f)
         || ! close (processor->getLayerLowEqParam (0)->get(), -6.0f)
         || ! close (processor->getLayerMidEqParam (0)->get(), -4.0f)
         || ! close (processor->getLayerHighEqParam (0)->get(), 2.0f)
         || ! close (compressor->get(), 0.27f)
         || ! close (delay->get(), 0.18f)
         || ! close (processor->getLayerReverbSendParam (0)->get(), 0.36f)
         || processor->getLayerRotaryModeParam (0)->getIndex()
                != ChurchDAWRotary::Speaker::stopMode
         || ! close (processor->getLayerRotaryAmountParam (0)->get(), 0.0f))
        return false;

    juce::MemoryBlock profiledState;
    processor->getStateInformation (profiledState);
    const auto profiledXml =
        juce::AudioProcessor::getXmlFromBinary (
            profiledState.getData(),
            (int) profiledState.getSize());
    bool savedProfileRevision = false;

    if (profiledXml != nullptr)
        forEachXmlChildElementWithTagName (
            *profiledXml, layer, "LAYER")
            if (layer->getIntAttribute ("index", -1) == 0)
                savedProfileRevision =
                    layer->getIntAttribute (
                        "toneProfileRevision", 0) == 7;

    if (! savedProfileRevision)
        return false;

    return true;
}

bool runIdleDelayTailGainProbe()
{
    constexpr int blockSize = 256;
    constexpr int delaySamples = (int) (0.38 * 48000.0);
    constexpr int windowSamples = 900;
    constexpr int renderSamples = delaySamples * 3 + windowSamples;

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-idle-delay-tail-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    const auto sample = temp.getChildFile (
        "Delay-C3-V127.wav");

    if (! writeShortDelayBurstWav (sample))
        return false;

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Idle Delay Tail__";
    entry.library = "Probe Pack";
    entry.category = "PIANOS";
    entry.files.add (sample);

    const auto findFloatParameter = [] (
        TimbresProcessor& owner,
        const juce::String& parameterId) -> juce::AudioParameterFloat*
    {
        for (auto* parameter : owner.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterFloat*> (parameter);

        return nullptr;
    };

    const auto render = [&] (float amount,
                             std::array<float, 3>& peaks)
    {
        auto processor = std::make_unique<TimbresProcessor>();
        processor->prepareToPlay (48000.0, blockSize);
        processor->loadTimbreIntoLayer (0, entry, true);
        std::atomic<size_t> memoryPeak { 0 };

        if (! dispatchUntilLoaded (*processor, memoryPeak, 10))
            return false;

        auto* delay = findFloatParameter (*processor, "dly0");

        if (delay == nullptr)
            return false;

        *delay = amount;
        *processor->getLayerAttackParam (0) = 0.001f;
        *processor->getLayerReleaseParam (0) = 0.05f;
        *processor->getLayerReverbSendParam (0) = 0.0f;

        juce::AudioBuffer<float> audio (2, blockSize);
        juce::MidiBuffer midi;

        // Deixa os smoothers chegarem ao alvo sem preencher a linha de delay.
        for (int block = 0; block < 190; ++block)
        {
            audio.clear();
            midi.clear();
            processor->processBlock (audio, midi);
        }

        peaks.fill (0.0f);

        for (int rendered = 0; rendered < renderSamples;
             rendered += blockSize)
        {
            audio.clear();
            midi.clear();

            if (rendered == 0)
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, 60, (juce::uint8) 127), 0);

            processor->processBlock (audio, midi);

            for (int s = 0; s < blockSize; ++s)
            {
                const int absolute = rendered + s;

                for (int echo = 0; echo < 3; ++echo)
                {
                    const int begin = delaySamples * (echo + 1);

                    if (absolute >= begin
                         && absolute < begin + windowSamples)
                        peaks[(std::size_t) echo] = juce::jmax (
                            peaks[(std::size_t) echo],
                            std::abs (audio.getSample (0, s)),
                            std::abs (audio.getSample (1, s)));
                }
            }
        }

        return true;
    };

    std::array<float, 3> low {}, high {};

    if (! render (0.10f, low) || ! render (0.50f, high))
        return false;

    for (std::size_t echo = 0; echo < low.size(); ++echo)
    {
        if (low[echo] <= 1.0e-6f || high[echo] <= 1.0e-6f)
            return false;

        const float amountRatio = high[echo] / low[echo];

        if (amountRatio < 4.7f || amountRatio > 5.3f)
            return false;
    }

    // O segundo eco ja cai depois do limiar ocioso de 0,6 s. Ele deve manter
    // o feedback de 0,32 e a dose do knob, nao saltar para wet=1 nem sumir.
    for (const auto& peaks : { low, high })
    {
        const float feedback12 = peaks[1] / peaks[0];
        const float feedback23 = peaks[2] / peaks[1];

        if (feedback12 < 0.27f || feedback12 > 0.37f
             || feedback23 < 0.27f || feedback23 > 0.37f)
            return false;
    }

    return true;
}

bool runChurchOrganLayerIntegrationProbe()
{
    using Family = churchorgan::ChurchOrganFamily;
    using Parameter = churchorgan::OrganParameterId;

    constexpr int layer = 0;
    constexpr double sampleRate = 48000.0;
    constexpr std::array<int, 4> blockSizes { 192, 384, 512, 1024 };

    const auto close = [] (float actual, float expected,
                           float tolerance = 0.0001f)
    {
        return std::isfinite (actual)
            && std::abs (actual - expected) <= tolerance;
    };

    const auto findBoolParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterBool*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr
                 && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterBool*> (
                    parameter);

        return nullptr;
    };

    const auto findFloatParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterFloat*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr
                 && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterFloat*> (
                    parameter);

        return nullptr;
    };

    const auto findIntParameter = [] (
        TimbresProcessor& processor,
        const juce::String& parameterId) -> juce::AudioParameterInt*
    {
        for (auto* parameter : processor.getParameters())
            if (auto* identified =
                    dynamic_cast<juce::AudioProcessorParameterWithID*> (
                        parameter);
                identified != nullptr
                 && identified->paramID == parameterId)
                return dynamic_cast<juce::AudioParameterInt*> (
                    parameter);

        return nullptr;
    };

    struct AudioStats
    {
        double sumSquares = 0.0;
        std::uint64_t valueCount = 0;
        std::uint64_t fingerprint =
            1469598103934665603ULL;
        float peak = 0.0f;
    };

    const auto accumulateAudio = [] (
        const juce::AudioBuffer<float>& audio,
        AudioStats& stats)
    {
        for (int channel = 0;
             channel < audio.getNumChannels();
             ++channel)
        {
            const auto* samples =
                audio.getReadPointer (channel);

            for (int sample = 0;
                 sample < audio.getNumSamples();
                 ++sample)
            {
                const float value = samples[sample];

                if (! std::isfinite (value))
                    return false;

                stats.peak = juce::jmax (
                    stats.peak, std::abs (value));
                stats.sumSquares +=
                    (double) value * (double) value;
                const auto quantised =
                    static_cast<std::uint32_t> (
                        (std::int32_t) std::llround (
                            (double) value
                            * 1000000.0));
                stats.fingerprint ^=
                    (std::uint64_t) quantised;
                stats.fingerprint *=
                    1099511628211ULL;
                ++stats.valueCount;
            }
        }

        return true;
    };

    juce::String currentGate {
        "single-layer setup"
    };
    bool probePassed = false;
    const juce::ScopeGuard reportFailedGate {
        [&]
        {
            if (! probePassed)
                std::cerr
                    << "Church Organ host probe gate: "
                    << currentGate << std::endl;
        }
    };

    struct Tone
    {
        float attack = 0.003f;
        float release = 0.55f;
        float low = 0.0f;
        float mid = 0.0f;
        float high = 0.0f;
        int rotaryMode = ChurchDAWRotary::Speaker::stopMode;
        float rotaryAmount = 0.0f;
    };

    const Tone toneA {
        0.147f, 3.25f,
        2.75f, -3.25f, 1.50f,
        ChurchDAWRotary::Speaker::fastMode, 0.73f
    };
    const Tone toneB {
        0.421f, 6.10f,
        -4.50f, 3.75f, -1.25f,
        ChurchDAWRotary::Speaker::stopMode, 0.29f
    };

    const auto setTone = [&] (TimbresProcessor& processor,
                              const Tone& tone)
    {
        *processor.getLayerAttackParam (layer) = tone.attack;
        *processor.getLayerReleaseParam (layer) = tone.release;
        *processor.getLayerLowEqParam (layer) = tone.low;
        *processor.getLayerMidEqParam (layer) = tone.mid;
        *processor.getLayerHighEqParam (layer) = tone.high;
        *processor.getLayerRotaryModeParam (layer) =
            tone.rotaryMode;
        *processor.getLayerRotaryAmountParam (layer) =
            tone.rotaryAmount;
    };

    const auto toneShapeMatches = [&] (
        const TimbresProcessor& processor,
        const Tone& expected)
    {
        return close (
                   processor.getLayerAttackParam (layer)->get(),
                   expected.attack)
            && close (
                   processor.getLayerReleaseParam (layer)->get(),
                   expected.release)
            && close (
                   processor.getLayerLowEqParam (layer)->get(),
                   expected.low)
            && close (
                   processor.getLayerMidEqParam (layer)->get(),
                   expected.mid)
            && close (
                   processor.getLayerHighEqParam (layer)->get(),
                   expected.high)
            && close (
                   processor.getLayerRotaryAmountParam (layer)->get(),
                   expected.rotaryAmount);
    };

    const auto toneMatches = [&] (
        const TimbresProcessor& processor,
        const Tone& expected)
    {
        return toneShapeMatches (processor, expected)
            && processor.getLayerRotaryModeParam (layer)->getIndex()
                   == expected.rotaryMode;
    };

    const auto hasUnityOrganTrim = [&] (
        const TimbresProcessor& processor)
    {
        const auto* organ =
            processor.getLayerChurchOrganProcessor (layer);

        if (organ == nullptr
             || ! processor.isLayerChurchOrgan (layer)
             || ! close (
                    processor.getLayerNativeFamilyTrim (layer),
                    1.0f))
            return false;

        for (int family = 0; family < 4; ++family)
            if (! close (
                    organ->familyOutputTrim (
                        static_cast<Family> (family)),
                    1.0f))
                return false;

        return close (
            organ->diagnostics().familyOutputTrim, 1.0f);
    };

    const auto renderVariableBlocks = [&] (
        TimbresProcessor& processor)
    {
        auto* organ =
            processor.getLayerChurchOrganProcessor (layer);

        if (organ == nullptr)
            return false;

        const auto before = organ->diagnostics();
        std::uint64_t expectedSamples = before.renderedSamples;
        double sumSquares = 0.0;
        float peak = 0.0f;

        for (std::size_t block = 0;
             block < blockSizes.size();
             ++block)
        {
            const int numSamples = blockSizes[block];
            juce::AudioBuffer<float> audio (2, numSamples);
            juce::MidiBuffer midi;

            if (block == 0)
            {
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, 48, (juce::uint8) 112),
                    0);
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, 60, (juce::uint8) 104),
                    8);
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, 67, (juce::uint8) 96),
                    16);
            }

            if (block + 1 == blockSizes.size())
            {
                const int noteOffSample =
                    juce::jmax (0, numSamples - 1);
                midi.addEvent (
                    juce::MidiMessage::noteOff (1, 48),
                    noteOffSample);
                midi.addEvent (
                    juce::MidiMessage::noteOff (1, 60),
                    noteOffSample);
                midi.addEvent (
                    juce::MidiMessage::noteOff (1, 67),
                    noteOffSample);
            }

            audio.clear();
            processor.processBlock (audio, midi);
            expectedSamples +=
                static_cast<std::uint64_t> (numSamples);

            for (int channel = 0;
                 channel < audio.getNumChannels();
                 ++channel)
            {
                const auto* samples =
                    audio.getReadPointer (channel);

                for (int sample = 0;
                     sample < numSamples;
                     ++sample)
                {
                    const float value = samples[sample];

                    if (! std::isfinite (value))
                        return false;

                    peak = juce::jmax (
                        peak, std::abs (value));
                    sumSquares +=
                        (double) value * (double) value;
                }
            }
        }

        const auto after = organ->diagnostics();
        return after.renderedSamples == expectedSamples
            && after.invalidOutputSamples == 0
            && std::isfinite (after.lastBlockPeak)
            && peak > 1.0e-5f
            && peak < 24.0f
            && sumSquares > 1.0e-8;
    };

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (
        sampleRate, blockSizes.back());

    if (! processor->loadChurchOrganIntoLayer (
            layer, Family::tonewheel)
         || processor->getLayerChurchOrganFamily (layer)
                != Family::tonewheel
         || ! hasUnityOrganTrim (*processor))
        return false;

    auto* organ =
        processor->getLayerChurchOrganProcessor (layer);

    if (organ == nullptr
         || ! organ->diagnostics().prepared
         || organ->diagnostics().suspended
         || ! close (
                organ->parameters().getPlain (
                    Parameter::performanceAttackMs),
                0.0f)
         || ! close (
                organ->parameters().getPlain (
                    Parameter::performanceReleaseMs),
                0.0f))
        return false;

    organ->setParameterPlain (Parameter::age, 0.37f);
    setTone (*processor, toneA);

    if (! renderVariableBlocks (*processor)
         || ! close (
                organ->parameters().getPlain (
                    Parameter::performanceAttackMs),
                toneA.attack * 1000.0f, 0.01f)
         || ! close (
                organ->parameters().getPlain (
                    Parameter::performanceReleaseMs),
                toneA.release * 1000.0f, 0.01f))
        return false;

    // A -> B -> A no processador real. A troca precisa memorizar os controles
    // do canal por familia, sem confundir o perfil do motor sintetizado com o
    // trim de samples PAD/AMBIENCE.
    if (! processor->loadChurchOrganIntoLayer (
            layer, Family::transistor)
         || processor->getLayerChurchOrganFamily (layer)
                != Family::transistor
         || ! hasUnityOrganTrim (*processor))
        return false;

    setTone (*processor, toneB);

    if (! processor->loadChurchOrganIntoLayer (
            layer, Family::tonewheel)
         || ! toneMatches (*processor, toneA)
         || ! hasUnityOrganTrim (*processor))
        return false;

    organ = processor->getLayerChurchOrganProcessor (layer);
    if (organ == nullptr
         || ! close (
                organ->parameters().getPlain (
                    Parameter::age),
                0.37f))
        return false;

    // Brake e o quarto estado do Rotary interno, mas o parametro legado do
    // canal possui somente Stop/Slow/Fast. A memoria A->B->A precisa guardar o
    // estado real do Church Organ, sem rebaixa-lo para Stop ao trocar familia.
    currentGate = "Church Organ Brake A-B-A memory";
    processor->setChurchOrganRotaryMode (
        layer,
        churchdaw::organ::ChurchOrganRotaryMode::brake);

    currentGate = "Church Organ Brake immediate state";

    if (! close (
            organ->parameters().getPlain (
                Parameter::rotaryMode),
            3.0f))
        return false;

    currentGate = "Church Organ Brake switch to B";

    if (! processor->loadChurchOrganIntoLayer (
            layer, Family::transistor))
        return false;

    currentGate = "Church Organ Brake return to A";

    if (! processor->loadChurchOrganIntoLayer (
            layer, Family::tonewheel))
        return false;

    organ = processor->getLayerChurchOrganProcessor (layer);
    currentGate = "Church Organ Brake recalled value";

    const float recalledBrake =
        organ != nullptr
            ? organ->parameters().getPlain (
                  Parameter::rotaryMode)
            : -1.0f;

    if (organ == nullptr
         || ! close (recalledBrake, 3.0f))
    {
        std::cerr << "Brake recalled plain="
                  << recalledBrake
                  << " channel="
                  << processor->getLayerRotaryModeParam (
                         layer)->getIndex()
                  << std::endl;
        return false;
    }

    currentGate = "Church Organ Brake save state";
    juce::MemoryBlock savedState;
    processor->getStateInformation (savedState);

    if (savedState.isEmpty())
        return false;

    // Simula restart de host que entrega o estado antes de prepareToPlay().
    auto restarted = std::make_unique<TimbresProcessor>();
    currentGate = "Church Organ Brake restore before prepare";
    restarted->setStateInformation (
        savedState.getData(),
        (int) savedState.getSize());

    if (! restarted->isLayerChurchOrgan (layer)
         || restarted->getLayerChurchOrganFamily (layer)
                != Family::tonewheel
         || ! close (
                restarted->getLayerNativeFamilyTrim (layer),
                1.0f))
        return false;

    restarted->prepareToPlay (
        sampleRate, blockSizes.back());
    auto* restoredOrgan =
        restarted->getLayerChurchOrganProcessor (layer);
    currentGate = "Church Organ Brake restore after prepare";

    const bool restoredOrganReady =
        restoredOrgan != nullptr
        && restoredOrgan->diagnostics().prepared
        && ! restoredOrgan->diagnostics().suspended;
    const float restoredAge =
        restoredOrgan != nullptr
            ? restoredOrgan->parameters().getPlain (
                  Parameter::age)
            : -1.0f;
    const float restoredRotaryMode =
        restoredOrgan != nullptr
            ? restoredOrgan->parameters().getPlain (
                  Parameter::rotaryMode)
            : -1.0f;
    const bool restoredTone =
        toneShapeMatches (*restarted, toneA);
    const bool restoredTrim =
        hasUnityOrganTrim (*restarted);

    if (! restoredOrganReady
         || restarted->getLayerChurchOrganFamily (layer)
                != Family::tonewheel
         || ! close (restoredAge, 0.37f)
         || ! close (restoredRotaryMode, 3.0f)
         || ! restoredTone
         || ! restoredTrim)
    {
        std::cerr
            << "Brake restart ready=" << restoredOrganReady
            << " family="
            << static_cast<int> (
                   restarted->getLayerChurchOrganFamily (
                       layer))
            << " age=" << restoredAge
            << " rotary=" << restoredRotaryMode
            << " tone=" << restoredTone
            << " trim=" << restoredTrim
            << std::endl;
        return false;
    }

    currentGate = "Church Organ Brake render after restart";

    if (! renderVariableBlocks (*restarted))
        return false;

    // O banco A/B tambem precisa sobreviver ao restart, nao apenas o perfil
    // atualmente selecionado.
    currentGate = "Church Organ Brake memory bank load B";

    if (! restarted->loadChurchOrganIntoLayer (
            layer, Family::transistor))
        return false;

    currentGate = "Church Organ Brake memory bank tone B";

    if (! toneMatches (*restarted, toneB))
        return false;

    currentGate = "Church Organ Brake memory bank load A";

    if (! restarted->loadChurchOrganIntoLayer (
            layer, Family::tonewheel))
        return false;

    currentGate = "Church Organ Brake memory bank tone A";
    const auto* brakeAfterBank =
        restarted->getLayerChurchOrganProcessor (layer);

    if (! toneShapeMatches (*restarted, toneA)
         || brakeAfterBank == nullptr
         || ! close (
                brakeAfterBank->parameters().getPlain (
                    Parameter::rotaryMode),
                3.0f)
         || ! hasUnityOrganTrim (*restarted))
        return false;

    processor.reset();

    // Um blob ainda nao reconhecido pelo motor pode voltar a ser valido em uma
    // versao futura. Se o host restaura antes de prepareToPlay(), o fallback da
    // familia mantem o canal utilizavel sem substituir esse blob no autosave.
    {
        currentGate = "pending adversarial state injection";
        auto pendingSource =
            std::make_unique<TimbresProcessor>();

        if (! pendingSource->loadChurchOrganIntoLayer (
                layer, Family::pipe))
            return false;

        juce::MemoryBlock sourceState;
        pendingSource->getStateInformation (sourceState);
        auto sourceXml =
            juce::AudioProcessor::getXmlFromBinary (
                sourceState.getData(),
                (int) sourceState.getSize());
        juce::XmlElement* sourceLayer = nullptr;

        if (sourceXml == nullptr)
            return false;

        forEachXmlChildElementWithTagName (
            *sourceXml, candidate, "LAYER")
            if (candidate->getIntAttribute (
                    "index", -1) == layer)
            {
                sourceLayer = candidate;
                break;
            }

        constexpr std::array<std::uint8_t, 31>
            adversarialBytes {{
                0x00, 0xff, 0x43, 0x68, 0x75, 0x72, 0x63, 0x68,
                0x4f, 0x72, 0x67, 0x61, 0x6e, 0x53, 0x74, 0x61,
                0x74, 0x65, 0x00, 0x7f, 0x80, 0x01, 0xfe, 0x10,
                0x20, 0x40, 0x08, 0x04, 0x02, 0x01, 0xaa
            }};
        const juce::MemoryBlock adversarialState (
            adversarialBytes.data(),
            adversarialBytes.size());
        const auto adversarialBase64 =
            adversarialState.toBase64Encoding();

        if (sourceLayer == nullptr
             || adversarialBase64.isEmpty())
            return false;

        sourceLayer->setAttribute (
            "churchOrgan", true);
        sourceLayer->setAttribute (
            "organFamily",
            (int) Family::pipe);
        sourceLayer->setAttribute (
            "organState",
            adversarialBase64);

        juce::MemoryBlock injectedState;
        juce::AudioProcessor::copyXmlToBinary (
            *sourceXml, injectedState);

        const auto savePreservesPending =
            [&] (TimbresProcessor& host,
                 juce::MemoryBlock& snapshot)
        {
            snapshot.reset();
            host.getStateInformation (snapshot);
            const auto xml =
                juce::AudioProcessor::getXmlFromBinary (
                    snapshot.getData(),
                    (int) snapshot.getSize());

            if (xml == nullptr)
                return false;

            forEachXmlChildElementWithTagName (
                *xml, candidate, "LAYER")
                if (candidate->getIntAttribute (
                        "index", -1) == layer)
                    return candidate->getBoolAttribute (
                               "churchOrgan", false)
                        && candidate->getIntAttribute (
                               "organFamily", -1)
                               == (int) Family::pipe
                        && candidate->getStringAttribute (
                               "organState")
                               == adversarialBase64;

            return false;
        };

        auto pendingHost =
            std::make_unique<TimbresProcessor>();
        pendingHost->setStateInformation (
            injectedState.getData(),
            (int) injectedState.getSize());

        if (! pendingHost->isLayerChurchOrgan (layer)
             || pendingHost->getLayerChurchOrganFamily (
                    layer) != Family::pipe)
            return false;

        currentGate =
            "pending adversarial save before prepare";
        juce::MemoryBlock beforePrepareSave;

        if (! savePreservesPending (
                *pendingHost, beforePrepareSave))
            return false;

        currentGate =
            "pending adversarial fallback after prepare";
        pendingHost->prepareToPlay (
            sampleRate, blockSizes.back());
        const auto* fallbackOrgan =
            pendingHost->getLayerChurchOrganProcessor (
                layer);

        if (fallbackOrgan == nullptr
             || ! fallbackOrgan->diagnostics().prepared
             || fallbackOrgan->diagnostics().suspended
             || fallbackOrgan->activeFamily()
                    != Family::pipe)
            return false;

        juce::MemoryBlock fallbackSave;

        if (! savePreservesPending (
                *pendingHost, fallbackSave))
            return false;

        currentGate =
            "pending adversarial restart before prepare";
        auto pendingRestart =
            std::make_unique<TimbresProcessor>();
        pendingRestart->setStateInformation (
            fallbackSave.getData(),
            (int) fallbackSave.getSize());
        juce::MemoryBlock restartBeforePrepareSave;

        if (! savePreservesPending (
                *pendingRestart,
                restartBeforePrepareSave))
            return false;

        currentGate =
            "pending adversarial restart after prepare";
        pendingRestart->prepareToPlay (
            sampleRate, blockSizes.back());
        const auto* restartedFallbackOrgan =
            pendingRestart
                ->getLayerChurchOrganProcessor (layer);
        juce::MemoryBlock restartAfterPrepareSave;

        if (restartedFallbackOrgan == nullptr
             || ! restartedFallbackOrgan
                    ->diagnostics().prepared
             || restartedFallbackOrgan
                    ->diagnostics().suspended
             || restartedFallbackOrgan->activeFamily()
                    != Family::pipe
             || ! savePreservesPending (
                    *pendingRestart,
                    restartAfterPrepareSave))
            return false;
    }

    // Integração real das cinco camadas. O produto é um layer-stack por design:
    // transmite o mesmo bloco MIDI aos instrumentos internos e não oferece
    // roteamento MIDI por layer. Este probe não alega esse roteamento; usa SOLO,
    // canal/manual e acorde sequenciais para provar independência de estado e
    // saída. As cinco instâncias continuam carregadas e renderizando em todos os
    // blocos, enquanto o medidor pós-SOLO prova qual camada chegou à saída.
    {
        currentGate = "five-layer setup";
        // O probe usa cinco camadas com valores escritos a mao (familia,
        // idade, expressao, tom, canal MIDI e nota por camada). Ele NAO
        // acompanha numLayers: quando a mesa passou de 5 para 14 canais
        // em 31/07/2026, amarrar layerCount a numLayers deixaria nove
        // conjuntos de dados zerados e o teste passaria a medir silencio.
        // Cinco camadas independentes ja provam o que este probe alega —
        // isolamento de estado e de saida entre camadas.
        constexpr int layerCount = 5;
        static_assert (
            TimbresProcessor::numLayers >= layerCount,
            "O ChurchDAW precisa de pelo menos cinco camadas para este probe");
        static_assert (
            TimbresProcessor::numLayers > layerCount,
            "O probe de SOLO fantasma precisa de pelo menos um canal vazio");
        constexpr int emptyLayerIndex =
            TimbresProcessor::numLayers - 1;

        const std::array<Family, layerCount> families {{
            Family::tonewheel,
            Family::transistor,
            Family::pipe,
            Family::worshipHybrid,
            Family::tonewheel
        }};
        const std::array<float, layerCount> ages {{
            0.11f, 0.23f, 0.35f, 0.47f, 0.59f
        }};
        const std::array<float, layerCount> expressions {{
            0.91f, 0.89f, 0.87f, 0.85f, 0.83f
        }};
        const std::array<Tone, layerCount> tones {{
            {
                0.031f, 0.75f,
                1.00f, -1.00f, 0.50f,
                ChurchDAWRotary::Speaker::slowMode, 0.21f
            },
            {
                0.073f, 1.15f,
                -1.50f, 0.75f, 1.00f,
                ChurchDAWRotary::Speaker::fastMode, 0.34f
            },
            {
                0.119f, 1.75f,
                2.00f, 1.25f, -0.75f,
                ChurchDAWRotary::Speaker::slowMode, 0.47f
            },
            {
                0.167f, 2.35f,
                -2.25f, -1.50f, 1.75f,
                ChurchDAWRotary::Speaker::stopMode, 0.58f
            },
            {
                0.213f, 2.95f,
                3.00f, -2.00f, 2.25f,
                ChurchDAWRotary::Speaker::fastMode, 0.71f
            }
        }};
        const std::array<int, layerCount> midiChannels {{
            1, 1, 2, 1, 3
        }};
        const std::array<int, layerCount> baseNotes {{
            48, 52, 48, 55, 36
        }};

        const auto expectedInternalRotaryMode = [] (
            int channelMode)
        {
            return channelMode
                       == ChurchDAWRotary::Speaker::slowMode
                     ? 1.0f
                 : channelMode
                       == ChurchDAWRotary::Speaker::fastMode
                     ? 2.0f
                     : 0.0f;
        };

        const auto setLayerTone = [&] (
            TimbresProcessor& target,
            int targetLayer,
            const Tone& tone)
        {
            *target.getLayerAttackParam (targetLayer) =
                tone.attack;
            *target.getLayerReleaseParam (targetLayer) =
                tone.release;
            *target.getLayerLowEqParam (targetLayer) =
                tone.low;
            *target.getLayerMidEqParam (targetLayer) =
                tone.mid;
            *target.getLayerHighEqParam (targetLayer) =
                tone.high;
            *target.getLayerRotaryModeParam (targetLayer) =
                tone.rotaryMode;
            *target.getLayerRotaryAmountParam (targetLayer) =
                tone.rotaryAmount;
        };

        const auto layerToneMatches = [&] (
            const TimbresProcessor& target,
            int targetLayer,
            const Tone& expected)
        {
            return close (
                       target.getLayerAttackParam (
                           targetLayer)->get(),
                       expected.attack)
                && close (
                       target.getLayerReleaseParam (
                           targetLayer)->get(),
                       expected.release)
                && close (
                       target.getLayerLowEqParam (
                           targetLayer)->get(),
                       expected.low)
                && close (
                       target.getLayerMidEqParam (
                           targetLayer)->get(),
                       expected.mid)
                && close (
                       target.getLayerHighEqParam (
                           targetLayer)->get(),
                       expected.high)
                && target.getLayerRotaryModeParam (
                       targetLayer)->getIndex()
                       == expected.rotaryMode
                && close (
                       target.getLayerRotaryAmountParam (
                           targetLayer)->get(),
                       expected.rotaryAmount);
        };

        const auto hasUnityTrimAt = [&] (
            const TimbresProcessor& target,
            int targetLayer)
        {
            const auto* organ =
                target.getLayerChurchOrganProcessor (
                    targetLayer);

            if (organ == nullptr
                 || ! target.isLayerChurchOrgan (
                        targetLayer)
                 || ! close (
                        target.getLayerNativeFamilyTrim (
                            targetLayer),
                        1.0f)
                 || ! close (
                        organ->diagnostics()
                            .familyOutputTrim,
                        1.0f))
                return false;

            for (int familyIndex = 0;
                 familyIndex < 4;
                 ++familyIndex)
                if (! close (
                        organ->familyOutputTrim (
                            static_cast<Family> (
                                familyIndex)),
                        1.0f))
                    return false;

            return true;
        };

        auto multilayer =
            std::make_unique<TimbresProcessor>();
        multilayer->prepareToPlay (
            sampleRate, blockSizes.back());

        std::array<juce::AudioParameterBool*,
                   layerCount> soloParameters {};
        auto* emptyLayerSolo = findBoolParameter (
            *multilayer,
            "solo" + juce::String (emptyLayerIndex));

        if (emptyLayerSolo == nullptr)
            return false;

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            if (! multilayer->loadChurchOrganIntoLayer (
                    targetLayer, families[targetLayer]))
                return false;

            auto* organ =
                multilayer->getLayerChurchOrganProcessor (
                    targetLayer);
            soloParameters[targetLayer] =
                findBoolParameter (
                    *multilayer,
                    "solo" + juce::String (
                        targetLayer));
            auto* reverb =
                findFloatParameter (
                    *multilayer,
                    "rvb" + juce::String (
                        targetLayer));
            auto* delay =
                findFloatParameter (
                    *multilayer,
                    "dly" + juce::String (
                        targetLayer));
            auto* compressor =
                findFloatParameter (
                    *multilayer,
                    "comp" + juce::String (
                        targetLayer));

            if (organ == nullptr
                 || soloParameters[targetLayer] == nullptr
                 || reverb == nullptr
                 || delay == nullptr
                 || compressor == nullptr
                 || organ->activeFamily()
                        != families[targetLayer]
                 || ! organ->diagnostics().prepared
                 || organ->diagnostics().suspended
                 || ! hasUnityTrimAt (
                        *multilayer, targetLayer))
                return false;

            *soloParameters[targetLayer] = false;
            *reverb = 0.0f;
            *delay = 0.0f;
            *compressor = 0.0f;
            organ->setParameterPlain (
                Parameter::age, ages[targetLayer]);
            organ->setParameterPlain (
                Parameter::expression,
                expressions[targetLayer]);
            organ->setParameterPlain (
                Parameter::reverbAmount, 0.0f);
            setLayerTone (
                *multilayer,
                targetLayer,
                tones[targetLayer]);
        }

        std::array<std::uint64_t,
                   layerCount> expectedSamples {};

        for (int currentLayer = 0;
             currentLayer < layerCount;
             ++currentLayer)
            expectedSamples[currentLayer] =
                multilayer
                    ->getLayerChurchOrganProcessor (
                        currentLayer)
                    ->diagnostics().renderedSamples;

        const auto processObserved = [&] (
            int numSamples,
            juce::MidiBuffer& midi,
            AudioStats& stats)
        {
            juce::AudioBuffer<float> audio (
                2, numSamples);
            audio.clear();
            multilayer->processBlock (audio, midi);

            if (! accumulateAudio (audio, stats))
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
            {
                expectedSamples[currentLayer] +=
                    static_cast<std::uint64_t> (
                        numSamples);
                const auto diagnostics =
                    multilayer
                        ->getLayerChurchOrganProcessor (
                            currentLayer)
                        ->diagnostics();

                if (diagnostics.renderedSamples
                        != expectedSamples[currentLayer]
                     || diagnostics.invalidOutputSamples
                            != 0
                     || diagnostics.droppedAdapterEvents
                            != 0
                     || diagnostics.droppedEngineEvents
                            != 0
                     || ! std::isfinite (
                            diagnostics.lastBlockPeak)
                     || diagnostics.family
                            != families[currentLayer]
                     || ! diagnostics.prepared
                     || diagnostics.suspended)
                    return false;
            }

            return true;
        };

        const auto drainMultilayerNotes = [&]
        {
            for (int block = 0;
                 block < 16;
                 ++block)
            {
                bool allClear = true;

                for (int currentLayer = 0;
                     currentLayer < layerCount;
                     ++currentLayer)
                    allClear =
                        allClear
                        && multilayer
                               ->getLayerChurchOrganProcessor (
                                   currentLayer)
                               ->diagnostics().activeNotes
                               == 0;

                if (allClear)
                    return true;

                juce::MidiBuffer drainMidi;
                AudioStats drainStats;

                if (! processObserved (
                        1024, drainMidi,
                        drainStats))
                    return false;
            }

            return false;
        };

        // Regressao macOS/estado legado: SOLO num slot sem identidade nao pode
        // ligar o modo global e calar os instrumentos carregados. O teste passa
        // pelo prepareToPlay E pelo render, cobrindo as duas copias historicas
        // do calculo. O ultimo canal permanece vazio de proposito.
        currentGate = "empty-layer solo isolation";

        for (int loadedLayer = 0;
             loadedLayer < layerCount;
             ++loadedLayer)
            if (! multilayer->isLayerLoaded (loadedLayer))
                return false;

        if (multilayer->isLayerLoaded (emptyLayerIndex))
            return false;

        *emptyLayerSolo = true;

        if (multilayer->hasAnyLoadedSolo())
            return false;

        multilayer->prepareToPlay (
            sampleRate, blockSizes.back());

        for (int currentLayer = 0;
             currentLayer < layerCount;
             ++currentLayer)
            expectedSamples[currentLayer] =
                multilayer
                    ->getLayerChurchOrganProcessor (
                        currentLayer)
                    ->diagnostics().renderedSamples;

        multilayer->panic();
        juce::MidiBuffer ghostCleanupMidi;
        AudioStats ghostCleanupStats;

        if (! processObserved (
                blockSizes.front(), ghostCleanupMidi,
                ghostCleanupStats)
             || ! drainMultilayerNotes())
            return false;

        for (int currentLayer = 0;
             currentLayer < layerCount;
             ++currentLayer)
        {
            float left = 0.0f;
            float right = 0.0f;
            multilayer->readAndResetLayerLevels (
                currentLayer, left, right);
        }

        AudioStats ghostSoloStats;

        for (std::size_t block = 0;
             block < blockSizes.size();
             ++block)
        {
            const int numSamples = blockSizes[block];
            juce::MidiBuffer midi;

            if (block == 0)
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        midiChannels[0], baseNotes[0],
                        static_cast<juce::uint8> (112)),
                    0);

            if (block + 1 == blockSizes.size())
                midi.addEvent (
                    juce::MidiMessage::noteOff (
                        midiChannels[0], baseNotes[0]),
                    numSamples - 1);

            if (! processObserved (
                    numSamples, midi, ghostSoloStats))
                return false;
        }

        if (ghostSoloStats.peak <= 1.0e-5f
             || ghostSoloStats.sumSquares <= 1.0e-8
             || ghostSoloStats.valueCount == 0)
            return false;

        bool heardLoadedLayer = false;

        for (int currentLayer = 0;
             currentLayer < layerCount;
             ++currentLayer)
        {
            float left = 0.0f;
            float right = 0.0f;
            multilayer->readAndResetLayerLevels (
                currentLayer, left, right);
            heardLoadedLayer = heardLoadedLayer
                            || juce::jmax (left, right) > 1.0e-5f;
        }

        if (! heardLoadedLayer)
            return false;

        *emptyLayerSolo = false;

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            currentGate =
                "five-layer variable blocks layer "
                + juce::String (targetLayer);
            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                *soloParameters[currentLayer] =
                    currentLayer == targetLayer;

            multilayer->prepareToPlay (
                sampleRate, blockSizes.back());

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                expectedSamples[currentLayer] =
                    multilayer
                        ->getLayerChurchOrganProcessor (
                            currentLayer)
                        ->diagnostics()
                        .renderedSamples;

            multilayer->panic();
            juce::MidiBuffer cleanupMidi;
            AudioStats cleanupStats;

            if (! processObserved (
                    blockSizes.front(),
                    cleanupMidi,
                    cleanupStats)
                 || ! drainMultilayerNotes())
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
            {
                float left = 0.0f;
                float right = 0.0f;
                multilayer->readAndResetLayerLevels (
                    currentLayer, left, right);
            }

            AudioStats targetStats;
            const int channel =
                midiChannels[targetLayer];
            const int root =
                baseNotes[targetLayer];
            constexpr std::array<int, 3>
                chordOffsets {{ 0, 7, 12 }};

            for (std::size_t block = 0;
                 block < blockSizes.size();
                 ++block)
            {
                const int numSamples =
                    blockSizes[block];
                juce::MidiBuffer midi;

                if (block == 0)
                    for (std::size_t note = 0;
                         note < chordOffsets.size();
                         ++note)
                        midi.addEvent (
                            juce::MidiMessage::noteOn (
                                channel,
                                root
                                    + chordOffsets[note],
                                static_cast<juce::uint8> (
                                    112
                                    - 8 * (int) note)),
                            8 * (int) note);

                if (block + 1
                     == blockSizes.size())
                    for (std::size_t note = 0;
                         note < chordOffsets.size();
                         ++note)
                        midi.addEvent (
                            juce::MidiMessage::noteOff (
                                channel,
                                root
                                    + chordOffsets[note]),
                            numSamples - 1
                                - 8 * (int) (
                                    chordOffsets.size()
                                    - 1 - note));

                if (! processObserved (
                        numSamples, midi,
                        targetStats))
                    return false;
            }

            if (targetStats.peak <= 1.0e-5f
                 || targetStats.peak >= 24.0f
                 || targetStats.sumSquares
                        <= 1.0e-8
                 || targetStats.valueCount == 0)
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
            {
                float left = 0.0f;
                float right = 0.0f;
                multilayer->readAndResetLayerLevels (
                    currentLayer, left, right);
                const float layerPeak =
                    juce::jmax (left, right);

                if (currentLayer == targetLayer
                     ? layerPeak <= 1.0e-5f
                     : layerPeak > 1.0e-8f)
                    return false;
            }
        }

        // Matriz P0 do Rotary. Há sempre exatamente um owner: Tonewheel e
        // Worship usam o RotaryCabinet interno; Transistor e Pipe usam somente
        // o ChannelRotarySpeaker. Cada camada atravessa bypass/full-wet e
        // Slow/Fast/Brake/Stop com áudio real, sem NaN, null, desaparecimento
        // ou a atenuação que denunciaria dois estágios em série.
        using RotaryMode =
            churchdaw::organ::ChurchOrganRotaryMode;
        const std::array<RotaryMode, 4>
            rotaryModes {{
                RotaryMode::slow,
                RotaryMode::fast,
                RotaryMode::brake,
                RotaryMode::stop
            }};
        const std::array<float, 2>
            rotaryAmounts {{ 0.0f, 1.0f }};

        for (int rotaryLayer = 0;
             rotaryLayer < layerCount;
             ++rotaryLayer)
        {
            currentGate =
                "Rotary ownership family/layer "
                + juce::String (rotaryLayer);
            const bool expectedInternal =
                families[rotaryLayer]
                    == Family::tonewheel
                || families[rotaryLayer]
                    == Family::worshipHybrid;
            const bool usesInternal =
                multilayer
                    ->layerUsesInternalChurchOrganRotary (
                        rotaryLayer);
            const bool usesChannel =
                multilayer->layerUsesChannelRotary (
                    rotaryLayer);

            if (usesInternal == usesChannel
                 || usesInternal != expectedInternal
                 || usesChannel == expectedInternal)
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                *soloParameters[currentLayer] =
                    currentLayer == rotaryLayer;

            std::array<std::array<double, 4>, 2>
                rotaryRms {};
            std::array<std::array<std::uint64_t, 4>, 2>
                rotaryFingerprints {};

            for (std::size_t amount = 0;
                 amount < rotaryAmounts.size();
                 ++amount)
            {
                for (std::size_t mode = 0;
                     mode < rotaryModes.size();
                     ++mode)
                {
                    currentGate =
                        "Rotary audio layer "
                        + juce::String (rotaryLayer)
                        + " amount "
                        + juce::String ((int) amount)
                        + " mode "
                        + juce::String ((int) mode);

                    // Cada combinação recomeça em estado determinístico. Isto
                    // impede release, fase ou velocidade da combinação anterior
                    // de mascarar a comparação dry/full-wet.
                    multilayer->prepareToPlay (
                        sampleRate,
                        blockSizes.back());

                    for (int currentLayer = 0;
                         currentLayer < layerCount;
                         ++currentLayer)
                    {
                        *soloParameters[currentLayer] =
                            currentLayer
                                == rotaryLayer;
                        expectedSamples[currentLayer] =
                            multilayer
                                ->getLayerChurchOrganProcessor (
                                    currentLayer)
                                ->diagnostics()
                                .renderedSamples;
                    }

                    multilayer->panic();
                    juce::MidiBuffer cleanupMidi;
                    AudioStats cleanupStats;

                    if (! processObserved (
                            blockSizes.front(),
                            cleanupMidi,
                            cleanupStats)
                         || ! drainMultilayerNotes())
                        return false;

                    multilayer->setChurchOrganRotaryMix (
                        rotaryLayer,
                        rotaryAmounts[amount]);

                    // Brake parte de uma rotação Fast real; os demais casos
                    // começam diretamente no modo que está sendo medido.
                    if (rotaryModes[mode]
                            == RotaryMode::brake)
                    {
                        multilayer
                            ->setChurchOrganRotaryMode (
                                rotaryLayer,
                                RotaryMode::fast);

                        for (int warmup = 0;
                             warmup < 4;
                             ++warmup)
                        {
                            juce::MidiBuffer warmupMidi;
                            AudioStats warmupStats;

                            if (! processObserved (
                                    1024,
                                    warmupMidi,
                                    warmupStats))
                                return false;
                        }
                    }

                    multilayer->setChurchOrganRotaryMode (
                        rotaryLayer,
                        rotaryModes[mode]);

                    AudioStats modeStats;
                    juce::MidiBuffer noteOn;
                    noteOn.addEvent (
                        juce::MidiMessage::allNotesOff (
                            midiChannels[rotaryLayer]),
                        0);
                    noteOn.addEvent (
                        juce::MidiMessage::noteOn (
                            midiChannels[rotaryLayer],
                            baseNotes[rotaryLayer],
                            (juce::uint8) 112),
                        1);
                    noteOn.addEvent (
                        juce::MidiMessage::noteOn (
                            midiChannels[rotaryLayer],
                            baseNotes[rotaryLayer] + 7,
                            (juce::uint8) 104),
                        9);

                    if (! processObserved (
                            512, noteOn, modeStats))
                        return false;

                    juce::MidiBuffer noteOff;
                    noteOff.addEvent (
                        juce::MidiMessage::noteOff (
                            midiChannels[rotaryLayer],
                            baseNotes[rotaryLayer]),
                        1008);
                    noteOff.addEvent (
                        juce::MidiMessage::noteOff (
                            midiChannels[rotaryLayer],
                            baseNotes[rotaryLayer] + 7),
                        1023);

                    if (! processObserved (
                            1024, noteOff,
                            modeStats)
                         || modeStats.valueCount == 0
                         || modeStats.peak <= 1.0e-5f
                         || modeStats.peak >= 24.0f
                         || modeStats.sumSquares
                                <= 1.0e-8)
                        return false;

                    rotaryRms[amount][mode] =
                        std::sqrt (
                            modeStats.sumSquares
                            / (double)
                                modeStats.valueCount);
                    rotaryFingerprints[amount][mode] =
                        modeStats.fingerprint;
                }
            }

            for (std::size_t mode = 0;
                 mode < rotaryModes.size();
                 ++mode)
            {
                const double dry =
                    rotaryRms[0][mode];
                const double wet =
                    rotaryRms[1][mode];
                const double ratio =
                    dry > 0.0 ? wet / dry : 0.0;

                if (! std::isfinite (dry)
                     || ! std::isfinite (wet)
                     || ! std::isfinite (ratio)
                     || dry <= 1.0e-6
                     || wet <= 1.0e-6
                     || ratio < 0.70
                     || ratio > 1.43
                     || rotaryFingerprints[0][mode]
                            == rotaryFingerprints[1][mode])
                {
                    currentGate =
                        "Rotary loudness/DSP layer "
                        + juce::String (rotaryLayer)
                        + " mode "
                        + juce::String ((int) mode)
                        + " ratio "
                        + juce::String (ratio, 4);
                    return false;
                }
            }
        }

        for (auto* solo : soloParameters)
            *solo = false;

        currentGate = "five-layer final profile";
        multilayer->panic();
        juce::MidiBuffer finalCleanupMidi;
        AudioStats finalCleanupStats;

        if (! processObserved (
                blockSizes.front(),
                finalCleanupMidi,
                finalCleanupStats))
            return false;

        // A matriz acima termina em Stop. Reaplique os cinco perfis distintos e
        // renderize um bloco para confirmar que nenhum estado atravessou camada,
        // inclusive entre as duas instâncias Tonewheel.
        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            auto* organ =
                multilayer
                    ->getLayerChurchOrganProcessor (
                        targetLayer);
            organ->setParameterPlain (
                Parameter::age, ages[targetLayer]);
            organ->setParameterPlain (
                Parameter::expression,
                expressions[targetLayer]);
            organ->setParameterPlain (
                Parameter::reverbAmount, 0.0f);
            setLayerTone (
                *multilayer,
                targetLayer,
                tones[targetLayer]);
        }

        juce::MidiBuffer syncMidi;
        AudioStats syncStats;

        if (! processObserved (
                blockSizes.front(),
                syncMidi,
                syncStats))
            return false;

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            const auto* organ =
                multilayer
                    ->getLayerChurchOrganProcessor (
                        targetLayer);
            const bool expectedInternal =
                families[targetLayer]
                    == Family::tonewheel
                || families[targetLayer]
                    == Family::worshipHybrid;

            if (organ == nullptr
                 || multilayer
                        ->getLayerChurchOrganFamily (
                            targetLayer)
                        != families[targetLayer]
                 || ! layerToneMatches (
                        *multilayer,
                        targetLayer,
                        tones[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::age),
                        ages[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::expression),
                        expressions[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::rotaryMode),
                        expectedInternalRotaryMode (
                            tones[targetLayer]
                                .rotaryMode))
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::rotaryMix),
                        tones[targetLayer]
                            .rotaryAmount)
                 || ! hasUnityTrimAt (
                        *multilayer, targetLayer)
                 || multilayer
                        ->layerUsesInternalChurchOrganRotary (
                            targetLayer)
                        != expectedInternal
                 || multilayer
                        ->layerUsesChannelRotary (
                            targetLayer)
                        == expectedInternal)
                return false;
        }

        for (const int tonewheelLayer :
             { 0, 4 })
        {
            const auto rotaryDiagnostics =
                multilayer
                    ->getLayerChurchOrganProcessor (
                        tonewheelLayer)
                    ->tonewheelEngine()
                    .diagnostics();

            if (! std::isfinite (
                    rotaryDiagnostics
                        .rotaryHornSpeedHz)
                 || ! std::isfinite (
                    rotaryDiagnostics
                        .rotaryDrumSpeedHz)
                 || rotaryDiagnostics
                        .rotaryHornSpeedHz <= 0.0f
                 || rotaryDiagnostics
                        .rotaryDrumSpeedHz <= 0.0f)
                return false;
        }

        juce::MemoryBlock multilayerState;
        currentGate = "five-layer state encoding";
        multilayer->getStateInformation (
            multilayerState);
        auto multilayerXml =
            juce::AudioProcessor::getXmlFromBinary (
                multilayerState.getData(),
                (int) multilayerState.getSize());

        if (multilayerState.isEmpty()
             || multilayerXml == nullptr)
            return false;

        // O estado salvo cobre TODOS os canais da mesa, nao apenas os
        // cinco que este probe carregou: o app grava um <LAYER> por
        // canal (14 desde 31/07/2026), e os vazios precisam estar la —
        // sem eles um projeto salvo perderia os canais ainda nao usados.
        // Dimensionar por layerCount fazia o teste reprovar justamente o
        // formato correto ("stateLayerCount != 5").
        constexpr int totalCanais = TimbresProcessor::numLayers;
        std::array<const juce::XmlElement*,
                   totalCanais> stateLayers {};
        int stateLayerCount = 0;

        forEachXmlChildElementWithTagName (
            *multilayerXml, candidate, "LAYER")
        {
            ++stateLayerCount;
            const int stateLayer =
                candidate->getIntAttribute (
                    "index", -1);

            if (! juce::isPositiveAndBelow (
                    stateLayer, totalCanais)
                 || stateLayers[stateLayer]
                        != nullptr)
                return false;

            stateLayers[stateLayer] = candidate;
        }

        if (stateLayerCount != totalCanais)
            return false;

        const juce::Identifier stateType {
            "ChurchOrganLayerState"
        };
        const juce::Identifier parametersType {
            "OrganParameters"
        };
        const juce::Identifier rotaryModeProperty {
            "p" + juce::String (
                (int) Parameter::rotaryMode)
        };
        const juce::Identifier rotaryMixProperty {
            "p" + juce::String (
                (int) Parameter::rotaryMix)
        };
        const juce::Identifier ageProperty {
            "p" + juce::String (
                (int) Parameter::age)
        };

        for (int stateLayer = 0;
             stateLayer < layerCount;
             ++stateLayer)
        {
            const auto* layerState =
                stateLayers[stateLayer];

            if (layerState == nullptr
                 || ! layerState->getBoolAttribute (
                        "churchOrgan", false)
                 || layerState->getIntAttribute (
                        "organFamily", -1)
                        != (int) families[stateLayer]
                 || layerState->getStringAttribute (
                        "organState").isEmpty()
                 || layerState->getStringAttribute (
                        "vstId").isNotEmpty()
                 || layerState->getStringAttribute (
                        "vstState").isNotEmpty()
                 || layerState->getIntAttribute (
                        "rotMode", -1)
                        != tones[stateLayer]
                               .rotaryMode
                 || ! close (
                        (float)
                            layerState
                                ->getDoubleAttribute (
                                    "rotAmt", -1.0),
                        tones[stateLayer]
                            .rotaryAmount))
                return false;

            juce::MemoryBlock organState;

            if (! organState.fromBase64Encoding (
                    layerState->getStringAttribute (
                        "organState")))
                return false;

            const auto stateTree =
                juce::ValueTree::readFromData (
                    organState.getData(),
                    organState.getSize());

            if (! stateTree.isValid()
                 || ! stateTree.hasType (
                        stateType)
                 || (int) stateTree.getProperty (
                        "family", -1)
                        != (int) families[stateLayer])
                return false;

            int parameterChildCount = 0;

            for (int child = 0;
                 child < stateTree.getNumChildren();
                 ++child)
                if (stateTree.getChild (
                        child).hasType (
                            parametersType))
                    ++parameterChildCount;

            const auto parameterState =
                stateTree.getChildWithName (
                    parametersType);

            // Um único filho de parâmetros contém o único Rotary do motor. Não
            // existe estado paralelo "Rotary" nem VST concorrente no canal.
            if (parameterChildCount != 1
                 || ! parameterState.isValid()
                 || stateTree.getChildWithName (
                        juce::Identifier {
                            "Rotary"
                        }).isValid()
                 || ! close (
                        (float) parameterState
                            .getProperty (
                                rotaryModeProperty),
                        expectedInternalRotaryMode (
                            tones[stateLayer]
                                .rotaryMode))
                 || ! close (
                        (float) parameterState
                            .getProperty (
                                rotaryMixProperty),
                        tones[stateLayer]
                            .rotaryAmount)
                 || ! close (
                        (float) parameterState
                            .getProperty (
                                ageProperty),
                        ages[stateLayer]))
                return false;
        }

        auto multilayerRestarted =
            std::make_unique<TimbresProcessor>();
        currentGate = "five-layer state restore";
        multilayerRestarted->setStateInformation (
            multilayerState.getData(),
            (int) multilayerState.getSize());

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
            if (! multilayerRestarted
                    ->isLayerChurchOrgan (
                        targetLayer)
                 || multilayerRestarted
                        ->getLayerChurchOrganFamily (
                            targetLayer)
                        != families[targetLayer]
                 || ! close (
                        multilayerRestarted
                            ->getLayerNativeFamilyTrim (
                                targetLayer),
                        1.0f))
                return false;

        multilayerRestarted->prepareToPlay (
            sampleRate, blockSizes.back());

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            const auto* organ =
                multilayerRestarted
                    ->getLayerChurchOrganProcessor (
                        targetLayer);
            const bool expectedInternal =
                families[targetLayer]
                    == Family::tonewheel
                || families[targetLayer]
                    == Family::worshipHybrid;

            if (organ == nullptr
                 || ! organ->diagnostics().prepared
                 || organ->diagnostics().suspended
                 || multilayerRestarted
                        ->getLayerSolo (
                            targetLayer)
                 || ! layerToneMatches (
                        *multilayerRestarted,
                        targetLayer,
                        tones[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::age),
                        ages[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::expression),
                        expressions[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::rotaryMode),
                        expectedInternalRotaryMode (
                            tones[targetLayer]
                                .rotaryMode))
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::rotaryMix),
                        tones[targetLayer]
                            .rotaryAmount)
                 || ! hasUnityTrimAt (
                        *multilayerRestarted,
                        targetLayer)
                 || multilayerRestarted
                        ->layerUsesInternalChurchOrganRotary (
                            targetLayer)
                        != expectedInternal
                 || multilayerRestarted
                        ->layerUsesChannelRotary (
                            targetLayer)
                        == expectedInternal)
                return false;
        }

        std::array<std::uint64_t,
                   layerCount> restartSamples {};

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
            restartSamples[targetLayer] =
                multilayerRestarted
                    ->getLayerChurchOrganProcessor (
                        targetLayer)
                    ->diagnostics().renderedSamples;

        juce::AudioBuffer<float> restartAudio (
            2, blockSizes.front());
        juce::MidiBuffer restartMidi;
        AudioStats restartStats;
        restartAudio.clear();
        multilayerRestarted->processBlock (
            restartAudio, restartMidi);

        if (! accumulateAudio (
                restartAudio, restartStats))
            return false;

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            const auto diagnostics =
                multilayerRestarted
                    ->getLayerChurchOrganProcessor (
                        targetLayer)
                    ->diagnostics();

            if (diagnostics.renderedSamples
                    != restartSamples[targetLayer]
                       + (std::uint64_t)
                            blockSizes.front()
                 || diagnostics.invalidOutputSamples
                        != 0
                 || ! std::isfinite (
                        diagnostics.lastBlockPeak))
                return false;

            restartSamples[targetLayer] =
                diagnostics.renderedSamples;
        }

        std::array<juce::AudioParameterBool*,
                   layerCount> restartSolos {};

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            restartSolos[targetLayer] =
                findBoolParameter (
                    *multilayerRestarted,
                    "solo" + juce::String (
                        targetLayer));

            if (restartSolos[targetLayer]
                    == nullptr)
                return false;
        }

        const auto processRestartedObserved = [&] (
            int numSamples,
            juce::MidiBuffer& midi,
            AudioStats& stats)
        {
            juce::AudioBuffer<float> audio (
                2, numSamples);
            audio.clear();
            multilayerRestarted->processBlock (
                audio, midi);

            if (! accumulateAudio (audio, stats))
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
            {
                restartSamples[currentLayer] +=
                    static_cast<std::uint64_t> (
                        numSamples);
                const auto diagnostics =
                    multilayerRestarted
                        ->getLayerChurchOrganProcessor (
                            currentLayer)
                        ->diagnostics();

                if (diagnostics.renderedSamples
                        != restartSamples[currentLayer]
                     || diagnostics.invalidOutputSamples
                            != 0
                     || diagnostics.droppedAdapterEvents
                            != 0
                     || diagnostics.droppedEngineEvents
                            != 0
                     || ! std::isfinite (
                            diagnostics.lastBlockPeak)
                     || diagnostics.family
                            != families[currentLayer]
                     || ! diagnostics.prepared
                     || diagnostics.suspended)
                    return false;
            }

            return true;
        };

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            currentGate =
                "five-layer restored audio layer "
                + juce::String (targetLayer);

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                *restartSolos[currentLayer] =
                    currentLayer == targetLayer;

            multilayerRestarted->prepareToPlay (
                sampleRate, blockSizes.back());

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                restartSamples[currentLayer] =
                    multilayerRestarted
                        ->getLayerChurchOrganProcessor (
                            currentLayer)
                        ->diagnostics()
                        .renderedSamples;

            multilayerRestarted->panic();
            juce::MidiBuffer cleanupMidi;
            AudioStats cleanupStats;

            if (! processRestartedObserved (
                    blockSizes.front(),
                    cleanupMidi,
                    cleanupStats))
                return false;

            for (int currentLayer = 0;
                 currentLayer < layerCount;
                 ++currentLayer)
                if (multilayerRestarted
                        ->getLayerChurchOrganProcessor (
                            currentLayer)
                        ->diagnostics().activeNotes
                        != 0)
                    return false;

            AudioStats restoredAudioStats;
            juce::MidiBuffer noteOn;
            noteOn.addEvent (
                juce::MidiMessage::noteOn (
                    midiChannels[targetLayer],
                    baseNotes[targetLayer],
                    (juce::uint8) 112),
                0);
            noteOn.addEvent (
                juce::MidiMessage::noteOn (
                    midiChannels[targetLayer],
                    baseNotes[targetLayer] + 7,
                    (juce::uint8) 104),
                8);

            if (! processRestartedObserved (
                    512, noteOn,
                    restoredAudioStats))
                return false;

            juce::MidiBuffer noteOff;
            noteOff.addEvent (
                juce::MidiMessage::noteOff (
                    midiChannels[targetLayer],
                    baseNotes[targetLayer]),
                1008);
            noteOff.addEvent (
                juce::MidiMessage::noteOff (
                    midiChannels[targetLayer],
                    baseNotes[targetLayer] + 7),
                1023);

            if (! processRestartedObserved (
                    1024, noteOff,
                    restoredAudioStats)
                 || restoredAudioStats.valueCount
                        == 0
                 || restoredAudioStats.peak
                        <= 1.0e-5f
                 || restoredAudioStats.peak
                        >= 24.0f
                 || restoredAudioStats.sumSquares
                        <= 1.0e-8)
                return false;
        }

        for (auto* solo : restartSolos)
            *solo = false;

        for (int targetLayer = 0;
             targetLayer < layerCount;
             ++targetLayer)
        {
            const auto* organ =
                multilayerRestarted
                    ->getLayerChurchOrganProcessor (
                        targetLayer);

            if (! layerToneMatches (
                    *multilayerRestarted,
                    targetLayer,
                    tones[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::age),
                        ages[targetLayer])
                 || ! close (
                        organ->parameters().getPlain (
                            Parameter::expression),
                        expressions[targetLayer]))
                return false;
        }
    }

    // Sustain global precisa entrar no primeiro bloco mesmo quando já estava ON
    // antes do load/reprepare. CC121 deve resetar e ressincronizar o latch; PANIC
    // continua absoluto, mas a nota seguinte volta funcional e sustentada.
    {
        currentGate = "global sustain before load";
        auto sustainHost =
            std::make_unique<TimbresProcessor>();
        auto* globalSustain =
            findBoolParameter (
                *sustainHost, "gsus");

        if (globalSustain == nullptr)
            return false;

        *globalSustain = true;
        sustainHost->prepareToPlay (
            sampleRate, blockSizes.back());

        if (! sustainHost->loadChurchOrganIntoLayer (
                layer, Family::tonewheel))
            return false;

        auto* sustainOrgan =
            sustainHost
                ->getLayerChurchOrganProcessor (
                    layer);
        auto* sustainReverb =
            findFloatParameter (
                *sustainHost, "rvb0");
        auto* sustainDelay =
            findFloatParameter (
                *sustainHost, "dly0");

        if (sustainOrgan == nullptr
             || sustainReverb == nullptr
             || sustainDelay == nullptr)
            return false;

        *sustainReverb = 0.0f;
        *sustainDelay = 0.0f;
        sustainHost->setChurchOrganRotaryMix (
            layer, 0.0f);
        sustainHost->setChurchOrganRotaryMode (
            layer,
            churchdaw::organ::
                ChurchOrganRotaryMode::stop);
        sustainOrgan->setParameterPlain (
            Parameter::reverbAmount, 0.0f);

        std::uint64_t expectedSustainSamples =
            sustainOrgan->diagnostics()
                .renderedSamples;

        const auto processSustainHost = [&] (
            int numSamples,
            juce::MidiBuffer& midi,
            AudioStats& stats)
        {
            juce::AudioBuffer<float> audio (
                2, numSamples);
            audio.clear();
            sustainHost->processBlock (
                audio, midi);
            expectedSustainSamples +=
                static_cast<std::uint64_t> (
                    numSamples);
            const auto diagnostics =
                sustainOrgan->diagnostics();

            return accumulateAudio (
                       audio, stats)
                && stats.peak < 24.0f
                && diagnostics.renderedSamples
                       == expectedSustainSamples
                && diagnostics.invalidOutputSamples
                       == 0
                && diagnostics.droppedAdapterEvents
                       == 0
                && diagnostics.droppedEngineEvents
                       == 0
                && std::isfinite (
                       diagnostics.lastBlockPeak);
        };

        const auto playReleasedSustainNote = [&] (
            int note,
            float& tailPeak)
        {
            juce::MidiBuffer noteOn;
            noteOn.addEvent (
                juce::MidiMessage::noteOn (
                    1, note,
                    (juce::uint8) 112),
                0);
            AudioStats noteOnStats;

            if (! processSustainHost (
                    512, noteOn, noteOnStats))
                return false;

            juce::MidiBuffer noteOff;
            noteOff.addEvent (
                juce::MidiMessage::noteOff (
                    1, note),
                511);
            AudioStats noteOffStats;

            if (! processSustainHost (
                    512, noteOff,
                    noteOffStats))
                return false;

            juce::MidiBuffer tailMidi;
            AudioStats tailStats;

            if (! processSustainHost (
                    512, tailMidi, tailStats))
                return false;

            tailPeak = tailStats.peak;
            return tailStats.peak > 1.0e-5f
                && tailStats.sumSquares > 1.0e-8
                && sustainOrgan->diagnostics()
                       .activeNotes > 0;
        };

        float initialTail = 0.0f;

        if (! playReleasedSustainNote (
                48, initialTail))
            return false;

        // Reprepare zera os motores, não o parâmetro global. O primeiro bloco
        // após a nova preparação deve publicar CC64 antes do novo note-on.
        currentGate = "global sustain after reprepare";
        sustainHost->prepareToPlay (
            sampleRate, blockSizes.back());
        sustainOrgan =
            sustainHost
                ->getLayerChurchOrganProcessor (
                    layer);

        if (sustainOrgan == nullptr
             || ! sustainOrgan->diagnostics()
                    .prepared
             || sustainOrgan->diagnostics()
                    .suspended
             || ! globalSustain->get())
            return false;

        expectedSustainSamples =
            sustainOrgan->diagnostics()
                .renderedSamples;
        float reprepareTail = 0.0f;

        if (! playReleasedSustainNote (
                55, reprepareTail))
            return false;

        juce::MidiBuffer resetControllers;
        currentGate = "global sustain after CC121";
        resetControllers.addEvent (
            juce::MidiMessage::controllerEvent (
                1, 121, 0),
            0);
        AudioStats resetStats;

        if (! processSustainHost (
                512, resetControllers,
                resetStats))
            return false;

        // CC121 solta a nota que pertencia ao latch anterior. O global é
        // reaplicado para os eventos futuros, não ressuscita aquela voz.
        bool previousLatchReleased = false;

        for (int block = 0;
             block < 8;
             ++block)
        {
            juce::MidiBuffer postResetMidi;
            AudioStats postResetStats;

            if (! processSustainHost (
                    512, postResetMidi,
                    postResetStats))
                return false;

            if (sustainOrgan->diagnostics()
                    .activeNotes == 0)
            {
                previousLatchReleased = true;
                break;
            }
        }

        if (! previousLatchReleased)
            return false;

        currentGate =
            "global sustain future note after CC121";
        float postResetTail = 0.0f;

        if (! playReleasedSustainNote (
                57, postResetTail))
            return false;

        sustainHost->panic();
        currentGate = "global sustain panic silence";
        float finalPanicPeak = 0.0f;

        for (int block = 0;
             block < 8;
             ++block)
        {
            juce::MidiBuffer panicMidi;
            AudioStats panicStats;

            if (! processSustainHost (
                    1024, panicMidi,
                    panicStats))
                return false;

            finalPanicPeak = panicStats.peak;
        }

        if (sustainOrgan->diagnostics()
                .activeNotes != 0
             || finalPanicPeak > 1.0e-3f)
            return false;

        float postPanicTail = 0.0f;
        currentGate = "global sustain note after panic";

        if (! playReleasedSustainNote (
                60, postPanicTail))
            return false;

        // CC120/123 recebidos pelo host sao globais para os tres manuais do
        // Church Organ, mesmo quando o controlador chega nos canais 4..16.
        // Alem de silenciar o motor, precisam zerar o ledger de transpose para
        // que a proxima nota nao herde a afinacao da frase anterior.
        currentGate = "global organ all-off and transpose ledger";
        *globalSustain = false;
        auto* layerTranspose =
            findIntParameter (*sustainHost, "transp0");

        if (layerTranspose == nullptr)
            return false;

        const auto waitForOrganSilence = [&] (
            int maxBlocks = 96)
        {
            for (int block = 0;
                 block < maxBlocks;
                 ++block)
            {
                if (sustainOrgan->diagnostics()
                        .activeNotes == 0)
                    return true;

                juce::MidiBuffer emptyMidi;
                AudioStats emptyStats;

                if (! processSustainHost (
                        1024, emptyMidi, emptyStats))
                    return false;
            }

            return sustainOrgan->diagnostics()
                       .activeNotes == 0;
        };

        const auto resetAllOffCase = [&]
        {
            sustainHost->panic();

            // panicPending e entregue no callback seguinte. Consuma-o num
            // bloco vazio; caso contrario ele divide o mesmo sample do primeiro
            // note-on da sonda e testa PANIC, nao o caso pretendido.
            juce::MidiBuffer panicDelivery;
            AudioStats panicDeliveryStats;
            return processSustainHost (
                       64, panicDelivery,
                       panicDeliveryStats)
                && waitForOrganSilence();
        };

        const auto startAllManuals = [&]
        {
            juce::MidiBuffer notes;
            notes.addEvent (
                juce::MidiMessage::noteOn (
                    1, 48, (juce::uint8) 112), 0);
            notes.addEvent (
                juce::MidiMessage::noteOn (
                    2, 55, (juce::uint8) 108), 4);
            notes.addEvent (
                juce::MidiMessage::noteOn (
                    3, 36, (juce::uint8) 120), 8);
            AudioStats stats;

            return processSustainHost (
                       512, notes, stats)
                && sustainOrgan->diagnostics()
                       .activeNotes >= 3;
        };

        for (const auto [controller, sourceChannel] :
                std::array<std::pair<int, int>, 2> {{
                    { 123, 1 },
                    { 120, 4 }
                }})
        {
            currentGate =
                "global organ all-off start CC"
                + juce::String (controller)
                + " ch" + juce::String (sourceChannel);

            if (! resetAllOffCase()
                 || ! startAllManuals())
                return false;

            currentGate =
                "global organ all-off render CC"
                + juce::String (controller)
                + " ch" + juce::String (sourceChannel);
            juce::MidiBuffer allOff;
            allOff.addEvent (
                juce::MidiMessage::controllerEvent (
                    sourceChannel, controller, 0), 64);
            AudioStats allOffStats;

            if (! processSustainHost (
                    512, allOff, allOffStats))
                return false;

            currentGate =
                "global organ all-off silence CC"
                + juce::String (controller)
                + " ch" + juce::String (sourceChannel);

            if (! waitForOrganSilence())
                return false;
        }

        // Dentro do adapter, all-off tem prioridade final no mesmo sample.
        // Portanto as duas ordens de origem abaixo devem terminar em silencio.
        for (const bool allOffFirst : { false, true })
        {
            currentGate =
                allOffFirst
                    ? "same-sample all-off before note"
                    : "same-sample note before all-off";

            if (! resetAllOffCase())
                return false;

            juce::MidiBuffer simultaneous;
            const auto note =
                juce::MidiMessage::noteOn (
                    1, 62, (juce::uint8) 112);
            const auto allOff =
                juce::MidiMessage::controllerEvent (
                    4, 123, 0);

            if (allOffFirst)
            {
                simultaneous.addEvent (allOff, 64);
                simultaneous.addEvent (note, 64);
            }
            else
            {
                simultaneous.addEvent (note, 64);
                simultaneous.addEvent (allOff, 64);
            }

            AudioStats simultaneousStats;

            if (! processSustainHost (
                    512, simultaneous,
                    simultaneousStats))
                return false;

            currentGate =
                allOffFirst
                    ? "same-sample all-off before note silence"
                    : "same-sample note before all-off silence";

            if (! waitForOrganSilence())
                return false;
        }

        // Se o all-off nao limpar o FIFO de transpose, este note-off sairia
        // em 72 (frase antiga) em vez de 48 (frase nova) e a voz ficaria presa.
        if (! resetAllOffCase())
            return false;

        *layerTranspose = 12;
        currentGate = "transpose ledger old phrase";
        juce::MidiBuffer oldPhrase;
        oldPhrase.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, (juce::uint8) 112), 0);
        AudioStats oldPhraseStats;

        if (! processSustainHost (
                512, oldPhrase, oldPhraseStats))
            return false;

        juce::MidiBuffer ledgerReset;
        currentGate = "transpose ledger global reset";
        ledgerReset.addEvent (
            juce::MidiMessage::controllerEvent (
                4, 123, 0), 0);
        AudioStats ledgerResetStats;

        if (! processSustainHost (
                512, ledgerReset,
                ledgerResetStats))
            return false;

        currentGate = "transpose ledger reset silence";

        if (! waitForOrganSilence())
            return false;

        *layerTranspose = -12;
        currentGate = "transpose ledger new phrase";
        juce::MidiBuffer newPhrase;
        newPhrase.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, (juce::uint8) 112), 0);
        AudioStats newPhraseStats;

        if (! processSustainHost (
                512, newPhrase, newPhraseStats)
             || sustainOrgan->diagnostics()
                    .activeNotes == 0)
            return false;

        juce::MidiBuffer newPhraseOff;
        currentGate = "transpose ledger new note-off";
        newPhraseOff.addEvent (
            juce::MidiMessage::noteOff (
                1, 60), 0);
        AudioStats newPhraseOffStats;

        if (! processSustainHost (
                512, newPhraseOff,
                newPhraseOffStats))
            return false;

        currentGate = "transpose ledger final silence";

        if (! waitForOrganSilence())
            return false;

        *layerTranspose = 0;
    }

    currentGate = "PAD trim isolation";
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-organ-pad-isolation",
                              {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeProbeWav (sample, 261.625565f))
        return false;

    TimbresProcessor::TimbreEntry pad;
    pad.label = "__Church Organ Trim Isolation__";
    pad.library = "Probe Pack";
    pad.category = "PADS";
    pad.files.add (sample);
    restarted->loadTimbreIntoLayer (
        layer, pad, false);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (
            *restarted, memoryPeak, 10)
         || restarted->getLayerTimbre (layer)
                != pad.label
         || restarted->isLayerChurchOrgan (layer)
         || ! close (
                restarted->getLayerNativeFamilyTrim (layer),
                TimbresProcessor::nativePadAmbienceTrim))
        return false;

    // Retornar do sample PAD ao motor sintetizado precisa remover +4,5 dB
    // integralmente, inclusive de todos os trims internos por familia.
    const bool padIsolationPassed =
        restarted->loadChurchOrganIntoLayer (
            layer, Family::worshipHybrid)
        && restarted->getLayerChurchOrganFamily (layer)
               == Family::worshipHybrid
        && hasUnityOrganTrim (*restarted);
    probePassed = padIsolationPassed;
    return padIsolationPassed;
}

struct NativeFamilyLevel
{
    double rms = 0.0;
    float peak = 0.0f;
    bool ok = false;
};

NativeFamilyLevel renderNativeFamilyLevel (
    const juce::File& sample,
    const juce::String& category,
    int voiceCount)
{
    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 128);

    TimbresProcessor::TimbreEntry entry;
    entry.label = "__Family Gain " + category + "__";
    entry.library = "Probe Pack";
    entry.category = category;
    entry.files.add (sample);
    processor->loadTimbreIntoLayer (0, entry, false);

    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != entry.label)
    {
        std::cerr << "FAMILY_GAIN_LOAD_FAIL category="
                  << category << " loaded="
                  << processor->getLayerTimbre (0) << "\n";
        return {};
    }

    const float expectedTrim =
        TimbresProcessor::nativeFamilyTrimForCategory (category);

    if (std::abs (
            processor->getLayerNativeFamilyTrim (0)
            - expectedTrim) > 0.0001f)
    {
        std::cerr << "FAMILY_GAIN_TRIM_FAIL category="
                  << category << " actual="
                  << processor->getLayerNativeFamilyTrim (0)
                  << " expected=" << expectedTrim << "\n";
        return {};
    }

    juce::AudioBuffer<float> audio (2, 128);
    juce::MidiBuffer midi;

    for (int channel = 1; channel <= voiceCount; ++channel)
        midi.addEvent (
            juce::MidiMessage::noteOn (
                channel, 60, (juce::uint8) 127),
            0);

    double sumSquares = 0.0;
    std::uint64_t measuredSamples = 0;
    float peak = 0.0f;

    for (int block = 0; block < 32; ++block)
    {
        audio.clear();
        processor->processBlock (audio, midi);

        // Descarta apenas o ataque de 3 ms. A janela restante fica muito antes
        // do fim do sample e mede o ganho estacionario, sem cauda/declique.
        if (block < 4)
            continue;

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto* samples = audio.getReadPointer (channel);

            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                const float value = samples[i];

                if (! std::isfinite (value))
                    return {};

                peak = juce::jmax (peak, std::abs (value));
                sumSquares += (double) value * (double) value;
                ++measuredSamples;
            }
        }
    }

    NativeFamilyLevel result;
    result.rms = measuredSamples > 0
                   ? std::sqrt (
                         sumSquares / (double) measuredSamples)
                   : 0.0;
    result.peak = peak;
    result.ok = result.rms > 1.0e-8
             && std::isfinite (result.rms)
             && std::isfinite (result.peak);
    return result;
}

bool runNativeSourceMetadataGainProbe()
{
    constexpr double sourceGain = 1.716853141784668; // Astralis.nsmp4
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-source-metadata-gain-probe",
                              {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto neutralDir = temp.getChildFile ("Neutral");
    const auto gainedDir = temp.getChildFile ("Gained");
    const auto bakedDir = temp.getChildFile ("AlreadyApplied");

    if (neutralDir.createDirectory().failed()
         || gainedDir.createDirectory().failed()
         || bakedDir.createDirectory().failed())
        return false;

    const auto neutralSample = neutralDir.getChildFile ("C3.wav");
    const auto gainedSample = gainedDir.getChildFile ("C3.wav");
    const auto bakedSample = bakedDir.getChildFile ("C3.wav");

    if (! writeProbeWav (neutralSample, 261.625565f)
         || ! writeProbeWav (gainedSample, 261.625565f)
         || ! writeProbeWav (bakedSample, 261.625565f))
        return false;

    const auto sidecar = [] (double gain, bool applied)
    {
        return juce::String (R"json({
  "schema": 1,
  "globalGain": {
    "linear": )json")
            + juce::String (gain, 12)
            + juce::String (R"json(,
    "appliedToPcm": )json")
            + (applied ? "true" : "false")
            + R"json(
  },
  "globalDetune": {
    "cents": 0.0,
    "appliedToPcm": false
  },
  "zones": [
    { "file": "C3.wav", "root": 60,
      "keyLo": 0, "keyHi": 127, "velLo": 0, "velHi": 127 }
  ]
})json";
    };

    if (! gainedDir.getChildFile ("churchdaw-timbre.json")
             .replaceWithText (sidecar (sourceGain, false))
         || ! bakedDir.getChildFile ("churchdaw-timbre.json")
                 .replaceWithText (sidecar (sourceGain, true)))
        return false;

    const auto neutral = renderNativeFamilyLevel (
        neutralSample, "PIANOS", 1);
    const auto gained = renderNativeFamilyLevel (
        gainedSample, "PIANOS", 1);
    const auto alreadyApplied = renderNativeFamilyLevel (
        bakedSample, "PIANOS", 1);

    if (! neutral.ok || ! gained.ok || ! alreadyApplied.ok
         || neutral.rms <= 0.0 || neutral.peak <= 0.0f)
        return false;

    const double rmsRatio = gained.rms / neutral.rms;
    const double peakRatio = (double) gained.peak / (double) neutral.peak;
    const double bakedRmsRatio = alreadyApplied.rms / neutral.rms;
    const double bakedPeakRatio =
        (double) alreadyApplied.peak / (double) neutral.peak;

    const bool passed = std::abs (rmsRatio - sourceGain) < 0.002
        && std::abs (peakRatio - sourceGain) < 0.002
        && std::abs (bakedRmsRatio - 1.0) < 0.002
        && std::abs (bakedPeakRatio - 1.0) < 0.002;

    if (passed)
        std::cout << "NATIVE_SOURCE_METADATA_GAIN_OK"
                  << " source_gain=" << sourceGain
                  << " rms_ratio=" << rmsRatio
                  << " peak_ratio=" << peakRatio
                  << " applied_pcm_ratio=" << bakedRmsRatio << "\n";

    return passed;
}

bool runNativeFamilyGainMatrix()
{
    if (std::abs (
            TimbresProcessor::nativeFamilyTrimForCategory (
                "AMBIENCIAS")
            - TimbresProcessor::nativePadAmbienceTrim) > 0.0001f
         || std::abs (
                TimbresProcessor::nativeFamilyTrimForCategory (
                    "PIANOS") - 1.0f) > 0.0001f
         || std::abs (
                TimbresProcessor::nativeFamilyTrimForCategory (
                    "ORGAOS") - 1.0f) > 0.0001f)
        return false;

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-family-gain-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    // O parser do produto usa a convencao Ableton: C3 == MIDI 60.
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeProbeWav (sample, 261.625565f))
        return false;

    constexpr std::array<int, 4> counts { 1, 4, 8, 16 };
    std::array<NativeFamilyLevel, counts.size()> piano {};
    std::array<NativeFamilyLevel, counts.size()> pad {};

    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        piano[i] = renderNativeFamilyLevel (
            sample, "PIANOS", counts[i]);
        pad[i] = renderNativeFamilyLevel (
            sample, "PADS", counts[i]);

        if (! piano[i].ok || ! pad[i].ok)
        {
            std::cerr << "FAMILY_GAIN_RENDER_FAIL voices=" << counts[i]
                      << " piano_ok=" << piano[i].ok
                      << " pad_ok=" << pad[i].ok
                      << " piano_rms=" << piano[i].rms
                      << " pad_rms=" << pad[i].rms << "\n";
            return false;
        }

        const double rmsDeltaDb =
            juce::Decibels::gainToDecibels (
                pad[i].rms / piano[i].rms);
        const float peakDeltaDb =
            juce::Decibels::gainToDecibels (
                pad[i].peak / piano[i].peak);

        std::cout << "FAMILY_GAIN voices=" << counts[i]
                  << " piano_rms=" << piano[i].rms
                  << " pad_rms=" << pad[i].rms
                  << " rms_delta_db=" << rmsDeltaDb
                  << " peak_delta_db=" << peakDeltaDb << "\n";

        if (std::abs (rmsDeltaDb - 4.5) > 0.08
             || std::abs ((double) peakDeltaDb - 4.5) > 0.08)
            return false;
    }

    // Vozes identicas e alinhadas devem crescer linearmente. Se algum limiter
    // ou AGC permanente entrar neste caminho, 4/8/16 vozes deixarao de seguir
    // a referencia de uma voz muito antes de atingir a protecao final do app.
    for (std::size_t i = 1; i < counts.size(); ++i)
    {
        const double expected = (double) counts[i];
        const double pianoRatio = piano[i].rms / piano[0].rms;
        const double padRatio = pad[i].rms / pad[0].rms;

        std::cout << "FAMILY_GAIN_LINEAR voices=" << counts[i]
                  << " piano_ratio=" << pianoRatio
                  << " pad_ratio=" << padRatio << "\n";

        if (std::abs (pianoRatio - expected) > expected * 0.015
             || std::abs (padRatio - expected) > expected * 0.015)
            return false;
    }

    return true;
}

bool waitForLibraryScan (TimbresProcessor& processor,
                         int timeoutSeconds)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds (timeoutSeconds);

    while (! processor.hasScannedLibrary()
           && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    return processor.hasScannedLibrary();
}

bool runSidecarCategoryDiscoveryProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-sidecar-category-probe",
                              {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    if (! writeProbeWav (
            temp.getChildFile ("Sanfona-C3-V127-0000.wav"),
            261.625565f)
         || ! temp.getChildFile ("churchdaw-timbre.json")
                  .replaceWithText (
                      R"json({"schema":1,"name":"Sanfona","category":"ORGAOS"})json"))
        return false;

    const auto entries = TimbresProcessor::scanTimbres (temp);

    return entries.size() == 1
        && entries.getReference (0).label == "Sanfona"
        && entries.getReference (0).category == "ORGAOS";
}

bool runCatalogAndLegacyOrganRestoreProbe()
{
    const auto fail = [] (const char* stage)
    {
        std::cerr << "CATALOG_LEGACY_ORGAN_FAIL stage="
                  << stage << "\n";
        return false;
    };

    juce::StringArray familyNames;

    for (int family = 0;
         family < TimbresProcessor::churchOrganFamilyCount;
         ++family)
    {
        const auto name = juce::String (
            TimbresProcessor::churchOrganFamilyName (
                static_cast<churchorgan::ChurchOrganFamily> (family)));

        if (name.isEmpty() || familyNames.contains (name))
            return fail ("engine-family-name");

        familyNames.add (name);
    }

    if (familyNames.size() != 4)
        return fail ("engine-family-count");

    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-catalog-legacy-organ-probe",
                              {}, false);

    if (temp.createDirectory().failed())
        return fail ("temp-directory");

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };
    const auto sample = temp.getChildFile ("C3.wav");

    if (! writeProbeWav (sample, 261.625565f))
        return fail ("write-wav");

    juce::MemoryBlock sampleBefore;

    if (! sample.loadFileAsData (sampleBefore)
         || sampleBefore.getSize() == 0)
        return fail ("read-original-wav");

    const juce::String legacyLibrary = "Legacy Samples";
    juce::Array<TimbresProcessor::TimbreEntry> catalog;

    const auto addEntry = [&] (const juce::String& label)
    {
        TimbresProcessor::TimbreEntry entry;
        entry.label = label;
        entry.library = legacyLibrary;
        entry.files.add (sample);
        catalog.add (std::move (entry));
    };

    addEntry ("organ");
    addEntry ("orgao");
    addEntry (juce::String::fromUTF8 (
        "\xc3\xb3rg\xc3\xa3o"));
    addEntry ("B3");
    addEntry ("Farf");
    addEntry ("Pipe Layers");
    addEntry ("Studio Grand");

    auto processor = std::make_unique<TimbresProcessor>();
    processor->replaceEntriesForCatalogTest (catalog);
    processor->prepareToPlay (48000.0, 128);

    const auto catalogIsCompleteAndFiltered =
        [&catalog] (const TimbresProcessor& target)
    {
        const auto& cached = target.getEntriesNonBlocking();

        if (cached.size() != catalog.size())
            return false;

        for (int i = 0; i < catalog.size(); ++i)
        {
            const auto& expected = catalog.getReference (i);
            const auto& actual = cached.getReference (i);

            if (actual.label != expected.label
                 || actual.category != expected.category
                 || actual.library != expected.library
                 || actual.files.size() != expected.files.size()
                 || actual.files.size() != 1
                 || actual.files.getReference (0).getFullPathName()
                        != expected.files.getReference (0).getFullPathName())
                return false;

            const bool isPiano = actual.label == "Studio Grand";

            if (isPiano
                    != TimbresProcessor::
                           isSampleTimbreVisibleForNewSelection (actual))
                return false;

            if (! isPiano
                 && ! TimbresProcessor::looksLikeOrgan (
                        actual.label, actual.category))
                return false;
        }

        return true;
    };

    if (! catalogIsCompleteAndFiltered (*processor))
        return fail ("catalog-filter-cache");

    const auto pipeLabel = juce::String ("Pipe Layers");
    processor->requestLayerLoadForCatalogTest (
        0, pipeLabel, legacyLibrary);
    std::atomic<size_t> memoryPeak { 0 };

    if (! dispatchUntilLoaded (*processor, memoryPeak, 10)
         || processor->getLayerTimbre (0) != pipeLabel
         || processor->getLayerLibrary (0) != legacyLibrary
         || ! processor->isLayerOrgan (0)
         || processor->isLayerChurchOrgan (0)
         || ! processor->layerUsesChannelRotary (0))
        return fail ("initial-legacy-load");

    const auto renderLayerRms = [] (TimbresProcessor& target)
    {
        juce::AudioBuffer<float> audio (2, 128);
        juce::MidiBuffer midi;
        double sumSquares = 0.0;
        std::uint64_t measuredSamples = 0;

        for (int block = 0; block < 32; ++block)
        {
            audio.clear();
            midi.clear();

            if (block == 0)
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1, 60, (juce::uint8) 127),
                    0);

            target.processBlock (audio, midi);

            if (block < 4)
                continue;

            for (int channel = 0; channel < audio.getNumChannels();
                 ++channel)
            {
                const auto* samples =
                    audio.getReadPointer (channel);

                for (int i = 0; i < audio.getNumSamples(); ++i)
                {
                    const float value = samples[i];

                    if (! std::isfinite (value))
                        return -1.0;

                    sumSquares +=
                        (double) value * (double) value;
                    ++measuredSamples;
                }
            }
        }

        return measuredSamples > 0
                 ? std::sqrt (
                       sumSquares / (double) measuredSamples)
                 : 0.0;
    };

    const double originalRms = renderLayerRms (*processor);

    if (! std::isfinite (originalRms)
         || originalRms <= 1.0e-8)
        return fail ("initial-audio-rms");

    juce::MemoryBlock savedState;
    processor->getStateInformation (savedState);

    if (savedState.getSize() == 0)
        return fail ("save-state");

    processor.reset();

    auto restarted = std::make_unique<TimbresProcessor>();
    restarted->replaceEntriesForCatalogTest (catalog);
    restarted->prepareToPlay (48000.0, 128);
    restarted->setStateInformation (
        savedState.getData(), (int) savedState.getSize());

    const bool restartDispatched =
        dispatchUntilLoaded (*restarted, memoryPeak, 10);
    const bool restartCatalogOk =
        catalogIsCompleteAndFiltered (*restarted);
    const auto restartTimbre =
        restarted->getLayerTimbre (0);
    const auto restartLibrary =
        restarted->getLayerLibrary (0);
    const bool restartIsOrgan =
        restarted->isLayerOrgan (0);
    const bool restartIsChurchOrgan =
        restarted->isLayerChurchOrgan (0);
    const bool restartUsesChannelRotary =
        restarted->layerUsesChannelRotary (0);

    if (! restartDispatched
         || ! restartCatalogOk
         || restartTimbre != pipeLabel
         || restartLibrary != legacyLibrary
         || ! restartIsOrgan
         || restartIsChurchOrgan
         || ! restartUsesChannelRotary)
    {
        std::cerr
            << "CATALOG_RESTART_STATE dispatched="
            << restartDispatched
            << " catalog=" << restartCatalogOk
            << " timbre=\"" << restartTimbre << "\""
            << " library=\"" << restartLibrary << "\""
            << " organ=" << restartIsOrgan
            << " church=" << restartIsChurchOrgan
            << " channelRotary="
            << restartUsesChannelRotary << "\n";
        return fail ("restart-restore");
    }

    const double restoredRms = renderLayerRms (*restarted);

    if (! std::isfinite (restoredRms)
         || restoredRms <= 1.0e-8)
        return fail ("restored-audio-rms");

    juce::MemoryBlock sampleAfter;

    if (! sample.existsAsFile()
         || ! sample.loadFileAsData (sampleAfter)
         || sampleAfter.getSize() != sampleBefore.getSize()
         || sampleAfter != sampleBefore)
        return fail ("wav-preservation");

    std::cout << "CATALOG_LEGACY_ORGAN_OK"
              << " families=" << familyNames.size()
              << " cached=" << catalog.size()
              << " original_rms=" << originalRms
              << " restored_rms=" << restoredRms
              << "\n";
    return true;
}

bool runPerTimbreRecallProbe()
{
    const auto temp = juce::File::getSpecialLocation (
                          juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "churchdaw-tone-memory-probe", {}, false);

    if (temp.createDirectory().failed())
        return false;

    const juce::ScopeGuard cleanup { [&temp]
    {
        temp.deleteRecursively (false);
    } };

    const auto fileA = temp.getChildFile ("Memory_A_C4.wav");
    const auto fileB = temp.getChildFile ("Memory_B_C4.wav");

    if (! writeProbeWav (fileA, 220.0f)
         || ! writeProbeWav (fileB, 330.0f))
        return false;

    TimbresProcessor::TimbreEntry entryA;
    entryA.label = "__Tone Memory A__";
    entryA.library = "Probe Pack";
    entryA.category = "PIANOS";
    entryA.files.add (fileA);

    auto entryB = entryA;
    entryB.label = "__Tone Memory B__";
    entryB.category = "PADS";
    entryB.files.clear();
    entryB.files.add (fileB);

    auto processor = std::make_unique<TimbresProcessor>();
    processor->prepareToPlay (48000.0, 128);

    if (! waitForLibraryScan (*processor, 10))
        return false;

    std::atomic<size_t> peak { 0 };
    processor->loadTimbreIntoLayer (0, entryA, true);

    if (! dispatchUntilLoaded (*processor, peak, 10)
         || processor->getLayerTimbre (0) != entryA.label
         || std::abs (processor->getLayerNativeFamilyTrim (0) - 1.0f)
                > 0.0001f)
        return false;

    *processor->getLayerAttackParam (0) = 0.137f;
    *processor->getLayerReleaseParam (0) = 3.25f;
    *processor->getLayerLowEqParam (0) = 2.5f;
    *processor->getLayerMidEqParam (0) = -3.5f;
    *processor->getLayerHighEqParam (0) = 1.75f;

    // Pedido interativo inválido faz rollback completo: A e seus cinco
    // controles continuam iguais; defaults de B só entram no commit vencedor.
    auto missingEntry = entryB;
    missingEntry.label = "__Tone Memory Missing__";
    missingEntry.category = "PADS";
    missingEntry.files.clear();
    missingEntry.files.add (
        temp.getChildFile ("arquivo-inexistente.wav"));
    processor->loadTimbreIntoLayer (0, missingEntry, true);

    if (! dispatchUntilLoaded (*processor, peak, 10)
         || processor->getLayerTimbre (0) != entryA.label
         || std::abs (processor->getLayerNativeFamilyTrim (0) - 1.0f)
                > 0.0001f
         || std::abs (
                processor->getLayerAttackParam (0)->get() - 0.137f)
                > 0.0001f
         || std::abs (
                processor->getLayerReleaseParam (0)->get() - 3.25f)
                > 0.0001f
         || std::abs (
                processor->getLayerLowEqParam (0)->get() - 2.5f)
                > 0.0001f
         || std::abs (
                processor->getLayerMidEqParam (0)->get() + 3.5f)
                > 0.0001f
         || std::abs (
                processor->getLayerHighEqParam (0)->get() - 1.75f)
                > 0.0001f)
        return false;

    juce::MemoryBlock stateA;
    processor->getStateInformation (stateA);
    auto stateBXml = juce::AudioProcessor::getXmlFromBinary (
        stateA.getData(), (int) stateA.getSize());

    if (stateBXml == nullptr)
        return false;

    auto* layerZero = stateBXml->getChildByName ("LAYER");

    if (layerZero == nullptr)
        return false;

    layerZero->setAttribute ("timbre", entryB.label);
    layerZero->setAttribute ("lib", entryB.library);
    layerZero->setAttribute ("attack", 0.42);
    layerZero->setAttribute ("release", 6.1);
    layerZero->setAttribute ("eqLow", -5.0);
    layerZero->setAttribute ("eqMid", 4.0);
    layerZero->setAttribute ("eqHigh", -1.5);

    auto* profileB = stateBXml->createNewChildElement ("TONE");
    profileB->setAttribute ("layer", 0);
    profileB->setAttribute ("timbre", entryB.label);
    profileB->setAttribute ("lib", entryB.library);
    profileB->setAttribute ("attack", 0.42);
    profileB->setAttribute ("release", 6.1);
    profileB->setAttribute ("eqLow", -5.0);
    profileB->setAttribute ("eqMid", 4.0);
    profileB->setAttribute ("eqHigh", -1.5);
    profileB->setAttribute ("lcut", 0.0);
    profileB->setAttribute ("hcut", 0.0);
    profileB->setAttribute ("aeq", false);
    profileB->setAttribute (
        "rotMode", ChurchDAWRotary::Speaker::stopMode);
    profileB->setAttribute ("rotAmt", 0.0);

    juce::MemoryBlock stateB;
    juce::AudioProcessor::copyXmlToBinary (*stateBXml, stateB);

    // Restaura B SOBRE A no mesmo processador: este era o caso que podia
    // gravar os parametros de B sob a identidade A.
    processor->setStateInformation (
        stateB.getData(), (int) stateB.getSize());
    processor->loadTimbreIntoLayer (0, entryB, true);

    if (! dispatchUntilLoaded (*processor, peak, 10))
        return false;

    const auto close = [] (float a, float b)
    {
        return std::abs (a - b) < 0.0001f;
    };

    if (processor->getLayerTimbre (0) != entryB.label
         || ! close (
                processor->getLayerNativeFamilyTrim (0),
                TimbresProcessor::nativePadAmbienceTrim)
         || ! close (processor->getLayerAttackParam (0)->get(), 0.42f)
         || ! close (processor->getLayerReleaseParam (0)->get(), 6.1f)
         || ! close (processor->getLayerLowEqParam (0)->get(), -5.0f)
         || ! close (processor->getLayerMidEqParam (0)->get(), 4.0f)
         || ! close (processor->getLayerHighEqParam (0)->get(), -1.5f))
        return false;

    processor->loadTimbreIntoLayer (0, entryA, true);

    if (! dispatchUntilLoaded (*processor, peak, 10))
        return false;

    return processor->getLayerTimbre (0) == entryA.label
        && close (processor->getLayerNativeFamilyTrim (0), 1.0f)
        && close (processor->getLayerAttackParam (0)->get(), 0.137f)
        && close (processor->getLayerReleaseParam (0)->get(), 3.25f)
        && close (processor->getLayerLowEqParam (0)->get(), 2.5f)
        && close (processor->getLayerMidEqParam (0)->get(), -3.5f)
        && close (processor->getLayerHighEqParam (0)->get(), 1.75f);
}

bool runAsyncPluginLoadStateProbe()
{
    ChurchDAWPluginLoading::RequestGeneration generation;
    const auto first = generation.begin();

    if (! generation.isCurrent (first))
        return false;

    const auto second = generation.begin();

    if (generation.isCurrent (first)
         || ! generation.isCurrent (second))
        return false;

    int committedValue = 0;

    if (generation.commitIfCurrent (
            first, [&committedValue]
            {
                committedValue = 1;
            })
         || committedValue != 0)
        return false;

    if (! generation.commitIfCurrent (
            second, [&committedValue]
            {
                committedValue = 2;
            })
         || committedValue != 2)
        return false;

    generation.cancel();

    if (generation.isCurrent (second))
        return false;

    juce::PluginDescription kontaktAu;
    kontaktAu.name = "Kontakt 8";
    kontaktAu.pluginFormatName = "AudioUnit";
    kontaktAu.manufacturerName = "Native Instruments";
    kontaktAu.fileOrIdentifier = "AudioUnit:Kontakt";
    kontaktAu.uniqueId = 101;

    auto kontaktVst = kontaktAu;
    kontaktVst.pluginFormatName = "VST3";
    kontaktVst.fileOrIdentifier = "VST3:Kontakt";
    kontaktVst.uniqueId = 102;

    juce::PluginDescription bigSky;
    bigSky.name = "BigSky";
    bigSky.pluginFormatName = "VST3";
    bigSky.manufacturerName = "Strymon";
    bigSky.fileOrIdentifier = "VST3:BigSky";
    bigSky.uniqueId = 103;

    juce::Array<juce::PluginDescription> choices {
        kontaktAu, bigSky, kontaktVst
    };

    const auto auLabel = ChurchDAWPluginLoading::displayName (
        kontaktAu, choices);
    const auto vstLabel = ChurchDAWPluginLoading::displayName (
        kontaktVst, choices);

    if (! auLabel.contains ("AudioUnit")
         || ! auLabel.contains ("Native Instruments")
         || ! vstLabel.contains ("VST3")
         || ChurchDAWPluginLoading::displayName (
                bigSky, choices) != "BigSky")
        return false;

    const auto selectedIdentifier =
        kontaktAu.createIdentifierString();
    choices.move (0, choices.size() - 1);

    const int restoredIndex =
        ChurchDAWPluginLoading::findChoiceByIdentifier (
            choices, selectedIdentifier);

    return juce::isPositiveAndBelow (
               restoredIndex, choices.size())
        && choices.getReference (restoredIndex)
               .pluginFormatName == "AudioUnit";
}
}

//==============================================================================
// CONTINUIDADE ENTRE CAMADAS DE VELOCITY (reclamacao de cliente, set/2026:
// "da pra ouvir no PA a troca de camadas"). Toca a MESMA nota de velocity 1 a
// 127 com a cadeia neutra, mede o RMS dos primeiros 0,5 s da saida real do
// motor e compara o maior degrau entre velocities vizinhas com o casamento de
// nivel desligado (comportamento antigo) e ligado.
//
// Uso: ChurchDAWTimbreLoadStressTests --velocity-continuity <pasta> <prefixo> [nota ...]
static bool neutraliseLayerZero (TimbresProcessor& processor,
                                 const TimbresProcessor::TimbreEntry& entry)
{
    processor.applyFactoryTimbreShape (0, entry.label, entry.category, false);
    *processor.getLayerAttackParam (0) = 0.001f;
    *processor.getLayerReleaseParam (0) = 0.05f;
    *processor.getLayerLowEqParam (0) = 0.0f;
    *processor.getLayerMidEqParam (0) = 0.0f;
    *processor.getLayerHighEqParam (0) = 0.0f;
    *processor.getLayerRotaryModeParam (0) = ChurchDAWRotary::Speaker::stopMode;
    *processor.getLayerRotaryAmountParam (0) = 0.0f;
    *processor.getLayerReverbSendParam (0) = 0.0f;

    auto* volume = dynamic_cast<juce::AudioParameterFloat*> (processor.getLayerVolParam (0));
    auto* compressor = dynamic_cast<juce::AudioParameterFloat*> (processor.getLayerCompParam (0));
    if (volume == nullptr || compressor == nullptr)
        return false;
    *volume = 1.0f;
    *compressor = 0.0f;

    for (auto* parameter : processor.getParameters())
        if (auto* identified = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
        {
            if (identified->paramID == "lcut0" || identified->paramID == "hcut0" || identified->paramID == "dly0")
            {
                if (auto* value = dynamic_cast<juce::AudioParameterFloat*> (parameter)) *value = 0.0f;
            }
            else if (identified->paramID == "aeq0")
            {
                if (auto* value = dynamic_cast<juce::AudioParameterBool*> (parameter)) *value = false;
            }
        }
    return true;
}

static std::array<float, 128> sweepVelocities (TimbresProcessor& processor, int midiNote,
                                               double sampleRate, int blockSize)
{
    std::array<float, 128> levelDb {};
    const int measureSamples = (int) (0.5 * sampleRate) / blockSize * blockSize;
    const int tailSamples = (int) (0.4 * sampleRate) / blockSize * blockSize;
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    for (int velocity = 1; velocity <= 127; ++velocity)
    {
        double energy = 0.0;
        juce::int64 counted = 0;

        for (int rendered = 0; rendered < measureSamples + tailSamples; rendered += blockSize)
        {
            audio.clear();
            midi.clear();
            if (rendered == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, (juce::uint8) velocity), 0);
            if (rendered == measureSamples)
                midi.addEvent (juce::MidiMessage::noteOff (1, midiNote), 0);
            processor.processBlock (audio, midi);

            if (rendered < measureSamples)
                for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                {
                    const float* samples = audio.getReadPointer (channel);
                    for (int i = 0; i < blockSize; ++i)
                        energy += (double) samples[i] * samples[i];
                    counted += blockSize;
                }
        }

        levelDb[(size_t) velocity] = (float) (10.0 * std::log10 (
            std::max (1.0e-14, energy / (double) std::max<juce::int64> (1, counted))));
    }
    return levelDb;
}

static int runVelocityContinuityProbe (const juce::File& directory, const juce::String& prefix,
                                       juce::Array<int> notes)
{
    const auto files = collectSamples (directory, prefix);
    if (files.size() < 2)
    {
        std::cerr << "Biblioteca/prefixo invalido: " << files.size() << " samples\n";
        return 3;
    }

    TimbresProcessor::TimbreEntry entry;
    entry.label = prefix;
    entry.library = "Velocity Probe";
    entry.category = "PIANOS";
    entry.files = files;

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    TimbresProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    processor.loadTimbreIntoLayer (0, entry, true);
    std::atomic<size_t> peak { 0 };

    if (! dispatchUntilLoaded (processor, peak, 180) || processor.getLayerTimbre (0) != entry.label)
    {
        std::cerr << "Timbre nao carregou\n";
        return 4;
    }
    if (! neutraliseLayerZero (processor, entry))
        return 5;

    juce::AudioBuffer<float> warm (2, blockSize);
    juce::MidiBuffer none;
    for (int block = 0; block < 32; ++block) { warm.clear(); processor.processBlock (warm, none); }

    if (notes.isEmpty())
        notes = { 48, 60, 72 };

    bool improved = true;
    for (const int note : notes)
    {
        float worst[2] {}, median[2] {};
        int audible[2] {};
        for (const bool matched : { false, true })
        {
            churchDAWLayerLevelMatchingForTests.store (matched);
            const auto level = sweepVelocities (processor, note, sampleRate, blockSize);
            std::vector<float> steps;
            for (int v = 2; v <= 127; ++v)
                steps.push_back (level[(size_t) v] - level[(size_t) v - 1]);
            std::vector<float> sorted (steps);
            std::sort (sorted.begin(), sorted.end());
            const int index = matched ? 1 : 0;
            worst[index] = sorted.back();
            median[index] = sorted[sorted.size() / 2];
            audible[index] = (int) std::count_if (steps.begin(), steps.end(),
                                                  [] (float s) { return std::abs (s) > 1.0f; });
            std::cout << "VELOCITY_SWEEP note=" << note << (matched ? " casado" : " antigo ")
                      << " v1=" << level[1] << "dB v64=" << level[64] << "dB v127=" << level[127]
                      << "dB faixa=" << (level[127] - level[1]) << "dB maior_degrau=" << worst[index]
                      << "dB degrau_mediano=" << median[index] << "dB degraus>1dB=" << audible[index];
            std::cout << " em:";
            for (int v = 2; v <= 127; ++v)
            {
                const float s = level[(size_t) v] - level[(size_t) v - 1];
                if (std::abs (s) > 1.0f)
                    std::cout << ' ' << (v - 1) << "->" << v << '(' << juce::String (s, 1) << ')';
            }
            std::cout << '\n';
        }
        // Timbre de camada unica sai identico (nada a casar): "nao piorou" basta.
        improved = improved && worst[1] <= worst[0] + 0.01f && audible[1] <= audible[0];
    }

    churchDAWLayerLevelMatchingForTests.store (true);
    std::cout << (improved ? "VELOCITY_CONTINUITY_OK\n" : "VELOCITY_CONTINUITY_REGRESSION\n");
    return improved ? 0 : 1;
}

// Todos os timbres que o app enxerga (mesma varredura das Bibliotecas): para
// cada um, quantas trocas de camada audiveis (>1 dB, ignorando a base v1-4 da
// curva) existem com o casamento desligado e ligado.
static int runVelocityContinuityAllProbe()
{
    const auto entries = TimbresProcessor::scanAllLibraries (nullptr, false);
    if (entries.isEmpty())
    {
        std::cerr << "Nenhum timbre encontrado nas bibliotecas\n";
        return 3;
    }

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    bool ok = true;

    for (const auto& entry : entries)
    {
        TimbresProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.loadTimbreIntoLayer (0, entry, true);
        std::atomic<size_t> peak { 0 };

        if (! dispatchUntilLoaded (processor, peak, 180) || processor.getLayerTimbre (0) != entry.label
             || ! neutraliseLayerZero (processor, entry))
        {
            std::cout << "TIMBRE \"" << entry.label << "\" nao carregou\n";
            continue;
        }

        juce::AudioBuffer<float> warm (2, blockSize);
        juce::MidiBuffer none;
        for (int block = 0; block < 32; ++block) { warm.clear(); processor.processBlock (warm, none); }

        int jumps[2] {};
        float worst[2] {};
        int measuredNotes = 0;
        for (const int note : { 48, 60, 72 })
        {
            bool audibleNote = false;
            for (const bool matched : { false, true })
            {
                churchDAWLayerLevelMatchingForTests.store (matched);
                const auto level = sweepVelocities (processor, note, sampleRate, blockSize);
                if (level[127] < -90.0f) continue;          // nota fora da regiao do timbre
                audibleNote = true;
                const int index = matched ? 1 : 0;
                for (int v = 6; v <= 127; ++v)               // v1-5: base da curva, igual nos dois
                {
                    const float step = level[(size_t) v] - level[(size_t) v - 1];
                    worst[index] = std::max (worst[index], step);
                    if (step > 1.0f) ++jumps[index];
                }
            }
            if (audibleNote) ++measuredNotes;
        }
        churchDAWLayerLevelMatchingForTests.store (true);

        const bool noWorse = worst[1] <= worst[0] + 0.01f && jumps[1] <= jumps[0];
        ok = ok && noWorse;
        std::cout << "TIMBRE \"" << entry.label << "\" [" << entry.category << "] notas=" << measuredNotes
                  << " trocas_audiveis antes=" << jumps[0] << " depois=" << jumps[1]
                  << " maior_degrau antes=" << juce::String (worst[0], 2) << "dB depois="
                  << juce::String (worst[1], 2) << "dB" << (noWorse ? "" : "  <-- PIOROU") << '\n';
    }

    std::cout << (ok ? "VELOCITY_CONTINUITY_ALL_OK\n" : "VELOCITY_CONTINUITY_ALL_REGRESSION\n");
    return ok ? 0 : 1;
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;

    if (argc == 2 && juce::String::fromUTF8 (argv[1]) == "--velocity-continuity-all")
        return runVelocityContinuityAllProbe();

    if (argc >= 4 && juce::String::fromUTF8 (argv[1]) == "--velocity-continuity")
    {
        juce::Array<int> notes;
        for (int i = 4; i < argc; ++i)
            notes.add (juce::String (argv[i]).getIntValue());
        return runVelocityContinuityProbe (juce::File (juce::String::fromUTF8 (argv[2])),
                                           juce::String::fromUTF8 (argv[3]).trim(), notes);
    }

    if (argc == 2
         && juce::String::fromUTF8 (argv[1]) == "--anti-alias-only")
    {
        NativeAntiAliasMetrics metrics;
        const bool passed = runNativeAntiAliasProbe (metrics);

        std::cout << "NATIVE_ANTIALIAS"
                  << " alias_before_db=" << metrics.aliasBeforeDb
                  << " alias_after_db=" << metrics.aliasAfterDb
                  << " reduction_db=" << metrics.aliasReductionDb
                  << " passband_change_db=" << metrics.passbandChangeDb
                  << " stream_misses=" << metrics.streamMisses << '\n';

        if (! passed)
        {
            std::cerr << "Resampler nativo nao suprimiu alias antes da decimacao ou perdeu passband\n";
            return 1;
        }

        return 0;
    }

    if (argc == 3
         && juce::String::fromUTF8 (argv[1]) == "--stream-window-only")
    {
        const juce::File sample (juce::String::fromUTF8 (argv[2]));
        juce::Array<juce::File> files;

        if (sample.existsAsFile())
        {
            files.add (sample);
            files.add (sample);
        }

        if (! runStreamerGenerationStress (files))
        {
            std::cerr << "Primeira janela/continuidade entre callbacks falhou\n";
            return 1;
        }

        std::cout << "STREAM_WINDOW_CONTINUITY_OK"
                  << " first_span=12000 preserved_between_callbacks=true\n";
        return 0;
    }

    if (argc == 2
         && juce::String::fromUTF8 (argv[1]) == "--controls-only")
    {
        if (! runAsyncPluginLoadStateProbe())
        {
            std::cerr << "Geracao assincrona ou identidade/rotulo de plugin falhou\n";
            return 1;
        }

        if (! runChannelControlStateProbe())
        {
            std::cerr << "Controles/cena vazia falharam no estado salvo, migrado ou zerado\n";
            return 1;
        }

        if (! runNativeKeyDepthProbe())
        {
            std::cerr << "Profundidade de note-on repetido cortou a tecla antes do ultimo note-off\n";
            return 1;
        }

        if (! runNativeSampleAccurateMidiProbe())
        {
            std::cerr << "Sampler nativo antecipou NOTE ON/OFF, CC64 ou ALL OFF dentro do bloco\n";
            return 1;
        }

        std::cout << "NATIVE_SAMPLE_ACCURATE_MIDI_OK"
                  << " note_on=40 note_off=170"
                  << " pedal_off=200 all_off=220\n";

        if (! runParallelNativeRenderParityProbe())
        {
            std::cerr << "Render paralelo por camada divergiu do serial ou nao foi ativado\n";
            return 1;
        }

        std::cout << "PARALLEL_NATIVE_RENDER_BIT_EXACT_OK layers=2 voices=32\n";

        NativeAntiAliasMetrics antiAliasMetrics;

        if (! runNativeAntiAliasProbe (antiAliasMetrics))
        {
            std::cerr << "Resampler nativo nao suprimiu alias antes da decimacao ou perdeu passband\n";
            return 1;
        }

        std::cout << "NATIVE_ANTIALIAS_OK"
                  << " alias_before_db=" << antiAliasMetrics.aliasBeforeDb
                  << " alias_after_db=" << antiAliasMetrics.aliasAfterDb
                  << " reduction_db=" << antiAliasMetrics.aliasReductionDb
                  << " passband_change_db=" << antiAliasMetrics.passbandChangeDb
                  << " stream_misses=" << antiAliasMetrics.streamMisses << '\n';

        if (! runIgnoredLoopMarkerPlaybackProbe())
        {
            std::cerr << "Loop legado alterou playback, RAM/streaming ou fim natural do sample\n";
            return 1;
        }

        if (! runExplicitSidecarLoopPlaybackProbe())
        {
            std::cerr << "Loop explicito do sidecar cortou, estalou ou leu a cauda fisica\n";
            return 1;
        }

        if (! runEvenRootBoundaryProbe())
        {
            std::cerr << "Fronteira entre roots pares escolheu a amostra errada\n";
            return 1;
        }

        if (! runIdleDelayTailGainProbe())
        {
            std::cerr << "Cauda ociosa do delay mudou dose, feedback ou foi cortada\n";
            return 1;
        }

        std::cout << "IDLE_DELAY_TAIL_OK amount_ratio=5 feedback=0.32\n";

        if (! runPerTimbreRecallProbe())
        {
            std::cerr << "Memoria A-B-A por timbre falhou na troca/restauracao real\n";
            return 1;
        }

        if (! runSidecarCategoryDiscoveryProbe())
        {
            std::cerr << "Categoria declarada no sidecar nao entrou no seletor\n";
            return 1;
        }

        if (! runCatalogAndLegacyOrganRestoreProbe())
        {
            std::cerr << "Catalogo Church Organ ou restore legado de Pipe Layers falhou\n";
            return 1;
        }

        if (! runChurchOrganLayerIntegrationProbe())
        {
            std::cerr << "Integracao Church Organ falhou em blocos variaveis, estado, memoria ou trim\n";
            return 1;
        }

        if (! runNativeSourceMetadataGainProbe())
        {
            std::cerr << "Ganho global da fonte NSMP foi ignorado, limitado pelo fader ou aplicado duas vezes\n";
            return 1;
        }

        if (! runNativeFamilyGainMatrix())
        {
            std::cerr << "Trim PAD/AMBIENCE falhou em 1/4/8/16 vozes ou entrou limitacao permanente\n";
            return 1;
        }

        std::cout << "CHANNEL_CONTROL_STATE_OK\n";
        return 0;
    }

    if (argc < 3)
    {
        std::cout
            << "Uso: ChurchDAWTimbreLoadStressTests <pasta> <prefixo-do-timbre>\n"
            << "A sonda requer uma biblioteca real e nao roda automaticamente no CTest.\n";
        return 2;
    }

    const juce::File directory (
        juce::String::fromUTF8 (argv[1]));
    const auto prefix = juce::String::fromUTF8 (argv[2]).trim();
    auto files = collectSamples (directory, prefix);

    if (! directory.isDirectory() || prefix.isEmpty() || files.size() < 16)
    {
        std::cerr << "Biblioteca/prefixo invalido: encontrados "
                  << files.size() << " samples\n";
        return 3;
    }

    TimbresProcessor::TimbreEntry entry;
    entry.label = prefix;
    entry.library = "Stress";
    entry.category = "PIANOS";
    entry.files = files;

    auto finalEntry = entry;
    finalEntry.label = prefix + " Final";

    std::atomic<size_t> peak { privateBytes() };
    const auto startedBytes = peak.load();
    TimbresProcessor processor;
    processor.prepareToPlay (48000.0, 256);
    recordPeak (peak);

    // Primeira carga completa: cria o piano que precisa continuar tocando
    // enquanto o usuario escolhe o proximo.
    processor.loadTimbreIntoLayer (0, entry);

    if (! dispatchUntilLoaded (processor, peak, 120)
         || processor.getLayerTimbre (0) != prefix)
    {
        std::cerr << "A primeira carga nao terminou corretamente\n";
        return 4;
    }

    const auto loadedBytes = privateBytes();

    // Reproduz uma rajada adversarial entre todos os canais. Os canais 2..5 sao
    // limpos logo depois, portanto seus jobs devem cancelar ainda na fila/gate
    // sem criar outro piano pesado. No canal principal, cada novo pedido cancela
    // a geracao anterior sem remover o piano que ja esta tocando.
    constexpr int rapidRequestCount = 100;
    const auto rapidStarted = std::chrono::steady_clock::now();

    for (int attempt = 0; attempt < rapidRequestCount; ++attempt)
    {
        const int layer = attempt % TimbresProcessor::numLayers;
        processor.loadTimbreIntoLayer (layer, entry);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2);

        if (layer != 0)
            processor.clearLayer (layer);

        recordPeak (peak);
    }

    // O ultimo pedido tem um rotulo distinto para provar que ele realmente foi
    // instalado; conferir apenas o rotulo inicial permitiria um falso positivo.
    // Antes da conclusao, o piano anterior precisa continuar ativo.
    processor.loadTimbreIntoLayer (0, finalEntry);

    if (processor.getLayerTimbre (0) != prefix)
    {
        std::cerr << "O pedido novo removeu o timbre anterior antes da conclusao\n";
        return 5;
    }

    if (! dispatchUntilLoaded (processor, peak, 120)
         || processor.getLayerTimbre (0) != finalEntry.label)
    {
        std::cerr << "A carga final/cancelamentos nao terminaram corretamente\n";
        return 5;
    }

    for (int layer = 1; layer < TimbresProcessor::numLayers; ++layer)
    {
        if (processor.getLayerTimbre (layer).isNotEmpty())
        {
            std::cerr << "Um pedido cancelado instalou timbre em outro canal\n";
            return 8;
        }
    }

    const auto rapidMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - rapidStarted).count();

    // Deixa os timers de purge liberarem qualquer conjunto aposentado e confirma
    // que nenhum loadingCount ficou preso por worker cancelado.
    const auto purgeDeadline = std::chrono::steady_clock::now()
                             + std::chrono::seconds (3);

    while (std::chrono::steady_clock::now() < purgeDeadline)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        recordPeak (peak);
    }

    const auto finalBytes = privateBytes();
    constexpr size_t hardCeiling = (size_t) 1536 * 1024 * 1024;

    if (! runStreamerGenerationStress (files))
    {
        std::cerr << "A reciclagem de voz publicou fonte/geracao incorreta\n";
        return 9;
    }

    // Shutdown adversarial: cinco canais entram quase juntos e o processador e
    // destruido sem esperar a fila. O abort global + gate devem liberar todos,
    // sem std::terminate, deadlock ou callback tocando objeto morto.
    const auto shutdownStarted = std::chrono::steady_clock::now();

    {
        TimbresProcessor shutdownProbe;
        shutdownProbe.prepareToPlay (48000.0, 256);

        for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
            shutdownProbe.loadTimbreIntoLayer (layer, entry);

        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        recordPeak (peak);
    }

    const auto shutdownMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - shutdownStarted).count();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    recordPeak (peak);
    const auto peakBytes = peak.load();

    std::cout << "samples=" << files.size()
              << " rapidRequests=" << rapidRequestCount
              << " rapidMs=" << rapidMs
              << " startMiB=" << startedBytes / (1024 * 1024)
              << " loadedMiB=" << loadedBytes / (1024 * 1024)
              << " peakMiB=" << peakBytes / (1024 * 1024)
              << " finalMiB=" << finalBytes / (1024 * 1024)
              << " shutdownMs=" << shutdownMs
              << " streamRace=pass"
              << "\n";

    if (peakBytes == 0 || peakBytes > hardCeiling)
    {
        std::cerr << "Pico de memoria excedeu o teto de 1536 MiB\n";
        return 6;
    }

    if (shutdownMs > 10000)
    {
        std::cerr << "Shutdown dos workers excedeu 10 segundos\n";
        return 7;
    }

    std::cout << "PASS: streaming, cancelamento por canal e memoria limitada\n";
    return 0;
}
