#pragma once

#include "AppPaths.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <vector>

//==============================================================================
// "Pad Contínuo" worship: toca um pad sustentado em loop no tom escolhido.
//
// Usa os 7 pads naturais em WAV (A..G, ~5 min cada) numa pasta; os 5 sustenidos
// (C#, D#, F#, G#, A#) saem afinando +1 semitom o natural de baixo. Troca de tom
// faz CROSSFADE suave (sem clique). Carregamento do WAV é assíncrono (thread de
// fundo) — o áudio nunca trava. É um nó do grafo que soma no master.
//==============================================================================
class PadProcessor : public juce::AudioProcessor,
                     private juce::Thread
{
public:
    PadProcessor()
        : juce::AudioProcessor (BusesProperties().withOutput ("Pad", juce::AudioChannelSet::stereo(), true)),
          juce::Thread ("PadLoader")
    {
        // PARAMETROS EXPOSTOS = MAPEAVEIS POR MIDI.
        //
        // O MIDI Learn resolve o alvo por getParameters() do processador
        // (ver MainComponent::applyCCTarget). Enquanto o Pad nao expunha
        // nenhum parametro, nao havia o que mapear — o musico so conseguia
        // mexer nele com o dedo na tela, no meio do culto.
        addParameter (padCutoff = new juce::AudioParameterFloat (
            juce::ParameterID { "padCutoff", 1 },
            "Pad Cutoff",
            juce::NormalisableRange<float> (0.0f, 1.0f),
            1.0f));

        // A FAIXA TEM DE SER 0..1,5, igual a do setGain e a do knob da tela.
        //
        // Com o parametro limitado a 0..1, tudo acima disso era descartado em
        // silencio: o terco superior do knob virava zona morta, o encoder do
        // Launchkey parava de subir em ~67% do curso, e ao reabrir o painel o
        // knob voltava sozinho para 1,0. O padrao tambem tem de ser 0,9 — o
        // mesmo de sempre — senao todo cliente ganharia um pad ~1 dB mais
        // baixo depois de atualizar.
        addParameter (padVolume = new juce::AudioParameterFloat (
            juce::ParameterID { "padVolume", 1 },
            "Pad Volume",
            juce::NormalisableRange<float> (0.0f, 1.5f),
            0.9f));

        // Constroi a LUT fixa do sinc/window na thread que cria o processador.
        // A primeira inicializacao de um static local pode usar um guard do
        // runtime; aquecer aqui garante que esse guard nunca estreia no
        // callback. A tabela e std::array: nao ha heap nem depois nem agora.
        (void) tabelaAntiAlias();

        formatManager.registerBasicFormats();
        startThread();
    }

    ~PadProcessor() override
    {
        signalThreadShouldExit();
        loadEvent.signal();
        // Um WAV de pad pode ter varios minutos e o reader faz IO em blocos.
        // Tres segundos permitiam que JUCE matasse a thread durante um read,
        // deixando AudioFormatReader/AudioBuffer vivos no shutdown (e, sob
        // ASan, reportando vazamentos). O timeout precisa cobrir um bloco de
        // disco lento; o evento continua acordando o caminho de espera sem
        // atrasar o encerramento normal.
        stopThread (10000);

        // O device ja parou antes de destruir o processador. Solta os pinos
        // restantes explicitamente para manter os contadores coerentes antes
        // de o cache (dono real dos WAVs) ser destruido nesta thread.
        for (auto& voice : voices)
            resetVoice (voice);
    }

    // pasta com pad_C.wav ... pad_B.wav (naturais A..G).
    // AppPaths resolve o bundle do macOS: la o executavel fica dentro de
    // ChurchDAW.app/Contents/MacOS, entao "ao lado do exe" nao e o que o
    // usuario ve como pasta do aplicativo.
    static bool isCompletePadFolder (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return false;

        for (const char note : { 'A', 'B', 'C', 'D', 'E', 'F', 'G' })
            if (! folder.getChildFile (
                      "pad_" + juce::String::charToString (note) + ".wav")
                     .existsAsFile())
                return false;

        return true;
    }

    static juce::File padsFolder()
    {
        // Nao basta a pasta EXISTIR: se um "Pads" errado aparecer primeiro (o
        // acervo de worship tem um), aceitar so pelo nome faria a busca parar
        // nele e o pad continuar "sem WAV". Exigir o conjunto natural A..G
        // tambem impede abrir um pack parcial que falharia ao trocar de tom.
        auto valid = [] (const juce::File& f)
        {
            return isCompletePadFolder (f);
        };

        // Modulo baixado pela loja (30/07): o instalador de bibliotecas
        // grava em managedLibrariesFolder()/Pads — que o findLibrary NAO
        // varre. E o caminho do pack "Pad Continuo" no Android.
        if (auto f = AppPaths::managedLibrariesFolder().getChildFile ("Pads");
            valid (f))
            return f;

        if (auto f = AppPaths::findLibrary ({ "Pads", "ChurchDAW Pads" }); valid (f))
            return f;

        // No Android o pack pode acabar um nivel abaixo do esperado: o
        // install_folder do catalogo e "Pads", mas instalacoes antigas
        // gravaram em Libraries/<id>/Pads. Varrer as raizes gerenciadas ate
        // dois niveis acha os dois casos sem depender do nome da pasta -
        // o pad_C.wav e que confirma. Cliente reclamou de "nao tem pad"
        // com a biblioteca ja baixada (31/07).
        for (auto raiz : { AppPaths::managedLibrariesFolder(),
                           AppPaths::managedDataFolder() })
        {
            if (! raiz.isDirectory())
                continue;

            if (valid (raiz))
                return raiz;

            for (const auto& nivel1 : raiz.findChildFiles (juce::File::findDirectories, false))
            {
                if (valid (nivel1))
                    return nivel1;

                for (const auto& nivel2 : nivel1.findChildFiles (juce::File::findDirectories, false))
                    if (valid (nivel2))
                        return nivel2;
            }
        }

        // Sao 7 arquivos: o usuario deixa a pasta solta na propria home
        // ("C:\Users\<ele>\ChurchDAW Pads"), que nao e raiz de biblioteca, nem
        // fica ao lado do exe, nem no ProgramData -- os tres lugares que o
        // findLibrary varre. Sem isto o painel do Pad abre dizendo que nao
        // achou os WAV mesmo com eles no disco.
        for (auto loc : { juce::File::userHomeDirectory,
                          juce::File::userDocumentsDirectory,
                          juce::File::userMusicDirectory })
            for (const char* n : { "ChurchDAW Pads", "Pads" })
                if (auto f = juce::File::getSpecialLocation (loc).getChildFile (n); valid (f))
                    return f;

        return {};
    }

    static bool padsAvailable()
    {
        return padsFolder().isDirectory();
    }

    //==============================================================================
    // UI (message thread): escolhe o tom (0=C .. 11=B) ou -1 = desligar
    void setKey (int key)
    {
        requestedKey.store (juce::jlimit (-1, 11, key));
        loadEvent.signal();     // acorda o loader (se precisar carregar)
    }

    int  getKey() const noexcept       { return requestedKey.load(); }
    bool isPlaying() const noexcept     { return requestedKey.load() >= 0; }

    // Transposicao global. Chamada da message thread e tambem do callback de
    // parametro do host (audioProcessorParameterChanged) — por isso e apenas
    // um store atomico, sem lock. A thread de audio le o atomico e soma no
    // rate de cada voz, entao o Pad acompanha o Transpose Global do app.
    void setTransposeSemis (int semis) noexcept
    {
        transposeSemis.store (juce::jlimit (-12, 12, semis));
    }

    int getTransposeSemis() const noexcept { return transposeSemis.load(); }

    void setGain (float g) noexcept
    {
        const auto v = juce::jlimit (0.0f, 1.5f, g);
        padGain.store (v);

        // Mantem o parametro em sincronia: senao o knob da tela e o valor
        // mapeado no controlador contariam historias diferentes.
        if (padVolume != nullptr)
            padVolume->setValueNotifyingHost (padVolume->convertTo0to1 (v));
    }

    float getGain() const noexcept
    {
        return padVolume != nullptr ? padVolume->get() : padGain.load();
    }

    // Corte 0..1 (0 = mais fechado, 1 = aberto). Mexer por aqui mantem o
    // parametro em dia, entao o knob da tela e o controle mapeado no
    // teclado mostram sempre o mesmo valor.
    void setCutoff (float t) noexcept
    {
        if (padCutoff != nullptr)
            padCutoff->setValueNotifyingHost (
                padCutoff->convertTo0to1 (juce::jlimit (0.0f, 1.0f, t)));
    }

    float getCutoff() const noexcept
    {
        return padCutoff != nullptr ? padCutoff->get() : 1.0f;
    }

    //==============================================================================
    // CONFIGURACAO POR TOM (pedido do dono, 05/08/2026).
    //
    // O QUE MORA AQUI — E O QUE DELIBERADAMENTE NAO MORA.
    //
    // O aparelho, o canal e a nota FISICA que comandam cada tom ja moram no
    // MIDI Learn GLOBAL do app (noteMappings -> midi-note-map.xml, com
    // NoteActionKind::padKey guardando o tom). Guardar isso de novo aqui
    // criaria dois lugares para mapear a mesma tecla e duas verdades para o
    // musico conferir no meio do culto — exatamente o que nao se pode ter.
    //
    // Entao esta estrutura guarda SO o que o mapa global nao sabe: a tecla do
    // computador, o modo de disparo e a politica de velocidade.
    //
    // ATENCAO — `oitava` e `notaDeSaida` NAO TEM CONTROLE NA TELA, de proposito.
    // Elas so alimentariam texto: producesMidi() e false, o no do grafo so tem
    // barramento de audio e a afinacao continua fixa em 2^(kSemis/12). Um menu
    // de oitava marcavel em cima disso e um botao que nao faz nada — e o numero
    // de nota que ele produzia contradizia a tecla FISICA mostrada no mesmo pad.
    // Os campos ficam para que um arquivo ja gravado continue carregando e
    // regravando igual. Quem for expor oitava de novo tem de ANTES somar o
    // semitom no rate do processBlock, e por um atomico proprio de transposicao
    // — a thread de audio nunca le esta estrutura, e nao pode passar a ler.
    //
    // Tudo isto e estado de MESSAGE THREAD. A thread de audio nunca le esta
    // estrutura: ela continua enxergando apenas o requestedKey atomico.
    enum class ModoDeVelocidade { midi = 0, fixa = 1, curva = 2 };

    // alternar            = aciona liga, acionar de novo desliga (o gesto da TELA hoje)
    // sustentar           = aciona liga e CONFIRMA; so "Parar" desliga (o gesto da NOTA MIDI hoje)
    // enquantoPressionado = soa enquanto o dedo/botao/tecla ficar em cima
    enum class ModoDeDisparo { alternar = 0, sustentar = 1, enquantoPressionado = 2 };

    // So existe entrada por NOTA. O campo existe para que um mapa gravado hoje
    // continue legivel se um dia entrar CC ou Program Change: um tipo
    // desconhecido e RECUSADO na leitura, em vez de ser tratado como nota por
    // engano e virar tecla presa no palco.
    enum class TipoDeEntrada { nota = 0 };

    struct MapeamentoDeTom
    {
        int oitava = 3;                 // C3 = 60, a mesma convencao do getMidiNoteName (.., 3) do resto do app
        int notaDeSaida = -1;           // -1 = derivada da oitava; >= 0 = fixada a dedo
        int teclaDoComputador = 0;      // 0 = nenhuma; senao o keyCode de um juce::KeyPress
        ModoDeVelocidade modoDeVelocidade = ModoDeVelocidade::midi;
        int velocidadeFixa = 100;
        ModoDeDisparo modoDeDisparo = ModoDeDisparo::alternar;
        TipoDeEntrada tipoDeEntrada = TipoDeEntrada::nota;

        // Igual ao padrao = nao vai para o disco. Assim um setup que nunca
        // abriu a configuracao grava EXATAMENTE o mesmo XML de antes.
        bool ehPadrao() const noexcept
        {
            return oitava == 3
                    && notaDeSaida < 0
                    && teclaDoComputador == 0
                    && modoDeVelocidade == ModoDeVelocidade::midi
                    && velocidadeFixa == 100
                    && modoDeDisparo == ModoDeDisparo::alternar
                    && tipoDeEntrada == TipoDeEntrada::nota;
        }
    };

    const MapeamentoDeTom& mapeamentoDoTom (int tom) const noexcept
    {
        return mapeamentos[(size_t) juce::jlimit (0, 11, tom)];
    }

    void setMapeamentoDoTom (int tom, const MapeamentoDeTom& novo)
    {
        if (! juce::isPositiveAndBelow (tom, 12))
            return;

        mapeamentos[(size_t) tom] = novo;
    }

    // Tom que responde a esta tecla do computador, ou -1 se nenhuma responde.
    int tomDaTeclaDoComputador (int keyCode) const noexcept
    {
        if (keyCode == 0)
            return -1;

        for (int t = 0; t < 12; ++t)
            if (mapeamentos[(size_t) t].teclaDoComputador == keyCode)
                return t;

        return -1;
    }

    // O painel so pede foco de teclado quando ha ao menos uma tecla atribuida:
    // sem isto, abrir o Pad passaria a roubar o teclado do resto do app sem
    // nenhum ganho para quem nunca configurou tecla nenhuma.
    bool algumaTeclaDoComputadorAtribuida() const noexcept
    {
        for (const auto& m : mapeamentos)
            if (m.teclaDoComputador != 0)
                return true;

        return false;
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int) override
    {
        deviceSampleRate = sampleRate >= 1000.0 ? sampleRate : 44100.0;

        for (auto& v : voices)
            resetVoice (v);

        caudaDeRoubo.reset();
        loadEvent.signal();
        currentKey = -1;

        for (auto& f : filtro)
            f.reset();

        cutoffSuave = juce::jmin (cutoffHz(),
                                  0.45f * (float) deviceSampleRate);

        ganhoSuave.reset (deviceSampleRate, 0.025);
        ganhoSuave.setCurrentAndTargetValue (
            juce::jlimit (0.0f, 1.5f,
                          padVolume != nullptr ? padVolume->get() : padGain.load()));
    }

    // Frequencia de corte em Hz a partir do parametro 0..1.
    //
    // A escala e LOGARITMICA de proposito: em escala linear, metade do curso
    // do knob ja estaria em 9 kHz e o musico nao teria controle nenhum na
    // regiao que importa para abrir e fechar um pad (200 Hz a 2 kHz).
    float cutoffHz() const noexcept
    {
        const float t = padCutoff != nullptr ? padCutoff->get() : 1.0f;
        return 120.0f * std::pow (160.0f, juce::jlimit (0.0f, 1.0f, t));   // 120 Hz .. 19,2 kHz
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();

        const int numSamples = buffer.getNumSamples();

        // O ganho vem do PARAMETRO quando ele existe, para que o knob de
        // volume do Pad tambem possa ser mapeado num controlador. O padGain
        // continua sendo a fonte para quem chama setGain() direto.
        //
        // Alvo suavizado (mesma logica do corte): aplicado por amostra DEPOIS
        // da soma das vozes, nao como constante por bloco — um CC de 7 bits no
        // volume viria como escada de 128 degraus numa fronteira de bloco.
        ganhoSuave.setTargetValue (
            juce::jlimit (0.0f, 1.5f,
                          padVolume != nullptr ? padVolume->get() : padGain.load()));

        const float fadeInc = 1.0f / (float) (juce::jmax (1.0, kFadeSeconds * deviceSampleRate));

        const int semiNow = juce::jlimit (-12, 12, transposeSemis.load (std::memory_order_relaxed));
        if (semiNow != lastAppliedTransposeSemis)
        {
            lastAppliedTransposeSemis = semiNow;
            currentTransposeRatio = std::pow (2.0, (double) semiNow / 12.0);
            for (auto& v : voices)
                if (v.active)
                    v.rate = v.baseRate * currentTransposeRatio;
        }

        // trata mudança de tom pedida pela UI
        const int req = requestedKey.load();
        if (req != currentKey)
        {
            if (req < 0)
            {
                for (auto& v : voices) v.target = 0.0f;   // desligar: some tudo
                currentKey = -1;
            }
            else
            {
                // pega o buffer natural do cache (sem travar o áudio se ocupado)
                const char letter = kLetter[req];
                CacheEntry* cacheEntry = nullptr;
                const juce::AudioBuffer<float>* buf = nullptr;
                double srcSr = 44100.0;
                int lpIni = 0, lpFim = 0;

                if (cacheLock.tryEnter())
                {
                    auto it = cache.find (letter);
                    if (it != cache.end()
                         && it->second.buffer != nullptr
                         && it->second.buffer->getNumSamples() > 0)
                    {
                        cacheEntry = &it->second;
                        cacheEntry->activeVoiceUsers.fetch_add (
                            1, std::memory_order_acquire);
                        buf   = cacheEntry->buffer.get();
                        srcSr = it->second.sampleRate;
                        lpIni = it->second.loopStart;
                        lpFim = it->second.loopEnd;
                    }
                    cacheLock.exit();
                }

                if (buf != nullptr && buf->getNumSamples() > 0)
                {
                    // Ja existe pad soando? Entao esta e uma TROCA de tom no
                    // meio da musica: a voz nova deve nascer no loopStart (que
                    // ja foi calculado), nao em pos=0 -- senao toca o intro
                    // silencioso de 3-5 s e abre um buraco de 15-19 dB. Quando
                    // o pad esta sendo LIGADO do zero, pos=0 preserva o swell
                    // de entrada intencional.
                    const bool jaSoando = std::any_of (
                        voices.begin(), voices.end(),
                        [] (const Voice& v)
                        { return v.active && v.gain > 0.0001f; });

                    for (auto& v : voices) v.target = 0.0f;   // some os antigos

                    if (auto* slot = freeVoice())
                    {
                        slot->cacheEntry = cacheEntry;
                        slot->buffer = buf;
                        cacheEntry = nullptr; // o slot assumiu o pino
                        slot->baseRate = (srcSr / deviceSampleRate)
                                         * std::pow (2.0, (double) kSemis[req] / 12.0);
                        slot->rate = slot->baseRate * currentTransposeRatio;
                        slot->gain = 0.0f;
                        slot->target = 1.0f;
                        slot->lastOutputL = 0.0f;
                        slot->lastOutputR = 0.0f;
                        slot->active = true;

                        // Saneado aqui, uma vez, e nao no laco de amostras.
                        // Cache sem pontos (ou incoerente) = laco do arquivo
                        // inteiro, igual ao comportamento antigo.
                        const int n = buf->getNumSamples();
                        slot->loopEnd   = (lpFim > 0 && lpFim <= n) ? lpFim : n;
                        slot->loopStart = (lpIni >= 0 && lpIni < slot->loopEnd) ? lpIni : 0;
                        slot->pos = jaSoando ? (double) slot->loopStart : 0.0;
                    }
                    currentKey = req;   // só marca como atendido quando o buffer chegou
                }

                // Defesa para futuras mudancas em freeVoice(): se nenhum slot
                // assumir o buffer, o pino nunca pode vazar. Hoje este ramo so
                // acontece com cache invalido.
                if (cacheEntry != nullptr)
                {
                    cacheEntry->activeVoiceUsers.fetch_sub (
                        1, std::memory_order_release);
                }
                // se ainda não carregou, mantém o pad atual tocando e tenta no próximo bloco
            }
        }

        auto* outL = buffer.getWritePointer (0);
        auto* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

        for (auto& v : voices)
        {
            if (! v.active || v.buffer == nullptr)
                continue;

            const auto& b = *v.buffer;
            const int len = b.getNumSamples();

            if (len <= 0)   // buffer vazio: nao ha o que ler (nem indice valido pro jlimit)
                continue;

            const float* srcL = b.getReadPointer (0);
            const float* srcR = b.getNumChannels() > 1 ? b.getReadPointer (1) : srcL;

            // Janela de repeticao resolvida UMA VEZ por bloco. Os valores ja
            // vieram saneados de quem montou a voz; o clamp aqui e cinto de
            // seguranca contra um buffer trocado sob os pes (troca de sample
            // rate, evicçao de cache) — ler fora do buffer no meio do culto e
            // ruido em fundo de escala, ou crash.
            const int loopEnd   = (v.loopEnd > 0 && v.loopEnd <= len) ? v.loopEnd : len;
            const int loopStart = (v.loopStart >= 0 && v.loopStart < loopEnd) ? v.loopStart : 0;
            const double loopLen = (double) (loopEnd - loopStart);

            for (int n = 0; n < numSamples; ++n)
            {
                // envelope de fade (in/out) suave
                if (v.gain < v.target)      v.gain = juce::jmin (v.target, v.gain + fadeInc);
                else if (v.gain > v.target) v.gain = juce::jmax (v.target, v.gain - fadeInc);

                const auto amostra = lerReamostradoStereo (
                    srcL, srcR, len, loopStart, loopEnd, v.pos, v.rate);

                const float g = v.gain;
                v.lastOutputL = amostra.l * g;
                v.lastOutputR = amostra.r * g;
                outL[n] += v.lastOutputL;
                if (outR != nullptr) outR[n] += v.lastOutputR;

                // Volta para loopStart preservando a fase fracionaria. O `if`
                // extra e um trinco, NAO um `while`: com um valor absurdo em
                // pos (rate corrompido, buffer trocado) um `while` giraria
                // preso dentro do callback e derrubaria o audio do culto.
                v.pos += v.rate;
                if (v.pos >= (double) loopEnd)
                {
                    v.pos -= loopLen;

                    if (! (v.pos >= (double) loopStart && v.pos < (double) loopEnd))
                        v.pos = (double) loopStart;
                }
            }

            if (v.target <= 0.0f && v.gain <= 0.0001f)   // terminou de sumir: libera
            {
                resetVoice (v);
            }
        }
        // applyGainRamp completaria a transicao dentro de um unico bloco
        // (~1 ms).
        // A quinta troca pode chegar antes das quatro rampas antigas acabarem.
        // A voz roubada deixa aqui seu ultimo frame por uma cauda de 5 ms, no
        // mesmo caminho de ganho/cutoff que ela usava, sem manter outro WAV.
        renderizarCaudaDeRoubo (outL, outR, numSamples);

        for (int n = 0; n < numSamples; ++n)
        {
            const float g = ganhoSuave.getNextValue();
            outL[n] *= g;
            if (outR != nullptr) outR[n] *= g;
        }

        aplicarCutoff (outL, outR, numSamples);
    }

    // FILTRO DE CORTE DO PAD (pedido de cliente, 01/08/2026).
    //
    // Fica DEPOIS da soma das vozes, e nao dentro do laco por voz: durante o
    // crossfade de 1,6 s entre tons ha duas vozes soando, e filtrar cada uma
    // em separado deixaria o corte "pulando" na troca de tom.
    //
    // Passa-baixa de 2 polos (dois TPT de 1 polo em serie). TPT porque ele e
    // estavel quando a frequencia muda a cada amostra — que e exatamente o
    // caso aqui, com um knob ou pedal varrendo o corte ao vivo.
    void aplicarCutoff (float* outL, float* outR, int numSamples) noexcept
    {
        const float alvo = cutoffHz();

        // TETO RELATIVO A NYQUIST, NAO ABSOLUTO.
        //
        // A estabilidade do TPT exige fc < sr/2, e nao "fc < 20 kHz". Com um
        // teto fixo de 20 kHz o filtro EXPLODE em qualquer taxa <= 38,4 kHz:
        // acima de Nyquist o tan() fica negativo, o coeficiente sai de (0,1)
        // e o polo sai do circulo unitario. Em 32 kHz — que o ASIO4ALL e
        // drivers onboard oferecem — o polo vale -1,96, ou seja, dobra a
        // cada amostra: ruido em fundo de escala no meio do culto.
        const float tetoHz = 0.45f * (float) deviceSampleRate;
        const float alvoSeguro = juce::jmin (alvo, tetoHz);

        // Nada a fazer quando o corte esta no topo: o filtro so tiraria CPU.
        //
        // Sair sem zerar o estado deixava um resto de audio congelado no
        // integrador; ao voltar do bypass com o pad em silencio, esse resto
        // saia como um "tec" de cerca de -20 dBFS. Zerar aqui nao custa nada
        // e o bypass ja garante que ninguem sente falta do que foi jogado
        // fora.
        if (alvoSeguro >= tetoHz && cutoffSuave >= tetoHz)
        {
            for (auto& f : filtro)
                f.reset();

            return;
        }

        // O corte caminha ate o alvo em ~30 ms. Sem isso, um CC de 7 bits
        // (128 degraus) vira ruido de escada audivel ao girar o knob.
        const float coef = 1.0f - std::exp (-1.0f / (float) (0.03 * deviceSampleRate));

        // cutoffSuave continua iterado por amostra (estado exato ao fim do
        // bloco, zero erro de interpolacao no escalar) — mas o coeficiente
        // `a` do TPT so e calculado 2x por bloco (inicio e fim) e interpolado
        // linearmente. O tan sai do laco interno: em regime permanente, com o
        // cutoffSuave parado ha ~150 ms (o estado do pad em 99,9% do culto),
        // isto corta ~48k chamadas de tan+divisao por segundo.
        const float cutoffInicio = cutoffSuave;
        for (int n = 0; n < numSamples; ++n)
            cutoffSuave += (alvoSeguro - cutoffSuave) * coef;
        const float cutoffFim = cutoffSuave;

        const float sr = (float) deviceSampleRate;
        auto aDe = [tetoHz, sr] (float fc)
        {
            const float g = std::tan (juce::MathConstants<float>::pi
                                        * juce::jlimit (20.0f, tetoHz, fc)
                                        / sr);
            return g / (1.0f + g);
        };

        const float aInicio = aDe (cutoffInicio);
        const float aFim    = aDe (cutoffFim);
        const float aInc = numSamples > 0 ? (aFim - aInicio) / (float) numSamples : 0.0f;

        for (int n = 0; n < numSamples; ++n)
        {
            const float a = aInicio + aInc * (float) n;
            outL[n] = filtro[1].processa (filtro[0].processa (outL[n], a), a);

            if (outR != nullptr)
                outR[n] = filtro[3].processa (filtro[2].processa (outR[n], a), a);
        }
    }

    //==============================================================================
    const juce::String getName() const override           { return "Pad Continuo"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    double getTailLengthSeconds() const override           { return (double) kFadeSeconds; }
    juce::AudioProcessorEditor* createEditor() override    { return nullptr; }
    bool hasEditor() const override                        { return false; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    // ESTADO DO PAD.
    //
    // Antes estes dois metodos eram vazios: nem o tom, nem o volume, nem o
    // corte sobreviviam a fechar o app. O tom o musico ate reescolhe rapido;
    // um corte ajustado a dedo, nao — ele reabriria o culto seguinte com o
    // pad todo aberto sem entender por que.
    void getStateInformation (juce::MemoryBlock& destino) override
    {
        juce::XmlElement xml ("PAD");
        xml.setAttribute ("gain", (double) getGain());
        xml.setAttribute ("cutoff", (double) getCutoff());

        // CONFIGURACAO POR TOM: so os tons que FUGIRAM do padrao viram filho.
        //
        // Quem nunca abriu a configuracao grava um <PAD> sem nenhum <TOM>, que
        // e byte a byte o que as versoes anteriores gravavam. E uma versao
        // anterior que leia este arquivo simplesmente ignora os filhos que nao
        // conhece — a compatibilidade vale nos dois sentidos.
        for (int t = 0; t < 12; ++t)
        {
            const auto& m = mapeamentos[(size_t) t];

            if (m.ehPadrao())
                continue;

            auto* e = xml.createNewChildElement ("TOM");
            e->setAttribute ("tom", t);
            e->setAttribute ("oitava", m.oitava);
            e->setAttribute ("entrada", "note");
            e->setAttribute ("modoVel", (int) m.modoDeVelocidade);
            e->setAttribute ("velFixa", m.velocidadeFixa);
            e->setAttribute ("disparo", (int) m.modoDeDisparo);

            if (m.notaDeSaida >= 0)
                e->setAttribute ("nota", m.notaDeSaida);

            // Descricao de texto, nao o codigo cru: e a forma documentada de
            // guardar um KeyPress em disco (createFromDescription le de volta)
            // e ainda deixa o XML legivel quando o cliente manda o arquivo.
            if (m.teclaDoComputador != 0)
                e->setAttribute ("teclaPc",
                                 juce::KeyPress (m.teclaDoComputador).getTextDescription());
        }

        copyXmlToBinary (xml, destino);
    }

    void setStateInformation (const void* dados, int tamanho) override
    {
        auto xml = getXmlFromBinary (dados, tamanho);

        if (xml == nullptr || ! xml->hasTagName ("PAD"))
            return;

        // Leitura ADITIVA: atributo ausente mantem o valor atual, para um
        // estado gravado por versao antiga nao zerar o que ela nao conhecia.
        if (xml->hasAttribute ("gain"))
            setGain ((float) xml->getDoubleAttribute ("gain", (double) getGain()));

        if (xml->hasAttribute ("cutoff"))
            setCutoff ((float) xml->getDoubleAttribute ("cutoff", (double) getCutoff()));

        // CONFIGURACAO POR TOM.
        //
        // Estado gravado por versao antiga nao tem nenhum <TOM>: o laco nao
        // roda e os 12 tons ficam no padrao (oitava 3, sem tecla de PC,
        // velocidade "midi", disparo "alternar"), que e exatamente como o app
        // se comportava antes. Nada de limpar `mapeamentos` aqui: um arquivo
        // antigo apagaria a configuracao viva de quem ja tinha configurado.
        for (auto* e : xml->getChildIterator())
        {
            if (! e->hasTagName ("TOM"))
                continue;

            const int t = e->getIntAttribute ("tom", -1);

            if (! juce::isPositiveAndBelow (t, 12))
                continue;

            // Tipo de entrada desconhecido = entrada de uma versao FUTURA.
            // Ler assim mesmo transformaria, por exemplo, um mapeamento de CC
            // numa "nota" inventada. Melhor pular a entrada inteira.
            const auto tipo = e->getStringAttribute ("entrada", "note");

            if (tipo.isNotEmpty() && tipo != "note")
                continue;

            MapeamentoDeTom m;
            m.oitava      = juce::jlimit (-2, 8, e->getIntAttribute ("oitava", 3));
            m.notaDeSaida = e->hasAttribute ("nota")
                              ? juce::jlimit (0, 127, e->getIntAttribute ("nota", 0))
                              : -1;
            m.velocidadeFixa = juce::jlimit (1, 127, e->getIntAttribute ("velFixa", 100));

            const int mv = e->getIntAttribute ("modoVel", 0);
            m.modoDeVelocidade = mv == 1 ? ModoDeVelocidade::fixa
                               : mv == 2 ? ModoDeVelocidade::curva
                                         : ModoDeVelocidade::midi;

            const int md = e->getIntAttribute ("disparo", 0);
            m.modoDeDisparo = md == 1 ? ModoDeDisparo::sustentar
                            : md == 2 ? ModoDeDisparo::enquantoPressionado
                                      : ModoDeDisparo::alternar;

            if (e->hasAttribute ("teclaPc"))
            {
                const auto tecla = juce::KeyPress::createFromDescription (
                    e->getStringAttribute ("teclaPc"));

                if (tecla.isValid())
                    m.teclaDoComputador = tecla.getKeyCode();
            }

            mapeamentos[(size_t) t] = m;
        }

        // Uma tecla do computador so pode comandar UM tom. Um arquivo editado
        // a mao (ou vindo de outra maquina) podia repetir a mesma tecla em
        // dois tons; nesse caso o ultimo tom vencia em silencio e o musico
        // ficava com um pad que "as vezes" respondia. Aqui a duplicata e
        // apagada logo na leitura.
        for (int a = 0; a < 12; ++a)
        {
            const int tecla = mapeamentos[(size_t) a].teclaDoComputador;

            if (tecla == 0)
                continue;

            for (int b = a + 1; b < 12; ++b)
                if (mapeamentos[(size_t) b].teclaDoComputador == tecla)
                    mapeamentos[(size_t) b].teclaDoComputador = 0;
        }

        // O tom ativo e estado de execucao, como uma nota MIDI sustentada:
        // restaura-lo faria o app voltar tocando sozinho depois de reiniciar.
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

private:
    static constexpr double kFadeSeconds = 1.6;   // crossfade entre tons
    static constexpr double kStealDeclickSeconds = 0.005;

    // tom (0..11) -> letra natural do arquivo + semitons de afinação
    static constexpr char kLetter[12] = { 'C','C','D','D','E','F','F','G','G','A','A','B' };
    static constexpr int  kSemis [12] = {  0 , 1 , 0 , 1 , 0 , 0 , 1 , 0 , 1 , 0 , 1 , 0  };

    struct CacheEntry
    {
        // Somente PadLoader cria/destroi o dono compartilhado. O callback
        // prende a entrada com activeVoiceUsers e le apenas o ponteiro cru;
        // assim note-on, fade concluido e roubo de voz nao executam refcount,
        // free() ou destrutor de AudioBuffer na thread de tempo real.
        std::shared_ptr<juce::AudioBuffer<float>> buffer;
        std::atomic<int> activeVoiceUsers { 0 };
        double sampleRate = 44100.0;
        int loopStart = 0, loopEnd = 0;
    };

    struct Voice
    {
        CacheEntry* cacheEntry = nullptr;
        const juce::AudioBuffer<float>* buffer = nullptr;
        double pos = 0.0, baseRate = 1.0, rate = 1.0;
        float gain = 0.0f, target = 0.0f;
        float lastOutputL = 0.0f, lastOutputR = 0.0f;
        bool active = false;

        // Janela de repeticao, JA VALIDADA na thread de carga (ver Loop).
        // A thread de audio nao decide nada sobre loop: so le estes dois.
        int loopStart = 0, loopEnd = 0;
    };

    struct CaudaDeRoubo
    {
        float l = 0.0f, r = 0.0f;
        float passoL = 0.0f, passoR = 0.0f;
        int restantes = 0;

        void reset() noexcept { *this = {}; }
    };

    // Reamostragem band-limited do Pad Continuo.
    //
    // Interpolacao linear reconstrui imagens ate o Nyquist da FONTE. Quando o
    // cursor anda mais de uma amostra por frame (rate > 1), parte dessa banda
    // fica acima do Nyquist da SAIDA e dobra para baixo como campainha/metal.
    // O sinc abaixo corta em 92% do novo Nyquist antes de escolher a amostra.
    //
    // A LUT e fixa, criada no construtor, e o caminho quente usa apenas pilha,
    // aritmetica e leituras do WAV: zero new/delete, lock, seno ou cosseno no
    // callback. Para rate <= 1 preservamos exatamente a interpolacao antiga.
    static constexpr int kAntiAliasRadius = 32;
    static constexpr int kAntiAliasLookupSize = 4096;

    struct AntiAliasTable
    {
        std::array<float, (size_t) kAntiAliasLookupSize + 1> sinc {};
        std::array<float, (size_t) kAntiAliasLookupSize + 1> blackman {};

        AntiAliasTable() noexcept
        {
            for (int i = 0; i <= kAntiAliasLookupSize; ++i)
            {
                const double normalizado = (double) i / kAntiAliasLookupSize;
                const double x = normalizado * kAntiAliasRadius;
                sinc[(size_t) i] = x <= 1.0e-12
                    ? 1.0f
                    : (float) (std::sin (juce::MathConstants<double>::pi * x)
                               / (juce::MathConstants<double>::pi * x));

                // Blackman simetrica: 1 no centro, 0 na borda do suporte.
                const double janela = 0.42
                    + 0.50 * std::cos (juce::MathConstants<double>::pi
                                       * normalizado)
                    + 0.08 * std::cos (juce::MathConstants<double>::twoPi
                                       * normalizado);
                blackman[(size_t) i] = (float) juce::jmax (0.0, janela);
            }
        }

        static float interpolar (const std::array<float,
                                  (size_t) kAntiAliasLookupSize + 1>& tabela,
                                  double indice) noexcept
        {
            const double seguro = juce::jlimit (
                0.0, (double) kAntiAliasLookupSize, indice);
            const int i0 = (int) seguro;

            if (i0 >= kAntiAliasLookupSize)
                return tabela[(size_t) kAntiAliasLookupSize];

            const float frac = (float) (seguro - i0);
            return tabela[(size_t) i0]
                    + (tabela[(size_t) i0 + 1] - tabela[(size_t) i0]) * frac;
        }

        float lerSinc (double x) const noexcept
        {
            return interpolar (sinc, x * kAntiAliasLookupSize
                                      / kAntiAliasRadius);
        }

        float lerJanela (double distancia) const noexcept
        {
            return interpolar (blackman, distancia * kAntiAliasLookupSize
                                           / kAntiAliasRadius);
        }
    };

    static const AntiAliasTable& tabelaAntiAlias() noexcept
    {
        static const AntiAliasTable tabela;
        return tabela;
    }

    static int indiceRepetido (int indice, int len,
                               int loopStart, int loopEnd) noexcept
    {
        const int loopLen = loopEnd - loopStart;

        if (indice < 0)
        {
            // Loop do arquivo inteiro: depois da primeira volta, o passado de
            // pos=0 e a cauda, nao uma extensao DC da primeira amostra.
            if (loopStart != 0 || loopLen <= 0)
                return 0;

            const auto resto = (juce::int64) indice % loopLen;
            return (int) (resto < 0 ? resto + loopLen : resto);
        }

        if (indice < loopEnd)
            return juce::jmin (len - 1, indice);

        if (loopLen <= 0)
            return juce::jlimit (0, len - 1, indice);

        // O suporte do FIR olha algumas amostras alem do fim. Elas pertencem
        // ao comeco do loop, nao ao trecho descartado depois de um chunk smpl.
        const auto relativo = (juce::int64) indice - loopStart;
        return loopStart + (int) (relativo % loopLen);
    }

    struct AmostraStereo
    {
        float l = 0.0f;
        float r = 0.0f;
    };

    static AmostraStereo lerReamostradoStereo (
        const float* srcL, const float* srcR, int len,
        int loopStart, int loopEnd, double pos, double rate) noexcept
    {
        const int i0 = juce::jlimit (0, len - 1, (int) pos);
        int i1 = i0 + 1;

        if (i1 >= loopEnd)
            i1 = loopStart;

        const float frac = (float) (pos - (double) i0);
        const AmostraStereo linear {
            srcL[i0] + (srcL[i1] - srcL[i0]) * frac,
            srcR[i0] + (srcR[i1] - srcR[i0]) * frac
        };

        if (! std::isfinite (rate) || rate <= 1.0)
            return linear;

        const double cutoff = juce::jlimit (1.0e-6, 0.92, 0.92 / rate);
        const auto& tabela = tabelaAntiAlias();
        const int centro = (int) std::floor (pos);
        double somaL = 0.0;
        double somaR = 0.0;
        double somaPesos = 0.0;

        for (int deslocamento = -kAntiAliasRadius;
             deslocamento <= kAntiAliasRadius; ++deslocamento)
        {
            const int indice = centro + deslocamento;
            const double distancia = (double) indice - pos;
            const double modulo = std::abs (distancia);

            if (modulo >= kAntiAliasRadius)
                continue;

            const double peso = cutoff
                * (double) tabela.lerSinc (modulo * cutoff)
                * (double) tabela.lerJanela (modulo);
            const int indiceFonte = indiceRepetido (
                indice, len, loopStart, loopEnd);
            somaL += (double) srcL[indiceFonte] * peso;
            somaR += (double) srcR[indiceFonte] * peso;
            somaPesos += peso;
        }

        // Normalizar por fase conserva DC e a banda passante. O fallback e
        // defensivo para rates corrompidos; com os limites reais a soma fica
        // sempre perto de 1.
        if (! std::isfinite (somaL) || ! std::isfinite (somaR)
             || std::abs (somaPesos) <= 1.0e-12)
            return linear;

        const AmostraStereo saida {
            (float) (somaL / somaPesos),
            (float) (somaR / somaPesos)
        };
        return std::isfinite (saida.l) && std::isfinite (saida.r)
                ? saida : linear;
    }

    static float lerReamostrado (const float* src, int len,
                                 int loopStart, int loopEnd,
                                 double pos, double rate) noexcept
    {
        return lerReamostradoStereo (src, src, len, loopStart, loopEnd,
                                     pos, rate).l;
    }

    void iniciarCaudaDeRoubo (const Voice& voz) noexcept
    {
        caudaDeRoubo.l += voz.lastOutputL;
        caudaDeRoubo.r += voz.lastOutputR;
        caudaDeRoubo.restantes = juce::jmax (
            1, (int) std::round (kStealDeclickSeconds * deviceSampleRate));
        caudaDeRoubo.passoL = -caudaDeRoubo.l / caudaDeRoubo.restantes;
        caudaDeRoubo.passoR = -caudaDeRoubo.r / caudaDeRoubo.restantes;
    }

    void renderizarCaudaDeRoubo (float* outL, float* outR,
                                 int numSamples) noexcept
    {
        for (int n = 0; n < numSamples && caudaDeRoubo.restantes > 0; ++n)
        {
            outL[n] += caudaDeRoubo.l;
            if (outR != nullptr)
                outR[n] += caudaDeRoubo.r;

            caudaDeRoubo.l += caudaDeRoubo.passoL;
            caudaDeRoubo.r += caudaDeRoubo.passoR;
            --caudaDeRoubo.restantes;
        }

        if (caudaDeRoubo.restantes <= 0)
            caudaDeRoubo.reset();
    }

    void releaseVoiceBuffer (Voice& voice) noexcept
    {
        auto* const entry = voice.cacheEntry;
        voice.cacheEntry = nullptr;
        voice.buffer = nullptr;

        if (entry == nullptr)
            return;

        const int previous = entry->activeVoiceUsers.fetch_sub (
            1, std::memory_order_release);
        jassert (previous > 0);

        // Nao sinaliza WaitableEvent daqui: isso pode entrar no kernel. O
        // loader faz uma varredura leve a cada 250 ms fora do callback.
    }

    void resetVoice (Voice& voice) noexcept
    {
        releaseVoiceBuffer (voice);
        voice = {};
    }

    Voice* freeVoice()
    {
        for (auto& v : voices)
            if (! v.active) return &v;

        // nenhuma livre: reusa a de menor ganho (a que está sumindo)
        Voice* q = &voices[0];
        for (auto& v : voices) if (v.gain < q->gain) q = &v;

        // O slot sera reescrito imediatamente. Preserva o valor na fronteira
        // e o leva a zero com armazenamento fixo, evitando o degrau da quinta
        // troca sem alocacao, lock adicional ou referencia extra ao WAV.
        iniciarCaudaDeRoubo (*q);
        resetVoice (*q);
        return q;
    }

    //==============================================================================
    // LOOP DO PAD CONTINUO.
    // A SOLUCAO E TODA FEITA AQUI, NA THREAD DE CARGA. O callback de audio nao
    // ganhou nem um ramo a mais no laco interno: ele so passou a comparar com
    // loopEnd em vez de len. Nada de segunda leitura, mixagem ou envelope por
    // amostra na thread de audio — num app que toca ao vivo, o laco de saida e
    // o ultimo lugar onde se deve por logica nova.
    //
    // 1) PONTO DE LOOP que pula a subida de entrada: o intro silencioso passa a
    //    tocar UMA VEZ so, na primeira passada, e nunca mais.
    // 2) EMENDA CORRELACIONADA, calculada em conjunto para L/R. O ponto de
    //    entrada e o fim podem andar somente dentro de trechos estaveis; a
    //    busca favorece forma de onda, fase, energia e imagem stereo parecidas.
    //    Depois um crossfade curto e normalizado pela correlacao e gravado no
    //    proprio buffer. Nada disto roda no callback de audio.
    // 3) Se o WAV tiver chunk smpl, os pontos do autor mandam (ver lerSmpl).
    struct Loop
    {
        int inicio = 0;
        int fim = 0;
        int crossfade = 0;
        float correlacao = 0.0f;
        float correlacaoDeFase = 0.0f;
    };

    // A regressao sintetica precisa exercitar exatamente a preparacao usada
    // pelo loader, sem publicar uma API de produto para um detalhe interno.
    friend struct PadProcessorTestAccess;

    static constexpr double kEnvJanelaSeg    = 0.05;  // janela do envelope RMS
    static constexpr float  kEnvFracaoLimiar = 0.85f; // -1,4 dB do RMS de referencia
    static constexpr double kEnvEstavelSeg   = 2.0;   // tem de FICAR acima, nao so encostar
    static constexpr double kEnvMargemSeg    = 1.0;   // folga depois do ponto detectado
    static constexpr double kBuscaInicioSeg  = 2.0;   // so avanca dentro do sustain estavel
    static constexpr double kBuscaFimSeg     = 1.5;   // no maximo isto e descartado do fim
    static constexpr double kAnaliseFaseSeg  = 0.12;  // varias voltas ate para fundamentais graves
    static constexpr double kXfadeBomSeg     = 0.12;  // emenda coerente: curta, sem chorus artificial
    static constexpr double kXfadeMedioSeg   = 0.08;
    static constexpr double kXfadeRuimSeg    = 0.04;  // baixa correlacao: nao prolonga o batimento
    static constexpr double kXfadeSmplSeg    = 0.02;  // ponto autorado nunca e deslocado
    static constexpr double kLoopMinSeg      = 5.0;   // abaixo disto nao vale a pena

    // Pontos gravados pelo autor do WAV (chunk smpl). {-1,-1} = nao ha.
    // Na spec RIFF o campo `end` e a ULTIMA amostra do loop (inclusiva); aqui
    // tudo trabalha com fim EXCLUSIVO, dai o +1.
    static Loop lerSmpl (const juce::StringPairArray& meta, int len)
    {
        if (meta.getValue ("NumSampleLoops", "0").getIntValue() < 1)
            return { -1, -1 };

        const juce::int64 s = meta.getValue ("Loop0Start", "-1").getLargeIntValue();
        const juce::int64 e = meta.getValue ("Loop0End",   "-1").getLargeIntValue();

        if (s < 0 || e <= s || s >= (juce::int64) len)
            return { -1, -1 };

        const int inicio = (int) juce::jlimit ((juce::int64) 0, (juce::int64) len - 1, s);
        const int fim    = (int) juce::jlimit ((juce::int64) inicio + 1, (juce::int64) len, e + 1);

        return { inicio, fim };
    }

    // Acha onde o pad ja esta "cheio". Envelope RMS em janelas curtas, limiar
    // RELATIVO a mediana da metade final do arquivo — mediana, e nao media,
    // porque o intro silencioso puxaria uma media para baixo e o limiar junto.
    static Loop detectarPorEnvelope (const juce::AudioBuffer<float>& b, double sr)
    {
        const int len   = b.getNumSamples();
        const int chans = juce::jmax (1, b.getNumChannels());
        const int w     = juce::jmax (1, (int) (kEnvJanelaSeg * sr));
        const int nw    = len / w;

        if (nw < 8)
            return { 0, len };

        std::vector<float> env ((size_t) nw, 0.0f);

        for (int i = 0; i < nw; ++i)
        {
            double acc = 0.0;

            for (int c = 0; c < chans; ++c)
            {
                const float* p = b.getReadPointer (c) + (juce::int64) i * w;

                for (int k = 0; k < w; ++k)
                    acc += (double) p[k] * (double) p[k];
            }

            env[(size_t) i] = (float) std::sqrt (acc / (double) w / (double) chans);
        }

        std::vector<float> metade (env.begin() + nw / 2, env.end());
        std::nth_element (metade.begin(), metade.begin() + metade.size() / 2, metade.end());
        const float ref = metade[metade.size() / 2];

        if (! (ref > 0.0f))
            return { 0, len };

        const float limiar = kEnvFracaoLimiar * ref;

        // FIM: recua enquanto o final estiver fraco. Os pads de hoje terminam
        // em nivel cheio e isto nao corta nada — existe para o dia em que um
        // pack vier com fade-out, caso em que cruzar contra o silencio final
        // devolveria o mesmo buraco por outro caminho.
        int fimW = nw;
        while (fimW > 0 && env[(size_t) fimW - 1] < limiar)
            --fimW;

        const int fim = (fimW == nw) ? len : fimW * w;

        // INICIO: primeira janela a partir da qual o sinal FICA acima do limiar
        // por kEnvEstavelSeg seguidos. Exigir permanencia impede parar num pico
        // isolado no meio da subida — que existe, e daria um loopStart cedo
        // demais, com o buraco de volta pela metade.
        const int precisa = juce::jmax (1, (int) (kEnvEstavelSeg / kEnvJanelaSeg));
        int corrido = 0, iniW = -1;

        for (int i = fimW - 1; i >= 0; --i)
        {
            corrido = env[(size_t) i] >= limiar ? corrido + 1 : 0;

            if (corrido >= precisa)
                iniW = i;
        }

        if (iniW <= 0)
            return { 0, fim };

        return { iniW * w + (int) (kEnvMargemSeg * sr), fim };
    }

    struct AnaliseEmenda
    {
        double pontuacao = std::numeric_limits<double>::lowest();
        float correlacao = 0.0f;
        float correlacaoDeFase = 0.0f;
        bool valida = false;
    };

    // Compara [fim-janela, fim) com o pre-roll [inicio-janela, inicio).
    //
    // A soma e centrada (DC nao pode fingir correlacao) e usa o mesmo offset
    // para os dois canais. L, R, Mid e Side participam do guard de fase; assim
    // uma aparente boa soma mono nao pode esconder um Side invertido. Side
    // quase silencioso e ignorado para que ruido de arredondamento nao mande
    // na escolha. Tudo usa acumuladores fixos na pilha.
    static AnaliseEmenda analisarEmenda (const juce::AudioBuffer<float>& b,
                                         int inicio, int fim, int janela)
    {
        AnaliseEmenda resultado;
        const int canais = juce::jlimit (1, 2, b.getNumChannels());

        if (janela < 8 || inicio < janela || fim < janela
             || inicio >= fim || fim > b.getNumSamples())
            return resultado;

        std::array<double, 4> somaA {}, somaB {}, quadradoA {}, quadradoB {}, produto {};
        int quantidade = 0;

        const float* aL = b.getReadPointer (0) + fim - janela;
        const float* bL = b.getReadPointer (0) + inicio - janela;
        const float* aR = canais > 1 ? b.getReadPointer (1) + fim - janela : aL;
        const float* bR = canais > 1 ? b.getReadPointer (1) + inicio - janela : bL;

        // Sem decimacao: pular amostras sem um low-pass anterior poderia dar
        // nota alta/shimmer como "em fase" por alias justamente numa rotina
        // criada para impedir metalizacao.
        for (int i = 0; i < janela; ++i)
        {
            const std::array<double, 4> a {
                aL[i], aR[i], 0.5 * ((double) aL[i] + aR[i]),
                0.5 * ((double) aL[i] - aR[i])
            };
            const std::array<double, 4> entrada {
                bL[i], bR[i], 0.5 * ((double) bL[i] + bR[i]),
                0.5 * ((double) bL[i] - bR[i])
            };
            const int representacoes = canais > 1 ? 4 : 1;

            for (int r = 0; r < representacoes; ++r)
            {
                somaA[(size_t) r] += a[(size_t) r];
                somaB[(size_t) r] += entrada[(size_t) r];
                quadradoA[(size_t) r] += a[(size_t) r] * a[(size_t) r];
                quadradoB[(size_t) r] += entrada[(size_t) r] * entrada[(size_t) r];
                produto[(size_t) r] += a[(size_t) r] * entrada[(size_t) r];
            }

            ++quantidade;
        }

        if (quantidade < 4)
            return resultado;

        std::array<double, 4> energiaA {}, energiaB {}, covariancia {}, correlacao {};
        const int representacoes = canais > 1 ? 4 : 1;

        for (int r = 0; r < representacoes; ++r)
        {
            const double invN = 1.0 / (double) quantidade;
            energiaA[(size_t) r] = juce::jmax (0.0, quadradoA[(size_t) r]
                                                        - somaA[(size_t) r] * somaA[(size_t) r] * invN);
            energiaB[(size_t) r] = juce::jmax (0.0, quadradoB[(size_t) r]
                                                        - somaB[(size_t) r] * somaB[(size_t) r] * invN);
            covariancia[(size_t) r] = produto[(size_t) r]
                                        - somaA[(size_t) r] * somaB[(size_t) r] * invN;

            const double denominador = std::sqrt (energiaA[(size_t) r]
                                                   * energiaB[(size_t) r]);
            correlacao[(size_t) r] = denominador > 1.0e-15
                                      ? juce::jlimit (-1.0, 1.0,
                                                      covariancia[(size_t) r] / denominador)
                                      : 0.0;
        }

        const double energiaTotalA = energiaA[0] + (canais > 1 ? energiaA[1] : 0.0);
        const double energiaTotalB = energiaB[0] + (canais > 1 ? energiaB[1] : 0.0);
        const double covarianciaTotal = covariancia[0]
                                        + (canais > 1 ? covariancia[1] : 0.0);
        const double denominadorTotal = std::sqrt (energiaTotalA * energiaTotalB);

        if (denominadorTotal <= 1.0e-15)
            return resultado;

        const double correlacaoTotal = juce::jlimit (-1.0, 1.0,
                                                      covarianciaTotal / denominadorTotal);
        double guardaDeFase = correlacaoTotal;
        const double energiaSignificativa = 0.005 * juce::jmin (energiaTotalA,
                                                                energiaTotalB);

        // L/R sempre defendem a imagem stereo quando carregam energia. Mid e
        // Side pegam o caso em que os canais isolados parecem aceitaveis, mas
        // a largura muda de polaridade na volta.
        for (int r = 0; r < representacoes; ++r)
            if (juce::jmin (energiaA[(size_t) r], energiaB[(size_t) r])
                    > energiaSignificativa)
                guardaDeFase = juce::jmin (guardaDeFase, correlacao[(size_t) r]);

        const double diferencaDeNivel = std::abs (0.5 * std::log (
            (energiaTotalB + 1.0e-20) / (energiaTotalA + 1.0e-20)));
        double diferencaDeBalanco = 0.0;

        if (canais > 1)
        {
            const double balancoA = 0.5 * std::log ((energiaA[0] + 1.0e-20)
                                                    / (energiaA[1] + 1.0e-20));
            const double balancoB = 0.5 * std::log ((energiaB[0] + 1.0e-20)
                                                    / (energiaB[1] + 1.0e-20));
            diferencaDeBalanco = std::abs (balancoA - balancoB);
        }

        resultado.pontuacao = 0.75 * correlacaoTotal + 0.25 * guardaDeFase
                               - 0.12 * juce::jmin (2.0, diferencaDeNivel)
                               - 0.05 * juce::jmin (2.0, diferencaDeBalanco);
        resultado.correlacao = (float) correlacaoTotal;
        resultado.correlacaoDeFase = (float) guardaDeFase;
        resultado.valida = true;
        return resultado;
    }

    static bool emendaMelhor (const AnaliseEmenda& candidata,
                              const AnaliseEmenda& atual) noexcept
    {
        return candidata.valida
                && (! atual.valida || candidata.pontuacao > atual.pontuacao + 1.0e-8);
    }

    // Busca unidimensional com passe grosso e refinamento amostra a amostra.
    // Alternar inicio/fim evita o custo quadratico de testar todo par possivel.
    static AnaliseEmenda buscarEmenda (const juce::AudioBuffer<float>& b,
                                       int& inicio, int& fim, int janela,
                                       int minimo, int maximo, int passo,
                                       bool moverInicio)
    {
        int escolhido = moverInicio ? inicio : fim;
        AnaliseEmenda melhor = analisarEmenda (b, inicio, fim, janela);

        auto testar = [&] (int candidato)
        {
            const auto a = moverInicio ? analisarEmenda (b, candidato, fim, janela)
                                       : analisarEmenda (b, inicio, candidato, janela);
            if (emendaMelhor (a, melhor))
            {
                melhor = a;
                escolhido = candidato;
            }
        };

        for (int candidato = minimo; candidato <= maximo; candidato += passo)
            testar (candidato);

        const int centro = escolhido;
        for (int candidato = juce::jmax (minimo, centro - passo);
             candidato <= juce::jmin (maximo, centro + passo); ++candidato)
            testar (candidato);

        if (moverInicio)
            inicio = escolhido;
        else
            fim = escolhido;

        return melhor;
    }

    static AnaliseEmenda alinharLoopInferido (const juce::AudioBuffer<float>& b,
                                              double sr, int loopMin,
                                              int& inicio, int& fim)
    {
        const int inicioEnvelope = inicio;
        const int fimEnvelope = fim;
        const int janela = juce::jmax (32, (int) std::round (kAnaliseFaseSeg * sr));
        const int passo = juce::jmax (1, (int) std::round (sr / 700.0));
        const int inicioMax = juce::jmin (fim - loopMin,
                                         inicioEnvelope + (int) (kBuscaInicioSeg * sr));

        if (inicioEnvelope >= janela && inicioMax >= inicioEnvelope)
            buscarEmenda (b, inicio, fim, janela, inicioEnvelope, inicioMax,
                          passo, true);

        const int fimMin = juce::jmax (inicio + loopMin,
                                      fimEnvelope - (int) (kBuscaFimSeg * sr));

        if (fimMin <= fimEnvelope)
            buscarEmenda (b, inicio, fim, janela, fimMin, fimEnvelope,
                          passo, false);

        // A mudanca do fim altera a melhor fase de entrada; um ultimo passe
        // fecha a coordenada sem explodir em uma busca inicio x fim.
        const int inicioMaxFinal = juce::jmin (fim - loopMin,
                                              inicioEnvelope + (int) (kBuscaInicioSeg * sr));
        if (inicioEnvelope >= janela && inicioMaxFinal >= inicioEnvelope)
            buscarEmenda (b, inicio, fim, janela, inicioEnvelope, inicioMaxFinal,
                          passo, true);

        return analisarEmenda (b, inicio, fim, janela);
    }

    // Grava o crossfade NO BUFFER. So a thread de carga chama, e so antes de o
    // buffer entrar no cache: nenhuma voz pode estar lendo dele ainda. As
    // regioes nao se sobrepoem porque x <= (fim-inicio)/2.
    //
    // A lei comeca numa mistura linear (sem o +3 dB da antiga cos/sin quando
    // os sinais casam) e normaliza a potencia esperada por
    // a^2+b^2+2*rho*a*b. Um unico par de ganhos atende L/R, portanto largura,
    // pan e relacao de fase entre os canais nao sao remodelados.
    static void gravarCrossfade (juce::AudioBuffer<float>& b, int inicio, int fim,
                                 int x, float correlacao)
    {
        const double rho = juce::jlimit (0.0, 0.999,
                                         (double) correlacao);

        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            float* p = b.getWritePointer (c);

            for (int i = 0; i < x; ++i)
            {
                const double linear = (double) (i + 1) / (double) x;
                const double t = linear * linear * (3.0 - 2.0 * linear);
                const double saiBase = 1.0 - t;
                const double entraBase = t;
                const double potencia = saiBase * saiBase + entraBase * entraBase
                                          + 2.0 * rho * saiBase * entraBase;
                const float normalizador = (float) (1.0 / std::sqrt (
                    juce::jmax (0.5, potencia)));
                const float sai = (float) saiBase * normalizador;
                const float entra = (float) entraBase * normalizador;

                p[fim - x + i] = p[fim - x + i] * sai
                                  + p[inicio - x + i] * entra;
            }
        }
    }

    // Decide os pontos, aplica o crossfade e devolve a janela JA VALIDADA.
    // Se qualquer invariante nao fechar, devolve {0, len}, igual ao legado.
    static Loop prepararLoop (juce::AudioBuffer<float>& b, double sr,
                              const juce::StringPairArray& meta)
    {
        const int len = b.getNumSamples();

        if (len <= 0 || sr < 1000.0)
            return { 0, juce::jmax (0, len) };

        const int loopMin = (int) (kLoopMinSeg * sr);
        Loop lp = lerSmpl (meta, len);
        const bool doAutor = lp.inicio >= 0 && (lp.fim - lp.inicio) >= loopMin;

        if (! doAutor)
            lp = detectarPorEnvelope (b, sr);

        lp.fim    = juce::jlimit (1, len, lp.fim);
        lp.inicio = juce::jlimit (0, lp.fim - 1, lp.inicio);

        if (lp.fim - lp.inicio < loopMin)
            return { 0, len };

        AnaliseEmenda analise;

        if (! doAutor)
            analise = alinharLoopInferido (b, sr, loopMin, lp.inicio, lp.fim);

        int x = 0;

        if (doAutor)
        {
            // O autor continua mandando nos pontos. So uma descontinuidade
            // real recebe 20 ms de overlap; ponto ja casado fica byte intacto.
            float excessoDeDegrau = 0.0f;

            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                const float salto = std::abs (b.getSample (c, lp.inicio)
                                               - b.getSample (c, lp.fim - 1));
                const float derivadaNoInicio = lp.inicio + 1 < lp.fim
                    ? std::abs (b.getSample (c, lp.inicio + 1)
                                - b.getSample (c, lp.inicio))
                    : 0.0f;
                // Mede ANTES do ultimo frame: se ele proprio for o espigao da
                // emenda, usa-lo como "derivada normal" esconderia o defeito.
                const float derivadaNoFim = lp.fim >= 3
                    ? std::abs (b.getSample (c, lp.fim - 2)
                                - b.getSample (c, lp.fim - 3))
                    : 0.0f;
                const float derivadaLocal = juce::jmax (derivadaNoInicio,
                                                         derivadaNoFim);

                // Uma volta perfeita ainda tem a derivada normal entre duas
                // amostras. Tratar qualquer diferenca > 1e-4 como clique faria
                // todos os loops autorados receberem processamento desnecessario.
                excessoDeDegrau = juce::jmax (excessoDeDegrau,
                                               salto - 3.0f * derivadaLocal);
            }

            if (excessoDeDegrau >= 1.0e-4f)
                x = (int) std::round (kXfadeSmplSeg * sr);
        }
        else
        {
            const float fase = analise.valida ? analise.correlacaoDeFase : -1.0f;
            const double segundos = fase >= 0.65f ? kXfadeBomSeg
                                   : fase >= 0.30f ? kXfadeMedioSeg
                                                  : kXfadeRuimSeg;
            x = (int) std::round (segundos * sr);
        }

        x = juce::jmin (x, lp.inicio, (lp.fim - lp.inicio) / 2);

        if (x >= 16)
        {
            // Mede de novo no tamanho EXATO do overlap. A busca usa 120 ms,
            // mas a defesa de baixa correlacao pode ter escolhido 80/40 ms.
            analise = analisarEmenda (b, lp.inicio, lp.fim, x);
            lp.correlacao = analise.valida ? analise.correlacao : 0.0f;
            lp.correlacaoDeFase = analise.valida ? analise.correlacaoDeFase : 0.0f;
            lp.crossfade = x;
            gravarCrossfade (b, lp.inicio, lp.fim, x, lp.correlacao);
        }
        else if (analise.valida)
        {
            lp.correlacao = analise.correlacao;
            lp.correlacaoDeFase = analise.correlacaoDeFase;
        }

        return lp;
    }

    void run() override   // thread de fundo: carrega os WAV grandes sem travar o áudio
    {
        while (! threadShouldExit())
        {
            // Acorda imediatamente para pedidos de tom e, no maximo, a cada
            // 250 ms para recolher entradas cujo ultimo pino foi solto pelo
            // callback. O polling e apenas desta thread ociosa; evita tanto
            // signal()/syscall no audio quanto o sono infinito que deixaria
            // um WAV antigo preso ate a proxima troca de tom.
            loadEvent.wait (250);

            trimCacheToLimit();

            if (threadShouldExit())
                break;

            const int k = requestedKey.load();
            if (k >= 0)
                ensureLoaded (kLetter[k]);
        }
    }

    void ensureLoaded (char letter)
    {
        {
            const juce::ScopedLock sl (cacheLock);
            if (cache.count (letter) > 0)
                return;
        }

        auto file = padsFolder().getChildFile ("pad_" + juce::String::charToString (letter) + ".wav");
        if (! file.existsAsFile())
            return;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            return;

        const int chans = (int) juce::jmax (1u, reader->numChannels);
        const int frames = (int) juce::jmin ((juce::int64) (kMaxSeconds * (double) reader->sampleRate),
                                             reader->lengthInSamples);
        if (frames <= 0)
            return;

        auto buf = std::make_shared<juce::AudioBuffer<float>> (chans, frames);

        // le em blocos pra poder abortar: um read unico de ~100MB em midia
        // lenta estoura os 3s do stopThread e a thread morreria no meio
        for (int done = 0; done < frames;)
        {
            const int todo = juce::jmin (frames - done, kReadBlockFrames);
            reader->read (buf.get(), done, todo, (juce::int64) done, true, true);
            done += todo;

            if (threadShouldExit())
                return;   // shutdown: descarta o parcial
        }

        // Pontos de loop + crossfade: AQUI, na thread de carga, com o buffer
        // ainda privado. Depois que ele entra no cache uma voz pode pega-lo a
        // qualquer bloco, e escrever nele passaria a ser corrida de dados.
        const Loop lp = prepararLoop (*buf, (double) reader->sampleRate,
                                      reader->metadataValues);

        const juce::ScopedLock sl (cacheLock);
        if (cache.size() >= kMaxCached)        // limita RAM: guarda no máx. 2 pads
        {
            // remove um que não seja o pedido agora
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->first != kLetter[juce::jlimit (0, 11, requestedKey.load())]
                     && it->second.activeVoiceUsers.load (
                            std::memory_order_acquire) == 0)
                { cache.erase (it); break; }
        }

        auto [it, inserted] = cache.try_emplace (letter);
        if (inserted)
        {
            it->second.buffer = std::move (buf);
            it->second.sampleRate = (double) reader->sampleRate;
            it->second.loopStart = lp.inicio;
            it->second.loopEnd = lp.fim;
        }
    }

    void drainReleasePool()
    {
        trimCacheToLimit();
    }

    void trimCacheToLimit()
    {
        const juce::ScopedLock sl (cacheLock);
        const char requestedLetter =
            kLetter[juce::jlimit (0, 11, requestedKey.load())];

        while (cache.size() > kMaxCached)
        {
            auto removable = cache.end();

            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->first != requestedLetter
                     && it->second.activeVoiceUsers.load (
                            std::memory_order_acquire) == 0)
                {
                    removable = it;
                    break;
                }

            // As entradas restantes ainda pertencem a vozes ativas. A proxima
            // liberacao acorda esta thread e tenta novamente; jamais derrubar
            // a ultima referencia de um WAV dentro do callback de audio.
            if (removable == cache.end())
                break;

            cache.erase (removable);
        }
    }

   #if JUCE_ANDROID
    static constexpr double kMaxSeconds = 75.0;    // ~57 MB float32 max (evita swap/zram)
    static constexpr size_t kMaxCached  = 1;
   #elif JUCE_IOS
    static constexpr double kMaxSeconds = 90.0;    // ~69 MB por tom no iPhone/iPad
    static constexpr size_t kMaxCached  = 2;
   #else
    static constexpr double kMaxSeconds = 360.0;   // teto anti-OOM (6 min)
    static constexpr size_t kMaxCached  = 2;
   #endif
    static constexpr int    kReadBlockFrames = 1 << 17;   // 128k frames por read

    // Um polo TPT (topology-preserving transform). Guarda so o estado do
    // integrador; o coeficiente chega pronto a cada amostra, porque o corte
    // pode estar sendo varrido por um pedal.
    struct PoloTPT
    {
        float z = 0.0f;

        void reset() noexcept { z = 0.0f; }

        float processa (float x, float a) noexcept
        {
            const float v = (x - z) * a;
            const float y = v + z;
            z = y + v;

            // SANEAR AS DUAS PONTAS, nao so o estado.
            //
            // O Pad e um no do grafo: um NaN daqui contamina o master inteiro
            // e o culto fica sem som. Zerar apenas "z" protegia a proxima
            // amostra e mandava a suja para fora assim mesmo. Denormal quem
            // trata e o ScopedNoDenormals do processBlock — aqui o assunto e
            // NaN/infinito.
            if (! std::isfinite (z))
                z = 0.0f;

            if (! std::isfinite (y))
                return 0.0f;

            return y;
        }
    };

    juce::AudioFormatManager formatManager;
    double deviceSampleRate = 44100.0;

    juce::AudioParameterFloat* padCutoff = nullptr;
    juce::AudioParameterFloat* padVolume = nullptr;
    // Dois polos por canal = 12 dB/oitava. Um polo so (6 dB) mal fecha o pad:
    // o musico gira o knob inteiro e ainda ouve o brilho.
    std::array<PoloTPT, 4> filtro;    // [0][1] = esquerdo, [2][3] = direito
    float cutoffSuave = 19200.0f;     // so a thread de audio toca
    // Suavizacao do ganho de saida (mesma familia do corte): um CC de 7 bits
    // no volume do pad vira escada audivel sem isso — e o pad e um drone
    // tonal sem transiente pra mascarar os degraus.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> ganhoSuave;

    std::atomic<int>   requestedKey { -1 };
    std::atomic<float> padGain { 0.9f };
    std::atomic<int>   transposeSemis { 0 };
    int lastAppliedTransposeSemis = 0;
    double currentTransposeRatio = 1.0;
    int currentKey = -1;

    // Configuracao por tom. SO MESSAGE THREAD (painel + save/restore do chain);
    // a thread de audio nunca encosta aqui, entao nao precisa de lock nem de
    // atomico — e nao pode ganhar um, porque ha juce::String no caminho.
    std::array<MapeamentoDeTom, 12> mapeamentos;

    std::array<Voice, 4> voices;
    CaudaDeRoubo caudaDeRoubo;

    std::map<char, CacheEntry> cache;
    juce::CriticalSection cacheLock;

    juce::WaitableEvent loadEvent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadProcessor)
};
