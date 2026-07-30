#include "OrganAmpCab.h"
#include "RotaryCabinet.h"
#include "ScannerVibrato.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace
{
    std::atomic<std::size_t> allocationCount { 0 };
    std::atomic<bool> trackAllocations { false };

    void recordAllocation() noexcept
    {
        if (trackAllocations.load (std::memory_order_relaxed))
            allocationCount.fetch_add (1, std::memory_order_relaxed);
    }
}

void* operator new (std::size_t size)
{
    recordAllocation();
    if (auto* memory = std::malloc (size))
        return memory;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    recordAllocation();
    if (auto* memory = std::malloc (size))
        return memory;
    throw std::bad_alloc();
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;
    constexpr float twoPi =
        6.283185307179586476925286766559f;

    struct TestContext
    {
        void expect (bool condition, const char* description)
        {
            ++checks;
            if (! condition)
            {
                ++failures;
                std::cerr
                    << "FAIL: " << description << '\n';
            }
        }

        void expectNear (
            float actual,
            float expected,
            float tolerance,
            const char* description)
        {
            expect (
                std::abs (actual - expected) <= tolerance,
                description);
        }

        int checks = 0;
        int failures = 0;
    };

    bool allFinite (const std::vector<float>& values)
    {
        return std::all_of (
            values.begin(),
            values.end(),
            [] (float value) { return std::isfinite (value); });
    }

    float peakOf (const std::vector<float>& values)
    {
        auto peak = 0.0f;
        for (const auto value : values)
            peak = std::max (peak, std::abs (value));
        return peak;
    }

    float rmsDifference (
        const std::vector<float>& left,
        const std::vector<float>& right,
        std::size_t start = 0)
    {
        auto sum = 0.0;
        const auto count =
            std::min (left.size(), right.size());
        for (auto index = start; index < count; ++index)
        {
            const auto difference =
                static_cast<double> (
                    left[index] - right[index]);
            sum += difference * difference;
        }
        const auto samples =
            count > start ? count - start : 1;
        return static_cast<float> (
            std::sqrt (sum / static_cast<double> (samples)));
    }

    float dftMagnitude (
        const std::vector<float>& signal,
        std::size_t bin)
    {
        auto real = 0.0;
        auto imaginary = 0.0;
        const auto count = signal.size();
        for (std::size_t index = 0;
             index < count;
             ++index)
        {
            const auto angle =
                -static_cast<double> (twoPi)
                * static_cast<double> (bin)
                * static_cast<double> (index)
                / static_cast<double> (count);
            real +=
                static_cast<double> (signal[index])
                * std::cos (angle);
            imaginary +=
                static_cast<double> (signal[index])
                * std::sin (angle);
        }
        return static_cast<float> (
            2.0
            * std::sqrt (real * real + imaginary * imaginary)
            / static_cast<double> (count));
    }

    void fillSine (
        std::vector<float>& values,
        float frequency,
        float level = 0.4f,
        float phaseOffset = 0.0f)
    {
        for (std::size_t index = 0;
             index < values.size();
             ++index)
        {
            values[index] =
                level
                * std::sin (
                    twoPi * frequency
                        * static_cast<float> (index)
                        / static_cast<float> (sampleRate)
                    + phaseOffset);
        }
    }

    template <typename Processor>
    void processInBlocks (
        Processor& processor,
        std::vector<float>& left,
        std::vector<float>* right = nullptr)
    {
        for (std::size_t offset = 0;
             offset < left.size();
             offset += blockSize)
        {
            const auto remaining = left.size() - offset;
            const auto samples = std::min (blockSize, remaining);
            processor.process (
                left.data() + offset,
                right != nullptr ? right->data() + offset : nullptr,
                samples);
        }
    }

    void testPrepareValidation (TestContext& context)
    {
        churchorgan::ScannerVibrato scanner;
        churchorgan::ScannerPrepareSpec scannerSpec;
        scannerSpec.sampleRate =
            std::numeric_limits<double>::quiet_NaN();
        context.expect (
            ! scanner.prepare (scannerSpec),
            "scanner rejects non-finite sample rate");
        scannerSpec.sampleRate = sampleRate;
        context.expect (
            scanner.prepare (scannerSpec),
            "scanner prepares at 48 kHz");

        churchorgan::OrganAmpCab amp;
        churchorgan::OrganAmpPrepareSpec ampSpec;
        ampSpec.maximumBlockSize = 0;
        context.expect (
            ! amp.prepare (ampSpec),
            "amp rejects zero maximum block size");
        ampSpec.maximumBlockSize = blockSize;
        context.expect (
            amp.prepare (ampSpec),
            "amp prepares at 48 kHz");

        churchorgan::RotaryCabinet rotary;
        churchorgan::RotaryPrepareSpec rotarySpec;
        rotarySpec.sampleRate = 400000.0;
        context.expect (
            ! rotary.prepare (rotarySpec),
            "rotary rejects unsupported sample rate");
        rotarySpec.sampleRate = sampleRate;
        context.expect (
            rotary.prepare (rotarySpec),
            "rotary prepares at 48 kHz");
    }

    void testScannerBypassAndModulation (TestContext& context)
    {
        constexpr std::size_t samples =
            static_cast<std::size_t> (sampleRate);

        churchorgan::ScannerVibrato bypass;
        churchorgan::ScannerParameters bypassParameters;
        bypassParameters.mode =
            churchorgan::ScannerMode::bypass;
        bypassParameters.output = 1.0f;
        bypass.setParameters (bypassParameters);
        context.expect (
            bypass.prepare ({ sampleRate, blockSize, 17U, 0.2f }),
            "scanner bypass prepares");

        std::vector<float> bypassSignal (samples);
        fillSine (bypassSignal, 441.0f);
        const auto original = bypassSignal;
        processInBlocks (bypass, bypassSignal);

        auto maximumError = 0.0f;
        for (std::size_t index = 0;
             index < samples;
             ++index)
        {
            maximumError = std::max (
                maximumError,
                std::abs (
                    bypassSignal[index] - original[index]));
        }
        context.expect (
            maximumError < 1.0e-7f,
            "scanner bypass remains sample transparent");

        churchorgan::ScannerVibrato scanner;
        churchorgan::ScannerParameters parameters;
        parameters.mode = churchorgan::ScannerMode::chorus3;
        parameters.character =
            churchorgan::ScannerCharacter::deep;
        parameters.stereoSpread = 0.8f;
        parameters.age = 0.35f;
        parameters.noise = 0.0f;
        parameters.worshipIntensity = 0.7f;
        scanner.setParameters (parameters);
        context.expect (
            scanner.prepare ({ sampleRate, blockSize, 91U, 0.33f }),
            "scanner chorus prepares");

        std::vector<float> left (samples);
        std::vector<float> right (samples);
        fillSine (left, 523.25f);
        right = left;
        processInBlocks (scanner, left, &right);

        context.expect (
            allFinite (left) && allFinite (right),
            "scanner output remains finite");
        context.expect (
            rmsDifference (
                left,
                right,
                static_cast<std::size_t> (sampleRate * 0.15))
                > 0.002f,
            "scanner creates coherent subtle stereo motion");
        context.expect (
            peakOf (left) < 1.1f && peakOf (right) < 1.1f,
            "scanner output remains bounded");
        context.expectNear (
            scanner.nominalWetLatencyMilliseconds(),
            4.15f,
            0.001f,
            "scanner reports nominal phase-line latency");
    }

    void testAmpCharacterAndProtection (TestContext& context)
    {
        churchorgan::OrganAmpCab amp;
        churchorgan::OrganAmpParameters parameters;
        parameters.profile =
            churchorgan::OrganAmpProfile::driven;
        parameters.drive = 1.0f;
        parameters.bias = 0.8f;
        parameters.sag = 0.8f;
        parameters.cabinet = 0.75f;
        parameters.output = 1.5f;
        parameters.automaticGain = true;
        parameters.peakProtection = true;
        amp.setParameters (parameters);

        churchorgan::OrganAmpPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.oversampling =
            churchorgan::OrganAmpOversampling::x8;
        context.expect (
            amp.prepare (spec),
            "driven amp prepares at 8x");
        context.expect (
            amp.oversamplingFactor() == 8,
            "amp reports selected oversampling factor");

        constexpr std::size_t samples =
            static_cast<std::size_t> (sampleRate * 2.0);
        std::vector<float> left (samples);
        std::vector<float> right (samples);
        for (std::size_t index = 0;
             index < samples;
             ++index)
        {
            const auto impulse =
                index % 997 == 0
                    ? (index % 2 == 0 ? 8.0f : -8.0f)
                    : 0.0f;
            left[index] =
                1.8f
                    * std::sin (
                        twoPi * 220.0f
                        * static_cast<float> (index)
                        / static_cast<float> (sampleRate))
                + impulse;
            right[index] =
                1.6f
                    * std::sin (
                        twoPi * 330.0f
                        * static_cast<float> (index)
                        / static_cast<float> (sampleRate))
                - impulse;
        }
        processInBlocks (amp, left, &right);

        context.expect (
            allFinite (left) && allFinite (right),
            "amp sanitises extreme input without invalid samples");
        context.expect (
            peakOf (left) < 0.986f
                && peakOf (right) < 0.986f
                && amp.lastPeak() < 0.986f,
            "amp peak protection provides bounded headroom");

        churchorgan::OrganAmpCab dcTest;
        churchorgan::OrganAmpParameters dcParameters;
        dcParameters.profile =
            churchorgan::OrganAmpProfile::warm;
        dcParameters.drive = 0.65f;
        dcTest.setParameters (dcParameters);
        context.expect (
            dcTest.prepare ({ sampleRate, blockSize,
                              churchorgan::OrganAmpOversampling::x4 }),
            "amp DC test prepares");
        std::vector<float> dc (
            static_cast<std::size_t> (sampleRate * 1.5),
            0.2f);
        processInBlocks (dcTest, dc);
        context.expect (
            std::abs (dc.back()) < 0.01f,
            "asymmetric amp stage removes steady DC");
    }

    std::vector<float> renderAliasProbe (
        churchorgan::OrganAmpOversampling quality)
    {
        constexpr std::size_t analysisSamples = 8192;
        constexpr std::size_t warmupSamples = 8192;
        constexpr std::size_t fundamentalBin = 1536;
        constexpr auto frequency =
            static_cast<float> (
                sampleRate
                * static_cast<double> (fundamentalBin)
                / static_cast<double> (analysisSamples));

        churchorgan::OrganAmpCab amp;
        churchorgan::OrganAmpParameters parameters;
        parameters.profile =
            churchorgan::OrganAmpProfile::driven;
        parameters.drive = 1.0f;
        parameters.bias = 0.8f;
        parameters.sag = 0.0f;
        parameters.lowTightening = 0.0f;
        parameters.tone = 1.0f;
        parameters.presence = 0.5f;
        parameters.cabinet = 0.0f;
        parameters.output = 0.55f;
        parameters.automaticGain = false;
        parameters.peakProtection = false;
        amp.setParameters (parameters);
        amp.prepare ({ sampleRate, blockSize, quality });

        std::vector<float> result (analysisSamples);
        std::array<float, blockSize> block {};
        auto rendered = std::size_t { 0 };
        const auto totalSamples =
            warmupSamples + analysisSamples;
        while (rendered < totalSamples)
        {
            const auto count =
                std::min (blockSize, totalSamples - rendered);
            for (std::size_t sample = 0;
                 sample < count;
                 ++sample)
            {
                const auto absolute = rendered + sample;
                block[sample] =
                    0.88f
                    * std::sin (
                        twoPi * frequency
                        * static_cast<float> (absolute)
                        / static_cast<float> (sampleRate));
            }
            amp.process (block.data(), nullptr, count);
            for (std::size_t sample = 0;
                 sample < count;
                 ++sample)
            {
                const auto absolute = rendered + sample;
                if (absolute >= warmupSamples)
                {
                    result[absolute - warmupSamples] =
                        block[sample];
                }
            }
            rendered += count;
        }
        return result;
    }

    void testAmpOversamplingAliasReduction (TestContext& context)
    {
        const auto direct = renderAliasProbe (
            churchorgan::OrganAmpOversampling::off);
        const auto ultra = renderAliasProbe (
            churchorgan::OrganAmpOversampling::x8);

        // A 9 kHz probe aliases its third/fifth/seventh odd harmonics to
        // 21/3/15 kHz in a 48 kHz direct-rate nonlinear stage.
        const auto directAlias =
            dftMagnitude (direct, 3584)
            + dftMagnitude (direct, 512)
            + dftMagnitude (direct, 2560);
        const auto ultraAlias =
            dftMagnitude (ultra, 3584)
            + dftMagnitude (ultra, 512)
            + dftMagnitude (ultra, 2560);

        std::cout
            << "amp_alias_probe direct=" << directAlias
            << " x8=" << ultraAlias
            << " ratio="
            << (directAlias > 0.0f
                    ? ultraAlias / directAlias
                    : 0.0f)
            << '\n';
        context.expect (
            ultraAlias < directAlias * 0.72f,
            "8x nonlinear path materially reduces measured aliases");
    }

    void testRotaryGeometryAndRamps (TestContext& context)
    {
        churchorgan::RotaryCabinet rotary;
        churchorgan::RotaryParameters parameters;
        parameters.speedMode =
            churchorgan::RotarySpeedMode::stop;
        parameters.perspective =
            churchorgan::RotaryPerspective::wideWorship;
        parameters.worshipIntensity = 0.8f;
        parameters.mix = 1.0f;
        rotary.setParameters (parameters);
        context.expect (
            rotary.prepare ({ sampleRate, blockSize, 0.11f, 0.64f }),
            "rotary prepares from stopped state");

        auto diagnostics = rotary.diagnostics();
        context.expectNear (
            diagnostics.hornSpeedHz,
            0.0f,
            1.0e-7f,
            "stopped horn starts stationary");
        context.expectNear (
            diagnostics.drumSpeedHz,
            0.0f,
            1.0e-7f,
            "stopped drum starts stationary");

        rotary.setSpeedMode (churchorgan::RotarySpeedMode::fast);
        std::vector<float> left (
            static_cast<std::size_t> (sampleRate * 8.0));
        std::vector<float> right (left.size());
        fillSine (left, 196.0f, 0.45f);
        right = left;

        auto maximumJump = 0.0f;
        for (std::size_t offset = 0;
             offset < left.size();
             offset += blockSize)
        {
            const auto count =
                std::min (blockSize, left.size() - offset);
            rotary.process (
                left.data() + offset,
                right.data() + offset,
                count);

            if (offset > 0)
            {
                maximumJump = std::max (
                    maximumJump,
                    std::abs (
                        left[offset] - left[offset - 1]));
                maximumJump = std::max (
                    maximumJump,
                    std::abs (
                        right[offset] - right[offset - 1]));
            }
        }

        diagnostics = rotary.diagnostics();
        context.expect (
            diagnostics.hornSpeedHz
                > diagnostics.hornTargetHz * 0.96f,
            "horn reaches fast target through inertia ramp");
        context.expect (
            diagnostics.drumSpeedHz
                > diagnostics.drumTargetHz * 0.84f,
            "drum reaches fast target through slower inertia ramp");
        context.expect (
            rmsDifference (
                left,
                right,
                static_cast<std::size_t> (sampleRate))
                > 0.008f,
            "rotary geometry creates stereo movement");
        context.expect (
            maximumJump < 0.25f,
            "rotary ramp remains free of block-edge clicks");
        context.expect (
            allFinite (left) && allFinite (right),
            "rotary output remains finite during acceleration");
        context.expect (
            peakOf (left) < 0.996f
                && peakOf (right) < 0.996f,
            "rotary output remains headroom protected");

        rotary.setSpeedMode (
            churchorgan::RotarySpeedMode::brake);
        std::array<float, blockSize> brakeLeft {};
        std::array<float, blockSize> brakeRight {};
        const auto brakeBlocks =
            static_cast<std::size_t> (
                sampleRate * 2.0 / blockSize);
        for (std::size_t block = 0;
             block < brakeBlocks;
             ++block)
        {
            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                const auto time =
                    static_cast<float> (
                        block * blockSize + sample)
                    / static_cast<float> (sampleRate);
                brakeLeft[sample] =
                    0.35f * std::sin (twoPi * 196.0f * time);
                brakeRight[sample] = brakeLeft[sample];
            }
            rotary.process (
                brakeLeft.data(),
                brakeRight.data(),
                blockSize);
        }
        diagnostics = rotary.diagnostics();
        context.expect (
            diagnostics.hornSpeedHz < 0.002f
                && diagnostics.drumSpeedHz < 0.002f,
            "brake brings both rotors to rest");
    }

    void testRotaryDeterminismAndMonoPerspective (
        TestContext& context)
    {
        churchorgan::RotaryParameters parameters;
        parameters.speedMode =
            churchorgan::RotarySpeedMode::fast;
        parameters.perspective =
            churchorgan::RotaryPerspective::vintageMono;
        parameters.mix = 1.0f;

        churchorgan::RotaryCabinet first;
        churchorgan::RotaryCabinet second;
        first.setParameters (parameters);
        second.setParameters (parameters);
        const churchorgan::RotaryPrepareSpec spec {
            sampleRate, blockSize, 0.23f, 0.79f
        };
        context.expect (
            first.prepare (spec) && second.prepare (spec),
            "deterministic rotary pair prepares");

        std::vector<float> firstLeft (
            static_cast<std::size_t> (sampleRate));
        std::vector<float> firstRight (firstLeft.size());
        fillSine (firstLeft, 311.0f);
        firstRight = firstLeft;
        auto secondLeft = firstLeft;
        auto secondRight = firstRight;

        processInBlocks (first, firstLeft, &firstRight);
        processInBlocks (second, secondLeft, &secondRight);

        context.expect (
            firstLeft == secondLeft
                && firstRight == secondRight,
            "rotary render is deterministic for equal state");
        context.expect (
            rmsDifference (
                firstLeft,
                firstRight,
                static_cast<std::size_t> (sampleRate * 0.25))
                < 1.0e-5f,
            "vintage mono perspective converges to mono");
    }

    float rotaryProbeSample (
        std::uint64_t sampleIndex,
        bool rightChannel) noexcept
    {
        constexpr std::array<float, 7> frequencies {
            82.4069f,
            123.4708f,
            196.0f,
            293.6648f,
            440.0f,
            659.2551f,
            987.7666f
        };

        auto sample = 0.0f;
        for (std::size_t tone = 0;
             tone < frequencies.size();
             ++tone)
        {
            const auto phaseOffset = rightChannel
                ? 0.13f * static_cast<float> (tone + 1)
                : 0.0f;
            sample += 0.032f
                * std::sin (
                    twoPi * frequencies[tone]
                        * static_cast<float> (sampleIndex)
                        / static_cast<float> (sampleRate)
                    + phaseOffset);
        }
        return sample;
    }

    void testRotaryLongMixLoudness (
        TestContext& context,
        bool stereo)
    {
        using churchorgan::RotarySpeedMode;

        churchorgan::RotaryCabinet rotary;
        churchorgan::RotaryParameters parameters;
        parameters.perspective =
            churchorgan::RotaryPerspective::balanced;
        parameters.speedMode = RotarySpeedMode::stop;
        parameters.mix = 0.0f;
        parameters.output = 0.90f;
        parameters.hornLevel = 0.78f;
        parameters.drumLevel = 0.88f;
        rotary.setParameters (parameters);
        context.expect (
            rotary.prepare (
                { sampleRate, blockSize, 0.19f, 0.71f }),
            stereo
                ? "stereo long rotary probe prepares"
                : "mono long rotary probe prepares");

        constexpr std::array<RotarySpeedMode, 4> modes {
            RotarySpeedMode::stop,
            RotarySpeedMode::slow,
            RotarySpeedMode::fast,
            RotarySpeedMode::brake
        };
        constexpr std::array<float, 5> mixes {
            0.0f, 0.25f, 0.50f, 0.75f, 1.0f
        };
        constexpr std::size_t warmupSamples =
            static_cast<std::size_t> (sampleRate * 10.0);
        constexpr std::size_t segmentSamples =
            static_cast<std::size_t> (sampleRate * 3.0);
        constexpr std::size_t settleSamples =
            static_cast<std::size_t> (sampleRate * 1.0);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        auto absoluteSample = std::uint64_t { 0 };
        auto peak = 0.0f;
        auto maximumJump = 0.0f;
        auto previousLeft = 0.0f;
        auto previousRight = 0.0f;
        auto firstOutput = true;
        auto finite = true;
        auto outputSum = 0.0;
        auto outputSamples = std::uint64_t { 0 };
        auto worstAbsoluteDb = 0.0;

        const auto renderRange =
            [&] (std::size_t totalSamples,
                 std::size_t measurementStart,
                 double& outputEnergy,
                 double& referenceEnergy)
            {
                outputEnergy = 0.0;
                referenceEnergy = 0.0;

                for (std::size_t offset = 0;
                     offset < totalSamples;
                     offset += blockSize)
                {
                    const auto count = std::min (
                        blockSize, totalSamples - offset);
                    for (std::size_t sample = 0;
                         sample < count;
                         ++sample)
                    {
                        left[sample] = rotaryProbeSample (
                            absoluteSample
                                + static_cast<std::uint64_t> (sample),
                            false);
                        right[sample] = rotaryProbeSample (
                            absoluteSample
                                + static_cast<std::uint64_t> (sample),
                            true);
                    }

                    rotary.process (
                        left.data(),
                        stereo ? right.data() : nullptr,
                        count);

                    for (std::size_t sample = 0;
                         sample < count;
                         ++sample)
                    {
                        const auto outLeft = left[sample];
                        const auto outRight =
                            stereo ? right[sample] : outLeft;
                        finite = finite
                            && std::isfinite (outLeft)
                            && std::isfinite (outRight);
                        peak = std::max (
                            peak,
                            std::max (
                                std::abs (outLeft),
                                std::abs (outRight)));
                        if (! firstOutput)
                        {
                            maximumJump = std::max (
                                maximumJump,
                                std::abs (outLeft - previousLeft));
                            maximumJump = std::max (
                                maximumJump,
                                std::abs (outRight - previousRight));
                        }
                        firstOutput = false;
                        previousLeft = outLeft;
                        previousRight = outRight;
                        outputSum += static_cast<double> (outLeft);
                        if (stereo)
                            outputSum += static_cast<double> (outRight);
                        outputSamples += stereo ? 2U : 1U;

                        const auto localIndex = offset + sample;
                        if (localIndex >= measurementStart)
                        {
                            const auto dryLeft =
                                0.90f * rotaryProbeSample (
                                    absoluteSample
                                        + static_cast<std::uint64_t> (
                                            sample),
                                    false);
                            const auto dryRight = stereo
                                ? 0.90f * rotaryProbeSample (
                                    absoluteSample
                                        + static_cast<std::uint64_t> (
                                            sample),
                                    true)
                                : dryLeft;
                            outputEnergy += 0.5
                                * (static_cast<double> (outLeft)
                                       * outLeft
                                   + static_cast<double> (outRight)
                                       * outRight);
                            referenceEnergy += 0.5
                                * (static_cast<double> (dryLeft)
                                       * dryLeft
                                   + static_cast<double> (dryRight)
                                       * dryRight);
                        }
                    }
                    absoluteSample += count;
                }
            };

        double ignoredOutput = 0.0;
        double ignoredReference = 0.0;
        renderRange (
            warmupSamples,
            warmupSamples,
            ignoredOutput,
            ignoredReference);

        for (const auto mode : modes)
        {
            parameters.speedMode = mode;
            for (const auto mix : mixes)
            {
                parameters.mix = mix;
                rotary.setParameters (parameters);

                double outputEnergy = 0.0;
                double referenceEnergy = 0.0;
                renderRange (
                    segmentSamples,
                    settleSamples,
                    outputEnergy,
                    referenceEnergy);

                const auto db = 10.0 * std::log10 (
                    std::max (outputEnergy, 1.0e-20)
                    / std::max (referenceEnergy, 1.0e-20));
                worstAbsoluteDb = std::max (
                    worstAbsoluteDb, std::abs (db));
                context.expect (
                    std::isfinite (db)
                        && db >= -1.0
                        && db <= 1.0,
                    stereo
                        ? "stereo rotary mix remains within +/-1 dB"
                        : "mono rotary mix remains within +/-1 dB");
            }
        }

        const auto mean = outputSamples > 0
            ? outputSum / static_cast<double> (outputSamples)
            : 0.0;
        const auto diagnostics = rotary.diagnostics();
        std::cout
            << (stereo ? "rotary_stereo_60s" : "rotary_mono_60s")
            << " worst_db=" << worstAbsoluteDb
            << " peak=" << peak
            << " dc=" << mean
            << " max_jump=" << maximumJump
            << " wet_cal=" << diagnostics.wetCalibrationGain
            << " correlation=" << diagnostics.dryWetCorrelation
            << '\n';
        context.expect (
            finite,
            stereo
                ? "stereo 60 s sweep has no NaN or Inf"
                : "mono 60 s sweep has no NaN or Inf");
        context.expect (
            peak < 0.996f,
            stereo
                ? "stereo 60 s sweep keeps a safe peak"
                : "mono 60 s sweep keeps a safe peak");
        context.expect (
            std::abs (mean) < 0.0015,
            stereo
                ? "stereo 60 s sweep has no material DC"
                : "mono 60 s sweep has no material DC");
        context.expect (
            maximumJump < 0.20f,
            stereo
                ? "stereo mode/mix changes remain click free"
                : "mono mode/mix changes remain click free");
    }

    void testNoRenderAllocations (TestContext& context)
    {
        churchorgan::ScannerVibrato scanner;
        churchorgan::ScannerParameters scannerParameters;
        scannerParameters.mode =
            churchorgan::ScannerMode::chorus2;
        scanner.setParameters (scannerParameters);
        context.expect (
            scanner.prepare ({ sampleRate, 256, 1U, 0.0f }),
            "allocation scanner prepares");

        churchorgan::OrganAmpCab amp;
        context.expect (
            amp.prepare ({ sampleRate, 256,
                           churchorgan::OrganAmpOversampling::x8 }),
            "allocation amp prepares");

        churchorgan::RotaryCabinet rotary;
        context.expect (
            rotary.prepare ({ sampleRate, 256, 0.0f, 0.5f }),
            "allocation rotary prepares");

        std::array<float, 256> left {};
        std::array<float, 256> right {};
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            left[index] =
                0.3f
                * std::sin (
                    twoPi * 440.0f
                    * static_cast<float> (index)
                    / static_cast<float> (sampleRate));
            right[index] = left[index];
        }

        const auto before =
            allocationCount.load (std::memory_order_relaxed);
        trackAllocations.store (true, std::memory_order_release);
        scanner.process (left.data(), right.data(), left.size());
        amp.process (left.data(), right.data(), left.size());
        rotary.process (left.data(), right.data(), left.size());
        trackAllocations.store (false, std::memory_order_release);
        const auto after =
            allocationCount.load (std::memory_order_relaxed);

        context.expect (
            after == before,
            "scanner + amp + rotary allocate zero bytes in render");
    }

    void testSampleRateAndInvalidInputMatrix (TestContext& context)
    {
        constexpr std::array<double, 5> sampleRates {
            44100.0, 48000.0, 96000.0, 192000.0, 384000.0
        };

        for (const auto rate : sampleRates)
        {
            churchorgan::ScannerVibrato scanner;
            churchorgan::ScannerParameters scannerParameters;
            scannerParameters.mode =
                churchorgan::ScannerMode::vibrato3;
            scannerParameters.rate =
                std::numeric_limits<float>::infinity();
            scanner.setParameters (scannerParameters);

            churchorgan::OrganAmpCab amp;
            churchorgan::OrganAmpParameters ampParameters;
            ampParameters.profile =
                churchorgan::OrganAmpProfile::worshipSmooth;
            ampParameters.output = 0.9f;
            amp.setParameters (ampParameters);

            churchorgan::RotaryCabinet rotary;
            churchorgan::RotaryParameters rotaryParameters;
            rotaryParameters.speedMode =
                churchorgan::RotarySpeedMode::fast;
            rotaryParameters.crossoverHz =
                std::numeric_limits<float>::quiet_NaN();
            rotary.setParameters (rotaryParameters);

            const auto prepared =
                scanner.prepare ({ rate, 257, 7U, 0.4f })
                && amp.prepare (
                    { rate, 257,
                      rate >= 192000.0
                          ? churchorgan::OrganAmpOversampling::x4
                          : churchorgan::OrganAmpOversampling::x2 })
                && rotary.prepare (
                    { rate, 257, 0.2f, 0.7f });
            context.expect (
                prepared,
                "motion chain prepares across sample-rate matrix");

            std::array<float, 257> left {};
            std::array<float, 257> right {};
            for (std::size_t index = 0;
                 index < left.size();
                 ++index)
            {
                left[index] =
                    0.25f
                    * std::sin (
                        twoPi * 330.0f
                        * static_cast<float> (index)
                        / static_cast<float> (rate));
                right[index] = left[index];
            }
            left[9] =
                std::numeric_limits<float>::quiet_NaN();
            right[33] =
                std::numeric_limits<float>::infinity();

            scanner.process (
                left.data(), right.data(), left.size());
            amp.process (
                left.data(), right.data(), left.size());
            rotary.process (
                left.data(), right.data(), left.size());

            const auto finite = std::all_of (
                left.begin(),
                left.end(),
                [] (float value) { return std::isfinite (value); })
                && std::all_of (
                    right.begin(),
                    right.end(),
                    [] (float value)
                    {
                        return std::isfinite (value);
                    });
            context.expect (
                finite,
                "motion chain sanitises invalid input across rates");
        }
    }
}

int main()
{
    TestContext context;

    testPrepareValidation (context);
    testScannerBypassAndModulation (context);
    testAmpCharacterAndProtection (context);
    testAmpOversamplingAliasReduction (context);
    testRotaryGeometryAndRamps (context);
    testRotaryDeterminismAndMonoPerspective (context);
    testRotaryLongMixLoudness (context, false);
    testRotaryLongMixLoudness (context, true);
    testNoRenderAllocations (context);
    testSampleRateAndInvalidInputMatrix (context);

    if (context.failures != 0)
    {
        std::cerr
            << "CHURCH_ORGAN_MOTION_TESTS_FAILED checks="
            << context.checks
            << " failures=" << context.failures
            << '\n';
        return 1;
    }

    std::cout
        << "CHURCH_ORGAN_MOTION_TESTS_OK checks="
        << context.checks
        << " allocations="
        << allocationCount.load (std::memory_order_relaxed)
        << '\n';
    return 0;
}
