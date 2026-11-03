#include "MobileAudioBufferPolicy.h"

#include <iostream>

namespace
{
int fail (const char* message)
{
    std::cerr << "MOBILE_BUFFER_POLICY_FAIL: " << message << '\n';
    return 1;
}
}

int main()
{
    {
        constexpr int ipadSizes[] = { 64, 128, 256, 512, 1024 };
        const int picked = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            ipadSizes, 5, 48000.0, 12.0);
        if (picked != 512)
            return fail ("perfil Natural escolheu 1024 no iPad");
    }

    {
        constexpr int unorderedSizes[] = { 1024, 256, 768, 512, 384 };
        const int natural = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            unorderedSizes, 5, 48000.0, 12.0);
        const int rapid = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            unorderedSizes, 5, 48000.0, 8.0);
        const int nativeFast =
            ChurchDAWMobileAudio::chooseSmallestAvailableBuffer (
                unorderedSizes, 5);
        const int safe = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            unorderedSizes, 5, 48000.0, 20.0);
        if (natural != 512 || rapid != 384 || safe != 1024
             || nativeFast != 256)
            return fail ("presets nao escolheram o buffer mais proximo");
    }

    {
        constexpr int tiedSizes[] = { 256, 512 };
        const double midpointMs = 1000.0 * 384.0 / 48000.0;
        if (ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
                tiedSizes, 2, 48000.0, midpointMs) != 512)
            return fail ("empate nao favoreceu a margem contra xruns");
        if (ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
                nullptr, 0, 48000.0, 12.0) != 0)
            return fail ("lista vazia retornou buffer invalido");
        if (ChurchDAWMobileAudio::chooseSmallestAvailableBuffer (
                nullptr, 0) != 0)
            return fail ("lista vazia retornou buffer rapido invalido");
    }

    {
        // Faixa de palco que precisamos suportar sem degradar o DSP: uma
        // interface USB pode anunciar 128, a saida nativa do TANK usa 256 e o
        // perfil Natural a 48 kHz deve ficar em 512 (10,67 ms), sem saltar
        // desnecessariamente para 1024.
        constexpr int stageSizes[] = { 1024, 512, 256, 128 };
        const int usbRapid = ChurchDAWMobileAudio::chooseSmallestAvailableBuffer (
            stageSizes, 4);
        const int nativeTank =
            ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
                stageSizes, 4, 48000.0, 1000.0 * 256.0 / 48000.0);
        const int natural = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            stageSizes, 4, 48000.0, 12.0);

        if (usbRapid != 128 || nativeTank != 256 || natural != 512)
            return fail ("faixa de palco 128/256/512 foi escolhida incorretamente");

        ChurchDAWMobileAudio::BufferTunerState stageState;
        const auto protectedDecision = ChurchDAWMobileAudio::decideBuffer (
            128, 128, stageSizes, 4, 2, true, stageState);
        if (protectedDecision.requestedBuffer != 256
             || protectedDecision.reason
                  != ChurchDAWMobileAudio::BufferDecisionReason::repeatedXRuns)
            return fail ("xrun em 128 nao subiu somente um degrau para 256");
    }

    constexpr int sizes[] = { 256, 384, 512, 768, 1024, 1536, 2048 };
    ChurchDAWMobileAudio::BufferTunerState state;

    auto decision = ChurchDAWMobileAudio::decideBuffer (
        768, 768, sizes, 7, 1, true, state);
    if (decision.requestedBuffer != 768 || decision.state.xrunPressure != 1)
        return fail ("um xrun isolado reabriu o dispositivo");

    decision = ChurchDAWMobileAudio::decideBuffer (
        768, 768, sizes, 7, 0, true, decision.state);
    if (decision.requestedBuffer != 768 || decision.state.xrunPressure != 0)
        return fail ("janela limpa nao descartou xrun isolado");

    state = {};
    decision = ChurchDAWMobileAudio::decideBuffer (
        768, 768, sizes, 7, 1, true, state);
    decision = ChurchDAWMobileAudio::decideBuffer (
        768, 768, sizes, 7, 1, true, decision.state);
    if (decision.requestedBuffer != 1024
         || decision.reason
              != ChurchDAWMobileAudio::BufferDecisionReason::repeatedXRuns)
        return fail ("xruns repetidos nao aumentaram um degrau");

    {
        constexpr int unorderedAdaptive[] = {
            2048, 512, 1536, 384, 1024, 768, 256
        };
        state = {};
        decision = ChurchDAWMobileAudio::decideBuffer (
            768, 768, unorderedAdaptive, 7, 2, true, state);
        if (decision.requestedBuffer != 1024)
            return fail ("lista fora de ordem pulou mais de um degrau");
    }

    state = {};
    decision = ChurchDAWMobileAudio::decideBuffer (
        1024, 768, sizes, 7, 2, true, state);
    if (decision.requestedBuffer != 1536)
        return fail ("rajada de xruns nao recebeu margem imediatamente");

    state = {};
    decision = ChurchDAWMobileAudio::decideBuffer (
        1024, 768, sizes, 7, 2, true, state, 1024);
    if (decision.requestedBuffer != 1024
         || decision.state.xrunPressure != 0)
        return fail ("limite da rota nao impediu reaberturas sem ganho");

    state = {};
    for (int i = 0; i < 15; ++i)
    {
        decision = ChurchDAWMobileAudio::decideBuffer (
            1536, 768, sizes, 7, 0, true, state);
        state = decision.state;
    }
    if (decision.requestedBuffer != 1536)
        return fail ("tuner reduziu buffer enquanto havia MIDI");

    state = {};
    decision = {};
    for (int i = 0; i < 15; ++i)
    {
        decision = ChurchDAWMobileAudio::decideBuffer (
            1536, 768, sizes, 7, 0, false, state);
        state = decision.state;
    }
    if (decision.requestedBuffer != 1024
         || decision.reason
              != ChurchDAWMobileAudio::BufferDecisionReason::stableIdleRecovery)
        return fail ("latencia temporaria nao recuperou apos 120 s estaveis");

    state = {};
    decision = {};
    for (int i = 0; i < 30; ++i)
    {
        decision = ChurchDAWMobileAudio::decideBuffer (
            768, 768, sizes, 7, 0, false, state);
        state = decision.state;
    }
    if (decision.requestedBuffer != 768)
        return fail ("tuner reduziu abaixo da preferencia manual");

    std::cout << "MOBILE_BUFFER_POLICY_OK\n";
    return 0;
}
