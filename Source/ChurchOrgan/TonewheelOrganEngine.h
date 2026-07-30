#pragma once

#include "ChurchOrganTypes.h"
#include "HarmonicFoldback.h"
#include "HarmonicPercussion.h"
#include "KeyClickGenerator.h"
#include "MultiContactKeying.h"
#include "OrganAmpCab.h"
#include "OrganParameters.h"
#include "RotaryCabinet.h"
#include "ScannerVibrato.h"
#include "TonewheelBank.h"
#include "TonewheelCharacter.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class OrganQualityProfile : std::uint8_t
    {
        eco = 0,
        standard,
        ultra
    };

    struct TonewheelOrganPrepareSpec
    {
        double sampleRate = 48000.0;
        std::size_t maximumBlockSize = 512;
        std::uint64_t deterministicSeed = 0x4348555243484f52ULL;
        OrganQualityProfile quality = OrganQualityProfile::standard;
    };

    struct TonewheelOrganDiagnostics
    {
        std::uint64_t totalRenderedSamples = 0;
        std::uint64_t droppedEvents = 0;
        std::uint64_t invalidOutputSamples = 0;
        std::uint32_t activeUpperNotes = 0;
        std::uint32_t activeLowerNotes = 0;
        std::uint32_t activePedalNotes = 0;
        std::uint32_t queuedEvents = 0;
        float lastBlockPeak = 0.0f;
        float rotaryHornSpeedHz = 0.0f;
        float rotaryDrumSpeedHz = 0.0f;
    };

    // Família Tonewheel completa e independente da UI/JUCE. O banco de rodas
    // é compartilhado por Upper/Lower/Pedal; os eventos mantêm sampleOffset.
    class TonewheelOrganEngine
    {
    public:
        static constexpr std::size_t maximumBlockCapacity = 4096;
        static constexpr std::size_t maximumEvents = 1024;
        static constexpr std::size_t manualCount = 3;

        TonewheelOrganEngine() noexcept;

        TonewheelOrganEngine (const TonewheelOrganEngine&) = delete;
        TonewheelOrganEngine& operator= (const TonewheelOrganEngine&) = delete;

        bool prepare (const TonewheelOrganPrepareSpec& spec) noexcept;
        void reset() noexcept;

        bool render (AudioBlockView output) noexcept;

        bool queueNoteOn (
            Manual manual,
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueNoteOff (
            Manual manual,
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueAllNotesOff (
            Manual manual,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueExpression (
            int valueZeroTo127,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueVolume (
            int valueZeroTo127,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueSustain (
            Manual manual,
            bool down,
            std::uint32_t sampleOffset = 0) noexcept;

        OrganParameterStore& parameters() noexcept;
        const OrganParameterStore& parameters() const noexcept;

        TonewheelOrganDiagnostics diagnostics() const noexcept;
        const TonewheelOrganPrepareSpec& prepareSpec() const noexcept;

        static float drawbarGain (float positionZeroToEight) noexcept;

        static constexpr bool renderUsesFixedStorage = true;

    private:
        struct ManualState
        {
            MultiContactKeying contacts;
            KeyClickGenerator clicks;
            std::array<std::uint8_t, midiNoteCount> activeNotes {};
            std::array<std::int16_t, midiNoteCount> activeSlots {};
            std::array<std::uint8_t, midiNoteCount> velocities {};
            std::array<bool, midiNoteCount> physicallyHeld {};
            std::array<bool, midiNoteCount> sustained {};
            std::size_t activeCount = 0;
            bool sustainDown = false;
        };

        static std::size_t manualIndex (Manual manual) noexcept;
        static bool validManual (Manual manual) noexcept;
        static bool validNote (int note) noexcept;
        static int eventPriority (EventType type) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static float decibelsToGain (float decibels) noexcept;
        static std::uint32_t floatBits (float value) noexcept;
        static float bitsFloat (std::uint32_t value) noexcept;

        bool queueEvent (const NoteEvent& event) noexcept;
        void applyEvent (const NoteEvent& event) noexcept;
        void applyNoteOn (Manual manual, int note, int velocity) noexcept;
        void applyNoteOff (Manual manual, int note) noexcept;
        void applyAllNotesOff (Manual manual) noexcept;
        void applySustain (Manual manual, bool down) noexcept;

        void activateManualNote (
            ManualState& state,
            int note,
            int velocity,
            bool triggerClick) noexcept;
        void releaseManualNote (
            ManualState& state,
            int note,
            bool triggerClick) noexcept;
        void ensureManualActive (
            ManualState& state,
            int note) noexcept;
        void removeManualActiveAt (
            ManualState& state,
            std::size_t activeIndex) noexcept;

        void refreshPedalVoices() noexcept;
        void updatePedalMode (bool polyphonic) noexcept;
        void updateBlockTargets (
            const OrganParameterSnapshot& snapshot) noexcept;
        void configureCharacter (
            const OrganParameterSnapshot& snapshot) noexcept;
        void configurePercussion (
            const OrganParameterSnapshot& snapshot) noexcept;
        void configureScanner (
            const OrganParameterSnapshot& snapshot) noexcept;
        void configureAmp (
            const OrganParameterSnapshot& snapshot) noexcept;
        void configureRotary (
            const OrganParameterSnapshot& snapshot) noexcept;

        float renderManualSample (
            std::size_t manual,
            float clickAmount) noexcept;
        float renderPercussionSample() noexcept;
        std::size_t totalActiveNotes() const noexcept;
        void publishDiagnostics (
            float peak,
            std::uint64_t invalidOutputSamples) noexcept;

        TonewheelOrganPrepareSpec spec_ {};
        TonewheelBank tonewheels_;
        TonewheelCharacter character_;
        std::array<ManualState, manualCount> manuals_ {};
        HarmonicPercussion percussion_;
        std::array<ScannerVibrato, manualCount> scanners_ {};
        OrganAmpCab amp_;
        RotaryCabinet rotary_;
        OrganParameterStore parameters_;

        std::array<std::array<std::uint8_t, drawbarCount>,
                   midiNoteCount> wheelMap_ {};
        std::array<std::array<float, drawbarCount>,
                   manualCount> drawbarCurrentGains_ {};
        std::array<std::array<float, drawbarCount>,
                   manualCount> drawbarTargetGains_ {};

        std::array<NoteEvent, maximumEvents> events_ {};
        std::size_t eventCount_ = 0;
        std::uint64_t droppedEvents_ = 0;

        std::array<std::array<float, maximumBlockCapacity>,
                   manualCount> manualLeft_ {};
        std::array<std::array<float, maximumBlockCapacity>,
                   manualCount> manualRight_ {};
        std::array<float, maximumBlockCapacity> mixLeft_ {};
        std::array<float, maximumBlockCapacity> mixRight_ {};

        std::array<bool, midiNoteCount> pedalPhysicallyHeld_ {};
        std::array<std::uint8_t, midiNoteCount> pedalVelocities_ {};
        int monophonicPedalNote_ = -1;
        bool pedalPolyphonic_ = false;
        bool percussionEnabled_ = false;

        float drawbarSmoothingCoefficient_ = 1.0f;
        float controlSmoothingCoefficient_ = 1.0f;
        float expressionCurrent_ = 1.0f;
        float expressionTarget_ = 1.0f;
        float midiVolumeCurrent_ = 1.0f;
        float midiVolumeTarget_ = 1.0f;
        float masterGainCurrent_ = 0.5f;
        float masterGainTarget_ = 0.5f;
        float autoHeadroomCurrent_ = 1.0f;
        float clickAmount_ = 0.34f;
        float baseOutputTrim_ = 0.13f;
        std::uint32_t lastParameterRevision_ = 0;
        bool prepared_ = false;

        std::atomic<std::uint64_t> totalRenderedSamples_ { 0 };
        std::atomic<std::uint64_t> diagnosticsDroppedEvents_ { 0 };
        std::atomic<std::uint64_t> diagnosticsInvalidOutput_ { 0 };
        std::atomic<std::uint32_t> diagnosticsActiveUpper_ { 0 };
        std::atomic<std::uint32_t> diagnosticsActiveLower_ { 0 };
        std::atomic<std::uint32_t> diagnosticsActivePedal_ { 0 };
        std::atomic<std::uint32_t> diagnosticsQueuedEvents_ { 0 };
        std::atomic<std::uint32_t> diagnosticsPeakBits_ { 0 };
        std::atomic<std::uint32_t> diagnosticsHornSpeedBits_ { 0 };
        std::atomic<std::uint32_t> diagnosticsDrumSpeedBits_ { 0 };
    };
}
