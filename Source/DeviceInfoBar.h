#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"

//==============================================================================
// Barra premium do cabecalho que mostra, em dois "chips" escuros e elegantes,
// o CONTROLADOR MIDI e a INTERFACE de audio conectados (com ponto de status
// verde/cinza e um mini-icone vetorial). Substitui o antigo botao de entrada
// de guitarra/mic. So exibe informacao — nada de clicar.
//==============================================================================
class DeviceInfoBar : public juce::Component
{
public:
    DeviceInfoBar()
    {
        setInterceptsMouseClicks (false, false);
    }

    void setControllerName (const juce::String& name, bool connected)
    {
        if (controller != name || ctrlOn != connected)
        {
            controller = name;
            ctrlOn = connected;
            repaint();
        }
    }

    void setInterfaceName (const juce::String& name, bool connected)
    {
        if (interfaceName != name || ifaceOn != connected)
        {
            interfaceName = name;
            ifaceOn = connected;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        const int gap = 8;
        // nome do controlador costuma ser mais longo → ganha mais largura
        const int chipW = juce::roundToInt ((area.getWidth() - gap) * 0.57f);

        // Tags CURTAS: "CONTROLADOR"/"INTERFACE" nao cabiam e saiam cortadas
        // ("CONTROLADO", "INTERFA"), e ainda roubavam largura do nome do aparelho,
        // que e a informacao que importa.
        drawChip (g, area.removeFromLeft (chipW).toFloat(),
                  juce::String::fromUTF8 ("TECLADO"),
                  controller.isNotEmpty() ? controller : juce::String ("Nenhum"),
                  ctrlOn, Glyph::keys);

        area.removeFromLeft (gap);

        drawChip (g, area.toFloat(),
                  juce::String::fromUTF8 ("\xc3\x81UDIO"),   // ÁUDIO
                  interfaceName.isNotEmpty() ? interfaceName : juce::String ("Nenhum"),
                  ifaceOn, Glyph::speaker);
    }

private:
    enum class Glyph { keys, speaker };

    static void drawChip (juce::Graphics& g, juce::Rectangle<float> chip,
                          const juce::String& tag, const juce::String& name,
                          bool connected, Glyph glyph)
    {
        // HIERARQUIA: estes chips sao informacao de contexto, nao acao. Tinham
        // chapa, borda e brilho iguais aos dos botoes e competiam com eles no
        // topo. Agora sao quase transparentes -- so uma sombra de contorno.
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (chip, NordLookAndFeel::radiusControl);
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.drawRoundedRectangle (chip.reduced (0.5f), NordLookAndFeel::radiusControl, 1.0f);

        auto inner = chip.reduced (8.0f, 5.0f);

        // ponto de status + mini-icone
        auto iconCol = inner.removeFromLeft (20.0f);
        const auto accent = connected ? juce::Colour (0xff5fd08a) : juce::Colour (0xff6a6d75);

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
        g.setColour (NordLookAndFeel::dimText().withAlpha (0.50f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened().withExtraKerningFactor (0.10f));
        g.drawText (tag, inner.removeFromTop (10.0f).toNearestInt(),
                    juce::Justification::bottomLeft, false);

        // nome do aparelho: peso normal, nao negrito. Antes gritava tanto quanto
        // o rotulo de um botao de acao.
        g.setColour (connected ? juce::Colours::white.withAlpha (0.72f)
                               : juce::Colours::white.withAlpha (0.34f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (name, inner.toNearestInt(), juce::Justification::centredLeft, true);
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
    bool ctrlOn = false, ifaceOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceInfoBar)
};
