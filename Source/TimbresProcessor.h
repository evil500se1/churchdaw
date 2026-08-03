#pragma once

// juce::MidiInput (lista de teclados por canal) vive em juce_audio_devices.
// Testes console e o app desktop precisam do include explicito; sem ele o CI
// macOS falha em ChurchDAWTimbreLoadStressTests com "no member named MidiInput".
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "AppPaths.h"
#include "SampleStream.h"
#include "WorshipReverb.h"
#include "ChannelToneShaper.h"
#include "TimbreToneMemory.h"
#include "ChannelRotarySpeaker.h"
#include "PerformanceMidiSafety.h"
#include "AndroidStageLayout.h"
#include "UiProfiler.h"
#include "ChurchOrgan/ChurchOrganEditor.h"
#include "ChurchOrgan/ChurchOrganLayerProcessor.h"

// INSTRUMENTACAO DO CAMINHO DE AUDIO -- DESLIGADA POR PADRAO.
//
// O relatorio de carga ("AUDIO: media ... pico ... lentos ...") foi essencial
// para caçar os estalos no Android: ele mede do PROPRIO caminho do audio, que e
// o unico lugar onde os numeros sao verdadeiros.
//
// Mas ele monta o texto ali mesmo, na thread de audio: cerca de vinte
// alocacoes de juce::String mais uma escrita em log, uma vez por segundo.
// Alocar e escrever em arquivo dentro do callback e exatamente o que nao se
// pode fazer em audio de tempo real -- foi um risco aceito de propósito
// enquanto o problema estava em aberto, e medido em 28/07/2026 ainda ativo na
// versao 1.4.0 (53 linhas em 30 segundos no aparelho).
//
// Agora que os estalos foram resolvidos, ela fica atras deste interruptor:
// disponivel quando for preciso investigar de novo, ausente do binario que o
// cliente recebe.
//
// Para ligar: -DCHURCHDAW_AUDIO_INSTRUMENTATION=1 no CMake, ou definir no
// Projucer (Android).
#ifndef CHURCHDAW_AUDIO_INSTRUMENTATION
 #define CHURCHDAW_AUDIO_INSTRUMENTATION 0
#endif

// Streaming do disco em TODAS as plataformas.
//
// Nao e apenas uma otimizacao de celular. Um unico Amber Upright instalado no
// PC tem 880 AIFFs, 2,86 GiB em PCM de 24 bits; decodificado para float ocupa
// ~3,81 GiB. Durante uma troca, o motor precisa manter o timbre antigo ate as
// vozes terminarem, portanto o caminho antigo podia passar de 7,6 GiB com so
// dois pianos. Sob pressao de memoria, bad_alloc escapava da std::thread e
// std::terminate encerrava o app com 0xC0000409.
//
// O mesmo preload + janelas que ja protege Android limita o custo no Windows e
// macOS sem alterar o formato da biblioteca. O define continua sobrescrevivel
// apenas para sondas isoladas; builds do produto usam streaming.
#ifndef CHURCHDAW_STREAM_SAMPLES
 #define CHURCHDAW_STREAM_SAMPLES 1
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "NordLookAndFeel.h"
#include "LevelMeter.h"
#include "MasterProcessor.h"
#include "TimbrePicker.h"
#include "BinaryData.h"
#include "ChurchDAWServices.h"
#include "AndroidCultoConnectivity.h"
#include "TimbreCatalog.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

//==============================================================================
// Estado pequeno e testavel do carregamento assíncrono de plugins. AUv3 exige
// que a message thread continue livre durante a instanciação; um ticket por
// destino permite descartar uma resposta antiga sem jamais retirar antes o
// plugin que ainda está tocando.
namespace ChurchDAWPluginLoading
{
class RequestGeneration
{
public:
    using Ticket = juce::uint64;

    Ticket begin() noexcept
    {
        const std::lock_guard<std::mutex> lock (commitGate);
        return generation.fetch_add (1, std::memory_order_acq_rel) + 1;
    }

    void cancel() noexcept
    {
        const std::lock_guard<std::mutex> lock (commitGate);
        generation.fetch_add (1, std::memory_order_acq_rel);
    }

    bool isCurrent (Ticket ticket) const noexcept
    {
        return ticket != 0
            && generation.load (std::memory_order_acquire) == ticket;
    }

    template <typename Commit>
    bool commitIfCurrent (Ticket ticket, Commit&& commit)
    {
        // Este gate e deliberadamente NAO recursivo. O lambda de commit deve
        // conter somente moves/flags locais, nunca codigo de vendor, callbacks
        // ou bombeamento de mensagens. Assim um begin/cancel reentrante nao
        // consegue alterar a geracao no meio do swap e tambem nao passa pelo
        // comportamento recursivo de juce::CriticalSection.
        const std::lock_guard<std::mutex> lock (commitGate);

        if (! isCurrent (ticket))
            return false;

        std::forward<Commit> (commit)();
        return true;
    }

private:
    std::mutex commitGate;
    std::atomic<Ticket> generation { 0 };
};

inline bool hasDuplicateName (
    const juce::Array<juce::PluginDescription>& choices,
    const juce::PluginDescription& candidate)
{
    int matches = 0;

    for (const auto& item : choices)
        if (item.name.trim().equalsIgnoreCase (candidate.name.trim())
             && ++matches > 1)
            return true;

    return false;
}

inline juce::String displayName (
    const juce::PluginDescription& plugin,
    const juce::Array<juce::PluginDescription>& choices)
{
    auto name = plugin.name.trim();

    if (name.isEmpty())
        name = "Plugin";

    if (! hasDuplicateName (choices, plugin))
        return name;

    auto format = plugin.pluginFormatName.trim();
    auto maker = plugin.manufacturerName.trim();

    if (format.isEmpty())
        format = "Formato desconhecido";

    if (maker.isEmpty())
        maker = "Fabricante desconhecido";

    return name + " [" + format + " | " + maker + "]";
}

inline int findChoiceByIdentifier (
    const juce::Array<juce::PluginDescription>& choices,
    const juce::String& identifier)
{
    for (int i = 0; i < choices.size(); ++i)
        if (choices.getReference (i).createIdentifierString()
             == identifier)
            return i;

    return -1;
}
}

//==============================================================================
// "Church Live" - instrumento nativo da ChurchDAW.
//
// Sampler com 5 canais de timbre em camadas: cada canal carrega um timbre da
// biblioteca (samples .aif/.wav com nota/velocidade no nome do arquivo) e tem
// seu proprio volume, transposicao, loop e medidor de nivel - tudo mapeavel
// via MIDI Learn. O carregamento dos samples roda em segundo plano.
//==============================================================================
class TimbresProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster
{
public:
    // 14 canais (31/07/2026). Eram 5 e virou pedido recorrente de cliente:
    // uma equipe com dois teclados, orgao, pads e naipe de sopros estoura
    // cinco em qualquer domingo. O teto e fixo de proposito -- o JUCE so
    // aceita criar parametros de automacao na CONSTRUCAO do processador,
    // entao "ilimitado" nao existe; o que existe e um teto generoso com a
    // mesa mostrando os canais em uso. Cada canal a mais custa 19
    // parametros, um buffer e uma cadeia de EQ/comp, nada por voz.
    static constexpr int numLayers = 14;
    static constexpr int churchOrganFamilyCount = 4;
    static_assert (
        static_cast<int> (
            churchorgan::ChurchOrganFamily::worshipHybrid) + 1
            == churchOrganFamilyCount,
        "O picker e o motor precisam expor exatamente as quatro familias Church Organ");

    // Modelo Nord: cada secao tem orcamento proprio e fechado de vozes, para que
    // um pad segurado no pedal nunca roube a polifonia do piano. Antes eram 64
    // vozes num pool unico -- e como cada tecla dispara uma voz POR camada, dois
    // acordes com pedal ja estouravam o pool e cortavam o piano no meio.
    // Canal 1 e o do piano principal (96 vozes: acorde com pedal segura
    // cauda longa); os demais carregam pad/orgao/naipe e 40 bastam. Como
    // funcao, o orcamento acompanha numLayers sem lista escrita a mao --
    // uma lista de 5 valores num array de 14 daria ZERO voz aos canais
    // novos, e eles simplesmente nao soariam.
    static constexpr int voicesForLayer (int layerIndex) noexcept
    {
        return layerIndex == 0 ? 96 : 40;
    }

    static constexpr int maxVoices = 96 + 40 * (numLayers - 1);
    static constexpr int realtimeBlockReserve = 8192;

    // Quanto de cada zona fica residente na RAM quando o resto vem do disco.
    //
    // ERREI FEIO AQUI NA PRIMEIRA VERSAO: deixei 0,15 s, raciocinando apenas
    // sobre o tempo que a thread leva para entregar a PRIMEIRA janela. So que
    // isso faz TODA nota que dure mais de 150 ms ir ao disco -- e com 20 vozes
    // soando sao 20 aberturas de FLAC, com busca e decodificacao, dentro de
    // 150 ms. O armazenamento do celular nao entrega, e cada atraso vira um
    // BLOCO DE SILENCIO no meio da nota, ou seja um clique.
    // Medido: 422, 478, 542 falhas POR SEGUNDO. Era esse o "pipoco".
    //
    // 0,75 s ainda entrega meio segundo inteiro de antecedencia para a primeira
    // leitura (o pedido sai aos 0,25 s), mas custa ~230 MB por piano de 880 zonas
    // em vez dos ~620 MB de 2 s. No aparelho, UM piano + janelas chegava perto
    // de 1 GB e o Android passava a comprimir/mover paginas; depois de algum
    // tempo isso virava pipoco e podia matar a rota de audio. O tocador worship
    // sustenta as notas por ~1,4 s, portanto o teste continuo exercita de verdade
    // a fronteira RAM/disco com este valor.
    static constexpr double preloadSeconds = 0.75;

    // RESERVA PARA A POLIFONIA (-6 dB).
    //
    // Um sample sozinho ja vem normalizado perto de 0 dBFS. Duas notas somam
    // ~+6 dB, quatro ~+12 -- entao um acorde comum estoura o teto mesmo com
    // todos os faders em 0 dB, e nao ha fader que conserte isso depois: o corte
    // acontece ANTES, quando as vozes se somam.
    //
    // Medido no aparelho com o tocador automatico, acordes de 4 notas e 9-13
    // vozes: pico chegou a +2,7 dBFS. Passar de 0 dBFS corta a onda no topo, e
    // piano cortado soa EXATAMENTE como pipoco -- foi isso o tempo todo, e nao
    // falha de audio: a medicao no mesmo instante acusava 0 blocos atrasados e
    // 6% de carga.
    //
    // Isto ficou pior por causa da propria calibragem de dinamica: ela levantou
    // o nivel MEDIO em ~5 dB (o pico de uma nota so continuou 1.0). Ou seja, a
    // reserva ja era necessaria antes e virou obrigatoria depois.
    //
    // A primeira correcao usava -9 dB porque ainda nao havia protecao final no
    // bus. -6 dB continuou conservador demais: o cliente precisava levar o
    // MASTER ao maximo para um nivel normal. O limiter true-peak com lookahead
    // permite calibrar a voz em -4 dB; acordes densos continuam protegidos sem
    // sacrificar o corpo de uma nota ou camada comum em Master 0 dB.
    static constexpr float polyphonyHeadroom = 0.6309573f; // -4,00 dB
    static constexpr float organVelocityGain = 1.0f;       // tonewheel nao e velocity-sensitive
    static constexpr float nativePadAmbienceTrim = 1.6788040f; // +4,50 dB

    // Calibracao por FAMILIA, resolvida na troca de timbre (message/worker
    // completion thread), nunca no callback. A medicao integral do acervo
    // mostrou que pads/ambiências nativos ficam 6--12 dB abaixo mesmo sem
    // encostar no limiter; +4,5 dB recupera corpo sem mexer em piano, orgao,
    // VST/AU ou no ganho global.
    static float nativeFamilyTrimForCategory (
        const juce::String& category) noexcept
    {
        return category.equalsIgnoreCase ("PADS")
                || category.equalsIgnoreCase ("AMBIENCIAS")
             ? nativePadAmbienceTrim
             : 1.0f;
    }

    // Decaimento da rampa de declique do roubo de voz: cai ~60 dB em 1 ms a
    // 48 kHz. Curto o bastante para nao borrar o ataque da nota nova, longo o
    // bastante para o degrau nao ser audivel.
    static constexpr float declickDecay = 0.9858f;

    //==============================================================================
    struct TimbreEntry
    {
        juce::String label;
        juce::Array<juce::File> files;
        juce::String category;   // secao vinda da pasta (ex.: "01. Pianos" -> PIANOS)
        juce::String library;    // biblioteca de origem (Live Pianos, REDKEYS, ...)
    };

    //==============================================================================
    // Resposta do teclado (curva de velocity) -- indices do parametro "velcurve".
    enum { curveVeryLight = 0, curveLight, curveNatural, curveHeavy, curveFixed };

    // Reescreve a velocity que chegou do controlador. Expoente MENOR que 1
    // levanta as notas fracas (o teclado passa a exigir menos forca pra soar
    // cheio -- teclas pesadas de martelo); MAIOR que 1 abaixa (sintetizador
    // leve que manda 120 em qualquer toque). "Fixa" descarta a dinamica, que e
    // o que se quer em orgao e pad.
    static int mapVelocity (int vel, int curve) noexcept
    {
        if (curve == curveFixed)
            return 100;

        float e = 1.0f;

        switch (curve)
        {
            case curveVeryLight: e = 0.45f; break;
            case curveLight:     e = 0.65f; break;
            case curveHeavy:     e = 1.60f; break;
            default:             return vel;   // Natural = o que o teclado mandou
        }

        const float v = juce::jlimit (0.0f, 1.0f, (float) vel / 127.0f);
        return juce::jlimit (1, 127, juce::roundToInt (std::pow (v, e) * 127.0f));
    }

    struct LibraryDef { juce::String name; juce::File folder; };

    // todas as bibliotecas nativas disponiveis - permite escolher timbre de
    // qualquer uma dentro de qualquer canal; so as que existem no disco entram
    static juce::Array<LibraryDef> knownLibraries()
    {
        juce::Array<LibraryDef> libs;

        auto add = [&libs] (const juce::String& name, const juce::File& folder)
        {
            if (folder.isDirectory())
                libs.add ({ name, folder });
        };

        // O nome da biblioteca aparece como secao no seletor de timbres. Por
        // acordo com o autor do acervo de pianos, ele entra no produto sob a
        // marca da casa -- nada na interface identifica a biblioteca de origem.
        add ("Church Live", defaultSamplesFolder());
        add ("REDKEYS",     redkeysSamplesFolder());
        add ("Worship",     worshipSamplesFolder());
        add ("BASELAB",     baselabSamplesFolder());

        // Packs novos nao exigem uma versao nova do motor. O instalador interno
        // grava um marcador assinado pelo nosso catalogo em cada pasta; qualquer
        // pack valido passa a aparecer como uma secao no seletor de timbres.
        const auto managed = AppPaths::managedLibrariesFolder();

        if (managed.isDirectory())
            for (const auto& folder : managed.findChildFiles (juce::File::findDirectories, false))
            {
                // Staging, backup e demais pastas transacionais nunca podem
                // entrar no motor, mesmo se uma queda ocorreu depois de o
                // marcador ser escrito. A recuperacao do instalador tenta
                // limpá-las; este filtro e a ultima barreira independente.
                if (folder.getFileName().startsWithChar ('.'))
                    continue;

                const auto marker = folder.getChildFile (".churchdaw-library.json");

                if (! marker.existsAsFile())
                    continue;   // download incompleto/staging nunca vira biblioteca

                bool alreadyAdded = false;

                for (const auto& lib : libs)
                    if (lib.folder == folder)
                    {
                        alreadyAdded = true;
                        break;
                    }

                if (alreadyAdded)
                    continue;

                const auto parsed = juce::JSON::parse (marker);
                auto* object = parsed.getDynamicObject();

                if (object == nullptr
                     || object->getProperty ("id").toString().trim().isEmpty()
                     || (int) object->getProperty ("version") < 1)
                    continue;

                auto name = object->getProperty ("name").toString().trim();

                if (name.isEmpty())
                    name = folder.getFileName();

                add (name, folder);
            }

        return libs;
    }

    static juce::File holyKeysSamplesFolder()
    {
        // No macOS o executavel fica em ChurchDAW.app/Contents/MacOS. appFolder()
        // sobe ate a pasta que o usuario enxerga; no Windows continua sendo a
        // pasta do .exe.
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library6", "HolyKeys" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return f;
        }

        auto installed = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                             .getChildFile ("ChurchDAW").getChildFile ("Library6");

        if (installed.isDirectory())
            return installed;

        return {};   // nao achou: o usuario aponta a pasta em Audio/MIDI
    }

    static juce::File studioPianosSamplesFolder()
    {
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library5", "StudioPianos" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return f;
        }

        auto installed = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                             .getChildFile ("ChurchDAW").getChildFile ("Library5");

        if (installed.isDirectory())
            return installed;

        return {};
    }

    static juce::File defaultSamplesFolder()
    {
        // Destino do instalador Android 1.2+. Fica dentro do armazenamento
        // privado do app, portanto nao requer READ_MEDIA_AUDIO.
        for (const char* name : { "Library", "ChurchLive", "church-live" })
            if (auto f = AppPaths::managedLibrariesFolder().getChildFile (name); f.isDirectory())
                return f;

        // modo portatil: biblioteca ao lado do executavel tem prioridade
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library", "ChurchLive Library", "SAMPLES" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return f;
        }

        // biblioteca instalada pelo instalador (ProgramData / Application Support)
        auto installed = AppPaths::sharedDataFolder().getChildFile ("Library");

        if (installed.isDirectory())
            return installed;

        // pasta apontada pelo usuario, procurando fundo: packs comerciais vem em
        // arvores como "Live Pianos 4.0/Live Pianos 4.0/SAMPLES"
        return AppPaths::findLibraryDeep ({ "SAMPLES", "Samples", "Library", "ChurchLive Library" });
    }

    // "C#4" -> nota MIDI (convencao do Ableton: C3 = 60); -1 se invalido
    static int noteNameToMidi (const juce::String& n)
    {
        if (n.isEmpty())
            return -1;

        static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        const auto letter = (juce::juce_wchar) juce::CharacterFunctions::toUpperCase (n[0]);

        if (letter < 'A' || letter > 'G')
            return -1;

        int semi = semis[(int) (letter - 'A')];
        int pos = 1;

        if (pos < n.length() && n[pos] == '#') { ++semi; ++pos; }
        else if (pos < n.length() && n[pos] == 'b') { --semi; ++pos; }

        const auto octStr = n.substring (pos);

        if (octStr.isEmpty() || octStr.retainCharacters ("-0123456789") != octStr)
            return -1;

        const int midi = (octStr.getIntValue() + 2) * 12 + semi;
        return (midi >= 0 && midi < 128) ? midi : -1;
    }

    // "B3 Fabio-A0-V127-0B8J" -> instrumento / nota / velocidade
    static bool parseSampleName (const juce::String& stem, juce::String& instr, int& note, int& vel)
    {
        auto parts = juce::StringArray::fromTokens (stem, "-", "");

        if (parts.size() < 4)
            return false;

        const int velIdx = parts.size() - 2;
        const auto velTok = parts[velIdx];

        // aceita "V127" (Live Pianos) e "47" (REDKEYS) como velocidade
        const auto digits = velTok.startsWithChar ('V') ? velTok.substring (1) : velTok;

        if (digits.isEmpty() || ! digits.containsOnly ("0123456789"))
            return false;

        vel = digits.getIntValue();

        if (vel < 1 || vel > 127)
            return false;

        int noteIdx = velIdx - 1;
        juce::String noteTok = parts[noteIdx];

        // octava negativa ("C#-1") foi dividida em dois tokens
        if (noteTok.containsOnly ("0123456789") && noteIdx > 0)
        {
            noteTok = parts[noteIdx - 1] + "-" + noteTok;
            --noteIdx;
        }

        note = noteNameToMidi (noteTok);

        if (note < 0 || noteIdx < 1)
            return false;

        juce::StringArray nameParts;

        for (int i = 0; i < noteIdx; ++i)
            nameParts.add (parts[i]);

        instr = nameParts.joinIntoString ("-").trim();
        return instr.isNotEmpty();
    }

    // "10ANALOG" -> zona 10 do timbre "ANALOG"; zonas de 3 em 3 semitons a
    // partir de C0 (mesmo layout dos conjuntos de 29 samples com nota no nome)
    static bool parseNumberedName (const juce::String& stem, juce::String& instr, int& note)
    {
        int i = 0;

        while (i < stem.length() && juce::CharacterFunctions::isDigit (stem[i]))
            ++i;

        if (i == 0 || i >= stem.length())
            return false;

        const int index = stem.substring (0, i).getIntValue();
        instr = stem.substring (i).trim();

        if (index < 1 || instr.isEmpty() || ! juce::CharacterFunctions::isLetter (instr[0]))
            return false;

        note = 24 + (index - 1) * 3;   // C0, D#0, F#0, A0, C1... (convencao Ableton)
        return note < 128;
    }

    // "GS ELEVATE GRAND 060 127" ou "GS_IMPERIALGRAND_098_127" -> instrumento /
    // nota MIDI crua / velocidade. Tenta ESPACO primeiro (preserva nomes com
    // underscore tipo Worship "felt_gp"); se nao houver, tenta UNDERSCORE.
    static bool parseSpaceMidiTok (const juce::String& stem, const juce::String& sep,
                                   juce::String& instr, int& note, int& vel)
    {
        auto parts = juce::StringArray::fromTokens (stem, sep, "");
        parts.removeEmptyStrings();

        if (parts.size() < 3)
            return false;

        const auto velTok = parts[parts.size() - 1];
        const auto noteTok = parts[parts.size() - 2];

        if (! velTok.containsOnly ("0123456789") || ! noteTok.containsOnly ("0123456789"))
            return false;

        note = noteTok.getIntValue();
        vel = juce::jlimit (1, 127, velTok.getIntValue());

        if (note < 0 || note > 127)
            return false;

        juce::StringArray nameParts;

        for (int i = 0; i < parts.size() - 2; ++i)
            nameParts.add (parts[i]);

        instr = nameParts.joinIntoString (" ").trim();
        return instr.isNotEmpty();
    }

    static bool parseSpaceMidiName (const juce::String& stem, juce::String& instr, int& note, int& vel)
    {
        if (stem.containsChar (' ') && parseSpaceMidiTok (stem, " ", instr, note, vel))
            return true;

        return parseSpaceMidiTok (stem, "_", instr, note, vel);
    }

    // "A#3", "E0-2", "C" -> nota (sem octava assume a octava central)
    static bool parseNoteOnlyName (const juce::String& stem, int& note)
    {
        auto s = stem.trim();

        // remove sufixo de camada tipo "-2"
        const int dash = s.lastIndexOfChar ('-');

        if (dash > 0 && s.substring (dash + 1).containsOnly ("0123456789"))
            s = s.substring (0, dash);

        if (s.isEmpty())
            return false;

        const auto letter = (juce::juce_wchar) juce::CharacterFunctions::toUpperCase (s[0]);

        if (letter < 'A' || letter > 'G')
            return false;

        static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };
        int semi = semis[(int) (letter - 'A')];
        int pos = 1;

        if (pos < s.length() && s[pos] == '#') { ++semi; ++pos; }
        else if (pos < s.length() && s[pos] == 'b') { --semi; ++pos; }

        const auto octStr = s.substring (pos);

        if (octStr.isEmpty())
            note = 60 + semi;                                        // sem octava: central
        else if (octStr.retainCharacters ("-0123456789") == octStr)
            note = (octStr.getIntValue() + 2) * 12 + semi;
        else
            return false;

        return note >= 0 && note < 128;
    }

    // secao a partir do nome de uma pasta ancestral ("01. Pianos" -> PIANOS)
    static juce::String categoryFromFolderName (const juce::String& s)
    {
        const auto n = s.toLowerCase();

        if (n.contains ("ambien") || n.contains ("atmo"))   return "AMBIENCIAS";
        if (n.contains ("bell") || n.contains ("sino"))     return "BELLS";
        if (n.contains ("organ")
             || n.contains ("orgao")
             || n.contains (juce::String::fromUTF8 ("org\xc3\xa3o"))
             || n.contains (juce::String::fromUTF8 ("\xc3\xb3rg\xc3\xa3o"))
             || n.contains ("b3")
             || n.contains ("farf")
             || n.contains ("pipe"))                            return "ORGAOS";
        if (n.contains ("string") || n.contains ("corda"))  return "STRINGS";
        if (n.contains ("pad"))                             return "PADS";
        if (n.contains ("lead") || n.contains ("synth"))    return "LEADS & SYNTHS";
        if (n.contains ("digital") || n.contains ("eletric") || n.contains ("electric")
             || n.contains ("rhodes") || n.contains ("tines") || n.contains ("keys") || n.startsWith ("ep")) return "EP & TECLAS";
        if (n.contains ("piano") || n.contains ("grand") || n.contains ("upright")) return "PIANOS";

        return {};
    }

    static bool looksLikeOrgan (const juce::String& name,
                                const juce::String& category)
    {
        const auto text = (name + " " + category).toLowerCase();
        return text.contains ("organ")
                || text.contains ("orgao")
                || text.contains (juce::String::fromUTF8 ("org\xc3\xa3o"))
                || text.contains (juce::String::fromUTF8 ("\xc3\xb3rg\xc3\xa3o"))
                || text.contains ("b3")
                || text.contains ("farf")
                || text.contains ("pipe");
    }

    // Política apenas de DESCOBERTA no seletor. Os timbres sampleados de órgão
    // continuam no índice e nos caminhos de restore para que projetos antigos
    // abram sem alteração; eles só deixam de ser oferecidos para novas escolhas,
    // pois o produto agora possui quatro famílias próprias de Church Organ.
    static bool isSampleTimbreVisibleForNewSelection (
        const TimbreEntry& entry)
    {
        const auto effectiveCategory =
            entry.category.isNotEmpty()
                ? entry.category
                : categoryFromFolderName (entry.label);

        return ! looksLikeOrgan (
            entry.label, effectiveCategory);
    }

    // varre recursivamente: cada pasta com audio vira um timbre; pastas gigantes
    // tipo "Imported" sao desmembradas pelo nome do instrumento; a secao vem
    // das pastas ancestrais (ex.: "01. Pianos/04. Digital Pianos/Dyno Piano RX")
    static void scanFolderInto (const juce::File& dir, const juce::String& inheritedCategory,
                                juce::Array<TimbreEntry>& out, int depth,
                                const std::atomic<bool>* abort = nullptr)
    {
        if (depth > 6)
            return;

        // cancelamento por pasta: o destrutor liga o flag e o join nao espera
        // a varredura inteira do disco
        if (abort != nullptr && abort->load())
            return;

        auto category = categoryFromFolderName (dir.getFileName());

        if (category.isEmpty())
            category = inheritedCategory;

        auto audioFiles = dir.findChildFiles (juce::File::findFiles, false, "*.aif;*.aiff;*.wav;*.flac");

        if (! audioFiles.isEmpty())
        {
            if (audioFiles.size() > 120 || dir.getFileName().equalsIgnoreCase ("Imported"))
            {
                std::map<juce::String, TimbreEntry> groups;

                for (const auto& f : audioFiles)
                {
                    juce::String instr;
                    int note = 0, vel = 0;
                    const auto stem = f.getFileNameWithoutExtension();

                    if (parseSampleName (stem, instr, note, vel)
                         || parseSpaceMidiName (stem, instr, note, vel)
                         || parseNumberedName (stem, instr, note))
                    {
                        auto& e = groups[instr];
                        e.label = instr;
                        e.category = category;
                        e.files.add (f);
                    }
                    else if (parseNoteOnlyName (stem, note))
                    {
                        auto& e = groups["Ambiencia"];
                        e.label = "Ambiencia";
                        e.category = "AMBIENCIAS";
                        e.files.add (f);
                    }
                }

                for (auto& g : groups)
                    out.add (std::move (g.second));
            }
            else
            {
                TimbreEntry e;
                e.label = dir.getFileName();
                e.files = audioFiles;
                e.category = category;
                out.add (std::move (e));
            }
        }

        for (const auto& sub : dir.findChildFiles (juce::File::findDirectories, false))
        {
            if (abort != nullptr && abort->load())
                return;

            if (! sub.getFileName().startsWith ("__MACOSX"))
                scanFolderInto (sub, category, out, depth + 1, abort);
        }
    }

    static juce::Array<TimbreEntry> scanTimbres (const juce::File& samplesRoot,
                                                 const std::atomic<bool>* abort = nullptr)
    {
        juce::Array<TimbreEntry> out;
        scanFolderInto (samplesRoot, {}, out, 0, abort);

        std::sort (out.begin(), out.end(),
                   [] (const TimbreEntry& a, const TimbreEntry& b) { return a.label < b.label; });
        return out;
    }

    // Indice persistente da biblioteca. No Android, enumerar 9 mil samples pelo
    // FUSE/MediaProvider a cada abertura custa segundos mesmo quando nada mudou.
    // O cache guarda somente metadados e caminhos; o audio continua vindo dos
    // arquivos originais. A assinatura usa a raiz e as pastas imediatamente
    // abaixo dela: cobre a organizacao normal da ChurchDAW sem reenumerar todos
    // os samples so para decidir se o cache ainda vale.
    static juce::File entriesCacheFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ChurchDAW")
                   .getChildFile ("timbre-index-v2.bin");
    }

    static juce::uint64 librariesSignature (const juce::Array<LibraryDef>& libraries)
    {
        juce::uint64 hash = 1469598103934665603ULL;   // FNV-1a 64

        auto mixString = [&hash] (const juce::String& text)
        {
            const auto utf8 = text.toUTF8();

            for (auto* p = reinterpret_cast<const unsigned char*> (utf8.getAddress()); *p != 0; ++p)
            {
                hash ^= (juce::uint64) *p;
                hash *= 1099511628211ULL;
            }
        };

        auto mixTime = [&mixString] (const juce::File& file)
        {
            mixString (juce::String (file.getLastModificationTime().toMilliseconds()));
        };

        for (const auto& lib : libraries)
        {
            mixString (lib.name);
            mixString (lib.folder.getFullPathName());
            mixTime (lib.folder);

            auto children = lib.folder.findChildFiles (juce::File::findDirectories, false);
            std::sort (children.begin(), children.end(), [] (const juce::File& a, const juce::File& b)
            {
                return a.getFullPathName().compareNatural (b.getFullPathName()) < 0;
            });

            for (const auto& child : children)
            {
                mixString (child.getFileName());
                mixTime (child);
            }
        }

        return hash;
    }

    static bool loadEntriesCache (const juce::Array<LibraryDef>& libraries,
                                  juce::Array<TimbreEntry>& entries)
    {
        juce::MemoryBlock data;
        const auto file = entriesCacheFile();

        if (! file.existsAsFile() || ! file.loadFileAsData (data)
             || data.getSize() < 24 || data.getSize() > 64 * 1024 * 1024)
            return false;

        juce::MemoryInputStream in (data, false);

        if (in.readInt() != 0x43444958 || in.readInt() != 2)   // "CDIX", formato 2
            return false;

        if ((juce::uint64) in.readInt64() != librariesSignature (libraries))
            return false;

        const int entryCount = in.readInt();

        if (entryCount < 0 || entryCount > 50000)
            return false;

        juce::Array<TimbreEntry> restored;
        restored.ensureStorageAllocated (entryCount);
        int totalFiles = 0;

        for (int i = 0; i < entryCount && ! in.isExhausted(); ++i)
        {
            TimbreEntry entry;
            entry.label    = in.readString();
            entry.category = in.readString();
            entry.library  = in.readString();

            const int fileCount = in.readInt();

            if (fileCount < 0 || fileCount > 50000
                 || (totalFiles += fileCount) > 500000)
                return false;

            for (int f = 0; f < fileCount; ++f)
            {
                if (in.isExhausted())
                    return false;

                entry.files.add (juce::File (in.readString()));
            }

            restored.add (std::move (entry));
        }

        if (restored.size() != entryCount)
            return false;

        entries = std::move (restored);
        juce::Logger::writeToLog ("BIBLIOTECA: indice rapido carregado ("
                                  + juce::String (entries.size()) + " timbres, "
                                  + juce::String (totalFiles) + " samples)");
        return true;
    }

    static void saveEntriesCache (const juce::Array<LibraryDef>& libraries,
                                  const juce::Array<TimbreEntry>& entries)
    {
        juce::MemoryOutputStream out;
        out.writeInt (0x43444958);   // "CDIX"
        out.writeInt (2);
        out.writeInt64 ((juce::int64) librariesSignature (libraries));
        out.writeInt (entries.size());

        for (const auto& entry : entries)
        {
            out.writeString (entry.label);
            out.writeString (entry.category);
            out.writeString (entry.library);
            out.writeInt (entry.files.size());

            for (const auto& sample : entry.files)
                out.writeString (sample.getFullPathName());
        }

        const auto file = entriesCacheFile();
        file.getParentDirectory().createDirectory();

        if (file.replaceWithData (out.getData(), out.getDataSize()))
            juce::Logger::writeToLog ("BIBLIOTECA: indice atualizado ("
                                      + juce::String (entries.size()) + " timbres)");
    }

    static std::mutex& libraryScanMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    // escaneia TODAS as bibliotecas nativas, marcando a origem de cada timbre;
    // "abort" (opcional) cancela entre pastas -- usado no shutdown do processador
    static juce::Array<TimbreEntry> scanAllLibraries (const std::atomic<bool>* abort = nullptr,
                                                      bool allowCache = true)
    {
        // Existem varios TimbresProcessor (um por slot/canal). Depois de instalar
        // um pack todos pedem atualizacao quase juntos. Sem coordenacao, cada um
        // enumerava os mesmos 13 GB e cinco varreduras competiam pelo FUSE, pelo
        // cache e pelo audio. O primeiro atualiza o indice; os seguintes entram
        // depois e carregam esse mesmo indice em milissegundos.
        std::lock_guard<std::mutex> scanLock (libraryScanMutex());

        const auto libraries = knownLibraries();
        juce::Array<TimbreEntry> out;

        if (allowCache && loadEntriesCache (libraries, out))
            return out;

        for (const auto& lib : libraries)
        {
            if (abort != nullptr && abort->load())
                return out;

            auto entries = scanTimbres (lib.folder, abort);

            for (auto& e : entries)
            {
                e.library = lib.name;
                out.add (std::move (e));
            }
        }

        // Nunca substitua um indice valido por um cache vazio. No Android,
        // uma permissao ainda nao concedida (ou uma biblioteca em copia pelo
        // USB) pode fazer a primeira varredura enxergar zero timbres; persistir
        // esse resultado deixaria as proximas aberturas instantaneas, mas vazias.
        if ((abort == nullptr || ! abort->load()) && ! out.isEmpty())
            saveEntriesCache (libraries, out);

        return out;
    }

    //==============================================================================
    TimbresProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Saida", juce::AudioChannelSet::stereo(), true))
    {
        // Valores neutros de cada canal. Ficam AQUI, num laco, e nao em
        // listas escritas a mao junto do membro: com 14 canais uma lista
        // de cinco valores deixaria os nove ultimos com zero -- cor fixa
        // errada, pedal "solto" em vez de desconhecido e trim mudo. Erro
        // silencioso, do tipo que so aparece no meio do culto.
        for (int l = 0; l < numLayers; ++l)
        {
            layerColourIdx[l] = -1;                     // segue a categoria
            lastChurchOrganChannelRotaryMode[l].store (-1);
            churchOrganSustainMirror[l].store (sustainStateUnknown);
            pluginSustainMirror[l].store (sustainStateUnknown);
            layerNativeFamilyTrim[l].store (1.0f);      // ganho unitario
        }

        for (int l = 0; l < numLayers; ++l)
        {
            // O adapter reúne os quatro motores e vive pelo tempo do canal.
            // A alocação acontece na construção/message thread; prepareToPlay
            // reserva todo o storage de áudio antes do primeiro callback.
            layers[l].churchOrgan =
                std::make_unique<
                    churchorgan::ChurchOrganLayerProcessor>();

            // curva historica ate um timbre ser carregado e medido: canal vazio
            // nunca toca, mas isto evita ler lixo se algo disparar antes da carga
            for (int v = 0; v < 128; ++v)
                layers[l].velLut[v] = legacyVelGain (v);

            const auto n = juce::String (l + 1);
            addParameter (layers[l].vol = new juce::AudioParameterFloat (
                { "vol" + juce::String (l), 1 }, "Canal " + n + " Volume", 0.0f, 1.5f, 1.0f));
            addParameter (layers[l].transp = new juce::AudioParameterInt (
                { "transp" + juce::String (l), 1 }, "Canal " + n + " Transpor", -24, 24, 0));
            addParameter (layers[l].loop = new juce::AudioParameterBool (
                { "loop" + juce::String (l), 1 }, "Canal " + n + " Loop", false));
            addParameter (layers[l].mute = new juce::AudioParameterBool (
                { "mute" + juce::String (l), 1 }, "Canal " + n + " Mute", false));
            addParameter (layers[l].solo = new juce::AudioParameterBool (
                { "solo" + juce::String (l), 1 }, "Canal " + n + " Solo", false));
            addParameter (layers[l].on = new juce::AudioParameterBool (
                { "on" + juce::String (l), 1 }, "Canal " + n + " Ligado", true));
            addParameter (layers[l].comp = new juce::AudioParameterFloat (
                { "comp" + juce::String (l), 1 }, "Canal " + n + " Compressor", 0.0f, 1.0f, 0.0f));
            addParameter (layers[l].delay = new juce::AudioParameterFloat (
                { "dly" + juce::String (l), 1 }, "Canal " + n + " Delay", 0.0f, 1.0f, 0.0f));
            // IDs, ordem e faixa historicos. Estes dois parametros continuam
            // sendo os filtros HPF/LPF antigos para que setups de clientes
            // mantenham exatamente o mesmo som. O novo EQ de tres bandas e
            // acrescentado depois de todos os parametros legados.
            addParameter (layers[l].lowcut = new juce::AudioParameterFloat (
                { "lcut" + juce::String (l), 1 }, "Canal " + n + " Low Cut",
                0.0f, 1.0f, 0.0f));
            addParameter (layers[l].highcut = new juce::AudioParameterFloat (
                { "hcut" + juce::String (l), 1 }, "Canal " + n + " High Cut",
                0.0f, 1.0f, 0.0f));
            addParameter (layers[l].autoEq = new juce::AudioParameterBool (
                { "aeq" + juce::String (l), 1 }, "Canal " + n + " Auto EQ", false));
            addParameter (layers[l].reverbSend = new juce::AudioParameterFloat (
                { "rvb" + juce::String (l), 1 }, "Canal " + n + " Reverb", 0.0f, 1.0f, 0.0f));
        }

        addParameter (gain = new juce::AudioParameterFloat ({ "gain", 1 }, "Volume geral", 0.0f, 1.5f, 1.0f));
        addParameter (globalTransp = new juce::AudioParameterInt ({ "gtransp", 1 }, "Transpor global", -12, 12, 0));
        addParameter (globalSustain = new juce::AudioParameterBool ({ "gsus", 1 }, "Sustain global", false));
        addParameter (pedalInvert = new juce::AudioParameterBool ({ "pedinv", 1 }, "Inverter pedal", false));

        // Cada controlador entrega a mesma forca com uma velocity diferente: um
        // sintetizador leve manda 120 onde um piano de martelo manda 70. Sem
        // isto, metade dos compradores acha o app "duro" e a outra metade acha
        // que "estoura", e nenhum dos dois tem o que fazer a respeito.
        addParameter (velCurve = new juce::AudioParameterChoice (
            { "velcurve", 1 }, "Resposta do teclado",
            { "Muito leve", "Leve", "Natural", "Pesada", "Fixa" }, curveNatural));

        // Reverb nativo 100% wet. Os envios rvb0..rvb4 ja existiam, mas no
        // Android nao produziam som porque dependiam de um plugin BigSky, e a
        // plataforma nao hospeda VST/AU. O preset e o retorno sao globais; a
        // quantidade continua independente em cada canal.
        addParameter (nativeReverbPreset = new juce::AudioParameterChoice (
            { "rvbpreset", 1 }, "Church Reverb",
            { "Bloom", "Cloud", "Chorale", "Shimmer", "Magneto", "Nonlinear",
              "Reflections", "Room", "Hall", "Plate", "Spring", "Swell" }, 1));
        addParameter (reverbReturn = new juce::AudioParameterFloat (
            { "rvbreturn", 1 }, "Reverb Return", 0.0f, 1.0f, 0.72f));

        // APENDIDOS depois de todos os parametros existentes: Attack/Release,
        // EQ de tres bandas e Rotary nao deslocam os indices historicos usados
        // por hosts, mapas MIDI e setups ja instalados.
        for (int l = 0; l < numLayers; ++l)
        {
            const auto n = juce::String (l + 1);
            addParameter (layers[l].attack = new juce::AudioParameterFloat (
                { "attack" + juce::String (l), 1 }, "Canal " + n + " Attack",
                juce::NormalisableRange<float> (0.001f, 2.0f, 0.001f, 0.35f),
                0.003f));
            addParameter (layers[l].release = new juce::AudioParameterFloat (
                { "release" + juce::String (l), 1 }, "Canal " + n + " Release",
                juce::NormalisableRange<float> (0.05f, 12.0f, 0.01f, 0.45f),
                0.55f));
            addParameter (layers[l].eqLow = new juce::AudioParameterFloat (
                { "eqLow" + juce::String (l), 1 }, "Canal " + n + " EQ Low",
                -12.0f, 12.0f, 0.0f));
            addParameter (layers[l].eqMid = new juce::AudioParameterFloat (
                { "eqMid" + juce::String (l), 1 }, "Canal " + n + " EQ Mid",
                -12.0f, 12.0f, 0.0f));
            addParameter (layers[l].eqHigh = new juce::AudioParameterFloat (
                { "eqHigh" + juce::String (l), 1 }, "Canal " + n + " EQ High",
                -12.0f, 12.0f, 0.0f));
            addParameter (layers[l].rotaryMode = new juce::AudioParameterChoice (
                { "rotMode" + juce::String (l), 1 },
                "Canal " + n + " Rotary",
                { "Slow", "Stop", "Fast" },
                ChurchDAWRotary::Speaker::stopMode));
            addParameter (layers[l].rotaryAmount = new juce::AudioParameterFloat (
                { "rotAmt" + juce::String (l), 1 },
                "Canal " + n + " Rotary Amount",
                0.0f, 1.0f, 0.0f));
        }

        // varre a biblioteca em segundo plano ja na criacao: abrir o painel
        // depois disso e instantaneo (a varredura sincrona travava o audio/UI)
        startEntriesScanAsync();
    }

    void setExternalMaster (MasterProcessor* m) noexcept    { externalMaster = m; }
    MasterProcessor* getExternalMaster() const noexcept     { return externalMaster; }

    using PluginLoadCompletion =
        std::function<void (bool, const juce::String&)>;

    enum class HostedPluginLocation
    {
        insert,
        instrument
    };

    using HostedPluginWillRetireCallback =
        std::function<void (HostedPluginLocation, int,
                            juce::AudioPluginInstance*)>;

    static void invokePluginLoadCompletionNoThrow (
        PluginLoadCompletion& completion,
        bool loaded,
        const juce::String& error) noexcept
    {
        if (completion == nullptr)
            return;

        try
        {
            completion (loaded, error);
        }
        catch (...)
        {
        }
    }

    // recursos da DAW para hospedar plugins dentro do canal master
    void setPluginResources (
        juce::KnownPluginList* list,
        juce::AudioPluginFormatManager* fm,
        const juce::StringArray* quarantine = nullptr) noexcept
    {
        if (formatManagerRef != fm)
        {
            for (int slot = 0; slot < numInserts; ++slot)
            {
                insertPluginLoadGeneration[slot].cancel();
                insertPluginLoadInFlight[slot].store (
                    false, std::memory_order_release);
            }

            for (int layer = 0; layer < numLayers; ++layer)
            {
                instrumentPluginLoadGeneration[layer].cancel();
                instrumentPluginLoadInFlight[layer].store (
                    false, std::memory_order_release);
            }
        }

        knownPluginsRef = list;
        formatManagerRef = fm;
        pluginQuarantineRef = quarantine;
    }

    juce::KnownPluginList* getKnownPlugins() const noexcept { return knownPluginsRef; }

    bool isHostedPluginBlocked (const juce::PluginDescription& description) const
    {
        const auto& file = description.fileOrIdentifier;

        return (knownPluginsRef != nullptr
                 && knownPluginsRef->isBlacklisted (file, true))
            || (pluginQuarantineRef != nullptr
                 && pluginQuarantineRef->contains (file, true));
    }

    void setHostedPluginWillRetireCallback (
        HostedPluginWillRetireCallback callback)
    {
        const juce::ScopedLock lock (
            hostedPluginEditorCallbackLock);
        hostedPluginWillRetire = std::move (callback);
    }

    // O destrutor de um editor de vendor pode abrir um loop modal e entregar um
    // callback AUv3 no meio da propria destruicao. Durante esta janela o
    // callback de carga fica coalescido no processador ate o teardown externo
    // terminar, evitando spin no loop modal e troca sob um editor desmontando.
    void beginHostedPluginEditorTeardown() noexcept
    {
        hostedPluginEditorTeardownDepth.fetch_add (
            1, std::memory_order_acq_rel);
    }

    void endHostedPluginEditorTeardown() noexcept
    {
        const int previous =
            hostedPluginEditorTeardownDepth.fetch_sub (
                1, std::memory_order_acq_rel);
        jassert (previous > 0);

        if (previous == 1)
            postDeferredHostedPluginFinishes();
    }

    bool getPendingInsertDescription (
        int slot, juce::PluginDescription& result)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts))
            return false;

        const juce::ScopedLock lock (getCallbackLock());

        if (! pendingInsertDescriptionValid[slot])
            return false;

        result = pendingInsertDescriptions[slot];
        return true;
    }

    bool getPendingInstrumentDescription (
        int layer, juce::PluginDescription& result) const
    {
        if (! juce::isPositiveAndBelow (layer, numLayers))
            return false;

        const juce::ScopedLock lock (layerIdentityLock);

        if (! pendingVstDescriptionValid[layer])
            return false;

        result = pendingVstDescriptions[layer];
        return true;
    }

    static constexpr int numInserts = 2;

    juce::AudioPluginInstance* getInsert (int slot) const
    {
        return juce::isPositiveAndBelow (slot, numInserts) ? inserts[slot].get() : nullptr;
    }

    //==============================================================================
    // Seletor de reverb NATIVO. O BigSky expoe "EFFECT TYPE" como parametro
    // continuo, mas ele so assume 12 valores -- as maquinas ficam em posicoes
    // regulares de i/11 (medido varrendo o parametro e lendo o texto de volta).
    // Com isso da pra trocar a maquina sem abrir a interface do plugin.
    static constexpr int numReverbMachines = 12;

    static const char* reverbMachineName (int i)
    {
        static const char* const names[numReverbMachines] =
            { "Bloom", "Cloud", "Chorale", "Shimmer", "Magneto", "Nonlinear",
              "Reflections", "Room", "Hall", "Plate", "Spring", "Swell" };

        return juce::isPositiveAndBelow (i, numReverbMachines) ? names[i] : "";
    }

    juce::AudioProcessorParameter* getInsertParamByName (int slot, const juce::String& name) const
    {
        if (auto* p = getInsert (slot))
            for (auto* par : p->getParameters())
                if (par->getName (48).containsIgnoreCase (name))
                    return par;

        return nullptr;
    }

    // Slot do reverb externo. O valor e atualizado apenas quando um insert
    // muda; consultar String/nome de plugin em todo callback de audio criava
    // trabalho desnecessario justamente na thread de tempo real.
    int getReverbSlot() const
    {
        return externalReverbSlot.load (std::memory_order_relaxed);
    }

    int getReverbMachine() const
    {
        const int slot = getReverbSlot();

        if (slot < 0)
            return nativeReverbPreset != nullptr ? nativeReverbPreset->getIndex() : 1;

        auto* par = getInsertParamByName (slot, "EFFECT TYPE");

        if (par == nullptr)
            return -1;

        // Pelo NOME que o proprio plugin devolve, nao por conta do valor. O
        // arredondamento assumia que a escala e exatamente i/11 -- e verdade no
        // que medi, mas se a Strymon mudar isso num update o seletor passaria a
        // mostrar a maquina errada em silencio. Casar o texto nunca mente.
        const auto txt = par->getCurrentValueAsText().trim();

        for (int i = 0; i < numReverbMachines; ++i)
            if (txt.equalsIgnoreCase (reverbMachineName (i)))
                return i;

        // fallback: se o texto nao bater com nada, usa a escala medida
        return juce::jlimit (0, numReverbMachines - 1,
                             juce::roundToInt (par->getValue() * (float) (numReverbMachines - 1)));
    }

    void setReverbMachine (int idx)
    {
        const int slot = getReverbSlot();

        if (! juce::isPositiveAndBelow (idx, numReverbMachines))
            return;

        // Espelha sempre a escolha no backend nativo. Assim, se o BigSky for
        // removido no meio do show, o fallback continua exatamente na mesma
        // maquina em vez de voltar para um preset antigo.
        if (nativeReverbPreset != nullptr && nativeReverbPreset->getIndex() != idx)
        {
            const float v = (float) idx / (float) (numReverbMachines - 1);
            nativeReverbPreset->beginChangeGesture();
            nativeReverbPreset->setValueNotifyingHost (v);
            nativeReverbPreset->endChangeGesture();
        }

        if (slot < 0)
            return;

        if (auto* par = getInsertParamByName (slot, "EFFECT TYPE"))
        {
            const float v = (float) idx / (float) (numReverbMachines - 1);
            par->beginChangeGesture();
            par->setValueNotifyingHost (v);
            par->endChangeGesture();
        }
    }

    juce::String getInsertName (int slot) const
    {
        if (auto* p = getInsert (slot))
            return p->getName();

        return {};
    }

    bool isUsingExternalReverb() const noexcept { return getReverbSlot() >= 0; }

    juce::AudioProcessorParameter* getReverbReturnParam() const noexcept
    {
        return reverbReturn;
    }

    void forceExternalReverbWet (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts)
             || slot != getReverbSlot()
             || ! getInsertName (slot).containsIgnoreCase ("bigsky"))
            return;

        if (auto* mix = externalReverbMixParam.load (std::memory_order_acquire))
        {
            mix->beginChangeGesture();
            mix->setValueNotifyingHost (1.0f);
            mix->endChangeGesture();
        }
    }

    void captureExternalReverbMix (int slot)
    {
        if (slot != getReverbSlot() || ! juce::isPositiveAndBelow (slot, numInserts))
            return;

        if (auto* mix = getInsertParamByName (slot, "MIX"))
        {
            externalReverbOriginalMix[slot] = mix->getValue();
            externalReverbOriginalMixValid[slot] = true;
        }
    }

    void restoreDemotedExternalReverbMix (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts)
             || ! externalReverbOriginalMixValid[slot])
            return;

        if (auto* mix = getInsertParamByName (slot, "MIX"))
            mix->setValueNotifyingHost (externalReverbOriginalMix[slot]);

        externalReverbOriginalMixValid[slot] = false;
    }

    void refreshExternalReverbSlot (int replacedSlot)
    {
        const int previous = getReverbSlot();
        int found = -1;

        for (int i = 0; i < numInserts; ++i)
            if (insertPrepared[i].load (
                    std::memory_order_acquire)
                 && getInsertName (i).containsIgnoreCase (
                        "bigsky"))
            {
                found = i;
                break;
            }

        // Se outro BigSky tomou o lugar de backend, o antigo volta ao MIX que
        // tinha antes de ser transformado em retorno 100% wet. Assim ele pode
        // continuar como efeito serial sem apagar o sinal seco do master.
        if (previous != found && previous != replacedSlot)
            restoreDemotedExternalReverbMix (previous);
        else if (previous != found && previous == replacedSlot
                 && juce::isPositiveAndBelow (previous, numInserts))
            externalReverbOriginalMixValid[previous] = false;

        auto* mix = found >= 0 ? getInsertParamByName (found, "MIX") : nullptr;
        auto* type = found >= 0 ? getInsertParamByName (found, "EFFECT TYPE") : nullptr;
        externalReverbMixParam.store (mix, std::memory_order_release);
        externalReverbTypeParam.store (type, std::memory_order_release);
        externalReverbSlot.store (found, std::memory_order_release);

        if (found >= 0 && (found != previous || found == replacedSlot))
            captureExternalReverbMix (found);

        // Nunca congele uma cauda nativa ao alternar de backend. Sem o reset,
        // remover o BigSky mais tarde faria uma cauda antiga reaparecer.
        if (previous != found)
            worshipReverb.reset();
    }

    // Agenda a criação sem bloquear a message thread. O retorno diz apenas se
    // o pedido foi aceito; sucesso/falha real chega em completion. O insert atual
    // continua processando até a nova instância estar criada, restaurada e
    // preparada (swap em duas fases, indispensável para AUv3).
    bool loadInsert (
        int slot,
        const juce::PluginDescription& desc,
        const juce::MemoryBlock* restoredState = nullptr,
        PluginLoadCompletion completion = {},
        bool preservePendingOnFailure = false)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts)
             || formatManagerRef == nullptr
             || isHostedPluginBlocked (desc))
            return false;

        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket = 0;

        try
        {
            juce::MemoryBlock stateToRestore;

            if (restoredState != nullptr && restoredState->getSize() > 0)
                stateToRestore = *restoredState;

            // Termine todas as copias que podem alocar antes de invalidar o
            // pedido anterior. Depois de begin(), apenas moves/clear entram no
            // estado compartilhado.
            auto requestedId = desc.createIdentifierString();
            auto requestedState = stateToRestore.toBase64Encoding();
            juce::PluginDescription requestedDescription (desc);
            ticket = insertPluginLoadGeneration[slot].begin();

            {
                const juce::ScopedLock callbackGuard (
                    getCallbackLock());
                pendingInsertIds[slot] = std::move (requestedId);
                pendingInsertStates[slot] =
                    std::move (requestedState);
                pendingInsertDescriptions[slot] =
                    std::move (requestedDescription);
                pendingInsertDescriptionValid[slot] = true;
                insertPluginLoadInFlight[slot].store (
                    true, std::memory_order_release);

                if (! preservePendingOnFailure)
                    insertStateRestorePending[slot].store (
                        false, std::memory_order_release);
            }

            juce::WeakReference<TimbresProcessor> weakThis (this);
            formatManagerRef->createPluginInstanceAsync (
                desc, hostSampleRate, juce::jmax (64, lastBlockSize),
                [weakThis, slot, ticket,
                 preservePendingOnFailure,
                 stateToRestore = std::move (stateToRestore),
                 completion = std::move (completion)] (
                    std::unique_ptr<juce::AudioPluginInstance> instance,
                    const juce::String& error) mutable
                {
                    struct Result
                    {
                        std::unique_ptr<juce::AudioPluginInstance> instance;
                        juce::String error;
                    };

                    auto result = std::make_shared<Result>();
                    result->instance = std::move (instance);
                    result->error = error;

                    auto deliver =
                        [weakThis, slot, ticket, result,
                         preservePendingOnFailure,
                         stateToRestore = std::move (stateToRestore),
                         completion = std::move (completion)] () mutable
                    {
                        if (auto* self = weakThis.get())
                            self->finishInsertPluginLoad (
                                slot, ticket,
                                std::move (result->instance),
                                result->error, stateToRestore,
                                std::move (completion),
                                preservePendingOnFailure);
                    };

                    if (juce::MessageManager::getInstance()
                            ->isThisTheMessageThread())
                        deliver();
                    else
                        (void) juce::MessageManager::callAsync (
                            std::move (deliver));
                });

            return true;
        }
        catch (...)
        {
            // O pedido nem chegou ao format manager. Se ele ja havia publicado
            // uma intencao interativa, volte ao plugin que continua tocando;
            // restore conserva o alvo para autocura no proximo boot.
            if (ticket != 0)
            {
                const juce::ScopedLock callbackGuard (
                    getCallbackLock());
                (void) insertPluginLoadGeneration[slot]
                    .commitIfCurrent (ticket, [this, slot,
                                               preservePendingOnFailure]
                {
                    insertPluginLoadInFlight[slot].store (
                        false, std::memory_order_release);

                    if (! preservePendingOnFailure)
                    {
                        pendingInsertIds[slot].clear();
                        pendingInsertStates[slot].clear();
                        pendingInsertDescriptionValid[slot] = false;
                        insertStateRestorePending[slot].store (
                            false, std::memory_order_release);
                    }
                });
            }
            return false;
        }
    }

    void finishInsertPluginLoad (
        int slot,
        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket,
        std::unique_ptr<juce::AudioPluginInstance> instance,
        const juce::String& creationError,
        const juce::MemoryBlock& restoredState,
        PluginLoadCompletion completion,
        bool preservePendingOnFailure)
    {
        jassert (juce::MessageManager::getInstance()
                    ->isThisTheMessageThread());

        if (! juce::isPositiveAndBelow (slot, numInserts)
             || ! insertPluginLoadGeneration[slot].isCurrent (ticket))
            return;

        if (isHostedPluginEditorTeardownActive())
        {
            try
            {
                struct DeferredResult
                {
                    std::unique_ptr<juce::AudioPluginInstance> instance;
                    juce::String error;
                    juce::MemoryBlock state;
                    PluginLoadCompletion completion;
                };

                auto deferred = std::make_shared<DeferredResult>();
                deferred->instance = std::move (instance);
                deferred->error = creationError;
                deferred->state = restoredState;
                deferred->completion = std::move (completion);
                juce::WeakReference<TimbresProcessor> weakThis (this);

                deferHostedPluginFinish (
                    HostedPluginLocation::insert, slot,
                    [weakThis, slot, ticket, deferred,
                     preservePendingOnFailure] () mutable
                    {
                        if (auto* self = weakThis.get())
                            self->finishInsertPluginLoad (
                                slot, ticket,
                                std::move (deferred->instance),
                                deferred->error, deferred->state,
                                std::move (deferred->completion),
                                preservePendingOnFailure);
                    });
            }
            catch (...)
            {
                // O alvo continua pendente e podera ser restaurado no proximo
                // boot; nunca troque uma instancia durante teardown de editor.
            }
            return;
        }

        auto failCurrentRequest =
            [this, slot, ticket, preservePendingOnFailure,
             &completion] (const juce::String& error) mutable
        {
            if (preservePendingOnFailure
                 && insertStateRestorePending[slot].load (
                        std::memory_order_acquire))
            {
                if (! finishFailedInsertStateRestore (
                        slot, ticket))
                    return;

                if (insertPluginLoadGeneration[slot]
                        .isCurrent (ticket))
                    invokePluginLoadCompletionNoThrow (
                        completion, false, error);
                return;
            }

            const bool stillCurrent = [&]
                {
                    const juce::ScopedLock callbackGuard (
                        getCallbackLock());
                    return insertPluginLoadGeneration[slot]
                        .commitIfCurrent (
                            ticket,
                            [this, slot,
                             preservePending = bool (preservePendingOnFailure)]
                    {
                        insertPluginLoadInFlight[slot].store (
                            false, std::memory_order_release);
                        insertStateRestorePending[slot].store (
                            false, std::memory_order_release);

                        if (! preservePending)
                        {
                            pendingInsertIds[slot].clear();
                            pendingInsertStates[slot].clear();
                            pendingInsertDescriptionValid[slot] = false;
                        }
                    });
                }();

            if (! stillCurrent)
                return;

            sendChangeMessage();
            invokePluginLoadCompletionNoThrow (
                completion, false, error);
        };

        const auto requestStillAllowed = [this, slot]
        {
            juce::PluginDescription pending;
            return getPendingInsertDescription (slot, pending)
                && ! isHostedPluginBlocked (pending);
        };

        if (! requestStillAllowed())
        {
            juce::Logger::writeToLog (
                "PLUGIN: insert bloqueado durante carga assincrona");
            failCurrentRequest (
                "Plugin foi colocado em quarentena durante a carga");
            return;
        }

        if (instance == nullptr)
        {
            juce::Logger::writeToLog (
                "PLUGIN: falha ao criar insert "
                + juce::String (slot + 1) + ": " + creationError);
            failCurrentRequest (creationError);

            return;
        }

        bool temporaryNeedsRelease = true;
        const juce::ScopeGuard releaseTemporaryOnExit {
            [this, &instance, &temporaryNeedsRelease]
        {
            if (temporaryNeedsRelease)
                releasePluginResourcesNoThrow (instance.get());
        } };

        try
        {
            instance->prepareToPlay (
                hostSampleRate,
                juce::jmax (64, lastBlockSize));

            if (restoredState.getSize() > 0)
                instance->setStateInformation (
                    restoredState.getData(),
                    (int) restoredState.getSize());
        }
        catch (...)
        {
            failCurrentRequest (
                "Plugin falhou durante prepare/restore");

            return;
        }

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        notifyHostedPluginWillRetire (
            HostedPluginLocation::insert, slot,
            getInsert (slot));

        // Fechar o editor pode bombear mensagens; o scanner pode concluir e
        // colocar exatamente este ID em quarentena nessa janela.
        if (! requestStillAllowed())
        {
            failCurrentRequest (
                "Plugin foi colocado em quarentena antes do commit");
            return;
        }

        // Fechar o editor pode bombear mensagens e iniciar outro pedido. O
        // commit atomico e revalidado DEPOIS disso; callback stale nao troca o
        // grafo nem apaga o pending do pedido mais novo.
        bool committed = false;
        {
            const bool resumeAfterSwap = ! isSuspended();

            if (resumeAfterSwap)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterSwap]
            {
                if (resumeAfterSwap)
                    suspendProcessing (false);
            } };

            const juce::ScopedLock callbackGuard (
                getCallbackLock());
            committed = insertPluginLoadGeneration[slot]
                .commitIfCurrent (ticket, [this, slot, &instance,
                                           &old, &oldWasPrepared,
                                           &temporaryNeedsRelease]
                {
                    oldWasPrepared =
                        insertPrepared[slot].exchange (
                        false,
                        std::memory_order_acq_rel);
                    old = std::move (inserts[slot]);
                    inserts[slot] = std::move (instance);
                    temporaryNeedsRelease = false;
                    insertPrepared[slot].store (
                        true,
                        std::memory_order_release);
                    pendingInsertIds[slot].clear();
                    pendingInsertStates[slot].clear();
                    pendingInsertDescriptionValid[slot] = false;
                    insertPluginLoadInFlight[slot].store (
                        false, std::memory_order_release);
                    insertStateRestorePending[slot].store (
                        false, std::memory_order_release);
                });
        }

        if (! committed)
            return;

        try
        {
            refreshExternalReverbSlot (slot);

            // Reverb como ENVIO exige o plugin 100% molhado: o seco ja vem do
            // caminho direto do canal. Com MIX no meio, o seco voltaria dobrado.
            forceExternalReverbWet (getReverbSlot());
        }
        catch (...)
        {
            juce::Logger::writeToLog (
                "PLUGIN: insert instalado; atualizacao do roteamento de reverb falhou");
        }

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        old.reset();

        sendChangeMessage();

        // releaseResources/destrutor do plugin antigo podem bombear a fila e
        // iniciar D. C ja foi instalado, mas nao conclui a UI nem toca no
        // estado persistente de D se deixou de ser o pedido atual.
        if (insertPluginLoadGeneration[slot].isCurrent (ticket))
            invokePluginLoadCompletionNoThrow (
                completion, true, {});
    }

    void unloadInsert (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts))
            return;

        notifyHostedPluginWillRetire (
            HostedPluginLocation::insert, slot,
            getInsert (slot));

        // A limpeza explicita deve vencer qualquer pedido entregue por um loop
        // modal aberto ao destruir o editor; por isso cancela DEPOIS do aviso.
        insertPluginLoadGeneration[slot].cancel();
        insertPluginLoadInFlight[slot].store (
            false, std::memory_order_release);
        insertStateRestorePending[slot].store (
            false, std::memory_order_release);

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        {
            const bool resumeAfterUnload = ! isSuspended();

            if (resumeAfterUnload)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterUnload]
            {
                if (resumeAfterUnload)
                    suspendProcessing (false);
            } };

            {
                const juce::ScopedLock callbackGuard (
                    getCallbackLock());
                oldWasPrepared =
                    insertPrepared[slot].exchange (
                    false,
                    std::memory_order_acq_rel);
                old = std::move (inserts[slot]);
                refreshExternalReverbSlot (slot);
                forceExternalReverbWet (
                    getReverbSlot());
            }
        }

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        old.reset();

        // slot esvaziado de proposito: estado pendente de restauracao cai junto
        {
            const juce::ScopedLock callbackGuard (
                getCallbackLock());
            pendingInsertIds[slot].clear();
            pendingInsertStates[slot].clear();
            pendingInsertDescriptionValid[slot] = false;
        }

        sendChangeMessage();
    }

    // corta todo o som na hora (botao PANIC)
    void panic()
    {
        const bool resumeAfterPanic = ! isSuspended();

        if (resumeAfterPanic)
            suspendProcessing (true);
        {
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterPanic]
            {
                if (resumeAfterPanic)
                    suspendProcessing (false);
            } };

            stopAllVoices();
            worshipReverb.reset();
            reverbBus.clear();

            for (auto& rotary : rotarySpeakers)
                rotary.reset();

            if (auto* external = getInsert (getReverbSlot()))
                external->reset();

            // O PANIC so parava as vozes NATIVAS: um VSTi hospedado no canal
            // (Kontakt) seguia tocando e nao havia como cala-lo pela DAW. Arme
            // ainda sob suspensão para o primeiro bloco retomado obrigatoriamente
            // entregar a fronteira a todos os destinos.
            panicPending.store (
                true,
                std::memory_order_release);
        }
    }

    // recalcula o EQ do canal (chamado ao trocar timbre ou ligar/desligar Auto EQ)
    void updateLayerEq (int l)
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return;

        // Pode ser chamado por ButtonParameterAttachment durante uma troca de
        // timbre que JA esta suspensa. Nunca religue o callback de um escopo
        // externo no meio da troca de zones/voices.
        const bool resumeAfterUpdate = ! isSuspended();

        if (resumeAfterUpdate)
            suspendProcessing (true);
        const juce::ScopeGuard restoreProcessing {
            [this, resumeAfterUpdate]
        {
            if (resumeAfterUpdate)
                suspendProcessing (false);
        } };

        eqs[l].setCoeffs (eqPresetFor (layerCategory[l], hostSampleRate));
        eqs[l].reset();
    }

    ~TimbresProcessor() override
    {
        for (auto& generation : insertPluginLoadGeneration)
            generation.cancel();

        for (auto& generation : instrumentPluginLoadGeneration)
            generation.cancel();

        // O editor precisa largar AudioProcessorEditor/GenericEditor antes que
        // qualquer instancia seja liberada. Gerações já canceladas tornam
        // inofensivo um callback AUv3 entregue por um loop modal do vendor.
        for (int slot = 0; slot < numInserts; ++slot)
            notifyHostedPluginWillRetire (
                HostedPluginLocation::insert, slot,
                getInsert (slot));

        for (int layer = 0; layer < numLayers; ++layer)
            notifyHostedPluginWillRetire (
                HostedPluginLocation::instrument, layer,
                getLayerInstrument (layer));

        setHostedPluginWillRetireCallback ({});
        releaseResources();

        masterReference.clear();

        for (auto& requestAbort : layerLoadAbort)
            if (requestAbort != nullptr)
                requestAbort->store (true, std::memory_order_release);

        // liga o abort ANTES do join: os workers checam entre arquivos/pastas e
        // retornam cedo -- o join espera no maximo ~1 arquivo, nao o scan todo
        workerAbort->store (true, std::memory_order_release);

        // O pool e persistente durante a vida do processador; nenhuma troca de
        // timbre cria uma thread do sistema. O token acima faz jobs longos
        // retornarem entre arquivos antes de aguardarmos a saida.
        workerPool.removeAllJobs (false, -1);
    }

    juce::File getRootFolder() const                    { return rootFolder; }

    // troca a biblioteca (Church Live 2 = REDKEYS) e reescaneia em segundo plano
    void setLibraryRoot (const juce::File& f)
    {
        rootFolder = f;
        entriesScanned = false;
        entriesCache.clear();
        startEntriesScanAsync();
    }

    static juce::File redkeysSamplesFolder()
    {
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library2", "REDKEYS" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return f;
        }

        auto installed = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                             .getChildFile ("ChurchDAW").getChildFile ("Library2");

        if (installed.isDirectory())
            return installed;

        // pasta apontada pelo usuario: acha a subpasta "Samples" onde estiver
        if (auto root = AppPaths::userLibraryRoot(); root.isDirectory())
            for (const char* n : { "Library2", "REDKEYS" })
                if (auto base = root.getChildFile (n); base.isDirectory())
                {
                    for (const auto& d : base.findChildFiles (juce::File::findDirectories, true))
                        if (d.getFileName().equalsIgnoreCase ("Samples"))
                            return d;

                    return base;
                }

        return {};
    }

    // LICENCA: desse acervo so os PADS foram liberados para uso comercial -- os
    // pianos (14 GB) ficaram de fora do acordo. Apontar a raiz da biblioteca
    // direto para a pasta de pads garante que o scanner nunca alcance o resto,
    // mesmo com o acervo inteiro no disco (o caso da maquina de desenvolvimento).
    // "DrumPads" nao conta: e percussao, nao pad.
    static juce::File worshipLicensedRoot (const juce::File& base)
    {
        if (! base.isDirectory())
            return base;

        for (const auto& d : base.findChildFiles (juce::File::findDirectories, true))
            if (d.getFileName().containsIgnoreCase ("Pads")
                 && ! d.getFileName().containsIgnoreCase ("Drum"))
                return d;

        // ja instalada so com os pads (o que o instalador entrega)
        return base;
    }

    static juce::File worshipSamplesFolder()
    {
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library3", "Worship" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return worshipLicensedRoot (f);
        }

        auto installed = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                             .getChildFile ("ChurchDAW").getChildFile ("Library3");

        if (installed.isDirectory())
            return worshipLicensedRoot (installed);

        if (auto root = AppPaths::userLibraryRoot(); root.isDirectory())
            for (const char* n : { "Library3", "Worship" })
                if (auto f = root.getChildFile (n); f.isDirectory())
                    return worshipLicensedRoot (f);

        return {};
    }

    static juce::File baselabSamplesFolder()
    {
        const auto exeDir = AppPaths::appFolder();

        for (const char* name : { "Library4", "BaseLab" })
        {
            auto f = exeDir.getChildFile (name);

            if (f.isDirectory())
                return f;
        }

        auto installed = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                             .getChildFile ("ChurchDAW").getChildFile ("Library4");

        if (installed.isDirectory())
            return installed;

        return {};
    }
    juce::String getLayerTimbre (int l) const
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return {};

        const juce::ScopedLock lock (layerIdentityLock);
        return layers[l].timbre;
    }

    juce::String getLayerLibrary (int l) const
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return {};

        const juce::ScopedLock lock (layerIdentityLock);
        return layers[l].libraryName;
    }

    // Nome de exibicao: tira os sufixos tecnicos do arquivo e normaliza a
    // capitalizacao. Vive AQUI (e nao no editor) porque o visor de programa do
    // MainComponent precisa exatamente do mesmo nome -- estava mostrando o cru
    // ("Black Upright LP4 FC") enquanto a regua ja mostrava o limpo.
    static juce::String prettyLabel (const juce::String& label)
    {
        // NOMES QUE A LIMPEZA AUTOMATICA NAO RESOLVE. Vem antes de tudo porque
        // dependem do rotulo CRU:
        //
        //  - "BELLP4": a regra de sufixo tira "LP4" e sobra "BEL" -- o L pertence
        //    a palavra BELL, nao ao codigo. Nenhuma ordem de sufixos acerta os
        //    dois casos ("FXLP4" precisa perder LP4 inteiro).
        //
        //  - "FXLP4", "FX NS4" e "FXFC2" limpavam TODOS para "FX": tres timbres
        //    diferentes com o mesmo nome na lista, impossivel de distinguir na
        //    hora de escolher.
        struct Exact { const char* from; const char* to; };

        static const Exact exact[] =
        {
            { "BELLP4",  "Sinos 2" },
            { "BELL FC", "Sinos" },
            { "FXLP4",   "Efeitos" },
            { "FX NS4",  "Efeitos 2" },
            { "FXFC2",   "Efeitos 3" },
        };

        for (const auto& e : exact)
            if (label.trim().equalsIgnoreCase (e.from))
                return juce::String::fromUTF8 (e.to);

        auto s = label.trim().replaceCharacter ('_', ' ').replaceCharacter ('-', ' ');

        // LP4/FC sao codigos internos do acervo, nao nomes de produto. Remove
        // tanto tokens separados ("Black Upright LP4 FC") quanto o codigo
        // colado ao nome ("BELLP4" -> "BELL"), sem alterar a chave usada para
        // localizar o timbre no disco.
        juce::StringArray cleanTokens;
        cleanTokens.addTokens (s, " ", {});
        cleanTokens.removeEmptyStrings();

        for (int i = cleanTokens.size(); --i >= 0;)
        {
            auto token = cleanTokens[i];
            const auto upper = token.toUpperCase();
            const auto lpNumber = upper.startsWith ("LP") ? upper.substring (2)
                                                          : juce::String();

            if (upper == "FC" || upper == "NS4" || upper == "TC"
                 || (lpNumber.isNotEmpty() && lpNumber.containsOnly ("0123456789")))
            {
                cleanTokens.remove (i);
                continue;
            }

            // Codigo COLADO no fim do nome. A ordem importa: "LP4" antes de "P4"
            // e de "FC", senao um prefixo curto corta no lugar errado.
            //
            // CUIDADO com "BELLP4": tirar "LP4" deixa "BEL" -- come um L, porque
            // o L pertence a palavra BELL e nao ao codigo. Por isso "P4" existe
            // nesta lista e e testado depois: BELLP4 -> BELL, e FXLP4 -> FX.
            for (const char* compactSuffix : { "LP4", "P4", "FC2", "FC" })
            {
                const juce::String suffix (compactSuffix);

                if (token.endsWithIgnoreCase (suffix) && token.length() > suffix.length() + 1)
                {
                    token = token.dropLastCharacters (suffix.length());
                    break;
                }
            }

            cleanTokens.set (i, token);
        }

        s = cleanTokens.joinIntoString (" ").trim();

        // Rotulos que identificavam terceiros, por tres motivos diferentes:
        //  - "Fabio" e o primeiro nome do autor do acervo de pianos, e o acordo
        //    com ele pede exatamente que o acervo nao seja reconhecivel;
        //  - "B3" (Hammond), "Farf" (Farfisa) e "MK I" (Rhodes) sao marcas
        //    registradas de terceiros, num produto que vai a venda;
        //  - "Stockholm" entrega o hardware sueco de onde o timbre foi tirado.
        // A troca e SO DE EXIBICAO: o rotulo cru segue sendo a chave dos setups
        // salvos (loadLayerByLabel) e do icone de categoria -- kindForName ainda
        // ve "b3"/"farf" e devolve o glyph de orgao.
        struct Rename { const char* from; const char* to; };

        static const Rename renames[] =
        {
            { "B3 Fabio Rotary", "\xc3\x93rg\xc3\xa3o Cl\xc3\xa1ssico Rotary" },
            { "B3 Fabio",        "\xc3\x93rg\xc3\xa3o Cl\xc3\xa1ssico" },
            { "B3 Full Rotary",  "\xc3\x93rg\xc3\xa3o Cheio Rotary" },
            { "B3 Full",         "\xc3\x93rg\xc3\xa3o Cheio" },
            { "B3 Jazz Rotary",  "\xc3\x93rg\xc3\xa3o Jazz Rotary" },
            { "B3 Jazz",         "\xc3\x93rg\xc3\xa3o Jazz" },
            { "B3 Pop Rotary",   "\xc3\x93rg\xc3\xa3o Pop Rotary" },
            { "B3 Pop",          "\xc3\x93rg\xc3\xa3o Pop" },
            { "Farf Duo Rotary", "\xc3\x93rg\xc3\xa3o Combo Rotary" },
            { "Farf Duo",        "\xc3\x93rg\xc3\xa3o Combo" },
            { "String Fabio",    "Cordas Suaves" },
            { "EP Stockholm",    "EP Vintage" },
            { "Tines MK I",      "Tines Cl\xc3\xa1ssico" },
        };

        for (const auto& r : renames)
            if (s.equalsIgnoreCase (r.from))
                return juce::String::fromUTF8 (r.to);

        // PALAVRAS GRUDADAS. O acervo veio com nomes em caixa alta e sem espaco
        // ("PADMIX", "SYNTHFULL", "STHANALOG"), e o Title Case sozinho produzia
        // "Padmix" e "Sthanalog" -- que parecem erro de digitacao numa lista que
        // vai para o cliente. Separar aqui e so exibicao: a chave crua continua
        // sendo a do disco.
        struct Split { const char* from; const char* to; };

        static const Split splits[] =
        {
            { "PADMIX",      "Pad Mix" },
            { "PADDARK",     "Pad Dark" },
            { "PADVOX",      "Pad Vox" },
            { "SYNTHFULL",   "Synth Full" },
            { "SYNTHPLAYER", "Synth Player" },
            { "SYNTHSTRING", "Synth String" },
            { "STHANALOG",   "Synth Analog" },
            { "BELL",        "Sinos" },
            { "STRING",      "Cordas" },
            { "WARM",        "Pad Quente" },
            { "REV",         "Reverso" },
            { "REV2",        "Reverso 2" },
            { "PLUCKED",     "Dedilhado" },
            { "PLUCKED2",    "Dedilhado 2" },
        };

        for (const auto& sp : splits)
            if (s.equalsIgnoreCase (sp.from))
                return juce::String::fromUTF8 (sp.to);

        juce::StringArray tokens;
        tokens.addTokens (s, " ", {});
        tokens.removeEmptyStrings();

        // siglas de verdade ficam intactas; o resto vira Title Case
        static const char* const acronyms[] = { "XL", "DX7", "CP70", "EP9", "B3",
                                                "MK1", "MK2", "TX", "JX", "II", "III", "IV",
                                                // "EP" saia como "Ep Ballad"; "FX" como "Fx"
                                                "EP", "FX" };

        for (auto& t : tokens)
        {
            bool isAcronym = false;

            for (const char* a : acronyms)
                if (t.equalsIgnoreCase (a))
                    isAcronym = true;

            if (isAcronym)
            {
                t = t.toUpperCase();
                continue;
            }

            if (t.containsAnyOf ("0123456789") && t.length() <= 3)
                continue;

            // capitalizacao intencional ("FullTines") nao se mexe
            const bool allLower = (t == t.toLowerCase());
            const bool allUpper = (t == t.toUpperCase());

            if (allLower || allUpper)
                t = t.substring (0, 1).toUpperCase() + t.substring (1).toLowerCase();
        }

        return tokens.joinIntoString (" ");
    }

    //==============================================================================
    // COR DO CANAL. Por padrao vem da categoria do timbre (piano azul, pad roxo...),
    // o que ensina o sistema sozinho. Mas dois pianos ficariam os dois azuis -- e
    // ai a cor para de distinguir justo quando mais precisa. Por isso da para
    // sobrescrever canal a canal. -1 = automatico (segue a categoria).
    static constexpr int numChannelColours = 8;

    static juce::Colour channelColour (int i)
    {
        switch (i)
        {
            case 0:  return NordLookAndFeel::catPiano();
            case 1:  return NordLookAndFeel::catPad();
            case 2:  return NordLookAndFeel::catBell();
            case 3:  return NordLookAndFeel::catStrings();
            case 4:  return NordLookAndFeel::catOrgan();
            case 5:  return NordLookAndFeel::catLead();
            case 6:  return NordLookAndFeel::catPlugin();
            default: return NordLookAndFeel::nordRed();
        }
    }

    static const char* channelColourName (int i)
    {
        static const char* const n[numChannelColours] =
            { "Azul", "Roxo", "Dourado", "Verde", "Laranja", "Anil", "Ambar", "Vermelho" };

        return juce::isPositiveAndBelow (i, numChannelColours) ? n[i] : "";
    }

    int getLayerColourIndex (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layerColourIdx[l] : -1;
    }

    void setLayerColourIndex (int l, int idx)
    {
        if (juce::isPositiveAndBelow (l, numLayers))
        {
            layerColourIdx[l] = (idx < 0 || idx >= numChannelColours) ? -1 : idx;
            sendChangeMessage();
        }
    }

    // Acorde tocado. O nome e calculado na thread da INTERFACE a partir de uma
    // mascara mantida incrementalmente no note-on/note-off -- ou seja, a thread de
    // audio nao paga NADA por bloco, so um OR/AND por tecla.
    void getHeldNotes (bool out[128]) const noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            const juce::uint32 w = heldBits[i].load (std::memory_order_relaxed);

            for (int b = 0; b < 32; ++b)
                out[i * 32 + b] = (w & (1u << b)) != 0;
        }
    }

    // canal em foco na interface; -1 = nenhum (estado inicial)
    int  getSelectedLayer() const        { return uiSelectedLayer.load(); }
    void setSelectedLayer (int l)        { uiSelectedLayer.store (l < 0 ? -1 : juce::jlimit (0, numLayers - 1, l)); }
    bool getLayerMute (int l) const                     { return juce::isPositiveAndBelow (l, numLayers) && layers[l].mute != nullptr && layers[l].mute->get(); }
    bool getLayerSolo (int l) const                     { return juce::isPositiveAndBelow (l, numLayers) && layers[l].solo != nullptr && layers[l].solo->get(); }
    bool isLayerLoaded (int l) const noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
            && (loadedLayerMask.load (std::memory_order_acquire)
                 & (juce::uint32 (1) << l)) != 0;
    }

    // Uma unica definicao de SOLO global para audio, UI e LiveScenes. A
    // identidade vem de uma mascara atomica porque String/unique_ptr/locks nao
    // podem entrar no callback de audio.
    static bool hasAnySoloInMasks (juce::uint32 loadedMask,
                                   juce::uint32 soloMask) noexcept
    {
        return (loadedMask & soloMask) != 0;
    }

    bool hasAnyLoadedSolo() const noexcept
    {
        const auto loaded = loadedLayerMask.load (
            std::memory_order_acquire);
        juce::uint32 solos = 0;

        for (int l = 0; l < numLayers; ++l)
            if (layers[l].solo != nullptr
                 && layers[l].solo->get())
                solos |= juce::uint32 (1) << l;

        return hasAnySoloInMasks (loaded, solos);
    }
    bool isLoading() const noexcept                     { return loadingCount->load() > 0; }

    // Tem trabalho pendente? Cobre as TRES fases: escaneamento da biblioteca,
    // timbres na fila esperando esse escaneamento (requestLayerLoad guarda em
    // pendingLayerLabels quando a lista ainda nao existe) e samples decodificando.
    // A splash usa isto pra so entregar a DAW quando os canais estao mesmo prontos.
    bool isBusyLoading() const noexcept
    {
        if (loadingCount->load() > 0)
            return true;

        // AUv3/AU/VST sao criados assincronamente e nao passam pelo contador
        // de samples. Uma cena-pagina so pode aplicar os faders finais depois
        // que esses pedidos terminaram; caso contrario o callback do plugin
        // restauraria defaults por cima da cena alguns quadros depois.
        {
            const juce::ScopedLock callbackLock (getCallbackLock());

            for (int i = 0; i < numInserts; ++i)
                if (insertPluginLoadInFlight[i].load (
                        std::memory_order_acquire))
                    return true;
        }

        {
            const juce::ScopedLock lock (layerIdentityLock);

            for (int l = 0; l < numLayers; ++l)
                if (pendingLayerLabels[l].isNotEmpty()
                     || instrumentPluginLoadInFlight[l].load (
                            std::memory_order_acquire))
                    return true;
        }

        return ! entriesScanned;
    }

    // Diferente de isBusyLoading(): um plugin ausente pode deixar a intencao
    // de restore guardada para autocura sem haver trabalho em execucao. A
    // splash nao deve ficar eterna por isso, mas uma troca de CENA precisa
    // saber que a pagina ainda nao terminou e avisar em vez de declarar
    // sucesso com metade do som.
    bool hasPendingStateRestore() const noexcept
    {
        for (int i = 0; i < numInserts; ++i)
            if (insertStateRestorePending[i].load (
                    std::memory_order_acquire))
                return true;

        for (int l = 0; l < numLayers; ++l)
            if (layerStateRestorePending[l].load (
                    std::memory_order_acquire))
                return true;

        return false;
    }

    // PRESET SHOWROOM: pedido pelo MainComponent quando NAO existe setup salvo
    // (primeira execucao). Fica pendente porque nesse momento a biblioteca ainda
    // nem foi escaneada -- e consumido no fim do scan, e so se os canais
    // continuarem todos vazios (um restore atrasado tem prioridade).
    void requestShowroomPreset() { showroomPending = true; }

    // Curva de velocity. Exposta porque quem edita e o painel Audio/MIDI (la
    // fica a escolha da entrada MIDI, e isto e calibragem do MESMO teclado),
    // nao a regua master -- a coluna do master ja esta cheia e o controle
    // ficava cortado na borda do card.
    juce::AudioParameterChoice* getVelCurveParam() const noexcept { return velCurve; }

    // ---- parametros expostos pro controle externo (Launchkey em modo DAW) ----
    //== Acessores usados pelo LIVE MODE (cenas) =============================
    //
    // A cena le e escreve APENAS estes campos. Nada aqui recarrega sample:
    // sao todas escritas de parametro, instantaneas, e com a rampa do
    // pos-fader nem cortam a cauda da musica anterior.
    juce::AudioParameterBool*  getLayerMuteParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].mute : nullptr;
    }

    juce::AudioParameterBool*  getLayerSoloParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].solo : nullptr;
    }

    juce::AudioParameterInt*   getLayerTransposeParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].transp : nullptr;
    }

    juce::AudioParameterFloat* getLayerReverbSendParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].reverbSend : nullptr;
    }

    juce::AudioProcessorParameter* getLayerVolParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].vol : nullptr;
    }

    juce::AudioProcessorParameter* getLayerCompParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].comp : nullptr;
    }

    juce::AudioParameterFloat* getLayerAttackParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].attack : nullptr;
    }

    juce::AudioParameterFloat* getLayerReleaseParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].release : nullptr;
    }

    juce::AudioParameterFloat* getLayerLowEqParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].eqLow : nullptr;
    }

    juce::AudioParameterFloat* getLayerMidEqParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].eqMid : nullptr;
    }

    juce::AudioParameterFloat* getLayerHighEqParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].eqHigh : nullptr;
    }

    juce::AudioParameterChoice* getLayerRotaryModeParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].rotaryMode : nullptr;
    }

    juce::AudioParameterFloat* getLayerRotaryAmountParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].rotaryAmount : nullptr;
    }

    bool isProgrammaticToneChange() const noexcept
    {
        return programmaticToneChangeDepth.load (
                   std::memory_order_acquire) > 0;
    }

    // Aplica somente ao escolher um NOVO timbre. Restaurar uma sessao nao chama
    // este metodo, portanto os ajustes do cliente sempre ganham dos defaults.
    void applyFactoryTimbreShape (int l, const juce::String& label,
                                  const juce::String& category = {},
                                  bool enableNativeRotary = true)
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        // Tudo que pode alocar/normalizar Strings acontece antes de tocar nos
        // parametros. Falha extrema deixa o timbre anterior integralmente igual.
        const auto defaults =
            ChurchDAWChannelTone::defaultsFor (label, category);
        const auto normalizedCategory =
            category.trim().toUpperCase();
        const bool organ = looksLikeOrgan (label, category);
        const bool bakedRotary =
            label.containsIgnoreCase ("rotary");

        programmaticToneChangeDepth.fetch_add (
            1, std::memory_order_acq_rel);
        const juce::ScopeGuard finishProgrammaticChange { [this]
        {
            programmaticToneChangeDepth.fetch_sub (
                1, std::memory_order_acq_rel);
        } };

        *layers[l].attack = defaults.attackSeconds;
        *layers[l].release = defaults.releaseSeconds;
        *layers[l].eqLow = defaults.lowDb;
        *layers[l].eqMid = defaults.midDb;
        *layers[l].eqHigh = defaults.highDb;
        *layers[l].autoEq = false;

        // Preserva o preset worship antigo: pads/ambiencias ja entravam com
        // HPF em 0,32. Outros timbres entram sem corte legado. Restaurar sessao
        // nao chama este metodo, logo nenhum ajuste do cliente e sobrescrito.
        *layers[l].lowcut =
            normalizedCategory == "PADS" || normalizedCategory == "AMBIENCIAS"
                ? 0.32f : 0.0f;
        *layers[l].highcut = 0.0f;

        *layers[l].rotaryMode =
            enableNativeRotary && organ && ! bakedRotary
                ? ChurchDAWRotary::Speaker::slowMode
                : ChurchDAWRotary::Speaker::stopMode;
        *layers[l].rotaryAmount =
            enableNativeRotary && organ && ! bakedRotary ? 0.65f : 0.0f;
    }

    juce::AudioProcessorParameter* getLayerOnParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].on : nullptr;
    }

    bool getLayerOn (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) && layers[l].on != nullptr && layers[l].on->get();
    }

    //==============================================================================
    // Roteamento MIDI por canal (desktop: 2 teclados → piano / pad).
    mutable juce::CriticalSection midiRouteLock;
    juce::String layerMidiDeviceId[numLayers];
    std::atomic<int> layerMidiListenChannel[numLayers] {};
    juce::StringArray forceMidiDeviceIds;
    juce::Array<int> forceMidiChannels;

    void rebuildMidiDeviceForceMap()
    {
        // Chamar com midiRouteLock tomado.
        forceMidiDeviceIds.clearQuick();
        forceMidiChannels.clearQuick();

        for (int l = 0; l < numLayers; ++l)
        {
            const auto& id = layerMidiDeviceId[l];
            const int ch = layerMidiListenChannel[l].load (
                std::memory_order_relaxed);
            if (id.isEmpty() || ch <= 0)
                continue;

            if (! forceMidiDeviceIds.contains (id))
            {
                forceMidiDeviceIds.add (id);
                forceMidiChannels.add (ch);
            }
        }
    }

    // ---- Entrada MIDI por canal (2 teclados: um piano, outro pad) ------------
    // deviceId vazio = Todos os teclados. listenChannel 0 = omni.
    juce::String getLayerMidiInputDevice (int l) const
    {
        const juce::ScopedLock sl (midiRouteLock);
        return juce::isPositiveAndBelow (l, numLayers) ? layerMidiDeviceId[l]
                                                       : juce::String();
    }

    int getLayerMidiListenChannel (int l) const noexcept
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return 0;
        return layerMidiListenChannel[l].load (std::memory_order_relaxed);
    }

    void setLayerMidiInputDevice (int l, juce::String deviceId)
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return;

        deviceId = deviceId.trim();
        {
            const juce::ScopedLock sl (midiRouteLock);
            layerMidiDeviceId[l] = deviceId;

            if (deviceId.isEmpty())
            {
                layerMidiListenChannel[l].store (0, std::memory_order_relaxed);
            }
            else
            {
                int ch = 0;
                for (int i = 0; i < numLayers; ++i)
                    if (i != l && layerMidiDeviceId[i] == deviceId)
                    {
                        ch = layerMidiListenChannel[i].load (
                            std::memory_order_relaxed);
                        if (ch > 0)
                            break;
                    }

                if (ch <= 0)
                {
                    bool used[17] = {};
                    for (int i = 0; i < numLayers; ++i)
                    {
                        const int c = layerMidiListenChannel[i].load (
                            std::memory_order_relaxed);
                        if (c >= 1 && c <= 16)
                            used[c] = true;
                    }

                    ch = juce::jlimit (1, 16, l + 1);
                    if (used[ch])
                    {
                        ch = 1;
                        while (ch <= 16 && used[ch])
                            ++ch;
                        if (ch > 16)
                            ch = (l % 16) + 1;
                    }
                }

                layerMidiListenChannel[l].store (ch, std::memory_order_relaxed);
            }

            rebuildMidiDeviceForceMap();
        }

        sendChangeMessage();
    }

    // Thread de MIDI: teclado amarrado a um canal de layer → força o canal MIDI
    // da mensagem (teclados USB costumam mandar tudo no canal 1).
    int getForcedMidiChannelForDevice (const juce::String& deviceId) const
    {
        if (deviceId.isEmpty())
            return 0;

        const juce::ScopedLock sl (midiRouteLock);
        for (int i = 0; i < forceMidiDeviceIds.size(); ++i)
            if (forceMidiDeviceIds.getReference (i) == deviceId)
                return forceMidiChannels.getUnchecked (i);

        return 0;
    }

    juce::AudioProcessorParameter* getGlobalTranspParam() const { return globalTransp; }

    // Globais em unidade REAL, para o LIVE MODE.
    //
    // A cena precisa gravar e devolver volume geral, transpose global e
    // retorno de reverb. Chegar neles pelo ponteiro generico obrigaria a
    // guardar valor normalizado, e o normalizado ja custou um bug: a unidade
    // do volume geral e 1,0 de ganho, que normalizado da 0,667 — quem lesse
    // "1,0" como normalizado entregaria 1,5 de ganho, +3,5 dB.
    juce::AudioParameterFloat* getVolumeGeralParam()   const noexcept { return gain; }
    juce::AudioParameterInt*   getTransposeGeralParam() const noexcept { return globalTransp; }
    juce::AudioParameterFloat* getRetornoDeReverbParam() const noexcept { return reverbReturn; }

    // pico do canal por lado; a UI le e zera (so pode existir UM leitor)
    void readAndResetLayerLevels (int l, float& outL, float& outR) noexcept
    {
        const int i = juce::jlimit (0, numLayers - 1, l);
        outL = layerLevelsL[i].exchange (0.0f);
        outR = layerLevelsR[i].exchange (0.0f);
    }

    float readAndResetLayerLevel (int l) noexcept
    {
        float a = 0.0f, b = 0.0f;
        readAndResetLayerLevels (l, a, b);
        return juce::jmax (a, b);
    }

    // espelho NAO-destrutivo do nivel, com decaimento feito na thread de audio:
    // o oficial (readAndResetLayerLevel) zera na leitura e so suporta UM leitor
    // (o medidor da regua) -- o visor CANAIS le por aqui sem roubar o pico dele
    float getLayerLevelShown (int l) const noexcept
    {
        return layerLevelShown[juce::jlimit (0, numLayers - 1, l)].load();
    }

    const juce::Array<TimbreEntry>& getEntries()
    {
        if (! entriesScanned)
        {
            entriesCache = scanAllLibraries();
            entriesScanned = true;
        }

        return entriesCache;
    }

    // versao que nunca bloqueia: retorna o que ja foi escaneado
    const juce::Array<TimbreEntry>& getEntriesNonBlocking() const noexcept
    {
        return entriesCache;
    }

    void startEntriesScanAsync (bool allowCache = true)
    {
        const int gen = ++scanGeneration;   // invalida escaneamentos anteriores
        juce::WeakReference<TimbresProcessor> weakThis (this);
        auto abort = workerAbort;   // shared_ptr POR VALOR: sobrevive ao processador

        const bool started = startWorkerThread ([weakThis, gen, abort, allowCache] () mutable
        {
            std::shared_ptr<juce::Array<TimbreEntry>> scanned;

            try
            {
                scanned = std::make_shared<juce::Array<TimbreEntry>> (
                    scanAllLibraries (abort.get(), allowCache));
            }
            catch (...)
            {
                // Falha de scan/alocacao preserva o cache anterior e ainda
                // precisa encerrar o splash na thread de mensagens.
            }

            auto enqueueCompletion = [&]() noexcept
            {
                try
                {
                    return juce::MessageManager::callAsync (
                        [weakThis, scanned, gen] () mutable
                    {
                        if (auto* self = weakThis.get())
                        {
                            if (gen != self->scanGeneration)
                                return;   // escaneamento velho e descartado

                            if (scanned != nullptr)
                                self->finishEntriesScan (std::move (*scanned));
                            else
                                self->finishEntriesScanFailure();
                        }
                    });
                }
                catch (...)
                {
                    return false;
                }
            };

            if (! enqueueCompletion())
            {
                // Segunda tentativa sem o array potencialmente grande. Se a
                // propria fila ja acabou, o app esta fechando e nao ha splash.
                scanned.reset();
                enqueueCompletion();
            }
        });

        if (! started)
        {
            // Estamos na message thread: e seguro encerrar o estado de busy
            // diretamente e manter a ultima lista conhecida.
            finishEntriesScanFailure();
        }
    }

    void rescanLibrary()
    {
        entriesScanned = false;

        // A assinatura do cache inclui todas as bibliotecas e seus diretórios:
        // se um pack mudou, loadEntriesCache o rejeita automaticamente. Manter
        // os dados anteriores até finishEntriesScan evita que a UI descarregue
        // o timbre em execução e mostre "(vazio)" durante a reindexação.
        startEntriesScanAsync (true);
    }

    bool hasScannedLibrary() const noexcept     { return entriesScanned; }

   #if defined (CHURCHDAW_INTERNAL_CATALOG_TEST_HOOKS)
    // Injeta um catálogo sintético sem alterar o scanner/cache de produção.
    // Incrementar a geração invalida qualquer callback assíncrono iniciado pelo
    // construtor, permitindo provar restore legado de forma determinística.
    void replaceEntriesForCatalogTest (
        juce::Array<TimbreEntry> entries)
    {
        ++scanGeneration;
        entriesCache = std::move (entries);
        entriesScanned = true;
    }

    void requestLayerLoadForCatalogTest (
        int layerIndex,
        const juce::String& label,
        const juce::String& library)
    {
        requestLayerLoad (
            layerIndex, label, library);
    }
   #endif

    void clearLayer (int layerIndex,
                     bool preserveRestore = false,
                     bool preserveChurchOrgan = false)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        // Retira primeiro o bit transitivo: durante a troca, um SOLO deste
        // canal nao pode contaminar os que continuam carregados.
        *layers[layerIndex].solo = false;
        setLayerLoaded (layerIndex, false);

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        // Limpar o canal nao deve apagar a regulagem feita para o timbre. Se o
        // cliente o selecionar novamente, ENV/EQ retornam como estavam.
        rememberOutgoingTimbreTone (layerIndex);

        // canal limpo invalida decode em voo e cargas agendadas deste canal
        cancelLayerLoad (layerIndex);
        ++layerLoadGeneration[layerIndex];
        {
            const juce::ScopedLock lock (layerIdentityLock);
            pendingLayerLabels[layerIndex].clear();
            pendingLayerLibs[layerIndex].clear();
            loadingLabels[layerIndex].clear();
            loadingLibs[layerIndex].clear();
        }

        bool retired = false;
        {
            const bool resumeAfterClear = ! isSuspended();

            if (resumeAfterClear)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterClear]
            {
                if (resumeAfterClear)
                    suspendProcessing (false);
            } };

            auto old = std::move (layers[layerIndex].zones);

            instNotesOn[layerIndex] = 0;
            noteTranspose.clearLayer (layerIndex);
            pluginSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
            pluginPitchCentresPending[layerIndex].store (
                false,
                std::memory_order_release);

            for (auto& v : voices)
                if (v.active && v.layerIndex == layerIndex)
                {
                    v.heldByPedal = false;
                    releaseVoice (v);   // fade suave em vez de corte
                }

            if (! old.empty())
                retired = retireZonesWhileSuspended (old);

            layerIsOrgan[layerIndex].store (
                false, std::memory_order_relaxed);
            layerNativeFamilyTrim[layerIndex].store (
                1.0f, std::memory_order_relaxed);
            rotarySpeakers[layerIndex].reset();

            if (! preserveChurchOrgan)
            {
                layerIsChurchOrgan[layerIndex].store (
                    false, std::memory_order_release);
                layerChurchOrganFamily[layerIndex].store (
                    0, std::memory_order_release);
                lastChurchOrganChannelRotaryMode[layerIndex].store (
                    -1, std::memory_order_release);
                churchOrganSustainMirror[layerIndex].store (
                    sustainStateUnknown,
                    std::memory_order_release);
                pendingChurchOrganStates[layerIndex].reset();
                churchOrganReverbBuffers[layerIndex].clear();

                if (auto* organ =
                        layers[layerIndex].churchOrgan.get())
                {
                    organ->setProcessingSuspended (true);
                    (void) organ->reset();
                }
            }
        }

        if (retired)
            schedulePurge();

        {
            const juce::ScopedLock lock (layerIdentityLock);
            layers[layerIndex].timbre.clear();
            layers[layerIndex].libraryName.clear();
        }
        // Um plugin antigo pode continuar no canal ate clearInstrumentPlugin
        // ou ate o commit do novo alvo nativo. Nesse intervalo a identidade
        // antiga ainda existe e o bloqueio de captura precisa sobreviver.
        bool hasInstrument = false;
        {
            const juce::ScopedLock pluginLock (layerPluginStateLock);
            hasInstrument = layers[layerIndex].instrument != nullptr;
        }

        // clearLayer remove a identidade nativa, mas pode ser a primeira metade
        // de uma troca que ainda preserva plugin/Church Organ. Reponha o bit
        // somente se uma dessas identidades realmente sobreviveu.
        setLayerLoaded (layerIndex,
                        hasInstrument || isLayerChurchOrgan (layerIndex));

        if (! preserveRestore && ! hasInstrument)
            completeLayerStateRestore (layerIndex);
        sendChangeMessage();
    }

    // ---- VSTi externo (Kontakt etc.) hospedado no canal ----
    juce::AudioPluginInstance* getLayerInstrument (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].instrument.get() : nullptr;
    }

   #if defined (CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS)
    // Teste de integração do roteador MIDI hospedado. A macro existe somente
    // no executável de testes e nunca entra nos binários Windows/macOS/Android.
    // Instalar um capturador real de AudioPluginInstance prova os bytes que
    // chegam ao ramo VST/AU, sem depender de plugin comercial.
    bool installInstrumentForTest (
        int layerIndex,
        std::unique_ptr<juce::AudioPluginInstance> instance)
    {
        if (! juce::isPositiveAndBelow (
                layerIndex, numLayers)
             || instance == nullptr)
            return false;

        const bool resumeAfterSwap = ! isSuspended();

        if (resumeAfterSwap)
            suspendProcessing (true);

        const juce::ScopeGuard restoreProcessing {
            [this, resumeAfterSwap]
        {
            if (resumeAfterSwap)
                suspendProcessing (false);
        } };

        try
        {
            instance->prepareToPlay (
                hostSampleRate,
                juce::jmax (64, lastBlockSize));
            reserveInstrumentScratchFor (*instance);
        }
        catch (...)
        {
            releasePluginResourcesNoThrow (
                instance.get());
            return false;
        }

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        {
            const juce::ScopedLock pluginLock (
                layerPluginStateLock);
            oldWasPrepared =
                pluginPrepared[layerIndex].exchange (
                    false,
                    std::memory_order_acq_rel);
            old = std::move (
                layers[layerIndex].instrument);
            layers[layerIndex].instrument =
                std::move (instance);
            pluginPrepared[layerIndex].store (
                true,
                std::memory_order_release);
            instNotesOn[layerIndex] = 0;
            noteTranspose.clearLayer (layerIndex);
            pluginSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
            pluginPitchCentresPending[layerIndex].store (
                true,
                std::memory_order_release);
            setLayerLoaded (layerIndex, true);
        }

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        return true;
    }

    int getInstrumentScratchChannelsForTest() const noexcept
    {
        return instrumentBuf.getNumChannels();
    }

    int getInstrumentScratchSamplesForTest() const noexcept
    {
        return instrumentBuf.getNumSamples();
    }
   #endif

    juce::String getLayerInstrumentName (int l) const
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return {};

        const juce::ScopedLock lock (layerIdentityLock);
        return layers[l].instrumentName;
    }

    bool isLayerOrgan (int l) const noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
                && layerIsOrgan[l].load (std::memory_order_relaxed);
    }

    float getLayerNativeFamilyTrim (int l) const noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
                 ? layerNativeFamilyTrim[l].load (
                       std::memory_order_relaxed)
                 : 1.0f;
    }

    static const char* churchOrganFamilyName (
        churchorgan::ChurchOrganFamily family) noexcept
    {
        switch (family)
        {
            case churchorgan::ChurchOrganFamily::tonewheel:
                return "Tonewheel Organ";
            case churchorgan::ChurchOrganFamily::transistor:
                return "Transistor Organ";
            case churchorgan::ChurchOrganFamily::pipe:
                return "Pipe Organ";
            case churchorgan::ChurchOrganFamily::worshipHybrid:
                return "Worship Hybrid";
        }

        return "Tonewheel Organ";
    }

    static bool churchOrganFamilyUsesInternalRotary (
        churchorgan::ChurchOrganFamily family) noexcept
    {
        return family
                   == churchorgan::ChurchOrganFamily::tonewheel
            || family
                   == churchorgan::ChurchOrganFamily::worshipHybrid;
    }

    bool isLayerChurchOrgan (int l) const noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
            && layerIsChurchOrgan[l].load (
                   std::memory_order_acquire);
    }

    churchorgan::ChurchOrganFamily getLayerChurchOrganFamily (
        int l) const noexcept
    {
        const int stored = juce::isPositiveAndBelow (l, numLayers)
                         ? layerChurchOrganFamily[l].load (
                               std::memory_order_acquire)
                         : 0;
        return static_cast<churchorgan::ChurchOrganFamily> (
            juce::jlimit (0, 3, stored));
    }

    churchorgan::ChurchOrganLayerProcessor*
        getLayerChurchOrganProcessor (int l) noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
                 ? layers[l].churchOrgan.get()
                 : nullptr;
    }

    bool layerUsesInternalChurchOrganRotary (
        int l) const noexcept
    {
        return isLayerChurchOrgan (l)
            && churchOrganFamilyUsesInternalRotary (
                   getLayerChurchOrganFamily (l));
    }

    bool layerUsesChannelRotary (
        int l) const noexcept
    {
        return isLayerOrgan (l)
            && ! layerUsesInternalChurchOrganRotary (l);
    }

    const churchorgan::ChurchOrganLayerProcessor*
        getLayerChurchOrganProcessor (int l) const noexcept
    {
        return juce::isPositiveAndBelow (l, numLayers)
                 ? layers[l].churchOrgan.get()
                 : nullptr;
    }

    // Troca sample/VST/Church Organ em uma única suspensão. O adapter mantém
    // ganho unitário: ele não recebe o polyphonyHeadroom do sampler, o trim de
    // PAD/AMBIENCE nem um limiter/AGC duplicado.
    bool loadChurchOrganIntoLayer (
        int layerIndex,
        churchorgan::ChurchOrganFamily requestedFamily,
        const juce::MemoryBlock* restoredState = nullptr,
        bool applyFactoryDefaults = true)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return false;

        const int familyIndex = static_cast<int> (requestedFamily);

        if (familyIndex < 0
             || familyIndex >= churchOrganFamilyCount)
            return false;

        notifyHostedPluginWillRetire (
            HostedPluginLocation::instrument, layerIndex,
            getLayerInstrument (layerIndex));
        instrumentPluginLoadGeneration[layerIndex].cancel();
        instrumentPluginLoadInFlight[layerIndex].store (
            false, std::memory_order_release);

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        auto* organ = layers[layerIndex].churchOrgan.get();

        if (organ == nullptr)
            return false;

        std::unique_ptr<juce::AudioPluginInstance> oldInstrument;
        bool oldInstrumentWasPrepared = false;
        const bool resumeAfterSwap = ! isSuspended();

        if (resumeAfterSwap)
            suspendProcessing (true);

        bool configured = false;
        {
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterSwap]
            {
                if (resumeAfterSwap)
                    suspendProcessing (false);
            } };

            // Limpa samples/vozes/identidade, mas preserva o adapter que será
            // configurado logo abaixo dentro da mesma fronteira suspensa.
            oldInstrumentWasPrepared =
                pluginPrepared[layerIndex].load (
                    std::memory_order_acquire);
            clearLayer (layerIndex, true, true);

            {
                const juce::ScopedLock pluginLock (
                    layerPluginStateLock);
                oldInstrument = std::move (
                    layers[layerIndex].instrument);
                pluginPrepared[layerIndex].store (
                    false,
                    std::memory_order_release);

                const juce::ScopedLock identityLock (
                    layerIdentityLock);
                layers[layerIndex].instrumentId.clear();
                layers[layerIndex].instrumentName.clear();
                pendingVstIds[layerIndex].clear();
                pendingVstStates[layerIndex].clear();
                pendingVstDescriptionValid[layerIndex] = false;
            }

            organ->setProcessingSuspended (true);

            const bool prepared = organ->diagnostics().prepared;

            if (prepared)
            {
                if (restoredState != nullptr
                     && restoredState->getSize() > 0)
                    configured = organ->setStateInformation (
                        restoredState->getData(),
                        restoredState->getSize());

                if (! configured)
                {
                    (void) organ->reset();
                    configured =
                        organ->selectFamilyWhileSuspended (
                            requestedFamily);
                }

                // A calibração aprovada é a saída nativa de cada motor. Nunca
                // aplique aqui o -4 dB das vozes sampled ou o +4,5 dB de PAD.
                for (int family = 0;
                     family < churchOrganFamilyCount;
                     ++family)
                    organ->setFamilyOutputTrim (
                        static_cast<churchorgan::ChurchOrganFamily> (
                            family),
                        1.0f);
            }
            else
            {
                // Hosts podem restaurar estado antes de prepareToPlay().
                // Conservamos o blob e a família; a preparação consome ambos
                // fora do callback quando a taxa/bloco reais forem conhecidos.
                pendingChurchOrganStates[layerIndex].reset();

                if (restoredState != nullptr
                     && restoredState->getSize() > 0)
                    pendingChurchOrganStates[layerIndex].append (
                        restoredState->getData(),
                        restoredState->getSize());

                configured = true;
            }

            const auto activeFamily =
                prepared && configured
                    ? organ->activeFamily()
                    : requestedFamily;

            layerChurchOrganFamily[layerIndex].store (
                static_cast<int> (activeFamily),
                std::memory_order_release);
            layerIsChurchOrgan[layerIndex].store (
                configured, std::memory_order_release);
            layerIsOrgan[layerIndex].store (
                configured, std::memory_order_release);
            layerNativeFamilyTrim[layerIndex].store (
                1.0f, std::memory_order_release);
            layerCategory[layerIndex] = "ORGAOS";
            churchOrganSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);

            bool supersededByPlugin = false;
            {
                const juce::ScopedLock identityLock (
                    layerIdentityLock);
                layers[layerIndex].timbre =
                    churchOrganFamilyName (activeFamily);
                layers[layerIndex].libraryName =
                    "Church Organ Engine";
                loadingLabels[layerIndex].clear();
                loadingLibs[layerIndex].clear();
                pendingLayerLabels[layerIndex].clear();
                pendingLayerLibs[layerIndex].clear();
            }
            setLayerLoaded (layerIndex, configured);

            bool recalledChurchOrganTone = false;

            if (configured && applyFactoryDefaults)
            {
                applyFactoryTimbreShape (
                    layerIndex,
                    churchOrganFamilyName (activeFamily),
                    "ORGAOS",
                    false);
                // Um único controle, um único estágio: Tonewheel/Worship usam
                // RotaryCabinet interno; Transistor/Pipe usam o Rotary do
                // canal. A escolha por família acontece no caminho de FX.
                *layers[layerIndex].rotaryMode =
                    ChurchDAWRotary::Speaker::slowMode;
                *layers[layerIndex].rotaryAmount = 0.82f;
                recalledChurchOrganTone =
                    applyRememberedTimbreTone (
                    layerIndex,
                    churchOrganFamilyName (activeFamily),
                    "Church Organ Engine");
                *layers[layerIndex].on = true;
                *layers[layerIndex].mute = false;
                *layers[layerIndex].solo = false;
            }

            updateLayerEq (layerIndex);
            manualEqs[layerIndex].syncToTargets (
                layers[layerIndex].eqLow != nullptr
                    ? (float) *layers[layerIndex].eqLow : 0.0f,
                layers[layerIndex].eqMid != nullptr
                    ? (float) *layers[layerIndex].eqMid : 0.0f,
                layers[layerIndex].eqHigh != nullptr
                    ? (float) *layers[layerIndex].eqHigh : 0.0f);

            const int currentChannelRotaryMode =
                layers[layerIndex].rotaryMode != nullptr
                    ? layers[layerIndex].rotaryMode->getIndex()
                    : ChurchDAWRotary::Speaker::stopMode;
            lastChurchOrganChannelRotaryMode[layerIndex].store (
                currentChannelRotaryMode,
                std::memory_order_release);

            if (configured && applyFactoryDefaults
                 && ! recalledChurchOrganTone)
            {
                const float plainMode =
                    currentChannelRotaryMode
                        == ChurchDAWRotary::Speaker::slowMode
                            ? 1.0f
                        : currentChannelRotaryMode
                            == ChurchDAWRotary::Speaker::fastMode
                            ? 2.0f
                            : 0.0f;
                organ->setParameterPlain (
                    churchorgan::OrganParameterId::rotaryMode,
                    plainMode);
                organ->setParameterPlain (
                    churchorgan::OrganParameterId::rotaryMix,
                    layers[layerIndex].rotaryAmount != nullptr
                        ? (float) *layers[layerIndex].rotaryAmount
                        : 0.0f);
            }

            organ->setProcessingSuspended (! configured);
        }

        if (oldInstrumentWasPrepared)
            releasePluginResourcesNoThrow (
                oldInstrument.get());
        oldInstrument.reset();

        if (configured)
            completeLayerStateRestore (layerIndex);

        sendChangeMessage();
        return configured;
    }

    void setChurchOrganEnabled (int layerIndex, bool enabled)
    {
        if (juce::isPositiveAndBelow (layerIndex, numLayers)
             && layers[layerIndex].on != nullptr)
            *layers[layerIndex].on = enabled;
    }

    bool setChurchOrganQuality (
        int layerIndex,
        churchdaw::organ::ChurchOrganQuality quality)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers)
             || ! isLayerChurchOrgan (layerIndex))
            return false;

        const juce::ScopedLock toneLock (
            layerToneTransactionLock);
        auto* organ =
            layers[layerIndex].churchOrgan.get();

        if (organ == nullptr
             || ! organ->diagnostics().prepared)
            return false;

        const bool resumeAfterChange = ! isSuspended();

        if (resumeAfterChange)
            suspendProcessing (true);
        const juce::ScopeGuard restoreProcessing {
            [this, resumeAfterChange]
        {
            if (resumeAfterChange)
                suspendProcessing (false);
        } };

        organ->setProcessingSuspended (true);
        auto state = organ->createState();
        state.setProperty (
            "quality",
            juce::jlimit (
                0, 2, static_cast<int> (quality)),
            nullptr);
        const bool restored =
            organ->restoreState (state);

        if (restored)
        {
            for (int family = 0; family < 4; ++family)
                organ->setFamilyOutputTrim (
                    static_cast<
                        churchorgan::ChurchOrganFamily> (
                            family),
                    1.0f);
            churchOrganSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
        }

        organ->setProcessingSuspended (! restored);

        if (restored)
            sendChangeMessage();

        return restored;
    }

    void setChurchOrganRotaryMode (
        int layerIndex,
        churchdaw::organ::ChurchOrganRotaryMode mode)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        auto* organ = layers[layerIndex].churchOrgan.get();

        if (organ == nullptr)
            return;

        int plainMode = 0;
        int channelMode =
            layers[layerIndex].rotaryMode != nullptr
                ? layers[layerIndex].rotaryMode->getIndex()
                : ChurchDAWRotary::Speaker::stopMode;

        switch (mode)
        {
            case churchdaw::organ::ChurchOrganRotaryMode::stop:
                plainMode = 0;
                channelMode =
                    ChurchDAWRotary::Speaker::stopMode;
                break;
            case churchdaw::organ::ChurchOrganRotaryMode::slow:
                plainMode = 1;
                channelMode =
                    ChurchDAWRotary::Speaker::slowMode;
                break;
            case churchdaw::organ::ChurchOrganRotaryMode::fast:
                plainMode = 2;
                channelMode =
                    ChurchDAWRotary::Speaker::fastMode;
                break;
            case churchdaw::organ::ChurchOrganRotaryMode::brake:
                plainMode = 3;
                // O RotaryCabinet interno possui Brake próprio. Transistor e
                // Pipe usam exclusivamente o ChannelRotarySpeaker; nele STOP
                // é a frenagem física com inércia (não um bypass instantâneo).
                if (! churchOrganFamilyUsesInternalRotary (
                        getLayerChurchOrganFamily (layerIndex)))
                    channelMode =
                        ChurchDAWRotary::Speaker::stopMode;
                break;
        }

        organ->setParameterPlain (
            churchorgan::OrganParameterId::rotaryMode,
            (float) plainMode);

        const bool channelOwnsRotary =
            ! churchOrganFamilyUsesInternalRotary (
                getLayerChurchOrganFamily (layerIndex));
        if ((mode
               != churchdaw::organ::ChurchOrganRotaryMode::brake
             || channelOwnsRotary)
             && layers[layerIndex].rotaryMode != nullptr)
        {
            *layers[layerIndex].rotaryMode = channelMode;
            lastChurchOrganChannelRotaryMode[layerIndex].store (
                channelMode, std::memory_order_release);
        }
        else if (mode
                   == churchdaw::organ::ChurchOrganRotaryMode::brake)
        {
            // Impede o primeiro callback de interpretar o tracker -1 como
            // mudança do strip e sobrescrever rotaryMode=Brake no motor.
            lastChurchOrganChannelRotaryMode[layerIndex].store (
                channelMode, std::memory_order_release);
        }
    }

    void setChurchOrganRotaryMix (
        int layerIndex, float normalised)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const float value =
            juce::jlimit (0.0f, 1.0f, normalised);

        if (layers[layerIndex].rotaryAmount != nullptr)
            *layers[layerIndex].rotaryAmount = value;

        if (auto* organ =
                layers[layerIndex].churchOrgan.get())
            organ->setParameterPlain (
                churchorgan::OrganParameterId::rotaryMix,
                value);
    }

    // Agenda um VST/AU/AUv3 sem esperar sua fábrica na message thread. O canal
    // atual permanece audível até o callback preparar e validar o substituto.
    bool loadInstrumentPlugin (
        int layerIndex,
        const juce::PluginDescription& desc,
        bool applyFactoryDefaults = true,
        const juce::MemoryBlock* restoredState = nullptr,
        PluginLoadCompletion completion = {})
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers)
             || formatManagerRef == nullptr
             || isHostedPluginBlocked (desc))
            return false;

        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket = 0;

        try
        {
            juce::MemoryBlock stateToRestore;

            if (restoredState != nullptr && restoredState->getSize() > 0)
                stateToRestore = *restoredState;

            auto requestedId = desc.createIdentifierString();
            auto requestedState = stateToRestore.toBase64Encoding();
            juce::PluginDescription requestedDescription (desc);
            auto restoreTargetId = requestedId;
            auto restoreTargetState = requestedState;
            ticket = instrumentPluginLoadGeneration[layerIndex].begin();

            {
                const juce::ScopedLock lock (layerIdentityLock);

                // Manual e restore ficam serializaveis desde a aceitacao. Sem
                // isto um autosave durante a fabrica AUv3 lenta gravava o plugin
                // antigo que ainda estava tocando.
                pendingVstIds[layerIndex] = std::move (requestedId);
                pendingVstStates[layerIndex] =
                    std::move (requestedState);
                pendingVstDescriptions[layerIndex] =
                    std::move (requestedDescription);
                pendingVstDescriptionValid[layerIndex] = true;
                instrumentPluginLoadInFlight[layerIndex].store (
                    true, std::memory_order_release);

                if (layerStateRestorePending[layerIndex].load (
                        std::memory_order_acquire))
                {
                    layerStateRestoreTargetTimbre[layerIndex].clear();
                    layerStateRestoreTargetLibrary[layerIndex].clear();
                    layerStateRestoreTargetVstId[layerIndex] =
                        std::move (restoreTargetId);
                    layerStateRestoreTargetVstState[layerIndex] =
                        applyFactoryDefaults
                            ? juce::String()
                            : std::move (restoreTargetState);
                }
            }

            juce::WeakReference<TimbresProcessor> weakThis (this);
            formatManagerRef->createPluginInstanceAsync (
                desc, hostSampleRate, juce::jmax (64, lastBlockSize),
                [weakThis, layerIndex, ticket, desc,
                 applyFactoryDefaults,
                 stateToRestore = std::move (stateToRestore),
                 completion = std::move (completion)] (
                    std::unique_ptr<juce::AudioPluginInstance> instance,
                    const juce::String& error) mutable
                {
                    struct Result
                    {
                        std::unique_ptr<juce::AudioPluginInstance> instance;
                        juce::String error;
                    };

                    auto result = std::make_shared<Result>();
                    result->instance = std::move (instance);
                    result->error = error;

                    auto deliver =
                        [weakThis, layerIndex, ticket, desc,
                         applyFactoryDefaults, result,
                         stateToRestore = std::move (stateToRestore),
                         completion = std::move (completion)] () mutable
                    {
                        if (auto* self = weakThis.get())
                            self->finishInstrumentPluginLoad (
                                layerIndex, ticket, desc,
                                applyFactoryDefaults,
                                stateToRestore,
                                std::move (result->instance),
                                result->error,
                                std::move (completion));
                    };

                    if (juce::MessageManager::getInstance()
                            ->isThisTheMessageThread())
                        deliver();
                    else
                        (void) juce::MessageManager::callAsync (
                            std::move (deliver));
                });

            return true;
        }
        catch (...)
        {
            if (ticket != 0)
            {
                const bool stillCurrent = [&]
                {
                    const juce::ScopedLock lock (layerIdentityLock);
                    return instrumentPluginLoadGeneration[layerIndex]
                        .commitIfCurrent (
                            ticket,
                            [this, layerIndex, applyFactoryDefaults]
                    {
                        instrumentPluginLoadInFlight[layerIndex].store (
                            false, std::memory_order_release);

                        if (applyFactoryDefaults)
                        {
                            pendingVstIds[layerIndex].clear();
                            pendingVstStates[layerIndex].clear();
                            pendingVstDescriptionValid[layerIndex] = false;
                        }
                    });
                }();

                if (applyFactoryDefaults && stillCurrent)
                    completeLayerStateRestore (layerIndex);
            }
            return false;
        }
    }

    void finishInstrumentPluginLoad (
        int layerIndex,
        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket,
        const juce::PluginDescription& desc,
        bool applyFactoryDefaults,
        const juce::MemoryBlock& restoredState,
        std::unique_ptr<juce::AudioPluginInstance> instance,
        const juce::String& creationError,
        PluginLoadCompletion completion)
    {
        jassert (juce::MessageManager::getInstance()
                    ->isThisTheMessageThread());

        if (! juce::isPositiveAndBelow (layerIndex, numLayers)
             || ! instrumentPluginLoadGeneration[layerIndex]
                     .isCurrent (ticket))
            return;

        if (isHostedPluginEditorTeardownActive())
        {
            try
            {
                struct DeferredResult
                {
                    juce::PluginDescription description;
                    juce::MemoryBlock state;
                    std::unique_ptr<juce::AudioPluginInstance> instance;
                    juce::String error;
                    PluginLoadCompletion completion;
                };

                auto deferred = std::make_shared<DeferredResult>();
                deferred->description = desc;
                deferred->state = restoredState;
                deferred->instance = std::move (instance);
                deferred->error = creationError;
                deferred->completion = std::move (completion);
                juce::WeakReference<TimbresProcessor> weakThis (this);

                deferHostedPluginFinish (
                    HostedPluginLocation::instrument, layerIndex,
                    [weakThis, layerIndex, ticket,
                     applyFactoryDefaults, deferred] () mutable
                    {
                        if (auto* self = weakThis.get())
                            self->finishInstrumentPluginLoad (
                                layerIndex, ticket,
                                deferred->description,
                                applyFactoryDefaults,
                                deferred->state,
                                std::move (deferred->instance),
                                deferred->error,
                                std::move (deferred->completion));
                    });
            }
            catch (...)
            {
                // A intencao continua persistida; nunca troque a instancia no
                // meio da destruicao de um editor de vendor.
            }
            return;
        }

        auto failCurrentRequest =
            [this, layerIndex, ticket, applyFactoryDefaults,
             &completion] (const juce::String& error) mutable
        {
            const bool preservePending = ! applyFactoryDefaults;

            if (preservePending
                 && layerStateRestorePending[layerIndex].load (
                        std::memory_order_acquire))
            {
                if (! finishFailedInstrumentStateRestore (
                        layerIndex, ticket))
                    return;

                if (instrumentPluginLoadGeneration[layerIndex]
                        .isCurrent (ticket))
                    invokePluginLoadCompletionNoThrow (
                        completion, false, error);
                return;
            }

            const bool stillCurrent = [&]
                {
                    const juce::ScopedLock lock (
                        layerIdentityLock);
                    return instrumentPluginLoadGeneration[layerIndex]
                        .commitIfCurrent (
                            ticket,
                            [this, layerIndex, preservePending]
                    {
                        instrumentPluginLoadInFlight[layerIndex].store (
                            false, std::memory_order_release);

                        if (! preservePending)
                        {
                            pendingVstIds[layerIndex].clear();
                            pendingVstStates[layerIndex].clear();
                            pendingVstDescriptionValid[layerIndex] = false;
                        }
                    });
                }();

            if (! stillCurrent)
                return;

            if (! preservePending)
                completeLayerStateRestore (layerIndex);

            sendChangeMessage();
            invokePluginLoadCompletionNoThrow (
                completion, false, error);
        };

        if (isHostedPluginBlocked (desc))
        {
            juce::Logger::writeToLog (
                "PLUGIN: instrumento bloqueado durante carga assincrona "
                + desc.fileOrIdentifier);
            failCurrentRequest (
                "Plugin foi colocado em quarentena durante a carga");
            return;
        }

        if (instance == nullptr)
        {
            juce::Logger::writeToLog (
                "PLUGIN: falha ao criar instrumento " + desc.name
                + ": " + creationError);
            failCurrentRequest (creationError);

            return;
        }

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        // Se qualquer etapa anterior ao commit falhar, finalize a instancia
        // temporaria. Depois do move, a carga e sucesso e nenhum catch pode
        // anunciar falha deixando o plugin ja instalado.
        bool temporaryNeedsRelease = false;
        const juce::ScopeGuard releaseTemporaryOnFailure {
            [this, &instance, &temporaryNeedsRelease]
        {
            if (temporaryNeedsRelease)
                releasePluginResourcesNoThrow (instance.get());
        } };

        // DESLIGA os barramentos de saida extras ANTES de preparar. O Kontakt abre
        // com varios pares de saida por padrao: sem isto ele RENDERIZA todos eles a
        // cada bloco e nos jogamos fora tudo menos o primeiro par. Era a maior
        // conta do motor — o mesmo Kontakt sozinho, com 2 canais, roda folgado.
        try
        {
            if (instance->getBusCount (false) > 1)
            {
                auto layout = instance->getBusesLayout();

                for (int b = 1; b < layout.outputBuses.size(); ++b)
                    layout.outputBuses.getReference (b) =
                        juce::AudioChannelSet::disabled();

                instance->setBusesLayout (layout);
            }

            temporaryNeedsRelease = true;
            instance->prepareToPlay (
                hostSampleRate, juce::jmax (64, lastBlockSize));

            // Restore e two-phase: valide o blob na instancia temporaria antes
            // de limpar/trocar o canal atual. Base64 corrompido ou plugin que
            // lanca deixa o timbre anterior intacto e o alvo pendente salvo.
            if (! applyFactoryDefaults
                 && restoredState.getSize() > 0)
                instance->setStateInformation (
                    restoredState.getData(),
                    (int) restoredState.getSize());

        }
        catch (...)
        {
            failCurrentRequest (
                "Plugin falhou durante prepare/restore");

            return;
        }

        juce::String pluginIdentifier;
        juce::String pluginName;
        bool pluginIsOrgan = false;
        try
        {
            pluginIdentifier = desc.createIdentifierString();
            pluginName = desc.name;
            pluginIsOrgan = looksLikeOrgan (pluginName, {});
            rememberOutgoingTimbreTone (layerIndex);
        }
        catch (...)
        {
            failCurrentRequest (
                "Plugin falhou antes do commit no canal");
            return;
        }

        bool scratchReady = false;
        {
            const bool resumeAfterReserve = ! isSuspended();

            if (resumeAfterReserve)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterReserve]
            {
                if (resumeAfterReserve)
                    suspendProcessing (false);
            } };

            try
            {
                reserveInstrumentScratchFor (*instance);
                scratchReady = true;
            }
            catch (...)
            {
            }
        }

        if (! scratchReady)
        {
            failCurrentRequest (
                "Plugin nao conseguiu reservar o buffer do canal");
            return;
        }

        std::unique_ptr<juce::AudioPluginInstance>
            oldInstrument;
        bool oldInstrumentWasPrepared = false;

        if (! instrumentPluginLoadGeneration[layerIndex]
                 .isCurrent (ticket))
            return;

        if (isHostedPluginBlocked (desc))
        {
            failCurrentRequest (
                "Plugin foi colocado em quarentena antes do commit");
            return;
        }

        // Retire o timbre nativo antes de fechar o editor antigo. Esse callback
        // pode bombear mensagens e iniciar outro pedido; depois dele este
        // resultado antigo apenas revalida o ticket, nunca limpa o pedido novo.
        clearLayer (layerIndex, true);

        if (! instrumentPluginLoadGeneration[layerIndex]
                 .isCurrent (ticket))
            return;

        notifyHostedPluginWillRetire (
            HostedPluginLocation::instrument, layerIndex,
            getLayerInstrument (layerIndex));

        // O callback do editor pode reentrar na message thread e concluir um
        // scan. Nunca limpe o canal atual se o candidato acabou de ser
        // bloqueado nessa janela.
        if (isHostedPluginBlocked (desc))
        {
            failCurrentRequest (
                "Plugin foi colocado em quarentena antes do commit");
            return;
        }

        bool committed = false;
        {
            const bool resumeAfterSwap = ! isSuspended();

            if (resumeAfterSwap)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterSwap]
            {
                if (resumeAfterSwap)
                    suspendProcessing (false);
            } };

            const juce::ScopedLock pluginLock (
                layerPluginStateLock);
            const juce::ScopedLock identityLock (
                layerIdentityLock);
            committed = instrumentPluginLoadGeneration[layerIndex]
                .commitIfCurrent (
                    ticket,
                    [this, layerIndex, &instance,
                     &oldInstrument, &oldInstrumentWasPrepared,
                     &temporaryNeedsRelease, &pluginIdentifier,
                    &pluginName, pluginIsOrgan]
                    {
                        oldInstrumentWasPrepared =
                            pluginPrepared[layerIndex].exchange (
                                false, std::memory_order_acq_rel);
                        oldInstrument = std::move (
                            layers[layerIndex].instrument);
                        layers[layerIndex].instrument =
                            std::move (instance);
                        temporaryNeedsRelease = false;
                        pluginPrepared[layerIndex].store (
                            true, std::memory_order_release);
                        pluginSustainMirror[layerIndex].store (
                            sustainStateUnknown,
                            std::memory_order_release);
                        pluginPitchCentresPending[layerIndex].store (
                            true, std::memory_order_release);

                        layers[layerIndex].instrumentId.swapWith (
                            pluginIdentifier);
                        layers[layerIndex].instrumentName.swapWith (
                            pluginName);
                        pendingVstIds[layerIndex].clear();
                        pendingVstStates[layerIndex].clear();
                        pendingVstDescriptionValid[layerIndex] = false;
                        instrumentPluginLoadInFlight[layerIndex].store (
                            false, std::memory_order_release);
                        layerStateRestoreTargetTimbre[layerIndex].clear();
                        layerStateRestoreTargetLibrary[layerIndex].clear();
                        layerStateRestoreTargetVstId[layerIndex].clear();
                        layerStateRestoreTargetVstState[layerIndex].clear();
                        layerStateRestorePending[layerIndex].store (
                            false, std::memory_order_release);

                        layerCategory[layerIndex].clear();
                        layerIsOrgan[layerIndex].store (
                            pluginIsOrgan, std::memory_order_relaxed);
                        layerNativeFamilyTrim[layerIndex].store (
                            1.0f, std::memory_order_relaxed);
                        setLayerLoaded (layerIndex, true);
                    });

            if (! committed)
                return;

            // Auto EQ por categoria pertence aos packs nativos. Um VST/AU nao
            // pode herdar os coeficientes do piano/pad anterior.
            try
            {
                if (applyFactoryDefaults)
                {
                    // VST/AU novo parte em bypass de Rotary: muitos orgaos
                    // externos ja trazem Leslie e dois rotores criariam afinacao
                    // instavel.
                    applyFactoryTimbreShape (
                        layerIndex, desc.name, {}, false);
                    applyRememberedTimbreTone (
                        layerIndex, desc.name,
                        pluginToneLibrary (
                            desc.createIdentifierString()));
                }
                updateLayerEq (layerIndex);
                manualEqs[layerIndex].syncToTargets (
                    layers[layerIndex].eqLow != nullptr
                        ? (float) *layers[layerIndex].eqLow : 0.0f,
                    layers[layerIndex].eqMid != nullptr
                        ? (float) *layers[layerIndex].eqMid : 0.0f,
                    layers[layerIndex].eqHigh != nullptr
                        ? (float) *layers[layerIndex].eqHigh : 0.0f);
            }
            catch (...)
            {
                // O plugin ja pertence ao grafo. Falha cosmetica/default nao
                // pode produzir completion=false nem destruir o antigo sem o
                // release pareado.
                juce::Logger::writeToLog (
                    "PLUGIN: instrumento instalado; defaults/EQ nao puderam ser aplicados");
            }
        }

        if (oldInstrumentWasPrepared)
            releasePluginResourcesNoThrow (
                oldInstrument.get());
        oldInstrument.reset();

        sendChangeMessage();

        if (instrumentPluginLoadGeneration[layerIndex]
                .isCurrent (ticket))
            invokePluginLoadCompletionNoThrow (
                completion, true, {});
    }

    void clearInstrumentPlugin (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        notifyHostedPluginWillRetire (
            HostedPluginLocation::instrument, layerIndex,
            getLayerInstrument (layerIndex));

        // A limpeza explicita vence qualquer pedido entregue enquanto o editor
        // de vendor fecha e bombeia mensagens.
        instrumentPluginLoadGeneration[layerIndex].cancel();
        instrumentPluginLoadInFlight[layerIndex].store (
            false, std::memory_order_release);

        // A API e publica: mesmo um chamador futuro que nao passe antes por
        // clearLayer nao pode deixar SOLO/identidade fantasma do plugin.
        *layers[layerIndex].solo = false;
        setLayerLoaded (layerIndex, false);

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        rememberOutgoingTimbreTone (layerIndex);

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        {
            const bool resumeAfterClear = ! isSuspended();

            if (resumeAfterClear)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterClear]
            {
                if (resumeAfterClear)
                    suspendProcessing (false);
            } };

            const juce::ScopedLock pluginLock (layerPluginStateLock);
            old = std::move (layers[layerIndex].instrument);
            instNotesOn[layerIndex] = 0;
            noteTranspose.clearLayer (layerIndex);
            pluginSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
            pluginPitchCentresPending[layerIndex].store (
                false,
                std::memory_order_release);
            oldWasPrepared =
                pluginPrepared[layerIndex].exchange (
                false,
                std::memory_order_acq_rel);
            {
                const juce::ScopedLock identityLock (layerIdentityLock);
                layers[layerIndex].instrumentId.clear();
                layers[layerIndex].instrumentName.clear();
            }
            layerIsOrgan[layerIndex].store (
                false, std::memory_order_relaxed);
            layerNativeFamilyTrim[layerIndex].store (
                1.0f, std::memory_order_relaxed);
            rotarySpeakers[layerIndex].reset();
        }

        // canal deixa de ser VSTi: estado pendente de restauracao cai junto
        {
            const juce::ScopedLock lock (layerIdentityLock);
            pendingVstIds[layerIndex].clear();
            pendingVstStates[layerIndex].clear();
            pendingVstDescriptionValid[layerIndex] = false;
        }

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        old.reset();

        bool nativeIdentitySurvived = isLayerChurchOrgan (layerIndex);
        {
            const juce::ScopedLock identityLock (layerIdentityLock);
            nativeIdentitySurvived = nativeIdentitySurvived
                                  || layers[layerIndex].timbre.isNotEmpty();
        }
        setLayerLoaded (layerIndex, nativeIdentitySurvived);

        completeLayerStateRestore (layerIndex);
        sendChangeMessage();
    }

    // decodifica os samples em segundo plano e troca na thread de mensagens
    void loadTimbreIntoLayer (int layerIndex, const TimbreEntry& entry,
                              bool applyFactoryToneOnSuccess = false)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        // Uma escolha nativa substitui também qualquer AU/VST ainda em
        // criação; o callback antigo será descartado pelo ticket.
        instrumentPluginLoadGeneration[layerIndex].cancel();
        instrumentPluginLoadInFlight[layerIndex].store (
            false, std::memory_order_release);
        {
            const juce::ScopedLock lock (layerIdentityLock);
            pendingVstIds[layerIndex].clear();
            pendingVstStates[layerIndex].clear();
            pendingVstDescriptionValid[layerIndex] = false;
        }

        {
            const juce::ScopedLock lock (layerIdentityLock);

            if (layerStateRestorePending[layerIndex].load (
                    std::memory_order_acquire))
            {
                // Um restore falho pode ser substituido por uma escolha manual.
                // O alvo muda antes de qualquer copia/alocacao do loader.
                layerStateRestoreTargetTimbre[layerIndex] = entry.label;
                layerStateRestoreTargetLibrary[layerIndex] = entry.library;
                layerStateRestoreTargetVstId[layerIndex].clear();
                layerStateRestoreTargetVstState[layerIndex].clear();
            }
        }

        std::vector<juce::File> files;
        juce::String label, library, category;
        std::shared_ptr<std::atomic<bool>> requestAbort;
        std::shared_ptr<std::atomic<bool>> completionDone;
        std::shared_ptr<std::vector<std::unique_ptr<Zone>>> zones;

        try
        {
            // Todas as copias/alocacoes acontecem ANTES de cancelar o pedido
            // atual ou incrementar loadingCount. Se nem isto couber, o timbre
            // que ja toca segue intacto e nao deixamos splash/worker pela metade.
            files.reserve ((size_t) entry.files.size());

            for (const auto& file : entry.files)
                files.push_back (file);
            label = entry.label;
            library = entry.library;
            category = entry.category;
            requestAbort = std::make_shared<std::atomic<bool>> (false);
            completionDone = std::make_shared<std::atomic<bool>> (false);
            zones = std::make_shared<std::vector<std::unique_ptr<Zone>>>();
        }
        catch (...)
        {
            sendChangeMessage();
            return;
        }

        // A geracao impedia o resultado velho de ser INSTALADO, mas nao impedia
        // que ele continuasse decodificando gigabytes. Numa troca rapida, cada
        // clique criava outro piano inteiro em paralelo. O token por canal faz o
        // pedido anterior parar no proximo arquivo sem cancelar os outros canais.
        cancelLayerLoad (layerIndex);
        layerLoadAbort[layerIndex] = requestAbort;

        // geracao por canal: se o canal for limpo/trocado antes do decode acabar,
        // finishLayerLoad descarta o resultado velho
        const int gen = ++layerLoadGeneration[layerIndex];

        // rotulo em voo: um save durante o decode ainda grava este timbre
        {
            const juce::ScopedLock lock (layerIdentityLock);
            loadingLabels[layerIndex] = entry.label;
            loadingLibs[layerIndex] = entry.library;
        }

        loadingCount->fetch_add (1, std::memory_order_acq_rel);
        sendChangeMessage();

        juce::WeakReference<TimbresProcessor> weakThis (this);
        auto abort = workerAbort;   // shared_ptr POR VALOR: sobrevive ao processador
        auto counter = loadingCount;
        auto gate = layerDecodeGate;
        auto completeLoading = [counter, completionDone]() noexcept
        {
            if (! completionDone->exchange (true, std::memory_order_acq_rel))
                counter->fetch_sub (1, std::memory_order_acq_rel);
        };

        // Construcao do job usa MOVE e acontece antes de entrar no boundary
        // noexcept do criador de thread; nao duplica o array de 880 caminhos.
        auto job =
            [weakThis, layerIndex,
             files = std::move (files),
             label = std::move (label),
             library = std::move (library),
             category = std::move (category),
             gen, abort, requestAbort, zones, gate, completeLoading,
             applyFactoryToneOnSuccess] () mutable
        {
            const double startedAt = juce::Time::getMillisecondCounterHiRes();
            enum class Failure { none, outOfMemory, decode, unknown };
            Failure failure = Failure::none;
            bool callbackOwnsCompletion = false;

            try
            {
                const auto cancelledBeforeGate =
                    decodeCancelled (abort.get(), requestAbort.get());

                if (! cancelledBeforeGate)
                {
                    // Um piano por vez POR PROCESSADOR. Outros canais ficam
                    // esperando sem alocar AudioBuffers; jobs cancelados antes
                    // da vez nem entram no mutex, e os cancelados enquanto
                    // esperavam saem imediatamente depois de adquiri-lo.
                    std::unique_lock<std::mutex> decodeLock (*gate);

                    if (! decodeCancelled (abort.get(), requestAbort.get()))
                        *zones = decodeZones (
                            files, abort.get(), requestAbort.get());
                }
            }
            catch (const std::bad_alloc&)
            {
                failure = Failure::outOfMemory;
            }
            catch (const std::exception&)
            {
                failure = Failure::decode;
            }
            catch (...)
            {
                failure = Failure::unknown;
            }

            const bool cancelled = abort->load (std::memory_order_acquire)
                                || requestAbort->load (std::memory_order_acquire);

            // Resultado parcial cancelado nao deve atravessar a fila de mensagens:
            // libera o preload nesta propria thread e posta apenas a conclusao.
            if (cancelled || failure != Failure::none)
                zones->clear();

            const int decodedZoneCount = (int) zones->size();
            const double elapsedSeconds =
                (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0;

            auto enqueueCompletion = [&]() noexcept
            {
                try
                {
                    return juce::MessageManager::callAsync (
                        [weakThis, layerIndex, zones, label, library, category,
                         gen, completeLoading, applyFactoryToneOnSuccess]
                    {
                        // Normalmente o contador volta na thread de mensagens,
                        // antes de publicar a troca. Mesmo se o processador ja
                        // morreu, o ticket compartilhado e concluido uma vez.
                        completeLoading();

                        if (auto* self = weakThis.get())
                            try
                            {
                                self->finishLayerLoad (
                                    layerIndex, std::move (*zones),
                                    label, library, category, gen,
                                    applyFactoryToneOnSuccess);
                            }
                            catch (...)
                            {
                                // O commit tem RAII de suspensao e ownership.
                                // Falha de alocacao nunca pode escapar da fila
                                // de mensagens e encerrar o aplicativo.
                                self->schedulePurge();
                            }
                    });
                }
                catch (...)
                {
                    return false;
                }
            };

            // callAsync retorna bool. Se a primeira postagem falhar por pressao
            // de memoria, libera o preload pesado e tenta UMA conclusao vazia.
            // Se nem ela couber (fila ja encerrada/shutdown), apenas o ticket
            // atomico e concluido no worker; nenhum estado/audio e tocado fora
            // da message thread e loadingCount nunca fica preso.
            callbackOwnsCompletion = enqueueCompletion();

            if (! callbackOwnsCompletion)
            {
                zones->clear();
                callbackOwnsCompletion = enqueueCompletion();
            }

            if (! callbackOwnsCompletion)
                completeLoading();

            try
            {
                if (failure != Failure::none)
                {
                    const char* reason = failure == Failure::outOfMemory ? "memoria insuficiente"
                                       : failure == Failure::decode      ? "erro de decode"
                                                                        : "erro desconhecido";
                    juce::Logger::writeToLog ("BIBLIOTECA: falha segura ao carregar "
                                              + label + " (" + reason + ")");
                }
                else if (! cancelled)
                {
                    juce::Logger::writeToLog ("BIBLIOTECA: timbre " + label + " pronto em "
                                              + juce::String (elapsedSeconds, 2)
                                              + " s (" + juce::String (decodedZoneCount) + " zonas)");
                }
            }
            catch (...)
            {
                // O log e diagnostico; a conclusao ja foi postada/contabilizada.
            }
        };

        const bool started = startWorkerThread (std::move (job));

        if (! started)
        {
            requestAbort->store (true, std::memory_order_release);
            completeLoading();
            juce::Logger::writeToLog ("BIBLIOTECA: nao foi possivel iniciar o worker de timbre");
            sendChangeMessage();
        }
    }

    //==============================================================================
    const juce::String getName() const override { return "Church Live"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        hostSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
        const int block = juce::jmax (64, samplesPerBlock);
        lastBlockSize = block;

        // A rampa comeca do valor real de cada canal, e nao de zero: senao
        // todo canal ligado faria um fade-in de 20 ms ao trocar o buffer de
        // audio no meio do culto.
        //
        // O SOLO PRECISA ENTRAR NESTA CONTA. Sem ele, um canal silenciado por
        // solo alheio comecava com o ganho cheio e a rampa o levava a zero
        // tocando — ou seja, vazava audio de canais que deviam estar mudos
        // logo apos qualquer troca de buffer. Esta condicao tem de ser a
        // mesma do "audible" usado no render.
        const bool algumSolo = hasAnyLoadedSolo();

        for (int l = 0; l < numLayers; ++l)
        {
            const auto& lay = layers[l];
            const bool audivel = lay.on != nullptr && (bool) *lay.on
                                  && ! (lay.mute != nullptr && (bool) *lay.mute)
                                  && (! algumSolo
                                       || (lay.solo != nullptr && (bool) *lay.solo));

            const float ganhoGlobalAtual = gain != nullptr ? (float) *gain : 1.0f;
            postGainSuave[l] = audivel && lay.vol != nullptr
                                 ? (float) *lay.vol * ganhoGlobalAtual
                                 : 0.0f;
        }

        // O callback copia/remapeia MIDI ate tres vezes. Reserve uma rajada
        // deliberadamente grande aqui para que aftertouch/CC de alta taxa e o
        // PANIC de 16 canais nao facam o MidiBuffer crescer durante o audio.
        constexpr size_t realtimeMidiReserve = 256 * 1024;
        midiScratch.ensureSize (realtimeMidiReserve);
        midiCopy.ensureSize (realtimeMidiReserve);
        layerMidi.ensureSize (realtimeMidiReserve);
        pitchBendGuard.requestPitchCentresOnNextBlock();

       #if JUCE_ANDROID || JUCE_MAC
        // O marcador e consultado aqui, e nao a cada bloco: tocar o disco na
        // thread de audio seria criar o defeito que estamos medindo.
       #if JUCE_ANDROID
        //
        // No Android 11+ um .txt dentro de Music pode ficar invisivel para um
        // app que possui apenas READ_MEDIA_AUDIO. Por isso os marcadores novos
        // usam extensao WAV (o conteudo nunca e aberto) e entram normalmente no
        // MediaStore. Mantemos o .txt para builds/telefones antigos.
        const auto testRoot = AppPaths::sharedDataFolder();
        selfTestStress = testRoot.getChildFile ("selftest-stress.wav").existsAsFile();
        selfTestOn = selfTestStress
                  || testRoot.getChildFile ("selftest-worship.wav").existsAsFile()
                  || testRoot.getChildFile ("selftest.txt").existsAsFile();
       #else
        // No macOS o diagnostico de CI e ativado somente para o processo de
        // teste. Um comprador nunca recebe notas automaticas por acidente.
        const auto selfTestMode = juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_SELFTEST", {});
        selfTestStress = selfTestMode.equalsIgnoreCase ("stress");
        selfTestRestore = selfTestMode.equalsIgnoreCase ("restore");
        selfTestExternalMidi = selfTestMode.equalsIgnoreCase ("external");
        selfTestOn = selfTestStress
                  || selfTestMode.equalsIgnoreCase ("worship")
                  || selfTestRestore
                  || selfTestExternalMidi;
        selfTestQuitSamples = (juce::int64) (hostSampleRate
            * juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_SELFTEST_QUIT_SECONDS", "0")
                  .getDoubleValue());
       #endif

        if (selfTestOn)
        {
            auto testName = selfTestStress ? juce::String ("ESTRESSE")
                           : selfTestExternalMidi ? juce::String ("MIDI EXTERNO")
                           : selfTestRestore ? juce::String ("RESTAURACAO")
                           : juce::String ("WORSHIP SUAVE");
            juce::Logger::writeToLog ("AUDIO: TESTE " + testName + " LIGADO");
        }
       #endif

       #if CHURCHDAW_STREAM_SAMPLES
        // Aloca TODAS as janelas aqui, nunca durante o toque: 256 vozes x 2
        // janelas x 16384 x 2 canais x 4 bytes = 64 MB, reservados de uma vez.
        // Alocar no note-on seria pedir memoria na thread de audio -- a origem
        // classica do estalo aleatorio que nao se reproduz.
        if (streamer.numStreams() != maxVoices)
            streamer.prepare (maxVoices);
       #endif

        const int reservedBlock = juce::jmax (realtimeBlockReserve, block);

        for (auto& b : layerBuffers)
            b.setSize (2, reservedBlock, false, false, true);

        for (int l = 0; l < numLayers; ++l)
        {
            churchOrganReverbBuffers[l].setSize (
                2, reservedBlock, false, false, true);
            churchOrganReverbBuffers[l].clear();

            auto* organ = layers[l].churchOrgan.get();

            if (organ == nullptr)
                continue;

            const bool active =
                layerIsChurchOrgan[l].load (
                    std::memory_order_acquire);
            juce::MemoryBlock stateToRestore;

            organ->setProcessingSuspended (true);

            if (pendingChurchOrganStates[l].getSize() > 0)
                stateToRestore = pendingChurchOrganStates[l];
            else if (active && organ->diagnostics().prepared)
                organ->getStateInformation (stateToRestore);

            churchorgan::ChurchOrganLayerPrepareSpec organSpec;
            organSpec.sampleRate = hostSampleRate;
            organSpec.maximumBlockSize =
                static_cast<std::size_t> (reservedBlock);
            organSpec.deterministicSeed ^=
                static_cast<std::uint64_t> (l + 1)
                    * 0x9e3779b97f4a7c15ULL;
            organSpec.initialFamily =
                getLayerChurchOrganFamily (l);

            const bool prepared = organ->prepare (organSpec);
            bool stateRestored =
                stateToRestore.getSize() == 0;
            bool stateReady = stateRestored;

            // prepare() deliberadamente deixa o adapter apto a processar.
            // Restauração/reset/seleção só são legais suspensos.
            if (prepared)
                organ->setProcessingSuspended (true);

            if (prepared && stateToRestore.getSize() > 0)
                stateRestored = organ->setStateInformation (
                    stateToRestore.getData(),
                    stateToRestore.getSize());

            stateReady = prepared && stateRestored;

            if (prepared && ! stateReady)
                stateReady =
                    organ->selectFamilyWhileSuspended (
                        getLayerChurchOrganFamily (l));

            if (prepared && stateReady)
            {
                for (int family = 0; family < 4; ++family)
                    organ->setFamilyOutputTrim (
                        static_cast<churchorgan::ChurchOrganFamily> (
                            family),
                        1.0f);

                layerChurchOrganFamily[l].store (
                    static_cast<int> (organ->activeFamily()),
                    std::memory_order_release);
                lastChurchOrganChannelRotaryMode[l].store (
                    stateToRestore.getSize() > 0
                        && layers[l].rotaryMode != nullptr
                            ? layers[l].rotaryMode->getIndex()
                            : -1,
                    std::memory_order_release);

                // Um blob pendente só deixa de existir depois que o adapter
                // confirmou a restauração. O fallback mantém o blob para
                // diagnóstico/auto-cura em um save posterior.
                if (stateRestored)
                    pendingChurchOrganStates[l].reset();
            }
            else if (active && ! stateReady)
            {
                // Falha de preparação nunca deixa um ponteiro parcialmente
                // pronto entrar no callback.
                layerIsChurchOrgan[l].store (
                    false, std::memory_order_release);
                layerIsOrgan[l].store (
                    false, std::memory_order_release);
                setLayerLoaded (l, false);
            }

            organ->setProcessingSuspended (
                ! prepared || ! stateReady || ! active);
            churchOrganSustainMirror[l].store (
                sustainStateUnknown,
                std::memory_order_release);
        }

        int pluginScratchChannels = 2;
        for (const auto& lay : layers)
            if (lay.instrument != nullptr)
                pluginScratchChannels = juce::jmax (
                    pluginScratchChannels,
                    juce::jmax (lay.instrument->getTotalNumInputChannels(),
                                lay.instrument->getTotalNumOutputChannels()));

        instrumentBuf.setSize (pluginScratchChannels, reservedBlock,
                               false, false, true);

        // Reserva larga: setSize no callback so muda o tamanho logico e nunca
        // precisa pedir memoria se o backend entregar um bloco maior na troca
        // de rota (Bluetooth/USB/AAudio).
        reverbBus.setSize (2, reservedBlock, false, false, true);
        reverbBus.clear();
        worshipReverb.setPreset (nativeReverbPreset != nullptr
                                   ? nativeReverbPreset->getIndex() : 1);
        worshipReverb.prepare (hostSampleRate, block);
        reverbReturnSmooth.reset (hostSampleRate, 0.040);
        const float initialReverbReturn =
            reverbReturn != nullptr ? (float) *reverbReturn : 0.72f;
        reverbReturnSmooth.setCurrentAndTargetValue (
            std::isfinite (initialReverbReturn)
                ? juce::jlimit (0.0f, 1.0f, initialReverbReturn)
                : 0.72f);

        for (int l = 0; l < numLayers; ++l)
        {
            reverbSendSmooth[l].reset (hostSampleRate, 0.030);
            reverbSendSmooth[l].setCurrentAndTargetValue (
                layers[l].reverbSend != nullptr ? (float) *layers[l].reverbSend : 0.0f);
            delayBufs[l].setSize (2, (int) (hostSampleRate * 0.6) + block, false, false, true);
            delayBufs[l].clear();
            delayWrite[l] = 0;
            hpState[l][0] = hpState[l][1] = 0.0f;
            lpState[l][0] = lpState[l][1] = 0.0f;

            eqs[l].prepare (hostSampleRate, block);
            eqs[l].setCoeffs (eqPresetFor (layerCategory[l], hostSampleRate));
            manualEqs[l].prepare (
                hostSampleRate, block,
                layers[l].eqLow != nullptr ? (float) *layers[l].eqLow : 0.0f,
                layers[l].eqMid != nullptr ? (float) *layers[l].eqMid : 0.0f,
                layers[l].eqHigh != nullptr ? (float) *layers[l].eqHigh : 0.0f);
            rotarySpeakers[l].prepare (hostSampleRate, block);
        }

        {
            const juce::ScopedLock callbackGuard (
                getCallbackLock());

            for (int slot = 0;
                 slot < numInserts;
                 ++slot)
            {
                const bool wasPrepared =
                    insertPrepared[slot].exchange (
                        false,
                        std::memory_order_acq_rel);

                if (inserts[slot] == nullptr)
                    continue;

                if (wasPrepared)
                    releasePluginResourcesNoThrow (
                        inserts[slot].get());

                try
                {
                    inserts[slot]->prepareToPlay (
                        hostSampleRate, block);
                    insertPrepared[slot].store (
                        true,
                        std::memory_order_release);
                }
                catch (...)
                {
                    // Mantém a instância/estado para uma futura tentativa, mas
                    // o callback a deixa em bypass até um prepare válido.
                    try
                    {
                        inserts[slot]->releaseResources();
                    }
                    catch (...)
                    {
                    }

                    juce::Logger::writeToLog (
                        "PLUGIN: insert "
                        + juce::String (slot + 1)
                        + " falhou em prepareToPlay; bypass ativo");
                }
            }

            refreshExternalReverbSlot (-1);
            forceExternalReverbWet (getReverbSlot());
        }

        {
            const juce::ScopedLock pluginLock (
                layerPluginStateLock);

            for (int layer = 0;
                 layer < numLayers;
                 ++layer)
            {
                const bool wasPrepared =
                    pluginPrepared[layer].exchange (
                        false,
                        std::memory_order_acq_rel);

                if (layers[layer].instrument == nullptr)
                    continue;

                if (wasPrepared)
                    releasePluginResourcesNoThrow (
                        layers[layer].instrument.get());

                try
                {
                    layers[layer].instrument->prepareToPlay (
                        hostSampleRate, block);
                    pluginPrepared[layer].store (
                        true,
                        std::memory_order_release);
                }
                catch (...)
                {
                    try
                    {
                        layers[layer].instrument
                            ->releaseResources();
                    }
                    catch (...)
                    {
                    }

                    juce::Logger::writeToLog (
                        "PLUGIN: canal "
                        + juce::String (layer + 1)
                        + " falhou em prepareToPlay; bypass ativo");
                }
            }
        }

        for (auto& sustainMirror : pluginSustainMirror)
            sustainMirror.store (
                sustainStateUnknown,
                std::memory_order_release);

        for (auto& centresPending :
                pluginPitchCentresPending)
            centresPending.store (
                false,
                std::memory_order_release);

        const juce::dsp::ProcessSpec spec { hostSampleRate, (juce::uint32) block, 2 };

        for (auto& c : comps)
        {
            c.prepare (spec);
            c.setAttack (8.0f);
            c.setRelease (150.0f);
        }

        for (auto& v : voices)
        {
            v.adsr.setSampleRate (hostSampleRate);
            v.active = false;

           #if CHURCHDAW_STREAM_SAMPLES
            streamer.stream ((int) (&v - voices)).stop();
           #endif
        }

        processorPrepared.store (true, std::memory_order_release);
    }

    void releaseResources() override
    {
        // JUCE pode alternar a rota de áudio várias vezes durante a vida do
        // processador. Cada prepareToPlay() encaminhado a AU/VST/insert precisa
        // de um releaseResources() correspondente; o corpo vazio antigo vazava
        // recursos de plugins que dependem desse pareamento.
        const bool wasProcessorPrepared =
            processorPrepared.exchange (
                false, std::memory_order_acq_rel);
        const bool resumeAfterRelease = ! isSuspended();

        if (resumeAfterRelease)
            suspendProcessing (true);
        const juce::ScopeGuard restoreProcessing {
            [this, resumeAfterRelease]
        {
            if (resumeAfterRelease)
                suspendProcessing (false);
        } };

        {
            const juce::ScopedLock callbackGuard (
                getCallbackLock());

            for (int slot = 0; slot < numInserts; ++slot)
            {
                const bool wasPrepared =
                    insertPrepared[slot].exchange (
                    false,
                    std::memory_order_acq_rel);

                if (inserts[slot] != nullptr
                     && wasPrepared)
                    try
                    {
                        inserts[slot]->releaseResources();
                    }
                    catch (...)
                    {
                        juce::Logger::writeToLog (
                            "PLUGIN: insert " + juce::String (slot + 1)
                            + " falhou em releaseResources");
                    }
            }

            {
                const juce::ScopedLock pluginLock (
                    layerPluginStateLock);

                for (int layer = 0;
                     layer < numLayers;
                     ++layer)
                {
                    const bool wasPrepared =
                        pluginPrepared[layer].exchange (
                        false,
                        std::memory_order_acq_rel);

                    if (layers[layer].instrument != nullptr
                         && wasPrepared)
                        try
                        {
                            layers[layer].instrument
                                ->releaseResources();
                        }
                        catch (...)
                        {
                            juce::Logger::writeToLog (
                                "PLUGIN: canal "
                                + juce::String (layer + 1)
                                + " falhou em releaseResources");
                        }
                }
            }

            refreshExternalReverbSlot (-1);

            // Um processor pode ser destruido antes do primeiro prepareToPlay
            // (scan/teste/erro de rota). Nesse estado SampleStreamer ainda nao
            // possui maxVoices entradas e stopAllVoices acessaria fora do vetor.
            // O exchange tambem torna release/destrutor idempotentes.
            if (wasProcessorPrepared)
                stopAllVoices();
            pitchBendGuard.reset();
            panicPending.store (
                false,
                std::memory_order_release);

            for (auto& centresPending :
                    pluginPitchCentresPending)
                centresPending.store (
                    false,
                    std::memory_order_release);

            worshipReverb.reset();
            reverbBus.clear();

            for (auto& rotary : rotarySpeakers)
                rotary.reset();
        }
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        // denormals nas caudas de reverb/delay explodem o uso de CPU (pipocos)
        juce::ScopedNoDenormals noDenormals;

       #if JUCE_ANDROID || JUCE_MAC
        const auto blockStart = juce::Time::getHighResolutionTicks();
       #endif

       #if JUCE_ANDROID
        // MEDICAO DO PRAZO. Um bloco de N amostras a SR precisa sair em menos de
        // N/SR segundos; passou disso, a placa fica sem o que tocar e sai estalo.
        // Medir separa duas causas que SOAM IGUAIS e se resolvem ao contrario:
        //   carga alta          -> otimizar o motor / aumentar o buffer
        //   carga baixa e estala-> a thread nao esta sendo ACORDADA a tempo,
        //                          e mexer no motor nao resolve nada
       #endif

       #if JUCE_ANDROID || JUCE_MAC
        selfTestTick (midi, buffer.getNumSamples());
       #endif

        buffer.clear();

        // O render dos órgãos reconstrói o estado sample-accurate dos CC64 a
        // partir deste ponto. Usar apenas pedalDown depois de varrer o bloco
        // anteciparia um pedal que chegou no meio do buffer para a amostra 0.
        pedalDownAtBlockStart = pedalDown;

        // sustain global desligado: solta as notas que ele segurava
        const bool gs = (bool) *globalSustain;

        if (prevGlobalSustain && ! gs && ! pedalDown)
            releasePedalHeldVoices();

        prevGlobalSustain = gs;

        // auto-cura anti-pedal-fantasma: se nenhum sustain esta ativo, nenhuma
        // voz pode seguir "segurada por pedal" (eventos MIDI perdidos durante a
        // troca de timbre viravam notas presas sem isso)
        if (! pedalDown && ! gs)
            releasePedalHeldVoices();

        // Resposta do teclado: aplicada UMA vez, aqui na entrada, antes de
        // qualquer coisa ler o bloco. Assim vozes nativas, VSTi de canal e
        // inserts do MASTER FX veem todos a MESMA velocity -- e, principalmente,
        // a curva passa a escolher tambem a CAMADA do multisample, nao so o
        // volume: mexer apenas no ganho deixaria a camada de pianissimo tocando
        // com forca de forte, que e o erro classico de "curva de velocity".
        if (const int curve = velCurve->getIndex(); curve != curveNatural)
        {
            midiScratch.clear();

            for (const auto meta : midi)
            {
                auto m = meta.getMessage();

                // noteOn com velocity 0 e note-off disfarcado: nao remapear
                if (m.isNoteOn() && m.getVelocity() > 0)
                    m = juce::MidiMessage::noteOn (m.getChannel(), m.getNoteNumber(),
                                                   (juce::uint8) mapVelocity (m.getVelocity(), curve));

                midiScratch.addEvent (m, meta.samplePosition);
            }

            midi.swapWith (midiScratch);
        }

        // pedal invertido (pedal sem chave): reescreve o CC64 NA FONTE, antes da
        // copia -- senao VSTi de canal e inserts recebem o sustain ao contrario
        if ((bool) *pedalInvert)
        {
            // reusa o MESMO scratch do remap acima: os dois blocos rodam em
            // sequencia e cada um faz clear antes de preencher
            midiScratch.clear();

            for (const auto meta : midi)
            {
                auto m = meta.getMessage();

                if (m.isController() && m.getControllerNumber() == 64)
                    m = juce::MidiMessage::controllerEvent (m.getChannel(), 64, 127 - m.getControllerValue());

                midiScratch.addEvent (m, meta.samplePosition);
            }

            midi.swapWith (midiScratch);
        }

        midiCopy.clear();
        midiCopy.addEvents (midi, 0, buffer.getNumSamples(), 0);   // para os plugins internos

        // PANIC pedido pela interface: entra no inicio do bloco e alcanca TUDO que
        // recebe MIDI daqui pra frente (VSTi dos canais e inserts do MASTER FX).
        if (panicPending.exchange (false))
        {
            pitchBendGuard.reset();
            ChurchDAWPerformanceMidiSafety::appendFullPanic (midiCopy, 0);

            for (auto& n : instNotesOn)
                n = 0;

            noteTranspose.clear();

            for (auto& sustainMirror :
                    churchOrganSustainMirror)
                sustainMirror.store (
                    sustainStateUnknown,
                    std::memory_order_release);

            for (auto& sustainMirror :
                    pluginSustainMirror)
                sustainMirror.store (
                    sustainStateUnknown,
                    std::memory_order_release);

            for (auto& centresPending :
                    pluginPitchCentresPending)
                centresPending.store (
                    false,
                    std::memory_order_release);
        }

        // AU/VST que perde o retorno do Pitch Wheel pode ficar desafinado para
        // sempre. O guard preserva bend e pre-bend intencionais, mas recentra
        // um valor herdado da frase anterior antes da proxima nota. Usa o
        // scratch pre-reservado em prepareToPlay(), sem alocacao por bloco.
        pitchBendGuard.process (
            midiCopy, midiScratch, gs,
            pedalDownAtBlockStart ? 1 : 0);
        midiCopy.swapWith (midiScratch);

        // O guard central também define a ordem determinística da fronteira
        // all-off: todo evento comum da mesma amostra vem antes de CC120/123.
        // O sampler nativo precisa consumir o MESMO stream de Church Organ e
        // VST/AU; usar o MIDI bruto aqui recriaria a divergência
        // all-off->note-on que deixava apenas o piano nativo tocando.
        for (const auto meta : midiCopy)
        {
            const auto m = meta.getMessage();

            if (m.isNoteOn())               noteOn (m.getChannel(), m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff())         noteOff (m.getChannel(), m.getNoteNumber());
            else if (m.isSustainPedalOn())  setPedal (true);    // inversao ja aplicada na fonte
            else if (m.isSustainPedalOff()) setPedal (false);
            else if (m.isController()
                      && m.getControllerNumber() == 121)
                setPedal (false);
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                stopAllVoices();
        }

        midi.clear();

        // rede de seguranca anti-nota-presa: voz sem tecla pressionada, sem
        // pedal e sem release e orfa - solta na hora
        for (auto& v : voices)
            if (v.active && ! v.released && ! v.heldByPedal
                 && juce::isPositiveAndBelow (v.note, 128) && keyDown[v.note] == 0)
                releaseVoice (v);

        // EFEITO DE ENVIO: cada canal manda a sua dose para este barramento.
        // No computador um BigSky carregado pode continuar sendo o backend; no
        // Android (e como fallback em qualquer plataforma) o Church Reverb
        // nativo processa o mesmo barramento, sempre 100% molhado.
        const int reverbSlot = getReverbSlot();
        reverbBusActive = true;
        reverbBus.setSize (2, buffer.getNumSamples(), false, false, true);
        reverbBus.clear();

        renderVoices (buffer);

       #if JUCE_ANDROID
        const auto reverbStart = juce::Time::getHighResolutionTicks();
       #endif

        const float rawReturnTarget =
            reverbReturn != nullptr ? (float) *reverbReturn : 0.72f;
        const float returnTarget =
            std::isfinite (rawReturnTarget)
                ? juce::jlimit (0.0f, 1.0f, rawReturnTarget)
                : 0.72f;
        reverbReturnSmooth.setTargetValue (returnTarget);
        const float returnStart = reverbReturnSmooth.getCurrentValue();
        const float returnEnd = reverbReturnSmooth.skip (buffer.getNumSamples());

        auto addReverbReturn = [&] ()
        {
            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            {
                if (std::abs (returnEnd - returnStart) > 0.000001f)
                    buffer.addFromWithRamp (ch, 0, reverbBus.getReadPointer (ch),
                                            buffer.getNumSamples(), returnStart, returnEnd);
                else
                    buffer.addFrom (ch, 0, reverbBus, ch, 0,
                                    buffer.getNumSamples(), returnEnd);
            }
        };

        if (reverbSlot < 0)
        {
            worshipReverb.setPreset (nativeReverbPreset != nullptr
                                       ? nativeReverbPreset->getIndex() : 1);
            worshipReverb.process (reverbBus);
            addReverbReturn();
        }

        for (int i = 0; i < numInserts; ++i)
        {
            auto* ins = inserts[i].get();

            if (ins == nullptr
                 || ! insertPrepared[i].load (
                        std::memory_order_acquire))
                continue;

            if (i == reverbSlot)
            {
                // Mudancas feitas dentro da GUI do BigSky tambem ficam
                // espelhadas no fallback nativo. Sem isso, remover o plugin
                // voltava para uma maquina antiga apesar de o combo mostrar a
                // escolha atual.
                if (auto* type = externalReverbTypeParam.load (std::memory_order_acquire);
                    type != nullptr && nativeReverbPreset != nullptr)
                {
                    const int idx = juce::jlimit (
                        0, numReverbMachines - 1,
                        juce::roundToInt (type->getValue()
                                          * (float) (numReverbMachines - 1)));

                    if (nativeReverbPreset->getIndex() != idx)
                        static_cast<juce::AudioProcessorParameter*> (
                            nativeReverbPreset)->setValue (
                            (float) idx / (float) (numReverbMachines - 1));
                }

                // O editor do plugin pode tentar baixar o MIX. O barramento de
                // envio precisa continuar 100% wet; AudioProcessorParameter
                // aceita automacao na thread de audio e o ponteiro foi cacheado
                // fora dela quando o insert mudou.
                if (auto* mix = externalReverbMixParam.load (std::memory_order_acquire);
                    mix != nullptr && mix->getValue() < 0.9999f)
                    mix->setValue (1.0f);

                // barramento de envio: processa e soma de volta
                ins->processBlock (reverbBus, midiCopy);
                addReverbReturn();
            }
            else
            {
                ins->processBlock (buffer, midiCopy);   // demais efeitos seguem em serie
            }
        }

       #if JUCE_ANDROID
        fxTicks += juce::Time::getHighResolutionTicks() - reverbStart;
       #endif

        // protecao: se algo explodir (NaN/sobrecarga), reseta o processamento.
        // NaN e detectado pela SOMA dos samples: o min/max SIMD do getMagnitude
        // engole NaN no MEIO do buffer, mas na soma qualquer NaN/Inf contamina
        // o total. O teste de magnitude segue sendo o de NIVEL absurdo.
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const float* chk = buffer.getReadPointer (ch);
            float sum = 0.0f;

            for (int s = 0; s < buffer.getNumSamples(); ++s)
                sum += chk[s];

            const float m = buffer.getMagnitude (ch, 0, buffer.getNumSamples());

            if (! std::isfinite (sum) || m > 24.0f)
            {
                #if JUCE_ANDROID
                 ++nanGuardHits;
                 lastNanMag = m;
                 lastNanSum = sum;
                #endif
                buffer.clear();

                // NaN preso na memoria dos efeitos (delay realimentado, filtros,
                // compressor, Auto EQ) silenciaria o master pra sempre
                for (int l = 0; l < numLayers; ++l)
                {
                    delayBufs[l].clear();
                    hpState[l][0] = hpState[l][1] = 0.0f;
                    lpState[l][0] = lpState[l][1] = 0.0f;
                    comps[l].reset();
                    eqs[l].reset();
                    manualEqs[l].reset();
                    rotarySpeakers[l].reset();
                }

                worshipReverb.reset();
                reverbBus.clear();

                if (auto* external = getInsert (reverbSlot))
                    external->reset();

                break;
            }
        }

       #if JUCE_ANDROID
        {
            const double used = juce::Time::highResolutionTicksToSeconds
                                    (juce::Time::getHighResolutionTicks() - blockStart);
            const double budget = (double) buffer.getNumSamples() / juce::jmax (1.0, hostSampleRate);
            const double load = budget > 0.0 ? used / budget : 0.0;

            ++blocksSeen;

            sumBudget += budget;
            peakLoad = juce::jmax (peakLoad, load);
            sumLoad += load;

            // PICO DE SAIDA. Medir isto separa dois defeitos que soam IGUAIS:
            //   falha de audio (o motor nao entregou a tempo)  -> ja descartado,
            //                                                     0 blocos atrasados
            //   ESTOURO (entregou, mas acima de 0 dBFS)        -> distorce, e o
            //                                                     estalo e continuo
            // Acima de 1.0 o conversor corta a onda no topo, e piano cortado soa
            // exatamente como pipoco.
            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
                peakOut = juce::jmax (peakOut, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

            if (load > 0.90)
                ++blocksLate;

            // INTERVALO ENTRE CHAMADAS -- a metade que faltava.
            //
            // Medir so a duracao do bloco responde "o motor e rapido?", e ele e.
            // Nao responde "ele foi CHAMADO na hora?". Se o sistema acorda a
            // thread tarde, a placa fica sem material mesmo com o motor ocioso --
            // e o estalo e identico. Aqui o prazo entre uma chamada e a proxima
            // deveria ser exatamente o tamanho do bloco em segundos.
            if (lastBlockStart != 0)
            {
                const double gap = juce::Time::highResolutionTicksToSeconds (blockStart - lastBlockStart);

                peakGap = juce::jmax (peakGap, gap / budget);

                if (gap > budget * 1.6)
                    ++lateWakes;
            }

            lastBlockStart = blockStart;

            // Relatorio uma vez por segundo, e do PROPRIO caminho do audio: um
            // timer da interface mediria outra coisa. O texto e montado aqui
            // mesmo -- nao e ideal numa thread de audio, mas acontece 1x/s e o
            // que ele revela vale mais que o risco de um estalo a cada segundo.
            if (blocksSeen * budget >= 1.0)
            {
               #if CHURCHDAW_AUDIO_INSTRUMENTATION
                juce::Logger::writeToLog ("AUDIO: media "
                    + juce::String (100.0 * sumLoad / (double) blocksSeen, 1) + "%  pico "
                    + juce::String (100.0 * peakLoad, 1) + "%  lentos "
                    + juce::String (blocksLate) + "/" + juce::String (blocksSeen)
                    + "  | CHAMADA atrasada " + juce::String (lateWakes)
                    + "x  pior intervalo " + juce::String (peakGap, 2) + "x"
                    + "  | PICO " + juce::String (juce::Decibels::gainToDecibels (peakOut), 1) + " dBFS"
                    + (peakOut > 1.0f ? "  <<< ESTOURANDO" : juce::String())
                    + "  | VOZES " + juce::String (100.0 * juce::Time::highResolutionTicksToSeconds (voicesTicks) / juce::jmax (0.001, sumBudget), 0) + "%"
                    + "  FX " + juce::String (100.0 * juce::Time::highResolutionTicksToSeconds (fxTicks) / juce::jmax (0.001, sumBudget), 0) + "%"
                    + (nanGuardHits > 0 ? ("  <<< PROTECAO NaN DISPAROU " + juce::String (nanGuardHits) + "x (mag " + juce::String (lastNanMag, 2) + " soma " + juce::String (lastNanSum, 2) + ")") : juce::String())
                    + "  | FALHA-STREAM " + juce::String (streamMisses)
                    + (streamMisses > 0 ? ("  <<< CLIQUE (sem-janela " + juce::String (missReason[1])
                        + " / outra-nota " + juce::String (missReason[2])
                        + " / passou " + juce::String (missReason[3]) + ")") : juce::String())
                    + "  | vozes " + juce::String (activeVoiceCount()));
               #endif

                blocksSeen = 0;
                blocksLate = 0;
                lateWakes = 0;
                peakLoad = 0.0;
                peakGap = 0.0;
                peakOut = 0.0f;
                streamMisses = 0;
                nanGuardHits = 0;
                voicesTicks = 0; fxTicks = 0; sumBudget = 0.0;
                missReason[0] = missReason[1] = missReason[2] = missReason[3] = 0;
                sumLoad = 0.0;
            }
        }
       #endif

       #if JUCE_MAC
        if (selfTestOn)
        {
            const double used = juce::Time::highResolutionTicksToSeconds
                                    (juce::Time::getHighResolutionTicks() - blockStart);
            const double budget = (double) buffer.getNumSamples() / juce::jmax (1.0, hostSampleRate);
            float blockPeak = 0.0f;
            bool finite = true;

            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            {
                blockPeak = juce::jmax (blockPeak,
                                        buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
                const auto* samples = buffer.getReadPointer (ch);

                for (int s = 0; s < buffer.getNumSamples(); ++s)
                    if (! std::isfinite (samples[s]))
                    {
                        finite = false;
                        break;
                    }
            }

            ++macSelfTestBlocks;
            macSelfTestSeconds += budget;
            macSelfTestPeak = juce::jmax (macSelfTestPeak, blockPeak);
            macSelfTestMaxLoad = juce::jmax (macSelfTestMaxLoad,
                                             budget > 0.0 ? used / budget : 0.0);

            if (activeVoiceCount() > 0 && blockPeak > 0.00001f)
                ++macSelfTestAudibleBlocks;

            if (! finite)
                ++macSelfTestInvalidBlocks;

            if (macSelfTestSeconds >= 1.0)
            {
                juce::Logger::writeToLog ("MAC_SELFTEST: blocks "
                    + juce::String (macSelfTestBlocks)
                    + " audible " + juce::String (macSelfTestAudibleBlocks)
                    + " voices " + juce::String (activeVoiceCount())
                    + " peak " + juce::String (macSelfTestPeak, 6)
                    + " maxLoad " + juce::String (macSelfTestMaxLoad, 3)
                    + " invalid " + juce::String (macSelfTestInvalidBlocks));

                macSelfTestBlocks = 0;
                macSelfTestAudibleBlocks = 0;
                macSelfTestInvalidBlocks = 0;
                macSelfTestSeconds = 0.0;
                macSelfTestPeak = 0.0f;
                macSelfTestMaxLoad = 0.0;
            }
        }
       #endif
    }

   #if JUCE_ANDROID || JUCE_MAC
    int activeVoiceCount() const noexcept
    {
        int n = 0;

        for (const auto& v : voices)
            if (v.active)
                ++n;

        return n;
    }

   #if JUCE_ANDROID
    int blocksSeen = 0, blocksLate = 0, lateWakes = 0;
    double peakLoad = 0.0, sumLoad = 0.0, peakGap = 0.0;
    float peakOut = 0.0f;
    int streamMisses = 0;
    int missReason[4] { 0, 0, 0, 0 };
    juce::int64 voicesTicks = 0, fxTicks = 0;
    int nanGuardHits = 0;
    float lastNanMag = 0.0f, lastNanSum = 0.0f;
    double sumBudget = 0.0;
    juce::int64 lastBlockStart = 0;
   #endif

    //== TOCADOR AUTOMATICO (so para diagnostico) ==============================
    //
    // Gera acordes por conta propria para que a medicao possa ser feita sem
    // ninguem no teclado. Nao e capricho: sem isto, cada teste dependia de o
    // usuario estar com a mao no instrumento na janela exata da captura -- e a
    // medicao mais importante que fizemos saiu com "vozes 0" justamente por
    // isso.
    //
    // LIGA POR ARQUIVO, nunca por padrao. Num celular de comprador o marcador
    // nao existe e este codigo nunca roda.
    //
    // Toca como um tecladista de igreja tocaria: acordes de quatro notas,
    // sustentados, trocando a cada meio segundo -- e o padrao que constroi
    // polifonia de verdade e portanto o que estressa o motor.
    void selfTestTick (juce::MidiBuffer& midi, int numSamples)
    {
        if (! selfTestOn)
            return;

        selfTestElapsedSamples += numSamples;

       #if JUCE_MAC
        if (! selfTestQuitRequested
             && selfTestQuitSamples > 0
             && selfTestElapsedSamples >= selfTestQuitSamples)
        {
            selfTestQuitRequested = true;
            juce::MessageManager::callAsync ([]
            {
                juce::JUCEApplicationBase::quit();
            });
        }
       #endif

        // Neste modo as notas precisam vir da porta CoreMIDI criada pelo teste.
        // Gerar notas aqui mascararia exatamente a falha de hot-plug que queremos
        // detectar.
        if (selfTestExternalMidi)
            return;

        selfTestCounter += numSamples;

        if (selfTestStress)
        {
            const int period = (int) (hostSampleRate * 0.5);

            if (selfTestCounter < period)
                return;

            selfTestCounter -= period;

            for (int i = 0; i < 4; ++i)
                if (selfTestNotes[i] > 0)
                    midi.addEvent (juce::MidiMessage::noteOff (1, selfTestNotes[i]), 0);

            // Gerador deterministico: duas medicoes precisam receber a mesma
            // polifonia para a comparacao de carga/estalos ser honesta.
            selfTestSeed = selfTestSeed * 1664525u + 1013904223u;
            const int root = 48 + (int) ((selfTestSeed >> 16) % 24);
            static const int triad[4] = { 0, 4, 7, 12 };

            for (int i = 0; i < 4; ++i)
            {
                selfTestNotes[i] = juce::jlimit (21, 108, root + triad[i]);
                midi.addEvent (juce::MidiMessage::noteOn (
                    1, selfTestNotes[i],
                    (juce::uint8) (70 + (int) ((selfTestSeed >> (8 + i)) % 50))), 1);
            }

            return;
        }

        // Frase worship suave e pausada: Cmaj9, Gadd9/B, Am7 e Fadd9.
        // As vozes entram em tres gestos (grave, miolo e brilho), respiram e so
        // entao trocam de acorde. Alem de soar musical, isso testa ataque baixo,
        // cauda, release e streaming sem esconder clique atras de volume alto.
        const int period = (int) (hostSampleRate * 0.28);

        if (selfTestCounter < period)
            return;

        selfTestCounter -= period;

        static constexpr int chords[4][6] =
        {
            { 48, 55, 60, 64, 67, 74 },  // Cmaj9
            { 47, 55, 59, 62, 67, 69 },  // Gadd9/B
            { 45, 52, 57, 60, 64, 67 },  // Am7
            { 41, 48, 53, 57, 60, 67 }   // Fadd9
        };

        auto noteOn = [&] (int slot, int velocity)
        {
            selfTestNotes[slot] = chords[selfTestChord][slot];
            midi.addEvent (juce::MidiMessage::noteOn (
                1, selfTestNotes[slot], (juce::uint8) velocity), 1);
        };

        auto noteOff = [&] (int slot)
        {
            if (selfTestNotes[slot] > 0)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, selfTestNotes[slot]), 0);
                selfTestNotes[slot] = 0;
            }
        };

        switch (selfTestPhase)
        {
            case 0:
                for (int i = 0; i < 6; ++i)
                    noteOff (i);
                noteOn (0, 46);
                noteOn (1, 52);
                break;

            case 1:
                noteOn (2, 58);
                noteOn (3, 54);
                break;

            case 2:
                noteOn (4, 48);
                noteOn (5, 42);
                break;

            case 4:
                noteOff (5);   // o brilho sai primeiro, como a mao direita
                break;

            case 5:
                for (int i = 0; i < 6; ++i)
                    noteOff (i);
                break;

            default:
                break;         // duas etapas de respiracao entre acordes
        }

        if (++selfTestPhase >= 8)
        {
            selfTestPhase = 0;
            selfTestChord = (selfTestChord + 1) % 4;
        }
    }

    bool selfTestOn = false;
    bool selfTestStress = false;
    bool selfTestRestore = false;
    bool selfTestExternalMidi = false;
    bool selfTestQuitRequested = false;
    int selfTestCounter = 0;
    int selfTestNotes[8] { 0, 0, 0, 0, 0, 0, 0, 0 };
    int selfTestPhase = 0, selfTestChord = 0;
    unsigned int selfTestSeed = 12345u;
    juce::int64 selfTestElapsedSamples = 0;
    juce::int64 selfTestQuitSamples = 0;

   #if JUCE_MAC
    int macSelfTestBlocks = 0;
    int macSelfTestAudibleBlocks = 0;
    int macSelfTestInvalidBlocks = 0;
    double macSelfTestSeconds = 0.0;
    double macSelfTestMaxLoad = 0.0;
    float macSelfTestPeak = 0.0f;
   #endif
   #endif


    double getTailLengthSeconds() const override
    {
        return getReverbSlot() >= 0 ? 30.0 : worshipReverb.getTailLengthSeconds();
    }
    bool acceptsMidi() const override                           { return true; }
    bool producesMidi() const override                          { return false; }
    bool hasEditor() const override                             { return true; }
    juce::AudioProcessorEditor* createEditor() override;        // definido apos o editor
    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    bool consumeLastStateCaptureHadFailure() noexcept
    {
        const juce::ScopedLock captureLock (
            layerToneTransactionLock);
        return lastStateCaptureHadFailure.exchange (
            false, std::memory_order_acq_rel);
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        const juce::ScopedLock toneLock (layerToneTransactionLock);
        lastStateCaptureHadFailure.store (
            false, std::memory_order_release);
        bool stateCaptureHadFailure = false;

        // Plugins nao garantem que getStateInformation seja seguro enquanto
        // processBlock roda. Salvar e raro; uma pausa curta e preferivel a
        // corromper estado ou derrubar AU/VST no macOS/Windows.
        const bool resumeAfterSave = ! isSuspended();

        if (resumeAfterSave)
            suspendProcessing (true);
        const juce::ScopeGuard restoreProcessing { [this, resumeAfterSave]
        {
            if (resumeAfterSave)
                suspendProcessing (false);
        } };

        // Atualiza inclusive o timbre que esta tocando agora; o cliente nao
        // precisa trocar de preset antes de fechar para a regulagem ser salva.
        for (int l = 0; l < numLayers; ++l)
            rememberOutgoingTimbreTone (l);

        juce::XmlElement xml ("TIMBRES2");
        xml.setAttribute ("toneSchema", 3);
        // Versoes antigas ligavam por engano o encoder 8 do Launchkey ao
        // transpose global e depois persistiam o valor. Este marcador distingue
        // um transpose salvo conscientemente pela versao corrigida.
        xml.setAttribute ("transposeSchema", 1);
        xml.setAttribute ("root", rootFolder.getFullPathName());
        xml.setAttribute ("gain", (double) *gain);
        xml.setAttribute ("gtransp", (int) *globalTransp);
        xml.setAttribute ("gsus", (bool) *globalSustain);
        xml.setAttribute ("pedinv", (bool) *pedalInvert);
        xml.setAttribute ("velcurve", velCurve->getIndex());
        xml.setAttribute ("rvbpreset", nativeReverbPreset->getIndex());
        const float savedReverbReturn = (float) *reverbReturn;
        xml.setAttribute (
            "rvbreturn",
            (double) (std::isfinite (savedReverbReturn)
                        ? juce::jlimit (0.0f, 1.0f, savedReverbReturn)
                        : 0.72f));

        // Banco pequeno e limitado por canal+timbre. Os elementos TONE sao
        // ignorados por builds antigos, portanto o arquivo continua retroativo.
        timbreToneMemory.writeToXml (xml);

        {
            const juce::ScopedLock callbackGuard (
                getCallbackLock());

            for (int i = 0; i < numInserts; ++i)
            if (pendingInsertIds[i].isNotEmpty())
            {
                // Durante o two-phase o plugin antigo ainda toca, mas a intenção
                // persistente já é o novo alvo. Assim fechar no meio da criação
                // não troca silenciosamente o projeto de volta para o antigo.
                auto* insXml = xml.createNewChildElement ("INSERT");
                insXml->setAttribute ("slot", i);
                insXml->setAttribute ("id", pendingInsertIds[i]);
                insXml->setAttribute ("state", pendingInsertStates[i]);
            }
            else if (inserts[i] != nullptr)
            {
                auto* insXml = xml.createNewChildElement ("INSERT");
                insXml->setAttribute ("slot", i);
                insXml->setAttribute ("id", inserts[i]->getPluginDescription().createIdentifierString());

                juce::MemoryBlock mb;
                try
                {
                    inserts[i]->getStateInformation (mb);
                }
                catch (...)
                {
                    mb.reset();
                    stateCaptureHadFailure = true;
                }
                insXml->setAttribute ("state", mb.toBase64Encoding());
            }
        }

        for (int l = 0; l < numLayers; ++l)
        {
            auto* layerXml = xml.createNewChildElement ("LAYER");
            layerXml->setAttribute ("index", l);

            // salvar durante o boot: o timbre ainda nao chegou em layers[l] --
            // cai pro rotulo pendente (esperando o scan) ou pro em voo
            // (decodificando); senao fechar o app na splash perdia o setup
            juce::String timbre, lib, restoreVstId, restoreVstState;
            juce::String pendingVstId, pendingVstState;
            bool restorePending = false;
            {
                // Strings JUCE compartilham heap. O snapshot atomico impede
                // autosave de observar pending=true com o alvo ja apagado.
                const juce::ScopedLock lock (layerIdentityLock);
                timbre = layers[l].timbre;
                lib = layers[l].libraryName;
                restorePending = layerStateRestorePending[l].load (
                    std::memory_order_acquire);

                if (restorePending)
                {
                    // Parametros ja pertencem ao novo projeto; enquanto o
                    // decode termina, salve sempre sob a identidade-alvo.
                    timbre = layerStateRestoreTargetTimbre[l];
                    lib = layerStateRestoreTargetLibrary[l];
                    restoreVstId = layerStateRestoreTargetVstId[l];
                    restoreVstState = layerStateRestoreTargetVstState[l];
                }
                else if (timbre.isEmpty()
                          && pendingLayerLabels[l].isNotEmpty())
                {
                    timbre = pendingLayerLabels[l];
                    lib = pendingLayerLibs[l];
                }
                else if (timbre.isEmpty()
                          && loadingLabels[l].isNotEmpty())
                {
                    timbre = loadingLabels[l];
                    lib = loadingLibs[l];
                }

                pendingVstId = pendingVstIds[l];
                pendingVstState = pendingVstStates[l];
            }

            juce::String livePluginId;
            juce::MemoryBlock livePluginState;
            bool hasLivePlugin = false;
            juce::MemoryBlock liveChurchOrganState;
            const bool hasLiveChurchOrgan =
                ! restorePending
                && layerIsChurchOrgan[l].load (
                    std::memory_order_acquire);

            if (! restorePending && pendingVstId.isEmpty())
            {
                // Mantem a instancia viva enquanto captura o blob, mas nunca
                // segura o lock leve de Strings ao executar codigo de terceiro.
                const juce::ScopedLock pluginLock (layerPluginStateLock);

                if (layers[l].instrument != nullptr)
                {
                    {
                        const juce::ScopedLock identityLock (
                            layerIdentityLock);
                        livePluginId = layers[l].instrumentId;
                    }

                    hasLivePlugin = true;
                    try
                    {
                        layers[l].instrument->getStateInformation (
                            livePluginState);
                    }
                    catch (...)
                    {
                        livePluginState.reset();
                        stateCaptureHadFailure = true;
                    }
                }
            }

            if (hasLiveChurchOrgan)
            {
                if (auto* organ =
                        layers[l].churchOrgan.get())
                {
                    const bool wasSuspended =
                        organ->isProcessingSuspended();
                    organ->setProcessingSuspended (true);

                    // Se um restore ainda não foi aceito, preserve exatamente
                    // o blob original. Serializar o fallback preparado aqui
                    // destruiria a auto-cura no próximo start/save.
                    if (pendingChurchOrganStates[l].getSize() > 0)
                        liveChurchOrganState =
                            pendingChurchOrganStates[l];
                    else if (organ->diagnostics().prepared)
                        organ->getStateInformation (
                            liveChurchOrganState);

                    organ->setProcessingSuspended (
                        wasSuspended);
                }
            }

            layerXml->setAttribute ("timbre", timbre);
            layerXml->setAttribute ("lib", lib);
            layerXml->setAttribute ("vol", (double) *layers[l].vol);
            layerXml->setAttribute ("transp", (int) *layers[l].transp);
            layerXml->setAttribute ("loop", (bool) *layers[l].loop);
            layerXml->setAttribute ("mute", (bool) *layers[l].mute);
            // SOLO e monitoracao momentanea de ensaio, nao parte do setup.
            // Persisti-lo fazia um canal antigo/oculto voltar em solo no boot e
            // todos os demais so soavam depois de tambem apertar SOLO.
            layerXml->setAttribute ("solo", false);
            layerXml->setAttribute ("on", (bool) *layers[l].on);
            layerXml->setAttribute ("comp", (double) *layers[l].comp);
            layerXml->setAttribute ("dly", (double) *layers[l].delay);
            layerXml->setAttribute ("attack", (double) *layers[l].attack);
            layerXml->setAttribute ("release", (double) *layers[l].release);
            // Filtros historicos continuam separados do novo EQ para que o
            // som de setups anteriores permaneça bit-a-bit equivalente.
            layerXml->setAttribute ("lcut", (double) *layers[l].lowcut);
            layerXml->setAttribute ("hcut", (double) *layers[l].highcut);
            layerXml->setAttribute ("eqLow", (double) *layers[l].eqLow);
            layerXml->setAttribute ("eqMid", (double) *layers[l].eqMid);
            layerXml->setAttribute ("eqHigh", (double) *layers[l].eqHigh);
            layerXml->setAttribute ("aeq", (bool) *layers[l].autoEq);
            layerXml->setAttribute ("rvb", (double) *layers[l].reverbSend);
            layerXml->setAttribute ("rotMode", layers[l].rotaryMode->getIndex());
            const float rotaryAmount = (float) *layers[l].rotaryAmount;
            layerXml->setAttribute (
                "rotAmt",
                (double) (std::isfinite (rotaryAmount)
                            ? juce::jlimit (0.0f, 1.0f, rotaryAmount)
                            : 0.0f));
            layerXml->setAttribute ("cor", layerColourIdx[l]);
            {
                const juce::ScopedLock sl (midiRouteLock);
                if (layerMidiDeviceId[l].isNotEmpty())
                    layerXml->setAttribute ("midiDev", layerMidiDeviceId[l]);
                const int mch = layerMidiListenChannel[l].load (
                    std::memory_order_relaxed);
                if (mch > 0)
                    layerXml->setAttribute ("midiCh", mch);
            }

            if (hasLiveChurchOrgan)
            {
                layerXml->setAttribute (
                    "churchOrgan", true);
                layerXml->setAttribute (
                    "organFamily",
                    static_cast<int> (
                        getLayerChurchOrganFamily (l)));
                layerXml->setAttribute (
                    "organState",
                    liveChurchOrganState.toBase64Encoding());
            }

            // VSTi externo (Kontakt) hospedado no canal
            if (hasLiveChurchOrgan)
            {
                // Church Organ é a fonte do canal; VST/sample antigos não
                // vazam para o mesmo estado.
            }
            else if (restorePending)
            {
                // Se o plugin alvo esta ausente, preserve o pedido pendente.
                // Alvo nativo deixa estes campos vazios, portanto um VST/AU
                // antigo que ainda produz audio durante o decode nao vaza.
                if (restoreVstId.isNotEmpty())
                {
                    layerXml->setAttribute ("vstId", restoreVstId);
                    layerXml->setAttribute ("vstState", restoreVstState);
                }
            }
            else if (pendingVstId.isNotEmpty())
            {
                // Escolha AU/AUv3 aceita, mas ainda em criação: a intenção nova
                // vence o instrumento antigo que continua audível durante o
                // two-phase. Falha interativa limpa este pending e o próximo
                // save volta naturalmente ao plugin antigo.
                layerXml->setAttribute ("vstId", pendingVstId);
                layerXml->setAttribute ("vstState", pendingVstState);
            }
            else if (hasLivePlugin)
            {
                layerXml->setAttribute ("vstId", livePluginId);
                layerXml->setAttribute (
                    "vstState", livePluginState.toBase64Encoding());
            }
        }

        copyXmlToBinary (xml, dest);
        lastStateCaptureHadFailure.store (
            stateCaptureHadFailure, std::memory_order_release);
    }

    void setStateInformation (const void* data, int size) override
    {
        auto xml = getXmlFromBinary (data, size);

        if (xml == nullptr)
            return;

        const bool oldestState = xml->hasTagName ("TIMBRES");
        const bool currentState = xml->hasTagName ("TIMBRES2");

        // Um blob valido de outro processador nao deve apagar apenas o banco de
        // timbres e deixar o restante tocando. Valide a raiz antes de iniciar
        // qualquer parte da transacao.
        if (! oldestState && ! currentState)
            return;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        programmaticToneChangeDepth.fetch_add (
            1, std::memory_order_acq_rel);
        const juce::ScopeGuard finishProgrammaticChange { [this]
        {
            programmaticToneChangeDepth.fetch_sub (
                1, std::memory_order_acq_rel);
        } };

        const juce::XmlElement* restoreLayerXml[numLayers] {};
        const juce::XmlElement* restoreInsertXml[numInserts] {};

        // Descobre TODOS os alvos antes de mudar banco/parametros/inserts. Se
        // um save reentrar durante a restauracao, ele ja enxerga o novo projeto
        // completo (ou canal vazio), nunca identidades do projeto anterior.
        if (currentState)
            for (const auto* child : xml->getChildIterator())
            {
                if (child->hasTagName ("LAYER"))
                {
                    const int layer =
                        child->getIntAttribute ("index", -1);

                    if (juce::isPositiveAndBelow (layer, numLayers)
                         && restoreLayerXml[layer] == nullptr)
                        restoreLayerXml[layer] = child;
                }
                else if (child->hasTagName ("INSERT"))
                {
                    const int slot =
                        child->getIntAttribute ("slot", -1);

                    if (juce::isPositiveAndBelow (slot, numInserts)
                         && restoreInsertXml[slot] == nullptr)
                        restoreInsertXml[slot] = child;
                }
            }

        {
            // Arma identidades e alvos numa unica secao critica. Um autosave
            // concorrente enxerga o projeto anterior inteiro ou o novo alvo
            // inteiro, jamais pending=true acompanhado por Strings vazias.
            const juce::ScopedLock lock (layerIdentityLock);

            for (int l = 0; l < numLayers; ++l)
            {
                layerStateRestorePending[l].store (
                    true, std::memory_order_release);
                pendingLayerLabels[l].clear();
                pendingLayerLibs[l].clear();
                loadingLabels[l].clear();
                loadingLibs[l].clear();

                layerStateRestoreTargetTimbre[l].clear();
                layerStateRestoreTargetLibrary[l].clear();
                layerStateRestoreTargetVstId[l].clear();
                layerStateRestoreTargetVstState[l].clear();

                if (const auto* layerXml = restoreLayerXml[l])
                {
                    layerStateRestoreTargetTimbre[l] =
                        layerXml->getStringAttribute ("timbre");
                    layerStateRestoreTargetLibrary[l] =
                        layerXml->getStringAttribute ("lib");
                    layerStateRestoreTargetVstId[l] =
                        layerXml->getStringAttribute ("vstId");
                    layerStateRestoreTargetVstState[l] =
                        layerXml->getStringAttribute ("vstState");
                }
            }

            if (oldestState)
                layerStateRestoreTargetTimbre[0] =
                    xml->getStringAttribute ("timbre");
        }

        for (int l = 0; l < numLayers; ++l)
        {
            cancelLayerLoad (l);
            ++layerLoadGeneration[l];
            instrumentPluginLoadGeneration[l].cancel();
            instrumentPluginLoadInFlight[l].store (
                false, std::memory_order_release);
        }

        for (int slot = 0; slot < numInserts; ++slot)
        {
            insertPluginLoadGeneration[slot].cancel();
            insertPluginLoadInFlight[slot].store (
                false, std::memory_order_release);
            insertStateRestorePending[slot].store (
                false, std::memory_order_release);
        }

        // Restore e substitutivo tambem nos MASTER FX. A ausencia de <INSERT>
        // representa slot vazio, nao autorizacao para herdar o efeito do projeto
        // anterior (inclusive no formato legado TIMBRES).
        for (int slot = 0; slot < numInserts; ++slot)
            if (restoreInsertXml[slot] == nullptr)
                unloadInsert (slot);

        // Carregar outro projeto substitui o banco anterior por completo.
        // Estados antigos nao possuem TONE e simplesmente comecam vazios.
        timbreToneMemory.clear();

        // A restauracao e substitutiva: uma camada ausente ou um projeto antigo
        // de um unico canal nao pode herdar mute, ENV, EQ ou FX do projeto que
        // estava aberto. As camadas presentes sobrescrevem estes defaults logo
        // abaixo; as ausentes permanecem realmente vazias e neutras.
        for (int l = 0; l < numLayers; ++l)
            resetLayerParametersToDefaults (l);

        const juce::File root (xml->getStringAttribute ("root", defaultSamplesFolder().getFullPathName()));

        if (root.isDirectory()
             && root != rootFolder)
        {
            rootFolder = root;
            entriesScanned = false;
            // Trocar a raiz durante um restore invalida o índice atual, mas o
            // pedido precisa também iniciar a nova varredura. O código anterior
            // deixava pendingLayerLabels aguardando para sempre. Quando a raiz
            // é a mesma, preserve o catálogo já validado e carregue de imediato.
            startEntriesScanAsync (true);
        }

        // formato antigo (um unico timbre) vira o canal 1
        if (oldestState)
        {
            *layers[0].attack = 0.003f;
            *layers[0].release = 0.55f;
            *layers[0].eqLow = 0.0f;
            *layers[0].eqMid = 0.0f;
            *layers[0].eqHigh = 0.0f;
            *layers[0].rotaryMode = ChurchDAWRotary::Speaker::stopMode;
            *layers[0].rotaryAmount = 0.0f;
            *layers[0].transp = xml->getIntAttribute ("transpose", 0);
            *layers[0].vol = (float) xml->getDoubleAttribute ("gain", 1.0);
            *layers[0].loop = xml->getBoolAttribute ("loop", false);

            const auto target =
                xml->getStringAttribute ("timbre");

            if (target.isNotEmpty())
                requestLayerLoad (0, target);
            else
            {
                clearLayer (0);

                if (layers[0].instrument != nullptr)
                    clearInstrumentPlugin (0);
            }

            // O formato de um canal representa os demais como vazios.
            for (int l = 1; l < numLayers; ++l)
            {
                clearLayer (l);

                if (layers[l].instrument != nullptr)
                    clearInstrumentPlugin (l);
            }
            return;
        }

        timbreToneMemory.readFromXml (*xml);

        *gain = (float) xml->getDoubleAttribute ("gain", 1.0);
        const int savedGlobalTranspose = juce::jlimit (
            -12, 12, xml->getIntAttribute ("gtransp", 0));
        const bool trustedTransposeState =
            xml->getIntAttribute ("transposeSchema", 0) >= 1;
        *globalTransp = trustedTransposeState
                            ? savedGlobalTranspose
                            : 0;

        if (! trustedTransposeState && savedGlobalTranspose != 0)
            juce::Logger::writeToLog (
                "MIDI SAFETY: transpose global legado foi recentralizado; "
                "o encoder 8 de builds antigas podia altera-lo sem querer");
        *globalSustain = xml->getBoolAttribute ("gsus", false);
        *pedalInvert = xml->getBoolAttribute ("pedinv", false);
        *velCurve = xml->getIntAttribute ("velcurve", curveNatural);
        *nativeReverbPreset = juce::jlimit (
            0, numReverbMachines - 1, xml->getIntAttribute ("rvbpreset", 1));
        const double restoredReverbReturn =
            xml->getDoubleAttribute ("rvbreturn", 0.72);
        *reverbReturn = std::isfinite (restoredReverbReturn)
                      ? (float) juce::jlimit (
                            0.0, 1.0, restoredReverbReturn)
                      : 0.72f;
        worshipReverb.setPreset (nativeReverbPreset->getIndex());

        // estado novo substitui qualquer pendencia de restauracao anterior
        {
            const juce::ScopedLock callbackGuard (
                getCallbackLock());

            for (int i = 0; i < numInserts; ++i)
            {
                pendingInsertIds[i].clear();
                pendingInsertStates[i].clear();
                pendingInsertDescriptionValid[i] = false;
            }
        }

        {
            const juce::ScopedLock lock (layerIdentityLock);

            for (int l = 0; l < numLayers; ++l)
            {
                pendingVstIds[l].clear();
                pendingVstStates[l].clear();
                pendingVstDescriptionValid[l] = false;
            }
        }

        for (int slot = 0; slot < numInserts; ++slot)
        {
            const auto* insXml = restoreInsertXml[slot];

            if (insXml == nullptr)
                continue;

            insertStateRestorePending[slot].store (
                true, std::memory_order_release);

            const auto identifier =
                insXml->getStringAttribute ("id");
            const auto encodedState =
                insXml->getStringAttribute ("state");

            // O alvo fica serializável durante toda a criação assíncrona. O
            // commit bem-sucedido limpa estes campos; ausência, base64 ruim ou
            // falha do vendor preserva a auto-cura para a próxima abertura.
            {
                const juce::ScopedLock callbackGuard (
                    getCallbackLock());
                pendingInsertIds[slot] = identifier;
                pendingInsertStates[slot] = encodedState;
                pendingInsertDescriptionValid[slot] = false;
            }

            juce::MemoryBlock decodedState;
            const bool decoded = encodedState.isEmpty()
                || decodedState.fromBase64Encoding (encodedState);
            std::unique_ptr<juce::PluginDescription> description;

            if (knownPluginsRef != nullptr)
                description = knownPluginsRef
                    ->getTypeForIdentifierString (identifier);

            const bool accepted = decoded
                && description != nullptr
                && ! isHostedPluginBlocked (*description)
                && loadInsert (
                    slot, *description,
                    decodedState.getSize() > 0
                        ? &decodedState : nullptr,
                    {}, true);

            if (! accepted)
            {
                // Restore e substitutivo mesmo se o ID sumiu, o state esta
                // corrompido ou o scanner bloqueou o plugin: nunca deixe o
                // efeito da pagina anterior tocando sob a nova identidade.
                unloadInsert (slot);

                const juce::ScopedLock callbackGuard (
                    getCallbackLock());
                pendingInsertIds[slot] = identifier;
                pendingInsertStates[slot] = encodedState;
                pendingInsertDescriptionValid[slot] =
                    description != nullptr;

                if (description != nullptr)
                    pendingInsertDescriptions[slot] = *description;

                insertPluginLoadInFlight[slot].store (
                    false, std::memory_order_release);
                insertStateRestorePending[slot].store (
                    true, std::memory_order_release);
            }
        }

        for (int l = 0; l < numLayers; ++l)
        {
            const auto* layerXml = restoreLayerXml[l];

            if (layerXml == nullptr)
            {
                // Estado incompleto/adversarial: camada ausente equivale a
                // vazia, jamais a "continue tocando o projeto anterior".
                clearLayer (l);

                if (layers[l].instrument != nullptr)
                    clearInstrumentPlugin (l);
                continue;
            }

            const auto safeAttribute = [layerXml] (const char* name,
                                                   double fallback,
                                                   double minimum,
                                                   double maximum)
            {
                const double value =
                    layerXml->getDoubleAttribute (name, fallback);
                return std::isfinite (value)
                         ? juce::jlimit (minimum, maximum, value)
                         : fallback;
            };

            *layers[l].vol = (float) layerXml->getDoubleAttribute ("vol", 1.0);
            *layers[l].transp = trustedTransposeState
                                    ? juce::jlimit (
                                          -24, 24,
                                          layerXml->getIntAttribute (
                                              "transp", 0))
                                    : 0;
            *layers[l].loop = layerXml->getBoolAttribute ("loop", false);
            *layers[l].mute = layerXml->getBoolAttribute ("mute", false);
            // Estados antigos podem conter solo=true. Ignore sempre: abrir um
            // projeto nunca deve nascer silenciando os outros canais por uma
            // monitoracao temporaria da sessao anterior.
            *layers[l].solo = false;
            *layers[l].on = layerXml->getBoolAttribute ("on", true);
            *layers[l].comp = (float) layerXml->getDoubleAttribute ("comp", 0.0);
            *layers[l].delay = (float) layerXml->getDoubleAttribute ("dly", 0.0);
            *layers[l].attack = (float) safeAttribute (
                "attack", 0.003, 0.001, 2.0);
            *layers[l].release = (float) safeAttribute (
                "release", 0.55, 0.05, 12.0);
            // lcut/hcut sao os filtros 0..1 da versao ja instalada. Nao os
            // reinterpretar como dB: isso mudaria o som salvo do cliente.
            *layers[l].lowcut = (float) safeAttribute (
                "lcut", 0.0, 0.0, 1.0);
            *layers[l].highcut = (float) safeAttribute (
                "hcut", 0.0, 0.0, 1.0);
            *layers[l].eqLow = (float) safeAttribute (
                "eqLow", 0.0, -12.0, 12.0);
            *layers[l].eqMid = (float) (
                layerXml->hasAttribute ("eqMid")
                    ? safeAttribute ("eqMid", 0.0, -12.0, 12.0)
                    : safeAttribute ("mid", 0.0, -12.0, 12.0));
            *layers[l].eqHigh = (float) safeAttribute (
                "eqHigh", 0.0, -12.0, 12.0);
            *layers[l].autoEq = layerXml->getBoolAttribute ("aeq", false);
            *layers[l].reverbSend = (float) layerXml->getDoubleAttribute ("rvb", 0.0);
            *layers[l].rotaryMode = juce::jlimit (
                ChurchDAWRotary::Speaker::slowMode,
                ChurchDAWRotary::Speaker::fastMode,
                layerXml->getIntAttribute (
                    "rotMode", ChurchDAWRotary::Speaker::stopMode));
            // Estado anterior ao Rotary: Amount 0 garante bypass integral.
            *layers[l].rotaryAmount = (float) safeAttribute (
                "rotAmt", 0.0, 0.0, 1.0);
            layerColourIdx[l] = layerXml->getIntAttribute ("cor", -1);   // -1 = automatico
            {
                const juce::ScopedLock sl (midiRouteLock);
                layerMidiDeviceId[l] =
                    layerXml->getStringAttribute ("midiDev");
                const int mch = juce::jlimit (
                    0, 16, layerXml->getIntAttribute ("midiCh", 0));
                // Se salvou device sem canal, reatribui no fim do restore.
                layerMidiListenChannel[l].store (
                    mch, std::memory_order_relaxed);
            }

            if (layerXml->getBoolAttribute (
                    "churchOrgan", false))
            {
                const auto family =
                    static_cast<churchorgan::ChurchOrganFamily> (
                        juce::jlimit (
                            0, 3,
                            layerXml->getIntAttribute (
                                "organFamily", 0)));
                juce::MemoryBlock decodedOrganState;
                const auto encodedOrganState =
                    layerXml->getStringAttribute (
                        "organState");
                const bool decoded =
                    encodedOrganState.isEmpty()
                    || decodedOrganState.fromBase64Encoding (
                        encodedOrganState);

                if (loadChurchOrganIntoLayer (
                        l, family,
                        decoded
                            && decodedOrganState.getSize() > 0
                            ? &decodedOrganState
                            : nullptr,
                        false))
                    completeLayerStateRestore (l);

                continue;
            }

            // VSTi externo (Kontakt) do canal, ou timbre nativo
            const auto vstId = layerXml->getStringAttribute ("vstId");

            if (vstId.isNotEmpty())
            {
                const auto savedVstState =
                    layerXml->getStringAttribute ("vstState");
                juce::MemoryBlock decodedVstState;
                const bool decoded =
                    savedVstState.isEmpty()
                    || decodedVstState.fromBase64Encoding (savedVstState);

                std::unique_ptr<juce::PluginDescription> description;

                if (knownPluginsRef != nullptr)
                    description = knownPluginsRef
                        ->getTypeForIdentifierString (vstId);

                const bool accepted = decoded
                    && description != nullptr
                    && ! isHostedPluginBlocked (*description)
                    && loadInstrumentPlugin (
                        l, *description, false,
                        decodedVstState.getSize() > 0
                            ? &decodedVstState : nullptr);

                if (! accepted)
                {
                    // Um alvo ausente/bloqueado continua serializavel para
                    // autocura, mas o audio e sempre o da pagina nova: vazio.
                    clearLayer (l);

                    if (layers[l].instrument != nullptr)
                        clearInstrumentPlugin (l);

                    const juce::ScopedLock lock (layerIdentityLock);
                    pendingVstIds[l] = vstId;
                    pendingVstStates[l] = savedVstState;
                    pendingVstDescriptionValid[l] =
                        description != nullptr;

                    if (description != nullptr)
                        pendingVstDescriptions[l] = *description;

                    instrumentPluginLoadInFlight[l].store (
                        false, std::memory_order_release);
                    layerStateRestoreTargetTimbre[l].clear();
                    layerStateRestoreTargetLibrary[l].clear();
                    layerStateRestoreTargetVstId[l] = vstId;
                    layerStateRestoreTargetVstState[l] = savedVstState;
                    layerStateRestorePending[l].store (
                        true, std::memory_order_release);
                }
            }
            else
            {
                const auto targetTimbre =
                    layerXml->getStringAttribute ("timbre");

                if (targetTimbre.isNotEmpty())
                    requestLayerLoad (
                        l, targetTimbre,
                        layerXml->getStringAttribute ("lib"));
                else
                {
                    // LAYER vazio no projeto novo significa canal vazio. A
                    // versao anterior deixava o timbre/plugin do projeto velho
                    // tocando e depois o salvava como se pertencesse ao novo.
                    clearLayer (l);

                    if (layers[l].instrument != nullptr)
                        clearInstrumentPlugin (l);
                }
            }
        }

        // Restaura mapa teclado→canal (2 teclados: piano/pad). Se salvou
        // device sem canal, reatribui canais estáveis.
        {
            const juce::ScopedLock sl (midiRouteLock);
            for (int l = 0; l < numLayers; ++l)
            {
                if (layerMidiDeviceId[l].isEmpty())
                    continue;
                if (layerMidiListenChannel[l].load (
                        std::memory_order_relaxed) > 0)
                    continue;

                // Reusa a mesma lógica do setLayerMidiInputDevice.
                int ch = 0;
                for (int i = 0; i < numLayers; ++i)
                    if (i != l && layerMidiDeviceId[i] == layerMidiDeviceId[l])
                    {
                        ch = layerMidiListenChannel[i].load (
                            std::memory_order_relaxed);
                        if (ch > 0)
                            break;
                    }
                if (ch <= 0)
                {
                    bool used[17] = {};
                    for (int i = 0; i < numLayers; ++i)
                    {
                        const int c = layerMidiListenChannel[i].load (
                            std::memory_order_relaxed);
                        if (c >= 1 && c <= 16)
                            used[c] = true;
                    }
                    ch = juce::jlimit (1, 16, l + 1);
                    if (used[ch])
                    {
                        ch = 1;
                        while (ch <= 16 && used[ch])
                            ++ch;
                        if (ch > 16)
                            ch = (l % 16) + 1;
                    }
                }
                layerMidiListenChannel[l].store (
                    ch, std::memory_order_relaxed);
            }
            rebuildMidiDeviceForceMap();
        }
    }

private:
    bool isHostedPluginEditorTeardownActive() const noexcept
    {
        return hostedPluginEditorTeardownDepth.load (
                   std::memory_order_acquire) > 0;
    }

    void deferHostedPluginFinish (
        HostedPluginLocation location,
        int index,
        std::function<void()> finish)
    {
        const juce::ScopedLock lock (
            hostedPluginDeferredFinishLock);

        if (location == HostedPluginLocation::insert)
        {
            if (juce::isPositiveAndBelow (index, numInserts))
                deferredInsertPluginFinishes[index] =
                    std::move (finish);
        }
        else if (juce::isPositiveAndBelow (index, numLayers))
        {
            deferredInstrumentPluginFinishes[index] =
                std::move (finish);
        }
    }

    void postDeferredHostedPluginFinishes() noexcept
    {
        std::function<void()> insertFinishes[numInserts];
        std::function<void()> instrumentFinishes[numLayers];

        {
            const juce::ScopedLock lock (
                hostedPluginDeferredFinishLock);

            for (int slot = 0; slot < numInserts; ++slot)
                insertFinishes[slot].swap (
                    deferredInsertPluginFinishes[slot]);

            for (int layer = 0; layer < numLayers; ++layer)
                instrumentFinishes[layer].swap (
                    deferredInstrumentPluginFinishes[layer]);
        }

        const auto postOnce = [] (
            std::function<void()> finish) noexcept
        {
            if (! finish)
                return;

            std::function<void()> fallback;

            try
            {
                fallback = finish;

                if (juce::MessageManager::callAsync (
                        std::move (finish)))
                    return;
            }
            catch (...)
            {
            }

            // Se o message loop ja estiver fechando, depth e zero e executar
            // aqui e mais seguro que perder ownership/completion para sempre.
            try
            {
                if (fallback)
                    fallback();
                else if (finish)
                    finish();
            }
            catch (...)
            {
            }
        };

        for (auto& finish : insertFinishes)
            postOnce (std::move (finish));

        for (auto& finish : instrumentFinishes)
            postOnce (std::move (finish));
    }

    void notifyHostedPluginWillRetire (
        HostedPluginLocation location,
        int index,
        juce::AudioPluginInstance* instance) noexcept
    {
        if (instance == nullptr)
            return;

        HostedPluginWillRetireCallback callback;

        try
        {
            const juce::ScopedLock lock (
                hostedPluginEditorCallbackLock);
            callback = hostedPluginWillRetire;
        }
        catch (...)
        {
            return;
        }

        if (callback == nullptr)
            return;

        try
        {
            callback (location, index, instance);
        }
        catch (...)
        {
            // UI de plugin nunca pode impedir a troca/limpeza do grafo.
        }
    }

    bool finishFailedInsertStateRestore (
        int slot,
        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts)
             || ! insertPluginLoadGeneration[slot].isCurrent (ticket))
            return false;

        // O editor guarda uma referencia ao plugin antigo. Feche-o antes de
        // retirar a instancia, mas revalide o ticket depois: o loop modal do
        // vendor pode iniciar um pedido mais novo, que sempre deve vencer.
        notifyHostedPluginWillRetire (
            HostedPluginLocation::insert, slot,
            getInsert (slot));

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        bool committed = false;
        {
            const bool resumeAfterClear = ! isSuspended();

            if (resumeAfterClear)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterClear]
            {
                if (resumeAfterClear)
                    suspendProcessing (false);
            } };

            const juce::ScopedLock callbackGuard (
                getCallbackLock());
            committed = insertPluginLoadGeneration[slot]
                .commitIfCurrent (ticket, [this, slot, &old,
                                           &oldWasPrepared]
                {
                    oldWasPrepared = insertPrepared[slot].exchange (
                        false, std::memory_order_acq_rel);
                    old = std::move (inserts[slot]);
                    insertPluginLoadInFlight[slot].store (
                        false, std::memory_order_release);
                    insertStateRestorePending[slot].store (
                        true, std::memory_order_release);
                    // ID/estado/descricao permanecem como intencao de
                    // autocura. O pending fica armado para a cena detectar a
                    // falha e executar timeout/rollback, sem prender a splash.
                });
        }

        if (! committed)
            return false;

        try
        {
            refreshExternalReverbSlot (slot);
            forceExternalReverbWet (getReverbSlot());
        }
        catch (...)
        {
        }

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        old.reset();
        sendChangeMessage();
        return true;
    }

    bool finishFailedInstrumentStateRestore (
        int layerIndex,
        ChurchDAWPluginLoading::RequestGeneration::Ticket ticket)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers)
             || ! instrumentPluginLoadGeneration[layerIndex]
                    .isCurrent (ticket))
            return false;

        // A pagina pediu uma fonte que terminou em falha. Nao deixe o timbre
        // da cena anterior tocando sob a identidade nova. A limpeza nativa vem
        // antes do callback de editor, para um pedido reentrante iniciado ao
        // fechar a janela nao ser cancelado por este resultado antigo.
        clearLayer (layerIndex, true);

        if (! instrumentPluginLoadGeneration[layerIndex]
                .isCurrent (ticket))
            return false;

        notifyHostedPluginWillRetire (
            HostedPluginLocation::instrument, layerIndex,
            getLayerInstrument (layerIndex));

        std::unique_ptr<juce::AudioPluginInstance> old;
        bool oldWasPrepared = false;
        bool committed = false;
        {
            const bool resumeAfterClear = ! isSuspended();

            if (resumeAfterClear)
                suspendProcessing (true);
            const juce::ScopeGuard restoreProcessing {
                [this, resumeAfterClear]
            {
                if (resumeAfterClear)
                    suspendProcessing (false);
            } };

            const juce::ScopedLock pluginLock (
                layerPluginStateLock);
            const juce::ScopedLock identityLock (
                layerIdentityLock);
            committed = instrumentPluginLoadGeneration[layerIndex]
                .commitIfCurrent (
                    ticket,
                    [this, layerIndex, &old, &oldWasPrepared]
                    {
                        oldWasPrepared =
                            pluginPrepared[layerIndex].exchange (
                                false, std::memory_order_acq_rel);
                        old = std::move (
                            layers[layerIndex].instrument);
                        instNotesOn[layerIndex] = 0;
                        noteTranspose.clearLayer (layerIndex);
                        pluginSustainMirror[layerIndex].store (
                            sustainStateUnknown,
                            std::memory_order_release);
                        pluginPitchCentresPending[layerIndex].store (
                            false, std::memory_order_release);
                        layers[layerIndex].instrumentId.clear();
                        layers[layerIndex].instrumentName.clear();
                        instrumentPluginLoadInFlight[layerIndex].store (
                            false, std::memory_order_release);
                        layerStateRestorePending[layerIndex].store (
                            true, std::memory_order_release);
                        layerCategory[layerIndex].clear();
                        layerIsOrgan[layerIndex].store (
                            false, std::memory_order_relaxed);
                        layerNativeFamilyTrim[layerIndex].store (
                            1.0f, std::memory_order_relaxed);
                        setLayerLoaded (layerIndex, false);
                        // pendingVstIds/States/Description ficam preservados
                        // para o proximo scan/boot, sem fingir que ainda carrega.
                    });
        }

        if (! committed)
            return false;

        if (oldWasPrepared)
            releasePluginResourcesNoThrow (old.get());
        old.reset();
        sendChangeMessage();
        return true;
    }

    void setLayerLoaded (int layerIndex, bool loaded) noexcept
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const auto bit = juce::uint32 (1) << layerIndex;

        if (loaded)
            loadedLayerMask.fetch_or (bit, std::memory_order_release);
        else
            loadedLayerMask.fetch_and (~bit, std::memory_order_release);
    }

    static void releasePluginResourcesNoThrow (
        juce::AudioPluginInstance* instance) noexcept
    {
        if (instance == nullptr)
            return;

        try
        {
            instance->releaseResources();
        }
        catch (...)
        {
        }
    }

    void reserveInstrumentScratchFor (
        const juce::AudioPluginInstance& instance)
    {
        const int requiredPluginChannels = juce::jmax (
            2,
            instance.getTotalNumInputChannels(),
            instance.getTotalNumOutputChannels());
        const int requiredScratchChannels = juce::jmax (
            instrumentBuf.getNumChannels(),
            requiredPluginChannels);
        const int requiredScratchSamples = juce::jmax (
            instrumentBuf.getNumSamples(),
            realtimeBlockReserve,
            juce::jmax (64, lastBlockSize));

        const juce::ScopedLock callbackGuard (
            getCallbackLock());
        instrumentBuf.setSize (
            requiredScratchChannels,
            requiredScratchSamples,
            false, false, false);
    }

    //==============================================================================
    struct Zone
    {
        juce::AudioBuffer<float> data;
        double sourceSampleRate = 44100.0;
        int rootNote = 60;
        int keyLo = 0, keyHi = 127;
        int velLo = 0, velHi = 127;

        // Loop REAL: lido do chunk 'smpl' do WAV quando existe, senao estimado em
        // cruzamento de zero. Antes o loop era chutado em 40%/95% do arquivo, sem
        // olhar o conteudo -- num pad segurado isso dava um clique periodico.
        int loopStart = 0, loopEnd = 0, loopXfade = 0;
        bool hasLoop = false;

        // Nivel medido do ataque (dBFS RMS dos primeiros 0,5 s). Serve para
        // descobrir quanta dinamica JA vem gravada no material -- ver
        // buildVelocityLut. -100 = nao medido.
        float rmsDb = -100.0f;

        // false quando o nome do arquivo nao trouxe nota nenhuma e caiu no
        // palpite 60. computeRanges nao pode esticar essa zona pelo teclado
        // inteiro: a nota 127 sobre um root 60 da ratio 47,9 e o sample toca
        // em 1/48 do tempo, com aliasing destrutivo.
        bool noteFromName = true;

       #if CHURCHDAW_STREAM_SAMPLES
        // No celular "data" guarda so o COMECO do sample (ver preloadSeconds);
        // o resto vem do disco pela janela da voz. streamTotal e o comprimento
        // COMPLETO em amostras -- e ele, nao data.getNumSamples(), que diz onde
        // a nota termina.
        std::shared_ptr<SampleStreamer::Source> stream =
            std::make_shared<SampleStreamer::Source>();
        int streamTotal = 0;
        bool streamed = false;      // false = coube inteiro na RAM, nao ha o que buscar
       #endif
    };

    struct Layer
    {
        std::vector<std::unique_ptr<Zone>> zones;

        // Curva de velocity CALIBRADA para este timbre (128 ganhos prontos).
        // Preenchida no finishLayerLoad, com o processamento suspenso. Comeca
        // com a curva historica para o caso de um canal tocar antes de carregar.
        float velLut[128];

        juce::String timbre;
        juce::String libraryName;   // biblioteca de origem (para restaurar cross-lib)
        std::unique_ptr<juce::AudioPluginInstance> instrument;   // VSTi externo (Kontakt) no canal
        std::unique_ptr<churchorgan::ChurchOrganLayerProcessor>
            churchOrgan;   // motor sintetizado nativo (Tonewheel/Transistor/Pipe/Hybrid)
        juce::String instrumentId, instrumentName;
        juce::AudioParameterFloat* vol = nullptr;
        juce::AudioParameterInt* transp = nullptr;
        juce::AudioParameterBool* loop = nullptr;
        juce::AudioParameterBool* mute = nullptr;
        juce::AudioParameterBool* solo = nullptr;
        juce::AudioParameterBool* on = nullptr;      // liga/desliga o canal
        juce::AudioParameterFloat* comp = nullptr;     // intensidade do compressor
        juce::AudioParameterFloat* delay = nullptr;    // mix do delay do canal
        juce::AudioParameterFloat* attack = nullptr;   // ataque do envelope, segundos
        juce::AudioParameterFloat* release = nullptr;  // release central, segundos
        juce::AudioParameterFloat* lowcut = nullptr;   // HPF legado, 0..1
        juce::AudioParameterFloat* highcut = nullptr;  // LPF legado, 0..1
        juce::AudioParameterFloat* eqLow = nullptr;    // LOW shelf, dB
        juce::AudioParameterFloat* eqMid = nullptr;    // MID bell, dB
        juce::AudioParameterFloat* eqHigh = nullptr;   // HIGH shelf, dB
        juce::AudioParameterBool* autoEq = nullptr;    // EQ automatico por tipo (worship)
        juce::AudioParameterFloat* reverbSend = nullptr;   // dose deste canal no BigSky
        juce::AudioParameterChoice* rotaryMode = nullptr;  // Slow / Stop / Fast
        juce::AudioParameterFloat* rotaryAmount = nullptr; // profundidade/mix do Leslie
    };

    static juce::String pluginToneLibrary (
        const juce::String& pluginIdentifier)
    {
        return "@PLUGIN/" + pluginIdentifier;
    }

    void resetLayerParametersToDefaults (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        auto& layer = layers[layerIndex];
        *layer.vol = 1.0f;
        *layer.transp = 0;
        *layer.loop = false;
        *layer.mute = false;
        *layer.solo = false;
        *layer.on = true;
        *layer.comp = 0.0f;
        *layer.delay = 0.0f;
        *layer.attack = 0.003f;
        *layer.release = 0.55f;
        *layer.lowcut = 0.0f;
        *layer.highcut = 0.0f;
        *layer.eqLow = 0.0f;
        *layer.eqMid = 0.0f;
        *layer.eqHigh = 0.0f;
        *layer.autoEq = false;
        *layer.reverbSend = 0.0f;
        *layer.rotaryMode = ChurchDAWRotary::Speaker::stopMode;
        *layer.rotaryAmount = 0.0f;
        layerColourIdx[layerIndex] = -1;
        layerCategory[layerIndex].clear();
        layerIsOrgan[layerIndex].store (
            false, std::memory_order_relaxed);
        layerIsChurchOrgan[layerIndex].store (
            false, std::memory_order_release);
        churchOrganSustainMirror[layerIndex].store (
            sustainStateUnknown,
            std::memory_order_release);
        pluginSustainMirror[layerIndex].store (
            sustainStateUnknown,
            std::memory_order_release);
        layerNativeFamilyTrim[layerIndex].store (
            1.0f, std::memory_order_relaxed);
        {
            const juce::ScopedLock sl (midiRouteLock);
            layerMidiDeviceId[layerIndex].clear();
            layerMidiListenChannel[layerIndex].store (
                0, std::memory_order_relaxed);
            rebuildMidiDeviceForceMap();
        }
        const bool resumeAfterDspReset = ! isSuspended();

        if (resumeAfterDspReset)
            suspendProcessing (true);
        const juce::ScopeGuard restoreProcessing {
            [this, resumeAfterDspReset]
        {
            if (resumeAfterDspReset)
                suspendProcessing (false);
        } };

        rotarySpeakers[layerIndex].reset();
        updateLayerEq (layerIndex);
        manualEqs[layerIndex].syncToTargets (
            0.0f, 0.0f, 0.0f);
    }

    ChurchDAWTimbreTone::Settings currentTimbreTone (
        int layerIndex) const noexcept
    {
        ChurchDAWTimbreTone::Settings tone;

        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return tone;

        const auto& layer = layers[layerIndex];
        tone.attackSeconds =
            layer.attack != nullptr ? (float) *layer.attack : 0.003f;
        tone.releaseSeconds =
            layer.release != nullptr ? (float) *layer.release : 0.55f;
        tone.lowDb =
            layer.eqLow != nullptr ? (float) *layer.eqLow : 0.0f;
        tone.midDb =
            layer.eqMid != nullptr ? (float) *layer.eqMid : 0.0f;
        tone.highDb =
            layer.eqHigh != nullptr ? (float) *layer.eqHigh : 0.0f;
        tone.legacyLowCut =
            layer.lowcut != nullptr ? (float) *layer.lowcut : 0.0f;
        tone.legacyHighCut =
            layer.highcut != nullptr ? (float) *layer.highcut : 0.0f;
        tone.autoEq =
            layer.autoEq != nullptr && layer.autoEq->get();
        const bool churchOrgan =
            layerIsChurchOrgan[layerIndex].load (
                std::memory_order_acquire);

        if (churchOrgan && layer.churchOrgan != nullptr)
        {
            // O Rotary interno possui um quarto estado (Brake) que nao cabe no
            // AudioParameterChoice legado do strip. Brake vem necessariamente
            // do motor. Nos demais estados, o parametro do canal e a fonte mais
            // recente: ele pode ter mudado na message thread e ainda nao ter
            // sido espelhado pelo proximo callback. Converta a ordem legada
            // Slow/Stop/Fast para a ordem canonica Stop/Slow/Fast/Brake.
            const int engineMode = juce::jlimit (
                0, 3,
                (int) std::lround (
                    layer.churchOrgan->parameters().getPlain (
                        churchorgan::OrganParameterId::rotaryMode)));

            if (engineMode == 3)
            {
                tone.rotaryMode = 3;
            }
            else
            {
                const int channelMode =
                    layer.rotaryMode != nullptr
                        ? layer.rotaryMode->getIndex()
                        : ChurchDAWRotary::Speaker::stopMode;
                tone.rotaryMode =
                    channelMode
                            == ChurchDAWRotary::Speaker::slowMode
                        ? 1
                        : channelMode
                                  == ChurchDAWRotary::Speaker::fastMode
                            ? 2
                            : 0;
            }
        }
        else
        {
            tone.rotaryMode =
                layer.rotaryMode != nullptr
                    ? layer.rotaryMode->getIndex()
                    : ChurchDAWRotary::Speaker::stopMode;
        }
        tone.rotaryAmount =
            layer.rotaryAmount != nullptr
                ? (float) *layer.rotaryAmount
                : 0.0f;
        return ChurchDAWTimbreTone::bounded (tone);
    }

    void rememberCurrentTimbreTone (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        juce::String label, library;
        {
            const juce::ScopedLock lock (layerIdentityLock);
            const auto& layer = layers[layerIndex];

            if (layer.instrumentName.isNotEmpty()
                 && layer.instrumentId.isNotEmpty())
            {
                label = layer.instrumentName;
                library = pluginToneLibrary (layer.instrumentId);
            }
            else
            {
                label = layer.timbre;
                library = layer.libraryName;
            }
        }

        if (label.isNotEmpty())
            timbreToneMemory.remember (
                layerIndex, label, library,
                currentTimbreTone (layerIndex));
    }

    void rememberOutgoingTimbreTone (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        {
            const juce::ScopedLock identityLock (layerIdentityLock);

            if (layerStateRestorePending[layerIndex].load (
                    std::memory_order_acquire))
                return;
        }

        rememberCurrentTimbreTone (layerIndex);
    }

    void completeLayerStateRestore (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        const juce::ScopedLock lock (layerIdentityLock);
        layerStateRestoreTargetTimbre[layerIndex].clear();
        layerStateRestoreTargetLibrary[layerIndex].clear();
        layerStateRestoreTargetVstId[layerIndex].clear();
        layerStateRestoreTargetVstState[layerIndex].clear();
        layerStateRestorePending[layerIndex].store (
            false, std::memory_order_release);
    }

    bool applyRememberedTimbreTone (
        int layerIndex, const juce::String& label,
        const juce::String& library)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return false;

        const juce::ScopedLock toneLock (layerToneTransactionLock);
        ChurchDAWTimbreTone::Settings tone;

        if (! timbreToneMemory.recall (
                layerIndex, label, library, tone))
            return false;

        programmaticToneChangeDepth.fetch_add (
            1, std::memory_order_acq_rel);
        const juce::ScopeGuard finishProgrammaticChange { [this]
        {
            programmaticToneChangeDepth.fetch_sub (
                1, std::memory_order_acq_rel);
        } };

        auto& layer = layers[layerIndex];
        *layer.attack = tone.attackSeconds;
        *layer.release = tone.releaseSeconds;
        *layer.eqLow = tone.lowDb;
        *layer.eqMid = tone.midDb;
        *layer.eqHigh = tone.highDb;
        *layer.lowcut = tone.legacyLowCut;
        *layer.highcut = tone.legacyHighCut;
        *layer.autoEq = tone.autoEq;
        const bool churchOrgan =
            layerIsChurchOrgan[layerIndex].load (
                std::memory_order_acquire);

        if (churchOrgan)
        {
            const int organMode = juce::jlimit (
                0, 3, tone.rotaryMode);
            int channelMode = ChurchDAWRotary::Speaker::stopMode;

            if (organMode == 1)
                channelMode = ChurchDAWRotary::Speaker::slowMode;
            else if (organMode == 2)
                channelMode = ChurchDAWRotary::Speaker::fastMode;

            // Tonewheel/Hybrid usam este valor no RotaryCabinet interno.
            // Transistor/Pipe mapeiam Brake para STOP no estágio externo, que
            // preserva a desaceleração física, mas mantêm o valor 3 na memória.
            if (layer.churchOrgan != nullptr)
                layer.churchOrgan->setParameterPlain (
                    churchorgan::OrganParameterId::rotaryMode,
                    (float) organMode);

            *layer.rotaryMode = channelMode;
            lastChurchOrganChannelRotaryMode[layerIndex].store (
                channelMode, std::memory_order_release);
        }
        else
        {
            *layer.rotaryMode = juce::jlimit (
                ChurchDAWRotary::Speaker::slowMode,
                ChurchDAWRotary::Speaker::fastMode,
                tone.rotaryMode);
        }
        *layer.rotaryAmount = tone.rotaryAmount;
        return true;
    }

    // EQ multibanda por canal (ate 4 bandas IIR, estereo) para o Auto EQ
    struct LayerEq
    {
        juce::dsp::IIR::Filter<float> bands[2][4];
        int numBands = 0;

        void prepare (double sr, int block)
        {
            const juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (64, block), 1 };

            for (int ch = 0; ch < 2; ++ch)
                for (auto& b : bands[ch])
                    b.prepare (spec);
        }

        void reset()
        {
            for (int ch = 0; ch < 2; ++ch)
                for (auto& b : bands[ch])
                    b.reset();
        }

        void setCoeffs (const juce::Array<juce::dsp::IIR::Coefficients<float>::Ptr>& cs)
        {
            numBands = juce::jmin (4, cs.size());

            for (int b = 0; b < numBands; ++b)
                for (int ch = 0; ch < 2; ++ch)
                    *bands[ch][b].coefficients = *cs[b];
        }

        void process (juce::AudioBuffer<float>& buf, int numSamples)
        {
            for (int ch = 0; ch < juce::jmin (2, buf.getNumChannels()); ++ch)
            {
                float* d = buf.getWritePointer (ch);

                for (int b = 0; b < numBands; ++b)
                {
                    auto& filt = bands[ch][b];

                    for (int s = 0; s < numSamples; ++s)
                        d[s] = filt.processSample (d[s]);
                }
            }
        }
    };

    // presets de EQ pesquisados para mix de louvor (worship), por categoria
    static juce::Array<juce::dsp::IIR::Coefficients<float>::Ptr> eqPresetFor (const juce::String& cat, double sr)
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        auto dB = [] (float d) { return juce::Decibels::decibelsToGain (d); };
        juce::Array<C::Ptr> v;

        if (cat == "PIANOS")
        {
            v.add (C::makeHighPass   (sr, 80.0f));
            v.add (C::makePeakFilter (sr, 250.0f, 2.0f, dB (-3.0f)));    // tira o "papelao"/mud
            v.add (C::makePeakFilter (sr, 3500.0f, 1.0f, dB (2.0f)));   // presenca para cortar na mix
            v.add (C::makeHighShelf  (sr, 10000.0f, 0.7f, dB (2.0f)));  // brilho do martelo
        }
        else if (cat == "EP & TECLAS")
        {
            v.add (C::makeHighPass   (sr, 80.0f));
            v.add (C::makePeakFilter (sr, 500.0f, 4.0f, dB (-3.0f)));   // limpa mud dos keys
            v.add (C::makePeakFilter (sr, 3000.0f, 1.2f, dB (2.0f)));   // presenca
            v.add (C::makeHighShelf  (sr, 11000.0f, 0.7f, dB (1.5f)));
        }
        else if (cat == "PADS" || cat == "AMBIENCIAS")
        {
            v.add (C::makeHighPass   (sr, 140.0f));                     // sai da frente do kick/baixo
            v.add (C::makePeakFilter (sr, 700.0f, 1.2f, dB (-2.5f)));   // abre espaco pro vocal
            v.add (C::makeHighShelf  (sr, 10000.0f, 0.7f, dB (2.5f)));  // ar/atmosfera
        }
        else if (cat == "ORGAOS")
        {
            v.add (C::makeHighPass   (sr, 100.0f));
            v.add (C::makePeakFilter (sr, 280.0f, 1.0f, dB (1.5f)));    // corpo/fundamentais
            v.add (C::makePeakFilter (sr, 4000.0f, 1.5f, dB (-1.5f)));  // tira o agudo aspero
            v.add (C::makeHighShelf  (sr, 10000.0f, 0.7f, dB (2.0f)));  // shimmer
        }
        else if (cat == "STRINGS")
        {
            v.add (C::makeHighPass   (sr, 100.0f));
            v.add (C::makePeakFilter (sr, 250.0f, 1.0f, dB (1.0f)));    // fullness das cordas
            v.add (C::makePeakFilter (sr, 3500.0f, 1.2f, dB (1.0f)));   // presenca
            v.add (C::makeHighShelf  (sr, 8500.0f, 0.7f, dB (-2.0f)));  // reduz "scratch" do arco
        }
        else if (cat == "LEADS & SYNTHS")
        {
            v.add (C::makeHighPass   (sr, 100.0f));
            v.add (C::makePeakFilter (sr, 1500.0f, 1.0f, dB (2.0f)));   // cut-through do lead
            v.add (C::makePeakFilter (sr, 3500.0f, 1.2f, dB (1.5f)));
        }
        else   // geral: limpeza leve
        {
            v.add (C::makeHighPass   (sr, 80.0f));
            v.add (C::makePeakFilter (sr, 250.0f, 1.5f, dB (-2.0f)));
            v.add (C::makePeakFilter (sr, 3500.0f, 1.0f, dB (1.5f)));
        }

        return v;
    }

    struct Voice
    {
        const Zone* zone = nullptr;
        int layerIndex = 0;
        double pos = 0.0, ratio = 1.0;
        float gain = 1.0f;
        juce::ADSR adsr;
        int note = -1;
        int soundingNote = -1;   // nota ja transposta, usada no release acustico
        int channel = 1;   // canal MIDI do note-on (2 teclados na mesma nota)
        int velocity = 100;
        bool active = false;
        bool released = false;
        bool heldByPedal = false;
        juce::int64 startOrder = 0;
        float lastEnv = 0.0f;   // ultima envoltoria renderizada (escolha da vitima do roubo)

        // Ultimo valor que esta voz produziu, e a rampa que apaga o degrau
        // quando ela e ROUBADA no meio de uma nota. Ver startVoiceForLayer.
        float lastOutL = 0.0f, lastOutR = 0.0f;
        float declickL = 0.0f, declickR = 0.0f;
        // Quando uma janela de streaming chega tarde, o relógio do envelope
        // continua avançando durante o silêncio. Ao a janela voltar, esta rampa
        // curta impede que a nota reapareça num degrau.
        float streamRecoveryGain = 1.0f;

        // A primeira janela do disco e pedida UMA vez por nota. Sem esta marca
        // o pedido se repetia a cada bloco durante meio segundo e a thread de
        // leitura relia o mesmo trecho sem parar. Zerada junto com o resto no
        // note-on (a voz e reatribuida por atribuicao de struct).
        bool primeiraJanelaPedida = false;
    };

    // Curva historica do motor. Continua sendo a resposta para material que NAO
    // tem camadas de velocity (orgao, lead, pad de uma camada so): ali o sample
    // nao carrega dinamica nenhuma e a curva precisa fornecer tudo.
    static float legacyVelGain (int v) noexcept
    {
        const float vn = juce::jlimit (0.0f, 1.0f, (float) v / 127.0f);
        return 0.05f + 0.95f * vn * vn;
    }

    // Quantas amostras de silencio ha antes do primeiro som audivel.
    //
    // -60 dBFS e o limiar: abaixo disso o conteudo e inaudivel, entao descartar
    // nao muda o timbre -- muda so o tempo entre apertar a tecla e ouvir.
    //
    // O TETO importa mais que o limiar. Sem ele, samples que comecam em silencio
    // DE PROPOSITO seriam decapitados: nesta biblioteca sao os 39 arquivos "REV"
    // (pratos e swells invertidos), que sobem do nada por mais de 120 ms. Cortar
    // aqueles destruiria o efeito. Passou do teto, nao mexemos.
    static int headSilence (const juce::AudioBuffer<float>& d, int maxSkip) noexcept
    {
        const int n  = juce::jmin (d.getNumSamples(), juce::jmax (0, maxSkip));
        const int ch = d.getNumChannels();

        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c)
                if (std::abs (d.getSample (c, i)) >= 0.001f)   // -60 dBFS
                    return i;

        return 0;   // silencioso alem do teto: intencao, nao tempo morto
    }

    // Nivel RMS do ataque, em dBFS. Mede a partir da primeira amostra audivel
    // para nao medir o silencio de cabeca do arquivo.
    static float attackRmsDb (const juce::AudioBuffer<float>& d, int window) noexcept
    {
        const int len = d.getNumSamples();

        if (len <= 0 || d.getNumChannels() <= 0)
            return -100.0f;

        const float* s = d.getReadPointer (0);
        int start = 0;

        while (start < len && std::abs (s[start]) < 0.001f)   // -60 dBFS
            ++start;

        if (start >= len)
            return -100.0f;

        const int n = juce::jmin (window, len - start);

        if (n <= 0)
            return -100.0f;

        double acc = 0.0;

        for (int i = 0; i < n; ++i)
            acc += (double) s[start + i] * (double) s[start + i];

        const double rms = std::sqrt (acc / (double) n);
        return rms > 1.0e-7 ? (float) (20.0 * std::log10 (rms)) : -100.0f;
    }

    // CALIBRAGEM DE DINAMICA -- a mudanca de maior impacto do motor.
    //
    // O problema: as camadas de velocity de uma biblioteca boa JA sao gravadas
    // com niveis diferentes (medido no acervo do usuario: 28,7 dB entre a camada
    // mais fraca e a mais forte da mesma nota). A curva de ganho do motor somava
    // outros 26 dB em cima disso -- ~55 dB de faixa, contra os 35-40 dB de um
    // piano acustico real. Na pratica o pianissimo sumia debaixo da banda, o
    // tecladista compensava martelando a tecla e passava a viver so nas duas
    // camadas de cima: piano duro e sempre igual.
    //
    // A correcao nao pode ser uma constante nova, porque as bibliotecas se
    // comportam de formas OPOSTAS: uma traz 28,7 dB gravados, outra traz 12, e
    // biblioteca normalizada por pico nao traz nenhum. Entao a curva e MEDIDA do
    // material: quanto mais dinamica ja existe no sample, menos a curva impoe.
    //
    // A dispersao e medida SEMPRE dentro da MESMA nota. Comparar notas
    // diferentes seria um erro grave: o RMS desaba com a altura (nota aguda
    // decai rapido), e uma compensacao baseada nisso mandaria +23 dB no agudo.
    static void buildVelocityLut (const std::vector<std::unique_ptr<Zone>>& zs, float* lut) noexcept
    {
        for (int v = 0; v < 128; ++v)
            lut[v] = legacyVelGain (v);

        bool velocitySeen[128] {};
        int velocityCount = 0;
        int velBottom = 127;
        int velTop = 1;

        for (const auto& z : zs)
        {
            const int velocity = juce::jlimit (1, 127, z->velHi);

            if (! velocitySeen[velocity])
            {
                velocitySeen[velocity] = true;
                ++velocityCount;
                velBottom = juce::jmin (velBottom, velocity);
                velTop = juce::jmax (velTop, velocity);
            }
        }

        // Uma camada so: o material nao tem dinamica gravada. Mantem a curva
        // historica BIT A BIT -- e o caso de todo o naipe de orgao e de lead.
        if (velocityCount <= 1)
            return;

        // menor limite superior de velocity = a camada mais fraca do conjunto
        velBottom = juce::jlimit (1, 126, velBottom);

        // dispersao por nota: nivel da camada de topo menos o da camada de baixo
        // Existe no maximo uma medicao util por nota MIDI. O array fixo fecha um
        // P0: a versao anterior usava juce::Array dentro desta funcao noexcept;
        // bad_alloc ali chamava std::terminate durante a troca de timbre.
        float spread[128] {};
        bool noteMeasured[128] {};
        int spreadCount = 0;

        for (const auto& a : zs)
        {
            if (a->velHi != velBottom || a->rmsDb <= -99.0f)
                continue;

            const int note = juce::jlimit (0, 127, a->rootNote);

            if (noteMeasured[note])
                continue;

            for (const auto& b : zs)
                if (b->velHi == velTop && b->rootNote == a->rootNote && b->rmsDb > -99.0f)
                {
                    spread[spreadCount++] = b->rmsDb - a->rmsDb;
                    noteMeasured[note] = true;
                    break;
                }
        }

        if (spreadCount == 0)
            return;   // sem medicao confiavel: nao arrisca, fica como estava

        std::sort (spread, spread + spreadCount);
        const float sDb = spread[spreadCount / 2];   // mediana

        // O que falta para completar a faixa de um piano real. Material com
        // muita dinamica gravada recebe pouca curva; material achatado recebe
        // quase tudo. O piso de 8 dB impede curva plana demais.
        const float eDb = juce::jlimit (8.0f, 38.0f, 38.0f - sDb);

        const float span = 20.0f * std::log10 (127.0f / (float) velBottom);

        if (span < 1.0f)
            return;

        // p < 1 levanta as notas fracas; p > 1 as abaixa
        const float p = juce::jlimit (0.30f, 3.50f, eDb / span);

        for (int v = 0; v < 128; ++v)
            lut[v] = std::pow (juce::jlimit (0.0f, 1.0f, (float) v / 127.0f), p);

        lut[0] = 0.0f;
    }

    // Decodificar o cabecalho e os 2 s residentes de centenas de FLACs era
    // totalmente serial. Num piano grande isso deixava quatro nucleos potentes
    // parados e o usuario via "Carregando timbre..." por cerca de 15 s.
    // As zonas sao independentes: dividimos os arquivos entre ate quatro
    // workers, cada um com seu AudioFormatManager, e recalculamos os ranges
    // somente depois de reunir tudo. O resultado sonoro e identico; muda apenas
    // quanto tempo levamos para prepara-lo.
    static bool decodeCancelled (const std::atomic<bool>* globalAbort,
                                 const std::atomic<bool>* requestAbort) noexcept
    {
        return (globalAbort != nullptr
                 && globalAbort->load (std::memory_order_acquire))
            || (requestAbort != nullptr
                 && requestAbort->load (std::memory_order_acquire));
    }

    static std::vector<std::unique_ptr<Zone>> decodeZones (
        const std::vector<juce::File>& files,
        const std::atomic<bool>* globalAbort = nullptr,
        const std::atomic<bool>* requestAbort = nullptr)
    {
        if (decodeCancelled (globalAbort, requestAbort))
            return {};

       #if CHURCHDAW_STREAM_SAMPLES
        const int hw = (int) std::thread::hardware_concurrency();
        const int fileCount = (int) files.size();
        const int workerCount = juce::jlimit (
            1, 4, juce::jmin (fileCount, juce::jmax (1, hw / 2)));

        if (workerCount > 1 && fileCount >= 64)
        {
            std::vector<std::vector<juce::File>> chunks ((size_t) workerCount);
            std::vector<std::vector<std::unique_ptr<Zone>>> partial ((size_t) workerCount);
            std::vector<std::exception_ptr> failures ((size_t) workerCount);
            std::vector<std::thread> workers;
            workers.reserve ((size_t) workerCount);

            // Blocos contiguos: ao reunir worker 0, 1, 2... a ordem original
            // dos arquivos permanece intacta. Isto importa quando um pack traz
            // duas zonas com a mesma nota/velocity e findZone escolhe a primeira.
            for (int i = 0; i < fileCount; ++i)
            {
                const int w = juce::jmin (workerCount - 1,
                                          (int) (((juce::int64) i * workerCount) / fileCount));
                chunks[(size_t) w].push_back (files[(size_t) i]);
            }

            try
            {
                for (int w = 0; w < workerCount; ++w)
                    workers.emplace_back ([&, w]
                    {
                        try
                        {
                            partial[(size_t) w] = decodeZonesSerial (
                                chunks[(size_t) w], globalAbort, requestAbort);
                        }
                        catch (...)
                        {
                            // Excecao jamais pode escapar de std::thread: isso
                            // chama std::terminate e derruba o processo inteiro.
                            failures[(size_t) w] = std::current_exception();
                        }
                    });
            }
            catch (...)
            {
                // Se a criacao de uma das threads falhar, as anteriores ainda
                // sao joinable; destruir o vetor sem join tambem terminaria o app.
                for (auto& worker : workers)
                    if (worker.joinable())
                        worker.join();

                throw;
            }

            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();

            for (const auto& failure : failures)
                if (failure != nullptr)
                    std::rethrow_exception (failure);

            if (decodeCancelled (globalAbort, requestAbort))
                return {};

            std::vector<std::unique_ptr<Zone>> zones;
            size_t total = 0;

            for (const auto& p : partial)
                total += p.size();

            zones.reserve (total);

            for (auto& p : partial)
                for (auto& z : p)
                    zones.push_back (std::move (z));

            computeRanges (zones);
            return zones;
        }
       #endif

        auto zones = decodeZonesSerial (files, globalAbort, requestAbort);

        if (decodeCancelled (globalAbort, requestAbort))
            return {};

        computeRanges (zones);
        return zones;
    }

    static std::vector<std::unique_ptr<Zone>> decodeZonesSerial (
        const std::vector<juce::File>& files,
        const std::atomic<bool>* globalAbort = nullptr,
        const std::atomic<bool>* requestAbort = nullptr)
    {
        juce::AudioFormatManager afm;
        afm.registerBasicFormats();

        std::vector<std::unique_ptr<Zone>> zones;

        for (const auto& f : files)
        {
            // cancelamento por arquivo: o join do destrutor espera no maximo o
            // decode do arquivo atual, nao o timbre inteiro
            if (decodeCancelled (globalAbort, requestAbort))
                break;

            juce::String instr;
            int note = 60, vel = 127;
            bool noteKnown = true;
            const auto stem = f.getFileNameWithoutExtension();

            if (! parseSampleName (stem, instr, note, vel)
                 && ! parseSpaceMidiName (stem, instr, note, vel))
            {
                vel = 127;

                if (! parseNumberedName (stem, instr, note))
                    if (! parseNoteOnlyName (stem, note))
                    {
                        note = 60;
                        noteKnown = false;   // computeRanges limita o alcance desta zona
                    }

                if (note < 0 || note > 127)
                {
                    note = 60;
                    noteKnown = false;
                }
            }

            std::unique_ptr<juce::AudioFormatReader> reader (afm.createReaderFor (f));

            if (reader == nullptr || reader->lengthInSamples <= 0)
                continue;

            auto z = std::make_unique<Zone>();
            z->rootNote = note;
            z->noteFromName = noteKnown;
            z->velHi = juce::jlimit (1, 127, vel);
            // header corrompido com sampleRate absurdo estourava o int de "len"
            // abaixo (alocacao invalida em thread sem try/catch)
            z->sourceSampleRate = reader->sampleRate > 0 && reader->sampleRate <= 384000.0
                                      ? reader->sampleRate : 44100.0;

            // cauda ORIGINAL COMPLETA, sem cortes (guarda de sanidade em 120 s
            // apenas para nao estourar memoria com arquivo corrompido/gigante)
            const auto maxLen = (juce::int64) (z->sourceSampleRate * 120.0);
            const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);

            // header corrompido com numChannels=65535 alocaria ~TB (bad_alloc
            // em worker thread); o motor so le os canais 0/1, entao 2 bastam.
            // O jlimit tambem cobre header que declara 0 canais.
            const int chans = juce::jlimit (1, 2, (int) reader->numChannels);

            int skip = 0;   // tempo morto cortado do inicio (ver bloco abaixo)

           #if CHURCHDAW_STREAM_SAMPLES
            // CELULAR: le so o comeco; o resto vem do disco enquanto a nota soa.
            //
            // A janela LIDA e maior que a GUARDADA de proposito. O RMS do ataque
            // e medido em 0,5 s e a calibragem de dinamica (buildVelocityLut)
            // depende dele -- ela ja foi testada no PC e nao pode mudar so
            // porque o celular guarda menos. Entao: le 0,5 s, mede, guarda 0,15 s.
            // O buffer lido precisa cobrir o MAIOR entre a janela de medicao
            // (0,5 s, exigida pelo RMS do ataque) e o preload que vai ficar
            // residente. Ficou so 0,5 s numa versao anterior, e como o preload e
            // recortado DESTE buffer, aumentar preloadSeconds para 2 s nao teve
            // efeito nenhum -- o preload continuou 0,5 s e as notas seguiam indo
            // ao disco, que era a origem dos cliques.
            const int measureLen = juce::jmin (len, (int) (z->sourceSampleRate * 0.5));
            const int readLen    = juce::jmin (len, (int) (z->sourceSampleRate
                                                            * juce::jmax (0.5, preloadSeconds)));

            juce::AudioBuffer<float> head (chans, juce::jmax (1, readLen));
            head.clear();
            reader->read (&head, 0, readLen, 0, true, true);

            skip = juce::jmin (headSilence (head, (int) (z->sourceSampleRate * 0.120)),
                               juce::jmax (0, readLen - 1));

            if (skip > 0)
            {
                // readLen, nao measureLen: encolher para a janela de MEDICAO
                // jogaria fora justamente o preload que acabamos de ler.
                const int kept = readLen - skip;

                for (int ch = 0; ch < chans; ++ch)
                {
                    auto* p = head.getWritePointer (ch);
                    std::memmove (p, p + skip, sizeof (float) * (size_t) kept);
                }

                head.setSize (chans, kept, true, true, true);
            }

            z->rmsDb = attackRmsDb (head, (int) (z->sourceSampleRate * 0.5));

            const int preloadLen = juce::jmin (head.getNumSamples(),
                                               (int) (z->sourceSampleRate * preloadSeconds));

            z->data.setSize (chans, juce::jmax (1, preloadLen));
            z->data.clear();

            for (int ch = 0; ch < chans; ++ch)
                z->data.copyFrom (ch, 0, head, ch, 0, preloadLen);

            // A posicao 0 da VOZ e a primeira amostra audivel, nao a primeira do
            // arquivo: o deslocamento vai para a fonte, senao o streaming
            // reintroduziria o tempo morto que acabamos de cortar.
            z->streamTotal        = len - skip;
            z->streamed           = (z->streamTotal > preloadLen);
            z->stream->file         = f;
            z->stream->numChannels  = chans;
            z->stream->fileOffset   = skip;
            z->stream->totalSamples = z->streamTotal;
           #else
            z->data.setSize (chans, len);
            reader->read (&z->data, 0, len, 0, true, true);
           #endif

            resolveLoop (*z, reader->metadataValues, len);

           #if CHURCHDAW_STREAM_SAMPLES
            // ZONA COM LOOP NAO E TRANSMITIDA: o loop volta para tras, e a janela
            // deslizante ja passou por ali -- buscar de novo a cada volta seria
            // ler o disco em circulo, varias vezes por segundo, por voz. Como
            // material com loop e curto por natureza (pad, orgao, sustentado),
            // ele cabe na RAM sem drama. Custa uma segunda leitura na CARGA, que
            // acontece uma vez, fora da thread de audio.
            if (z->hasLoop && z->streamed)
            {
                z->data.setSize (chans, len);
                z->data.clear();
                reader->read (&z->data, 0, len, 0, true, true);
                z->streamed = false;
                skip = 0;                       // o corte sera refeito abaixo, no buffer inteiro
                z->stream->fileOffset = 0;
                z->rmsDb = -100.0f;             // remedido adiante
            }
           #endif

            // TEMPO MORTO NO INICIO DO ARQUIVO -- o atraso que nenhuma medicao
            // de latencia acusa, porque nao e do sistema: esta dentro do sample.
            //
            // Medido nesta biblioteca: 8955 dos 9183 samples (97,5%) comecam com
            // silencio gravado, mediana de 12,4 ms. No piano sao 12,0 ms com
            // variacao de +-0,1 ms entre notas -- essa constancia denuncia
            // pre-roll de gravacao, nao intencao musical.
            //
            // 12 ms de arquivo + 10,7 ms de buffer = ~23 ms entre apertar a tecla
            // e ouvir. Mais da METADE vinha do arquivo.
            //
            // Por que e seguro: so corta o que esta abaixo de -60 dBFS, que e
            // inaudivel por definicao. O ataque nao muda de forma, o pad nao
            // perde o crescendo -- sai apenas o pedaco que ninguem ouviria.
            //
            // Nao usamos o marcador 'Start' do AIFF: no piano ele existe e vale
            // ZERO enquanto o som comeca 530 quadros adiante, entao confiar nele
            // deixaria justamente o timbre principal sem correcao.
            // Caminho de RAM: o corte acontece aqui, sobre o buffer inteiro.
            // No streaming ele ja foi feito sobre a janela de medicao e virou
            // deslocamento na fonte -- MENOS quando a zona tem loop e voltou
            // para a RAM logo acima, caso em que ela passa por aqui tambem.
           #if CHURCHDAW_STREAM_SAMPLES
            if (! z->streamed && z->rmsDb < -99.0f)
           #endif
            {
            skip = headSilence (z->data, (int) (z->sourceSampleRate * 0.120));

            if (skip > 0)
            {
                const int kept = len - skip;

                for (int ch = 0; ch < z->data.getNumChannels(); ++ch)
                {
                    auto* p = z->data.getWritePointer (ch);
                    std::memmove (p, p + skip, sizeof (float) * (size_t) kept);
                }

                z->data.setSize (z->data.getNumChannels(), kept, true, true, true);
            }

            // medicao na thread de CARGA, nunca na de audio.
            // DEPOIS do corte: com o silencio fora, a janela de 0,5 s pega meio
            // segundo de SOM em vez de 12 ms de nada mais 488 ms de som.
            z->rmsDb = attackRmsDb (z->data, (int) (z->sourceSampleRate * 0.5));
            }

            // loops sao posicoes ABSOLUTAS no arquivo: andam junto com o corte.
            // Vale para os DOIS caminhos -- no streaming a voz tambem conta a
            // partir da primeira amostra audivel.
            if (skip > 0 && z->loopEnd > 0)
            {
                z->loopStart = juce::jmax (0, z->loopStart - skip);
                z->loopEnd   = juce::jmax (0, z->loopEnd   - skip);

                if (z->loopEnd <= z->loopStart)
                    z->loopStart = z->loopEnd = 0;   // corte invalidou o loop
            }

            zones.push_back (std::move (z));
        }

        return decodeCancelled (globalAbort, requestAbort)
                 ? std::vector<std::unique_ptr<Zone>>()
                 : std::move (zones);
    }

    // procura o cruzamento de zero mais proximo de "around" (janela de +-2000
    // amostras). Emendar o loop num cruzamento de zero ja elimina a maior parte
    // do estalo; o crossfade cuida do resto.
    // Olha os DOIS canais. Antes so o canal 0 era considerado: num sample
    // estereo o cruzamento de zero da esquerda cai num ponto qualquer da direita,
    // e a emenda do loop vazava pelo outro lado. Agora, entre os cruzamentos
    // validos da esquerda, escolhe aquele em que a direita tambem esta mais perto
    // de zero.
    static int nearestZeroCrossing (const juce::AudioBuffer<float>& d, int around, int lo, int hi)
    {
        const float* s = d.getReadPointer (0);
        const float* r2 = d.getReadPointer (juce::jmin (1, d.getNumChannels() - 1));
        around = juce::jlimit (lo + 1, hi - 1, around);

        int best = -1;
        float bestErr = 0.0f;

        for (int r = 0; r < 2000; ++r)
        {
            const int a = around - r, b = around + r;

            for (const int c : { a, b })
            {
                if (c <= lo || c >= hi)
                    continue;

                if (! (s[c - 1] <= 0.0f && s[c] > 0.0f))
                    continue;

                const float err = std::abs (r2[c]);

                if (best < 0 || err < bestErr)
                {
                    best = c;
                    bestErr = err;

                    // ja esta praticamente no zero nos dois canais: nao ha o que
                    // melhorar, e continuar procurando so afasta do alvo
                    if (err < 1.0e-4f)
                        return best;
                }
            }
        }

        return best >= 0 ? best : around;
    }

    static void resolveLoop (Zone& z, const juce::StringPairArray& meta, int len)
    {
        if (len < 4096)
            return;

        // 1) o loop gravado no proprio arquivo (chunk 'smpl' do WAV)
        if (meta.getValue ("NumSampleLoops", "0").getIntValue() > 0)
        {
            const int ls = meta.getValue ("Loop0Start", "-1").getIntValue();

            // loopEnd nunca passa de len-2: com le == len-1 a guarda de fim de
            // sample matava a voz antes do wrap
            const int le = juce::jmin (meta.getValue ("Loop0End", "-1").getIntValue(), len - 2);

            if (ls >= 0 && le > ls + 256)
            {
                z.loopStart = ls;
                z.loopEnd   = le;
                z.hasLoop   = true;
            }
        }

        // 1b) AIFF guarda o loop de um jeito COMPLETAMENTE diferente, e por isso
        // ele nunca era lido: o leitor do JUCE publica "NumSampleLoops" = 2
        // SEMPRE (juce_AiffAudioFormat.cpp:91), mesmo em arquivo sem loop, e as
        // chaves "Loop0Start"/"Loop0End" que o bloco acima procura so existem em
        // WAV. No AIFF vem "Loop0StartIdentifier"/"Loop0EndIdentifier", que sao
        // IDs de MARCADOR, nao posicoes -- a posicao esta na tabela de cue
        // points (Cue{i}Identifier / Cue{i}Offset). Sem isto, TODO arquivo AIFF
        // caia no palpite de 40%/95% da secao 2, ignorando o loop real que o
        // autor da biblioteca gravou. Vale para pads, strings e orgao; o piano
        // toca sem loop e nao e afetado.
        if (! z.hasLoop && meta.getValue ("Loop0Type", "0").getIntValue() > 0)
        {
            const int idStart = meta.getValue ("Loop0StartIdentifier", "-1").getIntValue();
            const int idEnd   = meta.getValue ("Loop0EndIdentifier", "-1").getIntValue();
            const int numCues = meta.getValue ("NumCuePoints", "0").getIntValue();

            int ls = -1, le = -1;

            for (int i = 0; i < numCues && i < 512; ++i)
            {
                const auto pre = "Cue" + juce::String (i);
                const int id = meta.getValue (pre + "Identifier", "-1").getIntValue();

                if (id < 0)
                    continue;

                const int off = meta.getValue (pre + "Offset", "-1").getIntValue();

                if (off < 0)
                    continue;

                if (id == idStart) ls = off;
                if (id == idEnd)   le = off;
            }

            le = juce::jmin (le, len - 2);

            if (ls >= 0 && le > ls + 256)
            {
                z.loopStart = ls;
                z.loopEnd   = le;
                z.hasLoop   = true;
            }
        }

        // 2) sem metadado: estima na cauda, mas ancorado em cruzamento de zero.
        //
        // SO SE O SAMPLE INTEIRO ESTIVER NA MEMORIA. Esta secao varre o buffer em
        // 40% e 95% do comprimento do ARQUIVO -- com streaming o buffer tem so os
        // 0,15 s do preload, e a varredura saia lendo memoria alheia. Foi
        // exatamente isso: SIGSEGV em nearestZeroCrossing ao carregar o piano.
        //
        // A guarda olha o buffer, nao a plataforma: assim ela protege qualquer
        // chamada futura em que "len" e o arquivo e "data" e um pedaco dele.
        //
        // Nao perdemos nada de real: loop de VERDADE vem por metadado (secoes 1 e
        // 1b), e ao encontrar um o carregador traz a zona inteira para a RAM.
        // O que se perde e o loop ADIVINHADO -- que em piano seria errado.
        if (! z.hasLoop && z.data.getNumSamples() >= len)
        {
            const int a = nearestZeroCrossing (z.data, (int) (len * 0.4), 0, len);

            // "a" perto do fim inverte a janela (lo+1 > hi-1) e leria fora do
            // buffer: sem janela valida, sem loop estimado
            const int b = a <= len - 1027 ? nearestZeroCrossing (z.data, (int) (len * 0.95), a + 1024, len - 1)
                                          : a;

            if (b > a + 1024)
            {
                z.loopStart = a;
                z.loopEnd   = b;
                z.hasLoop   = true;
            }
        }

        if (! z.hasLoop)
            return;

        // crossfade de ate ~50 ms, limitado pelo material disponivel antes do
        // inicio do loop (o fade le de loopStart - xfade)
        const int wanted = (int) (z.sourceSampleRate * 0.05);
        z.loopXfade = juce::jmin (wanted, z.loopStart, (z.loopEnd - z.loopStart) / 4);

        if (z.loopXfade < 32)
            z.loopXfade = 0;
    }

    static void computeRanges (std::vector<std::unique_ptr<Zone>>& zs)
    {
        std::vector<int> vels;
        vels.reserve (128);
        bool velocitySeen[128] {};

        for (auto& z : zs)
        {
            const int velocity = juce::jlimit (0, 127, z->velHi);

            if (! velocitySeen[velocity])
            {
                velocitySeen[velocity] = true;
                vels.push_back (velocity);
            }
        }

        std::sort (vels.begin(), vels.end());

        for (auto& z : zs)
        {
            const auto found = std::lower_bound (vels.begin(), vels.end(), z->velHi);
            const auto idx = (size_t) std::distance (vels.begin(), found);
            z->velLo = idx == 0 ? 0 : vels[idx - 1] + 1;
        }

        std::vector<Zone*> ofLayer;
        ofLayer.reserve (zs.size());

        for (const int velocity : vels)
        {
            ofLayer.clear();

            for (auto& z : zs)
                if (z->velHi == velocity)
                    ofLayer.push_back (z.get());

            std::sort (ofLayer.begin(), ofLayer.end(),
                       [] (const Zone* a, const Zone* b) { return a->rootNote < b->rootNote; });

            for (size_t i = 0; i < ofLayer.size(); ++i)
            {
                ofLayer[i]->keyLo = i == 0 ? 0
                                           : (ofLayer[i - 1]->rootNote + ofLayer[i]->rootNote) / 2 + 1;
                ofLayer[i]->keyHi = i == ofLayer.size() - 1 ? 127
                                                            : (ofLayer[i]->rootNote + ofLayer[i + 1]->rootNote) / 2;
            }
        }

        // camada de topo se estende ate 127 (nome tipo V100 deixava 101-127 mudos),
        // espelhando o que keyHi ja faz; DEPOIS do laco acima, que agrupa por velHi
        for (auto& z : zs)
            if (! vels.empty() && z->velHi == vels.back())
                z->velHi = 127;

        // Zona cujo arquivo nao trazia nota no nome ficou com root 60 por palpite.
        // Esticada de 0 a 127 pelo laco acima, a nota 127 daria ratio 2^(67/12) =
        // 47,9: o sample inteiro tocaria em 1/48 do tempo, agudissimo e com
        // aliasing destrutivo. Meia oitava para cada lado mantem o som utilizavel.
        for (auto& z : zs)
            if (! z->noteFromName)
            {
                z->keyLo = juce::jmax (z->keyLo, z->rootNote - 6);
                z->keyHi = juce::jmin (z->keyHi, z->rootNote + 6);
            }
    }

    // Chamado SOMENTE com o processamento suspenso. A insercao normal preserva
    // os samples ate as vozes liberarem seus ponteiros crus. Sob OOM, push_back
    // pode falhar; nesse caso silenciamos apenas as vozes que pertencem a este
    // conjunto antes de destrui-lo. Pausa de audio no pior caso e aceitavel;
    // use-after-free nunca e.
    bool retireZonesWhileSuspended (
        std::vector<std::unique_ptr<Zone>>& old) noexcept
    {
        if (old.empty())
            return false;

        try
        {
            retiredZones.push_back (std::move (old));
            return true;
        }
        catch (...)
        {
            for (auto& voice : voices)
            {
                if (! voice.active || voice.zone == nullptr)
                    continue;

                bool belongsToOldSet = false;

                for (const auto& zone : old)
                    if (voice.zone == zone.get())
                    {
                        belongsToOldSet = true;
                        break;
                    }

                if (belongsToOldSet)
                    silenceVoiceWithoutClick (voice);
            }

            old.clear();
            return false;
        }
    }

    // AudioBuffers de um piano podem somar centenas de MB. Destruir o vetor no
    // MessageManager (resultado obsoleto ou purge) devolve milhares de blocos ao
    // allocator e congela a interface. A posse passa inteira para um worker; o
    // lambda vazio termina e libera os samples nessa thread.
    void reclaimZonesAsync (
        std::vector<std::unique_ptr<Zone>> zones) noexcept
    {
        if (zones.empty())
            return;

        try
        {
            auto owned = std::make_shared<
                std::vector<std::unique_ptr<Zone>>> (std::move (zones));

            if (startWorkerThread ([owned] {}))
                return;
        }
        catch (...)
        {
        }

        // Falha extrema ao criar o ticket/thread: o parametro ainda possui os
        // dados e sera liberado com seguranca no chamador. Jank e preferivel a
        // vazamento ou use-after-free nessa situacao de memoria esgotada.
    }

    void finishLayerLoad (int layerIndex,
                          std::vector<std::unique_ptr<Zone>> zones,
                          const juce::String& label,
                          const juce::String& library,
                          const juce::String& category,
                          int gen,
                          bool applyFactoryToneOnSuccess)
    {
        const juce::ScopedLock toneLock (layerToneTransactionLock);

        // geracao mudou: o canal foi limpo, trocado ou virou VSTi durante o
        // decode -- o resultado velho nao pode atropelar o estado atual
        if (gen != layerLoadGeneration[layerIndex].load())
        {
            reclaimZonesAsync (std::move (zones));
            sendChangeMessage();
            return;
        }

        // Decode vazio e rollback: o canal anterior continua sendo a verdade.
        // Restore de projeto nao perde o alvo porque ele vive nos campos
        // layerStateRestoreTarget* enquanto pending=true.
        if (! zones.empty())
        {
            // O timbre anterior continuou tocando e editavel durante todo o
            // decode. Capture os valores MAIS RECENTES somente quando sabemos
            // que a troca vai acontecer; pedido cancelado/falho nao altera nada.
            rememberOutgoingTimbreTone (layerIndex);

            // Resolve antes de suspender: o callback nunca le String, mas o
            // flag atomico precisa mudar junto com a troca do audio.
            juce::String cat = category;

            if (cat.isEmpty())
                for (const auto& e : entriesCache)
                    if (e.label == label)
                    {
                        cat = e.category;
                        break;
                    }

            const bool targetIsOrgan = looksLikeOrgan (label, cat);
            const float targetFamilyTrim =
                nativeFamilyTrimForCategory (cat);
            notifyHostedPluginWillRetire (
                HostedPluginLocation::instrument, layerIndex,
                getLayerInstrument (layerIndex));

            bool supersededByPlugin = false;
            {
                const juce::ScopedLock identityLock (
                    layerIdentityLock);

                // Destruir o editor antigo pode entregar uma selecao AU/VST
                // mais nova. Nesse caso ela vence e este decode nativo fica
                // stale sem apagar sua descricao/pending.
                if (pendingVstIds[layerIndex].isNotEmpty())
                {
                    loadingLabels[layerIndex].clear();
                    loadingLibs[layerIndex].clear();
                    supersededByPlugin = true;
                }
            }

            if (supersededByPlugin)
            {
                sendChangeMessage();
                return;
            }

            std::unique_ptr<juce::AudioPluginInstance> oldInstrument;
            bool oldInstrumentWasPrepared = false;
            bool retired = false;
            {
                const bool resumeAfterSwap = ! isSuspended();

                if (resumeAfterSwap)
                    suspendProcessing (true);
                const juce::ScopeGuard restoreProcessing {
                    [this, resumeAfterSwap]
                {
                    if (resumeAfterSwap)
                        suspendProcessing (false);
                } };

                auto old = std::move (layers[layerIndex].zones);
                instNotesOn[layerIndex] = 0;
                noteTranspose.clearLayer (layerIndex);
                pluginSustainMirror[layerIndex].store (
                    sustainStateUnknown,
                    std::memory_order_release);
                pluginPitchCentresPending[layerIndex].store (
                    false,
                    std::memory_order_release);
                oldInstrumentWasPrepared =
                    pluginPrepared[layerIndex].exchange (
                    false,
                    std::memory_order_acq_rel);
                bool oldOwnershipResolved = old.empty();
                const juce::ScopeGuard protectOldZones {
                    [this, &old, &oldOwnershipResolved]
                {
                    // Qualquer excecao depois do move precisa aposentar ou
                    // silenciar as vozes antes que o vetor local seja
                    // destruido e o callback volte (evita Voice::zone pendente).
                    if (! oldOwnershipResolved && ! old.empty())
                    {
                        (void) retireZonesWhileSuspended (old);
                        oldOwnershipResolved = true;
                    }
                } };
                {
                    const juce::ScopedLock pluginLock (
                        layerPluginStateLock);
                    oldInstrument = std::move (
                        layers[layerIndex].instrument);
                    {
                        // A promocao do alvo, a retirada do plugin e o fim dos
                        // fallbacks formam uma unica mudanca de identidade.
                        const juce::ScopedLock identityLock (
                            layerIdentityLock);
                        loadingLabels[layerIndex].clear();
                        loadingLibs[layerIndex].clear();
                        pendingVstIds[layerIndex].clear();
                        pendingVstStates[layerIndex].clear();
                        pendingVstDescriptionValid[layerIndex] = false;
                        layers[layerIndex].instrumentId.clear();
                        layers[layerIndex].instrumentName.clear();
                        layers[layerIndex].timbre = label;
                        layers[layerIndex].libraryName = library;
                    }
                }
                layers[layerIndex].zones = std::move (zones);
                layerCategory[layerIndex] = cat;
                layerIsOrgan[layerIndex].store (
                    targetIsOrgan,
                    std::memory_order_relaxed);
                layerIsChurchOrgan[layerIndex].store (
                    false, std::memory_order_release);
                layerChurchOrganFamily[layerIndex].store (
                    0, std::memory_order_release);
                lastChurchOrganChannelRotaryMode[layerIndex].store (
                    -1, std::memory_order_release);
                layerNativeFamilyTrim[layerIndex].store (
                    targetFamilyTrim,
                    std::memory_order_relaxed);
                rotarySpeakers[layerIndex].reset();
                churchOrganSustainMirror[layerIndex].store (
                    sustainStateUnknown,
                    std::memory_order_release);
                pendingChurchOrganStates[layerIndex].reset();
                churchOrganReverbBuffers[layerIndex].clear();
                setLayerLoaded (layerIndex, true);

                if (auto* organ =
                        layers[layerIndex].churchOrgan.get())
                {
                    organ->setProcessingSuspended (true);
                    (void) organ->reset();
                }

                // curva de velocity calibrada para ESTE timbre, montada com o
                // processamento suspenso (a thread de audio le esta tabela)
                buildVelocityLut (
                    layers[layerIndex].zones,
                    layers[layerIndex].velLut);

                // troca sem corte: as vozes antigas fazem fade suave e as notas
                // ainda seguradas renascem imediatamente no timbre novo
                int heldNotes[64], heldVels[64], heldChans[64];
                int numHeld = 0;

                for (auto& v : voices)
                    if (v.active && v.layerIndex == layerIndex)
                    {
                        // so renasce nota com a TECLA fisicamente pressionada;
                        // nota segurada apenas pelo pedal faz fade e morre
                        if (! v.released && ! v.heldByPedal
                             && juce::isPositiveAndBelow (v.note, 128)
                             && keyDown[v.note] != 0
                             && numHeld < 64)
                        {
                            int ch = v.channel;

                            if (! (v.channel >= 1 && v.channel <= 16
                                    && (keyDown[v.note]
                                         & (1u << ((v.channel - 1) & 15)))
                                           != 0))
                            {
                                ch = 1;

                                for (juce::uint16 bits = keyDown[v.note];
                                     (bits & 1u) == 0 && ch < 16;
                                     bits = (juce::uint16) (bits >> 1))
                                    ++ch;
                            }

                            heldNotes[numHeld] = v.note;
                            heldVels[numHeld] = v.velocity;
                            heldChans[numHeld] = ch;
                            ++numHeld;
                        }

                        v.heldByPedal = false;
                        releaseVoice (v);
                    }

                // So agora o novo timbre existe de verdade. Um timbre inedito
                // recebe o padrao historico; um editado recupera ENV/EQ.
                if (applyFactoryToneOnSuccess)
                {
                    applyFactoryTimbreShape (layerIndex, label, cat);
                    applyRememberedTimbreTone (
                        layerIndex, label, library);

                    // Estas mudancas pertencem a SELECAO que acabou de vencer
                    // a geracao. Pedido cancelado/falho nunca altera o canal A.
                    *layers[layerIndex].on = true;
                    *layers[layerIndex].mute = false;
                    *layers[layerIndex].solo = false;

                    if (cat == "PADS" || cat == "AMBIENCIAS")
                    {
                        *layers[layerIndex].reverbSend = 0.45f;
                        *layers[layerIndex].comp = 0.12f;
                        *layers[layerIndex].loop = true;
                    }
                }

                // Auto EQ pertence ao novo timbre e precisa trocar no mesmo
                // boundary dos samples; nenhum bloco usa a categoria anterior.
                updateLayerEq (layerIndex);
                manualEqs[layerIndex].syncToTargets (
                    layers[layerIndex].eqLow != nullptr
                        ? (float) *layers[layerIndex].eqLow : 0.0f,
                    layers[layerIndex].eqMid != nullptr
                        ? (float) *layers[layerIndex].eqMid : 0.0f,
                    layers[layerIndex].eqHigh != nullptr
                        ? (float) *layers[layerIndex].eqHigh : 0.0f);

                retired = ! old.empty()
                           && retireZonesWhileSuspended (old);
                oldOwnershipResolved = true;

                for (int i = 0; i < numHeld; ++i)
                    startVoiceForLayer (
                        layerIndex, heldNotes[i], heldVels[i],
                        heldChans[i]);
            }

            if (retired)
                schedulePurge();

            // Seleção nativa bem-sucedida substitui definitivamente qualquer
            // VST/AU (ou estado pendente dele). A instância antiga é destruída
            // fora da suspensão para não prender o callback em código de
            // terceiro.
            if (oldInstrumentWasPrepared)
                releasePluginResourcesNoThrow (
                    oldInstrument.get());
            oldInstrument.reset();

            completeLayerStateRestore (layerIndex);
        }
        else
        {
            const juce::ScopedLock identityLock (layerIdentityLock);
            loadingLabels[layerIndex].clear();
            loadingLibs[layerIndex].clear();
        }

        updateHostDisplay();
        sendChangeMessage();
    }

    void schedulePurge() noexcept
    {
        try
        {
            juce::WeakReference<TimbresProcessor> weakThis (this);

            juce::Timer::callAfterDelay (1500, [weakThis]
            {
                if (auto* self = weakThis.get())
                    self->purgeRetiredZones();
            });
        }
        catch (...)
        {
            // Aposentados continuam possuidos e seguros. Uma proxima troca ou o
            // destrutor tera nova oportunidade de libera-los.
        }
    }

    // libera os samples aposentados quando nenhuma voz os usa mais
    void purgeRetiredZones() noexcept
    {
        if (retiredZones.empty())
            return;

        bool audioSuspendedByThisScope = false;

        try
        {
            for (;;)
            {
                // Um conjunto por vez: mover um vector transfere apenas tres
                // ponteiros e nao aloca. A versao anterior criava outro vetor e
                // fazia reserve/push_back justamente sob pressao de memoria.
                std::vector<std::unique_ptr<Zone>> safeToDestroy;
                bool foundSafe = false;
                bool stillWaiting = false;

                const bool resumeAfterPurge =
                    ! isSuspended();

                if (resumeAfterPurge)
                {
                    suspendProcessing (true);
                    audioSuspendedByThisScope = true;
                }

                for (auto it = retiredZones.begin();
                     it != retiredZones.end(); ++it)
                {
                    bool inUse = false;

                    for (auto& voice : voices)
                    {
                        if (! voice.active)
                            continue;

                        for (const auto& zone : *it)
                            if (voice.zone == zone.get())
                            {
                                inUse = true;
                                break;
                            }

                        if (inUse)
                            break;
                    }

                    if (! inUse)
                    {
                        safeToDestroy = std::move (*it);
                        retiredZones.erase (it);
                        foundSafe = true;
                        break;
                    }
                }

                stillWaiting = ! retiredZones.empty();
                if (resumeAfterPurge)
                {
                    suspendProcessing (false);
                    audioSuspendedByThisScope = false;
                }

                // Destruicao pesada fora da suspensao E fora da UI.
                reclaimZonesAsync (std::move (safeToDestroy));

                if (! stillWaiting)
                    break;

                if (! foundSafe)
                {
                    schedulePurge();
                    break;
                }
            }
        }
        catch (...)
        {
            if (audioSuspendedByThisScope)
                suspendProcessing (false);

            if (! retiredZones.empty())
                schedulePurge();
        }
    }

    // Setup de fabrica: piano + pad + strings, o trio do louvor. Prefere os
    // timbres "vitrine" mas cai no primeiro da categoria se nao existirem.
    void applyShowroomIfPending()
    {
        if (! showroomPending)
            return;

        showroomPending = false;

        for (int l = 0; l < numLayers; ++l)
            if (getLayerTimbre (l).isNotEmpty() || layers[l].instrument != nullptr)
                return;   // algum estado chegou no meio do caminho: nao atropelar

        auto pick = [this] (const char* cat, const char* prefer) -> const TimbreEntry*
        {
            const TimbreEntry* first = nullptr;

            for (const auto& e : entriesCache)
                if (e.category == cat)
                {
                    if (e.label.containsIgnoreCase (prefer))
                        return &e;

                    if (first == nullptr)
                        first = &e;
                }

            return first;
        };

        const TimbreEntry* picks[] = { pick ("PIANOS", "Studio Grand"),
                                       pick ("PADS", "Airy"),
                                       pick ("STRINGS", "String") };

        for (int l = 0; l < 3; ++l)
            if (picks[l] != nullptr)
                loadTimbreIntoLayer (l, *picks[l], true);

        // Mix inicial audivel, mas com ataque do piano limpo. Isto roda apenas
        // na primeira abertura sem setup salvo; nunca altera uma sessao do
        // usuario.
        *nativeReverbPreset = 1; // Cloud
        *reverbReturn = 0.72f;
        *layers[0].reverbSend = picks[0] != nullptr ? 0.20f : 0.0f;
        *layers[1].reverbSend = picks[1] != nullptr ? 0.45f : 0.0f;
        *layers[2].reverbSend = picks[2] != nullptr ? 0.28f : 0.0f;

        if (picks[1] != nullptr)
        {
            *layers[1].comp = 0.12f;
            *layers[1].loop = true;
        }
    }

    // casa por rotulo E biblioteca (biblioteca vazia = qualquer uma)
    void loadLayerByLabel (int layerIndex, const juce::String& label, const juce::String& library = {})
    {
        if (label.isEmpty())
            return;

        // 1a passada: casa rotulo + biblioteca exata
        if (library.isNotEmpty())
            for (const auto& e : entriesCache)
                if (e.label == label && e.library == library)
                {
                    loadTimbreIntoLayer (layerIndex, e);
                    return;
                }

        // Estados antigos nao guardavam biblioteca, portanto so eles podem
        // recorrer ao primeiro rotulo homonimo. Se o projeto informou um pack,
        // nunca troque silenciosamente pelo mesmo nome de outro pack.
        if (library.isEmpty())
            for (const auto& e : entriesCache)
                if (e.label == label)
                {
                    loadTimbreIntoLayer (layerIndex, e);
                    return;
                }

        // AUTO-CURA: rotulo nao existe na biblioteca atual (drive desconectado?).
        // Guarda em loadingLabels/Libs, que o save re-emite -- o setup sobrevive
        // a qualquer numero de save/restart e cura quando o drive voltar. So no
        // caminho de FALHA: quando casa, loadTimbreIntoLayer assume os campos.
        if (juce::isPositiveAndBelow (layerIndex, numLayers))
        {
            const juce::ScopedLock lock (layerIdentityLock);
            loadingLabels[layerIndex] = label;
            loadingLibs[layerIndex] = library;
        }
    }

    // se a biblioteca ainda esta sendo escaneada, agenda o carregamento
    void requestLayerLoad (int layerIndex, const juce::String& label, const juce::String& library = {})
    {
        if (label.isEmpty() || ! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        if (entriesScanned)
            loadLayerByLabel (layerIndex, label, library);
        else
        {
            const juce::ScopedLock lock (layerIdentityLock);
            pendingLayerLabels[layerIndex] = label;
            pendingLayerLibs[layerIndex] = library;
        }
    }

    void drainPendingLayerLoads()
    {
        for (int l = 0; l < numLayers; ++l)
        {
            // Move primeiro: loadLayerByLabel pode iniciar callbacks/loads e nao
            // deve observar aliases nos slots que isBusyLoading consulta.
            juce::String label, library;
            {
                const juce::ScopedLock lock (layerIdentityLock);
                label = std::move (pendingLayerLabels[l]);
                library = std::move (pendingLayerLibs[l]);
                pendingLayerLabels[l].clear();
                pendingLayerLibs[l].clear();
            }

            if (label.isNotEmpty())
            {
                // Rotulo ausente sobrevive em loadingLabels/Libs e o save o
                // reemite. O slot pending, porem, sempre precisa ser drenado.
                loadLayerByLabel (l, label, library);
            }
        }
    }

    void finishEntriesScanFailure()
    {
        // Mantem o ultimo cache conhecido. Isto tambem cobre falha antes de
        // iniciar o worker: a splash nao pode esperar para sempre por pedidos
        // de restore que ficaram em pendingLayerLabels.
        entriesScanned = true;
        drainPendingLayerLoads();
        sendChangeMessage();
    }

    void finishEntriesScan (juce::Array<TimbreEntry> scanned)
    {
        entriesCache = std::move (scanned);
        entriesScanned = true;
        drainPendingLayerLoads();

       #if JUCE_ANDROID || JUCE_MAC
        // MODO DE TESTE: carrega timbres sozinho assim que a biblioteca termina
        // de ser lida.
        //
        // Existe porque a alternativa era automatizar TOQUES na tela para
        // escolher o instrumento, e isso falhava o tempo todo -- teclado virtual
        // abrindo por cima, texto acumulado no campo de busca, lista ainda
        // vazia. Cada falha dessas terminava numa medicao com "vozes 0", ou seja
        // uma hora de teste jogada fora medindo silencio.
        //
        // So roda com um marcador de diagnostico presente. Num aparelho de
        // comprador o arquivo nao existe.
        if (selfTestOn && ! selfTestRestore)
        {
            // Lista PROPRIA, com contagem propria: dimensiona-la por
            // numLayers deixava ponteiros nulos nos canais alem do quinto
            // (o stress test percorre todos e faria containsIgnoreCase
            // num nullptr). O ciclo repete os mesmos timbres, que e o
            // suficiente para medir carga com todos os canais soando.
            static const char* const wanted[] = {
                "Black Upright", "Pad2", "REV", "BELLP4", "Pad3"
            };
            constexpr int numWanted = (int) (sizeof (wanted) / sizeof (wanted[0]));

            const int layersToLoad = selfTestStress ? numLayers : 1;

            for (int l = 0; l < layersToLoad; ++l)
            {
                if (getLayerTimbre (l).isNotEmpty())
                    continue;

                for (const auto& e : entriesCache)
                    if (e.label.containsIgnoreCase (wanted[l % numWanted]))
                    {
                        juce::Logger::writeToLog ("AUDIO: teste carregando canal "
                                                   + juce::String (l + 1) + " = " + e.label);
                        loadTimbreIntoLayer (l, e);
                        break;
                    }
            }
        }
       #endif

        applyShowroomIfPending();
        sendChangeMessage();
    }

    const Zone* findZone (const Layer& layer, int note, int vel) const
    {
        for (const auto& z : layer.zones)
            if (note >= z->keyLo && note <= z->keyHi && vel >= z->velLo && vel <= z->velHi)
                return z.get();

        return nullptr;
    }

    // Pontuacao da vitima: quanto MENOR, melhor candidata a ser roubada.
    // Voz ja solta (ou so segura pelo pedal) e praticamente inaudivel vale menos
    // que uma voz nova sendo tocada com o dedo na tecla -- roubar a inaudivel nao
    // produz clique, que e o truque que faz o roubo da Nord passar despercebido.
    static float stealScore (const Voice& v)
    {
        float s = v.lastEnv * v.gain;

        if (v.released)     s *= 0.05f;
        if (v.heldByPedal)  s *= 0.25f;

        return s;
    }

    void releaseVoice (Voice& voice)
    {
        if (! voice.active || voice.released)
            return;

        const int layerIndex = juce::jlimit (
            0, numLayers - 1, voice.layerIndex);
        const auto& layer = layers[layerIndex];
        const float attack = ChurchDAWChannelTone::attackForPlayback (
            layer.attack != nullptr ? (float) *layer.attack : 0.003f);
        const float release = ChurchDAWChannelTone::releaseForNote (
            layer.release != nullptr ? (float) *layer.release : 0.55f,
            voice.soundingNote >= 0 ? voice.soundingNote : voice.note);

        // Le o RELEASE no momento em que a tecla/pedal solta. Assim o cliente
        // pode ajustar a cauda enquanto sustenta o acorde e ouvir o resultado
        // sem precisar disparar outra nota.
        voice.adsr.setParameters ({ attack, 0.0f, 1.0f, release });
        voice.released = true;
        voice.adsr.noteOff();
    }

    // Interpolacao cubica de Hermite (4 pontos). A linear que estava aqui antes
    // e o metodo mais barato e mais sujo que existe: como as zonas se esticam
    // pelas teclas vizinhas, quase toda nota tocada esta transposta, entao o
    // teclado inteiro passava por ela -- perdendo brilho e ganhando aliasing.
    // Versao SEM verificacao de limite, para quando o bloco inteiro ja foi
    // provado seguro antes do laco.
    //
    // A com verificacao faz 4 testes por chamada e e chamada DUAS vezes por
    // amostra (esquerda e direita): sao 8 desvios por amostra, por voz. Com 40
    // vozes e blocos de 1024, isso e mais de 300 mil desvios por bloco -- e
    // desvio dentro de laco quente impede o compilador de vetorizar. Medido
    // antes desta separacao: 115% do prazo com 40 vozes, ou seja o motor nao
    // entregava.
    static inline float hermiteFast (const float* s, int i, float f) noexcept
    {
        const float xm1 = s[i - 1];
        const float x0  = s[i];
        const float x1  = s[i + 1];
        const float x2  = s[i + 2];

        const float c = (x1 - xm1) * 0.5f;
        const float w = x0 - x1;
        const float a = w + c + (x2 - x0) * 0.5f;
        const float b = a + w + c;

        return ((a * f - b) * f + c) * f + x0;
    }

    static inline float hermite (const float* s, int i, float f, int len)
    {
        const float xm1 = s[i > 0 ? i - 1 : 0];
        const float x0  = s[i];
        const float x1  = s[i + 1 < len ? i + 1 : len - 1];
        const float x2  = s[i + 2 < len ? i + 2 : len - 1];

        const float c = (x1 - xm1) * 0.5f;
        const float w = x0 - x1;
        const float a = w + c + (x2 - x0) * 0.5f;
        const float b = a + w + c;

        return ((a * f - b) * f + c) * f + x0;
    }

    Voice* grabVoice (int layerIndex)
    {
        Voice* freeVoice = nullptr;
        Voice* victimInLayer = nullptr;
        Voice* victimAnywhere = nullptr;
        float bestInLayer = 1.0e9f, bestAnywhere = 1.0e9f;
        int usedInLayer = 0;

        for (auto& v : voices)
        {
            if (! v.active)
            {
                if (freeVoice == nullptr)
                    freeVoice = &v;

                continue;
            }

            const float s = stealScore (v);

            if (s < bestAnywhere) { bestAnywhere = s; victimAnywhere = &v; }

            if (v.layerIndex != layerIndex)
                continue;

            ++usedInLayer;

            if (s < bestInLayer) { bestInLayer = s; victimInLayer = &v; }
        }

        // orcamento da camada estourado: a vitima sai de DENTRO da propria camada
        if (usedInLayer >= voicesForLayer (layerIndex) && victimInLayer != nullptr)
            return victimInLayer;

        if (freeVoice != nullptr)
            return freeVoice;

        // pool global exausto (nao deveria acontecer): rouba a mais fraca de todas
        return victimAnywhere != nullptr ? victimAnywhere : &voices[0];
    }

    void startVoiceForLayer (int l, int note, int vel, int channel)
    {
        auto& layer = layers[l];

        // OFF e desligamento real: nao cria voz, nao abre FLAC e nao consome
        // streamer em silencio. Mute/solo/volume continuam pos-fader para que
        // possam ser alternados durante uma nota sustentada sem reinicia-la.
        if (layer.on == nullptr || ! (bool) *layer.on || layer.zones.empty())
            return;

        const int t = juce::jlimit (0, 127, note + (int) *layer.transp + (int) *globalTransp);
        const auto* zone = findZone (layer, t, juce::jlimit (1, 127, vel));

        if (zone == nullptr)
            return;

        auto* v = grabVoice (l);

        // ROUBO DE VOZ SEM ESTALO.
        //
        // Quando a polifonia estoura o orcamento, esta voz e tomada de uma nota
        // que ainda estava soando. Ate agora o corte era SECO: o sinal saltava do
        // valor em que estava para zero num unico sample, e um degrau assim e um
        // clique curto -- os "pipoquinhos" que aparecem justamente quando se toca
        // bastante, e que nenhuma medicao de prazo, estouro ou disco enxerga,
        // porque nao e nenhuma das tres coisas.
        //
        // A rampa abaixo continua de onde a nota parou e decai depressa, somada
        // por cima da nota nova (que comeca em zero por causa do ataque de 3 ms).
        // O degrau desaparece sem mascarar nada: sao poucos milissegundos.
        if (v->active)
        {
            v->declickL = v->lastOutL;
            v->declickR = v->lastOutR;
        }

        v->zone = zone;
        v->lastEnv = 0.0f;
        v->layerIndex = l;
        v->pos = 0.0;
        v->ratio = std::pow (2.0, (t - zone->rootNote) / 12.0) * (zone->sourceSampleRate / hostSampleRate);

        // Ganho de velocity CALIBRADO para este timbre (ver buildVelocityLut):
        // a curva completa o que falta de dinamica em vez de somar em cima da
        // que ja esta gravada no material. Uma leitura de tabela, sem pow().
        // Tonewheel/transistor organ has no velocity-dependent steady level.
        // Applying the legacy quadratic curve here made velocity 80 about
        // 7.4 dB quieter than a real organ key and forced Master to +10 dB.
        // Velocity remains available to hosted instruments and future click
        // articulation, but the native organ body is calibrated consistently.
        const bool organVoice =
            layerIsOrgan[l].load (std::memory_order_relaxed);
        const float velocityGain = organVoice
            ? organVelocityGain
            : layer.velLut[juce::jlimit (0, 127, vel)];
        const float familyTrim =
            layerNativeFamilyTrim[l].load (
                std::memory_order_relaxed);
        v->gain = velocityGain * polyphonyHeadroom * familyTrim;
        v->note = note;
        v->soundingNote = t;
        v->channel = channel;
        v->velocity = vel;
        v->released = false;
        v->heldByPedal = false;
        v->streamRecoveryGain = 1.0f;
        v->primeiraJanelaPedida = false;   // nota nova volta a poder pedir o disco
        v->startOrder = ++voiceCounter;
        v->adsr.setSampleRate (hostSampleRate);
        const float attack = ChurchDAWChannelTone::attackForPlayback (
            layer.attack != nullptr ? (float) *layer.attack : 0.003f);
        // Release continua respeitando o comportamento acustico por nota, mas
        // agora o valor central vem do knob/preset do timbre.
        const float rel = ChurchDAWChannelTone::releaseForNote (
            layer.release != nullptr ? (float) *layer.release : 0.55f, t);

        // OBRIGATORIO antes do noteOn: juce::ADSR::noteOn() com attack > 0 apenas
        // troca de estado, NAO zera o valor do envelope (juce_ADSR.h:130-146).
        // Como quatro caminhos devolvem voz ao pool com o envelope sujo -- fim do
        // sample deixa 1,0 exato, alem de stopAllVoices, stopLayerVoices e o roubo
        // de voz -- a nota seguinte que caisse nesse slot comecava do valor
        // antigo e pulava a rampa de ataque. Resultado: transiente diferente de
        // nota para nota, sem padrao. Sem esta linha o ataque nao e deterministico.
        v->adsr.reset();
        v->adsr.setParameters ({ attack, 0.0f, 1.0f, rel });
        v->adsr.noteOn();

       #if CHURCHDAW_STREAM_SAMPLES
        // Arma a janela desta voz e ja pede o trecho que vem DEPOIS do preload.
        // O pedido sai agora, no note-on, e nao quando a nota chegar la: a
        // thread de leitura ganha os ~150 ms do preload inteiro de vantagem.
        if (zone->streamed)
        {
            // Arma a janela, mas NAO pede nada ainda.
            //
            // Pedir aqui parecia esperto -- dava a thread de leitura os 2 s do
            // preload de vantagem. Na pratica era o contrario: a MAIORIA das
            // notas termina antes dos 2 s e nunca chega a usar o disco, entao
            // cada nota tocada enfileirava uma leitura de 1,5 s de audio que
            // seria jogada fora. Com acordes seguidos, esse trabalho inutil
            // atrasava as leituras que importam -- as das notas realmente
            // sustentadas. O pedido agora sai so quando a nota se aproxima do
            // fim do preload (ver o laco de renderizacao).
            auto& st = streamer.stream ((int) (v - voices));
            st.restart (zone->stream);
        }
        else
        {
            streamer.stream ((int) (v - voices)).stop();
        }
       #endif

        v->active = true;
    }

    bool layerAcceptsMidiChannel (int layer, int midiChannel) const noexcept
    {
        if (! juce::isPositiveAndBelow (layer, numLayers))
            return false;

        const int listen = layerMidiListenChannel[layer].load (
            std::memory_order_relaxed);
        // 0 = omni (todos os teclados / qualquer canal MIDI)
        return listen == 0 || listen == midiChannel;
    }

    void noteOn (int channel, int note, int vel)
    {
        if (juce::isPositiveAndBelow (note, 128))
        {
            keyDown[note] |= (juce::uint16) (1u << ((channel - 1) & 15));

            heldBits[note >> 5].fetch_or (1u << (note & 31), std::memory_order_relaxed);
        }

        for (int l = 0; l < numLayers; ++l)
            if (layerAcceptsMidiChannel (l, channel))
                startVoiceForLayer (l, note, vel, channel);
    }

    void noteOff (int channel, int note)
    {
        if (juce::isPositiveAndBelow (note, 128))
        {
            keyDown[note] &= (juce::uint16) ~(1u << ((channel - 1) & 15));

            // o bit do acorde so apaga quando o ULTIMO canal solta a nota
            if (keyDown[note] == 0)
                heldBits[note >> 5].fetch_and (~(1u << (note & 31)), std::memory_order_relaxed);
        }

        for (auto& v : voices)
            if (v.active && v.note == note && v.channel == channel)
            {
                if (pedalDown || (bool) *globalSustain)
                {
                    v.heldByPedal = true;
                }
                else
                {
                    releaseVoice (v);
                }
            }
    }

    void releasePedalHeldVoices()
    {
        for (auto& v : voices)
            if (v.active && v.heldByPedal)
            {
                v.heldByPedal = false;
                releaseVoice (v);
            }
    }

    void setPedal (bool down)
    {
        pedalDown = down;

        if (! down && ! (bool) *globalSustain)
            releasePedalHeldVoices();
    }

    void stopAllVoices()
    {
        pedalDown = false;

        // CC120/123 e PANIC são globais neste host. Limpar apenas as vozes
        // sampled deixava o FIFO de transpose dos instrumentos/órgãos vivo:
        // depois de mudar a transposição, o próximo note-off podia ser enviado
        // à nota antiga e prender a nova. O storage é fixo e RT-safe.
        for (auto& notesOn : instNotesOn)
            notesOn = 0;

        noteTranspose.clear();

        for (auto& sustainMirror :
                churchOrganSustainMirror)
            sustainMirror.store (
                sustainStateUnknown,
                std::memory_order_release);

        for (auto& sustainMirror :
                pluginSustainMirror)
            sustainMirror.store (
                sustainStateUnknown,
                std::memory_order_release);

        for (auto& k : keyDown)
            k = 0;

        for (auto& h : heldBits)
            h.store (0, std::memory_order_relaxed);

        for (auto& v : voices)
            silenceVoiceWithoutClick (v);
    }

    void stopLayerVoices (int layerIndex)
    {
        if (juce::isPositiveAndBelow (
                layerIndex, numLayers))
        {
            churchOrganSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
            pluginSustainMirror[layerIndex].store (
                sustainStateUnknown,
                std::memory_order_release);
        }

        for (auto& v : voices)
            if (v.layerIndex == layerIndex)
                silenceVoiceWithoutClick (v);
    }

    // Desliga a voz somando o ponto onde ela parou na rampa de declique do
    // CANAL. Sem isto, PANIC e troca de timbre cortam o som num degrau seco --
    // e um degrau e um estalo, ainda que curto. A rampa e do canal, e nao da
    // voz, porque a voz para de ser renderizada no mesmo instante.
    void silenceVoiceWithoutClick (Voice& v) noexcept
    {
        if (v.active && juce::isPositiveAndBelow (v.layerIndex, numLayers))
        {
            layerDeclickL[v.layerIndex] += v.lastOutL;
            layerDeclickR[v.layerIndex] += v.lastOutR;
        }

       #if CHURCHDAW_STREAM_SAMPLES
        streamer.stream ((int) (&v - voices)).stop();
       #endif

        v.active = false;
        v.declickL = v.declickR = 0.0f;
        v.lastOutL = v.lastOutR = 0.0f;
    }

    void renderVoices (juce::AudioBuffer<float>& buffer)
    {
       #if JUCE_ANDROID
        const auto renderStart = juce::Time::getHighResolutionTicks();
       #endif

        const int numSamples = buffer.getNumSamples();
        const int numOut = juce::jmin (2, buffer.getNumChannels());
        const float masterGain = (float) *gain;

        const bool anySolo = hasAnyLoadedSolo();

        // buffers por canal (crescem junto com o bloco do driver - sem isso
        // um bloco maior que o preparado corrompia o audio)
        for (auto& a : layerHasAudio)
            a = false;

        // A capacidade de 8192 foi reservada em prepareToPlay(), mas o tamanho
        // LÓGICO entregue ao motor precisa ser exatamente o bloco atual.
        // avoidReallocating preserva o storage e torna 192/384/512/1024 apenas
        // mudanças de view, sem alocação no callback.
        for (auto& b : layerBuffers)
        {
            b.setSize (
                2, numSamples, false, false, true);
            b.clear();
        }

        for (auto& b : churchOrganReverbBuffers)
        {
            b.setSize (
                2, numSamples, false, false, true);
            b.clear();
        }

        // Fontes sintetizadas Church Organ e VSTi recebem o MIDI do bloco com
        // transposição sample-accurate. O sampler legado continua no caminho
        // de vozes logo abaixo.
        for (int l = 0; l < numLayers; ++l)
        {
            auto& lay = layers[l];
            const bool isChurchOrgan =
                layerIsChurchOrgan[l].load (
                    std::memory_order_acquire);

            if (isChurchOrgan)
            {
                auto* organ = lay.churchOrgan.get();

                if (organ == nullptr
                     || ! organ->diagnostics().prepared)
                {
                    instNotesOn[l] = 0;
                    continue;
                }

                const int semis =
                    (int) *lay.transp + (int) *globalTransp;
                const bool forceGlobalSustain =
                    (bool) *globalSustain;
                layerMidi.clear();

                bool physicalSustain =
                    pedalDownAtBlockStart;
                int sustainMirror =
                    churchOrganSustainMirror[l].load (
                        std::memory_order_acquire);

                // O adapter usa canais 1/2/3 para upper/lower/pedal. Este
                // helper só escreve no MidiBuffer já pré-reservado; não aloca
                // nem bloqueia a thread de áudio.
                const auto queueOrganSustain =
                    [&] (bool down, int sampleOffset) noexcept
                {
                    for (int channel = 1;
                         channel <= 3;
                         ++channel)
                    {
                        layerMidi.addEvent (
                            juce::MidiMessage::controllerEvent (
                                channel, 64,
                                down ? 127 : 0),
                            sampleOffset);
                    }
                };

                const bool sustainAtBlockStart =
                    forceGlobalSustain || physicalSustain;
                const int wantedStartState =
                    sustainAtBlockStart
                        ? sustainStateOn
                        : sustainStateOff;

                // Load, reprepare, reset e PANIC deixam o espelho UNKNOWN.
                // Portanto um sustain global já ligado é enviado no primeiro
                // bloco, em vez de a igualdade booleana ocultar o CC64.
                if (sustainMirror != wantedStartState)
                {
                    queueOrganSustain (
                        sustainAtBlockStart, 0);
                    sustainMirror = wantedStartState;
                }

                // O adapter executa CC120/CC123 por ultimo entre eventos que
                // compartilham o mesmo sample. O ledger do host precisa imitar
                // exatamente essa ordem: limpar no instante em que encontra o
                // CC faria um note-on posterior no mesmo offset parecer ativo,
                // embora o motor o silencie no fim daquele sample.
                int pendingOrganAllOffOffset = -1;
                bool pendingOrganAllSoundOff = false;
                bool pendingOrganAllNotesOff = false;
                bool pendingOrganPitchCentres = false;
                bool pendingOrganPreservePhysicalSustain = false;
                const auto flushOrganAllOffLedger =
                    [&] () noexcept
                {
                    if (pendingOrganAllOffOffset < 0)
                        return;

                    if (pendingOrganPitchCentres)
                        for (int channel = 1;
                             channel <= 3;
                             ++channel)
                        {
                            layerMidi.addEvent (
                                juce::MidiMessage::pitchWheel (
                                    channel,
                                    ChurchDAWPerformanceMidiSafety::
                                        PitchBendGuard::centreValue),
                                pendingOrganAllOffOffset);
                        }

                    for (int channel = 1;
                         channel <= 3;
                         ++channel)
                    {
                        if (pendingOrganAllSoundOff)
                            layerMidi.addEvent (
                                juce::MidiMessage::allSoundOff (
                                    channel),
                                pendingOrganAllOffOffset);

                        if (pendingOrganAllNotesOff)
                            layerMidi.addEvent (
                                juce::MidiMessage::allNotesOff (
                                    channel),
                                pendingOrganAllOffOffset);
                    }

                    const bool physicalSustainAfterBoundary =
                        pendingOrganPreservePhysicalSustain
                            ? physicalSustain
                            : false;
                    const bool sustainAfterBoundary =
                        forceGlobalSustain
                        || physicalSustainAfterBoundary;

                    instNotesOn[l] = 0;
                    noteTranspose.clearLayer (l);
                    physicalSustain =
                        physicalSustainAfterBoundary;

                    if (sustainAfterBoundary
                         && pendingOrganAllOffOffset + 1
                              < numSamples)
                    {
                        queueOrganSustain (
                            true,
                            pendingOrganAllOffOffset + 1);
                        sustainMirror =
                            sustainStateOn;
                    }
                    else if (sustainAfterBoundary)
                    {
                        sustainMirror =
                            sustainStateUnknown;
                    }
                    else
                    {
                        sustainMirror =
                            sustainStateOff;
                    }

                    pendingOrganAllOffOffset = -1;
                    pendingOrganAllSoundOff = false;
                    pendingOrganAllNotesOff = false;
                    pendingOrganPitchCentres = false;
                    pendingOrganPreservePhysicalSustain =
                        false;
                };

                for (const auto meta : midiCopy)
                {
                    auto m = meta.getMessage();
                    // Teclado dedicado a outro canal: ignore notes/CC de canal
                    // MIDI (panic global e sustain global seguem o path nativo).
                    if ((m.isNoteOnOrOff() || m.isAftertouch()
                          || m.isPitchWheel()
                          || (m.isController()
                               && m.getControllerNumber() != 64
                               && m.getControllerNumber() != 120
                               && m.getControllerNumber() != 121
                               && m.getControllerNumber() != 123))
                         && ! layerAcceptsMidiChannel (l, m.getChannel()))
                        continue;

                    const int eventOffset = juce::jlimit (
                        0, juce::jmax (0, numSamples - 1),
                        (int) meta.samplePosition);

                    if (pendingOrganAllOffOffset >= 0
                         && eventOffset
                              != pendingOrganAllOffOffset)
                        flushOrganAllOffLedger();

                    if (m.isNoteOn())
                    {
                        const int inputNote = m.getNoteNumber();
                        const int outputNote = juce::jlimit (
                            0, 127, inputNote + semis);

                        // O ledger possui capacidade fixa para permanecer
                        // lock/allocation-free no callback. Se um controlador
                        // defeituoso exceder oito NOTE ON iguais sem NOTE OFF,
                        // falhe fechado: não envie a nona voz e silencie a
                        // camada no fim desta amostra.
                        if (! noteTranspose.recordNoteOn (
                                l, m.getChannel(), inputNote,
                                outputNote))
                        {
                            queueOrganSustain (
                                false, eventOffset);

                            pendingOrganAllOffOffset =
                                eventOffset;
                            pendingOrganAllSoundOff = true;
                            pendingOrganAllNotesOff = true;
                            pendingOrganPitchCentres = true;
                            pendingOrganPreservePhysicalSustain =
                                true;

                            // A saturação só ocorre com uma fonte MIDI
                            // patológica. Um PANIC global no próximo bloco
                            // limpa também inserts, sampler e o estado interno
                            // do PitchBendGuard, em vez de arriscar uma voz
                            // sem ledger em qualquer destino.
                            panicPending.store (
                                true,
                                std::memory_order_release);
                            continue;
                        }

                        m.setNoteNumber (outputNote);
                        ++instNotesOn[l];
                    }
                    else if (m.isNoteOff())
                    {
                        const int inputNote = m.getNoteNumber();
                        m.setNoteNumber (
                            noteTranspose.translatedNoteOff (
                                l, m.getChannel(), inputNote));
                        instNotesOn[l] =
                            juce::jmax (0, instNotesOn[l] - 1);
                    }
                    else if (m.isController())
                    {
                        const int controller =
                            m.getControllerNumber();

                        if (controller == 120
                             || controller == 123)
                        {
                            // A fronteira é global nos três manuais e fica
                            // pendente até o fim da amostra, garantindo que
                            // nenhum PitchWheel/evento ordinário posterior
                            // sobreviva depois dela.
                            physicalSustain = false;
                            queueOrganSustain (
                                false, eventOffset);
                            pendingOrganAllOffOffset =
                                eventOffset;
                            pendingOrganPreservePhysicalSustain =
                                false;

                            if (controller == 120)
                                pendingOrganAllSoundOff = true;
                            else
                                pendingOrganAllNotesOff = true;

                            continue;
                        }

                        if (controller == 64)
                        {
                            physicalSustain =
                                m.getControllerValue() >= 64;

                            // O pedal do host é global, portanto os três
                            // manuais precisam receber o mesmo estado. Isso
                            // também mantém o espelho por layer verdadeiro.
                            queueOrganSustain (
                                forceGlobalSustain
                                    || physicalSustain,
                                eventOffset);
                            sustainMirror =
                                forceGlobalSustain
                                     || physicalSustain
                                    ? sustainStateOn
                                    : sustainStateOff;
                            continue;
                        }
                        else if (controller == 121)
                        {
                            // Reset All Controllers desliga o latch no motor.
                            // Como o pedal do host e global, replique o reset
                            // para os tres manuais e reaplique o sustain do
                            // toggle somente DEPOIS dos resets.
                            physicalSustain = false;
                            sustainMirror =
                                sustainStateOff;

                            for (int channel = 1;
                                 channel <= 3;
                                 ++channel)
                            {
                                layerMidi.addEvent (
                                    juce::MidiMessage::controllerEvent (
                                        channel, 121,
                                        m.getControllerValue()),
                                    eventOffset);
                            }

                            queueOrganSustain (
                                forceGlobalSustain,
                                eventOffset);
                            sustainMirror =
                                forceGlobalSustain
                                    ? sustainStateOn
                                    : sustainStateOff;
                            continue;
                        }
                    }

                    layerMidi.addEvent (m, meta.samplePosition);
                }

                flushOrganAllOffLedger();

                churchOrganSustainMirror[l].store (
                    sustainMirror,
                    std::memory_order_release);

                if (instNotesOn[l] > 0
                     && ! pedalDown
                     && ! forceGlobalSustain
                     && ! anyKeyDown())
                {
                    const int failClosedOffset =
                        juce::jmax (0, numSamples - 1);

                    queueOrganSustain (
                        false, failClosedOffset);

                    for (int channel = 1; channel <= 3; ++channel)
                        layerMidi.addEvent (
                            juce::MidiMessage::pitchWheel (
                                channel,
                                ChurchDAWPerformanceMidiSafety::
                                    PitchBendGuard::centreValue),
                            failClosedOffset);

                    // Todos os centros precisam preceder a fronteira terminal
                    // em todos os manuais. Intercalar centro/CC por canal ainda
                    // deixava centros posteriores a um CC120 anterior.
                    for (int channel = 1; channel <= 3; ++channel)
                    {
                        layerMidi.addEvent (
                            juce::MidiMessage::allSoundOff (
                                channel),
                            failClosedOffset);
                        layerMidi.addEvent (
                            juce::MidiMessage::allNotesOff (
                                channel),
                            failClosedOffset);
                    }

                    instNotesOn[l] = 0;
                    noteTranspose.clearLayer (l);
                    churchOrganSustainMirror[l].store (
                        sustainStateOff,
                        std::memory_order_release);

                    // O contador local detectou uma voz que o estado físico já
                    // considera solta. O mesmo retrigger ambíguo pode ter ficado
                    // no guard global, sampler, outro plugin ou insert: use a
                    // única fronteira fail-closed para reconciliar tudo.
                    panicPending.store (
                        true,
                        std::memory_order_release);
                }

                // O mesmo estado de UI alimenta a família ativa. Tonewheel e
                // Worship consomem estes parâmetros no RotaryCabinet interno;
                // Transistor e Pipe os consomem no único ChannelRotarySpeaker
                // aplicado mais abaixo.
                const int channelRotaryMode =
                    lay.rotaryMode != nullptr
                        ? lay.rotaryMode->getIndex()
                        : ChurchDAWRotary::Speaker::stopMode;

                if (lastChurchOrganChannelRotaryMode[l].exchange (
                        channelRotaryMode,
                        std::memory_order_acq_rel)
                     != channelRotaryMode)
                {
                    const float organRotaryMode =
                        channelRotaryMode
                            == ChurchDAWRotary::Speaker::slowMode
                                ? 1.0f
                            : channelRotaryMode
                                == ChurchDAWRotary::Speaker::fastMode
                                ? 2.0f
                                : 0.0f;
                    organ->setParameterPlain (
                        churchorgan::OrganParameterId::rotaryMode,
                        organRotaryMode);
                }

                organ->setParameterPlain (
                    churchorgan::OrganParameterId::rotaryMix,
                    lay.rotaryAmount != nullptr
                        ? (float) *lay.rotaryAmount
                        : 0.0f);

                // ATK/REL do strip agora controla de verdade a articulacao das
                // quatro familias Church Organ. Os defaults historicos
                // (3 ms / 550 ms) viram o sentinel "articulacao nativa", entao
                // projetos antigos permanecem bit-a-bit iguais; qualquer
                // ajuste do cliente publica coeficientes RT-safe no motor.
                organ->setChannelPerformanceEnvelopeSeconds (
                    lay.attack != nullptr
                        ? (float) *lay.attack
                        : churchorgan::
                              ChurchOrganLayerProcessor::
                                  nativeChannelAttackSeconds,
                    lay.release != nullptr
                        ? (float) *lay.release
                        : churchorgan::
                              ChurchOrganLayerProcessor::
                                  nativeChannelReleaseSeconds);

                auto& organSend =
                    churchOrganReverbBuffers[l];
                organSend.setSize (
                    2, numSamples, false, false, true);
                organSend.clear();

                const bool rendered = organ->processBlock (
                    layerBuffers[l], layerMidi, &organSend);

                if (! rendered)
                {
                    layerBuffers[l].clear();
                    organSend.clear();
                }

                layerHasAudio[l] = rendered;
                continue;
            }

            if (lay.instrument == nullptr
                 || ! pluginPrepared[l].load (
                        std::memory_order_acquire))
            {
                instNotesOn[l] = 0;
                noteTranspose.clearLayer (l);
                pluginSustainMirror[l].store (
                    sustainStateUnknown,
                    std::memory_order_release);
                pluginPitchCentresPending[l].store (
                    false,
                    std::memory_order_release);
                continue;
            }

            const int semis = (int) *lay.transp + (int) *globalTransp;
            const bool forceGlobalSustain =
                (bool) *globalSustain;

            layerMidi.clear();

            // O estado inicial de pitch é propriedade da instância recém-criada,
            // não do barramento MIDI compartilhado. Assim um hot-load neste
            // canal recebe 16 centros antes de qualquer evento, sem recentralizar
            // notas sustentadas nos outros canais VST/AU.
            if (pluginPitchCentresPending[l].exchange (
                    false,
                    std::memory_order_acq_rel))
                for (int channel = 1; channel <= 16; ++channel)
                    layerMidi.addEvent (
                        juce::MidiMessage::pitchWheel (
                            channel,
                            ChurchDAWPerformanceMidiSafety::
                                PitchBendGuard::centreValue),
                        0);

            bool physicalSustain =
                pedalDownAtBlockStart;
            int sustainMirror =
                pluginSustainMirror[l].load (
                    std::memory_order_acquire);

            const auto queuePluginSustain =
                [&] (bool down, int sampleOffset) noexcept
            {
                for (int channel = 1;
                     channel <= 16;
                     ++channel)
                {
                    layerMidi.addEvent (
                        juce::MidiMessage::controllerEvent (
                            channel, 64, down ? 127 : 0),
                        sampleOffset);
                }
            };

            const bool sustainAtBlockStart =
                forceGlobalSustain || physicalSustain;
            const int wantedStartState =
                sustainAtBlockStart
                    ? sustainStateOn
                    : sustainStateOff;

            // Um plugin novo/repreparado começa com espelho UNKNOWN. Enviar
            // CC64 de verdade no primeiro bloco cobre inclusive o caso em que
            // o toggle global já estava ligado antes do load.
            if (sustainMirror != wantedStartState)
            {
                queuePluginSustain (
                    sustainAtBlockStart, 0);
                sustainMirror = wantedStartState;
            }

            int pendingPluginAllOffOffset = -1;
            bool pendingPluginAllSoundOff = false;
            bool pendingPluginAllNotesOff = false;
            bool pendingPluginPitchCentres = false;
            bool pendingPluginPreservePhysicalSustain = false;

            const auto flushPluginAllOff =
                [&] () noexcept
            {
                if (pendingPluginAllOffOffset < 0)
                    return;

                // Sustain OFF precede o boundary. CC120/123 são então os
                // últimos eventos desta amostra em todos os 16 canais.
                queuePluginSustain (
                    false, pendingPluginAllOffOffset);

                if (pendingPluginPitchCentres)
                    for (int channel = 1;
                         channel <= 16;
                         ++channel)
                    {
                        layerMidi.addEvent (
                            juce::MidiMessage::pitchWheel (
                                channel,
                                ChurchDAWPerformanceMidiSafety::
                                    PitchBendGuard::centreValue),
                            pendingPluginAllOffOffset);
                    }

                for (int channel = 1;
                     channel <= 16;
                     ++channel)
                {
                    if (pendingPluginAllSoundOff)
                        layerMidi.addEvent (
                            juce::MidiMessage::allSoundOff (
                                channel),
                            pendingPluginAllOffOffset);

                    if (pendingPluginAllNotesOff)
                        layerMidi.addEvent (
                            juce::MidiMessage::allNotesOff (
                                channel),
                            pendingPluginAllOffOffset);
                }

                const bool preservePhysicalSustain =
                    pendingPluginPreservePhysicalSustain;
                const bool physicalSustainAfterBoundary =
                    preservePhysicalSustain
                        ? physicalSustain
                        : false;
                const bool sustainAfterBoundary =
                    forceGlobalSustain
                    || physicalSustainAfterBoundary;

                instNotesOn[l] = 0;
                noteTranspose.clearLayer (l);
                physicalSustain =
                    physicalSustainAfterBoundary;

                // O sustain do toggle pertence ao host, não ao controlador
                // resetado. Em overflow interno, o pedal físico continua
                // realmente abaixado e também deve ser restaurado. Reative
                // apenas na amostra seguinte para nunca cancelar o all-off.
                if (sustainAfterBoundary
                     && pendingPluginAllOffOffset + 1
                          < numSamples)
                {
                    queuePluginSustain (
                        true,
                        pendingPluginAllOffOffset + 1);
                    sustainMirror = sustainStateOn;
                }
                else if (sustainAfterBoundary)
                {
                    sustainMirror =
                        sustainStateUnknown;
                }
                else
                {
                    sustainMirror =
                        sustainStateOff;
                }

                pendingPluginAllOffOffset = -1;
                pendingPluginAllSoundOff = false;
                pendingPluginAllNotesOff = false;
                pendingPluginPitchCentres = false;
                pendingPluginPreservePhysicalSustain =
                    false;
            };

            for (const auto meta : midiCopy)
            {
                auto m = meta.getMessage();
                if ((m.isNoteOnOrOff() || m.isAftertouch()
                      || m.isPitchWheel()
                      || (m.isController()
                           && m.getControllerNumber() != 64
                           && m.getControllerNumber() != 120
                           && m.getControllerNumber() != 121
                           && m.getControllerNumber() != 123))
                     && ! layerAcceptsMidiChannel (l, m.getChannel()))
                    continue;

                const int eventOffset = juce::jlimit (
                    0, juce::jmax (0, numSamples - 1),
                    (int) meta.samplePosition);

                if (pendingPluginAllOffOffset >= 0
                     && eventOffset
                          != pendingPluginAllOffOffset)
                    flushPluginAllOff();

                if (m.isController()
                     && (m.getControllerNumber() == 120
                          || m.getControllerNumber() == 123))
                {
                    const int controller =
                        m.getControllerNumber();

                    // O guard central já move all-off para depois dos demais
                    // eventos da mesma amostra. Mantemos um boundary local
                    // também, pois VST/AU precisam receber o controlador em
                    // TODOS os canais e o ledger deve limpar na mesma ordem.
                    pendingPluginAllOffOffset =
                        eventOffset;
                    pendingPluginPreservePhysicalSustain =
                        false;

                    if (controller == 120)
                        pendingPluginAllSoundOff = true;
                    else
                        pendingPluginAllNotesOff = true;

                    continue;
                }

                if (m.isNoteOn())
                {
                    const int n = m.getNoteNumber();
                    const int out = juce::jlimit (0, 127, n + semis);

                    if (! noteTranspose.recordNoteOn (
                            l, m.getChannel(), n, out))
                    {
                        // Storage fixo saturado: não deixe uma nota sem
                        // correspondência entrar no plugin. O all-off global
                        // no fim desta amostra elimina as oito vozes antigas.
                        pendingPluginAllOffOffset =
                            eventOffset;
                        pendingPluginAllSoundOff = true;
                        pendingPluginAllNotesOff = true;
                        pendingPluginPitchCentres = true;
                        pendingPluginPreservePhysicalSustain =
                            true;

                        panicPending.store (
                            true,
                            std::memory_order_release);
                        continue;
                    }

                    m.setNoteNumber (out);
                    ++instNotesOn[l];
                }
                else if (m.isNoteOff())
                {
                    // Usa a transposicao do NOTE ON, nao a de agora: mexer no
                    // transpose com a nota presa fazia o note-off chegar noutra
                    // tecla, e a original ficava tocando pra sempre no Kontakt.
                    const int n = m.getNoteNumber();
                    m.setNoteNumber (noteTranspose.translatedNoteOff (
                        l, m.getChannel(), n));
                    instNotesOn[l] = juce::jmax (0, instNotesOn[l] - 1);
                }
                else if (m.isController())
                {
                    const int controller =
                        m.getControllerNumber();

                    if (controller == 64)
                    {
                        // O pedal físico e o toggle são globais no ChurchDAW.
                        // Espelhar em 16 canais evita sustain preso quando o
                        // teclado e o preset do plugin usam canais distintos.
                        physicalSustain =
                            m.getControllerValue() >= 64;
                        queuePluginSustain (
                            forceGlobalSustain
                                || physicalSustain,
                            eventOffset);
                        sustainMirror =
                            forceGlobalSustain
                                 || physicalSustain
                                ? sustainStateOn
                                : sustainStateOff;
                        continue;
                    }

                    if (controller == 121)
                    {
                        physicalSustain = false;
                        sustainMirror =
                            sustainStateOff;

                        for (int channel = 1;
                             channel <= 16;
                             ++channel)
                        {
                            layerMidi.addEvent (
                                juce::MidiMessage::controllerEvent (
                                    channel, 121,
                                    m.getControllerValue()),
                                eventOffset);
                        }

                        queuePluginSustain (
                            forceGlobalSustain,
                            eventOffset);
                        sustainMirror =
                            forceGlobalSustain
                                ? sustainStateOn
                                : sustainStateOff;
                        continue;
                    }
                }

                layerMidi.addEvent (m, eventOffset);
            }

            flushPluginAllOff();
            pluginSustainMirror[l].store (
                sustainMirror,
                std::memory_order_release);

            // Rede de seguranca: sem tecla pressionada e sem pedal nao PODE sobrar
            // nota tocando. Se sobrou, algum note-off se perdeu (suspendProcessing
            // durante uma troca de timbre engole eventos do bloco) — calamos o
            // plugin em vez de deixar a nota presa no meio do culto.
            if (instNotesOn[l] > 0
                 && ! pedalDown
                 && ! forceGlobalSustain
                 && ! anyKeyDown())
            {
                const int failClosedOffset =
                    juce::jmax (0, numSamples - 1);

                queuePluginSustain (
                    false, failClosedOffset);

                for (int ch = 1; ch <= 16; ++ch)
                    layerMidi.addEvent (
                        juce::MidiMessage::pitchWheel (
                            ch,
                            ChurchDAWPerformanceMidiSafety::
                                PitchBendGuard::centreValue),
                        failClosedOffset);

                // Boundary global determinístico: primeiro centralize os 16
                // canais, só então emita CC120/123 terminais.
                for (int ch = 1; ch <= 16; ++ch)
                {
                    layerMidi.addEvent (
                        juce::MidiMessage::allSoundOff (ch),
                        failClosedOffset);
                    layerMidi.addEvent (
                        juce::MidiMessage::allNotesOff (ch),
                        failClosedOffset);
                }

                instNotesOn[l] = 0;
                noteTranspose.clearLayer (l);
                pluginSustainMirror[l].store (
                    sustainStateOff,
                    std::memory_order_release);

                panicPending.store (
                    true,
                    std::memory_order_release);
            }

            auto* inst = lay.instrument.get();
            const int nCh = juce::jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels());
            instrumentBuf.setSize (nCh, numSamples, false, false, true);
            instrumentBuf.clear();
            inst->processBlock (instrumentBuf, layerMidi);

            // plugin que explodiu (NaN/inf) nao pode contaminar o delay/EQ do
            // canal. Teste pela SOMA: o min/max SIMD do getMagnitude engole NaN
            // no MEIO do buffer; na soma qualquer NaN/Inf contamina o total.
            for (int ch = 0; ch < juce::jmin (2, nCh); ++ch)
            {
                const float* chk = instrumentBuf.getReadPointer (ch);
                float sum = 0.0f;

                for (int s = 0; s < numSamples; ++s)
                    sum += chk[s];

                if (! std::isfinite (sum))
                {
                    instrumentBuf.clear();
                    break;
                }
            }

            // copia a saida estereo principal do plugin para o canal
            // (VSTi de saida mono duplica o esquerdo -- o canal 1 do scratch e silencio)
            auto& lb = layerBuffers[l];
            const int rightSrc = inst->getTotalNumOutputChannels() > 1 ? 1 : 0;
            lb.copyFrom (0, 0, instrumentBuf, 0, 0, numSamples);
            lb.copyFrom (1, 0, instrumentBuf, rightSrc, 0, numSamples);
            layerHasAudio[l] = true;
        }

        for (auto& v : voices)
        {
            if (! v.active || v.zone == nullptr)
                continue;

           #if CHURCHDAW_STREAM_SAMPLES
            // Mantem a janela travada SOMENTE durante este bloco. O destrutor
            // libera tambem em todos os caminhos com continue abaixo.
            SampleStreamer::VoiceStream::ReadAccess streamReadAccess;
           #endif

            const auto& d = v.zone->data;
            const int srcChans = d.getNumChannels();

            // No streaming "len" e o comprimento REAL do sample no disco, nao o
            // do pedaco residente: e ele que diz onde a nota termina.
           #if CHURCHDAW_STREAM_SAMPLES
            const int len = v.zone->streamed ? v.zone->streamTotal : d.getNumSamples();
           #else
            const int len = d.getNumSamples();
           #endif
            auto& lb = layerBuffers[v.layerIndex];
            const bool loopOn = (bool) *layers[v.layerIndex].loop && v.zone->hasLoop;
            const double loopStart = (double) v.zone->loopStart;
            const double loopEnd   = (double) v.zone->loopEnd;
            const double loopLen   = loopEnd - loopStart;
            const int    xfade     = v.zone->loopXfade;
            const double xfadeFrom = loopEnd - (double) xfade;
            const double fadeLen = juce::jmin (2000.0, len * 0.1);   // declique no fim do sample

            // nota exatamente na root note: nao ha o que interpolar, le direto.
            // sai mais barato E mais limpo que passar pela cubica.
            const bool noInterp = std::abs (v.ratio - 1.0) < 1.0e-9;

            // ponteiros FORA do laco de amostras: antes o getReadPointer e o
            // addSample eram chamados 2x por amostra por voz (dezenas de milhares
            // de chamadas por bloco so pra achar o mesmo endereco)
            const float* srcL = d.getReadPointer (0);
            const float* srcR = d.getReadPointer (juce::jmin (1, srcChans - 1));

            // Indice base: a amostra "srcBase" do sample e a posicao 0 do
            // ponteiro acima. Zero enquanto lemos o pedaco residente; no
            // streaming passa a ser o inicio da janela.
            int srcBase = 0;
            int srcSpan = d.getNumSamples();

           #if CHURCHDAW_STREAM_SAMPLES
            if (v.zone->streamed)
            {
                auto& st = streamer.stream ((int) (&v - voices));
                const int preloadLen = d.getNumSamples();

                // quanto do sample este bloco vai percorrer (+4 pela hermite)
                const int span = (int) std::ceil ((double) numSamples * std::abs (v.ratio)) + 4;

                if (v.pos + span < (double) preloadLen)
                {
                    // Ainda dentro do pedaco residente. Se a nota esta chegando
                    // perto do fim dele, pede AGORA o trecho seguinte -- meio
                    // segundo de antecedencia e muito mais do que o disco
                    // precisa, e so as notas que realmente sustentam pagam esse
                    // custo. Notas curtas morrem antes daqui e nunca tocam o
                    // disco.
                    // PEDIR UMA VEZ, NAO A CADA BLOCO.
                    //
                    // A condicao abaixo fica verdadeira durante ~0,5 s de cada
                    // nota sustentada. Como request() se re-arma assim que a
                    // leitura anterior termina e fillImpl nao verifica se a
                    // janela ja esta carregada, o mesmo trecho era relido a
                    // cada bloco: a thread de disco nunca chegava ao wait() e
                    // saturava um nucleo, roubando CPU do callback de audio.
                    // No celular isso virava chiado e "FALHA-STREAM".
                    if (! v.primeiraJanelaPedida
                         && v.pos > (double) preloadLen - hostSampleRate * 0.5)
                    {
                        // A primeira janela tambem sobrepoe o fim do preload.
                        // Se ela comecar exatamente em preloadLen, o bloco que
                        // cruza essa fronteira nao cabe inteiro em nenhum dos
                        // dois buffers e vira um bloco de silencio audivel.
                        //
                        // So marca como pedida quando o pedido ENTROU. Se ele
                        // foi recusado (ja havia leitura em andamento) ou a
                        // leitura falhar, o proximo bloco tenta de novo — que
                        // era o comportamento antigo e o que evita o clique.
                        v.primeiraJanelaPedida =
                            st.request (juce::jmax ((juce::int64) 0,
                                                   (juce::int64) preloadLen
                                                     - (juce::int64) SampleStreamer::windowOverlap));
                        streamer.wake();
                    }
                }
                else
                {
                    const float* wL = nullptr;
                    const float* wR = nullptr;
                    int off = 0;
                    const auto streamPos = (juce::int64) v.pos;

                    // A ultima janela termina exatamente no fim do arquivo. Ela
                    // nao pode satisfazer o "span" de um bloco inteiro quando
                    // restam menos samples que isso, embora contenha tudo o que
                    // ainda e legal ler. Exigir o bloco cheio aqui silenciava o
                    // ultimo bloco da nota e registrava FALHA-STREAM motivo 3.
                    //
                    // O caminho lento abaixo para antes de v.pos + 2 >= len e
                    // faz o fade final; portanto, no fim, precisamos adquirir
                    // somente o trecho real restante. Para os demais blocos a
                    // exigencia continua identica.
                    const int remaining = (int) juce::jmax (
                        (juce::int64) 0, (juce::int64) len - streamPos);

                    if (remaining <= 2)
                    {
                        v.active = false;
                        st.stop();
                        continue;
                    }

                    const int acquireSpan = juce::jmin (span, remaining);

                    if (st.acquire (streamPos, acquireSpan,
                                    wL, wR, off, streamReadAccess))
                    {
                        srcL    = wL;
                        srcR    = wR;
                        srcBase = (int) ((juce::int64) v.pos - off);
                        srcSpan = SampleStreamer::windowSamples;
                        st.prefetch ((juce::int64) v.pos, streamReadAccess);
                    }
                    else
                    {
                        // A JANELA NAO CHEGOU A TEMPO. Toca silencio neste bloco
                        // e segue o tempo: ler o buffer residente com um indice
                        // que ja passou dele seria ler memoria de outro lugar --
                        // ruido aleatorio, muito pior que uma falha curta.
                        //
                        // MAS UM SILENCIO NO MEIO DE UMA NOTA E UM CLIQUE. Ele
                        // nao aparece como bloco atrasado (o motor entregou no
                        // prazo) nem como estouro (o pico ate CAI) -- por isso e
                        // contado aqui: e um defeito audivel que as duas outras
                        // medicoes sao cegas para ver.
                       #if JUCE_ANDROID
                        ++streamMisses;
                        ++missReason[juce::jlimit (0, 3, st.lastFailReason)];
                       #endif

                        st.request ((juce::int64) v.pos);
                        streamer.wake();

                        // O áudio ficou indisponível, não o tempo musical.
                        // Congelar o ADSR fazia Attack/Release recomeçarem na
                        // amplitude antiga quando a janela chegava, gerando um
                        // degrau audível e uma cauda mais longa que o knob.
                        for (int s = 0; s < numSamples; ++s)
                            v.lastEnv = v.adsr.getNextSample();

                        v.streamRecoveryGain = 0.0f;
                        v.pos += (double) numSamples * v.ratio;

                        if (! v.adsr.isActive()
                             || v.pos + 2.0 >= (double) len)
                        {
                            v.active = false;

                            st.stop();
                        }

                        continue;
                    }
                }
            }
           #endif

            float* outL = lb.getWritePointer (0);
            float* outR = lb.getWritePointer (1);

            layerHasAudio[v.layerIndex] = true;

            float env = 0.0f;

            // ---- CAMINHO RAPIDO ----------------------------------------------
            // Vale quando, para o bloco INTEIRO, nada excepcional pode acontecer:
            // sem loop, sem chegar perto do fim do sample, sem sair da janela e
            // sem precisar da rampa de declique. E o caso da esmagadora maioria
            // dos blocos -- o meio de uma nota sustentada.
            //
            // Provando isso UMA vez aqui, o laco de amostras fica sem nenhum
            // desvio: sem checar limite na interpolacao, sem calcular o fade de
            // fim (que fazia uma divisao em double por amostra), sem testar
            // declique. Foi o que tirou a media de 115% do prazo com 40 vozes.
            const double blockSpan = (double) numSamples * v.ratio + 2.0;
            const int    i0First   = (int) v.pos - srcBase;
            const int    i0Last    = (int) (v.pos + blockSpan) - srcBase;

            const bool fastPath = ! loopOn
                                   && v.declickL == 0.0f && v.declickR == 0.0f
                                   && v.streamRecoveryGain >= 1.0f
                                   && i0First >= 1
                                   && i0Last + 2 < srcSpan
                                   && (double) len - (v.pos + blockSpan) > fadeLen
                                   && v.adsr.isActive();

            if (fastPath)
            {
                const float g = v.gain;

                for (int s = 0; s < numSamples; ++s)
                {
                    env = v.adsr.getNextSample();

                    const int i0 = (int) v.pos - srcBase;
                    const float frac = (float) (v.pos - std::floor (v.pos));
                    const float amp = env * g;

                    const float wetL = (noInterp ? srcL[i0] : hermiteFast (srcL, i0, frac)) * amp;
                    const float wetR = (noInterp ? srcR[i0] : hermiteFast (srcR, i0, frac)) * amp;

                    outL[s] += wetL;
                    outR[s] += wetR;

                    v.lastOutL = wetL;
                    v.lastOutR = wetR;

                    v.pos += v.ratio;
                }

                v.lastEnv = env;

                if (! v.adsr.isActive())
                {
                    v.active = false;

                   #if CHURCHDAW_STREAM_SAMPLES
                    streamer.stream ((int) (&v - voices)).stop();
                   #endif
                }

                continue;
            }

            const float streamRecoveryStep =
                1.0f / (float) juce::jmax (1.0, hostSampleRate * 0.005);

            for (int s = 0; s < numSamples; ++s)
            {
                if (loopOn && v.pos >= loopEnd)
                {
                    // while, nao if: com ratio > loopLen (single-cycle muito
                    // transposto) uma subtracao so deixava pos alem do fim e o
                    // hermite lia fora do buffer. loopLen > 256 garantido pela
                    // validacao, entao nao ha loop infinito.
                    while (v.pos >= loopEnd)
                        v.pos -= loopLen;
                }
                else if (v.pos + 2.0 >= (double) len)
                {
                    v.active = false;
                    break;
                }

                env = v.adsr.getNextSample();

                if (! v.adsr.isActive())
                {
                    v.active = false;
                    break;
                }

                // "v.pos" e a posicao no SAMPLE; "i0" e o indice no buffer que
                // estamos lendo. Sao a mesma coisa na RAM (srcBase = 0) e coisas
                // diferentes no streaming, onde o buffer e uma janela que comeca
                // no meio do arquivo.
                const int i0 = (int) v.pos - srcBase;
                const float frac = (float) (v.pos - std::floor (v.pos));

                // fade suave nos ultimos samples (sem loop) para nao dar clique
                float endFade = 1.0f;

                if (! loopOn)
                {
                    const double remaining = (double) len - v.pos;

                    if (remaining < fadeLen)
                        endFade = (float) juce::jmax (0.0, remaining / fadeLen);
                }

                if (v.streamRecoveryGain < 1.0f)
                    v.streamRecoveryGain = juce::jmin (
                        1.0f,
                        v.streamRecoveryGain + streamRecoveryStep);

                const float amp = env * v.gain * endFade
                                * v.streamRecoveryGain;

                float sL, sR;

                if (noInterp)
                {
                    sL = srcL[i0];
                    sR = srcR[i0];
                }
                else
                {
                    sL = hermite (srcL, i0, frac, srcSpan);
                    sR = hermite (srcR, i0, frac, srcSpan);
                }

                // crossfade da emenda do loop: nos ultimos "xfade" samples antes
                // do fim, ja mistura o material equivalente de antes do inicio do
                // loop, para o salto nao aparecer
                if (loopOn && xfade > 0 && v.pos >= xfadeFrom)
                {
                    const float t = (float) ((v.pos - xfadeFrom) / (double) xfade);
                    const double p2 = v.pos - loopLen;
                    // zona com loop nunca e transmitida, entao srcBase e 0 aqui;
                    // fica explicito para nao virar bug se isso mudar
                    const int j0 = (int) p2 - srcBase;

                    if (j0 >= 1)
                    {
                        const float f2 = (float) (p2 - std::floor (p2));
                        const float aL = hermite (srcL, j0, f2, srcSpan);
                        const float aR = hermite (srcR, j0, f2, srcSpan);

                        // POTENCIA CONSTANTE, nao rampa linear. Os dois trechos
                        // do loop sao o mesmo som em fases diferentes, ou seja
                        // pouco correlacionados: com a rampa linear que estava
                        // aqui (sL += (aL-sL)*t) a soma no meio da emenda perde
                        // ~3 dB e o pad "afunda" a cada volta do loop. Com
                        // sqrt(1-t) e sqrt(t) a energia se mantem.
                        const float gA = std::sqrt (juce::jlimit (0.0f, 1.0f, 1.0f - t));
                        const float gB = std::sqrt (juce::jlimit (0.0f, 1.0f, t));

                        sL = sL * gA + aL * gB;
                        sR = sR * gA + aR * gB;
                    }
                }

                float wetL = sL * amp;
                float wetR = sR * amp;

                // rampa de declique do roubo: ~1 ms de decaimento exponencial
                if (v.declickL != 0.0f || v.declickR != 0.0f)
                {
                    wetL += v.declickL;
                    wetR += v.declickR;

                    v.declickL *= declickDecay;
                    v.declickR *= declickDecay;

                    if (std::abs (v.declickL) < 1.0e-5f) v.declickL = 0.0f;
                    if (std::abs (v.declickR) < 1.0e-5f) v.declickR = 0.0f;
                }

                outL[s] += wetL;
                outR[s] += wetR;

                // guarda o ultimo valor produzido: e dele que sai a rampa de
                // declique se esta voz for roubada (ver startVoiceForLayer)
                v.lastOutL = wetL;
                v.lastOutR = wetR;

                v.pos += v.ratio;
            }

            v.lastEnv = env;

           #if CHURCHDAW_STREAM_SAMPLES
            if (! v.active)
                streamer.stream ((int) (&v - voices)).stop();
           #endif
        }

       #if JUCE_ANDROID
        // Divide a medicao em VOZES e EFEITOS. Sem essa separacao, "o motor esta
        // em 100%" nao diz onde otimizar -- e a otimizacao do laco das vozes
        // rendeu pouco justamente por eu ter suposto o lugar errado.
        voicesTicks += juce::Time::getHighResolutionTicks() - renderStart;
        const auto fxStart = juce::Time::getHighResolutionTicks();
       #endif

        // Rampas de declique dos cortes secos (PANIC, troca de timbre): saem
        // aqui, no buffer do canal, porque a voz que as gerou ja nao existe.
        for (int l = 0; l < numLayers; ++l)
        {
            if (layerDeclickL[l] == 0.0f && layerDeclickR[l] == 0.0f)
                continue;

            auto& lb = layerBuffers[l];
            auto* dL = lb.getWritePointer (0);
            auto* dR = lb.getWritePointer (1);

            for (int s = 0; s < numSamples; ++s)
            {
                dL[s] += layerDeclickL[l];
                dR[s] += layerDeclickR[l];

                layerDeclickL[l] *= declickDecay;
                layerDeclickR[l] *= declickDecay;
            }

            if (std::abs (layerDeclickL[l]) < 1.0e-5f) layerDeclickL[l] = 0.0f;
            if (std::abs (layerDeclickR[l]) < 1.0e-5f) layerDeclickR[l] = 0.0f;

            layerHasAudio[l] = true;   // senao a cadeia do canal seria pulada
        }

        for (int l = 0; l < numLayers; ++l)
        {
            auto& lay = layers[l];
            auto& lb = layerBuffers[l];
            auto& sendSmoother = reverbSendSmooth[l];
            const bool organ = layerIsOrgan[l].load (
                std::memory_order_relaxed);
            const bool churchOrgan =
                layerIsChurchOrgan[l].load (
                    std::memory_order_acquire);
            const bool useChannelRotary =
                layerUsesChannelRotary (l);
            sendSmoother.setTargetValue (
                lay.reverbSend != nullptr ? (float) *lay.reverbSend : 0.0f);
            const float sendStart = sendSmoother.getCurrentValue();
            const float sendEnd = sendSmoother.skip (numSamples);

            // CANAL EM SILENCIO: pula a cadeia inteira.
            //
            // Antes o compressor, os filtros, o delay e o Auto EQ de 4 bandas
            // rodavam nos CINCO canais a cada bloco, mesmo nos vazios e nos
            // mutados. Agora so roda quem tem som — e ainda deixa a cauda do
            // delay escoar antes de desligar, pra nao cortar o eco.
            if (layerHasAudio[l])
                layerIdle[l] = 0;
            else
                ++layerIdle[l];

            const int tailBlocks = (int) (0.6 * hostSampleRate / (double) juce::jmax (1, numSamples));

            if (! layerHasAudio[l] && layerIdle[l] > tailBlocks)
            {
                // o espelho do visor decai mesmo com o canal ocioso -- este
                // continue pulava o decaimento la do fim e o mini-medidor do
                // visor CANAIS ficava com residuo aceso pra sempre
                const float decayed = layerLevelShown[l].load() * 0.94f;
                layerLevelShown[l].store (decayed < 0.001f ? 0.0f : decayed);
                if (useChannelRotary)
                    rotarySpeakers[l].advanceSilently (
                        numSamples,
                        organ && lay.rotaryMode != nullptr
                            ? lay.rotaryMode->getIndex()
                            : ChurchDAWRotary::Speaker::stopMode);
                manualEqs[l].advanceSilently (
                    numSamples,
                    lay.eqLow != nullptr
                        ? (float) *lay.eqLow : 0.0f,
                    lay.eqMid != nullptr
                        ? (float) *lay.eqMid : 0.0f,
                    lay.eqHigh != nullptr
                        ? (float) *lay.eqHigh : 0.0f);
                continue;
            }

            // compressor nativo (um knob: quanto mais, mais aperta e compensa)
            const float compAmt = (float) *lay.comp;

            if (compAmt > 0.001f)
            {
                comps[l].setThreshold (-6.0f - 24.0f * compAmt);
                comps[l].setRatio (1.0f + 7.0f * compAmt);

                juce::dsp::AudioBlock<float> block (lb.getArrayOfWritePointers(), 2, (size_t) numSamples);
                juce::dsp::ProcessContextReplacing<float> ctx (block);
                comps[l].process (ctx);

                lb.applyGain (0, numSamples, juce::Decibels::decibelsToGain (7.0f * compAmt));
            }

            // Compatibilidade sonora: estes sao exatamente os filtros LOW/HIGH
            // 0..1 das versoes ja instaladas. Ficam antes do novo EQ e so
            // custam CPU quando um setup antigo (ou o preset de pad) os usa.
            const float legacyLowCut =
                lay.lowcut != nullptr ? (float) *lay.lowcut : 0.0f;
            const float legacyHighCut =
                lay.highcut != nullptr ? (float) *lay.highcut : 0.0f;

            if (legacyLowCut > 0.001f || legacyHighCut > 0.001f)
            {
                const float twoPi = juce::MathConstants<float>::twoPi;
                const float highPassCoeff = 1.0f - std::exp (
                    -twoPi * (20.0f * std::pow (40.0f, legacyLowCut))
                    / (float) hostSampleRate);
                const float lowPassCoeff = 1.0f - std::exp (
                    -twoPi * (18000.0f / std::pow (22.5f, legacyHighCut))
                    / (float) hostSampleRate);

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* samples = lb.getWritePointer (ch);
                    float hp = hpState[l][ch];
                    float lp = lpState[l][ch];

                    for (int s = 0; s < numSamples; ++s)
                    {
                        float value = samples[s];

                        if (legacyLowCut > 0.001f)
                        {
                            hp += highPassCoeff * (value - hp);
                            value -= hp;
                        }

                        if (legacyHighCut > 0.001f)
                        {
                            lp += lowPassCoeff * (value - lp);
                            value = lp;
                        }

                        samples[s] = value;
                    }

                    hpState[l][ch] = hp;
                    lpState[l][ch] = lp;
                }
            }
            else
            {
                hpState[l][0] = hpState[l][1] = 0.0f;
                lpState[l][0] = lpState[l][1] = 0.0f;
            }

            // EQ manual de verdade: LOW shelf, MID bell e HIGH shelf, todos
            // bipolares em dB. A classe suaviza mudancas para nao chiar/zipar
            // quando o cliente gira o knob com uma nota sustentada.
            manualEqs[l].process (
                lb, numSamples,
                lay.eqLow != nullptr ? (float) *lay.eqLow : 0.0f,
                lay.eqMid != nullptr ? (float) *lay.eqMid : 0.0f,
                lay.eqHigh != nullptr ? (float) *lay.eqHigh : 0.0f);

            // Há exatamente um estágio por família:
            // - Tonewheel/Worship: RotaryCabinet dentro do novo motor;
            // - Transistor/Pipe e órgãos sampled: ChannelRotarySpeaker.
            // O flag/família são atômicos; nenhuma String entra no callback.
            if (useChannelRotary)
                rotarySpeakers[l].process (
                    lb, numSamples,
                    organ && lay.rotaryAmount != nullptr
                            ? (float) *lay.rotaryAmount
                            : 0.0f,
                    organ && lay.rotaryMode != nullptr
                            ? lay.rotaryMode->getIndex()
                            : ChurchDAWRotary::Speaker::stopMode);

            // delay do canal (tempo fixo ~380 ms, realimentacao suave)
            const float dAmt = (float) *lay.delay;

            if (dAmt > 0.001f)
            {
                auto& db = delayBufs[l];
                const int dsize = db.getNumSamples();
                const int dS = juce::jmin (dsize - 1, (int) (0.38 * hostSampleRate));
                int wp = delayWrite[l];

                // ponteiros crus e volta do indice por comparacao: o laco antigo
                // fazia DUAS divisoes inteiras (%) por amostra, por canal
                float* dL = db.getWritePointer (0);
                float* dR = db.getWritePointer (1);
                float* xL = lb.getWritePointer (0);
                float* xR = lb.getWritePointer (1);

                int rp = wp - dS;

                if (rp < 0)
                    rp += dsize;

                for (int s = 0; s < numSamples; ++s)
                {
                    const float echoL = dL[rp], echoR = dR[rp];
                    const float inL = xL[s], inR = xR[s];

                    xL[s] = inL + echoL * dAmt;
                    xR[s] = inR + echoR * dAmt;
                    dL[wp] = inL + echoL * 0.32f;
                    dR[wp] = inR + echoR * 0.32f;

                    if (++wp >= dsize) wp = 0;
                    if (++rp >= dsize) rp = 0;
                }

                delayWrite[l] = wp;
            }

            // Auto EQ (mix de louvor por tipo de instrumento) - reversivel
            if ((bool) *lay.autoEq)
                eqs[l].process (lb, numSamples);

            const bool audible = (bool) *lay.on
                                  && ! (bool) *lay.mute
                                  && (! anySolo || (bool) *lay.solo);
            // RAMPA NO GANHO POS-FADER.
            //
            // Antes o ganho era constante dentro do bloco e mudava de degrau
            // na fronteira: ir de 1.0 a 0.0 ao desligar um canal cortava a
            // onda no meio — estalo audivel, e a cauda nao decaia, sumia. Da
            // para ouvir isso hoje nos pads 1-5 do Launchkey.
            //
            // 20 ms e o menor tempo que nao se ouve como clique e ainda soa
            // como "desligou agora". Como isto roda por bloco, o teto e o
            // proprio tamanho do bloco.
            const float postGainAlvo =
                (audible ? (float) *lay.vol : 0.0f)
                * masterGain;

            const float postGainAnterior = postGainSuave[l];
            const float passoMaximo =
                (float) numSamples / (float) juce::jmax (1.0, 0.020 * hostSampleRate);

            const float postGain = postGainAnterior
                                 + juce::jlimit (-passoMaximo, passoMaximo,
                                                 postGainAlvo - postGainAnterior);

            postGainSuave[l] = postGain;

            float blockPeak = 0.0f;

            // Continua somando enquanto a rampa desce, mesmo com o alvo ja em
            // zero: e justamente esse rabo que faz a cauda decair em vez de
            // ser cortada.
            if (postGain > 0.0f || postGainAnterior > 0.0f)
            {
                // pico POR LADO, pos-fader e pos-mute (postGain ja carrega os dois):
                // o medidor do canal e estereo, entao L e R viajam separados
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float peak = lb.getMagnitude (ch, 0, numSamples) * postGain;
                    blockPeak = juce::jmax (blockPeak, peak);
                    auto& slot = ch == 0 ? layerLevelsL[l] : layerLevelsR[l];

                    float current = slot.load();

                    while (peak > current && ! slot.compare_exchange_weak (current, peak)) {}
                }

                for (int ch = 0; ch < numOut; ++ch)
                    buffer.addFromWithRamp (ch, 0, lb.getReadPointer (ch), numSamples,
                                            postGainAnterior, postGain);

                // ENVIO PARA O REVERB: o mesmo sinal pos-fader vai tambem para o
                // barramento do Church Reverb/BigSky, na dose do knob REVERB. E assim
                // que se consegue "reverb diferente por timbre" com UMA instancia
                // -- mexer no MIX do plugin mudaria para todos os canais de uma vez.
                if (juce::jmax (sendStart, sendEnd) > 0.0005f && reverbBusActive)
                    for (int ch = 0; ch < 2; ++ch)
                        reverbBus.addFromWithRamp (ch, 0, lb.getReadPointer (ch),
                                                  numSamples,
                                                  postGainAnterior * sendStart,
                                                  postGain * sendEnd);

                // Worship Hybrid fornece um envio próprio, separado do áudio
                // principal. Ele segue mute/solo/fader/master do canal, mas não
                // é multiplicado novamente pelo knob REVERB.
                if (churchOrgan && reverbBusActive)
                {
                    auto& organSend =
                        churchOrganReverbBuffers[l];

                    for (int ch = 0; ch < 2; ++ch)
                        reverbBus.addFromWithRamp (
                            ch, 0, organSend.getReadPointer (ch),
                            numSamples, postGainAnterior, postGain);
                }
            }

            // decaimento do espelho acontece AQUI (e nao no leitor) para o nivel
            // cair mesmo se ninguem estiver lendo -- canal mudo nao fica aceso
            {
                const float decayed = layerLevelShown[l].load() * 0.94f;
                layerLevelShown[l].store (juce::jmax (blockPeak, decayed < 0.001f ? 0.0f : decayed));
            }
        }

       #if JUCE_ANDROID
        fxTicks += juce::Time::getHighResolutionTicks() - fxStart;
       #endif
    }


    //==============================================================================
    Layer layers[numLayers];
    Voice voices[maxVoices];

   #if CHURCHDAW_STREAM_SAMPLES
    // Uma janela deslizante por voz. O indice do stream E o indice da voz no
    // array acima -- assim nao ha mapa nem alocacao no caminho do audio.
    SampleStreamer streamer;
   #endif
    juce::int64 voiceCounter = 0;
    bool pedalDown = false;
    // Bitmask POR CANAL MIDI (bit ch-1) de tecla fisicamente pressionada. O
    // note-off e idempotente por canal (auto-cura se um evento se perder no
    // suspendProcessing, como o bool antigo). "Solta" == keyDown[n] == 0.
    // Dois teclados no MESMO canal e mesma nota continuam ambiguos (limite do MIDI).
    juce::uint16 keyDown[128] = {};
    double hostSampleRate = 44100.0;
    std::atomic<bool> processorPrepared { false };

    std::atomic<int> uiSelectedLayer { -1 };   // abre sem canal selecionado
    // ATENCAO ao mexer em numLayers: listas escritas a mao ("{-1,-1,...}")
    // deixariam os canais novos com 0 em vez do valor neutro. Tudo que
    // precisa de valor inicial diferente de zero e preenchido em
    // inicializarCanais(), chamado no construtor.
    int layerColourIdx[numLayers] {};   // -1 = segue a categoria
    std::atomic<juce::uint32> heldBits[4] {};   // teclas pressionadas (para o acorde)
    // Ganho pos-fader efetivo do bloco anterior, por canal. So a thread de
    // audio toca nisto — e a memoria da rampa que evita o degrau ao ligar,
    // desligar, mutar ou dar solo num canal.
    float postGainSuave[numLayers] {};

    std::atomic<float> layerLevelsL[numLayers] {};
    std::atomic<float> layerLevelsR[numLayers] {};
    std::atomic<float> layerLevelShown[numLayers] {};   // espelho p/ o visor CANAIS

    // ---- notas presas nos VSTi hospedados (Kontakt) ----
    std::atomic<bool> panicPending { false };   // PANIC entregue no proximo bloco
    int instNotesOn[numLayers] {};              // note-ons ainda sem note-off
    ChurchDAWPerformanceMidiSafety::NoteTransposeLedger<numLayers>
        noteTranspose;
    ChurchDAWPerformanceMidiSafety::PitchBendGuard pitchBendGuard;

    bool anyKeyDown() const noexcept
    {
        for (auto k : keyDown)
            if (k != 0)
                return true;

        return false;
    }

    bool layerHasAudio[numLayers] {};   // o canal produziu som neste bloco?

    // Rampa de declique por canal, para os cortes que apagam a voz inteira
    // (PANIC, troca de timbre, mudanca de biblioteca).
    float layerDeclickL[numLayers] {};
    float layerDeclickR[numLayers] {};
    int layerIdle[numLayers] {};        // blocos seguidos em silencio (cauda do delay)

    juce::AudioBuffer<float> reverbBus;   // barramento de envio do reverb
    WorshipReverb worshipReverb;          // fallback nativo e backend do Android
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbReturnSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbSendSmooth[numLayers];
    bool reverbBusActive = false;
    juce::AudioBuffer<float> layerBuffers[numLayers];
    juce::AudioBuffer<float> churchOrganReverbBuffers[numLayers];
    juce::dsp::Compressor<float> comps[numLayers];
    LayerEq eqs[numLayers];
    ChurchDAWChannelTone::ThreeBandEq manualEqs[numLayers];
    ChurchDAWTimbreTone::Store<numLayers> timbreToneMemory;
    std::atomic<int> programmaticToneChangeDepth { 0 };
    std::atomic<bool> lastStateCaptureHadFailure { false };
    mutable juce::CriticalSection layerToneTransactionLock;
    mutable juce::CriticalSection layerIdentityLock;
    mutable juce::CriticalSection layerPluginStateLock;
    static_assert (numLayers <= 32,
                   "A mascara de identidade usa um bit por canal");
    static_assert (std::atomic<juce::uint32>::is_always_lock_free,
                   "A identidade dos canais precisa ser lock-free no callback");
    std::atomic<juce::uint32> loadedLayerMask { 0 };
    std::atomic<bool> layerStateRestorePending[numLayers] {};
    std::atomic<bool> insertStateRestorePending[numInserts] {};
    std::atomic<bool> insertPluginLoadInFlight[numInserts] {};
    std::atomic<bool> instrumentPluginLoadInFlight[numLayers] {};
    juce::String layerStateRestoreTargetTimbre[numLayers];
    juce::String layerStateRestoreTargetLibrary[numLayers];
    juce::String layerStateRestoreTargetVstId[numLayers];
    juce::String layerStateRestoreTargetVstState[numLayers];
    ChurchDAWRotary::Speaker rotarySpeakers[numLayers];
    std::atomic<bool> layerIsOrgan[numLayers] {};
    std::atomic<bool> layerIsChurchOrgan[numLayers] {};
    std::atomic<int> layerChurchOrganFamily[numLayers] {};
    std::atomic<int> lastChurchOrganChannelRotaryMode[numLayers] {};
    static constexpr int sustainStateUnknown = -1;
    static constexpr int sustainStateOff = 0;
    static constexpr int sustainStateOn = 1;
    std::atomic<int> churchOrganSustainMirror[numLayers] {};
    std::atomic<int> pluginSustainMirror[numLayers] {};
    std::atomic<bool> pluginPitchCentresPending[numLayers] {};
    std::atomic<bool> pluginPrepared[numLayers] {};
    juce::MemoryBlock pendingChurchOrganStates[numLayers];
    static_assert (
        std::atomic<float>::is_always_lock_free,
        "O trim por familia precisa ser lock-free no callback");
    std::atomic<float> layerNativeFamilyTrim[numLayers] {};
    juce::String layerCategory[numLayers];   // categoria do timbre carregado (para o Auto EQ)
    int lastBlockSize = 512;
    juce::MidiBuffer midiCopy, layerMidi;
    juce::MidiBuffer midiScratch;   // scratch do remap de velocity e da inversao de pedal (sem alocar por bloco)
    juce::AudioBuffer<float> instrumentBuf;   // saida do VSTi por canal (scratch)

    juce::AudioBuffer<float> delayBufs[numLayers];
    int delayWrite[numLayers] = {};
    float hpState[numLayers][2] = {};
    float lpState[numLayers][2] = {};

    std::unique_ptr<juce::AudioPluginInstance> inserts[numInserts];
    std::atomic<bool> insertPrepared[numInserts] {
        false, false
    };
    std::atomic<int> externalReverbSlot { -1 };
    std::atomic<juce::AudioProcessorParameter*> externalReverbMixParam { nullptr };
    std::atomic<juce::AudioProcessorParameter*> externalReverbTypeParam { nullptr };
    float externalReverbOriginalMix[numInserts] { 1.0f, 1.0f };
    bool externalReverbOriginalMixValid[numInserts] { false, false };
    juce::KnownPluginList* knownPluginsRef = nullptr;
    juce::AudioPluginFormatManager* formatManagerRef = nullptr;
    const juce::StringArray* pluginQuarantineRef = nullptr;   // pertence ao MainComponent
    mutable juce::CriticalSection hostedPluginEditorCallbackLock;
    mutable juce::CriticalSection hostedPluginDeferredFinishLock;
    HostedPluginWillRetireCallback hostedPluginWillRetire;
    std::atomic<int> hostedPluginEditorTeardownDepth { 0 };
    std::function<void()> deferredInsertPluginFinishes[numInserts];
    std::function<void()> deferredInstrumentPluginFinishes[numLayers];
    ChurchDAWPluginLoading::RequestGeneration
        insertPluginLoadGeneration[numInserts];
    ChurchDAWPluginLoading::RequestGeneration
        instrumentPluginLoadGeneration[numLayers];

    std::vector<std::vector<std::unique_ptr<Zone>>> retiredZones;   // samples antigos ate as vozes acabarem

    juce::File rootFolder { defaultSamplesFolder() };
    juce::Array<TimbreEntry> entriesCache;
    bool entriesScanned = false;
    int scanGeneration = 0;
    juce::String pendingLayerLabels[numLayers];
    juce::String pendingLayerLibs[numLayers];
    juce::String loadingLabels[numLayers];   // rotulo/lib do decode em voo (fallback do save)
    juce::String loadingLibs[numLayers];
    std::atomic<int> layerLoadGeneration[numLayers] {};   // invalida decodes velhos do canal
    std::shared_ptr<std::atomic<bool>> layerLoadAbort[numLayers]; // cancela decode velho por canal
    juce::String pendingVstIds[numLayers], pendingVstStates[numLayers];           // VSTi que falhou no restore (auto-cura no save)
    juce::String pendingInsertIds[numInserts], pendingInsertStates[numInserts];   // idem para os INSERTs
    juce::PluginDescription pendingVstDescriptions[numLayers];
    juce::PluginDescription pendingInsertDescriptions[numInserts];
    bool pendingVstDescriptionValid[numLayers] {};
    bool pendingInsertDescriptionValid[numInserts] {};
    bool showroomPending = false;   // primeira execucao: carregar setup de fabrica pos-scan
    // Compartilhado com os tickets dos workers: mesmo se callAsync falhar ou o
    // processador morrer antes do callback, cada pedido decrementa exatamente
    // uma vez sem acessar memoria ja destruida.
    std::shared_ptr<std::atomic<int>> loadingCount =
        std::make_shared<std::atomic<int>> (0);

    // Gate POR PROCESSADOR: no maximo um conjunto de zonas pesadas e construido
    // de cada vez, mesmo quando canais diferentes sao escolhidos rapidamente.
    // shared_ptr garante vida ate o ultimo worker sair durante o destrutor.
    std::shared_ptr<std::mutex> layerDecodeGate =
        std::make_shared<std::mutex>();

    // Dois workers persistentes: um pode decodificar enquanto o outro conclui
    // scan/reclaim. A thread de audio continua com prioridade superior; o gate
    // layerDecodeGate garante apenas um piano pesado por processador.
    juce::ThreadPool workerPool {
        juce::ThreadPool::Options()
            .withThreadName ("ChurchDAW library")
            .withNumberOfThreads (2)
            .withDesiredThreadPriority (juce::Thread::Priority::normal)
    };

    // sinal de cancelamento dos workers: o destrutor liga o flag e os loops de
    // scan/decode retornam entre um arquivo/pasta e outro, entao o join nunca
    // espera o trabalho inteiro. shared_ptr porque o lambda captura por valor
    // e pode sobreviver ao processador.
    std::shared_ptr<std::atomic<bool>> workerAbort = std::make_shared<std::atomic<bool>> (false);

    void cancelLayerLoad (int layerIndex) noexcept
    {
        if (juce::isPositiveAndBelow (layerIndex, numLayers))
            if (auto requestAbort = layerLoadAbort[layerIndex])
                requestAbort->store (true, std::memory_order_release);
    }

    template <typename WorkerFunction>
    bool startWorkerThread (WorkerFunction&& fn) noexcept
    {
        try
        {
            workerPool.addJob (
                [fn = std::forward<WorkerFunction> (fn)] () mutable
            {
                try
                {
                    fn();
                }
                catch (...)
                {
                    // A fronteira do pool nunca deixa excecao escapar: o
                    // carregador reporta sua falha; scan/reclaim preferem perder
                    // um resultado a encerrar o aplicativo.
                }
            });

            return true;
        }
        catch (...)
        {
            // Nada que possa alocar dentro do handler: a falha mais provavel
            // aqui e justamente memoria insuficiente.
        }

        return false;
    }

    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterInt* globalTransp = nullptr;
    juce::AudioParameterBool* globalSustain = nullptr;
    juce::AudioParameterBool* pedalInvert = nullptr;
    juce::AudioParameterChoice* velCurve = nullptr;
    juce::AudioParameterChoice* nativeReverbPreset = nullptr;
    juce::AudioParameterFloat* reverbReturn = nullptr;
    bool prevGlobalSustain = false;
    bool pedalDownAtBlockStart = false;
    MasterProcessor* externalMaster = nullptr;   // master da DAW (nao possuido)

    friend class TimbresEditor;

    JUCE_DECLARE_WEAK_REFERENCEABLE (TimbresProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbresProcessor)
};

//==============================================================================
// Ponte message-thread entre o editor premium e o adapter DSP do canal.
// O editor recebe snapshots POD/atômicos; nenhuma chamada daqui participa do
// callback de áudio.
class TimbresChurchOrganUiSink final
    : public churchdaw::organ::ChurchOrganParameterSink
{
public:
    TimbresChurchOrganUiSink (
        TimbresProcessor& processorToUse,
        int layerToUse)
        : processor (processorToUse),
          layer (juce::jlimit (
              0, TimbresProcessor::numLayers - 1,
              layerToUse))
    {
        cosmeticState.setValue (
            churchdaw::organ::ChurchOrganParameter::amplifier,
            0.58f);
        cosmeticState.setValue (
            churchdaw::organ::ChurchOrganParameter::cabinet,
            0.46f);
        cosmeticState.setValue (
            churchdaw::organ::ChurchOrganParameter::acceleration,
            0.46f);
        cosmeticState.setValue (
            churchdaw::organ::ChurchOrganParameter::deceleration,
            0.62f);
        cosmeticState.setValue (
            churchdaw::organ::ChurchOrganParameter::worshipTexture,
            0.44f);

        presets =
        {
            makePreset (
                "soft-worship", "Soft Worship", "Worship",
                "Warm - Pad - Wide",
                { 7, 3, 8, 6, 2, 3, 1, 0, 1 },
                churchdaw::organ::ChurchOrganRotaryMode::slow,
                0.42f, true),
            makePreset (
                "gospel-fire", "Gospel Fire", "Gospel",
                "Bright - Fast - Drive",
                { 8, 6, 8, 7, 5, 6, 4, 3, 3 },
                churchdaw::organ::ChurchOrganRotaryMode::fast,
                0.18f, false),
            makePreset (
                "velvet-chapel", "Velvet Chapel", "Classic",
                "Clean - Slow - Soft",
                { 6, 2, 7, 4, 1, 2, 0, 0, 0 },
                churchdaw::organ::ChurchOrganRotaryMode::slow,
                0.12f, false),
            makePreset (
                "open-heavens", "Open Heavens", "Worship",
                "Shimmer - Air - Cinematic",
                { 7, 4, 8, 5, 3, 4, 2, 1, 2 },
                churchdaw::organ::ChurchOrganRotaryMode::slow,
                0.88f, true),
            makePreset (
                "cathedral-principal", "Cathedral Principal",
                "Pipe", "Pipe - Grand - Room",
                { 8, 5, 8, 7, 6, 5, 4, 3, 4 },
                churchdaw::organ::ChurchOrganRotaryMode::stop,
                0.30f, false),
            makePreset (
                "transistor-sun", "Transistor Sun",
                "Transistor", "Bite - Combo - Dry",
                { 4, 1, 8, 7, 4, 5, 2, 2, 2 },
                churchdaw::organ::ChurchOrganRotaryMode::fast,
                0.06f, false)
        };

        selectPresentationForFamily (
            processor.getLayerChurchOrganFamily (layer));
    }

    churchdaw::organ::ChurchOrganUiSnapshot
        getUiSnapshot() const override
    {
        auto state = cosmeticState;
        const auto* organ =
            processor.getLayerChurchOrganProcessor (layer);

        if (organ == nullptr)
        {
            state.engineEnabled = false;
            return state;
        }

        const auto& parameters = organ->parameters();
        static constexpr churchorgan::OrganParameterId drawbars[9] {
            churchorgan::OrganParameterId::upper16,
            churchorgan::OrganParameterId::upper5AndOneThird,
            churchorgan::OrganParameterId::upper8,
            churchorgan::OrganParameterId::upper4,
            churchorgan::OrganParameterId::upper2AndTwoThirds,
            churchorgan::OrganParameterId::upper2,
            churchorgan::OrganParameterId::upper1AndThreeFifths,
            churchorgan::OrganParameterId::upper1AndOneThird,
            churchorgan::OrganParameterId::upper1
        };

        for (int i = 0; i < 9; ++i)
            state.drawbars[(size_t) i] = juce::jlimit (
                0, 8, juce::roundToInt (
                    parameters.getPlain (drawbars[i])));

        for (int i = 0;
             i < churchdaw::organ::churchOrganParameterCount;
             ++i)
        {
            churchorgan::OrganParameterId dsp {};
            const auto ui = static_cast<
                churchdaw::organ::ChurchOrganParameter> (i);

            if (mapParameter (ui, dsp))
                state.setValue (
                    ui, parameters.getNormalised (dsp));
        }

        const int rotary = juce::jlimit (
            0, 3, juce::roundToInt (
                parameters.getPlain (
                    churchorgan::OrganParameterId::rotaryMode)));
        state.rotaryMode =
            static_cast<
                churchdaw::organ::ChurchOrganRotaryMode> (
                    rotary);

        const int quality = juce::jlimit (
            0, 2, juce::roundToInt (
                parameters.getPlain (
                    churchorgan::OrganParameterId::quality)));
        state.quality =
            static_cast<
                churchdaw::organ::ChurchOrganQuality> (
                    quality);
        state.percussionEnabled =
            parameters.getPlain (
                churchorgan::OrganParameterId::percussionEnabled)
            >= 0.5f;
        state.percussionHarmonic =
            parameters.getPlain (
                churchorgan::OrganParameterId::percussionHarmonic)
                < 0.5f ? 2 : 3;
        state.chorusMode = juce::jlimit (
            0, 6, juce::roundToInt (
                parameters.getPlain (
                    churchorgan::OrganParameterId::scannerMode)));
        state.tonewheelProfile = juce::jlimit (
            0, 3, juce::roundToInt (
                parameters.getPlain (
                    churchorgan::OrganParameterId::tonewheelProfile)));
        state.autoMixEnabled =
            parameters.getPlain (
                churchorgan::OrganParameterId::worshipAutoMix)
            >= 0.5f;
        state.engineEnabled =
            processor.getLayerOn (layer);

        const auto diagnostics = organ->diagnostics();
        state.midiConnected = true;
        state.midiActivity =
            diagnostics.activeNotes > 0;
        state.cpuPercent = 0.0f;
        state.presetId = selectedPresetId;
        state.presetName = selectedPresetName;
        state.presetCategory = selectedPresetCategory;
        return state;
    }

    std::vector<churchdaw::organ::ChurchOrganPresetSummary>
        getPresetSummaries() const override
    {
        return presets;
    }

    void setDrawbar (int index, int value) override
    {
        static constexpr churchorgan::OrganParameterId drawbars[9] {
            churchorgan::OrganParameterId::upper16,
            churchorgan::OrganParameterId::upper5AndOneThird,
            churchorgan::OrganParameterId::upper8,
            churchorgan::OrganParameterId::upper4,
            churchorgan::OrganParameterId::upper2AndTwoThirds,
            churchorgan::OrganParameterId::upper2,
            churchorgan::OrganParameterId::upper1AndThreeFifths,
            churchorgan::OrganParameterId::upper1AndOneThird,
            churchorgan::OrganParameterId::upper1
        };

        if (auto* organ =
                processor.getLayerChurchOrganProcessor (layer);
            organ != nullptr
             && juce::isPositiveAndBelow (index, 9))
            organ->setParameterPlain (
                drawbars[index],
                (float) juce::jlimit (0, 8, value));
    }

    void setParameter (
        churchdaw::organ::ChurchOrganParameter parameter,
        float normalisedValue) override
    {
        const float value =
            juce::jlimit (0.0f, 1.0f, normalisedValue);
        churchorgan::OrganParameterId dsp {};

        if (mapParameter (parameter, dsp))
        {
            if (auto* organ =
                    processor.getLayerChurchOrganProcessor (
                        layer))
                organ->setParameterNormalised (dsp, value);

            if (dsp
                 == churchorgan::OrganParameterId::rotaryMix)
                processor.setChurchOrganRotaryMix (
                    layer, value);
        }
        else
        {
            // Estes controles ainda não têm ID DSP no catálogo congelado.
            // Guardamos somente a apresentação; não os ligamos por aproximação
            // a outro estágio sonoro.
            cosmeticState.setValue (parameter, value);
        }
    }

    void setRotaryMode (
        churchdaw::organ::ChurchOrganRotaryMode mode) override
    {
        processor.setChurchOrganRotaryMode (layer, mode);
    }

    void setPercussionEnabled (bool enabled) override
    {
        setPlain (
            churchorgan::OrganParameterId::percussionEnabled,
            enabled ? 1.0f : 0.0f);
    }

    void setPercussionHarmonic (int harmonic) override
    {
        setPlain (
            churchorgan::OrganParameterId::percussionHarmonic,
            harmonic == 2 ? 0.0f : 1.0f);
    }

    void setChorusMode (int mode) override
    {
        setPlain (
            churchorgan::OrganParameterId::scannerMode,
            (float) juce::jlimit (0, 6, mode));
    }

    void setTonewheelProfile (int profile) override
    {
        setPlain (
            churchorgan::OrganParameterId::tonewheelProfile,
            (float) juce::jlimit (0, 3, profile));
    }

    void setAutoMixEnabled (bool enabled) override
    {
        setPlain (
            churchorgan::OrganParameterId::worshipAutoMix,
            enabled ? 1.0f : 0.0f);
    }

    void setQuality (
        churchdaw::organ::ChurchOrganQuality quality) override
    {
        (void) processor.setChurchOrganQuality (
            layer, quality);
    }

    void setEngineEnabled (bool enabled) override
    {
        processor.setChurchOrganEnabled (layer, enabled);
    }

    void selectPreset (
        const juce::String& presetId) override
    {
        const auto iterator = std::find_if (
            presets.begin(), presets.end(),
            [&presetId] (const auto& preset)
            {
                return preset.id == presetId;
            });

        if (iterator == presets.end())
            return;

        const auto family =
            familyForPreset (iterator->id);

        if (! processor.loadChurchOrganIntoLayer (
                layer, family, nullptr, false))
            return;

        selectedPresetId = iterator->id;
        selectedPresetName = iterator->name;
        selectedPresetCategory = iterator->category;

        for (int i = 0; i < 9; ++i)
            setDrawbar (
                i, iterator->drawbars[(size_t) i]);

        setRotaryMode (iterator->rotary);
        setParameter (
            churchdaw::organ::ChurchOrganParameter::
                worshipIntensity,
            iterator->worshipIntensity);
    }

    void setPresetFavourite (
        const juce::String& presetId,
        bool favourite) override
    {
        const auto iterator = std::find_if (
            presets.begin(), presets.end(),
            [&presetId] (const auto& preset)
            {
                return preset.id == presetId;
            });

        if (iterator != presets.end())
            iterator->favourite = favourite;
    }

private:
    static bool mapParameter (
        churchdaw::organ::ChurchOrganParameter parameter,
        churchorgan::OrganParameterId& result) noexcept
    {
        using Ui =
            churchdaw::organ::ChurchOrganParameter;
        using Dsp = churchorgan::OrganParameterId;

        switch (parameter)
        {
            case Ui::drive:             result = Dsp::drive; break;
            case Ui::expression:        result = Dsp::expression; break;
            case Ui::reverb:            result = Dsp::reverbAmount; break;
            case Ui::worshipIntensity:  result = Dsp::worshipIntensity; break;
            case Ui::tonewheelAge:      result = Dsp::age; break;
            case Ui::keyClick:          result = Dsp::keyClick; break;
            case Ui::leakage:           result = Dsp::leakage; break;
            case Ui::crosstalk:         result = Dsp::crosstalk; break;
            case Ui::hum:               result = Dsp::hum; break;
            case Ui::contactSpread:     result = Dsp::contactSpread; break;
            case Ui::transformer:       result = Dsp::transformer; break;
            case Ui::tone:              result = Dsp::tone; break;
            case Ui::hornLevel:         result = Dsp::rotaryHornLevel; break;
            case Ui::drumLevel:         result = Dsp::rotaryDrumLevel; break;
            case Ui::rotaryDistance:    result = Dsp::rotaryMicDistance; break;
            case Ui::rotaryAngle:       result = Dsp::rotaryMicAngle; break;
            case Ui::rotaryWidth:       result = Dsp::rotaryStereoWidth; break;
            case Ui::rotaryRoom:        result = Dsp::rotaryRoom; break;
            case Ui::worshipPad:        result = Dsp::worshipPad; break;
            case Ui::worshipAir:        result = Dsp::worshipAir; break;
            case Ui::worshipStrings:    result = Dsp::worshipStrings; break;
            case Ui::worshipShimmer:    result = Dsp::worshipShimmer; break;
            case Ui::worshipDucking:    result = Dsp::worshipDucking; break;
            case Ui::worshipAttack:     result = Dsp::worshipAttackMs; break;
            case Ui::worshipRelease:    result = Dsp::worshipReleaseMs; break;

            case Ui::amplifier:
            case Ui::cabinet:
            case Ui::acceleration:
            case Ui::deceleration:
            case Ui::worshipTexture:
            case Ui::count:
                return false;
        }

        return true;
    }

    static churchdaw::organ::ChurchOrganPresetSummary
        makePreset (
            juce::String id,
            juce::String name,
            juce::String category,
            juce::String tags,
            std::array<int, 9> drawbars,
            churchdaw::organ::ChurchOrganRotaryMode rotary,
            float worship,
            bool favourite)
    {
        churchdaw::organ::ChurchOrganPresetSummary preset;
        preset.id = std::move (id);
        preset.name = std::move (name);
        preset.category = std::move (category);
        preset.tags = std::move (tags);
        preset.drawbars = drawbars;
        preset.rotary = rotary;
        preset.worshipIntensity = worship;
        preset.favourite = favourite;
        return preset;
    }

    static churchorgan::ChurchOrganFamily familyForPreset (
        const juce::String& presetId) noexcept
    {
        if (presetId == "transistor-sun")
            return churchorgan::ChurchOrganFamily::transistor;
        if (presetId == "cathedral-principal")
            return churchorgan::ChurchOrganFamily::pipe;
        if (presetId == "soft-worship"
             || presetId == "open-heavens")
            return churchorgan::ChurchOrganFamily::worshipHybrid;

        return churchorgan::ChurchOrganFamily::tonewheel;
    }

    void selectPresentationForFamily (
        churchorgan::ChurchOrganFamily family)
    {
        switch (family)
        {
            case churchorgan::ChurchOrganFamily::tonewheel:
                selectedPresetId = "velvet-chapel";
                selectedPresetName = "Velvet Chapel";
                selectedPresetCategory = "Classic";
                break;
            case churchorgan::ChurchOrganFamily::transistor:
                selectedPresetId = "transistor-sun";
                selectedPresetName = "Transistor Sun";
                selectedPresetCategory = "Transistor";
                break;
            case churchorgan::ChurchOrganFamily::pipe:
                selectedPresetId = "cathedral-principal";
                selectedPresetName = "Cathedral Principal";
                selectedPresetCategory = "Pipe";
                break;
            case churchorgan::ChurchOrganFamily::worshipHybrid:
                selectedPresetId = "soft-worship";
                selectedPresetName = "Soft Worship";
                selectedPresetCategory = "Worship";
                break;
        }
    }

    void setPlain (
        churchorgan::OrganParameterId parameter,
        float value)
    {
        if (auto* organ =
                processor.getLayerChurchOrganProcessor (layer))
            organ->setParameterPlain (parameter, value);
    }

    TimbresProcessor& processor;
    int layer = 0;
    churchdaw::organ::ChurchOrganUiSnapshot cosmeticState;
    std::vector<
        churchdaw::organ::ChurchOrganPresetSummary> presets;
    juce::String selectedPresetId;
    juce::String selectedPresetName;
    juce::String selectedPresetCategory;
};

class TimbresChurchOrganOverlay final
    : public juce::Component
{
public:
    TimbresChurchOrganOverlay (
        TimbresProcessor& processor,
        int layer)
        : sink (processor, layer),
          editor (sink)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        addAndMakeVisible (editor);

        // Navegacao e SECUNDARIA: o vermelho chapado gritava mais que o
        // proprio organ (revisao do dono, item 1). Botao escuro e discreto;
        // o protagonista e o motor.
        closeButton.setButtonText (
            juce::String::fromUTF8 ("\xe2\x86\x90 MIXER"));
        closeButton.setTooltip (
            "Fechar o editor do Church Organ sem interromper o som");
        closeButton.setColour (
            juce::TextButton::buttonColourId,
            juce::Colour (0xff23262d));
        closeButton.setColour (
            juce::TextButton::textColourOffId,
            juce::Colours::white.withAlpha (0.85f));
        closeButton.onClick = [this]
        {
            if (onClose != nullptr)
                onClose();
        };
        addAndMakeVisible (closeButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0d10));
    }

    void resized() override
    {
        editor.setBounds (getLocalBounds());
        closeButton.setBounds (
            getLocalBounds()
                .removeFromTop (46)
                .removeFromRight (104)
                .reduced (6, 7));
        closeButton.toFront (false);
    }

    bool keyPressed (
        const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onClose != nullptr)
                onClose();
            return true;
        }

        return juce::Component::keyPressed (key);
    }

    std::function<void()> onClose;

private:
    TimbresChurchOrganUiSink sink;
    churchdaw::organ::ChurchOrganEditor editor;
    juce::TextButton closeButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (
        TimbresChurchOrganOverlay)
};

//==============================================================================
// Icone pequeno que identifica o tipo de instrumento do canal
// (branco/cinza quando inativo, vermelho quando carregado)
//==============================================================================
class TimbreGlyph : public juce::Component
{
public:
    TimbreGlyph() = default;

    enum Kind { none, piano, organ, strings, lead, pad, ambience, bells, note,
                nordBrand, kontaktBrand, vstBrand };   // marcas (badge do que esta carregado)

    // decide o icone do canal a partir do que esta carregado: plugin externo
    // vira badge da marca dele; timbre nativo usa o glyph da categoria.
    //
    // Os pianos nativos JA MOSTRARAM badge Nord aqui, e isso saiu de proposito:
    // o badge anunciava de qual hardware/acervo o timbre veio, que e exatamente
    // o que o acordo com o autor do acervo pede para nao aparecer -- e ainda
    // colava uma marca de terceiro num produto a venda. Agora e o glyph da casa.
    static Kind resolveKind (bool isPlugin, const juce::String& pluginName, const juce::String& timbre)
    {
        if (isPlugin)
        {
            const auto n = pluginName.toLowerCase();

            if (n.contains ("kontakt"))
                return kontaktBrand;

            return vstBrand;
        }

        return kindForName (timbre);
    }

    static Kind kindForName (const juce::String& name)
    {
        const auto n = name.toLowerCase();

        if (n.isEmpty())
            return none;

        if (n.contains ("ambien"))
            return ambience;

        if (n.contains ("bell"))
            return bells;

        if (n.contains ("b3") || n.contains ("organ") || n.contains ("pipe") || n.contains ("farf"))
            return organ;

        if (n.contains ("string"))
            return strings;

        if (n.contains ("piano") || n.contains ("grand") || n.contains ("upright")
             || n.contains ("tines") || n.startsWith ("ep") || n.contains ("keys"))
            return piano;

        if (n.contains ("lead") || n.contains ("saw") || n.contains ("sine") || n.contains ("plucked"))
            return lead;

        if (n.contains ("pad") || n.contains ("ambien") || n.contains ("warm") || n.contains ("analog")
             || n.contains ("synth") || n.contains ("vox") || n.contains ("rev") || n.contains ("fx"))
            return pad;

        return note;
    }

    void setKind (Kind k, bool isActive)
    {
        if (kind != k || active != isActive)
        {
            kind = k;
            active = isActive;
            repaint();
        }
    }

    static juce::Image imageForKind (Kind k)
    {
        switch (k)
        {
            case piano:    return juce::ImageCache::getFromMemory (BinaryData::glyph_piano_png, BinaryData::glyph_piano_pngSize);
            case organ:    return juce::ImageCache::getFromMemory (BinaryData::glyph_organ_png, BinaryData::glyph_organ_pngSize);
            case pad:      return juce::ImageCache::getFromMemory (BinaryData::glyph_pad_png, BinaryData::glyph_pad_pngSize);
            case strings:  return juce::ImageCache::getFromMemory (BinaryData::glyph_strings_png, BinaryData::glyph_strings_pngSize);
            case ambience: return juce::ImageCache::getFromMemory (BinaryData::glyph_ambience_png, BinaryData::glyph_ambience_pngSize);
            case bells:    return juce::ImageCache::getFromMemory (BinaryData::glyph_bells_png, BinaryData::glyph_bells_pngSize);
            case lead:     return juce::ImageCache::getFromMemory (BinaryData::glyph_lead_png, BinaryData::glyph_lead_pngSize);
            case note:     return juce::ImageCache::getFromMemory (BinaryData::icon_png, BinaryData::icon_pngSize);
            default:       return {};
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (kind == none)
            return;

        g.setOpacity (active ? 1.0f : 0.45f);
        paintBadge (g, getLocalBounds().toFloat().reduced (1.0f), kind);
        g.setOpacity (1.0f);
    }

    //==============================================================================
    // Badge completo em QUALQUER tamanho/lugar (card do canal, linha de lista...).
    // Marca (Nord/Kontakt/VST) desenha a arte da marca; o resto desenha o simbolo
    // vetorial da categoria. Tudo sobre a mesma chapa — e o que da a unidade.
    static void paintBadge (juce::Graphics& g, juce::Rectangle<float> plate, Kind k)
    {
        if (k == none)
            return;

        // marcas (badge preenchido do que esta carregado) — desenho original,
        // NAO usa os logotipos oficiais (marca registrada) pra nao violar direito
        if (k == nordBrand || k == kontaktBrand || k == vstBrand)
        {
            paintBrandArt (g, plate, k);
            return;
        }

        // Icone de categoria: MESMA chapa dos badges de marca + simbolo vetorial
        // nitido. Antes eram os PNGs de Assets — renders 3D escuros, desenhados
        // pra fundo claro, colados soltos no card sem moldura: conviviam com os
        // badges preenchidos do Nord/Kontakt como se fossem de outro aplicativo,
        // e dentro de uma chapa grafite viravam um borrao. Agora a fileira toda
        // fala a mesma lingua e o que distingue e a FORMA, nao a cor.
        paintBadgePlate (g, plate, juce::Colour (0xff30343c), juce::Colour (0xff1a1d22));

        auto b = plate.reduced (plate.getWidth() * 0.22f, plate.getHeight() * 0.23f);
        g.setColour (juce::Colour (0xffe4e6ea));

        const float x = b.getX(), y = b.getY(), w = b.getWidth(), h = b.getHeight();

        switch (k)
        {
            case piano:
            {
                // teclado com teclas pretas: le-se muito melhor que dois riscos
                auto kb = juce::Rectangle<float> (x, y + h * 0.22f, w, h * 0.56f);
                g.drawRoundedRectangle (kb, 2.0f, 1.4f);

                for (float f : { 0.30f, 0.52f, 0.74f })
                    g.fillRect (juce::Rectangle<float> (x + w * f - w * 0.05f, kb.getY() + 1.0f,
                                                        w * 0.10f, kb.getHeight() * 0.58f));
                break;
            }

            case organ:
            {
                const float bw = w * 0.16f;
                const float xs[4] = { 0.06f, 0.32f, 0.58f, 0.84f };
                const float hs[4] = { 0.55f, 0.8f, 0.8f, 0.55f };

                for (int i = 0; i < 4; ++i)
                    g.fillRoundedRectangle (x + w * xs[i], y + h * (0.9f - hs[i]), bw, h * hs[i], 1.5f);
                break;
            }

            case strings:
                for (int i = 0; i < 3; ++i)
                    g.drawLine (x + w * (0.25f + 0.25f * (float) i), y + h * 0.15f,
                                x + w * (0.25f + 0.25f * (float) i), y + h * 0.85f, 1.2f);
                g.drawLine (x + w * 0.08f, y + h * 0.75f, x + w * 0.92f, y + h * 0.35f, 1.6f);
                break;

            case lead:
            {
                juce::Path p;
                p.startNewSubPath (x, y + h * 0.7f);
                p.lineTo (x + w * 0.33f, y + h * 0.3f);
                p.lineTo (x + w * 0.33f, y + h * 0.7f);
                p.lineTo (x + w * 0.66f, y + h * 0.3f);
                p.lineTo (x + w * 0.66f, y + h * 0.7f);
                p.lineTo (x + w, y + h * 0.3f);
                g.strokePath (p, juce::PathStrokeType (1.5f));
                break;
            }

            case pad:
                for (int i = 0; i < 2; ++i)
                {
                    const float yy = y + h * (0.35f + 0.3f * (float) i);
                    juce::Path p;
                    p.startNewSubPath (x, yy);
                    p.cubicTo (x + w * 0.25f, yy - h * 0.22f, x + w * 0.5f, yy + h * 0.22f, x + w, yy);
                    g.strokePath (p, juce::PathStrokeType (1.4f));
                }
                break;

            case ambience:
                // ondas concentricas subindo = espaco / ar
                for (int i = 1; i <= 3; ++i)
                {
                    const float r = w * 0.17f * (float) i;
                    juce::Path arc;
                    arc.addCentredArc (x + w * 0.5f, y + h * 0.86f, r, r, 0.0f, -2.1f, 2.1f, true);
                    g.strokePath (arc, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
                }
                break;

            case bells:
            {
                juce::Path bell;
                bell.startNewSubPath (x + w * 0.18f, y + h * 0.70f);
                bell.cubicTo (x + w * 0.20f, y + h * 0.14f, x + w * 0.80f, y + h * 0.14f,
                              x + w * 0.82f, y + h * 0.70f);
                g.strokePath (bell, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
                g.drawLine (x + w * 0.10f, y + h * 0.72f, x + w * 0.90f, y + h * 0.72f, 1.5f);
                g.fillEllipse (x + w * 0.42f, y + h * 0.78f, w * 0.16f, w * 0.16f);
                break;
            }

            case note:
                g.fillEllipse (x + w * 0.15f, y + h * 0.6f, w * 0.35f, h * 0.28f);
                g.drawLine (x + w * 0.5f, y + h * 0.72f, x + w * 0.5f, y + h * 0.15f, 1.5f);
                g.drawLine (x + w * 0.5f, y + h * 0.15f, x + w * 0.8f, y + h * 0.28f, 1.5f);
                break;

            default:
                break;
        }
    }

    // badge quadrado preenchido (estilo "icone de app") que identifica a MARCA do
    // que esta no canal. Arte ORIGINAL: badge Nord = vermelho com teclas; Kontakt
    // = grafite com onda de sampler ambar; VST generico = grafite com onda. Nao
    // reproduz os logotipos oficiais (sao marcas registradas).
    static void paintBrandArt (juce::Graphics& g, juce::Rectangle<float> b, Kind kind)
    {
        const float x = b.getX(), y = b.getY(), w = b.getWidth(), h = b.getHeight();

        auto fillBadge = [&] (juce::Colour top, juce::Colour bot) { paintBadgePlate (g, b, top, bot); };

        if (kind == nordBrand)
        {
            fillBadge (NordLookAndFeel::nordRed(), NordLookAndFeel::nordRedDark());

            // tampa do grand (diagonal) + teclas brancas = "piano vermelho" Nord
            g.setColour (juce::Colours::white.withAlpha (0.88f));
            g.drawLine (x + w * 0.16f, y + h * 0.34f, x + w * 0.84f, y + h * 0.24f, 1.6f);

            const float kw = w * 0.12f, ky = y + h * 0.44f, kh = h * 0.32f;
            g.setColour (juce::Colours::white.withAlpha (0.94f));
            for (int i = 0; i < 4; ++i)
                g.fillRoundedRectangle (x + w * 0.20f + (float) i * (kw + w * 0.05f), ky, kw, kh, 1.0f);
        }
        else if (kind == kontaktBrand)
        {
            fillBadge (juce::Colour (0xff2b2e34), juce::Colour (0xff16181c));

            // onda de sampler ambar
            g.setColour (juce::Colour (0xffe6913f));
            const float bw = w * 0.085f, midY = y + h * 0.52f;
            const float hs[5] = { 0.30f, 0.62f, 0.96f, 0.56f, 0.34f };
            for (int i = 0; i < 5; ++i)
            {
                const float bh = h * hs[i] * 0.42f;
                g.fillRoundedRectangle (x + w * 0.19f + (float) i * (bw + w * 0.055f), midY - bh, bw, bh * 2.0f, 1.2f);
            }
        }
        else   // vstBrand
        {
            fillBadge (juce::Colour (0xff353941), juce::Colour (0xff1e2127));

            g.setColour (NordLookAndFeel::nordRedBright());
            juce::Path p;
            p.startNewSubPath (x + w * 0.15f, y + h * 0.52f);
            p.cubicTo (x + w * 0.34f, y + h * 0.18f, x + w * 0.52f, y + h * 0.86f, x + w * 0.70f, y + h * 0.42f);
            p.lineTo (x + w * 0.86f, y + h * 0.52f);
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // chapa comum de TODOS os icones de canal (marca ou categoria): quadrado
    // arredondado com gradiente, brilho no topo e aro — o que da a unidade
    static void paintBadgePlate (juce::Graphics& g, juce::Rectangle<float> b,
                                 juce::Colour top, juce::Colour bot)
    {
        const float rad = juce::jmin (8.0f, b.getWidth() * 0.24f);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (b.translated (0.0f, 1.0f), rad);
        g.setGradientFill (juce::ColourGradient (top, b.getX(), b.getY(),
                                                 bot, b.getX(), b.getBottom(), false));
        g.fillRoundedRectangle (b, rad);
        g.setColour (juce::Colours::white.withAlpha (0.10f));         // brilho no topo
        g.drawLine (b.getX() + 4.0f, b.getY() + 1.2f, b.getRight() - 4.0f, b.getY() + 1.2f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.25f));         // aro
        g.drawRoundedRectangle (b, rad, 1.0f);
    }

private:
    Kind kind = none;
    bool active = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbreGlyph)
};

//==============================================================================
// Fita fina que ACENDE com o sinal do canal. Num palco escuro voce ve num
// relance qual canal esta soando, sem precisar caçar o medidor.
//
// E um componente proprio, e nao um pedaco do card, de proposito: ele repinta a
// 30 Hz e o card tem sombra borrada — repintar o card inteiro nesse ritmo comeria
// CPU que pertence ao audio.
//==============================================================================
class ActivityBar : public juce::Component
{
public:
    ActivityBar() { setInterceptsMouseClicks (false, false); }

    void setLevel (float newLevel)
    {
        newLevel = juce::jlimit (0.0f, 1.0f, newLevel);

        // Limiar maior no touch: menos repaint, mesma leitura de "esta tocando".
       #if JUCE_ANDROID || JUCE_IOS
        const float thresh = 0.06f;
       #else
        const float thresh = 0.03f;
       #endif

        if (std::abs (newLevel - level) > thresh)
        {
            level = newLevel;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (level <= 0.02f)
            return;   // silencio: deixa aparecer o acento normal do card

        auto r = getLocalBounds().toFloat();

       #if JUCE_ANDROID || JUCE_IOS
        // Sem glow expandido (caro no software renderer) — barra solida basta.
        g.setColour (NordLookAndFeel::nordRed().withAlpha (
            juce::jmin (1.0f, 0.30f + level * 1.2f)));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
       #else
        g.setColour (NordLookAndFeel::nordRedBright().withAlpha (level * 0.30f));
        g.fillRoundedRectangle (r.expanded (3.0f, 2.5f), 4.0f);
        g.setColour (NordLookAndFeel::nordRedBright().withAlpha (
            juce::jmin (1.0f, 0.25f + level * 1.5f)));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
       #endif
    }

private:
    float level = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ActivityBar)
};

//==============================================================================
// Janela para a interface de um plugin hospedado dentro do Church Live
//==============================================================================
class InsertPluginWindow : public juce::DocumentWindow
{
public:
    InsertPluginWindow (juce::AudioPluginInstance& p, std::function<void()> onClosed)
        : DocumentWindow (p.getName(), NordLookAndFeel::background(),
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          plugin (p), closedCallback (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);

        if (auto* ed = plugin.createEditorIfNeeded())
            setContentOwned (ed, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (plugin), true);

        setTopLeftPosition (220, 160);
        setVisible (true);
        toFront (true);
    }

    ~InsertPluginWindow() override
    {
        if (auto* ed = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            if (plugin.getActiveEditor() == ed)
                plugin.editorBeingDeleted (ed);

        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (closedCallback)
            closedCallback();
    }

    juce::AudioPluginInstance* getPluginInstance() noexcept
    {
        return &plugin;
    }

private:
    juce::AudioPluginInstance& plugin;
    std::function<void()> closedCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertPluginWindow)
};

//==============================================================================
// Editor de plugin EMBUTIDO "como nativo": card premium DENTRO do Church Live
// (nao e janela do sistema) com moldura da marca, barra de titulo arrastavel e
// o editor real do plugin (BigSky etc.) dentro. Fecha no X.
//==============================================================================
class InsertEditorCard : public juce::Component
{
public:
    InsertEditorCard (juce::AudioPluginInstance& p, std::function<void()> onClosed)
        : plugin (p), closedCallback (std::move (onClosed))
    {
        if (auto* ed = plugin.createEditorIfNeeded())
            editor.reset (ed);
        else
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);

        addAndMakeVisible (*editor);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // Ã—
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (closedCallback) closedCallback(); };
        addAndMakeVisible (closeBtn);

        fitToEditor();
    }

    ~InsertEditorCard() override
    {
        // OBRIGATORIO antes de destruir o editor manualmente (senao o proximo
        // createEditorIfNeeded devolve ponteiro morto)
        if (editor != nullptr && plugin.getActiveEditor() == editor.get())
            plugin.editorBeingDeleted (editor.get());

        editor.reset();
    }

    void childBoundsChanged (juce::Component* c) override
    {
        if (c == editor.get() && ! resizingSelf)
            fitToEditor();   // plugin redimensionou a si mesmo: acompanha
    }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced ((float) margin);

        {
            juce::Path sp;
            sp.addRoundedRectangle (card, 12.0f);
            juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 24, { 0, 7 }).drawForPath (g, sp);
        }

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1c22), card.getX(), card.getY(),
                                                 juce::Colour (0xff101013), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, 12.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.45f));
        g.drawRoundedRectangle (card, 12.0f, 1.2f);

        // barra de titulo: acento + nome do plugin + grip de arrastar
        auto head = card.removeFromTop ((float) headerH);
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (head.reduced (14.0f, 0.0f).removeFromTop (8.0f).withTrimmedTop (5.0f), 1.5f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (13.5f)).boldened().withExtraKerningFactor (0.06f));
        g.drawText (plugin.getName().toUpperCase(), head.toNearestInt().reduced (44, 0),
                    juce::Justification::centred);

        g.setColour (NordLookAndFeel::dimText().withAlpha (0.45f));
        for (int rr = 0; rr < 2; ++rr)
            for (int cc = 0; cc < 3; ++cc)
                g.fillEllipse (head.getX() + 16.0f + (float) cc * 5.0f,
                               head.getCentreY() + 1.0f + (float) rr * 5.0f - 4.0f, 2.2f, 2.2f);
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
        closeBtn.setBounds (card.getRight() - 36, card.getY() + 9, 26, 22);

        if (editor != nullptr)
            editor->setTopLeftPosition (card.getX() + 2, card.getY() + headerH);
    }

    // arrasta o card pela barra de titulo
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = e.position.y < (float) (margin + headerH);
        if (dragging)
            dragger.startDraggingComponent (this, e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging)
            dragger.dragComponent (this, e, nullptr);
    }

    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }

    juce::AudioPluginInstance* getPluginInstance() noexcept
    {
        return &plugin;
    }

private:
    void fitToEditor()
    {
        resizingSelf = true;
        const int ew = juce::jmax (220, editor != nullptr ? editor->getWidth() : 220);
        const int eh = juce::jmax (120, editor != nullptr ? editor->getHeight() : 120);
        setSize (ew + margin * 2 + 4, eh + margin * 2 + headerH + 4);
        resizingSelf = false;
    }

    static constexpr int margin = 12, headerH = 36;

    juce::AudioPluginInstance& plugin;
    std::function<void()> closedCallback;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::TextButton closeBtn;
    juce::ComponentDragger dragger;
    bool dragging = false, resizingSelf = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InsertEditorCard)
};

//==============================================================================
// Editor do Church Live: mesa com 6 reguas verticais - 5 canais de timbre
// (icone, seletor, fader, medidor, M/S, REV/COMP, transposicao, loop),
// MASTER + globais, e painel inferior com reverb nativo e plugins internos.
//==============================================================================
class TimbresEditor : public juce::AudioProcessorEditor,
                      private juce::ChangeListener
{
public:
    explicit TimbresEditor (TimbresProcessor& p)
        : AudioProcessorEditor (p), proc (p)
    {
        proc.addChangeListener (this);

        if (auto* m = proc.getExternalMaster())
            m->addChangeListener (this);

        if (auto* plugins = proc.getKnownPlugins())
            plugins->addChangeListener (this);

        // Desktop: "CHURCH LIVE" em vermelho discreto de secao (nao neon).
        title.setText ("CHURCH LIVE", juce::dontSendNotification);
        title.setFont (NordLookAndFeel::stageLabel (12.0f));
        title.setColour (juce::Label::textColourId, NordLookAndFeel::nordRed().withAlpha (0.92f));
        addAndMakeVisible (title);

        // Navegacao entre os 14 canais (8 aparecem por vez).
        for (auto* b : { &canalAnteriorBtn, &canalProximoBtn })
        {
            b->setColour (juce::TextButton::buttonColourId,
                          juce::Colour (0xff262a31));
            b->setColour (juce::TextButton::textColourOffId,
                          juce::Colours::white.withAlpha (0.92f));
            b->setWantsKeyboardFocus (false);   // seta nao rouba foco no culto
            addChildComponent (*b);
        }

        canalAnteriorBtn.onClick = [this] { rolarCanais (-1); };
        canalProximoBtn.onClick  = [this] { rolarCanais (+1); };
        canalAnteriorBtn.setTooltip ("Canais anteriores");
        canalProximoBtn.setTooltip ("Proximos canais");

        status.setJustificationType (juce::Justification::centredRight);
        status.setFont (juce::Font (juce::FontOptions (12.0f)));
        status.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (status);

        editToneButton.setButtonText ("Editar som");
        editToneButton.setTooltip (
            "Abrir o canal selecionado em modo focado, sem interromper o audio");
        // Desktop "EDITAR SOM": chapa grafite, nao bloco vermelho solido.
        editToneButton.setColour (juce::TextButton::buttonColourId,
                                  juce::Colour (0xff252a32));
        editToneButton.setColour (juce::TextButton::buttonOnColourId,
                                  juce::Colour (0xff2e343e));
        editToneButton.setColour (juce::TextButton::textColourOffId,
                                  NordLookAndFeel::txtSecondary());
        editToneButton.setColour (juce::TextButton::textColourOnId,
                                  NordLookAndFeel::txtPrimary());
        editToneButton.onClick = [this]
        {
           #if JUCE_ANDROID
            // No touch, o editor focado continua sendo a mesa de edicao. O
            // Modo Palco abre as reguas compactas com fader, meter, M/S e
            // master atualizando juntas, como no Windows/macOS. A troca e so
            // visual: attachments e audio nunca sao desmontados.
            mobileStageMode = ! mobileStageMode;
            mobileMasterPage = false;
            editToneButton.setToggleState (mobileStageMode,
                                           juce::dontSendNotification);
            editToneButton.setButtonText (mobileStageMode
                                            ? "Editar canal"
                                            : "Modo palco");
            editToneButton.setTooltip (mobileStageMode
                ? "Voltar ao canal em foco para editar todos os controles"
                : "Ver faders, meters, Mute, Solo e Master em tempo real");

            // O layout conhece quantas reguas cabem nesta tela. Ele centraliza
            // o canal editado na primeira pagina somente depois de calcular
            // esse numero, evitando paginas parciais no fim da lista.
            androidStageRevealSelected = mobileStageMode;

            resized();
            repaint();
           #elif ! JUCE_IOS
            desktopToneEditMode = ! desktopToneEditMode;
            mobileMasterPage = false;
            editToneButton.setToggleState (desktopToneEditMode,
                                           juce::dontSendNotification);
            editToneButton.setButtonText (
                desktopToneEditMode ? "Voltar ao mixer" : "Editar som");
            editToneButton.setTooltip (
                desktopToneEditMode
                    ? "Voltar para a visao geral dos cinco canais"
                    : "Abrir o canal selecionado em modo focado, sem interromper o audio");
            resized();
            repaint();
           #endif
        };
        addAndMakeVisible (editToneButton);

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];
            auto& mobileTab = mobilePageButtons[l];

            // As abas eram apenas retangulos pintados tratados em mouseDown:
            // TalkBack, teclado e D-pad nao conseguiam sequer encontra-las.
            // O botao transparente fornece semantica/foco e deixa o desenho
            // premium continuar centralizado em paint().
            mobileTab.setButtonText ("Canal " + juce::String (l + 1));
            mobileTab.setTooltip (
                "Abrir controles do canal " + juce::String (l + 1));
            mobileTab.setColour (juce::TextButton::buttonColourId,
                                 juce::Colours::transparentBlack);
            mobileTab.setColour (juce::TextButton::buttonOnColourId,
                                 juce::Colours::transparentBlack);
            mobileTab.setColour (juce::TextButton::textColourOffId,
                                 juce::Colours::transparentBlack);
            mobileTab.setColour (juce::TextButton::textColourOnId,
                                 juce::Colours::transparentBlack);
            mobileTab.setWantsKeyboardFocus (true);
            mobileTab.onClick = [this, l] { activateMobileLayerPage (l); };
            addAndMakeVisible (mobileTab);

            s.name.setText ("Canal " + juce::String (l + 1), juce::dontSendNotification);
            s.name.setJustificationType (juce::Justification::centred);
            // Rotulo de secao: sentence case (menos grito); M/S/ON/MASTER em caps.

            // espacado. O canal em foco fica ACESO em vermelho, os outros apagados
            // -- e a mesma ideia da "rotulagem em LED" deles para palco escuro.
            s.name.setFont (NordLookAndFeel::stageLabel (11.0f));
            s.name.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
            addAndMakeVisible (s.name);

            // botao liga/desliga do canal
            s.onBtn.setClickingTogglesState (true);
            // ON: vermelho da marca (como desktop), sem bright neon.
            s.onBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
            s.onBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a3d44));
            s.onBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            s.onBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::dimText());
            // texto muda ON/OFF conforme o estado (nao ambiguo)
            s.onBtn.onStateChange = [this, l]
            {
                strips[l].onBtn.setButtonText (strips[l].onBtn.getToggleState() ? "ON" : "OFF");
            };
            // NAO seleciona o canal aqui. O ButtonParameterAttachment chama
            // setToggleState com notificacao ao restaurar o estado salvo, e isso
            // dispara onClick -- os 5 canais eram "clicados" na abertura e o
            // ultimo ficava em foco. Alem disso, ligar/desligar um canal nao e o
            // mesmo gesto que "quero editar este canal": o clique no card cuida
            // do foco (ver mouseDown).
            s.onAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].on, s.onBtn);
            s.onBtn.setButtonText (s.onBtn.getToggleState() ? "ON" : "OFF");
            addAndMakeVisible (s.onBtn);

            addAndMakeVisible (s.glyph);

            // HIERARQUIA INVERTIDA: a categoria ("Piano", "Pad") e FIXA -- quem
            // muda, e portanto quem carrega informacao, e o preset. A categoria
            // gritava em 17px negrito e competia com o nome do som logo abaixo.
            // Agora ela e rotulo de apoio; o heroi e o seletor de preset.
            s.timbreName.setJustificationType (juce::Justification::centredLeft);
           #if JUCE_ANDROID || JUCE_IOS
            s.timbreName.setFont (NordLookAndFeel::stageLabel (11.0f));
           #else
            // Categoria precisa caber entre o icone e o ON inclusive em
            // "Instrumento"; sem kerning extra ela continua firme e nao vira
            // "Stri..." nos strips de 720p.
            s.timbreName.setFont (juce::Font (juce::FontOptions (
                10.25f, juce::Font::bold)));
           #endif
            s.timbreName.setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
            s.timbreName.setMinimumHorizontalScale (0.7f);
            addAndMakeVisible (s.timbreName);

            // Card vazio (desktop): texto cinza, sem chapa rosa.
            s.addButton.setButtonText ("Adicionar");
            s.addButton.setColour (juce::TextButton::buttonColourId,
                                   juce::Colours::transparentBlack);
            s.addButton.setColour (juce::TextButton::textColourOffId,
                                   NordLookAndFeel::txtSecondary());
            s.addButton.onClick = [this, l]
            {
                // Abre o seletor (com TIMBRES PARA BAIXAR) mesmo sem pack
                // completo instalado. So manda para Bibliotecas se o seletor
                // ainda nao tiver nem organ engine nem catalogo baixavel.
                selectStrip (l);
                openPickerFor (l);
            };
            addChildComponent (s.addButton);

            s.combo.setTextWhenNothingSelected ("Adicionar");
            // o menu padrao do JUCE nao tem busca; com centenas de timbres isso
            // virava rolagem infinita — clicar no combo abre o seletor com busca
            s.combo.onShowPopup = [this, l] { openPickerFor (l); };

            // MIDI de entrada por canal (cliente Mac/Windows: 2 teclados).
            s.midiInBox.setTextWhenNothingSelected (
                juce::String::fromUTF8 ("MIDI: Todos"));
            s.midiInBox.setTooltip (
                juce::String::fromUTF8 (
                    "Qual teclado controla este canal. "
                    "Ex.: teclado 1 no piano, teclado 2 no pad."));
            s.midiInBox.onChange =
                [this, l, lifetime = editorCallbackLifetime]
            {
                if (! lifetime->load (std::memory_order_acquire)
                     || updatingCombos)
                    return;

                selectStrip (l);
                const int id = strips[l].midiInBox.getSelectedId();
                if (id <= 1)
                {
                    proc.setLayerMidiInputDevice (l, {});
                    return;
                }

                // id 2..n mapeia para devices na mesma ordem da lista.
                const auto devices = juce::MidiInput::getAvailableDevices();
                const int index = id - 2;
                if (juce::isPositiveAndBelow (index, devices.size()))
                    proc.setLayerMidiInputDevice (
                        l, devices.getReference (index).identifier);
                else
                {
                    // Item offline: extrai o id do texto "MIDI: xxx (offline)".
                    auto t = strips[l].midiInBox.getText();
                    if (t.startsWith ("MIDI: "))
                        t = t.fromFirstOccurrenceOf ("MIDI: ", false, false);
                    t = t.upToFirstOccurrenceOf (" (offline)", false, false).trim();
                    proc.setLayerMidiInputDevice (l, t);
                }
            };
            addAndMakeVisible (s.midiInBox);
            s.combo.onChange =
                [this, l, lifetime = editorCallbackLifetime]
            {
                if (! lifetime->load (std::memory_order_acquire)
                     || updatingCombos)
                    return;

                selectStrip (l);

                const int id = strips[l].combo.getSelectedId();
                const auto& entries = proc.getEntriesNonBlocking();

                // a janela do VSTi guarda referencia ao AudioPluginInstance:
                // fecha ANTES de qualquer ramo destruir/trocar o plugin do canal
                layerPluginWindows[l].reset();

                if (churchOrganOverlayLayer == l)
                    closeChurchOrganEditor();

                if (id >= downloadBaseId)
                {
                    // Pack online: nao e um timbre selecionavel no combo.
                    return;
                }
                else if (id == 1)
                {
                    proc.clearLayer (l);
                    proc.clearInstrumentPlugin (l);
                }
                else if (id >= organBaseId
                          && id - organBaseId
                               < TimbresProcessor::
                                     churchOrganFamilyCount)
                {
                    const auto family =
                        static_cast<
                            churchorgan::ChurchOrganFamily> (
                                id - organBaseId);

                    if (proc.loadChurchOrganIntoLayer (
                            l, family))
                        openChurchOrganEditor (l);
                    else
                        refreshFromProcessor();
                }
                else if (id >= vstBaseId && id - vstBaseId < instrumentsCache.size())
                {
                    // AUv3 e alguns Audio Units concluem em outro ciclo da
                    // message thread. Só abra o editor e altere os controles
                    // depois do commit real; até lá o instrumento anterior toca.
                    auto safe =
                        juce::Component::SafePointer<TimbresEditor> (
                            this);
                    std::weak_ptr<std::atomic<bool>> lifetime (
                        editorCallbackLifetime);
                    const bool accepted = proc.loadInstrumentPlugin (
                        l,
                        instrumentsCache.getReference (id - vstBaseId),
                        true, nullptr,
                        [safe, lifetime, l] (
                            bool loaded, const juce::String&)
                        {
                            const auto alive = lifetime.lock();

                            if (alive == nullptr
                                 || ! alive->load (
                                     std::memory_order_acquire))
                                return;

                            if (auto* editor = safe.getComponent())
                            {
                                if (loaded)
                                {
                                    *editor->proc.layers[l].on = true;
                                    *editor->proc.layers[l].mute = false;
                                    *editor->proc.layers[l].solo = false;
                                    editor->openLayerPlugin (l);
                                }
                                else
                                {
                                    // O canal anterior permaneceu intacto.
                                    editor->refreshFromProcessor();
                                }
                            }
                        });

                    if (! accepted)
                    {
                        refreshFromProcessor();
                    }
                }
                else if (id >= 2 && id - 2 < entries.size())
                {
                    const auto& entry = entries.getReference (id - 2);
                    // O timbre e os controles atuais permanecem intactos
                    // enquanto o decode acontece. Defaults só entram no commit
                    // bem-sucedido da mesma geração.
                    proc.loadTimbreIntoLayer (l, entry, true);
                }
            };
            // No Android, D-pad/teclado precisa alcancar o seletor. No desktop
            // ele segue fora da ordem de foco para uma seta acidental nao trocar
            // o timbre durante o culto.
           #if JUCE_ANDROID
            s.combo.setWantsKeyboardFocus (true);
           #else
            s.combo.setWantsKeyboardFocus (false);
           #endif
            addAndMakeVisible (s.combo);

            s.vol.setSliderStyle (juce::Slider::LinearVertical);
            // NUNCA foco de teclado em slider: o Slider do JUCE responde as
            // setas do teclado do PC quando focado — ao vivo, uma seta mudava
            // o fader/transpose sem o musico perceber
            s.vol.setWantsKeyboardFocus (false);
            s.vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.vol.onDragStart = [this, l] { selectStrip (l); };
            // rotulo de dB proprio (padrao de mixer, nao o ganho interno)
            s.vol.onValueChange = [this, l]
            {
                const double v = strips[l].vol.getValue();
                const bool silent = v <= 0.0009;
                const float db = silent ? -100.0f : (float) juce::Decibels::gainToDecibels (v);

                const auto readout = silent
                    ? juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e dB")
                    : (db >= 0.0f ? "+" : juce::String())
                        + juce::String (db, 1) + " dB";

                strips[l].volDb.setText (readout,
                                         juce::dontSendNotification);

                // Cor avisa quando o canal esta quente. MESMA escala do readout do
                // MASTER (ambar acima de 0, vermelho acima de +3): antes o canal
                // ficava VERMELHO ja em +0,6 dB enquanto o master no mesmo valor
                // ficava so ambar — dois avisos com significados diferentes pra
                // mesma coisa, e o vermelho perdia o peso de "cuidado".
                const auto c = db > 3.0f  ? juce::Colour (0xffd83a3a)       // vermelho: impulso forte
                              : db > 0.05f ? juce::Colour (0xffe0a83a)      // ambar: acima da unidade
                                           : juce::Colours::white.withAlpha (0.92f);
                strips[l].volDb.setColour (juce::Label::textColourId, c);
                strips[l].volDb.repaint();
            };
            s.volAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].vol, s.vol);
            addAndMakeVisible (s.vol);

            s.volDb.setJustificationType (juce::Justification::centred);
            // Readout de mixer: caixa fixa + algarismos de largura fixa. Isso
            // alinha "+3.5", "-1.2" e "−∞" na mesma linha em todos os canais.
            s.volDb.setFont (juce::Font (juce::FontOptions (
                juce::Font::getDefaultMonospacedFontName(), 14.0f,
                juce::Font::bold)));
            s.volDb.setColour (juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible (s.volDb);
            s.vol.onValueChange();   // texto inicial

            // Medidor ESTEREO por canal. O processador zera o pico ao ler, entao
            // as duas barras nao podem ler cada uma por si: a primeira consulta do
            // quadro busca L e R juntos e guarda; a segunda so devolve o guardado.
            s.meter = std::make_unique<LevelMeter> (2,
                [this, l] (int ch) -> float
                {
                    auto& st = strips[l];

                    if (ch == 0)
                        proc.readAndResetLayerLevels (l, st.pendingL, st.pendingR);

                    return ch == 0 ? st.pendingL : st.pendingR;
                });
            // o card acende junto com o medidor, no mesmo quadro
            s.meter->onLevel = [this, l] (float level) { strips[l].activity.setLevel (level); };
            addAndMakeVisible (*s.meter);

            addAndMakeVisible (s.activity);

            s.transp.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.transp.setWantsKeyboardFocus (false);   // setas do PC nao mexem no tom
            s.transp.setScrollWheelEnabled (false);   // trackpad do Mac nao muda afinacao ao rolar
            s.transp.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
            s.transp.setTextValueSuffix (" st");
            s.transp.setDoubleClickReturnValue (true, 0.0);
            s.transp.setTooltip ("TRANSPOSE — afinação do canal em semitons. Duplo-clique = 0");
            s.transp.onDragStart = [this, l] { selectStrip (l); };
            s.transpAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].transp, s.transp);
            addAndMakeVisible (s.transp);

            s.transpLabel.setText ("TRANSPOSE", juce::dontSendNotification);
            s.transpLabel.setJustificationType (juce::Justification::centred);
            s.transpLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
            s.transpLabel.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
            addAndMakeVisible (s.transpLabel);

            s.loop.setButtonText ("Loop");
            s.loop.setClickingTogglesState (true);
            s.loop.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedBright());
            s.loop.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            s.loopAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].loop, s.loop);
            addAndMakeVisible (s.loop);

            // canal-plugin: botao para abrir a interface do Kontakt/VST
            s.pluginOpenBtn.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::nordRedDark());
            s.pluginOpenBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            s.pluginOpenBtn.onClick = [this, l] { selectStrip (l); openLayerPlugin (l); };
            addChildComponent (s.pluginOpenBtn);

            // botao Auto EQ (mix de louvor por tipo): liga aplica, desliga volta ao original
            s.autoEqBtn.setClickingTogglesState (true);
            s.autoEqBtn.setTooltip ("Auto EQ do canal");
            s.autoEqBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedBright());
            s.autoEqBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            // sem selectStrip aqui pelo mesmo motivo do botao ON: o attachment
            // dispara onClick ao restaurar o estado e roubava o foco na abertura
            s.autoEqBtn.onClick = [this, l] { proc.updateLayerEq (l); };
            s.autoEqAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].autoEq, s.autoEqBtn);
            addAndMakeVisible (s.autoEqBtn);

            s.muteBtn.setClickingTogglesState (true);
            // Palco escuro: M ativo = ambar (atencao), S ativo = verde (foco).
            // Vermelho fica para ON e PANIC — estados criticos de marca.
            s.muteBtn.setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (0xff2a2e36));
            s.muteBtn.setColour (juce::TextButton::buttonOnColourId,
                                 NordLookAndFeel::amberSolo());
            s.muteBtn.setColour (juce::TextButton::textColourOffId,
                                 NordLookAndFeel::txtSecondary());
            s.muteBtn.setColour (juce::TextButton::textColourOnId,
                                 juce::Colours::black);
            s.muteAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].mute, s.muteBtn);
            addAndMakeVisible (s.muteBtn);

            s.soloBtn.setClickingTogglesState (true);
            s.soloBtn.setColour (juce::TextButton::buttonColourId,
                                 juce::Colour (0xff2a2e36));
            s.soloBtn.setColour (juce::TextButton::buttonOnColourId,
                                 NordLookAndFeel::greenOk());
            s.soloBtn.setColour (juce::TextButton::textColourOffId,
                                 NordLookAndFeel::txtSecondary());
            s.soloBtn.setColour (juce::TextButton::textColourOnId,
                                 juce::Colours::black);
            s.soloAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].solo, s.soloBtn);
            addAndMakeVisible (s.soloBtn);

            // Duas paginas compactas preservam a leitura nos cinco canais:
            // ENV/FX = COMP, REVERB, DELAY, ATTACK, RELEASE
            // EQ     = LOW, MID, HIGH
            // Sete knobs espremidos numa unica fileira ficariam cegos no
            // celular; a pagina troca os controles, nunca os valores.
            for (auto* knob : { &s.compKnob, &s.dlyKnob, &s.echoKnob,
                                &s.rotaryKnob,
                                &s.attackKnob, &s.releaseKnob,
                                &s.lowKnob, &s.midKnob, &s.highKnob })
            {
                knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
               #if JUCE_ANDROID
                // D-pad/teclado em tablet precisa alcançar os controles de
                // edição. Transpose continua sem foco para não mudar o tom por
                // acidente ao vivo; aqui o foco é explícito e seguro.
                knob->setWantsKeyboardFocus (true);
               #else
                knob->setWantsKeyboardFocus (false);
               #endif
                knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                knob->onDragStart = [this, l] { selectStrip (l); };
                knob->setPopupDisplayEnabled (true, false, this);

                addAndMakeVisible (*knob);
            }

            for (auto* knob : { &s.compKnob, &s.dlyKnob, &s.echoKnob })
            {
                knob->setDoubleClickReturnValue (true, 0.0);
                knob->textFromValueFunction = [] (double value)
                {
                    return juce::String (
                        juce::roundToInt (value * 100.0)) + "%";
                };
            }

            s.rotaryKnob.setDoubleClickReturnValue (true, 0.65);
            s.rotaryKnob.textFromValueFunction = [] (double value)
            {
                return juce::String (juce::roundToInt (value * 100.0)) + "%";
            };

            const auto formatEnvelopeTime = [] (double seconds)
            {
                if (seconds < 1.0)
                    return juce::String (
                        juce::roundToInt (seconds * 1000.0)) + " ms";

                return juce::String (seconds, seconds < 10.0 ? 2 : 1) + " s";
            };

            s.attackKnob.setDoubleClickReturnValue (true, 0.003);
            s.releaseKnob.setDoubleClickReturnValue (true, 0.55);
            s.attackKnob.textFromValueFunction = formatEnvelopeTime;
            s.releaseKnob.textFromValueFunction = formatEnvelopeTime;

            for (auto* knob : { &s.lowKnob, &s.midKnob, &s.highKnob })
            {
                knob->setDoubleClickReturnValue (true, 0.0);
                knob->textFromValueFunction = [] (double value)
                {
                    const auto prefix = value > 0.049 ? "+" : juce::String();
                    return prefix + juce::String (value, 1) + " dB";
                };
            }

            s.compKnob.setTooltip (juce::String::fromUTF8 ("COMP \xe2\x80\x94 compress\xc3\xa3o (punch/cola). Duplo-clique = 0"));
            s.dlyKnob .setTooltip (juce::String::fromUTF8 ("REVERB \xe2\x80\x94 quanto deste canal entra no ambiente worship. Duplo-clique = 0"));
            s.echoKnob.setTooltip (juce::String::fromUTF8 ("DELAY \xe2\x80\x94 eco do canal (~380 ms). Duplo-clique = 0"));
            s.rotaryKnob.setTooltip ("ROTARY — profundidade do Leslie de órgão. Duplo-clique = 65%");
            s.attackKnob.setTooltip ("ATTACK — tempo para o timbre chegar ao volume total");
            s.releaseKnob.setTooltip ("RELEASE — cauda depois de soltar a tecla ou o pedal");
            s.lowKnob .setTooltip ("LOW — graves em dB. Centro = neutro");
            s.midKnob .setTooltip ("MID — corpo e presença em dB. Centro = neutro");
            s.highKnob.setTooltip ("HIGH — brilho e ar em dB. Centro = neutro");

            s.compAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].comp, s.compKnob);
            s.dlyAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].reverbSend, s.dlyKnob);
            s.echoAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].delay, s.echoKnob);
            s.rotaryAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].rotaryAmount, s.rotaryKnob);
            s.attackAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].attack, s.attackKnob);
            s.releaseAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].release, s.releaseKnob);
            s.lowAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].eqLow, s.lowKnob);
            s.midAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].eqMid, s.midKnob);
            s.highAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].eqHigh, s.highKnob);

            s.compLabel.setText ("COMP", juce::dontSendNotification);
            s.dlyLabel.setText ("REVERB", juce::dontSendNotification);
            s.echoLabel.setText ("DELAY", juce::dontSendNotification);
            s.rotaryLabel.setText ("ROTARY", juce::dontSendNotification);
            // Abreviacoes consagradas em mixers/synths: continuam legiveis
            // mesmo com cinco strips lado a lado; o tooltip mostra o nome
            // completo e a funcao.
            s.attackLabel.setText ("ATK", juce::dontSendNotification);
            s.releaseLabel.setText ("REL", juce::dontSendNotification);
            s.lowLabel.setText ("LOW", juce::dontSendNotification);
            s.midLabel.setText ("MID", juce::dontSendNotification);
            s.highLabel.setText ("HIGH", juce::dontSendNotification);

            for (auto* lb : { &s.compLabel, &s.dlyLabel, &s.echoLabel,
                              &s.rotaryLabel,
                              &s.attackLabel, &s.releaseLabel,
                              &s.lowLabel, &s.midLabel, &s.highLabel })
            {
                lb->setJustificationType (juce::Justification::centred);
                lb->setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
                lb->setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
                addAndMakeVisible (*lb);
            }

            // Caixa estreita no celular: capitalizacao normal evita o "S..."
            // sem sacrificar a leitura no desktop.
            s.rotaryModeBox.addItem ("Slow", 1);
            s.rotaryModeBox.addItem ("Stop", 2);
            s.rotaryModeBox.addItem ("Fast", 3);
            s.rotaryModeBox.setTooltip ("Velocidade do Leslie: Slow, Stop ou Fast");
           #if JUCE_ANDROID
            s.rotaryModeBox.setWantsKeyboardFocus (true);
           #else
            s.rotaryModeBox.setWantsKeyboardFocus (false);
           #endif
            s.rotaryModeAttachment =
                std::make_unique<juce::ComboBoxParameterAttachment> (
                    *proc.layers[l].rotaryMode, s.rotaryModeBox);
            addAndMakeVisible (s.rotaryModeBox);

            s.tonePageBtn.setButtonText ("EQ");
            s.tonePageBtn.setTooltip ("Alternar entre envelope/efeitos e EQ");
            s.tonePageBtn.onClick = [this, l]
            {
                strips[l].eqPage = ! strips[l].eqPage;
                updateTonePage (l);
            };
            addAndMakeVisible (s.tonePageBtn);
            updateTonePageVisibility (l);
        }

        mobileMasterPageButton.setButtonText ("Master");
        mobileMasterPageButton.setTooltip ("Abrir controles do master");
        mobileMasterPageButton.setColour (juce::TextButton::buttonColourId,
                                          juce::Colours::transparentBlack);
        mobileMasterPageButton.setColour (juce::TextButton::buttonOnColourId,
                                          juce::Colours::transparentBlack);
        mobileMasterPageButton.setColour (juce::TextButton::textColourOffId,
                                          juce::Colours::transparentBlack);
        mobileMasterPageButton.setColour (juce::TextButton::textColourOnId,
                                          juce::Colours::transparentBlack);
        mobileMasterPageButton.setWantsKeyboardFocus (true);
        mobileMasterPageButton.onClick = [this] { activateMobileMasterPage(); };
        addAndMakeVisible (mobileMasterPageButton);

        // regua do MASTER da DAW (fader + medidor grandes) e controles globais
        masterName.setText ("MASTER", juce::dontSendNotification);
        masterName.setJustificationType (juce::Justification::centred);
        masterName.setFont (juce::Font (juce::FontOptions (13.0f)).boldened().withExtraKerningFactor (0.10f));
        // branco, nao vermelho: a coluna master ja tinha 6 elementos vermelhos
        // empilhados — quando tudo e vermelho, nada se destaca
        masterName.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (masterName);

        masterDbSlider.setSliderStyle (juce::Slider::LinearVertical);
        masterDbSlider.setWantsKeyboardFocus (false);   // setas do PC nao mexem no master
        masterDbSlider.setRange (MasterProcessor::minusInfinityDb, MasterProcessor::maxGainDb, 0.1);
        // Meio da curva em -6: poe o 0 dB a ~66% do curso, MESMA posicao
    // visual da unidade nos faders de canal (ganho 1,0 em 0..1,5). Com o
    // meio em -12 a unidade caia a ~75% e o master parecia "topado" ao
    // lado dos canais (apontado pelo dono em 30/07).
    masterDbSlider.setSkewFactorFromMidPoint (-6.0);
        masterDbSlider.setDoubleClickReturnValue (true, 0.0);
        // caixa de texto maior: o dB do master e o numero que o tecladista mais
        // consulta na mesa, e estava do mesmo tamanho dos dB de canal
        masterDbSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 22);
        masterDbSlider.textFromValueFunction = [] (double value)
        {
            if (value <= MasterProcessor::minusInfinityDb + 0.05)
                return juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e dB");

            return (value >= 0.0 ? "+" : juce::String())
                    + juce::String (value, 1) + " dB";
        };
        masterDbSlider.onValueChange = [this]
        {
            if (auto* m = proc.getExternalMaster())
                if (! syncingMaster)
                    m->setMasterDb ((float) masterDbSlider.getValue());
            refreshMasterDbColour();
        };
        addAndMakeVisible (masterDbSlider);

        if (auto* m = proc.getExternalMaster())
        {
            masterDbSlider.setValue (m->getMasterDb(), juce::dontSendNotification);
            refreshMasterDbColour();

            masterMeter = std::make_unique<LevelMeter> (2,
                [m] (int ch) { return m->readAndResetLevel (ch); },
                [m]          { return m->readAndResetClip(); });
            masterMeter->setChannelLabels (true);
            masterMeter->setPeakReadout (true);
            addAndMakeVisible (*masterMeter);
        }

        // PANIC contido: chapa escura + contorno vermelho (ver NordLookAndFeel
        // drawButtonBackground com componentID "panicOutline").
        panicButton.setComponentID ("panicOutline");
        panicButton.setColour (juce::TextButton::buttonColourId,
                               juce::Colour (0xff1a1014));
        panicButton.setColour (juce::TextButton::textColourOffId,
                               NordLookAndFeel::nordRed());
        panicButton.onClick = [this] { proc.panic(); };
        addAndMakeVisible (panicButton);

        gTranspSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        gTranspSlider.setWantsKeyboardFocus (false);   // setas do PC nao mexem no tom
        gTranspSlider.setScrollWheelEnabled (false);   // gesto de pagina nao pode transpor o show
        gTranspSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
        gTranspSlider.setTextValueSuffix (" st");
        gTranspSlider.setDoubleClickReturnValue (true, 0.0);
        gTranspSlider.setTooltip ("TRANSPOSE GLOBAL — duplo-clique repõe a afinação em 0");
        gTranspAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.globalTransp, gTranspSlider);
        addAndMakeVisible (gTranspSlider);

        gTranspLabel.setText ("Transpose global", juce::dontSendNotification);
        gTranspLabel.setJustificationType (juce::Justification::centred);
        gTranspLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
        gTranspLabel.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (gTranspLabel);

        // O mesmo seletor controla o Church Reverb nativo e, quando houver um
        // BigSky carregado no computador, a maquina equivalente do plugin.
        for (int i = 0; i < TimbresProcessor::numReverbMachines; ++i)
            reverbTypeCombo.addItem (TimbresProcessor::reverbMachineName (i), i + 1);

        reverbTypeCombo.setWantsKeyboardFocus (false);   // setas do PC nao trocam a maquina
        reverbTypeCombo.setTextWhenNothingSelected ("Reverb");
        reverbTypeCombo.onChange = [this]
        {
            const int id = reverbTypeCombo.getSelectedId();

            if (id > 0 && ! updatingCombos)
                proc.setReverbMachine (id - 1);
        };
        addChildComponent (reverbTypeCombo);

        reverbReturnSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        reverbReturnSlider.setWantsKeyboardFocus (false);
        reverbReturnSlider.setDoubleClickReturnValue (true, 0.72);
        reverbReturnSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        reverbReturnSlider.setTooltip (juce::String::fromUTF8 (
            "RETURN \xe2\x80\x94 volume geral da cauda de reverb. Duplo-clique = 72%"));
        reverbReturnAttachment = std::make_unique<juce::SliderParameterAttachment> (
            *proc.reverbReturn, reverbReturnSlider);
        // SliderParameterAttachment instala seus proprios conversores no
        // construtor; sobrescrevemos DEPOIS para o readout ser 72%, nao 0.72.
        reverbReturnSlider.textFromValueFunction = [] (double v)
        {
            return juce::String (juce::roundToInt (v * 100.0)) + "%";
        };
        reverbReturnSlider.valueFromTextFunction = [] (const juce::String& text)
        {
            return text.retainCharacters ("0123456789.,").replaceCharacter (',', '.')
                       .getDoubleValue() / 100.0;
        };
        reverbReturnSlider.updateText();
        addChildComponent (reverbReturnSlider);

        reverbReturnLabel.setText ("RETURN", juce::dontSendNotification);
        reverbReturnLabel.setJustificationType (juce::Justification::centredLeft);
        reverbReturnLabel.setFont (NordLookAndFeel::stageLabel (9.0f));
        reverbReturnLabel.setColour (juce::Label::textColourId,
                                     NordLookAndFeel::dimText());
        addChildComponent (reverbReturnLabel);

        sustainToggle.setButtonText ("Sustain");
        sustainToggle.setTooltip ("Sustain global: mantém as notas mesmo sem pedal físico");
        sustainAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.globalSustain, sustainToggle);
        addAndMakeVisible (sustainToggle);

        invPedalToggle.setButtonText ("Inverter pedal");
        invPedalToggle.setTooltip ("Inverter a polaridade do pedal de sustain");
        invPedalAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.pedalInvert, invPedalToggle);
        addAndMakeVisible (invPedalToggle);


        // Auto EQ de masterizacao worship no bus inteiro
        masterAutoEqBtn.setButtonText ("Auto EQ Master");
        masterAutoEqBtn.setClickingTogglesState (true);
        masterAutoEqBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedDark());
        masterAutoEqBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);

        if (auto* m = proc.getExternalMaster())
            masterAutoEqBtn.setToggleState (m->getAutoEq(), juce::dontSendNotification);

        masterAutoEqBtn.onClick = [this]
        {
            if (auto* m = proc.getExternalMaster())
                m->setAutoEq (masterAutoEqBtn.getToggleState());

            refreshMasterUtilityLabels();
        };
        masterAutoEqBtn.setTooltip ("Auto EQ Master: curva global otimizada para mix de louvor");
        addAndMakeVisible (masterAutoEqBtn);

        masterPunchBtn.setButtonText ("Punch / Peso");
        masterPunchBtn.setClickingTogglesState (true);
        masterPunchBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedDark());
        masterPunchBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);

        if (auto* m = proc.getExternalMaster())
            masterPunchBtn.setToggleState (m->getPunch(), juce::dontSendNotification);

        masterPunchBtn.onClick = [this]
        {
            if (auto* m = proc.getExternalMaster())
                m->setPunch (masterPunchBtn.getToggleState());

            refreshMasterUtilityLabels();
        };
        masterPunchBtn.setTooltip ("Punch / Peso: reforço de impacto e corpo no master");
        addAndMakeVisible (masterPunchBtn);
        refreshMasterUtilityLabels();

        // ---- painel inferior: plugins de efeito no canal master (BigSky etc.) ----
        fxTitle.setText ("MASTER FX", juce::dontSendNotification);
        fxTitle.setFont (juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.08f));
        fxTitle.setJustificationType (juce::Justification::centred);
        fxTitle.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());   // subtitulo, nao marca
        addAndMakeVisible (fxTitle);

        rebuildPluginCaches();

        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            insertCombos[i].addItem ("Sem efeito", 1);

            for (int e = 0; e < effectsCache.size(); ++e)
                insertCombos[i].addItem (
                    ChurchDAWPluginLoading::displayName (
                        effectsCache.getReference (e), effectsCache),
                    e + 2);

            insertCombos[i].setTextWhenNothingSelected ("Sem efeito");
            insertCombos[i].setWantsKeyboardFocus (false);   // setas do PC nao trocam o efeito
            insertCombos[i].onChange =
                [this, i, lifetime = editorCallbackLifetime]
            {
                if (! lifetime->load (std::memory_order_acquire)
                     || updatingCombos)
                    return;

                insertWindows[i].reset();   // fecha o editor embutido antes de trocar

                const int id = insertCombos[i].getSelectedId();

                if (id == 1)
                    proc.unloadInsert (i);
                else if (id >= 2 && id - 2 < effectsCache.size())
                    proc.loadInsert (i, effectsCache.getReference (id - 2));
            };
            addAndMakeVisible (insertCombos[i]);

            insertOpenButtons[i].setButtonText ("Editar");
            insertOpenButtons[i].setTooltip ("Abrir o editor do efeito carregado");
            insertOpenButtons[i].onClick = [this, i] { openInsertEmbedded (i); };
            addAndMakeVisible (insertOpenButtons[i]);
        }

        // Biblioteca vazia DEPOIS de uma varredura concluida (ex.: o usuario
        // acabou de instalar o pack): tenta novamente. Antes, o editor abria
        // enquanto o indice rapido ainda estava na thread de fundo, via a lista
        // momentaneamente vazia e cancelava o cache para iniciar uma varredura
        // completa de 9 mil arquivos. Era a causa dos ~40 s de "Lendo
        // biblioteca..." em toda abertura no Android.
        if (proc.getEntriesNonBlocking().isEmpty() && proc.hasScannedLibrary())
            proc.rescanLibrary();

        populateCombos();
        refreshFromProcessor();
        // Puxa o catalogo de timbres individuais cedo (rede), para a secao
        // "TIMBRES PARA BAIXAR" ja existir quando o musico abrir o seletor.
        ensureTimbreCatalogLoaded();
        setSize (960, 622);

        auto safe = juce::Component::SafePointer<TimbresEditor> (
            this);
        std::weak_ptr<std::atomic<bool>> lifetime (
            editorCallbackLifetime);
        proc.setHostedPluginWillRetireCallback (
            [safe, lifetime] (
                TimbresProcessor::HostedPluginLocation location,
                int index,
                juce::AudioPluginInstance* instance)
            {
                const auto alive = lifetime.lock();

                if (alive == nullptr
                     || ! alive->load (std::memory_order_acquire))
                    return;

                if (auto* editor = safe.getComponent())
                    editor->closeHostedPluginEditorBeforeRetire (
                        location, index, instance);
            });
    }

    ~TimbresEditor() override
    {
        // SafePointer continua resolvendo durante o corpo do destrutor. Feche a
        // porta das completions primeiro e bloqueie commits AUv3 enquanto os
        // editores de vendor (que podem bombear mensagens) sao desmontados.
        editorCallbackLifetime->store (
            false, std::memory_order_release);

        // Listeners podem ter callbacks ja enfileirados. Desconecte todas as
        // fontes antes de resetar qualquer editor de vendor, cujo destrutor pode
        // bombear a fila de mensagens e reentrar neste objeto em teardown.
        if (auto* m = proc.getExternalMaster())
            m->removeChangeListener (this);

        if (auto* plugins = proc.getKnownPlugins())
            plugins->removeChangeListener (this);

        proc.removeChangeListener (this);

        proc.beginHostedPluginEditorTeardown();
        {
            const juce::ScopeGuard finishEditorTeardown { [this]
            {
                proc.endHostedPluginEditorTeardown();
            } };

            proc.setHostedPluginWillRetireCallback ({});

            for (auto& w : insertWindows)
                w.reset();

            for (auto& w : layerPluginWindows)
                w.reset();
        }

        churchOrganOverlay.reset();
    }

    bool isMobileMasterPageActive() const noexcept
    {
        return focusedMobileLayout && mobileMasterPage;
    }

    void setOpenLibrariesCallback (std::function<void()> callback)
    {
        onOpenLibraries = std::move (callback);
    }

    void activateMobileLayerPage (int layer)
    {
        if (! juce::isPositiveAndBelow (
                layer, TimbresProcessor::numLayers))
            return;

        const bool wasMasterPage = mobileMasterPage;
        const bool channelChanged = selectedStrip != layer;
        mobileMasterPage = false;
        selectStrip (layer);

        // selectStrip ja agenda o relayout quando o canal muda. Se foi apenas
        // MASTER -> mesmo canal, agenda aqui tambem. A chamada assincrona evita
        // mover o proprio botao durante o callback de clique/tecla.
        if (wasMasterPage && ! channelChanged)
        {
            juce::Component::SafePointer<TimbresEditor> safe (this);
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* editor = safe.getComponent())
                {
                    editor->resized();
                    editor->repaint();
                }
            });
        }
    }

    void activateMobileMasterPage()
    {
        mobileMasterPage = true;
        repaint();

        juce::Component::SafePointer<TimbresEditor> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* editor = safe.getComponent())
            {
                editor->resized();
                editor->repaint();
            }
        });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // No celular os cinco canais viram abas largas no topo e apenas o canal
        // em foco abre os controles. Isso elimina as seis reguas espremidas sem
        // esconder nenhum canal: um toque na aba troca a mesa imediatamente.
        if (focusedMobileLayout)
        {
            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                if (strips[l].tabBounds.contains (e.getPosition()))
                {
                    activateMobileLayerPage (l);
                    return;
                }

            if (mobileMasterTabBounds.contains (e.getPosition()))
            {
                activateMobileMasterPage();
                return;
            }
        }

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
            if (strips[l].cardBounds.contains (e.getPosition()))
            {
                if (e.mods.isPopupMenu())
                    showColourMenuFor (l);
                else
                    selectStrip (l);

                return;
            }
    }

    // Menu de cor do canal (botao direito no card). A cor por categoria distingue
    // sozinha ate voce ter DOIS do mesmo tipo -- dois pianos ficariam os dois
    // azuis. Aqui da para separar manualmente sem perder o padrao automatico.
    void showColourMenuFor (int l)
    {
        juce::PopupMenu m;
        const int current = proc.getLayerColourIndex (l);

        m.addSectionHeader ("Cor do canal " + juce::String (l + 1));
        m.addItem (1, juce::String::fromUTF8 ("Autom\xc3\xa1tico (categoria)"), true, current < 0);
        m.addSeparator();

        for (int i = 0; i < TimbresProcessor::numChannelColours; ++i)
            m.addColouredItem (i + 2, TimbresProcessor::channelColourName (i),
                               TimbresProcessor::channelColour (i), true, current == i);

        auto safe = juce::Component::SafePointer<TimbresEditor> (this);

        m.showMenuAsync (juce::PopupMenu::Options().withMinimumWidth (170),
                         [safe, l] (int result)
                         {
                             if (auto* ed = safe.getComponent(); ed != nullptr && result > 0)
                             {
                                 ed->proc.setLayerColourIndex (l, result - 2);   // 1 -> -1 (auto)
                                 ed->repaint();
                             }
                         });
    }

    void paint (juce::Graphics& g) override
    {
        CHURCHDAW_UI_SCOPE ("TimbresEditor::paint");
        // Fundo premium com vinheta (console no escuro do templo).
        NordLookAndFeel::fillAppBackground (g, getLocalBounds().toFloat());

        auto drawCard = [this, &g] (juce::Rectangle<int> r, bool selected, bool empty,
                                    bool muted = false, bool dimmed = false, bool master = false,
                                    juce::Colour accentColour = {}, bool soloed = false)
        {
            if (r.isEmpty())
                return;

            auto card = r.toFloat();
            const float radius = shortLandscapeMobileLayout ? 6.0f
                                                            : NordLookAndFeel::radiusCard;

            // No editor focado o card e a tela toda: moldura vermelha gritaria.
            const bool paintSelected = selected && ! focusedMobileLayout;
            // Master: acento fino (desktop). Categoria pinta a barra; selected
            // nao precisa de vermelho extra.
            const auto accent = empty   ? juce::Colour()
                              : soloed  ? NordLookAndFeel::amberSolo()
                              : master  ? NordLookAndFeel::nordRed()
                              : accentColour;

            NordLookAndFeel::drawGlassCard (g, card, radius, paintSelected,
                                            accent, empty, master);

            if (selected && focusedMobileLayout)
            {
                g.setColour (juce::Colours::white.withAlpha (0.14f));
                g.drawRoundedRectangle (card, radius, 1.4f);
            }

            // Canal calado por SOLO de outro: escurecimento LEVE.
            if (dimmed)
            {
                g.setColour (juce::Colours::black.withAlpha (0.22f));
                g.fillRoundedRectangle (card, radius);
            }

            // MUTE: card um pouco abafado + barra ambar.
            if (muted)
            {
                g.setColour (NordLookAndFeel::bgApp().withAlpha (0.28f));
                g.fillRoundedRectangle (card, radius);
                g.setColour (NordLookAndFeel::amberSolo().withAlpha (0.92f));
                g.fillRoundedRectangle (card.getX() + 3.0f, card.getY() + 12.0f,
                                        4.0f, card.getHeight() - 24.0f, 2.0f);
            }
        };

        // A UI usa a mesma invariante RT-safe do motor: SOLO em slot vazio nao
        // escurece nem cala os instrumentos carregados.
        const bool anySolo = proc.hasAnyLoadedSolo();

        if (focusedMobileLayout)
        {
            auto tabRail = strips[0].tabBounds;

            for (int l = 1; l < TimbresProcessor::numLayers; ++l)
                tabRail = tabRail.getUnion (strips[l].tabBounds);

            tabRail = tabRail.getUnion (mobileMasterTabBounds);

            if (! tabRail.isEmpty())
            {
                auto rail = tabRail.toFloat().expanded (2.0f, 1.0f);
                g.setColour (juce::Colours::black.withAlpha (0.34f));
                g.fillRoundedRectangle (rail.translated (0.0f, 1.0f), 8.0f);
                g.setColour (juce::Colour (0xff15171c));
                g.fillRoundedRectangle (rail, 8.0f);
                g.setColour (juce::Colours::white.withAlpha (0.07f));
                g.drawRoundedRectangle (rail.reduced (0.5f), 8.0f, 1.0f);
            }

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
            {
                const auto r = strips[l].tabBounds;

                if (r.isEmpty())
                    continue;

                const bool selected = ! mobileMasterPage && l == selectedStrip;
                const bool loaded = proc.getLayerTimbre (l).isNotEmpty()
                                     || proc.getLayerInstrument (l) != nullptr;
                const auto accent = loaded ? categoryColourFor (l)
                                           : NordLookAndFeel::txtDisabled();
                auto rf = r.toFloat();

                if (selected)
                {
                    // Fundo mais SOLIDO no canal ativo (pedido do dono): o
                    // gradiente sutil sumia sob luz de palco.
                    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3d434e),
                                                             rf.getX(), rf.getY(),
                                                             juce::Colour (0xff2b2f37),
                                                             rf.getX(), rf.getBottom(), false));
                    g.fillRoundedRectangle (rf, 6.0f);
                    g.setColour (juce::Colours::white.withAlpha (0.22f));
                    g.drawRoundedRectangle (rf.reduced (1.0f), 6.0f, 2.0f);

                    // indicador vertical a esquerda + trilho embaixo: a
                    // selecao le-se por forma, nao so por cor (item 6).
                    g.setColour (loaded ? accent : NordLookAndFeel::nordRedBright());
                    g.fillRoundedRectangle (rf.withTrimmedTop (7.0f)
                                              .withTrimmedBottom (7.0f)
                                              .removeFromLeft (3.0f)
                                              .translated (2.0f, 0.0f), 1.5f);
                    auto activeRail = rf.reduced (8.0f, 0.0f)
                                        .removeFromBottom (4.0f).withTrimmedBottom (1.0f);
                    g.fillRoundedRectangle (activeRail, 1.5f);
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.025f));
                    g.fillRoundedRectangle (rf, 5.0f);
                }

                auto text = r.reduced (7, 5);
                auto number = r.getWidth() < 92 ? text : text.removeFromLeft (28);
                g.setColour (selected ? juce::Colours::white
                                      : NordLookAndFeel::dimText());
                g.setFont (NordLookAndFeel::stageLabel (selected ? 11.5f : 10.0f));
                g.drawText ("C" + juce::String (l + 1), number,
                            juce::Justification::centred);

                if (mobilePageButtons[l].hasKeyboardFocus (false))
                {
                    g.setColour (juce::Colours::white.withAlpha (0.90f));
                    g.drawRoundedRectangle (rf.reduced (2.0f), 5.0f, 2.0f);
                }

                if (r.getWidth() < 92)
                    continue;

                juce::String label = proc.getLayerInstrumentName (l);

                if (label.isEmpty())
                    label = proc.getLayerTimbre (l);

                if (label.isEmpty())
                    label = "Adicionar";

                label = displayName (label);

                g.setFont (juce::Font (juce::FontOptions (selected ? 12.0f : 10.5f))
                               .boldened());
                g.setColour (selected ? juce::Colours::white.withAlpha (0.95f)
                                      : NordLookAndFeel::txtSecondary());
                g.drawFittedText (label, text, juce::Justification::centredLeft, 1, 0.78f);

                // Um ponto pequeno diz "ha som aqui" sem transformar cada aba
                // numa faixa colorida concorrendo com o canal ativo.
                g.setColour (loaded ? accent.withAlpha (selected ? 1.0f : 0.72f)
                                    : NordLookAndFeel::txtDisabled().withAlpha (0.35f));
                g.fillEllipse (rf.getRight() - 9.0f, rf.getCentreY() - 2.5f, 5.0f, 5.0f);
            }

            if (! mobileMasterTabBounds.isEmpty())
            {
                const auto r = mobileMasterTabBounds;
                const auto rf = r.toFloat();

                if (mobileMasterPage)
                {
                    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3c3136),
                                                             rf.getX(), rf.getY(),
                                                             juce::Colour (0xff292329),
                                                             rf.getX(), rf.getBottom(), false));
                    g.fillRoundedRectangle (rf, 6.0f);
                    g.setColour (NordLookAndFeel::nordRedBright());
                    g.fillRoundedRectangle (rf.reduced (8.0f, 0.0f)
                                              .removeFromBottom (4.0f)
                                              .withTrimmedBottom (1.0f), 1.5f);
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.025f));
                    g.fillRoundedRectangle (rf, 5.0f);
                }

                g.setColour (mobileMasterPage ? juce::Colours::white
                                              : NordLookAndFeel::txtSecondary());
                g.setFont (NordLookAndFeel::stageLabel (
                    mobileMasterTabBounds.getWidth() < 92 ? 9.0f : 10.0f));
                g.drawFittedText (mobileMasterTabBounds.getWidth() < 92 ? "MST" : "MASTER",
                                  r.reduced (5, 4), juce::Justification::centred, 1);

                if (mobileMasterPageButton.hasKeyboardFocus (false))
                {
                    g.setColour (juce::Colours::white.withAlpha (0.90f));
                    g.drawRoundedRectangle (rf.reduced (2.0f), 5.0f, 2.0f);
                }
            }
        }

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            const bool empty = proc.getLayerTimbre (l).isEmpty() && proc.getLayerInstrument (l) == nullptr;
            const bool muted = ! empty && proc.getLayerMute (l);
            const bool dimmed = ! empty && anySolo && ! proc.getLayerSolo (l) && ! muted;
            drawCard (strips[l].cardBounds, ! mobileMasterPage && l == selectedStrip,
                      empty, muted, dimmed, false,
                      empty ? juce::Colour() : categoryColourFor (l),
                      ! empty && proc.getLayerSolo (l));

            // CINTA DE AGRUPAMENTO em volta dos knobs, como as molduras "B3
            // PERCUSSION" / "VIB-CHORUS" da serigrafia da Nord: uma linha fina
            // interrompida no topo pelo rotulo. E o que separa "estes 4 controles
            // sao um bloco" de "estes 4 controles estao soltos no card".
            if (! empty && ! strips[l].fxGroup.isEmpty())
            {
                auto box = strips[l].fxGroup.toFloat().expanded (5.0f, 3.0f).withTrimmedBottom (2.0f);
                const juce::String tag (
                    strips[l].eqPage ? "EQ 3 BANDAS" : "ENV / FX");

                // Na interface touch a cinta ganha o titulo "SOM" DENTRO da
                // moldura (fora encostava no combo em tablets). Da nome a
                // secao dos knobs e separa performance de timbre (revisao do
                // dono, item 3).
                if (focusedMobileLayout)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.13f));
                    g.drawRoundedRectangle (box, 4.0f, 1.0f);
                    g.setFont (NordLookAndFeel::stageLabel (10.0f));
                    g.setColour (NordLookAndFeel::dimText().withAlpha (0.75f));
                    g.drawText (strips[l].eqPage ? "EQ" : "SOM",
                                box.toNearestInt().reduced (10, 4)
                                    .removeFromTop (14),
                                juce::Justification::centredLeft, false);
                    continue;
                }

                g.setFont (NordLookAndFeel::stageLabel (8.5f));
                const float tagW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), tag) + 10.0f;
                const float midX = box.getCentreX();

                juce::Path frame;
                frame.startNewSubPath (midX - tagW * 0.5f, box.getY());
                frame.lineTo (box.getX() + 4.0f, box.getY());
                frame.quadraticTo (box.getX(), box.getY(), box.getX(), box.getY() + 4.0f);
                frame.lineTo (box.getX(), box.getBottom() - 4.0f);
                frame.quadraticTo (box.getX(), box.getBottom(), box.getX() + 4.0f, box.getBottom());
                frame.lineTo (box.getRight() - 4.0f, box.getBottom());
                frame.quadraticTo (box.getRight(), box.getBottom(), box.getRight(), box.getBottom() - 4.0f);
                frame.lineTo (box.getRight(), box.getY() + 4.0f);
                frame.quadraticTo (box.getRight(), box.getY(), box.getRight() - 4.0f, box.getY());
                frame.lineTo (midX + tagW * 0.5f, box.getY());

                g.setColour (juce::Colours::white.withAlpha (0.13f));
                g.strokePath (frame, juce::PathStrokeType (1.0f));

                g.setColour (NordLookAndFeel::dimText().withAlpha (0.75f));
                g.drawText (tag, juce::Rectangle<float> (midX - tagW * 0.5f, box.getY() - 6.0f, tagW, 12.0f)
                                     .toNearestInt(),
                            juce::Justification::centred, false);
            }

            // Canal vazio: anel tracejado com "+" SEMPRE acima do botao
            // "Adicionar instrumento". Nunca desenhar o + em cima da letra —
            // a posicao do anel deriva dos bounds reais do botao.
            if (empty && ! strips[l].cardBounds.isEmpty())
            {
                const auto btn = strips[l].addButton.getBounds();
                const float rr = (float) kEmptyRing;
                float cx = 0.0f, cy = 0.0f;
                bool drawRing = false;

                if (strips[l].stageEmptyRingCentre != juce::Point<int>())
                {
                    cx = (float) strips[l].stageEmptyRingCentre.x;
                    cy = (float) strips[l].stageEmptyRingCentre.y;
                    drawRing = true;
                }
                else if (! btn.isEmpty())
                {
                    cx = (float) btn.getCentreX();
                    cy = (float) (btn.getY() - kEmptyGapY - kEmptyRing);
                    // So pinta se o anel couber dentro do card (abaixo do titulo).
                    const float minY = (float) strips[l].cardBounds.getY() + 28.0f + rr;
                    if (cy >= minY)
                        drawRing = true;
                }
                else
                {
                    const auto c = emptyRingCentre (strips[l].cardBounds);
                    cx = (float) c.x;
                    cy = (float) c.y;
                    drawRing = true;
                }

                if (drawRing)
                {
                    // Desktop Win/Mac: anel tracejado discreto + + fino.
                    // Sem glow vermelho (modo palco Android ficava extravagante).
                    juce::Path circ;
                    circ.addEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
                    juce::Path dashed;
                    const float dashes[] = { 4.5f, 3.5f };
                    juce::PathStrokeType (1.5f).createDashedStroke (dashed, circ, dashes, 2);
                    g.setColour (NordLookAndFeel::dimText().withAlpha (0.38f));
                    g.fillPath (dashed);

                    const float arm = juce::jmin (11.0f, rr * 0.42f);
                    g.setColour (NordLookAndFeel::nordRed().withAlpha (0.55f));
                    g.drawLine (cx - arm, cy, cx + arm, cy, 2.2f);
                    g.drawLine (cx, cy - arm, cx, cy + arm, 2.2f);
                }
            }
        }

        drawCard (masterCardBounds, false, false, false, false, true);
        drawCard (bottomPanelBounds, false, false);

        // Cada familia do master ganha uma chapa e um titulo curtos. No desktop
        // isso transforma a coluna estreita numa sequencia de modulos legiveis;
        // no touch conserva a mesma hierarquia em dimensoes maiores.
        if (! masterFxGroupBounds.isEmpty()
             || ! masterLevelGroupBounds.isEmpty()
             || ! masterLiveGroupBounds.isEmpty()
             || ! masterUtilityGroupBounds.isEmpty())
        {
            auto drawMasterGroup = [&g] (juce::Rectangle<int> bounds,
                                         const juce::String& tag,
                                         juce::Colour accent)
            {
                if (bounds.isEmpty())
                    return;

                // Chapa com o mesmo vocabulario dos cards grandes: raio maior,
                // filete de acento com 3 px e etiqueta legivel. A versao 7,5 px
                // com pill de 9 px lia como poeira ao lado de botoes de 48 dp --
                // era o grupo inteiro perdendo autoridade por causa do rotulo.
                auto box = bounds.toFloat().expanded (3.0f, 2.0f);
                g.setColour (juce::Colours::black.withAlpha (0.20f));
                g.fillRoundedRectangle (box, 8.0f);
                g.setColour (juce::Colours::white.withAlpha (0.11f));
                g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);
                g.setColour (accent.withAlpha (0.80f));
                g.fillRoundedRectangle (box.getX(), box.getY() + 7.0f,
                                        3.0f, juce::jmax (0.0f, box.getHeight() - 14.0f), 1.5f);

                auto font = NordLookAndFeel::stageLabel (9.0f);
                const float textW = juce::GlyphArrangement::getStringWidth (font, tag) + 12.0f;
                auto tagArea = juce::Rectangle<float> (box.getX() + 9.0f, box.getY() - 6.0f,
                                                       textW, 12.0f);
                g.setColour (juce::Colour (0xff262a31));
                g.fillRoundedRectangle (tagArea, 4.0f);
                g.setColour (accent.withAlpha (0.35f));
                g.drawRoundedRectangle (tagArea.reduced (0.5f), 4.0f, 1.0f);
                g.setFont (font);
                g.setColour (accent.withAlpha (0.95f));
                g.drawText (tag, tagArea.toNearestInt(), juce::Justification::centred, false);
            };

            // Nomes de console (redesenho do dono): "FX" e "LEVEL" secos nao
            // diziam que este e o painel de comando GLOBAL.
            drawMasterGroup (masterFxGroupBounds, "MASTER FX",
                             NordLookAndFeel::nordRedBright());
            drawMasterGroup (masterLevelGroupBounds, "OUTPUT / LEVEL",
                             juce::Colour (0xff55a9c5));
            drawMasterGroup (masterLiveGroupBounds, "LIVE CONTROL",
                             juce::Colour (0xffd9a23a));
            drawMasterGroup (masterUtilityGroupBounds, "UTILIDADES",
                             NordLookAndFeel::dimText());
        }

        // divisoria do master: fecha o bloco MASTER FX e abre o bloco de saida.
        // Sem ela os dois combos de efeito, o fader, o PANIC e os interruptores
        // liam como uma pilha unica de nove controles soltos.
        if (! fxDivider.isEmpty())
        {
            auto d = fxDivider.toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (d);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRect (d.translated (0.0f, 1.0f));
        }

        // bolinha de status (esquerda do texto "Pronto", no canto superior direito)
        {
            auto sb = status.getBounds();
            if (! sb.isEmpty())
            {
                const float tw = juce::GlyphArrangement::getStringWidth (status.getFont(), status.getText());
                const float dot = 9.0f;
                const float cx = (float) sb.getRight() - tw - 12.0f;
                const float cy = (float) sb.getCentreY();
                g.setColour (statusColour.withAlpha (0.28f));
                g.fillEllipse (cx - dot * 0.5f - 2.0f, cy - dot * 0.5f - 2.0f, dot + 4.0f, dot + 4.0f);
                g.setColour (statusColour);
                g.fillEllipse (cx - dot * 0.5f, cy - dot * 0.5f, dot, dot);
            }
        }

        // Marca de UNIDADE (0 dB) nas reguas, como em mesa fisica. Sem ela,
        // o fader do master em "0.0 dB" a ~75% do curso parecia conta errada
        // — na verdade ha +10 dB de headroom acima da unidade (spec do
        // Master: "ponto de 0.0 dB claramente marcado").
        {
            auto drawUnityMark = [&g] (juce::Slider& sl, double unityValue,
                                       int trackSpan)
            {
                if (! sl.isVisible() || sl.getBounds().isEmpty())
                    return;

                const auto pos = sl.getPositionOfValue (unityValue);
                const auto b = sl.getBounds();
                g.setColour (juce::Colours::white.withAlpha (0.40f));

                if (sl.isHorizontal())
                {
                    const float x = (float) b.getX() + pos;
                    g.fillRect (x - 0.75f, (float) b.getY() + 2.0f,
                                1.5f, (float) juce::jmin (10, trackSpan));
                    g.setFont (NordLookAndFeel::stageLabel (8.0f));
                    g.drawText ("0", juce::Rectangle<int> (
                                    (int) x - 7, b.getY() - 9, 14, 9),
                                juce::Justification::centred, false);
                }
                else
                {
                    const float y = (float) b.getY() + pos;
                    g.fillRect ((float) b.getX() - 4.0f, y - 0.75f,
                                8.0f, 1.5f);
                    g.fillRect ((float) b.getRight() - 4.0f, y - 0.75f,
                                8.0f, 1.5f);
                    g.setFont (NordLookAndFeel::stageLabel (8.0f));
                    g.drawText ("0", juce::Rectangle<int> (
                                    b.getX() - 16, (int) y - 5, 11, 10),
                                juce::Justification::centredRight, false);
                }
            };

            drawUnityMark (masterDbSlider, 0.0, 10);   // 0 dB do master

            if (focusedMobileLayout
                 && juce::isPositiveAndBelow (selectedStrip,
                                              TimbresProcessor::numLayers))
                drawUnityMark (strips[selectedStrip].vol, 1.0, 10);  // ganho 1,0 = 0 dB
        }
    }

    /** Roda do mouse sobre a faixa de canais anda de um em um. Fora dela
        (fader, knob, master) o evento segue para quem estava embaixo. */
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (TimbresProcessor::numLayers <= canaisNaTela
             || ! faixaDeCanais.contains (e.getPosition()))
        {
            juce::AudioProcessorEditor::mouseWheelMove (e, wheel);
            return;
        }

        // Mouse comum manda no eixo Y; trackpad manda no X. Aceitar os
        // dois evita a sensacao de "a roda nao faz nada aqui".
        const auto giro = std::abs (wheel.deltaX) > std::abs (wheel.deltaY)
                            ? wheel.deltaX : wheel.deltaY;

        if (std::abs (giro) < 0.08f)
            return;

        rolarCanais (giro > 0.0f ? -1 : 1);
    }

    void resized() override
    {
        CHURCHDAW_UI_SCOPE ("TimbresEditor::resized");
        if (churchOrganOverlay != nullptr)
        {
            churchOrganOverlay->setBounds (
                getLocalBounds());
            churchOrganOverlay->toFront (false);
        }

       #if JUCE_ANDROID || JUCE_IOS
        // A moldura touch usa margens menores, mas alvos maiores. O espaco
        // recuperado aqui vai para preset, fader e grupos do master.
        // 568x320 e 640x360 sao paisagens reais de aparelhos compactos.
        // O limite antigo (>=700, <=320, proporcao 2:1) deixava justamente
        // esses celulares no layout alto e espremia os controles.
        // 520 acompanha o MainComponent (560 na janela): paisagem de tablet
        // usa o card console; o teto antigo de 380 empurrava o tablet para
        // o layout empilhado e cortava os knobs abaixo da dobra.
        shortLandscapeMobileLayout = getWidth() >= 540
                                  && getHeight() <= 520
                                  && getWidth() >= getHeight() * 3 / 2;
        auto area = getLocalBounds().reduced (shortLandscapeMobileLayout ? 6 : 8,
                                              shortLandscapeMobileLayout ? 4 : 6);
        // +15% de legibilidade no aparelho real (pedido do dono, 30/07):
        // 14->16 e 10->11,5 na paisagem curta, com a linha acompanhando.
       #if JUCE_ANDROID
        // O seletor de modo e uma acao de palco, nao um link miudo. Reserva um
        // alvo touch de 44/48 dp e deixa o restante do editor refluindo abaixo.
        auto top = area.removeFromTop (shortLandscapeMobileLayout ? 44 : 48);
       #else
        auto top = area.removeFromTop (shortLandscapeMobileLayout ? 21 : 22);
       #endif
        title.setFont (juce::Font (juce::FontOptions (16.0f)).boldened());
        status.setFont (juce::Font (juce::FontOptions (
                             shortLandscapeMobileLayout ? 11.5f : 12.0f)));
       #if JUCE_ANDROID
        // Remove primeiro os alvos touch e entrega ao titulo apenas o que
        // realmente sobrou. Na largura de 320/360 dp, reservar o titulo antes
        // das setas fazia a segunda seta nascer com largura negativa.
        const int modeButtonW = juce::jlimit (98, 124,
                                               top.getWidth() / 3);
        editToneButton.setVisible (true);
        editToneButton.setToggleState (mobileStageMode,
                                       juce::dontSendNotification);
        editToneButton.setButtonText (mobileStageMode
                                        ? "Editar canal"
                                        : "Modo palco");
        editToneButton.setTooltip (mobileStageMode
            ? "Voltar ao canal em foco para editar todos os controles"
            : "Ver faders, meters, Mute, Solo e Master em tempo real");
        // Modo palco: botao de modo um pouco mais estreito — mais ar pro status.
        const int modeW = mobileStageMode
                            ? juce::jlimit (92, 112, modeButtonW - 8)
                            : juce::jmin (modeButtonW, top.getWidth());
        editToneButton.setBounds (top.removeFromRight (modeW));
        top.removeFromRight (juce::jmin (4, top.getWidth()));

        if (mobileStageMode)
        {
            // Setas de pagina: alvos 40 dp (ainda tocaveis, menos chrome).
            const int arrowW = juce::jmin (40, top.getHeight());
            canalProximoBtn.setVisible (true);
            canalProximoBtn.setBounds (top.removeFromRight (arrowW));
            top.removeFromRight (juce::jmin (2, top.getWidth()));
            canalAnteriorBtn.setVisible (true);
            canalAnteriorBtn.setBounds (top.removeFromRight (arrowW));
            top.removeFromRight (juce::jmin (3, top.getWidth()));
            // No palco o status "Pronto" fica compacto a direita do titulo.
            status.setFont (juce::Font (juce::FontOptions (11.0f)));
        }
        else
        {
            canalAnteriorBtn.setVisible (false);
            canalProximoBtn.setVisible (false);
            canalAnteriorBtn.setBounds ({});
            canalProximoBtn.setBounds ({});
        }

        const int titleW = juce::jmin (mobileStageMode ? 130 : 160, top.getWidth());
        title.setBounds (top.removeFromLeft (titleW));
       #else
        title.setBounds (top.removeFromLeft (
            shortLandscapeMobileLayout ? 150 : 160));
        editToneButton.setVisible (false);
        editToneButton.setBounds ({});
       #endif
        status.setBounds (top);
        area.removeFromTop (shortLandscapeMobileLayout ? 2 : 4);
       #else
        auto area = getLocalBounds().reduced (12);

        auto top = area.removeFromTop (26);
        title.setBounds (top.removeFromLeft (180));
        editToneButton.setVisible (true);
        editToneButton.setBounds (
            top.removeFromRight (desktopToneEditMode ? 136 : 112)
                .reduced (0, 1));
        top.removeFromRight (8);
        status.setBounds (top);
        area.removeFromTop (6);
       #endif

        // MASTER FX agora mora DENTRO da regua master (sem painel inferior) —
        // os canais ganham a altura toda
        bottomPanelBounds = {};
        masterFxGroupBounds = {};
        masterLevelGroupBounds = {};
        masterLiveGroupBounds = {};
        masterUtilityGroupBounds = {};

       #if JUCE_ANDROID || JUCE_IOS
        // O touch abre no editor focado (14 abas + Master). No Android, o
        // Modo Palco explicito usa duas, quatro ou seis reguas por pagina,
        // meters simultaneos e Master sempre ao lado.
       #if JUCE_ANDROID
        focusedMobileLayout = ! mobileStageMode;
       #else
        focusedMobileLayout = true;
       #endif
       #else
        // O mesmo editor focado do touch vira o painel de edicao detalhada do
        // desktop. Assim Windows, macOS e Android compartilham a hierarquia e
        // o Modo Palco continua mostrando as cinco reguas.
        focusedMobileLayout = desktopToneEditMode;
       #endif

        for (auto& button : mobilePageButtons)
        {
            button.setVisible (focusedMobileLayout);

            if (! focusedMobileLayout)
                button.setBounds ({});
        }

        mobileMasterPageButton.setVisible (focusedMobileLayout);

        if (! focusedMobileLayout)
            mobileMasterPageButton.setBounds ({});

       #if JUCE_ANDROID
        if (mobileStageMode)
        {
            layoutAndroidStageMode (area);
            return;
        }
       #endif

        if (focusedMobileLayout)
        {
            canalAnteriorBtn.setVisible (false);
            canalProximoBtn.setVisible (false);
            canalAnteriorBtn.setBounds ({});
            canalProximoBtn.setBounds ({});
            layoutFocusedMobile (area);
            return;
        }

        // O modo focado usa faders horizontais. Ao voltar ao Modo Palco,
        // restaura explicitamente a geometria tradicional; depender do estilo
        // deixado pelo layout anterior fazia apenas o canal editado voltar
        // horizontal entre quatro faders verticais.
        for (auto& s : strips)
        {
            s.vol.setSliderStyle (juce::Slider::LinearVertical);
            // DELAY e um controle do editor focado. Sua attachment continua
            // ativa no Modo Palco, mas ele nao ganha um quinto knob espremido.
            s.echoKnob.setBounds ({});
            s.echoLabel.setBounds ({});
            s.rotaryKnob.setBounds ({});
            s.rotaryLabel.setBounds ({});
            s.rotaryModeBox.setBounds ({});
        }

        masterDbSlider.setSliderStyle (juce::Slider::LinearVertical);

        for (auto& s : strips)
            s.tabBounds = {};

        // O master precisa de largura propria: ele concentra quatro familias de
        // controle e um meter estereo. Cerca de 16% entrega 10--15% a mais de
        // respiro que a versao anterior sem quebrar a grade dos oito canais.
        faixaDeCanais = area;
        const int desktopMasterW = juce::jlimit (158, 208,
                                                  juce::roundToInt (area.getWidth() * 0.16f));
        // Em 960 px, seis canais mantem os textos e alvos com largura util;
        // as setas continuam dando acesso imediato aos demais. Em 1280+, a
        // visao tradicional de oito canais permanece intacta.
        canaisNaTela = getWidth() < 1100 ? 6 : 8;
        const int stripW = juce::jmax (1, (area.getWidth() - desktopMasterW)
                                           / canaisNaTela);
        const bool phoneLayout = getHeight() < 440;

        limitarPrimeiroCanalVisivel();
        posicionarSetasDeCanal (area, stripW);

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];

            const int coluna = l - primeiroCanalVisivel;

            if (coluna < 0 || coluna >= canaisNaTela)
            {
                // Fora da janela de rolagem: esconder de verdade. Só dar
                // bounds fora da tela deixaria os sliders capturando
                // cliques e desenhando por cima do master.
                esconderStrip (s);
                continue;
            }

            auto col = area.removeFromLeft (stripW);
            s.cardBounds = col.reduced (5, 0);

            // fita de atividade EXATAMENTE onde o acento do card e desenhado
            s.activity.setBounds (s.cardBounds.getX() + 12, s.cardBounds.getY() + 6,
                                  juce::jmax (0, s.cardBounds.getWidth() - 24), 3);

            // canal carregado = timbre nativo OU plugin (Kontakt) hospedado
            const bool loaded = proc.getLayerTimbre (l).isNotEmpty()
                                 || proc.getLayerInstrument (l) != nullptr;
            const bool isPlugin = proc.getLayerInstrument (l) != nullptr;

            auto inner = s.cardBounds.reduced (10, 10);

            if (! loaded)
            {
                // canal vazio: mantem "CANAL X" no topo (orientacao) + convite central.
                // O botao encosta no circulo tracejado do paint (mesma geometria)
                // pra os dois lerem como UM elemento.
                inner.removeFromTop (6);
                s.name.setBounds (inner.removeFromTop (16));

                const auto c = emptyRingCentre (s.cardBounds);
                s.addButton.setBounds (s.cardBounds.getX() + 12,
                                       c.y + kEmptyRing + kEmptyGapY,
                                       s.cardBounds.getWidth() - 24, kEmptyBtnH);
                continue;
            }

            // TELEFONE EM PAISAGEM: a area util do editor fica perto de 250 px
            // logicos. O layout de desktop tentava preservar icone/categoria e
            // fader de 90 px; com isso M/S, quatro efeitos, Loop e Auto EQ iam
            // fisicamente para fora do card. Aqui o nome do proprio combo assume
            // o papel do titulo e todo controle operacional permanece tocavel.
            if (phoneLayout)
            {
                s.compLabel.setText ("CMP", juce::dontSendNotification);
                s.dlyLabel.setText  ("REV", juce::dontSendNotification);
                s.attackLabel.setText ("ATK", juce::dontSendNotification);
                s.releaseLabel.setText ("REL", juce::dontSendNotification);
                s.lowLabel.setText  ("LO",  juce::dontSendNotification);
                s.midLabel.setText  ("MID", juce::dontSendNotification);
                s.highLabel.setText ("HI",  juce::dontSendNotification);
                s.loop.setButtonText ("LOOP");
                s.autoEqBtn.setButtonText ("EQ");

                inner.removeFromTop (3);

                auto headRow = inner.removeFromTop (16);
                s.onBtn.setBounds (headRow.removeFromRight (38).withSizeKeepingCentre (36, 16));
                s.name.setBounds (headRow);
                inner.removeFromTop (3);

                s.glyph.setBounds ({});
                s.timbreName.setBounds ({});
                s.combo.setBounds (inner.removeFromTop (22));
                inner.removeFromTop (3);

                // Ancora as acoes importantes no rodape; o fader recebe a sobra.
                auto loopArea = inner.removeFromBottom (18);
                inner.removeFromBottom (2);
                auto knobRow = inner.removeFromBottom (38);
                inner.removeFromBottom (2);
                auto msRow = inner.removeFromBottom (20);
                inner.removeFromBottom (2);
                auto faderArea = inner;

                auto dbRow = faderArea.removeFromBottom (14);

                if (s.meter != nullptr)
                    s.meter->setBounds (faderArea.removeFromRight (NordLookAndFeel::meterChanW));

                faderArea.removeFromRight (3);
                s.vol.setBounds (faderArea);
                s.volDb.setBounds (dbRow.withTrimmedRight (22));

                const int msW = juce::jmin (42, msRow.getWidth() / 2 - 3);
                auto msBox = msRow.withSizeKeepingCentre (msW * 2 + 6, 20);
                s.muteBtn.setBounds (msBox.removeFromLeft (msW));
                msBox.removeFromLeft (6);
                s.soloBtn.setBounds (msBox);

                s.fxGroup = knobRow;

                auto placePhoneKnob = [] (juce::Rectangle<int> cell,
                                          juce::Slider& knob, juce::Label& label)
                {
                    label.setBounds (cell.removeFromBottom (9));
                    const int d = juce::jmin (28, juce::jmin (cell.getWidth(), cell.getHeight()));
                    knob.setBounds (cell.withSizeKeepingCentre (d, d));
                };

                if (s.eqPage)
                {
                    const int cw = knobRow.getWidth() / 3;
                    placePhoneKnob (knobRow.removeFromLeft (cw), s.lowKnob,  s.lowLabel);
                    placePhoneKnob (knobRow.removeFromLeft (cw), s.midKnob,  s.midLabel);
                    placePhoneKnob (knobRow,                     s.highKnob, s.highLabel);
                }
                else
                {
                    const int numControls = isPlugin ? 2 : 4;
                    const int cw = knobRow.getWidth() / numControls;
                    placePhoneKnob (knobRow.removeFromLeft (cw), s.compKnob, s.compLabel);
                    placePhoneKnob (isPlugin ? knobRow
                                             : knobRow.removeFromLeft (cw),
                                    s.dlyKnob, s.dlyLabel);

                    if (! isPlugin)
                    {
                        placePhoneKnob (knobRow.removeFromLeft (cw), s.attackKnob,  s.attackLabel);
                        placePhoneKnob (knobRow,                     s.releaseKnob, s.releaseLabel);
                    }
                }

                auto actions = loopArea.withSizeKeepingCentre (
                    juce::jmin (168, loopArea.getWidth()), 18);
                const int actionW = actions.getWidth() / 3;
                auto left = actions.removeFromLeft (actionW);
                auto centre = actions.removeFromLeft (actionW);
                s.loop.setBounds (left);
                s.pluginOpenBtn.setBounds (left);
                s.tonePageBtn.setBounds (centre);
                s.autoEqBtn.setBounds (actions);

                s.transp.setBounds ({});
                s.transpLabel.setBounds ({});
                continue;
            }

            s.compLabel.setText ("COMP", juce::dontSendNotification);
            s.dlyLabel.setText  ("REVERB", juce::dontSendNotification);
            s.attackLabel.setText ("ATTACK", juce::dontSendNotification);
            s.releaseLabel.setText ("RELEASE", juce::dontSendNotification);
            s.lowLabel.setText  ("LOW", juce::dontSendNotification);
            s.midLabel.setText  ("MID", juce::dontSendNotification);
            s.highLabel.setText ("HIGH", juce::dontSendNotification);
            s.loop.setButtonText ("Loop");
            s.autoEqBtn.setButtonText ("AUTO");

            inner.removeFromTop (4);

            // Grade fixa de cabecalho: CANAL ocupa sua propria linha; abaixo,
            // icone, categoria e ON usam colunas previsiveis em todos os cards.
            s.name.setBounds (inner.removeFromTop (18));
            inner.removeFromTop (4);

            auto identityRow = inner.removeFromTop (32);
            s.glyph.setBounds (identityRow.removeFromLeft (29)
                                  .withSizeKeepingCentre (26, 26));
            identityRow.removeFromLeft (3);
            s.onBtn.setBounds (identityRow.removeFromRight (33)
                                  .withSizeKeepingCentre (31, 18));
            identityRow.removeFromRight (3);
            s.timbreName.setBounds (identityRow);
            inner.removeFromTop (7);

            // O seletor e uma regua comum: mesma altura, raio, padding e linha
            // de base, independentemente do comprimento do preset.
            s.combo.setBounds (inner.removeFromTop (28));
            inner.removeFromTop (4);
            // Teclado MIDI deste canal (desktop: 2 teclados → piano / pad).
            s.midiInBox.setBounds (inner.removeFromTop (24));
            inner.removeFromTop (7);

            // TODOS OS CANAIS ABERTOS: cada regua mostra a cadeia completa (knobs,
            // transpose, loop, Auto EQ). Antes so o canal selecionado abria e os
            // outros ficavam enxutos -- economizava altura, mas dava uma mesa
            // desigual, com um canal "grande" e quatro pela metade.
            const int detailH = juce::jlimit (24, 170, inner.getHeight() - 96);

            // FADER COM TETO. Ele consumia toda a sobra do card, entao numa janela
            // alta virava um trilho enorme e vazio no miolo -- e a folga aparecia
            // como um BURACO entre o fim do fader e os botoes ancorados embaixo.
            // Com teto, a sobra vira margem inferior uniforme em todos os canais.
            const int maxFaderH = 210;
            const int available = inner.getHeight() - detailH - 10;
            const int faderH = juce::jmin (maxFaderH, juce::jmax (90, available));

            auto faderArea = inner.removeFromTop (faderH);
            inner.removeFromTop (10);
            auto bottom = inner.removeFromTop (detailH);

            auto dbRow = faderArea.removeFromBottom (20);
            const auto meterArea = faderArea.removeFromRight (NordLookAndFeel::meterChanW);
            s.meter->setBounds (meterArea);
            faderArea.removeFromRight (4);
            s.vol.setBounds (faderArea);
            // Centralizado sob o conjunto fader+meter, nao sob uma largura
            // variavel de trilho. Todos os valores ocupam a mesma caixa.
            s.volDb.setBounds (dbRow);

            auto takeRow = [&bottom] (int h)
            {
                return bottom.getHeight() >= h ? bottom.removeFromTop (h)
                                               : juce::Rectangle<int>();
            };

            auto msRow = takeRow (26);

            if (msRow.isEmpty())
            {
                s.muteBtn.setBounds ({});
                s.soloBtn.setBounds ({});
            }
            else
            {
                const int msW = juce::jmin (46, msRow.getWidth() / 2 - 4);
                auto msBox = msRow.withSizeKeepingCentre (msW * 2 + 8, 24);
                s.muteBtn.setBounds (msBox.removeFromLeft (msW));
                msBox.removeFromLeft (8);
                s.soloBtn.setBounds (msBox);
            }

            takeRow (4);

            // Dois por linha: em cards de ~120 px, quatro knobs lado a lado
            // obrigavam COMP/REVERB/ATTACK/RELEASE a parecer "CO.../RE...".
            // A grade 2x2 usa a altura que ja sobrava no detailH e mantem todas
            // as linhas de base identicas, inclusive em plugin/orgao.
            {
                auto knobGrid = takeRow (102);

                s.fxGroup = knobGrid;   // o paint desenha a cinta em volta desta area

                if (knobGrid.isEmpty())
                {
                    for (auto* k : { &s.compKnob, &s.dlyKnob, &s.attackKnob,
                                     &s.echoKnob, &s.rotaryKnob,
                                     &s.releaseKnob, &s.lowKnob, &s.midKnob,
                                     &s.highKnob })
                        k->setBounds ({});
                    for (auto* lb : { &s.compLabel, &s.dlyLabel, &s.attackLabel,
                                      &s.echoLabel, &s.rotaryLabel,
                                      &s.releaseLabel, &s.lowLabel, &s.midLabel,
                                      &s.highLabel })
                        lb->setBounds ({});
                    s.rotaryModeBox.setBounds ({});
                }
                else
                {
                    auto placeGridKnob = [] (juce::Rectangle<int> cell,
                                             juce::Slider& knob,
                                             juce::Label& label)
                    {
                        label.setBounds (cell.removeFromBottom (13));
                        cell.removeFromBottom (2);
                        const int d = juce::jmin (38,
                            juce::jmin (cell.getWidth() - 4, cell.getHeight()));
                        knob.setBounds (cell.withSizeKeepingCentre (
                            juce::jmax (1, d), juce::jmax (1, d)));
                    };

                    const int rowGap = 4;
                    auto topKnobs = knobGrid.removeFromTop (
                        (knobGrid.getHeight() - rowGap) / 2);
                    knobGrid.removeFromTop (rowGap);
                    auto bottomKnobs = knobGrid;
                    const int topCellW = topKnobs.getWidth() / 2;
                    const int bottomCellW = bottomKnobs.getWidth() / 2;

                    if (s.eqPage)
                    {
                        placeGridKnob (topKnobs.removeFromLeft (topCellW),
                                       s.lowKnob, s.lowLabel);
                        placeGridKnob (topKnobs, s.midKnob, s.midLabel);
                        placeGridKnob (bottomKnobs.withSizeKeepingCentre (
                                           bottomCellW, bottomKnobs.getHeight()),
                                       s.highKnob, s.highLabel);
                    }
                    else
                    {
                        placeGridKnob (topKnobs.removeFromLeft (topCellW),
                                       s.compKnob, s.compLabel);
                        placeGridKnob (topKnobs, s.dlyKnob, s.dlyLabel);

                        if (proc.isLayerOrgan (l))
                        {
                            placeGridKnob (bottomKnobs.removeFromLeft (bottomCellW),
                                           s.rotaryKnob, s.rotaryLabel);
                            s.rotaryModeBox.setBounds (bottomKnobs.reduced (3, 12));
                        }
                        else if (isPlugin)
                        {
                            placeGridKnob (bottomKnobs.removeFromLeft (bottomCellW),
                                           s.echoKnob, s.echoLabel);
                        }
                        else
                        {
                            placeGridKnob (bottomKnobs.removeFromLeft (bottomCellW),
                                           s.attackKnob, s.attackLabel);
                            placeGridKnob (bottomKnobs,
                                           s.releaseKnob, s.releaseLabel);
                        }
                    }
                }
            }

            // TRANSPOSE por canal saiu: existe o global no master, e ao vivo quase
            // nao se usa o por-canal. Os ~56px que ele ocupava viraram respiro
            // entre os blocos, agora que todos os canais abrem completos.
            s.transp.setBounds ({});
            s.transpLabel.setBounds ({});
            takeRow (5);
            auto loopArea = takeRow (22);

            if (loopArea.isEmpty())
            {
                s.loop.setBounds ({});
                s.autoEqBtn.setBounds ({});
                s.tonePageBtn.setBounds ({});
                s.pluginOpenBtn.setBounds ({});
            }
            else
            {
                auto actions = loopArea.withSizeKeepingCentre (
                    loopArea.getWidth(), 22);
                constexpr int actionGap = 4;
                const int actionW = (actions.getWidth() - actionGap * 2) / 3;
                auto left = actions.removeFromLeft (actionW);
                actions.removeFromLeft (actionGap);
                auto centre = actions.removeFromLeft (actionW);
                actions.removeFromLeft (actionGap);

                s.loop.setBounds (left);
                s.pluginOpenBtn.setBounds (left);   // mesmo lugar do Loop
                s.tonePageBtn.setBounds (centre);
                s.autoEqBtn.setBounds (actions);
            }
        }

        // regua do MASTER + controles globais
        auto& mcol = masterCardBounds;
        mcol = area.reduced (5, 0);

        auto inner = mcol.reduced (10, 10);

        if (phoneLayout)
        {
            // O TextBoxBelow de 74 px foi desenhado dentro de uma coluna de cerca
            // de 30 px no telefone: virava apenas "... ..." e espremia a pista do
            // fader. No palco o valor continua acessivel pelo gesto/tooltip; aqui
            // priorizamos uma pista de volume larga e um meter legivel.
            masterDbSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            sustainToggle.setButtonText ("SUS");
            invPedalToggle.setButtonText ("Inv.");
            masterAutoEqBtn.setButtonText ("EQ");
            masterPunchBtn.setButtonText ("PESO");
            gTranspLabel.setText ("TRANSPOSE", juce::dontSendNotification);

            inner.removeFromTop (3);
            masterName.setBounds (inner.removeFromTop (16));
            inner.removeFromTop (2);
            fxTitle.setBounds (inner.removeFromTop (10));
            inner.removeFromTop (2);

            for (int i = 0; i < TimbresProcessor::numInserts; ++i)
            {
                auto row = inner.removeFromTop (20);
                insertOpenButtons[i].setBounds (row.removeFromRight (40));
                row.removeFromRight (3);
                insertCombos[i].setBounds (row);

                if (i < TimbresProcessor::numInserts - 1)
                    inner.removeFromTop (2);
            }

            reverbTypeCombo.setVisible (false);
            reverbTypeCombo.setBounds ({});
            reverbReturnSlider.setVisible (false);
            reverbReturnSlider.setBounds ({});
            reverbReturnLabel.setVisible (false);
            reverbReturnLabel.setBounds ({});
            inner.removeFromTop (3);
            fxDivider = inner.removeFromTop (1);
            inner.removeFromTop (3);

            // Saida a esquerda; comandos do show empilhados a direita.
            auto commands = inner.removeFromRight (juce::jmin (64, inner.getWidth() * 3 / 5));
            inner.removeFromRight (4);

            if (masterMeter != nullptr)
            {
                masterMeter->setBounds (inner.removeFromRight (NordLookAndFeel::meterChanW));
                inner.removeFromRight (2);
            }

            masterDbSlider.setBounds (inner);

            panicButton.setBounds (commands.removeFromTop (18));
            commands.removeFromTop (1);

            auto transpArea = commands.removeFromTop (26);
            gTranspLabel.setBounds (transpArea.removeFromBottom (8));
            gTranspSlider.setBounds (transpArea);
            commands.removeFromTop (1);

            sustainToggle.setBounds (commands.removeFromTop (16));
            commands.removeFromTop (1);
            invPedalToggle.setBounds (commands.removeFromTop (16));
            commands.removeFromTop (1);

            auto finalRow = commands.removeFromTop (18);
            auto left = finalRow.removeFromLeft (finalRow.getWidth() / 2 - 2);
            finalRow.removeFromLeft (4);
            masterAutoEqBtn.setBounds (left);
            masterPunchBtn.setBounds (finalRow);
            return;
        }

        masterDbSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        masterDbSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 82, 22);
        sustainToggle.setButtonText ("Sustain");
        invPedalToggle.setButtonText ("Inverter pedal");
        refreshMasterUtilityLabels();
        gTranspSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gTranspSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
        gTranspLabel.setText ("TRANSPOSE", juce::dontSendNotification);

        inner.removeFromTop (4);
        masterName.setBounds (inner.removeFromTop (22));
        inner.removeFromTop (6);
        fxTitle.setBounds ({});   // o titulo agora pertence a chapa do modulo
        fxDivider = {};

        // Cinco zonas: header acima e quatro modulos com orcamento proprio.
        // A distribuicao proporcional absorve janelas maiores sem voltar a
        // esmagar o OUTPUT quando a altura cai para o desktop 720p.
        constexpr int masterGroupGap = 6;
        const int groupsHeight = juce::jmax (
            4, inner.getHeight() - masterGroupGap * 3);
        const bool compactMasterVertical = groupsHeight < 350;
        const bool veryCompactMasterVertical = groupsHeight < 300;
        const int fxHeight = groupsHeight * 32 / 100;
        const int outputHeight = groupsHeight * 26 / 100;
        const int liveHeight = groupsHeight
            * (compactMasterVertical ? 18 : 16) / 100;

        masterFxGroupBounds = inner.removeFromTop (fxHeight);
        inner.removeFromTop (masterGroupGap);
        masterLevelGroupBounds = inner.removeFromTop (outputHeight);
        inner.removeFromTop (masterGroupGap);
        masterLiveGroupBounds = inner.removeFromTop (liveHeight);
        inner.removeFromTop (masterGroupGap);
        masterUtilityGroupBounds = inner;

        // ---- MASTER FX -----------------------------------------------------
        const int groupVPad = veryCompactMasterVertical ? 3
                            : compactMasterVertical ? 4 : 6;
        const int groupLabelSpace = veryCompactMasterVertical ? 7
                                  : compactMasterVertical ? 8 : 10;
        auto fxContent = masterFxGroupBounds.reduced (8, groupVPad);
        fxContent.removeFromTop (groupLabelSpace);
        const int fxRowH = veryCompactMasterVertical ? 17
                         : compactMasterVertical ? 19 : 22;
        const int fxGap = veryCompactMasterVertical ? 1
                        : compactMasterVertical ? 2 : 4;

        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            auto row = fxContent.removeFromTop (fxRowH);

            if (insertOpenButtons[i].isVisible())
            {
                insertOpenButtons[i].setBounds (row.removeFromRight (56));
                row.removeFromRight (5);
            }
            else
            {
                insertOpenButtons[i].setBounds ({});
            }

            insertCombos[i].setBounds (row);

            if (i < TimbresProcessor::numInserts - 1)
                fxContent.removeFromTop (fxGap);
        }

        fxContent.removeFromTop (veryCompactMasterVertical ? 2
                                 : compactMasterVertical ? 3 : 4);
        reverbTypeCombo.setVisible (true);
        reverbTypeCombo.setBounds (fxContent.removeFromTop (fxRowH));
        fxContent.removeFromTop (fxGap);
        auto returnRow = fxContent.removeFromTop (
            juce::jmin (veryCompactMasterVertical ? 15
                        : compactMasterVertical ? 17 : 20,
                        fxContent.getHeight()));
        reverbReturnLabel.setVisible (true);
        reverbReturnLabel.setBounds (returnRow.removeFromLeft (48));
        reverbReturnSlider.setVisible (true);
        reverbReturnSlider.setBounds (returnRow);

        // ---- OUTPUT / LEVEL ----------------------------------------------
        auto levelContent = masterLevelGroupBounds.reduced (8, groupVPad);
        levelContent.removeFromTop (groupLabelSpace);

        if (masterMeter != nullptr)
        {
            masterMeter->setBounds (levelContent.removeFromRight (
                juce::jmin (58, juce::jmax (48, levelContent.getWidth() / 3))));
            levelContent.removeFromRight (8);
        }

        masterDbSlider.setBounds (levelContent);

        // ---- LIVE CONTROL -------------------------------------------------
        auto liveContent = masterLiveGroupBounds.reduced (8, groupVPad);
        liveContent.removeFromTop (groupLabelSpace);
        panicButton.setBounds (liveContent.removeFromTop (
            juce::jmin (veryCompactMasterVertical ? 18
                        : compactMasterVertical ? 20 : 26,
                        liveContent.getHeight())));
        liveContent.removeFromTop (compactMasterVertical ? 2 : 4);

        auto transposeRow = liveContent;
        gTranspLabel.setBounds (transposeRow.removeFromLeft (
            juce::jmin (62, transposeRow.getWidth() / 2)));
        transposeRow.removeFromLeft (4);
        gTranspSlider.setBounds (transposeRow);

        // ---- UTILIDADES ---------------------------------------------------
        auto utilityContent = masterUtilityGroupBounds.reduced (8, groupVPad);
        utilityContent.removeFromTop (groupLabelSpace);
        const int switchW = utilityContent.getWidth();
        const int switchH = veryCompactMasterVertical ? 15
                          : compactMasterVertical ? 18 : 20;
        const int firstSwitchGap = veryCompactMasterVertical ? 2
                                 : compactMasterVertical ? 3 : 4;
        const int secondSwitchGap = veryCompactMasterVertical ? 2
                                  : compactMasterVertical ? 4 : 5;

        auto sustainArea = utilityContent.removeFromTop (
            juce::jmin (switchH, utilityContent.getHeight()));
        sustainToggle.setBounds (sustainArea.withSizeKeepingCentre (
            switchW, sustainArea.getHeight()));
        utilityContent.removeFromTop (firstSwitchGap);

        auto invertArea = utilityContent.removeFromTop (
            juce::jmin (switchH, utilityContent.getHeight()));
        invPedalToggle.setBounds (invertArea.withSizeKeepingCentre (
            switchW, invertArea.getHeight()));
        utilityContent.removeFromTop (secondSwitchGap);

        auto finalRow = utilityContent.removeFromTop (
            juce::jmin (26, utilityContent.getHeight()));
        const int finalGap = 6;
        const int finalW = juce::jmax (1, (finalRow.getWidth() - finalGap) / 2);
        masterAutoEqBtn.setBounds (finalRow.removeFromLeft (finalW));
        finalRow.removeFromLeft (finalGap);
        masterPunchBtn.setBounds (finalRow);
    }

private:
   #if JUCE_ANDROID
    void layoutAndroidStageMode (juce::Rectangle<int> area)
    {
        focusedMobileLayout = false;
        mobileMasterPage = false;
        mobileMasterTabBounds = {};
        bottomPanelBounds = {};
        masterFxGroupBounds = {};
        masterLevelGroupBounds = {};
        masterLiveGroupBounds = {};
        masterUtilityGroupBounds = {};
        fxDivider = {};

        // O palco mostra apenas os controles de performance. Os componentes e
        // attachments continuam vivos: zerar a geometria nao altera um unico
        // parametro nem interrompe os meters.
        for (auto& s : strips)
            esconderStrip (s);

        fxTitle.setBounds ({});
        gTranspSlider.setBounds ({});
        gTranspLabel.setBounds ({});
        sustainToggle.setBounds ({});
        invPedalToggle.setBounds ({});
        reverbTypeCombo.setBounds ({});
        reverbTypeCombo.setVisible (false);
        reverbReturnSlider.setBounds ({});
        reverbReturnSlider.setVisible (false);
        reverbReturnLabel.setBounds ({});
        reverbReturnLabel.setVisible (false);

        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            insertCombos[i].setBounds ({});
            insertOpenButtons[i].setBounds ({});
        }

        canaisNaTela = juce::jmin (
            TimbresProcessor::numLayers,
            ChurchDAWAndroidStageLayout::stageChannelCountForWidth (
                area.getWidth()));

        if (androidStageLastChannelsOnScreen != canaisNaTela)
        {
            androidStageLastChannelsOnScreen = canaisNaTela;
            androidStageRevealSelected = true;
        }

        if (androidStageRevealSelected)
        {
            int focus = selectedStrip;

            if (! juce::isPositiveAndBelow (focus,
                                             TimbresProcessor::numLayers))
                focus = proc.getSelectedLayer();

            if (juce::isPositiveAndBelow (focus,
                                          TimbresProcessor::numLayers))
                primeiroCanalVisivel =
                    ChurchDAWAndroidStageLayout::pageContaining (
                        focus, canaisNaTela,
                        TimbresProcessor::numLayers);

            androidStageRevealSelected = false;
        }

        limitarPrimeiroCanalVisivel();

        constexpr int stageGap = 4;
        const auto stageColumns =
            ChurchDAWAndroidStageLayout::calculateStageColumns (
                area.getWidth(), canaisNaTela);
        const int masterW = stageColumns.masterWidth;

        auto channelArea = area;
        auto masterArea = channelArea.removeFromRight (
            juce::jmin (masterW, channelArea.getWidth()));
        channelArea.removeFromRight (
            juce::jmin (stageGap, channelArea.getWidth()));
        faixaDeCanais = channelArea;

        const int maxStart = juce::jmax (
            0, TimbresProcessor::numLayers - canaisNaTela);
        const bool canPage = TimbresProcessor::numLayers > canaisNaTela;
        canalAnteriorBtn.setVisible (canPage);
        canalProximoBtn.setVisible (canPage);
        canalAnteriorBtn.setEnabled (primeiroCanalVisivel > 0);
        canalProximoBtn.setEnabled (primeiroCanalVisivel < maxStart);
        canalAnteriorBtn.setTooltip ("Pagina anterior de canais");
        canalProximoBtn.setTooltip ("Proxima pagina de canais");
        canalAnteriorBtn.toFront (false);
        canalProximoBtn.toFront (false);

        auto remainingChannels = channelArea;
        int remainingColumns = canaisNaTela;

        for (int column = 0; column < canaisNaTela; ++column)
        {
            const int layer = primeiroCanalVisivel + column;
            auto cell = column == canaisNaTela - 1
                          ? remainingChannels
                          : remainingChannels.removeFromLeft (
                                remainingChannels.getWidth()
                                    / remainingColumns);
            --remainingColumns;

            if (! juce::isPositiveAndBelow (layer,
                                             TimbresProcessor::numLayers))
                continue;

            auto& s = strips[layer];
            s.cardBounds = cell.reduced (1, 0);
            s.activity.setBounds (s.cardBounds.getX() + 4,
                                  s.cardBounds.getY() + 3,
                                  juce::jmax (0,
                                      s.cardBounds.getWidth() - 8), 2);

            auto inner = s.cardBounds.reduced (4);
            const bool loaded =
                proc.getLayerTimbre (layer).isNotEmpty()
                || proc.getLayerInstrument (layer) != nullptr
                || proc.isLayerChurchOrgan (layer);

            auto header = inner.removeFromTop (
                juce::jmin (44, inner.getHeight()));
            auto power = header.removeFromRight (
                juce::jmin (44, header.getWidth()));
            header.removeFromRight (juce::jmin (3, header.getWidth()));
            s.name.setBounds (header);
            s.onBtn.setBounds (loaded ? power
                                      : juce::Rectangle<int>());
            inner.removeFromTop (juce::jmin (3, inner.getHeight()));

            if (! loaded)
            {
                // Canal vazio no Modo Palco: grupo anel (paint) + botao ABAIXO.
                s.name.setBounds (header);
                s.onBtn.setBounds ({});
                s.combo.setBounds ({});
                s.combo.setInterceptsMouseClicks (true, false);
                s.combo.setWantsKeyboardFocus (false);

                const int ringR = kEmptyRing;
                const int btnH = juce::jlimit (36, 44,
                    juce::jmin (kEmptyBtnH, juce::jmax (32, inner.getHeight() / 5)));
                const int gap = kEmptyGapY;
                const int groupH = ringR * 2 + gap + btnH;
                const int top = inner.getY()
                              + juce::jmax (0, (inner.getHeight() - groupH) / 2);

                s.stageEmptyRingCentre = { inner.getCentreX(), top + ringR };

                s.addButton.setBounds (
                    inner.getX() + 6,
                    top + ringR * 2 + gap,
                    juce::jmax (1, inner.getWidth() - 12),
                    btnH);
                s.addButton.setVisible (true);
                s.addButton.toFront (false);
                continue;
            }

            // Preset = heroi do card: chip mais alto, fonte sobe com a altura.
            s.combo.setInterceptsMouseClicks (true, false);
            s.combo.setWantsKeyboardFocus (false);
            s.combo.setBounds (inner.removeFromTop (
                juce::jmin (36, inner.getHeight())));
            inner.removeFromTop (juce::jmin (3, inner.getHeight()));
            // Seletor de teclado compacto no modo palco.
            if (inner.getHeight() > 160)
            {
                s.midiInBox.setBounds (inner.removeFromTop (
                    juce::jmin (28, inner.getHeight())));
                inner.removeFromTop (juce::jmin (3, inner.getHeight()));
            }
            else
            {
                s.midiInBox.setBounds ({});
            }

            auto actions = inner.removeFromBottom (
                juce::jmin (46, inner.getHeight()));
            constexpr int actionGap = 4;
            const int actionW = juce::jmax (
                1, (actions.getWidth() - actionGap) / 2);
            s.muteBtn.setBounds (actions.removeFromLeft (actionW));
            actions.removeFromLeft (
                juce::jmin (actionGap, actions.getWidth()));
            s.soloBtn.setBounds (actions);
            inner.removeFromBottom (juce::jmin (4, inner.getHeight()));

            if (inner.getHeight() >= 112)
            {
                auto db = inner.removeFromBottom (18);
                s.volDb.setBounds (db);
                inner.removeFromBottom (juce::jmin (2, inner.getHeight()));
            }

            s.vol.setSliderStyle (juce::Slider::LinearVertical);
            s.vol.setTextBoxStyle (juce::Slider::NoTextBox,
                                   false, 0, 0);

            if (s.meter != nullptr)
            {
                // Meter um pouco mais largo — legivel no palco escuro.
                const int meterW = juce::jlimit (
                    22, 32, inner.getWidth() / 3);
                s.meter->setBounds (inner.removeFromRight (meterW)
                                         .reduced (0, 2));
                inner.removeFromRight (juce::jmin (4, inner.getWidth()));
            }

            s.vol.setBounds (inner.withSizeKeepingCentre (
                juce::jmin (56, inner.getWidth()), inner.getHeight()));
        }

        // MASTER fixo: saida e PANIC nunca saem da tela enquanto as setas
        // paginam apenas os canais. Em telas largas, EQ/PESO ocupam uma segunda
        // fileira; em telefones estreitos cedem espaco ao fader e ao PANIC.
        masterCardBounds = masterArea;
        auto masterInner = masterCardBounds.reduced (4);
        masterName.setBounds (masterInner.removeFromTop (
            juce::jmin (28, masterInner.getHeight())));
        masterInner.removeFromTop (
            juce::jmin (4, masterInner.getHeight()));

        // PANIC contido (36-40 dp): master fader ganha o espaco.
        panicButton.setBounds (masterInner.removeFromBottom (
            juce::jmin (40, masterInner.getHeight())));
        masterInner.removeFromBottom (
            juce::jmin (6, masterInner.getHeight()));

        // O contrato do Modo Palco e simples e seguro: canais, faders,
        // meters, M/S, preset e PANIC. EQ/PESO/inserts continuam no editor,
        // evitando mudancas de timbre por toque acidental no show.
        masterAutoEqBtn.setBounds ({});
        masterPunchBtn.setBounds ({});

        masterDbSlider.setSliderStyle (juce::Slider::LinearVertical);
        masterDbSlider.setTextBoxStyle (juce::Slider::NoTextBox,
                                        false, 0, 0);

        if (masterMeter != nullptr)
        {
            const int meterW = juce::jlimit (
                24, 34, masterInner.getWidth() / 3);
            masterMeter->setBounds (masterInner.removeFromRight (meterW)
                                         .reduced (0, 2));
            masterInner.removeFromRight (
                juce::jmin (4, masterInner.getWidth()));
        }

        masterDbSlider.setBounds (masterInner.withSizeKeepingCentre (
            juce::jmin (58, masterInner.getWidth()),
            masterInner.getHeight()));
    }
   #endif

    void layoutFocusedMobile (juce::Rectangle<int> area)
    {
       #if JUCE_ANDROID
        // O Modo Palco transforma o combo em leitura. Reentrar no editor
        // detalhado restaura imediatamente toque, teclado e D-pad.
        faixaDeCanais = {};

        for (auto& s : strips)
        {
            s.combo.setInterceptsMouseClicks (true, true);
            s.combo.setWantsKeyboardFocus (true);
        }
       #endif

        // Garante que sempre exista um canal em foco. Na primeira abertura
        // prefere o primeiro carregado; se a mesa esta vazia, o canal 1 vira o
        // convite inicial.
        int focus = selectedStrip;

        if (! juce::isPositiveAndBelow (focus, TimbresProcessor::numLayers))
        {
            focus = 0;

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                if (proc.getLayerTimbre (l).isNotEmpty()
                     || proc.getLayerInstrument (l) != nullptr)
                {
                    focus = l;
                    break;
                }

            selectedStrip = focus;
            proc.setSelectedLayer (focus);
        }

        // A selecao inicial nao passa por selectStrip(). Sem atualizar estas
        // cores aqui, o primeiro canal podia estar aberto e ainda parecer
        // inativo ate o usuario tocar numa aba.
        // Desktop: canal em foco = branco, nao vermelho gritando.
        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
            strips[l].name.setColour (juce::Label::textColourId,
                                      l == focus ? juce::Colours::white
                                                 : NordLookAndFeel::dimText());

        // Remove toda geometria da regua que estava aberta antes. Os componentes
        // continuam vivos/atualizando attachments, so nao ficam empilhados por
        // baixo do canal novo.
        for (auto& s : strips)
        {
            s.cardBounds = {};
            s.tabBounds = {};
            s.fxGroup = {};

            juce::Component* controls[] = {
                &s.name, &s.timbreName, &s.transpLabel, &s.volDb,
                &s.glyph, &s.activity, &s.combo, &s.midiInBox, &s.vol, &s.transp,
                &s.loop, &s.muteBtn, &s.soloBtn, &s.onBtn,
                &s.autoEqBtn, &s.tonePageBtn, &s.pluginOpenBtn, &s.addButton,
                &s.rotaryModeBox,
                &s.compKnob, &s.dlyKnob, &s.echoKnob, &s.rotaryKnob,
                &s.attackKnob, &s.releaseKnob,
                &s.lowKnob, &s.midKnob, &s.highKnob,
                &s.compLabel, &s.dlyLabel, &s.echoLabel, &s.rotaryLabel,
                &s.attackLabel, &s.releaseLabel,
                &s.lowLabel, &s.midLabel, &s.highLabel
            };

            for (auto* c : controls)
                c->setBounds ({});

            if (s.meter != nullptr)
                s.meter->setBounds ({});
        }

        // Canais + MASTER sao paginas. No Android, a faixa tambem e paginada:
        // quinze celulas simultaneas davam apenas ~21 dp em portrait. As setas,
        // o Master e cada aba ficam agora com alvo minimo de 44 dp.
        const int tabsH = shortLandscapeMobileLayout
                            ? juce::jmin (48, area.getHeight())
                            : juce::jlimit (48, 56, area.getHeight() / 7);
        auto tabs = area.removeFromTop (tabsH);
        const int gap = shortLandscapeMobileLayout ? 2 : 3;

       #if JUCE_ANDROID
        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            strips[l].tabBounds = {};
            mobilePageButtons[l].setBounds ({});
        }

        constexpr int arrowW = 44;
        constexpr int masterTabW = 48;
        canalAnteriorBtn.setVisible (true);
        canalAnteriorBtn.setBounds (
            tabs.removeFromLeft (juce::jmin (arrowW, tabs.getWidth())));
        tabs.removeFromLeft (juce::jmin (gap, tabs.getWidth()));

        mobileMasterTabBounds = tabs.removeFromRight (
            juce::jmin (masterTabW, tabs.getWidth()));
        mobileMasterPageButton.setBounds (mobileMasterTabBounds);
        tabs.removeFromRight (juce::jmin (gap, tabs.getWidth()));

        canalProximoBtn.setVisible (true);
        canalProximoBtn.setBounds (
            tabs.removeFromRight (juce::jmin (arrowW, tabs.getWidth())));
        tabs.removeFromRight (juce::jmin (gap, tabs.getWidth()));

        canaisNaTela =
            ChurchDAWAndroidStageLayout::focusedChannelCountForWidth (
                tabs.getWidth(), gap);
        canaisNaTela = juce::jmin (canaisNaTela,
                                   TimbresProcessor::numLayers);

        if (focus < primeiroCanalVisivel
             || focus >= primeiroCanalVisivel + canaisNaTela)
            primeiroCanalVisivel =
                ChurchDAWAndroidStageLayout::pageContaining (
                    focus, canaisNaTela,
                    TimbresProcessor::numLayers);

        limitarPrimeiroCanalVisivel();
        const int maxStart = juce::jmax (
            0, TimbresProcessor::numLayers - canaisNaTela);
        canalAnteriorBtn.setEnabled (primeiroCanalVisivel > 0);
        canalProximoBtn.setEnabled (primeiroCanalVisivel < maxStart);
        canalAnteriorBtn.setTooltip ("Canais anteriores");
        canalProximoBtn.setTooltip ("Proximos canais");

        const int tabW = juce::jmax (
            1, (tabs.getWidth() - gap * (canaisNaTela - 1))
                   / canaisNaTela);

        for (int column = 0; column < canaisNaTela; ++column)
        {
            const int l = primeiroCanalVisivel + column;

            if (! juce::isPositiveAndBelow (
                    l, TimbresProcessor::numLayers))
                break;

            strips[l].tabBounds = column == canaisNaTela - 1
                                    ? tabs
                                    : tabs.removeFromLeft (tabW);
            mobilePageButtons[l].setBounds (strips[l].tabBounds);

            if (column < canaisNaTela - 1)
                tabs.removeFromLeft (juce::jmin (gap, tabs.getWidth()));
        }
       #else
        constexpr int numMobilePages = TimbresProcessor::numLayers + 1;
        const int tabW = juce::jmax (1, (tabs.getWidth() - gap * (numMobilePages - 1))
                                        / numMobilePages);

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            strips[l].tabBounds = tabs.removeFromLeft (tabW);
            mobilePageButtons[l].setBounds (strips[l].tabBounds);
            tabs.removeFromLeft (gap);
        }

        mobileMasterTabBounds = tabs;
        mobileMasterPageButton.setBounds (mobileMasterTabBounds);
       #endif
        area.removeFromTop (shortLandscapeMobileLayout ? 4 : 6);

        auto clearMasterGeometry = [this]
        {
            masterCardBounds = {};
            masterName.setBounds ({});
            fxTitle.setBounds ({});
            masterDbSlider.setBounds ({});
            gTranspSlider.setBounds ({});
            gTranspLabel.setBounds ({});
            panicButton.setBounds ({});
            sustainToggle.setBounds ({});
            invPedalToggle.setBounds ({});
            masterAutoEqBtn.setBounds ({});
            masterPunchBtn.setBounds ({});
            reverbTypeCombo.setBounds ({});
            reverbTypeCombo.setVisible (false);
            reverbReturnSlider.setBounds ({});
            reverbReturnSlider.setVisible (false);
            reverbReturnLabel.setBounds ({});
            reverbReturnLabel.setVisible (false);
            masterFxGroupBounds = {};
            masterLevelGroupBounds = {};
            masterLiveGroupBounds = {};
            masterUtilityGroupBounds = {};
            fxDivider = {};

            for (int i = 0; i < TimbresProcessor::numInserts; ++i)
            {
                insertCombos[i].setBounds ({});
                insertOpenButtons[i].setBounds ({});
            }

            if (masterMeter != nullptr)
                masterMeter->setBounds ({});
        };

        if (! mobileMasterPage)
        {
            clearMasterGeometry();

            auto& s = strips[focus];
            s.cardBounds = area;
            const bool loaded = proc.getLayerTimbre (focus).isNotEmpty()
                                 || proc.getLayerInstrument (focus) != nullptr;
            const bool isPlugin = proc.getLayerInstrument (focus) != nullptr;
            const bool isOrgan = proc.isLayerOrgan (focus);

            if (shortLandscapeMobileLayout)
            {
                s.activity.setBounds (s.cardBounds.getX() + 10,
                                      s.cardBounds.getY() + 4,
                                      juce::jmax (0, s.cardBounds.getWidth() - 20), 2);

                auto inner = s.cardBounds.reduced (8, 5);
                inner.removeFromTop (2);

                if (! loaded)
                {
                    // Grupo anel+botao: botao embaixo pra o paint nao desenhar
                    // o + em cima da letra.
                    const int buttonW = juce::jmin (300, inner.getWidth() - 20);
                    const int btnH = 48;
                    const int groupH = kEmptyRing * 2 + kEmptyGapY + btnH;
                    const int top = inner.getY()
                                  + juce::jmax (0, (inner.getHeight() - groupH) / 2);
                    s.addButton.setBounds (
                        inner.getCentreX() - buttonW / 2,
                        top + kEmptyRing * 2 + kEmptyGapY,
                        buttonW, btnH);
                    return;
                }

                // Nome completo quando ha palco largo (TANK 3: 808 dp de
                // card); abreviacao so no aparelho realmente estreito
                // (640x360 da ~628 e mantem REV/DLY/ATK).
                const bool wideStage = inner.getWidth() >= 700;
                s.compLabel.setText ("COMP", juce::dontSendNotification);
                s.dlyLabel.setText  (wideStage ? "REVERB" : "REV", juce::dontSendNotification);
                s.echoLabel.setText (wideStage ? "DELAY" : "DLY", juce::dontSendNotification);
                s.rotaryLabel.setText (wideStage ? "ROTARY" : "ROT", juce::dontSendNotification);
                s.attackLabel.setText (wideStage ? "ATTACK" : "ATK", juce::dontSendNotification);
                s.releaseLabel.setText (wideStage ? "RELEASE" : "REL", juce::dontSendNotification);
                s.lowLabel.setText  ("LOW", juce::dontSendNotification);
                s.midLabel.setText  ("MID", juce::dontSendNotification);
                s.highLabel.setText  ("HI", juce::dontSendNotification);
                s.loop.setButtonText ("LOOP");
                s.autoEqBtn.setButtonText (wideStage ? "AUTO EQ" : "A.EQ");

                // Paisagem curta NAO e retrato achatado. A janela 640x360 deixa
                // cerca de 120 dp para o card depois das barras do app. Empilhar
                // cabecalho, preset, acoes, volume e knobs escondia todo o ENV.
                // Aqui o card vira tres regioes horizontais:
                // identidade | performance | controles 3x2.
                constexpr int columnGap = 6;
                const int identityW = juce::jlimit (
                    190, 224, inner.getWidth() * 34 / 100);
                auto identity = inner.removeFromLeft (identityW);
                inner.removeFromLeft (columnGap);
                // Quatro acoes (M, S, Loop e Rotary/Auto EQ) precisam de
                // 4 x 48 dp + 3 x 4 dp de respiro = 204 dp. Com o piso antigo
                // de 196, o seletor Rotary terminava em 46 dp no 640x360.
                const int performanceW = juce::jlimit (
                    204, 224, inner.getWidth() * 34 / 100);
                auto performance = inner.removeFromLeft (
                    juce::jmin (performanceW,
                                juce::jmax (1, inner.getWidth() - 156)));
                inner.removeFromLeft (juce::jmin (columnGap, inner.getWidth()));
                auto controlsArea = inner;

                // As duas fileiras cresciam ate 100 dp e o resto do card
                // virava faixa vazia acima e abaixo (medido no TANK 3:
                // ~33 dp mortos de cada lado). Com folga, os controles
                // engordam ate 128 dp — botao de 60 dp e alvo de palco.
                constexpr int rowGap = 6;
                const int twoRowHeight = juce::jlimit (
                    100, 128, identity.getHeight() - 8);
                const int rowHeight = (twoRowHeight - rowGap) / 2;
                // Ancorado no TOPO como a regua do Windows (o CANAL/ON abre
                // o card); centrado ele flutuava com vazio acima e abaixo.
                auto identityBlock = identity.withTrimmedTop (
                    juce::jmin (10, identity.getHeight() / 8));
                identityBlock = identityBlock.removeFromTop (
                    juce::jmin (twoRowHeight, identityBlock.getHeight()));
                auto identityHead = identityBlock.removeFromTop (
                    juce::jmin (rowHeight, identityBlock.getHeight()));
                identityBlock.removeFromTop (
                    juce::jmin (rowGap, identityBlock.getHeight()));
                auto presetRow = identityBlock.removeFromTop (
                    juce::jmin (rowHeight, identityBlock.getHeight()));

                s.onBtn.setBounds (identityHead.removeFromRight (
                    juce::jmin (58, identityHead.getWidth())));
                identityHead.removeFromRight (
                    juce::jmin (4, identityHead.getWidth()));
                s.tonePageBtn.setBounds (identityHead.removeFromRight (
                    juce::jmin (48, identityHead.getWidth())));
                identityHead.removeFromRight (
                    juce::jmin (4, identityHead.getWidth()));
                s.glyph.setBounds (identityHead.removeFromLeft (
                    juce::jmin (34, identityHead.getWidth()))
                    .withSizeKeepingCentre (30, 30));
                identityHead.removeFromLeft (
                    juce::jmin (3, identityHead.getWidth()));
                s.name.setBounds (identityHead);
                s.timbreName.setBounds ({});
                s.combo.setBounds (presetRow);

                // Console como no Windows (pedido do dono, 30/07 tarde):
                // fader VERTICAL com o meter segmentado ao lado, usando a
                // ALTURA toda da coluna — a dupla e a assinatura visual da
                // mesa. O fader horizontal lia como player de video. Acoes
                // continuam na base, na largura toda.
                auto performanceBlock = performance;
                auto actions = performanceBlock.removeFromBottom (
                    juce::jmin (rowHeight, performanceBlock.getHeight()));
                performanceBlock.removeFromBottom (
                    juce::jmin (rowGap, performanceBlock.getHeight()));
                auto levelArea = performanceBlock.reduced (2, 0);
                // ar sobre o thumb: sem isto o knob do fader encostava na
                // linha de acento do card
                levelArea.removeFromTop (
                    juce::jmin (10, levelArea.getHeight()));

                auto transposeColumn = levelArea.removeFromRight (
                    juce::jmin (54, levelArea.getWidth() / 3));
                s.transp.setSliderStyle (
                    juce::Slider::RotaryHorizontalVerticalDrag);
                s.transp.setTextBoxStyle (
                    juce::Slider::TextBoxBelow, false, 38, 13);
                s.transp.setBounds (transposeColumn.removeFromTop (
                    juce::jmin (74, transposeColumn.getHeight())));
                // "TRANSPOSE" escrito, como no Windows — o "0 st" sozinho
                // nao dizia O QUE o knob afina.
                s.transpLabel.setFont (
                    juce::Font (juce::FontOptions (9.5f)));
                s.transpLabel.setBounds (transposeColumn.removeFromTop (
                    juce::jmin (12, transposeColumn.getHeight())));
                levelArea.removeFromRight (
                    juce::jmin (4, levelArea.getWidth()));

                s.vol.setSliderStyle (juce::Slider::LinearVertical);

                // Leitura de dB sob o fader (padrao de mixer do Windows):
                // branco normal, ambar acima de 0 dB, vermelho acima de +3.
                auto dbRow = levelArea.removeFromBottom (
                    juce::jmin (18, levelArea.getHeight()));
                s.volDb.setBounds (dbRow);

                if (s.meter != nullptr)
                {
                    s.meter->setBounds (levelArea.removeFromRight (
                        juce::jmin (24, levelArea.getWidth() / 3))
                            .reduced (1, 2));
                    levelArea.removeFromRight (
                        juce::jmin (4, levelArea.getWidth()));
                }

                s.vol.setBounds (levelArea.withSizeKeepingCentre (
                    juce::jmin (56, levelArea.getWidth()),
                    levelArea.getHeight()));

                // Pesos em vez de quartos iguais (revisao do dono): M e S sao
                // quadrados curtos, LOOP medio, e o AUTO EQ ganha a sobra --
                // "A.EQ" espremido vira "AUTO EQ" por extenso.
                constexpr int actionGap = 4;
                const int actionUnit = juce::jmax (
                    1, (actions.getWidth() - actionGap * 3) / 4);
                const int actionSmall = juce::jmax (1, actionUnit * 4 / 5);
                s.muteBtn.setBounds (actions.removeFromLeft (actionSmall));
                actions.removeFromLeft (
                    juce::jmin (actionGap, actions.getWidth()));
                s.soloBtn.setBounds (actions.removeFromLeft (actionSmall));
                actions.removeFromLeft (
                    juce::jmin (actionGap, actions.getWidth()));
                auto loopBounds = actions.removeFromLeft (actionUnit);
                s.loop.setBounds (loopBounds);
                actions.removeFromLeft (
                    juce::jmin (actionGap, actions.getWidth()));

                if (isOrgan && ! s.eqPage)
                {
                    // No canal de organ o rail e M/S/ORGAO/rotary. O ORGAO
                    // no slot padrao truncava ("ORG..."): ele fica com a
                    // SOBRA larga e o seletor de rotary com o slot padrao.
                    s.pluginOpenBtn.setBounds (actions);
                    s.rotaryModeBox.setBounds (loopBounds);
                    s.autoEqBtn.setBounds ({});
                }
                else
                {
                    s.pluginOpenBtn.setBounds (loopBounds);
                    s.autoEqBtn.setBounds (actions);
                    s.rotaryModeBox.setBounds ({});
                }

                s.fxGroup = controlsArea;

                auto placeLandscapeKnob = [] (
                    juce::Rectangle<int> cell,
                    juce::Slider& knob, juce::Label& label)
                {
                    // Em 640x360 o arredondamento dos dois rows deixava
                    // 56 px por celula. Um label de 9 px sobrava apenas
                    // 47 px para o knob (abaixo do alvo Android de 48 dp).
                    // Num 820x360 (TANK 3) a celula passa de 90 px: o rotulo
                    // de 8,5 px ficava ilegivel de longe no palco. O tamanho
                    // acompanha a folga da celula; o aparelho compacto mantem
                    // a garantia antiga de 48 dp de knob.
                    const bool roomy = cell.getHeight() >= 78;
                    label.setFont (juce::Font (juce::FontOptions (
                        roomy ? 11.0f : 8.5f)).boldened());
                    label.setMinimumHorizontalScale (0.72f);
                    // Knob e rotulo viajam JUNTOS, centrados na celula. Antes
                    // o knob ficava no centro e o rotulo no rodape da celula
                    // de 145 dp — um vao que desconectava nome e controle
                    // (revisao do dono, item 4). +8% no knob quando ha folga.
                    const int knobSide = roomy ? 52 : 48;
                    const int labelH = roomy ? 14 : 9;
                    const int knobGap = roomy ? 4 : 0;
                    auto group = cell.withSizeKeepingCentre (
                        cell.getWidth(),
                        juce::jmin (cell.getHeight(),
                                    knobSide + knobGap + labelH));
                    auto knobArea = group.removeFromTop (
                        juce::jmax (1, group.getHeight() - labelH));
                    label.setBounds (group);
                    knob.setBounds (knobArea.withSizeKeepingCentre (
                        juce::jmin (knobSide, knobArea.getWidth()),
                        juce::jmin (knobSide, knobArea.getHeight())));
                };

                if (s.eqPage)
                {
                    const int cellW = juce::jmax (
                        1, controlsArea.getWidth() / 3);
                    placeLandscapeKnob (
                        controlsArea.removeFromLeft (cellW),
                        s.lowKnob, s.lowLabel);
                    placeLandscapeKnob (
                        controlsArea.removeFromLeft (cellW),
                        s.midKnob, s.midLabel);
                    placeLandscapeKnob (
                        controlsArea, s.highKnob, s.highLabel);
                }
                else
                {
                    struct KnobAndLabel
                    {
                        juce::Slider* knob;
                        juce::Label* label;
                    };
                    KnobAndLabel controls[6] {
                        { &s.compKnob, &s.compLabel },
                        { &s.dlyKnob, &s.dlyLabel },
                        { &s.echoKnob, &s.echoLabel }
                    };
                    int numControls = 3;

                    if (isOrgan)
                        controls[numControls++] = {
                            &s.rotaryKnob, &s.rotaryLabel
                        };

                    if (! isPlugin)
                    {
                        controls[numControls++] = {
                            &s.attackKnob, &s.attackLabel
                        };
                        controls[numControls++] = {
                            &s.releaseKnob, &s.releaseLabel
                        };
                    }

                    const int rows = numControls > 3 ? 2 : 1;
                    const int controlsPerRow =
                        (numControls + rows - 1) / rows;
                    const int controlsGap = rows == 2 ? 3 : 0;
                    const int controlRowH = juce::jmax (
                        1, (controlsArea.getHeight()
                            - controlsGap * (rows - 1)) / rows);

                    for (int row = 0; row < rows; ++row)
                    {
                        auto rowArea = controlsArea.removeFromTop (
                            juce::jmin (controlRowH,
                                        controlsArea.getHeight()));
                        if (row + 1 < rows)
                            controlsArea.removeFromTop (
                                juce::jmin (
                                    controlsGap, controlsArea.getHeight()));
                        const int first = row * controlsPerRow;
                        const int count = juce::jmin (
                            controlsPerRow, numControls - first);
                        // Celula pelo TOTAL de colunas, nao pelo resto: a
                        // fileira incompleta (ATK/REL) fica CENTRADA sob a de
                        // cima, em vez de jogada para as extremidades com um
                        // vazio no meio (revisao do dono, item 4).
                        const int cellW = juce::jmax (
                            1, rowArea.getWidth()
                                   / juce::jmax (1, controlsPerRow));

                        if (count < controlsPerRow)
                            rowArea = rowArea.withSizeKeepingCentre (
                                cellW * count, rowArea.getHeight());

                        for (int column = 0; column < count; ++column)
                        {
                            auto cell = column == count - 1
                                      ? rowArea
                                      : rowArea.removeFromLeft (cellW);
                            placeLandscapeKnob (
                                cell, *controls[first + column].knob,
                                *controls[first + column].label);
                        }
                    }
                }
                return;
            }

            s.activity.setBounds (s.cardBounds.getX() + 14, s.cardBounds.getY() + 6,
                                  juce::jmax (0, s.cardBounds.getWidth() - 28), 4);

            auto inner = s.cardBounds.reduced (16, 12);

            // mesmo teto de largura do master: no tablet o card em foco centrado
            // le como instrumento; esticado le como planilha
            inner = inner.withSizeKeepingCentre (juce::jmin (inner.getWidth(), 760),
                                                 inner.getHeight());
            inner.removeFromTop (4);

            auto head = inner.removeFromTop (48);
            s.onBtn.setBounds (head.removeFromRight (64));
            head.removeFromRight (8);
            s.glyph.setBounds (head.removeFromLeft (48).withSizeKeepingCentre (34, 34));
            head.removeFromLeft (6);
            s.name.setBounds (head.removeFromLeft (juce::jmin (104, head.getWidth() / 3)));
            s.timbreName.setBounds (head);

            if (! loaded)
            {
                // Mesma hierarquia do modo palco: anel em cima, botao embaixo.
                const int buttonW = juce::jmin (360, inner.getWidth() - 20);
                const int btnH = 58;
                const int groupH = kEmptyRing * 2 + kEmptyGapY + btnH;
                const int top = inner.getY()
                              + juce::jmax (0, (inner.getHeight() - groupH) / 2);
                s.addButton.setBounds (
                    inner.getCentreX() - buttonW / 2,
                    top + kEmptyRing * 2 + kEmptyGapY,
                    buttonW, btnH);
                return;
            }

            s.compLabel.setText ("COMP", juce::dontSendNotification);
            s.dlyLabel.setText  ("REVERB", juce::dontSendNotification);
            s.echoLabel.setText ("DELAY", juce::dontSendNotification);
            s.rotaryLabel.setText ("ROTARY", juce::dontSendNotification);
            s.attackLabel.setText ("ATK", juce::dontSendNotification);
            s.releaseLabel.setText ("REL", juce::dontSendNotification);
            s.lowLabel.setText  ("LOW", juce::dontSendNotification);
            s.midLabel.setText  ("MID", juce::dontSendNotification);
            s.highLabel.setText ("HIGH", juce::dontSendNotification);
            s.loop.setButtonText ("LOOP");
            s.autoEqBtn.setButtonText ("AUTO EQ");

           #if JUCE_ANDROID || JUCE_IOS
            const bool rotaryInActionRail = isOrgan && ! s.eqPage;
           #else
            const bool rotaryInActionRail = false;
           #endif

            inner.removeFromTop (4);
            s.combo.setBounds (inner.removeFromTop (48));
            inner.removeFromTop (8);

            auto actions = inner.removeFromBottom (48);
            constexpr int actionGap = 8;
            constexpr int actionCount = 5;
            const int actionW = juce::jmax (
                48, (actions.getWidth() - actionGap * (actionCount - 1))
                        / actionCount);
            s.muteBtn.setBounds (actions.removeFromLeft (actionW));
            actions.removeFromLeft (actionGap);
            s.soloBtn.setBounds (actions.removeFromLeft (actionW));
            actions.removeFromLeft (actionGap);
            auto loopBounds = actions.removeFromLeft (actionW);
            s.loop.setBounds (loopBounds);
            s.pluginOpenBtn.setBounds (loopBounds);
            actions.removeFromLeft (actionGap);
            s.tonePageBtn.setBounds (actions.removeFromLeft (actionW));
            actions.removeFromLeft (actionGap);

            if (rotaryInActionRail)
            {
                s.autoEqBtn.setBounds ({});
                s.rotaryModeBox.setBounds (actions);
            }
            else
            {
                s.autoEqBtn.setBounds (actions);

                if (! isOrgan || s.eqPage)
                    s.rotaryModeBox.setBounds ({});
            }
            inner.removeFromBottom (8);

            const bool stackControls = inner.getWidth() < 560;
            auto volumeArea = stackControls ? inner.removeFromTop (64)
                                            : inner.removeFromLeft (
                                                  juce::jlimit (190, 360,
                                                                inner.getWidth() * 38 / 100));

            if (stackControls)
                inner.removeFromTop (8);
            else
                inner.removeFromLeft (10);

            s.vol.setSliderStyle (juce::Slider::LinearHorizontal);
            auto db = volumeArea.removeFromBottom (16);
            s.volDb.setBounds (db);

            auto transposeArea = volumeArea.removeFromRight (
                juce::jmin (76, juce::jmax (62, volumeArea.getWidth() / 5)));
            s.transpLabel.setBounds (transposeArea.removeFromBottom (13));
            s.transp.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.transp.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
            s.transp.setBounds (transposeArea);
            volumeArea.removeFromRight (6);

            if (s.meter != nullptr)
            {
                s.meter->setBounds (volumeArea.removeFromRight (28).reduced (3, 1));
                volumeArea.removeFromRight (6);
            }

            s.vol.setBounds (volumeArea);

            s.fxGroup = inner;

            auto placeKnob = [] (juce::Rectangle<int> cell,
                                 juce::Slider& knob, juce::Label& label)
            {
                label.setBounds (cell.removeFromBottom (14));
                const int d = juce::jmax (48, juce::jmin (76,
                                            juce::jmin (cell.getWidth() - 4,
                                                        cell.getHeight())));
                knob.setBounds (cell.withSizeKeepingCentre (
                    juce::jmin (d, cell.getWidth()), juce::jmin (d, cell.getHeight())));
            };

            if (s.eqPage)
            {
                const int cellW = juce::jmax (1, inner.getWidth() / 3);
                placeKnob (inner.removeFromLeft (cellW), s.lowKnob,  s.lowLabel);
                placeKnob (inner.removeFromLeft (cellW), s.midKnob,  s.midLabel);
                placeKnob (inner,                         s.highKnob, s.highLabel);
            }
            else
            {
                struct KnobAndLabel
                {
                    juce::Slider* knob;
                    juce::Label* label;
                    juce::ComboBox* mode;
                };
                KnobAndLabel controls[6] {
                    { &s.compKnob, &s.compLabel, nullptr },
                    { &s.dlyKnob, &s.dlyLabel, nullptr },
                    { &s.echoKnob, &s.echoLabel, nullptr }
                };
                int numControls = 3;

                if (isOrgan)
                    controls[numControls++] = {
                        &s.rotaryKnob, &s.rotaryLabel,
                        rotaryInActionRail ? nullptr : &s.rotaryModeBox
                    };

                if (! isPlugin)
                {
                    controls[numControls++] = {
                        &s.attackKnob, &s.attackLabel, nullptr
                    };
                    controls[numControls++] = {
                        &s.releaseKnob, &s.releaseLabel, nullptr
                    };
                }

                auto placeControl = [&placeKnob] (
                    juce::Rectangle<int> cell, const KnobAndLabel& control)
                {
                    if (control.mode != nullptr)
                    {
                        auto selector = cell.removeFromBottom (
                            juce::jmin (48, cell.getHeight()));
                        control.mode->setBounds (selector.reduced (2, 0));
                        cell.removeFromBottom (3);
                    }

                    placeKnob (cell, *control.knob, *control.label);
                };

                // Em 320/360 dp, seis knobs em uma fileira cairiam abaixo de
                // 48 dp. O mesmo conjunto vira grade 3x2 sem rolagem.
                if (numControls > 4
                     && inner.getWidth() / numControls < 56
                     && inner.getHeight() >= 112)
                {
                    auto firstRow = inner.removeFromTop (inner.getHeight() / 2);
                    auto secondRow = inner;

                    for (int row = 0; row < 2; ++row)
                    {
                        auto cells = row == 0 ? firstRow : secondRow;
                        const int first = row * 3;
                        const int count = juce::jmin (3, numControls - first);
                        const int cellW = juce::jmax (
                            1, cells.getWidth() / juce::jmax (1, count));

                        for (int column = 0; column < count; ++column)
                        {
                            auto cell = column == count - 1
                                          ? cells : cells.removeFromLeft (cellW);
                            placeControl (cell, controls[first + column]);
                        }
                    }
                }
                else
                {
                    const int cellW = juce::jmax (
                        1, inner.getWidth() / numControls);

                    for (int i = 0; i < numControls; ++i)
                    {
                        auto cell = i == numControls - 1
                                      ? inner : inner.removeFromLeft (cellW);
                        placeControl (cell, controls[i]);
                    }
                }
            }
            return;
        }

        // Pagina MASTER: largura total e familias separadas. A altura minima do
        // editor mobile (aplicada pelo MainComponent) garante 48 dp por linha;
        // em paisagem baixa o Viewport rola esta pagina verticalmente.
        masterCardBounds = area;
        // Em 360x800, 12 dp em cima e embaixo deixavam a ultima linha do
        // MASTER com 41 dp. Mantemos o respiro em telas altas e recuperamos os
        // 8 dp necessarios para AUTO EQ/PUNCH cumprirem o alvo minimo de 48 dp.
        const int masterVerticalPadding =
            masterCardBounds.getHeight() < 430 ? 8 : 12;
        auto m = masterCardBounds.reduced (
            shortLandscapeMobileLayout ? 6 : 16,
            masterVerticalPadding);

        // Num tablet a pilha do master esticada de borda a borda vira um formulario
        // gigante; 760 dp e o maximo que ainda le como PAINEL. No celular (755 dp
        // uteis) nada muda -- o teto so trabalha onde ha sobra.
        m = m.withSizeKeepingCentre (juce::jmin (m.getWidth(), 760), m.getHeight());
        m.removeFromTop (4);

        masterDbSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        masterDbSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        sustainToggle.setButtonText ("SUSTAIN");
        invPedalToggle.setButtonText ("INV. PEDAL");
        masterAutoEqBtn.setButtonText ("AUTO EQ");
        masterPunchBtn.setButtonText ("PUNCH / PESO");
        gTranspSlider.setBounds ({});
        gTranspLabel.setBounds ({});
        fxTitle.setBounds ({});
        reverbTypeCombo.setVisible (false);
        reverbTypeCombo.setBounds ({});
        reverbReturnSlider.setVisible (false);
        reverbReturnSlider.setBounds ({});
        reverbReturnLabel.setVisible (false);
        reverbReturnLabel.setBounds ({});

        if (shortLandscapeMobileLayout)
        {
            for (int i = 0; i < TimbresProcessor::numInserts; ++i)
            {
                insertOpenButtons[i].setBounds ({});
                insertOpenButtons[i].setVisible (false);
                insertCombos[i].setBounds ({});
                insertCombos[i].setVisible (false);
            }

            masterName.setBounds (m.removeFromTop (22));
            m.removeFromTop (4);
            fxDivider = {};

            // Redesenho do dono (30/07): tres MODULOS verticais de console
            // (FX 30% | OUTPUT/LEVEL 26% | LIVE CONTROL 44%), cada um
            // preenchendo a altura — a faixa unica de 48 dp centrada deixava
            // ~96 dp mortos acima e abaixo no TANK 3. A faixa unica
            // sobrevive apenas onde ela e inevitavel (~120 dp de card no
            // 640x360).
            const bool masterModuleStack = m.getHeight() >= 150;
            const int totalW = m.getWidth();

            if (masterModuleStack)
            {
                sustainToggle.setButtonText ("SUSTAIN");
                invPedalToggle.setButtonText ("INV. PEDAL");
                masterAutoEqBtn.setButtonText ("AUTO EQ");
                masterPunchBtn.setButtonText ("PESO");

                auto fxColumn = m.removeFromLeft (totalW * 30 / 100);
                m.removeFromLeft (8);
                auto levelColumn = m.removeFromLeft (totalW * 26 / 100);
                m.removeFromLeft (8);
                auto liveColumn = m;

                masterFxGroupBounds = fxColumn;
                masterLevelGroupBounds = levelColumn;
                masterLiveGroupBounds = liveColumn;

                // ---- MASTER FX: preset em cima, retorno embaixo ----
                auto fxContent = fxColumn.reduced (8, 6);
                fxContent.removeFromTop (16);   // ar para a etiqueta "FX"
                reverbTypeCombo.setVisible (true);
                reverbTypeCombo.setBounds (fxContent.removeFromTop (
                    juce::jmin (46, fxContent.getHeight())));
                fxContent.removeFromTop (8);
                auto returnRow = fxContent.removeFromTop (
                    juce::jmin (44, fxContent.getHeight()));
                reverbReturnLabel.setVisible (true);
                reverbReturnLabel.setBounds (returnRow.removeFromLeft (
                    juce::jmin (44, returnRow.getWidth() / 3)));
                reverbReturnSlider.setVisible (true);
                reverbReturnSlider.setBounds (returnRow);

                // ---- OUTPUT/LEVEL: medidor de presenca + fader ----
                auto levelContent = levelColumn.reduced (8, 6);
                levelContent.removeFromTop (16);

                if (masterMeter != nullptr)
                {
                    masterMeter->setBounds (levelContent
                        .removeFromRight (juce::jmin (26,
                            levelContent.getWidth() / 3)).reduced (1, 2));
                    levelContent.removeFromRight (6);
                }

                // Leitura de dB do MASTER visivel (cobranca do dono): a
                // caixa de texto do slider volta — o NoTextBox herdado do
                // ramo comum escondia o valor da saida principal.
                masterDbSlider.setTextBoxStyle (
                    juce::Slider::TextBoxBelow, false, 74, 20);
                masterDbSlider.setBounds (levelContent.withSizeKeepingCentre (
                    levelContent.getWidth(),
                    juce::jmin (72, levelContent.getHeight())));

                // ---- LIVE CONTROL: PANIC prioritario, toggles grandes,
                //      EQ/PESO secundarios da MESMA largura ----
                auto liveContent = liveColumn.reduced (8, 6);
                liveContent.removeFromTop (16);
                panicButton.setBounds (liveContent.removeFromTop (
                    juce::jmin (56, liveContent.getHeight())));
                liveContent.removeFromTop (8);

                auto toggleRow = liveContent.removeFromTop (
                    juce::jmin (48, liveContent.getHeight()));
                const int halfW = (toggleRow.getWidth() - 8) / 2;
                sustainToggle.setBounds (toggleRow.removeFromLeft (halfW));
                toggleRow.removeFromLeft (8);
                invPedalToggle.setBounds (toggleRow);
                liveContent.removeFromTop (8);

                auto secondaryRow = liveContent.removeFromTop (
                    juce::jmin (42, juce::jmax (1, liveContent.getHeight())));
                const int secW = (secondaryRow.getWidth() - 8) / 2;
                masterAutoEqBtn.setBounds (secondaryRow.removeFromLeft (secW));
                secondaryRow.removeFromLeft (8);
                masterPunchBtn.setBounds (secondaryRow);
                return;
            }

            sustainToggle.setButtonText ("SUS");
            invPedalToggle.setButtonText ("INV");
            masterAutoEqBtn.setButtonText ("EQ");
            masterPunchBtn.setButtonText ("PESO");

            // Uma única faixa de 48 dp. A versão anterior empilhava três linhas
            // dentro de ~66 dp e o PANIC terminava com apenas 18 dp no
            // 640x360. Reservar larguras mínimas para FX/LEVEL deixa o restante
            // para cinco ações LIVE verdadeiramente tocáveis.
            const int fxWidth = juce::jmin (
                190, juce::jmax (168, totalW * 31 / 100));
            const int levelWidth = juce::jmin (
                92, juce::jmax (76, totalW * 15 / 100));
            auto fxColumn = m.removeFromLeft (fxWidth);
            m.removeFromLeft (8);
            auto levelColumn = m.removeFromLeft (levelWidth);
            m.removeFromLeft (8);
            auto liveColumn = m;

            masterFxGroupBounds = fxColumn;
            masterLevelGroupBounds = levelColumn;
            masterLiveGroupBounds = liveColumn;

            // No mobile os slots de plugin eram duas linhas "Sem efeito" que
            // nunca poderiam funcionar (Android/iOS nao hospedam VST/AU).
            // O mesmo espaco agora controla preset e retorno do DSP nativo.
            auto fxContent = fxColumn.reduced (4, 4)
                                  .withSizeKeepingCentre (
                                      juce::jmax (1, fxColumn.getWidth() - 8),
                                      juce::jmin (48, juce::jmax (
                                          1, fxColumn.getHeight() - 8)));

            reverbTypeCombo.setVisible (true);
            auto presetArea = fxContent.removeFromLeft (
                juce::jmin (juce::jmax (82, fxContent.getWidth() * 55 / 100),
                            fxContent.getWidth()));
            reverbTypeCombo.setBounds (presetArea);
            fxContent.removeFromLeft (4);

            auto returnRow = fxContent;
            reverbReturnLabel.setVisible (true);
            reverbReturnLabel.setBounds (returnRow.removeFromLeft (
                juce::jmin (24, returnRow.getWidth() / 3)));
            reverbReturnSlider.setVisible (true);
            reverbReturnSlider.setBounds (returnRow);

            auto levelContent = levelColumn.reduced (4, 4);
            auto levelRow = levelContent.withSizeKeepingCentre (
                levelContent.getWidth(), juce::jmin (48, levelContent.getHeight()));

            if (masterMeter != nullptr)
            {
                masterMeter->setBounds (
                    levelRow.removeFromRight (18).reduced (1));
                levelRow.removeFromRight (2);
            }

            masterDbSlider.setBounds (levelRow);

            auto liveContent = liveColumn.reduced (0, 4)
                                   .withSizeKeepingCentre (
                                       liveColumn.getWidth(),
                                       juce::jmin (48, juce::jmax (
                                           1, liveColumn.getHeight() - 8)));
            constexpr int liveControls = 5;
            constexpr int liveGap = 4;
            const int cellWidth = juce::jmax (
                1, (liveContent.getWidth()
                      - liveGap * (liveControls - 1)) / liveControls);
            juce::Component* controls[liveControls] {
                &panicButton, &sustainToggle, &invPedalToggle,
                &masterAutoEqBtn, &masterPunchBtn
            };

            for (int i = 0; i < liveControls; ++i)
            {
                auto cell = i == liveControls - 1
                              ? liveContent
                              : liveContent.removeFromLeft (cellWidth);
                controls[i]->setBounds (cell);

                if (i < liveControls - 1)
                    liveContent.removeFromLeft (liveGap);
            }
            return;
        }

        masterName.setBounds (m.removeFromTop (32));
        m.removeFromTop (6);

        const int fxTop = m.getY();

        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            insertOpenButtons[i].setBounds ({});
            insertOpenButtons[i].setVisible (false);
            insertCombos[i].setBounds ({});
            insertCombos[i].setVisible (false);
        }

        reverbTypeCombo.setVisible (true);
        reverbTypeCombo.setBounds (m.removeFromTop (48));
        m.removeFromTop (6);

        auto returnRow = m.removeFromTop (48);
        reverbReturnLabel.setVisible (true);
        reverbReturnLabel.setBounds (returnRow.removeFromLeft (
            juce::jmin (76, returnRow.getWidth() / 4)));
        reverbReturnSlider.setVisible (true);
        reverbReturnSlider.setBounds (returnRow);

        masterFxGroupBounds = { m.getX(), fxTop, m.getWidth(),
                                juce::jmax (1, m.getY() - fxTop) };
        m.removeFromTop (11);
        fxDivider = m.removeFromTop (1);
        m.removeFromTop (11);

        const int levelTop = m.getY();
        auto volume = m.removeFromTop (56);

        if (masterMeter != nullptr)
        {
            masterMeter->setBounds (volume.removeFromRight (30).reduced (3, 1));
            volume.removeFromRight (6);
        }

        // O estilo herdado do construtor e LinearVertical, mas esta faixa tem
        // 56 px de ALTURA e a largura toda da coluna (ate 760): um fader
        // vertical ali fica com 56 px de curso — "curso minusculo", como o
        // dono relatou em 30/07. Horizontal usa a largura inteira e vira um
        // fader de mesa de verdade.
        masterDbSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        masterDbSlider.setBounds (volume);
        masterLevelGroupBounds = { m.getX(), levelTop, m.getWidth(),
                                   juce::jmax (1, m.getY() - levelTop) };
        m.removeFromTop (14);

        // PANIC mais alto que as outras fileiras DE PROPOSITO: e o botao que se
        // procura em panico, literalmente. 54 dp + o halo do LookAndFeel fazem
        // ele dominar o grupo LIVE sem gritar na tela inteira.
        const int liveTop = m.getY();
        panicButton.setBounds (m.removeFromTop (54));
        m.removeFromTop (8);

        auto switches = m.removeFromTop (48);
        auto sus = switches.removeFromLeft (switches.getWidth() / 2 - 4);
        switches.removeFromLeft (8);
        sustainToggle.setBounds (sus);
        invPedalToggle.setBounds (switches);
        m.removeFromTop (8);

        auto finalRow = m.removeFromTop (48);
        auto eq = finalRow.removeFromLeft (finalRow.getWidth() / 2 - 4);
        finalRow.removeFromLeft (8);
        masterAutoEqBtn.setBounds (eq);
        masterPunchBtn.setBounds (finalRow);

        masterLiveGroupBounds = { m.getX(), liveTop, m.getWidth(),
                                  juce::jmax (1, finalRow.getBottom() - liveTop) };
    }

    // Abre o seletor com busca no lugar do menu do ComboBox. Clicar de novo (ou
    // Esc / X) fecha.
    void openPickerFor (int layer)
    {
        if (picker != nullptr)
        {
            // mesmo canal alterna (fecha); canal DIFERENTE troca o picker --
            // so fechar deixava o combo recem-clicado com "menu ativo" preso
            const bool sameLayer = (layer == pickerLayer);

            // Com o picker no componente de topo, o mouseUp do MESMO toque
            // que abriu dispara showPopup de novo (o combo perde a flag de
            // menu ativo ao mudar o foco). Sem este guarda, o seletor abria
            // e fechava no mesmo dedo.
            if (sameLayer
                 && juce::Time::getMillisecondCounter() - pickerOpenedAtMs < 400)
                return;

            closePicker();

            if (sameLayer)
                return;
        }

        selectStrip (layer);
        pickerLayer = layer;
        pickerOpenedAtMs = juce::Time::getMillisecondCounter();

        // Garante catalogo remoto + secao TIMBRES PARA BAIXAR antes de abrir.
        // Sem isto a lista abria com o snapshot antigo (so samples locais).
        refreshDownloadablePickerItems();

        auto* p = new TimbrePicker (pickerItems, strips[layer].combo.getSelectedId());
        picker.reset (p);

        p->drawIcon = [] (juce::Graphics& g, juce::Rectangle<float> r, int k)
        {
            TimbreGlyph::paintBadge (g, r, (TimbreGlyph::Kind) k);
        };

        p->colourForKind = [] (int k)
        {
            switch ((TimbreGlyph::Kind) k)
            {
                case TimbreGlyph::piano:
                case TimbreGlyph::nordBrand:    return NordLookAndFeel::catPiano();
                case TimbreGlyph::organ:        return NordLookAndFeel::catOrgan();
                case TimbreGlyph::strings:      return NordLookAndFeel::catStrings();
                case TimbreGlyph::pad:
                case TimbreGlyph::ambience:     return NordLookAndFeel::catPad();
                case TimbreGlyph::bells:        return NordLookAndFeel::catBell();
                case TimbreGlyph::lead:         return NordLookAndFeel::catLead();
                case TimbreGlyph::kontaktBrand:
                case TimbreGlyph::vstBrand:     return NordLookAndFeel::catPlugin();
                default:                        return NordLookAndFeel::txtSecondary();
            }
        };

        {
            const auto currentText = strips[layer].combo.getText();
            p->setContext ("Canal " + juce::String (layer + 1),
                           currentText == "(vazio)" ? juce::String() : currentText);
        }

        p->onPick = [this, layer] (int id)
        {
            // IDs de download nao carregam timbre — so a acao Baixar.
            if (id >= downloadBaseId)
                return;

            strips[layer].combo.setSelectedId (id, juce::sendNotificationSync);
            closePicker();
        };

        p->onDownload = [this] (const juce::String& packId)
        {
            startPickerDownload (packId);
        };

        p->onCancel = [this]
        {
            // Nao cancela download em andamento ao fechar: o installer segue
            // e o rescan acontece no finished. Fecha so a UI.
            closePicker();
        };

       #if JUCE_ANDROID || JUCE_IOS
        // TELA CHEIA de verdade: dentro do editor o card tinha ~207 dp de
        // altura no TANK 3 e a lista virava uma fresta com rolagem infinita
        // ("tem que rolar muito pra ver" — dono, 30/07). O seletor sobe para
        // o componente de topo e usa a janela inteira; a estrutura interna
        // vira duas colunas (categorias | busca + lista).
        p->useTouchLayout (true);

        // Janela e conteudo-da-janela NAO servem de ancora: o picker ficava
        // por ultimo na ordem (provado por uiautomator) e mesmo assim os
        // pixels mostravam o mixer — o componente principal composita numa
        // camada propria por cima dos irmaos. A ancora que FUNCIONA e o
        // proprio componente principal (filho do conteudo da janela), o
        // mesmo dominio do painel Audio/MIDI, que aparece. No celular ele
        // ja cobre a janela inteira — tela cheia de verdade.
        juce::Component* host = this;
        juce::Component* mainLevel = this;

        while (auto* parent = host->getParentComponent())
        {
            if (dynamic_cast<juce::ResizableWindow*> (parent) != nullptr)
                break;

            mainLevel = host;
            host = parent;
        }

        host = mainLevel;   // ultimo componente ANTES do conteudo da janela

        if (host != nullptr && host != this)
        {
            p->setSize (juce::jmax (320, host->getWidth() - 16),
                        juce::jmax (220, host->getHeight() - 12));
            host->addAndMakeVisible (p);
            p->setTopLeftPosition ((host->getWidth() - p->getWidth()) / 2,
                                   (host->getHeight() - p->getHeight()) / 2);
        }
        else
        {
            p->setSize (juce::jmax (320, getWidth() - 12),
                        juce::jmax (220, getHeight() - 10));
            addAndMakeVisible (p);
        }

        p->toFront (true);
       #else
        addAndMakeVisible (p);

        // No editor compacto o picker de desktop (518 px logicos) passava do
        // rodape. Escala uniforme mantem a hierarquia, os alvos e a busca.
        const float fitX = (float) juce::jmax (1, getWidth()  - 8) / (float) p->getWidth();
        const float fitY = (float) juce::jmax (1, getHeight() - 8) / (float) p->getHeight();
        const float pickerScale = juce::jmin (1.0f, juce::jmin (fitX, fitY));

        if (pickerScale < 0.999f)
            p->setTransform (juce::AffineTransform::scale (pickerScale));

        const int visualW = juce::roundToInt ((float) p->getWidth()  * pickerScale);
        const int visualH = juce::roundToInt ((float) p->getHeight() * pickerScale);

        const int visualX = juce::jmax (2, (getWidth()  - visualW) / 2);
        const int visualY = juce::jmax (2, (getHeight() - visualH) / 2);
        p->setTopLeftPosition (juce::roundToInt ((float) visualX / pickerScale),
                               juce::roundToInt ((float) visualY / pickerScale));
        p->toFront (true);
       #endif

        // foco inicial (lista no touch, busca no desktop) so depois de montado
        juce::Component::SafePointer<TimbrePicker> safePicker (p);
        juce::MessageManager::callAsync ([safePicker]
        {
            if (auto* c = safePicker.getComponent())
                c->focusSearch();
        });
    }

    // descarte ADIADO: fechar de dentro do proprio callback destruiria o
    // componente com o handler ainda na pilha
    void closePicker()
    {
        // devolve o ComboBox ao estado "menu fechado", senao o proximo clique
        // nao dispara showPopup() de novo
        if (juce::isPositiveAndBelow (pickerLayer, TimbresProcessor::numLayers))
            strips[pickerLayer].combo.hidePopup();

        pickerLayer = -1;

        // a posse vai pro lambda: um picker novo pode nascer antes do descarte
        // adiado rodar, e um reset() no membro destruiria o novo
        auto doomed = std::make_shared<std::unique_ptr<TimbrePicker>> (std::move (picker));
        juce::MessageManager::callAsync ([doomed] { doomed->reset(); });
    }

    std::unique_ptr<TimbrePicker> picker;
    juce::Array<TimbrePicker::Item> pickerItems;
    juce::Array<ChurchDAWServices::LibraryPack> downloadPacks;
    juce::StringArray downloadableTimbreIds;
    ChurchDAWTimbreCatalog::Catalog timbreCatalog;
    bool timbreCatalogFetchRunning = false;
    std::unique_ptr<ChurchDAWServices::LibraryInstaller> pickerInstaller;
    std::unique_ptr<ChurchDAWTimbreCatalog::Installer> timbreInstaller;
    int pickerLayer = -1;
    juce::uint32 pickerOpenedAtMs = 0;   // guarda contra o toggle no mesmo toque

    // Geometria do convite do card vazio, compartilhada por paint() e resized()
    // pra o circulo tracejado e o botao nunca se separarem.
    static constexpr int kEmptyRing = 30;   // raio do circulo tracejado (mais tocavel)
    static constexpr int kEmptyGapY = 12;   // respiro entre o circulo e o botao
    static constexpr int kEmptyBtnH = 40;

    static juce::Point<int> emptyRingCentre (juce::Rectangle<int> card)
    {
        const int groupH = kEmptyRing * 2 + kEmptyGapY + kEmptyBtnH;
        const int top = card.getCentreY() - groupH / 2;
        return { card.getCentreX(), top + kEmptyRing };
    }

    struct Strip
    {
        juce::Label name, timbreName, transpLabel, volDb;
        TimbreGlyph glyph;
        ActivityBar activity;   // acento do topo do card acende com o sinal
        PickerComboBox combo;   // abre o seletor com busca em vez do menu padrao
        // Entrada MIDI deste canal: Todos | Teclado A | Teclado B (desktop).
        juce::ComboBox midiInBox;
        juce::Slider vol, transp;
        juce::TextButton loop { "Loop" };
        juce::TextButton muteBtn { "M" }, soloBtn { "S" }, onBtn { "ON" };
        juce::TextButton autoEqBtn { "Auto EQ" };
        juce::TextButton tonePageBtn { "EQ" };
        juce::TextButton pluginOpenBtn { "Abrir" };   // canal-plugin (abre a interface do VSTi)
        juce::TextButton addButton { "Adicionar" };   // card vazio (+ no anel)
        // Remover o timbre exigia abrir o seletor e escolher "canal vazio" —
        // o dono chamou de esquisito (30/07) e tem razao: e uma acao de
        // limpar escondida dentro de uma acao de escolher. Este X fica na
        // cara do card, so aparece com timbre carregado.
        juce::TextButton clearBtn { juce::String::fromUTF8 ("\xc3\x97") };
        juce::ComboBox rotaryModeBox;
        juce::Slider compKnob, dlyKnob, echoKnob, rotaryKnob,
                     attackKnob, releaseKnob,
                     lowKnob, midKnob, highKnob;
        juce::Label compLabel, dlyLabel, echoLabel, rotaryLabel,
                    attackLabel, releaseLabel,
                    lowLabel, midLabel, highLabel;
        std::unique_ptr<LevelMeter> meter;
        float pendingL = 0.0f, pendingR = 0.0f;   // pico lido no quadro (L e R juntos)
        juce::Rectangle<int> cardBounds, tabBounds;
        juce::Point<int> stageEmptyRingCentre;
        juce::Rectangle<int> fxGroup;   // cinta de agrupamento em volta dos knobs
        bool lastLoaded = false;
        bool eqPage = false;
        std::unique_ptr<juce::SliderParameterAttachment> volAttachment, transpAttachment,
                                                         compAttachment, dlyAttachment, echoAttachment,
                                                         rotaryAttachment,
                                                         attackAttachment, releaseAttachment,
                                                         lowAttachment, midAttachment, highAttachment;
        std::unique_ptr<juce::ComboBoxParameterAttachment> rotaryModeAttachment;
        std::unique_ptr<juce::ButtonParameterAttachment> loopAttachment, muteAttachment,
                                                         soloAttachment, onAttachment, autoEqAttachment;
    };

    void updateTonePageVisibility (int layer)
    {
        if (! juce::isPositiveAndBelow (layer, TimbresProcessor::numLayers))
            return;

        auto& strip = strips[layer];
        const bool eq = strip.eqPage;
        const bool loaded = proc.getLayerTimbre (layer).isNotEmpty()
                         || proc.getLayerInstrument (layer) != nullptr;
        const bool isPlugin = proc.getLayerInstrument (layer) != nullptr;
        const bool isChurchOrgan =
            proc.isLayerChurchOrgan (layer);
        const bool isOrgan = proc.isLayerOrgan (layer);

        juce::Component* sharedFxControls[] = {
            &strip.compKnob, &strip.dlyKnob, &strip.echoKnob,
            &strip.compLabel, &strip.dlyLabel, &strip.echoLabel
        };

        for (auto* component : sharedFxControls)
            component->setVisible (loaded && ! eq);

        strip.rotaryKnob.setVisible (loaded && ! eq && isOrgan);
        strip.rotaryLabel.setVisible (loaded && ! eq && isOrgan);
        strip.rotaryModeBox.setVisible (loaded && ! eq && isOrgan);

        // O sampler nativo tem envelope proprio. VST/AU recebe COMP, Reverb,
        // Delay e EQ pos-instrumento, mas ATK/REL nao controlariam seu envelope;
        // esconder e mais honesto que oferecer um knob sem efeito.
        juce::Component* nativeEnvelopeControls[] = {
            &strip.attackKnob, &strip.releaseKnob,
            &strip.attackLabel, &strip.releaseLabel
        };

        for (auto* component : nativeEnvelopeControls)
            component->setVisible (
                loaded && ! eq && ! isPlugin
                && ! isChurchOrgan);

        juce::Component* eqControls[] = {
            &strip.lowKnob, &strip.midKnob, &strip.highKnob,
            &strip.lowLabel, &strip.midLabel, &strip.highLabel
        };

        for (auto* component : eqControls)
            component->setVisible (loaded && eq);

        strip.tonePageBtn.setVisible (loaded);
        strip.tonePageBtn.setButtonText (eq ? "ENV" : "EQ");
    }

    void updateTonePage (int layer)
    {
        updateTonePageVisibility (layer);
        resized();
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (auto* plugins = proc.getKnownPlugins();
            plugins != nullptr && source == plugins)
        {
            refreshPluginCatalogPreservingSelection();
            return;
        }

        if (auto* m = proc.getExternalMaster(); m != nullptr && source == m)
        {
            syncingMaster = true;
            masterDbSlider.setValue (m->getMasterDb(), juce::dontSendNotification);
            masterAutoEqBtn.setToggleState (m->getAutoEq(),
                                            juce::dontSendNotification);
            masterPunchBtn.setToggleState (m->getPunch(),
                                           juce::dontSendNotification);
            syncingMaster = false;
            refreshMasterDbColour();
            refreshMasterUtilityLabels();
            return;
        }

        refreshFromProcessor();
    }

    // ganho > 0 dB (impulsionando o master) fica ambar/vermelho pra avisar ao vivo
    void refreshMasterDbColour()
    {
        const float db = (float) masterDbSlider.getValue();
        const auto c = db > 3.0f  ? juce::Colour (0xffd83a3a)
                     : db > 0.05f ? juce::Colour (0xffe0a83a)
                                  : juce::Colours::white.withAlpha (0.85f);
        masterDbSlider.setColour (juce::Slider::textBoxTextColourId, c);
        masterDbSlider.repaint();
    }

    void refreshMasterUtilityLabels()
    {
       #if JUCE_ANDROID || JUCE_IOS
        // O touch usa abreviacoes proprias definidas no layout responsivo.
        return;
       #else
        if (getHeight() < 380)
        {
            masterAutoEqBtn.setButtonText ("EQ");
            masterPunchBtn.setButtonText ("PESO");
            return;
        }

        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
        masterAutoEqBtn.setButtonText (
            juce::String ("AUTO EQ") + dot
                + (masterAutoEqBtn.getToggleState() ? "ON" : "OFF"));
        masterPunchBtn.setButtonText (
            juce::String ("PESO") + dot
                + (masterPunchBtn.getToggleState() ? "ON" : "OFF"));
       #endif
    }

    void selectStrip (int l)
    {
        if (selectedStrip != l && juce::isPositiveAndBelow (l, TimbresProcessor::numLayers))
        {
            const int previous = selectedStrip;
            selectedStrip = l;
            proc.setSelectedLayer (l);   // o visor OLED do cabecalho segue este

            // "acende" o rotulo do canal em foco e apaga os outros
            for (int i = 0; i < TimbresProcessor::numLayers; ++i)
                strips[i].name.setColour (juce::Label::textColourId,
                                          i == l ? juce::Colours::white
                                                 : NordLookAndFeel::dimText());

            // Repaint so dos cards que mudaram (nao a tela inteira).
            if (juce::isPositiveAndBelow (previous, TimbresProcessor::numLayers)
                 && ! strips[previous].cardBounds.isEmpty())
                repaint (strips[previous].cardBounds.expanded (4));
            if (! strips[l].cardBounds.isEmpty())
                repaint (strips[l].cardBounds.expanded (4));
            if (strips[l].cardBounds.isEmpty() && strips[previous].cardBounds.isEmpty())
                repaint();

            // relayout DEFERIDO (move os detalhes p/ o canal em foco). callAsync
            // evita re-entrancia: selectStrip e chamado de dentro do handler de
            // mouse do proprio knob/combo — chamar resized() ali corrompe o
            // despacho do evento. SafePointer protege contra destruicao.
            juce::Component::SafePointer<TimbresEditor> safe (this);
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* e = safe.getComponent())
                    e->resized();
            });
        }
    }

    void closeHostedPluginEditorBeforeRetire (
        TimbresProcessor::HostedPluginLocation location,
        int index,
        juce::AudioPluginInstance* instance)
    {
        if (instance == nullptr)
            return;

        const bool matches =
            location == TimbresProcessor::HostedPluginLocation::insert
                ? juce::isPositiveAndBelow (
                      index, TimbresProcessor::numInserts)
                    && insertWindows[index] != nullptr
                    && insertWindows[index]->getPluginInstance()
                           == instance
                : juce::isPositiveAndBelow (
                      index, TimbresProcessor::numLayers)
                    && layerPluginWindows[index] != nullptr
                    && layerPluginWindows[index]->getPluginInstance()
                           == instance;

        if (! matches)
            return;

        // AudioProcessorEditor::~AudioProcessorEditor de alguns vendors abre
        // um loop modal. Adie qualquer completion AUv3 entregue nesse intervalo
        // ate a referencia ao plugin ter desaparecido por completo.
        auto* processor = &proc;
        processor->beginHostedPluginEditorTeardown();
        const juce::ScopeGuard finishEditorTeardown { [processor]
        {
            processor->endHostedPluginEditorTeardown();
        } };

        if (location
             == TimbresProcessor::HostedPluginLocation::insert)
            insertWindows[index].reset();
        else
            layerPluginWindows[index].reset();
    }

    // abre o editor do plugin do MASTER FX EMBUTIDO (card "nativo" dentro do
    // Church Live, arrastavel) — nada de janela separada do sistema
    void openInsertEmbedded (int i)
    {
        if (! juce::isPositiveAndBelow (i, TimbresProcessor::numInserts))
            return;

        if (insertWindows[i] != nullptr)
        {
            insertWindows[i]->toFront (true);
            return;
        }

        if (auto* p = proc.getInsert (i))
        {
            auto safe = juce::Component::SafePointer<TimbresEditor> (this);
            std::weak_ptr<std::atomic<bool>> lifetime (
                editorCallbackLifetime);

            insertWindows[i] = std::make_unique<InsertEditorCard> (*p, [safe, lifetime, i]
            {
                juce::MessageManager::callAsync ([safe, lifetime, i]
                {
                    const auto alive = lifetime.lock();

                    if (alive == nullptr
                         || ! alive->load (
                             std::memory_order_acquire))
                        return;

                    if (auto* ed = safe.getComponent())
                        ed->insertWindows[i].reset();
                });
            });

            addAndMakeVisible (*insertWindows[i]);

            // centraliza com clamp: editor maior que a tela ainda mostra o topo
            auto b = insertWindows[i]->getBounds();
            insertWindows[i]->setTopLeftPosition (juce::jmax (2, (getWidth()  - b.getWidth())  / 2),
                                                  juce::jmax (2, (getHeight() - b.getHeight()) / 2));
            insertWindows[i]->toFront (true);
        }
    }

    // abre a interface do VSTi (Kontakt) hospedado no canal
    void openLayerPlugin (int l)
    {
        if (! juce::isPositiveAndBelow (l, TimbresProcessor::numLayers))
            return;

        if (proc.isLayerChurchOrgan (l))
        {
            openChurchOrganEditor (l);
            return;
        }

        if (layerPluginWindows[l] != nullptr)
        {
            layerPluginWindows[l]->toFront (true);
            return;
        }

        if (auto* p = proc.getLayerInstrument (l))
        {
            auto safe = juce::Component::SafePointer<TimbresEditor> (this);
            std::weak_ptr<std::atomic<bool>> lifetime (
                editorCallbackLifetime);

            layerPluginWindows[l] = std::make_unique<InsertPluginWindow> (*p, [safe, lifetime, l]
            {
                juce::MessageManager::callAsync ([safe, lifetime, l]
                {
                    const auto alive = lifetime.lock();

                    if (alive == nullptr
                         || ! alive->load (
                             std::memory_order_acquire))
                        return;

                    if (auto* ed = safe.getComponent())
                        ed->layerPluginWindows[l].reset();
                });
            });
        }
    }

    void openChurchOrganEditor (int layer)
    {
        if (! juce::isPositiveAndBelow (
                layer, TimbresProcessor::numLayers)
             || ! proc.isLayerChurchOrgan (layer))
            return;

        if (churchOrganOverlay != nullptr
             && churchOrganOverlayLayer == layer)
        {
            churchOrganOverlay->toFront (true);
            return;
        }

        closeChurchOrganEditor();
        churchOrganOverlayLayer = layer;
        churchOrganOverlay =
            std::make_unique<TimbresChurchOrganOverlay> (
                proc, layer);

        auto safe =
            juce::Component::SafePointer<TimbresEditor> (
                this);
        churchOrganOverlay->onClose = [safe]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* editor = safe.getComponent())
                    editor->closeChurchOrganEditor();
            });
        };

        addAndMakeVisible (*churchOrganOverlay);
        churchOrganOverlay->setBounds (getLocalBounds());
        churchOrganOverlay->toFront (true);
        churchOrganOverlay->grabKeyboardFocus();
    }

public:
    // Botao BACK do Android (via MainComponent::handleBackButton): fecha a
    // camada de cima — seletor de timbres, organ ou Modo Palco — em vez de
    // deixar o app morrer durante a apresentacao.
    bool handleBackRequest()
    {
        if (picker != nullptr)
        {
            closePicker();
            return true;
        }

        if (churchOrganOverlay != nullptr)
        {
            closeChurchOrganEditor();
            return true;
        }

       #if JUCE_ANDROID
        if (mobileStageMode)
        {
            mobileStageMode = false;
            mobileMasterPage = false;
            androidStageRevealSelected = false;
            editToneButton.setToggleState (false,
                                           juce::dontSendNotification);
            editToneButton.setButtonText ("MODO PALCO");
            editToneButton.setTooltip (
                "Ver faders, meters, Mute, Solo e Master em tempo real");
            resized();
            repaint();
            return true;
        }
       #endif

        return false;
    }

    void closeChurchOrganEditor()
    {
        if (churchOrganOverlay == nullptr)
            return;

        churchOrganOverlayLayer = -1;
        auto doomed = std::make_shared<
            std::unique_ptr<TimbresChurchOrganOverlay>> (
                std::move (churchOrganOverlay));
        juce::MessageManager::callAsync (
            [doomed] { doomed->reset(); });
    }

    // remove sufixos tecnicos do nome exibido ("Amber Upright LP4 FC" -> "Amber Upright")
    static juce::String displayName (const juce::String& label)
    {
        return TimbresProcessor::prettyLabel (label);
    }


    // titulo de categoria bonito para o card (ex.: "PIANOS" -> "Piano")
    // Cor de apoio da categoria. Entra so na linha de topo, no icone e no arco
    // dos knobs -- nunca no texto, no fundo do card, nos medidores nem no PANIC.
    juce::Colour categoryColourFor (int l) const
    {
        // escolha manual do usuario tem prioridade sobre a categoria
        if (const int idx = proc.getLayerColourIndex (l); idx >= 0)
            return TimbresProcessor::channelColour (idx);

        const bool isPlugin = proc.getLayerInstrument (l) != nullptr;

        switch (TimbreGlyph::resolveKind (isPlugin, proc.getLayerInstrumentName (l),
                                          proc.getLayerTimbre (l)))
        {
            case TimbreGlyph::piano:
            case TimbreGlyph::nordBrand:    return NordLookAndFeel::catPiano();
            case TimbreGlyph::organ:        return NordLookAndFeel::catOrgan();
            case TimbreGlyph::strings:      return NordLookAndFeel::catStrings();
            case TimbreGlyph::pad:
            case TimbreGlyph::ambience:     return NordLookAndFeel::catPad();
            case TimbreGlyph::bells:        return NordLookAndFeel::catBell();
            case TimbreGlyph::lead:         return NordLookAndFeel::catLead();
            case TimbreGlyph::kontaktBrand:
            case TimbreGlyph::vstBrand:     return NordLookAndFeel::catPlugin();
            default:                        return NordLookAndFeel::txtSecondary();
        }
    }

    juce::String categoryTitleFor (const juce::String& timbre) const
    {
        juce::String cat;

        for (const auto& e : proc.getEntriesNonBlocking())
            if (e.label == timbre)
            {
                cat = e.category.isNotEmpty() ? e.category : categoryFor (e.label);
                break;
            }

        if (cat.isEmpty())
            cat = categoryFor (timbre);

        if (cat == "PIANOS")          return "Piano";
        if (cat == "EP & TECLAS")     return "EP & Teclas";
        if (cat == "ORGAOS")          return juce::String::fromUTF8 ("\xc3\x93rg\xc3\xa3o");   // Ã“rgÃ£o
        if (cat == "PADS")            return "Pad";
        if (cat == "AMBIENCIAS")      return juce::String::fromUTF8 ("Ambi\xc3\xaancia");   // AmbiÃªncia
        if (cat == "BELLS")           return "Bell";
        if (cat == "STRINGS")         return "Strings";
        if (cat == "LEADS & SYNTHS")  return "Lead / Synth";

        return "Instrumento";
    }

    // titulo humano da categoria para o SUBTITULO dos itens do seletor
    // ("Piano acustico · Church Live" em vez da chave tecnica "PIANOS")
    static juce::String categoryDisplayTitle (const juce::String& key)
    {
        if (key == "PIANOS")          return juce::String::fromUTF8 ("Piano ac\xc3\xbastico");
        if (key == "EP & TECLAS")     return "EP & Teclas";
        if (key == "ORGAOS")          return juce::String::fromUTF8 ("\xc3\x93rg\xc3\xa3o");
        if (key == "PADS")            return juce::String::fromUTF8 ("Pad atmosf\xc3\xa9rico");
        if (key == "AMBIENCIAS")      return juce::String::fromUTF8 ("Ambi\xc3\xaancia");
        if (key == "BELLS")           return "Bell";
        if (key == "STRINGS")         return "Strings";
        if (key == "LEADS & SYNTHS")  return "Lead / Synth";
        return "Instrumento";
    }

    static juce::String categoryFor (const juce::String& label)
    {
        const auto n = label.toLowerCase();

        if (n.contains ("ambien"))
            return "AMBIENCIAS";

        if (n.contains ("bell"))
            return "BELLS";

        if (n.contains ("b3") || n.contains ("organ") || n.contains ("pipe") || n.contains ("farf"))
            return "ORGAOS";

        if (n.contains ("string"))
            return "STRINGS";

        if (n.startsWith ("ep") || n.contains ("tines") || n.contains ("keys"))
            return "EP & TECLAS";

        if (n.contains ("piano") || n.contains ("grand") || n.contains ("upright"))
            return "PIANOS";

        if (n.contains ("pad") || n.contains ("warm") || n.contains ("vox"))
            return "PADS";

        if (n.contains ("lead") || n.contains ("saw") || n.contains ("sine") || n.contains ("plucked")
             || n.contains ("synth") || n.contains ("analog"))
            return "LEADS & SYNTHS";

        return "OUTROS";
    }

    void rebuildPluginCaches()
    {
        effectsCache.clearQuick();
        instrumentsCache.clearQuick();

        const auto addUnique = [] (
            juce::Array<juce::PluginDescription>& destination,
            const juce::PluginDescription& description)
        {
            const auto identifier =
                description.createIdentifierString();

            for (const auto& existing : destination)
                if (existing.createIdentifierString() == identifier)
                    return;

            destination.add (description);
        };

        if (auto* list = proc.getKnownPlugins())
            for (const auto& description : list->getTypes())
                if (! proc.isHostedPluginBlocked (description))
                    addUnique (description.isInstrument
                                   ? instrumentsCache
                                   : effectsCache,
                               description);

        // Uma atualização do scanner não pode transformar em "vazio" um
        // plugin que já está tocando. Preserve sua descrição mesmo se a lista
        // conhecida estiver, naquele instante, sendo reconstruída.
        for (int slot = 0; slot < TimbresProcessor::numInserts; ++slot)
        {
            if (auto* plugin = proc.getInsert (slot))
                addUnique (effectsCache,
                           plugin->getPluginDescription());

            juce::PluginDescription pending;
            if (proc.getPendingInsertDescription (slot, pending)
                 && ! proc.isHostedPluginBlocked (pending))
                addUnique (effectsCache, pending);
        }

        for (int layer = 0;
             layer < TimbresProcessor::numLayers;
             ++layer)
        {
            if (auto* plugin = proc.getLayerInstrument (layer))
                addUnique (instrumentsCache,
                           plugin->getPluginDescription());

            juce::PluginDescription pending;
            if (proc.getPendingInstrumentDescription (layer, pending)
                 && ! proc.isHostedPluginBlocked (pending))
                addUnique (instrumentsCache, pending);
        }
    }

    static int findPluginChoice (
        const juce::Array<juce::PluginDescription>& choices,
        const juce::String& identifier)
    {
        return ChurchDAWPluginLoading::findChoiceByIdentifier (
            choices, identifier);
    }

    void refreshPluginCatalogPreservingSelection()
    {
        juce::String selectedInsertIds[
            TimbresProcessor::numInserts];
        juce::String selectedInstrumentIds[
            TimbresProcessor::numLayers];

        for (int slot = 0;
             slot < TimbresProcessor::numInserts;
             ++slot)
        {
            const int oldIndex =
                insertCombos[slot].getSelectedId() - 2;

            if (juce::isPositiveAndBelow (
                    oldIndex, effectsCache.size()))
                selectedInsertIds[slot] =
                    effectsCache.getReference (oldIndex)
                        .createIdentifierString();
            else if (auto* plugin = proc.getInsert (slot))
                selectedInsertIds[slot] =
                    plugin->getPluginDescription()
                        .createIdentifierString();
        }

        for (int layer = 0;
             layer < TimbresProcessor::numLayers;
             ++layer)
        {
            const int oldIndex =
                strips[layer].combo.getSelectedId() - vstBaseId;

            if (juce::isPositiveAndBelow (
                    oldIndex, instrumentsCache.size()))
                selectedInstrumentIds[layer] =
                    instrumentsCache.getReference (oldIndex)
                        .createIdentifierString();
            else if (auto* plugin =
                         proc.getLayerInstrument (layer))
                selectedInstrumentIds[layer] =
                    plugin->getPluginDescription()
                        .createIdentifierString();
        }

        const bool wasUpdating = updatingCombos;
        updatingCombos = true;
        rebuildPluginCaches();

        for (int slot = 0;
             slot < TimbresProcessor::numInserts;
             ++slot)
        {
            auto& combo = insertCombos[slot];
            combo.clear (juce::dontSendNotification);
            combo.addItem ("Sem efeito", 1);

            for (int i = 0; i < effectsCache.size(); ++i)
                combo.addItem (
                    ChurchDAWPluginLoading::displayName (
                        effectsCache.getReference (i),
                        effectsCache),
                    i + 2);

            const int selected = findPluginChoice (
                effectsCache, selectedInsertIds[slot]);
            combo.setSelectedId (
                selected >= 0 ? selected + 2 : 1,
                juce::dontSendNotification);
        }

        // Reconstrói também o picker dos canais. Depois restaura cada AU/VST
        // pela identidade estável, nunca pelo índice que mudou com o scan.
        populateCombos();

        for (int layer = 0;
             layer < TimbresProcessor::numLayers;
             ++layer)
        {
            const int selected = findPluginChoice (
                instrumentsCache,
                selectedInstrumentIds[layer]);

            if (selected >= 0)
                strips[layer].combo.setSelectedId (
                    vstBaseId + selected,
                    juce::dontSendNotification);
        }

        updatingCombos = wasUpdating;
        resized();
        repaint();
    }

    void populateCombos()
    {
        const auto& entries = proc.getEntriesNonBlocking();

        static const char* categoryOrder[] = { "PIANOS", "EP & TECLAS", "ORGAOS", "PADS",
                                               "AMBIENCIAS", "BELLS", "STRINGS",
                                               "LEADS & SYNTHS", "OUTROS" };

        // ordem das bibliotecas (para o seletor cruzado)
        juce::StringArray libOrder;

        for (int e = 0; e < entries.size(); ++e)
        {
            if (! TimbresProcessor::
                    isSampleTimbreVisibleForNewSelection (
                        entries.getReference (e)))
                continue;

            const auto& lib = entries.getReference (e).library;
            libOrder.addIfNotAlreadyThere (lib.isNotEmpty() ? lib : juce::String ("Biblioteca"));
        }

        // Monta a lista (id, nome, secao) UMA vez: e a mesma que alimenta os
        // combos de todos os canais E o seletor com busca.
        pickerItems.clearQuick();
        downloadPacks.clearQuick();

        // O ComboBox interno sempre teve "(vazio)", mas o seletor com busca
        // substitui o popup padrao e nao copiava essa opcao. Resultado: depois
        // de carregar um timbre nao existia caminho de toque para remover o
        // instrumento do canal. Mantemos a acao no topo, separada dos sons.
        pickerItems.add ({ 1, "Canal vazio / remover instrumento", "CANAL",
                           (int) TimbreGlyph::kindForName ("vazio") });

        for (int family = 0;
             family < TimbresProcessor::churchOrganFamilyCount;
             ++family)
        {
            const auto organFamily =
                static_cast<churchorgan::ChurchOrganFamily> (
                    family);
            pickerItems.add ({
                organBaseId + family,
                TimbresProcessor::churchOrganFamilyName (
                    organFamily),
                "CHURCH ORGAN ENGINE",
                (int) TimbreGlyph::organ,
                "ORGAOS",
                juce::String::fromUTF8 ("\xc3\x93rg\xc3\xa3o \xc2\xb7 Motor dedicado")
            });
        }

        // VSTi externos (Kontakt etc.) primeiro - "de fora"
        for (int i = 0; i < instrumentsCache.size(); ++i)
        {
            const auto& plugin = instrumentsCache.getReference (i);
            const auto name = ChurchDAWPluginLoading::displayName (
                plugin, instrumentsCache);
            auto detail = plugin.pluginFormatName.trim();

            if (plugin.manufacturerName.trim().isNotEmpty())
                detail += juce::String::fromUTF8 (" \xc2\xb7 ")
                       + plugin.manufacturerName.trim();

            pickerItems.add ({ vstBaseId + i, name, juce::String ("PLUGINS / KONTAKT"),
                               (int) TimbreGlyph::resolveKind (true, plugin.name, {}),
                               juce::String ("PLUGINS"),
                               detail.isNotEmpty()
                                   ? detail
                                   : juce::String ("Plugin externo") });
        }

        // agrupa por BIBLIOTECA -> CATEGORIA (escolha timbre de qualquer lib)
        for (const auto& lib : libOrder)
        {
            for (const char* cat : categoryOrder)
            {
                for (int e = 0; e < entries.size(); ++e)
                {
                    const auto& entry = entries.getReference (e);
                    const auto entryLib = entry.library.isNotEmpty() ? entry.library : juce::String ("Biblioteca");

                    if (entryLib != lib)
                        continue;

                    if (! TimbresProcessor::
                            isSampleTimbreVisibleForNewSelection (
                                entry))
                        continue;

                    const auto entryCat = entry.category.isNotEmpty() ? entry.category
                                                                      : categoryFor (entry.label);

                    if (entryCat != cat)
                        continue;

                    // kindForName (e nao resolveKind): na lista o que interessa e
                    // distinguir CATEGORIA. resolveKind transforma todo piano em
                    // badge Nord — o que deixaria metade da lista igual e ainda
                    // atribuiria a marca Nord a timbres que nao sao dela.
                    pickerItems.add ({ e + 2, displayName (entry.label), lib.toUpperCase(),
                                       (int) TimbreGlyph::kindForName (entry.label),
                                       entryCat,
                                       categoryDisplayTitle (entryCat)
                                           + juce::String::fromUTF8 (" \xc2\xb7 ") + lib });
                }
            }
        }

        // Bibliotecas do catalogo ainda nao instaladas: aparecem no seletor
        // com botao Baixar (um pack por vez — unidade real do servidor).
        appendDownloadablePacksToPicker();

        for (auto& s : strips)
        {
            const int current = s.combo.getSelectedId();

            s.combo.clear (juce::dontSendNotification);
            s.combo.addItem ("(vazio)", 1);

            juce::String lastSection;

            for (const auto& it : pickerItems)
            {
                // O ComboBox ja recebeu "(vazio)" com id 1 logo acima. O item
                // descritivo de remocao existe para o picker customizado; somar
                // os dois criava IDs duplicados (cinco assertions por refresh).
                // Packs baixaveis tambem ficam so no seletor visual.
                if (it.id == 1 || it.downloadable)
                    continue;

                if (it.section != lastSection)
                {
                    s.combo.addSectionHeading (it.section);   // o LookAndFeel desenha o filete
                    lastSection = it.section;
                }

                s.combo.addItem (it.name, it.id);
            }

            s.combo.setSelectedId (current, juce::dontSendNotification);
        }

        knownEntryCount = entries.size();
    }

    void ensureTimbreCatalogLoaded()
    {
        // Cache imediato (nao bloqueia).
        if (! timbreCatalog.valid)
            timbreCatalog = ChurchDAWTimbreCatalog::loadCachedCatalog();

        if (timbreCatalogFetchRunning)
            return;

        // Sempre busca o remoto na primeira vez (e de novo se o cache era
        // invalido). Assim o seletor recebe a lista de timbres individuais.
        timbreCatalogFetchRunning = true;
        auto safe = juce::Component::SafePointer<TimbresEditor> (this);
        juce::Thread::launch ([safe]
        {
            auto remote = ChurchDAWTimbreCatalog::fetchRemoteCatalog();
            juce::MessageManager::callAsync (
                [safe, remote = std::move (remote)]() mutable
                {
                    if (auto* self = safe.getComponent())
                    {
                        self->timbreCatalogFetchRunning = false;

                        if (remote.valid)
                        {
                            self->timbreCatalog = std::move (remote);
                            juce::Logger::writeToLog (
                                "TIMBRE-CATALOG: "
                                + juce::String (self->timbreCatalog.instruments.size())
                                + " instrumentos");

                            // SEMPRE reconstrói a lista (antes so atualizava com
                            // o picker aberto — por isso a secao sumia).
                            self->populateCombos();

                            if (self->picker != nullptr)
                            {
                                const int layer = self->pickerLayer;
                                const int current =
                                    juce::isPositiveAndBelow (
                                        layer, TimbresProcessor::numLayers)
                                        ? self->strips[layer].combo.getSelectedId()
                                        : 0;
                                self->picker->replaceItems (self->pickerItems,
                                                            current);
                            }
                        }
                        else
                        {
                            juce::Logger::writeToLog (
                                "TIMBRE-CATALOG: falha "
                                + remote.diagnostic);
                        }
                    }
                });
        });
    }

    // Remove itens baixaveis antigos e recoloca com o catalogo atual.
    // Chamado ao abrir o seletor para nao depender so do populateCombos inicial.
    void refreshDownloadablePickerItems()
    {
        ensureTimbreCatalogLoaded();

        juce::Array<TimbrePicker::Item> kept;
        kept.ensureStorageAllocated (pickerItems.size());

        for (const auto& it : pickerItems)
            if (! it.downloadable)
                kept.add (it);

        pickerItems = std::move (kept);
        appendDownloadablePacksToPicker();
    }

    void appendDownloadablePacksToPicker()
    {
        // Nao chama ensureTimbreCatalogLoaded aqui de forma recursiva com
        // refresh — o ensure ja foi feito pelo chamador ou populateCombos.
        if (! timbreCatalogFetchRunning && ! timbreCatalog.valid)
            ensureTimbreCatalogLoaded();

        downloadableTimbreIds.clearQuick();

        // 1) TIMBRES individuais prontos no catalogo (Baixar 1 a 1).
        // Orgaos de sample (B3, Farf, Pipe...) NAO entram: o produto usa o
        // Church Organ Engine. Nome limpo (sem LP/LP4/FC) e sem "N samples".
        if (timbreCatalog.valid)
        {
            for (const auto& inst : timbreCatalog.instruments)
            {
                if (! inst.canDownload())
                    continue;

                const auto labelForFilter = inst.rawLabel.isNotEmpty()
                                                ? inst.rawLabel
                                                : inst.name;

                if (TimbresProcessor::looksLikeOrgan (labelForFilter, inst.category)
                     || inst.category.equalsIgnoreCase ("ORGAOS"))
                    continue;

                // id/path do pack no servidor (b3-fabio, pipe-organs, farf...)
                if (inst.id.containsIgnoreCase ("b3")
                     || inst.id.containsIgnoreCase ("farf")
                     || inst.id.containsIgnoreCase ("pipe")
                     || inst.id.containsIgnoreCase ("organ")
                     || inst.id.containsIgnoreCase ("orgao"))
                    continue;

                if (inst.isInstalledLocally())
                    continue;

                const int id = downloadBaseId + (int) downloadableTimbreIds.size();
                downloadableTimbreIds.add ("timbre:" + inst.id);

                juce::String action = "Baixar";

                if (inst.downloadBytes > 0)
                    action += juce::String::fromUTF8 (" \xc2\xb7 ")
                           + ChurchDAWServices::humanBytes (inst.downloadBytes);

                // Nome comercial limpo: tira LP4, FC e codigos de amostra.
                const auto pretty = displayName (
                    labelForFilter.isNotEmpty() ? labelForFilter : inst.name);

                TimbrePicker::Item item;
                item.id = id;
                item.name = pretty.isNotEmpty() ? pretty : inst.name;
                item.section = juce::String::fromUTF8 ("Timbres para baixar");
                item.kind = (int) TimbreGlyph::kindForName (labelForFilter);
                item.category = inst.category;
                // Subtitulo = categoria legivel; se o nome ja e generico, reforca
                // com o id limpo pra nao parecer item quebrado.
                item.subtitle = categoryDisplayTitle (inst.category);
                if (item.name.equalsIgnoreCase (item.subtitle)
                     || item.name.containsIgnoreCase ("acustico")
                     || item.name.containsIgnoreCase ("acústico"))
                {
                    const auto fromId = displayName (inst.id.replaceCharacter ('-', ' '));
                    if (fromId.isNotEmpty()
                         && ! fromId.equalsIgnoreCase (item.subtitle))
                        item.name = fromId;
                }
                item.downloadable = true;
                item.packId = "timbre:" + inst.id;
                item.actionLabel = action;
                item.actionEnabled = true;
                item.progress = -1.0f;
                pickerItems.add (std::move (item));
            }
        }

        // 2) Packs legados (Pad Continuo, Church Live completo) se ainda faltarem.
        downloadPacks.clearQuick();
        const auto manifest = ChurchDAWServices::loadCachedManifest();

        if (! manifest.valid)
            return;

        for (const auto& pack : manifest.libraries)
        {
            if (pack.isInstalled())
                continue;

            // SEMPRE oferece pack completo (church-live / pad-continuo) alem
            // dos timbres 1 a 1 — o cliente escolhe o que baixar.
            const int id = downloadBaseId + downloadableTimbreIds.size();
            downloadableTimbreIds.add (pack.id);
            downloadPacks.add (pack);

            juce::String action;
            bool enabled = false;

            if (! pack.included)
            {
                action = pack.priceLabel.isNotEmpty()
                             ? pack.priceLabel
                             : juce::String ("Em breve");
            }
            else if (pack.canDownload())
            {
                if (pack.id.equalsIgnoreCase ("church-live"))
                    action = "Baixar tudo";
                else
                    action = "Baixar pack";

                if (pack.downloadBytes > 0)
                    action += juce::String::fromUTF8 (" \xc2\xb7 ")
                           + ChurchDAWServices::humanBytes (pack.downloadBytes);

                enabled = true;
            }
            else
            {
                action = "Indisponivel";
            }

            juce::String categoryKey;

            if (pack.id == "pad-continuo")
                categoryKey = "PADS";
            else if (pack.id.containsIgnoreCase ("organ"))
                categoryKey = "ORGAOS";
            else if (pack.id.containsIgnoreCase ("piano")
                      || pack.id.containsIgnoreCase ("keys"))
                categoryKey = "PIANOS";

            juce::String subtitle;
            // Nome + subtitulo claros: evita linha generica "Piano acustico" sem
            // contexto (pack grande lendo como um piano sozinho).
            juce::String packName;
            if (pack.id.equalsIgnoreCase ("church-live"))
            {
                packName = "Church Live completo";
                subtitle = juce::String::fromUTF8 (
                    "Biblioteca completa \xc2\xb7 todos os timbres (~12 GB)");
            }
            else if (pack.id.equalsIgnoreCase ("pad-continuo"))
            {
                packName = "Pad continuo";
                subtitle = juce::String::fromUTF8 (
                    "Pack leve para culto \xc2\xb7 pads A\xe2\x80\x93G");
            }
            else
            {
                packName = ChurchDAWAndroidCulto::displayNameForPack (
                    pack.id, pack.name);
                subtitle = juce::String::fromUTF8 (
                    "Pack completo \xc2\xb7 alternativa ao download por timbre");
            }

            TimbrePicker::Item item;
            item.id = id;
            item.name = packName;
            item.section = juce::String::fromUTF8 ("Bibliotecas completas");
            item.kind = pack.id == "pad-continuo" ? (int) TimbreGlyph::pad
                                                  : (int) TimbreGlyph::piano;
            item.category = categoryKey;
            item.subtitle = subtitle;
            item.downloadable = true;
            item.packId = pack.id;
            item.actionLabel = action;
            item.actionEnabled = enabled;
            item.progress = -1.0f;
            pickerItems.add (std::move (item));
        }
    }

    const ChurchDAWTimbreCatalog::Instrument* findDownloadableTimbre (
        const juce::String& timbreId) const
    {
        for (const auto& inst : timbreCatalog.instruments)
            if (inst.id == timbreId)
                return &inst;

        return nullptr;
    }

    const ChurchDAWServices::LibraryPack* findDownloadPack (
        const juce::String& packId) const
    {
        for (const auto& p : downloadPacks)
            if (p.id == packId)
                return &p;

        return nullptr;
    }

    void refreshPickerAfterInstall()
    {
        proc.rescanLibrary();
        populateCombos();

        if (picker != nullptr)
        {
            const int layer = pickerLayer;
            const int current =
                juce::isPositiveAndBelow (layer, TimbresProcessor::numLayers)
                    ? strips[layer].combo.getSelectedId()
                    : 0;
            picker->replaceItems (pickerItems, current);
        }
    }

    void startPickerDownload (const juce::String& packId)
    {
        if (pickerInstaller != nullptr || timbreInstaller != nullptr)
            return;

        // ---- Timbre individual ----
        if (packId.startsWith ("timbre:"))
        {
            const auto timbreId = packId.fromFirstOccurrenceOf ("timbre:", false, false);
            const auto* found = findDownloadableTimbre (timbreId);

            if (found == nullptr || ! found->canDownload())
            {
                if (picker != nullptr)
                    picker->updateDownloadState (
                        packId, -1.0f, "Indisponivel", false);
                return;
            }

            const auto inst = *found;

            auto beginTimbre = [this, inst, packId]
            {
                if (timbreInstaller != nullptr)
                    return;

                if (picker != nullptr)
                    picker->updateDownloadState (packId, 0.0f, "Iniciando...", false);

                auto safe = juce::Component::SafePointer<TimbresEditor> (this);
                timbreInstaller =
                    std::make_unique<ChurchDAWTimbreCatalog::Installer> (
                        inst,
                        [safe, packId] (float value, const juce::String& text)
                        {
                            if (auto* self = safe.getComponent())
                                if (self->picker != nullptr)
                                {
                                    juce::String label =
                                        text.isNotEmpty() ? text
                                                          : juce::String ("Baixando...");

                                    if (label.length() > 18)
                                        label = juce::String ((int) (value * 100.0f))
                                              + "%";

                                    self->picker->updateDownloadState (
                                        packId, value, label, false);
                                }
                        },
                        [safe, packId] (juce::Result result)
                        {
                            if (auto* self = safe.getComponent())
                            {
                                self->timbreInstaller.reset();

                                if (result.wasOk())
                                    self->refreshPickerAfterInstall();
                                else if (self->picker != nullptr)
                                    self->picker->updateDownloadState (
                                        packId, -1.0f, "Tentar de novo", true);
                            }
                        });
                timbreInstaller->begin();
            };

            if (inst.downloadBytes >= (juce::int64) 80 * 1024 * 1024)
            {
                juce::AlertWindow::showOkCancelBox (
                    juce::MessageBoxIconType::QuestionIcon,
                    "Baixar " + inst.name + "?",
                    "Timbre individual\n\n"
                        "Download: "
                        + ChurchDAWServices::humanBytes (inst.downloadBytes)
                        + "\nSamples: " + juce::String (inst.fileCount)
                        + "\n\nPrefira Wi-Fi. Depois ele aparece na lista "
                          "para tocar.",
                    "Baixar agora",
                    "Cancelar",
                    this,
                    juce::ModalCallbackFunction::create (
                        [safe = juce::Component::SafePointer<TimbresEditor> (this),
                         beginTimbre] (int result)
                        {
                            if (result != 0 && safe != nullptr)
                                beginTimbre();
                        }));
                return;
            }

            beginTimbre();
            return;
        }

        // ---- Pack completo (legado) ----
        const auto* found = findDownloadPack (packId);

        if (found == nullptr || ! found->canDownload() || ! found->included)
            return;

        const auto pack = *found;

        auto begin = [this, pack]()
        {
            if (pickerInstaller != nullptr)
                return;

            if (picker != nullptr)
                picker->updateDownloadState (pack.id, 0.0f, "Iniciando...", false);

            auto safe = juce::Component::SafePointer<TimbresEditor> (this);
            pickerInstaller = std::make_unique<ChurchDAWServices::LibraryInstaller> (
                pack,
                [safe, id = pack.id] (float value, const juce::String& text)
                {
                    if (auto* self = safe.getComponent())
                        if (self->picker != nullptr)
                        {
                            juce::String label = text.isNotEmpty()
                                                     ? text
                                                     : juce::String ("Baixando...");

                            if (label.length() > 18)
                                label = juce::String ((int) (value * 100.0f)) + "%";

                            self->picker->updateDownloadState (
                                id, value, label, false);
                        }
                },
                [safe, id = pack.id] (juce::Result result)
                {
                    if (auto* self = safe.getComponent())
                    {
                        self->pickerInstaller.reset();

                        if (result.wasOk())
                            self->refreshPickerAfterInstall();
                        else if (self->picker != nullptr)
                            self->picker->updateDownloadState (
                                id, -1.0f, "Tentar de novo", true);
                    }
                });
            pickerInstaller->begin();
        };

        if (pack.downloadBytes >= (juce::int64) 100 * 1024 * 1024)
        {
            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::QuestionIcon,
                "Baixar " + pack.name + "?",
                "Pack completo (varios timbres).\n\n"
                    "Download: "
                    + ChurchDAWServices::humanBytes (pack.downloadBytes)
                    + "\nPrefira Wi-Fi e mantenha o app aberto.",
                "Baixar agora",
                "Cancelar",
                this,
                juce::ModalCallbackFunction::create (
                    [safe = juce::Component::SafePointer<TimbresEditor> (this),
                     begin] (int result)
                    {
                        if (result != 0 && safe != nullptr)
                            begin();
                    }));
            return;
        }

        begin();
    }

    void refreshFromProcessor()
    {
        updatingCombos = true;

        const auto& entries = proc.getEntriesNonBlocking();

        if (knownEntryCount != entries.size())
            populateCombos();

        // O desktop abria com selectedStrip = -1 mesmo quando cinco canais ja
        // estavam carregados. A moldura de selecao existia, mas nunca aparecia
        // ate o primeiro clique. Segue a selecao persistida; sem uma, prefere o
        // primeiro canal carregado e deixa o estado imediatamente legivel.
        if (! juce::isPositiveAndBelow (selectedStrip,
                                        TimbresProcessor::numLayers))
        {
            int initial = proc.getSelectedLayer();

            if (! juce::isPositiveAndBelow (initial,
                                             TimbresProcessor::numLayers))
                for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                    if (proc.getLayerTimbre (l).isNotEmpty()
                         || proc.getLayerInstrument (l) != nullptr
                         || proc.isLayerChurchOrgan (l))
                    {
                        initial = l;
                        break;
                    }

            if (! juce::isPositiveAndBelow (initial,
                                             TimbresProcessor::numLayers))
                initial = 0;

            selectedStrip = initial;
            proc.setSelectedLayer (initial);
        }

        bool layoutChanged = false;

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];
            const auto timbre = proc.getLayerTimbre (l);
            const auto libName = proc.getLayerLibrary (l);
            const auto vstName = proc.getLayerInstrumentName (l);   // canal com Kontakt/VST?
            const bool isPlugin = vstName.isNotEmpty();
            const bool isChurchOrgan =
                proc.isLayerChurchOrgan (l);
            const bool loaded =
                timbre.isNotEmpty() || isPlugin
                || isChurchOrgan;
            int selected = 1;
            bool loadedHiddenSampleOrgan = false;
            juce::PluginDescription pendingInstrument;
            const bool hasPendingInstrument =
                proc.getPendingInstrumentDescription (
                    l, pendingInstrument);

            // plugin do canal morreu por fora (restore de estado, troca
            // interna): a janela dele nao pode sobreviver ao instance
            if (proc.getLayerInstrument (l) == nullptr)
                layerPluginWindows[l].reset();

            if (hasPendingInstrument)
            {
                const auto pendingId =
                    pendingInstrument.createIdentifierString();

                for (int i = 0; i < instrumentsCache.size(); ++i)
                    if (instrumentsCache.getReference (i)
                            .createIdentifierString() == pendingId)
                    {
                        selected = vstBaseId + i;
                        break;
                    }
            }
            else if (isChurchOrgan)
            {
                selected = organBaseId
                         + static_cast<int> (
                               proc.getLayerChurchOrganFamily (
                                   l));
            }
            else if (isPlugin)
            {
                const auto vstId = proc.getLayerInstrument (l)->getPluginDescription().createIdentifierString();

                for (int i = 0; i < instrumentsCache.size(); ++i)
                    if (instrumentsCache.getReference (i).createIdentifierString() == vstId)
                    {
                        selected = vstBaseId + i;
                        break;
                    }
            }
            else
            {
                // casa rotulo + biblioteca (rotulos podem repetir entre bibliotecas)
                for (int e = 0; e < entries.size(); ++e)
                    if (entries.getReference (e).label == timbre
                         && (libName.isEmpty() || entries.getReference (e).library == libName))
                    {
                        const auto& entry =
                            entries.getReference (e);

                        if (TimbresProcessor::
                                isSampleTimbreVisibleForNewSelection (
                                    entry))
                            selected = e + 2;
                        else
                        {
                            // Setup legado continua carregado e identificado,
                            // mas o timbre sampleado não volta a ser uma opção
                            // no catálogo para novas escolhas.
                            selected = 0;
                            loadedHiddenSampleOrgan = true;
                        }
                        break;
                    }
            }

            s.combo.setSelectedId (selected, juce::dontSendNotification);

            if (loadedHiddenSampleOrgan)
                s.combo.setText (
                    displayName (timbre),
                    juce::dontSendNotification);
            s.glyph.setKind (TimbreGlyph::resolveKind (isPlugin, vstName, timbre), loaded);
            // titulo forte = tipo/categoria; seletor abaixo = preset/plugin
            s.timbreName.setText (isPlugin ? vstName : (loaded ? categoryTitleFor (timbre) : juce::String()),
                                  juce::dontSendNotification);

            // Atualiza lista de teclados MIDI para este canal.
            {
                s.midiInBox.clear (juce::dontSendNotification);
                s.midiInBox.addItem (
                    juce::String::fromUTF8 ("MIDI: Todos"), 1);
                int selectedId = 1;
                const auto chosen = proc.getLayerMidiInputDevice (l);
                const auto devices = juce::MidiInput::getAvailableDevices();
                int nextId = 2;
                for (const auto& d : devices)
                {
                    // Nome legivel; o id real fica no texto do item via
                    // setItemEnabled + armazenamento no item text = identifier.
                    // Usamos o identifier como item text interno? Combobox
                    // mostra name; guardamos identifier no user data via id
                    // sequencial e mapa — simples: item text = name, e no
                    // onChange reconsulta devices pela ordem.
                    // Melhor: item text = identifier, name no texto:
                    const auto label =
                        juce::String::fromUTF8 ("MIDI: ")
                        + (d.name.isNotEmpty() ? d.name : d.identifier);
                    s.midiInBox.addItem (label, nextId);
                    // Guarda identifier no item text de forma recuperavel:
                    // trocamos para usar identifier como selected text base.
                    if (chosen.isNotEmpty()
                         && (chosen == d.identifier || chosen == d.name))
                        selectedId = nextId;
                    ++nextId;
                }
                // Se o device sumiu, mantem o id escolhido como item extra.
                if (chosen.isNotEmpty() && selectedId == 1)
                {
                    s.midiInBox.addItem (
                        juce::String::fromUTF8 ("MIDI: ") + chosen
                            + juce::String::fromUTF8 (" (offline)"),
                        nextId);
                    selectedId = nextId;
                }
                s.midiInBox.setSelectedId (
                    selectedId, juce::dontSendNotification);
                s.midiInBox.setEnabled (true);
                s.midiInBox.setVisible (true);
            }

            // canais nativos tem knobs de canal-strip; canal-plugin mostra "Abrir"
            s.name.setVisible (true);   // "CANAL X" sempre visivel (cheio ou vazio)
            s.name.setColour (juce::Label::textColourId,
                              l == selectedStrip ? juce::Colours::white
                                                 : NordLookAndFeel::dimText());

            // cor da categoria nos arcos dos knobs e no icone
            {
                const auto cc = categoryColourFor (l);

                for (auto* k : { &s.compKnob, &s.dlyKnob, &s.echoKnob,
                                 &s.rotaryKnob,
                                 &s.attackKnob, &s.releaseKnob,
                                 &s.lowKnob, &s.midKnob, &s.highKnob })
                    k->setColour (juce::Slider::rotarySliderFillColourId, cc);
            }

            juce::Component* stripControls[] = { &s.glyph, &s.timbreName, &s.vol, &s.volDb, &s.combo,
                                                 &s.transp, &s.transpLabel,
                                                 &s.muteBtn, &s.soloBtn,
                                                  &s.compKnob, &s.compLabel,
                                                  &s.dlyKnob, &s.echoKnob,
                                                  &s.rotaryKnob,
                                                  &s.attackKnob,
                                                  &s.releaseKnob, &s.lowKnob,
                                                  &s.midKnob, &s.highKnob,
                                                  &s.dlyLabel, &s.echoLabel,
                                                  &s.rotaryLabel,
                                                  &s.attackLabel,
                                                 &s.releaseLabel, &s.lowLabel,
                                                 &s.midLabel, &s.highLabel,
                                                  &s.onBtn, &s.autoEqBtn,
                                                  &s.tonePageBtn,
                                                  &s.rotaryModeBox };

            for (auto* c : stripControls)
                c->setVisible (loaded);

            // MIDI de teclado: sempre visivel (pode pré-escolher com canal vazio).
            s.midiInBox.setVisible (true);

            updateTonePageVisibility (l);

            s.loop.setVisible (
                loaded && ! isPlugin && ! isChurchOrgan);
            // "EDITAR ORGAN" nao cabia na largura do botao: em retrato quebrava
            // em duas linhas encostando na borda, e em paisagem quebrava DENTRO
            // da palavra, exibindo "EDITA / R N...". Conferido nas capturas de
            // 30/07/2026 em .claude/reports/visual-regression/.
            //
            // O vizinho "Abrir" (5 caracteres) cabe folgado, entao o rotulo curto
            // resolve sem tocar em layout. Acentuacao escapada segue a convencao
            // do arquivo -- ver "\xc3\x93rg\xc3\xa3o" nos nomes de timbre.
            s.pluginOpenBtn.setButtonText (
                isChurchOrgan ? juce::String::fromUTF8 ("\xc3\x93RG\xc3\x83O")
                              : juce::String ("Abrir"));
            s.pluginOpenBtn.setVisible (
                isPlugin || isChurchOrgan);
            s.addButton.setVisible (! loaded);

            if (s.meter != nullptr)
                s.meter->setVisible (loaded);

            s.activity.setVisible (loaded);

            if (s.lastLoaded != loaded)
            {
                s.lastLoaded = loaded;
                layoutChanged = true;
            }
        }

        if (churchOrganOverlay != nullptr
             && (! juce::isPositiveAndBelow (
                     churchOrganOverlayLayer,
                     TimbresProcessor::numLayers)
                 || ! proc.isLayerChurchOrgan (
                        churchOrganOverlayLayer)))
            closeChurchOrganEditor();

        // plugins internos: sincroniza combos e botoes
        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            int selected = 1;
            juce::PluginDescription pendingInsert;

            if (proc.getPendingInsertDescription (i, pendingInsert))
            {
                const auto id =
                    pendingInsert.createIdentifierString();

                for (int e = 0; e < effectsCache.size(); ++e)
                    if (effectsCache.getReference (e)
                            .createIdentifierString() == id)
                    {
                        selected = e + 2;
                        break;
                    }
            }
            else if (auto* p = proc.getInsert (i))
            {
                const auto id = p->getPluginDescription().createIdentifierString();

                for (int e = 0; e < effectsCache.size(); ++e)
                    if (effectsCache.getReference (e).createIdentifierString() == id)
                    {
                        selected = e + 2;
                        break;
                    }
            }

            const bool effectLoaded = proc.getInsert (i) != nullptr;
            insertCombos[i].setSelectedId (selected, juce::dontSendNotification);
            insertOpenButtons[i].setEnabled (effectLoaded);
            // Slot vazio ganha toda a largura. "Editar" aparece apenas quando
            // existe algo que de fato possa ser aberto.
            insertOpenButtons[i].setVisible (effectLoaded);
        }

        // segue a maquina que estiver de fato no plugin (o usuario pode ter
        // trocado pela interface do BigSky)
        {
            const int m = proc.getReverbMachine();
            reverbTypeCombo.setSelectedId (m >= 0 ? m + 1 : 0, juce::dontSendNotification);
        }

        updatingCombos = false;

        // Layout so se loaded/empty mudou. Full repaint e o maior ofensor de
        // jank no Android (software renderer) — no modo palco basta status.
        if (layoutChanged)
        {
            resized();
            repaint();
        }
        else
        {
           #if JUCE_ANDROID || JUCE_IOS
            // Status bar + molduras leves: meters/activity se atualizam sozinhos.
            repaint (0, 0, getWidth(), 52);
            if (mobileStageMode)
            {
                for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                    if (! strips[l].cardBounds.isEmpty())
                        repaint (strips[l].cardBounds.withHeight (48));
            }
           #else
            repaint();
           #endif
        }

        const bool loading    = proc.isLoading();
        const bool ready      = ! proc.getEntriesNonBlocking().isEmpty();
        const bool libMissing = ! ready && proc.hasScannedLibrary();
        libraryMissing = libMissing;

        // O convite central e o maior alvo da tela vazia. Quando nao existe
        // pack, ele precisa resolver a causa em vez de abrir um seletor sem
        // itens. A cor vermelha tambem eleva o erro acima do status discreto do
        // cabecalho, sem criar outro modal.
        for (auto& s : strips)
        {
            s.addButton.setButtonText (libMissing ? "Abrir bibliotecas"
                                                   : "Adicionar");
            s.addButton.setColour (
                juce::TextButton::buttonColourId,
                libMissing ? NordLookAndFeel::nordRedDark()
                           : juce::Colours::transparentBlack);
            s.addButton.setColour (
                juce::TextButton::textColourOffId,
                libMissing ? juce::Colours::white
                           : NordLookAndFeel::dimText());
        }

        status.setText (loading ? "Carregando timbre..."
                          : ready ? "Pronto"
                          : libMissing ? juce::String::fromUTF8 ("Biblioteca n\xc3\xa3o instalada \xe2\x80\x94 abra Bibliotecas")
                                       : "Lendo biblioteca...",
                        juce::dontSendNotification);

        statusColour = loading ? juce::Colour (0xffe0a83a)      // ambar
                        : ready ? juce::Colour (0xff6fcf6f)      // verde
                        : libMissing ? juce::Colour (0xffd83a3a) // vermelho
                                     : juce::Colour (0xffe0a83a);

        bool hasLoadedChannel = false;

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
            hasLoadedChannel = hasLoadedChannel
                            || proc.getLayerTimbre (l).isNotEmpty()
                            || proc.getLayerInstrument (l) != nullptr;

       #if JUCE_ANDROID
        // Mesmo que o ultimo canal seja limpo enquanto o palco esta aberto, a
        // saida para o editor detalhado nunca pode desaparecer.
        editToneButton.setEnabled (hasLoadedChannel || mobileStageMode);
       #else
        editToneButton.setEnabled (hasLoadedChannel);
       #endif
        repaint (0, 0, getWidth(), 46);
    }

    TimbresProcessor& proc;

    // MESA COM 14 CANAIS, 8 NA TELA.
    //
    // Dividir a largura por 14 deixava cada card com ~120 px: nome do
    // timbre virava "...", os botoes viravam "Adic..." e os knobs
    // sumiam. Oito e a conta que mantem o card legivel num monitor
    // comum; o resto entra rolando de lado, com as setas ‹ › e a roda
    // do mouse. Quem usa cinco canais nao ve diferenca nenhuma.
    int canaisNaTela = 8;
    int primeiroCanalVisivel = 0;   // 0..numLayers-canaisNaTela
    juce::TextButton canalAnteriorBtn { juce::String::fromUTF8 ("\xe2\x80\xb9") },
                     canalProximoBtn  { juce::String::fromUTF8 ("\xe2\x80\xba") };
    juce::Rectangle<int> faixaDeCanais;   // area onde a rolagem vale

    void limitarPrimeiroCanalVisivel()
    {
        primeiroCanalVisivel =
            ChurchDAWAndroidStageLayout::clampPageStart (
                primeiroCanalVisivel, canaisNaTela,
                TimbresProcessor::numLayers);
    }

    /** Tira o canal da tela zerando os bounds de tudo que ele desenha.
        Componente de area zero nao pinta nem recebe clique — e, ao voltar
        para a janela de rolagem, o proprio resized() devolve os bounds.
        setVisible(false) aqui foi tentado antes e DESLIGAVA o canal para
        sempre: os canais 9..14 voltavam sem titulo e sem controle, porque
        nada no layout os liga de novo. */
    void esconderStrip (Strip& s)
    {
        s.cardBounds = {};
        s.tabBounds = {};
        s.fxGroup = {};
        s.stageEmptyRingCentre = {};

        const std::initializer_list<juce::Component*> partes {
            &s.name, &s.timbreName, &s.transpLabel, &s.volDb,
            &s.glyph, &s.activity, &s.combo, &s.midiInBox, &s.vol, &s.transp,
            &s.loop, &s.muteBtn, &s.soloBtn, &s.onBtn, &s.autoEqBtn,
            &s.tonePageBtn, &s.pluginOpenBtn, &s.addButton, &s.clearBtn,
            &s.rotaryModeBox, &s.compKnob, &s.dlyKnob, &s.echoKnob,
            &s.rotaryKnob, &s.attackKnob, &s.releaseKnob, &s.lowKnob,
            &s.midKnob, &s.highKnob, &s.compLabel, &s.dlyLabel,
            &s.echoLabel, &s.rotaryLabel, &s.attackLabel,
            &s.releaseLabel, &s.lowLabel, &s.midLabel, &s.highLabel
        };

        for (auto* c : partes)
            c->setBounds ({});

        if (s.meter != nullptr)
            s.meter->setBounds ({});
    }

    void posicionarSetasDeCanal (juce::Rectangle<int> area, int stripW)
    {
        const bool precisaRolar =
            TimbresProcessor::numLayers > canaisNaTela;

        canalAnteriorBtn.setVisible (precisaRolar);
        canalProximoBtn.setVisible (precisaRolar);

        if (! precisaRolar)
        {
            canalAnteriorBtn.setBounds ({});
            canalProximoBtn.setBounds ({});
            return;
        }

        // As setas ficam nas pontas da faixa de canais, na altura do
        // cabecalho do card, longe de fader e knobs.
        const int lado = 26;
        const int alturaBotao = 26;
        const int y = area.getY() + 4;

        canalAnteriorBtn.setBounds (area.getX() + 2, y, lado, alturaBotao);
        canalProximoBtn.setBounds (
            area.getX() + canaisNaTela * stripW - lado - 2, y, lado, alturaBotao);
        // Os labels/controles dos strips recebem bounds depois das setas e podem
        // compartilhar esta faixa do cabecalho. Garanta que o alvo clicavel das
        // setas fique acima deles, nao apenas o desenho visivel.
        canalAnteriorBtn.toFront (false);
        canalProximoBtn.toFront (false);

        canalAnteriorBtn.setEnabled (primeiroCanalVisivel > 0);
        canalProximoBtn.setEnabled (
            primeiroCanalVisivel
                < TimbresProcessor::numLayers - canaisNaTela);
    }

    void rolarCanais (int passos)
    {
       #if JUCE_ANDROID
        if (mobileStageMode || focusedMobileLayout)
        {
            const int antes = primeiroCanalVisivel;
            if (passos > 0)
                primeiroCanalVisivel =
                    ChurchDAWAndroidStageLayout::nextPageStart (
                        primeiroCanalVisivel, canaisNaTela,
                        TimbresProcessor::numLayers);
            else if (passos < 0)
                primeiroCanalVisivel =
                    ChurchDAWAndroidStageLayout::previousPageStart (
                        primeiroCanalVisivel, canaisNaTela,
                        TimbresProcessor::numLayers);

            androidStageRevealSelected = false;
            limitarPrimeiroCanalVisivel();

            if (primeiroCanalVisivel != antes)
            {
                resized();
                repaint();
            }

            return;
        }
       #endif

        const int antes = primeiroCanalVisivel;
        primeiroCanalVisivel += passos;
        limitarPrimeiroCanalVisivel();

        if (primeiroCanalVisivel != antes)
        {
            resized();
            repaint();
        }
    }

    Strip strips[TimbresProcessor::numLayers];
    juce::TextButton mobilePageButtons[TimbresProcessor::numLayers];
    juce::TextButton mobileMasterPageButton;
    juce::Label title, status, masterName, gTranspLabel, fxTitle, reverbReturnLabel;
    juce::TextButton editToneButton;
    juce::Colour statusColour { 0xff6fcf6f };   // bolinha de status: verde=pronto, ambar=carregando, vermelho=erro
    juce::Slider masterDbSlider, gTranspSlider, reverbReturnSlider;
    juce::TextButton panicButton { "PANIC" };
    juce::ToggleButton sustainToggle, invPedalToggle;
    juce::TextButton masterAutoEqBtn, masterPunchBtn;
    std::unique_ptr<LevelMeter> masterMeter;
    juce::Rectangle<int> masterCardBounds, bottomPanelBounds;
    juce::Rectangle<int> masterFxGroupBounds, masterLevelGroupBounds,
                         masterLiveGroupBounds, masterUtilityGroupBounds;
    std::unique_ptr<juce::SliderParameterAttachment> gTranspAttachment;
    std::unique_ptr<juce::SliderParameterAttachment> reverbReturnAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> sustainAttachment, invPedalAttachment;

    juce::Array<juce::PluginDescription> effectsCache;
    juce::Array<juce::PluginDescription> instrumentsCache;   // Kontakt e VSTi para os canais
    std::unique_ptr<InsertPluginWindow> layerPluginWindows[TimbresProcessor::numLayers];
    std::unique_ptr<TimbresChurchOrganOverlay>
        churchOrganOverlay;
    int churchOrganOverlayLayer = -1;
    static constexpr int organBaseId = 400000;
    static constexpr int vstBaseId = 500000;                 // ids do combo p/ VSTi
    static constexpr int downloadBaseId = 800000;            // packs online (so no seletor)
    juce::ComboBox insertCombos[TimbresProcessor::numInserts];
    juce::TextButton insertOpenButtons[TimbresProcessor::numInserts];
    std::unique_ptr<InsertEditorCard> insertWindows[TimbresProcessor::numInserts];   // editores EMBUTIDOS (estilo nativo)
    bool updatingCombos = false;
    bool syncingMaster = false;
    std::shared_ptr<std::atomic<bool>> editorCallbackLifetime =
        std::make_shared<std::atomic<bool>> (true);
    bool desktopToneEditMode = false;
    juce::ComboBox reverbTypeCombo;   // Church Reverb ou BigSky externo
    juce::Rectangle<int> fxDivider;   // linha que separa MASTER FX do bloco de saida
    int selectedStrip = -1;   // nenhum canal em foco ao abrir
    int knownEntryCount = -1;
    bool focusedMobileLayout = false;
    bool shortLandscapeMobileLayout = false;
    bool mobileMasterPage = false;
   #if JUCE_ANDROID
    bool mobileStageMode = false;
    bool androidStageRevealSelected = false;
    int androidStageLastChannelsOnScreen = 0;
   #endif
    bool libraryMissing = false;
    std::function<void()> onOpenLibraries;
    juce::Rectangle<int> mobileMasterTabBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbresEditor)
};

inline juce::AudioProcessorEditor* TimbresProcessor::createEditor()
{
    return new TimbresEditor (*this);
}
