#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"
#include "LiveScenes.h"

#include <array>

//==============================================================================
// Faixa das 10 cenas do LIVE MODE.
//
// Mora na faixa de programa, o mesmo retangulo onde o visor e o painel de
// MIDI Learn ja se alternam — por isso entra sem mexer no layout do
// cabecalho, que no celular ja esta no limite de largura.
//
// Regra de ouro do palco: UM TOQUE NUNCA GRAVA.
// Carregar cena e o gesto do culto e tem de ser imediato; gravar e gesto de
// ensaio e exige armar o botao GRAVAR antes. Sem isso, um toque errado no
// escuro apagaria a cena da proxima musica.
//==============================================================================
class LiveScenePanel : public juce::Component,
                       private juce::Timer
{
public:
    //==========================================================================
    // Botao de cena com TOQUE LONGO.
    //
    // Renomear e apagar nao podem morar num botao proprio: a faixa ja tem onze
    // alvos e no celular nao sobra largura. O toque longo (ou botao direito no
    // computador) resolve sem tirar nada da tela — e, principalmente, sem
    // criar mais um alvo que se pode acertar por engano no escuro.
    //
    // Segurar NAO troca a musica: se o menu abriu, o clique e engolido. Um
    // toque longo que tambem trocasse de cena seria a pior combinacao
    // possivel — o musico abriria o menu e o culto mudaria de som junto.
    //==========================================================================
    class BotaoDeCena : public juce::TextButton,
                        private juce::Timer
    {
    public:
        std::function<void()> aoSegurar;

        void mouseDown (const juce::MouseEvent& e) override
        {
            segurou = false;

            if (e.mods.isPopupMenu())
            {
                disparar();
                return;
            }

            startTimer (550);
            juce::TextButton::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Dedo escorregando nao e toque longo — no tablet a mao treme.
            if (e.getDistanceFromDragStart() > 10)
                stopTimer();

            juce::TextButton::mouseDrag (e);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            stopTimer();

            if (segurou)
            {
                segurou = false;

                // O menu so abre AGORA, com o dedo ja fora da tela.
                //
                // Abrir aos 550 ms, com o dedo parado em cima, punha o menu
                // debaixo do proprio dedo: ao levantar, o PopupMenu lia aquilo
                // como escolha e podia disparar "Apagar cena N" sem que
                // ninguem tivesse escolhido nada.
                if (aoSegurar != nullptr)
                    aoSegurar();

                return;   // e nao troca de musica
            }

            juce::TextButton::mouseUp (e);
        }

    private:
        void timerCallback() override
        {
            stopTimer();
            segurou = true;   // marca; quem abre o menu e o mouseUp
        }

        void disparar()
        {
            segurou = true;

            if (aoSegurar != nullptr)
                aoSegurar();
        }

        bool segurou = false;
    };

    LiveScenePanel()
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            auto* b = botoes.add (new BotaoDeCena());
            b->setWantsKeyboardFocus (false);   // seta do teclado nao troca musica
            b->onClick = [this, i] { acionar (i); };
            b->aoSegurar = [this, i] { mostrarMenu (i); };
            addAndMakeVisible (b);
        }

        gravarBtn.setButtonText ("GRAVAR");
        gravarBtn.setWantsKeyboardFocus (false);
        gravarBtn.setClickingTogglesState (true);
        gravarBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        gravarBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff8f0c21));
        gravarBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::dimText());
        gravarBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        gravarBtn.onClick = [this] { armarOuDesarmar(); };
        addAndMakeVisible (gravarBtn);

        // RENOMEAR SEM DIALOGO.
        //
        // O app compila com JUCE_MODAL_LOOPS_PERMITTED=0 e todo o projeto usa
        // so as APIs assincronas — uma janela modal montada na mao aqui nem
        // apareceria. Alem disso, no tablet um dialogo cobriria justamente a
        // faixa de cenas. Editar dentro do proprio botao resolve os dois: o
        // nome nasce onde vai ser lido.
        editorDeNome.setJustification (juce::Justification::centred);
        editorDeNome.setSelectAllWhenFocused (true);
        editorDeNome.setInputRestrictions (18);
        editorDeNome.setTextToShowWhenEmpty ("Nome da cena",
                                             NordLookAndFeel::dimText().withAlpha (0.78f));
        editorDeNome.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101216));
        editorDeNome.setColour (juce::TextEditor::outlineColourId, NordLookAndFeel::nordRed());
        editorDeNome.setColour (juce::TextEditor::focusedOutlineColourId, NordLookAndFeel::nordRed());
        editorDeNome.onReturnKey = [this] { concluirRenome (true); };
        editorDeNome.onEscapeKey = [this] { concluirRenome (false); };
        editorDeNome.onFocusLost = [this] { concluirRenome (true); };
        addChildComponent (editorDeNome);
    }

    // O painel nao conhece o motor: quem aplica e grava e o MainComponent.
    // Devolvem o resultado porque o painel precisa saber se o toque pegou —
    // e so ele pode contar isso ao musico.
    std::function<ChurchDAWLive::Resultado (int)> onCarregarCena;
    std::function<ChurchDAWLive::Resultado (int)> onGravarCena;

    // Toque longo. O painel abre o menu e edita o nome no proprio botao; quem
    // guarda e persiste e o MainComponent.
    std::function<void (int, juce::String)> onRenomearCena;
    std::function<void (int)> onApagarCena;

    void atualizar (const ChurchDAWLive::Cena* cenas, int ativa)
    {
        for (int i = 0; i < botoes.size(); ++i)
        {
            auto* b = botoes[i];
            const auto& cena = cenas[i];

            preenchida[i] = cena.preenchida;
            nomes[i] = cena.nome;
            b->setButtonText (ChurchDAWLive::rotuloDaCena (cena, i));

            // Tres estados visiveis, e a diferenca importa no escuro:
            // vazia (apagada), gravada (cinza) e a que esta tocando (vermelha).
            const auto cor = i == ativa        ? NordLookAndFeel::nordRed()
                           : cena.preenchida   ? juce::Colour (0xff2f333b)
                                               : juce::Colour (0xff1d1f24);

            b->setColour (juce::TextButton::buttonColourId, cor);
            b->setColour (juce::TextButton::textColourOffId,
                          cena.preenchida ? juce::Colours::white
                                          : NordLookAndFeel::dimText().withAlpha (0.55f));
        }

        repaint();
    }

    bool estaArmadoParaGravar() const { return gravarBtn.getToggleState(); }

    // Recado de palco vindo de fora (ex.: o motor desistiu de esperar o timbre).
    void avisar (juce::String texto) { mostrarAviso (std::move (texto)); }

    // ARMAR NAO ATRAVESSA O FECHAMENTO DO PAINEL.
    //
    // Sem isto, "um toque nunca grava" continuava verdadeiro na letra e falso
    // na pratica: bastava armar no ensaio, fechar o Live (ou entrar no MIDI
    // Learn, que fecha por fora), e horas depois o primeiro toque numa cena —
    // feito para TROCAR de musica — gravava por cima dela. O audio nao mudava
    // nada, entao a perda so apareceria no culto seguinte.
    //
    // Armar e uma intencao de segundos. Se o painel saiu da tela, a intencao
    // morreu junto.
    void visibilityChanged() override
    {
        if (! isVisible())
            desarmar();
    }

    // ARMAR EXPIRA SOZINHO.
    //
    // O visibilityChanged sozinho nao bastava, e o caso que ele NAO cobre e
    // justamente o comum: no palco o Live fica aberto o culto inteiro, e o
    // JUCE nem sequer chama visibilityChanged quando a janela e minimizada ou
    // quando o Android manda o app para segundo plano — o flag do componente
    // continua true. Sem prazo, "armado" atravessava o ensaio, a tela
    // bloqueada e duas horas de espera, e o primeiro toque do culto — feito
    // para TROCAR de musica — gravava por cima dela.
    //
    // Armar e uma intencao de segundos: quem arma vai tocar na cena agora.
    void armarOuDesarmar()
    {
        if (gravarBtn.getToggleState())
            startTimer (9000);
        else
            stopTimer();

        repaint();
    }

    void desarmar()
    {
        stopTimer();

        if (gravarBtn.getToggleState())
            gravarBtn.setToggleState (false, juce::dontSendNotification);

        // Sempre limpa e sempre bump: um aviso deixado para tras reaparecia ao
        // reabrir o painel, contando um erro que ja passou.
        ++geracaoDoAviso;
        aviso.clear();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (alturaDoRotulo <= 0)
            return;   // faixa curta: a altura toda foi para os botoes

        const bool temAviso = aviso.isNotEmpty();

        g.setColour (temAviso ? NordLookAndFeel::nordRed() : NordLookAndFeel::dimText());
        g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened().withExtraKerningFactor (0.22f));
        // SEM toUpperCase: ele nao sobe letra acentuada e a tela saia com
        // "CENA 5 ESTá VAZIA". As mensagens ja vem escritas como devem
        // aparecer (ver avisoDoResultado).
        g.drawText (temAviso
                      ? aviso
                      : (gravarBtn.getToggleState()
                          ? juce::String::fromUTF8 ("LIVE MODE  \xe2\x80\xa2  toque numa cena para GRAVAR por cima")
                          : juce::String::fromUTF8 ("LIVE MODE  \xe2\x80\xa2  toque para trocar de m\xc3\xbasica")),
                    getLocalBounds().reduced (8, 4).removeFromTop (alturaDoRotulo),
                    juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 4);

        // O rotulo so entra se sobrar altura de verdade. Numa faixa curta ele
        // roubava 14 px que ja faltavam e a conta chegava a dar altura
        // NEGATIVA — os botoes simplesmente sumiam da tela.
        alturaDoRotulo = area.getHeight() >= 46 ? 14 : 0;
        area.removeFromTop (alturaDoRotulo);

        if (area.getHeight() <= 0 || area.getWidth() <= 0)
            return;

        // O GRAVAR nunca pode comer a faixa inteira. Numa janela freeform
        // estreita do Android 16 o minimo fixo de 52 px sobrava zero para as
        // dez cenas, e o Live Mode aparecia so com o botao de gravar.
        const int larguraGravar = juce::jlimit (juce::jmin (36, area.getWidth() / 3),
                                                86,
                                                area.getWidth() / 6);

        gravarBtn.setBounds (encolher (area.removeFromRight (larguraGravar)));
        area.removeFromRight (juce::jmin (6, area.getWidth() / 4));

        // DUAS FILEIRAS quando uma so daria alvo de dedo pequeno demais — mas
        // SO se houver altura para isso. Dez botoes de 21 px de largura sao
        // ruins; trocar por quarenta de largura e DEZESSEIS de altura e pior,
        // e foi o que acontecia na faixa de 52 px do retrato. Pior ainda: num
        // botao de 16 px o dedo passa dos 10 px de arrasto antes dos 550 ms, o
        // toque longo morre e o gesto vira clique — tentar renomear TROCAVA a
        // musica.
        const bool larguraApertada = (area.getWidth() / botoes.size()) < 44;
        const bool cabeDuasFileiras = area.getHeight() >= 72;
        const int porFileira = (larguraApertada && cabeDuasFileiras) ? botoes.size() / 2
                                                                     : botoes.size();
        const int fileiras   = juce::jmax (1, botoes.size() / juce::jmax (1, porFileira));
        const int alturaFileira = juce::jmax (1, area.getHeight() / fileiras);

        for (int f = 0; f < fileiras; ++f)
        {
            auto linha = area.removeFromTop (alturaFileira);
            const int largura = juce::jmax (1, linha.getWidth() / juce::jmax (1, porFileira));

            for (int i = 0; i < porFileira; ++i)
            {
                const int indice = f * porFileira + i;

                if (! juce::isPositiveAndBelow (indice, botoes.size()))
                    break;

                const int w = i == porFileira - 1 ? linha.getWidth() : largura;
                botoes[indice]->setBounds (encolher (linha.removeFromLeft (w)));
            }
        }

        // Rotacao, IME e resize freeform podem acontecer com o editor aberto.
        // Ele precisa continuar exatamente em cima do botao que esta nomeando.
        if (juce::isPositiveAndBelow (renomeando, botoes.size())
             && editorDeNome.isVisible())
            editorDeNome.setBounds (botoes[renomeando]->getBounds());
    }

private:
    void timerCallback() override
    {
        stopTimer();

        if (gravarBtn.getToggleState())
        {
            desarmar();
            mostrarAviso (juce::String::fromUTF8 ("GRAVAR DESARMOU SOZINHO"));
        }
    }

    // Recuo proporcional. O recuo fixo de 6 px virava altura negativa numa
    // faixa de 20 px, e um Rectangle de altura negativa nao desenha nada.
    static juce::Rectangle<int> encolher (juce::Rectangle<int> r)
    {
        return r.reduced (juce::jmin (2, r.getWidth()  / 6),
                          juce::jmin (6, r.getHeight() / 6));
    }

    void mostrarMenu (int indice)
    {
        if (! juce::isPositiveAndBelow (indice, (int) preenchida.size()) || ! preenchida[indice])
        {
            // Cena vazia nao tem nome para trocar nem conteudo para apagar. Um
            // menu vazio so ensinaria o musico a desconfiar do toque longo.
            mostrarAviso (ChurchDAWLive::avisoDoResultado (ChurchDAWLive::Resultado::vazia, indice));
            return;
        }

        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 ("Renomear cena\xe2\x80\xa6"));
        m.addItem (2, "Apagar cena " + juce::String (indice + 1));

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (botoes[indice]),
            [ref = juce::Component::SafePointer<LiveScenePanel> (this), indice] (int escolha)
            {
                auto* self = ref.getComponent();

                if (self == nullptr)
                    return;

                if (escolha == 1)
                {
                    // ADIAR o inicio da edicao.
                    //
                    // O callback do menu roda enquanto o PopupMenu ainda esta
                    // se desmontando, e ao sair ele devolve o foco de teclado
                    // para onde estava — roubando de volta o que o editor
                    // acabou de pegar. O efeito era cruel: o campo aparecia,
                    // o musico digitava, e cada letra virava atalho da janela
                    // principal. Um quadro de espera resolve.
                    juce::MessageManager::callAsync ([ref, indice]
                    {
                        if (auto* p = ref.getComponent())
                            p->iniciarRenome (indice);
                    });
                }
                else if (escolha == 2 && self->onApagarCena != nullptr)
                {
                    self->onApagarCena (indice);
                }
            });
    }

    void iniciarRenome (int indice)
    {
        if (! juce::isPositiveAndBelow (indice, botoes.size()))
            return;

        renomeando = indice;

        // Nome atual, nao o rotulo: o rotulo de uma cena sem nome e o numero
        // do slot, e comecar a digitar em cima de "3" faria o musico apagar um
        // "3" que nunca foi nome nenhum.
        editorDeNome.setText (nomes[indice], juce::dontSendNotification);
        editorDeNome.setBounds (botoes[indice]->getBounds());
        editorDeNome.setVisible (true);
        editorDeNome.toFront (true);
        editorDeNome.grabKeyboardFocus();
    }

    void concluirRenome (bool guardar)
    {
        const int indice = renomeando;

        if (indice < 0)
            return;

        // Zera ANTES de avisar: o onFocusLost dispara quando o editor some, e
        // sem isto a conclusao se chamaria de novo, em cima de um indice que
        // ja foi tratado.
        renomeando = -1;
        editorDeNome.setVisible (false);

        if (guardar && onRenomearCena != nullptr)
            onRenomearCena (indice, editorDeNome.getText().trim());
    }

    void acionar (int indice)
    {
        if (gravarBtn.getToggleState())
        {
            const auto r = onGravarCena != nullptr ? onGravarCena (indice)
                                                   : ChurchDAWLive::Resultado::indisponivel;

            // So desarma se GRAVOU. Desarmar depois de uma gravacao que nao
            // aconteceu era pior que o bug original: o botao apagava, o musico
            // lia isso como "gravou" e seguia o culto confiando numa cena que
            // nao existe — ele so descobriria no domingo seguinte.
            // desarmar() tambem para o prazo — senao ele dispararia depois e
            // anunciaria "gravar desarmou sozinho" logo apos uma gravacao que
            // deu certo.
            if (r == ChurchDAWLive::Resultado::ok)
                desarmar();

            mostrarAviso (ChurchDAWLive::avisoDoResultado (r, indice));
            repaint();
            return;
        }

        const auto r = onCarregarCena != nullptr ? onCarregarCena (indice)
                                                 : ChurchDAWLive::Resultado::indisponivel;
        mostrarAviso (ChurchDAWLive::avisoDoResultado (r, indice));
    }

    // O aviso some sozinho: e recado de palco, nao estado. Deixar fixo faria
    // o musico ler no proximo culto um erro da semana passada.
    void mostrarAviso (juce::String texto)
    {
        aviso = std::move (texto);
        repaint();

        if (aviso.isEmpty())
            return;

        // Geracao: dois toques seguidos deixariam o timer do PRIMEIRO apagar o
        // aviso do SEGUNDO 2,6 s antes da hora — justo no caso em que o musico
        // esta insistindo porque nao entendeu o que houve.
        const auto minhaGeracao = ++geracaoDoAviso;

        juce::Timer::callAfterDelay (2600,
            [ref = juce::Component::SafePointer<LiveScenePanel> (this), minhaGeracao]
            {
                if (auto* self = ref.getComponent())
                    if (self->geracaoDoAviso == minhaGeracao)
                    {
                        self->aviso.clear();
                        self->repaint();
                    }
            });
    }

    juce::String aviso;
    int geracaoDoAviso = 0;
    int alturaDoRotulo = 14;   // 0 numa faixa curta; paint() e resized() usam o MESMO valor
    std::array<bool, ChurchDAWLive::numeroDeCenas> preenchida {};
    std::array<juce::String, ChurchDAWLive::numeroDeCenas> nomes {};
    int renomeando = -1;
    juce::TextEditor editorDeNome;
    juce::OwnedArray<BotaoDeCena> botoes;
    juce::TextButton gravarBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveScenePanel)
};
