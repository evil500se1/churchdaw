#include "HarmonicPercussion.h"
#include "KeyClickGenerator.h"
#include "MultiContactKeying.h"

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
                std::cerr << "CHURCH_ORGAN_ARTICULATION_FAIL: "
                          << message << '\n';
            }
        }
    };

    bool sameBits (float left, float right)
    {
        return std::memcmp (&left, &right, sizeof (float)) == 0;
    }

    void testMultiContactValidation (TestSuite& suite)
    {
        churchorgan::MultiContactKeying contacts;
        churchorgan::MultiContactParameters parameters;

        suite.expect (
            ! contacts.prepare (12000.0, 1, parameters),
            "multi-contact accepted invalid sample rate");

        parameters.bounceAmount = 0.8f;
        suite.expect (
            ! contacts.prepare (sampleRate, 1, parameters),
            "multi-contact accepted unsafe bounce");
    }

    void testMultiContactSequenceAndDeterminism (TestSuite& suite)
    {
        churchorgan::MultiContactKeying left;
        churchorgan::MultiContactKeying right;
        churchorgan::MultiContactKeying different;

        suite.expect (
            left.prepare (sampleRate, 0x1122334455667788ULL),
            "multi-contact left prepare failed");
        suite.expect (
            right.prepare (sampleRate, 0x1122334455667788ULL),
            "multi-contact right prepare failed");
        suite.expect (
            different.prepare (sampleRate, 0x8877665544332211ULL),
            "multi-contact different-seed prepare failed");

        left.noteOn (60, 100);
        right.noteOn (60, 100);
        different.noteOn (60, 100);

        std::array<int, churchorgan::articulationContactCount>
            firstAudible {};
        firstAudible.fill (-1);
        bool differentSeedObserved = false;

        for (int sample = 0; sample < 4096; ++sample)
        {
            left.advance();
            right.advance();
            different.advance();

            for (std::size_t contact = 0;
                 contact < firstAudible.size();
                 ++contact)
            {
                const float leftGain =
                    left.contactGain (60, contact);
                const float rightGain =
                    right.contactGain (60, contact);
                const float differentGain =
                    different.contactGain (60, contact);

                suite.expect (
                    sameBits (leftGain, rightGain),
                    "same seed lost bit-exact contact determinism");
                suite.expect (
                    std::isfinite (leftGain)
                        && leftGain >= 0.0f
                        && leftGain <= 1.0f,
                    "multi-contact gain escaped finite unit range");

                if (firstAudible[contact] < 0
                     && leftGain > 0.000001f)
                    firstAudible[contact] = sample;

                differentSeedObserved =
                    differentSeedObserved
                    || ! sameBits (leftGain, differentGain);
            }
        }

        int distinctStarts = 0;

        for (std::size_t contact = 0;
             contact < firstAudible.size();
             ++contact)
        {
            suite.expect (
                firstAudible[contact] >= 0,
                "multi-contact never closed a contact");
            suite.expect (
                left.contactGain (60, contact) > 0.99f,
                "multi-contact did not settle closed");

            bool firstAtThisTime = true;

            for (std::size_t prior = 0;
                 prior < contact;
                 ++prior)
                firstAtThisTime =
                    firstAtThisTime
                    && firstAudible[prior] != firstAudible[contact];

            if (firstAtThisTime)
                ++distinctStarts;
        }

        suite.expect (
            distinctStarts >= 6,
            "multi-contact stagger collapsed into too few contacts");
        suite.expect (
            firstAudible[2] < firstAudible[8],
            "8-foot contact should precede 1-foot contact");
        suite.expect (
            differentSeedObserved,
            "multi-contact seed did not affect micro-timing");

        left.noteOff (60);

        for (int sample = 0; sample < 8192; ++sample)
            left.advance();

        suite.expect (
            ! left.isHeld (60),
            "multi-contact note remained held after note-off");
        suite.expect (
            ! left.isAudible (60)
                && left.activeNoteCount() == 0,
            "multi-contact release did not retire voice");
    }

    void testKeyClick (TestSuite& suite)
    {
        churchorgan::KeyClickGenerator left;
        churchorgan::KeyClickGenerator right;
        churchorgan::KeyClickGenerator different;

        suite.expect (
            left.prepare (sampleRate, 0x12345678ULL),
            "key-click left prepare failed");
        suite.expect (
            right.prepare (sampleRate, 0x12345678ULL),
            "key-click right prepare failed");
        suite.expect (
            different.prepare (sampleRate, 0xabcdef01ULL),
            "key-click different-seed prepare failed");

        left.noteOn (64, 96);
        right.noteOn (64, 96);
        different.noteOn (64, 96);

        double onsetEnergy = 0.0;
        bool differentSeedObserved = false;

        for (int sample = 0; sample < 4096; ++sample)
        {
            const float a = left.renderSample();
            const float b = right.renderSample();
            const float c = different.renderSample();
            suite.expect (
                sameBits (a, b),
                "same seed lost bit-exact key-click determinism");
            suite.expect (
                std::isfinite (a),
                "key-click generated invalid sample");
            differentSeedObserved =
                differentSeedObserved || ! sameBits (a, c);
            onsetEnergy +=
                static_cast<double> (a) * static_cast<double> (a);
        }

        suite.expect (
            onsetEnergy > 0.000001,
            "key-click onset has no energy");
        suite.expect (
            differentSeedObserved,
            "key-click seed did not affect noise");
        suite.expect (
            left.activeClickCount() == 0,
            "key-click onset failed to retire");

        left.noteOff (64);
        double releaseEnergy = 0.0;

        for (int sample = 0; sample < 4096; ++sample)
        {
            const float value = left.renderSample();
            releaseEnergy +=
                static_cast<double> (value)
                * static_cast<double> (value);
        }

        suite.expect (
            releaseEnergy > 0.0000001,
            "key-click release has no energy");
        suite.expect (
            ! left.isHeld (64)
                && left.activeClickCount() == 0,
            "key-click release failed to retire");
    }

    float gainAfter (
        churchorgan::PercussionTiming timing,
        int samples)
    {
        churchorgan::HarmonicPercussionParameters parameters;
        parameters.timing = timing;
        churchorgan::HarmonicPercussion percussion;
        percussion.prepare (sampleRate, parameters);
        percussion.noteOn (60, 100);

        for (int sample = 0; sample < samples; ++sample)
            percussion.advanceEnvelope();

        return percussion.currentGain();
    }

    float peakFor (churchorgan::PercussionVolume volume)
    {
        churchorgan::HarmonicPercussionParameters parameters;
        parameters.volume = volume;
        churchorgan::HarmonicPercussion percussion;
        percussion.prepare (sampleRate, parameters);
        percussion.noteOn (60, 100);
        float peak = 0.0f;

        for (int sample = 0; sample < 256; ++sample)
            peak = std::max (
                peak, percussion.advanceEnvelope());

        return peak;
    }

    void testPercussionModes (TestSuite& suite)
    {
        churchorgan::HarmonicPercussion percussion;
        churchorgan::HarmonicPercussionParameters parameters;

        suite.expect (
            percussion.prepare (sampleRate, parameters),
            "percussion prepare failed");
        suite.expect (
            percussion.noteOn (60, 100),
            "single-trigger failed first note");
        suite.expect (
            percussion.triggeredMidiNote() == 60
                && percussion.requestedHarmonicMidi() == 79
                && percussion.harmonicDrawbarIndex() == 4,
            "third-harmonic routing is incorrect");
        suite.expect (
            ! percussion.noteOn (64, 100)
                && percussion.triggeredMidiNote() == 60,
            "single-trigger retriggered during legato");

        percussion.noteOff (60);
        percussion.noteOff (64);
        suite.expect (
            percussion.heldNoteCount() == 0,
            "single-trigger did not rearm after all releases");
        suite.expect (
            percussion.noteOn (67, 100)
                && percussion.triggeredMidiNote() == 67,
            "single-trigger failed after rearm");

        percussion.setHarmonic (
            churchorgan::PercussionHarmonic::second);
        suite.expect (
            percussion.requestedHarmonicMidi() == 79
                && percussion.harmonicSemitoneOffset() == 12
                && percussion.harmonicDrawbarIndex() == 3,
            "second-harmonic routing is incorrect");

        percussion.allNotesOff();
        percussion.setTriggerMode (
            churchorgan::PercussionTriggerMode::retrigger);
        suite.expect (
            percussion.noteOn (60, 90),
            "retrigger mode failed first note");
        suite.expect (
            percussion.noteOn (64, 90)
                && percussion.triggeredMidiNote() == 64,
            "retrigger mode failed legato note");

        const int observationSamples =
            static_cast<int> (sampleRate * 0.18);
        const float fast = gainAfter (
            churchorgan::PercussionTiming::fast,
            observationSamples);
        const float slow = gainAfter (
            churchorgan::PercussionTiming::slow,
            observationSamples);
        suite.expect (
            slow > fast * 2.0f,
            "slow percussion does not outlast fast percussion");

        const float softPeak = peakFor (
            churchorgan::PercussionVolume::soft);
        const float normalPeak = peakFor (
            churchorgan::PercussionVolume::normal);
        suite.expect (
            normalPeak > softPeak * 1.8f,
            "normal percussion is not materially above soft");

        double carrierEnergy = 0.0;

        for (int sample = 0; sample < 2048; ++sample)
        {
            const float carrier = std::sin (
                static_cast<float> (sample) * 0.071f);
            const float value = percussion.processSample (carrier);
            suite.expect (
                std::isfinite (value),
                "percussion produced invalid carrier output");
            carrierEnergy +=
                static_cast<double> (value)
                * static_cast<double> (value);
        }

        suite.expect (
            carrierEnergy > 0.000001,
            "percussion carrier output has no energy");
    }

    void testRenderPathAllocations (TestSuite& suite)
    {
        churchorgan::MultiContactKeying contacts;
        churchorgan::KeyClickGenerator click;
        churchorgan::HarmonicPercussion percussion;

        suite.expect (
            contacts.prepare (sampleRate, 0x10101010ULL),
            "allocation test multi-contact prepare failed");
        suite.expect (
            click.prepare (sampleRate, 0x20202020ULL),
            "allocation test key-click prepare failed");
        suite.expect (
            percussion.prepare (sampleRate),
            "allocation test percussion prepare failed");

        contacts.noteOn (48, 100);
        click.noteOn (48, 100);
        percussion.noteOn (48, 100);
        const auto before =
            allocationCount.load (std::memory_order_relaxed);
        float sink = 0.0f;

        for (int sample = 0; sample < 96000; ++sample)
        {
            if (sample == 24000)
            {
                contacts.noteOn (55, 80);
                click.noteOn (55, 80);
                percussion.noteOn (55, 80);
            }

            if (sample == 48000)
            {
                contacts.allNotesOff();
                click.allNotesOff();
                percussion.allNotesOff();
            }

            contacts.advance();
            sink += contacts.contactGain (
                48,
                static_cast<std::size_t> (sample % 9));
            sink += click.renderSample();
            sink += percussion.processSample (
                std::sin (static_cast<float> (sample) * 0.037f));
        }

        const auto after =
            allocationCount.load (std::memory_order_relaxed);
        suite.expect (
            after == before,
            "audio path allocated memory after prepare");
        suite.expect (
            std::isfinite (sink),
            "allocation test sink became invalid");
    }
}

int main()
{
    TestSuite suite;
    testMultiContactValidation (suite);
    testMultiContactSequenceAndDeterminism (suite);
    testKeyClick (suite);
    testPercussionModes (suite);
    testRenderPathAllocations (suite);

    if (suite.failures != 0)
    {
        std::cerr << "CHURCH_ORGAN_ARTICULATION_TESTS_FAILED checks="
                  << suite.checks
                  << " failures=" << suite.failures
                  << '\n';
        return 1;
    }

    std::cout << "CHURCH_ORGAN_ARTICULATION_TESTS_OK checks="
              << suite.checks
              << " allocations="
              << allocationCount.load (std::memory_order_relaxed)
              << '\n';
    return 0;
}
