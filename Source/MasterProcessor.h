#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>

#include "StereoLookaheadLimiter.h"

//==============================================================================
// No de master do grafo: aplica o volume geral (com suavizacao) e mede os
// niveis de pico de cada canal para os medidores da interface.
// O volume e um parametro (mapeavel via MIDI Learn) e as mudancas sao
// anunciadas via ChangeBroadcaster para sincronizar todos os faders na tela.
//==============================================================================
class MasterProcessor : public juce::AudioProcessor,
                        public juce::ChangeBroadcaster
{
public:
    static constexpr float minusInfinityDb = -60.0f;
    static constexpr float maxGainDb = 10.0f;

    MasterProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput ("Entrada", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Saida", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (dbParam = new juce::AudioParameterFloat (
            { "masterdb", 1 }, "Master (dB)",
            juce::NormalisableRange<float> (minusInfinityDb, maxGainDb, 0.1f), 0.0f));
    }

    void setMasterDb (float db) noexcept
    {
        if (! std::isfinite (db)) return;
        *dbParam = juce::jlimit (minusInfinityDb, maxGainDb, db);
        sendChangeMessage();
    }

    float getMasterDb() const noexcept          { return (float) *dbParam; }

    // Auto EQ de masterizacao (mix de louvor): sorriso sutil no bus inteiro
    void setAutoEq (bool on) noexcept
    {
        if (autoEqOn.exchange (on) != on)
            sendChangeMessage();
    }

    bool getAutoEq() const noexcept             { return autoEqOn.load(); }

    // Punch / Peso (worship): peso no grave + compressao paralela saturada,
    // com anti-mud, para dar corpo e soco sem embolar
    void setPunch (bool on) noexcept
    {
        if (punchOn.exchange (on) != on)
            sendChangeMessage();
    }

    bool getPunch() const noexcept              { return punchOn.load(); }

    // Diagnostico de regressao: a rampa de 5 ms precisa avancar por amostra,
    // nao uma unica vez por callback.
    float getPunchMakeupCurrentForTest() const noexcept
    {
        return punchMakeup.getCurrentValue();
    }

    // UMA CAIXA DE NIVEL POR MEDIDOR -- ver readAndResetLevel().
    enum LevelReader
    {
        levelReaderChain  = 0,   // regua da tela da cadeia (MainComponent)
        levelReaderMaster = 1,   // regua do master do editor / Church Live
        numLevelReaders
    };

    // o medidor le e zera; a queda suave fica por conta da interface.
    //
    // "Le e zera" entrega o maior pico desde a consulta ANTERIOR DAQUELE leitor,
    // e e justamente por isso que nenhum quadro se perde -- mas e contrato de
    // leitor UNICO. E existem DOIS medidores de master vivos ao mesmo tempo: a
    // regua da tela da cadeia e a do Church Live. Com o editor de Timbres
    // DOCADO os dois ficam visiveis, e cada um levava so parte dos picos: barra
    // tremida e, pior, SUBESTIMANDO o nivel -- que e o erro perigoso, porque o
    // operador confia no medidor pra empurrar volume.
    //
    // A saida e dar a cada medidor a SUA caixa: a thread de audio deposita o
    // mesmo pico em todas, cada leitor zera so a sua, e a balistica de cada um
    // fica identica a de antes. Custa um punhado de CAS por bloco.
    float readAndResetLevel (int channel, int reader = levelReaderChain) noexcept
    {
        return levels[juce::jlimit (0, 1, channel)]
                     [juce::jlimit (0, (int) numLevelReaders - 1, reader)]
                   .exchange (0.0f);
    }

    // ALERTA TRAVADO DE SOBRECARGA -- leitura NAO destrutiva (varios medidores).
    //
    // Nao e "passou de 0 dBFS". Passar de 0 dBFS na ENTRADA do limiter e o
    // regime NORMAL de trabalho dele: o brick-wall existe exatamente pra isso e
    // a saida sai garantida em -1 dBFS (StereoLookaheadLimiter::outputCeilingDb
    // e o cinto de seguranca por amostra) -- ou seja, a igreja nao ouviu
    // distorcao nenhuma. Travar um alerta vermelho nisso o acenderia no primeiro
    // louvor forte e o deixaria aceso o culto inteiro: alarme permanente e
    // alarme nenhum, e o musico aprende a ignorar a luz que importa.
    //
    // O que MERECE trava aqui e gain-staging quebrado: +12 dBFS chegando no
    // limiter significa mais de 12 dB de reducao continua, mistura achatada.
    // Isso pode acontecer entre uma musica e outra e ninguem ver, entao a trava
    // espera o clique do musico no medidor (ver LevelMeter::setClipClearFn).
    // Sendo "le sem zerar", quantos medidores existirem consultam sem disputar.
    static constexpr float overloadThresholdGain = 4.0f;   // +12 dBFS

    bool getClipLatched() const noexcept        { return clipLatched.load (std::memory_order_relaxed); }
    void clearClipLatch() noexcept              { clipLatched.store (false, std::memory_order_relaxed); }

    float getLimiterGainReductionDb() const noexcept
    {
        return reductionDbMirror.load (std::memory_order_relaxed);
    }

    uint64_t getLimiterEmergencyCorrectionCount() const noexcept
    {
        return emergencyCorrectionsMirror.load (std::memory_order_relaxed);
    }

    // Contador de disparos da guarda NaN do master (chip AUDIO ENGINE). Analogo
    // ao nanGuardHits do TimbresProcessor, porem atomic para leitura fora da
    // thread de audio.
    uint64_t getMasterNanGuardHits() const noexcept
    {
        return nanGuardHits.load (std::memory_order_relaxed);
    }

    //==============================================================================
    const juce::String getName() const override { return "Master"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        gain.reset (sampleRate, 0.05);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (getMasterDb(), minusInfinityDb));

        autoEqMix.reset (sampleRate, 0.010);
        autoEqMix.setCurrentAndTargetValue (autoEqOn.load() ? 1.0f : 0.0f);
        autoEqDspBypassed = false;

        punchMix.reset (sampleRate, 0.005);
        punchMix.setCurrentAndTargetValue (punchOn.load() ? 1.0f : 0.0f);
        punchMakeup.reset (sampleRate, 0.005);
        punchMakeup.setCurrentAndTargetValue (punchOn.load() ? 1.0f : 0.0f);
        punchDspBypassed = false;

        // A trava de sobrecarga espera o clique do musico -- mas nao pode
        // atravessar uma TROCA DE DISPOSITIVO ou o reinicio do motor: ali o
        // sinal de antes nem existe mais, e o vermelho ficaria acusando um
        // evento de outra sessao. Os picos acumulados tambem morrem aqui, senao
        // o primeiro quadro depois da troca mostraria um spike do que passou.
        clipLatched.store (false, std::memory_order_relaxed);

        for (int ch = 0; ch < 2; ++ch)
            for (int r = 0; r < numLevelReaders; ++r)
                levels[ch][r].store (0.0f, std::memory_order_relaxed);

        // EQ de masterizacao worship (sorriso sutil, ±1-2 dB, Q largo):
        // peso no grave, tira lama, dominio nos medios asperos, ar no agudo
        using C = juce::dsp::IIR::Coefficients<float>;
        auto dB = [] (float d) { return juce::Decibels::decibelsToGain (d); };

        // Teto relativo a Nyquist (nao absoluto) para o shelf de agudo: acima de
        // sr/2 o biquad RBJ permanece estavel mas espelha a resposta para o alias
        // (sr - fc). 0.45*sr deixa margem e em 44.1/48 kHz nao altera o shelf.
        const double ny = juce::jmin (sampleRate * 0.45, 11000.0);

        juce::dsp::IIR::Coefficients<float>::Ptr coeffs[numBands] = {
            C::makeLowShelf  (sampleRate, 110.0f,   0.7f, dB (1.5f)),   // peso/calor
            C::makePeakFilter (sampleRate, 450.0f,  1.0f, dB (-1.5f)),  // tira lama
            C::makePeakFilter (sampleRate, 3000.0f, 1.2f, dB (-1.0f)),  // suaviza aspereza
            C::makeHighShelf (sampleRate, (float) juce::jmin (11000.0, ny), 0.7f, dB (2.0f))    // ar/brilho
        };

        const int block = juce::jmax (8192, samplesPerBlock);
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) block, 1 };
        autoEqBuf.setSize (2, block, false, false, true);

        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < numBands; ++b)
            {
                eq[ch][b].prepare (spec);
                *eq[ch][b].coefficients = *coeffs[b];
                eq[ch][b].reset();
            }

        // ---- cadeia de Punch / Peso ----
        punchBuf.setSize (2, block, false, false, true);
        punchParallelBuf.setSize (2, block, false, false, true);

        // peso no grave + limpeza da lama (para nao embolar)
        juce::dsp::IIR::Coefficients<float>::Ptr wc[2] = {
            C::makeLowShelf  (sampleRate, 90.0f,  0.7f, dB (3.0f)),   // peso/corpo
            C::makePeakFilter (sampleRate, 320.0f, 1.2f, dB (-2.0f))  // tira a lama
        };

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int b = 0; b < 2; ++b)
            {
                weightEq[ch][b].prepare (spec);
                *weightEq[ch][b].coefficients = *wc[b];
                weightEq[ch][b].reset();
            }

            // HPF no caminho paralelo: o sub nao dispara o compressor
            punchHp[ch].prepare (spec);
            *punchHp[ch].coefficients = *C::makeHighPass (sampleRate, 130.0f);
            punchHp[ch].reset();
        }

        const juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) block, 2 };
        punchComp.prepare (stereoSpec);
        punchComp.setThreshold (-21.0f);
        punchComp.setRatio (3.5f);
        punchComp.setAttack (12.0f);
        punchComp.setRelease (140.0f);

        masterLimiter.prepare (sampleRate, block);
        preparedSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        silentSamples = 0;
        masterDspSleeping = false;
        reductionDbMirror.store (0.0f, std::memory_order_relaxed);
        emergencyCorrectionsMirror.store (0, std::memory_order_relaxed);
        setLatencySamples (masterLimiter.getLatencySamples());
    }

    void releaseResources() override
    {
        masterLimiter.reset();
        silentSamples = 0;
        masterDspSleeping = false;
        reductionDbMirror.store (0.0f, std::memory_order_relaxed);
        emergencyCorrectionsMirror.store (0, std::memory_order_relaxed);
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int numSamples = buffer.getNumSamples();

        // Protecao NaN/sobrecarga: VST3 de terceiro nos slots FX (effect_to_master)
        // e a entrada de audio ao vivo (device_input_to_chain) chegam AQUI, depois
        // da unica guarda que existe no projeto (dentro do TimbresProcessor).
        // Um unico NaN contamina o estado dos biquads IIR realimentados da cadeia
        // de master (Auto EQ, weightEq, punchHp, punchComp) e o master fica mudo
        // ate reiniciar -- PANIC, troca de canal/cena/timbre nao traz o som de
        // volta, pois o defeito mora no estado dos filtros deste no. A soma
        // detecta NaN (getMagnitude engole NaN no meio do buffer).
        //
        // SOMENTE NaN emudece. Nivel alto porem FINITO nao pode zerar o buffer:
        // esta guarda esta a montante do fader de master e do limiter brick-wall,
        // entao um erro de gain-staging num plugin de FX (makeup exagerado,
        // realimentacao de microfone) derrubaria a mistura INTEIRA e puxar o
        // fader nao resolveria. Ver o tratamento de sobrecarga logo abaixo.
        float emergencyPeak = 0.0f;
        bool nonFiniteInput = false;

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const float* chk = buffer.getReadPointer (ch);
            double sum = 0.0; // finite float samples cannot overflow this detector
            float channelPeak = 0.0f;

            for (int s = 0; s < numSamples; ++s)
            {
                sum += chk[s];
                channelPeak = juce::jmax (channelPeak, std::abs (chk[s]));
            }

            if (! std::isfinite (sum))
            {
                nanGuardHits.fetch_add (1, std::memory_order_relaxed);
                buffer.clear();
                nonFiniteInput = true;

                // NaN preso no estado dos filtros silenciaria o master para sempre.
                for (int b = 0; b < numBands; ++b)
                    for (int c = 0; c < 2; ++c)
                        eq[c][b].reset();

                for (int b = 0; b < 2; ++b)
                    for (int c = 0; c < 2; ++c)
                        weightEq[c][b].reset();

                for (int c = 0; c < 2; ++c)
                    punchHp[c].reset();

                punchComp.reset();
                masterLimiter.reset();

                break;
            }

            emergencyPeak = juce::jmax (emergencyPeak, channelPeak);
        }

        // SOBRECARGA (finita): so um teto de emergencia, sem emudecer e sem
        // resetar filtro nenhum. Acima de 24.0 linear (+27,6 dBFS) o sinal ja e
        // erro de gain-staging; atenuar mantem a mistura audivel e deixa o
        // limiter brick-wall fazer o acabamento (e o contador de correcoes de
        // emergencia dele continua sendo o diagnostico exposto na tela).
        // O pico e acumulado na mesma passagem escalar que a soma anti-NaN
        // acima. Isso retira uma segunda leitura completa do buffer em todo
        // callback sem mudar nenhuma amostra do caminho normal.
        if (! nonFiniteInput && emergencyPeak > 24.0f)
            buffer.applyGain (24.0f / emergencyPeak);

        // Poupanca de CPU em silencio continuo: apos 350 ms de entrada abaixo de
        // -140 dBFS sem nenhuma rampa ativa, caudas IIR e lookahead do limiter ja
        // decairam totalmente. Dormir aqui tira 14 biquads + compressor + limiter
        // do orcamento da thread de audio no Android/iOS quando nenhuma nota soa.
        {
            autoEqMix.setTargetValue (autoEqOn.load() ? 1.0f : 0.0f);
            punchMix.setTargetValue (punchOn.load() ? 1.0f : 0.0f);
            gain.setTargetValue (juce::Decibels::decibelsToGain (getMasterDb(), minusInfinityDb));
            punchMakeup.setTargetValue (punchOn.load() ? 1.0f : 0.0f);

            const bool anySmoothing = autoEqMix.isSmoothing()
                                   || punchMix.isSmoothing()
                                   || gain.isSmoothing()
                                   || punchMakeup.isSmoothing();

            if (! nonFiniteInput && ! anySmoothing && emergencyPeak <= 1.0e-7f)
            {
                const int sleepThresholdSamples =
                    juce::jmax (2048, (int) (preparedSampleRate * 0.35));
                silentSamples = juce::jmin (sleepThresholdSamples, silentSamples + numSamples);

                if (silentSamples >= sleepThresholdSamples)
                {
                    if (! masterDspSleeping)
                    {
                        for (int b = 0; b < numBands; ++b)
                            for (int c = 0; c < 2; ++c)
                                eq[c][b].reset();
                        for (int b = 0; b < 2; ++b)
                            for (int c = 0; c < 2; ++c)
                                weightEq[c][b].reset();
                        for (int c = 0; c < 2; ++c)
                            punchHp[c].reset();
                        punchComp.reset();
                        masterLimiter.reset();
                        reductionDbMirror.store (0.0f, std::memory_order_relaxed);
                        masterDspSleeping = true;
                    }

                    buffer.clear();
                    return;
                }
            }
            else
            {
                silentSamples = 0;
                masterDspSleeping = false;
            }
        }

        // Auto EQ de masterizacao (antes do ganho). O wet roda sempre para que
        // os IIR acompanhem o sinal inclusive no bypass; congelar o estado aqui
        // fazia uma cauda antiga reaparecer quando o usuario religava o EQ.
        // Somente a mistura dry/wet muda, com uma rampa compartilhada por L/R.
        {
            const int chs = juce::jmin (2, buffer.getNumChannels());
            const int scratchCapacity = autoEqBuf.getNumSamples();
            const bool autoEqEnabled = autoEqOn.load();
            autoEqMix.setTargetValue (autoEqEnabled ? 1.0f : 0.0f);

            const bool fullyBypassed = ! autoEqEnabled
                                     && ! autoEqMix.isSmoothing()
                                     && autoEqMix.getCurrentValue() == 0.0f;

            if (fullyBypassed)
            {
                if (! autoEqDspBypassed)
                {
                    // Mix exatamente zero: o wet nao alcanca a saida. Zerar o
                    // estado uma vez impede cauda antiga ao religar e evita
                    // oito biquads, duas copias e a mistura por amostra. Nao
                    // repita oito resets em todo callback durante o bypass.
                    for (int b = 0; b < numBands; ++b)
                        for (int ch = 0; ch < 2; ++ch)
                            eq[ch][b].reset();
                    autoEqDspBypassed = true;
                }
            }
            else
            {
                autoEqDspBypassed = false;

                for (int offset = 0;
                     chs > 0 && scratchCapacity > 0 && offset < numSamples;
                     offset += scratchCapacity)
                {
                    const int count =
                        juce::jmin (scratchCapacity, numSamples - offset);

                    for (int ch = 0; ch < chs; ++ch)
                    {
                        autoEqBuf.copyFrom (
                            ch, 0, buffer, ch, offset, count);
                        float* wet = autoEqBuf.getWritePointer (ch);

                        for (int b = 0; b < numBands; ++b)
                            for (int s = 0; s < count; ++s)
                                wet[s] = eq[ch][b].processSample (wet[s]);
                    }

                    for (int s = 0; s < count; ++s)
                    {
                        const float mix = autoEqMix.getNextValue();

                        for (int ch = 0; ch < chs; ++ch)
                        {
                            float* dry = buffer.getWritePointer (ch, offset);
                            dry[s] += (autoEqBuf.getSample (ch, s) - dry[s]) * mix;
                        }
                    }
                }
            }
        }

        // Punch / Peso: compressao paralela saturada + peso no grave + anti-mud.
        //
        // A MISTURA e suavizada por punchMix. Enquanto a rampa esta andando, o
        // caminho wet continua inteiro; somente depois de chegar exatamente a
        // zero ele e resetado uma vez e dorme. Ao religar, os 5 ms de crossfade
        // escondem o aquecimento dos IIR sem manter HPF, compressor, saturador e
        // quatro biquads trabalhando durante minutos de bypass.
        {
            const int chs = juce::jmin (2, buffer.getNumChannels());
            const int scratchCapacity = juce::jmin (
                punchBuf.getNumSamples(),
                punchParallelBuf.getNumSamples());

            const bool punchEnabled = punchOn.load();
            punchMix.setTargetValue (punchEnabled ? 1.0f : 0.0f);
            const bool fullyWet = punchEnabled
                               && ! punchMix.isSmoothing()
                               && punchMix.getCurrentValue() == 1.0f;
            const bool fullyBypassed = ! punchEnabled
                                    && ! punchMix.isSmoothing()
                                    && punchMix.getCurrentValue() == 0.0f;

            if (fullyBypassed)
            {
                if (! punchDspBypassed)
                {
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        punchHp[ch].reset();
                        for (int b = 0; b < 2; ++b)
                            weightEq[ch][b].reset();
                    }
                    punchComp.reset();
                    punchDspBypassed = true;
                }
            }
            else
            {
                punchDspBypassed = false;

                // O driver pode entregar ocasionalmente um bloco maior que o
                // informado no prepareToPlay. Processamos em fatias do scratch ja
                // reservado; nunca redimensionamos/alocamos na thread de audio.
                for (int offset = 0;
                     chs > 0 && scratchCapacity > 0 && offset < numSamples;
                     offset += scratchCapacity)
                {
                    const int count =
                        juce::jmin (scratchCapacity, numSamples - offset);

                    // caminho paralelo: copia + HPF (tira sub do compressor)
                    for (int ch = 0; ch < chs; ++ch)
                    {
                        if (! fullyWet)
                            punchBuf.copyFrom (
                                ch, 0, buffer, ch, offset, count);
                        punchParallelBuf.copyFrom (
                            ch, 0, buffer, ch, offset, count);
                        float* p = punchParallelBuf.getWritePointer (ch);

                        for (int s = 0; s < count; ++s)
                            p[s] = punchHp[ch].processSample (p[s]);
                    }

                    if (chs == 1)
                        punchParallelBuf.copyFrom (1, 0, punchParallelBuf, 0, 0, count);

                    // compressao forte no paralelo (soco)
                    {
                        juce::dsp::AudioBlock<float> blk (
                            punchParallelBuf.getArrayOfWritePointers(),
                            2, (size_t) count);
                        juce::dsp::ProcessContextReplacing<float> ctx (blk);
                        punchComp.process (ctx);
                    }

                    // Compressao/saturacao paralela e EQ de peso pertencem ao wet.
                    for (int ch = 0; ch < chs; ++ch)
                    {
                        const float* parallel =
                            punchParallelBuf.getReadPointer (ch);
                        float* wet = fullyWet
                            ? buffer.getWritePointer (ch, offset)
                            : punchBuf.getWritePointer (ch);

                        for (int s = 0; s < count; ++s)
                        {
                            const float driven = juce::jlimit (
                                -5.0f, 5.0f, parallel[s] * 1.95f);

                            // Padé de alta precisão: erro máximo abaixo de -120 dB
                            // no intervalo musical [-3, 3]. Evita a chamada escalar
                            // cara de libm por amostra e mantém a mesma curva tanh;
                            // fora de [-5, 5] o saturador já é indistinguível de 1.
                            wet[s] += juce::dsp::FastMathApproximations::tanh (driven)
                                    * 0.45f;
                        }
                    }

                    for (int ch = 0; ch < chs; ++ch)
                    {
                        float* wet = fullyWet
                            ? buffer.getWritePointer (ch, offset)
                            : punchBuf.getWritePointer (ch);

                        for (int b = 0; b < 2; ++b)
                            for (int s = 0; s < count; ++s)
                                wet[s] = weightEq[ch][b].processSample (wet[s]);
                    }

                    // Um valor por AMOSTRA, compartilhado por L/R. Antes a rampa
                    // percorria primeiro todo L e depois todo R, deslocando o
                    // centro estereo e soando como modulacao/rotary.
                    if (fullyWet)
                    {
                        punchMix.skip (count);
                    }
                    else
                    {
                        for (int s = 0; s < count; ++s)
                        {
                            const float mix = punchMix.getNextValue();

                            for (int ch = 0; ch < chs; ++ch)
                            {
                                float* dry = buffer.getWritePointer (ch, offset);
                                const float wet = punchBuf.getSample (ch, s);
                                dry[s] += (wet - dry[s]) * mix;
                            }
                        }
                    }
                }
            }
        }

        gain.setTargetValue (juce::Decibels::decibelsToGain (getMasterDb(), minusInfinityDb));
        // A rampa termina na amostra prevista, mesmo quando um callback cruza
        // os 50 ms. skip()+applyGainRamp esticava o trecho restante ate o fim
        // do bloco e fazia o fader soar diferente conforme o buffer do driver.
        // A sobrecarga de AudioBuffer compartilha cada valor entre L/R.
        gain.applyGain (buffer, numSamples);

        // Quando o controle principal está no máximo, o caminho de retorno do
        // punch tende a ficar menos presente em salas pequenas. Esta etapa de
        // "pré-master" adiciona densidade sem destruir headroom do limiter.
        // Rampeada para nao saltar de degrau ao ligar/desligar o punch.
        punchMakeup.setTargetValue (punchOn.load() ? 1.0f : 0.0f);
        if (punchMakeup.isSmoothing())
        {
            // O smoother esta em dB: converter so os extremos e interpolar
            // ganho linear tambem mudava a curva entre tamanhos de callback.
            // Apenas as amostras da transicao pagam esta conversao; depois
            // dela o ganho constante volta ao caminho vetorial.
            const int rampSamples = juce::jmin (
                numSamples, punchMakeup.getNumSamplesToReachTarget());
            for (int sample = 0; sample < rampSamples; ++sample)
            {
                const float makeup = juce::Decibels::decibelsToGain (
                    punchMakeup.getNextValue());
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    buffer.getWritePointer (channel)[sample] *= makeup;
            }
            if (rampSamples < numSamples)
                buffer.applyGain (rampSamples, numSamples - rampSamples,
                    juce::Decibels::decibelsToGain (punchMakeup.getCurrentValue()));
        }
        else
        {
            buffer.applyGain (juce::Decibels::decibelsToGain (
                punchMakeup.getCurrentValue()));
        }

        // SOBRECARGA DE GAIN-STAGING, medida na entrada do limiter (ver
        // getClipLatched). O teste NAO e "> 1.0": passar de 0 dBFS aqui e o
        // trabalho normal do brick-wall, que devolve a saida em -1 dBFS. Acima
        // de +12 dBFS ja nao e mais trabalho, e erro de nivel a montante.
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            if (buffer.getMagnitude (ch, 0, numSamples) > overloadThresholdGain)
                clipLatched.store (true, std::memory_order_relaxed);

        // Ultimo estagio antes da saida fisica. O lookahead stereo-linked
        // preserva o ataque do piano e impede que Return/Master no maximo
        // virem clipping digital/chiado no DAC do Android.
        masterLimiter.process (buffer);

        // Espelho atomic da reducao de ganho do limiter: leitura segura na
        // thread de UI (getCurrentGainReductionDb() e valido apenas no callback
        // de audio). Sem isto, o chip AUDIO ENGINE nao consegue expor o quanto
        // a mistura esta sendo esmagada pelo limiter.
        reductionDbMirror.store (masterLimiter.getCurrentGainReductionDb(),
                                 std::memory_order_relaxed);
        emergencyCorrectionsMirror.store (masterLimiter.getEmergencyCorrectionCount(),
                                          std::memory_order_relaxed);

        // O MESMO pico vai pra caixa de cada medidor: assim os dois medidores de
        // master (cadeia e Church Live) podem estar visiveis ao mesmo tempo sem
        // um roubar o pico do outro. Ver readAndResetLevel().
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const float peak = buffer.getMagnitude (ch, 0, numSamples);

            for (int r = 0; r < numLevelReaders; ++r)
            {
                float current = levels[ch][r].load();
                while (peak > current && ! levels[ch][r].compare_exchange_weak (current, peak)) {}
            }
        }
    }

    //==============================================================================
    double getTailLengthSeconds() const override                { return 0.0; }
    bool acceptsMidi() const override                           { return false; }
    bool producesMidi() const override                          { return false; }
    juce::AudioProcessorEditor* createEditor() override         { return nullptr; }
    bool hasEditor() const override                             { return false; }
    int getNumPrograms() override                               { return 1; }
    int getCurrentProgram() override                            { return 0; }
    void setCurrentProgram (int) override                       {}
    const juce::String getProgramName (int) override            { return {}; }
    void changeProgramName (int, const juce::String&) override  {}
    void getStateInformation (juce::MemoryBlock&) override      {}
    void setStateInformation (const void*, int) override        {}

private:
    static constexpr int numBands = 4;

    juce::AudioParameterFloat* dbParam = nullptr;
    // [canal][leitor] -- uma caixa por medidor, ver readAndResetLevel()
    std::atomic<float> levels[2][numLevelReaders] {};
    // Trava de sobrecarga, para leitura nao destrutiva (ver getClipLatched)
    std::atomic<bool> clipLatched { false };
    std::atomic<bool> autoEqOn { false };
    // Novos setups ja abrem com o corpo worship calibrado. Projetos salvos
    // continuam restaurando explicitamente a escolha do usuario.
    std::atomic<bool> punchOn { false };
    // Espelho atomic da reducao de ganho do limiter para a thread de UI, e
    // contador de disparos da guarda NaN (chip AUDIO ENGINE).
    std::atomic<float> reductionDbMirror { 0.0f };
    std::atomic<uint64_t> emergencyCorrectionsMirror { 0 };
    std::atomic<uint64_t> nanGuardHits { 0 };
    juce::dsp::IIR::Filter<float> eq[2][numBands];
    juce::AudioBuffer<float> autoEqBuf;
    juce::SmoothedValue<float> autoEqMix { 0.0f };
    bool autoEqDspBypassed = false;
    juce::dsp::IIR::Filter<float> weightEq[2][2], punchHp[2];
    juce::dsp::Compressor<float> punchComp;
    juce::AudioBuffer<float> punchBuf, punchParallelBuf;
    bool punchDspBypassed = false;
    juce::SmoothedValue<float> gain { 1.0f };
    // Quantidade da mistura paralela do punch, suavizada. Durante a transicao o
    // wet continua andando; em bypass completo o DSP dorme e volta pela mesma
    // rampa, sem "tec" ao ligar/desligar no meio da musica.
    juce::SmoothedValue<float> punchMix { 0.0f };
    // Makeup de +1 dB do punch, tambem rampeado (era um applyGain condicional
    // que saltava de degrau ao ligar/desligar).
    juce::SmoothedValue<float> punchMakeup { 0.0f };
    StereoLookaheadLimiter masterLimiter;
    double preparedSampleRate = 48000.0;
    int silentSamples = 0;
    bool masterDspSleeping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
