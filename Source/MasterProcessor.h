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

    // o medidor le e zera; a queda suave fica por conta da interface
    float readAndResetLevel (int channel) noexcept
    {
        return levels[juce::jlimit (0, 1, channel)].exchange (0.0f);
    }

    bool readAndResetClip() noexcept            { return clipped.exchange (false); }

    float getLimiterGainReductionDb() const noexcept
    {
        return masterLimiter.getCurrentGainReductionDb();
    }

    uint64_t getLimiterEmergencyCorrectionCount() const noexcept
    {
        return masterLimiter.getEmergencyCorrectionCount();
    }

    //==============================================================================
    const juce::String getName() const override { return "Master"; }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        gain.reset (sampleRate, 0.05);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (getMasterDb(), minusInfinityDb));

        // EQ de masterizacao worship (sorriso sutil, ±1-2 dB, Q largo):
        // peso no grave, tira lama, dominio nos medios asperos, ar no agudo
        using C = juce::dsp::IIR::Coefficients<float>;
        auto dB = [] (float d) { return juce::Decibels::decibelsToGain (d); };

        juce::dsp::IIR::Coefficients<float>::Ptr coeffs[numBands] = {
            C::makeLowShelf  (sampleRate, 110.0f,   0.7f, dB (1.5f)),   // peso/calor
            C::makePeakFilter (sampleRate, 450.0f,  1.0f, dB (-1.5f)),  // tira lama
            C::makePeakFilter (sampleRate, 3000.0f, 1.2f, dB (-1.0f)),  // suaviza aspereza
            C::makeHighShelf (sampleRate, 11000.0f, 0.7f, dB (2.0f))    // ar/brilho
        };

        const int block = juce::jmax (64, samplesPerBlock);
        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) block, 1 };

        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < numBands; ++b)
            {
                eq[ch][b].prepare (spec);
                *eq[ch][b].coefficients = *coeffs[b];
                eq[ch][b].reset();
            }

        // ---- cadeia de Punch / Peso ----
        punchBuf.setSize (2, block, false, false, true);

        // peso no grave + limpeza da lama (para nao embolar)
        juce::dsp::IIR::Coefficients<float>::Ptr wc[2] = {
            C::makeLowShelf  (sampleRate, 90.0f,  0.7f, dB (2.5f)),   // peso/corpo
            C::makePeakFilter (sampleRate, 320.0f, 1.2f, dB (-1.8f))  // tira a lama
        };

        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < 2; ++b)
            {
                weightEq[ch][b].prepare (spec);
                *weightEq[ch][b].coefficients = *wc[b];
                weightEq[ch][b].reset();

                // HPF no caminho paralelo: o sub nao dispara o compressor
                punchHp[ch].prepare (spec);
                *punchHp[ch].coefficients = *C::makeHighPass (sampleRate, 130.0f);
                punchHp[ch].reset();
            }

        const juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) block, 2 };
        punchComp.prepare (stereoSpec);
        punchComp.setThreshold (-24.0f);
        punchComp.setRatio (4.0f);
        punchComp.setAttack (12.0f);
        punchComp.setRelease (140.0f);

        masterLimiter.prepare (sampleRate, block);
        setLatencySamples (masterLimiter.getLatencySamples());
    }

    void releaseResources() override
    {
        masterLimiter.reset();
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

        // Auto EQ de masterizacao (antes do ganho); desligado = mix original
        if (autoEqOn.load())
        {
            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            {
                float* d = buffer.getWritePointer (ch);

                for (int b = 0; b < numBands; ++b)
                    for (int s = 0; s < numSamples; ++s)
                        d[s] = eq[ch][b].processSample (d[s]);
            }
        }

        // Punch / Peso: compressao paralela saturada + peso no grave + anti-mud
        if (punchOn.load())
        {
            const int chs = juce::jmin (2, buffer.getNumChannels());

            if (punchBuf.getNumSamples() < numSamples)
                punchBuf.setSize (2, numSamples, false, false, true);

            // caminho paralelo: copia + HPF (tira sub do compressor)
            for (int ch = 0; ch < chs; ++ch)
            {
                punchBuf.copyFrom (ch, 0, buffer, ch, 0, numSamples);
                float* p = punchBuf.getWritePointer (ch);

                for (int s = 0; s < numSamples; ++s)
                    p[s] = punchHp[ch].processSample (p[s]);
            }

            // compressao forte no paralelo (soco)
            {
                juce::dsp::AudioBlock<float> blk (punchBuf.getArrayOfWritePointers(), (size_t) chs, (size_t) numSamples);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                punchComp.process (ctx);
            }

            // saturacao suave (harmonicos = peso que traduz em qualquer caixa)
            // + mistura paralela de volta ao seco (mantem a dinamica original)
            for (int ch = 0; ch < chs; ++ch)
            {
                const float* p = punchBuf.getReadPointer (ch);
                float* d = buffer.getWritePointer (ch);

                for (int s = 0; s < numSamples; ++s)
                    d[s] += std::tanh (p[s] * 1.6f) * 0.30f;
            }

            // peso no grave + limpeza da lama para nao embolar
            for (int ch = 0; ch < chs; ++ch)
            {
                float* d = buffer.getWritePointer (ch);

                for (int b = 0; b < 2; ++b)
                    for (int s = 0; s < numSamples; ++s)
                        d[s] = weightEq[ch][b].processSample (d[s]);
            }
        }

        gain.setTargetValue (juce::Decibels::decibelsToGain (getMasterDb(), minusInfinityDb));

        const float start = gain.getCurrentValue();
        gain.skip (numSamples);
        const float end = gain.getCurrentValue();
        buffer.applyGainRamp (0, numSamples, start, end);

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            if (buffer.getMagnitude (ch, 0, numSamples) > 1.0f)
                clipped.store (true);

        // Ultimo estagio antes da saida fisica. O lookahead stereo-linked
        // preserva o ataque do piano e impede que Return/Master no maximo
        // virem clipping digital/chiado no DAC do Android.
        masterLimiter.process (buffer);

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const float peak = buffer.getMagnitude (ch, 0, numSamples);

            float current = levels[ch].load();
            while (peak > current && ! levels[ch].compare_exchange_weak (current, peak)) {}

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
    std::atomic<float> levels[2] { 0.0f, 0.0f };
    std::atomic<bool> clipped { false };
    std::atomic<bool> autoEqOn { false };
    std::atomic<bool> punchOn { false };
    juce::dsp::IIR::Filter<float> eq[2][numBands];
    juce::dsp::IIR::Filter<float> weightEq[2][2], punchHp[2];
    juce::dsp::Compressor<float> punchComp;
    juce::AudioBuffer<float> punchBuf;
    juce::SmoothedValue<float> gain { 1.0f };
    StereoLookaheadLimiter masterLimiter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterProcessor)
};
