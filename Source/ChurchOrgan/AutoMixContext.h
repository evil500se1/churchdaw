#pragma once

#include "WorshipDspUtilities.h"

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum AutoMixSidechain : std::uint32_t
    {
        autoMixPiano = 1u << 0,
        autoMixExternalPad = 1u << 1,
        autoMixExternalStrings = 1u << 2,
        autoMixMasterProximity = 1u << 3,
        autoMixAllSources =
            autoMixPiano
            | autoMixExternalPad
            | autoMixExternalStrings
            | autoMixMasterProximity
    };

    struct AutoMixPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
    };

    // All fields are already-computed control/envelope values. This contract
    // deliberately cannot accept, inspect or retain audio buffers.
    struct AutoMixControlFrame
    {
        float pianoEnvelope = 0.0f;
        float externalPadEnvelope = 0.0f;
        float externalStringsEnvelope = 0.0f;
        float masterProximity = 0.0f;
    };

    struct AutoMixParameters
    {
        bool enabled = true;
        float amount = 0.32f;
        float reactionMilliseconds = 720.0f;
        float recoveryMilliseconds = 1900.0f;
        float maximumAttenuationDecibels = 3.0f;
        float pianoConflict = 0.72f;
        float externalPadConflict = 0.86f;
        float externalStringsConflict = 0.58f;
        float masterProtection = 0.90f;
        float lowMidZone = 0.65f;
        float midZone = 0.72f;
        float soloRoomLiftDecibels = 0.45f;
        std::uint32_t sidechainMask = autoMixAllSources;
    };

    struct AutoMixGains
    {
        float worshipLayer = 1.0f;
        float organLowMid = 1.0f;
        float organMid = 1.0f;
        float drive = 1.0f;
        float output = 1.0f;
        float room = 1.0f;
    };

    // Slow, bounded contextual gain computer. One instance belongs to one
    // ChurchDAW channel. It consumes control frames only and is independent of
    // every audio buffer and audio thread owned by other channels.
    class AutoMixContext
    {
    public:
        bool prepare (const AutoMixPrepareSpec& spec) noexcept;
        void reset() noexcept;

        void setParameters (
            const AutoMixParameters& parameters) noexcept;
        AutoMixParameters parameters() const noexcept;

        void setControlFrame (
            const AutoMixControlFrame& controls) noexcept;
        AutoMixControlFrame controlFrame() const noexcept;

        // Advances the slow control model by exactly numSamples. The return
        // value is constant for the calling block and contains no references.
        AutoMixGains advance (std::size_t numSamples) noexcept;
        AutoMixGains currentGains() const noexcept;

        static constexpr bool usesAudioBuffers = false;
        static constexpr bool processUsesFixedStorage = true;

    private:
        static float sanitiseEnvelope (float value) noexcept;
        static float approach (
            float current,
            float target,
            float attackMultiplier,
            float recoveryMultiplier) noexcept;

        void updateTimeConstants() noexcept;
        AutoMixGains targetGains() const noexcept;

        AutoMixPrepareSpec spec_ {};
        AutoMixParameters parameters_ {};
        AutoMixControlFrame requestedControls_ {};
        AutoMixControlFrame smoothedControls_ {};
        AutoMixGains gains_ {};

        float reactionSeconds_ = 0.72f;
        float recoverySeconds_ = 1.90f;
        bool prepared_ = false;
    };
}
