#include "WorshipReverb.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace
{
constexpr int fftOrder = 18;
constexpr int fftSize = 1 << fftOrder;

struct RenderedAudio
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> mid;
    std::vector<float> side;
    float peak = 0.0f;
    bool finite = true;
};

enum SignalPath
{
    leftPath = 0,
    rightPath,
    midPath,
    sidePath,
    numSignalPaths
};

constexpr std::array<const char*, numSignalPaths> signalPathNames
{
    "L", "R", "Mid", "Side"
};

const std::vector<float>& signalFor (
    const RenderedAudio& audio, SignalPath path) noexcept
{
    switch (path)
    {
        case leftPath:  return audio.left;
        case rightPath: return audio.right;
        case sidePath:  return audio.side;
        case midPath:
        default:        return audio.mid;
    }
}

struct SpectrumMeasurement
{
    double levelDb = -200.0;
    double centroidCents = 0.0;
    double peakCents = 0.0;
    double widthCents = 0.0;
    double meanAbsoluteCents = 0.0;
    bool valid = false;
};

struct PitchStabilitySummary
{
    double centroidP95 = 999.0;
    double peakP95 = 999.0;
    double centroidSpan90 = 999.0;
    double excessWidthP95 = 999.0;
    double excessMeanAbsoluteP95 = 999.0;
    double peakLevelDb = -200.0;
    size_t frames = 0;
    bool valid = false;
};

double percentile (std::vector<double> values, double amount)
{
    if (values.empty())
        return std::numeric_limits<double>::quiet_NaN();

    std::sort (values.begin(), values.end());
    const double position = juce::jlimit (0.0, 1.0, amount)
                          * (double) (values.size() - 1);
    const size_t lower = (size_t) std::floor (position);
    const size_t upper = juce::jmin (values.size() - 1, lower + 1);
    const double fraction = position - (double) lower;
    return values[lower] + fraction * (values[upper] - values[lower]);
}

double rmsDb (const std::vector<float>& samples, size_t start, size_t length)
{
    if (start >= samples.size() || length == 0)
        return -200.0;

    length = juce::jmin (length, samples.size() - start);
    long double sum = 0.0;

    for (size_t i = 0; i < length; ++i)
    {
        const long double value = samples[start + i];
        sum += value * value;
    }

    const double rms = std::sqrt ((double) (sum / (long double) length));
    return juce::Decibels::gainToDecibels (rms, -200.0);
}

double estimateT60 (const std::vector<float>& samples,
                    size_t tailStart,
                    double sampleRate)
{
    if (tailStart >= samples.size())
        return std::numeric_limits<double>::quiet_NaN();

    std::vector<long double> reverseEnergy (
        samples.size() - tailStart + 1, 0.0L);

    for (size_t i = samples.size(); i-- > tailStart;)
    {
        const long double sample = samples[i];
        reverseEnergy[i - tailStart] =
            reverseEnergy[i - tailStart + 1] + sample * sample;
    }

    const long double reference = reverseEnergy.front();

    if (reference <= 1.0e-24L)
        return std::numeric_limits<double>::quiet_NaN();

    long double sumTime = 0.0;
    long double sumDb = 0.0;
    long double sumTimeSquared = 0.0;
    long double sumTimeDb = 0.0;
    size_t count = 0;
    const size_t stride = juce::jmax (
        (size_t) 1, (size_t) std::llround (sampleRate / 200.0));

    for (size_t offset = 0; offset + 1 < reverseEnergy.size(); offset += stride)
    {
        const double levelDb = 10.0 * std::log10 (
            (double) (reverseEnergy[offset] / reference));

        if (levelDb <= -5.0 && levelDb >= -35.0)
        {
            const long double seconds = (long double) offset / sampleRate;
            sumTime += seconds;
            sumDb += levelDb;
            sumTimeSquared += seconds * seconds;
            sumTimeDb += seconds * levelDb;
            ++count;
        }
    }

    const long double denominator =
        (long double) count * sumTimeSquared - sumTime * sumTime;

    if (count < 20 || std::abs ((double) denominator) < 1.0e-18)
        return std::numeric_limits<double>::quiet_NaN();

    const double slope = (double) (
        ((long double) count * sumTimeDb - sumTime * sumDb) / denominator);
    return slope < -0.001
         ? -60.0 / slope
         : std::numeric_limits<double>::quiet_NaN();
}

class NarrowbandAnalyser
{
public:
    SpectrumMeasurement analyse (const std::vector<float>& samples,
                                 size_t start,
                                 double sampleRate,
                                 double centreFrequency,
                                 double halfWidthCents,
                                 double windowSeconds = 1.0)
    {
        const int windowSamples = juce::jmin (
            fftSize, (int) std::llround (sampleRate * windowSeconds));

        if (start + (size_t) windowSamples > samples.size())
            return {};

        std::fill (spectrum.begin(), spectrum.end(), 0.0f);
        long double squareSum = 0.0;

        for (int i = 0; i < windowSamples; ++i)
        {
            const float value = samples[start + (size_t) i];
            const float phase = (float) i / (float) (windowSamples - 1);
            const float hann = 0.5f - 0.5f * std::cos (
                juce::MathConstants<float>::twoPi * phase);
            spectrum[(size_t) i] = value * hann;
            squareSum += (long double) value * (long double) value;
        }

        fft.performRealOnlyForwardTransform (spectrum.data(), true);

        const double lowFrequency =
            centreFrequency * std::pow (2.0, -halfWidthCents / 1200.0);
        const double highFrequency =
            centreFrequency * std::pow (2.0, halfWidthCents / 1200.0);
        const int lowBin = juce::jmax (
            1, (int) std::floor (lowFrequency * fftSize / sampleRate));
        const int highBin = juce::jmin (
            fftSize / 2 - 2,
            (int) std::ceil (highFrequency * fftSize / sampleRate));

        long double powerSum = 0.0;
        long double centsSum = 0.0;
        long double centsSquaredSum = 0.0;
        long double absoluteCentsSum = 0.0;
        int peakBin = lowBin;
        double peakPower = 0.0;

        for (int bin = lowBin; bin <= highBin; ++bin)
        {
            const double real = spectrum[(size_t) (2 * bin)];
            const double imaginary = spectrum[(size_t) (2 * bin + 1)];
            const double power = real * real + imaginary * imaginary;
            const double frequency = (double) bin * sampleRate / fftSize;
            const double cents = 1200.0 * std::log2 (
                frequency / centreFrequency);
            powerSum += power;
            centsSum += (long double) power * cents;
            centsSquaredSum += (long double) power * cents * cents;
            absoluteCentsSum += (long double) power * std::abs (cents);

            if (power > peakPower)
            {
                peakPower = power;
                peakBin = bin;
            }
        }

        if (powerSum <= 1.0e-24L)
            return {};

        const auto logPowerAt = [this] (int bin)
        {
            const double real = spectrum[(size_t) (2 * bin)];
            const double imaginary = spectrum[(size_t) (2 * bin + 1)];
            return std::log (juce::jmax (
                1.0e-30, real * real + imaginary * imaginary));
        };

        const double left = logPowerAt (peakBin - 1);
        const double middle = logPowerAt (peakBin);
        const double right = logPowerAt (peakBin + 1);
        const double denominator = left - 2.0 * middle + right;
        const double offset = std::abs (denominator) > 1.0e-12
                            ? juce::jlimit (-0.5, 0.5,
                                           0.5 * (left - right) / denominator)
                            : 0.0;
        const double peakFrequency =
            ((double) peakBin + offset) * sampleRate / fftSize;

        SpectrumMeasurement result;
        result.levelDb = juce::Decibels::gainToDecibels (
            std::sqrt ((double) (squareSum / (long double) windowSamples)),
            -200.0);
        result.centroidCents = (double) (centsSum / powerSum);
        const double meanSquareCents =
            (double) (centsSquaredSum / powerSum);
        result.widthCents = std::sqrt (juce::jmax (
            0.0,
            meanSquareCents
                - result.centroidCents * result.centroidCents));
        result.meanAbsoluteCents =
            (double) (absoluteCentsSum / powerSum);
        result.peakCents = 1200.0 * std::log2 (
            peakFrequency / centreFrequency);
        result.valid = std::isfinite (result.centroidCents)
                    && std::isfinite (result.peakCents)
                    && std::isfinite (result.widthCents)
                    && std::isfinite (result.meanAbsoluteCents);
        return result;
    }

private:
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> spectrum =
        std::vector<float> ((size_t) fftSize * 2, 0.0f);
};

SpectrumMeasurement stableToneReference (NarrowbandAnalyser& analyser,
                                         double sampleRate,
                                         double frequency,
                                         double halfWidthCents,
                                         double windowSeconds)
{
    const size_t sampleCount = (size_t) juce::jmin (
        fftSize, (int) std::llround (sampleRate * windowSeconds));
    std::vector<float> tone (sampleCount, 0.0f);
    const double phaseIncrement =
        juce::MathConstants<double>::twoPi * frequency / sampleRate;
    double phase = 0.371;

    for (auto& sample : tone)
    {
        sample = 0.25f * (float) std::sin (phase);
        phase += phaseIncrement;

        if (phase > juce::MathConstants<double>::pi)
            phase -= juce::MathConstants<double>::twoPi;
    }

    return analyser.analyse (
        tone, 0, sampleRate, frequency, halfWidthCents, windowSeconds);
}

PitchStabilitySummary measurePitchStability (
    NarrowbandAnalyser& analyser,
    const std::vector<float>& samples,
    double sampleRate,
    double frequency,
    double beginSeconds,
    double endSeconds,
    double hopSeconds,
    double halfWidthCents,
    double windowSeconds,
    double absoluteLevelFloorDb,
    double relativeLevelFloorDb)
{
    std::vector<SpectrumMeasurement> candidates;
    const size_t hop = juce::jmax (
        (size_t) 1, (size_t) std::llround (hopSeconds * sampleRate));
    const size_t begin =
        (size_t) std::llround (beginSeconds * sampleRate);
    const size_t end =
        (size_t) std::llround (endSeconds * sampleRate);

    for (size_t start = begin; start <= end; start += hop)
    {
        const auto frame = analyser.analyse (
            samples, start, sampleRate, frequency,
            halfWidthCents, windowSeconds);

        if (frame.valid)
            candidates.push_back (frame);
    }

    PitchStabilitySummary result;

    for (const auto& frame : candidates)
        result.peakLevelDb = juce::jmax (result.peakLevelDb, frame.levelDb);

    const auto reference = stableToneReference (
        analyser, sampleRate, frequency, halfWidthCents, windowSeconds);

    if (! reference.valid)
        return result;

    std::vector<double> absoluteCentroids;
    std::vector<double> absolutePeaks;
    std::vector<double> signedCentroids;
    std::vector<double> excessWidths;
    std::vector<double> excessMeanAbsolutes;

    for (const auto& frame : candidates)
    {
        if (frame.levelDb <= absoluteLevelFloorDb
            || frame.levelDb - result.peakLevelDb <= relativeLevelFloorDb)
        {
            continue;
        }

        absoluteCentroids.push_back (std::abs (frame.centroidCents));
        absolutePeaks.push_back (std::abs (frame.peakCents));
        signedCentroids.push_back (frame.centroidCents);
        excessWidths.push_back (std::sqrt (juce::jmax (
            0.0,
            frame.widthCents * frame.widthCents
                - reference.widthCents * reference.widthCents)));
        excessMeanAbsolutes.push_back (juce::jmax (
            0.0,
            frame.meanAbsoluteCents
                - reference.meanAbsoluteCents));
    }

    result.frames = absoluteCentroids.size();

    if (result.frames == 0)
        return result;

    result.centroidP95 = percentile (absoluteCentroids, 0.95);
    result.peakP95 = percentile (absolutePeaks, 0.95);
    result.centroidSpan90 =
        percentile (signedCentroids, 0.95)
            - percentile (signedCentroids, 0.05);
    result.excessWidthP95 = percentile (excessWidths, 0.95);
    result.excessMeanAbsoluteP95 =
        percentile (excessMeanAbsolutes, 0.95);
    result.valid = std::isfinite (result.centroidP95)
                && std::isfinite (result.peakP95)
                && std::isfinite (result.centroidSpan90)
                && std::isfinite (result.excessWidthP95)
                && std::isfinite (result.excessMeanAbsoluteP95);
    return result;
}

bool stabilityWithin (const PitchStabilitySummary& summary,
                      size_t minimumFrames,
                      double centroidP95Limit,
                      double peakP95Limit,
                      double centroidSpan90Limit,
                      double excessWidthP95Limit,
                      double excessMeanAbsoluteP95Limit) noexcept
{
    return summary.valid
        && summary.frames >= minimumFrames
        && summary.centroidP95 <= centroidP95Limit
        && summary.peakP95 <= peakP95Limit
        && summary.centroidSpan90 <= centroidSpan90Limit
        && summary.excessWidthP95 <= excessWidthP95Limit
        && summary.excessMeanAbsoluteP95 <= excessMeanAbsoluteP95Limit;
}

void printStability (const PitchStabilitySummary& summary)
{
    std::cout << "centroid95=" << summary.centroidP95
              << "c peak95=" << summary.peakP95
              << "c span90=" << summary.centroidSpan90
              << "c width+=" << summary.excessWidthP95
              << "c abs+=" << summary.excessMeanAbsoluteP95
              << "c frames=" << summary.frames;
}

RenderedAudio renderTone (int preset,
                          double sampleRate,
                          int blockSize,
                          double frequency,
                          double inputSeconds,
                          double totalSeconds,
                          bool fadeRelease)
{
    WorshipReverb reverb {};
    reverb.setPreset (preset);
    reverb.prepare (sampleRate, blockSize);

    const int totalSamples = (int) std::ceil (totalSeconds * sampleRate);
    const int inputSamples = (int) std::ceil (inputSeconds * sampleRate);
    const int fadeSamples = juce::jmax (1, (int) std::ceil (0.020 * sampleRate));
    const float amplitude = juce::Decibels::decibelsToGain (-18.0f);
    const double phaseIncrement =
        juce::MathConstants<double>::twoPi * frequency / sampleRate;
    double phase = 0.0;

    RenderedAudio rendered;
    rendered.left.resize ((size_t) totalSamples);
    rendered.right.resize ((size_t) totalSamples);
    rendered.mid.resize ((size_t) totalSamples);
    rendered.side.resize ((size_t) totalSamples);
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int valid = juce::jmin (blockSize, totalSamples - start);
        buffer.clear();

        for (int i = 0; i < valid; ++i)
        {
            const int position = start + i;
            float envelope = 0.0f;

            if (position < inputSamples)
            {
                envelope = juce::jmin (1.0f, (float) position / fadeSamples);

                if (fadeRelease)
                    envelope *= juce::jmin (
                        1.0f, (float) (inputSamples - position) / fadeSamples);
            }

            const float sample = amplitude * envelope * (float) std::sin (phase);
            phase += phaseIncrement;

            if (phase > juce::MathConstants<double>::pi)
                phase -= juce::MathConstants<double>::twoPi;

            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        reverb.process (buffer);

        for (int i = 0; i < valid; ++i)
        {
            const float left = buffer.getSample (0, i);
            const float right = buffer.getSample (1, i);
            const float mid = 0.5f * (left + right);
            const float side = 0.5f * (left - right);
            const size_t position = (size_t) (start + i);
            rendered.left[position] = left;
            rendered.right[position] = right;
            rendered.mid[position] = mid;
            rendered.side[position] = side;
            rendered.peak = juce::jmax (
                rendered.peak, std::abs (left), std::abs (right));
            rendered.finite = rendered.finite
                           && std::isfinite (left)
                           && std::isfinite (right)
                           && std::isfinite (mid)
                           && std::isfinite (side);
        }
    }

    return rendered;
}

RenderedAudio renderLegacyMovingDelayControl (double sampleRate,
                                               double frequency,
                                               double totalSeconds)
{
    const size_t totalSamples =
        (size_t) std::ceil (totalSeconds * sampleRate);
    const float amplitude = juce::Decibels::decibelsToGain (-18.0f);
    constexpr double modulationDepthSeconds = 0.005;
    constexpr double modulationRateHz = 0.13;
    RenderedAudio rendered;
    rendered.left.resize (totalSamples);
    rendered.right.resize (totalSamples);
    rendered.mid.resize (totalSamples);
    rendered.side.resize (totalSamples);

    for (size_t i = 0; i < totalSamples; ++i)
    {
        const double seconds = (double) i / sampleRate;
        const double movingDelay = modulationDepthSeconds * std::sin (
            juce::MathConstants<double>::twoPi
                * modulationRateHz * seconds);
        const double phaseL =
            juce::MathConstants<double>::twoPi * frequency
                * (seconds - movingDelay);
        const double phaseR =
            juce::MathConstants<double>::twoPi * frequency
                * (seconds + movingDelay);
        const float left = amplitude * (float) std::sin (phaseL);
        const float right = amplitude * (float) std::sin (phaseR);
        const float mid = 0.5f * (left + right);
        const float side = 0.5f * (left - right);
        rendered.left[i] = left;
        rendered.right[i] = right;
        rendered.mid[i] = mid;
        rendered.side[i] = side;
        rendered.peak = juce::jmax (
            rendered.peak, std::abs (left), std::abs (right));
        rendered.finite = rendered.finite
                       && std::isfinite (left)
                       && std::isfinite (right)
                       && std::isfinite (mid)
                       && std::isfinite (side);
    }

    return rendered;
}

bool testSteadyPitch (int onlyConfiguration = -1)
{
    struct Configuration
    {
        double sampleRate;
        int blockSize;
        double frequency;
    };

    constexpr Configuration configurations[]
    {
        { 44100.0,  64, 220.0 },
        { 48000.0, 127, 440.0 },
        { 48000.0, 256, 220.0 },
        { 96000.0, 511, 440.0 }
    };

    bool passed = true;

    for (int configurationIndex = 0;
         configurationIndex < (int) std::size (configurations);
         ++configurationIndex)
    {
        if (onlyConfiguration >= 0
            && configurationIndex != onlyConfiguration)
            continue;

        const auto& configuration =
            configurations[(size_t) configurationIndex];
        const auto rendered = renderTone (
            1, configuration.sampleRate, configuration.blockSize,
            configuration.frequency, 12.0, 12.0, false);
        NarrowbandAnalyser analyser;
        bool currentPassed = rendered.finite && rendered.peak < 1.0f;
        std::cout << "Cloud steady " << configuration.frequency << " Hz @ "
                  << configuration.sampleRate << "/" << configuration.blockSize
                  << '\n';

        for (int pathIndex = 0; pathIndex < numSignalPaths; ++pathIndex)
        {
            const auto path = (SignalPath) pathIndex;
            const auto summary = measurePitchStability (
                analyser, signalFor (rendered, path),
                configuration.sampleRate, configuration.frequency,
                5.0, 10.75, 0.25, 50.0, 1.0, -85.0, -45.0);
            const bool pathPassed = stabilityWithin (
                summary, 10,
                1.0, 1.5, 1.5, 3.0, 1.5);
            currentPassed = currentPassed && pathPassed;
            std::cout << "  " << signalPathNames[(size_t) pathIndex] << ": ";
            printStability (summary);
            std::cout << " -> " << (pathPassed ? "PASS" : "FAIL") << '\n';
        }

        passed = passed && currentPassed;
        std::cout << "  result -> "
                  << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    return passed;
}

bool runSteadyRenderWarmupOnly()
{
    struct Configuration
    {
        double sampleRate;
        int blockSize;
        double frequency;
    };

    constexpr Configuration configurations[]
    {
        { 44100.0,  64, 220.0 },
        { 48000.0, 127, 440.0 },
        { 48000.0, 256, 220.0 },
        { 96000.0, 511, 440.0 }
    };

    bool passed = true;

    for (const auto& configuration : configurations)
    {
        const auto rendered = renderTone (
            1, configuration.sampleRate, configuration.blockSize,
            configuration.frequency, 12.0, 12.0, false);
        passed = passed && rendered.finite && rendered.peak < 1.0f;
    }

    std::cout << "Steady render-only warmup -> "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testCloudTail()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 440.0;
    WorshipReverb lengthProbe;
    lengthProbe.setPreset (1);
    lengthProbe.prepare (sampleRate, 128);
    const double inputSeconds = 4.0;
    const double totalSeconds =
        inputSeconds + lengthProbe.getTailLengthSeconds() + 1.5;
    const auto rendered = renderTone (
        1, sampleRate, 128, frequency, inputSeconds, totalSeconds, true);
    NarrowbandAnalyser analyser;
    std::vector<SpectrumMeasurement> measurements;

    for (double seconds = inputSeconds + 0.25;
         seconds + 1.0 < totalSeconds;
         seconds += 0.25)
    {
        const auto frame = analyser.analyse (
            rendered.mid, (size_t) std::llround (seconds * sampleRate),
            sampleRate, frequency, 75.0);

        if (frame.valid)
            measurements.push_back (frame);
    }

    double peakTailDb = -200.0;

    for (const auto& frame : measurements)
        peakTailDb = juce::jmax (peakTailDb, frame.levelDb);

    std::vector<double> acceptedErrors;
    std::vector<double> earlyCentroids;
    std::vector<double> lateCentroids;

    for (const auto& frame : measurements)
    {
        const double relative = frame.levelDb - peakTailDb;

        if (frame.levelDb > -85.0 && relative > -50.0)
            acceptedErrors.push_back (std::abs (frame.centroidCents));

        if (relative <= -6.0 && relative >= -18.0)
            earlyCentroids.push_back (frame.centroidCents);
        else if (relative <= -24.0 && relative >= -48.0)
            lateCentroids.push_back (frame.centroidCents);
    }

    const double p95 = percentile (acceptedErrors, 0.95);
    const double earlyMedian = percentile (earlyCentroids, 0.50);
    const double lateMedian = percentile (lateCentroids, 0.50);
    const double medianShift =
        std::isfinite (earlyMedian) && std::isfinite (lateMedian)
        ? std::abs (lateMedian - earlyMedian)
        : 999.0;
    const double finalDb = rmsDb (
        rendered.mid,
        rendered.mid.size() - (size_t) std::llround (0.5 * sampleRate),
        (size_t) std::llround (0.5 * sampleRate));
    bool pitchStable = true;

    std::cout << "Cloud tail paths\n";

    for (int pathIndex = 0; pathIndex < numSignalPaths; ++pathIndex)
    {
        const auto path = (SignalPath) pathIndex;
        const auto temporal = measurePitchStability (
            analyser, signalFor (rendered, path),
            sampleRate, frequency,
            inputSeconds + 0.25, totalSeconds - 1.25,
            0.25, 75.0, 1.0, -85.0, -50.0);
        const auto spectral = measurePitchStability (
            analyser, signalFor (rendered, path),
            sampleRate, frequency,
            inputSeconds + 0.50, totalSeconds - 4.25,
            0.50, 75.0, 4.0, -85.0, -45.0);
        const bool temporalPassed = stabilityWithin (
            temporal, 8,
            4.0, 5.5, 7.0, 12.0, 5.0);
        const bool spectralPassed = stabilityWithin (
            spectral, 5,
            3.0, 5.5, 5.0, 9.5, 5.5);
        const bool pathPassed = temporalPassed && spectralPassed;
        pitchStable = pitchStable && pathPassed;

        std::cout << "  " << signalPathNames[(size_t) pathIndex]
                  << " temporal: ";
        printStability (temporal);
        std::cout << "; 4s: ";
        printStability (spectral);
        std::cout << " -> " << (pathPassed ? "PASS" : "FAIL") << '\n';
    }

    const bool passed =
        rendered.finite
        && rendered.peak < 1.0f
        && acceptedErrors.size() >= 8
        && p95 <= 4.0
        && medianShift <= 2.0
        && finalDb <= peakTailDb - 50.0
        && pitchStable;

    std::cout << "Cloud tail: centroid P95=" << p95
              << " c, early/final shift=" << medianShift
              << " c, tail drop=" << (peakTailDb - finalDb)
              << " dB -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testLegacyMovingDelayIsRejected()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 440.0;
    const auto rendered = renderLegacyMovingDelayControl (
        sampleRate, frequency, 12.0);
    NarrowbandAnalyser analyser;
    bool directPathsRejected = true;
    bool symmetricPathsRejected = true;

    std::cout << "Legacy moving-delay negative control"
              << " (5 ms @ 0.13 Hz)\n";

    for (int pathIndex = 0; pathIndex < numSignalPaths; ++pathIndex)
    {
        const auto path = (SignalPath) pathIndex;
        const auto temporal = measurePitchStability (
            analyser, signalFor (rendered, path),
            sampleRate, frequency,
            1.0, 10.75, 0.25, 75.0, 1.0, -85.0, -45.0);
        const auto spectral = measurePitchStability (
            analyser, signalFor (rendered, path),
            sampleRate, frequency,
            1.0, 7.5, 0.50, 75.0, 4.0, -85.0, -45.0);
        const bool temporalAccepted = stabilityWithin (
            temporal, 10,
            1.0, 1.5, 1.5, 3.0, 1.5);
        const bool spectralAccepted = stabilityWithin (
            spectral, 5,
            3.0, 5.5, 5.0, 9.5, 5.5);

        if (path == leftPath || path == rightPath)
            directPathsRejected =
                directPathsRejected && ! temporalAccepted;
        else
            symmetricPathsRejected =
                symmetricPathsRejected && ! spectralAccepted;

        std::cout << "  " << signalPathNames[(size_t) pathIndex]
                  << " temporal: ";
        printStability (temporal);
        std::cout << "; 4s: ";
        printStability (spectral);
        std::cout << " -> temporal "
                  << (temporalAccepted ? "ACCEPTED" : "REJECTED")
                  << ", spectral "
                  << (spectralAccepted ? "ACCEPTED" : "REJECTED")
                  << '\n';
    }

    const bool passed =
        rendered.finite
        && directPathsRejected
        && symmetricPathsRejected;
    std::cout << "  negative control -> "
              << (passed ? "REJECTED/PASS" : "MISSED/FAIL") << '\n';
    return passed;
}

bool testCloudBandDecay()
{
    struct Band
    {
        double frequency;
        double targetT60;
    };

    constexpr Band bands[]
    {
        {  125.0, 10.0 },
        { 1000.0, 12.5 },
        { 8000.0,  5.5 }
    };

    constexpr double sampleRate = 48000.0;
    constexpr double inputSeconds = 4.0;
    bool passed = true;

    for (const auto& band : bands)
    {
        const auto rendered = renderTone (
            1, sampleRate, 128, band.frequency,
            inputSeconds, inputSeconds + 17.0, true);
        const size_t tailStart = (size_t) std::llround (
            (inputSeconds + 0.15) * sampleRate);
        const double measured = estimateT60 (
            rendered.mid, tailStart, sampleRate);
        const double relativeError =
            std::abs (measured - band.targetT60) / band.targetT60;
        const bool currentPassed =
            rendered.finite && std::isfinite (measured)
            && relativeError <= 0.12;
        passed = passed && currentPassed;

        std::cout << "Cloud T60 " << band.frequency
                  << " Hz: target=" << band.targetT60
                  << " s, measured=" << measured
                  << " s, error=" << relativeError * 100.0
                  << "% -> " << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    return passed;
}

bool testShimmerIntervals()
{
    constexpr double sampleRate = 48000.0;
    NarrowbandAnalyser analyser;
    bool passed = true;

    for (const double fundamental : { 220.0, 440.0 })
    {
        const auto rendered = renderTone (
            3, sampleRate, 128, fundamental, 34.0, 34.0, false);
        std::vector<double> earlyOctaveErrors;
        std::vector<double> lateOctaveErrors;

        for (double seconds = 5.0; seconds <= 10.75; seconds += 0.25)
        {
            const size_t start =
                (size_t) std::llround (seconds * sampleRate);
            const auto octave = analyser.analyse (
                rendered.mid, start, sampleRate,
                fundamental * 2.0, 45.0);

            if (octave.valid)
                earlyOctaveErrors.push_back (std::abs (octave.peakCents));
        }

        for (double seconds = 30.0; seconds <= 32.75; seconds += 0.25)
        {
            const size_t start =
                (size_t) std::llround (seconds * sampleRate);
            const auto octave = analyser.analyse (
                rendered.mid, start, sampleRate,
                fundamental * 2.0, 45.0);

            if (octave.valid)
                lateOctaveErrors.push_back (std::abs (octave.peakCents));
        }

        const double earlyMedian =
            percentile (earlyOctaveErrors, 0.50);
        const double earlyP95 =
            percentile (earlyOctaveErrors, 0.95);
        const double lateMedian =
            percentile (lateOctaveErrors, 0.50);
        const double lateP95 =
            percentile (lateOctaveErrors, 0.95);
        const bool currentPassed =
            rendered.finite
            && rendered.peak < 1.0f
            && earlyOctaveErrors.size() >= 10
            && lateOctaveErrors.size() >= 10
            && earlyMedian <= 1.0
            && earlyP95 <= 2.0
            && lateMedian <= 1.0
            && lateP95 <= 2.0;
        passed = passed && currentPassed;

        std::cout << "Shimmer " << fundamental
                  << " Hz: octave early median/P95="
                  << earlyMedian << "/" << earlyP95
                  << " c, 30s median/P95="
                  << lateMedian << "/" << lateP95
                  << " c -> " << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    return passed;
}

bool testAllPresetsRemainFinite()
{
    bool passed = true;

    for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
    {
        const auto rendered = renderTone (
            preset, 48000.0, 127, 220.0, 0.35, 3.0, true);
        const bool currentPassed = rendered.finite && rendered.peak < 1.0f;
        passed = passed && currentPassed;
        std::cout << std::setw (12) << WorshipReverb::presetName (preset)
                  << ": peak=" << rendered.peak
                  << " -> " << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    return passed;
}

bool testAllPresetImpulseQuality()
{
    struct Metrics
    {
        long double totalEnergy = 0.0;
        long double sideEnergy = 0.0;
        long double earlyEnergy = 0.0;
        long double lateEnergy = 0.0;
        long double sum = 0.0;
        float peak = 0.0f;
        bool finite = true;
        size_t samples = 0;
    };

    bool passed = true;

    for (const double sampleRate : { 44100.0, 96000.0 })
    {
        for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
        {
            constexpr int blockSize = 127;
            constexpr double renderSeconds = 12.0;
            WorshipReverb reverb;
            reverb.setPreset (preset);
            reverb.prepare (sampleRate, blockSize);
            juce::AudioBuffer<float> buffer (2, blockSize);
            const int totalSamples =
                (int) std::ceil (renderSeconds * sampleRate);
            const int earlyEnd = (int) std::ceil (1.5 * sampleRate);
            const int lateStart = (int) std::ceil (10.0 * sampleRate);
            Metrics metrics;

            for (int start = 0; start < totalSamples; start += blockSize)
            {
                buffer.clear();
                const int valid = juce::jmin (
                    blockSize, totalSamples - start);

                if (start == 0)
                {
                    buffer.setSample (0, 0, 0.80f);
                    buffer.setSample (1, 0, 0.80f);
                }

                reverb.process (buffer);

                for (int sample = 0; sample < valid; ++sample)
                {
                    const float left = buffer.getSample (0, sample);
                    const float right = buffer.getSample (1, sample);
                    const float mid = 0.5f * (left + right);
                    const float side = 0.5f * (left - right);
                    const long double stereoEnergy =
                        (long double) left * left
                        + (long double) right * right;
                    const int position = start + sample;
                    metrics.totalEnergy += stereoEnergy;
                    metrics.sideEnergy +=
                        (long double) side * side;
                    metrics.sum += mid;
                    metrics.peak = juce::jmax (
                        metrics.peak, std::abs (left), std::abs (right));
                    metrics.finite = metrics.finite
                                  && std::isfinite (left)
                                  && std::isfinite (right);
                    ++metrics.samples;

                    if (position < earlyEnd)
                        metrics.earlyEnergy += stereoEnergy;
                    if (position >= lateStart)
                        metrics.lateEnergy += stereoEnergy;
                }
            }

            const double rms = metrics.samples > 0
                ? std::sqrt ((double) (
                    metrics.totalEnergy
                    / (long double) (2 * metrics.samples)))
                : 0.0;
            const double dc = metrics.samples > 0
                ? std::abs ((double) (
                    metrics.sum / (long double) metrics.samples))
                : 1.0;
            const double sideRatio = metrics.totalEnergy > 1.0e-30L
                ? (double) (metrics.sideEnergy / metrics.totalEnergy)
                : 0.0;
            const double lateToEarlyDb =
                metrics.earlyEnergy > 1.0e-30L
                ? 10.0 * std::log10 ((double) (
                    juce::jmax (metrics.lateEnergy, 1.0e-30L)
                    / metrics.earlyEnergy))
                : 0.0;
            const bool currentPassed =
                metrics.finite
                && metrics.peak < 1.0f
                && metrics.totalEnergy > 1.0e-10L
                && sideRatio > 1.0e-6
                && dc <= rms * 0.025 + 1.0e-9
                && lateToEarlyDb <= -12.0;
            passed = passed && currentPassed;

            std::cout << std::setw (12)
                      << WorshipReverb::presetName (preset)
                      << " impulse @" << sampleRate
                      << ": peak=" << metrics.peak
                      << " side=" << 100.0 * sideRatio
                      << "% dc/rms="
                      << (rms > 0.0 ? dc / rms : 999.0)
                      << " late/early=" << lateToEarlyDb
                      << " dB -> "
                      << (currentPassed ? "PASS" : "FAIL") << '\n';
        }
    }

    return passed;
}

bool testAllPresetNoiseFloor()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 257;
    constexpr double inputSeconds = 0.35;
    bool passed = true;

    for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
    {
        WorshipReverb reverb;
        reverb.setPreset (preset);
        reverb.prepare (sampleRate, blockSize);
        const double totalSeconds =
            reverb.getTailLengthSeconds() + 1.25;
        const int totalSamples =
            (int) std::ceil (totalSeconds * sampleRate);
        const int inputSamples =
            (int) std::ceil (inputSeconds * sampleRate);
        const int floorStart =
            totalSamples - (int) std::ceil (0.75 * sampleRate);
        juce::AudioBuffer<float> buffer (2, blockSize);
        double phase = 0.0;
        const double phaseIncrement =
            juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
        float peak = 0.0f;
        float floorPeak = 0.0f;
        long double floorEnergy = 0.0L;
        size_t floorSamples = 0;
        bool finite = true;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            buffer.clear();
            const int valid = juce::jmin (
                blockSize, totalSamples - start);

            for (int sample = 0; sample < valid; ++sample)
            {
                const int position = start + sample;
                const float input = position < inputSamples
                    ? 0.95f * (float) std::sin (phase)
                    : 0.0f;
                phase += phaseIncrement;

                if (phase > juce::MathConstants<double>::pi)
                    phase -= juce::MathConstants<double>::twoPi;

                buffer.setSample (0, sample, input);
                buffer.setSample (1, sample, input);
            }

            reverb.process (buffer);

            for (int sample = 0; sample < valid; ++sample)
            {
                const int position = start + sample;
                const float left = buffer.getSample (0, sample);
                const float right = buffer.getSample (1, sample);
                peak = juce::jmax (
                    peak, std::abs (left), std::abs (right));
                finite = finite
                      && std::isfinite (left)
                      && std::isfinite (right);

                if (position >= floorStart)
                {
                    floorPeak = juce::jmax (
                        floorPeak, std::abs (left), std::abs (right));
                    floorEnergy +=
                        (long double) left * left
                        + (long double) right * right;
                    floorSamples += 2;
                }
            }
        }

        const double floorRms = floorSamples > 0
            ? std::sqrt ((double) (
                floorEnergy / (long double) floorSamples))
            : 1.0;
        const double floorDb =
            juce::Decibels::gainToDecibels (floorRms, -200.0);
        const bool currentPassed =
            finite
            && peak < 1.0f
            && floorPeak <= 0.000002f
            && floorDb <= -115.0;
        passed = passed && currentPassed;

        std::cout << std::setw (12)
                  << WorshipReverb::presetName (preset)
                  << " max-return tail: peak=" << peak
                  << " floorPeak=" << floorPeak
                  << " floor=" << floorDb << " dBFS -> "
                  << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    return passed;
}

bool testBlockSizeEnergyConsistency()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blocks[] { 32, 127, 511, 8192 };
    constexpr int presets[] { 1, 3, 7, 8, 9, 11 };
    bool passed = true;

    for (const int preset : presets)
    {
        double referenceRmsDb = 0.0;
        float referencePeak = 0.0f;

        for (size_t blockIndex = 0;
             blockIndex < std::size (blocks); ++blockIndex)
        {
            const auto rendered = renderTone (
                preset, sampleRate, blocks[blockIndex],
                220.0, 1.0, 6.0, true);
            const double currentRmsDb = rmsDb (
                rendered.mid,
                (size_t) std::llround (2.0 * sampleRate),
                (size_t) std::llround (3.0 * sampleRate));

            if (blockIndex == 0)
            {
                referenceRmsDb = currentRmsDb;
                referencePeak = rendered.peak;
            }

            const double rmsDifference =
                std::abs (currentRmsDb - referenceRmsDb);
            const double peakRatio = referencePeak > 1.0e-12f
                ? rendered.peak / referencePeak
                : 999.0;
            const bool currentPassed =
                rendered.finite
                && rendered.peak < 1.0f
                && rmsDifference <= 1.5
                && peakRatio >= 0.70
                && peakRatio <= 1.40;
            passed = passed && currentPassed;

            std::cout << std::setw (12)
                      << WorshipReverb::presetName (preset)
                      << " block=" << blocks[blockIndex]
                      << ": rmsDelta=" << rmsDifference
                      << " dB peakRatio=" << peakRatio
                      << " -> "
                      << (currentPassed ? "PASS" : "FAIL") << '\n';
        }
    }

    return passed;
}

bool testRandomTransitionStress()
{
    struct Configuration
    {
        double sampleRate;
        int blockSize;
    };

    constexpr Configuration configurations[]
    {
        { 44100.0,   32 },
        { 44100.0,  127 },
        { 48000.0,  511 },
        { 48000.0, 8192 },
        { 88200.0,  256 },
        { 96000.0, 2048 }
    };

    WorshipReverb reverb;
    uint32_t state = UINT32_C (0x9e3779b9);
    bool passed = true;
    double renderedSeconds = 0.0;
    const auto started = std::chrono::steady_clock::now();

    for (const auto& configuration : configurations)
    {
        reverb.prepare (
            configuration.sampleRate, configuration.blockSize);
        juce::AudioBuffer<float> buffer (
            2, configuration.blockSize);
        const int blocks = juce::jmax (
            24,
            (int) std::ceil (
                1.2 * configuration.sampleRate
                / configuration.blockSize));
        float peak = 0.0f;
        bool finite = true;

        for (int block = 0; block < blocks; ++block)
        {
            if ((block % 3) == 0)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                reverb.setPreset (
                    (int) (state % WorshipReverb::numPresets));
            }

            for (int sample = 0;
                 sample < configuration.blockSize; ++sample)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                const float noise =
                    (float) (state & UINT32_C (0xffff))
                    / 32767.5f - 1.0f;
                const float left = juce::jlimit (
                    -1.0f, 1.0f, noise * 0.95f);
                const float right = juce::jlimit (
                    -1.0f, 1.0f,
                    -noise * 0.73f + 0.17f);
                buffer.setSample (0, sample, left);
                buffer.setSample (1, sample, right);
            }

            reverb.process (buffer);

            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0;
                     sample < configuration.blockSize; ++sample)
                {
                    const float value =
                        buffer.getSample (channel, sample);
                    peak = juce::jmax (peak, std::abs (value));
                    finite = finite && std::isfinite (value);
                }
        }

        renderedSeconds +=
            (double) blocks * configuration.blockSize
            / configuration.sampleRate;

        reverb.reset();
        buffer.clear();
        reverb.process (buffer);
        const float resetMagnitude =
            juce::jmax (
                buffer.getMagnitude (0, 0, buffer.getNumSamples()),
                buffer.getMagnitude (1, 0, buffer.getNumSamples()));
        const bool currentPassed =
            finite && peak < 1.0f && resetMagnitude == 0.0f;
        passed = passed && currentPassed;

        std::cout << "Stress "
                  << configuration.sampleRate << "/"
                  << configuration.blockSize
                  << ": peak=" << peak
                  << " reset=" << resetMagnitude
                  << " -> "
                  << (currentPassed ? "PASS" : "FAIL") << '\n';
    }

    const double elapsed = std::chrono::duration<double> (
        std::chrono::steady_clock::now() - started).count();
    const double realtimeFactor =
        elapsed > 0.0 ? renderedSeconds / elapsed : 0.0;
    const bool realtimePassed = realtimeFactor >= 1.0;
    passed = passed && realtimePassed;
    std::cout << "Stress throughput: " << realtimeFactor
              << "x realtime -> "
              << (realtimePassed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testFractionalDelayBoundsAndReprepare()
{
    struct Configuration
    {
        double sampleRate;
        int blockSize;
    };

    constexpr Configuration configurations[]
    {
        { 44100.0,   32 },
        { 48000.0,  127 },
        { 96000.0,  511 },
        { 32000.0, 2048 },
        { 48000.0, 8192 }
    };

    WorshipReverb reverb {};
    bool passed = true;
    uint32_t noiseState = UINT32_C (0x6d2b79f5);

    for (const auto& configuration : configurations)
    {
        for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
        {
            reverb.setPreset (preset);
            reverb.prepare (
                configuration.sampleRate, configuration.blockSize);
            juce::AudioBuffer<float> buffer (2, configuration.blockSize);
            const int totalSamples =
                (int) std::ceil (configuration.sampleRate * 0.65);
            double phaseL = 0.0;
            double phaseR = 0.41;
            const double incrementL =
                juce::MathConstants<double>::twoPi * 220.0
                / configuration.sampleRate;
            const double incrementR =
                juce::MathConstants<double>::twoPi * 277.18
                / configuration.sampleRate;

            for (int start = 0; start < totalSamples;
                 start += configuration.blockSize)
            {
                buffer.clear();
                const int valid = juce::jmin (
                    configuration.blockSize, totalSamples - start);

                for (int sample = 0; sample < valid; ++sample)
                {
                    const int position = start + sample;
                    const float envelope =
                        position < (int) (configuration.sampleRate * 0.18)
                        ? 1.0f : 0.0f;
                    noiseState ^= noiseState << 13;
                    noiseState ^= noiseState >> 17;
                    noiseState ^= noiseState << 5;
                    const float noise =
                        ((float) (noiseState & UINT32_C (0xffff))
                            / 32767.5f - 1.0f) * 0.000001f;
                    buffer.setSample (
                        0, sample,
                        envelope * (0.08f * (float) std::sin (phaseL)
                                    + noise));
                    buffer.setSample (
                        1, sample,
                        envelope * (0.075f * (float) std::sin (phaseR)
                                    - noise));
                    phaseL += incrementL;
                    phaseR += incrementR;
                }

                reverb.process (buffer);

                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < configuration.blockSize;
                         ++sample)
                    {
                        const float value =
                            buffer.getSample (channel, sample);
                        passed = passed
                              && std::isfinite (value)
                              && std::abs (value) < 3.0f;
                    }
            }
        }
    }

    std::cout << "Fractional-delay bounds/reprepare: "
              << std::size (configurations) << " rates/blocks x "
              << WorshipReverb::numPresets << " presets -> "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testPresetChanges()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 127;
    constexpr int totalSamples = (int) (12.0 * sampleRate);
    constexpr float amplitude = 0.10f;
    const double phaseIncrement =
        juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
    WorshipReverb reverb;
    reverb.setPreset (1);
    reverb.prepare (sampleRate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::vector<float> output ((size_t) totalSamples, 0.0f);
    double phase = 0.0;
    float peak = 0.0f;
    float maximumStep = 0.0f;
    float previous = 0.0f;
    bool finite = true;
    bool changedToHall = false;
    bool changedToRoom = false;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        if (! changedToHall && start >= (int) (4.0 * sampleRate))
        {
            reverb.setPreset (8);
            changedToHall = true;
        }

        if (! changedToRoom && start >= (int) (8.0 * sampleRate))
        {
            reverb.setPreset (7);
            changedToRoom = true;
        }

        const int valid = juce::jmin (blockSize, totalSamples - start);
        buffer.clear();

        for (int i = 0; i < valid; ++i)
        {
            const float sample = amplitude * (float) std::sin (phase);
            phase += phaseIncrement;

            if (phase > juce::MathConstants<double>::pi)
                phase -= juce::MathConstants<double>::twoPi;

            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        reverb.process (buffer);

        for (int i = 0; i < valid; ++i)
        {
            const float sample = 0.5f * (
                buffer.getSample (0, i) + buffer.getSample (1, i));
            output[(size_t) (start + i)] = sample;
            peak = juce::jmax (peak, std::abs (sample));
            maximumStep = juce::jmax (maximumStep, std::abs (sample - previous));
            previous = sample;
            finite = finite && std::isfinite (sample);
        }
    }

    NarrowbandAnalyser analyser;
    std::vector<double> pitchErrors;

    for (const double seconds : { 2.5, 6.5, 10.5 })
    {
        const auto frame = analyser.analyse (
            output, (size_t) std::llround (seconds * sampleRate),
            sampleRate, 220.0, 50.0);

        if (frame.valid)
            pitchErrors.push_back (std::abs (frame.centroidCents));
    }

    const double worstPitch = pitchErrors.empty()
                            ? 999.0
                            : *std::max_element (
                                  pitchErrors.begin(), pitchErrors.end());
    const bool passed =
        finite && peak < 1.0f && maximumStep < 0.02f
        && pitchErrors.size() == 3 && worstPitch <= 2.0;

    std::cout << "Preset changes Cloud->Hall->Room: max step="
              << maximumStep << ", worst pitch=" << worstPitch
              << " c -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

bool testRapidPresetReversal()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 127;
    constexpr int totalSamples = (int) (4.0 * sampleRate);
    constexpr int comparisonStart = (int) (2.20 * sampleRate);
    constexpr float amplitude = 0.10f;
    const double phaseIncrement =
        juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
    WorshipReverb reference;
    WorshipReverb subject;
    reference.setPreset (1);
    subject.setPreset (1);
    reference.prepare (sampleRate, blockSize);
    subject.prepare (sampleRate, blockSize);
    juce::AudioBuffer<float> referenceBuffer (2, blockSize);
    juce::AudioBuffer<float> subjectBuffer (2, blockSize);
    double phase = 0.0;
    long double errorEnergy = 0.0;
    long double referenceEnergy = 0.0;
    float maximumSettledError = 0.0f;
    float maximumStep = 0.0f;
    float previousLeft = 0.0f;
    bool finite = true;
    bool requestedHall = false;
    bool reversedToCloud = false;
    size_t comparedSamples = 0;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        if (! requestedHall && start >= (int) (2.0 * sampleRate))
        {
            subject.setPreset (8);
            requestedHall = true;
        }

        if (! reversedToCloud && start >= (int) (2.010 * sampleRate))
        {
            subject.setPreset (1);
            reversedToCloud = true;
        }

        const int valid = juce::jmin (blockSize, totalSamples - start);
        referenceBuffer.clear();
        subjectBuffer.clear();

        for (int i = 0; i < valid; ++i)
        {
            const float sample = amplitude * (float) std::sin (phase);
            phase += phaseIncrement;

            if (phase > juce::MathConstants<double>::pi)
                phase -= juce::MathConstants<double>::twoPi;

            referenceBuffer.setSample (0, i, sample);
            referenceBuffer.setSample (1, i, sample);
            subjectBuffer.setSample (0, i, sample);
            subjectBuffer.setSample (1, i, sample);
        }

        reference.process (referenceBuffer);
        subject.process (subjectBuffer);

        for (int i = 0; i < valid; ++i)
        {
            const float subjectLeft = subjectBuffer.getSample (0, i);
            const float subjectRight = subjectBuffer.getSample (1, i);
            const float referenceLeft = referenceBuffer.getSample (0, i);
            const float referenceRight = referenceBuffer.getSample (1, i);
            maximumStep = juce::jmax (
                maximumStep, std::abs (subjectLeft - previousLeft));
            previousLeft = subjectLeft;
            finite = finite
                  && std::isfinite (subjectLeft)
                  && std::isfinite (subjectRight);

            if (start + i >= comparisonStart)
            {
                for (const auto pair : {
                         std::pair<float, float> { subjectLeft, referenceLeft },
                         std::pair<float, float> { subjectRight, referenceRight } })
                {
                    const long double error =
                        (long double) pair.first - pair.second;
                    errorEnergy += error * error;
                    referenceEnergy +=
                        (long double) pair.second * pair.second;
                    maximumSettledError = juce::jmax (
                        maximumSettledError,
                        std::abs (pair.first - pair.second));
                    ++comparedSamples;
                }
            }
        }
    }

    const double relativeErrorDb =
        comparedSamples > 0 && referenceEnergy > 1.0e-24L
        ? 10.0 * std::log10 ((double) (errorEnergy / referenceEnergy))
        : 0.0;
    const bool passed =
        finite
        && requestedHall
        && reversedToCloud
        && subject.getPreset() == 1
        && comparedSamples > 1000
        && maximumStep < 0.02f
        && maximumSettledError <= 1.0e-6f
        && relativeErrorDb <= -100.0;

    std::cout << "Rapid preset reversal Cloud->Hall->Cloud: max step="
              << maximumStep
              << ", settled max error=" << maximumSettledError
              << ", relative error=" << relativeErrorDb
              << " dB -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}
} // namespace

int main (int argc, char** argv)
{
    // O motor roda dentro de TimbresProcessor::processBlock(), que mantém FTZ
    // e DAZ ligados com ScopedNoDenormals durante todo o callback. Replicar o
    // mesmo contrato aqui evita que caudas subnormais de um ensaio anterior
    // contaminem o estado numérico do ensaio seguinte.
    juce::ScopedNoDenormals noDenormals;
    std::cout << std::fixed << std::setprecision (3);
    int failures = 0;

    const auto selected = [argc, argv] (const char* name)
    {
        if (argc <= 1)
            return true;

        for (int argument = 1; argument < argc; ++argument)
            if (std::strcmp (argv[argument], "--all") == 0
                || std::strcmp (argv[argument], name) == 0)
                return true;

        return false;
    };
    const auto explicitlySelected = [argc, argv] (const char* name)
    {
        for (int argument = 1; argument < argc; ++argument)
            if (std::strcmp (argv[argument], name) == 0)
                return true;

        return false;
    };

    if (selected ("--steady") && ! testSteadyPitch())
        ++failures;
    if (explicitlySelected ("--steady-44100-64") && ! testSteadyPitch (0))
        ++failures;
    if (explicitlySelected ("--steady-48000-127") && ! testSteadyPitch (1))
        ++failures;
    if (explicitlySelected ("--steady-48000-256") && ! testSteadyPitch (2))
        ++failures;
    if (explicitlySelected ("--steady-96000-511") && ! testSteadyPitch (3))
        ++failures;
    if (explicitlySelected ("--steady-render-warmup")
        && ! runSteadyRenderWarmupOnly())
        ++failures;
    if (selected ("--cloud-tail") && ! testCloudTail())
        ++failures;
    if (selected ("--legacy-negative")
        && ! testLegacyMovingDelayIsRejected())
        ++failures;
    if (selected ("--cloud-decay") && ! testCloudBandDecay())
        ++failures;
    if (selected ("--shimmer") && ! testShimmerIntervals())
        ++failures;
    if (selected ("--finite") && ! testAllPresetsRemainFinite())
        ++failures;
    if (selected ("--quality") && ! testAllPresetImpulseQuality())
        ++failures;
    if (selected ("--noise-floor") && ! testAllPresetNoiseFloor())
        ++failures;
    if (selected ("--block-consistency")
        && ! testBlockSizeEnergyConsistency())
        ++failures;
    if (selected ("--stress") && ! testRandomTransitionStress())
        ++failures;
    if (selected ("--bounds")
        && ! testFractionalDelayBoundsAndReprepare())
        ++failures;
    if (selected ("--preset-changes") && ! testPresetChanges())
        ++failures;
    if (selected ("--rapid-reversal") && ! testRapidPresetReversal())
        ++failures;

    if (failures == 0)
    {
        std::cout << "WORSHIP_REVERB_TESTS_OK\n";
        return 0;
    }

    std::cerr << "WORSHIP_REVERB_TESTS_FAILED=" << failures << '\n';
    return 1;
}
