#pragma once

#include "ChurchOrganTypes.h"
#include "HarmonicFoldback.h"
#include "TonewheelBank.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    class ChurchOrganCore
    {
    public:
        static_assert (
            std::atomic<std::uint32_t>::is_always_lock_free,
            "Church Organ requires lock-free 32-bit atomics");
        static_assert (
            std::atomic<std::uint64_t>::is_always_lock_free,
            "Church Organ requires lock-free 64-bit atomics");

        ChurchOrganCore() noexcept;

        ChurchOrganCore (const ChurchOrganCore&) = delete;
        ChurchOrganCore& operator= (const ChurchOrganCore&) = delete;

        bool prepare (const PrepareSpec& spec) noexcept;
        void reset() noexcept;

        // Substitui o conteudo do bloco. right == nullptr produz mono.
        // Depois de prepare(), este metodo usa apenas armazenamento fixo.
        bool render (AudioBlockView output) noexcept;

        bool queueUpperNoteOn (
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueUpperNoteOff (
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueUpperAllNotesOff (
            std::uint32_t sampleOffset = 0) noexcept;

        void setDrawbarPosition (
            std::size_t drawbar,
            float positionZeroToEight) noexcept;
        float targetDrawbarPosition (
            std::size_t drawbar) const noexcept;
        float currentDrawbarGain (
            std::size_t drawbar) const noexcept;

        static float gainForDrawbarPosition (
            float positionZeroToEight) noexcept;

        FoldbackResult foldback (
            std::size_t drawbar,
            int upperManualMidiNote) const noexcept;
        double tonewheelFrequencyForMidi (
            int wheelMidiNote) const noexcept;

        DiagnosticsSnapshot diagnostics() const noexcept;

        static constexpr bool renderUsesFixedStorage = true;
        static constexpr std::size_t maximumPolyphony =
            midiNoteCount;

    private:
        struct NoteState
        {
            float contact = 0.0f;
            std::uint8_t velocity = 0;
            bool held = false;
        };

        static constexpr std::uint32_t positionScale = 8192;

        static std::uint32_t quantisePosition (
            float positionZeroToEight) noexcept;
        static float dequantisePosition (
            std::uint32_t quantised) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint32_t floatBits (float value) noexcept;
        static float bitsFloat (std::uint32_t value) noexcept;

        bool queueEvent (const NoteEvent& event) noexcept;
        void applyEvent (const NoteEvent& event) noexcept;
        void activateNote (int note, int velocity) noexcept;
        void releaseNote (int note) noexcept;
        void releaseAllNotes() noexcept;
        void removeActiveNoteAt (std::size_t activeIndex) noexcept;
        void publishDiagnostics (
            float peak,
            double sum,
            double sumSquares,
            std::size_t samples,
            std::uint64_t invalidSamples) noexcept;

        PrepareSpec spec_ {};
        TonewheelBank tonewheels_;

        std::array<std::array<std::uint8_t, drawbarCount>,
                   midiNoteCount> wheelMap_ {};
        std::array<NoteState, midiNoteCount> notes_ {};
        std::array<std::uint8_t, midiNoteCount> activeNotes_ {};
        std::array<std::int16_t, midiNoteCount> activeSlots_ {};
        std::size_t activeNoteCount_ = 0;

        std::array<NoteEvent, maximumQueuedEvents> events_ {};
        std::size_t eventCount_ = 0;
        std::uint64_t droppedEventCount_ = 0;

        std::array<std::atomic<std::uint32_t>, drawbarCount>
            drawbarPositionQ_ {};
        std::array<float, drawbarCount> drawbarCurrentGain_ {};
        std::array<float, drawbarCount> drawbarTargetGain_ {};

        float drawbarSmoothingCoefficient_ = 1.0f;
        float contactAttackCoefficient_ = 1.0f;
        float contactReleaseCoefficient_ = 1.0f;
        float outputTrim_ = 0.11f;
        bool prepared_ = false;

        std::atomic<std::uint64_t> totalRenderedSamples_ { 0 };
        std::atomic<std::uint64_t> diagnosticsDroppedEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticsInvalidSamples_ { 0 };
        std::atomic<std::uint32_t> diagnosticsActiveNotes_ { 0 };
        std::atomic<std::uint32_t> diagnosticsQueuedEvents_ { 0 };
        std::atomic<std::uint32_t> diagnosticsPeakBits_ { 0 };
        std::atomic<std::uint32_t> diagnosticsMeanBits_ { 0 };
        std::atomic<std::uint32_t> diagnosticsRmsBits_ { 0 };
    };
}
