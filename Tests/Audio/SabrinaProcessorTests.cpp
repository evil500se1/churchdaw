//==============================================================================
// Testes offline do processador Sabrina (saturador tanh nativo do ChurchDAW).
//
// Regras que este arquivo protege:
//   1. Seguranca de boot: o efeito NASCE em bypass. Nada entra no caminho de
//      audio sem quem ligue explicitamente.
//   2. Bypass de verdade: em bypass a saida e BIT-EXACT igual a entrada e o
//      contador de blocos processados fica em zero (nao ha custo algum).
//   3. Ligado, ele realmente processa: muda o sinal, nao gera NaN/Inf, cria
//      harmonicos e conta os blocos efetivamente processados.
//   4. O drive e limitado a [minDrive, maxDrive] para nunca virar distorcao
//      de guitarra.
//==============================================================================
#include "Plugins/Sabrina/SabrinaProcessor.h"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;
constexpr int blockCount = 24;
constexpr double toneFrequency = 1000.0;
constexpr double totalSamples = (double) blockSize * (double) blockCount;
constexpr float amplitude = 0.5f;

// Seno 0.5 @1 kHz. Com 6144 amostras a 48 kHz cabem exatamente 128 periodos
// de 1 kHz (e 256 de 2 kHz, 384 de 3 kHz), entao o Goertzel abaixo nao sofre
// vazamento espectral e qualquer harmonico medido e do processador, nao da
// janela.
std::vector<float> makeTone()
{
    std::vector<float> tone ((size_t) totalSamples);

    for (int sample = 0; sample < (int) totalSamples; ++sample)
        tone[(size_t) sample] = amplitude
            * (float) std::sin (
                juce::MathConstants<double>::twoPi * toneFrequency
                * (double) sample / sampleRate);

    return tone;
}

struct Render
{
    std::vector<float> input;
    std::vector<float> output;
    int processedBlocks = 0;
    bool finite = true;
    float peak = 0.0f;
    double rms = 0.0;
    double maximumDifference = 0.0;
    int bitExactMismatches = 0;
};

Render render (bool bypassed, float drive)
{
    const auto tone = makeTone();
    SabrinaProcessor processor;

    // O drive vem ANTES do prepareToPlay: assim a rampa interna ja nasce no
    // alvo e o bloco 1 nao contem transiente de parametro.
    processor.setDrive (drive);
    processor.setSabrinaBypassed (bypassed);
    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> block (2, blockSize);
    juce::MidiBuffer midi;
    Render result;
    result.input = tone;
    result.output.assign (tone.size(), 0.0f);
    long double energy = 0.0L;

    for (int index = 0; index < blockCount; ++index)
    {
        const int start = index * blockSize;

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = tone[(size_t) (start + sample)];
            block.setSample (0, sample, value);
            block.setSample (1, sample, value);
        }

        processor.processBlock (block, midi);

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = block.getSample (0, sample);
            const float original = tone[(size_t) (start + sample)];
            result.output[(size_t) (start + sample)] = value;
            result.finite = result.finite && std::isfinite (value);
            result.peak = juce::jmax (result.peak, std::abs (value));
            result.maximumDifference = juce::jmax (
                result.maximumDifference,
                (double) std::abs (value - original));

            // Comparacao BIT-EXACT, sem epsilon: em bypass nao pode haver nem
            // uma unidade de arredondamento de diferenca.
            if (! (value == original))
                ++result.bitExactMismatches;

            energy += (long double) value * (long double) value;
        }
    }

    result.rms = std::sqrt (
        (double) (energy / (long double) result.output.size()));
    result.processedBlocks = processor.getProcessedBlockCount();
    return result;
}

double harmonicAmplitude (const std::vector<float>& samples,
                          double frequency)
{
    const double omega =
        juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const double coefficient = 2.0 * std::cos (omega);
    double previous = 0.0;
    double previousPrevious = 0.0;

    for (const float sample : samples)
    {
        const double current =
            (double) sample + coefficient * previous - previousPrevious;
        previousPrevious = previous;
        previous = current;
    }

    const double real =
        previous - previousPrevious * std::cos (omega);
    const double imaginary = previousPrevious * std::sin (omega);
    return 2.0 * std::sqrt (real * real + imaginary * imaginary)
           / (double) samples.size();
}

// (a) Seguranca: bypass nasce LIGADO.
bool testBypassBornEngaged()
{
    SabrinaProcessor processor;
    const bool passed =
           processor.isSabrinaBypassed()
        && processor.getProcessedBlockCount() == 0
        && processor.getTailLengthSeconds() == 0.0
        && ! processor.acceptsMidi()
        && ! processor.producesMidi();

    std::cout << "Sabrina born bypassed: bypassed="
              << (processor.isSabrinaBypassed() ? "true" : "false")
              << ", blocks=" << processor.getProcessedBlockCount()
              << ", tail=" << processor.getTailLengthSeconds()
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

// (b) Em bypass: saida BIT-EXACT igual a entrada e zero blocos processados.
bool testBypassIsBitExactAndFree()
{
    const auto rendered = render (true, 2.0f);
    const bool passed =
           rendered.finite
        && rendered.bitExactMismatches == 0
        && rendered.maximumDifference == 0.0
        && rendered.processedBlocks == 0;

    std::cout << "Sabrina bypass bit-exact: mismatches="
              << rendered.bitExactMismatches
              << ", max|out-in|=" << rendered.maximumDifference
              << ", blocks=" << rendered.processedBlocks
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

// (c) Ligado (drive 2.0): muda o sinal, nao gera NaN/Inf, cria harmonicos e
// conta os blocos efetivamente processados.
bool testEngagedSaturatesAndCounts()
{
    const auto bypassed = render (true, 2.0f);
    const auto engaged = render (false, 2.0f);

    const double inputFundamental =
        harmonicAmplitude (bypassed.output, 1000.0);
    const double inputSecond = harmonicAmplitude (bypassed.output, 2000.0);
    const double inputThird = harmonicAmplitude (bypassed.output, 3000.0);
    const double outputFundamental =
        harmonicAmplitude (engaged.output, 1000.0);
    const double outputSecond = harmonicAmplitude (engaged.output, 2000.0);
    const double outputThird = harmonicAmplitude (engaged.output, 3000.0);
    const double rmsDeltaDb =
        20.0 * std::log10 (engaged.rms / bypassed.rms);

    const bool passed =
           engaged.finite
        && engaged.maximumDifference > 1.0e-3
        && std::abs (engaged.rms - bypassed.rms) > 1.0e-3
        && std::abs (rmsDeltaDb) > 0.1
        && engaged.processedBlocks == blockCount
        && engaged.peak < 1.0f
        && inputSecond < 1.0e-5
        && inputThird < 1.0e-5
        && outputThird > 1.0e-3;

    std::cout << "Sabrina engaged: max|out-in|="
              << engaged.maximumDifference
              << ", rms " << bypassed.rms << " -> " << engaged.rms
              << " (" << rmsDeltaDb << " dB)"
              << ", peak=" << engaged.peak
              << ", blocks=" << engaged.processedBlocks
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    std::cout << "Sabrina harmonics: in[1k=" << inputFundamental
              << " 2k=" << inputSecond << " 3k=" << inputThird
              << "] out[1k=" << outputFundamental
              << " 2k=" << outputSecond << " 3k=" << outputThird
              << "] -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

// (d) O drive e limitado a [minDrive, maxDrive].
bool testDriveIsClamped()
{
    SabrinaProcessor processor;
    processor.setDrive (99.0f);
    const float high = processor.getDrive();
    processor.setDrive (-1.0f);
    const float low = processor.getDrive();
    processor.setDrive (2.5f);
    const float middle = processor.getDrive();

    const bool passed =
           high == SabrinaProcessor::maxDrive
        && low == SabrinaProcessor::minDrive
        && middle == 2.5f
        && SabrinaProcessor::minDrive == 1.0f
        && SabrinaProcessor::maxDrive == 4.0f;

    std::cout << "Sabrina drive clamp: setDrive(99)=" << high
              << ", setDrive(-1)=" << low
              << ", setDrive(2.5)=" << middle
              << " -> " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}
} // namespace

int main()
{
    juce::ScopedNoDenormals noDenormals;
    std::cout << std::fixed << std::setprecision (6);
    int failures = 0;

    if (! testBypassBornEngaged())
        ++failures;
    if (! testBypassIsBitExactAndFree())
        ++failures;
    if (! testEngagedSaturatesAndCounts())
        ++failures;
    if (! testDriveIsClamped())
        ++failures;

    if (failures == 0)
    {
        std::cout << "SABRINA_TESTS_OK\n";
        return 0;
    }

    std::cerr << "SABRINA_TESTS_FAILED=" << failures << '\n';
    return 1;
}
