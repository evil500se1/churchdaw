#include "ScannerVibrato.h"

#include <array>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr std::array<float, 18> scannerTapShape {
            0.000000f,  0.342020f,  0.642788f,
            0.866025f,  0.984808f,  0.984808f,
            0.866025f,  0.642788f,  0.342020f,
            0.000000f, -0.342020f, -0.642788f,
           -0.866025f, -0.984808f, -0.984808f,
           -0.866025f, -0.642788f, -0.342020f
        };

        constexpr float baseDelayMilliseconds = 4.15f;
        constexpr float minimumSampleRate = 8000.0f;
        constexpr float maximumSampleRate = 384000.0f;

        inline float sineQuarterWave (float angle) noexcept
        {
            const auto squared = angle * angle;
            return angle
                * (1.0f
                   + squared
                       * (-1.0f / 6.0f
                          + squared
                              * (1.0f / 120.0f
                                 - squared / 5040.0f)));
        }
    }

    bool ScannerVibrato::prepare (
        const ScannerPrepareSpec& requestedSpec) noexcept
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
        spec_.initialPhase = dsp::wrapUnitPhase (
            dsp::finiteOr (requestedSpec.initialPhase, 0.137f));
        noiseState_ = requestedSpec.deterministicSeed != 0
            ? requestedSpec.deterministicSeed
            : 0x5343414eU;

        const auto initialCalibration = calibration (
            parameters_.mode,
            parameters_.character);
        const auto initialExcursion =
            initialCalibration.delayExcursionMilliseconds
            * 0.001f
            * static_cast<float> (spec_.sampleRate);
        const auto initialBandwidth =
            dsp::onePoleStepForCutoff (spec_.sampleRate, 14500.0f);

        rateHz_.prepare (
            spec_.sampleRate, 45.0f, initialCalibration.rateHz);
        excursionSamples_.prepare (
            spec_.sampleRate, 32.0f, initialExcursion);
        wetMix_.prepare (
            spec_.sampleRate, 28.0f, initialCalibration.wetMix);
        spreadPhase_.prepare (
            spec_.sampleRate, 45.0f, 0.0f);
        age_.prepare (
            spec_.sampleRate, 65.0f, parameters_.age);
        noiseGain_.prepare (
            spec_.sampleRate, 90.0f, 0.0f);
        output_.prepare (
            spec_.sampleRate, 24.0f, parameters_.output);
        bandwidthStep_.prepare (
            spec_.sampleRate, 65.0f, initialBandwidth);

        prepared_ = true;
        reset();
        updateTargets();
        rateHz_.reset (rateHz_.target());
        excursionSamples_.reset (excursionSamples_.target());
        wetMix_.reset (wetMix_.target());
        spreadPhase_.reset (spreadPhase_.target());
        age_.reset (age_.target());
        noiseGain_.reset (noiseGain_.target());
        output_.reset (output_.target());
        bandwidthStep_.reset (bandwidthStep_.target());
        return true;
    }

    void ScannerVibrato::reset() noexcept
    {
        leftDelay_.reset();
        rightDelay_.reset();
        leftBandwidthState1_ = 0.0f;
        leftBandwidthState2_ = 0.0f;
        rightBandwidthState1_ = 0.0f;
        rightBandwidthState2_ = 0.0f;
        phase_ = dsp::wrapUnitPhase (spec_.initialPhase);
    }

    void ScannerVibrato::setParameters (
        const ScannerParameters& requested) noexcept
    {
        parameters_ = requested;
        parameters_.rate = dsp::clamp (
            dsp::finiteOr (requested.rate, 1.0f), 0.5f, 1.5f);
        parameters_.depth = dsp::clamp (
            dsp::finiteOr (requested.depth, 1.0f), 0.0f, 1.0f);
        parameters_.mix = dsp::clamp (
            dsp::finiteOr (requested.mix, 1.0f), 0.0f, 1.0f);
        parameters_.stereoSpread = dsp::clamp (
            dsp::finiteOr (requested.stereoSpread, 0.16f),
            0.0f,
            1.0f);
        parameters_.age = dsp::clamp (
            dsp::finiteOr (requested.age, 0.12f), 0.0f, 1.0f);
        parameters_.noise = dsp::clamp (
            dsp::finiteOr (requested.noise, 0.0f), 0.0f, 1.0f);
        parameters_.worshipIntensity = dsp::clamp (
            dsp::finiteOr (requested.worshipIntensity, 0.0f),
            0.0f,
            1.0f);
        parameters_.output = dsp::clamp (
            dsp::finiteOr (requested.output, 1.0f), 0.0f, 1.5f);

        if (prepared_)
            updateTargets();
    }

    ScannerParameters ScannerVibrato::parameters() const noexcept
    {
        return parameters_;
    }

    void ScannerVibrato::process (
        float* left,
        float* right,
        std::size_t numSamples) noexcept
    {
        if (! prepared_ || left == nullptr || numSamples == 0)
            return;

        // Bypass estabilizado é o caso dominante no Pedal e em muitos
        // presets. O crossfade continua sendo processado enquanto o wet ainda
        // está descendo; depois disso o caminho fica realmente transparente e
        // sem custo de 18 taps por sample.
        const auto stableBypass =
            parameters_.mode == ScannerMode::bypass
            && std::abs (wetMix_.current()) < 0.000001f
            && std::abs (wetMix_.target()) < 0.000001f;

        const auto stereo = right != nullptr && right != left;
        const auto sampleRate = static_cast<float> (spec_.sampleRate);

        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto dryLeft = dsp::finiteOr (left[sample], 0.0f);
            const auto dryRight = stereo
                ? dsp::finiteOr (right[sample], 0.0f)
                : dryLeft;

            const auto bandwidth = bandwidthStep_.next();
            leftBandwidthState1_ +=
                bandwidth * (dryLeft - leftBandwidthState1_);
            leftBandwidthState2_ +=
                bandwidth
                * (leftBandwidthState1_ - leftBandwidthState2_);

            rightBandwidthState1_ +=
                bandwidth * (dryRight - rightBandwidthState1_);
            rightBandwidthState2_ +=
                bandwidth
                * (rightBandwidthState1_ - rightBandwidthState2_);

            const auto noiseGain = noiseGain_.next();
            auto rawNoiseLeft = 0.0f;
            auto rawNoiseRight = 0.0f;
            if (std::abs (noiseGain) > 1.0e-10f
                || std::abs (noiseGain_.target()) > 1.0e-10f)
            {
                rawNoiseLeft =
                    static_cast<float> (nextNoise (noiseState_))
                        * (1.0f / 2147483648.0f)
                    - 1.0f;
                rawNoiseRight =
                    static_cast<float> (nextNoise (noiseState_))
                        * (1.0f / 2147483648.0f)
                    - 1.0f;
            }

            leftDelay_.push (
                leftBandwidthState2_ + rawNoiseLeft * noiseGain);
            rightDelay_.push (
                rightBandwidthState2_ + rawNoiseRight * noiseGain);

            const auto rate = rateHz_.next();
            const auto excursion = excursionSamples_.next();
            const auto spread = spreadPhase_.next();
            const auto wet = dsp::clamp (wetMix_.next(), 0.0f, 1.0f);
            const auto age = age_.next();
            const auto outputGain = output_.next();

            if (! stableBypass)
            {
                auto leftPhase = phase_ - 0.5f * spread;
                if (leftPhase < 0.0f)
                    leftPhase += 1.0f;
                auto rightPhase = phase_ + 0.5f * spread;
                if (rightPhase >= 1.0f)
                    rightPhase -= 1.0f;

                auto wetLeft = scannerSample (
                    leftDelay_, leftPhase, excursion);
                auto wetRight = scannerSample (
                    rightDelay_, rightPhase, excursion);

                // A very gentle odd-order phase-line colour. It is bounded and
                // deliberately below obvious "drive" territory.
                wetLeft -= age * 0.055f * wetLeft * wetLeft * wetLeft;
                wetRight -= age * 0.055f * wetRight * wetRight * wetRight;

                left[sample] =
                    dsp::finiteOr (
                        ((1.0f - wet) * dryLeft + wet * wetLeft)
                            * outputGain,
                        0.0f);
                if (stereo)
                {
                    right[sample] =
                        dsp::finiteOr (
                            ((1.0f - wet) * dryRight + wet * wetRight)
                                * outputGain,
                            0.0f);
                }
            }

            phase_ += rate / sampleRate;
            if (phase_ >= 1.0f)
                phase_ -= 1.0f;
        }
    }

    float ScannerVibrato::currentPhase() const noexcept
    {
        return phase_;
    }

    float ScannerVibrato::nominalWetLatencyMilliseconds() const noexcept
    {
        return baseDelayMilliseconds;
    }

    ScannerVibrato::ModeCalibration ScannerVibrato::calibration (
        ScannerMode mode,
        ScannerCharacter character) noexcept
    {
        const auto deep = character == ScannerCharacter::deep;
        const auto rate = deep ? 6.82f : 6.48f;
        const auto depthScale = deep ? 1.12f : 0.88f;

        switch (mode)
        {
            case ScannerMode::vibrato1:
                return { 0.34f * depthScale, 1.0f, rate };
            case ScannerMode::vibrato2:
                return { 0.67f * depthScale, 1.0f, rate };
            case ScannerMode::vibrato3:
                return { 1.04f * depthScale, 1.0f, rate };
            case ScannerMode::chorus1:
                return { 0.38f * depthScale, 0.38f, rate };
            case ScannerMode::chorus2:
                return { 0.72f * depthScale, 0.49f, rate };
            case ScannerMode::chorus3:
                return { 1.08f * depthScale, 0.61f, rate };
            case ScannerMode::bypass:
            default:
                return { 0.0f, 0.0f, rate };
        }
    }

    std::uint32_t ScannerVibrato::nextNoise (
        std::uint32_t& state) noexcept
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    }

    float ScannerVibrato::scannerSample (
        const dsp::FixedFractionalDelay<delayCapacity>& line,
        float phase,
        float excursionSamples) const noexcept
    {
        constexpr auto tapCount =
            static_cast<float> (scannerTapShape.size());
        const auto scannerPosition =
            phase * tapCount;
        const auto firstIndex =
            static_cast<std::size_t> (scannerPosition)
            % scannerTapShape.size();
        const auto secondIndex =
            (firstIndex + 1) % scannerTapShape.size();
        const auto fraction =
            scannerPosition
            - static_cast<float> (
                static_cast<std::size_t> (scannerPosition));

        // Equal-power moving contact between adjacent phase-line taps. The
        // quarter-wave polynomial is accurate to better than 2e-4 here and
        // avoids four transcendental calls per stereo sample.
        const auto fadeAngle = fraction * (0.5f * dsp::pi);
        const auto firstWeight = sineQuarterWave (
            0.5f * dsp::pi - fadeAngle);
        const auto secondWeight = sineQuarterWave (fadeAngle);
        const auto normalisation =
            1.0f / (firstWeight + secondWeight + 1.0e-12f);
        const auto baseSamples =
            baseDelayMilliseconds
            * 0.001f
            * static_cast<float> (spec_.sampleRate);
        const auto delayA =
            baseSamples
            + excursionSamples * scannerTapShape[firstIndex];
        const auto delayB =
            baseSamples
            + excursionSamples * scannerTapShape[secondIndex];

        return (firstWeight * line.read (delayA)
                + secondWeight * line.read (delayB))
            * normalisation;
    }

    void ScannerVibrato::updateTargets() noexcept
    {
        const auto mode = calibration (
            parameters_.mode,
            parameters_.character);
        const auto worship = parameters_.worshipIntensity;
        const auto excursionMilliseconds =
            mode.delayExcursionMilliseconds
            * parameters_.depth
            * (1.0f + 0.10f * worship);
        const auto wet =
            mode.wetMix
            * parameters_.mix
            * (1.0f - 0.06f * worship);
        const auto spread =
            parameters_.stereoSpread
            * (0.038f + 0.024f * worship);
        const auto age =
            dsp::clamp (
                parameters_.age + 0.12f * worship,
                0.0f,
                1.0f);
        const auto cutoff =
            15800.0f
            - 6100.0f * age
            - (parameters_.character == ScannerCharacter::deep
                   ? 900.0f
                   : 0.0f);

        rateHz_.setTarget (
            mode.rateHz
            * parameters_.rate
            * (1.0f - 0.045f * worship));
        excursionSamples_.setTarget (
            excursionMilliseconds
            * 0.001f
            * static_cast<float> (spec_.sampleRate));
        wetMix_.setTarget (dsp::clamp (wet, 0.0f, 1.0f));
        spreadPhase_.setTarget (spread);
        age_.setTarget (age);
        noiseGain_.setTarget (
            parameters_.noise
            * (0.00018f + 0.00008f * age));
        output_.setTarget (parameters_.output);
        bandwidthStep_.setTarget (
            dsp::onePoleStepForCutoff (spec_.sampleRate, cutoff));
    }
}
