#include "AutoMixContext.h"
#include "WorshipLayerEngine.h"

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
    constexpr std::size_t blockSize = 128;
    constexpr double durationSeconds = 30.0;

    struct Result
    {
        double elapsedSeconds = 0.0;
        double oneCorePercent = 0.0;
        double realtimeFactor = 0.0;
        float layerPeak = 0.0f;
        float sendPeak = 0.0f;
        bool finite = true;
    };

    Result run (
        churchorgan::WorshipQuality quality,
        int heldNotes)
    {
        churchorgan::WorshipLayerEngine engine;
        engine.setWorshipIntensity (0.82f);
        engine.setOutputGain (0.78f);
        if (! engine.prepare ({
                sampleRate,
                blockSize,
                quality,
                0x1020304050607080ULL
            }))
        {
            return {};
        }

        for (int note = 0; note < heldNotes; ++note)
            engine.queueNoteOn (43 + note, 102, 0);

        churchorgan::AutoMixContext autoMix;
        churchorgan::AutoMixParameters autoMixParameters;
        autoMixParameters.amount = 0.38f;
        autoMix.setParameters (autoMixParameters);
        if (! autoMix.prepare ({ sampleRate, blockSize }))
            return {};
        autoMix.setControlFrame ({ 0.42f, 0.18f, 0.24f, 0.62f });

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        std::array<float, blockSize> sendLeft {};
        std::array<float, blockSize> sendRight {};
        const auto totalSamples =
            static_cast<std::size_t> (
                durationSeconds * sampleRate);
        auto rendered = std::size_t { 0 };
        auto layerPeak = 0.0f;
        auto sendPeak = 0.0f;
        auto finite = true;

        const auto start = std::chrono::steady_clock::now();
        while (rendered < totalSamples)
        {
            const auto count =
                std::min (blockSize, totalSamples - rendered);
            std::fill_n (left.data(), count, 0.0f);
            std::fill_n (right.data(), count, 0.0f);
            std::fill_n (sendLeft.data(), count, 0.0f);
            std::fill_n (sendRight.data(), count, 0.0f);

            const auto gains = autoMix.advance (count);
            engine.setAutoMixLayerGain (gains.worshipLayer);
            engine.renderAdd ({
                left.data(),
                right.data(),
                sendLeft.data(),
                sendRight.data(),
                count
            });

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
                layerPeak = std::max (
                    layerPeak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
                sendPeak = std::max (
                    sendPeak,
                    std::max (
                        std::abs (sendLeft[sample]),
                        std::abs (sendRight[sample])));
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
            layerPeak,
            sendPeak,
            finite
        };
    }

    void print (
        const char* label,
        const Result& result)
    {
        std::cout
            << label
            << " elapsed_s=" << result.elapsedSeconds
            << " one_core_percent=" << result.oneCorePercent
            << " realtime_x=" << result.realtimeFactor
            << " layer_peak=" << result.layerPeak
            << " send_peak=" << result.sendPeak
            << " finite=" << (result.finite ? 1 : 0)
            << '\n';
    }
}

int main()
{
    std::cout << std::fixed << std::setprecision (4);
    std::cout
        << "fixed_memory_bytes worship="
        << sizeof (churchorgan::WorshipLayerEngine)
        << " automix=" << sizeof (churchorgan::AutoMixContext)
        << " total="
        << (sizeof (churchorgan::WorshipLayerEngine)
            + sizeof (churchorgan::AutoMixContext))
        << '\n';

    const auto eco = run (
        churchorgan::WorshipQuality::eco, 10);
    const auto standard = run (
        churchorgan::WorshipQuality::standard, 16);
    const auto ultra = run (
        churchorgan::WorshipQuality::ultra, 24);

    print ("eco_10_notes", eco);
    print ("standard_16_notes", standard);
    print ("ultra_24_notes", ultra);

    const auto passed =
        eco.finite
        && standard.finite
        && ultra.finite
        && eco.realtimeFactor > 1.0
        && standard.realtimeFactor > 1.0
        && ultra.realtimeFactor > 1.0
        && eco.layerPeak < 0.681f
        && standard.layerPeak < 0.681f
        && ultra.layerPeak < 0.681f
        && eco.sendPeak < 0.501f
        && standard.sendPeak < 0.501f
        && ultra.sendPeak < 0.501f;

    std::cout
        << (passed
                ? "CHURCH_ORGAN_WORSHIP_BENCHMARK_OK"
                : "CHURCH_ORGAN_WORSHIP_BENCHMARK_FAILED")
        << '\n';
    return passed ? 0 : 1;
}
