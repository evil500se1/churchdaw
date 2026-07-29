#pragma once

// EXPERIMENTO TEMPORARIO: 1 congela o desenho para medir quanto ele custa.
#ifndef CHURCHDAW_UI_BISECT
 #define CHURCHDAW_UI_BISECT 0
#endif

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"

#include <vector>

//==============================================================================
// Visor de programa copiado do display colorido do Nord Electro 7:
//
//   +--------------------------------+
//   | [ A:11 ]  <- chapa vermelha    |
//   |                                |
//   | Nord Electro 7  <- nome grande |
//   | ______________________________ |
//   | [i] B3                         |
//   | [i] EP9 Stockholm           XL |
//   | [i] SymphStr Legato Amb        |
//   +--------------------------------+
//
// A logica do layout deles: UMA coisa grande (o programa) e uma lista compacta
// do que esta empilhado embaixo dele. Serve exatamente para os nossos canais.
//==============================================================================
class OledDisplay : public juce::Component,
                    private juce::Timer
{
public:
    // Pictogramas do visor, como os iconezinhos de orgao/piano/sample da lista do
    // Electro 7. Enum PROPRIO de proposito: este componente nao conhece o
    // TimbresProcessor -- quem traduz a categoria e o MainComponent.
    enum class Icon { generic, piano, organ, strings, pad, bells, plugin };

    struct Row
    {
        int index = 0;             // numero do canal, mostrado junto ao pictograma
        Icon icon = Icon::generic;
        juce::String name;         // timbre carregado
        juce::String tag;          // etiqueta a direita (ex.: "-6.7 dB")
        float dbVal = -200.0f;     // valor numerico da tag: pinta ambar/vermelho quente
        bool empty = false;        // canal sem timbre: linha enxuta (sem icone/tag/medidor)
        bool active = true;        // canal desligado sai apagado
        bool focused = false;      // canal em foco: chapa de realce atras da linha
    };

    OledDisplay()
    {
        setInterceptsMouseClicks (false, false);
        // so o acorde precisa desta cadencia. No celular ela cai: este visor
        // ocupa a largura toda da tela e cada quadro dele custa caro no
        // desenho por software -- ver a nota em LevelMeter sobre a thread da
        // interface saturando um nucleo enquanto se toca.
       #if CHURCHDAW_UI_BISECT
        // experimento: visor congelado, para medir quanto ele custa
       #elif JUCE_ANDROID || JUCE_IOS
        startTimerHz (10);
       #else
        startTimerHz (20);
       #endif
    }

    // devolve o nome do acorde tocado agora ("" = nada). O visor consulta sozinho:
    // o timer do MainComponent e de 1 Hz, lento demais para acompanhar as maos.
    std::function<juce::String()> fetchChord;

    // nivel 0..1 do canal (0-based) para os mini-medidores das linhas -- mesmo
    // motivo do acorde: 1 Hz e lento demais para sinal de audio
    std::function<float (int)> fetchLevel;

    void setProgram (const juce::String& slot, const juce::String& title, std::vector<Row> newRows)
    {
        if (slot == slotText && title == titleText && sameRows (newRows))
            return;

        slotText = slot;
        titleText = title;
        rows = std::move (newRows);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float r = NordLookAndFeel::radiusCard;

        // vidro: preto profundo, nao cinza. No visor real o preto e mais escuro
        // que o chassi ao redor -- e isso que faz o branco "saltar"
        g.setColour (juce::Colour (0xff0d0d0f));
        g.fillRoundedRectangle (b, r);
        g.setColour (juce::Colours::black.withAlpha (0.9f));
        g.drawRoundedRectangle (b.reduced (0.5f), r, 1.4f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawRoundedRectangle (b.reduced (2.0f), r - 1.0f, 1.0f);

        // Faixa ultrabaixa de celular em paisagem: programa, nome e acorde ficam
        // numa unica linha. A coluna "CANAIS" repetia as abas logo abaixo e,
        // com menos de 40 dp, consumia altura sem conseguir mostrar uma linha.
        if (getHeight() < 40)
        {
            auto line = b.reduced (8.0f, 3.0f);

            if (slotText.isNotEmpty())
            {
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                const float chipW = juce::jmin (
                    line.getWidth() * 0.24f,
                    juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), slotText) + 15.0f);
                auto chip = line.removeFromLeft (chipW)
                                .withSizeKeepingCentre (chipW, juce::jmin (18.0f, line.getHeight()));

                g.setGradientFill (juce::ColourGradient (
                    NordLookAndFeel::nordRedBright(), chip.getX(), chip.getY(),
                    NordLookAndFeel::nordRed(), chip.getX(), chip.getBottom(), false));
                g.fillRoundedRectangle (chip, 3.0f);
                g.setColour (juce::Colours::white);
                g.drawText (slotText, chip.reduced (6.0f, 0.0f).toNearestInt(),
                            juce::Justification::centred, true);
                line.removeFromLeft (7.0f);
            }

            if (chordText.isNotEmpty() && line.getWidth() > 90.0f)
            {
                g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                const float chordW = juce::jmin (
                    76.0f,
                    juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), chordText) + 18.0f);
                auto chord = line.removeFromRight (chordW)
                                 .withSizeKeepingCentre (chordW, juce::jmin (18.0f, line.getHeight()));
                g.setColour (NordLookAndFeel::oledAmber().withAlpha (0.12f));
                g.fillRoundedRectangle (chord, 3.0f);
                g.setColour (NordLookAndFeel::oledAmber());
                g.drawText (chordText, chord.toNearestInt(),
                            juce::Justification::centred, true);
                line.removeFromRight (7.0f);
            }

            if (titleText.isNotEmpty())
            {
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
                g.drawFittedText (titleText, line.toNearestInt(),
                                  juce::Justification::centredLeft, 1, 0.78f);
            }

            return;
        }

        // CELULAR DEITADO: a faixa toda tem ~60 px de altura, e o empilhamento de
        // PC pede 22 (chapa) + 10 (respiro) + 40 (nome) = 72. O nome simplesmente
        // caia para fora e o visor mostrava a chapa vermelha sozinha num retangulo
        // preto -- justamente o "vazio sem intencao" que o comentario abaixo diz
        // querer evitar. Aqui chapa e nome dividem a MESMA linha.
        const bool tight = getHeight() < 90;

        auto inner = b.reduced (tight ? 12.0f : 18.0f, tight ? 6.0f : 12.0f);

        // DUAS COLUNAS. Empilhado, o painel precisava ser estreito e sobrava meia
        // tela de preto de cada lado -- vazio sem intencao, que e o que mais
        // denuncia software amador. Lado a lado ele ocupa a faixa inteira:
        // esquerda = o programa em foco, direita = o setup completo.
        //
        // Apertado a direita encolhe: a lista de canais e referencia, o NOME do
        // programa e o que se le de longe tocando.
        auto right = inner.removeFromRight (inner.getWidth() * (tight ? 0.34f : 0.52f));
        inner.removeFromRight (tight ? 12.0f : 22.0f);

        if (tight)
        {
            if (slotText.isNotEmpty())
            {
                g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                const float chipW = juce::jmin (inner.getWidth() * 0.5f,
                                                juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), slotText) + 20.0f);
                auto chip = inner.removeFromLeft (chipW).withSizeKeepingCentre (chipW, 20.0f);

                g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRedBright(), chip.getX(), chip.getY(),
                                                         NordLookAndFeel::nordRed(), chip.getX(), chip.getBottom(), false));
                g.fillRoundedRectangle (chip, 3.5f);
                g.setColour (juce::Colours::white);
                g.drawText (slotText, chip.reduced (9.0f, 0.0f).toNearestInt(),
                            juce::Justification::centred, true);

                inner.removeFromLeft (10.0f);
            }

            if (titleText.isNotEmpty())
            {
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (juce::FontOptions (25.0f, juce::Font::bold)));
                g.drawText (titleText, inner.toNearestInt(), juce::Justification::centredLeft, true);
            }

        }
        else
        {
        // ---- coluna esquerda: chapa vermelha + nome grande ----
        if (slotText.isNotEmpty())
        {
            auto chipRow = inner.removeFromTop (22.0f);

            // chapa do TAMANHO DO TEXTO. Esticada pela coluna inteira ela virava
            // uma barra vermelha enorme com tres palavras perdidas dentro.
            g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            const float chipW = juce::jmin (chipRow.getWidth(),
                                            juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), slotText) + 24.0f);
            auto chip = chipRow.withWidth (chipW);
            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRedBright(), chip.getX(), chip.getY(),
                                                     NordLookAndFeel::nordRed(), chip.getX(), chip.getBottom(),
                                                     false));
            g.fillRoundedRectangle (chip, 3.5f);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawLine (chip.getX() + 3.0f, chip.getY() + 1.0f, chip.getRight() - 3.0f, chip.getY() + 1.0f, 1.0f);

            g.setColour (juce::Colours::white);
            g.drawText (slotText, chip.reduced (11.0f, 0.0f).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }

        inner.removeFromTop (10.0f);

        // nome do programa: agora com a coluna inteira para ele, cabe MUITO maior
        if (titleText.isNotEmpty())
        {
            auto line = inner.removeFromTop (juce::jmin (40.0f, inner.getHeight()));

            g.setColour (juce::Colours::white.withAlpha (0.13f));
            g.setFont (juce::Font (juce::FontOptions (32.0f, juce::Font::bold)));

            for (int d = 1; d <= 2; ++d)
                g.drawText (titleText, line.toNearestInt().translated (0, d),
                            juce::Justification::centredLeft, true);

            g.setColour (juce::Colours::white);
            g.drawText (titleText, line.toNearestInt(), juce::Justification::centredLeft, true);
        }

        // ---- ACORDE TOCADO ----
        // O tecladista de igreja toca muito de ouvido, sem cifra na frente. Ver o
        // acorde confirma na hora, e e a informacao que muda mais rapido na tela --
        // por isso ela ganha luz propria em vez de virar mais uma linha cinza.
        if (chordText.isNotEmpty() && inner.getHeight() >= 26.0f)
        {
            inner.removeFromTop (6.0f);
            auto row = inner.removeFromTop (26.0f);

            g.setFont (juce::Font (juce::FontOptions (19.0f, juce::Font::bold)));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), chordText) + 26.0f;
            auto chip = row.withWidth (juce::jmin (w, row.getWidth()));

            g.setColour (NordLookAndFeel::oledAmber().withAlpha (0.13f));
            g.fillRoundedRectangle (chip, NordLookAndFeel::radiusControl);
            g.setColour (NordLookAndFeel::oledAmber().withAlpha (0.45f));
            g.drawRoundedRectangle (chip.reduced (0.5f), NordLookAndFeel::radiusControl, 1.0f);

            g.setColour (NordLookAndFeel::oledAmber());
            g.drawText (chordText, chip.toNearestInt(), juce::Justification::centred, true);
        }
        }   // fim do layout empilhado (PC / tablet)

        // ---- coluna direita: a lista dos canais, com titulo proprio ----
        {
            auto head = right.removeFromTop (13.0f);
            g.setFont (NordLookAndFeel::stageLabel (9.5f));
            g.setColour (juce::Colours::white.withAlpha (0.38f));
            g.drawText ("CANAIS", head.toNearestInt(), juce::Justification::centredLeft, false);

            right.removeFromTop (3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            g.fillRect (juce::Rectangle<float> (right.getX(), right.getY(), right.getWidth(), 1.0f));
            right.removeFromTop (6.0f);
        }

        const float rowH = juce::jlimit (13.0f, 18.0f, right.getHeight() / 5.0f);

        for (const auto& row : rows)
        {
            if (right.getHeight() < rowH - 1.0f)
                break;

            auto line = right.removeFromTop (rowH);
            const float alpha = row.active ? 1.0f : 0.38f;

            if (row.focused)
            {
                g.setColour (NordLookAndFeel::nordRed().withAlpha (0.28f));
                g.fillRoundedRectangle (line.expanded (3.0f, 0.5f), 2.5f);
            }

            // numero do canal (canal vazio nao ganha icone: um travessao ja diz
            // tudo -- dois simbolos apagados na mesma linha era ruido)
            {
                auto num = line.removeFromLeft (11.0f);
                g.setColour (juce::Colours::white.withAlpha (0.45f * alpha));
                g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
                g.drawText (juce::String (row.index), num.toNearestInt(),
                            juce::Justification::centredLeft, false);

                auto ic = line.removeFromLeft (14.0f).withSizeKeepingCentre (11.0f, 9.0f);

                if (! row.empty)
                    paintIcon (g, ic, row.icon, juce::Colours::white.withAlpha (0.85f * alpha));
            }

            line.removeFromLeft (5.0f);

            if (! row.empty)
            {
                // etiqueta a direita, com a MESMA semantica de cor do fader:
                // quente em ambar, impulsionando ganho em vermelho
                if (row.tag.isNotEmpty())
                {
                    auto tag = line.removeFromRight (52.0f);
                    g.setColour (row.dbVal > 0.0f  ? juce::Colour (0xffe0503a).withAlpha (alpha)
                               : row.dbVal > -3.0f ? juce::Colour (0xffe0a83a).withAlpha (alpha)
                                                   : juce::Colours::white.withAlpha (0.65f * alpha));
                    g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
                    g.drawText (row.tag, tag.toNearestInt(), juce::Justification::centredRight, true);
                }

                // mini-medidor entre o nome e o dB: o mesmo dado da barrinha de
                // atividade dos cards, aqui como fita horizontal de 3px
                {
                    const size_t li = (size_t) juce::jlimit (0, (int) (sizeof (levels) / sizeof (levels[0])) - 1,
                                                             row.index - 1);
                    auto met = line.removeFromRight (40.0f).withSizeKeepingCentre (34.0f, 3.0f);

                    g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));
                    g.fillRoundedRectangle (met, 1.5f);

                    const float lv = juce::jlimit (0.0f, 1.0f, std::sqrt (levels[li]));

                    if (lv > 0.01f)
                    {
                        g.setColour (NordLookAndFeel::nordRedBright().withAlpha ((0.55f + 0.45f * lv) * alpha));
                        g.fillRoundedRectangle (met.removeFromLeft (met.getWidth() * lv), 1.5f);
                    }

                    line.removeFromRight (6.0f);
                }
            }

            // contraste maior nos nomes da lista: 92% de branco puro num fundo
            // quase preto ainda lia fraco no tamanho 12
            g.setColour (juce::Colours::white.withAlpha ((row.focused ? 1.0f : row.empty ? 0.35f : 0.88f) * alpha));
            g.setFont (juce::Font (juce::FontOptions (12.5f, row.focused ? juce::Font::bold
                                                                         : juce::Font::plain)));
            g.drawText (row.name, line.toNearestInt(), juce::Justification::centredLeft, true);
        }
    }

private:
    void timerCallback() override
    {
        bool dirty = false;

        // repinta SO quando o acorde muda de nome -- e raro (poucas vezes por
        // segundo), entao o painel nao fica redesenhando a 20 Hz a toa
        if (fetchChord != nullptr)
        {
            const auto c = fetchChord();

            if (c != chordText)
            {
                chordText = c;
                dirty = true;
            }
        }

        // mini-medidores: mesmo criterio -- so repinta se algum nivel se mexeu
        // de verdade (canais parados custam zero)
        if (fetchLevel != nullptr)
        {
            for (size_t i = 0; i < sizeof (levels) / sizeof (levels[0]); ++i)
            {
                const float lv = fetchLevel ((int) i);

                if (std::abs (lv - levels[i]) > 0.012f)
                {
                    levels[i] = lv;
                    dirty = true;
                }
            }
        }

        if (dirty)
            repaint();
    }

    // Pictogramas minusculos (11x9): tem que ler a essa escala, entao sao formas
    // primarias -- nada de contorno fino, que some.
    static void paintIcon (juce::Graphics& g, juce::Rectangle<float> b, Icon icon, juce::Colour c)
    {
        const float x = b.getX(), y = b.getY(), w = b.getWidth(), h = b.getHeight();
        g.setColour (c);

        switch (icon)
        {
            case Icon::piano:   // mini teclado: base clara com dois entalhes pretos
                g.fillRoundedRectangle (b, 1.0f);
                g.setColour (juce::Colour (0xff0d0d0f));
                g.fillRect (x + w * 0.24f, y, w * 0.13f, h * 0.62f);
                g.fillRect (x + w * 0.60f, y, w * 0.13f, h * 0.62f);
                break;

            case Icon::organ:   // drawbars: tres barras de alturas diferentes
                g.fillRect (x + w * 0.06f, y + h * 0.35f, w * 0.18f, h * 0.65f);
                g.fillRect (x + w * 0.40f, y,             w * 0.18f, h);
                g.fillRect (x + w * 0.74f, y + h * 0.55f, w * 0.18f, h * 0.45f);
                break;

            case Icon::strings: // cordas: tres diagonais
                for (int i = 0; i < 3; ++i)
                {
                    const float o = (float) i * w * 0.30f;
                    g.drawLine (x + o, y + h, x + o + w * 0.30f, y, 1.2f);
                }
                break;

            case Icon::pad:     // onda: duas curvas encadeadas
            {
                juce::Path p;
                p.startNewSubPath (x, y + h * 0.5f);
                p.quadraticTo (x + w * 0.25f, y - h * 0.15f, x + w * 0.5f, y + h * 0.5f);
                p.quadraticTo (x + w * 0.75f, y + h * 1.15f, x + w,        y + h * 0.5f);
                g.strokePath (p, juce::PathStrokeType (1.3f));
                break;
            }

            case Icon::bells:   // sino: trapezio com badalo
            {
                juce::Path p;
                p.startNewSubPath (x + w * 0.5f, y);
                p.quadraticTo (x + w, y + h * 0.35f, x + w * 0.92f, y + h * 0.78f);
                p.lineTo (x + w * 0.08f, y + h * 0.78f);
                p.quadraticTo (x, y + h * 0.35f, x + w * 0.5f, y);
                p.closeSubPath();
                g.fillPath (p);
                g.fillEllipse (x + w * 0.38f, y + h * 0.80f, w * 0.24f, h * 0.20f);
                break;
            }

            case Icon::plugin:  // modulo externo: caixa com pino
                g.drawRoundedRectangle (b.reduced (0.6f), 1.5f, 1.2f);
                g.fillRect (x + w * 0.42f, y + h * 0.30f, w * 0.16f, h * 0.40f);
                break;

            case Icon::generic:
            default:
                g.fillRoundedRectangle (b.reduced (w * 0.18f, h * 0.22f), 1.5f);
                break;
        }
    }

    bool sameRows (const std::vector<Row>& other) const
    {
        if (other.size() != rows.size())
            return false;

        for (size_t i = 0; i < other.size(); ++i)
            if (other[i].index != rows[i].index || other[i].icon != rows[i].icon
                 || other[i].name != rows[i].name
                 || other[i].tag != rows[i].tag || other[i].active != rows[i].active
                 || other[i].focused != rows[i].focused || other[i].empty != rows[i].empty
                 || std::abs (other[i].dbVal - rows[i].dbVal) > 0.05f)
                return false;

        return true;
    }

    juce::String slotText, titleText, chordText;
    std::vector<Row> rows;
    float levels[8] = {};   // niveis atuais dos mini-medidores (0..1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OledDisplay)
};
