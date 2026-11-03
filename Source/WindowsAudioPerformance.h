#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// O WINDOWS 11 EM CPU HIBRIDA (Intel 12a geracao em diante / Core Ultra: nucleos
// P, E e LP-E) E O QUE FAZ UM PC FORTE PIPOCAR.
//
// Duas coisas do sistema, nenhuma delas "falta de CPU":
//
//  1. EcoQoS. Quando a janela do ChurchDAW nao esta em primeiro plano -- o
//     operador no software de projecao durante o culto, o caso normal de
//     igreja -- o Windows pode classificar o processo como "em segundo plano" e
//     estrangula-lo: threads vao para os nucleos de eficiencia (no Core Ultra,
//     ate para os LP-E do tile SoC) e o clock cai. O callback do WASAPI ja
//     entra no MMCSS "Pro Audio" pelo JUCE, mas o resto do motor nao.
//
//  2. A metade paralela do sampler (ChurchDAWVoiceRenderWorker) roda DENTRO do
//     prazo do callback: a thread de audio despacha e espera. Ela nascia como
//     TIME_CRITICAL comum (15), ABAIXO do MMCSS da thread que a espera e sem
//     nenhuma pista para o escalonador de que e trabalho de audio. Quando ela
//     cai num nucleo lento, o callback inteiro estoura.
//
// Tudo aqui e header-only e sem <windows.h> (que arrastaria macros como min,
// max, small e DrawText para os 20 mil+ linhas da UI). As funcoes sao
// resolvidas por nome com juce::DynamicLibrary, do mesmo jeito que o proprio
// JUCE faz com o avrt.dll no WASAPI (juce_WASAPI_windows.cpp). As constantes
// abaixo sao ABI estavel documentada do Windows.
//==============================================================================
#if JUCE_WINDOWS
namespace ChurchDAWWindowsPerformance
{
    namespace abi
    {
        // processthreadsapi.h
        constexpr int processPowerThrottling = 4;          // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
        constexpr unsigned long powerThrottlingVersion = 1;       // PROCESS_POWER_THROTTLING_CURRENT_VERSION
        constexpr unsigned long throttleExecutionSpeed = 0x1;     // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        constexpr unsigned long throttleIgnoreTimerResolution = 0x4; // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION

        struct PowerThrottlingState          // PROCESS_POWER_THROTTLING_STATE
        {
            unsigned long version;
            unsigned long controlMask;
            unsigned long stateMask;
        };
        static_assert (sizeof (PowerThrottlingState) == 12, "layout do PROCESS_POWER_THROTTLING_STATE");

        // avrt.h
        constexpr int avrtPriorityNormal = 0;              // AVRT_PRIORITY_NORMAL

        using SetProcessInformationFn = int (__stdcall*) (void*, int, void*, unsigned long);
        using AvSetMmThreadCharacteristicsFn = void* (__stdcall*) (const wchar_t*, unsigned long*);
        using AvSetMmThreadPriorityFn = int (__stdcall*) (void*, int);
        using AvRevertMmThreadCharacteristicsFn = int (__stdcall*) (void*);

        inline void* currentProcessPseudoHandle() noexcept { return (void*) (juce::pointer_sized_int) -1; }
    }

    //==========================================================================
    // Tira o processo inteiro do EcoQoS: "HighQoS" explicito, com a janela em
    // primeiro plano ou nao. Tambem pede que as resolucoes de timer do processo
    // continuem valendo em segundo plano (Windows 11). Chamar uma vez, cedo, na
    // thread principal. Em Windows antigos sem o bit de timer, tenta so o de
    // velocidade; sem a API (Windows 7), nao faz nada.
    inline bool optProcessOutOfPowerThrottling() noexcept
    {
        juce::DynamicLibrary kernel ("kernel32.dll");
        const auto setInfo = (abi::SetProcessInformationFn) kernel.getFunction ("SetProcessInformation");
        if (setInfo == nullptr)
            return false;

        auto apply = [setInfo] (unsigned long mask)
        {
            abi::PowerThrottlingState state { abi::powerThrottlingVersion, mask, 0 };   // stateMask 0 = nunca estrangular
            return setInfo (abi::currentProcessPseudoHandle(), abi::processPowerThrottling,
                            &state, (unsigned long) sizeof (state)) != 0;
        };

        return apply (abi::throttleExecutionSpeed | abi::throttleIgnoreTimerResolution)
            || apply (abi::throttleExecutionSpeed);
    }

    //==========================================================================
    // Coloca a thread ATUAL no MMCSS "Pro Audio", a mesma classe da thread do
    // WASAPI no JUCE. Para threads que trabalham dentro do prazo do callback
    // (render paralelo de vozes). Sai do MMCSS no destrutor, na mesma thread.
    class ScopedProAudioThread
    {
    public:
        ScopedProAudioThread() noexcept
        {
            const auto setCharacteristics = (abi::AvSetMmThreadCharacteristicsFn)
                avrt.getFunction ("AvSetMmThreadCharacteristicsW");
            const auto setPriority = (abi::AvSetMmThreadPriorityFn)
                avrt.getFunction ("AvSetMmThreadPriority");

            if (setCharacteristics == nullptr)
                return;

            unsigned long taskIndex = 0;
            handle = setCharacteristics (L"Pro Audio", &taskIndex);

            if (handle != nullptr && setPriority != nullptr)
                setPriority (handle, abi::avrtPriorityNormal);
        }

        ~ScopedProAudioThread() noexcept
        {
            if (handle == nullptr)
                return;

            if (const auto revert = (abi::AvRevertMmThreadCharacteristicsFn)
                    avrt.getFunction ("AvRevertMmThreadCharacteristics"))
                revert (handle);
        }

        bool isActive() const noexcept { return handle != nullptr; }

    private:
        juce::DynamicLibrary avrt { "avrt.dll" };
        void* handle = nullptr;

        JUCE_DECLARE_NON_COPYABLE (ScopedProAudioThread)
    };
}
#endif
