#include <chrono>
#include <atomic>
#include <cstring>
#include <iostream>

#include <juce_gui_extra/juce_gui_extra.h>

#include "TimbreCatalog.h"
#include "TimbrePickerInteraction.h"

namespace
{
int failures = 0;

void check (bool condition, const char* name)
{
    if (condition)
        std::cout << "  ok   " << name << '\n';
    else
    {
        ++failures;
        std::cout << "  FALHOU " << name << '\n';
    }
}

class CountingInputStream final : public juce::InputStream
{
public:
    explicit CountingInputStream (juce::int64 availableBytesToUse)
        : availableBytes (availableBytesToUse)
    {
    }

    juce::int64 getTotalLength() override { return availableBytes; }
    juce::int64 getPosition() override { return position; }

    bool setPosition (juce::int64 next) override
    {
        if (next < 0 || next > availableBytes)
            return false;

        position = next;
        return true;
    }

    int read (void* destination, int maximumBytesToRead) override
    {
        ++readCalls;

        if (position >= availableBytes)
        {
            readPastAvailableBytes = true;
            return 0;
        }

        const auto remaining = availableBytes - position;
        const int count = (int) juce::jmin (
            remaining, (juce::int64) maximumBytesToRead);
        std::memset (destination, 0x5a, (size_t) count);
        position += count;
        return count;
    }

    bool isExhausted() override
    {
        // Simula uma conexao keep-alive que nao anuncia EOF ao consumidor.
        return false;
    }

    juce::int64 availableBytes = 0;
    juce::int64 position = 0;
    int readCalls = 0;
    bool readPastAvailableBytes = false;
};

void runExactLengthTests()
{
    {
        CountingInputStream input (300000);
        juce::MemoryOutputStream output;
        juce::int64 total = 0;
        const auto result = ChurchDAWServices::copyExactDownloadBytes (
            input, output, 0, 300000, {}, {}, total);

        check (result.wasOk(), "copia exata conclui sem depender de EOF");
        check (total == 300000, "copia exata contabiliza tamanho publicado");
        check (! input.readPastAvailableBytes,
               "copia exata nao pede leitura extra em conexao keep-alive");
        check ((juce::int64) output.getDataSize() == 300000,
               "copia exata persiste todos os bytes");
    }

    {
        CountingInputStream input (1000);
        juce::MemoryOutputStream output;
        juce::int64 total = 0;
        const auto result = ChurchDAWServices::copyExactDownloadBytes (
            input, output, 0, 2000, {}, {}, total);

        check (result.failed(), "conexao truncada falha em vez de marcar pronto");
        check (total == 1000, "conexao truncada preserva tamanho retomavel");
    }

    {
        CountingInputStream input (600);
        juce::MemoryOutputStream output;
        juce::int64 total = 0;
        const auto result = ChurchDAWServices::copyExactDownloadBytes (
            input, output, 400, 1000, {}, {}, total);

        check (result.wasOk(), "retomada copia apenas bytes restantes");
        check (total == 1000 && output.getDataSize() == 600,
               "retomada termina no tamanho total correto");
    }

    {
        CountingInputStream input (400000);
        juce::MemoryOutputStream output;
        juce::int64 total = 0;
        bool cancel = false;
        const auto result = ChurchDAWServices::copyExactDownloadBytes (
            input,
            output,
            0,
            400000,
            [&cancel] { return cancel; },
            [&cancel] (juce::int64) { cancel = true; },
            total);

        check (result.failed(), "cancelamento interrompe copia longa");
        check (total > 0 && total < 400000,
               "cancelamento deixa parcial util para retomar");
    }
}

int runNetworkEofProbe()
{
    constexpr juce::int64 expectedBytes = 2356478;
    const juce::String url =
        "https://churchdaw.com/app/android/timbres/white-ep-ab-v1.zip";
    int status = 0;
    juce::StringPairArray responseHeaders;
    auto options = juce::URL::InputStreamOptions (
                       juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (15000)
                       .withNumRedirectsToFollow (0)
                       .withStatusCode (&status)
                       .withResponseHeaders (&responseHeaders);
    auto input = juce::URL (url).createInputStream (options);

    if (input == nullptr)
    {
        std::cerr << "NETWORK_EOF_PROBE_CONNECT_FAIL status=" << status << '\n';
        return 2;
    }

    std::cout << "NETWORK_EOF_PROBE_CONNECTED status=" << status
              << " content_length=" << responseHeaders["Content-Length"]
              << std::endl;

    juce::HeapBlock<char> buffer (64 * 1024);
    juce::int64 done = 0;
    int reads = 0;
    const auto started = std::chrono::steady_clock::now();

    for (;;)
    {
        const int received = input->read (buffer.getData(), 64 * 1024);
        ++reads;

        if (received <= 0)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            std::cout << "NETWORK_EOF_PROBE_EOF bytes=" << done
                      << " reads=" << reads << " elapsed_ms=" << elapsed
                      << std::endl;
            return done == expectedBytes ? 0 : 3;
        }

        done += received;

        if (done == expectedBytes)
            std::cout << "NETWORK_EOF_PROBE_EXPECTED_REACHED reads=" << reads
                      << " requesting_one_more_read=1" << std::endl;

        if (done > expectedBytes)
        {
            std::cerr << "NETWORK_EOF_PROBE_OVERRUN bytes=" << done << '\n';
            return 4;
        }
    }
}

int runLiveInstallProbe()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    ChurchDAWTimbreCatalog::Instrument instrument;
    instrument.id = "white-ep-ab-probe";
    instrument.name = "White EP AB";
    instrument.rawLabel = "White EP AB";
    instrument.category = "PIANOS";
    instrument.libraryId = "church-live";
    instrument.installRelative = "OpenNord User/White EP AB Probe";
    instrument.fileCount = 9;
    instrument.downloadBytes = 2356478;
    instrument.downloadUrl =
        "https://churchdaw.com/app/android/timbres/white-ep-ab-v1.zip";
    instrument.sha256 =
        "dac2bd36be99980ae4831188b01707bda25fe93e882ae52d3450c33b83067128";
    instrument.included = true;
    instrument.ready = true;

    std::atomic<bool> done { false };
    juce::Result finalResult = juce::Result::fail ("callback ausente");
    float lastProgress = -1.0f;
    ChurchDAWTimbreCatalog::Installer installer (
        instrument,
        [&lastProgress] (float value, const juce::String& text)
        {
            lastProgress = value;
            std::cout << "LIVE_INSTALL_PROGRESS value=" << value
                      << " text=\"" << text << "\"" << std::endl;
        },
        [&done, &finalResult] (juce::Result result)
        {
            finalResult = std::move (result);
            done.store (true);
        });

    installer.begin();
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds (60);

    while (! done.load() && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

    if (! done.load())
    {
        installer.signalThreadShouldExit();
        std::cerr << "LIVE_INSTALL_TIMEOUT last_progress=" << lastProgress << '\n';
        return 5;
    }

    if (finalResult.failed())
    {
        std::cerr << "LIVE_INSTALL_FAIL " << finalResult.getErrorMessage() << '\n';
        return 6;
    }

    int wavCount = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (
             instrument.installFolder(), true, "*.wav", juce::File::findFiles))
    {
        juce::ignoreUnused (entry);
        ++wavCount;
    }

    std::cout << "LIVE_INSTALL_OK wav_count=" << wavCount
              << " folder=\"" << instrument.installFolder().getFullPathName()
              << "\"" << std::endl;
    return wavCount == 9 ? 0 : 7;
}

int runLiveMultipartResumeProbe()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto catalog = ChurchDAWTimbreCatalog::fetchRemoteCatalog();
    ChurchDAWTimbreCatalog::Instrument instrument;

    for (const auto& candidate : catalog.instruments)
        if (candidate.id == "string-fabio")
        {
            instrument = candidate;
            break;
        }

    if (! instrument.canDownload() || instrument.parts.size() != 2)
    {
        std::cerr << "MULTIPART_CATALOG_INVALID diagnostic=\""
                  << catalog.diagnostic << "\"\n";
        return 20;
    }

    auto runInstaller = [&instrument] (bool cancelEarly,
                                       bool& sawResume,
                                       float& lastProgress,
                                       juce::Result& finalResult,
                                       int timeoutSeconds) -> bool
    {
        std::atomic<bool> done { false };
        std::atomic<bool> cancelRequested { false };
        std::unique_ptr<ChurchDAWTimbreCatalog::Installer> installer;
        installer = std::make_unique<ChurchDAWTimbreCatalog::Installer> (
            instrument,
            [&sawResume, &lastProgress] (float value, const juce::String& text)
            {
                lastProgress = value;

                if (text.containsIgnoreCase ("Retomando"))
                    sawResume = true;

                std::cout << "MULTIPART_PROGRESS value=" << value
                          << " text=\"" << text << "\"" << std::endl;
            },
            [&done, &finalResult] (juce::Result result)
            {
                finalResult = std::move (result);
                done.store (true);
            });

        installer->begin();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds (timeoutSeconds);

        while (! done.load() && std::chrono::steady_clock::now() < deadline)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

            if (cancelEarly && ! cancelRequested.load()
                 && lastProgress >= 0.055f && lastProgress < 0.70f)
            {
                cancelRequested.store (true);
                installer->cancel();
                std::cout << "MULTIPART_CANCEL_REQUESTED progress="
                          << lastProgress << std::endl;
            }
        }

        if (! done.load())
        {
            installer->cancel();
            std::cerr << "MULTIPART_TIMEOUT progress=" << lastProgress << '\n';
            return false;
        }

        return true;
    };

    bool firstSawResume = false;
    float firstProgress = -1.0f;
    juce::Result firstResult = juce::Result::ok();

    if (! runInstaller (true, firstSawResume, firstProgress,
                        firstResult, 90))
        return 21;

    const auto tempRoot = AppPaths::managedDataFolder()
                              .getChildFile ("Downloads")
                              .getChildFile ("timbres");
    const auto partial = tempRoot.getChildFile (
        instrument.id + "-p01.download.part");
    const auto partialBytes = partial.existsAsFile() ? partial.getSize() : 0;

    if (firstResult.wasOk() || partialBytes <= 0
         || partialBytes >= instrument.parts[0].downloadBytes)
    {
        std::cerr << "MULTIPART_CANCEL_DID_NOT_PRESERVE result=\""
                  << firstResult.getErrorMessage() << "\" bytes="
                  << partialBytes << '\n';
        return 22;
    }

    std::cout << "MULTIPART_PARTIAL_PRESERVED bytes=" << partialBytes
              << std::endl;

    bool sawResume = false;
    float resumedProgress = -1.0f;
    juce::Result resumedResult = juce::Result::fail ("callback ausente");

    if (! runInstaller (false, sawResume, resumedProgress,
                        resumedResult, 1200))
        return 23;

    if (resumedResult.failed())
    {
        std::cerr << "MULTIPART_RESUME_FAILED \""
                  << resumedResult.getErrorMessage() << "\"\n";
        return 24;
    }

    int audioFiles = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (
             instrument.installFolder(), false,
             "*.flac;*.wav;*.aif;*.aiff", juce::File::findFiles))
    {
        juce::ignoreUnused (entry);
        ++audioFiles;
    }

    const bool tempClean = ! partial.existsAsFile()
        && ! tempRoot.getChildFile (
            instrument.id + "-p02.download.part").existsAsFile()
        && ! tempRoot.getChildFile (
            instrument.id + "-download.zip").existsAsFile();

    std::cout << "MULTIPART_RESUME_OK resumed=" << (sawResume ? 1 : 0)
              << " audio_files=" << audioFiles
              << " marker=" << (instrument.installedMarker().existsAsFile() ? 1 : 0)
              << " temp_clean=" << (tempClean ? 1 : 0) << std::endl;

    return sawResume && audioFiles >= instrument.fileCount
            && instrument.installedMarker().existsAsFile() && tempClean
        ? 0 : 25;
}

int runLiveLibraryResumeProbe()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto startup = ChurchDAWServices::checkRemoteManifest (30000);
    ChurchDAWServices::LibraryPack pack;

    for (const auto& candidate : startup.manifest.libraries)
        if (ChurchDAWServices::isPadContinuoPackId (candidate.id))
        {
            pack = candidate;
            break;
        }

    if (! pack.canDownload() || pack.parts.size() != 7)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
        const auto fetched = ChurchDAWServices::fetchDistributionJson (
            ChurchDAWServices::manifestUrl, {}, 2 * 1024 * 1024,
            30000, deadline);
        const auto normalized = ChurchDAWServices::normalizeManifestText (
            fetched.body);
        const auto actualSha = juce::SHA256 (
            normalized.toRawUTF8(),
            (size_t) normalized.getNumBytesAsUTF8()).toHexString().toLowerCase();
        std::cerr << "LIBRARY_CATALOG_INVALID diagnostic=\""
                  << startup.diagnostic << "\" update=\""
                  << startup.updateDiagnostic << "\" expected_sha=\""
                  << startup.updatePolicy.catalogSha256 << "\" actual_sha=\""
                  << actualSha << "\" http=" << fetched.statusCode
                  << " bytes=" << fetched.body.getNumBytesAsUTF8()
                  << " transport=\"" << fetched.transportError << "\"\n";
        return 30;
    }

    auto runInstaller = [&pack] (bool cancelEarly,
                                 float& lastProgress,
                                 juce::Result& finalResult,
                                 int timeoutSeconds) -> bool
    {
        std::atomic<bool> done { false };
        bool cancelRequested = false;
        auto installer = std::make_unique<ChurchDAWServices::LibraryInstaller> (
            pack,
            [&lastProgress] (float value, const juce::String& text)
            {
                lastProgress = value;
                std::cout << "LIBRARY_PROGRESS value=" << value
                          << " text=\"" << text << "\"" << std::endl;
            },
            [&done, &finalResult] (juce::Result result)
            {
                finalResult = std::move (result);
                done.store (true);
            });

        installer->begin();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds (timeoutSeconds);

        while (! done.load() && std::chrono::steady_clock::now() < deadline)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

            if (cancelEarly && ! cancelRequested
                 && lastProgress >= 0.025f && lastProgress < 0.80f)
            {
                cancelRequested = true;
                installer->cancel();
                std::cout << "LIBRARY_CANCEL_REQUESTED progress="
                          << lastProgress << std::endl;
            }
        }

        if (! done.load())
        {
            installer->cancel();
            std::cerr << "LIBRARY_TIMEOUT progress=" << lastProgress << '\n';
            return false;
        }

        return true;
    };

    float firstProgress = -1.0f;
    juce::Result firstResult = juce::Result::ok();

    if (! runInstaller (true, firstProgress, firstResult, 90))
        return 31;

    const auto root = AppPaths::managedLibrariesFolder();
    const auto partialPattern = "." + pack.id + "*.download.part";
    const auto partials = root.findChildFiles (
        juce::File::findFiles, false,
        partialPattern);
    juce::int64 partialBytes = 0;

    for (const auto& file : partials)
        partialBytes += file.getSize();

    if (firstResult.wasOk() || partialBytes <= 0)
    {
        std::cerr << "LIBRARY_CANCEL_DID_NOT_PRESERVE result=\""
                  << firstResult.getErrorMessage() << "\" bytes="
                  << partialBytes << '\n';
        return 32;
    }

    std::cout << "LIBRARY_PARTIAL_PRESERVED bytes=" << partialBytes
              << std::endl;

    float resumedProgress = -1.0f;
    juce::Result resumedResult = juce::Result::fail ("callback ausente");

    if (! runInstaller (false, resumedProgress, resumedResult, 900))
        return 33;

    if (resumedResult.failed())
    {
        std::cerr << "LIBRARY_RESUME_FAILED \""
                  << resumedResult.getErrorMessage() << "\"\n";
        return 34;
    }

    const auto leftovers = root.findChildFiles (
        juce::File::findFiles, false,
        partialPattern);
    const bool marker = pack.installedFolder()
                            .getChildFile (".churchdaw-library.json")
                            .existsAsFile();

    std::cout << "LIBRARY_RESUME_OK installed="
              << (pack.isInstalled() ? 1 : 0)
              << " marker=" << (marker ? 1 : 0)
              << " leftovers=" << leftovers.size() << std::endl;

    return pack.isInstalled() && marker && leftovers.isEmpty() ? 0 : 35;
}
}

int main (int argc, char** argv)
{
    if (argc > 1 && juce::String (argv[1]) == "--network-eof-probe")
        return runNetworkEofProbe();

    if (argc > 1 && juce::String (argv[1]) == "--live-install")
        return runLiveInstallProbe();

    if (argc > 1 && juce::String (argv[1]) == "--live-multipart-resume")
        return runLiveMultipartResumeProbe();

    if (argc > 1 && juce::String (argv[1]) == "--live-library-resume")
        return runLiveLibraryResumeProbe();

    const auto parsed = ChurchDAWTimbreCatalog::parseCatalogJson (R"json(
    {
      "library_id": "church-live",
      "library_name": "Church Live",
      "instruments": [{
        "id": "fixture",
        "name": "Fixture",
        "raw_label": "Fixture",
        "category": "PADS",
        "library_id": "church-live",
        "install_relative": "Fixture",
        "file_count": 4,
        "download_bytes": 4096,
        "download_url": "https://churchdaw.com/app/android/timbres/fixture-v1.zip",
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "included": true,
        "ready": true,
        "parts": [null]
      }]
    })json");

    check (parsed.valid, "catalogo aceita parts:[null] legado");
    check (parsed.instruments.size() == 1, "catalogo preserva instrumento");
    check (parsed.instruments[0].parts.isEmpty(), "null nao vira parte falsa");
    check (parsed.instruments[0].canDownload(), "download unico continua valido");

    check (ChurchDAWTimbreCatalog::isOfficialTimbreDownloadUrl (
               "https://churchdaw.com/app/android/timbres/fixture-v1.zip"),
           "URL oficial de ZIP unico aceita");
    check (ChurchDAWTimbreCatalog::isOfficialTimbreDownloadUrl (
               "https://churchdaw.com/app/android/timbres/fixture-v1.zip.part01"),
           "URL oficial multipart aceita");
    check (! ChurchDAWTimbreCatalog::isOfficialTimbreDownloadUrl (
               "http://churchdaw.com/app/android/timbres/fixture-v1.zip"),
           "download HTTP inseguro rejeitado");
    check (! ChurchDAWTimbreCatalog::isOfficialTimbreDownloadUrl (
               "https://evil.example/fixture-v1.zip"),
           "host externo rejeitado");
    check (! ChurchDAWTimbreCatalog::isOfficialTimbreDownloadUrl (
               "https://churchdaw.com/app/android/timbres/../fixture-v1.zip"),
           "caminho de download relativo rejeitado");

    ChurchDAWTimbrePickerInteraction::DownloadStateRegistry downloads;
    check (downloads.begin ("timbre:piano-a"),
           "primeiro download individual entra na fila");
    check (downloads.begin ("timbre:pad-b"),
           "segundo download individual pode iniciar em paralelo");
    check (! downloads.begin ("timbre:piano-a") && downloads.size() == 2,
           "mesmo timbre nao inicia duas vezes");
    downloads.finish ("timbre:piano-a");
    check (! downloads.contains ("timbre:piano-a")
               && downloads.contains ("timbre:pad-b"),
           "conclusao remove apenas o download correspondente");

    runExactLengthTests();

    std::cout << (failures == 0 ? "TIMBRE_DOWNLOAD_TESTS_OK\n"
                                : "TIMBRE_DOWNLOAD_TESTS_FAIL\n");
    return failures == 0 ? 0 : 1;
}
