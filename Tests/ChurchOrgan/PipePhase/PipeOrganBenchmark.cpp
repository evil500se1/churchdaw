#include "PipeOrganEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;

    struct Result
    {
        double audioSeconds = 0.0;
        double wallSeconds = 0.0;
        double cpuSeconds = 0.0;
        double oneCorePercent = 0.0;
        double wallRealtime = 0.0;
        double cpuRealtime = 0.0;
        float peak = 0.0f;
        std::uint64_t invalid = 0;
    };

    churchorgan::PipeOrganPrepareSpec specFor (
        churchorgan::PipeOrganQuality quality,
        std::uint64_t seed)
    {
        churchorgan::PipeOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = seed;
        spec.quality = quality;
        spec.temperament =
            churchorgan::PipeOrganTemperament::churchWell;
        spec.tuningAHz = 440.0f;
        spec.outputTrim = 0.082f;
        spec.stereoWidth = 0.72f;
        return spec;
    }

    Result run (
        int notes,
        double requestedSeconds,
        churchorgan::PipeOrganQuality quality,
        bool useAllManuals,
        std::uint64_t seed)
    {
        churchorgan::PipeOrganEngine engine;
        Result result;

        if (! engine.prepare (specFor (quality, seed)))
            return result;

        engine.setTremulant (
            churchorgan::PipeOrganManual::lower,
            true, 0.48f, 5.6f);

        for (int voice = 0; voice < notes; ++voice)
        {
            const auto manual =
                useAllManuals
                    ? static_cast<churchorgan::PipeOrganManual> (
                        voice % 3)
                    : churchorgan::PipeOrganManual::upper;
            const int note =
                notes >= 128
                    ? voice
                    : 36 + (voice * 7) % 61;
            engine.queueNoteOn (
                manual,
                note,
                82 + (voice * 11) % 46,
                0);
        }

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        const auto requestedSamples =
            static_cast<std::uint64_t> (
                requestedSeconds * sampleRate);
        const auto blocks =
            (requestedSamples + blockSize - 1) / blockSize;
        const auto beginWall =
            std::chrono::steady_clock::now();
        const auto beginCpu = std::clock();
        float peak = 0.0f;

        for (std::uint64_t block = 0; block < blocks; ++block)
        {
            if (! engine.render ({
                    left.data(), right.data(), blockSize }))
                return {};

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
                peak = std::max (
                    peak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
        }

        const auto endCpu = std::clock();
        const auto endWall =
            std::chrono::steady_clock::now();
        result.audioSeconds =
            static_cast<double> (blocks * blockSize)
            / sampleRate;
        result.wallSeconds =
            std::chrono::duration<double> (
                endWall - beginWall).count();
        result.cpuSeconds =
            static_cast<double> (endCpu - beginCpu)
            / static_cast<double> (CLOCKS_PER_SEC);
        result.oneCorePercent =
            result.cpuSeconds / result.audioSeconds * 100.0;
        result.wallRealtime =
            result.audioSeconds / result.wallSeconds;
        result.cpuRealtime =
            result.audioSeconds / result.cpuSeconds;
        result.peak = peak;
        result.invalid =
            engine.diagnostics().invalidSamples;
        return result;
    }

    void print (const char* label, const Result& result)
    {
        std::cout << label
                  << " audio_s=" << result.audioSeconds
                  << " wall_s=" << result.wallSeconds
                  << " cpu_s=" << result.cpuSeconds
                  << " one_core_pct=" << result.oneCorePercent
                  << " wall_realtime_x=" << result.wallRealtime
                  << " cpu_realtime_x=" << result.cpuRealtime
                  << " peak=" << result.peak
                  << " invalid=" << result.invalid
                  << '\n';
    }
}

int main()
{
    const auto typical = run (
        12,
        20.0,
        churchorgan::PipeOrganQuality::standard,
        true,
        0x123456789abcdef0ULL);
    const auto full = run (
        128,
        4.0,
        churchorgan::PipeOrganQuality::eco,
        false,
        0x0fedcba987654321ULL);
    const auto ultra = run (
        8,
        6.0,
        churchorgan::PipeOrganQuality::ultra,
        true,
        0x3141592653589793ULL);

    print ("PIPE_STANDARD_12_NOTES", typical);
    print ("PIPE_ECO_128_NOTES", full);
    print ("PIPE_ULTRA_8_NOTES", ultra);

    const bool passed =
        typical.wallSeconds > 0.0
        && full.wallSeconds > 0.0
        && ultra.wallSeconds > 0.0
        && typical.cpuSeconds > 0.0
        && full.cpuSeconds > 0.0
        && ultra.cpuSeconds > 0.0
        && typical.wallRealtime > 1.0
        && full.wallRealtime > 1.0
        && ultra.wallRealtime > 1.0
        && typical.cpuRealtime > 1.0
        && full.cpuRealtime > 1.0
        && ultra.cpuRealtime > 1.0
        && typical.invalid == 0
        && full.invalid == 0
        && ultra.invalid == 0
        && typical.peak < 0.97f
        && full.peak < 0.97f
        && ultra.peak < 0.97f;

    std::cout << (
        passed
            ? "CHURCH_PIPE_ORGAN_BENCHMARK_OK"
            : "CHURCH_PIPE_ORGAN_BENCHMARK_FAILED")
              << '\n';
    return passed ? 0 : 1;
}
