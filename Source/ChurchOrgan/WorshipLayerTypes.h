#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::size_t worshipLayerCount = 7;
    inline constexpr std::size_t maximumWorshipVoices = 48;
    inline constexpr std::size_t maximumWorshipEvents = 512;

    enum class WorshipQuality : std::uint8_t
    {
        eco = 0,
        standard,
        ultra
    };

    enum class WorshipLayer : std::uint8_t
    {
        pad = 0,
        air,
        strings,
        shimmer,
        octave,
        texture,
        reverseBloom
    };

    enum class WorshipEventType : std::uint8_t
    {
        noteOn = 0,
        noteOff,
        allNotesOff,
        expression
    };

    struct WorshipEvent
    {
        WorshipEventType type = WorshipEventType::noteOff;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        float value = 0.0f;
        std::uint32_t sampleOffset = 0;
    };

    struct WorshipLayerParameters
    {
        bool enabled = true;
        float level = 0.0f;
        float attackMilliseconds = 250.0f;
        float releaseMilliseconds = 1600.0f;
        std::uint8_t lowestNote = 0;
        std::uint8_t highestNote = 127;
        float velocityResponse = 0.0f;
        float expressionResponse = 0.8f;
        float stereoWidth = 0.5f;
        float highPassHz = 120.0f;
        float lowPassHz = 12000.0f;
        float ducking = 0.7f;
        float reverbSend = 0.35f;
    };

    struct WorshipPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        WorshipQuality quality = WorshipQuality::standard;
        std::uint64_t deterministicSeed =
            0x574f52534849504cULL;
    };

    struct WorshipAudioBlock
    {
        // The layer is added to the channel buffers. right == nullptr means
        // mono. Reverb send buffers are optional and are also additive.
        float* left = nullptr;
        float* right = nullptr;
        float* reverbSendLeft = nullptr;
        float* reverbSendRight = nullptr;
        std::size_t numSamples = 0;
    };

    struct WorshipIntensityProfile
    {
        std::array<float, worshipLayerCount> layerGain {};
        float spectralOpening = 0.0f;
        float stereoExpansion = 0.0f;
        float reverbExpansion = 0.0f;
        float loudnessCompensation = 1.0f;
        float perceptualEnergyTarget = 0.0f;
    };

    struct WorshipDiagnostics
    {
        std::uint64_t renderedSamples = 0;
        std::uint64_t droppedEvents = 0;
        std::uint64_t stolenVoices = 0;
        std::uint32_t heldVoices = 0;
        std::uint32_t tailVoices = 0;
        std::uint32_t queuedEvents = 0;
        float currentIntensity = 0.0f;
        float lastLayerPeak = 0.0f;
        float lastReverbPeak = 0.0f;
    };

    constexpr std::size_t worshipLayerIndex (
        WorshipLayer layer) noexcept
    {
        return static_cast<std::size_t> (layer);
    }
}
