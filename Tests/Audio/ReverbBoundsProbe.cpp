// Sonda de limites do WorshipReverb.
//
// POR QUE ELA EXISTE
// ------------------
// Esta sonda nasceu durante a investigacao de fechamentos 0xC0000409 e de uma
// suite que antes devolvia resultados diferentes entre execucoes. A causa DSP
// encontrada foi corrigida em WorshipReverb::FractionalDelay: um wrap feito em
// float podia arredondar a posicao para exatamente data.size().
//
// Agora este executavel e uma REGRESSAO: deve terminar sem assert em Debug e
// sem alerta em AddressSanitizer. Um novo fechamento na troca de timbre nao
// implica automaticamente falha do reverb; carregamento/pressao de memoria tem
// uma frente de teste separada.
//
// Se uma regressao reaparecer, Debug imprime a pilha no momento do assert, e
// RelWithDebInfo usa AddressSanitizer para localizar o acesso invalido.
//
// Compilar:
//   cmake -S . -B build -DCHURCHDAW_BUILD_AUDIO_TESTS=ON
//   cmake --build build --config Debug --target ChurchDAWReverbBoundsProbe
//
// Ela imita o que o app faz: mesma taxa/bloco do MainComponent, o buffer de
// reverb superdimensionado do TimbresProcessor (jmax(8192, block)), e troca de
// preset -- que e o caminho exercitado quando o usuario troca de timbre.

#include "../../Source/WorshipReverb.h"

#include <juce_core/juce_core.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#if defined (_MSC_VER)
 #include <crtdbg.h>
 #include <float.h>
 #include <windows.h>
#endif

namespace
{
    int assertsSeen = 0;

#if defined (_MSC_VER)
    // Onde o NaN NASCE, nao onde ele explode.
    //
    // Desmascarando a excecao de operacao invalida, a primeira conta que
    // produzir NaN (0/0, Inf-Inf, sqrt de negativo...) levanta excecao na hora,
    // com a pilha ainda apontando para a linha culpada. Sem isso, so se ve o
    // estrago la na frente, no indice do vector.
    LONG CALLBACK floatingPointTrap (EXCEPTION_POINTERS* info)
    {
        const auto code = info->ExceptionRecord->ExceptionCode;

        if (code == EXCEPTION_FLT_INVALID_OPERATION
             || code == EXCEPTION_FLT_DIVIDE_BY_ZERO
             || code == EXCEPTION_FLT_OVERFLOW)
        {
            std::cerr << "\n================ NaN/Inf CRIADO AQUI ================\n"
                      << "codigo: 0x" << std::hex << code << std::dec << "\n"
                      << juce::SystemStats::getStackBacktrace()
                      << "=====================================================\n";
            std::cerr.flush();
            std::exit (3);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

#if defined (_MSC_VER)
    int __cdecl reportHook (int reportType, char* message, int* returnValue)
    {
        if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR)
        {
            ++assertsSeen;
            std::cerr << "\n================ ACESSO FORA DOS LIMITES ================\n"
                      << (message != nullptr ? message : "(sem mensagem)")
                      << "\n---------------- pilha ----------------\n"
                      << juce::SystemStats::getStackBacktrace()
                      << "=========================================================\n";
            std::cerr.flush();

            if (returnValue != nullptr)
                *returnValue = 0;      // nao invocar o depurador

            return 1;                  // tratado: nao abrir caixa de dialogo
        }

        return 0;
    }
#endif

    void fill (juce::AudioBuffer<float>& buffer, int numSamples, float amplitude, double& phase,
               double frequency, double sampleRate)
    {
        buffer.setSize (2, numSamples, false, false, true);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto s = (float) (std::sin (phase) * amplitude);
            buffer.setSample (0, i, s);
            buffer.setSample (1, i, s);
            phase += juce::MathConstants<double>::twoPi * frequency / sampleRate;

            if (phase > juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
    }

    void banner (const char* text)
    {
        std::cout << "\n--- " << text << " ---" << std::endl;
    }
}

int main()
{
#if defined (_MSC_VER)
    _CrtSetReportHook (reportHook);
    _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

#if defined (_MSC_VER)
    AddVectoredExceptionHandler (1, floatingPointTrap);
    unsigned int previousControl = 0;
    _controlfp_s (&previousControl, 0, _EM_INVALID | _EM_ZERODIVIDE);
#endif

    WorshipReverb reverb;
    juce::AudioBuffer<float> bus;
    double phase = 0.0;

    // Exatamente o que o app usa: 44,1 kHz e bloco 128 (visto na barra do
    // ChurchDAW), com o barramento reservado em 8192 amostras.
    const double sampleRate = 44100.0;
    const int hostBlock = 128;

    banner ("prepare(44100, 128) e blocos normais");
    reverb.prepare (sampleRate, hostBlock);

    for (int b = 0; b < 200; ++b)
    {
        fill (bus, hostBlock, 0.3f, phase, 220.0, sampleRate);
        reverb.process (bus);
    }

    // O TimbresProcessor reserva reverbBus com jmax(8192, block). Se em algum
    // caminho ele entregar o buffer inteiro em vez de apenas numSamples, o
    // reverb recebe um bloco MUITO maior do que aquele para o qual foi
    // preparado. E o suspeito numero um.
    banner ("bloco MAIOR do que o preparado (8192 vs 128)");
    fill (bus, 8192, 0.3f, phase, 220.0, sampleRate);
    reverb.process (bus);

    banner ("troca de preset em todas as 12 maquinas");
    for (int preset = 0; preset < WorshipReverb::numPresets; ++preset)
    {
        reverb.setPreset (preset);

        for (int b = 0; b < 40; ++b)
        {
            fill (bus, hostBlock, 0.3f, phase, 220.0, sampleRate);
            reverb.process (bus);
        }

        std::cout << "  " << WorshipReverb::presetName (preset) << " ok" << std::endl;
    }

    banner ("troca de preset no MEIO do bloco, ida e volta (caminho do troca-timbre)");
    for (int repeat = 0; repeat < 60; ++repeat)
    {
        reverb.setPreset (repeat % 2 == 0 ? 1 : 8);
        fill (bus, hostBlock, 0.3f, phase, 220.0, sampleRate);
        reverb.process (bus);
        reverb.reset();
        fill (bus, hostBlock, 0.3f, phase, 220.0, sampleRate);
        reverb.process (bus);
    }

    banner ("re-prepare com blocos variados (troca de dispositivo/buffer)");
    for (const int block : { 32, 64, 128, 256, 512, 1024, 2048 })
    {
        reverb.prepare (sampleRate, block);

        for (int b = 0; b < 30; ++b)
        {
            fill (bus, block, 0.3f, phase, 220.0, sampleRate);
            reverb.process (bus);
        }

        std::cout << "  bloco " << block << " ok" << std::endl;
    }

    // Cada taxa em uma instancia NOVA: separa "a troca de taxa quebra" de
    // "algum estado sujo de antes quebra".
    banner ("taxas de amostragem, instancia NOVA a cada uma");
    for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        WorshipReverb fresh;
        juce::AudioBuffer<float> freshBus;
        double freshPhase = 0.0;
        fresh.prepare (rate, 512);

        for (int b = 0; b < 30; ++b)
        {
            fill (freshBus, 512, 0.3f, freshPhase, 220.0, rate);
            fresh.process (freshBus);
        }

        std::cout << "  " << rate << " Hz (nova) ok" << std::endl;
    }

    // Agora a MESMA instancia mudando de taxa -- o caminho que quebrou antes.
    banner ("troca de taxa na MESMA instancia");
    for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        reverb.prepare (rate, 512);

        for (int b = 0; b < 30; ++b)
        {
            fill (bus, 512, 0.3f, phase, 220.0, rate);
            reverb.process (bus);
        }

        std::cout << "  " << rate << " Hz (mesma) ok" << std::endl;
    }

    std::cout << "\n=========================================" << std::endl;
    std::cout << (assertsSeen == 0
                    ? "NENHUM acesso fora dos limites detectado"
                    : "ACESSOS FORA DOS LIMITES DETECTADOS")
              << ": " << assertsSeen << std::endl;

    return assertsSeen == 0 ? 0 : 1;
}
