#pragma once

#include "PipeOrganTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    class PipeOrganEngine
    {
    public:
        PipeOrganEngine() noexcept;

        PipeOrganEngine (const PipeOrganEngine&) = delete;
        PipeOrganEngine& operator= (const PipeOrganEngine&) = delete;

        bool prepare (const PipeOrganPrepareSpec& spec) noexcept;
        void reset() noexcept;
        bool render (PipeOrganAudioBlockView output) noexcept;

        bool queueNoteOn (
            PipeOrganManual manual,
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueNoteOff (
            PipeOrganManual manual,
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueAllNotesOff (
            PipeOrganManual manual,
            std::uint32_t sampleOffset = 0) noexcept;

        // Parameter setters are lock-free by ownership: call them on the
        // audio/control handoff thread, never concurrently with render().
        void setRankLevel (
            PipeOrganManual manual,
            PipeOrganRank rank,
            float levelZeroToOne) noexcept;
        float targetRankLevel (
            PipeOrganManual manual,
            PipeOrganRank rank) const noexcept;
        float currentRankLevel (
            PipeOrganManual manual,
            PipeOrganRank rank) const noexcept;

        void setCoupler (
            PipeOrganManual source,
            PipeOrganManual destination,
            bool enabled) noexcept;
        bool couplerEnabled (
            PipeOrganManual source,
            PipeOrganManual destination) const noexcept;

        void setSwell (
            PipeOrganManual manual,
            float levelZeroToOne) noexcept;
        float targetSwell (PipeOrganManual manual) const noexcept;
        void setCrescendo (float levelZeroToOne) noexcept;
        float targetCrescendo() const noexcept;

        void setTremulant (
            PipeOrganManual manual,
            bool enabled,
            float depthZeroToOne,
            float rateHz) noexcept;
        void setTemperament (
            PipeOrganTemperament temperament) noexcept;
        PipeOrganTemperament temperament() const noexcept;
        // Zero preserves every rank's native speech/release articulation.
        // A non-zero override changes only the tonal per-pipe envelope; chiff,
        // speech transient, wind and rank spectra remain family-authentic.
        void setPerformanceEnvelopeMilliseconds (
            float attackMilliseconds,
            float releaseMilliseconds) noexcept;

        void loadDefaultRegistration() noexcept;
        std::size_t activeDivisionVoiceCount() const noexcept;
        PipeOrganDiagnostics diagnostics() const noexcept;

        static const char* rankName (PipeOrganRank rank) noexcept;
        static constexpr std::size_t maximumDivisionVoices =
            pipeOrganManualCount * pipeOrganMidiNoteCount;
        static constexpr bool renderUsesFixedStorage = true;

    private:
        static constexpr std::size_t sineTableSize = 8192;
        static constexpr std::size_t pipeWaveTableSize = 1024;
        static constexpr std::size_t maximumPartials = 12;
        using PipeWaveTable =
            std::array<float, pipeWaveTableSize>;

        struct PipeState
        {
            float phase = 0.0f;
            float secondaryPhase = 0.0f;
            float phaseIncrement = 0.0f;
            float secondaryIncrement = 0.0f;
            float envelope = 0.0f;
            float speechEnvelope = 0.0f;
            float chiffEnvelope = 0.0f;
            float wind = 0.0f;
            float windTarget = 0.0f;
            float noiseLow = 0.0f;
            float noiseHigh = 0.0f;
            std::uint64_t randomState = 1;
            bool speaking = false;
        };

        struct DivisionVoice
        {
            std::array<PipeState, pipeOrganRankCount> pipes {};
            std::uint32_t generation = 0;
            std::uint8_t velocity = 0;
            std::uint16_t speakingMask = 0;
        };

        struct TremulantState
        {
            float phase = 0.0f;
            float depth = 0.42f;
            float rateHz = 5.8f;
            bool enabled = false;
        };

        static bool validSpec (
            const PipeOrganPrepareSpec& spec) noexcept;
        static std::size_t manualIndex (PipeOrganManual manual) noexcept;
        static std::size_t rankIndex (PipeOrganRank rank) noexcept;
        static float clampUnit (float value) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static float lowpassCoefficient (
            double sampleRate,
            float frequencyHz) noexcept;
        static float envelopeDecayCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static double unitFromHash (std::uint64_t value) noexcept;
        static float nextNoise (std::uint64_t& state) noexcept;
        static float centsRatio (float cents) noexcept;
        static float smoothstep (float value) noexcept;

        bool queueEvent (const PipeOrganEvent& event) noexcept;
        void applyEvent (const PipeOrganEvent& event) noexcept;
        void activateNote (
            PipeOrganManual manual,
            int note,
            int velocity) noexcept;
        void releaseNote (
            PipeOrganManual manual,
            int note) noexcept;
        void releaseAllNotes (PipeOrganManual manual) noexcept;
        bool desiredGate (
            std::size_t destinationManual,
            std::size_t note) const noexcept;
        void ensureActive (
            std::size_t manual,
            std::size_t note) noexcept;
        void ensureCoupledDestinations (
            std::size_t sourceManual,
            std::size_t note) noexcept;
        void removeActiveAt (std::size_t activeIndex) noexcept;

        void buildSineTable() noexcept;
        void buildPipeWaveTables() noexcept;
        void rebuildFrequencies() noexcept;
        void rebuildPanGains() noexcept;
        void refreshQualityAndRankCoefficients() noexcept;
        float waveLookup (
            const PipeWaveTable& table,
            float phase) const noexcept;
        void startPipe (
            PipeState& pipe,
            std::size_t manual,
            std::size_t note,
            std::size_t rank,
            std::uint32_t generation) noexcept;
        float renderPipe (
            PipeState& pipe,
            std::size_t manual,
            std::size_t note,
            std::size_t rank,
            float effectiveLevel,
            bool gate,
            float tremulantValue) noexcept;
        void renderVoices (
            float& left,
            float& right,
            std::uint32_t& soundingPipes) noexcept;
        void updateControlsAndWindLfos() noexcept;
        float effectiveRankLevel (
            std::size_t manual,
            std::size_t rank) const noexcept;
        float tremulantValue (std::size_t manual) const noexcept;
        void processOutput (
            float inputLeft,
            float inputRight,
            float& outputLeft,
            float& outputRight) noexcept;
        void publishDiagnostics (
            float peak,
            double sum,
            double sumSquares,
            std::size_t samples,
            std::uint32_t soundingPipes) noexcept;

        PipeOrganPrepareSpec spec_ {};

        std::array<float, sineTableSize> sineTable_ {};
        std::array<std::array<PipeWaveTable, maximumPartials>,
                   pipeOrganRankCount> normalWaveTables_ {};
        std::array<std::array<PipeWaveTable, maximumPartials>,
                   pipeOrganRankCount> brightWaveTables_ {};
        std::array<std::array<float, pipeOrganMidiNoteCount>,
                   pipeOrganRankCount> frequencies_ {};
        std::array<std::array<std::uint8_t, pipeOrganMidiNoteCount>,
                   pipeOrganRankCount> allowedPartials_ {};
        std::array<float, pipeOrganRankCount> harmonicNormalisation_ {};
        std::array<float, pipeOrganRankCount> attackCoefficients_ {};
        std::array<float, pipeOrganRankCount> releaseCoefficients_ {};
        std::array<float, pipeOrganRankCount> speechDecayCoefficients_ {};
        std::array<float, pipeOrganRankCount> chiffDecayCoefficients_ {};

        std::array<std::array<DivisionVoice, pipeOrganMidiNoteCount>,
                   pipeOrganManualCount> voices_ {};
        std::array<std::array<bool, pipeOrganMidiNoteCount>,
                   pipeOrganManualCount> held_ {};
        std::array<std::uint16_t, maximumDivisionVoices>
            activeVoiceIds_ {};
        std::array<std::int16_t, maximumDivisionVoices>
            activeSlots_ {};
        std::size_t activeVoiceCount_ = 0;

        std::array<std::array<bool, pipeOrganManualCount>,
                   pipeOrganManualCount> couplers_ {};
        std::array<std::array<float, pipeOrganRankCount>,
                   pipeOrganManualCount> rankTargets_ {};
        std::array<std::array<float, pipeOrganRankCount>,
                   pipeOrganManualCount> rankCurrents_ {};
        std::array<float, pipeOrganManualCount> swellTargets_ {};
        std::array<float, pipeOrganManualCount> swellCurrents_ {};
        std::array<TremulantState, pipeOrganManualCount> tremulants_ {};
        std::array<float, pipeOrganManualCount> tremulantSamples_ {};
        std::array<std::array<float, pipeOrganRankCount>,
                   pipeOrganManualCount> effectiveRankLevels_ {};
        std::array<std::uint16_t, pipeOrganManualCount>
            effectiveRankMasks_ {};
        std::array<std::uint8_t, 1U << pipeOrganRankCount>
            firstSetBit_ {};

        std::array<std::array<std::array<float, pipeOrganMidiNoteCount>,
                              pipeOrganRankCount>,
                   pipeOrganManualCount> panLeft_ {};
        std::array<std::array<std::array<float, pipeOrganMidiNoteCount>,
                              pipeOrganRankCount>,
                   pipeOrganManualCount> panRight_ {};

        std::array<float, pipeOrganManualCount> windPressure_ {};
        std::array<float, pipeOrganManualCount> windNoise_ {};
        std::array<float, pipeOrganManualCount> windNoiseTarget_ {};
        std::array<std::uint64_t, pipeOrganManualCount> windRandom_ {};
        std::array<float, pipeOrganManualCount> lastWindLoad_ {};

        std::array<PipeOrganEvent, pipeOrganMaximumEvents> events_ {};
        std::size_t eventCount_ = 0;
        std::uint64_t droppedEvents_ = 0;
        std::uint64_t renderedSamples_ = 0;
        std::uint64_t invalidSamples_ = 0;
        std::uint64_t sampleCounter_ = 0;

        float crescendoTarget_ = 0.0f;
        float crescendoCurrent_ = 0.0f;
        float performanceAttackMilliseconds_ = 0.0f;
        float performanceReleaseMilliseconds_ = 0.0f;
        float rankSmoothingCoefficient_ = 1.0f;
        float swellSmoothingCoefficient_ = 1.0f;
        float crescendoSmoothingCoefficient_ = 1.0f;
        float windPressureCoefficient_ = 1.0f;
        float windNoiseCoefficient_ = 1.0f;
        float pipeWindCoefficient_ = 1.0f;
        float chiffLowCoefficient_ = 1.0f;
        float chiffHighCoefficient_ = 1.0f;
        float dcCoefficient_ = 0.99f;
        float dcInputLeft_ = 0.0f;
        float dcOutputLeft_ = 0.0f;
        float dcInputRight_ = 0.0f;
        float dcOutputRight_ = 0.0f;
        std::size_t partialLimit_ = 8;
        PipeOrganDiagnostics diagnostics_ {};
        bool prepared_ = false;
    };
}
