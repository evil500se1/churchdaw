#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "NordLookAndFeel.h"

#include <functional>

//==============================================================================
// Medidor de nivel generico (1 ou 2 barras): gradiente verde -> amarelo ->
// vermelho, linha de pico com queda suave e luz de clip opcional (clique
// para apagar). As fontes de nivel/clip sao funcoes "le e zera".
//==============================================================================
class LevelMeter : public juce::Component,
                   private juce::Timer
{
public:
    using LevelFn = std::function<float (int channel)>;
    using ClipFn  = std::function<bool()>;

    LevelMeter (int numChannels, LevelFn levelSource, ClipFn clipSource = {})
        : channels (juce::jlimit (1, 2, numChannels)),
          levelFn (std::move (levelSource)),
          clipFn (std::move (clipSource))
    {
        startTimerHz (30);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (! clipSticky)
            return;

        clipSticky = false;
        repaint();   // sem isto a luz seguia acesa ate algum outro quadro mudar
    }

    // mostra "L" / "R" embaixo de cada barra (so faz sentido em medidor estereo)
    void setChannelLabels (bool shouldShow) { channelLabels = shouldShow; repaint(); }

    // avisa o nivel exibido (0..1) a cada quadro. Serve pra acender o card do
    // canal sem criar um segundo timer nem um segundo leitor do nivel (o
    // readAndResetLayerLevel do processador ZERA a cada leitura: so pode ter um).
    std::function<void (float)> onLevel;

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        juce::Rectangle<float> labelRow;
        if (channelLabels && channels == 2)
            labelRow = bounds.removeFromBottom (13.0f);

        g.setColour (juce::Colour (0xff15171b));
        g.fillRoundedRectangle (bounds, 5.0f);

        // luz de clip DENTRO da moldura, como tampa do medidor: flutuando acima
        // com um vao ela lia como um elemento vermelho perdido/cortado
        if (clipFn)
        {
            auto clipArea = bounds.removeFromTop (12.0f).reduced (4.0f, 2.5f);

            g.setColour (clipSticky ? juce::Colour (0xffff1744) : juce::Colour (0xff3a2226));
            g.fillRoundedRectangle (clipArea, 3.0f);

            if (clipSticky)
            {
                g.setColour (juce::Colour (0x60ff1744));
                g.drawRoundedRectangle (clipArea.expanded (1.5f), 4.0f, 1.5f);
            }

            bounds.removeFromTop (1.0f);
        }

        auto meterArea = bounds.reduced (channels == 1 ? 3.0f : 5.0f);
        const float gap = channels == 1 ? 0.0f : 4.0f;
        const float barWidth = (meterArea.getWidth() - gap * (float) (channels - 1)) / (float) channels;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto barArea = juce::Rectangle<float> (meterArea.getX() + (float) ch * (barWidth + gap),
                                                   meterArea.getY(), barWidth, meterArea.getHeight());

            // ESCADA SEGMENTADA em dBFS. A cor de cada celula vem da FAIXA DE NIVEL
            // que ela representa -- nao de um degrade continuo. Assim a fronteira
            // dos -6 e dos -3 dBFS fica no mesmo lugar em todos os canais, e o
            // musico aprende a posicao em vez de julgar o tom da cor.
            const float lvl = displayDb[ch];
            const float segH = 3.0f, segGap = 1.0f, step = segH + segGap;
            const int numSeg = juce::jmax (4, (int) ((barArea.getHeight() + segGap) / step));

            for (int i = 0; i < numSeg; ++i)
            {
                const float segTop = barArea.getY() + (float) i * step;
                auto seg = juce::Rectangle<float> (barArea.getX(), segTop, barArea.getWidth(), segH);

                // dB representado por esta celula (topo = 0 dBFS)
                const float prop = 1.0f - (segTop + segH * 0.5f - barArea.getY())
                                            / juce::jmax (1.0f, barArea.getHeight());
                const float segDb = proportionToDb (prop);

                if (segDb <= lvl + 0.01f)
                    g.setColour (colourForDb (segDb));
                else
                    g.setColour (NordLookAndFeel::meterOff());

                g.fillRect (seg);
            }

            // marcador de pico: linha fina alinhada aos segmentos
            if (peakDb[ch] > minDb + 0.5f)
            {
                const float y = barArea.getBottom() - barArea.getHeight() * dbToProportion (peakDb[ch]);
                g.setColour (peakDb[ch] >= -0.05f ? NordLookAndFeel::redClip() : juce::Colours::white);
                g.fillRect (juce::Rectangle<float> (barArea.getX(), y - 0.9f, barArea.getWidth(), 1.8f));
            }
        }

        // Escala: as marcas estavam a 14% de alpha — invisiveis, o medidor lia como
        // um tubo cinza sem referencia. Agora dao pra ver, e o 0 dB (a linha que
        // realmente importa) tem peso e cor proprios.
        for (float mark : { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
        {
            const float y = meterArea.getBottom() - meterArea.getHeight() * dbToProportion (mark);

            if (mark == 0.0f)
            {
                g.setColour (juce::Colour (0xffffd740).withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> (meterArea.getX(), y - 0.75f,
                                                    meterArea.getWidth(), 1.5f));
            }
            else
            {
                g.setColour (juce::Colours::white.withAlpha (0.22f));
                g.drawHorizontalLine ((int) y, meterArea.getX(), meterArea.getRight());
            }
        }

        if (! labelRow.isEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            const char* names[] = { "L", "R" };
            for (int ch = 0; ch < channels; ++ch)
            {
                auto cell = juce::Rectangle<float> (meterArea.getX() + (float) ch * (barWidth + gap),
                                                    labelRow.getY(), barWidth, labelRow.getHeight());
                g.drawText (names[ch], cell, juce::Justification::centred);
            }
        }
    }

private:
    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 6.0f;

    static float dbToProportion (float db) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }

    static float proportionToDb (float p) noexcept
    {
        return minDb + juce::jlimit (0.0f, 1.0f, p) * (maxDb - minDb);
    }

    // Faixas de nivel do spec. Cada celula pinta pela faixa a que PERTENCE, entao
    // as fronteiras de -12, -6 e -3 dBFS caem sempre na mesma altura.
    static juce::Colour colourForDb (float db) noexcept
    {
        if (db >= 0.0f)    return NordLookAndFeel::redClip();
        if (db >= -3.0f)   return NordLookAndFeel::meterHot();
        if (db >= -6.0f)   return NordLookAndFeel::meterWarn();
        if (db >= -12.0f)  return NordLookAndFeel::meterMid();
        if (db >= -30.0f)  return NordLookAndFeel::meterLow();

        return NordLookAndFeel::meterVeryLow();
    }

    void timerCallback() override
    {
        // escondido (ex.: tela da cadeia sob o Church Live fullscreen): nao
        // consome a fonte le-e-zera -- roubava quadros e clips do medidor
        // visivel. Zera o estado de exibicao pra nao mostrar nivel velho ao
        // reaparecer.
        if (! isShowing())
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                displayDb[ch] = minDb;
                peakDb[ch] = minDb;
                lastShownDb[ch] = minDb;
                lastShownPeak[ch] = minDb;
                peakHoldFrames[ch] = 0;
            }

            wasHidden = true;   // marca pra drenar o acumulado ao reaparecer

            // o card de atividade (onLevel) tambem apaga: senao ele reaparece
            // aceso com o brilho de antes enquanto a barra ja pinta zerada
            if (onLevel != nullptr)
                onLevel (0.0f);

            return;
        }

        // primeiro tick de volta apos periodo escondido (isShowing() tambem e
        // false com a janela MINIMIZADA, quando NENHUM medidor consome as
        // fontes): o le-e-zera acumulou o pico maximo e a trava de clip do
        // periodo inteiro. Le UMA vez e DESCARTA, senao o acumulado viraria
        // spike falso, peak-hold de evento antigo e clipSticky aceso por um
        // clip de minutos atras. Display segue zerado neste tick; do proximo
        // em diante, comportamento normal.
        if (wasHidden)
        {
            wasHidden = false;

            if (clipFn)
                clipFn();           // drena sem acender clipSticky

            if (levelFn)
                for (int ch = 0; ch < channels; ++ch)
                    levelFn (ch);   // drena o pico acumulado sem exibir

            return;
        }

        bool changed = false;

        if (clipFn && clipFn() && ! clipSticky)
        {
            clipSticky = true;
            changed = true;
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            const float newDb = juce::Decibels::gainToDecibels (levelFn ? levelFn (ch) : 0.0f, minDb);

            displayDb[ch] = newDb > displayDb[ch] ? newDb
                                                  : juce::jmax (displayDb[ch] - 1.2f, minDb);

            if (newDb > peakDb[ch])
            {
                peakDb[ch] = newDb;
                peakHoldFrames[ch] = 45;
            }
            else if (peakHoldFrames[ch] > 0)
            {
                --peakHoldFrames[ch];
            }
            else
            {
                peakDb[ch] = juce::jmax (peakDb[ch] - 0.8f, minDb);
            }

            if (std::abs (displayDb[ch] - lastShownDb[ch]) > 0.3f
                 || std::abs (peakDb[ch] - lastShownPeak[ch]) > 0.5f)
            {
                lastShownDb[ch] = displayDb[ch];
                lastShownPeak[ch] = peakDb[ch];
                changed = true;
            }
        }

        // so repinta quando algo mudou de verdade (economiza CPU de interface)
        if (changed)
            repaint();

        if (onLevel != nullptr)
        {
            float top = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                top = juce::jmax (top, dbToProportion (displayDb[ch]));

            onLevel (top);
        }
    }

    int channels = 2;
    LevelFn levelFn;
    ClipFn clipFn;
    float displayDb[2] { minDb, minDb };
    float peakDb[2] { minDb, minDb };
    float lastShownDb[2] { minDb, minDb };
    float lastShownPeak[2] { minDb, minDb };
    int peakHoldFrames[2] { 0, 0 };
    bool clipSticky = false;
    bool channelLabels = false;
    // Nasce true: medidor recem-construido (re-dock do editor) tambem encontra
    // acumulo de quando nao existia consumidor -- o primeiro tick sempre drena
    bool wasHidden = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
