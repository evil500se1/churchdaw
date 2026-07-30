#include "WorshipHybridEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;
    constexpr double durationSeconds = 8.0;

    struct Result
    {
        double elapsedSeconds = 0.0;
        double oneCorePercent = 0.0;
        double realtimeFactor = 0.0;
        float mainPeak = 0.0f;
        float sendPeak = 0.0f;
        bool finite = true;
        bool rendered = true;
    };

    Result run (
        churchorgan::WorshipHybridQuality quality)
    {
        auto engine =
            std::make_unique<churchorgan::WorshipHybridEngine>();
        engine->setWorshipIntensity (0.78f);
        engine->setOutputGain (0.94f);
        if (! engine->prepare ({
                sampleRate,
                blockSize,
                0x42454e43484d4152ULL,
                quality
            }))
        {
            return { 0.0, 0.0, 0.0, 0.0f, 0.0f, false, false };
        }

        churchorgan::AutoMixParameters autoMix;
        autoMix.amount = 0.38f;
        autoMix.reactionMilliseconds = 720.0f;
        autoMix.recoveryMilliseconds = 1900.0f;
        engine->setAutoMixParameters (autoMix);
        engine->setAutoMixControlFrame ({
            0.42f, 0.18f, 0.24f, 0.62f
        });

        const std::array<int, 10> upper {
            48, 52, 55, 60, 64, 67, 72, 76, 79, 84
        };
        const std::array<int, 4> lower {
            43, 50, 55, 59
        };
        for (const auto note : upper)
            engine->queueNoteOn (
                churchorgan::Manual::upper, note, 104, 0);
        for (const auto note : lower)
            engine->queueNoteOn (
                churchorgan::Manual::lower, note, 92, 0);
        engine->queueNoteOn (
            churchorgan::Manual::pedal, 36, 116, 0);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        std::array<float, blockSize> sendLeft {};
        std::array<float, blockSize> sendRight {};
        const auto totalSamples =
            static_cast<std::size_t> (
                sampleRate * durationSeconds);
        auto renderedSamples = std::size_t { 0 };
        auto mainPeak = 0.0f;
        auto sendPeak = 0.0f;
        auto finite = true;
        auto rendered = true;

        const auto start = std::chrono::steady_clock::now();
        while (renderedSamples < totalSamples)
        {
            const auto count = std::min (
                blockSize,
                totalSamples - renderedSamples);
            rendered =
                engine->render ({
                    left.data(),
                    right.data(),
                    sendLeft.data(),
                    sendRight.data(),
                    count
                })
                && rendered;

            for (std::size_t sample = 0;
                 sample < count;
                 ++sample)
            {
                finite =
                    finite
                    && std::isfinite (left[sample])
                    && std::isfinite (right[sample])
                    && std::isfinite (sendLeft[sample])
                    && std::isfinite (sendRight[sample]);
                mainPeak = std::max (
                    mainPeak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
                sendPeak = std::max (
                    sendPeak,
                    std::max (
                        std::abs (sendLeft[sample]),
                        std::abs (sendRight[sample])));
            }
            renderedSamples += count;
        }
        const auto finish = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration<double> (
                finish - start).count();
        return {
            elapsed,
            100.0 * elapsed / durationSeconds,
            durationSeconds / elapsed,
            mainPeak,
            sendPeak,
            finite,
            rendered
        };
    }

    void print (
        const char* name,
        const Result& result)
    {
        std::cout
            << name
            << " elapsed_s=" << result.elapsedSeconds
            << " one_core_percent=" << result.oneCorePercent
            << " realtime_x=" << result.realtimeFactor
            << " main_peak=" << result.mainPeak
            << " send_peak=" << result.sendPeak
            << " finite=" << (result.finite ? 1 : 0)
            << " rendered=" << (result.rendered ? 1 : 0)
            << '\n';
    }
}

int main()
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout
        << "typical=48kHz/128 samples/14 manual notes+pedal"
        << " duration_s=" << durationSeconds
        << " engine_bytes="
        << sizeof (churchorgan::WorshipHybridEngine)
        << '\n';

    const auto eco = run (
        churchorgan::WorshipHybridQuality::eco);
    const auto standard = run (
        churchorgan::WorshipHybridQuality::standard);
    const auto ultra = run (
        churchorgan::WorshipHybridQuality::ultra);
    print ("Eco", eco);
    print ("Standard", standard);
    print ("Ultra", ultra);

    const auto valid = [] (const Result& result)
    {
        return result.rendered
            && result.finite
            && result.mainPeak < 0.985001f
            && result.sendPeak < 0.500001f
            && result.elapsedSeconds > 0.0;
    };
    return valid (eco)
            && valid (standard)
            && valid (ultra)
        ? 0
        : 1;
}
