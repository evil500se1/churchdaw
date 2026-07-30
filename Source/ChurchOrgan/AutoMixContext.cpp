#include "AutoMixContext.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 8000.0;
        constexpr double maximumSampleRate = 384000.0;

        bool sourceEnabled (
            std::uint32_t mask,
            AutoMixSidechain source) noexcept
        {
            return (mask & static_cast<std::uint32_t> (source)) != 0;
        }
    }

    bool AutoMixContext::prepare (
        const AutoMixPrepareSpec& requested) noexcept
    {
        if (! std::isfinite (requested.sampleRate)
            || requested.sampleRate < minimumSampleRate
            || requested.sampleRate > maximumSampleRate
            || requested.maximumBlockSize == 0)
        {
            prepared_ = false;
            return false;
        }

        spec_ = requested;
        prepared_ = true;
        updateTimeConstants();
        reset();
        return true;
    }

    void AutoMixContext::reset() noexcept
    {
        smoothedControls_ = {};
        gains_ = {};
    }

    void AutoMixContext::setParameters (
        const AutoMixParameters& requested) noexcept
    {
        parameters_ = requested;
        parameters_.amount = worshipdsp::clamp (
            worshipdsp::finiteOr (requested.amount, 0.32f),
            0.0f,
            1.0f);
        parameters_.reactionMilliseconds = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.reactionMilliseconds,
                720.0f),
            250.0f,
            5000.0f);
        parameters_.recoveryMilliseconds = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.recoveryMilliseconds,
                1900.0f),
            800.0f,
            10000.0f);
        parameters_.maximumAttenuationDecibels =
            worshipdsp::clamp (
                worshipdsp::finiteOr (
                    requested.maximumAttenuationDecibels,
                    3.0f),
                0.0f,
                6.0f);
        parameters_.pianoConflict = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.pianoConflict,
                0.72f),
            0.0f,
            1.0f);
        parameters_.externalPadConflict = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.externalPadConflict,
                0.86f),
            0.0f,
            1.0f);
        parameters_.externalStringsConflict =
            worshipdsp::clamp (
                worshipdsp::finiteOr (
                    requested.externalStringsConflict,
                    0.58f),
                0.0f,
                1.0f);
        parameters_.masterProtection = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.masterProtection,
                0.90f),
            0.0f,
            1.0f);
        parameters_.lowMidZone = worshipdsp::clamp (
            worshipdsp::finiteOr (requested.lowMidZone, 0.65f),
            0.0f,
            1.0f);
        parameters_.midZone = worshipdsp::clamp (
            worshipdsp::finiteOr (requested.midZone, 0.72f),
            0.0f,
            1.0f);
        parameters_.soloRoomLiftDecibels =
            worshipdsp::clamp (
                worshipdsp::finiteOr (
                    requested.soloRoomLiftDecibels,
                    0.45f),
                0.0f,
                1.0f);

        if (prepared_)
            updateTimeConstants();
    }

    AutoMixParameters AutoMixContext::parameters() const noexcept
    {
        return parameters_;
    }

    void AutoMixContext::setControlFrame (
        const AutoMixControlFrame& controls) noexcept
    {
        requestedControls_.pianoEnvelope =
            sanitiseEnvelope (controls.pianoEnvelope);
        requestedControls_.externalPadEnvelope =
            sanitiseEnvelope (controls.externalPadEnvelope);
        requestedControls_.externalStringsEnvelope =
            sanitiseEnvelope (controls.externalStringsEnvelope);
        requestedControls_.masterProximity =
            sanitiseEnvelope (controls.masterProximity);
    }

    AutoMixControlFrame AutoMixContext::controlFrame() const noexcept
    {
        return requestedControls_;
    }

    AutoMixGains AutoMixContext::advance (
        std::size_t numSamples) noexcept
    {
        if (! prepared_ || numSamples == 0)
            return gains_;

        const auto seconds =
            static_cast<double> (numSamples) / spec_.sampleRate;
        const auto controlMultiplier = static_cast<float> (
            std::exp (
                -seconds
                / static_cast<double> (
                    std::max (0.25f, reactionSeconds_))));

        auto smoothControl = [controlMultiplier] (
                                 float current,
                                 float target) noexcept
        {
            return target
                + (current - target) * controlMultiplier;
        };

        smoothedControls_.pianoEnvelope = smoothControl (
            smoothedControls_.pianoEnvelope,
            requestedControls_.pianoEnvelope);
        smoothedControls_.externalPadEnvelope = smoothControl (
            smoothedControls_.externalPadEnvelope,
            requestedControls_.externalPadEnvelope);
        smoothedControls_.externalStringsEnvelope = smoothControl (
            smoothedControls_.externalStringsEnvelope,
            requestedControls_.externalStringsEnvelope);
        smoothedControls_.masterProximity = smoothControl (
            smoothedControls_.masterProximity,
            requestedControls_.masterProximity);

        const auto attackMultiplier = static_cast<float> (
            std::exp (
                -seconds
                / static_cast<double> (reactionSeconds_)));
        const auto recoveryMultiplier = static_cast<float> (
            std::exp (
                -seconds
                / static_cast<double> (recoverySeconds_)));
        const auto targets = targetGains();

        gains_.worshipLayer = approach (
            gains_.worshipLayer,
            targets.worshipLayer,
            attackMultiplier,
            recoveryMultiplier);
        gains_.organLowMid = approach (
            gains_.organLowMid,
            targets.organLowMid,
            attackMultiplier,
            recoveryMultiplier);
        gains_.organMid = approach (
            gains_.organMid,
            targets.organMid,
            attackMultiplier,
            recoveryMultiplier);
        gains_.drive = approach (
            gains_.drive,
            targets.drive,
            attackMultiplier,
            recoveryMultiplier);
        gains_.output = approach (
            gains_.output,
            targets.output,
            attackMultiplier,
            recoveryMultiplier);

        // Room moves upward when alone and downward when a source returns.
        gains_.room = approach (
            gains_.room,
            targets.room,
            attackMultiplier,
            recoveryMultiplier);
        return gains_;
    }

    AutoMixGains AutoMixContext::currentGains() const noexcept
    {
        return gains_;
    }

    float AutoMixContext::sanitiseEnvelope (float value) noexcept
    {
        return worshipdsp::clamp (
            worshipdsp::finiteOr (value, 0.0f),
            0.0f,
            1.0f);
    }

    float AutoMixContext::approach (
        float current,
        float target,
        float attackMultiplier,
        float recoveryMultiplier) noexcept
    {
        const auto multiplier =
            target < current
                ? attackMultiplier
                : recoveryMultiplier;
        return target + (current - target) * multiplier;
    }

    void AutoMixContext::updateTimeConstants() noexcept
    {
        reactionSeconds_ =
            parameters_.reactionMilliseconds * 0.001f;
        recoverySeconds_ =
            std::max (
                parameters_.recoveryMilliseconds * 0.001f,
                reactionSeconds_ * 1.35f);
    }

    AutoMixGains AutoMixContext::targetGains() const noexcept
    {
        if (! parameters_.enabled || parameters_.amount <= 0.0f)
            return {};

        const auto mask = parameters_.sidechainMask;
        const auto piano = sourceEnabled (mask, autoMixPiano)
            ? worshipdsp::smoothStep (
                0.12f,
                0.92f,
                smoothedControls_.pianoEnvelope)
            : 0.0f;
        const auto pad = sourceEnabled (mask, autoMixExternalPad)
            ? worshipdsp::smoothStep (
                0.10f,
                0.88f,
                smoothedControls_.externalPadEnvelope)
            : 0.0f;
        const auto strings =
            sourceEnabled (mask, autoMixExternalStrings)
                ? worshipdsp::smoothStep (
                    0.14f,
                    0.90f,
                    smoothedControls_.externalStringsEnvelope)
                : 0.0f;
        const auto master =
            sourceEnabled (mask, autoMixMasterProximity)
                ? worshipdsp::smoothStep (
                    0.68f,
                    1.0f,
                    smoothedControls_.masterProximity)
                : 0.0f;
        const auto amount = parameters_.amount;
        const auto maximum =
            parameters_.maximumAttenuationDecibels;

        const auto layerConflict = worshipdsp::clamp (
            pad * parameters_.externalPadConflict
                + 0.32f
                    * strings
                    * parameters_.externalStringsConflict
                + 0.12f
                    * piano
                    * parameters_.pianoConflict,
            0.0f,
            1.0f);
        const auto lowMidConflict = worshipdsp::clamp (
            parameters_.lowMidZone
                * (0.60f * pad + 0.68f * strings + 0.20f * piano),
            0.0f,
            1.0f);
        const auto midConflict = worshipdsp::clamp (
            parameters_.midZone
                * (0.86f * piano + 0.28f * strings + 0.15f * pad),
            0.0f,
            1.0f);
        const auto masterConflict =
            master * parameters_.masterProtection;

        const auto gainFor = [amount, maximum] (
                                 float conflict,
                                 float scale) noexcept
        {
            const auto attenuation = std::min (
                maximum,
                maximum
                    * amount
                    * worshipdsp::clamp (
                        conflict * scale,
                        0.0f,
                        1.0f));
            return worshipdsp::decibelsToGain (-attenuation);
        };

        const auto strongestSource =
            std::max (piano, std::max (pad, strings));
        const auto alone =
            (1.0f - strongestSource) * (1.0f - master);
        const auto roomLift =
            parameters_.soloRoomLiftDecibels
            * amount
            * alone;

        return {
            gainFor (layerConflict, 1.0f),
            gainFor (lowMidConflict, 0.70f),
            gainFor (midConflict, 0.72f),
            gainFor (masterConflict, 0.54f),
            gainFor (masterConflict, 1.0f),
            worshipdsp::decibelsToGain (roomLift)
        };
    }
}
