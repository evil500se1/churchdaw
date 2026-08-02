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
    // Renomear continua no toque longo (ou botao direito), porque precisa de
    // contexto e nao e gesto de palco. LIMPAR agora e visivel para a cena
    // selecionada, mas ainda pede confirmacao; o toque longo permanece como
    // atalho para renomear ou apagar sem carregar outra cena.
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

        void setSelecionadoComoDestino (bool deveFicarSelecionado)
        {
            if (selecionadoComoDestino == deveFicarSelecionado)
                return;

            selecionadoComoDestino = deveFicarSelecionado;
            repaint();
        }

        void paintButton (juce::Graphics& g, bool estaSobre, bool estaPressionado) override
        {
            juce::TextButton::paintButton (g, estaSobre, estaPressionado);

            // Vermelho continua reservado para a cena realmente no ar. A
            // borda ambar significa apenas "este e o destino que estou
            // preparando" e, portanto, nunca mente que um slot vazio tocou.
            if (selecionadoComoDestino)
            {
                g.setColour (juce::Colour (0xffffc247));
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f),
                                        3.5f, 2.0f);
            }
        }

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
        bool selecionadoComoDestino = false;
    };

    LiveScenePanel()
    {
        setTitle ("Cenas ao vivo");
        setDescription ("Dez cenas para preparar, salvar, carregar e limpar sem interromper o audio.");

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            auto* b = botoes.add (new BotaoDeCena());
            b->setWantsKeyboardFocus (false);   // seta do teclado nao troca musica
            // O titulo acessivel nunca vira o apelido da musica. Assim a
            // automacao/leitor de tela continua encontrando "Cena 2" mesmo
            // depois que o texto visual muda para "Aleluia".
            b->setTitle ("Cena " + juce::String (i + 1));
            b->onClick = [this, i] { acionar (i); };
            b->aoSegurar = [this, i] { mostrarMenu (i); };
            addAndMakeVisible (b);
        }

        gravarBtn.setButtonText ("GRAVAR");
        gravarBtn.setTitle ("Gravar cena");
        gravarBtn.setWantsKeyboardFocus (false);
        gravarBtn.setClickingTogglesState (true);
        gravarBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        gravarBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff8f0c21));
        gravarBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::dimText());
        gravarBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        gravarBtn.onClick = [this] { armarOuDesarmar(); };
        addAndMakeVisible (gravarBtn);

        limparBtn.setButtonText ("LIMPAR");
        limparBtn.setTitle ("Limpar cena");
        limparBtn.setWantsKeyboardFocus (false);
        limparBtn.setClickingTogglesState (true);
        limparBtn.setTooltip ("Limpar a cena selecionada");
        limparBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202329));
        limparBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff6f4a08));
        limparBtn.setColour (juce::TextButton::textColourOffId,
                             NordLookAndFeel::dimText().withAlpha (0.90f));
        limparBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        limparBtn.onClick = [this] { armarOuDesarmarLimpeza(); };
        limparBtn.setEnabled (false);
        addAndMakeVisible (limparBtn);

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
        const int selecionadaAntes = interacao.selecionada;
        ativaAtual = ativa;

        // A unica transicao preenchida -> vazia e a confirmacao de LIMPAR.
        // Volta a selecao para a cena que continua no ar (ou para nenhuma), em
        // vez de deixar uma borda de edicao sobre algo que acabou de ser
        // apagado deliberadamente.
        if (juce::isPositiveAndBelow (selecionadaAntes, (int) preenchida.size())
             && preenchida[(size_t) selecionadaAntes]
             && ! cenas[selecionadaAntes].preenchida)
            interacao.cenaFoiApagada (selecionadaAntes, ativa);

        interacao.sincronizarComAtiva (ativa);

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

            b->setTooltip (cena.preenchida
                              ? "Toque para carregar. Segure para renomear ou apagar."
                              : "Toque para preparar uma nova cena.");

            juce::String descricao = "Cena " + juce::String (i + 1) + ". ";

            if (! cena.preenchida)
                descricao += "Vazia. Toque para preparar e depois use Salvar.";
            else
                descricao += cena.nome.isNotEmpty()
                               ? "Salva como " + cena.nome + "."
                               : "Salva sem nome.";

            if (i == ativa)
                descricao += " Esta e a cena ativa no audio.";
            else if (i == interacao.selecionada)
                descricao += " Selecionada como destino; nao esta ativa no audio.";

            b->setDescription (descricao);
        }

        atualizarAcoes();
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
        {
            desarmar();
            desarmarLimpeza();
        }
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
        if (! gravarBtn.getToggleState())
        {
            stopTimer();

            repaint();
            return;
        }

        // Gravar e limpar sao intencoes mutuamente exclusivas. Nenhum toque
        // numa cena pode ficar ambiguo entre sobrescrever e apagar.
        desarmarLimpeza();

        // Fluxo pedido pelo cliente: toque numa cena vazia para prepara-la e,
        // depois de ajustar os timbres, toque SALVAR. Continua sendo dois
        // gestos deliberados, mas nao exige descobrir a ordem invertida
        // GRAVAR -> tocar novamente no mesmo numero.
        const int destinoNovo = interacao.destinoNovoAoPressionarGravar (true, preenchida);

        if (destinoNovo >= 0)
        {
            executarGravacao (destinoNovo, true);
            return;
        }

        startTimer (9000);

        repaint();
    }

    void desarmar()
    {
        stopTimer();

        if (gravarBtn.getToggleState())
            gravarBtn.setToggleState (false, juce::dontSendNotification);

        // Sempre limpa e sempre bump: um aviso deixado para tras reaparecia ao
        // reabrir o painel, contando um erro que ja passou.
        limparAviso();
    }

    void armarOuDesarmarLimpeza()
    {
        if (limparBtn.getToggleState())
        {
            // Entrar em LIMPAR nunca herda um GRAVAR armado. O proximo toque
            // escolhe o slot a apagar e nao carrega/troca o audio.
            desarmar();
            limparAviso();
        }

        atualizarAcoes();
        repaint();
    }

    void desarmarLimpeza()
    {
        if (limparBtn.getToggleState())
            limparBtn.setToggleState (false, juce::dontSendNotification);

        atualizarAcoes();
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
                      : (limparBtn.getToggleState()
                          ? juce::String::fromUTF8 ("LIMPAR  \xe2\x80\xa2  toque na cena que deseja apagar")
                          : gravarBtn.getToggleState()
                          ? juce::String::fromUTF8 ("LIVE MODE  \xe2\x80\xa2  toque numa cena para GRAVAR por cima")
                          : destinoNovoSelecionado()
                              ? juce::String::fromUTF8 ("CENA ")
                                  + juce::String (interacao.selecionada + 1)
                                  + juce::String::fromUTF8 (" NOVA  \xe2\x80\xa2  ajuste o som e toque SALVAR")
                              : juce::String::fromUTF8 ("LIVE MODE  \xe2\x80\xa2  toque para trocar de m\xc3\xbasica")),
                    getLocalBounds().reduced (8, 4).removeFromTop (alturaDoRotulo),
                    juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 4);

        const auto layout = ChurchDAWLive::calcularLayoutDaFaixaDeCenas (
            area.getWidth(), area.getHeight());
        alturaDoRotulo = layout.alturaDoRotulo;
        area.removeFromTop (alturaDoRotulo);

        if (area.getHeight() <= 0 || area.getWidth() <= 0)
            return;

        auto acoes = area.removeFromRight (layout.larguraDasAcoes);

        if (layout.acoesEmpilhadas)
        {
            const int metade = acoes.getHeight() / 2;
            gravarBtn.setBounds (encolher (acoes.removeFromTop (metade)));
            limparBtn.setBounds (encolher (acoes));
        }
        else
        {
            limparBtn.setBounds (encolher (acoes.removeFromRight (layout.larguraDoLimpar)));
            gravarBtn.setBounds (encolher (acoes));
        }

        area.removeFromRight (layout.espacoEntreCenasEAcoes);
        atualizarAcoes();

        for (int f = 0; f < layout.fileirasDeCenas; ++f)
        {
            auto linha = area.removeFromTop (layout.alturaDaFileira);
            const int largura = juce::jmax (
                1, linha.getWidth() / juce::jmax (1, layout.cenasPorFileira));

            for (int i = 0; i < layout.cenasPorFileira; ++i)
            {
                const int indice = f * layout.cenasPorFileira + i;

                if (! juce::isPositiveAndBelow (indice, botoes.size()))
                    break;

                const int w = i == layout.cenasPorFileira - 1 ? linha.getWidth() : largura;
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

    // Recuo so usa a folga ACIMA de 44 px. Reduzir o Rectangle do componente
    // reduz tambem o hit target; portanto nenhuma margem visual pode transformar
    // uma celula valida de 44 px em um botao de 40/32 px.
    static juce::Rectangle<int> encolher (juce::Rectangle<int> r)
    {
        const int x = juce::jmin (2, juce::jmax (0, (r.getWidth()  - 44) / 2));
        const int y = juce::jmin (2, juce::jmax (0, (r.getHeight() - 44) / 2));
        return r.reduced (x, y);
    }

    bool destinoNovoSelecionado() const
    {
        const int indice = interacao.selecionada;
        return juce::isPositiveAndBelow (indice, (int) preenchida.size())
            && ! preenchida[(size_t) indice];
    }

    void atualizarAcessibilidadeDasCenas()
    {
        for (int i = 0; i < botoes.size(); ++i)
        {
            juce::String descricao = "Cena " + juce::String (i + 1) + ". ";

            if (! preenchida[(size_t) i])
                descricao += "Vazia. Toque para preparar e depois use Salvar.";
            else
                descricao += nomes[(size_t) i].isNotEmpty()
                               ? "Salva como " + nomes[(size_t) i] + "."
                               : "Salva sem nome.";

            if (i == ativaAtual)
                descricao += " Esta e a cena ativa no audio.";
            else if (i == interacao.selecionada)
                descricao += " Selecionada como destino; nao esta ativa no audio.";

            botoes[i]->setDescription (descricao);
        }
    }

    void atualizarAcoes()
    {
        const int selecionada = interacao.selecionada;
        const bool indiceValido = juce::isPositiveAndBelow (selecionada,
                                                             (int) preenchida.size());
        const bool temCenaParaLimpar = indiceValido && preenchida[(size_t) selecionada];
        bool haAlgumaCenaPreenchida = false;

        for (const bool temCena : preenchida)
            haAlgumaCenaPreenchida = haAlgumaCenaPreenchida || temCena;

        if (falhaDeGravacaoRecente)
            gravarBtn.setButtonText (retryExigeToqueNaCena ? "CENA" : "TENTAR");
        else
            gravarBtn.setButtonText (indiceValido && ! temCenaParaLimpar ? "SALVAR" : "GRAVAR");

        limparBtn.setEnabled (haAlgumaCenaPreenchida);
        limparBtn.setButtonText (limparBtn.getToggleState()
                                   ? (limparBtn.getWidth() < 70 ? "SAIR" : "CANCELAR")
                                   : (limparBtn.getWidth() < 48 ? "X" : "LIMPAR"));

        gravarBtn.setDescription (gravarBtn.getToggleState()
                                    ? "Gravacao armada. Toque novamente numa cena para tentar salvar."
                                    : destinoNovoSelecionado()
                                        ? "Salvar o som atual na cena vazia selecionada."
                                        : "Armar gravacao de uma cena.");
        limparBtn.setDescription (limparBtn.getToggleState()
                                    ? "Limpeza armada. Toque numa cena preenchida para escolher sem carrega-la."
                                    : "Escolher uma cena preenchida para apagar sem mudar o audio.");

        setDescription (limparBtn.getToggleState()
                          ? "Modo de limpeza. O proximo toque escolhe uma cena para apagar sem carrega-la."
                          : destinoNovoSelecionado()
                              ? "Nova cena selecionada. Ajuste o som e use Salvar."
                              : "Dez cenas para preparar, salvar e carregar ao vivo.");

        atualizarAcessibilidadeDasCenas();

        for (int i = 0; i < botoes.size(); ++i)
            botoes[i]->setSelecionadoComoDestino (i == selecionada && i != ativaAtual);
    }

    void limparAviso()
    {
        ++geracaoDoAviso;
        aviso.clear();
        falhaDeGravacaoRecente = false;
        retryExigeToqueNaCena = false;
        atualizarAcoes();
        repaint();
    }

    void executarGravacao (int indice, bool veioDoBotaoSalvar)
    {
        const auto r = onGravarCena != nullptr ? onGravarCena (indice)
                                               : ChurchDAWLive::Resultado::indisponivel;

        const auto origem = veioDoBotaoSalvar
                              ? ChurchDAWLive::OrigemDaGravacao::botaoSalvar
                              : ChurchDAWLive::OrigemDaGravacao::gravarArmadoECena;
        const bool manterArmado = ChurchDAWLive::manterGravarArmadoParaRetry (r, origem);

        if (! manterArmado)
            desarmar();
        else
            startTimer (9000);   // o retry ganha um prazo novo completo

        falhaDeGravacaoRecente = r != ChurchDAWLive::Resultado::ok;
        retryExigeToqueNaCena = manterArmado;
        mostrarAviso (ChurchDAWLive::avisoDaGravacao (r, origem, indice));
        atualizarAcoes();
        repaint();
    }

    void mostrarMenu (int indice)
    {
        // Toque longo e um gesto de administracao. Ele sempre cancela as
        // intencoes perigosas e apenas SELECIONA o slot; nunca carrega o audio.
        // Isso tambem torna uma cena inativa o alvo do LIMPAR visivel.
        desarmar();
        desarmarLimpeza();
        interacao.selecionarSemAcionar (indice);
        atualizarAcoes();

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
        const bool limparArmado = limparBtn.getToggleState();
        const auto acao = interacao.tocar (
            indice,
            juce::isPositiveAndBelow (indice, (int) preenchida.size())
                && preenchida[(size_t) indice],
            gravarBtn.getToggleState(),
            limparArmado);

        atualizarAcoes();

        if (acao == ChurchDAWLive::AcaoDeCena::nenhuma)
        {
            if (limparArmado)
                mostrarAviso (ChurchDAWLive::avisoDoResultado (
                    ChurchDAWLive::Resultado::vazia, indice));

            return;
        }

        if (acao == ChurchDAWLive::AcaoDeCena::apagar)
        {
            // A escolha nao chamou onCarregarCena: cenaAtiva e audio seguem
            // exatamente onde estavam. A confirmacao continua no callback.
            desarmar();
            desarmarLimpeza();

            if (onApagarCena != nullptr)
                onApagarCena (indice);
            else
                mostrarAviso (ChurchDAWLive::avisoDoResultado (
                    ChurchDAWLive::Resultado::indisponivel, indice));

            return;
        }

        if (acao == ChurchDAWLive::AcaoDeCena::gravar)
        {
            executarGravacao (indice, false);
            return;
        }

        if (acao == ChurchDAWLive::AcaoDeCena::prepararNova)
        {
            const auto r = onCarregarCena != nullptr ? onCarregarCena (indice)
                                                     : ChurchDAWLive::Resultado::indisponivel;

            if (r == ChurchDAWLive::Resultado::vazia)
            {
                // Nao e erro: o slot vazio virou uma pagina nova e o callback
                // preparou o motor sem timbres. O texto explica o proximo gesto
                // e a cena anterior continua intacta no banco para voltar depois.
                limparAviso();
                atualizarAcoes();
            }
            else
            {
                mostrarAviso (ChurchDAWLive::avisoDoResultado (r, indice));
            }

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
        if (aviso.isNotEmpty())
            setDescription ("Aviso: " + aviso);
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
                        self->falhaDeGravacaoRecente = false;
                        self->retryExigeToqueNaCena = false;
                        self->atualizarAcoes();
                        self->repaint();
                    }
            });
    }

    juce::String aviso;
    int geracaoDoAviso = 0;
    bool falhaDeGravacaoRecente = false;
    bool retryExigeToqueNaCena = false;
    int alturaDoRotulo = 14;   // 0 numa faixa curta; paint() e resized() usam o MESMO valor
    std::array<bool, ChurchDAWLive::numeroDeCenas> preenchida {};
    std::array<juce::String, ChurchDAWLive::numeroDeCenas> nomes {};
    ChurchDAWLive::InteracaoDeCenas interacao;
    int ativaAtual = -1;
    int renomeando = -1;
    juce::TextEditor editorDeNome;
    juce::OwnedArray<BotaoDeCena> botoes;
    juce::TextButton gravarBtn;
    juce::TextButton limparBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveScenePanel)
};
