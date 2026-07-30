#include "OrganAmpCab.h"
#include "RotaryCabinet.h"
#include "ScannerVibrato.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 256;
    constexpr double durationSeconds = 30.0;
    constexpr float twoPi =
        6.283185307179586476925286766559f;

    struct Result
    {
        double elapsedSeconds = 0.0;
        double oneCorePercent = 0.0;
        double realtimeFactor = 0.0;
        float peak = 0.0f;
        bool finite = true;
    };

    Result runChain (
        churchorgan::OrganAmpOversampling quality,
        churchorgan::RotarySpeedMode speed)
    {
        churchorgan::ScannerVibrato scanner;
        churchorgan::ScannerParameters scannerParameters;
        scannerParameters.mode =
            churchorgan::ScannerMode::chorus2;
        scannerParameters.character =
            churchorgan::ScannerCharacter::smooth;
        scannerParameters.stereoSpread = 0.22f;
        scannerParameters.worshipIntensity = 0.55f;
        scanner.setParameters (scannerParameters);
        if (! scanner.prepare (
                { sampleRate, blockSize, 0x51U, 0.13f }))
        {
            return {};
        }

        churchorgan::OrganAmpCab amp;
        churchorgan::OrganAmpParameters ampParameters;
        ampParameters.profile =
            churchorgan::OrganAmpProfile::worshipSmooth;
        ampParameters.drive = 0.42f;
        ampParameters.worshipIntensity = 0.62f;
        amp.setParameters (ampParameters);
        if (! amp.prepare ({ sampleRate, blockSize, quality }))
            return {};

        churchorgan::RotaryCabinet rotary;
        churchorgan::RotaryParameters rotaryParameters;
        rotaryParameters.speedMode = speed;
        rotaryParameters.perspective =
            churchorgan::RotaryPerspective::wideWorship;
        rotaryParameters.worshipIntensity = 0.72f;
        rotary.setParameters (rotaryParameters);
        if (! rotary.prepare (
                { sampleRate, blockSize, 0.19f, 0.68f }))
        {
            return {};
        }

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        const auto totalSamples =
            static_cast<std::size_t> (
                durationSeconds * sampleRate);
        auto rendered = std::size_t { 0 };
        auto phase = 0.0f;
        auto peak = 0.0f;
        auto finite = true;

        const auto start = std::chrono::steady_clock::now();
        while (rendered < totalSamples)
        {
            const auto count =
                std::min (blockSize, totalSamples - rendered);
            for (std::size_t sample = 0;
                 sample < count;
                 ++sample)
            {
                const auto fundamental = std::sin (phase);
                const auto harmonic =
                    0.32f * std::sin (2.0f * phase)
                    + 0.17f * std::sin (3.0f * phase)
                    + 0.08f * std::sin (5.0f * phase);
                left[sample] =
                    0.24f * (fundamental + harmonic);
                right[sample] = left[sample];
                phase +=
                    twoPi * 146.83f
                    / static_cast<float> (sampleRate);
                if (phase >= twoPi)
                    phase -= twoPi;
            }

            scanner.process (left.data(), right.data(), count);
            amp.process (left.data(), right.data(), count);
            rotary.process (left.data(), right.data(), count);

            for (std::size_t sample = 0;
                 sample < count;
                 ++sample)
            {
                finite =
                    finite
                    && std::isfinite (left[sample])
                    && std::isfinite (right[sample]);
                peak = std::max (
                    peak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
            }
            rendered += count;
        }
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration<double> (end - start).count();

        return {
            elapsed,
            100.0 * elapsed / durationSeconds,
            durationSeconds / elapsed,
            peak,
            finite
        };
    }

    void printResult (
        const char* label,
        const Result& result)
    {
        std::cout
            << label
            << " elapsed_s=" << result.elapsedSeconds
            << " one_core_percent=" << result.oneCorePercent
            << " realtime_x=" << result.realtimeFactor
            << " peak=" << result.peak
            << " finite=" << (result.finite ? 1 : 0)
            << '\n';
    }
}

int main()
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout
        << "fixed_memory_bytes scanner="
        << sizeof (churchorgan::ScannerVibrato)
        << " amp=" << sizeof (churchorgan::OrganAmpCab)
        << " rotary=" << sizeof (churchorgan::RotaryCabinet)
        << " chain="
        << (sizeof (churchorgan::ScannerVibrato)
            + sizeof (churchorgan::OrganAmpCab)
            + sizeof (churchorgan::RotaryCabinet))
        << '\n';

    const auto mobile = runChain (
        churchorgan::OrganAmpOversampling::x2,
        churchorgan::RotarySpeedMode::slow);
    const auto desktop = runChain (
        churchorgan::OrganAmpOversampling::x4,
        churchorgan::RotarySpeedMode::fast);
    const auto ultra = runChain (
        churchorgan::OrganAmpOversampling::x8,
        churchorgan::RotarySpeedMode::fast);

    printResult ("mobile_x2_slow", mobile);
    printResult ("desktop_x4_fast", desktop);
    printResult ("ultra_x8_fast", ultra);

    const auto passed =
        mobile.finite
        && desktop.finite
        && ultra.finite
        && mobile.realtimeFactor > 1.0
        && desktop.realtimeFactor > 1.0
        && ultra.realtimeFactor > 1.0
        && mobile.peak < 1.0f
        && desktop.peak < 1.0f
        && ultra.peak < 1.0f;

    std::cout
        << (passed
                ? "CHURCH_ORGAN_MOTION_BENCHMARK_OK"
                : "CHURCH_ORGAN_MOTION_BENCHMARK_FAILED")
        << '\n';
    return passed ? 0 : 1;
}
