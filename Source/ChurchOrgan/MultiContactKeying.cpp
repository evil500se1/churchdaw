#include "MultiContactKeying.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double pi =
            3.141592653589793238462643383279502884;
        constexpr double twoPi = 2.0 * pi;

        // Sequencia original ChurchDAW: o corpo fundamental fecha primeiro,
        // seguido pelos registros que definem presenca. Nao transcreve uma
        // matriz mecanica ou comercial.
        constexpr std::array<float, articulationContactCount>
            noteOnPositions {{
                0.78f, // 16'
                0.91f, // 5 1/3'
                0.00f, // 8'
                0.13f, // 4'
                0.27f, // 2 2/3'
                0.39f, // 2'
                0.53f, // 1 3/5'
                0.66f, // 1 1/3'
                1.00f  // 1'
            }};

        constexpr std::array<float, articulationContactCount>
            noteOffPositions {{
                0.88f, // 16'
                1.00f, // 5 1/3'
                0.74f, // 8'
                0.57f, // 4'
                0.42f, // 2 2/3'
                0.29f, // 2'
                0.18f, // 1 3/5'
                0.08f, // 1 1/3'
                0.00f  // 1'
            }};

        bool finiteInRange (
            float value,
            float minimum,
            float maximum) noexcept
        {
            return std::isfinite (value)
                   && value >= minimum
                   && value <= maximum;
        }

        float clampUnit (float value) noexcept
        {
            return std::max (0.0f, std::min (1.0f, value));
        }
    }

    bool MultiContactKeying::prepare (
        double sampleRate,
        std::uint64_t deterministicSeed,
        const MultiContactParameters& parameters) noexcept
    {
        if (! validParameters (sampleRate, parameters))
            return false;

        sampleRate_ = sampleRate;
        seed_ = deterministicSeed;
        parameters_ = parameters;
        attackCoefficient_ = smoothingCoefficient (
            sampleRate_, parameters_.contactAttackMilliseconds);
        releaseCoefficient_ = smoothingCoefficient (
            sampleRate_, parameters_.contactReleaseMilliseconds);
        bounceDecayCoefficient_ = decayCoefficient (
            sampleRate_, parameters_.bounceDecayMilliseconds);
        bouncePhaseIncrement_ = static_cast<float> (
            twoPi * static_cast<double> (parameters_.bounceFrequencyHz)
            / sampleRate_);
        prepared_ = true;
        reset();
        return true;
    }

    void MultiContactKeying::reset() noexcept
    {
        notes_ = {};
        activeNotes_ = {};
        activeSlots_.fill (-1);
        activeNoteCount_ = 0;
    }

    void MultiContactKeying::setPerformanceEnvelopeMilliseconds (
        float attackMilliseconds,
        float releaseMilliseconds) noexcept
    {
        if (! prepared_)
            return;

        const auto attack = std::isfinite (attackMilliseconds)
                                && attackMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (2000.0f, attackMilliseconds))
            : parameters_.contactAttackMilliseconds;
        const auto release = std::isfinite (releaseMilliseconds)
                                 && releaseMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (12000.0f, releaseMilliseconds))
            : parameters_.contactReleaseMilliseconds;

        attackCoefficient_ = smoothingCoefficient (
            sampleRate_, attack);
        releaseCoefficient_ = smoothingCoefficient (
            sampleRate_, release);
    }

    void MultiContactKeying::noteOn (
        int midiNote,
        int velocity) noexcept
    {
        if (! prepared_
             || midiNote < 0
             || midiNote >= static_cast<int> (maximumPolyphony))
            return;

        auto& note = notes_[static_cast<std::size_t> (midiNote)];
        ensureActive (midiNote);
        note.held = true;
        note.velocity = static_cast<std::uint8_t> (
            std::max (1, std::min (127, velocity)));
        ++note.generation;

        for (std::size_t contact = 0;
             contact < contactCount;
             ++contact)
        {
            auto& state = note.contacts[contact];
            state.targetClosed = true;
            state.delaySamples = contactDelay (
                midiNote, contact, note.generation, true);
            state.bounceEnvelope = 1.0f;
            const auto phaseHash = mixedHash (
                seed_
                ^ (static_cast<std::uint64_t> (midiNote + 1) << 33)
                ^ (static_cast<std::uint64_t> (contact + 1) << 17)
                ^ static_cast<std::uint64_t> (note.generation));
            state.bouncePhase = static_cast<float> (
                twoPi * unitFromHash (phaseHash));
        }
    }

    void MultiContactKeying::noteOff (int midiNote) noexcept
    {
        if (! prepared_
             || midiNote < 0
             || midiNote >= static_cast<int> (maximumPolyphony))
            return;

        auto& note = notes_[static_cast<std::size_t> (midiNote)];

        if (! note.held)
            return;

        note.held = false;
        ++note.generation;

        for (std::size_t contact = 0;
             contact < contactCount;
             ++contact)
        {
            auto& state = note.contacts[contact];
            state.targetClosed = false;
            state.delaySamples = contactDelay (
                midiNote, contact, note.generation, false);
            state.bounceEnvelope = 1.0f;
            const auto phaseHash = mixedHash (
                seed_
                ^ 0xa0761d6478bd642fULL
                ^ (static_cast<std::uint64_t> (midiNote + 1) << 31)
                ^ (static_cast<std::uint64_t> (contact + 1) << 15)
                ^ static_cast<std::uint64_t> (note.generation));
            state.bouncePhase = static_cast<float> (
                twoPi * unitFromHash (phaseHash));
        }
    }

    void MultiContactKeying::allNotesOff() noexcept
    {
        for (int note = 0;
             note < static_cast<int> (maximumPolyphony);
             ++note)
            if (notes_[static_cast<std::size_t> (note)].held)
                noteOff (note);
    }

    void MultiContactKeying::advance() noexcept
    {
        std::size_t activeIndex = 0;

        while (activeIndex < activeNoteCount_)
        {
            const auto noteIndex =
                static_cast<std::size_t> (activeNotes_[activeIndex]);
            auto& note = notes_[noteIndex];
            bool audible = note.held;

            for (auto& contact : note.contacts)
            {
                if (contact.delaySamples > 0)
                {
                    --contact.delaySamples;
                    audible = audible
                              || contact.level > 0.000001f;
                    continue;
                }

                const float target =
                    contact.targetClosed ? 1.0f : 0.0f;

                // Depois que o contato estabiliza, este é o caminho comum
                // durante notas sustentadas. Evita seno e smoothing inúteis
                // sem mudar estado audível; um novo evento redefine fase e
                // envelope de bounce deterministicamente.
                if (contact.bounceEnvelope == 0.0f
                     && contact.level == target)
                {
                    contact.output = target;
                    audible = audible || target > 0.000001f;
                    continue;
                }

                const float coefficient =
                    contact.targetClosed
                        ? attackCoefficient_
                        : releaseCoefficient_;

                contact.level +=
                    (target - contact.level) * coefficient;

                const float remaining =
                    contact.targetClosed
                        ? 1.0f - contact.level
                        : contact.level;
                float bounce = 0.0f;

                if (contact.bounceEnvelope > 0.000001f
                     && parameters_.bounceAmount > 0.0f)
                {
                    contact.bounceEnvelope *= bounceDecayCoefficient_;
                    contact.bouncePhase += bouncePhaseIncrement_;

                    if (contact.bouncePhase > static_cast<float> (twoPi))
                        contact.bouncePhase -= static_cast<float> (twoPi);

                    bounce =
                        parameters_.bounceAmount
                        * contact.bounceEnvelope
                        * std::sin (contact.bouncePhase)
                        * remaining;
                }
                else
                {
                    contact.bounceEnvelope = 0.0f;
                }

                contact.output = clampUnit (contact.level + bounce);

                if (std::abs (target - contact.level) < 0.000001f)
                {
                    contact.level = target;
                    contact.output = target;
                }

                audible = audible
                          || contact.output > 0.000001f
                          || contact.bounceEnvelope > 0.000001f;
            }

            if (! audible)
            {
                for (auto& contact : note.contacts)
                    contact = {};

                removeActiveAt (activeIndex);
                continue;
            }

            ++activeIndex;
        }
    }

    float MultiContactKeying::contactGain (
        int midiNote,
        std::size_t contactIndex) const noexcept
    {
        if (midiNote < 0
             || midiNote >= static_cast<int> (maximumPolyphony)
             || contactIndex >= contactCount)
            return 0.0f;

        return notes_[static_cast<std::size_t> (midiNote)]
            .contacts[contactIndex].output;
    }

    bool MultiContactKeying::isHeld (int midiNote) const noexcept
    {
        return midiNote >= 0
               && midiNote < static_cast<int> (maximumPolyphony)
               && notes_[static_cast<std::size_t> (midiNote)].held;
    }

    bool MultiContactKeying::isAudible (int midiNote) const noexcept
    {
        return midiNote >= 0
               && midiNote < static_cast<int> (maximumPolyphony)
               && activeSlots_[static_cast<std::size_t> (midiNote)] >= 0;
    }

    std::size_t MultiContactKeying::activeNoteCount() const noexcept
    {
        return activeNoteCount_;
    }

    const MultiContactParameters&
    MultiContactKeying::parameters() const noexcept
    {
        return parameters_;
    }

    bool MultiContactKeying::validParameters (
        double sampleRate,
        const MultiContactParameters& parameters) noexcept
    {
        return std::isfinite (sampleRate)
               && sampleRate >= 32000.0
               && sampleRate <= 384000.0
               && finiteInRange (
                   parameters.noteOnSpreadMilliseconds, 0.0f, 12.0f)
               && finiteInRange (
                   parameters.noteOffSpreadMilliseconds, 0.0f, 12.0f)
               && finiteInRange (
                   parameters.contactAttackMilliseconds, 0.02f, 20.0f)
               && finiteInRange (
                   parameters.contactReleaseMilliseconds, 0.02f, 30.0f)
               && finiteInRange (
                   parameters.timingJitterMilliseconds, 0.0f, 4.0f)
               && finiteInRange (
                   parameters.bounceAmount, 0.0f, 0.35f)
               && finiteInRange (
                   parameters.bounceDecayMilliseconds, 0.10f, 30.0f)
               && finiteInRange (
                   parameters.bounceFrequencyHz, 40.0f, 8000.0f)
               && parameters.bounceFrequencyHz
                   < static_cast<float> (sampleRate * 0.45);
    }

    float MultiContactKeying::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -1.0 / (sampleRate
                        * static_cast<double> (milliseconds)
                        * 0.001)));
    }

    float MultiContactKeying::decayCoefficient (
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

    std::uint64_t MultiContactKeying::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    double MultiContactKeying::unitFromHash (
        std::uint64_t value) noexcept
    {
        return static_cast<double> (value >> 11)
               * (1.0 / 9007199254740992.0);
    }

    std::uint32_t MultiContactKeying::contactDelay (
        int midiNote,
        std::size_t contact,
        std::uint32_t generation,
        bool noteOn) const noexcept
    {
        const auto& positions =
            noteOn ? noteOnPositions : noteOffPositions;
        const double spreadMilliseconds =
            noteOn
                ? parameters_.noteOnSpreadMilliseconds
                : parameters_.noteOffSpreadMilliseconds;
        const auto salt =
            noteOn
                ? 0xe7037ed1a0b428dbULL
                : 0x8ebc6af09c88c6e3ULL;
        const auto hash = mixedHash (
            seed_ ^ salt
            ^ (static_cast<std::uint64_t> (midiNote + 1) << 36)
            ^ (static_cast<std::uint64_t> (contact + 1) << 20)
            ^ static_cast<std::uint64_t> (generation));
        const double signedJitter =
            (2.0 * unitFromHash (hash) - 1.0)
            * static_cast<double> (
                parameters_.timingJitterMilliseconds);
        const double milliseconds = std::max (
            0.0,
            spreadMilliseconds
                * static_cast<double> (positions[contact])
            + signedJitter);
        const double samples =
            milliseconds * 0.001 * sampleRate_;
        return static_cast<std::uint32_t> (
            std::min (
                samples + 0.5,
                static_cast<double> (
                    static_cast<std::uint32_t> (-1))));
    }

    void MultiContactKeying::ensureActive (int midiNote) noexcept
    {
        const auto noteIndex = static_cast<std::size_t> (midiNote);

        if (activeSlots_[noteIndex] >= 0)
            return;

        activeNotes_[activeNoteCount_] =
            static_cast<std::uint8_t> (midiNote);
        activeSlots_[noteIndex] =
            static_cast<std::int16_t> (activeNoteCount_);
        ++activeNoteCount_;
    }

    void MultiContactKeying::removeActiveAt (
        std::size_t activeIndex) noexcept
    {
        const auto removed =
            static_cast<std::size_t> (activeNotes_[activeIndex]);
        --activeNoteCount_;

        if (activeIndex != activeNoteCount_)
        {
            const auto moved = activeNotes_[activeNoteCount_];
            activeNotes_[activeIndex] = moved;
            activeSlots_[static_cast<std::size_t> (moved)] =
                static_cast<std::int16_t> (activeIndex);
        }

        activeSlots_[removed] = -1;
    }
}
