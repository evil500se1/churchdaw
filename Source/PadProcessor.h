#pragma once

#include "AppPaths.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <map>
#include <memory>

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

        formatManager.registerBasicFormats();
        startThread();
    }

    ~PadProcessor() override
    {
        signalThreadShouldExit();
        loadEvent.signal();
        stopThread (3000);
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
    void prepareToPlay (double sampleRate, int) override
    {
        deviceSampleRate = sampleRate >= 1000.0 ? sampleRate : 44100.0;

        const juce::ScopedLock sl (voiceLock);
        for (auto& v : voices) v = {};
        currentKey = -1;   // esquece o tom: forca re-disparo do cache no proximo bloco (rate depende do sample rate novo)

        for (auto& f : filtro)
            f.reset();

        cutoffSuave = juce::jmin (cutoffHz(),
                                  0.45f * (float) deviceSampleRate);
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
        const float outGain = padVolume != nullptr
                                ? juce::jlimit (0.0f, 1.5f, padVolume->get())
                                : padGain.load();

        const float fadeInc = 1.0f / (float) (juce::jmax (1.0, kFadeSeconds * deviceSampleRate));

        const juce::ScopedLock sl (voiceLock);

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
                std::shared_ptr<juce::AudioBuffer<float>> buf;
                double srcSr = 44100.0;

                if (cacheLock.tryEnter())
                {
                    auto it = cache.find (letter);
                    if (it != cache.end()) { buf = it->second.buffer; srcSr = it->second.sampleRate; }
                    cacheLock.exit();
                }

                if (buf != nullptr && buf->getNumSamples() > 0)
                {
                    for (auto& v : voices) v.target = 0.0f;   // some os antigos

                    if (auto* slot = freeVoice())
                    {
                        slot->buffer = buf;
                        slot->pos = 0.0;
                        slot->rate = (srcSr / deviceSampleRate)
                                     * std::pow (2.0, (double) kSemis[req] / 12.0);
                        slot->gain = 0.0f;
                        slot->target = 1.0f;
                        slot->active = true;
                    }
                    currentKey = req;   // só marca como atendido quando o buffer chegou
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
            const float* srcL = b.getReadPointer (0);
            const float* srcR = b.getNumChannels() > 1 ? b.getReadPointer (1) : srcL;

            for (int n = 0; n < numSamples; ++n)
            {
                // envelope de fade (in/out) suave
                if (v.gain < v.target)      v.gain = juce::jmin (v.target, v.gain + fadeInc);
                else if (v.gain > v.target) v.gain = juce::jmax (v.target, v.gain - fadeInc);

                const int i0 = (int) v.pos;
                int i1 = i0 + 1; if (i1 >= len) i1 = 0;
                const float frac = (float) (v.pos - (double) i0);

                const float l = srcL[i0] + (srcL[i1] - srcL[i0]) * frac;
                const float r = srcR[i0] + (srcR[i1] - srcR[i0]) * frac;

                const float g = v.gain * outGain;
                outL[n] += l * g;
                if (outR != nullptr) outR[n] += r * g;

                v.pos += v.rate;
                if (v.pos >= (double) len) v.pos -= (double) len;
            }

            if (v.target <= 0.0f && v.gain <= 0.0001f)   // terminou de sumir: libera
            {
                // nao solta a possivel ultima referencia aqui (free gigante
                // dentro do callback): estaciona no pool (ja sob voiceLock)
                // e o loader descarta no run()
                if (releaseCount < (int) releasePool.size())
                    releasePool[(size_t) releaseCount++] = std::move (v.buffer);
                v = {};
                loadEvent.signal();
            }
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

        for (int n = 0; n < numSamples; ++n)
        {
            cutoffSuave += (alvoSeguro - cutoffSuave) * coef;

            const float g = std::tan (juce::MathConstants<float>::pi
                                        * juce::jlimit (20.0f, tetoHz, cutoffSuave)
                                        / (float) deviceSampleRate);
            const float a = g / (1.0f + g);

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

        // O tom ativo e estado de execucao, como uma nota MIDI sustentada:
        // restaura-lo faria o app voltar tocando sozinho depois de reiniciar.
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

private:
    static constexpr double kFadeSeconds = 1.6;   // crossfade entre tons

    // tom (0..11) -> letra natural do arquivo + semitons de afinação
    static constexpr char kLetter[12] = { 'C','C','D','D','E','F','F','G','G','A','A','B' };
    static constexpr int  kSemis [12] = {  0 , 1 , 0 , 1 , 0 , 0 , 1 , 0 , 1 , 0 , 1 , 0  };

    struct Voice
    {
        std::shared_ptr<juce::AudioBuffer<float>> buffer;
        double pos = 0.0, rate = 1.0;
        float gain = 0.0f, target = 0.0f;
        bool active = false;
    };

    Voice* freeVoice()
    {
        for (auto& v : voices)
            if (! v.active) return &v;

        // nenhuma livre: reusa a de menor ganho (a que está sumindo)
        Voice* q = &voices[0];
        for (auto& v : voices) if (v.gain < q->gain) q = &v;
        return q;
    }

    struct CacheEntry { std::shared_ptr<juce::AudioBuffer<float>> buffer; double sampleRate = 44100.0; };

    void run() override   // thread de fundo: carrega os WAV grandes sem travar o áudio
    {
        while (! threadShouldExit())
        {
            loadEvent.wait (-1);

            drainReleasePool();

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

        const juce::ScopedLock sl (cacheLock);
        if (cache.size() >= kMaxCached)        // limita RAM: guarda no máx. 2 pads
        {
            // remove um que não seja o pedido agora
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->first != kLetter[juce::jlimit (0, 11, requestedKey.load())]
                     && it->second.buffer.use_count() <= 1)   // nao evicta pad que uma voz ainda segura
                { cache.erase (it); break; }
        }
        cache[letter] = { buf, (double) reader->sampleRate };
    }

    void drainReleasePool()
    {
        // move os ponteiros pra fora antes de soltar: o free de ~100MB nao
        // pode rodar com o voiceLock preso (seguraria o processBlock)
        decltype (releasePool) local;

        {
            const juce::ScopedLock sl (voiceLock);
            for (int i = 0; i < releaseCount; ++i)
                local[(size_t) i] = std::move (releasePool[(size_t) i]);
            releaseCount = 0;
        }

        // Libera as referencias das vozes na thread de carga e, so depois,
        // apara entradas que ficaram retidas no cache enquanto ainda havia
        // crossfade. Sem esta segunda etapa o limite de dois pads era apenas
        // temporario: se o musico parasse de trocar o tom, 3..5 WAVs grandes
        // podiam ficar na RAM ate fechar o app.
        for (auto& buffer : local)
            buffer.reset();

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
                     && it->second.buffer.use_count() <= 1)
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

    static constexpr double kMaxSeconds = 360.0;   // teto anti-OOM (6 min)
    static constexpr size_t kMaxCached  = 2;
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

    std::atomic<int>   requestedKey { -1 };
    std::atomic<float> padGain { 0.9f };
    int currentKey = -1;

    std::array<Voice, 4> voices;
    juce::CriticalSection voiceLock;

    // pool fixo (sem alocacao na thread de audio) de buffers descartados pelo
    // processBlock; protegido pelo voiceLock, esvaziado pelo loader no run()
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, 8> releasePool;
    int releaseCount = 0;

    std::map<char, CacheEntry> cache;
    juce::CriticalSection cacheLock;

    juce::WaitableEvent loadEvent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadProcessor)
};
