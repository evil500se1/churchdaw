#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

//==============================================================================
// Tipografia empacotada do shell Church Live: Barlow, Barlow Condensed e
// JetBrains Mono (SIL OFL 1.1; licencas em Assets/Fonts). Compartilhada pelo
// NordLookAndFeel (fonte padrao do app mobile) e pelo skin do palco.
//==============================================================================
namespace ChurchLiveSkin
{

enum class Face { barlow, barlowSemi, barlowBold, condBold, condExtra, condExtraItalic, mono };

inline juce::Typeface::Ptr typefaceFor (Face face)
{
    static const juce::Typeface::Ptr faces[] = {
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowRegular_ttf, (size_t) BinaryData::BarlowRegular_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiBold_ttf, (size_t) BinaryData::BarlowSemiBold_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowBold_ttf, (size_t) BinaryData::BarlowBold_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedBold_ttf, (size_t) BinaryData::BarlowCondensedBold_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedExtraBold_ttf, (size_t) BinaryData::BarlowCondensedExtraBold_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedExtraBoldItalic_ttf, (size_t) BinaryData::BarlowCondensedExtraBoldItalic_ttfSize),
        juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoSemiBold_ttf, (size_t) BinaryData::JetBrainsMonoSemiBold_ttfSize)
    };
    return faces[(size_t) face];
}

// `px` e o tamanho em px de CSS (a referencia); o JUCE mede altura de linha
// (ascent + descent), ~1,2x o corpo da fonte.
inline float textWidth (const juce::Font& f, const juce::String& s)
{
    return juce::GlyphArrangement::getStringWidth (f, s) + f.getExtraKerningFactor() * f.getHeight() * (float) s.length();
}

inline juce::Font font (Face face, float px, float tracking = 0.0f)
{
    px = juce::jmax (1.0f, px);
    const auto typeface = typefaceFor (face);
    const auto options = typeface != nullptr ? juce::FontOptions (typeface)
                                              : juce::FontOptions (juce::Font::bold);
    return juce::Font (options.withHeight (px * 1.2f)).withExtraKerningFactor (tracking);
}

} // namespace ChurchLiveSkin
