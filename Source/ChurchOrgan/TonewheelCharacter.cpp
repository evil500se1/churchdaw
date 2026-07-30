#include "TonewheelCharacter.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr float pi =
            3.141592653589793238462643383279502884f;
        constexpr float twoPi = 2.0f * pi;

        float clamp01 (float value) noexcept
        {
            if (! std::isfinite (value))
                return 0.0f;

            return std::max (0.0f, std::min (1.0f, value));
        }

        float onePoleCoefficient (
            double sampleRate,
            float frequencyHz) noexcept
        {
            const auto frequency = std::max (
                1.0f,
                std::min (
                    static_cast<float> (sampleRate * 0.45),
                    frequencyHz));
            return static_cast<float> (
                1.0 - std::exp (
                    -2.0 * static_cast<double> (pi)
                    * static_cast<double> (frequency)
                    / sampleRate));
        }
    }

    bool TonewheelCharacter::prepare (
        double sampleRate,
        std::uint64_t deterministicSeed,
        const TonewheelCharacterParameters& parameters) noexcept
    {
        prepared_ = false;

        if (! validSampleRate (sampleRate))
            return false;

        sampleRate_ = sampleRate;
        seed_ = deterministicSeed != 0
                    ? deterministicSeed
                    : 0x4348415241435445ULL;
        target_ = sanitise (parameters, sampleRate_);
        current_ = target_;
        smoothingCoefficient_ = smoothingCoefficient (
            sampleRate_, target_.smoothingMilliseconds);
        transformerLowCoefficient_ = onePoleCoefficient (
            sampleRate_, 720.0f);
        dcPole_ = static_cast<float> (
            std::exp (-2.0 * static_cast<double> (pi)
                      * 9.0 / sampleRate_));

        for (std::size_t tap = 0; tap < leakageTapCount; ++tap)
        {
            const auto hash = mixedHash (
                seed_ ^ (0x9e3779b97f4a7c15ULL * (tap + 1)));
            const auto band = tap % 4;
            const std::size_t bandStart =
                band == 0 ? 0
                : band == 1 ? 20
                : band == 2 ? 44
                            : 68;
            const std::size_t bandWidth =
                band == 3 ? 28 : 20;
            leakageWheels_[tap] = static_cast<std::uint8_t> (
                bandStart
                + static_cast<std::size_t> (hash % bandWidth));
            leakageWeights_[tap] =
                (tap % 2 == 0 ? 1.0f : -1.0f)
                * (0.72f + 0.28f * std::abs (signedUnit (hash >> 8)));
        }

        prepared_ = true;
        reset();
        return true;
    }

    void TonewheelCharacter::reset() noexcept
    {
        humPhase_ = twoPi
                    * (0.5f + 0.5f * signedUnit (
                        mixedHash (seed_ ^ 0x48554d5048415345ULL)));
        noiseState_ = mixedHash (seed_ ^ 0x4d4543484e4f4953ULL);
        if (noiseState_ == 0)
            noiseState_ = 1;
        mechanicalState_ = 0.0f;
        leakageOutput_ = 0.0f;
        transformerLowState_ = 0.0f;
        transformerPreviousInput_ = 0.0f;
        transformerDcState_ = 0.0f;
        current_ = target_;
        humIncrement_ = twoPi * current_.mainsFrequencyHz
                        / static_cast<float> (sampleRate_);
    }

    void TonewheelCharacter::setParameters (
        const TonewheelCharacterParameters& parameters) noexcept
    {
        if (! prepared_)
            return;

        target_ = sanitise (parameters, sampleRate_);
        smoothingCoefficient_ = smoothingCoefficient (
            sampleRate_, target_.smoothingMilliseconds);
    }

    void TonewheelCharacter::beginSample (
        const TonewheelBank& tonewheels) noexcept
    {
        if (! prepared_)
        {
            leakageOutput_ = 0.0f;
            return;
        }

        updateSmoothedParameters();

        const float agedLeakageGain =
            current_.leakage
            * (0.008f + 0.014f * current_.age);
        const float humGain =
            current_.hum * (0.0020f + 0.0025f * current_.age);
        const float mechanicalGain =
            current_.mechanicalNoise
            * (0.0015f + 0.0020f * current_.age);

        float wheelBleed = 0.0f;
        if (agedLeakageGain > 1.0e-9f)
        {
            for (std::size_t tap = 0; tap < leakageTapCount; ++tap)
                wheelBleed += tonewheels.sample (leakageWheels_[tap])
                              * leakageWeights_[tap];

            wheelBleed *=
                1.0f / static_cast<float> (leakageTapCount);
        }

        humIncrement_ = twoPi * current_.mainsFrequencyHz
                        / static_cast<float> (sampleRate_);
        humPhase_ += humIncrement_;
        if (humPhase_ >= twoPi)
            humPhase_ -= twoPi;

        float hum = 0.0f;
        if (humGain > 1.0e-9f)
        {
            hum =
                std::sin (humPhase_)
                + 0.19f * std::sin (
                    2.0f * humPhase_ + 0.37f);
        }

        if (mechanicalGain > 1.0e-9f)
        {
            const auto rawNoise = nextNoise (noiseState_);
            mechanicalState_ +=
                0.0125f * (rawNoise - mechanicalState_);
        }

        leakageOutput_ =
            wheelBleed * agedLeakageGain
            + hum * humGain
            + mechanicalState_ * mechanicalGain;

        if (! std::isfinite (leakageOutput_))
            leakageOutput_ = 0.0f;
    }

    float TonewheelCharacter::leakageSample() const noexcept
    {
        return leakageOutput_;
    }

    float TonewheelCharacter::crosstalkSample (
        const TonewheelBank& tonewheels,
        std::size_t wheelIndex,
        float keyboardRegionZeroToOne) const noexcept
    {
        if (! prepared_
             || wheelIndex >= TonewheelBank::wheelCount)
            return 0.0f;

        const auto lower =
            wheelIndex > 0 ? wheelIndex - 1 : wheelIndex + 1;
        const auto upper =
            wheelIndex + 1 < TonewheelBank::wheelCount
                ? wheelIndex + 1
                : wheelIndex - 1;
        const auto octave =
            wheelIndex + 12 < TonewheelBank::wheelCount
                ? wheelIndex + 12
                : (wheelIndex >= 12 ? wheelIndex - 12 : wheelIndex);
        const auto region = clamp01 (keyboardRegionZeroToOne);
        const float regionTilt = 0.78f + 0.44f * region;
        const float amount =
            current_.crosstalk
            * (0.0035f + 0.0095f * current_.age)
            * regionTilt;

        const float neighbour =
            tonewheels.sample (lower) * 0.47f
            - tonewheels.sample (upper) * 0.36f
            + tonewheels.sample (octave) * 0.17f;
        return neighbour * amount;
    }

    float TonewheelCharacter::processTransformer (
        float input) noexcept
    {
        if (! prepared_ || ! std::isfinite (input))
            return 0.0f;

        const auto amount = clamp01 (current_.transformer);
        if (amount <= 0.000001f)
            return input;

        transformerLowState_ +=
            transformerLowCoefficient_ * (input - transformerLowState_);
        const float high = input - transformerLowState_;
        const float frequencyShaped =
            transformerLowState_ * (1.0f + 0.16f * amount)
            + high * (1.0f - 0.12f * amount);
        const float drive = 1.0f + 2.9f * amount;
        const float biased =
            frequencyShaped * drive
            + 0.055f * amount * frequencyShaped * frequencyShaped;
        const float saturated =
            biased / (1.0f + std::abs (biased));
        const float compensated =
            saturated * (1.0f + 0.73f * amount);
        const float shaped =
            input + amount * (compensated - input);

        const float dcBlocked =
            shaped - transformerPreviousInput_
            + dcPole_ * transformerDcState_;
        transformerPreviousInput_ = shaped;
        transformerDcState_ = dcBlocked;

        if (! std::isfinite (dcBlocked))
        {
            transformerPreviousInput_ = 0.0f;
            transformerDcState_ = 0.0f;
            return 0.0f;
        }

        return dcBlocked;
    }

    const TonewheelCharacterParameters&
        TonewheelCharacter::targetParameters() const noexcept
    {
        return target_;
    }

    bool TonewheelCharacter::validSampleRate (
        double sampleRate) noexcept
    {
        return std::isfinite (sampleRate)
               && sampleRate >= 32000.0
               && sampleRate <= 384000.0;
    }

    TonewheelCharacterParameters TonewheelCharacter::sanitise (
        const TonewheelCharacterParameters& parameters,
        double sampleRate) noexcept
    {
        TonewheelCharacterParameters result = parameters;
        result.leakage = clamp01 (parameters.leakage);
        result.crosstalk = clamp01 (parameters.crosstalk);
        result.hum = clamp01 (parameters.hum);
        result.age = clamp01 (parameters.age);
        result.mechanicalNoise = clamp01 (parameters.mechanicalNoise);
        result.transformer = clamp01 (parameters.transformer);
        result.mainsFrequencyHz =
            std::isfinite (parameters.mainsFrequencyHz)
                ? std::max (45.0f, std::min (65.0f,
                                            parameters.mainsFrequencyHz))
                : 60.0f;
        result.smoothingMilliseconds =
            std::isfinite (parameters.smoothingMilliseconds)
                ? std::max (1.0f, std::min (
                    500.0f, parameters.smoothingMilliseconds))
                : 40.0f;

        if (! validSampleRate (sampleRate))
            result = {};

        return result;
    }

    float TonewheelCharacter::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        const double samples = std::max (
            1.0,
            sampleRate * static_cast<double> (milliseconds) * 0.001);
        return static_cast<float> (1.0 - std::exp (-1.0 / samples));
    }

    std::uint64_t TonewheelCharacter::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    float TonewheelCharacter::signedUnit (
        std::uint64_t value) noexcept
    {
        const auto unit = static_cast<double> (value >> 11)
                          * (1.0 / 9007199254740992.0);
        return static_cast<float> (unit * 2.0 - 1.0);
    }

    float TonewheelCharacter::nextNoise (
        std::uint64_t& state) noexcept
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const auto value = state * 2685821657736338717ULL;
        return signedUnit (value);
    }

    void TonewheelCharacter::updateSmoothedParameters() noexcept
    {
        auto smooth = [this] (float& current, float target)
        {
            current += (target - current) * smoothingCoefficient_;
        };

        smooth (current_.leakage, target_.leakage);
        smooth (current_.crosstalk, target_.crosstalk);
        smooth (current_.hum, target_.hum);
        smooth (current_.age, target_.age);
        smooth (current_.mechanicalNoise, target_.mechanicalNoise);
        smooth (current_.transformer, target_.transformer);
        smooth (current_.mainsFrequencyHz, target_.mainsFrequencyHz);
        current_.smoothingMilliseconds = target_.smoothingMilliseconds;
    }
}
