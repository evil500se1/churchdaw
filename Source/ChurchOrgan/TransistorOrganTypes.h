#pragma once

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::size_t transistorTabCount = 8;
    inline constexpr std::size_t transistorMidiNoteCount = 128;
    inline constexpr std::size_t transistorMaximumQueuedEvents = 512;
    // The integrated Transistor path is followed by ChurchDAW's channel
    // rotary. Keep the electronic generator vibrato available, but make it
    // an explicit opt-in so the factory cannot stack two motion stages.
    inline constexpr bool transistorDefaultVibratoEnabled = false;
    inline constexpr float transistorDefaultVibratoDepth = 0.55f;

    enum class TransistorOrganProfile : std::uint8_t
    {
        britishCombo = 0,
        italianCombo,
        germanCombo
    };

    enum class TransistorOrganManual : std::uint8_t
    {
        upper = 0
    };

    enum class TransistorOrganEventType : std::uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff
    };

    enum class TransistorOrganTab : std::uint8_t
    {
        bass16 = 0,
        principal8,
        reed8,
        flute4,
        string4,
        bright2,
        mixture,
        brilliance
    };

    struct TransistorOrganEvent
    {
        TransistorOrganEventType type =
            TransistorOrganEventType::noteOff;
        TransistorOrganManual manual =
            TransistorOrganManual::upper;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint32_t sampleOffset = 0;
    };

    struct TransistorAudioBlockView
    {
        float* left = nullptr;
        float* right = nullptr;
        std::size_t numSamples = 0;
    };

    struct TransistorOrganPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 128;
        std::uint64_t deterministicSeed = 0x434f4d424f4f5247ULL;
        TransistorOrganProfile profile =
            TransistorOrganProfile::britishCombo;
        float outputTrim = 0.045f;
        float drive = 0.18f;
        float stereoWidth = 0.22f;
        float vibratoDepth = transistorDefaultVibratoDepth;
        bool vibratoEnabled = transistorDefaultVibratoEnabled;
    };

    struct TransistorOrganDiagnostics
    {
        std::uint64_t renderedSamples = 0;
        std::uint64_t droppedEvents = 0;
        std::uint64_t invalidSamples = 0;
        std::uint32_t activeUpperNotes = 0;
        float lastBlockPeak = 0.0f;
        float lastBlockMean = 0.0f;
        float lastBlockRms = 0.0f;
    };
}
