#include "SoundFontEngine.h"

// Unica unidade que instancia o TinySoundFont (single-header, MIT).
#define TSF_IMPLEMENTATION
#include "ThirdParty/tsf.h"

namespace churchsf2
{
static tsf* asTsf (void* h) noexcept { return static_cast<tsf*> (h); }

void TsfDeleter::operator() (void* h) const noexcept
{
    if (h != nullptr)
        tsf_close (asTsf (h));
}

std::unique_ptr<SoundFontInstance> SoundFontInstance::load (
    const juce::File& file, double sampleRate, int maxVoices)
{
    if (! file.existsAsFile() || sampleRate <= 0.0)
        return {};

    // tsf_load_filename le o arquivo inteiro para a memoria. SoundFonts de
    // culto tipicos tem 5-200 MB; o load roda em thread de fundo (nunca no
    // callback) e o dono decide o limite de tamanho antes de chamar.
    tsf* raw = tsf_load_filename (file.getFullPathName().toRawUTF8());
    if (raw == nullptr)
        return {};

    std::unique_ptr<SoundFontInstance> instance (new SoundFontInstance());
    instance->handle = TsfHandle (raw);
    instance->path = file.getFullPathName();
    instance->rate = sampleRate;

    // UNWEAVED: L plano seguido de R plano no scratch; o renderAdd soma nos
    // planos do bus da camada.
    tsf_set_output (raw, TSF_STEREO_UNWEAVED, (int) sampleRate, 0.0f);

    // Pre-aloca TODAS as vozes agora: nenhuma alocacao sobra pro callback.
    // 64 vozes cobre acorde denso + pedal em timbres de culto.
    if (! tsf_set_max_voices (raw, juce::jlimit (16, 256, maxVoices)))
        return {};

    instance->scratch.allocate ((size_t) maxRenderBlock * 2, true);

    const int count = tsf_get_presetcount (raw);
    for (int i = 0; i < count; ++i)
    {
        PresetInfo info;
        const char* name = tsf_get_presetname (raw, i);
        info.name = (name != nullptr && name[0] != 0)
            ? juce::String::fromUTF8 (name)
            : "Preset " + juce::String (i + 1);
        instance->presets.add (info);
    }

    if (instance->presets.isEmpty())
        return {};

    // Canal 0 do TSF: sustain/all-off implementados internamente sem heap.
    tsf_channel_set_presetindex (raw, 0, 0);
    instance->activePreset = 0;
    return instance;
}

SoundFontInstance::~SoundFontInstance() = default;

void SoundFontInstance::selectPreset (int index) noexcept
{
    if (! juce::isPositiveAndBelow (index, presets.size())
         || index == activePreset)
        return;

    // Troca de preset solta as vozes antigas com release natural (o TSF
    // mantem cada voz amarrada ao preset que a criou).
    tsf_channel_note_off_all (asTsf (handle.get()), 0);
    tsf_channel_set_presetindex (asTsf (handle.get()), 0, index);
    activePreset = index;
}

void SoundFontInstance::noteOn (int midiNote, float velocity01) noexcept
{
    if (juce::isPositiveAndBelow (midiNote, 128))
        tsf_channel_note_on (asTsf (handle.get()), 0, midiNote,
                             juce::jlimit (0.0f, 1.0f, velocity01));
}

void SoundFontInstance::noteOff (int midiNote) noexcept
{
    if (juce::isPositiveAndBelow (midiNote, 128))
        tsf_channel_note_off (asTsf (handle.get()), 0, midiNote);
}

void SoundFontInstance::setSustain (bool down) noexcept
{
    tsf_channel_set_sustain (asTsf (handle.get()), 0, down ? 1 : 0);
}

void SoundFontInstance::pitchWheel (int value14bit) noexcept
{
    tsf_channel_set_pitchwheel (asTsf (handle.get()), 0,
                                juce::jlimit (0, 16383, value14bit));
}

void SoundFontInstance::allSoundOff() noexcept
{
    tsf_channel_sounds_off_all (asTsf (handle.get()), 0);
    tsf_channel_set_sustain (asTsf (handle.get()), 0, 0);
}

void SoundFontInstance::allNotesOff() noexcept
{
    tsf_channel_note_off_all (asTsf (handle.get()), 0);
}

void SoundFontInstance::renderAdd (
    float* left, float* right, int numSamples) noexcept
{
    auto* f = asTsf (handle.get());

    while (numSamples > 0)
    {
        const int chunk = juce::jmin (numSamples, maxRenderBlock);

        // flag_mixing = 0: o TSF sobrescreve o scratch (mais barato que
        // zerar); a soma no bus acontece logo abaixo.
        tsf_render_float (f, scratch.get(), chunk, 0);

        const float* srcL = scratch.get();
        const float* srcR = scratch.get() + chunk;
        for (int i = 0; i < chunk; ++i)
        {
            left[i] += srcL[i];
            right[i] += srcR[i];
        }

        left += chunk;
        right += chunk;
        numSamples -= chunk;
    }
}

bool SoundFontInstance::isQuiet() const noexcept
{
    return tsf_active_voice_count (asTsf (handle.get())) == 0;
}

} // namespace churchsf2
