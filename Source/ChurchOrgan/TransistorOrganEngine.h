#pragma once

#include "TransistorDividerBank.h"
#include "TransistorOrganTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    class TransistorOrganEngine
    {
    public:
        TransistorOrganEngine() noexcept;

        TransistorOrganEngine (const TransistorOrganEngine&) = delete;
        TransistorOrganEngine& operator= (
            const TransistorOrganEngine&) = delete;

        bool prepare (const TransistorOrganPrepareSpec& spec) noexcept;
        void reset() noexcept;

        // Substitui o bloco. right == nullptr seleciona mono.
        bool render (TransistorAudioBlockView output) noexcept;

        bool queueUpperNoteOn (
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueUpperNoteOff (
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueUpperAllNotesOff (
            std::uint32_t sampleOffset = 0) noexcept;

        // Setters devem ser entregues pela thread de audio.
        void setProfile (
            TransistorOrganProfile profile,
            bool loadDefaultTabs = false) noexcept;
        TransistorOrganProfile profile() const noexcept;
        void loadProfileDefaultTabs() noexcept;

        void setTabLevel (
            TransistorOrganTab tab,
            float levelZeroToOne) noexcept;
        float targetTabLevel (TransistorOrganTab tab) const noexcept;
        float currentTabLevel (TransistorOrganTab tab) const noexcept;

        void setVibratoEnabled (bool enabled) noexcept;
        void setVibratoDepth (float depthZeroToOne) noexcept;
        void setDrive (float driveZeroToOne) noexcept;
        void setOutputTrim (float trim) noexcept;
        void setStereoWidth (float widthZeroToOne) noexcept;
        // Zero retains the native fast electronic keying. Non-zero times
        // update the live per-note gates without resetting held voices.
        void setPerformanceEnvelopeMilliseconds (
            float attackMilliseconds,
            float releaseMilliseconds) noexcept;

        bool vibratoEnabled() const noexcept;
        float vibratoDepth() const noexcept;
        float drive() const noexcept;
        float outputTrim() const noexcept;
        float stereoWidth() const noexcept;

        std::size_t activeUpperNoteCount() const noexcept;
        TransistorOrganDiagnostics diagnostics() const noexcept;

        static const char* tabName (TransistorOrganTab tab) noexcept;
        static constexpr std::size_t maximumPolyphony =
            transistorMidiNoteCount;
        static constexpr bool renderUsesFixedStorage = true;

    private:
        struct NoteState
        {
            float gate = 0.0f;
            float clickEnvelope = 0.0f;
            float clickLowState = 0.0f;
            float clickHighState = 0.0f;
            std::uint64_t randomState = 1;
            std::uint32_t generation = 0;
            std::uint8_t velocity = 0;
            bool held = false;
        };

        struct RankRoute
        {
            std::array<std::int16_t, 2> wheelMidi {{ -1, -1 }};
        };

        static bool validSpec (
            const TransistorOrganPrepareSpec& spec) noexcept;
        static std::size_t tabIndex (TransistorOrganTab tab) noexcept;
        static float clampUnit (float value) noexcept;
        static float onePoleCoefficient (
            double sampleRate,
            float frequencyHz) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static float envelopeDecayCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static float nextNoise (std::uint64_t& state) noexcept;
        static int foldToDividerRange (int midiNote) noexcept;

        bool queueEvent (const TransistorOrganEvent& event) noexcept;
        void applyEvent (const TransistorOrganEvent& event) noexcept;
        void activateNote (int note, int velocity) noexcept;
        void releaseNote (int note) noexcept;
        void releaseAllNotes() noexcept;
        void ensureActive (int note) noexcept;
        void removeActiveAt (std::size_t activeIndex) noexcept;
        void rebuildActiveTapMask() noexcept;
        void buildRankRoutes() noexcept;
        void refreshProfileCoefficients() noexcept;
        float renderVoices() noexcept;
        float renderVoicesAggregated() noexcept;
        float renderKeyTransient (NoteState& note) noexcept;
        float processDriveAndDc (float input) noexcept;
        void processCabinet (
            float input,
            float& left,
            float& right) noexcept;
        float vibratoPitchMultiplier() noexcept;
        void publishDiagnostics (
            float peak,
            double sum,
            double sumSquares,
            std::size_t samples,
            std::uint64_t invalidSamples) noexcept;

        TransistorOrganPrepareSpec spec_ {};
        TransistorDividerBank dividerBank_;

        std::array<std::array<RankRoute, transistorTabCount>,
                   transistorMidiNoteCount> routes_ {};
        alignas(32)
        std::array<std::array<float, TransistorDividerBank::tapCount>,
                   transistorTabCount> offsetGateSums_ {};
        std::array<std::array<std::uint8_t,
                              transistorMidiNoteCount>,
                   transistorTabCount> offsetTapIndices_ {};
        std::array<NoteState, transistorMidiNoteCount> notes_ {};
        std::array<std::uint8_t, transistorMidiNoteCount>
            activeNotes_ {};
        std::array<std::int16_t, transistorMidiNoteCount>
            activeSlots_ {};
        std::size_t activeNoteCount_ = 0;
        std::uint64_t activeLowerTapMask_ = 0;
        std::uint32_t activeUpperTapMask_ = 0;

        std::array<TransistorOrganEvent,
                   transistorMaximumQueuedEvents> events_ {};
        std::size_t eventCount_ = 0;
        std::uint64_t droppedEvents_ = 0;

        std::array<float, transistorTabCount> tabTargets_ {};
        std::array<float, transistorTabCount> tabCurrents_ {};

        float gateAttackCoefficient_ = 1.0f;
        float gateReleaseCoefficient_ = 1.0f;
        float tabSmoothingCoefficient_ = 1.0f;
        float clickLowCoefficient_ = 1.0f;
        float clickHighCoefficient_ = 1.0f;
        float clickOnDecayCoefficient_ = 0.0f;
        float clickOffDecayCoefficient_ = 0.0f;
        float cabinetCoefficientLeft_ = 1.0f;
        float cabinetCoefficientRight_ = 1.0f;
        float dcBlockCoefficient_ = 0.99f;
        float allpassCoefficient_ = 0.42f;

        float vibratoSin_ = 0.0f;
        float vibratoCos_ = 1.0f;
        float vibratoSinStep_ = 0.0f;
        float vibratoCosStep_ = 1.0f;
        std::uint32_t vibratoNormaliseCounter_ = 0;
        float preDcInput_ = 0.0f;
        float preDcOutput_ = 0.0f;
        float postDcInput_ = 0.0f;
        float postDcOutput_ = 0.0f;
        float cabinetStateLeft_ = 0.0f;
        float cabinetStateRight_ = 0.0f;
        float allpassState_ = 0.0f;

        std::uint64_t renderedSamples_ = 0;
        std::uint64_t invalidSamples_ = 0;
        TransistorOrganDiagnostics diagnostics_ {};
        bool prepared_ = false;
    };
}
