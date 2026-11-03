#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>

namespace ChurchDAWRotary
{
    // Leslie de dois rotores para os canais de orgao.
    //
    // - crossover em 800 Hz;
    // - horn e drum com velocidades/inercia independentes e sentidos opostos;
    // - Doppler por delay fracionario;
    // - movimento de amplitude/estereo;
    // - nenhuma alocacao ou lock no callback.
    //
    // Mode segue o half-moon fisico: 0=SLOW, 1=STOP, 2=FAST.
    class Speaker
    {
    public:
        static constexpr int slowMode = 0;
        static constexpr int stopMode = 1;
        static constexpr int fastMode = 2;

        void prepare (double newSampleRate, int maximumBlockSize)
        {
            sampleRate = juce::jmax (8000.0, newSampleRate);
            const int capacity = juce::jmax (
                128, (int) std::ceil (sampleRate * 0.020)
                         + juce::jmax (1, maximumBlockSize) + 8);

            delay.setSize (4, capacity, false, false, true);
            mix.reset (sampleRate, 0.055);

            const double pole = std::exp (
                -juce::MathConstants<double>::twoPi * 800.0 / sampleRate);
            crossoverPole = (float) juce::jlimit (0.0, 0.99999, pole);
            crossoverFeed = 1.0f - crossoverPole;
            loudnessDetectorStep = (float) (1.0
                - std::exp (-1.0 / (sampleRate * 1.5)));
            loudnessGainStep = (float) (1.0
                - std::exp (-1.0 / (sampleRate * 3.5)));
            reset();
        }

        void reset() noexcept
        {
            clearSignalHistory();
            hornPhase = 0.0;
            drumPhase = juce::MathConstants<double>::pi * 0.5;
            hornRateHz = 0.0;
            drumRateHz = 0.0;
            mix.setCurrentAndTargetValue (0.0f);
            resetLoudnessCompensation();
        }

        void clearSignalHistory() noexcept
        {
            delay.clear();
            writeIndex = 0;
            lowState.fill (0.0f);
            historyHasSignal = false;
        }

        void process (juce::AudioBuffer<float>& buffer,
                      int numSamples,
                      float amount,
                      int mode) noexcept
        {
            juce::ScopedNoDenormals noDenormals;

            if (buffer.getNumChannels() <= 0 || delay.getNumSamples() <= 0)
                return;

            numSamples = juce::jlimit (0, buffer.getNumSamples(), numSamples);
            amount = std::isfinite (amount)
                         ? juce::jlimit (0.0f, 1.0f, amount)
                         : 0.0f;
            mode = juce::jlimit (slowMode, fastMode, mode);

            const double hornTarget = mode == fastMode ? 6.67
                                     : mode == slowMode ? 0.80 : 0.0;
            const double drumTarget = mode == fastMode ? 5.67
                                     : mode == slowMode ? 0.67 : 0.0;

            // STOP significa rotores fisicamente parados, mas o som continua
            // atravessando a caixa. Bypass e Amount=0, inclusive em estados
            // antigos migrados. Isso preserva a inercia real Slow/Stop/Fast.
            mix.setTargetValue (amount);

            if (mix.getCurrentValue() <= 1.0e-6f
                 && mix.getTargetValue() <= 1.0e-6f)
            {
                // Nunca deixe alguns milissegundos do timbre anterior
                // congelados no delay para reaparecer quando o Rotary voltar.
                if (historyHasSignal)
                    clearSignalHistory();

                // Amount=0 is a true bypass. Detector and correction state
                // from the previous register must not survive here: otherwise
                // a later re-enable starts with that stale gain and pumps for
                // several seconds while the slow compensator relearns.
                resetLoudnessCompensation();

                // Caminho comum de piano/pad e de estados antigos: nenhum loop
                // por amostra e nenhum exp quando os rotores ja estao parados.
                if (hornTarget <= 1.0e-9 && drumTarget <= 1.0e-9
                     && std::abs (hornRateHz) <= 1.0e-9
                     && std::abs (drumRateHz) <= 1.0e-9)
                    return;

                const double hornCoeff = smoothingCoefficient (
                    transitionSeconds (
                        hornRateHz, hornTarget, 0.90, 1.20, 1.40));
                const double drumCoeff = smoothingCoefficient (
                    transitionSeconds (
                        drumRateHz, drumTarget, 3.20, 4.50, 5.00));
                advanceMotionSilently (
                    numSamples, hornTarget, drumTarget,
                    hornCoeff, drumCoeff);
                return;
            }

            const double hornCoeff = smoothingCoefficient (
                transitionSeconds (
                    hornRateHz, hornTarget, 0.90, 1.20, 1.40));
            const double drumCoeff = smoothingCoefficient (
                transitionSeconds (
                    drumRateHz, drumTarget, 3.20, 4.50, 5.00));

            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getNumChannels() > 1
                              ? buffer.getWritePointer (1)
                              : left;
            historyHasSignal = true;
            const bool depthIsMoving = mix.isSmoothing();
            const float steadyDepth = mix.getTargetValue();

            for (int sample = 0; sample < numSamples; ++sample)
            {
                hornRateHz += (hornTarget - hornRateHz) * hornCoeff;
                drumRateHz += (drumTarget - drumRateHz) * drumCoeff;

                hornPhase = wrapPhase (
                    hornPhase + juce::MathConstants<double>::twoPi
                                  * hornRateHz / sampleRate);
                // Rotores fisicos giram em direcoes opostas.
                drumPhase = wrapPhase (
                    drumPhase - juce::MathConstants<double>::twoPi
                                  * drumRateHz / sampleRate);

                const float inL = std::isfinite (left[sample]) ? left[sample] : 0.0f;
                const float inR = std::isfinite (right[sample]) ? right[sample] : inL;

                lowState[0] = crossoverFeed * inL + crossoverPole * lowState[0];
                lowState[1] = crossoverFeed * inR + crossoverPole * lowState[1];

                const float lowL = lowState[0];
                const float lowR = lowState[1];
                const float highL = inL - lowL;
                const float highR = inR - lowR;

                delay.setSample (0, writeIndex, lowL);
                delay.setSample (1, writeIndex, lowR);
                delay.setSample (2, writeIndex, highL);
                delay.setSample (3, writeIndex, highR);

                // A senoide move apenas parametros do Leslie (nao e o audio
                // em si). Esta aproximacao suave evita duas chamadas libm por
                // sample, importante com varios orgaos no Android.
                const float hornSin = fastSin (hornPhase);
                const float drumSin = fastSin (drumPhase);
                const float currentDepth =
                    depthIsMoving ? mix.getNextValue() : steadyDepth;
                const float motionPresence = juce::jlimit (
                    0.0f, 1.0f,
                    (float) ((std::abs (hornRateHz)
                              + std::abs (drumRateHz)) * 0.75));
                const float modulationDepth =
                    currentDepth * motionPresence;

                // Amount now controls physical modulation depth. Both rotor
                // bands share the cabinet propagation delay; only their
                // Doppler excursions differ. At zero depth the delay is zero
                // and low+high reconstroi o input, permitindo entrada suave
                // no modo Amount=0 sem artefatos de transicao.
                const double cabinetDelaySeconds =
                    0.00235 * (double) currentDepth;
                const float hornDelay = (float) (sampleRate
                    * (cabinetDelaySeconds
                       + 0.00052 * (double) modulationDepth
                                     * (double) hornSin));
                const float drumDelay = (float) (sampleRate
                    * (cabinetDelaySeconds
                       + 0.00022 * (double) modulationDepth
                                     * (double) drumSin));

                const float delayedLowL = readFractional (0, drumDelay);
                const float delayedLowR = readFractional (1, drumDelay);
                const float delayedHighL = readFractional (2, hornDelay);
                const float delayedHighR = readFractional (3, hornDelay);

                constexpr float stereo = 0.62f;

                // A Leslie e um cabinet inserido, nao um chorus dry/wet.
                // Somar o sinal direto a uma copia atrasada criava comb
                // filtering e podia apagar fundamentais quando Amount subia.
                // A base abaixo atravessa o mesmo atraso de propagacao do
                // cabinet; ela preserva corpo/ataque sem criar um segundo
                // caminho direto concorrente.
                const float baseDelay =
                    (float) (sampleRate * cabinetDelaySeconds);
                const float cabinetBaseL =
                    readFractional (0, baseDelay)
                  + readFractional (2, baseDelay);
                const float cabinetBaseR =
                    readFractional (1, baseDelay)
                  + readFractional (3, baseDelay);

                const float rotorL =
                    delayedLowL
                        * (1.0f
                           + modulationDepth * 0.12f * stereo * drumSin)
                  + delayedHighL
                        * (1.0f
                           + modulationDepth * 0.22f * stereo * hornSin);
                const float rotorR =
                    delayedLowR
                        * (1.0f
                           - modulationDepth * 0.12f * stereo * drumSin)
                  + delayedHighR
                        * (1.0f
                           - modulationDepth * 0.22f * stereo * hornSin);

                // Mesmo em Amount maximo, 32% do cabinet comum permanece.
                // Isso limita nulos entre horn/drum no crossover sem reduzir o
                // movimento espacial ou a variacao Doppler do caminho rotor.
                const float rotorWeight = 0.68f * currentDepth;
                const float baseWeight = 1.0f - rotorWeight;
                const float rawOutputL =
                    cabinetBaseL * baseWeight + rotorL * rotorWeight;
                const float rawOutputR =
                    cabinetBaseR * baseWeight + rotorR * rotorWeight;

                // Um detector de 1,5 s e um ganho de 3,5 s compensam apenas a
                // energia integrada do cabinet. Sao muito mais lentos que os
                // rotores e que a musica, portanto nao funcionam como AGC nem
                // "respiram" junto com as notas. O limite evita elevar ruido
                // ou picos em uma entrada anormal.
                const bool stereoOutput = buffer.getNumChannels() > 1;
                const float inputPower = stereoOutput
                    ? 0.5f * (inL * inL + inR * inR)
                    : inL * inL;
                const float outputPower = stereoOutput
                    ? 0.5f * (rawOutputL * rawOutputL
                              + rawOutputR * rawOutputR)
                    : rawOutputL * rawOutputL;
                inputPowerState += loudnessDetectorStep
                    * (inputPower - inputPowerState);
                outputPowerState += loudnessDetectorStep
                    * (outputPower - outputPowerState);

                if (++loudnessUpdateCounter >= 64U)
                {
                    loudnessUpdateCounter = 0;

                    if (inputPowerState > 1.0e-10f
                         && outputPowerState > 1.0e-11f)
                        targetLoudnessGain = juce::jlimit (
                            0.72f, 1.65f,
                            std::sqrt (
                                inputPowerState / outputPowerState));
                }

                loudnessGain += loudnessGainStep
                    * (targetLoudnessGain - loudnessGain);
                const float outputL = rawOutputL * loudnessGain;
                const float outputR = rawOutputR * loudnessGain;

                left[sample] = std::isfinite (outputL) ? outputL : 0.0f;

                if (buffer.getNumChannels() > 1)
                    right[sample] =
                        std::isfinite (outputR) ? outputR : 0.0f;

                if (++writeIndex >= delay.getNumSamples())
                    writeIndex = 0;
            }
        }

        // Mantem a fisica Slow/Stop/Fast durante o silencio sem processar
        // crossover, delay ou trigonometria por sample. Assim FAST->STOP nao
        // congela quando a ultima nota termina e o proximo ataque ja encontra
        // os rotores na velocidade correta.
        void advanceSilently (int numSamples, int mode) noexcept
        {
            numSamples = juce::jmax (0, numSamples);
            mode = juce::jlimit (slowMode, fastMode, mode);

            if (numSamples == 0)
                return;

            const double hornTarget = mode == fastMode ? 6.67
                                     : mode == slowMode ? 0.80 : 0.0;
            const double drumTarget = mode == fastMode ? 5.67
                                     : mode == slowMode ? 0.67 : 0.0;

            if (hornTarget <= 1.0e-9 && drumTarget <= 1.0e-9
                 && std::abs (hornRateHz) <= 1.0e-9
                 && std::abs (drumRateHz) <= 1.0e-9)
                return;

            advanceMotionSilently (
                numSamples, hornTarget, drumTarget,
                smoothingCoefficient (transitionSeconds (
                    hornRateHz, hornTarget, 0.90, 1.20, 1.40)),
                smoothingCoefficient (transitionSeconds (
                    drumRateHz, drumTarget, 3.20, 4.50, 5.00)));
        }

        double getCurrentHornRateHzForTest() const noexcept { return hornRateHz; }
        double getCurrentDrumRateHzForTest() const noexcept { return drumRateHz; }

    private:
        void resetLoudnessCompensation() noexcept
        {
            inputPowerState = 0.0f;
            outputPowerState = 0.0f;
            targetLoudnessGain = 1.0f;
            loudnessGain = 1.0f;
            loudnessUpdateCounter = 0;
        }

        static double transitionSeconds (double current,
                                         double target,
                                         double rise,
                                         double fall,
                                         double brake) noexcept
        {
            if (target <= 1.0e-9)
                return brake;

            return target > current ? rise : fall;
        }

        double smoothingCoefficient (double seconds) const noexcept
        {
            return 1.0 - std::exp (-1.0 / (sampleRate * juce::jmax (0.001, seconds)));
        }

        void advanceMotionSilently (int samples,
                                    double hornTarget,
                                    double drumTarget,
                                    double hornCoeff,
                                    double drumCoeff) noexcept
        {
            const auto advanceRotor = [this, samples] (
                double& rate,
                double target,
                double coefficient,
                double& phase,
                double direction) noexcept
            {
                const double q = juce::jlimit (
                    0.0, 1.0, 1.0 - coefficient);
                const double qn = std::pow (q, (double) samples);
                const double initialOffset = rate - target;
                double summedRate = target * (double) samples;

                if (coefficient > 1.0e-15)
                    summedRate += initialOffset * q * (1.0 - qn)
                                  / coefficient;
                else
                    summedRate += initialOffset * (double) samples;

                rate = target + initialOffset * qn;
                phase = wrapPhase (
                    phase + direction
                          * juce::MathConstants<double>::twoPi
                          * summedRate / sampleRate);
            };

            advanceRotor (
                hornRateHz, hornTarget, hornCoeff, hornPhase, 1.0);
            advanceRotor (
                drumRateHz, drumTarget, drumCoeff, drumPhase, -1.0);
        }

        static double wrapPhase (double phase) noexcept
        {
            const auto twoPi = juce::MathConstants<double>::twoPi;
            phase = std::fmod (phase, twoPi);
            return phase < 0.0 ? phase + twoPi : phase;
        }

        static float fastSin (double phase) noexcept
        {
            constexpr float pi = juce::MathConstants<float>::pi;
            constexpr float twoPi = juce::MathConstants<float>::twoPi;
            float x = (float) phase;

            if (x > pi)
                x -= twoPi;

            // Parabola corrigida (erro pequeno e continuo); suficiente para
            // LFO/Doppler de ate 6,67 Hz e muito mais barata que libm.
            constexpr float b = 4.0f / pi;
            constexpr float c = -4.0f / (pi * pi);
            constexpr float correction = 0.225f;
            float y = b * x + c * x * std::abs (x);
            return correction * (y * std::abs (y) - y) + y;
        }

        float readFractional (int channel, float delaySamples) const noexcept
        {
            const int size = delay.getNumSamples();
            if (size <= 0 || ! std::isfinite (delaySamples) || channel < 0 || channel >= delay.getNumChannels())
                return 0.0f;

            float position = std::fmod ((float) writeIndex - delaySamples, (float) size);
            if (position < 0.0f)
                position += (float) size;

            const int index0 = juce::jlimit (0, size - 1, (int) position);
            const int index1 = index0 + 1 < size ? index0 + 1 : 0;
            const float fraction = position - (float) index0;
            const float a = delay.getSample (channel, index0);
            const float b = delay.getSample (channel, index1);
            return a + fraction * (b - a);
        }

        double sampleRate = 48000.0;
        double hornPhase = 0.0;
        double drumPhase = 0.0;
        double hornRateHz = 0.0;
        double drumRateHz = 0.0;
        float crossoverPole = 0.9f;
        float crossoverFeed = 0.1f;
        std::array<float, 2> lowState {};
        juce::AudioBuffer<float> delay;
        int writeIndex = 0;
        bool historyHasSignal = false;
        float loudnessDetectorStep = 0.0f;
        float loudnessGainStep = 0.0f;
        float inputPowerState = 0.0f;
        float outputPowerState = 0.0f;
        float targetLoudnessGain = 1.0f;
        float loudnessGain = 1.0f;
        std::uint32_t loudnessUpdateCounter = 0;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mix;
    };
}

