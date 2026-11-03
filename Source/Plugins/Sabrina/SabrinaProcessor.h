//==============================================================================
// Sabrina: saturacao/sculpt NATIVA do ChurchDAW.
//
// Historia: existiu uma versao VST2 (Plugins/Sabrina/bin/win_x64/Sabrina.dll)
// que nunca chegou a funcionar -- LoadLibrary falhava com 1114 (DllMain do
// TDM-GCC 4.9.2 com pseudo-reloc), o host chamava carga de DLL na thread de
// audio e o binario nem estava no git (gitignored). O codigo foi removido na
// 1.6.2. Esta versao entrega o mesmo papel sem dependencia externa: um
// saturador suave que da corpo ao som, com bypass que nao custa nada.
#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

class SabrinaProcessor : public juce::AudioProcessor
{
public:
    SabrinaProcessor();

    // Politica de seguranca: nasce em BYPASS. Um efeito que entra ligado pode
    // travar a abertura do app em igreja; quem quiser som, liga.
    void setSabrinaBypassed (bool shouldBeBypassed) noexcept;
    bool isSabrinaBypassed() const noexcept;

    // Quantos blocos foram efetivamente processados. Em bypass tem que ficar
    // em zero -- e assim o teste prova que nao ha processamento algum.
    int getProcessedBlockCount() const noexcept;

    // Quanta saturacao. 1.0 = quase linear; acima disso engrossa e gera
    // harmonicos. Limite baixo para nunca virar distorcao de guitarra.
    static constexpr float minDrive = 1.0f;
    static constexpr float maxDrive = 4.0f;
    void setDrive (float newDrive) noexcept;
    float getDrive() const noexcept;

    void prepareToPlay (double sampleRate,
                        int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    const juce::String getName() const override { return "Sabrina"; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    std::atomic<bool> bypassed { true };
    std::atomic<float> drive { 1.8f };
    std::atomic<int> processedBlocks { 0 };

    // Rampa do parametro por bloco: evita estalo quando o drive muda.
    float currentDrive = 1.8f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SabrinaProcessor)
};
