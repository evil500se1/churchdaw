#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ChurchOrganDesignTokens.h"
#include "ChurchOrganViewModel.h"

namespace churchdaw::organ
{
class ChurchOrganLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ChurchOrganLookAndFeel();

    void drawButtonBackground (juce::Graphics&,
                               juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&,
                         juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    void drawRotarySlider (juce::Graphics&,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPosProportional,
                           float rotaryStartAngle,
                           float rotaryEndAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           juce::Slider::SliderStyle,
                           juce::Slider&) override;
};

class ChurchOrganPanel final : public juce::Component
{
public:
    ChurchOrganPanel (juce::String title,
                      juce::String detail = {},
                      bool instrumentSurface = false);

    void setDetail (juce::String);
    juce::Rectangle<int> getContentBounds() const noexcept;

    void paint (juce::Graphics&) override;

private:
    juce::String title;
    juce::String detail;
    bool instrumentSurface = false;
};

class ChurchOrganDrawbar final : public juce::Slider
{
public:
    ChurchOrganDrawbar (int index, juce::String footage);

    void setMidiFeedback (bool active, bool waitingForPickup);

    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::String getTextFromValue (double value) override;

private:
    int drawbarIndex = 0;
    juce::String footage;
    bool midiActive = false;
    bool pickupWaiting = false;
};

class ChurchOrganDrawbarBank final : public juce::Component
{
public:
    ChurchOrganDrawbarBank();

    void setValues (const std::array<int, 9>& values,
                    juce::NotificationType notification = juce::dontSendNotification);
    std::array<int, 9> getValues() const;

    void setMidiFeedback (int drawbarIndex, bool waitingForPickup);
    void setTwoRowLayout (bool shouldUseTwoRows);
    bool isUsingTwoRows() const noexcept { return useTwoRows; }

    juce::Rectangle<int> getDrawbarBounds (int index) const;

    std::function<void (int index, int value)> onDrawbarChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::array<std::unique_ptr<ChurchOrganDrawbar>, 9> drawbars;
    bool useTwoRows = false;
    int midiIndex = -1;
    bool midiPickup = false;
};

class ChurchOrganParameterSlider final : public juce::Slider
{
public:
    enum class Presentation
    {
        rotary,
        horizontal
    };

    ChurchOrganParameterSlider (juce::String accessibleName,
                                Presentation presentation = Presentation::rotary,
                                juce::String suffix = "%");

    void setNormalisedValue (float value,
                             juce::NotificationType notification = juce::dontSendNotification);
    float getNormalisedValue() const noexcept;

private:
    juce::String suffix;
};

class ChurchOrganSegmentedControl final : public juce::Component
{
public:
    ChurchOrganSegmentedControl (juce::String accessibleName,
                                 std::vector<juce::String> options);

    void setSelectedIndex (int index,
                           juce::NotificationType notification = juce::dontSendNotification);
    int getSelectedIndex() const noexcept { return selectedIndex; }
    int getNumOptions() const noexcept { return static_cast<int> (buttons.size()); }
    juce::Rectangle<int> getOptionBounds (int index) const;

    std::function<void (int index)> onSelectionChanged;

    void resized() override;

private:
    juce::String accessibleName;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
    int selectedIndex = 0;
};

class ChurchOrganRotaryVisualizer final : public juce::Component
{
public:
    ChurchOrganRotaryVisualizer();

    void setMode (ChurchOrganRotaryMode);
    void setBalance (float hornLevel, float drumLevel);
    void paint (juce::Graphics&) override;

private:
    ChurchOrganRotaryMode mode = ChurchOrganRotaryMode::slow;
    float horn = 0.68f;
    float drum = 0.61f;
};

class ChurchOrganWorshipLayers final : public juce::Component
{
public:
    ChurchOrganWorshipLayers();

    void setIntensity (float normalisedIntensity);
    void paint (juce::Graphics&) override;

private:
    float intensity = 0.42f;
};
} // namespace churchdaw::organ
