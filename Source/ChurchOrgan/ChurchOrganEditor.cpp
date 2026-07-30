#include "ChurchOrganEditor.h"

#include <algorithm>
#include <array>
#include <utility>

#include "ChurchOrganDesignTokens.h"
#include "UI/ChurchOrganControls.h"

namespace churchdaw::organ
{
namespace
{
using Tokens = ChurchOrganDesignTokens;
using Page = ChurchOrganLayout::Page;
using LiveCard = ChurchOrganLayout::LiveCard;
using Parameter = ChurchOrganParameter;
using Presentation = ChurchOrganParameterSlider::Presentation;

constexpr int pageIndex (Page page) noexcept
{
    return static_cast<int> (page);
}

constexpr int cardIndex (LiveCard card) noexcept
{
    return static_cast<int> (card);
}

void setButtonTextIfDifferent (juce::TextButton& button, const juce::String& text)
{
    if (button.getButtonText() != text)
        button.setButtonText (text);
}

int rotaryModeIndex (ChurchOrganRotaryMode mode) noexcept
{
    switch (mode)
    {
        case ChurchOrganRotaryMode::stop:  return 0;
        case ChurchOrganRotaryMode::slow:  return 1;
        case ChurchOrganRotaryMode::fast:  return 2;
        case ChurchOrganRotaryMode::brake: return 3;
    }

    return 0;
}

ChurchOrganRotaryMode rotaryModeFromIndex (int index) noexcept
{
    switch (index)
    {
        case 1:  return ChurchOrganRotaryMode::slow;
        case 2:  return ChurchOrganRotaryMode::fast;
        case 3:  return ChurchOrganRotaryMode::brake;
        default: return ChurchOrganRotaryMode::stop;
    }
}

juce::String qualityName (ChurchOrganQuality quality)
{
    switch (quality)
    {
        case ChurchOrganQuality::eco:      return "ECO";
        case ChurchOrganQuality::standard: return "STANDARD";
        case ChurchOrganQuality::ultra:    return "ULTRA";
    }

    return {};
}

juce::String qualityBadgeName (ChurchOrganQuality quality)
{
    return quality == ChurchOrganQuality::standard ? "STD"
                                                   : qualityName (quality);
}

ChurchOrganQuality nextQuality (ChurchOrganQuality quality) noexcept
{
    switch (quality)
    {
        case ChurchOrganQuality::eco:      return ChurchOrganQuality::standard;
        case ChurchOrganQuality::standard: return ChurchOrganQuality::ultra;
        case ChurchOrganQuality::ultra:    return ChurchOrganQuality::eco;
    }

    return ChurchOrganQuality::standard;
}

class HeaderComponent final : public juce::Component
{
public:
    explicit HeaderComponent (ChurchOrganParameterSink& sinkToUse)
        : sink (sinkToUse)
    {
        configureButton (presetButton,
                         "Current organ preset",
                         "Open the preset browser");
        configureButton (midiButton,
                         "MIDI status",
                         "MIDI input state");
        configureButton (qualityButton,
                         "Engine quality",
                         "Cycle engine quality between Eco, Standard and Ultra");
        configureButton (powerButton,
                         "Organ engine power",
                         "Enable or bypass the organ engine");
        cpuAccessibility.setTitle ("CPU usage");
        cpuAccessibility.setDescription ("Organ engine CPU usage");
        cpuAccessibility.setJustificationType (juce::Justification::centred);
        cpuAccessibility.setColour (juce::Label::textColourId,
                                    juce::Colours::transparentBlack);
        cpuAccessibility.setInterceptsMouseClicks (false, false);
        cpuAccessibility.setAccessible (true);
        addChildren();

        presetButton.onClick = [this]
        {
            if (onOpenPresets != nullptr)
                onOpenPresets();
        };
        qualityButton.onClick = [this] { sink.setQuality (nextQuality (quality)); };
        powerButton.onClick = [this] { sink.setEngineEnabled (! engineEnabled); };

        // MIDI is a status, but remains keyboard-readable and supplies a future
        // Learn entry point without changing geometry.
        midiButton.onClick = [this]
        {
            midiButton.setDescription (midiConnected
                                           ? "MIDI input connected and ready"
                                           : "No MIDI input connected");
        };
    }

    std::function<void()> onOpenPresets;

    void setSizeClass (ChurchOrganLayout::SizeClass newClass)
    {
        if (sizeClass == newClass)
            return;

        sizeClass = newClass;
        resized();
        repaint();
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        quality = state.quality;
        engineEnabled = state.engineEnabled;
        midiConnected = state.midiConnected;
        midiActivity = state.midiActivity;
        cpuPercent = juce::jlimit (0.0f, 100.0f, state.cpuPercent);
        currentPresetName = state.presetName;
        currentPresetCategory = state.presetCategory;

        updateResponsiveButtonTexts();
        presetButton.setTitle ("Current preset: " + state.presetName);
        presetButton.setDescription ("Open Presets. Category "
                                     + state.presetCategory);

        midiButton.setToggleState (state.midiActivity,
                                   juce::dontSendNotification);
        midiButton.setTitle (state.midiConnected
                                 ? "MIDI connected"
                                 : "MIDI disconnected");

        qualityButton.setTitle ("Quality " + qualityName (quality));

        powerButton.setToggleState (engineEnabled,
                                    juce::dontSendNotification);
        powerButton.setTitle (engineEnabled
                                  ? "Organ engine enabled"
                                  : "Organ engine bypassed");
        const auto cpuText = "CPU " + juce::String (juce::roundToInt (cpuPercent))
                           + " percent";
        cpuAccessibility.setText (cpuText, juce::dontSendNotification);
        cpuAccessibility.setDescription (cpuText);
        repaint (cpuBounds);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        juce::ColourGradient background (Tokens::surface2().brighter (0.04f),
                                         0.0f, bounds.getY(),
                                         Tokens::appBackground(),
                                         0.0f, bounds.getBottom(),
                                         false);
        g.setGradientFill (background);
        g.fillRect (bounds);
        g.setColour (Tokens::churchRed().withAlpha (0.78f));
        g.fillRect (0.0f, bounds.getBottom() - 2.0f,
                    bounds.getWidth(), 2.0f);

        drawMark (g, markBounds.toFloat());

        if (! compactHeader())
        {
            g.setColour (Tokens::text());
            g.setFont (Tokens::wordmarkFont (20.0f));
            g.drawText ("Church", wordmarkBounds,
                        juce::Justification::centredLeft);

            const int churchWidth = juce::roundToInt (
                juce::GlyphArrangement::getStringWidth (
                    Tokens::wordmarkFont (20.0f), "Church"));
            auto dawBounds = wordmarkBounds.withTrimmedLeft (churchWidth);
            g.setColour (Tokens::churchRed());
            g.drawText ("DAW", dawBounds,
                        juce::Justification::centredLeft);

            g.setColour (Tokens::textSecondary());
            g.setFont (Tokens::stageLabelFont (10.0f));
            g.drawText ("CHURCH ORGAN ENGINE",
                        engineNameBounds,
                        juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (Tokens::text());
            g.setFont (Tokens::wordmarkFont (12.5f));
            g.drawFittedText ("ChurchDAW", wordmarkBounds,
                              juce::Justification::centredLeft, 1);
        }

        if (! cpuBounds.isEmpty())
        {
            Tokens::fillCard (g, cpuBounds.toFloat(),
                              Tokens::elevatedSurface(),
                              Tokens::radiusControl);
            auto cpuTextBounds = cpuBounds;
            g.setColour (Tokens::textSecondary());
            g.setFont (Tokens::stageLabelFont (compactHeader() ? 7.0f : 8.0f));
            g.drawText ("CPU",
                        cpuTextBounds.removeFromTop (cpuTextBounds.getHeight() / 2),
                        juce::Justification::centred);
            g.setColour (cpuPercent >= 80.0f ? Tokens::error()
                                             : Tokens::okGreen());
            g.setFont (Tokens::valueFont (compactHeader() ? 9.0f : 11.0f));
            g.drawText (juce::String (juce::roundToInt (cpuPercent)) + "%",
                        cpuTextBounds,
                        juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (compactHeader() ? 5 : 10, 5);
        const int gap = compactHeader() ? 4 : 7;
        const int target = compactHeader() ? 40 : 52;

        markBounds = area.removeFromLeft (compactHeader() ? 34 : 48)
                         .withSizeKeepingCentre (compactHeader() ? 30 : 42,
                                                 compactHeader() ? 30 : 42);
        area.removeFromLeft (gap);

        if (compactHeader())
        {
            const bool roomyCompact = getWidth() >= 600;
            wordmarkBounds = area.removeFromLeft (
                roomyCompact ? 78 : juce::jlimit (52, 78, getWidth() / 5));
            engineNameBounds = {};

            const int powerWidth = roomyCompact ? 52 : juce::jmax (40, target);
            powerButton.setBounds (area.removeFromRight (powerWidth));
            area.removeFromRight (gap);
            const int cpuWidth = roomyCompact ? 48 : juce::jmax (34, target - 8);
            cpuBounds = area.removeFromRight (cpuWidth);
            cpuAccessibility.setBounds (cpuBounds);
            area.removeFromRight (gap);
            qualityButton.setBounds (
                area.removeFromRight (roomyCompact ? 58
                                                  : juce::jmax (40, target)));
            area.removeFromRight (gap);
            midiButton.setBounds (
                area.removeFromRight (roomyCompact ? 72
                                                  : juce::jmax (40, target)));
            area.removeFromRight (gap);
            presetButton.setBounds (area);
        }
        else
        {
            wordmarkBounds = area.removeFromLeft (132);
            area.removeFromLeft (18);
            engineNameBounds = area.removeFromLeft (
                juce::jlimit (130, 210, getWidth() / 6));
            area.removeFromLeft (gap);

            powerButton.setBounds (area.removeFromRight (82));
            area.removeFromRight (gap);
            cpuBounds = area.removeFromRight (58);
            cpuAccessibility.setBounds (cpuBounds);
            area.removeFromRight (gap);
            qualityButton.setBounds (area.removeFromRight (112));
            area.removeFromRight (gap);
            midiButton.setBounds (area.removeFromRight (96));
            area.removeFromRight (gap);
            presetButton.setBounds (area);
        }

        updateResponsiveButtonTexts();
    }

private:
    static void configureButton (juce::TextButton& button,
                                 const juce::String& title,
                                 const juce::String& description)
    {
        button.setTitle (title);
        button.setDescription (description);
        button.setTooltip (description);
        button.setWantsKeyboardFocus (true);
        button.setClickingTogglesState (false);
        button.setButtonText (title);
    }

    void addChildren()
    {
        addAndMakeVisible (presetButton);
        addAndMakeVisible (midiButton);
        addAndMakeVisible (qualityButton);
        addAndMakeVisible (powerButton);
        addAndMakeVisible (cpuAccessibility);
    }

    bool compactHeader() const noexcept
    {
        return sizeClass == ChurchOrganLayout::SizeClass::compactPortrait
            || sizeClass == ChurchOrganLayout::SizeClass::compactLandscape
            || (sizeClass == ChurchOrganLayout::SizeClass::medium
                && getWidth() < 900);
    }

    void updatePresetButtonText()
    {
        if (currentPresetName.isEmpty())
            return;

        const auto presetText =
            compactHeader()
                ? (getWidth() <= 340
                       ? currentPresetName.upToFirstOccurrenceOf (
                           " ", false, false)
                       : currentPresetName)
                : currentPresetCategory.toUpperCase()
                    + "  " + currentPresetName;
        setButtonTextIfDifferent (presetButton, presetText);
    }

    void updateResponsiveButtonTexts()
    {
        updatePresetButtonText();
        setButtonTextIfDifferent (
            midiButton,
            compactHeader()
                ? (midiActivity ? "CC"
                   : midiConnected ? "MIDI" : "OFF")
                : (midiActivity ? "MIDI CC"
                   : midiConnected ? "MIDI READY" : "MIDI OFF"));
        setButtonTextIfDifferent (
            qualityButton,
            compactHeader()
                ? qualityBadgeName (quality).substring (0, 3)
                : qualityBadgeName (quality));
        setButtonTextIfDifferent (
            powerButton,
            compactHeader()
                ? (engineEnabled ? "ON" : "OFF")
                : (engineEnabled ? "POWER ON" : "BYPASS"));
    }

    static void drawMark (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float radius = juce::jmin (8.0f, area.getWidth() * 0.22f);
        g.setColour (Tokens::churchRedPressed());
        g.fillRoundedRectangle (area, radius);
        g.setColour (Tokens::churchRed().brighter (0.25f));
        g.drawRoundedRectangle (area.reduced (0.5f), radius, 1.0f);

        const auto centre = area.getCentre();
        const float unit = area.getWidth() / 11.0f;
        g.setColour (Tokens::text());
        g.fillRoundedRectangle (centre.x - unit * 0.55f,
                                area.getY() + unit * 1.35f,
                                unit * 1.1f,
                                unit * 4.1f,
                                unit * 0.3f);
        g.fillRoundedRectangle (centre.x - unit * 2.0f,
                                area.getY() + unit * 2.35f,
                                unit * 4.0f,
                                unit * 1.0f,
                                unit * 0.3f);

        auto keys = area.reduced (unit * 2.0f)
                        .withTrimmedTop (area.getHeight() * 0.45f);
        g.setColour (Tokens::text().withAlpha (0.92f));
        g.fillRoundedRectangle (keys, unit * 0.35f);
        g.setColour (Tokens::surface1());
        for (int key = 1; key < 4; ++key)
            g.drawVerticalLine (
                juce::roundToInt (keys.getX() + key * keys.getWidth() / 4.0f),
                keys.getY(), keys.getBottom());
    }

    ChurchOrganParameterSink& sink;
    ChurchOrganLayout::SizeClass sizeClass =
        ChurchOrganLayout::SizeClass::expanded;
    ChurchOrganQuality quality = ChurchOrganQuality::standard;
    bool engineEnabled = true;
    bool midiConnected = true;
    bool midiActivity = false;
    float cpuPercent = 0.0f;
    juce::String currentPresetName;
    juce::String currentPresetCategory;

    juce::TextButton presetButton;
    juce::TextButton midiButton;
    juce::TextButton qualityButton;
    juce::TextButton powerButton;
    juce::Label cpuAccessibility;

    juce::Rectangle<int> markBounds;
    juce::Rectangle<int> wordmarkBounds;
    juce::Rectangle<int> engineNameBounds;
    juce::Rectangle<int> cpuBounds;
};

class ParameterCell final : public juce::Component
{
public:
    ParameterCell (ChurchOrganParameterSink& sinkToUse,
                   juce::String name,
                   Parameter parameterToUse,
                   Presentation presentation)
        : sink (sinkToUse),
          parameter (parameterToUse),
          label (name, name),
          slider (name, presentation)
    {
        label.setJustificationType (juce::Justification::centred);
        label.setFont (Tokens::stageLabelFont (9.0f));
        label.setColour (juce::Label::textColourId, Tokens::textSecondary());
        label.setAccessible (false);
        addAndMakeVisible (label);
        addAndMakeVisible (slider);

        slider.onValueChange = [this]
        {
            sink.setParameter (parameter, slider.getNormalisedValue());
        };
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        slider.setNormalisedValue (state.value (parameter));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (18));
        slider.setBounds (area);
    }

private:
    ChurchOrganParameterSink& sink;
    Parameter parameter;
    juce::Label label;
    ChurchOrganParameterSlider slider;
};

struct ParameterSpec
{
    juce::String name;
    Parameter parameter;
    Presentation presentation = Presentation::rotary;
};

class ParameterGroup final : public juce::Component
{
public:
    ParameterGroup (ChurchOrganParameterSink& sink,
                    juce::String title,
                    juce::String detail,
                    std::vector<ParameterSpec> specs)
        : panel (std::move (title), std::move (detail))
    {
        addAndMakeVisible (panel);
        for (auto& spec : specs)
        {
            auto cell = std::make_unique<ParameterCell> (
                sink, std::move (spec.name), spec.parameter, spec.presentation);
            addAndMakeVisible (*cell);
            cells.push_back (std::move (cell));
        }
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        for (auto& cell : cells)
            cell->refresh (state);
    }

    void resized() override
    {
        panel.setBounds (getLocalBounds());
        auto body = panel.getContentBounds();
        const int count = static_cast<int> (cells.size());
        if (count == 0)
            return;

        int columns = getWidth() < 360 ? 2
                      : getWidth() < 580 ? juce::jmin (3, count)
                                         : juce::jmin (4, count);
        if (cells.front()->getHeight() > 0 && getHeight() < 150)
            columns = juce::jmin (count, 4);

        const int rows = (count + columns - 1) / columns;
        constexpr int gap = 7;
        const int cellWidth = std::max (1,
            (body.getWidth() - gap * (columns - 1)) / columns);
        const int availableCellHeight = std::max (1,
            (body.getHeight() - gap * (rows - 1)) / rows);
        const int cellHeight = std::min (availableCellHeight, 210);
        const int gridHeight = cellHeight * rows + gap * (rows - 1);
        if (gridHeight < body.getHeight())
        {
            body.setY (body.getCentreY() - gridHeight / 2);
            body.setHeight (gridHeight);
        }

        for (int index = 0; index < count; ++index)
        {
            const int row = index / columns;
            const int column = index % columns;
            int width = cellWidth;
            if (column == columns - 1)
                width = body.getRight()
                      - (body.getX() + column * (cellWidth + gap));
            cells[static_cast<size_t> (index)]->setBounds (
                body.getX() + column * (cellWidth + gap),
                body.getY() + row * (cellHeight + gap),
                width,
                cellHeight);
        }
    }

private:
    ChurchOrganPanel panel;
    std::vector<std::unique_ptr<ParameterCell>> cells;
};

class LivePerformanceCard final : public juce::Component
{
public:
    LivePerformanceCard (LiveCard cardToUse,
                         ChurchOrganParameterSink& sinkToUse)
        : card (cardToUse),
          sink (sinkToUse),
          panel (titleFor (cardToUse), {})
    {
        addAndMakeVisible (panel);
        createControls();
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        switch (card)
        {
            case LiveCard::percussion:
                powerButton->setToggleState (state.percussionEnabled,
                                             juce::dontSendNotification);
                setButtonTextIfDifferent (*powerButton,
                                          state.percussionEnabled ? "ON" : "OFF");
                segmented->setSelectedIndex (state.percussionHarmonic == 2 ? 0 : 1);
                panel.setDetail (state.percussionEnabled
                                     ? juce::String (state.percussionHarmonic) + "nd/rd"
                                     : "Off");
                break;

            case LiveCard::chorusVibrato:
            {
                const int compactIndex = state.chorusMode == 0 ? 0
                                       : state.chorusMode <= 2 ? 1
                                       : state.chorusMode <= 5 ? 2 : 3;
                segmented->setSelectedIndex (compactIndex);
                panel.setDetail (state.chorusMode == 0 ? "Scanner off" : "Scanner active");
                break;
            }

            case LiveCard::drive:
                slider->setNormalisedValue (state.value (Parameter::drive));
                panel.setDetail (state.value (Parameter::drive) < 0.35f
                                     ? "Warm valve" : "Driven amp");
                break;

            case LiveCard::rotary:
                segmented->setSelectedIndex (rotaryModeIndex (state.rotaryMode));
                panel.setDetail (rotaryModeIndex (state.rotaryMode) == 2 ? "Fast"
                                 : rotaryModeIndex (state.rotaryMode) == 1 ? "Slow"
                                 : rotaryModeIndex (state.rotaryMode) == 3 ? "Brake"
                                                                          : "Stop");
                break;

            case LiveCard::expression:
                slider->setNormalisedValue (state.value (Parameter::expression));
                panel.setDetail (juce::String (juce::roundToInt (
                                     state.value (Parameter::expression) * 100.0f)) + "%");
                break;

            case LiveCard::worship:
                slider->setNormalisedValue (state.value (Parameter::worshipIntensity));
                layers->setIntensity (state.value (Parameter::worshipIntensity));
                panel.setDetail (juce::String (juce::roundToInt (
                                     state.value (Parameter::worshipIntensity) * 100.0f)) + "%");
                break;

            case LiveCard::reverb:
                slider->setNormalisedValue (state.value (Parameter::reverb));
                panel.setDetail (state.value (Parameter::reverb) < 0.4f
                                     ? "Chapel" : "Wide space");
                break;

            case LiveCard::count:
                break;
        }
    }

    void resized() override
    {
        panel.setBounds (getLocalBounds());
        auto body = panel.getContentBounds();

        if (powerButton != nullptr && segmented != nullptr)
        {
            const int buttonWidth = juce::jmin (58, body.getWidth() / 3);
            powerButton->setBounds (body.removeFromLeft (buttonWidth));
            body.removeFromLeft (5);
            segmented->setBounds (body);
            return;
        }

        if (segmented != nullptr)
        {
            segmented->setBounds (body);
            return;
        }

        if (slider != nullptr && layers != nullptr)
        {
            const int layerHeight = juce::jmin (30, body.getHeight() / 3);
            layers->setBounds (body.removeFromBottom (layerHeight));
            body.removeFromBottom (juce::jmin (4, body.getHeight()));
            slider->setBounds (body);
            return;
        }

        if (slider != nullptr)
        {
            if (card == LiveCard::drive
                || card == LiveCard::reverb)
            {
                const bool compactContinuousControl =
                    getHeight() < 90;
                const auto desiredStyle = compactContinuousControl
                    ? juce::Slider::LinearHorizontal
                    : juce::Slider::RotaryHorizontalVerticalDrag;
                if (slider->getSliderStyle() != desiredStyle)
                {
                    slider->setSliderStyle (desiredStyle);
                    slider->setTextBoxStyle (
                        compactContinuousControl
                            ? juce::Slider::TextBoxRight
                            : juce::Slider::TextBoxBelow,
                        false,
                        compactContinuousControl ? 58 : 62,
                        compactContinuousControl ? 22 : 20);
                }
            }

            if (slider->getSliderStyle() == juce::Slider::LinearHorizontal)
                slider->setBounds (body);
            else
                slider->setBounds (body.withSizeKeepingCentre (
                    juce::jmin (body.getWidth(), 96),
                    juce::jmin (body.getHeight(), 96)));
        }
    }

private:
    static juce::String titleFor (LiveCard card)
    {
        switch (card)
        {
            case LiveCard::percussion:      return "PERCUSSION";
            case LiveCard::chorusVibrato:   return "CHORUS / VIBRATO";
            case LiveCard::drive:           return "DRIVE";
            case LiveCard::rotary:          return "ROTARY";
            case LiveCard::expression:      return "EXPRESSION";
            // "WORSHIP" seco lia como um efeito qualquer; e um MACRO que
            // mistura camadas (Organ/Pad/Air/Strings/Shimmer) — o nome diz.
            case LiveCard::worship:         return "WORSHIP BLEND";
            case LiveCard::reverb:          return "REVERB";
            case LiveCard::count:           break;
        }
        return {};
    }

    void createControls()
    {
        switch (card)
        {
            case LiveCard::percussion:
                powerButton = std::make_unique<juce::TextButton> ("ON");
                powerButton->setTitle ("Percussion power");
                powerButton->setDescription ("Enable or disable harmonic percussion");
                powerButton->setTooltip ("Percussion on or off");
                powerButton->onClick = [this]
                {
                    sink.setPercussionEnabled (! powerButton->getToggleState());
                };
                addAndMakeVisible (*powerButton);

                segmented = std::make_unique<ChurchOrganSegmentedControl> (
                    "Percussion harmonic", std::vector<juce::String> { "2ND", "3RD" });
                segmented->onSelectionChanged = [this] (int index)
                {
                    sink.setPercussionHarmonic (index == 0 ? 2 : 3);
                };
                addAndMakeVisible (*segmented);
                break;

            case LiveCard::chorusVibrato:
                segmented = std::make_unique<ChurchOrganSegmentedControl> (
                    "Scanner chorus and vibrato",
                    std::vector<juce::String> { "OFF", "V1", "C2", "C3" });
                segmented->onSelectionChanged = [this] (int index)
                {
                    static constexpr int map[] { 0, 1, 5, 6 };
                    sink.setChorusMode (map[juce::jlimit (0, 3, index)]);
                };
                addAndMakeVisible (*segmented);
                break;

            case LiveCard::drive:
                createSlider ("Drive", Parameter::drive, Presentation::rotary);
                break;

            case LiveCard::rotary:
                segmented = std::make_unique<ChurchOrganSegmentedControl> (
                    "Rotary speed",
                    std::vector<juce::String> { "STOP", "SLOW", "FAST", "BRAKE" });
                segmented->onSelectionChanged = [this] (int index)
                {
                    sink.setRotaryMode (rotaryModeFromIndex (index));
                };
                addAndMakeVisible (*segmented);
                break;

            case LiveCard::expression:
                createSlider ("Expression", Parameter::expression,
                              Presentation::horizontal);
                break;

            case LiveCard::worship:
                createSlider ("Worship Intensity", Parameter::worshipIntensity,
                              Presentation::horizontal);
                layers = std::make_unique<ChurchOrganWorshipLayers>();
                addAndMakeVisible (*layers);
                break;

            case LiveCard::reverb:
                createSlider ("Reverb", Parameter::reverb, Presentation::rotary);
                break;

            case LiveCard::count:
                break;
        }
    }

    void createSlider (const juce::String& name,
                       Parameter parameter,
                       Presentation presentation)
    {
        slider = std::make_unique<ChurchOrganParameterSlider> (name, presentation);
        slider->onValueChange = [this, parameter]
        {
            sink.setParameter (parameter, slider->getNormalisedValue());
        };
        addAndMakeVisible (*slider);
    }

    LiveCard card;
    ChurchOrganParameterSink& sink;
    ChurchOrganPanel panel;
    std::unique_ptr<juce::TextButton> powerButton;
    std::unique_ptr<ChurchOrganSegmentedControl> segmented;
    std::unique_ptr<ChurchOrganParameterSlider> slider;
    std::unique_ptr<ChurchOrganWorshipLayers> layers;
};

class LivePageComponent final : public juce::Component
{
public:
    explicit LivePageComponent (ChurchOrganParameterSink& sink)
        : drawbarPanel ("DRAWBARS", "HARMONIC ENGINE", true)
    {
        setTitle ("Live performance page");
        setDescription ("No-scroll performance surface");
        setAccessible (true);

        addAndMakeVisible (drawbarPanel);
        addAndMakeVisible (drawbarBank);
        drawbarBank.onDrawbarChanged = [&sink] (int index, int value)
        {
            sink.setDrawbar (index, value);
        };

        for (int index = 0; index < ChurchOrganLayout::liveCardCount; ++index)
        {
            auto card = std::make_unique<LivePerformanceCard> (
                static_cast<LiveCard> (index), sink);
            addAndMakeVisible (*card);
            performanceCards[static_cast<size_t> (index)] = std::move (card);
        }
    }

    void setSizeClass (ChurchOrganLayout::SizeClass newSizeClass)
    {
        if (sizeClass == newSizeClass)
            return;
        sizeClass = newSizeClass;
        resized();
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        drawbarBank.setValues (state.drawbars);
        drawbarBank.setMidiFeedback (state.midiDrawbarIndex,
                                     state.midiPickupWaiting);
        for (auto& card : performanceCards)
            card->refresh (state);
    }

    ChurchOrganLayout::Live getLayout() const noexcept { return liveLayout; }

    juce::Rectangle<int> getDrawbarBounds (int index) const
    {
        return drawbarBank.getDrawbarBounds (index)
            .translated (drawbarBank.getX(), drawbarBank.getY());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Tokens::appBackground());
    }

    void resized() override
    {
        liveLayout = ChurchOrganLayout::calculateLive (getLocalBounds(), sizeClass);
        drawbarPanel.setBounds (liveLayout.drawbarCard);
        drawbarBank.setTwoRowLayout (liveLayout.drawbarsUseTwoRows);
        drawbarBank.setBounds (
            drawbarPanel.getContentBounds().translated (drawbarPanel.getX(),
                                                        drawbarPanel.getY()));

        for (int index = 0; index < ChurchOrganLayout::liveCardCount; ++index)
        {
            auto& card = *performanceCards[static_cast<size_t> (index)];
            card.setVisible (liveLayout.visible[static_cast<size_t> (index)]);
            card.setBounds (liveLayout.cards[static_cast<size_t> (index)]);
        }
    }

private:
    ChurchOrganLayout::SizeClass sizeClass =
        ChurchOrganLayout::SizeClass::expanded;
    ChurchOrganLayout::Live liveLayout;
    ChurchOrganPanel drawbarPanel;
    ChurchOrganDrawbarBank drawbarBank;
    std::array<std::unique_ptr<LivePerformanceCard>,
               ChurchOrganLayout::liveCardCount> performanceCards;
};

class EditingPageBase : public juce::Component
{
public:
    EditingPageBase (juce::String eyebrowToUse,
                     juce::String titleToUse,
                     juce::String descriptionToUse)
        : eyebrow (std::move (eyebrowToUse)),
          title (std::move (titleToUse)),
          description (std::move (descriptionToUse))
    {
        setTitle (title);
        setDescription (description);
        setAccessible (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Tokens::appBackground());
        auto area = getLocalBounds().reduced (pageGutter(), 8);
        auto heading = area.removeFromTop (headingHeight());

        if (compactHeading())
        {
            // Tela baixa (celular em paisagem): titulo enxuto numa LINHA —
            // o cabecalho de 94 px empurrava drawbars, character e presets
            // para baixo da dobra (auditoria de 30/07). A descricao sai; o
            // conteudo e o protagonista.
            g.setColour (Tokens::text());
            g.setFont (Tokens::valueFont (16.0f));
            const float titleW = juce::GlyphArrangement::getStringWidth (
                Tokens::valueFont (16.0f), title);
            g.drawFittedText (title, heading,
                              juce::Justification::centredLeft, 1);
            g.setColour (Tokens::organAmberLight());
            g.setFont (Tokens::stageLabelFont (9.0f));
            g.drawFittedText (eyebrow.toUpperCase(),
                              heading.withTrimmedLeft ((int) titleW + 12),
                              juce::Justification::centredLeft, 1);
            return;
        }

        g.setColour (Tokens::organAmberLight());
        g.setFont (Tokens::stageLabelFont (9.0f));
        g.drawFittedText (eyebrow.toUpperCase(),
                          heading.removeFromTop (18),
                          juce::Justification::centredLeft, 1);
        g.setColour (Tokens::text());
        g.setFont (Tokens::valueFont (getWidth() < 500 ? 24.0f : 30.0f));
        g.drawFittedText (title,
                          heading.removeFromTop (getWidth() < 500 ? 34 : 42),
                          juce::Justification::centredLeft, 1);
        g.setColour (Tokens::textSecondary());
        g.setFont (Tokens::bodyFont (getWidth() < 500 ? 11.0f : 13.0f));
        g.drawFittedText (description, heading,
                          juce::Justification::centredLeft,
                          getWidth() < 500 ? 2 : 1);
    }

protected:
    bool compactHeading() const
    {
        // A pagina vive num Viewport rolavel: getHeight() e a altura do
        // CONTEUDO (sempre grande). O que importa e a JANELA visivel.
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            return vp->getMaximumVisibleHeight() < 460;

        return getHeight() < 460;
    }
    int pageGutter() const noexcept { return getWidth() < 500 ? 8 : 14; }
    int headingHeight() const noexcept
    {
        return compactHeading() ? 26 : (getWidth() < 500 ? 88 : 94);
    }

    juce::Rectangle<int> bodyBounds() const noexcept
    {
        return getLocalBounds().reduced (pageGutter(), 8)
            .withTrimmedTop (headingHeight());
    }

private:
    juce::String eyebrow;
    juce::String title;
    juce::String description;
};

class DrawbarsEditingPage final : public EditingPageBase
{
public:
    explicit DrawbarsEditingPage (ChurchOrganParameterSink& sinkToUse)
        : EditingPageBase ("Controle tonewheel", "Drawbars",
                           juce::String::fromUTF8 ("Molde o espectro harm\xc3\xb4nico com resposta MIDI imediata.")),
          sink (sinkToUse),
          drawbarPanel ("UPPER MANUAL", "9 HARMONICS", true),
          percussionPanel ("PERCUSSION", "TRANSIENT"),
          chorusPanel ("CHORUS / VIBRATO", "SCANNER"),
          harmonic ("Percussion harmonic",
                    std::vector<juce::String> { "2ND", "3RD" }),
          chorus ("Scanner chorus and vibrato",
                  std::vector<juce::String> {
                      "OFF", "V1", "V2", "V3", "C1", "C2", "C3" })
    {
        addAndMakeVisible (drawbarPanel);
        addAndMakeVisible (drawbars);
        addAndMakeVisible (percussionPanel);
        addAndMakeVisible (chorusPanel);
        addAndMakeVisible (percussionPower);
        addAndMakeVisible (harmonic);
        addAndMakeVisible (chorus);

        drawbars.onDrawbarChanged = [this] (int index, int value)
        {
            sink.setDrawbar (index, value);
        };
        percussionPower.setTitle ("Percussion power");
        percussionPower.setTooltip ("Enable or disable harmonic percussion");
        percussionPower.onClick = [this]
        {
            sink.setPercussionEnabled (! percussionPower.getToggleState());
        };
        harmonic.onSelectionChanged = [this] (int index)
        {
            sink.setPercussionHarmonic (index == 0 ? 2 : 3);
        };
        chorus.onSelectionChanged = [this] (int index)
        {
            sink.setChorusMode (index);
        };
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        drawbars.setValues (state.drawbars);
        drawbars.setMidiFeedback (state.midiDrawbarIndex,
                                  state.midiPickupWaiting);
        percussionPower.setToggleState (state.percussionEnabled,
                                        juce::dontSendNotification);
        setButtonTextIfDifferent (percussionPower,
                                  state.percussionEnabled ? "ON" : "OFF");
        harmonic.setSelectedIndex (state.percussionHarmonic == 2 ? 0 : 1);
        chorus.setSelectedIndex (state.chorusMode);
    }

    void resized() override
    {
        auto area = bodyBounds();
        constexpr int gap = 10;
        const bool narrow = getWidth() < 600;
        const int drawbarHeight = narrow
                                    ? juce::jmin (420, area.getHeight() * 3 / 5)
                                    : juce::jmin (370, area.getHeight() / 2 + 40);
        drawbarPanel.setBounds (area.removeFromTop (drawbarHeight));
        drawbars.setTwoRowLayout (getWidth() < 480);
        drawbars.setBounds (
            drawbarPanel.getContentBounds().translated (drawbarPanel.getX(),
                                                        drawbarPanel.getY()));
        area.removeFromTop (std::min (gap, area.getHeight()));

        juce::Rectangle<int> percussionBounds;
        juce::Rectangle<int> chorusBounds;
        if (narrow)
        {
            percussionBounds = area.removeFromTop ((area.getHeight() - gap) / 2);
            area.removeFromTop (std::min (gap, area.getHeight()));
            chorusBounds = area;
        }
        else
        {
            percussionBounds = area.removeFromLeft ((area.getWidth() - gap) / 2);
            area.removeFromLeft (std::min (gap, area.getWidth()));
            chorusBounds = area;
        }

        percussionPanel.setBounds (percussionBounds);
        chorusPanel.setBounds (chorusBounds);

        auto percussionBody =
            percussionPanel.getContentBounds().translated (percussionPanel.getX(),
                                                           percussionPanel.getY());
        percussionPower.setBounds (
            percussionBody.removeFromLeft (juce::jmin (72, percussionBody.getWidth() / 3)));
        percussionBody.removeFromLeft (6);
        harmonic.setBounds (percussionBody);

        chorus.setBounds (
            chorusPanel.getContentBounds().translated (chorusPanel.getX(),
                                                       chorusPanel.getY()));
    }

private:
    ChurchOrganParameterSink& sink;
    ChurchOrganPanel drawbarPanel;
    ChurchOrganDrawbarBank drawbars;
    ChurchOrganPanel percussionPanel;
    ChurchOrganPanel chorusPanel;
    juce::TextButton percussionPower { "ON" };
    ChurchOrganSegmentedControl harmonic;
    ChurchOrganSegmentedControl chorus;
};

class CharacterEditingPage final : public EditingPageBase
{
public:
    explicit CharacterEditingPage (ChurchOrganParameterSink& sinkToUse)
        : EditingPageBase (juce::String::fromUTF8 ("Personalidade do \xc3\xb3rg\xc3\xa3o"), "Character",
                           juce::String::fromUTF8 ("Do desgaste das tonewheels \xc3\xa0 resposta do amplificador.")),
          sink (sinkToUse),
          profiles ("Tonewheel profile",
                    std::vector<juce::String> { "CLEAN", "AGED", "ROAD", "CUSTOM" }),
          tonewheel (sink, "TONEWHEEL", "SOURCE",
                     {
                         { "AGE", Parameter::tonewheelAge },
                         { "TONE", Parameter::tone },
                         { "TRANSFORMER", Parameter::transformer }
                     }),
          contacts (sink, "CONTACTS & LEAKAGE", "ARTICULATION",
                    {
                        { "KEY CLICK", Parameter::keyClick },
                        { "LEAKAGE", Parameter::leakage },
                        { "CROSSTALK", Parameter::crosstalk },
                        { "HUM", Parameter::hum },
                        { "CONTACT", Parameter::contactSpread }
                    }),
          amplifier (sink, "AMPLIFIER & CABINET", "OUTPUT",
                     {
                         { "DRIVE", Parameter::drive },
                         { "AMPLIFIER", Parameter::amplifier },
                         { "CABINET", Parameter::cabinet },
                         { "REVERB", Parameter::reverb }
                     })
    {
        addAndMakeVisible (profiles);
        addAndMakeVisible (tonewheel);
        addAndMakeVisible (contacts);
        addAndMakeVisible (amplifier);
        profiles.onSelectionChanged = [this] (int index)
        {
            sink.setTonewheelProfile (index);
        };
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        profiles.setSelectedIndex (state.tonewheelProfile);
        tonewheel.refresh (state);
        contacts.refresh (state);
        amplifier.refresh (state);
    }

    void resized() override
    {
        auto area = bodyBounds();
        constexpr int gap = 10;
        profiles.setBounds (area.removeFromTop (getWidth() < 500 ? 52 : 58));
        area.removeFromTop (gap);

        if (getWidth() >= 940)
        {
            const int width = (area.getWidth() - gap * 2) / 3;
            tonewheel.setBounds (area.removeFromLeft (width));
            area.removeFromLeft (gap);
            contacts.setBounds (area.removeFromLeft (width));
            area.removeFromLeft (gap);
            amplifier.setBounds (area);
        }
        else
        {
            const int height = (area.getHeight() - gap * 2) / 3;
            tonewheel.setBounds (area.removeFromTop (height));
            area.removeFromTop (gap);
            contacts.setBounds (area.removeFromTop (height));
            area.removeFromTop (gap);
            amplifier.setBounds (area);
        }
    }

private:
    ChurchOrganParameterSink& sink;
    ChurchOrganSegmentedControl profiles;
    ParameterGroup tonewheel;
    ParameterGroup contacts;
    ParameterGroup amplifier;
};

class RotaryStage final : public juce::Component
{
public:
    explicit RotaryStage (ChurchOrganParameterSink& sinkToUse)
        : sink (sinkToUse),
          panel ("ROTARY CABINET", "PERFORMANCE"),
          speeds ("Rotary speed",
                  std::vector<juce::String> { "STOP", "SLOW", "FAST", "BRAKE" })
    {
        addAndMakeVisible (panel);
        addAndMakeVisible (visualizer);
        addAndMakeVisible (speeds);
        speeds.onSelectionChanged = [this] (int index)
        {
            sink.setRotaryMode (rotaryModeFromIndex (index));
        };
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        speeds.setSelectedIndex (rotaryModeIndex (state.rotaryMode));
        visualizer.setMode (state.rotaryMode);
        visualizer.setBalance (state.value (Parameter::hornLevel),
                               state.value (Parameter::drumLevel));
    }

    void resized() override
    {
        panel.setBounds (getLocalBounds());
        auto area = panel.getContentBounds();
        const int selectorHeight = juce::jmin (60, area.getHeight() / 4);
        speeds.setBounds (area.removeFromBottom (selectorHeight));
        area.removeFromBottom (juce::jmin (8, area.getHeight()));
        visualizer.setBounds (area);
    }

private:
    ChurchOrganParameterSink& sink;
    ChurchOrganPanel panel;
    ChurchOrganRotaryVisualizer visualizer;
    ChurchOrganSegmentedControl speeds;
};

class RotaryEditingPage final : public EditingPageBase
{
public:
    explicit RotaryEditingPage (ChurchOrganParameterSink& sink)
        : EditingPageBase ("Motor de movimento", "Rotary",
                           juce::String::fromUTF8 ("Velocidades de performance com controles f\xc3\xadsicos da caixa.")),
          stage (sink),
          parameters (sink, "CABINET, MOTORS & MICS", "ADVANCED",
                      {
                          { "HORN", Parameter::hornLevel },
                          { "DRUM", Parameter::drumLevel },
                          { "ACCEL", Parameter::acceleration },
                          { "DECEL", Parameter::deceleration },
                          { "DISTANCE", Parameter::rotaryDistance },
                          { "ANGLE", Parameter::rotaryAngle },
                          { "WIDTH", Parameter::rotaryWidth },
                          { "ROOM", Parameter::rotaryRoom }
                      })
    {
        addAndMakeVisible (stage);
        addAndMakeVisible (parameters);
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        stage.refresh (state);
        parameters.refresh (state);
    }

    void resized() override
    {
        auto area = bodyBounds();
        constexpr int gap = 10;
        if (getWidth() >= 780)
        {
            stage.setBounds (area.removeFromLeft (
                juce::jmin (420, area.getWidth() * 2 / 5)));
            area.removeFromLeft (gap);
            parameters.setBounds (area);
        }
        else
        {
            stage.setBounds (area.removeFromTop (
                juce::jmin (430, area.getHeight() / 2)));
            area.removeFromTop (gap);
            parameters.setBounds (area);
        }
    }

private:
    RotaryStage stage;
    ParameterGroup parameters;
};

class WorshipMacroCard final : public juce::Component
{
public:
    explicit WorshipMacroCard (ChurchOrganParameterSink& sinkToUse)
        : sink (sinkToUse),
          panel ("WORSHIP INTENSITY", "CHURCHDAW SIGNATURE", true),
          intensity ("Worship Intensity", Presentation::horizontal),
          autoMix ("AUTO MIX")
    {
        addAndMakeVisible (panel);
        addAndMakeVisible (intensity);
        addAndMakeVisible (layers);
        addAndMakeVisible (autoMix);
        intensity.onValueChange = [this]
        {
            sink.setParameter (Parameter::worshipIntensity,
                               intensity.getNormalisedValue());
        };
        autoMix.setTitle ("Automatic worship mix");
        autoMix.setDescription ("Keep the organ central while balancing worship layers");
        autoMix.setTooltip ("Automatic worship layer mixing");
        autoMix.onClick = [this]
        {
            sink.setAutoMixEnabled (! autoMix.getToggleState());
        };
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        const auto value = state.value (Parameter::worshipIntensity);
        intensity.setNormalisedValue (value);
        layers.setIntensity (value);
        autoMix.setToggleState (state.autoMixEnabled,
                                juce::dontSendNotification);
        setButtonTextIfDifferent (autoMix,
                                  state.autoMixEnabled ? "AUTO MIX ON" : "AUTO MIX OFF");
        panel.setDetail (juce::String (juce::roundToInt (value * 100.0f)) + "%");
    }

    void resized() override
    {
        panel.setBounds (getLocalBounds());
        auto area = panel.getContentBounds();
        autoMix.setBounds (area.removeFromTop (juce::jmin (50, area.getHeight() / 4))
                               .removeFromRight (juce::jmin (150, area.getWidth())));
        area.removeFromTop (8);
        layers.setBounds (area.removeFromBottom (juce::jmin (44, area.getHeight() / 4)));
        area.removeFromBottom (8);
        intensity.setBounds (area.withSizeKeepingCentre (
            area.getWidth(), juce::jmin (80, area.getHeight())));
    }

private:
    ChurchOrganParameterSink& sink;
    ChurchOrganPanel panel;
    ChurchOrganParameterSlider intensity;
    ChurchOrganWorshipLayers layers;
    juce::TextButton autoMix;
};

class WorshipEditingPage final : public EditingPageBase
{
public:
    explicit WorshipEditingPage (ChurchOrganParameterSink& sink)
        : EditingPageBase ("Assinatura ChurchDAW", "Worship",
                           juce::String::fromUTF8 ("Um gesto constr\xc3\xb3i camadas, espa\xc3\xa7o e din\xc3\xa2mica ao redor do \xc3\xb3rg\xc3\xa3o.")),
          macro (sink),
          parameters (sink, "WORSHIP LAYER", "SCULPT",
                      {
                          { "PAD", Parameter::worshipPad },
                          { "AIR", Parameter::worshipAir },
                          { "STRINGS", Parameter::worshipStrings },
                          { "SHIMMER", Parameter::worshipShimmer },
                          { "TEXTURE", Parameter::worshipTexture },
                          { "DUCKING", Parameter::worshipDucking },
                          { "ATTACK", Parameter::worshipAttack },
                          { "RELEASE", Parameter::worshipRelease }
                      })
    {
        addAndMakeVisible (macro);
        addAndMakeVisible (parameters);
    }

    void refresh (const ChurchOrganUiSnapshot& state)
    {
        macro.refresh (state);
        parameters.refresh (state);
    }

    void resized() override
    {
        auto area = bodyBounds();
        constexpr int gap = 10;
        if (getWidth() >= 820)
        {
            macro.setBounds (area.removeFromLeft (
                juce::jmin (430, area.getWidth() * 2 / 5)));
            area.removeFromLeft (gap);
            parameters.setBounds (area);
        }
        else
        {
            macro.setBounds (area.removeFromTop (
                juce::jmin (390, area.getHeight() * 2 / 5)));
            area.removeFromTop (gap);
            parameters.setBounds (area);
        }
    }

private:
    WorshipMacroCard macro;
    ParameterGroup parameters;
};

class PresetButton final : public juce::Button
{
public:
    PresetButton() : juce::Button ("Preset")
    {
        setWantsKeyboardFocus (true);
    }

    void setPreset (ChurchOrganPresetSummary newPreset)
    {
        preset = std::move (newPreset);
        setTitle ("Preset " + preset.name);
        setDescription (preset.category + ". " + preset.tags);
        setTooltip ("Load " + preset.name + " with seamless switching");
        repaint();
    }

    const ChurchOrganPresetSummary& getPreset() const noexcept { return preset; }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        auto fill = getToggleState() ? Tokens::surface3()
                    : highlighted ? Tokens::surface2().brighter (0.08f)
                                  : Tokens::surface1();
        if (down)
            fill = fill.brighter (0.12f);
        Tokens::fillCard (g, area, fill, Tokens::radiusCard);

        if (getToggleState())
        {
            g.setColour (Tokens::churchRed());
            g.fillRoundedRectangle (area.withWidth (4.0f),
                                    Tokens::radiusCard);
            g.drawRoundedRectangle (area, Tokens::radiusCard, 1.5f);
        }

        auto textArea = getLocalBounds().reduced (14, 9);
        auto top = textArea.removeFromTop (16);
        g.setColour (Tokens::organAmberLight());
        g.setFont (Tokens::stageLabelFont (9.0f));
        g.drawText (preset.category.toUpperCase(), top,
                    juce::Justification::centredLeft);
        if (preset.favourite)
        {
            g.setColour (Tokens::warning());
            g.drawText ("*", top, juce::Justification::centredRight);
        }

        g.setColour (Tokens::text());
        g.setFont (Tokens::valueFont (14.0f));
        g.drawFittedText (preset.name,
                          textArea.removeFromTop (24),
                          juce::Justification::centredLeft, 1);
        g.setColour (Tokens::textSecondary());
        g.setFont (Tokens::bodyFont (10.0f));
        g.drawFittedText (preset.tags, textArea,
                          juce::Justification::centredLeft, 1);

        if (hasKeyboardFocus (true))
            Tokens::drawFocusRing (g, getLocalBounds().toFloat(),
                                   Tokens::radiusCard);
    }

private:
    ChurchOrganPresetSummary preset;
};

class PresetsEditingPage final : public EditingPageBase
{
public:
    explicit PresetsEditingPage (ChurchOrganParameterSink& sinkToUse)
        : EditingPageBase (juce::String::fromUTF8 ("Mem\xc3\xb3ria de performance"), "Presets",
                           "Encontre e troque sons sem cortar notas sustentadas ou caudas."),
          sink (sinkToUse)
    {
        search.setTitle ("Search presets");
        search.setDescription ("Search by preset name, category or tags");
        search.setTooltip ("Search presets");
        search.setTextToShowWhenEmpty ("Buscar preset, tag ou categoria",
                                       Tokens::textDisabled());
        search.onTextChange = [this] { applyFilter(); };
        addAndMakeVisible (search);

        category.setTitle ("Preset category");
        category.setDescription ("Filter preset category");
        category.setTooltip ("Preset category filter");
        category.addItemList (
            juce::StringArray { "All", "Classic", "Gospel", "Worship",
                                "Pipe", "Transistor", "Favourites" }, 1);
        category.setSelectedId (1);
        category.onChange = [this] { applyFilter(); };
        addAndMakeVisible (category);
    }

    void refresh (const ChurchOrganUiSnapshot& state,
                  std::vector<ChurchOrganPresetSummary> newPresets)
    {
        if (presetButtons.size() != newPresets.size())
        {
            presetButtons.clear();
            for (int index = 0; index < static_cast<int> (newPresets.size()); ++index)
            {
                auto button = std::make_unique<PresetButton>();
                button->onClick = [this, pointer = button.get()]
                {
                    sink.selectPreset (pointer->getPreset().id);
                };
                addAndMakeVisible (*button);
                presetButtons.push_back (std::move (button));
            }
        }

        presets = std::move (newPresets);
        selectedPreset = state.presetId;
        for (int index = 0; index < static_cast<int> (presetButtons.size()); ++index)
        {
            auto& button = *presetButtons[static_cast<size_t> (index)];
            button.setPreset (presets[static_cast<size_t> (index)]);
            button.setToggleState (
                presets[static_cast<size_t> (index)].id == selectedPreset,
                juce::dontSendNotification);
        }
        applyFilter();
    }

    void resized() override
    {
        auto area = bodyBounds();
        constexpr int gap = 9;
        auto toolbar = area.removeFromTop (getWidth() < 500 ? 100 : 50);
        if (getWidth() < 500)
        {
            search.setBounds (toolbar.removeFromTop (46));
            toolbar.removeFromTop (6);
            category.setBounds (toolbar);
        }
        else
        {
            search.setBounds (toolbar.removeFromLeft (toolbar.getWidth() * 2 / 3));
            toolbar.removeFromLeft (gap);
            category.setBounds (toolbar);
        }
        area.removeFromTop (gap);

        const int columns = getWidth() < 620 ? 1 : 2;
        const int visibleCount = static_cast<int> (std::count_if (
            presetButtons.begin(), presetButtons.end(),
            [] (const auto& button) { return button->isVisible(); }));
        const int rows = std::max (1, (visibleCount + columns - 1) / columns);
        const int width = std::max (1, (area.getWidth() - gap * (columns - 1)) / columns);
        const int height = juce::jmax (86,
            (area.getHeight() - gap * (rows - 1)) / rows);

        int visibleIndex = 0;
        for (auto& button : presetButtons)
        {
            if (! button->isVisible())
                continue;
            const int row = visibleIndex / columns;
            const int column = visibleIndex % columns;
            button->setBounds (
                area.getX() + column * (width + gap),
                area.getY() + row * (height + gap),
                column == columns - 1 ? area.getRight()
                                      - (area.getX() + column * (width + gap))
                                      : width,
                height);
            ++visibleIndex;
        }
    }

private:
    void applyFilter()
    {
        const auto query = search.getText().trim().toLowerCase();
        const auto selectedCategory = category.getText();

        for (int index = 0; index < static_cast<int> (presetButtons.size()); ++index)
        {
            const auto& preset = presets[static_cast<size_t> (index)];
            const auto haystack = (preset.name + " " + preset.category
                                   + " " + preset.tags).toLowerCase();
            const bool queryMatches = query.isEmpty() || haystack.contains (query);
            const bool categoryMatches =
                selectedCategory.isEmpty() || selectedCategory == "All"
                || (selectedCategory == "Favourites" && preset.favourite)
                || preset.category.equalsIgnoreCase (selectedCategory);
            presetButtons[static_cast<size_t> (index)]->setVisible (
                queryMatches && categoryMatches);
        }
        resized();
    }

    ChurchOrganParameterSink& sink;
    juce::TextEditor search;
    juce::ComboBox category;
    std::vector<ChurchOrganPresetSummary> presets;
    std::vector<std::unique_ptr<PresetButton>> presetButtons;
    juce::String selectedPreset;
};
} // namespace

struct ChurchOrganEditor::Impl final : public juce::Component
{
    Impl (ChurchOrganEditor& ownerToUse,
          ChurchOrganParameterSink& sinkToUse)
        : owner (ownerToUse),
          sink (sinkToUse),
          header (sink),
          livePage (sink),
          drawbarsPage (sink),
          characterPage (sink),
          rotaryPage (sink),
          worshipPage (sink),
          presetsPage (sink)
    {
        setLookAndFeel (&lookAndFeel);
        setTitle ("Church Organ Engine");
        setDescription ("ChurchDAW organ performance and editing interface");
        setAccessible (true);

        addAndMakeVisible (header);
        header.onOpenPresets = [this] { setPage (Page::presets); };

        for (int index = 0; index < ChurchOrganLayout::pageCount; ++index)
        {
            auto& button = navigationButtons[static_cast<size_t> (index)];
            const auto page = static_cast<Page> (index);
            button.setButtonText (ChurchOrganLayout::pageName (page));
            button.setTitle (ChurchOrganLayout::pageName (page) + " page");
            button.setDescription ("Open "
                                   + ChurchOrganLayout::pageName (page)
                                   + " page. Shortcut "
                                   + juce::String (index + 1));
            button.setTooltip ("Open "
                               + ChurchOrganLayout::pageName (page)
                               + " - key " + juce::String (index + 1));
            button.setWantsKeyboardFocus (true);
            button.onClick = [this, page] { setPage (page); };
            addAndMakeVisible (button);
        }

        addAndMakeVisible (livePage);
        addAndMakeVisible (pageViewport);
        pageViewport.setVisible (false);
        pageViewport.setScrollBarsShown (true, false);
        pageViewport.setScrollOnDragMode (
            juce::Viewport::ScrollOnDragMode::all);
        pageViewport.setWantsKeyboardFocus (false);
        pageViewport.setTitle ("Scrollable advanced organ page");

        seamlessStatus.setJustificationType (juce::Justification::centredLeft);
        seamlessStatus.setFont (Tokens::stageLabelFont (9.5f));
        // Linha fina de rodape, nao um card: a caixa com moldura ocupava
        // area nobre e parecia um painel solto (revisao do dono, item 10).
        seamlessStatus.setColour (juce::Label::backgroundColourId,
                                  juce::Colours::transparentBlack);
        seamlessStatus.setColour (juce::Label::outlineColourId,
                                  juce::Colours::transparentBlack);
        seamlessStatus.setColour (juce::Label::textColourId,
                                  Tokens::okGreen().withAlpha (0.85f));
        seamlessStatus.setTitle ("Seamless switching status");
        seamlessStatus.setDescription ("Organ engine is ready");
        seamlessStatus.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (seamlessStatus);

        setPage (Page::live);
        refreshFromModel();
    }

    ~Impl() override
    {
        pageViewport.setViewedComponent (nullptr, false);
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Tokens::appBackground());
        g.setColour (Tokens::elevatedSurface());
        g.fillRect (shell.navigation);
        g.setColour (Tokens::border());
        if (shell.navigationAtBottom)
            g.drawHorizontalLine (shell.navigation.getY(),
                                  static_cast<float> (shell.navigation.getX()),
                                  static_cast<float> (shell.navigation.getRight()));
        else
            g.drawHorizontalLine (shell.navigation.getBottom() - 1,
                                  static_cast<float> (shell.navigation.getX()),
                                  static_cast<float> (shell.navigation.getRight()));

        if (! lastSnapshot.engineEnabled)
        {
            g.setColour (Tokens::appBackground().withAlpha (0.64f));
            g.fillRect (shell.content);
            auto bypass = shell.content.withSizeKeepingCentre (
                juce::jmin (360, shell.content.getWidth() - 24),
                juce::jmin (74, shell.content.getHeight() - 16));
            Tokens::fillCard (g, bypass.toFloat(),
                              Tokens::elevatedSurface(),
                              Tokens::radiusPanel);
            g.setColour (Tokens::error());
            g.setFont (Tokens::stageLabelFont (12.0f));
            g.drawFittedText ("ORGAN ENGINE BYPASS",
                              bypass.reduced (10),
                              juce::Justification::centred, 1);
        }
    }

    void resized() override
    {
        shell = ChurchOrganLayout::calculateShell (getLocalBounds());
        header.setSizeClass (shell.sizeClass);
        header.setBounds (shell.header);

        auto navigation = shell.navigation;
        const int count = ChurchOrganLayout::pageCount;
        const int width = navigation.getWidth() / count;
        for (int index = 0; index < count; ++index)
        {
            auto& button = navigationButtons[static_cast<size_t> (index)];
            const int currentWidth = index == count - 1
                                       ? navigation.getWidth() : width;
            button.setBounds (navigation.removeFromLeft (currentWidth));
            const auto page = static_cast<Page> (index);
            // O nome da aba encolhe pela LARGURA da fileira, nao pelo header
            // compacto: num 820 dp cabem "DRAWBARS" e "CHARACTER" por
            // extenso, e "BARS/ROTOR" lia como jargao (revisao do dono).
            const bool compact = shell.navigation.getWidth() < 700;
            const auto compactName =
                getWidth() <= 340 && page == Page::presets
                    ? juce::String ("SETS")
                    : ChurchOrganLayout::compactPageName (page);
            setButtonTextIfDifferent (
                button,
                compact ? compactName
                        : ChurchOrganLayout::pageName (page));
        }

        const int statusHeight = shell.sizeClass
                                      == ChurchOrganLayout::SizeClass::compactLandscape
                                     ? 30 : 36;
        auto pageArea = shell.content;
        auto statusRow = pageArea.removeFromBottom (
            juce::jmin (statusHeight + 6, pageArea.getHeight()));

        livePage.setSizeClass (shell.sizeClass);
        livePage.setBounds (pageArea);
        pageViewport.setBounds (pageArea);
        layoutActiveEditingPage();

        const int statusWidth = juce::jlimit (
            150, 300, statusRow.getWidth() - 12);
        seamlessStatus.setBounds (
            statusRow.reduced (6, 3)
                .withWidth (statusWidth));
        seamlessStatus.toFront (false);
    }

    void refreshFromModel()
    {
        lastSnapshot = sink.getUiSnapshot();
        header.refresh (lastSnapshot);
        livePage.refresh (lastSnapshot);
        drawbarsPage.refresh (lastSnapshot);
        characterPage.refresh (lastSnapshot);
        rotaryPage.refresh (lastSnapshot);
        worshipPage.refresh (lastSnapshot);
        presetsPage.refresh (lastSnapshot, sink.getPresetSummaries());

        const auto statusText = lastSnapshot.seamlessSwitching
                              ? "  SEAMLESS  "
                                + juce::String (juce::roundToInt (
                                    lastSnapshot.seamlessProgress * 100.0f))
                                + "%  " + lastSnapshot.seamlessMessage
                              : "  ENGINE READY  -  "
                                + lastSnapshot.seamlessMessage;
        if (seamlessStatus.getText() != statusText)
            seamlessStatus.setText (statusText, juce::dontSendNotification);
        seamlessStatus.setColour (
            juce::Label::textColourId,
            lastSnapshot.seamlessSwitching ? Tokens::organAmberLight()
                                           : Tokens::okGreen());
        seamlessStatus.setDescription (lastSnapshot.seamlessMessage);
        repaint();
    }

    void setPage (Page newPage)
    {
        if (newPage == Page::count)
            return;

        activePage = newPage;
        for (int index = 0; index < ChurchOrganLayout::pageCount; ++index)
            navigationButtons[static_cast<size_t> (index)].setToggleState (
                index == pageIndex (activePage),
                juce::dontSendNotification);

        if (activePage == Page::live)
        {
            pageViewport.setViewedComponent (nullptr, false);
            pageViewport.setVisible (false);
            livePage.setVisible (true);
        }
        else
        {
            livePage.setVisible (false);
            pageViewport.setVisible (true);
            pageViewport.setViewedComponent (pageComponent (activePage), false);
            pageViewport.setViewPosition (0, 0);
            layoutActiveEditingPage();
        }

        resized();
        if (auto* handler = owner.getAccessibilityHandler())
            handler->notifyAccessibilityEvent (
                juce::AccessibilityEvent::structureChanged);
    }

    juce::Component* pageComponent (Page page) noexcept
    {
        switch (page)
        {
            case Page::drawbars:  return &drawbarsPage;
            case Page::character: return &characterPage;
            case Page::rotary:    return &rotaryPage;
            case Page::worship:   return &worshipPage;
            case Page::presets:   return &presetsPage;
            case Page::live:
            case Page::count:     return nullptr;
        }
        return nullptr;
    }

    void layoutActiveEditingPage()
    {
        if (activePage == Page::live || activePage == Page::count)
            return;

        auto* component = pageComponent (activePage);
        if (component == nullptr)
            return;

        const int width = juce::jmax (1, pageViewport.getMaximumVisibleWidth());
        const int height = ChurchOrganLayout::preferredEditingPageHeight (
            activePage, width, pageViewport.getHeight());
        component->setBounds (0, 0, width, height);
    }

    ChurchOrganEditor& owner;
    ChurchOrganParameterSink& sink;
    ChurchOrganLookAndFeel lookAndFeel;
    ChurchOrganLayout::Shell shell;
    ChurchOrganUiSnapshot lastSnapshot;
    Page activePage = Page::live;

    HeaderComponent header;
    std::array<juce::TextButton, ChurchOrganLayout::pageCount> navigationButtons;
    LivePageComponent livePage;
    juce::Viewport pageViewport;
    DrawbarsEditingPage drawbarsPage;
    CharacterEditingPage characterPage;
    RotaryEditingPage rotaryPage;
    WorshipEditingPage worshipPage;
    PresetsEditingPage presetsPage;
    juce::Label seamlessStatus;
};

ChurchOrganEditor::ChurchOrganEditor (ChurchOrganParameterSink& sink)
    : impl (std::make_unique<Impl> (*this, sink))
{
    setTitle ("Church Organ Engine");
    setDescription ("Responsive ChurchDAW organ editor");
    setWantsKeyboardFocus (true);
    setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    addAndMakeVisible (*impl);
    setSize (1366, 768);
    startTimerHz (10);
}

ChurchOrganEditor::~ChurchOrganEditor()
{
    stopTimer();
    impl.reset();
}

void ChurchOrganEditor::paint (juce::Graphics& g)
{
    g.fillAll (Tokens::appBackground());
}

void ChurchOrganEditor::resized()
{
    impl->setBounds (getLocalBounds());
}

bool ChurchOrganEditor::keyPressed (const juce::KeyPress& key)
{
    const juce::juce_wchar character = key.getTextCharacter();
    if (character >= '1' && character <= '6')
    {
        showPage (static_cast<Page> (character - '1'));
        return true;
    }

    return juce::Component::keyPressed (key);
}

ChurchOrganLayout::Page ChurchOrganEditor::getActivePage() const noexcept
{
    return impl->activePage;
}

void ChurchOrganEditor::showPage (Page page)
{
    impl->setPage (page);
}

ChurchOrganLayout::Shell ChurchOrganEditor::getShellLayoutForTesting() const noexcept
{
    return impl->shell;
}

ChurchOrganLayout::Live ChurchOrganEditor::getLiveLayoutForTesting() const noexcept
{
    return impl->livePage.getLayout();
}

juce::Rectangle<int> ChurchOrganEditor::getNavigationButtonBoundsForTesting (
    int index) const
{
    if (! juce::isPositiveAndBelow (
            index, static_cast<int> (impl->navigationButtons.size())))
        return {};
    return impl->navigationButtons[static_cast<size_t> (index)].getBounds();
}

juce::Rectangle<int> ChurchOrganEditor::getLiveDrawbarBoundsForTesting (
    int index) const
{
    return impl->livePage.getDrawbarBounds (index)
        .translated (impl->livePage.getX(), impl->livePage.getY());
}

void ChurchOrganEditor::timerCallback()
{
    impl->refreshFromModel();
}
} // namespace churchdaw::organ
