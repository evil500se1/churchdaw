#pragma once

#include "TonewheelBank.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    struct TonewheelCharacterParameters
    {
        float leakage = 0.08f;
        float crosstalk = 0.05f;
        float hum = 0.0f;
        float age = 0.12f;
        float mechanicalNoise = 0.0f;
        float transformer = 0.32f;
        float mainsFrequencyHz = 60.0f;
        float smoothingMilliseconds = 40.0f;
    };

    // Caráter esparso do gerador. Reutiliza o TonewheelBank compartilhado:
    // não cria osciladores por tecla e não usa uma matriz densa.
    class TonewheelCharacter
    {
    public:
        static constexpr std::size_t leakageTapCount = 8;

        bool prepare (
            double sampleRate,
            std::uint64_t deterministicSeed,
            const TonewheelCharacterParameters& parameters = {}) noexcept;

        void reset() noexcept;
        void setParameters (
            const TonewheelCharacterParameters& parameters) noexcept;

        // Deve ser chamado exatamente uma vez por sample, antes das consultas
        // de leakage/crosstalk e antes de TonewheelBank::advance().
        void beginSample (const TonewheelBank& tonewheels) noexcept;

        float leakageSample() const noexcept;

        // wheelIndex é a roda principal usada por um contato. O retorno usa
        // apenas vizinhos esparsos e já inclui o ganho de crosstalk.
        float crosstalkSample (
            const TonewheelBank& tonewheels,
            std::size_t wheelIndex,
            float keyboardRegionZeroToOne) const noexcept;

        // Matching transformer original, leve e dependente de frequência.
        float processTransformer (float input) noexcept;

        const TonewheelCharacterParameters& targetParameters() const noexcept;

    private:
        static bool validSampleRate (double sampleRate) noexcept;
        static TonewheelCharacterParameters sanitise (
            const TonewheelCharacterParameters& parameters,
            double sampleRate) noexcept;
        static float smoothingCoefficient (
            double sampleRate,
            float milliseconds) noexcept;
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static float signedUnit (std::uint64_t value) noexcept;
        static float nextNoise (std::uint64_t& state) noexcept;

        void updateSmoothedParameters() noexcept;

        std::array<std::uint8_t, leakageTapCount> leakageWheels_ {};
        std::array<float, leakageTapCount> leakageWeights_ {};

        TonewheelCharacterParameters target_ {};
        TonewheelCharacterParameters current_ {};
        double sampleRate_ = 48000.0;
        std::uint64_t seed_ = 0;
        std::uint64_t noiseState_ = 1;

        float smoothingCoefficient_ = 1.0f;
        float humPhase_ = 0.0f;
        float humIncrement_ = 0.0f;
        float mechanicalState_ = 0.0f;
        float leakageOutput_ = 0.0f;
        float transformerLowState_ = 0.0f;
        float transformerPreviousInput_ = 0.0f;
        float transformerDcState_ = 0.0f;
        float transformerLowCoefficient_ = 0.0f;
        float dcPole_ = 0.995f;
        bool prepared_ = false;
    };
}
