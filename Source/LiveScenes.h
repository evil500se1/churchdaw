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

    // Cena vazia nao tem estado para guardar. Escrever os 14 canais dela era
    // gravar 140 elementos <L> de valores de fabrica que a leitura aditiva
    // ignoraria de qualquer jeito — puro peso no arquivo que e reescrito a
    // cada gravacao.
    if (! cena.preenchida)
        return;

    c->setAttribute ("name", cena.nome);
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
        l->setAttribute ("send", (double) canal.envioDeReverb);
        l->setAttribute ("timbre", canal.rotuloDoTimbre);
    }
}

inline void lerCena (const juce::XmlElement& c, Cena& cena)
{
    cena.preenchida = c.getIntAttribute ("filled", cena.preenchida ? 1 : 0) != 0;
    cena.nome       = c.getStringAttribute ("name", cena.nome);
    cena.gravadaEm  = c.getStringAttribute ("savedAt", cena.gravadaEm);

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
        canal.envioDeReverb  = (float) l->getDoubleAttribute ("send", (double) canal.envioDeReverb);
        canal.rotuloDoTimbre = l->getStringAttribute ("timbre", canal.rotuloDoTimbre);
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
