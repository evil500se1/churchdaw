#pragma once

#include "OrganArticulationTypes.h"

#include <array>
#include <cstddef>

namespace churchorgan
{
    // Envelope monofonico de percussao harmonica. O chamador fornece o sample
    // da roda indicada por harmonicDrawbarIndex()/triggeredMidiNote().
    class HarmonicPercussion
    {
    public:
        bool prepare (
            double sampleRate,
            const HarmonicPercussionParameters& parameters = {}) noexcept;

        void reset() noexcept;

        // Retorna true quando este evento realmente dispara a percussao.
        bool noteOn (int midiNote, int velocity) noexcept;
        void noteOff (int midiNote) noexcept;
        void allNotesOff() noexcept;

        // Avanca o envelope em um sample e aplica ao carrier harmonico.
        float processSample (float harmonicCarrier) noexcept;
        float advanceEnvelope() noexcept;

        void setHarmonic (PercussionHarmonic harmonic) noexcept;
        void setTiming (PercussionTiming timing) noexcept;
        void setVolume (PercussionVolume volume) noexcept;
        void setTriggerMode (PercussionTriggerMode mode) noexcept;

        PercussionHarmonic harmonic() const noexcept;
        PercussionTiming timing() const noexcept;
        PercussionVolume volume() const noexcept;
        PercussionTriggerMode triggerMode() const noexcept;

        int triggeredMidiNote() const noexcept;
        int requestedHarmonicMidi() const noexcept;
        int harmonicSemitoneOffset() const noexcept;
        std::size_t harmonicDrawbarIndex() const noexcept;
        std::size_t heldNoteCount() const noexcept;
        float currentGain() const noexcept;
        bool isActive() const noexcept;

        const HarmonicPercussionParameters& parameters() const noexcept;

    private:
        enum class EnvelopeStage
        {
            idle,
            attack,
            decay,
            release
        };

        static bool validParameters (
            double sampleRate,
            const HarmonicPercussionParameters& parameters) noexcept;
        static float decayCoefficient (
            double sampleRate,
            float milliseconds) noexcept;

        void trigger (int midiNote) noexcept;
        void refreshDecayCoefficient() noexcept;
        float selectedPeakGain() const noexcept;

        std::array<bool, articulationMidiNoteCount> held_ {};
        std::size_t heldCount_ = 0;
        HarmonicPercussionParameters parameters_ {};
        double sampleRate_ = 48000.0;
        float envelope_ = 0.0f;
        float peakGain_ = 0.0f;
        float attackIncrement_ = 0.0f;
        float selectedDecayCoefficient_ = 0.0f;
        float releaseCoefficient_ = 0.0f;
        int triggeredNote_ = -1;
        EnvelopeStage stage_ = EnvelopeStage::idle;
        bool prepared_ = false;
    };
}
