#include "FinalOutputGuard.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace
{
struct Rendered
{
    std::vector<float> left, right;
    int latency = 0;
    uint64_t corrections = 0;
    bool accepted = true;
};

template <typename Limiter>
Rendered render (double rate, int block, int signal, bool useGuard = true)
{
    constexpr int total = 32768;
    FinalStereoOutputGuard<Limiter> guard;
    guard.prepare (rate, 64); // All larger blocks exercise underestimated prepare.
    Rendered result;
    result.left.resize (total); result.right.resize (total);
    result.latency = guard.latencySamples();
    juce::AudioBuffer<float> buffer (2, block);
    for (int position = 0; position < total; position += block)
    {
        const int count = std::min (block, total - position);
        for (int i = 0; i < count; ++i)
        {
            const int p = position + i;
            const double t = (double) p / rate;
            float x = 0;
            if (signal == 0) // transparent impulse followed by low musical audio
                x = p == 0 ? 0.2f : p > 512 ? (float) (0.03 * std::sin (t * 1700)) : 0;
            else if (signal == 1) // one bounded instrument, four loud stems + click
                x = p < 20000 ? (float) (0.7 * std::sin (t * 1771)
                    + 0.72 * std::sin (t * 731) + 0.72 * std::sin (t * 2191)
                    + 0.72 * std::sin (t * 6329) + 0.72 * std::sin (t * 14531)
                    + (p % 24000 < 48 ? 0.05 : 0)) : 0;
            else if (signal == 2) // nonfinite/pathological burst then quiet recovery
                x = p == 17 ? std::numeric_limits<float>::quiet_NaN()
                  : p == 39 ? std::numeric_limits<float>::infinity()
                  : p == 67 ? std::numeric_limits<float>::max()
                  : p > 16384 ? 0.125f : 0;
            else if (signal == 3) // same nonlinear intersample adversary as production probe
                x = p < 20000 ? (float) (1.3 * std::sin (2.0 * juce::MathConstants<double>::pi * 34.0 * p / 71.0)) : 0;
            buffer.setSample (0, i, x);
            buffer.setSample (1, i, x * 0.37f);
        }
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, count);
        if (useGuard)
        {
            result.accepted = guard.process (view) && result.accepted;
        }
        std::copy_n (buffer.getReadPointer (0), count, result.left.begin() + position);
        std::copy_n (buffer.getReadPointer (1), count, result.right.begin() + position);
    }
    result.corrections = guard.emergencyCorrections();
    return result;
}

float reconstructedPeak (const std::vector<float>& signal)
{
    // Independent 8x, 64-tap Hann-windowed sinc reconstruction. Reported as
    // a measured test bound, not a standards-certified true-peak meter.
    constexpr int taps = 64, phases = 8;
    std::array<std::array<double, taps>, phases> kernel {};
    for (int phase = 0; phase < phases; ++phase)
    {
        double sum = 0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double x = tap - (taps / 2 - 1) - (double) phase / phases;
            const double sinc = std::abs (x) < 1.0e-12 ? 1.0
                : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            kernel[phase][tap] = sinc * (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * x / (taps / 2)));
            sum += kernel[phase][tap];
        }
        for (auto& value : kernel[phase]) value /= sum;
    }
    double peak = 0;
    for (size_t p = taps; p + taps < signal.size(); ++p)
        for (const auto& phase : kernel)
        {
            double sample = 0;
            for (int tap = 0; tap < taps; ++tap)
                sample += signal[p + tap - taps / 2 + 1] * phase[tap];
            peak = std::max (peak, std::abs (sample));
        }
    return (float) peak;
}

template <typename Limiter>
bool testPolicy (const char* name)
{
    bool ok = true;
    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        auto reference = render<Limiter> (rate, 64, 1);
        const auto raw = render<Limiter> (rate, 257, 1, false);
        float beforePeak = 0, samplePeak = 0, stereoError = 0, blockError = 0;
        uint64_t corrections = 0;
        for (const int block : { 1, 64, 257, 1024, 8192 })
        {
            auto output = render<Limiter> (rate, block, 1);
            corrections += output.corrections;
            ok = ok && output.accepted;
            for (size_t s = 0; s < output.left.size(); ++s)
            {
                ok = ok && std::isfinite (output.left[s]) && std::isfinite (output.right[s]);
                beforePeak = std::max (beforePeak, std::abs (raw.left[s]));
                samplePeak = std::max (samplePeak, std::abs (output.left[s]));
                stereoError = std::max (stereoError, std::abs (output.right[s] - output.left[s] * 0.37f));
                blockError = std::max (blockError, std::abs (output.left[s] - reference.left[s]));
            }
        }
        const auto impulse = render<Limiter> (rate, 257, 0);
        const auto source = render<Limiter> (rate, 257, 0, false);
        float transparencyError = 0;
        for (int s = 0; s < (int) impulse.left.size(); ++s)
            transparencyError = std::max (transparencyError, std::abs (
                impulse.left[(size_t) s] - (s >= impulse.latency ? source.left[(size_t) s - impulse.latency] : 0)));
        auto recovery = render<Limiter> (rate, 1024, 2);
        bool finiteRecovery = true;
        for (const float sample : recovery.left) finiteRecovery = finiteRecovery && std::isfinite (sample);
        const auto adversary = render<Limiter> (rate, 257, 3);
        const float truePeak = std::max (reconstructedPeak (reference.left), reconstructedPeak (adversary.left));
        bool silence = true;
        for (size_t i = 25000; i < reference.left.size(); ++i) silence = silence && reference.left[i] == 0.0f;
        const bool passed = samplePeak <= ceiling + 1e-5f && truePeak <= ceiling + 0.001f
            && stereoError <= 2e-7f && blockError <= 1e-7f && transparencyError <= 1e-7f
            && finiteRecovery && recovery.left.back() > 0.01f && silence && corrections == 0;
        ok = ok && passed;
        std::cout << name << " rate=" << rate << " latency_samples=" << impulse.latency
            << " latency_ms=" << 1000.0 * impulse.latency / rate << " raw_peak=" << beforePeak
            << " output_peak=" << samplePeak << " reconstructed_peak=" << truePeak
            << " partition_error=" << blockError << " stereo_error=" << stereoError
            << " unity_error=" << transparencyError << " emergency=" << corrections
            << " recovery=" << finiteRecovery << " silence=" << silence
            << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    }
    return ok;
}


// Exercise actual callback pointer shapes without opening a device or GUI.
bool runFinalOutputPointerTests()
{
    bool ok = true;
    const auto expect = [&] (bool value, const char* label)
    {
        if (! value) { ok = false; std::cerr << "FINAL_OUTPUT_FAIL " << label << '\n'; }
    };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        FinalOutputGuard guard;
        juce::AudioBuffer<float> buffer (4, 2056);
        float* output[] = { buffer.getWritePointer (0), buffer.getWritePointer (1),
                            buffer.getWritePointer (2), buffer.getWritePointer (3) };
        expect (guard.prepare (rate, 64, 1), "mono prepare");
        const int latency = guard.latencySamples();
        buffer.clear(); buffer.setSample (0, 0, 0.125f); buffer.setSample (0, 2048, 0.25f);
        expect (guard.process (output, 1, 2048), "mono callback accepted");
        expect (buffer.getSample (0, latency) == 0.125f && buffer.getMagnitude (0, 0, latency) == 0,
                "mono exact impulse latency");
        expect (buffer.getSample (0, 2048) == 0.25f, "callback tail canary preserved");

        // Match the application boundary: existing master output first, then
        // an independent stem, then the new final guard. Below threshold the
        // guard must only delay that exact sum, retaining both relative timing
        // and samples. Instrument latency becomes two lookaheads; stem one.
        StereoLookaheadLimiter existingMaster;
        existingMaster.prepare (rate, 64);
        juce::AudioBuffer<float> mixture (2, 2048);
        mixture.clear();
        mixture.setSample (0, 0, 0.125f); mixture.setSample (1, 0, 0.0625f);
        existingMaster.process (mixture);
        mixture.addSample (0, 0, 0.0625f); mixture.addSample (1, 0, 0.03125f);
        juce::AudioBuffer<float> unguarded (mixture);
        guard.prepare (rate, 64, 2);
        guard.process (mixture);
        float cascadeError = 0;
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < mixture.getNumSamples(); ++sample)
                cascadeError = juce::jmax (cascadeError, std::abs (
                    mixture.getSample (channel, sample)
                    - (sample >= latency ? unguarded.getSample (channel, sample - latency) : 0.0f)));
        expect (cascadeError <= 1e-7f && mixture.getSample (0, latency) == 0.0625f
                 && mixture.getSample (0, latency * 2) == 0.125f,
                "existing master plus stem retain exact samples and declared final delay");

        expect (guard.prepare (rate, 64, 2), "stereo prepare");
        buffer.clear(); buffer.setSample (1, 0, 0.2f);
        float* onlyRight[] = { nullptr, output[1] };
        expect (guard.process (onlyRight, 2, 2048), "null left pointer accepted");
        expect (buffer.getSample (1, latency) == 0.2f, "right-only impulse retains channel/timing");
        guard.reset(); buffer.clear(); buffer.setSample (0, 0, 0.2f);
        float* onlyLeft[] = { output[0], nullptr };
        expect (guard.process (onlyLeft, 2, 64), "null right pointer accepted");
        expect (guard.process (nullptr, 2, 512), "all-null callback advances silently");
        expect (guard.process (nullptr, 0, 512), "no-output callback advances silently");
        buffer.clear(); guard.process (output, 2, 512);
        expect (buffer.getMagnitude (0, 512) == 0, "inactive interval does not replay old impulse");

        guard.reset(); buffer.clear(); buffer.setSample (0, 0, 0.125f);
        guard.process (output, 2, 64); guard.reset(); buffer.clear(); guard.process (output, 2, 512);
        expect (buffer.getMagnitude (0, 512) == 0, "route reset removes buffered audio");

        buffer.clear();
        for (int ch = 0; ch < 4; ++ch)
            juce::FloatVectorOperations::fill (output[ch], 0.125f, 2048);
        expect (! guard.process (output, 3, 2048) && guard.unexpectedOutputCount() == 1,
                "unexpected route reported");
        expect (buffer.getMagnitude (2, 0, 2048) == 0.0f
                 && buffer.getSample (0, 2047) == 0.125f,
                "unexpected output silenced without changing prepared pair");

        expect (guard.prepare (rate, 64, 4), "four-output route prepares two pairs");
        for (int s = 0; s < 2048; ++s)
        {
            output[0][s] = 2.7f; output[1][s] = 1.35f;
            output[2][s] = -3.2f; output[3][s] = -0.8f;
        }
        expect (guard.process (output, 4, 2048), "both prepared output pairs protected");
        expect (buffer.getMagnitude (0, 2048) <= juce::Decibels::decibelsToGain (-1.0f) + 1e-5f,
                "all output pairs within ceiling");
        expect (std::abs (output[1][2047] - output[0][2047] * 0.5f) < 1e-7f
                && std::abs (output[3][2047] - output[2][2047] * 0.25f) < 1e-7f,
                "each stereo output pair preserves balance");

        guard.release();
        expect (guard.latencySamples() == 0 && guard.reductionDb() == 0
                && guard.emergencyCorrections() == 0, "stopped-device mirrors clear");
        expect (! guard.process (output, 4, 2048) && buffer.getMagnitude (0, 2048) == 0,
                "unprepared callback cannot bypass output safety");
        expect (guard.prepare (rate, 64, 0) && guard.process (nullptr, 0, 512),
                "disabled audio route accepted silently");
        expect (guard.process (nullptr, 0, 0), "empty callback harmless");
    }
    std::cout << "FINAL_OUTPUT_POINTER_ROUTES " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}

} // namespace

bool runFinalOutputGuardTests()
{
    const bool output = testPolicy<StereoLookaheadLimiter> ("FINAL_OUTPUT_3MS");
    const bool routes = runFinalOutputPointerTests();
    return output && routes;
}
