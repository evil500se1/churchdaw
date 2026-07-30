#pragma once

#include <array>

#include <juce_graphics/juce_graphics.h>

namespace churchdaw::organ
{
class ChurchOrganLayout final
{
public:
    enum class SizeClass
    {
        compactPortrait,
        compactLandscape,
        medium,
        expanded,
        largeStage
    };

    enum class Page
    {
        live,
        drawbars,
        character,
        rotary,
        worship,
        presets,
        count
    };

    enum class LiveCard
    {
        percussion,
        chorusVibrato,
        drive,
        rotary,
        expression,
        worship,
        reverb,
        count
    };

    static constexpr int pageCount = static_cast<int> (Page::count);
    static constexpr int liveCardCount = static_cast<int> (LiveCard::count);

    struct Shell
    {
        SizeClass sizeClass = SizeClass::expanded;
        juce::Rectangle<int> full;
        juce::Rectangle<int> header;
        juce::Rectangle<int> navigation;
        juce::Rectangle<int> content;
        bool navigationAtBottom = false;
        bool compactHeader = false;
        int minimumTarget = 40;
    };

    struct Live
    {
        juce::Rectangle<int> content;
        juce::Rectangle<int> drawbarCard;
        std::array<juce::Rectangle<int>, liveCardCount> cards {};
        std::array<bool, liveCardCount> visible {};
        int visibleCardCount = 0;
        int cardColumns = 1;
        bool drawbarsUseTwoRows = false;
    };

    static SizeClass classify (juce::Rectangle<int> available) noexcept;
    static Shell calculateShell (juce::Rectangle<int> available) noexcept;
    static Live calculateLive (juce::Rectangle<int> content,
                               SizeClass sizeClass) noexcept;

    static int preferredEditingPageHeight (Page page,
                                           int contentWidth,
                                           int visibleHeight) noexcept;

    static juce::String pageName (Page page);
    static juce::String compactPageName (Page page);

private:
    static int compactPortraitVisibleCards (juce::Rectangle<int> content) noexcept;
};
} // namespace churchdaw::organ
