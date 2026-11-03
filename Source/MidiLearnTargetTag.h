#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

// Pequena ponte entre a interface e o MIDI Learn rapido. SliderParameterAttachment
// sabe qual parametro controla, mas nao expoe essa informacao quando o usuario
// apenas clica (sem alterar o valor). A tag fica no proprio componente e permite
// que o MainComponent arme exatamente esse parametro ou acao no clique.
namespace ChurchDAWMidiLearnTarget
{
    inline const juce::Identifier processorPointerProperty {
        "churchdawMidiLearnProcessor"
    };
    inline const juce::Identifier parameterIndexProperty {
        "churchdawMidiLearnParameterIndex"
    };
    inline const juce::Identifier actionKindProperty {
        "churchdawMidiLearnActionKind"
    };
    inline const juce::Identifier actionArgProperty {
        "churchdawMidiLearnActionArg"
    };
    inline const juce::Identifier displayNameProperty {
        "churchdawMidiLearnDisplayName"
    };
    inline const juce::Identifier slotIndexProperty {
        "churchdawMidiLearnSlotIndex"
    };
    inline const juce::Identifier nodeIdProperty {
        "churchdawMidiLearnNodeId"
    };

    inline void tag (juce::Component& component,
                     juce::AudioProcessor& processor,
                     juce::AudioProcessorParameter& parameter,
                     const juce::String& displayName = {},
                     int slotIndex = -1,
                     juce::uint32 nodeId = 0)
    {
        component.getProperties().set (
            processorPointerProperty,
            static_cast<juce::int64> (
                reinterpret_cast<std::intptr_t> (&processor)));
        component.getProperties().set (parameterIndexProperty,
                                       parameter.getParameterIndex());
        if (slotIndex >= 0)
            component.getProperties().set (slotIndexProperty, slotIndex);
        if (nodeId != 0)
            component.getProperties().set (nodeIdProperty, static_cast<juce::int64> (nodeId));
        if (displayName.isNotEmpty())
            component.getProperties().set (displayNameProperty, displayName);
    }

    inline void tagAction (juce::Component& component,
                           int actionKind,
                           int actionArg = -1,
                           const juce::String& displayName = {})
    {
        component.getProperties().set (actionKindProperty, actionKind);
        component.getProperties().set (actionArgProperty, actionArg);
        if (displayName.isNotEmpty())
            component.getProperties().set (displayNameProperty, displayName);
    }

    inline void clear (juce::Component& component)
    {
        component.getProperties().remove (processorPointerProperty);
        component.getProperties().remove (parameterIndexProperty);
        component.getProperties().remove (slotIndexProperty);
        component.getProperties().remove (nodeIdProperty);
        component.getProperties().remove (actionKindProperty);
        component.getProperties().remove (actionArgProperty);
        component.getProperties().remove (displayNameProperty);
    }

    inline bool readSlotInfo (juce::Component& component,
                              int& slotIndex,
                              juce::uint32& nodeId) noexcept
    {
        const auto* properties = &component.getProperties();
        slotIndex = properties->contains (slotIndexProperty)
            ? static_cast<int> ((*properties)[slotIndexProperty]) : -1;
        nodeId = properties->contains (nodeIdProperty)
            ? static_cast<juce::uint32> (static_cast<juce::int64> ((*properties)[nodeIdProperty])) : 0;
        return slotIndex >= 0 || nodeId != 0;
    }

    inline bool read (juce::Component& component,
                      juce::AudioProcessor*& processor,
                      int& parameterIndex) noexcept
    {
        const auto* properties = &component.getProperties();

        if (! properties->contains (processorPointerProperty)
             || ! properties->contains (parameterIndexProperty))
            return false;

        const auto address = static_cast<std::intptr_t> (
            static_cast<juce::int64> ((*properties)[processorPointerProperty]));
        processor = reinterpret_cast<juce::AudioProcessor*> (address);
        parameterIndex = static_cast<int> (
            (*properties)[parameterIndexProperty]);
        return processor != nullptr && parameterIndex >= 0;
    }

    inline bool readAction (juce::Component& component,
                            int& actionKind,
                            int& actionArg,
                            juce::String& displayName) noexcept
    {
        const auto* properties = &component.getProperties();

        if (! properties->contains (actionKindProperty))
            return false;

        actionKind = static_cast<int> ((*properties)[actionKindProperty]);
        actionArg = properties->contains (actionArgProperty)
            ? static_cast<int> ((*properties)[actionArgProperty]) : -1;
        displayName = properties->getWithDefault (displayNameProperty, juce::var()).toString();
        return actionKind > 0;
    }

    inline juce::String readDisplayName (juce::Component& component) noexcept
    {
        const auto* properties = &component.getProperties();
        if (properties->contains (displayNameProperty))
            return (*properties)[displayNameProperty].toString();
        return {};
    }
}
