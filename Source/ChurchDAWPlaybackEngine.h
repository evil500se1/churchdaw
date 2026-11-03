#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

// Reprodução independente do grafo de instrumentos. Os stems entram depois
// do ChurchDAW graph, então trocar uma cena não reinicia o playback e o click
// permanece no mesmo relógio. A API de load/clear roda na message thread;
// getNextAudioBlock roda exclusivamente no callback de áudio.
class ChurchDAWPlaybackEngine final
{
public:
    static constexpr int maxStems = 4;

    ChurchDAWPlaybackEngine()
        : readAheadThread ("ChurchDAW Playback Read-Ahead")
    {
        formatManager.registerBasicFormats();
        for (auto& gain : gains)
            gain.store (1.0f);
        readAheadEnabled = readAheadThread.startThread();
    }

    ~ChurchDAWPlaybackEngine()
    {
        stop();
        for (auto& stem : stems)
            stem.transport.setSource (nullptr);
        if (readAheadThread.isThreadRunning())
            readAheadThread.stopThread (2000);
    }

    bool loadStem (int index, const juce::File& file)
    {
        if (! juce::isPositiveAndBelow (index, maxStems)
             || ! file.existsAsFile())
            return false;

        auto* reader = formatManager.createReaderFor (file);
        if (reader == nullptr)
            return false;

        auto source = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        const auto sampleRate = reader->sampleRate;
        auto& stem = stems[(size_t) index];
        stem.transport.stop();
        stem.transport.setSource (nullptr);
        stem.reader = std::move (source);
        stem.transport.setSource (
            stem.reader.get(),
            readAheadEnabled ? readAheadBufferSamples : 0,
            readAheadEnabled ? &readAheadThread : nullptr,
            sampleRate,
            2);
        stem.file = file;
        loaded[(size_t) index].store (true);
        return true;
    }

    void clearStem (int index)
    {
        if (! juce::isPositiveAndBelow (index, maxStems))
            return;
        auto& stem = stems[(size_t) index];
        stem.transport.stop();
        stem.transport.setSource (nullptr);
        stem.reader.reset();
        stem.file = juce::File();
        loaded[(size_t) index].store (false);
    }

    void prepareToPlay (int samplesPerBlock, double sampleRate)
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        temp.setSize (2, juce::jmax (samplesPerBlock, 2048), false, true, true);
        for (auto& stem : stems)
            stem.transport.prepareToPlay (temp.getNumSamples(), currentSampleRate);
        clickSamplePosition = 0;
    }

    void releaseResources()
    {
        for (auto& stem : stems)
            stem.transport.releaseResources();
    }

    void start()
    {
        bool hasStem = false;
        for (const auto& isLoadedFlag : loaded)
            hasStem = hasStem || isLoadedFlag.load (std::memory_order_acquire);
        if (! hasStem)
        {
            playing.store (false, std::memory_order_release);
            return;
        }
        clickResetRequested.store (true, std::memory_order_release);
        for (auto& stem : stems)
            if (stem.reader != nullptr)
                stem.transport.start();
        playing.store (true, std::memory_order_release);
    }

    void stop()
    {
        for (auto& stem : stems)
            stem.transport.stop();
        playing.store (false, std::memory_order_release);
    }

    void setStemGain (int index, float gain)
    {
        if (juce::isPositiveAndBelow (index, maxStems) && std::isfinite (gain))
            gains[(size_t) index].store (juce::jlimit (0.0f, 2.0f, gain),
                                         std::memory_order_relaxed);
    }

    void setBpm (double newBpm)
    {
        if (std::isfinite (newBpm))
            bpm.store (juce::jlimit (30.0, 300.0, newBpm), std::memory_order_relaxed);
    }

    void setClickEnabled (bool enabled) noexcept
    {
        clickEnabled.store (enabled, std::memory_order_release);
    }

    bool isPlaying() const noexcept { return playing.load (std::memory_order_acquire); }
    bool isLoaded (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, maxStems)
            && loaded[(size_t) index].load (std::memory_order_acquire);
    }
    bool isClickEnabled() const noexcept
    {
        return clickEnabled.load (std::memory_order_acquire);
    }
    juce::String stemName (int index) const
    {
        return juce::isPositiveAndBelow (index, maxStems)
            ? stems[(size_t) index].file.getFileName() : juce::String();
    }

    // Chamado depois do AudioProcessorPlayer, no mesmo buffer do device.
    void getNextAudioBlock (float* const* output, int numChannels, int numSamples) noexcept
    {
        if (! playing.load (std::memory_order_acquire)
             || output == nullptr || numChannels <= 0 || numSamples <= 0)
            return;

        const int capacity = temp.getNumSamples();
        if (capacity <= 0) return;
        if (clickResetRequested.exchange (false, std::memory_order_acq_rel))
            clickSamplePosition = 0;

        // A larger callback must not drop the entire backing track. Render
        // bounded slices through the already-prepared transport/scratch buffers.
        for (int i = 0; i < maxStems; ++i)
        {
            if (! isLoaded (i)) continue;
            const auto gain = gains[(size_t) i].load (std::memory_order_relaxed);
            for (int offset = 0; offset < numSamples; offset += capacity)
            {
                const int count = juce::jmin (capacity, numSamples - offset);
                temp.clear (0, count);
                juce::AudioSourceChannelInfo info (&temp, 0, count);
                stems[(size_t) i].transport.getNextAudioBlock (info);
                // Playback is mixed after the graph. Reject nonfinite file
                // samples and bound only pathological overloads before gain
                // and summing: four finite float-WAV stems can otherwise
                // overflow to infinity even when each decoded sample is finite.
                // The emergency threshold matches the instrument/master guard;
                // ordinary audio, including modest floating-point headroom,
                // retains its exact samples.
                for (int channel = 0; channel < temp.getNumChannels(); ++channel)
                {
                    auto* data = temp.getWritePointer (channel);
                    for (int sample = 0; sample < count; ++sample)
                        data[sample] = std::isfinite (data[sample])
                            ? juce::jlimit (-24.0f, 24.0f, data[sample]) : 0.0f;
                }
                for (int ch = 0; ch < numChannels; ++ch)
                    if (output[ch] != nullptr)
                        juce::FloatVectorOperations::addWithMultiply (
                            output[ch] + offset,
                            temp.getReadPointer (juce::jmin (ch, temp.getNumChannels() - 1)),
                            gain, count);
            }
        }

        if (! clickEnabled.load (std::memory_order_acquire))
        {
            clickSamplePosition += numSamples;
            return;
        }

        const auto samplesPerBeat = juce::jmax (1.0,
            currentSampleRate * 60.0 / bpm.load (std::memory_order_relaxed));
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto position = clickSamplePosition++;
            const auto beat = static_cast<int> (position / samplesPerBeat);
            const auto inBeat = position - static_cast<juce::int64> (beat * samplesPerBeat);
            if (inBeat < 48.0)
            {
                const auto envelope = 1.0f - (float) inBeat / 48.0f;
                const auto tone = beat % 4 == 0 ? 0.055f : 0.035f;
                const auto click = tone * envelope;
                for (int ch = 0; ch < numChannels; ++ch)
                    if (output[ch] != nullptr)
                        output[ch][sample] += click;
            }
        }
    }

private:
    struct Stem
    {
        juce::AudioTransportSource transport;
        std::unique_ptr<juce::AudioFormatReaderSource> reader;
        juce::File file;
    };

    juce::AudioFormatManager formatManager;
    // Quatro streams podiam ler WAV/AIFF diretamente dentro do callback de
    // audio. O buffer conserva exatamente as mesmas amostras, mas antecipa o
    // acesso ao armazenamento numa thread comum, longe da thread realtime.
    static constexpr int readAheadBufferSamples = 32768;
    juce::TimeSliceThread readAheadThread;
    bool readAheadEnabled = false;
    std::array<Stem, maxStems> stems;
    std::array<std::atomic<bool>, maxStems> loaded {};
    std::array<std::atomic<float>, maxStems> gains;
    std::atomic<bool> playing { false };
    std::atomic<bool> clickEnabled { false };
    std::atomic<double> bpm { 120.0 };
    juce::AudioBuffer<float> temp;
    double currentSampleRate = 44100.0;
    juce::int64 clickSamplePosition = 0; // audio thread (or stopped-device prepare)
    std::atomic<bool> clickResetRequested { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchDAWPlaybackEngine)
};
