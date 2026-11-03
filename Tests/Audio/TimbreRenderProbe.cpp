// =============================================================================
// SONDA OFFLINE DE RENDER DE TIMBRE -- ChurchDAWTimbreRenderProbe
// =============================================================================
//
// POR QUE ISTO EXISTE
// -------------------
// "Soa metalico" nao e uma medicao. Sem um jeito de fazer o MESMO motor que
// toca no culto produzir um arquivo, toda investigacao de ruido vira palpite:
// o que se ouve na caixa passou por driver, buffer, reverb, limiter e pela
// propria sala. Esta sonda instancia o TimbresProcessor de verdade, carrega um
// timbre de verdade da biblioteca do usuario, dispara uma nota e grava o
// resultado em WAV -- SEM interface, SEM placa de som e mais rapido que o
// tempo real. O que sai daqui pode ser comparado amostra a amostra com o WAV de
// origem.
//
// Ela NAO entra no CTest: depende da biblioteca instalada na maquina e serve
// para diagnostico manual, como a ChurchDAWTimbreLoadStressTests.
//
// O QUE ELA MEDE SOZINHA
// ----------------------
//   pico / RMS / DC / fator de crista        -- prova que ha audio, nao silencio
//   amostras nao-finitas                     -- NaN/inf mascarados
//   buracos de silencio no meio da nota      -- clique por janela de streaming
//                                               que nao chegou (ver
//                                               TimbresProcessor.h:10440)
//   envelope de RMS por fatia                -- decaimento, pulsacao periodica
//   RMS DA BANDA ALTA por fatia              -- "chiado" vira numero (ver abaixo)
//   vozes ativas por fatia                   -- correlaciona ruido com polifonia
//   roubo de voz / falha de stream / NaN     -- contadores do proprio motor
//
// MODO --zones
// ------------
// Le o metadado de loop de CADA arquivo do timbre sem tocar nada. O motor de
// timbres ignora esses marcadores; o modo existe apenas para inspecionar arquivos
// antigos e comparar o WAV de origem com renders anteriores.
//
// CENARIO DE PEDAL / ACORDE / REPETICAO
// -------------------------------------
// A queixa "com o pedal de sustain, no fim do som fica um xiado" NAO se
// reproduz com uma nota so: ela depende de MUITAS vozes vivas ao mesmo tempo,
// que e exatamente o que o pedal produz (noteOff com pedal pisado nao libera a
// voz, apenas marca heldByPedal -- TimbresProcessor.h:9340). Por isso a sonda
// deixou de tocar uma nota e passou a tocar uma SEQUENCIA de eventos:
//
//   --pedal 1              CC64 pisado antes da primeira nota
//   --pedal-off <seg>      CC64 solto neste instante (releasePedalHeldVoices
//                          solta TODAS as vozes no mesmo sample-frame)
//   --chord 60,64,67,72    varias notas ao mesmo tempo
//   --repeat <n> <int>     repete a nota/acorde n vezes a cada <int> segundos,
//                          que e como as vozes se acumulam
//   --sequence <arquivo>   script livre de eventos, para reproduzir uma frase
//
// COMO O "CHIADO" VIRA NUMERO
// ---------------------------
// Chiado e energia de banda ALTA que sobra quando o sinal principal ja decaiu.
// A sonda mede, por fatia de tempo, o RMS acima de --hf (padrao 5 kHz) alem do
// RMS total, e imprime as duas series lado a lado com a contagem de vozes. Se o
// defeito for soma de caudas (ruido de fundo dos samples somado), a banda alta
// sobe JUNTO com o numero de vozes e desce quando elas morrem. Se for roubo de
// voz, ela salta nos instantes em que o contador de roubo anda. Sao hipoteses
// diferentes com assinaturas diferentes -- e agora distinguiveis.
//
// CUIDADO COM O STREAMING (--pace)
// --------------------------------
// Samples longos continuam no disco (CHURCHDAW_STREAM_SAMPLES=1). Renderizar
// mais rapido que o tempo real pode passar na frente da thread leitora e produzir
// silencio de um bloco -- que e um defeito DA SONDA, nao do motor. Por isso o
// relatorio conta os buracos, e --pace 1 renderiza no ritmo do tempo
// real quando for preciso descartar essa duvida.
//
// USO
// ---
//   ChurchDAWTimbreRenderProbe --list
//   ChurchDAWTimbreRenderProbe --timbre "Prophetic Ambience" --note 60 --vel 100 \
//                              --hold 4 --tail 3 --out D:\saida.wav
//   ChurchDAWTimbreRenderProbe --timbre "Prophetic Ambience" --zones
//   ChurchDAWTimbreRenderProbe --folder "C:\...\Imported" --prefix "10Pad1" ...
//   ChurchDAWTimbreRenderProbe --timbre "Astoria Grand" --chord 60,64,67,72 \
//                              --pedal 1 --repeat 8 1.0 --hold 0.6 \
//                              --pedal-off 9 --tail 8 --out D:\pedal.wav
// =============================================================================

#include "../../Source/TimbresProcessor.h"
#include "../../Source/MasterProcessor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace
{

//==============================================================================
struct Options
{
    bool list = false;
    bool zones = false;
    bool map = false;
    juce::String timbre;
    juce::String library;
    juce::String folder;
    juce::String prefix;
    juce::String out;
    juce::String sequence;
    int note = 60;
    int vel = 100;
    int layer = 0;
    double hold = 4.0;
    double tail = 3.0;
    double sampleRate = 48000.0;
    int blockSize = 256;
    int bits = 32;              // 32 = float IEEE; 24 = PCM
    float reverbSend = 0.0f;    // 0 = motor seco, sem o reverb nativo no caminho
    bool reverbSet = false;
    bool factory = false;       // reproduz a escolha pela interface
    double pace = 0.0;          // 0 = o mais rapido possivel; 1 = tempo real
    int transpose = 0;
    float masterDb = 0.0f;
    bool masterSet = false;
    bool punch = false;
    bool punchSet = false;

    // -- cenario do pedal ---------------------------------------------------
    bool pedal = false;         // pisa o CC64 antes da primeira nota
    double pedalOff = -1.0;     // < 0 = nunca solta dentro do render
    std::vector<int> chord;     // vazio = usa --note
    int repeat = 1;
    double repeatInterval = 0.0;

    // -- analise ------------------------------------------------------------
    double hfHz = 5000.0;       // corte da banda usada para medir "chiado"
    double slice = 0.25;        // fatia de tempo do relatorio, em segundos
};

// Duracao maxima do render. 300 s a 48 kHz estereo em float sao ~115 MB de
// buffer -- acima disso a sonda passa a medir a paciencia de quem espera.
constexpr double duracaoMaximaSegundos = 300.0;

int erro (const juce::String& mensagem)
{
    std::cerr << "TIMBRE_RENDER_PROBE_FAIL: " << mensagem << '\n';
    return 1;
}

void uso()
{
    std::cout
        << "Sonda offline de render do motor de samples do ChurchDAW.\n\n"
        << "  --list                 lista as bibliotecas/timbres encontrados e sai\n"
        << "  --timbre <texto>       escolhe o timbre pelo rotulo (igual ou contendo)\n"
        << "  --library <texto>      restringe a busca a uma biblioteca\n"
        << "  --folder <pasta>       monta o timbre a mao a partir de uma pasta\n"
        << "  --prefix <texto>       prefixo dos arquivos dentro de --folder\n"
        << "  --zones                relata marcadores de loop IGNORADOS pelo motor e sai\n"
        << "  --map                  carrega e varre o mapa efetivo das 128 notas\n"
        << "  --note <0..127>        nota MIDI (padrao 60)\n"
        << "  --vel <1..127>         velocity (padrao 100)\n"
        << "  --transpose <-24..24>  transposicao do canal em semitons (padrao 0)\n"
        << "  --hold <segundos>      tempo com a tecla apertada (padrao 4)\n"
        << "  --tail <segundos>      tempo de cauda depois do ultimo evento (padrao 3)\n"
        << "  --sr <taxa>            taxa de amostragem (padrao 48000)\n"
        << "  --block <amostras>     tamanho do bloco (padrao 256)\n"
        << "  --layer <0..13>        canal do motor (padrao 0)\n"
        << "  --factory              reproduz a selecao pela interface: perfil do\n"
        << "                         sidecar ou controles neutros (sem FX escondido)\n"
        << "  --reverb <0..1>        envio do reverb nativo do canal (vence --factory)\n"
        << "  --bits <24|32>         24 = PCM; 32 = float IEEE (padrao)\n"
        << "  --pace <fator>         0 = o mais rapido possivel (padrao); 1 = tempo real\n"
        << "  --master-db <-60..10>  passa o render pelo master neste ganho\n"
        << "  --punch <0|1>          liga/desliga PESO no master (ativa o master)\n"
        << "  --out <arquivo.wav>    destino do WAV\n"
        << '\n'
        << "CENARIO DE PEDAL / ACORDE (a queixa do chiado nao aparece com 1 nota)\n"
        << "  --pedal <0|1>          pisa o sustain (CC64) antes da primeira nota\n"
        << "  --pedal-off <seg>      solta o sustain neste instante, contado do inicio\n"
        << "  --chord <n1,n2,...>    toca varias notas ao mesmo tempo (vence --note)\n"
        << "  --repeat <n> <interv>  repete a nota/acorde n vezes a cada <interv> s\n"
        << "                         (com o pedal pisado, e assim que as vozes somam)\n"
        << "  --sequence <arquivo>   script de eventos; vence todas as opcoes acima.\n"
        << "                         Um evento por linha (ou separados por \"/\"):\n"
        << "                           <seg> on <nota> [vel]\n"
        << "                           <seg> off <nota>\n"
        << "                           <seg> pedal <0|1>\n"
        << "                         \"#\" comenta a linha inteira.\n"
        << '\n'
        << "ANALISE\n"
        << "  --hf <Hz>              corte da banda alta usada para medir o chiado\n"
        << "                         (padrao 5000)\n"
        << "  --slice <segundos>     fatia de tempo do relatorio (padrao 0,25)\n";
}

//==============================================================================
// Um evento MIDI com hora marcada. A sonda deixou de ter "a nota" e passou a
// ter uma LISTA -- e o que permite pedal, acorde e repeticao sem tres caminhos
// de codigo diferentes.
struct Evento
{
    enum class Tipo { noteOn, noteOff, pedalOn, pedalOff };

    double tempo = 0.0;        // segundos desde o inicio do render
    Tipo tipo = Tipo::noteOn;
    int note = 60;
    int vel = 100;
    juce::int64 amostra = 0;   // preenchido depois, quando a taxa e conhecida
    int ordem = 0;             // desempate estavel entre eventos do mesmo instante
};

const char* nomeDoTipo (Evento::Tipo t)
{
    switch (t)
    {
        case Evento::Tipo::noteOn:   return "on";
        case Evento::Tipo::noteOff:  return "off";
        case Evento::Tipo::pedalOn:  return "pedal-ON";
        case Evento::Tipo::pedalOff: return "pedal-OFF";
    }

    return "?";
}

//==============================================================================
// Monta a linha do tempo a partir das opcoes.
//
// A ORDEM DENTRO DO MESMO INSTANTE IMPORTA. O pedal precisa entrar ANTES das
// notas: se o CC64 chegasse depois do primeiro note-off, aquele note-off ja
// teria liberado a voz e o cenario deixaria de ser o do dono. Por isso os
// eventos carregam "ordem" e a ordenacao e estavel.
std::vector<Evento> montarLinhaDoTempo (const Options& opt)
{
    std::vector<Evento> eventos;
    int ordem = 0;

    const auto add = [&] (double tempo, Evento::Tipo tipo, int note, int vel)
    {
        Evento e;
        e.tempo = juce::jmax (0.0, tempo);
        e.tipo = tipo;
        e.note = note;
        e.vel = vel;
        e.ordem = ordem++;
        eventos.push_back (e);
    };

    if (opt.pedal)
        add (0.0, Evento::Tipo::pedalOn, 0, 0);

    std::vector<int> notas = opt.chord;

    if (notas.empty())
        notas.push_back (opt.note);

    for (int r = 0; r < opt.repeat; ++r)
    {
        const double inicio = (double) r * opt.repeatInterval;

        for (int n : notas)
            add (inicio, Evento::Tipo::noteOn, n, opt.vel);

        for (int n : notas)
            add (inicio + opt.hold, Evento::Tipo::noteOff, n, 0);
    }

    if (opt.pedalOff >= 0.0)
        add (opt.pedalOff, Evento::Tipo::pedalOff, 0, 0);

    return eventos;
}

//==============================================================================
// Le o script de --sequence. Formato deliberadamente burro: uma linha por
// evento, campos separados por espaco. "/" tambem separa eventos, para caber
// tudo numa linha so ao chamar da mao.
bool lerSequencia (const juce::File& arquivo, const Options& opt,
                   std::vector<Evento>& eventos, juce::String& falha)
{
    if (! arquivo.existsAsFile())
    {
        falha = "arquivo de --sequence nao encontrado: " + arquivo.getFullPathName();
        return false;
    }

    const auto texto = arquivo.loadFileAsString().replaceCharacter ('/', '\n');
    juce::StringArray linhas;
    linhas.addLines (texto);

    int ordem = 0;
    int numeroDaLinha = 0;

    for (auto linha : linhas)
    {
        ++numeroDaLinha;
        linha = linha.trim();

        if (linha.isEmpty() || linha.startsWithChar ('#'))
            continue;

        juce::StringArray campos;
        campos.addTokens (linha, " \t", "");
        campos.removeEmptyStrings();

        if (campos.size() < 2)
        {
            falha = "linha " + juce::String (numeroDaLinha)
                      + " incompleta: \"" + linha + "\"";
            return false;
        }

        Evento e;
        e.tempo = juce::jmax (0.0, campos[0].getDoubleValue());
        e.ordem = ordem++;

        const auto verbo = campos[1].toLowerCase();

        if (verbo == "on")
        {
            if (campos.size() < 3)
            {
                falha = "linha " + juce::String (numeroDaLinha) + ": \"on\" precisa da nota";
                return false;
            }

            e.tipo = Evento::Tipo::noteOn;
            e.note = juce::jlimit (0, 127, campos[2].getIntValue());
            e.vel = campos.size() >= 4 ? juce::jlimit (1, 127, campos[3].getIntValue())
                                       : opt.vel;
        }
        else if (verbo == "off")
        {
            if (campos.size() < 3)
            {
                falha = "linha " + juce::String (numeroDaLinha) + ": \"off\" precisa da nota";
                return false;
            }

            e.tipo = Evento::Tipo::noteOff;
            e.note = juce::jlimit (0, 127, campos[2].getIntValue());
        }
        else if (verbo == "pedal")
        {
            if (campos.size() < 3)
            {
                falha = "linha " + juce::String (numeroDaLinha) + ": \"pedal\" precisa de 0 ou 1";
                return false;
            }

            e.tipo = campos[2].getIntValue() != 0 ? Evento::Tipo::pedalOn
                                                  : Evento::Tipo::pedalOff;
        }
        else
        {
            falha = "linha " + juce::String (numeroDaLinha)
                      + ": verbo desconhecido \"" + campos[1] + "\""
                        " (esperado on / off / pedal)";
            return false;
        }

        eventos.push_back (e);
    }

    if (eventos.empty())
    {
        falha = "o arquivo de --sequence nao tem nenhum evento";
        return false;
    }

    return true;
}

//==============================================================================
// Espera o carregamento em segundo plano terminar, girando a fila de mensagens.
// Copiado do padrao de Tests/Audio/TimbreLoadStressTests.cpp:69.
bool dispatchUntilLoaded (TimbresProcessor& processor, int timeoutSeconds)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds (timeoutSeconds);

    while (processor.isLoading()
           && std::chrono::steady_clock::now() < deadline)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }

    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    return ! processor.isLoading();
}

//==============================================================================
juce::Array<juce::File> collectSamples (const juce::File& directory,
                                        const juce::String& prefix)
{
    juce::Array<juce::File> files;

    for (const auto& file : directory.findChildFiles (
             juce::File::findFiles, false, "*.aif;*.aiff;*.wav;*.flac"))
        if (prefix.isEmpty()
             || file.getFileNameWithoutExtension().startsWithIgnoreCase (prefix))
            files.add (file);

    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   return a.getFileName().compareNatural (b.getFileName()) < 0;
               });

    return files;
}

//==============================================================================
// Relatorio de marcadores POR ARQUIVO. Eles sao informativos: o motor de timbres
// nao os consulta nem repete qualquer trecho do sample.
int relatarZonas (const TimbresProcessor::TimbreEntry& entry)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    int comSmpl = 0, comCue = 0, semNada = 0, ilegivel = 0;

    std::cout << "MARCADORES DE LOOP IGNORADOS PELO MOTOR -- TIMBRE \""
              << entry.label << "\" ("
              << entry.files.size() << ")\n"
              << "estado        len       loopStart loopEnd   arquivo\n";

    for (const auto& f : entry.files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            formats.createReaderFor (f));

        if (reader == nullptr)
        {
            ++ilegivel;
            std::cout << "ILEGIVEL      -         -         -         "
                      << f.getFileName() << '\n';
            continue;
        }

        const auto& meta = reader->metadataValues;
        const int len = (int) juce::jmin ((juce::int64) std::numeric_limits<int>::max(),
                                          reader->lengthInSamples);

        const int numLoops = meta.getValue ("NumSampleLoops", "0").getIntValue();
        const int ls = meta.getValue ("Loop0Start", "-1").getIntValue();
        const int leRaw = meta.getValue ("Loop0End", "-1").getIntValue();
        const int le = juce::jmin (leRaw, len - 2);

        // Chunk 'smpl' do WAV.
        const bool smplValido = numLoops > 0 && ls >= 0 && le > ls + 256;

        // Secao 1b: AIFF guarda o loop como IDs de marcador na tabela de cues.
        const int loopType = meta.getValue ("Loop0Type", "0").getIntValue();
        bool cueValido = false;
        int cueStart = -1, cueEnd = -1;

        if (! smplValido && loopType > 0)
        {
            const int idStart = meta.getValue ("Loop0StartIdentifier", "-1").getIntValue();
            const int idEnd   = meta.getValue ("Loop0EndIdentifier", "-1").getIntValue();
            const int numCues = meta.getValue ("NumCuePoints", "0").getIntValue();

            for (int i = 0; i < numCues && i < 512; ++i)
            {
                const auto pre = "Cue" + juce::String (i);
                const int id = meta.getValue (pre + "Identifier", "-1").getIntValue();

                if (id < 0)
                    continue;

                const int off = meta.getValue (pre + "Offset", "-1").getIntValue();

                if (off < 0)
                    continue;

                if (id == idStart) cueStart = off;
                if (id == idEnd)   cueEnd = off;
            }

            cueEnd = juce::jmin (cueEnd, len - 2);
            cueValido = cueStart >= 0 && cueEnd > cueStart + 256;
        }

        juce::String estado, a, b;

        if (smplValido)      { estado = "SMPL";        a = juce::String (ls);        b = juce::String (le);      ++comSmpl; }
        else if (cueValido)  { estado = "AIFF-CUE";    a = juce::String (cueStart);  b = juce::String (cueEnd);  ++comCue; }
        else if (len < 4096) { estado = "CURTO";        a = "-"; b = "-"; ++semNada; }
        else                 { estado = "SEM-MARCADOR"; a = "-"; b = "-"; ++semNada; }

        std::cout << estado.paddedRight (' ', 14)
                  << juce::String (len).paddedRight (' ', 10)
                  << a.paddedRight (' ', 10)
                  << b.paddedRight (' ', 10)
                  << f.getFileName() << '\n';
    }

    const int total = entry.files.size();

    std::cout << "\nRESUMO DE MARCADORES (IGNORADOS)  total " << total
              << "  | smpl " << comSmpl
              << "  | aiff-cue " << comCue
              << "  | sem marcador " << semNada
              << "  | ilegivel " << ilegivel << '\n';

    if (total > 0)
        std::cout << "PORCENTAGEM SEM MARCADOR: "
                  << juce::String (100.0 * semNada / total, 1) << "%\n";

    std::cout << "TIMBRE_RENDER_PROBE_ZONES_OK\n";
    return 0;
}

//==============================================================================
struct Estatisticas
{
    float pico = 0.0f;
    double soma = 0.0;         // soma dos quadrados (os dois canais)
    double somaDc = 0.0;
    juce::int64 amostras = 0;
    juce::int64 naoFinitas = 0;
    int buracos = 0;           // trechos de silencio absoluto FECHADOS por audio
    juce::int64 amostrasEmBuracos = 0;
    juce::int64 ultimaComSom = -1;   // ultima amostra nao-nula do arquivo inteiro
};

//==============================================================================
// Uma fatia do relatorio: e aqui que "chiado no fim" deixa de ser adjetivo.
struct Fatia
{
    double rms = 0.0;       // banda inteira
    double rmsAlta = 0.0;   // acima de --hf
    int vozesMax = 0;
    juce::uint32 roubos = 0;   // roubos ACUMULADOS ate o fim da fatia
};

//==============================================================================
// Passa-altas de 3 secoes (18 dB/oitava) para isolar a banda do chiado.
//
// Uma secao so (12 dB/oit) deixa passar fundamental e primeiros harmonicos de
// piano demais, e ai a medicao mede a NOTA, nao o ruido. Tres secoes iguais
// derrubam o corte alguns dB e afundam a musica o bastante para o que interessa
// aqui, que e COMPARAR a mesma banda ao longo do tempo dentro do mesmo render.
// Nao e um filtro de medicao calibrado, e nem precisa ser: a pergunta e "esta
// banda sobe no fim?", nao "quantos dB SPL exatos".
//
// EM DOUBLE, E DE PROPOSITO. A primeira versao usava
// juce::SingleThreadedIIRFilter, que guarda estado em float. Um passa-altas em
// float carrega ruido de quantizacao do proprio estado e pode entrar em ciclo
// limite -- e o fenomeno que estamos cacando vive entre -60 e -90 dBFS, ou
// seja, o instrumento de medida nao pode ter piso na mesma faixa do fenomeno.
// Em double o piso do filtro cai para a casa de -300 dBFS e sai da conta.
//
// REGISTRO DE UM ERRO MEU, para ninguem repetir: cheguei a atribuir a este
// filtro um "piso medido de -100,0 dBFS" que aparecia em varias fatias
// seguidas. Era falso. -100,0 e o valor de juce::Decibels::defaultMinusInfinitydB
// (juce_Decibels.h:133): gainToDecibels GRAMPEIA ali por padrao. O filtro nao
// tinha nada a ver. Por isso todo dB deste relatorio passa por dbfsFundo(),
// que informa um piso explicito de -200 dB -- sem isso a sonda seria incapaz
// de relatar qualquer coisa abaixo de -100 dBFS, que e justamente onde mora o
// ruido de fundo de uma voz sozinha (a referencia contra a qual a soma de
// dezenas de caudas precisa ser comparada).
//
// Coeficientes RBJ (Audio EQ Cookbook), calculados aqui em vez de virem do
// juce::IIRCoefficients porque aqueles sao float e a intencao e justamente nao
// ter float nenhum neste caminho.
struct BandaAlta
{
    void prepare (double sr, double hz)
    {
        const double q = 0.7071067811865476;
        const double w0 = 2.0 * juce::MathConstants<double>::pi * hz / sr;
        const double cosw = std::cos (w0);
        const double alpha = std::sin (w0) / (2.0 * q);

        const double b0 = (1.0 + cosw) * 0.5;
        const double b1 = -(1.0 + cosw);
        const double b2 = (1.0 + cosw) * 0.5;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosw;
        const double a2 = 1.0 - alpha;

        for (auto& s : secoes)
        {
            s.b0 = b0 / a0;
            s.b1 = b1 / a0;
            s.b2 = b2 / a0;
            s.a1 = a1 / a0;
            s.a2 = a2 / a0;
            s.x1 = s.x2 = s.y1 = s.y2 = 0.0;
        }
    }

    double processa (double x) noexcept
    {
        for (auto& s : secoes)
            x = s.processa (x);

        return x;
    }

    struct Secao
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        double processa (double x) noexcept
        {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;  x1 = x;
            y2 = y1;  y1 = y;
            return y;
        }
    };

    Secao secoes[3];
};

//==============================================================================
// dBFS com piso EXPLICITO de -200 dB.
//
// juce::Decibels::gainToDecibels grampeia em -100 dB por padrao
// (defaultMinusInfinitydB, juce_Decibels.h:133). Numa sonda que existe para
// medir ruido de fundo isso e um teto disfarcado de piso: uma voz sozinha de
// piano 24 bits tem fundo perto de -110 dBFS e apareceria como "-100,0",
// exatamente igual a dez vozes somadas. A comparacao que interessa -- uma voz
// contra dezenas -- ficaria invisivel.
juce::String dbfsFundo (double v)
{
    if (! (v > 0.0))
        return "-inf";

    return juce::String (juce::Decibels::gainToDecibels (v, -200.0), 1);
}

}   // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    Options opt;
    bool repeatIntervalDado = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (juce::String::fromUTF8 (argv[i]));
        const auto valor = [&] () -> juce::String
        {
            return (i + 1 < argc) ? juce::String::fromUTF8 (argv[++i]) : juce::String();
        };

        if (arg == "--list")            opt.list = true;
        else if (arg == "--zones")      opt.zones = true;
        else if (arg == "--map")        opt.map = true;
        else if (arg == "--timbre")     opt.timbre = valor();
        else if (arg == "--library")    opt.library = valor();
        else if (arg == "--folder")     opt.folder = valor();
        else if (arg == "--prefix")     opt.prefix = valor();
        else if (arg == "--out")        opt.out = valor();
        else if (arg == "--note")       opt.note = valor().getIntValue();
        else if (arg == "--vel")        opt.vel = valor().getIntValue();
        else if (arg == "--transpose")  opt.transpose = valor().getIntValue();
        else if (arg == "--layer")      opt.layer = valor().getIntValue();
        else if (arg == "--hold")       opt.hold = valor().getDoubleValue();
        else if (arg == "--tail")       opt.tail = valor().getDoubleValue();
        else if (arg == "--sr")         opt.sampleRate = valor().getDoubleValue();
        else if (arg == "--block")      opt.blockSize = valor().getIntValue();
        else if (arg == "--bits")       opt.bits = valor().getIntValue();
        else if (arg == "--factory")    opt.factory = true;
        else if (arg == "--reverb")   { opt.reverbSend = (float) valor().getDoubleValue(); opt.reverbSet = true; }
        else if (arg == "--pace")       opt.pace = valor().getDoubleValue();
        else if (arg == "--master-db") { opt.masterDb = (float) valor().getDoubleValue(); opt.masterSet = true; }
        else if (arg == "--punch")     { opt.punch = valor().getIntValue() != 0; opt.punchSet = true; }
        else if (arg == "--pedal")      opt.pedal = valor().getIntValue() != 0;
        else if (arg == "--pedal-off")  opt.pedalOff = valor().getDoubleValue();
        else if (arg == "--sequence")   opt.sequence = valor();
        else if (arg == "--hf")         opt.hfHz = valor().getDoubleValue();
        else if (arg == "--slice")      opt.slice = valor().getDoubleValue();
        else if (arg == "--chord")
        {
            juce::StringArray partes;
            partes.addTokens (valor(), ",;", "");
            partes.removeEmptyStrings();

            opt.chord.clear();

            for (const auto& p : partes)
            {
                const auto t = p.trim();

                if (! t.containsOnly ("0123456789") || t.isEmpty())
                    return erro ("--chord aceita numeros MIDI separados por virgula;"
                                 " recebi \"" + t + "\"");

                opt.chord.push_back (juce::jlimit (0, 127, t.getIntValue()));
            }

            if (opt.chord.empty())
                return erro ("--chord veio vazio");
        }
        else if (arg == "--repeat")
        {
            opt.repeat = valor().getIntValue();

            // O intervalo e o SEGUNDO valor. Ele e opcional na linha de comando
            // (se vier outra opcao em seguida, nao consome) porque engolir um
            // "--pedal" aqui daria um erro mudo e confuso.
            if (i + 1 < argc)
            {
                const juce::String proximo (juce::String::fromUTF8 (argv[i + 1]));

                if (! proximo.startsWith ("--"))
                {
                    opt.repeatInterval = proximo.getDoubleValue();
                    repeatIntervalDado = true;
                    ++i;
                }
            }
        }
        else if (arg == "--help" || arg == "-h") { uso(); return 0; }
        else return erro ("argumento desconhecido: " + arg);
    }

    if (argc <= 1)
    {
        uso();
        return 0;
    }

    opt.note = juce::jlimit (0, 127, opt.note);
    opt.vel = juce::jlimit (1, 127, opt.vel);
    opt.layer = juce::jlimit (0, TimbresProcessor::numLayers - 1, opt.layer);
    opt.transpose = juce::jlimit (-24, 24, opt.transpose);
    opt.hold = juce::jlimit (0.05, 120.0, opt.hold);
    opt.tail = juce::jlimit (0.0, 120.0, opt.tail);
    opt.blockSize = juce::jlimit (16, 8192, opt.blockSize);
    opt.reverbSend = juce::jlimit (0.0f, 1.0f, opt.reverbSend);
    opt.masterDb = juce::jlimit (MasterProcessor::minusInfinityDb,
                                 MasterProcessor::maxGainDb, opt.masterDb);
    opt.repeat = juce::jlimit (1, 512, opt.repeat);
    opt.slice = juce::jlimit (0.01, 5.0, opt.slice);
    opt.hfHz = juce::jlimit (100.0, juce::jmax (200.0, opt.sampleRate * 0.45), opt.hfHz);

    // Sem intervalo explicito, repetir "no mesmo lugar" nao acumularia nada de
    // interessante: o padrao passa a ser tocar de novo assim que a anterior
    // solta a tecla.
    if (opt.repeat > 1 && ! repeatIntervalDado)
        opt.repeatInterval = opt.hold;

    opt.repeatInterval = juce::jlimit (0.0, 120.0, opt.repeatInterval);

    if (opt.bits != 24 && opt.bits != 32)
        return erro ("--bits aceita 24 ou 32");

    // -------------------------------------------------------------------------
    // 1) Linha do tempo dos eventos
    // -------------------------------------------------------------------------
    std::vector<Evento> eventos;

    if (opt.sequence.isNotEmpty())
    {
        const juce::File arquivo =
            juce::File::isAbsolutePath (opt.sequence)
                ? juce::File (opt.sequence)
                : juce::File::getCurrentWorkingDirectory().getChildFile (opt.sequence);

        juce::String falha;

        if (! lerSequencia (arquivo, opt, eventos, falha))
            return erro (falha);
    }
    else
    {
        eventos = montarLinhaDoTempo (opt);
    }

    std::stable_sort (eventos.begin(), eventos.end(),
                      [] (const Evento& a, const Evento& b)
                      {
                          if (a.tempo != b.tempo)
                              return a.tempo < b.tempo;

                          return a.ordem < b.ordem;
                      });

    double fimDosEventos = 0.0;

    for (const auto& e : eventos)
        fimDosEventos = juce::jmax (fimDosEventos, e.tempo);

    const double duracao = juce::jmin (duracaoMaximaSegundos,
                                       fimDosEventos + opt.tail);

    if (duracao <= 0.0)
        return erro ("a linha do tempo ficou com duracao zero");

    // -------------------------------------------------------------------------
    // 2) Escolha do timbre
    // -------------------------------------------------------------------------
    TimbresProcessor::TimbreEntry entry;
    bool entryPronta = false;

    if (opt.folder.isNotEmpty())
    {
        const juce::File pasta (opt.folder);

        if (! pasta.isDirectory())
            return erro ("--folder nao e uma pasta: " + opt.folder);

        entry.files = collectSamples (pasta, opt.prefix);
        entry.label = opt.prefix.isNotEmpty() ? opt.prefix : pasta.getFileName();
        entry.library = "Manual";
        entry.category = "PADS";

        if (entry.files.isEmpty())
            return erro ("nenhum sample encontrado em " + opt.folder
                          + " com prefixo \"" + opt.prefix + "\"");

        entryPronta = true;
    }

    // O processador precisa existir antes do scan (getEntries usa o cache dele).
    // A construcao ja dispara startEntriesScanAsync (TimbresProcessor.h:1137).
    auto processor = std::make_unique<TimbresProcessor>();

    if (! entryPronta || opt.list)
    {
        const auto& entradas = processor->getEntries();

        if (opt.list)
        {
            std::cout << "TIMBRES ENCONTRADOS (" << entradas.size() << ")\n"
                      << "biblioteca            categoria        n    rotulo\n";

            for (const auto& e : entradas)
                std::cout << e.library.paddedRight (' ', 22)
                          << e.category.paddedRight (' ', 17)
                          << juce::String (e.files.size()).paddedRight (' ', 5)
                          << e.label << '\n';

            std::cout << "TIMBRE_RENDER_PROBE_LIST_OK\n";
            return 0;
        }

        if (opt.timbre.isEmpty())
            return erro ("informe --timbre <texto> (use --list para ver os disponiveis)"
                         " ou --folder/--prefix");

        const TimbresProcessor::TimbreEntry* exato = nullptr;
        const TimbresProcessor::TimbreEntry* parcial = nullptr;

        for (const auto& e : entradas)
        {
            if (opt.library.isNotEmpty()
                 && ! e.library.containsIgnoreCase (opt.library))
                continue;

            if (e.label.equalsIgnoreCase (opt.timbre) && exato == nullptr)
                exato = &e;
            else if (e.label.containsIgnoreCase (opt.timbre) && parcial == nullptr)
                parcial = &e;
        }

        const auto* escolhido = exato != nullptr ? exato : parcial;

        if (escolhido == nullptr)
            return erro ("timbre nao encontrado: \"" + opt.timbre + "\"");

        entry = *escolhido;
        entryPronta = true;
    }

    std::cout << "TIMBRE      " << entry.label
              << "   biblioteca " << entry.library
              << "   categoria " << entry.category
              << "   arquivos " << entry.files.size() << '\n';

    if (opt.zones)
        return relatarZonas (entry);

    // -------------------------------------------------------------------------
    // 3) Prepara o motor e carrega o timbre
    // -------------------------------------------------------------------------
    processor->setPlayConfigDetails (0, 2, opt.sampleRate, opt.blockSize);
    processor->prepareToPlay (opt.sampleRate, opt.blockSize);

    const auto carregouEm = std::chrono::steady_clock::now();

    // applyFactoryToneOnSuccess = true e o que a INTERFACE faz quando o musico
    // escolhe um timbre. O padrao atual e neutro; somente um sidecar explicito
    // pode publicar envelope/EQ/FX autorais. Sem --factory a sonda preserva os
    // valores atuais do processador, util para isolar o sample.
    processor->loadTimbreIntoLayer (opt.layer, entry, opt.factory);

    if (! dispatchUntilLoaded (*processor, 300))
        return erro ("o carregamento do timbre nao terminou em 300 s");

    if (processor->getLayerTimbre (opt.layer).isEmpty())
        return erro ("o canal ficou sem timbre depois da carga (rotulo vazio)");

    const auto carregouMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - carregouEm).count();

    std::cout << "CARGA       " << carregouMs << " ms   canal " << opt.layer
              << "   rotulo instalado \"" << processor->getLayerTimbre (opt.layer)
              << "\"\n";

    if (opt.map)
    {
        int mapped = 0;
        int rangeStart = 0;
        int previousRoot = processor->getZoneRootNoteForTest (
            opt.layer, 0, opt.vel);
        int previousLoopStart = processor->getZoneLoopStartForTest (
            opt.layer, 0, opt.vel);
        int previousLoopEnd = processor->getZoneLoopEndForTest (
            opt.layer, 0, opt.vel);

        std::cout << "MAPA EFETIVO (128 notas verificadas)\n"
                  << "faixa       root   loop PCM [inicio..fim)\n";

        for (int note = 0; note <= 128; ++note)
        {
            const int root = note < 128
                               ? processor->getZoneRootNoteForTest (
                                     opt.layer, note, opt.vel)
                               : std::numeric_limits<int>::min();
            const int loopStart = note < 128
                                    ? processor->getZoneLoopStartForTest (
                                          opt.layer, note, opt.vel)
                                    : std::numeric_limits<int>::min();
            const int loopEnd = note < 128
                                  ? processor->getZoneLoopEndForTest (
                                        opt.layer, note, opt.vel)
                                  : std::numeric_limits<int>::min();

            if (note < 128 && root >= 0)
                ++mapped;

            if (note == 0
                 || (root == previousRoot
                     && loopStart == previousLoopStart
                     && loopEnd == previousLoopEnd))
                continue;

            std::cout << juce::String (rangeStart).paddedLeft (' ', 3)
                      << ".." << juce::String (note - 1).paddedLeft (' ', 3)
                      << "   " << juce::String (previousRoot).paddedLeft (' ', 4)
                      << "   " << previousLoopStart << ".." << previousLoopEnd
                      << '\n';

            rangeStart = note;
            previousRoot = root;
            previousLoopStart = loopStart;
            previousLoopEnd = loopEnd;
        }

        std::cout << "MAPPED      " << mapped << "/128\n"
                  << "TIMBRE_RENDER_PROBE_MAP_OK\n";
        return 0;
    }

    // Ajustes de parametro por ID: o motor expoe tudo como AudioProcessorParameter,
    // entao a sonda nao precisa de acesso privado a estrutura das camadas.
    const auto ajustar = [&processor] (const juce::String& id, float valorNormalizado)
    {
        for (auto* p : processor->getParameters())
            if (auto* comId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                if (comId->paramID == id)
                {
                    comId->setValueNotifyingHost (valorNormalizado);
                    return true;
                }

        return false;
    };

    // Leitura do que FICOU valendo depois da carga (o --factory pode ter mexido).
    const auto lerNormalizado = [&processor] (const juce::String& id) -> float
    {
        for (auto* p : processor->getParameters())
            if (auto* comId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                if (comId->paramID == id)
                    return comId->getValue();

        return -1.0f;
    };

    // rvb<l> 0..1: 0 deixa o motor SECO, sem o reverb nativo no caminho.
    if (opt.reverbSet)
        ajustar ("rvb" + juce::String (opt.layer), opt.reverbSend);

    const float reverbEfetivo = lerNormalizado ("rvb" + juce::String (opt.layer));

    if (opt.transpose != 0)
    {
        // transp<l> vai de -24 a +24: normalizado = (v + 24) / 48
        ajustar ("transp" + juce::String (opt.layer),
                 (float) (opt.transpose + 24) / 48.0f);
    }

    // -------------------------------------------------------------------------
    // 4) Render offline
    // -------------------------------------------------------------------------
    const juce::int64 amostrasTotal = (juce::int64) (duracao * opt.sampleRate);

    for (auto& e : eventos)
        e.amostra = (juce::int64) (e.tempo * opt.sampleRate);

    // Fronteira usada pela deteccao de buraco e de "fim precoce": ate o ultimo
    // evento o motor DEVE estar produzindo alguma coisa. Depois dela vem so a
    // cauda, e cauda que chega a zero e o fim normal da nota, nao um clique.
    const juce::int64 amostraUltimoEvento =
        (juce::int64) (fimDosEventos * opt.sampleRate);

    juce::AudioBuffer<float> saida (2, (int) amostrasTotal);
    saida.clear();

    juce::AudioBuffer<float> bloco (2, opt.blockSize);
    juce::MidiBuffer midi;

    std::unique_ptr<MasterProcessor> master;
    float piorReducaoDoLimiterDb = 0.0f;

    if (opt.masterSet || opt.punchSet)
    {
        master = std::make_unique<MasterProcessor>();
        master->prepareToPlay (opt.sampleRate, opt.blockSize);
        master->setAutoEq (false);
        master->setMasterDb (opt.masterDb);
        master->setPunch (opt.punch);
    }

    // Serie temporal de vozes ativas, amostrada UMA vez por bloco logo depois de
    // processBlock. Nao ha custo nenhum para o motor: a sonda chama processBlock
    // na propria thread, entao ler a contagem entre blocos e tao valido quanto
    // ler dentro dele -- e nao obriga o produto a contar vozes a cada bloco.
    std::vector<int> vozesPorBloco;
    std::vector<juce::int64> posPorBloco;
    vozesPorBloco.reserve ((size_t) (amostrasTotal / opt.blockSize + 2));
    posPorBloco.reserve ((size_t) (amostrasTotal / opt.blockSize + 2));

    // Roubos acumulados por bloco: e o que permite dizer "o chiado comecou no
    // mesmo instante em que o roubo comecou" -- ou provar que nao comecou.
    std::vector<juce::uint32> roubosPorBloco;
    roubosPorBloco.reserve ((size_t) (amostrasTotal / opt.blockSize + 2));

    juce::int64 pos = 0;
    size_t proxEvento = 0;
    int vozesMaximo = 0;
    juce::int64 amostraDoPico = 0;
    int vozesNoPedalOff = -1;
    const juce::uint32 roubosAntes = processor->getVoiceStealsInLayer();
    const juce::uint32 tomadasAntes = processor->getVoiceTakeovers();
    const juce::uint32 streamAntes = processor->getStreamMissCount();
    const juce::uint32 nanAntes = processor->getNanGuardHitCount();
    const juce::uint32 globaisAntes = processor->getVoiceStealsGlobal();
    const juce::uint32 inaudiveisAntes = processor->getInaudibleCuts();

    const auto renderComecou = std::chrono::steady_clock::now();

    while (pos < amostrasTotal)
    {
        const int n = (int) juce::jmin ((juce::int64) opt.blockSize, amostrasTotal - pos);

        bloco.setSize (2, n, false, false, true);
        bloco.clear();
        midi.clear();

        bool houvePedalOff = false;

        while (proxEvento < eventos.size()
                && eventos[proxEvento].amostra < pos + n)
        {
            const auto& e = eventos[proxEvento];
            const int off = (int) juce::jlimit ((juce::int64) 0,
                                                (juce::int64) (n - 1),
                                                e.amostra - pos);

            switch (e.tipo)
            {
                case Evento::Tipo::noteOn:
                    midi.addEvent (juce::MidiMessage::noteOn (
                                       1, e.note, (juce::uint8) e.vel), off);
                    break;

                case Evento::Tipo::noteOff:
                    midi.addEvent (juce::MidiMessage::noteOff (1, e.note), off);
                    break;

                case Evento::Tipo::pedalOn:
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), off);
                    break;

                case Evento::Tipo::pedalOff:
                    midi.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), off);
                    houvePedalOff = true;
                    break;
            }

            ++proxEvento;
        }

        // Vozes ANTES do bloco que solta o pedal: e o numero que responde
        // "quantas caudas comecaram no mesmo sample-frame?" (H4).
        const int vozesAntesDoBloco = processor->getActiveVoiceCount();

        processor->processBlock (bloco, midi);

        if (master != nullptr)
        {
            master->processBlock (bloco, midi);
            piorReducaoDoLimiterDb = juce::jmin (
                piorReducaoDoLimiterDb, master->getLimiterGainReductionDb());
        }

        for (int ch = 0; ch < 2; ++ch)
            saida.copyFrom (ch, (int) pos, bloco, ch, 0, n);

        const int vozesAgora = processor->getActiveVoiceCount();

        if (houvePedalOff && vozesNoPedalOff < 0)
            vozesNoPedalOff = vozesAntesDoBloco;

        if (vozesAgora > vozesMaximo)
        {
            vozesMaximo = vozesAgora;
            amostraDoPico = pos;
        }

        vozesPorBloco.push_back (vozesAgora);
        posPorBloco.push_back (pos);
        roubosPorBloco.push_back (processor->getVoiceStealsInLayer() - roubosAntes);

        pos += n;

        // O carregamento e o streamer usam a fila de mensagens/threads proprias.
        // Girar a fila entre blocos mantem a sonda equivalente ao app rodando.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (0);

        if (opt.pace > 0.0)
        {
            const auto microssegundos = (long long) (1.0e6 * (double) n
                                                     / (opt.sampleRate * opt.pace));
            std::this_thread::sleep_for (std::chrono::microseconds (microssegundos));
        }
    }

    const auto renderMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - renderComecou).count();

    const juce::uint32 roubos = processor->getVoiceStealsInLayer() - roubosAntes;
    const juce::uint32 tomadas = processor->getVoiceTakeovers() - tomadasAntes;
    const juce::uint32 streamMiss = processor->getStreamMissCount() - streamAntes;
    const juce::uint32 nanHits = processor->getNanGuardHitCount() - nanAntes;
    const juce::uint32 roubosGlobais = processor->getVoiceStealsGlobal() - globaisAntes;
    const juce::uint32 inaudiveis = processor->getInaudibleCuts() - inaudiveisAntes;

    // -------------------------------------------------------------------------
    // 5) Medicoes -- a sonda so vale se ela mesma provar que gerou audio
    // -------------------------------------------------------------------------
    Estatisticas est;
    const auto* L = saida.getReadPointer (0);
    const auto* R = saida.getReadPointer (1);

    int runZero = 0;
    const int limiarBuraco = (int) (opt.sampleRate * 0.002);   // 2 ms

    for (juce::int64 i = 0; i < amostrasTotal; ++i)
    {
        const float l = L[i], r = R[i];

        if (! std::isfinite (l) || ! std::isfinite (r))
        {
            ++est.naoFinitas;
            continue;
        }

        est.pico = juce::jmax (est.pico, std::abs (l), std::abs (r));
        est.soma += (double) l * l + (double) r * r;
        est.somaDc += (double) l + (double) r;
        est.amostras += 2;

        // Buraco: silencio ABSOLUTO (bit a bit zero) dentro da nota. O motor
        // preenche com zero quando a janela de streaming nao chega
        // (TimbresProcessor.h:10440); audio real quase nunca fica exatamente 0.
        //
        // So conta o buraco FECHADO por audio de novo. Silencio que vai ate o
        // fim da nota nao e um clique: e o sample que acabou naturalmente, e isso
        // e relatado a parte como FIM PRECOCE se havia eventos posteriores.
        if (l == 0.0f && r == 0.0f)
        {
            if (i < amostraUltimoEvento)
                ++runZero;
        }
        else
        {
            est.ultimaComSom = i;

            if (i < amostraUltimoEvento && runZero >= limiarBuraco)
            {
                ++est.buracos;
                est.amostrasEmBuracos += runZero;
            }

            runZero = 0;
        }
    }

    const double rms = est.amostras > 0 ? std::sqrt (est.soma / (double) est.amostras) : 0.0;
    const double dc = est.amostras > 0 ? est.somaDc / (double) est.amostras : 0.0;

    // -- fatias: RMS total, RMS da banda alta e vozes -------------------------
    const juce::int64 amostrasPorFatia =
        juce::jmax ((juce::int64) 1, (juce::int64) (opt.sampleRate * opt.slice));
    const size_t numFatias =
        (size_t) ((amostrasTotal + amostrasPorFatia - 1) / amostrasPorFatia);

    std::vector<Fatia> fatias (numFatias);

    {
        BandaAlta hp;
        hp.prepare (opt.sampleRate, opt.hfHz);

        std::vector<double> somaTotal (numFatias, 0.0);
        std::vector<double> somaAlta (numFatias, 0.0);
        std::vector<juce::int64> contagem (numFatias, 0);

        for (juce::int64 i = 0; i < amostrasTotal; ++i)
        {
            if (! std::isfinite (L[i]) || ! std::isfinite (R[i]))
                continue;

            const double mono = 0.5 * ((double) L[i] + (double) R[i]);
            const double alto = hp.processa (mono);
            const size_t f = (size_t) juce::jmin ((juce::int64) numFatias - 1,
                                                  i / amostrasPorFatia);

            somaTotal[f] += mono * mono;
            somaAlta[f] += alto * alto;
            ++contagem[f];
        }

        for (size_t f = 0; f < numFatias; ++f)
        {
            if (contagem[f] > 0)
            {
                fatias[f].rms = std::sqrt (somaTotal[f] / (double) contagem[f]);
                fatias[f].rmsAlta = std::sqrt (somaAlta[f] / (double) contagem[f]);
            }
        }
    }

    for (size_t b = 0; b < vozesPorBloco.size(); ++b)
    {
        const size_t f = (size_t) juce::jmin ((juce::int64) numFatias - 1,
                                              posPorBloco[b] / amostrasPorFatia);
        fatias[f].vozesMax = juce::jmax (fatias[f].vozesMax, vozesPorBloco[b]);
        fatias[f].roubos = juce::jmax (fatias[f].roubos, roubosPorBloco[b]);
    }

    // -------------------------------------------------------------------------
    // 6) Escreve o WAV
    // -------------------------------------------------------------------------
    juce::File destino;

    if (opt.out.isNotEmpty())
    {
        destino = juce::File::isAbsolutePath (opt.out)
                    ? juce::File (opt.out)
                    : juce::File::getCurrentWorkingDirectory().getChildFile (opt.out);
    }
    else
    {
        destino = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("ChurchDAW-render-"
                                     + juce::File::createLegalFileName (entry.label)
                                     + "-n" + juce::String (opt.note)
                                     + "-v" + juce::String (opt.vel) + ".wav");
    }

    destino.getParentDirectory().createDirectory();
    destino.deleteFile();

    auto stream = destino.createOutputStream();

    if (stream == nullptr)
        return erro ("nao foi possivel abrir para escrita: " + destino.getFullPathName());

    auto* rawStream = stream.release();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (rawStream, opt.sampleRate, 2, opt.bits, {}, 0));

    if (writer == nullptr)
    {
        delete rawStream;
        return erro ("WavAudioFormat recusou "
                      + juce::String (opt.bits) + " bits a "
                      + juce::String (opt.sampleRate, 0) + " Hz");
    }

    if (! writer->writeFromAudioSampleBuffer (saida, 0, (int) amostrasTotal))
        return erro ("falha ao gravar as amostras no WAV");

    writer.reset();   // fecha o arquivo antes de medir o tamanho

    // -------------------------------------------------------------------------
    // 7) Relatorio
    // -------------------------------------------------------------------------
    const auto dbfs = [] (double v)
    {
        return v > 0.0 ? juce::String (juce::Decibels::gainToDecibels (v, -200.0), 2)
                       : juce::String ("-inf");
    };

    juce::String descricaoDasNotas;

    if (opt.sequence.isNotEmpty())
    {
        descricaoDasNotas = "script " + opt.sequence;
    }
    else if (! opt.chord.empty())
    {
        for (size_t i = 0; i < opt.chord.size(); ++i)
            descricaoDasNotas << (i > 0 ? "," : "") << opt.chord[(size_t) i];
    }
    else
    {
        descricaoDasNotas = juce::String (opt.note);
    }

    std::cout << "RENDER      notas " << descricaoDasNotas
              << "  vel " << opt.vel
              << "  hold " << opt.hold << " s  cauda " << opt.tail << " s"
              << "  sr " << juce::String (opt.sampleRate, 0)
              << "  bloco " << opt.blockSize << '\n'
              << "CENARIO     "
              << (opt.sequence.isNotEmpty()
                    ? juce::String ("pedal e notas definidos PELO SCRIPT")
                    : "pedal " + juce::String (opt.pedal ? "PISADO" : "solto")
                        + "   pedal-off "
                        + (opt.pedalOff >= 0.0 ? juce::String (opt.pedalOff, 3) + " s"
                                               : juce::String ("nunca"))
                        + "   repeticoes " + juce::String (opt.repeat)
                        + " a cada " + juce::String (opt.repeatInterval, 3) + " s")
              << "   eventos " << (int) eventos.size()
              << "   duracao " << juce::String (duracao, 3) << " s\n"
              << "CANAL       reverbSend " << juce::String (reverbEfetivo, 3)
              << "   factory " << (opt.factory ? "SIM" : "NAO")
              << "   transpose " << opt.transpose << '\n';

    if (master != nullptr)
        std::cout << "MASTER      " << juce::String (opt.masterDb, 1)
                  << " dB   PESO " << (opt.punch ? "ON" : "OFF")
                  << "   pior reducao limiter "
                  << juce::String (piorReducaoDoLimiterDb, 2) << " dB"
                  << "   correcoes emergenciais "
                  << master->getLimiterEmergencyCorrectionCount() << '\n';

    std::cout
              << "TEMPO       " << renderMs << " ms para "
              << juce::String ((double) amostrasTotal / opt.sampleRate, 3)
              << " s de audio  ("
              << juce::String (((double) amostrasTotal / opt.sampleRate)
                                 / juce::jmax (0.001, (double) renderMs / 1000.0), 1)
              << "x tempo real)\n"
              << "PICO        " << juce::String (est.pico, 6)
              << "  (" << dbfs (est.pico) << " dBFS)\n"
              << "RMS         " << juce::String (rms, 6)
              << "  (" << dbfs (rms) << " dBFS)\n"
              << "CRISTA      " << (rms > 0.0 ? juce::String (est.pico / rms, 2)
                                              : juce::String ("-")) << '\n'
              << "DC          " << juce::String (dc, 8) << '\n'
              << "NAO-FINITO  " << est.naoFinitas << " amostras\n"
              << "BURACOS     " << est.buracos << " trecho(s) de silencio absoluto >= 2 ms"
              << " FECHADOS por audio ate o ultimo evento  ("
              << est.amostrasEmBuracos << " amostras)\n";

    // -- diagnostico do motor -------------------------------------------------
    //
    // Estes numeros vem do PROPRIO TimbresProcessor, por acessores const. A
    // sonda nao estima nada aqui.
    std::cout << "VOZES-PICO  " << vozesMaximo
              << " simultaneas (orcamento do canal " << opt.layer << ": "
              << TimbresProcessor::voicesForLayer (opt.layer)
              << ";  pool total " << TimbresProcessor::maxVoices << ")"
              << "   pico em " << juce::String ((double) amostraDoPico / opt.sampleRate, 3)
              << " s\n";

    if (vozesNoPedalOff >= 0)
        std::cout << "PEDAL-OFF   " << vozesNoPedalOff
                  << " vozes vivas no instante em que o pedal soltou"
                     " -- releasePedalHeldVoices poe TODAS em release no mesmo"
                     " sample-frame (TimbresProcessor.h:9349)\n";

    std::cout << "ROUBO       " << roubos
              << " roubo(s) por orcamento de camada estourado"
              << "   (dos quais " << tomadas
              << " pegaram uma voz AINDA ATIVA e precisaram da rampa de declique)"
              << "   roubo global " << roubosGlobais << '\n'
              << "FALHA-STREAM " << streamMiss
              << " bloco(s) em que a janela de disco nao chegou\n"
              << "GUARDA-NAN  " << nanHits << " disparo(s)\n"
              << "CORTE-INAUDIVEL " << inaudiveis
              << " voz(es) solta(s) com rampa de 20 ms por terem ficado abaixo"
                 " do limiar de inaudibilidade, em vez de somarem ruido ate o"
                 " fim do arquivo\n";

    if (vozesMaximo >= TimbresProcessor::voicesForLayer (opt.layer))
        std::cout << "AVISO       o pico de vozes ALCANCOU o orcamento do canal."
                     " A partir daqui todo note-on rouba uma voz que ainda estava"
                     " soando.\n";

    // A nota morreu antes do fim dos eventos? O sample termina naturalmente.
    if (est.ultimaComSom >= 0 && est.ultimaComSom < amostraUltimoEvento - limiarBuraco)
        std::cout << "FIM PRECOCE o som acabou em "
                  << juce::String ((double) est.ultimaComSom / opt.sampleRate, 3)
                  << " s, mas ainda havia evento ate "
                  << juce::String (fimDosEventos, 3)
                  << " s -- o arquivo de origem terminou naturalmente\n";

    // -- series temporais -----------------------------------------------------
    //
    // As tres linhas sao lidas EM COLUNA. "Chiado no fim" tem uma assinatura
    // muito especifica aqui: ALTA sobe (ou para de descer) enquanto RMS desce.
    const auto imprimirSerie = [&] (const juce::String& rotulo, auto pega)
    {
        std::cout << rotulo;

        for (const auto& f : fatias)
            std::cout << ' ' << pega (f);

        std::cout << '\n';
    };

    std::cout << "\nSERIES por " << juce::String (opt.slice, 3) << " s"
                 "  (leia em coluna: as tres linhas cobrem as mesmas fatias)\n";

    imprimirSerie ("RMS   dBFS ", [] (const Fatia& f) { return dbfsFundo (f.rms); });

    imprimirSerie ("ALTA  dBFS (>" + juce::String (opt.hfHz, 0) + " Hz) ",
                   [] (const Fatia& f) { return dbfsFundo (f.rmsAlta); });

    imprimirSerie ("VOZES      ", [] (const Fatia& f) { return juce::String (f.vozesMax); });
    imprimirSerie ("ROUBOS     ", [] (const Fatia& f) { return juce::String ((int) f.roubos); });

    std::cout << "\nARQUIVO     " << destino.getFullPathName() << '\n'
              << "TAMANHO     " << destino.getSize() << " bytes\n";

    if (est.buracos > 0 && opt.pace <= 0.0)
        std::cout << "AVISO       ha silencio absoluto dentro da nota. Pode ser a"
                     " janela de streaming que nao acompanhou a sonda rodando"
                     " acima do tempo real. Repita com --pace 1 para separar"
                     " defeito do motor de artefato da sonda.\n";

    if (est.naoFinitas > 0)
        return erro ("o motor produziu amostras NaN/inf");

    if (est.pico <= 0.0f)
        return erro ("o render saiu em silencio absoluto (pico 0)");

    std::cout << "TIMBRE_RENDER_PROBE_OK\n";
    return 0;
}
