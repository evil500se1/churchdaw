#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "NordLookAndFeel.h"

#include <functional>
#include <vector>

//==============================================================================
// EDITOR VISUAL DE ZONA (pedido do dono, 17/ago): o combo "Zona: de C3" diz o
// que está valendo, mas não deixa VER onde a mão esquerda termina e a direita
// começa. Aqui o músico enxerga o teclado inteiro e arrasta a faixa.
//
// O painel é CEGO ao motor: recebe (lo,hi) e devolve (lo,hi) por callback. Toda
// a política de áudio/MIDI continua no TimbresProcessor -- este arquivo não
// conhece camada, voz nem nota tocando.
//==============================================================================

// Botao da strip: icone VETORIAL de teclas. O glifo Unicode de teclado
// (U+2328) nao existe na fonte do app e saia como um traco solto.
class ZoneEditButton final : public juce::Button
{
public:
    ZoneEditButton() : juce::Button ("zoneEdit") {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);

        g.setColour (juce::Colour (down ? 0xff24272e
                                        : highlighted ? 0xff1f2229 : 0xff1a1d23));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (highlighted ? 0.16f : 0.07f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 0.8f);

        auto icon = r.reduced (juce::jmax (3.0f, r.getWidth() * 0.20f),
                               juce::jmax (4.0f, r.getHeight() * 0.28f));
        g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 0.86f : 0.35f));
        g.drawRoundedRectangle (icon, 1.4f, 1.0f);

        const float x0 = icon.getX(), w = icon.getWidth();
        const float top = icon.getY(), h = icon.getHeight();

        for (const float f : { 0.33f, 0.66f })
            g.drawLine (x0 + w * f, top, x0 + w * f, top + h, 0.8f);

        for (const float f : { 0.22f, 0.5f, 0.78f })
            g.fillRect (juce::Rectangle<float> (
                x0 + w * f - 0.9f, top, 1.8f, h * 0.55f));
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoneEditButton)
};

class ZoneKeyEditor final : public juce::Component
{
public:
    ZoneKeyEditor()
    {
        setWantsKeyboardFocus (false);
    }

    // Faixa em notas MIDI (0..127). hi >= lo garantido pelo chamador.
    void setZone (int newLo, int newHi)
    {
        const int l = juce::jlimit (0, 127, newLo);
        const int h = juce::jlimit (l, 127, newHi);

        if (l == lo && h == hi)
            return;

        lo = l;
        hi = h;
        ensureRangeVisible();
        repaint();
    }

    int getLo() const noexcept { return lo; }
    int getHi() const noexcept { return hi; }

    // Chamado ao soltar o dedo/mouse: a zona definitiva.
    std::function<void (int, int)> onZoneApplied;
    // Durante o arraste, para o rótulo "C1 – B3" acompanhar ao vivo.
    std::function<void (int, int)> onZonePreview;

    void resized() override
    {
        chooseVisibleRange();
        rebuildKeys();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        g.setColour (NordLookAndFeel::bgControl());
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

        drawRuler (g);

        for (const auto& k : whites)
            drawWhiteKey (g, k);

        // Pretas por último: elas se sobrepõem às brancas vizinhas.
        for (const auto& k : blacks)
            drawBlackKey (g, k);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const float x = (float) e.position.x;
        const float grab = grabTolerance();

        if (std::abs (x - edgeX (lo, false)) <= grab)
        {
            grabMode = Grab::low;
        }
        else if (std::abs (x - edgeX (hi, true)) <= grab)
        {
            grabMode = Grab::high;
        }
        else
        {
            grabMode = Grab::fresh;
            anchor = noteAt (x);
            lo = hi = anchor;
        }

        dragging = true;
        notifyPreview();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;

        const int n = noteAt ((float) e.position.x);
        const int previousLo = lo, previousHi = hi;

        switch (grabMode)
        {
            case Grab::low:   lo = juce::jmin (n, hi); break;
            case Grab::high:  hi = juce::jmax (n, lo); break;
            case Grab::fresh: lo = juce::jmin (anchor, n);
                              hi = juce::jmax (anchor, n); break;
            case Grab::none:  break;
        }

        if (lo != previousLo || hi != previousHi)
        {
            notifyPreview();
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging)
            return;

        dragging = false;
        grabMode = Grab::none;
        repaint();

        if (onZoneApplied != nullptr)
            onZoneApplied (lo, hi);
    }

private:
    struct KeyRect
    {
        int note = 0;
        juce::Rectangle<float> bounds;
    };

    enum class Grab { none, low, high, fresh };

    static bool isBlackKey (int note) noexcept
    {
        static constexpr bool black[12] =
            { false, true, false, true, false, false,
              true, false, true, false, true, false };
        return black[note % 12];
    }

    // Quantas teclas BRANCAS existem em [from, to].
    static int countWhites (int from, int to) noexcept
    {
        int total = 0;
        for (int n = from; n <= to; ++n)
            if (! isBlackKey (n))
                ++total;
        return total;
    }

    // Alcance visível: teclado inteiro só faz sentido em tela larga. Num strip
    // estreito/telefone, 88 teclas viram tiras de 3 px e ninguém acerta o dedo.
    void chooseVisibleRange()
    {
        const int wanted = getWidth() >= 700 ? 21 : 36;   // A0 ou C2
        const int wantedTop = getWidth() >= 700 ? 108 : 96;

        firstNote = wanted;
        lastNote = wantedTop;

        // A zona ATUAL sempre cabe na tela, mesmo vinda de um setup antigo.
        // "Todo o teclado" (0..127) NAO conta: ele so quer dizer "sem split",
        // e esticar para 128 teclas espremeria o piano inteiro sem ganho.
        const bool zonaCheia = lo <= 0 && hi >= 127;

        if (! zonaCheia)
        {
            if (lo < firstNote) firstNote = lo;
            if (hi > lastNote)  lastNote = hi;
        }

        // Começa e termina em tecla branca: meia tecla preta na borda fica feia
        // e confunde o alvo do dedo.
        while (firstNote > 0 && isBlackKey (firstNote))
            --firstNote;
        while (lastNote < 127 && isBlackKey (lastNote))
            ++lastNote;
    }

    void ensureRangeVisible()
    {
        if (getWidth() > 0 && (lo < firstNote || hi > lastNote))
            resized();
    }

    juce::Rectangle<float> keyboardArea() const
    {
        return getLocalBounds().toFloat()
                   .reduced (6.0f, 5.0f)
                   .withTrimmedTop (rulerHeight() + 4.0f);
    }

    float rulerHeight() const noexcept
    {
        return juce::jlimit (10.0f, 18.0f, (float) getHeight() * 0.14f);
    }

    void rebuildKeys()
    {
        whites.clear();
        blacks.clear();

        const auto kb = keyboardArea();
        const int numWhites = countWhites (firstNote, lastNote);

        if (numWhites <= 0 || kb.getWidth() <= 0.0f || kb.getHeight() <= 0.0f)
            return;

        whiteWidth = kb.getWidth() / (float) numWhites;
        const float blackW = whiteWidth * 0.62f;
        const float blackH = kb.getHeight() * 0.62f;

        int whiteIndex = 0;

        for (int n = firstNote; n <= lastNote; ++n)
        {
            if (isBlackKey (n))
                continue;

            KeyRect key;
            key.note = n;
            key.bounds = { kb.getX() + (float) whiteIndex * whiteWidth,
                           kb.getY(), whiteWidth, kb.getHeight() };
            whites.push_back (key);
            ++whiteIndex;
        }

        // Cada preta nasce na divisa das duas brancas que a cercam.
        for (int n = firstNote; n <= lastNote; ++n)
        {
            if (! isBlackKey (n))
                continue;

            const int whitesBefore = countWhites (firstNote, n - 1);
            KeyRect key;
            key.note = n;
            key.bounds = { kb.getX() + (float) whitesBefore * whiteWidth
                               - blackW * 0.5f,
                           kb.getY(), blackW, blackH };
            blacks.push_back (key);
        }
    }

    float grabTolerance() const noexcept
    {
       #if JUCE_ANDROID || JUCE_IOS
        return juce::jmax (26.0f, whiteWidth);
       #else
        return juce::jmax (14.0f, whiteWidth * 0.7f);
       #endif
    }

    // Borda ESQUERDA da nota (rightEdge=false) ou DIREITA (rightEdge=true).
    float edgeX (int note, bool rightEdge) const
    {
        const auto kb = keyboardArea();

        if (whites.empty())
            return kb.getX();

        const int whitesBefore = countWhites (firstNote, note - 1);
        const float x = kb.getX() + (float) whitesBefore * whiteWidth;

        if (! rightEdge)
            return x;

        return x + (isBlackKey (note) ? 0.0f : whiteWidth);
    }

    int noteAt (float x) const
    {
        // Pretas primeiro: elas ficam POR CIMA das brancas.
        for (const auto& k : blacks)
            if (x >= k.bounds.getX() && x < k.bounds.getRight())
                return k.note;

        for (const auto& k : whites)
            if (x >= k.bounds.getX() && x < k.bounds.getRight())
                return k.note;

        // Dedo saiu do teclado durante o arraste (comum no iPad): em vez de
        // travar, projeta pela coluna de brancas.
        const auto kb = keyboardArea();

        if (whites.empty() || whiteWidth <= 0.0f)
            return firstNote;

        const int index = juce::jlimit (
            0, (int) whites.size() - 1,
            (int) ((x - kb.getX()) / whiteWidth));
        return whites[(size_t) index].note;
    }

    bool inZone (int note) const noexcept
    {
        return note >= lo && note <= hi;
    }

    // Tocar a borda do teclado visível = "vai até o fim", senão o preset
    // "Zona: Toda" nunca seria reproduzível pelo arraste.
    void notifyPreview()
    {
        if (onZonePreview != nullptr)
            onZonePreview (effectiveLo(), effectiveHi());
    }

    int effectiveLo() const noexcept { return lo <= firstNote ? 0 : lo; }
    int effectiveHi() const noexcept { return hi >= lastNote ? 127 : hi; }

    void drawRuler (juce::Graphics& g)
    {
        auto track = getLocalBounds().toFloat().reduced (6.0f, 5.0f)
                         .withHeight (rulerHeight());

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (track, 4.0f);

        const float x1 = edgeX (lo, false);
        const float x2 = edgeX (hi, true);
        auto band = track.withLeft (juce::jmax (track.getX(), x1))
                         .withRight (juce::jmin (track.getRight(), x2));

        if (band.getWidth() > 1.0f)
        {
            g.setColour (NordLookAndFeel::nordRed().withAlpha (
                dragging ? 0.95f : 0.80f));
            g.fillRoundedRectangle (band, 4.0f);
        }

        // Pegas nas pontas: dizem "isto se arrasta" sem precisar de legenda.
        g.setColour (juce::Colours::white.withAlpha (0.92f));
        for (const float x : { x1, x2 })
            g.fillRoundedRectangle (
                juce::Rectangle<float> (x - 1.5f, track.getY() - 1.0f,
                                        3.0f, track.getHeight() + 2.0f)
                    .getIntersection (track.expanded (0.0f, 1.0f)), 1.5f);
    }

    void drawWhiteKey (juce::Graphics& g, const KeyRect& key)
    {
        auto r = key.bounds.reduced (0.6f, 0.0f);
        const bool on = inZone (key.note);

        // Fora da zona a tecla ESCURECE, mas continua sendo uma tecla: com o
        // cinza antigo (0xff5a5d64) o teclado inteiro lia como "desligado" e o
        // musico perdia a referencia de onde estava no piano.
        g.setGradientFill (juce::ColourGradient (
            on ? juce::Colour (0xfff6f6f8) : juce::Colour (0xffa8abb2),
            r.getX(), r.getY(),
            on ? juce::Colour (0xffc6cad0) : juce::Colour (0xff74777d),
            r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);

        g.setColour (juce::Colours::black.withAlpha (0.30f));
        g.drawRoundedRectangle (r, 3.0f, 0.8f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRed().withAlpha (0.55f));
            g.fillRoundedRectangle (
                r.withTrimmedTop (r.getHeight() - 5.0f).reduced (1.0f, 0.0f),
                2.0f);
        }

        // Só os DÓs recebem nome: um rótulo por tecla vira ruído ilegível.
        if (key.note % 12 == 0 && r.getWidth() >= 13.0f)
        {
            g.setColour (juce::Colour (0xff33363c).withAlpha (on ? 1.0f : 0.75f));
            g.setFont (NordLookAndFeel::stageLabel (
                juce::jlimit (7.0f, 9.5f, r.getWidth() * 0.42f)));
            g.drawText (noteLabel (key.note),
                        r.toNearestInt().removeFromBottom (14),
                        juce::Justification::centred, false);
        }
    }

    void drawBlackKey (juce::Graphics& g, const KeyRect& key)
    {
        auto r = key.bounds;
        const bool on = inZone (key.note);

        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.5f), 2.5f);

        g.setGradientFill (juce::ColourGradient (
            on ? juce::Colour (0xff35373d) : juce::Colour (0xff23252a),
            r.getX(), r.getY(),
            on ? juce::Colour (0xff0e0f12) : juce::Colour (0xff0c0d10),
            r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 2.5f);

        g.setColour (juce::Colours::white.withAlpha (on ? 0.22f : 0.08f));
        g.fillRoundedRectangle (
            r.withHeight (r.getHeight() * 0.16f).reduced (1.5f, 1.0f), 1.5f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.75f));
            g.fillRoundedRectangle (
                r.withTrimmedTop (r.getHeight() - 4.0f).reduced (0.8f, 0.0f),
                1.5f);
        }
    }

public:
    // "C3", "F#4" -- mesma convenção do combo de zona da strip.
    static juce::String noteLabel (int note)
    {
        return juce::MidiMessage::getMidiNoteName (note, true, true, 4);
    }

private:
    int lo = 0, hi = 127;
    int firstNote = 21, lastNote = 108;
    int anchor = 0;
    float whiteWidth = 0.0f;
    bool dragging = false;
    Grab grabMode = Grab::none;
    std::vector<KeyRect> whites, blacks;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoneKeyEditor)
};

//==============================================================================
// Painel completo: cabeçalho com a faixa em notas, teclado, presets rápidos e
// as ações. Vive como overlay do editor (mesmo padrão do Church Organ).
//==============================================================================
class ZoneEditorPanel final : public juce::Component
{
public:
    ZoneEditorPanel (int channelNumber, juce::String timbreName,
                     int initialLo, int initialHi)
        : channel (channelNumber), timbre (std::move (timbreName)),
          originalLo (initialLo), originalHi (initialHi)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        keys.setZone (initialLo, initialHi);
        keys.onZonePreview = [this] (int l, int h) { updateRangeLabel (l, h); };
        keys.onZoneApplied = [this] (int l, int h)
        {
            updateRangeLabel (l, h);

            if (onZoneChanged != nullptr)
                onZoneChanged (l, h);
        };
        addAndMakeVisible (keys);

        rangeLabel.setJustificationType (juce::Justification::centredLeft);
        rangeLabel.setColour (juce::Label::textColourId,
                              juce::Colours::white);
        addAndMakeVisible (rangeLabel);

        hintLabel.setJustificationType (juce::Justification::centredRight);
        hintLabel.setColour (juce::Label::textColourId,
                             NordLookAndFeel::txtSecondary());
        hintLabel.setText (juce::String::fromUTF8 (
            "Arraste sobre o teclado para escolher a faixa deste canal"),
            juce::dontSendNotification);
        addAndMakeVisible (hintLabel);

        // Mesmos presets do combo da strip: as duas listas TÊM de bater, senão
        // o combo cai no item "personalizado" sem motivo.
        struct Preset { const char* rotulo; int lo, hi; };
        static const Preset presets[] = {
            { "Todo o teclado", 0, 127 },
            { "Abaixo de C3",   0, 47 },
            { "De C3",          48, 127 },
            { "Abaixo de C4",   0, 59 },
            { "De C4",          60, 127 },
            { "Abaixo de C5",   0, 71 },
            { "De C5",          72, 127 },
        };

        for (const auto& preset : presets)
        {
            auto* button = presetButtons.add (
                new juce::TextButton (preset.rotulo));
            button->setComponentID ("stripAction");
            const int presetLo = preset.lo, presetHi = preset.hi;
            button->onClick = [this, presetLo, presetHi]
            {
                keys.setZone (presetLo, presetHi);
                updateRangeLabel (presetLo, presetHi);

                if (onZoneChanged != nullptr)
                    onZoneChanged (presetLo, presetHi);
            };
            addAndMakeVisible (*button);
        }

        closeButton.setComponentID ("stripAction");
        closeButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x90  MIXER"));
        closeButton.onClick = [this] { if (onClose != nullptr) onClose(); };
        addAndMakeVisible (closeButton);

        // Desfazer é obrigatório aqui: o músico mexe AO VIVO e precisa de um
        // caminho de volta sem ter de lembrar os números antigos.
        revertButton.setComponentID ("stripAction");
        revertButton.setButtonText ("DESFAZER");
        revertButton.onClick = [this]
        {
            keys.setZone (originalLo, originalHi);
            updateRangeLabel (originalLo, originalHi);

            if (onZoneChanged != nullptr)
                onZoneChanged (originalLo, originalHi);
        };
        addAndMakeVisible (revertButton);

        updateRangeLabel (initialLo, initialHi);
    }

    std::function<void (int, int)> onZoneChanged;
    std::function<void()> onClose;

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onClose != nullptr)
                onClose();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (NordLookAndFeel::bgApp());

        auto header = getLocalBounds().removeFromTop (headerHeight());
        g.setColour (NordLookAndFeel::bgCard());
        g.fillRect (header);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.55f));
        g.fillRect (header.removeFromBottom (2));

        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (NordLookAndFeel::stageLabel (10.0f));
        g.drawText ("ZONA DO CANAL " + juce::String (channel)
                        + (timbre.isNotEmpty()
                               ? juce::String::fromUTF8 ("  \xc2\xb7  ") + timbre
                               : juce::String()),
                    titleBounds, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const bool wide = getWidth() >= 900 && getHeight() >= 440;
        auto area = getLocalBounds();

        auto header = area.removeFromTop (headerHeight()).reduced (wide ? 14 : 10, 6);
        const int actionW = wide ? 104 : 92;
        closeButton.setBounds (header.removeFromLeft (actionW)
                                   .withSizeKeepingCentre (actionW, wide ? 28 : 34));
        header.removeFromLeft (8);
        revertButton.setBounds (header.removeFromRight (actionW)
                                    .withSizeKeepingCentre (actionW, wide ? 28 : 34));
        titleBounds = header;

        area.reduce (wide ? 14 : 10, wide ? 12 : 8);

        auto readout = area.removeFromTop (wide ? 34 : 30);
        rangeLabel.setBounds (readout.removeFromLeft (
            juce::jmin (260, readout.getWidth() / 2)));
        hintLabel.setBounds (readout);
        rangeLabel.setFont (juce::Font (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(),
            wide ? 22.0f : 19.0f, juce::Font::bold)));
        hintLabel.setFont (NordLookAndFeel::stageLabel (wide ? 10.0f : 9.0f));

        area.removeFromTop (6);

        // Presets embaixo: o teclado é o protagonista e fica com a sobra.
        auto presetRow = area.removeFromBottom (wide ? 34 : 46);
        area.removeFromBottom (10);

        const int count = presetButtons.size();

        if (count > 0)
        {
            const int gap = 6;
            const int cellW = (presetRow.getWidth() - gap * (count - 1)) / count;

            for (int i = 0; i < count; ++i)
            {
                presetButtons[i]->setBounds (presetRow.removeFromLeft (cellW));
                presetRow.removeFromLeft (gap);
            }
        }

        keys.setBounds (area);
    }

private:
    int headerHeight() const noexcept
    {
        return getWidth() >= 900 && getHeight() >= 440 ? 44 : 52;
    }

    void updateRangeLabel (int l, int h)
    {
        const auto text = (l <= 0 && h >= 127)
            ? juce::String ("Todo o teclado")
            : ZoneKeyEditor::noteLabel (l)
                  + juce::String::fromUTF8 ("  \xe2\x80\x93  ")
                  + ZoneKeyEditor::noteLabel (h);
        rangeLabel.setText (text, juce::dontSendNotification);
    }

    int channel = 1;
    juce::String timbre;
    int originalLo = 0, originalHi = 127;

    ZoneKeyEditor keys;
    juce::Label rangeLabel, hintLabel;
    juce::TextButton closeButton, revertButton;
    juce::OwnedArray<juce::TextButton> presetButtons;
    juce::Rectangle<int> titleBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoneEditorPanel)
};
