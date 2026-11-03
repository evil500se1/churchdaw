#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <exception>

namespace ChurchDAWPluginPreview
{
inline void clearEditor (juce::DocumentWindow& window,
                         juce::AudioProcessor* processor)
{
    if (processor != nullptr)
        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (
                window.getContentComponent()))
            if (processor->getActiveEditor() == editor)
                processor->editorBeingDeleted (editor);

    window.clearContentComponent();
}

inline void attachEditor (juce::DocumentWindow& window,
                          juce::AudioProcessorEditor* editor)
{
    // Fixed-size native plugin views must retain the size requested by their
    // manufacturer. Resizing every view to 640x460 can crop controls or trigger
    // repeated host/plugin resize requests.
    window.setResizable (editor->isResizable(), false);
    window.setContentOwned (editor, true);
    window.centreWithSize (window.getWidth(), window.getHeight());
}

// Called only from an exception handler. Vendor C++ failures can be reported;
// native crashes and an editor that never returns require separate isolation.
inline juce::String currentExceptionMessage()
{
    try { throw; }
    catch (const std::exception& error)
    {
        const auto detail = juce::String::fromUTF8 (error.what()).trim();
        return detail.isNotEmpty() ? detail : juce::String ("Erro interno do plugin.");
    }
    catch (...) { return "Erro interno do plugin sem detalhes."; }
}

inline juce::Result prepareAndAttachEditor (juce::DocumentWindow& window,
                                            juce::AudioProcessor& processor,
                                            double sampleRate, int blockSize)
{
    std::unique_ptr<juce::AudioProcessorEditor> pendingEditor;
    juce::String failure;
    try
    {
        processor.enableAllBuses();
        processor.prepareToPlay (sampleRate, blockSize);
        pendingEditor.reset (processor.createEditorIfNeeded());
        if (pendingEditor == nullptr)
            pendingEditor = std::make_unique<juce::GenericAudioProcessorEditor> (processor);
        attachEditor (window, pendingEditor.get());
        pendingEditor.release();
        return juce::Result::ok();
    }
    catch (...) { failure = currentExceptionMessage(); }

    // A vendor parent/resize callback can throw before OR after JUCE marks
    // setContentOwned's content as owned. Use a safe pointer to tell which
    // side actually deleted it, instead of assuming ownership was transferred.
    juce::Component::SafePointer<juce::AudioProcessorEditor> remaining (pendingEditor.release());
    try { clearEditor (window, &processor); }
    catch (...) { failure += " / Encerramento: " + currentExceptionMessage(); }
    if (auto* editor = remaining.getComponent())
    {
        if (processor.getActiveEditor() == editor)
            processor.editorBeingDeleted (editor);
        delete editor;
    }
    return juce::Result::fail (failure);
}

// Visibility on a native window notifies every hosted child through
// parentHierarchyChanged. A failure here must not bypass editor teardown.
inline juce::Result showEditorSafely (juce::DocumentWindow& window)
{
    juce::Component::SafePointer<juce::DocumentWindow> safe (&window);
    try
    {
        safe->setVisible (true);
        if (safe != nullptr) safe->enterModalState (true, nullptr, false);
        if (safe != nullptr) safe->toFront (true);
        if (safe != nullptr) return juce::Result::ok();
        return juce::Result::fail ("A janela do plugin foi fechada durante a abertura.");
    }
    catch (...) { return juce::Result::fail (currentExceptionMessage()); }
}

inline juce::Result hideAndClearEditorSafely (juce::DocumentWindow& window,
                                             juce::AudioProcessor* processor)
{
    juce::String failure;
    try { if (window.isCurrentlyModal()) window.exitModalState (0); }
    catch (...) { failure = currentExceptionMessage(); }
    try { window.setVisible (false); }
    catch (...)
    {
        const auto detail = currentExceptionMessage();
        failure = failure.isEmpty() ? detail : failure + " / " + detail;
    }

    // Continue after a vendor hide callback throws: retaining an editor while
    // releasing/destroying its processor would violate the host contract.
    try { clearEditor (window, processor); }
    catch (...)
    {
        const auto detail = currentExceptionMessage();
        failure = failure.isEmpty() ? detail : failure + " / " + detail;
    }
    return failure.isEmpty() ? juce::Result::ok() : juce::Result::fail (failure);
}

inline juce::Result releaseResourcesSafely (juce::AudioProcessor& processor)
{
    try { processor.releaseResources(); }
    catch (...) { return juce::Result::fail (currentExceptionMessage()); }
    return juce::Result::ok();
}
}
