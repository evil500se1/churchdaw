#pragma once

#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::size_t pipeOrganManualCount = 3;
    inline constexpr std::size_t pipeOrganRankCount = 11;
    inline constexpr std::size_t pipeOrganMidiNoteCount = 128;
    inline constexpr std::size_t pipeOrganMaximumEvents = 1024;

    enum class PipeOrganManual : std::uint8_t
    {
        upper = 0,
        lower,
        pedal
    };

    enum class PipeOrganRank : std::uint8_t
    {
        principal = 0,
        flute,
        gedackt,
        bourdon,
        octave,
        mixture,
        strings,
        reed,
        trumpet,
        choir,
        pedal
    };

    enum class PipeOrganQuality : std::uint8_t
    {
        eco = 0,
        standard,
        ultra
    };

    enum class PipeOrganTemperament : std::uint8_t
    {
        equal = 0,
        churchWell,
        gentleMeantone,
        pureChoir
    };

    enum class PipeOrganEventType : std::uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff
    };

    struct PipeOrganEvent
    {
        PipeOrganEventType type = PipeOrganEventType::noteOff;
        PipeOrganManual manual = PipeOrganManual::upper;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint32_t sampleOffset = 0;
    };

    struct PipeOrganAudioBlockView
    {
        float* left = nullptr;
        float* right = nullptr;
        std::size_t numSamples = 0;
    };

    struct PipeOrganPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 128;
        std::uint64_t deterministicSeed = 0x504950454f524741ULL;
        PipeOrganQuality quality = PipeOrganQuality::standard;
        PipeOrganTemperament temperament =
            PipeOrganTemperament::churchWell;
        float tuningAHz = 440.0f;
        float outputTrim = 0.082f;
        float stereoWidth = 0.72f;
    };

    struct PipeOrganDiagnostics
    {
        std::uint64_t renderedSamples = 0;
        std::uint64_t droppedEvents = 0;
        std::uint64_t invalidSamples = 0;
        std::uint32_t activeDivisionVoices = 0;
        std::uint32_t soundingPipes = 0;
        float lastBlockPeak = 0.0f;
        float lastBlockMean = 0.0f;
        float lastBlockRms = 0.0f;
        float upperWindPressure = 1.0f;
        float lowerWindPressure = 1.0f;
        float pedalWindPressure = 1.0f;
    };
}
