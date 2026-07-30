#pragma once

#include "OrganArticulationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    // Gerador sintetico de transientes de contato. Nao usa samples nem I/O.
    // Os eventos e renderSample devem ocorrer na thread de audio.
    class KeyClickGenerator
    {
    public:
        bool prepare (
            double sampleRate,
            std::uint64_t deterministicSeed,
            const KeyClickParameters& parameters = {}) noexcept;

        void reset() noexcept;
        void noteOn (int midiNote, int velocity) noexcept;
        void noteOff (int midiNote) noexcept;
        void allNotesOff() noexcept;

        float renderSample() noexcept;

        bool isHeld (int midiNote) const noexcept;
        std::size_t activeClickCount() const noexcept;
        const KeyClickParameters& parameters() const noexcept;

        static constexpr std::size_t maximumPolyphony =
            articulationMidiNoteCount;

    private:
        struct ClickVoice
        {
            float lowState = 0.0f;
            float highState = 0.0f;
            float envelope = 0.0f;
            float decayCoefficient = 0.0f;
            std::uint64_t randomState = 1;
            bool active = false;
        };

        static bool validParameters (
            double sampleRate,
            const KeyClickParameters& parameters) noexcept;
        static float filterCoefficient (
            double sampleRate,
            float frequencyHz) noexcept;
        static float envelopeCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static float nextNoise (std::uint64_t& state) noexcept;

        void trigger (
            int midiNote,
            int velocity,
            bool release) noexcept;
        void ensureActive (int midiNote) noexcept;
        void removeActiveAt (std::size_t activeIndex) noexcept;

        std::array<ClickVoice, maximumPolyphony> voices_ {};
        std::array<std::uint8_t, maximumPolyphony> activeNotes_ {};
        std::array<std::int16_t, maximumPolyphony> activeSlots_ {};
        std::array<std::uint32_t, maximumPolyphony> generations_ {};
        std::array<std::uint8_t, maximumPolyphony> velocities_ {};
        std::array<bool, maximumPolyphony> held_ {};
        std::size_t activeVoiceCount_ = 0;

        KeyClickParameters parameters_ {};
        double sampleRate_ = 48000.0;
        std::uint64_t seed_ = 0;
        float lowCutCoefficient_ = 1.0f;
        float highCutCoefficient_ = 1.0f;
        float onsetDecayCoefficient_ = 0.0f;
        float releaseDecayCoefficient_ = 0.0f;
        bool prepared_ = false;
    };
}
