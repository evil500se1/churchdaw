#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include "PianoVisualState.h"
#include "ChordDetect.h"
#include "NordLookAndFeel.h"

// A read-only musical monitor. It observes the host's existing lock-free MIDI
// telemetry; it never sends notes or touches the processor/audio callback.
class PerformanceDock final : public juce::Component,
                              public juce::SettableTooltipClient,
                              private juce::Timer
{
public:
    explicit PerformanceDock (std::shared_ptr<PianoVisualState> visualState)
        : state (std::move (visualState))
    {
        setComponentID ("performanceDock");
        setTitle ("Monitor de notas MIDI");
        setDescription ("Teclas recebidas, notas sustentadas e leitura de acordes da sessao.");
        setTooltip (juce::String::fromUTF8 (
            "Acompanha as notas MIDI recebidas e o sustain. "
            "O nome do acorde descreve as notas tocadas; inversões podem ter mais de uma leitura."));
        setInterceptsMouseClicks (true, false);
        setWantsKeyboardFocus (false);
    }

    ~PerformanceDock() override { stopTimer(); }

    void visibilityChanged() override
    {
        // 30 Hz parado; a animacao sobe para 60 Hz so enquanto ha movimento.
        if (isVisible())
        {
            // Batidas acontecidas com o dock escondido nao viram um lampejo
            // coletivo quando ele reaparece.
            if (state != nullptr)
                for (size_t n = 0; n < lastStrike.size(); ++n)
                    lastStrike[n] = state->strikes[n].load (std::memory_order_relaxed);

            timerHz = 30;
            startTimerHz (timerHz);
        }
        else
        {
            stopTimer();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff171a20), 0.0f, 0.0f,
                                                 juce::Colour (0xff101216), 0.0f, bounds.getBottom(),
                                                 false));
        g.fillRect (bounds);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine (0, 0.0f, bounds.getWidth());

        auto content = getLocalBounds().reduced (18, 7);
        auto harmony = content.removeFromLeft (186);
        content.removeFromLeft (16);
        auto pedal = content.removeFromRight (114);
        content.removeFromRight (20);

        paintHarmony (g, harmony);
        paintKeyboard (g, content.toFloat());
        paintPedal (g, pedal);
    }

private:
    void paintHarmony (juce::Graphics& g, juce::Rectangle<int> harmony) const
    {
        g.setFont (NordLookAndFeel::stageLabel (8.0f));
        g.setColour (NordLookAndFeel::txtDisabled());
        g.drawText ("NOTAS AO VIVO", harmony.removeFromTop (13),
                    juce::Justification::centredLeft);
        const auto& reading = tracker.value();
        const bool silent = reading.kind == ChordDetect::Kind::silence;

        if (! silent)
        {
            // Filete vermelho: o acorde lido e o dado mais importante da barra.
            g.setColour (NordLookAndFeel::nordRed());
            g.fillRoundedRectangle (harmony.toFloat().removeFromLeft (3.0f).reduced (0.0f, 5.0f), 1.5f);
            harmony.removeFromLeft (10);
        }

        g.setFont (juce::Font (juce::FontOptions (silent ? 12.0f : 24.0f,
                                                 silent ? juce::Font::plain : juce::Font::bold)));
        g.setColour (silent ? NordLookAndFeel::txtSecondary()
                            : NordLookAndFeel::txtPrimary());
        g.drawFittedText (silent ? juce::String ("Aguardando notas") : reading.name,
                          harmony, juce::Justification::centredLeft, 1, 0.65f);
    }

    static void paintWhiteKey (juce::Graphics& g, juce::Rectangle<float> key,
                               int note, float whiteW, float d, float lit, float ringing)
    {
        const auto red = NordLookAndFeel::nordRed();
        const auto redBright = NordLookAndFeel::nordRedBright();
        const auto redDark = NordLookAndFeel::nordRedDark();
        const bool pressed = d > 0.5f;

        auto top = juce::Colour (0xfff5f2eb).interpolatedWith (redBright, lit);
        auto bottom = juce::Colour (0xffd9d4ca).interpolatedWith (red, lit);
        top = top.interpolatedWith (redBright, 0.34f * ringing);
        bottom = bottom.interpolatedWith (red, 0.34f * ringing);

        juce::Path shape;
        shape.addRoundedRectangle (key.getX(), key.getY(), key.getWidth(), key.getHeight(),
                                   2.2f, 2.2f, false, false, true, true);
        g.setGradientFill (juce::ColourGradient (top, 0.0f, key.getY(),
                                                 bottom, 0.0f, key.getBottom(), false));
        g.fillPath (shape);

        const float lip = 3.0f - 1.5f * d;
        const float lipCorner = juce::jmin (2.2f, lip);
        juce::Path lipPath;
        lipPath.addRoundedRectangle (key.getX(), key.getBottom() - lip,
                                     key.getWidth(), lip,
                                     lipCorner, lipCorner, false, false, true, true);
        g.setColour (juce::Colour (0xffb7b1a5).withAlpha (1.0f - 0.3f * ringing)
                         .interpolatedWith (redDark.withAlpha (0.75f), d));
        g.fillPath (lipPath);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.24f),
                                                 0.0f, key.getY(),
                                                 juce::Colours::transparentBlack,
                                                 0.0f, key.getY() + 5.0f, false));
        g.fillRect (key.withHeight (5.0f));

        if (note % 12 == 0 && whiteW >= 14.0f)
        {
            const bool middleC = note == 60;
            g.setColour (pressed ? juce::Colours::white.withAlpha (0.92f)
                                 : middleC ? juce::Colour (0xff4d515a) : juce::Colour (0xff9c978d));
            g.setFont (juce::Font (juce::FontOptions (juce::jmin (9.0f, whiteW * 0.42f),
                                                      middleC ? juce::Font::bold : juce::Font::plain)));
            g.drawText ("C" + juce::String (note / 12 - 1),
                        key.withTrimmedBottom (4.0f).removeFromBottom (11.0f),
                        juce::Justification::centred, false);
        }
    }

    static void paintBlackKey (juce::Graphics& g, juce::Rectangle<float> key,
                               float d, float lit, float ringing)
    {
        const auto red = NordLookAndFeel::nordRed();
        const auto redDark = NordLookAndFeel::nordRedDark();

        g.setColour (juce::Colours::black.withAlpha (0.34f));
        g.fillRoundedRectangle (key.translated (1.3f, 1.2f), 2.0f);

        auto top = juce::Colour (0xff2d3139).interpolatedWith (red, lit);
        auto bottom = juce::Colour (0xff0c0d10).interpolatedWith (redDark, lit);
        top = top.interpolatedWith (red, 0.5f * ringing);
        bottom = bottom.interpolatedWith (redDark, 0.5f * ringing);

        juce::Path shape;
        shape.addRoundedRectangle (key.getX(), key.getY(), key.getWidth(), key.getHeight(),
                                   1.8f, 1.8f, false, false, true, true);
        g.setGradientFill (juce::ColourGradient (top, 0.0f, key.getY(),
                                                 bottom, 0.0f, key.getBottom(), false));
        g.fillPath (shape);

        const float restBevel = juce::jmax (3.0f, key.getHeight() * 0.13f);
        const float bevel = restBevel + (2.0f - restBevel) * d;
        g.setColour (juce::Colours::white.withAlpha (0.14f - 0.02f * d));
        g.fillRoundedRectangle (key.withTop (key.getBottom() - bevel).reduced (1.2f, 0.5f), 1.2f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (key.withTrimmedBottom (bevel).withWidth (1.0f).translated (1.0f, 0.0f));
    }

    void ensureRestingBedCached (juce::Rectangle<int> outer, float scale) const
    {
        const int pxW = juce::jmax (1, juce::roundToInt ((float) outer.getWidth() * scale));
        const int pxH = juce::jmax (1, juce::roundToInt ((float) outer.getHeight() * scale));
        if (cachedBedImage.isValid()
            && cachedBedOuterSize == outer.withZeroOrigin()
            && cachedBedImage.getWidth() == pxW
            && cachedBedImage.getHeight() == pxH)
            return;

        cachedBedOuterSize = outer.withZeroOrigin();
        cachedBedImage = juce::Image (juce::Image::ARGB, pxW, pxH, true);
        juce::Graphics ig (cachedBedImage);
        ig.addTransform (juce::AffineTransform::scale (scale));

        const auto bed = juce::Rectangle<float> (4.0f, 3.0f,
                                                 (float) outer.getWidth() - 8.0f,
                                                 (float) outer.getHeight() - 6.0f);
        ig.setColour (juce::Colour (0xff07080a));
        ig.fillRoundedRectangle (bed.expanded (3.0f, 2.0f), 5.0f);
        ig.setColour (NordLookAndFeel::nordRed().withAlpha (0.82f));
        ig.fillRect (bed.withHeight (2.0f));

        const auto keys = bed.withTrimmedTop (2.5f);
        constexpr int whiteCount = 52;
        const float whiteW = keys.getWidth() / (float) whiteCount;

        int whiteIndex = 0;
        for (int note = firstNote; note <= lastNote; ++note)
        {
            if (isBlack (note)) continue;
            const auto key = juce::Rectangle<float> (keys.getX() + whiteW * (float) whiteIndex++,
                                                     keys.getY(),
                                                     juce::jmax (1.0f, whiteW - 1.0f),
                                                     keys.getHeight());
            paintWhiteKey (ig, key, note, whiteW, 0.0f, 0.0f, 0.0f);
        }

        whiteIndex = 0;
        for (int note = firstNote; note <= lastNote; ++note)
        {
            if (! isBlack (note)) { ++whiteIndex; continue; }
            const auto key = juce::Rectangle<float> (keys.getX() + ((float) whiteIndex - 0.32f) * whiteW,
                                                     keys.getY(), whiteW * 0.64f,
                                                     keys.getHeight() * 0.62f);
            paintBlackKey (ig, key, 0.0f, 0.0f, 0.0f);
        }
    }

    // Um mini piano de verdade: teclas de marfim, pretas com bisel e sombra,
    // feltro vermelho no fundo do teclado e as oitavas marcadas. Nota tocada
    // acende em vermelho; nota que continua soando (sustain/release) fica com
    // o tom suave, para o musico ver o que ainda esta na caixa.
    void paintKeyboard (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        constexpr int whiteCount = 52;
        const float height = juce::jmin (46.0f, area.getHeight());
        const float width = juce::jmin (area.getWidth(), juce::jmax (740.0f, height * 26.0f));
        if (width < 120.0f || height < 12.0f)
            return;

        const auto bedInt = area.withSizeKeepingCentre (width, height).toNearestInt();
        const auto outer = bedInt.expanded (4, 3);
        const float scale = juce::jlimit (1.0f, 3.0f,
                                          g.getInternalContext().getPhysicalPixelScaleFactor());
        ensureRestingBedCached (outer, scale);
        g.drawImage (cachedBedImage, outer.toFloat());

        const auto bed = bedInt.toFloat();
        const auto keys = bed.withTrimmedTop (2.5f);
        const float whiteW = keys.getWidth() / (float) whiteCount;

        const auto isNoteActive = [this] (int n) noexcept
        {
            return n >= firstNote && n <= lastNote
                && (depth[(size_t) n] > 0.001f
                    || glow[(size_t) n] > 0.001f
                    || flash[(size_t) n] > 0.001f);
        };

        bool anyActive = false;
        int whiteIndex = 0;
        for (int note = firstNote; note <= lastNote; ++note)
        {
            if (isBlack (note))
            {
                anyActive |= isNoteActive (note);
                continue;
            }

            const int wi = whiteIndex++;
            if (! isNoteActive (note))
                continue;

            anyActive = true;
            const auto key = juce::Rectangle<float> (keys.getX() + whiteW * (float) wi,
                                                     keys.getY(),
                                                     juce::jmax (1.0f, whiteW - 1.0f),
                                                     keys.getHeight());
            const float d = depth[(size_t) note];
            const float lit = juce::jmax (d, flash[(size_t) note]);
            const float ringing = glow[(size_t) note] * (1.0f - d);
            paintWhiteKey (g, key, note, whiteW, d, lit, ringing);
        }

        if (! anyActive)
            return;

        whiteIndex = 0;
        for (int note = firstNote; note <= lastNote; ++note)
        {
            if (! isBlack (note)) { ++whiteIndex; continue; }

            if (! isNoteActive (note)
                && ! isNoteActive (note - 1)
                && ! isNoteActive (note + 1))
                continue;

            const auto key = juce::Rectangle<float> (keys.getX() + ((float) whiteIndex - 0.32f) * whiteW,
                                                     keys.getY(), whiteW * 0.64f,
                                                     keys.getHeight() * 0.62f);
            const float d = depth[(size_t) note];
            const float lit = juce::jmax (d, flash[(size_t) note]);
            const float ringing = glow[(size_t) note] * (1.0f - d);
            paintBlackKey (g, key, d, lit, ringing);
        }
    }

    void paintPedal (juce::Graphics& g, juce::Rectangle<int> pedal) const
    {
        auto pedalRow = pedal.removeFromTop (juce::jmax (22, pedal.getHeight() / 2));
        const auto lamp = pedalRow.removeFromLeft (14).toFloat().withSizeKeepingCentre (7.0f, 7.0f);
        if (sustain)
        {
            g.setColour (NordLookAndFeel::greenOk().withAlpha (0.22f));
            g.fillEllipse (lamp.expanded (4.0f));
        }
        g.setColour (sustain ? NordLookAndFeel::greenOk() : juce::Colour (0xff3a404a));
        g.fillEllipse (lamp);

        g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        g.setColour (sustain ? NordLookAndFeel::txtPrimary() : NordLookAndFeel::txtSecondary());
        g.drawText ("Sustain", pedalRow.withTrimmedLeft (2), juce::Justification::centredLeft);

        const int noteCount = (int) std::count (sounding.begin(), sounding.end(), true);
        g.setColour (NordLookAndFeel::txtDisabled());
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (noteCount == 0 ? juce::String ("Monitor MIDI")
                                   : juce::String (noteCount) + (noteCount == 1 ? " nota" : " notas"),
                    pedal.withTrimmedLeft (16), juce::Justification::centredLeft);
    }

    static constexpr int firstNote = 21, lastNote = 108;
    static bool isBlack (int note) noexcept
    {
        const int pitch = note % 12;
        return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
    }

    // Aproximacao exponencial ao alvo; true enquanto ainda se move.
    static bool approach (float& value, float target, float dt, float tau) noexcept
    {
        if (value == target)
            return false;

        value += (target - value) * (1.0f - std::exp (-dt / tau));
        if (std::abs (target - value) < 0.004f)
            value = target;
        return true;
    }

    void timerCallback() override
    {
        if (! isShowing() || state == nullptr) return;

        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.0, 0.1, (now - lastTickMs) * 0.001);
        lastTickMs = now;

        bool changed = false, moving = false;
        for (size_t n = 0; n < held.size(); ++n)
        {
            const bool key = state->channels[n].load (std::memory_order_relaxed) != 0;
            const bool sound = state->sounding[n].load (std::memory_order_relaxed) != 0;
            const auto strike = state->strikes[n].load (std::memory_order_relaxed);

            // Nota curtinha (liga e desliga entre dois quadros) nunca aparecia
            // em "held". O contador de batidas pega todas: cada batida acende a
            // tecla por um instante.
            if (strike != lastStrike[n])
            {
                lastStrike[n] = strike;
                flash[n] = 1.0f;
                changed = true;
            }

            changed |= key != held[n] || sound != sounding[n];
            held[n] = key;
            sounding[n] = sound;

            // Acende quase na hora (~10 ms) e apaga macio (~45 ms); o tom de
            // "ainda soando" esmaece junto com o release em vez de sumir seco.
            changed |= approach (depth[n], key ? 1.0f : 0.0f, dt, key ? 0.010f : 0.045f);
            changed |= approach (glow[n], sound ? 1.0f : 0.0f, dt, sound ? 0.020f : 0.200f);

            if (flash[n] > 0.0f)
            {
                flash[n] *= std::exp (-dt / 0.09f);
                if (flash[n] < 0.02f) flash[n] = 0.0f;
                changed = true;
            }

            moving |= depth[n] != (key ? 1.0f : 0.0f) || glow[n] != (sound ? 1.0f : 0.0f)
                      || flash[n] > 0.0f;
        }
        const bool pedal = state->sustainChannels.load (std::memory_order_relaxed) != 0;
        changed |= pedal != sustain;
        sustain = pedal;
        changed |= tracker.update (sounding.data(), now);
        if (changed) repaint();

        // 60 Hz so enquanto alguma tecla se move; parado volta a 30 Hz.
        const int wanted = moving ? 60 : 30;
        if (wanted != timerHz)
        {
            timerHz = wanted;
            startTimerHz (timerHz);
        }
    }

    std::shared_ptr<PianoVisualState> state;
    std::array<bool, 128> held {}, sounding {};
    std::array<uint32_t, 128> lastStrike {};
    std::array<float, 128> depth {}, glow {}, flash {};
    LiveChordTracker tracker;
    mutable juce::Image cachedBedImage;
    mutable juce::Rectangle<int> cachedBedOuterSize;
    bool sustain = false;
    int timerHz = 30;
    double lastTickMs = 0.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformanceDock)
};
