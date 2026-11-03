#include "PadSynthProcessor.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
int falha (const char* mensagem)
{
    std::cerr << "PAD_SYNTH_FAIL: " << mensagem << '\n';
    return 1;
}

void definir (juce::RangedAudioParameter* parametro, float valorReal)
{
    parametro->setValueNotifyingHost (parametro->convertTo0to1 (valorReal));
}

bool blocoFinito (const juce::AudioBuffer<float>& buffer)
{
    for (int canal = 0; canal < buffer.getNumChannels(); ++canal)
        for (int amostra = 0; amostra < buffer.getNumSamples(); ++amostra)
            if (! std::isfinite (buffer.getSample (canal, amostra)))
                return false;
    return true;
}

float picoDoBloco (const juce::AudioBuffer<float>& buffer)
{
    float pico = 0.0f;
    for (int canal = 0; canal < buffer.getNumChannels(); ++canal)
        for (int amostra = 0; amostra < buffer.getNumSamples(); ++amostra)
            pico = juce::jmax (pico,
                               std::abs (buffer.getSample (canal, amostra)));
    return pico;
}

bool quaseIgual (float a, float b, float tolerancia = 0.0011f)
{
    return std::abs (a - b) <= tolerancia;
}
}

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    // O motor nasce desligado: atualizar o app nunca pode somar um pad a um
    // setup existente sem uma escolha explicita do musico.
    PadSynthProcessor desligado;
    if (desligado.isEngineEnabled()
         || desligado.getNumPrograms() != PadSynthProcessor::numPrograms
         || desligado.getProgramName (0).isEmpty())
        return falha ("estado inicial seguro/banco de programas invalido");

    static constexpr std::array<const char*, PadSynthProcessor::numPrograms>
        nomesDeFabrica {
            "Warm Pad", "Heaven Pad", "Soft Pad", "Ambience Heaven",
            "Deep Atmos", "Cloud Pad", "Dark Pad", "Bright Ambience",
            "Ethereal", "Ocean Pad", "Air Pad", "Choir Pad",
            "Drone Pad", "Shimmer", "Space Pad", "Infinity"
        };
    for (int programa = 0; programa < PadSynthProcessor::numPrograms; ++programa)
        if (desligado.getProgramName (programa) != nomesDeFabrica[(size_t) programa])
            return falha ("nomes do banco de 16 pads divergiram do contrato da UI");

    desligado.prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
    desligado.processBlock (buffer, midi);
    if (! blocoFinito (buffer) || picoDoBloco (buffer) > 0.000001f)
        return falha ("motor desligado respondeu a MIDI");

    PadSynthProcessor pad;
    definir (pad.fadeIn, 0.01f);
    definir (pad.fadeOut, 0.01f);
    definir (pad.chorusMix, 0.0f);
    definir (pad.reverbMix, 0.0f);
    definir (pad.delayMix, 0.0f);
    definir (pad.layer (0).attack, 0.003f);
    definir (pad.layer (1).attack, 0.003f);
    definir (pad.layer (0).release, 0.04f);
    definir (pad.layer (1).release, 0.04f);
    pad.prepareToPlay (sampleRate, blockSize);
    pad.setEngineEnabled (true);

    float maiorPico = 0.0f;
    for (int bloco = 0; bloco < 16; ++bloco)
    {
        juce::MidiBuffer eventos;
        if (bloco == 0)
            eventos.addEvent (
                juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        pad.processBlock (buffer, eventos);
        if (! blocoFinito (buffer))
            return falha ("DSP produziu NaN/Inf");
        maiorPico = juce::jmax (maiorPico, picoDoBloco (buffer));
    }

    if (maiorPico < 0.0001f || pad.readAndResetOutputPeak() < 0.0001f)
        return falha ("note-on nao produziu audio real");

    // PANIC e executado no limite do bloco de audio e precisa remover vozes
    // imediatamente, sem deixar nota presa.
    pad.requestPanic();
    juce::MidiBuffer semEventos;
    pad.processBlock (buffer, semEventos);
    if (! blocoFinito (buffer) || picoDoBloco (buffer) > 0.000001f)
        return falha ("PANIC nao encerrou todas as vozes");

    // Exercita os controles adicionados pelo editor premium no caminho de
    // audio: SUB quadrado, noise digital, band-pass, drive, envelope de
    // modulacao, LFO sample/hold com fase e gate. Nao basta o attachment
    // existir na tela; a combinacao precisa renderizar audio finito de verdade.
    PadSynthProcessor ramosDsp;
    definir (ramosDsp.fadeIn, 0.01f);
    definir (ramosDsp.fadeOut, 0.01f);
    definir (ramosDsp.masterDb, -12.0f);
    definir (ramosDsp.chorusMix, 0.24f);
    definir (ramosDsp.reverbMix, 0.18f);
    definir (ramosDsp.delayMix, 0.16f);
    definir (ramosDsp.delayTime, 0.08f);
    definir (ramosDsp.delayFeedback, 0.20f);
    definir (ramosDsp.gateEnabled, 1.0f);
    definir (ramosDsp.gateRate, 3.0f);
    definir (ramosDsp.gateDepth, 0.72f);
    definir (ramosDsp.holdMode, 0.0f);
    definir (ramosDsp.toggleMode, 0.0f);

    for (int camada = 0; camada < PadSynthProcessor::numLayers; ++camada)
        definir (ramosDsp.layer (camada).enabled, camada == 0 ? 1.0f : 0.0f);

    auto& camada = ramosDsp.layer (0);
    definir (camada.osc1Wave, 4.0f);
    definir (camada.osc2Wave, 3.0f);
    definir (camada.subWave, 2.0f);
    definir (camada.noiseType, 2.0f);
    definir (camada.filterType, 3.0f);
    definir (camada.subOctave, -3.0f);
    definir (camada.osc1Level, 0.46f);
    definir (camada.osc2Level, 0.24f);
    definir (camada.subLevel, 0.32f);
    definir (camada.noiseLevel, 0.12f);
    definir (camada.cutoff, 1800.0f);
    definir (camada.resonance, 0.36f);
    definir (camada.filterEnv, 0.48f);
    definir (camada.drive, 0.42f);
    definir (camada.attack, 0.003f);
    definir (camada.decay, 0.04f);
    definir (camada.sustain, 0.78f);
    definir (camada.release, 0.04f);
    definir (camada.modAttack, 0.003f);
    definir (camada.modDecay, 0.04f);
    definir (camada.modSustain, 0.45f);
    definir (camada.modRelease, 0.05f);
    definir (camada.lfoWave, 3.0f);
    definir (camada.lfoRate, 5.0f);
    definir (camada.lfoDepth, 0.28f);
    definir (camada.lfoFade, 0.0f);
    definir (camada.lfoPhase, 137.0f);

    ramosDsp.prepareToPlay (sampleRate, blockSize);
    ramosDsp.setEngineEnabled (true);
    float picoDosRamos = 0.0f;
    for (int bloco = 0; bloco < 24; ++bloco)
    {
        juce::MidiBuffer eventos;
        if (bloco == 0)
            eventos.addEvent (
                juce::MidiMessage::noteOn (1, 55, (juce::uint8) 96), 0);
        ramosDsp.processBlock (buffer, eventos);
        if (! blocoFinito (buffer))
            return falha ("novos ramos de DSP produziram NaN/Inf");
        picoDosRamos = juce::jmax (picoDosRamos, picoDoBloco (buffer));
    }
    if (picoDosRamos < 0.0001f)
        return falha ("novos controles do PAD nao produziram audio");

    // Estado completo: programa, nome, favorito e parametros globais precisam
    // voltar iguais quando o projeto for reaberto.
    pad.selectProgram (3);
    pad.changeProgramName (3, "Sunday Atmosphere");
    pad.setProgramFavourite (3, true);
    definir (pad.transpose, 7.0f);
    definir (pad.masterDb, -3.5f);
    definir (pad.velocityCurve, 3.0f);
    definir (pad.fixedVelocity, 77.0f);
    definir (pad.holdMode, 0.0f);
    definir (pad.toggleMode, 1.0f);
    definir (pad.eqEnabled, 0.0f);
    definir (pad.chorusEnabled, 0.0f);
    definir (pad.reverbEnabled, 1.0f);
    definir (pad.delayEnabled, 0.0f);
    definir (pad.gateEnabled, 1.0f);
    definir (pad.gateRate, 2.0f);
    definir (pad.gateDepth, 0.84f);
    definir (pad.layer (2).enabled, 1.0f);
    definir (pad.layer (2).subWave, 2.0f);
    definir (pad.layer (2).noiseType, 1.0f);
    definir (pad.layer (2).filterType, 2.0f);
    definir (pad.layer (2).subOctave, -3.0f);
    definir (pad.layer (2).drive, 0.45f);
    definir (pad.layer (2).modAttack, 0.25f);
    definir (pad.layer (2).modSustain, 0.44f);
    definir (pad.layer (2).lfoPhase, 123.0f);

    juce::MemoryBlock estado;
    pad.getStateInformation (estado);
    if (estado.getSize() == 0)
        return falha ("serializacao do motor retornou vazia");

    PadSynthProcessor restaurado;
    restaurado.setStateInformation (estado.getData(), (int) estado.getSize());
    if (restaurado.getCurrentProgram() != 3
         || restaurado.getProgramName (3) != "Sunday Atmosphere"
         || ! restaurado.isProgramFavourite (3)
         || restaurado.transpose->get() != 7
         || ! quaseIgual (restaurado.masterDb->get(), -3.5f, 0.051f)
         || restaurado.velocityCurve->getIndex() != 3
         || restaurado.fixedVelocity->get() != 77
         || restaurado.holdMode->get()
         || ! restaurado.toggleMode->get()
         || restaurado.eqEnabled->get()
         || restaurado.chorusEnabled->get()
         || ! restaurado.reverbEnabled->get()
         || restaurado.delayEnabled->get()
         || ! restaurado.gateEnabled->get()
         || restaurado.gateRate->getIndex() != 2
         || ! quaseIgual (restaurado.gateDepth->get(), 0.84f)
         || ! restaurado.layer (2).enabled->get()
         || restaurado.layer (2).subWave->getIndex() != 2
         || restaurado.layer (2).noiseType->getIndex() != 1
         || restaurado.layer (2).filterType->getIndex() != 2
         || restaurado.layer (2).subOctave->get() != -3
         || ! quaseIgual (restaurado.layer (2).drive->get(), 0.45f)
         || ! quaseIgual (restaurado.layer (2).modAttack->get(), 0.25f)
         || ! quaseIgual (restaurado.layer (2).modSustain->get(), 0.44f)
         || ! quaseIgual (restaurado.layer (2).lfoPhase->get(), 123.0f, 0.01f))
        return falha ("estado do PAD nao sobreviveu a ida e volta");

    std::cout << "PAD_SYNTH_OK\n";
    return 0;
}
