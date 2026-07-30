#include "OrganAmpCab.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 8000.0;
        constexpr double maximumSampleRate = 384000.0;
    }

    bool OrganAmpCab::prepare (
        const OrganAmpPrepareSpec& requestedSpec) noexcept
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
        oversamplingFactor_ = factorFor (spec_.oversampling);

        lowTightStep_ =
            dsp::onePoleStepForCutoff (spec_.sampleRate, 72.0f);
        driveBandStep_ =
            dsp::onePoleStepForCutoff (spec_.sampleRate, 920.0f);
        envelopeAttackStep_ =
            dsp::smoothingStep (spec_.sampleRate, 7.0f);
        envelopeReleaseStep_ =
            dsp::smoothingStep (spec_.sampleRate, 145.0f);
        bodyStep_ =
            dsp::onePoleStepForCutoff (spec_.sampleRate, 145.0f);
        dcCoefficient_ = static_cast<float> (
            std::exp (
                -static_cast<double> (dsp::twoPi) * 11.0
                / spec_.sampleRate));

        const auto oversampledRate =
            spec_.sampleRate
            * static_cast<double> (oversamplingFactor_);
        const auto antiAliasCutoff =
            std::min (
                spec_.sampleRate * 0.43,
                oversampledRate * 0.45);
        // oversampledRate can legitimately exceed the public 384 kHz host
        // limit (for example 192 kHz x4). Compute this internal coefficient
        // directly instead of passing through the host-rate sanitizer.
        antiAliasStep_ = static_cast<float> (
            1.0 - std::exp (
                -static_cast<double> (dsp::twoPi)
                * antiAliasCutoff
                / oversampledRate));

        const auto profile = calibration (parameters_.profile);
        const auto initialPreGain = std::pow (
            10.0f,
            (profile.baseDriveDecibels
             + parameters_.drive * profile.driveRangeDecibels)
                / 20.0f);
        const auto initialCabinetStep =
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                profile.cabinetCutoffHz);

        preGain_.prepare (
            spec_.sampleRate, 24.0f, initialPreGain);
        bias_.prepare (
            spec_.sampleRate,
            35.0f,
            parameters_.bias * profile.biasScale);
        sag_.prepare (
            spec_.sampleRate,
            55.0f,
            parameters_.sag * profile.sagScale);
        tightening_.prepare (
            spec_.sampleRate, 48.0f, parameters_.lowTightening);
        tone_.prepare (
            spec_.sampleRate, 42.0f, parameters_.tone);
        presence_.prepare (
            spec_.sampleRate, 42.0f, parameters_.presence);
        cabinet_.prepare (
            spec_.sampleRate, 65.0f, parameters_.cabinet);
        cabinetStep_.prepare (
            spec_.sampleRate, 70.0f, initialCabinetStep);
        outputGain_.prepare (
            spec_.sampleRate,
            35.0f,
            parameters_.output * profile.outputTrim);
        powerDrive_.prepare (
            spec_.sampleRate, 45.0f, profile.powerDrive);

        prepared_ = true;
        reset();
        updateTargets();
        preGain_.reset (preGain_.target());
        bias_.reset (bias_.target());
        sag_.reset (sag_.target());
        tightening_.reset (tightening_.target());
        tone_.reset (tone_.target());
        presence_.reset (presence_.target());
        cabinet_.reset (cabinet_.target());
        cabinetStep_.reset (cabinetStep_.target());
        outputGain_.reset (outputGain_.target());
        powerDrive_.reset (powerDrive_.target());
        return true;
    }

    void OrganAmpCab::reset() noexcept
    {
        left_ = {};
        right_ = {};
        lastPeak_ = 0.0f;
    }

    void OrganAmpCab::setParameters (
        const OrganAmpParameters& requested) noexcept
    {
        parameters_ = requested;
        parameters_.drive = dsp::clamp (
            dsp::finiteOr (requested.drive, 0.28f), 0.0f, 1.0f);
        parameters_.bias = dsp::clamp (
            dsp::finiteOr (requested.bias, 0.35f), 0.0f, 1.0f);
        parameters_.sag = dsp::clamp (
            dsp::finiteOr (requested.sag, 0.28f), 0.0f, 1.0f);
        parameters_.lowTightening = dsp::clamp (
            dsp::finiteOr (requested.lowTightening, 0.24f),
            0.0f,
            1.0f);
        parameters_.tone = dsp::clamp (
            dsp::finiteOr (requested.tone, 0.55f), 0.0f, 1.0f);
        parameters_.presence = dsp::clamp (
            dsp::finiteOr (requested.presence, 0.42f), 0.0f, 1.0f);
        parameters_.cabinet = dsp::clamp (
            dsp::finiteOr (requested.cabinet, 0.62f), 0.0f, 1.0f);
        parameters_.worshipIntensity = dsp::clamp (
            dsp::finiteOr (requested.worshipIntensity, 0.0f),
            0.0f,
            1.0f);
        parameters_.output = dsp::clamp (
            dsp::finiteOr (requested.output, 0.92f), 0.0f, 1.5f);

        if (prepared_)
            updateTargets();
    }

    OrganAmpParameters OrganAmpCab::parameters() const noexcept
    {
        return parameters_;
    }

    void OrganAmpCab::process (
        float* left,
        float* right,
        std::size_t numSamples) noexcept
    {
        if (! prepared_ || left == nullptr || numSamples == 0)
            return;

        const auto stereo = right != nullptr && right != left;
        auto peak = 0.0f;

        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto preGain = preGain_.next();
            const auto bias = dsp::clamp (
                dsp::finiteOr (bias_.next(), 0.0f),
                -0.24f,
                0.24f);
            const auto sag = sag_.next();
            const auto tightening = tightening_.next();
            const auto tone = tone_.next();
            const auto presence = presence_.next();
            const auto cabinet = cabinet_.next();
            const auto cabinetStep = cabinetStep_.next();
            const auto outputGain = outputGain_.next();
            const auto powerDrive = dsp::clamp (
                dsp::finiteOr (powerDrive_.next(), 1.0f),
                0.8f,
                1.8f);
            // Bias moves on a tens-of-milliseconds control smoother and is
            // shared by both channels and every oversampled sub-step.
            // Calculate its DC compensation once per output sample.
            const auto primaryOffset = std::tanh (bias);
            const auto secondaryOffset =
                std::tanh (-0.38f * bias);

            const auto processedLeft = processSample (
                left[sample],
                left_,
                preGain,
                bias,
                primaryOffset,
                secondaryOffset,
                sag,
                tightening,
                tone,
                presence,
                cabinet,
                cabinetStep,
                outputGain,
                powerDrive);
            left[sample] = processedLeft;
            peak = std::max (peak, std::abs (processedLeft));

            if (stereo)
            {
                const auto processedRight = processSample (
                    right[sample],
                    right_,
                    preGain,
                    bias,
                    primaryOffset,
                    secondaryOffset,
                    sag,
                    tightening,
                    tone,
                    presence,
                    cabinet,
                    cabinetStep,
                    outputGain,
                    powerDrive);
                right[sample] = processedRight;
                peak = std::max (peak, std::abs (processedRight));
            }
        }

        lastPeak_ = dsp::finiteOr (peak, 0.0f);
    }

    int OrganAmpCab::oversamplingFactor() const noexcept
    {
        return oversamplingFactor_;
    }

    float OrganAmpCab::lastPeak() const noexcept
    {
        return lastPeak_;
    }

    OrganAmpCab::ProfileCalibration OrganAmpCab::calibration (
        OrganAmpProfile profile) noexcept
    {
        switch (profile)
        {
            case OrganAmpProfile::clean:
                return {
                    0.0f, 14.0f, 0.045f, 0.15f,
                    13200.0f, 1.02f, 0.98f
                };
            case OrganAmpProfile::driven:
                return {
                    5.0f, 23.0f, 0.145f, 0.52f,
                    8200.0f, 1.34f, 0.88f
                };
            case OrganAmpProfile::worshipSmooth:
                return {
                    2.0f, 16.0f, 0.075f, 0.31f,
                    9400.0f, 1.12f, 0.94f
                };
            case OrganAmpProfile::warm:
            default:
                return {
                    2.5f, 18.0f, 0.095f, 0.38f,
                    10200.0f, 1.18f, 0.92f
                };
        }
    }

    int OrganAmpCab::factorFor (
        OrganAmpOversampling quality) noexcept
    {
        switch (quality)
        {
            case OrganAmpOversampling::x2:
                return 2;
            case OrganAmpOversampling::x4:
                return 4;
            case OrganAmpOversampling::x8:
                return 8;
            case OrganAmpOversampling::off:
            default:
                return 1;
        }
    }

    float OrganAmpCab::nonlinearStage (
        float input,
        float bias,
        float primaryOffset,
        float secondaryOffset,
        float powerDrive) noexcept
    {
        input = dsp::clamp (
            dsp::finiteOr (input, 0.0f), -18.0f, 18.0f);

        const auto primary =
            std::tanh (input + bias) - primaryOffset;
        const auto secondary =
            std::tanh (0.58f * input - 0.38f * bias)
            - secondaryOffset;
        const auto preamp = 0.84f * primary + 0.16f * secondary;

        return std::tanh (
            preamp * powerDrive);
    }

    float OrganAmpCab::processSample (
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
        float powerDrive) noexcept
    {
        input = dsp::finiteOr (input, 0.0f);

        state.lowTight +=
            lowTightStep_ * (input - state.lowTight);
        const auto tightened =
            input - tightening * 0.42f * state.lowTight;

        state.driveBand +=
            driveBandStep_ * (tightened - state.driveBand);
        const auto highBand = tightened - state.driveBand;
        const auto frequencyShaped =
            state.driveBand * (1.0f - 0.13f * tightening)
            + highBand * (0.82f + 0.34f * tone);

        const auto envelopeTarget =
            std::abs (frequencyShaped) * preGain;
        const auto envelopeStep =
            envelopeTarget > state.envelope
                ? envelopeAttackStep_
                : envelopeReleaseStep_;
        state.envelope +=
            envelopeStep * (envelopeTarget - state.envelope);
        const auto sagGain =
            1.0f
            / (1.0f + sag * 0.52f * state.envelope);
        const auto drivenInput =
            frequencyShaped * preGain * sagGain;

        const auto previous = state.previousInput;
        const auto inverseFactor =
            1.0f / static_cast<float> (oversamplingFactor_);
        auto nonlinear = state.antiAlias2;

        for (int subSample = 1;
             subSample <= oversamplingFactor_;
             ++subSample)
        {
            const auto fraction =
                static_cast<float> (subSample) * inverseFactor;
            const auto interpolated =
                previous + (drivenInput - previous) * fraction;
            const auto saturated = nonlinearStage (
                interpolated,
                bias,
                primaryOffset,
                secondaryOffset,
                powerDrive);

            state.antiAlias1 +=
                antiAliasStep_
                * (saturated - state.antiAlias1);
            state.antiAlias2 +=
                antiAliasStep_
                * (state.antiAlias1 - state.antiAlias2);
            nonlinear = state.antiAlias2;
        }
        state.previousInput = drivenInput;

        // DC blocker after the asymmetric stages.
        auto dcBlocked =
            nonlinear - state.dcInput
            + dcCoefficient_ * state.dcOutput;
        state.dcInput = nonlinear;
        state.dcOutput = dcBlocked;

        state.cabinet1 +=
            cabinetStep * (dcBlocked - state.cabinet1);
        state.cabinet2 +=
            cabinetStep * (state.cabinet1 - state.cabinet2);
        const auto cabinetFiltered =
            dcBlocked
            + cabinet
                * (state.cabinet2 - dcBlocked);

        state.cabinetBody +=
            bodyStep_ * (cabinetFiltered - state.cabinetBody);
        auto output =
            cabinetFiltered
            + cabinet * 0.11f * state.cabinetBody;

        // Presence acts only on the residual high band and therefore cannot
        // turn into an uncontrolled broadband gain.
        output +=
            (presence - 0.5f)
            * 0.18f
            * (dcBlocked - state.cabinet1);
        output *= outputGain;

        if (parameters_.peakProtection)
            output = dsp::softPeakProtect (output);
        else
            output = dsp::clamp (output, -1.5f, 1.5f);

        return dsp::finiteOr (output, 0.0f);
    }

    void OrganAmpCab::updateTargets() noexcept
    {
        const auto profile = calibration (parameters_.profile);
        const auto worship = parameters_.worshipIntensity;
        const auto driveDecibels =
            profile.baseDriveDecibels
            + parameters_.drive * profile.driveRangeDecibels
            + worship * 1.8f;
        const auto preGain =
            std::pow (10.0f, driveDecibels / 20.0f);
        const auto autoGain =
            parameters_.automaticGain
                ? 1.0f
                    / std::sqrt (
                        1.0f + 0.34f * (preGain - 1.0f))
                : 1.0f;
        const auto cabinetAmount =
            dsp::clamp (
                parameters_.cabinet + 0.12f * worship,
                0.0f,
                1.0f);
        const auto tone =
            dsp::clamp (
                parameters_.tone - 0.08f * worship,
                0.0f,
                1.0f);
        const auto cutoff =
            profile.cabinetCutoffHz
            * (0.72f + 0.48f * tone)
            * (1.0f - 0.18f * cabinetAmount);

        preGain_.setTarget (preGain);
        bias_.setTarget (
            (0.35f + 0.65f * parameters_.bias)
            * profile.biasScale);
        sag_.setTarget (
            parameters_.sag * profile.sagScale
            + 0.055f * worship);
        tightening_.setTarget (parameters_.lowTightening);
        tone_.setTarget (tone);
        presence_.setTarget (
            dsp::clamp (
                parameters_.presence + 0.04f * worship,
                0.0f,
                1.0f));
        cabinet_.setTarget (cabinetAmount);
        cabinetStep_.setTarget (
            dsp::onePoleStepForCutoff (
                spec_.sampleRate,
                cutoff));
        outputGain_.setTarget (
            parameters_.output
            * profile.outputTrim
            * autoGain);
        powerDrive_.setTarget (
            profile.powerDrive + 0.08f * worship);
    }
}
