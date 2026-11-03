#pragma once

#include "WorshipDspUtilities.h"
#include "WorshipLayerTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    class WorshipLayerEngine
    {
    public:
        static constexpr std::size_t phaseTableSize = 1024;
        static_assert (
            (phaseTableSize & (phaseTableSize - 1)) == 0,
            "Worship phase table must be a power of two");

        WorshipLayerEngine() noexcept;

        WorshipLayerEngine (const WorshipLayerEngine&) = delete;
        WorshipLayerEngine& operator= (
            const WorshipLayerEngine&) = delete;

        bool prepare (const WorshipPrepareSpec& spec) noexcept;
        void reset() noexcept;

        void setLayerParameters (
            WorshipLayer layer,
            const WorshipLayerParameters& parameters) noexcept;
        WorshipLayerParameters layerParameters (
            WorshipLayer layer) const noexcept;
        static WorshipLayerParameters defaultParameters (
            WorshipLayer layer) noexcept;

        void setWorshipIntensity (float intensity) noexcept;
        float targetWorshipIntensity() const noexcept;
        static WorshipIntensityProfile intensityProfile (
            float intensity) noexcept;

        void setOutputGain (float gain) noexcept;
        void setExternalDucking (float amount) noexcept;
        void setAutoMixLayerGain (float linearGain) noexcept;

        bool queueNoteOn (
            int note,
            int velocity,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueNoteOff (
            int note,
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueAllNotesOff (
            std::uint32_t sampleOffset = 0) noexcept;
        bool queueExpression (
            float expression,
            std::uint32_t sampleOffset = 0) noexcept;

        // Adds the internal layer and optional reverb send to the supplied
        // channel. No buffer is retained after this call.
        void renderAdd (const WorshipAudioBlock& block) noexcept;

        WorshipDiagnostics diagnostics() const noexcept;
        std::size_t voiceLimit() const noexcept;

        static constexpr bool renderUsesFixedStorage = true;
        static constexpr bool renderUsesLocks = false;

    private:
        struct Rotation
        {
            float sine = 0.0f;
            float cosine = 1.0f;
        };

        struct Oscillator
        {
            float sine = 0.0f;
            float cosine = 1.0f;
            Rotation rotation {};

            void advance() noexcept;
            void setState (
                float sineValue,
                float cosineValue) noexcept;
            void stabilise() noexcept;
        };

        struct Voice
        {
            bool active = false;
            bool held = false;
            bool harmonic2Allowed = true;
            bool harmonic3Allowed = true;
            std::uint8_t note = 0;
            std::uint8_t velocity = 0;
            float pan = 0.0f;
            Oscillator primary {};
            Oscillator detuned {};
            std::array<float, worshipLayerCount> envelopes {};
            std::array<float, worshipLayerCount> lastLeft {};
            std::array<float, worshipLayerCount> lastRight {};
            std::uint32_t noiseState = 1;
            std::uint32_t stabiliseCounter = 0;
            std::uint64_t ageSamples = 0;
        };

        struct LayerRuntime
        {
            worshipdsp::SmoothedValue level;
            worshipdsp::SmoothedValue intensityGain;
            worshipdsp::SmoothedValue width;
            worshipdsp::SmoothedValue lowPassStep;
            worshipdsp::SmoothedValue highPassStep;
            worshipdsp::SmoothedValue reverbSend;
            float attackStep = 1.0f;
            float releaseStep = 1.0f;
            float leftLowPass = 0.0f;
            float rightLowPass = 0.0f;
            float leftHighPassMemory = 0.0f;
            float rightHighPassMemory = 0.0f;
        };

        struct StereoSample
        {
            float left = 0.0f;
            float right = 0.0f;
        };

        static std::size_t limitForQuality (
            WorshipQuality quality) noexcept;
        static float densityForQuality (
            WorshipQuality quality) noexcept;
        static std::uint32_t nextNoise (
            std::uint32_t& state) noexcept;
        static std::uint64_t hash (
            std::uint64_t value) noexcept;

        bool queueEvent (const WorshipEvent& event) noexcept;
        void applyEvent (const WorshipEvent& event) noexcept;
        void activateNote (int note, int velocity) noexcept;
        void releaseNote (int note) noexcept;
        void releaseAllNotes() noexcept;
        std::size_t chooseVoice (int note) noexcept;
        void initialiseVoice (
            Voice& voice,
            std::size_t voiceIndex,
            int note,
            int velocity) noexcept;

        StereoSample componentSample (
            WorshipLayer layer,
            const Voice& voice,
            float noiseLeft,
            float noiseRight,
            float envelope,
            float width) const noexcept;
        float envelopeTarget (
            const Voice& voice,
            std::size_t layerIndex) const noexcept;
        bool voiceFinished (const Voice& voice) const noexcept;
        void updateLayerTargets (
            std::size_t layerIndex) noexcept;
        void updateAllTargets() noexcept;
        void carryUnprocessedEvents (
            std::size_t firstUnprocessed,
            std::size_t renderedSamples) noexcept;
        void publishDiagnostics (
            float layerPeak,
            float reverbPeak) noexcept;

        WorshipPrepareSpec spec_ {};
        std::array<WorshipLayerParameters, worshipLayerCount>
            layerParameters_ {};
        std::array<LayerRuntime, worshipLayerCount> layerRuntime_ {};
        std::array<Voice, maximumWorshipVoices> voices_ {};
        std::array<WorshipEvent, maximumWorshipEvents> events_ {};
        std::array<Rotation, 128> primaryRotation_ {};
        std::array<Rotation, 128> detunedRotation_ {};
        std::array<bool, 128> audibleNote_ {};
        std::array<bool, 128> harmonic2Allowed_ {};
        std::array<bool, 128> harmonic3Allowed_ {};
        std::array<float, phaseTableSize> phaseSine_ {};
        std::array<float, phaseTableSize> phaseCosine_ {};

        worshipdsp::SmoothedValue intensity_;
        worshipdsp::SmoothedValue loudnessCompensation_;
        worshipdsp::SmoothedValue expression_;
        worshipdsp::SmoothedValue outputGain_;
        worshipdsp::SmoothedValue externalDucking_;
        worshipdsp::SmoothedValue autoMixLayerGain_;

        std::size_t eventCount_ = 0;
        std::size_t voiceLimit_ = 0;
        float density_ = 1.0f;
        float targetIntensity_ = 0.35f;
        float targetOutputGain_ = 0.72f;
        float targetExternalDucking_ = 0.0f;
        float targetAutoMixLayerGain_ = 1.0f;
        std::uint64_t activationSerial_ = 0;
        std::uint64_t renderedSamples_ = 0;
        std::uint64_t droppedEvents_ = 0;
        std::array<float, worshipLayerCount> stealDeclickLeft_ {};
        std::array<float, worshipLayerCount> stealDeclickRight_ {};
        float stealDeclickDecay_ = 0.995f;
        std::uint64_t stolenVoices_ = 0;
        WorshipDiagnostics diagnostics_ {};
        bool prepared_ = false;
    };
}
