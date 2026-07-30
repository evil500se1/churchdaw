#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace churchdaw::organ
{
// Church Organ Engine owns its visual language instead of mutating the host
// LookAndFeel.  This keeps the future processor/editor adapter self-contained.
struct ChurchOrganDesignTokens final
{
    // Surfaces
    static juce::Colour appBackground()       noexcept { return juce::Colour (0xff0e1014); }
    static juce::Colour surface1()            noexcept { return juce::Colour (0xff171b21); }
    static juce::Colour surface2()            noexcept { return juce::Colour (0xff1d222a); }
    static juce::Colour surface3()            noexcept { return juce::Colour (0xff252b34); }
    static juce::Colour elevatedSurface()     noexcept { return juce::Colour (0xff12151a); }
    static juce::Colour border()              noexcept { return juce::Colour (0xff343b47); }
    static juce::Colour borderStrong()        noexcept { return juce::Colour (0xff4a5260); }

    // Content
    static juce::Colour text()                noexcept { return juce::Colour (0xfff5f6f8); }
    static juce::Colour textSecondary()       noexcept { return juce::Colour (0xffa8afb9); }
    static juce::Colour textDisabled()        noexcept { return juce::Colour (0xff737b87); }

    // ChurchDAW and organ accents
    static juce::Colour churchRed()           noexcept { return juce::Colour (0xffea1747); }
    static juce::Colour churchRedPressed()    noexcept { return juce::Colour (0xffc80e38); }
    static juce::Colour organAmber()          noexcept { return juce::Colour (0xffd99234); }
    static juce::Colour organAmberLight()     noexcept { return juce::Colour (0xfff2b654); }
    static juce::Colour okGreen()             noexcept { return juce::Colour (0xff55c878); }
    static juce::Colour warning()             noexcept { return juce::Colour (0xffffb648); }
    static juce::Colour error()               noexcept { return juce::Colour (0xffff405c); }

    static juce::Colour focusRing()           noexcept { return juce::Colour (0xfff7d08a); }
    static juce::Colour rotarySlow()          noexcept { return organAmberLight(); }
    static juce::Colour rotaryFast()          noexcept { return churchRed(); }

    // Spacing uses logical pixels. JUCE's peer scale factor provides platform
    // DPI scaling, so the editor does not branch on operating-system names.
    static constexpr int space2xs = 3;
    static constexpr int spaceXs  = 6;
    static constexpr int spaceSm  = 9;
    static constexpr int spaceMd  = 12;
    static constexpr int spaceLg  = 16;
    static constexpr int spaceXl  = 22;

    static constexpr int minimumMouseTarget = 40;
    static constexpr int minimumTouchTarget = 48;

    static constexpr float radiusControl = 6.0f;
    static constexpr float radiusCard    = 10.0f;
    static constexpr float radiusPanel   = 14.0f;

    static juce::Font wordmarkFont (float height)
    {
        return juce::Font (juce::FontOptions (height, juce::Font::bold))
            .withExtraKerningFactor (-0.035f);
    }

    static juce::Font stageLabelFont (float height)
    {
        return juce::Font (juce::FontOptions (height, juce::Font::bold))
            .withExtraKerningFactor (0.13f);
    }

    static juce::Font bodyFont (float height)
    {
        return juce::Font (juce::FontOptions (height));
    }

    static juce::Font valueFont (float height)
    {
        return juce::Font (juce::FontOptions (height, juce::Font::bold));
    }

    static void fillCard (juce::Graphics& g,
                          juce::Rectangle<float> bounds,
                          juce::Colour colour = surface1(),
                          float radius = radiusCard)
    {
        g.setColour (colour);
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (border().withAlpha (0.86f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
    }

    static void drawFocusRing (juce::Graphics& g,
                               juce::Rectangle<float> bounds,
                               float radius = radiusControl)
    {
        g.setColour (focusRing());
        g.drawRoundedRectangle (bounds.reduced (1.5f), radius, 2.0f);
    }
};
} // namespace churchdaw::organ
