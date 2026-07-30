#include "TonewheelBank.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double pi =
            3.141592653589793238462643383279502884;
        constexpr double twoPi = 2.0 * pi;
    }

    bool TonewheelBank::prepare (
        double sampleRate,
        std::uint64_t deterministicSeed,
        float maximumOffsetCents) noexcept
    {
        if (! std::isfinite (sampleRate)
             || sampleRate < 32000.0
             || sampleRate > 384000.0)
            return false;

        sampleRate_ = sampleRate;
        seed_ = deterministicSeed;
        maximumOffsetCents = std::max (
            0.0f, std::min (2.0f, maximumOffsetCents));

        for (std::size_t wheel = 0; wheel < wheelCount; ++wheel)
        {
            const auto pitchHash = mixedHash (
                seed_ ^ (0x9e3779b97f4a7c15ULL * (wheel + 1)));
            const double signedUnit =
                2.0 * unitFromHash (pitchHash) - 1.0;
            const double cents =
                static_cast<double> (maximumOffsetCents) * signedUnit;
            const int midi =
                HarmonicFoldback::lowestWheelMidi
                + static_cast<int> (wheel);
            const double nominal = equalTemperedFrequency (midi);
            const double calibrated =
                nominal * std::pow (2.0, cents / 1200.0);
            const double radians = twoPi * calibrated / sampleRate_;

            frequencies_[wheel] = calibrated;
            rotationReal_[wheel] =
                static_cast<float> (std::cos (radians));
            rotationImaginary_[wheel] =
                static_cast<float> (std::sin (radians));
        }

        prepared_ = true;
        resetPhases();
        return true;
    }

    void TonewheelBank::resetPhases() noexcept
    {
        if (! prepared_)
            return;

        for (std::size_t wheel = 0; wheel < wheelCount; ++wheel)
        {
            const auto phaseHash = mixedHash (
                seed_ + 0xd1b54a32d192ed03ULL * (wheel + 1));
            const double phase = twoPi * unitFromHash (phaseHash);
            real_[wheel] = static_cast<float> (std::cos (phase));
            imaginary_[wheel] = static_cast<float> (std::sin (phase));
        }

        samplesSinceNormalisation_ = 0;
    }

    void TonewheelBank::advance() noexcept
    {
        for (std::size_t wheel = 0; wheel < wheelCount; ++wheel)
        {
            const float real = real_[wheel];
            const float imaginary = imaginary_[wheel];
            real_[wheel] =
                real * rotationReal_[wheel]
                - imaginary * rotationImaginary_[wheel];
            imaginary_[wheel] =
                imaginary * rotationReal_[wheel]
                + real * rotationImaginary_[wheel];
        }

        // Uma iteracao de Newton mantem o modulo unitario sem sqrt e sem criar
        // um pico de custo perceptivel. A posicao deste passo depende apenas do
        // total de samples, nao do tamanho dos blocos.
        if (++samplesSinceNormalisation_ >= 2048)
        {
            for (std::size_t wheel = 0; wheel < wheelCount; ++wheel)
            {
                const float magnitudeSquared =
                    real_[wheel] * real_[wheel]
                    + imaginary_[wheel] * imaginary_[wheel];
                const float correction =
                    1.5f - 0.5f * magnitudeSquared;
                real_[wheel] *= correction;
                imaginary_[wheel] *= correction;
            }

            samplesSinceNormalisation_ = 0;
        }
    }

    float TonewheelBank::sample (std::size_t wheelIndex) const noexcept
    {
        return wheelIndex < wheelCount
                   ? imaginary_[wheelIndex]
                   : 0.0f;
    }

    double TonewheelBank::frequency (
        std::size_t wheelIndex) const noexcept
    {
        return wheelIndex < wheelCount
                   ? frequencies_[wheelIndex]
                   : 0.0;
    }

    double TonewheelBank::frequencyForMidi (int midiNote) const noexcept
    {
        if (midiNote < HarmonicFoldback::lowestWheelMidi
             || midiNote > HarmonicFoldback::highestWheelMidi)
            return 0.0;

        return frequency (static_cast<std::size_t> (
            midiNote - HarmonicFoldback::lowestWheelMidi));
    }

    double TonewheelBank::equalTemperedFrequency (
        int midiNote) noexcept
    {
        return 440.0 * std::pow (
            2.0, static_cast<double> (midiNote - 69) / 12.0);
    }

    std::uint64_t TonewheelBank::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    double TonewheelBank::unitFromHash (
        std::uint64_t value) noexcept
    {
        return static_cast<double> (value >> 11)
               * (1.0 / 9007199254740992.0);
    }
}
