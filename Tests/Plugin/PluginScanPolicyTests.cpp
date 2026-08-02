#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginScanPolicy.h"

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
}

int main()
{
    using namespace ChurchDAWPluginScan;

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
