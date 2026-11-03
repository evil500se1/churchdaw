#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>

// Motor SoundFont (SF2) por camada — pedido dos clientes iPad/iPhone, mas
// compilado em TODAS as plataformas (o baú de soundfonts gospel gratuitos
// vira timbre de culto em qualquer aparelho).
//
// Desenho: espelha o Church Organ — um motor interno por camada, carregado em
// thread de fundo e publicado para a thread de áudio por swap atômico. O TSF
// (TinySoundFont, MIT) não é thread-safe entre load e render, então a
// instância só é tocada pela thread de áudio DEPOIS de publicada, e a antiga
// morre fora do callback (retire()).
//
// RT-safety do render: tsf_set_max_voices pré-aloca todas as vozes no load;
// note_on/note_off/render não alocam. Sustain/CC64 e o próprio ledger de
// notas ficam com o canal 0 da API de canais do TSF (implementa sustain e
// all-notes-off internamente, sem heap).

namespace churchsf2
{
struct TsfDeleter { void operator() (void* handle) const noexcept; };
using TsfHandle = std::unique_ptr<void, TsfDeleter>;

struct PresetInfo
{
    juce::String name;   // nome do preset dentro do SF2 (indice = posicao)
};

class SoundFontInstance
{
public:
    // Thread de fundo. Retorna nullptr se o arquivo for inválido/corrompido.
    // maxVoices pré-aloca o pool inteiro; sampleRate fixa a taxa do render
    // (recarregar na troca de taxa, como o organ faz no prepare).
    static std::unique_ptr<SoundFontInstance> load (
        const juce::File& file, double sampleRate, int maxVoices);

    ~SoundFontInstance();

    const juce::String& filePath() const noexcept { return path; }
    double loadedSampleRate() const noexcept      { return rate; }
    int presetCount() const noexcept              { return presets.size(); }
    const juce::Array<PresetInfo>& presetList() const noexcept
    {
        return presets;
    }

    // ---- Somente thread de áudio depois de publicado ----------------------

    // Seleciona o preset ativo (índice na presetList). Sem alocação: o TSF
    // troca ponteiros internos. Índice inválido é ignorado.
    void selectPreset (int index) noexcept;
    int currentPreset() const noexcept { return activePreset; }

    void noteOn (int midiNote, float velocity01) noexcept;
    void noteOff (int midiNote) noexcept;
    void setSustain (bool down) noexcept;
    void pitchWheel (int value14bit) noexcept;

    // CC120/123 do PANIC: mata tudo (sounds off) e devolve o sustain a zero.
    void allSoundOff() noexcept;
    // Solta as notas respeitando release (troca de cena suave / flush).
    void allNotesOff() noexcept;

    // Renderiza ADITIVAMENTE em dois canais planos (o bus da camada soma as
    // fontes). numSamples limitado pelo chamador ao bloco corrente.
    void renderAdd (float* left, float* right, int numSamples) noexcept;

    // O motor tem alguma voz soando/cauda? (gate de silêncio da camada)
    bool isQuiet() const noexcept;

private:
    SoundFontInstance() = default;

    TsfHandle handle;
    juce::String path;
    double rate = 48000.0;
    int activePreset = -1;
    juce::Array<PresetInfo> presets;
    // O TSF renderiza UNWEAVED num buffer unico (L plano + R plano); o bus da
    // camada usa planos separados. Scratch fixo alocado no LOAD (nunca no
    // callback), processado em fatias de maxRenderBlock.
    static constexpr int maxRenderBlock = 4096;
    juce::HeapBlock<float> scratch;   // 2 * maxRenderBlock floats
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundFontInstance)
};

// Porta-instância por camada: a thread de fundo publica, a de áudio consome,
// e a retirada acontece na message thread (nunca destruir tsf no callback).
class SoundFontSlot
{
public:
    // Thread de áudio: instância corrente (pode ser nullptr).
    SoundFontInstance* current() const noexcept
    {
        return active.load (std::memory_order_acquire);
    }

    // Message thread: publica a instância nova e devolve a antiga para o
    // chamador destruir fora do callback (padrão retire do projeto).
    std::unique_ptr<SoundFontInstance> publish (
        std::unique_ptr<SoundFontInstance> next) noexcept
    {
        auto* raw = next.release();
        auto* old = active.exchange (raw, std::memory_order_acq_rel);
        return std::unique_ptr<SoundFontInstance> (old);
    }

    std::unique_ptr<SoundFontInstance> clear() noexcept
    {
        return publish (nullptr);
    }

private:
    std::atomic<SoundFontInstance*> active { nullptr };
};

} // namespace churchsf2
