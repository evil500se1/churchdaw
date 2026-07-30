#include "HarmonicPercussion.h"

#include <algorithm>
#include <cmath>

namespace churchorgan
{
    namespace
    {
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

    bool HarmonicPercussion::prepare (
        double sampleRate,
        const HarmonicPercussionParameters& parameters) noexcept
    {
        if (! validParameters (sampleRate, parameters))
            return false;

        sampleRate_ = sampleRate;
        parameters_ = parameters;
        releaseCoefficient_ = decayCoefficient (
            sampleRate_, parameters_.releaseMilliseconds);
        refreshDecayCoefficient();
        prepared_ = true;
        reset();
        return true;
    }

    void HarmonicPercussion::reset() noexcept
    {
        held_ = {};
        heldCount_ = 0;
        envelope_ = 0.0f;
        peakGain_ = 0.0f;
        attackIncrement_ = 0.0f;
        triggeredNote_ = -1;
        stage_ = EnvelopeStage::idle;
    }

    bool HarmonicPercussion::noteOn (
        int midiNote,
        int velocity) noexcept
    {
        if (! prepared_
             || velocity <= 0
             || midiNote < 0
             || midiNote >= static_cast<int> (
                 articulationMidiNoteCount))
            return false;

        const auto note = static_cast<std::size_t> (midiNote);
        const bool anyHeldBefore = heldCount_ > 0;

        if (! held_[note])
        {
            held_[note] = true;
            ++heldCount_;
        }

        const bool shouldTrigger =
            parameters_.triggerMode
                == PercussionTriggerMode::retrigger
            || ! anyHeldBefore;

        if (shouldTrigger)
            trigger (midiNote);

        return shouldTrigger;
    }

    void HarmonicPercussion::noteOff (int midiNote) noexcept
    {
        if (! prepared_
             || midiNote < 0
             || midiNote >= static_cast<int> (
                 articulationMidiNoteCount))
            return;

        const auto note = static_cast<std::size_t> (midiNote);

        if (! held_[note])
            return;

        held_[note] = false;
        --heldCount_;

        if (midiNote == triggeredNote_
             && stage_ != EnvelopeStage::idle)
            stage_ = EnvelopeStage::release;
    }

    void HarmonicPercussion::allNotesOff() noexcept
    {
        held_ = {};
        heldCount_ = 0;

        if (stage_ != EnvelopeStage::idle)
            stage_ = EnvelopeStage::release;
    }

    float HarmonicPercussion::processSample (
        float harmonicCarrier) noexcept
    {
        return harmonicCarrier * advanceEnvelope();
    }

    float HarmonicPercussion::advanceEnvelope() noexcept
    {
        switch (stage_)
        {
            case EnvelopeStage::idle:
                envelope_ = 0.0f;
                break;

            case EnvelopeStage::attack:
                envelope_ += attackIncrement_;

                if (envelope_ >= peakGain_)
                {
                    envelope_ = peakGain_;
                    stage_ = EnvelopeStage::decay;
                }
                break;

            case EnvelopeStage::decay:
                envelope_ *= selectedDecayCoefficient_;

                if (envelope_ < 0.000001f)
                {
                    envelope_ = 0.0f;
                    stage_ = EnvelopeStage::idle;
                    triggeredNote_ = -1;
                }
                break;

            case EnvelopeStage::release:
                envelope_ *= releaseCoefficient_;

                if (envelope_ < 0.000001f)
                {
                    envelope_ = 0.0f;
                    stage_ = EnvelopeStage::idle;
                    triggeredNote_ = -1;
                }
                break;
        }

        return envelope_;
    }

    void HarmonicPercussion::setHarmonic (
        PercussionHarmonic harmonic) noexcept
    {
        parameters_.harmonic = harmonic;
    }

    void HarmonicPercussion::setTiming (
        PercussionTiming timing) noexcept
    {
        parameters_.timing = timing;
        refreshDecayCoefficient();
    }

    void HarmonicPercussion::setVolume (
        PercussionVolume volume) noexcept
    {
        parameters_.volume = volume;
    }

    void HarmonicPercussion::setTriggerMode (
        PercussionTriggerMode mode) noexcept
    {
        parameters_.triggerMode = mode;
    }

    PercussionHarmonic HarmonicPercussion::harmonic() const noexcept
    {
        return parameters_.harmonic;
    }

    PercussionTiming HarmonicPercussion::timing() const noexcept
    {
        return parameters_.timing;
    }

    PercussionVolume HarmonicPercussion::volume() const noexcept
    {
        return parameters_.volume;
    }

    PercussionTriggerMode
    HarmonicPercussion::triggerMode() const noexcept
    {
        return parameters_.triggerMode;
    }

    int HarmonicPercussion::triggeredMidiNote() const noexcept
    {
        return triggeredNote_;
    }

    int HarmonicPercussion::requestedHarmonicMidi() const noexcept
    {
        return triggeredNote_ >= 0
                   ? triggeredNote_ + harmonicSemitoneOffset()
                   : -1;
    }

    int HarmonicPercussion::harmonicSemitoneOffset() const noexcept
    {
        return parameters_.harmonic == PercussionHarmonic::second
                   ? 12
                   : 19;
    }

    std::size_t
    HarmonicPercussion::harmonicDrawbarIndex() const noexcept
    {
        return parameters_.harmonic == PercussionHarmonic::second
                   ? 3U
                   : 4U;
    }

    std::size_t HarmonicPercussion::heldNoteCount() const noexcept
    {
        return heldCount_;
    }

    float HarmonicPercussion::currentGain() const noexcept
    {
        return envelope_;
    }

    bool HarmonicPercussion::isActive() const noexcept
    {
        return stage_ != EnvelopeStage::idle;
    }

    const HarmonicPercussionParameters&
    HarmonicPercussion::parameters() const noexcept
    {
        return parameters_;
    }

    bool HarmonicPercussion::validParameters (
        double sampleRate,
        const HarmonicPercussionParameters& parameters) noexcept
    {
        return std::isfinite (sampleRate)
               && sampleRate >= 32000.0
               && sampleRate <= 384000.0
               && finiteInRange (
                   parameters.attackMilliseconds, 0.02f, 20.0f)
               && finiteInRange (
                   parameters.fastDecayMilliseconds, 20.0f, 5000.0f)
               && finiteInRange (
                   parameters.slowDecayMilliseconds, 20.0f, 8000.0f)
               && parameters.slowDecayMilliseconds
                   > parameters.fastDecayMilliseconds
               && finiteInRange (
                   parameters.releaseMilliseconds, 1.0f, 1000.0f)
               && finiteInRange (
                   parameters.normalGain, 0.0f, 1.0f)
               && finiteInRange (
                   parameters.softGain, 0.0f, 1.0f)
               && parameters.softGain <= parameters.normalGain;
    }

    float HarmonicPercussion::decayCoefficient (
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

    void HarmonicPercussion::trigger (int midiNote) noexcept
    {
        triggeredNote_ = midiNote;
        peakGain_ = selectedPeakGain();
        const auto attackSamples = std::max (
            1.0,
            sampleRate_
                * static_cast<double> (
                    parameters_.attackMilliseconds)
                * 0.001);
        envelope_ = 0.0f;
        attackIncrement_ =
            peakGain_ / static_cast<float> (attackSamples);
        stage_ = EnvelopeStage::attack;
    }

    void HarmonicPercussion::refreshDecayCoefficient() noexcept
    {
        const float milliseconds =
            parameters_.timing == PercussionTiming::fast
                ? parameters_.fastDecayMilliseconds
                : parameters_.slowDecayMilliseconds;
        selectedDecayCoefficient_ = decayCoefficient (
            sampleRate_, milliseconds);
    }

    float HarmonicPercussion::selectedPeakGain() const noexcept
    {
        return parameters_.volume == PercussionVolume::soft
                   ? parameters_.softGain
                   : parameters_.normalGain;
    }
}
