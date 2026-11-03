#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include <functional>
#include <algorithm>
#include <cmath>

//==============================================================================
// Medidor de Reducao de Ganho do Master Limiter (True-Peak).
// Formato de console de estudio profissional (SSL/Neve): calha com vidro fume,
// chip informativo "GR", escada de LEDs segmentados em dB com codigo de cores
// (ambar -> laranja -> vermelho de sobrecarga), linha de peak-hold e readout
// numerico com alto contraste na base.
//==============================================================================
class LimiterReductionMeter : public juce::Component,
                              public juce::SettableTooltipClient,
                              private juce::Timer
{
public:
    using ReductionFn = std::function<float()>;

    explicit LimiterReductionMeter (ReductionFn reductionSource)
        : reductionFn (std::move (reductionSource))
    {
       #if CHURCHDAW_UI_BISECT
       #elif JUCE_ANDROID
        timerHz = 6;
       #elif JUCE_IOS
        timerHz = 10;
       #else
        timerHz = 45;
       #endif

        if (timerHz > 0)
            startTimerHz (timerHz);

        decayDbPerTick = 28.0f / (float) juce::jmax (1, timerHz);
        peakDecayDbPerTick = 16.0f / (float) juce::jmax (1, timerHz);
        peakHoldTicks = juce::roundToInt (1.2f * (float) timerHz);

        setTooltip (juce::String::fromUTF8 (
            u8"REDUÇÃO DO LIMITER (GR) — medidor de compressão do Master True-Peak contra distorção"));
    }

    ~LimiterReductionMeter() override
    {
        stopTimer();
    }

    void timerCallback() override
    {
        if (! isShowing() || getWidth() <= 0 || getHeight() <= 0)
        {
            displayDb = 0.0f;
            peakDb = 0.0f;
            lastRepaintedDb = 0.0f;
            lastRepaintedPeakDb = 0.0f;
            holdTicksLeft = 0;
            return;
        }

        float rawReduction = 0.0f;
        if (reductionFn != nullptr)
        {
            const float r = reductionFn();
            if (std::isfinite (r))
                rawReduction = r;
        }

        // Gain reduction in dB is <= 0.0f
        const float instantGr = juce::jmin (0.0f, rawReduction);

        // Balistica: ataque instantaneo (mais negativo), queda suave (para 0)
        if (instantGr < displayDb)
        {
            displayDb = instantGr;
        }
        else
        {
            displayDb = juce::jmin (0.0f, displayDb + decayDbPerTick);
        }

        // Peak-hold para visualizacao de transientes rapidos do culto
        if (instantGr < peakDb)
        {
            peakDb = instantGr;
            holdTicksLeft = peakHoldTicks;
        }
        else if (holdTicksLeft > 0)
        {
            --holdTicksLeft;
        }
        else
        {
            peakDb = juce::jmin (0.0f, peakDb + peakDecayDbPerTick);
        }

        const bool needsRepaint = (std::abs (displayDb - lastRepaintedDb) > 0.08f)
                               || (std::abs (peakDb - lastRepaintedPeakDb) > 0.08f)
                               || (displayDb == 0.0f && lastRepaintedDb != 0.0f)
                               || (peakDb == 0.0f && lastRepaintedPeakDb != 0.0f);

        if (needsRepaint)
        {
            lastRepaintedDb = displayDb;
            lastRepaintedPeakDb = peakDb;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        if (bounds.getWidth() < 8.0f || bounds.getHeight() < 24.0f)
            return;

        // Moldura / Rebaixo identico ao LevelMeter (vidro fume de console)
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff151014),
                                                 bounds.getX(), bounds.getY(),
                                                 juce::Colour (0xff050607),
                                                 bounds.getRight(), bounds.getBottom(), true));
        g.fillRoundedRectangle (bounds, 4.0f);

        const bool activeLimiting = displayDb < -0.4f;
        g.setColour (activeLimiting ? juce::Colour (0xffffb020).withAlpha (0.45f)
                                   : juce::Colours::white.withAlpha (0.028f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, activeLimiting ? 1.2f : 0.8f);

        // Chip do topo: "GR" (Gain Reduction) alinhado ao "LIM" do LevelMeter
        auto chipArea = bounds.removeFromTop (12.0f).reduced (3.0f, 2.0f);
        const auto chipOnColour  = juce::Colour (0xffffb020);
        const auto chipOffColour = juce::Colour (0xff28241b);
        g.setColour (activeLimiting ? chipOnColour : chipOffColour);
        g.fillRoundedRectangle (chipArea, 2.5f);

        if (activeLimiting)
        {
            g.setColour (chipOnColour.withAlpha (0.35f));
            g.drawRoundedRectangle (chipArea.expanded (1.0f), 3.0f, 1.0f);
        }

        g.setColour (activeLimiting ? juce::Colour (0xff1a1207)
                                   : juce::Colours::white.withAlpha (0.40f));
        g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold))
                       .withExtraKerningFactor (0.08f));
        g.drawText ("GR", chipArea.toNearestInt(), juce::Justification::centred, false);

        bounds.removeFromTop (1.0f);

        // Leitura numerica de dB na base (perfeitamente alinhada com peakReadout do LevelMeter)
        const float peakH = 17.0f;
        auto peakRow = bounds.removeFromBottom (peakH);

        const auto mono = juce::Font (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(), 8.5f,
            juce::Font::bold));
        g.setFont (mono);

        juce::String grText;
        if (displayDb <= -0.1f)
            grText = juce::String (displayDb, 1);
        else
            grText = "0.0";

        g.setColour (activeLimiting ? (displayDb <= -6.0f ? juce::Colour (0xffff1744)
                                                          : juce::Colour (0xffffb020))
                                    : juce::Colours::white.withAlpha (0.45f));
        g.drawFittedText (grText, peakRow.toNearestInt(), juce::Justification::centred, 1, 0.75f);

        // Area da barra / escada LED
        auto barArea = bounds.reduced (2.5f, 2.0f);
        if (barArea.getHeight() < 10.0f || barArea.getWidth() < 4.0f)
            return;

        // Fundo da calha de reducao
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1014),
                                                 barArea.getX(), barArea.getY(),
                                                 juce::Colour (0xff08090c),
                                                 barArea.getRight(), barArea.getY(), false));
        g.fillRoundedRectangle (barArea, 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.030f));
        g.drawRoundedRectangle (barArea.reduced (0.35f), 2.0f, 0.7f);

        // Escada invertida: 0 dB no topo, maxReductionDb (-16 dB) no fundo
        static constexpr float maxReductionDb = -16.0f;

        const float segH = 1.30f;
        const float segGap = 0.55f;
        const float step = segH + segGap;
        const int numSeg = juce::jmax (1, (int) ((barArea.getHeight() + segGap) / step));

        for (int i = 0; i < numSeg; ++i)
        {
            const float segTop = barArea.getY() + (float) i * step;
            auto seg = juce::Rectangle<float> (barArea.getX(), segTop,
                                               barArea.getWidth(), segH)
                           .getIntersection (barArea);

            if (seg.isEmpty())
                continue;

            // Fracao de reducao no centro do segmento (0 no topo ate 1.0 no fundo)
            const float frac = (seg.getCentreY() - barArea.getY()) / barArea.getHeight();
            const float segDb = frac * maxReductionDb;

            // Ativo quando o ganho foi atenuado ate esta profundidade
            const bool segActive = (displayDb <= segDb + 0.05f) && (displayDb < -0.1f);

            juce::Colour ledCol;
            if (segDb > -3.0f)
                ledCol = juce::Colour (0xffffb020); // Ambar sutil: controle de pico transparente
            else if (segDb > -6.0f)
                ledCol = juce::Colour (0xffff8810); // Laranja: contencao moderada
            else
                ledCol = juce::Colour (0xffff1744); // Vermelho: alerta contra esmagamento / sobrecarga

            if (segActive)
            {
                g.setColour (ledCol.withAlpha (0.15f));
                g.fillRect (seg.expanded (0.5f, 0.2f));

                g.setColour (ledCol);
                g.fillRect (seg);

                g.setColour (juce::Colours::white.withAlpha (0.16f));
                g.fillRect (seg.withHeight (0.5f));
            }
            else
            {
                g.setColour (juce::Colour (0xff1d191d)); // Segmento LED apagado
                g.fillRect (seg);
            }
        }

        // Marcador de pico de reducao (peak-hold line)
        if (peakDb < -0.1f)
        {
            const float peakFrac = juce::jlimit (0.0f, 1.0f, peakDb / maxReductionDb);
            const float peakY = barArea.getY() + peakFrac * barArea.getHeight();
            g.setColour (juce::Colours::white);
            g.fillRect (juce::Rectangle<float> (barArea.getX(), peakY - 0.9f,
                                                barArea.getWidth(), 1.8f)
                            .getIntersection (barArea));
        }

        // Marcas de escala sutis: 0, -3, -6, -12 dB
        for (float mark : { 0.0f, -3.0f, -6.0f, -12.0f })
        {
            const float frac = mark / maxReductionDb;
            const float my = barArea.getY() + frac * barArea.getHeight();
            g.setColour (juce::Colours::white.withAlpha (0.22f));
            g.drawHorizontalLine ((int) my, barArea.getX(), barArea.getX() + 3.0f);
            g.drawHorizontalLine ((int) my, barArea.getRight() - 3.0f, barArea.getRight());
        }
    }

private:
    ReductionFn reductionFn;
    int timerHz = 45;
    float decayDbPerTick = 0.6f;
    float peakDecayDbPerTick = 0.35f;
    int peakHoldTicks = 54;

    float displayDb = 0.0f;
    float peakDb = 0.0f;
    int holdTicksLeft = 0;

    float lastRepaintedDb = 999.0f;
    float lastRepaintedPeakDb = 999.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterReductionMeter)
};
