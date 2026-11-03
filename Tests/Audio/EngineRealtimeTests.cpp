#include "WorshipReverb.h"
#include "MasterProcessor.h"
#include "ChurchDAWPlaybackEngine.h"
#include "SampleStream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

bool runFinalOutputGuardTests();
bool runDiagnosticTestToneTests();

namespace
{
uint64_t hashSample (uint64_t hash, float sample)
{
    uint32_t bits = 0;
    std::memcpy (&bits, &sample, sizeof (bits));
    return (hash ^ bits) * 1099511628211ull;
}
// Deterministic source, generated outside timed regions; no device or user state.
bool measureReverb()
{
    bool finite = true;
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
        for (const int blockSize : { 64, 257, 1024 })
        {
            for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
            {
                WorshipReverb reverb;
                reverb.setPreset (preset);
                reverb.prepare (rate, 8192);
                juce::AudioBuffer<float> buffer (2, blockSize);
                std::vector<double> timings;
                uint64_t hash = 1469598103934665603ull;
                const int blocks = (int) (rate * 1.5 / blockSize);
                timings.reserve ((size_t) blocks);
                double energy = 0;
                for (int block = 0; block < blocks; ++block)
                {
                    // Include a morph that starts mid-block and the tail.
                    if (block == blocks / 2) reverb.setPreset ((preset + 1) % WorshipReverb::numPresets);
                    for (int s = 0; s < blockSize; ++s)
                    {
                        const double t = (double) (block * blockSize + s) / rate;
                        const float x = block < blocks * 3 / 4 ? (float) (0.14 * std::sin (t * 1771.0) + 0.06 * std::sin (t * 7109.0)) : 0;
                        buffer.setSample (0, s, x);
                        buffer.setSample (1, s, x * 0.71f);
                    }
                    const auto start = std::chrono::steady_clock::now();
                    reverb.process (buffer);
                    timings.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count());
                    for (int ch = 0; ch < 2; ++ch)
                        for (int s = 0; s < blockSize; ++s)
                        {
                            const auto value = buffer.getSample (ch, s);
                            finite = finite && std::isfinite (value);
                            energy += (double) value * value;
                            hash = hashSample (hash, value);
                        }
                }
                std::sort (timings.begin(), timings.end());
                const auto median = timings[timings.size() / 2];
                const auto p99 = timings[(timings.size() - 1) * 99 / 100];
                std::cout << "REVERB rate=" << rate << " block=" << blockSize << " preset=" << preset
                          << " hash=" << hash << " energy=" << energy
                          << " median_us=" << median << " p99_us=" << p99 << '\n';
                finite = finite && energy > 0;
            }
        }
    return finite;
}

bool finiteBuffer (const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int s = 0; s < buffer.getNumSamples(); ++s)
            if (! std::isfinite (buffer.getSample (ch, s))) return false;
    return true;
}
bool testMasterRecovery()
{
    MasterProcessor master;
    master.setMasterDb (0.0f); master.setAutoEq (false); master.setPunch (false);
    master.prepareToPlay (48000, 256);
    master.setMasterDb (std::numeric_limits<float>::quiet_NaN());
    bool ok = master.getMasterDb() == 0.0f;
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block)
    {
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 1.0e38f, 256);
        master.processBlock (buffer, midi);
        ok = ok && finiteBuffer (buffer);
    }
    ok = ok && buffer.getMagnitude (0, 256) > 0.1f && buffer.getMagnitude (0, 256) <= 1.0f;
    for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
    {
        buffer.clear(); buffer.setSample (1, 87, bad);
        master.processBlock (buffer, midi);
        ok = ok && finiteBuffer (buffer) && buffer.getMagnitude (0, 256) == 0.0f;
        for (int block = 0; block < 3; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.125f, 256);
            master.processBlock (buffer, midi);
        }
        ok = ok && finiteBuffer (buffer) && buffer.getMagnitude (0, 256) > 0.01f;
    }
    std::cout << "MASTER_EXTREME_FINITE_AND_RECOVERY " << (ok ? "OK" : "FAIL") << '\n';
    return ok;
}

std::vector<float> renderMasterTransition (int blockSize, bool punch)
{
    constexpr int totalSamples = 12288;
    MasterProcessor master;
    master.setAutoEq (false);
    master.setPunch (false);
    master.setMasterDb (0.0f);
    master.prepareToPlay (48000.0, 64);
    juce::MidiBuffer midi;
    std::vector<float> result ((size_t) totalSamples * 2);

    // Match control-event positions exactly; only the callback partition changes.
    // Both the 240-sample Punch ramp and 2400-sample master ramp end inside
    // the 4096-sample callback, rather than on its boundary.
    for (int position = 0; position < totalSamples;)
    {
        if (position == 0)
        {
            if (punch) master.setPunch (true);
            else master.setMasterDb (-12.0f);
        }
        if (position == 8192)
        {
            if (punch) master.setPunch (false);
            else master.setMasterDb (0.0f);
        }
        const int nextEvent = position < 8192 ? 8192 : totalSamples;
        const int count = std::min (blockSize, nextEvent - position);
        juce::AudioBuffer<float> buffer (2, count);
        for (int sample = 0; sample < count; ++sample)
        {
            const float input = 0.09f + 0.02f * std::sin ((float) (position + sample) * 0.031f);
            buffer.setSample (0, sample, input);
            buffer.setSample (1, sample, input);
        }
        master.processBlock (buffer, midi);
        for (int channel = 0; channel < 2; ++channel)
            std::copy_n (buffer.getReadPointer (channel), count,
                         result.begin() + channel * totalSamples + position);
        position += count;
    }
    return result;
}

bool testMasterTransitionPartitions()
{
    bool ok = true;
    for (const bool punch : { false, true })
    {
        const auto reference = renderMasterTransition (64, punch);
        float error = 0.0f, stereoError = 0.0f;
        for (const int blockSize : { 1, 257, 4096, 8192 })
        {
            const auto subject = renderMasterTransition (blockSize, punch);
            for (size_t sample = 0; sample < subject.size(); ++sample)
            {
                ok = ok && std::isfinite (subject[sample]);
                error = std::max (error, std::abs (subject[sample] - reference[sample]));
            }
            for (size_t sample = 0; sample < subject.size() / 2; ++sample)
                stereoError = std::max (stereoError, std::abs (
                    subject[sample] - subject[sample + subject.size() / 2]));
        }
        const bool passed = error < 2.0e-6f && stereoError < 1.0e-7f;
        ok = ok && passed;
        std::cout << "MASTER_" << (punch ? "PUNCH" : "GAIN")
                  << "_CALLBACK_PARTITIONS " << (passed ? "OK" : "FAIL")
                  << " error=" << error << " stereo_error=" << stereoError << '\n';
    }
    return ok;
}

bool testPlaybackExtremeFile()
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getNonexistentChildFile ("churchdaw-extreme-float-stem", ".wav", false);
    const juce::ScopeGuard cleanup { [&] { file.deleteFile(); } };
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (! stream) return false;
        auto writer = format.createWriterFor (stream,
            juce::AudioFormatWriterOptions()
                .withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (32)
                .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (! writer) return false;
        juce::AudioBuffer<float> samples (2, 48000);
        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (samples.getWritePointer (channel),
                std::numeric_limits<float>::max() * 0.49f, samples.getNumSamples());
        if (! writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples())) return false;
    }
    ChurchDAWPlaybackEngine playback;
    playback.prepareToPlay (64, 48000.0);
    for (int stem = 0; stem < ChurchDAWPlaybackEngine::maxStems; ++stem)
    {
        if (! playback.loadStem (stem, file)) return false;
        playback.setStemGain (stem, 2.0f);
    }
    playback.start();
    juce::AudioBuffer<float> output (2, 256);
    output.clear();
    playback.getNextAudioBlock (output.getArrayOfWritePointers(), 2, output.getNumSamples());
    const bool ok = finiteBuffer (output) && output.getMagnitude (0, 256) > 0.1f;
    std::cout << "PLAYBACK_EXTREME_FINITE_FILE " << (ok ? "OK" : "FAIL") << '\n';
    return ok;
}
bool writeFixture (const juce::File& file)
{
    auto stream = file.createOutputStream();
    if (! stream) return false;
    juce::WavAudioFormat format;
    auto writer = std::unique_ptr<juce::AudioFormatWriter> (
        format.createWriterFor (stream.release(), 48000, 2, 24, {}, 0));
    if (! writer) return false;
    juce::AudioBuffer<float> samples (2, 192000);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill (samples.getWritePointer (ch), 0.125f, samples.getNumSamples());
    return writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples());
}
bool testPlayback (const juce::File& file)
{
    ChurchDAWPlaybackEngine playback;
    playback.prepareToPlay (64, 48000);
    if (! playback.loadStem (0, file)) return false;
    playback.start();
    juce::AudioBuffer<float> output (2, 8208);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill (output.getWritePointer (ch), 0.03125f, output.getNumSamples());
    playback.getNextAudioBlock (output.getArrayOfWritePointers(), 2, 8192);
    bool ok = finiteBuffer (output) && output.getSample (0, 8191) > 0.14f
              && output.getSample (1, 8191) > 0.14f
              && output.getSample (0, 8192) == 0.03125f;
    playback.setStemGain (0, std::numeric_limits<float>::quiet_NaN());
    playback.setBpm (std::numeric_limits<double>::quiet_NaN());
    playback.setClickEnabled (true);
    output.clear();
    playback.getNextAudioBlock (output.getArrayOfWritePointers(), 2, 64);
    // Click clock advanced while disabled: enabling between beats must not
    // restart it and add an unscheduled click over the constant stem.
    ok = ok && finiteBuffer (output) && std::abs (output.getSample (0, 0) - 0.125f) < 0.00001f;
    std::cout << "PLAYBACK_OVERSIZED_PARAMETER_SAFETY_AND_CLOCK " << (ok ? "OK" : "FAIL") << '\n';
    return ok;
}
bool testStreamIdleAndWake (const juce::File& file)
{
    SampleStreamer::Source source;
    source.file = file; source.numChannels = 2; source.totalSamples = 192000;
    SampleStreamer streamer;
    streamer.prepare (4);
    juce::Thread::sleep (220);
    const auto passes = streamer.getWorkerPassCountForTest();
    bool ok = passes <= 10; // old 2 ms polling performs about 100 empty scans
    auto& voice = streamer.stream (0);
    voice.restart (&source);
    ok = voice.requestFirstWindow (48000, 256) && ok;
    streamer.wake();
    const auto started = juce::Time::getMillisecondCounterHiRes();
    bool acquired = false;
    while (juce::Time::getMillisecondCounterHiRes() - started < 1000)
    {
        const float *left = nullptr, *right = nullptr; int offset = 0;
        SampleStreamer::VoiceStream::ReadAccess access;
        if (voice.acquire (48000, 256, left, right, offset, access))
        {
            acquired = std::abs (left[offset] - 0.125f) < 0.00001f
                    && std::abs (right[offset] - 0.125f) < 0.00001f;
            break;
        }
        juce::Thread::sleep (1);
    }
    voice.stop(); streamer.shutdown();
    ok = ok && acquired;
    std::cout << "STREAM_IDLE_AND_WAKE " << (ok ? "OK" : "FAIL") << " idle_passes=" << passes << '\n';
    return ok;
}
}
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::ScopedNoDenormals noDenormals;
    std::cout << std::fixed << std::setprecision (6);
    if (argc == 2 && juce::String (argv[1]) == "--benchmark") return measureReverb() ? 0 : 1;
    const auto fixture = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getNonexistentChildFile ("churchdaw-engine-regression", ".wav", false);
    const juce::ScopeGuard cleanup { [&] { fixture.deleteFile(); } };
    if (! writeFixture (fixture)) return 1;
    const bool master = testMasterRecovery();
    const bool transitions = testMasterTransitionPartitions();
    const bool playback = testPlayback (fixture);
    const bool extremePlayback = testPlaybackExtremeFile();
    const bool streaming = testStreamIdleAndWake (fixture);
    const bool finalOutput = runFinalOutputGuardTests();
    const bool diagnostic = runDiagnosticTestToneTests();
    return master && transitions && playback && extremePlayback && streaming && finalOutput && diagnostic ? 0 : 1;
}
