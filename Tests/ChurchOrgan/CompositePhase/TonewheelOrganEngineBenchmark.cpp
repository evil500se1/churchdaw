#include "TonewheelOrganEngine.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>

namespace
{
    struct Result
    {
        double elapsedSeconds = 0.0;
        double oneCorePercent = 0.0;
        double realtimeMultiple = 0.0;
        float peak = 0.0f;
        std::uint64_t invalid = 0;
    };

    Result run (
        churchorgan::OrganQualityProfile quality,
        double audioSeconds)
    {
        using namespace churchorgan;
        constexpr std::size_t blockSize = 128;
        constexpr double sampleRate = 48000.0;

        TonewheelOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.quality = quality;
        spec.deterministicSeed = 0x42454e43484d4152ULL;

        TonewheelOrganEngine engine;
        if (! engine.prepare (spec))
            return {};

        engine.parameters().setPlain (
            OrganParameterId::scannerMode, 5.0f);
        engine.parameters().setPlain (
            OrganParameterId::drive, 0.46f);
        engine.parameters().setPlain (
            OrganParameterId::rotaryMode, 2.0f);
        engine.parameters().setPlain (
            OrganParameterId::worshipIntensity, 0.55f);

        for (const int note : { 48, 55, 60, 64, 67 })
            engine.queueNoteOn (Manual::upper, note, 100, 0);
        engine.queueNoteOn (Manual::lower, 43, 92, 0);
        engine.queueNoteOn (Manual::pedal, 36, 110, 0);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        const auto blocks = static_cast<std::size_t> (
            std::ceil (audioSeconds * sampleRate / blockSize));

        const auto start = std::chrono::steady_clock::now();
        for (std::size_t block = 0; block < blocks; ++block)
        {
            if ((block % 211) == 0)
                engine.parameters().setNormalised (
                    OrganParameterId::rotaryStereoWidth,
                    static_cast<float> ((block / 211) % 10) / 9.0f);

            if (! engine.render ({ left.data(), right.data(), blockSize }))
                return {};
        }
        const auto end = std::chrono::steady_clock::now();

        Result result;
        result.elapsedSeconds =
            std::chrono::duration<double> (end - start).count();
        const auto renderedSeconds =
            static_cast<double> (blocks * blockSize) / sampleRate;
        result.oneCorePercent =
            100.0 * result.elapsedSeconds / renderedSeconds;
        result.realtimeMultiple =
            renderedSeconds / result.elapsedSeconds;
        const auto diagnostics = engine.diagnostics();
        result.peak = diagnostics.lastBlockPeak;
        result.invalid = diagnostics.invalidOutputSamples;
        return result;
    }

    const char* name (churchorgan::OrganQualityProfile quality)
    {
        using churchorgan::OrganQualityProfile;
        switch (quality)
        {
            case OrganQualityProfile::eco: return "eco";
            case OrganQualityProfile::standard: return "standard";
            case OrganQualityProfile::ultra: return "ultra";
        }
        return "unknown";
    }
}

int main()
{
    using churchorgan::OrganQualityProfile;
    bool ok = true;

    std::cout << std::fixed << std::setprecision (4);
    for (const auto quality : {
             OrganQualityProfile::eco,
             OrganQualityProfile::standard,
             OrganQualityProfile::ultra })
    {
        const auto result = run (quality, 20.0);
        std::cout << name (quality)
                  << " elapsed_s=" << result.elapsedSeconds
                  << " one_core_percent=" << result.oneCorePercent
                  << " realtime_x=" << result.realtimeMultiple
                  << " peak=" << result.peak
                  << " invalid=" << result.invalid
                  << '\n';

        const double budget =
            quality == OrganQualityProfile::eco ? 10.0
            : quality == OrganQualityProfile::standard ? 12.0
                                                       : 18.0;
        ok = ok
             && result.elapsedSeconds > 0.0
             && result.oneCorePercent < budget
             && result.invalid == 0
             && std::isfinite (result.peak)
             && result.peak < 2.0f;
    }

    if (! ok)
    {
        std::cerr << "CHURCH_ORGAN_COMPOSITE_BENCHMARK_FAILED\n";
        return 1;
    }

    std::cout << "CHURCH_ORGAN_COMPOSITE_BENCHMARK_OK\n";
    return 0;
}
