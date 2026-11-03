#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <cmath>
#include <memory>

// A short, bounded sample audition, independent of all layer voices and MIDI.
// File I/O and ownership changes stay on the worker. The audio callback only
// tries the lock; it never waits for a decoder or allocates memory.
class TimbreAudition
{
public:
    enum class Status { idle, loading, playing, failed };
    TimbreAudition() : state (std::make_shared<State>()) {}
    ~TimbreAudition() { stop (true); }

    void request (const juce::File& file)
    {
        stop();
        const auto s = state;
        const auto generation = s->generation.load();
        s->hardStop.store (false);
        s->status.store (Status::loading);
        workers.removeAllJobs (true, 0);
        workers.addJob ([s, generation, file]
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
            juce::AudioBuffer<float> decoded;
            double rate = 0;
            bool valid = reader != nullptr && reader->sampleRate >= 8000.0
                && reader->sampleRate <= 192000.0 && reader->lengthInSamples > 1;
            if (valid)
            {
                rate = reader->sampleRate;
                const int length = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (rate * 5.0));
                decoded.setSize (juce::jlimit (1, 2, (int) reader->numChannels), length);
                valid = reader->read (&decoded, 0, length, 0, true, true);
                float peak = 0;
                for (int c = 0; c < decoded.getNumChannels(); ++c)
                    for (int i = 0; i < length; ++i)
                    {
                        auto& value = decoded.getWritePointer (c)[i];
                        if (! std::isfinite (value)) value = 0;
                        peak = juce::jmax (peak, std::abs (value));
                    }
                if (peak > 0.00001f) decoded.applyGain (juce::jmin (4.0f, 1.0f / peak));
            }
            const juce::SpinLock::ScopedLockType lock (s->lock);
            if (s->generation.load() != generation) return;
            if (! valid) { s->status.store (Status::failed); return; }
            s->sample = std::move (decoded);
            s->sampleRate = rate;
            s->position = 0;
            s->envelope = 0;
            s->active = true;
            s->stopping.store (false);
            s->publishedGeneration = generation;
            s->status.store (Status::playing);
        });
    }

    void stop (bool immediately = false) noexcept
    {
        // The generation invalidates work already decoding, including device
        // shutdown, picker dismissal and panic before publication.
        state->generation.fetch_add (1);
        state->stopping.store (true);
        state->hardStop.store (immediately);
        state->status.store (Status::idle);
    }
    void setLevelDb (float db) noexcept
    {
        if (! std::isfinite (db)) db = -24.0f;
        state->gain.store (juce::Decibels::decibelsToGain (juce::jlimit (-48.0f, -12.0f, db)));
    }
    Status getStatus() const noexcept
    {
        const auto status = state->status.load();
        return status == Status::playing && state->publishedGeneration.load() != state->generation.load()
            ? Status::idle : status;
    }
    bool isPlaying() const noexcept
    { const auto status = getStatus(); return status == Status::loading || status == Status::playing; }

    void renderAdd (juce::AudioBuffer<float>& output, double outputRate) noexcept
    {
        auto& s = *state;
        const juce::SpinLock::ScopedTryLockType lock (s.lock);
        if (! lock.isLocked() || ! s.active || ! std::isfinite (outputRate) || outputRate < 8000) return;
        if (s.hardStop.load()) { s.active = false; return; }
        const float step = (float) (1.0 / (outputRate * 0.012));
        const double increment = s.sampleRate / outputRate;
        const int length = s.sample.getNumSamples();
        const bool stopping = s.stopping.load() || s.publishedGeneration.load() != s.generation.load();
        const float gain = s.gain.load();
        for (int i = 0; i < output.getNumSamples(); ++i)
        {
            if (s.position >= length - 1 || (stopping && s.envelope <= 0))
            {
                s.active = false;
                if (s.status.load() == Status::playing) s.status.store (Status::idle);
                break;
            }
            s.envelope = stopping ? juce::jmax (0.0f, s.envelope - step)
                                  : juce::jmin (1.0f, s.envelope + step);
            const float tail = juce::jlimit (0.0f, 1.0f, (float) ((length - 1 - s.position) / (s.sampleRate * 0.04)));
            const int index = (int) s.position;
            const float fraction = (float) (s.position - index);
            for (int c = 0; c < juce::jmin (2, output.getNumChannels()); ++c)
            {
                const auto* samples = s.sample.getReadPointer (juce::jmin (c, s.sample.getNumChannels() - 1));
                const float value = samples[index] + fraction * (samples[index + 1] - samples[index]);
                output.addSample (c, i, juce::jlimit (-1.0f, 1.0f, value) * gain * s.envelope * tail);
            }
            s.position += increment;
        }
    }
private:
    struct State
    {
        juce::SpinLock lock;
        juce::AudioBuffer<float> sample;
        double position = 0, sampleRate = 44100;
        float envelope = 0;
        bool active = false;
        std::atomic<unsigned> generation { 0 }, publishedGeneration { 0 };
        std::atomic<Status> status { Status::idle };
        std::atomic<bool> stopping { false }, hardStop { false };
        std::atomic<float> gain { 0.063095735f }; // -24 dBFS ceiling at the default setting
    };
    std::shared_ptr<State> state;
    juce::ThreadPool workers { 1 };
};
