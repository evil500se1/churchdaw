#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// LIVE MODE — 10 cenas para trocar de musica no culto.
//
// Pedido de cliente (01/08/2026): "uns 10 programas pelo menos para salvar
// cenas".
//
// A DECISAO CENTRAL: a cena guarda ESTADO, nunca manda recarregar sample.
//
// Isso nao e limitacao, e o desenho certo. Sao 14 canais: o jeito de
// trabalhar e deixar os dois pianos carregados em canais diferentes e a cena
// ligar um e desligar o outro. Ligar/desligar canal e mudar volume sao
// escritas de parametro — instantaneas, e com a rampa do pos-fader elas nem
// cortam a cauda. Recarregar sample e a UNICA operacao lenta do app, e entre
// duas musicas o tecladista tem uns quatro segundos com o pastor falando: se
// a troca de cena pudesse recarregar timbre, uma cena errada silenciaria o
// culto sem chance de desfazer.
//
// A cena guarda tambem o RoTULO do timbre de cada canal, mas so para o painel
// poder avisar "esta cena foi gravada com Grand Piano no canal 1; hoje esta
// Rhodes". Avisar e diferente de trocar.
//==============================================================================
namespace ChurchDAWLive
{

inline constexpr int numeroDeCenas = 10;
inline constexpr int numeroDeCanais = 14;

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

    // Pad Continuo: o que mais muda de uma musica para a outra.
    int   padTom = -1;           // -1 = desligado
    float padVolume = 0.9f;
    float padCorte = 1.0f;
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

    // Cena vazia nao tem estado para guardar. Escrever os 14 canais dela era
    // gravar 140 elementos <L> de valores de fabrica que a leitura aditiva
    // ignoraria de qualquer jeito — puro peso no arquivo que e reescrito a
    // cada gravacao.
    if (! cena.preenchida)
        return;

    c->setAttribute ("name", cena.nome);
    c->setAttribute ("savedAt", cena.gravadaEm);

    auto* g = c->createNewChildElement ("LIVE");
    g->setAttribute ("gain", (double) cena.volumeGeral);
    g->setAttribute ("gtransp", cena.transposeGeral);
    g->setAttribute ("rvbreturn", (double) cena.retornoDeReverb);

    auto* m = c->createNewChildElement ("MASTER");
    m->setAttribute ("db", (double) cena.masterDb);

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
    indisponivel    // motor ainda nao existe (so na largada do app)
};

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
        case Resultado::ok:           break;
    }

    return {};
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
