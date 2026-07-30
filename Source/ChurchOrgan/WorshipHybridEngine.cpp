#include "WorshipHybridEngine.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double minimumSampleRate = 32000.0;
        constexpr double maximumSampleRate = 192000.0;
        constexpr float hybridFamilyIndex = 3.0f;
        constexpr std::uint64_t defaultHybridSeed =
            0x4859425249444f52ULL;
        constexpr std::uint64_t tonewheelSeedSalt =
            0x544f4e4557484545ULL;
        constexpr std::uint64_t worshipSeedSalt =
            0x574f52534849504cULL;

        float cleanFilterMemory (float value) noexcept
        {
            return std::abs (value) < 1.0e-20f ? 0.0f : value;
        }

        float organBalanceForIntensity (float intensity) noexcept
        {
            // The macro opens the Worship bed without making the Tonewheel
            // appear to recede. Only the upper end receives a subtle trim.
            return 1.0f
                - 0.08f
                    * worshipdsp::smoothStep (
                        0.55f, 1.0f, intensity);
        }
    }

    bool WorshipHybridEngine::prepare (
        const WorshipHybridPrepareSpec& requested) noexcept
    {
        prepared_ = false;

        if (! std::isfinite (requested.sampleRate)
            || requested.sampleRate < minimumSampleRate
            || requested.sampleRate > maximumSampleRate
            || requested.maximumBlockSize == 0
            || requested.maximumBlockSize > maximumBlockCapacity)
        {
            return false;
        }

        spec_ = requested;
        if (spec_.quality != WorshipHybridQuality::eco
            && spec_.quality != WorshipHybridQuality::standard
            && spec_.quality != WorshipHybridQuality::ultra)
        {
            spec_.quality = WorshipHybridQuality::standard;
        }
        if (spec_.deterministicSeed == 0)
            spec_.deterministicSeed = defaultHybridSeed;

        activeState_ = 0;
        stateBReady_ = false;
        if (! prepareState (
                states_[activeState_],
                spec_.deterministicSeed))
        {
            return false;
        }

        low180Step_ =
            worshipdsp::filterStep (spec_.sampleRate, 180.0f);
        low900Step_ =
            worshipdsp::filterStep (spec_.sampleRate, 900.0f);
        low3200Step_ =
            worshipdsp::filterStep (spec_.sampleRate, 3200.0f);
        dcCoefficient_ = static_cast<float> (
            std::exp (
                -static_cast<double> (worshipdsp::twoPi)
                * 8.0
                / spec_.sampleRate));

        targetIntensity_ = clamp01 (targetIntensity_);
        targetOutputGain_ = worshipdsp::clamp (
            worshipdsp::finiteOr (targetOutputGain_, 0.92f),
            0.0f,
            1.5f);
        organBalance_.prepare (
            spec_.sampleRate,
            90.0f,
            organBalanceForIntensity (targetIntensity_));
        outputGain_.prepare (
            spec_.sampleRate,
            45.0f,
            targetOutputGain_);

        applyIntensityTo (
            states_[activeState_], targetIntensity_);
        states_[activeState_].autoMix.setParameters (
            autoMixParameters_);
        autoMixParameters_ =
            states_[activeState_].autoMix.parameters();
        states_[activeState_].autoMix.setControlFrame (
            autoMixControls_);
        autoMixControls_ =
            states_[activeState_].autoMix.controlFrame();

        eventCount_ = 0;
        renderedSamples_ = 0;
        droppedWrapperEvents_ = 0;
        droppedChildEvents_ = 0;
        invalidSamples_ = 0;
        diagnostics_ = {};
        clearPerformanceState();
        workLeft_.fill (0.0f);
        workRight_.fill (0.0f);
        sendLeft_.fill (0.0f);
        sendRight_.fill (0.0f);
        prepared_ = true;
        publishDiagnostics ({}, 0.0f, 0.0f, 0);
        return true;
    }

    void WorshipHybridEngine::reset() noexcept
    {
        eventCount_ = 0;
        renderedSamples_ = 0;
        droppedWrapperEvents_ = 0;
        droppedChildEvents_ = 0;
        invalidSamples_ = 0;
        diagnostics_ = {};
        clearPerformanceState();

        auto resetState = [] (EngineState& state) noexcept
        {
            if (! state.prepared)
                return;
            state.tonewheel.reset();
            state.worship.reset();
            state.autoMix.reset();
            state.leftFilter = {};
            state.rightFilter = {};
        };

        resetState (states_[activeState_]);
        if (stateBReady_)
            resetState (states_[1u - activeState_]);

        organBalance_.reset (
            organBalanceForIntensity (targetIntensity_));
        outputGain_.reset (targetOutputGain_);
        publishDiagnostics (
            states_[activeState_].autoMix.currentGains(),
            0.0f,
            0.0f,
            0);
    }

    bool WorshipHybridEngine::render (
        const WorshipHybridAudioBlock& output) noexcept
    {
        if (! prepared_
            || output.left == nullptr
            || output.numSamples == 0
            || output.numSamples > spec_.maximumBlockSize
            || output.numSamples > maximumBlockCapacity)
        {
            clearOutput (output);
            return false;
        }

        auto& state = states_[activeState_];
        std::size_t firstUnprocessed = 0;
        while (firstUnprocessed < eventCount_
               && events_[firstUnprocessed].sampleOffset
                    < output.numSamples)
        {
            if (! dispatchEvent (
                    state, events_[firstUnprocessed]))
            {
                ++droppedChildEvents_;
            }
            ++firstUnprocessed;
        }
        carryEvents (firstUnprocessed, output.numSamples);

        const auto gains =
            state.autoMix.advance (output.numSamples);
        state.worship.setAutoMixLayerGain (
            gains.worshipLayer);

        std::fill_n (
            sendLeft_.data(), output.numSamples, 0.0f);
        std::fill_n (
            sendRight_.data(), output.numSamples, 0.0f);

        if (! state.tonewheel.render ({
                workLeft_.data(),
                workRight_.data(),
                output.numSamples
            }))
        {
            clearOutput (output);
            return false;
        }

        for (std::size_t sample = 0;
             sample < output.numSamples;
             ++sample)
        {
            const auto balance = organBalance_.next();
            workLeft_[sample] =
                contextualOrganSample (
                    workLeft_[sample],
                    state.leftFilter,
                    gains)
                * balance;
            workRight_[sample] =
                contextualOrganSample (
                    workRight_[sample],
                    state.rightFilter,
                    gains)
                * balance;
        }

        state.worship.renderAdd ({
            workLeft_.data(),
            workRight_.data(),
            sendLeft_.data(),
            sendRight_.data(),
            output.numSamples
        });

        float mainPeak = 0.0f;
        float sendPeak = 0.0f;
        std::uint64_t blockInvalid = 0;
        const bool stereoMain =
            output.right != nullptr
            && output.right != output.left;
        const bool stereoSend =
            output.reverbSendLeft != nullptr
            && output.reverbSendRight != nullptr
            && output.reverbSendRight
                != output.reverbSendLeft;

        for (std::size_t sample = 0;
             sample < output.numSamples;
             ++sample)
        {
            const auto outputScale =
                outputGain_.next() * gains.output;
            auto left = workLeft_[sample] * outputScale;
            auto right = workRight_[sample] * outputScale;
            auto sendLeft =
                sendLeft_[sample]
                * outputScale
                * gains.room;
            auto sendRight =
                sendRight_[sample]
                * outputScale
                * gains.room;

            if (! std::isfinite (left))
            {
                left = 0.0f;
                ++blockInvalid;
            }
            if (! std::isfinite (right))
            {
                right = 0.0f;
                ++blockInvalid;
            }
            if (! std::isfinite (sendLeft))
            {
                sendLeft = 0.0f;
                ++blockInvalid;
            }
            if (! std::isfinite (sendRight))
            {
                sendRight = 0.0f;
                ++blockInvalid;
            }

            left = softProtect (
                dcBlock (left, state.leftFilter),
                0.82f,
                0.985f);
            right = softProtect (
                dcBlock (right, state.rightFilter),
                0.82f,
                0.985f);
            sendLeft = softProtect (
                sendLeft, 0.34f, 0.50f);
            sendRight = softProtect (
                sendRight, 0.34f, 0.50f);

            mainPeak = std::max (
                mainPeak,
                std::max (
                    std::abs (left),
                    std::abs (right)));
            sendPeak = std::max (
                sendPeak,
                std::max (
                    std::abs (sendLeft),
                    std::abs (sendRight)));

            output.left[sample] = stereoMain
                ? left
                : 0.5f * (left + right);
            if (stereoMain)
                output.right[sample] = right;

            if (output.reverbSendLeft != nullptr)
            {
                output.reverbSendLeft[sample] =
                    stereoSend
                        ? sendLeft
                        : 0.5f * (sendLeft + sendRight);
            }
            if (stereoSend)
                output.reverbSendRight[sample] = sendRight;
        }

        renderedSamples_ += output.numSamples;
        invalidSamples_ += blockInvalid;
        publishDiagnostics (
            gains, mainPeak, sendPeak, blockInvalid);
        return true;
    }

    bool WorshipHybridEngine::queueNoteOn (
        Manual manual,
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (velocity <= 0)
            return queueNoteOff (
                manual, note, sampleOffset);
        if (! validManual (manual) || ! validNote (note))
        {
            ++droppedWrapperEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::noteOn;
        event.manual = manual;
        event.note = static_cast<std::uint8_t> (note);
        event.velocity = static_cast<std::uint8_t> (
            std::max (1, std::min (127, velocity)));
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool WorshipHybridEngine::queueNoteOff (
        Manual manual,
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual) || ! validNote (note))
        {
            ++droppedWrapperEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::noteOff;
        event.manual = manual;
        event.note = static_cast<std::uint8_t> (note);
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool WorshipHybridEngine::queueSustain (
        Manual manual,
        bool down,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual))
        {
            ++droppedWrapperEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::sustain;
        event.manual = manual;
        event.velocity = down ? 127u : 0u;
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool WorshipHybridEngine::queueAllNotesOff (
        Manual manual,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual))
        {
            ++droppedWrapperEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::allNotesOff;
        event.manual = manual;
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool WorshipHybridEngine::queueAllNotesOffAll (
        std::uint32_t sampleOffset) noexcept
    {
        if (! prepared_ || eventCount_ + 3 > events_.size())
        {
            ++droppedWrapperEvents_;
            return false;
        }

        return queueAllNotesOff (
                   Manual::upper, sampleOffset)
            && queueAllNotesOff (
                   Manual::lower, sampleOffset)
            && queueAllNotesOff (
                   Manual::pedal, sampleOffset);
    }

    bool WorshipHybridEngine::queueExpression (
        int valueZeroTo127,
        std::uint32_t sampleOffset) noexcept
    {
        NoteEvent event;
        event.type = EventType::expression;
        event.velocity = static_cast<std::uint8_t> (
            std::max (0, std::min (127, valueZeroTo127)));
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool WorshipHybridEngine::queueVolume (
        int valueZeroTo127,
        std::uint32_t sampleOffset) noexcept
    {
        NoteEvent event;
        event.type = EventType::volume;
        event.velocity = static_cast<std::uint8_t> (
            std::max (0, std::min (127, valueZeroTo127)));
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    void WorshipHybridEngine::setWorshipIntensity (
        float intensity) noexcept
    {
        targetIntensity_ = clamp01 (intensity);
        organBalance_.setTarget (
            organBalanceForIntensity (targetIntensity_));
        if (prepared_)
            applyIntensityTo (
                states_[activeState_], targetIntensity_);
        if (stateBReady_)
            applyIntensityTo (
                states_[1u - activeState_],
                targetIntensity_);
    }

    float WorshipHybridEngine::worshipIntensity() const noexcept
    {
        return targetIntensity_;
    }

    void WorshipHybridEngine::setOutputGain (
        float linearGain) noexcept
    {
        targetOutputGain_ = worshipdsp::clamp (
            worshipdsp::finiteOr (
                linearGain, targetOutputGain_),
            0.0f,
            1.5f);
        outputGain_.setTarget (targetOutputGain_);
    }

    void WorshipHybridEngine::setAutoMixParameters (
        const AutoMixParameters& parameters) noexcept
    {
        autoMixParameters_ = parameters;
        if (prepared_)
        {
            states_[activeState_].autoMix.setParameters (
                autoMixParameters_);
            autoMixParameters_ =
                states_[activeState_].autoMix.parameters();
        }
        if (stateBReady_)
        {
            states_[1u - activeState_].autoMix.setParameters (
                autoMixParameters_);
        }
    }

    void WorshipHybridEngine::setAutoMixControlFrame (
        const AutoMixControlFrame& controls) noexcept
    {
        autoMixControls_ = controls;
        if (prepared_)
        {
            states_[activeState_].autoMix.setControlFrame (
                autoMixControls_);
            autoMixControls_ =
                states_[activeState_].autoMix.controlFrame();
        }
        if (stateBReady_)
        {
            states_[1u - activeState_].autoMix.setControlFrame (
                autoMixControls_);
        }
    }

    bool WorshipHybridEngine::setLayerManualMask (
        std::uint8_t mask) noexcept
    {
        constexpr auto validBits = static_cast<std::uint8_t> (
            hybridMirrorUpper
            | hybridMirrorLower
            | hybridMirrorPedal);
        if ((mask & ~validBits) != 0
            || eventCount_ != 0
            || ! performanceIsIdle())
        {
            return false;
        }
        layerManualMask_ = mask;
        return true;
    }

    std::uint8_t
        WorshipHybridEngine::layerManualMask() const noexcept
    {
        return layerManualMask_;
    }

    TonewheelOrganEngine&
        WorshipHybridEngine::tonewheel() noexcept
    {
        return states_[activeState_].tonewheel;
    }

    WorshipLayerEngine&
        WorshipHybridEngine::worshipLayer() noexcept
    {
        return states_[activeState_].worship;
    }

    AutoMixContext& WorshipHybridEngine::autoMix() noexcept
    {
        return states_[activeState_].autoMix;
    }

    const TonewheelOrganEngine&
        WorshipHybridEngine::tonewheel() const noexcept
    {
        return states_[activeState_].tonewheel;
    }

    const WorshipLayerEngine&
        WorshipHybridEngine::worshipLayer() const noexcept
    {
        return states_[activeState_].worship;
    }

    const AutoMixContext&
        WorshipHybridEngine::autoMix() const noexcept
    {
        return states_[activeState_].autoMix;
    }

    bool WorshipHybridEngine::prepareStateB (
        std::uint64_t deterministicSeed) noexcept
    {
        if (! prepared_)
            return false;

        auto& destination = states_[1u - activeState_];
        const auto seed =
            deterministicSeed != 0
                ? deterministicSeed
                : (spec_.deterministicSeed
                   ^ 0x4253544154455345ULL);
        stateBReady_ = false;
        if (! prepareState (destination, seed))
            return false;

        cloneActiveConfigurationTo (destination);
        destination.leftFilter = {};
        destination.rightFilter = {};
        stateBReady_ = true;
        publishDiagnostics (
            states_[activeState_].autoMix.currentGains(),
            diagnostics_.lastMainPeak,
            diagnostics_.lastReverbSendPeak,
            0);
        return true;
    }

    bool WorshipHybridEngine::stateBReady() const noexcept
    {
        return stateBReady_;
    }

    TonewheelOrganEngine*
        WorshipHybridEngine::stateBTonewheel() noexcept
    {
        return stateBReady_
            ? &states_[1u - activeState_].tonewheel
            : nullptr;
    }

    WorshipLayerEngine*
        WorshipHybridEngine::stateBWorshipLayer() noexcept
    {
        return stateBReady_
            ? &states_[1u - activeState_].worship
            : nullptr;
    }

    AutoMixContext*
        WorshipHybridEngine::stateBAutoMix() noexcept
    {
        return stateBReady_
            ? &states_[1u - activeState_].autoMix
            : nullptr;
    }

    void WorshipHybridEngine::discardStateB() noexcept
    {
        if (stateBReady_)
        {
            auto& state = states_[1u - activeState_];
            state.tonewheel.reset();
            state.worship.reset();
            state.autoMix.reset();
            state.leftFilter = {};
            state.rightFilter = {};
        }
        stateBReady_ = false;
        diagnostics_.preparedStateB = false;
    }

    WorshipHybridDiagnostics
        WorshipHybridEngine::diagnostics() const noexcept
    {
        return diagnostics_;
    }

    bool WorshipHybridEngine::validManual (
        Manual manual) noexcept
    {
        return manual == Manual::upper
            || manual == Manual::lower
            || manual == Manual::pedal;
    }

    bool WorshipHybridEngine::validNote (int note) noexcept
    {
        return note >= 0
            && note < static_cast<int> (midiNoteCount);
    }

    std::size_t WorshipHybridEngine::manualIndex (
        Manual manual) noexcept
    {
        return static_cast<std::size_t> (manual);
    }

    std::uint8_t WorshipHybridEngine::manualBit (
        Manual manual) noexcept
    {
        return static_cast<std::uint8_t> (
            1u << manualIndex (manual));
    }

    int WorshipHybridEngine::eventPriority (
        EventType type) noexcept
    {
        // Preserve source order at equal sample offsets. A panic/all-off is
        // intentionally delayed until every other event at that sample has
        // run, guaranteeing that it wins over a simultaneous note-on.
        switch (type)
        {
            case EventType::allNotesOff: return 1;
            case EventType::noteOff:
            case EventType::sustain:
            case EventType::expression:
            case EventType::volume:
            case EventType::noteOn:      return 0;
        }
        return 0;
    }

    OrganQualityProfile
        WorshipHybridEngine::tonewheelQuality (
            WorshipHybridQuality quality) noexcept
    {
        switch (quality)
        {
            case WorshipHybridQuality::eco:
                return OrganQualityProfile::eco;
            case WorshipHybridQuality::ultra:
                return OrganQualityProfile::ultra;
            case WorshipHybridQuality::standard:
            default:
                return OrganQualityProfile::standard;
        }
    }

    WorshipQuality WorshipHybridEngine::worshipQuality (
        WorshipHybridQuality quality) noexcept
    {
        switch (quality)
        {
            case WorshipHybridQuality::eco:
                return WorshipQuality::eco;
            case WorshipHybridQuality::ultra:
                return WorshipQuality::ultra;
            case WorshipHybridQuality::standard:
            default:
                return WorshipQuality::standard;
        }
    }

    float WorshipHybridEngine::clamp01 (float value) noexcept
    {
        return worshipdsp::clamp (
            worshipdsp::finiteOr (value, 0.35f),
            0.0f,
            1.0f);
    }

    float WorshipHybridEngine::softProtect (
        float value,
        float threshold,
        float ceiling) noexcept
    {
        value = worshipdsp::finiteOr (value, 0.0f);
        const auto magnitude = std::abs (value);
        if (magnitude <= threshold)
            return value;

        const auto safeCeiling =
            std::max (threshold + 0.001f, ceiling);
        const auto scaled =
            (magnitude - threshold)
            / (safeCeiling - threshold);
        const auto protectedMagnitude =
            threshold
            + (safeCeiling - threshold)
                * scaled / (1.0f + scaled);
        return std::copysign (protectedMagnitude, value);
    }

    bool WorshipHybridEngine::prepareState (
        EngineState& state,
        std::uint64_t seed) noexcept
    {
        state.prepared = false;
        if (seed == 0)
            seed = defaultHybridSeed;

        TonewheelOrganPrepareSpec tonewheelSpec;
        tonewheelSpec.sampleRate = spec_.sampleRate;
        tonewheelSpec.maximumBlockSize =
            spec_.maximumBlockSize;
        tonewheelSpec.deterministicSeed =
            seed ^ tonewheelSeedSalt;
        tonewheelSpec.quality =
            tonewheelQuality (spec_.quality);

        WorshipPrepareSpec worshipSpec;
        worshipSpec.sampleRate = spec_.sampleRate;
        worshipSpec.maximumBlockSize =
            spec_.maximumBlockSize;
        worshipSpec.deterministicSeed =
            seed ^ worshipSeedSalt;
        worshipSpec.quality =
            worshipQuality (spec_.quality);

        AutoMixPrepareSpec autoMixSpec;
        autoMixSpec.sampleRate = spec_.sampleRate;
        autoMixSpec.maximumBlockSize =
            spec_.maximumBlockSize;

        if (! state.tonewheel.prepare (tonewheelSpec)
            || ! state.worship.prepare (worshipSpec)
            || ! state.autoMix.prepare (autoMixSpec))
        {
            return false;
        }

        state.tonewheel.parameters().setPlain (
            OrganParameterId::family, hybridFamilyIndex);
        state.tonewheel.parameters().setPlain (
            OrganParameterId::quality,
            static_cast<float> (spec_.quality));
        state.autoMix.setParameters (autoMixParameters_);
        state.autoMix.setControlFrame (autoMixControls_);
        applyIntensityTo (state, targetIntensity_);
        state.leftFilter = {};
        state.rightFilter = {};
        state.prepared = true;
        return true;
    }

    void WorshipHybridEngine::cloneActiveConfigurationTo (
        EngineState& destination) noexcept
    {
        const auto& source = states_[activeState_];
        const auto snapshot =
            source.tonewheel.parameters().snapshot();
        for (std::size_t index = 0;
             index < organParameterCount;
             ++index)
        {
            destination.tonewheel.parameters().setPlain (
                static_cast<OrganParameterId> (index),
                snapshot.values[index]);
        }
        destination.tonewheel.parameters().setPlain (
            OrganParameterId::family, hybridFamilyIndex);

        for (std::size_t index = 0;
             index < worshipLayerCount;
             ++index)
        {
            const auto layer =
                static_cast<WorshipLayer> (index);
            destination.worship.setLayerParameters (
                layer,
                source.worship.layerParameters (layer));
        }
        destination.worship.setWorshipIntensity (
            source.worship.targetWorshipIntensity());
        destination.autoMix.setParameters (
            source.autoMix.parameters());
        destination.autoMix.setControlFrame (
            source.autoMix.controlFrame());
    }

    void WorshipHybridEngine::applyIntensityTo (
        EngineState& state,
        float intensity) noexcept
    {
        intensity = clamp01 (intensity);
        state.tonewheel.parameters().setPlain (
            OrganParameterId::family, hybridFamilyIndex);
        state.tonewheel.parameters().setPlain (
            OrganParameterId::worshipIntensity,
            intensity);
        state.worship.setWorshipIntensity (intensity);
    }

    bool WorshipHybridEngine::queueEvent (
        const NoteEvent& event) noexcept
    {
        if (! prepared_ || eventCount_ >= events_.size())
        {
            ++droppedWrapperEvents_;
            return false;
        }

        std::size_t insertion = eventCount_;
        while (insertion > 0)
        {
            const auto& previous =
                events_[insertion - 1];
            const auto shouldMove =
                previous.sampleOffset > event.sampleOffset
                || (previous.sampleOffset
                        == event.sampleOffset
                    && eventPriority (previous.type)
                        > eventPriority (event.type));
            if (! shouldMove)
                break;
            events_[insertion] = previous;
            --insertion;
        }
        events_[insertion] = event;
        ++eventCount_;
        return true;
    }

    bool WorshipHybridEngine::dispatchEvent (
        EngineState& state,
        const NoteEvent& event) noexcept
    {
        bool accepted =
            dispatchTonewheelEvent (
                state.tonewheel, event);

        if (event.type == EventType::expression)
        {
            return dispatchWorshipEvent (
                       state.worship, event)
                && accepted;
        }
        if (event.type == EventType::volume)
            return accepted;

        const auto index = manualIndex (event.manual);
        auto& manualState = performance_[index];
        const auto note =
            static_cast<std::size_t> (event.note);

        switch (event.type)
        {
            case EventType::noteOn:
            {
                const auto wasHeld =
                    effectiveLayerHeld (event.note);
                manualState.physicallyHeld[note] = true;
                manualState.sustained[note] = false;
                manualState.velocities[note] =
                    event.velocity;
                const auto nowHeld =
                    effectiveLayerHeld (event.note);
                if (! wasHeld && nowHeld)
                {
                    accepted =
                        state.worship.queueNoteOn (
                            event.note,
                            event.velocity,
                            event.sampleOffset)
                        && accepted;
                }
                break;
            }

            case EventType::noteOff:
            {
                const auto wasHeld =
                    effectiveLayerHeld (event.note);
                manualState.physicallyHeld[note] = false;
                manualState.sustained[note] =
                    manualState.sustainDown;
                const auto nowHeld =
                    effectiveLayerHeld (event.note);
                if (wasHeld && ! nowHeld)
                {
                    accepted =
                        state.worship.queueNoteOff (
                            event.note,
                            event.sampleOffset)
                        && accepted;
                }
                break;
            }

            case EventType::sustain:
            {
                const auto down = event.velocity != 0;
                manualState.sustainDown = down;
                if (! down)
                {
                    for (std::size_t midiNote = 0;
                         midiNote < midiNoteCount;
                         ++midiNote)
                    {
                        const auto wasHeld =
                            effectiveLayerHeld (
                                static_cast<int> (midiNote));
                        if (! manualState.physicallyHeld[midiNote])
                            manualState.sustained[midiNote] = false;
                        const auto nowHeld =
                            effectiveLayerHeld (
                                static_cast<int> (midiNote));
                        if (wasHeld && ! nowHeld)
                        {
                            accepted =
                                state.worship.queueNoteOff (
                                    static_cast<int> (midiNote),
                                    event.sampleOffset)
                                && accepted;
                        }
                    }
                }
                break;
            }

            case EventType::allNotesOff:
            {
                for (std::size_t midiNote = 0;
                     midiNote < midiNoteCount;
                     ++midiNote)
                {
                    const auto wasHeld =
                        effectiveLayerHeld (
                            static_cast<int> (midiNote));
                    manualState.physicallyHeld[midiNote] = false;
                    manualState.sustained[midiNote] = false;
                    manualState.velocities[midiNote] = 0;
                    const auto nowHeld =
                        effectiveLayerHeld (
                            static_cast<int> (midiNote));
                    if (wasHeld && ! nowHeld)
                    {
                        accepted =
                            state.worship.queueNoteOff (
                                static_cast<int> (midiNote),
                                event.sampleOffset)
                            && accepted;
                    }
                }
                manualState.sustainDown = false;
                break;
            }

            case EventType::expression:
            case EventType::volume:
                break;
        }
        return accepted;
    }

    bool WorshipHybridEngine::dispatchTonewheelEvent (
        TonewheelOrganEngine& engine,
        const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case EventType::noteOn:
                return engine.queueNoteOn (
                    event.manual,
                    event.note,
                    event.velocity,
                    event.sampleOffset);
            case EventType::noteOff:
                return engine.queueNoteOff (
                    event.manual,
                    event.note,
                    event.sampleOffset);
            case EventType::allNotesOff:
                return engine.queueAllNotesOff (
                    event.manual,
                    event.sampleOffset);
            case EventType::expression:
                return engine.queueExpression (
                    event.velocity,
                    event.sampleOffset);
            case EventType::volume:
                return engine.queueVolume (
                    event.velocity,
                    event.sampleOffset);
            case EventType::sustain:
                return engine.queueSustain (
                    event.manual,
                    event.velocity != 0,
                    event.sampleOffset);
        }
        return false;
    }

    bool WorshipHybridEngine::dispatchWorshipEvent (
        WorshipLayerEngine& engine,
        const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case EventType::noteOn:
                return engine.queueNoteOn (
                    event.note,
                    event.velocity,
                    event.sampleOffset);
            case EventType::noteOff:
                return engine.queueNoteOff (
                    event.note,
                    event.sampleOffset);
            case EventType::allNotesOff:
                return engine.queueAllNotesOff (
                    event.sampleOffset);
            case EventType::expression:
                return engine.queueExpression (
                    static_cast<float> (event.velocity)
                        / 127.0f,
                    event.sampleOffset);
            case EventType::volume:
            case EventType::sustain:
                return true;
        }
        return false;
    }

    void WorshipHybridEngine::carryEvents (
        std::size_t firstUnprocessed,
        std::size_t renderedSamples) noexcept
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
                offset > renderedSamples
                    ? static_cast<std::uint32_t> (
                        offset - renderedSamples)
                    : 0u;
        }
        eventCount_ = remaining;
    }

    bool WorshipHybridEngine::layerMirrors (
        Manual manual) const noexcept
    {
        return (layerManualMask_ & manualBit (manual)) != 0;
    }

    bool WorshipHybridEngine::effectiveLayerHeld (
        int note) const noexcept
    {
        if (! validNote (note))
            return false;
        const auto index = static_cast<std::size_t> (note);
        for (std::size_t manual = 0;
             manual < performance_.size();
             ++manual)
        {
            const auto manualValue =
                static_cast<Manual> (manual);
            if (layerMirrors (manualValue)
                && (performance_[manual].physicallyHeld[index]
                    || performance_[manual].sustained[index]))
            {
                return true;
            }
        }
        return false;
    }

    void WorshipHybridEngine::clearPerformanceState() noexcept
    {
        performance_.fill ({});
    }

    bool WorshipHybridEngine::performanceIsIdle() const noexcept
    {
        for (const auto& manual : performance_)
        {
            for (std::size_t note = 0;
                 note < midiNoteCount;
                 ++note)
            {
                if (manual.physicallyHeld[note]
                    || manual.sustained[note])
                {
                    return false;
                }
            }
        }
        return true;
    }

    float WorshipHybridEngine::contextualOrganSample (
        float input,
        ContextFilterState& state,
        const AutoMixGains& gains) noexcept
    {
        input = worshipdsp::finiteOr (input, 0.0f);
        state.low180 +=
            low180Step_ * (input - state.low180);
        state.low900 +=
            low900Step_ * (input - state.low900);
        state.low3200 +=
            low3200Step_ * (input - state.low3200);

        const auto low = state.low180;
        const auto lowMid = state.low900 - state.low180;
        const auto mid = state.low3200 - state.low900;
        const auto high = input - state.low3200;
        const auto driveSafety =
            std::sqrt (
                worshipdsp::clamp (
                    worshipdsp::finiteOr (gains.drive, 1.0f),
                    0.0f,
                    1.25f));
        const auto result =
            (low
             + lowMid * gains.organLowMid
             + mid * gains.organMid
             + high)
            * driveSafety;

        state.low180 = cleanFilterMemory (state.low180);
        state.low900 = cleanFilterMemory (state.low900);
        state.low3200 = cleanFilterMemory (state.low3200);
        return worshipdsp::finiteOr (result, 0.0f);
    }

    float WorshipHybridEngine::dcBlock (
        float input,
        ContextFilterState& state) const noexcept
    {
        const auto output =
            input
            - state.dcInput
            + dcCoefficient_ * state.dcOutput;
        state.dcInput = input;
        state.dcOutput = cleanFilterMemory (
            worshipdsp::finiteOr (output, 0.0f));
        return state.dcOutput;
    }

    void WorshipHybridEngine::clearOutput (
        const WorshipHybridAudioBlock& output) noexcept
    {
        const auto safeSamples = std::min (
            output.numSamples, maximumBlockCapacity);
        if (output.left != nullptr)
            std::fill_n (output.left, safeSamples, 0.0f);
        if (output.right != nullptr
            && output.right != output.left)
        {
            std::fill_n (output.right, safeSamples, 0.0f);
        }
        if (output.reverbSendLeft != nullptr)
        {
            std::fill_n (
                output.reverbSendLeft,
                safeSamples,
                0.0f);
        }
        if (output.reverbSendRight != nullptr
            && output.reverbSendRight
                != output.reverbSendLeft)
        {
            std::fill_n (
                output.reverbSendRight,
                safeSamples,
                0.0f);
        }
    }

    void WorshipHybridEngine::publishDiagnostics (
        const AutoMixGains& gains,
        float mainPeak,
        float sendPeak,
        std::uint64_t) noexcept
    {
        const auto tonewheelDiagnostics =
            states_[activeState_].tonewheel.diagnostics();
        const auto worshipDiagnostics =
            states_[activeState_].worship.diagnostics();

        diagnostics_.renderedSamples = renderedSamples_;
        diagnostics_.droppedWrapperEvents =
            droppedWrapperEvents_;
        diagnostics_.droppedChildEvents =
            droppedChildEvents_;
        diagnostics_.invalidSamples = invalidSamples_;
        diagnostics_.queuedEvents =
            static_cast<std::uint32_t> (eventCount_);
        diagnostics_.activeTonewheelNotes =
            tonewheelDiagnostics.activeUpperNotes
            + tonewheelDiagnostics.activeLowerNotes
            + tonewheelDiagnostics.activePedalNotes;
        diagnostics_.activeWorshipVoices =
            worshipDiagnostics.heldVoices;
        diagnostics_.worshipTailVoices =
            worshipDiagnostics.tailVoices;
        diagnostics_.lastMainPeak = mainPeak;
        diagnostics_.lastReverbSendPeak = sendPeak;
        diagnostics_.worshipIntensity = targetIntensity_;
        diagnostics_.autoMixGains = gains;
        diagnostics_.preparedStateB = stateBReady_;
    }
}
