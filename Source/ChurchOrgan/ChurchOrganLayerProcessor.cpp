#include "ChurchOrganLayerProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 32000.0;
        constexpr double maximumSampleRate = 192000.0;
        constexpr std::uint64_t defaultLayerSeed =
            0x4c415945524f5247ULL;
        constexpr std::uint64_t tonewheelSeedSalt =
            0x544f4e4557484545ULL;
        constexpr std::uint64_t transistorSeedSalt =
            0x5452414e53495354ULL;
        constexpr std::uint64_t pipeSeedSalt =
            0x504950454f524741ULL;
        constexpr std::uint64_t hybridSeedSalt =
            0x4859425249444f52ULL;

        constexpr std::array<float, transistorTabCount>
            defaultTransistorTabs {{
                0.00f, 0.88f, 0.62f, 0.50f,
                0.70f, 0.25f, 0.35f, 0.15f
            }};
        constexpr std::uint32_t transistorVibratoIntentVersion = 1;
        constexpr float legacyVibratoDepthTolerance = 1.0e-6f;

        constexpr std::array<float, pipeOrganManualCount>
            defaultPipeSwell {{ 1.0f, 0.86f, 1.0f }};
        constexpr std::array<float, pipeOrganManualCount>
            defaultPipeTremulantRates {{ 5.9f, 5.5f, 4.8f }};

        constexpr const char* stateType =
            "ChurchOrganLayerState";
        constexpr const char* generalStateType =
            "OrganParameters";
        constexpr const char* transistorStateType =
            "Transistor";
        constexpr const char* pipeStateType = "Pipe";
        constexpr const char* hybridStateType = "Hybrid";

        juce::Identifier indexedIdentifier (
            const char* prefix,
            std::size_t index)
        {
            return juce::Identifier (
                juce::String (prefix)
                + juce::String (
                    static_cast<juce::int64> (index)));
        }

        juce::Identifier matrixIdentifier (
            const char* prefix,
            std::size_t row,
            std::size_t column)
        {
            return juce::Identifier (
                juce::String (prefix)
                + juce::String (
                    static_cast<juce::int64> (row))
                + "_"
                + juce::String (
                    static_cast<juce::int64> (column)));
        }

        float stateFloat (
            const juce::ValueTree& tree,
            const juce::Identifier& property,
            float fallback,
            float minimum,
            float maximum) noexcept
        {
            if (! tree.hasProperty (property))
                return fallback;
            const auto value =
                static_cast<float> (
                    static_cast<double> (
                        tree.getProperty (property)));
            if (! std::isfinite (value))
                return fallback;
            return std::max (
                minimum, std::min (maximum, value));
        }

        std::uint32_t stateUInt (
            const juce::ValueTree& tree,
            const juce::Identifier& property,
            std::uint32_t fallback,
            std::uint32_t maximum) noexcept
        {
            if (! tree.hasProperty (property))
                return fallback;
            const auto value =
                static_cast<juce::int64> (
                    tree.getProperty (property));
            if (value < 0
                || static_cast<std::uint64_t> (value) > maximum)
            {
                return fallback;
            }
            return static_cast<std::uint32_t> (value);
        }

        std::uint64_t stateSeed (
            const juce::ValueTree& tree,
            std::uint64_t fallback) noexcept
        {
            if (! tree.hasProperty ("seed"))
                return fallback;
            const auto signedValue =
                static_cast<juce::int64> (
                    tree.getProperty ("seed"));
            const auto result =
                static_cast<std::uint64_t> (signedValue);
            return result != 0 ? result : fallback;
        }

        float onePoleStep (
            double sampleRate,
            float milliseconds) noexcept
        {
            const auto samples =
                sampleRate
                * static_cast<double> (milliseconds)
                * 0.001;
            return static_cast<float> (
                1.0 - std::exp (
                    -1.0 / std::max (1.0, samples)));
        }
    }

    static_assert (
        std::atomic<std::uint32_t>::is_always_lock_free,
        "Layer adapter requires lock-free 32-bit atomics");
    static_assert (
        std::atomic<std::uint64_t>::is_always_lock_free,
        "Supported ChurchDAW targets require lock-free 64-bit atomics");

    ChurchOrganLayerProcessor::AtomicFloat::AtomicFloat() noexcept
    {
        store (0.0f, std::memory_order_relaxed);
    }

    ChurchOrganLayerProcessor::AtomicFloat::AtomicFloat (
        float initial) noexcept
    {
        store (initial, std::memory_order_relaxed);
    }

    void ChurchOrganLayerProcessor::AtomicFloat::store (
        float value,
        std::memory_order order) noexcept
    {
        bits_.store (
            ChurchOrganLayerProcessor::floatBits (value),
            order);
    }

    float ChurchOrganLayerProcessor::AtomicFloat::load (
        std::memory_order order) const noexcept
    {
        return ChurchOrganLayerProcessor::bitsFloat (
            bits_.load (order));
    }

    ChurchOrganLayerProcessor::ChurchOrganLayerProcessor() noexcept
    {
        initialiseAtomicDefaults();
    }

    bool ChurchOrganLayerProcessor::prepare (
        const ChurchOrganLayerPrepareSpec& requested) noexcept
    {
        if (insideProcess_.load (std::memory_order_acquire) != 0
            || ! std::isfinite (requested.sampleRate)
            || requested.sampleRate < minimumSampleRate
            || requested.sampleRate > maximumSampleRate
            || requested.maximumBlockSize == 0
            || requested.maximumBlockSize > maximumBlockCapacity
            || ! validQuality (requested.quality)
            || ! validFamily (requested.initialFamily)
            || ! std::isfinite (requested.transistorEngineTrim)
            || requested.transistorEngineTrim < 0.0f
            || requested.transistorEngineTrim > 0.25f
            || ! std::isfinite (requested.pipeEngineTrim)
            || requested.pipeEngineTrim < 0.0f
            || requested.pipeEngineTrim > 0.25f
            || ! std::isfinite (requested.pipeTuningAHz)
            || requested.pipeTuningAHz < 400.0f
            || requested.pipeTuningAHz > 480.0f
            || ! std::isfinite (requested.pipeStereoWidth)
            || requested.pipeStereoWidth < 0.0f
            || requested.pipeStereoWidth > 1.0f)
        {
            return false;
        }

        suspended_.store (1, std::memory_order_release);
        prepared_.store (0, std::memory_order_release);
        spec_ = requested;
        if (spec_.deterministicSeed == 0)
            spec_.deterministicSeed = defaultLayerSeed;

        if (! prepareEngines (spec_))
            return false;

        activeFamily_.store (
            static_cast<std::uint32_t> (
                spec_.initialFamily),
            std::memory_order_release);
        parameters_.setPlain (
            OrganParameterId::family,
            static_cast<float> (spec_.initialFamily));
        parameters_.setPlain (
            OrganParameterId::quality,
            static_cast<float> (spec_.quality));

        appliedParameterRevisions_.fill (0);
        appliedConfigurationRevisions_.fill (0);
        for (std::size_t family = 0;
             family < familyCount;
             ++family)
        {
            applyPendingConfiguration (
                static_cast<ChurchOrganFamily> (family));
        }

        controllerSmoothingStep_ =
            onePoleStep (spec_.sampleRate, 12.0f);
        trimSmoothingStep_ =
            onePoleStep (spec_.sampleRate, 24.0f);
        midiVolumeCurrent_ =
            midiVolumeTarget_ = clampUnit (
                requestedMidiVolume_.load());
        midiExpressionCurrent_ =
            midiExpressionTarget_ = clampUnit (
                requestedMidiExpression_.load());
        familyTrimCurrent_ = familyOutputTrim (
            spec_.initialFamily);

        renderedSamples_ = 0;
        receivedMidiEvents_ = 0;
        droppedAdapterEvents_ = 0;
        droppedEngineEvents_ = 0;
        ignoredChannelEvents_ = 0;
        unsupportedPitchEvents_ = 0;
        invalidOutputSamples_ = 0;
        resetEnginesAndPerformance();
        prepared_.store (1, std::memory_order_release);
        suspended_.store (0, std::memory_order_release);
        diagnosticRenderedSamples_.store (
            0, std::memory_order_release);
        diagnosticReceivedMidiEvents_.store (
            0, std::memory_order_release);
        diagnosticDroppedAdapterEvents_.store (
            0, std::memory_order_release);
        diagnosticDroppedEngineEvents_.store (
            0, std::memory_order_release);
        diagnosticIgnoredChannelEvents_.store (
            0, std::memory_order_release);
        diagnosticUnsupportedPitchEvents_.store (
            0, std::memory_order_release);
        diagnosticInvalidOutputSamples_.store (
            0, std::memory_order_release);
        diagnosticQueuedEvents_.store (
            0, std::memory_order_release);
        diagnosticActiveNotes_.store (
            0, std::memory_order_release);
        diagnosticPeakBits_.store (
            floatBits (0.0f), std::memory_order_release);
        return true;
    }

    bool ChurchOrganLayerProcessor::reset() noexcept
    {
        if (prepared_.load (std::memory_order_acquire) == 0
            || ! stateOperationAllowed())
        {
            return false;
        }

        resetEnginesAndPerformance();
        midiVolumeCurrent_ =
            midiVolumeTarget_ = clampUnit (
                requestedMidiVolume_.load());
        midiExpressionCurrent_ =
            midiExpressionTarget_ = clampUnit (
                requestedMidiExpression_.load());
        familyTrimCurrent_ =
            familyOutputTrim (activeFamily());
        renderedSamples_ = 0;
        receivedMidiEvents_ = 0;
        droppedAdapterEvents_ = 0;
        droppedEngineEvents_ = 0;
        ignoredChannelEvents_ = 0;
        unsupportedPitchEvents_ = 0;
        invalidOutputSamples_ = 0;
        diagnosticRenderedSamples_.store (
            0, std::memory_order_release);
        diagnosticReceivedMidiEvents_.store (
            0, std::memory_order_release);
        diagnosticDroppedAdapterEvents_.store (
            0, std::memory_order_release);
        diagnosticDroppedEngineEvents_.store (
            0, std::memory_order_release);
        diagnosticIgnoredChannelEvents_.store (
            0, std::memory_order_release);
        diagnosticUnsupportedPitchEvents_.store (
            0, std::memory_order_release);
        diagnosticInvalidOutputSamples_.store (
            0, std::memory_order_release);
        diagnosticQueuedEvents_.store (
            0, std::memory_order_release);
        diagnosticActiveNotes_.store (
            0, std::memory_order_release);
        diagnosticPeakBits_.store (
            floatBits (0.0f), std::memory_order_release);
        return true;
    }

    bool ChurchOrganLayerProcessor::processBlock (
        juce::AudioBuffer<float>& output,
        const juce::MidiBuffer& midi,
        juce::AudioBuffer<float>* reverbSend) noexcept
    {
        juce::ScopedNoDenormals noDenormals;

        if (insideProcess_.exchange (
                1, std::memory_order_acq_rel) != 0)
        {
            clearBuffers (output, reverbSend);
            return false;
        }

        struct ProcessGuard
        {
            std::atomic<std::uint32_t>& flag;
            ~ProcessGuard()
            {
                flag.store (0, std::memory_order_release);
            }
        } guard { insideProcess_ };

        const auto samples =
            static_cast<std::size_t> (
                std::max (0, output.getNumSamples()));
        if (prepared_.load (std::memory_order_acquire) == 0
            || suspended_.load (std::memory_order_acquire) != 0
            || output.getNumChannels() <= 0
            || samples == 0
            || samples > spec_.maximumBlockSize
            || samples > maximumBlockCapacity
            || (reverbSend != nullptr
                && (reverbSend == &output
                    || reverbSend->getNumSamples()
                        < output.getNumSamples())))
        {
            clearBuffers (output, reverbSend);
            return false;
        }

        const auto family = activeFamily();
        applyPendingConfiguration (family);

        // External/restored controller state becomes the target at block zero.
        // MIDI events collected below can replace it at their exact offset.
        midiVolumeTarget_ = clampUnit (
            requestedMidiVolume_.load (
                std::memory_order_acquire));
        midiExpressionTarget_ = clampUnit (
            requestedMidiExpression_.load (
                std::memory_order_acquire));

        for (int channel = 2;
             channel < output.getNumChannels();
             ++channel)
        {
            output.clear (
                channel, 0, output.getNumSamples());
        }
        if (activeFamily() != ChurchOrganFamily::worshipHybrid
            && reverbSend != nullptr)
        {
            reverbSend->clear();
        }
        else if (reverbSend != nullptr)
        {
            for (int channel = 2;
                 channel < reverbSend->getNumChannels();
                 ++channel)
            {
                reverbSend->clear (
                    channel,
                    0,
                    output.getNumSamples());
            }
        }

        collectMidi (midi);
        std::size_t chunkStart = 0;
        while (chunkStart < samples)
        {
            const auto chunkSamples = std::min (
                maximumEngineBlockCapacity,
                samples - chunkStart);
            dispatchCurrentBlockEvents (chunkSamples);
            if (! renderActiveFamily (
                    output,
                    chunkStart,
                    chunkSamples,
                    reverbSend))
            {
                clearBuffers (output, reverbSend);
                return false;
            }
            applyControllerGain (
                output,
                chunkStart,
                chunkSamples,
                reverbSend);
            chunkStart += chunkSamples;
        }

        renderedSamples_ += samples;
        publishDiagnostics (output);
        return true;
    }

    void ChurchOrganLayerProcessor::setProcessingSuspended (
        bool suspended) noexcept
    {
        suspended_.store (
            suspended ? 1u : 0u,
            std::memory_order_release);
    }

    bool ChurchOrganLayerProcessor::isProcessingSuspended() const noexcept
    {
        return suspended_.load (
            std::memory_order_acquire) != 0;
    }

    bool ChurchOrganLayerProcessor::isInsideProcessBlock() const noexcept
    {
        return insideProcess_.load (
            std::memory_order_acquire) != 0;
    }

    bool ChurchOrganLayerProcessor::selectFamilyWhileSuspended (
        ChurchOrganFamily family) noexcept
    {
        if (! validFamily (family)
            || prepared_.load (std::memory_order_acquire) == 0
            || ! stateOperationAllowed())
        {
            return false;
        }

        resetEnginesAndPerformance();
        activeFamily_.store (
            static_cast<std::uint32_t> (family),
            std::memory_order_release);
        parameters_.setPlain (
            OrganParameterId::family,
            static_cast<float> (family));
        applyPendingConfiguration (family);
        familyTrimCurrent_ = familyOutputTrim (family);
        spec_.initialFamily = family;
        return true;
    }

    ChurchOrganFamily
        ChurchOrganLayerProcessor::activeFamily() const noexcept
    {
        const auto value = activeFamily_.load (
            std::memory_order_acquire);
        return value < familyCount
            ? static_cast<ChurchOrganFamily> (value)
            : ChurchOrganFamily::tonewheel;
    }

    OrganParameterStore&
        ChurchOrganLayerProcessor::parameters() noexcept
    {
        return parameters_;
    }

    const OrganParameterStore&
        ChurchOrganLayerProcessor::parameters() const noexcept
    {
        return parameters_;
    }

    void ChurchOrganLayerProcessor::setParameterPlain (
        OrganParameterId parameter,
        float value) noexcept
    {
        if (static_cast<std::size_t> (parameter)
            < organParameterCount)
        {
            parameters_.setPlain (parameter, value);
        }
    }

    void ChurchOrganLayerProcessor::setParameterNormalised (
        OrganParameterId parameter,
        float value) noexcept
    {
        if (static_cast<std::size_t> (parameter)
            < organParameterCount)
        {
            parameters_.setNormalised (parameter, value);
        }
    }

    void ChurchOrganLayerProcessor::setChannelPerformanceEnvelopeSeconds (
        float attackSeconds,
        float releaseSeconds) noexcept
    {
        const auto channelValue = [] (
            float value,
            float nativeValue,
            float minimum,
            float maximum) noexcept
        {
            if (! std::isfinite (value))
                return 0.0f;
            const auto bounded =
                std::max (minimum, std::min (maximum, value));
            return std::abs (bounded - nativeValue) <= 1.0e-6f
                ? 0.0f
                : bounded * 1000.0f;
        };

        parameters_.setPlain (
            OrganParameterId::performanceAttackMs,
            channelValue (
                attackSeconds,
                nativeChannelAttackSeconds,
                0.001f,
                2.0f));
        parameters_.setPlain (
            OrganParameterId::performanceReleaseMs,
            channelValue (
                releaseSeconds,
                nativeChannelReleaseSeconds,
                0.05f,
                12.0f));
    }

    void ChurchOrganLayerProcessor::setFamilyOutputTrim (
        ChurchOrganFamily family,
        float linearGain) noexcept
    {
        if (! validFamily (family))
            return;
        familyOutputTrims_[familyIndex (family)].store (
            clampFinite (
                linearGain, 0.0f, 2.0f, 1.0f));
    }

    float ChurchOrganLayerProcessor::familyOutputTrim (
        ChurchOrganFamily family) const noexcept
    {
        return validFamily (family)
            ? clampFinite (
                familyOutputTrims_[familyIndex (family)].load(),
                0.0f,
                2.0f,
                1.0f)
            : 1.0f;
    }

    void ChurchOrganLayerProcessor::setTransistorProfile (
        TransistorOrganProfile profile) noexcept
    {
        const auto index =
            static_cast<std::uint32_t> (profile);
        if (index > static_cast<std::uint32_t> (
                TransistorOrganProfile::germanCombo))
        {
            return;
        }
        transistorProfile_.store (
            index, std::memory_order_release);
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setTransistorTabLevel (
        TransistorOrganTab tab,
        float levelZeroToOne) noexcept
    {
        const auto index = static_cast<std::size_t> (tab);
        if (index >= transistorTabs_.size())
            return;
        transistorTabs_[index].store (
            clampUnit (levelZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setTransistorVibrato (
        bool enabled,
        float depthZeroToOne) noexcept
    {
        transistorVibratoEnabled_.store (
            enabled ? 1u : 0u,
            std::memory_order_release);
        transistorVibratoDepth_.store (
            clampUnit (depthZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setTransistorDrive (
        float driveZeroToOne) noexcept
    {
        transistorDrive_.store (
            clampUnit (driveZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setTransistorStereoWidth (
        float widthZeroToOne) noexcept
    {
        transistorStereoWidth_.store (
            clampUnit (widthZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeRankLevel (
        PipeOrganManual manual,
        PipeOrganRank rank,
        float levelZeroToOne) noexcept
    {
        const auto manualValue =
            static_cast<std::size_t> (manual);
        const auto rankValue =
            static_cast<std::size_t> (rank);
        if (manualValue >= pipeOrganManualCount
            || rankValue >= pipeOrganRankCount)
        {
            return;
        }
        pipeRankLevels_[manualValue][rankValue].store (
            clampUnit (levelZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeCoupler (
        PipeOrganManual source,
        PipeOrganManual destination,
        bool enabled) noexcept
    {
        const auto sourceValue =
            static_cast<std::size_t> (source);
        const auto destinationValue =
            static_cast<std::size_t> (destination);
        if (sourceValue >= pipeOrganManualCount
            || destinationValue >= pipeOrganManualCount
            || sourceValue == destinationValue)
        {
            return;
        }
        pipeCouplers_[sourceValue][destinationValue].store (
            enabled ? 1u : 0u,
            std::memory_order_release);
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeSwell (
        PipeOrganManual manual,
        float levelZeroToOne) noexcept
    {
        const auto index =
            static_cast<std::size_t> (manual);
        if (index >= pipeOrganManualCount)
            return;
        pipeSwell_[index].store (
            clampUnit (levelZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeCrescendo (
        float levelZeroToOne) noexcept
    {
        pipeCrescendo_.store (
            clampUnit (levelZeroToOne));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeTremulant (
        PipeOrganManual manual,
        bool enabled,
        float depthZeroToOne,
        float rateHz) noexcept
    {
        const auto index =
            static_cast<std::size_t> (manual);
        if (index >= pipeOrganManualCount)
            return;
        pipeTremulantEnabled_[index].store (
            enabled ? 1u : 0u,
            std::memory_order_release);
        pipeTremulantDepth_[index].store (
            clampUnit (depthZeroToOne));
        pipeTremulantRate_[index].store (
            clampFinite (
                rateHz, 0.1f, 12.0f,
                defaultPipeTremulantRates[index]));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setPipeTemperament (
        PipeOrganTemperament temperament) noexcept
    {
        const auto value =
            static_cast<std::uint32_t> (temperament);
        if (value > static_cast<std::uint32_t> (
                PipeOrganTemperament::pureChoir))
        {
            return;
        }
        pipeTemperament_.store (
            value, std::memory_order_release);
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setHybridAutoMixParameters (
        const AutoMixParameters& parameters) noexcept
    {
        hybridAutoMix_.enabled.store (
            parameters.enabled ? 1u : 0u,
            std::memory_order_release);
        hybridAutoMix_.amount.store (
            clampUnit (parameters.amount));
        hybridAutoMix_.reactionMilliseconds.store (
            clampFinite (
                parameters.reactionMilliseconds,
                250.0f,
                5000.0f,
                720.0f));
        hybridAutoMix_.recoveryMilliseconds.store (
            clampFinite (
                parameters.recoveryMilliseconds,
                800.0f,
                10000.0f,
                1900.0f));
        hybridAutoMix_.maximumAttenuationDecibels.store (
            clampFinite (
                parameters.maximumAttenuationDecibels,
                0.0f,
                6.0f,
                3.0f));
        hybridAutoMix_.pianoConflict.store (
            clampUnit (parameters.pianoConflict));
        hybridAutoMix_.externalPadConflict.store (
            clampUnit (parameters.externalPadConflict));
        hybridAutoMix_.externalStringsConflict.store (
            clampUnit (parameters.externalStringsConflict));
        hybridAutoMix_.masterProtection.store (
            clampUnit (parameters.masterProtection));
        hybridAutoMix_.lowMidZone.store (
            clampUnit (parameters.lowMidZone));
        hybridAutoMix_.midZone.store (
            clampUnit (parameters.midZone));
        hybridAutoMix_.soloRoomLiftDecibels.store (
            clampFinite (
                parameters.soloRoomLiftDecibels,
                0.0f,
                1.0f,
                0.45f));
        hybridAutoMix_.sidechainMask.store (
            parameters.sidechainMask & autoMixAllSources,
            std::memory_order_release);
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    void ChurchOrganLayerProcessor::setHybridAutoMixControlFrame (
        const AutoMixControlFrame& controls) noexcept
    {
        hybridAutoMix_.pianoEnvelope.store (
            clampUnit (controls.pianoEnvelope));
        hybridAutoMix_.externalPadEnvelope.store (
            clampUnit (controls.externalPadEnvelope));
        hybridAutoMix_.externalStringsEnvelope.store (
            clampUnit (controls.externalStringsEnvelope));
        hybridAutoMix_.masterProximity.store (
            clampUnit (controls.masterProximity));
        configurationRevision_.fetch_add (
            1, std::memory_order_release);
    }

    bool ChurchOrganLayerProcessor::validFamily (
        ChurchOrganFamily family) noexcept
    {
        return static_cast<std::size_t> (family) < familyCount;
    }

    bool ChurchOrganLayerProcessor::validQuality (
        ChurchOrganLayerQuality quality) noexcept
    {
        return quality == ChurchOrganLayerQuality::eco
            || quality == ChurchOrganLayerQuality::standard
            || quality == ChurchOrganLayerQuality::ultra;
    }

    bool ChurchOrganLayerProcessor::validManual (
        Manual manual) noexcept
    {
        return manual == Manual::upper
            || manual == Manual::lower
            || manual == Manual::pedal;
    }

    std::size_t ChurchOrganLayerProcessor::familyIndex (
        ChurchOrganFamily family) noexcept
    {
        return static_cast<std::size_t> (family);
    }

    std::size_t ChurchOrganLayerProcessor::manualIndex (
        Manual manual) noexcept
    {
        return static_cast<std::size_t> (manual);
    }

    Manual ChurchOrganLayerProcessor::manualForChannel (
        int midiChannel,
        bool& valid) noexcept
    {
        valid = true;
        switch (midiChannel)
        {
            case 1: return Manual::upper;
            case 2: return Manual::lower;
            case 3: return Manual::pedal;
            default:
                valid = false;
                return Manual::upper;
        }
    }

    int ChurchOrganLayerProcessor::eventPriority (
        AdapterEventType type) noexcept
    {
        // Preserve source order for equal offsets. The sole safety exception
        // is an all-off: it is moved after every other event at that sample so
        // a simultaneous note-on can never survive a panic.
        switch (type)
        {
            case AdapterEventType::allSoundOff:
            case AdapterEventType::allNotesOff:
                return 1;
            case AdapterEventType::noteOn:
            case AdapterEventType::noteOff:
            case AdapterEventType::sustain:
            case AdapterEventType::volume:
            case AdapterEventType::expression:
            case AdapterEventType::resetControllers:
            case AdapterEventType::pitchBend:
                return 0;
        }
        return 0;
    }

    float ChurchOrganLayerProcessor::clampUnit (
        float value) noexcept
    {
        return clampFinite (value, 0.0f, 1.0f, 0.0f);
    }

    float ChurchOrganLayerProcessor::clampFinite (
        float value,
        float minimum,
        float maximum,
        float fallback) noexcept
    {
        if (! std::isfinite (value))
            value = fallback;
        return std::max (
            minimum, std::min (maximum, value));
    }

    std::uint32_t ChurchOrganLayerProcessor::floatBits (
        float value) noexcept
    {
        std::uint32_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        return bits;
    }

    float ChurchOrganLayerProcessor::bitsFloat (
        std::uint32_t value) noexcept
    {
        float result = 0.0f;
        std::memcpy (&result, &value, sizeof (result));
        return result;
    }

    OrganQualityProfile ChurchOrganLayerProcessor::tonewheelQuality (
        ChurchOrganLayerQuality quality) noexcept
    {
        switch (quality)
        {
            case ChurchOrganLayerQuality::eco:
                return OrganQualityProfile::eco;
            case ChurchOrganLayerQuality::ultra:
                return OrganQualityProfile::ultra;
            case ChurchOrganLayerQuality::standard:
            default:
                return OrganQualityProfile::standard;
        }
    }

    WorshipHybridQuality ChurchOrganLayerProcessor::hybridQuality (
        ChurchOrganLayerQuality quality) noexcept
    {
        switch (quality)
        {
            case ChurchOrganLayerQuality::eco:
                return WorshipHybridQuality::eco;
            case ChurchOrganLayerQuality::ultra:
                return WorshipHybridQuality::ultra;
            case ChurchOrganLayerQuality::standard:
            default:
                return WorshipHybridQuality::standard;
        }
    }

    PipeOrganQuality ChurchOrganLayerProcessor::pipeQuality (
        ChurchOrganLayerQuality quality) noexcept
    {
        switch (quality)
        {
            case ChurchOrganLayerQuality::eco:
                return PipeOrganQuality::eco;
            case ChurchOrganLayerQuality::ultra:
                return PipeOrganQuality::ultra;
            case ChurchOrganLayerQuality::standard:
            default:
                return PipeOrganQuality::standard;
        }
    }

    PipeOrganManual ChurchOrganLayerProcessor::pipeManual (
        Manual manual) noexcept
    {
        switch (manual)
        {
            case Manual::lower:
                return PipeOrganManual::lower;
            case Manual::pedal:
                return PipeOrganManual::pedal;
            case Manual::upper:
            default:
                return PipeOrganManual::upper;
        }
    }

    void ChurchOrganLayerProcessor::initialiseAtomicDefaults() noexcept
    {
        for (auto& trim : familyOutputTrims_)
            trim.store (1.0f, std::memory_order_relaxed);

        transistorProfile_.store (
            static_cast<std::uint32_t> (
                TransistorOrganProfile::britishCombo),
            std::memory_order_relaxed);
        for (std::size_t tab = 0;
             tab < transistorTabs_.size();
             ++tab)
        {
            transistorTabs_[tab].store (
                defaultTransistorTabs[tab],
                std::memory_order_relaxed);
        }
        transistorVibratoEnabled_.store (
            transistorDefaultVibratoEnabled ? 1u : 0u,
            std::memory_order_relaxed);
        transistorVibratoDepth_.store (
            transistorDefaultVibratoDepth,
            std::memory_order_relaxed);
        transistorDrive_.store (
            0.18f, std::memory_order_relaxed);
        transistorStereoWidth_.store (
            0.22f, std::memory_order_relaxed);

        for (auto& manual : pipeRankLevels_)
            for (auto& rank : manual)
                rank.store (0.0f, std::memory_order_relaxed);

        pipeRankLevels_[0][static_cast<std::size_t> (
            PipeOrganRank::principal)].store (
                0.80f, std::memory_order_relaxed);
        pipeRankLevels_[0][static_cast<std::size_t> (
            PipeOrganRank::flute)].store (
                0.30f, std::memory_order_relaxed);
        pipeRankLevels_[0][static_cast<std::size_t> (
            PipeOrganRank::octave)].store (
                0.48f, std::memory_order_relaxed);
        pipeRankLevels_[0][static_cast<std::size_t> (
            PipeOrganRank::mixture)].store (
                0.16f, std::memory_order_relaxed);
        pipeRankLevels_[0][static_cast<std::size_t> (
            PipeOrganRank::strings)].store (
                0.14f, std::memory_order_relaxed);
        pipeRankLevels_[1][static_cast<std::size_t> (
            PipeOrganRank::gedackt)].store (
                0.72f, std::memory_order_relaxed);
        pipeRankLevels_[1][static_cast<std::size_t> (
            PipeOrganRank::flute)].store (
                0.38f, std::memory_order_relaxed);
        pipeRankLevels_[1][static_cast<std::size_t> (
            PipeOrganRank::choir)].store (
                0.44f, std::memory_order_relaxed);
        pipeRankLevels_[1][static_cast<std::size_t> (
            PipeOrganRank::reed)].store (
                0.10f, std::memory_order_relaxed);
        pipeRankLevels_[2][static_cast<std::size_t> (
            PipeOrganRank::pedal)].store (
                0.84f, std::memory_order_relaxed);
        pipeRankLevels_[2][static_cast<std::size_t> (
            PipeOrganRank::bourdon)].store (
                0.54f, std::memory_order_relaxed);
        pipeRankLevels_[2][static_cast<std::size_t> (
            PipeOrganRank::principal)].store (
                0.18f, std::memory_order_relaxed);

        for (auto& source : pipeCouplers_)
            for (auto& destination : source)
                destination.store (
                    0, std::memory_order_relaxed);

        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            pipeSwell_[manual].store (
                defaultPipeSwell[manual],
                std::memory_order_relaxed);
            pipeTremulantEnabled_[manual].store (
                0, std::memory_order_relaxed);
            pipeTremulantDepth_[manual].store (
                0.42f, std::memory_order_relaxed);
            pipeTremulantRate_[manual].store (
                defaultPipeTremulantRates[manual],
                std::memory_order_relaxed);
        }
        pipeCrescendo_.store (
            0.0f, std::memory_order_relaxed);
        pipeTemperament_.store (
            static_cast<std::uint32_t> (
                PipeOrganTemperament::churchWell),
            std::memory_order_relaxed);

        AutoMixParameters defaultAutoMix;
        setHybridAutoMixParameters (defaultAutoMix);
        setHybridAutoMixControlFrame ({});
        atomicDefaultsInitialised_ = true;
    }

    bool ChurchOrganLayerProcessor::prepareEngines (
        const ChurchOrganLayerPrepareSpec& spec) noexcept
    {
        const auto engineMaximumBlock = std::min (
            spec.maximumBlockSize,
            maximumEngineBlockCapacity);

        TonewheelOrganPrepareSpec tonewheelSpec;
        tonewheelSpec.sampleRate = spec.sampleRate;
        tonewheelSpec.maximumBlockSize =
            engineMaximumBlock;
        tonewheelSpec.deterministicSeed =
            spec.deterministicSeed ^ tonewheelSeedSalt;
        tonewheelSpec.quality =
            tonewheelQuality (spec.quality);

        TransistorOrganPrepareSpec transistorSpec;
        transistorSpec.sampleRate = spec.sampleRate;
        transistorSpec.maximumBlockSize =
            engineMaximumBlock;
        transistorSpec.deterministicSeed =
            spec.deterministicSeed ^ transistorSeedSalt;
        transistorSpec.profile =
            static_cast<TransistorOrganProfile> (
                transistorProfile_.load (
                    std::memory_order_acquire));
        transistorSpec.outputTrim =
            spec.transistorEngineTrim;
        transistorSpec.drive =
            clampUnit (transistorDrive_.load());
        transistorSpec.stereoWidth =
            clampUnit (transistorStereoWidth_.load());
        transistorSpec.vibratoDepth =
            clampUnit (transistorVibratoDepth_.load());
        transistorSpec.vibratoEnabled =
            transistorVibratoEnabled_.load (
                std::memory_order_acquire) != 0;

        PipeOrganPrepareSpec pipeSpec;
        pipeSpec.sampleRate = spec.sampleRate;
        pipeSpec.maximumBlockSize =
            engineMaximumBlock;
        pipeSpec.deterministicSeed =
            spec.deterministicSeed ^ pipeSeedSalt;
        pipeSpec.quality = pipeQuality (spec.quality);
        pipeSpec.temperament =
            static_cast<PipeOrganTemperament> (
                pipeTemperament_.load (
                    std::memory_order_acquire));
        pipeSpec.tuningAHz = spec.pipeTuningAHz;
        pipeSpec.outputTrim = spec.pipeEngineTrim;
        pipeSpec.stereoWidth = spec.pipeStereoWidth;

        WorshipHybridPrepareSpec hybridSpec;
        hybridSpec.sampleRate = spec.sampleRate;
        hybridSpec.maximumBlockSize =
            engineMaximumBlock;
        hybridSpec.deterministicSeed =
            spec.deterministicSeed ^ hybridSeedSalt;
        hybridSpec.quality = hybridQuality (spec.quality);

        return tonewheel_.prepare (tonewheelSpec)
            && transistor_.prepare (transistorSpec)
            && pipe_.prepare (pipeSpec)
            && hybrid_.prepare (hybridSpec);
    }

    void ChurchOrganLayerProcessor::resetEnginesAndPerformance() noexcept
    {
        tonewheel_.reset();
        transistor_.reset();
        pipe_.reset();
        hybrid_.reset();
        performance_.fill ({});
        events_.fill ({});
        gainEvents_.fill ({});
        eventCount_ = 0;
        gainEventCount_ = 0;
    }

    void ChurchOrganLayerProcessor::collectMidi (
        const juce::MidiBuffer& midi) noexcept
    {
        for (const auto metadata : midi)
        {
            ++receivedMidiEvents_;
            if (metadata.data == nullptr
                || metadata.numBytes <= 0)
            {
                continue;
            }

            const auto status = metadata.data[0];
            if (status < 0x80u || status >= 0xf0u)
                continue;

            const auto messageType =
                static_cast<std::uint8_t> (status & 0xf0u);
            const auto channel =
                static_cast<int> (status & 0x0fu) + 1;
            bool validChannel = false;
            const auto manual =
                manualForChannel (channel, validChannel);

            const auto isRelevant =
                messageType == 0x80u
                || messageType == 0x90u
                || messageType == 0xb0u
                || messageType == 0xe0u;
            if (! validChannel)
            {
                if (isRelevant)
                    ++ignoredChannelEvents_;
                continue;
            }

            const auto offset =
                metadata.samplePosition > 0
                    ? static_cast<std::uint32_t> (
                        std::min<juce::int64> (
                            metadata.samplePosition,
                            std::numeric_limits<
                                std::uint32_t>::max()))
                    : 0u;
            const auto data1 =
                static_cast<std::uint8_t> (
                    metadata.numBytes > 1
                        ? metadata.data[1] & 0x7fu
                        : 0u);
            const auto data2 =
                static_cast<std::uint8_t> (
                    metadata.numBytes > 2
                        ? metadata.data[2] & 0x7fu
                        : 0u);

            AdapterEvent event;
            event.manual = manual;
            event.data1 = data1;
            event.data2 = data2;
            event.sampleOffset = offset;

            if (messageType == 0x90u)
            {
                event.type = data2 == 0
                    ? AdapterEventType::noteOff
                    : AdapterEventType::noteOn;
                queueAdapterEvent (event);
            }
            else if (messageType == 0x80u)
            {
                event.type = AdapterEventType::noteOff;
                queueAdapterEvent (event);
            }
            else if (messageType == 0xe0u)
            {
                event.type = AdapterEventType::pitchBend;
                queueAdapterEvent (event);
            }
            else if (messageType == 0xb0u)
            {
                switch (data1)
                {
                    case 7:
                        event.type = AdapterEventType::volume;
                        queueAdapterEvent (event);
                        break;
                    case 11:
                        event.type = AdapterEventType::expression;
                        queueAdapterEvent (event);
                        break;
                    case 64:
                        event.type = AdapterEventType::sustain;
                        queueAdapterEvent (event);
                        break;
                    case 120:
                        event.type = AdapterEventType::allSoundOff;
                        queueAdapterEvent (event);
                        break;
                    case 121:
                        event.type =
                            AdapterEventType::resetControllers;
                        queueAdapterEvent (event);
                        break;
                    case 123:
                        event.type = AdapterEventType::allNotesOff;
                        queueAdapterEvent (event);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    bool ChurchOrganLayerProcessor::queueAdapterEvent (
        const AdapterEvent& event) noexcept
    {
        if (eventCount_ >= events_.size())
        {
            ++droppedAdapterEvents_;
            return false;
        }

        std::size_t insertion = eventCount_;
        while (insertion > 0)
        {
            const auto& previous =
                events_[insertion - 1];
            const auto move =
                previous.sampleOffset > event.sampleOffset
                || (previous.sampleOffset == event.sampleOffset
                    && eventPriority (previous.type)
                        > eventPriority (event.type));
            if (! move)
                break;
            events_[insertion] = previous;
            --insertion;
        }
        events_[insertion] = event;
        ++eventCount_;
        return true;
    }

    void ChurchOrganLayerProcessor::dispatchCurrentBlockEvents (
        std::size_t numSamples) noexcept
    {
        gainEventCount_ = 0;
        std::size_t firstUnprocessed = 0;
        while (firstUnprocessed < eventCount_
               && events_[firstUnprocessed].sampleOffset
                    < numSamples)
        {
            if (! dispatchEvent (events_[firstUnprocessed]))
                ++droppedEngineEvents_;
            ++firstUnprocessed;
        }
        carryAdapterEvents (
            firstUnprocessed, numSamples);
    }

    bool ChurchOrganLayerProcessor::dispatchEvent (
        const AdapterEvent& event) noexcept
    {
        if (event.type
            == AdapterEventType::resetControllers)
        {
            auto sustainOff = event;
            sustainOff.type = AdapterEventType::sustain;
            sustainOff.data2 = 0;
            const auto sustainAccepted =
                dispatchEvent (sustainOff);

            auto expressionReset = event;
            expressionReset.type =
                AdapterEventType::expression;
            expressionReset.data2 = 127;
            return appendGainEvent (expressionReset)
                && sustainAccepted;
        }
        if (event.type == AdapterEventType::volume
            || event.type == AdapterEventType::expression)
        {
            return appendGainEvent (event);
        }
        if (event.type == AdapterEventType::pitchBend)
        {
            ++unsupportedPitchEvents_;
            return true;
        }

        switch (activeFamily())
        {
            case ChurchOrganFamily::tonewheel:
                return dispatchTonewheelEvent (event);
            case ChurchOrganFamily::transistor:
                return dispatchTransistorEvent (event);
            case ChurchOrganFamily::pipe:
                return dispatchPipeEvent (event);
            case ChurchOrganFamily::worshipHybrid:
                return dispatchHybridEvent (event);
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::dispatchTonewheelEvent (
        const AdapterEvent& event) noexcept
    {
        switch (event.type)
        {
            case AdapterEventType::noteOn:
                return tonewheel_.queueNoteOn (
                    event.manual,
                    event.data1,
                    event.data2,
                    event.sampleOffset);
            case AdapterEventType::noteOff:
                return tonewheel_.queueNoteOff (
                    event.manual,
                    event.data1,
                    event.sampleOffset);
            case AdapterEventType::sustain:
                return tonewheel_.queueSustain (
                    event.manual,
                    event.data2 >= 64,
                    event.sampleOffset);
            case AdapterEventType::allNotesOff:
            case AdapterEventType::allSoundOff:
                return tonewheel_.queueAllNotesOff (
                    event.manual,
                    event.sampleOffset);
            case AdapterEventType::volume:
            case AdapterEventType::expression:
            case AdapterEventType::resetControllers:
            case AdapterEventType::pitchBend:
                return true;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::dispatchTransistorEvent (
        const AdapterEvent& event) noexcept
    {
        if (! activeFamilySupportsManual (event.manual))
        {
            ++ignoredChannelEvents_;
            return true;
        }
        switch (event.type)
        {
            case AdapterEventType::noteOn:
                return dispatchExternalSustainNoteOn (event);
            case AdapterEventType::noteOff:
                return dispatchExternalSustainNoteOff (event);
            case AdapterEventType::sustain:
                return dispatchExternalSustain (event);
            case AdapterEventType::allNotesOff:
            case AdapterEventType::allSoundOff:
                return dispatchExternalAllNotesOff (event);
            case AdapterEventType::volume:
            case AdapterEventType::expression:
            case AdapterEventType::resetControllers:
            case AdapterEventType::pitchBend:
                return true;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::dispatchPipeEvent (
        const AdapterEvent& event) noexcept
    {
        switch (event.type)
        {
            case AdapterEventType::noteOn:
                return dispatchExternalSustainNoteOn (event);
            case AdapterEventType::noteOff:
                return dispatchExternalSustainNoteOff (event);
            case AdapterEventType::sustain:
                return dispatchExternalSustain (event);
            case AdapterEventType::allNotesOff:
            case AdapterEventType::allSoundOff:
                return dispatchExternalAllNotesOff (event);
            case AdapterEventType::volume:
            case AdapterEventType::expression:
            case AdapterEventType::resetControllers:
            case AdapterEventType::pitchBend:
                return true;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::dispatchHybridEvent (
        const AdapterEvent& event) noexcept
    {
        switch (event.type)
        {
            case AdapterEventType::noteOn:
                return hybrid_.queueNoteOn (
                    event.manual,
                    event.data1,
                    event.data2,
                    event.sampleOffset);
            case AdapterEventType::noteOff:
                return hybrid_.queueNoteOff (
                    event.manual,
                    event.data1,
                    event.sampleOffset);
            case AdapterEventType::sustain:
                return hybrid_.queueSustain (
                    event.manual,
                    event.data2 >= 64,
                    event.sampleOffset);
            case AdapterEventType::allNotesOff:
            case AdapterEventType::allSoundOff:
                return hybrid_.queueAllNotesOff (
                    event.manual,
                    event.sampleOffset);
            case AdapterEventType::volume:
            case AdapterEventType::expression:
            case AdapterEventType::resetControllers:
            case AdapterEventType::pitchBend:
                return true;
        }
        return false;
    }

    void ChurchOrganLayerProcessor::carryAdapterEvents (
        std::size_t firstUnprocessed,
        std::size_t numSamples) noexcept
    {
        const auto remaining =
            eventCount_ - firstUnprocessed;
        for (std::size_t index = 0;
             index < remaining;
             ++index)
        {
            events_[index] =
                events_[firstUnprocessed + index];
            const auto offset =
                events_[index].sampleOffset;
            events_[index].sampleOffset =
                offset > numSamples
                    ? static_cast<std::uint32_t> (
                        offset - numSamples)
                    : 0u;
        }
        eventCount_ = remaining;
    }

    bool ChurchOrganLayerProcessor::appendGainEvent (
        const AdapterEvent& event) noexcept
    {
        if (gainEventCount_ >= gainEvents_.size())
        {
            ++droppedAdapterEvents_;
            return false;
        }

        GainEvent gainEvent;
        gainEvent.type = event.type;
        gainEvent.target =
            static_cast<float> (event.data2) / 127.0f;
        gainEvent.sampleOffset = event.sampleOffset;
        gainEvents_[gainEventCount_++] = gainEvent;

        if (event.type == AdapterEventType::volume)
            requestedMidiVolume_.store (gainEvent.target);
        else
            requestedMidiExpression_.store (gainEvent.target);
        return true;
    }

    bool ChurchOrganLayerProcessor::dispatchExternalSustainNoteOn (
        const AdapterEvent& event) noexcept
    {
        if (! activeFamilySupportsManual (event.manual))
            return true;
        auto& state =
            performance_[manualIndex (event.manual)];
        const auto note =
            static_cast<std::size_t> (event.data1);
        state.physicallyHeld[note] = true;
        state.sustained[note] = false;
        return queueActiveNoteOn (
            event.manual,
            event.data1,
            event.data2,
            event.sampleOffset);
    }

    bool ChurchOrganLayerProcessor::dispatchExternalSustainNoteOff (
        const AdapterEvent& event) noexcept
    {
        if (! activeFamilySupportsManual (event.manual))
            return true;
        auto& state =
            performance_[manualIndex (event.manual)];
        const auto note =
            static_cast<std::size_t> (event.data1);
        state.physicallyHeld[note] = false;
        if (state.sustainDown)
        {
            state.sustained[note] = true;
            return true;
        }
        state.sustained[note] = false;
        return queueActiveNoteOff (
            event.manual,
            event.data1,
            event.sampleOffset);
    }

    bool ChurchOrganLayerProcessor::dispatchExternalSustain (
        const AdapterEvent& event) noexcept
    {
        if (! activeFamilySupportsManual (event.manual))
            return true;
        auto& state =
            performance_[manualIndex (event.manual)];
        const auto down = event.data2 >= 64;
        if (state.sustainDown == down)
            return true;
        state.sustainDown = down;
        if (down)
            return true;

        auto accepted = true;
        for (std::size_t note = 0;
             note < midiNoteCount;
             ++note)
        {
            if (state.sustained[note]
                && ! state.physicallyHeld[note])
            {
                state.sustained[note] = false;
                accepted =
                    queueActiveNoteOff (
                        event.manual,
                        static_cast<int> (note),
                        event.sampleOffset)
                    && accepted;
            }
        }
        return accepted;
    }

    bool ChurchOrganLayerProcessor::dispatchExternalAllNotesOff (
        const AdapterEvent& event) noexcept
    {
        if (! activeFamilySupportsManual (event.manual))
            return true;
        auto& state =
            performance_[manualIndex (event.manual)];
        state = {};
        return queueActiveAllNotesOff (
            event.manual, event.sampleOffset);
    }

    bool ChurchOrganLayerProcessor::queueActiveNoteOn (
        Manual manual,
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        switch (activeFamily())
        {
            case ChurchOrganFamily::transistor:
                return transistor_.queueUpperNoteOn (
                    note, velocity, sampleOffset);
            case ChurchOrganFamily::pipe:
                return pipe_.queueNoteOn (
                    pipeManual (manual),
                    note,
                    velocity,
                    sampleOffset);
            case ChurchOrganFamily::tonewheel:
            case ChurchOrganFamily::worshipHybrid:
                break;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::queueActiveNoteOff (
        Manual manual,
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        switch (activeFamily())
        {
            case ChurchOrganFamily::transistor:
                return transistor_.queueUpperNoteOff (
                    note, sampleOffset);
            case ChurchOrganFamily::pipe:
                return pipe_.queueNoteOff (
                    pipeManual (manual),
                    note,
                    sampleOffset);
            case ChurchOrganFamily::tonewheel:
            case ChurchOrganFamily::worshipHybrid:
                break;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::queueActiveAllNotesOff (
        Manual manual,
        std::uint32_t sampleOffset) noexcept
    {
        switch (activeFamily())
        {
            case ChurchOrganFamily::transistor:
                return transistor_.queueUpperAllNotesOff (
                    sampleOffset);
            case ChurchOrganFamily::pipe:
                return pipe_.queueAllNotesOff (
                    pipeManual (manual),
                    sampleOffset);
            case ChurchOrganFamily::tonewheel:
            case ChurchOrganFamily::worshipHybrid:
                break;
        }
        return false;
    }

    bool ChurchOrganLayerProcessor::activeFamilySupportsManual (
        Manual manual) const noexcept
    {
        return validManual (manual)
            && (activeFamily()
                    != ChurchOrganFamily::transistor
                || manual == Manual::upper);
    }

    void ChurchOrganLayerProcessor::applyPendingConfiguration (
        ChurchOrganFamily family) noexcept
    {
        if (! validFamily (family))
            return;
        const auto index = familyIndex (family);
        const auto parameterRevision =
            parameters_.revision();
        if (appliedParameterRevisions_[index]
            != parameterRevision)
        {
            const auto snapshot = parameters_.snapshot();
            applyOrganParameters (family, snapshot);
            appliedParameterRevisions_[index] =
                parameterRevision;
        }

        const auto configurationRevision =
            configurationRevision_.load (
                std::memory_order_acquire);
        if (appliedConfigurationRevisions_[index]
            == configurationRevision)
        {
            return;
        }

        switch (family)
        {
            case ChurchOrganFamily::tonewheel:
                break;
            case ChurchOrganFamily::transistor:
                applyTransistorConfiguration();
                break;
            case ChurchOrganFamily::pipe:
                applyPipeConfiguration();
                break;
            case ChurchOrganFamily::worshipHybrid:
            {
                const auto snapshot = parameters_.snapshot();
                applyHybridConfiguration (snapshot);
                break;
            }
        }
        appliedConfigurationRevisions_[index] =
            configurationRevision;
    }

    void ChurchOrganLayerProcessor::applyOrganParameters (
        ChurchOrganFamily family,
        const OrganParameterSnapshot& snapshot) noexcept
    {
        const auto performanceAttack =
            snapshot.get (OrganParameterId::performanceAttackMs);
        const auto performanceRelease =
            snapshot.get (OrganParameterId::performanceReleaseMs);

        if (family == ChurchOrganFamily::tonewheel
            || family == ChurchOrganFamily::worshipHybrid)
        {
            auto& destination =
                family == ChurchOrganFamily::tonewheel
                    ? tonewheel_.parameters()
                    : hybrid_.tonewheel().parameters();
            for (std::size_t parameter = 0;
                 parameter < organParameterCount;
                 ++parameter)
            {
                destination.setPlain (
                    static_cast<OrganParameterId> (parameter),
                    snapshot.values[parameter]);
            }
            destination.setPlain (
                OrganParameterId::family,
                family == ChurchOrganFamily::tonewheel
                    ? 0.0f
                    : 3.0f);
            destination.setPlain (
                OrganParameterId::quality,
                static_cast<float> (spec_.quality));

            if (family == ChurchOrganFamily::worshipHybrid)
                applyHybridConfiguration (snapshot);
            return;
        }

        if (family == ChurchOrganFamily::transistor)
        {
            transistor_.setPerformanceEnvelopeMilliseconds (
                performanceAttack,
                performanceRelease);
        }
        else if (family == ChurchOrganFamily::pipe)
        {
            pipe_.setPerformanceEnvelopeMilliseconds (
                performanceAttack,
                performanceRelease);
        }
    }

    void ChurchOrganLayerProcessor::applyTransistorConfiguration() noexcept
    {
        transistor_.setProfile (
            static_cast<TransistorOrganProfile> (
                transistorProfile_.load (
                    std::memory_order_acquire)),
            false);
        for (std::size_t tab = 0;
             tab < transistorTabCount;
             ++tab)
        {
            transistor_.setTabLevel (
                static_cast<TransistorOrganTab> (tab),
                clampUnit (transistorTabs_[tab].load()));
        }
        transistor_.setVibratoEnabled (
            transistorVibratoEnabled_.load (
                std::memory_order_acquire) != 0);
        transistor_.setVibratoDepth (
            clampUnit (transistorVibratoDepth_.load()));
        transistor_.setDrive (
            clampUnit (transistorDrive_.load()));
        transistor_.setStereoWidth (
            clampUnit (transistorStereoWidth_.load()));
        transistor_.setOutputTrim (
            spec_.transistorEngineTrim);
    }

    void ChurchOrganLayerProcessor::applyPipeConfiguration() noexcept
    {
        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            for (std::size_t rank = 0;
                 rank < pipeOrganRankCount;
                 ++rank)
            {
                pipe_.setRankLevel (
                    static_cast<PipeOrganManual> (manual),
                    static_cast<PipeOrganRank> (rank),
                    clampUnit (
                        pipeRankLevels_[manual][rank].load()));
            }
            pipe_.setSwell (
                static_cast<PipeOrganManual> (manual),
                clampUnit (pipeSwell_[manual].load()));
            pipe_.setTremulant (
                static_cast<PipeOrganManual> (manual),
                pipeTremulantEnabled_[manual].load (
                    std::memory_order_acquire) != 0,
                clampUnit (
                    pipeTremulantDepth_[manual].load()),
                clampFinite (
                    pipeTremulantRate_[manual].load(),
                    0.1f,
                    12.0f,
                    defaultPipeTremulantRates[manual]));

            for (std::size_t destination = 0;
                 destination < pipeOrganManualCount;
                 ++destination)
            {
                if (manual != destination)
                {
                    pipe_.setCoupler (
                        static_cast<PipeOrganManual> (manual),
                        static_cast<PipeOrganManual> (destination),
                        pipeCouplers_[manual][destination].load (
                            std::memory_order_acquire) != 0);
                }
            }
        }
        pipe_.setCrescendo (
            clampUnit (pipeCrescendo_.load()));
        pipe_.setTemperament (
            static_cast<PipeOrganTemperament> (
                pipeTemperament_.load (
                    std::memory_order_acquire)));
    }

    void ChurchOrganLayerProcessor::applyHybridConfiguration (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        hybrid_.setWorshipIntensity (
            snapshot.get (
                OrganParameterId::worshipIntensity));
        hybrid_.worshipLayer().setExternalDucking (
            snapshot.get (
                OrganParameterId::worshipDucking));

        const std::array<OrganParameterId, 4> levelParameters {{
            OrganParameterId::worshipPad,
            OrganParameterId::worshipAir,
            OrganParameterId::worshipStrings,
            OrganParameterId::worshipShimmer
        }};
        for (std::size_t layer = 0;
             layer < worshipLayerCount;
             ++layer)
        {
            const auto layerValue =
                static_cast<WorshipLayer> (layer);
            auto parameters =
                hybrid_.worshipLayer().layerParameters (
                    layerValue);
            if (layer < levelParameters.size())
            {
                parameters.level =
                    snapshot.get (levelParameters[layer]);
            }
            parameters.stereoWidth =
                snapshot.get (
                    OrganParameterId::worshipWidth);
            parameters.attackMilliseconds =
                snapshot.get (
                    OrganParameterId::performanceAttackMs) > 0.0f
                    ? snapshot.get (
                        OrganParameterId::performanceAttackMs)
                    : snapshot.get (
                        OrganParameterId::worshipAttackMs);
            parameters.releaseMilliseconds =
                snapshot.get (
                    OrganParameterId::performanceReleaseMs) > 0.0f
                    ? snapshot.get (
                        OrganParameterId::performanceReleaseMs)
                    : snapshot.get (
                        OrganParameterId::worshipReleaseMs);
            parameters.reverbSend =
                snapshot.get (
                    OrganParameterId::reverbAmount);
            hybrid_.worshipLayer().setLayerParameters (
                layerValue, parameters);
        }
        hybrid_.setAutoMixParameters (
            atomicAutoMixParameters());
        hybrid_.setAutoMixControlFrame (
            atomicAutoMixControls());
    }

    AutoMixParameters
        ChurchOrganLayerProcessor::atomicAutoMixParameters() const noexcept
    {
        AutoMixParameters result;
        result.enabled =
            hybridAutoMix_.enabled.load (
                std::memory_order_acquire) != 0;
        result.amount = hybridAutoMix_.amount.load();
        result.reactionMilliseconds =
            hybridAutoMix_.reactionMilliseconds.load();
        result.recoveryMilliseconds =
            hybridAutoMix_.recoveryMilliseconds.load();
        result.maximumAttenuationDecibels =
            hybridAutoMix_.maximumAttenuationDecibels.load();
        result.pianoConflict =
            hybridAutoMix_.pianoConflict.load();
        result.externalPadConflict =
            hybridAutoMix_.externalPadConflict.load();
        result.externalStringsConflict =
            hybridAutoMix_.externalStringsConflict.load();
        result.masterProtection =
            hybridAutoMix_.masterProtection.load();
        result.lowMidZone =
            hybridAutoMix_.lowMidZone.load();
        result.midZone =
            hybridAutoMix_.midZone.load();
        result.soloRoomLiftDecibels =
            hybridAutoMix_.soloRoomLiftDecibels.load();
        result.sidechainMask =
            hybridAutoMix_.sidechainMask.load (
                std::memory_order_acquire)
            & autoMixAllSources;
        return result;
    }

    AutoMixControlFrame
        ChurchOrganLayerProcessor::atomicAutoMixControls() const noexcept
    {
        return {
            clampUnit (
                hybridAutoMix_.pianoEnvelope.load()),
            clampUnit (
                hybridAutoMix_.externalPadEnvelope.load()),
            clampUnit (
                hybridAutoMix_.externalStringsEnvelope.load()),
            clampUnit (
                hybridAutoMix_.masterProximity.load())
        };
    }

    bool ChurchOrganLayerProcessor::renderActiveFamily (
        juce::AudioBuffer<float>& output,
        std::size_t startSample,
        std::size_t numSamples,
        juce::AudioBuffer<float>* reverbSend) noexcept
    {
        const auto channels = output.getNumChannels();
        const auto start =
            static_cast<int> (startSample);
        auto* left = output.getWritePointer (0, start);
        auto* right = channels > 1
            ? output.getWritePointer (1, start)
            : nullptr;

        switch (activeFamily())
        {
            case ChurchOrganFamily::tonewheel:
                return tonewheel_.render ({
                    left, right, numSamples
                });
            case ChurchOrganFamily::transistor:
                return transistor_.render ({
                    left, right, numSamples
                });
            case ChurchOrganFamily::pipe:
                return pipe_.render ({
                    left, right, numSamples
                });
            case ChurchOrganFamily::worshipHybrid:
            {
                float* sendLeft = nullptr;
                float* sendRight = nullptr;
                if (reverbSend != nullptr
                    && reverbSend->getNumChannels() > 0)
                {
                    sendLeft =
                        reverbSend->getWritePointer (0, start);
                    if (reverbSend->getNumChannels() > 1)
                        sendRight =
                            reverbSend->getWritePointer (1, start);
                }
                return hybrid_.render ({
                    left,
                    right,
                    sendLeft,
                    sendRight,
                    numSamples
                });
            }
        }
        return false;
    }

    void ChurchOrganLayerProcessor::applyControllerGain (
        juce::AudioBuffer<float>& output,
        std::size_t startSample,
        std::size_t numSamples,
        juce::AudioBuffer<float>* reverbSend) noexcept
    {
        const auto mainChannels =
            std::min (2, output.getNumChannels());
        const auto sendChannels =
            reverbSend != nullptr
                ? std::min (2, reverbSend->getNumChannels())
                : 0;
        const auto trimTarget =
            familyOutputTrim (activeFamily());
        std::size_t gainEvent = 0;

        for (std::size_t sample = 0;
             sample < numSamples;
             ++sample)
        {
            while (gainEvent < gainEventCount_
                   && gainEvents_[gainEvent].sampleOffset
                        <= sample)
            {
                const auto& event = gainEvents_[gainEvent];
                if (event.type == AdapterEventType::volume)
                    midiVolumeTarget_ = event.target;
                else
                    midiExpressionTarget_ = event.target;
                ++gainEvent;
            }

            midiVolumeCurrent_ +=
                controllerSmoothingStep_
                * (midiVolumeTarget_ - midiVolumeCurrent_);
            midiExpressionCurrent_ +=
                controllerSmoothingStep_
                * (midiExpressionTarget_
                   - midiExpressionCurrent_);
            familyTrimCurrent_ +=
                trimSmoothingStep_
                * (trimTarget - familyTrimCurrent_);
            const auto gain =
                midiVolumeCurrent_
                * midiExpressionCurrent_
                * familyTrimCurrent_;

            for (int channel = 0;
                 channel < mainChannels;
                 ++channel)
            {
                auto* data = output.getWritePointer (
                    channel,
                    static_cast<int> (startSample));
                auto value = data[sample] * gain;
                if (! std::isfinite (value))
                {
                    value = 0.0f;
                    ++invalidOutputSamples_;
                }
                data[sample] = value;
            }

            for (int channel = 0;
                 channel < sendChannels;
                 ++channel)
            {
                auto* data =
                    reverbSend->getWritePointer (
                        channel,
                        static_cast<int> (startSample));
                auto value = data[sample] * gain;
                if (! std::isfinite (value))
                {
                    value = 0.0f;
                    ++invalidOutputSamples_;
                }
                data[sample] = value;
            }
        }
        gainEventCount_ = 0;
    }

    void ChurchOrganLayerProcessor::clearBuffers (
        juce::AudioBuffer<float>& output,
        juce::AudioBuffer<float>* reverbSend) noexcept
    {
        output.clear();
        if (reverbSend != nullptr
            && reverbSend != &output)
        {
            reverbSend->clear();
        }
    }

    void ChurchOrganLayerProcessor::publishDiagnostics (
        const juce::AudioBuffer<float>& output) noexcept
    {
        auto peak = 0.0f;
        const auto channels =
            std::min (2, output.getNumChannels());
        for (int channel = 0;
             channel < channels;
             ++channel)
        {
            const auto* data =
                output.getReadPointer (channel);
            for (int sample = 0;
                 sample < output.getNumSamples();
                 ++sample)
            {
                peak = std::max (
                    peak, std::abs (data[sample]));
            }
        }

        diagnosticRenderedSamples_.store (
            renderedSamples_, std::memory_order_release);
        diagnosticReceivedMidiEvents_.store (
            receivedMidiEvents_, std::memory_order_release);
        diagnosticDroppedAdapterEvents_.store (
            droppedAdapterEvents_, std::memory_order_release);
        diagnosticDroppedEngineEvents_.store (
            droppedEngineEvents_, std::memory_order_release);
        diagnosticIgnoredChannelEvents_.store (
            ignoredChannelEvents_, std::memory_order_release);
        diagnosticUnsupportedPitchEvents_.store (
            unsupportedPitchEvents_, std::memory_order_release);
        diagnosticInvalidOutputSamples_.store (
            invalidOutputSamples_, std::memory_order_release);
        diagnosticQueuedEvents_.store (
            static_cast<std::uint32_t> (eventCount_),
            std::memory_order_release);
        diagnosticActiveNotes_.store (
            activeNoteCount(), std::memory_order_release);
        diagnosticPeakBits_.store (
            floatBits (peak), std::memory_order_release);
    }

    std::uint32_t
        ChurchOrganLayerProcessor::activeNoteCount() const noexcept
    {
        switch (activeFamily())
        {
            case ChurchOrganFamily::tonewheel:
            {
                const auto diagnostics =
                    tonewheel_.diagnostics();
                return diagnostics.activeUpperNotes
                    + diagnostics.activeLowerNotes
                    + diagnostics.activePedalNotes;
            }
            case ChurchOrganFamily::transistor:
                return static_cast<std::uint32_t> (
                    transistor_.activeUpperNoteCount());
            case ChurchOrganFamily::pipe:
                return static_cast<std::uint32_t> (
                    pipe_.activeDivisionVoiceCount());
            case ChurchOrganFamily::worshipHybrid:
                return hybrid_.diagnostics()
                    .activeTonewheelNotes;
        }
        return 0;
    }

    bool ChurchOrganLayerProcessor::stateOperationAllowed() const noexcept
    {
        return suspended_.load (
                   std::memory_order_acquire) != 0
            && insideProcess_.load (
                   std::memory_order_acquire) == 0;
    }

    juce::ValueTree ChurchOrganLayerProcessor::createState() const
    {
        juce::ValueTree root { stateType };
        root.setProperty (
            "schema",
            static_cast<int> (stateSchemaVersion),
            nullptr);
        root.setProperty (
            "family",
            static_cast<int> (activeFamily()),
            nullptr);
        root.setProperty (
            "quality",
            static_cast<int> (spec_.quality),
            nullptr);
        root.setProperty (
            "seed",
            static_cast<juce::int64> (
                spec_.deterministicSeed),
            nullptr);
        root.setProperty (
            "sampleRate", spec_.sampleRate, nullptr);
        root.setProperty (
            "maximumBlock",
            static_cast<int> (spec_.maximumBlockSize),
            nullptr);
        root.setProperty (
            "transistorEngineTrim",
            spec_.transistorEngineTrim,
            nullptr);
        root.setProperty (
            "pipeEngineTrim",
            spec_.pipeEngineTrim,
            nullptr);
        root.setProperty (
            "pipeTuningA",
            spec_.pipeTuningAHz,
            nullptr);
        root.setProperty (
            "pipeStereoWidth",
            spec_.pipeStereoWidth,
            nullptr);
        addGeneralState (root);
        addTransistorState (root);
        addPipeState (root);
        addHybridState (root);
        return root;
    }

    bool ChurchOrganLayerProcessor::restoreState (
        const juce::ValueTree& state)
    {
        if (! state.hasType (stateType)
            || ! stateOperationAllowed())
        {
            return false;
        }

        const auto schemaProperty =
            static_cast<juce::int64> (
                state.getProperty ("schema", 1));
        if (schemaProperty < 1
            || schemaProperty
                > static_cast<juce::int64> (stateSchemaVersion))
        {
            return false;
        }
        const auto schema =
            static_cast<std::uint32_t> (schemaProperty);

        auto restoredSpec = spec_;
        if (restoredSpec.sampleRate < minimumSampleRate)
            restoredSpec = {};
        auto restoredFamily = activeFamily();

        parameters_.resetToDefaults();
        initialiseAtomicDefaults();
        if (! restoreGeneralState (
                state,
                schema,
                restoredSpec,
                restoredFamily))
        {
            return false;
        }
        restoreTransistorState (state);
        restorePipeState (state);
        restoreHybridState (state);
        configurationRevision_.fetch_add (
            1, std::memory_order_release);

        const auto wasPrepared =
            prepared_.load (
                std::memory_order_acquire) != 0;
        restoredSpec.initialFamily = restoredFamily;
        if (wasPrepared)
        {
            if (! prepare (restoredSpec))
                return false;
            setProcessingSuspended (true);
        }
        else
        {
            spec_ = restoredSpec;
            activeFamily_.store (
                static_cast<std::uint32_t> (
                    restoredFamily),
                std::memory_order_release);
            parameters_.setPlain (
                OrganParameterId::family,
                static_cast<float> (restoredFamily));
            parameters_.setPlain (
                OrganParameterId::quality,
                static_cast<float> (
                    restoredSpec.quality));
        }
        return true;
    }

    void ChurchOrganLayerProcessor::getStateInformation (
        juce::MemoryBlock& destination) const
    {
        destination.reset();
        juce::MemoryOutputStream stream (
            destination, false);
        createState().writeToStream (stream);
    }

    bool ChurchOrganLayerProcessor::setStateInformation (
        const void* data,
        std::size_t size)
    {
        if (data == nullptr || size == 0)
            return false;
        const auto state =
            juce::ValueTree::readFromData (data, size);
        return state.isValid() && restoreState (state);
    }

    void ChurchOrganLayerProcessor::addGeneralState (
        juce::ValueTree& root) const
    {
        juce::ValueTree parametersState {
            generalStateType
        };
        const auto snapshot = parameters_.snapshot();
        parametersState.setProperty (
            "count",
            static_cast<int> (organParameterCount),
            nullptr);
        for (std::size_t index = 0;
             index < organParameterCount;
             ++index)
        {
            parametersState.setProperty (
                indexedIdentifier ("p", index),
                snapshot.values[index],
                nullptr);
        }
        parametersState.setProperty (
            "midiVolume",
            requestedMidiVolume_.load(),
            nullptr);
        parametersState.setProperty (
            "midiExpression",
            requestedMidiExpression_.load(),
            nullptr);
        for (std::size_t family = 0;
             family < familyCount;
             ++family)
        {
            parametersState.setProperty (
                indexedIdentifier ("trim", family),
                familyOutputTrims_[family].load(),
                nullptr);
        }
        root.addChild (parametersState, -1, nullptr);
    }

    void ChurchOrganLayerProcessor::addTransistorState (
        juce::ValueTree& root) const
    {
        juce::ValueTree state { transistorStateType };
        state.setProperty (
            "profile",
            static_cast<int> (
                transistorProfile_.load (
                    std::memory_order_acquire)),
            nullptr);
        state.setProperty (
            "vibratoEnabled",
            transistorVibratoEnabled_.load (
                std::memory_order_acquire) != 0,
            nullptr);
        // Version 1 marks the switch as intentional. Older states have no
        // marker because vibrato used to be enabled implicitly by the
        // factory, alongside the channel rotary.
        state.setProperty (
            "vibratoIntentVersion",
            static_cast<int> (transistorVibratoIntentVersion),
            nullptr);
        state.setProperty (
            "vibratoDepth",
            transistorVibratoDepth_.load(),
            nullptr);
        state.setProperty (
            "drive",
            transistorDrive_.load(),
            nullptr);
        state.setProperty (
            "stereoWidth",
            transistorStereoWidth_.load(),
            nullptr);
        for (std::size_t tab = 0;
             tab < transistorTabCount;
             ++tab)
        {
            state.setProperty (
                indexedIdentifier ("tab", tab),
                transistorTabs_[tab].load(),
                nullptr);
        }
        root.addChild (state, -1, nullptr);
    }

    void ChurchOrganLayerProcessor::addPipeState (
        juce::ValueTree& root) const
    {
        juce::ValueTree state { pipeStateType };
        state.setProperty (
            "temperament",
            static_cast<int> (
                pipeTemperament_.load (
                    std::memory_order_acquire)),
            nullptr);
        state.setProperty (
            "crescendo",
            pipeCrescendo_.load(),
            nullptr);
        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            state.setProperty (
                indexedIdentifier ("swell", manual),
                pipeSwell_[manual].load(),
                nullptr);
            state.setProperty (
                indexedIdentifier ("tremEnabled", manual),
                pipeTremulantEnabled_[manual].load (
                    std::memory_order_acquire) != 0,
                nullptr);
            state.setProperty (
                indexedIdentifier ("tremDepth", manual),
                pipeTremulantDepth_[manual].load(),
                nullptr);
            state.setProperty (
                indexedIdentifier ("tremRate", manual),
                pipeTremulantRate_[manual].load(),
                nullptr);

            for (std::size_t rank = 0;
                 rank < pipeOrganRankCount;
                 ++rank)
            {
                state.setProperty (
                    matrixIdentifier (
                        "rank", manual, rank),
                    pipeRankLevels_[manual][rank].load(),
                    nullptr);
            }
            for (std::size_t destination = 0;
                 destination < pipeOrganManualCount;
                 ++destination)
            {
                if (manual != destination)
                {
                    state.setProperty (
                        matrixIdentifier (
                            "coupler",
                            manual,
                            destination),
                        pipeCouplers_[manual][destination].load (
                            std::memory_order_acquire) != 0,
                        nullptr);
                }
            }
        }
        root.addChild (state, -1, nullptr);
    }

    void ChurchOrganLayerProcessor::addHybridState (
        juce::ValueTree& root) const
    {
        juce::ValueTree state { hybridStateType };
        const auto parameters = atomicAutoMixParameters();
        const auto controls = atomicAutoMixControls();
        state.setProperty (
            "enabled", parameters.enabled, nullptr);
        state.setProperty (
            "amount", parameters.amount, nullptr);
        state.setProperty (
            "reaction",
            parameters.reactionMilliseconds,
            nullptr);
        state.setProperty (
            "recovery",
            parameters.recoveryMilliseconds,
            nullptr);
        state.setProperty (
            "maximumAttenuation",
            parameters.maximumAttenuationDecibels,
            nullptr);
        state.setProperty (
            "pianoConflict",
            parameters.pianoConflict,
            nullptr);
        state.setProperty (
            "padConflict",
            parameters.externalPadConflict,
            nullptr);
        state.setProperty (
            "stringsConflict",
            parameters.externalStringsConflict,
            nullptr);
        state.setProperty (
            "masterProtection",
            parameters.masterProtection,
            nullptr);
        state.setProperty (
            "lowMidZone",
            parameters.lowMidZone,
            nullptr);
        state.setProperty (
            "midZone",
            parameters.midZone,
            nullptr);
        state.setProperty (
            "soloRoomLift",
            parameters.soloRoomLiftDecibels,
            nullptr);
        state.setProperty (
            "sidechainMask",
            static_cast<int> (parameters.sidechainMask),
            nullptr);
        state.setProperty (
            "pianoEnvelope",
            controls.pianoEnvelope,
            nullptr);
        state.setProperty (
            "padEnvelope",
            controls.externalPadEnvelope,
            nullptr);
        state.setProperty (
            "stringsEnvelope",
            controls.externalStringsEnvelope,
            nullptr);
        state.setProperty (
            "masterProximity",
            controls.masterProximity,
            nullptr);
        root.addChild (state, -1, nullptr);
    }

    bool ChurchOrganLayerProcessor::restoreGeneralState (
        const juce::ValueTree& root,
        std::uint32_t schema,
        ChurchOrganLayerPrepareSpec& restoredSpec,
        ChurchOrganFamily& restoredFamily)
    {
        const auto familyValue = stateUInt (
            root,
            "family",
            static_cast<std::uint32_t> (restoredFamily),
            static_cast<std::uint32_t> (
                ChurchOrganFamily::worshipHybrid));
        const auto qualityValue = stateUInt (
            root,
            "quality",
            static_cast<std::uint32_t> (
                restoredSpec.quality),
            static_cast<std::uint32_t> (
                ChurchOrganLayerQuality::ultra));
        restoredFamily =
            static_cast<ChurchOrganFamily> (familyValue);
        restoredSpec.quality =
            static_cast<ChurchOrganLayerQuality> (qualityValue);
        restoredSpec.deterministicSeed =
            stateSeed (
                root,
                restoredSpec.deterministicSeed != 0
                    ? restoredSpec.deterministicSeed
                    : defaultLayerSeed);
        restoredSpec.transistorEngineTrim =
            stateFloat (
                root,
                "transistorEngineTrim",
                restoredSpec.transistorEngineTrim,
                0.0f,
                0.25f);
        restoredSpec.pipeEngineTrim =
            stateFloat (
                root,
                "pipeEngineTrim",
                restoredSpec.pipeEngineTrim,
                0.0f,
                0.25f);
        restoredSpec.pipeTuningAHz =
            stateFloat (
                root,
                "pipeTuningA",
                restoredSpec.pipeTuningAHz,
                400.0f,
                480.0f);
        restoredSpec.pipeStereoWidth =
            stateFloat (
                root,
                "pipeStereoWidth",
                restoredSpec.pipeStereoWidth,
                0.0f,
                1.0f);

        const auto state =
            root.getChildWithName (generalStateType);
        if (state.isValid())
        {
            // A shorter packet from an older build must not inherit values
            // already present in the destination processor. Appended
            // parameters migrate deterministically to catalog defaults.
            for (std::size_t index = 0;
                 index < organParameterCount;
                 ++index)
            {
                const auto id =
                    static_cast<OrganParameterId> (index);
                parameters_.setPlain (
                    id, descriptor (id).defaultValue);
            }

            const auto encodedCount = stateUInt (
                state,
                "count",
                0,
                static_cast<std::uint32_t> (
                    organParameterCount));
            const auto count = std::min<std::size_t> (
                encodedCount, organParameterCount);
            for (std::size_t index = 0;
                 index < count;
                 ++index)
            {
                const auto id =
                    static_cast<OrganParameterId> (index);
                const auto value = stateFloat (
                    state,
                    indexedIdentifier ("p", index),
                    descriptor (id).defaultValue,
                    descriptor (id).minimum,
                    descriptor (id).maximum);
                parameters_.setPlain (id, value);
            }
            requestedMidiVolume_.store (
                stateFloat (
                    state,
                    "midiVolume",
                    1.0f,
                    0.0f,
                    1.0f));
            requestedMidiExpression_.store (
                stateFloat (
                    state,
                    "midiExpression",
                    1.0f,
                    0.0f,
                    1.0f));
            for (std::size_t family = 0;
                 family < familyCount;
                 ++family)
            {
                familyOutputTrims_[family].store (
                    stateFloat (
                        state,
                        indexedIdentifier ("trim", family),
                        1.0f,
                        0.0f,
                        2.0f));
            }
        }
        else if (schema == 1)
        {
            parameters_.setPlain (
                OrganParameterId::performanceAttackMs, 0.0f);
            parameters_.setPlain (
                OrganParameterId::performanceReleaseMs, 0.0f);
            const auto legacyGain = stateFloat (
                root,
                "outputGain",
                1.0f,
                0.0f,
                2.0f);
            for (auto& trim : familyOutputTrims_)
                trim.store (legacyGain);
            requestedMidiVolume_.store (
                stateFloat (
                    root,
                    "midiVolume",
                    1.0f,
                    0.0f,
                    1.0f));
            requestedMidiExpression_.store (
                stateFloat (
                    root,
                    "midiExpression",
                    1.0f,
                    0.0f,
                    1.0f));
        }
        return validFamily (restoredFamily)
            && validQuality (restoredSpec.quality);
    }

    void ChurchOrganLayerProcessor::restoreTransistorState (
        const juce::ValueTree& root)
    {
        const auto state =
            root.getChildWithName (transistorStateType);
        if (! state.isValid())
            return;
        transistorProfile_.store (
            stateUInt (
                state,
                "profile",
                0,
                static_cast<std::uint32_t> (
                    TransistorOrganProfile::germanCombo)),
            std::memory_order_release);
        const auto vibratoEnabled = static_cast<bool> (
            state.getProperty (
                "vibratoEnabled",
                transistorDefaultVibratoEnabled));
        const auto vibratoDepth = stateFloat (
            state,
            "vibratoDepth",
            transistorDefaultVibratoDepth,
            0.0f,
            1.0f);
        const auto vibratoIntentVersion = stateUInt (
            state,
            "vibratoIntentVersion",
            0,
            transistorVibratoIntentVersion);
        const bool legacyFactoryVibrato =
            vibratoIntentVersion == 0
            && vibratoEnabled
            && std::abs (
                   vibratoDepth
                   - transistorDefaultVibratoDepth)
                   <= legacyVibratoDepthTolerance;
        transistorVibratoEnabled_.store (
            vibratoEnabled && ! legacyFactoryVibrato ? 1u : 0u,
            std::memory_order_release);
        transistorVibratoDepth_.store (vibratoDepth);
        transistorDrive_.store (
            stateFloat (
                state, "drive",
                0.18f, 0.0f, 1.0f));
        transistorStereoWidth_.store (
            stateFloat (
                state, "stereoWidth",
                0.22f, 0.0f, 1.0f));
        for (std::size_t tab = 0;
             tab < transistorTabCount;
             ++tab)
        {
            transistorTabs_[tab].store (
                stateFloat (
                    state,
                    indexedIdentifier ("tab", tab),
                    defaultTransistorTabs[tab],
                    0.0f,
                    1.0f));
        }
    }

    void ChurchOrganLayerProcessor::restorePipeState (
        const juce::ValueTree& root)
    {
        const auto state =
            root.getChildWithName (pipeStateType);
        if (! state.isValid())
            return;
        pipeTemperament_.store (
            stateUInt (
                state,
                "temperament",
                static_cast<std::uint32_t> (
                    PipeOrganTemperament::churchWell),
                static_cast<std::uint32_t> (
                    PipeOrganTemperament::pureChoir)),
            std::memory_order_release);
        pipeCrescendo_.store (
            stateFloat (
                state, "crescendo",
                0.0f, 0.0f, 1.0f));
        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            pipeSwell_[manual].store (
                stateFloat (
                    state,
                    indexedIdentifier ("swell", manual),
                    defaultPipeSwell[manual],
                    0.0f,
                    1.0f));
            pipeTremulantEnabled_[manual].store (
                static_cast<bool> (
                    state.getProperty (
                        indexedIdentifier (
                            "tremEnabled", manual),
                        false))
                    ? 1u
                    : 0u,
                std::memory_order_release);
            pipeTremulantDepth_[manual].store (
                stateFloat (
                    state,
                    indexedIdentifier (
                        "tremDepth", manual),
                    0.42f,
                    0.0f,
                    1.0f));
            pipeTremulantRate_[manual].store (
                stateFloat (
                    state,
                    indexedIdentifier (
                        "tremRate", manual),
                    defaultPipeTremulantRates[manual],
                    0.1f,
                    12.0f));
            for (std::size_t rank = 0;
                 rank < pipeOrganRankCount;
                 ++rank)
            {
                const auto fallback =
                    pipeRankLevels_[manual][rank].load();
                pipeRankLevels_[manual][rank].store (
                    stateFloat (
                        state,
                        matrixIdentifier (
                            "rank", manual, rank),
                        fallback,
                        0.0f,
                        1.0f));
            }
            for (std::size_t destination = 0;
                 destination < pipeOrganManualCount;
                 ++destination)
            {
                if (manual == destination)
                    continue;
                pipeCouplers_[manual][destination].store (
                    static_cast<bool> (
                        state.getProperty (
                            matrixIdentifier (
                                "coupler",
                                manual,
                                destination),
                            false))
                        ? 1u
                        : 0u,
                    std::memory_order_release);
            }
        }
    }

    void ChurchOrganLayerProcessor::restoreHybridState (
        const juce::ValueTree& root)
    {
        const auto state =
            root.getChildWithName (hybridStateType);
        if (! state.isValid())
            return;
        AutoMixParameters parameters;
        parameters.enabled = static_cast<bool> (
            state.getProperty ("enabled", true));
        parameters.amount = stateFloat (
            state, "amount", 0.32f, 0.0f, 1.0f);
        parameters.reactionMilliseconds = stateFloat (
            state, "reaction", 720.0f, 250.0f, 5000.0f);
        parameters.recoveryMilliseconds = stateFloat (
            state, "recovery", 1900.0f, 800.0f, 10000.0f);
        parameters.maximumAttenuationDecibels = stateFloat (
            state, "maximumAttenuation", 3.0f, 0.0f, 6.0f);
        parameters.pianoConflict = stateFloat (
            state, "pianoConflict", 0.72f, 0.0f, 1.0f);
        parameters.externalPadConflict = stateFloat (
            state, "padConflict", 0.86f, 0.0f, 1.0f);
        parameters.externalStringsConflict = stateFloat (
            state, "stringsConflict", 0.58f, 0.0f, 1.0f);
        parameters.masterProtection = stateFloat (
            state, "masterProtection", 0.90f, 0.0f, 1.0f);
        parameters.lowMidZone = stateFloat (
            state, "lowMidZone", 0.65f, 0.0f, 1.0f);
        parameters.midZone = stateFloat (
            state, "midZone", 0.72f, 0.0f, 1.0f);
        parameters.soloRoomLiftDecibels = stateFloat (
            state, "soloRoomLift", 0.45f, 0.0f, 1.0f);
        parameters.sidechainMask = stateUInt (
            state,
            "sidechainMask",
            autoMixAllSources,
            autoMixAllSources);
        setHybridAutoMixParameters (parameters);

        AutoMixControlFrame controls;
        controls.pianoEnvelope = stateFloat (
            state, "pianoEnvelope", 0.0f, 0.0f, 1.0f);
        controls.externalPadEnvelope = stateFloat (
            state, "padEnvelope", 0.0f, 0.0f, 1.0f);
        controls.externalStringsEnvelope = stateFloat (
            state, "stringsEnvelope", 0.0f, 0.0f, 1.0f);
        controls.masterProximity = stateFloat (
            state, "masterProximity", 0.0f, 0.0f, 1.0f);
        setHybridAutoMixControlFrame (controls);
    }

    ChurchOrganLayerDiagnostics
        ChurchOrganLayerProcessor::diagnostics() const noexcept
    {
        ChurchOrganLayerDiagnostics result;
        result.renderedSamples =
            diagnosticRenderedSamples_.load (
                std::memory_order_acquire);
        result.receivedMidiEvents =
            diagnosticReceivedMidiEvents_.load (
                std::memory_order_acquire);
        result.droppedAdapterEvents =
            diagnosticDroppedAdapterEvents_.load (
                std::memory_order_acquire);
        result.droppedEngineEvents =
            diagnosticDroppedEngineEvents_.load (
                std::memory_order_acquire);
        result.ignoredChannelEvents =
            diagnosticIgnoredChannelEvents_.load (
                std::memory_order_acquire);
        result.unsupportedPitchEvents =
            diagnosticUnsupportedPitchEvents_.load (
                std::memory_order_acquire);
        result.invalidOutputSamples =
            diagnosticInvalidOutputSamples_.load (
                std::memory_order_acquire);
        result.queuedAdapterEvents =
            diagnosticQueuedEvents_.load (
                std::memory_order_acquire);
        result.activeNotes =
            diagnosticActiveNotes_.load (
                std::memory_order_acquire);
        result.lastBlockPeak = bitsFloat (
            diagnosticPeakBits_.load (
                std::memory_order_acquire));
        result.midiVolume =
            requestedMidiVolume_.load (
                std::memory_order_acquire);
        result.midiExpression =
            requestedMidiExpression_.load (
                std::memory_order_acquire);
        result.family = activeFamily();
        result.familyOutputTrim =
            familyOutputTrim (result.family);
        result.prepared =
            prepared_.load (
                std::memory_order_acquire) != 0;
        result.suspended = isProcessingSuspended();
        result.pitchBendSupported = false;
        return result;
    }

    const ChurchOrganLayerPrepareSpec&
        ChurchOrganLayerProcessor::prepareSpec() const noexcept
    {
        return spec_;
    }

    TonewheelOrganEngine&
        ChurchOrganLayerProcessor::tonewheelEngine() noexcept
    {
        return tonewheel_;
    }

    TransistorOrganEngine&
        ChurchOrganLayerProcessor::transistorEngine() noexcept
    {
        return transistor_;
    }

    PipeOrganEngine&
        ChurchOrganLayerProcessor::pipeEngine() noexcept
    {
        return pipe_;
    }

    WorshipHybridEngine&
        ChurchOrganLayerProcessor::worshipHybridEngine() noexcept
    {
        return hybrid_;
    }
}
