#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace ChurchDAWMobileAudio
{
enum class BufferDecisionReason
{
    none,
    repeatedXRuns,
    stableIdleRecovery
};

struct BufferTunerState
{
    int xrunPressure = 0;
    int stableWindows = 0;
};

struct BufferTunerDecision
{
    int requestedBuffer = 0;
    BufferTunerState state;
    BufferDecisionReason reason = BufferDecisionReason::none;
};

// Escolhe o tamanho realmente mais proximo da latencia desejada. Usar o
// primeiro tamanho >= alvo cria saltos enormes em listas esparsas: no iPad,
// 12 ms a 48 kHz pulava de 512 (10,67 ms) para 1024 (21,33 ms). Em empate,
// prefere o buffer maior para dar a mesma latencia aproximada com mais folga
// contra xruns. A funcao nao presume que a lista esteja ordenada.
inline int chooseClosestBufferForTargetMs (
    const int* availableBuffers, int numAvailableBuffers,
    double sampleRate, double targetMs) noexcept
{
    if (availableBuffers == nullptr || numAvailableBuffers <= 0
         || sampleRate <= 0.0 || targetMs <= 0.0)
        return 0;

    int best = 0;
    double bestError = std::numeric_limits<double>::max();

    for (int i = 0; i < numAvailableBuffers; ++i)
    {
        const int candidate = availableBuffers[i];
        if (candidate <= 0)
            continue;

        const double candidateMs = 1000.0 * (double) candidate / sampleRate;
        const double error = std::abs (candidateMs - targetMs);

        if (error < bestError - 1.0e-9
             || (std::abs (error - bestError) <= 1.0e-9
                  && candidate > best))
        {
            best = candidate;
            bestError = error;
        }
    }

    return best;
}

// O perfil Rapida deve respeitar a granularidade real da rota, nao uma meta
// abstrata em milissegundos. Assim usa 256 no TANK e passa automaticamente a
// 128 quando uma interface USB realmente anunciar esse tamanho.
inline int chooseSmallestAvailableBuffer (
    const int* availableBuffers, int numAvailableBuffers) noexcept
{
    if (availableBuffers == nullptr || numAvailableBuffers <= 0)
        return 0;

    int smallest = 0;

    for (int i = 0; i < numAvailableBuffers; ++i)
        if (availableBuffers[i] > 0
             && (smallest == 0 || availableBuffers[i] < smallest))
            smallest = availableBuffers[i];

    return smallest;
}

// Uma janela corresponde aos 8 s usados pelo timer do app. Um xrun isolado
// pode ser hot-plug/notificacao do sistema e nao justifica reabrir o audio.
// Dois xruns na mesma janela, ou um em duas janelas consecutivas, confirmam
// pressao real. Depois de 15 janelas limpas (120 s), sem MIDI, recupera um
// degrau de latencia por vez ate a preferencia escolhida pelo musico.
inline BufferTunerDecision decideBuffer (
    int currentBuffer, int preferredBuffer,
    const int* availableBuffers, int numAvailableBuffers,
    int newXRuns, bool midiRecentlyActive,
    BufferTunerState previous,
    int maximumAdaptiveBuffer = 2048) noexcept
{
    BufferTunerDecision result;
    result.requestedBuffer = currentBuffer;
    result.state = previous;

    if (newXRuns > 0)
    {
        result.state.stableWindows = 0;
        result.state.xrunPressure = std::min (
            3, result.state.xrunPressure + (newXRuns >= 2 ? 2 : 1));
    }
    else
    {
        ++result.state.stableWindows;
        result.state.xrunPressure = std::max (
            0, result.state.xrunPressure - 1);
    }

    if (result.state.xrunPressure >= 2)
    {
        int nextHigher = 0;
        for (int i = 0; i < numAvailableBuffers; ++i)
            if (availableBuffers[i] > currentBuffer
                 && availableBuffers[i] <= maximumAdaptiveBuffer
                 && (nextHigher == 0 || availableBuffers[i] < nextHigher))
                nextHigher = availableBuffers[i];

        // APIs/rotas nao garantem a ordem da lista. Escolher o primeiro item
        // podia saltar de 256 direto para 2048, acrescentando muita latencia
        // quando 384/512 tambem estavam disponiveis.
        if (nextHigher > 0)
        {
            result.requestedBuffer = nextHigher;
            result.reason = BufferDecisionReason::repeatedXRuns;
            result.state = {};
            return result;
        }

        // Ja esta no maior degrau permitido. Zera a pressao para nao tentar
        // reabrir o mesmo dispositivo a cada janela.
        result.state.xrunPressure = 0;
        return result;
    }

    if (newXRuns == 0
         && ! midiRecentlyActive
         && currentBuffer > preferredBuffer
         && result.state.stableWindows >= 15)
    {
        int lower = 0;
        for (int i = 0; i < numAvailableBuffers; ++i)
            if (availableBuffers[i] >= preferredBuffer
                 && availableBuffers[i] < currentBuffer)
                lower = std::max (lower, availableBuffers[i]);

        if (lower > 0)
        {
            result.requestedBuffer = lower;
            result.reason = BufferDecisionReason::stableIdleRecovery;
            result.state = {};
        }
    }

    return result;
}
}
