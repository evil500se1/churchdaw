#include "../../Source/TimbresProcessor.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#elif JUCE_MAC
 #include <mach/mach.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

static_assert (CHURCHDAW_STREAM_SAMPLES == 1,
               "Builds do produto nao podem voltar a decodificar pianos inteiros");

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
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;

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
