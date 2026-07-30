#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"

//==============================================================================
// Guia de MIDI Learn que ocupa temporariamente o visor de programa.
//
// No celular nao existe largura de cabecalho suficiente para uma instrucao
// completa. Esta faixa transforma o mapeamento em um fluxo de dois passos,
// mostra o teclado que esta respondendo e confirma tanto o aprendizado quanto
// o uso do CC depois de salvo.
//==============================================================================
class MidiLearnPanel : public juce::Component
{
public:
    enum class Stage
    {
        chooseTarget,
        moveHardware,
        mapped,
        activity,
        clearConfirmation
    };

    MidiLearnPanel()
    {
        setInterceptsMouseClicks (false, false);
        showChooseTarget();
    }

    void setDevice (const juce::String& name, bool isConnected)
    {
        if (deviceName != name || connected != isConnected)
        {
            deviceName = name;
            connected = isConnected;
            repaint();
        }
    }

    void setMappingCount (int count)
    {
        count = juce::jmax (0, count);

        if (mappingCount != count)
        {
            mappingCount = count;
            repaint();
        }
    }

    void showChooseTarget()
    {
        setContent (Stage::chooseTarget,
                    "PASSO 1",
                    "Toque no controle que voce quer comandar",
                    "Volume, reverb, master ou qualquer knob do app");
    }

    void showMoveHardware (const juce::String& target)
    {
        setContent (Stage::moveHardware,
                    "PASSO 2",
                    target.isNotEmpty() ? target + " selecionado" : "Controle selecionado",
                    "Agora mova um knob ou fader do seu teclado");
    }

    void showMapped (const juce::String& target, int cc)
    {
        setContent (Stage::mapped,
                    "PRONTO",
                    "CC " + juce::String (cc) + " ligado a " + target,
                    "Mapeamento salvo automaticamente");
    }

    void showActivity (const juce::String& target, int cc, int value)
    {
        setContent (Stage::activity,
                    "ATIVO",
                    "CC " + juce::String (cc) + " controlando " + target,
                    "Sinal recebido  " + juce::String (juce::roundToInt (value * 100.0f / 127.0f)) + "%");
    }

    void showPickup (const juce::String& target, int cc)
    {
        setContent (Stage::activity,
                    "PICKUP",
                    "CC " + juce::String (cc) + " pronto para " + target,
                    "Aproxime o controle da posicao atual para evitar salto");
    }

    void showClearConfirmation()
    {
        setContent (Stage::clearConfirmation,
                    "CONFIRMAR",
                    "Apagar todos os mapeamentos MIDI?",
                    "Toque em Confirmar no cabecalho para apagar");
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        if (bounds.isEmpty())
            return;

        const bool success = stage == Stage::mapped || stage == Stage::activity;
        const auto accent = success ? NordLookAndFeel::greenOk()
                                    : NordLookAndFeel::oledAmber();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff181a20),
                                                 bounds.getX(), bounds.getY(),
                                                 juce::Colour (0xff0d0f13),
                                                 bounds.getX(), bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 7.0f, 1.0f);
        g.setColour (accent.withAlpha (0.92f));
        g.fillRoundedRectangle (bounds.removeFromBottom (2.0f), 1.0f);

        auto area = getLocalBounds().reduced (13, 5);
        const int deviceWidth = juce::jlimit (122, 178, juce::roundToInt (getWidth() * 0.21f));
        const int countWidth = getWidth() >= 560 ? 76 : 58;

        auto device = area.removeFromLeft (juce::jmin (deviceWidth, area.getWidth()));
        auto count = area.removeFromRight (juce::jmin (countWidth, area.getWidth()));
        area.reduce (12, 0);

        // Teclado conectado: o usuario sabe qual aparelho deve mover.
        const auto deviceAccent = connected ? NordLookAndFeel::greenOk()
                                            : NordLookAndFeel::redClip();
        auto dot = juce::Rectangle<float> ((float) device.getX(),
                                           (float) device.getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (deviceAccent.withAlpha (0.22f));
        g.fillEllipse (dot.expanded (3.0f));
        g.setColour (deviceAccent);
        g.fillEllipse (dot);

        auto deviceText = device.withTrimmedLeft (17);
        g.setColour (juce::Colours::white.withAlpha (0.48f));
        g.setFont (NordLookAndFeel::stageLabel (8.5f));
        g.drawText (connected ? "MIDI CONECTADO" : "SEM TECLADO",
                    deviceText.removeFromTop (14), juce::Justification::centredLeft, false);
        g.setColour (connected ? juce::Colours::white.withAlpha (0.86f)
                               : juce::Colours::white.withAlpha (0.52f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (connected && deviceName.isNotEmpty() ? deviceName : "Conecte via USB",
                    deviceText, juce::Justification::centredLeft, true);

        if (! area.isEmpty())
        {
            auto badge = area.removeFromLeft (juce::jmin (72, area.getWidth()));
            badge.reduce (0, 6);
            g.setColour (accent.withAlpha (0.14f));
            g.fillRoundedRectangle (badge.toFloat(), 4.0f);
            g.setColour (accent.withAlpha (0.72f));
            g.drawRoundedRectangle (badge.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setColour (accent);
            g.setFont (NordLookAndFeel::stageLabel (9.0f));
            g.drawText (badgeText, badge, juce::Justification::centred, false);

            area.removeFromLeft (11);
            auto titleArea = area.removeFromTop (24);
            g.setColour (NordLookAndFeel::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (14.5f)).boldened());
            g.drawText (title, titleArea, juce::Justification::centredLeft, true);
            g.setColour (NordLookAndFeel::txtSecondary().withAlpha (0.78f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (detail, area, juce::Justification::centredLeft, true);
        }

        if (! count.isEmpty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (count.getX(), count.getY() + 3, 1, count.getHeight() - 6);
            count.removeFromLeft (9);
            auto numberArea = count.removeFromTop (24);
            g.setColour (mappingCount > 0 ? NordLookAndFeel::greenOk()
                                          : NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (17.0f)).boldened());
            g.drawText (juce::String (mappingCount), numberArea,
                        juce::Justification::centred, false);
            g.setColour (juce::Colours::white.withAlpha (0.42f));
            g.setFont (NordLookAndFeel::stageLabel (8.0f));
            g.drawText (mappingCount == 1 ? "MAPA" : "MAPAS", count,
                        juce::Justification::centred, false);
        }
    }

private:
    void setContent (Stage nextStage, juce::String badge,
                     juce::String nextTitle, juce::String nextDetail)
    {
        stage = nextStage;
        badgeText = std::move (badge);
        title = std::move (nextTitle);
        detail = std::move (nextDetail);
        repaint();
    }

    Stage stage = Stage::chooseTarget;
    juce::String badgeText, title, detail, deviceName;
    int mappingCount = 0;
    bool connected = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearnPanel)
};
