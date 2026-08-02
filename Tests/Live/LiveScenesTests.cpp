#include "LiveScenes.h"

#include <cmath>
#include <iostream>

//==============================================================================
// LIVE MODE — regressoes da serializacao das cenas.
//
// O arquivo das cenas e a unica coisa do Live Mode que sobrevive ao domingo
// seguinte. Um erro aqui nao aparece no teste de tela: aparece no culto, com o
// musico tocando a cena 3 e ouvindo outra coisa.
//==============================================================================
namespace
{
bool perto (float a, float b, float tolerancia = 0.0001f)
{
    return std::abs (a - b) <= tolerancia;
}

int falha (const char* mensagem)
{
    std::cerr << "LIVE_SCENES_FAIL: " << mensagem << '\n';
    return 1;
}

ChurchDAWLive::Cena cenaDeExemplo()
{
    ChurchDAWLive::Cena c;
    c.preenchida = true;
    c.nome = "Santo Espirito";
    c.gravadaEm = "2026-08-01T03:00:00.000-03:00";

    c.volumeGeral = 1.2f;
    c.transposeGeral = -3;
    c.retornoDeReverb = 0.55f;
    c.masterDb = -4.5f;

    c.padTom = 7;
    c.padVolume = 1.35f;
    c.padCorte = 0.62f;

    for (int i = 0; i < ChurchDAWLive::numeroDeCanais; ++i)
    {
        auto& canal = c.canais[i];
        canal.ligado = (i % 3) != 0;
        canal.mudo = (i % 4) == 0;
        canal.solo = false;
        canal.volume = 0.25f + 0.05f * (float) i;
        canal.transpose = (i % 5) - 2;
        canal.envioDeReverb = 0.1f * (float) (i % 7);
        canal.rotuloDoTimbre = "Timbre " + juce::String (i);
    }

    return c;
}
}

int main()
{
    // ------------------------------------------------------------------
    // 1. Ida e volta completa.
    // ------------------------------------------------------------------
    {
        const auto original = cenaDeExemplo();

        juce::XmlElement raiz ("LIVESCENES");
        ChurchDAWLive::escreverCena (raiz, original, 2);

        auto* no = raiz.getChildByName ("SCENE");

        if (no == nullptr)
            return falha ("escreverCena nao criou o elemento SCENE");

        if (no->getIntAttribute ("index", -1) != 2)
            return falha ("o indice da cena nao sobreviveu");

        ChurchDAWLive::Cena lida;
        ChurchDAWLive::lerCena (*no, lida);

        if (! lida.preenchida || lida.nome != original.nome
             || lida.gravadaEm != original.gravadaEm)
            return falha ("identificacao da cena nao sobreviveu");

        if (! perto (lida.volumeGeral, original.volumeGeral)
             || lida.transposeGeral != original.transposeGeral
             || ! perto (lida.retornoDeReverb, original.retornoDeReverb))
            return falha ("globais do Church Live nao sobreviveram");

        if (! perto (lida.masterDb, original.masterDb))
            return falha ("master nao sobreviveu");

        if (lida.padTom != original.padTom
             || ! perto (lida.padVolume, original.padVolume)
             || ! perto (lida.padCorte, original.padCorte))
            return falha ("estado do Pad nao sobreviveu");

        for (int i = 0; i < ChurchDAWLive::numeroDeCanais; ++i)
        {
            const auto& a = original.canais[i];
            const auto& b = lida.canais[i];

            if (a.ligado != b.ligado || a.mudo != b.mudo || a.solo != b.solo
                 || ! perto (a.volume, b.volume)
                 || a.transpose != b.transpose
                 || ! perto (a.envioDeReverb, b.envioDeReverb)
                 || a.rotuloDoTimbre != b.rotuloDoTimbre)
                return falha ("um canal nao sobreviveu a ida e volta");
        }
    }

    // ------------------------------------------------------------------
    // 2. A leitura e ADITIVA: atributo ausente nao pode zerar o que ja
    //    estava na cena. E o que permite uma cena gravada numa versao antiga
    //    ser lida por uma nova sem perder parametro que ela nem conhecia.
    // ------------------------------------------------------------------
    {
        auto cena = cenaDeExemplo();
        const auto antes = cena;

        juce::XmlElement magro ("SCENE");
        magro.setAttribute ("index", 0);
        magro.setAttribute ("filled", 1);

        ChurchDAWLive::lerCena (magro, cena);

        if (! perto (cena.volumeGeral, antes.volumeGeral)
             || cena.transposeGeral != antes.transposeGeral
             || ! perto (cena.masterDb, antes.masterDb)
             || cena.padTom != antes.padTom
             || ! perto (cena.padCorte, antes.padCorte))
            return falha ("leitura de XML magro apagou valores existentes");

        for (int i = 0; i < ChurchDAWLive::numeroDeCanais; ++i)
            if (cena.canais[i].ligado != antes.canais[i].ligado
                 || ! perto (cena.canais[i].volume, antes.canais[i].volume))
                return falha ("leitura de XML magro apagou canais existentes");
    }

    // ------------------------------------------------------------------
    // 3. O DEFAULT DO VOLUME esta no dominio NORMALIZADO.
    //
    //    Regressao real: o campo vale 0..1 normalizado, mas o default era
    //    1.0f — que normalizado significa ganho 1,5, ou +3,5 dB. Como a
    //    leitura e aditiva, qualquer canal sem o atributo "vol" entrava no
    //    culto 3,5 dB acima do resto.
    // ------------------------------------------------------------------
    {
        const ChurchDAWLive::CanalDaCena novo;

        if (! perto (novo.volume, 1.0f / 1.5f))
            return falha ("default do volume do canal saiu da unidade "
                          "(1,0 de ganho = 0,667 normalizado)");
    }

    // ------------------------------------------------------------------
    // 4. Valores absurdos vindos de arquivo editado a mao sao grampeados.
    // ------------------------------------------------------------------
    {
        juce::XmlElement doente ("SCENE");
        doente.setAttribute ("filled", 1);

        auto* p = doente.createNewChildElement ("PAD");
        p->setAttribute ("key", 99);

        auto* l = doente.createNewChildElement ("L");
        l->setAttribute ("i", 0);
        l->setAttribute ("transp", 500);

        auto* fora = doente.createNewChildElement ("L");
        fora->setAttribute ("i", ChurchDAWLive::numeroDeCanais + 40);
        fora->setAttribute ("vol", 0.5);

        ChurchDAWLive::Cena cena;
        ChurchDAWLive::lerCena (doente, cena);   // nao pode estourar memoria

        if (cena.padTom > 11 || cena.padTom < -1)
            return falha ("tom do Pad fora da faixa nao foi grampeado");

        if (cena.canais[0].transpose > 24 || cena.canais[0].transpose < -24)
            return falha ("transpose de canal fora da faixa nao foi grampeado");
    }

    // ------------------------------------------------------------------
    // 5. Cena vazia nao carrega estado — e nao pode parecer cena gravada.
    // ------------------------------------------------------------------
    {
        ChurchDAWLive::Cena vazia;

        juce::XmlElement raiz ("LIVESCENES");
        ChurchDAWLive::escreverCena (raiz, vazia, 5);

        auto* no = raiz.getChildByName ("SCENE");

        if (no == nullptr)
            return falha ("cena vazia precisa existir no arquivo, so que sem estado");

        if (no->getChildByName ("L") != nullptr || no->getChildByName ("PAD") != nullptr)
            return falha ("cena vazia gravou estado que ninguem vai ler");

        if (ChurchDAWLive::rotuloDaCena (vazia, 5) != "6")
            return falha ("cena vazia precisa mostrar o numero do slot");

        ChurchDAWLive::Cena comNome;
        comNome.preenchida = true;
        comNome.nome = "Aleluia";

        if (ChurchDAWLive::rotuloDaCena (comNome, 0) != "Aleluia")
            return falha ("cena gravada precisa mostrar o proprio nome");
    }

    // ------------------------------------------------------------------
    // 6. Cada resultado de toque tem uma explicacao propria. "Nao aconteceu
    //    nada" e a pior resposta possivel no palco.
    // ------------------------------------------------------------------
    {
        using R = ChurchDAWLive::Resultado;

        if (ChurchDAWLive::avisoDoResultado (R::ok, 0).isNotEmpty())
            return falha ("sucesso nao deve virar aviso na tela");

        // TODOS os resultados de falha, sem excecao. Esta lista precisa crescer
        // junto com o enum: um valor novo sem mensagem volta a ser exatamente o
        // "nao aconteceu nada" que o enum foi criado para eliminar.
        for (const auto r : { R::vazia, R::carregando, R::naoGravou,
                              R::indisponivel, R::paginaInvalida })
            if (ChurchDAWLive::avisoDoResultado (r, 2).isEmpty())
                return falha ("falha silenciosa: resultado sem explicacao");

        // Guarda contra esquecer de atualizar a lista acima: se alguem
        // acrescentar um valor no fim do enum, este contador denuncia.
        if ((int) R::paginaInvalida != 5)
            return falha ("o enum Resultado mudou — atualize a lista de avisos deste teste");

        if (! ChurchDAWLive::avisoDoResultado (R::vazia, 2).contains ("3"))
            return falha ("o aviso de cena vazia precisa dizer QUAL cena");
    }

    // ------------------------------------------------------------------
    // 7. Cena ANTIGA (sem os blocos LIVE/MASTER) nao pode se passar por cena
    //    que gravou zeros.
    //
    //    Regressao real: ao passar a APLICAR as globais, uma cena gravada por
    //    versao anterior zerava o transpose global e jogava o reverb para o
    //    valor de fabrica no meio do louvor — porque "ausente" e "gravado
    //    como padrao" eram indistinguiveis na hora de aplicar.
    // ------------------------------------------------------------------
    {
        juce::XmlElement antiga ("SCENE");
        antiga.setAttribute ("index", 0);
        antiga.setAttribute ("filled", 1);

        auto* l = antiga.createNewChildElement ("L");
        l->setAttribute ("i", 0);
        l->setAttribute ("on", 1);

        ChurchDAWLive::Cena cena;
        ChurchDAWLive::lerCena (antiga, cena);

        if (cena.temGlobais)
            return falha ("cena sem o bloco LIVE nao pode dizer que tem globais");

        if (cena.temMaster)
            return falha ("cena sem o bloco MASTER nao pode dizer que tem master");

        // E a cena nova tem de marcar os dois, senao ninguem aplica nada.
        auto nova = cenaDeExemplo();
        nova.temGlobais = true;
        nova.temMaster = true;

        juce::XmlElement raiz ("LIVESCENES");
        ChurchDAWLive::escreverCena (raiz, nova, 0);

        ChurchDAWLive::Cena volta;
        ChurchDAWLive::lerCena (*raiz.getChildByName ("SCENE"), volta);

        if (! volta.temGlobais || ! volta.temMaster)
            return falha ("cena gravada agora precisa marcar globais e master na volta");
    }

    // ------------------------------------------------------------------
    // 8. Fluxo pedido pelo cliente: uma cena vazia vira DESTINO de edicao,
    //    nao cena salva e nao gravacao acidental. Preparar o motor vazio e
    //    responsabilidade do callback do Main; SALVAR continua o segundo gesto.
    // ------------------------------------------------------------------
    {
        using A = ChurchDAWLive::AcaoDeCena;

        std::array<bool, ChurchDAWLive::numeroDeCenas> preenchidas {};
        preenchidas[0] = true;

        ChurchDAWLive::InteracaoDeCenas interacao;
        int ativa = 0;   // pertence ao motor; a interacao nao pode muda-la

        interacao.sincronizarComAtiva (ativa);

        if (interacao.selecionada != 0)
            return falha ("selecao inicial nao acompanhou a cena ativa");

        if (interacao.tocar (1, false, false) != A::prepararNova)
            return falha ("tocar cena vazia nao preparou uma nova cena");

        if (interacao.selecionada != 1 || ativa != 0 || preenchidas[1])
            return falha ("selecionar cena vazia alterou a cena no ar ou gravou sozinha");

        if (interacao.destinoNovoAoPressionarGravar (false, preenchidas) != -1)
            return falha ("cena vazia gravaria sem o segundo gesto em SALVAR");

        if (interacao.destinoNovoAoPressionarGravar (true, preenchidas) != 1)
            return falha ("SALVAR nao encontrou a cena vazia selecionada");

        preenchidas[1] = true;

        if (interacao.tocar (1, true, false) != A::carregar)
            return falha ("cena preenchida deixou de carregar com um toque");

        if (interacao.tocar (2, false, true) != A::gravar)
            return falha ("fluxo antigo GRAVAR e tocar slot deixou de funcionar");

        interacao.cenaFoiApagada (2, ativa);

        if (interacao.selecionada != ativa)
            return falha ("apagar a selecionada nao devolveu selecao para a cena no ar");

        if (interacao.tocar (-1, false, false) != A::nenhuma
             || interacao.selecionada != ativa)
            return falha ("indice invalido corrompeu a selecao de cenas");
    }

    // ------------------------------------------------------------------
    // 9. Duas cenas sao independentes no banco e no XML. Gravar ou limpar
    //    a Cena 2 nao pode contaminar a Cena 1 que o cliente ja salvou.
    // ------------------------------------------------------------------
    {
        std::array<ChurchDAWLive::Cena, ChurchDAWLive::numeroDeCenas> banco {};
        banco[0] = cenaDeExemplo();
        banco[0].nome = "Cena 1";
        banco[0].padTom = 2;
        banco[0].canais[0].volume = 0.31f;
        banco[0].estadoDoMotorId = juce::String::repeatedString ("a", 64);

        banco[1] = cenaDeExemplo();
        banco[1].nome = "Cena 2";
        banco[1].padTom = 9;
        banco[1].canais[0].volume = 0.87f;
        banco[1].estadoDoMotorId = juce::String::repeatedString ("b", 64);

        juce::XmlElement raiz ("LIVESCENES");

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            ChurchDAWLive::escreverCena (raiz, banco[(size_t) i], i);

        std::array<ChurchDAWLive::Cena, ChurchDAWLive::numeroDeCenas> volta {};
        int quantidade = 0;

        for (auto* no : raiz.getChildWithTagNameIterator ("SCENE"))
        {
            const int i = no->getIntAttribute ("index", -1);

            if (! juce::isPositiveAndBelow (i, ChurchDAWLive::numeroDeCenas))
                return falha ("banco serializou indice de cena invalido");

            ChurchDAWLive::lerCena (*no, volta[(size_t) i]);
            ++quantidade;
        }

        if (quantidade != ChurchDAWLive::numeroDeCenas)
            return falha ("banco nao preservou os dez slots de cena");

        if (! volta[0].preenchida || ! volta[1].preenchida
             || volta[0].nome != "Cena 1" || volta[1].nome != "Cena 2"
             || volta[0].estadoDoMotorId != banco[0].estadoDoMotorId
             || volta[1].estadoDoMotorId != banco[1].estadoDoMotorId
             || volta[0].padTom != 2 || volta[1].padTom != 9
             || ! perto (volta[0].canais[0].volume, 0.31f)
             || ! perto (volta[1].canais[0].volume, 0.87f))
            return falha ("duas cenas se contaminaram na ida e volta do banco");

        const auto cena1Antes = banco[0];
        banco[1] = ChurchDAWLive::Cena{};

        juce::XmlElement depoisDeLimpar ("LIVESCENES");
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            ChurchDAWLive::escreverCena (depoisDeLimpar, banco[(size_t) i], i);

        auto* cena1Xml = depoisDeLimpar.getChildElement (0);
        auto* cena2Xml = depoisDeLimpar.getChildElement (1);

        if (cena1Xml == nullptr || cena2Xml == nullptr
             || cena1Xml->getIntAttribute ("filled", 0) != 1
             || cena2Xml->getIntAttribute ("filled", 1) != 0
             || cena2Xml->getNumChildElements() != 0)
            return falha ("limpar Cena 2 alterou a estrutura de outro slot");

        if (cena1Xml->getChildByName ("ENGINE") == nullptr)
            return falha ("Cena 1 perdeu a referencia da pagina de audio");

        if (banco[0].nome != cena1Antes.nome
             || banco[0].padTom != cena1Antes.padTom
             || ! perto (banco[0].canais[0].volume, cena1Antes.canais[0].volume))
            return falha ("limpar Cena 2 alterou a Cena 1 em memoria");

        if (! ChurchDAWLive::idDeEstadoDoMotorValido (
                juce::String::repeatedString ("F", 64))
             || ChurchDAWLive::idDeEstadoDoMotorValido ("../../setup")
             || ChurchDAWLive::idDeEstadoDoMotorValido (
                    juce::String::repeatedString ("g", 64)))
            return falha ("validacao do ID de pagina aceita caminho/hash invalido");
    }

    // ------------------------------------------------------------------
    // 10. LIMPAR escolhe uma cena preenchida sem carrega-la. A cena ativa
    //     pertence ao motor e nao pode mudar so porque o usuario apontou o
    //     slot inativo que deseja apagar.
    // ------------------------------------------------------------------
    {
        using A = ChurchDAWLive::AcaoDeCena;

        ChurchDAWLive::InteracaoDeCenas interacao;
        int ativa = 0;
        interacao.sincronizarComAtiva (ativa);

        if (interacao.tocar (4, true, false, true) != A::apagar)
            return falha ("LIMPAR nao escolheu uma cena preenchida inativa");

        if (interacao.selecionada != 4 || ativa != 0)
            return falha ("escolher cena para limpar mudou a cena no ar");

        if (interacao.tocar (5, false, false, true) != A::nenhuma
             || interacao.selecionada != 5 || ativa != 0)
            return falha ("LIMPAR em slot vazio carregou audio ou perdeu a selecao");

        interacao.selecionarSemAcionar (7);

        if (interacao.selecionada != 7 || ativa != 0)
            return falha ("selecao administrativa acionou a cena inativa");
    }

    // ------------------------------------------------------------------
    // 11. Persistencia e transacional: sucesso conserva o novo estado;
    //     falha devolve slot E cena ativa ao snapshot anterior. Assim a tela
    //     nunca chama de salva uma cena que so existe ate fechar o app.
    // ------------------------------------------------------------------
    {
        using R = ChurchDAWLive::Resultado;

        auto anterior = cenaDeExemplo();
        anterior.nome = "Antes";
        anterior.padTom = 2;
        ChurchDAWLive::EstadoAntesDePersistirCena snapshot { anterior, 0 };

        auto atual = cenaDeExemplo();
        atual.nome = "Depois";
        atual.padTom = 9;
        int ativa = 1;

        if (ChurchDAWLive::concluirPersistenciaDeCena (
                true, snapshot, atual, ativa) != R::ok
             || atual.nome != "Depois" || atual.padTom != 9 || ativa != 1)
            return falha ("sucesso de persistencia reverteu a cena nova");

        atual.nome = "Nao deve sobreviver";
        atual.padTom = 11;
        ativa = 6;

        if (ChurchDAWLive::concluirPersistenciaDeCena (
                false, snapshot, atual, ativa) != R::naoGravou)
            return falha ("falha de persistencia foi reportada como sucesso");

        if (atual.nome != "Antes" || atual.padTom != 2 || ativa != 0)
            return falha ("falha de persistencia nao fez rollback completo");
    }

    // ------------------------------------------------------------------
    // 12. Retry coerente nos dois fluxos. No SALVAR cada toque e uma tentativa
    //     completa; no legado GRAVAR -> cena, uma falha continua armada para o
    //     proximo toque nao virar apenas um cancelamento silencioso.
    // ------------------------------------------------------------------
    {
        using O = ChurchDAWLive::OrigemDaGravacao;
        using R = ChurchDAWLive::Resultado;

        for (const auto resultado : { R::vazia, R::carregando, R::naoGravou,
                                      R::indisponivel, R::paginaInvalida })
        {
            if (ChurchDAWLive::manterGravarArmadoParaRetry (
                    resultado, O::botaoSalvar))
                return falha ("SALVAR deixou toggle armado e primeiro retry so cancelaria");

            if (! ChurchDAWLive::manterGravarArmadoParaRetry (
                    resultado, O::gravarArmadoECena))
                return falha ("fluxo legado perdeu o modo armado depois de falha");
        }

        if (! ChurchDAWLive::avisoDaGravacao (
                R::carregando, O::botaoSalvar, 1).containsIgnoreCase ("SALVAR")
             || ! ChurchDAWLive::avisoDaGravacao (
                R::carregando, O::gravarArmadoECena, 1).containsIgnoreCase ("CENA")
             || ! ChurchDAWLive::avisoDaGravacao (
                R::indisponivel, O::botaoSalvar, 1).containsIgnoreCase ("SALVAR")
             || ! ChurchDAWLive::avisoDaGravacao (
                R::indisponivel, O::gravarArmadoECena, 1).containsIgnoreCase ("CENA"))
            return falha ("aviso de retry manda tocar no controle errado");

        if (ChurchDAWLive::manterGravarArmadoParaRetry (
                R::ok, O::botaoSalvar)
             || ChurchDAWLive::manterGravarArmadoParaRetry (
                R::ok, O::gravarArmadoECena))
            return falha ("gravacao concluida continuou armada");
    }

    // ------------------------------------------------------------------
    // 13. O primeiro pedido preempta uma restauracao antiga; somente a pagina
    //     que ja foi enviada deve aguardar o proprio carregamento. O mesmo
    //     helper fixa o contrato simetrico de tamanho entre write e read.
    // ------------------------------------------------------------------
    {
        for (int pagina = 0; pagina <= 1; ++pagina)
            for (int busy = 0; busy <= 1; ++busy)
                for (int pending = 0; pending <= 1; ++pending)
                {
                    const bool esperado = pagina != 0
                                       && (busy != 0 || pending != 0);

                    if (ChurchDAWLive::deveAguardarCena (
                            pagina != 0, busy != 0, pending != 0) != esperado)
                        return falha ("matriz de preempcao/espera de cena divergiu");
                }

        const auto maximo = ChurchDAWLive::tamanhoMaximoPaginaDeCena;

        if (ChurchDAWLive::paginaDeCenaTemTamanhoValido (0)
             || ! ChurchDAWLive::paginaDeCenaTemTamanhoValido (maximo)
             || ChurchDAWLive::paginaDeCenaTemTamanhoValido (maximo + 1))
            return falha ("write/read discordam no limite da pagina de cena");
    }

    // ------------------------------------------------------------------
    // 14. Startup: o chain global pode estar atrasado depois de uma queda.
    //     Cena v1 ativa precisa ser aplicada ANTES de a pagina ser capturada;
    //     senao o chain A sobrescreve silenciosamente a cena B.
    // ------------------------------------------------------------------
    {
        struct EstadoAudivel
        {
            float volume = 0.0f;
            int pad = -1;
        };

        const EstadoAudivel chainA { 0.18f, 2 };
        const EstadoAudivel cenaB  { 0.84f, 9 };
        auto motor = chainA;
        auto cenaPersistida = cenaB;
        EstadoAudivel paginaAnexada;
        bool anexouPagina = false;

        const auto plano = ChurchDAWLive::planejarReconciliacaoNoStartup (
            true, false);

        if (plano.aplicarPaginaPorSha
             || ! plano.aplicarCenaAntesDeAnexarPagina
             || ! plano.anexarPaginaDepoisDeAplicar)
            return falha ("startup legado nao exigiu aplicar cena antes de anexar SHA");

        if (plano.aplicarCenaAntesDeAnexarPagina)
            motor = cenaPersistida;

        if (plano.anexarPaginaDepoisDeAplicar)
        {
            paginaAnexada = motor;
            anexouPagina = true;
        }

        if (! anexouPagina
             || ! perto (motor.volume, cenaB.volume) || motor.pad != cenaB.pad
             || ! perto (paginaAnexada.volume, cenaB.volume)
             || paginaAnexada.pad != cenaB.pad
             || ! perto (cenaPersistida.volume, cenaB.volume)
             || cenaPersistida.pad != cenaB.pad)
            return falha ("startup capturou chain A por cima da cena legada B");

        const auto planoV2 = ChurchDAWLive::planejarReconciliacaoNoStartup (
            true, true);

        if (! planoV2.aplicarPaginaPorSha
             || planoV2.aplicarCenaAntesDeAnexarPagina
             || planoV2.anexarPaginaDepoisDeAplicar)
            return falha ("startup v2 deixou de escolher a pagina persistida por SHA");

        const auto planoVazio = ChurchDAWLive::planejarReconciliacaoNoStartup (
            false, false);

        if (planoVazio.aplicarPaginaPorSha
             || planoVazio.aplicarCenaAntesDeAnexarPagina
             || planoVazio.anexarPaginaDepoisDeAplicar)
            return falha ("startup planejou reconciliacao para slot vazio");
    }

    // ------------------------------------------------------------------
    // 15. Supressao geracional do autosave pesado durante recall de cena.
    //     Invalida timer velho, bloqueia callbacks da cena, uma soltura velha
    //     nao libera a nova, e o primeiro ajuste posterior volta a agendar.
    // ------------------------------------------------------------------
    {
        ChurchDAWLive::ControleDeGravacaoDoSetupDuranteCena controle;
        int geracaoDaCena = 0;

        const auto timerAnterior = controle.agendarSePermitido (geracaoDaCena);

        if (timerAnterior == 0
             || ! controle.timerAindaValido (timerAnterior, geracaoDaCena))
            return falha ("autosave normal nao gerou timer valido");

        geracaoDaCena = 1;
        controle.iniciarSupressao (geracaoDaCena);

        if (controle.timerAindaValido (timerAnterior, geracaoDaCena))
            return falha ("inicio da cena nao invalidou timer anterior");

        if (controle.agendarSePermitido (geracaoDaCena) != 0)
            return falha ("callback produzido pela cena armou autosave");

        geracaoDaCena = 2;
        controle.iniciarSupressao (geracaoDaCena);

        if (controle.encerrarSupressao (1, geracaoDaCena)
             || ! controle.estaSuprimida (geracaoDaCena))
            return falha ("soltura stale da cena A liberou a cena B");

        if (! controle.encerrarSupressao (2, geracaoDaCena)
             || controle.estaSuprimida (geracaoDaCena))
            return falha ("geracao corrente nao encerrou a supressao");

        const auto primeiroAjuste =
            controle.agendarSePermitido (geracaoDaCena);

        if (primeiroAjuste == 0
             || ! controle.timerAindaValido (primeiroAjuste, geracaoDaCena))
            return falha ("primeiro ajuste depois da cena nao rearmou autosave");
    }

    // ------------------------------------------------------------------
    // 16. Os layouts efetivamente usados por celular retrato/paisagem e
    //     desktop compacto preservam 44 px em cenas, GRAVAR e LIMPAR. O teste
    //     recebe a area interna real, depois das margens do MainComponent e do
    //     painel, para nao validar uma geometria ideal que nunca chega a tela.
    // ------------------------------------------------------------------
    {
        struct Caso { int largura; int altura; const char* nome; };
        const Caso casos[] {
            { 288, 90, "telefone 320 dp retrato" },
            { 328, 90, "telefone 360 dp retrato" },
            { 692, 44, "telefone 720 dp paisagem curta" },
            { 914, 46, "desktop compacto 960" }
        };

        for (const auto& caso : casos)
        {
            const auto layout = ChurchDAWLive::calcularLayoutDaFaixaDeCenas (
                caso.largura, caso.altura);

            if (! layout.suportaAlvosDe44)
            {
                std::cerr << "LIVE_SCENES_LAYOUT_FAIL: " << caso.nome
                          << " cena=" << layout.larguraMinimaDaCena << 'x'
                          << layout.alturaDaFileira
                          << " gravar=" << layout.larguraDoGravar << 'x'
                          << layout.alturaMinimaDaAcao
                          << " limpar=" << layout.larguraDoLimpar << 'x'
                          << layout.alturaMinimaDaAcao << '\n';
                return 1;
            }
        }
    }

    std::cout << "LIVE_SCENES_OK\n";
    return 0;
}
