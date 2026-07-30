#include "ChurchOrganControls.h"

#include <cmath>

namespace churchdaw::organ
{
namespace
{
using Tokens = ChurchOrganDesignTokens;

juce::Colour buttonFill (const juce::Button& button,
                         bool highlighted,
                         bool down)
{
    if (button.getToggleState())
        return (down ? Tokens::churchRedPressed() : Tokens::churchRed());

    if (down)
        return Tokens::surface3().brighter (0.12f);

    if (highlighted)
        return Tokens::surface3();

    return Tokens::surface2();
}

juce::String drawbarFootage (int index)
{
    static constexpr const char* names[]
    {
        "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"
    };

    return juce::String (names[juce::jlimit (0, 8, index)]);
}
} // namespace

ChurchOrganLookAndFeel::ChurchOrganLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, Tokens::surface2());
    setColour (juce::TextButton::buttonOnColourId, Tokens::churchRed());
    setColour (juce::TextButton::textColourOffId, Tokens::textSecondary());
    setColour (juce::TextButton::textColourOnId, Tokens::text());

    setColour (juce::Slider::backgroundColourId, Tokens::surface3());
    setColour (juce::Slider::trackColourId, Tokens::organAmber());
    setColour (juce::Slider::thumbColourId, Tokens::organAmberLight());
    setColour (juce::Slider::textBoxTextColourId, Tokens::text());
    setColour (juce::Slider::textBoxBackgroundColourId, Tokens::elevatedSurface());
    setColour (juce::Slider::textBoxOutlineColourId, Tokens::border());

    setColour (juce::Label::textColourId, Tokens::text());
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TextEditor::backgroundColourId, Tokens::elevatedSurface());
    setColour (juce::TextEditor::textColourId, Tokens::text());
    setColour (juce::TextEditor::highlightColourId, Tokens::churchRed().withAlpha (0.55f));
    setColour (juce::TextEditor::outlineColourId, Tokens::border());
    setColour (juce::TextEditor::focusedOutlineColourId, Tokens::focusRing());

    setColour (juce::ComboBox::backgroundColourId, Tokens::elevatedSurface());
    setColour (juce::ComboBox::textColourId, Tokens::text());
    setColour (juce::ComboBox::outlineColourId, Tokens::border());
    setColour (juce::ComboBox::arrowColourId, Tokens::organAmberLight());
}

void ChurchOrganLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                                    juce::Button& button,
                                                    const juce::Colour&,
                                                    bool highlighted,
                                                    bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const auto radius = Tokens::radiusControl;

    g.setColour (buttonFill (button, highlighted, down));
    g.fillRoundedRectangle (bounds, radius);

    auto outline = button.getToggleState()
                       ? Tokens::churchRed().brighter (0.28f)
                       : Tokens::border();
    g.setColour (outline);
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    if (button.hasKeyboardFocus (true))
        Tokens::drawFocusRing (g, bounds, radius);
}

void ChurchOrganLookAndFeel::drawButtonText (juce::Graphics& g,
                                              juce::TextButton& button,
                                              bool,
                                              bool)
{
    const bool compact =
        button.getWidth() < 72 || button.getHeight() < 40;
    g.setColour (button.getToggleState() ? Tokens::text()
                                          : button.findColour (juce::TextButton::textColourOffId));
    g.setFont (Tokens::stageLabelFont (
        juce::jlimit (compact ? 8.0f : 10.0f,
                      14.0f,
                      button.getHeight() * 0.28f)));
    g.drawFittedText (button.getButtonText(),
                      button.getLocalBounds().reduced (compact ? 2 : 6, 2),
                      juce::Justification::centred,
                      1,
                      compact ? 0.45f : 0.7f);
}

void ChurchOrganLookAndFeel::drawRotarySlider (juce::Graphics& g,
                                                int x,
                                                int y,
                                                int width,
                                                int height,
                                                float position,
                                                float startAngle,
                                                float endAngle,
                                                juce::Slider& slider)
{
    const float diameter = std::min (static_cast<float> (width),
                                     static_cast<float> (height)) - 10.0f;
    auto circle = juce::Rectangle<float> (diameter, diameter)
                    .withCentre ({ x + width * 0.5f, y + height * 0.5f });

    const float lineWidth = juce::jmax (3.0f, diameter * 0.075f);
    const float angle = startAngle + position * (endAngle - startAngle);

    juce::Path backgroundArc;
    backgroundArc.addCentredArc (circle.getCentreX(), circle.getCentreY(),
                                 circle.getWidth() * 0.5f - lineWidth,
                                 circle.getHeight() * 0.5f - lineWidth,
                                 0.0f, startAngle, endAngle, true);
    g.setColour (Tokens::surface3());
    g.strokePath (backgroundArc,
                  juce::PathStrokeType (lineWidth,
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));

    juce::Path valueArc;
    valueArc.addCentredArc (circle.getCentreX(), circle.getCentreY(),
                            circle.getWidth() * 0.5f - lineWidth,
                            circle.getHeight() * 0.5f - lineWidth,
                            0.0f, startAngle, angle, true);
    g.setColour (Tokens::organAmber());
    g.strokePath (valueArc,
                  juce::PathStrokeType (lineWidth,
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));

    const auto body = circle.reduced (lineWidth * 1.8f);
    juce::ColourGradient bodyGradient (Tokens::surface3().brighter (0.16f),
                                       body.getCentreX(), body.getY(),
                                       Tokens::elevatedSurface(),
                                       body.getCentreX(), body.getBottom(),
                                       false);
    g.setGradientFill (bodyGradient);
    g.fillEllipse (body);
    g.setColour (Tokens::borderStrong());
    g.drawEllipse (body, 1.0f);

    juce::Path pointer;
    const auto radius = body.getWidth() * 0.38f;
    const auto centre = body.getCentre();
    pointer.startNewSubPath (centre);
    pointer.lineTo (centre.x + std::sin (angle) * radius,
                    centre.y - std::cos (angle) * radius);
    g.setColour (Tokens::organAmberLight());
    g.strokePath (pointer,
                  juce::PathStrokeType (juce::jmax (2.0f, diameter * 0.035f),
                                        juce::PathStrokeType::curved,
                                        juce::PathStrokeType::rounded));

    if (slider.hasKeyboardFocus (true))
        Tokens::drawFocusRing (g, slider.getLocalBounds().toFloat(), Tokens::radiusControl);
}

void ChurchOrganLookAndFeel::drawLinearSlider (juce::Graphics& g,
                                                int x,
                                                int y,
                                                int width,
                                                int height,
                                                float sliderPos,
                                                float,
                                                float,
                                                juce::Slider::SliderStyle style,
                                                juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal
        && style != juce::Slider::LinearBar
        && style != juce::Slider::LinearBarVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          0.0f, 0.0f, style, slider);
        return;
    }

    auto bounds = juce::Rectangle<float> (static_cast<float> (x),
                                          static_cast<float> (y),
                                          static_cast<float> (width),
                                          static_cast<float> (height));
    const float trackHeight = juce::jlimit (4.0f, 8.0f, bounds.getHeight() * 0.16f);
    auto track = juce::Rectangle<float> (bounds.getX() + 8.0f,
                                         bounds.getCentreY() - trackHeight * 0.5f,
                                         bounds.getWidth() - 16.0f,
                                         trackHeight);

    g.setColour (Tokens::surface3());
    g.fillRoundedRectangle (track, trackHeight * 0.5f);

    auto valueTrack = track.withRight (juce::jlimit (track.getX(), track.getRight(), sliderPos));
    g.setColour (Tokens::organAmber());
    g.fillRoundedRectangle (valueTrack, trackHeight * 0.5f);

    const float thumbSize = juce::jlimit (14.0f, 22.0f, bounds.getHeight() * 0.46f);
    g.setColour (Tokens::organAmberLight());
    g.fillEllipse (sliderPos - thumbSize * 0.5f,
                   bounds.getCentreY() - thumbSize * 0.5f,
                   thumbSize,
                   thumbSize);

    if (slider.hasKeyboardFocus (true))
        Tokens::drawFocusRing (g, slider.getLocalBounds().toFloat(), Tokens::radiusControl);
}

ChurchOrganPanel::ChurchOrganPanel (juce::String titleToUse,
                                    juce::String detailToUse,
                                    bool useInstrumentSurface)
    : title (std::move (titleToUse)),
      detail (std::move (detailToUse)),
      instrumentSurface (useInstrumentSurface)
{
    setInterceptsMouseClicks (false, true);
    setAccessible (false);
}

void ChurchOrganPanel::setDetail (juce::String newDetail)
{
    if (detail == newDetail)
        return;

    detail = std::move (newDetail);
    repaint();
}

juce::Rectangle<int> ChurchOrganPanel::getContentBounds() const noexcept
{
    const bool ultraCompact = getHeight() < 80;
    const int header = ultraCompact ? 18 : getHeight() < 90 ? 27 : 36;
    return getLocalBounds()
        .reduced (ultraCompact ? 4 : 8)
        .withTrimmedTop (header);
}

void ChurchOrganPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    Tokens::fillCard (g, bounds,
                      instrumentSurface ? Tokens::surface2() : Tokens::surface1(),
                      Tokens::radiusCard);

    const float titleHeight =
        getHeight() < 80 ? 8.5f : getHeight() < 90 ? 10.0f : 11.5f;
    g.setColour (Tokens::text());
    g.setFont (Tokens::stageLabelFont (titleHeight));
    g.drawFittedText (title,
                      getLocalBounds().reduced (
                          getHeight() < 80 ? 6 : 10,
                          getHeight() < 80 ? 3 : 6).removeFromTop (18),
                      juce::Justification::centredLeft,
                      1);

    if (detail.isNotEmpty() && getWidth() > 118)
    {
        g.setColour (Tokens::textSecondary());
        g.setFont (Tokens::bodyFont (juce::jmax (9.0f, titleHeight - 1.5f)));
        g.drawFittedText (detail,
                          getLocalBounds().reduced (
                              getHeight() < 80 ? 6 : 10,
                              getHeight() < 80 ? 3 : 6).removeFromTop (18),
                          juce::Justification::centredRight,
                          1);
    }

    g.setColour (instrumentSurface ? Tokens::organAmber() : Tokens::borderStrong());
    g.fillEllipse (static_cast<float> (getWidth() - 15), 10.0f, 4.0f, 4.0f);
}

ChurchOrganDrawbar::ChurchOrganDrawbar (int index, juce::String footageToUse)
    : drawbarIndex (index),
      footage (std::move (footageToUse))
{
    setSliderStyle (juce::Slider::LinearVertical);
    setRange (0.0, 8.0, 1.0);
    setDoubleClickReturnValue (true, 0.0);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setWantsKeyboardFocus (true);
    setMouseDragSensitivity (110);
    // Precision Touchpad/trackpad envia a rolagem como mouse wheel. O gesto
    // pertence ao Viewport da página BARS, não ao valor do drawbar; com esta
    // flag o Slider encaminha o evento aos pais sem alterar o parâmetro.
    setScrollWheelEnabled (false);
    setTitle ("Drawbar " + footage);
    setDescription ("Harmonic drawbar " + footage + ", range zero to eight");
    setTooltip ("Drawbar " + footage + " - double-click returns to zero");
}

void ChurchOrganDrawbar::setMidiFeedback (bool active, bool waitingForPickup)
{
    if (midiActive == active && pickupWaiting == waitingForPickup)
        return;

    midiActive = active;
    pickupWaiting = waitingForPickup;
    repaint();
}

void ChurchOrganDrawbar::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (3.0f);
    const auto compact = getHeight() < 108;
    const float labelHeight = compact ? 16.0f : 22.0f;
    auto labelArea = area.removeFromTop (labelHeight);

    g.setColour (Tokens::text());
    g.setFont (Tokens::valueFont (compact ? 9.0f : 11.5f));
    g.drawFittedText (footage, labelArea.toNearestInt(),
                      juce::Justification::centred, 1);

    if (! compact)
    {
        auto valueArea = area.removeFromTop (15.0f);
        g.setColour (Tokens::organAmberLight());
        g.setFont (Tokens::valueFont (10.0f));
        g.drawText (juce::String (juce::roundToInt (getValue())),
                    valueArea, juce::Justification::centred);
    }

    if (pickupWaiting)
    {
        auto pickup = area.removeFromTop (compact ? 9.0f : 13.0f);
        g.setColour (Tokens::warning());
        g.setFont (Tokens::stageLabelFont (compact ? 7.0f : 8.0f));
        g.drawFittedText ("PICKUP", pickup.toNearestInt(),
                          juce::Justification::centred, 1);
    }

    auto track = area.reduced (juce::jmax (2.0f, area.getWidth() * 0.22f), 3.0f);
    if (track.getWidth() < 10.0f)
        track = track.withSizeKeepingCentre (10.0f, track.getHeight());

    const auto trackWidth = juce::jlimit (12.0f, 24.0f, track.getWidth());
    auto well = track.withSizeKeepingCentre (trackWidth, track.getHeight());

    g.setColour (Tokens::elevatedSurface());
    g.fillRoundedRectangle (well, trackWidth * 0.32f);
    g.setColour (Tokens::border());
    g.drawRoundedRectangle (well, trackWidth * 0.32f, 1.0f);

    for (int step = 1; step < 8; ++step)
    {
        const float y = well.getY() + step * well.getHeight() / 8.0f;
        g.setColour (Tokens::borderStrong().withAlpha (0.45f));
        g.drawHorizontalLine (juce::roundToInt (y),
                              well.getX() + 3.0f,
                              well.getRight() - 3.0f);
    }

    const float proportion = static_cast<float> (getValue() / 8.0);
    const float handleY = juce::jmap (proportion,
                                     well.getBottom() - 7.0f,
                                     well.getY() + 7.0f);
    auto fill = well.withTop (handleY).reduced (3.0f, 0.0f);
    g.setColour (Tokens::organAmber().withAlpha (0.46f));
    g.fillRoundedRectangle (fill, 3.0f);

    auto handle = juce::Rectangle<float> (
        juce::jlimit (26.0f, 68.0f, getWidth() - 9.0f),
        compact ? 14.0f : 18.0f).withCentre ({ well.getCentreX(), handleY });
    handle = handle.getIntersection (getLocalBounds().toFloat().reduced (2.0f));

    juce::ColourGradient handleGradient (Tokens::organAmberLight(),
                                         handle.getCentreX(), handle.getY(),
                                         Tokens::organAmber().darker (0.28f),
                                         handle.getCentreX(), handle.getBottom(),
                                         false);
    g.setGradientFill (handleGradient);
    g.fillRoundedRectangle (handle, 4.0f);
    g.setColour (Tokens::organAmberLight().brighter (0.15f));
    g.drawRoundedRectangle (handle, 4.0f, 1.0f);

    g.setColour (Tokens::elevatedSurface().withAlpha (0.55f));
    for (int stripe = -1; stripe <= 1; ++stripe)
        g.drawHorizontalLine (juce::roundToInt (handle.getCentreY()) + stripe * 3,
                              handle.getCentreX() - 7.0f,
                              handle.getCentreX() + 7.0f);

    if (midiActive)
    {
        g.setColour (Tokens::okGreen());
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f),
                                Tokens::radiusControl, 2.0f);
    }

    if (hasKeyboardFocus (true))
        Tokens::drawFocusRing (g, getLocalBounds().toFloat(), Tokens::radiusControl);
}

void ChurchOrganDrawbar::mouseDoubleClick (const juce::MouseEvent& event)
{
    juce::Slider::mouseDoubleClick (event);
    setValue (0.0, juce::sendNotificationSync);
}

juce::String ChurchOrganDrawbar::getTextFromValue (double value)
{
    return juce::String (juce::roundToInt (value)) + " of 8";
}

ChurchOrganDrawbarBank::ChurchOrganDrawbarBank()
{
    setTitle ("Nine harmonic drawbars");
    setDescription ("Tonewheel harmonic registration");
    setAccessible (true);

    for (int index = 0; index < static_cast<int> (drawbars.size()); ++index)
    {
        auto slider = std::make_unique<ChurchOrganDrawbar> (index, drawbarFootage (index));
        slider->onValueChange = [this, index, pointer = slider.get()]
        {
            if (onDrawbarChanged != nullptr)
                onDrawbarChanged (index, juce::roundToInt (pointer->getValue()));
        };
        addAndMakeVisible (*slider);
        drawbars[static_cast<size_t> (index)] = std::move (slider);
    }
}

void ChurchOrganDrawbarBank::setValues (const std::array<int, 9>& values,
                                         juce::NotificationType notification)
{
    for (int index = 0; index < static_cast<int> (drawbars.size()); ++index)
        drawbars[static_cast<size_t> (index)]->setValue (
            juce::jlimit (0, 8, values[static_cast<size_t> (index)]),
            notification);
}

std::array<int, 9> ChurchOrganDrawbarBank::getValues() const
{
    std::array<int, 9> values {};
    for (int index = 0; index < static_cast<int> (drawbars.size()); ++index)
        values[static_cast<size_t> (index)] =
            juce::roundToInt (drawbars[static_cast<size_t> (index)]->getValue());
    return values;
}

void ChurchOrganDrawbarBank::setMidiFeedback (int drawbarIndex, bool waitingForPickup)
{
    midiIndex = drawbarIndex;
    midiPickup = waitingForPickup;

    for (int index = 0; index < static_cast<int> (drawbars.size()); ++index)
        drawbars[static_cast<size_t> (index)]->setMidiFeedback (
            index == midiIndex, index == midiIndex && midiPickup);
}

void ChurchOrganDrawbarBank::setTwoRowLayout (bool shouldUseTwoRows)
{
    if (useTwoRows == shouldUseTwoRows)
        return;

    useTwoRows = shouldUseTwoRows;
    resized();
}

juce::Rectangle<int> ChurchOrganDrawbarBank::getDrawbarBounds (int index) const
{
    if (! juce::isPositiveAndBelow (index, static_cast<int> (drawbars.size())))
        return {};

    return drawbars[static_cast<size_t> (index)]->getBounds();
}

void ChurchOrganDrawbarBank::paint (juce::Graphics& g)
{
    if (useTwoRows)
    {
        g.setColour (Tokens::border().withAlpha (0.5f));
        g.drawHorizontalLine (getHeight() / 2,
                              4.0f,
                              static_cast<float> (getWidth() - 4));
    }
}

void ChurchOrganDrawbarBank::resized()
{
    auto area = getLocalBounds();
    constexpr int gap = 3;

    const auto placeRow = [this] (juce::Rectangle<int> row, int first, int count)
    {
        constexpr int rowGap = 3;
        const int cellWidth = std::max (1, (row.getWidth() - rowGap * (count - 1)) / count);
        for (int offset = 0; offset < count; ++offset)
        {
            const int width = offset == count - 1 ? row.getRight() - row.getX() : cellWidth;
            drawbars[static_cast<size_t> (first + offset)]->setBounds (
                row.removeFromLeft (width));
            if (offset != count - 1)
                row.removeFromLeft (std::min (rowGap, row.getWidth()));
        }
    };

    if (useTwoRows)
    {
        auto firstRow = area.removeFromTop ((area.getHeight() - gap) / 2);
        area.removeFromTop (std::min (gap, area.getHeight()));
        placeRow (firstRow, 0, 5);
        placeRow (area, 5, 4);
        return;
    }

    placeRow (area, 0, 9);
}

ChurchOrganParameterSlider::ChurchOrganParameterSlider (
    juce::String accessibleName,
    Presentation presentation,
    juce::String suffixToUse)
    : suffix (std::move (suffixToUse))
{
    setRange (0.0, 100.0, 1.0);
    setWantsKeyboardFocus (true);
    setTitle (accessibleName);
    setDescription (accessibleName + ", zero to one hundred percent");
    setTooltip (accessibleName + " - use arrows for fine adjustment");
    setTextValueSuffix (suffix);
    setDoubleClickReturnValue (true, 50.0);
    // Todas as páginas avançadas são roláveis. Evita que trackpads editem
    // knobs/sliders acidentalmente enquanto o usuário navega pela página.
    setScrollWheelEnabled (false);

    if (presentation == Presentation::rotary)
    {
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                             juce::MathConstants<float>::pi * 2.75f,
                             true);
        setTextBoxStyle (juce::Slider::TextBoxBelow, false, 62, 20);
    }
    else
    {
        setSliderStyle (juce::Slider::LinearHorizontal);
        setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 22);
    }
}

void ChurchOrganParameterSlider::setNormalisedValue (
    float value,
    juce::NotificationType notification)
{
    setValue (juce::jlimit (0.0f, 1.0f, value) * 100.0f, notification);
}

float ChurchOrganParameterSlider::getNormalisedValue() const noexcept
{
    return static_cast<float> (getValue() / 100.0);
}

ChurchOrganSegmentedControl::ChurchOrganSegmentedControl (
    juce::String name,
    std::vector<juce::String> options)
    : accessibleName (std::move (name))
{
    setTitle (accessibleName);
    setDescription (accessibleName + " selection");
    setAccessible (true);

    for (int index = 0; index < static_cast<int> (options.size()); ++index)
    {
        auto button = std::make_unique<juce::TextButton> (options[static_cast<size_t> (index)]);
        button->setClickingTogglesState (false);
        button->setWantsKeyboardFocus (true);
        button->setTitle (accessibleName + ": " + options[static_cast<size_t> (index)]);
        button->setDescription ("Select " + options[static_cast<size_t> (index)]);
        button->setTooltip (accessibleName + " - " + options[static_cast<size_t> (index)]);
        button->onClick = [this, index]
        {
            setSelectedIndex (index, juce::sendNotificationSync);
        };
        addAndMakeVisible (*button);
        buttons.push_back (std::move (button));
    }

    setSelectedIndex (0);
}

void ChurchOrganSegmentedControl::setSelectedIndex (
    int index,
    juce::NotificationType notification)
{
    if (buttons.empty())
        return;

    index = juce::jlimit (0, static_cast<int> (buttons.size()) - 1, index);
    const bool changed = selectedIndex != index;
    selectedIndex = index;

    for (int buttonIndex = 0; buttonIndex < static_cast<int> (buttons.size()); ++buttonIndex)
        buttons[static_cast<size_t> (buttonIndex)]->setToggleState (
            buttonIndex == selectedIndex, juce::dontSendNotification);

    if (changed && notification != juce::dontSendNotification
        && onSelectionChanged != nullptr)
        onSelectionChanged (selectedIndex);
}

juce::Rectangle<int> ChurchOrganSegmentedControl::getOptionBounds (int index) const
{
    if (! juce::isPositiveAndBelow (index, static_cast<int> (buttons.size())))
        return {};

    return buttons[static_cast<size_t> (index)]->getBounds();
}

void ChurchOrganSegmentedControl::resized()
{
    if (buttons.empty())
        return;

    auto area = getLocalBounds();
    constexpr int gap = 3;
    const int width = std::max (1,
        (area.getWidth() - gap * (static_cast<int> (buttons.size()) - 1))
        / static_cast<int> (buttons.size()));

    for (int index = 0; index < static_cast<int> (buttons.size()); ++index)
    {
        const int currentWidth = index == static_cast<int> (buttons.size()) - 1
                                   ? area.getWidth() : width;
        buttons[static_cast<size_t> (index)]->setBounds (area.removeFromLeft (currentWidth));
        if (index != static_cast<int> (buttons.size()) - 1)
            area.removeFromLeft (std::min (gap, area.getWidth()));
    }
}

ChurchOrganRotaryVisualizer::ChurchOrganRotaryVisualizer()
{
    setTitle ("Rotary cabinet motion");
    setDescription ("Static reduced-motion visualisation of horn and drum speed");
    setAccessible (true);
    setInterceptsMouseClicks (false, false);
}

void ChurchOrganRotaryVisualizer::setMode (ChurchOrganRotaryMode newMode)
{
    if (mode == newMode)
        return;

    mode = newMode;
    repaint();
}

void ChurchOrganRotaryVisualizer::setBalance (float hornLevel, float drumLevel)
{
    const float newHorn = juce::jlimit (0.0f, 1.0f, hornLevel);
    const float newDrum = juce::jlimit (0.0f, 1.0f, drumLevel);
    if (horn == newHorn && drum == newDrum)
        return;

    horn = newHorn;
    drum = newDrum;
    repaint();
}

void ChurchOrganRotaryVisualizer::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (8.0f);
    const float diameter = std::min (area.getWidth(), area.getHeight());
    auto cabinet = juce::Rectangle<float> (diameter, diameter).withCentre (area.getCentre());

    g.setColour (Tokens::elevatedSurface());
    g.fillEllipse (cabinet);
    g.setColour (Tokens::border());
    g.drawEllipse (cabinet, 1.5f);

    const float activity = mode == ChurchOrganRotaryMode::fast ? 1.0f
                           : mode == ChurchOrganRotaryMode::slow ? 0.55f
                           : mode == ChurchOrganRotaryMode::brake ? 0.28f : 0.08f;
    const auto accent = mode == ChurchOrganRotaryMode::fast
                            ? Tokens::rotaryFast()
                            : Tokens::rotarySlow();

    for (int arc = 0; arc < 3; ++arc)
    {
        juce::Path path;
        const float inset = 10.0f + arc * 11.0f;
        auto ellipse = cabinet.reduced (inset);
        path.addCentredArc (ellipse.getCentreX(), ellipse.getCentreY(),
                            ellipse.getWidth() * 0.5f,
                            ellipse.getHeight() * 0.5f,
                            0.0f,
                            -juce::MathConstants<float>::halfPi + arc * 0.52f,
                            -juce::MathConstants<float>::halfPi
                                + arc * 0.52f
                                + juce::MathConstants<float>::pi * (0.65f + activity),
                            true);
        g.setColour (accent.withAlpha (0.32f + 0.2f * activity));
        g.strokePath (path, juce::PathStrokeType (3.0f,
                                                  juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    auto centre = cabinet.reduced (diameter * 0.30f);
    g.setColour (Tokens::surface3());
    g.fillEllipse (centre);
    g.setColour (accent);
    g.drawEllipse (centre, 2.0f);

    const float bladeLength = centre.getWidth() * 0.45f;
    const float bladeWidth = juce::jmax (3.0f, centre.getWidth() * 0.08f);
    const float baseAngle = mode == ChurchOrganRotaryMode::fast ? 0.8f
                            : mode == ChurchOrganRotaryMode::slow ? 0.35f : 0.0f;
    for (int blade = 0; blade < 3; ++blade)
    {
        const float angle = baseAngle + blade * juce::MathConstants<float>::twoPi / 3.0f;
        juce::Path path;
        path.startNewSubPath (centre.getCentre());
        path.lineTo (centre.getCentreX() + std::cos (angle) * bladeLength,
                     centre.getCentreY() + std::sin (angle) * bladeLength);
        g.setColour (Tokens::textSecondary().withAlpha (0.8f));
        g.strokePath (path,
                      juce::PathStrokeType (bladeWidth,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
    }

    const auto modeText = mode == ChurchOrganRotaryMode::fast ? "FAST"
                          : mode == ChurchOrganRotaryMode::slow ? "SLOW"
                          : mode == ChurchOrganRotaryMode::brake ? "BRAKE" : "STOP";
    g.setColour (Tokens::text());
    g.setFont (Tokens::stageLabelFont (12.0f));
    g.drawText (modeText, cabinet.toNearestInt().removeFromBottom (28),
                juce::Justification::centred);

    g.setColour (Tokens::textSecondary());
    g.setFont (Tokens::bodyFont (9.5f));
    g.drawText ("H " + juce::String (juce::roundToInt (horn * 100.0f))
                    + "  D " + juce::String (juce::roundToInt (drum * 100.0f)),
                cabinet.toNearestInt().removeFromTop (24),
                juce::Justification::centred);
}

ChurchOrganWorshipLayers::ChurchOrganWorshipLayers()
{
    setTitle ("Worship layers");
    setDescription ("Organ, pad, air, strings and shimmer intensity");
    setAccessible (true);
    setInterceptsMouseClicks (false, false);
}

void ChurchOrganWorshipLayers::setIntensity (float normalisedIntensity)
{
    const auto next = juce::jlimit (0.0f, 1.0f, normalisedIntensity);
    if (intensity == next)
        return;

    intensity = next;
    repaint();
}

void ChurchOrganWorshipLayers::paint (juce::Graphics& g)
{
    static constexpr const char* names[] { "ORGAN", "PAD", "AIR", "STRINGS", "SHIMMER" };
    static constexpr float thresholds[] { 0.0f, 0.25f, 0.55f, 0.55f, 0.80f };

    auto area = getLocalBounds();
    constexpr int gap = 4;
    const int itemWidth = std::max (1, (area.getWidth() - gap * 4) / 5);

    for (int index = 0; index < 5; ++index)
    {
        const int width = index == 4 ? area.getWidth() : itemWidth;
        auto item = area.removeFromLeft (width);
        const bool active = intensity >= thresholds[index];
        g.setColour (active ? Tokens::organAmber().withAlpha (0.36f)
                            : Tokens::surface3());
        g.fillRoundedRectangle (item.toFloat(), Tokens::radiusControl);
        g.setColour (active ? Tokens::organAmberLight() : Tokens::textDisabled());
        g.drawRoundedRectangle (item.toFloat().reduced (0.5f),
                                Tokens::radiusControl, 1.0f);
        g.setFont (Tokens::stageLabelFont (juce::jlimit (7.0f, 10.0f,
                                                         item.getWidth() * 0.11f)));
        g.drawFittedText (names[index], item.reduced (3),
                          juce::Justification::centred, 1);

        if (index != 4)
            area.removeFromLeft (std::min (gap, area.getWidth()));
    }
}
} // namespace churchdaw::organ
