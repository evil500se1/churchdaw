#pragma once

// Cronometro de interface, temporario e desligado por padrao.
//
// Motivo: no TANK 3 medimos ~15 fps durante rolagem e UM quadro de 6.563 ms.
// Seis segundos e meio nao e "desenho lento", e bloqueio -- e adivinhar qual
// e o bloqueio custa mais caro do que perguntar ao proprio app. Este arquivo
// existe para responder com medida, nao com teoria.
//
// Ligar com -DCHURCHDAW_UI_PROFILE=1. No Android escreve direto no logcat
// (tag ChurchDAW-UI), porque o FileLogger grava na area privada do pacote e
// um APK de release nao permite `run-as` para le-la.
//
// REMOVER antes do artefato final: procurar CHURCHDAW_UI_PROFILE.

#include <juce_core/juce_core.h>

#ifndef CHURCHDAW_UI_PROFILE
 #define CHURCHDAW_UI_PROFILE 0
#endif

#if CHURCHDAW_UI_PROFILE

 #if JUCE_ANDROID
  #include <android/log.h>
 #endif

namespace ChurchDAWUiProfile
{
    // Abaixo disto o quadro cabe em 60 fps com folga; nao interessa registrar.
    // Pode ser baixado para 0 quando o que se quer e a DISTRIBUICAO inteira e
    // nao so os picos -- otimizar olhando apenas os outliers leva a conclusao
    // errada sobre quantos tiques realmente custam caro.
   #ifndef CHURCHDAW_UI_PROFILE_ABOVE_MS
    #define CHURCHDAW_UI_PROFILE_ABOVE_MS 12.0
   #endif
    inline constexpr double reportAboveMs = CHURCHDAW_UI_PROFILE_ABOVE_MS;

    inline void report (const char* what, double elapsedMs)
    {
        if (elapsedMs < reportAboveMs)
            return;

       #if JUCE_ANDROID
        __android_log_print (ANDROID_LOG_WARN, "ChurchDAW-UI",
                             "%s %.1f ms", what, elapsedMs);
       #else
        juce::Logger::writeToLog (
            juce::String ("[ui] ") + what + " "
             + juce::String (elapsedMs, 1) + " ms");
       #endif
    }

    // Cronometro de VOLTAS, para dividir uma funcao longa em trechos sem ter
    // de aninhar escopos. Cada CHURCHDAW_UI_LAP fecha o trecho anterior.
    struct Lap
    {
        juce::int64 mark = juce::Time::getHighResolutionTicks();

        double next() noexcept
        {
            const auto now = juce::Time::getHighResolutionTicks();
            const auto ms = juce::Time::highResolutionTicksToSeconds (now - mark) * 1000.0;
            mark = now;
            return ms;
        }
    };

    struct Scope
    {
        explicit Scope (const char* nameIn) noexcept
            : name (nameIn),
              start (juce::Time::getHighResolutionTicks())
        {
        }

        ~Scope() noexcept
        {
            const auto ticks = juce::Time::getHighResolutionTicks() - start;
            report (name, juce::Time::highResolutionTicksToSeconds (ticks) * 1000.0);
        }

        const char* name;
        juce::int64 start;
    };
}

// Nome fixo de proposito: um escopo por funcao. Concatenar __LINE__ exigiria
// dupla indirecao de macro e nao traria nada aqui.
 #define CHURCHDAW_UI_SCOPE(nome) \
     const ChurchDAWUiProfile::Scope churchdawUiScope { nome }

 #define CHURCHDAW_UI_LAP_BEGIN() \
     ChurchDAWUiProfile::Lap churchdawUiLap

 #define CHURCHDAW_UI_LAP(nome) \
     ChurchDAWUiProfile::report (nome, churchdawUiLap.next())

#else
 #define CHURCHDAW_UI_SCOPE(nome) do {} while (false)
 #define CHURCHDAW_UI_LAP_BEGIN()  do {} while (false)
 #define CHURCHDAW_UI_LAP(nome)    do {} while (false)
#endif
