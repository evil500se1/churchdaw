#include "PadSynthProcessor.h"
#include "DeterministicAudioCapture.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using ChurchDAWTest::AudioCapture;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t scenarioVersion = 1;
constexpr double sampleRate = 48000.0;
// O horizonte de prova permanece em 851968 frames. O processamento continua
// normalmente além da captura; o gate final aceita zero bitwise ou apenas o
// piso numérico multiplataforma extremamente baixo documentado abaixo.
constexpr std::uint64_t captureFrames = 131072;
constexpr std::uint64_t renderFrames = 851968;
constexpr std::uint64_t chordFrame = 4096;
constexpr std::uint64_t sustainDownFrame = 24593;
constexpr std::uint64_t noteOffFrame = 32768;
constexpr std::uint64_t firstAutomationFrame = 49152;
constexpr std::uint64_t secondAutomationFrame = 65536;
constexpr std::uint64_t sustainUpFrame = 73741;
constexpr std::size_t guardSamples = 32;
constexpr std::uint32_t guardBits = UINT32_C (0x4b123456);
constexpr double baselineMaxAbsolute = 3.0e-5;
constexpr double baselineRmsNull = 2.0e-6;
// This is a stuck-voice/effect-tail diagnostic for the final one-second
// window, not a musical-equivalence tolerance. The x86_64 JUCE reverb can
// remain at about 1.824544e-7 peak / 3.76727e-8 RMS without any active voice.
// These fixed limits add a small portability margin while remaining below
// -132 dBFS peak and -146 dBFS RMS. Production DSP is never reset by the probe.
constexpr double finalSilencePeakFloor = 2.5e-7;
constexpr double finalSilenceRmsFloor = 5.0e-8;
constexpr double minimumTailDecayRatio = 1.0e-4; // At least 80 dB below tail.

juce::File juceFileForPath (const std::filesystem::path& path)
{
   #if JUCE_WINDOWS
    // std::filesystem::path is UTF-16 on Windows, and JUCE has a native
    // wchar_t overload. This avoids the locale-dependent path.string().
    return juce::File (juce::String (path.c_str()));
   #else
    // Native POSIX paths are bytes. Apple filesystems expose those bytes as
    // UTF-8, which must be stated explicitly because String (const char*) is
    // intentionally ASCII-only in JUCE.
    return juce::File (juce::String (juce::CharPointer_UTF8 (path.c_str())));
   #endif
}

std::string sha256FileHex (const std::filesystem::path& path)
{
    return juce::SHA256 (juceFileForPath (path))
        .toHexString().toLowerCase().toStdString();
}

struct ScheduledMidi
{
    std::uint64_t frame = 0;
    juce::MidiMessage message;
};

struct RegionMetrics
{
    double peak = 0.0;
    double rms = 0.0;
};

struct RenderResult
{
    AudioCapture capture;
    RegionMetrics silence;
    RegionMetrics activeChord;
    RegionMetrics sustainedChord;
    RegionMetrics releasedTail;
    RegionMetrics finalSilence;
    std::uint64_t finalSilenceSampleCount = 0;
    std::uint64_t finalSilenceNonFiniteCount = 0;
    std::uint64_t finalSilenceNonZeroBitCount = 0;
    int midiEventsDelivered = 0;
    int automationPointsApplied = 0;
    double setupMilliseconds = 0.0;
    double callbackMilliseconds = 0.0;
};

struct FinalSilenceAssessment
{
    bool finite = false;
    bool exactBitwiseZero = false;
    bool withinNumericalFloor = false;

    bool passed() const noexcept
    {
        return finite && (exactBitwiseZero || withinNumericalFloor);
    }
};

FinalSilenceAssessment assessFinalSilence (
    const RegionMetrics& metrics,
    std::uint64_t nonFiniteCount,
    std::uint64_t nonZeroBitCount) noexcept
{
    FinalSilenceAssessment result;
    result.finite = nonFiniteCount == 0
                 && std::isfinite (metrics.peak)
                 && std::isfinite (metrics.rms)
                 && metrics.peak >= 0.0
                 && metrics.rms >= 0.0;
    result.exactBitwiseZero = result.finite
                           && nonZeroBitCount == 0
                           && metrics.peak == 0.0
                           && metrics.rms == 0.0;
    result.withinNumericalFloor = result.finite
                               && metrics.peak <= finalSilencePeakFloor
                               && metrics.rms <= finalSilenceRmsFloor;
    return result;
}

double amplitudeDbfs (double amplitude) noexcept
{
    return amplitude > 0.0
        ? 20.0 * std::log10 (amplitude)
        : -std::numeric_limits<double>::infinity();
}

class GuardedStereoBlock
{
public:
    explicit GuardedStereoBlock (int samples)
        : numSamples (samples)
    {
        if (numSamples <= 0)
            throw std::runtime_error ("block size must be positive");

        for (int channel = 0; channel < 2; ++channel)
        {
            storage[(std::size_t) channel].resize (
                (std::size_t) numSamples + guardSamples * 2);
            channels[(std::size_t) channel] =
                storage[(std::size_t) channel].data() + guardSamples;
        }
        buffer.setDataToReferTo (channels.data(), 2, numSamples);
        reset();
    }

    void reset()
    {
        const float guard = ChurchDAWTest::floatFromBits (guardBits);
        for (auto& channel : storage)
        {
            std::fill (channel.begin(), channel.end(), guard);
            std::fill (channel.begin() + (std::ptrdiff_t) guardSamples,
                       channel.begin() + (std::ptrdiff_t) guardSamples
                           + numSamples,
                       std::numeric_limits<float>::quiet_NaN());
        }
        // The storage is external, so JUCE cannot observe the NaN prefill.
        // Force the next processor clear() to touch every active sample.
        buffer.setNotClear();
    }

    bool guardsIntact (std::string& error) const
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto& data = storage[(std::size_t) channel];
            for (std::size_t i = 0; i < guardSamples; ++i)
            {
                if (ChurchDAWTest::floatBits (data[i]) != guardBits)
                {
                    error = "buffer underrun guard changed on channel "
                          + std::to_string (channel) + " at guard sample "
                          + std::to_string (i);
                    return false;
                }
            }

            const auto suffix = guardSamples + (std::size_t) numSamples;
            for (std::size_t i = 0; i < guardSamples; ++i)
            {
                if (ChurchDAWTest::floatBits (data[suffix + i]) != guardBits)
                {
                    error = "buffer overrun guard changed on channel "
                          + std::to_string (channel) + " at guard sample "
                          + std::to_string (i);
                    return false;
                }
            }
        }
        return true;
    }

    bool activeSamplesFinite (std::string& error) const
    {
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < numSamples; ++sample)
                if (! std::isfinite (
                        channels[(std::size_t) channel][sample]))
                {
                    error = "NaN/Inf or unwritten sample on channel "
                          + std::to_string (channel) + " at sample "
                          + std::to_string (sample);
                    return false;
                }
        return true;
    }

    juce::AudioBuffer<float>& audio() noexcept { return buffer; }

private:
    int numSamples = 0;
    std::array<std::vector<float>, 2> storage;
    std::array<float*, 2> channels {};
    juce::AudioBuffer<float> buffer;
};

void setParameter (juce::RangedAudioParameter* parameter, float realValue,
                   const char* name)
{
    if (parameter == nullptr)
        throw std::runtime_error (std::string ("missing PadSynth parameter: ")
                                  + name);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (realValue));
}

void configureProcessor (PadSynthProcessor& processor, int blockSize)
{
    setParameter (processor.masterDb, -16.0f, "masterDb");
    setParameter (processor.midiChannel, 0.0f, "midiChannel");
    setParameter (processor.transpose, 0.0f, "transpose");
    setParameter (processor.velocityCurve, 1.0f, "velocityCurve");
    setParameter (processor.fixedVelocity, 104.0f, "fixedVelocity");
    setParameter (processor.holdMode, 0.0f, "holdMode");
    setParameter (processor.toggleMode, 0.0f, "toggleMode");
    setParameter (processor.toneLow, 1.7f, "toneLow");
    setParameter (processor.toneMid, -1.1f, "toneMid");
    setParameter (processor.toneHigh, 2.2f, "toneHigh");
    setParameter (processor.chorusMix, 0.23f, "chorusMix");
    setParameter (processor.reverbMix, 0.24f, "reverbMix");
    setParameter (processor.delayMix, 0.17f, "delayMix");
    setParameter (processor.delayTime, 0.19f, "delayTime");
    setParameter (processor.delayFeedback, 0.27f, "delayFeedback");
    setParameter (processor.eqEnabled, 1.0f, "eqEnabled");
    setParameter (processor.chorusEnabled, 1.0f, "chorusEnabled");
    setParameter (processor.reverbEnabled, 1.0f, "reverbEnabled");
    setParameter (processor.delayEnabled, 1.0f, "delayEnabled");
    setParameter (processor.macroAmbience, 0.61f, "macroAmbience");
    setParameter (processor.macroReverb, 0.58f, "macroReverb");
    setParameter (processor.macroDelay, 0.37f, "macroDelay");
    setParameter (processor.macroMaster, 0.51f, "macroMaster");
    setParameter (processor.xyX, 0.44f, "xyX");
    setParameter (processor.xyY, 0.63f, "xyY");
    setParameter (processor.fadeIn, 0.01f, "fadeIn");
    setParameter (processor.fadeOut, 0.01f, "fadeOut");
    setParameter (processor.utilityAttack, 1.0f, "utilityAttack");
    setParameter (processor.utilityRelease, 1.0f, "utilityRelease");
    setParameter (processor.gateEnabled, 1.0f, "gateEnabled");
    setParameter (processor.gateRate, 2.0f, "gateRate");
    setParameter (processor.gateDepth, 0.38f, "gateDepth");

    for (int index = 0; index < PadSynthProcessor::numLayers; ++index)
    {
        auto& layer = processor.layer (index);
        setParameter (layer.enabled, 1.0f, "layer.enabled");
        setParameter (layer.osc1Wave, (float) ((index + 2) % 5),
                      "layer.osc1Wave");
        setParameter (layer.osc2Wave, (float) ((index + 1) % 5),
                      "layer.osc2Wave");
        setParameter (layer.subWave, (float) (index % 3), "layer.subWave");
        setParameter (layer.noiseType, (float) (index % 3),
                      "layer.noiseType");
        setParameter (layer.filterType, (float) (index % 4),
                      "layer.filterType");
        setParameter (layer.osc1Octave, index == 3 ? 1.0f : 0.0f,
                      "layer.osc1Octave");
        setParameter (layer.osc2Octave, index == 0 ? -1.0f : 0.0f,
                      "layer.osc2Octave");
        setParameter (layer.subOctave, -2.0f, "layer.subOctave");
        setParameter (layer.osc2Tune, -9.0f + 6.0f * (float) index,
                      "layer.osc2Tune");
        setParameter (layer.osc1Level, 0.24f + 0.025f * (float) index,
                      "layer.osc1Level");
        setParameter (layer.osc2Level, 0.14f + 0.015f * (float) index,
                      "layer.osc2Level");
        setParameter (layer.subLevel, 0.08f, "layer.subLevel");
        setParameter (layer.noiseLevel, 0.018f + 0.004f * (float) index,
                      "layer.noiseLevel");
        setParameter (layer.cutoff, 1250.0f + 920.0f * (float) index,
                      "layer.cutoff");
        setParameter (layer.resonance, 0.17f + 0.07f * (float) index,
                      "layer.resonance");
        setParameter (layer.filterEnv, -0.18f + 0.15f * (float) index,
                      "layer.filterEnv");
        setParameter (layer.drive, 0.08f + 0.05f * (float) index,
                      "layer.drive");
        setParameter (layer.attack, 0.006f + 0.003f * (float) index,
                      "layer.attack");
        setParameter (layer.decay, 0.11f + 0.03f * (float) index,
                      "layer.decay");
        setParameter (layer.sustain, 0.68f + 0.04f * (float) index,
                      "layer.sustain");
        setParameter (layer.release, 0.34f + 0.09f * (float) index,
                      "layer.release");
        setParameter (layer.modAttack, 0.009f + 0.004f * (float) index,
                      "layer.modAttack");
        setParameter (layer.modDecay, 0.16f + 0.04f * (float) index,
                      "layer.modDecay");
        setParameter (layer.modSustain, 0.46f + 0.06f * (float) index,
                      "layer.modSustain");
        setParameter (layer.modRelease, 0.30f + 0.08f * (float) index,
                      "layer.modRelease");
        setParameter (layer.pan, -0.45f + 0.30f * (float) index,
                      "layer.pan");
        setParameter (layer.lfoWave, (float) (index % 4), "layer.lfoWave");
        setParameter (layer.lfoRate, 0.21f + 0.17f * (float) index,
                      "layer.lfoRate");
        setParameter (layer.lfoDepth, 0.12f + 0.06f * (float) index,
                      "layer.lfoDepth");
        setParameter (layer.lfoFade, 0.04f * (float) index,
                      "layer.lfoFade");
        setParameter (layer.lfoPhase, 37.0f * (float) index,
                      "layer.lfoPhase");
    }

    processor.setEngineEnabled (true);
    processor.prepareToPlay ((double) sampleRate, blockSize);
}

std::vector<ScheduledMidi> makeMidiSchedule()
{
    static constexpr std::array<int, 6> notes { 48, 55, 60, 64, 67, 72 };
    static constexpr std::array<int, 6> velocities { 91, 99, 107, 113, 101, 95 };
    std::vector<ScheduledMidi> events;
    events.reserve (18);

    for (std::size_t i = 0; i < notes.size(); ++i)
        events.push_back ({ chordFrame + i * 19,
                            juce::MidiMessage::noteOn (
                                1, notes[i], (juce::uint8) velocities[i]) });

    events.push_back ({ sustainDownFrame,
                        juce::MidiMessage::controllerEvent (1, 64, 127) });
    for (std::size_t i = 0; i < notes.size(); ++i)
        events.push_back ({ noteOffFrame + i * 13,
                            juce::MidiMessage::noteOff (1, notes[i]) });

    events.push_back ({ 40973, juce::MidiMessage::pitchWheel (1, 12288) });
    events.push_back ({ 57371, juce::MidiMessage::pitchWheel (1, 8192) });
    events.push_back ({ sustainUpFrame,
                        juce::MidiMessage::controllerEvent (1, 64, 0) });

    std::stable_sort (events.begin(), events.end(),
                      [] (const auto& a, const auto& b)
                      {
                          return a.frame < b.frame;
                      });
    return events;
}

int applyAutomation (PadSynthProcessor& processor,
                     std::uint64_t blockStart)
{
    if (blockStart == firstAutomationFrame)
    {
        setParameter (processor.layer (0).cutoff, 6400.0f,
                      "automation.layer0.cutoff");
        setParameter (processor.layer (2).lfoDepth, 0.61f,
                      "automation.layer2.lfoDepth");
        setParameter (processor.xyX, 0.76f, "automation.xyX");
        return 1;
    }

    if (blockStart == secondAutomationFrame)
    {
        setParameter (processor.layer (0).cutoff, 940.0f,
                      "automation.layer0.cutoff");
        setParameter (processor.layer (2).lfoDepth, 0.19f,
                      "automation.layer2.lfoDepth");
        setParameter (processor.toneHigh, -2.8f, "automation.toneHigh");
        setParameter (processor.gateDepth, 0.57f, "automation.gateDepth");
        return 1;
    }
    return 0;
}

RegionMetrics measureRegion (const AudioCapture& capture,
                             std::uint64_t firstFrame,
                             std::uint64_t endFrame)
{
    if (firstFrame >= endFrame || endFrame > capture.frames)
        throw std::runtime_error ("invalid diagnostic region");

    RegionMetrics result;
    long double sumSquares = 0.0;
    std::uint64_t count = 0;
    for (std::uint64_t frame = firstFrame; frame < endFrame; ++frame)
        for (std::uint32_t channel = 0; channel < capture.channels; ++channel)
        {
            const auto index = static_cast<std::size_t> (
                frame * capture.channels + channel);
            const float sample = capture.interleaved[index];
            if (! std::isfinite (sample))
                return { std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN() };
            result.peak = std::max (
                result.peak, std::abs (static_cast<double> (sample)));
            const auto wide = static_cast<long double> (sample);
            sumSquares += wide * wide;
            ++count;
        }
    result.rms = count == 0 ? 0.0 : std::sqrt (static_cast<double> (
        sumSquares / static_cast<long double> (count)));
    return result;
}

RenderResult renderScenario (int blockSize, bool measureCallbacks = false)
{
    if (blockSize != 128 && blockSize != 256 && blockSize != 512)
        throw std::runtime_error ("scenario supports block sizes 128/256/512");
    if (captureFrames % (std::uint64_t) blockSize != 0
         || renderFrames % (std::uint64_t) blockSize != 0)
        throw std::runtime_error ("scenario frame count must divide block size");

    const auto setupStarted = Clock::now();
    PadSynthProcessor processor;
    configureProcessor (processor, blockSize);
    const auto schedule = makeMidiSchedule();
    std::size_t nextEvent = 0;
    GuardedStereoBlock guarded (blockSize);
    const auto setupStopped = Clock::now();

    RenderResult result;
    result.setupMilliseconds =
        std::chrono::duration<double, std::milli> (
            setupStopped - setupStarted).count();
    result.capture.scenarioVersion = scenarioVersion;
    result.capture.sampleRate = sampleRate;
    result.capture.blockSize = (std::uint32_t) blockSize;
    result.capture.channels = 2;
    result.capture.frames = captureFrames;
    result.capture.interleaved.reserve (
        (std::size_t) captureFrames * result.capture.channels);
    long double finalSilenceSquares = 0.0;

    for (std::uint64_t blockStart = 0; blockStart < renderFrames;
         blockStart += (std::uint64_t) blockSize)
    {
        result.automationPointsApplied += applyAutomation (processor, blockStart);
        juce::MidiBuffer midi;
        const auto blockEnd = blockStart + (std::uint64_t) blockSize;
        while (nextEvent < schedule.size()
               && schedule[nextEvent].frame < blockEnd)
        {
            if (schedule[nextEvent].frame < blockStart)
                throw std::runtime_error ("MIDI scheduler skipped an event");
            midi.addEvent (
                schedule[nextEvent].message,
                (int) (schedule[nextEvent].frame - blockStart));
            ++nextEvent;
            ++result.midiEventsDelivered;
        }

        guarded.reset();
        const bool timeThisCallback = measureCallbacks
                                   && blockStart < captureFrames;
        Clock::time_point callbackStarted;
        if (timeThisCallback)
            callbackStarted = Clock::now();
        processor.processBlock (guarded.audio(), midi);
        if (timeThisCallback)
            result.callbackMilliseconds +=
                std::chrono::duration<double, std::milli> (
                    Clock::now() - callbackStarted).count();

        std::string guardError;
        if (! guarded.guardsIntact (guardError))
            throw std::runtime_error (
                guardError + " in callback block starting at frame "
                + std::to_string (blockStart));
        if (! guarded.activeSamplesFinite (guardError))
            throw std::runtime_error (
                guardError + " in callback block starting at frame "
                + std::to_string (blockStart));

        for (int sample = 0; sample < blockSize; ++sample)
            for (int channel = 0; channel < 2; ++channel)
            {
                const float value = guarded.audio().getSample (channel, sample);
                const auto absoluteFrame = blockStart
                    + static_cast<std::uint64_t> (sample);
                if (absoluteFrame < captureFrames)
                    result.capture.interleaved.push_back (value);
                if (absoluteFrame >= renderFrames
                        - static_cast<std::uint64_t> (sampleRate))
                {
                    ++result.finalSilenceSampleCount;
                    if (! std::isfinite (value))
                    {
                        ++result.finalSilenceNonFiniteCount;
                    }
                    else
                    {
                        result.finalSilence.peak = std::max (
                            result.finalSilence.peak,
                            std::abs (static_cast<double> (value)));
                        const auto wide = static_cast<long double> (value);
                        finalSilenceSquares += wide * wide;
                    }
                    if (ChurchDAWTest::floatBits (value)
                         != ChurchDAWTest::floatBits (0.0f))
                        ++result.finalSilenceNonZeroBitCount;
                }
            }
    }

    if (nextEvent != schedule.size())
        throw std::runtime_error ("MIDI scheduler did not deliver every event");
    if (result.automationPointsApplied != 2)
        throw std::runtime_error ("parameter automation points were not applied");
    if (result.capture.interleaved.size()
         != (std::size_t) captureFrames * result.capture.channels)
        throw std::runtime_error ("rendered sample count is incorrect");

    result.silence = measureRegion (result.capture, 0, chordFrame);
    result.activeChord = measureRegion (
        result.capture, chordFrame, sustainDownFrame);
    result.sustainedChord = measureRegion (
        result.capture, noteOffFrame + 128, sustainUpFrame);
    result.releasedTail = measureRegion (
        result.capture, sustainUpFrame + 128, 110000);
    result.finalSilence.rms = result.finalSilenceSampleCount == 0 ? 0.0
        : result.finalSilenceNonFiniteCount != 0
            ? std::numeric_limits<double>::quiet_NaN()
            : std::sqrt (static_cast<double> (
                finalSilenceSquares
                    / static_cast<long double> (
                        result.finalSilenceSampleCount)));
    return result;
}

void validateScenario (const RenderResult& result)
{
    const auto diagnostics = ChurchDAWTest::diagnose (result.capture);
    if (! diagnostics.isFinite())
        throw std::runtime_error ("capture contains NaN/Inf");
    if (diagnostics.sampleCount != captureFrames * 2)
        throw std::runtime_error ("capture sample-count gate failed");
    if (result.silence.peak > 1.0e-9)
        throw std::runtime_error ("pre-note silence gate failed");
    if (result.activeChord.rms < 1.0e-5)
        throw std::runtime_error ("note-on chord did not produce audio");
    if (result.sustainedChord.rms < 1.0e-5)
        throw std::runtime_error ("sustain did not keep released notes sounding");
    if (result.releasedTail.rms < 1.0e-8)
        throw std::runtime_error ("sustain release/tail did not produce audio");
    const auto finalAssessment = assessFinalSilence (
        result.finalSilence,
        result.finalSilenceNonFiniteCount,
        result.finalSilenceNonZeroBitCount);
    if (result.finalSilenceSampleCount
         != static_cast<std::uint64_t> (sampleRate) * 2)
        throw std::runtime_error (
            "final silence diagnostic did not inspect exactly one stereo second");
    if (! finalAssessment.passed())
    {
        std::ostringstream message;
        message << std::setprecision (12)
                << "stuck voice/effect tail exceeded the final numerical "
                   "silence floor (peak=" << result.finalSilence.peak
                << ", rms=" << result.finalSilence.rms
                << ", peak_limit=" << finalSilencePeakFloor
                << ", rms_limit=" << finalSilenceRmsFloor
                << ", nonfinite=" << result.finalSilenceNonFiniteCount << ')';
        throw std::runtime_error (message.str());
    }
    if (result.finalSilence.rms
         > result.releasedTail.rms * minimumTailDecayRatio)
        throw std::runtime_error (
            "effect tail did not decay at least 80 dB before the final window");
    if (diagnostics.peak > 8.0)
        throw std::runtime_error ("runaway output peak gate failed");
}

void printCaptureDiagnostics (const RenderResult& result)
{
    const auto diagnostics = ChurchDAWTest::diagnose (result.capture);
    const auto finalAssessment = assessFinalSilence (
        result.finalSilence,
        result.finalSilenceNonFiniteCount,
        result.finalSilenceNonZeroBitCount);
    const double tailDecayDb = result.releasedTail.rms > 0.0
        ? amplitudeDbfs (result.finalSilence.rms / result.releasedTail.rms)
        : std::numeric_limits<double>::infinity();
    std::cout << std::setprecision (12)
              << "PAD_SYNTH_CAPTURE scenario=" << result.capture.scenarioVersion
              << " block=" << result.capture.blockSize
              << " frames=" << result.capture.frames
              << " samples=" << diagnostics.sampleCount
              << " finite=" << (diagnostics.isFinite() ? 1 : 0)
              << " nonfinite=" << diagnostics.nonFiniteCount
              << " guards=ok"
              << " peak=" << diagnostics.peak
              << " rms=" << diagnostics.rms
              << " energy=" << static_cast<double> (diagnostics.energy)
              << " fingerprint="
              << ChurchDAWTest::fingerprintHex (diagnostics.fingerprint)
              << " midi_events=" << result.midiEventsDelivered
              << " automation_points=" << result.automationPointsApplied
              << '\n';
    std::cout << "PAD_SYNTH_REGIONS block=" << result.capture.blockSize
              << " silence_peak=" << result.silence.peak
              << " chord_rms=" << result.activeChord.rms
              << " sustain_rms=" << result.sustainedChord.rms
              << " release_rms=" << result.releasedTail.rms
              << " final_silence_peak=" << result.finalSilence.peak
              << " final_silence_peak_dbfs="
              << amplitudeDbfs (result.finalSilence.peak)
              << " final_silence_rms=" << result.finalSilence.rms
              << " final_silence_rms_dbfs="
              << amplitudeDbfs (result.finalSilence.rms)
              << " final_silence_exact_bits="
              << (finalAssessment.exactBitwiseZero ? 1 : 0)
              << " final_silence_floor_pass="
              << (finalAssessment.withinNumericalFloor ? 1 : 0)
              << " final_silence_nonfinite="
              << result.finalSilenceNonFiniteCount
              << " final_silence_nonzero_bits="
              << result.finalSilenceNonZeroBitCount
              << " tail_decay_db=" << tailDecayDb << '\n';
}

void printComparison (const char* label,
                      const AudioCapture& reference,
                      const AudioCapture& candidate,
                      const ChurchDAWTest::AudioComparison& comparison)
{
    std::cout << std::setprecision (12)
              << label
              << " reference_block=" << reference.blockSize
              << " candidate_block=" << candidate.blockSize
              << " samples=" << comparison.sampleCount
              << " compatible=" << (comparison.compatible ? 1 : 0)
              << " finite=" << (comparison.finite ? 1 : 0)
              << " bit_identical=" << (comparison.bitIdentical ? 1 : 0)
              << " max_abs=" << comparison.maxAbsoluteError
              << " rms_null=" << comparison.rmsNull
              << " rms_null_dbfs=" << comparison.rmsNullDbfs;
    if (! comparison.bitIdentical
         && comparison.firstBitMismatch
                != std::numeric_limits<std::uint64_t>::max())
        std::cout << " first_bit_mismatch=" << comparison.firstBitMismatch;
    if (! comparison.error.empty())
        std::cout << " error=\"" << comparison.error << '"';
    std::cout << '\n';
}

void requireBitIdentity (
    const char* context,
    const ChurchDAWTest::AudioComparison& comparison)
{
    if (! comparison.compatible || ! comparison.finite
         || ! comparison.bitIdentical)
    {
        std::ostringstream message;
        message << context << " was not raw-float bit identical";
        if (! comparison.error.empty())
            message << ": " << comparison.error;
        if (comparison.firstBitMismatch
             != std::numeric_limits<std::uint64_t>::max())
            message << " (first mismatch sample "
                    << comparison.firstBitMismatch << ')';
        throw std::runtime_error (message.str());
    }
}

void compareAgainstCanonicalBaseline (
    const std::filesystem::path& baselinePath,
    const std::vector<RenderResult>& candidates)
{
    constexpr char canonicalSha256[] =
        "14091235134b378621bb33a43958f5fa7f0ad40c4fd4f256d4547dda22609c6c";
    AudioCapture baseline;
    std::string error;
    if (! ChurchDAWTest::readCaptureFile (baselinePath, baseline, error))
        throw std::runtime_error ("canonical baseline rejected: " + error);
    constexpr std::uint64_t canonicalFileBytes = UINT64_C (1048624);
    constexpr std::uint64_t canonicalFingerprint = UINT64_C (0xfdb2f8f5388ee9ba);
    std::error_code sizeError;
    const auto baselineBytes = std::filesystem::file_size (baselinePath,
                                                            sizeError);
    if (sizeError || baselineBytes != canonicalFileBytes)
        throw std::runtime_error (
            "canonical baseline byte size does not match the versioned contract");
    const auto actualSha256 = sha256FileHex (baselinePath);
    if (actualSha256 != canonicalSha256)
        throw std::runtime_error (
            "canonical baseline SHA-256 does not match the versioned contract");
    if (baseline.scenarioVersion != scenarioVersion
         || ChurchDAWTest::doubleBits (baseline.sampleRate)
                != ChurchDAWTest::doubleBits (sampleRate)
         || baseline.blockSize != 128 || baseline.channels != 2
         || baseline.frames != captureFrames
         || ChurchDAWTest::stableFingerprint (baseline)
                != canonicalFingerprint)
        throw std::runtime_error (
            "canonical baseline identity does not match the versioned PadSynth v1 contract");

    const auto baselineDiagnostics = ChurchDAWTest::diagnose (baseline);
    if (! baselineDiagnostics.isFinite())
        throw std::runtime_error ("canonical baseline contains NaN/Inf");

    for (const auto& candidate : candidates)
    {
        const auto comparison = ChurchDAWTest::compare (
            baseline, candidate.capture);
        printComparison ("PAD_SYNTH_BASELINE_NULL_DIAGNOSTIC",
                         baseline, candidate.capture, comparison);
        const bool passed = comparison.compatible && comparison.finite
                         && comparison.maxAbsoluteError
                                <= baselineMaxAbsolute
                         && comparison.rmsNull <= baselineRmsNull;
        std::cout << std::setprecision (12)
                  << "PAD_SYNTH_BASELINE_GATE candidate_block="
                  << candidate.capture.blockSize
                  << " max_abs_limit=" << baselineMaxAbsolute
                  << " rms_null_limit=" << baselineRmsNull
                  << " result=" << (passed ? "PASS" : "FAIL") << '\n';
        if (! passed)
            throw std::runtime_error (
                "render exceeded the immutable cross-toolchain baseline "
                "tolerance");
    }
}

int runSelfTest (const std::filesystem::path& captureDirectory,
                 const std::filesystem::path& baselinePath,
                 int timingRepetitions)
{
    static constexpr std::array<int, 3> blockSizes { 128, 256, 512 };
    std::vector<RenderResult> canonical;
    canonical.reserve (blockSizes.size());

    if (! captureDirectory.empty())
    {
        std::error_code error;
        std::filesystem::create_directories (captureDirectory, error);
        if (error)
            throw std::runtime_error ("cannot create capture directory: "
                                      + error.message());
    }

    for (const int blockSize : blockSizes)
    {
        canonical.push_back (renderScenario (blockSize));
        validateScenario (canonical.back());
        printCaptureDiagnostics (canonical.back());

        // Aquece codigo, caches e alocadores. O aquecimento tambem precisa ser
        // bit-identico, mas nunca entra nas estatisticas.
        auto warmup = renderScenario (blockSize);
        validateScenario (warmup);
        requireBitIdentity (
            "warm-up render",
            ChurchDAWTest::compare (
                canonical.back().capture, warmup.capture));

        std::vector<double> callbackMilliseconds;
        std::vector<double> setupMilliseconds;
        callbackMilliseconds.reserve ((std::size_t) timingRepetitions);
        setupMilliseconds.reserve ((std::size_t) timingRepetitions);
        for (int repetition = 0; repetition < timingRepetitions; ++repetition)
        {
            auto repeated = renderScenario (blockSize, true);
            validateScenario (repeated);
            requireBitIdentity (
                "same-block repeated render",
                ChurchDAWTest::compare (
                    canonical.back().capture, repeated.capture));
            callbackMilliseconds.push_back (repeated.callbackMilliseconds);
            setupMilliseconds.push_back (repeated.setupMilliseconds);
        }

        std::sort (callbackMilliseconds.begin(), callbackMilliseconds.end());
        std::sort (setupMilliseconds.begin(), setupMilliseconds.end());
        const double renderedMilliseconds =
            1000.0 * (double) captureFrames / sampleRate;
        const double callbackMedian =
            callbackMilliseconds[callbackMilliseconds.size() / 2];
        const double setupMedian =
            setupMilliseconds[setupMilliseconds.size() / 2];
        std::cout << std::setprecision (8)
                  << "PAD_SYNTH_TIMING block=" << blockSize
                  << " warmups=1"
                  << " repetitions=" << timingRepetitions
                  << " callback_min_ms=" << callbackMilliseconds.front()
                  << " callback_median_ms=" << callbackMedian
                  << " callback_max_ms=" << callbackMilliseconds.back()
                  << " setup_median_ms=" << setupMedian
                  << " median_realtime_x="
                  << renderedMilliseconds / callbackMedian
                  << " threshold_gate=none\n";

        if (! captureDirectory.empty())
        {
            const auto file = captureDirectory
                / ("padsynth-v1-48k-bs" + std::to_string (blockSize)
                   + ".cdpcap");
            std::string error;
            if (! ChurchDAWTest::writeCaptureFile (
                    file, canonical.back().capture, error))
                throw std::runtime_error (error);
            std::cout << "PAD_SYNTH_CAPTURE_WRITTEN path=\""
                      << file.string() << "\"\n";
        }
    }

    for (std::size_t i = 1; i < canonical.size(); ++i)
    {
        const auto comparison = ChurchDAWTest::compare (
            canonical.front().capture, canonical[i].capture);
        printComparison ("PAD_SYNTH_BLOCK_NULL_DIAGNOSTIC",
                         canonical.front().capture,
                         canonical[i].capture,
                         comparison);
        requireBitIdentity ("128/256/512 block render", comparison);
    }

    if (! baselinePath.empty())
        compareAgainstCanonicalBaseline (baselinePath, canonical);
    else
        std::cout << "PAD_SYNTH_BASELINE_GATE result=SKIPPED"
                     " reason=no-baseline-path\n";

    std::cout << "PAD_SYNTH_RENDER_PROBE_OK\n";
    return 0;
}

int runFileComparison (const std::filesystem::path& referencePath,
                       const std::filesystem::path& candidatePath,
                       double maxAbsoluteLimit,
                       double rmsNullLimit)
{
    AudioCapture reference;
    AudioCapture candidate;
    std::string error;
    if (! ChurchDAWTest::readCaptureFile (referencePath, reference, error))
        throw std::runtime_error (error);
    if (! ChurchDAWTest::readCaptureFile (candidatePath, candidate, error))
        throw std::runtime_error (error);

    const auto referenceDiagnostics = ChurchDAWTest::diagnose (reference);
    const auto candidateDiagnostics = ChurchDAWTest::diagnose (candidate);
    std::cout << "PAD_SYNTH_COMPARE_INPUT role=reference path=\""
              << referencePath.string() << "\" block=" << reference.blockSize
              << " samples=" << referenceDiagnostics.sampleCount
              << " finite=" << (referenceDiagnostics.isFinite() ? 1 : 0)
              << " peak=" << std::setprecision (12)
              << referenceDiagnostics.peak
              << " rms=" << referenceDiagnostics.rms
              << " fingerprint="
              << ChurchDAWTest::fingerprintHex (
                     referenceDiagnostics.fingerprint) << '\n';
    std::cout << "PAD_SYNTH_COMPARE_INPUT role=candidate path=\""
              << candidatePath.string() << "\" block=" << candidate.blockSize
              << " samples=" << candidateDiagnostics.sampleCount
              << " finite=" << (candidateDiagnostics.isFinite() ? 1 : 0)
              << " peak=" << candidateDiagnostics.peak
              << " rms=" << candidateDiagnostics.rms
              << " fingerprint="
              << ChurchDAWTest::fingerprintHex (
                     candidateDiagnostics.fingerprint) << '\n';

    const auto comparison = ChurchDAWTest::compare (reference, candidate);
    printComparison ("PAD_SYNTH_CAPTURE_COMPARE", reference, candidate,
                     comparison);
    const bool exactGate = maxAbsoluteLimit == 0.0 && rmsNullLimit == 0.0;
    const bool passed = referenceDiagnostics.isFinite()
                     && candidateDiagnostics.isFinite()
                     && comparison.compatible && comparison.finite
                     && (! exactGate || comparison.bitIdentical)
                     && comparison.maxAbsoluteError <= maxAbsoluteLimit
                     && comparison.rmsNull <= rmsNullLimit;
    std::cout << "PAD_SYNTH_COMPARE_GATE max_abs_limit=" << maxAbsoluteLimit
              << " rms_null_limit=" << rmsNullLimit
              << " exact_bit_gate=" << (exactGate ? 1 : 0)
              << " result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}

int runGenerateBaseline (const std::filesystem::path& outputPath)
{
    if (outputPath.empty())
        throw std::runtime_error ("baseline output path is empty");

    std::error_code fileError;
    const auto parent = outputPath.parent_path();
    if (! parent.empty())
    {
        std::filesystem::create_directories (parent, fileError);
        if (fileError)
            throw std::runtime_error ("cannot create baseline directory: "
                                      + fileError.message());
    }

    static constexpr std::array<int, 3> blockSizes { 128, 256, 512 };
    std::vector<RenderResult> renders;
    renders.reserve (blockSizes.size());
    for (const int blockSize : blockSizes)
    {
        renders.push_back (renderScenario (blockSize));
        validateScenario (renders.back());
    }
    for (std::size_t i = 1; i < renders.size(); ++i)
        requireBitIdentity (
            "baseline-generation cross-block render",
            ChurchDAWTest::compare (
                renders.front().capture, renders[i].capture));

    std::string error;
    if (! ChurchDAWTest::writeCaptureFileExclusive (
            outputPath, renders.front().capture, error))
        throw std::runtime_error (error);

    AudioCapture verified;
    if (! ChurchDAWTest::readCaptureFile (outputPath, verified, error))
        throw std::runtime_error ("generated baseline failed readback: "
                                  + error);
    requireBitIdentity (
        "generated baseline readback",
        ChurchDAWTest::compare (renders.front().capture, verified));

    const auto diagnostics = ChurchDAWTest::diagnose (verified);
    std::cout << "PAD_SYNTH_BASELINE_WRITTEN path=\""
              << outputPath.string() << "\" bytes="
              << std::filesystem::file_size (outputPath)
              << " content_fingerprint="
              << ChurchDAWTest::fingerprintHex (diagnostics.fingerprint)
              << " overwrite_allowed=0\n";
    return 0;
}

enum class Mode
{
    selfTest,
    compare,
    infrastructureSelfTest,
    generateBaseline
};

struct CommandLineOptions
{
    Mode mode = Mode::selfTest;
    bool help = false;
    int timingRepetitions = 5;
    std::filesystem::path captureDirectory;
    std::filesystem::path baselinePath;
    std::filesystem::path referencePath;
    std::filesystem::path candidatePath;
    std::filesystem::path baselineOutputPath;
    double maxAbsoluteLimit = 0.0;
    double rmsNullLimit = 0.0;
};

class UsageError final : public std::runtime_error
{
public:
    explicit UsageError (const std::string& message)
        : std::runtime_error (message)
    {
    }
};

int parseStrictInteger (const std::string& text, const char* option)
{
    int value = 0;
    const auto parsed = std::from_chars (
        text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc()
         || parsed.ptr != text.data() + text.size())
        throw UsageError (std::string (option)
                          + " requires a strict integer");
    return value;
}

double parseStrictDouble (const std::string& text, const char* option)
{
    if (text.empty()
         || std::any_of (text.begin(), text.end(), [] (unsigned char c)
            {
                return std::isspace (c) != 0;
            }))
        throw UsageError (std::string (option)
                          + " requires a strict decimal number");

    std::size_t consumed = 0;
    double value = 0.0;
    try
    {
        value = std::stod (text, &consumed);
    }
    catch (const std::exception&)
    {
        throw UsageError (std::string (option)
                          + " requires a strict decimal number");
    }
    if (consumed != text.size() || ! std::isfinite (value))
        throw UsageError (std::string (option)
                          + " requires a finite strict decimal number");
    return value;
}

CommandLineOptions parseCommandLine (const std::vector<std::string>& arguments)
{
    CommandLineOptions options;
    std::optional<Mode> selectedMode;
    bool repetitionsSeen = false;
    bool captureDirectorySeen = false;
    bool baselineSeen = false;
    bool maxAbsoluteSeen = false;
    bool rmsNullSeen = false;

    if (arguments.size() == 1
         && (arguments.front() == "--help" || arguments.front() == "-h"))
    {
        options.help = true;
        return options;
    }

    const auto chooseMode = [&selectedMode] (Mode mode, const char* option)
    {
        if (selectedMode.has_value())
            throw UsageError (std::string (option)
                              + " conflicts with another mode");
        selectedMode = mode;
    };

    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        const auto& argument = arguments[i];
        const auto takeValue = [&] (const char* option) -> const std::string&
        {
            if (i + 1 >= arguments.size())
                throw UsageError (std::string (option)
                                  + " is missing its value");
            return arguments[++i];
        };

        if (argument == "--help" || argument == "-h")
            throw UsageError ("--help must be used by itself");
        if (argument == "--self-test")
        {
            chooseMode (Mode::selfTest, "--self-test");
            continue;
        }
        if (argument == "--compare")
        {
            chooseMode (Mode::compare, "--compare");
            options.referencePath = takeValue ("--compare");
            options.candidatePath = takeValue ("--compare");
            continue;
        }
        if (argument == "--infrastructure-self-test")
        {
            chooseMode (Mode::infrastructureSelfTest,
                        "--infrastructure-self-test");
            continue;
        }
        if (argument == "--generate-baseline")
        {
            chooseMode (Mode::generateBaseline, "--generate-baseline");
            options.baselineOutputPath = takeValue ("--generate-baseline");
            continue;
        }
        if (argument == "--capture-dir")
        {
            if (captureDirectorySeen)
                throw UsageError ("--capture-dir was specified twice");
            captureDirectorySeen = true;
            options.captureDirectory = takeValue ("--capture-dir");
            continue;
        }
        if (argument == "--baseline")
        {
            if (baselineSeen)
                throw UsageError ("--baseline was specified twice");
            baselineSeen = true;
            options.baselinePath = takeValue ("--baseline");
            continue;
        }
        if (argument == "--repetitions")
        {
            if (repetitionsSeen)
                throw UsageError ("--repetitions was specified twice");
            repetitionsSeen = true;
            options.timingRepetitions = parseStrictInteger (
                takeValue ("--repetitions"), "--repetitions");
            continue;
        }
        if (argument == "--max-abs")
        {
            if (maxAbsoluteSeen)
                throw UsageError ("--max-abs was specified twice");
            maxAbsoluteSeen = true;
            options.maxAbsoluteLimit = parseStrictDouble (
                takeValue ("--max-abs"), "--max-abs");
            continue;
        }
        if (argument == "--rms-null")
        {
            if (rmsNullSeen)
                throw UsageError ("--rms-null was specified twice");
            rmsNullSeen = true;
            options.rmsNullLimit = parseStrictDouble (
                takeValue ("--rms-null"), "--rms-null");
            continue;
        }
        throw UsageError ("unknown argument: " + argument);
    }

    options.mode = selectedMode.value_or (Mode::selfTest);
    if (options.timingRepetitions < 3 || options.timingRepetitions > 99
         || options.timingRepetitions % 2 == 0)
        throw UsageError (
            "--repetitions must be an odd integer between 3 and 99");
    if (options.maxAbsoluteLimit < 0.0 || options.rmsNullLimit < 0.0)
        throw UsageError ("comparison limits must be >= 0");

    if (options.mode == Mode::selfTest)
    {
        if (maxAbsoluteSeen || rmsNullSeen)
            throw UsageError (
                "--max-abs/--rms-null require --compare");
    }
    else if (options.mode == Mode::compare)
    {
        if (repetitionsSeen || captureDirectorySeen || baselineSeen)
            throw UsageError (
                "self-test options cannot be combined with --compare");
        if (options.referencePath.empty() || options.candidatePath.empty())
            throw UsageError ("--compare requires two non-empty paths");
    }
    else
    {
        if (repetitionsSeen || captureDirectorySeen || baselineSeen
             || maxAbsoluteSeen || rmsNullSeen)
            throw UsageError (
                "the selected maintenance mode accepts no extra options");
        if (options.mode == Mode::generateBaseline
             && options.baselineOutputPath.empty())
            throw UsageError (
                "--generate-baseline requires a non-empty path");
    }

    return options;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto seed = static_cast<unsigned long long> (
            Clock::now().time_since_epoch().count());
        for (unsigned int attempt = 0; attempt < 100; ++attempt)
        {
            path = std::filesystem::temp_directory_path()
                 / ("churchdaw-capture-negative-" + std::to_string (seed)
                    + "-" + std::to_string (attempt));
            std::error_code error;
            if (std::filesystem::create_directory (path, error))
                return;
            if (error && error != std::errc::file_exists)
                throw std::runtime_error (
                    "cannot create capture test directory: "
                    + error.message());
        }
        throw std::runtime_error (
            "cannot allocate a unique capture test directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all (path, ignored);
    }

    std::filesystem::path path;
};

std::vector<std::uint8_t> readBytes (const std::filesystem::path& path)
{
    constexpr std::uint64_t maxFixtureBytes =
        ChurchDAWTest::CaptureFileDetail::maxPayloadBytes
        + ChurchDAWTest::CaptureFileDetail::headerSize;
    std::ifstream input (path, std::ios::binary | std::ios::ate);
    if (! input)
        throw std::runtime_error ("cannot open fixture bytes");
    const auto end = input.tellg();
    if (end < 0)
        throw std::runtime_error ("cannot measure fixture bytes");
    const auto size = static_cast<std::uint64_t> (end);
    if (size > maxFixtureBytes)
        throw std::runtime_error ("fixture bytes exceed safety limit");
    if (size > static_cast<std::uint64_t> (std::numeric_limits<std::size_t>::max())
         || size > static_cast<std::uint64_t> (
                        std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error ("fixture byte length cannot be represented");
    std::vector<std::uint8_t> bytes (static_cast<std::size_t> (size));
    input.seekg (0);
    if (! bytes.empty())
        input.read (reinterpret_cast<char*> (bytes.data()),
                    static_cast<std::streamsize> (bytes.size()));
    if (! input)
        throw std::runtime_error ("cannot read fixture bytes");
    return bytes;
}

void writeBytes (const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output (path, std::ios::binary | std::ios::trunc);
    if (! output)
        throw std::runtime_error ("cannot create mutated capture fixture");
    if (! bytes.empty())
        output.write (reinterpret_cast<const char*> (bytes.data()),
                      static_cast<std::streamsize> (bytes.size()));
    if (! output)
        throw std::runtime_error ("cannot write mutated capture fixture");
}

bool verifySha256 (const std::filesystem::path& path,
                   const std::string& expected,
                   std::string& actual)
{
    actual = sha256FileHex (path);
    return actual == expected;
}

void setU32LittleEndian (std::vector<std::uint8_t>& bytes,
                         std::uint64_t offset, std::uint32_t value)
{
    if (offset > bytes.size() || bytes.size() - (std::size_t) offset < 4)
        throw std::runtime_error ("test mutation exceeded fixture bounds");
    for (unsigned int i = 0; i < 4; ++i)
        bytes[(std::size_t) offset + i] =
            static_cast<std::uint8_t> (value >> (8 * i));
}

void setU64LittleEndian (std::vector<std::uint8_t>& bytes,
                         std::uint64_t offset, std::uint64_t value)
{
    if (offset > bytes.size() || bytes.size() - (std::size_t) offset < 8)
        throw std::runtime_error ("test mutation exceeded fixture bounds");
    for (unsigned int i = 0; i < 8; ++i)
        bytes[(std::size_t) offset + i] =
            static_cast<std::uint8_t> (value >> (8 * i));
}

void requireTest (bool condition, const std::string& message, int& passed)
{
    if (! condition)
        throw std::runtime_error ("infrastructure test failed: " + message);
    ++passed;
}

void expectReadFailure (const TemporaryDirectory& temporary,
                        const std::string& name,
                        const std::vector<std::uint8_t>& bytes,
                        const std::string& expectedError,
                        int& passed)
{
    const auto file = temporary.path / (name + ".cdpcap");
    writeBytes (file, bytes);
    AudioCapture capture;
    std::string error;
    const bool accepted = ChurchDAWTest::readCaptureFile (
        file, capture, error);
    requireTest (! accepted, name + " was accepted", passed);
    requireTest (error.find (expectedError) != std::string::npos,
                 name + " returned unexpected error: " + error, passed);
}

void expectUsageFailure (std::vector<std::string> arguments,
                         const std::string& name, int& passed)
{
    try
    {
        (void) parseCommandLine (arguments);
    }
    catch (const UsageError&)
    {
        ++passed;
        return;
    }
    throw std::runtime_error ("CLI negative test was accepted: " + name);
}

int runInfrastructureSelfTest()
{
    using namespace ChurchDAWTest::CaptureFileDetail;
    TemporaryDirectory temporary;
    int passed = 0;

    GuardedStereoBlock guardedProbe (16);
    std::string detectorError;
    requireTest (! guardedProbe.activeSamplesFinite (detectorError)
                     && detectorError.find ("unwritten") != std::string::npos,
                 "NaN/unwritten-sample negative path was not detected",
                 passed);
    guardedProbe.audio().clear();
    requireTest (guardedProbe.activeSamplesFinite (detectorError),
                 "cleared active samples were not accepted", passed);
    guardedProbe.audio().getWritePointer (0)[-1] = 0.0f;
    requireTest (! guardedProbe.guardsIntact (detectorError)
                     && detectorError.find ("underrun") != std::string::npos,
                 "red-zone fault injection was not detected", passed);

    const auto exactSilence = assessFinalSilence ({ 0.0, 0.0 }, 0, 0);
    requireTest (exactSilence.passed()
                     && exactSilence.exactBitwiseZero
                     && exactSilence.withinNumericalFloor,
                 "exact final silence was not accepted", passed);
    const auto knownX86Residue = assessFinalSilence (
        { 1.82454400033e-7, 3.76726964461e-8 }, 0, 1);
    requireTest (knownX86Residue.passed()
                     && ! knownX86Residue.exactBitwiseZero
                     && knownX86Residue.withinNumericalFloor,
                 "known x86 numerical tail was not accepted", passed);
    requireTest (! assessFinalSilence (
                      { finalSilencePeakFloor * 1.01,
                        finalSilenceRmsFloor * 0.5 }, 0, 1).passed(),
                 "peak above final-silence floor was accepted", passed);
    requireTest (! assessFinalSilence (
                      { finalSilencePeakFloor * 0.5,
                        finalSilenceRmsFloor * 1.01 }, 0, 1).passed(),
                 "RMS above final-silence floor was accepted", passed);
    requireTest (! assessFinalSilence (
                      { 0.0, std::numeric_limits<double>::quiet_NaN() },
                      1, 1).passed(),
                 "non-finite final silence was accepted", passed);
    const auto signedZeroSilence = assessFinalSilence ({ 0.0, 0.0 }, 0, 1);
    requireTest (signedZeroSilence.passed()
                     && ! signedZeroSilence.exactBitwiseZero,
                 "non-canonical zero was reported as bitwise exact", passed);

    AudioCapture original;
    original.scenarioVersion = 7;
    original.sampleRate = 48000.0;
    original.blockSize = 128;
    original.channels = 2;
    original.frames = 4;
    original.interleaved = {
        0.0f, -0.0f, 0.25f, -0.25f,
        0.5f, -0.5f, 1.0f, -1.0f
    };

    const auto canonicalFile = temporary.path / "round-trip.cdpcap";
    std::string error;
    requireTest (ChurchDAWTest::writeCaptureFile (
                     canonicalFile, original, error),
                 "serialization write: " + error, passed);
    AudioCapture roundTrip;
    requireTest (ChurchDAWTest::readCaptureFile (
                     canonicalFile, roundTrip, error),
                 "serialization read: " + error, passed);
    const auto roundTripComparison = ChurchDAWTest::compare (
        original, roundTrip);
    requireTest (roundTripComparison.compatible
                     && roundTripComparison.finite
                     && roundTripComparison.bitIdentical,
                 "serialization did not preserve raw float bits", passed);

    const auto beforeExclusiveBytes = readBytes (canonicalFile);
    const auto beforeExclusiveTime =
        std::filesystem::last_write_time (canonicalFile);
    error.clear();
    requireTest (! ChurchDAWTest::writeCaptureFileExclusive (
                     canonicalFile, original, error),
                 "exclusive writer accepted a second creation", passed);
    requireTest (readBytes (canonicalFile) == beforeExclusiveBytes
                     && std::filesystem::last_write_time (canonicalFile)
                            == beforeExclusiveTime,
                 "exclusive writer changed existing bytes or mtime", passed);

    auto nonFiniteCapture = original;
    nonFiniteCapture.interleaved[0] =
        std::numeric_limits<float>::quiet_NaN();
    error.clear();
    requireTest (! ChurchDAWTest::writeCaptureFile (
                     temporary.path / "nonfinite-write.cdpcap",
                     nonFiniteCapture, error)
                     && error.find ("NaN or Inf") != std::string::npos,
                 "writer accepted a non-finite sample", passed);

    AudioCapture positiveZero = original;
    AudioCapture negativeZero = original;
    positiveZero.frames = negativeZero.frames = 1;
    positiveZero.channels = negativeZero.channels = 1;
    positiveZero.interleaved = { 0.0f };
    negativeZero.interleaved = { -0.0f };
    const auto signedZeroComparison = ChurchDAWTest::compare (
        positiveZero, negativeZero);
    requireTest (signedZeroComparison.compatible
                     && signedZeroComparison.finite
                     && ! signedZeroComparison.bitIdentical
                     && signedZeroComparison.firstBitMismatch == 0
                     && signedZeroComparison.maxAbsoluteError == 0.0
                     && signedZeroComparison.rmsNull == 0.0,
                 "+0/-0 were not distinguished at the bit gate", passed);

    const auto validBytes = readBytes (canonicalFile);
    requireTest (validBytes.size() == headerSize
                                      + original.interleaved.size()
                                            * sizeof (float),
                 "serialized file size is wrong", passed);

    const auto validSha256 = sha256FileHex (canonicalFile);
    std::string observedSha256;
    requireTest (verifySha256 (canonicalFile, validSha256, observedSha256)
                     && observedSha256 == validSha256,
                 "SHA-256 verifier rejected its valid fixture", passed);
   #if defined (_WIN32)
    const auto unicodeShaFile = temporary.path
        / std::filesystem::path (L"sha-\u00e1udio-\u57fa\u6e96.cdpcap");
   #else
    const auto unicodeShaFile = temporary.path
        / std::filesystem::path ("sha-\xc3\xa1" "udio-\xe5\x9f\xba\xe6\xba\x96.cdpcap");
   #endif
    std::filesystem::copy_file (canonicalFile, unicodeShaFile);
    requireTest (verifySha256 (unicodeShaFile, validSha256, observedSha256)
                     && observedSha256 == validSha256,
                 "SHA-256 verifier rejected a Unicode path", passed);
    auto shaMutated = validBytes;
    shaMutated.back() ^= 0x01u;
    const auto shaMutatedFile = temporary.path / "sha-mutated.cdpcap";
    writeBytes (shaMutatedFile, shaMutated);
    requireTest (! verifySha256 (shaMutatedFile, validSha256, observedSha256)
                     && observedSha256 != validSha256,
                 "SHA-256 verifier accepted an adulterated fixture", passed);

    const auto concurrentFile = temporary.path / "concurrent.cdpcap";
    constexpr std::size_t concurrentAttempts = 8;
    std::mutex startMutex;
    std::condition_variable startCondition;
    std::size_t ready = 0;
    bool start = false;
    std::vector<unsigned char> concurrentWinners (concurrentAttempts, 0);
    std::vector<std::string> concurrentErrors (concurrentAttempts);
    std::vector<std::thread> writers;
    writers.reserve (concurrentAttempts);
    for (std::size_t i = 0; i < concurrentAttempts; ++i)
    {
        writers.emplace_back ([&, i]
        {
            {
                std::unique_lock<std::mutex> lock (startMutex);
                ++ready;
                startCondition.notify_all();
                startCondition.wait (lock, [&] { return start; });
            }
            concurrentWinners[i] = ChurchDAWTest::writeCaptureFileExclusive (
                                        concurrentFile, original,
                                        concurrentErrors[i]) ? 1 : 0;
        });
    }
    {
        std::unique_lock<std::mutex> lock (startMutex);
        startCondition.wait (lock, [&] { return ready == concurrentAttempts; });
        start = true;
    }
    startCondition.notify_all();
    for (auto& writer : writers)
        writer.join();
    const auto winnerCount = static_cast<std::size_t> (
        std::count (concurrentWinners.begin(), concurrentWinners.end(), true));
    requireTest (winnerCount == 1,
                 "concurrent exclusive creation did not have exactly one winner",
                 passed);
    requireTest (readBytes (concurrentFile) == validBytes,
                 "concurrent loser changed final capture bytes", passed);
    AudioCapture concurrentCapture;
    error.clear();
    requireTest (ChurchDAWTest::readCaptureFile (
                     concurrentFile, concurrentCapture, error),
                 "concurrent winner produced unreadable capture: " + error,
                 passed);
    requireTest (ChurchDAWTest::compare (original, concurrentCapture)
                     .bitIdentical,
                 "concurrent winner produced altered samples", passed);

    auto mutated = validBytes;
    mutated[0] ^= 0x01u;
    expectReadFailure (temporary, "corrupt-magic", mutated,
                       "invalid capture magic", passed);

    mutated = validBytes;
    setU32LittleEndian (mutated, formatOffset, formatVersion + 1);
    expectReadFailure (temporary, "corrupt-version", mutated,
                       "unsupported capture format version", passed);

    mutated = validBytes;
    setU32LittleEndian (mutated, blockSizeOffset, 256);
    expectReadFailure (temporary, "tampered-valid-header", mutated,
                       "integrity mismatch", passed);

    mutated = validBytes;
    setU32LittleEndian (mutated, blockSizeOffset, 0);
    expectReadFailure (temporary, "zero-block-size", mutated,
                       "block size", passed);

    mutated = validBytes;
    setU64LittleEndian (
        mutated, sampleRateOffset,
        ChurchDAWTest::doubleBits (
            std::numeric_limits<double>::quiet_NaN()));
    expectReadFailure (temporary, "nonfinite-sample-rate", mutated,
                       "sample rate", passed);

    mutated = validBytes;
    mutated[(std::size_t) integrityOffset] ^= 0x80u;
    expectReadFailure (temporary, "corrupt-integrity", mutated,
                       "integrity mismatch", passed);

    mutated = validBytes;
    mutated[(std::size_t) headerSize] ^= 0x01u;
    expectReadFailure (temporary, "corrupt-audio", mutated,
                       "integrity mismatch", passed);

    mutated = validBytes;
    setU32LittleEndian (mutated, headerSize, UINT32_C (0x7fc00000));
    expectReadFailure (temporary, "nonfinite-audio", mutated,
                       "NaN or Inf", passed);

    mutated = validBytes;
    mutated.resize ((std::size_t) headerSize - 1);
    expectReadFailure (temporary, "truncated-header", mutated,
                       "truncated capture header", passed);

    mutated = validBytes;
    mutated.pop_back();
    expectReadFailure (temporary, "truncated-payload", mutated,
                       "truncated capture sample data", passed);

    mutated = validBytes;
    mutated.push_back (0x55u);
    expectReadFailure (temporary, "trailing-data", mutated,
                       "unexpected trailing data", passed);

    mutated = validBytes;
    setU64LittleEndian (mutated, framesOffset,
                        std::numeric_limits<std::uint64_t>::max());
    expectReadFailure (temporary, "malicious-huge-dimensions", mutated,
                       "overflow", passed);

    const auto oversizedFixture = temporary.path / "oversized-fixture.bin";
    writeBytes (oversizedFixture, {});
    std::filesystem::resize_file (
        oversizedFixture, maxPayloadBytes + headerSize + 1);
    bool oversizedRejected = false;
    try
    {
        (void) readBytes (oversizedFixture);
    }
    catch (const std::runtime_error& oversizedError)
    {
        oversizedRejected = std::string (oversizedError.what()).find (
            "safety limit") != std::string::npos;
    }
    requireTest (oversizedRejected,
                 "readBytes allocated or accepted an oversized fixture", passed);

    expectUsageFailure ({ "--repetitions", "3junk" },
                        "integer trailing junk", passed);
    expectUsageFailure ({ "--repetitions" },
                        "missing integer", passed);
    expectUsageFailure ({ "--repetitions", "4" },
                        "even timing sample count", passed);
    expectUsageFailure ({ "--compare", "only-one" },
                        "missing comparison path", passed);
    expectUsageFailure ({ "--max-abs", "1" },
                        "comparison option in self-test", passed);
    expectUsageFailure (
        { "--compare", "a", "b", "--capture-dir", "x" },
        "self-test option in compare mode", passed);
    expectUsageFailure (
        { "--self-test", "--compare", "a", "b" },
        "conflicting modes", passed);
    expectUsageFailure (
        { "--compare", "a", "b", "--rms-null", "1junk" },
        "double trailing junk", passed);
    expectUsageFailure (
        { "--compare", "a", "b", "--max-abs", "nan" },
        "nonfinite comparison limit", passed);
    expectUsageFailure (
        { "--infrastructure-self-test", "--repetitions", "5" },
        "maintenance mode options", passed);
    expectUsageFailure ({ "--help", "--self-test" },
                        "help mixed with mode", passed);

    const auto validOptions = parseCommandLine (
        { "--self-test", "--repetitions", "5",
          "--baseline", "baseline.cdpcap" });
    requireTest (validOptions.mode == Mode::selfTest
                     && validOptions.timingRepetitions == 5
                     && validOptions.baselinePath == "baseline.cdpcap",
                 "valid CLI was parsed incorrectly", passed);

    std::cout << "PAD_SYNTH_INFRASTRUCTURE_TESTS_OK assertions="
              << passed << " payload_cap_bytes=" << maxPayloadBytes << '\n';
    return 0;
}

void printUsage (std::ostream& output, const char* executable)
{
    output
        << "Usage:\n"
        << "  " << executable
        << " [--self-test] [--repetitions ODD_N] [--capture-dir DIR]"
           " [--baseline FILE]\n"
        << "  " << executable
        << " --compare REFERENCE CANDIDATE"
           " [--max-abs VALUE] [--rms-null VALUE]\n"
        << "  " << executable << " --infrastructure-self-test\n"
        << "  " << executable << " --generate-baseline FILE\n\n"
        << "Zero comparison limits enforce raw float-bit identity.\n"
        << "The canonical baseline is read-only during tests and uses fixed "
           "cross-toolchain limits of max_abs=3e-5 and rms_null=2e-6.\n";
}
}

int main (int argc, char* argv[])
{
    try
    {
        std::vector<std::string> arguments;
        arguments.reserve (argc > 1 ? (std::size_t) argc - 1 : 0);
        for (int i = 1; i < argc; ++i)
            arguments.emplace_back (argv[i]);

        const auto options = parseCommandLine (arguments);
        if (options.help)
        {
            printUsage (std::cout, argv[0]);
            return 0;
        }

        switch (options.mode)
        {
            case Mode::selfTest:
                return runSelfTest (
                    options.captureDirectory, options.baselinePath,
                    options.timingRepetitions);
            case Mode::compare:
                return runFileComparison (
                    options.referencePath, options.candidatePath,
                    options.maxAbsoluteLimit, options.rmsNullLimit);
            case Mode::infrastructureSelfTest:
                return runInfrastructureSelfTest();
            case Mode::generateBaseline:
                return runGenerateBaseline (options.baselineOutputPath);
        }
        throw std::runtime_error ("unreachable command mode");
    }
    catch (const UsageError& error)
    {
        std::cerr << "PAD_SYNTH_RENDER_PROBE_USAGE_ERROR: "
                  << error.what() << '\n';
        printUsage (std::cerr, argv[0]);
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "PAD_SYNTH_RENDER_PROBE_FAIL: " << error.what() << '\n';
        return 1;
    }
}
