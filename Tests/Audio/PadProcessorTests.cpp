#include "PadProcessor.h"

#include <cmath>
#include <iostream>

namespace
{
bool perto (float a, float b, float tolerancia = 0.0001f)
{
    return std::abs (a - b) <= tolerancia;
}

int falha (const char* mensagem)
{
    std::cerr << "PAD_PROCESSOR_FAIL: " << mensagem << '\n';
    return 1;
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto pastaTeste = juce::File::getSpecialLocation (
        juce::File::tempDirectory).getNonexistentChildFile (
            "ChurchDAW-PadProcessorTests", {}, false);

    if (pastaTeste.createDirectory().failed())
        return falha ("nao foi possivel criar a pasta temporaria");

    pastaTeste.getChildFile ("pad_C.wav").create();

    if (PadProcessor::isCompletePadFolder (pastaTeste))
        return falha ("pack parcial foi aceito como completo");

    for (const char note : { 'A', 'B', 'D', 'E', 'F', 'G' })
        if (pastaTeste.getChildFile (
                "pad_" + juce::String::charToString (note) + ".wav")
                .create().failed())
            return falha ("nao foi possivel montar a fixture A..G");

    if (! PadProcessor::isCompletePadFolder (pastaTeste))
        return falha ("conjunto A..G completo nao foi reconhecido");

    if (! pastaTeste.deleteRecursively())
        return falha ("nao foi possivel limpar a pasta temporaria");

    PadProcessor pad;

    if (pad.getParameters().size() != 2)
        return falha ("cutoff e volume precisam estar expostos ao MIDI Learn");

    if (! perto (pad.getGain(), 0.9f) || ! perto (pad.getCutoff(), 1.0f))
        return falha ("defaults de volume/corte mudaram");

    // AudioProcessorParameter::setValueNotifyingHost recebe 0..1 normalizado.
    // Este gate pega a regressao em que 1,5 era enviado diretamente e virava
    // 1,0 no parametro, inutilizando o terco superior do knob.
    pad.setGain (1.5f);

    if (! perto (pad.getGain(), 1.5f)
         || ! perto (pad.getParameters()[1]->getValue(), 1.0f))
        return falha ("volume de 150% nao sobrevive a normalizacao");

    pad.setCutoff (0.37f);
    pad.setKey (7);

    juce::MemoryBlock estado;
    pad.getStateInformation (estado);

    if (estado.isEmpty())
        return falha ("estado do Pad ficou vazio");

    PadProcessor restaurado;
    restaurado.setStateInformation (estado.getData(), (int) estado.getSize());

    if (! perto (restaurado.getGain(), 1.5f)
         || ! perto (restaurado.getCutoff(), 0.37f))
        return falha ("volume/corte nao sobreviveram ao round-trip");

    // Uma tecla ativa e estado de execucao, nao configuracao. Restaurar o tom
    // faria o programa iniciar tocando sozinho depois de um restart.
    if (restaurado.getKey() != -1)
        return falha ("estado restaurado reativou o pad sozinho");

    // Em 32 kHz, 19,2 kHz fica acima de Nyquist. O corte aberto deve sair do
    // caminho e a transicao para fechado nunca pode produzir NaN/inf.
    restaurado.setCutoff (1.0f);
    restaurado.prepareToPlay (32000.0, 256);

    juce::AudioBuffer<float> bloco (2, 256);
    for (int canal = 0; canal < bloco.getNumChannels(); ++canal)
        for (int i = 0; i < bloco.getNumSamples(); ++i)
            bloco.setSample (canal, i, (i & 1) == 0 ? 0.25f : -0.25f);

    const auto copia = bloco;
    restaurado.aplicarCutoff (bloco.getWritePointer (0),
                              bloco.getWritePointer (1),
                              bloco.getNumSamples());

    for (int canal = 0; canal < bloco.getNumChannels(); ++canal)
        for (int i = 0; i < bloco.getNumSamples(); ++i)
            if (! perto (bloco.getSample (canal, i), copia.getSample (canal, i)))
                return falha ("cutoff aberto nao entrou em bypass a 32 kHz");

    restaurado.setCutoff (0.0f);

    for (int rodada = 0; rodada < 64; ++rodada)
    {
        bloco.clear();
        bloco.addSample (0, 0, 1.0f);
        bloco.addSample (1, 0, 1.0f);
        restaurado.aplicarCutoff (bloco.getWritePointer (0),
                                  bloco.getWritePointer (1),
                                  bloco.getNumSamples());

        for (int canal = 0; canal < bloco.getNumChannels(); ++canal)
            for (int i = 0; i < bloco.getNumSamples(); ++i)
            {
                const auto amostra = bloco.getSample (canal, i);

                if (! std::isfinite (amostra) || std::abs (amostra) > 8.0f)
                    return falha ("filtro ficou instavel a 32 kHz");
            }
    }

    std::cout << "PAD_PROCESSOR_OK\n";
    return 0;
}
