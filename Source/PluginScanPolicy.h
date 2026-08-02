#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ChurchDAWPluginScan
{

// O bool devolvido por KnownPluginList::CustomScanner NAO significa
// "plugin valido": false significa especificamente "o processo caiu" e o
// JUCE o grava na blacklist persistente. Erros de host, protocolo,
// arquitetura ou uma classe simplesmente rejeitada devem aparecer no
// diagnostico/failedFiles, nunca como um falso "desativado" permanente.
enum class PhaseFinish
{
    result,
    crash,
    timeout,
    cancelled,
    launchFailed,
    sendFailed
};

inline bool deveEntrarNaBlacklistPersistente (PhaseFinish finish)
{
    return finish == PhaseFinish::crash || finish == PhaseFinish::timeout;
}

inline bool precisaRevalidarBlacklistLegada (int semanticsVersion)
{
    // v1 misturava crash de plugin com arquitetura, licenca e falha do host.
    // No scanner isolado v2 e seguro tentar tudo uma vez e deixar apenas
    // crash/timeout reais voltarem para a blacklist.
    return semanticsVersion < 2;
}

// Alguns componentes da Apple fazem parte da infraestrutura do sistema (mixers,
// panners, fala/rede) e nao sao instrumentos ou efeitos selecionaveis no
// ChurchDAW. Eles devem ser removidos da lista de candidatos ANTES do scanner:
// retornar false de um CustomScanner faz o JUCE grava-los na blacklist.
inline bool ehAudioUnitDeSistema (const juce::String& fileOrIdentifier)
{
    if (! fileOrIdentifier.startsWithIgnoreCase ("AudioUnit:")
         || ! fileOrIdentifier.endsWithIgnoreCase (",appl"))
        return false;

    for (const auto* category : { "AudioUnit:Mixers/", "AudioUnit:Panners/" })
        if (fileOrIdentifier.startsWithIgnoreCase (category))
            return true;

    // Efeitos musicais (inclusive Reverb2), EQ/dynamics/limiter e instrumentos
    // da Apple ficam permitidos. So geradores de infraestrutura entram aqui.
    for (const auto* subtype : { "augn,ttsp,", "augn,nrcv,",
                                 "augn,nsnd,", "augn,sspl,", "augn,afpl," })
        if (fileOrIdentifier.containsIgnoreCase (subtype))
            return true;

    return false;
}

inline juce::StringArray filtrarAudioUnitsHospedaveis (
    const juce::StringArray& identifiers)
{
    juce::StringArray result;

    for (const auto& identifier : identifiers)
        if (! ehAudioUnitDeSistema (identifier))
            result.addIfNotAlreadyThere (identifier);

    return result;
}

// Migra somente falsos positivos criados pela politica antiga. Blacklists de
// plugins de terceiros continuam intactas para que crash/timeout reais nao
// sejam mascarados.
inline int removerAudioUnitsDeSistemaDaBlacklist (juce::KnownPluginList& list)
{
    const auto existing = list.getBlacklistedFiles();
    int removed = 0;

    for (const auto& identifier : existing)
        if (ehAudioUnitDeSistema (identifier))
        {
            list.removeFromBlacklist (identifier);
            ++removed;
        }

    return removed;
}

inline int removerAudioUnitsDeSistemaDoDeadMansPedal (const juce::File& file)
{
    if (! file.existsAsFile())
        return 0;

    juce::StringArray lines;
    file.readLines (lines);
    lines.removeEmptyStrings();

    const auto filtered = filtrarAudioUnitsHospedaveis (lines);
    const int removed = lines.size() - filtered.size();

    if (removed > 0
         && ! file.replaceWithText (filtered.joinIntoString ("\n"), true, true))
        return -1;

    return removed;
}

} // namespace ChurchDAWPluginScan
