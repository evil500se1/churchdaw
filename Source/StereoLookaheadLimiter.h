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
// - detector true-peak FIR equiripple em 8x;
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
    // O detector 8x subestima em ~1,02 dB o padrao adversarial period-71
    // reconstruido offline em alta resolucao. -2,1 dB e o ponto mais alto que
    // ainda preserva o teto verdadeiro de -1 dBFS com margem mensuravel.
    static constexpr float detectorCeilingDb = -2.1f;
    static constexpr double lookaheadSeconds = 0.003;

    void prepare (double newSampleRate, int maximumBlockSize = 8192)
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

        for (auto& channel : delay)
            channel.assign ((size_t) lookaheadSamples, 0.0f);

        const int queueCapacity = lookaheadSamples + detectorLatencySamples + 8;
        queueValues.assign ((size_t) queueCapacity, 1.0f);
        queueIndices.assign ((size_t) queueCapacity, 0);
        detectorCapacity = juce::jmax (1, maximumBlockSize);
        detectorInput.setSize (
            2, detectorCapacity, false, false, true);
        detectedPeaks.assign ((size_t) detectorCapacity, 0.0f);

        // O primeiro half-band e deliberadamente estreito: ele preserva a
        // ultima oitava do PCM, onde interpoladores curtos subestimavam picos.
        detectorOversampling.clearOversamplingStages();
        detectorOversampling.addOversamplingStage (
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandFIREquiripple,
            0.015f, -90.0f, 0.10f, -60.0f);
        detectorOversampling.addOversamplingStage (
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandFIREquiripple,
            0.10f, -80.0f, 0.10f, -60.0f);
        detectorOversampling.addOversamplingStage (
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandFIREquiripple,
            0.10f, -70.0f, 0.10f, -60.0f);
        detectorOversampling.initProcessing ((size_t) detectorCapacity);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : delay)
            std::fill (channel.begin(), channel.end(), 0.0f);

        detectorInput.clear();
        detectorOversampling.reset();

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
        const int channels = juce::jmin (2, buffer.getNumChannels());
        const int samples = buffer.getNumSamples();

        if (channels <= 0 || samples <= 0
            || lookaheadSamples <= 0 || delay[0].empty()
            || detectorCapacity <= 0 || detectedPeaks.empty())
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

        for (int chunkStart = 0;
             chunkStart < samples;
             chunkStart += detectorCapacity)
        {
            const int count =
                juce::jmin (detectorCapacity, samples - chunkStart);
            auto* detectorLeft = detectorInput.getWritePointer (0);
            auto* detectorRight = detectorInput.getWritePointer (1);

            for (int local = 0; local < count; ++local)
            {
                const int sample = chunkStart + local;
                const float rawL = left[sample];
                const float rawR = channels > 1 ? right[sample] : rawL;
                detectorLeft[local] = sanitiseInput (rawL);
                detectorRight[local] = sanitiseInput (rawR);
            }

            juce::dsp::AudioBlock<float> detectorBlock (detectorInput);
            auto detectorSlice =
                detectorBlock.getSubBlock (0, (size_t) count);
            const juce::dsp::AudioBlock<const float> readOnlySlice (
                detectorSlice);
            const auto oversampled =
                detectorOversampling.processSamplesUp (readOnlySlice);
            bool detectorFault = false;

            for (int local = 0; local < count; ++local)
            {
                float peak = juce::jmax (
                    std::abs (detectorLeft[local]),
                    std::abs (detectorRight[local]));

                for (int channel = 0; channel < 2; ++channel)
                    for (int phase = 0;
                         phase < detectorOversamplingFactor;
                         ++phase)
                    {
                        const float value = oversampled.getSample (
                            (size_t) channel,
                            (size_t) local * detectorOversamplingFactor
                                + (size_t) phase);

                        if (std::isfinite (value))
                            peak = juce::jmax (peak, std::abs (value));
                        else
                            detectorFault = true;
                    }

                detectedPeaks[(size_t) local] =
                    detectorFault ? maximumSafeInputMagnitude : peak;
            }

            if (detectorFault)
            {
                std::fill_n (
                    detectedPeaks.begin(), count,
                    maximumSafeInputMagnitude);
                detectorOversampling.reset();
                ++emergencyCorrections;
            }

            for (int local = 0; local < count; ++local)
            {
                const int sample = chunkStart + local;
                const float rawInputL = left[sample];
                const float rawInputR =
                    channels > 1 ? right[sample] : rawInputL;
                const bool inputIsSafe =
                    isSafeInput (rawInputL) && isSafeInput (rawInputR);
                const float inputL = sanitiseInput (rawInputL);
                const float inputR = sanitiseInput (rawInputR);

                if (! inputIsSafe)
                    ++emergencyCorrections;

                const float detectedPeak =
                    detectedPeaks[(size_t) local];
                const float requiredGain =
                    detectedPeak > detectorCeiling
                        ? detectorCeiling / detectedPeak
                        : 1.0f;
                const float target = pushRequiredGain (
                    juce::jlimit (0.0f, 1.0f, requiredGain));
                updateGain (target);

                const float delayedL =
                    delay[0][(size_t) delayIndex];
                const float delayedR =
                    delay[1][(size_t) delayIndex];
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
                    const float correction =
                        outputCeiling / samplePeak;
                    outputL *= correction;
                    outputR *= correction;
                    currentGain *= correction;
                    attackTarget =
                        juce::jmin (attackTarget, currentGain);
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
    }

private:
    static constexpr size_t detectorOversamplingFactor = 8;
    static constexpr int detectorLatencySamples = 106;
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
    int detectorCapacity = 0;
    float releaseCoefficient = 0.0001736f;
    float currentGain = 1.0f;
    float attackTarget = 1.0f;
    float attackStep = 0.0f;
    int attackRemaining = 0;

    std::array<std::vector<float>, 2> delay;
    juce::dsp::Oversampling<float> detectorOversampling { 2 };
    juce::AudioBuffer<float> detectorInput;
    std::vector<float> detectedPeaks;

    std::vector<float> queueValues;
    std::vector<uint64_t> queueIndices;
    int queueHead = 0;
    int queueSize = 0;
    uint64_t sampleCounter = 0;
    uint64_t emergencyCorrections = 0;
};
