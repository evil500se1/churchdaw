#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The desktop header allocates every region before placing components. This
// keeps device names readable without borrowing space from navigation.
struct DesktopHeaderLayout
{
    juce::Rectangle<int> brand, devices, engine, save, navigation;
    static constexpr int height = 100;

    static bool supports (juce::Rectangle<int> bounds) noexcept
    {
        return bounds.getWidth() >= 900 && bounds.getHeight() >= 580;
    }

    static DesktopHeaderLayout calculate (juce::Rectangle<int> bounds)
    {
        DesktopHeaderLayout result;
        auto top = bounds.withHeight (56).reduced (18, 4);
        const bool compact = bounds.getWidth() < 1100;
        result.brand = top.removeFromLeft (compact ? 164 : 184);
        top.removeFromLeft (compact ? 12 : 18);
        result.save = top.removeFromRight (compact ? 86 : 92).reduced (0, 5);
        top.removeFromRight (compact ? 10 : 12);
        result.engine = top.removeFromRight (compact ? 208 : 230);
        top.removeFromRight (compact ? 8 : 10);
        result.devices = top;
        result.navigation = { bounds.getX() + 18, bounds.getY() + 58,
                              juce::jmax (0, bounds.getWidth() - 36), 38 };
        return result;
    }
};
