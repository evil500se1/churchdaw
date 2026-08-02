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

    void runProbe()
    {
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
            const auto identifiers = audioUnit->searchPathsForPlugins (
                audioUnit->getDefaultLocationsToSearch(), true, true);

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

        juce::KnownPluginList knownPlugins;
        knownPlugins.setCustomScanner (
            std::make_unique<ChurchDAWPluginScanner>());
        juce::OwnedArray<juce::PluginDescription> found;
        const auto candidate = fixture.getFullPathName();
        const bool added = vst3 != nullptr
            && knownPlugins.scanAndAddFile (
                candidate, false, found, *vst3);
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
       #else
        expect (false, "This integration probe must run on macOS or Windows");
       #endif

        if (failures == 0)
       #if JUCE_WINDOWS
            std::cout
                << "Plugin scanner V2 vendor smoke passed: " << found.size()
                << " VST3 class(es) loaded from the Windows fixture and the "
                   "candidate was not blacklisted\n";
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
    int failures = 0;
};
}

START_JUCE_APPLICATION (PluginScanIntegrationApplication)
