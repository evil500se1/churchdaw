#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include "PadProcessor.h"
#include "MidiLearnTargetTag.h"
#include "MidiMappingSafety.h"

#include <array>
#include <cmath>

//==============================================================================
// Painel PREMIUM do Pad Continuo — EMBUTIDO na DAW (nao e janela separada): um
// card flutuante com sombra que aparece por cima do mixer.
//
// GRADE 3x4 (pedido do dono, 05/08/2026), nesta ordem:
//
//      C    C#   D
//      D#   E    F
//      F#   G    G#
//      A    A#   B
//
// Antes disto era um teclado de piano desenhado a mao. O teclado prometia um
// instrumento (teclas brancas e pretas, larguras diferentes, pretas por cima
// das brancas) quando o que existe aqui e um SELETOR de 12 tons: os cinco
// sustenidos ficavam com 42x62 px contra 66x100 das naturais, sem motivo
// nenhum — todos os doze fazem exatamente a mesma coisa. Na grade cada tom
// ocupa uma celula igual, o nome e a unica coisa que precisa ser lida a dois
// metros, e o resto (a tecla FISICA que comanda o tom e a tecla do computador)
// so aparece no hover ou no modo de configuracao.
//
// Sustenidos continuam distinguiveis, mas por SUPERFICIE mais escura — nao por
// imitacao de tecla preta.
//==============================================================================
class PadPanel : public juce::Component,
                 private juce::Timer
{
public:
    // TAMANHO PRETENDIDO DO CARD.
    //
    // Publico de proposito: quem posiciona o card (MainComponent::resized) le
    // estas constantes em vez dos 560x318 cravados que herdamos do teclado
    // antigo. Neste tamanho cada celula da grade fica com ~59 px de altura.
    //
    // Se a janela nao comportar, quem posiciona corta com jmin e o layout
    // abaixo se adapta sozinho: o visor cede a altura primeiro e a grade
    // segura o piso de NordLookAndFeel::hitTouch por celula.
    static constexpr int larguraPreferida = 640;
    static constexpr int alturaPreferida  = 470;

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

        if (pad != nullptr)
            for (auto* parameter : pad->getParameters())
            {
                const auto id = ChurchDAWMidiMapping::stableParameterId (
                    parameter);
                if (id == "padVolume")
                    ChurchDAWMidiLearnTarget::tag (vol, *pad, *parameter);
                else if (id == "padCutoff")
                    ChurchDAWMidiLearnTarget::tag (corte, *pad, *parameter);
            }

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

        // MODO CONFIGURAR: a porta de entrada da configuracao por tom em tela
        // de TOQUE. No desktop o clique direito ja abre o menu do tom, mas
        // clique direito nao existe no tablet do palco — e um toque longo
        // dentro de um card que tambem se arrasta seria ambiguo. Com o modo
        // ligado, tocar num tom ABRE a configuracao dele em vez de tocar; e o
        // proprio botao fica ambar, avisando que os toques mudaram de sentido.
        editBtn.setButtonText ("Editar");
        editBtn.setClickingTogglesState (true);
        editBtn.setColour (juce::TextButton::buttonColourId, padHover());
        editBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xfff59e0b));
       #if CHURCHDAW_MOBILE_SHELL
        editBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff1a1206));
       #else
        editBtn.setColour (juce::TextButton::textColourOnId, juce::Colour (0xffffce80));
       #endif
        editBtn.setTooltip (juce::String::fromUTF8 (
            "Configurar cada tom: aprender MIDI, modo de disparo e tecla do computador"));
        editBtn.onClick = [this]
        {
            // O Learn global manda: com ele armado o visor promete "toque no tom
            // que quer mapear", e ligar o Editar por baixo faria o toque abrir
            // um menu em vez de escolher o alvo — o mapeamento parava de
            // funcionar sem nenhum aviso. O botao ja nasce desabilitado durante
            // o Learn (setMidiLearnMode); isto e o cinto de seguranca.
            if (midiLearnMode)
            {
                editBtn.setToggleState (false, juce::dontSendNotification);
                return;
            }

            modoEdicao = editBtn.getToggleState();
            editBtn.setButtonText (modoEdicao ? "Pronto" : "Editar");

            if (! modoEdicao)
                capturandoTeclaParaTom = -1;

            atualizarFocoDeTeclado();
            repaint();
        };
        addAndMakeVisible (editBtn);

        // O desenho continua compacto, mas uma camada transparente de 44 px
        // recebe o toque ao redor dele. O X antigo de 26x22 era um alvo de
        // mouse em uma tela que tambem roda no palco por toque.
        closeHitTarget.setColour (juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
        closeHitTarget.setColour (juce::TextButton::buttonOnColourId,
                                  juce::Colours::transparentBlack);
        closeHitTarget.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeHitTarget);

        closeBtn.setComponentID ("timbrePickerClose");
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

        current = pad != nullptr ? pad->getKey() : -1;
        stopBtn.setEnabled (current >= 0);

        setSize (larguraPreferida, alturaPreferida);
    }

    ~PadPanel() override
    {
        stopTimer();
    }

    std::function<void()> onClose;
    std::function<void(int)> onMidiLearnTarget;

    // UM CLIQUE SO para "aprender MIDI neste tom".
    //
    // Pelo caminho antigo o aprendizado exige dois gestos separados — ligar
    // "Mapear MIDI" no topo da tela e depois tocar no tom. Quem hospeda o painel
    // liga este callback (MainComponent, junto do onMidiLearnTarget) e o item do
    // menu passa a fazer os dois passos de uma vez. Se ninguem ligar, o item
    // ainda funciona DENTRO do Learn global; fora dele nasce desabilitado, com
    // a linha explicando o que falta.
    std::function<void(int)> onPedirAprenderTom;

    void setMidiLearnMode (bool active)
    {
        midiLearnMode = active;
        stopBtn.setEnabled (active || current >= 0);

        // Dois modos de ensaio ligados ao mesmo tempo dariam dois sentidos ao
        // mesmo toque. O Learn global manda — e enquanto ele estiver armado o
        // Editar fica DESABILITADO, senao o musico o religava por baixo e o
        // toque voltava a abrir menu em vez de escolher o alvo do mapeamento.
        if (active && modoEdicao)
        {
            modoEdicao = false;
            editBtn.setToggleState (false, juce::dontSendNotification);
            editBtn.setButtonText ("Editar");
        }

        editBtn.setEnabled (! active);

        if (active)
            capturandoTeclaParaTom = -1;

        atualizarFocoDeTeclado();
        repaint();
    }

    // O aprendizado de notas entra pelo MIDI Learn GLOBAL do app
    // (onMidiLearnTarget + setMidiLearnMode acima), e nao por um segundo
    // sistema proprio deste painel: e o mesmo gesto que o musico ja usa para
    // mapear qualquer outro controle, o passo 2 aparece no MidiLearnPanel, e
    // o mapa continua num unico arquivo (midi-note-map.xml). O que este painel
    // acrescenta e o que o mapa global nao guarda — modo de disparo e tecla do
    // computador — e isso vive no PadProcessor.

    // ressincroniza o destaque com o motor (ex.: ao reabrir)
    void refreshState()
    {
        const int anterior = current;
        current = pad != nullptr ? pad->getKey() : -1;

        // Mudou por FORA do painel: nota MIDI mapeada, cena, pad do Launchkey.
        // E isso que acende o selo "MIDI" no tom — o musico precisa saber se o
        // pad esta soando porque ele tocou na tela ou porque a tecla do teclado
        // disparou, principalmente quando ele nao lembra de ter tocado nada.
        if (current != anterior)
            origem = current >= 0 ? Origem::midi : Origem::nenhuma;

        stopBtn.setEnabled (midiLearnMode || current >= 0);
        refreshFromProcessor();
        atualizarFocoDeTeclado();
        ajustarTimer();
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
    // Aqui o tom mostra a etiqueta da tecla que o comanda ("C1"), na mesma
    // grade onde ele escolhe o tom.
    //
    // Cada posicao guarda o rotulo das teclas do tom 0..11; a ultima e a de
    // Parar. Vazio = nenhuma tecla atribuida.
    //
    // nota/canal/aparelho sao OPCIONAIS e ficam em branco por padrao: quem
    // preenche o rotulo hoje (MainComponent::atualizarEtiquetasDoPad) ainda nao
    // passa esses tres, e o painel so os mostra quando vierem preenchidos — nao
    // ha campo escrevendo "canal 0" na tela por nao ter o dado.
    struct TeclaMapeada
    {
        juce::String rotulo;
        bool aparelhoPresente = true;   // false = mapeado, mas esse teclado nao esta ligado agora
        int nota = -1;                  // -1 = nao informado
        int canal = 0;                  // 0 = nao informado
        juce::String aparelho;          // vazio = nao informado
    };

    // Um pad de borracha / pedal que manda CC comandando este tom.
    //
    // O painel so precisa saber DUAS coisas: se existe um, e se o musico ja
    // disse que aquele pad tem trava. Nada de tipo tecnico na tela.
    struct BotaoDeCC
    {
        bool existe = false;
        bool comTrava = false;
    };

    void setBotoesDeCC (const std::array<BotaoDeCC, 12>& porTom, BotaoDeCC parar)
    {
        for (int i = 0; i < 12; ++i)
            botoesDeCC[(size_t) i] = porTom[(size_t) i];

        botaoDeCCDeParar = parar;
        repaint();
    }

    // (tom, comTrava). tom < 0 = a acao Parar.
    std::function<void(int, bool)> onTrocarModoDoBotaoDeCC;

    void setTeclasMapeadas (const std::array<TeclaMapeada, 12>& porTom, TeclaMapeada parar)
    {
        for (int i = 0; i < 12; ++i)
            teclas[(size_t) i] = porTom[(size_t) i];

        pararPresente = parar.aparelhoPresente;
        etiquetaDeParar = parar.rotulo;

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

    int getTomAt (juce::Point<float> p) const { return tomEm (p); }
    juce::Rectangle<float> getCellBounds (int tom) const
    {
        if (juce::isPositiveAndBelow (tom, 12))
            return celulas[tom];
        return {};
    }

    void select (int k)
    {
        current = k;

        if (pad != nullptr)
            pad->setKey (k);

        origem = k >= 0 ? Origem::toque : Origem::nenhuma;
        stopBtn.setEnabled (midiLearnMode || k >= 0);
        ajustarTimer();
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

    //==============================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        toFront (true);
        const auto pt = e.position;

        // Clicar no card devolve o foco de teclado quando ha tecla de PC
        // atribuida. Sem isto a tecla parava de responder assim que o musico
        // encostasse em qualquer outra coisa da tela — e sem nenhum sintoma que
        // explicasse o porque.
        if (getWantsKeyboardFocus() && ! hasKeyboardFocus (true))
            grabKeyboardFocus();

       #if ! CHURCHDAW_MOBILE_SHELL
        // arrastar o card pela faixa do topo (titulo) — reposiciona onde quiser
        auto card = getLocalBounds().reduced (compacto ? 8 : 12);
        if (pt.y < (float) (card.getY() + (compacto ? 40 : 46)))
        {
            draggingCard = true;
            dragger.startDraggingComponent (this, e);
            return;
        }
       #endif

        draggingCard = false;

        const int tom = tomEm (pt);

        if (tom < 0)
            return;

        // Clique direito no desktop, modo Editar no toque: os dois abrem a
        // configuracao DAQUELE tom, sem tocar nada.
        //
        // ORDEM IGUAL A DO VISOR (desenharDisplay testa o Learn primeiro): com
        // o Learn armado, TOCAR num tom escolhe o alvo, que e exatamente o que
        // o visor esta prometendo em letras grandes. Testar o Editar antes
        // fazia a tela dizer uma coisa e o toque fazer outra. O clique direito
        // continua abrindo o menu porque e um gesto deliberado de mouse, sem
        // ambiguidade nenhuma com o "toque no tom" do visor.
        if (e.mods.isPopupMenu() || (modoEdicao && ! midiLearnMode))
        {
            mostrarMenuDoTom (tom);
            return;
        }

        // Quem segura o tom no modo "enquanto pressionado" e o DEDO, e so
        // quando o toque vai mesmo tocar (no MIDI Learn ele escolhe alvo).
        if (! midiLearnMode
             && modoDeDisparoDoTom (tom) == PadProcessor::ModoDeDisparo::enquantoPressionado)
        {
            tomSustentadoPorGesto = tom;
            fonteSustentadaPorGesto = e.source.getIndex();
        }

        // RESPOSTA IMEDIATA: o pad afunda antes de qualquer coisa acontecer no
        // motor. Carregar o WAV de um tom novo pode levar blocos de audio; sem
        // este quadro o musico ficava sem retorno nenhum e tocava de novo.
        marcarPressionado (tom);
        handleUserSelection (tom);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingCard)
        {
            toFront (true);
            dragger.dragComponent (this, e, nullptr);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        draggingCard = false;

        // Modo "enquanto pressionado": soltar cala o tom. So cala se ele ainda
        // for o tom que este gesto ligou — no meio tempo uma nota MIDI ou uma
        // cena pode ter trocado o pad, e derrubar isso seria roubar o comando
        // de quem chegou depois.
        if (tomSustentadoPorGesto >= 0
            && (fonteSustentadaPorGesto < 0
                || e.source.getIndex() == fonteSustentadaPorGesto))
        {
            if (current == tomSustentadoPorGesto)
                select (-1);

            tomSustentadoPorGesto = -1;
            fonteSustentadaPorGesto = -1;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { atualizarHover (tomEm (e.position)); }
    void mouseExit (const juce::MouseEvent&)   override { atualizarHover (-1); }

    //==============================================================================
    // TECLA DO COMPUTADOR.
    //
    // O painel so PEDE foco de teclado quando existe ao menos uma tecla
    // atribuida (ver atualizarFocoDeTeclado). Sem nenhuma configurada — que e
    // o estado de todo mundo que atualizar o app — nada muda: o Pad nao rouba
    // o teclado de ninguem. E o keyPressed devolve false para toda tecla que
    // nao seja de um tom, entao os atalhos do resto do app continuam subindo.
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (capturandoTeclaParaTom >= 0)
        {
            const int alvo = capturandoTeclaParaTom;
            capturandoTeclaParaTom = -1;

            if (key.getKeyCode() != juce::KeyPress::escapeKey)
                atribuirTecla (alvo, key.getKeyCode());

            atualizarFocoDeTeclado();
            repaint();
            return true;
        }

        // Dentro do MIDI Learn a tecla do PC nao pode escolher alvo: o alvo se
        // escolhe TOCANDO no tom, e um atalho fazendo isso as escondidas
        // deixaria o proximo note-on que chegasse mapeado no tom errado.
        if (pad == nullptr || midiLearnMode)
            return false;

        const int tom = pad->tomDaTeclaDoComputador (key.getKeyCode());

        if (tom < 0)
            return false;   // nao e nossa: deixa seguir para o resto do app

        // AUTO-REPEAT DO SISTEMA NAO PODE REDISPARAR.
        //
        // Segurar a tecla faz o SO mandar keyPressed dezenas de vezes por
        // segundo. No modo "alternar" isso ligaria e desligaria o pad em
        // sequencia; no "enquanto pressionado" reiniciaria o crossfade a cada
        // repeticao. Guardar qual tecla ja esta na mao resolve os dois.
        if (teclaSegurada == key.getKeyCode())
            return true;

        teclaSegurada = key.getKeyCode();
        marcarPressionado (tom);

        if (modoDeDisparoDoTom (tom) == PadProcessor::ModoDeDisparo::enquantoPressionado)
            tomSustentadoPorTecla = tom;

        handleUserSelection (tom);
        origem = Origem::teclaPc;
        return true;
    }

    bool keyStateChanged (bool isKeyDown) override
    {
        juce::ignoreUnused (isKeyDown);

        if (teclaSegurada != 0 && ! juce::KeyPress::isKeyCurrentlyDown (teclaSegurada))
        {
            teclaSegurada = 0;

            if (tomSustentadoPorTecla >= 0)
            {
                if (current == tomSustentadoPorTecla)
                    select (-1);

                tomSustentadoPorTecla = -1;
            }
        }

        // Nunca consome: keyStateChanged e um aviso, e devolver true aqui
        // engoliria a tecla de quem realmente a espera.
        return false;
    }

    // O KEY-UP SO CHEGA COM O FOCO AQUI DENTRO.
    //
    // Se o musico solta a tecla depois de clicar num combo, abrir um alerta ou
    // uma janela de plugin, o keyStateChanged nunca vem: no modo "enquanto
    // pressionado" o tom ficava PRESO soando, e em qualquer modo a tecla virava
    // letra morta (o keyPressed saia por "teclaSegurada == key" sem fazer nada).
    // Perder o foco e, para nos, a mesma coisa que soltar a tecla.
    void focusLost (FocusChangeType) override            { soltarTeclaSeOFocoSaiu(); }
    void focusOfChildComponentChanged (FocusChangeType) override { soltarTeclaSeOFocoSaiu(); }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        auto card = getLocalBounds().toFloat().reduced (compacto ? 8.0f : 12.0f);
        const float rad = 14.0f;

        g.setColour (desktop ? juce::Colour (0xff1b1f26) : NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (card, rad);
        g.setColour (desktop ? juce::Colour (0xff3b414b) : NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (card.reduced (0.5f), rad, 1.0f);

        // titulo
        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (compacto ? 13.0f : 15.0f)).boldened()
                       .withExtraKerningFactor (desktop ? 0.025f : 0.12f));
        g.drawText (juce::String::fromUTF8 ("PAD CONT\xc3\x8dNUO"), titleBounds, juce::Justification::centred);

        // alca de arrastar (grip) no topo-esquerdo — indica que o card move
        {
            const float gx = card.getX() + 20.0f, gy = card.getY() + 16.0f;
            g.setColour (NordLookAndFeel::dimText().withAlpha (0.45f));
            for (int rr = 0; rr < 2; ++rr)
                for (int cc = 0; cc < 3; ++cc)
                    g.fillEllipse (gx + (float) cc * 5.0f, gy + (float) rr * 5.0f, 2.2f, 2.2f);
        }

        desenharDisplay (g);

        for (int t = 0; t < 12; ++t)
            desenharPad (g, t);
    }

    void resized() override
    {
        // A grade precisa de mais altura que o teclado antigo, entao o limiar de
        // "apertado" subiu junto. No tamanho preferido (640x470) o painel usa o
        // ramo folgado; numa janela que nao comporte isso ele cai no compacto e
        // continua inteiro, cedendo primeiro o visor e so depois a celula.
        compacto = getWidth() < 470 || getHeight() < 360;

        auto card = getLocalBounds().reduced (compacto ? 8 : 12);
        const auto closeHit = juce::Rectangle<int> (card.getRight() - 44,
                                                    card.getY(), 44, 44);
        closeHitTarget.setBounds (closeHit);
        closeBtn.setBounds (closeHit.withSizeKeepingCentre (26, 22));
        closeBtn.toFront (false);

        auto area = card.reduced (compacto ? 12 : 18, compacto ? 10 : 14);
        titleBounds = area.removeFromTop (compacto ? 18 : 22);
        area.removeFromTop (compacto ? 4 : 8);

        // RODAPE PRIMEIRO, ancorado embaixo: 44 px reais de alvo para Parar,
        // Editar e os dois knobs.
        auto bottom = area.removeFromBottom (compacto ? 48 : 54);
        area.removeFromBottom (compacto ? 8 : 12);

        auto volArea = bottom.removeFromLeft (compacto ? 66 : 82);
        volLbl.setBounds (volArea.removeFromBottom (12));
        vol.setBounds (volArea.withSizeKeepingCentre (44, 44));

        // O corte fica ao lado do volume: sao os dois controles continuos do
        // pad, e o musico procura os dois no mesmo canto.
        auto corteArea = bottom.removeFromLeft (compacto ? 76 : 92);
        corteLbl.setBounds (corteArea.removeFromBottom (12));
        corte.setBounds (corteArea.withSizeKeepingCentre (44, 44));

        // Parar encostado na direita (fim da linha = acao final), Editar logo a
        // esquerda dele. Os jmax evitam largura negativa numa janela minuscula.
        const int larguraStop = juce::jmin (compacto ? 146 : 168,
                                            juce::jmax (0, bottom.getWidth() - 50));
        stopBtn.setBounds (bottom.removeFromRight (larguraStop)
                                 .withSizeKeepingCentre (juce::jmax (1, larguraStop), 44));
        bottom.removeFromRight (6);

        const int larguraEdit = juce::jmin (compacto ? 72 : 84, juce::jmax (0, bottom.getWidth()));
        editBtn.setBounds (bottom.removeFromRight (larguraEdit)
                                 .withSizeKeepingCentre (juce::jmax (1, larguraEdit), 44));

        // O QUE SOBROU SE DIVIDE ENTRE VISOR E GRADE — COM PRIORIDADE PARA A
        // GRADE. O visor e conforto; a grade e o controle. Numa janela baixa o
        // visor encolhe ate sumir e os doze tons continuam inteiros, em vez de
        // um visor bonito com celulas de 12 px.
        const int gapGrade   = compacto ? 5 : 7;
        const int gapDisplay = compacto ? 8 : 12;
        // O PISO DA GRADE E O ALVO DE TOQUE, nao um numero bonito. Com 30 px o
        // painel aceitava celulas de ~35 px de altura — menos que as teclas do
        // teclado antigo e abaixo do hitTouch que o resto do app respeita.
        // Como as linhas ficam a 5 px uma da outra, errar por 5 px ali nao
        // "erra dentro da mesma tecla": acerta OUTRO TOM.
        const int gradeMinima = 4 * NordLookAndFeel::hitTouch + 3 * gapGrade;
        // Um visor de 5 px nao e visor, e uma tira preta roubando altura da
        // grade. Ou ele cabe legivel, ou cede tudo para os doze tons.
        const int displayMinimo = 24;
        const int alturaDisplay = juce::jlimit (0, compacto ? 40 : 52,
                                                area.getHeight() - gradeMinima - gapDisplay);

        if (alturaDisplay >= displayMinimo)
        {
            displayBounds = area.removeFromTop (alturaDisplay).toFloat();
            area.removeFromTop (gapDisplay);
        }
        else
        {
            displayBounds = {};
        }

        // GRADE 3x4: indice = tom, entao a ordem sai C C# D / D# E F / F# G G# /
        // A A# B sem nenhuma tabela de traducao.
        auto grade = area.toFloat();
        const float cw = (grade.getWidth()  - (float) gapGrade * 2.0f) / 3.0f;
        const float ch = (grade.getHeight() - (float) gapGrade * 3.0f) / 4.0f;

        for (int t = 0; t < 12; ++t)
        {
            const int coluna = t % 3;
            const int linha  = t / 3;
            celulas[t] = juce::Rectangle<float> (
                grade.getX() + (float) coluna * (cw + (float) gapGrade),
                grade.getY() + (float) linha  * (ch + (float) gapGrade),
                juce::jmax (0.0f, cw), juce::jmax (0.0f, ch));
        }
    }

private:
    //==============================================================================
    // PALETA DA GRADE (valores exatos definidos pelo dono, 05/08/2026).
    //
    // O CHROME do card (borda, filete do topo, titulo, visor) continua usando o
    // NordLookAndFeel, para o card seguir igual ao resto do app. So a grade usa
    // esta paleta — e onde o dono especificou cada valor.
    static juce::Colour padSurface()      { return juce::Colour (0xff16181f); }   // naturais
    static juce::Colour padSurfaceAlt()   { return juce::Colour (0xff111319); }   // sustenidos (mais escuro)
    static juce::Colour padSurfaceDeep()  { return juce::Colour (0xff0d0f14); }
    static juce::Colour padHover()        { return juce::Colour (0xff222630); }
    static juce::Colour padHoverAlt()     { return juce::Colour (0xff1c1f27); }
    static juce::Colour padAccent()       { return juce::Colour (0xffe5484d); }
    static juce::Colour padAccentBright() { return juce::Colour (0xfff43f5e); }
    static juce::Colour padText()         { return juce::Colour (0xfff3f4f6); }
    static juce::Colour padTextMuted()    { return juce::Colour (0xff9ca3af); }
    static juce::Colour padOk()           { return juce::Colour (0xff22c55e); }

    // Sustenidos: superficie mais escura, NAO tecla preta. Todos os doze tons
    // tem o mesmo tamanho e o mesmo peso — porque fazem a mesma coisa.
    static bool ehSustenido (int tom)
    {
        return tom == 1 || tom == 3 || tom == 6 || tom == 8 || tom == 10;
    }

    enum class Origem { nenhuma, toque, teclaPc, midi };

    enum ItemDoMenu
    {
        kInfo              = 900,   // linhas so de leitura (sempre desabilitadas)
        kAprender          = 1,
        kAtribuirTecla     = 2,
        kLimparTecla       = 3,
        kTestar            = 4,
        kDisparoAlternar   = 10,
        kDisparoSustentar  = 11,
        kDisparoMomentaneo = 12,

        // Conserto do pad de borracha / pedal que manda CC.
        kBotaoAlterna      = 20,
        kBotaoComTrava     = 21
    };

    //==============================================================================
    void handleUserSelection (int key)
    {
        if (midiLearnMode && onMidiLearnTarget)
        {
            onMidiLearnTarget (key);
            return;
        }

        if (key < 0)
        {
            select (-1);
            return;
        }

        switch (modoDeDisparoDoTom (key))
        {
            case PadProcessor::ModoDeDisparo::sustentar:
                // Idempotente, igual ao que a nota MIDI mapeada ja faz: apertar
                // de novo CONFIRMA o tom em vez de desligar. Quem escolhe este
                // modo quer poder conferir o tom no meio do louvor sem risco.
                select (key);
                break;

            case PadProcessor::ModoDeDisparo::enquantoPressionado:
                // Quem esta segurando (dedo ou tecla do PC) e anotado por QUEM
                // CHAMOU, nao aqui. Anotar o gesto de mouse neste ponto fazia a
                // tecla do computador registrar-se como se fosse o dedo, e o
                // proximo soltar de botao do mouse — em qualquer lugar do card —
                // calava o tom que a tecla estava segurando.
                select (key);
                break;

            case PadProcessor::ModoDeDisparo::alternar:
            default:
                // PADRAO. E exatamente o que o clique na tela sempre fez; um
                // padrao diferente mudaria o gesto de todo mundo que so
                // atualizou o app.
                toggle (key);
                break;
        }
    }

    PadProcessor::ModoDeDisparo modoDeDisparoDoTom (int tom) const
    {
        return pad != nullptr ? pad->mapeamentoDoTom (tom).modoDeDisparo
                              : PadProcessor::ModoDeDisparo::alternar;
    }

    int tomEm (juce::Point<float> p) const
    {
        for (int t = 0; t < 12; ++t)
            if (celulas[t].contains (p))
                return t;

        return -1;
    }

    void repintarPad (int t)
    {
        if (juce::isPositiveAndBelow (t, 12) && ! celulas[t].isEmpty())
            repaint (celulas[t].expanded (5.0f).getSmallestIntegerContainer());
    }

    void atualizarHover (int t)
    {
        if (t == tomSobMouse)
            return;

        const int antigo = tomSobMouse;
        tomSobMouse = t;
        repintarPad (antigo);
        repintarPad (t);
    }

    void marcarPressionado (int t)
    {
        tomPressionado = t;

        // <= 120 ms, como o dono pediu: o suficiente para o dedo ver que o
        // toque foi aceito, curto demais para virar animacao de videogame.
        fimDoPressionadoMs = juce::Time::getMillisecondCounter() + 110;
        repintarPad (t);
        ajustarTimer();
    }

    void ajustarTimer()
    {
        // O card fica VIVO a sessao inteira, so escondido. Sem o isVisible aqui,
        // um pad ligado manteria este timer repintando de 50 em 50 ms com o
        // painel fechado — durante o culto inteiro, sem ninguem olhando.
        const bool precisa = isVisible() && (current >= 0 || tomPressionado >= 0);

        if (precisa)
        {
            // O flash curto do toque ainda precisa de 50 ms para apagar em
            // <=120 ms. Depois disso, um pad continuo usa somente 8 quadros/s:
            // o pulso de 2,2 s continua suave, mas deixa de repintar a celula
            // 20 vezes por segundo durante o culto inteiro.
            const int periodoDesejado = tomPressionado >= 0 ? 50 : 125;

            if (! isTimerRunning() || periodoDesejado != periodoDoTimerMs)
            {
                periodoDoTimerMs = periodoDesejado;
                startTimer (periodoDoTimerMs);
            }
        }
        else if (isTimerRunning())
        {
            stopTimer();
            periodoDoTimerMs = 0;
        }
    }

    void visibilityChanged() override
    {
        if (! isVisible())
        {
            // ESTADO DE GESTO NAO SOBREVIVE AO FECHAMENTO DO CARD.
            //
            // Fechar o painel com o dedo (ou a tecla) ainda em cima de um pad
            // no modo "enquanto pressionado" nao entrega o mouseUp/keyUp: o
            // gesto ficava pendurado e o PROXIMO soltar de botao, ja noutra
            // sessao do card, calava um tom que ninguem tinha pedido para calar.
            // O tom que estiver soando continua soando — quem desliga e Parar.
            tomSustentadoPorGesto = -1;
            tomSustentadoPorTecla = -1;
            teclaSegurada = 0;
            capturandoTeclaParaTom = -1;
            tomPressionado = -1;
            draggingCard = false;
        }

        atualizarHover (-1);   // fechar com o mouse em cima deixava um pad aceso de hover
        ajustarTimer();
    }

    void timerCallback() override
    {
        if (tomPressionado >= 0
             && juce::Time::getMillisecondCounter() >= fimDoPressionadoMs)
        {
            const int t = tomPressionado;
            tomPressionado = -1;
            repintarPad (t);
        }

        // Pulso do tom sustentado. Repinta SO a celula: o pulso nao pode custar
        // o card inteiro (sombra incluida) vinte vezes por segundo num tablet.
        if (current >= 0)
            repintarPad (current);

        ajustarTimer();
    }

    // Fase 0..1 de um ciclo lento e discreto (2,2 s). Lido direto do relogio:
    // sem estado para dessincronizar entre celulas.
    static float faseDoPulso()
    {
        const auto ms = juce::Time::getMillisecondCounter() % 2200;
        return 0.5f + 0.5f * std::sin ((float) ms / 2200.0f
                                        * juce::MathConstants<float>::twoPi);
    }

    //==============================================================================
    void atualizarFocoDeTeclado()
    {
        const bool quer = capturandoTeclaParaTom >= 0
                           || (pad != nullptr && pad->algumaTeclaDoComputadorAtribuida());

        setWantsKeyboardFocus (quer);

        if (! quer)
        {
            teclaSegurada = 0;
            tomSustentadoPorTecla = -1;
        }

        // AQUI NAO SE PEGA O FOCO. Esta funcao roda dentro de refreshState(),
        // que o MainComponent dispara a CADA nota MIDI mapeada executada — o
        // card roubava o teclado no meio do culto e a proxima tecla digitada em
        // qualquer lugar do app virava atalho do pad. O foco so e tomado onde o
        // gesto pede: no clique dentro do card (mouseDown) e no instante em que
        // "Atribuir tecla do computador" e armado (aplicarEscolhaDoMenu).
    }

    // Soltar a tecla = perder o foco. So conta quando o foco saiu do card
    // INTEIRO: se ele foi para um filho (um knob, o Parar), as teclas continuam
    // subindo ate aqui pelo encadeamento normal do JUCE, e limpar a tecla na mao
    // faria o auto-repeat do sistema redisparar o tom. O JUCE grava o novo dono
    // do foco ANTES de avisar quem perdeu, entao esta pergunta e confiavel.
    void soltarTeclaSeOFocoSaiu()
    {
        if (hasKeyboardFocus (true))
            return;

        if (teclaSegurada == 0 && tomSustentadoPorTecla < 0)
            return;

        teclaSegurada = 0;

        if (tomSustentadoPorTecla >= 0)
        {
            // So cala se o tom sustentado ainda for o que esta soando: uma nota
            // MIDI ou uma cena pode ter trocado o pad no meio do caminho, e
            // derrubar isso seria roubar o comando de quem chegou depois.
            if (current == tomSustentadoPorTecla)
                select (-1);

            tomSustentadoPorTecla = -1;
        }

        repaint();
    }

    void atribuirTecla (int tom, int keyCode)
    {
        if (pad == nullptr || ! juce::isPositiveAndBelow (tom, 12) || keyCode == 0)
            return;

        // UMA TECLA, UM TOM. Se a tecla ja comandava outro tom, ela sai de la —
        // senao dois tons responderiam a mesma tecla e o musico so descobriria
        // no palco qual dos dois o codigo escolheu.
        const int anterior = pad->tomDaTeclaDoComputador (keyCode);

        if (anterior >= 0 && anterior != tom)
        {
            auto outro = pad->mapeamentoDoTom (anterior);
            outro.teclaDoComputador = 0;
            pad->setMapeamentoDoTom (anterior, outro);
        }

        auto m = pad->mapeamentoDoTom (tom);
        m.teclaDoComputador = keyCode;
        pad->setMapeamentoDoTom (tom, m);
    }

    juce::String textoDaTeclaDoComputador (int tom) const
    {
        if (pad == nullptr)
            return {};

        const int codigo = pad->mapeamentoDoTom (tom).teclaDoComputador;

        if (codigo == 0)
            return {};

        return "[" + juce::KeyPress (codigo).getTextDescription() + "]";
    }

    //==============================================================================
    void mostrarMenuDoTom (int tom)
    {
        if (pad == nullptr || ! juce::isPositiveAndBelow (tom, 12))
            return;

        const auto m = pad->mapeamentoDoTom (tom);
        const auto& tecla = teclas[(size_t) tom];

        // So o nome do tom. O numero de nota que ficava aqui era derivado de uma
        // "oitava" que o motor nunca leu, e brigava com a tecla fisica real
        // logo abaixo ("tecla atual: C1"): duas notas diferentes para o mesmo
        // tom, na mesma tela, e uma delas inventada.
        juce::PopupMenu menu;
        menu.addSectionHeader (juce::String ("TOM ") + noteName (tom));

        // Passo 1+2 do fluxo pedido: escolher o tom e mandar aprender. Dentro do
        // Learn global isto ja funciona hoje (mesmo caminho do toque no tom);
        // fora dele, so quando quem hospeda o painel ligar onPedirAprenderTom.
        const bool podeAprender = (midiLearnMode && onMidiLearnTarget != nullptr)
                                   || onPedirAprenderTom != nullptr;

        menu.addItem (kAprender,
                      juce::String::fromUTF8 ("Aprender MIDI para este tom\xe2\x80\xa6"),
                      podeAprender);

        if (! podeAprender)
            menu.addItem (kInfo,
                          juce::String::fromUTF8 ("   ligue \"Mapear MIDI\" no topo da tela"),
                          false);

        if (tecla.rotulo.isNotEmpty())
        {
            menu.addItem (kInfo, juce::String::fromUTF8 ("   tecla atual: ") + tecla.rotulo
                                   + (tecla.aparelhoPresente
                                        ? juce::String()
                                        : juce::String::fromUTF8 ("  (teclado desligado)")),
                          false);

            // So aparece quando quem preenche as etiquetas informar os detalhes.
            if (tecla.nota >= 0 || tecla.canal > 0 || tecla.aparelho.isNotEmpty())
            {
                juce::StringArray partes;

                if (tecla.aparelho.isNotEmpty()) partes.add (tecla.aparelho);
                if (tecla.canal > 0)             partes.add ("canal " + juce::String (tecla.canal));
                if (tecla.nota >= 0)             partes.add ("nota " + juce::String (tecla.nota));

                menu.addItem (kInfo, "   " + partes.joinIntoString (juce::String::fromUTF8 (" \xe2\x80\xa2 ")), false);
            }

            menu.addItem (kInfo,
                          juce::String::fromUTF8 ("   para remover: no Mapear MIDI, toque a tecla de novo"),
                          false);
        }

        menu.addSeparator();

        // ATE ONDE ESTE AJUSTE ALCANCA — dito no proprio menu.
        //
        // O modo de disparo e lido no GESTO: o toque na tela e a tecla do
        // computador (PadPanel::mouseDown / keyPressed). A nota MIDI mapeada
        // continua SELECIONANDO o tom, porque quem a executa e
        // MainComponent::executarAcaoDeNota e o app nao guarda o par
        // note-on/note-off do Pad. Sem esse par, "enquanto pressionado" pela
        // tecla do teclado ficaria preso soando ate alguem apertar Parar — e
        // como o cabecalho deste menu fala da tecla MIDI, era exatamente essa a
        // leitura que o musico fazia. Melhor dizer do que deixar descobrir no
        // culto; ligar o modo tambem no caminho MIDI e trabalho a parte.
        juce::PopupMenu disparo;
        disparo.addItem (kInfo, juce::String::fromUTF8 ("vale para o toque e a tecla do computador"), false);
        disparo.addItem (kInfo, juce::String::fromUTF8 ("a tecla MIDI sempre SELECIONA o tom"), false);
        disparo.addSeparator();
        disparo.addItem (kDisparoAlternar, juce::String::fromUTF8 ("Alternar  (liga / desliga)"),
                         true, m.modoDeDisparo == PadProcessor::ModoDeDisparo::alternar);
        disparo.addItem (kDisparoSustentar, juce::String::fromUTF8 ("Sustentar  (s\xc3\xb3 Parar desliga)"),
                         true, m.modoDeDisparo == PadProcessor::ModoDeDisparo::sustentar);
        disparo.addItem (kDisparoMomentaneo, "Enquanto pressionado",
                         true, m.modoDeDisparo == PadProcessor::ModoDeDisparo::enquantoPressionado);
        menu.addSubMenu (juce::String::fromUTF8 ("Disparo  (toque / tecla do PC)"), disparo);

        // O PAD DE BORRACHA / PEDAL QUE MANDA CC.
        //
        // O NOME DO SUBMENU E UM PEDIDO DE SOCORRO, NAO UM TERMO TECNICO. O
        // cliente e musico: ele nunca vai procurar "modo latching" nem "borda de
        // subida". Ele vai procurar onde reclamar que o pad esta esquisito. As
        // duas opcoes tambem sao descritas pelo SINTOMA, nao pelo mecanismo.
        //
        // So aparece quando existe um pad/pedal de CC ligado a este tom: um item
        // permanente aqui viraria mais uma coisa para o musico configurar errado.
        const auto& botao = botoesDeCC[(size_t) tom];

        if (botao.existe && onTrocarModoDoBotaoDeCC != nullptr)
        {
            juce::PopupMenu conserto;
            conserto.addItem (kInfo,
                              juce::String::fromUTF8 ("vale para o pad / pedal que voc\xc3\xaa mapeou"),
                              false);
            conserto.addSeparator();
            conserto.addItem (kBotaoAlterna,
                              juce::String::fromUTF8 ("Aperta liga, aperta desliga  (normal)"),
                              true, ! botao.comTrava);
            conserto.addItem (kBotaoComTrava,
                              juce::String::fromUTF8 ("Meu pad tem trava (fica aceso sozinho)"),
                              true, botao.comTrava);
            menu.addSubMenu (
                juce::String::fromUTF8 ("Se o pad n\xc3\xa3o estiver funcionando\xe2\x80\xa6"),
                conserto);
        }

        // SUBMENU "OITAVA": REMOVIDO DE PROPOSITO.
        //
        // Ele oferecia C1..C5 com marca de selecao, mas nada no motor lia esse
        // valor: producesMidi() e false, o no do grafo so tem barramento de
        // audio e a afinacao continua fixa em 2^(kSemis/12). O operador escolhia
        // C5 esperando o pad uma oitava acima e o som nao mudava. Pior: o numero
        // que ele alimentava na tela ("60 / C3") contradizia a tecla FISICA
        // mostrada logo acima no mesmo pad — a segunda fonte de verdade que o
        // comentario de PadProcessor::MapeamentoDeTom diz que nao pode existir.
        // Os campos oitava/notaDeSaida continuam no PadProcessor so para um
        // arquivo ja gravado continuar carregando; quem for transpor de verdade
        // tem de ligar isso ao rate do processBlock ANTES de reexpor o menu.

        menu.addSeparator();
        menu.addItem (kAtribuirTecla, juce::String::fromUTF8 ("Atribuir tecla do computador\xe2\x80\xa6"));
        menu.addItem (kLimparTecla,
                      m.teclaDoComputador != 0
                        ? juce::String::fromUTF8 ("Remover tecla ")
                            + juce::KeyPress (m.teclaDoComputador).getTextDescription()
                        : juce::String::fromUTF8 ("Remover tecla do computador"),
                      m.teclaDoComputador != 0);

        menu.addSeparator();
        menu.addItem (kTestar, "Tocar este tom agora");

        // withTargetScreenArea DEPOIS de withTargetComponent: o proprio JUCE
        // avisa que o primeiro tambem define a area, entao a ordem inversa
        // abriria o menu no canto do card em vez de junto da celula tocada.
        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withTargetComponent (this)
                .withTargetScreenArea (localAreaToGlobal (celulas[tom].getSmallestIntegerContainer()))
                .withMinimumWidth (compacto ? 210 : 240),
            [ref = juce::Component::SafePointer<PadPanel> (this), tom] (int escolha)
            {
                if (auto* self = ref.getComponent())
                    self->aplicarEscolhaDoMenu (tom, escolha);
            });
    }

    void aplicarEscolhaDoMenu (int tom, int escolha)
    {
        if (pad == nullptr || escolha == 0 || ! juce::isPositiveAndBelow (tom, 12))
            return;

        auto m = pad->mapeamentoDoTom (tom);

        if (escolha == kAprender)
        {
            if (midiLearnMode && onMidiLearnTarget)
                onMidiLearnTarget (tom);
            else if (onPedirAprenderTom)
                onPedirAprenderTom (tom);

            return;
        }

        if (escolha == kAtribuirTecla)
        {
            capturandoTeclaParaTom = tom;
            atualizarFocoDeTeclado();

            // UNICO lugar que toma o teclado: aqui o gesto pede isso em letras
            // grandes no visor ("Aperte a tecla do tom X"). Fora daqui o card
            // nao pega foco de ninguem.
            if (isShowing())
                grabKeyboardFocus();

            repaint();
            return;
        }

        if (escolha == kLimparTecla)
        {
            m.teclaDoComputador = 0;
            pad->setMapeamentoDoTom (tom, m);
            atualizarFocoDeTeclado();
            repaint();
            return;
        }

        if (escolha == kTestar)
        {
            marcarPressionado (tom);
            select (tom);
            return;
        }

        if (escolha == kBotaoAlterna || escolha == kBotaoComTrava)
        {
            if (onTrocarModoDoBotaoDeCC)
                onTrocarModoDoBotaoDeCC (tom, escolha == kBotaoComTrava);

            return;
        }

        switch (escolha)
        {
            case kDisparoAlternar:   m.modoDeDisparo = PadProcessor::ModoDeDisparo::alternar; break;
            case kDisparoSustentar:  m.modoDeDisparo = PadProcessor::ModoDeDisparo::sustentar; break;
            case kDisparoMomentaneo: m.modoDeDisparo = PadProcessor::ModoDeDisparo::enquantoPressionado; break;
            default: return;
        }

        pad->setMapeamentoDoTom (tom, m);
        repaint();
    }

    //==============================================================================
    void desenharDisplay (juce::Graphics& g)
    {
        if (displayBounds.isEmpty())
            return;

        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        auto d = displayBounds;
        if (desktop)
        {
            g.setColour (juce::Colour (0xff151920));
            g.fillRoundedRectangle (d, 8.0f);
            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.drawRoundedRectangle (d.reduced (0.5f), 8.0f, 1.0f);
        }
        else
        {
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (d.translated (0, 1.5f), 9.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0c1512), d.getX(), d.getY(),
                                                 juce::Colour (0xff070a09), d.getX(), d.getBottom(), false));
        g.fillRoundedRectangle (d, 9.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (d.reduced (0.5f), 9.0f, 1.0f);
        }

        const auto dInt = d.toNearestInt();

        // Capturar tecla e o unico estado em que o teclado do PC esta ARMADO.
        // Ele vem primeiro porque e o mais urgente de explicar: sem instrucao,
        // a proxima tecla que o musico encostasse viraria atalho sem aviso.
        if (capturandoTeclaParaTom >= 0)
        {
            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
            g.setColour (juce::Colour (0xfff59e0b));
            g.drawText ("ATRIBUIR TECLA", dInt.reduced (14, 8).removeFromTop (11),
                        juce::Justification::centredLeft);

            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawText (juce::String::fromUTF8 ("Aperte a tecla do tom ")
                          + noteName (capturandoTeclaParaTom)
                          + juce::String::fromUTF8 ("   (ESC cancela)"),
                        dInt.reduced (14, 4), juce::Justification::centred);
            return;
        }

        // Com o MIDI Learn armado o visor vira instrucao: mapear e coisa
        // de ensaio, e o musico precisa LER o que fazer em vez de
        // adivinhar por que a tecla parou de ligar o pad.
        if (midiLearnMode)
        {
            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
            g.setColour (NordLookAndFeel::nordRedBright());
            g.drawText ("MAPEAR NOTAS", dInt.reduced (14, 8).removeFromTop (11),
                        juce::Justification::centredLeft);

            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawText (juce::String::fromUTF8 ("Toque no tom que quer mapear"),
                        dInt.reduced (14, 4), juce::Justification::centred);
            return;
        }

        if (modoEdicao)
        {
            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
            g.setColour (juce::Colour (0xfff59e0b));
            g.drawText ("CONFIGURAR", dInt.reduced (14, 8).removeFromTop (11),
                        juce::Justification::centredLeft);

            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawText (juce::String::fromUTF8 ("Toque num tom para configur\xc3\xa1-lo"),
                        dInt.reduced (14, 4), juce::Justification::centred);
            return;
        }

        if (current >= 0)
        {
            // o rotulo "PAD" repetia o titulo do card logo acima — saiu
            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
            g.setColour (NordLookAndFeel::dimText());
            g.drawText ("TOM", dInt.reduced (14, 8).removeFromTop (11).removeFromLeft (140),
                        juce::Justification::centredLeft);

            g.setFont (juce::Font (juce::FontOptions (compacto ? 24.0f : 30.0f)).boldened());
            if (! desktop)
            {
                g.setColour (padAccent().withAlpha (0.35f));
                g.drawText (noteName (current), dInt.translated (0, 1), juce::Justification::centred);
            }
            g.setColour (padAccentBright());
            g.drawText (noteName (current), dInt, juce::Justification::centred);

            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
            g.setColour (padOk());
            g.drawText (origem == Origem::midi
                          ? juce::String::fromUTF8 ("\xe2\x97\x8f TOCANDO \xe2\x80\xa2 MIDI")
                          : juce::String::fromUTF8 ("\xe2\x97\x8f TOCANDO"),
                        dInt.reduced (14, 8).removeFromBottom (11).removeFromRight (140),
                        juce::Justification::centredRight);
            return;
        }

        // instrucao em vez de "— desligado —": diz o que fazer
        g.setFont (juce::Font (juce::FontOptions (compacto ? 12.0f : 13.5f)));
        g.setColour (NordLookAndFeel::dimText().withAlpha (0.75f));
        g.drawText (juce::String::fromUTF8 ("Escolha o tom da m\xc3\xbasica"),
                    dInt, juce::Justification::centred);
    }

    //==============================================================================
    // UM PAD DA GRADE.
    //
    // NENHUM ESTADO E CODIFICADO SO POR COR. Sob refletor, num tablet inclinado
    // e com o musico enxergando de lado, cor e a primeira coisa que se perde —
    // e ha quem simplesmente nao distinga vermelho de cinza escuro. Entao:
    //   pressionado -> o retangulo ENCOLHE 2% (forma)
    //   sustentado  -> escrito "HOLD" + anel + barra de atividade (texto+forma)
    //   via MIDI    -> escrito "MIDI"
    //   mapeado     -> o rotulo da tecla fisica, escrito
    //   tecla do PC -> o nome da tecla entre colchetes
    // A cor vem por cima disso tudo, como reforco, nunca como unico sinal.
    void desenharPad (juce::Graphics& g, int tom)
    {
        const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
        const auto celula = celulas[tom];

        if (celula.isEmpty())
            return;

        const bool sustentado  = (tom == current);
        const bool pressionado = (tom == tomPressionado);
        const bool sobMouse    = (tom == tomSobMouse) && ! pressionado;
        const bool sustenido   = ehSustenido (tom);
        const bool capturando  = (tom == capturandoTeclaParaTom);
        const bool detalhado   = modoEdicao || midiLearnMode || sobMouse || pressionado || capturando;
        const bool mapeado     = teclas[(size_t) tom].rotulo.isNotEmpty();
        const float pulso      = faseDoPulso();

        // "transform: scale(0.98)": no paint isso e desenhar 2% menor.
        auto r = pressionado ? celula.reduced (celula.getWidth()  * 0.010f,
                                               celula.getHeight() * 0.010f)
                             : celula;
        const float rad = 8.0f;

        if (! desktop && pressionado)
        {
            // glow suave — uma camada de accent com alpha, nao um neon
            g.setColour (padAccentBright().withAlpha (0.26f));
            g.fillRoundedRectangle (celula.expanded (2.5f), rad + 2.5f);
        }
        else if (! desktop)
        {
            // a "elevacao" do hover e esta sombra ficando um pouco mais funda
            g.setColour (juce::Colours::black.withAlpha (sobMouse ? 0.45f : 0.30f));
            g.fillRoundedRectangle (r.translated (0.0f, sobMouse ? 2.2f : 1.4f), rad);
        }

        juce::Colour topo, base;

        if (pressionado)     { topo = padAccentBright();          base = padAccent(); }
        else if (sustentado) { topo = padAccent().darker (0.34f); base = padAccent().darker (0.60f); }
        else if (sobMouse)   { topo = sustenido ? padHoverAlt() : padHover();
                               base = sustenido ? padHoverAlt().darker (0.22f) : padHover().darker (0.20f); }
        else if (sustenido)  { topo = padSurfaceAlt();            base = padSurfaceDeep(); }
        else                 { topo = padSurface();               base = padSurface().darker (0.22f); }

        if (desktop)
            g.setColour (topo.interpolatedWith (base, 0.35f));
        else
            g.setGradientFill (juce::ColourGradient (topo, r.getX(), r.getY(),
                                                     base, r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);

        // ---- contorno por estado ----
        if (pressionado)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.6f);
        }
        else if (sustentado)
        {
            // pulso MUITO discreto: 0,36 -> 0,56 de alpha num anel, ~2,2 s de
            // ciclo. Nada piscando forte; e palco, nao videogame.
            g.setColour (padAccentBright().withAlpha (0.36f + 0.20f * pulso));
            g.drawRoundedRectangle (r.reduced (0.6f), rad, 2.0f);
        }
        else if (sobMouse)
        {
            g.setColour (desktop ? NordLookAndFeel::txtSecondary().withAlpha (0.45f)
                                  : padAccentBright().withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), rad, desktop ? 1.0f : 1.4f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.075f));
            g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

            // "um detalhe fino com o accent": filete vertical na borda esquerda.
            // Mais forte no tom que tem tecla fisica — reforco do rotulo escrito
            // logo acima, nunca a unica pista.
            auto fio = r.reduced (0.0f, r.getHeight() * 0.30f).withWidth (3.0f).translated (2.0f, 0.0f);
            if (! desktop || mapeado)
            {
                g.setColour (padAccent().withAlpha (mapeado ? 0.55f : 0.18f));
                g.fillRoundedRectangle (fio, 1.5f);
            }
        }

        if (capturando)
        {
            g.setColour (juce::Colour (0xfff59e0b).withAlpha (0.85f));
            g.drawRoundedRectangle (r.reduced (2.0f), rad - 1.5f, 1.8f);
        }

        // ---- textos ----
        const auto corNome = (pressionado || sustentado) ? juce::Colours::white : padText();
        const auto corSec  = (pressionado || sustentado) ? juce::Colours::white.withAlpha (0.85f)
                                                         : padTextMuted();

        auto interno = r.reduced (juce::jmin (10.0f, r.getWidth()  * 0.07f),
                                  juce::jmin (6.0f,  r.getHeight() * 0.13f));

        // O nome do tom e a UNICA coisa que precisa ser lida a dois metros.
        const float alturaNome = juce::jlimit (15.0f, 32.0f,
                                               juce::jmin (r.getHeight() * 0.52f,
                                                           r.getWidth()  * 0.30f));

        const auto textoTecla = textoDaTeclaDoComputador (tom);

        // O limiar do desenho completo e o MESMO piso que o resized() garante
        // por celula. Com um numero maior que ele existiria uma faixa de
        // tamanhos em que a celula e grande o bastante para ser tocada mas
        // pequena demais para mostrar o que ela e — e a etiqueta da tecla
        // fisica sumia sem motivo aparente.
        if (r.getHeight() >= (float) NordLookAndFeel::hitTouch)
        {
            auto faixaTopo = interno.removeFromTop (12.0f);
            auto faixaBase = interno.removeFromBottom (12.0f);

            g.setColour (corNome);
            g.setFont (juce::Font (juce::FontOptions (alturaNome, juce::Font::bold)));
            g.drawText (noteName (tom), interno.toNearestInt(), juce::Justification::centred);

            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)).withExtraKerningFactor (0.06f));

            if (mapeado)
            {
                // APARELHO AUSENTE NAO PODE PARECER TECLA VIVA. O mapa guarda a
                // porta; se o musico mapeou no teclado de casa e no domingo
                // levou outro, a tecla nao responde — e um rotulo aceso ali
                // estaria prometendo exatamente o que nao funciona.
                const bool vivo = teclas[(size_t) tom].aparelhoPresente;

                g.setColour (! vivo ? padTextMuted().withAlpha (0.55f)
                                    : (pressionado || sustentado) ? juce::Colours::white.withAlpha (0.92f)
                                                                  : padAccentBright().withAlpha (0.92f));
                g.drawText (detalhado ? "MIDI " + teclas[(size_t) tom].rotulo
                                          + (vivo ? juce::String() : juce::String (" (?)"))
                                      : teclas[(size_t) tom].rotulo,
                            faixaTopo.toNearestInt(), juce::Justification::centredLeft, false);
            }

            if (textoTecla.isNotEmpty())
            {
                g.setColour (corSec);
                g.drawText (textoTecla, faixaTopo.toNearestInt(), juce::Justification::centredRight, false);
            }

            // A NOTA "60 - C3" SAIU DAQUI. Ela vinha da oitava configuravel, que
            // nenhum caminho de audio le, e aparecia a 12 px de distancia da
            // tecla FISICA de verdade ("MIDI C1") na mesma celula: duas notas
            // contraditorias para o mesmo tom. Fica a que comanda o som.

            if (sustentado)
            {
                g.setColour (juce::Colours::white.withAlpha (0.95f));
                g.setFont (NordLookAndFeel::stageLabel (9.0f));
                g.drawText (origem == Origem::midi
                              ? juce::String::fromUTF8 ("HOLD \xe2\x80\xa2 MIDI")
                              : juce::String ("HOLD"),
                            faixaBase.toNearestInt(), juce::Justification::centredRight, false);
            }
        }
        else
        {
            // CELULA BAIXA (janela sem altura para o card inteiro): uma linha
            // so. O nome continua no centro e o resto encosta nas bordas.
            g.setColour (corNome);
            g.setFont (juce::Font (juce::FontOptions (alturaNome, juce::Font::bold)));
            g.drawText (noteName (tom), interno.toNearestInt(), juce::Justification::centred);

            g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));

            if (mapeado)
            {
                const bool vivo = teclas[(size_t) tom].aparelhoPresente;
                g.setColour (! vivo ? padTextMuted().withAlpha (0.55f)
                                    : (pressionado || sustentado) ? juce::Colours::white.withAlpha (0.92f)
                                                                  : padAccentBright().withAlpha (0.90f));
                g.drawText (teclas[(size_t) tom].rotulo, interno.toNearestInt(),
                            juce::Justification::centredLeft, false);
            }

            juce::String direita;

            if (sustentado)                   direita = "HOLD";
            else if (textoTecla.isNotEmpty()) direita = textoTecla;

            if (direita.isNotEmpty())
            {
                g.setColour (sustentado ? juce::Colours::white.withAlpha (0.95f) : corSec);
                g.drawText (direita, interno.toNearestInt(), juce::Justification::centredRight, false);
            }
        }

        // INDICADOR DE ATIVIDADE: filete na aresta de baixo que respira junto
        // com o anel. Fica fora das faixas de texto (que sao recuadas 6 px),
        // entao nao briga com o "HOLD".
        if (sustentado)
        {
            auto barra = r.reduced (rad, 0.0f).removeFromBottom (3.0f).translated (0.0f, -2.0f);

            if (! barra.isEmpty())
            {
                g.setColour (juce::Colours::black.withAlpha (0.30f));
                g.fillRoundedRectangle (barra, 1.5f);
                g.setColour (padOk().withAlpha (0.55f + 0.30f * pulso));
                g.fillRoundedRectangle (barra.withWidth (barra.getWidth() * (0.40f + 0.55f * pulso)), 1.5f);
            }
        }
    }

    //==============================================================================
    PadProcessor* pad;
    juce::Label volLbl;
    juce::TextButton stopBtn, editBtn, closeHitTarget, closeBtn;
    juce::Slider vol;
    juce::Slider corte;
    juce::Label  corteLbl;
    int current = -1;
    int periodoDoTimerMs = 0;

    juce::Rectangle<int> titleBounds;
    juce::Rectangle<float> displayBounds;
    juce::Rectangle<float> celulas[12];   // indice = tom (0 = C .. 11 = B)

    juce::ComponentDragger dragger;
    bool draggingCard = false;
    bool midiLearnMode = false;
    bool modoEdicao = false;
    bool compacto = false;

    // Estado visual e de gesto (tudo message thread).
    Origem origem = Origem::nenhuma;
    int tomSobMouse = -1;
    int tomPressionado = -1;
    juce::uint32 fimDoPressionadoMs = 0;
    int tomSustentadoPorGesto = -1;   // modo "enquanto pressionado", pelo mouse/dedo
    int fonteSustentadaPorGesto = -1; // touch index do dedo que segurou o tom
    int tomSustentadoPorTecla = -1;   // modo "enquanto pressionado", pela tecla do PC
    int teclaSegurada = 0;            // keyCode que este painel considera "na mao"
    int capturandoTeclaParaTom = -1;  // >= 0 = esperando a tecla do computador

    // Tecla fisica atribuida a cada tom (0..11) e a Parar.
    std::array<TeclaMapeada, 12> teclas;
    std::array<BotaoDeCC, 12> botoesDeCC {};
    BotaoDeCC botaoDeCCDeParar;
    juce::String etiquetaDeParar;
    bool pararPresente = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPanel)
};
