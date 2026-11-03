#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include "MidiLearnTargetTag.h"

//==============================================================================
// Overlay transparente que cobre o console durante o modo "Mapear MIDI".
//
// Permite selecionar qualquer controle (fader, knob, botão, pad, cena) com um
// clique simples do mouse, sem alterar o valor de áudio nem acionar funções do
// palco. O controle selecionado fica com destaque visual pulsante na tela
// enquanto aguarda o sinal do controlador físico (fader, knob, pad ou tecla).
// Quando o sinal chega, o mapeamento é confirmado com flash verde e salvo.
//==============================================================================
class MidiLearnConsoleOverlay final : public juce::Component,
                                      private juce::Timer
{
public:
    std::function<void (juce::Component* hit,
                        juce::Point<int> localPos,
                        juce::Point<int> screenPos)> onControlClicked;

    std::function<bool (juce::Point<int> screenPos)> shouldPassThrough;

    MidiLearnConsoleOverlay()
    {
        setInterceptsMouseClicks (true, true);

    }

    ~MidiLearnConsoleOverlay() override
    {
        stopTimer();
    }

    bool hitTest (int x, int y) override
    {
        if (shouldPassThrough != nullptr)
        {
            const auto screenPos = localPointToGlobal (juce::Point<int> (x, y));
            if (shouldPassThrough (screenPos))
                return false;
        }
        // The base hitTest honours setInterceptsMouseClicks. Returning true
        // unconditionally made mouseDown find this overlay instead of the fader.
        return juce::Component::hitTest (x, y);
    }

    void visibilityChanged() override
    {
        if (isShowing() && hasActiveTarget())
            startTimerHz (24);
        else
            stopTimer();
    }

    void setTarget (juce::Component* comp,
                    const juce::String& name,
                    juce::Rectangle<int> customBounds = {})
    {
        targetComponent = comp;
        targetName = name;
        targetCustomBounds = customBounds;
        flashText.clear();
        flashExpiresMs = 0;
        if (isShowing())
            startTimerHz (24);
        invalidateOverlayBounds();
    }

    void clearTarget()
    {
        stopTimer();
        const auto oldDirty = lastDirtyBounds;
        lastDirtyBounds = {};
        targetComponent = nullptr;
        targetName.clear();
        targetCustomBounds = {};
        flashText.clear();
        flashExpiresMs = 0;
        if (! oldDirty.isEmpty())
            repaint (oldDirty);
        else
            repaint();
    }

    void flashSuccess (const juce::String& message, bool saved = true)
    {
        flashText = message;
        flashSaved = saved;
        flashExpiresMs = juce::Time::getMillisecondCounter() + 4000;
        if (isShowing())
            startTimerHz (24);
        invalidateOverlayBounds();
    }

    bool hasActiveTarget() const noexcept
    {
        return targetComponent != nullptr || ! targetCustomBounds.isEmpty();
    }

    void paint (juce::Graphics& g) override
    {
        juce::Rectangle<int> bounds;

        if (targetComponent != nullptr && targetComponent->isShowing())
        {
            bounds = getLocalArea (targetComponent.getComponent(),
                                   targetComponent->getLocalBounds());
        }
        else if (! targetCustomBounds.isEmpty())
        {
            bounds = targetCustomBounds;
        }

        if (bounds.isEmpty())
            return;

        const auto b = bounds.expanded (4, 4);
        const float alpha = 0.70f + 0.30f * std::sin (pulsePhase);
        const bool isSuccess = flashText.isNotEmpty();

        // Subtle outline keeps the selected control legible.
        // Borda principal sólida
        const auto strokeColor = isSuccess
            ? (flashSaved ? juce::Colour (0xff2ecc71) : juce::Colour (0xffef6262))
            : juce::Colour (0xffffa800);

        g.setColour (strokeColor.withAlpha (alpha));
        g.drawRoundedRectangle (b.toFloat(), 5.0f, 1.5f);

        // Badge informativo sobre o controle
        const juce::String badgeText = isSuccess
            ? ((flashSaved ? juce::String::fromUTF8 ("✓ ") : juce::String ("! ")) + flashText)
            : (targetName.isNotEmpty()
                ? ("AGUARDANDO MIDI: " + targetName)
                : juce::String ("AGUARDANDO SINAL MIDI..."));

        juce::Font font (juce::FontOptions (11.5f, juce::Font::bold));
        g.setFont (font);
        const int textW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, badgeText));
        const int badgeW = juce::jmin (getLocalBounds().getWidth() - 16, textW + 20);
        const int badgeH = 22;

        int badgeX = juce::jlimit (
            4, juce::jmax (4, getLocalBounds().getWidth() - badgeW - 4),
            b.getCentreX() - badgeW / 2);
        int badgeY = b.getY() - badgeH - 5;
        if (badgeY < 4)
            badgeY = b.getBottom() + 5;
        if (badgeY + badgeH > getLocalBounds().getHeight() - 4)
            badgeY = juce::jmax (4, b.getY() + 4);

        const juce::Rectangle<int> badgeRect (badgeX, badgeY, badgeW, badgeH);
        lastDirtyBounds = b.expanded (4).getUnion (badgeRect.expanded (6));

        // Sombra suave do badge
        g.setColour (juce::Colours::black.withAlpha (0.65f));
        g.fillRoundedRectangle (badgeRect.toFloat().translated (0.0f, 2.0f), 5.0f);

        // Corpo do badge
        g.setColour (juce::Colour (0xf412151d));
        g.fillRoundedRectangle (badgeRect.toFloat(), 5.0f);

        // Borda do badge
        g.setColour (strokeColor.withAlpha (0.95f));
        g.drawRoundedRectangle (badgeRect.toFloat(), 5.0f, 1.2f);

        // Texto do badge
        g.setColour (isSuccess ? juce::Colour (0xff98ffad) : juce::Colours::white);
        g.drawText (badgeText, badgeRect.reduced (4, 0), juce::Justification::centred, true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onControlClicked != nullptr)
        {
            const auto screenPos = e.getScreenPosition();
            // Desativa temporariamente para inspecionar o controle real sob o cursor
            setInterceptsMouseClicks (false, false);
            auto* parent = getParentComponent();
            auto* hit = parent != nullptr
                ? parent->getComponentAt (parent->getLocalPoint (nullptr, screenPos))
                : nullptr;
            setInterceptsMouseClicks (true, true);

            onControlClicked (hit, e.getPosition(), screenPos);
        }
    }

    void mouseDrag (const juce::MouseEvent&) override {}
    void mouseUp (const juce::MouseEvent&) override {}

private:
    void timerCallback() override
    {
        if (! isShowing() || getWidth() <= 0 || getHeight() <= 0)
        {
            stopTimer();
            return;
        }

        pulsePhase += 0.16f;
        if (pulsePhase > juce::MathConstants<float>::twoPi)
            pulsePhase -= juce::MathConstants<float>::twoPi;

        if (flashExpiresMs > 0 && juce::Time::getMillisecondCounter() > flashExpiresMs)
        {
            clearTarget();
            return;
        }

        if (targetComponent != nullptr || ! targetCustomBounds.isEmpty() || flashText.isNotEmpty())
            invalidateOverlayBounds();
    }

    void invalidateOverlayBounds()
    {
        juce::Rectangle<int> bounds;
        if (targetComponent != nullptr && targetComponent->isShowing())
            bounds = getLocalArea (targetComponent.getComponent(),
                                   targetComponent->getLocalBounds());
        else if (! targetCustomBounds.isEmpty())
            bounds = targetCustomBounds;

        juce::Rectangle<int> currentDirty;
        if (! bounds.isEmpty())
        {
            const auto b = bounds.expanded (8);
            const auto badgeZone = juce::Rectangle<int> (
                juce::jmax (0, b.getCentreX() - 220),
                juce::jmax (0, b.getY() - 36),
                440,
                b.getHeight() + 72).getIntersection (getLocalBounds());
            currentDirty = b.getUnion (badgeZone);
        }

        const auto dirty = lastDirtyBounds.getUnion (currentDirty);
        lastDirtyBounds = currentDirty;
        if (! dirty.isEmpty())
            repaint (dirty);
        else
            repaint();
    }

    juce::Rectangle<int> lastDirtyBounds;
    juce::Component::SafePointer<juce::Component> targetComponent;
    juce::String targetName;
    juce::Rectangle<int> targetCustomBounds;
    bool flashSaved = true;
    juce::String flashText;
    juce::uint32 flashExpiresMs = 0;
    float pulsePhase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearnConsoleOverlay)
};
