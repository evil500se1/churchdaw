#include "ChurchOrganCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace churchorgan
{
    namespace
    {
        constexpr std::array<float, 9> drawbarGainCurve {{
            0.0f,
            0.0891250938f, // -21 dB
            0.1258925412f, // -18 dB
            0.1778279410f, // -15 dB
            0.2511886432f, // -12 dB
            0.3548133892f, //  -9 dB
            0.5011872336f, //  -6 dB
            0.7079457844f, //  -3 dB
            1.0f
        }};
    }

    ChurchOrganCore::ChurchOrganCore() noexcept
    {
        activeSlots_.fill (-1);

        for (auto& position : drawbarPositionQ_)
            position.store (0, std::memory_order_relaxed);
    }

    bool ChurchOrganCore::prepare (
        const PrepareSpec& requestedSpec) noexcept
    {
        prepared_ = false;

        if (! std::isfinite (requestedSpec.sampleRate)
             || requestedSpec.sampleRate < 32000.0
             || requestedSpec.sampleRate > 384000.0
             || requestedSpec.maximumBlockSize == 0
             || requestedSpec.maximumBlockSize > 16384
             || ! std::isfinite (requestedSpec.outputTrim))
            return false;

        spec_ = requestedSpec;
        spec_.drawbarSmoothingMilliseconds = std::max (
            0.1f, std::min (
                500.0f, requestedSpec.drawbarSmoothingMilliseconds));
        spec_.contactAttackMilliseconds = std::max (
            0.05f, std::min (
                20.0f, requestedSpec.contactAttackMilliseconds));
        spec_.contactReleaseMilliseconds = std::max (
            0.05f, std::min (
                100.0f, requestedSpec.contactReleaseMilliseconds));
        spec_.outputTrim = std::max (
            0.0f, std::min (0.5f, requestedSpec.outputTrim));

        if (! tonewheels_.prepare (
                spec_.sampleRate,
                spec_.deterministicSeed,
                spec_.maximumWheelOffsetCents))
            return false;

        for (std::size_t note = 0; note < midiNoteCount; ++note)
            for (std::size_t drawbar = 0;
                 drawbar < drawbarCount;
                 ++drawbar)
            {
                const auto mapped = HarmonicFoldback::map (
                    drawbar, static_cast<int> (note));

                if (! mapped.valid
                     || mapped.wheelIndex < 0
                     || mapped.wheelIndex >=
                            HarmonicFoldback::wheelCount)
                    return false;

                wheelMap_[note][drawbar] =
                    static_cast<std::uint8_t> (mapped.wheelIndex);
            }

        drawbarSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate,
            spec_.drawbarSmoothingMilliseconds);
        contactAttackCoefficient_ = smoothingCoefficient (
            spec_.sampleRate,
            spec_.contactAttackMilliseconds);
        contactReleaseCoefficient_ = smoothingCoefficient (
            spec_.sampleRate,
            spec_.contactReleaseMilliseconds);
        outputTrim_ = spec_.outputTrim;

        for (std::size_t drawbar = 0;
             drawbar < drawbarCount;
             ++drawbar)
        {
            const float target = gainForDrawbarPosition (
                targetDrawbarPosition (drawbar));
            drawbarCurrentGain_[drawbar] = target;
            drawbarTargetGain_[drawbar] = target;
        }

        prepared_ = true;
        reset();
        return true;
    }

    void ChurchOrganCore::reset() noexcept
    {
        for (auto& note : notes_)
            note = {};

        activeNotes_.fill (0);
        activeSlots_.fill (-1);
        activeNoteCount_ = 0;
        eventCount_ = 0;
        droppedEventCount_ = 0;
        tonewheels_.resetPhases();

        totalRenderedSamples_.store (0, std::memory_order_relaxed);
        diagnosticsDroppedEvents_.store (0, std::memory_order_relaxed);
        diagnosticsInvalidSamples_.store (0, std::memory_order_relaxed);
        diagnosticsActiveNotes_.store (0, std::memory_order_relaxed);
        diagnosticsQueuedEvents_.store (0, std::memory_order_relaxed);
        diagnosticsPeakBits_.store (0, std::memory_order_relaxed);
        diagnosticsMeanBits_.store (0, std::memory_order_relaxed);
        diagnosticsRmsBits_.store (0, std::memory_order_relaxed);
    }

    bool ChurchOrganCore::render (
        AudioBlockView output) noexcept
    {
        if (! prepared_
             || output.left == nullptr
             || output.numSamples == 0
             || output.numSamples > spec_.maximumBlockSize)
            return false;

        for (std::size_t drawbar = 0;
             drawbar < drawbarCount;
             ++drawbar)
            drawbarTargetGain_[drawbar] =
                gainForDrawbarPosition (
                    targetDrawbarPosition (drawbar));

        std::size_t processedEvents = 0;
        float peak = 0.0f;
        double sum = 0.0;
        double sumSquares = 0.0;
        std::uint64_t invalidSamples = 0;

        for (std::size_t sampleIndex = 0;
             sampleIndex < output.numSamples;
             ++sampleIndex)
        {
            while (processedEvents < eventCount_
                    && events_[processedEvents].sampleOffset
                           <= sampleIndex)
            {
                applyEvent (events_[processedEvents]);
                ++processedEvents;
            }

            for (std::size_t drawbar = 0;
                 drawbar < drawbarCount;
                 ++drawbar)
                drawbarCurrentGain_[drawbar] +=
                    (drawbarTargetGain_[drawbar]
                     - drawbarCurrentGain_[drawbar])
                    * drawbarSmoothingCoefficient_;

            float mixed = 0.0f;
            std::size_t activeIndex = 0;

            while (activeIndex < activeNoteCount_)
            {
                const int noteNumber =
                    static_cast<int> (activeNotes_[activeIndex]);
                auto& note = notes_[
                    static_cast<std::size_t> (noteNumber)];
                const float coefficient =
                    note.held
                        ? contactAttackCoefficient_
                        : contactReleaseCoefficient_;
                const float target = note.held ? 1.0f : 0.0f;
                note.contact +=
                    (target - note.contact) * coefficient;

                if (note.held && note.contact > 0.999999f)
                    note.contact = 1.0f;

                if (! note.held && note.contact < 0.000001f)
                {
                    note.contact = 0.0f;
                    removeActiveNoteAt (activeIndex);
                    continue;
                }

                float noteSignal = 0.0f;
                const auto& mapping =
                    wheelMap_[static_cast<std::size_t> (noteNumber)];

                for (std::size_t drawbar = 0;
                     drawbar < drawbarCount;
                     ++drawbar)
                    noteSignal +=
                        tonewheels_.sample (mapping[drawbar])
                        * drawbarCurrentGain_[drawbar];

                mixed += noteSignal * note.contact;
                ++activeIndex;
            }

            float value = mixed * outputTrim_;

            if (! std::isfinite (value))
            {
                value = 0.0f;
                ++invalidSamples;
            }

            output.left[sampleIndex] = value;

            if (output.right != nullptr
                 && output.right != output.left)
                output.right[sampleIndex] = value;

            const float absolute = std::abs (value);
            peak = std::max (peak, absolute);
            sum += static_cast<double> (value);
            sumSquares +=
                static_cast<double> (value)
                * static_cast<double> (value);
            tonewheels_.advance();
        }

        const std::size_t remainingEvents =
            eventCount_ - processedEvents;

        for (std::size_t event = 0;
             event < remainingEvents;
             ++event)
        {
            events_[event] =
                events_[processedEvents + event];
            events_[event].sampleOffset -=
                static_cast<std::uint32_t> (output.numSamples);
        }

        eventCount_ = remainingEvents;
        totalRenderedSamples_.fetch_add (
            output.numSamples, std::memory_order_relaxed);
        publishDiagnostics (
            peak, sum, sumSquares, output.numSamples, invalidSamples);
        return true;
    }

    bool ChurchOrganCore::queueUpperNoteOn (
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (velocity <= 0)
            return queueUpperNoteOff (note, sampleOffset);

        if (note < 0
             || note >= static_cast<int> (midiNoteCount))
        {
            ++droppedEventCount_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::noteOn;
        event.manual = Manual::upper;
        event.note = static_cast<std::uint8_t> (note);
        event.velocity = static_cast<std::uint8_t> (
            std::max (1, std::min (127, velocity)));
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool ChurchOrganCore::queueUpperNoteOff (
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (note < 0
             || note >= static_cast<int> (midiNoteCount))
        {
            ++droppedEventCount_;
            return false;
        }

        NoteEvent event;
        event.type = EventType::noteOff;
        event.manual = Manual::upper;
        event.note = static_cast<std::uint8_t> (note);
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    bool ChurchOrganCore::queueUpperAllNotesOff (
        std::uint32_t sampleOffset) noexcept
    {
        NoteEvent event;
        event.type = EventType::allNotesOff;
        event.manual = Manual::upper;
        event.sampleOffset = sampleOffset;
        return queueEvent (event);
    }

    void ChurchOrganCore::setDrawbarPosition (
        std::size_t drawbar,
        float positionZeroToEight) noexcept
    {
        if (drawbar < drawbarCount)
            drawbarPositionQ_[drawbar].store (
                quantisePosition (positionZeroToEight),
                std::memory_order_release);
    }

    float ChurchOrganCore::targetDrawbarPosition (
        std::size_t drawbar) const noexcept
    {
        if (drawbar >= drawbarCount)
            return 0.0f;

        return dequantisePosition (
            drawbarPositionQ_[drawbar].load (
                std::memory_order_acquire));
    }

    float ChurchOrganCore::currentDrawbarGain (
        std::size_t drawbar) const noexcept
    {
        return drawbar < drawbarCount
                   ? drawbarCurrentGain_[drawbar]
                   : 0.0f;
    }

    float ChurchOrganCore::gainForDrawbarPosition (
        float positionZeroToEight) noexcept
    {
        if (! std::isfinite (positionZeroToEight))
            return 0.0f;

        const float clamped = std::max (
            0.0f, std::min (8.0f, positionZeroToEight));
        const auto lower =
            static_cast<std::size_t> (clamped);

        if (lower >= drawbarGainCurve.size() - 1)
            return drawbarGainCurve.back();

        const float fraction =
            clamped - static_cast<float> (lower);
        return drawbarGainCurve[lower]
               + fraction
                   * (drawbarGainCurve[lower + 1]
                      - drawbarGainCurve[lower]);
    }

    FoldbackResult ChurchOrganCore::foldback (
        std::size_t drawbar,
        int upperManualMidiNote) const noexcept
    {
        return HarmonicFoldback::map (
            drawbar, upperManualMidiNote);
    }

    double ChurchOrganCore::tonewheelFrequencyForMidi (
        int wheelMidiNote) const noexcept
    {
        return tonewheels_.frequencyForMidi (wheelMidiNote);
    }

    DiagnosticsSnapshot ChurchOrganCore::diagnostics() const noexcept
    {
        DiagnosticsSnapshot snapshot;
        snapshot.totalRenderedSamples =
            totalRenderedSamples_.load (std::memory_order_acquire);
        snapshot.droppedEvents =
            diagnosticsDroppedEvents_.load (
                std::memory_order_acquire);
        snapshot.invalidOutputSamples =
            diagnosticsInvalidSamples_.load (
                std::memory_order_acquire);
        snapshot.activeUpperNotes =
            diagnosticsActiveNotes_.load (
                std::memory_order_acquire);
        snapshot.queuedEvents =
            diagnosticsQueuedEvents_.load (
                std::memory_order_acquire);
        snapshot.lastBlockPeak = bitsFloat (
            diagnosticsPeakBits_.load (
                std::memory_order_acquire));
        snapshot.lastBlockMean = bitsFloat (
            diagnosticsMeanBits_.load (
                std::memory_order_acquire));
        snapshot.lastBlockRms = bitsFloat (
            diagnosticsRmsBits_.load (
                std::memory_order_acquire));
        return snapshot;
    }

    std::uint32_t ChurchOrganCore::quantisePosition (
        float positionZeroToEight) noexcept
    {
        if (! std::isfinite (positionZeroToEight))
            return 0;

        const float clamped = std::max (
            0.0f, std::min (8.0f, positionZeroToEight));
        return static_cast<std::uint32_t> (
            clamped * static_cast<float> (positionScale) + 0.5f);
    }

    float ChurchOrganCore::dequantisePosition (
        std::uint32_t quantised) noexcept
    {
        const auto maximum = 8u * positionScale;
        quantised = std::min (maximum, quantised);
        return static_cast<float> (quantised)
               / static_cast<float> (positionScale);
    }

    float ChurchOrganCore::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        const double samples = std::max (
            1.0,
            sampleRate
                * static_cast<double> (milliseconds)
                * 0.001);
        return static_cast<float> (
            1.0 - std::exp (-1.0 / samples));
    }

    std::uint32_t ChurchOrganCore::floatBits (
        float value) noexcept
    {
        std::uint32_t bits = 0;
        static_assert (sizeof (bits) == sizeof (value),
                       "float storage must be 32 bit");
        std::memcpy (&bits, &value, sizeof (bits));
        return bits;
    }

    float ChurchOrganCore::bitsFloat (
        std::uint32_t value) noexcept
    {
        float result = 0.0f;
        std::memcpy (&result, &value, sizeof (result));
        return result;
    }

    bool ChurchOrganCore::queueEvent (
        const NoteEvent& event) noexcept
    {
        if (eventCount_ >= events_.size())
        {
            ++droppedEventCount_;
            return false;
        }

        std::size_t insertion = eventCount_;

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

    void ChurchOrganCore::applyEvent (
        const NoteEvent& event) noexcept
    {
        if (event.manual != Manual::upper)
            return;

        switch (event.type)
        {
            case EventType::noteOn:
                activateNote (event.note, event.velocity);
                break;
            case EventType::noteOff:
                releaseNote (event.note);
                break;
            case EventType::allNotesOff:
                releaseAllNotes();
                break;
            case EventType::expression:
            case EventType::volume:
            case EventType::sustain:
                // O core de fundação não possui ainda os controladores. O
                // TonewheelOrganEngine integrado os aplica sample-accurate.
                break;
        }
    }

    void ChurchOrganCore::activateNote (
        int note,
        int velocity) noexcept
    {
        auto& state = notes_[static_cast<std::size_t> (note)];
        state.held = true;
        state.velocity = static_cast<std::uint8_t> (velocity);

        if (activeSlots_[static_cast<std::size_t> (note)] >= 0)
            return;

        if (activeNoteCount_ >= activeNotes_.size())
        {
            ++droppedEventCount_;
            return;
        }

        activeNotes_[activeNoteCount_] =
            static_cast<std::uint8_t> (note);
        activeSlots_[static_cast<std::size_t> (note)] =
            static_cast<std::int16_t> (activeNoteCount_);
        ++activeNoteCount_;
    }

    void ChurchOrganCore::releaseNote (int note) noexcept
    {
        notes_[static_cast<std::size_t> (note)].held = false;
    }

    void ChurchOrganCore::releaseAllNotes() noexcept
    {
        for (std::size_t index = 0;
             index < activeNoteCount_;
             ++index)
            notes_[activeNotes_[index]].held = false;
    }

    void ChurchOrganCore::removeActiveNoteAt (
        std::size_t activeIndex) noexcept
    {
        const std::size_t last = activeNoteCount_ - 1;
        const auto removedNote = activeNotes_[activeIndex];

        if (activeIndex != last)
        {
            const auto movedNote = activeNotes_[last];
            activeNotes_[activeIndex] = movedNote;
            activeSlots_[movedNote] =
                static_cast<std::int16_t> (activeIndex);
        }

        activeSlots_[removedNote] = -1;
        activeNotes_[last] = 0;
        --activeNoteCount_;
    }

    void ChurchOrganCore::publishDiagnostics (
        float peak,
        double sum,
        double sumSquares,
        std::size_t samples,
        std::uint64_t invalidSamples) noexcept
    {
        const double divisor =
            static_cast<double> (std::max<std::size_t> (1, samples));
        const float mean =
            static_cast<float> (sum / divisor);
        const float rms = static_cast<float> (
            std::sqrt (std::max (0.0, sumSquares / divisor)));

        diagnosticsDroppedEvents_.store (
            droppedEventCount_, std::memory_order_release);
        diagnosticsInvalidSamples_.fetch_add (
            invalidSamples, std::memory_order_release);
        diagnosticsActiveNotes_.store (
            static_cast<std::uint32_t> (activeNoteCount_),
            std::memory_order_release);
        diagnosticsQueuedEvents_.store (
            static_cast<std::uint32_t> (eventCount_),
            std::memory_order_release);
        diagnosticsPeakBits_.store (
            floatBits (peak), std::memory_order_release);
        diagnosticsMeanBits_.store (
            floatBits (mean), std::memory_order_release);
        diagnosticsRmsBits_.store (
            floatBits (rms), std::memory_order_release);
    }
}
