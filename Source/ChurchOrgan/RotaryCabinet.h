#pragma once

#include "OrganDspUtilities.h"

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class RotarySpeedMode : std::uint8_t
    {
        stop = 0,
        slow,
        fast,
        brake
    };

    enum class RotaryPerspective : std::uint8_t
    {
        stageClose = 0,
        balanced,
        openRoom,
        vintageMono,
        wideWorship
    };

    struct RotaryPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        float initialHornPhase = 0.083f;
        float initialDrumPhase = 0.617f;
    };

    struct RotaryParameters
    {
        RotarySpeedMode speedMode = RotarySpeedMode::slow;
        RotaryPerspective perspective = RotaryPerspective::balanced;

        float mix = 1.0f;
        float crossoverHz = 760.0f;
        float hornLevel = 0.78f;
        float drumLevel = 0.88f;

        float hornSlowHz = 0.76f;
        float hornFastHz = 6.15f;
        float drumSlowHz = 0.62f;
        float drumFastHz = 5.05f;

        float hornAccelerationSeconds = 1.20f;
        float hornDecelerationSeconds = 0.86f;
        float drumAccelerationSeconds = 3.85f;
        float drumDecelerationSeconds = 3.10f;
        float brakeSeconds = 0.22f;

        float doppler = 0.72f;
        float amplitudeModulation = 0.66f;
        float micDistance = 0.45f;
        float stereoWidth = 0.64f;
        float cabinetResonance = 0.34f;
        float airAbsorption = 0.22f;
        float earlyReflections = 0.12f;
        float worshipIntensity = 0.0f;
        float output = 0.90f;
        bool reverseDirection = false;
    };

    struct RotaryDiagnostics
    {
        float hornSpeedHz = 0.0f;
        float drumSpeedHz = 0.0f;
        float hornTargetHz = 0.0f;
        float drumTargetHz = 0.0f;
        float hornPhase = 0.0f;
        float drumPhase = 0.0f;
        float wetCalibrationGain = 1.0f;
        float dryWetCorrelation = 0.0f;
        float lastPeak = 0.0f;
    };

    // Physical-geometry-inspired rotary cabinet. The treble horn and bass
    // rotor own independent angle, inertia, directional gain and propagation
    // delay. No impulse response, sample or third-party response curve is used.
    class RotaryCabinet
    {
    public:
        RotaryCabinet() noexcept = default;

        RotaryCabinet (const RotaryCabinet&) = delete;
        RotaryCabinet& operator= (const RotaryCabinet&) = delete;

        bool prepare (const RotaryPrepareSpec& spec) noexcept;
        void reset() noexcept;

        // Call on the audio thread or before processing.
        void setParameters (const RotaryParameters& parameters) noexcept;
        RotaryParameters parameters() const noexcept;
        void setSpeedMode (RotarySpeedMode mode) noexcept;

        void process (
            float* left,
            float* right,
            std::size_t numSamples) noexcept;

        RotaryDiagnostics diagnostics() const noexcept;
        float nominalWetLatencyMilliseconds() const noexcept;

        static constexpr bool processUsesFixedStorage = true;
        static constexpr std::size_t delayCapacity = 8192;

    private:
        struct PerspectiveCalibration
        {
            float distanceScale = 1.0f;
            float widthScale = 1.0f;
            float dopplerScale = 1.0f;
            float amplitudeScale = 1.0f;
            float addedEarly = 0.0f;
            float addedAir = 0.0f;
            float monoBlend = 0.0f;
        };

        static PerspectiveCalibration calibration (
            RotaryPerspective perspective) noexcept;

        void updateTargets() noexcept;
        void updateRotorSpeeds() noexcept;
        float hornDelaySamples (
            float microphoneAngle,
            float distance,
            float doppler) const noexcept;
        float drumDelaySamples (
            float microphoneAngle,
            float distance,
            float doppler) const noexcept;
        static float hornDirectivity (
            float angle,
            float amount) noexcept;
        static float drumDirectivity (
            float angle,
            float amount) noexcept;

        RotaryPrepareSpec spec_ {};
        RotaryParameters parameters_ {};

        dsp::FixedFractionalDelay<delayCapacity> hornLine_;
        dsp::FixedFractionalDelay<delayCapacity> drumLine_;
        dsp::FixedFractionalDelay<delayCapacity> sideLine_;

        dsp::SmoothedValue crossoverStep_;
        dsp::SmoothedValue wetMix_;
        dsp::SmoothedValue hornLevel_;
        dsp::SmoothedValue drumLevel_;
        dsp::SmoothedValue doppler_;
        dsp::SmoothedValue amplitude_;
        dsp::SmoothedValue distance_;
        dsp::SmoothedValue width_;
        dsp::SmoothedValue resonance_;
        dsp::SmoothedValue airStep_;
        dsp::SmoothedValue early_;
        dsp::SmoothedValue monoBlend_;
        dsp::SmoothedValue output_;
        dsp::SmoothedValue wetCalibration_;

        float crossoverState_ = 0.0f;
        float leftAirState_ = 0.0f;
        float rightAirState_ = 0.0f;
        float leftBodyState_ = 0.0f;
        float rightBodyState_ = 0.0f;
        float bodyStep_ = 0.0f;

        float hornPhase_ = 0.0f;
        float drumPhase_ = 0.0f;
        float hornSin_ = 0.0f;
        float hornCos_ = 1.0f;
        float drumSin_ = 0.0f;
        float drumCos_ = 1.0f;
        float hornSpeedHz_ = 0.0f;
        float drumSpeedHz_ = 0.0f;
        float hornTargetHz_ = 0.0f;
        float drumTargetHz_ = 0.0f;
        float hornAccelerationStep_ = 0.0f;
        float hornDecelerationStep_ = 0.0f;
        float drumAccelerationStep_ = 0.0f;
        float drumDecelerationStep_ = 0.0f;
        float brakeStep_ = 0.0f;
        float loudnessMeterStep_ = 0.0f;
        float dryPowerState_ = 0.0f;
        float wetPowerState_ = 0.0f;
        float dryWetCrossState_ = 0.0f;
        float dryWetCorrelation_ = 0.0f;
        std::uint32_t loudnessUpdateCounter_ = 0;
        std::uint32_t rotorNormaliseCounter_ = 0;
        float lastPeak_ = 0.0f;
        bool prepared_ = false;
    };
}
