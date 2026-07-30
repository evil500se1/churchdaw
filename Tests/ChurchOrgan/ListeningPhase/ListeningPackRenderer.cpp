#include "OrganParameters.h"
#include "TonewheelOrganEngine.h"
#include "TransistorOrganEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

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
    constexpr std::uint32_t sampleRateInteger = 48000;
    constexpr std::size_t blockSize = 128;
    constexpr float minimumSafePeak = 0.015f;
    constexpr float maximumSafePeak = 0.9995f;
    constexpr float maximumApproximateTruePeak = 1.05f;
    constexpr double maximumDc = 0.005;
    constexpr double maximumDcToRms = 0.035;
    constexpr double minimumRmsDecibels = -50.0;
    constexpr double maximumRmsDecibels = -3.0;
    constexpr double minimumCrestDecibels = 1.0;
    constexpr double maximumCrestDecibels = 30.0;
    constexpr double minimumMonoCompatibilityDecibels = -12.0;
    constexpr double maximumTailRmsDecibels = -40.0;
    constexpr float maximumTailPeak = 0.10f;
    constexpr float maximumBoundaryJump = 0.40f;
    constexpr std::uint64_t fnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t fnvPrime = 1099511628211ULL;

    enum class SceneEngine : std::uint8_t
    {
        tonewheel,
        transistor
    };

    enum class TonePreset : std::uint8_t
    {
        clean,
        aged,
        road,
        classicDry,
        gospelMotion,
        softWorship
    };

    enum class EventAction : std::uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff
    };

    struct TimedEvent
    {
        std::uint64_t sample = 0;
        EventAction action = EventAction::noteOff;
        churchorgan::Manual manual = churchorgan::Manual::upper;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
    };

    struct TimedControl
    {
        std::uint64_t sample = 0;
        churchorgan::OrganParameterId parameter =
            churchorgan::OrganParameterId::masterGainDb;
        float value = 0.0f;
    };

    struct Scene
    {
        SceneEngine engine = SceneEngine::tonewheel;
        TonePreset tonePreset = TonePreset::clean;
        churchorgan::TransistorOrganProfile transistorProfile =
            churchorgan::TransistorOrganProfile::britishCombo;
        const char* label = "";
        const char* fileName = "";
        double durationSeconds = 16.0;
        int transpose = 0;
        std::uint64_t seed = 0;
        std::vector<TimedEvent> events;
        std::vector<TimedControl> controls;
    };

    struct RenderAudit
    {
        std::uint64_t checksum = fnvOffset;
        std::uint64_t renderAllocations = 0;
        std::uint64_t invalidEngineSamples = 0;
        bool renderSucceeded = true;
    };

    struct Metrics
    {
        double dcLeft = 0.0;
        double dcRight = 0.0;
        double dcMaximum = 0.0;
        double dcToRms = 0.0;
        double rms = 0.0;
        double rmsDecibels = -200.0;
        float peak = 0.0f;
        float approximateTruePeak4x = 0.0f;
        double crestDecibels = 0.0;
        float maximumSampleJump = 0.0f;
        float maximumBlockBoundaryJump = 0.0f;
        double deltaRms = 0.0;
        double monoCompatibilityDecibels = -200.0;
        double tailRmsDecibels = -200.0;
        float tailPeak = 0.0f;
        std::uint64_t invalidSamples = 0;
    };

    struct SceneReport
    {
        Scene scene;
        RenderAudit audit;
        Metrics metrics;
        std::filesystem::path path;
        std::vector<std::string> failures;
    };

    struct LoudnessProbe
    {
        const char* engine = "";
        int notes = 0;
        double rmsDecibels = -200.0;
        float peak = 0.0f;
        double crestDecibels = 0.0;
        double deltaFromPreviousDecibels = 0.0;
        std::uint64_t renderAllocations = 0;
        std::uint64_t invalidSamples = 0;
        bool passed = true;
    };

    std::uint64_t sampleAt (double seconds) noexcept
    {
        return static_cast<std::uint64_t> (
            std::llround (seconds * sampleRate));
    }

    std::uint64_t controlSampleAt (double seconds) noexcept
    {
        const auto raw = sampleAt (seconds);
        return ((raw + blockSize / 2) / blockSize) * blockSize;
    }

    int clampMidi (int note) noexcept
    {
        return std::max (0, std::min (127, note));
    }

    double decibels (double gain) noexcept
    {
        return gain > 1.0e-20
                   ? 20.0 * std::log10 (gain)
                   : -200.0;
    }

    void addEvent (
        std::vector<TimedEvent>& events,
        double seconds,
        EventAction action,
        churchorgan::Manual manual,
        int note,
        int velocity = 0)
    {
        events.push_back ({
            sampleAt (seconds),
            action,
            manual,
            static_cast<std::uint8_t> (clampMidi (note)),
            static_cast<std::uint8_t> (
                std::max (0, std::min (127, velocity)))
        });
    }

    void addNote (
        std::vector<TimedEvent>& events,
        churchorgan::Manual manual,
        int note,
        int velocity,
        double start,
        double duration)
    {
        addEvent (
            events,
            start,
            EventAction::noteOn,
            manual,
            note,
            velocity);
        addEvent (
            events,
            start + duration,
            EventAction::noteOff,
            manual,
            note);
    }

    template <std::size_t count>
    void addChord (
        std::vector<TimedEvent>& events,
        churchorgan::Manual manual,
        const std::array<int, count>& notes,
        int transpose,
        int velocity,
        double start,
        double duration)
    {
        for (std::size_t i = 0; i < notes.size(); ++i)
            addNote (
                events,
                manual,
                notes[i] + transpose,
                std::max (1, velocity - static_cast<int> (i) * 2),
                start,
                duration);
    }

    void addGlissando (
        std::vector<TimedEvent>& events,
        int firstNote,
        int lastNote,
        int transpose,
        double start,
        double stepSeconds)
    {
        const int direction = lastNote >= firstNote ? 1 : -1;
        int index = 0;

        for (int note = firstNote;
             note != lastNote + direction;
             note += direction)
        {
            const double onset =
                start + static_cast<double> (index) * stepSeconds;
            addNote (
                events,
                churchorgan::Manual::upper,
                note + transpose,
                82 + (index % 5) * 7,
                onset,
                0.095);
            ++index;
        }
    }

    void addPalmSmear (
        std::vector<TimedEvent>& events,
        int transpose,
        double start)
    {
        constexpr std::array<int, 12> smear {{
            48, 49, 51, 52, 54, 55,
            57, 58, 60, 61, 63, 64
        }};

        for (std::size_t i = 0; i < smear.size(); ++i)
            addNote (
                events,
                churchorgan::Manual::upper,
                smear[i] + transpose,
                74 + static_cast<int> (i % 4) * 10,
                start + static_cast<double> (i) * 0.012,
                0.18 + static_cast<double> (i % 3) * 0.025);
    }

    void addAllNotesOff (
        std::vector<TimedEvent>& events,
        double seconds)
    {
        for (auto manual : {
                 churchorgan::Manual::upper,
                 churchorgan::Manual::lower,
                 churchorgan::Manual::pedal })
            addEvent (
                events,
                seconds,
                EventAction::allNotesOff,
                manual,
                0);
    }

    void sortEvents (std::vector<TimedEvent>& events)
    {
        std::stable_sort (
            events.begin(),
            events.end(),
            [] (const TimedEvent& left, const TimedEvent& right)
            {
                if (left.sample != right.sample)
                    return left.sample < right.sample;

                return static_cast<int> (left.action)
                       > static_cast<int> (right.action);
            });
    }

    void addControl (
        std::vector<TimedControl>& controls,
        double seconds,
        churchorgan::OrganParameterId parameter,
        float value)
    {
        controls.push_back ({
            controlSampleAt (seconds), parameter, value
        });
    }

    void setDrawbars (
        std::vector<TimedControl>& controls,
        bool upper,
        const std::array<float, churchorgan::drawbarCount>& values)
    {
        constexpr std::array<churchorgan::OrganParameterId,
                             churchorgan::drawbarCount>
            upperIds {{
                churchorgan::OrganParameterId::upper16,
                churchorgan::OrganParameterId::upper5AndOneThird,
                churchorgan::OrganParameterId::upper8,
                churchorgan::OrganParameterId::upper4,
                churchorgan::OrganParameterId::upper2AndTwoThirds,
                churchorgan::OrganParameterId::upper2,
                churchorgan::OrganParameterId::upper1AndThreeFifths,
                churchorgan::OrganParameterId::upper1AndOneThird,
                churchorgan::OrganParameterId::upper1
            }};
        constexpr std::array<churchorgan::OrganParameterId,
                             churchorgan::drawbarCount>
            lowerIds {{
                churchorgan::OrganParameterId::lower16,
                churchorgan::OrganParameterId::lower5AndOneThird,
                churchorgan::OrganParameterId::lower8,
                churchorgan::OrganParameterId::lower4,
                churchorgan::OrganParameterId::lower2AndTwoThirds,
                churchorgan::OrganParameterId::lower2,
                churchorgan::OrganParameterId::lower1AndThreeFifths,
                churchorgan::OrganParameterId::lower1AndOneThird,
                churchorgan::OrganParameterId::lower1
            }};
        const auto& ids = upper ? upperIds : lowerIds;

        for (std::size_t i = 0; i < values.size(); ++i)
            addControl (controls, 0.0, ids[i], values[i]);
    }

    std::vector<TimedEvent> makeTonewheelPhrase (int transpose)
    {
        std::vector<TimedEvent> events;
        events.reserve (192);
        constexpr std::array<int, 5> chordOne {{ 60, 64, 67, 74, 76 }};
        constexpr std::array<int, 5> chordTwo {{ 57, 60, 65, 69, 76 }};
        constexpr std::array<int, 5> chordThree {{ 53, 57, 60, 67, 69 }};
        constexpr std::array<int, 5> chordFour {{ 55, 60, 64, 67, 74 }};
        constexpr std::array<int, 3> lowerOne {{ 48, 55, 59 }};
        constexpr std::array<int, 3> lowerTwo {{ 45, 52, 55 }};
        constexpr std::array<int, 3> lowerThree {{ 41, 48, 52 }};
        constexpr std::array<int, 3> lowerFour {{ 43, 50, 52 }};

        addChord (
            events, churchorgan::Manual::upper,
            chordOne, transpose, 102, 0.35, 1.95);
        addChord (
            events, churchorgan::Manual::lower,
            lowerOne, transpose, 88, 0.35, 1.95);
        addNote (
            events, churchorgan::Manual::pedal,
            36 + transpose, 96, 0.35, 1.95);

        addChord (
            events, churchorgan::Manual::upper,
            chordTwo, transpose, 106, 2.55, 1.85);
        addChord (
            events, churchorgan::Manual::lower,
            lowerTwo, transpose, 86, 2.55, 1.85);
        addNote (
            events, churchorgan::Manual::pedal,
            33 + transpose, 94, 2.55, 1.85);

        addChord (
            events, churchorgan::Manual::upper,
            chordThree, transpose, 98, 4.65, 1.55);
        addChord (
            events, churchorgan::Manual::lower,
            lowerThree, transpose, 84, 4.65, 1.55);
        addNote (
            events, churchorgan::Manual::pedal,
            29 + transpose, 92, 4.65, 1.55);

        addGlissando (events, 55, 82, transpose, 6.34, 0.018);
        addPalmSmear (events, transpose, 7.12);

        constexpr std::array<int, 8> staccato {{
            72, 76, 79, 74, 77, 81, 79, 84
        }};

        for (std::size_t i = 0; i < staccato.size(); ++i)
            addNote (
                events,
                churchorgan::Manual::upper,
                staccato[i] + transpose,
                92 + static_cast<int> (i % 3) * 9,
                8.25 + static_cast<double> (i) * 0.31,
                i % 2 == 0 ? 0.13 : 0.19);

        addChord (
            events, churchorgan::Manual::upper,
            chordFour, transpose, 108, 11.05, 2.15);
        addChord (
            events, churchorgan::Manual::lower,
            lowerFour, transpose, 88, 11.05, 2.15);
        addNote (
            events, churchorgan::Manual::pedal,
            31 + transpose, 98, 11.05, 2.15);

        addAllNotesOff (events, 13.35);
        sortEvents (events);
        return events;
    }

    std::vector<TimedEvent> makeTransistorPhrase (int transpose)
    {
        std::vector<TimedEvent> events;
        events.reserve (160);
        constexpr std::array<int, 5> chordOne {{ 60, 64, 67, 71, 74 }};
        constexpr std::array<int, 5> chordTwo {{ 58, 62, 65, 69, 72 }};
        constexpr std::array<int, 5> chordThree {{ 55, 60, 64, 67, 74 }};

        addChord (
            events, churchorgan::Manual::upper,
            chordOne, transpose, 104, 0.30, 2.05);
        addChord (
            events, churchorgan::Manual::upper,
            chordTwo, transpose, 110, 2.58, 1.82);
        addChord (
            events, churchorgan::Manual::upper,
            chordThree, transpose, 100, 4.62, 1.72);
        addGlissando (events, 52, 81, transpose, 6.52, 0.017);
        addPalmSmear (events, transpose, 7.28);

        constexpr std::array<int, 10> phrase {{
            67, 70, 74, 77, 75, 72, 79, 77, 82, 79
        }};

        for (std::size_t i = 0; i < phrase.size(); ++i)
            addNote (
                events,
                churchorgan::Manual::upper,
                phrase[i] + transpose,
                88 + static_cast<int> (i % 4) * 8,
                8.24 + static_cast<double> (i) * 0.285,
                i % 3 == 0 ? 0.11 : 0.17);

        addChord (
            events, churchorgan::Manual::upper,
            chordOne, transpose - 5, 106, 11.28, 1.72);
        addAllNotesOff (events, 13.15);
        sortEvents (events);
        return events;
    }

    void configureTonePreset (Scene& scene)
    {
        auto& controls = scene.controls;
        controls.reserve (96);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::pedal16, 4.0f);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::pedal8, 2.0f);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::rotaryHornLevel, 0.0f);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::rotaryDrumLevel, 0.0f);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::rotaryStereoWidth, 0.72f);
        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::hum, 0.0f);

        switch (scene.tonePreset)
        {
            case TonePreset::clean:
                setDrawbars (
                    controls, true,
                    {{ 5, 0, 8, 4, 0, 1, 0, 0, 0 }});
                setDrawbars (
                    controls, false,
                    {{ 0, 0, 7, 3, 0, 0, 0, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -9.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.05f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.03f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.02f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.28f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.18f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.08f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tone, 0.58f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 4.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMix, 0.28f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 0.72f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 0.0f);
                addControl (controls, 3.0, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                addControl (controls, 6.0, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                addControl (controls, 10.0, churchorgan::OrganParameterId::rotaryMode, 3.0f);
                addControl (controls, 11.2, churchorgan::OrganParameterId::rotaryMode, 0.0f);
                break;

            case TonePreset::aged:
                setDrawbars (
                    controls, true,
                    {{ 7, 1, 8, 5, 2, 2, 1, 0, 0 }});
                setDrawbars (
                    controls, false,
                    {{ 2, 0, 8, 4, 0, 1, 0, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -10.5f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.38f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.16f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.13f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.42f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.48f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.24f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tone, 0.50f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 5.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMix, 0.48f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 0.86f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                addControl (controls, 4.5, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                addControl (controls, 9.0, churchorgan::OrganParameterId::rotaryMode, 3.0f);
                addControl (controls, 10.5, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                break;

            case TonePreset::road:
                setDrawbars (
                    controls, true,
                    {{ 8, 3, 8, 6, 4, 4, 3, 2, 1 }});
                setDrawbars (
                    controls, false,
                    {{ 5, 1, 8, 5, 2, 2, 1, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -13.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 2.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.68f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.30f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.25f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::hum, 0.025f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.55f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.68f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.48f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 3.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tone, 0.46f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 6.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMix, 0.62f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 0.94f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryRoom, 0.30f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                addControl (controls, 5.0, churchorgan::OrganParameterId::rotaryMode, 3.0f);
                addControl (controls, 6.2, churchorgan::OrganParameterId::rotaryMode, 0.0f);
                addControl (controls, 8.0, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                addControl (controls, 11.0, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                break;

            case TonePreset::classicDry:
                setDrawbars (
                    controls, true,
                    {{ 8, 0, 8, 5, 0, 3, 0, 0, 0 }});
                setDrawbars (
                    controls, false,
                    {{ 0, 0, 8, 3, 0, 0, 0, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -9.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.10f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.02f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.02f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.36f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.20f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.06f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 0.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::percussionEnabled, 1.0f);
                break;

            case TonePreset::gospelMotion:
                setDrawbars (
                    controls, true,
                    {{ 8, 8, 8, 8, 0, 8, 0, 8, 8 }});
                setDrawbars (
                    controls, false,
                    {{ 8, 0, 8, 6, 0, 3, 0, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -14.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.34f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.14f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.12f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.48f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.62f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.62f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 3.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tone, 0.56f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 4.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMix, 0.32f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::percussionEnabled, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::percussionHarmonic, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::percussionVolume, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 1.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                addControl (controls, 2.5, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                addControl (controls, 10.0, churchorgan::OrganParameterId::rotaryMode, 3.0f);
                addControl (controls, 11.2, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                break;

            case TonePreset::softWorship:
                setDrawbars (
                    controls, true,
                    {{ 3, 0, 8, 3, 0, 1, 0, 0, 0 }});
                setDrawbars (
                    controls, false,
                    {{ 0, 0, 7, 2, 0, 1, 0, 0, 0 }});
                addControl (controls, 0.0, churchorgan::OrganParameterId::masterGainDb, -10.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tonewheelProfile, 3.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::age, 0.14f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::leakage, 0.04f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::crosstalk, 0.03f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::keyClick, 0.18f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::transformer, 0.30f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::drive, 0.14f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::ampProfile, 4.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::tone, 0.62f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMode, 6.0f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::scannerMix, 0.68f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::worshipIntensity, 0.88f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMix, 0.76f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryRoom, 0.44f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryStereoWidth, 0.92f);
                addControl (controls, 0.0, churchorgan::OrganParameterId::rotaryMode, 0.0f);
                addControl (controls, 2.0, churchorgan::OrganParameterId::rotaryMode, 1.0f);
                addControl (controls, 8.5, churchorgan::OrganParameterId::rotaryMode, 2.0f);
                addControl (controls, 13.0, churchorgan::OrganParameterId::rotaryMode, 3.0f);
                break;
        }

        addControl (
            controls, 0.0,
            churchorgan::OrganParameterId::expression, 0.68f);
        addControl (
            controls, 1.10,
            churchorgan::OrganParameterId::expression, 0.92f);
        addControl (
            controls, 2.10,
            churchorgan::OrganParameterId::expression, 1.00f);
        addControl (
            controls, 4.10,
            churchorgan::OrganParameterId::expression, 0.74f);
        addControl (
            controls, 5.20,
            churchorgan::OrganParameterId::expression, 0.96f);
        addControl (
            controls, 8.00,
            churchorgan::OrganParameterId::expression, 0.82f);
        addControl (
            controls, 11.00,
            churchorgan::OrganParameterId::expression, 0.66f);
        addControl (
            controls, 12.20,
            churchorgan::OrganParameterId::expression, 1.00f);

        std::stable_sort (
            controls.begin(),
            controls.end(),
            [] (const TimedControl& left, const TimedControl& right)
            {
                return left.sample < right.sample;
            });
    }

    std::vector<Scene> makeScenes()
    {
        std::vector<Scene> scenes;
        scenes.reserve (9);

        auto addTone = [&] (
            TonePreset preset,
            const char* label,
            const char* fileName,
            int transpose,
            std::uint64_t seed)
        {
            Scene scene;
            scene.engine = SceneEngine::tonewheel;
            scene.tonePreset = preset;
            scene.label = label;
            scene.fileName = fileName;
            scene.durationSeconds = 16.0;
            scene.transpose = transpose;
            scene.seed = seed;
            scene.events = makeTonewheelPhrase (transpose);
            configureTonePreset (scene);
            scenes.push_back (std::move (scene));
        };

        addTone (
            TonePreset::clean,
            "Tonewheel Clean",
            "01_tonewheel_clean.wav",
            0,
            0x1001001001001001ULL);
        addTone (
            TonePreset::aged,
            "Tonewheel Aged",
            "02_tonewheel_aged.wav",
            -2,
            0x2002002002002002ULL);
        addTone (
            TonePreset::road,
            "Tonewheel Road",
            "03_tonewheel_road.wav",
            2,
            0x3003003003003003ULL);
        addTone (
            TonePreset::classicDry,
            "Classic Dry",
            "04_classic_dry.wav",
            -5,
            0x4004004004004004ULL);
        addTone (
            TonePreset::gospelMotion,
            "Gospel Motion",
            "05_gospel_motion.wav",
            1,
            0x5005005005005005ULL);
        addTone (
            TonePreset::softWorship,
            "Soft Worship",
            "06_soft_worship.wav",
            -3,
            0x6006006006006006ULL);

        const auto addTransistor = [&] (
            churchorgan::TransistorOrganProfile profile,
            const char* label,
            const char* fileName,
            int transpose,
            std::uint64_t seed)
        {
            Scene scene;
            scene.engine = SceneEngine::transistor;
            scene.transistorProfile = profile;
            scene.label = label;
            scene.fileName = fileName;
            scene.durationSeconds = 15.5;
            scene.transpose = transpose;
            scene.seed = seed;
            scene.events = makeTransistorPhrase (transpose);
            scenes.push_back (std::move (scene));
        };

        addTransistor (
            churchorgan::TransistorOrganProfile::britishCombo,
            "Transistor British",
            "07_transistor_british.wav",
            0,
            0x7007007007007007ULL);
        addTransistor (
            churchorgan::TransistorOrganProfile::italianCombo,
            "Transistor Italian",
            "08_transistor_italian.wav",
            3,
            0x8008008008008008ULL);
        addTransistor (
            churchorgan::TransistorOrganProfile::germanCombo,
            "Transistor German",
            "09_transistor_german.wav",
            -3,
            0x9009009009009009ULL);
        return scenes;
    }

    std::int32_t quantise24 (float value) noexcept
    {
        if (! std::isfinite (value))
            value = 0.0f;

        const double clamped =
            std::max (-1.0, std::min (
                8388607.0 / 8388608.0,
                static_cast<double> (value)));
        return static_cast<std::int32_t> (
            std::llround (clamped * 8388608.0));
    }

    void hashPcm24 (
        std::uint64_t& checksum,
        float value) noexcept
    {
        const auto sample =
            static_cast<std::uint32_t> (quantise24 (value));

        for (unsigned int byte = 0; byte < 3; ++byte)
        {
            checksum ^=
                static_cast<std::uint8_t> (
                    sample >> (byte * 8));
            checksum *= fnvPrime;
        }
    }

    bool queueTonewheelEvent (
        churchorgan::TonewheelOrganEngine& engine,
        const TimedEvent& event,
        std::uint32_t offset) noexcept
    {
        switch (event.action)
        {
            case EventAction::noteOn:
                return engine.queueNoteOn (
                    event.manual,
                    event.note,
                    event.velocity,
                    offset);

            case EventAction::noteOff:
                return engine.queueNoteOff (
                    event.manual,
                    event.note,
                    offset);

            case EventAction::allNotesOff:
                return engine.queueAllNotesOff (
                    event.manual,
                    offset);
        }

        return false;
    }

    RenderAudit renderTonewheel (
        const Scene& scene,
        std::vector<float>* capturedInterleaved)
    {
        RenderAudit audit;
        auto engine =
            std::make_unique<churchorgan::TonewheelOrganEngine>();
        churchorgan::TonewheelOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = scene.seed;
        spec.quality = churchorgan::OrganQualityProfile::standard;

        if (! engine->prepare (spec))
        {
            audit.renderSucceeded = false;
            return audit;
        }

        const auto totalSamples = sampleAt (scene.durationSeconds);

        if (capturedInterleaved != nullptr)
            capturedInterleaved->assign (
                static_cast<std::size_t> (totalSamples) * 2,
                0.0f);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        std::size_t eventIndex = 0;
        std::size_t controlIndex = 0;

        for (std::uint64_t blockStart = 0;
             blockStart < totalSamples;
             blockStart += blockSize)
        {
            while (controlIndex < scene.controls.size()
                   && scene.controls[controlIndex].sample <= blockStart)
            {
                engine->parameters().setPlain (
                    scene.controls[controlIndex].parameter,
                    scene.controls[controlIndex].value);
                ++controlIndex;
            }

            const auto count = static_cast<std::size_t> (
                std::min<std::uint64_t> (
                    blockSize, totalSamples - blockStart));
            const auto blockEnd = blockStart + count;

            while (eventIndex < scene.events.size()
                   && scene.events[eventIndex].sample < blockEnd)
            {
                if (scene.events[eventIndex].sample >= blockStart)
                {
                    const auto offset = static_cast<std::uint32_t> (
                        scene.events[eventIndex].sample - blockStart);
                    if (! queueTonewheelEvent (
                            *engine,
                            scene.events[eventIndex],
                            offset))
                        audit.renderSucceeded = false;
                }

                ++eventIndex;
            }

            const auto before =
                allocationCount.load (std::memory_order_relaxed);
            const bool rendered = engine->render ({
                left.data(), right.data(), count
            });
            const auto after =
                allocationCount.load (std::memory_order_relaxed);
            audit.renderAllocations += after - before;
            audit.renderSucceeded =
                audit.renderSucceeded && rendered;

            for (std::size_t sample = 0; sample < count; ++sample)
            {
                const auto absolute =
                    static_cast<std::size_t> (blockStart) + sample;

                if (capturedInterleaved != nullptr)
                {
                    (*capturedInterleaved)[absolute * 2] =
                        left[sample];
                    (*capturedInterleaved)[absolute * 2 + 1] =
                        right[sample];
                }

                hashPcm24 (audit.checksum, left[sample]);
                hashPcm24 (audit.checksum, right[sample]);
            }
        }

        audit.invalidEngineSamples =
            engine->diagnostics().invalidOutputSamples;
        return audit;
    }

    RenderAudit renderTransistor (
        const Scene& scene,
        std::vector<float>* capturedInterleaved)
    {
        RenderAudit audit;
        auto engine =
            std::make_unique<churchorgan::TransistorOrganEngine>();
        churchorgan::TransistorOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = scene.seed;
        spec.profile = scene.transistorProfile;
        spec.outputTrim = 0.045f;
        spec.drive =
            scene.transistorProfile
                    == churchorgan::TransistorOrganProfile::italianCombo
                ? 0.26f
                : scene.transistorProfile
                          == churchorgan::TransistorOrganProfile::germanCombo
                      ? 0.16f
                      : 0.20f;
        spec.stereoWidth = 0.24f;
        spec.vibratoDepth = 0.58f;
        spec.vibratoEnabled = true;

        if (! engine->prepare (spec))
        {
            audit.renderSucceeded = false;
            return audit;
        }

        const auto totalSamples = sampleAt (scene.durationSeconds);

        if (capturedInterleaved != nullptr)
            capturedInterleaved->assign (
                static_cast<std::size_t> (totalSamples) * 2,
                0.0f);

        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        std::size_t eventIndex = 0;

        for (std::uint64_t blockStart = 0;
             blockStart < totalSamples;
             blockStart += blockSize)
        {
            const auto count = static_cast<std::size_t> (
                std::min<std::uint64_t> (
                    blockSize, totalSamples - blockStart));
            const auto blockEnd = blockStart + count;

            while (eventIndex < scene.events.size()
                   && scene.events[eventIndex].sample < blockEnd)
            {
                if (scene.events[eventIndex].sample >= blockStart)
                {
                    const auto& event = scene.events[eventIndex];
                    const auto offset = static_cast<std::uint32_t> (
                        event.sample - blockStart);
                    bool queued = true;

                    if (event.action == EventAction::noteOn)
                        queued = engine->queueUpperNoteOn (
                            event.note, event.velocity, offset);
                    else if (event.action == EventAction::noteOff)
                        queued = engine->queueUpperNoteOff (
                            event.note, offset);
                    else
                        queued = engine->queueUpperAllNotesOff (offset);

                    if (! queued)
                        audit.renderSucceeded = false;
                }

                ++eventIndex;
            }

            const auto before =
                allocationCount.load (std::memory_order_relaxed);
            const bool rendered = engine->render ({
                left.data(), right.data(), count
            });
            const auto after =
                allocationCount.load (std::memory_order_relaxed);
            audit.renderAllocations += after - before;
            audit.renderSucceeded =
                audit.renderSucceeded && rendered;

            for (std::size_t sample = 0; sample < count; ++sample)
            {
                const auto absolute =
                    static_cast<std::size_t> (blockStart) + sample;

                if (capturedInterleaved != nullptr)
                {
                    (*capturedInterleaved)[absolute * 2] =
                        left[sample];
                    (*capturedInterleaved)[absolute * 2 + 1] =
                        right[sample];
                }

                hashPcm24 (audit.checksum, left[sample]);
                hashPcm24 (audit.checksum, right[sample]);
            }
        }

        audit.invalidEngineSamples =
            engine->diagnostics().invalidSamples;
        return audit;
    }

    RenderAudit renderScene (
        const Scene& scene,
        std::vector<float>* capturedInterleaved)
    {
        return scene.engine == SceneEngine::tonewheel
                   ? renderTonewheel (scene, capturedInterleaved)
                   : renderTransistor (scene, capturedInterleaved);
    }

    constexpr std::array<int, 16> loudnessVoicing {{
        48, 52, 55, 59,
        60, 64, 67, 71,
        72, 76, 79, 83,
        84, 88, 91, 95
    }};

    LoudnessProbe renderTonewheelLoudnessProbe (
        int noteCount)
    {
        LoudnessProbe result;
        result.engine = "Tonewheel";
        result.notes = noteCount;
        auto engine =
            std::make_unique<churchorgan::TonewheelOrganEngine>();
        churchorgan::TonewheelOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed =
            0x4c4f5544544f4e45ULL
            ^ static_cast<std::uint64_t> (noteCount);
        spec.quality = churchorgan::OrganQualityProfile::standard;
        result.passed = engine->prepare (spec);

        engine->parameters().setPlain (
            churchorgan::OrganParameterId::masterGainDb,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::rotaryMix,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::scannerMode,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::tonewheelProfile,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::leakage,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::crosstalk,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::hum,
            0.0f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::drive,
            0.12f);
        engine->parameters().setPlain (
            churchorgan::OrganParameterId::ampProfile,
            0.0f);
        constexpr std::array<churchorgan::OrganParameterId,
                             churchorgan::drawbarCount>
            upperDrawbars {{
                churchorgan::OrganParameterId::upper16,
                churchorgan::OrganParameterId::upper5AndOneThird,
                churchorgan::OrganParameterId::upper8,
                churchorgan::OrganParameterId::upper4,
                churchorgan::OrganParameterId::upper2AndTwoThirds,
                churchorgan::OrganParameterId::upper2,
                churchorgan::OrganParameterId::upper1AndThreeFifths,
                churchorgan::OrganParameterId::upper1AndOneThird,
                churchorgan::OrganParameterId::upper1
            }};

        for (auto drawbar : upperDrawbars)
            engine->parameters().setPlain (drawbar, 8.0f);

        for (int i = 0; i < noteCount; ++i)
            result.passed =
                engine->queueNoteOn (
                    churchorgan::Manual::upper,
                    loudnessVoicing[static_cast<std::size_t> (i)],
                    100,
                    0)
                && result.passed;

        constexpr std::uint64_t totalSamples =
            sampleRateInteger * 3ULL;
        constexpr std::uint64_t measurementStart =
            sampleRateInteger;
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        double sumSquares = 0.0;
        std::uint64_t measuredSamples = 0;

        for (std::uint64_t blockStart = 0;
             blockStart < totalSamples;
             blockStart += blockSize)
        {
            const auto count = static_cast<std::size_t> (
                std::min<std::uint64_t> (
                    blockSize, totalSamples - blockStart));
            const auto before =
                allocationCount.load (std::memory_order_relaxed);
            const bool rendered = engine->render ({
                left.data(), right.data(), count
            });
            const auto after =
                allocationCount.load (std::memory_order_relaxed);
            result.renderAllocations += after - before;
            result.passed = result.passed && rendered;

            for (std::size_t sample = 0; sample < count; ++sample)
            {
                const auto absolute = blockStart + sample;

                if (! std::isfinite (left[sample])
                     || ! std::isfinite (right[sample]))
                {
                    ++result.invalidSamples;
                    continue;
                }

                if (absolute >= measurementStart)
                {
                    sumSquares +=
                        0.5
                        * (static_cast<double> (left[sample])
                               * left[sample]
                           + static_cast<double> (right[sample])
                                 * right[sample]);
                    result.peak = std::max (
                        result.peak,
                        std::max (
                            std::abs (left[sample]),
                            std::abs (right[sample])));
                    ++measuredSamples;
                }
            }
        }

        result.invalidSamples +=
            engine->diagnostics().invalidOutputSamples;
        const double rms =
            measuredSamples > 0
                ? std::sqrt (
                    sumSquares
                    / static_cast<double> (measuredSamples))
                : 0.0;
        result.rmsDecibels = decibels (rms);
        result.crestDecibels =
            rms > 1.0e-20
                ? decibels (
                    static_cast<double> (result.peak) / rms)
                : 0.0;
        result.passed =
            result.passed
            && result.renderAllocations == 0
            && result.invalidSamples == 0
            && result.rmsDecibels > -60.0
            && result.peak > 0.005f
            && result.peak <= maximumSafePeak;
        return result;
    }

    LoudnessProbe renderTransistorLoudnessProbe (
        int noteCount)
    {
        LoudnessProbe result;
        result.engine = "Transistor";
        result.notes = noteCount;
        auto engine =
            std::make_unique<churchorgan::TransistorOrganEngine>();
        churchorgan::TransistorOrganPrepareSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed =
            0x4c4f55445452414eULL
            ^ static_cast<std::uint64_t> (noteCount);
        spec.profile =
            churchorgan::TransistorOrganProfile::britishCombo;
        result.passed = engine->prepare (spec);

        for (int i = 0; i < noteCount; ++i)
            result.passed =
                engine->queueUpperNoteOn (
                    loudnessVoicing[static_cast<std::size_t> (i)],
                    100,
                    0)
                && result.passed;

        constexpr std::uint64_t totalSamples =
            sampleRateInteger * 3ULL;
        constexpr std::uint64_t measurementStart =
            sampleRateInteger;
        std::array<float, blockSize> left {};
        std::array<float, blockSize> right {};
        double sumSquares = 0.0;
        std::uint64_t measuredSamples = 0;

        for (std::uint64_t blockStart = 0;
             blockStart < totalSamples;
             blockStart += blockSize)
        {
            const auto count = static_cast<std::size_t> (
                std::min<std::uint64_t> (
                    blockSize, totalSamples - blockStart));
            const auto before =
                allocationCount.load (std::memory_order_relaxed);
            const bool rendered = engine->render ({
                left.data(), right.data(), count
            });
            const auto after =
                allocationCount.load (std::memory_order_relaxed);
            result.renderAllocations += after - before;
            result.passed = result.passed && rendered;

            for (std::size_t sample = 0; sample < count; ++sample)
            {
                const auto absolute = blockStart + sample;

                if (! std::isfinite (left[sample])
                     || ! std::isfinite (right[sample]))
                {
                    ++result.invalidSamples;
                    continue;
                }

                if (absolute >= measurementStart)
                {
                    sumSquares +=
                        0.5
                        * (static_cast<double> (left[sample])
                               * left[sample]
                           + static_cast<double> (right[sample])
                                 * right[sample]);
                    result.peak = std::max (
                        result.peak,
                        std::max (
                            std::abs (left[sample]),
                            std::abs (right[sample])));
                    ++measuredSamples;
                }
            }
        }

        result.invalidSamples +=
            engine->diagnostics().invalidSamples;
        const double rms =
            measuredSamples > 0
                ? std::sqrt (
                    sumSquares
                    / static_cast<double> (measuredSamples))
                : 0.0;
        result.rmsDecibels = decibels (rms);
        result.crestDecibels =
            rms > 1.0e-20
                ? decibels (
                    static_cast<double> (result.peak) / rms)
                : 0.0;
        result.passed =
            result.passed
            && result.renderAllocations == 0
            && result.invalidSamples == 0
            && result.rmsDecibels > -60.0
            && result.peak > 0.005f
            && result.peak <= maximumSafePeak;
        return result;
    }

    std::vector<LoudnessProbe> runLoudnessMatrix()
    {
        constexpr std::array<int, 4> noteCounts {{ 1, 4, 8, 16 }};
        std::vector<LoudnessProbe> results;
        results.reserve (noteCounts.size() * 2);

        for (const char* engine : { "Tonewheel", "Transistor" })
        {
            double previous = 0.0;
            bool hasPrevious = false;

            for (int notes : noteCounts)
            {
                auto probe =
                    std::strcmp (engine, "Tonewheel") == 0
                        ? renderTonewheelLoudnessProbe (notes)
                        : renderTransistorLoudnessProbe (notes);

                if (hasPrevious)
                {
                    probe.deltaFromPreviousDecibels =
                        probe.rmsDecibels - previous;
                    probe.passed =
                        probe.passed
                        && probe.deltaFromPreviousDecibels >= -3.0
                        && probe.deltaFromPreviousDecibels <= 18.0;
                }

                previous = probe.rmsDecibels;
                hasPrevious = true;
                results.push_back (probe);
            }
        }

        return results;
    }

    float catmullRom (
        float p0,
        float p1,
        float p2,
        float p3,
        float t) noexcept
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return 0.5f
               * (2.0f * p1
                  + (-p0 + p2) * t
                  + (2.0f * p0 - 5.0f * p1
                     + 4.0f * p2 - p3) * t2
                  + (-p0 + 3.0f * p1
                     - 3.0f * p2 + p3) * t3);
    }

    Metrics analyse (
        const std::vector<float>& interleaved)
    {
        Metrics metrics;
        const std::size_t samples = interleaved.size() / 2;

        if (samples == 0)
            return metrics;

        double sumLeft = 0.0;
        double sumRight = 0.0;
        double sumSquares = 0.0;
        double monoSquares = 0.0;
        double deltaSquares = 0.0;
        std::size_t deltaCount = 0;

        for (std::size_t i = 0; i < samples; ++i)
        {
            float left = interleaved[i * 2];
            float right = interleaved[i * 2 + 1];

            if (! std::isfinite (left) || ! std::isfinite (right))
            {
                ++metrics.invalidSamples;
                left = 0.0f;
                right = 0.0f;
            }

            sumLeft += left;
            sumRight += right;
            sumSquares +=
                0.5
                * (static_cast<double> (left) * left
                   + static_cast<double> (right) * right);
            const double mono =
                0.5 * (static_cast<double> (left) + right);
            monoSquares += mono * mono;
            metrics.peak = std::max (
                metrics.peak,
                std::max (std::abs (left), std::abs (right)));

            if (i > 0)
            {
                const float previousLeft =
                    interleaved[(i - 1) * 2];
                const float previousRight =
                    interleaved[(i - 1) * 2 + 1];
                const float jump = std::max (
                    std::abs (left - previousLeft),
                    std::abs (right - previousRight));
                metrics.maximumSampleJump = std::max (
                    metrics.maximumSampleJump, jump);
                deltaSquares +=
                    static_cast<double> (jump) * jump;
                ++deltaCount;

                if (i % blockSize == 0)
                    metrics.maximumBlockBoundaryJump = std::max (
                        metrics.maximumBlockBoundaryJump,
                        jump);
            }
        }

        metrics.dcLeft =
            sumLeft / static_cast<double> (samples);
        metrics.dcRight =
            sumRight / static_cast<double> (samples);
        metrics.dcMaximum = std::max (
            std::abs (metrics.dcLeft),
            std::abs (metrics.dcRight));
        metrics.rms = std::sqrt (
            sumSquares / static_cast<double> (samples));
        metrics.rmsDecibels = decibels (metrics.rms);
        metrics.dcToRms =
            metrics.rms > 1.0e-20
                ? metrics.dcMaximum / metrics.rms
                : 0.0;
        metrics.crestDecibels =
            metrics.rms > 1.0e-20
                ? decibels (
                    static_cast<double> (metrics.peak)
                    / metrics.rms)
                : 0.0;
        metrics.deltaRms =
            deltaCount > 0
                ? std::sqrt (
                    deltaSquares
                    / static_cast<double> (deltaCount))
                : 0.0;
        const double monoRms = std::sqrt (
            monoSquares / static_cast<double> (samples));
        metrics.monoCompatibilityDecibels =
            metrics.rms > 1.0e-20
                ? decibels (monoRms / metrics.rms)
                : -200.0;

        metrics.approximateTruePeak4x = metrics.peak;

        if (samples >= 4)
            for (std::size_t i = 1; i + 2 < samples; ++i)
                for (std::size_t channel = 0; channel < 2; ++channel)
                {
                    const float p0 =
                        interleaved[(i - 1) * 2 + channel];
                    const float p1 =
                        interleaved[i * 2 + channel];
                    const float p2 =
                        interleaved[(i + 1) * 2 + channel];
                    const float p3 =
                        interleaved[(i + 2) * 2 + channel];

                    for (int phase = 1; phase < 4; ++phase)
                        metrics.approximateTruePeak4x = std::max (
                            metrics.approximateTruePeak4x,
                            std::abs (
                                catmullRom (
                                    p0, p1, p2, p3,
                                    static_cast<float> (phase) * 0.25f)));
                }

        const std::size_t tailSamples =
            std::min<std::size_t> (
                samples,
                sampleRateInteger / 2);
        double tailSquares = 0.0;

        for (std::size_t i = samples - tailSamples;
             i < samples;
             ++i)
        {
            const float left = interleaved[i * 2];
            const float right = interleaved[i * 2 + 1];
            tailSquares +=
                0.5
                * (static_cast<double> (left) * left
                   + static_cast<double> (right) * right);
            metrics.tailPeak = std::max (
                metrics.tailPeak,
                std::max (std::abs (left), std::abs (right)));
        }

        metrics.tailRmsDecibels = decibels (
            std::sqrt (
                tailSquares / static_cast<double> (tailSamples)));
        return metrics;
    }

    void appendFailure (
        std::vector<std::string>& failures,
        bool condition,
        const char* message)
    {
        if (! condition)
            failures.emplace_back (message);
    }

    std::vector<std::string> validate (
        const RenderAudit& first,
        const RenderAudit& second,
        const Metrics& metrics)
    {
        std::vector<std::string> failures;
        appendFailure (
            failures,
            first.renderSucceeded && second.renderSucceeded,
            "render returned failure");
        appendFailure (
            failures,
            first.renderAllocations == 0
                && second.renderAllocations == 0,
            "audio callback allocated memory");
        appendFailure (
            failures,
            first.invalidEngineSamples == 0
                && second.invalidEngineSamples == 0
                && metrics.invalidSamples == 0,
            "NaN/Inf or engine-invalid sample detected");
        appendFailure (
            failures,
            first.checksum == second.checksum
                && first.checksum != fnvOffset,
            "deterministic PCM checksum mismatch");
        appendFailure (
            failures,
            metrics.peak >= minimumSafePeak
                && metrics.peak <= maximumSafePeak,
            "sample peak outside useful/safe range");
        appendFailure (
            failures,
            metrics.approximateTruePeak4x
                <= maximumApproximateTruePeak,
            "approximate 4x true peak is unsafe");
        appendFailure (
            failures,
            metrics.rmsDecibels >= minimumRmsDecibels
                && metrics.rmsDecibels <= maximumRmsDecibels,
            "RMS outside listening-pack range");
        appendFailure (
            failures,
            metrics.dcMaximum <= maximumDc
                && metrics.dcToRms <= maximumDcToRms,
            "DC offset is excessive");
        appendFailure (
            failures,
            metrics.crestDecibels >= minimumCrestDecibels
                && metrics.crestDecibels <= maximumCrestDecibels,
            "crest factor outside safe musical range");
        appendFailure (
            failures,
            metrics.maximumBlockBoundaryJump
                <= maximumBoundaryJump
                && metrics.maximumBlockBoundaryJump
                       <= std::max (
                           0.04f,
                           static_cast<float> (
                               metrics.deltaRms * 14.0)),
            "block-boundary discontinuity detected");
        appendFailure (
            failures,
            metrics.monoCompatibilityDecibels
                >= minimumMonoCompatibilityDecibels,
            "mono compatibility loss exceeds 12 dB");
        appendFailure (
            failures,
            metrics.tailRmsDecibels
                    <= maximumTailRmsDecibels
                && metrics.tailPeak <= maximumTailPeak,
            "post-tail did not reach safe silence");
        return failures;
    }

    void writeU16 (
        std::ostream& stream,
        std::uint16_t value)
    {
        const std::array<char, 2> bytes {{
            static_cast<char> (value & 0xff),
            static_cast<char> ((value >> 8) & 0xff)
        }};
        stream.write (bytes.data(), bytes.size());
    }

    void writeU32 (
        std::ostream& stream,
        std::uint32_t value)
    {
        const std::array<char, 4> bytes {{
            static_cast<char> (value & 0xff),
            static_cast<char> ((value >> 8) & 0xff),
            static_cast<char> ((value >> 16) & 0xff),
            static_cast<char> ((value >> 24) & 0xff)
        }};
        stream.write (bytes.data(), bytes.size());
    }

    bool writeWave24 (
        const std::filesystem::path& path,
        const std::vector<float>& interleaved)
    {
        const auto frames = interleaved.size() / 2;
        const std::uint64_t dataBytes64 = frames * 6;

        if (dataBytes64
            > std::numeric_limits<std::uint32_t>::max() - 36ULL)
            return false;

        const auto dataBytes =
            static_cast<std::uint32_t> (dataBytes64);
        std::ofstream stream (
            path, std::ios::binary | std::ios::trunc);

        if (! stream)
            return false;

        stream.write ("RIFF", 4);
        writeU32 (stream, 36U + dataBytes);
        stream.write ("WAVE", 4);
        stream.write ("fmt ", 4);
        writeU32 (stream, 16);
        writeU16 (stream, 1);
        writeU16 (stream, 2);
        writeU32 (stream, sampleRateInteger);
        writeU32 (stream, sampleRateInteger * 6);
        writeU16 (stream, 6);
        writeU16 (stream, 24);
        stream.write ("data", 4);
        writeU32 (stream, dataBytes);

        for (float value : interleaved)
        {
            const auto sample =
                static_cast<std::uint32_t> (quantise24 (value));
            const std::array<char, 3> bytes {{
                static_cast<char> (sample & 0xff),
                static_cast<char> ((sample >> 8) & 0xff),
                static_cast<char> ((sample >> 16) & 0xff)
            }};
            stream.write (bytes.data(), bytes.size());
        }

        return static_cast<bool> (stream);
    }

    void printReport (const SceneReport& report)
    {
        const auto& m = report.metrics;
        std::cout
            << std::fixed << std::setprecision (6)
            << "LISTENING_METRIC"
            << " scene=\"" << report.scene.label << '"'
            << " rms_db=" << m.rmsDecibels
            << " peak=" << m.peak
            << " true_peak_4x=" << m.approximateTruePeak4x
            << " crest_db=" << m.crestDecibels
            << " dc=" << m.dcMaximum
            << " dc_to_rms=" << m.dcToRms
            << " boundary_jump=" << m.maximumBlockBoundaryJump
            << " max_jump=" << m.maximumSampleJump
            << " mono_db=" << m.monoCompatibilityDecibels
            << " tail_rms_db=" << m.tailRmsDecibels
            << " tail_peak=" << m.tailPeak
            << " rt_alloc=" << report.audit.renderAllocations
            << " checksum=" << std::hex << std::uppercase
            << report.audit.checksum << std::dec
            << " status="
            << (report.failures.empty() ? "OK" : "FAILED")
            << '\n';

        for (const auto& failure : report.failures)
            std::cerr
                << "LISTENING_FAIL scene=\""
                << report.scene.label
                << "\" reason=\"" << failure << "\"\n";
    }

    bool writeCsv (
        const std::filesystem::path& path,
        const std::vector<SceneReport>& reports)
    {
        std::ofstream stream (path, std::ios::trunc);

        if (! stream)
            return false;

        stream
            << "scene,file,rms_db,peak,true_peak_4x,crest_db,"
               "dc,dc_to_rms,boundary_jump,max_jump,mono_db,"
               "tail_rms_db,tail_peak,rt_alloc,checksum,status\n";
        stream << std::fixed << std::setprecision (8);

        for (const auto& report : reports)
        {
            const auto& m = report.metrics;
            stream
                << '"' << report.scene.label << "\","
                << '"' << report.scene.fileName << "\","
                << m.rmsDecibels << ','
                << m.peak << ','
                << m.approximateTruePeak4x << ','
                << m.crestDecibels << ','
                << m.dcMaximum << ','
                << m.dcToRms << ','
                << m.maximumBlockBoundaryJump << ','
                << m.maximumSampleJump << ','
                << m.monoCompatibilityDecibels << ','
                << m.tailRmsDecibels << ','
                << m.tailPeak << ','
                << report.audit.renderAllocations << ','
                << std::hex << std::uppercase
                << report.audit.checksum
                << std::dec << ','
                << (report.failures.empty() ? "OK" : "FAILED")
                << '\n';
        }

        return static_cast<bool> (stream);
    }

    void printLoudnessMatrix (
        const std::vector<LoudnessProbe>& probes)
    {
        for (const auto& probe : probes)
            std::cout
                << std::fixed << std::setprecision (6)
                << "LOUDNESS_PROBE"
                << " engine=" << probe.engine
                << " notes=" << probe.notes
                << " rms_db=" << probe.rmsDecibels
                << " peak=" << probe.peak
                << " crest_db=" << probe.crestDecibels
                << " delta_db="
                << probe.deltaFromPreviousDecibels
                << " rt_alloc=" << probe.renderAllocations
                << " invalid=" << probe.invalidSamples
                << " status="
                << (probe.passed ? "OK" : "FAILED")
                << '\n';
    }

    bool writeLoudnessCsv (
        const std::filesystem::path& path,
        const std::vector<LoudnessProbe>& probes)
    {
        std::ofstream stream (path, std::ios::trunc);

        if (! stream)
            return false;

        stream
            << "engine,notes,rms_db,peak,crest_db,"
               "delta_from_previous_db,rt_alloc,invalid,status\n";
        stream << std::fixed << std::setprecision (8);

        for (const auto& probe : probes)
            stream
                << probe.engine << ','
                << probe.notes << ','
                << probe.rmsDecibels << ','
                << probe.peak << ','
                << probe.crestDecibels << ','
                << probe.deltaFromPreviousDecibels << ','
                << probe.renderAllocations << ','
                << probe.invalidSamples << ','
                << (probe.passed ? "OK" : "FAILED")
                << '\n';

        return static_cast<bool> (stream);
    }
}

int main (int argc, char** argv)
{
    const std::filesystem::path outputDirectory =
        argc > 1
            ? std::filesystem::path (argv[1])
            : std::filesystem::current_path() / "listening-pack";
    std::error_code error;
    std::filesystem::create_directories (
        outputDirectory, error);

    if (error)
    {
        std::cerr
            << "LISTENING_FATAL could_not_create_output path=\""
            << outputDirectory.string()
            << "\" error=\"" << error.message() << "\"\n";
        return 2;
    }

    auto scenes = makeScenes();
    std::vector<SceneReport> reports;
    reports.reserve (scenes.size());
    bool allPassed = true;

    for (const auto& scene : scenes)
    {
        std::vector<float> captured;
        const auto first = renderScene (scene, &captured);
        const auto second = renderScene (scene, nullptr);
        SceneReport report;
        report.scene = scene;
        report.audit = first;
        report.metrics = analyse (captured);
        report.path = outputDirectory / scene.fileName;
        report.failures = validate (
            first, second, report.metrics);

        if (! writeWave24 (report.path, captured))
            report.failures.emplace_back (
                "could not write 24-bit WAV");

        allPassed = allPassed && report.failures.empty();
        printReport (report);
        reports.push_back (std::move (report));
    }

    if (! writeCsv (
            outputDirectory / "listening_metrics.csv",
            reports))
    {
        std::cerr
            << "LISTENING_FATAL could_not_write_metrics_csv\n";
        allPassed = false;
    }

    auto loudnessProbes = runLoudnessMatrix();
    printLoudnessMatrix (loudnessProbes);

    for (const auto& probe : loudnessProbes)
        allPassed = allPassed && probe.passed;

    if (! writeLoudnessCsv (
            outputDirectory / "loudness_matrix.csv",
            loudnessProbes))
    {
        std::cerr
            << "LISTENING_FATAL could_not_write_loudness_csv\n";
        allPassed = false;
    }

    std::cout
        << (allPassed
                ? "CHURCH_ORGAN_LISTENING_PACK_OK"
                : "CHURCH_ORGAN_LISTENING_PACK_FAILED")
        << " scenes=" << reports.size()
        << " output=\"" << outputDirectory.string() << "\"\n";
    return allPassed ? 0 : 1;
}
