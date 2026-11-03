#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace ChurchDAWPluginProduct
{
// Keep major versions, mono/stereo editions and instrument/effect identities distinct.
// The format suffix is the only text we strip; IDs still target a precise instance.
inline juce::String key (const juce::PluginDescription& d)
{
    auto name = (d.name.isNotEmpty() ? d.name : d.descriptiveName).trim().toLowerCase();
    for (const auto& suffix : { " (au)", " (vst3)", " (vst)", " - au", " - vst3" })
        if (name.endsWith (suffix)) name = name.dropLastCharacters ((int) strlen (suffix)).trim();
    return d.manufacturerName.trim().toLowerCase() + ":" + name
        + (d.isInstrument ? ":instrument" : ":effect");
}
}
