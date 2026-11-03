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

// Mantem as descricoes aprovadas no pai enquanto as demais classes do mesmo
// shell continuam sendo provadas em filhos descartaveis. Uma queda posterior
// nunca pode limpar a classe que ja terminou a validacao.
class CandidateValidationAccumulator
{
public:
    explicit CandidateValidationAccumulator (
        juce::OwnedArray<juce::PluginDescription>& destinationToUse)
        : destination (destinationToUse)
    {
    }

    void addLoaded (const juce::PluginDescription& description)
    {
        destination.add (new juce::PluginDescription (description));
        ++summary.loaded;
    }

    void addFailure (PhaseFinish finish, bool crashWasInsidePluginClass = true)
    {
        if (crashWasInsidePluginClass
             && deveEntrarNaBlacklistPersistente (finish))
            ++summary.fatal;
        else
            ++summary.retryableOrSkipped;
    }

    void addRetryableOrSkipped() { ++summary.retryableOrSkipped; }

    const CandidateValidationSummary& getSummary() const noexcept
    {
        return summary;
    }

private:
    juce::OwnedArray<juce::PluginDescription>& destination;
    CandidateValidationSummary summary;
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

// KnownPluginList::isBlacklisted existia apenas na copia local do JUCE usada
// durante parte do desenvolvimento. A API publica e portavel do JUCE 9 e o
// snapshot devolvido por getBlacklistedFiles(). Centralizar a consulta aqui
// impede que o aplicativo dependa de um patch privado do framework.
inline bool estaNaBlacklist (
    const juce::KnownPluginList& list,
    const juce::String& identifier,
    bool ignoreCase = true)
{
    return list.getBlacklistedFiles().contains (identifier, ignoreCase);
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

//==============================================================================
// Papel do plugin
//
// Alguns VST3 de terceiros criam e tocam normalmente, mas publicam
// isInstrument=false no PluginDescription. Confiar somente nesse bit faz o
// instrumento sumir da lista de timbres e aparecer como efeito de canal. A
// categoria da factory e as capacidades observadas na instancia validada sao
// evidencias complementares, sem promover efeitos MIDI comuns a instrumentos.
struct RuntimePluginRoleEvidence
{
    bool acceptsMidi = false;
    int inputChannels = -1;
    int outputChannels = -1;
};

inline bool categoriaDeclaraInstrumento (const juce::String& category)
{
    juce::StringArray tokens;
    tokens.addTokens (category, "|,;/", "\"'");

    for (auto token : tokens)
    {
        token = token.trim().toLowerCase();

        for (const auto* instrumentToken : {
                 "instrument", "instruments", "synth", "synths",
                 "sampler", "musicdevice", "music device", "vsti" })
            if (token == instrumentToken)
                return true;
    }

    return false;
}

inline bool descricaoDeclaraInstrumento (
    const juce::PluginDescription& description)
{
    return description.isInstrument
        || categoriaDeclaraInstrumento (description.category);
}

inline bool deveSerTratadoComoInstrumento (
    const juce::PluginDescription& description,
    const RuntimePluginRoleEvidence& runtime)
{
    if (descricaoDeclaraInstrumento (description))
        return true;

    // Esta e a assinatura estrutural segura de um gerador musical: recebe
    // notas, nao recebe audio e produz audio. Um compressor/delay que aceita
    // MIDI continua efeito porque possui entrada de audio; um gerador de teste
    // sem MIDI tambem continua efeito.
    return runtime.acceptsMidi
        && runtime.inputChannels == 0
        && runtime.outputChannels > 0;
}

inline bool normalizarPapelDoPluginValidado (
    const juce::PluginDescription& discovered,
    juce::PluginDescription& validated,
    const RuntimePluginRoleEvidence& runtime)
{
    bool changed = false;

    if (validated.category.trim().isEmpty()
         && discovered.category.trim().isNotEmpty())
    {
        validated.category = discovered.category;
        changed = true;
    }

    juce::PluginDescription combined (validated);
    combined.isInstrument = validated.isInstrument
                         || discovered.isInstrument;

    if (! categoriaDeclaraInstrumento (combined.category)
         && categoriaDeclaraInstrumento (discovered.category))
        combined.category = discovered.category;

    const bool instrument = deveSerTratadoComoInstrumento (
        combined, runtime);

    if (validated.isInstrument != instrument)
    {
        validated.isInstrument = instrument;
        changed = true;
    }

    if (runtime.inputChannels >= 0
         && validated.numInputChannels != runtime.inputChannels)
    {
        validated.numInputChannels = runtime.inputChannels;
        changed = true;
    }

    if (runtime.outputChannels >= 0
         && validated.numOutputChannels != runtime.outputChannels)
    {
        validated.numOutputChannels = runtime.outputChannels;
        changed = true;
    }

    return changed;
}

// Corrige listas antigas quando a propria categoria ja continha a informacao,
// mas o bool foi gravado incorretamente. Casos ambiguos continuam dependendo
// da nova validacao em processo isolado, que possui a evidencia de runtime.
inline int normalizarPapeisDaListaConhecida (juce::KnownPluginList& list)
{
    const auto existing = list.getTypes();
    int changed = 0;

    for (auto description : existing)
        if (! description.isInstrument
             && categoriaDeclaraInstrumento (description.category))
        {
            const auto previous = description;
            description.isInstrument = true;
            list.removeType (previous);
            list.addType (description);
            ++changed;
        }

    return changed;
}

//==============================================================================
// Pastas de varredura
//
// Builds antigos so criavam os caminhos padrao quando nao havia nenhuma
// configuracao salva. Depois que o usuario adicionava uma pasta personalizada,
// novos caminhos oficiais (inclusive LOCALAPPDATA/VST3_PATH) nunca entravam na
// busca. O modelo abaixo permite mesclar os defaults a cada abertura sem
// reativar uma pasta que o usuario desmarcou.
// Audio Units are enumerated from the OS registry. An empty/missing VST
// folder must not prevent a Mac with only AU plugins from being scanned.
inline bool podeIniciarBusca (juce::AudioPluginFormatManager& formats,
                              int existingSearchFolders)
{
    if (existingSearchFolders > 0)
        return true;

    for (auto* format : formats.getFormats())
        if (format != nullptr && format->canScanForPlugins()
             && format->getName().containsIgnoreCase ("AudioUnit"))
            return true;

    return false;
}

enum class PluginScanLocationKind
{
    system = 0,
    custom = 1,
    downloads = 2
};

struct PluginScanLocation
{
    juce::File folder;
    PluginScanLocationKind kind = PluginScanLocationKind::custom;
    bool enabled = true;
};

inline juce::String caminhoCanonicoDeScan (const juce::File& folder)
{
    auto path = folder.getFullPathName().trim().replaceCharacter ('\\', '/');

    while (path.length() > 1 && path.endsWithChar ('/'))
        path = path.dropLastCharacters (1);

   #if JUCE_WINDOWS || JUCE_MAC
    path = path.toLowerCase();
   #endif

    return path;
}

inline bool mesmaPastaDeScan (const juce::File& lhs,
                              const juce::File& rhs)
{
    const auto left = caminhoCanonicoDeScan (lhs);
    return left.isNotEmpty() && left == caminhoCanonicoDeScan (rhs);
}

inline bool adicionarPastaDeScanSeAusente (
    juce::Array<PluginScanLocation>& locations,
    const juce::File& folder,
    PluginScanLocationKind kind,
    bool enabled = true)
{
    if (folder == juce::File{}
         || folder.getFullPathName().trim().isEmpty())
        return false;

    for (const auto& location : locations)
        if (mesmaPastaDeScan (location.folder, folder))
            return false;

    locations.add ({ folder, kind, enabled });
    return true;
}

inline int mesclarPastasPadraoDePlugins (
    juce::Array<PluginScanLocation>& locations,
    juce::AudioPluginFormatManager& formats,
    bool includeDownloads)
{
    int added = 0;

    for (auto* format : formats.getFormats())
        if (format != nullptr && format->canScanForPlugins())
        {
            const auto defaults = format->getDefaultLocationsToSearch();

            for (int i = 0; i < defaults.getNumPaths(); ++i)
                added += adicionarPastaDeScanSeAusente (
                    locations, defaults[i],
                    PluginScanLocationKind::system) ? 1 : 0;
        }

   #if JUCE_WINDOWS
    // Nao derive LOCALAPPDATA de USERPROFILE: perfis redirecionados e algumas
    // contas corporativas usam volumes distintos. Os dois fallbacks abaixo
    // tambem protegem hosts JUCE antigos que nao retornavam todos os defaults.
    const auto localAppData = juce::SystemStats::getEnvironmentVariable (
        "LOCALAPPDATA",
        juce::File::getSpecialLocation (
            juce::File::windowsLocalAppData).getFullPathName());
    added += adicionarPastaDeScanSeAusente (
        locations,
        juce::File (localAppData).getChildFile (
            "Programs\\Common\\VST3"),
        PluginScanLocationKind::system) ? 1 : 0;

    const auto commonProgramFiles =
        juce::SystemStats::getEnvironmentVariable (
            "CommonProgramFiles",
            juce::File::getSpecialLocation (
                juce::File::windowsProgramFilesCommon).getFullPathName());
    added += adicionarPastaDeScanSeAusente (
        locations,
        juce::File (commonProgramFiles).getChildFile ("VST3"),
        PluginScanLocationKind::system) ? 1 : 0;

   #if JUCE_PLUGINHOST_VST
    // VST2 nao definiu uma unica pasta oficial no Windows. Estes sao os
    // locais 64-bit usados pelos instaladores mais comuns; pastas x86 ficam de
    // fora porque DLL de 32 bits nao pode ser carregada pelo ChurchDAW 64-bit.
    const auto programFiles = juce::SystemStats::getEnvironmentVariable (
        "ProgramFiles",
        juce::File::getSpecialLocation (
            juce::File::windowsProgramFiles).getFullPathName());
    const auto programFilesFolder = juce::File (programFiles);
    added += adicionarPastaDeScanSeAusente (
        locations, programFilesFolder.getChildFile ("VstPlugins"),
        PluginScanLocationKind::system) ? 1 : 0;
    added += adicionarPastaDeScanSeAusente (
        locations,
        programFilesFolder.getChildFile ("Steinberg\\VstPlugins"),
        PluginScanLocationKind::system) ? 1 : 0;
    added += adicionarPastaDeScanSeAusente (
        locations, juce::File (commonProgramFiles).getChildFile ("VST2"),
        PluginScanLocationKind::system) ? 1 : 0;
    added += adicionarPastaDeScanSeAusente (
        locations,
        juce::File (localAppData).getChildFile ("Programs\\Common\\VST2"),
        PluginScanLocationKind::system) ? 1 : 0;

    const juce::FileSearchPath vstEnvironmentPaths (
        juce::SystemStats::getEnvironmentVariable ("VST_PATH", {}));
    for (int i = 0; i < vstEnvironmentPaths.getNumPaths(); ++i)
        added += adicionarPastaDeScanSeAusente (
            locations, vstEnvironmentPaths[i],
            PluginScanLocationKind::system) ? 1 : 0;
   #endif
   #endif

    if (includeDownloads)
        added += adicionarPastaDeScanSeAusente (
            locations,
            juce::File::getSpecialLocation (
                juce::File::userHomeDirectory).getChildFile ("Downloads"),
            PluginScanLocationKind::downloads) ? 1 : 0;

    return added;
}

} // namespace ChurchDAWPluginScan
