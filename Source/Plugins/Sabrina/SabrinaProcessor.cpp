#include "SabrinaProcessor.h"

SabrinaProcessor::SabrinaProcessor()
    : juce::AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void SabrinaProcessor::setSabrinaBypassed (bool shouldBeBypassed) noexcept
{
    bypassed.store (shouldBeBypassed, std::memory_order_release);
}

bool SabrinaProcessor::isSabrinaBypassed() const noexcept
{
    return bypassed.load (std::memory_order_acquire);
}

int SabrinaProcessor::getProcessedBlockCount() const noexcept
{
    return processedBlocks.load (std::memory_order_acquire);
}

void SabrinaProcessor::setDrive (float newDrive) noexcept
{
    drive.store (juce::jlimit (minDrive, maxDrive, newDrive),
                 std::memory_order_release);
}

float SabrinaProcessor::getDrive() const noexcept
{
    return drive.load (std::memory_order_acquire);
}

void SabrinaProcessor::prepareToPlay (double, int)
{
    currentDrive = drive.load (std::memory_order_acquire);
}

void SabrinaProcessor::releaseResources()
{
}

void SabrinaProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer&)
{
    // Bypass de verdade: nao toca no buffer e nao conta processamento. Assim a
    // saida e bit-exact igual a entrada (o teste compara com == 0.0f).
    if (bypassed.load (std::memory_order_acquire))
        return;

    const auto alvo = drive.load (std::memory_order_acquire);
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0)
        return;

    // Normalizador: mantem a saida em escala parecida com a entrada, para o
    // volume nao subir quando o drive aumenta.
    const float norm = 1.0f / std::tanh (alvo);
    const float passo = numSamples > 0
                            ? (alvo - currentDrive) / (float) numSamples
                            : 0.0f;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* dados = buffer.getWritePointer (ch);
        float d = currentDrive;

        for (int i = 0; i < numSamples; ++i)
        {
            d += passo;
            dados[i] = std::tanh (dados[i] * d) * norm;
        }
    }

    currentDrive = alvo;
    processedBlocks.fetch_add (1, std::memory_order_acq_rel);
}
