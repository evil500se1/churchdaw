#pragma once

#include "AutoMixContext.h"
#include "TonewheelOrganEngine.h"
#include "WorshipLayerEngine.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class WorshipHybridQuality : std::uint8_t
    {
        eco = 0,
        standard,
        ultra
    };

    struct WorshipHybridPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        std::uint64_t deterministicSeed =
            0x4859425249444f52ULL;
        WorshipHybridQuality quality =
            WorshipHybridQuality::standard;
    };

    struct WorshipHybridAudioBlock
    {
        float* left = nullptr;
        float* right = nullptr;
        float* reverbSendLeft = nullptr;
        float* reverbSendRight = nullptr;
        std::size_t numSamples = 0;
    };

    struct WorshipHybridDiagnostics
    {
        std::uint64_t renderedSamples = 0;
        std::uint64_t droppedWrapperEvents = 0;
        std::uint64_t droppedChildEvents = 0;
        std::uint64_t invalidSamples = 0;
        std::uint32_t queuedEvents = 0;
        std::uint32_t activeTonewheelNotes = 0;
        std::uint32_t activeWorshipVoices = 0;
        std::uint32_t worshipTailVoices = 0;
        float lastMainPeak = 0.0f;
        float lastReverbSendPeak = 0.0f;
        float worshipIntensity = 0.0f;
        AutoMixGains autoMixGains {};
        bool preparedStateB = false;
    };

    enum WorshipHybridManualMask : std::uint8_t
    {
        hybridMirrorUpper = 1u << 0,
        hybridMirrorLower = 1u << 1,
        hybridMirrorPedal = 1u << 2,
        hybridMirrorUpperAndLower =
            hybridMirrorUpper | hybridMirrorLower
    };

    // Fourth Church Organ family: the complete Tonewheel engine and the
    // internal Worship layer share one sample-accurate performance stream.
    class WorshipHybridEngine
    {
    public:
        static constexpr std::size_t maximumBlockCapacity =
            TonewheelOrganEngine::maximumBlockCapacity;
        static constexpr std::size_t maximumEvents =
            TonewheelOrganEngine::maximumEvents;

        WorshipHybridEngine() noexcept = default;

        WorshipHybridEngine (const WorshipHybridEngine&) = delete;
        WorshipHybridEngine& operator= (
            const WorshipHybridEngine&) = delete;

        bool prepare (
            const WorshipHybridPrepareSpec& spec) noexcept;
        void reset() noexcept;

        bool render (
            const WorshipHybridAudioBlock& output) noexcept;

        bool queueNoteOn (
            Manual manual,
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueNoteOff (
            Manual manual,
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueSustain (
            Manual manual,
            bool down,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueAllNotesOff (
            Manual manual,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueAllNotesOffAll (
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueExpression (
            int valueZeroTo127,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueVolume (
            int valueZeroTo127,
            std::uint32_t sampleOffset = 0) noexcept;

        void setWorshipIntensity (float intensity) noexcept;
        float worshipIntensity() const noexcept;
        void setOutputGain (float linearGain) noexcept;

        void setAutoMixParameters (
            const AutoMixParameters& parameters) noexcept;
        void setAutoMixControlFrame (
            const AutoMixControlFrame& controls) noexcept;

        // May only change while no physical/sustained performance note is
        // active. This keeps Worship reference counting deterministic.
        bool setLayerManualMask (std::uint8_t mask) noexcept;
        std::uint8_t layerManualMask() const noexcept;

        TonewheelOrganEngine& tonewheel() noexcept;
        WorshipLayerEngine& worshipLayer() noexcept;
        AutoMixContext& autoMix() noexcept;
        const TonewheelOrganEngine& tonewheel() const noexcept;
        const WorshipLayerEngine& worshipLayer() const noexcept;
        const AutoMixContext& autoMix() const noexcept;

        // Prepares a complete silent B state outside the audio callback.
        // It is intentionally not swapped here: a future owner must render A
        // and B together and crossfade, so active tails can never be cut.
        bool prepareStateB (
            std::uint64_t deterministicSeed = 0) noexcept;
        bool stateBReady() const noexcept;
        TonewheelOrganEngine* stateBTonewheel() noexcept;
        WorshipLayerEngine* stateBWorshipLayer() noexcept;
        AutoMixContext* stateBAutoMix() noexcept;
        void discardStateB() noexcept;

        WorshipHybridDiagnostics diagnostics() const noexcept;

        static constexpr bool renderUsesFixedStorage = true;
        static constexpr bool renderUsesLocks = false;

    private:
        struct ContextFilterState
        {
            float low180 = 0.0f;
            float low900 = 0.0f;
            float low3200 = 0.0f;
            float dcInput = 0.0f;
            float dcOutput = 0.0f;
        };

        struct EngineState
        {
            TonewheelOrganEngine tonewheel;
            WorshipLayerEngine worship;
            AutoMixContext autoMix;
            ContextFilterState leftFilter {};
            ContextFilterState rightFilter {};
            bool prepared = false;
        };

        struct ManualPerformanceState
        {
            std::array<bool, midiNoteCount> physicallyHeld {};
            std::array<bool, midiNoteCount> sustained {};
            std::array<std::uint8_t, midiNoteCount> velocities {};
            bool sustainDown = false;
        };

        static bool validManual (Manual manual) noexcept;
        static bool validNote (int note) noexcept;
        static std::size_t manualIndex (Manual manual) noexcept;
        static std::uint8_t manualBit (Manual manual) noexcept;
        static int eventPriority (EventType type) noexcept;
        static OrganQualityProfile tonewheelQuality (
            WorshipHybridQuality quality) noexcept;
        static WorshipQuality worshipQuality (
            WorshipHybridQuality quality) noexcept;
        static float clamp01 (float value) noexcept;
        static float softProtect (
            float value,
            float threshold,
            float ceiling) noexcept;

        bool prepareState (
            EngineState& state,
            std::uint64_t seed) noexcept;
        void cloneActiveConfigurationTo (
            EngineState& destination) noexcept;
        void applyIntensityTo (
            EngineState& state,
            float intensity) noexcept;

        bool queueEvent (const NoteEvent& event) noexcept;
        bool dispatchEvent (
            EngineState& state,
            const NoteEvent& event) noexcept;
        bool dispatchTonewheelEvent (
            TonewheelOrganEngine& engine,
            const NoteEvent& event) noexcept;
        bool dispatchWorshipEvent (
            WorshipLayerEngine& engine,
            const NoteEvent& event) noexcept;
        void carryEvents (
            std::size_t firstUnprocessed,
            std::size_t renderedSamples) noexcept;

        bool layerMirrors (Manual manual) const noexcept;
        bool effectiveLayerHeld (int note) const noexcept;
        void clearPerformanceState() noexcept;
        bool performanceIsIdle() const noexcept;

        float contextualOrganSample (
            float input,
            ContextFilterState& state,
            const AutoMixGains& gains) noexcept;
        float dcBlock (
            float input,
            ContextFilterState& state) const noexcept;
        void clearOutput (
            const WorshipHybridAudioBlock& output) noexcept;
        void publishDiagnostics (
            const AutoMixGains& gains,
            float mainPeak,
            float sendPeak,
            std::uint64_t invalidSamples) noexcept;

        std::array<EngineState, 2> states_ {};
        std::size_t activeState_ = 0;
        bool stateBReady_ = false;

        std::array<ManualPerformanceState,
                   TonewheelOrganEngine::manualCount> performance_ {};
        std::array<NoteEvent, maximumEvents> events_ {};
        std::size_t eventCount_ = 0;

        std::array<float, maximumBlockCapacity> workLeft_ {};
        std::array<float, maximumBlockCapacity> workRight_ {};
        std::array<float, maximumBlockCapacity> sendLeft_ {};
        std::array<float, maximumBlockCapacity> sendRight_ {};

        worshipdsp::SmoothedValue organBalance_;
        worshipdsp::SmoothedValue outputGain_;
        WorshipHybridPrepareSpec spec_ {};
        AutoMixParameters autoMixParameters_ {};
        AutoMixControlFrame autoMixControls_ {};
        float targetIntensity_ = 0.35f;
        float targetOutputGain_ = 0.92f;
        float low180Step_ = 0.0f;
        float low900Step_ = 0.0f;
        float low3200Step_ = 0.0f;
        float dcCoefficient_ = 0.995f;
        std::uint8_t layerManualMask_ =
            hybridMirrorUpperAndLower;

        std::uint64_t renderedSamples_ = 0;
        std::uint64_t droppedWrapperEvents_ = 0;
        std::uint64_t droppedChildEvents_ = 0;
        std::uint64_t invalidSamples_ = 0;
        WorshipHybridDiagnostics diagnostics_ {};
        bool prepared_ = false;
    };
}
