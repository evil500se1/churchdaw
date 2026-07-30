#pragma once

#include "OrganDspUtilities.h"

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class ScannerMode : std::uint8_t
    {
        bypass = 0,
        vibrato1,
        vibrato2,
        vibrato3,
        chorus1,
        chorus2,
        chorus3
    };

    enum class ScannerCharacter : std::uint8_t
    {
        smooth,
        deep
    };

    struct ScannerPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        std::uint32_t deterministicSeed = 0x5343414eU;
        float initialPhase = 0.137f;
    };

    struct ScannerParameters
    {
        ScannerMode mode = ScannerMode::bypass;
        ScannerCharacter character = ScannerCharacter::smooth;
        float rate = 1.0f;          // 0.5 .. 1.5
        float depth = 1.0f;         // 0 .. 1
        float mix = 1.0f;           // scales the mode's calibrated wet mix
        float stereoSpread = 0.16f; // deliberately subtle: 0 .. 1
        float age = 0.12f;          // bandwidth and soft phase-line colour
        float noise = 0.0f;         // deterministic, very low-level
        float worshipIntensity = 0.0f;
        float output = 1.0f;
    };

    // Dedicated multipoint virtual scanner. Create one instance per manual
    // when Upper, Lower and Pedal require independent routing.
    class ScannerVibrato
    {
    public:
        ScannerVibrato() noexcept = default;

        ScannerVibrato (const ScannerVibrato&) = delete;
        ScannerVibrato& operator= (const ScannerVibrato&) = delete;

        bool prepare (const ScannerPrepareSpec& spec) noexcept;
        void reset() noexcept;

        // Call from the audio thread or before processing. All continuous
        // parameters are internally smoothed; this function does not allocate.
        void setParameters (const ScannerParameters& parameters) noexcept;
        ScannerParameters parameters() const noexcept;

        // In-place stereo or mono. right == nullptr (or right == left) means
        // mono. Invalid samples are sanitised to zero.
        void process (
            float* left,
            float* right,
            std::size_t numSamples) noexcept;

        float currentPhase() const noexcept;
        float nominalWetLatencyMilliseconds() const noexcept;

        static constexpr bool processUsesFixedStorage = true;
        static constexpr std::size_t delayCapacity = 8192;

    private:
        struct ModeCalibration
        {
            float delayExcursionMilliseconds = 0.0f;
            float wetMix = 0.0f;
            float rateHz = 6.55f;
        };

        static ModeCalibration calibration (
            ScannerMode mode,
            ScannerCharacter character) noexcept;

        static std::uint32_t nextNoise (std::uint32_t& state) noexcept;

        float scannerSample (
            const dsp::FixedFractionalDelay<delayCapacity>& line,
            float phase,
            float excursionSamples) const noexcept;
        void updateTargets() noexcept;

        ScannerPrepareSpec spec_ {};
        ScannerParameters parameters_ {};

        dsp::FixedFractionalDelay<delayCapacity> leftDelay_;
        dsp::FixedFractionalDelay<delayCapacity> rightDelay_;

        dsp::SmoothedValue rateHz_;
        dsp::SmoothedValue excursionSamples_;
        dsp::SmoothedValue wetMix_;
        dsp::SmoothedValue spreadPhase_;
        dsp::SmoothedValue age_;
        dsp::SmoothedValue noiseGain_;
        dsp::SmoothedValue output_;
        dsp::SmoothedValue bandwidthStep_;

        float leftBandwidthState1_ = 0.0f;
        float leftBandwidthState2_ = 0.0f;
        float rightBandwidthState1_ = 0.0f;
        float rightBandwidthState2_ = 0.0f;
        float phase_ = 0.0f;
        std::uint32_t noiseState_ = 0x5343414eU;
        bool prepared_ = false;
    };
}
