#include "TransistorDividerBank.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double phaseScale = 4294967296.0;
        constexpr float inversePhaseScale =
            1.0f / 4294967296.0f;
    }

    bool TransistorDividerBank::prepare (
        double sampleRate,
        std::uint64_t deterministicSeed) noexcept
    {
        if (! std::isfinite (sampleRate)
             || sampleRate < 32000.0
             || sampleRate > 384000.0)
            return false;

        sampleRate_ = sampleRate;
        seed_ = deterministicSeed;

        for (std::size_t pitchClass = 0;
             pitchClass < pitchClassCount;
             ++pitchClass)
        {
            const int baseMidi =
                lowestMidi + static_cast<int> (pitchClass);
            const double increment =
                equalTemperedFrequency (baseMidi)
                * phaseScale
                / sampleRate_;
            baseIncrements_[pitchClass] =
                static_cast<std::uint32_t> (increment + 0.5);
        }

        for (int midi = lowestMidi; midi <= highestMidi; ++midi)
            frequencies_[static_cast<std::size_t> (midi - lowestMidi)] =
                equalTemperedFrequency (midi);

        prepared_ = true;
        resetPhases();
        return true;
    }

    void TransistorDividerBank::resetPhases() noexcept
    {
        if (! prepared_)
            return;

        for (std::size_t pitchClass = 0;
             pitchClass < pitchClassCount;
             ++pitchClass)
            masterPhases_[pitchClass] =
                static_cast<std::uint32_t> (
                    mixedHash (
                        seed_
                        ^ (0x9e3779b97f4a7c15ULL
                           * static_cast<std::uint64_t> (
                               pitchClass + 1)))
                    >> 32);

        modulatedIncrements_ = {};
        taps_ = {};
    }

    void TransistorDividerBank::advance (
        float pitchMultiplier) noexcept
    {
        if (! prepared_)
            return;

        if (! std::isfinite (pitchMultiplier))
            pitchMultiplier = 1.0f;

        pitchMultiplier = std::max (
            0.97f, std::min (1.03f, pitchMultiplier));

        for (std::size_t pitchClass = 0;
             pitchClass < pitchClassCount;
             ++pitchClass)
        {
            const auto modulatedBaseIncrement =
                static_cast<std::uint32_t> (
                    static_cast<double> (
                        baseIncrements_[pitchClass])
                    * static_cast<double> (pitchMultiplier)
                    + 0.5);

            for (std::size_t octave = 0; octave < 8; ++octave)
            {
                const std::size_t tapIndex =
                    octave * pitchClassCount + pitchClass;
                const auto tapPhaseBits =
                    masterPhases_[pitchClass]
                    << static_cast<unsigned int> (octave);
                const auto tapIncrementBits =
                    modulatedBaseIncrement
                    << static_cast<unsigned int> (octave);
                const float phase = phaseUnit (tapPhaseBits);
                const float delta =
                    static_cast<float> (tapIncrementBits)
                    * inversePhaseScale;

                renderWaveforms (
                    phase, delta, taps_[tapIndex]);
            }

            masterPhases_[pitchClass] += modulatedBaseIncrement;
        }
    }

    void TransistorDividerBank::setActiveTapMask (
        std::uint64_t lowerTapMask,
        std::uint32_t upperTapMask) noexcept
    {
        activeTapCount_ = 0;

        for (std::size_t tapIndex = 0;
             tapIndex < tapCount;
             ++tapIndex)
        {
            const bool selected =
                tapIndex < 64
                    ? (lowerTapMask
                       & (std::uint64_t { 1 } << tapIndex)) != 0
                    : (upperTapMask
                       & (std::uint32_t { 1 }
                          << (tapIndex - 64))) != 0;

            if (selected)
            {
                const auto octave = tapIndex / pitchClassCount;
                activeTapIndices_[activeTapCount_] =
                    static_cast<std::uint8_t> (tapIndex);
                activeTapPitchClasses_[activeTapCount_] =
                    static_cast<std::uint8_t> (
                        tapIndex - octave * pitchClassCount);
                activeTapOctaves_[activeTapCount_] =
                    static_cast<std::uint8_t> (octave);
                ++activeTapCount_;
            }
        }
    }

    void TransistorDividerBank::advanceActive (
        float pitchMultiplier) noexcept
    {
        if (! prepared_)
            return;

        if (! std::isfinite (pitchMultiplier))
            pitchMultiplier = 1.0f;

        pitchMultiplier = std::max (
            0.97f, std::min (1.03f, pitchMultiplier));

        for (std::size_t pitchClass = 0;
             pitchClass < pitchClassCount;
             ++pitchClass)
            modulatedIncrements_[pitchClass] =
                static_cast<std::uint32_t> (
                    static_cast<double> (
                        baseIncrements_[pitchClass])
                    * static_cast<double> (pitchMultiplier)
                    + 0.5);

        for (std::size_t active = 0;
             active < activeTapCount_;
             ++active)
        {
            const auto tapIndex =
                static_cast<std::size_t> (activeTapIndices_[active]);
            const auto pitchClass =
                static_cast<std::size_t> (
                    activeTapPitchClasses_[active]);
            const auto shift =
                static_cast<unsigned int> (
                    activeTapOctaves_[active]);
            const auto tapPhaseBits =
                masterPhases_[pitchClass] << shift;
            const auto tapIncrementBits =
                modulatedIncrements_[pitchClass] << shift;
            const float phase = phaseUnit (tapPhaseBits);
            const float delta =
                static_cast<float> (tapIncrementBits)
                * inversePhaseScale;
            renderWaveforms (
                phase, delta, taps_[tapIndex]);
        }

        for (std::size_t pitchClass = 0;
             pitchClass < pitchClassCount;
             ++pitchClass)
            masterPhases_[pitchClass] +=
                modulatedIncrements_[pitchClass];
    }

    TransistorDividerTap TransistorDividerBank::tapForMidi (
        int midiNote) const noexcept
    {
        if (midiNote < lowestMidi || midiNote > highestMidi)
            return {};

        return taps_[static_cast<std::size_t> (
            midiNote - lowestMidi)];
    }

    float TransistorDividerBank::squareForMidi (
        int midiNote) const noexcept
    {
        return tapForMidi (midiNote).square;
    }

    float TransistorDividerBank::pulseForMidi (
        int midiNote) const noexcept
    {
        return tapForMidi (midiNote).pulse;
    }

    float TransistorDividerBank::rampForMidi (
        int midiNote) const noexcept
    {
        return tapForMidi (midiNote).ramp;
    }

    double TransistorDividerBank::frequencyForMidi (
        int midiNote) const noexcept
    {
        if (midiNote < lowestMidi || midiNote > highestMidi)
            return 0.0;

        return frequencies_[static_cast<std::size_t> (
            midiNote - lowestMidi)];
    }

    double TransistorDividerBank::equalTemperedFrequency (
        int midiNote) noexcept
    {
        return 440.0 * std::pow (
            2.0,
            static_cast<double> (midiNote - 69) / 12.0);
    }

    std::uint64_t TransistorDividerBank::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    float TransistorDividerBank::phaseUnit (
        std::uint32_t phase) noexcept
    {
        return static_cast<float> (phase)
               * inversePhaseScale;
    }

    float TransistorDividerBank::polyBlep (
        float phase,
        float phaseDelta) noexcept
    {
        if (phaseDelta <= 0.0f || phaseDelta >= 1.0f)
            return 0.0f;

        if (phase < phaseDelta)
        {
            const float normalised = phase / phaseDelta;
            return normalised + normalised
                   - normalised * normalised - 1.0f;
        }

        if (phase > 1.0f - phaseDelta)
        {
            const float normalised =
                (phase - 1.0f) / phaseDelta;
            return normalised * normalised
                   + normalised + normalised + 1.0f;
        }

        return 0.0f;
    }

    void TransistorDividerBank::renderWaveforms (
        float phase,
        float phaseDelta,
        TransistorDividerTap& tap) noexcept
    {
        const float startCorrection =
            polyBlep (phase, phaseDelta);
        float squareEdge = phase - 0.5f;

        if (squareEdge < 0.0f)
            squareEdge += 1.0f;

        float pulseEdge = phase - 0.36f;

        if (pulseEdge < 0.0f)
            pulseEdge += 1.0f;

        tap.square =
            (phase < 0.5f ? 1.0f : -1.0f)
            + startCorrection
            - polyBlep (squareEdge, phaseDelta);
        tap.pulse =
            (phase < 0.36f ? 1.0f : -1.0f)
            + startCorrection
            - polyBlep (pulseEdge, phaseDelta);
        tap.ramp =
            2.0f * phase - 1.0f - startCorrection;
    }

}
