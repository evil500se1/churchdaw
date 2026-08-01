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
        for (const auto r : { R::vazia, R::carregando, R::naoGravou, R::indisponivel })
            if (ChurchDAWLive::avisoDoResultado (r, 2).isEmpty())
                return falha ("falha silenciosa: resultado sem explicacao");

        // Guarda contra esquecer de atualizar a lista acima: se alguem
        // acrescentar um valor no fim do enum, este contador denuncia.
        if ((int) R::indisponivel != 4)
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

    std::cout << "LIVE_SCENES_OK\n";
    return 0;
}
