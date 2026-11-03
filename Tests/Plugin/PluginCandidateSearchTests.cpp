#include "PluginCandidateSearch.h"
#include <iostream>
#include <stdexcept>

namespace
{
class SearchFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return "AudioUnit"; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override
    {
        ++searches;
        juce::Thread::sleep (150); // a slow OS registry, even with explicit IDs
        return {"unexpected-registry-entry"};
    }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& result,
                              const juce::String& id) override
    {
        auto* description = new juce::PluginDescription();
        description->name = "Fixture";
        description->fileOrIdentifier = id;
        description->pluginFormatName = getName();
        result.add (description);
    }
    bool fileMightContainThisPluginType (const juce::String&) override { return true; }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override { return id; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override
    { return false; }
    int searches = 0;
private:
    void createPluginInstance (const juce::PluginDescription&, double, int,
                               PluginCreationCallback callback) override
    { callback ({}, "not used"); }
};

struct SearchGate
{
    juce::WaitableEvent entered, release, exited;
    std::atomic<bool> cancelled {false};
};
}

int runPluginCandidateSearchTests (bool legacy)
{
    using namespace ChurchDAWPluginScan;
    int failures = 0;
    const auto expect = [&] (bool passed, const char* message)
    { if (! passed) { ++failures; std::cerr << "FAIL: " << message << '\n'; } };

    SearchFormat source;
    const juce::StringArray ids {"requested-plugin"};
    PreparedPluginFormat prepared (source, ids);
    juce::KnownPluginList known;
    {
        // This is the constructor invoked synchronously by scanFor(ids).
        juce::PluginDirectoryScanner scanner (
            known, legacy ? static_cast<juce::AudioPluginFormat&> (source) : prepared,
            {}, true, {}, true);
        scanner.setFilesOrIdentifiersToScan (ids);
        expect (source.searches == 0,
                "Explicit scan must not re-enumerate the AU registry on the GUI thread");
        juce::String current;
        while (scanner.scanNextFile (false, current)) {}
    }
    expect (known.getNumTypes() == 1
             && known.getTypes()[0].fileOrIdentifier == "requested-plugin",
            "Prepared scan must still validate the requested candidate");
    expect (prepared.getName() == source.getName(), "Format identity must be preserved");

    const auto gate = std::make_shared<SearchGate>();
    auto job = std::make_unique<AsyncCandidateSearch> ([gate] (const std::atomic<bool>& cancelled)
    {
        gate->entered.signal();
        gate->release.wait (3000);
        gate->cancelled.store (cancelled.load());
        gate->exited.signal();
        return CandidateSearchResult {{"late-result"}, {}};
    });
    expect (gate->entered.wait (1000), "Discovery worker must start without blocking the caller");
    expect (! job->takeResult().has_value(), "Running discovery must not claim completion");
    const auto beforeCancel = juce::Time::getMillisecondCounterHiRes();
    job.reset();
    expect (juce::Time::getMillisecondCounterHiRes() - beforeCancel < 100,
            "Closing the manager must not join a blocked discovery worker");
    gate->release.signal();
    expect (gate->exited.wait (1000), "Cancelled worker must be able to finish safely");
    expect (gate->cancelled.load(), "Closing must signal cancellation before discarding the job");

    AsyncCandidateSearch next ([] (const std::atomic<bool>&)
    { return CandidateSearchResult {{"new-result"}, {}}; });
    std::optional<CandidateSearchResult> result;
    for (int i = 0; i < 200 && ! result.has_value(); ++i)
    { result = next.takeResult(); if (! result) juce::Thread::sleep (5); }
    expect (result && result->identifiers == juce::StringArray {"new-result"},
            "Reopening must receive only the new search, never a cancelled result");
    expect (! next.takeResult(), "Discovery results must be consumed exactly once");

    AsyncCandidateSearch failed ([] (const std::atomic<bool>&) -> CandidateSearchResult
    { throw std::runtime_error ("fixture search failure"); });
    result.reset();
    for (int i = 0; i < 200 && ! result.has_value(); ++i)
    { result = failed.takeResult(); if (! result) juce::Thread::sleep (5); }
    expect (result && result->error == "fixture search failure" && result->identifiers.isEmpty(),
            "Discovery failures must be reported, not mistaken for an empty successful scan");
    std::cout << "PLUGIN_CANDIDATE_SEARCH failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
