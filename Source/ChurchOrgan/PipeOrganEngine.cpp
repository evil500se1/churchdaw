#include "PipeOrganEngine.h"

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

        struct RankDefinition
        {
            std::array<float, 12> harmonics {};
            int semitoneOffset = 0;
            float attackMilliseconds = 10.0f;
            float releaseMilliseconds = 90.0f;
            float speechMilliseconds = 65.0f;
            float speechBrightness = 0.2f;
            float chiffMilliseconds = 12.0f;
            float chiffAmount = 0.05f;
            float windCents = 1.0f;
            float gain = 0.8f;
            float secondaryDetuneCents = 0.0f;
            float crescendoThreshold = 0.5f;
            float crescendoMaximum = 0.7f;
        };

        constexpr std::array<float, 12> harmonics (
            float h1, float h2, float h3, float h4,
            float h5, float h6, float h7, float h8,
            float h9, float h10, float h11, float h12) noexcept
        {
            return {{
                h1, h2, h3, h4, h5, h6,
                h7, h8, h9, h10, h11, h12
            }};
        }

        constexpr RankDefinition rank (
            std::array<float, 12> spectrum,
            int semitoneOffset,
            float attackMilliseconds,
            float releaseMilliseconds,
            float speechMilliseconds,
            float speechBrightness,
            float chiffMilliseconds,
            float chiffAmount,
            float windCents,
            float gain,
            float secondaryDetuneCents,
            float crescendoThreshold,
            float crescendoMaximum) noexcept
        {
            RankDefinition result {};
            result.harmonics = spectrum;
            result.semitoneOffset = semitoneOffset;
            result.attackMilliseconds = attackMilliseconds;
            result.releaseMilliseconds = releaseMilliseconds;
            result.speechMilliseconds = speechMilliseconds;
            result.speechBrightness = speechBrightness;
            result.chiffMilliseconds = chiffMilliseconds;
            result.chiffAmount = chiffAmount;
            result.windCents = windCents;
            result.gain = gain;
            result.secondaryDetuneCents = secondaryDetuneCents;
            result.crescendoThreshold = crescendoThreshold;
            result.crescendoMaximum = crescendoMaximum;
            return result;
        }

        constexpr std::array<RankDefinition, pipeOrganRankCount>
            rankDefinitions {{
                rank (
                    harmonics (
                        1.00f, 0.46f, 0.30f, 0.20f,
                        0.14f, 0.10f, 0.072f, 0.052f,
                        0.037f, 0.027f, 0.019f, 0.013f),
                    0, 8.0f, 95.0f, 58.0f, 0.20f,
                    13.0f, 0.070f, 1.25f, 0.86f, 0.0f,
                    0.00f, 0.82f),
                rank (
                    harmonics (
                        1.00f, 0.17f, 0.055f, 0.022f,
                        0.009f, 0.004f, 0.002f, 0.001f,
                        0.0f, 0.0f, 0.0f, 0.0f),
                    0, 24.0f, 155.0f, 105.0f, 0.08f,
                    18.0f, 0.045f, 0.75f, 0.78f, 0.0f,
                    0.08f, 0.68f),
                rank (
                    harmonics (
                        1.00f, 0.0f, 0.31f, 0.0f,
                        0.135f, 0.0f, 0.060f, 0.0f,
                        0.027f, 0.0f, 0.012f, 0.0f),
                    0, 17.0f, 145.0f, 88.0f, 0.12f,
                    16.0f, 0.050f, 0.85f, 0.82f, 0.0f,
                    0.18f, 0.72f),
                rank (
                    harmonics (
                        1.00f, 0.0f, 0.39f, 0.0f,
                        0.18f, 0.0f, 0.082f, 0.0f,
                        0.036f, 0.0f, 0.016f, 0.0f),
                    -12, 32.0f, 220.0f, 120.0f, 0.10f,
                    20.0f, 0.038f, 0.68f, 0.76f, 0.0f,
                    0.10f, 0.78f),
                rank (
                    harmonics (
                        1.00f, 0.44f, 0.28f, 0.18f,
                        0.12f, 0.080f, 0.055f, 0.038f,
                        0.026f, 0.018f, 0.012f, 0.008f),
                    12, 6.0f, 82.0f, 48.0f, 0.24f,
                    10.0f, 0.082f, 1.45f, 0.66f, 0.0f,
                    0.28f, 0.74f),
                rank (
                    harmonics (
                        0.0f, 0.78f, 0.64f, 0.49f,
                        0.35f, 0.24f, 0.16f, 0.11f,
                        0.075f, 0.050f, 0.034f, 0.022f),
                    0, 4.5f, 72.0f, 42.0f, 0.35f,
                    8.0f, 0.095f, 1.80f, 0.52f, 0.0f,
                    0.52f, 0.72f),
                rank (
                    harmonics (
                        1.00f, 0.57f, 0.43f, 0.32f,
                        0.24f, 0.18f, 0.135f, 0.102f,
                        0.077f, 0.058f, 0.043f, 0.032f),
                    0, 30.0f, 180.0f, 135.0f, 0.18f,
                    20.0f, 0.035f, 1.05f, 0.62f, 3.4f,
                    0.38f, 0.68f),
                rank (
                    harmonics (
                        1.00f, 0.83f, 0.68f, 0.55f,
                        0.45f, 0.37f, 0.30f, 0.245f,
                        0.198f, 0.158f, 0.125f, 0.098f),
                    0, 11.0f, 105.0f, 72.0f, 0.28f,
                    14.0f, 0.060f, 1.35f, 0.68f, 0.0f,
                    0.62f, 0.72f),
                rank (
                    harmonics (
                        0.82f, 0.78f, 1.00f, 0.91f,
                        0.74f, 0.57f, 0.43f, 0.32f,
                        0.235f, 0.170f, 0.121f, 0.085f),
                    0, 8.0f, 92.0f, 54.0f, 0.34f,
                    11.0f, 0.075f, 1.55f, 0.72f, 0.0f,
                    0.74f, 0.82f),
                rank (
                    harmonics (
                        1.00f, 0.27f, 0.125f, 0.062f,
                        0.032f, 0.017f, 0.009f, 0.005f,
                        0.003f, 0.002f, 0.001f, 0.0f),
                    0, 20.0f, 150.0f, 96.0f, 0.11f,
                    17.0f, 0.042f, 0.80f, 0.66f, 0.0f,
                    0.42f, 0.66f),
                rank (
                    harmonics (
                        1.00f, 0.32f, 0.13f, 0.055f,
                        0.024f, 0.011f, 0.005f, 0.002f,
                        0.001f, 0.0f, 0.0f, 0.0f),
                    -12, 38.0f, 260.0f, 145.0f, 0.08f,
                    22.0f, 0.028f, 0.55f, 0.84f, 0.0f,
                    0.16f, 0.90f)
            }};

        constexpr std::array<std::array<float, 12>, 4>
            temperamentCents {{
                {{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }},
                {{ 0.0f, 1.8f, -2.2f, 1.1f, -1.4f, 2.0f,
                   -0.6f, 1.3f, -2.0f, 1.7f, -1.1f, 0.5f }},
                {{ 0.0f, -4.2f, 3.1f, -5.8f, 2.2f, -2.7f,
                   -6.4f, 2.8f, -3.5f, 4.0f, -5.0f, 1.2f }},
                {{ 0.0f, 2.4f, -7.8f, -1.8f, -9.6f, -5.2f,
                   -1.4f, 1.6f, -7.0f, -9.2f, -5.8f, -10.6f }}
            }};

        constexpr std::array<const char*, pipeOrganRankCount>
            rankNames {{
                "Principal",
                "Flute",
                "Gedackt",
                "Bourdon",
                "Octave",
                "Mixture",
                "Strings",
                "Reed",
                "Trumpet",
                "Choir",
                "Pedal"
            }};

        bool validManual (PipeOrganManual manual) noexcept
        {
            return static_cast<std::size_t> (manual)
                   < pipeOrganManualCount;
        }

        bool validRank (PipeOrganRank rankValue) noexcept
        {
            return static_cast<std::size_t> (rankValue)
                   < pipeOrganRankCount;
        }
    }

    PipeOrganEngine::PipeOrganEngine() noexcept
    {
        activeSlots_.fill (-1);
    }

    bool PipeOrganEngine::prepare (
        const PipeOrganPrepareSpec& spec) noexcept
    {
        if (! validSpec (spec))
            return false;

        spec_ = spec;
        buildSineTable();

        for (std::size_t mask = 1;
             mask < firstSetBit_.size();
             ++mask)
        {
            std::size_t bit = 0;
            std::size_t shifted = mask;

            while ((shifted & 1U) == 0U)
            {
                shifted >>= 1U;
                ++bit;
            }

            firstSetBit_[mask] =
                static_cast<std::uint8_t> (bit);
        }

        rebuildFrequencies();
        rebuildPanGains();
        refreshQualityAndRankCoefficients();
        buildPipeWaveTables();
        rankSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 24.0f);
        swellSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 34.0f);
        crescendoSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 55.0f);
        windPressureCoefficient_ = lowpassCoefficient (
            spec_.sampleRate, 3.2f);
        windNoiseCoefficient_ = lowpassCoefficient (
            spec_.sampleRate, 1.15f);
        pipeWindCoefficient_ = lowpassCoefficient (
            spec_.sampleRate, 11.5f);
        chiffLowCoefficient_ = lowpassCoefficient (
            spec_.sampleRate, 1150.0f);
        chiffHighCoefficient_ = lowpassCoefficient (
            spec_.sampleRate,
            static_cast<float> (
                std::min (7200.0, spec_.sampleRate * 0.38)));
        dcCoefficient_ = static_cast<float> (
            std::exp (-twoPi * 12.0 / spec_.sampleRate));
        couplers_ = {};
        tremulants_ = {};
        tremulants_[0].rateHz = 5.9f;
        tremulants_[1].rateHz = 5.5f;
        tremulants_[2].rateHz = 4.8f;
        prepared_ = true;
        loadDefaultRegistration();
        reset();
        return true;
    }

    void PipeOrganEngine::reset() noexcept
    {
        voices_ = {};
        held_ = {};
        activeVoiceIds_ = {};
        activeSlots_.fill (-1);
        activeVoiceCount_ = 0;
        events_ = {};
        eventCount_ = 0;
        droppedEvents_ = 0;
        renderedSamples_ = 0;
        invalidSamples_ = 0;
        sampleCounter_ = 0;
        rankCurrents_ = rankTargets_;
        swellCurrents_ = swellTargets_;
        crescendoCurrent_ = crescendoTarget_;
        tremulantSamples_ = {};
        effectiveRankLevels_ = {};
        effectiveRankMasks_ = {};
        windPressure_.fill (1.0f);
        windNoise_ = {};
        windNoiseTarget_ = {};
        lastWindLoad_ = {};

        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            windRandom_[manual] = mixedHash (
                spec_.deterministicSeed
                ^ (0xd1b54a32d192ed03ULL
                   * static_cast<std::uint64_t> (manual + 1)));
            tremulants_[manual].phase = 0.0f;
        }

        dcInputLeft_ = 0.0f;
        dcOutputLeft_ = 0.0f;
        dcInputRight_ = 0.0f;
        dcOutputRight_ = 0.0f;
        diagnostics_ = {};
        diagnostics_.upperWindPressure = 1.0f;
        diagnostics_.lowerWindPressure = 1.0f;
        diagnostics_.pedalWindPressure = 1.0f;
    }

    bool PipeOrganEngine::render (
        PipeOrganAudioBlockView output) noexcept
    {
        if (! prepared_
             || output.numSamples > spec_.maximumBlockSize
             || (output.numSamples > 0 && output.left == nullptr))
            return false;

        for (std::size_t event = 0; event < eventCount_; ++event)
            if (events_[event].sampleOffset >= output.numSamples)
            {
                eventCount_ = 0;
                return false;
            }

        std::size_t eventIndex = 0;
        float peak = 0.0f;
        double sum = 0.0;
        double sumSquares = 0.0;
        std::uint32_t soundingPipes = 0;

        for (std::size_t sample = 0;
             sample < output.numSamples;
             ++sample)
        {
            while (eventIndex < eventCount_
                   && events_[eventIndex].sampleOffset == sample)
            {
                applyEvent (events_[eventIndex]);
                ++eventIndex;
            }

            updateControlsAndWindLfos();
            float rawLeft = 0.0f;
            float rawRight = 0.0f;
            std::uint32_t sampleSoundingPipes = 0;
            renderVoices (
                rawLeft, rawRight, sampleSoundingPipes);
            soundingPipes = std::max (
                soundingPipes, sampleSoundingPipes);

            float left = 0.0f;
            float right = 0.0f;
            processOutput (
                rawLeft, rawRight, left, right);

            if (! std::isfinite (left)
                 || ! std::isfinite (right))
            {
                left = 0.0f;
                right = 0.0f;
                ++invalidSamples_;
            }

            if (output.right == nullptr)
            {
                const float centred =
                    (left + right) * 0.7071067811865475f;
                left = 0.97f * centred
                       / std::sqrt (
                           1.0f + centred * centred);
            }

            output.left[sample] = left;

            if (output.right != nullptr)
                output.right[sample] = right;

            const float samplePeak =
                output.right != nullptr
                    ? std::max (
                        std::abs (left), std::abs (right))
                    : std::abs (left);
            peak = std::max (peak, samplePeak);
            const double measured =
                output.right != nullptr
                    ? 0.5 * (
                        static_cast<double> (left)
                        + static_cast<double> (right))
                    : static_cast<double> (left);
            sum += measured;
            sumSquares += measured * measured;
            ++sampleCounter_;
        }

        eventCount_ = 0;
        renderedSamples_ += output.numSamples;
        publishDiagnostics (
            peak,
            sum,
            sumSquares,
            output.numSamples,
            soundingPipes);
        return true;
    }

    bool PipeOrganEngine::queueNoteOn (
        PipeOrganManual manual,
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual)
             || note < 0
             || note >= static_cast<int> (pipeOrganMidiNoteCount)
             || velocity <= 0
             || velocity > 127)
            return false;

        return queueEvent ({
            PipeOrganEventType::noteOn,
            manual,
            static_cast<std::uint8_t> (note),
            static_cast<std::uint8_t> (velocity),
            sampleOffset
        });
    }

    bool PipeOrganEngine::queueNoteOff (
        PipeOrganManual manual,
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual)
             || note < 0
             || note >= static_cast<int> (pipeOrganMidiNoteCount))
            return false;

        return queueEvent ({
            PipeOrganEventType::noteOff,
            manual,
            static_cast<std::uint8_t> (note),
            0,
            sampleOffset
        });
    }

    bool PipeOrganEngine::queueAllNotesOff (
        PipeOrganManual manual,
        std::uint32_t sampleOffset) noexcept
    {
        if (! validManual (manual))
            return false;

        return queueEvent ({
            PipeOrganEventType::allNotesOff,
            manual,
            0,
            0,
            sampleOffset
        });
    }

    void PipeOrganEngine::setRankLevel (
        PipeOrganManual manual,
        PipeOrganRank rankValue,
        float levelZeroToOne) noexcept
    {
        if (! validManual (manual) || ! validRank (rankValue))
            return;

        const auto division = manualIndex (manual);
        const auto stop = rankIndex (rankValue);
        const float level = clampUnit (levelZeroToOne);
        rankTargets_[division][stop] = level;

        if (level > 0.0f)
            for (std::size_t note = 0;
                 note < pipeOrganMidiNoteCount;
                 ++note)
                if (desiredGate (division, note))
                    ensureActive (division, note);
    }

    float PipeOrganEngine::targetRankLevel (
        PipeOrganManual manual,
        PipeOrganRank rankValue) const noexcept
    {
        if (! validManual (manual) || ! validRank (rankValue))
            return 0.0f;

        return rankTargets_[manualIndex (manual)]
                           [rankIndex (rankValue)];
    }

    float PipeOrganEngine::currentRankLevel (
        PipeOrganManual manual,
        PipeOrganRank rankValue) const noexcept
    {
        if (! validManual (manual) || ! validRank (rankValue))
            return 0.0f;

        return rankCurrents_[manualIndex (manual)]
                            [rankIndex (rankValue)];
    }

    void PipeOrganEngine::setCoupler (
        PipeOrganManual source,
        PipeOrganManual destination,
        bool enabled) noexcept
    {
        if (! validManual (source)
             || ! validManual (destination)
             || source == destination)
            return;

        const auto sourceIndex = manualIndex (source);
        const auto destinationIndex = manualIndex (destination);
        couplers_[sourceIndex][destinationIndex] = enabled;

        if (enabled)
            for (std::size_t note = 0;
                 note < pipeOrganMidiNoteCount;
                 ++note)
                if (held_[sourceIndex][note])
                    ensureActive (destinationIndex, note);
    }

    bool PipeOrganEngine::couplerEnabled (
        PipeOrganManual source,
        PipeOrganManual destination) const noexcept
    {
        return validManual (source)
               && validManual (destination)
               && source != destination
               && couplers_[manualIndex (source)]
                           [manualIndex (destination)];
    }

    void PipeOrganEngine::setSwell (
        PipeOrganManual manual,
        float levelZeroToOne) noexcept
    {
        if (validManual (manual))
            swellTargets_[manualIndex (manual)] =
                clampUnit (levelZeroToOne);
    }

    float PipeOrganEngine::targetSwell (
        PipeOrganManual manual) const noexcept
    {
        return validManual (manual)
                   ? swellTargets_[manualIndex (manual)]
                   : 0.0f;
    }

    void PipeOrganEngine::setCrescendo (
        float levelZeroToOne) noexcept
    {
        crescendoTarget_ = clampUnit (levelZeroToOne);

        if (crescendoTarget_ > 0.0f)
            for (std::size_t manual = 0;
                 manual < pipeOrganManualCount;
                 ++manual)
                for (std::size_t note = 0;
                     note < pipeOrganMidiNoteCount;
                     ++note)
                    if (desiredGate (manual, note))
                        ensureActive (manual, note);
    }

    float PipeOrganEngine::targetCrescendo() const noexcept
    {
        return crescendoTarget_;
    }

    void PipeOrganEngine::setTremulant (
        PipeOrganManual manual,
        bool enabled,
        float depthZeroToOne,
        float rateHz) noexcept
    {
        if (! validManual (manual))
            return;

        auto& tremulant = tremulants_[manualIndex (manual)];
        tremulant.enabled = enabled;
        tremulant.depth = clampUnit (depthZeroToOne);

        if (std::isfinite (rateHz))
            tremulant.rateHz = std::max (
                2.0f, std::min (9.0f, rateHz));
    }

    void PipeOrganEngine::setTemperament (
        PipeOrganTemperament temperamentValue) noexcept
    {
        if (static_cast<std::size_t> (temperamentValue)
             >= temperamentCents.size())
            return;

        spec_.temperament = temperamentValue;
        rebuildFrequencies();
    }

    PipeOrganTemperament
    PipeOrganEngine::temperament() const noexcept
    {
        return spec_.temperament;
    }

    void PipeOrganEngine::setPerformanceEnvelopeMilliseconds (
        float attackMilliseconds,
        float releaseMilliseconds) noexcept
    {
        const auto attack = std::isfinite (attackMilliseconds)
                                && attackMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (2000.0f, attackMilliseconds))
            : 0.0f;
        const auto release = std::isfinite (releaseMilliseconds)
                                 && releaseMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (12000.0f, releaseMilliseconds))
            : 0.0f;

        if (attack == performanceAttackMilliseconds_
            && release == performanceReleaseMilliseconds_)
        {
            return;
        }

        performanceAttackMilliseconds_ = attack;
        performanceReleaseMilliseconds_ = release;
        if (prepared_)
            refreshQualityAndRankCoefficients();
    }

    void PipeOrganEngine::loadDefaultRegistration() noexcept
    {
        rankTargets_ = {};

        rankTargets_[0][rankIndex (
            PipeOrganRank::principal)] = 0.80f;
        rankTargets_[0][rankIndex (
            PipeOrganRank::flute)] = 0.30f;
        rankTargets_[0][rankIndex (
            PipeOrganRank::octave)] = 0.48f;
        rankTargets_[0][rankIndex (
            PipeOrganRank::mixture)] = 0.16f;
        rankTargets_[0][rankIndex (
            PipeOrganRank::strings)] = 0.14f;

        rankTargets_[1][rankIndex (
            PipeOrganRank::gedackt)] = 0.72f;
        rankTargets_[1][rankIndex (
            PipeOrganRank::flute)] = 0.38f;
        rankTargets_[1][rankIndex (
            PipeOrganRank::choir)] = 0.44f;
        rankTargets_[1][rankIndex (
            PipeOrganRank::reed)] = 0.10f;

        rankTargets_[2][rankIndex (
            PipeOrganRank::pedal)] = 0.84f;
        rankTargets_[2][rankIndex (
            PipeOrganRank::bourdon)] = 0.54f;
        rankTargets_[2][rankIndex (
            PipeOrganRank::principal)] = 0.18f;

        swellTargets_ = {{ 1.0f, 0.86f, 1.0f }};
        crescendoTarget_ = 0.0f;

        if (! prepared_)
        {
            rankCurrents_ = rankTargets_;
            swellCurrents_ = swellTargets_;
            crescendoCurrent_ = crescendoTarget_;
        }
        else
            for (std::size_t manual = 0;
                 manual < pipeOrganManualCount;
                 ++manual)
                for (std::size_t note = 0;
                     note < pipeOrganMidiNoteCount;
                     ++note)
                    if (desiredGate (manual, note))
                        ensureActive (manual, note);
    }

    std::size_t
    PipeOrganEngine::activeDivisionVoiceCount() const noexcept
    {
        return activeVoiceCount_;
    }

    PipeOrganDiagnostics PipeOrganEngine::diagnostics() const noexcept
    {
        return diagnostics_;
    }

    const char* PipeOrganEngine::rankName (
        PipeOrganRank rankValue) noexcept
    {
        const auto index = rankIndex (rankValue);
        return index < rankNames.size()
                   ? rankNames[index]
                   : "";
    }

    bool PipeOrganEngine::validSpec (
        const PipeOrganPrepareSpec& spec) noexcept
    {
        return std::isfinite (spec.sampleRate)
               && spec.sampleRate >= 32000.0
               && spec.sampleRate <= 384000.0
               && spec.maximumBlockSize > 0
               && spec.maximumBlockSize <= 8192
               && static_cast<std::size_t> (spec.quality) <=
                   static_cast<std::size_t> (
                       PipeOrganQuality::ultra)
               && static_cast<std::size_t> (spec.temperament)
                   < temperamentCents.size()
               && std::isfinite (spec.tuningAHz)
               && spec.tuningAHz >= 400.0f
               && spec.tuningAHz <= 480.0f
               && std::isfinite (spec.outputTrim)
               && spec.outputTrim >= 0.0f
               && spec.outputTrim <= 0.25f
               && std::isfinite (spec.stereoWidth)
               && spec.stereoWidth >= 0.0f
               && spec.stereoWidth <= 1.0f;
    }

    std::size_t PipeOrganEngine::manualIndex (
        PipeOrganManual manual) noexcept
    {
        return static_cast<std::size_t> (manual);
    }

    std::size_t PipeOrganEngine::rankIndex (
        PipeOrganRank rankValue) noexcept
    {
        return static_cast<std::size_t> (rankValue);
    }

    float PipeOrganEngine::clampUnit (float value) noexcept
    {
        if (! std::isfinite (value))
            return 0.0f;

        return std::max (0.0f, std::min (1.0f, value));
    }

    float PipeOrganEngine::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -1.0
                / (sampleRate
                   * static_cast<double> (milliseconds)
                   * 0.001)));
    }

    float PipeOrganEngine::lowpassCoefficient (
        double sampleRate,
        float frequencyHz) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -twoPi * static_cast<double> (frequencyHz)
                / sampleRate));
    }

    float PipeOrganEngine::envelopeDecayCoefficient (
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

    std::uint64_t PipeOrganEngine::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    double PipeOrganEngine::unitFromHash (
        std::uint64_t value) noexcept
    {
        return static_cast<double> (value >> 11)
               * (1.0 / 9007199254740992.0);
    }

    float PipeOrganEngine::nextNoise (
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

    float PipeOrganEngine::centsRatio (float cents) noexcept
    {
        const float exponent =
            cents * (0.6931471805599453f / 1200.0f);
        return 1.0f
               + exponent
               + 0.5f * exponent * exponent
               + (1.0f / 6.0f)
                 * exponent * exponent * exponent;
    }

    float PipeOrganEngine::smoothstep (float value) noexcept
    {
        const float clamped = clampUnit (value);
        return clamped * clamped * (3.0f - 2.0f * clamped);
    }

    bool PipeOrganEngine::queueEvent (
        const PipeOrganEvent& event) noexcept
    {
        if (! prepared_
             || event.sampleOffset >= spec_.maximumBlockSize
             || eventCount_ >= events_.size())
        {
            ++droppedEvents_;
            return false;
        }

        std::size_t insertion = eventCount_;

        while (insertion > 0
               && events_[insertion - 1].sampleOffset
                   > event.sampleOffset)
        {
            events_[insertion] = events_[insertion - 1];
            --insertion;
        }

        events_[insertion] = event;
        ++eventCount_;
        return true;
    }

    void PipeOrganEngine::applyEvent (
        const PipeOrganEvent& event) noexcept
    {
        switch (event.type)
        {
            case PipeOrganEventType::noteOn:
                activateNote (
                    event.manual, event.note, event.velocity);
                break;

            case PipeOrganEventType::noteOff:
                releaseNote (event.manual, event.note);
                break;

            case PipeOrganEventType::allNotesOff:
                releaseAllNotes (event.manual);
                break;
        }
    }

    void PipeOrganEngine::activateNote (
        PipeOrganManual manual,
        int note,
        int velocity) noexcept
    {
        const auto division = manualIndex (manual);
        const auto noteIndex = static_cast<std::size_t> (note);
        held_[division][noteIndex] = true;
        auto& sourceVoice = voices_[division][noteIndex];
        sourceVoice.velocity =
            static_cast<std::uint8_t> (velocity);
        ++sourceVoice.generation;
        ensureActive (division, noteIndex);
        ensureCoupledDestinations (division, noteIndex);
    }

    void PipeOrganEngine::releaseNote (
        PipeOrganManual manual,
        int note) noexcept
    {
        held_[manualIndex (manual)]
             [static_cast<std::size_t> (note)] = false;
    }

    void PipeOrganEngine::releaseAllNotes (
        PipeOrganManual manual) noexcept
    {
        held_[manualIndex (manual)].fill (false);
    }

    bool PipeOrganEngine::desiredGate (
        std::size_t destinationManual,
        std::size_t note) const noexcept
    {
        if (held_[destinationManual][note])
            return true;

        for (std::size_t source = 0;
             source < pipeOrganManualCount;
             ++source)
            if (source != destinationManual
                 && couplers_[source][destinationManual]
                 && held_[source][note])
                return true;

        return false;
    }

    void PipeOrganEngine::ensureActive (
        std::size_t manual,
        std::size_t note) noexcept
    {
        const std::size_t voiceId =
            manual * pipeOrganMidiNoteCount + note;

        if (activeSlots_[voiceId] >= 0)
            return;

        activeVoiceIds_[activeVoiceCount_] =
            static_cast<std::uint16_t> (voiceId);
        activeSlots_[voiceId] =
            static_cast<std::int16_t> (activeVoiceCount_);
        ++activeVoiceCount_;
    }

    void PipeOrganEngine::ensureCoupledDestinations (
        std::size_t sourceManual,
        std::size_t note) noexcept
    {
        for (std::size_t destination = 0;
             destination < pipeOrganManualCount;
             ++destination)
            if (sourceManual != destination
                 && couplers_[sourceManual][destination])
            {
                auto& voice = voices_[destination][note];
                voice.velocity =
                    voices_[sourceManual][note].velocity;
                ++voice.generation;
                ensureActive (destination, note);
            }
    }

    void PipeOrganEngine::removeActiveAt (
        std::size_t activeIndex) noexcept
    {
        const auto removed =
            static_cast<std::size_t> (
                activeVoiceIds_[activeIndex]);
        --activeVoiceCount_;

        if (activeIndex != activeVoiceCount_)
        {
            const auto moved =
                activeVoiceIds_[activeVoiceCount_];
            activeVoiceIds_[activeIndex] = moved;
            activeSlots_[static_cast<std::size_t> (moved)] =
                static_cast<std::int16_t> (activeIndex);
        }

        activeSlots_[removed] = -1;
    }

    void PipeOrganEngine::buildSineTable() noexcept
    {
        for (std::size_t index = 0;
             index < sineTableSize;
             ++index)
            sineTable_[index] = static_cast<float> (
                std::sin (
                    twoPi
                    * static_cast<double> (index)
                    / static_cast<double> (sineTableSize)));
    }

    void PipeOrganEngine::buildPipeWaveTables() noexcept
    {
        normalWaveTables_ = {};
        brightWaveTables_ = {};
        constexpr std::size_t sineStride =
            sineTableSize / pipeWaveTableSize;

        for (std::size_t rankValue = 0;
             rankValue < pipeOrganRankCount;
             ++rankValue)
            for (std::size_t allowed = 1;
                 allowed <= partialLimit_;
                 ++allowed)
                for (std::size_t sample = 0;
                     sample < pipeWaveTableSize;
                     ++sample)
                {
                    float normal = 0.0f;
                    float bright = 0.0f;

                    for (std::size_t partial = 0;
                         partial < allowed;
                         ++partial)
                    {
                        const float coefficient =
                            rankDefinitions[rankValue]
                                .harmonics[partial];

                        if (coefficient == 0.0f)
                            continue;

                        const std::size_t harmonic = partial + 1;
                        const std::size_t sineIndex =
                            (sample * harmonic * sineStride)
                            & (sineTableSize - 1);
                        const float sine = sineTable_[sineIndex];
                        const float brightGain =
                            1.0f
                            + rankDefinitions[rankValue]
                                  .speechBrightness
                              * static_cast<float> (partial)
                              / static_cast<float> (
                                  std::max<std::size_t> (
                                      1, partialLimit_ - 1));
                        normal += coefficient * sine;
                        bright += coefficient * brightGain * sine;
                    }

                    normalWaveTables_[rankValue][allowed - 1][sample] =
                        normal
                        * harmonicNormalisation_[rankValue];
                    brightWaveTables_[rankValue][allowed - 1][sample] =
                        bright
                        * harmonicNormalisation_[rankValue];
                }
    }

    void PipeOrganEngine::rebuildFrequencies() noexcept
    {
        const auto temperamentIndex =
            static_cast<std::size_t> (spec_.temperament);

        for (std::size_t rankValue = 0;
             rankValue < pipeOrganRankCount;
             ++rankValue)
            for (std::size_t note = 0;
                 note < pipeOrganMidiNoteCount;
                 ++note)
            {
                const int rankMidi =
                    static_cast<int> (note)
                    + rankDefinitions[rankValue].semitoneOffset;
                int pitchClass = rankMidi % 12;

                if (pitchClass < 0)
                    pitchClass += 12;

                const double temperedMidi =
                    static_cast<double> (rankMidi)
                    + static_cast<double> (
                        temperamentCents[temperamentIndex]
                                        [static_cast<std::size_t> (
                                            pitchClass)])
                      * 0.01;
                frequencies_[rankValue][note] =
                    spec_.tuningAHz
                    * static_cast<float> (
                        std::pow (
                            2.0,
                            (temperedMidi - 69.0) / 12.0));
                const float guard =
                    static_cast<float> (
                        spec_.sampleRate * 0.46);
                const auto allowed = static_cast<std::size_t> (
                    std::max (
                        0.0f,
                        std::floor (
                            guard
                            / std::max (
                                frequencies_[rankValue][note],
                                0.001f))));
                allowedPartials_[rankValue][note] =
                    static_cast<std::uint8_t> (
                        std::min (partialLimit_, allowed));
            }
    }

    void PipeOrganEngine::rebuildPanGains() noexcept
    {
        const float qualityWidth =
            spec_.quality == PipeOrganQuality::eco
                ? 0.52f
                : spec_.quality == PipeOrganQuality::standard
                    ? 0.82f
                    : 1.0f;

        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
            for (std::size_t rankValue = 0;
                 rankValue < pipeOrganRankCount;
                 ++rankValue)
                for (std::size_t note = 0;
                     note < pipeOrganMidiNoteCount;
                     ++note)
                {
                    const float keyboard =
                        2.0f
                        * static_cast<float> (note)
                        / static_cast<float> (
                            pipeOrganMidiNoteCount - 1)
                        - 1.0f;
                    const auto positionHash = mixedHash (
                        spec_.deterministicSeed
                        ^ (static_cast<std::uint64_t> (
                               manual + 1) << 44)
                        ^ (static_cast<std::uint64_t> (
                               rankValue + 1) << 28)
                        ^ static_cast<std::uint64_t> (note + 1));
                    const float microPosition =
                        static_cast<float> (
                            2.0 * unitFromHash (positionHash) - 1.0)
                        * 0.075f;
                    const float manualOffset =
                        manual == 0 ? -0.08f
                        : manual == 1 ? 0.08f
                                      : 0.0f;
                    const float position = std::max (
                        -1.0f,
                        std::min (
                            1.0f,
                            (0.62f * keyboard
                             + microPosition
                             + manualOffset)
                            * spec_.stereoWidth
                            * qualityWidth));
                    const float angle =
                        (position + 1.0f)
                        * static_cast<float> (pi * 0.25);
                    panLeft_[manual][rankValue][note] =
                        std::cos (angle);
                    panRight_[manual][rankValue][note] =
                        std::sin (angle);
                }
    }

    void PipeOrganEngine::refreshQualityAndRankCoefficients() noexcept
    {
        partialLimit_ =
            spec_.quality == PipeOrganQuality::eco
                ? 4
                : spec_.quality == PipeOrganQuality::standard
                    ? 8
                    : 12;

        for (std::size_t rankValue = 0;
             rankValue < pipeOrganRankCount;
             ++rankValue)
        {
            float sum = 0.0f;

            for (std::size_t partial = 0;
                 partial < partialLimit_;
                 ++partial)
                sum += std::abs (
                    rankDefinitions[rankValue]
                        .harmonics[partial]);

            harmonicNormalisation_[rankValue] =
                sum > 0.0f ? 1.0f / sum : 1.0f;
            attackCoefficients_[rankValue] =
                smoothingCoefficient (
                    spec_.sampleRate,
                    performanceAttackMilliseconds_ > 0.0f
                        ? performanceAttackMilliseconds_
                        : rankDefinitions[rankValue]
                            .attackMilliseconds);
            releaseCoefficients_[rankValue] =
                smoothingCoefficient (
                    spec_.sampleRate,
                    performanceReleaseMilliseconds_ > 0.0f
                        ? performanceReleaseMilliseconds_
                        : rankDefinitions[rankValue]
                            .releaseMilliseconds);
            speechDecayCoefficients_[rankValue] =
                envelopeDecayCoefficient (
                    spec_.sampleRate,
                    rankDefinitions[rankValue]
                        .speechMilliseconds);
            chiffDecayCoefficients_[rankValue] =
                envelopeDecayCoefficient (
                    spec_.sampleRate,
                    rankDefinitions[rankValue]
                        .chiffMilliseconds);

            for (std::size_t note = 0;
                 note < pipeOrganMidiNoteCount;
                 ++note)
            {
                const float frequency =
                    frequencies_[rankValue][note];
                const float guard =
                    static_cast<float> (
                        spec_.sampleRate * 0.46);
                const auto allowed = static_cast<std::size_t> (
                    std::max (
                        0.0f,
                        std::floor (
                            guard
                            / std::max (frequency, 0.001f))));
                allowedPartials_[rankValue][note] =
                    static_cast<std::uint8_t> (
                        std::min (partialLimit_, allowed));
            }
        }
    }

    float PipeOrganEngine::waveLookup (
        const PipeWaveTable& table,
        float phase) const noexcept
    {
        if (phase >= 1.0f)
            phase -= 1.0f;
        else if (phase < 0.0f)
            phase += 1.0f;

        const float position =
            phase * static_cast<float> (pipeWaveTableSize);
        const auto unwrappedBase =
            static_cast<std::size_t> (position);
        const auto base =
            unwrappedBase & (pipeWaveTableSize - 1);

        if (spec_.quality == PipeOrganQuality::eco)
            return table[base];

        const float fraction =
            position - static_cast<float> (unwrappedBase);
        const auto next =
            (base + 1) & (pipeWaveTableSize - 1);

        if (spec_.quality == PipeOrganQuality::standard)
            return table[base]
                   + fraction
                     * (table[next] - table[base]);

        const auto previous =
            (base + pipeWaveTableSize - 1)
            & (pipeWaveTableSize - 1);
        const auto after =
            (base + 2) & (pipeWaveTableSize - 1);
        const float y0 = table[previous];
        const float y1 = table[base];
        const float y2 = table[next];
        const float y3 = table[after];
        const float a0 =
            -0.5f * y0 + 1.5f * y1
            - 1.5f * y2 + 0.5f * y3;
        const float a1 =
            y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float a2 = -0.5f * y0 + 0.5f * y2;
        return ((a0 * fraction + a1) * fraction + a2)
               * fraction + y1;
    }

    void PipeOrganEngine::startPipe (
        PipeState& pipe,
        std::size_t manual,
        std::size_t note,
        std::size_t rankValue,
        std::uint32_t generation) noexcept
    {
        const auto identity =
            spec_.deterministicSeed
            ^ (static_cast<std::uint64_t> (manual + 1) << 52)
            ^ (static_cast<std::uint64_t> (rankValue + 1) << 36)
            ^ (static_cast<std::uint64_t> (note + 1) << 20)
            ^ static_cast<std::uint64_t> (generation);
        pipe.randomState = mixedHash (identity);

        if (pipe.randomState == 0)
            pipe.randomState = 1;

        pipe.phase = static_cast<float> (
            unitFromHash (mixedHash (
                identity ^ 0xa0761d6478bd642fULL)));
        pipe.secondaryPhase = static_cast<float> (
            unitFromHash (mixedHash (
                identity ^ 0xe7037ed1a0b428dbULL)));
        pipe.phaseIncrement = 0.0f;
        pipe.secondaryIncrement = 0.0f;
        pipe.envelope = 0.0f;
        pipe.speechEnvelope = 1.0f;
        pipe.chiffEnvelope = 1.0f;
        pipe.wind = 0.0f;
        pipe.windTarget = 0.0f;
        pipe.noiseLow = 0.0f;
        pipe.noiseHigh = 0.0f;
        pipe.speaking = true;
    }

    float PipeOrganEngine::renderPipe (
        PipeState& pipe,
        std::size_t manual,
        std::size_t note,
        std::size_t rankValue,
        float effectiveLevel,
        bool gate,
        float tremulant) noexcept
    {
        const bool shouldSpeak =
            gate && effectiveLevel > 0.000001f;
        auto& voice = voices_[manual][note];

        if (shouldSpeak && ! pipe.speaking)
            startPipe (
                pipe,
                manual,
                note,
                rankValue,
                voice.generation);

        if (! pipe.speaking)
            return 0.0f;

        const float envelopeTarget = shouldSpeak ? 1.0f : 0.0f;
        const float envelopeCoefficient =
            shouldSpeak
                ? attackCoefficients_[rankValue]
                : releaseCoefficients_[rankValue];
        pipe.envelope +=
            (envelopeTarget - pipe.envelope)
            * envelopeCoefficient;
        pipe.speechEnvelope *=
            speechDecayCoefficients_[rankValue];
        pipe.chiffEnvelope *=
            chiffDecayCoefficients_[rankValue];

        if (std::abs (
                envelopeTarget - pipe.envelope) < 0.000001f)
            pipe.envelope = envelopeTarget;

        if (pipe.speechEnvelope < 0.000001f)
            pipe.speechEnvelope = 0.0f;

        if (pipe.chiffEnvelope < 0.000001f)
            pipe.chiffEnvelope = 0.0f;

        if ((sampleCounter_ & 63ULL) == 0)
            pipe.windTarget = nextNoise (pipe.randomState);

        pipe.wind +=
            pipeWindCoefficient_
            * (pipe.windTarget - pipe.wind);
        const auto& definition = rankDefinitions[rankValue];
        const float baseFrequency =
            frequencies_[rankValue][note];

        if ((sampleCounter_ & 15ULL) == 0
             || pipe.phaseIncrement == 0.0f)
        {
            const float pressureCents =
                (windPressure_[manual] - 1.0f) * 34.0f;
            const float tremulantCents =
                tremulants_[manual].enabled
                    ? tremulant
                      * tremulants_[manual].depth
                      * 7.5f
                    : 0.0f;
            const float windCents =
                pipe.wind * definition.windCents
                + windNoise_[manual] * 1.4f
                + pressureCents
                + tremulantCents;
            pipe.phaseIncrement =
                baseFrequency
                * centsRatio (windCents)
                / static_cast<float> (spec_.sampleRate);
            pipe.secondaryIncrement =
                pipe.phaseIncrement
                * centsRatio (
                    definition.secondaryDetuneCents);
        }

        pipe.phase += pipe.phaseIncrement;

        if (pipe.phase >= 1.0f)
            pipe.phase -= 1.0f;

        const bool useSecondary =
            definition.secondaryDetuneCents != 0.0f
            && spec_.quality != PipeOrganQuality::eco;
        const bool renderSecondary =
            useSecondary
            && (spec_.quality == PipeOrganQuality::ultra
                || activeVoiceCount_ < 96);

        if (useSecondary)
        {
            pipe.secondaryPhase +=
                pipe.secondaryIncrement;

            if (pipe.secondaryPhase >= 1.0f)
                pipe.secondaryPhase -= 1.0f;
        }

        const std::size_t allowed =
            allowedPartials_[rankValue][note];
        float tonal = 0.0f;

        if (allowed > 0)
        {
            const auto& normalTable =
                normalWaveTables_[rankValue][allowed - 1];
            const float normal =
                waveLookup (normalTable, pipe.phase);
            float primary = normal;

            if (pipe.speechEnvelope > 0.00001f)
            {
                const auto& brightTable =
                    brightWaveTables_[rankValue][allowed - 1];
                const float bright =
                    waveLookup (brightTable, pipe.phase);
                primary +=
                    pipe.speechEnvelope * (bright - normal);
            }

            tonal = primary;

            if (renderSecondary)
            {
                const float secondaryNormal =
                    waveLookup (
                        normalTable, pipe.secondaryPhase);
                float secondary = secondaryNormal;

                if (pipe.speechEnvelope > 0.00001f)
                {
                    const auto& brightTable =
                        brightWaveTables_[rankValue][allowed - 1];
                    const float secondaryBright =
                        waveLookup (
                            brightTable, pipe.secondaryPhase);
                    secondary +=
                        pipe.speechEnvelope
                        * (secondaryBright - secondaryNormal);
                }

                tonal =
                    0.56f * primary + 0.44f * secondary;
            }
        }

        float chiff = 0.0f;

        if (pipe.chiffEnvelope > 0.000001f
             && definition.chiffAmount > 0.0f
             && spec_.quality != PipeOrganQuality::eco)
        {
            const float noise = nextNoise (pipe.randomState);
            pipe.noiseLow +=
                chiffLowCoefficient_
                * (noise - pipe.noiseLow);
            const float highPassed = noise - pipe.noiseLow;
            pipe.noiseHigh +=
                chiffHighCoefficient_
                * (highPassed - pipe.noiseHigh);
            chiff =
                pipe.noiseHigh
                * pipe.chiffEnvelope
                * definition.chiffAmount
                * (0.35f + 0.65f * pipe.envelope);
        }

        const float tremulantAmplitude =
            tremulants_[manual].enabled
                ? 1.0f
                  + tremulant
                    * tremulants_[manual].depth * 0.12f
                : 1.0f;
        const float output =
            (tonal * pipe.envelope + chiff)
            * definition.gain
            * effectiveLevel
            * tremulantAmplitude;

        if (! shouldSpeak
             && pipe.envelope < 0.000001f
             && pipe.chiffEnvelope < 0.000001f)
        {
            const auto randomState = pipe.randomState;
            pipe = {};
            pipe.randomState = randomState;
        }

        return output;
    }

    void PipeOrganEngine::renderVoices (
        float& left,
        float& right,
        std::uint32_t& soundingPipes) noexcept
    {
        left = 0.0f;
        right = 0.0f;
        std::size_t activeIndex = 0;

        while (activeIndex < activeVoiceCount_)
        {
            const auto voiceId = static_cast<std::size_t> (
                activeVoiceIds_[activeIndex]);
            const std::size_t manual =
                voiceId / pipeOrganMidiNoteCount;
            const std::size_t note =
                voiceId % pipeOrganMidiNoteCount;
            auto& voice = voices_[manual][note];
            const bool gate = desiredGate (manual, note);
            bool voiceNeeded = false;
            const float tremulant = tremulantValue (manual);
            const float divisionGain =
                manual == manualIndex (PipeOrganManual::pedal)
                    ? 0.62f + 0.38f * swellCurrents_[manual]
                    : 0.16f + 0.84f * swellCurrents_[manual];

            std::uint16_t rankMask =
                voice.speakingMask;

            if (gate)
                rankMask = static_cast<std::uint16_t> (
                    rankMask | effectiveRankMasks_[manual]);

            while (rankMask != 0)
            {
                const std::size_t rankValue =
                    firstSetBit_[rankMask];
                rankMask = static_cast<std::uint16_t> (
                    rankMask & (rankMask - 1U));
                const float level =
                    effectiveRankLevel (manual, rankValue);
                auto& pipe = voice.pipes[rankValue];

                if (gate && level > 0.000001f)
                    voiceNeeded = true;

                if (! pipe.speaking
                     && (! gate || level <= 0.000001f))
                    continue;

                const float pipeSample = renderPipe (
                    pipe,
                    manual,
                    note,
                    rankValue,
                    level,
                    gate,
                    tremulant);

                if (pipe.speaking)
                {
                    voiceNeeded = true;
                    voice.speakingMask =
                        static_cast<std::uint16_t> (
                            voice.speakingMask
                            | (1U << rankValue));
                    ++soundingPipes;
                    lastWindLoad_[manual] +=
                        pipe.envelope * level * 0.0026f;
                }
                else
                    voice.speakingMask =
                        static_cast<std::uint16_t> (
                            voice.speakingMask
                            & ~static_cast<std::uint16_t> (
                                1U << rankValue));

                const float scaled =
                    pipeSample * divisionGain;
                left +=
                    scaled * panLeft_[manual][rankValue][note];
                right +=
                    scaled * panRight_[manual][rankValue][note];
            }

            if (! voiceNeeded)
            {
                const auto generation = voice.generation;
                const auto velocity = voice.velocity;
                voice = {};
                voice.generation = generation;
                voice.velocity = velocity;
                removeActiveAt (activeIndex);
                continue;
            }

            ++activeIndex;
        }
    }

    void PipeOrganEngine::updateControlsAndWindLfos() noexcept
    {
        crescendoCurrent_ +=
            (crescendoTarget_ - crescendoCurrent_)
            * crescendoSmoothingCoefficient_;

        for (std::size_t manual = 0;
             manual < pipeOrganManualCount;
             ++manual)
        {
            for (std::size_t rankValue = 0;
                 rankValue < pipeOrganRankCount;
                 ++rankValue)
                rankCurrents_[manual][rankValue] +=
                    (rankTargets_[manual][rankValue]
                     - rankCurrents_[manual][rankValue])
                    * rankSmoothingCoefficient_;

            swellCurrents_[manual] +=
                (swellTargets_[manual] - swellCurrents_[manual])
                * swellSmoothingCoefficient_;
            auto& tremulant = tremulants_[manual];
            tremulant.phase +=
                tremulant.rateHz
                / static_cast<float> (spec_.sampleRate);

            if (tremulant.phase >= 1.0f)
                tremulant.phase -= 1.0f;

            tremulantSamples_[manual] = std::sin (
                static_cast<float> (twoPi)
                * tremulant.phase);

            if ((sampleCounter_ & 127ULL) == 0)
                windNoiseTarget_[manual] =
                    nextNoise (windRandom_[manual]);

            windNoise_[manual] +=
                (windNoiseTarget_[manual] - windNoise_[manual])
                * windNoiseCoefficient_;
            const float pressureTarget = std::max (
                0.925f,
                std::min (
                    1.018f,
                    1.0f
                    - std::min (0.070f, lastWindLoad_[manual])
                    + 0.006f * windNoise_[manual]));
            windPressure_[manual] +=
                (pressureTarget - windPressure_[manual])
                * windPressureCoefficient_;
            lastWindLoad_[manual] = 0.0f;

            for (std::size_t rankValue = 0;
                 rankValue < pipeOrganRankCount;
                 ++rankValue)
            {
                const auto& definition =
                    rankDefinitions[rankValue];
                const float denominator =
                    std::max (
                        0.001f,
                        1.0f
                        - definition.crescendoThreshold);
                const float progression = smoothstep (
                    (crescendoCurrent_
                     - definition.crescendoThreshold)
                    / denominator);
                const float crescendoRegistration =
                    progression
                    * definition.crescendoMaximum;
                float result = std::max (
                    rankCurrents_[manual][rankValue],
                    crescendoRegistration);

                if (manual == manualIndex (
                        PipeOrganManual::pedal)
                     && rankValue != rankIndex (
                         PipeOrganRank::pedal)
                     && rankValue != rankIndex (
                         PipeOrganRank::bourdon)
                     && rankValue != rankIndex (
                         PipeOrganRank::principal))
                    result *= 0.72f;

                effectiveRankLevels_[manual][rankValue] =
                    result;

                if (result > 0.000001f)
                    effectiveRankMasks_[manual] =
                        static_cast<std::uint16_t> (
                            effectiveRankMasks_[manual]
                            | (1U << rankValue));
                else
                    effectiveRankMasks_[manual] =
                        static_cast<std::uint16_t> (
                            effectiveRankMasks_[manual]
                            & ~static_cast<std::uint16_t> (
                                1U << rankValue));
            }
        }
    }

    float PipeOrganEngine::effectiveRankLevel (
        std::size_t manual,
        std::size_t rankValue) const noexcept
    {
        return effectiveRankLevels_[manual][rankValue];
    }

    float PipeOrganEngine::tremulantValue (
        std::size_t manual) const noexcept
    {
        return tremulantSamples_[manual];
    }

    void PipeOrganEngine::processOutput (
        float inputLeft,
        float inputRight,
        float& outputLeft,
        float& outputRight) noexcept
    {
        const float scaledLeft =
            inputLeft * spec_.outputTrim;
        const float scaledRight =
            inputRight * spec_.outputTrim;
        const float dcLeft =
            scaledLeft - dcInputLeft_
            + dcCoefficient_ * dcOutputLeft_;
        const float dcRight =
            scaledRight - dcInputRight_
            + dcCoefficient_ * dcOutputRight_;
        dcInputLeft_ = scaledLeft;
        dcOutputLeft_ = dcLeft;
        dcInputRight_ = scaledRight;
        dcOutputRight_ = dcRight;
        outputLeft =
            0.97f * dcLeft
            / std::sqrt (1.0f + dcLeft * dcLeft);
        outputRight =
            0.97f * dcRight
            / std::sqrt (1.0f + dcRight * dcRight);
    }

    void PipeOrganEngine::publishDiagnostics (
        float peak,
        double sum,
        double sumSquares,
        std::size_t samples,
        std::uint32_t soundingPipes) noexcept
    {
        diagnostics_.renderedSamples = renderedSamples_;
        diagnostics_.droppedEvents = droppedEvents_;
        diagnostics_.invalidSamples = invalidSamples_;
        diagnostics_.activeDivisionVoices =
            static_cast<std::uint32_t> (activeVoiceCount_);
        diagnostics_.soundingPipes = soundingPipes;
        diagnostics_.lastBlockPeak = peak;
        diagnostics_.upperWindPressure = windPressure_[0];
        diagnostics_.lowerWindPressure = windPressure_[1];
        diagnostics_.pedalWindPressure = windPressure_[2];

        if (samples > 0)
        {
            const double inverse =
                1.0 / static_cast<double> (samples);
            diagnostics_.lastBlockMean =
                static_cast<float> (sum * inverse);
            diagnostics_.lastBlockRms =
                static_cast<float> (
                    std::sqrt (sumSquares * inverse));
        }
        else
        {
            diagnostics_.lastBlockMean = 0.0f;
            diagnostics_.lastBlockRms = 0.0f;
        }
    }
}
