#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

//==============================================================================
// Protecao final do master, pensada para preservar transientes de piano.
//
// - detector stereo-linked (a imagem estereo nao balanca durante a reducao);
// - 3 ms de lookahead e ataque linear curto, sem hard clip no caminho normal;
// - detector true-peak hibrido: FIR 4x da ITU-R BS.1770-5 mais uma
//   sentinela FIR de banda completa nas fases intermediarias;
// - margem interna de 1,1 dB e teto de saida em -1 dBFS;
// - nenhuma alocacao no callback.
//
// A cadeia de oversampling segue a tecnica da ITU-R BS.1770, mas isto nao e um
// medidor certificado. O resultado tambem e reconstruido em alta resolucao nos
// testes offline, inclusive com sinais adversariais proximos de Nyquist.
class StereoLookaheadLimiter
{
public:
    static constexpr float outputCeilingDb = -1.0f;
    // A margem continua deliberadamente conservadora. Os testes reconstroem a
    // saida em alta resolucao, inclusive com o padrao adversarial period-71.
    static constexpr float detectorCeilingDb = -2.1f;
    static constexpr double lookaheadSeconds = 0.003;

    void prepare (double newSampleRate,
                  [[maybe_unused]] int maximumBlockSize = 8192)
    {
        sampleRate = juce::jmax (8000.0, newSampleRate);
        lookaheadSamples = juce::jmax (
            detectorLatencySamples + 2,
            (int) std::ceil (sampleRate * lookaheadSeconds));
        attackSamples = juce::jlimit (
            1, lookaheadSamples - detectorLatencySamples,
            (int) std::ceil (sampleRate * 0.0008));
        releaseCoefficient = 1.0f - std::exp (
            -1.0f / (float) (sampleRate * 0.120));

        truePeakDetector.prepare();

        for (auto& channel : delay)
            channel.assign ((size_t) lookaheadSamples, 0.0f);

        const int queueCapacity = lookaheadSamples + detectorLatencySamples + 8;
        queueValues.assign ((size_t) queueCapacity, 1.0f);
        queueIndices.assign ((size_t) queueCapacity, 0);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : delay)
            std::fill (channel.begin(), channel.end(), 0.0f);

        truePeakDetector.reset();

        delayIndex = 0;
        queueHead = 0;
        queueSize = 0;
        sampleCounter = 0;
        currentGain = 1.0f;
        attackTarget = 1.0f;
        attackStep = 0.0f;
        attackRemaining = 0;
        emergencyCorrections = 0;
    }

    int getLatencySamples() const noexcept
    {
        return lookaheadSamples;
    }

    float getCurrentGainReductionDb() const noexcept
    {
        return juce::Decibels::gainToDecibels (
            juce::jmax (currentGain, 0.000001f));
    }

    uint64_t getEmergencyCorrectionCount() const noexcept
    {
        return emergencyCorrections;
    }

    void process (juce::AudioBuffer<float>& buffer) noexcept
    {
        juce::ScopedNoDenormals noDenormals;

        const int channels = juce::jmin (2, buffer.getNumChannels());
        const int samples = buffer.getNumSamples();

        if (channels <= 0 || samples <= 0
            || lookaheadSamples <= 0 || delay[0].empty())
        {
            buffer.clear();
            return;
        }

        auto* left = buffer.getWritePointer (0);
        auto* right = channels > 1 ? buffer.getWritePointer (1) : left;
        const float detectorCeiling =
            juce::Decibels::decibelsToGain (detectorCeilingDb);
        const float outputCeiling =
            juce::Decibels::decibelsToGain (outputCeilingDb);

        for (int sample = 0; sample < samples; ++sample)
        {
            const float rawInputL = left[sample];
            const float rawInputR =
                channels > 1 ? right[sample] : rawInputL;
            const bool inputIsSafe =
                isSafeInput (rawInputL) && isSafeInput (rawInputR);
            const float inputL = sanitiseInput (rawInputL);
            const float inputR = sanitiseInput (rawInputR);

            if (! inputIsSafe)
                ++emergencyCorrections;

            const float detectedPeak = truePeakDetector.process (
                inputL, inputR, detectorCeiling);
            const float requiredGain =
                detectedPeak > detectorCeiling
                    ? detectorCeiling / detectedPeak
                    : 1.0f;
            const float target = pushRequiredGain (
                juce::jlimit (0.0f, 1.0f, requiredGain));
            updateGain (target);

            const float delayedL = delay[0][(size_t) delayIndex];
            const float delayedR = delay[1][(size_t) delayIndex];
            delay[0][(size_t) delayIndex] = inputL;
            delay[1][(size_t) delayIndex] = inputR;

            if (++delayIndex >= lookaheadSamples)
                delayIndex = 0;

            float outputL = delayedL * currentGain;
            float outputR = delayedR * currentGain;
            const float samplePeak = juce::jmax (
                std::abs (outputL), std::abs (outputR));

            // Cinto de seguranca para estado numerico corrompido ou pico
            // de amostra inesperado. Escala o par estereo; nao hard-clippa.
            if (std::isfinite (samplePeak)
                && samplePeak > outputCeiling)
            {
                const float correction = outputCeiling / samplePeak;
                outputL *= correction;
                outputR *= correction;
                currentGain *= correction;
                attackTarget = juce::jmin (attackTarget, currentGain);
                attackRemaining = 0;
                attackStep = 0.0f;
                ++emergencyCorrections;
            }
            else if (! std::isfinite (samplePeak))
            {
                outputL = 0.0f;
                outputR = 0.0f;
                currentGain = 0.0f;
                attackTarget = 0.0f;
                ++emergencyCorrections;
            }

            left[sample] = outputL;

            if (channels > 1)
                right[sample] = outputR;

            ++sampleCounter;
        }
    }

private:
    // A parte curta usa os 48 coeficientes de referencia da ITU-R
    // BS.1770-5. Ela e complementada por quatro fases de sinc-Lanczos longo
    // nos oitavos intermediarios. Essa sentinela conserva a ultima oitava do
    // PCM (inclusive sequencias adversariais quase-Nyquist) sem executar os
    // tres estagios genericos de oversampling que ocupavam ~62% do callback
    // no TANK 3. Os coeficientes longos sao calculados apenas no prepare().
    class TruePeakDetector4x
    {
    public:
        void prepare() noexcept
        {
            maximumPhaseL1 = officialMaximumPhaseL1;

            for (int phase = 0; phase < guardPhases; ++phase)
            {
                const double fraction =
                    (double) (phase * 2 + 1) / 8.0;
                const double centre =
                    ((double) guardTaps - 1.0) * 0.5;
                double phaseL1 = 0.0;

                for (int tap = 0; tap < guardTaps; ++tap)
                {
                    const double distance =
                        centre - (double) tap + fraction;
                    const double normalised =
                        distance / ((double) guardTaps * 0.5);
                    const double coefficient =
                        sinc (distance) * sinc (normalised);
                    guardCoefficients[(size_t) phase][(size_t) tap] =
                        (float) coefficient;
                    phaseL1 += std::abs (coefficient);
                }

                maximumPhaseL1 = juce::jmax (
                    maximumPhaseL1, (float) phaseL1);
            }
        }

        void reset() noexcept
        {
            for (auto& channel : officialHistory)
                channel.fill (0.0f);

            for (auto& channel : guardHistory)
                channel.fill (0.0f);

            aboveSafeThreshold.fill (0);
            officialWriteIndex = 0;
            guardWriteIndex = 0;
        }

        float process (float left, float right,
                       float detectorCeiling) noexcept
        {
            const std::array<float, 2> input { left, right };
            const float safeThreshold = detectorCeiling / maximumPhaseL1;

            for (int channel = 0; channel < 2; ++channel)
            {
                const float old =
                    guardHistory[(size_t) channel][(size_t) guardWriteIndex];

                if (std::abs (old) > safeThreshold && aboveSafeThreshold[(size_t) channel] > 0)
                    --aboveSafeThreshold[(size_t) channel];

                officialHistory[(size_t) channel]
                               [(size_t) officialWriteIndex] =
                    input[(size_t) channel];
                guardHistory[(size_t) channel][(size_t) guardWriteIndex] =
                    input[(size_t) channel];
                guardHistory[(size_t) channel]
                            [(size_t) guardWriteIndex + guardTaps] =
                    input[(size_t) channel];

                if (std::abs (input[(size_t) channel]) > safeThreshold)
                    ++aboveSafeThreshold[(size_t) channel];
            }

            float peak = juce::jmax (std::abs (left), std::abs (right));

            // Limite matematico do FIR: |y| <= max(|x|) * soma(|h|).
            // Abaixo deste ponto nenhum pico interpolado pode atingir o teto,
            // portanto pular os MACs e exato para a decisao do limiter.
            if (aboveSafeThreshold[0] == 0
                 && aboveSafeThreshold[1] == 0)
            {
                advance();
                return peak;
            }

            for (int channel = 0; channel < 2; ++channel)
            {
                for (const auto& phase : officialCoefficients)
                {
                    float value = 0.0f;
                    int readIndex = officialWriteIndex;

                    for (int tap = 0; tap < officialTapsPerPhase; ++tap)
                    {
                        value += officialHistory[(size_t) channel]
                                                [(size_t) readIndex]
                               * phase[(size_t) tap];

                        if (--readIndex < 0)
                            readIndex = officialTapsPerPhase - 1;
                    }

                    peak = juce::jmax (peak, std::abs (value));
                }

                const auto* guardSamples =
                    guardHistory[(size_t) channel].data() + guardWriteIndex;

                for (const auto& phase : guardCoefficients)
                {
                    float value = 0.0f;

                    for (int tap = 0; tap < guardTaps; ++tap)
                        value += guardSamples[tap] * phase[(size_t) tap];

                    peak = juce::jmax (peak, std::abs (value));
                }
            }

            advance();
            return peak;
        }

    private:
        void advance() noexcept
        {
            if (++officialWriteIndex >= officialTapsPerPhase)
                officialWriteIndex = 0;

            if (--guardWriteIndex < 0)
                guardWriteIndex = guardTaps - 1;
        }

        static double sinc (double value) noexcept
        {
            if (std::abs (value) < 1.0e-12)
                return 1.0;

            const double angle =
                juce::MathConstants<double>::pi * value;
            return std::sin (angle) / angle;
        }

        static constexpr int officialPhases = 4;
        static constexpr int officialTapsPerPhase = 12;
        static constexpr int guardPhases = 4;
        static constexpr int guardTaps = 144;
        static constexpr float officialMaximumPhaseL1 =
            2.0228271484375f;

        static constexpr std::array<
            std::array<float, officialTapsPerPhase>, officialPhases>
            officialCoefficients {{
                {{  0.0017089843750f,  0.0109863281250f,
                   -0.0196533203125f,  0.0332031250000f,
                   -0.0594482421875f,  0.1373291015625f,
                    0.9721679687500f, -0.1022949218750f,
                    0.0476074218750f, -0.0266113281250f,
                    0.0148925781250f, -0.0083007812500f }},
                {{ -0.0291748046875f,  0.0292968750000f,
                   -0.0517578125000f,  0.0891113281250f,
                   -0.1665039062500f,  0.4650878906250f,
                    0.7797851562500f, -0.2003173828125f,
                    0.1015625000000f, -0.0582275390625f,
                    0.0330810546875f, -0.0189208984375f }},
                {{ -0.0189208984375f,  0.0330810546875f,
                   -0.0582275390625f,  0.1015625000000f,
                   -0.2003173828125f,  0.7797851562500f,
                    0.4650878906250f, -0.1665039062500f,
                    0.0891113281250f, -0.0517578125000f,
                    0.0292968750000f, -0.0291748046875f }},
                {{ -0.0083007812500f,  0.0148925781250f,
                   -0.0266113281250f,  0.0476074218750f,
                   -0.1022949218750f,  0.9721679687500f,
                    0.1373291015625f, -0.0594482421875f,
                    0.0332031250000f, -0.0196533203125f,
                    0.0109863281250f,  0.0017089843750f }}
            }};

        std::array<
            std::array<float, officialTapsPerPhase>, 2> officialHistory {};
        std::array<std::array<float, guardTaps * 2>, 2> guardHistory {};
        std::array<std::array<float, guardTaps>, guardPhases>
            guardCoefficients {};
        std::array<int, 2> aboveSafeThreshold {};
        float maximumPhaseL1 = officialMaximumPhaseL1;
        int officialWriteIndex = 0;
        int guardWriteIndex = 0;
    };

    static constexpr int detectorLatencySamples = 72;
    static constexpr float maximumSafeInputMagnitude = 64.0f;

    static bool isSafeInput (float value) noexcept
    {
        return std::isfinite (value)
            && std::abs (value) <= maximumSafeInputMagnitude;
    }

    static float sanitiseInput (float value) noexcept
    {
        if (! std::isfinite (value))
            return 0.0f;

        return juce::jlimit (
            -maximumSafeInputMagnitude,
            maximumSafeInputMagnitude,
            value);
    }

    int queuePosition (int logicalIndex) const noexcept
    {
        const int capacity = (int) queueValues.size();
        const int position = queueHead + logicalIndex;
        return position < capacity ? position : position - capacity;
    }

    float pushRequiredGain (float requiredGain) noexcept
    {
        const uint64_t window =
            (uint64_t) (lookaheadSamples + detectorLatencySamples);

        while (queueSize > 0)
        {
            const int front = queueHead;

            if (queueIndices[(size_t) front] + window > sampleCounter)
                break;

            queueHead = queuePosition (1);
            --queueSize;
        }

        while (queueSize > 0)
        {
            const int back = queuePosition (queueSize - 1);

            if (queueValues[(size_t) back] < requiredGain)
                break;

            --queueSize;
        }

        const int insertion = queuePosition (queueSize);
        queueValues[(size_t) insertion] = requiredGain;
        queueIndices[(size_t) insertion] = sampleCounter;
        ++queueSize;
        return queueValues[(size_t) queueHead];
    }

    void updateGain (float target) noexcept
    {
        if (target < attackTarget - 0.0000001f)
        {
            attackTarget = target;
            attackRemaining = attackSamples;
            attackStep = (attackTarget - currentGain)
                       / (float) attackRemaining;
        }

        if (attackRemaining > 0)
        {
            currentGain += attackStep;

            if (--attackRemaining == 0 || currentGain <= attackTarget)
            {
                currentGain = attackTarget;
                attackRemaining = 0;
            }

            return;
        }

        if (target < currentGain)
        {
            currentGain = target;
            attackTarget = target;
            return;
        }

        if (target > currentGain)
        {
            currentGain += releaseCoefficient * (target - currentGain);

            if (std::abs (target - currentGain) < 0.000001f)
                currentGain = target;
        }

        attackTarget = target;
    }

    double sampleRate = 48000.0;
    int lookaheadSamples = 144;
    int attackSamples = 39;
    int delayIndex = 0;
    float releaseCoefficient = 0.0001736f;
    float currentGain = 1.0f;
    float attackTarget = 1.0f;
    float attackStep = 0.0f;
    int attackRemaining = 0;

    std::array<std::vector<float>, 2> delay;
    TruePeakDetector4x truePeakDetector;

    std::vector<float> queueValues;
    std::vector<uint64_t> queueIndices;
    int queueHead = 0;
    int queueSize = 0;
    uint64_t sampleCounter = 0;
    uint64_t emergencyCorrections = 0;
};
