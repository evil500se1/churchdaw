#pragma once

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChurchOrganLayout.h"
#include "UI/ChurchOrganViewModel.h"

namespace churchdaw::organ
{
// Standalone, host-agnostic editor surface. It deliberately derives from
// Component rather than AudioProcessorEditor; the processor adapter can own it
// later without making this UI phase depend on DSP or host state.
class ChurchOrganEditor final : public juce::Component,
                                private juce::Timer
{
public:
    explicit ChurchOrganEditor (ChurchOrganParameterSink&);
    ~ChurchOrganEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    ChurchOrganLayout::Page getActivePage() const noexcept;
    void showPage (ChurchOrganLayout::Page);
    void setHeaderRightInset (int insetPx);

    // Read-only geometry probes used by the isolated UI harness.
    ChurchOrganLayout::Shell getShellLayoutForTesting() const noexcept;
    ChurchOrganLayout::Live getLiveLayoutForTesting() const noexcept;
    juce::Rectangle<int> getNavigationButtonBoundsForTesting (int index) const;
    juce::Rectangle<int> getLiveDrawbarBoundsForTesting (int index) const;
    bool livePageUsesViewportForTesting() const noexcept { return false; }

private:
    void timerCallback() override;

    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchOrganEditor)
};
} // namespace churchdaw::organ
