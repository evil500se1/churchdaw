#include "WorshipLayerEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 8000.0;
        constexpr double maximumSampleRate = 384000.0;
        constexpr float centreGain = 0.7071067811865475f;

        float midiFrequency (int note) noexcept
        {
            return 440.0f
                * std::pow (
                    2.0f,
                    (static_cast<float> (note) - 69.0f)
                        / 12.0f);
        }

        float normalisedNoise (std::uint32_t value) noexcept
        {
            return static_cast<float> (value)
                    * (1.0f / 2147483648.0f)
                - 1.0f;
        }
    }

    WorshipLayerEngine::WorshipLayerEngine() noexcept
    {
        for (std::size_t index = 0;
             index < worshipLayerCount;
             ++index)
        {
            layerParameters_[index] = defaultParameters (
                static_cast<WorshipLayer> (index));
        }
    }

    bool WorshipLayerEngine::prepare (
        const WorshipPrepareSpec& requested) noexcept
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
        if (spec_.quality != WorshipQuality::eco
            && spec_.quality != WorshipQuality::standard
            && spec_.quality != WorshipQuality::ultra)
        {
            spec_.quality = WorshipQuality::standard;
        }
        if (spec_.deterministicSeed == 0)
            spec_.deterministicSeed =
                0x574f52534849504cULL;

        voiceLimit_ = limitForQuality (spec_.quality);
        density_ = densityForQuality (spec_.quality);

        const auto detuneCents =
            spec_.quality == WorshipQuality::eco
                ? 1.15f
                : (spec_.quality == WorshipQuality::standard
                       ? 2.35f
                       : 3.65f);
        const auto detuneRatio =
            std::pow (2.0f, detuneCents / 1200.0f);
        const auto safeNyquist =
            static_cast<float> (spec_.sampleRate * 0.45);

        for (std::size_t index = 0;
             index < phaseTableSize;
             ++index)
        {
            const auto phase =
                worshipdsp::twoPi
                * static_cast<float> (index)
                / static_cast<float> (phaseTableSize);
            phaseSine_[index] = std::sin (phase);
            phaseCosine_[index] = std::cos (phase);
        }

        for (std::size_t note = 0;
             note < primaryRotation_.size();
             ++note)
        {
            const auto frequency =
                midiFrequency (static_cast<int> (note));
            audibleNote_[note] = frequency <= safeNyquist;
            harmonic2Allowed_[note] =
                frequency * 2.0f <= safeNyquist;
            harmonic3Allowed_[note] =
                frequency * 3.0f <= safeNyquist;

            const auto primaryAngle =
                audibleNote_[note]
                    ? worshipdsp::twoPi
                        * frequency
                        / static_cast<float> (spec_.sampleRate)
                    : 0.0f;
            const auto detunedAngle =
                audibleNote_[note]
                    ? worshipdsp::twoPi
                        * frequency
                        * detuneRatio
                        / static_cast<float> (spec_.sampleRate)
                    : 0.0f;
            primaryRotation_[note] = {
                std::sin (primaryAngle),
                std::cos (primaryAngle)
            };
            detunedRotation_[note] = {
                std::sin (detunedAngle),
                std::cos (detunedAngle)
            };
        }

        const auto profile = intensityProfile (targetIntensity_);
        for (std::size_t index = 0;
             index < worshipLayerCount;
             ++index)
        {
            const auto& parameters = layerParameters_[index];
            auto& runtime = layerRuntime_[index];

            runtime.level.prepare (
                spec_.sampleRate,
                34.0f,
                parameters.enabled ? parameters.level : 0.0f);
            runtime.intensityGain.prepare (
                spec_.sampleRate,
                95.0f,
                profile.layerGain[index]);
            runtime.width.prepare (
                spec_.sampleRate,
                90.0f,
                parameters.stereoWidth);
            runtime.lowPassStep.prepare (
                spec_.sampleRate,
                85.0f,
                worshipdsp::filterStep (
                    spec_.sampleRate,
                    parameters.lowPassHz));
            runtime.highPassStep.prepare (
                spec_.sampleRate,
                85.0f,
                worshipdsp::filterStep (
                    spec_.sampleRate,
                    parameters.highPassHz));
            runtime.reverbSend.prepare (
                spec_.sampleRate,
                110.0f,
                parameters.reverbSend);
        }

        intensity_.prepare (
            spec_.sampleRate, 120.0f, targetIntensity_);
        loudnessCompensation_.prepare (
            spec_.sampleRate,
            140.0f,
            profile.loudnessCompensation);
        expression_.prepare (
            spec_.sampleRate, 18.0f, 1.0f);
        outputGain_.prepare (
            spec_.sampleRate, 45.0f, targetOutputGain_);
        externalDucking_.prepare (
            spec_.sampleRate,
            160.0f,
            targetExternalDucking_);
        autoMixLayerGain_.prepare (
            spec_.sampleRate,
            240.0f,
            targetAutoMixLayerGain_);

        prepared_ = true;
        updateAllTargets();
        reset();

        intensity_.reset (intensity_.target());
        loudnessCompensation_.reset (
            loudnessCompensation_.target());
        outputGain_.reset (outputGain_.target());
        for (auto& runtime : layerRuntime_)
        {
            runtime.level.reset (runtime.level.target());
            runtime.intensityGain.reset (
                runtime.intensityGain.target());
            runtime.width.reset (runtime.width.target());
            runtime.lowPassStep.reset (
                runtime.lowPassStep.target());
            runtime.highPassStep.reset (
                runtime.highPassStep.target());
            runtime.reverbSend.reset (
                runtime.reverbSend.target());
        }
        return true;
    }

    void WorshipLayerEngine::reset() noexcept
    {
        voices_.fill ({});
        events_.fill ({});
        eventCount_ = 0;
        activationSerial_ = 0;
        renderedSamples_ = 0;
        droppedEvents_ = 0;
        stolenVoices_ = 0;
        diagnostics_ = {};

        expression_.reset (1.0f);
        externalDucking_.reset (
            externalDucking_.target());
        autoMixLayerGain_.reset (
            autoMixLayerGain_.target());

        for (auto& runtime : layerRuntime_)
        {
            runtime.leftLowPass = 0.0f;
            runtime.rightLowPass = 0.0f;
            runtime.leftHighPassMemory = 0.0f;
            runtime.rightHighPassMemory = 0.0f;
        }
    }

    void WorshipLayerEngine::setLayerParameters (
        WorshipLayer layer,
        const WorshipLayerParameters& requested) noexcept
    {
        const auto index = worshipLayerIndex (layer);
        if (index >= worshipLayerCount)
            return;

        auto parameters = requested;
        parameters.level = worshipdsp::clamp (
            worshipdsp::finiteOr (requested.level, 0.0f),
            0.0f,
            1.2f);
        parameters.attackMilliseconds = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.attackMilliseconds,
                250.0f),
            0.5f,
            10000.0f);
        parameters.releaseMilliseconds = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.releaseMilliseconds,
                1600.0f),
            5.0f,
            30000.0f);
        if (parameters.lowestNote > parameters.highestNote)
            std::swap (
                parameters.lowestNote,
                parameters.highestNote);
        parameters.velocityResponse = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.velocityResponse,
                0.0f),
            0.0f,
            1.0f);
        parameters.expressionResponse = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.expressionResponse,
                0.8f),
            0.0f,
            1.0f);
        parameters.stereoWidth = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.stereoWidth,
                0.5f),
            0.0f,
            1.0f);
        parameters.highPassHz = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.highPassHz,
                120.0f),
            10.0f,
            16000.0f);
        parameters.lowPassHz = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.lowPassHz,
                12000.0f),
            100.0f,
            22000.0f);
        if (parameters.highPassHz
            > parameters.lowPassHz * 0.88f)
        {
            parameters.highPassHz =
                parameters.lowPassHz * 0.88f;
        }
        parameters.ducking = worshipdsp::clamp (
            worshipdsp::finiteOr (requested.ducking, 0.7f),
            0.0f,
            1.0f);
        parameters.reverbSend = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requested.reverbSend,
                0.35f),
            0.0f,
            1.0f);

        layerParameters_[index] = parameters;
        if (prepared_)
            updateLayerTargets (index);
    }

    WorshipLayerParameters WorshipLayerEngine::layerParameters (
        WorshipLayer layer) const noexcept
    {
        const auto index = worshipLayerIndex (layer);
        return index < worshipLayerCount
            ? layerParameters_[index]
            : WorshipLayerParameters {};
    }

    WorshipLayerParameters WorshipLayerEngine::defaultParameters (
        WorshipLayer layer) noexcept
    {
        switch (layer)
        {
            case WorshipLayer::pad:
                return {
                    true, 0.46f, 420.0f, 2400.0f,
                    24, 112, 0.14f, 0.84f, 0.58f,
                    105.0f, 7800.0f, 0.72f, 0.36f
                };
            case WorshipLayer::air:
                return {
                    true, 0.16f, 680.0f, 3100.0f,
                    36, 127, 0.04f, 0.92f, 0.88f,
                    1750.0f, 16500.0f, 0.86f, 0.64f
                };
            case WorshipLayer::strings:
                return {
                    true, 0.29f, 285.0f, 1850.0f,
                    36, 112, 0.24f, 0.78f, 0.72f,
                    165.0f, 10600.0f, 0.66f, 0.40f
                };
            case WorshipLayer::shimmer:
                return {
                    true, 0.12f, 930.0f, 4100.0f,
                    48, 127, 0.02f, 0.96f, 0.92f,
                    620.0f, 17200.0f, 0.92f, 0.80f
                };
            case WorshipLayer::octave:
                return {
                    true, 0.18f, 190.0f, 1250.0f,
                    36, 100, 0.12f, 0.86f, 0.38f,
                    135.0f, 9200.0f, 0.58f, 0.30f
                };
            case WorshipLayer::texture:
                return {
                    true, 0.105f, 1250.0f, 4600.0f,
                    24, 127, 0.0f, 0.72f, 1.0f,
                    480.0f, 6800.0f, 1.0f, 0.68f
                };
            case WorshipLayer::reverseBloom:
            default:
                return {
                    true, 0.17f, 1080.0f, 2900.0f,
                    36, 112, 0.08f, 0.90f, 0.82f,
                    185.0f, 11600.0f, 0.82f, 0.58f
                };
        }
    }

    void WorshipLayerEngine::setWorshipIntensity (
        float intensity) noexcept
    {
        targetIntensity_ = worshipdsp::clamp (
            worshipdsp::finiteOr (intensity, 0.35f),
            0.0f,
            1.0f);
        if (prepared_)
            updateAllTargets();
    }

    float WorshipLayerEngine::targetWorshipIntensity() const noexcept
    {
        return targetIntensity_;
    }

    WorshipIntensityProfile WorshipLayerEngine::intensityProfile (
        float requestedIntensity) noexcept
    {
        const auto intensity = worshipdsp::clamp (
            worshipdsp::finiteOr (
                requestedIntensity,
                0.0f),
            0.0f,
            1.0f);
        WorshipIntensityProfile profile;

        profile.layerGain[worshipLayerIndex (WorshipLayer::pad)] =
            0.025f
            + 0.975f
                * worshipdsp::smoothStep (
                    0.12f, 0.64f, intensity);
        profile.layerGain[worshipLayerIndex (WorshipLayer::air)] =
            0.006f
            + 0.994f
                * worshipdsp::smoothStep (
                    0.49f, 0.88f, intensity);
        profile.layerGain[worshipLayerIndex (WorshipLayer::strings)] =
            0.006f
            + 0.994f
                * worshipdsp::smoothStep (
                    0.46f, 0.84f, intensity);
        profile.layerGain[worshipLayerIndex (WorshipLayer::shimmer)] =
            worshipdsp::smoothStep (
                0.76f, 1.0f, intensity);
        profile.layerGain[worshipLayerIndex (WorshipLayer::octave)] =
            0.018f
            + 0.78f
                * worshipdsp::smoothStep (
                    0.25f, 0.73f, intensity);
        profile.layerGain[worshipLayerIndex (WorshipLayer::texture)] =
            0.004f
            + 0.82f
                * worshipdsp::smoothStep (
                    0.34f, 0.88f, intensity);
        profile.layerGain[
            worshipLayerIndex (WorshipLayer::reverseBloom)] =
            0.006f
            + 0.88f
                * worshipdsp::smoothStep (
                    0.31f, 0.79f, intensity);

        profile.spectralOpening =
            0.18f
            + 0.82f
                * worshipdsp::smoothStep (
                    0.10f, 0.91f, intensity);
        profile.stereoExpansion =
            0.10f
            + 0.90f
                * worshipdsp::smoothStep (
                    0.43f, 1.0f, intensity);
        profile.reverbExpansion =
            0.08f
            + 0.92f
                * worshipdsp::smoothStep (
                    0.21f, 0.91f, intensity);
        profile.loudnessCompensation =
            1.0f
            / std::sqrt (
                1.0f
                + 0.38f * intensity
                + 0.22f * intensity * intensity);
        profile.perceptualEnergyTarget =
            (0.05f + 0.95f * std::sqrt (intensity))
            * profile.loudnessCompensation;
        return profile;
    }

    void WorshipLayerEngine::setOutputGain (float gain) noexcept
    {
        targetOutputGain_ = worshipdsp::clamp (
            worshipdsp::finiteOr (gain, 0.72f),
            0.0f,
            1.0f);
        if (prepared_)
            outputGain_.setTarget (targetOutputGain_);
    }

    void WorshipLayerEngine::setExternalDucking (
        float amount) noexcept
    {
        targetExternalDucking_ = worshipdsp::clamp (
            worshipdsp::finiteOr (amount, 0.0f),
            0.0f,
            1.0f);
        if (prepared_)
            externalDucking_.setTarget (
                targetExternalDucking_);
    }

    void WorshipLayerEngine::setAutoMixLayerGain (
        float linearGain) noexcept
    {
        targetAutoMixLayerGain_ = worshipdsp::clamp (
            worshipdsp::finiteOr (linearGain, 1.0f),
            0.50f,
            1.05f);
        if (prepared_)
            autoMixLayerGain_.setTarget (
                targetAutoMixLayerGain_);
    }

    bool WorshipLayerEngine::queueNoteOn (
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (note < 0 || note > 127
            || velocity < 0 || velocity > 127)
        {
            return false;
        }
        if (velocity == 0)
            return queueNoteOff (note, sampleOffset);

        return queueEvent ({
            WorshipEventType::noteOn,
            static_cast<std::uint8_t> (note),
            static_cast<std::uint8_t> (velocity),
            0.0f,
            sampleOffset
        });
    }

    bool WorshipLayerEngine::queueNoteOff (
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (note < 0 || note > 127)
            return false;
        return queueEvent ({
            WorshipEventType::noteOff,
            static_cast<std::uint8_t> (note),
            0,
            0.0f,
            sampleOffset
        });
    }

    bool WorshipLayerEngine::queueAllNotesOff (
        std::uint32_t sampleOffset) noexcept
    {
        return queueEvent ({
            WorshipEventType::allNotesOff,
            0,
            0,
            0.0f,
            sampleOffset
        });
    }

    bool WorshipLayerEngine::queueExpression (
        float expression,
        std::uint32_t sampleOffset) noexcept
    {
        if (! std::isfinite (expression))
            return false;
        return queueEvent ({
            WorshipEventType::expression,
            0,
            0,
            worshipdsp::clamp (expression, 0.0f, 1.0f),
            sampleOffset
        });
    }

    void WorshipLayerEngine::renderAdd (
        const WorshipAudioBlock& block) noexcept
    {
        if (! prepared_
            || block.left == nullptr
            || block.numSamples == 0)
        {
            return;
        }

        const auto stereo =
            block.right != nullptr
            && block.right != block.left;
        const auto stereoSend =
            block.reverbSendLeft != nullptr
            && block.reverbSendRight != nullptr
            && block.reverbSendRight
                != block.reverbSendLeft;
        const auto voiceScale =
            0.105f * density_;
        auto eventIndex = std::size_t { 0 };
        auto layerPeak = 0.0f;
        auto reverbPeak = 0.0f;

        for (std::size_t sample = 0;
             sample < block.numSamples;
             ++sample)
        {
            while (eventIndex < eventCount_
                   && events_[eventIndex].sampleOffset
                       <= sample)
            {
                applyEvent (events_[eventIndex]);
                ++eventIndex;
            }

            const auto currentIntensity = intensity_.next();
            const auto loudness =
                loudnessCompensation_.next();
            const auto expression = expression_.next();
            const auto externalDucking =
                externalDucking_.next();
            const auto autoMixGain =
                autoMixLayerGain_.next();
            const auto masterOutput =
                outputGain_.next() * loudness;

            std::array<float, worshipLayerCount> rawLeft {};
            std::array<float, worshipLayerCount> rawRight {};
            std::array<float, worshipLayerCount> layerGain {};
            std::array<float, worshipLayerCount> layerWidth {};
            std::array<float, worshipLayerCount> lowPassStep {};
            std::array<float, worshipLayerCount> highPassStep {};
            std::array<float, worshipLayerCount> sendGain {};

            for (std::size_t layer = 0;
                 layer < worshipLayerCount;
                 ++layer)
            {
                auto& runtime = layerRuntime_[layer];
                const auto& parameters =
                    layerParameters_[layer];
                const auto duckGain =
                    1.0f
                    - 0.36f
                        * externalDucking
                        * parameters.ducking;
                layerGain[layer] =
                    runtime.level.next()
                    * runtime.intensityGain.next()
                    * duckGain
                    * autoMixGain;
                layerWidth[layer] = runtime.width.next();
                lowPassStep[layer] =
                    runtime.lowPassStep.next();
                highPassStep[layer] =
                    runtime.highPassStep.next();
                sendGain[layer] =
                    runtime.reverbSend.next();
            }

            for (std::size_t voiceIndex = 0;
                 voiceIndex < voiceLimit_;
                 ++voiceIndex)
            {
                auto& voice = voices_[voiceIndex];
                if (! voice.active)
                    continue;

                const auto noiseLeft =
                    normalisedNoise (
                        nextNoise (voice.noiseState));
                const auto noiseRight =
                    normalisedNoise (
                        nextNoise (voice.noiseState));
                const auto velocity =
                    static_cast<float> (voice.velocity)
                    * (1.0f / 127.0f);

                for (std::size_t layer = 0;
                     layer < worshipLayerCount;
                     ++layer)
                {
                    auto& envelope = voice.envelopes[layer];
                    const auto target =
                        envelopeTarget (voice, layer);
                    const auto envelopeStep =
                        target > envelope
                            ? layerRuntime_[layer].attackStep
                            : layerRuntime_[layer].releaseStep;
                    envelope +=
                        envelopeStep * (target - envelope);

                    if (envelope < 1.0e-7f
                        && target == 0.0f)
                    {
                        envelope = 0.0f;
                        continue;
                    }

                    const auto& parameters =
                        layerParameters_[layer];
                    const auto velocityGain =
                        1.0f
                        - parameters.velocityResponse
                            * (1.0f - velocity);
                    const auto expressionGain =
                        1.0f
                        - parameters.expressionResponse
                            * (1.0f - expression);
                    const auto component = componentSample (
                        static_cast<WorshipLayer> (layer),
                        voice,
                        noiseLeft,
                        noiseRight,
                        envelope,
                        layerWidth[layer]);
                    const auto gain =
                        envelope
                        * velocityGain
                        * expressionGain
                        * voiceScale;
                    rawLeft[layer] += component.left * gain;
                    rawRight[layer] += component.right * gain;
                }

                voice.primary.advance();
                if (spec_.quality != WorshipQuality::eco)
                    voice.detuned.advance();
                ++voice.ageSamples;
                ++voice.stabiliseCounter;
                if ((voice.stabiliseCounter & 2047u) == 0u)
                {
                    voice.primary.stabilise();
                    if (spec_.quality != WorshipQuality::eco)
                        voice.detuned.stabilise();
                }

                if (voiceFinished (voice))
                    voice.active = false;
            }

            auto mixedLeft = 0.0f;
            auto mixedRight = 0.0f;
            auto sendLeft = 0.0f;
            auto sendRight = 0.0f;

            for (std::size_t layer = 0;
                 layer < worshipLayerCount;
                 ++layer)
            {
                auto& runtime = layerRuntime_[layer];
                runtime.leftLowPass +=
                    lowPassStep[layer]
                    * (rawLeft[layer]
                       - runtime.leftLowPass);
                runtime.rightLowPass +=
                    lowPassStep[layer]
                    * (rawRight[layer]
                       - runtime.rightLowPass);
                runtime.leftHighPassMemory +=
                    highPassStep[layer]
                    * (runtime.leftLowPass
                       - runtime.leftHighPassMemory);
                runtime.rightHighPassMemory +=
                    highPassStep[layer]
                    * (runtime.rightLowPass
                       - runtime.rightHighPassMemory);

                if (std::abs (runtime.leftLowPass) < 1.0e-20f)
                    runtime.leftLowPass = 0.0f;
                if (std::abs (runtime.rightLowPass) < 1.0e-20f)
                    runtime.rightLowPass = 0.0f;
                if (std::abs (runtime.leftHighPassMemory) < 1.0e-20f)
                    runtime.leftHighPassMemory = 0.0f;
                if (std::abs (runtime.rightHighPassMemory) < 1.0e-20f)
                    runtime.rightHighPassMemory = 0.0f;

                const auto filteredLeft =
                    (runtime.leftLowPass
                     - runtime.leftHighPassMemory)
                    * layerGain[layer];
                const auto filteredRight =
                    (runtime.rightLowPass
                     - runtime.rightHighPassMemory)
                    * layerGain[layer];
                mixedLeft += filteredLeft;
                mixedRight += filteredRight;
                sendLeft += filteredLeft * sendGain[layer];
                sendRight += filteredRight * sendGain[layer];
            }

            auto layerLeft = worshipdsp::protectedSample (
                mixedLeft * masterOutput);
            auto layerRight = worshipdsp::protectedSample (
                mixedRight * masterOutput);
            auto reverbLeft = worshipdsp::protectedSample (
                sendLeft * masterOutput,
                0.34f,
                0.50f);
            auto reverbRight = worshipdsp::protectedSample (
                sendRight * masterOutput,
                0.34f,
                0.50f);

            if (! stereo)
            {
                layerLeft = worshipdsp::protectedSample (
                    centreGain * (layerLeft + layerRight));
                layerRight = layerLeft;
            }
            if (block.reverbSendLeft != nullptr && ! stereoSend)
            {
                reverbLeft = worshipdsp::protectedSample (
                    centreGain * (reverbLeft + reverbRight),
                    0.34f,
                    0.50f);
                reverbRight = reverbLeft;
            }

            block.left[sample] =
                worshipdsp::finiteOr (
                    block.left[sample],
                    0.0f)
                + layerLeft;
            if (stereo)
            {
                block.right[sample] =
                    worshipdsp::finiteOr (
                        block.right[sample],
                        0.0f)
                    + layerRight;
            }

            if (block.reverbSendLeft != nullptr)
            {
                block.reverbSendLeft[sample] =
                    worshipdsp::finiteOr (
                        block.reverbSendLeft[sample],
                        0.0f)
                    + reverbLeft;
                if (stereoSend)
                {
                    block.reverbSendRight[sample] =
                        worshipdsp::finiteOr (
                            block.reverbSendRight[sample],
                            0.0f)
                        + reverbRight;
                }
            }

            layerPeak = std::max (
                layerPeak,
                std::max (
                    std::abs (layerLeft),
                    std::abs (layerRight)));
            reverbPeak = std::max (
                reverbPeak,
                std::max (
                    std::abs (reverbLeft),
                    std::abs (reverbRight)));
            diagnostics_.currentIntensity =
                currentIntensity;
        }

        carryUnprocessedEvents (
            eventIndex,
            block.numSamples);
        renderedSamples_ += block.numSamples;
        publishDiagnostics (layerPeak, reverbPeak);
    }

    WorshipDiagnostics WorshipLayerEngine::diagnostics() const noexcept
    {
        return diagnostics_;
    }

    std::size_t WorshipLayerEngine::voiceLimit() const noexcept
    {
        return voiceLimit_;
    }

    void WorshipLayerEngine::Oscillator::advance() noexcept
    {
        const auto nextSine =
            sine * rotation.cosine
            + cosine * rotation.sine;
        const auto nextCosine =
            cosine * rotation.cosine
            - sine * rotation.sine;
        sine = nextSine;
        cosine = nextCosine;
    }

    void WorshipLayerEngine::Oscillator::setState (
        float sineValue,
        float cosineValue) noexcept
    {
        sine = worshipdsp::finiteOr (sineValue, 0.0f);
        cosine = worshipdsp::finiteOr (cosineValue, 1.0f);
    }

    void WorshipLayerEngine::Oscillator::stabilise() noexcept
    {
        const auto magnitudeSquared =
            sine * sine + cosine * cosine;
        const auto correction =
            1.5f - 0.5f * magnitudeSquared;
        sine *= correction;
        cosine *= correction;
    }

    std::size_t WorshipLayerEngine::limitForQuality (
        WorshipQuality quality) noexcept
    {
        switch (quality)
        {
            case WorshipQuality::eco:
                return 16;
            case WorshipQuality::ultra:
                return maximumWorshipVoices;
            case WorshipQuality::standard:
            default:
                return 32;
        }
    }

    float WorshipLayerEngine::densityForQuality (
        WorshipQuality quality) noexcept
    {
        switch (quality)
        {
            case WorshipQuality::eco:
                return 0.84f;
            case WorshipQuality::ultra:
                return 1.08f;
            case WorshipQuality::standard:
            default:
                return 1.0f;
        }
    }

    std::uint32_t WorshipLayerEngine::nextNoise (
        std::uint32_t& state) noexcept
    {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        if (state == 0)
            state = 0x9e3779b9u;
        return state;
    }

    std::uint64_t WorshipLayerEngine::hash (
        std::uint64_t value) noexcept
    {
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31u;
        return value;
    }

    bool WorshipLayerEngine::queueEvent (
        const WorshipEvent& event) noexcept
    {
        if (eventCount_ >= events_.size())
        {
            ++droppedEvents_;
            return false;
        }

        auto insertion = eventCount_;
        while (insertion > 0
               && events_[insertion - 1].sampleOffset
                   > event.sampleOffset)
        {
            events_[insertion] = events_[insertion - 1];
            --insertion;
        }
        events_[insertion] = event;
        ++eventCount_;
        return true;
    }

    void WorshipLayerEngine::applyEvent (
        const WorshipEvent& event) noexcept
    {
        switch (event.type)
        {
            case WorshipEventType::noteOn:
                activateNote (event.note, event.velocity);
                break;
            case WorshipEventType::noteOff:
                releaseNote (event.note);
                break;
            case WorshipEventType::allNotesOff:
                releaseAllNotes();
                break;
            case WorshipEventType::expression:
                expression_.setTarget (
                    worshipdsp::clamp (
                        worshipdsp::finiteOr (
                            event.value,
                            1.0f),
                        0.0f,
                        1.0f));
                break;
        }
    }

    void WorshipLayerEngine::activateNote (
        int note,
        int velocity) noexcept
    {
        const auto index = chooseVoice (note);
        auto& voice = voices_[index];

        if (voice.active
            && voice.note == static_cast<std::uint8_t> (note))
        {
            voice.held = true;
            voice.velocity =
                static_cast<std::uint8_t> (velocity);
            return;
        }

        if (voice.active)
            ++stolenVoices_;
        initialiseVoice (
            voice,
            index,
            note,
            velocity);
    }

    void WorshipLayerEngine::releaseNote (int note) noexcept
    {
        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            auto& voice = voices_[index];
            if (voice.active
                && voice.note
                    == static_cast<std::uint8_t> (note))
            {
                voice.held = false;
            }
        }
    }

    void WorshipLayerEngine::releaseAllNotes() noexcept
    {
        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            voices_[index].held = false;
        }
    }

    std::size_t WorshipLayerEngine::chooseVoice (
        int note) noexcept
    {
        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            if (voices_[index].active
                && voices_[index].note
                    == static_cast<std::uint8_t> (note))
            {
                return index;
            }
        }

        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            if (! voices_[index].active)
                return index;
        }

        auto selected = std::size_t { 0 };
        auto selectedScore =
            std::numeric_limits<float>::max();
        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            const auto& voice = voices_[index];
            auto energy = voice.held ? 2.0f : 0.0f;
            for (const auto envelope : voice.envelopes)
                energy += envelope;
            if (energy < selectedScore)
            {
                selectedScore = energy;
                selected = index;
            }
        }
        return selected;
    }

    void WorshipLayerEngine::initialiseVoice (
        Voice& voice,
        std::size_t voiceIndex,
        int note,
        int velocity) noexcept
    {
        voice = {};
        voice.active = true;
        voice.held = true;
        voice.note = static_cast<std::uint8_t> (note);
        voice.velocity =
            static_cast<std::uint8_t> (velocity);
        voice.harmonic2Allowed =
            harmonic2Allowed_[static_cast<std::size_t> (note)];
        voice.harmonic3Allowed =
            harmonic3Allowed_[static_cast<std::size_t> (note)];
        voice.primary.rotation =
            primaryRotation_[static_cast<std::size_t> (note)];
        voice.detuned.rotation =
            detunedRotation_[static_cast<std::size_t> (note)];

        const auto identity = hash (
            spec_.deterministicSeed
            ^ (static_cast<std::uint64_t> (note) << 32u)
            ^ (activationSerial_ << 7u)
            ^ static_cast<std::uint64_t> (voiceIndex));
        const auto secondIdentity = hash (
            identity ^ 0x9e3779b97f4a7c15ULL);
        const auto primaryPhaseIndex =
            static_cast<std::size_t> (identity)
            & (phaseTableSize - 1);
        const auto detunedPhaseIndex =
            static_cast<std::size_t> (secondIdentity)
            & (phaseTableSize - 1);
        voice.primary.setState (
            phaseSine_[primaryPhaseIndex],
            phaseCosine_[primaryPhaseIndex]);
        voice.detuned.setState (
            phaseSine_[detunedPhaseIndex],
            phaseCosine_[detunedPhaseIndex]);
        voice.noiseState =
            static_cast<std::uint32_t> (
                secondIdentity & 0xffffffffULL);
        if (voice.noiseState == 0)
            voice.noiseState = 1;
        voice.pan =
            normalisedNoise (
                static_cast<std::uint32_t> (identity));
        ++activationSerial_;
    }

    WorshipLayerEngine::StereoSample
    WorshipLayerEngine::componentSample (
        WorshipLayer layer,
        const Voice& voice,
        float noiseLeft,
        float noiseRight,
        float envelope,
        float width) const noexcept
    {
        if (! audibleNote_[voice.note])
            return {};

        const auto sineA = voice.primary.sine;
        const auto cosineA = voice.primary.cosine;
        const auto sineB =
            spec_.quality == WorshipQuality::eco
                ? sineA
                : voice.detuned.sine;
        const auto cosineB =
            spec_.quality == WorshipQuality::eco
                ? cosineA
                : voice.detuned.cosine;
        const auto harmonic2 =
            voice.harmonic2Allowed
                ? 2.0f * sineA * cosineA
                : 0.0f;
        const auto harmonic3 =
            voice.harmonic3Allowed
                ? 3.0f * sineA
                    - 4.0f * sineA * sineA * sineA
                : 0.0f;
        StereoSample result;

        switch (layer)
        {
            case WorshipLayer::pad:
                result.left = 0.67f * sineA + 0.33f * sineB;
                result.right = 0.67f * sineB + 0.33f * sineA;
                break;
            case WorshipLayer::air:
                result.left =
                    0.34f * noiseLeft + 0.055f * harmonic2;
                result.right =
                    0.34f * noiseRight + 0.055f * harmonic2;
                break;
            case WorshipLayer::strings:
                result.left =
                    0.58f * sineA
                    + 0.22f * harmonic2
                    + 0.12f * harmonic3
                    + 0.14f * sineB;
                result.right =
                    0.58f * sineB
                    + 0.22f * harmonic2
                    + 0.12f * harmonic3
                    + 0.14f * sineA;
                break;
            case WorshipLayer::shimmer:
                result.left =
                    0.70f * harmonic2
                    + 0.22f * harmonic3
                    + 0.08f * sineB;
                result.right =
                    0.70f * harmonic2
                    + 0.22f * harmonic3
                    + 0.08f * sineA;
                break;
            case WorshipLayer::octave:
                result.left =
                    harmonic2 + 0.08f * sineB;
                result.right =
                    harmonic2 + 0.08f * sineA;
                break;
            case WorshipLayer::texture:
            {
                // Difference-phase motion only. Using the raw oscillator
                // product would also create an unbounded sum-frequency term.
                const auto slowInteraction =
                    cosineA * cosineB + sineA * sineB;
                result.left =
                    0.22f * noiseLeft
                    + 0.20f * slowInteraction
                    + 0.08f * harmonic3;
                result.right =
                    0.22f * noiseRight
                    - 0.20f * slowInteraction
                    + 0.08f * harmonic3;
                break;
            }
            case WorshipLayer::reverseBloom:
            default:
            {
                const auto bloom =
                    0.16f
                    + 0.84f * envelope * envelope;
                result.left =
                    bloom
                    * (0.62f * sineA + 0.38f * sineB);
                result.right =
                    bloom
                    * (0.62f * sineB + 0.38f * sineA);
                break;
            }
        }

        const auto pan =
            voice.pan * width * 0.42f;
        result.left *= centreGain * (1.0f - pan);
        result.right *= centreGain * (1.0f + pan);
        return result;
    }

    float WorshipLayerEngine::envelopeTarget (
        const Voice& voice,
        std::size_t layerIndex) const noexcept
    {
        const auto& parameters =
            layerParameters_[layerIndex];
        return voice.held
                && parameters.enabled
                && voice.note >= parameters.lowestNote
                && voice.note <= parameters.highestNote
            ? 1.0f
            : 0.0f;
    }

    bool WorshipLayerEngine::voiceFinished (
        const Voice& voice) const noexcept
    {
        if (voice.held)
            return false;
        for (const auto envelope : voice.envelopes)
        {
            if (envelope >= 1.0e-5f)
                return false;
        }
        return true;
    }

    void WorshipLayerEngine::updateLayerTargets (
        std::size_t layerIndex) noexcept
    {
        const auto profile = intensityProfile (targetIntensity_);
        const auto& parameters =
            layerParameters_[layerIndex];
        auto& runtime = layerRuntime_[layerIndex];
        const auto effectiveLowPass =
            parameters.lowPassHz
            * (0.62f + 0.38f * profile.spectralOpening);
        const auto effectiveHighPass =
            parameters.highPassHz
            * (1.08f - 0.08f * profile.spectralOpening);

        runtime.level.setTarget (
            parameters.enabled ? parameters.level : 0.0f);
        runtime.intensityGain.setTarget (
            profile.layerGain[layerIndex]);
        runtime.width.setTarget (
            parameters.stereoWidth
            * (0.72f + 0.28f * profile.stereoExpansion));
        runtime.lowPassStep.setTarget (
            worshipdsp::filterStep (
                spec_.sampleRate,
                effectiveLowPass));
        runtime.highPassStep.setTarget (
            worshipdsp::filterStep (
                spec_.sampleRate,
                effectiveHighPass));
        runtime.reverbSend.setTarget (
            parameters.reverbSend
            * (0.70f + 0.30f * profile.reverbExpansion));
        runtime.attackStep = worshipdsp::onePoleStep (
            spec_.sampleRate,
            parameters.attackMilliseconds);
        runtime.releaseStep = worshipdsp::onePoleStep (
            spec_.sampleRate,
            parameters.releaseMilliseconds);
    }

    void WorshipLayerEngine::updateAllTargets() noexcept
    {
        const auto profile = intensityProfile (targetIntensity_);
        intensity_.setTarget (targetIntensity_);
        loudnessCompensation_.setTarget (
            profile.loudnessCompensation);
        outputGain_.setTarget (targetOutputGain_);
        for (std::size_t index = 0;
             index < worshipLayerCount;
             ++index)
        {
            updateLayerTargets (index);
        }
    }

    void WorshipLayerEngine::carryUnprocessedEvents (
        std::size_t firstUnprocessed,
        std::size_t renderedSamples) noexcept
    {
        const auto remaining =
            eventCount_ > firstUnprocessed
                ? eventCount_ - firstUnprocessed
                : 0;
        for (std::size_t index = 0;
             index < remaining;
             ++index)
        {
            events_[index] =
                events_[firstUnprocessed + index];
            events_[index].sampleOffset -=
                static_cast<std::uint32_t> (
                    std::min<std::size_t> (
                        events_[index].sampleOffset,
                        renderedSamples));
        }
        eventCount_ = remaining;
    }

    void WorshipLayerEngine::publishDiagnostics (
        float layerPeak,
        float reverbPeak) noexcept
    {
        auto held = std::uint32_t { 0 };
        auto tails = std::uint32_t { 0 };
        for (std::size_t index = 0;
             index < voiceLimit_;
             ++index)
        {
            if (! voices_[index].active)
                continue;
            if (voices_[index].held)
                ++held;
            else
                ++tails;
        }

        diagnostics_.renderedSamples = renderedSamples_;
        diagnostics_.droppedEvents = droppedEvents_;
        diagnostics_.stolenVoices = stolenVoices_;
        diagnostics_.heldVoices = held;
        diagnostics_.tailVoices = tails;
        diagnostics_.queuedEvents =
            static_cast<std::uint32_t> (eventCount_);
        diagnostics_.lastLayerPeak =
            worshipdsp::finiteOr (layerPeak, 0.0f);
        diagnostics_.lastReverbPeak =
            worshipdsp::finiteOr (reverbPeak, 0.0f);
    }
}
