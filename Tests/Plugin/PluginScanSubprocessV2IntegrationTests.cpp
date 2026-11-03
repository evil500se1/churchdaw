#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginScanSubprocessV2.h"

#include <iostream>

namespace
{
class PluginScanIntegrationApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return "ChurchDAWPluginScanIntegrationTests";
    }

    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        using namespace ChurchDAWPluginScan;

        if (commandLine.contains (processUID))
        {
            auto scanner = std::make_unique<PluginScannerSubprocess>();

            if (scanner->initialiseFromCommandLine (commandLine, processUID))
            {
                scannerWorker = std::move (scanner);
                return;
            }

            failAndQuit ("scanner child could not initialise from command line");
            return;
        }

        preparedRoutingOnly = commandLine.contains ("--prepared-routing-only");

        // initialise() roda antes do dispatch loop. A prova fica assíncrona
        // para que o filho AU possa usar normalmente a message thread JUCE.
        juce::MessageManager::callAsync ([this] { runProbe(); });
    }

    void shutdown() override { scannerWorker.reset(); }

    void systemRequestedQuit() override { quit(); }

private:
    void expect (bool condition, const juce::String& message)
    {
        if (condition)
            return;

        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }

    void failAndQuit (const juce::String& message)
    {
        expect (false, message);
        setApplicationReturnValue (1);
        quit();
    }

    // Exercise the same PreparedPluginFormat -> PluginDirectoryScanner ->
    // CustomScanner path used by the gallery, with an invalid local bundle.
    // No installed plugin is enumerated or loaded by this regression.
    void runPreparedFormatProbe()
    {
        using namespace ChurchDAWPluginScan;
        const auto fixture = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getNonexistentChildFile ("churchdaw-prepared-scan", ".vst3", false);
        expect (fixture.createDirectory().wasOk(), "Could not create routing fixture");
        const auto candidate = fixture.getFullPathName();
        juce::VST3PluginFormat vst3;
        PreparedPluginFormat prepared (vst3, { candidate });
        juce::KnownPluginList known;
        auto custom = std::make_unique<ChurchDAWPluginScanner>();
        const auto reportFile = custom->getReportFile();
        known.setCustomScanner (std::move (custom));
        {
            juce::PluginDirectoryScanner scanner (known, prepared, {}, true, {}, true);
            juce::String current;
            while (scanner.scanNextFile (false, current)) {}
        }
        known.scanFinished();

        int inventories = 0, discoveries = 0;
        juce::StringArray lines;
        reportFile.readLines (lines);
        for (const auto& line : lines)
        {
            const auto record = juce::JSON::parse (line);
            if (record.getProperty ("recordType", {}).toString() != "plugin") continue;
            inventories += record.getProperty ("phase", {}).toString() == "vst3_inventory" ? 1 : 0;
            discoveries += record.getProperty ("phase", {}).toString() == "discovery" ? 1 : 0;
        }
        expect (inventories == 1 && discoveries == 0,
                "Prepared VST3 scans must retain per-class isolation, never whole-factory discovery");
        expect (known.getNumTypes() == 0
                 && ! estaNaBlacklist (known, candidate),
                "An invalid routing fixture must remain retryable without adding a fake plugin");
        fixture.deleteRecursively();
        std::cout << "Prepared VST3 routing: inventories=" << inventories
                  << " discoveries=" << discoveries << " failures=" << failures << '\n';
    }

    void runProbe()
    {
        runPreparedFormatProbe();
        if (preparedRoutingOnly)
        {
            setApplicationReturnValue (failures == 0 ? 0 : 1);
            quit();
            return;
        }
       #if JUCE_MAC
        using namespace ChurchDAWPluginScan;

        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);
        juce::AudioPluginFormat* audioUnit = nullptr;

        for (auto* format : formats.getFormats())
            if (format != nullptr
                 && format->getName().containsIgnoreCase ("AudioUnit"))
            {
                audioUnit = format;
                break;
            }

        expect (audioUnit != nullptr,
                "Audio Unit format was not registered in the probe");

        juce::String dlsIdentifier;

        if (audioUnit != nullptr)
        {
            std::atomic<bool> cancelled {false};
            const auto invalid = searchPluginCandidates ("Missing-Test-Format", {}, cancelled);
            expect (invalid.error.isNotEmpty() && invalid.identifiers.isEmpty(),
                    "A failed enumeration must not masquerade as an empty successful scan");
            const auto timedOut = searchPluginCandidates (audioUnit->getName(), {}, cancelled, 0);
            expect (timedOut.error.isNotEmpty() && timedOut.identifiers.isEmpty(),
                    "A timed-out discovery must be stopped without publishing candidates");
            const auto search = searchPluginCandidates (audioUnit->getName(), {}, cancelled);
            expect (search.error.isEmpty(), "Isolated AU discovery failed: " + search.error);
            const auto identifiers = search.identifiers;

            for (const auto& identifier : identifiers)
                if (identifier.containsIgnoreCase (",dls ,appl"))
                {
                    dlsIdentifier = identifier;
                    break;
                }
        }

        expect (dlsIdentifier.isNotEmpty(),
                "DLSMusicDevice was not present in the macOS Audio Unit registry");
        expect (! ehAudioUnitDeSistema (dlsIdentifier),
                "DLSMusicDevice was incorrectly filtered as infrastructure");

        if (audioUnit != nullptr && dlsIdentifier.isNotEmpty())
        {
            juce::KnownPluginList knownPlugins;
            knownPlugins.setCustomScanner (
                std::make_unique<ChurchDAWPluginScanner>());
            juce::OwnedArray<juce::PluginDescription> found;

            const bool added = knownPlugins.scanAndAddFile (
                dlsIdentifier, false, found, *audioUnit);
            knownPlugins.scanFinished();

            expect (added, "DLSMusicDevice did not produce a loaded result");
            expect (! found.isEmpty(),
                    "V2 returned no PluginDescription for DLSMusicDevice");
            expect (! knownPlugins.isBlacklisted (dlsIdentifier, true),
                    "A valid system instrument was added to the blacklist");

            bool exactLoadedDescription = false;

            for (const auto* description : found)
                if (description != nullptr
                     && description->fileOrIdentifier == dlsIdentifier
                     && description->pluginFormatName.containsIgnoreCase (
                            "AudioUnit")
                     && description->isInstrument)
                {
                    exactLoadedDescription = true;
                    break;
                }

            expect (exactLoadedDescription,
                    "Loaded result did not preserve the DLS AU identity");
        }
       #elif JUCE_WINDOWS
        using namespace ChurchDAWPluginScan;

        const auto fixtureValue = juce::SystemStats::getEnvironmentVariable (
                                      "CHURCHDAW_PLUGIN_SCAN_FIXTURE", {})
                                      .trim()
                                      .unquoted();
        const int minimumLoaded = juce::jmax (
            1, juce::SystemStats::getEnvironmentVariable (
                   "CHURCHDAW_PLUGIN_SCAN_MIN_LOADED", "1")
                   .getIntValue());
        const int minimumFactoryClasses = juce::jmax (
            1, juce::SystemStats::getEnvironmentVariable (
                   "CHURCHDAW_PLUGIN_SCAN_MIN_FACTORY_CLASSES", "1")
                   .getIntValue());
        const int minimumFailedClasses = juce::jmax (
            0, juce::SystemStats::getEnvironmentVariable (
                   "CHURCHDAW_PLUGIN_SCAN_MIN_FAILED_CLASSES", "0")
                   .getIntValue());
        const auto requiredFailureStatus =
            juce::SystemStats::getEnvironmentVariable (
                "CHURCHDAW_PLUGIN_SCAN_REQUIRED_FAILURE_STATUS", {})
                .trim();
        const bool strictSynthetic =
            juce::SystemStats::getEnvironmentVariable (
                "CHURCHDAW_PLUGIN_SCAN_STRICT_SYNTHETIC", {})
                .trim() == "1";

        if (fixtureValue.isEmpty())
        {
            failAndQuit (
                "CHURCHDAW_PLUGIN_SCAN_FIXTURE must name one VST3 fixture");
            return;
        }

        const auto fixture = juce::File::isAbsolutePath (fixtureValue)
            ? juce::File (fixtureValue)
            : juce::File::getCurrentWorkingDirectory().getChildFile (
                  fixtureValue);

        if (! fixture.exists())
        {
            failAndQuit ("VST3 fixture does not exist: "
                         + fixture.getFullPathName());
            return;
        }

        juce::AudioPluginFormatManager formats;
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
        auto* vst3 = formats.getFormat (0);

        expect (vst3 != nullptr && vst3->getName() == "VST3",
                "VST3 format was not registered in the Windows probe");
        if (vst3 == nullptr)
        {
            setApplicationReturnValue (1);
            quit();
            return;
        }


        juce::KnownPluginList knownPlugins;
        auto scanner = std::make_unique<ChurchDAWPluginScanner>();
        const auto reportFile = scanner->getReportFile();
        knownPlugins.setCustomScanner (std::move (scanner));
        juce::OwnedArray<juce::PluginDescription> found;
        const auto candidate = fixture.getFullPathName();
        PreparedPluginFormat prepared (*vst3, { candidate });
        const bool added = knownPlugins.scanAndAddFile (
                candidate, false, found, prepared);
        knownPlugins.scanFinished();

        expect (added, "Windows VST3 fixture did not produce a loaded result");
        expect (found.size() >= minimumLoaded,
                "V2 returned fewer PluginDescriptions than required for the Windows fixture");
        expect (! knownPlugins.isBlacklisted (candidate, true),
                "The Windows VST3 fixture was added to the blacklist");

        bool everyIdentityBelongsToCandidate = ! found.isEmpty();
        juce::StringArray uniqueIdentifiers;

        for (const auto* description : found)
        {
            const auto identifier = description != nullptr
                ? description->createIdentifierString()
                : juce::String();
            const bool belongs = description != nullptr
                && description->name.isNotEmpty()
                && identifier.isNotEmpty()
                && ! uniqueIdentifiers.contains (identifier)
                && description->pluginFormatName == "VST3"
                && (juce::File (description->fileOrIdentifier) == fixture
                    || (fixture.isDirectory()
                        && juce::File (description->fileOrIdentifier)
                               .isAChildOf (fixture)));
            everyIdentityBelongsToCandidate &= belongs;

            if (identifier.isNotEmpty())
                uniqueIdentifiers.add (identifier);
        }

        expect (everyIdentityBelongsToCandidate,
                "A loaded VST3 identity was empty, duplicated or outside the fixture candidate");

        int factoryClassCount = -1;
        int loadedClassReports = 0;
        int failedClassReports = 0;
        juce::Array<int> attemptedClassIndexes;
        juce::StringArray classTokens;
        bool classRecordsValid = true;
        bool requiredFailureStatusObserved = requiredFailureStatus.isEmpty();
        juce::String classZeroStatus;
        juce::String classZeroToken;
        juce::String classOneStatus;
        juce::String classOneFailureStage;
        juce::String classOneToken;

        if (reportFile.existsAsFile())
        {
            juce::StringArray reportLines;
            reportFile.readLines (reportLines);

            for (const auto& line : reportLines)
            {
                const auto record = juce::JSON::parse (line);
                const auto* object = record.getDynamicObject();

                if (object == nullptr
                     || object->getProperty ("recordType").toString()
                            != "plugin")
                    continue;

                const auto phase = object->getProperty ("phase").toString();
                const auto status = object->getProperty ("status").toString();

                if (phase == "vst3_inventory" && status == "inventoried")
                    factoryClassCount = object->getProperty ("classCount")
                                            .toString().getIntValue();

                if (phase != "vst3_class_validation")
                    continue;

                const auto classIndexText = object->getProperty ("classIndex")
                                                .toString();
                const auto token = object->getProperty ("token").toString();
                const bool indexIsInteger = object->hasProperty ("classIndex")
                    && classIndexText.isNotEmpty()
                    && classIndexText.containsOnly ("0123456789");
                const int classIndex = indexIsInteger
                    ? classIndexText.getIntValue() : -1;

                classRecordsValid &= indexIsInteger
                    && token.isNotEmpty()
                    && ! classTokens.contains (token);

                attemptedClassIndexes.add (classIndex);
                classTokens.add (token);

                if (classIndex == 0)
                {
                    classZeroStatus = status;
                    classZeroToken = token;
                }
                else if (classIndex == 1)
                {
                    classOneStatus = status;
                    classOneFailureStage =
                        object->getProperty ("failureStage").toString();
                    classOneToken = token;
                }

                if (status == "loaded_success")
                    ++loadedClassReports;
                else if (status != "class_skipped")
                {
                    ++failedClassReports;
                    requiredFailureStatusObserved |=
                        status == requiredFailureStatus;
                }
            }
        }

        expect (factoryClassCount >= minimumFactoryClasses,
                "The VST3 inventory record was missing or announced too few classes");
        expect (classRecordsValid,
                "A V2 class report omitted a numeric classIndex or a unique token");
        expect (attemptedClassIndexes.size() == factoryClassCount,
                "The V2 report does not contain one attempt for every factory class index");

        for (int classIndex = 0; classIndex < factoryClassCount; ++classIndex)
        {
            int occurrences = 0;

            for (const auto attempted : attemptedClassIndexes)
                occurrences += attempted == classIndex ? 1 : 0;

            expect (occurrences == 1,
                    "The V2 report skipped or duplicated a factory class index");
        }

        expect (loadedClassReports == found.size(),
                "Loaded VST3 descriptions and successful class reports disagree");
        expect (failedClassReports >= minimumFailedClasses,
                "The VST3 fixture produced fewer failed class attempts than required");
        expect (requiredFailureStatusObserved,
                "The VST3 fixture did not produce the required failure status");

        if (strictSynthetic)
        {
            expect (factoryClassCount == 2,
                    "The synthetic VST3 factory must expose exactly two classes");
            expect (found.size() == 1,
                    "Exactly one synthetic VST3 class must survive validation");
            expect (found.size() == 1 && found[0] != nullptr
                     && found[0]->name == "ChurchDAW Fixture Valid",
                    "The surviving synthetic class has the wrong identity");
            expect (found.size() == 1 && found[0] != nullptr
                     && found[0]->isInstrument
                     && found[0]->numInputChannels == 0
                     && found[0]->numOutputChannels == 2
                     && ChurchDAWPluginScan::descricaoDeclaraInstrumento (
                            *found[0]),
                    "A MIDI/output-only VST3 that omitted instrument metadata was not promoted to the instrument catalog");
            expect (loadedClassReports == 1 && failedClassReports == 1,
                    "The synthetic shell must report one load and one crash");
            expect (classZeroStatus == "loaded_success",
                    "Synthetic factory class 0 did not load successfully");
            expect (classOneStatus == "crash",
                    "Synthetic factory class 1 was not classified as a crash");
            expect (classOneFailureStage == "creation",
                    "Synthetic factory class 1 did not crash during creation");
            expect (classZeroToken.isNotEmpty()
                     && classOneToken.isNotEmpty()
                     && classZeroToken != classOneToken,
                    "Synthetic class attempts did not use distinct non-empty tokens");
        }
       #else
        expect (false, "This integration probe must run on macOS or Windows");
       #endif

        if (failures == 0)
       #if JUCE_WINDOWS
            std::cout
                << "Plugin scanner V2 vendor smoke passed: " << found.size()
                << " VST3 class(es) loaded from " << factoryClassCount
                << " inventoried factory class(es), " << failedClassReports
                << " failed class attempt(s), and the candidate was not blacklisted\n";
       #else
            std::cout
                << "Plugin scanner V2 integration passed: "
                   "DLSMusicDevice loaded in a disposable child and was not blacklisted\n";
       #endif

        setApplicationReturnValue (failures == 0 ? 0 : 1);
        quit();
    }

    std::unique_ptr<
        ChurchDAWPluginScan::PluginScannerSubprocess> scannerWorker;
    bool preparedRoutingOnly = false;
    int failures = 0;
};
}

START_JUCE_APPLICATION (PluginScanIntegrationApplication)
