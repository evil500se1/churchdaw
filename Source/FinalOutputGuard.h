#pragma once
#include "StereoLookaheadLimiter.h"
#include <atomic>
#include <memory>
#include <vector>

// Final protection for the physical mix, including independent playback.
// Call prepare/reset/release only while device callbacks are stopped. Call
// process only from the audio callback. Diagnostic getters are thread-safe.
template <typename Limiter = StereoLookaheadLimiter>
class FinalStereoOutputGuard
{
public:
    bool prepare (double sampleRate, int expectedBlock, int outputChannels = 2)
    {
        prepared = false;
        limiters.clear();
        clearMirrors();
        if (! std::isfinite (sampleRate) || sampleRate < 8000.0
            || outputChannels < 0 || outputChannels > 256)
            return false;
        configuredChannels = outputChannels;
        capacity = juce::jlimit (64, 8192, expectedBlock);
        scratch.setSize (2, capacity, false, true, false);
        for (int pair = 0; pair < (outputChannels + 1) / 2; ++pair)
        {
            auto limiter = std::make_unique<Limiter>();
            limiter->prepare (sampleRate, capacity);
            limiters.push_back (std::move (limiter));
        }
        latency.store (limiters.empty() ? 0 : limiters.front()->getLatencySamples(),
                       std::memory_order_relaxed);
        prepared = true;
        return true;
    }

    void reset() noexcept
    {
        for (auto& limiter : limiters) limiter->reset();
        scratch.clear();
        reduction.store (0.0f, std::memory_order_relaxed);
        corrections.store (0, std::memory_order_relaxed);
        unexpectedOutputs.store (0, std::memory_order_relaxed);
    }

    void release() noexcept
    {
        reset();
        prepared = false;
        latency.store (0, std::memory_order_relaxed);
    }

    int latencySamples() const noexcept { return latency.load (std::memory_order_relaxed); }
    float reductionDb() const noexcept { return reduction.load (std::memory_order_relaxed); }
    uint64_t emergencyCorrections() const noexcept { return corrections.load (std::memory_order_relaxed); }
    uint64_t unexpectedOutputCount() const noexcept { return unexpectedOutputs.load (std::memory_order_relaxed); }

    bool process (juce::AudioBuffer<float>& sum) noexcept
    {
        return process (sum.getArrayOfWritePointers(), sum.getNumChannels(), sum.getNumSamples());
    }

    bool process (float* const* output, int numOutputs, int numSamples) noexcept
    {
        if (numSamples <= 0) return true;
        if (! prepared || numOutputs < 0)
        {
            clearOutputs (output, 0, juce::jmax (0, numOutputs), numSamples);
            return false;
        }
        const bool routeMatches = numOutputs <= configuredChannels;
        if (! routeMatches)
        {
            // Route changes normally call prepare first. A malformed callback
            // cannot silently pass an extra, unprotected loudspeaker channel.
            clearOutputs (output, configuredChannels, numOutputs, numSamples);
            unexpectedOutputs.fetch_add (1, std::memory_order_relaxed);
        }
        uint64_t count = 0;
        float maximumReduction = 0.0f;
        for (size_t pair = 0; pair < limiters.size(); ++pair)
        {
            for (int offset = 0; offset < numSamples; offset += capacity)
            {
                const int samples = juce::jmin (capacity, numSamples - offset);
                for (int channel = 0; channel < 2; ++channel)
                {
                    const int index = (int) pair * 2 + channel;
                    const auto* source = output != nullptr && index < numOutputs
                        && index < configuredChannels ? output[index] : nullptr;
                    if (source != nullptr)
                        scratch.copyFrom (channel, 0, source + offset, samples);
                    else
                        scratch.clear (channel, 0, samples);
                }
                // A stereo external-data AudioBuffer uses JUCE's inline
                // channel-pointer storage, with no allocation or resizing.
                juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), 2, samples);
                limiters[pair]->process (view);
                for (int channel = 0; channel < 2; ++channel)
                {
                    const int index = (int) pair * 2 + channel;
                    auto* destination = output != nullptr && index < numOutputs
                        && index < configuredChannels ? output[index] : nullptr;
                    if (destination != nullptr)
                        juce::FloatVectorOperations::copy (destination + offset,
                            scratch.getReadPointer (channel), samples);
                }
            }
            count += limiters[pair]->getEmergencyCorrectionCount();
            maximumReduction = juce::jmin (maximumReduction, limiters[pair]->getCurrentGainReductionDb());
        }
        corrections.store (count, std::memory_order_relaxed);
        reduction.store (maximumReduction, std::memory_order_relaxed);
        return routeMatches;
    }

private:
    static void clearOutputs (float* const* output, int first, int end, int samples) noexcept
    {
        if (output != nullptr)
            for (int channel = first; channel < end; ++channel)
                if (output[channel] != nullptr)
                    juce::FloatVectorOperations::clear (output[channel], samples);
    }
    void clearMirrors() noexcept
    {
        latency.store (0, std::memory_order_relaxed);
        reduction.store (0.0f, std::memory_order_relaxed);
        corrections.store (0, std::memory_order_relaxed);
        unexpectedOutputs.store (0, std::memory_order_relaxed);
    }
    std::vector<std::unique_ptr<Limiter>> limiters;
    juce::AudioBuffer<float> scratch;
    int configuredChannels = 0, capacity = 64;
    bool prepared = false;
    std::atomic<int> latency { 0 };
    std::atomic<float> reduction { 0.0f };
    std::atomic<uint64_t> corrections { 0 }, unexpectedOutputs { 0 };
};

using FinalOutputGuard = FinalStereoOutputGuard<>;
