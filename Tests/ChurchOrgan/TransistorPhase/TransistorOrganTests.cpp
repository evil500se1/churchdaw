#include "TransistorDividerBank.h"
#include "TransistorOrganEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    constexpr double twoPi =
        6.283185307179586476925286766559005768;

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
                std::cerr << "CHURCH_TRANSISTOR_ORGAN_FAIL: "
                          << message << '\n';
            }
        }
    };

    bool sameBits (float left, float right)
    {
        return std::memcmp (&left, &right, sizeof (float)) == 0;
    }

    churchorgan::TransistorOrganPrepareSpec specFor (
        std::size_t maximumBlock,
        churchorgan::TransistorOrganProfile profile =
            churchorgan::TransistorOrganProfile::britishCombo,
        std::uint64_t seed = 0x1020304050607080ULL)
    {
        churchorgan::TransistorOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = maximumBlock;
        spec.deterministicSeed = seed;
        spec.profile = profile;
        spec.outputTrim = 0.045f;
        spec.drive = 0.18f;
        spec.stereoWidth = 0.22f;
        spec.vibratoDepth = 0.55f;
        spec.vibratoEnabled = true;
        return spec;
    }

    void selectOnlyTab (
        churchorgan::TransistorOrganEngine& engine,
        churchorgan::TransistorOrganTab selected,
        float level = 1.0f)
    {
        for (std::size_t tab = 0;
             tab < churchorgan::transistorTabCount;
             ++tab)
            engine.setTabLevel (
                static_cast<churchorgan::TransistorOrganTab> (tab),
                tab == static_cast<std::size_t> (selected)
                    ? level
                    : 0.0f);
    }

    void testValidationAndApi (TestSuite& suite)
    {
        churchorgan::TransistorOrganEngine engine;
        auto spec = specFor (128);
        spec.sampleRate = 12000.0;
        suite.expect (
            ! engine.prepare (spec),
            "engine accepted invalid sample rate");

        spec = specFor (0);
        suite.expect (
            ! engine.prepare (spec),
            "engine accepted zero maximum block");

        spec = specFor (128);
        spec.drive = 1.2f;
        suite.expect (
            ! engine.prepare (spec),
            "engine accepted drive outside range");

        spec = specFor (128);
        suite.expect (
            engine.prepare (spec),
            "valid engine prepare failed");
        suite.expect (
            ! engine.queueUpperNoteOn (-1, 100),
            "engine accepted invalid negative note");
        suite.expect (
            ! engine.queueUpperNoteOn (60, 0),
            "engine accepted zero-velocity note-on");
        suite.expect (
            ! engine.queueUpperNoteOn (60, 100, 128),
            "engine accepted event beyond maximum block");
        suite.expect (
            std::strlen (
                churchorgan::TransistorOrganEngine::tabName (
                    churchorgan::TransistorOrganTab::mixture)) > 0,
            "tab API returned empty valid name");
    }

    void testSupportedRatesMonoAndTabs (TestSuite& suite)
    {
        constexpr std::array<double, 6> rates {{
            32000.0, 44100.0, 48000.0,
            96000.0, 192000.0, 384000.0
        }};
        constexpr std::size_t blockSize = 64;
        std::array<float, blockSize> mono {};

        for (double rate : rates)
        {
            auto spec = specFor (blockSize);
            spec.sampleRate = rate;
            spec.vibratoEnabled = false;
            churchorgan::TransistorOrganEngine engine;
            suite.expect (
                engine.prepare (spec),
                "engine rejected supported sample rate");
            selectOnlyTab (
                engine,
                churchorgan::TransistorOrganTab::principal8);
            engine.queueUpperNoteOn (60, 100, 0);
            double energy = 0.0;

            for (int block = 0; block < 12; ++block)
            {
                suite.expect (
                    engine.render ({
                        mono.data(), nullptr, blockSize }),
                    "mono render failed at supported rate");

                for (float sample : mono)
                {
                    suite.expect (
                        std::isfinite (sample),
                        "mono render produced invalid sample");
                    energy +=
                        static_cast<double> (sample)
                        * static_cast<double> (sample);
                }
            }

            suite.expect (
                energy > 0.000001,
                "mono render has no energy at supported rate");
        }

        for (std::size_t tab = 0;
             tab < churchorgan::transistorTabCount;
             ++tab)
        {
            churchorgan::TransistorOrganEngine engine;
            suite.expect (
                engine.prepare (specFor (blockSize)),
                "tab coverage engine prepare failed");
            engine.setVibratoEnabled (false);
            selectOnlyTab (
                engine,
                static_cast<churchorgan::TransistorOrganTab> (tab));
            engine.queueUpperNoteOn (60, 100, 0);
            double energy = 0.0;

            for (int block = 0; block < 16; ++block)
            {
                engine.render ({ mono.data(), nullptr, blockSize });

                for (float sample : mono)
                    energy +=
                        static_cast<double> (sample)
                        * static_cast<double> (sample);
            }

            suite.expect (
                energy > 0.000001,
                "a valid tab/rank route produced no energy");
        }
    }

    void testDividerDeterminismAndCoherence (TestSuite& suite)
    {
        churchorgan::TransistorDividerBank left;
        churchorgan::TransistorDividerBank right;
        churchorgan::TransistorDividerBank different;
        suite.expect (
            left.prepare (sampleRate, 0x11223344ULL),
            "divider left prepare failed");
        suite.expect (
            right.prepare (sampleRate, 0x11223344ULL),
            "divider right prepare failed");
        suite.expect (
            different.prepare (sampleRate, 0x55667788ULL),
            "divider different prepare failed");

        int lowRisingEdges = 0;
        int highRisingEdges = 0;
        float previousLow = 0.0f;
        float previousHigh = 0.0f;
        bool differentObserved = false;

        for (int sample = 0;
             sample < static_cast<int> (sampleRate);
             ++sample)
        {
            left.advance();
            right.advance();
            different.advance();
            const auto low = left.tapForMidi (60);
            const auto high = left.tapForMidi (72);
            const auto matching = right.tapForMidi (60);
            const auto other = different.tapForMidi (60);

            suite.expect (
                sameBits (low.square, matching.square)
                    && sameBits (low.pulse, matching.pulse)
                    && sameBits (low.ramp, matching.ramp),
                "divider same seed lost bit determinism");
            suite.expect (
                std::isfinite (low.square)
                    && std::isfinite (low.pulse)
                    && std::isfinite (low.ramp),
                "divider produced invalid tap");
            differentObserved =
                differentObserved
                || ! sameBits (low.square, other.square);

            if (previousLow <= 0.0f && low.square > 0.0f)
                ++lowRisingEdges;

            if (previousHigh <= 0.0f && high.square > 0.0f)
                ++highRisingEdges;

            previousLow = low.square;
            previousHigh = high.square;
        }

        suite.expect (
            differentObserved,
            "divider seed did not affect free-running phase");
        suite.expect (
            std::abs (highRisingEdges - 2 * lowRisingEdges) <= 2,
            "divider octave taps lost 2:1 edge relationship");
        suite.expect (
            std::abs (
                left.frequencyForMidi (72)
                / left.frequencyForMidi (60)
                - 2.0) < 0.0000001,
            "divider octave frequency ratio is incorrect");
    }

    struct SpectralEnergy
    {
        double expected = 0.0;
        double residual = 0.0;
    };

    template <std::size_t size>
    SpectralEnergy measureSquareSpectrum (
        const std::array<float, size>& signal,
        double fundamental)
    {
        SpectralEnergy result;

        for (std::size_t bin = 1; bin < size / 2; ++bin)
        {
            double real = 0.0;
            double imaginary = 0.0;

            for (std::size_t sample = 0; sample < size; ++sample)
            {
                const double window =
                    0.5
                    - 0.5 * std::cos (
                        twoPi
                        * static_cast<double> (sample)
                        / static_cast<double> (size - 1));
                const double phase =
                    twoPi
                    * static_cast<double> (bin * sample)
                    / static_cast<double> (size);
                const double value =
                    static_cast<double> (signal[sample]) * window;
                real += value * std::cos (phase);
                imaginary -= value * std::sin (phase);
            }

            const double energy =
                real * real + imaginary * imaginary;
            bool expectedBin = false;

            for (int harmonic = 1;
                 harmonic * fundamental < sampleRate * 0.5;
                 harmonic += 2)
            {
                const double expected =
                    static_cast<double> (harmonic)
                    * fundamental
                    * static_cast<double> (size)
                    / sampleRate;

                if (std::abs (
                        static_cast<double> (bin) - expected) <= 4.0)
                {
                    expectedBin = true;
                    break;
                }
            }

            if (expectedBin)
                result.expected += energy;
            else
                result.residual += energy;
        }

        return result;
    }

    void testDividerAntiAliasing (TestSuite& suite)
    {
        constexpr std::size_t fftSize = 2048;
        constexpr int midiNote = 108;
        churchorgan::TransistorDividerBank bank;
        suite.expect (
            bank.prepare (sampleRate, 0x1234abcdULL),
            "anti-alias divider prepare failed");

        std::array<float, fftSize> corrected {};
        std::array<float, fftSize> naive {};
        const double frequency =
            bank.frequencyForMidi (midiNote);
        double phase = 0.371;

        for (std::size_t sample = 0; sample < fftSize; ++sample)
        {
            bank.advance();
            corrected[sample] = bank.squareForMidi (midiNote);
            naive[sample] = phase < 0.5 ? 1.0f : -1.0f;
            phase += frequency / sampleRate;
            phase -= std::floor (phase);
        }

        const auto correctedEnergy =
            measureSquareSpectrum (corrected, frequency);
        const auto naiveEnergy =
            measureSquareSpectrum (naive, frequency);
        const double correctedRatio =
            correctedEnergy.residual
            / std::max (correctedEnergy.expected, 1.0e-30);
        const double naiveRatio =
            naiveEnergy.residual
            / std::max (naiveEnergy.expected, 1.0e-30);

        std::cout << "TRANSISTOR_ALIAS corrected_ratio="
                  << correctedRatio
                  << " naive_ratio=" << naiveRatio
                  << " reduction_pct="
                  << (1.0 - correctedRatio / naiveRatio) * 100.0
                  << '\n';

        suite.expect (
            correctedRatio <= naiveRatio * 0.02,
            "polyBLEP alias reduction fell below 98 percent");
        suite.expect (
            correctedRatio < 0.08,
            "polyBLEP high-note residual exceeded objective");
    }

    void testSampleAccurateEvents (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 512;
        churchorgan::TransistorOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "event engine prepare failed");
        engine.setVibratoEnabled (false);
        selectOnlyTab (
            engine,
            churchorgan::TransistorOrganTab::principal8);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        suite.expect (
            engine.queueUpperNoteOff (60, 400),
            "event note-off queue failed");
        suite.expect (
            engine.queueUpperNoteOn (60, 100, 37),
            "event note-on queue failed");
        suite.expect (
            engine.render ({ left.data(), right.data(), blockSize }),
            "event render failed");

        bool prefixSilent = true;
        double heldEnergy = 0.0;

        for (std::size_t sample = 0; sample < blockSize; ++sample)
        {
            if (sample < 37)
                prefixSilent =
                    prefixSilent && left[sample] == 0.0f;

            if (sample >= 37 && sample < 400)
                heldEnergy +=
                    static_cast<double> (left[sample])
                    * static_cast<double> (left[sample]);
        }

        suite.expect (
            prefixSilent,
            "sample-accurate note-on leaked before offset");
        suite.expect (
            heldEnergy > 0.000001,
            "sample-accurate held interval has no energy");

        for (int block = 0; block < 16; ++block)
            engine.render ({ left.data(), right.data(), blockSize });

        suite.expect (
            engine.activeUpperNoteCount() == 0,
            "released event voice failed to retire");
    }

    void testEngineDeterminismAndProfiles (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::TransistorOrganEngine left;
        churchorgan::TransistorOrganEngine right;
        suite.expect (
            left.prepare (specFor (blockSize)),
            "determinism left prepare failed");
        suite.expect (
            right.prepare (specFor (blockSize)),
            "determinism right prepare failed");

        std::array<float, blockSize> leftA {};
        std::array<float, blockSize> rightA {};
        std::array<float, blockSize> leftB {};
        std::array<float, blockSize> rightB {};
        double energy = 0.0;

        for (int block = 0; block < 96; ++block)
        {
            if (block == 0)
            {
                left.queueUpperNoteOn (48, 90, 0);
                right.queueUpperNoteOn (48, 90, 0);
            }

            if (block == 7)
            {
                left.queueUpperNoteOn (55, 100, 31);
                right.queueUpperNoteOn (55, 100, 31);
            }

            if (block == 40)
            {
                left.queueUpperNoteOff (48, 17);
                right.queueUpperNoteOff (48, 17);
            }

            if (block == 60)
            {
                left.queueUpperAllNotesOff (64);
                right.queueUpperAllNotesOff (64);
            }

            suite.expect (
                left.render ({
                    leftA.data(), rightA.data(), blockSize }),
                "determinism left render failed");
            suite.expect (
                right.render ({
                    leftB.data(), rightB.data(), blockSize }),
                "determinism right render failed");

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                suite.expect (
                    sameBits (leftA[sample], leftB[sample])
                        && sameBits (rightA[sample], rightB[sample]),
                    "same seed/event stream lost engine determinism");
                suite.expect (
                    std::isfinite (leftA[sample])
                        && std::isfinite (rightA[sample]),
                    "engine produced invalid deterministic output");
                energy +=
                    static_cast<double> (leftA[sample])
                    * static_cast<double> (leftA[sample]);
            }
        }

        suite.expect (
            energy > 0.001,
            "deterministic engine output has no energy");

        std::array<double, 3> profileSignatures {};

        for (std::size_t profileIndex = 0;
             profileIndex < profileSignatures.size();
             ++profileIndex)
        {
            churchorgan::TransistorOrganEngine profileEngine;
            const auto profile =
                static_cast<churchorgan::TransistorOrganProfile> (
                    profileIndex);
            suite.expect (
                profileEngine.prepare (
                    specFor (blockSize, profile, 0x77777777ULL)),
                "profile engine prepare failed");
            profileEngine.setVibratoEnabled (false);
            profileEngine.queueUpperNoteOn (60, 100, 0);
            double signature = 0.0;

            for (int block = 0; block < 24; ++block)
            {
                profileEngine.render ({
                    leftA.data(), rightA.data(), blockSize });

                for (std::size_t sample = 0;
                     sample < blockSize;
                     ++sample)
                    signature +=
                        static_cast<double> (leftA[sample])
                        * (1.0
                           + 0.0001
                             * static_cast<double> (
                                 block * blockSize
                                 + static_cast<int> (sample)));
            }

            profileSignatures[profileIndex] = signature;
        }

        suite.expect (
            std::abs (
                profileSignatures[0] - profileSignatures[1]) > 0.01
                && std::abs (
                    profileSignatures[1] - profileSignatures[2]) > 0.01
                && std::abs (
                    profileSignatures[0] - profileSignatures[2]) > 0.01,
            "three combo profiles are not observably distinct");
    }

    void testDcStereoAndHeadroom (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::TransistorOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "DC engine prepare failed");
        engine.setVibratoEnabled (false);
        engine.queueUpperNoteOn (60, 100, 0);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        double sum = 0.0;
        double sumSquares = 0.0;
        std::size_t measured = 0;
        bool stereoDifference = false;

        for (int block = 0; block < 400; ++block)
        {
            engine.render ({
                left.data(), right.data(), blockSize });

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                if (block >= 64)
                {
                    sum += left[sample];
                    sumSquares +=
                        static_cast<double> (left[sample])
                        * static_cast<double> (left[sample]);
                    ++measured;
                }

                stereoDifference =
                    stereoDifference
                    || ! sameBits (left[sample], right[sample]);
            }
        }

        const double mean =
            sum / static_cast<double> (measured);
        const double rms = std::sqrt (
            sumSquares / static_cast<double> (measured));
        std::cout << "TRANSISTOR_DC mean=" << mean
                  << " rms=" << rms
                  << " mean_to_rms="
                  << std::abs (mean) / std::max (rms, 1.0e-30)
                  << '\n';
        suite.expect (
            std::abs (mean) < std::max (0.0005, rms * 0.012),
            "sustained transistor output retained excessive DC");
        suite.expect (
            stereoDifference,
            "stereo cabinet remained dual-mono");

        churchorgan::TransistorOrganEngine full;
        suite.expect (
            full.prepare (specFor (blockSize)),
            "headroom engine prepare failed");

        for (int note = 0; note < 128; ++note)
            suite.expect (
                full.queueUpperNoteOn (note, 127, 0),
                "headroom polyphony event queue failed");

        float peak = 0.0f;

        for (int block = 0; block < 64; ++block)
        {
            suite.expect (
                full.render ({
                    left.data(), right.data(), blockSize }),
                "headroom render failed");

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
                peak = std::max (
                    peak,
                    std::max (
                        std::abs (left[sample]),
                        std::abs (right[sample])));
        }

        suite.expect (
            peak > 0.05f && peak <= 0.999f,
            "128-note headroom escaped bounded output");
        suite.expect (
            full.diagnostics().invalidSamples == 0,
            "headroom render reported invalid samples");
    }

    void testHighPolyphonyDeterminism (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::TransistorOrganEngine left;
        churchorgan::TransistorOrganEngine right;
        suite.expect (
            left.prepare (
                specFor (
                    blockSize,
                    churchorgan::TransistorOrganProfile::italianCombo,
                    0xabcddcba12344321ULL)),
            "high-poly left prepare failed");
        suite.expect (
            right.prepare (
                specFor (
                    blockSize,
                    churchorgan::TransistorOrganProfile::italianCombo,
                    0xabcddcba12344321ULL)),
            "high-poly right prepare failed");

        for (int note = 12; note < 84; ++note)
        {
            const int velocity = 64 + (note * 17) % 64;
            suite.expect (
                left.queueUpperNoteOn (note, velocity, 0),
                "high-poly left note-on failed");
            suite.expect (
                right.queueUpperNoteOn (note, velocity, 0),
                "high-poly right note-on failed");
        }

        std::array<float, blockSize> leftA {};
        std::array<float, blockSize> rightA {};
        std::array<float, blockSize> leftB {};
        std::array<float, blockSize> rightB {};
        float peak = 0.0f;

        for (int block = 0; block < 80; ++block)
        {
            if (block == 24)
            {
                for (int note = 12; note < 48; ++note)
                {
                    left.queueUpperNoteOff (note, 37);
                    right.queueUpperNoteOff (note, 37);
                }
            }

            suite.expect (
                left.render ({
                    leftA.data(), rightA.data(), blockSize }),
                "high-poly left render failed");
            suite.expect (
                right.render ({
                    leftB.data(), rightB.data(), blockSize }),
                "high-poly right render failed");

            for (std::size_t sample = 0;
                 sample < blockSize;
                 ++sample)
            {
                suite.expect (
                    sameBits (leftA[sample], leftB[sample])
                        && sameBits (rightA[sample], rightB[sample]),
                    "high-poly aggregation lost bit determinism");
                suite.expect (
                    std::isfinite (leftA[sample])
                        && std::isfinite (rightA[sample]),
                    "high-poly aggregation produced invalid sample");
                peak = std::max (
                    peak,
                    std::max (
                        std::abs (leftA[sample]),
                        std::abs (rightA[sample])));
            }
        }

        suite.expect (
            peak > 0.05f && peak <= 0.999f,
            "high-poly aggregation escaped safe peak bounds");
    }

    void testZeroAllocations (TestSuite& suite)
    {
        constexpr std::size_t blockSize = 128;
        churchorgan::TransistorOrganEngine engine;
        suite.expect (
            engine.prepare (specFor (blockSize)),
            "allocation engine prepare failed");
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};

        for (int note = 36; note < 84; note += 4)
            engine.queueUpperNoteOn (note, 100, 0);

        const auto before =
            allocationCount.load (std::memory_order_relaxed);
        float sink = 0.0f;

        for (int block = 0; block < 750; ++block)
        {
            if (block == 250)
                engine.queueUpperNoteOn (91, 85, 63);

            if (block == 500)
                engine.queueUpperAllNotesOff (17);

            engine.setTabLevel (
                churchorgan::TransistorOrganTab::mixture,
                static_cast<float> (block % 101) * 0.01f);
            suite.expect (
                engine.render ({
                    left.data(), right.data(), blockSize }),
                "allocation render failed");
            sink += left[static_cast<std::size_t> (block) % blockSize];
        }

        const auto after =
            allocationCount.load (std::memory_order_relaxed);
        suite.expect (
            after == before,
            "transistor audio path allocated after prepare");
        suite.expect (
            std::isfinite (sink),
            "allocation test sink became invalid");
    }
}

int main()
{
    TestSuite suite;
    testValidationAndApi (suite);
    testSupportedRatesMonoAndTabs (suite);
    testDividerDeterminismAndCoherence (suite);
    testDividerAntiAliasing (suite);
    testSampleAccurateEvents (suite);
    testEngineDeterminismAndProfiles (suite);
    testDcStereoAndHeadroom (suite);
    testHighPolyphonyDeterminism (suite);
    testZeroAllocations (suite);

    if (suite.failures != 0)
    {
        std::cerr << "CHURCH_TRANSISTOR_ORGAN_TESTS_FAILED checks="
                  << suite.checks
                  << " failures=" << suite.failures
                  << '\n';
        return 1;
    }

    std::cout << "CHURCH_TRANSISTOR_ORGAN_TESTS_OK checks="
              << suite.checks
              << " allocations="
              << allocationCount.load (std::memory_order_relaxed)
              << '\n';
    return 0;
}
