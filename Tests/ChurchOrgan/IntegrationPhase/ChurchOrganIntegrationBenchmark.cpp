#include "ChurchOrganLayerProcessor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>

namespace
{
    using churchorgan::ChurchOrganFamily;
    using churchorgan::ChurchOrganLayerPrepareSpec;
    using churchorgan::ChurchOrganLayerProcessor;
    using churchorgan::ChurchOrganLayerQuality;

    constexpr double benchmarkSampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int secondsPerFamily = 3;
    constexpr int blocksPerFamily =
        static_cast<int> (benchmarkSampleRate)
        * secondsPerFamily / blockSize;

    const char* familyName (ChurchOrganFamily family)
    {
        switch (family)
        {
            case ChurchOrganFamily::tonewheel:
                return "Tonewheel";
            case ChurchOrganFamily::transistor:
                return "Transistor";
            case ChurchOrganFamily::pipe:
                return "Pipe";
            case ChurchOrganFamily::worshipHybrid:
                return "WorshipHybrid";
        }
        return "Invalid";
    }

    void addBenchmarkRegistration (
        juce::MidiBuffer& midi,
        ChurchOrganFamily family)
    {
        constexpr std::array<int, 8> upper {{
            48, 52, 55, 60, 64, 67, 72, 76
        }};
        for (const auto note : upper)
        {
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    1, note, static_cast<juce::uint8> (108)),
                0);
        }
        if (family != ChurchOrganFamily::transistor)
        {
            for (const auto note :
                 std::array<int, 4> {{ 43, 48, 52, 55 }})
            {
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        2, note, static_cast<juce::uint8> (100)),
                    0);
            }
            for (const auto note :
                 std::array<int, 2> {{ 31, 36 }})
            {
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        3, note, static_cast<juce::uint8> (116)),
                    0);
            }
        }
    }

    struct Result
    {
        double cpuPercentOfOneCore = 0.0;
        double realtimeFactor = 0.0;
        double rms = 0.0;
        float peak = 0.0f;
        bool finite = true;
        bool rendered = true;
    };

    Result benchmarkFamily (ChurchOrganFamily family)
    {
        auto processor =
            std::make_unique<ChurchOrganLayerProcessor>();
        ChurchOrganLayerPrepareSpec spec;
        spec.sampleRate = benchmarkSampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = 0x42454e43484d4152ULL;
        spec.quality = ChurchOrganLayerQuality::standard;
        spec.initialFamily = family;

        Result result;
        if (! processor->prepare (spec))
        {
            result.rendered = false;
            return result;
        }

        juce::AudioBuffer<float> audio (2, blockSize);
        juce::AudioBuffer<float> send (2, blockSize);
        juce::MidiBuffer noteOn;
        juce::MidiBuffer empty;
        addBenchmarkRegistration (noteOn, family);

        // Warm instruction/data caches before timing.
        if (! processor->processBlock (
                audio, noteOn, &send))
        {
            result.rendered = false;
            return result;
        }
        for (int block = 0; block < 8; ++block)
        {
            if (! processor->processBlock (
                    audio, empty, &send))
            {
                result.rendered = false;
                return result;
            }
        }

        double squareSum = 0.0;
        std::uint64_t sampleCount = 0;
        const auto started =
            std::chrono::steady_clock::now();
        for (int block = 0;
             block < blocksPerFamily;
             ++block)
        {
            if (! processor->processBlock (
                    audio, empty, &send))
            {
                result.rendered = false;
                break;
            }
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto* data = audio.getReadPointer (channel);
                for (int sample = 0;
                     sample < blockSize;
                     ++sample)
                {
                    const auto value = data[sample];
                    result.finite =
                        result.finite && std::isfinite (value);
                    if (std::isfinite (value))
                    {
                        result.peak = std::max (
                            result.peak, std::abs (value));
                        squareSum +=
                            static_cast<double> (value)
                            * static_cast<double> (value);
                    }
                    ++sampleCount;
                }
            }
        }
        const auto elapsed =
            std::chrono::duration<double> (
                std::chrono::steady_clock::now() - started)
                .count();
        const auto renderedSeconds =
            static_cast<double> (
                blocksPerFamily * blockSize)
                / benchmarkSampleRate;
        result.cpuPercentOfOneCore =
            elapsed / renderedSeconds * 100.0;
        result.realtimeFactor =
            elapsed > 0.0 ? renderedSeconds / elapsed : 0.0;
        result.rms =
            sampleCount != 0
                ? std::sqrt (
                    squareSum
                    / static_cast<double> (sampleCount))
                : 0.0;
        return result;
    }
}

int main()
{
    const std::array<ChurchOrganFamily, 4> families {{
        ChurchOrganFamily::tonewheel,
        ChurchOrganFamily::transistor,
        ChurchOrganFamily::pipe,
        ChurchOrganFamily::worshipHybrid
    }};

    std::cout
        << "Church Organ adapter benchmark: 48 kHz, 128 samples, "
        << secondsPerFamily << " s/family, standard quality\n"
        << "Family             one-core CPU    realtime x"
        << "         RMS        peak\n";

    bool success = true;
    for (const auto family : families)
    {
        const auto result = benchmarkFamily (family);
        success = success
            && result.rendered
            && result.finite
            && result.realtimeFactor > 0.0
            && std::isfinite (result.cpuPercentOfOneCore)
            && std::isfinite (result.rms)
            && std::isfinite (result.peak);
        std::cout
            << std::left << std::setw (18) << familyName (family)
            << std::right << std::fixed << std::setprecision (2)
            << std::setw (10) << result.cpuPercentOfOneCore << "%"
            << std::setw (14) << result.realtimeFactor << "x"
            << std::setprecision (7)
            << std::setw (13) << result.rms
            << std::setw (12) << result.peak
            << (result.rendered && result.finite
                    ? ""
                    : "  INVALID")
            << '\n';
    }

    return success ? 0 : 1;
}
