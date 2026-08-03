#pragma once

// EXPERIMENTO TEMPORARIO: 1 congela o desenho para medir quanto ele custa.
#ifndef CHURCHDAW_UI_BISECT
 #define CHURCHDAW_UI_BISECT 0
#endif

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
        // No celular o JUCE desenha por SOFTWARE, e a tela e grande (2460x1080).
        // Medido com o app tocando: a thread da interface saturava um nucleo
        // inteiro (100%) e o audio, que precisa de prazo, ficava sem CPU -- era
        // essa a origem do pipoco, nao falta de memoria nem o streaming.
        // 15 Hz ainda le como movimento continuo num medidor de nivel; 30 Hz e
        // luxo de PC.
       #if CHURCHDAW_UI_BISECT
        // experimento: medidores congelados, para medir quanto eles custam
       #elif JUCE_ANDROID || JUCE_IOS
        // 10 Hz: ainda le fluido no palco e devolve CPU ao audio (era 15/30).
        // Sem voice limit — so menos pintura na message thread.
        startTimerHz (10);
       #else
        startTimerHz (30);
       #endif
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

    // No master, alem do peak-hold grafico, mostra o pico numerico de cada lado.
    // Nos meters de canal isto permanece desligado para conservar a grade enxuta.
    void setPeakReadout (bool shouldShow) { peakReadout = shouldShow; repaint(); }

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

        juce::Rectangle<float> peakRow;
        if (peakReadout && channels == 2)
            peakRow = bounds.removeFromBottom (17.0f);

        // Fundo um pouco mais escuro + moldura sutil: o meter se destaca do card.
        g.setColour (juce::Colour (0xff0e1014));
        g.fillRoundedRectangle (bounds, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, 1.0f);

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

            g.setColour (clipSticky ? juce::Colours::white
                                    : juce::Colours::white.withAlpha (0.34f));
            g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold))
                           .withExtraKerningFactor (0.08f));
            g.drawText ("CLIP", clipArea.toNearestInt(),
                        juce::Justification::centred, false);

            bounds.removeFromTop (1.0f);
        }

        auto meterArea = bounds.reduced (channels == 1 ? 3.0f : 5.0f);
        const float gap = channels == 1 ? 0.0f : 4.0f;
        const float barWidth = (meterArea.getWidth() - gap * (float) (channels - 1)) / (float) channels;

        if (! peakRow.isEmpty())
        {
            const auto mono = juce::Font (juce::FontOptions (
                juce::Font::getDefaultMonospacedFontName(), 9.0f,
                juce::Font::bold));
            g.setFont (mono);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto cell = juce::Rectangle<float> (
                    meterArea.getX() + (float) ch * (barWidth + gap),
                    peakRow.getY(), barWidth, peakRow.getHeight());
                const float db = peakDb[ch];
                const auto value = db <= minDb + 0.5f
                    ? juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e")
                    : (db >= 0.0f ? "+" : juce::String())
                        + juce::String (db, 1);

                g.setColour (db >= -0.05f ? NordLookAndFeel::redClip()
                                          : juce::Colours::white.withAlpha (0.72f));
                g.drawFittedText (value, cell.toNearestInt(),
                                  juce::Justification::centred, 1, 0.72f);
            }
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            auto barArea = juce::Rectangle<float> (meterArea.getX() + (float) ch * (barWidth + gap),
                                                   meterArea.getY(), barWidth, meterArea.getHeight());

            // ESCADA SEGMENTADA em dBFS. A cor de cada celula vem da FAIXA DE NIVEL
            // que ela representa -- nao de um degrade continuo. Assim a fronteira
            // dos -6 e dos -3 dBFS fica no mesmo lugar em todos os canais, e o
            // musico aprende a posicao em vez de julgar o tom da cor.
            const float lvl = displayDb[ch];
            // Segmentos um pouco mais altos: meter le de longe no palco.
            const float segH = barArea.getWidth() >= 10.0f ? 3.6f : 3.0f;
            const float segGap = 1.0f, step = segH + segGap;
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

        // Escala: as marcas estavam a 14% de alpha â€” invisiveis, o medidor lia como
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

            // Nao acorda ActivityBar a cada tick se o nivel mal mudou.
            if (std::abs (top - lastOnLevel) > 0.04f || (top < 0.02f && lastOnLevel > 0.02f))
            {
                lastOnLevel = top;
                onLevel (top);
            }
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
    bool peakReadout = false;
    // Nasce true: medidor recem-construido (re-dock do editor) tambem encontra
    // acumulo de quando nao existia consumidor -- o primeiro tick sempre drena
    bool wasHidden = true;
    float lastOnLevel = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
