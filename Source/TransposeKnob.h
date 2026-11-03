#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include "NordLookAndFeel.h"

//==============================================================================
// Knob dedicado do TRANSPOSE GLOBAL.
//
// Por que nao usa o drawRotarySlider comum: o transpose e o unico parametro
// BIPOLAR do painel (-12..+12, centro em 0). Com o arco comum, que sempre cresce
// da esquerda, "0" aparece como um arco pela metade — indistinguivel de "-6"
// num relance. Aqui o arco nasce no centro e cresce para o lado do sinal, entao
// da para ler "esta acima ou abaixo do tom" sem processar o numero.
//
// O valor vive num visor central em vez da caixa de texto embaixo: ao vivo, no
// escuro, o musico le o numero e nao a posicao do ponteiro. O visor usa o mesmo
// ambar sobre vidro (oledAmber/oledBack) do resto do app.
//
// Escala com o tamanho: em painel compacto some a serigrafia e o visor encolhe,
// mas o numero continua sendo o elemento mais legivel.
//==============================================================================
class TransposeKnobLookAndFeel : public NordLookAndFeel
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (1.5f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;

        if (NordLookAndFeel::usesDesktopShell (slider))
        {
            if (bounds.isEmpty()) return;
            const auto centre = bounds.getCentre();
            const float alpha = slider.isEnabled() ? 1.0f : 0.38f;
            const int semis = (int) std::lround (slider.getValue());
            const auto ink = semis == 0 ? juce::Colour (0xfff4f1ed)
                                        : NordLookAndFeel::nordRedBright();
            const float zeroPosition = (float) slider.valueToProportionOfLength (0.0);
            const float zeroAngle = rotaryStartAngle + zeroPosition * (rotaryEndAngle - rotaryStartAngle);
            const float valueAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
            const float arcRadius = radius * 0.92f;
            const auto stroke = juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded);
            if (radius >= 6.0f)
            {
                juce::Path track;
                track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                     rotaryStartAngle, rotaryEndAngle, true);
                g.setColour (juce::Colour (0xff4b515c).withAlpha (alpha));
                g.strokePath (track, stroke);
                if (semis != 0)
                {
                    juce::Path value;
                    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                         juce::jmin (zeroAngle, valueAngle),
                                         juce::jmax (zeroAngle, valueAngle), true);
                    g.setColour (ink.withAlpha (alpha));
                    g.strokePath (value, stroke);
                }
                const auto zeroMark = centre + juce::Point<float> (
                    std::sin (zeroAngle) * arcRadius, -std::cos (zeroAngle) * arcRadius);
                g.setColour (juce::Colour (0xffd5d9df).withAlpha (alpha));
                g.fillEllipse (zeroMark.x - 1.0f, zeroMark.y - 1.0f, 2.0f, 2.0f);
            }
            const auto body = bounds.withSizeKeepingCentre (radius * 1.54f, radius * 1.54f);
            g.setColour (slider.isMouseOverOrDragging() ? juce::Colour (0xff3c444f)
                                                       : juce::Colour (0xff303741));
            g.fillEllipse (body);
            if (slider.hasKeyboardFocus (false))
            {
                g.setColour (NordLookAndFeel::nordRedBright().withAlpha (alpha));
                g.drawEllipse (body.reduced (0.5f), 1.0f);
            }
            const juce::String text = (semis > 0 ? "+" : "") + juce::String (semis);
            g.setColour (ink.withAlpha (alpha));
            g.setFont (juce::Font (juce::FontOptions (
                juce::jlimit (12.0f, 26.0f, radius * 0.64f), juce::Font::bold)));
            g.drawFittedText (text, body.reduced (2.0f).toNearestInt(),
                              juce::Justification::centred, 1, 0.85f);
            return;
        }

        if (radius < 6.0f)   // espaco degenerado: nao tenta desenhar, so o visor
        {
            drawDisplay (g, bounds, slider, radius);
            return;
        }

        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float midAngle = (rotaryStartAngle + rotaryEndAngle) * 0.5f;

        auto onCircle = [cx, cy] (float a, float r)
        {
            return juce::Point<float> (cx + std::sin (a) * r, cy - std::cos (a) * r);
        };

        const bool roomy = radius >= 22.0f;

        //----------------------------------------------------------------------
        // 1. Anel de LEDs de semitom (-12 a +12): Estilo console de estúdio Pro.
        //    Os LEDs acendem proporcionalmente à transposição, com LED central 0.
        //----------------------------------------------------------------------
        const int currentSemi = juce::jlimit (-12, 12, (int) std::lround (slider.getValue()));
        const float ledRingR = radius * (roomy ? 0.88f : 0.84f);

        // Calha rebaixada onde os LEDs do anel ficam instalados
        {
            juce::Path collar;
            collar.addCentredArc (cx, cy, ledRingR, ledRingR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colour (0xff0e1014));
            g.strokePath (collar, juce::PathStrokeType (roomy ? 5.5f : 4.2f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.strokePath (collar, juce::PathStrokeType (roomy ? 6.5f : 5.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
        }

        for (int semi = -12; semi <= 12; ++semi)
        {
            const float t = (float) (semi + 12) / 24.0f;
            const float a = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
            const auto pt = onCircle (a, ledRingR);
            const bool isZero = (semi == 0);
            const bool isMajor = (semi % 6 == 0);

            bool isLit = false;
            if (currentSemi == 0)
                isLit = isZero;
            else if (currentSemi > 0)
                isLit = (semi >= 0 && semi <= currentSemi);
            else
                isLit = (semi <= 0 && semi >= currentSemi);

            const float dotSize = isZero ? (roomy ? 4.8f : 3.8f)
                                         : (isMajor ? (roomy ? 4.0f : 3.2f)
                                                    : (roomy ? 3.2f : 2.5f));

            if (isLit)
            {
                const auto ledCol = isZero ? NordLookAndFeel::oledAmber()
                                  : (semi > 0 ? juce::Colour (0xffffcf40)
                                              : juce::Colour (0xffff8c00));

                // Halo de difusão de luz
                g.setColour (ledCol.withAlpha (0.36f));
                g.fillEllipse (pt.x - dotSize * 0.9f, pt.y - dotSize * 0.9f, dotSize * 1.8f, dotSize * 1.8f);

                // Corpo do LED aceso
                g.setColour (ledCol);
                g.fillEllipse (pt.x - dotSize * 0.5f, pt.y - dotSize * 0.5f, dotSize, dotSize);

                // Ponto central de alta emissão (hotspot)
                g.setColour (juce::Colours::white.withAlpha (0.80f));
                g.fillEllipse (pt.x - dotSize * 0.22f, pt.y - dotSize * 0.22f, dotSize * 0.44f, dotSize * 0.44f);
            }
            else
            {
                // LED apagado (célula escura com aro metálico sutil)
                g.setColour (juce::Colour (0xff14161c));
                g.fillEllipse (pt.x - dotSize * 0.5f, pt.y - dotSize * 0.5f, dotSize, dotSize);
                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.drawEllipse (pt.x - dotSize * 0.5f, pt.y - dotSize * 0.5f, dotSize, dotSize, 0.5f);
            }
        }

        //----------------------------------------------------------------------
        // 2. Corpo do knob: sombra de apoio, gradiente diagonal e luz na aresta
        //    superior. Mesma linguagem do knob comum, so que maior.
        //----------------------------------------------------------------------
        const float bodyR = radius * (roomy ? 0.68f : 0.74f);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillEllipse (cx - bodyR * 1.04f, cy - bodyR * 1.04f + 2.0f,
                       bodyR * 2.08f, bodyR * 2.08f);

        juce::ColourGradient body (juce::Colour (0xff474c57), cx - bodyR * 0.45f, cy - bodyR * 0.75f,
                                   juce::Colour (0xff13161b), cx + bodyR * 0.42f, cy + bodyR * 0.85f,
                                   true);
        g.setGradientFill (body);
        g.fillEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f);

        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.4f);

        {
            juce::Path hi;
            hi.addCentredArc (cx, cy, bodyR - 1.2f, bodyR - 1.2f, 0.0f, -2.3f, -0.6f, true);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.strokePath (hi, juce::PathStrokeType (1.7f));
        }

        //----------------------------------------------------------------------
        // 4. Ponteiro. Sombra grossa embaixo, traco claro em cima: sobrevive
        //    tanto ao corpo escuro quanto ao arco aceso.
        //----------------------------------------------------------------------
        const float tipR = bodyR * 0.94f;
        const float baseR = bodyR * (roomy ? 0.62f : 0.30f);

        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawLine ({ onCircle (angle, baseR), onCircle (angle, tipR) }, roomy ? 4.6f : 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.95f));
        g.drawLine ({ onCircle (angle, baseR), onCircle (angle, tipR) }, roomy ? 2.6f : 2.4f);

        drawDisplay (g, bounds, slider, radius);
    }

private:
    //==========================================================================
    // Visor central: vidro escuro recuado, numero ambar com halo. O sinal fica
    // sempre visivel ("+2" e "-2", nunca "2"), porque no palco a diferenca
    // entre subir e descer um tom nao pode depender de memoria.
    //==========================================================================
    static void drawDisplay (juce::Graphics& g, juce::Rectangle<float> bounds,
                             juce::Slider& slider, float radius)
    {
        const int semis = juce::jlimit (-12, 12, (int) std::lround (slider.getValue()));
        const bool roomy = radius >= 22.0f;

        const float glassW = juce::jmin (bounds.getWidth() * 0.62f, radius * 1.30f);
        const float glassH = juce::jmin (bounds.getHeight() * 0.42f, radius * 0.72f);

        if (glassW < 12.0f || glassH < 9.0f)
            return;   // sem espaco honesto para o visor: melhor nada que ilegivel

        const auto glass = juce::Rectangle<float> (glassW, glassH)
                               .withCentre (bounds.getCentre());
        const float corner = juce::jmin (4.0f, glassH * 0.28f);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (glass.expanded (1.4f), corner + 1.0f);

        g.setColour (NordLookAndFeel::oledBack());
        g.fillRoundedRectangle (glass, corner);

        g.setColour (juce::Colours::black.withAlpha (0.9f));
        g.drawRoundedRectangle (glass, corner, 1.0f);

        // brilho de vidro na metade de cima
        {
            juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.10f),
                                        glass.getCentreX(), glass.getY(),
                                        juce::Colours::transparentWhite,
                                        glass.getCentreX(), glass.getCentreY(), false);
            g.setGradientFill (sheen);
            g.fillRoundedRectangle (glass.withHeight (glass.getHeight() * 0.55f), corner);
        }

        const juce::String text = (semis > 0 ? "+" : "") + juce::String (semis);
        const float digitH = juce::jlimit (9.0f, 26.0f, glassH * 0.74f);

        auto textArea = glass;

        // "st" so quando ha folga: sem isso ele rouba largura do numero, que e
        // o que precisa ser lido de longe.
        if (roomy && glassW > 40.0f)
        {
            auto suffix = textArea.removeFromRight (glassW * 0.30f);
            g.setColour (NordLookAndFeel::oledAmber().withAlpha (0.55f));
            g.setFont (NordLookAndFeel::stageLabel (juce::jmax (7.0f, digitH * 0.42f)));
            g.drawText ("st", suffix.translated (-2.0f, 0.5f),
                        juce::Justification::centredLeft, false);
        }

        // Halo e texto numérico de alto contraste no centro do dial
        const auto textCol = semis == 0 ? juce::Colour (0xffffe066)
                           : (semis > 0 ? juce::Colour (0xffffcf40) : juce::Colour (0xffff9e40));

        g.setColour (textCol.withAlpha (0.32f));
        g.setFont (juce::Font (juce::FontOptions (digitH, juce::Font::bold)));
        g.drawText (text, textArea.translated (0.0f, 0.6f),
                    juce::Justification::centred, false);

        g.setColour (textCol);
        g.drawText (text, textArea, juce::Justification::centred, false);
    }
};
