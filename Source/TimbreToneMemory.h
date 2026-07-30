#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

// Memoria dos controles que pertencem ao SOM escolhido, e nao ao fader do
// canal. Assim dois pianos no mesmo canal podem conservar envelopes/EQ
// diferentes sem transformar a sessao em um banco de presets sem limite.
namespace ChurchDAWTimbreTone
{
struct Settings
{
    float attackSeconds = 0.003f;
    float releaseSeconds = 0.55f;
    float lowDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;

    // Estes controles ja faziam parte da cor do timbre antes do novo EQ. Eles
    // tambem precisam acompanhar A -> B -> A; caso contrario um pad poderia
    // herdar o filtro/Auto EQ/Rotary do piano ou orgao que estava no canal.
    float legacyLowCut = 0.0f;
    float legacyHighCut = 0.0f;
    bool autoEq = false;
    // O Store e neutro quanto a identidade. Sample/VST usa a ordem legada do
    // seletor (0 Slow, 1 Stop, 2 Fast). Church Organ grava a ordem canonica do
    // motor (0 Stop, 1 Slow, 2 Fast, 3 Brake); o host converte ao aplicar. O
    // valor 3 jamais e aplicado a um seletor legado.
    int rotaryMode = 1;
    float rotaryAmount = 0.0f;
};

inline Settings bounded (Settings value) noexcept
{
    const auto finiteOr = [] (float candidate, float fallback) noexcept
    {
        return std::isfinite (candidate) ? candidate : fallback;
    };

    value.attackSeconds = juce::jlimit (
        0.001f, 2.0f, finiteOr (value.attackSeconds, 0.003f));
    value.releaseSeconds = juce::jlimit (
        0.05f, 12.0f, finiteOr (value.releaseSeconds, 0.55f));
    value.lowDb = juce::jlimit (
        -12.0f, 12.0f, finiteOr (value.lowDb, 0.0f));
    value.midDb = juce::jlimit (
        -12.0f, 12.0f, finiteOr (value.midDb, 0.0f));
    value.highDb = juce::jlimit (
        -12.0f, 12.0f, finiteOr (value.highDb, 0.0f));
    value.legacyLowCut = juce::jlimit (
        0.0f, 1.0f, finiteOr (value.legacyLowCut, 0.0f));
    value.legacyHighCut = juce::jlimit (
        0.0f, 1.0f, finiteOr (value.legacyHighCut, 0.0f));
    value.rotaryMode = juce::jlimit (0, 3, value.rotaryMode);
    value.rotaryAmount = juce::jlimit (
        0.0f, 1.0f, finiteOr (value.rotaryAmount, 0.0f));
    return value;
}

template <int NumLayers, int MaxEntriesPerLayer = 96>
class Store
{
public:
    static_assert (NumLayers > 0);
    static_assert (MaxEntriesPerLayer > 0);

    void clear()
    {
        const juce::ScopedLock lock (mutex);

        for (auto& list : entries)
            list.clearQuick();
    }

    void remember (int layer, const juce::String& rawLabel,
                   const juce::String& rawLibrary,
                   Settings settings)
    {
        if (! juce::isPositiveAndBelow (layer, NumLayers))
            return;

        const auto label = cleanLabel (rawLabel);

        if (label.isEmpty())
            return;

        Entry entry;
        entry.label = label;
        entry.library = cleanLibrary (rawLibrary);
        entry.settings = bounded (settings);

        const juce::ScopedLock lock (mutex);
        upsertUnlocked (layer, std::move (entry));
    }

    bool recall (int layer, const juce::String& rawLabel,
                 const juce::String& rawLibrary,
                 Settings& result)
    {
        if (! juce::isPositiveAndBelow (layer, NumLayers))
            return false;

        const auto label = cleanLabel (rawLabel);
        const auto library = cleanLibrary (rawLibrary);

        if (label.isEmpty())
            return false;

        const juce::ScopedLock lock (mutex);
        auto& list = entries[layer];

        // Primeiro, identidade completa. A mesma etiqueta pode existir em dois
        // packs diferentes e nao deve compartilhar ajustes por acidente.
        for (int i = list.size(); --i >= 0;)
            if (sameText (list.getReference (i).label, label)
                 && sameText (list.getReference (i).library, library))
            {
                result = list.getReference (i).settings;
                return true;
            }

        // Estados antigos podiam guardar a etiqueta sem biblioteca. Ao abrir
        // esse mesmo timbre num pack moderno, recupere o perfil legado; assim
        // que o usuario sair dele, remember() cria a identidade completa.
        if (library.isNotEmpty())
            for (int i = list.size(); --i >= 0;)
                if (list.getReference (i).library.isEmpty()
                     && sameText (
                            list.getReference (i).label, label))
                {
                    auto promoted = list.getReference (i);
                    promoted.library = library;
                    result = promoted.settings;
                    list.remove (i);
                    upsertUnlocked (layer, std::move (promoted));
                    return true;
                }

        // Consulta tambem legada (sem lib): o item mais recente com a mesma
        // etiqueta e a unica cura possivel.
        if (library.isEmpty())
            for (int i = list.size(); --i >= 0;)
                if (sameText (list.getReference (i).label, label))
                {
                    result = list.getReference (i).settings;
                    return true;
                }

        return false;
    }

    int size (int layer) const
    {
        if (! juce::isPositiveAndBelow (layer, NumLayers))
            return 0;

        const juce::ScopedLock lock (mutex);
        return entries[layer].size();
    }

    void writeToXml (juce::XmlElement& parent) const
    {
        const juce::ScopedLock lock (mutex);

        for (int layer = 0; layer < NumLayers; ++layer)
            for (const auto& entry : entries[layer])
            {
                auto* tone = parent.createNewChildElement ("TONE");
                tone->setAttribute ("layer", layer);
                tone->setAttribute ("timbre", entry.label);
                tone->setAttribute ("lib", entry.library);
                tone->setAttribute (
                    "attack", (double) entry.settings.attackSeconds);
                tone->setAttribute (
                    "release", (double) entry.settings.releaseSeconds);
                tone->setAttribute ("eqLow", (double) entry.settings.lowDb);
                tone->setAttribute ("eqMid", (double) entry.settings.midDb);
                tone->setAttribute ("eqHigh", (double) entry.settings.highDb);
                tone->setAttribute (
                    "lcut", (double) entry.settings.legacyLowCut);
                tone->setAttribute (
                    "hcut", (double) entry.settings.legacyHighCut);
                tone->setAttribute ("aeq", entry.settings.autoEq);
                tone->setAttribute ("rotMode", entry.settings.rotaryMode);
                tone->setAttribute (
                    "rotAmt", (double) entry.settings.rotaryAmount);
            }
    }

    void readFromXml (const juce::XmlElement& parent)
    {
        const juce::ScopedLock lock (mutex);

        for (auto& list : entries)
            list.clearQuick();

        int accepted = 0;
        int visited = 0;
        constexpr int maxVisited =
            NumLayers * MaxEntriesPerLayer * 4 + 64;

        for (const auto* tone : parent.getChildIterator())
        {
            if (++visited > maxVisited)
                break;

            if (! tone->hasTagName ("TONE"))
                continue;

            const int layer = tone->getIntAttribute ("layer", -1);

            if (! juce::isPositiveAndBelow (layer, NumLayers))
                continue;

            Entry entry;
            entry.label = cleanLabel (
                tone->getStringAttribute ("timbre"));
            entry.library = cleanLibrary (
                tone->getStringAttribute ("lib"));

            if (entry.label.isEmpty())
                continue;

            const auto safeDouble = [tone] (const char* name,
                                            double fallback) noexcept
            {
                const double value =
                    tone->getDoubleAttribute (name, fallback);
                return std::isfinite (value) ? value : fallback;
            };

            entry.settings.attackSeconds = (float) safeDouble (
                "attack", 0.003);
            entry.settings.releaseSeconds = (float) safeDouble (
                "release", 0.55);
            entry.settings.lowDb = (float) safeDouble ("eqLow", 0.0);
            entry.settings.midDb = (float) safeDouble ("eqMid", 0.0);
            entry.settings.highDb = (float) safeDouble ("eqHigh", 0.0);
            entry.settings.legacyLowCut = (float) safeDouble ("lcut", 0.0);
            entry.settings.legacyHighCut = (float) safeDouble ("hcut", 0.0);
            entry.settings.autoEq =
                tone->getBoolAttribute ("aeq", false);
            entry.settings.rotaryMode =
                tone->getIntAttribute ("rotMode", 1);
            entry.settings.rotaryAmount = (float) safeDouble (
                "rotAmt", 0.0);
            entry.settings = bounded (entry.settings);

            upsertUnlocked (layer, std::move (entry));

            // Um XML hostil nao pode transformar a restauracao em trabalho
            // ilimitado. O writer oficial nunca produz mais do que este total.
            if (++accepted >= NumLayers * MaxEntriesPerLayer)
                break;
        }
    }

private:
    struct Entry
    {
        juce::String label;
        juce::String library;
        Settings settings;
    };

    static juce::String cleanLabel (const juce::String& text)
    {
        return text.trim().substring (0, 160);
    }

    static juce::String cleanLibrary (const juce::String& text)
    {
        // Identificadores VST/AU podem ser maiores do que nomes de packs.
        return text.trim().substring (0, 1024);
    }

    static bool sameText (const juce::String& a,
                          const juce::String& b) noexcept
    {
        return a.equalsIgnoreCase (b);
    }

    void upsertUnlocked (int layer, Entry entry)
    {
        auto& list = entries[layer];

        for (int i = list.size(); --i >= 0;)
            if (sameText (list.getReference (i).label, entry.label)
                 && sameText (list.getReference (i).library, entry.library))
            {
                // Move para o fim: o limite remove primeiro o item menos
                // recentemente usado, nao um timbre que acabou de ser editado.
                list.remove (i);
                break;
            }

        while (list.size() >= MaxEntriesPerLayer)
            list.remove (0);

        list.add (std::move (entry));
    }

    mutable juce::CriticalSection mutex;
    juce::Array<Entry> entries[NumLayers];
};
}
