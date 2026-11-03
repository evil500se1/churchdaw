#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

// App-owned output diagnostic. Unlike AudioDeviceManager::playTestSound(),
// it enters the mix BEFORE final protection/recording, even with no playback.
class DiagnosticTestTone
{
public:
    void prepare (double rate) noexcept
    {
        // Keep all sample-count conversions bounded even if a driver reports
        // a corrupt finite value. This includes PCM rates through 768 kHz.
        sampleRate = std::isfinite (rate) && rate >= 8000.0 && rate <= 768000.0
            ? rate : 44100.0;
        length = juce::jmax (1, (int) std::lround (sampleRate));
        attack = juce::jmax (1, (int) std::lround (sampleRate * 0.10));
        release = juce::jmax (1, (int) std::lround (sampleRate * 0.25));
        cancelLength = juce::jmax (1, (int) std::lround (sampleRate * 0.005));
        phaseStep = juce::MathConstants<double>::twoPi * 440.0 / sampleRate;
        reset();
    }

    // prepare/reset belong to stopped-device lifecycle; requests are UI-safe.
    void reset() noexcept
    {
        command.store (none, std::memory_order_relaxed);
        active = pendingRestart = false;
        position = cancelRemaining = 0;
        phase = 0.0;
    }
    void requestStart() noexcept { command.store (start, std::memory_order_release); }
    void requestStop() noexcept { command.store (stop, std::memory_order_release); }

    void processAdd (float* const* output, int channels, int samples) noexcept
    {
        if (samples <= 0) return;
        const auto request = command.exchange (none, std::memory_order_acq_rel);
        if (request == start)
        {
            if (active)
            {
                pendingRestart = true;
                if (cancelRemaining == 0) cancelRemaining = cancelLength;
            }
            else begin();
        }
        else if (request == stop)
        {
            pendingRestart = false;
            if (active && cancelRemaining == 0) cancelRemaining = cancelLength;
        }
        for (int sample = 0; sample < samples && active; ++sample)
        {
            float envelope = position < attack ? (float) position / attack : 1.0f;
            if (position >= length - release)
                envelope *= (float) (length - position) / release;
            if (cancelRemaining > 0)
                envelope *= (float) cancelRemaining / cancelLength;
            const float value = 0.5f * (float) std::sin (phase) * envelope;
            if (output != nullptr)
                for (int channel = 0; channel < channels; ++channel)
                    if (output[channel] != nullptr) output[channel][sample] += value;
            phase += phaseStep;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
            const bool cancelled = cancelRemaining > 0 && --cancelRemaining == 0;
            if (++position >= length || cancelled)
            {
                active = false;
                if (pendingRestart) begin();
            }
        }
    }

private:
    void begin() noexcept
    {
        active = true;
        pendingRestart = false;
        position = cancelRemaining = 0;
        phase = 0.0;
    }
    enum Command { none, start, stop };
    std::atomic<int> command { none };
    double sampleRate = 44100.0, phaseStep = juce::MathConstants<double>::twoPi * 440.0 / 44100.0;
    double phase = 0.0;
    int length = 44100, attack = 4410, release = 11025, cancelLength = 221;
    int position = 0, cancelRemaining = 0;
    bool active = false, pendingRestart = false;
};
