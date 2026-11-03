//==============================================================================
// Sinal compartilhado de atividade de audio: "esta saindo som agora?".
//
// Motivo: o encerramento por licenca vencida rodava em thread propria, sem
// depender de rede, e fechava a janela principal no meio do culto -- o pior
// momento possivel para uma igreja. Com este sinal o app adia o encerramento
// enquanto houver som, avisa o musico e encerra no primeiro silencio.
//
// Header-only e sem dependencia de CMake: basta incluir. Atualizado na thread
// de audio (um peak por bloco) e lido de qualquer thread.
#pragma once

#include <atomic>
#include <cmath>
#include <juce_core/juce_core.h>

struct ChurchDAWAudioActivity
{
    // Chamado no callback de audio, DEPOIS de o buffer de saida estar pronto.
    static void noteBlock (float* const* data, int channels, int samples) noexcept
    {
        if (data == nullptr || channels <= 0 || samples <= 0)
            return;

        float peak = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const float* d = data[c];
            if (d == nullptr)
                continue;

            for (int i = 0; i < samples; ++i)
            {
                const float a = std::fabs (d[i]);
                if (a > peak)
                    peak = a;
            }
        }

        // Limiar baixo, mas acima do ruido de float: silencio de verdade da 0.
        if (peak > 1.0e-5f)
            lastAudibleMs.store (juce::Time::currentTimeMillis(),
                                 std::memory_order_release);
    }

    static bool audibleWithin (juce::int64 windowMs) noexcept
    {
        const auto last = lastAudibleMs.load (std::memory_order_acquire);
        if (last <= 0)
            return false;

        return (juce::Time::currentTimeMillis() - last) <= windowMs;
    }

    inline static std::atomic<juce::int64> lastAudibleMs { 0 };
};
