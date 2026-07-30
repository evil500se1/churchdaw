#pragma once

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::size_t drawbarCount = 9;
    inline constexpr std::size_t midiNoteCount = 128;
    inline constexpr std::size_t maximumQueuedEvents = 512;

    enum class Manual : std::uint8_t
    {
        upper = 0,
        lower,
        pedal
    };

    enum class EventType : std::uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff,
        expression,
        volume,
        sustain
    };

    // Eventos entram na mesma thread que chamara render(). O adapter de
    // plataforma deve copiar os eventos do bloco para esta estrutura antes do
    // callback; o core deliberadamente nao implementa uma fila MPSC.
    struct NoteEvent
    {
        EventType type = EventType::noteOff;
        Manual manual = Manual::upper;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint32_t sampleOffset = 0;
    };

    struct AudioBlockView
    {
        float* left = nullptr;
        float* right = nullptr; // nullptr significa mono
        std::size_t numSamples = 0;
    };

    struct PrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        std::uint64_t deterministicSeed = 0x4348555243484f52ULL;
        float drawbarSmoothingMilliseconds = 18.0f;
        float contactAttackMilliseconds = 0.70f;
        float contactReleaseMilliseconds = 1.20f;
        float maximumWheelOffsetCents = 0.0f;
        float outputTrim = 0.11f;
    };

    // Snapshot POD: nenhum texto, ponteiro ou container dinamico cruza a
    // fronteira da thread de audio.
    struct DiagnosticsSnapshot
    {
        std::uint64_t totalRenderedSamples = 0;
        std::uint64_t droppedEvents = 0;
        std::uint64_t invalidOutputSamples = 0;
        std::uint32_t activeUpperNotes = 0;
        std::uint32_t queuedEvents = 0;
        float lastBlockPeak = 0.0f;
        float lastBlockMean = 0.0f;
        float lastBlockRms = 0.0f;
    };
}
