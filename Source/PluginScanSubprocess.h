#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <mutex>
#include <queue>

// ESCANEAMENTO DE PLUGINS FORA DO PROCESSO.
//
// Ate 31/07/2026 o ChurchDAW varria os plugins dentro do proprio processo.
// Basta UM plugin quebrado para o app inteiro morrer: o Mac do cliente
// gerou este relatorio ao escanear
//
//   Thread 0  Piano V3  ...
//             juce::DescriptionLister::findDescriptionsSlow(...)
//             juce::VST3PluginFormatHeadless::findAllTypesForFile(...)
//             juce::KnownPluginList::scanAndAddFile(...)
//   EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x8
//
// — exatamente o "escaneia pela metade e fecha" relatado. Um crash dentro
// do plugin vira crash do host porque compartilham o mesmo processo.
//
// Agora cada plugin e carregado por um PROCESSO FILHO (o proprio
// executavel, relancado com um UID de canal). Se o plugin explode, morre
// so o filho: o ChurchDAW percebe a conexao caida, marca aquele plugin no
// arquivo "PluginsComProblema.txt" e SEGUE para o proximo. O culto nao
// perde o app por causa de um VST de terceiro.
//
// Estrutura (o mesmo desenho do AudioPluginHost oficial do JUCE):
//   PluginScannerSubprocess  roda no filho e faz o findAllTypesForFile
//   ScannerSuperprocess      lado do pai, conversa com o filho
//   ChurchDAWPluginScanner   KnownPluginList::CustomScanner que liga os dois
namespace ChurchDAWPluginScan
{

// Identificador do canal entre pai e filho. Precisa ser o mesmo dos dois
// lados e nao pode colidir com outro app.
inline constexpr const char* processUID = "churchdaw-plugin-scan";

//==============================================================================
/** Lado FILHO: recebe (formato, arquivo), carrega o plugin e devolve o XML. */
class PluginScannerSubprocess final : private juce::ChildProcessWorker,
                                      private juce::AsyncUpdater
{
public:
    PluginScannerSubprocess()
    {
        juce::addDefaultFormatsToManager (formatManager);
    }

    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override
    {
        if (mb.isEmpty())
            return;

        const std::lock_guard<std::mutex> lock (mutex);

        if (const auto results = doScan (mb); ! results.isEmpty())
        {
            sendResults (results);
        }
        else
        {
            // Formato que exige a thread de mensagens: repete no laco do
            // message thread em vez de responder vazio.
            pendingBlocks.emplace (mb);
            triggerAsyncUpdate();
        }
    }

    void handleConnectionLost() override
    {
        juce::JUCEApplicationBase::quit();
    }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (pendingBlocks.empty())
                return;

            sendResults (doScan (pendingBlocks.front()));
            pendingBlocks.pop();
        }
    }

    juce::OwnedArray<juce::PluginDescription> doScan (
        const juce::MemoryBlock& block)
    {
        juce::MemoryInputStream stream { block, false };
        const auto formatName = stream.readString();
        const auto identifier = stream.readString();

        juce::PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        pd.uniqueId = pd.deprecatedUid = 0;

        auto* matchingFormat = [&]() -> juce::AudioPluginFormat*
        {
            for (auto* format : formatManager.getFormats())
                if (format->getName() == formatName)
                    return format;

            return nullptr;
        }();

        juce::OwnedArray<juce::PluginDescription> results;

        if (matchingFormat != nullptr
             && (juce::MessageManager::getInstance()->isThisTheMessageThread()
                  || matchingFormat->requiresUnblockedMessageThreadDuringCreation (pd)))
        {
            matchingFormat->findAllTypesForFile (results, identifier);
        }

        return results;
    }

    void sendResults (const juce::OwnedArray<juce::PluginDescription>& results)
    {
        juce::XmlElement xml ("LIST");

        for (const auto& desc : results)
            xml.addChildElement (desc->createXml().release());

        const auto str = xml.toString();
        sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });
    }

    std::mutex mutex;
    std::queue<juce::MemoryBlock> pendingBlocks;
    juce::AudioPluginFormatManager formatManager;
};

//==============================================================================
/** Lado PAI: lanca o filho e espera a resposta dele. */
class ScannerSuperprocess final : private juce::ChildProcessCoordinator
{
public:
    // PARAR O PIPE ANTES DE DESTRUIR OS MEMBROS.
    //
    // Sem este destrutor a ordem era: corpo vazio -> destroi mutex/condvar ->
    // so entao ~ChildProcessCoordinator mata a thread do pipe. Entre os dois
    // ultimos passos a thread do pipe ainda esta viva, e um
    // handleMessageFromWorker/handleConnectionLost travaria um std::mutex JA
    // DESTRUIDO.
    //
    // Isso era inofensivo enquanto tudo acontecia serializado na thread de
    // mensagens. Passou a ser corrida de verdade quando a varredura foi para
    // um ThreadPool (allowAsync), porque "superprocess = nullptr" abaixo roda
    // na thread do pool enquanto os callbacks chegam pela thread do pipe.
    ~ScannerSuperprocess() override
    {
        killWorkerProcess();
    }

    ScannerSuperprocess()
    {
        launchWorkerProcess (
            juce::File::getSpecialLocation (juce::File::currentExecutableFile),
            processUID, 0, 0);
    }

    enum class State { timeout, gotResult, connectionLost };

    struct Response
    {
        State state;
        std::unique_ptr<juce::XmlElement> xml;
    };

    Response getResponse()
    {
        std::unique_lock<std::mutex> lock { mutex };

        if (! condvar.wait_for (lock, std::chrono::milliseconds { 50 },
                                [&] { return gotResult || connectionLost; }))
            return { State::timeout, nullptr };

        const auto state = connectionLost ? State::connectionLost
                                          : State::gotResult;
        connectionLost = false;
        gotResult = false;

        return { state, std::move (pluginDescription) };
    }

    using juce::ChildProcessCoordinator::sendMessageToWorker;

private:
    void handleMessageFromWorker (const juce::MemoryBlock& mb) override
    {
        const std::lock_guard<std::mutex> lock { mutex };
        pluginDescription = juce::parseXML (mb.toString());
        gotResult = true;
        condvar.notify_one();
    }

    void handleConnectionLost() override
    {
        // O filho morreu — quase sempre porque o plugin da vez travou.
        const std::lock_guard<std::mutex> lock { mutex };
        connectionLost = true;
        condvar.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condvar;
    std::unique_ptr<juce::XmlElement> pluginDescription;
    bool connectionLost = false;
    bool gotResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScannerSuperprocess)
};

//==============================================================================
/** Componente de sistema da Apple que nao serve para nada aqui.

    O macOS registra dezenas de Audio Units proprios — mixers, panners,
    conversores de formato, unidades de saida. Nenhum deles e instrumento
    ou efeito que um musico va usar no ChurchDAW, e sao justamente os que
    quebram a varredura: o Mac de um cliente parou em "AUVectorPanner" a
    0% em 31/07/2026, com "aufx,rvb2,appl" e "aumx,mxmx,appl" ja marcados
    como "falhou ao iniciar" logo acima. Cada um desses custa ate 90 s de
    prazo antes de ser descartado, e o cliente ve o app travado.

    O filtro e conservador de proposito: so descarta as CATEGORIAS de
    sistema. Audio Units da Apple que fazem som de verdade (DLSMusicDevice,
    AUSampler, que aparecem como "Synths"/"MusicDevices") continuam sendo
    varridos normalmente.
*/
inline bool ehAudioUnitDeSistema (const juce::String& fileOrIdentifier)
{
    if (! fileOrIdentifier.startsWithIgnoreCase ("AudioUnit:"))
        return false;

    // So vale para componentes da propria Apple: o fabricante e o ultimo
    // campo do identificador ("AudioUnit:Mixers/aumx,mxmx,appl").
    if (! fileOrIdentifier.endsWithIgnoreCase (",appl"))
        return false;

    // CATEGORIAS INTEIRAS que nao servem para tocar: mixers e panners de
    // sistema (AUMixer3D, AUMatrixMixer, AUVectorPanner...). "Synths/" fica
    // de fora de proposito — e onde vivem o DLSMusicDevice e o AUSampler,
    // que tocam nota de verdade.
    for (const auto* categoria : { "AudioUnit:Mixers/", "AudioUnit:Panners/" })
        if (fileOrIdentifier.startsWithIgnoreCase (categoria))
            return true;

    // EM "Effects/" E "Generators/" O CORTE E POR SUBTIPO, NAO PELA CATEGORIA.
    //
    // Banir "Effects/" inteiro era desproporcional: aquela categoria guarda
    // AUNBandEQ, AUDynamicsProcessor e AUPeakLimiter — que numa igreja sem
    // nenhum plugin pago sao o UNICO equalizador, compressor e limitador
    // disponiveis no Mac. O que travava era um componente especifico
    // (aufx,rvb2 = AUMatrixReverb, visto no Mac do cliente em 31/07).
    //
    // Aqui ficam so os que abrem rede, motor de fala ou tocam arquivo — os
    // candidatos classicos a travar a varredura, e sem uso ao vivo.
    for (const auto* subtipo : { "aufx,rvb2,",   // AUMatrixReverb: o que travou
                                 "augn,ttsp,",   // AUSpeechSynthesis (fala)
                                 "augn,nrcv,",   // AUNetReceive (rede)
                                 "augn,nsnd,",   // AUNetSend (rede)
                                 "augn,sspl,",   // AUScheduledSoundPlayer
                                 "augn,afpl," }) // AUAudioFilePlayer
        if (fileOrIdentifier.containsIgnoreCase (subtipo))
            return true;

    return false;
}

/** Liga a lista de plugins ao processo filho. */
class ChurchDAWPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    ChurchDAWPluginScanner() = default;

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        if (ehAudioUnitDeSistema (fileOrIdentifier))
        {
            juce::Logger::writeToLog ("PLUGINS: " + fileOrIdentifier
                                      + " e componente de sistema da Apple — ignorado");

            // "false" e o retorno CERTO aqui, apesar de parecer erro.
            //
            // KnownPluginList::scanAndAddFile chama addToBlacklist quando o
            // scanner devolve false (juce_KnownPluginList.cpp:195-196), e o
            // PluginDirectoryScanner so acusa "falhou ao carregar" para quem
            // NAO esta na blacklist (juce_PluginDirectoryScanner.cpp:119-120).
            //
            // Devolver "true" com a lista vazia — que era o que estava aqui —
            // produzia o oposto do pretendido: os componentes da Apple caiam
            // no alerta "estes pareciam plugins, mas nao carregaram" ao fim de
            // TODA varredura, e ainda eram revarridos toda vez, porque nada
            // ficava registrado.
            return false;
        }

        if (addPluginDescriptions (format.getName(), fileOrIdentifier, result))
            return true;

        // Conexao perdida = o filho caiu com o plugin. Descartar o
        // processo faz o proximo plugin comecar num filho novo e limpo,
        // em vez de contaminar a varredura inteira.
        superprocess = nullptr;
        return false;
    }

    void scanFinished() override
    {
        superprocess = nullptr;
    }

private:
    bool addPluginDescriptions (const juce::String& formatName,
                                const juce::String& fileOrIdentifier,
                                juce::OwnedArray<juce::PluginDescription>& result)
    {
        if (superprocess == nullptr)
            superprocess = std::make_unique<ScannerSuperprocess>();

        juce::MemoryBlock block;
        juce::MemoryOutputStream stream { block, true };
        stream.writeString (formatName);
        stream.writeString (fileOrIdentifier);
        stream.flush();

        if (! superprocess->sendMessageToWorker (block))
            return false;

        // PRAZO POR PLUGIN.
        //
        // Sem isto o laco espera para sempre. Um plugin que MORRE derruba
        // o filho e a conexao cai — isso o codigo ja tratava. Mas um
        // plugin que TRAVA (fica girando dentro do proprio carregamento,
        // esperando rede, ou abrindo dialogo invisivel) deixa o filho
        // vivo e mudo: getResponse() devolve timeout indefinidamente e a
        // varredura congela com o app inteiro parado na tela "Procurando
        // plugins...". Foi o que travou o Mac de um cliente em 31/07/2026,
        // e como a varredura nunca terminava os Audio Units — o BigSky
        // dele entre eles — nem chegavam a ser procurados.
        //
        // 90 s cobre com folga um Kontakt grande carregando de disco
        // lento; passou disso, o plugin fica na lista de problemas e a
        // varredura segue para o proximo.
        constexpr int prazoPorPluginMs = 90000;
        const auto limite = juce::Time::getMillisecondCounter()
                          + (juce::uint32) prazoPorPluginMs;

        for (;;)
        {
            if (shouldExit())
                return true;

            if (juce::Time::getMillisecondCounter() > limite)
            {
                juce::Logger::writeToLog (
                    "PLUGINS: " + fileOrIdentifier
                    + " nao respondeu em " + juce::String (prazoPorPluginMs / 1000)
                    + " s — marcado como problematico e ignorado");
                return false;   // entra na lista de problemas do JUCE
            }

            const auto response = superprocess->getResponse();

            if (response.state == ScannerSuperprocess::State::timeout)
                continue;

            if (response.xml != nullptr)
            {
                for (const auto* item : response.xml->getChildIterator())
                {
                    auto desc = std::make_unique<juce::PluginDescription>();

                    if (desc->loadFromXml (*item))
                        result.add (std::move (desc));
                }
            }

            return response.state == ScannerSuperprocess::State::gotResult;
        }
    }

    std::unique_ptr<ScannerSuperprocess> superprocess;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchDAWPluginScanner)
};

}   // namespace ChurchDAWPluginScan
