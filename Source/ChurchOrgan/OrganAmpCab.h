#pragma once

#include "OrganDspUtilities.h"

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class OrganAmpProfile : std::uint8_t
    {
        clean = 0,
        warm,
        driven,
        worshipSmooth
    };

    enum class OrganAmpOversampling : std::uint8_t
    {
        off = 1,
        x2 = 2,
        x4 = 4,
        x8 = 8
    };

    struct OrganAmpPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        OrganAmpOversampling oversampling =
            OrganAmpOversampling::x4;
    };

    struct OrganAmpParameters
    {
        OrganAmpProfile profile = OrganAmpProfile::warm;
        float drive = 0.28f;
        float bias = 0.35f;
        float sag = 0.28f;
        float lowTightening = 0.24f;
        float tone = 0.55f;
        float presence = 0.42f;
        float cabinet = 0.62f;
        float worshipIntensity = 0.0f;
        float output = 0.92f;
        bool automaticGain = true;
        bool peakProtection = true;
    };

    // Frequency-aware transformer/preamp/power-amp/cabinet character stage.
    // Oversampling quality is fixed by prepare(), so the render path never
    // performs a structural allocation or unsafe quality transition.
    class OrganAmpCab
    {
    public:
        OrganAmpCab() noexcept = default;

        OrganAmpCab (const OrganAmpCab&) = delete;
        OrganAmpCab& operator= (const OrganAmpCab&) = delete;

        bool prepare (const OrganAmpPrepareSpec& spec) noexcept;
        void reset() noexcept;

        // Call on the audio thread or before processing.
        void setParameters (const OrganAmpParameters& parameters) noexcept;
        OrganAmpParameters parameters() const noexcept;

        void process (
            float* left,
            float* right,
            std::size_t numSamples) noexcept;

        int oversamplingFactor() const noexcept;
        float lastPeak() const noexcept;

        static constexpr bool processUsesFixedStorage = true;
        static constexpr int declaredLatencySamples = 0;

    private:
        struct ChannelState
        {
            float lowTight = 0.0f;
            float driveBand = 0.0f;
            float envelope = 0.0f;
            float previousInput = 0.0f;
            float antiAlias1 = 0.0f;
            float antiAlias2 = 0.0f;
            float cabinet1 = 0.0f;
            float cabinet2 = 0.0f;
            float cabinetBody = 0.0f;
            float dcInput = 0.0f;
            float dcOutput = 0.0f;
        };

        struct ProfileCalibration
        {
            float baseDriveDecibels = 0.0f;
            float driveRangeDecibels = 16.0f;
            float biasScale = 0.08f;
            float sagScale = 0.25f;
            float cabinetCutoffHz = 11000.0f;
            float powerDrive = 1.05f;
            float outputTrim = 0.96f;
        };

        static ProfileCalibration calibration (
            OrganAmpProfile profile) noexcept;
        static int factorFor (
            OrganAmpOversampling quality) noexcept;
        static float nonlinearStage (
            float input,
            float bias,
            float primaryOffset,
            float secondaryOffset,
            float powerDrive) noexcept;

        float processSample (
            float input,
            ChannelState& state,
            float preGain,
            float bias,
            float primaryOffset,
            float secondaryOffset,
            float sag,
            float tightening,
            float tone,
            float presence,
            float cabinet,
            float cabinetStep,
            float outputGain,
            float powerDrive) noexcept;
        void updateTargets() noexcept;

        OrganAmpPrepareSpec spec_ {};
        OrganAmpParameters parameters_ {};
        ChannelState left_ {};
        ChannelState right_ {};

        dsp::SmoothedValue preGain_;
        dsp::SmoothedValue bias_;
        dsp::SmoothedValue sag_;
        dsp::SmoothedValue tightening_;
        dsp::SmoothedValue tone_;
        dsp::SmoothedValue presence_;
        dsp::SmoothedValue cabinet_;
        dsp::SmoothedValue cabinetStep_;
        dsp::SmoothedValue outputGain_;
        dsp::SmoothedValue powerDrive_;

        float lowTightStep_ = 0.0f;
        float driveBandStep_ = 0.0f;
        float envelopeAttackStep_ = 0.0f;
        float envelopeReleaseStep_ = 0.0f;
        float bodyStep_ = 0.0f;
        float dcCoefficient_ = 0.995f;
        float antiAliasStep_ = 1.0f;
        float lastPeak_ = 0.0f;
        int oversamplingFactor_ = 1;
        bool prepared_ = false;
    };
}
