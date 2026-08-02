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
    // O contrato do JUCE permite devolver false somente quando o processo do
    // plugin CAIU. Timeout prova apenas que a tentativa nao terminou no prazo:
    // primeira autorizacao, licenca ou inicializacao lenta continuam retryable.
    return finish == PhaseFinish::crash;
}

inline bool ehTentativaInconclusiva (PhaseFinish finish)
{
    return finish == PhaseFinish::timeout
        || finish == PhaseFinish::launchFailed
        || finish == PhaseFinish::sendFailed;
}

// Resultado agregado de TODAS as classes descobertas dentro de um candidato.
// Um arquivo VST3 pode ser um shell com varias classes; a blacklist do JUCE,
// entretanto, e por arquivo. Portanto uma classe que caiu nao pode esconder
// uma irma valida, incompatível com a arquitetura atual ou apenas inconclusiva.
struct CandidateValidationSummary
{
    int loaded = 0;
    int fatal = 0;
    int retryableOrSkipped = 0;
};

inline bool deveEntrarNaBlacklistPersistente (
    const CandidateValidationSummary& summary)
{
    // false no CustomScanner significa "o ARQUIVO caiu". Isso so foi provado
    // quando existe ao menos uma tentativa fatal e nao existe uma unica classe
    // sobrevivente, retryable/inconclusiva ou pulada por arquitetura.
    return summary.loaded == 0
        && summary.fatal > 0
        && summary.retryableOrSkipped == 0;
}

inline bool precisaRevalidarBlacklistLegada (int semanticsVersion)
{
    // v1 misturava crash com arquitetura/licenca/host; v2 ainda transformava
    // timeout em blacklist. No v3 somente um crash observado volta a lista
    // persistente, e todos os estados antigos sao revalidados uma vez.
    return semanticsVersion < 3;
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
// plugins de terceiros continuam intactas para que crashes reais nao
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
