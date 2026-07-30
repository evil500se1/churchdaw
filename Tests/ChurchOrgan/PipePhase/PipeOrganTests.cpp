#include "PipeOrganEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>

namespace
{
    std::atomic<std::uint64_t> allocationCount { 0 };
}

void* operator new (std::size_t size)
{
    allocationCount.fetch_add (1, std::memory_order_relaxed);

    if (void* memory = std::malloc (size == 0 ? 1 : size))
        return memory;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
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

    struct TestSuite
    {
        int checks = 0;
        int failures = 0;

        void expect (bool condition, const char* message)
        {
            ++checks;

            if (! condition)
            {
                ++failures;
                std::cerr << "CHURCH_PIPE_ORGAN_FAIL: "
                          << message << '\n';
            }
        }
    };

    bool sameBits (float left, float right)
    {
        return std::memcmp (&left, &right, sizeof (float)) == 0;
    }

    churchorgan::PipeOrganPrepareSpec specFor (
        std::size_t maximumBlock,
        churchorgan::PipeOrganQuality quality =
            churchorgan::PipeOrganQuality::standard,
        std::uint64_t seed = 0x1020304050607080ULL)
    {
        churchorgan::PipeOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = maximumBlock;
        spec.deterministicSeed = seed;
        spec.quality = quality;
        spec.temperament =
            churchorgan::PipeOrganTemperament::churchWell;
        spec.tuningAHz = 440.0f;
        spec.outputTrim = 0.082f;
        spec.stereoWidth = 0.72f;
        return spec;
    }

    void clearRanks (
        churchorgan::PipeOrganEngine& engine,
        churchorgan::PipeOrganManual manual)
    {
        for (std::size_t rank = 0;
             rank < churchorgan::pipeOrganRankCount;
             ++rank)
            engine.setRankLevel (
                manual,
                static_cast<churchorgan::PipeOrganRank> (rank),
                0.0f);
    }

    void selectOnlyRank (
        churchorgan::PipeOrganEngine& engine,
        churchorgan::PipeOrganManual manual,
        churchorgan::PipeOrganRank selected)
    {
        clearRanks (engine, manual);
        engine.setRankLevel (manual, selected, 1.0f);
    }

    template <std::size_t blockSize>
    double renderEnergy (
        churchorgan::PipeOrganEngine& engine,
        int blocks,
        bool stereo = false)
    {
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        double energy = 0.0;

        for (int block = 0; block < blocks; ++block)
        {
            engine.render ({
                left.data(),
                stereo ? right.data() : nullptr,
                blockSize
            });

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                energy +=
                    static_cast<double> (left[sample])
                    * static_cast<double> (left[sample]);

                if (stereo)
                    energy +=
                        static_cast<double> (right[sample])
                        * static_cast<double> (right[sample]);
            }
        }

        return energy;
    }

    void testValidationAndNames (TestSuite& suite)
    {
        churchorgan::PipeOrganEngine engine;
        auto spec = specFor (128);
        spec.sampleRate =
            std::numeric_limits<double>::quiet_NaN();
        suite.expect (
            ! engine.prepare (spec),
            "pipe engine accepted NaN sample rate");

        spec = specFor (0);
        suite.expect (
            ! engine.prepare (spec),
            "pipe engine accepted zero block size");

        spec = specFor (128);
        spec.tuningAHz = 390.0f;
        suite.expect (
            ! engine.prepare (spec),
            "pipe engine accepted unsafe tuning");

        spec = specFor (128);
        suite.expect (
            engine.prepare (spec),
            "valid pipe prepare failed");
        suite.expect (
            ! engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                -1, 100),
            "pipe engine accepted invalid note");
        suite.expect (
            ! engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                60, 0),
            "pipe engine accepted zero velocity");
        suite.expect (
            ! engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                60, 100, 128),
            "pipe engine accepted event outside prepared block");
        suite.expect (
            sizeof (churchorgan::PipeOrganEngine)
                < 4U * 1024U * 1024U,
            "fixed pipe engine storage exceeded 4 MiB budget");

        for (std::size_t rank = 0;
             rank < churchorgan::pipeOrganRankCount;
             ++rank)
            suite.expect (
                std::strlen (
                    churchorgan::PipeOrganEngine::rankName (
                        static_cast<churchorgan::PipeOrganRank> (
                            rank))) > 0,
                "valid rank returned empty name");
    }

    void testRatesQualitiesAndRanks (TestSuite& suite)
    {
        constexpr std::array<double, 6> rates {{
            32000.0, 44100.0, 48000.0,
            96000.0, 192000.0, 384000.0
        }};
        constexpr std::size_t blockSize = 64;
        std::array<double, 3> qualitySignatures {};

        for (double rate : rates)
        {
            auto spec = specFor (blockSize);
            spec.sampleRate = rate;
            churchorgan::PipeOrganEngine engine;
            suite.expect (
                engine.prepare (spec),
                "pipe engine rejected supported sample rate");
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                60, 100, 0);
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::lower,
                55, 96, 11);
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::pedal,
                36, 110, 23);
            const double energy =
                renderEnergy<blockSize> (engine, 18);
            suite.expect (
                std::isfinite (energy) && energy > 0.000001,
                "supported sample rate produced invalid/silent mono");
            suite.expect (
                engine.diagnostics().invalidSamples == 0,
                "supported sample rate reported invalid samples");
        }

        for (std::size_t quality = 0; quality < 3; ++quality)
        {
            churchorgan::PipeOrganEngine engine;
            suite.expect (
                engine.prepare (
                    specFor (
                        blockSize,
                        static_cast<churchorgan::PipeOrganQuality> (
                            quality),
                        0x77777777ULL)),
                "quality prepare failed");
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                64, 100, 0);
            const double energy =
                renderEnergy<blockSize> (engine, 24, true);
            qualitySignatures[quality] =
                energy + engine.diagnostics().lastBlockRms;
            suite.expect (
                energy > 0.000001
                    && engine.diagnostics().invalidSamples == 0,
                "quality mode produced invalid/silent output");
        }

        suite.expect (
            std::abs (
                qualitySignatures[0]
                - qualitySignatures[1]) > 0.00001
                && std::abs (
                    qualitySignatures[1]
                    - qualitySignatures[2]) > 0.00001,
            "Eco/Standard/Ultra are not observably distinct");

        for (std::size_t rank = 0;
             rank < churchorgan::pipeOrganRankCount;
             ++rank)
        {
            churchorgan::PipeOrganEngine engine;
            suite.expect (
                engine.prepare (specFor (blockSize)),
                "rank coverage prepare failed");
            selectOnlyRank (
                engine,
                churchorgan::PipeOrganManual::upper,
                static_cast<churchorgan::PipeOrganRank> (rank));
            engine.reset();
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                60, 100, 0);
            const double energy =
                renderEnergy<blockSize> (engine, 32);
            suite.expect (
                energy > 0.0000001,
                "a valid pipe rank produced no energy");
        }
    }

    void testSampleAccurateEvents (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 512;
        churchorgan::PipeOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "sample-accurate prepare failed");
        selectOnlyRank (
            engine,
            churchorgan::PipeOrganManual::upper,
            churchorgan::PipeOrganRank::principal);
        engine.reset();
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        suite.expect (
            engine.queueNoteOff (
                churchorgan::PipeOrganManual::upper,
                60, 400),
            "sample-accurate note-off queue failed");
        suite.expect (
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                60, 100, 37),
            "sample-accurate note-on queue failed");
        suite.expect (
            engine.render ({
                left.data(), right.data(), blockSize }),
            "sample-accurate render failed");

        bool prefixSilent = true;
        double heldEnergy = 0.0;

        for (std::size_t sample = 0;
             sample < blockSize;
             ++sample)
        {
            if (sample < 37)
                prefixSilent =
                    prefixSilent
                    && left[sample] == 0.0f
                    && right[sample] == 0.0f;

            if (sample >= 37 && sample < 400)
                heldEnergy +=
                    static_cast<double> (left[sample])
                    * static_cast<double> (left[sample]);
        }

        suite.expect (
            prefixSilent,
            "note-on leaked before sample offset");
        suite.expect (
            heldEnergy > 0.0000001,
            "held sample interval has no energy");

        for (int block = 0; block < 120; ++block)
            engine.render ({
                left.data(), right.data(), blockSize });

        suite.expect (
            engine.activeDivisionVoiceCount() == 0,
            "released pipe voice did not retire");
    }

    void testDeterminism (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::PipeOrganEngine leftEngine;
        churchorgan::PipeOrganEngine rightEngine;
        churchorgan::PipeOrganEngine differentEngine;
        suite.expect (
            leftEngine.prepare (specFor (
                blockSize,
                churchorgan::PipeOrganQuality::standard,
                0x1122334455667788ULL)),
            "determinism left prepare failed");
        suite.expect (
            rightEngine.prepare (specFor (
                blockSize,
                churchorgan::PipeOrganQuality::standard,
                0x1122334455667788ULL)),
            "determinism right prepare failed");
        suite.expect (
            differentEngine.prepare (specFor (
                blockSize,
                churchorgan::PipeOrganQuality::standard,
                0x8877665544332211ULL)),
            "different-seed prepare failed");

        std::array<float, blockSize> leftA {};
        std::array<float, blockSize> rightA {};
        std::array<float, blockSize> leftB {};
        std::array<float, blockSize> rightB {};
        std::array<float, blockSize> different {};
        bool differentObserved = false;

        for (int block = 0; block < 72; ++block)
        {
            if (block == 0)
                for (auto* engine : {
                         &leftEngine,
                         &rightEngine,
                         &differentEngine })
                    engine->queueNoteOn (
                        churchorgan::PipeOrganManual::upper,
                        60, 100, 17);

            if (block == 8)
                for (auto* engine : {
                         &leftEngine,
                         &rightEngine,
                         &differentEngine })
                    engine->queueNoteOn (
                        churchorgan::PipeOrganManual::lower,
                        55, 94, 63);

            if (block == 38)
                for (auto* engine : {
                         &leftEngine,
                         &rightEngine,
                         &differentEngine })
                    engine->queueNoteOff (
                        churchorgan::PipeOrganManual::upper,
                        60, 31);

            leftEngine.render ({
                leftA.data(), rightA.data(), blockSize });
            rightEngine.render ({
                leftB.data(), rightB.data(), blockSize });
            differentEngine.render ({
                different.data(), nullptr, blockSize });

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                suite.expect (
                    sameBits (leftA[sample], leftB[sample])
                        && sameBits (
                            rightA[sample], rightB[sample]),
                    "same seed lost bit-exact determinism");
                differentObserved =
                    differentObserved
                    || ! sameBits (
                        leftA[sample], different[sample]);
            }
        }

        suite.expect (
            differentObserved,
            "different seed did not change pipe speech/field");
    }

    struct AbsoluteEvent
    {
        churchorgan::PipeOrganEventType type;
        churchorgan::PipeOrganManual manual;
        int note;
        int velocity;
        std::size_t sample;
    };

    void queueAbsoluteEvent (
        churchorgan::PipeOrganEngine& engine,
        const AbsoluteEvent& event,
        std::size_t blockStart)
    {
        const auto offset = static_cast<std::uint32_t> (
            event.sample - blockStart);

        switch (event.type)
        {
            case churchorgan::PipeOrganEventType::noteOn:
                engine.queueNoteOn (
                    event.manual,
                    event.note,
                    event.velocity,
                    offset);
                break;

            case churchorgan::PipeOrganEventType::noteOff:
                engine.queueNoteOff (
                    event.manual,
                    event.note,
                    offset);
                break;

            case churchorgan::PipeOrganEventType::allNotesOff:
                engine.queueAllNotesOff (
                    event.manual,
                    offset);
                break;
        }
    }

    void testBlockPartitioning (TestSuite& suite)
    {
        constexpr std::size_t totalSamples = 1024;
        constexpr std::size_t smallBlock = 128;
        constexpr std::array<AbsoluteEvent, 6> events {{
            { churchorgan::PipeOrganEventType::noteOn,
              churchorgan::PipeOrganManual::upper, 60, 100, 20 },
            { churchorgan::PipeOrganEventType::noteOn,
              churchorgan::PipeOrganManual::lower, 55, 96, 190 },
            { churchorgan::PipeOrganEventType::noteOn,
              churchorgan::PipeOrganManual::pedal, 36, 110, 260 },
            { churchorgan::PipeOrganEventType::noteOff,
              churchorgan::PipeOrganManual::upper, 60, 0, 700 },
            { churchorgan::PipeOrganEventType::noteOff,
              churchorgan::PipeOrganManual::lower, 55, 0, 900 },
            { churchorgan::PipeOrganEventType::allNotesOff,
              churchorgan::PipeOrganManual::pedal, 0, 0, 977 }
        }};

        churchorgan::PipeOrganEngine whole;
        churchorgan::PipeOrganEngine partitioned;
        suite.expect (
            whole.prepare (specFor (totalSamples)),
            "whole partition prepare failed");
        suite.expect (
            partitioned.prepare (specFor (smallBlock)),
            "small partition prepare failed");
        std::array<float, totalSamples> wholeLeft {};
        std::array<float, totalSamples> wholeRight {};
        std::array<float, totalSamples> partLeft {};
        std::array<float, totalSamples> partRight {};

        for (const auto& event : events)
            queueAbsoluteEvent (whole, event, 0);

        suite.expect (
            whole.render ({
                wholeLeft.data(),
                wholeRight.data(),
                totalSamples }),
            "whole partition render failed");

        for (std::size_t start = 0;
             start < totalSamples;
             start += smallBlock)
        {
            for (const auto& event : events)
                if (event.sample >= start
                     && event.sample < start + smallBlock)
                    queueAbsoluteEvent (
                        partitioned, event, start);

            suite.expect (
                partitioned.render ({
                    partLeft.data() + start,
                    partRight.data() + start,
                    smallBlock }),
                "small partition render failed");
        }

        for (std::size_t sample = 0;
             sample < totalSamples;
             ++sample)
            suite.expect (
                sameBits (wholeLeft[sample], partLeft[sample])
                    && sameBits (
                        wholeRight[sample], partRight[sample]),
                "block partition changed pipe output");
    }

    void testCouplersExpressionAndTemperament (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::PipeOrganEngine coupled;
        churchorgan::PipeOrganEngine dry;
        suite.expect (
            coupled.prepare (specFor (blockSize)),
            "coupled prepare failed");
        suite.expect (
            dry.prepare (specFor (blockSize)),
            "dry coupler prepare failed");

        for (auto* engine : { &coupled, &dry })
        {
            clearRanks (
                *engine,
                churchorgan::PipeOrganManual::upper);
            clearRanks (
                *engine,
                churchorgan::PipeOrganManual::lower);
            engine->setRankLevel (
                churchorgan::PipeOrganManual::lower,
                churchorgan::PipeOrganRank::principal,
                1.0f);
            engine->reset();
        }

        coupled.setCoupler (
            churchorgan::PipeOrganManual::upper,
            churchorgan::PipeOrganManual::lower,
            true);
        coupled.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        dry.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        const double coupledEnergy =
            renderEnergy<blockSize> (coupled, 32);
        const double dryEnergy =
            renderEnergy<blockSize> (dry, 32);
        suite.expect (
            coupledEnergy > dryEnergy + 0.00001,
            "upper-to-lower coupler did not route destination rank");

        churchorgan::PipeOrganEngine swellOpen;
        churchorgan::PipeOrganEngine swellClosed;
        swellOpen.prepare (specFor (blockSize));
        swellClosed.prepare (specFor (blockSize));
        swellOpen.setSwell (
            churchorgan::PipeOrganManual::upper, 1.0f);
        swellClosed.setSwell (
            churchorgan::PipeOrganManual::upper, 0.0f);
        swellOpen.reset();
        swellClosed.reset();
        swellOpen.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        swellClosed.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        const double openEnergy =
            renderEnergy<blockSize> (swellOpen, 48);
        const double closedEnergy =
            renderEnergy<blockSize> (swellClosed, 48);
        suite.expect (
            openEnergy > closedEnergy * 4.0,
            "swell range is not materially expressive");

        churchorgan::PipeOrganEngine crescendo;
        crescendo.prepare (specFor (blockSize));
        clearRanks (
            crescendo,
            churchorgan::PipeOrganManual::upper);
        crescendo.setCrescendo (1.0f);
        crescendo.reset();
        crescendo.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        suite.expect (
            renderEnergy<blockSize> (crescendo, 32) > 0.00001,
            "crescendo failed to add progressive registration");

        churchorgan::PipeOrganEngine equal;
        churchorgan::PipeOrganEngine meantone;
        equal.prepare (specFor (blockSize));
        meantone.prepare (specFor (blockSize));
        equal.setTemperament (
            churchorgan::PipeOrganTemperament::equal);
        meantone.setTemperament (
            churchorgan::PipeOrganTemperament::gentleMeantone);
        equal.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            61, 100, 0);
        meantone.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            61, 100, 0);
        std::array<float, blockSize> equalBuffer {};
        std::array<float, blockSize> meantoneBuffer {};
        bool temperamentDifference = false;

        for (int block = 0; block < 12; ++block)
        {
            equal.render ({
                equalBuffer.data(), nullptr, blockSize });
            meantone.render ({
                meantoneBuffer.data(), nullptr, blockSize });

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
                temperamentDifference =
                    temperamentDifference
                    || ! sameBits (
                        equalBuffer[sample],
                        meantoneBuffer[sample]);
        }

        suite.expect (
            temperamentDifference,
            "temperament selection did not affect tuning");

        equal.setTemperament (
            churchorgan::PipeOrganTemperament::equal);
        meantone.setTemperament (
            churchorgan::PipeOrganTemperament::equal);
        meantone.setTremulant (
            churchorgan::PipeOrganManual::upper,
            true, 0.8f, 6.1f);
        equal.reset();
        meantone.reset();
        equal.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        meantone.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        bool tremulantDifference = false;

        for (int block = 0; block < 80; ++block)
        {
            equal.render ({
                equalBuffer.data(), nullptr, blockSize });
            meantone.render ({
                meantoneBuffer.data(), nullptr, blockSize });

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
                tremulantDifference =
                    tremulantDifference
                    || ! sameBits (
                        equalBuffer[sample],
                        meantoneBuffer[sample]);
        }

        suite.expect (
            tremulantDifference,
            "division tremulant did not affect sustained pipes");
    }

    void testNaNDcPeakAndPolyphony (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::PipeOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "NaN/DC engine prepare failed");
        const float nan =
            std::numeric_limits<float>::quiet_NaN();
        engine.setRankLevel (
            churchorgan::PipeOrganManual::upper,
            churchorgan::PipeOrganRank::principal,
            nan);
        engine.setSwell (
            churchorgan::PipeOrganManual::upper, nan);
        engine.setCrescendo (nan);
        suite.expect (
            engine.targetRankLevel (
                churchorgan::PipeOrganManual::upper,
                churchorgan::PipeOrganRank::principal) == 0.0f
                && engine.targetSwell (
                    churchorgan::PipeOrganManual::upper) == 0.0f
                && engine.targetCrescendo() == 0.0f,
            "NaN parameter sanitisation failed");

        engine.loadDefaultRegistration();
        engine.reset();
        engine.queueNoteOn (
            churchorgan::PipeOrganManual::upper,
            60, 100, 0);
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        double sum = 0.0;
        double sumSquares = 0.0;
        std::size_t measured = 0;
        bool stereoDifference = false;

        for (int block = 0; block < 750; ++block)
        {
            suite.expect (
                engine.render ({
                    left.data(), right.data(), blockSize }),
                "DC render failed");

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                suite.expect (
                    std::isfinite (left[sample])
                        && std::isfinite (right[sample]),
                    "pipe output contains NaN/Inf");
                stereoDifference =
                    stereoDifference
                    || ! sameBits (left[sample], right[sample]);

                if (block >= 160)
                {
                    sum += left[sample];
                    sumSquares +=
                        static_cast<double> (left[sample])
                        * static_cast<double> (left[sample]);
                    ++measured;
                }
            }
        }

        const double mean =
            sum / static_cast<double> (measured);
        const double rms = std::sqrt (
            sumSquares / static_cast<double> (measured));
        std::cout << "PIPE_DC mean=" << mean
                  << " rms=" << rms
                  << " mean_to_rms="
                  << std::abs (mean) / std::max (rms, 1.0e-30)
                  << '\n';
        suite.expect (
            std::abs (mean)
                < std::max (0.0005, rms * 0.012),
            "sustained pipe output retained excessive DC");
        suite.expect (
            stereoDifference,
            "pipe field remained dual-mono");

        churchorgan::PipeOrganEngine full;
        suite.expect (
            full.prepare (specFor (blockSize)),
            "128-note prepare failed");

        for (std::size_t rank = 0;
             rank < churchorgan::pipeOrganRankCount;
             ++rank)
            full.setRankLevel (
                churchorgan::PipeOrganManual::upper,
                static_cast<churchorgan::PipeOrganRank> (rank),
                1.0f);

        full.reset();

        for (int note = 0; note < 128; ++note)
            suite.expect (
                full.queueNoteOn (
                    churchorgan::PipeOrganManual::upper,
                    note, 127, 0),
                "128-note queue failed");

        float peak = 0.0f;

        for (int block = 0; block < 20; ++block)
        {
            suite.expect (
                full.render ({
                    left.data(), right.data(), blockSize }),
                "128-note render failed");

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
                peak = std::max (
                    peak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
        }

        std::cout << "PIPE_128 peak=" << peak
                  << " sounding_pipes="
                  << full.diagnostics().soundingPipes
                  << '\n';
        suite.expect (
            peak > 0.02f && peak < 0.97f,
            "128-note headroom escaped smooth bound");
        suite.expect (
            full.diagnostics().invalidSamples == 0,
            "128-note render reported invalid samples");
    }

    void testZeroAllocation (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::PipeOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "allocation engine prepare failed");
        engine.setCoupler (
            churchorgan::PipeOrganManual::upper,
            churchorgan::PipeOrganManual::lower,
            true);
        engine.setTremulant (
            churchorgan::PipeOrganManual::lower,
            true, 0.55f, 5.7f);
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};

        for (int note = 36; note < 84; note += 4)
            engine.queueNoteOn (
                churchorgan::PipeOrganManual::upper,
                note, 100, 0);

        engine.queueNoteOn (
            churchorgan::PipeOrganManual::pedal,
            36, 110, 0);
        const auto before =
            allocationCount.load (std::memory_order_relaxed);
        float sink = 0.0f;

        for (int block = 0; block < 500; ++block)
        {
            if (block == 160)
                engine.queueNoteOn (
                    churchorgan::PipeOrganManual::lower,
                    67, 92, 63);

            if (block == 320)
            {
                engine.queueAllNotesOff (
                    churchorgan::PipeOrganManual::upper,
                    17);
                engine.queueAllNotesOff (
                    churchorgan::PipeOrganManual::lower,
                    17);
                engine.queueAllNotesOff (
                    churchorgan::PipeOrganManual::pedal,
                    17);
            }

            engine.setSwell (
                churchorgan::PipeOrganManual::upper,
                static_cast<float> (block % 101) * 0.01f);
            engine.setCrescendo (
                static_cast<float> (block % 151) / 150.0f);
            suite.expect (
                engine.render ({
                    left.data(), right.data(), blockSize }),
                "allocation render failed");
            sink += left[static_cast<std::size_t> (block)
                         % blockSize];
        }

        const auto after =
            allocationCount.load (std::memory_order_relaxed);
        suite.expect (
            after == before,
            "pipe render/event/control path allocated after prepare");
        suite.expect (
            std::isfinite (sink),
            "allocation sink became invalid");
    }
}

int main()
{
    TestSuite suite;
    testValidationAndNames (suite);
    testRatesQualitiesAndRanks (suite);
    testSampleAccurateEvents (suite);
    testDeterminism (suite);
    testBlockPartitioning (suite);
    testCouplersExpressionAndTemperament (suite);
    testNaNDcPeakAndPolyphony (suite);
    testZeroAllocation (suite);

    if (suite.failures != 0)
    {
        std::cerr << "CHURCH_PIPE_ORGAN_TESTS_FAILED checks="
                  << suite.checks
                  << " failures=" << suite.failures
                  << '\n';
        return 1;
    }

    std::cout << "CHURCH_PIPE_ORGAN_TESTS_OK checks="
              << suite.checks
              << " allocations="
              << allocationCount.load (std::memory_order_relaxed)
              << " engine_bytes="
              << sizeof (churchorgan::PipeOrganEngine)
              << '\n';
    return 0;
}
