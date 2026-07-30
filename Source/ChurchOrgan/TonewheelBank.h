#pragma once

#include "HarmonicFoldback.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    // Banco cromatico compartilhado. Cada roda e atualizada uma unica vez por
    // sample, independentemente de quantas teclas/drawbars a reutilizem.
    class TonewheelBank
    {
    public:
        static constexpr std::size_t wheelCount =
            static_cast<std::size_t> (HarmonicFoldback::wheelCount);

        bool prepare (double sampleRate,
                      std::uint64_t deterministicSeed,
                      float maximumOffsetCents) noexcept;

        void resetPhases() noexcept;
        void advance() noexcept;

        float sample (std::size_t wheelIndex) const noexcept;
        double frequency (std::size_t wheelIndex) const noexcept;
        double frequencyForMidi (int midiNote) const noexcept;

        static double equalTemperedFrequency (int midiNote) noexcept;

    private:
        static std::uint64_t mixedHash (std::uint64_t value) noexcept;
        static double unitFromHash (std::uint64_t value) noexcept;

        std::array<float, wheelCount> real_ {};
        std::array<float, wheelCount> imaginary_ {};
        std::array<float, wheelCount> rotationReal_ {};
        std::array<float, wheelCount> rotationImaginary_ {};
        std::array<double, wheelCount> frequencies_ {};

        double sampleRate_ = 48000.0;
        std::uint64_t seed_ = 0;
        std::uint32_t samplesSinceNormalisation_ = 0;
        bool prepared_ = false;
    };
}
