#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>

namespace ChurchDAWPluginScan
{
struct CandidateSearchResult
{
    juce::StringArray identifiers;
    juce::String error;
};

// Discovery may wait on a disk, the AU registry or a child process. The GUI
// polls a value-only result; cancelling/destroying the owner never joins a
// worker and the worker never borrows a Component or AudioPluginFormat.
class AsyncCandidateSearch final
{
public:
    using Work = std::function<CandidateSearchResult (const std::atomic<bool>&)>;

    explicit AsyncCandidateSearch (Work work)
        : state (std::make_shared<State>())
    {
        const auto shared = state;
        if (! juce::Thread::launch ([shared, work = std::move (work)]
        {
            CandidateSearchResult result;
            try
            {
                if (! shared->cancelled.load())
                    result = work (shared->cancelled);
            }
            catch (const std::exception& error)
            {
                result.error = error.what();
            }
            catch (...)
            {
                result.error = "Falha ao procurar plugins.";
            }
            const std::lock_guard<std::mutex> lock (shared->mutex);
            if (! shared->cancelled.load())
                shared->result = std::move (result);
        }))
            state->result = CandidateSearchResult {{}, "Nao foi possivel iniciar a busca."};
    }

    ~AsyncCandidateSearch() { cancel(); }

    void cancel() noexcept { state->cancelled.store (true); }

    std::optional<CandidateSearchResult> takeResult()
    {
        const std::lock_guard<std::mutex> lock (state->mutex);
        if (state->cancelled.load())
            return {};
        auto result = std::move (state->result);
        state->result.reset();
        return result;
    }

private:
    struct State
    {
        std::atomic<bool> cancelled {false};
        std::mutex mutex;
        std::optional<CandidateSearchResult> result;
    };
    std::shared_ptr<State> state;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AsyncCandidateSearch)
};

// PluginDirectoryScanner enumerates even when PluginListComponent::scanFor
// receives explicit identifiers: its constructor searches BEFORE those IDs
// are set. Feed it the already discovered snapshot to avoid a second native
// search on the message thread. Keep this adapter alive until scanning ends.
class PreparedPluginFormat final : public juce::AudioPluginFormat
{
public:
    PreparedPluginFormat (juce::AudioPluginFormat& sourceToUse,
                          juce::StringArray candidatesToUse)
        : source (sourceToUse), candidates (std::move (candidatesToUse)) {}

    // The snapshot replaces enumeration only. Custom scanners must still see
    // the real backend when selecting format-specific validation (VST3 shells).
    juce::AudioPluginFormat& getSourceFormat() const noexcept { return source; }

    juce::String getName() const override { return source.getName(); }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override
    { return candidates; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool canScanForPlugins() const override { return source.canScanForPlugins(); }
    bool isTrivialToScan() const override { return source.isTrivialToScan(); }
    bool fileMightContainThisPluginType (const juce::String& id) override
    { return source.fileMightContainThisPluginType (id); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override
    { return source.getNameOfPluginFromIdentifier (id); }
    bool pluginNeedsRescanning (const juce::PluginDescription& desc) override
    { return source.pluginNeedsRescanning (desc); }
    bool doesPluginStillExist (const juce::PluginDescription& desc) override
    { return source.doesPluginStillExist (desc); }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription& desc) const override
    { return source.requiresUnblockedMessageThreadDuringCreation (desc); }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& result,
                              const juce::String& id) override
    { source.findAllTypesForFile (result, id); }

private:
    void createPluginInstance (const juce::PluginDescription& desc, double rate,
                               int block, PluginCreationCallback callback) override
    { source.createPluginInstanceAsync (desc, rate, block, std::move (callback)); }

    juce::AudioPluginFormat& source;
    const juce::StringArray candidates;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreparedPluginFormat)
};
}
