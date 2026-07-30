#pragma once

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::size_t articulationContactCount = 9;
    inline constexpr std::size_t articulationMidiNoteCount = 128;

    struct MultiContactParameters
    {
        float noteOnSpreadMilliseconds = 1.65f;
        float noteOffSpreadMilliseconds = 1.10f;
        float contactAttackMilliseconds = 0.42f;
        float contactReleaseMilliseconds = 0.72f;
        float timingJitterMilliseconds = 0.16f;
        float bounceAmount = 0.055f;
        float bounceDecayMilliseconds = 2.8f;
        float bounceFrequencyHz = 920.0f;
    };

    struct KeyClickParameters
    {
        float onsetLevel = 0.075f;
        float releaseLevel = 0.032f;
        float onsetDecayMilliseconds = 7.5f;
        float releaseDecayMilliseconds = 4.5f;
        float lowCutHz = 850.0f;
        float highCutHz = 6800.0f;
        float velocityInfluence = 0.12f;
    };

    enum class PercussionHarmonic : std::uint8_t
    {
        second,
        third
    };

    enum class PercussionTiming : std::uint8_t
    {
        fast,
        slow
    };

    enum class PercussionVolume : std::uint8_t
    {
        soft,
        normal
    };

    enum class PercussionTriggerMode : std::uint8_t
    {
        singleTrigger,
        retrigger
    };

    struct HarmonicPercussionParameters
    {
        PercussionHarmonic harmonic = PercussionHarmonic::third;
        PercussionTiming timing = PercussionTiming::fast;
        PercussionVolume volume = PercussionVolume::normal;
        PercussionTriggerMode triggerMode =
            PercussionTriggerMode::singleTrigger;
        float attackMilliseconds = 0.55f;
        float fastDecayMilliseconds = 230.0f;
        float slowDecayMilliseconds = 820.0f;
        float releaseMilliseconds = 32.0f;
        float normalGain = 0.36f;
        float softGain = 0.18f;
    };
}
