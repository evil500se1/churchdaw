#include "ChurchOrganLayout.h"

#include <algorithm>
#include <cmath>

namespace churchdaw::organ
{
namespace
{
using SizeClass = ChurchOrganLayout::SizeClass;

int gutterFor (SizeClass sizeClass) noexcept
{
    switch (sizeClass)
    {
        case SizeClass::compactPortrait:
        case SizeClass::compactLandscape: return 6;
        case SizeClass::medium:           return 10;
        case SizeClass::expanded:         return 12;
        case SizeClass::largeStage:       return 16;
    }

    return 10;
}

int clampInt (int value, int minimum, int maximum) noexcept
{
    return std::max (minimum, std::min (maximum, value));
}
} // namespace

ChurchOrganLayout::SizeClass ChurchOrganLayout::classify (juce::Rectangle<int> available) noexcept
{
    const auto width = available.getWidth();
    const auto height = available.getHeight();

    if (height <= 500 && width > height)
        return SizeClass::compactLandscape;

    if (width < 600 && height >= width)
        return SizeClass::compactPortrait;

    if (width >= 1600 && height >= 900)
        return SizeClass::largeStage;

    if (width <= 1040)
        return SizeClass::medium;

    return SizeClass::expanded;
}

ChurchOrganLayout::Shell ChurchOrganLayout::calculateShell (juce::Rectangle<int> available) noexcept
{
    Shell result;
    result.full = available;
    result.sizeClass = classify (available);

    int headerHeight = 72;
    int navigationHeight = 52;

    switch (result.sizeClass)
    {
        case SizeClass::compactPortrait:
            headerHeight = available.getWidth() <= 340 ? 54 : 58;
            navigationHeight = 54;
            result.navigationAtBottom = true;
            result.compactHeader = true;
            result.minimumTarget = 48;
            break;

        case SizeClass::compactLandscape:
            headerHeight = 44;
            navigationHeight = 48;
            result.navigationAtBottom = true;
            result.compactHeader = true;
            result.minimumTarget = 48;
            break;

        case SizeClass::medium:
            headerHeight = 64;
            navigationHeight = 56;
            result.navigationAtBottom = available.getWidth() < 760;
            result.compactHeader = available.getWidth() < 760;
            result.minimumTarget = 48;
            break;

        case SizeClass::expanded:
            headerHeight = 72;
            navigationHeight = 52;
            result.minimumTarget = 40;
            break;

        case SizeClass::largeStage:
            headerHeight = 82;
            navigationHeight = 58;
            result.minimumTarget = 44;
            break;
    }

    auto remaining = available;
    result.header = remaining.removeFromTop (std::min (headerHeight, remaining.getHeight()));

    if (result.navigationAtBottom)
    {
        result.navigation = remaining.removeFromBottom (
            std::min (navigationHeight, remaining.getHeight()));
        result.content = remaining;
    }
    else
    {
        result.navigation = remaining.removeFromTop (
            std::min (navigationHeight, remaining.getHeight()));
        result.content = remaining;
    }

    return result;
}

int ChurchOrganLayout::compactPortraitVisibleCards (juce::Rectangle<int> content) noexcept
{
    if (content.getWidth() <= 340 && content.getHeight() < 540)
        return 5;

    if (content.getHeight() < 590)
        return 6;

    return liveCardCount;
}

ChurchOrganLayout::Live ChurchOrganLayout::calculateLive (juce::Rectangle<int> content,
                                                           SizeClass sizeClass) noexcept
{
    Live result;
    result.content = content;
    result.visible.fill (false);

    const int gutter = gutterFor (sizeClass);
    auto inner = content.reduced (gutter);

    if (inner.isEmpty())
        return result;

    int drawbarHeight = 220;
    bool cardsBesideDrawbars = false;

    switch (sizeClass)
    {
        case SizeClass::compactPortrait:
            drawbarHeight = clampInt (
                juce::roundToInt (inner.getHeight() * (inner.getHeight() < 560 ? 0.46f : 0.54f)),
                180,
                410);
            result.visibleCardCount = compactPortraitVisibleCards (content);
            result.cardColumns = 2;
            result.drawbarsUseTwoRows = true;
            break;

        case SizeClass::compactLandscape:
            cardsBesideDrawbars = true;
            result.visibleCardCount = 5;
            result.cardColumns = 2;
            // Fileira UNICA classica quando ha largura (>=700 dp de area):
            // cada drawbar ganha a altura toda do card — curso longo, valor
            // visivel, leitura de hardware (revisao do dono, item 2). Duas
            // fileiras so onde nao cabem 9 lado a lado (640x360).
            result.drawbarsUseTwoRows = inner.getWidth() < 700;
            break;

        case SizeClass::medium:
            drawbarHeight = content.getHeight() > content.getWidth()
                                ? clampInt (juce::roundToInt (inner.getHeight() * 0.42f), 280, 475)
                                : clampInt (juce::roundToInt (inner.getHeight() * 0.48f), 190, 330);
            result.visibleCardCount = liveCardCount;
            result.cardColumns = content.getWidth() < 760 ? 2 : 3;
            result.drawbarsUseTwoRows = content.getWidth() < 480;
            break;

        case SizeClass::expanded:
            drawbarHeight = clampInt (juce::roundToInt (inner.getHeight() * 0.46f), 240, 360);
            result.visibleCardCount = liveCardCount;
            result.cardColumns = 4;
            break;

        case SizeClass::largeStage:
            drawbarHeight = clampInt (juce::roundToInt (inner.getHeight() * 0.48f), 330, 480);
            result.visibleCardCount = liveCardCount;
            result.cardColumns = 4;
            break;
    }

    if (cardsBesideDrawbars)
    {
        const int drawbarWidth = clampInt (
            juce::roundToInt (inner.getWidth() * 0.58f),
            std::min (300, inner.getWidth()),
            std::min (430, inner.getWidth()));
        result.drawbarCard = inner.removeFromLeft (drawbarWidth);
        if (! inner.isEmpty())
            inner.removeFromLeft (std::min (gutter, inner.getWidth()));
    }
    else
    {
        // Keep enough space for the required compact performance controls.
        const int minimumCardRows =
            sizeClass == SizeClass::compactPortrait ? 3 : 1;
        const int minimumCardArea =
            minimumCardRows * 56
            + std::max (0, minimumCardRows - 1) * gutter;
        drawbarHeight = std::min (
            drawbarHeight,
            std::max (80, inner.getHeight() - minimumCardArea - gutter));
        result.drawbarCard = inner.removeFromTop (drawbarHeight);

        if (! inner.isEmpty())
            inner.removeFromTop (std::min (gutter, inner.getHeight()));
    }

    // Compact Live always exposes Rotary, Worship, Drive, Reverb and
    // Expression. Percussion and scanner remain next in priority and appear
    // as soon as height permits. Larger surfaces keep the stage-oriented
    // grouping used by the desktop layout.
    constexpr std::array<LiveCard, liveCardCount> compactPriority
    {
        LiveCard::rotary,
        LiveCard::worship,
        LiveCard::drive,
        LiveCard::reverb,
        LiveCard::expression,
        LiveCard::percussion,
        LiveCard::chorusVibrato
    };
    constexpr std::array<LiveCard, liveCardCount> stagePriority
    {
        LiveCard::percussion,
        LiveCard::rotary,
        LiveCard::worship,
        LiveCard::expression,
        LiveCard::chorusVibrato,
        LiveCard::drive,
        LiveCard::reverb
    };
    const auto& priority =
        sizeClass == SizeClass::compactPortrait
            || sizeClass == SizeClass::compactLandscape
        ? compactPriority
        : stagePriority;

    result.visibleCardCount = std::min (result.visibleCardCount, liveCardCount);
    for (int index = 0; index < result.visibleCardCount; ++index)
        result.visible[static_cast<size_t> (priority[static_cast<size_t> (index)])] = true;

    const int rows = std::max (1,
        (result.visibleCardCount + result.cardColumns - 1) / result.cardColumns);
    const int totalHorizontalGutters = gutter * std::max (0, result.cardColumns - 1);
    const int totalVerticalGutters = gutter * std::max (0, rows - 1);
    const int cellWidth = std::max (1,
        (inner.getWidth() - totalHorizontalGutters) / std::max (1, result.cardColumns));
    const int cellHeight = std::max (1,
        (inner.getHeight() - totalVerticalGutters) / rows);

    int visibleIndex = 0;
    for (const auto card : priority)
    {
        const auto cardIndex = static_cast<size_t> (card);
        if (! result.visible[cardIndex])
            continue;

        const int row = visibleIndex / result.cardColumns;
        const int column = visibleIndex % result.cardColumns;
        int width = cellWidth;

        // Let the last item in a partially-filled row consume the remainder.
        if (visibleIndex == result.visibleCardCount - 1
            && (result.visibleCardCount % result.cardColumns) != 0)
        {
            const int usedColumns = result.visibleCardCount % result.cardColumns;
            width = inner.getWidth()
                    - column * (cellWidth + gutter)
                    - (usedColumns - column - 1) * (cellWidth + gutter);
        }

        result.cards[cardIndex] =
            { inner.getX() + column * (cellWidth + gutter),
              inner.getY() + row * (cellHeight + gutter),
              width,
              cellHeight };
        ++visibleIndex;
    }

    return result;
}

int ChurchOrganLayout::preferredEditingPageHeight (Page page,
                                                    int contentWidth,
                                                    int visibleHeight) noexcept
{
    const auto sizeClass = classify ({ 0, 0, contentWidth, visibleHeight });

    int minimum = visibleHeight;
    switch (page)
    {
        case Page::live:
            return visibleHeight;

        case Page::drawbars:
            minimum = (sizeClass == SizeClass::compactLandscape ? 650
                      : contentWidth < 600 ? 830 : 720);
            break;

        case Page::character:
            minimum = (contentWidth < 600 ? 1120 : contentWidth < 900 ? 900 : 720);
            break;

        case Page::rotary:
            minimum = (contentWidth < 600 ? 980 : contentWidth < 900 ? 820 : 700);
            break;

        case Page::worship:
            minimum = (contentWidth < 600 ? 1050 : contentWidth < 900 ? 860 : 720);
            break;

        case Page::presets:
            minimum = (contentWidth < 600 ? 940 : 760);
            break;

        case Page::count:
            break;
    }

    return std::max (visibleHeight, minimum);
}

juce::String ChurchOrganLayout::pageName (Page page)
{
    switch (page)
    {
        case Page::live:      return "LIVE";
        case Page::drawbars:  return "DRAWBARS";
        case Page::character: return "CHARACTER";
        case Page::rotary:    return "ROTARY";
        case Page::worship:   return "WORSHIP";
        case Page::presets:   return "PRESETS";
        case Page::count:     break;
    }

    return {};
}

juce::String ChurchOrganLayout::compactPageName (Page page)
{
    switch (page)
    {
        case Page::live:      return "LIVE";
        case Page::drawbars:  return "BARS";
        case Page::character: return "TONE";
        case Page::rotary:    return "ROTOR";
        case Page::worship:   return "WORSHIP";
        case Page::presets:   return "PRESETS";
        case Page::count:     break;
    }

    return {};
}
} // namespace churchdaw::organ
