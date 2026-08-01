#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include "PadProcessor.h"

//==============================================================================
// Painel PREMIUM do Pad Continuo — EMBUTIDO na DAW (nao e janela separada): um
// card flutuante com sombra que aparece por cima do mixer. Teclado desenhado a
// mao (marfim + pretas envernizadas), tecla acesa em vermelho, "display" LCD com
// o tom. Clicar nas teclas dispara o pad sustentado.
//==============================================================================
class PadPanel : public juce::Component
{
public:
    explicit PadPanel (PadProcessor* p) : pad (p)
    {
        vol.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        vol.setWantsKeyboardFocus (false);   // setas do teclado do PC nao mexem no knob
        vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        vol.setRange (0.0, 1.5, 0.01);
        vol.setDoubleClickReturnValue (true, 0.9);
        vol.setValue (pad != nullptr ? pad->getGain() : 0.9, juce::dontSendNotification);
        vol.onValueChange = [this]
        {
            if (pad != nullptr)
                pad->setGain ((float) vol.getValue());

            refreshVolLabel();
        };
        addAndMakeVisible (vol);

        // o knob nao mostrava valor nenhum; agora o proprio rotulo e o readout
        volLbl.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
        volLbl.setJustificationType (juce::Justification::centred);
        volLbl.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (volLbl);
        refreshVolLabel();

        // CORTE (pedido de cliente, 01/08/2026): abre e fecha o brilho do pad
        // ao vivo. O parametro mora no PadProcessor, entao o mesmo controle
        // pode ser mapeado num knob ou pedal pelo Mapear MIDI.
        corte.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        corte.setWantsKeyboardFocus (false);
        corte.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        corte.setRange (0.0, 1.0, 0.001);
        corte.setDoubleClickReturnValue (true, 1.0);   // duplo clique = totalmente aberto
        corte.setValue (pad != nullptr ? pad->getCutoff() : 1.0, juce::dontSendNotification);
        corte.onValueChange = [this]
        {
            if (pad != nullptr)
                pad->setCutoff ((float) corte.getValue());

            refreshCorteLabel();
        };
        addAndMakeVisible (corte);

        corteLbl.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
        corteLbl.setJustificationType (juce::Justification::centred);
        corteLbl.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (corteLbl);
        refreshCorteLabel();

        stopBtn.setButtonText ("Parar");
        stopBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8f0c21));
        stopBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        stopBtn.onClick = [this] { handleUserSelection (-1); };
        addAndMakeVisible (stopBtn);

        // O desenho continua compacto, mas uma camada transparente de 44 px
        // recebe o toque ao redor dele. O X antigo de 26x22 era um alvo de
        // mouse em uma tela que tambem roda no palco por toque.
        closeHitTarget.setColour (juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
        closeHitTarget.setColour (juce::TextButton::buttonOnColourId,
                                  juce::Colours::transparentBlack);
        closeHitTarget.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeHitTarget);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

        current = pad != nullptr ? pad->getKey() : -1;
        stopBtn.setEnabled (current >= 0);

        // Era 624x400 com o teclado ocupando 60% do card. Isto aqui e um SELETOR
        // de 12 tons, nao um instrumento pra tocar — o teclado nao precisa (nem
        // deve) prometer mais do que faz.
        setSize (560, 318);
    }

    std::function<void()> onClose;
    std::function<void(int)> onMidiLearnTarget;

    void setMidiLearnMode (bool active)
    {
        midiLearnMode = active;
        stopBtn.setEnabled (active || current >= 0);
    }

    // O aprendizado de notas entra pelo MIDI Learn GLOBAL do app
    // (onMidiLearnTarget + setMidiLearnMode acima), e nao por um botao
    // proprio deste painel: e o mesmo gesto que o musico ja usa para mapear
    // qualquer outro controle, e o passo 2 aparece no MidiLearnPanel.

    // ressincroniza o destaque com o motor (ex.: ao reabrir)
    void refreshState()
    {
        current = pad != nullptr ? pad->getKey() : -1;
        stopBtn.setEnabled (midiLearnMode || current >= 0);
        refreshFromProcessor();
        repaint();
    }

    static const char* noteName (int i)
    {
        static const char* nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return nm[juce::jlimit (0, 11, i)];
    }

    // TECLAS FISICAS ATRIBUIDAS A CADA TOM.
    //
    // Sem isto o mapeamento por nota era invisivel: o unico sintoma de uma
    // tecla mapeada — inclusive de uma mapeada POR ENGANO — era ela parar de
    // tocar, sem nenhum lugar no app onde o musico pudesse descobrir o porque.
    // Aqui o tom mostra a etiqueta da tecla que o comanda ("C1"), no mesmo
    // teclado onde ele escolhe o tom.
    //
    // Cada posicao guarda o rotulo das teclas do tom 0..11; a ultima e a de
    // Parar. Vazio = nenhuma tecla atribuida.
    struct TeclaMapeada
    {
        juce::String rotulo;
        bool aparelhoPresente = true;   // false = mapeado, mas esse teclado nao esta ligado agora
    };

    void setTeclasMapeadas (const std::array<TeclaMapeada, 12>& porTom, TeclaMapeada parar)
    {
        for (int i = 0; i < 12; ++i)
        {
            etiquetas[i] = porTom[(size_t) i].rotulo;
            presente[i]  = porTom[(size_t) i].aparelhoPresente;
        }

        pararPresente = parar.aparelhoPresente;
        etiquetaDeParar = std::move (parar.rotulo);

        // NO PROPRIO BOTAO, nao so na dica.
        //
        // Dica de mouse nao existe em tela sensivel ao toque — que e onde este
        // app roda no palco. E desde que a tecla de tom passou a SELECIONAR em
        // vez de alternar, "Parar" virou a unica forma de calar o Pad pelo
        // teclado: era justamente a atribuicao mais importante e a unica
        // invisivel.
        // fromUTF8 explicito, como no resto do projeto: o construtor simples de
        // String com bytes crus saiu como "Â·" na tela.
        stopBtn.setButtonText (etiquetaDeParar.isEmpty()
                                 ? juce::String ("Parar")
                                 : juce::String::fromUTF8 ("Parar  \xe2\x80\xa2  ") + etiquetaDeParar
                                     + (pararPresente ? juce::String() : juce::String (" (?)")));

        stopBtn.setTooltip (etiquetaDeParar.isEmpty()
                              ? juce::String ("Desliga o Pad Continuo")
                              : pararPresente
                                  ? "Desliga o Pad Continuo  (tecla " + etiquetaDeParar + ")"
                                  : juce::String::fromUTF8 ("Tecla ") + etiquetaDeParar
                                      + juce::String::fromUTF8 (" mapeada noutro teclado, que n\xc3\xa3o est\xc3\xa1 ligado agora"));
        repaint();
    }

    void toggle (int i) { select (current == i ? -1 : i); }

    void select (int k)
    {
        current = k;

        if (pad != nullptr)
            pad->setKey (k);

        stopBtn.setEnabled (midiLearnMode || k >= 0);
        repaint();
    }

    void refreshVolLabel()
    {
        volLbl.setText ("VOLUME  " + juce::String (juce::roundToInt (vol.getValue() * 100.0)) + "%",
                        juce::dontSendNotification);
    }

    // Chamado quando algo mexe no Pad por fora (um CC mapeado, por exemplo).
    // Sem isto o knob da tela continuaria mostrando o valor antigo enquanto o
    // som ja tinha mudado.
    void refreshFromProcessor()
    {
        if (pad == nullptr)
            return;

        vol.setValue (pad->getGain(), juce::dontSendNotification);
        corte.setValue (pad->getCutoff(), juce::dontSendNotification);
        refreshVolLabel();
        refreshCorteLabel();
    }

    void refreshCorteLabel()
    {
        // Mostra a frequencia de verdade, nao a porcentagem do knob: "CORTE
        // 1.5 kHz" diz ao musico o que ele esta fazendo; "CORTE 50%" nao diz
        // nada. Com o knob no fim, "aberto" e mais honesto que "19.2 kHz",
        // porque ali o filtro sai do caminho.
        const double t = corte.getValue();

        if (t >= 0.999)
        {
            corteLbl.setText ("CORTE  aberto", juce::dontSendNotification);
            return;
        }

        const double hz = 120.0 * std::pow (160.0, t);

        corteLbl.setText (hz >= 1000.0
                            ? "CORTE  " + juce::String (hz / 1000.0, 1) + " kHz"
                            : "CORTE  " + juce::String (juce::roundToInt (hz)) + " Hz",
                          juce::dontSendNotification);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto pt = e.position;

        // arrastar o card pela faixa do topo (titulo) — reposiciona onde quiser
        auto card = getLocalBounds().reduced (12);
        if (pt.y < (float) (card.getY() + 46))
        {
            draggingCard = true;
            dragger.startDraggingComponent (this, e);
            return;
        }

        draggingCard = false;
        for (int i = 0; i < 5; ++i) if (blackRects[i].contains (pt)) { handleUserSelection (kBlack[i]); return; }
        for (int i = 0; i < 7; ++i) if (whiteRects[i].contains (pt)) { handleUserSelection (kWhite[i]); return; }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingCard)
            dragger.dragComponent (this, e, nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced (12.0f);
        const float rad = 14.0f;

        // sombra do card flutuante (aparece por cima do mixer)
        {
            juce::Path sp;
            sp.addRoundedRectangle (card, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 26, { 0, 7 }).drawForPath (g, sp);
        }

        // corpo do card (vidro escuro com vinheta)
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1c22), card.getX(), card.getY(),
                                                 juce::Colour (0xff101013), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, rad);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (card.getX() + 14.0f, card.getY() + 1.5f, card.getRight() - 14.0f, card.getY() + 1.5f, 1.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.45f));
        g.drawRoundedRectangle (card, rad, 1.2f);

        // acento vermelho no topo do card
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (card.reduced (16.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f), 1.5f);

        // titulo
        g.setColour (NordLookAndFeel::nordRedBright());
        g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened().withExtraKerningFactor (0.12f));
        g.drawText (juce::String::fromUTF8 ("PAD CONT\xc3\x8dNUO"), titleBounds, juce::Justification::centred);

        // alca de arrastar (grip) no topo-esquerdo — indica que o card move
        {
            const float gx = card.getX() + 20.0f, gy = card.getY() + 16.0f;
            g.setColour (NordLookAndFeel::dimText().withAlpha (0.45f));
            for (int rr = 0; rr < 2; ++rr)
                for (int cc = 0; cc < 3; ++cc)
                    g.fillEllipse (gx + (float) cc * 5.0f, gy + (float) rr * 5.0f, 2.2f, 2.2f);
        }

        // display LCD (tom tocando)
        {
            auto d = displayBounds;
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRoundedRectangle (d.translated (0, 1.5f), 9.0f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0c1512), d.getX(), d.getY(),
                                                     juce::Colour (0xff070a09), d.getX(), d.getBottom(), false));
            g.fillRoundedRectangle (d, 9.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (d.reduced (0.5f), 9.0f, 1.0f);

            // Com o MIDI Learn armado o visor vira instrucao: mapear e coisa
            // de ensaio, e o musico precisa LER o que fazer em vez de
            // adivinhar por que a tecla parou de ligar o pad.
            if (midiLearnMode)
            {
                g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
                g.setColour (NordLookAndFeel::nordRedBright());
                g.drawText ("MAPEAR NOTAS",
                            d.toNearestInt().reduced (14, 8).removeFromTop (11),
                            juce::Justification::centredLeft);

                g.setFont (juce::Font (juce::FontOptions (12.5f)));
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawText (juce::String::fromUTF8 ("Toque no tom que quer mapear"),
                            d.toNearestInt().reduced (14, 4),
                            juce::Justification::centred);
            }
            else if (current >= 0)
            {
                // o rotulo "PAD" repetia o titulo do card logo acima — saiu
                g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
                g.setColour (NordLookAndFeel::dimText());
                g.drawText ("TOM", d.toNearestInt().reduced (14, 8).removeFromTop (11).removeFromLeft (140),
                            juce::Justification::centredLeft);

                g.setColour (NordLookAndFeel::nordRed().withAlpha (0.35f));
                g.setFont (juce::Font (juce::FontOptions (30.0f)).boldened());
                g.drawText (noteName (current), d.toNearestInt().translated (0, 1), juce::Justification::centred);
                g.setColour (NordLookAndFeel::nordRedBright());
                g.drawText (noteName (current), d.toNearestInt(), juce::Justification::centred);

                g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
                g.setColour (juce::Colour (0xff5fd08a));
                g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8f TOCANDO"),
                            d.toNearestInt().reduced (14, 8).removeFromBottom (11).removeFromRight (120),
                            juce::Justification::centredRight);
            }
            else
            {
                // instrucao em vez de "— desligado —": diz o que fazer
                g.setFont (juce::Font (juce::FontOptions (13.5f)));
                g.setColour (NordLookAndFeel::dimText().withAlpha (0.75f));
                g.drawText (juce::String::fromUTF8 ("Escolha o tom da m\xc3\xbasica"),
                            d.toNearestInt(), juce::Justification::centred);
            }
        }

        for (int w = 0; w < 7; ++w) drawWhite (g, whiteRects[w], kWhite[w]);
        for (int i = 0; i < 5; ++i) drawBlack (g, blackRects[i], kBlack[i]);

        // As etiquetas vem DEPOIS das teclas pretas, que sao desenhadas por
        // cima das brancas. Desenhadas junto com a tecla branca, a etiqueta do
        // C ficava escondida atras do C# — some justamente onde o musico
        // procura por ela.
        for (int w = 0; w < 7; ++w) desenharEtiqueta (g, whiteRects[w], kWhite[w], kWhite[w] == current);
        for (int i = 0; i < 5; ++i) desenharEtiqueta (g, blackRects[i], kBlack[i], kBlack[i] == current);
    }

    void drawWhite (juce::Graphics& g, juce::Rectangle<float> r, int key)
    {
        const bool on = (key == current);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.translated (0, 2.0f), 5.0f);

        // marfim CONTIDO: o branco quase puro de antes era a area mais clara da
        // tela inteira e cegava num palco escuro
        g.setGradientFill (on
            ? juce::ColourGradient (juce::Colour (0xffe4123a), r.getX(), r.getY(), juce::Colour (0xff8f0c21), r.getX(), r.getBottom(), false)
            : juce::ColourGradient (juce::Colour (0xffd6d8dd), r.getX(), r.getY(), juce::Colour (0xffa3a8b1), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 5.0f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.45f));
            g.drawRoundedRectangle (r.expanded (1.5f), 6.0f, 2.5f);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.10f));
            g.drawLine (r.getX() + 3.0f, r.getY() + 1.0f, r.getRight() - 3.0f, r.getY() + 1.0f, 1.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        g.setColour (on ? juce::Colours::white : juce::Colour (0xff33363c));
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
        g.drawText (noteName (key), r.toNearestInt().removeFromBottom (26), juce::Justification::centred);
    }

    void drawBlack (juce::Graphics& g, juce::Rectangle<float> r, int key)
    {
        const bool on = (key == current);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (r.translated (0, 2.5f), 3.5f);

        g.setGradientFill (on
            ? juce::ColourGradient (juce::Colour (0xffe4123a), r.getX(), r.getY(), juce::Colour (0xff7c0b1d), r.getX(), r.getBottom(), false)
            : juce::ColourGradient (juce::Colour (0xff35373d), r.getX(), r.getY(), juce::Colour (0xff0e0f12), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.5f);

        g.setColour (juce::Colours::white.withAlpha (on ? 0.30f : 0.14f));   // verniz no topo
        g.fillRoundedRectangle (r.withHeight (r.getHeight() * 0.16f).reduced (2.0f, 1.5f), 2.0f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.55f));
            g.drawRoundedRectangle (r.expanded (1.0f), 4.5f, 1.5f);
        }

        g.setColour (on ? juce::Colours::white : juce::Colours::white.withAlpha (0.78f));
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText (noteName (key), r.toNearestInt().removeFromBottom (18), juce::Justification::centred);
    }

    // Etiqueta da tecla fisica, no ALTO da tecla desenhada — longe do nome do
    // tom, que fica embaixo, e pequena o bastante para nao competir com ele.
    // O ponto sozinho ja diria "tem alguma coisa aqui"; o rotulo diz QUAL
    // tecla, que e o que o musico precisa para achar (ou desfazer) o mapa.
    void desenharEtiqueta (juce::Graphics& g, juce::Rectangle<float> r, int key, bool on)
    {
        if (! juce::isPositiveAndBelow (key, 12) || etiquetas[key].isEmpty())
            return;

        auto faixa = r.toNearestInt().removeFromTop (juce::jmin (15, r.toNearestInt().getHeight() / 3));

        // APARELHO AUSENTE NAO PODE PARECER TECLA VIVA.
        //
        // O mapa guarda o identificador da porta. Se o musico mapeou no teclado
        // de casa e no domingo levou outro, a tecla nao responde — e uma
        // etiqueta vermelha ali estaria prometendo justamente o que nao
        // funciona. Apagada, ela conta a verdade: "isto esta mapeado, mas nao
        // com o que esta plugado agora".
        const bool vivo = presente[key];

        g.setColour (! vivo ? NordLookAndFeel::dimText().withAlpha (0.55f)
                            : on ? juce::Colours::white.withAlpha (0.92f)
                                 : NordLookAndFeel::nordRed().withAlpha (0.95f));
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText (etiquetas[key], faixa, juce::Justification::centred);
    }

    void resized() override
    {
        const bool narrow = getWidth() < 440 || getHeight() < 290;
        auto card = getLocalBounds().reduced (narrow ? 8 : 12);
        const auto closeHit = juce::Rectangle<int> (card.getRight() - 44,
                                                    card.getY(), 44, 44);
        closeHitTarget.setBounds (closeHit);
        closeBtn.setBounds (closeHit.withSizeKeepingCentre (26, 22));
        closeBtn.toFront (false);

        auto area = card.reduced (narrow ? 12 : 22, narrow ? 8 : 16);
        titleBounds = area.removeFromTop (narrow ? 20 : 22);
        area.removeFromTop (narrow ? 4 : 8);
        displayBounds = area.removeFromTop (narrow ? 44 : 52).toFloat();
        area.removeFromTop (narrow ? 6 : 14);

        // 44 px reais para Parar e para os dois knobs. O grafismo dos knobs
        // cresce apenas quatro pixels, mas o alvo deixa de exigir precisao de
        // mouse. O botao Parar tambem ganha largura para "Parar • C#1 (?)".
        auto bottom = area.removeFromBottom (narrow ? 52 : 54);

        auto volArea = bottom.removeFromLeft (narrow ? 70 : 86);
        volLbl.setBounds (volArea.removeFromBottom (12));
        vol.setBounds (volArea.withSizeKeepingCentre (44, 44));

        // O corte fica ao lado do volume: sao os dois controles continuos do
        // pad, e o musico procura os dois no mesmo canto.
        auto corteArea = bottom.removeFromLeft (narrow ? 80 : 96);
        corteLbl.setBounds (corteArea.removeFromBottom (12));
        corte.setBounds (corteArea.withSizeKeepingCentre (44, 44));

        const int stopWidth = juce::jmin (narrow ? 144 : 148, bottom.getWidth());
        stopBtn.setBounds (bottom.removeFromRight (stopWidth)
                                .withSizeKeepingCentre (stopWidth, 44));
        area.removeFromBottom (narrow ? 4 : 12);

        auto kb = area.toFloat();
        const float ww = kb.getWidth() / 7.0f;
        for (int w = 0; w < 7; ++w)
            whiteRects[w] = juce::Rectangle<float> (kb.getX() + (float) w * ww + 2.0f, kb.getY(), ww - 4.0f, kb.getHeight());

        const float bw = ww * 0.60f, bh = kb.getHeight() * 0.62f;
        static const int afterWhite[5] = { 0, 1, 3, 4, 5 };
        for (int i = 0; i < 5; ++i)
            blackRects[i] = juce::Rectangle<float> (kb.getX() + (float) (afterWhite[i] + 1) * ww - bw * 0.5f, kb.getY(), bw, bh);
    }

private:
    void handleUserSelection (int key)
    {
        if (midiLearnMode && onMidiLearnTarget)
        {
            onMidiLearnTarget (key);
            return;
        }

        if (key < 0)
            select (-1);
        else
            toggle (key);
    }

    PadProcessor* pad;
    juce::Label volLbl;
    juce::TextButton stopBtn, closeHitTarget, closeBtn;
    juce::Slider vol;
    juce::Slider corte;
    juce::Label  corteLbl;
    int current = -1;

    juce::Rectangle<int> titleBounds;
    juce::Rectangle<float> displayBounds;
    juce::Rectangle<float> whiteRects[7], blackRects[5];
    static constexpr int kWhite[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kBlack[5] = { 1, 3, 6, 8, 10 };

    juce::ComponentDragger dragger;
    bool draggingCard = false;
    bool midiLearnMode = false;

    // Rotulo da tecla fisica atribuida a cada tom (0..11) e a Parar.
    juce::String etiquetas[12];
    bool presente[12] { true, true, true, true, true, true, true, true, true, true, true, true };
    juce::String etiquetaDeParar;
    bool pararPresente = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPanel)
};
