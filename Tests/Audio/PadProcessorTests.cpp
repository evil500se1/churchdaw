#include "PadProcessor.h"
#include "MidiMappingSafety.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

#if defined(_MSC_VER)
 #include <malloc.h>
#endif

namespace
{
thread_local bool contarAlocacoes = false;
thread_local std::uint64_t alocacoesDaThread = 0;

void registrarAlocacao() noexcept
{
    if (contarAlocacoes)
        ++alocacoesDaThread;
}

void* alocar (std::size_t tamanho)
{
    registrarAlocacao();

    if (auto* memoria = std::malloc (std::max<std::size_t> (tamanho, 1)))
        return memoria;

    throw std::bad_alloc {};
}

void* alocarAlinhado (std::size_t tamanho, std::size_t alinhamento)
{
    registrarAlocacao();

#if defined(_MSC_VER)
    if (auto* memoria = _aligned_malloc (
            std::max<std::size_t> (tamanho, 1), alinhamento))
        return memoria;
#else
    void* memoria = nullptr;
    if (posix_memalign (&memoria, alinhamento,
                        std::max<std::size_t> (tamanho, 1)) == 0)
        return memoria;
#endif

    throw std::bad_alloc {};
}

void liberarAlinhado (void* memoria) noexcept
{
#if defined(_MSC_VER)
    _aligned_free (memoria);
#else
    std::free (memoria);
#endif
}
}

void* operator new (std::size_t tamanho)
{
    return alocar (tamanho);
}

void* operator new[] (std::size_t tamanho)
{
    return alocar (tamanho);
}

void operator delete (void* memoria) noexcept
{
    std::free (memoria);
}

void operator delete[] (void* memoria) noexcept
{
    std::free (memoria);
}

void operator delete (void* memoria, std::size_t) noexcept
{
    std::free (memoria);
}

void operator delete[] (void* memoria, std::size_t) noexcept
{
    std::free (memoria);
}

void* operator new (std::size_t tamanho, const std::nothrow_t&) noexcept
{
    try { return alocar (tamanho); }
    catch (...) { return nullptr; }
}

void* operator new[] (std::size_t tamanho, const std::nothrow_t&) noexcept
{
    try { return alocar (tamanho); }
    catch (...) { return nullptr; }
}

void operator delete (void* memoria, const std::nothrow_t&) noexcept
{
    std::free (memoria);
}

void operator delete[] (void* memoria, const std::nothrow_t&) noexcept
{
    std::free (memoria);
}

#if defined(__cpp_aligned_new)
void* operator new (std::size_t tamanho, std::align_val_t alinhamento)
{
    return alocarAlinhado (tamanho, (std::size_t) alinhamento);
}

void* operator new[] (std::size_t tamanho, std::align_val_t alinhamento)
{
    return alocarAlinhado (tamanho, (std::size_t) alinhamento);
}

void operator delete (void* memoria, std::align_val_t) noexcept
{
    liberarAlinhado (memoria);
}

void operator delete[] (void* memoria, std::align_val_t) noexcept
{
    liberarAlinhado (memoria);
}

void operator delete (void* memoria, std::size_t, std::align_val_t) noexcept
{
    liberarAlinhado (memoria);
}

void operator delete[] (void* memoria, std::size_t, std::align_val_t) noexcept
{
    liberarAlinhado (memoria);
}

void* operator new (std::size_t tamanho, std::align_val_t alinhamento,
                    const std::nothrow_t&) noexcept
{
    try { return alocarAlinhado (tamanho, (std::size_t) alinhamento); }
    catch (...) { return nullptr; }
}

void* operator new[] (std::size_t tamanho, std::align_val_t alinhamento,
                      const std::nothrow_t&) noexcept
{
    try { return alocarAlinhado (tamanho, (std::size_t) alinhamento); }
    catch (...) { return nullptr; }
}

void operator delete (void* memoria, std::align_val_t,
                      const std::nothrow_t&) noexcept
{
    liberarAlinhado (memoria);
}

void operator delete[] (void* memoria, std::align_val_t,
                        const std::nothrow_t&) noexcept
{
    liberarAlinhado (memoria);
}
#endif

struct PadProcessorTestAccess
{
    struct Resultado
    {
        int inicio = 0;
        int fim = 0;
        int crossfade = 0;
        float correlacao = 0.0f;
        float correlacaoDeFase = 0.0f;
    };

    static Resultado prepararLoop (juce::AudioBuffer<float>& buffer, double sampleRate,
                                    const juce::StringPairArray& metadata = {})
    {
        const auto loop = PadProcessor::prepararLoop (buffer, sampleRate, metadata);
        return { loop.inicio, loop.fim, loop.crossfade,
                 loop.correlacao, loop.correlacaoDeFase };
    }

    static float reamostrar (const float* src, int len,
                             int loopStart, int loopEnd,
                             double pos, double rate) noexcept
    {
        return PadProcessor::lerReamostrado (src, len, loopStart, loopEnd,
                                             pos, rate);
    }

    static int prepararEExecutarQuintaTroca (PadProcessor& pad) noexcept
    {
        pad.caudaDeRoubo.reset();

        for (int i = 0; i < (int) pad.voices.size(); ++i)
        {
            auto& voz = pad.voices[(size_t) i];
            voz = {};
            voz.active = true;
            voz.gain = 0.8f - 0.2f * (float) i;
            voz.lastOutputL = i == 3 ? 0.75f : 0.1f * (float) (i + 1);
            voz.lastOutputR = i == 3 ? -0.50f : -0.1f * (float) (i + 1);
        }

        auto* roubada = pad.freeVoice();
        const int indice = roubada != nullptr
            ? (int) (roubada - pad.voices.data()) : -1;

        if (roubada != nullptr)
            *roubada = {}; // simula a inicializacao imediata da quinta voz

        return indice;
    }

    static void renderizarCaudaDeRoubo (PadProcessor& pad,
                                        float* outL, float* outR,
                                        int numSamples) noexcept
    {
        pad.renderizarCaudaDeRoubo (outL, outR, numSamples);
    }

    static bool activeVoicePinsCacheUntilAudioRelease (PadProcessor& pad)
    {
        auto sample = std::make_shared<juce::AudioBuffer<float>> (2, 64);
        sample->clear();
        std::weak_ptr<juce::AudioBuffer<float>> weakSample = sample;

        {
            const juce::ScopedLock lock (pad.cacheLock);
            auto prepararEntrada = [&pad] (char note)
            {
                auto& entry = pad.cache[note];
                entry.buffer = std::make_shared<juce::AudioBuffer<float>> (2, 64);
                entry.sampleRate = 48000.0;
                entry.loopStart = 0;
                entry.loopEnd = 64;
            };

            auto& pinned = pad.cache['C'];
            pinned.buffer = sample;
            pinned.sampleRate = 48000.0;
            pinned.loopStart = 0;
            pinned.loopEnd = 64;
            pinned.activeVoiceUsers.store (1, std::memory_order_relaxed);
            prepararEntrada ('D');
            prepararEntrada ('E');

            pad.voices[0].cacheEntry = &pinned;
            pad.voices[0].buffer = pinned.buffer.get();
            pad.voices[0].active = true;
        }

        // D e o tom solicitado. Com tres entradas e limite dois, o coletor
        // pode remover E, mas C precisa sobreviver enquanto a voz usa seu WAV.
        pad.requestedKey.store (2);
        sample.reset();
        pad.trimCacheToLimit();

        {
            const juce::ScopedLock lock (pad.cacheLock);
            if (pad.cache.count ('C') != 1
                 || pad.cache['C'].activeVoiceUsers.load (
                        std::memory_order_relaxed) != 1
                 || weakSample.expired())
                return false;

            auto& extra = pad.cache['F'];
            extra.buffer = std::make_shared<juce::AudioBuffer<float>> (2, 64);
            extra.sampleRate = 48000.0;
            extra.loopEnd = 64;
        }

        // A liberacao feita no caminho de audio so decrementa um atomico. A
        // destruicao real do buffer acontece depois em trimCacheToLimit().
        pad.resetVoice (pad.voices[0]);
        pad.trimCacheToLimit();

        const juce::ScopedLock lock (pad.cacheLock);
        return pad.cache.count ('C') == 0 && weakSample.expired();
    }

    static void prepareRealtimeSwitchingFixture (PadProcessor& pad)
    {
        pad.signalThreadShouldExit();
        pad.loadEvent.signal();
        pad.stopThread (3000);
        pad.prepareToPlay (48000.0, 128);
        pad.requestedKey.store (-1, std::memory_order_relaxed);
        pad.currentKey = -1;

        const juce::ScopedLock lock (pad.cacheLock);
        pad.cache.clear();

        for (const char note : { 'C', 'D' })
        {
            auto& entry = pad.cache[note];
            entry.buffer = std::make_shared<juce::AudioBuffer<float>> (2, 4096);
            entry.sampleRate = 48000.0;
            entry.loopStart = 0;
            entry.loopEnd = 4096;

            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < 4096; ++i)
                    entry.buffer->setSample (
                        channel, i,
                        0.15f * (float) std::sin (
                            juce::MathConstants<double>::twoPi
                            * (double) (note == 'C' ? 7 : 11) * (double) i
                            / 4096.0
                            + (channel == 0 ? 0.0 : 0.23)));
        }
    }

    static void requestKeyFromRealtimeSnapshot (PadProcessor& pad,
                                                int key) noexcept
    {
        pad.requestedKey.store (key, std::memory_order_relaxed);
    }

    static double activeTargetVoiceRate (const PadProcessor& pad) noexcept
    {
        for (const auto& v : pad.voices)
            if (v.active && v.target > 0.5f)
                return v.rate;
        return 0.0;
    }
};

static_assert (noexcept (PadProcessorTestAccess::reamostrar (
    nullptr, 1, 0, 1, 0.0, 2.0)),
    "o caminho de resampling do callback precisa ser noexcept");

namespace
{
struct ParameterChangeProbe final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged (juce::AudioProcessor* processor,
                                         int parameterIndex, float) override
    {
        if (processor == nullptr
             || ! juce::isPositiveAndBelow (parameterIndex,
                                             processor->getParameters().size()))
            return;

        changedIds.addIfNotAlreadyThere (
            ChurchDAWMidiMapping::stableParameterId (
                processor->getParameters()[parameterIndex]));
    }

    void audioProcessorChanged (juce::AudioProcessor*,
                                const juce::AudioProcessorListener::ChangeDetails&) override {}

    juce::StringArray changedIds;
};

bool perto (float a, float b, float tolerancia = 0.0001f)
{
    return std::abs (a - b) <= tolerancia;
}

int falha (const char* mensagem)
{
    std::cerr << "PAD_PROCESSOR_FAIL: " << mensagem << '\n';
    return 1;
}

double rmsStereo (const juce::AudioBuffer<float>& buffer, int inicio, int quantidade)
{
    double energia = 0.0;

    for (int canal = 0; canal < buffer.getNumChannels(); ++canal)
        for (int i = 0; i < quantidade; ++i)
        {
            const double x = buffer.getSample (canal, inicio + i);
            energia += x * x;
        }

    return std::sqrt (energia / (double) (quantidade * buffer.getNumChannels()));
}

double rmsSide (const juce::AudioBuffer<float>& buffer, int inicio, int quantidade)
{
    double energia = 0.0;

    for (int i = 0; i < quantidade; ++i)
    {
        const double side = 0.5 * ((double) buffer.getSample (0, inicio + i)
                                   - buffer.getSample (1, inicio + i));
        energia += side * side;
    }

    return std::sqrt (energia / (double) quantidade);
}

float interpolacaoLinearLegada (const float* src, int len,
                                 double pos) noexcept
{
    const int i0 = juce::jlimit (0, len - 1, (int) pos);
    const int i1 = juce::jmin (len - 1, i0 + 1);
    const float frac = (float) (pos - (double) i0);
    return src[i0] + (src[i1] - src[i0]) * frac;
}

struct MedicaoReamostragem
{
    double rmsProduto = 0.0;
    double rmsLinear = 0.0;
};

MedicaoReamostragem medirTom (double rate, double frequenciaFonte)
{
    constexpr int tamanhoFonte = 65536;
    constexpr int quantidadeSaida = 8192;
    constexpr double posInicial = 4096.25;
    std::vector<float> fonte ((size_t) tamanhoFonte);

    for (int i = 0; i < tamanhoFonte; ++i)
        fonte[(size_t) i] = (float) std::sin (
            juce::MathConstants<double>::twoPi * frequenciaFonte * (double) i);

    double energiaProduto = 0.0;
    double energiaLinear = 0.0;
    double pos = posInicial;

    for (int i = 0; i < quantidadeSaida; ++i)
    {
        const float produto = PadProcessorTestAccess::reamostrar (
            fonte.data(), tamanhoFonte, 0, tamanhoFonte, pos, rate);
        const float linear = interpolacaoLinearLegada (
            fonte.data(), tamanhoFonte, pos);
        energiaProduto += (double) produto * produto;
        energiaLinear += (double) linear * linear;
        pos += rate;
    }

    return { std::sqrt (energiaProduto / quantidadeSaida),
             std::sqrt (energiaLinear / quantidadeSaida) };
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

    if (! PadProcessorTestAccess::activeVoicePinsCacheUntilAudioRelease (pad))
        return falha ("cache destruiu WAV na thread de audio ou ignorou voz ativa");

    if (pad.getParameters().size() != 2)
        return falha ("cutoff e volume precisam estar expostos ao MIDI Learn");

    if (! perto (pad.getGain(), 0.9f) || ! perto (pad.getCutoff(), 1.0f))
        return falha ("defaults de volume/corte mudaram");

    // O MIDI Learn arma o ultimo parametro tocado atraves do listener do
    // AudioProcessor. Se PadProcessor mudar valores sem notificar o host, os
    // knobs mexem visualmente mas nunca podem ser escolhidos como alvo.
    ParameterChangeProbe parameterProbe;
    pad.juce::AudioProcessor::addListener (&parameterProbe);
    pad.setGain (0.73f);
    pad.setCutoff (0.37f);
    pad.juce::AudioProcessor::removeListener (&parameterProbe);

    if (! parameterProbe.changedIds.contains ("padVolume")
         || ! parameterProbe.changedIds.contains ("padCutoff"))
        return falha ("volume/corte nao notificaram o MIDI Learn");

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

    // Gate espectral objetivo: para cada ratio > 1, um tom acima do novo
    // Nyquist deve ser removido ANTES da decimacao. A interpolacao linear
    // legada fica ao lado como controle A/B; filtrar depois nao passaria aqui,
    // pois a energia ja teria dobrado para uma frequencia audivel.
    for (const double rate : { 1.10, 1.50, 2.00, 3.00 })
    {
        const double novoNyquistNaFonte = 0.5 / rate;
        const double tomForaDaBanda = novoNyquistNaFonte
                                      + 0.70 * (0.5 - novoNyquistNaFonte);
        const auto alias = medirTom (rate, tomForaDaBanda);
        const double reducaoDb = 20.0 * std::log10 (
            (alias.rmsLinear + 1.0e-20) / (alias.rmsProduto + 1.0e-20));

        std::cout << "PAD_ALIAS rate=" << rate
                  << " linear_rms=" << alias.rmsLinear
                  << " filtered_rms=" << alias.rmsProduto
                  << " reduction_db=" << reducaoDb << '\n';

        if (reducaoDb < 24.0)
            return falha ("resampling nao reduziu alias em pelo menos 24 dB");

        // Um tom a 16% do Nyquist de saida fica bem dentro da banda util e
        // nao pode pagar a reducao de alias com perda perceptivel de nivel.
        const auto bandaPassante = medirTom (rate, 0.08 / rate);
        const double ganho = bandaPassante.rmsProduto
                              / juce::jmax (1.0e-20, bandaPassante.rmsLinear);

        if (ganho < 0.97 || ganho > 1.03)
            return falha ("anti-alias alterou nivel dentro da banda passante");
    }

    // O helper chamado pelo callback e noexcept e precisa continuar sem heap.
    // A tabela e aquecida antes de ligar o contador, como acontece no produto
    // durante a construcao/prepare e nunca na thread de audio.
    std::array<float, 512> fonteRt {};
    for (int i = 0; i < (int) fonteRt.size(); ++i)
        fonteRt[(size_t) i] = (float) std::sin (0.07 * (double) i);

    volatile float sumidouroRt = PadProcessorTestAccess::reamostrar (
        fonteRt.data(), (int) fonteRt.size(), 64, 448, 128.25, 2.0);
    const auto antesDasAlocacoes = alocacoesDaThread;
    contarAlocacoes = true;

    for (int i = 0; i < 4096; ++i)
        sumidouroRt += PadProcessorTestAccess::reamostrar (
            fonteRt.data(), (int) fonteRt.size(), 64, 448,
            128.25 + std::fmod ((double) i * 1.75, 319.0), 1.75);

    contarAlocacoes = false;
    const auto alocacoesNoRender = alocacoesDaThread - antesDasAlocacoes;

    if (alocacoesNoRender != 0 || ! std::isfinite (sumidouroRt))
        return falha ("caminho de resampling alocou ou produziu valor invalido");

    // Exercita o processBlock inteiro como em uma rajada de comandos MIDI:
    // troca C/D a cada bloco, forcando crossfade e roubo de voz repetidamente.
    // O fixture para o loader antes da medicao para que o teste observe apenas
    // o callback, sem uma thread auxiliar disputar o cache artificial.
    PadProcessor padRt;
    PadProcessorTestAccess::prepareRealtimeSwitchingFixture (padRt);
    juce::AudioBuffer<float> blocoRt (2, 128);
    juce::MidiBuffer midiRt;
    std::uint64_t amostrasInvalidasRt = 0;
    float picoRt = 0.0f;
    const auto antesDoProcessBlock = alocacoesDaThread;
    contarAlocacoes = true;

    for (int blocoIndex = 0; blocoIndex < 1024; ++blocoIndex)
    {
        PadProcessorTestAccess::requestKeyFromRealtimeSnapshot (
            padRt, (blocoIndex & 1) == 0 ? 0 : 2);
        padRt.processBlock (blocoRt, midiRt);

        for (int channel = 0; channel < blocoRt.getNumChannels(); ++channel)
            for (int i = 0; i < blocoRt.getNumSamples(); ++i)
            {
                const float value = blocoRt.getSample (channel, i);
                if (! std::isfinite (value))
                    ++amostrasInvalidasRt;
                else
                    picoRt = juce::jmax (picoRt, std::abs (value));
            }
    }

    contarAlocacoes = false;
    const auto alocacoesNoProcessBlock =
        alocacoesDaThread - antesDoProcessBlock;

    if (alocacoesNoProcessBlock != 0)
        return falha ("trocas densas do Pad alocaram no processBlock");

    if (amostrasInvalidasRt != 0 || picoRt > 8.0f)
        return falha ("trocas densas do Pad produziram NaN/inf ou pico invalido");

    std::cout << "PAD_DENSE_SWITCH_RT blocks=1024 allocations="
              << alocacoesNoProcessBlock << " peak=" << picoRt << '\n';

    // Transpose Global com o Pad ja sustentando precisa atualizar o rate das
    // vozes ativas no bloco seguinte, sem exigir nova troca de tom e sem alocar.
    {
        PadProcessor padTranspose;
        PadProcessorTestAccess::prepareRealtimeSwitchingFixture (padTranspose);
        PadProcessorTestAccess::requestKeyFromRealtimeSnapshot (padTranspose, 0);
        padTranspose.processBlock (blocoRt, midiRt);

        if (std::abs (PadProcessorTestAccess::activeTargetVoiceRate (padTranspose) - 1.0) > 1.0e-9)
            return falha ("rate inicial do Pad em C deveria ser 1.0");

        const auto antesDoTranspose = alocacoesDaThread;
        contarAlocacoes = true;
        padTranspose.setTransposeSemis (7);
        padTranspose.processBlock (blocoRt, midiRt);
        const double rateMaisSete = PadProcessorTestAccess::activeTargetVoiceRate (padTranspose);
        padTranspose.setTransposeSemis (-5);
        padTranspose.processBlock (blocoRt, midiRt);
        const double rateMenosCinco = PadProcessorTestAccess::activeTargetVoiceRate (padTranspose);
        contarAlocacoes = false;

        if (alocacoesDaThread - antesDoTranspose != 0)
            return falha ("Transpose Global do Pad alocou na thread de audio");

        if (std::abs (rateMaisSete - std::pow (2.0, 7.0 / 12.0)) > 1.0e-9
             || std::abs (rateMenosCinco - std::pow (2.0, -5.0 / 12.0)) > 1.0e-9)
            return falha ("Transpose Global nao atualizou o rate do Pad em sustentacao");
    }

    // Com quatro vozes ainda no fade de 1,6 s, a quinta troca precisa roubar
    // uma delas. O sample anterior da voz roubada nao pode simplesmente sumir:
    // uma cauda fixa de 5 ms deve comecar no mesmo valor e chegar suavemente a
    // zero, sem heap no caminho que roda sob o callback.
    PadProcessor padRoubo;
    padRoubo.prepareToPlay (48000.0, 256);
    std::array<float, 256> rouboL {};
    std::array<float, 256> rouboR {};
    const auto alocacoesAntesDoRoubo = alocacoesDaThread;
    contarAlocacoes = true;
    const int indiceRoubado = PadProcessorTestAccess::prepararEExecutarQuintaTroca (
        padRoubo);
    PadProcessorTestAccess::renderizarCaudaDeRoubo (
        padRoubo, rouboL.data(), rouboR.data(), (int) rouboL.size());
    contarAlocacoes = false;
    const auto alocacoesDoRoubo = alocacoesDaThread - alocacoesAntesDoRoubo;

    if (indiceRoubado != 3)
        return falha ("quinta troca nao escolheu a voz de menor ganho");

    if (! perto (rouboL[0], 0.75f, 0.00001f)
         || ! perto (rouboR[0], -0.50f, 0.00001f))
        return falha ("quinta troca sobrescreveu voz ativa sem preservar continuidade");

    constexpr int amostrasDeClick = 240; // 5 ms a 48 kHz
    if (std::abs (rouboL[(size_t) amostrasDeClick - 1]) > 0.01f
         || std::abs (rouboR[(size_t) amostrasDeClick - 1]) > 0.01f
         || rouboL[(size_t) amostrasDeClick] != 0.0f
         || rouboR[(size_t) amostrasDeClick] != 0.0f)
        return falha ("cauda da voz roubada nao terminou em 5 ms sem degrau");

    for (int i = 1; i < amostrasDeClick; ++i)
        if (std::abs (rouboL[(size_t) i] - rouboL[(size_t) i - 1]) > 0.004f
             || std::abs (rouboR[(size_t) i] - rouboR[(size_t) i - 1]) > 0.003f)
            return falha ("declique da quinta troca nao foi uma rampa limitada");

    if (alocacoesDoRoubo != 0)
        return falha ("declique da quinta troca alocou no caminho de audio");

    // Cruza a volta de um loop periodico em rate > 1. O erro contra a forma de
    // onda ideal detecta leitura fora da janela, clamp no fim ou perda da fase
    // fracionaria quando o kernel consulta amostras dos dois lados da emenda.
    constexpr int loopStartAa = 0;
    constexpr int loopLengthAa = 1024;
    constexpr int loopEndAa = loopStartAa + loopLengthAa;
    constexpr double ciclosNoLoop = 149.0;
    std::array<float, loopEndAa> fonteLoop {};

    for (int i = 0; i < loopEndAa; ++i)
    {
        const double fase = juce::MathConstants<double>::twoPi * ciclosNoLoop
                             * ((double) i - loopStartAa) / loopLengthAa;
        fonteLoop[(size_t) i] = (float) std::sin (fase);
    }

    double posLoop = loopEndAa - 24.5;
    double erroQuadratico = 0.0;
    double maiorErroDePassoNaEmenda = 0.0;
    float anterior = 0.0f;
    double idealAnterior = 0.0;
    bool acabouDeVoltar = false;

    for (int i = 0; i < 256; ++i)
    {
        const float atual = PadProcessorTestAccess::reamostrar (
            fonteLoop.data(), (int) fonteLoop.size(), loopStartAa, loopEndAa,
            posLoop, 1.75);
        const double faseIdeal = juce::MathConstants<double>::twoPi * ciclosNoLoop
                                  * (posLoop - loopStartAa) / loopLengthAa;
        const double ideal = std::sin (faseIdeal);
        const double erro = (double) atual - ideal;
        erroQuadratico += erro * erro;

        if (i > 0 && acabouDeVoltar)
            maiorErroDePassoNaEmenda = juce::jmax (
                maiorErroDePassoNaEmenda,
                std::abs (((double) atual - anterior)
                           - (ideal - idealAnterior)));

        anterior = atual;
        idealAnterior = ideal;
        posLoop += 1.75;
        acabouDeVoltar = posLoop >= loopEndAa;
        if (acabouDeVoltar)
            posLoop -= loopLengthAa;
    }

    const double rmseLoop = std::sqrt (erroQuadratico / 256.0);
    std::cout << "PAD_ANTIALIAS_RT allocations=" << alocacoesNoRender
              << " loop_rmse=" << rmseLoop
              << " seam_step_error=" << maiorErroDePassoNaEmenda
              << " steal_allocations=" << alocacoesDoRoubo << '\n';

    if (rmseLoop > 0.01 || maiorErroDePassoNaEmenda > 0.01)
        return falha ("anti-alias quebrou continuidade/fase na volta do loop");

    // Emenda sintetica propositalmente fora de fase. O sustain comeca aos 2 s;
    // o detector por envelope soma a margem e proporia 3 s. O arquivo termina
    // meia volta da fundamental depois desse ponto: o crossfade legado de 2 s
    // misturava quase anti-fase e cavava um buraco/chorus. A busca nova deve
    // mover inicio/fim dentro do sustain e casar L, R, Mid e Side em conjunto.
    constexpr double srLoop = 4000.0;
    constexpr double fundamental = 50.0;
    const int inicioSustain = (int) (2.0 * srLoop);
    const int tamanho = (int) std::round ((12.0 + 0.5 / fundamental) * srLoop);
    juce::AudioBuffer<float> sintetico (2, tamanho);
    sintetico.clear();

    for (int i = inicioSustain; i < tamanho; ++i)
    {
        const double t = (double) (i - inicioSustain) / srLoop;
        const double fase = juce::MathConstants<double>::twoPi * fundamental * t;

        sintetico.setSample (0, i, (float) (0.40 * std::sin (fase)
                                             + 0.11 * std::sin (2.0 * fase + 0.2)));
        sintetico.setSample (1, i, (float) (0.30 * std::sin (fase + 0.7)
                                             + 0.14 * std::sin (3.0 * fase - 0.4)));
    }

    juce::AudioBuffer<float> sinteticoOriginal;
    sinteticoOriginal.makeCopyOf (sintetico);
    const auto emenda = PadProcessorTestAccess::prepararLoop (sintetico, srLoop);

    if (emenda.inicio < inicioSustain || emenda.fim > tamanho
         || emenda.fim - emenda.inicio < (int) (5.0 * srLoop))
        return falha ("busca de fase devolveu janela de loop invalida");

    if (emenda.correlacao < 0.95f || emenda.correlacaoDeFase < 0.95f)
        return falha ("emenda nao casou correlacao/fase stereo");

    if (emenda.crossfade != (int) std::round (0.12 * srLoop))
        return falha ("emenda bem correlacionada nao usou overlap curto de 120 ms");

    // A preparacao e destrutiva somente na cauda reservada ao overlap. Intro,
    // sustain e pre-roll permanecem byte a byte iguais ao WAV carregado.
    for (int canal = 0; canal < sintetico.getNumChannels(); ++canal)
        for (int i = 0; i < emenda.fim - emenda.crossfade; ++i)
            if (sintetico.getSample (canal, i)
                    != sinteticoOriginal.getSample (canal, i))
                return falha ("preparacao alterou audio fora do overlap");

    // O ultimo frame do overlap vira exatamente o frame imediatamente anterior
    // ao loopStart. A virada seguinte tem, portanto, a derivada natural do WAV,
    // nao um degrau inventado pelo loop.
    for (int canal = 0; canal < sintetico.getNumChannels(); ++canal)
    {
        if (! perto (sintetico.getSample (canal, emenda.fim - 1),
                     sinteticoOriginal.getSample (canal, emenda.inicio - 1),
                     0.00001f))
            return falha ("fim do overlap nao converge para o pre-roll");

        const float saltoDaEmenda = std::abs (
            sintetico.getSample (canal, emenda.fim - 1)
             - sintetico.getSample (canal, emenda.inicio));
        const float derivadaNatural = std::abs (
            sinteticoOriginal.getSample (canal, emenda.inicio - 1)
             - sinteticoOriginal.getSample (canal, emenda.inicio));

        if (saltoDaEmenda > derivadaNatural + 0.00001f)
            return falha ("volta do loop criou degrau alem da derivada natural");
    }

    const double rmsSaida = rmsStereo (sintetico,
                                        emenda.fim - emenda.crossfade,
                                        emenda.crossfade);
    const double rmsCauda = rmsStereo (sinteticoOriginal,
                                        emenda.fim - emenda.crossfade,
                                        emenda.crossfade);
    const double rmsEntrada = rmsStereo (sinteticoOriginal,
                                          emenda.inicio - emenda.crossfade,
                                          emenda.crossfade);
    const double rmsReferencia = std::sqrt (0.5 * (rmsCauda * rmsCauda
                                                   + rmsEntrada * rmsEntrada));

    if (rmsSaida < 0.85 * rmsReferencia || rmsSaida > 1.15 * rmsReferencia)
        return falha ("crossfade correlation-aware cavou buraco ou criou pico");

    const double sideSaida = rmsSide (sintetico,
                                      emenda.fim - emenda.crossfade,
                                      emenda.crossfade);
    const double sideCauda = rmsSide (sinteticoOriginal,
                                      emenda.fim - emenda.crossfade,
                                      emenda.crossfade);
    const double sideEntrada = rmsSide (sinteticoOriginal,
                                        emenda.inicio - emenda.crossfade,
                                        emenda.crossfade);
    const double sideReferencia = std::sqrt (0.5 * (sideCauda * sideCauda
                                                    + sideEntrada * sideEntrada));

    if (sideSaida < 0.80 * sideReferencia || sideSaida > 1.20 * sideReferencia)
        return falha ("crossfade colapsou ou inflou a largura stereo");

    // Nem todo material oferece um par realmente correlacionado. Nesse caso
    // a defesa e temporal: em vez de misturar fontes sem fase comum por 2 s e
    // produzir flange/batimento, limita o overlap a 40 ms.
    juce::AudioBuffer<float> descorrelacionado (2, tamanho);
    descorrelacionado.clear();
    std::uint32_t estadoRuido = 0x51a7c3d9u;
    auto ruido = [&estadoRuido]
    {
        estadoRuido = estadoRuido * 1664525u + 1013904223u;
        return ((float) ((estadoRuido >> 8) & 0xffffu) / 32767.5f - 1.0f) * 0.25f;
    };

    for (int i = inicioSustain; i < tamanho; ++i)
    {
        descorrelacionado.setSample (0, i, ruido());
        descorrelacionado.setSample (1, i, ruido());
    }

    const auto emendaRuim = PadProcessorTestAccess::prepararLoop (
        descorrelacionado, srLoop);

    if (emendaRuim.correlacaoDeFase >= 0.30f
         || emendaRuim.crossfade != (int) std::round (0.04 * srLoop))
        return falha ("fallback sem fase prolongou overlap metalico");

    for (int canal = 0; canal < descorrelacionado.getNumChannels(); ++canal)
        for (int i = emendaRuim.fim - emendaRuim.crossfade;
             i < emendaRuim.fim; ++i)
            if (! std::isfinite (descorrelacionado.getSample (canal, i)))
                return falha ("fallback sem fase produziu NaN/inf");

    // Um chunk smpl legitimo continua soberano: a analise pode alisar uma
    // descontinuidade, mas nunca deslocar os pontos escolhidos pelo autor.
    juce::AudioBuffer<float> autorado;
    autorado.makeCopyOf (sinteticoOriginal);
    juce::StringPairArray smpl;
    const int inicioAutor = (int) (4.0 * srLoop);
    const int fimAutor = (int) (10.0 * srLoop);
    smpl.set ("NumSampleLoops", "1");
    smpl.set ("Loop0Start", juce::String (inicioAutor));
    smpl.set ("Loop0End", juce::String (fimAutor - 1));
    const auto emendaAutor = PadProcessorTestAccess::prepararLoop (autorado, srLoop, smpl);

    if (emendaAutor.inicio != inicioAutor || emendaAutor.fim != fimAutor)
        return falha ("pontos do chunk smpl foram deslocados");

    if (emendaAutor.crossfade != 0)
        return falha ("loop smpl ja coerente foi reprocessado sem necessidade");

    for (int canal = 0; canal < autorado.getNumChannels(); ++canal)
        for (int i = 0; i < autorado.getNumSamples(); ++i)
            if (autorado.getSample (canal, i)
                    != sinteticoOriginal.getSample (canal, i))
                return falha ("loop smpl coerente deixou de ser byte intacto");

    // Se o ponto smpl estiver certo mas o ultimo frame trouxer um espigao, os
    // pontos continuam soberanos e apenas a cauda recebe o overlap curto.
    juce::AudioBuffer<float> autoradoComDegrau;
    autoradoComDegrau.makeCopyOf (sinteticoOriginal);
    autoradoComDegrau.setSample (0, fimAutor - 1, 1.0f);
    autoradoComDegrau.setSample (1, fimAutor - 1, -1.0f);
    const auto emendaAutorComDegrau = PadProcessorTestAccess::prepararLoop (
        autoradoComDegrau, srLoop, smpl);

    if (emendaAutorComDegrau.inicio != inicioAutor
         || emendaAutorComDegrau.fim != fimAutor
         || emendaAutorComDegrau.crossfade != (int) std::round (0.02 * srLoop))
        return falha ("loop smpl descontínuo nao recebeu overlap conservador");

    for (int canal = 0; canal < autoradoComDegrau.getNumChannels(); ++canal)
        if (! perto (autoradoComDegrau.getSample (canal, fimAutor - 1),
                     sinteticoOriginal.getSample (canal, inicioAutor - 1),
                     0.00001f))
            return falha ("overlap smpl nao removeu o espigao final");

    std::cout << "PAD_PROCESSOR_OK\n";
    return 0;
}
