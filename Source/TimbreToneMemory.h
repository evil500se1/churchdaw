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
    // Revisao do perfil publicado pelo pack quando estes valores foram
    // editados. Um sidecar mais novo pode migrar apenas memorias antigas e,
    // depois disso, as escolhas do musico voltam a ter precedencia normal.
    int profileRevision = 0;
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

    bool nordFilterOn = false;
    int nordFilterType = 1;
    float nordFilterCutoffHz = 18000.0f;
    float nordFilterResonance = 0.0f;
    int nordFilterKeyTrack = 0;
    int nordFilterDrive = 0;
    bool nordFilterBright = false;
    float nordFilterEnvAmount = 0.0f;
    float nordFilterEnvAttackSeconds = 0.001f;
    float nordFilterEnvDecaySeconds = 10.0f;
    float nordFilterEnvReleaseSeconds = 0.55f;
    bool nordFilterVelocity = false;
    bool nordAmpEqOn = false;
    float nordAmpBassDb = 0.0f;
    float nordAmpMidDb = 0.0f;
    float nordAmpMidHz = 850.0f;
    float nordAmpTrebleDb = 0.0f;
};

inline Settings bounded (Settings value) noexcept
{
    const auto finiteOr = [] (float candidate, float fallback) noexcept
    {
        return std::isfinite (candidate) ? candidate : fallback;
    };

    value.profileRevision = juce::jlimit (
        0, 1000000, value.profileRevision);
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
    value.nordFilterType = juce::jlimit (0, 5, value.nordFilterType);
    value.nordFilterCutoffHz = juce::jlimit (
        20.0f, 20000.0f,
        finiteOr (value.nordFilterCutoffHz, 18000.0f));
    value.nordFilterResonance = juce::jlimit (
        0.0f, 1.0f,
        finiteOr (value.nordFilterResonance, 0.0f));
    value.nordFilterKeyTrack = juce::jlimit (
        0, 3, value.nordFilterKeyTrack);
    value.nordFilterDrive = juce::jlimit (
        0, 3, value.nordFilterDrive);
    value.nordFilterEnvAmount = juce::jlimit (
        -1.0f, 1.0f,
        finiteOr (value.nordFilterEnvAmount, 0.0f));
    value.nordFilterEnvAttackSeconds = juce::jlimit (
        0.001f, 10.0f,
        finiteOr (value.nordFilterEnvAttackSeconds, 0.001f));
    value.nordFilterEnvDecaySeconds = juce::jlimit (
        0.001f, 10.0f,
        finiteOr (value.nordFilterEnvDecaySeconds, 10.0f));
    value.nordFilterEnvReleaseSeconds = juce::jlimit (
        0.01f, 10.0f,
        finiteOr (value.nordFilterEnvReleaseSeconds, 0.55f));
    value.nordAmpBassDb = juce::jlimit (
        -15.0f, 15.0f, finiteOr (value.nordAmpBassDb, 0.0f));
    value.nordAmpMidDb = juce::jlimit (
        -15.0f, 15.0f, finiteOr (value.nordAmpMidDb, 0.0f));
    value.nordAmpMidHz = juce::jlimit (
        200.0f, 8000.0f, finiteOr (value.nordAmpMidHz, 850.0f));
    value.nordAmpTrebleDb = juce::jlimit (
        -15.0f, 15.0f, finiteOr (value.nordAmpTrebleDb, 0.0f));
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
                    "profileRev", entry.settings.profileRevision);
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
                tone->setAttribute ("nfOn", entry.settings.nordFilterOn);
                tone->setAttribute ("nfType", entry.settings.nordFilterType);
                tone->setAttribute (
                    "nfCut", (double) entry.settings.nordFilterCutoffHz);
                tone->setAttribute (
                    "nfRes", (double) entry.settings.nordFilterResonance);
                tone->setAttribute (
                    "nfTrack", entry.settings.nordFilterKeyTrack);
                tone->setAttribute (
                    "nfDrive", entry.settings.nordFilterDrive);
                tone->setAttribute (
                    "nfBright", entry.settings.nordFilterBright);
                tone->setAttribute (
                    "nfEnvAmt", (double) entry.settings.nordFilterEnvAmount);
                tone->setAttribute (
                    "nfEnvA", (double) entry.settings.nordFilterEnvAttackSeconds);
                tone->setAttribute (
                    "nfEnvD", (double) entry.settings.nordFilterEnvDecaySeconds);
                tone->setAttribute (
                    "nfEnvR", (double) entry.settings.nordFilterEnvReleaseSeconds);
                tone->setAttribute (
                    "nfVel", entry.settings.nordFilterVelocity);
                tone->setAttribute (
                    "nEqOn", entry.settings.nordAmpEqOn);
                tone->setAttribute (
                    "nEqBass", (double) entry.settings.nordAmpBassDb);
                tone->setAttribute (
                    "nEqMid", (double) entry.settings.nordAmpMidDb);
                tone->setAttribute (
                    "nEqMidHz", (double) entry.settings.nordAmpMidHz);
                tone->setAttribute (
                    "nEqTreble", (double) entry.settings.nordAmpTrebleDb);
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
            entry.settings.profileRevision = juce::jlimit (
                0, 1000000,
                tone->getIntAttribute ("profileRev", 0));
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
            entry.settings.nordFilterOn =
                tone->getBoolAttribute ("nfOn", false);
            entry.settings.nordFilterType =
                tone->getIntAttribute ("nfType", 1);
            entry.settings.nordFilterCutoffHz = (float) safeDouble (
                "nfCut", 18000.0);
            entry.settings.nordFilterResonance = (float) safeDouble (
                "nfRes", 0.0);
            entry.settings.nordFilterKeyTrack =
                tone->getIntAttribute ("nfTrack", 0);
            entry.settings.nordFilterDrive =
                tone->getIntAttribute ("nfDrive", 0);
            entry.settings.nordFilterBright =
                tone->getBoolAttribute ("nfBright", false);
            entry.settings.nordFilterEnvAmount = (float) safeDouble (
                "nfEnvAmt", 0.0);
            entry.settings.nordFilterEnvAttackSeconds = (float) safeDouble (
                "nfEnvA", 0.001);
            entry.settings.nordFilterEnvDecaySeconds = (float) safeDouble (
                "nfEnvD", 10.0);
            entry.settings.nordFilterEnvReleaseSeconds = (float) safeDouble (
                "nfEnvR", 0.55);
            entry.settings.nordFilterVelocity =
                tone->getBoolAttribute ("nfVel", false);
            entry.settings.nordAmpEqOn =
                tone->getBoolAttribute ("nEqOn", false);
            entry.settings.nordAmpBassDb = (float) safeDouble (
                "nEqBass", 0.0);
            entry.settings.nordAmpMidDb = (float) safeDouble (
                "nEqMid", 0.0);
            entry.settings.nordAmpMidHz = (float) safeDouble (
                "nEqMidHz", 850.0);
            entry.settings.nordAmpTrebleDb = (float) safeDouble (
                "nEqTreble", 0.0);
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
