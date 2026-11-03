#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ChurchDAWPluginGallery
{
struct Layout
{
    int width = 0, columns = 1, cardWidth = 0, cardHeight = 208, gap = 14;
    juce::Rectangle<int> item (int index) const
    {
        return { (index % columns) * (cardWidth + gap),
                 (index / columns) * (cardHeight + gap), cardWidth, cardHeight };
    }
    int height (int count) const
    {
        return count <= 0 ? 0 : ((count + columns - 1) / columns) * (cardHeight + gap) - gap;
    }
};
inline Layout forWidth (int width)
{
    Layout l;
    l.width = juce::jmax (1, width);
    l.columns = juce::jlimit (1, 4, (l.width + l.gap) / 216);
    l.cardWidth = juce::jmax (1, (l.width - (l.columns - 1) * l.gap) / l.columns);
    return l;
}
}
