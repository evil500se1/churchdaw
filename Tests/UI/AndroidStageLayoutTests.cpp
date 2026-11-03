#include "AndroidStageLayout.h"

#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::vector<int> forwardPages (int visible)
{
    std::vector<int> result { 0 };

    for (;;)
    {
        const int next = ChurchDAWAndroidStageLayout::nextPageStart (
            result.back(), visible, 14);

        if (next == result.back())
            return result;

        result.push_back (next);
    }
}

void expectPages (int visible, const std::vector<int>& expected)
{
    const auto actual = forwardPages (visible);
    expect (actual == expected, "forward page sequence is wrong");

    int page = actual.back();

    for (auto it = actual.rbegin() + 1; it != actual.rend(); ++it)
    {
        page = ChurchDAWAndroidStageLayout::previousPageStart (
            page, visible, 14);
        expect (page == *it, "backward page sequence is not symmetric");
    }
}
}

int main()
{
    using namespace ChurchDAWAndroidStageLayout;

    expect (stageChannelCountForWidth (320) == 2,
            "320 px stage must show two channels");
    expect (stageChannelCountForWidth (360) == 2,
            "360 px stage must show two channels");
    expect (stageChannelCountForWidth (568) == 4,
            "568 px stage must show four channels");
    expect (stageChannelCountForWidth (640) == 4,
            "640 px stage must show four channels");
    expect (stageChannelCountForWidth (820) == 6,
            "820 px stage must show six channels");
    expect (stageChannelCountForWidth (1153) == 6,
            "1153 px stage must show six channels");

    for (const int width : { 320, 360, 568, 640, 820, 1153 })
    {
        const int visible = stageChannelCountForWidth (width);
        const auto columns = calculateStageColumns (width, visible);
        expect (columns.masterWidth >= 80,
                "Master escaped its minimum stage width");
        expect (columns.channelWidth >= 100,
                "channel strip became too narrow");
        expect (columns.actionWidth >= 44,
                "Mute/Solo touch target became smaller than 44 px");

        constexpr int fixedChrome = 44 + 3 + 48 + 3 + 44 + 3;
        const int availableTabs = width - fixedChrome;
        const int focusedCount = focusedChannelCountForWidth (
            availableTabs, 3);
        const int focusedTabWidth =
            (availableTabs - 3 * (focusedCount - 1)) / focusedCount;
        expect (focusedTabWidth >= 44,
                "focused channel tab became smaller than 44 px");
    }

    expectPages (2, { 0, 2, 4, 6, 8, 10, 12 });
    expectPages (4, { 0, 4, 8, 10 });
    expectPages (6, { 0, 6, 8 });

    for (const int visible : { 2, 4, 6 })
        for (const int selected : { 0, 8, 13 })
        {
            const int page = pageContaining (selected, visible, 14);
            expect (page <= selected && selected < page + visible,
                    "selected channel is not visible on revealed page");
            expect (page == clampPageStart (page, visible, 14),
                    "revealed page is outside the valid range");
        }

    expect (clampPageStart (-10, 4, 14) == 0,
            "negative page did not clamp to zero");
    expect (clampPageStart (99, 4, 14) == 10,
            "last page did not clamp to C11-C14");
    expect (pageContaining (99, 4, 14) == 0,
            "invalid selected channel must fail closed");

    if (failures != 0)
        return 1;

    std::cout << "Android stage layout tests passed\n";
    return 0;
}
