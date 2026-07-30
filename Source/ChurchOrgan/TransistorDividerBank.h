#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    struct TransistorDividerTap
    {
        float square = 0.0f;
        float pulse = 0.0f;
        float ramp = 0.0f;
    };

    // Doze cadeias cromaticas livres. Cada tap de oitava e derivado do mesmo
    // acumulador fixed-point, como uma cadeia top-octave/divider idealizada.
    class TransistorDividerBank
    {
    public:
        static constexpr int lowestMidi = 24;
        static constexpr int highestMidi = 119;
        static constexpr std::size_t pitchClassCount = 12;
        static constexpr std::size_t tapCount =
            static_cast<std::size_t> (highestMidi - lowestMidi + 1);

        bool prepare (
            double sampleRate,
            std::uint64_t deterministicSeed) noexcept;
        void resetPhases() noexcept;

        // Atualiza todos os 96 taps e depois avanca os 12 contadores mestres.
        // pitchMultiplier deve ficar proximo de 1.0.
        void advance (float pitchMultiplier = 1.0f) noexcept;
        // Define os taps usados pelo motor sem alocar. A selecao so muda em
        // eventos de nota; advanceActive() continua livre e sample-accurate.
        void setActiveTapMask (
            std::uint64_t lowerTapMask,
            std::uint32_t upperTapMask) noexcept;
        void advanceActive (
            float pitchMultiplier = 1.0f) noexcept;

        TransistorDividerTap tapForMidi (int midiNote) const noexcept;
        // Fast path para rotas pre-validadas em prepare().
        const TransistorDividerTap& tapAt (
            std::size_t tapIndex) const noexcept
        {
            return taps_[tapIndex];
        }
        float squareForMidi (int midiNote) const noexcept;
        float pulseForMidi (int midiNote) const noexcept;
        float rampForMidi (int midiNote) const noexcept;
        double frequencyForMidi (int midiNote) const noexcept;

        static double equalTemperedFrequency (int midiNote) noexcept;

    private:
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static float phaseUnit (std::uint32_t phase) noexcept;
        static float polyBlep (float phase, float phaseDelta) noexcept;
        static void renderWaveforms (
            float phase,
            float phaseDelta,
            TransistorDividerTap& tap) noexcept;

        // Q0.32 oferece resolucao de afinacao melhor que 0,005 cent no
        // pior caso suportado e torna o hot path inteiro/SIMD-friendly.
        std::array<std::uint32_t, pitchClassCount> masterPhases_ {};
        std::array<std::uint32_t, pitchClassCount> baseIncrements_ {};
        std::array<std::uint32_t, pitchClassCount>
            modulatedIncrements_ {};
        std::array<std::uint8_t, tapCount> activeTapIndices_ {};
        std::array<std::uint8_t, tapCount>
            activeTapPitchClasses_ {};
        std::array<std::uint8_t, tapCount>
            activeTapOctaves_ {};
        std::size_t activeTapCount_ = 0;
        std::array<double, tapCount> frequencies_ {};
        std::array<TransistorDividerTap, tapCount> taps_ {};

        double sampleRate_ = 48000.0;
        std::uint64_t seed_ = 0;
        bool prepared_ = false;
    };
}
