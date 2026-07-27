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

        stopBtn.setButtonText ("Parar");
        stopBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8f0c21));
        stopBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        stopBtn.onClick = [this] { select (-1); };
        addAndMakeVisible (stopBtn);

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

    // ressincroniza o destaque com o motor (ex.: ao reabrir)
    void refreshState()
    {
        current = pad != nullptr ? pad->getKey() : -1;
        stopBtn.setEnabled (current >= 0);
        repaint();
    }

    static const char* noteName (int i)
    {
        static const char* nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return nm[juce::jlimit (0, 11, i)];
    }

    void toggle (int i) { select (current == i ? -1 : i); }

    void select (int k)
    {
        current = k;

        if (pad != nullptr)
            pad->setKey (k);

        stopBtn.setEnabled (k >= 0);   // "Parar" sem nada tocando nao faz nada
        repaint();
    }

    void refreshVolLabel()
    {
        volLbl.setText ("VOLUME  " + juce::String (juce::roundToInt (vol.getValue() * 100.0)) + "%",
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
        for (int i = 0; i < 5; ++i) if (blackRects[i].contains (pt)) { toggle (kBlack[i]); return; }
        for (int i = 0; i < 7; ++i) if (whiteRects[i].contains (pt)) { toggle (kWhite[i]); return; }
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

            if (current >= 0)
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

    void resized() override
    {
        auto card = getLocalBounds().reduced (12);
        closeBtn.setBounds (card.getRight() - 38, card.getY() + 10, 26, 22);

        auto area = card.reduced (22, 16);
        titleBounds = area.removeFromTop (22);
        area.removeFromTop (8);
        displayBounds = area.removeFromTop (52).toFloat();
        area.removeFromTop (14);

        auto bottom = area.removeFromBottom (48);
        auto volArea = bottom.removeFromLeft (86);
        volLbl.setBounds (volArea.removeFromBottom (12));
        vol.setBounds (volArea.withSizeKeepingCentre (40, 36));
        stopBtn.setBounds (bottom.removeFromRight (104).withSizeKeepingCentre (104, 32));
        area.removeFromBottom (12);

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
    PadProcessor* pad;
    juce::Label volLbl;
    juce::TextButton stopBtn, closeBtn;
    juce::Slider vol;
    int current = -1;

    juce::Rectangle<int> titleBounds;
    juce::Rectangle<float> displayBounds;
    juce::Rectangle<float> whiteRects[7], blackRects[5];
    static constexpr int kWhite[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kBlack[5] = { 1, 3, 6, 8, 10 };

    juce::ComponentDragger dragger;
    bool draggingCard = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPanel)
};
