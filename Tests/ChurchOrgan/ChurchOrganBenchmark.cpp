#include "ChurchOrganCore.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;

    struct BenchmarkResult
    {
        double audioSeconds = 0.0;
        double wallSeconds = 0.0;
        double percentOfOneCore = 0.0;
        double realtimeMultiple = 0.0;
        float peak = 0.0f;
        std::uint64_t invalidSamples = 0;
        double checksum = 0.0;
        bool valid = false;
    };

    BenchmarkResult runBenchmark (
        int noteCount,
        double audioSeconds,
        std::uint64_t seed)
    {
        churchorgan::ChurchOrganCore core;
        constexpr std::array<float, 9> registration {{
            8.0f, 6.0f, 8.0f, 5.0f, 4.0f,
            3.0f, 2.0f, 1.0f, 0.0f
        }};

        for (std::size_t drawbar = 0;
             drawbar < registration.size();
             ++drawbar)
            core.setDrawbarPosition (
                drawbar, registration[drawbar]);

        churchorgan::PrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = seed;
        spec.maximumWheelOffsetCents = 0.15f;
        spec.outputTrim = 0.11f;

        BenchmarkResult result;
        result.audioSeconds = audioSeconds;

        if (! core.prepare (spec))
            return result;

        for (int note = 0; note < noteCount; ++note)
            if (! core.queueUpperNoteOn (
                    noteCount < 128
                        ? 48 + note * 3
                        : note,
                    100))
                return result;

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};

        // Aquecimento fora da medicao: enche caches e deixa contatos estaveis.
        const int warmupBlocks = static_cast<int> (
            2.0 * sampleRate
            / static_cast<double> (blockSize));

        for (int block = 0; block < warmupBlocks; ++block)
            if (! core.render (
                    { left.data(), right.data(), blockSize }))
                return result;

        const std::uint64_t measuredBlocks =
            static_cast<std::uint64_t> (
                audioSeconds * sampleRate
                / static_cast<double> (blockSize));
        volatile double checksum = 0.0;
        const auto started =
            std::chrono::steady_clock::now();

        for (std::uint64_t block = 0;
             block < measuredBlocks;
             ++block)
        {
            if (! core.render (
                    { left.data(), right.data(), blockSize }))
                return result;

            checksum += left[
                static_cast<std::size_t> (
                    block % blockSize)];
        }

        const auto ended =
            std::chrono::steady_clock::now();
        result.wallSeconds =
            std::chrono::duration<double> (
                ended - started).count();
        result.percentOfOneCore =
            result.wallSeconds / audioSeconds * 100.0;
        result.realtimeMultiple =
            audioSeconds / result.wallSeconds;
        const auto diagnostics = core.diagnostics();
        result.peak = diagnostics.lastBlockPeak;
        result.invalidSamples =
            diagnostics.invalidOutputSamples;
        result.checksum = checksum;
        result.valid =
            std::isfinite (result.checksum)
            && result.wallSeconds > 0.0
            && result.realtimeMultiple > 1.0
            && result.invalidSamples == 0;
        return result;
    }

    void printResult (
        const char* name,
        int notes,
        const BenchmarkResult& result)
    {
        std::cout
            << std::fixed << std::setprecision (4)
            << "CHURCH_ORGAN_BENCH"
            << " scenario=" << name
            << " notes=" << notes
            << " audio_s=" << result.audioSeconds
            << " wall_s=" << result.wallSeconds
            << " one_core_pct=" << result.percentOfOneCore
            << " realtime_x=" << result.realtimeMultiple
            << " peak=" << result.peak
            << " invalid=" << result.invalidSamples
            << " checksum=" << result.checksum
            << '\n';
    }
}

int main()
{
    const auto typical = runBenchmark (
        5, 120.0, 0x1111222233334444ULL);
    const auto fullPolyphony = runBenchmark (
        128, 20.0, 0x5555666677778888ULL);

    printResult ("typical", 5, typical);
    printResult ("full_polyphony", 128, fullPolyphony);

    if (! typical.valid || ! fullPolyphony.valid)
    {
        std::cerr
            << "CHURCH_ORGAN_BENCHMARK_FAILED\n";
        return 1;
    }

    std::cout << "CHURCH_ORGAN_BENCHMARK_OK\n";
    return 0;
}
