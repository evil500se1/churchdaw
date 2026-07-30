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
        voice.restart ((i & 1) == 0 ? sourceA : sourceB);
        voice.request (testStart);
        streamer.wake();

        if ((i & 3) == 0)
            juce::Thread::yield();
    }

    voice.restart (sourceB);
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

            voice.stop();
            return true;
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
    auto* sourceChannelTranspose = findIntParameter (*source, "transp0");
    auto* sourceGlobalTranspose = dynamic_cast<juce::AudioParameterInt*> (
        source->getGlobalTranspParam());

    if (sourceGlobalTranspose == nullptr || sourceChannelTranspose == nullptr)
        return false;

    *sourceChannelTranspose = -4;
    *sourceGlobalTranspose = 5;

    juce::MemoryBlock state;
    source->getStateInformation (state);

    if (state.isEmpty())
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
        && findIntParameter (*restored, "transp0")->get() == -4;

    if (! currentStateOk)
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
         || roundTripXml->getIntAttribute ("toneSchema", 0) != 3)
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
                    "eqHigh"), 1.25f))
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
    oldestXml.setAttribute ("loop", false);
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
         || ! close (oldestRestored->getLayerHighEqParam (1)->get(), 0.0f))
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
        constexpr int layerCount =
            TimbresProcessor::numLayers;
        static_assert (
            layerCount == 5,
            "O probe cobre exatamente as cinco camadas do ChurchDAW");

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

        std::array<const juce::XmlElement*,
                   layerCount> stateLayers {};
        int stateLayerCount = 0;

        forEachXmlChildElementWithTagName (
            *multilayerXml, candidate, "LAYER")
        {
            ++stateLayerCount;
            const int stateLayer =
                candidate->getIntAttribute (
                    "index", -1);

            if (! juce::isPositiveAndBelow (
                    stateLayer, layerCount)
                 || stateLayers[stateLayer]
                        != nullptr)
                return false;

            stateLayers[stateLayer] = candidate;
        }

        if (stateLayerCount != layerCount)
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
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;

    if (argc == 2
         && juce::String::fromUTF8 (argv[1]) == "--controls-only")
    {
        if (! runChannelControlStateProbe())
        {
            std::cerr << "Attack/Release/EQ/Rotary falharam no estado salvo ou migrado\n";
            return 1;
        }

        if (! runPerTimbreRecallProbe())
        {
            std::cerr << "Memoria A-B-A por timbre falhou na troca/restauracao real\n";
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
