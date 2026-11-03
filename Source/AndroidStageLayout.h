#pragma once

#include <algorithm>

namespace ChurchDAWAndroidStageLayout
{

inline int stageChannelCountForWidth (int width) noexcept
{
    if (width < 520)
        return 2;

    if (width < 760)
        return 4;

    return 6;
}

inline int focusedChannelCountForWidth (int availableWidth,
                                        int gap) noexcept
{
    const int safeGap = std::max (0, gap);
    const int count = (std::max (0, availableWidth) + safeGap)
                    / (44 + safeGap);
    return std::clamp (count, 2, 8);
}

inline int clampPageStart (int first, int visible, int total) noexcept
{
    if (visible <= 0 || total <= 0)
        return 0;

    return std::clamp (first, 0, std::max (0, total - visible));
}

inline int pageContaining (int selected, int visible, int total) noexcept
{
    if (visible <= 0 || total <= 0 || selected < 0 || selected >= total)
        return 0;

    return clampPageStart ((selected / visible) * visible, visible, total);
}

inline int nextPageStart (int first, int visible, int total) noexcept
{
    return clampPageStart (first + std::max (1, visible), visible, total);
}

inline int previousPageStart (int first, int visible, int total) noexcept
{
    if (first <= 0 || visible <= 0)
        return 0;

    // A ultima pagina pode comecar fora da grade regular: 14 canais com
    // seis visiveis termina em 8. Voltar de 8 deve cair em 6 e depois 0.
    const int previousRegular = ((first - 1) / visible) * visible;
    return clampPageStart (previousRegular, visible, total);
}

struct StageColumns
{
    int masterWidth = 0;
    int channelAreaWidth = 0;
    int channelWidth = 0;
    int actionWidth = 0;
};

inline StageColumns calculateStageColumns (int totalWidth,
                                           int visibleChannels) noexcept
{
    StageColumns result;
    constexpr int gap = 4;
    const int safeWidth = std::max (0, totalWidth);
    const int safeChannels = std::max (1, visibleChannels);
    const int desiredMaster = std::clamp ((safeWidth * 18 + 50) / 100,
                                          96, 152);
    const int maximumMaster = std::max (
        80, safeWidth - gap - safeChannels * 100);
    result.masterWidth = std::min (desiredMaster, maximumMaster);
    result.masterWidth = std::clamp (result.masterWidth, 0, safeWidth);
    result.channelAreaWidth = std::max (
        0, safeWidth - result.masterWidth - gap);
    result.channelWidth = result.channelAreaWidth / safeChannels;

    // card reduced(1,0), inner reduced(4), quatro pixels entre M/S.
    const int actionRowWidth = std::max (0, result.channelWidth - 10);
    result.actionWidth = std::max (0, (actionRowWidth - 4) / 2);
    return result;
}

} // namespace ChurchDAWAndroidStageLayout
