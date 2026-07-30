#pragma once

#include "OrganArticulationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    // Modela os nove barramentos de uma tecla como contatos independentes.
    // noteOn/noteOff e advance devem ser chamados pela thread de audio.
    class MultiContactKeying
    {
    public:
        bool prepare (
            double sampleRate,
            std::uint64_t deterministicSeed,
            const MultiContactParameters& parameters = {}) noexcept;

        void reset() noexcept;
        void noteOn (int midiNote, int velocity) noexcept;
        void noteOff (int midiNote) noexcept;
        void allNotesOff() noexcept;

        // Changes only the live per-contact envelope coefficients. Zero keeps
        // the audited native contact time, so defaults remain bit-compatible.
        // Safe on the audio/control handoff thread; no reset or allocation.
        void setPerformanceEnvelopeMilliseconds (
            float attackMilliseconds,
            float releaseMilliseconds) noexcept;

        // Avanca exatamente um sample para todas as teclas audiveis.
        void advance() noexcept;

        float contactGain (
            int midiNote,
            std::size_t contactIndex) const noexcept;
        bool isHeld (int midiNote) const noexcept;
        bool isAudible (int midiNote) const noexcept;
        std::size_t activeNoteCount() const noexcept;

        const MultiContactParameters& parameters() const noexcept;

        static constexpr std::size_t contactCount =
            articulationContactCount;
        static constexpr std::size_t maximumPolyphony =
            articulationMidiNoteCount;

    private:
        struct ContactState
        {
            float level = 0.0f;
            float output = 0.0f;
            float bounceEnvelope = 0.0f;
            float bouncePhase = 0.0f;
            std::uint32_t delaySamples = 0;
            bool targetClosed = false;
        };

        struct NoteState
        {
            std::array<ContactState, contactCount> contacts {};
            std::uint32_t generation = 0;
            std::uint8_t velocity = 0;
            bool held = false;
        };

        static bool validParameters (
            double sampleRate,
            const MultiContactParameters& parameters) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static float decayCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static double unitFromHash (std::uint64_t value) noexcept;

        std::uint32_t contactDelay (
            int midiNote,
            std::size_t contact,
            std::uint32_t generation,
            bool noteOn) const noexcept;
        void ensureActive (int midiNote) noexcept;
        void removeActiveAt (std::size_t activeIndex) noexcept;

        std::array<NoteState, maximumPolyphony> notes_ {};
        std::array<std::uint8_t, maximumPolyphony> activeNotes_ {};
        std::array<std::int16_t, maximumPolyphony> activeSlots_ {};
        std::size_t activeNoteCount_ = 0;

        MultiContactParameters parameters_ {};
        double sampleRate_ = 48000.0;
        std::uint64_t seed_ = 0;
        float attackCoefficient_ = 1.0f;
        float releaseCoefficient_ = 1.0f;
        float bounceDecayCoefficient_ = 0.0f;
        float bouncePhaseIncrement_ = 0.0f;
        bool prepared_ = false;
    };
}
