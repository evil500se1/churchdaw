//==============================================================================
// ChurchDAW Instrument - ponto de entrada do VST3.
//
// O QUE ESTE ARQUIVO E: dez linhas de cola. O instrumento ja existe e ja e um
// juce::AudioProcessor completo (TimbresProcessor: barramento estereo
// declarado, ~180 parametros de automacao, prepareToPlay que adota o sample
// rate do host, acceptsMidi, editor proprio e get/setStateInformation). O que
// faltava nao era motor: era o wrapper de plugin.
//
// O QUE ESTE ALVO DELIBERADAMENTE NAO TEM:
//
//  - MainComponent. Ele e dono do juce::AudioDeviceManager, do
//    AudioProcessorGraph, do AudioProcessorPlayer e do callback de MIDI vindo
//    do driver. Num plugin quem e dono disso e o host. Compilar o
//    MainComponent aqui nao seria "codigo morto": bastava um caminho antigo
//    chamar setAudioDeviceSetup para o ChurchDAW tentar abrir um SEGUNDO
//    dispositivo de audio dentro do Reaper -- e ASIO e exclusivo. Por isso ele
//    fica FORA do alvo, nao desligado em runtime.
//
//  - Host de VST3 aninhado (CHURCHDAW_HOSTS_PLUGINS=0). O scanner relanca
//    File::currentExecutableFile como processo filho; dentro de um plugin isso
//    relanca o HOST. O filho nunca conecta no pipe, cada plugin cai no timeout
//    e vai para a blacklist -- exatamente o modo de falha ja documentado em
//    Main.cpp. Alem disso, o musico que poe o ChurchDAW no Reaper JA TEM um
//    host: ele quer o som, nao o rack.
//
//  - DesktopUpdater. Ele chama installer.startAsProcess("/SILENT /RELAUNCH").
//    Um plugin nao roda o instalador do aplicativo nem relanca nada.
//
//  - Licenciamento desktop. DesktopLicenseClient sobe thread e rede na
//    construcao; N instancias no mesmo projeto do Reaper significariam N
//    clientes de licenca. Fica para a Fase 1.
//
// O aplicativo MinhaDAW nao e tocado por este alvo. Ele continua sendo gerado
// pelas MESMAS fontes de sempre; aqui apenas reaproveitamos o header do motor.
//==============================================================================

#include "../TimbresProcessor.h"

//==============================================================================
// O wrapper de plugin do JUCE chama isto uma vez por instancia.
//
// NAO chamamos setPluginResources() nem setExternalMaster(): ambos recebem
// ponteiros crus para objetos que pertencem ao MainComponent (KnownPluginList,
// AudioPluginFormatManager, MasterProcessor). Deixa-los nulos e o contrato
// suportado -- o proprio TimbresProcessor ja testa os dois por nullptr, e a
// sonda de render offline roda assim ha meses. O master do Reaper substitui o
// MasterProcessor; o rack de terceiros deixa de existir de proposito.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TimbresProcessor();
}
