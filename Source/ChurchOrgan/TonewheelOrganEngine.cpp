#include "TonewheelOrganEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace churchorgan
{
    namespace
    {
        constexpr std::array<float, 9> drawbarCurve {{
            0.0f,
            0.0891250938f,
            0.1258925412f,
            0.1778279410f,
            0.2511886432f,
            0.3548133892f,
            0.5011872336f,
            0.7079457844f,
            1.0f
        }};

        constexpr std::array<OrganParameterId, drawbarCount> upperDrawbars {{
            OrganParameterId::upper16,
            OrganParameterId::upper5AndOneThird,
            OrganParameterId::upper8,
            OrganParameterId::upper4,
            OrganParameterId::upper2AndTwoThirds,
            OrganParameterId::upper2,
            OrganParameterId::upper1AndThreeFifths,
            OrganParameterId::upper1AndOneThird,
            OrganParameterId::upper1
        }};

        constexpr std::array<OrganParameterId, drawbarCount> lowerDrawbars {{
            OrganParameterId::lower16,
            OrganParameterId::lower5AndOneThird,
            OrganParameterId::lower8,
            OrganParameterId::lower4,
            OrganParameterId::lower2AndTwoThirds,
            OrganParameterId::lower2,
            OrganParameterId::lower1AndThreeFifths,
            OrganParameterId::lower1AndOneThird,
            OrganParameterId::lower1
        }};

        float clamp01 (float value) noexcept
        {
            if (! std::isfinite (value))
                return 0.0f;

            return std::max (0.0f, std::min (1.0f, value));
        }
    }

    TonewheelOrganEngine::TonewheelOrganEngine() noexcept
    {
        for (auto& manual : manuals_)
            manual.activeSlots.fill (-1);
    }

    bool TonewheelOrganEngine::prepare (
        const TonewheelOrganPrepareSpec& requestedSpec) noexcept
    {
        prepared_ = false;

        if (! std::isfinite (requestedSpec.sampleRate)
             || requestedSpec.sampleRate < 32000.0
             || requestedSpec.sampleRate > 384000.0
             || requestedSpec.maximumBlockSize == 0
             || requestedSpec.maximumBlockSize > maximumBlockCapacity)
            return false;

        spec_ = requestedSpec;
        if (spec_.deterministicSeed == 0)
            spec_.deterministicSeed = 0x4348555243484f52ULL;

        if (! tonewheels_.prepare (
                spec_.sampleRate,
                spec_.deterministicSeed,
                spec_.quality == OrganQualityProfile::eco ? 0.0f
                                                         : 0.12f))
            return false;

        for (std::size_t note = 0; note < midiNoteCount; ++note)
            for (std::size_t drawbar = 0; drawbar < drawbarCount; ++drawbar)
            {
                const auto mapped = HarmonicFoldback::map (
                    drawbar, static_cast<int> (note));

                if (! mapped.valid
                     || mapped.wheelIndex < 0
                     || mapped.wheelIndex >= HarmonicFoldback::wheelCount)
                    return false;

                wheelMap_[note][drawbar] =
                    static_cast<std::uint8_t> (mapped.wheelIndex);
            }

        MultiContactParameters contactParameters;
        if (spec_.quality == OrganQualityProfile::eco)
        {
            contactParameters.timingJitterMilliseconds = 0.08f;
            contactParameters.bounceAmount = 0.025f;
        }

        KeyClickParameters clickParameters;
        if (spec_.quality == OrganQualityProfile::eco)
            clickParameters.highCutHz = 5600.0f;

        for (std::size_t manual = 0; manual < manualCount; ++manual)
        {
            const auto seed =
                spec_.deterministicSeed
                ^ (0x9e3779b97f4a7c15ULL * (manual + 1));

            if (! manuals_[manual].contacts.prepare (
                    spec_.sampleRate, seed, contactParameters)
                 || ! manuals_[manual].clicks.prepare (
                    spec_.sampleRate,
                    seed ^ 0x434c49434b534545ULL,
                    clickParameters))
                return false;

            ScannerPrepareSpec scannerSpec;
            scannerSpec.sampleRate = spec_.sampleRate;
            scannerSpec.maximumBlockSize = spec_.maximumBlockSize;
            scannerSpec.deterministicSeed = static_cast<std::uint32_t> (
                seed ^ (seed >> 32));
            scannerSpec.initialPhase =
                0.137f + static_cast<float> (manual) * 0.173f;

            if (! scanners_[manual].prepare (scannerSpec))
                return false;
        }

        HarmonicPercussionParameters percussionParameters;
        if (! percussion_.prepare (
                spec_.sampleRate, percussionParameters))
            return false;

        TonewheelCharacterParameters characterParameters;
        if (! character_.prepare (
                spec_.sampleRate,
                spec_.deterministicSeed ^ 0x4348415241435445ULL,
                characterParameters))
            return false;

        OrganAmpPrepareSpec ampSpec;
        ampSpec.sampleRate = spec_.sampleRate;
        ampSpec.maximumBlockSize = spec_.maximumBlockSize;
        ampSpec.oversampling =
            spec_.quality == OrganQualityProfile::ultra
                ? OrganAmpOversampling::x4
                : OrganAmpOversampling::x2;
        if (! amp_.prepare (ampSpec))
            return false;

        RotaryPrepareSpec rotarySpec;
        rotarySpec.sampleRate = spec_.sampleRate;
        rotarySpec.maximumBlockSize = spec_.maximumBlockSize;
        if (! rotary_.prepare (rotarySpec))
            return false;

        parameters_.resetToDefaults();
        parameters_.setPlain (
            OrganParameterId::quality,
            static_cast<float> (spec_.quality));
        const auto snapshot = parameters_.snapshot();
        lastParameterRevision_ = parameters_.revision();

        drawbarSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 18.0f);
        controlSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 12.0f);
        expressionCurrent_ =
            expressionTarget_ = snapshot.get (OrganParameterId::expression);
        masterGainCurrent_ =
            masterGainTarget_ = decibelsToGain (
                snapshot.get (OrganParameterId::masterGainDb));
        midiVolumeCurrent_ = midiVolumeTarget_ = 1.0f;
        autoHeadroomCurrent_ = 1.0f;

        updateBlockTargets (snapshot);
        for (std::size_t manual = 0; manual < manualCount; ++manual)
            drawbarCurrentGains_[manual] =
                drawbarTargetGains_[manual];

        prepared_ = true;
        reset();
        return true;
    }

    void TonewheelOrganEngine::reset() noexcept
    {
        eventCount_ = 0;
        droppedEvents_ = 0;
        tonewheels_.resetPhases();
        character_.reset();
        percussion_.reset();
        amp_.reset();
        rotary_.reset();

        for (std::size_t manual = 0; manual < manualCount; ++manual)
        {
            auto& state = manuals_[manual];
            state.contacts.reset();
            state.clicks.reset();
            state.activeNotes.fill (0);
            state.activeSlots.fill (-1);
            state.velocities.fill (0);
            state.physicallyHeld.fill (false);
            state.sustained.fill (false);
            state.activeCount = 0;
            state.sustainDown = false;
            scanners_[manual].reset();
        }

        pedalPhysicallyHeld_.fill (false);
        pedalVelocities_.fill (0);
        monophonicPedalNote_ = -1;
        percussionEnabled_ = false;

        const auto snapshot = parameters_.snapshot();
        lastParameterRevision_ = parameters_.revision();
        expressionCurrent_ =
            expressionTarget_ = snapshot.get (OrganParameterId::expression);
        masterGainCurrent_ =
            masterGainTarget_ = decibelsToGain (
                snapshot.get (OrganParameterId::masterGainDb));
        midiVolumeCurrent_ = midiVolumeTarget_ = 1.0f;
        autoHeadroomCurrent_ = 1.0f;

        totalRenderedSamples_.store (0, std::memory_order_relaxed);
        diagnosticsDroppedEvents_.store (0, std::memory_order_relaxed);
        diagnosticsInvalidOutput_.store (0, std::memory_order_relaxed);
        diagnosticsActiveUpper_.store (0, std::memory_order_relaxed);
        diagnosticsActiveLower_.store (0, std::memory_order_relaxed);
        diagnosticsActivePedal_.store (0, std::memory_order_relaxed);
        diagnosticsQueuedEvents_.store (0, std::memory_order_relaxed);
        diagnosticsPeakBits_.store (0, std::memory_order_relaxed);
        diagnosticsHornSpeedBits_.store (0, std::memory_order_relaxed);
        diagnosticsDrumSpeedBits_.store (0, std::memory_order_relaxed);
    }

    bool TonewheelOrganEngine::render (
        AudioBlockView output) noexcept
    {
        if (! prepared_
             || output.left == nullptr
             || output.numSamples == 0
             || output.numSamples > spec_.maximumBlockSize
             || output.numSamples > maximumBlockCapacity)
            return false;

        const auto parameterRevision = parameters_.revision();
        if (parameterRevision != lastParameterRevision_)
        {
            const auto snapshot = parameters_.snapshot();
            updateBlockTargets (snapshot);
            // If a writer changes another parameter while this snapshot is
            // being read, revision() will still differ on the next block and
            // the newer value cannot be skipped.
            lastParameterRevision_ = parameterRevision;
        }

        for (std::size_t manual = 0; manual < manualCount; ++manual)
        {
            std::fill_n (
                manualLeft_[manual].data(), output.numSamples, 0.0f);
            std::fill_n (
                manualRight_[manual].data(), output.numSamples, 0.0f);
        }

        std::size_t processedEvents = 0;

        for (std::size_t sample = 0; sample < output.numSamples; ++sample)
        {
            while (processedEvents < eventCount_
                    && events_[processedEvents].sampleOffset <= sample)
            {
                applyEvent (events_[processedEvents]);
                ++processedEvents;
            }

            for (std::size_t manual = 0; manual < manualCount; ++manual)
                for (std::size_t drawbar = 0;
                     drawbar < drawbarCount;
                     ++drawbar)
                    drawbarCurrentGains_[manual][drawbar] +=
                        (drawbarTargetGains_[manual][drawbar]
                         - drawbarCurrentGains_[manual][drawbar])
                        * drawbarSmoothingCoefficient_;

            expressionCurrent_ +=
                (expressionTarget_ - expressionCurrent_)
                * controlSmoothingCoefficient_;
            midiVolumeCurrent_ +=
                (midiVolumeTarget_ - midiVolumeCurrent_)
                * controlSmoothingCoefficient_;

            const auto active = totalActiveNotes();
            const float headroomTarget =
                1.0f / std::sqrt (
                    std::max (1.0f, static_cast<float> (active)));
            autoHeadroomCurrent_ +=
                (headroomTarget - autoHeadroomCurrent_)
                * controlSmoothingCoefficient_;

            character_.beginSample (tonewheels_);

            for (std::size_t manual = 0; manual < manualCount; ++manual)
            {
                manuals_[manual].contacts.advance();
                float value = renderManualSample (manual, clickAmount_);

                if (manual == manualIndex (Manual::upper))
                {
                    value += renderPercussionSample();

                    // Leakage/hum/mechanical character colours notes and
                    // their contact release, but must not leave the ChurchDAW
                    // master behaving like an open microphone while idle.
                    if (active != 0)
                        value += character_.leakageSample();
                }

                const float performanceGain =
                    baseOutputTrim_
                    * expressionCurrent_
                    * midiVolumeCurrent_
                    * autoHeadroomCurrent_;
                value *= performanceGain;
                manualLeft_[manual][sample] = value;
                manualRight_[manual][sample] = value;
            }

            tonewheels_.advance();
        }

        const auto remainingEvents = eventCount_ - processedEvents;
        for (std::size_t event = 0; event < remainingEvents; ++event)
        {
            events_[event] = events_[processedEvents + event];
            events_[event].sampleOffset -=
                static_cast<std::uint32_t> (output.numSamples);
        }
        eventCount_ = remainingEvents;

        for (std::size_t manual = 0; manual < manualCount; ++manual)
            scanners_[manual].process (
                manualLeft_[manual].data(),
                manualRight_[manual].data(),
                output.numSamples);

        constexpr std::array<float, manualCount> manualLevels {{
            1.0f, 0.86f, 0.72f
        }};

        for (std::size_t sample = 0; sample < output.numSamples; ++sample)
        {
            float left = 0.0f;
            float right = 0.0f;

            for (std::size_t manual = 0; manual < manualCount; ++manual)
            {
                left += manualLeft_[manual][sample] * manualLevels[manual];
                right += manualRight_[manual][sample] * manualLevels[manual];
            }

            // O OrganAmpCab já contém o matching transformer/preamp estéreo.
            // Manter o transformador mono do módulo de caráter aqui criaria
            // dupla saturação e tornaria seu smoothing dependente do tamanho
            // do bloco, pois os manuais são escaneados em uma segunda etapa.
            mixLeft_[sample] = left;
            mixRight_[sample] = right;
        }

        amp_.process (
            mixLeft_.data(), mixRight_.data(), output.numSamples);
        rotary_.process (
            mixLeft_.data(), mixRight_.data(), output.numSamples);

        float peak = 0.0f;
        std::uint64_t invalid = 0;

        for (std::size_t sample = 0; sample < output.numSamples; ++sample)
        {
            masterGainCurrent_ +=
                (masterGainTarget_ - masterGainCurrent_)
                * controlSmoothingCoefficient_;

            float left = mixLeft_[sample] * masterGainCurrent_;
            float right = mixRight_[sample] * masterGainCurrent_;

            if (! std::isfinite (left))
            {
                left = 0.0f;
                ++invalid;
            }
            if (! std::isfinite (right))
            {
                right = 0.0f;
                ++invalid;
            }

            peak = std::max (
                peak, std::max (std::abs (left), std::abs (right)));

            if (output.right == nullptr || output.right == output.left)
                output.left[sample] = 0.5f * (left + right);
            else
            {
                output.left[sample] = left;
                output.right[sample] = right;
            }
        }

        totalRenderedSamples_.fetch_add (
            output.numSamples, std::memory_order_relaxed);
        publishDiagnostics (peak, invalid);
        return true;
    }

    bool TonewheelOrganEngine::queueNoteOn (
        Manual manual,
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (velocity <= 0)
            return queueNoteOff (manual, note, sampleOffset);

        if (! validManual (manual) || ! validNote (note))
        {
            ++droppedEvents_;
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

    bool TonewheelOrganEngine::queueNoteOff (
        Manual manual,
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual) || ! validNote (note))
        {
            ++droppedEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::noteOff;
        event.manual = manual;
        event.note = static_cast<std::uint8_t> (note);
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool TonewheelOrganEngine::queueAllNotesOff (
        Manual manual,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual))
        {
            ++droppedEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::allNotesOff;
        event.manual = manual;
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool TonewheelOrganEngine::queueExpression (
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

    bool TonewheelOrganEngine::queueVolume (
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

    bool TonewheelOrganEngine::queueSustain (
        Manual manual,
        bool down,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual))
        {
            ++droppedEvents_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::sustain;
        event.manual = manual;
        event.velocity = down ? 127 : 0;
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    OrganParameterStore& TonewheelOrganEngine::parameters() noexcept
    {
        return parameters_;
    }

    const OrganParameterStore&
        TonewheelOrganEngine::parameters() const noexcept
    {
        return parameters_;
    }

    TonewheelOrganDiagnostics
        TonewheelOrganEngine::diagnostics() const noexcept
    {
        TonewheelOrganDiagnostics result;
        result.totalRenderedSamples =
            totalRenderedSamples_.load (std::memory_order_acquire);
        result.droppedEvents =
            diagnosticsDroppedEvents_.load (std::memory_order_acquire);
        result.invalidOutputSamples =
            diagnosticsInvalidOutput_.load (std::memory_order_acquire);
        result.activeUpperNotes =
            diagnosticsActiveUpper_.load (std::memory_order_acquire);
        result.activeLowerNotes =
            diagnosticsActiveLower_.load (std::memory_order_acquire);
        result.activePedalNotes =
            diagnosticsActivePedal_.load (std::memory_order_acquire);
        result.queuedEvents =
            diagnosticsQueuedEvents_.load (std::memory_order_acquire);
        result.lastBlockPeak = bitsFloat (
            diagnosticsPeakBits_.load (std::memory_order_acquire));
        result.rotaryHornSpeedHz = bitsFloat (
            diagnosticsHornSpeedBits_.load (std::memory_order_acquire));
        result.rotaryDrumSpeedHz = bitsFloat (
            diagnosticsDrumSpeedBits_.load (std::memory_order_acquire));
        return result;
    }

    const TonewheelOrganPrepareSpec&
        TonewheelOrganEngine::prepareSpec() const noexcept
    {
        return spec_;
    }

    float TonewheelOrganEngine::drawbarGain (
        float positionZeroToEight) noexcept
    {
        if (! std::isfinite (positionZeroToEight))
            return 0.0f;

        const auto clamped = std::max (
            0.0f, std::min (8.0f, positionZeroToEight));
        const auto lower = static_cast<std::size_t> (clamped);

        if (lower >= drawbarCurve.size() - 1)
            return drawbarCurve.back();

        const auto fraction =
            clamped - static_cast<float> (lower);
        return drawbarCurve[lower]
               + fraction
                     * (drawbarCurve[lower + 1]
                        - drawbarCurve[lower]);
    }

    std::size_t TonewheelOrganEngine::manualIndex (
        Manual manual) noexcept
    {
        return static_cast<std::size_t> (manual);
    }

    bool TonewheelOrganEngine::validManual (
        Manual manual) noexcept
    {
        return manualIndex (manual) < manualCount;
    }

    bool TonewheelOrganEngine::validNote (int note) noexcept
    {
        return note >= 0 && note < static_cast<int> (midiNoteCount);
    }

    int TonewheelOrganEngine::eventPriority (
        EventType type) noexcept
    {
        // Keep the controller/source order for every equal-offset event.
        // The sole exception is a panic/all-notes-off: it must be the last
        // event applied at that sample so a simultaneous note-on cannot
        // survive it.
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

    float TonewheelOrganEngine::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        const double samples = std::max (
            1.0,
            sampleRate * static_cast<double> (milliseconds) * 0.001);
        return static_cast<float> (1.0 - std::exp (-1.0 / samples));
    }

    float TonewheelOrganEngine::decibelsToGain (
        float decibels) noexcept
    {
        if (! std::isfinite (decibels))
            return 1.0f;
        return std::pow (10.0f, decibels * 0.05f);
    }

    std::uint32_t TonewheelOrganEngine::floatBits (
        float value) noexcept
    {
        std::uint32_t bits = 0;
        std::memcpy (&bits, &value, sizeof (bits));
        return bits;
    }

    float TonewheelOrganEngine::bitsFloat (
        std::uint32_t value) noexcept
    {
        float result = 0.0f;
        std::memcpy (&result, &value, sizeof (result));
        return result;
    }

    bool TonewheelOrganEngine::queueEvent (
        const NoteEvent& event) noexcept
    {
        if (eventCount_ >= events_.size())
        {
            ++droppedEvents_;
            return false;
        }

        std::size_t insertion = eventCount_;
        while (insertion > 0)
        {
            const auto& previous = events_[insertion - 1];
            const bool shouldMove =
                previous.sampleOffset > event.sampleOffset
                || (previous.sampleOffset == event.sampleOffset
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

    void TonewheelOrganEngine::applyEvent (
        const NoteEvent& event) noexcept
    {
        switch (event.type)
        {
            case EventType::noteOn:
                applyNoteOn (
                    event.manual, event.note, event.velocity);
                break;
            case EventType::noteOff:
                applyNoteOff (event.manual, event.note);
                break;
            case EventType::allNotesOff:
                applyAllNotesOff (event.manual);
                break;
            case EventType::expression:
                expressionTarget_ =
                    static_cast<float> (event.velocity) / 127.0f;
                parameters_.setPlain (
                    OrganParameterId::expression, expressionTarget_);
                break;
            case EventType::volume:
                midiVolumeTarget_ =
                    static_cast<float> (event.velocity) / 127.0f;
                break;
            case EventType::sustain:
                applySustain (event.manual, event.velocity >= 64);
                break;
        }
    }

    void TonewheelOrganEngine::applyNoteOn (
        Manual manual,
        int note,
        int velocity) noexcept
    {
        if (manual == Manual::pedal)
        {
            auto& state = manuals_[manualIndex (Manual::pedal)];
            const auto noteIndex = static_cast<std::size_t> (note);
            const bool wasSustained =
                state.sustained[noteIndex]
                && ! pedalPhysicallyHeld_[noteIndex];

            pedalPhysicallyHeld_[noteIndex] = true;
            pedalVelocities_[noteIndex] =
                static_cast<std::uint8_t> (velocity);
            state.sustained[noteIndex] = false;

            // Rearticule a mesma nota quando ela estava apenas presa pelo
            // sustain. refreshPedalVoices() nao cria outra voz para uma tecla
            // que ja esta ativa, portanto sem isto a segunda batida parecia
            // engolida ate o pedal ser solto.
            if (wasSustained && state.physicallyHeld[noteIndex])
                activateManualNote (state, note, velocity, true);

            refreshPedalVoices();
            return;
        }

        auto& state = manuals_[manualIndex (manual)];
        state.physicallyHeld[static_cast<std::size_t> (note)] = true;
        state.sustained[static_cast<std::size_t> (note)] = false;
        activateManualNote (state, note, velocity, true);

        if (manual == Manual::upper && percussionEnabled_)
            percussion_.noteOn (note, velocity);
    }

    void TonewheelOrganEngine::applyNoteOff (
        Manual manual,
        int note) noexcept
    {
        if (manual == Manual::pedal)
        {
            auto& state = manuals_[manualIndex (Manual::pedal)];
            const auto noteIndex = static_cast<std::size_t> (note);
            pedalPhysicallyHeld_[noteIndex] = false;

            if (state.sustainDown)
                state.sustained[noteIndex] = true;
            else
                state.sustained[noteIndex] = false;

            refreshPedalVoices();
            return;
        }

        auto& state = manuals_[manualIndex (manual)];
        state.physicallyHeld[static_cast<std::size_t> (note)] = false;
        state.clicks.noteOff (note);

        if (state.sustainDown)
            state.sustained[static_cast<std::size_t> (note)] = true;
        else
            releaseManualNote (state, note, false);

        if (manual == Manual::upper)
            percussion_.noteOff (note);
    }

    void TonewheelOrganEngine::applyAllNotesOff (
        Manual manual) noexcept
    {
        if (manual == Manual::pedal)
        {
            pedalPhysicallyHeld_.fill (false);
            monophonicPedalNote_ = -1;
        }

        auto& state = manuals_[manualIndex (manual)];
        state.physicallyHeld.fill (false);
        state.sustained.fill (false);
        state.sustainDown = false;
        state.contacts.allNotesOff();
        state.clicks.allNotesOff();

        if (manual == Manual::upper)
            percussion_.allNotesOff();
    }

    void TonewheelOrganEngine::applySustain (
        Manual manual,
        bool down) noexcept
    {
        auto& state = manuals_[manualIndex (manual)];

        if (state.sustainDown == down)
            return;

        state.sustainDown = down;
        if (down)
            return;

        if (manual == Manual::pedal)
        {
            // No manual de pedal, state.physicallyHeld representa as vozes
            // selecionadas pelo modo mono/poly; a verdade fisica vive no
            // array separado. Usar state.physicallyHeld aqui fazia toda nota
            // sustentada parecer ainda pressionada e o CC64-off nunca a
            // liberava.
            for (std::size_t note = 0; note < midiNoteCount; ++note)
                if (state.sustained[note]
                     && ! pedalPhysicallyHeld_[note])
                    state.sustained[note] = false;

            refreshPedalVoices();
            return;
        }

        for (std::size_t note = 0; note < midiNoteCount; ++note)
            if (state.sustained[note] && ! state.physicallyHeld[note])
            {
                state.sustained[note] = false;
                releaseManualNote (
                    state, static_cast<int> (note), false);
            }
    }

    void TonewheelOrganEngine::activateManualNote (
        ManualState& state,
        int note,
        int velocity,
        bool triggerClick) noexcept
    {
        state.velocities[static_cast<std::size_t> (note)] =
            static_cast<std::uint8_t> (velocity);
        ensureManualActive (state, note);
        state.contacts.noteOn (note, velocity);
        if (triggerClick)
            state.clicks.noteOn (note, velocity);
    }

    void TonewheelOrganEngine::releaseManualNote (
        ManualState& state,
        int note,
        bool triggerClick) noexcept
    {
        state.contacts.noteOff (note);
        if (triggerClick)
            state.clicks.noteOff (note);
    }

    void TonewheelOrganEngine::ensureManualActive (
        ManualState& state,
        int note) noexcept
    {
        const auto noteIndex = static_cast<std::size_t> (note);
        if (state.activeSlots[noteIndex] >= 0)
            return;

        if (state.activeCount >= state.activeNotes.size())
        {
            ++droppedEvents_;
            return;
        }

        state.activeNotes[state.activeCount] =
            static_cast<std::uint8_t> (note);
        state.activeSlots[noteIndex] =
            static_cast<std::int16_t> (state.activeCount);
        ++state.activeCount;
    }

    void TonewheelOrganEngine::removeManualActiveAt (
        ManualState& state,
        std::size_t activeIndex) noexcept
    {
        const auto last = state.activeCount - 1;
        const auto removedNote = state.activeNotes[activeIndex];

        if (activeIndex != last)
        {
            const auto movedNote = state.activeNotes[last];
            state.activeNotes[activeIndex] = movedNote;
            state.activeSlots[movedNote] =
                static_cast<std::int16_t> (activeIndex);
        }

        state.activeSlots[removedNote] = -1;
        state.activeNotes[last] = 0;
        --state.activeCount;
    }

    void TonewheelOrganEngine::refreshPedalVoices() noexcept
    {
        auto& state = manuals_[manualIndex (Manual::pedal)];

        if (pedalPolyphonic_)
        {
            for (std::size_t note = 0; note < midiNoteCount; ++note)
            {
                const bool shouldHold =
                    pedalPhysicallyHeld_[note]
                    || state.sustained[note];
                if (shouldHold && ! state.physicallyHeld[note])
                {
                    state.physicallyHeld[note] = true;
                    activateManualNote (
                        state,
                        static_cast<int> (note),
                        pedalVelocities_[note],
                        true);
                }
                else if (! shouldHold && state.physicallyHeld[note])
                {
                    state.physicallyHeld[note] = false;
                    releaseManualNote (
                        state, static_cast<int> (note), true);
                }
            }
            monophonicPedalNote_ = -1;
            return;
        }

        int selected = -1;

        // Uma tecla fisicamente pressionada sempre vence uma nota que ficou
        // apenas latched. Mantem a regra historica de prioridade para a nota
        // mais grave quando mais de um pedal fisico esta abaixado.
        for (std::size_t note = 0; note < midiNoteCount; ++note)
            if (pedalPhysicallyHeld_[note])
            {
                selected = static_cast<int> (note);
                break;
            }

        // Sem tecla fisica, conserve primeiro a voz monofonica atual se ela
        // esta sustentada. Isso evita saltar para outra nota antiga latched ao
        // receber o note-off da nota mais recente.
        if (selected < 0
             && monophonicPedalNote_ >= 0
             && state.sustained[
                    static_cast<std::size_t> (monophonicPedalNote_)])
            selected = monophonicPedalNote_;

        if (selected < 0)
            for (std::size_t note = 0; note < midiNoteCount; ++note)
                if (state.sustained[note])
                {
                    selected = static_cast<int> (note);
                    break;
                }

        if (selected == monophonicPedalNote_)
            return;

        if (monophonicPedalNote_ >= 0)
        {
            state.physicallyHeld[
                static_cast<std::size_t> (monophonicPedalNote_)] = false;
            releaseManualNote (
                state, monophonicPedalNote_, true);
        }

        monophonicPedalNote_ = selected;
        if (selected >= 0)
        {
            state.physicallyHeld[
                static_cast<std::size_t> (selected)] = true;
            activateManualNote (
                state,
                selected,
                pedalVelocities_[static_cast<std::size_t> (selected)],
                true);
        }
    }

    void TonewheelOrganEngine::updatePedalMode (
        bool polyphonic) noexcept
    {
        if (pedalPolyphonic_ == polyphonic)
            return;

        pedalPolyphonic_ = polyphonic;
        auto& state = manuals_[manualIndex (Manual::pedal)];
        state.contacts.allNotesOff();
        state.clicks.allNotesOff();
        state.physicallyHeld.fill (false);
        monophonicPedalNote_ = -1;
        refreshPedalVoices();
    }

    void TonewheelOrganEngine::updateBlockTargets (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        for (std::size_t drawbar = 0; drawbar < drawbarCount; ++drawbar)
        {
            drawbarTargetGains_[manualIndex (Manual::upper)][drawbar] =
                drawbarGain (snapshot.get (upperDrawbars[drawbar]));
            drawbarTargetGains_[manualIndex (Manual::lower)][drawbar] =
                drawbarGain (snapshot.get (lowerDrawbars[drawbar]));
            drawbarTargetGains_[manualIndex (Manual::pedal)][drawbar] = 0.0f;
        }

        drawbarTargetGains_[manualIndex (Manual::pedal)][0] =
            drawbarGain (snapshot.get (OrganParameterId::pedal16));
        drawbarTargetGains_[manualIndex (Manual::pedal)][2] =
            drawbarGain (snapshot.get (OrganParameterId::pedal8));

        expressionTarget_ = clamp01 (
            snapshot.get (OrganParameterId::expression));
        masterGainTarget_ = decibelsToGain (
            snapshot.get (OrganParameterId::masterGainDb));
        clickAmount_ = clamp01 (
            snapshot.get (OrganParameterId::keyClick));
        updatePedalMode (
            snapshot.get (OrganParameterId::pedalMode) >= 0.5f);

        const auto performanceAttack =
            snapshot.get (OrganParameterId::performanceAttackMs);
        const auto performanceRelease =
            snapshot.get (OrganParameterId::performanceReleaseMs);
        for (auto& manual : manuals_)
        {
            manual.contacts.setPerformanceEnvelopeMilliseconds (
                performanceAttack,
                performanceRelease);
        }

        configureCharacter (snapshot);
        configurePercussion (snapshot);
        configureScanner (snapshot);
        configureAmp (snapshot);
        configureRotary (snapshot);
    }

    void TonewheelOrganEngine::configureCharacter (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        const int profile = static_cast<int> (
            snapshot.get (OrganParameterId::tonewheelProfile) + 0.5f);
        TonewheelCharacterParameters parameters;
        parameters.age = clamp01 (snapshot.get (OrganParameterId::age));
        parameters.leakage = clamp01 (
            snapshot.get (OrganParameterId::leakage));
        parameters.crosstalk = clamp01 (
            snapshot.get (OrganParameterId::crosstalk));
        parameters.hum = clamp01 (snapshot.get (OrganParameterId::hum));
        parameters.transformer = clamp01 (
            snapshot.get (OrganParameterId::transformer));
        parameters.mechanicalNoise =
            profile == 2 ? 0.16f
            : profile == 1 ? 0.07f
                           : 0.0f;

        if (profile == 0)
        {
            parameters.age *= 0.45f;
            parameters.leakage *= 0.55f;
            parameters.crosstalk *= 0.55f;
        }
        else if (profile == 1)
        {
            parameters.age = clamp01 (parameters.age + 0.20f);
            parameters.leakage = clamp01 (parameters.leakage + 0.10f);
            parameters.crosstalk = clamp01 (parameters.crosstalk + 0.08f);
        }
        else if (profile == 2)
        {
            parameters.age = clamp01 (parameters.age + 0.38f);
            parameters.leakage = clamp01 (parameters.leakage + 0.22f);
            parameters.crosstalk = clamp01 (
                parameters.crosstalk + 0.20f);
        }

        character_.setParameters (parameters);
    }

    void TonewheelOrganEngine::configurePercussion (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        const bool enabled =
            snapshot.get (OrganParameterId::percussionEnabled) >= 0.5f;
        if (percussionEnabled_ && ! enabled)
            percussion_.allNotesOff();
        percussionEnabled_ = enabled;

        percussion_.setHarmonic (
            snapshot.get (OrganParameterId::percussionHarmonic) >= 0.5f
                ? PercussionHarmonic::third
                : PercussionHarmonic::second);
        percussion_.setVolume (
            snapshot.get (OrganParameterId::percussionVolume) >= 0.5f
                ? PercussionVolume::normal
                : PercussionVolume::soft);
        percussion_.setTiming (
            snapshot.get (OrganParameterId::percussionDecay) >= 0.5f
                ? PercussionTiming::slow
                : PercussionTiming::fast);
        percussion_.setTriggerMode (
            snapshot.get (OrganParameterId::percussionRetrigger) >= 0.5f
                ? PercussionTriggerMode::retrigger
                : PercussionTriggerMode::singleTrigger);
    }

    void TonewheelOrganEngine::configureScanner (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        ScannerParameters parameters;
        parameters.mode = static_cast<ScannerMode> (
            std::max (0, std::min (
                6,
                static_cast<int> (
                    snapshot.get (OrganParameterId::scannerMode) + 0.5f))));
        parameters.mix = clamp01 (
            snapshot.get (OrganParameterId::scannerMix));
        parameters.age = clamp01 (
            snapshot.get (OrganParameterId::age));
        parameters.worshipIntensity = clamp01 (
            snapshot.get (OrganParameterId::worshipIntensity));
        parameters.stereoSpread =
            spec_.quality == OrganQualityProfile::eco ? 0.08f : 0.16f;

        scanners_[manualIndex (Manual::upper)].setParameters (parameters);
        scanners_[manualIndex (Manual::lower)].setParameters (parameters);
        parameters.mode = ScannerMode::bypass;
        scanners_[manualIndex (Manual::pedal)].setParameters (parameters);
    }

    void TonewheelOrganEngine::configureAmp (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        const int profile = static_cast<int> (
            snapshot.get (OrganParameterId::ampProfile) + 0.5f);
        OrganAmpParameters parameters;
        parameters.profile =
            profile == 0 ? OrganAmpProfile::clean
            : profile == 1 ? OrganAmpProfile::warm
            : profile >= 4 ? OrganAmpProfile::worshipSmooth
                           : OrganAmpProfile::driven;
        parameters.drive = clamp01 (
            snapshot.get (OrganParameterId::drive));
        if (profile == 3)
            parameters.drive = clamp01 (parameters.drive + 0.18f);
        parameters.bias = 0.24f + 0.30f * clamp01 (
            snapshot.get (OrganParameterId::transformer));
        parameters.sag = 0.16f + 0.38f * clamp01 (
            snapshot.get (OrganParameterId::age));
        parameters.tone = clamp01 (
            snapshot.get (OrganParameterId::tone));
        parameters.worshipIntensity = clamp01 (
            snapshot.get (OrganParameterId::worshipIntensity));
        amp_.setParameters (parameters);
    }

    void TonewheelOrganEngine::configureRotary (
        const OrganParameterSnapshot& snapshot) noexcept
    {
        RotaryParameters parameters;
        parameters.speedMode = static_cast<RotarySpeedMode> (
            std::max (0, std::min (
                3,
                static_cast<int> (
                    snapshot.get (OrganParameterId::rotaryMode) + 0.5f))));
        parameters.mix = clamp01 (
            snapshot.get (OrganParameterId::rotaryMix));
        parameters.hornLevel = decibelsToGain (
            snapshot.get (OrganParameterId::rotaryHornLevel));
        parameters.drumLevel = decibelsToGain (
            snapshot.get (OrganParameterId::rotaryDrumLevel));
        parameters.crossoverHz =
            snapshot.get (OrganParameterId::rotaryCrossoverHz);
        parameters.micDistance = clamp01 (
            snapshot.get (OrganParameterId::rotaryMicDistance));
        parameters.stereoWidth = clamp01 (
            snapshot.get (OrganParameterId::rotaryStereoWidth));
        parameters.earlyReflections = 0.32f * clamp01 (
            snapshot.get (OrganParameterId::rotaryRoom));
        parameters.worshipIntensity = clamp01 (
            snapshot.get (OrganParameterId::worshipIntensity));
        parameters.perspective =
            parameters.worshipIntensity > 0.72f
                ? RotaryPerspective::wideWorship
                : RotaryPerspective::balanced;
        rotary_.setParameters (parameters);
    }

    float TonewheelOrganEngine::renderManualSample (
        std::size_t manual,
        float clickAmount) noexcept
    {
        auto& state = manuals_[manual];
        float mixed = 0.0f;
        std::size_t activeIndex = 0;

        while (activeIndex < state.activeCount)
        {
            const auto note =
                static_cast<int> (state.activeNotes[activeIndex]);

            if (! state.contacts.isAudible (note)
                 && ! state.physicallyHeld[static_cast<std::size_t> (note)]
                 && ! state.sustained[static_cast<std::size_t> (note)])
            {
                removeManualActiveAt (state, activeIndex);
                continue;
            }

            const auto& mapping =
                wheelMap_[static_cast<std::size_t> (note)];
            const float region =
                static_cast<float> (note) / 127.0f;
            float contactBusGain = 0.0f;

            for (std::size_t drawbar = 0;
                 drawbar < drawbarCount;
                 ++drawbar)
            {
                const auto gain = drawbarCurrentGains_[manual][drawbar];
                if (gain <= 0.000001f)
                    continue;

                const auto wheel = mapping[drawbar];
                const auto contact =
                    state.contacts.contactGain (note, drawbar);
                mixed += tonewheels_.sample (wheel) * contact * gain;
                contactBusGain += contact * gain;
            }

            // O wiring crosstalk entra uma vez no barramento da tecla, usando
            // vizinhança esparsa ao redor do registro de 8'. Isso preserva o
            // caráter regional sem consultar três vizinhos para cada um dos
            // nove drawbars.
            mixed += character_.crosstalkSample (
                         tonewheels_, mapping[2], region)
                     * std::min (2.0f, 0.22f * contactBusGain);

            ++activeIndex;
        }

        mixed += state.clicks.renderSample()
                 * clamp01 (clickAmount);
        return mixed;
    }

    float TonewheelOrganEngine::renderPercussionSample() noexcept
    {
        if (! percussionEnabled_)
            return 0.0f;

        const auto note = percussion_.triggeredMidiNote();
        if (! validNote (note))
            return percussion_.processSample (0.0f);

        const auto drawbar = percussion_.harmonicDrawbarIndex();
        if (drawbar >= drawbarCount)
            return percussion_.processSample (0.0f);

        const auto wheel =
            wheelMap_[static_cast<std::size_t> (note)][drawbar];
        return percussion_.processSample (tonewheels_.sample (wheel));
    }

    std::size_t TonewheelOrganEngine::totalActiveNotes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& manual : manuals_)
            total += manual.activeCount;
        return total;
    }

    void TonewheelOrganEngine::publishDiagnostics (
        float peak,
        std::uint64_t invalidOutputSamples) noexcept
    {
        diagnosticsDroppedEvents_.store (
            droppedEvents_, std::memory_order_release);
        diagnosticsInvalidOutput_.fetch_add (
            invalidOutputSamples, std::memory_order_release);
        diagnosticsActiveUpper_.store (
            static_cast<std::uint32_t> (
                manuals_[manualIndex (Manual::upper)].activeCount),
            std::memory_order_release);
        diagnosticsActiveLower_.store (
            static_cast<std::uint32_t> (
                manuals_[manualIndex (Manual::lower)].activeCount),
            std::memory_order_release);
        diagnosticsActivePedal_.store (
            static_cast<std::uint32_t> (
                manuals_[manualIndex (Manual::pedal)].activeCount),
            std::memory_order_release);
        diagnosticsQueuedEvents_.store (
            static_cast<std::uint32_t> (eventCount_),
            std::memory_order_release);
        diagnosticsPeakBits_.store (
            floatBits (peak), std::memory_order_release);

        const auto rotaryDiagnostics = rotary_.diagnostics();
        diagnosticsHornSpeedBits_.store (
            floatBits (rotaryDiagnostics.hornSpeedHz),
            std::memory_order_release);
        diagnosticsDrumSpeedBits_.store (
            floatBits (rotaryDiagnostics.drumSpeedHz),
            std::memory_order_release);
    }
}
