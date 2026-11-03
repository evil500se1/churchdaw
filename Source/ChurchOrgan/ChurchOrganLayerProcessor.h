#pragma once

#include "AutoMixContext.h"
#include "OrganParameters.h"
#include "PipeOrganEngine.h"
#include "TonewheelOrganEngine.h"
#include "TransistorOrganEngine.h"
#include "WorshipHybridEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class ChurchOrganFamily : std::uint8_t
    {
        tonewheel = 0,
        transistor,
        pipe,
        worshipHybrid
    };

    enum class ChurchOrganLayerQuality : std::uint8_t
    {
        eco = 0,
        standard,
        ultra
    };

    struct ChurchOrganLayerPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        std::uint64_t deterministicSeed =
            0x4c415945524f5247ULL;
        ChurchOrganLayerQuality quality =
            ChurchOrganLayerQuality::standard;
        ChurchOrganFamily initialFamily =
            ChurchOrganFamily::tonewheel;

        // Native engine calibration trims. They are not adapter headroom and
        // retain the audited defaults of each independent engine.
        float transistorEngineTrim = 0.045f;
        float pipeEngineTrim = 0.082f;
        float pipeTuningAHz = 440.0f;
        float pipeStereoWidth = 0.72f;
    };

    struct ChurchOrganLayerDiagnostics
    {
        std::uint64_t renderedSamples = 0;
        std::uint64_t receivedMidiEvents = 0;
        std::uint64_t droppedAdapterEvents = 0;
        std::uint64_t droppedEngineEvents = 0;
        std::uint64_t ignoredChannelEvents = 0;
        std::uint64_t unsupportedPitchEvents = 0;
        std::uint64_t invalidOutputSamples = 0;
        std::uint32_t queuedAdapterEvents = 0;
        std::uint32_t activeNotes = 0;
        float lastBlockPeak = 0.0f;
        float midiVolume = 1.0f;
        float midiExpression = 1.0f;
        float familyOutputTrim = 1.0f;
        ChurchOrganFamily family =
            ChurchOrganFamily::tonewheel;
        bool prepared = false;
        bool suspended = true;
        bool pitchBendSupported = false;
    };

    // JUCE-facing instrument adapter. All four engines are prepared together
    // outside the callback. processBlock() owns neither the MIDI buffer nor
    // the audio buffers after it returns.
    class ChurchOrganLayerProcessor
    {
    public:
        static constexpr std::size_t familyCount = 4;
        static constexpr std::size_t manualCount = 3;
        static constexpr std::size_t maximumEngineBlockCapacity =
            TonewheelOrganEngine::maximumBlockCapacity;
        static constexpr std::size_t maximumBlockCapacity = 8192;
        static constexpr std::size_t maximumAdapterEvents = 1024;
        static constexpr std::uint32_t stateSchemaVersion = 2;

        ChurchOrganLayerProcessor() noexcept;

        ChurchOrganLayerProcessor (
            const ChurchOrganLayerProcessor&) = delete;
        ChurchOrganLayerProcessor& operator= (
            const ChurchOrganLayerProcessor&) = delete;

        bool prepare (
            const ChurchOrganLayerPrepareSpec& spec) noexcept;

        // reset() is accepted only while suspended and outside processBlock.
        bool reset() noexcept;

        // Overwrites output. Extra channels are cleared. reverbSend is
        // optional and is non-zero only for Worship Hybrid.
        bool processBlock (
            juce::AudioBuffer<float>& output,
            const juce::MidiBuffer& midi,
            juce::AudioBuffer<float>* reverbSend = nullptr) noexcept;

        // Host/TimbresProcessor must suspend its callback first, then set this
        // flag and wait until processBlock is no longer active.
        void setProcessingSuspended (bool suspended) noexcept;
        bool isProcessingSuspended() const noexcept;
        bool isInsideProcessBlock() const noexcept;

        bool selectFamilyWhileSuspended (
            ChurchOrganFamily family) noexcept;
        ChurchOrganFamily activeFamily() const noexcept;

        OrganParameterStore& parameters() noexcept;
        const OrganParameterStore& parameters() const noexcept;
        void setParameterPlain (
            OrganParameterId parameter,
            float value) noexcept;
        void setParameterNormalised (
            OrganParameterId parameter,
            float value) noexcept;

        // Bridge for TimbresProcessor's existing channel ATK/REL parameters.
        // The current channel defaults are deliberately mapped to the native
        // family articulation, while every user deviation becomes audible.
        static constexpr float nativeChannelAttackSeconds = 0.003f;
        static constexpr float nativeChannelReleaseSeconds = 0.55f;
        void setChannelPerformanceEnvelopeSeconds (
            float attackSeconds,
            float releaseSeconds) noexcept;

        // Explicit adapter trim. Every family defaults to unity. No automatic
        // polyphony headroom, limiter, AGC or hidden -4 dB stage is applied.
        void setFamilyOutputTrim (
            ChurchOrganFamily family,
            float linearGain) noexcept;
        float familyOutputTrim (
            ChurchOrganFamily family) const noexcept;

        void setTransistorProfile (
            TransistorOrganProfile profile) noexcept;
        void setTransistorTabLevel (
            TransistorOrganTab tab,
            float levelZeroToOne) noexcept;
        void setTransistorVibrato (
            bool enabled,
            float depthZeroToOne) noexcept;
        void setTransistorDrive (
            float driveZeroToOne) noexcept;
        void setTransistorStereoWidth (
            float widthZeroToOne) noexcept;

        void setPipeRankLevel (
            PipeOrganManual manual,
            PipeOrganRank rank,
            float levelZeroToOne) noexcept;
        void setPipeCoupler (
            PipeOrganManual source,
            PipeOrganManual destination,
            bool enabled) noexcept;
        void setPipeSwell (
            PipeOrganManual manual,
            float levelZeroToOne) noexcept;
        void setPipeCrescendo (
            float levelZeroToOne) noexcept;
        void setPipeTremulant (
            PipeOrganManual manual,
            bool enabled,
            float depthZeroToOne,
            float rateHz) noexcept;
        void setPipeTemperament (
            PipeOrganTemperament temperament) noexcept;

        // These methods only publish atomics. The frozen Hybrid setters are
        // called by processBlock at the audio-thread handoff boundary.
        void setHybridAutoMixParameters (
            const AutoMixParameters& parameters) noexcept;
        void setHybridAutoMixControlFrame (
            const AutoMixControlFrame& controls) noexcept;

        // State operations allocate JUCE objects and are consequently
        // message-thread/suspended operations only.
        juce::ValueTree createState() const;
        bool restoreState (
            const juce::ValueTree& state);
        void getStateInformation (
            juce::MemoryBlock& destination) const;
        bool setStateInformation (
            const void* data,
            std::size_t size);

        ChurchOrganLayerDiagnostics diagnostics() const noexcept;
        const ChurchOrganLayerPrepareSpec& prepareSpec() const noexcept;

        TonewheelOrganEngine& tonewheelEngine() noexcept;
        TransistorOrganEngine& transistorEngine() noexcept;
        PipeOrganEngine& pipeEngine() noexcept;
        WorshipHybridEngine& worshipHybridEngine() noexcept;

        static constexpr bool processUsesFixedStorage = true;
        static constexpr bool processUsesLocks = false;
        static constexpr bool appliesPolyphonyHeadroom = false;
        static constexpr bool hasPermanentLimiterOrAgc = false;
        static constexpr bool pitchBendSupported = false;

    private:
        class AtomicFloat
        {
        public:
            AtomicFloat() noexcept;
            explicit AtomicFloat (float initial) noexcept;

            void store (
                float value,
                std::memory_order order =
                    std::memory_order_release) noexcept;
            float load (
                std::memory_order order =
                    std::memory_order_acquire) const noexcept;

        private:
            std::atomic<std::uint32_t> bits_ { 0 };
        };

        enum class AdapterEventType : std::uint8_t
        {
            noteOn = 0,
            noteOff,
            sustain,
            allNotesOff,
            allSoundOff,
            volume,
            expression,
            resetControllers,
            pitchBend
        };

        struct AdapterEvent
        {
            AdapterEventType type =
                AdapterEventType::noteOff;
            Manual manual = Manual::upper;
            std::uint8_t data1 = 0;
            std::uint8_t data2 = 0;
            std::uint32_t sampleOffset = 0;
        };

        struct GainEvent
        {
            AdapterEventType type =
                AdapterEventType::volume;
            float target = 1.0f;
            std::uint32_t sampleOffset = 0;
        };

        struct ManualPerformanceState
        {
            std::array<bool, midiNoteCount> physicallyHeld {};
            std::array<bool, midiNoteCount> sustained {};
            bool sustainDown = false;
        };

        struct AtomicAutoMixSettings
        {
            std::atomic<std::uint32_t> enabled { 1 };
            AtomicFloat amount { 0.32f };
            AtomicFloat reactionMilliseconds { 720.0f };
            AtomicFloat recoveryMilliseconds { 1900.0f };
            AtomicFloat maximumAttenuationDecibels { 3.0f };
            AtomicFloat pianoConflict { 0.72f };
            AtomicFloat externalPadConflict { 0.86f };
            AtomicFloat externalStringsConflict { 0.58f };
            AtomicFloat masterProtection { 0.90f };
            AtomicFloat lowMidZone { 0.65f };
            AtomicFloat midZone { 0.72f };
            AtomicFloat soloRoomLiftDecibels { 0.45f };
            std::atomic<std::uint32_t> sidechainMask {
                autoMixAllSources
            };

            AtomicFloat pianoEnvelope { 0.0f };
            AtomicFloat externalPadEnvelope { 0.0f };
            AtomicFloat externalStringsEnvelope { 0.0f };
            AtomicFloat masterProximity { 0.0f };
        };

        static bool validFamily (
            ChurchOrganFamily family) noexcept;
        static bool validQuality (
            ChurchOrganLayerQuality quality) noexcept;
        static bool validManual (Manual manual) noexcept;
        static std::size_t familyIndex (
            ChurchOrganFamily family) noexcept;
        static std::size_t manualIndex (
            Manual manual) noexcept;
        static Manual manualForChannel (
            int midiChannel,
            bool& valid) noexcept;
        static int eventPriority (
            AdapterEventType type) noexcept;
        static float clampUnit (float value) noexcept;
        static float clampFinite (
            float value,
            float minimum,
            float maximum,
            float fallback) noexcept;
        static std::uint32_t floatBits (float value) noexcept;
        static float bitsFloat (std::uint32_t value) noexcept;

        static OrganQualityProfile tonewheelQuality (
            ChurchOrganLayerQuality quality) noexcept;
        static WorshipHybridQuality hybridQuality (
            ChurchOrganLayerQuality quality) noexcept;
        static PipeOrganQuality pipeQuality (
            ChurchOrganLayerQuality quality) noexcept;
        static PipeOrganManual pipeManual (Manual manual) noexcept;

        void initialiseAtomicDefaults() noexcept;
        bool prepareEngines (
            const ChurchOrganLayerPrepareSpec& spec) noexcept;
        void resetEnginesAndPerformance() noexcept;

        void collectMidi (
            const juce::MidiBuffer& midi) noexcept;
        bool queueAdapterEvent (
            const AdapterEvent& event) noexcept;
        void dispatchCurrentBlockEvents (
            std::size_t numSamples) noexcept;
        bool dispatchEvent (
            const AdapterEvent& event) noexcept;
        bool dispatchTonewheelEvent (
            const AdapterEvent& event) noexcept;
        bool dispatchTransistorEvent (
            const AdapterEvent& event) noexcept;
        bool dispatchPipeEvent (
            const AdapterEvent& event) noexcept;
        bool dispatchHybridEvent (
            const AdapterEvent& event) noexcept;
        void carryAdapterEvents (
            std::size_t firstUnprocessed,
            std::size_t numSamples) noexcept;
        bool appendGainEvent (
            const AdapterEvent& event) noexcept;

        bool dispatchExternalSustainNoteOn (
            const AdapterEvent& event) noexcept;
        bool dispatchExternalSustainNoteOff (
            const AdapterEvent& event) noexcept;
        bool dispatchExternalSustain (
            const AdapterEvent& event) noexcept;
        bool dispatchExternalAllNotesOff (
            const AdapterEvent& event) noexcept;
        bool queueActiveNoteOn (
            Manual manual,
            int note,
            int velocity,
            std::uint32_t sampleOffset) noexcept;
        bool queueActiveNoteOff (
            Manual manual,
            int note,
            std::uint32_t sampleOffset) noexcept;
        bool queueActiveAllNotesOff (
            Manual manual,
            std::uint32_t sampleOffset) noexcept;
        bool activeFamilySupportsManual (
            Manual manual) const noexcept;

        void applyPendingConfiguration (
            ChurchOrganFamily family) noexcept;
        void applyOrganParameters (
            ChurchOrganFamily family,
            const OrganParameterSnapshot& snapshot) noexcept;
        void applyTransistorConfiguration() noexcept;
        void applyPipeConfiguration() noexcept;
        void applyHybridConfiguration (
            const OrganParameterSnapshot& snapshot) noexcept;
        AutoMixParameters atomicAutoMixParameters() const noexcept;
        AutoMixControlFrame atomicAutoMixControls() const noexcept;

        bool renderActiveFamily (
            juce::AudioBuffer<float>& output,
            std::size_t startSample,
            std::size_t numSamples,
            juce::AudioBuffer<float>* reverbSend) noexcept;
        void applyControllerGain (
            juce::AudioBuffer<float>& output,
            std::size_t startSample,
            std::size_t numSamples,
            juce::AudioBuffer<float>* reverbSend) noexcept;
        void clearBuffers (
            juce::AudioBuffer<float>& output,
            juce::AudioBuffer<float>* reverbSend) noexcept;
        void publishDiagnostics (
            const juce::AudioBuffer<float>& output) noexcept;
        std::uint32_t activeNoteCount() const noexcept;

        bool stateOperationAllowed() const noexcept;
        void addGeneralState (
            juce::ValueTree& root) const;
        void addTransistorState (
            juce::ValueTree& root) const;
        void addPipeState (
            juce::ValueTree& root) const;
        void addHybridState (
            juce::ValueTree& root) const;
        bool restoreGeneralState (
            const juce::ValueTree& root,
            std::uint32_t schema,
            ChurchOrganLayerPrepareSpec& restoredSpec,
            ChurchOrganFamily& restoredFamily);
        void restoreTransistorState (
            const juce::ValueTree& root);
        void restorePipeState (
            const juce::ValueTree& root);
        void restoreHybridState (
            const juce::ValueTree& root);

        TonewheelOrganEngine tonewheel_;
        TransistorOrganEngine transistor_;
        PipeOrganEngine pipe_;
        WorshipHybridEngine hybrid_;
        OrganParameterStore parameters_;

        std::array<AtomicFloat, familyCount> familyOutputTrims_ {};

        std::atomic<std::uint32_t> transistorProfile_ { 0 };
        std::array<AtomicFloat, transistorTabCount>
            transistorTabs_ {};
        std::atomic<std::uint32_t> transistorVibratoEnabled_ {
            transistorDefaultVibratoEnabled ? 1u : 0u
        };
        AtomicFloat transistorVibratoDepth_ {
            transistorDefaultVibratoDepth
        };
        AtomicFloat transistorDrive_ { 0.18f };
        AtomicFloat transistorStereoWidth_ { 0.22f };

        std::array<std::array<AtomicFloat, pipeOrganRankCount>,
                   pipeOrganManualCount> pipeRankLevels_ {};
        std::array<std::array<std::atomic<std::uint32_t>,
                              pipeOrganManualCount>,
                   pipeOrganManualCount> pipeCouplers_ {};
        std::array<AtomicFloat, pipeOrganManualCount> pipeSwell_ {};
        AtomicFloat pipeCrescendo_ { 0.0f };
        std::array<std::atomic<std::uint32_t>, pipeOrganManualCount>
            pipeTremulantEnabled_ {};
        std::array<AtomicFloat, pipeOrganManualCount>
            pipeTremulantDepth_ {};
        std::array<AtomicFloat, pipeOrganManualCount>
            pipeTremulantRate_ {};
        std::atomic<std::uint32_t> pipeTemperament_ {
            static_cast<std::uint32_t> (
                PipeOrganTemperament::churchWell)
        };

        AtomicAutoMixSettings hybridAutoMix_ {};

        std::array<ManualPerformanceState, manualCount>
            performance_ {};
        std::array<AdapterEvent, maximumAdapterEvents> events_ {};
        std::array<GainEvent, maximumAdapterEvents> gainEvents_ {};
        std::size_t eventCount_ = 0;
        std::size_t gainEventCount_ = 0;

        AtomicFloat requestedMidiVolume_ { 1.0f };
        AtomicFloat requestedMidiExpression_ { 1.0f };
        float midiVolumeCurrent_ = 1.0f;
        float midiVolumeTarget_ = 1.0f;
        float midiExpressionCurrent_ = 1.0f;
        float midiExpressionTarget_ = 1.0f;
        float familyTrimCurrent_ = 1.0f;
        float controllerSmoothingStep_ = 1.0f;
        float trimSmoothingStep_ = 1.0f;

        std::array<std::uint32_t, familyCount>
            appliedParameterRevisions_ {};
        std::array<std::uint32_t, familyCount>
            appliedConfigurationRevisions_ {};
        std::atomic<std::uint32_t> configurationRevision_ { 1 };

        ChurchOrganLayerPrepareSpec spec_ {};
        std::atomic<std::uint32_t> activeFamily_ {
            static_cast<std::uint32_t> (
                ChurchOrganFamily::tonewheel)
        };
        std::atomic<std::uint32_t> suspended_ { 1 };
        std::atomic<std::uint32_t> insideProcess_ { 0 };
        std::atomic<std::uint32_t> prepared_ { 0 };

        std::uint64_t renderedSamples_ = 0;
        std::uint64_t receivedMidiEvents_ = 0;
        std::uint64_t droppedAdapterEvents_ = 0;
        std::uint64_t droppedEngineEvents_ = 0;
        std::uint64_t ignoredChannelEvents_ = 0;
        std::uint64_t unsupportedPitchEvents_ = 0;
        std::uint64_t invalidOutputSamples_ = 0;

        std::atomic<std::uint64_t> diagnosticRenderedSamples_ { 0 };
        std::atomic<std::uint64_t> diagnosticReceivedMidiEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticDroppedAdapterEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticDroppedEngineEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticIgnoredChannelEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticUnsupportedPitchEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticInvalidOutputSamples_ { 0 };
        std::atomic<std::uint32_t> diagnosticQueuedEvents_ { 0 };
        std::atomic<std::uint32_t> diagnosticActiveNotes_ { 0 };
        std::atomic<std::uint32_t> diagnosticPeakBits_ { 0 };

        bool atomicDefaultsInitialised_ = false;
    };
}
