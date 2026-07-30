#include "TransistorOrganEngine.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>

#if defined (_WIN32)
 #if ! defined (WIN32_LEAN_AND_MEAN)
  #define WIN32_LEAN_AND_MEAN
 #endif
 #if ! defined (NOMINMAX)
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;

    struct BenchmarkResult
    {
        double audioSeconds = 0.0;
        double wallSeconds = 0.0;
        double cpuSeconds = 0.0;
        double oneCorePercent = 0.0;
        double realtimeFactor = 0.0;
        double cpuRealtimeFactor = 0.0;
        float peak = 0.0f;
        std::uint64_t invalidSamples = 0;
    };

    double currentThreadCpuSeconds() noexcept
    {
#if defined (_WIN32)
        FILETIME creation {};
        FILETIME exit {};
        FILETIME kernel {};
        FILETIME user {};

        if (GetThreadTimes (
                GetCurrentThread(),
                &creation,
                &exit,
                &kernel,
                &user) == 0)
            return 0.0;

        ULARGE_INTEGER kernelTicks {};
        kernelTicks.LowPart = kernel.dwLowDateTime;
        kernelTicks.HighPart = kernel.dwHighDateTime;
        ULARGE_INTEGER userTicks {};
        userTicks.LowPart = user.dwLowDateTime;
        userTicks.HighPart = user.dwHighDateTime;
        return static_cast<double> (
                   kernelTicks.QuadPart + userTicks.QuadPart)
               * 1.0e-7;
#else
        return static_cast<double> (std::clock())
               / static_cast<double> (CLOCKS_PER_SEC);
#endif
    }

    churchorgan::TransistorOrganPrepareSpec specFor (
        std::uint64_t seed)
    {
        churchorgan::TransistorOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = seed;
        spec.profile =
            churchorgan::TransistorOrganProfile::italianCombo;
        spec.outputTrim = 0.045f;
        spec.drive = 0.18f;
        spec.stereoWidth = 0.22f;
        spec.vibratoDepth = 0.55f;
        spec.vibratoEnabled = true;
        return spec;
    }

    BenchmarkResult runBenchmark (
        int noteCount,
        double requestedAudioSeconds,
        std::uint64_t seed)
    {
        churchorgan::TransistorOrganEngine engine;
        BenchmarkResult result;

        if (! engine.prepare (specFor (seed)))
            return result;

        for (int note = 0; note < noteCount; ++note)
        {
            const int midi = noteCount >= 128
                ? note
                : 36 + (note * 7) % 61;
            engine.queueUpperNoteOn (
                midi,
                80 + (note * 11) % 48,
                0);
        }

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        const auto totalSamples = static_cast<std::uint64_t> (
            requestedAudioSeconds * sampleRate);
        const auto blocks =
            (totalSamples + blockSize - 1) / blockSize;
        const auto begin = std::chrono::steady_clock::now();
        const auto beginCpu = currentThreadCpuSeconds();
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

        const auto end = std::chrono::steady_clock::now();
        const auto endCpu = currentThreadCpuSeconds();
        result.audioSeconds =
            static_cast<double> (blocks * blockSize) / sampleRate;
        result.wallSeconds =
            std::chrono::duration<double> (end - begin).count();
        result.cpuSeconds = endCpu - beginCpu;
        result.oneCorePercent =
            result.cpuSeconds / result.audioSeconds * 100.0;
        result.realtimeFactor =
            result.audioSeconds / result.wallSeconds;
        result.cpuRealtimeFactor =
            result.audioSeconds / result.cpuSeconds;
        result.peak = peak;
        result.invalidSamples =
            engine.diagnostics().invalidSamples;
        return result;
    }

    void printResult (
        const char* label,
        const BenchmarkResult& result)
    {
        std::cout << label
                  << " audio_s=" << result.audioSeconds
                  << " wall_s=" << result.wallSeconds
                  << " cpu_s=" << result.cpuSeconds
                  << " one_core_pct=" << result.oneCorePercent
                  << " wall_realtime_x=" << result.realtimeFactor
                  << " cpu_realtime_x=" << result.cpuRealtimeFactor
                  << " peak=" << result.peak
                  << " invalid=" << result.invalidSamples
                  << '\n';
    }
}

int main()
{
    const auto idle = runBenchmark (
        0, 10.0, 0x55aa55aa55aa55aaULL);
    const auto single = runBenchmark (
        1, 10.0, 0xaa55aa55aa55aa55ULL);
    const auto typical = runBenchmark (
        10, 30.0, 0x123456789abcdef0ULL);
    const auto full = runBenchmark (
        128, 5.0, 0x0fedcba987654321ULL);

    printResult ("TRANSISTOR_IDLE", idle);
    printResult ("TRANSISTOR_SINGLE_NOTE", single);
    printResult ("TRANSISTOR_TYPICAL_10_NOTES", typical);
    printResult ("TRANSISTOR_FULL_128_NOTES", full);

    const bool passed =
        idle.wallSeconds > 0.0
        && single.wallSeconds > 0.0
        && typical.wallSeconds > 0.0
        && full.wallSeconds > 0.0
        && idle.cpuSeconds > 0.0
        && single.cpuSeconds > 0.0
        && typical.cpuSeconds > 0.0
        && full.cpuSeconds > 0.0
        && typical.realtimeFactor > 1.0
        && full.realtimeFactor > 1.0
        && typical.cpuRealtimeFactor > 1.0
        && full.cpuRealtimeFactor > 1.0
        && typical.invalidSamples == 0
        && full.invalidSamples == 0
        && typical.peak <= 0.999f
        && full.peak <= 0.999f
        && typical.oneCorePercent <= 8.0
        && full.oneCorePercent <= 25.0;

    std::cout << (
        passed
            ? "CHURCH_TRANSISTOR_ORGAN_BENCHMARK_OK"
            : "CHURCH_TRANSISTOR_ORGAN_BENCHMARK_FAILED")
              << '\n';
    return passed ? 0 : 1;
}
