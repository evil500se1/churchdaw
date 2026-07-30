#include "KeyClickGenerator.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double twoPi =
            6.283185307179586476925286766559005768;

        bool finiteInRange (
            float value,
            float minimum,
            float maximum) noexcept
        {
            return std::isfinite (value)
                   && value >= minimum
                   && value <= maximum;
        }
    }

    bool KeyClickGenerator::prepare (
        double sampleRate,
        std::uint64_t deterministicSeed,
        const KeyClickParameters& parameters) noexcept
    {
        if (! validParameters (sampleRate, parameters))
            return false;

        sampleRate_ = sampleRate;
        seed_ = deterministicSeed;
        parameters_ = parameters;
        lowCutCoefficient_ = filterCoefficient (
            sampleRate_, parameters_.lowCutHz);
        highCutCoefficient_ = filterCoefficient (
            sampleRate_, parameters_.highCutHz);
        onsetDecayCoefficient_ = envelopeCoefficient (
            sampleRate_, parameters_.onsetDecayMilliseconds);
        releaseDecayCoefficient_ = envelopeCoefficient (
            sampleRate_, parameters_.releaseDecayMilliseconds);
        prepared_ = true;
        reset();
        return true;
    }

    void KeyClickGenerator::reset() noexcept
    {
        voices_ = {};
        activeNotes_ = {};
        activeSlots_.fill (-1);
        generations_ = {};
        velocities_ = {};
        held_ = {};
        activeVoiceCount_ = 0;
    }

    void KeyClickGenerator::noteOn (
        int midiNote,
        int velocity) noexcept
    {
        if (! prepared_
             || midiNote < 0
             || midiNote >= static_cast<int> (maximumPolyphony))
            return;

        const auto note = static_cast<std::size_t> (midiNote);
        held_[note] = true;
        velocities_[note] = static_cast<std::uint8_t> (
            std::max (1, std::min (127, velocity)));
        trigger (midiNote, velocities_[note], false);
    }

    void KeyClickGenerator::noteOff (int midiNote) noexcept
    {
        if (! prepared_
             || midiNote < 0
             || midiNote >= static_cast<int> (maximumPolyphony))
            return;

        const auto note = static_cast<std::size_t> (midiNote);

        if (! held_[note])
            return;

        held_[note] = false;
        trigger (midiNote, velocities_[note], true);
    }

    void KeyClickGenerator::allNotesOff() noexcept
    {
        for (int note = 0;
             note < static_cast<int> (maximumPolyphony);
             ++note)
            if (held_[static_cast<std::size_t> (note)])
                noteOff (note);
    }

    float KeyClickGenerator::renderSample() noexcept
    {
        float sum = 0.0f;
        std::size_t activeIndex = 0;

        while (activeIndex < activeVoiceCount_)
        {
            const auto note =
                static_cast<std::size_t> (activeNotes_[activeIndex]);
            auto& voice = voices_[note];
            const float noise = nextNoise (voice.randomState);

            voice.lowState +=
                lowCutCoefficient_ * (noise - voice.lowState);
            const float highPassed = noise - voice.lowState;
            voice.highState += highCutCoefficient_
                               * (highPassed - voice.highState);
            sum += voice.highState * voice.envelope;
            voice.envelope *= voice.decayCoefficient;

            if (voice.envelope < 0.000001f)
            {
                voice = {};
                removeActiveAt (activeIndex);
                continue;
            }

            ++activeIndex;
        }

        return sum;
    }

    bool KeyClickGenerator::isHeld (int midiNote) const noexcept
    {
        return midiNote >= 0
               && midiNote < static_cast<int> (maximumPolyphony)
               && held_[static_cast<std::size_t> (midiNote)];
    }

    std::size_t KeyClickGenerator::activeClickCount() const noexcept
    {
        return activeVoiceCount_;
    }

    const KeyClickParameters&
    KeyClickGenerator::parameters() const noexcept
    {
        return parameters_;
    }

    bool KeyClickGenerator::validParameters (
        double sampleRate,
        const KeyClickParameters& parameters) noexcept
    {
        const float maximumFilterFrequency =
            static_cast<float> (sampleRate * 0.45);

        return std::isfinite (sampleRate)
               && sampleRate >= 32000.0
               && sampleRate <= 384000.0
               && finiteInRange (
                   parameters.onsetLevel, 0.0f, 0.5f)
               && finiteInRange (
                   parameters.releaseLevel, 0.0f, 0.5f)
               && finiteInRange (
                   parameters.onsetDecayMilliseconds, 0.25f, 80.0f)
               && finiteInRange (
                   parameters.releaseDecayMilliseconds, 0.25f, 80.0f)
               && finiteInRange (
                   parameters.lowCutHz, 20.0f, maximumFilterFrequency)
               && finiteInRange (
                   parameters.highCutHz, 40.0f, maximumFilterFrequency)
               && parameters.highCutHz > parameters.lowCutHz
               && finiteInRange (
                   parameters.velocityInfluence, 0.0f, 1.0f);
    }

    float KeyClickGenerator::filterCoefficient (
        double sampleRate,
        float frequencyHz) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -twoPi * static_cast<double> (frequencyHz)
                / sampleRate));
    }

    float KeyClickGenerator::envelopeCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        return static_cast<float> (
            std::exp (
                std::log (0.001)
                / (sampleRate
                   * static_cast<double> (milliseconds)
                   * 0.001)));
    }

    std::uint64_t KeyClickGenerator::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    float KeyClickGenerator::nextNoise (
        std::uint64_t& state) noexcept
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const auto bits = state * 0x2545f4914f6cdd1dULL;
        const auto mantissa =
            static_cast<std::uint32_t> (bits >> 40);
        return static_cast<float> (mantissa)
                   * (2.0f / 16777215.0f)
               - 1.0f;
    }

    void KeyClickGenerator::trigger (
        int midiNote,
        int velocity,
        bool release) noexcept
    {
        const auto note = static_cast<std::size_t> (midiNote);
        auto& voice = voices_[note];
        ++generations_[note];
        ensureActive (midiNote);

        const float velocityUnit =
            static_cast<float> (velocity) * (1.0f / 127.0f);
        const float velocityGain =
            1.0f - parameters_.velocityInfluence
            + parameters_.velocityInfluence * velocityUnit;
        voice.envelope =
            (release
                 ? parameters_.releaseLevel
                 : parameters_.onsetLevel)
            * velocityGain;
        voice.decayCoefficient =
            release
                ? releaseDecayCoefficient_
                : onsetDecayCoefficient_;
        voice.lowState = 0.0f;
        voice.highState = 0.0f;
        voice.randomState = mixedHash (
            seed_
            ^ (release
                   ? 0xd1b54a32d192ed03ULL
                   : 0x94d049bb133111ebULL)
            ^ (static_cast<std::uint64_t> (note + 1) << 32)
            ^ static_cast<std::uint64_t> (generations_[note]));

        if (voice.randomState == 0)
            voice.randomState = 1;

        voice.active = true;
    }

    void KeyClickGenerator::ensureActive (int midiNote) noexcept
    {
        const auto note = static_cast<std::size_t> (midiNote);

        if (activeSlots_[note] >= 0)
            return;

        activeNotes_[activeVoiceCount_] =
            static_cast<std::uint8_t> (midiNote);
        activeSlots_[note] =
            static_cast<std::int16_t> (activeVoiceCount_);
        ++activeVoiceCount_;
    }

    void KeyClickGenerator::removeActiveAt (
        std::size_t activeIndex) noexcept
    {
        const auto removed =
            static_cast<std::size_t> (activeNotes_[activeIndex]);
        --activeVoiceCount_;

        if (activeIndex != activeVoiceCount_)
        {
            const auto moved = activeNotes_[activeVoiceCount_];
            activeNotes_[activeIndex] = moved;
            activeSlots_[static_cast<std::size_t> (moved)] =
                static_cast<std::int16_t> (activeIndex);
        }

        activeSlots_[removed] = -1;
    }
}
