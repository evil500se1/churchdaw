#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include <functional>

//==============================================================================
// Custom premium trigger button for Audio/MIDI configuration.
// Features dark metallic glass surface gradient, custom vector icons
// (Speaker/Waveform + MIDI keys), active state illumination (LED dot & halo),
// responsive text support, and hover/pressed visual states.
//==============================================================================
class AudioMidiButton : public juce::TextButton
{
public:
    AudioMidiButton (const juce::String& buttonName = "Audio / MIDI")
        : juce::TextButton (buttonName), buttonText (buttonName)
    {
        setClickingTogglesState (false);
    }

    void setButtonText (const juce::String& newText)
    {
        juce::TextButton::setButtonText (newText);
        if (buttonText != newText)
        {
            buttonText = newText;
            repaint();
        }
    }

    juce::String getButtonText() const { return buttonText; }

    void setActiveSupplier (std::function<bool()> supplier)
    {
        activeSupplier = std::move (supplier);
    }

    void setActive (bool active)
    {
        if (isActiveState != active)
        {
            isActiveState = active;
            repaint();
        }
    }

    bool isPanelActive() const
    {
        if (activeSupplier)
            return activeSupplier();
        return isActiveState;
    }

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const bool active = isPanelActive();
        auto bounds = getLocalBounds().toFloat();
        if (bounds.isEmpty())
            return;

        // Skin Church Live: o item do trilho e desenhado pelo LookAndFeel de skin.
        if (getProperties().getWithDefault ("churchSkin", juce::var()).toString() == "rail")
        {
            auto& lf = getLookAndFeel();
            lf.drawButtonBackground (g, *this, findColour (juce::TextButton::buttonColourId),
                                     shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            lf.drawButtonText (g, *this, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            return;
        }

        if (getComponentID() == "headerNav" && NordLookAndFeel::usesDesktopShell (*this))
        {
            // Reuse the shell's surfaces and glyphs. The audio panel supplies
            // its own active state; painting must not mutate the button toggle.
            getLookAndFeel().drawButtonBackground (
                g, *this, active ? NordLookAndFeel::nordRed()
                                 : juce::Colours::transparentBlack,
                shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
            NordLookAndFeel::drawDesktopNavigationContents (
                g, *this, shouldDrawButtonAsHighlighted, active);
            return;
        }

        // Dentro do shell desktop este componente participa da mesma barra de
        // navegacao dos demais destinos. O desenho antigo criava um card azul
        // elevado e quebrava a continuidade visual exatamente no ultimo item.
        if (getComponentID() == "headerNav")
        {
           #if CHURCHDAW_MOBILE_SHELL
            if (getHeight() >= 44)
            {
                NordLookAndFeel::drawTouchNavPill (g, bounds.reduced (1.0f), active, false,
                                                   shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown,
                                                   isEnabled() ? 1.0f : 0.42f);
                auto area = getLocalBounds().reduced (6, 2);
                const float side = juce::jmin (14.0f, (float) area.getHeight() - 4.0f);
                const auto icon = juce::Rectangle<float> ((float) area.getCentreX() - side * 0.5f,
                                                          (float) area.getY() + 3.0f, side, side);
                g.setColour (active ? NordLookAndFeel::nordRedBright()
                                    : findColour (juce::TextButton::textColourOffId));
                juce::Path speaker;
                speaker.startNewSubPath (icon.getX(), icon.getCentreY() - 2.5f);
                speaker.lineTo (icon.getX() + 4.0f, icon.getCentreY() - 2.5f);
                speaker.lineTo (icon.getX() + 8.0f, icon.getY() + 2.0f);
                speaker.lineTo (icon.getX() + 8.0f, icon.getBottom() - 2.0f);
                speaker.lineTo (icon.getX() + 4.0f, icon.getCentreY() + 2.5f);
                speaker.lineTo (icon.getX(), icon.getCentreY() + 2.5f);
                speaker.closeSubPath();
                g.fillPath (speaker);
                g.drawLine (icon.getX() + 10.5f, icon.getY() + 4.0f, icon.getX() + 10.5f, icon.getBottom() - 4.0f, 1.2f);
                g.drawLine (icon.getX() + 13.0f, icon.getY() + 2.0f, icon.getX() + 13.0f, icon.getBottom() - 2.0f, 1.2f);
                g.setFont (getLookAndFeel().getTextButtonFont (*this, getHeight()));
                g.drawFittedText (buttonText, area.withTrimmedTop (20).reduced (0, 1),
                                  juce::Justification::centred, 1, 0.72f);
                return;
            }
            {
                auto pill = bounds.reduced (1.5f, 3.0f);
                const float rad = juce::jmin (8.0f, pill.getHeight() * 0.24f);
                if (active)
                {
                    g.setGradientFill (juce::ColourGradient (
                        juce::Colour (0xff461a26), pill.getX(), pill.getY(),
                        juce::Colour (0xff2b1018), pill.getX(), pill.getBottom(), false));
                    g.fillRoundedRectangle (pill, rad);
                    g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.48f));
                    g.drawRoundedRectangle (pill.reduced (0.5f), rad, 1.0f);
                }
                else
                {
                    const float surfAlpha = shouldDrawButtonAsDown ? 0.085f
                                          : shouldDrawButtonAsHighlighted ? 0.060f : 0.028f;
                    g.setColour (juce::Colours::white.withAlpha (surfAlpha));
                    g.fillRoundedRectangle (pill, rad);
                    g.setColour (juce::Colours::white.withAlpha (
                        shouldDrawButtonAsHighlighted ? 0.11f : 0.055f));
                    g.drawRoundedRectangle (pill.reduced (0.5f), rad, 1.0f);
                }
            }
           #else
            if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
            {
                g.setColour (juce::Colours::white.withAlpha (
                    shouldDrawButtonAsDown ? 0.060f : 0.032f));
                g.fillRect (bounds);
            }

            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.drawVerticalLine (juce::roundToInt (bounds.getRight()),
                                bounds.getY() + 8.0f, bounds.getBottom() - 8.0f);
           #endif

            const auto label = buttonText.toUpperCase();
            const auto font = juce::Font (juce::FontOptions (11.0f,
                                                             juce::Font::bold));
            const float iconSide = 14.0f;
            const float textW = juce::GlyphArrangement::getStringWidth (font, label);
            const float groupW = iconSide + 6.0f + textW;
            const float startX = bounds.getCentreX() - groupW * 0.5f;
            auto icon = juce::Rectangle<float> (
                startX, bounds.getCentreY() - iconSide * 0.5f,
                iconSide, iconSide);

            const auto colour = active ? NordLookAndFeel::nordRedBright()
                                       : juce::Colours::white.withAlpha (0.78f);
            g.setColour (colour);
            juce::Path speaker;
            speaker.startNewSubPath (icon.getX(), icon.getCentreY() - 2.5f);
            speaker.lineTo (icon.getX() + 4.0f, icon.getCentreY() - 2.5f);
            speaker.lineTo (icon.getX() + 8.0f, icon.getY() + 2.0f);
            speaker.lineTo (icon.getX() + 8.0f, icon.getBottom() - 2.0f);
            speaker.lineTo (icon.getX() + 4.0f, icon.getCentreY() + 2.5f);
            speaker.lineTo (icon.getX(), icon.getCentreY() + 2.5f);
            speaker.closeSubPath();
            g.fillPath (speaker);
            g.drawLine (icon.getX() + 10.5f, icon.getY() + 4.0f,
                        icon.getX() + 10.5f, icon.getBottom() - 4.0f, 1.0f);
            g.drawLine (icon.getX() + 13.0f, icon.getY() + 2.0f,
                        icon.getX() + 13.0f, icon.getBottom() - 2.0f, 1.0f);

            g.setFont (font);
            g.drawText (label,
                        juce::Rectangle<float> (startX + iconSide + 6.0f,
                                                bounds.getY(), textW + 2.0f,
                                                bounds.getHeight()),
                        juce::Justification::centredLeft);

            if (active)
            {
                g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.9f));
                g.fillRoundedRectangle (bounds.getX() + 12.0f,
                                        bounds.getBottom() - 2.5f,
                                        bounds.getWidth() - 24.0f, 2.0f, 1.0f);
            }
            return;
        }

        const float h = bounds.getHeight();
        const float w = bounds.getWidth();
        const float r = juce::jmin (7.0f, h * 0.28f);

        // 1. Drop shadow (subtle outer elevation)
        {
            juce::Path shadowPath;
            shadowPath.addRoundedRectangle (bounds, r);
            juce::DropShadow (juce::Colours::black.withAlpha (shouldDrawButtonAsDown ? 0.25f : (shouldDrawButtonAsHighlighted ? 0.45f : 0.35f)),
                              shouldDrawButtonAsDown ? 2 : 4,
                              { 0, shouldDrawButtonAsDown ? 1 : 2 }).drawForPath (g, shadowPath);
        }

        // 2. Metallic Dark Glass Surface Gradient
        juce::Colour topColor, bottomColor;
        if (shouldDrawButtonAsDown)
        {
            topColor = juce::Colour (0xff12151c);
            bottomColor = juce::Colour (0xff1f2530);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            topColor = juce::Colour (0xff2b3342);
            bottomColor = juce::Colour (0xff181d27);
        }
        else if (active)
        {
            topColor = juce::Colour (0xff252c38);
            bottomColor = juce::Colour (0xff161a22);
        }
        else
        {
            topColor = juce::Colour (0xff202530);
            bottomColor = juce::Colour (0xff13161d);
        }

        g.setGradientFill (juce::ColourGradient (topColor, bounds.getX(), bounds.getY(),
                                                 bottomColor, bounds.getX(), bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, r);

        // 3. Inner Specular Top Line Highlight
        {
            auto specArea = bounds.reduced (1.2f).withHeight (1.2f);
            const float specAlpha = shouldDrawButtonAsDown ? 0.08f : (shouldDrawButtonAsHighlighted ? 0.25f : (active ? 0.20f : 0.14f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (specAlpha),
                                                     specArea.getX(), specArea.getY(),
                                                     juce::Colours::transparentWhite,
                                                     specArea.getRight(), specArea.getY(), false));
            g.fillRoundedRectangle (specArea, r * 0.5f);
        }

        // 4. Border Styling & Active Illumination Halo
        juce::Colour borderColor;
        float borderWidth = 1.0f;
        if (active)
        {
            borderColor = NordLookAndFeel::nordRedBright(); // Bright accent border
            borderWidth = 1.4f;

            // Active top rail accent line
            juce::Path topRail;
            topRail.addRoundedRectangle (bounds.getX() + r * 0.5f, bounds.getY() + 0.5f,
                                         w - r, 2.0f, 1.0f);
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.90f));
            g.fillPath (topRail);

            // Glowing outer stroke
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.35f));
            g.drawRoundedRectangle (bounds.reduced (0.2f), r, 2.2f);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            borderColor = NordLookAndFeel::nordRed().withAlpha (0.75f); // Nord Red hover border
            borderWidth = 1.2f;
        }
        else
        {
            borderColor = NordLookAndFeel::borderDefault().withAlpha (0.80f); // Standard graphite border
        }

        g.setColour (borderColor);
        g.drawRoundedRectangle (bounds.reduced (borderWidth * 0.5f), r, borderWidth);

        // 5. Compute Content Layout Area (LED dot, Vector Icons, Text)
        // Offset content down by 1px if pressed
        const float dy = shouldDrawButtonAsDown ? 1.0f : 0.0f;
        auto contentArea = bounds.reduced (r * 0.7f, 2.0f);
        contentArea.setY (contentArea.getY() + dy);

        float currentX = contentArea.getX();

        // A. Active/Inactive Status LED Dot
        const float ledRadius = juce::jlimit (2.2f, 3.2f, h * 0.10f);
        const float ledX = currentX + ledRadius + 1.0f;
        const float ledY = contentArea.getCentreY();

        if (active)
        {
            // Glowing halo for LED
            g.setColour (NordLookAndFeel::greenOk().withAlpha (0.40f));
            g.fillEllipse (ledX - ledRadius - 2.0f, ledY - ledRadius - 2.0f,
                           (ledRadius + 2.0f) * 2.0f, (ledRadius + 2.0f) * 2.0f);

            // Bright LED core
            g.setColour (NordLookAndFeel::greenOk());
            g.fillEllipse (ledX - ledRadius, ledY - ledRadius, ledRadius * 2.0f, ledRadius * 2.0f);

            // Specular dot inside LED
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.fillEllipse (ledX - ledRadius * 0.4f, ledY - ledRadius * 0.4f, ledRadius * 0.7f, ledRadius * 0.7f);
        }
        else
        {
            // Inactive dark LED dot
            g.setColour (juce::Colour (0xff404958));
            g.fillEllipse (ledX - ledRadius * 0.8f, ledY - ledRadius * 0.8f, ledRadius * 1.6f, ledRadius * 1.6f);
        }

        currentX = ledX + ledRadius + 4.0f;

        // B. Custom Vector Icons (Speaker/Waveform + MIDI Keys)
        const float iconH = juce::jlimit (10.0f, 16.0f, h * 0.46f);
        const float speakerW = iconH * 1.0f;
        const float midiW = iconH * 1.1f;
        const bool isNarrow = (buttonText.trim().equalsIgnoreCase (
                                   juce::String::fromUTF8 ("\xc3\x81udio"))
                               || w < 85.0f);

        juce::Colour iconColour = active ? juce::Colour (0xffffffff)
                                         : (shouldDrawButtonAsHighlighted ? juce::Colour (0xfff0f4fa)
                                                                        : juce::Colour (0xffb0b9c6));

        // Draw Speaker Icon
        {
            auto speakerRect = juce::Rectangle<float> (currentX, contentArea.getCentreY() - iconH * 0.5f, speakerW, iconH);

            const float sx = speakerRect.getX();
            const float sy = speakerRect.getY();
            const float sw = speakerRect.getWidth();
            const float sh = speakerRect.getHeight();

            g.setColour (iconColour);

            juce::Path sp;
            sp.startNewSubPath (sx, sy + sh * 0.32f);
            sp.lineTo (sx + sw * 0.28f, sy + sh * 0.32f);
            sp.lineTo (sx + sw * 0.52f, sy + sh * 0.10f);
            sp.lineTo (sx + sw * 0.52f, sy + sh * 0.90f);
            sp.lineTo (sx + sw * 0.28f, sy + sh * 0.68f);
            sp.lineTo (sx, sy + sh * 0.68f);
            sp.closeSubPath();
            g.fillPath (sp);

            // Soundwave lines
            g.drawLine (sx + sw * 0.68f, sy + sh * 0.32f, sx + sw * 0.68f, sy + sh * 0.68f, 1.2f);
            g.drawLine (sx + sw * 0.88f, sy + sh * 0.18f, sx + sw * 0.88f, sy + sh * 0.82f, 1.2f);

            currentX += speakerW + 3.0f;
        }

        // Draw MIDI Keys Icon (if width allows)
        if (! isNarrow && (w >= 90.0f))
        {
            auto midiRect = juce::Rectangle<float> (currentX, contentArea.getCentreY() - iconH * 0.45f, midiW, iconH * 0.9f);

            const float mx = midiRect.getX();
            const float my = midiRect.getY();
            const float mw = midiRect.getWidth();
            const float mh = midiRect.getHeight();

            g.setColour (iconColour.withAlpha (0.9f));
            g.drawRoundedRectangle (midiRect, 1.2f, 1.0f);

            // Key divider lines
            for (float f : { 0.33f, 0.66f })
                g.drawLine (mx + mw * f, my, mx + mw * f, my + mh, 1.0f);

            // Black key notches
            g.setColour (iconColour.withAlpha (0.95f));
            for (float f : { 0.22f, 0.50f, 0.78f })
                g.fillRect (juce::Rectangle<float> (mx + mw * f - 0.8f, my, 1.6f, mh * 0.55f));

            currentX += midiW + 4.0f;
        }
        else
        {
            currentX += 2.0f;
        }

        // C. Responsive Button Text
        const auto textBounds = juce::Rectangle<float> (currentX, contentArea.getY(),
                                                        contentArea.getRight() - currentX,
                                                        contentArea.getHeight());

        if (textBounds.getWidth() > 10.0f)
        {
            const float fontSize = juce::jlimit (10.0f, 13.0f, h * 0.42f);
            g.setFont (juce::FontOptions (fontSize, juce::Font::bold));

            juce::Colour textColour = active ? juce::Colours::white
                                             : (shouldDrawButtonAsHighlighted ? juce::Colour (0xffffffff)
                                                                            : juce::Colour (0xffd0d8e4));
            g.setColour (textColour);
            g.drawFittedText (buttonText, textBounds.toNearestInt(),
                              juce::Justification::centredLeft, 1);
        }
    }

private:
    juce::String buttonText;
    bool isActiveState = false;
    std::function<bool()> activeSupplier;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioMidiButton)
};
