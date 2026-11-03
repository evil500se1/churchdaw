#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>
#include <cmath>
#include <memory>
#include <vector>

// Only completed GUI gestures enter this history. MIDI automation, initial
// restores and audio-thread gestures must never fill it or allocate from RT.
class ParameterEditHistory
{
public:
    explicit ParameterEditHistory (const juce::Array<juce::AudioProcessorParameter*>& parameters)
    {
        for (auto* parameter : parameters)
            if (parameter != nullptr) watches.push_back (std::make_unique<Watch> (*this, *parameter));
    }
    std::function<void()> onChange;
    bool canUndo() const { return history.canUndo(); }
    bool canRedo() const { return history.canRedo(); }
    void undo() { history.undo(); if (onChange) onChange(); }
    void redo() { history.redo(); if (onChange) onChange(); }
    void clear() { history.clearUndoHistory(); if (onChange) onChange(); }
private:
    struct Edit final : juce::UndoableAction
    {
        Edit (ParameterEditHistory& o, juce::AudioProcessorParameter& p, float a, float b)
            : owner (o), parameter (p), before (a), after (b) {}
        bool perform() override { if (first) { first = false; return true; } return apply (after); }
        bool undo() override { return apply (before); }
        bool apply (float value)
        {
            const juce::ScopedValueSetter<bool> guard (owner.replaying, true);
            parameter.beginChangeGesture();
            parameter.setValueNotifyingHost (value);
            parameter.endChangeGesture();
            return true;
        }
        ParameterEditHistory& owner;
        juce::AudioProcessorParameter& parameter;
        float before, after;
        bool first = true;
    };
    struct Watch final : juce::AudioProcessorParameter::Listener
    {
        Watch (ParameterEditHistory& o, juce::AudioProcessorParameter& p) : owner (o), parameter (p)
        { parameter.addListener (this); }
        ~Watch() override { parameter.removeListener (this); }
        void parameterValueChanged (int, float) override {}
        void parameterGestureChanged (int, bool starting) override
        {
            const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
            if (messages == nullptr || ! messages->isThisTheMessageThread() || owner.replaying) return;
            if (starting) { before = parameter.getValue(); armed = true; return; }
            if (! armed) return;
            armed = false;
            const auto after = parameter.getValue();
            if (! std::isfinite (after) || std::abs (after - before) < 0.000001f) return;
            owner.history.beginNewTransaction (parameter.getName (80));
            owner.history.perform (new Edit (owner, parameter, before, after));
            if (owner.onChange) owner.onChange();
        }
        ParameterEditHistory& owner;
        juce::AudioProcessorParameter& parameter;
        float before = 0;
        bool armed = false;
    };
    bool replaying = false;
    juce::UndoManager history { 500, 0 };
    std::vector<std::unique_ptr<Watch>> watches;
};
