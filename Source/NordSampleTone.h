#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>

// Cadeia de cor para os pads sampleados. A taxonomia segue os controles
// documentados do Nord Stage 4 (LP12, LP24, LP M, LP+HP, BP, HP, tracking,
// drive, Bright e envelope A/D/R), mas a implementacao DSP e original do
// ChurchDAW. Nada deste arquivo depende ou copia codigo AGPL do OpenNord.
namespace ChurchDAWNordSample
{
enum FilterType
{
    lp12 = 0,
    lp24,
    lpM,
    lpHp,
    bandPass,
    highPass,
    numFilterTypes
};

struct FilterSettings
{
    bool enabled = false;
    int type = lp24;
    float cutoffHz = 18000.0f;
    float resonance = 0.0f;
    int keyTrack = 0;       // Off, 1/3, 2/3, 1:1.
    int drive = 0;          // Off, 1, 2, 3.
    float envelopeAmount = 0.0f;
    float envelopeAttackSeconds = 0.001f;
    float envelopeDecaySeconds = 10.0f; // maximo = Sustain.
    float envelopeReleaseSeconds = 0.55f;
    bool velocity = false;
    bool bright = false;
};

inline FilterSettings bounded (FilterSettings value) noexcept
{
    const auto finiteOr = [] (float candidate, float fallback) noexcept
    {
        return std::isfinite (candidate) ? candidate : fallback;
    };

    value.type = juce::jlimit (0, numFilterTypes - 1, value.type);
    value.cutoffHz = juce::jlimit (
        20.0f, 20000.0f, finiteOr (value.cutoffHz, 18000.0f));
    value.resonance = juce::jlimit (
        0.0f, 1.0f, finiteOr (value.resonance, 0.0f));
    value.keyTrack = juce::jlimit (0, 3, value.keyTrack);
    value.drive = juce::jlimit (0, 3, value.drive);
    value.envelopeAmount = juce::jlimit (
        -1.0f, 1.0f, finiteOr (value.envelopeAmount, 0.0f));
    value.envelopeAttackSeconds = juce::jlimit (
        0.001f, 10.0f,
        finiteOr (value.envelopeAttackSeconds, 0.001f));
    value.envelopeDecaySeconds = juce::jlimit (
        0.001f, 10.0f,
        finiteOr (value.envelopeDecaySeconds, 10.0f));
    value.envelopeReleaseSeconds = juce::jlimit (
        0.01f, 10.0f,
        finiteOr (value.envelopeReleaseSeconds, 0.55f));
    return value;
}

inline float keyTrackRatio (int selection) noexcept
{
    constexpr std::array<float, 4> values {
        0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f
    };
    return values[(std::size_t) juce::jlimit (0, 3, selection)];
}

inline float cutoffFor (const FilterSettings& rawSettings,
                        int midiNote, int velocity,
                        float envelopeValue,
                        double sampleRate) noexcept
{
    const auto settings = bounded (rawSettings);
    const float trackOctaves = ((float) juce::jlimit (0, 127, midiNote)
                                - 60.0f) / 12.0f
                             * keyTrackRatio (settings.keyTrack);
    const float velocityScale = settings.velocity
        ? juce::jmap ((float) juce::jlimit (1, 127, velocity),
                     1.0f, 127.0f, 0.18f, 1.0f)
        : 1.0f;
    // O curso bipolar cobre sete oitavas, suficiente para fechar ou abrir o
    // filtro integralmente sem um salto numerico no topo do knob.
    const float envelopeOctaves = settings.envelopeAmount * 7.0f
                                * juce::jlimit (0.0f, 1.0f, envelopeValue)
                                * velocityScale;
    const float nyquistSafe = (float) juce::jmax (
        40.0, juce::jmin (20000.0, sampleRate * 0.45));
    return juce::jlimit (
        20.0f, nyquistSafe,
        settings.cutoffHz * std::exp2 (trackOctaves + envelopeOctaves));
}

// Estado polifonico: cada voz sampled recebe seu proprio filtro e envelope.
// Isto evita que uma nota grave feche o filtro das notas agudas do mesmo acorde
// e reproduz a semantica por voz do sintetizador de referencia.
class VoiceFilter
{
public:
    void start (double newSampleRate, const FilterSettings& rawSettings)
    {
        sampleRate = juce::jmax (8000.0, newSampleRate);
        resetSignalState();
        brightCoefficient = 1.0f - std::exp (
            -juce::MathConstants<float>::twoPi * 2600.0f
            / (float) sampleRate);
        envelope.setSampleRate (sampleRate);
        envelope.reset();
        envelope.setParameters (envelopeParameters (rawSettings));
        envelope.noteOn();
    }

    void release (const FilterSettings& rawSettings)
    {
        envelope.setParameters (envelopeParameters (rawSettings));
        envelope.noteOff();
    }

    void reset() noexcept
    {
        envelope.reset();
        resetSignalState();
    }

    void advanceSilently (int samples, const FilterSettings& rawSettings)
    {
        juce::ignoreUnused (rawSettings);
        for (int i = 0; i < samples; ++i)
            (void) envelope.getNextSample();
    }

    void processStereo (float& left, float& right,
                        const FilterSettings& rawSettings,
                        int midiNote, int velocity) noexcept
    {
        const auto settings = bounded (rawSettings);
        processStereoBounded (
            left, right, settings, midiNote, velocity);
    }

    // Caminho do sampler: nordFilterSettingsFor() ja devolve os parametros
    // limitados uma vez por bloco. Repetir bounded() para cada amostra de cada
    // voz consumia uma parte relevante do callback sem alterar um unico valor.
    forcedinline void processStereoBounded (float& left, float& right,
                                            const FilterSettings& settings,
                                            int midiNote, int velocity) noexcept
    {
        const float env = envelope.getNextSample();

        if (! settings.enabled && ! settings.bright)
            return;

        const bool urgentCoefficientChange =
            settings.type != lastType
            || std::abs (settings.resonance - lastResonance) > 0.001f;

        if (--coefficientCountdown <= 0 || urgentCoefficientChange)
        {
            // Durante attack/decay, o envelope muda e mantemos exatamente a
            // cadencia antiga de oito samples. Em sustain, todos estes valores
            // ficam identicos: recalcular tan()/exp2() produziria os mesmos
            // bits e apenas queimaria CPU para cada uma das ~96 vozes.
            const bool coefficientInputsChanged =
                urgentCoefficientChange
                || settings.cutoffHz != lastCutoffHz
                || settings.resonance != lastResonance
                || settings.keyTrack != lastKeyTrack
                || settings.envelopeAmount != lastEnvelopeAmount
                || settings.velocity != lastVelocitySensitivity
                || midiNote != lastMidiNote
                || velocity != lastCoefficientVelocity
                || (settings.envelopeAmount != 0.0f
                    && env != lastEnvelopeValue);

            if (coefficientInputsChanged)
                updateCoefficients (
                    settings, midiNote, velocity, env);

            coefficientCountdown = 8;
        }

        if (settings.bright && velocity != lastBrightVelocity)
        {
            lastBrightVelocity = velocity;
            brightDynamics = juce::jmap (
                (float) juce::jlimit (1, 127, velocity),
                1.0f, 127.0f, 0.28f, 0.66f);
        }

        left = processChannel (left, 0, settings, brightDynamics);
        right = processChannel (right, 1, settings, brightDynamics);
    }

private:
    struct State
    {
        float ic1 = 0.0f;
        float ic2 = 0.0f;
    };

    struct Coefficients
    {
        float a1 = 1.0f;
        float a2 = 0.0f;
        float a3 = 0.0f;
        float k = 1.41421356f;
    };

    struct Outputs
    {
        float low = 0.0f;
        float band = 0.0f;
        float high = 0.0f;
    };

    static juce::ADSR::Parameters envelopeParameters (
        const FilterSettings& rawSettings) noexcept
    {
        const auto settings = bounded (rawSettings);
        const bool sustainMode = settings.envelopeDecaySeconds >= 9.995f;
        juce::ADSR::Parameters result;
        result.attack = settings.envelopeAttackSeconds;
        result.decay = sustainMode ? 0.0f : settings.envelopeDecaySeconds;
        result.sustain = sustainMode ? 1.0f : 0.0f;
        result.release = settings.envelopeReleaseSeconds;
        return result;
    }

    void resetSignalState() noexcept
    {
        for (auto& channel : states)
            for (auto& stage : channel)
                stage = {};
        brightLow.fill (0.0f);
        coefficientCountdown = 0;
        lastType = -1;
        lastResonance = -1.0f;
        lastCutoffHz = -1.0f;
        lastKeyTrack = -1;
        lastEnvelopeAmount = 2.0f;
        lastVelocitySensitivity = false;
        lastMidiNote = -1;
        lastCoefficientVelocity = -1;
        lastEnvelopeValue = 2.0f;
        lastBrightVelocity = -1;
        brightDynamics = 0.0f;
    }

    static Coefficients coefficientsFor (float cutoffHz, float resonance,
                                         double sampleRate) noexcept
    {
        cutoffHz = juce::jlimit (
            20.0f, (float) juce::jmin (20000.0, sampleRate * 0.45),
            cutoffHz);
        resonance = juce::jlimit (0.0f, 1.0f, resonance);
        const float g = std::tan (
            juce::MathConstants<float>::pi * cutoffHz
            / (float) sampleRate);
        Coefficients c;
        // k=sqrt(2) em zero entrega resposta musical sem pico; aproximar zero
        // em 100% leva a auto-oscilacao controlada do SVF.
        c.k = juce::jmax (0.055f,
                          1.41421356f - resonance * 1.35921356f);
        c.a1 = 1.0f / (1.0f + g * (g + c.k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }

    void updateCoefficients (const FilterSettings& settings,
                             int midiNote, int velocity,
                             float envelopeValue) noexcept
    {
        const float cutoff = cutoffFor (
            settings, midiNote, velocity, envelopeValue, sampleRate);
        stageCoefficients[0] = coefficientsFor (
            cutoff, settings.type == lpHp ? 0.0f : settings.resonance,
            sampleRate);

        float secondCutoff = cutoff;
        float secondResonance = settings.resonance;

        if (settings.type == lpHp)
        {
            // Neste modo o knob RES vira FREQ HP, como no painel original.
            secondCutoff = 20.0f * std::pow (
                1000.0f, juce::jlimit (0.0f, 1.0f, settings.resonance));
            secondResonance = 0.0f;
        }

        stageCoefficients[1] = coefficientsFor (
            secondCutoff, secondResonance, sampleRate);
        lastType = settings.type;
        lastResonance = settings.resonance;
        lastCutoffHz = settings.cutoffHz;
        lastKeyTrack = settings.keyTrack;
        lastEnvelopeAmount = settings.envelopeAmount;
        lastVelocitySensitivity = settings.velocity;
        lastMidiNote = midiNote;
        lastCoefficientVelocity = velocity;
        lastEnvelopeValue = envelopeValue;
    }

    static forcedinline Outputs processStage (
        float input, State& state, const Coefficients& c) noexcept
    {
        const float v3 = input - state.ic2;
        const float v1 = c.a1 * state.ic1 + c.a2 * v3;
        const float v2 = state.ic2 + c.a2 * state.ic1 + c.a3 * v3;
        state.ic1 = 2.0f * v1 - state.ic1;
        state.ic2 = 2.0f * v2 - state.ic2;
        return { v2, v1, input - c.k * v1 - v2 };
    }

    static forcedinline float driven (float input, int drive) noexcept
    {
        constexpr std::array<float, 4> gains { 1.0f, 1.8f, 3.4f, 6.4f };
        constexpr std::array<float, 4> normalisers {
            1.0f, 1.056198f, 1.002229f, 1.000006f
        };
        return drive == 0 ? input
                          : std::tanh (input * gains[(std::size_t) drive])
                              * normalisers[(std::size_t) drive];
    }

    forcedinline float processChannel (
        float input, int channel,
        const FilterSettings& settings,
        float cachedBrightDynamics) noexcept
    {
        if (settings.bright)
        {
            auto& low = brightLow[(std::size_t) channel];
            low += brightCoefficient * (input - low);
            input += (input - low) * cachedBrightDynamics;
        }

        if (! settings.enabled)
            return input;

        input = driven (input, settings.drive);
        auto& state = states[(std::size_t) channel];
        const auto first = processStage (
            input, state[0], stageCoefficients[0]);

        float output = input;
        switch (settings.type)
        {
            case lp12:
                output = first.low;
                break;
            case lp24:
                output = processStage (
                    first.low, state[1], stageCoefficients[1]).low;
                break;
            case lpM:
            {
                const float ladder = processStage (
                    std::tanh (first.low * 1.12f),
                    state[1], stageCoefficients[1]).low;
                // Um pequeno caminho de 2 polos conserva o grave quando a
                // ressonancia sobe, caracteristica que distingue o LP M.
                output = ladder * 0.88f + first.low * 0.12f;
                break;
            }
            case lpHp:
                output = first.low + processStage (
                    input, state[1], stageCoefficients[1]).high;
                break;
            case bandPass:
                output = first.band * 1.25f;
                break;
            case highPass:
                output = processStage (
                    first.high, state[1], stageCoefficients[1]).high;
                break;
            default:
                break;
        }

        if (! std::isfinite (output))
        {
            resetSignalState();
            return 0.0f;
        }

        return juce::jlimit (-8.0f, 8.0f, output);
    }

    std::array<std::array<State, 2>, 2> states {};
    std::array<Coefficients, 2> stageCoefficients {};
    std::array<float, 2> brightLow {};
    juce::ADSR envelope;
    double sampleRate = 44100.0;
    float brightCoefficient = 0.0f;
    float brightDynamics = 0.0f;
    float lastResonance = -1.0f;
    float lastCutoffHz = -1.0f;
    float lastEnvelopeAmount = 2.0f;
    float lastEnvelopeValue = 2.0f;
    int lastType = -1;
    int lastKeyTrack = -1;
    int lastMidiNote = -1;
    int lastCoefficientVelocity = -1;
    int lastBrightVelocity = -1;
    bool lastVelocitySensitivity = false;
    int coefficientCountdown = 0;
};

struct AmpEqSettings
{
    bool enabled = false;
    float bassDb = 0.0f;
    float midDb = 0.0f;
    float midHz = 850.0f;
    float trebleDb = 0.0f;
};

// EQ de amp dedicado ao pad: Bass 100 Hz, Mid varrivel 200 Hz..8 kHz e
// Treble 4 kHz, todos +/-15 dB conforme o painel documentado.
class AmpEq
{
public:
    void prepare (double newSampleRate, int maximumBlockSize,
                  AmpEqSettings initial = {})
    {
        sampleRate = juce::jmax (8000.0, newSampleRate);
        const juce::dsp::ProcessSpec spec {
            sampleRate,
            (juce::uint32) juce::jmax (1, maximumBlockSize),
            1
        };

        bassSmooth.reset (sampleRate, 0.045);
        midSmooth.reset (sampleRate, 0.045);
        midHzSmooth.reset (sampleRate, 0.045);
        trebleSmooth.reset (sampleRate, 0.045);
        syncToTargets (initial);

        // Instala biquads de ordem 2 antes do prepare para nunca realocar no
        // callback quando o cliente liga o EQ pela primeira vez.
        updateCoefficients (initial, true);
        for (int channel = 0; channel < 2; ++channel)
            for (auto& band : filters[channel])
                band.prepare (spec);
    }

    void reset() noexcept
    {
        for (int channel = 0; channel < 2; ++channel)
            for (auto& band : filters[channel])
                band.reset();
    }

    void syncToTargets (AmpEqSettings settings)
    {
        settings = boundedEq (settings);
        bassSmooth.setCurrentAndTargetValue (settings.bassDb);
        midSmooth.setCurrentAndTargetValue (settings.midDb);
        midHzSmooth.setCurrentAndTargetValue (settings.midHz);
        trebleSmooth.setCurrentAndTargetValue (settings.trebleDb);
        updateCoefficients (settings, true);
        reset();
    }

    void advanceSilently (int numSamples, AmpEqSettings settings)
    {
        if (numSamples <= 0)
            return;
        settings = boundedEq (settings);
        setTargets (settings);
        settings.bassDb = bassSmooth.skip (numSamples);
        settings.midDb = midSmooth.skip (numSamples);
        settings.midHz = midHzSmooth.skip (numSamples);
        settings.trebleDb = trebleSmooth.skip (numSamples);
        updateCoefficients (settings, false);
    }

    void process (juce::AudioBuffer<float>& buffer, int numSamples,
                  AmpEqSettings settings)
    {
        if (numSamples <= 0 || buffer.getNumChannels() <= 0)
            return;

        settings = boundedEq (settings);
        setTargets (settings);
        const int channels = juce::jmin (2, buffer.getNumChannels());
        int offset = 0;

        while (offset < numSamples)
        {
            const bool smoothing = bassSmooth.isSmoothing()
                                || midSmooth.isSmoothing()
                                || midHzSmooth.isSmoothing()
                                || trebleSmooth.isSmoothing();
            const int chunk = juce::jmin (
                numSamples - offset, smoothing ? 16 : numSamples - offset);
            AmpEqSettings current = settings;
            current.bassDb = bassSmooth.skip (chunk);
            current.midDb = midSmooth.skip (chunk);
            current.midHz = midHzSmooth.skip (chunk);
            current.trebleDb = trebleSmooth.skip (chunk);
            updateCoefficients (current, false);

            if (active)
                for (int channel = 0; channel < channels; ++channel)
                {
                    auto* samples = buffer.getWritePointer (channel) + offset;
                    for (int band = 0; band < 3; ++band)
                        for (int sample = 0; sample < chunk; ++sample)
                            samples[sample] = filters[channel][band]
                                .processSample (samples[sample]);
                }
            offset += chunk;
        }
    }

private:
    static AmpEqSettings boundedEq (AmpEqSettings value) noexcept
    {
        const auto finiteOr = [] (float candidate, float fallback) noexcept
        {
            return std::isfinite (candidate) ? candidate : fallback;
        };
        value.bassDb = juce::jlimit (
            -15.0f, 15.0f, finiteOr (value.bassDb, 0.0f));
        value.midDb = juce::jlimit (
            -15.0f, 15.0f, finiteOr (value.midDb, 0.0f));
        value.midHz = juce::jlimit (
            200.0f, 8000.0f, finiteOr (value.midHz, 850.0f));
        value.trebleDb = juce::jlimit (
            -15.0f, 15.0f, finiteOr (value.trebleDb, 0.0f));
        return value;
    }

    void setTargets (const AmpEqSettings& settings)
    {
        bassSmooth.setTargetValue (settings.bassDb);
        midSmooth.setTargetValue (settings.midDb);
        midHzSmooth.setTargetValue (settings.midHz);
        trebleSmooth.setTargetValue (settings.trebleDb);
    }

    void updateCoefficients (AmpEqSettings settings, bool force)
    {
        settings = boundedEq (settings);
        const bool shouldBeActive = settings.enabled
            && (std::abs (settings.bassDb) >= 0.005f
                 || std::abs (settings.midDb) >= 0.005f
                 || std::abs (settings.trebleDb) >= 0.005f);

        if (! force
             && settings.enabled == lastEnabled
             && std::abs (settings.bassDb - last.bassDb) < 0.005f
             && std::abs (settings.midDb - last.midDb) < 0.005f
             && std::abs (settings.midHz - last.midHz) < 0.5f
             && std::abs (settings.trebleDb - last.trebleDb) < 0.005f)
            return;

        last = settings;
        lastEnabled = settings.enabled;

        if (! shouldBeActive && ! force)
        {
            if (active)
                reset();
            active = false;
            return;
        }

        using Coefficients = juce::dsp::IIR::ArrayCoefficients<float>;
        const auto gain = [] (float db)
        {
            return juce::Decibels::decibelsToGain (db);
        };
        const float bassHz = (float) juce::jmin (100.0, sampleRate * 0.08);
        const float midHz = (float) juce::jmin (
            (double) settings.midHz, sampleRate * 0.40);
        const float trebleHz = (float) juce::jmin (4000.0, sampleRate * 0.30);
        const std::array<float, 6> coefficients[] = {
            Coefficients::makeLowShelf (
                sampleRate, bassHz, 0.72f, gain (settings.bassDb)),
            Coefficients::makePeakFilter (
                sampleRate, midHz, 0.85f, gain (settings.midDb)),
            Coefficients::makeHighShelf (
                sampleRate, trebleHz, 0.72f, gain (settings.trebleDb))
        };
        for (int band = 0; band < 3; ++band)
            for (int channel = 0; channel < 2; ++channel)
                *filters[channel][band].coefficients = coefficients[band];
        active = shouldBeActive;
    }

    juce::dsp::IIR::Filter<float> filters[2][3];
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bassSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midHzSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> trebleSmooth;
    AmpEqSettings last {};
    double sampleRate = 44100.0;
    bool lastEnabled = false;
    bool active = false;
};
}
