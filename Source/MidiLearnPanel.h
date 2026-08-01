#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
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
        clearConfirmation,

        // O mapa existe na sessao mas NAO foi para o disco. Precisa de estagio
        // proprio, e nao de um sufixo no titulo: o selo e o rodape sao lidos
        // antes do titulo, o acento verde e lido antes de tudo, e o titulo e o
        // unico dos tres que o paint corta com reticencias — ou seja, pendurar
        // "NAO SALVOU" no fim dele era esconder o aviso justamente onde ele
        // some primeiro, cercado por dois elementos afirmando o contrario.
        naoSalvou
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

    void showPlayNote (const juce::String& target)
    {
        setContent (Stage::moveHardware,
                    "PASSO 2",
                    target.isNotEmpty() ? target + " selecionado" : "Tom do Pad selecionado",
                    "Agora toque a tecla que vai acionar esse comando");
    }

    void showMapped (const juce::String& target, int cc, bool salvou = true)
    {
        setContent (salvou ? Stage::mapped : Stage::naoSalvou,
                    salvou ? "PRONTO" : juce::String::fromUTF8 ("N\xc3\x83O SALVOU"),
                    "CC " + juce::String (cc) + " ligado a " + target,
                    salvou ? juce::String ("Mapeamento salvo automaticamente")
                           : juce::String::fromUTF8 (
                                 "Vale s\xc3\xb3 nesta sess\xc3\xa3o \xe2\x80\x94 vai sumir ao fechar"));
    }

    void showMappedNote (const juce::String& target, int note, bool salvou = true)
    {
        setContent (salvou ? Stage::mapped : Stage::naoSalvou,
                    salvou ? "PRONTO" : juce::String::fromUTF8 ("N\xc3\x83O SALVOU"),
                    "Nota " + juce::MidiMessage::getMidiNoteName (note, true, true, 3)
                        + " ligada a " + target,
                    salvou ? juce::String ("Mapeamento salvo automaticamente")
                           : juce::String::fromUTF8 (
                                 "Vale s\xc3\xb3 nesta sess\xc3\xa3o \xe2\x80\x94 vai sumir ao fechar"));
    }

    // Recado livre com cara de ALERTA (usado pelo "Limpar mapas" quando o
    // disco recusa). Antes isso passava por showMapped e a tela dizia
    // "CC 0 ligado a limpou na sessao..." — CC 0 e um controle de verdade
    // (Bank Select), entao o musico lia que tinha acabado de mapea-lo.
    void showAviso (const juce::String& titulo, const juce::String& rodape)
    {
        setContent (Stage::naoSalvou, juce::String::fromUTF8 ("ATEN\xc3\x87\xc3\x83O"), titulo, rodape);
    }

    // DESMAPEAR precisa de frase propria.
    //
    // Reaproveitar o molde "ligada a {alvo}" produzia "Nota C1 ligada a volta a
    // tocar nota" — a tela contando o oposto do que aconteceu, justamente no
    // gesto que existe para o musico DESFAZER um mapeamento acidental.
    void showUnmappedNote (int note, const juce::String& rodape)
    {
        setContent (Stage::mapped,
                    "LIBERADA",
                    "Nota " + juce::MidiMessage::getMidiNoteName (note, true, true, 3)
                        + " voltou a tocar normalmente",
                    rodape);
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

        // Vermelho para "nao salvou": o olho le a COR antes da palavra, e um
        // aviso de falha pintado no verde de sucesso e pior que nenhum aviso.
        const auto accent = stage == Stage::naoSalvou ? NordLookAndFeel::nordRedBright()
                          : success                   ? NordLookAndFeel::greenOk()
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
        const bool narrow = getWidth() < 560;
        juce::Rectangle<int> device, count;

        // Em portrait/freeform, device + contador consumiam 180 px antes mesmo
        // do badge. Num painel de 360 px sobravam cerca de 47 px para a frase
        // que realmente importa (inclusive "NAO SALVOU"). Nessa largura os
        // dois indicadores secundarios saem; a mensagem conserva o espaco.
        if (! narrow)
        {
            const int deviceWidth = juce::jlimit (122, 178,
                                                   juce::roundToInt (getWidth() * 0.21f));
            device = area.removeFromLeft (juce::jmin (deviceWidth, area.getWidth()));
            count = area.removeFromRight (juce::jmin (76, area.getWidth()));
            area.reduce (12, 0);
        }
        else
        {
            area.reduce (2, 0);
        }

        // Teclado conectado: o usuario sabe qual aparelho deve mover.
        if (! device.isEmpty())
        {
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
        }

        if (! area.isEmpty())
        {
            auto badge = area.removeFromLeft (juce::jmin (narrow ? 80 : 72,
                                                         area.getWidth()));
            badge.reduce (0, 6);
            g.setColour (accent.withAlpha (0.14f));
            g.fillRoundedRectangle (badge.toFloat(), 4.0f);
            g.setColour (accent.withAlpha (0.72f));
            g.drawRoundedRectangle (badge.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setColour (accent);
            g.setFont (NordLookAndFeel::stageLabel (9.0f));
            g.drawText (badgeText, badge, juce::Justification::centred, false);

            area.removeFromLeft (narrow ? 8 : 11);
            auto titleArea = area.removeFromTop (24);
            g.setColour (NordLookAndFeel::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (14.5f)).boldened());
            if (narrow)
                g.drawFittedText (title, titleArea, juce::Justification::centredLeft,
                                  1, 0.78f);
            else
                g.drawText (title, titleArea, juce::Justification::centredLeft, true);
            g.setColour (NordLookAndFeel::txtSecondary().withAlpha (0.78f));
            g.setFont (juce::Font (juce::FontOptions (narrow ? 10.5f : 11.0f)));
            if (narrow)
                g.drawFittedText (detail, area, juce::Justification::centredLeft,
                                  1, 0.78f);
            else
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
