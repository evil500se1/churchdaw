#include "PluginPreviewLifecycle.h"
#include <iostream>
#include <stdexcept>

namespace
{
class PreviewProcessor final : public juce::AudioProcessor
{
public:
    struct Editor final : juce::AudioProcessorEditor
    {
        explicit Editor (PreviewProcessor& p) : AudioProcessorEditor (p), owner (p)
        {
            setResizable (owner.resizable, false);
            setSize (owner.width, owner.height);
        }
        void parentHierarchyChanged() override
        {
            if (owner.throwAttach && getParentComponent() != nullptr)
                throw std::runtime_error ("fixture attach failure");
            if (owner.throwShow && getParentComponent() != nullptr && isShowing())
                throw std::runtime_error ("fixture show failure");
            if (owner.throwHide && getParentComponent() != nullptr && ! isShowing())
                throw std::runtime_error ("fixture hide failure");
        }
        void moved() override
        {
            if (owner.throwAfterAttach && getParentComponent() != nullptr)
                throw std::runtime_error ("fixture owned resize failure");
        }
        ~Editor() override
        {
            owner.notifiedBeforeDestruction = owner.getActiveEditor() != this;
            // Keep the legacy reproduction from leaving a dangling pointer in
            // the test process after recording the broken host contract.
            if (! owner.notifiedBeforeDestruction)
                owner.editorBeingDeleted (this);
            ++owner.editorsDestroyed;
        }
        PreviewProcessor& owner;
    };

    const juce::String getName() const override { return "Preview fixture"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        if (throwEditorCreation) throw std::runtime_error ("fixture editor failure");
        if (throwUnknownEditorError) throw 17;
        return new Editor (*this);
    }
    void prepareToPlay (double, int) override
    {
        ++prepareCalls;
        if (throwPrepare) throw std::runtime_error ("fixture prepare failure");
    }
    void releaseResources() override
    {
        ++releaseCalls;
        if (throwRelease) throw std::runtime_error ("fixture release failure");
    }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    int width = 480, height = 270, editorsDestroyed = 0;
    bool resizable = false, notifiedBeforeDestruction = false;
    bool throwPrepare = false, throwEditorCreation = false;
    bool throwUnknownEditorError = false, throwRelease = false, throwAttach = false;
    bool throwAfterAttach = false, throwShow = false, throwHide = false;
    int prepareCalls = 0, releaseCalls = 0;
};

class PreviewWindow final : public juce::DocumentWindow
{
public:
    PreviewWindow() : DocumentWindow ("Preview test", juce::Colours::black,
                                      DocumentWindow::closeButton) {}
    void closeButtonPressed() override {}
};
}

int runPluginPreviewLifecycleTests (bool legacy)
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    { if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } };
    PreviewProcessor processor;
    PreviewWindow window;
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        processor.resizable = cycle >= 2;
        processor.width = cycle % 2 == 0 ? 480 : 1450;
        processor.height = cycle % 2 == 0 ? 270 : 950;
        auto* editor = processor.createEditorIfNeeded();
        expect (editor != nullptr && processor.getActiveEditor() == editor,
                "Opening must register a live editor");
        if (legacy)
        {
            window.setResizable (true, true);
            window.setContentOwned (editor, true);
            window.centreWithSize (juce::jlimit (420, 1280, juce::jmax (640, editor->getWidth())),
                                   juce::jlimit (320, 900, juce::jmax (460, editor->getHeight())));
        }
        else
            ChurchDAWPluginPreview::attachEditor (window, editor);
        expect (editor->getWidth() == processor.width && editor->getHeight() == processor.height,
                "Opening must preserve the manufacturer's initial editor size");
        expect (window.isResizable() == processor.resizable,
                "Fixed plugin editors must not be made resizable by the host");
        if (legacy)
            window.clearContentComponent();
        else
            ChurchDAWPluginPreview::clearEditor (window, &processor);
        expect (processor.notifiedBeforeDestruction,
                "Host must notify the processor before destroying its editor");
        expect (processor.getActiveEditor() == nullptr && processor.editorsDestroyed == cycle + 1,
                "Each close must destroy exactly one editor and allow reopening");
    }
    ChurchDAWPluginPreview::clearEditor (window, &processor);
    expect (processor.editorsDestroyed == 4, "Repeated close must be harmless");
    for (int failureStage = 0; failureStage < 5; ++failureStage)
    {
        PreviewProcessor failing;
        failing.throwPrepare = failureStage == 0;
        failing.throwEditorCreation = failureStage == 1;
        failing.throwUnknownEditorError = failureStage == 2;
        failing.throwAttach = failureStage == 3;
        failing.throwAfterAttach = failureStage == 4;
        const auto failed = ChurchDAWPluginPreview::prepareAndAttachEditor (
            window, failing, 48000.0, 512);
        expect (failed.failed() && failed.getErrorMessage().isNotEmpty(),
                "Catchable prepare/editor failures must produce an actionable error");
        if (failureStage < 2)
            expect (failed.getErrorMessage().contains (failureStage == 0 ? "prepare failure" : "editor failure"),
                    "Vendor exception detail must survive preview initialization");
        expect (window.getContentComponent() == nullptr && failing.getActiveEditor() == nullptr,
                "Failed preview must not leave an owned or registered editor");
        expect (failing.prepareCalls == 1 && failing.editorsDestroyed == (failureStage >= 3 ? 1 : 0),
                "Failed creation must not invent or double-delete an editor");
        expect (ChurchDAWPluginPreview::releaseResourcesSafely (failing).wasOk()
                 && failing.releaseCalls == 1,
                "Failed preview must be releasable exactly once");

        failing.throwPrepare = failing.throwEditorCreation = failing.throwUnknownEditorError = false;
        failing.throwAttach = failing.throwAfterAttach = false;
        expect (ChurchDAWPluginPreview::prepareAndAttachEditor (
                    window, failing, 48000.0, 512).wasOk(),
                "A failed preview must allow the next attempt to open");
        ChurchDAWPluginPreview::clearEditor (window, &failing);
        expect (failing.notifiedBeforeDestruction && failing.editorsDestroyed == (failureStage >= 3 ? 2 : 1),
                "Recovered preview must preserve editor ownership on close");
        failing.throwRelease = true;
        const auto release = ChurchDAWPluginPreview::releaseResourcesSafely (failing);
        expect (release.failed() && release.getErrorMessage().contains ("release failure")
                 && failing.releaseCalls == 2 && failing.getActiveEditor() == nullptr,
                "A catchable release failure must be reported after editor cleanup");
    }
    for (bool throwOnShow : { true, false })
    {
        PreviewProcessor visibility;
        PreviewWindow visibleWindow;
        expect (ChurchDAWPluginPreview::prepareAndAttachEditor (
                    visibleWindow, visibility, 48000.0, 512).wasOk(),
                "Visibility fixture must initialize before its vendor callback is armed");
        visibility.throwShow = throwOnShow;
        const auto shown = ChurchDAWPluginPreview::showEditorSafely (visibleWindow);
        expect (throwOnShow ? shown.failed() && shown.getErrorMessage().contains ("show failure")
                            : shown.wasOk(),
                "A vendor native show failure must return through the error boundary");
        visibility.throwHide = ! throwOnShow;
        const auto closed = ChurchDAWPluginPreview::hideAndClearEditorSafely (
            visibleWindow, &visibility);
        expect (throwOnShow ? closed.wasOk()
                            : closed.failed() && closed.getErrorMessage().contains ("hide failure"),
                "A vendor native hide failure must not escape cleanup");
        expect (visibility.notifiedBeforeDestruction && visibility.editorsDestroyed == 1
                 && visibility.getActiveEditor() == nullptr
                 && visibleWindow.getContentComponent() == nullptr
                 && visibility.releaseCalls == 0,
                "Visibility failure must delete and unregister the editor before releasing the processor");
        expect (ChurchDAWPluginPreview::releaseResourcesSafely (visibility).wasOk()
                 && visibility.releaseCalls == 1,
                "Visibility failure cleanup must still release the processor once");
        expect (ChurchDAWPluginPreview::hideAndClearEditorSafely (visibleWindow, &visibility).wasOk()
                 && visibility.editorsDestroyed == 1,
                "Repeated close after a visibility failure must be harmless");
    }
    std::cout << "PLUGIN_PREVIEW_LIFECYCLE failures=" << failures << '\n';
    return failures ? 1 : 0;
}
