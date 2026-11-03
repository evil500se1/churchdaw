#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "NordLookAndFeel.h"

//==============================================================================
// Device names remain complete in the tooltip. The desktop rack presents them
// as one continuous strip; compact and standalone layouts retain their chips.
//==============================================================================
class DeviceInfoBar : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    std::function<void()> onOpenSettings;

    DeviceInfoBar()
    {
        setInterceptsMouseClicks (true, false);
    }

    void setControllerName (const juce::String& name, bool connected)
    {
        if (controller != name || ctrlOn != connected)
        {
            controller = name;
            ctrlOn = connected;
            updateTooltip();
            repaint();
        }
    }

    // Tag de transporte (USB / BLE) no chip MIDI — opcional.
    void setControllerTransport (const juce::String& transportTag)
    {
        if (controllerTransport != transportTag)
        {
            controllerTransport = transportTag;
            updateTooltip();
            repaint();
        }
    }

    void setInterfaceName (const juce::String& name, bool connected)
    {
        if (interfaceName != name || ifaceOn != connected)
        {
            interfaceName = name;
            ifaceOn = connected;
            updateTooltip();
            repaint();
        }
    }

    void setInterfaceTransport (const juce::String& transportTag)
    {
        if (interfaceTransport != transportTag)
        {
            interfaceTransport = transportTag;
            updateTooltip();
            repaint();
        }
    }

    void setCompact (bool shouldBeCompact)
    {
        if (compact != shouldBeCompact)
        {
            compact = shouldBeCompact;
            repaint();
        }
    }

    void setIntegratedRack (bool shouldIntegrate)
    {
        if (integratedRack != shouldIntegrate)
        {
            integratedRack = shouldIntegrate;
            repaint();
        }
    }

    void setMappingCount (int count)
    {
        count = juce::jmax (0, count);

        if (mappingCount != count)
        {
            mappingCount = count;
            updateTooltip();
            repaint();
        }
    }

    void setMidiActivity (bool active)
    {
        if (midiActivity != active)
        {
            midiActivity = active;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();

        if (area.isEmpty())
            return;

        if (integratedRack && ! compact)
        {
            drawIntegratedRack (g, area.toFloat());
            return;
        }

        juce::String keyboardTag (compact ? "MIDI" : "TECLADO");

        if (controllerTransport.isNotEmpty())
            keyboardTag = controllerTransport + " " + keyboardTag;

        if (mappingCount > 0)
            keyboardTag << "  " << juce::String::charToString ((juce::juce_wchar) 0x2022)
                        << "  " << mappingCount << (mappingCount == 1 ? " MAPA" : " MAPAS");

        if (compact)
        {
            const int nameLines = area.getHeight() >= 38 ? 2 : 1;
            const auto shownName = controller.isNotEmpty()
                ? (nameLines > 1 ? controller : shortControllerName (controller))
                : juce::String ("Nenhum");
            drawChip (g, area.toFloat(), keyboardTag,
                      shownName,
                      ctrlOn, Glyph::keys, midiActivity,
                      nameLines);
            return;
        }

        const int gap = 8;
        const int chipW = (area.getWidth() - gap) / 2;

        // Tags CURTAS: "CONTROLADOR"/"INTERFACE" nao cabiam e saiam cortadas
        // ("CONTROLADO", "INTERFA"), e ainda roubavam largura do nome do aparelho,
        // que e a informacao que importa.
        drawChip (g, area.removeFromLeft (chipW).toFloat(),
                  keyboardTag,
                  controller.isNotEmpty() ? controller : juce::String ("Nenhum"),
                  ctrlOn, Glyph::keys, midiActivity, 1, integratedRack);

        area.removeFromLeft (gap);

        juce::String audioTag (juce::String::fromUTF8 ("\xc3\x81UDIO")); // ÁUDIO
        if (interfaceTransport.isNotEmpty())
            audioTag = interfaceTransport + " " + audioTag;

        drawChip (g, area.toFloat(),
                  audioTag,
                  interfaceName.isNotEmpty() ? interfaceName : juce::String ("Nenhum"),
                  ifaceOn, Glyph::speaker, false, 1, integratedRack);
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        setMouseCursor (onOpenSettings ? juce::MouseCursor::PointingHandCursor
                                      : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (onOpenSettings && event.mods.isLeftButtonDown()
            && ! event.mouseWasDraggedSinceMouseDown()
            && getLocalBounds().contains (event.getPosition()))
            onOpenSettings();
    }

private:
    enum class Glyph { keys, speaker };

    void drawIntegratedRack (juce::Graphics& g, juce::Rectangle<float> area)
    {
        if (onOpenSettings && isMouseOver())
        {
            g.setColour (juce::Colours::white.withAlpha (0.025f));
            g.fillRoundedRectangle (area.reduced (1.0f, 2.0f), 8.0f);
        }

        const float dividerX = std::floor (area.getCentreX()) + 0.5f;
        const float dividerInset = juce::jmin (13.0f, area.getHeight() * 0.25f);
        g.setColour (juce::Colours::white.withAlpha (0.075f));
        g.drawVerticalLine (juce::roundToInt (dividerX), area.getY() + dividerInset,
                            area.getBottom() - dividerInset);

        auto midiArea = area.removeFromLeft (area.getWidth() * 0.5f);
        const auto disconnected = juce::String::fromUTF8 ("N\xc3\xa3o conectado");
        drawIntegratedDevice (g, midiArea, "TECLADO",
                              controller.isNotEmpty() ? controller : disconnected,
                              ctrlOn, Glyph::keys, midiActivity, mappingCount,
                              onOpenSettings != nullptr);
        drawIntegratedDevice (g, area, juce::String::fromUTF8 ("SA\xc3\x8d" "DA DE \xc3\x81UDIO"),
                              interfaceName.isNotEmpty() ? interfaceName : disconnected,
                              ifaceOn, Glyph::speaker, false, 0,
                              onOpenSettings != nullptr);
    }

    static void drawIntegratedDevice (juce::Graphics& g, juce::Rectangle<float> area,
                                      juce::String label, const juce::String& name,
                                      bool connected, Glyph glyph, bool activity,
                                      int mappings, bool actionable)
    {
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (area.toNearestInt());
        auto inner = area.reduced (area.getWidth() < 155.0f ? 9.0f : 14.0f, 0.0f);
        if (inner.getWidth() <= 0.0f || inner.getHeight() <= 0.0f)
            return;

        const auto status = connected ? juce::Colour (0xff64d8ad)
                                      : juce::Colour (0xffd7ac70);
        const bool showIcon = inner.getWidth() >= 110.0f && inner.getHeight() >= 32.0f;
        if (showIcon)
        {
            auto iconArea = inner.removeFromLeft (23.0f);
            inner.removeFromLeft (10.0f);
            const auto icon = juce::Rectangle<float> (iconArea.getX(),
                                                     iconArea.getCentreY() - 8.0f,
                                                     20.0f, 16.0f);
            g.setColour (juce::Colours::white.withAlpha (connected ? 0.68f : 0.40f));
            drawRackIcon (g, icon, glyph);

            auto dot = juce::Rectangle<float> (icon.getRight() - 3.0f,
                                               icon.getBottom() - 2.0f, 5.0f, 5.0f);
            if (activity)
            {
                g.setColour (status.withAlpha (0.17f));
                g.fillEllipse (dot.expanded (2.5f));
            }
            g.setColour (juce::Colour (0xff171a20));
            g.fillEllipse (dot.expanded (1.5f));
            g.setColour (status);
            g.fillEllipse (dot);
        }

        if (actionable && inner.getWidth() >= 130.0f)
        {
            const auto affordance = inner.removeFromRight (10.0f);
            inner.removeFromRight (5.0f);
            juce::Path chevron;
            const float x = affordance.getCentreX();
            const float y = affordance.getCentreY();
            chevron.startNewSubPath (x - 2.0f, y - 3.0f);
            chevron.lineTo (x + 1.0f, y);
            chevron.lineTo (x - 2.0f, y + 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.24f));
            g.strokePath (chevron, juce::PathStrokeType (1.2f,
                          juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (inner.getWidth() < 115.0f)
            label = glyph == Glyph::keys ? "MIDI" : juce::String::fromUTF8 ("\xc3\x81UDIO");

        const float groupHeight = juce::jmin (32.0f, inner.getHeight());
        inner = inner.withSizeKeepingCentre (inner.getWidth(), groupHeight);
        auto labelLine = inner.removeFromTop (12.0f);
        const auto labelFont = juce::Font (juce::FontOptions (8.6f, juce::Font::bold))
                                   .withExtraKerningFactor (0.08f);
        g.setFont (labelFont);

        if (mappings > 0)
        {
            const juce::String mappingText = juce::String (mappings)
                                               + (mappings == 1 ? " MAPA" : " MAPAS");
            const float desiredWidth = juce::GlyphArrangement::getStringWidth (labelFont, mappingText) + 10.0f;
            const float countWidth = juce::jmin (desiredWidth, labelLine.getWidth() * 0.58f);
            const auto countArea = labelLine.removeFromRight (countWidth);
            labelLine.removeFromRight (5.0f);
            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.fillRoundedRectangle (countArea.reduced (0.0f, 0.5f), 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.57f));
            g.drawText (mappingText, countArea.reduced (4.0f, 0.0f).toNearestInt(),
                        juce::Justification::centred, true);
        }

        g.setColour (juce::Colours::white.withAlpha (0.43f));
        g.drawText (label, labelLine.toNearestInt(), juce::Justification::centredLeft, true);
        inner.removeFromTop (2.0f);

        if (! showIcon)
        {
            auto dotArea = inner.removeFromLeft (9.0f);
            g.setColour (status);
            g.fillEllipse (dotArea.getX(), dotArea.getCentreY() - 2.0f, 4.0f, 4.0f);
        }

        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::plain)));
        g.setColour (connected ? juce::Colour (0xffeef1f5) : NordLookAndFeel::txtSecondary());
        g.drawText (name, inner.toNearestInt(), juce::Justification::centredLeft, true);
    }

    static void drawRackIcon (juce::Graphics& g, juce::Rectangle<float> r, Glyph glyph)
    {
        if (glyph == Glyph::keys)
        {
            g.drawRoundedRectangle (r.reduced (0.7f), 2.1f, 1.2f);
            for (float position : { 0.25f, 0.5f, 0.75f })
                g.drawLine (r.getX() + r.getWidth() * position, r.getCentreY(),
                            r.getX() + r.getWidth() * position, r.getBottom() - 1.3f, 1.0f);
            for (float position : { 0.25f, 0.5f, 0.75f })
                g.fillRoundedRectangle (r.getX() + r.getWidth() * position - 1.1f,
                                         r.getY() + 1.4f, 2.2f, r.getHeight() * 0.44f, 0.5f);
            return;
        }

        juce::Path speaker;
        speaker.startNewSubPath (r.getX() + 1.0f, r.getY() + 5.5f);
        speaker.lineTo (r.getX() + 5.0f, r.getY() + 5.5f);
        speaker.lineTo (r.getX() + 9.0f, r.getY() + 2.0f);
        speaker.lineTo (r.getX() + 9.0f, r.getBottom() - 2.0f);
        speaker.lineTo (r.getX() + 5.0f, r.getBottom() - 5.5f);
        speaker.lineTo (r.getX() + 1.0f, r.getBottom() - 5.5f);
        speaker.closeSubPath();
        const juce::PathStrokeType stroke (1.25f, juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
        g.strokePath (speaker, stroke);
        juce::Path waves;
        waves.startNewSubPath (r.getX() + 12.0f, r.getY() + 5.0f);
        waves.quadraticTo (r.getX() + 15.0f, r.getCentreY(), r.getX() + 12.0f, r.getBottom() - 5.0f);
        waves.startNewSubPath (r.getX() + 15.0f, r.getY() + 2.0f);
        waves.quadraticTo (r.getX() + 20.0f, r.getCentreY(), r.getX() + 15.0f, r.getBottom() - 2.0f);
        g.strokePath (waves, stroke);
    }

    static juce::String shortControllerName (juce::String name)
    {
        if (name.containsIgnoreCase ("Automated CC"))
            return "Automated CC";

        const int launchkey = name.indexOfIgnoreCase ("Launchkey");
        if (launchkey >= 0)
            return name.substring (launchkey).upToFirstOccurrenceOf ("  ", false, false);

        if (name.containsIgnoreCase ("loopMIDI"))
            return "loopMIDI";

        return name.length() <= 16
            ? name
            : name.substring (0, 15)
                + juce::String::charToString ((juce::juce_wchar) 0x2026);
    }

    static void drawChip (juce::Graphics& g, juce::Rectangle<float> chip,
                          const juce::String& tag, const juce::String& name,
                          bool connected, Glyph glyph, bool activity,
                          int nameLines = 1, bool integrated = false)
    {
        // HIERARQUIA: estes chips sao informacao de contexto, nao acao. Tinham
        // chapa, borda e brilho iguais aos dos botoes e competiam com eles no
        // topo. Agora sao quase transparentes -- so uma sombra de contorno.
        juce::ignoreUnused (integrated);
        chip = chip.reduced (0.5f, 1.0f);
        g.setColour (NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (chip, 7.0f);
        g.setColour (NordLookAndFeel::borderSoft());
        g.drawRoundedRectangle (chip, 7.0f, 1.0f);

        auto inner = chip.reduced (8.0f, 5.0f);

        // ponto de status + mini-icone
        auto iconCol = inner.removeFromLeft (20.0f);
        const auto accent = activity  ? NordLookAndFeel::greenOk().brighter (0.25f)
                          : connected ? juce::Colour (0xff5fd08a)
                                      : juce::Colour (0xff6a6d75);

        // halo + ponto
        auto dot = juce::Rectangle<float> (iconCol.getX(), iconCol.getCentreY() - 9.0f, 7.0f, 7.0f);
        g.setColour (accent.withAlpha (0.25f));
        g.fillEllipse (dot.expanded (2.5f));
        g.setColour (accent);
        g.fillEllipse (dot);

        // icone vetorial (teclas / alto-falante) logo abaixo do ponto
        auto gi = juce::Rectangle<float> (iconCol.getX() - 1.0f, iconCol.getCentreY() + 1.0f, 13.0f, 11.0f);
        g.setColour (juce::Colours::white.withAlpha (connected ? 0.55f : 0.30f));
        if (glyph == Glyph::keys)   drawKeysIcon (g, gi);
        else                        drawSpeakerIcon (g, gi);

        // textos: tag pequena em cima, nome do aparelho embaixo.
        // A tag era VERMELHA de 8.5px dentro de um chip escuro — praticamente
        // ilegivel. Cinza claro com espacamento resolve e ainda tira vermelho
        // de um lugar onde ele nao significava nada.
        g.setColour (juce::Colours::white.withAlpha (0.68f));
        g.setFont (juce::Font (juce::FontOptions (9.2f)).boldened()
                       .withExtraKerningFactor (0.10f));
        g.drawText (tag, inner.removeFromTop (10.0f).toNearestInt(),
                    juce::Justification::bottomLeft, false);

        // nome do aparelho: peso normal, nao negrito. Antes gritava tanto quanto
        // o rotulo de um botao de acao.
        g.setColour (connected ? juce::Colours::white.withAlpha (0.92f)
                               : juce::Colours::white.withAlpha (0.34f));
        g.setFont (juce::Font (juce::FontOptions (
            nameLines > 1 ? 10.5f : 11.6f, juce::Font::bold)));
        g.drawFittedText (name, inner.toNearestInt(), juce::Justification::centredLeft,
                          nameLines, 0.68f);
    }

    void updateTooltip()
    {
        juce::String midiLine = juce::String ("TECLADO");
        if (controllerTransport.isNotEmpty())
            midiLine << " (" << controllerTransport << ")";
        midiLine << ": " << (controller.isNotEmpty() ? controller : juce::String ("Nenhum"));
        midiLine << (ctrlOn ? " — conectado" : juce::String::fromUTF8 (" — n\xc3\xa3o conectado"));
        if (mappingCount > 0)
            midiLine << " | " << mappingCount << (mappingCount == 1 ? " mapa MIDI" : " mapas MIDI");

        juce::String audioLine = juce::String::fromUTF8 ("\xc3\x81UDIO");
        if (interfaceTransport.isNotEmpty())
            audioLine << " (" << interfaceTransport << ")";
        audioLine << ": " << (interfaceName.isNotEmpty() ? interfaceName
                                                         : juce::String ("Nenhum"));
        audioLine << (ifaceOn ? " — conectado" : juce::String::fromUTF8 (" — n\xc3\xa3o conectado"));

        setTooltip (midiLine + "\n" + audioLine);
    }

    static void drawKeysIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.drawRoundedRectangle (r, 1.6f, 1.0f);
        const float x0 = r.getX(), w = r.getWidth(), top = r.getY(), h = r.getHeight();
        for (float f : { 0.33f, 0.66f })
            g.drawLine (x0 + w * f, top, x0 + w * f, top + h, 1.0f);
        // "teclas pretas" (mantem a cor atual definida pelo chamador)
        for (float f : { 0.22f, 0.5f, 0.78f })
            g.fillRect (juce::Rectangle<float> (x0 + w * f - 1.0f, top, 2.0f, h * 0.55f));
    }

    static void drawSpeakerIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
        juce::Path p;
        p.startNewSubPath (x, y + h * 0.32f);
        p.lineTo (x + w * 0.28f, y + h * 0.32f);
        p.lineTo (x + w * 0.52f, y + h * 0.10f);
        p.lineTo (x + w * 0.52f, y + h * 0.90f);
        p.lineTo (x + w * 0.28f, y + h * 0.68f);
        p.lineTo (x, y + h * 0.68f);
        p.closeSubPath();
        g.fillPath (p);
        // ondas de som
        g.drawLine (x + w * 0.66f, y + h * 0.32f, x + w * 0.66f, y + h * 0.68f, 1.0f);
        g.drawLine (x + w * 0.82f, y + h * 0.18f, x + w * 0.82f, y + h * 0.82f, 1.0f);
    }

    juce::String controller, interfaceName;
    juce::String controllerTransport, interfaceTransport;
    bool ctrlOn = false, ifaceOn = false;
    bool compact = false, midiActivity = false, integratedRack = false;
    int mappingCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceInfoBar)
};
