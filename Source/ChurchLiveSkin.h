#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MobileShellConfig.h"
#include "NordLookAndFeel.h"
#include "ChurchLiveFonts.h"

//==============================================================================
// Skin "Church Live" do shell mobile em paisagem (trilho de navegacao + barra
// superior + mesa de canais). So camada visual: tokens, tipografia empacotada,
// medidas relativas, geometria da barra superior (compartilhada entre o
// MainComponent e o TimbresEditor) e um LookAndFeel que so age em componentes
// marcados com a propriedade "churchSkin". Todo o resto cai no NordLookAndFeel.
//
// A referencia de design e 1600x900. Toda medida em "px de referencia" passa
// por Metrics::px/fpx, que escala pela altura/largura real e aplica um piso
// (alvo de toque e legibilidade nao encolhem junto com a tela).
//==============================================================================
namespace ChurchLiveSkin
{

//==============================================================================
// Tokens
inline juce::Colour bg()          { return juce::Colour (0xff0a0a0d); }
inline juce::Colour panel()       { return juce::Colour (0xff14141a); }
inline juce::Colour elevated()    { return juce::Colour (0xff1c1c24); }
inline juce::Colour inset()       { return juce::Colour (0xff0d0d11); }
inline juce::Colour line()        { return juce::Colour (0xff2b2b36); }
inline juce::Colour text()        { return juce::Colour (0xfff5f3f4); }
inline juce::Colour textSec()     { return juce::Colour (0xffa3a1ad); }
inline juce::Colour textMute()    { return juce::Colour (0xff8e8c99); }
inline juce::Colour redFill()     { return juce::Colour (0xffd92b40); }
inline juce::Colour redText()     { return juce::Colour (0xffff6b7d); }
inline juce::Colour accent()      { return juce::Colour (0xffff5a6e); }
inline juce::Colour accentTop()   { return juce::Colour (0xffff4d63); }
inline juce::Colour accentSoft()  { return juce::Colour (0xffff8a98); }
inline juce::Colour okGreen()     { return juce::Colour (0xff3ddc84); }
inline juce::Colour railBg()      { return juce::Colour (0xff0f0f13); }
inline juce::Colour railLine()    { return juce::Colour (0xff22222b); }
inline juce::Colour railActive()  { return juce::Colour (0xff2a1219); }
inline juce::Colour trackBg()     { return juce::Colour (0xff08080b); }
inline juce::Colour offGrey()     { return juce::Colour (0xff3a3a46); }
inline juce::Colour dashed()      { return juce::Colour (0xff34343f); }
inline juce::Colour muteAmber()   { return juce::Colour (0xfff4b73b); }
inline juce::Colour soloBlue()    { return juce::Colour (0xff4fa8ff); }
inline juce::Colour meterGreen()  { return juce::Colour (0xff2fcb74); }
inline juce::Colour meterAmber()  { return juce::Colour (0xfff4b73b); }
inline juce::Colour meterRed()    { return juce::Colour (0xfff2495c); }

//==============================================================================
//==============================================================================
// Escala relativa a referencia 1600x900.
struct Metrics
{
    float u = 1.0f;

    int px (float ref, int floorPx = 0) const noexcept
    {
        return juce::jmax (floorPx, juce::roundToInt (ref * u));
    }

    float fpx (float ref, float floorPx = 0.0f) const noexcept
    {
        return juce::jmax (floorPx, ref * u);
    }
};

inline Metrics metricsFor (int totalWidth, int totalHeight) noexcept
{
    Metrics m;
    m.u = juce::jlimit (0.44f, 1.40f, juce::jmin ((float) totalWidth / 1600.0f,
                                                  (float) totalHeight / 900.0f));
    return m;
}

// O MainComponent publica a escala no editor por esta propriedade; assim os dois
// concordam sem o editor precisar conhecer a janela inteira.
inline Metrics metricsOf (const juce::Component& component)
{
    Metrics m;
    const auto value = component.getProperties().getWithDefault ("churchU", 0.0);
    m.u = (double) value > 0.0 ? (float) (double) value
                               : metricsFor (component.getWidth() + 104, component.getHeight()).u;
    return m;
}

inline int railWidth (const Metrics& m)  { return m.px (104, 56); }
inline int topBarHeight (const Metrics& m) { return m.px (84, 52); }

// So o layout paisagem usa o shell novo (trilho + barra unica).
inline bool wantsShell (int width, int height) noexcept
{
    return width >= 640 && height >= 300 && width * 10 >= height * 14;
}

//==============================================================================
// Barra superior: MIDI | Pronto | Salvo | < > | Editar canal. A geometria e uma
// funcao pura da area da barra; MainComponent posiciona o chip MIDI e o Salvo,
// o TimbresEditor posiciona status, setas e o botao de modo.
struct TopBar
{
    juce::Rectangle<int> wordmark, midi, status, save, prev, next, edit;
};

inline TopBar layoutTopBar (juce::Rectangle<int> bar, const Metrics& m, bool arrows)
{
    TopBar t;
    const int gap = m.px (12, 6);
    const int chipH = juce::jmin (bar.getHeight(), m.px (56, 40));
    auto row = bar.reduced (m.px (28, 8), 0).withSizeKeepingCentre (
        juce::jmax (1, bar.getWidth() - m.px (56, 16)), chipH);

    t.edit = row.removeFromRight (m.px (186, 118));
    row.removeFromRight (gap);

    if (arrows)
    {
        t.next = row.removeFromRight (m.px (56, 40));
        row.removeFromRight (m.px (8, 4));
        t.prev = row.removeFromRight (m.px (56, 40));
        row.removeFromRight (gap);
    }

    t.save = row.removeFromRight (m.px (124, 84));
    row.removeFromRight (gap);
    t.status = row.removeFromRight (m.px (156, 108));
    row.removeFromRight (gap);
    t.midi = row.removeFromRight (m.px (176, 118));
    row.removeFromRight (gap);
    t.wordmark = row;
    return t;
}

//==============================================================================
// Pecas desenhadas (mesmos tokens em qualquer tela).
inline void drawWordmark (juce::Graphics& g, juce::Rectangle<int> area, const Metrics& m,
                         const char* accentWord = "LIVE", bool allowSubtitle = true, bool spaced = true)
{
    if (area.getWidth() < 40)
        return;

    const float titlePx = m.fpx (46, 20.0f);
    const bool showSubtitle = allowSubtitle && m.fpx (12) >= 8.0f && area.getHeight() >= titlePx * 1.7f;
    auto r = area.toFloat();

    // icone de barras (equalizador) a esquerda do nome
    const float iconH = titlePx * 0.62f;
    const float barW = juce::jmax (2.0f, iconH * 0.14f);
    float x = r.getX();
    const float cy = r.getCentreY() - (showSubtitle ? titlePx * 0.12f : 0.0f);
    const float heights[] = { 0.45f, 0.85f, 0.65f, 1.0f, 0.55f };
    g.setColour (accent());
    for (int i = 0; i < 5; ++i)
    {
        const float h = iconH * heights[i];
        g.fillRoundedRectangle (x, cy - h * 0.5f, barW, h, barW * 0.5f);
        x += barW * 1.9f;
    }
    x += barW * 1.4f;

    const auto titleFont = font (Face::condExtraItalic, titlePx, 0.01f);
    const float churchW = juce::GlyphArrangement::getStringWidth (titleFont, spaced ? "CHURCH " : "CHURCH");
    const float titleH = titlePx * 1.25f;
    const auto titleRect = juce::Rectangle<float> (x, cy - titleH * 0.5f,
                                                   juce::jmax (1.0f, r.getRight() - x), titleH);
    g.setFont (titleFont);
    g.setColour (text());
    g.drawText ("CHURCH", titleRect.toNearestInt(), juce::Justification::centredLeft, false);
    g.setColour (accent());
    g.drawText (accentWord, titleRect.withTrimmedLeft (churchW).toNearestInt(),
                juce::Justification::centredLeft, false);

    if (showSubtitle)
    {
        g.setFont (font (Face::barlow, m.fpx (12, 8.0f), 0.28f));
        g.setColour (textSec());
        g.drawText (juce::String::fromUTF8 ("TECLADO \xc2\xb7 TIMBRES \xc2\xb7 LIVE \xc2\xb7 IGREJA"),
                    juce::Rectangle<float> (x, cy + titleH * 0.42f,
                                            juce::jmax (1.0f, r.getRight() - x), m.fpx (16, 11.0f))
                        .toNearestInt(),
                    juce::Justification::centredLeft, false);
    }
}

// Item de trilho ativo. O desktop opta por seguir o estado de toggle do botao
// (churchActiveFromToggle); o mobile calcula "churchActive" no layout.
inline bool railIsActive (const juce::Button& b)
{
    return (bool) b.getProperties().getWithDefault ("churchActiveFromToggle", false)
               ? b.getToggleState()
               : (bool) b.getProperties().getWithDefault ("churchActive", false);
}

inline void drawChevron (juce::Graphics& g, juce::Rectangle<float> r, int direction /* 0 baixo, -1 esq, 1 dir */,
                         float thickness)
{
    juce::Path p;
    const auto c = r.getCentre();
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
    if (direction == 0)
    {
        p.startNewSubPath (c.x - s, c.y - s * 0.5f);
        p.lineTo (c.x, c.y + s * 0.5f);
        p.lineTo (c.x + s, c.y - s * 0.5f);
    }
    else
    {
        const float d = (float) direction;
        p.startNewSubPath (c.x - d * s * 0.5f, c.y - s);
        p.lineTo (c.x + d * s * 0.5f, c.y);
        p.lineTo (c.x - d * s * 0.5f, c.y + s);
    }
    g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded));
}

inline void drawPencil (juce::Graphics& g, juce::Rectangle<float> r, float thickness)
{
    const float x = r.getX(), y = r.getY(), s = r.getWidth();
    g.drawLine (x + s * 0.20f, y + s * 0.80f, x + s * 0.74f, y + s * 0.26f, thickness * 2.2f);
    juce::Path tip;
    tip.addTriangle (x + s * 0.12f, y + s * 0.90f, x + s * 0.20f, y + s * 0.66f, x + s * 0.34f, y + s * 0.80f);
    g.fillPath (tip);
    g.drawLine (x + s * 0.60f, y + s * 0.20f, x + s * 0.80f, y + s * 0.40f, thickness);
}

inline void drawPlus (juce::Graphics& g, juce::Point<float> c, float arm, float thickness)
{
    g.fillRoundedRectangle (c.x - arm, c.y - thickness * 0.5f, arm * 2.0f, thickness, thickness * 0.5f);
    g.fillRoundedRectangle (c.x - thickness * 0.5f, c.y - arm, thickness, arm * 2.0f, thickness * 0.5f);
}

inline void drawDashedRoundRect (juce::Graphics& g, juce::Rectangle<float> r, float radius,
                                 float thickness, juce::Colour colour)
{
    juce::Path outline, stroke;
    outline.addRoundedRectangle (r, radius);
    const float dashes[] = { thickness * 3.0f, thickness * 3.0f };
    juce::PathStrokeType (thickness).createDashedStroke (stroke, outline, dashes, 2);
    g.setColour (colour);
    g.fillPath (stroke);
}

inline void drawDashedCircle (juce::Graphics& g, juce::Point<float> c, float radius,
                              float thickness, juce::Colour colour)
{
    juce::Path outline, stroke;
    outline.addEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
    const float dashes[] = { thickness * 3.0f, thickness * 3.0f };
    juce::PathStrokeType (thickness).createDashedStroke (stroke, outline, dashes, 2);
    g.setColour (colour);
    g.fillPath (stroke);
}

// Pilula de status ("Pronto" / "Carregando" / "Erro"): icone + texto, nunca so cor.
inline void drawStatusChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& label,
                            juce::Colour tone, const Metrics& m)
{
    const float radius = r.getHeight() * 0.5f;
    g.setColour (tone.withAlpha (0.14f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (tone.withAlpha (0.55f));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);

    const float dot = m.fpx (12, 8.0f);
    const float padX = m.fpx (16, 8.0f);
    const float cx = r.getX() + padX + dot * 0.5f;
    g.setColour (tone.withAlpha (0.30f));
    g.fillEllipse (cx - dot, r.getCentreY() - dot, dot * 2.0f, dot * 2.0f);
    g.setColour (tone);
    g.fillEllipse (cx - dot * 0.5f, r.getCentreY() - dot * 0.5f, dot, dot);

    g.setColour (text());
    g.setFont (font (Face::barlowSemi, m.fpx (20, 12.0f)));
    g.drawFittedText (label, r.toNearestInt().withTrimmedLeft (juce::roundToInt (padX + dot + m.fpx (10, 5.0f)))
                                 .withTrimmedRight (juce::roundToInt (padX * 0.7f)),
                      juce::Justification::centredLeft, 1, 0.8f);
}

//==============================================================================
// LookAndFeel que so age em componentes marcados (propriedade "churchSkin").
class LookAndFeel : public NordLookAndFeel
{
public:
    static juce::String role (const juce::Component& c)
    {
        return c.getProperties().getWithDefault ("churchSkin", juce::var()).toString();
    }

    static Metrics metrics (const juce::Component& c)
    {
        Metrics m;
        const auto v = c.getProperties().getWithDefault ("churchU", 1.0);
        m.u = juce::jlimit (0.3f, 2.0f, (float) (double) v);
        return m;
    }

    //----------------------------------------------------------------------
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc,
                               bool highlighted, bool down) override
    {
        const auto r = role (b);
        if (r.isEmpty())
        {
            NordLookAndFeel::drawButtonBackground (g, b, bgc, highlighted, down);
            return;
        }

        const auto m = metrics (b);
        const float alpha = b.isEnabled() ? 1.0f : 0.40f;
        const auto bounds = b.getLocalBounds().toFloat();
        const bool on = r == "rail" ? railIsActive (b)
                                    : b.getToggleState();

        if (r == "switch")
        {
            const float h = bounds.getHeight();
            g.setColour ((on ? redFill() : juce::Colour (0xff2c2c36)).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds, h * 0.5f);
            const float inset = h * 0.125f, d = h - inset * 2.0f;
            const float kx = on ? bounds.getRight() - inset - d : bounds.getX() + inset;
            g.setColour ((on ? juce::Colours::white : juce::Colour (0xffb9b7c2)).withMultipliedAlpha (alpha));
            g.fillEllipse (kx, bounds.getY() + inset, d, d);
        }
        else if (r == "mute" || r == "solo")
        {
            const float radius = m.fpx (14, 6.0f);
            const auto lit = r == "mute" ? muteAmber() : soloBlue();
            g.setColour (on ? lit : (down ? juce::Colour (0xff2d2d38) : juce::Colour (0xff23232c)));
            g.fillRoundedRectangle (bounds, radius);
            if (! on)
            {
                g.setColour (line().withAlpha (0.8f));
                g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
            }
        }
        else if (r == "panic" || r == "edit")
        {
            const float radius = m.fpx (r == "panic" ? 18.0f : 16.0f, 8.0f);
            const auto top = r == "panic" ? juce::Colour (0xffe8384f) : juce::Colour (0xffe03048);
            const auto bottom = r == "panic" ? juce::Colour (0xffb01b2e) : juce::Colour (0xffb82035);
            g.setGradientFill (juce::ColourGradient ((down ? top.darker (0.15f) : top).withMultipliedAlpha (alpha),
                                                     0.0f, bounds.getY(),
                                                     (down ? bottom.darker (0.15f) : bottom).withMultipliedAlpha (alpha),
                                                     0.0f, bounds.getBottom(), false));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour (juce::Colour (0xffff7a8a).withAlpha ((r == "panic" ? 0.95f : 0.55f) * alpha));
            g.drawRoundedRectangle (bounds.reduced (0.75f), radius, r == "panic" ? 1.5f : 1.0f);
        }
        else if (r == "arrow" || r == "chip" || r == "pill")
        {
            const float radius = m.fpx (14, 6.0f);
            g.setColour ((down ? juce::Colour (0xff23232c) : panel()).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour (line().withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
        }
        else if (r == "rail")
        {
            const auto item = bounds.reduced (m.fpx (8, 3.0f), m.fpx (2, 1.0f));
            const float radius = m.fpx (14, 6.0f);
            if (on)
            {
                g.setColour (railActive());
                g.fillRoundedRectangle (item, radius);
                g.setColour (accent());
                g.fillRoundedRectangle (bounds.getX(), item.getY() + item.getHeight() * 0.18f,
                                        juce::jmax (2.0f, m.fpx (3, 2.0f)), item.getHeight() * 0.64f, 1.5f);
            }
            else if (highlighted || down)
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.07f : 0.04f));
                g.fillRoundedRectangle (item, radius);
            }
        }
        // "slot": o editor pinta a baia vazia; o botao so recebe o toque.
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool down) override
    {
        const auto r = role (b);
        if (r.isEmpty())
        {
            NordLookAndFeel::drawButtonText (g, b, highlighted, down);
            return;
        }

        const auto m = metrics (b);
        const float alpha = b.isEnabled() ? 1.0f : 0.40f;
        const auto bounds = b.getLocalBounds().toFloat();
        const bool on = r == "rail" ? railIsActive (b)
                                    : b.getToggleState();
        const auto label = b.getButtonText();

        if (r == "switch" && ! b.getProperties().getWithDefault ("churchNoLabel", false))
        {
            const float h = bounds.getHeight();
            g.setFont (font (Face::barlowBold, m.fpx (16, 10.0f), 0.02f));
            g.setColour ((on ? juce::Colours::white : textSec()).withMultipliedAlpha (alpha));
            auto textArea = bounds.reduced (h * 0.27f, 0.0f);
            g.drawText (on ? "ON" : "OFF", textArea.toNearestInt(),
                        on ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
        }
        else if (r == "mute" || r == "solo")
        {
            g.setFont (font (Face::barlowBold, m.fpx (21, 13.0f)));
            g.setColour (on ? bg() : text());
            g.drawText (label, bounds.toNearestInt(), juce::Justification::centred, false);
        }
        else if (r == "panic")
        {
            const float iconS = m.fpx (34, 16.0f);
            const auto f = font (Face::condExtra, m.fpx (36, 15.0f), 0.14f);
            const float tw = textWidth (f, label);
            const float gapW = m.fpx (10, 5.0f);
            const float total = juce::jmin (bounds.getWidth() - 4.0f, iconS + gapW + tw);
            const float x0 = bounds.getCentreX() - total * 0.5f;
            g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
            NordLookAndFeel::drawDesktopNavigationIcon (g, "navSaveFailed",
                {x0, bounds.getCentreY() - iconS * 0.5f, iconS, iconS}, false);
            g.setFont (f);
            g.drawText (label, juce::Rectangle<float> (x0 + iconS + gapW, bounds.getY(),
                                                       bounds.getRight() - (x0 + iconS + gapW), bounds.getHeight())
                                   .toNearestInt(),
                        juce::Justification::centredLeft, false);
        }
        else if (r == "edit")
        {
            const float iconS = m.fpx (24, 14.0f);
            const auto f = font (Face::barlowBold, m.fpx (21, 12.5f));
            const float tw = textWidth (f, label);
            const float gapW = m.fpx (10, 5.0f);
            const float total = juce::jmin (bounds.getWidth() - 6.0f, iconS + gapW + tw);
            const float x0 = bounds.getCentreX() - total * 0.5f;
            const juce::Rectangle<float> icon (x0, bounds.getCentreY() - iconS * 0.5f, iconS, iconS);
            g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
            if (label.containsIgnoreCase ("palco"))
                NordLookAndFeel::drawDesktopNavigationIcon (g, "navPad", icon, false);
            else
                drawPencil (g, icon, juce::jmax (1.2f, iconS * 0.09f));
            g.setFont (f);
            g.drawText (label, juce::Rectangle<float> (x0 + iconS + gapW, bounds.getY(),
                                                       juce::jmax (1.0f, bounds.getRight() - (x0 + iconS + gapW)), bounds.getHeight())
                                   .toNearestInt(),
                        juce::Justification::centredLeft, false);
        }
        else if (r == "pill")
        {
            g.setFont (font (Face::barlowBold, m.fpx (18, 11.0f), 0.04f));
            g.setColour (text().withMultipliedAlpha (alpha));
            g.drawFittedText (label, bounds.reduced (m.fpx (8, 3.0f), 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
        else if (r == "arrow")
        {
            g.setColour (text().withMultipliedAlpha (alpha));
            const float s = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.13f;
            drawChevron (g, bounds.withSizeKeepingCentre (s * 2.0f, s * 2.0f),
                         (int) b.getProperties().getWithDefault ("churchDir", 1),
                         juce::jmax (1.8f, m.fpx (3, 1.8f)));
        }
        else if (r == "chip")
        {
            // Chip "Salvo": icone de estado + texto (o estado nunca depende so de cor).
            const auto status = NordLookAndFeel::saveStatusLabel (b);
            const bool failed = status.containsIgnoreCase ("falhou");
            const auto tone = failed ? b.findColour (juce::TextButton::textColourOffId) : okGreen();
            const float iconS = m.fpx (24, 14.0f);
            const auto f = font (Face::barlowSemi, m.fpx (20, 12.0f));
            const float tw = textWidth (f, status);
            const float gapW = m.fpx (10, 5.0f);
            const float total = juce::jmin (bounds.getWidth() - 8.0f, iconS + gapW + tw);
            const float x0 = bounds.getCentreX() - total * 0.5f;
            g.setColour (tone.withMultipliedAlpha (alpha));
            NordLookAndFeel::drawDesktopNavigationIcon (g, failed ? "navSaveFailed" : "navSave",
                {x0, bounds.getCentreY() - iconS * 0.5f, iconS, iconS}, false);
            g.setColour (text().withMultipliedAlpha (alpha));
            g.setFont (f);
            g.drawFittedText (status, juce::Rectangle<float> (x0 + iconS + gapW, bounds.getY(),
                                                              juce::jmax (1.0f, bounds.getRight() - (x0 + iconS + gapW)), bounds.getHeight())
                                          .toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.8f);
        }
        else if (r == "rail")
        {
            const float iconS = m.fpx (28, 18.0f);
            const float labelPx = m.fpx (13, 9.0f);
            auto area = bounds.reduced (m.fpx (4, 1.0f), m.fpx (6, 2.0f));
            const float blockH = iconS + m.fpx (6, 2.0f) + labelPx * 1.25f;
            const float y0 = area.getCentreY() - blockH * 0.5f;
            const auto icon = juce::Rectangle<float> (area.getCentreX() - iconS * 0.5f, y0, iconS, iconS);
            g.setColour ((on ? accentSoft() : textSec()).withMultipliedAlpha (alpha));
            NordLookAndFeel::drawDesktopNavigationIcon (
                g, b.getProperties().getWithDefault ("churchIcon", b.getComponentID()).toString(), icon, false);
            g.setColour ((on ? juce::Colours::white : textSec()).withMultipliedAlpha (alpha));
            g.setFont (font (Face::barlowSemi, labelPx));
            g.drawFittedText (label, juce::Rectangle<float> (area.getX(), icon.getBottom() + m.fpx (4, 1.0f),
                                                             area.getWidth(), labelPx * 1.4f).toNearestInt(),
                              juce::Justification::centred, 1, 0.7f);
        }
    }

    //----------------------------------------------------------------------
    // Seletor de timbre: o editor pinta o cartao; o ComboBox so recebe o toque.
    void drawComboBox (juce::Graphics& g, int w, int h, bool isDown, int bx, int by, int bw, int bh,
                       juce::ComboBox& box) override
    {
        if (role (box).isEmpty())
            NordLookAndFeel::drawComboBox (g, w, h, isDown, bx, by, bw, bh, box);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        if (role (box).isEmpty())
            NordLookAndFeel::positionComboBoxText (box, label);
        else
            label.setBounds (0, 0, 0, 0);
    }

    // Rotulos pintados pelo editor (o componente fica para acessibilidade).
    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        if (role (label) != "hidden")
            NordLookAndFeel::drawLabel (g, label);
    }

    //----------------------------------------------------------------------
    static juce::Rectangle<float> handleSize (const juce::Slider& s)
    {
        const auto m = metrics (s);
        const bool master = role (s) == "masterFader";
        const float w = juce::jmin (m.fpx (master ? 100.0f : 92.0f, 30.0f),
                                    (float) s.getWidth() - 2.0f);
        const float h = juce::jmin (m.fpx (master ? 56.0f : 48.0f, 24.0f), (float) s.getHeight() * 0.5f);
        return { 0.0f, 0.0f, w, h };
    }

    int getSliderThumbRadius (juce::Slider& s) override
    {
        if (role (s).endsWith ("Fader") && s.isVertical())
            return juce::roundToInt (handleSize (s).getHeight() * 0.5f);
        return NordLookAndFeel::getSliderThumbRadius (s);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                           juce::Slider& s) override
    {
        const auto r = role (s);
        if (! (r == "channelFader" || r == "masterFader") || style != juce::Slider::LinearVertical)
        {
            NordLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos,
                                               maxSliderPos, style, s);
            return;
        }

        const auto m = metrics (s);
        const bool master = r == "masterFader";
        const float alpha = s.isEnabled() ? 1.0f : 0.40f;
        const float labelW = (float) (int) s.getProperties().getWithDefault ("churchLabelW", 0);
        const float cx = (float) x + labelW + ((float) width - labelW) * 0.5f;
        const auto handle = handleSize (s);
        juce::ignoreUnused (minSliderPos, maxSliderPos);   // so valem para sliders de duas pontas
        // curso real da calha: do maximo (topo) ao minimo (base)
        const float top = (float) s.getPositionOfValue (s.getMaximum());
        const float bottom = (float) s.getPositionOfValue (s.getMinimum());

        // marcas de escala (hairlines atras do cap) e rotulos de dB do master
        const bool channelGain = ! master;
        auto posOf = [&] (double db) -> float
        {
            const double v = channelGain ? (double) juce::Decibels::decibelsToGain (db) : db;
            return (float) s.getPositionOfValue (v);
        };
        struct Mark { double db; const char* label; };
        const Mark marks[] = { { 6.0, "+6" }, { 0.0, "0" }, { -6.0, "-6" }, { -12.0, "-12" }, { -24.0, "-24" } };
        const float tickL = (float) x + labelW + m.fpx (4, 2.0f);
        const float tickR = (float) (x + width) - m.fpx (4, 2.0f);
        g.setFont (font (Face::mono, m.fpx (13, 8.5f)));
        for (const auto& mark : marks)
        {
            const double v = channelGain ? (double) juce::Decibels::decibelsToGain (mark.db) : mark.db;
            if (v > s.getMaximum() + 1.0e-6 || v < s.getMinimum())
                continue;
            const float py = posOf (mark.db);
            const bool unity = mark.db == 0.0;
            g.setColour (juce::Colours::white.withAlpha ((unity ? 0.30f : 0.11f) * alpha));
            g.fillRect (tickL, py - 0.5f, tickR - tickL, unity ? 1.5f : 1.0f);
            if (master && labelW > 0.0f)
            {
                g.setColour (textSec().withMultipliedAlpha (alpha));
                g.drawText (mark.label, juce::Rectangle<float> ((float) x, py - m.fpx (8, 5.0f),
                                                                 labelW - m.fpx (4, 2.0f), m.fpx (16, 10.0f))
                                            .toNearestInt(),
                            juce::Justification::centredRight, false);
            }
        }
        if (master && labelW > 0.0f)
        {
            g.setColour (textSec().withMultipliedAlpha (alpha));
            g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e"),
                        juce::Rectangle<float> ((float) x, bottom - m.fpx (8, 5.0f),
                                                labelW - m.fpx (4, 2.0f), m.fpx (16, 10.0f)).toNearestInt(),
                        juce::Justification::centredRight, false);
        }

        // calha + preenchimento do cap ate a base
        const float trackW = juce::jmax (4.0f, m.fpx (8, 4.0f));
        const auto trackRect = juce::Rectangle<float> (cx - trackW * 0.5f, top, trackW, bottom - top);
        g.setColour (trackBg().withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        const auto fill = juce::Rectangle<float> (trackRect.getX(), sliderPos, trackW,
                                                  juce::jmax (0.0f, bottom - sliderPos));
        g.setGradientFill (juce::ColourGradient (accent().withMultipliedAlpha (alpha), 0.0f, sliderPos,
                                                 juce::Colour (0xffa81b2e).withMultipliedAlpha (alpha), 0.0f, bottom, false));
        g.fillRoundedRectangle (fill, trackW * 0.5f);

        // cap cinza com linha central
        auto cap = juce::Rectangle<float> (handle.getWidth(), handle.getHeight())
                       .withCentre ({ cx, sliderPos });
        const float radius = m.fpx (10, 4.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff54545f).withMultipliedAlpha (alpha),
                                                 0.0f, cap.getY(),
                                                 juce::Colour (0xff2a2a33).withMultipliedAlpha (alpha),
                                                 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, radius);
        g.setColour (juce::Colour (0xff666673).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (cap.reduced (0.5f), radius, 1.0f);
        const float markW = cap.getWidth() * (44.0f / 92.0f);
        const float markH = juce::jmax (2.0f, m.fpx (4, 2.0f));
        g.setColour (juce::Colours::white.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (cap.getCentreX() - markW * 0.5f, cap.getCentreY() - markH * 0.5f,
                                markW, markH, markH * 0.5f);
    }
};

inline LookAndFeel& lookAndFeel()
{
    static LookAndFeel instance;
    return instance;
}

// Marca "CHURCH LIVE" da barra superior. Fica acima do viewport do editor (que
// ocupa a barra no palco), por isso e um componente e nao pintura do pai.
class Wordmark : public juce::Component
{
public:
    // A marca e o atalho para "Sobre" (antes era o icone da igreja no trilho).
    Wordmark()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTitle ("Sobre o ChurchDAW");
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override { drawWordmark (g, getLocalBounds(), metricsOf (*this)); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick != nullptr && contains (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown())
            onClick();
    }
};

} // namespace ChurchLiveSkin
