#include "AutoMixContext.h"
#include "WorshipLayerEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
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

    struct TestContext
    {
        void expect (bool condition, const char* description)
        {
            ++checks;
            if (! condition)
            {
                ++failures;
                std::cerr << "FAIL: " << description << '\n';
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

    struct RenderedAudio
    {
        std::vector<float> left;
        std::vector<float> right;
        std::vector<float> sendLeft;
        std::vector<float> sendRight;
    };

    bool finite (
        const std::vector<float>& values)
    {
        return std::all_of (
            values.begin(),
            values.end(),
            [] (float value) { return std::isfinite (value); });
    }

    float peak (
        const std::vector<float>& values)
    {
        auto result = 0.0f;
        for (const auto value : values)
            result = std::max (result, std::abs (value));
        return result;
    }

    float rms (
        const std::vector<float>& values,
        std::size_t begin = 0,
        std::size_t end = 0)
    {
        if (end == 0 || end > values.size())
            end = values.size();
        if (begin >= end)
            return 0.0f;

        auto sum = 0.0;
        for (auto index = begin; index < end; ++index)
        {
            const auto value =
                static_cast<double> (values[index]);
            sum += value * value;
        }
        return static_cast<float> (
            std::sqrt (
                sum / static_cast<double> (end - begin)));
    }

    float stereoRms (
        const RenderedAudio& audio,
        std::size_t begin = 0)
    {
        const auto left = rms (audio.left, begin);
        const auto right = rms (audio.right, begin);
        return std::sqrt (
            0.5f * (left * left + right * right));
    }

    RenderedAudio render (
        churchorgan::WorshipLayerEngine& engine,
        std::size_t samples)
    {
        RenderedAudio audio;
        audio.left.resize (samples);
        audio.right.resize (samples);
        audio.sendLeft.resize (samples);
        audio.sendRight.resize (samples);

        for (std::size_t offset = 0;
             offset < samples;
             offset += blockSize)
        {
            const auto count =
                std::min (blockSize, samples - offset);
            engine.renderAdd ({
                audio.left.data() + offset,
                audio.right.data() + offset,
                audio.sendLeft.data() + offset,
                audio.sendRight.data() + offset,
                count
            });
        }
        return audio;
    }

    void configureSingleLayer (
        churchorgan::WorshipLayerEngine& engine,
        churchorgan::WorshipLayer selected,
        float attackMilliseconds,
        float releaseMilliseconds)
    {
        for (std::size_t index = 0;
             index < churchorgan::worshipLayerCount;
             ++index)
        {
            const auto layer =
                static_cast<churchorgan::WorshipLayer> (index);
            auto parameters =
                churchorgan::WorshipLayerEngine::
                    defaultParameters (layer);
            parameters.enabled = layer == selected;
            if (parameters.enabled)
            {
                parameters.level = 0.9f;
                parameters.attackMilliseconds =
                    attackMilliseconds;
                parameters.releaseMilliseconds =
                    releaseMilliseconds;
                parameters.lowestNote = 0;
                parameters.highestNote = 127;
                parameters.highPassHz = 20.0f;
                parameters.lowPassHz = 19000.0f;
                parameters.reverbSend = 0.5f;
            }
            engine.setLayerParameters (layer, parameters);
        }
        engine.setWorshipIntensity (1.0f);
        engine.setOutputGain (0.82f);
    }

    void queueChord (
        churchorgan::WorshipLayerEngine& engine,
        std::uint32_t offset = 0)
    {
        constexpr std::array<int, 5> chord {
            48, 55, 60, 64, 67
        };
        for (const auto note : chord)
            engine.queueNoteOn (note, 104, offset);
    }

    void testPrepareAndQualityLimits (TestContext& context)
    {
        churchorgan::WorshipLayerEngine invalid;
        churchorgan::WorshipPrepareSpec invalidSpec;
        invalidSpec.sampleRate =
            std::numeric_limits<double>::quiet_NaN();
        context.expect (
            ! invalid.prepare (invalidSpec),
            "worship engine rejects non-finite sample rate");

        constexpr std::array<
            churchorgan::WorshipQuality,
            3> qualities {
                churchorgan::WorshipQuality::eco,
                churchorgan::WorshipQuality::standard,
                churchorgan::WorshipQuality::ultra
            };
        constexpr std::array<std::size_t, 3> expected {
            16, 32, 48
        };

        for (std::size_t index = 0;
             index < qualities.size();
             ++index)
        {
            churchorgan::WorshipLayerEngine engine;
            context.expect (
                engine.prepare ({
                    sampleRate,
                    blockSize,
                    qualities[index],
                    91u
                }),
                "quality profile prepares");
            context.expect (
                engine.voiceLimit() == expected[index],
                "quality profile exposes fixed voice limit");
        }

        churchorgan::AutoMixContext autoMix;
        context.expect (
            ! autoMix.prepare ({ 1000.0, blockSize }),
            "auto mix rejects unsupported sample rate");
        context.expect (
            autoMix.prepare ({ sampleRate, blockSize }),
            "auto mix prepares at 48 kHz");
        context.expect (
            ! churchorgan::AutoMixContext::usesAudioBuffers,
            "auto mix contract cannot consume audio buffers");
    }

    void testSampleAccurateOffsets (TestContext& context)
    {
        churchorgan::WorshipLayerEngine engine;
        configureSingleLayer (
            engine,
            churchorgan::WorshipLayer::octave,
            0.5f,
            80.0f);
        context.expect (
            engine.prepare ({
                sampleRate,
                256,
                churchorgan::WorshipQuality::standard,
                0x1234u
            }),
            "offset test prepares");
        engine.queueNoteOn (60, 127, 37);
        engine.queueNoteOff (60, 93);

        std::array<float, 256> left {};
        std::array<float, 256> right {};
        engine.renderAdd ({
            left.data(),
            right.data(),
            nullptr,
            nullptr,
            left.size()
        });

        auto beforePeak = 0.0f;
        auto activePeak = 0.0f;
        auto releasePeak = 0.0f;
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (index < 37)
                beforePeak =
                    std::max (beforePeak, std::abs (left[index]));
            else if (index < 93)
                activePeak =
                    std::max (activePeak, std::abs (left[index]));
            else
                releasePeak =
                    std::max (releasePeak, std::abs (left[index]));
        }
        context.expectNear (
            beforePeak,
            0.0f,
            0.0f,
            "note-on offset leaves all earlier samples untouched");
        context.expect (
            activePeak > 1.0e-5f,
            "note-on begins inside its requested sample range");
        context.expect (
            releasePeak > 1.0e-5f,
            "note-off begins a tail instead of cutting audio");

        churchorgan::WorshipLayerEngine carried;
        configureSingleLayer (
            carried,
            churchorgan::WorshipLayer::pad,
            0.5f,
            100.0f);
        carried.prepare ({
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::eco,
            5u
        });
        carried.queueNoteOn (60, 100, 200);
        std::array<float, blockSize> first {};
        std::array<float, blockSize> second {};
        carried.renderAdd ({
            first.data(), nullptr, nullptr, nullptr, first.size()
        });
        carried.renderAdd ({
            second.data(), nullptr, nullptr, nullptr, second.size()
        });
        auto earlySecond = 0.0f;
        auto lateSecond = 0.0f;
        for (std::size_t index = 0; index < second.size(); ++index)
        {
            if (index < 72)
                earlySecond = std::max (
                    earlySecond, std::abs (second[index]));
            else
                lateSecond = std::max (
                    lateSecond, std::abs (second[index]));
        }
        context.expect (
            peak (std::vector<float> (
                first.begin(), first.end())) == 0.0f
                && earlySecond == 0.0f
                && lateSecond > 0.0f,
            "future event offset carries exactly into the next block");
    }

    void testDeterminism (TestContext& context)
    {
        churchorgan::WorshipLayerEngine first;
        churchorgan::WorshipLayerEngine second;
        first.setWorshipIntensity (0.78f);
        second.setWorshipIntensity (0.78f);
        const churchorgan::WorshipPrepareSpec spec {
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::standard,
            0x4142434445464748ULL
        };
        context.expect (
            first.prepare (spec) && second.prepare (spec),
            "deterministic engine pair prepares");
        queueChord (first, 11);
        queueChord (second, 11);
        first.queueExpression (0.72f, 79);
        second.queueExpression (0.72f, 79);
        first.queueNoteOff (60, 113);
        second.queueNoteOff (60, 113);

        const auto firstAudio = render (
            first,
            static_cast<std::size_t> (sampleRate * 1.2));
        const auto secondAudio = render (
            second,
            static_cast<std::size_t> (sampleRate * 1.2));

        context.expect (
            firstAudio.left == secondAudio.left
                && firstAudio.right == secondAudio.right
                && firstAudio.sendLeft == secondAudio.sendLeft
                && firstAudio.sendRight == secondAudio.sendRight,
            "equal seed/events render bit-identically");
    }

    void testIntensityCurvesAndAudio (TestContext& context)
    {
        auto previous =
            churchorgan::WorshipLayerEngine::intensityProfile (
                0.0f);
        for (int step = 1; step <= 200; ++step)
        {
            const auto intensity =
                static_cast<float> (step) / 200.0f;
            const auto current =
                churchorgan::WorshipLayerEngine::
                    intensityProfile (intensity);

            for (std::size_t layer = 0;
                 layer < churchorgan::worshipLayerCount;
                 ++layer)
            {
                context.expect (
                    current.layerGain[layer] + 1.0e-7f
                        >= previous.layerGain[layer],
                    "every intensity layer curve is monotonic");
            }
            context.expect (
                current.spectralOpening + 1.0e-7f
                    >= previous.spectralOpening
                    && current.stereoExpansion + 1.0e-7f
                        >= previous.stereoExpansion
                    && current.reverbExpansion + 1.0e-7f
                        >= previous.reverbExpansion,
                "intensity spectral/spatial curves are monotonic");
            context.expect (
                current.loudnessCompensation
                    <= previous.loudnessCompensation + 1.0e-7f,
                "intensity headroom compensation never increases");
            context.expect (
                current.perceptualEnergyTarget + 1.0e-7f
                    >= previous.perceptualEnergyTarget,
                "compensated perceptual energy remains monotonic");
            previous = current;
        }

        constexpr std::array<float, 5> intensities {
            0.15f, 0.35f, 0.55f, 0.78f, 1.0f
        };
        std::array<float, intensities.size()> measured {};

        for (std::size_t index = 0;
             index < intensities.size();
             ++index)
        {
            churchorgan::WorshipLayerEngine engine;
            engine.setWorshipIntensity (intensities[index]);
            engine.prepare ({
                sampleRate,
                blockSize,
                churchorgan::WorshipQuality::standard,
                0x90u
            });
            queueChord (engine);
            const auto audio = render (
                engine,
                static_cast<std::size_t> (sampleRate * 1.5));
            measured[index] = stereoRms (
                audio,
                static_cast<std::size_t> (sampleRate * 0.8));
            if (index > 0)
            {
                context.expect (
                    measured[index]
                        >= measured[index - 1] * 0.995f,
                    "rendered intensity has no perceptual loudness dip");
            }
        }

        std::cout
            << "intensity_rms"
            << " i15=" << measured[0]
            << " i35=" << measured[1]
            << " i55=" << measured[2]
            << " i78=" << measured[3]
            << " i100=" << measured[4]
            << '\n';
    }

    void testHeadroomSpectrumAndKeyRange (
        TestContext& context)
    {
        churchorgan::WorshipLayerEngine engine;
        engine.setWorshipIntensity (1.0f);
        engine.setOutputGain (1.0f);
        for (std::size_t index = 0;
             index < churchorgan::worshipLayerCount;
             ++index)
        {
            const auto layer =
                static_cast<churchorgan::WorshipLayer> (index);
            auto parameters =
                churchorgan::WorshipLayerEngine::
                    defaultParameters (layer);
            parameters.level = 1.2f;
            parameters.attackMilliseconds = 1.0f;
            parameters.releaseMilliseconds = 500.0f;
            engine.setLayerParameters (layer, parameters);
        }
        context.expect (
            engine.prepare ({
                sampleRate,
                blockSize,
                churchorgan::WorshipQuality::ultra,
                0xaabbccu
            }),
            "headroom stress engine prepares");

        for (int note = 36; note < 84; ++note)
            engine.queueNoteOn (note, 127, 0);
        const auto audio = render (
            engine,
            static_cast<std::size_t> (sampleRate * 1.0));
        const auto layerPeak =
            std::max (peak (audio.left), peak (audio.right));
        const auto sendPeak =
            std::max (
                peak (audio.sendLeft),
                peak (audio.sendRight));
        context.expect (
            finite (audio.left)
                && finite (audio.right)
                && finite (audio.sendLeft)
                && finite (audio.sendRight),
            "stress render remains finite");
        context.expect (
            layerPeak < 0.681f && sendPeak < 0.501f,
            "layer and reverb send retain declared headroom");

        auto mean = 0.0;
        auto differenceEnergy = 0.0;
        auto signalEnergy = 0.0;
        for (std::size_t index = 1;
             index < audio.left.size();
             ++index)
        {
            const auto value =
                static_cast<double> (audio.left[index]);
            const auto difference =
                value
                - static_cast<double> (audio.left[index - 1]);
            mean += value;
            signalEnergy += value * value;
            differenceEnergy += difference * difference;
        }
        mean /= static_cast<double> (audio.left.size() - 1);
        const auto spectralRatio =
            static_cast<float> (
                std::sqrt (
                    differenceEnergy
                    / std::max (signalEnergy, 1.0e-20)));
        context.expect (
            std::abs (mean) < 0.01,
            "worship synthesis has controlled DC");
        context.expect (
            spectralRatio > 0.01f && spectralRatio < 1.8f,
            "worship synthesis contains band-limited spectral motion");

        const auto diagnostics = engine.diagnostics();
        context.expect (
            diagnostics.heldVoices == 48
                && diagnostics.stolenVoices == 0,
            "ultra voice pool holds its fixed 48-note limit");

        churchorgan::WorshipLayerEngine rangeEngine;
        configureSingleLayer (
            rangeEngine,
            churchorgan::WorshipLayer::strings,
            0.5f,
            60.0f);
        auto rangeParameters = rangeEngine.layerParameters (
            churchorgan::WorshipLayer::strings);
        rangeParameters.lowestNote = 60;
        rangeParameters.highestNote = 60;
        rangeEngine.setLayerParameters (
            churchorgan::WorshipLayer::strings,
            rangeParameters);
        rangeEngine.prepare ({
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::eco,
            12u
        });
        rangeEngine.queueNoteOn (61, 127, 0);
        const auto outside = render (rangeEngine, 2048);
        context.expect (
            peak (outside.left) == 0.0f,
            "per-layer key range excludes notes exactly");
        rangeEngine.queueNoteOn (60, 127, 0);
        const auto inside = render (rangeEngine, 2048);
        context.expect (
            peak (inside.left) > 1.0e-5f,
            "per-layer key range includes configured note");
    }

    void testLayerComponentsAndPerformanceControls (
        TestContext& context)
    {
        for (std::size_t index = 0;
             index < churchorgan::worshipLayerCount;
             ++index)
        {
            churchorgan::WorshipLayerEngine engine;
            const auto layer =
                static_cast<churchorgan::WorshipLayer> (index);
            configureSingleLayer (engine, layer, 1.0f, 120.0f);
            engine.prepare ({
                sampleRate,
                blockSize,
                churchorgan::WorshipQuality::standard,
                0x500u + index
            });
            engine.queueNoteOn (60, 110, 0);
            const auto audio = render (engine, 4096);
            context.expect (
                peak (audio.left) > 1.0e-6f
                    && peak (audio.sendLeft) > 1.0e-7f,
                "every original worship component renders and sends");
        }

        auto renderControlled = [] (
                                    int velocity,
                                    float expression,
                                    float ducking,
                                    float autoMixGain)
        {
            churchorgan::WorshipLayerEngine engine;
            configureSingleLayer (
                engine,
                churchorgan::WorshipLayer::pad,
                4.0f,
                160.0f);
            auto parameters = engine.layerParameters (
                churchorgan::WorshipLayer::pad);
            parameters.velocityResponse = 1.0f;
            parameters.expressionResponse = 1.0f;
            parameters.ducking = 1.0f;
            engine.setLayerParameters (
                churchorgan::WorshipLayer::pad,
                parameters);
            engine.setExternalDucking (ducking);
            engine.setAutoMixLayerGain (autoMixGain);
            engine.prepare ({
                sampleRate,
                blockSize,
                churchorgan::WorshipQuality::eco,
                0x8811u
            });
            engine.queueExpression (expression, 0);
            engine.queueNoteOn (60, velocity, 0);
            return render (
                engine,
                static_cast<std::size_t> (sampleRate * 0.5));
        };

        const auto full = renderControlled (127, 1.0f, 0.0f, 1.0f);
        const auto soft = renderControlled (32, 1.0f, 0.0f, 1.0f);
        const auto expressionLow =
            renderControlled (127, 0.22f, 0.0f, 1.0f);
        const auto ducked =
            renderControlled (127, 1.0f, 1.0f, 0.50f);
        const auto analysisStart =
            static_cast<std::size_t> (sampleRate * 0.2);
        const auto fullRms = stereoRms (full, analysisStart);
        context.expect (
            stereoRms (soft, analysisStart) < fullRms * 0.40f,
            "velocity response controls layer contribution");
        context.expect (
            stereoRms (expressionLow, analysisStart)
                < fullRms * 0.35f,
            "sample-accurate expression controls layer contribution");
        context.expect (
            stereoRms (ducked, analysisStart) < fullRms * 0.40f,
            "ducking and auto-mix gains attenuate the internal layer");

        churchorgan::WorshipLayerEngine bloom;
        configureSingleLayer (
            bloom,
            churchorgan::WorshipLayer::reverseBloom,
            520.0f,
            800.0f);
        bloom.prepare ({
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::standard,
            0x719u
        });
        bloom.queueNoteOn (60, 110, 0);
        const auto bloomAudio = render (
            bloom,
            static_cast<std::size_t> (sampleRate * 1.5));
        const auto early = rms (
            bloomAudio.left,
            static_cast<std::size_t> (sampleRate * 0.05),
            static_cast<std::size_t> (sampleRate * 0.20));
        const auto late = rms (
            bloomAudio.left,
            static_cast<std::size_t> (sampleRate * 1.15),
            static_cast<std::size_t> (sampleRate * 1.40));
        context.expect (
            late > early * 3.0f,
            "reverse bloom grows naturally from its continuous envelope");
    }

    void testSampleRateMatrix (TestContext& context)
    {
        constexpr std::array<double, 4> rates {
            44100.0, 48000.0, 96000.0, 192000.0
        };
        for (const auto rate : rates)
        {
            churchorgan::WorshipLayerEngine engine;
            engine.setWorshipIntensity (0.74f);
            const auto prepared = engine.prepare ({
                rate,
                257,
                churchorgan::WorshipQuality::eco,
                0x818u
            });
            context.expect (
                prepared,
                "worship engine prepares across sample-rate matrix");
            engine.queueNoteOn (60, 100, 17);
            std::array<float, 257> left {};
            std::array<float, 257> right {};
            std::array<float, 257> send {};
            left[3] =
                std::numeric_limits<float>::quiet_NaN();
            engine.renderAdd ({
                left.data(),
                right.data(),
                send.data(),
                nullptr,
                left.size()
            });
            const auto allFinite =
                std::all_of (
                    left.begin(),
                    left.end(),
                    [] (float value)
                    {
                        return std::isfinite (value);
                    })
                && std::all_of (
                    right.begin(),
                    right.end(),
                    [] (float value)
                    {
                        return std::isfinite (value);
                    })
                && std::all_of (
                    send.begin(),
                    send.end(),
                    [] (float value)
                    {
                        return std::isfinite (value);
                    });
            context.expect (
                allFinite,
                "worship render sanitises buffers across sample rates");
        }
    }

    void testSeamlessRelease (TestContext& context)
    {
        churchorgan::WorshipLayerEngine engine;
        configureSingleLayer (
            engine,
            churchorgan::WorshipLayer::pad,
            8.0f,
            380.0f);
        engine.prepare ({
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::eco,
            0x777u
        });
        engine.queueNoteOn (60, 110, 0);
        const auto held = render (
            engine,
            static_cast<std::size_t> (sampleRate * 0.6));
        const auto previousSample = held.left.back();
        engine.queueNoteOff (60, 0);
        const auto release = render (
            engine,
            static_cast<std::size_t> (sampleRate * 5.0));

        const auto boundaryJump =
            std::abs (release.left.front() - previousSample);
        const auto earlyRms = rms (
            release.left,
            0,
            static_cast<std::size_t> (sampleRate * 0.15));
        const auto lateRms = rms (
            release.left,
            release.left.size()
                - static_cast<std::size_t> (sampleRate * 0.25));
        context.expect (
            boundaryJump < 0.08f,
            "note release is continuous across the block boundary");
        context.expect (
            earlyRms > lateRms * 20.0f
                && earlyRms > 1.0e-4f,
            "release tail decays smoothly instead of being cut");
        context.expect (
            engine.diagnostics().tailVoices == 0,
            "completed release returns its fixed voice slot");
    }

    void testAutoMixBoundsAndPumping (TestContext& context)
    {
        churchorgan::AutoMixContext autoMix;
        churchorgan::AutoMixParameters parameters;
        parameters.amount = 1.0f;
        parameters.reactionMilliseconds = 250.0f;
        parameters.recoveryMilliseconds = 800.0f;
        parameters.maximumAttenuationDecibels = 3.0f;
        parameters.soloRoomLiftDecibels = 0.6f;
        autoMix.setParameters (parameters);
        context.expect (
            autoMix.prepare ({ sampleRate, blockSize }),
            "auto mix stress profile prepares");

        autoMix.setControlFrame ({ 1.0f, 1.0f, 1.0f, 1.0f });
        const auto firstBlock = autoMix.advance (blockSize);
        context.expect (
            firstBlock.output > 0.995f,
            "auto mix cannot react abruptly in one audio block");

        auto gains = firstBlock;
        const auto settleBlocks =
            static_cast<std::size_t> (
                sampleRate * 8.0 / blockSize);
        for (std::size_t block = 0;
             block < settleBlocks;
             ++block)
        {
            gains = autoMix.advance (blockSize);
        }
        const auto minimumGain =
            churchorgan::worshipdsp::decibelsToGain (-3.0f);
        context.expect (
            gains.worshipLayer >= minimumGain
                && gains.organLowMid >= minimumGain
                && gains.organMid >= minimumGain
                && gains.drive >= minimumGain
                && gains.output >= minimumGain,
            "auto mix never exceeds maximum attenuation");
        context.expect (
            gains.worshipLayer < 0.9f
                && gains.output < 0.9f,
            "sustained conflicts produce conservative attenuation");
        context.expect (
            gains.room <= 1.001f,
            "room lift is suppressed while other sources are active");

        autoMix.reset();
        auto maximumBlockDelta = 0.0f;
        auto previousOutput = autoMix.currentGains().output;
        for (int block = 0; block < 600; ++block)
        {
            const auto value = block % 2 == 0 ? 1.0f : 0.0f;
            autoMix.setControlFrame ({
                value, value, value, value
            });
            const auto current = autoMix.advance (blockSize);
            maximumBlockDelta = std::max (
                maximumBlockDelta,
                std::abs (current.output - previousOutput));
            previousOutput = current.output;
        }
        context.expect (
            maximumBlockDelta < 0.002f,
            "rapid control toggles cannot create block-rate pumping");

        autoMix.setControlFrame ({});
        for (std::size_t block = 0;
             block < settleBlocks;
             ++block)
        {
            gains = autoMix.advance (blockSize);
        }
        context.expect (
            gains.output > 0.995f
                && gains.worshipLayer > 0.995f
                && gains.room > 1.0f,
            "auto mix restores gain slowly and adds bounded solo room");
    }

    void testQueueCapacityAndNoAllocations (
        TestContext& context)
    {
        churchorgan::WorshipLayerEngine overflow;
        overflow.prepare ({
            sampleRate,
            blockSize,
            churchorgan::WorshipQuality::eco,
            1u
        });
        auto accepted = std::size_t { 0 };
        for (std::size_t index = 0;
             index < churchorgan::maximumWorshipEvents + 1;
             ++index)
        {
            if (overflow.queueExpression (
                    static_cast<float> (index % 100) / 100.0f,
                    0))
            {
                ++accepted;
            }
        }
        std::array<float, 1> overflowAudio {};
        overflow.renderAdd ({
            overflowAudio.data(), nullptr, nullptr, nullptr, 1
        });
        context.expect (
            accepted == churchorgan::maximumWorshipEvents
                && overflow.diagnostics().droppedEvents == 1,
            "fixed event queue reports overflow without allocation");

        churchorgan::WorshipLayerEngine engine;
        engine.setWorshipIntensity (0.8f);
        context.expect (
            engine.prepare ({
                sampleRate,
                256,
                churchorgan::WorshipQuality::ultra,
                0x31u
            }),
            "allocation test engine prepares");
        queueChord (engine);

        churchorgan::AutoMixContext autoMix;
        context.expect (
            autoMix.prepare ({ sampleRate, 256 }),
            "allocation test auto mix prepares");
        autoMix.setControlFrame ({ 0.7f, 0.4f, 0.2f, 0.8f });

        std::array<float, 256> left {};
        std::array<float, 256> right {};
        std::array<float, 256> sendLeft {};
        std::array<float, 256> sendRight {};

        const auto before =
            allocationCount.load (std::memory_order_relaxed);
        trackAllocations.store (true, std::memory_order_release);
        const auto gains = autoMix.advance (left.size());
        engine.setAutoMixLayerGain (gains.worshipLayer);
        engine.renderAdd ({
            left.data(),
            right.data(),
            sendLeft.data(),
            sendRight.data(),
            left.size()
        });
        trackAllocations.store (false, std::memory_order_release);
        const auto after =
            allocationCount.load (std::memory_order_relaxed);

        context.expect (
            after == before,
            "worship render and auto mix allocate zero bytes");
        context.expect (
            std::all_of (
                left.begin(),
                left.end(),
                [] (float value) { return std::isfinite (value); }),
            "allocation render remains finite");
    }
}

int main()
{
    TestContext context;

    testPrepareAndQualityLimits (context);
    testSampleAccurateOffsets (context);
    testDeterminism (context);
    testIntensityCurvesAndAudio (context);
    testHeadroomSpectrumAndKeyRange (context);
    testLayerComponentsAndPerformanceControls (context);
    testSampleRateMatrix (context);
    testSeamlessRelease (context);
    testAutoMixBoundsAndPumping (context);
    testQueueCapacityAndNoAllocations (context);

    if (context.failures != 0)
    {
        std::cerr
            << "CHURCH_ORGAN_WORSHIP_TESTS_FAILED checks="
            << context.checks
            << " failures=" << context.failures
            << '\n';
        return 1;
    }

    std::cout
        << "CHURCH_ORGAN_WORSHIP_TESTS_OK checks="
        << context.checks
        << " allocations="
        << allocationCount.load (std::memory_order_relaxed)
        << '\n';
    return 0;
}
