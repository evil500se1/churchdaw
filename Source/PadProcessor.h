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
    static juce::File padsFolder()
    {
        // Nao basta a pasta EXISTIR: se um "Pads" errado aparecer primeiro (o
        // acervo de worship tem um), aceitar so pelo nome faria a busca parar
        // nele e o pad continuar "sem WAV". Exigir o pad_C confirma que e a
        // pasta certa e deixa a busca seguir.
        auto valid = [] (const juce::File& f)
        {
            return f.isDirectory() && f.getChildFile ("pad_C.wav").existsAsFile();
        };

        if (auto f = AppPaths::findLibrary ({ "Pads", "ChurchDAW Pads" }); valid (f))
            return f;

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
        return padsFolder().getChildFile ("pad_C.wav").existsAsFile();
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

    void setGain (float g) noexcept     { padGain.store (juce::jlimit (0.0f, 1.5f, g)); }
    float getGain() const noexcept      { return padGain.load(); }

    //==============================================================================
    void prepareToPlay (double sampleRate, int) override
    {
        deviceSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

        const juce::ScopedLock sl (voiceLock);
        for (auto& v : voices) v = {};
        currentKey = -1;   // esquece o tom: forca re-disparo do cache no proximo bloco (rate depende do sample rate novo)
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();

        const int numSamples = buffer.getNumSamples();
        const float outGain = padGain.load();
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
    void getStateInformation (juce::MemoryBlock&) override  {}
    void setStateInformation (const void*, int) override    {}

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
    }   // "local" morre aqui e libera os buffers ja sem lock

    static constexpr double kMaxSeconds = 360.0;   // teto anti-OOM (6 min)
    static constexpr size_t kMaxCached  = 2;
    static constexpr int    kReadBlockFrames = 1 << 17;   // 128k frames por read

    juce::AudioFormatManager formatManager;
    double deviceSampleRate = 44100.0;

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
