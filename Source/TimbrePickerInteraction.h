#pragma once

#include <juce_core/juce_core.h>
#include <set>
#include <string>

namespace ChurchDAWTimbrePickerInteraction
{
    enum class SingleClickAction
    {
        none,
        select,
        toggleFavorite,
        startDownload
    };

    constexpr SingleClickAction singleClickAction (bool validRow,
                                                    bool header,
                                                    bool downloadable,
                                                    bool favoriteHit) noexcept
    {
        if (! validRow || header)
            return SingleClickAction::none;

        if (downloadable)
            return SingleClickAction::startDownload;

        return favoriteHit ? SingleClickAction::toggleFavorite
                           : SingleClickAction::select;
    }

    constexpr bool activateOnDoubleClick (bool validRow,
                                          bool header,
                                          bool downloadable) noexcept
    {
        return validRow && ! header && ! downloadable;
    }

    // Estado pequeno e independente da UI: downloads diferentes podem
    // coexistir, mas o mesmo timbre/pack nunca deve ser iniciado duas vezes.
    class DownloadStateRegistry
    {
    public:
        bool begin (const juce::String& id)
        {
            return active.insert (id.toStdString()).second;
        }

        void finish (const juce::String& id)
        {
            active.erase (id.toStdString());
        }

        bool contains (const juce::String& id) const
        {
            return active.find (id.toStdString()) != active.end();
        }

        std::size_t size() const noexcept { return active.size(); }

    private:
        std::set<std::string> active;
    };
}
