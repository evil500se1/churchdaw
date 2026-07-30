#include "ChurchOrganCore.h"

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
                std::cerr << "CHURCH_ORGAN_TEST_FAIL: "
                          << message << '\n';
            }
        }
    };

    churchorgan::PrepareSpec specFor (
        std::size_t maximumBlock,
        std::uint64_t seed = 0x1020304050607080ULL)
    {
        churchorgan::PrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = maximumBlock;
        spec.deterministicSeed = seed;
        spec.drawbarSmoothingMilliseconds = 18.0f;
        spec.contactAttackMilliseconds = 0.70f;
        spec.contactReleaseMilliseconds = 1.20f;
        spec.maximumWheelOffsetCents = 0.0f;
        spec.outputTrim = 0.11f;
        return spec;
    }

    void setOnlyDrawbar (
        churchorgan::ChurchOrganCore& core,
        std::size_t selected,
        float position)
    {
        for (std::size_t drawbar = 0;
             drawbar < churchorgan::drawbarCount;
             ++drawbar)
            core.setDrawbarPosition (
                drawbar,
                drawbar == selected ? position : 0.0f);
    }

    void testFoldback (TestSuite& suite)
    {
        const auto& definitions =
            churchorgan::HarmonicFoldback::definitions();

        for (std::size_t drawbar = 0;
             drawbar < definitions.size();
             ++drawbar)
            for (int note = 0;
                 note < static_cast<int> (
                     churchorgan::midiNoteCount);
                 ++note)
            {
                const auto result =
                    churchorgan::HarmonicFoldback::map (
                        drawbar, note);
                const auto& definition = definitions[drawbar];

                suite.expect (result.valid,
                              "foldback returned invalid mapping");
                suite.expect (
                    result.foldedMidi
                        >= definition.minimumWheelMidi
                    && result.foldedMidi
                        <= definition.maximumWheelMidi,
                    "foldback escaped drawbar window");
                suite.expect (
                    result.wheelIndex >= 0
                    && result.wheelIndex
                        < churchorgan::HarmonicFoldback::wheelCount,
                    "foldback escaped shared bank");
                suite.expect (
                    (result.foldedMidi - result.requestedMidi)
                        % 12 == 0,
                    "foldback changed chromatic pitch class");
            }

        const auto low16 =
            churchorgan::HarmonicFoldback::map (0, 0);
        const auto highOne =
            churchorgan::HarmonicFoldback::map (8, 127);
        const auto sharedFour =
            churchorgan::HarmonicFoldback::map (3, 60);
        const auto sharedEight =
            churchorgan::HarmonicFoldback::map (2, 72);

        suite.expect (
            low16.valid
                && low16.requestedMidi == -12
                && low16.foldedMidi == 24,
            "16-foot lower foldback contract changed");
        suite.expect (
            highOne.valid
                && highOne.requestedMidi == 163
                && highOne.foldedMidi == 115,
            "1-foot upper foldback contract changed");
        suite.expect (
            sharedFour.valid
                && sharedEight.valid
                && sharedFour.wheelIndex
                    == sharedEight.wheelIndex,
            "two harmonics that request the same pitch do not share a wheel");

        churchorgan::ChurchOrganCore invalidCore;
        auto invalidSpec = specFor (128);
        invalidSpec.sampleRate = 12000.0;
        suite.expect (
            ! invalidCore.prepare (invalidSpec),
            "prepare accepted an alias-prone sample rate");
    }

    void testDrawbarRatios (TestSuite& suite)
    {
        constexpr std::array<float, 9> expected {{
            0.0f,
            0.0891250938f,
            0.1258925412f,
            0.1778279410f,
            0.2511886432f,
            0.3548133892f,
            0.5011872336f,
            0.7079457844f,
            1.0f
        }};

        for (std::size_t position = 0;
             position < expected.size();
             ++position)
            suite.expect (
                std::abs (
                    churchorgan::ChurchOrganCore::
                        gainForDrawbarPosition (
                            static_cast<float> (position))
                    - expected[position]) < 1.0e-7f,
                "drawbar calibration curve changed");

        suite.expect (
            std::abs (
                churchorgan::ChurchOrganCore::
                    gainForDrawbarPosition (7.0f)
                / churchorgan::ChurchOrganCore::
                    gainForDrawbarPosition (8.0f)
                - std::pow (10.0f, -3.0f / 20.0f))
                    < 1.0e-6f,
            "drawbar positions are not separated by 3 dB");

        const auto& definitions =
            churchorgan::HarmonicFoldback::definitions();
        const double base =
            churchorgan::TonewheelBank::
                equalTemperedFrequency (60);

        for (const auto& definition : definitions)
        {
            const double ratio =
                churchorgan::TonewheelBank::
                    equalTemperedFrequency (
                        60 + definition.semitoneOffset)
                / base;
            const double expectedRatio = std::pow (
                2.0,
                static_cast<double> (
                    definition.semitoneOffset) / 12.0);

            suite.expect (
                std::abs (ratio - expectedRatio) < 1.0e-12,
                "drawbar pitch ratio is inconsistent");
        }

        auto measuredRms = [] (float position)
        {
            churchorgan::ChurchOrganCore core;
            setOnlyDrawbar (core, 2, position);
            const auto spec = specFor (256);

            if (! core.prepare (spec)
                 || ! core.queueUpperNoteOn (60, 100))
                return 0.0;

            std::array<float, 256> mono {};
            double energy = 0.0;
            std::size_t samples = 0;

            for (int block = 0; block < 500; ++block)
            {
                core.render (
                    { mono.data(), nullptr, mono.size() });

                if (block < 20)
                    continue;

                for (float value : mono)
                {
                    energy +=
                        static_cast<double> (value) * value;
                    ++samples;
                }
            }

            return std::sqrt (
                energy / static_cast<double> (samples));
        };

        const double rmsEight = measuredRms (8.0f);
        const double rmsSeven = measuredRms (7.0f);
        suite.expect (
            rmsEight > 0.01
                && std::abs (
                    rmsSeven / rmsEight
                    - expected[7]) < 2.0e-4,
            "rendered drawbar level does not follow calibration");
    }

    void testDeterminismAndContinuity (TestSuite& suite)
    {
        churchorgan::ChurchOrganCore whole;
        churchorgan::ChurchOrganCore partitioned;

        for (auto* core : { &whole, &partitioned })
        {
            core->setDrawbarPosition (0, 8.0f);
            core->setDrawbarPosition (1, 8.0f);
            core->setDrawbarPosition (2, 8.0f);
        }

        auto wholeSpec = specFor (1024);
        auto partitionedSpec = specFor (256);
        wholeSpec.maximumWheelOffsetCents = 0.17f;
        partitionedSpec.maximumWheelOffsetCents = 0.17f;

        suite.expect (
            whole.prepare (wholeSpec)
                && partitioned.prepare (partitionedSpec),
            "determinism cores did not prepare");
        suite.expect (
            whole.queueUpperNoteOn (60, 100)
                && partitioned.queueUpperNoteOn (60, 100),
            "determinism note-on was rejected");

        std::array<float, 1024> wholeAudio {};
        std::array<float, 1024> partitionedAudio {};
        suite.expect (
            whole.render (
                { wholeAudio.data(), nullptr,
                  wholeAudio.size() }),
            "whole render failed");

        for (std::size_t block = 0; block < 4; ++block)
            suite.expect (
                partitioned.render (
                    { partitionedAudio.data() + block * 256,
                      nullptr, 256 }),
                "partitioned render failed");

        suite.expect (
            std::memcmp (
                wholeAudio.data(),
                partitionedAudio.data(),
                sizeof (wholeAudio)) == 0,
            "block partition changed deterministic output");

        float maximumDelta = 0.0f;

        for (std::size_t sample = 1;
             sample < wholeAudio.size();
             ++sample)
            maximumDelta = std::max (
                maximumDelta,
                std::abs (
                    wholeAudio[sample]
                    - wholeAudio[sample - 1]));

        suite.expect (
            maximumDelta < 0.08f,
            "phase continuity produced an implausible sample jump");

        churchorgan::ChurchOrganCore delayedWhole;
        churchorgan::ChurchOrganCore delayedParts;
        setOnlyDrawbar (delayedWhole, 2, 8.0f);
        setOnlyDrawbar (delayedParts, 2, 8.0f);
        suite.expect (
            delayedWhole.prepare (specFor (512))
                && delayedParts.prepare (specFor (128)),
            "delayed phase cores did not prepare");
        suite.expect (
            delayedWhole.queueUpperNoteOn (64, 100, 300)
                && delayedParts.queueUpperNoteOn (64, 100, 300),
            "future event was rejected");

        std::array<float, 512> delayedA {};
        std::array<float, 512> delayedB {};
        delayedWhole.render (
            { delayedA.data(), nullptr, delayedA.size() });

        for (std::size_t block = 0; block < 4; ++block)
            delayedParts.render (
                { delayedB.data() + block * 128,
                  nullptr, 128 });

        suite.expect (
            std::memcmp (
                delayedA.data(),
                delayedB.data(),
                sizeof (delayedA)) == 0,
            "future event or silent phase changed at a block boundary");

        float beforeEventPeak = 0.0f;
        float afterEventPeak = 0.0f;

        for (std::size_t sample = 0; sample < delayedA.size(); ++sample)
            if (sample < 300)
                beforeEventPeak = std::max (
                    beforeEventPeak,
                    std::abs (delayedA[sample]));
            else
                afterEventPeak = std::max (
                    afterEventPeak,
                    std::abs (delayedA[sample]));

        suite.expect (
            beforeEventPeak == 0.0f
                && afterEventPeak > 0.0001f,
            "sample-offset event timing is incorrect");

        churchorgan::ChurchOrganCore otherSeed;
        setOnlyDrawbar (otherSeed, 0, 8.0f);
        otherSeed.setDrawbarPosition (1, 8.0f);
        otherSeed.setDrawbarPosition (2, 8.0f);
        auto otherSpec = specFor (
            1024, 0xfedcba9876543210ULL);
        otherSpec.maximumWheelOffsetCents = 0.17f;
        suite.expect (
            otherSeed.prepare (otherSpec)
                && otherSeed.queueUpperNoteOn (60, 100),
            "different-seed core did not prepare");
        std::array<float, 1024> otherAudio {};
        otherSeed.render (
            { otherAudio.data(), nullptr,
              otherAudio.size() });
        suite.expect (
            std::memcmp (
                wholeAudio.data(),
                otherAudio.data(),
                sizeof (wholeAudio)) != 0,
            "different deterministic seeds produced identical output");
    }

    void testSmoothingDiagnosticsAndFiniteDc (
        TestSuite& suite)
    {
        churchorgan::ChurchOrganCore smoothing;
        setOnlyDrawbar (smoothing, 2, 0.0f);
        suite.expect (
            smoothing.prepare (specFor (128))
                && smoothing.queueUpperNoteOn (60, 100),
            "smoothing core did not prepare");

        std::array<float, 128> mono {};
        smoothing.render (
            { mono.data(), nullptr, mono.size() });
        smoothing.setDrawbarPosition (2, 8.0f);
        smoothing.render (
            { mono.data(), nullptr, mono.size() });
        const float earlyGain =
            smoothing.currentDrawbarGain (2);

        suite.expect (
            earlyGain > 0.0f && earlyGain < 0.5f,
            "drawbar target jumped without smoothing");

        for (int block = 0; block < 500; ++block)
            smoothing.render (
                { mono.data(), nullptr, mono.size() });

        suite.expect (
            smoothing.currentDrawbarGain (2) > 0.999f,
            "drawbar smoother did not converge");

        churchorgan::ChurchOrganCore finite;
        setOnlyDrawbar (finite, 2, 8.0f);
        suite.expect (
            finite.prepare (specFor (128))
                && finite.queueUpperNoteOn (60, 100),
            "finite/DC core did not prepare");

        double sum = 0.0;
        double energy = 0.0;
        std::size_t count = 0;
        bool allFinite = true;

        for (int block = 0; block < 1500; ++block)
        {
            finite.render (
                { mono.data(), nullptr, mono.size() });

            if (block < 20)
                continue;

            for (float value : mono)
            {
                allFinite = allFinite
                            && std::isfinite (value);
                sum += value;
                energy +=
                    static_cast<double> (value) * value;
                ++count;
            }
        }

        const double mean =
            sum / static_cast<double> (count);
        const double rms = std::sqrt (
            energy / static_cast<double> (count));
        const auto diagnostics = finite.diagnostics();

        suite.expect (allFinite,
                      "render produced NaN or infinity");
        suite.expect (
            std::abs (mean) < 1.0e-4,
            "tonewheel output has excessive DC");
        suite.expect (
            rms > 0.02 && rms < 0.12,
            "tonewheel RMS is outside the expected range");
        suite.expect (
            diagnostics.invalidOutputSamples == 0
                && diagnostics.activeUpperNotes == 1
                && diagnostics.lastBlockPeak > 0.01f
                && diagnostics.totalRenderedSamples
                    == 1500u * mono.size(),
            "diagnostics snapshot is inconsistent");
    }

    void testMonoStereoAndNoteOff (TestSuite& suite)
    {
        churchorgan::ChurchOrganCore mono;
        churchorgan::ChurchOrganCore stereo;
        setOnlyDrawbar (mono, 2, 8.0f);
        setOnlyDrawbar (stereo, 2, 8.0f);
        suite.expect (
            mono.prepare (specFor (512))
                && stereo.prepare (specFor (512))
                && mono.queueUpperNoteOn (67, 100)
                && stereo.queueUpperNoteOn (67, 100),
            "mono/stereo cores did not prepare");

        std::array<float, 512> monoAudio {};
        std::array<float, 512> stereoLeft {};
        std::array<float, 512> stereoRight {};
        suite.expect (
            mono.render (
                { monoAudio.data(), nullptr,
                  monoAudio.size() })
                && stereo.render (
                    { stereoLeft.data(), stereoRight.data(),
                      stereoLeft.size() }),
            "mono/stereo render failed");
        suite.expect (
            std::memcmp (
                monoAudio.data(),
                stereoLeft.data(),
                sizeof (monoAudio)) == 0,
            "stereo left differs from mono render");
        suite.expect (
            std::memcmp (
                stereoLeft.data(),
                stereoRight.data(),
                sizeof (stereoLeft)) == 0,
            "Phase-1 stereo output is not coherent dual mono");

        suite.expect (
            stereo.queueUpperNoteOff (67, 64),
            "note-off event was rejected");

        for (int block = 0; block < 20; ++block)
            stereo.render (
                { stereoLeft.data(), stereoRight.data(),
                  stereoLeft.size() });

        suite.expect (
            stereo.diagnostics().activeUpperNotes == 0,
            "note-off did not finish the contact release");
    }

    void testNoAllocationsAfterPrepare (TestSuite& suite)
    {
        churchorgan::ChurchOrganCore core;

        for (std::size_t drawbar = 0;
             drawbar < churchorgan::drawbarCount;
             ++drawbar)
            core.setDrawbarPosition (
                drawbar,
                static_cast<float> (8 - drawbar / 2));

        suite.expect (
            core.prepare (specFor (128))
                && core.queueUpperNoteOn (60, 100),
            "allocation-test core did not prepare");

        std::array<float, 128> left {};
        std::array<float, 128> right {};
        const auto before =
            allocationCount.load (std::memory_order_acquire);
        bool allRendered = true;

        for (int block = 0; block < 4000; ++block)
        {
            if (block % 257 == 0)
            {
                core.setDrawbarPosition (
                    static_cast<std::size_t> (
                        (block / 257)
                        % static_cast<int> (
                            churchorgan::drawbarCount)),
                    static_cast<float> (
                        (block / 257) % 9));
                core.queueUpperNoteOff (
                    60 + (block / 257) % 12, 64);
                core.queueUpperNoteOn (
                    60 + ((block / 257) + 1) % 12,
                    100, 65);
            }

            allRendered = allRendered
                          && core.render (
                              { left.data(), right.data(),
                                left.size() });
        }

        const auto after =
            allocationCount.load (std::memory_order_acquire);
        suite.expect (allRendered,
                      "allocation-test render failed");
        suite.expect (
            after == before,
            "render/event/drawbar path allocated after prepare");
        suite.expect (
            churchorgan::ChurchOrganCore::renderUsesFixedStorage,
            "core no longer declares fixed render storage");
    }
}

int main()
{
    TestSuite suite;
    testFoldback (suite);
    testDrawbarRatios (suite);
    testDeterminismAndContinuity (suite);
    testSmoothingDiagnosticsAndFiniteDc (suite);
    testMonoStereoAndNoteOff (suite);
    testNoAllocationsAfterPrepare (suite);

    if (suite.failures == 0)
    {
        std::cout
            << "CHURCH_ORGAN_CORE_TESTS_OK"
            << " checks=" << suite.checks
            << " allocations="
            << allocationCount.load (
                   std::memory_order_relaxed)
            << '\n';
        return 0;
    }

    std::cerr
        << "CHURCH_ORGAN_CORE_TESTS_FAILED"
        << " failures=" << suite.failures
        << " checks=" << suite.checks
        << '\n';
    return 1;
}
