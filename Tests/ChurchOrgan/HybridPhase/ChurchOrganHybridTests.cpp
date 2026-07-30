#include "WorshipHybridEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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
    using churchorgan::Manual;
    using churchorgan::OrganParameterId;
    using churchorgan::WorshipHybridEngine;
    using churchorgan::WorshipHybridQuality;

    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;
    constexpr std::uint64_t testSeed =
        0x0102030405060708ULL;

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

    struct Audio
    {
        std::vector<float> left;
        std::vector<float> right;
        std::vector<float> sendLeft;
        std::vector<float> sendRight;
    };

    bool finite (const std::vector<float>& values)
    {
        return std::all_of (
            values.begin(),
            values.end(),
            [] (float value)
            {
                return std::isfinite (value);
            });
    }

    float peak (
        const std::vector<float>& values,
        std::size_t begin = 0,
        std::size_t end = 0)
    {
        if (end == 0 || end > values.size())
            end = values.size();
        auto result = 0.0f;
        for (auto index = begin; index < end; ++index)
            result = std::max (
                result, std::abs (values[index]));
        return result;
    }

    float mean (
        const std::vector<float>& values,
        std::size_t begin = 0)
    {
        if (begin >= values.size())
            return 0.0f;
        auto sum = 0.0;
        for (auto index = begin; index < values.size(); ++index)
            sum += values[index];
        return static_cast<float> (
            sum / static_cast<double> (
                values.size() - begin));
    }

    std::unique_ptr<WorshipHybridEngine> makeEngine (
        double rate = sampleRate,
        std::size_t maximumBlock = blockSize,
        WorshipHybridQuality quality =
            WorshipHybridQuality::standard,
        std::uint64_t seed = testSeed)
    {
        auto engine =
            std::make_unique<WorshipHybridEngine>();
        if (! engine->prepare ({
                rate,
                maximumBlock,
                seed,
                quality
            }))
        {
            return {};
        }
        return engine;
    }

    void configureClean (
        WorshipHybridEngine& engine,
        bool fastWorship = true)
    {
        auto& parameters = engine.tonewheel().parameters();
        parameters.setPlain (OrganParameterId::leakage, 0.0f);
        parameters.setPlain (OrganParameterId::crosstalk, 0.0f);
        parameters.setPlain (OrganParameterId::hum, 0.0f);
        parameters.setPlain (OrganParameterId::keyClick, 0.0f);
        parameters.setPlain (OrganParameterId::contactSpread, 0.0f);
        parameters.setPlain (OrganParameterId::transformer, 0.0f);
        parameters.setPlain (OrganParameterId::drive, 0.0f);
        parameters.setPlain (OrganParameterId::rotaryMix, 0.0f);

        if (! fastWorship)
            return;

        for (std::size_t index = 0;
             index < churchorgan::worshipLayerCount;
             ++index)
        {
            const auto layer =
                static_cast<churchorgan::WorshipLayer> (index);
            auto requested =
                engine.worshipLayer().layerParameters (layer);
            requested.enabled =
                layer == churchorgan::WorshipLayer::pad;
            requested.level =
                requested.enabled ? 0.72f : 0.0f;
            requested.attackMilliseconds = 0.5f;
            requested.releaseMilliseconds = 180.0f;
            requested.reverbSend = 0.55f;
            engine.worshipLayer().setLayerParameters (
                layer, requested);
        }
        engine.setWorshipIntensity (0.82f);
    }

    bool renderBlock (
        WorshipHybridEngine& engine,
        float* left,
        float* right,
        float* sendLeft,
        float* sendRight,
        std::size_t samples)
    {
        return engine.render ({
            left,
            right,
            sendLeft,
            sendRight,
            samples
        });
    }

    Audio render (
        WorshipHybridEngine& engine,
        std::size_t samples,
        std::size_t requestedBlock = blockSize)
    {
        Audio audio;
        audio.left.resize (samples);
        audio.right.resize (samples);
        audio.sendLeft.resize (samples);
        audio.sendRight.resize (samples);

        for (std::size_t offset = 0;
             offset < samples;
             offset += requestedBlock)
        {
            const auto count =
                std::min (requestedBlock, samples - offset);
            if (! renderBlock (
                    engine,
                    audio.left.data() + offset,
                    audio.right.data() + offset,
                    audio.sendLeft.data() + offset,
                    audio.sendRight.data() + offset,
                    count))
            {
                audio.left.clear();
                return audio;
            }
        }
        return audio;
    }

    void warm (
        WorshipHybridEngine& engine,
        std::size_t samples = 4096)
    {
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        for (std::size_t rendered = 0;
             rendered < samples;
             rendered += blockSize)
        {
            const auto count =
                std::min (blockSize, samples - rendered);
            renderBlock (
                engine,
                left.data(),
                right.data(),
                nullptr,
                nullptr,
                count);
        }
    }

    void testPrepareAndProfiles (TestContext& test)
    {
        auto engine =
            std::make_unique<WorshipHybridEngine>();
        test.expect (
            ! engine->prepare ({
                std::numeric_limits<double>::quiet_NaN(),
                blockSize,
                testSeed,
                WorshipHybridQuality::standard
            }),
            "rejects non-finite sample rate");
        test.expect (
            ! engine->prepare ({
                16000.0,
                blockSize,
                testSeed,
                WorshipHybridQuality::standard
            }),
            "rejects unsupported low sample rate");
        test.expect (
            ! engine->prepare ({
                sampleRate,
                0,
                testSeed,
                WorshipHybridQuality::standard
            }),
            "rejects zero maximum block");
        test.expect (
            ! engine->prepare ({
                sampleRate,
                WorshipHybridEngine::maximumBlockCapacity + 1,
                testSeed,
                WorshipHybridQuality::standard
            }),
            "rejects oversized block");

        std::array<std::size_t, 3> limits {};
        for (std::size_t index = 0; index < limits.size(); ++index)
        {
            auto profile = makeEngine (
                sampleRate,
                blockSize,
                static_cast<WorshipHybridQuality> (index));
            test.expect (
                profile != nullptr,
                "prepares Eco, Standard and Ultra");
            if (profile)
            {
                limits[index] =
                    profile->worshipLayer().voiceLimit();
                test.expectNear (
                    profile->tonewheel().parameters().getPlain (
                        OrganParameterId::family),
                    3.0f,
                    0.0f,
                    "marks Tonewheel core as Hybrid family");
            }
        }
        test.expect (
            limits[0] < limits[1]
                && limits[1] < limits[2],
            "quality profiles increase Worship voice budget");
    }

    void testOffsetsBlocksAndTails (TestContext& test)
    {
        auto engine = makeEngine();
        test.expect (engine != nullptr, "offset engine prepares");
        if (! engine)
            return;
        configureClean (*engine);
        warm (*engine);

        test.expect (
            engine->queueNoteOn (
                Manual::upper, 60, 112, 37),
            "queues offset note-on");
        test.expect (
            engine->queueNoteOff (
                Manual::upper, 60, 93),
            "queues offset note-off");
        const auto audio = render (*engine, blockSize);
        test.expect (
            ! audio.left.empty(),
            "renders offset block");
        test.expect (
            peak (audio.left, 0, 37) < 1.0e-5f,
            "note-on is silent before exact offset");
        test.expect (
            peak (audio.left, 40, 93) > 1.0e-5f,
            "note-on produces audio after offset");
        test.expect (
            peak (audio.left, 94) > 1.0e-7f,
            "Worship release preserves tail after note-off");

        engine->reset();
        configureClean (*engine);
        warm (*engine);
        test.expect (
            engine->queueNoteOn (
                Manual::lower, 65, 100, 200),
            "queues event beyond one block");
        const auto first = render (*engine, blockSize);
        const auto second = render (*engine, blockSize);
        test.expect (
            peak (first.left) < 1.0e-5f,
            "future event does not leak into first block");
        test.expect (
            peak (second.left, 0, 72) < 1.0e-5f,
            "carried event remains silent before offset");
        test.expect (
            peak (second.left, 75) > 1.0e-5f,
            "carried event fires at next-block offset");

        engine->reset();
        configureClean (*engine);
        warm (*engine);
        test.expect (
            engine->queueNoteOn (
                Manual::upper, 67, 110, blockSize),
            "queues boundary event");
        const auto boundaryFirst = render (*engine, blockSize);
        const auto boundarySecond = render (*engine, blockSize);
        test.expect (
            peak (boundaryFirst.left) < 1.0e-5f,
            "event at block length waits for next block");
        test.expect (
            peak (boundarySecond.left, 3) > 1.0e-7f,
            "boundary event starts at next block sample zero");
    }

    void testMirroringSustainAndAllOff (TestContext& test)
    {
        auto engine = makeEngine();
        test.expect (
            engine != nullptr,
            "performance-state engine prepares");
        if (! engine)
            return;
        configureClean (*engine);

        test.expect (
            engine->queueNoteOn (
                Manual::upper, 60, 105, 0)
                && engine->queueNoteOn (
                    Manual::lower, 60, 96, 0),
            "same note queues on two mirrored manuals");
        test.expect (
            ! engine->setLayerManualMask (
                churchorgan::hybridMirrorUpper),
            "manual mask rejects pending performance event");
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 1,
            "two manuals reference one Worship voice");

        engine->queueNoteOff (Manual::upper, 60, 0);
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 1,
            "first manual release keeps shared note held");
        engine->queueNoteOff (Manual::lower, 60, 0);
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 0,
            "last manual release releases Worship note");
        test.expect (
            engine->diagnostics().worshipTailVoices > 0,
            "last manual release retains Worship tail");

        engine->reset();
        configureClean (*engine);
        engine->queueNoteOn (Manual::upper, 64, 108, 0);
        engine->queueSustain (Manual::upper, true, 24);
        engine->queueNoteOff (Manual::upper, 64, 48);
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 1,
            "sustain holds Worship note after physical release");
        test.expect (
            ! engine->setLayerManualMask (
                churchorgan::hybridMirrorUpper),
            "manual mask rejects sustained performance");
        engine->queueSustain (Manual::upper, false, 17);
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 0,
            "sustain-up releases Worship note sample-accurately");

        engine->reset();
        configureClean (*engine);
        engine->queueNoteOn (Manual::upper, 55, 100, 0);
        engine->queueNoteOn (Manual::lower, 62, 100, 0);
        engine->queueNoteOn (Manual::pedal, 36, 100, 0);
        render (*engine, blockSize);
        engine->queueExpression (41, 9);
        engine->queueAllNotesOffAll (37);
        render (*engine, blockSize);
        for (int releaseBlock = 0;
             releaseBlock < 10;
             ++releaseBlock)
        {
            render (*engine, blockSize);
        }
        const auto diagnostics = engine->diagnostics();
        test.expect (
            diagnostics.activeTonewheelNotes == 0,
            "all-notes-off clears all Tonewheel manuals");
        test.expect (
            diagnostics.activeWorshipVoices == 0,
            "all-notes-off clears mirrored Worship holds");
        test.expect (
            diagnostics.worshipTailVoices > 0,
            "all-notes-off does not truncate Worship tails");

        engine->reset();
        configureClean (*engine);
        test.expect (
            engine->setLayerManualMask (
                churchorgan::hybridMirrorPedal),
            "idle engine accepts pedal-only layer mask");
        engine->queueNoteOn (Manual::pedal, 43, 100, 0);
        render (*engine, blockSize);
        test.expect (
            engine->diagnostics().activeWorshipVoices == 1,
            "configured pedal manual mirrors to Worship");
    }

    void testDeterminismAndStateB (TestContext& test)
    {
        auto first = makeEngine();
        auto second = makeEngine();
        test.expect (
            first != nullptr && second != nullptr,
            "determinism pair prepares");
        if (! first || ! second)
            return;

        configureClean (*first);
        configureClean (*second);
        first->setWorshipIntensity (0.73f);
        second->setWorshipIntensity (0.73f);
        first->tonewheel().parameters().setPlain (
            OrganParameterId::masterGainDb, -3.0f);
        second->tonewheel().parameters().setPlain (
            OrganParameterId::masterGainDb, -3.0f);
        auto pad =
            first->worshipLayer().layerParameters (
                churchorgan::WorshipLayer::pad);
        pad.level = 0.61f;
        first->worshipLayer().setLayerParameters (
            churchorgan::WorshipLayer::pad, pad);
        second->worshipLayer().setLayerParameters (
            churchorgan::WorshipLayer::pad, pad);

        test.expect (
            first->prepareStateB(),
            "prepares silent state B");
        test.expect (
            first->stateBReady()
                && first->stateBTonewheel() != nullptr
                && first->stateBWorshipLayer() != nullptr
                && first->stateBAutoMix() != nullptr,
            "exposes all prepared B components");
        if (first->stateBTonewheel()
            && first->stateBWorshipLayer())
        {
            test.expectNear (
                first->stateBTonewheel()->parameters().getPlain (
                    OrganParameterId::masterGainDb),
                -3.0f,
                0.0f,
                "state B clones Tonewheel parameters");
            test.expectNear (
                first->stateBWorshipLayer()->layerParameters (
                    churchorgan::WorshipLayer::pad).level,
                0.61f,
                1.0e-6f,
                "state B clones Worship layer parameters");
        }

        const std::array<int, 5> notes {
            48, 55, 60, 64, 67
        };
        for (const auto note : notes)
        {
            first->queueNoteOn (
                Manual::upper, note, 104, 19);
            second->queueNoteOn (
                Manual::upper, note, 104, 19);
            first->queueNoteOff (
                Manual::upper, note, 401);
            second->queueNoteOff (
                Manual::upper, note, 401);
        }
        const auto firstAudio =
            render (*first, 1024);
        const auto secondAudio =
            render (*second, 1024);
        test.expect (
            firstAudio.left == secondAudio.left
                && firstAudio.right == secondAudio.right
                && firstAudio.sendLeft == secondAudio.sendLeft
                && firstAudio.sendRight == secondAudio.sendRight,
            "same seed and event stream are bit-deterministic");

        first->discardStateB();
        test.expect (
            ! first->stateBReady()
                && first->stateBTonewheel() == nullptr,
            "discard hides silent state B without swapping active");
    }

    void testRatesSafetyAndMono (TestContext& test)
    {
        const std::array<double, 4> rates {
            44100.0, 48000.0, 96000.0, 192000.0
        };
        for (const auto rate : rates)
        {
            auto engine = makeEngine (
                rate,
                257,
                WorshipHybridQuality::standard,
                testSeed);
            test.expect (
                engine != nullptr,
                "prepares supported production sample rate");
            if (! engine)
                continue;
            configureClean (*engine);
            engine->queueNoteOn (
                Manual::upper, 72, 118, 31);
            engine->queueNoteOff (
                Manual::upper, 72, 421);
            const auto audio = render (*engine, 1028, 257);
            test.expect (
                finite (audio.left)
                    && finite (audio.right)
                    && finite (audio.sendLeft)
                    && finite (audio.sendRight),
                "all sample rates render finite main and send");
            test.expect (
                peak (audio.left) < 0.985001f
                    && peak (audio.right) < 0.985001f,
                "main peak remains below safety ceiling");
            test.expect (
                peak (audio.sendLeft) < 0.500001f
                    && peak (audio.sendRight) < 0.500001f,
                "send peak remains below safety ceiling");
        }

        auto engine = makeEngine();
        test.expect (engine != nullptr, "safety engine prepares");
        if (! engine)
            return;
        configureClean (*engine);
        engine->setOutputGain (
            std::numeric_limits<float>::infinity());
        engine->setWorshipIntensity (
            std::numeric_limits<float>::quiet_NaN());
        engine->setOutputGain (1.5f);
        for (int note = 36; note <= 96; note += 3)
            engine->queueNoteOn (
                Manual::upper, note, 127, 0);

        auto audio = render (
            *engine,
            static_cast<std::size_t> (sampleRate * 1.5));
        test.expect (
            finite (audio.left)
                && finite (audio.right)
                && finite (audio.sendLeft)
                && finite (audio.sendRight),
            "stress render contains no NaN or infinity");
        test.expect (
            peak (audio.left) < 0.985001f
                && peak (audio.right) < 0.985001f,
            "stress main is peak protected");
        test.expect (
            peak (audio.sendLeft) < 0.500001f
                && peak (audio.sendRight) < 0.500001f,
            "stress reverb send is peak protected");
        test.expect (
            std::abs (mean (
                audio.left,
                static_cast<std::size_t> (sampleRate * 0.5)))
                < 0.01f
                && std::abs (mean (
                    audio.right,
                    static_cast<std::size_t> (sampleRate * 0.5)))
                < 0.01f,
            "DC blocker bounds long-term main mean");

        std::array<float, blockSize> mono;
        std::array<float, blockSize> monoSend;
        mono.fill (
            std::numeric_limits<float>::quiet_NaN());
        monoSend.fill (
            std::numeric_limits<float>::quiet_NaN());
        test.expect (
            renderBlock (
                *engine,
                mono.data(),
                nullptr,
                monoSend.data(),
                nullptr,
                blockSize),
            "supports mono main and mono optional send");
        test.expect (
            std::all_of (
                mono.begin(),
                mono.end(),
                [] (float value)
                {
                    return std::isfinite (value);
                })
                && std::all_of (
                    monoSend.begin(),
                    monoSend.end(),
                    [] (float value)
                    {
                        return std::isfinite (value);
                    }),
            "overwrites poisoned mono buffers with finite output");
    }

    void testAutoMixSlowControl (TestContext& test)
    {
        auto engine = makeEngine();
        test.expect (engine != nullptr, "AutoMix engine prepares");
        if (! engine)
            return;

        churchorgan::AutoMixParameters parameters;
        parameters.amount = 1.0f;
        parameters.maximumAttenuationDecibels = 6.0f;
        parameters.reactionMilliseconds = 250.0f;
        engine->setAutoMixParameters (parameters);
        engine->setAutoMixControlFrame ({
            1.0f, 1.0f, 1.0f, 1.0f
        });

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        renderBlock (
            *engine,
            left.data(),
            right.data(),
            nullptr,
            nullptr,
            blockSize);
        const auto first =
            engine->diagnostics().autoMixGains;
        test.expect (
            first.output > 0.99f
                && first.worshipLayer > 0.99f,
            "AutoMix does not jump on first control block");

        for (int block = 0; block < 1500; ++block)
        {
            renderBlock (
                *engine,
                left.data(),
                right.data(),
                nullptr,
                nullptr,
                blockSize);
        }
        const auto settled =
            engine->diagnostics().autoMixGains;
        test.expect (
            settled.output < first.output
                && settled.worshipLayer < first.worshipLayer
                && settled.organLowMid < first.organLowMid
                && settled.organMid < first.organMid,
            "piano/pad/strings/master envelopes slowly reduce zones");
        test.expect (
            settled.output > 0.49f
                && settled.worshipLayer > 0.49f,
            "AutoMix attenuation remains musically bounded");
    }

    void testZeroAllocationRender (TestContext& test)
    {
        auto engine = makeEngine();
        test.expect (
            engine != nullptr,
            "zero-allocation engine prepares");
        if (! engine)
            return;
        configureClean (*engine);
        engine->queueNoteOn (Manual::upper, 48, 100, 0);
        engine->queueNoteOn (Manual::upper, 55, 100, 0);
        engine->queueNoteOn (Manual::upper, 60, 100, 0);
        engine->queueNoteOff (Manual::upper, 48, 4097);
        engine->queueNoteOff (Manual::upper, 55, 4097);
        engine->queueNoteOff (Manual::upper, 60, 4097);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        std::array<float, blockSize> sendLeft {};
        std::array<float, blockSize> sendRight {};
        allocationCount.store (0, std::memory_order_relaxed);
        trackAllocations.store (true, std::memory_order_release);
        auto allRendered = true;
        for (int block = 0; block < 80; ++block)
        {
            allRendered =
                renderBlock (
                    *engine,
                    left.data(),
                    right.data(),
                    sendLeft.data(),
                    sendRight.data(),
                    blockSize)
                && allRendered;
        }
        trackAllocations.store (false, std::memory_order_release);
        test.expect (
            allRendered,
            "tracked render blocks all succeed");
        test.expect (
            allocationCount.load (std::memory_order_relaxed) == 0,
            "render performs zero dynamic allocations");
        test.expect (
            WorshipHybridEngine::renderUsesFixedStorage
                && ! WorshipHybridEngine::renderUsesLocks,
            "real-time contract advertises fixed storage and no locks");
    }
}

int main()
{
    TestContext test;
    testPrepareAndProfiles (test);
    testOffsetsBlocksAndTails (test);
    testMirroringSustainAndAllOff (test);
    testDeterminismAndStateB (test);
    testRatesSafetyAndMono (test);
    testAutoMixSlowControl (test);
    testZeroAllocationRender (test);

    std::cout
        << "ChurchOrgan Hybrid Phase checks="
        << test.checks
        << " failures=" << test.failures
        << '\n';
    return test.failures == 0 ? 0 : 1;
}
