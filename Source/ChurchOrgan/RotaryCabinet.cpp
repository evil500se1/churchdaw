#include "RotaryCabinet.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 8000.0;
        constexpr double maximumSampleRate = 384000.0;
        constexpr float speedOfSoundMetersPerSecond = 343.0f;
        constexpr float hornRadiusMeters = 0.172f;
        constexpr float drumRadiusMeters = 0.247f;
        constexpr float baseLatencyMilliseconds = 2.35f;

        float sanitiseSeconds (
            float requested,
            float fallback) noexcept
        {
            return dsp::clamp (
                dsp::finiteOr (requested, fallback),
                0.03f,
                12.0f);
        }

        // Accurate to better than 7e-5 over the rotary microphone range
        // [0, 1.12] radians, without a transcendental call per sample.
        inline void microphoneSinCos (
            float angle,
            float& sine,
            float& cosine) noexcept
        {
            const auto squared = angle * angle;
            sine = angle
                * (1.0f
                   + squared
                       * (-1.0f / 6.0f
                          + squared
                              * (1.0f / 120.0f
                                 - squared / 5040.0f)));
            cosine =
                1.0f
                + squared
                    * (-0.5f
                       + squared
                           * (1.0f / 24.0f
                              - squared / 720.0f));
        }

        inline void advanceRotor (
            float& phase,
            float& sine,
            float& cosine,
            float speedHz,
            float direction,
            float inverseSampleRate) noexcept
        {
            const auto phaseIncrement =
                direction * speedHz * inverseSampleRate;
            phase += phaseIncrement;
            if (phase >= 1.0f)
                phase -= 1.0f;
            else if (phase < 0.0f)
                phase += 1.0f;

            const auto angleIncrement =
                phaseIncrement * dsp::twoPi;
            const auto squared =
                angleIncrement * angleIncrement;
            const auto sineIncrement =
                angleIncrement * (1.0f - squared / 6.0f);
            const auto cosineIncrement =
                1.0f - 0.5f * squared
                + squared * squared / 24.0f;
            const auto nextSine =
                sine * cosineIncrement
                + cosine * sineIncrement;
            const auto nextCosine =
                cosine * cosineIncrement
                - sine * sineIncrement;
            sine = nextSine;
            cosine = nextCosine;
        }
    }

    bool RotaryCabinet::prepare (
        const RotaryPrepareSpec& requestedSpec) noexcept
    {
        if (! std::isfinite (requestedSpec.sampleRate)
            || requestedSpec.sampleRate < minimumSampleRate
            || requestedSpec.sampleRate > maximumSampleRate
            || requestedSpec.maximumBlockSize == 0)
        {
            prepared_ = false;
            return false;
        }

        spec_ = requestedSpec;
        spec_.initialHornPhase = dsp::wrapUnitPhase (
            dsp::finiteOr (
                requestedSpec.initialHornPhase,
                0.083f));
        spec_.initialDrumPhase = dsp::wrapUnitPhase (
            dsp::finiteOr (
                requestedSpec.initialDrumPhase,
                0.617f));

        const auto initialCrossover =
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                parameters_.crossoverHz);
        const auto initialAir =
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                13500.0f);

        crossoverStep_.prepare (
            spec_.sampleRate, 55.0f, initialCrossover);
        wetMix_.prepare (
            spec_.sampleRate, 30.0f, parameters_.mix);
        hornLevel_.prepare (
            spec_.sampleRate, 35.0f, parameters_.hornLevel);
        drumLevel_.prepare (
            spec_.sampleRate, 35.0f, parameters_.drumLevel);
        doppler_.prepare (
            spec_.sampleRate, 60.0f, parameters_.doppler);
        amplitude_.prepare (
            spec_.sampleRate,
            60.0f,
            parameters_.amplitudeModulation);
        distance_.prepare (
            spec_.sampleRate, 80.0f, parameters_.micDistance);
        width_.prepare (
            spec_.sampleRate, 80.0f, parameters_.stereoWidth);
        resonance_.prepare (
            spec_.sampleRate,
            70.0f,
            parameters_.cabinetResonance);
        airStep_.prepare (
            spec_.sampleRate, 75.0f, initialAir);
        early_.prepare (
            spec_.sampleRate,
            90.0f,
            parameters_.earlyReflections);
        monoBlend_.prepare (
            spec_.sampleRate, 75.0f, 0.0f);
        output_.prepare (
            spec_.sampleRate, 35.0f, parameters_.output);
        // The wet cabinet has deliberately frequency-dependent losses. A
        // very slow calibration stage restores integrated energy without
        // behaving like a short-window AGC or following rotor modulation.
        wetCalibration_.prepare (
            spec_.sampleRate, 3500.0f, 1.20f);

        bodyStep_ =
            dsp::onePoleStepForCutoff (spec_.sampleRate, 118.0f);
        loudnessMeterStep_ = dsp::smoothingStep (
            spec_.sampleRate, 1500.0f);

        prepared_ = true;
        updateTargets();
        reset();

        // A preset selected before prepare() is the initial state, not an
        // automation move. Start its smoothed controls at the calibrated
        // values so the first rendered block does not travel from defaults.
        crossoverStep_.reset (crossoverStep_.target());
        wetMix_.reset (wetMix_.target());
        hornLevel_.reset (hornLevel_.target());
        drumLevel_.reset (drumLevel_.target());
        doppler_.reset (doppler_.target());
        amplitude_.reset (amplitude_.target());
        distance_.reset (distance_.target());
        width_.reset (width_.target());
        resonance_.reset (resonance_.target());
        airStep_.reset (airStep_.target());
        early_.reset (early_.target());
        monoBlend_.reset (monoBlend_.target());
        output_.reset (output_.target());
        wetCalibration_.reset (1.20f);
        return true;
    }

    void RotaryCabinet::reset() noexcept
    {
        hornLine_.reset();
        drumLine_.reset();
        sideLine_.reset();
        crossoverState_ = 0.0f;
        leftAirState_ = 0.0f;
        rightAirState_ = 0.0f;
        leftBodyState_ = 0.0f;
        rightBodyState_ = 0.0f;
        dryPowerState_ = 0.0f;
        wetPowerState_ = 0.0f;
        dryWetCrossState_ = 0.0f;
        dryWetCorrelation_ = 0.0f;
        loudnessUpdateCounter_ = 0;
        wetCalibration_.reset (1.20f);
        lastPeak_ = 0.0f;

        hornPhase_ = dsp::wrapUnitPhase (spec_.initialHornPhase);
        drumPhase_ = dsp::wrapUnitPhase (spec_.initialDrumPhase);
        hornSin_ = std::sin (hornPhase_ * dsp::twoPi);
        hornCos_ = std::cos (hornPhase_ * dsp::twoPi);
        drumSin_ = std::sin (drumPhase_ * dsp::twoPi);
        drumCos_ = std::cos (drumPhase_ * dsp::twoPi);
        rotorNormaliseCounter_ = 0;

        switch (parameters_.speedMode)
        {
            case RotarySpeedMode::fast:
                hornSpeedHz_ = hornTargetHz_;
                drumSpeedHz_ = drumTargetHz_;
                break;
            case RotarySpeedMode::slow:
                hornSpeedHz_ = hornTargetHz_;
                drumSpeedHz_ = drumTargetHz_;
                break;
            case RotarySpeedMode::stop:
            case RotarySpeedMode::brake:
            default:
                hornSpeedHz_ = 0.0f;
                drumSpeedHz_ = 0.0f;
                break;
        }
    }

    void RotaryCabinet::setParameters (
        const RotaryParameters& requested) noexcept
    {
        parameters_ = requested;
        parameters_.mix = dsp::clamp (
            dsp::finiteOr (requested.mix, 1.0f), 0.0f, 1.0f);
        parameters_.crossoverHz = dsp::clamp (
            dsp::finiteOr (requested.crossoverHz, 760.0f),
            320.0f,
            1800.0f);
        parameters_.hornLevel = dsp::clamp (
            dsp::finiteOr (requested.hornLevel, 0.78f),
            0.0f,
            1.5f);
        parameters_.drumLevel = dsp::clamp (
            dsp::finiteOr (requested.drumLevel, 0.88f),
            0.0f,
            1.5f);
        parameters_.hornSlowHz = dsp::clamp (
            dsp::finiteOr (requested.hornSlowHz, 0.76f),
            0.15f,
            2.0f);
        parameters_.hornFastHz = dsp::clamp (
            dsp::finiteOr (requested.hornFastHz, 6.15f),
            2.0f,
            9.0f);
        parameters_.drumSlowHz = dsp::clamp (
            dsp::finiteOr (requested.drumSlowHz, 0.62f),
            0.10f,
            1.8f);
        parameters_.drumFastHz = dsp::clamp (
            dsp::finiteOr (requested.drumFastHz, 5.05f),
            1.5f,
            8.0f);
        parameters_.hornAccelerationSeconds = sanitiseSeconds (
            requested.hornAccelerationSeconds, 1.20f);
        parameters_.hornDecelerationSeconds = sanitiseSeconds (
            requested.hornDecelerationSeconds, 0.86f);
        parameters_.drumAccelerationSeconds = sanitiseSeconds (
            requested.drumAccelerationSeconds, 3.85f);
        parameters_.drumDecelerationSeconds = sanitiseSeconds (
            requested.drumDecelerationSeconds, 3.10f);
        parameters_.brakeSeconds = sanitiseSeconds (
            requested.brakeSeconds, 0.22f);
        parameters_.doppler = dsp::clamp (
            dsp::finiteOr (requested.doppler, 0.72f),
            0.0f,
            1.0f);
        parameters_.amplitudeModulation = dsp::clamp (
            dsp::finiteOr (
                requested.amplitudeModulation,
                0.66f),
            0.0f,
            1.0f);
        parameters_.micDistance = dsp::clamp (
            dsp::finiteOr (requested.micDistance, 0.45f),
            0.0f,
            1.0f);
        parameters_.stereoWidth = dsp::clamp (
            dsp::finiteOr (requested.stereoWidth, 0.64f),
            0.0f,
            1.0f);
        parameters_.cabinetResonance = dsp::clamp (
            dsp::finiteOr (
                requested.cabinetResonance,
                0.34f),
            0.0f,
            1.0f);
        parameters_.airAbsorption = dsp::clamp (
            dsp::finiteOr (requested.airAbsorption, 0.22f),
            0.0f,
            1.0f);
        parameters_.earlyReflections = dsp::clamp (
            dsp::finiteOr (
                requested.earlyReflections,
                0.12f),
            0.0f,
            1.0f);
        parameters_.worshipIntensity = dsp::clamp (
            dsp::finiteOr (
                requested.worshipIntensity,
                0.0f),
            0.0f,
            1.0f);
        parameters_.output = dsp::clamp (
            dsp::finiteOr (requested.output, 0.90f),
            0.0f,
            1.5f);

        if (prepared_)
            updateTargets();
    }

    RotaryParameters RotaryCabinet::parameters() const noexcept
    {
        return parameters_;
    }

    void RotaryCabinet::setSpeedMode (
        RotarySpeedMode mode) noexcept
    {
        parameters_.speedMode = mode;
        if (prepared_)
            updateTargets();
    }

    void RotaryCabinet::process (
        float* left,
        float* right,
        std::size_t numSamples) noexcept
    {
        if (! prepared_ || left == nullptr || numSamples == 0)
            return;

        const auto stereo = right != nullptr && right != left;
        const auto sampleRate = static_cast<float> (spec_.sampleRate);
        const auto inverseSampleRate = 1.0f / sampleRate;
        const auto direction =
            parameters_.reverseDirection ? -1.0f : 1.0f;
        auto peak = 0.0f;

        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto dryLeft = dsp::finiteOr (left[sample], 0.0f);
            const auto dryRight = stereo
                ? dsp::finiteOr (right[sample], 0.0f)
                : dryLeft;
            const auto mid = 0.5f * (dryLeft + dryRight);
            const auto side = 0.5f * (dryLeft - dryRight);

            const auto crossover = crossoverStep_.next();
            crossoverState_ +=
                crossover * (mid - crossoverState_);
            const auto drumInput = crossoverState_;
            const auto hornInput = mid - crossoverState_;
            hornLine_.push (hornInput);
            drumLine_.push (drumInput);
            sideLine_.push (side);

            updateRotorSpeeds();

            const auto wet = wetMix_.next();
            const auto hornLevel = hornLevel_.next();
            const auto drumLevel = drumLevel_.next();
            const auto doppler = doppler_.next();
            const auto amplitude = amplitude_.next();
            const auto distance = distance_.next();
            const auto width = width_.next();
            const auto resonance = resonance_.next();
            const auto airStep = airStep_.next();
            const auto early = early_.next();
            const auto monoBlend = monoBlend_.next();
            const auto outputGain = output_.next();

            const auto microphoneAngle =
                width * 1.12f;
            float microphoneSin = 0.0f;
            float microphoneCos = 1.0f;
            microphoneSinCos (
                microphoneAngle,
                microphoneSin,
                microphoneCos);
            float drumMicrophoneSin = 0.0f;
            float drumMicrophoneCos = 1.0f;
            microphoneSinCos (
                0.72f * microphoneAngle,
                drumMicrophoneSin,
                drumMicrophoneCos);

            const auto hornSinLeft =
                hornSin_ * microphoneCos
                + hornCos_ * microphoneSin;
            const auto hornSinRight =
                hornSin_ * microphoneCos
                - hornCos_ * microphoneSin;
            const auto hornCosLeft =
                hornCos_ * microphoneCos
                - hornSin_ * microphoneSin;
            const auto hornCosRight =
                hornCos_ * microphoneCos
                + hornSin_ * microphoneSin;
            const auto drumSinLeft =
                drumSin_ * drumMicrophoneCos
                + drumCos_ * drumMicrophoneSin;
            const auto drumSinRight =
                drumSin_ * drumMicrophoneCos
                - drumCos_ * drumMicrophoneSin;
            const auto drumCosLeft =
                drumCos_ * drumMicrophoneCos
                - drumSin_ * drumMicrophoneSin;
            const auto drumCosRight =
                drumCos_ * drumMicrophoneCos
                + drumSin_ * drumMicrophoneSin;

            const auto baseSeconds =
                (baseLatencyMilliseconds + 1.35f * distance)
                * 0.001f;
            const auto hornPathScale =
                hornRadiusMeters
                / speedOfSoundMetersPerSecond
                * doppler;
            const auto drumPathScale =
                drumRadiusMeters
                / speedOfSoundMetersPerSecond
                * doppler;
            const auto hornDelayLeft =
                (baseSeconds + hornPathScale * hornSinLeft)
                * sampleRate;
            const auto hornDelayRight =
                (baseSeconds + hornPathScale * hornSinRight)
                * sampleRate;
            const auto drumDelayLeft =
                (baseSeconds + drumPathScale * drumSinLeft)
                * sampleRate;
            const auto drumDelayRight =
                (baseSeconds + drumPathScale * drumSinRight)
                * sampleRate;

            const auto hornDirectivityLeft =
                0.78f
                + amplitude
                    * (0.18f * hornCosLeft
                       + 0.04f
                           * (2.0f * hornCosLeft * hornCosLeft
                              - 1.0f));
            const auto hornDirectivityRight =
                0.78f
                + amplitude
                    * (0.18f * hornCosRight
                       + 0.04f
                           * (2.0f * hornCosRight * hornCosRight
                              - 1.0f));
            const auto drumDirectivityLeft =
                0.86f
                + amplitude
                    * (0.115f * drumCosLeft
                       + 0.025f
                           * (2.0f * drumCosLeft * drumCosLeft
                              - 1.0f));
            const auto drumDirectivityRight =
                0.86f
                + amplitude
                    * (0.115f * drumCosRight
                       + 0.025f
                           * (2.0f * drumCosRight * drumCosRight
                              - 1.0f));

            auto wetLeft =
                hornLevel
                    * hornLine_.read (hornDelayLeft)
                    * hornDirectivityLeft
                + drumLevel
                    * drumLine_.read (drumDelayLeft)
                    * drumDirectivityLeft;
            auto wetRight =
                hornLevel
                    * hornLine_.read (hornDelayRight)
                    * hornDirectivityRight
                + drumLevel
                    * drumLine_.read (drumDelayRight)
                    * drumDirectivityRight;

            const auto baseDelay =
                (baseLatencyMilliseconds
                 + 1.35f * distance)
                * 0.001f
                * sampleRate;
            const auto reflectedLeft =
                0.58f
                    * hornLine_.read (
                        baseDelay + 0.0041f * sampleRate)
                + 0.42f
                    * drumLine_.read (
                        baseDelay + 0.0067f * sampleRate);
            const auto reflectedRight =
                0.54f
                    * hornLine_.read (
                        baseDelay + 0.0060f * sampleRate)
                + 0.46f
                    * drumLine_.read (
                        baseDelay + 0.0048f * sampleRate);
            wetLeft += early * 0.28f * reflectedLeft;
            wetRight += early * 0.28f * reflectedRight;

            // Preserve a modest amount of an already-stereo source without
            // masking the cabinet geometry.
            const auto delayedSide =
                sideLine_.read (baseDelay);
            const auto sideAmount =
                (0.08f + 0.10f * (1.0f - distance))
                * width;
            wetLeft += delayedSide * sideAmount;
            wetRight -= delayedSide * sideAmount;

            // Keep a quiet, delayed full-range cabinet base beneath horn and
            // drum directivity. This is a physical cabinet floor, not a dry
            // bypass: it carries the same propagation delay and subsequent
            // body/air filtering as the rotor path, and prevents angular
            // nulls from making a fully-wet organ disappear.
            const auto cabinetBase =
                hornLine_.read (baseDelay)
                + drumLine_.read (baseDelay);
            // The rotor path necessarily carries the cabinet propagation
            // delay.  Mixing it with the original, undelayed input creates a
            // fixed comb filter whenever mix is between 0 and 1.  Rebuild the
            // dry reference from the same delayed mid/side history so the
            // crossfade changes cabinet colour without adding metallic phase
            // notches.
            const auto alignedDryLeft = cabinetBase + delayedSide;
            const auto alignedDryRight = cabinetBase - delayedSide;
            const auto cabinetFloor =
                0.14f
                + 0.045f * (1.0f - distance)
                + 0.035f * resonance;
            wetLeft += cabinetFloor * cabinetBase;
            wetRight += cabinetFloor * cabinetBase;

            leftBodyState_ +=
                bodyStep_ * (wetLeft - leftBodyState_);
            rightBodyState_ +=
                bodyStep_ * (wetRight - rightBodyState_);
            wetLeft += resonance * 0.12f * leftBodyState_;
            wetRight += resonance * 0.12f * rightBodyState_;

            leftAirState_ +=
                airStep * (wetLeft - leftAirState_);
            rightAirState_ +=
                airStep * (wetRight - rightAirState_);
            wetLeft = leftAirState_;
            wetRight = rightAirState_;

            const auto wetMono = 0.5f * (wetLeft + wetRight);
            wetLeft += monoBlend * (wetMono - wetLeft);
            wetRight += monoBlend * (wetMono - wetRight);

            const auto dryPower = stereo
                ? 0.5f
                    * (alignedDryLeft * alignedDryLeft
                       + alignedDryRight * alignedDryRight)
                : alignedDryLeft * alignedDryLeft;
            const auto wetPower = stereo
                ? 0.5f
                    * (wetLeft * wetLeft + wetRight * wetRight)
                : wetLeft * wetLeft;
            const auto crossPower = stereo
                ? 0.5f
                    * (alignedDryLeft * wetLeft
                       + alignedDryRight * wetRight)
                : alignedDryLeft * wetLeft;

            dryPowerState_ += loudnessMeterStep_
                * (dryPower - dryPowerState_);
            wetPowerState_ += loudnessMeterStep_
                * (wetPower - wetPowerState_);
            dryWetCrossState_ += loudnessMeterStep_
                * (crossPower - dryWetCrossState_);

            // Recalculate the long-term targets only every 64 samples. Both
            // the 1.5 s detector and 3.5 s gain smoother are intentionally
            // much slower than horn/drum modulation, so this cannot pump at
            // musical or rotor rates.
            if (++loudnessUpdateCounter_ >= 64U)
            {
                loudnessUpdateCounter_ = 0;
                if (dryPowerState_ > 1.0e-9f
                    && wetPowerState_ > 1.0e-10f)
                {
                    const auto targetGain = std::sqrt (
                        dryPowerState_ / wetPowerState_);
                    wetCalibration_.setTarget (
                        dsp::clamp (targetGain, 0.68f, 2.25f));

                    const auto denominator = std::sqrt (
                        dryPowerState_ * wetPowerState_);
                    dryWetCorrelation_ = dsp::clamp (
                        dryWetCrossState_ / denominator,
                        -0.55f,
                        0.92f);
                }
            }

            const auto wetCalibrationGain =
                wetCalibration_.next();
            wetLeft *= wetCalibrationGain;
            wetRight *= wetCalibrationGain;

            // Equal-power interpolation avoids the -6 dB centre dip of a
            // linear crossfade. The slow correlation estimate then removes
            // the gain bump for similar dry/wet material and limits
            // cancellation for negatively correlated material.
            const auto dryWeight = std::sqrt (
                std::max (0.0f, 1.0f - wet));
            const auto wetWeight = std::sqrt (
                std::max (0.0f, wet));
            const auto correlationEnergy = std::max (
                0.42f,
                1.0f
                    + 2.0f
                        * dryWetCorrelation_
                        * dryWeight
                        * wetWeight);
            const auto correlationCompensation = dsp::clamp (
                1.0f / std::sqrt (correlationEnergy),
                0.78f,
                1.32f);

            auto outputLeft =
                (dryWeight * alignedDryLeft + wetWeight * wetLeft)
                * correlationCompensation
                * outputGain;
            auto outputRight =
                (dryWeight * alignedDryRight + wetWeight * wetRight)
                * correlationCompensation
                * outputGain;

            outputLeft = dsp::softPeakProtect (
                outputLeft, 0.82f, 0.995f);
            outputRight = dsp::softPeakProtect (
                outputRight, 0.82f, 0.995f);
            left[sample] = dsp::finiteOr (outputLeft, 0.0f);
            if (stereo)
                right[sample] =
                    dsp::finiteOr (outputRight, 0.0f);

            peak = std::max (
                peak,
                std::max (
                    std::abs (left[sample]),
                    std::abs (stereo
                                  ? right[sample]
                                  : left[sample])));

            advanceRotor (
                hornPhase_,
                hornSin_,
                hornCos_,
                hornSpeedHz_,
                direction,
                inverseSampleRate);
            advanceRotor (
                drumPhase_,
                drumSin_,
                drumCos_,
                drumSpeedHz_,
                direction,
                inverseSampleRate);

            if (++rotorNormaliseCounter_ >= 2048U)
            {
                rotorNormaliseCounter_ = 0;
                const auto hornNormalisation = 1.0f / std::sqrt (
                    hornSin_ * hornSin_ + hornCos_ * hornCos_);
                const auto drumNormalisation = 1.0f / std::sqrt (
                    drumSin_ * drumSin_ + drumCos_ * drumCos_);
                hornSin_ *= hornNormalisation;
                hornCos_ *= hornNormalisation;
                drumSin_ *= drumNormalisation;
                drumCos_ *= drumNormalisation;
            }
        }

        lastPeak_ = dsp::finiteOr (peak, 0.0f);
    }

    RotaryDiagnostics RotaryCabinet::diagnostics() const noexcept
    {
        return {
            hornSpeedHz_,
            drumSpeedHz_,
            hornTargetHz_,
            drumTargetHz_,
            hornPhase_,
            drumPhase_,
            wetCalibration_.current(),
            dryWetCorrelation_,
            lastPeak_
        };
    }

    float RotaryCabinet::nominalWetLatencyMilliseconds() const noexcept
    {
        return baseLatencyMilliseconds;
    }

    RotaryCabinet::PerspectiveCalibration
    RotaryCabinet::calibration (
        RotaryPerspective perspective) noexcept
    {
        switch (perspective)
        {
            case RotaryPerspective::stageClose:
                return {
                    0.58f, 0.82f, 1.08f, 1.10f,
                    0.00f, -0.08f, 0.0f
                };
            case RotaryPerspective::openRoom:
                return {
                    1.55f, 0.88f, 0.82f, 0.78f,
                    0.28f, 0.24f, 0.08f
                };
            case RotaryPerspective::vintageMono:
                return {
                    0.92f, 0.0f, 0.82f, 0.91f,
                    0.08f, 0.10f, 1.0f
                };
            case RotaryPerspective::wideWorship:
                return {
                    1.18f, 1.42f, 0.92f, 0.88f,
                    0.19f, 0.15f, 0.0f
                };
            case RotaryPerspective::balanced:
            default:
                return {
                    1.0f, 1.0f, 1.0f, 1.0f,
                    0.04f, 0.02f, 0.0f
                };
        }
    }

    void RotaryCabinet::updateTargets() noexcept
    {
        const auto profile = calibration (
            parameters_.perspective);
        const auto worship = parameters_.worshipIntensity;
        const auto distance =
            dsp::clamp (
                parameters_.micDistance
                    * profile.distanceScale
                    + 0.10f * worship,
                0.0f,
                1.0f);
        const auto width =
            dsp::clamp (
                parameters_.stereoWidth
                    * profile.widthScale
                    + 0.12f * worship,
                0.0f,
                1.0f);
        const auto doppler =
            dsp::clamp (
                parameters_.doppler
                    * profile.dopplerScale
                    * (1.0f - 0.08f * worship),
                0.0f,
                1.0f);
        const auto amplitude =
            dsp::clamp (
                parameters_.amplitudeModulation
                    * profile.amplitudeScale,
                0.0f,
                1.0f);
        const auto air =
            dsp::clamp (
                parameters_.airAbsorption
                    + profile.addedAir
                    + 0.08f * worship,
                0.0f,
                1.0f);
        const auto early =
            dsp::clamp (
                parameters_.earlyReflections
                    + profile.addedEarly
                    + 0.08f * worship,
                0.0f,
                1.0f);
        const auto airCutoff =
            16400.0f - 8800.0f * air - 2100.0f * distance;

        crossoverStep_.setTarget (
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                parameters_.crossoverHz));
        wetMix_.setTarget (parameters_.mix);
        hornLevel_.setTarget (parameters_.hornLevel);
        drumLevel_.setTarget (parameters_.drumLevel);
        doppler_.setTarget (doppler);
        amplitude_.setTarget (amplitude);
        distance_.setTarget (distance);
        width_.setTarget (width);
        resonance_.setTarget (
            dsp::clamp (
                parameters_.cabinetResonance
                    + 0.08f * worship,
                0.0f,
                1.0f));
        airStep_.setTarget (
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                airCutoff));
        early_.setTarget (early);
        monoBlend_.setTarget (profile.monoBlend);
        output_.setTarget (parameters_.output);

        const auto slowScale = 1.0f - 0.08f * worship;
        const auto fastScale = 1.0f - 0.035f * worship;
        switch (parameters_.speedMode)
        {
            case RotarySpeedMode::slow:
                hornTargetHz_ =
                    parameters_.hornSlowHz * slowScale;
                drumTargetHz_ =
                    parameters_.drumSlowHz * slowScale;
                break;
            case RotarySpeedMode::fast:
                hornTargetHz_ =
                    parameters_.hornFastHz * fastScale;
                drumTargetHz_ =
                    parameters_.drumFastHz * fastScale;
                break;
            case RotarySpeedMode::stop:
            case RotarySpeedMode::brake:
            default:
                hornTargetHz_ = 0.0f;
                drumTargetHz_ = 0.0f;
                break;
        }

        hornAccelerationStep_ = dsp::smoothingStep (
            spec_.sampleRate,
            parameters_.hornAccelerationSeconds * 1000.0f);
        hornDecelerationStep_ = dsp::smoothingStep (
            spec_.sampleRate,
            parameters_.hornDecelerationSeconds * 1000.0f);
        drumAccelerationStep_ = dsp::smoothingStep (
            spec_.sampleRate,
            parameters_.drumAccelerationSeconds * 1000.0f);
        drumDecelerationStep_ = dsp::smoothingStep (
            spec_.sampleRate,
            parameters_.drumDecelerationSeconds * 1000.0f);
        brakeStep_ = dsp::smoothingStep (
            spec_.sampleRate,
            parameters_.brakeSeconds * 1000.0f);
    }

    void RotaryCabinet::updateRotorSpeeds() noexcept
    {
        const auto braking =
            parameters_.speedMode == RotarySpeedMode::brake;
        const auto hornStep = braking
            ? brakeStep_
            : (hornTargetHz_ > hornSpeedHz_
                   ? hornAccelerationStep_
                   : hornDecelerationStep_);
        const auto drumStep = braking
            ? brakeStep_
            : (drumTargetHz_ > drumSpeedHz_
                   ? drumAccelerationStep_
                   : drumDecelerationStep_);

        hornSpeedHz_ +=
            hornStep * (hornTargetHz_ - hornSpeedHz_);
        drumSpeedHz_ +=
            drumStep * (drumTargetHz_ - drumSpeedHz_);

        if (hornTargetHz_ == 0.0f
            && hornSpeedHz_ < 0.00001f)
        {
            hornSpeedHz_ = 0.0f;
        }
        if (drumTargetHz_ == 0.0f
            && drumSpeedHz_ < 0.00001f)
        {
            drumSpeedHz_ = 0.0f;
        }
    }

    float RotaryCabinet::hornDelaySamples (
        float microphoneAngle,
        float distance,
        float doppler) const noexcept
    {
        const auto angle =
            hornPhase_ * dsp::twoPi - microphoneAngle;
        const auto pathSeconds =
            hornRadiusMeters
            / speedOfSoundMetersPerSecond
            * std::sin (angle)
            * doppler;
        const auto baseSeconds =
            (baseLatencyMilliseconds + 1.35f * distance)
            * 0.001f;
        return (baseSeconds + pathSeconds)
            * static_cast<float> (spec_.sampleRate);
    }

    float RotaryCabinet::drumDelaySamples (
        float microphoneAngle,
        float distance,
        float doppler) const noexcept
    {
        const auto angle =
            drumPhase_ * dsp::twoPi - microphoneAngle;
        const auto pathSeconds =
            drumRadiusMeters
            / speedOfSoundMetersPerSecond
            * std::sin (angle)
            * doppler;
        const auto baseSeconds =
            (baseLatencyMilliseconds + 1.35f * distance)
            * 0.001f;
        return (baseSeconds + pathSeconds)
            * static_cast<float> (spec_.sampleRate);
    }

    float RotaryCabinet::hornDirectivity (
        float angle,
        float amount) noexcept
    {
        return 0.78f
            + amount
                * (0.18f * std::cos (angle)
                   + 0.04f * std::cos (2.0f * angle));
    }

    float RotaryCabinet::drumDirectivity (
        float angle,
        float amount) noexcept
    {
        return 0.86f
            + amount
                * (0.115f * std::cos (angle)
                   + 0.025f * std::cos (2.0f * angle));
    }
}
