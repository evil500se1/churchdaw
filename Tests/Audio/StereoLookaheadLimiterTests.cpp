#include "StereoLookaheadLimiter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace
{
struct Render
{
    std::vector<float> inputLeft;
    std::vector<float> inputRight;
    std::vector<float> outputLeft;
    std::vector<float> outputRight;
    float samplePeak = 0.0f;
    float finalReductionDb = 0.0f;
    uint64_t emergencyCorrections = 0;
    int latency = 0;
    bool finite = true;
};

double sinc (double value) noexcept
{
    if (std::abs (value) < 1.0e-12)
        return 1.0;

    const double angle = juce::MathConstants<double>::pi * value;
    return std::sin (angle) / angle;
}

double reconstructedPeak (const std::vector<float>& samples)
{
    constexpr int radius = 16;
    constexpr int phases = 16;
    double peak = 0.0;

    for (size_t centre = radius;
         centre + radius + 1 < samples.size();
         ++centre)
    {
        peak = juce::jmax (peak, (double) std::abs (samples[centre]));

        for (int phaseIndex = 1; phaseIndex < phases; ++phaseIndex)
        {
            const double phase = (double) phaseIndex / (double) phases;
            double value = 0.0;
            double normalisation = 0.0;

            for (int offset = -radius + 1; offset <= radius; ++offset)
            {
                const double distance = phase - (double) offset;
                const double coefficient =
                    sinc (distance) * sinc (distance / (double) radius);
                value += (double) samples[
                    centre + (size_t) (offset + radius - 1)
                    - (size_t) (radius - 1)] * coefficient;
                normalisation += coefficient;
            }

            if (std::abs (normalisation) > 1.0e-12)
                peak = juce::jmax (
                    peak, std::abs (value / normalisation));
        }
    }

    return peak;
}

double periodicBandlimitedPeak (const std::vector<float>& period)
{
    const int size = (int) period.size();
    std::vector<std::complex<double>> spectrum ((size_t) size);

    for (int bin = 0; bin < size; ++bin)
    {
        std::complex<double> coefficient {};

        for (int sample = 0; sample < size; ++sample)
        {
            const double angle =
                -juce::MathConstants<double>::twoPi
                * (double) bin * (double) sample / (double) size;
            coefficient += (double) period[(size_t) sample]
                         * std::polar (1.0, angle);
        }

        spectrum[(size_t) bin] = coefficient / (double) size;
    }

    constexpr int reconstructionPoints = 131072;
    double peak = 0.0;

    for (int point = 0; point < reconstructionPoints; ++point)
    {
        const double time =
            (double) size * (double) point
            / (double) reconstructionPoints;
        std::complex<double> value {};

        for (int bin = 0; bin < size; ++bin)
        {
            const int signedBin =
                bin <= size / 2 ? bin : bin - size;
            const double angle =
                juce::MathConstants<double>::twoPi
                * (double) signedBin * time / (double) size;
            value += spectrum[(size_t) bin] * std::polar (1.0, angle);
        }

        if (! std::isfinite (value.real()))
            return std::numeric_limits<double>::infinity();

        peak = juce::jmax (peak, std::abs (value.real()));
    }

    return peak;
}

double finiteBandlimitedPeak (const std::vector<float>& samples)
{
    std::vector<std::pair<int, double>> nonzero;

    for (int sample = 0; sample < (int) samples.size(); ++sample)
        if (std::abs (samples[(size_t) sample]) > 1.0e-12f)
            nonzero.emplace_back (
                sample, (double) samples[(size_t) sample]);

    if (nonzero.empty())
        return 0.0;

    const double start = (double) nonzero.front().first - 8.0;
    const double end = (double) nonzero.back().first + 8.0;
    constexpr int reconstructionPoints = 131072;
    double peak = 0.0;

    for (int point = 0; point < reconstructionPoints; ++point)
    {
        const double time = start
            + (end - start) * (double) point
            / (double) (reconstructionPoints - 1);
        double value = 0.0;

        for (const auto& item : nonzero)
            value += item.second * sinc (time - (double) item.first);

        if (! std::isfinite (value))
            return std::numeric_limits<double>::infinity();

        peak = juce::jmax (peak, std::abs (value));
    }

    return peak;
}

Render render (double sampleRate, int blockSize,
               double signalSeconds, double silenceSeconds,
               float scale, bool proportionalStereo)
{
    const int signalSamples =
        (int) std::llround (signalSeconds * sampleRate);
    const int totalSamples =
        (int) std::llround ((signalSeconds + silenceSeconds) * sampleRate);
    Render result;
    result.inputLeft.resize ((size_t) totalSamples, 0.0f);
    result.inputRight.resize ((size_t) totalSamples, 0.0f);
    result.outputLeft.resize ((size_t) totalSamples, 0.0f);
    result.outputRight.resize ((size_t) totalSamples, 0.0f);

    for (int sample = 0; sample < signalSamples; ++sample)
    {
        const double seconds = (double) sample / sampleRate;
        const float attack = juce::jmin (
            1.0f, (float) sample / (float) (sampleRate * 0.010));
        const float burst =
            std::fmod (seconds, 0.47) < 0.035 ? 1.0f : 0.58f;
        const float left = scale * attack * burst * (
            0.50f * (float) std::sin (
                juce::MathConstants<double>::twoPi * 997.0 * seconds)
            + 0.31f * (float) std::sin (
                juce::MathConstants<double>::twoPi * 221.0 * seconds + 0.31)
            + 0.24f * (float) std::sin (
                juce::MathConstants<double>::twoPi * 3197.0 * seconds + 1.17));
        const float right = proportionalStereo
                          ? left * 0.5f
                          : scale * attack * burst * (
                                0.46f * (float) std::sin (
                                    juce::MathConstants<double>::twoPi
                                        * 997.0 * seconds + 0.11)
                                + 0.34f * (float) std::sin (
                                    juce::MathConstants<double>::twoPi
                                        * 277.0 * seconds + 0.73)
                                + 0.22f * (float) std::sin (
                                    juce::MathConstants<double>::twoPi
                                        * 4211.0 * seconds + 1.91));
        result.inputLeft[(size_t) sample] = left;
        result.inputRight[(size_t) sample] = right;
    }

    StereoLookaheadLimiter limiter;
    limiter.prepare (sampleRate);
    result.latency = limiter.getLatencySamples();
    juce::AudioBuffer<float> block (2, blockSize);

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int valid = juce::jmin (blockSize, totalSamples - start);
        block.clear();

        for (int sample = 0; sample < valid; ++sample)
        {
            block.setSample (
                0, sample, result.inputLeft[(size_t) (start + sample)]);
            block.setSample (
                1, sample, result.inputRight[(size_t) (start + sample)]);
        }

        limiter.process (block);

        for (int sample = 0; sample < valid; ++sample)
        {
            const float left = block.getSample (0, sample);
            const float right = block.getSample (1, sample);
            result.outputLeft[(size_t) (start + sample)] = left;
            result.outputRight[(size_t) (start + sample)] = right;
            result.samplePeak = juce::jmax (
                result.samplePeak, std::abs (left), std::abs (right));
            result.finite = result.finite
                         && std::isfinite (left)
                         && std::isfinite (right);
        }
    }

    result.finalReductionDb = limiter.getCurrentGainReductionDb();
    result.emergencyCorrections =
        limiter.getEmergencyCorrectionCount();
    return result;
}

bool testUnityAndLatency()
{
    constexpr double sampleRate = 48000.0;
    const auto rendered = render (
        sampleRate, 127, 1.0, 0.2, 0.20f, true);
    float maximumError = 0.0f;

    for (size_t source = 0;
         source + (size_t) rendered.latency < rendered.outputLeft.size();
         ++source)
    {
        maximumError = juce::jmax (
            maximumError,
            std::abs (rendered.outputLeft[
                source + (size_t) rendered.latency]
                - rendered.inputLeft[source]),
            std::abs (rendered.outputRight[
                source + (size_t) rendered.latency]
                - rendered.inputRight[source]));
    }

    const bool passed =
        rendered.finite
        && rendered.latency == 144
        && maximumError <= 1.0e-7f
        && rendered.emergencyCorrections == 0;
    std::cout << "Limiter unity/latency: latency="
              << rendered.latency << ", max error="
              << maximumError << ", emergency="
              << rendered.emergencyCorrections
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testMaximumReturnStress()
{
    struct Configuration
    {
        double sampleRate;
        int blockSize;
    };

    constexpr Configuration configurations[]
    {
        { 44100.0,  64 },
        { 48000.0, 127 },
        { 96000.0, 511 }
    };

    // Margem adversarial maior que a combinacao maxima atual:
    // dry + wet denso, Return CC127 e Master CC127 (+10 dB).
    constexpr float worstCaseScale = 4.952f;
    const double ceiling =
        juce::Decibels::decibelsToGain (
            StereoLookaheadLimiter::outputCeilingDb);
    bool passed = true;

    for (const auto& configuration : configurations)
    {
        const auto rendered = render (
            configuration.sampleRate, configuration.blockSize,
            2.5, 1.5, worstCaseScale, false);
        const double truePeak = juce::jmax (
            reconstructedPeak (rendered.outputLeft),
            reconstructedPeak (rendered.outputRight));
        const bool currentPassed =
            rendered.finite
            && rendered.samplePeak <= ceiling + 1.0e-5
            && truePeak <= ceiling + 0.006
            && rendered.finalReductionDb > -0.05f
            && rendered.emergencyCorrections == 0;
        passed = passed && currentPassed;

        std::cout << "Limiter stress @ "
                  << configuration.sampleRate << "/"
                  << configuration.blockSize
                  << ": sample=" << rendered.samplePeak
                  << ", reconstructed=" << truePeak
                  << ", finalGR=" << rendered.finalReductionDb
                  << " dB, emergency="
                  << rendered.emergencyCorrections
                  << " -> " << (currentPassed ? "PASS" : "FAIL")
                  << '\n';
    }

    return passed;
}

bool testAdversarialPeriodicTruePeak()
{
    const std::vector<std::vector<float>> patterns
    {
        {
             0.9840106f, -0.9794925f,  0.7688768f, -0.3116214f,
            -0.7780421f,  0.9295175f, -0.8957871f
        },
        {
            -1.0f, -1.0f,  0.0f, -1.0f,  0.0f, -1.0f,  0.0f,
            -1.0f,  1.0f, -1.0f,  1.0f,  0.0f,  1.0f,  0.0f,
             0.0f,  0.0f,  0.0f, -1.0f,  1.0f,  0.0f,  1.0f,
            -1.0f,  0.0f,  1.0f, -1.0f,  1.0f, -1.0f,  1.0f,
             0.0f,  0.0f,  0.0f,  0.0f, -1.0f,  0.0f, -1.0f,
             0.0f, -1.0f,  1.0f,  1.0f,  0.0f,  0.0f,  1.0f,
             0.0f,  0.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
             1.0f, -1.0f, -1.0f, -1.0f,  1.0f, -1.0f,  0.0f,
             0.0f,  1.0f,  0.0f,  1.0f, -1.0f,  1.0f, -1.0f
        },
        {
             0.447949469f, -0.346325964f,  0.277499497f,
            -0.217419982f,  0.161648959f, -0.108337738f,
             0.056431901f, -0.005193104f, -0.045991611f,
             0.097726725f, -0.150721282f,  0.205961987f,
            -0.265131652f,  0.332131892f, -0.426956505f,
             0.487709492f, -0.530834198f,  0.568599880f,
            -0.603180468f,  0.635572135f, -0.666364670f,
             0.695962489f, -0.724676430f,  0.752767801f,
            -0.780476749f,  0.808045864f, -0.835749745f,
             0.863936543f, -0.893111944f,  0.924160659f,
            -0.959277511f,  1.0f, -1.0f,  1.0f, -1.0f,
             1.0f, -1.0f,  1.0f, -1.0f,  1.0f, -1.0f,
             1.0f, -1.0f,  1.0f, -1.0f,  1.0f, -1.0f,
             1.0f, -1.0f,  1.0f, -1.0f,  1.0f, -1.0f,
             1.0f, -1.0f,  0.968635738f, -0.931356013f,
             0.899585426f, -0.870056212f,  0.841684580f,
            -0.813897192f,  0.786316693f, -0.758655012f,
             0.730664611f, -0.702107072f,  0.672728658f,
            -0.642234027f,  0.610249519f, -0.576249540f,
             0.539395809f, -0.498082370f
        }
    };
    constexpr int totalSamples = 48000;
    constexpr int blockSize = 127;
    const double ceiling = juce::Decibels::decibelsToGain (
        StereoLookaheadLimiter::outputCeilingDb);
    bool allPassed = true;

    for (const auto& pattern : patterns)
    {
        const int patternSize = (int) pattern.size();
        StereoLookaheadLimiter limiter;
        limiter.prepare (48000.0);
        juce::AudioBuffer<float> block (2, blockSize);
        std::vector<float> output;
        output.reserve ((size_t) totalSamples);
        bool finite = true;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int valid =
                juce::jmin (blockSize, totalSamples - start);
            block.clear();

            for (int sample = 0; sample < valid; ++sample)
            {
                const float value =
                    pattern[(size_t) ((start + sample) % patternSize)];
                block.setSample (0, sample, value);
                block.setSample (1, sample, value * 0.71f);
            }

            limiter.process (block);

            for (int sample = 0; sample < valid; ++sample)
            {
                const float value = block.getSample (0, sample);
                finite = finite && std::isfinite (value);
                output.push_back (value);
            }
        }

        std::vector<float> settledPeriod;
        settledPeriod.reserve ((size_t) patternSize);

        for (int sample = totalSamples - patternSize;
             sample < totalSamples;
             ++sample)
        {
            settledPeriod.push_back (output[(size_t) sample]);
        }

        const double inputTruePeak =
            periodicBandlimitedPeak (pattern);
        const double outputTruePeak =
            periodicBandlimitedPeak (settledPeriod);
        const bool passed =
            finite
            && inputTruePeak > 1.10
            && outputTruePeak <= ceiling + 0.001
            && limiter.getEmergencyCorrectionCount() == 0;
        allPassed = allPassed && passed;

        std::cout << "Limiter adversarial period-" << patternSize
                  << ": inputTP=" << inputTruePeak
                  << ", outputTP=" << outputTruePeak
                  << ", emergency="
                  << limiter.getEmergencyCorrectionCount()
                  << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    }

    return allPassed;
}

bool testStartupTruePeak()
{
    const std::vector<float> input { 1.0f, -1.0f, -1.0f, 1.0f };
    StereoLookaheadLimiter limiter;
    limiter.prepare (48000.0);
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    for (int sample = 0; sample < (int) input.size(); ++sample)
    {
        buffer.setSample (0, sample, input[(size_t) sample]);
        buffer.setSample (1, sample, input[(size_t) sample] * 0.5f);
    }

    limiter.process (buffer);
    std::vector<float> output ((size_t) buffer.getNumSamples());
    bool finite = true;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        output[(size_t) sample] = buffer.getSample (0, sample);
        finite = finite && std::isfinite (output[(size_t) sample]);
    }

    const double inputTruePeak = finiteBandlimitedPeak (input);
    const double outputTruePeak = finiteBandlimitedPeak (output);
    const double ceiling = juce::Decibels::decibelsToGain (
        StereoLookaheadLimiter::outputCeilingDb);
    const bool passed =
        finite
        && inputTruePeak > 1.65
        && outputTruePeak <= ceiling + 0.001
        && limiter.getEmergencyCorrectionCount() == 0;

    std::cout << "Limiter startup burst: inputTP=" << inputTruePeak
              << ", outputTP=" << outputTruePeak
              << ", emergency="
              << limiter.getEmergencyCorrectionCount()
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testStereoLink()
{
    const auto rendered = render (
        48000.0, 256, 1.5, 0.4, 4.0f, true);
    double maximumRatioError = 0.0;
    size_t compared = 0;

    for (size_t sample = (size_t) rendered.latency;
         sample < rendered.outputLeft.size();
         ++sample)
    {
        const float left = rendered.outputLeft[sample];
        const float right = rendered.outputRight[sample];

        if (std::abs (left) > 0.01f)
        {
            maximumRatioError = juce::jmax (
                maximumRatioError,
                std::abs ((double) right / (double) left - 0.5));
            ++compared;
        }
    }

    const bool passed =
        rendered.finite
        && compared > 1000
        && maximumRatioError <= 1.0e-6;
    std::cout << "Limiter stereo link: ratio error="
              << maximumRatioError << ", frames=" << compared
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testNonFiniteInputIsContained()
{
    StereoLookaheadLimiter limiter;
    limiter.prepare (48000.0);
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        buffer.setSample (0, sample, 0.2f);
        buffer.setSample (1, sample, -0.2f);
    }

    buffer.setSample (
        1, 17, std::numeric_limits<float>::quiet_NaN());
    buffer.setSample (
        0, 29, std::numeric_limits<float>::infinity());
    limiter.process (buffer);
    bool finite = true;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            finite = finite
                  && std::isfinite (buffer.getSample (channel, sample));

    const bool passed =
        finite && limiter.getEmergencyCorrectionCount() == 2;
    std::cout << "Limiter non-finite containment: emergency="
              << limiter.getEmergencyCorrectionCount()
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testLifecycleAndBlockInvariance()
{
    const auto smallBlocks = render (
        48000.0, 31, 0.35, 0.15, 3.0f, false);
    const auto largeBlocks = render (
        48000.0, 509, 0.35, 0.15, 3.0f, false);
    float blockError = 0.0f;

    for (size_t sample = 0;
         sample < smallBlocks.outputLeft.size();
         ++sample)
    {
        blockError = juce::jmax (
            blockError,
            std::abs (smallBlocks.outputLeft[sample]
                      - largeBlocks.outputLeft[sample]),
            std::abs (smallBlocks.outputRight[sample]
                      - largeBlocks.outputRight[sample]));
    }

    StereoLookaheadLimiter used;
    StereoLookaheadLimiter fresh;
    used.prepare (48000.0);
    fresh.prepare (48000.0);
    juce::AudioBuffer<float> warmup (2, 1024);
    warmup.clear();

    for (int sample = 0; sample < warmup.getNumSamples(); ++sample)
    {
        warmup.setSample (0, sample, sample % 2 == 0 ? 3.0f : -3.0f);
        warmup.setSample (1, sample, sample % 3 == 0 ? -2.0f : 2.0f);
    }

    used.process (warmup);
    used.reset();
    juce::AudioBuffer<float> afterReset (2, 777);
    juce::AudioBuffer<float> reference (2, 777);

    for (int sample = 0; sample < afterReset.getNumSamples(); ++sample)
    {
        const float left =
            0.3f * std::sin (0.037f * (float) sample);
        const float right =
            0.2f * std::cos (0.019f * (float) sample);
        afterReset.setSample (0, sample, left);
        afterReset.setSample (1, sample, right);
        reference.setSample (0, sample, left);
        reference.setSample (1, sample, right);
    }

    used.process (afterReset);
    fresh.process (reference);
    float resetError = 0.0f;

    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < afterReset.getNumSamples(); ++sample)
            resetError = juce::jmax (
                resetError,
                std::abs (afterReset.getSample (channel, sample)
                          - reference.getSample (channel, sample)));

    used.prepare (96000.0);
    juce::AudioBuffer<float> mono (1, 1021);

    for (int sample = 0; sample < mono.getNumSamples(); ++sample)
        mono.setSample (
            0, sample,
            2.5f * std::sin (0.41f * (float) sample));

    used.process (mono);
    bool monoFinite = true;
    float monoPeak = 0.0f;

    for (int sample = 0; sample < mono.getNumSamples(); ++sample)
    {
        monoFinite = monoFinite
                  && std::isfinite (mono.getSample (0, sample));
        monoPeak = juce::jmax (
            monoPeak, std::abs (mono.getSample (0, sample)));
    }

    const double ceiling = juce::Decibels::decibelsToGain (
        StereoLookaheadLimiter::outputCeilingDb);
    const bool passed =
        smallBlocks.finite && largeBlocks.finite
        && smallBlocks.emergencyCorrections == 0
        && largeBlocks.emergencyCorrections == 0
        && blockError <= 1.0e-7f
        && resetError <= 1.0e-7f
        && used.getLatencySamples() == 288
        && monoFinite
        && monoPeak <= ceiling + 1.0e-5
        && used.getEmergencyCorrectionCount() == 0;

    std::cout << "Limiter lifecycle: blockError=" << blockError
              << ", resetError=" << resetError
              << ", latency96k=" << used.getLatencySamples()
              << ", monoPeak=" << monoPeak
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testInternalDetectorChunking()
{
    constexpr int totalSamples = 509;
    constexpr int detectorCapacity = 31;
    StereoLookaheadLimiter chunked;
    StereoLookaheadLimiter reference;
    chunked.prepare (48000.0, detectorCapacity);
    reference.prepare (48000.0, detectorCapacity);

    std::vector<float> inputLeft ((size_t) totalSamples);
    std::vector<float> inputRight ((size_t) totalSamples);

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        inputLeft[(size_t) sample] =
            2.4f * std::sin (0.071f * (float) sample)
            + 0.7f * std::cos (0.411f * (float) sample);
        inputRight[(size_t) sample] =
            -1.8f * std::cos (0.053f * (float) sample + 0.2f)
            + 0.5f * std::sin (0.377f * (float) sample);
    }

    juce::AudioBuffer<float> oversized (2, totalSamples);

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        oversized.setSample (0, sample, inputLeft[(size_t) sample]);
        oversized.setSample (1, sample, inputRight[(size_t) sample]);
    }

    chunked.process (oversized);
    std::vector<float> referenceLeft ((size_t) totalSamples);
    std::vector<float> referenceRight ((size_t) totalSamples);

    for (int start = 0; start < totalSamples; start += detectorCapacity)
    {
        const int valid =
            juce::jmin (detectorCapacity, totalSamples - start);
        juce::AudioBuffer<float> block (2, valid);

        for (int sample = 0; sample < valid; ++sample)
        {
            block.setSample (
                0, sample, inputLeft[(size_t) (start + sample)]);
            block.setSample (
                1, sample, inputRight[(size_t) (start + sample)]);
        }

        reference.process (block);

        for (int sample = 0; sample < valid; ++sample)
        {
            referenceLeft[(size_t) (start + sample)] =
                block.getSample (0, sample);
            referenceRight[(size_t) (start + sample)] =
                block.getSample (1, sample);
        }
    }

    float maximumError = 0.0f;
    bool finite = true;

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        const float left = oversized.getSample (0, sample);
        const float right = oversized.getSample (1, sample);
        finite = finite
              && std::isfinite (left)
              && std::isfinite (right)
              && std::isfinite (referenceLeft[(size_t) sample])
              && std::isfinite (referenceRight[(size_t) sample]);
        maximumError = juce::jmax (
            maximumError,
            std::abs (left - referenceLeft[(size_t) sample]),
            std::abs (right - referenceRight[(size_t) sample]));
    }

    const bool passed =
        finite
        && maximumError <= 1.0e-7f
        && chunked.getLatencySamples() == 144
        && reference.getLatencySamples() == 144
        && chunked.getEmergencyCorrectionCount() == 0
        && reference.getEmergencyCorrectionCount() == 0;

    std::cout << "Limiter internal chunks: capacity="
              << detectorCapacity << ", call=" << totalSamples
              << ", max error=" << maximumError
              << ", emergency="
              << chunked.getEmergencyCorrectionCount() << "/"
              << reference.getEmergencyCorrectionCount()
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}
} // namespace

int main()
{
    juce::ScopedNoDenormals noDenormals;
    std::cout << std::fixed << std::setprecision (6);
    int failures = 0;

    if (! testUnityAndLatency())
        ++failures;
    if (! testMaximumReturnStress())
        ++failures;
    if (! testAdversarialPeriodicTruePeak())
        ++failures;
    if (! testStartupTruePeak())
        ++failures;
    if (! testStereoLink())
        ++failures;
    if (! testNonFiniteInputIsContained())
        ++failures;
    if (! testLifecycleAndBlockInvariance())
        ++failures;
    if (! testInternalDetectorChunking())
        ++failures;

    if (failures == 0)
    {
        std::cout << "MASTER_LIMITER_TESTS_OK\n";
        return 0;
    }

    std::cerr << "MASTER_LIMITER_TESTS_FAILED=" << failures << '\n';
    return 1;
}
