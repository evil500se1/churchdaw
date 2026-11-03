#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

//==============================================================================
// Reverb ambiente nativo da ChurchDAW.
//
// O motor e deliberadamente 100% molhado: cada canal envia a sua dose para um
// barramento separado e o TimbresProcessor soma o retorno ao caminho seco.
//
// Arquitetura:
//   predelay estereo -> reflexoes iniciais -> quatro allpasses de difusao
//   -> FDN de oito linhas/Hadamard -> decay em tres bandas
//   -> movimento por matriz ortogonal (sem mover os delays/sem Doppler)
//   -> duas vozes de pitch (+12 e +7) usadas apenas nos presets intencionais
//
// Todos os buffers sao alocados em prepare(). process() nao usa locks, Strings,
// new/delete nem redimensiona memoria, requisito importante para AAudio/Oboe.
class WorshipReverb
{
public:
    static constexpr int numPresets = 12;

    struct Preset
    {
        float decaySeconds;
        float lowDecaySeconds;
        float highDecaySeconds;
        float size;
        float predelayMs;
        float motionRateHz;
        float motionDepthDegrees;
        float diffusion;
        float earlyLevel;
        float lateLevel;
        float octaveLevel;
        float fifthLevel;
        float shimmerFeedback;
        float ducking;
        float width;
        float outputGain;
    };

    static const char* presetName (int index) noexcept
    {
        static constexpr const char* names[numPresets] =
        {
            "Bloom", "Cloud", "Chorale", "Shimmer", "Magneto", "Nonlinear",
            "Reflections", "Room", "Hall", "Plate", "Spring", "Swell"
        };

        return juce::isPositiveAndBelow (index, numPresets) ? names[index] : "";
    }

    void prepare (double newSampleRate, int maximumBlockSize)
    {
        sampleRate = juce::jmax (8000.0, newSampleRate);
        const int block = juce::jmax (64, maximumBlockSize);

        const int predelayCapacity = (int) std::ceil (sampleRate * 0.30) + block + 8;
        const int diffuserCapacity = (int) std::ceil (sampleRate * 0.055) + block + 8;
        const int fdnCapacity = (int) std::ceil (sampleRate * 0.18) + block + 8;

        for (auto& d : predelay)
            d.prepare (predelayCapacity);

        for (auto& channel : diffusers)
            for (auto& d : channel)
                d.prepare (diffuserCapacity);

        for (auto& d : fdn)
            d.prepare (fdnCapacity);

        // A compensacao de 8,76 cents corrige o pico dominante introduzido
        // pelo overlap granular; o resultado medido fica em +1200 cents.
        octave.prepare (sampleRate, 1.9899022f, 72.0f);
        fifth.prepare (sampleRate, 1.498307f, 86.0f);

        for (auto& h : inputHighPass)
            h.prepare (sampleRate, 95.0f);

        for (auto& h : shimmerHighPass)
            h.prepare (sampleRate, 430.0f);

        for (auto& l : shimmerLowPass)
            l.prepare (sampleRate, 9200.0f);

        for (auto& l : outputLowPass)
            l.prepare (sampleRate, 14500.0f);

        resetSmoother (predelaySamples, 0.40);
        resetSmoother (size, 0.65);
        resetSmoother (motionRate, 0.80);
        resetSmoother (motionDepthRadians, 0.65);
        resetSmoother (diffusion, 0.55);
        resetSmoother (earlyLevel, 0.45);
        resetSmoother (lateLevel, 0.45);
        resetSmoother (octaveLevel, 0.80);
        resetSmoother (fifthLevel, 0.80);
        resetSmoother (shimmerFeedback, 0.90);
        resetSmoother (ducking, 0.45);
        resetSmoother (width, 0.45);
        resetSmoother (outputGain, 0.45);

        feedbackLowCoefficient = 1.0f - std::exp (
            -juce::MathConstants<float>::twoPi * 350.0f / (float) sampleRate);
        feedbackHighCoefficient = 1.0f - std::exp (
            -juce::MathConstants<float>::twoPi * 3200.0f / (float) sampleRate);
        duckAttackCoefficient = 1.0f - std::exp (
            -1.0f / (float) (sampleRate * 0.008));
        duckReleaseCoefficient = 1.0f - std::exp (
            -1.0f / (float) (sampleRate * 0.42));

        presetRequestHoldSamples = juce::jmax (
            1, (int) std::lround (sampleRate * 0.025));
        presetMorphStep = 1.0f
                        / (float) juce::jmax (
                            1, (int) std::lround (sampleRate * 0.120));
        preparePresetCoefficients();

        appliedPreset = -1;
        applyPreset (desiredPreset.load (std::memory_order_relaxed), true);
        reset();
        prepared = true;
    }

    void reset() noexcept
    {
        clearSignalState();
        pendingPreset = appliedPreset;
        presetTransition = PresetTransition::idle;
        presetHoldSamplesRemaining = 0;
        presetMorphPosition = 1.0f;
        fdnDelaySamplesTarget = fdnDelaySamples;
        fdnFeedbackLowTarget = fdnFeedbackLow;
        fdnFeedbackMidTarget = fdnFeedbackMid;
        fdnFeedbackHighTarget = fdnFeedbackHigh;
    }

    void setPreset (int index) noexcept
    {
        desiredPreset.store (juce::jlimit (0, numPresets - 1, index),
                             std::memory_order_relaxed);
    }

    int getPreset() const noexcept
    {
        return desiredPreset.load (std::memory_order_relaxed);
    }

    double getTailLengthSeconds() const noexcept
    {
        const auto& p = preset (getPreset());
        const double longestRt60 = (double) juce::jmax (
            p.decaySeconds, p.lowDecaySeconds, p.highDecaySeconds);

        // Duas vezes RT60 cobre a operacao ate aproximadamente -120 dB, e os
        // 850 ms finais incluem predelay/pitch window e o detector de silencio.
        // Declarar apenas 1x RT60 faria um host cortar uma cauda ainda audivel.
        return longestRt60 * 2.0
             + (double) p.predelayMs * 0.001 + 0.85;
    }

    // Processa o barramento in-place e devolve somente o retorno molhado.
    void process (juce::AudioBuffer<float>& buffer) noexcept
    {
        juce::ScopedNoDenormals noDenormals;

        const int numSamples = buffer.getNumSamples();
        const bool stereo = buffer.getNumChannels() > 1;

        if (! prepared || buffer.getNumChannels() == 0 || numSamples <= 0)
        {
            buffer.clear();
            return;
        }

        const int requested = desiredPreset.load (std::memory_order_relaxed);
        handlePresetRequest (requested);

        float inputPeakForBlock = 0.0f;

        for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
            inputPeakForBlock = juce::jmax (
                inputPeakForBlock, buffer.getMagnitude (channel, 0, numSamples));

        if (inputPeakForBlock > 0.0000001f)
        {
            tailActive = true;
            quietSamples = 0;
        }
        else if (! tailActive)
        {
            buffer.clear();
            return;
        }

        auto* left = buffer.getWritePointer (0);
        auto* right = stereo ? buffer.getWritePointer (1) : left;
        float wetPeakForBlock = 0.0f;

        constexpr float hadamardNormalisation = 0.3535533905932738f; // 1/sqrt(8)
        constexpr std::array<float, 4> diffuserMsL { 4.37f, 7.13f, 10.73f, 15.17f };
        constexpr std::array<float, 4> diffuserMsR { 5.11f, 8.29f, 12.41f, 16.79f };
        constexpr std::array<float, 6> earlyMs { 11.3f, 17.9f, 27.1f, 39.7f, 56.3f, 78.1f };
        constexpr std::array<float, 6> earlyGain { 0.34f, -0.24f, 0.20f, 0.16f, -0.12f, 0.09f };
        constexpr std::array<float, 8> inputSigns
            { 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f };
        constexpr std::array<float, 8> sideSigns
            { 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f };
        constexpr std::array<float, 8> outSignsL
            { 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
        constexpr std::array<float, 8> outSignsR
            { -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f };

        // A matriz se move abaixo de 0,3 Hz. Atualiza-la uma vez por bloco
        // (188 Hz com buffer 256/48 kHz) e muito acima de Nyquist do controle,
        // evita quatro senos/divisoes por sample e continua perfeitamente lisa.
        const float blockMotionRate = motionRate.getNextValue();
        const float blockMotionDepth = motionDepthRadians.getNextValue();

        if (numSamples > 1)
        {
            motionRate.skip (numSamples - 1);
            motionDepthRadians.skip (numSamples - 1);
        }

        preparePitchStableMotion (
            blockMotionRate, blockMotionDepth, numSamples);

        // Stable presets keep the same fractional distances for the whole
        // block. Prepare clamp/integer/fraction once; indices still advance
        // per sample and interpolation keeps the exact original operation order.
        // A pending morph may change targets inside this block, so use the
        // original sample-by-sample path throughout every transition.
        const bool fixedDelays = presetTransition == PresetTransition::idle
                              && ! predelaySamples.isSmoothing() && ! size.isSmoothing();
        std::array<FractionalDelay::LinearDelay, 7> fixedPredelayTaps {};
        std::array<std::array<FractionalDelay::LinearDelay, 4>, 2> fixedDiffuserTaps {};
        if (fixedDelays)
        {
            const float predelayValue = juce::jlimit (
                1.0f, (float) predelay[0].capacity() - (float) (sampleRate * 0.09) - 4.0f,
                predelaySamples.getCurrentValue());
            fixedPredelayTaps[0] = predelay[0].prepareLinearDelay (predelayValue);
            for (int tap = 0; tap < 6; ++tap)
                fixedPredelayTaps[(size_t) tap + 1] = predelay[0].prepareLinearDelay (
                    predelayValue + earlyMs[(size_t) tap] * (float) sampleRate * 0.001f);
            for (int stage = 0; stage < 4; ++stage)
            {
                fixedDiffuserTaps[0][(size_t) stage] = diffusers[0][stage].prepareLinearDelay (
                    diffuserMsL[(size_t) stage] * size.getCurrentValue() * (float) sampleRate * 0.001f);
                fixedDiffuserTaps[1][(size_t) stage] = diffusers[1][stage].prepareLinearDelay (
                    diffuserMsR[(size_t) stage] * size.getCurrentValue() * (float) sampleRate * 0.001f);
            }
        }

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float dryL = left[sample];
            const float dryR = stereo ? right[sample] : dryL;
            float inL = inputHighPass[0].process (std::isfinite (dryL) ? dryL : 0.0f);
            float inR = inputHighPass[1].process (std::isfinite (dryR) ? dryR : 0.0f);
            if (! std::isfinite (inL)) inL = 0.0f;
            if (! std::isfinite (inR)) inR = 0.0f;

            const float inputPeak = juce::jmax (std::abs (inL), std::abs (inR));
            const float envRate = inputPeak > duckEnvelope
                                ? duckAttackCoefficient
                                : duckReleaseCoefficient;
            duckEnvelope += envRate * (inputPeak - duckEnvelope);

            const float currentPredelay = juce::jlimit (
                1.0f, (float) predelay[0].capacity() - (float) (sampleRate * 0.09) - 4.0f,
                predelaySamples.getNextValue());

            // Os dois predelays possuem exatamente a mesma capacidade e o
            // mesmo writeIndex (prepare/reset/write acontecem sempre em par).
            // Calcular clamp, indices e fracao uma vez evita repetir toda a
            // aritmetica para R sem mudar a interpolacao nem um bit do audio.
            const auto predelayTap =
                fixedDelays ? predelay[0].makeLinearTap (fixedPredelayTaps[0])
                            : predelay[0].makeLinearTap (currentPredelay);
            const float preL = predelay[0].read (predelayTap);
            const float preR = predelay[1].read (predelayTap);

            float reflectionsL = 0.0f;
            float reflectionsR = 0.0f;

            for (int tap = 0; tap < (int) earlyMs.size(); ++tap)
            {
                const float tapDelay = currentPredelay
                                     + earlyMs[(size_t) tap] * (float) sampleRate * 0.001f;
                const int mirrored = (int) earlyMs.size() - 1 - tap;
                const auto earlyTap =
                    fixedDelays ? predelay[0].makeLinearTap (fixedPredelayTaps[(size_t) tap + 1])
                                : predelay[0].makeLinearTap (tapDelay);
                reflectionsL += earlyGain[(size_t) tap]
                              * predelay[tap & 1].read (earlyTap);
                reflectionsR += earlyGain[(size_t) mirrored]
                              * predelay[(tap + 1) & 1].read (earlyTap);
            }

            predelay[0].write (inL);
            predelay[1].write (inR);

            const float currentSize = size.getNextValue();
            const float currentDiffusion = diffusion.getNextValue();
            float diffuseL = preL + reflectionsR * 0.08f;
            float diffuseR = preR + reflectionsL * 0.08f;

            for (int stage = 0; stage < 4; ++stage)
            {
                if (fixedDelays)
                {
                    diffuseL = diffusers[0][stage].process (
                        diffuseL, fixedDiffuserTaps[0][(size_t) stage], currentDiffusion);
                    diffuseR = diffusers[1][stage].process (
                        diffuseR, fixedDiffuserTaps[1][(size_t) stage], currentDiffusion);
                }
                else
                {
                    diffuseL = diffusers[0][stage].process (
                        diffuseL, diffuserMsL[(size_t) stage] * currentSize
                                    * (float) sampleRate * 0.001f,
                        currentDiffusion);
                    diffuseR = diffusers[1][stage].process (
                        diffuseR, diffuserMsR[(size_t) stage] * currentSize
                                    * (float) sampleRate * 0.001f,
                        currentDiffusion);
                }
            }

            float delayed[8];
            float filteredFeedback[8];
            const bool morphing =
                presetTransition == PresetTransition::morphing;
            const float morph = morphing ? presetMorphPosition : 1.0f;
            float oldOutputWeight = 0.0f;
            float newOutputWeight = 1.0f;

            if (morphing)
            {
                const float angle = morph
                                  * juce::MathConstants<float>::halfPi;
                oldOutputWeight =
                    juce::dsp::FastMathApproximations::cos (angle);
                newOutputWeight =
                    juce::dsp::FastMathApproximations::sin (angle);
            }

            for (int line = 0; line < 8; ++line)
            {
                // Comprimentos inteiros primos: read head realmente fixo, sem
                // Doppler e sem a coloracao/atenuacao de um interpolador dentro
                // do loop. Os primos reduzem coincidencias periodicas entre as
                // oito linhas e aumentam a densidade modal da cauda.
                const float oldTap = fdn[(size_t) line].readFixedPrepared (
                    fdnDelaySamples[(size_t) line]);
                float raw = oldTap;
                float outputTap = oldTap;
                float feedbackLow = fdnFeedbackLow[(size_t) line];
                float feedbackMid = fdnFeedbackMid[(size_t) line];
                float feedbackHigh = fdnFeedbackHigh[(size_t) line];

                if (morphing)
                {
                    const float newTap = fdn[(size_t) line].readFixedPrepared (
                        fdnDelaySamplesTarget[(size_t) line]);

                    // O loop de realimentacao usa interpolacao linear (soma de
                    // pesos = 1, portanto nunca ganha energia). A saida usa
                    // equal-power para duas caudas descorrelacionadas nao
                    // criarem o antigo buraco wet durante a troca.
                    raw += (newTap - raw) * morph;
                    outputTap = oldTap * oldOutputWeight
                              + newTap * newOutputWeight;
                    feedbackLow +=
                        (fdnFeedbackLowTarget[(size_t) line] - feedbackLow)
                        * morph;
                    feedbackMid +=
                        (fdnFeedbackMidTarget[(size_t) line] - feedbackMid)
                        * morph;
                    feedbackHigh +=
                        (fdnFeedbackHighTarget[(size_t) line] - feedbackHigh)
                        * morph;
                }

                auto& lowState = fdnFeedbackLowState[(size_t) line];
                auto& upperState = fdnFeedbackUpperState[(size_t) line];
                lowState += feedbackLowCoefficient * (raw - lowState);
                upperState += feedbackHighCoefficient * (raw - upperState);

                const float low = lowState;
                const float mid = upperState - lowState;
                const float high = raw - upperState;
                delayed[line] = outputTap;
                // A absorcao acontece por linha e ANTES de uma unica matriz
                // ortogonal, a forma canonica A(z) = U(z) * Gamma(z). Separar
                // o grave numa matriz estatica e medio/agudo numa matriz movel
                // cria dois FDNs sobrepostos no crossover; nos testes isso
                // deixou um modo quase marginal em 125 Hz e um ringing cerca
                // de 23 cents acima da nota no fim da cauda.
                filteredFeedback[line] =
                    low * feedbackLow
                    +
                    mid * feedbackMid
                    + high * feedbackHigh;
            }

            float lateL = 0.0f;
            float lateR = 0.0f;

            for (int line = 0; line < 8; ++line)
            {
                lateL += delayed[line] * outSignsL[(size_t) line];
                lateR += delayed[line] * outSignsR[(size_t) line];
            }

            lateL *= 0.25f;
            lateR *= 0.25f;

            const float oct = octaveLevel.getNextValue();
            const float fifthAmount = fifthLevel.getNextValue();
            float octaveL = 0.0f, octaveR = 0.0f;
            float fifthL = 0.0f, fifthR = 0.0f;

            // Cloud, Hall e Room nao pagam o custo do pitch shifter e, mais
            // importante, nao recebem harmonicos artificiais na cauda.
            if (oct > 0.00001f)
                octave.process (lateL, lateR, octaveL, octaveR);

            if (fifthAmount > 0.00001f)
                fifth.process (lateL, lateR, fifthL, fifthR);

            float pitchedL = octaveL * oct + fifthL * fifthAmount;
            float pitchedR = octaveR * oct + fifthR * fifthAmount;
            pitchedL = shimmerLowPass[0].process (shimmerHighPass[0].process (pitchedL));
            pitchedR = shimmerLowPass[1].process (shimmerHighPass[1].process (pitchedR));

            hadamard8 (filteredFeedback);
            applyPitchStableMotion (filteredFeedback);

            const float mono = 0.5f * (diffuseL + diffuseR);
            const float side = 0.5f * (diffuseL - diffuseR);
            const float shimmerFb = shimmerFeedback.getNextValue();

            for (int line = 0; line < 8; ++line)
            {
                const float injected = 0.235f * (
                    mono * inputSigns[(size_t) line]
                    + side * sideSigns[(size_t) line]);
                const float shimmer = 0.25f * shimmerFb * (
                    pitchedL * sideSigns[(size_t) line]
                    + pitchedR * inputSigns[(size_t) line]);
                float writeValue = injected
                                 + filteredFeedback[line]
                                   * hadamardNormalisation
                                 + shimmer;

                // O limite fica muito acima da operacao normal; ele nao colore
                // a cauda, apenas impede um estado corrompido de virar runaway.
                writeValue = std::isfinite (writeValue)
                               ? juce::jlimit (-3.0f, 3.0f, writeValue)
                               : 0.0f;
                fdn[(size_t) line].write (writeValue);
            }

            const float early = earlyLevel.getNextValue();
            const float late = lateLevel.getNextValue();
            float wetL = reflectionsL * early + lateL * late + pitchedL * 0.32f;
            float wetR = reflectionsR * early + lateR * late + pitchedR * 0.32f;

            const float currentWidth = width.getNextValue();
            const float mid = 0.5f * (wetL + wetR);
            const float wetSide = 0.5f * (wetL - wetR) * currentWidth;
            wetL = outputLowPass[0].process (mid + wetSide);
            wetR = outputLowPass[1].process (mid - wetSide);

            const float duck = 1.0f - ducking.getNextValue()
                                      * juce::jlimit (0.0f, 1.0f, duckEnvelope * 3.2f);
            const float gain = outputGain.getNextValue() * duck;

            if (stereo)
            {
                left[sample] = wetL * gain;
                right[sample] = wetR * gain;
                wetPeakForBlock = juce::jmax (
                    wetPeakForBlock, std::abs (left[sample]), std::abs (right[sample]));
            }
            else
            {
                left[sample] = 0.5f * (wetL + wetR) * gain;
                wetPeakForBlock = juce::jmax (
                    wetPeakForBlock, std::abs (left[sample]));
            }

            advancePresetTransition();
        }

        // Depois que a cauda ja caiu abaixo de -120 dBFS por 750 ms, o motor
        // entra em repouso. Isso devolve CPU/bateria no palco quando nenhum
        // canal envia reverb. Limpe tambem os buffers: deixar o residuo parado
        // permite que ele reapareca quando uma nova nota reativa o motor.
        if (inputPeakForBlock <= 0.0000001f && wetPeakForBlock < 0.000001f)
        {
            quietSamples += numSamples;

            if (quietSamples >= (int) (sampleRate * 0.75))
                clearSignalState();
        }
        else
        {
            quietSamples = 0;
        }
    }

private:
    class FractionalDelay
    {
    public:
        struct LinearDelay { int whole = 1; float fraction = 0.0f; };
        struct LinearTap
        {
            int newer = 0, older = 0;
            float fraction = 0.0f;
        };

        struct CubicTap
        {
            int previous = 0, centre = 0, next = 0, nextNext = 0;
            float cPrevious = 0.0f, cCentre = 1.0f;
            float cNext = 0.0f, cNextNext = 0.0f;
        };

        void prepare (int maximumSamples)
        {
            data.assign ((size_t) juce::jmax (16, maximumSamples), 0.0f);
            writeIndex = 0;
        }

        void reset() noexcept
        {
            std::fill (data.begin(), data.end(), 0.0f);
            writeIndex = 0;
        }

        int capacity() const noexcept { return (int) data.size(); }

        LinearDelay prepareLinearDelay (float delaySamples) const noexcept
        {
            const float delay = juce::jlimit (1.0f, (float) data.size() - 3.0f, delaySamples);
            const int whole = (int) delay;
            return { whole, delay - (float) whole };
        }

        LinearTap makeLinearTap (float delaySamples) const noexcept
        {
            if ((int) data.size() < 4 || ! std::isfinite (delaySamples)) return {};
            return makeLinearTap (prepareLinearDelay (delaySamples));
        }

        LinearTap makeLinearTap (LinearDelay delay) const noexcept
        {
            LinearTap tap;
            const int count = (int) data.size();
            tap.fraction = delay.fraction;
            tap.newer = writeIndex - delay.whole;

            if (tap.newer < 0)
                tap.newer += count;

            tap.older = tap.newer > 0 ? tap.newer - 1 : count - 1;
            return tap;
        }

        float read (const LinearTap& tap) const noexcept
        {
            return data[(size_t) tap.newer]
                 + tap.fraction
                     * (data[(size_t) tap.older]
                         - data[(size_t) tap.newer]);
        }

        float read (float delaySamples) const noexcept
        {
            if (data.empty())
                return 0.0f;

            return read (makeLinearTap (delaySamples));
        }

        float readFixed (int delaySamples) const noexcept
        {
            const int count = (int) data.size();
            if (count < 2)
                return 0.0f;

            const int delay = juce::jlimit (1, count - 1, delaySamples);
            int position = writeIndex - delay;

            if (position < 0)
                position += count;

            return data[(size_t) position];
        }

        // Caminho FDN: os taps vieram de preparePresetCoefficients(), que os
        // limita aos delays primos de cada preset e prepara 180 ms de margem.
        // Nao repita empty()/jlimit() para oito linhas em cada amostra. O
        // jassert protege novas tabelas em Debug e custa zero no Release.
        float readFixedPrepared (int delaySamples) const noexcept
        {
            jassert (! data.empty());
            jassert (delaySamples >= 1
                     && delaySamples < (int) data.size());

            const int count = (int) data.size();
            if (count < 2)
                return 0.0f;

            int position = writeIndex - delaySamples;

            if (position < 0)
                position += count;

            if ((unsigned int) position >= (unsigned int) count)
                position = 0;

            return data[(size_t) position];
        }

        CubicTap makeCubicTap (float delaySamples) const noexcept
        {
            CubicTap tap;

            if ((int) data.size() < 4 || ! std::isfinite (delaySamples))
                return tap;

            const int count = (int) data.size();
            const float maximum = (float) count - 3.0f;
            const float delay = juce::jlimit (1.0f, maximum, delaySamples);
            const int wholeDelay = (int) delay;
            const float delayFraction = delay - (float) wholeDelay;
            float t = 0.0f;
            tap.centre = writeIndex - wholeDelay;

            if (delayFraction > 0.0f)
            {
                --tap.centre;
                t = 1.0f - delayFraction;
            }

            if (tap.centre < 0)
                tap.centre += count;

            tap.previous = tap.centre > 0 ? tap.centre - 1 : count - 1;
            tap.next = tap.centre + 1 < count ? tap.centre + 1 : 0;
            tap.nextNext = tap.next + 1 < count ? tap.next + 1 : 0;

            // Lagrange de quatro pontos: resposta de fractional delay muito
            // mais plana que a interpolacao linear, especialmente nos agudos.
            tap.cPrevious =
                -t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f);
            tap.cCentre =
                (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
            tap.cNext =
                -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
            tap.cNextNext =
                (t + 1.0f) * t * (t - 1.0f) * (1.0f / 6.0f);
            return tap;
        }

        float readCubic (const CubicTap& tap) const noexcept
        {
            return data[(size_t) tap.previous] * tap.cPrevious
                 + data[(size_t) tap.centre] * tap.cCentre
                 + data[(size_t) tap.next] * tap.cNext
                 + data[(size_t) tap.nextNext] * tap.cNextNext;
        }

        float readCubic (float delaySamples) const noexcept
        {
            if (data.empty())
                return 0.0f;

            return readCubic (makeCubicTap (delaySamples));
        }

        void write (float value) noexcept
        {
            if (data.empty())
                return;

            data[(size_t) writeIndex] = value;

            if (++writeIndex >= (int) data.size())
                writeIndex = 0;
        }

    private:
        std::vector<float> data;
        int writeIndex = 0;
    };

    class Allpass
    {
    public:
        void prepare (int maximumSamples) { delay.prepare (maximumSamples); }
        void reset() noexcept             { delay.reset(); }

        float process (float input, float delaySamples, float gain) noexcept
        {
            const float g = juce::jlimit (0.20f, 0.90f, gain);
            const float delayed = delay.read (delaySamples);
            const float output = delayed - g * input;
            delay.write (input + g * output);
            return output;
        }

        FractionalDelay::LinearDelay prepareLinearDelay (float samples) const noexcept
        { return delay.prepareLinearDelay (samples); }

        float process (float input, FractionalDelay::LinearDelay tap, float gain) noexcept
        {
            const float g = juce::jlimit (0.20f, 0.90f, gain);
            const float delayed = delay.read (delay.makeLinearTap (tap));
            const float output = delayed - g * input;
            delay.write (input + g * output);
            return output;
        }

    private:
        FractionalDelay delay;
    };

    class OnePoleLowPass
    {
    public:
        void prepare (double sr, float cutoff) noexcept
        {
            coefficient = 1.0f - std::exp (
                -juce::MathConstants<float>::twoPi * cutoff / (float) sr);
            reset();
        }

        void reset() noexcept { state = 0.0f; }

        float process (float input) noexcept
        {
            state += coefficient * (input - state);
            return state;
        }

    private:
        float coefficient = 1.0f;
        float state = 0.0f;
    };

    class OnePoleHighPass
    {
    public:
        void prepare (double sr, float cutoff) noexcept
        {
            coefficient = std::exp (
                -juce::MathConstants<float>::twoPi * cutoff / (float) sr);
            reset();
        }

        void reset() noexcept
        {
            previousInput = 0.0f;
            previousOutput = 0.0f;
        }

        float process (float input) noexcept
        {
            const float output = coefficient
                               * (previousOutput + input - previousInput);
            previousInput = input;
            previousOutput = output;
            return output;
        }

    private:
        float coefficient = 0.99f;
        float previousInput = 0.0f;
        float previousOutput = 0.0f;
    };

    class PitchVoice
    {
    public:
        void prepare (double sr, float newRatio, float windowMs)
        {
            ratio = newRatio;
            windowSamples = juce::jlimit (
                512.0f, 8192.0f, (float) sr * windowMs * 0.001f);
            minimumDelay = juce::jmax (32.0f, (float) sr * 0.0015f);
            phaseIncrement = (ratio - 1.0f) / windowSamples;
            const float oscillatorIncrement =
                juce::MathConstants<float>::twoPi * phaseIncrement;
            oscillatorIncrementCos = std::cos (oscillatorIncrement);
            oscillatorIncrementSin = std::sin (oscillatorIncrement);
            const int capacity = (int) std::ceil (windowSamples + minimumDelay) + 16;

            for (auto& d : delay)
                d.prepare (capacity);

            reset();
        }

        void reset() noexcept
        {
            for (auto& d : delay)
                d.reset();

            phase = 0.0f;
            oscillatorCos = 1.0f;
            oscillatorSin = 0.0f;
            oscillatorSamples = 0;
        }

        void process (float inputL, float inputR,
                      float& outputL, float& outputR) noexcept
        {
            delay[0].write (inputL);
            delay[1].write (inputR);

            const float phaseA = phase;
            float phaseB = phaseA + 0.5f;

            if (phaseB >= 1.0f)
                phaseB -= 1.0f;

            const float delayA = minimumDelay + (1.0f - phaseA) * windowSamples;
            const float delayB = minimumDelay + (1.0f - phaseB) * windowSamples;
            const float weightA = 0.5f - 0.5f * oscillatorCos;
            const float weightB = 1.0f - weightA;
            const auto tapA = delay[0].makeCubicTap (delayA);
            const auto tapB = delay[0].makeCubicTap (delayB);

            // Os canais possuem o mesmo tamanho e write index. Calcule os
            // indices/coeficientes uma vez e reutilize-os em L/R.
            outputL = delay[0].readCubic (tapA) * weightA
                    + delay[0].readCubic (tapB) * weightB;
            outputR = delay[1].readCubic (tapA) * weightA
                    + delay[1].readCubic (tapB) * weightB;

            phase += phaseIncrement;

            while (phase >= 1.0f)
                phase -= 1.0f;

            const float oldCos = oscillatorCos;
            oscillatorCos = oldCos * oscillatorIncrementCos
                          - oscillatorSin * oscillatorIncrementSin;
            oscillatorSin = oscillatorSin * oscillatorIncrementCos
                          + oldCos * oscillatorIncrementSin;

            // A recorrencia evita cos() por sample. Ressincronizar raramente
            // com a fase que tambem dirige os delays elimina tanto deriva de
            // magnitude quanto deriva angular (importante em caudas > 30 s).
            if (++oscillatorSamples >= 2048)
            {
                const float exactAngle =
                    juce::MathConstants<float>::twoPi * phase;
                oscillatorCos = std::cos (exactAngle);
                oscillatorSin = std::sin (exactAngle);
                oscillatorSamples = 0;
            }
        }

    private:
        FractionalDelay delay[2];
        float ratio = 2.0f;
        float windowSamples = 2048.0f;
        float minimumDelay = 32.0f;
        float phaseIncrement = 0.0005f;
        float phase = 0.0f;
        float oscillatorIncrementCos = 1.0f;
        float oscillatorIncrementSin = 0.0f;
        float oscillatorCos = 1.0f;
        float oscillatorSin = 0.0f;
        int oscillatorSamples = 0;
    };

    using Smooth = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    enum class PresetTransition
    {
        idle,
        waitingForStableRequest,
        morphing
    };

    void handlePresetRequest (int requested) noexcept
    {
        if (! tailActive)
        {
            // Sem cauda audivel nao ha nada para preservar nem cruzar. Tambem
            // encerra uma transicao que tenha ficado pendente quando o motor
            // acabou de entrar em repouso.
            if (requested != appliedPreset
                || presetTransition != PresetTransition::idle)
                applyPreset (requested, true);

            return;
        }

        if (presetTransition == PresetTransition::idle)
        {
            if (requested != appliedPreset)
            {
                pendingPreset = requested;
                presetHoldSamplesRemaining = presetRequestHoldSamples;
                presetTransition =
                    PresetTransition::waitingForStableRequest;
            }

            return;
        }

        if (presetTransition == PresetTransition::waitingForStableRequest)
        {
            // Uma ida e volta muito rapida nao precisa tocar no FDN. Mantemos o
            // audio do preset ativo durante esta curta janela, sem fade-out.
            if (requested == appliedPreset)
            {
                pendingPreset = appliedPreset;
                presetHoldSamplesRemaining = 0;
                presetTransition = PresetTransition::idle;
            }
            else if (requested != pendingPreset)
            {
                pendingPreset = requested;
                presetHoldSamplesRemaining = presetRequestHoldSamples;
            }

            return;
        }

        if (requested == appliedPreset && requested != pendingPreset)
        {
            // Reverter um morph troca os dois taps e espelha a posicao. O valor
            // produzido na proxima amostra continua no mesmo ponto da curva;
            // nao ha salto, limpeza de buffer ou alocacao.
            fdnDelaySamples.swap (fdnDelaySamplesTarget);
            fdnFeedbackLow.swap (fdnFeedbackLowTarget);
            fdnFeedbackMid.swap (fdnFeedbackMidTarget);
            fdnFeedbackHigh.swap (fdnFeedbackHighTarget);
            presetMorphPosition = 1.0f - presetMorphPosition;
            pendingPreset = appliedPreset;
            setPresetControls (appliedPreset, false);
        }
    }

    void clearSignalState() noexcept
    {
        for (auto& d : predelay)
            d.reset();

        for (auto& channel : diffusers)
            for (auto& d : channel)
                d.reset();

        for (auto& d : fdn)
            d.reset();

        octave.reset();
        fifth.reset();

        for (auto& h : inputHighPass)
            h.reset();

        for (auto& h : shimmerHighPass)
            h.reset();

        for (auto& l : shimmerLowPass)
            l.reset();

        for (auto& l : outputLowPass)
            l.reset();

        fdnFeedbackLowState.fill (0.0f);
        fdnFeedbackUpperState.fill (0.0f);

        // Fases desencontradas: mesmo no primeiro acorde nao existe um glide
        // coerente das quatro rotacoes para a mesma direcao.
        matrixMotionPhase = { -2.31f, -0.73f, 0.91f, 2.47f };

        duckEnvelope = 0.0f;
        quietSamples = 0;
        tailActive = false;
    }

    void preparePitchStableMotion (float rateHz,
                                   float peakAngleRadians,
                                   int numSamples) noexcept
    {
        static constexpr std::array<float, 4> rateSpread
            { 0.73f, 0.89f, 1.14f, 1.37f };

        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        for (int rotation = 0; rotation < 4; ++rotation)
        {
            auto& phase = matrixMotionPhase[(size_t) rotation];
            const float phaseAdvance =
                twoPi * rateHz * rateSpread[(size_t) rotation]
                * (float) numSamples / (float) sampleRate;
            float centrePhase = phase + 0.5f * phaseAdvance;
            phase += phaseAdvance;

            while (centrePhase > juce::MathConstants<float>::pi)
                centrePhase -= twoPi;

            while (phase > juce::MathConstants<float>::pi)
                phase -= twoPi;

            const float lfo =
                juce::dsp::FastMathApproximations::sin (centrePhase);
            const float angle = peakAngleRadians * lfo;

            // Parametrizacao de Cayley de uma rotacao 2x2. Mesmo usando angle/2
            // como aproximacao de tan(angle/2), c*c+s*s continua exatamente 1
            // (salvo arredondamento), logo a matriz nao cria energia.
            const float tangentHalf = 0.5f * angle;
            const float tangentSquared = tangentHalf * tangentHalf;
            const float inverse = 1.0f / (1.0f + tangentSquared);
            matrixMotionCosine[(size_t) rotation] =
                (1.0f - tangentSquared) * inverse;
            matrixMotionSine[(size_t) rotation] =
                2.0f * tangentHalf * inverse;
        }
    }

    void applyPitchStableMotion (float* values) const noexcept
    {
        static constexpr std::array<int, 4> first  { 0, 1, 2, 3 };
        static constexpr std::array<int, 4> second { 5, 7, 4, 6 };

        for (int rotation = 0; rotation < 4; ++rotation)
        {
            const float cosine = matrixMotionCosine[(size_t) rotation];
            const float sine = matrixMotionSine[(size_t) rotation];
            const int a = first[(size_t) rotation];
            const int b = second[(size_t) rotation];
            const float x = values[a];
            const float y = values[b];
            values[a] = cosine * x - sine * y;
            values[b] = sine * x + cosine * y;
        }
    }

    void advancePresetTransition() noexcept
    {
        if (presetTransition == PresetTransition::waitingForStableRequest)
        {
            if (--presetHoldSamplesRemaining <= 0)
                applyPreset (pendingPreset, false);

            return;
        }

        if (presetTransition == PresetTransition::morphing)
        {
            presetMorphPosition += presetMorphStep;

            if (presetMorphPosition >= 1.0f)
            {
                presetMorphPosition = 1.0f;
                fdnDelaySamples = fdnDelaySamplesTarget;
                fdnFeedbackLow = fdnFeedbackLowTarget;
                fdnFeedbackMid = fdnFeedbackMidTarget;
                fdnFeedbackHigh = fdnFeedbackHighTarget;
                appliedPreset = pendingPreset;
                presetTransition = PresetTransition::idle;
                pendingPreset = appliedPreset;
            }
        }
    }

    void resetSmoother (Smooth& smoother, double seconds)
    {
        smoother.reset (sampleRate, seconds);
    }

    static const Preset& preset (int index) noexcept
    {
        static constexpr Preset presets[numPresets] =
        {
            // mid  low  high size pre  rate deg diff early late oct  5th fb   duck width out
            {  8.5f,  7.0f, 4.0f, 1.18f, 52.0f, 0.12f, 2.0f, 0.82f, 0.10f, 0.94f, 0.00f, 0.00f, 0.00f, 0.36f, 1.34f, 0.76f }, // Bloom
            { 12.5f, 10.0f, 5.5f, 1.30f, 62.0f, 0.16f, 5.0f, 0.86f, 0.06f, 0.98f, 0.00f, 0.00f, 0.00f, 0.32f, 1.42f, 0.74f }, // Cloud
            {  8.5f,  7.0f, 4.0f, 1.22f, 48.0f, 0.14f, 2.0f, 0.83f, 0.08f, 0.90f, 0.20f, 0.00f, 0.00f, 0.32f, 1.36f, 0.70f }, // Chorale
            {  9.5f,  7.5f, 4.0f, 1.27f, 64.0f, 0.11f, 2.0f, 0.84f, 0.06f, 0.88f, 0.50f, 0.00f, 0.06f, 0.28f, 1.40f, 0.66f }, // Shimmer
            {  4.8f,  4.0f, 2.5f, 1.08f, 34.0f, 0.20f, 2.0f, 0.68f, 0.14f, 0.86f, 0.00f, 0.00f, 0.00f, 0.24f, 1.28f, 0.77f }, // Magneto
            {  2.4f,  1.8f, 1.2f, 0.90f, 18.0f, 0.00f, 0.0f, 0.62f, 0.22f, 0.76f, 0.00f, 0.00f, 0.00f, 0.12f, 1.15f, 0.84f }, // Nonlinear
            {  3.4f,  2.6f, 1.7f, 0.96f, 22.0f, 0.00f, 0.0f, 0.58f, 0.40f, 0.70f, 0.00f, 0.00f, 0.00f, 0.16f, 1.24f, 0.80f }, // Reflections
            {  1.8f,  1.4f, 0.9f, 0.72f, 12.0f, 0.00f, 0.0f, 0.58f, 0.42f, 0.62f, 0.00f, 0.00f, 0.00f, 0.10f, 1.08f, 0.88f }, // Room
            {  6.5f,  5.4f, 3.2f, 1.12f, 38.0f, 0.14f, 1.5f, 0.78f, 0.14f, 0.90f, 0.00f, 0.00f, 0.00f, 0.26f, 1.30f, 0.78f }, // Hall
            {  4.0f,  3.2f, 2.7f, 0.84f, 18.0f, 0.20f, 1.0f, 0.76f, 0.20f, 0.82f, 0.00f, 0.00f, 0.00f, 0.18f, 1.20f, 0.83f }, // Plate
            {  2.6f,  2.1f, 1.6f, 0.78f, 13.0f, 0.00f, 0.0f, 0.54f, 0.28f, 0.70f, 0.00f, 0.00f, 0.00f, 0.14f, 1.12f, 0.82f }, // Spring
            { 13.5f, 10.5f, 5.8f, 1.28f, 72.0f, 0.095f,3.0f, 0.88f, 0.05f, 0.96f, 0.16f, 0.00f, 0.00f, 0.84f, 1.40f, 0.70f }  // Swell
        };

        return presets[(size_t) juce::jlimit (0, numPresets - 1, index)];
    }

    static void setSmooth (Smooth& smoother, float value, bool immediate) noexcept
    {
        if (immediate)
            smoother.setCurrentAndTargetValue (value);
        else
            smoother.setTargetValue (value);
    }

    struct PreparedPreset
    {
        std::array<int, 8> delaySamples {};
        std::array<float, 8> feedbackLow {};
        std::array<float, 8> feedbackMid {};
        std::array<float, 8> feedbackHigh {};
    };

    static bool isPrime (int value) noexcept
    {
        if (value < 2)
            return false;
        if ((value & 1) == 0)
            return value == 2;

        for (int divisor = 3; divisor <= value / divisor; divisor += 2)
            if (value % divisor == 0)
                return false;

        return true;
    }

    static int nearestPrime (int requested) noexcept
    {
        requested = juce::jmax (3, requested);

        for (int distance = 0; ; ++distance)
        {
            const int lower = requested - distance;
            const int upper = requested + distance;

            if (lower >= 3 && isPrime (lower))
                return lower;
            if (upper != lower && isPrime (upper))
                return upper;
        }
    }

    void preparePresetCoefficients()
    {
        static constexpr std::array<float, 8> baseDelayMs
            { 31.1f, 37.7f, 43.9f, 53.3f, 61.7f, 71.9f, 83.9f, 97.1f };

        for (int presetIndex = 0; presetIndex < numPresets; ++presetIndex)
        {
            const auto& p = preset (presetIndex);
            auto& preparedPreset = preparedPresets[(size_t) presetIndex];

            for (int line = 0; line < 8; ++line)
            {
                const int delaySamples = nearestPrime ((int) std::lround (
                    baseDelayMs[(size_t) line] * p.size
                    * (float) sampleRate * 0.001f));
                const float delaySeconds =
                    (float) delaySamples / (float) sampleRate;
                const float stabilityReserve =
                    1.0f - 0.30f * p.shimmerFeedback;
                const auto feedbackFor =
                    [delaySeconds, stabilityReserve] (float t60)
                    {
                        return juce::jlimit (
                            0.0f, 0.993f,
                            std::pow (0.001f,
                                      delaySeconds
                                      / juce::jmax (0.4f, t60))
                                * stabilityReserve);
                    };

                preparedPreset.delaySamples[(size_t) line] = delaySamples;
                preparedPreset.feedbackLow[(size_t) line] =
                    feedbackFor (p.lowDecaySeconds * 0.96f);
                preparedPreset.feedbackMid[(size_t) line] =
                    feedbackFor (p.decaySeconds * 1.185f);
                preparedPreset.feedbackHigh[(size_t) line] =
                    feedbackFor (p.highDecaySeconds * 0.84f);
            }
        }
    }

    void setPresetControls (int index, bool immediate)
    {
        const int safeIndex = juce::jlimit (0, numPresets - 1, index);
        const auto& p = preset (safeIndex);

        setSmooth (predelaySamples, p.predelayMs * (float) sampleRate * 0.001f, immediate);
        setSmooth (size, p.size, immediate);
        setSmooth (motionRate, p.motionRateHz, immediate);
        setSmooth (motionDepthRadians,
                   juce::degreesToRadians (p.motionDepthDegrees), immediate);
        setSmooth (diffusion, p.diffusion, immediate);
        setSmooth (earlyLevel, p.earlyLevel, immediate);
        setSmooth (lateLevel, p.lateLevel, immediate);
        setSmooth (octaveLevel, p.octaveLevel, immediate);
        setSmooth (fifthLevel, p.fifthLevel, immediate);
        setSmooth (shimmerFeedback, p.shimmerFeedback, immediate);
        setSmooth (ducking, p.ducking, immediate);
        setSmooth (width, p.width, immediate);
        setSmooth (outputGain, p.outputGain, immediate);
    }

    void applyPreset (int index, bool immediate)
    {
        const int safeIndex = juce::jlimit (0, numPresets - 1, index);
        setPresetControls (safeIndex, immediate);

        const auto& preparedPreset = preparedPresets[(size_t) safeIndex];

        if (immediate)
        {
            fdnDelaySamples = preparedPreset.delaySamples;
            fdnFeedbackLow = preparedPreset.feedbackLow;
            // Compensacoes medidas do crossover complementar fazem os T30
            // reais convergirem aos alvos, nao apenas aos nomes.
            fdnFeedbackMid = preparedPreset.feedbackMid;
            fdnFeedbackHigh = preparedPreset.feedbackHigh;
            fdnDelaySamplesTarget = fdnDelaySamples;
            fdnFeedbackLowTarget = fdnFeedbackLow;
            fdnFeedbackMidTarget = fdnFeedbackMid;
            fdnFeedbackHighTarget = fdnFeedbackHigh;
            appliedPreset = safeIndex;
            pendingPreset = safeIndex;
            presetMorphPosition = 1.0f;
            presetHoldSamplesRemaining = 0;
            presetTransition = PresetTransition::idle;
            return;
        }

        // Os dois taps leem os mesmos buffers FDN, que continuam intactos. O
        // callback apenas cruza taps/coefs precomputados por 120 ms.
        fdnDelaySamplesTarget = preparedPreset.delaySamples;
        fdnFeedbackLowTarget = preparedPreset.feedbackLow;
        fdnFeedbackMidTarget = preparedPreset.feedbackMid;
        fdnFeedbackHighTarget = preparedPreset.feedbackHigh;
        pendingPreset = safeIndex;
        presetMorphPosition = 0.0f;
        presetTransition = PresetTransition::morphing;
    }

    static void hadamard8 (float* values) noexcept
    {
        for (int span = 1; span < 8; span *= 2)
            for (int start = 0; start < 8; start += span * 2)
                for (int offset = 0; offset < span; ++offset)
                {
                    const int a = start + offset;
                    const int b = a + span;
                    const float x = values[a];
                    const float y = values[b];
                    values[a] = x + y;
                    values[b] = x - y;
                }
    }

    double sampleRate = 48000.0;
    bool prepared = false;
    std::atomic<int> desiredPreset { 1 }; // Cloud: padrao worship da primeira abertura
    int appliedPreset = -1;
    int pendingPreset = -1;

    FractionalDelay predelay[2];
    Allpass diffusers[2][4];
    std::array<FractionalDelay, 8> fdn;
    PitchVoice octave, fifth;

    OnePoleHighPass inputHighPass[2];
    OnePoleHighPass shimmerHighPass[2];
    OnePoleLowPass shimmerLowPass[2];
    OnePoleLowPass outputLowPass[2];

    std::array<float, 8> fdnFeedbackLowState {};
    std::array<float, 8> fdnFeedbackUpperState {};
    std::array<float, 4> matrixMotionPhase {};
    std::array<float, 4> matrixMotionCosine { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> matrixMotionSine {};
    float feedbackLowCoefficient = 0.04f;
    float feedbackHighCoefficient = 0.34f;
    float duckAttackCoefficient = 0.0026f;
    float duckReleaseCoefficient = 0.00005f;
    float duckEnvelope = 0.0f;
    int quietSamples = 0;
    bool tailActive = false;

    Smooth predelaySamples, size, motionRate, motionDepthRadians;
    Smooth diffusion, earlyLevel, lateLevel;
    Smooth octaveLevel, fifthLevel, shimmerFeedback;
    Smooth ducking, width, outputGain;
    std::array<PreparedPreset, numPresets> preparedPresets {};
    std::array<int, 8> fdnDelaySamples {};
    std::array<float, 8> fdnFeedbackLow {};
    std::array<float, 8> fdnFeedbackMid {};
    std::array<float, 8> fdnFeedbackHigh {};
    std::array<int, 8> fdnDelaySamplesTarget {};
    std::array<float, 8> fdnFeedbackLowTarget {};
    std::array<float, 8> fdnFeedbackMidTarget {};
    std::array<float, 8> fdnFeedbackHighTarget {};

    PresetTransition presetTransition = PresetTransition::idle;
    int presetRequestHoldSamples = 1200;
    int presetHoldSamplesRemaining = 0;
    float presetMorphPosition = 1.0f;
    float presetMorphStep = 1.0f / 5760.0f;
};
