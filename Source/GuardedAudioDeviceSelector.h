#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>

namespace ChurchDAWAudioUI
{
inline int bindOutputTest (juce::Component& parent, const std::function<void()>& callback)
{
    int matches = 0;
    constexpr auto marker = "churchdaw-protected-output-test";
    for (auto* child : parent.getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            if (button->getComponentID() == marker
                || (button->getButtonText() == juce::translate ("Test")
                    && button->getTooltip() == juce::translate ("Plays a test tone")))
            {
                button->setComponentID (marker);
                button->onClick = callback;
                ++matches;
            }
        matches += bindOutputTest (*child, callback);
    }
    return matches;
}
}

class GuardedAudioDeviceSelector final : public juce::AudioDeviceSelectorComponent
{
public:
    GuardedAudioDeviceSelector (juce::AudioDeviceManager& manager,
                               std::function<void()> outputTest)
        : AudioDeviceSelectorComponent (
              manager,
              0,
             #if JUCE_ANDROID || JUCE_IOS
              0,
             #else
              2,
             #endif
              0, 2, true, false, true, false),
          onOutputTest (std::move (outputTest))
    {
        ChurchDAWAudioUI::bindOutputTest (*this, onOutputTest);
    }
    void resized() override
    {
        AudioDeviceSelectorComponent::resized();
        // JUCE updateAllControls ends in virtual resized(), including after
        // driver-panel recreation. Bind synchronously before another click,
        // rather than waiting for the holder's cosmetic 700 ms refresh timer.
        ChurchDAWAudioUI::bindOutputTest (*this, onOutputTest);
    }
private:
    std::function<void()> onOutputTest;
};
