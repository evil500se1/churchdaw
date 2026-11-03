#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// Static artwork is decoded on the message thread and shared by every view.
// It never enters the audio callback or needs the network at runtime.
namespace ChurchDAWArtwork
{
inline const juce::Image& piano()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_grand_piano_png, BinaryData::premium_grand_piano_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& electricPiano()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_electric_piano_png, BinaryData::premium_electric_piano_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& organ()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_tonewheel_organ_png, BinaryData::premium_tonewheel_organ_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& synth()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_synth_png, BinaryData::premium_synth_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& strings()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_strings_png, BinaryData::premium_strings_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& bells()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_bells_png, BinaryData::premium_bells_pngSize)
        .rescaled (384, 384, juce::Graphics::highResamplingQuality);
    return image;
}
inline bool isElectricPiano (const juce::String& name)
{
    const auto n = name.toLowerCase();
    return n.contains ("electric") || n.contains ("tines") || n.contains ("rhodes")
        || n.startsWith ("ep ") || n.endsWith (" ep") || n.contains ("dx legend");
}
inline const juce::Image& graphite()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_graphite_png, BinaryData::premium_graphite_pngSize)
        .rescaled (768, 512, juce::Graphics::highResamplingQuality);
    return image;
}
inline const juce::Image& bigSky()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_bigsky_png, BinaryData::premium_bigsky_pngSize);
    return image;
}
inline const juce::Image& pianoteq()
{
    static const auto image = juce::ImageCache::getFromMemory (
        BinaryData::premium_pianoteq_png, BinaryData::premium_pianoteq_pngSize);
    return image;
}
inline void texture (juce::Graphics& g, juce::Rectangle<float> area, float opacity = 0.24f)
{
    if (area.isEmpty()) return;
    juce::Graphics::ScopedSaveState state (g);
    g.setOpacity (opacity);
    g.drawImage (graphite(), area, juce::RectanglePlacement::stretchToFit);
}
inline void image (juce::Graphics& g, const juce::Image& art, juce::Rectangle<float> area)
{
    if (area.isEmpty() || ! art.isValid()) return;
    juce::Graphics::ScopedSaveState state (g);
    g.setOpacity (1.0f);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (art, area, juce::RectanglePlacement::centred);
}
inline bool hasProduct (const juce::String& name)
{
    return name.containsIgnoreCase ("bigsky") || name.containsIgnoreCase ("pianoteq")
            || name.containsIgnoreCase ("keyscape");
}
inline void plugin (juce::Graphics& g, juce::Rectangle<float> area,
                    const juce::String& name, bool instrument)
{
    if (name.containsIgnoreCase ("bigsky")) { image (g, bigSky(), area.reduced (area.getHeight() < 70 ? 0 : 8)); return; }
    if (name.containsIgnoreCase ("pianoteq")) { image (g, pianoteq(), area.reduced (area.getHeight() < 70 ? 0 : 8)); return; }
    if (instrument) { image (g, piano(), area.reduced (4)); return; }

    // Static frequency-response illustration for plugins without product artwork.
    // No signal or level is represented by this decorative curve.
    juce::Graphics::ScopedSaveState state (g);
    auto plot = area.reduced (area.getWidth() * 0.13f, area.getHeight() * 0.20f);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    for (int i = 1; i < 5; ++i)
        g.drawVerticalLine (juce::roundToInt (plot.getX() + plot.getWidth() * i / 5), plot.getY(), plot.getBottom());
    for (int i = 1; i < 4; ++i)
        g.drawHorizontalLine (juce::roundToInt (plot.getY() + plot.getHeight() * i / 4), plot.getX(), plot.getRight());
    juce::Path curve;
    const bool reverb = name.containsIgnoreCase ("reverb") || name.containsIgnoreCase ("matrix");
    curve.startNewSubPath (plot.getX(), plot.getCentreY() + plot.getHeight() * 0.22f);
    curve.cubicTo (plot.getX() + plot.getWidth() * 0.28f, plot.getBottom(),
                   plot.getCentreX() - plot.getWidth() * 0.08f, plot.getY() - plot.getHeight() * 0.3f,
                   plot.getCentreX() + plot.getWidth() * 0.12f, plot.getCentreY());
    curve.cubicTo (plot.getRight() - plot.getWidth() * 0.19f, plot.getBottom(),
                   plot.getRight() - plot.getWidth() * 0.14f, plot.getCentreY(),
                   plot.getRight(), reverb ? plot.getBottom() : plot.getCentreY());
    const auto tint = reverb ? juce::Colour (0xffaaa3d7) : juce::Colour (0xff91b9c4);
    g.setColour (tint.withAlpha (0.08f));
    g.strokePath (curve, juce::PathStrokeType (9.0f));
    g.setColour (tint);
    g.strokePath (curve, juce::PathStrokeType (2.0f));
}
}
