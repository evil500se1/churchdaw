#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginScanSubprocessV2.h"

#include <initializer_list>
#include <iostream>

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

class EmptyScanFormat final : public juce::AudioPluginFormat
{
public:
    explicit EmptyScanFormat (juce::FileSearchPath defaultsToUse = {},
                              juce::String formatNameToUse = "VST3")
        : defaults (std::move (defaultsToUse)), name (std::move (formatNameToUse))
    {
    }

    juce::String getName() const override { return name; }
    void findAllTypesForFile (
        juce::OwnedArray<juce::PluginDescription>&,
        const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String&) override
    {
        return true;
    }
    juce::String getNameOfPluginFromIdentifier (
        const juce::String& identifier) override
    {
        return identifier;
    }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override
    {
        return true;
    }
    bool doesPluginStillExist (const juce::PluginDescription&) override
    {
        return true;
    }
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins (
        const juce::FileSearchPath&, bool, bool) override
    {
        return {};
    }
    juce::FileSearchPath getDefaultLocationsToSearch() override
    {
        return defaults;
    }

private:
    void createPluginInstance (
        const juce::PluginDescription&, double, int,
        PluginCreationCallback callback) override
    {
        callback ({}, "not used by this scan policy test");
    }

    bool requiresUnblockedMessageThreadDuringCreation (
        const juce::PluginDescription&) const override
    {
        return false;
    }

    juce::FileSearchPath defaults;
    juce::String name;
};

class SuccessfulEmptyScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (
        juce::AudioPluginFormat&,
        juce::OwnedArray<juce::PluginDescription>&,
        const juce::String&) override
    {
        return true;
    }
};
}

int main()
{
    using namespace ChurchDAWPluginScan;

    {
        juce::AudioPluginFormatManager formats;
        formats.addFormat (std::make_unique<EmptyScanFormat>());
        expect (! podeIniciarBusca (formats, 0),
                "File-only scans require an existing selected folder");
        expect (podeIniciarBusca (formats, 1),
                "An existing folder must enable file-based scanning");
        formats.addFormat (std::make_unique<EmptyScanFormat> (
            juce::FileSearchPath(), "AudioUnit"));
        expect (podeIniciarBusca (formats, 0),
                "AU-only Macs must scan the registry without any VST folder");
    }

    juce::DynamicObject::Ptr diagnosticRecord (new juce::DynamicObject());
    diagnosticRecord->setProperty ("recordType", "plugin");
    diagnosticRecord->setProperty ("status", "loaded_success");
    const auto diagnosticLine = serialiseDiagnosticJsonLine (
        diagnosticRecord.get());
    const auto diagnosticPayload = diagnosticLine.dropLastCharacters (1);
    juce::var parsedDiagnostic;
    const auto diagnosticParse = juce::JSON::parse (diagnosticPayload,
                                                     parsedDiagnostic);
    expect (diagnosticLine.endsWithChar ('\n')
             && ! diagnosticPayload.containsAnyOf ("\r\n"),
            "Scanner diagnostics must emit exactly one JSON object per line");
    expect (diagnosticParse.wasOk()
             && parsedDiagnostic.getProperty ("status", {})
                    .toString() == "loaded_success",
            "Scanner diagnostic JSON line must be independently parseable");

    ScanRequest classRequest;
    classRequest.phase = ScanPhase::vst3ClassValidation;
    classRequest.token = "class-token";
    classRequest.format = "VST3";
    classRequest.identifier = "Shell.vst3";
    classRequest.classIndex = 7;
    ScanRequest decodedClassRequest;
    juce::String protocolError;
    ScanRequest enumerationRequest;
    enumerationRequest.phase = ScanPhase::enumeration;
    enumerationRequest.token = "enumeration-token";
    enumerationRequest.format = "AudioUnit";
    enumerationRequest.identifier = "candidate-search";
    enumerationRequest.descriptionXml = "<SEARCH_PATHS><PATH value=\"/tmp/Plugin folder\"/></SEARCH_PATHS>";
    ScanRequest decodedEnumeration;
    expect (deserialiseRequest (serialiseRequest (enumerationRequest),
                               decodedEnumeration, protocolError)
             && decodedEnumeration.phase == ScanPhase::enumeration
             && decodedEnumeration.descriptionXml == enumerationRequest.descriptionXml,
            "Isolated enumeration must preserve search paths including spaces");
    expect (deserialiseRequest (serialiseRequest (classRequest),
                                decodedClassRequest, protocolError)
             && decodedClassRequest.phase
                    == ScanPhase::vst3ClassValidation
             && decodedClassRequest.classIndex == 7,
            "VST3 class requests must preserve their isolated class index");

    juce::PluginDescription metadataInstrument;
    metadataInstrument.name = "Metadata instrument";
    metadataInstrument.category = "Fx|Instrument|Piano";
    metadataInstrument.isInstrument = false;
    expect (descricaoDeclaraInstrumento (metadataInstrument),
            "VST3 Instrument subcategory must override a stale false role bit");

    juce::PluginDescription omittedRole;
    omittedRole.name = "Studio Grand fixture";
    omittedRole.pluginFormatName = "VST3";
    omittedRole.fileOrIdentifier = "StudioGrand.vst3";
    omittedRole.uniqueId = 0x5001;
    omittedRole.category = "Fx";
    omittedRole.isInstrument = false;
    expect (deveSerTratadoComoInstrumento (
                omittedRole, { true, 0, 2 }),
            "A MIDI-capable output-only VST3 must be treated as an instrument");
    expect (! deveSerTratadoComoInstrumento (
                omittedRole, { true, 2, 2 }),
            "A MIDI-controlled audio effect must remain an effect");
    expect (! deveSerTratadoComoInstrumento (
                omittedRole, { false, 0, 2 }),
            "A signal generator without MIDI must not become an instrument");

    juce::PluginDescription validatedRole = omittedRole;
    validatedRole.numInputChannels = 99;
    validatedRole.numOutputChannels = 99;
    const auto identityBeforeRoleNormalisation =
        validatedRole.createIdentifierString();
    expect (normalizarPapelDoPluginValidado (
                omittedRole, validatedRole, { true, 0, 2 }),
            "Runtime role normalisation must report a changed description");
    expect (validatedRole.isInstrument
             && validatedRole.numInputChannels == 0
             && validatedRole.numOutputChannels == 2,
            "Runtime role normalisation did not persist instrument topology");
    expect (validatedRole.createIdentifierString()
                 == identityBeforeRoleNormalisation,
            "Role normalisation must not change the stable plugin identity");

    {
        juce::KnownPluginList staleRoles;
        juce::PluginDescription staleInstrument;
        staleInstrument.name = "Saved piano";
        staleInstrument.pluginFormatName = "VST3";
        staleInstrument.fileOrIdentifier = "SavedPiano.vst3";
        staleInstrument.uniqueId = 0x5101;
        staleInstrument.category = "Instrument|Piano";
        staleInstrument.isInstrument = false;
        staleRoles.addType (staleInstrument);

        juce::PluginDescription savedEffect = staleInstrument;
        savedEffect.name = "Saved effect";
        savedEffect.fileOrIdentifier = "SavedEffect.vst3";
        savedEffect.uniqueId = 0x5102;
        savedEffect.category = "Fx|Delay";
        staleRoles.addType (savedEffect);

        expect (normalizarPapeisDaListaConhecida (staleRoles) == 1,
                "Exactly the stale instrument role must be migrated");

        const auto migrated = staleRoles.getTypeForIdentifierString (
            staleInstrument.createIdentifierString());
        expect (migrated != nullptr && migrated->isInstrument,
                "A saved VST3 instrument remained in the effects catalog");
    }

    {
        const auto locationRoot =
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("churchdaw-plugin-location-policy");
        const auto custom = locationRoot.getChildFile ("Custom");
        const auto defaultA = locationRoot.getChildFile ("DefaultA");
        const auto defaultB = locationRoot.getChildFile ("DefaultB");

        juce::Array<PluginScanLocation> locations;
        locations.add ({ custom, PluginScanLocationKind::custom, true });
        locations.add ({ defaultA, PluginScanLocationKind::system, false });

        juce::FileSearchPath defaults;
        defaults.add (defaultA);
        defaults.add (defaultB);
        juce::AudioPluginFormatManager locationFormats;
        locationFormats.addFormat (
            std::make_unique<EmptyScanFormat> (defaults));

        const int initialLocationCount = locations.size();
        const int mergedLocationCount = mesclarPastasPadraoDePlugins (
            locations, locationFormats, false);
        expect (mergedLocationCount >= 1,
                "A saved custom path must not suppress a missing system VST3 path");
        bool containsDefaultB = false;
        for (const auto& location : locations)
            containsDefaultB |= mesmaPastaDeScan (location.folder, defaultB);
        expect (containsDefaultB
                 && locations.size()
                        == initialLocationCount + mergedLocationCount,
                "The missing default VST3 path was not appended");
        expect (! locations[1].enabled,
                "Merging defaults must not re-enable a path disabled by the user");
        expect (mesclarPastasPadraoDePlugins (
                    locations, locationFormats, false) == 0
                 && locations.size()
                        == initialLocationCount + mergedLocationCount,
                "Repeated default-path migration must be idempotent");
    }

    expect (! deveEntrarNaBlacklistPersistente (PhaseFinish::result),
            "A normal validation rejection must remain retryable");
    expect (! deveEntrarNaBlacklistPersistente (PhaseFinish::cancelled),
            "A user cancellation must not persist a blacklist entry");
    expect (! deveEntrarNaBlacklistPersistente (PhaseFinish::launchFailed),
            "A host launch failure must not blacklist the plugin");
    expect (! deveEntrarNaBlacklistPersistente (PhaseFinish::sendFailed),
            "A host protocol failure must not blacklist the plugin");
    expect (deveEntrarNaBlacklistPersistente (PhaseFinish::crash),
            "A crashing plugin must remain isolated by the blacklist");
    expect (! deveEntrarNaBlacklistPersistente (PhaseFinish::timeout),
            "A timed-out plugin must remain retryable");
    expect (ehTentativaInconclusiva (PhaseFinish::timeout)
             && ehTentativaInconclusiva (PhaseFinish::launchFailed)
             && ehTentativaInconclusiva (PhaseFinish::sendFailed),
            "Timeout and transport failures must remain inconclusive");
    expect (! ehTentativaInconclusiva (PhaseFinish::crash)
             && ! ehTentativaInconclusiva (PhaseFinish::result)
             && ! ehTentativaInconclusiva (PhaseFinish::cancelled),
            "Crash, result and cancellation must not masquerade as inconclusive transport");

    const auto aggregateIsFatal = [] (
        int loaded,
        std::initializer_list<PhaseFinish> attempts,
        int retryableOrSkipped = 0)
    {
        CandidateValidationSummary summary;
        summary.loaded = loaded;
        summary.retryableOrSkipped = retryableOrSkipped;

        for (const auto finish : attempts)
            if (deveEntrarNaBlacklistPersistente (finish))
                ++summary.fatal;
            else
                ++summary.retryableOrSkipped;

        return deveEntrarNaBlacklistPersistente (
            summary);
    };

    expect (! aggregateIsFatal (
                0, { PhaseFinish::crash, PhaseFinish::launchFailed }),
            "A crashing shell class plus a host error must remain retryable");
    expect (! aggregateIsFatal (
                0, { PhaseFinish::timeout, PhaseFinish::result }),
            "A timed-out shell class plus an initialization failure must remain retryable");
    expect (! aggregateIsFatal (0, { PhaseFinish::crash }, 1),
            "An architecture-skipped sibling must protect the whole shell from blacklist");
    expect (aggregateIsFatal (0, { PhaseFinish::crash }),
            "A crash-only candidate must be blacklisted");
    expect (aggregateIsFatal (
                0, { PhaseFinish::crash, PhaseFinish::crash }),
            "A shell whose attempted classes all crash must be blacklisted");
    expect (! aggregateIsFatal (0, { PhaseFinish::timeout }),
            "A timeout-only candidate must remain retryable");
    expect (! aggregateIsFatal (
                0, { PhaseFinish::crash, PhaseFinish::timeout }),
            "A crashing shell class plus a timed-out sibling is inconclusive");
    expect (! aggregateIsFatal (
                1, { PhaseFinish::crash, PhaseFinish::timeout }),
            "Any loaded shell class must protect the whole bundle from blacklist");
    expect (! aggregateIsFatal (0, {}),
            "An empty or inconclusive candidate must not be called a crash");

    juce::OwnedArray<juce::PluginDescription> survivingShellClasses;
    CandidateValidationAccumulator shellAccumulator {
        survivingShellClasses };
    juce::PluginDescription validShellClass;
    validShellClass.name = "Valid class";
    validShellClass.pluginFormatName = "VST3";
    validShellClass.fileOrIdentifier = "Shell.vst3";
    validShellClass.uniqueId = 0x1234;
    shellAccumulator.addLoaded (validShellClass);
    shellAccumulator.addFailure (PhaseFinish::crash, true);

    expect (survivingShellClasses.size() == 1
             && survivingShellClasses[0]->name == "Valid class",
            "A later crashing shell class must not erase an already validated sibling");
    expect (! deveEntrarNaBlacklistPersistente (
                shellAccumulator.getSummary()),
            "A shell with a validated class must survive a sibling crash");

    juce::OwnedArray<juce::PluginDescription> unknownFactoryClasses;
    CandidateValidationAccumulator unknownClassAccumulator {
        unknownFactoryClasses };
    unknownClassAccumulator.addFailure (PhaseFinish::crash, false);
    expect (! deveEntrarNaBlacklistPersistente (
                unknownClassAccumulator.getSummary()),
            "A crash before identifying an audio class must remain inconclusive");
    expect (precisaRevalidarBlacklistLegada (0)
             && precisaRevalidarBlacklistLegada (1)
             && precisaRevalidarBlacklistLegada (2)
             && ! precisaRevalidarBlacklistLegada (3),
            "Legacy blacklist semantics migration must run exactly before v3");

    const juce::String appleMixer =
        "AudioUnit:Mixers/aumx,mcmx,appl";
    const juce::String applePanner =
        "AudioUnit:Panners/aupn,vbas,appl";
    const juce::String appleNetwork =
        "AudioUnit:Generators/augn,nsnd,appl";
    const juce::String kontakt =
        "AudioUnit:Synths/aumu,Nik8,NISI";
    const juce::String bigSky =
        "AudioUnit:Effects/aufx,BgSk,Strm";
    const juce::String appleDls =
        "AudioUnit:Synths/aumu,dls ,appl";
    const juce::String appleReverb =
        "AudioUnit:Effects/aufx,rvb2,appl";
    const juce::String thirdPartyPanner =
        "AudioUnit:Panners/aupn,tpan,Acme";

    expect (ehAudioUnitDeSistema (appleMixer), "Apple mixer must be skipped");
    expect (ehAudioUnitDeSistema (applePanner), "Apple panner must be skipped");
    expect (ehAudioUnitDeSistema (appleNetwork), "Apple network generator must be skipped");
    expect (! ehAudioUnitDeSistema (kontakt), "Kontakt AU must remain hostable");
    expect (! ehAudioUnitDeSistema (bigSky), "BigSky AU must remain hostable");
    expect (! ehAudioUnitDeSistema (appleDls), "Apple DLS instrument must remain hostable");
    expect (! ehAudioUnitDeSistema (appleReverb),
            "Apple musical effects must remain hostable");
    expect (! ehAudioUnitDeSistema (thirdPartyPanner),
            "Third-party panners must not be removed by Apple policy");
    expect (! ehAudioUnitDeSistema ("/Library/Audio/Plug-Ins/VST3/Test.vst3"),
            "VST3 identifiers must be unaffected");

    juce::StringArray candidates { appleMixer, kontakt, bigSky, applePanner,
                                   thirdPartyPanner };
    const auto filtered = filtrarAudioUnitsHospedaveis (candidates);
    expect (filtered.size() == 3, "Only policy Audio Units should be removed");
    expect (filtered.contains (kontakt), "Kontakt disappeared from filtered scan");
    expect (filtered.contains (bigSky), "BigSky disappeared from filtered scan");
    expect (filtered.contains (thirdPartyPanner),
            "Third-party panner disappeared from filtered scan");

    juce::KnownPluginList list;
    list.addToBlacklist (appleMixer);
    list.addToBlacklist (applePanner);
    list.addToBlacklist (kontakt);

    expect (removerAudioUnitsDeSistemaDaBlacklist (list) == 2,
            "Blacklist migration removed the wrong count");
    expect (! list.getBlacklistedFiles().contains (appleMixer),
            "Apple mixer remained blacklisted");
    expect (! list.getBlacklistedFiles().contains (applePanner),
            "Apple panner remained blacklisted");
    expect (list.getBlacklistedFiles().contains (kontakt),
            "A third-party failure was incorrectly cleared");

    {
        const auto staleRoot = juce::File::getSpecialLocation (
                                   juce::File::tempDirectory)
                                   .getNonexistentChildFile (
                                       "churchdaw-stale-shell", ".vst3", false);
        expect (staleRoot.createDirectory().wasOk(),
                "Could not create stale VST3 bundle fixture");

        juce::KnownPluginList staleList;
        juce::PluginDescription stale;
        stale.name = "Old shell class";
        stale.pluginFormatName = "VST3";
        stale.fileOrIdentifier = staleRoot.getChildFile ("Contents")
                                     .getChildFile ("x86_64-win")
                                     .getChildFile ("Shell.vst3")
                                     .getFullPathName();
        stale.uniqueId = 0x4001;
        staleList.addType (stale);

        juce::PluginDescription otherFormat = stale;
        otherFormat.name = "Unrelated format";
        otherFormat.pluginFormatName = "Other";
        otherFormat.fileOrIdentifier = staleRoot.getFullPathName();
        otherFormat.uniqueId = 0x4002;
        staleList.addType (otherFormat);
        staleList.setCustomScanner (
            std::make_unique<SuccessfulEmptyScanner>());

        EmptyScanFormat emptyFormat;
        juce::OwnedArray<juce::PluginDescription> foundAfterFailure;
        const bool added = staleList.scanAndAddFile (
            staleRoot.getFullPathName(), false,
            foundAfterFailure, emptyFormat);
        const auto remainingTypes = staleList.getTypes();

        expect (! added && foundAfterFailure.isEmpty(),
                "The clean failure fixture unexpectedly returned a plugin");
        expect (remainingTypes.size() == 1
                 && remainingTypes[0].pluginFormatName == "Other",
                "A failed normal rescan kept the stale VST3 description or removed another format");
        staleRoot.deleteRecursively();
    }

    const auto tempDir = juce::File::getSpecialLocation (
                             juce::File::tempDirectory)
                             .getNonexistentChildFile (
                                 "churchdaw-plugin-policy-test", {}, false);
    expect (tempDir.createDirectory().wasOk(),
            "Could not create temporary test directory");
    const auto deadMansPedal = tempDir.getChildFile ("PluginsComProblema.txt");
    expect (deadMansPedal.replaceWithText (
                appleMixer + "\n" + kontakt + "\n" + applePanner + "\n"),
            "Could not write dead-man test fixture");
    expect (removerAudioUnitsDeSistemaDoDeadMansPedal (deadMansPedal) == 2,
            "Dead-man migration removed the wrong count");

    juce::StringArray remaining;
    deadMansPedal.readLines (remaining);
    remaining.removeEmptyStrings();
    expect (remaining.size() == 1 && remaining[0] == kontakt,
            "Dead-man migration did not preserve the third-party entry");
    tempDir.deleteRecursively();

    if (failures != 0)
        return 1;

    std::cout << "Plugin scan policy tests passed\n";
    return 0;
}
