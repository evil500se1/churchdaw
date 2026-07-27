#pragma once

#include "AppPaths.h"
#include "SampleStream.h"

#if JUCE_ANDROID
 #include <sched.h>
 #include <unistd.h>
#endif

// Streaming do disco: ligado no celular, onde carregar tudo na RAM nao cabe
// (medido: 1,9 GB por piano, 9,5 GB com os 5 canais cheios num aparelho com
// 8,6 GB livres). No PC o caminho de RAM continua -- ele e mais simples, ja
// esta provado, e la a memoria sobra.
#ifndef CHURCHDAW_STREAM_SAMPLES
 #if JUCE_ANDROID || JUCE_IOS
  #define CHURCHDAW_STREAM_SAMPLES 1
 #else
  #define CHURCHDAW_STREAM_SAMPLES 0
 #endif
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "NordLookAndFeel.h"
#include "LevelMeter.h"
#include "MasterProcessor.h"
#include "TimbrePicker.h"
#include "BinaryData.h"

#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

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
    static constexpr int numLayers = 5;

    // Modelo Nord: cada secao tem orcamento proprio e fechado de vozes, para que
    // um pad segurado no pedal nunca roube a polifonia do piano. Antes eram 64
    // vozes num pool unico -- e como cada tecla dispara uma voz POR camada, dois
    // acordes com pedal ja estouravam o pool e cortavam o piano no meio.
    static constexpr int layerVoices[numLayers] = { 96, 40, 40, 40, 40 };
    static constexpr int maxVoices = 96 + 40 + 40 + 40 + 40;

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
    // 2 segundos muda a natureza do problema: a maioria das notas de piano
    // termina dentro disso e NUNCA toca o disco. So as sustentadas transmitem, e
    // aI sao poucas ao mesmo tempo.
    //
    // Custo: ~620 MB por instrumento (880 zonas). Contra 1,9 GB do modo RAM,
    // continua valendo -- cinco canais cheios dao ~3,1 GB, cabem nos 8,6 GB
    // livres. Ser economico demais aqui foi trocar memoria por clique.
    static constexpr double preloadSeconds = 2.0;

    // RESERVA PARA A POLIFONIA (-9 dB).
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
    // -9 dB deixa espaco para ~3 notas somarem sem cortar. Quem quiser mais
    // volume sobe o master -- levantar o que nao estourou e possivel; recuperar
    // o que foi cortado, nao.
    static constexpr float polyphonyHeadroom = 0.355f;   // -9 dB

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
        if (n.contains ("organ") || n.contains ("orgao") || n.contains ("b3")) return "ORGAOS";
        if (n.contains ("string") || n.contains ("corda"))  return "STRINGS";
        if (n.contains ("pad"))                             return "PADS";
        if (n.contains ("lead") || n.contains ("synth"))    return "LEADS & SYNTHS";
        if (n.contains ("digital") || n.contains ("eletric") || n.contains ("electric")
             || n.contains ("rhodes") || n.contains ("tines") || n.contains ("keys") || n.startsWith ("ep")) return "EP & TECLAS";
        if (n.contains ("piano") || n.contains ("grand") || n.contains ("upright")) return "PIANOS";

        return {};
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

    // escaneia TODAS as bibliotecas nativas, marcando a origem de cada timbre;
    // "abort" (opcional) cancela entre pastas -- usado no shutdown do processador
    static juce::Array<TimbreEntry> scanAllLibraries (const std::atomic<bool>* abort = nullptr,
                                                      bool allowCache = true)
    {
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
        for (int l = 0; l < numLayers; ++l)
        {
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
            addParameter (layers[l].lowcut = new juce::AudioParameterFloat (
                { "lcut" + juce::String (l), 1 }, "Canal " + n + " Low Cut", 0.0f, 1.0f, 0.0f));
            addParameter (layers[l].highcut = new juce::AudioParameterFloat (
                { "hcut" + juce::String (l), 1 }, "Canal " + n + " High Cut", 0.0f, 1.0f, 0.0f));
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

        // varre a biblioteca em segundo plano ja na criacao: abrir o painel
        // depois disso e instantaneo (a varredura sincrona travava o audio/UI)
        startEntriesScanAsync();
    }

    void setExternalMaster (MasterProcessor* m) noexcept    { externalMaster = m; }
    MasterProcessor* getExternalMaster() const noexcept     { return externalMaster; }

    // recursos da DAW para hospedar plugins dentro do canal master
    void setPluginResources (juce::KnownPluginList* list, juce::AudioPluginFormatManager* fm) noexcept
    {
        knownPluginsRef = list;
        formatManagerRef = fm;
    }

    juce::KnownPluginList* getKnownPlugins() const noexcept { return knownPluginsRef; }

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

    // slot do reverb = primeiro insert cujo plugin se chame BigSky
    int getReverbSlot() const
    {
        for (int i = 0; i < numInserts; ++i)
            if (getInsertName (i).containsIgnoreCase ("bigsky"))
                return i;

        return -1;
    }

    int getReverbMachine() const
    {
        const int slot = getReverbSlot();

        if (slot < 0)
            return -1;

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

        if (slot < 0 || ! juce::isPositiveAndBelow (idx, numReverbMachines))
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

    // carrega um plugin de efeito dentro do Church Live (thread de mensagens)
    bool loadInsert (int slot, const juce::PluginDescription& desc)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts) || formatManagerRef == nullptr)
            return false;

        juce::String error;
        auto instance = formatManagerRef->createPluginInstance (desc, hostSampleRate,
                                                                juce::jmax (64, lastBlockSize), error);

        if (instance == nullptr)
            return false;

        instance->prepareToPlay (hostSampleRate, juce::jmax (64, lastBlockSize));

        suspendProcessing (true);
        inserts[slot] = std::move (instance);

        // Reverb como ENVIO exige o plugin 100% molhado: o seco ja vem do caminho
        // direto do canal. Com o MIX no meio, o seco voltaria dobrado.
        if (getInsertName (slot).containsIgnoreCase ("bigsky"))
            if (auto* mix = getInsertParamByName (slot, "MIX"))
            {
                mix->beginChangeGesture();
                mix->setValueNotifyingHost (1.0f);
                mix->endChangeGesture();
            }
        suspendProcessing (false);

        // carga ok: o estado guardado de uma restauracao falhada nao vale mais
        pendingInsertIds[slot].clear();
        pendingInsertStates[slot].clear();

        sendChangeMessage();
        return true;
    }

    void unloadInsert (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, numInserts))
            return;

        std::unique_ptr<juce::AudioPluginInstance> old;

        suspendProcessing (true);
        old = std::move (inserts[slot]);
        suspendProcessing (false);

        // slot esvaziado de proposito: estado pendente de restauracao cai junto
        pendingInsertIds[slot].clear();
        pendingInsertStates[slot].clear();

        old.reset();
        sendChangeMessage();
    }

    // corta todo o som na hora (botao PANIC)
    void panic()
    {
        suspendProcessing (true);
        stopAllVoices();
        suspendProcessing (false);

        // O PANIC so parava as vozes NATIVAS: um VSTi hospedado no canal (Kontakt)
        // seguia tocando e nao havia como cala-lo pela DAW. A mensagem tem de ser
        // entregue na thread de AUDIO, entao fica pendente ate o proximo bloco.
        panicPending.store (true);
    }

    // recalcula o EQ do canal (chamado ao trocar timbre ou ligar/desligar Auto EQ)
    void updateLayerEq (int l)
    {
        if (! juce::isPositiveAndBelow (l, numLayers))
            return;

        suspendProcessing (true);
        eqs[l].setCoeffs (eqPresetFor (layerCategory[l], hostSampleRate));
        eqs[l].reset();
        suspendProcessing (false);
    }

    ~TimbresProcessor() override
    {
        masterReference.clear();

        // liga o abort ANTES do join: os workers checam entre arquivos/pastas e
        // retornam cedo -- o join espera no maximo ~1 arquivo, nao o scan todo
        workerAbort->store (true);

        // join das threads de decode/scan: o processador morre no graph.clear()
        // com o MessageManager ainda vivo, entao nenhum callAsync fica orfao.
        // masterReference ja foi limpo acima, entao o que elas postarem e no-op.
        const juce::ScopedLock sl (workerThreadsLock);

        for (auto& w : workerThreads)
            if (w.thread.joinable())
                w.thread.join();
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
    juce::String getLayerTimbre (int l) const           { return juce::isPositiveAndBelow (l, numLayers) ? layers[l].timbre : juce::String(); }
    juce::String getLayerLibrary (int l) const          { return juce::isPositiveAndBelow (l, numLayers) ? layers[l].libraryName : juce::String(); }

    // Nome de exibicao: tira os sufixos tecnicos do arquivo e normaliza a
    // capitalizacao. Vive AQUI (e nao no editor) porque o visor de programa do
    // MainComponent precisa exatamente do mesmo nome -- estava mostrando o cru
    // ("Black Upright LP4 FC") enquanto a regua ja mostrava o limpo.
    static juce::String prettyLabel (const juce::String& label)
    {
        auto s = label.trim();

        for (bool changed = true; changed;)
        {
            changed = false;

            for (const char* suffix : { "LP4", "FC", "NS4", "TC" })
            {
                const juce::String suf (suffix);

                if (s.endsWithIgnoreCase (suf) && s.length() > suf.length() + 1)
                {
                    s = s.dropLastCharacters (suf.length()).trim();
                    changed = true;
                }
            }
        }

        s = s.replaceCharacter ('_', ' ').replaceCharacter ('-', ' ');

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

        juce::StringArray tokens;
        tokens.addTokens (s, " ", {});
        tokens.removeEmptyStrings();

        // siglas de verdade ficam intactas; o resto vira Title Case
        static const char* const acronyms[] = { "XL", "DX7", "CP70", "EP9", "B3",
                                                "MK1", "MK2", "TX", "JX", "II", "III", "IV" };

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
    bool isLoading() const noexcept                     { return loadingCount.load() > 0; }

    // Tem trabalho pendente? Cobre as TRES fases: escaneamento da biblioteca,
    // timbres na fila esperando esse escaneamento (requestLayerLoad guarda em
    // pendingLayerLabels quando a lista ainda nao existe) e samples decodificando.
    // A splash usa isto pra so entregar a DAW quando os canais estao mesmo prontos.
    bool isBusyLoading() const noexcept
    {
        if (loadingCount.load() > 0)
            return true;

        for (int l = 0; l < numLayers; ++l)
            if (pendingLayerLabels[l].isNotEmpty())
                return true;

        return ! entriesScanned;
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
    juce::AudioProcessorParameter* getLayerVolParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].vol : nullptr;
    }

    juce::AudioProcessorParameter* getLayerCompParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].comp : nullptr;
    }

    juce::AudioProcessorParameter* getLayerOnParam (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].on : nullptr;
    }

    bool getLayerOn (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) && layers[l].on != nullptr && layers[l].on->get();
    }

    juce::AudioProcessorParameter* getGlobalTranspParam() const { return globalTransp; }

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

        startWorkerThread ([weakThis, gen, abort, allowCache]
        {
            auto scanned = std::make_shared<juce::Array<TimbreEntry>> (
                scanAllLibraries (abort.get(), allowCache));

            juce::MessageManager::callAsync ([weakThis, scanned, gen]
            {
                if (auto* self = weakThis.get())
                    if (gen == self->scanGeneration)   // escaneamento velho e descartado
                        self->finishEntriesScan (std::move (*scanned));
            });
        });
    }

    void rescanLibrary()
    {
        entriesScanned = false;
        entriesCache.clear();
        startEntriesScanAsync (false);
        sendChangeMessage();
    }

    bool hasScannedLibrary() const noexcept     { return entriesScanned; }

    void clearLayer (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        // canal limpo invalida decode em voo e cargas agendadas deste canal
        ++layerLoadGeneration[layerIndex];
        pendingLayerLabels[layerIndex].clear();
        pendingLayerLibs[layerIndex].clear();
        loadingLabels[layerIndex].clear();
        loadingLibs[layerIndex].clear();

        suspendProcessing (true);

        auto old = std::move (layers[layerIndex].zones);

        for (auto& v : voices)
            if (v.active && v.layerIndex == layerIndex)
            {
                v.released = true;
                v.heldByPedal = false;
                v.adsr.noteOff();   // fade suave em vez de corte
            }

        suspendProcessing (false);

        if (! old.empty())
        {
            retiredZones.push_back (std::move (old));
            schedulePurge();
        }

        layers[layerIndex].timbre.clear();
        layers[layerIndex].libraryName.clear();
        sendChangeMessage();
    }

    // ---- VSTi externo (Kontakt etc.) hospedado no canal ----
    juce::AudioPluginInstance* getLayerInstrument (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].instrument.get() : nullptr;
    }

    juce::String getLayerInstrumentName (int l) const
    {
        return juce::isPositiveAndBelow (l, numLayers) ? layers[l].instrumentName : juce::String();
    }

    // carrega um instrumento VST no canal (thread de mensagens)
    bool loadInstrumentPlugin (int layerIndex, const juce::PluginDescription& desc)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers) || formatManagerRef == nullptr)
            return false;

        juce::String error;
        auto instance = formatManagerRef->createPluginInstance (desc, hostSampleRate,
                                                                juce::jmax (64, lastBlockSize), error);

        if (instance == nullptr)
            return false;

        // DESLIGA os barramentos de saida extras ANTES de preparar. O Kontakt abre
        // com varios pares de saida por padrao: sem isto ele RENDERIZA todos eles a
        // cada bloco e nos jogamos fora tudo menos o primeiro par. Era a maior
        // conta do motor — o mesmo Kontakt sozinho, com 2 canais, roda folgado.
        if (instance->getBusCount (false) > 1)
        {
            auto layout = instance->getBusesLayout();

            for (int b = 1; b < layout.outputBuses.size(); ++b)
                layout.outputBuses.getReference (b) = juce::AudioChannelSet::disabled();

            instance->setBusesLayout (layout);   // se o plugin recusar, segue como estava
        }

        instance->prepareToPlay (hostSampleRate, juce::jmax (64, lastBlockSize));

        // limpa timbre nativo do canal (vira canal de plugin)
        clearLayer (layerIndex);

        suspendProcessing (true);
        layers[layerIndex].instrument = std::move (instance);
        layers[layerIndex].instrumentId = desc.createIdentifierString();
        layers[layerIndex].instrumentName = desc.name;
        suspendProcessing (false);

        // carga ok: o estado guardado de uma restauracao falhada nao vale mais
        pendingVstIds[layerIndex].clear();
        pendingVstStates[layerIndex].clear();

        sendChangeMessage();
        return true;
    }

    void clearInstrumentPlugin (int layerIndex)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        std::unique_ptr<juce::AudioPluginInstance> old;

        suspendProcessing (true);
        old = std::move (layers[layerIndex].instrument);
        layers[layerIndex].instrumentId.clear();
        layers[layerIndex].instrumentName.clear();
        suspendProcessing (false);

        // canal deixa de ser VSTi: estado pendente de restauracao cai junto
        pendingVstIds[layerIndex].clear();
        pendingVstStates[layerIndex].clear();

        old.reset();
        sendChangeMessage();
    }

    // decodifica os samples em segundo plano e troca na thread de mensagens
    void loadTimbreIntoLayer (int layerIndex, const TimbreEntry& entry)
    {
        if (! juce::isPositiveAndBelow (layerIndex, numLayers))
            return;

        clearInstrumentPlugin (layerIndex);   // timbre nativo substitui plugin no canal

        // geracao por canal: se o canal for limpo/trocado antes do decode acabar,
        // finishLayerLoad descarta o resultado velho
        const int gen = ++layerLoadGeneration[layerIndex];

        // rotulo em voo: um save durante o decode ainda grava este timbre
        loadingLabels[layerIndex] = entry.label;
        loadingLibs[layerIndex] = entry.library;

        ++loadingCount;
        sendChangeMessage();

        juce::WeakReference<TimbresProcessor> weakThis (this);
        const auto files = entry.files;
        const auto label = entry.label;
        const auto library = entry.library;
        const auto category = entry.category;
        auto abort = workerAbort;   // shared_ptr POR VALOR: sobrevive ao processador

        startWorkerThread ([weakThis, layerIndex, files, label, library, category, gen, abort]
        {
            const double startedAt = juce::Time::getMillisecondCounterHiRes();
            auto zones = std::make_shared<std::vector<std::unique_ptr<Zone>>> (decodeZones (files, abort.get()));
            juce::Logger::writeToLog ("BIBLIOTECA: timbre " + label + " pronto em "
                                      + juce::String ((juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0, 2)
                                      + " s (" + juce::String ((int) zones->size()) + " zonas)");

            juce::MessageManager::callAsync ([weakThis, layerIndex, zones, label, library, category, gen]
            {
                if (auto* self = weakThis.get())
                    self->finishLayerLoad (layerIndex, std::move (*zones), label, library, category, gen);
            });
        });
    }

    //==============================================================================
    const juce::String getName() const override { return "Church Live"; }

   #if JUCE_ANDROID
    // PRENDE A THREAD DE AUDIO NOS NUCLEOS GRANDES.
    //
    // O Android da prioridade de TEMPO REAL a ela (medido: politica SCHED_FIFO,
    // prio -3) e mesmo assim a coloca no NUCLEO 0. Em processador big.LITTLE os
    // primeiros nucleos sao os pequenos e economicos -- no Dimensity 8200 do
    // aparelho de teste, 0-3 sao A55 e 4-7 sao A78. Ou seja: o audio rodava no
    // nucleo mais fraco do celular enquanto os QUATRO potentes ficavam parados.
    // Dai o pipoco num aparelho que sobra maquina (underruns contados: 34).
    //
    // Descobrimos quais sao os grandes pela frequencia maxima que cada um
    // declara, em vez de fixar numeros: a divisao 0-3/4-7 vale para ESTE chip, e
    // um aparelho com outro arranjo receberia a escolha errada.
    static void pinAudioThreadToFastCores()
    {
        const int n = (int) sysconf (_SC_NPROCESSORS_CONF);

        if (n <= 1 || n > CPU_SETSIZE)
            return;

        std::vector<long> maxFreq ((size_t) n, 0);
        long best = 0;

        for (int c = 0; c < n; ++c)
        {
            const juce::File f ("/sys/devices/system/cpu/cpu" + juce::String (c)
                                 + "/cpufreq/cpuinfo_max_freq");

            if (f.existsAsFile())
                maxFreq[(size_t) c] = f.loadFileAsString().trim().getLargeIntValue();

            best = juce::jmax (best, maxFreq[(size_t) c]);
        }

        if (best <= 0)
            return;   // kernel nao expoe as frequencias: melhor nao mexer

        cpu_set_t set;
        CPU_ZERO (&set);
        int chosen = 0;

        // 85% do topo, nao apenas o maximo exato: em muitos chips o nucleo
        // "prime" e um pouco mais rapido que os outros grandes, e prender tudo
        // num nucleo so seria trocar um gargalo por outro pior.
        for (int c = 0; c < n; ++c)
            if (maxFreq[(size_t) c] * 100 >= best * 85)
            {
                CPU_SET (c, &set);
                ++chosen;
            }

        if (chosen > 0 && chosen < n)
            sched_setaffinity (0, sizeof (set), &set);
    }
   #endif

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        hostSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
        const int block = juce::jmax (64, samplesPerBlock);
        lastBlockSize = block;

        // pre-aloca o scratch MIDI do remap de velocity / inversao de pedal
        // (mesmo padrao do midiCopy: membro reusado, nada de local por bloco)
        midiScratch.ensureSize (2048);

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

        for (auto& b : layerBuffers)
            b.setSize (2, block, false, false, true);

        for (int l = 0; l < numLayers; ++l)
        {
            delayBufs[l].setSize (2, (int) (hostSampleRate * 0.6) + block, false, false, true);
            delayBufs[l].clear();
            delayWrite[l] = 0;
            hpState[l][0] = hpState[l][1] = 0.0f;
            lpState[l][0] = lpState[l][1] = 0.0f;

            eqs[l].prepare (hostSampleRate, block);
            eqs[l].setCoeffs (eqPresetFor (layerCategory[l], hostSampleRate));
        }

        for (auto& ins : inserts)
            if (ins != nullptr)
                ins->prepareToPlay (hostSampleRate, block);

        for (auto& lay : layers)
            if (lay.instrument != nullptr)
                lay.instrument->prepareToPlay (hostSampleRate, block);

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
        }
    }

    void releaseResources() override {}

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
        // Feito daqui porque e ESTA a thread que precisa ser presa, e ela e
        // criada pelo sistema -- nao ha outro ponto em que possamos falar "eu".
        // Uma vez por thread; se o dispositivo for reaberto, a thread nova
        // passa por aqui e se prende sozinha.
        thread_local bool pinned = false;

        if (! pinned)
        {
            pinned = true;
            pinAudioThreadToFastCores();
        }

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
            for (int ch = 1; ch <= 16; ++ch)
            {
                midiCopy.addEvent (juce::MidiMessage::controllerEvent (ch, 64, 0), 0);   // solta o pedal
                midiCopy.addEvent (juce::MidiMessage::allNotesOff (ch), 0);
                midiCopy.addEvent (juce::MidiMessage::allSoundOff (ch), 0);
            }

            for (auto& n : instNotesOn)
                n = 0;
        }

        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();

            if (m.isNoteOn())               noteOn (m.getChannel(), m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff())         noteOff (m.getChannel(), m.getNoteNumber());
            else if (m.isSustainPedalOn())  setPedal (true);    // inversao ja aplicada na fonte
            else if (m.isSustainPedalOff()) setPedal (false);
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                stopAllVoices();
        }

        midi.clear();

        // rede de seguranca anti-nota-presa: voz sem tecla pressionada, sem
        // pedal e sem release e orfa - solta na hora
        for (auto& v : voices)
            if (v.active && ! v.released && ! v.heldByPedal
                 && juce::isPositiveAndBelow (v.note, 128) && keyDown[v.note] == 0)
            {
                v.released = true;
                v.adsr.noteOff();
            }

        // O reverb virou EFEITO DE ENVIO: cada canal manda a sua dose para este
        // barramento, o BigSky processa so ele (100% molhado) e o resultado volta
        // somado ao seco. Antes ele era insert do master, entao o MIX era global.
        const int reverbSlot = getReverbSlot();
        reverbBusActive = (reverbSlot >= 0);

        if (reverbBusActive)
        {
            reverbBus.setSize (2, buffer.getNumSamples(), false, false, true);
            reverbBus.clear();
        }

        renderVoices (buffer);

        for (int i = 0; i < numInserts; ++i)
        {
            auto* ins = inserts[i].get();

            if (ins == nullptr)
                continue;

            if (i == reverbSlot)
            {
                // barramento de envio: processa e soma de volta
                ins->processBlock (reverbBus, midiCopy);

                for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
                    buffer.addFrom (ch, 0, reverbBus, ch, 0, buffer.getNumSamples());
            }
            else
            {
                ins->processBlock (buffer, midiCopy);   // demais efeitos seguem em serie
            }
        }

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
                }

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
                    + "  | FALHA-STREAM " + juce::String (streamMisses)
                    + (streamMisses > 0 ? ("  <<< CLIQUE (sem-janela " + juce::String (missReason[1])
                        + " / outra-nota " + juce::String (missReason[2])
                        + " / passou " + juce::String (missReason[3]) + ")") : juce::String())
                    + "  | vozes " + juce::String (activeVoiceCount()));

                blocksSeen = 0;
                blocksLate = 0;
                lateWakes = 0;
                peakLoad = 0.0;
                peakGap = 0.0;
                peakOut = 0.0f;
                streamMisses = 0;
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


    double getTailLengthSeconds() const override                { return 0.3; }
    bool acceptsMidi() const override                           { return true; }
    bool producesMidi() const override                          { return false; }
    bool hasEditor() const override                             { return true; }
    juce::AudioProcessorEditor* createEditor() override;        // definido apos o editor
    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::XmlElement xml ("TIMBRES2");
        xml.setAttribute ("root", rootFolder.getFullPathName());
        xml.setAttribute ("gain", (double) *gain);
        xml.setAttribute ("gtransp", (int) *globalTransp);
        xml.setAttribute ("gsus", (bool) *globalSustain);
        xml.setAttribute ("pedinv", (bool) *pedalInvert);
        xml.setAttribute ("velcurve", velCurve->getIndex());

        for (int i = 0; i < numInserts; ++i)
            if (inserts[i] != nullptr)
            {
                auto* insXml = xml.createNewChildElement ("INSERT");
                insXml->setAttribute ("slot", i);
                insXml->setAttribute ("id", inserts[i]->getPluginDescription().createIdentifierString());

                juce::MemoryBlock mb;
                inserts[i]->getStateInformation (mb);
                insXml->setAttribute ("state", mb.toBase64Encoding());
            }
            else if (pendingInsertIds[i].isNotEmpty())
            {
                // auto-cura: plugin que falhou no restore nao perde id/estado
                auto* insXml = xml.createNewChildElement ("INSERT");
                insXml->setAttribute ("slot", i);
                insXml->setAttribute ("id", pendingInsertIds[i]);
                insXml->setAttribute ("state", pendingInsertStates[i]);
            }

        for (int l = 0; l < numLayers; ++l)
        {
            auto* layerXml = xml.createNewChildElement ("LAYER");
            layerXml->setAttribute ("index", l);

            // salvar durante o boot: o timbre ainda nao chegou em layers[l] --
            // cai pro rotulo pendente (esperando o scan) ou pro em voo
            // (decodificando); senao fechar o app na splash perdia o setup
            juce::String timbre = layers[l].timbre;
            juce::String lib = layers[l].libraryName;

            if (timbre.isEmpty() && pendingLayerLabels[l].isNotEmpty())
            {
                timbre = pendingLayerLabels[l];
                lib = pendingLayerLibs[l];
            }
            else if (timbre.isEmpty() && loadingLabels[l].isNotEmpty())
            {
                timbre = loadingLabels[l];
                lib = loadingLibs[l];
            }

            layerXml->setAttribute ("timbre", timbre);
            layerXml->setAttribute ("lib", lib);
            layerXml->setAttribute ("vol", (double) *layers[l].vol);
            layerXml->setAttribute ("transp", (int) *layers[l].transp);
            layerXml->setAttribute ("loop", (bool) *layers[l].loop);
            layerXml->setAttribute ("mute", (bool) *layers[l].mute);
            layerXml->setAttribute ("solo", (bool) *layers[l].solo);
            layerXml->setAttribute ("on", (bool) *layers[l].on);
            layerXml->setAttribute ("comp", (double) *layers[l].comp);
            layerXml->setAttribute ("dly", (double) *layers[l].delay);
            layerXml->setAttribute ("lcut", (double) *layers[l].lowcut);
            layerXml->setAttribute ("hcut", (double) *layers[l].highcut);
            layerXml->setAttribute ("aeq", (bool) *layers[l].autoEq);
            layerXml->setAttribute ("rvb", (double) *layers[l].reverbSend);
            layerXml->setAttribute ("cor", layerColourIdx[l]);

            // VSTi externo (Kontakt) hospedado no canal
            if (layers[l].instrument != nullptr)
            {
                layerXml->setAttribute ("vstId", layers[l].instrumentId);

                juce::MemoryBlock mb;
                layers[l].instrument->getStateInformation (mb);
                layerXml->setAttribute ("vstState", mb.toBase64Encoding());
            }
            else if (pendingVstIds[l].isNotEmpty())
            {
                // auto-cura: VSTi que falhou no restore nao perde id/estado
                layerXml->setAttribute ("vstId", pendingVstIds[l]);
                layerXml->setAttribute ("vstState", pendingVstStates[l]);
            }
        }

        copyXmlToBinary (xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        auto xml = getXmlFromBinary (data, size);

        if (xml == nullptr)
            return;

        const juce::File root (xml->getStringAttribute ("root", defaultSamplesFolder().getFullPathName()));

        if (root.isDirectory())
        {
            rootFolder = root;
            entriesScanned = false;
        }

        // formato antigo (um unico timbre) vira o canal 1
        if (xml->hasTagName ("TIMBRES"))
        {
            *layers[0].transp = xml->getIntAttribute ("transpose", 0);
            *layers[0].vol = (float) xml->getDoubleAttribute ("gain", 1.0);
            *layers[0].loop = xml->getBoolAttribute ("loop", false);

            requestLayerLoad (0, xml->getStringAttribute ("timbre"));
            return;
        }

        if (! xml->hasTagName ("TIMBRES2"))
            return;

        *gain = (float) xml->getDoubleAttribute ("gain", 1.0);
        *globalTransp = xml->getIntAttribute ("gtransp", 0);
        *globalSustain = xml->getBoolAttribute ("gsus", false);
        *pedalInvert = xml->getBoolAttribute ("pedinv", false);
        *velCurve = xml->getIntAttribute ("velcurve", curveNatural);

        // estado novo substitui qualquer pendencia de restauracao anterior
        for (int i = 0; i < numInserts; ++i)
        {
            pendingInsertIds[i].clear();
            pendingInsertStates[i].clear();
        }

        for (int l = 0; l < numLayers; ++l)
        {
            pendingVstIds[l].clear();
            pendingVstStates[l].clear();
        }

        for (auto* insXml : xml->getChildIterator())
        {
            if (! insXml->hasTagName ("INSERT"))
                continue;

            const int slot = insXml->getIntAttribute ("slot", -1);

            if (! juce::isPositiveAndBelow (slot, numInserts))
                continue;

            bool restored = false;

            if (knownPluginsRef != nullptr)
                if (auto desc = knownPluginsRef->getTypeForIdentifierString (insXml->getStringAttribute ("id")))
                    if (loadInsert (slot, *desc))
                    {
                        juce::MemoryBlock mb;
                        mb.fromBase64Encoding (insXml->getStringAttribute ("state"));

                        if (mb.getSize() > 0 && inserts[slot] != nullptr)
                            inserts[slot]->setStateInformation (mb.getData(), (int) mb.getSize());

                        restored = true;
                    }

            // auto-cura: plugin ausente/falho guarda id+estado pro proximo save
            if (! restored)
            {
                pendingInsertIds[slot] = insXml->getStringAttribute ("id");
                pendingInsertStates[slot] = insXml->getStringAttribute ("state");
            }
        }

        for (auto* layerXml : xml->getChildIterator())
        {
            const int l = layerXml->getIntAttribute ("index", -1);

            if (! layerXml->hasTagName ("LAYER"))
                continue;

            if (! juce::isPositiveAndBelow (l, numLayers))
                continue;

            *layers[l].vol = (float) layerXml->getDoubleAttribute ("vol", 1.0);
            *layers[l].transp = layerXml->getIntAttribute ("transp", 0);
            *layers[l].loop = layerXml->getBoolAttribute ("loop", false);
            *layers[l].mute = layerXml->getBoolAttribute ("mute", false);
            *layers[l].solo = layerXml->getBoolAttribute ("solo", false);
            *layers[l].on = layerXml->getBoolAttribute ("on", true);
            *layers[l].comp = (float) layerXml->getDoubleAttribute ("comp", 0.0);
            *layers[l].delay = (float) layerXml->getDoubleAttribute ("dly", 0.0);
            *layers[l].lowcut = (float) layerXml->getDoubleAttribute ("lcut", 0.0);
            *layers[l].highcut = (float) layerXml->getDoubleAttribute ("hcut", 0.0);
            *layers[l].autoEq = layerXml->getBoolAttribute ("aeq", false);
            *layers[l].reverbSend = (float) layerXml->getDoubleAttribute ("rvb", 0.0);
            layerColourIdx[l] = layerXml->getIntAttribute ("cor", -1);   // -1 = automatico

            // VSTi externo (Kontakt) do canal, ou timbre nativo
            const auto vstId = layerXml->getStringAttribute ("vstId");

            if (vstId.isNotEmpty())
            {
                bool restored = false;

                if (knownPluginsRef != nullptr)
                    if (auto d = knownPluginsRef->getTypeForIdentifierString (vstId))
                        if (loadInstrumentPlugin (l, *d) && layers[l].instrument != nullptr)
                        {
                            juce::MemoryBlock mb;
                            mb.fromBase64Encoding (layerXml->getStringAttribute ("vstState"));

                            if (mb.getSize() > 0)
                                layers[l].instrument->setStateInformation (mb.getData(), (int) mb.getSize());

                            restored = true;
                        }

                // auto-cura: VSTi ausente/falho guarda id+estado pro proximo save
                if (! restored)
                {
                    pendingVstIds[l] = vstId;
                    pendingVstStates[l] = layerXml->getStringAttribute ("vstState");
                }
            }
            else
            {
                requestLayerLoad (l, layerXml->getStringAttribute ("timbre"), layerXml->getStringAttribute ("lib"));
            }
        }
    }

private:
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
        SampleStreamer::Source stream;
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
        juce::String instrumentId, instrumentName;
        juce::AudioParameterFloat* vol = nullptr;
        juce::AudioParameterInt* transp = nullptr;
        juce::AudioParameterBool* loop = nullptr;
        juce::AudioParameterBool* mute = nullptr;
        juce::AudioParameterBool* solo = nullptr;
        juce::AudioParameterBool* on = nullptr;      // liga/desliga o canal
        juce::AudioParameterFloat* comp = nullptr;     // intensidade do compressor
        juce::AudioParameterFloat* delay = nullptr;    // mix do delay do canal
        juce::AudioParameterFloat* lowcut = nullptr;   // corta graves (HPF)
        juce::AudioParameterFloat* highcut = nullptr;  // corta agudos (LPF)
        juce::AudioParameterBool* autoEq = nullptr;    // EQ automatico por tipo (worship)
        juce::AudioParameterFloat* reverbSend = nullptr;   // dose deste canal no BigSky
    };

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

        juce::Array<int> vels;

        for (const auto& z : zs)
            vels.addIfNotAlreadyThere (z->velHi);

        // Uma camada so: o material nao tem dinamica gravada. Mantem a curva
        // historica BIT A BIT -- e o caso de todo o naipe de orgao e de lead.
        if (vels.size() <= 1)
            return;

        vels.sort();

        // menor limite superior de velocity = a camada mais fraca do conjunto
        const int velBottom = juce::jlimit (1, 126, vels.getFirst());
        const int velTop    = vels.getLast();

        // dispersao por nota: nivel da camada de topo menos o da camada de baixo
        juce::Array<float> spread;

        for (const auto& a : zs)
        {
            if (a->velHi != velBottom || a->rmsDb <= -99.0f)
                continue;

            for (const auto& b : zs)
                if (b->velHi == velTop && b->rootNote == a->rootNote && b->rmsDb > -99.0f)
                {
                    spread.add (b->rmsDb - a->rmsDb);
                    break;
                }
        }

        if (spread.isEmpty())
            return;   // sem medicao confiavel: nao arrisca, fica como estava

        spread.sort();
        const float sDb = spread[spread.size() / 2];   // mediana

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
    static std::vector<std::unique_ptr<Zone>> decodeZones (const juce::Array<juce::File>& files,
                                                           const std::atomic<bool>* abort = nullptr)
    {
       #if CHURCHDAW_STREAM_SAMPLES
        const int hw = (int) std::thread::hardware_concurrency();
        const int workerCount = juce::jlimit (1, 4, juce::jmin (files.size(), juce::jmax (1, hw / 2)));

        if (workerCount > 1 && files.size() >= 64)
        {
            std::vector<juce::Array<juce::File>> chunks ((size_t) workerCount);
            std::vector<std::vector<std::unique_ptr<Zone>>> partial ((size_t) workerCount);
            std::vector<std::thread> workers;
            workers.reserve ((size_t) workerCount);

            // Blocos contiguos: ao reunir worker 0, 1, 2... a ordem original
            // dos arquivos permanece intacta. Isto importa quando um pack traz
            // duas zonas com a mesma nota/velocity e findZone escolhe a primeira.
            for (int i = 0; i < files.size(); ++i)
            {
                const int w = juce::jmin (workerCount - 1,
                                          (int) (((juce::int64) i * workerCount) / files.size()));
                chunks[(size_t) w].add (files.getReference (i));
            }

            for (int w = 0; w < workerCount; ++w)
                workers.emplace_back ([&, w]
                {
                    partial[(size_t) w] = decodeZonesSerial (chunks[(size_t) w], abort);
                });

            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();

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

        return decodeZonesSerial (files, abort);
    }

    static std::vector<std::unique_ptr<Zone>> decodeZonesSerial (const juce::Array<juce::File>& files,
                                                                 const std::atomic<bool>* abort = nullptr)
    {
        juce::AudioFormatManager afm;
        afm.registerBasicFormats();

        std::vector<std::unique_ptr<Zone>> zones;

        for (const auto& f : files)
        {
            // cancelamento por arquivo: o join do destrutor espera no maximo o
            // decode do arquivo atual, nao o timbre inteiro
            if (abort != nullptr && abort->load())
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
            z->stream.file        = f;
            z->stream.numChannels = chans;
            z->stream.fileOffset  = skip;
            z->stream.totalSamples = z->streamTotal;
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
                z->stream.fileOffset = 0;
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

        computeRanges (zones);
        return zones;
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
        juce::Array<int> vels;

        for (auto& z : zs)
            vels.addIfNotAlreadyThere (z->velHi);

        vels.sort();

        for (auto& z : zs)
        {
            const int idx = vels.indexOf (z->velHi);
            z->velLo = idx <= 0 ? 0 : vels[idx - 1] + 1;
        }

        for (int layer = 0; layer < vels.size(); ++layer)
        {
            juce::Array<Zone*> ofLayer;

            for (auto& z : zs)
                if (z->velHi == vels[layer])
                    ofLayer.add (z.get());

            std::sort (ofLayer.begin(), ofLayer.end(),
                       [] (const Zone* a, const Zone* b) { return a->rootNote < b->rootNote; });

            for (int i = 0; i < ofLayer.size(); ++i)
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
            if (! vels.isEmpty() && z->velHi == vels.getLast())
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

    void finishLayerLoad (int layerIndex, std::vector<std::unique_ptr<Zone>> zones, const juce::String& label,
                          const juce::String& library, const juce::String& category, int gen)
    {
        --loadingCount;   // SEMPRE, mesmo descartando -- senao a splash trava

        // geracao mudou: o canal foi limpo, trocado ou virou VSTi durante o
        // decode -- o resultado velho nao pode atropelar o estado atual
        if (gen != layerLoadGeneration[layerIndex].load())
        {
            sendChangeMessage();
            return;
        }

        // Decode VAZIO (arquivos sumiram / drive fora): NAO limpa loadingLabels/
        // Libs -- o save continua re-emitindo o rotulo e o proximo scan/restart
        // cura sozinho. So o sucesso promove e limpa, logo abaixo.
        if (! zones.empty())
        {
            // rotulo em voo cumpriu o papel: promovido para o canal
            loadingLabels[layerIndex].clear();
            loadingLibs[layerIndex].clear();

            suspendProcessing (true);

            auto old = std::move (layers[layerIndex].zones);
            layers[layerIndex].zones = std::move (zones);

            // curva de velocity calibrada para ESTE timbre, montada com o
            // processamento suspenso (a thread de audio le esta tabela)
            buildVelocityLut (layers[layerIndex].zones, layers[layerIndex].velLut);

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
                         && juce::isPositiveAndBelow (v.note, 128) && keyDown[v.note] != 0
                         && numHeld < 64)
                    {
                        // canal do retrigger: o DA PROPRIA VOZ se a tecla dele
                        // segue pressionada (senao duas vozes da mesma nota em
                        // canais diferentes renasceriam todas no canal mais
                        // baixo e o note-off de um teclado calaria o outro);
                        // bit mais baixo da bitmask so como fallback
                        int ch = v.channel;

                        if (! (v.channel >= 1 && v.channel <= 16
                                && (keyDown[v.note] & (1u << ((v.channel - 1) & 15))) != 0))
                        {
                            ch = 1;

                            for (juce::uint16 bits = keyDown[v.note]; (bits & 1u) == 0 && ch < 16; bits = (juce::uint16) (bits >> 1))
                                ++ch;
                        }

                        heldNotes[numHeld] = v.note;
                        heldVels[numHeld] = v.velocity;
                        heldChans[numHeld] = ch;
                        ++numHeld;
                    }

                    v.released = true;
                    v.heldByPedal = false;
                    v.adsr.noteOff();
                }

            for (int i = 0; i < numHeld; ++i)
                startVoiceForLayer (layerIndex, heldNotes[i], heldVels[i], heldChans[i]);

            suspendProcessing (false);

            if (! old.empty())
            {
                retiredZones.push_back (std::move (old));
                schedulePurge();
            }

            layers[layerIndex].timbre = label;
            layers[layerIndex].libraryName = library;

            // guarda a categoria do timbre e recalcula o EQ (para o Auto EQ)
            juce::String cat = category;

            if (cat.isEmpty())
                for (const auto& e : entriesCache)
                    if (e.label == label)
                    {
                        cat = e.category;
                        break;
                    }

            layerCategory[layerIndex] = cat;
            updateLayerEq (layerIndex);
        }

        updateHostDisplay();
        sendChangeMessage();
    }

    void schedulePurge()
    {
        juce::WeakReference<TimbresProcessor> weakThis (this);

        juce::Timer::callAfterDelay (1500, [weakThis]
        {
            if (auto* self = weakThis.get())
                self->purgeRetiredZones();
        });
    }

    // libera os samples aposentados quando nenhuma voz os usa mais
    void purgeRetiredZones()
    {
        if (retiredZones.empty())
            return;

        suspendProcessing (true);

        for (auto it = retiredZones.begin(); it != retiredZones.end();)
        {
            bool inUse = false;

            for (auto& v : voices)
            {
                if (! v.active)
                    continue;

                for (auto& z : *it)
                    if (v.zone == z.get())
                    {
                        inUse = true;
                        break;
                    }

                if (inUse)
                    break;
            }

            it = inUse ? std::next (it) : retiredZones.erase (it);
        }

        suspendProcessing (false);

        if (! retiredZones.empty())
            schedulePurge();
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
                loadTimbreIntoLayer (l, *picks[l]);
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

        // fallback: so o rotulo
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
            pendingLayerLabels[layerIndex] = label;
            pendingLayerLibs[layerIndex] = library;
        }
    }

    void finishEntriesScan (juce::Array<TimbreEntry> scanned)
    {
        entriesCache = std::move (scanned);
        entriesScanned = true;

        for (int l = 0; l < numLayers; ++l)
            if (pendingLayerLabels[l].isNotEmpty())
            {
                // rotulo que o scan NAO encontrou sobrevive em loadingLabels/Libs
                // (loadLayerByLabel guarda la na falha e o save re-emite de la).
                // NUNCA pode ficar em pendingLayerLabels: isBusyLoading os checa
                // e a splash travaria esperando um load que nunca vem.
                loadLayerByLabel (l, pendingLayerLabels[l], pendingLayerLibs[l]);
                pendingLayerLabels[l].clear();
                pendingLayerLibs[l].clear();
            }

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
            static const char* wanted[numLayers] = { "Black Upright", "Pad2", "REV", "BELLP4", "Pad3" };

            const int layersToLoad = selfTestStress ? numLayers : 1;

            for (int l = 0; l < layersToLoad; ++l)
            {
                if (getLayerTimbre (l).isNotEmpty())
                    continue;

                for (const auto& e : entriesCache)
                    if (e.label.containsIgnoreCase (wanted[l]))
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
        if (usedInLayer >= layerVoices[layerIndex] && victimInLayer != nullptr)
            return victimInLayer;

        if (freeVoice != nullptr)
            return freeVoice;

        // pool global exausto (nao deveria acontecer): rouba a mais fraca de todas
        return victimAnywhere != nullptr ? victimAnywhere : &voices[0];
    }

    void startVoiceForLayer (int l, int note, int vel, int channel)
    {
        auto& layer = layers[l];

        if (layer.zones.empty())
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
        v->gain = layer.velLut[juce::jlimit (0, 127, vel)] * polyphonyHeadroom;
        v->note = note;
        v->channel = channel;
        v->velocity = vel;
        v->released = false;
        v->heldByPedal = false;
        v->startOrder = ++voiceCounter;
        v->adsr.setSampleRate (hostSampleRate);
        // release natural de piano: graves amortecem devagar, agudos rapido
        // (como os abafadores reais); antes era 0.25 s fixo e cortava seco
        const float rel = juce::jmap ((float) juce::jlimit (21, 108, t), 21.0f, 108.0f, 0.75f, 0.28f);

        // OBRIGATORIO antes do noteOn: juce::ADSR::noteOn() com attack > 0 apenas
        // troca de estado, NAO zera o valor do envelope (juce_ADSR.h:130-146).
        // Como quatro caminhos devolvem voz ao pool com o envelope sujo -- fim do
        // sample deixa 1,0 exato, alem de stopAllVoices, stopLayerVoices e o roubo
        // de voz -- a nota seguinte que caisse nesse slot comecava do valor
        // antigo e pulava a rampa de ataque. Resultado: transiente diferente de
        // nota para nota, sem padrao. Sem esta linha o ataque nao e deterministico.
        v->adsr.reset();
        v->adsr.setParameters ({ 0.003f, 0.0f, 1.0f, rel });
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
            st.restart (&zone->stream);
        }
        else
        {
            streamer.stream ((int) (v - voices)).stop();
        }
       #endif

        v->active = true;
    }

    void noteOn (int channel, int note, int vel)
    {
        if (juce::isPositiveAndBelow (note, 128))
        {
            keyDown[note] |= (juce::uint16) (1u << ((channel - 1) & 15));

            heldBits[note >> 5].fetch_or (1u << (note & 31), std::memory_order_relaxed);
        }

        for (int l = 0; l < numLayers; ++l)
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
                    v.released = true;
                    v.adsr.noteOff();
                }
            }
    }

    void releasePedalHeldVoices()
    {
        for (auto& v : voices)
            if (v.active && v.heldByPedal)
            {
                v.heldByPedal = false;
                v.released = true;
                v.adsr.noteOff();
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

        for (auto& k : keyDown)
            k = 0;

        for (auto& h : heldBits)
            h.store (0, std::memory_order_relaxed);

        for (auto& v : voices)
            silenceVoiceWithoutClick (v);
    }

    void stopLayerVoices (int layerIndex)
    {
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

        bool anySolo = false;

        for (const auto& l : layers)
            anySolo = anySolo || (bool) *l.solo;

        // buffers por canal (crescem junto com o bloco do driver - sem isso
        // um bloco maior que o preparado corrompia o audio)
        for (auto& a : layerHasAudio)
            a = false;

        if (layerBuffers[0].getNumSamples() < numSamples)
            for (auto& b : layerBuffers)
                b.setSize (2, numSamples, false, false, true);

        for (auto& b : layerBuffers)
            b.clear();

        // canais que hospedam um VSTi (Kontakt): renderiza o plugin com o MIDI
        // do bloco transposto pela transposicao do canal + global
        for (int l = 0; l < numLayers; ++l)
        {
            auto& lay = layers[l];

            if (lay.instrument == nullptr)
            {
                instNotesOn[l] = 0;
                continue;
            }

            const int semis = (int) *lay.transp + (int) *globalTransp;

            layerMidi.clear();

            for (const auto meta : midiCopy)
            {
                auto m = meta.getMessage();

                if (m.isNoteOn())
                {
                    const int n = m.getNoteNumber();
                    const int out = juce::jlimit (0, 127, n + semis);
                    noteTransp[l][n] = (juce::int8) (out - n);   // guarda o deslocamento REAL
                    m.setNoteNumber (out);
                    ++instNotesOn[l];
                }
                else if (m.isNoteOff())
                {
                    // Usa a transposicao do NOTE ON, nao a de agora: mexer no
                    // transpose com a nota presa fazia o note-off chegar noutra
                    // tecla, e a original ficava tocando pra sempre no Kontakt.
                    const int n = m.getNoteNumber();
                    m.setNoteNumber (juce::jlimit (0, 127, n + (int) noteTransp[l][n]));
                    instNotesOn[l] = juce::jmax (0, instNotesOn[l] - 1);
                }

                layerMidi.addEvent (m, meta.samplePosition);
            }

            // Rede de seguranca: sem tecla pressionada e sem pedal nao PODE sobrar
            // nota tocando. Se sobrou, algum note-off se perdeu (suspendProcessing
            // durante uma troca de timbre engole eventos do bloco) — calamos o
            // plugin em vez de deixar a nota presa no meio do culto.
            if (instNotesOn[l] > 0 && ! pedalDown && ! (bool) *globalSustain && ! anyKeyDown())
            {
                for (int ch = 1; ch <= 16; ++ch)
                    layerMidi.addEvent (juce::MidiMessage::allNotesOff (ch), juce::jmax (0, numSamples - 1));

                instNotesOn[l] = 0;
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
                    if (v.pos > (double) preloadLen - hostSampleRate * 0.5)
                    {
                        // A primeira janela tambem sobrepoe o fim do preload.
                        // Se ela comecar exatamente em preloadLen, o bloco que
                        // cruza essa fronteira nao cabe inteiro em nenhum dos
                        // dois buffers e vira um bloco de silencio audivel.
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

                    if (st.acquire ((juce::int64) v.pos, span, wL, wR, off))
                    {
                        srcL    = wL;
                        srcR    = wR;
                        srcBase = (int) ((juce::int64) v.pos - off);
                        srcSpan = SampleStreamer::windowSamples;
                        st.prefetch ((juce::int64) v.pos);
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

                        v.pos += (double) numSamples * v.ratio;

                        if (v.pos + 2.0 >= (double) len)
                            v.active = false;

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
                    v.active = false;

                continue;
            }

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

                const float amp = env * v.gain * endFade;

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

            // filtros do canal (LOW corta graves, HIGH corta agudos)
            const float lc = (float) *lay.lowcut;
            const float hc = (float) *lay.highcut;

            if (lc > 0.001f || hc > 0.001f)
            {
                const float twoPi = juce::MathConstants<float>::twoPi;
                const float aH = 1.0f - std::exp (-twoPi * (20.0f * std::pow (40.0f, lc)) / (float) hostSampleRate);
                const float aL = 1.0f - std::exp (-twoPi * (18000.0f / std::pow (22.5f, hc)) / (float) hostSampleRate);

                for (int ch = 0; ch < 2; ++ch)
                {
                    float* x = lb.getWritePointer (ch);
                    float hpS = hpState[l][ch];
                    float lpS = lpState[l][ch];

                    for (int s = 0; s < numSamples; ++s)
                    {
                        float v = x[s];

                        if (lc > 0.001f) { hpS += aH * (v - hpS); v -= hpS; }
                        if (hc > 0.001f) { lpS += aL * (v - lpS); v = lpS; }

                        x[s] = v;
                    }

                    hpState[l][ch] = hpS;
                    lpState[l][ch] = lpS;
                }
            }

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
            const float postGain = (audible ? (float) *lay.vol : 0.0f) * masterGain;

            float blockPeak = 0.0f;

            if (postGain > 0.0f)
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
                    buffer.addFrom (ch, 0, lb, ch, 0, numSamples, postGain);

                // ENVIO PARA O REVERB: o mesmo sinal pos-fader vai tambem para o
                // barramento do BigSky, na dose do knob REVERB do canal. E assim
                // que se consegue "reverb diferente por timbre" com UMA instancia
                // -- mexer no MIX do plugin mudaria para todos os canais de uma vez.
                const float send = (float) *lay.reverbSend;

                if (send > 0.0005f && reverbBusActive)
                    for (int ch = 0; ch < 2; ++ch)
                        reverbBus.addFrom (ch, 0, lb, ch, 0, numSamples, postGain * send);
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

    std::atomic<int> uiSelectedLayer { -1 };   // abre sem canal selecionado
    int layerColourIdx[numLayers] { -1, -1, -1, -1, -1 };   // -1 = segue a categoria
    std::atomic<juce::uint32> heldBits[4] {};   // teclas pressionadas (para o acorde)
    std::atomic<float> layerLevelsL[numLayers] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::atomic<float> layerLevelsR[numLayers] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::atomic<float> layerLevelShown[numLayers] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };   // espelho p/ o visor CANAIS

    // ---- notas presas nos VSTi hospedados (Kontakt) ----
    std::atomic<bool> panicPending { false };   // PANIC entregue no proximo bloco
    int instNotesOn[numLayers] {};              // note-ons ainda sem note-off
    juce::int8 noteTransp[numLayers][128] {};   // transposicao usada em cada note-on

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
    bool reverbBusActive = false;
    juce::AudioBuffer<float> layerBuffers[numLayers];
    juce::dsp::Compressor<float> comps[numLayers];
    LayerEq eqs[numLayers];
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
    juce::KnownPluginList* knownPluginsRef = nullptr;
    juce::AudioPluginFormatManager* formatManagerRef = nullptr;

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
    juce::String pendingVstIds[numLayers], pendingVstStates[numLayers];           // VSTi que falhou no restore (auto-cura no save)
    juce::String pendingInsertIds[numInserts], pendingInsertStates[numInserts];   // idem para os INSERTs
    bool showroomPending = false;   // primeira execucao: carregar setup de fabrica pos-scan
    std::atomic<int> loadingCount { 0 };

    // threads de decode/scan com DONO: detach + callAsync no shutdown postava
    // no MessageManager ja destruido; o destrutor faz join de todas
    struct WorkerThread
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::vector<WorkerThread> workerThreads;
    juce::CriticalSection workerThreadsLock;

    // sinal de cancelamento dos workers: o destrutor liga o flag e os loops de
    // scan/decode retornam entre um arquivo/pasta e outro, entao o join nunca
    // espera o trabalho inteiro. shared_ptr porque o lambda captura por valor
    // e pode sobreviver ao processador.
    std::shared_ptr<std::atomic<bool>> workerAbort = std::make_shared<std::atomic<bool>> (false);

    void startWorkerThread (std::function<void()> fn)
    {
        auto done = std::make_shared<std::atomic<bool>> (false);

        std::thread t ([fn = std::move (fn), done]
        {
            fn();
            done->store (true);
        });

        const juce::ScopedLock sl (workerThreadsLock);

        // colhe as que ja terminaram (join imediato) pra lista nao crescer
        for (auto it = workerThreads.begin(); it != workerThreads.end();)
        {
            if (it->done->load() && it->thread.joinable())
            {
                it->thread.join();
                it = workerThreads.erase (it);
            }
            else
                ++it;
        }

        workerThreads.push_back ({ std::move (t), std::move (done) });
    }

    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterInt* globalTransp = nullptr;
    juce::AudioParameterBool* globalSustain = nullptr;
    juce::AudioParameterBool* pedalInvert = nullptr;
    juce::AudioParameterChoice* velCurve = nullptr;
    bool prevGlobalSustain = false;
    MasterProcessor* externalMaster = nullptr;   // master da DAW (nao possuido)

    friend class TimbresEditor;

    JUCE_DECLARE_WEAK_REFERENCEABLE (TimbresProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbresProcessor)
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

        if (std::abs (newLevel - level) > 0.03f)
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

        // brilho que sangra pros lados quando o canal esta forte
        g.setColour (NordLookAndFeel::nordRedBright().withAlpha (level * 0.30f));
        g.fillRoundedRectangle (r.expanded (3.0f, 2.5f), 4.0f);

        g.setColour (NordLookAndFeel::nordRedBright().withAlpha (juce::jmin (1.0f, 0.25f + level * 1.5f)));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
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

        title.setText ("CHURCH LIVE", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (16.0f)).boldened());
        title.setColour (juce::Label::textColourId, NordLookAndFeel::nordRedBright());
        addAndMakeVisible (title);

        status.setJustificationType (juce::Justification::centredRight);
        status.setFont (juce::Font (juce::FontOptions (12.0f)));
        status.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (status);

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];

            s.name.setText ("CANAL " + juce::String (l + 1), juce::dontSendNotification);
            s.name.setJustificationType (juce::Justification::centred);
            // rotulo de secao no estilo silkscreen da Nord: caixa alta, pesado,
            // espacado. O canal em foco fica ACESO em vermelho, os outros apagados
            // -- e a mesma ideia da "rotulagem em LED" deles para palco escuro.
            s.name.setFont (NordLookAndFeel::stageLabel (11.0f));
            s.name.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
            addAndMakeVisible (s.name);

            // botao liga/desliga do canal
            s.onBtn.setClickingTogglesState (true);
            s.onBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedBright());
            s.onBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a3d44));   // OFF = cinza
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
            s.timbreName.setFont (NordLookAndFeel::stageLabel (11.0f));
            s.timbreName.setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
            s.timbreName.setMinimumHorizontalScale (0.7f);
            addAndMakeVisible (s.timbreName);

            // card vazio: botao grande "+ Adicionar instrumento" abre o seletor
            s.addButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            s.addButton.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::dimText());
            s.addButton.onClick = [this, l] { selectStrip (l); strips[l].combo.showPopup(); };
            addChildComponent (s.addButton);

            s.combo.setTextWhenNothingSelected ("+ Adicionar instrumento");
            // o menu padrao do JUCE nao tem busca; com centenas de timbres isso
            // virava rolagem infinita — clicar no combo abre o seletor com busca
            s.combo.onShowPopup = [this, l] { openPickerFor (l); };
            s.combo.onChange = [this, l]
            {
                if (updatingCombos)
                    return;

                selectStrip (l);

                const int id = strips[l].combo.getSelectedId();
                const auto& entries = proc.getEntriesNonBlocking();

                // a janela do VSTi guarda referencia ao AudioPluginInstance:
                // fecha ANTES de qualquer ramo destruir/trocar o plugin do canal
                layerPluginWindows[l].reset();

                if (id == 1)
                {
                    proc.clearLayer (l);
                    proc.clearInstrumentPlugin (l);
                }
                else if (id >= vstBaseId && id - vstBaseId < instrumentsCache.size())
                {
                    // VSTi externo (Kontakt) no canal
                    proc.loadInstrumentPlugin (l, instrumentsCache.getReference (id - vstBaseId));
                    *proc.layers[l].on = true;
                    *proc.layers[l].mute = false;
                    *proc.layers[l].solo = false;
                    openLayerPlugin (l);
                }
                else if (id >= 2 && id - 2 < entries.size())
                {
                    const auto& entry = entries.getReference (id - 2);
                    proc.loadTimbreIntoLayer (l, entry);

                    // escolher timbre sempre LIGA o canal (nada de mute/solo preso)
                    *proc.layers[l].on = true;
                    *proc.layers[l].mute = false;
                    *proc.layers[l].solo = false;

                    // preset worship automatico para pads e ambiencias
                    const auto cat = categoryFor (entry.label);

                    if (cat == "PADS" || cat == "AMBIENCIAS")
                    {
                        *proc.layers[l].reverbSend = 0.45f;   // pad ja entra molhado
                        *proc.layers[l].lowcut = 0.32f;
                        *proc.layers[l].comp = 0.12f;
                        *proc.layers[l].loop = true;
                    }
                }
            };
            // combo focado tambem responde as setas do PC (troca o timbre!) —
            // mesmo perigo de palco do slider focado
            s.combo.setWantsKeyboardFocus (false);
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

                strips[l].volDb.setText (silent ? juce::String ("-inf dB")
                                                : juce::String (db, 1) + " dB",
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
            s.volDb.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
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
            s.transp.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
            s.transp.setTextValueSuffix (" st");
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
            s.autoEqBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedBright());
            s.autoEqBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            // sem selectStrip aqui pelo mesmo motivo do botao ON: o attachment
            // dispara onClick ao restaurar o estado e roubava o foco na abertura
            s.autoEqBtn.onClick = [this, l] { proc.updateLayerEq (l); };
            s.autoEqAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].autoEq, s.autoEqBtn);
            addAndMakeVisible (s.autoEqBtn);

            s.muteBtn.setClickingTogglesState (true);
            // MUTE = vermelho da marca (spec: o vermelho e o acento de Mute);
            // SOLO = ambar. Cores distintas, sem laranja intermediario ambiguo.
            s.muteBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
            s.muteBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            s.muteAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].mute, s.muteBtn);
            addAndMakeVisible (s.muteBtn);

            s.soloBtn.setClickingTogglesState (true);
            s.soloBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::amberSolo());
            s.soloBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            s.soloAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.layers[l].solo, s.soloBtn);
            addAndMakeVisible (s.soloBtn);

            // COMP / REVERB / LOW / HIGH. O knob do meio era o DELAY de tempo fixo
            // do canal; virou o ENVIO ao BigSky, que e o que se ajusta de verdade
            // musica a musica. O delay continua no motor, so saiu do painel.
            for (auto* knob : { &s.compKnob, &s.dlyKnob, &s.lowKnob, &s.highKnob })
            {
                knob->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                knob->setWantsKeyboardFocus (false);
                knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                knob->setDoubleClickReturnValue (true, 0.0);   // duplo-clique volta ao neutro
                knob->onDragStart = [this, l] { selectStrip (l); };

                // POPUP DE VALOR durante o ajuste. Sem ele o knob e cego: da pra
                // ver que girou, nao PARA QUANTO foi. Em porcentagem, que e a
                // unidade real destes quatro (sao macros 0-100%, nao dB).
                knob->textFromValueFunction = [] (double v)
                {
                    return juce::String (juce::roundToInt (v * 100.0)) + "%";
                };
                knob->setPopupDisplayEnabled (true, false, this);

                addAndMakeVisible (*knob);
            }
            s.compKnob.setTooltip (juce::String::fromUTF8 ("COMP \xe2\x80\x94 compress\xc3\xa3o (punch/cola). Duplo-clique = 0"));
            s.dlyKnob .setTooltip (juce::String::fromUTF8 ("REVERB \xe2\x80\x94 quanto deste canal vai para o BigSky. Duplo-clique = 0"));
            s.lowKnob .setTooltip (juce::String::fromUTF8 ("LOW \xe2\x80\x94 corta graves (limpa a base). Duplo-clique = 0"));
            s.highKnob.setTooltip (juce::String::fromUTF8 ("HIGH \xe2\x80\x94 corta agudos (tira brilho). Duplo-clique = 0"));

            s.compAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].comp, s.compKnob);
            s.dlyAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].reverbSend, s.dlyKnob);
            s.lowAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].lowcut, s.lowKnob);
            s.highAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.layers[l].highcut, s.highKnob);

            s.compLabel.setText ("COMP", juce::dontSendNotification);
            s.dlyLabel.setText ("REVERB", juce::dontSendNotification);
            s.lowLabel.setText ("LOW", juce::dontSendNotification);
            s.highLabel.setText ("HIGH", juce::dontSendNotification);

            for (auto* lb : { &s.compLabel, &s.dlyLabel, &s.lowLabel, &s.highLabel })
            {
                lb->setJustificationType (juce::Justification::centred);
                lb->setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
                lb->setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
                addAndMakeVisible (*lb);
            }
        }

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
        masterDbSlider.setSkewFactorFromMidPoint (-12.0);
        masterDbSlider.setDoubleClickReturnValue (true, 0.0);
        // caixa de texto maior: o dB do master e o numero que o tecladista mais
        // consulta na mesa, e estava do mesmo tamanho dos dB de canal
        masterDbSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 22);
        masterDbSlider.setTextValueSuffix (" dB");
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
            addAndMakeVisible (*masterMeter);
        }

        // O botao mais critico ao vivo estava do mesmo tamanho e cor de tudo o
        // resto. Agora e o unico vermelho VIVO da regua e o maior alvo de clique
        // (os toggles ligados usam um vermelho escuro pra nao competir com ele).
        panicButton.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::nordRedBright());
        panicButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        panicButton.onClick = [this] { proc.panic(); };
        addAndMakeVisible (panicButton);

        gTranspSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        gTranspSlider.setWantsKeyboardFocus (false);   // setas do PC nao mexem no tom
        gTranspSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 15);
        gTranspAttachment = std::make_unique<juce::SliderParameterAttachment> (*proc.globalTransp, gTranspSlider);
        addAndMakeVisible (gTranspSlider);

        gTranspLabel.setText ("TRANSPOSE GLOBAL", juce::dontSendNotification);
        gTranspLabel.setJustificationType (juce::Justification::centred);
        gTranspLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
        gTranspLabel.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (gTranspLabel);

        // seletor nativo das 12 maquinas do BigSky
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

        sustainToggle.setButtonText ("Sustain");
        sustainAttachment = std::make_unique<juce::ButtonParameterAttachment> (*proc.globalSustain, sustainToggle);
        addAndMakeVisible (sustainToggle);

        invPedalToggle.setButtonText ("Inv. pedal");
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
        };
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
        };
        addAndMakeVisible (masterPunchBtn);

        // ---- painel inferior: plugins de efeito no canal master (BigSky etc.) ----
        fxTitle.setText ("MASTER FX", juce::dontSendNotification);
        fxTitle.setFont (juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.08f));
        fxTitle.setJustificationType (juce::Justification::centred);
        fxTitle.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());   // subtitulo, nao marca
        addAndMakeVisible (fxTitle);

        if (auto* kl = proc.getKnownPlugins())
            for (const auto& d : kl->getTypes())
            {
                if (! d.isInstrument)
                    effectsCache.add (d);
                else
                    instrumentsCache.add (d);   // Kontakt e outros VSTi para os canais
            }

        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            insertCombos[i].addItem ("Sem efeito", 1);

            for (int e = 0; e < effectsCache.size(); ++e)
                insertCombos[i].addItem (effectsCache.getReference (e).name, e + 2);

            insertCombos[i].setTextWhenNothingSelected ("Sem efeito");
            insertCombos[i].setWantsKeyboardFocus (false);   // setas do PC nao trocam o efeito
            insertCombos[i].onChange = [this, i]
            {
                if (updatingCombos)
                    return;

                insertWindows[i].reset();   // fecha o editor embutido antes de trocar

                const int id = insertCombos[i].getSelectedId();

                if (id == 1)
                    proc.unloadInsert (i);
                else if (id >= 2 && id - 2 < effectsCache.size())
                    proc.loadInsert (i, effectsCache.getReference (id - 2));
            };
            addAndMakeVisible (insertCombos[i]);

            insertOpenButtons[i].setButtonText ("Abrir");
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
        setSize (960, 622);
    }

    ~TimbresEditor() override
    {
        for (auto& w : layerPluginWindows)
            w.reset();

        if (auto* m = proc.getExternalMaster())
            m->removeChangeListener (this);

        proc.removeChangeListener (this);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
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
        // fundo com leve vinheta vertical (profundidade sutil) em vez de preto
        // chapado: faz os cards com sombra parecerem pousados numa superficie
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff181920), 0.0f, 0.0f,
                                                 juce::Colour (0xff101013), 0.0f, (float) getHeight(), false));
        g.fillRect (getLocalBounds());

        auto drawCard = [&g] (juce::Rectangle<int> r, bool selected, bool empty,
                              bool muted = false, bool dimmed = false, bool master = false,
                              juce::Colour accentColour = {}, bool soloed = false)
        {
            if (r.isEmpty())
                return;

            auto card = r.toFloat();
            const float radius = NordLookAndFeel::radiusCard;

            // sombra suave de elevacao (em vez do retangulo preto duro): faz o
            // card parecer uma placa fresada pousada sobre a mesa
            {
                juce::Path sp;
                sp.addRoundedRectangle (card, radius);
                juce::DropShadow (juce::Colours::black.withAlpha (0.45f), 12, { 0, 4 }).drawForPath (g, sp);
            }

            // corpo "vidro fosco": gradiente vertical iluminado no topo
            // (selecionado mais claro; vazio levemente levantado = "disponivel")
            juce::Colour top, bot;
            // o MASTER e um barramento, nao mais um canal: chapa um tom acima
            // pra ele se ler como "a saida" e nao como "o sexto canal"
            // GRAFITE NEUTRO, QUASE PRETO. Tentei duas vezes puxar a cor pro azul
            // do painel fisico da Nord e as duas perderam classe na tela: ardosia
            // media ficou chapada, grafite azulado ficou sujo. A estrutura da Nord
            // (LED, barra de seccao, escada de medidor) e o que importa -- a cor
            // preta neutra e o que faz a tela parecer caro.
            if      (master)   { top = juce::Colour (0xff2d3138); bot = juce::Colour (0xff1a1d23); }
            else if (selected) { top = juce::Colour (0xff2b2f37); bot = juce::Colour (0xff20232a); }
            else if (empty)    { top = juce::Colour (0xff1a1c21); bot = juce::Colour (0xff141519); }
            else               { top = juce::Colour (0xff21242b); bot = juce::Colour (0xff17191e); }

            g.setGradientFill (juce::ColourGradient (top, card.getX(), card.getY(),
                                                     bot, card.getX(), card.getBottom(), false));
            g.fillRoundedRectangle (card, radius);

            g.setColour (juce::Colours::white.withAlpha (0.05f));   // brilho de 1px no topo
            g.drawLine (card.getX() + 10.0f, card.getY() + 1.0f, card.getRight() - 10.0f, card.getY() + 1.0f, 1.0f);

            if (selected)
            {
                g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.25f));
                g.drawRoundedRectangle (card.expanded (2.5f), radius + 2.5f, 3.0f);
                g.setColour (NordLookAndFeel::nordRedBright());
                g.drawRoundedRectangle (card, radius, 2.0f);
            }
            else
            {
                g.setColour (master ? NordLookAndFeel::nordRed().withAlpha (0.40f)
                                    : NordLookAndFeel::panelOutline());
                g.drawRoundedRectangle (card, radius, master ? 1.4f : 1.0f);
            }

            // Barra de acento fina no topo. A chapa de titulo cheia que eu tinha
            // posto aqui (imitando "ORGAN SECTION") pesava o card e brigava com o
            // visor de programa; este risco deixa o card respirar.
            {
                // LINHA DE TOPO de 3px na cor da CATEGORIA -- e o codigo de cor que
                // identifica o instrumento de longe. Solo troca para ambar (o spec
                // proibe usar ambar no medidor, entao ele vive aqui).
                auto accent = card.reduced (12.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f)
                                  .withHeight (3.0f);

                if (master)
                    accent = accent.withHeight (4.0f);

                g.setColour (empty      ? NordLookAndFeel::txtDisabled().withAlpha (0.30f)
                            : soloed    ? NordLookAndFeel::amberSolo()
                            : master    ? NordLookAndFeel::nordRedBright()
                            : accentColour.isTransparent() ? NordLookAndFeel::nordRed()
                                                           : accentColour);
                g.fillRoundedRectangle (accent, 1.5f);
            }

            // Canal calado por SOLO de outro: escurecimento LEVE (spec pede
            // "ligeiramente escurecidos"). O 40% de antes apagava o canal.
            if (dimmed)
            {
                g.setColour (juce::Colours::black.withAlpha (0.22f));
                g.fillRoundedRectangle (card, radius);
            }

            // MUTE: card a ~70% de intensidade. Os controles continuam legiveis e
            // clicaveis -- mutado nao e desabilitado.
            if (muted)
            {
                g.setColour (NordLookAndFeel::bgApp().withAlpha (0.30f));
                g.fillRoundedRectangle (card, radius);
            }

            // MUTE: barra laranja na borda esquerda (le-se a distancia no palco)
            if (muted)
            {
                g.setColour (juce::Colour (0xffe06a1e));
                g.fillRoundedRectangle (card.getX() + 3.0f, card.getY() + 12.0f,
                                        4.0f, card.getHeight() - 24.0f, 2.0f);
            }
        };

        // ha algum canal em SOLO? entao os demais estao calados
        bool anySolo = false;
        for (int q = 0; q < TimbresProcessor::numLayers; ++q)
            if (proc.getLayerSolo (q)) { anySolo = true; break; }

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            const bool empty = proc.getLayerTimbre (l).isEmpty() && proc.getLayerInstrument (l) == nullptr;
            const bool muted = ! empty && proc.getLayerMute (l);
            const bool dimmed = ! empty && anySolo && ! proc.getLayerSolo (l) && ! muted;
            drawCard (strips[l].cardBounds, l == selectedStrip, empty, muted, dimmed, false,
                      empty ? juce::Colour() : categoryColourFor (l),
                      ! empty && proc.getLayerSolo (l));

            // CINTA DE AGRUPAMENTO em volta dos knobs, como as molduras "B3
            // PERCUSSION" / "VIB-CHORUS" da serigrafia da Nord: uma linha fina
            // interrompida no topo pelo rotulo. E o que separa "estes 4 controles
            // sao um bloco" de "estes 4 controles estao soltos no card".
            if (! empty && ! strips[l].fxGroup.isEmpty())
            {
                auto box = strips[l].fxGroup.toFloat().expanded (5.0f, 3.0f).withTrimmedBottom (2.0f);
                const juce::String tag ("EQ / FX");

                // No telefone a cinta fica imediatamente abaixo dos botoes M/S.
                // O titulo acima dela invadia essa fileira; uma moldura continua
                // comunica o agrupamento sem disputar os poucos pixels verticais.
                if (getHeight() < 380)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.13f));
                    g.drawRoundedRectangle (box, 4.0f, 1.0f);
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

            // canal vazio: UM convite so (circulo tracejado + botao logo abaixo,
            // como grupo, centrado). Antes o circulo ficava a 32% da altura e o
            // botao a ~55%: dois CTAs concorrentes com um buraco entre eles.
            if (empty)
            {
                const auto c = emptyRingCentre (strips[l].cardBounds);
                const float cx = (float) c.x, cy = (float) c.y, rr = (float) kEmptyRing;

                juce::Path circ;
                circ.addEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
                juce::Path dashed;
                const float dashes[] = { 4.0f, 4.0f };
                juce::PathStrokeType (1.5f).createDashedStroke (dashed, circ, dashes, 2);
                g.setColour (NordLookAndFeel::dimText().withAlpha (0.35f));
                g.fillPath (dashed);

                g.setColour (NordLookAndFeel::nordRed().withAlpha (0.6f));
                g.drawLine (cx - 11.0f, cy, cx + 11.0f, cy, 2.6f);
                g.drawLine (cx, cy - 11.0f, cx, cy + 11.0f, 2.6f);
            }
        }

        drawCard (masterCardBounds, false, false, false, false, true);
        drawCard (bottomPanelBounds, false, false);

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
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto top = area.removeFromTop (26);
        title.setBounds (top.removeFromLeft (180));
        status.setBounds (top);
        area.removeFromTop (6);

        // MASTER FX agora mora DENTRO da regua master (sem painel inferior) —
        // os canais ganham a altura toda
        bottomPanelBounds = {};

        const int stripW = area.getWidth() / (TimbresProcessor::numLayers + 1);
        const bool phoneLayout = getHeight() < 380;

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];
            auto col = area.removeFromLeft (stripW);
            s.cardBounds = col.reduced (5, 0);

            // fita de atividade EXATAMENTE onde o acento do card e desenhado
            s.activity.setBounds (s.cardBounds.getX() + 12, s.cardBounds.getY() + 6,
                                  juce::jmax (0, s.cardBounds.getWidth() - 24), 3);

            // canal carregado = timbre nativo OU plugin (Kontakt) hospedado
            const bool loaded = proc.getLayerTimbre (l).isNotEmpty()
                                 || proc.getLayerInstrument (l) != nullptr;

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
                s.lowLabel.setText  ("LO",  juce::dontSendNotification);
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
                const int cw = knobRow.getWidth() / 4;

                auto placePhoneKnob = [] (juce::Rectangle<int> cell,
                                          juce::Slider& knob, juce::Label& label)
                {
                    label.setBounds (cell.removeFromBottom (9));
                    const int d = juce::jmin (28, juce::jmin (cell.getWidth(), cell.getHeight()));
                    knob.setBounds (cell.withSizeKeepingCentre (d, d));
                };

                placePhoneKnob (knobRow.removeFromLeft (cw), s.compKnob, s.compLabel);
                placePhoneKnob (knobRow.removeFromLeft (cw), s.dlyKnob,  s.dlyLabel);
                placePhoneKnob (knobRow.removeFromLeft (cw), s.lowKnob,  s.lowLabel);
                placePhoneKnob (knobRow,                     s.highKnob, s.highLabel);

                auto lr = loopArea.withSizeKeepingCentre (juce::jmin (150, loopArea.getWidth()), 18);
                auto leftHalf = lr.removeFromLeft (lr.getWidth() / 2 - 2);
                lr.removeFromLeft (4);
                s.loop.setBounds (leftHalf);
                s.pluginOpenBtn.setBounds (leftHalf);
                s.autoEqBtn.setBounds (lr);

                s.transp.setBounds ({});
                s.transpLabel.setBounds ({});
                continue;
            }

            s.compLabel.setText ("COMP", juce::dontSendNotification);
            s.dlyLabel.setText  ("REVERB", juce::dontSendNotification);
            s.lowLabel.setText  ("LOW", juce::dontSendNotification);
            s.highLabel.setText ("HIGH", juce::dontSendNotification);
            s.loop.setButtonText ("Loop");
            s.autoEqBtn.setButtonText ("Auto EQ");

            inner.removeFromTop (6);

            auto headRow = inner.removeFromTop (16);
            s.onBtn.setBounds (headRow.removeFromRight (38).withSizeKeepingCentre (36, 16));
            s.name.setBounds (headRow);
            inner.removeFromTop (4);

            // [icone] Categoria (titulo forte) - icone maior e recortado justo
            auto nameRow = inner.removeFromTop (36);
            s.glyph.setBounds (nameRow.removeFromLeft (44).withSizeKeepingCentre (36, 36));
            nameRow.removeFromLeft (8);
            s.timbreName.setBounds (nameRow);
            inner.removeFromTop (4);
            s.combo.setBounds (inner.removeFromTop (24));
            inner.removeFromTop (8);

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

            auto dbRow = faderArea.removeFromBottom (18);
            s.meter->setBounds (faderArea.removeFromRight (NordLookAndFeel::meterChanW));
            faderArea.removeFromRight (4);
            s.vol.setBounds (faderArea);
            s.volDb.setBounds (dbRow.withTrimmedRight (26));

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

            // 4 knobs numa fileira so (COMP / DELAY / LOW / HIGH): mais denso,
            // libera altura vertical e evita o corte dos controles a 720p
            {
                // 52 -> 60: os ~56px devolvidos pelo transpose viraram knob maior
                // (o corpo estava travado em 40px e o marcador sumia de longe)
                auto knobRow = takeRow (60);

                s.fxGroup = knobRow;   // o paint desenha a cinta em volta desta area

                if (knobRow.isEmpty())
                {
                    for (auto* k : { &s.compKnob, &s.dlyKnob, &s.lowKnob, &s.highKnob })   k->setBounds ({});
                    for (auto* lb : { &s.compLabel, &s.dlyLabel, &s.lowLabel, &s.highLabel }) lb->setBounds ({});
                }
                else
                {
                    const int cw = knobRow.getWidth() / 4;
                    auto place1 = [] (juce::Rectangle<int> cell, juce::Slider& k, juce::Label& lb)
                    {
                        // rotulo com altura FIXA de 13 e o knob centrado no que
                        // sobra: antes 11px + centragem no resto davam linhas de
                        // base ligeiramente diferentes entre os quatro knobs
                        lb.setBounds (cell.removeFromBottom (13));
                        cell.removeFromBottom (2);
                        const int d = juce::jmin (46, juce::jmin (cell.getWidth(), cell.getHeight()));
                        k.setBounds (cell.withSizeKeepingCentre (d, d));
                    };

                    place1 (knobRow.removeFromLeft (cw), s.compKnob, s.compLabel);
                    place1 (knobRow.removeFromLeft (cw), s.dlyKnob,  s.dlyLabel);
                    place1 (knobRow.removeFromLeft (cw), s.lowKnob,  s.lowLabel);
                    place1 (knobRow,                     s.highKnob, s.highLabel);
                }
            }

            // TRANSPOSE por canal saiu: existe o global no master, e ao vivo quase
            // nao se usa o por-canal. Os ~56px que ele ocupava viraram respiro
            // entre os blocos, agora que todos os canais abrem completos.
            s.transp.setBounds ({});
            s.transpLabel.setBounds ({});
            takeRow (10);

            takeRow (2);
            auto loopArea = takeRow (20);

            if (loopArea.isEmpty())
            {
                s.loop.setBounds ({});
                s.autoEqBtn.setBounds ({});
                s.pluginOpenBtn.setBounds ({});
            }
            else
            {
                // metade esquerda = Loop (nativo) OU Abrir (plugin); direita = Auto EQ
                auto lr = loopArea.withSizeKeepingCentre (juce::jmin (150, loopArea.getWidth()), 20);
                auto leftHalf = lr.removeFromLeft (lr.getWidth() / 2 - 3);
                lr.removeFromLeft (6);

                s.loop.setBounds (leftHalf);
                s.pluginOpenBtn.setBounds (leftHalf);   // mesmo lugar do Loop (so um fica visivel)
                s.autoEqBtn.setBounds (lr);
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

        masterDbSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 22);
        sustainToggle.setButtonText ("Sustain");
        invPedalToggle.setButtonText ("Inv. pedal");
        masterAutoEqBtn.setButtonText ("Auto EQ Master");
        masterPunchBtn.setButtonText ("Punch / Peso");
        gTranspLabel.setText ("TRANSPOSE GLOBAL", juce::dontSendNotification);

        inner.removeFromTop (6);
        masterName.setBounds (inner.removeFromTop (20));
        inner.removeFromTop (4);

        // MASTER FX embutido na regua: escolhe o efeito (BigSky...) aqui mesmo
        fxTitle.setBounds (inner.removeFromTop (14));
        inner.removeFromTop (3);

        // Os dois slots de FX liam como uma lista continua (3px entre eles, mesmo
        // espacamento das linhas de dentro). 7px separa os slots como DOIS blocos,
        // e os "Abrir" ficam alinhados numa coluna de largura fixa.
        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            auto row = inner.removeFromTop (26);
            insertOpenButtons[i].setBounds (row.removeFromRight (52));
            row.removeFromRight (5);
            insertCombos[i].setBounds (row);

            if (i < TimbresProcessor::numInserts - 1)
                inner.removeFromTop (7);
        }

        // seletor de maquina de reverb, logo abaixo dos slots: so aparece quando
        // ha um BigSky carregado
        if (proc.getReverbSlot() >= 0)
        {
            inner.removeFromTop (6);
            reverbTypeCombo.setVisible (true);
            reverbTypeCombo.setBounds (inner.removeFromTop (24));
        }
        else
        {
            reverbTypeCombo.setVisible (false);
            reverbTypeCombo.setBounds ({});
        }

        // divisoria: fecha o bloco MASTER FX e abre o bloco de saida
        inner.removeFromTop (7);
        fxDivider = inner.removeFromTop (1);
        inner.removeFromTop (7);

        // -96 reservava fader demais e era o que empurrava o "Punch / Peso" pra
        // fora sempre que qualquer bloco acima crescia 10px. Com -78 o fader ainda
        // fica confortavel e a coluna de controles tem folga estavel.
        const int mBottomH = juce::jlimit (24, 210, inner.getHeight() - 58);
        auto bottom = inner.removeFromBottom (mBottomH);
        auto faderArea = inner;

        if (masterMeter != nullptr)
        {
            masterMeter->setBounds (faderArea.removeFromRight (30));
            faderArea.removeFromRight (6);
        }

        masterDbSlider.setBounds (faderArea);

        auto takeRow = [&bottom] (int h)
        {
            return bottom.getHeight() >= h ? bottom.removeFromTop (h)
                                           : juce::Rectangle<int>();
        };

        auto panicArea = takeRow (32);
        panicButton.setBounds (panicArea.isEmpty() ? juce::Rectangle<int>()
                                                   : panicArea.withSizeKeepingCentre (juce::jmin (132, panicArea.getWidth()), 30));
        takeRow (6);
        auto transpArea = takeRow (42);   // era 56: o knob nao precisava de tanta caixa

        if (transpArea.isEmpty())
        {
            gTranspSlider.setBounds ({});
            gTranspLabel.setBounds ({});
        }
        else
        {
            gTranspLabel.setBounds (transpArea.removeFromBottom (12));
            gTranspSlider.setBounds (transpArea);
        }

        // interruptores (nao mais checkbox): precisam do trilho + folga do texto.
        // MESMA largura nos dois, senao os trilhos nao alinham na vertical.
        // Frestas de 3px entre os quatro ultimos controles custavam 12px e faziam
        // o "Punch / Peso" cair fora sempre que o painel de programa crescia.
        // 2px separa igual e devolve a linha.
        takeRow (2);
        const int swW = juce::jmin (112, bottom.getWidth());
        auto susArea = takeRow (20);
        sustainToggle.setBounds (susArea.isEmpty() ? juce::Rectangle<int>()
                                                   : susArea.withSizeKeepingCentre (swW, 20));
        takeRow (2);
        auto invArea = takeRow (20);
        invPedalToggle.setBounds (invArea.isEmpty() ? juce::Rectangle<int>()
                                                    : invArea.withSizeKeepingCentre (swW, 20));
        takeRow (2);
        auto aeqArea = takeRow (22);
        masterAutoEqBtn.setBounds (aeqArea.isEmpty() ? juce::Rectangle<int>()
                                                     : aeqArea.withSizeKeepingCentre (juce::jmin (120, aeqArea.getWidth()), 22));
        takeRow (2);
        auto punchArea = takeRow (22);
        masterPunchBtn.setBounds (punchArea.isEmpty() ? juce::Rectangle<int>()
                                                      : punchArea.withSizeKeepingCentre (juce::jmin (120, punchArea.getWidth()), 22));
    }

private:
    // Abre o seletor com busca no lugar do menu do ComboBox. Clicar de novo (ou
    // Esc / X) fecha.
    void openPickerFor (int layer)
    {
        if (picker != nullptr)
        {
            // mesmo canal alterna (fecha); canal DIFERENTE troca o picker --
            // so fechar deixava o combo recem-clicado com "menu ativo" preso
            const bool sameLayer = (layer == pickerLayer);
            closePicker();

            if (sameLayer)
                return;
        }

        selectStrip (layer);
        pickerLayer = layer;

        auto* p = new TimbrePicker (pickerItems, strips[layer].combo.getSelectedId());
        picker.reset (p);

        p->drawIcon = [] (juce::Graphics& g, juce::Rectangle<float> r, int k)
        {
            TimbreGlyph::paintBadge (g, r, (TimbreGlyph::Kind) k);
        };

        p->onPick = [this, layer] (int id)
        {
            strips[layer].combo.setSelectedId (id, juce::sendNotificationSync);
            closePicker();
        };

        p->onCancel = [this] { closePicker(); };

        addAndMakeVisible (p);

        // No editor compacto do celular o picker de desktop (518 px logicos)
        // passava do rodape. A lista ate rolava, mas os ultimos itens e o rodape
        // ficavam fisicamente fora da tela. Escala uniforme mantem a hierarquia,
        // os alvos de toque e a busca, e transforma tambem o hit-testing.
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

        // foco no campo de busca so depois de montado na tela
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
    int pickerLayer = -1;

    // Geometria do convite do card vazio, compartilhada por paint() e resized()
    // pra o circulo tracejado e o botao nunca se separarem.
    static constexpr int kEmptyRing = 26;   // raio do circulo tracejado
    static constexpr int kEmptyGapY = 14;   // respiro entre o circulo e o botao
    static constexpr int kEmptyBtnH = 40;

    static juce::Point<int> emptyRingCentre (juce::Rectangle<int> card)
    {
        const int groupH = kEmptyRing * 2 + kEmptyGapY + kEmptyBtnH;
        const int top = card.getCentreY() - groupH / 2 + 8;   // um respiro abaixo do centro geometrico
        return { card.getCentreX(), top + kEmptyRing };
    }

    struct Strip
    {
        juce::Label name, timbreName, transpLabel, volDb;
        TimbreGlyph glyph;
        ActivityBar activity;   // acento do topo do card acende com o sinal
        PickerComboBox combo;   // abre o seletor com busca em vez do menu padrao
        juce::Slider vol, transp;
        juce::TextButton loop { "Loop" };
        juce::TextButton muteBtn { "M" }, soloBtn { "S" }, onBtn { "ON" };
        juce::TextButton autoEqBtn { "Auto EQ" };
        juce::TextButton pluginOpenBtn { "Abrir" };   // canal-plugin (abre a interface do VSTi)
        juce::TextButton addButton { "+ Adicionar instrumento" };   // card vazio
        juce::Slider compKnob, dlyKnob, lowKnob, highKnob;
        juce::Label compLabel, dlyLabel, lowLabel, highLabel;
        std::unique_ptr<LevelMeter> meter;
        float pendingL = 0.0f, pendingR = 0.0f;   // pico lido no quadro (L e R juntos)
        juce::Rectangle<int> cardBounds;
        juce::Rectangle<int> fxGroup;   // cinta de agrupamento em volta dos knobs
        bool lastLoaded = false;
        std::unique_ptr<juce::SliderParameterAttachment> volAttachment, transpAttachment,
                                                         compAttachment,
                                                         dlyAttachment, lowAttachment, highAttachment;
        std::unique_ptr<juce::ButtonParameterAttachment> loopAttachment, muteAttachment,
                                                         soloAttachment, onAttachment, autoEqAttachment;
    };

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (auto* m = proc.getExternalMaster(); m != nullptr && source == m)
        {
            syncingMaster = true;
            masterDbSlider.setValue (m->getMasterDb(), juce::dontSendNotification);
            syncingMaster = false;
            refreshMasterDbColour();
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

    void selectStrip (int l)
    {
        if (selectedStrip != l && juce::isPositiveAndBelow (l, TimbresProcessor::numLayers))
        {
            selectedStrip = l;
            proc.setSelectedLayer (l);   // o visor OLED do cabecalho segue este

            // "acende" o rotulo do canal em foco e apaga os outros
            for (int i = 0; i < TimbresProcessor::numLayers; ++i)
                strips[i].name.setColour (juce::Label::textColourId,
                                          i == l ? NordLookAndFeel::nordRedBright()
                                                 : NordLookAndFeel::dimText());

            repaint();

            // relayout DEFERIDO (move os detalhes p/ o canal em foco). callAsync
            // evita re-entrancia: selectStrip e chamado de dentro do handler de
            // mouse do proprio knob/combo — chamar resized() ali corrompe o
            // despacho do evento. SafePointer protege contra destruicao.
            juce::Component::SafePointer<TimbresEditor> safe (this);
            juce::MessageManager::callAsync ([safe] { if (auto* e = safe.getComponent()) e->resized(); });
        }
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

            insertWindows[i] = std::make_unique<InsertEditorCard> (*p, [safe, i]
            {
                juce::MessageManager::callAsync ([safe, i]
                {
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

        if (layerPluginWindows[l] != nullptr)
        {
            layerPluginWindows[l]->toFront (true);
            return;
        }

        if (auto* p = proc.getLayerInstrument (l))
        {
            auto safe = juce::Component::SafePointer<TimbresEditor> (this);

            layerPluginWindows[l] = std::make_unique<InsertPluginWindow> (*p, [safe, l]
            {
                juce::MessageManager::callAsync ([safe, l]
                {
                    if (auto* ed = safe.getComponent())
                        ed->layerPluginWindows[l].reset();
                });
            });
        }
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
            const auto& lib = entries.getReference (e).library;
            libOrder.addIfNotAlreadyThere (lib.isNotEmpty() ? lib : juce::String ("Biblioteca"));
        }

        // Monta a lista (id, nome, secao) UMA vez: e a mesma que alimenta os
        // combos de todos os canais E o seletor com busca.
        pickerItems.clearQuick();

        // O ComboBox interno sempre teve "(vazio)", mas o seletor com busca
        // substitui o popup padrao e nao copiava essa opcao. Resultado: depois
        // de carregar um timbre nao existia caminho de toque para remover o
        // instrumento do canal. Mantemos a acao no topo, separada dos sons.
        pickerItems.add ({ 1, "Canal vazio / remover instrumento", "CANAL",
                           (int) TimbreGlyph::kindForName ("vazio") });

        // VSTi externos (Kontakt etc.) primeiro - "de fora"
        for (int i = 0; i < instrumentsCache.size(); ++i)
        {
            const auto& name = instrumentsCache.getReference (i).name;
            pickerItems.add ({ vstBaseId + i, name, juce::String ("PLUGINS / KONTAKT"),
                               (int) TimbreGlyph::resolveKind (true, name, {}) });
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

                    const auto entryCat = entry.category.isNotEmpty() ? entry.category
                                                                      : categoryFor (entry.label);

                    if (entryCat != cat)
                        continue;

                    // kindForName (e nao resolveKind): na lista o que interessa e
                    // distinguir CATEGORIA. resolveKind transforma todo piano em
                    // badge Nord — o que deixaria metade da lista igual e ainda
                    // atribuiria a marca Nord a timbres que nao sao dela.
                    pickerItems.add ({ e + 2, displayName (entry.label), lib.toUpperCase(),
                                       (int) TimbreGlyph::kindForName (entry.label) });
                }
            }
        }

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
                if (it.id == 1)
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

    void refreshFromProcessor()
    {
        updatingCombos = true;

        const auto& entries = proc.getEntriesNonBlocking();

        if (knownEntryCount != entries.size())
            populateCombos();

        bool layoutChanged = false;

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            auto& s = strips[l];
            const auto timbre = proc.getLayerTimbre (l);
            const auto libName = proc.getLayerLibrary (l);
            const auto vstName = proc.getLayerInstrumentName (l);   // canal com Kontakt/VST?
            const bool isPlugin = vstName.isNotEmpty();
            const bool loaded = timbre.isNotEmpty() || isPlugin;
            int selected = 1;

            // plugin do canal morreu por fora (restore de estado, troca
            // interna): a janela dele nao pode sobreviver ao instance
            if (proc.getLayerInstrument (l) == nullptr)
                layerPluginWindows[l].reset();

            if (isPlugin)
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
                        selected = e + 2;
                        break;
                    }
            }

            s.combo.setSelectedId (selected, juce::dontSendNotification);
            s.glyph.setKind (TimbreGlyph::resolveKind (isPlugin, vstName, timbre), loaded);
            // titulo forte = tipo/categoria; seletor abaixo = preset/plugin
            s.timbreName.setText (isPlugin ? vstName : (loaded ? categoryTitleFor (timbre) : juce::String()),
                                  juce::dontSendNotification);

            // canais nativos tem knobs de canal-strip; canal-plugin mostra "Abrir"
            s.name.setVisible (true);   // "CANAL X" sempre visivel (cheio ou vazio)
            s.name.setColour (juce::Label::textColourId,
                              l == selectedStrip ? NordLookAndFeel::nordRedBright()
                                                 : NordLookAndFeel::dimText());

            // cor da categoria nos arcos dos knobs e no icone
            {
                const auto cc = categoryColourFor (l);

                for (auto* k : { &s.compKnob, &s.dlyKnob, &s.lowKnob, &s.highKnob })
                    k->setColour (juce::Slider::rotarySliderFillColourId, cc);
            }

            juce::Component* stripControls[] = { &s.glyph, &s.timbreName, &s.vol, &s.volDb, &s.combo,
                                                 &s.transp, &s.transpLabel,
                                                 &s.muteBtn, &s.soloBtn,
                                                 &s.compKnob, &s.compLabel,
                                                 &s.dlyKnob, &s.lowKnob, &s.highKnob,
                                                 &s.dlyLabel, &s.lowLabel, &s.highLabel,
                                                 &s.onBtn, &s.autoEqBtn };

            for (auto* c : stripControls)
                c->setVisible (loaded);

            s.loop.setVisible (loaded && ! isPlugin);
            s.pluginOpenBtn.setVisible (isPlugin);
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

        // plugins internos: sincroniza combos e botoes
        for (int i = 0; i < TimbresProcessor::numInserts; ++i)
        {
            int selected = 1;

            if (auto* p = proc.getInsert (i))
            {
                const auto id = p->getPluginDescription().createIdentifierString();

                for (int e = 0; e < effectsCache.size(); ++e)
                    if (effectsCache.getReference (e).createIdentifierString() == id)
                    {
                        selected = e + 2;
                        break;
                    }
            }

            insertCombos[i].setSelectedId (selected, juce::dontSendNotification);
            insertOpenButtons[i].setEnabled (proc.getInsert (i) != nullptr);
        }

        // segue a maquina que estiver de fato no plugin (o usuario pode ter
        // trocado pela interface do BigSky)
        {
            const int m = proc.getReverbMachine();
            reverbTypeCombo.setSelectedId (m >= 0 ? m + 1 : 0, juce::dontSendNotification);
        }

        updatingCombos = false;
        juce::ignoreUnused (layoutChanged);

        // sempre realinha o layout ao estado real do motor (nada de cache dessincronizado)
        resized();
        repaint();

        const bool loading    = proc.isLoading();
        const bool ready      = ! proc.getEntriesNonBlocking().isEmpty();
        const bool libMissing = ! ready && proc.hasScannedLibrary();

        status.setText (loading ? "Carregando timbre..."
                          : ready ? "Pronto"
                          : libMissing ? juce::String::fromUTF8 ("Biblioteca n\xc3\xa3o encontrada \xe2\x80\x94 instale o pack e reabra")
                                       : "Lendo biblioteca...",
                        juce::dontSendNotification);

        statusColour = loading ? juce::Colour (0xffe0a83a)      // ambar
                        : ready ? juce::Colour (0xff6fcf6f)      // verde
                        : libMissing ? juce::Colour (0xffd83a3a) // vermelho
                                     : juce::Colour (0xffe0a83a);
        repaint (0, 0, getWidth(), 46);
    }

    TimbresProcessor& proc;
    juce::TooltipWindow tooltipWindow { this, 650 };   // dicas ao passar o mouse
    Strip strips[TimbresProcessor::numLayers];
    juce::Label title, status, masterName, gTranspLabel, fxTitle;
    juce::Colour statusColour { 0xff6fcf6f };   // bolinha de status: verde=pronto, ambar=carregando, vermelho=erro
    juce::Slider masterDbSlider, gTranspSlider;
    juce::TextButton panicButton { "PANIC" };
    juce::ToggleButton sustainToggle, invPedalToggle;
    juce::TextButton masterAutoEqBtn, masterPunchBtn;
    std::unique_ptr<LevelMeter> masterMeter;
    juce::Rectangle<int> masterCardBounds, bottomPanelBounds;
    std::unique_ptr<juce::SliderParameterAttachment> gTranspAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> sustainAttachment, invPedalAttachment;

    juce::Array<juce::PluginDescription> effectsCache;
    juce::Array<juce::PluginDescription> instrumentsCache;   // Kontakt e VSTi para os canais
    std::unique_ptr<InsertPluginWindow> layerPluginWindows[TimbresProcessor::numLayers];
    static constexpr int vstBaseId = 500000;                 // ids do combo p/ VSTi
    juce::ComboBox insertCombos[TimbresProcessor::numInserts];
    juce::TextButton insertOpenButtons[TimbresProcessor::numInserts];
    std::unique_ptr<InsertEditorCard> insertWindows[TimbresProcessor::numInserts];   // editores EMBUTIDOS (estilo nativo)
    bool updatingCombos = false;
    bool syncingMaster = false;
    juce::ComboBox reverbTypeCombo;   // 12 maquinas do BigSky, sem abrir o plugin
    juce::Rectangle<int> fxDivider;   // linha que separa MASTER FX do bloco de saida
    int selectedStrip = -1;   // nenhum canal em foco ao abrir
    int knownEntryCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbresEditor)
};

inline juce::AudioProcessorEditor* TimbresProcessor::createEditor()
{
    return new TimbresEditor (*this);
}
