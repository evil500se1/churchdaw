#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>

namespace ChurchDAWChannelTone
{
struct Defaults
{
    float attackSeconds = 0.003f;
    float releaseSeconds = 0.55f;
    float lowDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;
};

inline Defaults defaultsFor (const juce::String& rawLabel,
                             const juce::String& rawCategory = {})
{
    // O primeiro release precisa soar EXATAMENTE como a versao anterior:
    // attack de 3 ms, release central de 550 ms e EQ neutro. Alterar pads,
    // pianos ou setups antigos por baixo do cliente seria uma regressao.
    //
    // O label/categoria permanecem na API porque packs futuros poderao publicar
    // defaults explicitamente versionados; hoje eles nunca mudam o som legado.
    juce::ignoreUnused (rawLabel, rawCategory);
    return {};
}

// "Zero" absoluto pode começar um sample fora do zero-crossing em amplitude
// cheia e produzir um clique. Um milissegundo continua soando instantâneo, mas
// garante uma micro-rampa segura em qualquer sample rate/plataforma.
inline float attackForPlayback (float requestedSeconds) noexcept
{
    return std::isfinite (requestedSeconds)
             ? juce::jlimit (0.001f, 2.0f, requestedSeconds)
             : 0.003f;
}

// Mantem o comportamento acustico que o piano ja tinha: as notas graves
// liberam mais devagar e as agudas mais rapido. O knob RELEASE controla o
// centro desse desenho, em segundos, em vez de destruir a calibragem por nota.
inline float releaseForNote (float baseSeconds, int midiNote) noexcept
{
    const float bounded = juce::jlimit (0.05f, 12.0f, baseSeconds);
    const float noteScale = juce::jmap (
        (float) juce::jlimit (21, 108, midiNote),
        21.0f, 108.0f, 1.36363636f, 0.50909091f);
    return juce::jlimit (0.03f, 15.0f, bounded * noteScale);
}

// EQ musical por canal. LOW e HIGH sao shelves; MID e um bell largo para
// corpo/clareza. Os parametros sao suavizados antes de recalcular os
// coeficientes, evitando zipper noise ao girar os knobs durante uma nota.
class ThreeBandEq
{
public:
    void prepare (double newSampleRate, int maximumBlockSize,
                  float initialLowDb = 0.0f,
                  float initialMidDb = 0.0f,
                  float initialHighDb = 0.0f)
    {
        sampleRate = juce::jmax (8000.0, newSampleRate);
        const juce::dsp::ProcessSpec spec {
            sampleRate,
            (juce::uint32) juce::jmax (1, maximumBlockSize),
            1
        };

        lowSmooth.reset (sampleRate, 0.045);
        midSmooth.reset (sampleRate, 0.045);
        highSmooth.reset (sampleRate, 0.045);
        lowSmooth.setCurrentAndTargetValue (boundedDb (initialLowDb));
        midSmooth.setCurrentAndTargetValue (boundedDb (initialMidDb));
        highSmooth.setCurrentAndTargetValue (boundedDb (initialHighDb));

        // Estabelece BIQUADS (ordem 2) ANTES de prepare. O coeficiente default
        // do JUCE e ordem 1; trocar para ordem 2 na primeira mexida fazia
        // IIR::Filter::check() realocar memoria dentro do callback de audio.
        // Mesmo em 0 dB calculamos os tres biquads-unidade, mas active=false
        // continua garantindo bypass bit-transparente e custo zero.
        active = false;
        updateCoefficients (initialLowDb, initialMidDb, initialHighDb, true);

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

    void process (juce::AudioBuffer<float>& buffer, int numSamples,
                  float targetLowDb, float targetMidDb, float targetHighDb)
    {
        if (numSamples <= 0 || buffer.getNumChannels() <= 0)
            return;

        lowSmooth.setTargetValue (boundedDb (targetLowDb));
        midSmooth.setTargetValue (boundedDb (targetMidDb));
        highSmooth.setTargetValue (boundedDb (targetHighDb));

        const int channels = juce::jmin (2, buffer.getNumChannels());
        int offset = 0;

        // Durante automação, atualizar uma vez por bloco fazia o passo depender
        // do buffer: em 512 samples o ganho pulava quase 3 dB de uma vez. Em
        // sub-blocos de 16 mantêm o residual de automação abaixo de -50 dBFS
        // inclusive no HIGH (medido em 44,1/48/96 kHz);
        // quando os três valores estabilizam voltamos a um único bloco, sem
        // aumentar o custo normal do mixer.
        while (offset < numSamples)
        {
            const bool smoothing = lowSmooth.isSmoothing()
                                || midSmooth.isSmoothing()
                                || highSmooth.isSmoothing();
            const int chunk = juce::jmin (
                numSamples - offset, smoothing ? 16 : numSamples - offset);
            const float low = lowSmooth.skip (chunk);
            const float mid = midSmooth.skip (chunk);
            const float high = highSmooth.skip (chunk);
            updateCoefficients (low, mid, high, false);

            if (active)
                for (int channel = 0; channel < channels; ++channel)
                {
                    auto* samples = buffer.getWritePointer (channel) + offset;

                    for (int band = 0; band < 3; ++band)
                        for (int sample = 0; sample < chunk; ++sample)
                            samples[sample] =
                                filters[channel][band].processSample (
                                    samples[sample]);
                }

            offset += chunk;
        }
    }

    // Canal sem voz nao processa buffer, mas o tempo dos parametros continua
    // passando. Sem isto, girar o EQ no silencio fazia a proxima nota nascer
    // com o valor antigo e "morphar" durante os primeiros 45 ms.
    void advanceSilently (int numSamples,
                          float targetLowDb,
                          float targetMidDb,
                          float targetHighDb)
    {
        if (numSamples <= 0)
            return;

        lowSmooth.setTargetValue (boundedDb (targetLowDb));
        midSmooth.setTargetValue (boundedDb (targetMidDb));
        highSmooth.setTargetValue (boundedDb (targetHighDb));
        updateCoefficients (
            lowSmooth.skip (numSamples),
            midSmooth.skip (numSamples),
            highSmooth.skip (numSamples),
            false);
    }

    // Trocar de timbre e uma fronteira sonora, nao automacao do mesmo timbre.
    // Sincroniza o perfil sob suspensao para que a primeira nota do novo som ja
    // use seu EQ, sem herdar 45 ms do preset anterior.
    void syncToTargets (float lowDb, float midDb, float highDb)
    {
        lowDb = boundedDb (lowDb);
        midDb = boundedDb (midDb);
        highDb = boundedDb (highDb);
        lowSmooth.setCurrentAndTargetValue (lowDb);
        midSmooth.setCurrentAndTargetValue (midDb);
        highSmooth.setCurrentAndTargetValue (highDb);
        updateCoefficients (lowDb, midDb, highDb, true);
        reset();
    }

private:
    static float boundedDb (float value) noexcept
    {
        return std::isfinite (value) ? juce::jlimit (-12.0f, 12.0f, value)
                                     : 0.0f;
    }

    void updateCoefficients (float lowDb, float midDb, float highDb,
                             bool force)
    {
        lowDb = boundedDb (lowDb);
        midDb = boundedDb (midDb);
        highDb = boundedDb (highDb);

        if (! force
             && std::abs (lowDb - lastLowDb) < 0.005f
             && std::abs (midDb - lastMidDb) < 0.005f
             && std::abs (highDb - lastHighDb) < 0.005f)
            return;

        lastLowDb = lowDb;
        lastMidDb = midDb;
        lastHighDb = highDb;

        const bool shouldBeActive = std::abs (lowDb) >= 0.005f
                                 || std::abs (midDb) >= 0.005f
                                 || std::abs (highDb) >= 0.005f;

        if (! shouldBeActive && ! force)
        {
            if (active)
                reset();

            active = false;
            return;
        }

        using ArrayCoefficients = juce::dsp::IIR::ArrayCoefficients<float>;
        const auto gain = [] (float db)
        {
            return juce::Decibels::decibelsToGain (db);
        };

        const float lowHz = (float) juce::jmin (120.0, sampleRate * 0.08);
        const float midHz = (float) juce::jmin (850.0, sampleRate * 0.18);
        const float highHz = (float) juce::jmin (8000.0, sampleRate * 0.38);
        // ArrayCoefficients calcula tudo na pilha. Os objetos de estado dos
        // filtros ja foram preparados fora do callback e recebem somente os
        // seis valores novos; assim girar o EQ nao cria Ptr/ref-counted objects
        // na thread de audio.
        const std::array<float, 6> coefficients[] = {
            ArrayCoefficients::makeLowShelf (sampleRate, lowHz, 0.72f, gain (lowDb)),
            ArrayCoefficients::makePeakFilter (sampleRate, midHz, 0.85f, gain (midDb)),
            ArrayCoefficients::makeHighShelf (sampleRate, highHz, 0.72f, gain (highDb))
        };

        for (int band = 0; band < 3; ++band)
            for (int channel = 0; channel < 2; ++channel)
                *filters[channel][band].coefficients = coefficients[band];

        active = shouldBeActive;
    }

    juce::dsp::IIR::Filter<float> filters[2][3];
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midSmooth;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highSmooth;
    double sampleRate = 44100.0;
    float lastLowDb = 1000.0f;
    float lastMidDb = 1000.0f;
    float lastHighDb = 1000.0f;
    bool active = false;
};
}
