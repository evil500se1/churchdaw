#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>

//==============================================================================
// LIVE MODE — 10 cenas para trocar de musica no culto.
//
// Pedido de cliente (01/08/2026): "uns 10 programas pelo menos para salvar
// cenas".
//
// Cada cena nova e uma PAGINA completa: guarda mixer/Pad/Master aqui e aponta
// para um snapshot separado do motor (timbres nativos, Kontakt/AU/VST e seus
// estados). O arquivo grande fica fora deste XML para que trocar a cena ativa
// continue gravando apenas poucos KB, sem reescrever estados de plugin no meio
// do culto. Cenas antigas sem snapshot continuam compativeis como cenas de
// mixer; ao sair delas pela primeira vez, o Main captura a pagina que esta no
// ar antes de carregar outra.
//==============================================================================
namespace ChurchDAWLive
{

inline constexpr int numeroDeCenas = 10;
inline constexpr int numeroDeCanais = 14;
inline constexpr std::uint64_t tamanhoMaximoPaginaDeCena =
    256ull * 1024ull * 1024ull;

inline bool paginaDeCenaTemTamanhoValido (std::uint64_t bytes) noexcept
{
    return bytes > 0 && bytes <= tamanhoMaximoPaginaDeCena;
}

// Uma requisicao nova precisa chegar a setStateInformation/clearLayer para
// invalidar as geracoes da requisicao anterior. So esperamos depois que a
// pagina desta propria cena ja foi enviada ao motor.
inline bool deveAguardarCena (bool paginaJaSolicitada,
                              bool motorCarregando,
                              bool restauracaoPendente) noexcept
{
    return paginaJaSolicitada && (motorCarregando || restauracaoPendente);
}

// O startup tem duas fontes persistidas que podem divergir depois de uma queda:
// o setup global e a cena que estava ativa. Para uma cena v2, a pagina pelo SHA
// e a fonte de verdade. Para uma cena v1, os valores da PROPRIA cena precisam
// ser aplicados antes de anexar ao slot uma pagina capturada do motor. Capturar
// primeiro transformaria o setup global A na cena B e apagaria B para sempre.
struct PlanoDeReconciliacaoNoStartup
{
    bool aplicarPaginaPorSha = false;
    bool aplicarCenaAntesDeAnexarPagina = false;
    bool anexarPaginaDepoisDeAplicar = false;
};

inline PlanoDeReconciliacaoNoStartup planejarReconciliacaoNoStartup (
    bool cenaPreenchida, bool temShaValido) noexcept
{
    if (! cenaPreenchida)
        return {};

    if (temShaValido)
        return { true, false, false };

    return { false, true, true };
}

// Maquina geracional pura usada para impedir que os ChangeBroadcasters
// produzidos por setStateInformation() armem o autosave pesado do setup. O
// token zero significa "nao agendou"; tokens reais nunca usam zero.
struct ControleDeGravacaoDoSetupDuranteCena
{
    using GeracaoDeGravacao = std::uint64_t;

    GeracaoDeGravacao agendarSePermitido (int geracaoAtualDaCena) noexcept
    {
        if (estaSuprimida (geracaoAtualDaCena))
            return 0;

        return avancarGeracaoDaGravacao();
    }

    void iniciarSupressao (int geracaoAtualDaCena) noexcept
    {
        geracaoDaCenaSuprimida = geracaoAtualDaCena;

        // Invalida inclusive um timer armado antes de a troca comecar.
        (void) avancarGeracaoDaGravacao();
    }

    bool encerrarSupressao (int geracaoEsperada,
                            int geracaoAtualDaCena) noexcept
    {
        // Uma soltura atrasada da cena A nunca pode liberar a cena B.
        if (geracaoAtualDaCena != geracaoEsperada
             || geracaoDaCenaSuprimida != geracaoEsperada)
            return false;

        geracaoDaCenaSuprimida = -1;
        return true;
    }

    bool estaSuprimida (int geracaoAtualDaCena) const noexcept
    {
        return geracaoDaCenaSuprimida == geracaoAtualDaCena;
    }

    bool timerAindaValido (GeracaoDeGravacao token,
                           int geracaoAtualDaCena) const noexcept
    {
        return token != 0
            && token == geracaoDaGravacao
            && ! estaSuprimida (geracaoAtualDaCena);
    }

private:
    GeracaoDeGravacao avancarGeracaoDaGravacao() noexcept
    {
        // uint64_t tem wrap definido. Reserva zero para o retorno "bloqueado".
        if (++geracaoDaGravacao == 0)
            ++geracaoDaGravacao;

        return geracaoDaGravacao;
    }

    GeracaoDeGravacao geracaoDaGravacao = 0;
    int geracaoDaCenaSuprimida = -1;
};

// O painel divide as cenas em duas fileiras quando a tela e estreita. Com um
// numero impar sobraria um botao SEM bounds — invisivel e ainda assim
// clicavel na posicao antiga, que e exatamente a classe de bug que ja custou
// caro no botao Live do Android.
static_assert (numeroDeCenas % 2 == 0 && numeroDeCenas >= 2,
               "LIVE MODE: numeroDeCenas precisa ser par (o painel usa duas fileiras)");

struct CanalDaCena
{
    bool  ligado = true;
    bool  mudo = false;
    bool  solo = false;

    // NORMALIZADO (0..1), nao dB e nao ganho linear — e o dominio em que o
    // parametro do motor viaja. O motor vai de 0 a 1,5 de ganho, entao a
    // UNIDADE (ganho 1,0) fica em 1/1,5 = 0,667, e NAO em 1,0.
    //
    // O default importa de verdade: a leitura e aditiva, entao este valor e o
    // que sobra quando o atributo falta no XML. Com 1,0 aqui, um canal sem o
    // atributo entrava com ganho 1,5 — +3,5 dB de surpresa no meio do culto.
    float volume = 1.0f / 1.5f;

    int   transpose = 0;
    int   octave = 0;
    float envioDeReverb = 0.0f;

    // So diagnostico: o painel compara com o que esta carregado hoje e avisa
    // a diferenca. NUNCA e usado para carregar nada.
    juce::String rotuloDoTimbre;
};

struct Cena
{
    bool preenchida = false;
    juce::String nome;
    juce::String gravadaEm;      // ISO-8601, para o musico saber qual e a nova

    // Metadados de palco. Sao aditivos: cenas antigas continuam validas e
    // recebem valores neutros, enquanto Performance/Sessao passam a mostrar
    // a mesma informacao que o musico usa no culto.
    //
    // musica e deliberadamente separada de nome/secao. Uma musica pode possuir
    // varias cenas (Intro, Verso, Refrao...) e o Setlist continua referenciando
    // os snapshots reais. Assim o formato antigo permanece compativel, mas a
    // Session consegue apresentar a hierarquia culto -> musica -> cena sem
    // inventar uma segunda copia do estado de audio.
    juce::String musica;
    juce::String tonalidade { "C" };
    juce::String compasso { "4/4" };
    juce::String secao { "Cena" };
    double bpm = 120.0;
    double duracaoSegundos = 0.0;
    double transicaoSegundos = 0.35;

    // SHA-256 hexadecimal do arquivo binario em live-scene-pages/. Guardar so
    // o ID torna o XML pequeno e impede caminho injetado por arquivo corrompido.
    // Vazio = cena legada, apenas mixer.
    juce::String estadoDoMotorId;

    CanalDaCena canais[numeroDeCanais];

    // Globais do Church Live.
    //
    // "tem..." nao e frescura: cena gravada por uma versao que ainda nao
    // guardava estes campos NAO pode aplicar os valores de fabrica abaixo por
    // cima do que o musico ajustou. A leitura aditiva protege o XML, mas quem
    // aplica precisa saber a diferenca entre "gravado como 0" e "nunca
    // gravado" — senao tocar uma cena antiga zerava o transpose global e
    // jogava o reverb para 0,72 no meio do louvor.
    bool  temGlobais = false;
    float volumeGeral = 1.0f;
    int   transposeGeral = 0;
    float retornoDeReverb = 0.72f;

    // Master
    bool  temMaster = false;
    float masterDb = 0.0f;

    // Estes dois controles pertencem ao MasterProcessor externo, portanto nao
    // entram na pagina binaria do TimbresProcessor. As flags individuais
    // preservam cenas v2 antigas que tinham <MASTER db="...">, mas ainda nao
    // conheciam Auto EQ nem Punch/Peso: atributo ausente continua significando
    // "nao mexe" em vez de aplicar silenciosamente um default novo.
    bool  temMasterAutoEq = false;
    bool  masterAutoEq = false;
    bool  temMasterPunch = false;
    bool  masterPunch = true;

    // Pad Continuo: o que mais muda de uma musica para a outra.
    int   padTom = -1;           // -1 = desligado
    float padVolume = 0.9f;
    float padCorte = 1.0f;
};

inline bool idDeEstadoDoMotorValido (const juce::String& id)
{
    if (id.length() != 64)
        return false;

    for (auto c : id)
        if (! ((c >= '0' && c <= '9')
                || (c >= 'a' && c <= 'f')
                || (c >= 'A' && c <= 'F')))
            return false;

    return true;
}

//==============================================================================
// Estado puro da interacao do painel.
//
// A cena ATIVA pertence ao motor/MainComponent: e o som que esta no ar. Este
// estado guarda somente a cena SELECIONADA no painel. Separar os dois permite
// preparar um slot vazio sem apagar, trocar ou fingir que aquele slot esta
// tocando. Tambem deixa a regra de seguranca verificavel sem uma janela JUCE:
// tocar num slot vazio apenas o prepara; gravar continua exigindo outro gesto.
//==============================================================================
enum class AcaoDeCena
{
    nenhuma,
    carregar,
    prepararNova,
    gravar,
    apagar
};

struct InteracaoDeCenas
{
    int selecionada = -1;

    void sincronizarComAtiva (int ativa)
    {
        if (! juce::isPositiveAndBelow (selecionada, numeroDeCenas))
            selecionada = juce::isPositiveAndBelow (ativa, numeroDeCenas) ? ativa : -1;
    }

    AcaoDeCena tocar (int indice,
                      bool preenchida,
                      bool gravarArmado,
                      bool limparArmado = false)
    {
        if (! juce::isPositiveAndBelow (indice, numeroDeCenas))
            return AcaoDeCena::nenhuma;

        selecionada = indice;

        // LIMPAR e um modo de escolha, nao uma carga disfarçada. Assim o
        // musico consegue apontar uma cena preenchida que NAO esta no ar sem
        // mudar o audio apenas para poder apaga-la.
        if (limparArmado)
            return preenchida ? AcaoDeCena::apagar : AcaoDeCena::nenhuma;

        if (gravarArmado)
            return AcaoDeCena::gravar;

        return preenchida ? AcaoDeCena::carregar : AcaoDeCena::prepararNova;
    }

    int destinoNovoAoPressionarGravar (
        bool gravarFicouArmado,
        const std::array<bool, numeroDeCenas>& preenchidas) const
    {
        if (! gravarFicouArmado
             || ! juce::isPositiveAndBelow (selecionada, numeroDeCenas)
             || preenchidas[(size_t) selecionada])
            return -1;

        return selecionada;
    }

    void selecionarSemAcionar (int indice)
    {
        if (juce::isPositiveAndBelow (indice, numeroDeCenas))
            selecionada = indice;
    }

    void cenaFoiApagada (int indice, int ativa)
    {
        if (selecionada == indice)
            selecionada = juce::isPositiveAndBelow (ativa, numeroDeCenas) ? ativa : -1;
    }
};

// Decide o que o indicador/botao SALVAR do cabecalho deve gravar.
//
// Uma pagina nova ainda nao e uma cena ativa: o audio foi zerado, mas o slot
// continua vazio ate o segundo gesto de salvar. Por isso ela tem precedencia
// sobre a cena ativa anterior. Fora desse fluxo, SALVAR recaptura a cena que
// esta realmente no ar. Sem um dos dois contextos, ele continua sendo apenas
// o salvamento geral do projeto e nao sobrescreve um slot arbitrario.
inline int destinoDoSalvarSuperior (
    int cenaEmPreparacao,
    int cenaAtiva,
    const Cena (&cenas)[numeroDeCenas]) noexcept
{
    if (juce::isPositiveAndBelow (cenaEmPreparacao, numeroDeCenas)
         && ! cenas[cenaEmPreparacao].preenchida)
        return cenaEmPreparacao;

    if (juce::isPositiveAndBelow (cenaAtiva, numeroDeCenas)
         && cenas[cenaAtiva].preenchida)
        return cenaAtiva;

    return -1;
}

//==============================================================================
// Geometria pura da faixa de cenas.
//
// O layout real passa por aqui e o teste tambem. Isso impede que um comentario
// prometa 44 px enquanto margens, rotulo e duas fileiras deixam o Component
// clicavel com 30 px no aparelho. As dimensoes recebidas ja sao as da area
// interna do painel (depois da margem decorativa externa).
//==============================================================================
struct LayoutDaFaixaDeCenas
{
    int alturaDoRotulo = 0;
    int larguraDasAcoes = 0;
    int espacoEntreCenasEAcoes = 0;
    int fileirasDeCenas = 1;
    int cenasPorFileira = numeroDeCenas;
    int alturaDaFileira = 0;
    int larguraMinimaDaCena = 0;
    int larguraDoLimpar = 0;
    int larguraDoGravar = 0;
    int alturaMinimaDaAcao = 0;
    bool acoesEmpilhadas = false;
    bool suportaAlvosDe44 = false;
};

inline LayoutDaFaixaDeCenas calcularLayoutDaFaixaDeCenas (int largura, int altura)
{
    LayoutDaFaixaDeCenas r;
    largura = juce::jmax (0, largura);
    altura = juce::jmax (0, altura);

    if (largura == 0 || altura == 0)
        return r;

    r.espacoEntreCenasEAcoes = juce::jmin (6, largura / 4);

    // Uma fileira exige 10 * 44 para as cenas e 2 * 44 para as acoes. Quando
    // essa largura nao existe mas ha 88 px de altura, fazemos 5+5 e empilhamos
    // as duas acoes. E a configuracao usada no retrato de 320/360 dp.
    const int larguraMinimaEmUmaFileira = numeroDeCenas * 44 + 2 * 44
                                        + r.espacoEntreCenasEAcoes;
    const bool precisaDeDuas = largura < larguraMinimaEmUmaFileira;
    const bool cabemDuas = altura >= 2 * 44;

    r.fileirasDeCenas = precisaDeDuas && cabemDuas ? 2 : 1;
    r.cenasPorFileira = numeroDeCenas / r.fileirasDeCenas;
    r.acoesEmpilhadas = r.fileirasDeCenas == 2;

    const int larguraMinimaDasAcoes = r.acoesEmpilhadas ? 44 : 2 * 44;
    const int larguraMaximaDasAcoes = juce::jmax (
        0, largura - r.espacoEntreCenasEAcoes - r.cenasPorFileira * 44);

    if (larguraMaximaDasAcoes >= larguraMinimaDasAcoes)
        r.larguraDasAcoes = juce::jlimit (
            larguraMinimaDasAcoes,
            juce::jmin (154, larguraMaximaDasAcoes),
            largura / 4);
    else
        r.larguraDasAcoes = juce::jmin (largura, juce::jmax (1, largura / 4));

    // Rotulo so ocupa espaco quando, depois dele, TODOS os alvos continuam
    // com 44 px. Em faixas curtas a instrucao continua nos tooltips e na
    // descricao acessivel, sem roubar o alvo do dedo.
    const int sobraParaRotulo = altura - r.fileirasDeCenas * 44;
    r.alturaDoRotulo = sobraParaRotulo >= 10 ? juce::jmin (14, sobraParaRotulo) : 0;

    const int alturaDasAcoesECenas = altura - r.alturaDoRotulo;
    r.alturaDaFileira = alturaDasAcoesECenas / r.fileirasDeCenas;
    r.alturaMinimaDaAcao = r.acoesEmpilhadas ? alturaDasAcoesECenas / 2
                                              : alturaDasAcoesECenas;

    const int larguraDasCenas = juce::jmax (
        0, largura - r.larguraDasAcoes - r.espacoEntreCenasEAcoes);
    r.larguraMinimaDaCena = larguraDasCenas / r.cenasPorFileira;

    if (r.acoesEmpilhadas)
    {
        r.larguraDoLimpar = r.larguraDasAcoes;
        r.larguraDoGravar = r.larguraDasAcoes;
    }
    else
    {
        r.larguraDoLimpar = r.larguraDasAcoes / 2;
        r.larguraDoGravar = r.larguraDasAcoes - r.larguraDoLimpar;
    }

    r.suportaAlvosDe44 = r.alturaDaFileira >= 44
                      && r.alturaMinimaDaAcao >= 44
                      && r.larguraMinimaDaCena >= 44
                      && r.larguraDoLimpar >= 44
                      && r.larguraDoGravar >= 44;
    return r;
}

//==============================================================================
// Setlist — a ordem fixa do culto.
//
// As 10 cenas continuam soltas na grade; o setlist e so uma sequencia de
// indices que o musico definiu na ordem do culto. O gesto "proxima cena"
// (footswitch/CC, tratado no MainComponent com o mecanismo de pickup existente)
// chama proximaCena() e aciona o fluxo de carregamento que ja existe
// (AcaoDeCena::carregar via tocar()). Assim TODA a defesa anti-acidente do
// painel continua valendo: um toque so carrega, gravar continua exigindo armar
// GRAVAR antes, etc.
//
// Sem setlist configurado, proximaCena() devolve -1 e o chamador nao faz nada
// — nenhum comportamento existente muda. Este struct e estado puro de UI: nao
// entra na thread de audio. A unica String e o nome do culto; a sequencia
// continua fixa e sem alocacao durante navegacao/reordem.
//==============================================================================
struct Setlist
{
    static constexpr int capacidadeMaxima = 64; // teto alto, sem alocacao dinamica

    // Nome do culto/projeto ao vivo. O array abaixo continua sendo a sequencia
    // exata de snapshots; o agrupamento em musicas vem de Cena::musica.
    juce::String nome { "Culto atual" };
    std::array<int, capacidadeMaxima> ordem = {};
    int tamanho = 0;
    int posicao = -1; // -1 = antes da primeira; proximaCena devolve ordem[0]

    bool vazio() const noexcept { return tamanho <= 0; }

    bool indiceValido (int indice) const noexcept
    {
        return juce::isPositiveAndBelow (indice, numeroDeCenas);
    }

    void limpar() noexcept
    {
        tamanho = 0;
        posicao = -1;
    }

    // Adiciona uma cena ao fim do setlist. Ignora indice invalido e duplicata
    // para nao inflar a lista por engano. Nao aloca.
    void adicionar (int indice) noexcept
    {
        if (! indiceValido (indice) || tamanho >= capacidadeMaxima)
            return;

        for (int i = 0; i < tamanho; ++i)
            if (ordem[(size_t) i] == indice)
                return;

        ordem[(size_t) tamanho] = indice;
        ++tamanho;
    }

    // Reposiciona o cursor para a cena atualmente no ar, se ela estiver no
    // setlist. Devolve true se achou. Usado quando o musico carrega uma cena
    // manualmente e o setlist precisa se sincronizar sem pular musica.
    bool reposicionarPara (int indice) noexcept
    {
        for (int i = 0; i < tamanho; ++i)
        {
            if (ordem[(size_t) i] == indice)
            {
                posicao = i;
                return true;
            }
        }
        return false;
    }

    // Devolve o indice da proxima cena do setlist e avanca o cursor, ou -1 se
    // nao ha setlist ou chegou ao fim. NAO carrega nada por si: so entrega o
    // indice para o chamador acionar o fluxo existente. Ao chegar ao fim fica
    // parado (devolve -1) para o musico nao pular do fim para o inicio sem
    // querer no meio do culto.
    int proximaCena() noexcept
    {
        if (vazio())
            return -1;

        const int proxima = posicao + 1;

        if (! juce::isPositiveAndBelow (proxima, tamanho))
            return -1;

        posicao = proxima;
        return ordem[(size_t) posicao];
    }

    // Indice da cena do setlist na posicao atual do cursor, ou -1 se o cursor
    // ainda nao aponta para nenhuma. So para diagnostico/UI.
    int cenaAtual() const noexcept
    {
        if (! juce::isPositiveAndBelow (posicao, tamanho))
            return -1;

        return ordem[(size_t) posicao];
    }

    bool removerNaPosicao (int indice) noexcept
    {
        if (! juce::isPositiveAndBelow (indice, tamanho))
            return false;

        for (int i = indice; i + 1 < tamanho; ++i)
            ordem[(size_t) i] = ordem[(size_t) (i + 1)];

        --tamanho;
        if (posicao > indice) --posicao;
        else if (posicao == indice) posicao = juce::jmin (indice, tamanho - 1);
        if (tamanho <= 0) posicao = -1;
        return true;
    }

    bool mover (int origem, int destino) noexcept
    {
        if (! juce::isPositiveAndBelow (origem, tamanho)
             || ! juce::isPositiveAndBelow (destino, tamanho)
             || origem == destino)
            return false;

        const int cena = ordem[(size_t) origem];
        if (origem < destino)
            for (int i = origem; i < destino; ++i)
                ordem[(size_t) i] = ordem[(size_t) (i + 1)];
        else
            for (int i = origem; i > destino; --i)
                ordem[(size_t) i] = ordem[(size_t) (i - 1)];

        ordem[(size_t) destino] = cena;
        if (posicao == origem) posicao = destino;
        else if (origem < posicao && destino >= posicao) --posicao;
        else if (origem > posicao && destino <= posicao) ++posicao;
        return true;
    }
};

//==============================================================================
// Serializacao.
//
// REGRA: a leitura e ADITIVA. Atributo ausente significa "nao mexe neste
// parametro", e o padrao de cada campo e o valor que ja esta na cena passada
// por referencia — nunca um valor de fabrica.
//
// Isso e o oposto do setStateInformation do motor, que e substitutivo de
// proposito. Aqui tem de ser aditivo para que uma cena gravada numa versao
// antiga, lida por uma versao nova, nao zere um parametro que ela nem
// conhecia.
//==============================================================================
inline void escreverCena (juce::XmlElement& pai, const Cena& cena, int indice)
{
    auto* c = pai.createNewChildElement ("SCENE");
    c->setAttribute ("index", indice);
    c->setAttribute ("filled", cena.preenchida ? 1 : 0);
    c->setAttribute ("name", cena.nome);
    c->setAttribute ("song", cena.musica);
    c->setAttribute ("key", cena.tonalidade);
    c->setAttribute ("meter", cena.compasso);
    c->setAttribute ("section", cena.secao);
    c->setAttribute ("bpm", cena.bpm);
    c->setAttribute ("duration", cena.duracaoSegundos);
    c->setAttribute ("transition", cena.transicaoSegundos);

    // Cena vazia nao tem estado para guardar. Escrever os 14 canais dela era
    // gravar 140 elementos <L> de valores de fabrica que a leitura aditiva
    // ignoraria de qualquer jeito — puro peso no arquivo que e reescrito a
    // cada gravacao.
    if (! cena.preenchida)
        return;

    c->setAttribute ("savedAt", cena.gravadaEm);

    if (idDeEstadoDoMotorValido (cena.estadoDoMotorId))
    {
        auto* engine = c->createNewChildElement ("ENGINE");
        engine->setAttribute ("sha256", cena.estadoDoMotorId.toLowerCase());
    }

    // Nao materialize blocos que a cena legada nunca teve. Salvar o banco ao
    // renomear outro slot ou trocar a cena ativa precisa conservar
    // temGlobais/temMaster falsos; recapturar deliberadamente esta propria cena
    // e que a converte para o formato completo. Caso contrario uma simples
    // escrita do banco faria a proxima carga aplicar defaults que nunca fizeram
    // parte daquela musica.
    if (cena.temGlobais)
    {
        auto* g = c->createNewChildElement ("LIVE");
        g->setAttribute ("gain", (double) cena.volumeGeral);
        g->setAttribute ("gtransp", cena.transposeGeral);
        g->setAttribute ("rvbreturn", (double) cena.retornoDeReverb);
    }

    if (cena.temMaster)
    {
        auto* m = c->createNewChildElement ("MASTER");
        m->setAttribute ("db", (double) cena.masterDb);

        if (cena.temMasterAutoEq)
            m->setAttribute ("autoEq", cena.masterAutoEq ? 1 : 0);

        if (cena.temMasterPunch)
            m->setAttribute ("punch", cena.masterPunch ? 1 : 0);
    }

    auto* p = c->createNewChildElement ("PAD");
    p->setAttribute ("key", cena.padTom);
    p->setAttribute ("gain", (double) cena.padVolume);
    p->setAttribute ("cutoff", (double) cena.padCorte);

    for (int i = 0; i < numeroDeCanais; ++i)
    {
        const auto& canal = cena.canais[i];
        auto* l = c->createNewChildElement ("L");
        l->setAttribute ("i", i);
        l->setAttribute ("on", canal.ligado ? 1 : 0);
        l->setAttribute ("mute", canal.mudo ? 1 : 0);
        l->setAttribute ("solo", canal.solo ? 1 : 0);
        l->setAttribute ("vol", (double) canal.volume);
        l->setAttribute ("transp", canal.transpose);
        l->setAttribute ("oct", canal.octave);
        l->setAttribute ("send", (double) canal.envioDeReverb);
        l->setAttribute ("timbre", canal.rotuloDoTimbre);
    }
}

inline bool isLegacyChurchPadTimbre (const juce::String& label) noexcept
{
    const auto name = label.trim();
    return name.equalsIgnoreCase ("PAD LP")
        || name.equalsIgnoreCase ("Church Pad")
        || name.equalsIgnoreCase ("Pad Continuo");
}

inline void lerCena (const juce::XmlElement& c, Cena& cena,
                     bool migrateLegacyPadOctave = false)
{
    cena.preenchida = c.getIntAttribute ("filled", cena.preenchida ? 1 : 0) != 0;
    cena.nome       = c.getStringAttribute ("name", cena.nome);
    cena.musica     = c.getStringAttribute ("song", cena.musica)
                          .trim().substring (0, 56);
    cena.gravadaEm  = c.getStringAttribute ("savedAt", cena.gravadaEm);
    cena.tonalidade = c.getStringAttribute ("key", cena.tonalidade)
                          .trim().substring (0, 8);
    cena.compasso = c.getStringAttribute ("meter", cena.compasso)
                        .trim().substring (0, 8);
    cena.secao = c.getStringAttribute ("section", cena.secao)
                     .trim().substring (0, 28);
    cena.bpm = juce::jlimit (30.0, 300.0,
        c.getDoubleAttribute ("bpm", cena.bpm));
    cena.duracaoSegundos = juce::jlimit (0.0, 86400.0,
        c.getDoubleAttribute ("duration", cena.duracaoSegundos));
    cena.transicaoSegundos = juce::jlimit (0.0, 15.0,
        c.getDoubleAttribute ("transition", cena.transicaoSegundos));

    if (auto* engine = c.getChildByName ("ENGINE"))
    {
        const auto id = engine->getStringAttribute ("sha256").trim();

        if (idDeEstadoDoMotorValido (id))
            cena.estadoDoMotorId = id.toLowerCase();
    }

    if (auto* g = c.getChildByName ("LIVE"))
    {
        cena.temGlobais      = true;
        cena.volumeGeral     = (float) g->getDoubleAttribute ("gain", (double) cena.volumeGeral);
        cena.transposeGeral  = g->getIntAttribute ("gtransp", cena.transposeGeral);
        cena.retornoDeReverb = (float) g->getDoubleAttribute ("rvbreturn", (double) cena.retornoDeReverb);
    }

    if (auto* m = c.getChildByName ("MASTER"))
    {
        cena.temMaster = true;
        cena.masterDb  = (float) m->getDoubleAttribute ("db", (double) cena.masterDb);

        if (m->hasAttribute ("autoEq"))
        {
            cena.temMasterAutoEq = true;
            cena.masterAutoEq = m->getBoolAttribute ("autoEq", cena.masterAutoEq);
        }

        if (m->hasAttribute ("punch"))
        {
            cena.temMasterPunch = true;
            cena.masterPunch = m->getBoolAttribute ("punch", cena.masterPunch);
        }
    }

    if (auto* p = c.getChildByName ("PAD"))
    {
        cena.padTom    = juce::jlimit (-1, 11, p->getIntAttribute ("key", cena.padTom));
        cena.padVolume = (float) p->getDoubleAttribute ("gain", (double) cena.padVolume);
        cena.padCorte  = (float) p->getDoubleAttribute ("cutoff", (double) cena.padCorte);
    }

    for (auto* l : c.getChildWithTagNameIterator ("L"))
    {
        const int i = l->getIntAttribute ("i", -1);

        if (! juce::isPositiveAndBelow (i, numeroDeCanais))
            continue;

        auto& canal = cena.canais[i];
        canal.ligado         = l->getIntAttribute ("on", canal.ligado ? 1 : 0) != 0;
        canal.mudo           = l->getIntAttribute ("mute", canal.mudo ? 1 : 0) != 0;
        canal.solo           = l->getIntAttribute ("solo", canal.solo ? 1 : 0) != 0;
        canal.volume         = (float) l->getDoubleAttribute ("vol", (double) canal.volume);
        canal.transpose      = juce::jlimit (-24, 24, l->getIntAttribute ("transp", canal.transpose));
        canal.octave         = juce::jlimit (-4, 4, l->getIntAttribute ("oct", canal.octave));
        canal.envioDeReverb  = (float) l->getDoubleAttribute ("send", (double) canal.envioDeReverb);
        canal.rotuloDoTimbre = l->getStringAttribute ("timbre", canal.rotuloDoTimbre);

        // Versoes antigas aplicavam octave=-2 como "default" do Pad Church.
        // O valor correto pertence ao canal e deve nascer em zero; cenas novas
        // continuam livres para salvar qualquer oitava escolhida pelo musico.
        if (migrateLegacyPadOctave
             && canal.octave == -2
             && isLegacyChurchPadTimbre (canal.rotuloDoTimbre))
            canal.octave = 0;
    }
}

//==============================================================================
// Serializacao do setlist.
//
// Bloco separado (<SETLIST>) no mesmo pai que os <SCENE>: nao toca nos dados
// de nenhuma cena, entao um banco gravado por uma versao que nao conhecia
// setlist continua lendo identico. Leitura aditiva, como a das cenas: bloco
// ausente significa "sem setlist" e o comportamento do painel nao muda.
//==============================================================================
inline void escreverSetlist (juce::XmlElement& pai, const Setlist& setlist)
{
    const auto nome = setlist.nome.trim().substring (0, 64);
    if (setlist.vazio() && nome.isEmpty())
        return;

    auto* s = pai.createNewChildElement ("SETLIST");
    s->setAttribute ("name", nome);
    s->setAttribute ("size", setlist.tamanho);
    s->setAttribute ("pos", setlist.posicao);

    for (int i = 0; i < setlist.tamanho; ++i)
    {
        auto* e = s->createNewChildElement ("S");
        e->setAttribute ("i", i);
        e->setAttribute ("scene", setlist.ordem[(size_t) i]);
    }
}

inline void lerSetlist (const juce::XmlElement& pai, Setlist& setlist)
{
    setlist = Setlist{};

    if (auto* s = pai.getChildByName ("SETLIST"))
    {
        setlist.nome = s->getStringAttribute ("name", setlist.nome)
                           .trim().substring (0, 64);
        if (setlist.nome.isEmpty())
            setlist.nome = "Culto atual";
        const int tamanho = juce::jlimit (0, Setlist::capacidadeMaxima,
                                          s->getIntAttribute ("size", 0));
        const int posicao = juce::jlimit (-1, Setlist::capacidadeMaxima,
                                          s->getIntAttribute ("pos", -1));

        for (int i = 0; i < tamanho; ++i)
            setlist.ordem[(size_t) i] = 0;

        for (auto* e : s->getChildWithTagNameIterator ("S"))
        {
            const int idx = e->getIntAttribute ("i", -1);

            if (! juce::isPositiveAndBelow (idx, tamanho))
                continue;

            const int cena = e->getIntAttribute ("scene", -1);

            if (! setlist.indiceValido (cena))
                continue;

            setlist.ordem[(size_t) idx] = cena;
        }

        setlist.tamanho = tamanho;
        setlist.posicao = juce::isPositiveAndBelow (posicao, tamanho) ? posicao : -1;
    }
}

//==============================================================================
// Resultado de um toque numa cena.
//
// Existe porque "nao aconteceu nada" e a pior resposta possivel no palco. Um
// bool nao distingue "essa cena esta vazia" de "espera, tem timbre
// carregando" — e sao respostas opostas: uma o musico resolve tocando noutra
// cena, a outra ele resolve esperando um segundo.
//==============================================================================
enum class Resultado
{
    ok,             // cena aplicada (ou gravada)
    vazia,          // slot nunca gravado — carregar nao faz nada de proposito
    carregando,     // ha timbre entrando; a aplicacao se reagenda sozinha
    naoGravou,      // capturou na memoria mas o disco recusou
    indisponivel,   // motor ainda nao existe (so na largada do app)
    paginaInvalida  // snapshot da pagina ausente, truncado ou com hash errado
};

inline bool precisaAvisarResultadoTerminal (Resultado resultado) noexcept
{
    // carregando ainda possui uma cadeia de retry; ok/vazia ja sao estados
    // explicados pela propria interface. Todo o resto terminou e precisa chegar
    // ao musico, inclusive a falha de persistir o indice ativo no fim do recall.
    return resultado != Resultado::ok
        && resultado != Resultado::vazia
        && resultado != Resultado::carregando;
}

inline bool deveBloquearGravacaoAposArquivoIlegivel (
    bool arquivoPrincipalIlegivel,
    bool recuperouBancoLegadoValido) noexcept
{
    // Sem um banco valido para migrar, abrir dez slots vazios e permitir a
    // proxima gravacao destruiria o arquivo original. O bloqueio so pode ser
    // levantado quando uma copia legada valida realmente foi recuperada.
    return arquivoPrincipalIlegivel && ! recuperouBancoLegadoValido;
}

// Snapshot minimo para uma alteracao que so pode existir se chegar ao disco.
// O chamador muda a cena em memoria, tenta a escrita atomica e entrega o
// resultado aqui. Falha restaura TANTO o slot quanto a referencia da cena no
// ar; assim a tela nunca confirma um estado que desaparecera no proximo boot.
struct EstadoAntesDePersistirCena
{
    Cena cena;
    int ativa = -1;
};

inline Resultado concluirPersistenciaDeCena (bool persistiu,
                                              const EstadoAntesDePersistirCena& antes,
                                              Cena& cenaAtual,
                                              int& ativaAtual)
{
    if (persistiu)
        return Resultado::ok;

    cenaAtual = antes.cena;
    ativaAtual = antes.ativa;
    return Resultado::naoGravou;
}

enum class OrigemDaGravacao
{
    botaoSalvar,
    gravarArmadoECena
};

inline bool manterGravarArmadoParaRetry (Resultado resultado,
                                         OrigemDaGravacao origem)
{
    // No fluxo SALVAR o proprio botao e o disparo. Ele volta ao repouso para
    // que o proximo toque seja inequivocamente outra tentativa. No fluxo
    // legado GRAVAR -> cena, uma falha mantem o modo armado e renova o prazo;
    // caso contrario o primeiro retry apenas desligaria o toggle, sem gravar.
    return origem == OrigemDaGravacao::gravarArmadoECena
        && resultado != Resultado::ok;
}

inline juce::String avisoDoResultado (Resultado r, int indice)
{
    switch (r)
    {
        // Escritas ja no caso final: o painel NAO aplica toUpperCase (ele nao
        // sobe letra acentuada e produzia "CENA 5 ESTá VAZIA").
        case Resultado::vazia:        return juce::String::fromUTF8 ("CENA ") + juce::String (indice + 1)
                                             + juce::String::fromUTF8 (" EST\xc3\x81 VAZIA");
        case Resultado::carregando:   return juce::String::fromUTF8 ("CARREGANDO TIMBRE\xe2\x80\xa6 J\xc3\x81 VAI");
        case Resultado::naoGravou:    return juce::String::fromUTF8 ("N\xc3\x83O GRAVOU NO DISCO");
        case Resultado::indisponivel: return juce::String::fromUTF8 ("MOTOR AINDA INICIANDO");
        case Resultado::paginaInvalida:
            return juce::String::fromUTF8 ("CENA ") + juce::String (indice + 1)
                 + juce::String::fromUTF8 (" \xe2\x80\x94 P\xc3\x81GINA DE SOM AUSENTE OU DANIFICADA");
        case Resultado::ok:           break;
    }

    return {};
}

inline juce::String avisoDaGravacao (Resultado resultado,
                                     OrigemDaGravacao origem,
                                     int indice)
{
    if (resultado == Resultado::carregando)
        return origem == OrigemDaGravacao::botaoSalvar
                 ? juce::String::fromUTF8 ("CARREGANDO TIMBRE\xe2\x80\xa6 TOQUE SALVAR DE NOVO")
                 : juce::String::fromUTF8 ("CARREGANDO TIMBRE\xe2\x80\xa6 TOQUE A CENA DE NOVO");

    if (resultado == Resultado::indisponivel)
        return origem == OrigemDaGravacao::botaoSalvar
                 ? juce::String::fromUTF8 ("MOTOR INICIANDO\xe2\x80\xa6 TOQUE SALVAR DE NOVO")
                 : juce::String::fromUTF8 ("MOTOR INICIANDO\xe2\x80\xa6 TOQUE A CENA DE NOVO");

    return avisoDoResultado (resultado, indice);
}

//==============================================================================
// Nome mostrado no botao. Cena vazia nao pode parecer cena gravada.
//==============================================================================
inline juce::String rotuloDaCena (const Cena& cena, int indice)
{
    if (! cena.preenchida)
        return juce::String (indice + 1);

    return cena.nome.isNotEmpty() ? cena.nome : juce::String (indice + 1);
}

} // namespace ChurchDAWLive
