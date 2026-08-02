#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginScanDiagnostics.h"
#include "PluginScanPolicy.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

// Scanner ChurchDAW v2
//
// Cada candidato comum passa por discovery e depois por um processo de
// validacao para cada descricao. VST3 usa uma regra ainda mais estreita:
//   1. inventory: abre apenas factory/countClasses
//   2. class validation: UM filho novo por indice da factory, do getClassInfo
//      ate create/initialize/activate/release/unload
//
// A separacao e importante para shells VST3: se uma classe do bundle quebrar,
// as descricoes ja descobertas continuam no pai e cada classe restante ganha
// um filho novo. Nenhum estado de um fornecedor chega ao plugin seguinte.
namespace ChurchDAWPluginScan
{

inline constexpr const char* processUID = "churchdaw-plugin-scan";

//==============================================================================
class PluginScannerSubprocess final : private juce::ChildProcessWorker,
                                      private juce::AsyncUpdater
{
public:
    PluginScannerSubprocess()
    {
        CrashCapture::install();
    }

    ~PluginScannerSubprocess() override
    {
        juce::PluginHostDiagnostics::clearCallback();

        if (requestThread != nullptr)
            requestThread->stopThread (1000);
    }

    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    class RequestThread final : public juce::Thread
    {
    public:
        RequestThread (PluginScannerSubprocess& ownerToUse,
                       ScanRequest requestToUse)
            : juce::Thread ("ChurchDAW AU scan"), owner (ownerToUse),
              request (std::move (requestToUse))
        {
        }

        void run() override { owner.executeAndSend (request); }

    private:
        PluginScannerSubprocess& owner;
        ScanRequest request;
    };

    void handleMessageFromCoordinator (const juce::MemoryBlock& block) override
    {
        if (block.isEmpty())
            return;

        {
            const std::lock_guard<std::mutex> lock (queueMutex);
            pendingBlocks.emplace (block);
        }

        // Nunca carregue codigo de fornecedor na thread do pipe. A message
        // thread atende VST3/AUv2; AUv3 usa uma auxiliar para que o callback
        // assincrono da Apple continue sendo bombeado pela message thread.
        triggerAsyncUpdate();
    }

    void handleConnectionLost() override
    {
        juce::JUCEApplicationBase::quit();
    }

    void handleAsyncUpdate() override
    {
        for (;;)
        {
            juce::MemoryBlock block;

            {
                const std::lock_guard<std::mutex> lock (queueMutex);

                if (pendingBlocks.empty())
                    return;

                block = pendingBlocks.front();
                pendingBlocks.pop();
            }

            ScanRequest request;
            juce::String protocolError;

            if (! deserialiseRequest (block, request, protocolError))
            {
                auto result = makeBaseResult (request);
                setFailure (*result, "host_internal_error", "protocol",
                            "invalid_request", protocolError);
                sendXml (*result);
                continue;
            }

            beginDiagnostics (request);

            if (! formatsRegistered)
            {
                juce::PluginHostDiagnostics::ScopedCall registerFormats (
                    "factory", "host addDefaultFormatsToManager", true);
                juce::addDefaultFormatsToManager (formatManager);
                formatsRegistered = true;
                registerFormats.finish (formatManager.getNumFormats() > 0,
                                        formatManager.getNumFormats());
            }

            auto* format = findFormat (request.format);

            if (format == nullptr)
            {
                auto result = makeBaseResult (request);
                setFailure (*result, "host_internal_error", "factory",
                            "format_not_registered",
                            "format not registered in scanner worker");
                finishDiagnostics();
                sendXml (*result);
                continue;
            }

            juce::PluginDescription hint;
            hint.fileOrIdentifier = request.identifier;
            hint.uniqueId = hint.deprecatedUid = 0;

            if (auto xml = juce::parseXML (request.descriptionXml))
                hint.loadFromXml (*xml);

            if (format->requiresUnblockedMessageThreadDuringCreation (hint))
            {
                requestThread = std::make_unique<RequestThread> (*this, request);

                if (! requestThread->startThread())
                {
                    auto result = makeBaseResult (request);
                    setFailure (*result, "host_internal_error", "worker_startup",
                                "thread_start_failed",
                                "could not start asynchronous AU scan thread");
                    finishDiagnostics();
                    sendXml (*result);
                }

                return;
            }

            executeAndSend (request);
        }
    }

    juce::AudioPluginFormat* findFormat (const juce::String& name)
    {
        for (auto* format : formatManager.getFormats())
            if (format != nullptr && format->getName() == name)
                return format;

        return nullptr;
    }

    void beginDiagnostics (const ScanRequest& request)
    {
        CrashCapture::setTarget (request.token);
        CrashCapture::setActiveCall ("worker_startup", "request accepted");

        {
            const std::lock_guard<std::mutex> lock (eventMutex);
            lastCriticalFailure.reset();
        }

        juce::PluginHostDiagnostics::setCallback (
            [this, request] (const juce::PluginHostDiagnostics::Event& event)
            {
                CrashCapture::setActiveCall (event.stage, event.call);
                auto progress = diagnosticEventToXml (event, request);

                if (event.kind == juce::PluginHostDiagnostics::EventKind::end
                     && ! event.success && event.critical)
                {
                    const std::lock_guard<std::mutex> lock (eventMutex);
                    lastCriticalFailure = std::make_unique<juce::XmlElement> (
                        *progress);
                }

                sendXml (*progress);
            });
    }

    void finishDiagnostics()
    {
        juce::PluginHostDiagnostics::clearCallback();
        CrashCapture::setActiveCall ("idle", "none");
    }

    std::unique_ptr<juce::XmlElement> lastFailureCopy() const
    {
        const std::lock_guard<std::mutex> lock (eventMutex);
        return lastCriticalFailure != nullptr
                   ? std::make_unique<juce::XmlElement> (*lastCriticalFailure)
                   : nullptr;
    }

    std::unique_ptr<juce::XmlElement> makeBaseResult (
        const ScanRequest& request) const
    {
        auto result = std::make_unique<juce::XmlElement> ("SCAN_RESULT");
        result->setAttribute ("protocol", protocolVersion);
        result->setAttribute ("token", request.token);
        result->setAttribute ("phase", toString (request.phase));
        result->setAttribute ("format", request.format);
        result->setAttribute ("path", request.identifier);

        if (request.classIndex >= 0)
            result->setAttribute ("classIndex", request.classIndex);

        if (auto environment = juce::parseXML (request.environmentXml))
            if (environment->hasTagName ("ENVIRONMENT"))
                for (int i = 0; i < environment->getNumAttributes(); ++i)
                    result->setAttribute (environment->getAttributeName (i),
                                          environment->getAttributeValue (i));

        return result;
    }

    static void setFailure (juce::XmlElement& result,
                            const juce::String& status,
                            const juce::String& stage,
                            const juce::String& code,
                            const juce::String& message,
                            const juce::String& call = {})
    {
        result.setAttribute ("status", status);
        result.setAttribute ("failureStage", stage);
        result.setAttribute ("failureCall", call);
        result.setAttribute ("errorCode", code);
        result.setAttribute ("errorMessage", message);
    }

    void setFailureFromLastEvent (juce::XmlElement& result,
                                  const juce::String& status,
                                  const juce::String& fallbackStage,
                                  const juce::String& fallbackCode,
                                  const juce::String& fallbackMessage) const
    {
        const auto event = lastFailureCopy();

        setFailure (result, status,
                    event != nullptr
                        ? event->getStringAttribute ("stage", fallbackStage)
                        : fallbackStage,
                    event != nullptr
                        ? event->getStringAttribute ("code", fallbackCode)
                        : fallbackCode,
                    event != nullptr
                        ? event->getStringAttribute ("detail", fallbackMessage)
                        : fallbackMessage,
                    event != nullptr
                        ? event->getStringAttribute ("call")
                        : juce::String{});
    }

    std::unique_ptr<juce::XmlElement> inventoryVST3 (
        const ScanRequest& request, juce::AudioPluginFormat& format)
    {
        auto result = makeBaseResult (request);
        auto* vst3 = dynamic_cast<juce::VST3PluginFormatHeadless*> (&format);

        if (vst3 == nullptr)
        {
            setFailure (*result, "host_internal_error", "factory",
                        "not_vst3_format",
                        "VST3 inventory requested for another format");
            return result;
        }

        juce::PluginHostDiagnostics::ScopedCall inventory (
            "inventory", "VST3 IPluginFactory::countClasses", true,
            request.identifier);
        const int classCount = vst3->getNumClassesForFile (request.identifier);
        inventory.finish (classCount >= 0, classCount,
                          juce::String (classCount) + " factory class(es)");

        if (classCount < 0)
        {
            setFailureFromLastEvent (
                *result, "initialization_failure", "inventory",
                "vst3_factory_unavailable",
                "VST3 module did not expose a plugin factory");
            return result;
        }

        result->setAttribute ("status", "inventoried");
        result->setAttribute ("failureStage", "none");
        result->setAttribute ("errorCode", "0");
        result->setAttribute ("errorMessage", "");
        result->setAttribute ("classCount", classCount);
        return result;
    }

    std::unique_ptr<juce::XmlElement> validateVST3Class (
        const ScanRequest& request, juce::AudioPluginFormat& format)
    {
        auto result = makeBaseResult (request);
        auto* vst3 = dynamic_cast<juce::VST3PluginFormatHeadless*> (&format);

        if (vst3 == nullptr)
        {
            setFailure (*result, "host_internal_error", "factory",
                        "not_vst3_format",
                        "VST3 class validation requested for another format");
            return result;
        }

        juce::OwnedArray<juce::PluginDescription> descriptions;
        vst3->findTypeForFileAtClassIndex (
            descriptions, request.identifier, request.classIndex);

        if (descriptions.isEmpty())
        {
            if (lastFailureCopy() != nullptr)
                setFailureFromLastEvent (
                    *result, "plugin_internal_error", "discovery_class",
                    "class_metadata_failed",
                    "VST3 factory did not describe this class");
            else
            {
                result->setAttribute ("status", "class_skipped");
                result->setAttribute ("failureStage", "none");
                result->setAttribute ("errorCode", "0");
                result->setAttribute ("errorMessage", "");
            }

            return result;
        }

        if (descriptions.size() != 1)
        {
            setFailure (*result, "host_internal_error", "protocol",
                        "multiple_descriptions_for_class",
                        "one VST3 factory class returned multiple descriptions");
            return result;
        }

        auto validationRequest = request;
        validationRequest.descriptionXml = descriptions[0]->createXml()->toString (
            juce::XmlElement::TextFormat().singleLine());
        return validate (validationRequest, format);
    }

    std::unique_ptr<juce::XmlElement> discover (
        const ScanRequest& request, juce::AudioPluginFormat& format)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        auto result = makeBaseResult (request);
        juce::OwnedArray<juce::PluginDescription> descriptions;

        juce::PluginHostDiagnostics::ScopedCall scan (
            "discovery", "AudioPluginFormat::findAllTypesForFile", true,
            request.identifier);
        format.findAllTypesForFile (descriptions, request.identifier);
        scan.finish (! descriptions.isEmpty(), descriptions.size(),
                     descriptions.isEmpty()
                         ? "no PluginDescription returned"
                         : juce::String (descriptions.size()) + " description(s)");

        auto* list = result->createNewChildElement ("DESCRIPTIONS");

        for (const auto& description : descriptions)
            list->addChildElement (description->createXml().release());

        result->setAttribute ("descriptionCount", descriptions.size());
        result->setAttribute ("totalTimeMs", juce::String (juce::roundToInt (
            juce::Time::getMillisecondCounterHiRes() - started)));

        if (descriptions.isEmpty())
            setFailureFromLastEvent (*result, "plugin_internal_error",
                                     "discovery", "no_descriptions",
                                     "plugin factory returned no descriptions");
        else
        {
            result->setAttribute ("status", "discovered");
            result->setAttribute ("failureStage", "none");
            result->setAttribute ("errorCode", "0");
            result->setAttribute ("errorMessage", "");
        }

        return result;
    }

    std::unique_ptr<juce::XmlElement> validate (
        const ScanRequest& request, juce::AudioPluginFormat& format)
    {
        const auto totalStarted = juce::Time::getMillisecondCounterHiRes();
        auto result = makeBaseResult (request);
        auto descriptionXml = juce::parseXML (request.descriptionXml);
        juce::PluginDescription description;

        if (descriptionXml == nullptr || ! description.loadFromXml (*descriptionXml))
        {
            setFailure (*result, "host_internal_error", "protocol",
                        "invalid_description",
                        "worker could not decode PluginDescription");
            return result;
        }

        result->setAttribute ("name", description.name);
        result->setAttribute ("manufacturer", description.manufacturerName);
        result->setAttribute ("version", description.version);
        result->setAttribute ("category", description.category);

        const auto initialiseStarted = juce::Time::getMillisecondCounterHiRes();
        juce::String creationError;
        std::unique_ptr<juce::AudioPluginInstance> instance;

        {
            juce::PluginHostDiagnostics::ScopedCall create (
                "creation", "AudioPluginFormat::createInstanceFromDescription",
                true, description.name);
            instance = format.createInstanceFromDescription (
                description, 48000.0, 512, creationError);
            create.finish (instance != nullptr, instance != nullptr ? 0 : -1,
                           creationError);
        }

        auto stampTimes = [&]
        {
            result->setAttribute ("initializationTimeMs",
                                  juce::String (juce::roundToInt (
                                      juce::Time::getMillisecondCounterHiRes()
                                        - initialiseStarted)));
            result->setAttribute ("totalTimeMs",
                                  juce::String (juce::roundToInt (
                                      juce::Time::getMillisecondCounterHiRes()
                                        - totalStarted)));
        };

        if (instance == nullptr)
        {
            setFailureFromLastEvent (
                *result, "initialization_failure", "initialize", "instance_null",
                creationError.isNotEmpty() ? creationError
                                           : "plugin instance was not created");
            result->setAttribute ("stack", juce::SystemStats::getStackBacktrace());
            stampTimes();
            return result;
        }

        if (const auto failure = lastFailureCopy();
            failure != nullptr
             && (failure->getStringAttribute ("stage") == "creation"
                  || failure->getStringAttribute ("stage") == "initialize"))
        {
            setFailureFromLastEvent (*result, "initialization_failure",
                                     "initialize", "internal_initialize_failed",
                                     "plugin reported an initialization failure");
        }

        auto exerciseOnMessageThread = [&]
        {
            if (result->getStringAttribute ("status").isNotEmpty())
                return false;

            {
                juce::PluginHostDiagnostics::ScopedCall activate (
                    "activate", "AudioProcessor::prepareToPlay(48000,512)",
                    true, description.name);
                instance->prepareToPlay (48000.0, 512);
                activate.finish (true, 0, description.name);
            }

            if (const auto failure = lastFailureCopy();
                failure != nullptr
                 && failure->getStringAttribute ("stage") == "activate")
            {
                setFailureFromLastEvent (*result, "plugin_internal_error",
                                         "activate", "activation_failed",
                                         "plugin rejected activation");
                return false;
            }

            // Scanner valida o caminho de AUDIO, nao a janela grafica. Criar a
            // GUI durante a descoberta causa falsos negativos em plugins que
            // precisam de uma janela ativa, autorizacao/licenca ou AUv3 fora do
            // processo. A interface sera criada sob demanda pelo host, que ja
            // possui GenericAudioProcessorEditor como fallback.
            const bool hasEditor = instance->hasEditor();
            result->setAttribute ("editorAdvertised", hasEditor);
            result->setAttribute ("editorValidation", "deferred_until_open");

            if (request.phase == ScanPhase::vst3ClassValidation)
            {
                juce::PluginDescription validatedDescription;
                juce::PluginHostDiagnostics::ScopedCall describe (
                    "description",
                    "AudioPluginInstance::fillInPluginDescription", true,
                    description.name);
                instance->fillInPluginDescription (validatedDescription);

                // Preserve exactly the identity produced during discovery.
                // On Windows this may be the module inside the .vst3 bundle;
                // changing it to the bundle root would change JUCE's stable
                // identifier and break existing saved setups.
                validatedDescription.fileOrIdentifier =
                    description.fileOrIdentifier;
                validatedDescription.lastFileModTime =
                    description.lastFileModTime;

                const bool sameIdentity =
                    validatedDescription.isDuplicateOf (description)
                    && validatedDescription.pluginFormatName
                           == description.pluginFormatName;
                describe.finish (sameIdentity, sameIdentity ? 0 : -1,
                                 validatedDescription.name);

                if (! sameIdentity)
                {
                    setFailure (*result, "host_internal_error", "description",
                                "validated_identity_changed",
                                "plugin changed identity during validation");
                }
                else
                    result->addChildElement (
                        validatedDescription.createXml().release());
            }

            {
                juce::PluginHostDiagnostics::ScopedCall release (
                    "unload", "AudioProcessor::releaseResources", false,
                    description.name);
                instance->releaseResources();
                release.finish (true, 0, description.name);
            }

            return result->getStringAttribute ("status").isEmpty();
        };

        bool exercised = false;

        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
            exercised = exerciseOnMessageThread();
        else
        {
            const auto dispatched = juce::MessageManager::callSync ([&]
            {
                exercised = exerciseOnMessageThread();
            });

            if (! dispatched)
                setFailure (*result, "host_internal_error", "message_thread",
                            "callSync_failed",
                            "could not dispatch activation/editor validation");
        }

        if (result->getStringAttribute ("status").isEmpty())
        {
            if (exercised)
            {
                result->setAttribute ("status", "loaded_success");
                result->setAttribute ("failureStage", "none");
                result->setAttribute ("failureCall", "");
                result->setAttribute ("errorCode", "0");
                result->setAttribute ("errorMessage", "");
            }
            else
                setFailureFromLastEvent (*result, "plugin_internal_error",
                                         "validation", "validation_failed",
                                         "activation or editor validation failed");
        }

        if (result->getStringAttribute ("status") != "loaded_success")
            result->setAttribute ("stack", juce::SystemStats::getStackBacktrace());

        {
            juce::PluginHostDiagnostics::ScopedCall unload (
                "unload", "AudioPluginInstance destructor", false,
                description.name);
            instance.reset();
            unload.finish (true, 0, description.name);
        }

        stampTimes();
        return result;
    }

    void executeAndSend (const ScanRequest& request)
    {
        std::unique_ptr<juce::XmlElement> result;

        try
        {
            if (auto* format = findFormat (request.format))
            {
                switch (request.phase)
                {
                    case ScanPhase::discovery:
                        result = discover (request, *format);
                        break;

                    case ScanPhase::validation:
                        result = validate (request, *format);
                        break;

                    case ScanPhase::vst3Inventory:
                        result = inventoryVST3 (request, *format);
                        break;

                    case ScanPhase::vst3ClassValidation:
                        result = validateVST3Class (request, *format);
                        break;
                }
            }
            else
            {
                result = makeBaseResult (request);
                setFailure (*result, "host_internal_error", "factory",
                            "format_not_registered",
                            "format disappeared from scanner worker");
            }
        }
        catch (const std::exception& exception)
        {
            result = makeBaseResult (request);
            setFailureFromLastEvent (*result, "plugin_internal_error",
                                     "unknown", "cpp_exception",
                                     exception.what());
            result->setAttribute ("stack", juce::SystemStats::getStackBacktrace());
        }
        catch (...)
        {
            result = makeBaseResult (request);
            setFailureFromLastEvent (*result, "plugin_internal_error",
                                     "unknown", "unknown_exception",
                                     "plugin threw a non-standard exception");
            result->setAttribute ("stack", juce::SystemStats::getStackBacktrace());
        }

        finishDiagnostics();

        if (result != nullptr)
            sendXml (*result);
    }

    void sendXml (const juce::XmlElement& xml)
    {
        const auto text = xml.toString (
            juce::XmlElement::TextFormat().singleLine());
        sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() });
    }

    std::mutex queueMutex;
    mutable std::mutex eventMutex;
    std::queue<juce::MemoryBlock> pendingBlocks;
    std::unique_ptr<juce::XmlElement> lastCriticalFailure;
    juce::AudioPluginFormatManager formatManager;
    std::unique_ptr<RequestThread> requestThread;
    bool formatsRegistered = false;
};

//==============================================================================
class ScannerSuperprocess final : private juce::ChildProcessCoordinator
{
public:
    ~ScannerSuperprocess() override
    {
        // O pipe deve parar antes dos mutexes/strings desta classe morrerem.
        if (! terminationAttempted)
            stopAndReap();
    }

    ScannerSuperprocess()
    {
        launched = launchWorkerProcess (
            juce::File::getSpecialLocation (juce::File::currentExecutableFile),
            processUID, 15000, 0);
    }

    enum class State { pollTimeout, gotResult, connectionLost };

    struct Response
    {
        State state = State::pollTimeout;
        std::unique_ptr<juce::XmlElement> xml;
    };

    struct Progress
    {
        std::vector<std::unique_ptr<juce::XmlElement>> events;
        juce::String lastStage;
        juce::String lastCall;
        juce::String lastEvent;
        juce::String lastCode;
    };

    bool wasLaunched() const noexcept { return launched; }
    using juce::ChildProcessCoordinator::sendMessageToWorker;

    bool stopAndReap()
    {
        if (terminationAttempted)
            return terminationSucceeded;

        terminationAttempted = true;
        terminationSucceeded = killWorkerProcessAndWait (500, 2000);

        if (! terminationSucceeded)
            juce::Logger::writeToLog (
                "PLUGIN_SCAN child_reap_failed after forced termination");

        return terminationSucceeded;
    }

    Response getResponse()
    {
        std::unique_lock<std::mutex> lock { mutex };

        if (! condition.wait_for (lock, std::chrono::milliseconds { 50 },
                                  [&] { return gotResult || lost; }))
            return {};

        if (lost)
            return { State::connectionLost, nullptr };

        gotResult = false;
        return { State::gotResult, std::move (finalResult) };
    }

    Progress takeProgress()
    {
        const std::lock_guard<std::mutex> lock { mutex };
        Progress result;
        result.events = std::move (progressEvents);
        result.lastStage = lastStage;
        result.lastCall = lastCall;
        result.lastEvent = lastEvent;
        result.lastCode = lastCode;
        return result;
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& block) override
    {
        auto xml = juce::parseXML (block.toString());

        if (xml != nullptr && xml->hasTagName ("PROGRESS"))
        {
            const auto logLine = "PLUGIN_SCAN token="
                + xml->getStringAttribute ("token")
                + " phase=" + xml->getStringAttribute ("phase")
                + " stage=" + xml->getStringAttribute ("stage")
                + " call=\"" + xml->getStringAttribute ("call") + "\""
                + " event=" + xml->getStringAttribute ("event")
                + " success=" + xml->getStringAttribute ("success")
                + " code=" + xml->getStringAttribute ("code")
                + " elapsedMs=" + xml->getStringAttribute ("elapsedMs")
                + (xml->getStringAttribute ("detail").isNotEmpty()
                       ? " detail=\"" + xml->getStringAttribute ("detail") + "\""
                       : juce::String{});
            juce::Logger::writeToLog (logLine);

            const std::lock_guard<std::mutex> lock { mutex };
            lastStage = xml->getStringAttribute ("stage");
            lastCall = xml->getStringAttribute ("call");
            lastEvent = xml->getStringAttribute ("event");
            lastCode = xml->getStringAttribute ("code");
            progressEvents.push_back (std::move (xml));
            return;
        }

        const std::lock_guard<std::mutex> lock { mutex };
        finalResult = std::move (xml);
        gotResult = true;
        condition.notify_one();
    }

    void handleConnectionLost() override
    {
        const std::lock_guard<std::mutex> lock { mutex };
        lost = true;
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> finalResult;
    std::vector<std::unique_ptr<juce::XmlElement>> progressEvents;
    juce::String lastStage, lastCall, lastEvent, lastCode;
    bool launched = false;
    bool lost = false;
    bool gotResult = false;
    bool terminationAttempted = false;
    bool terminationSucceeded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScannerSuperprocess)
};

//==============================================================================
class ChurchDAWPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    ChurchDAWPluginScanner()
    {
        juce::Logger::writeToLog (
            "PLUGIN_SCAN report=" + reportWriter.getFile().getFullPathName());
    }

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        currentFormat = format.getName();

        if (ehAudioUnitDeSistema (fileOrIdentifier))
        {
            auto report = makePolicySkipReport (format.getName(), fileOrIdentifier);
            reportWriter.appendPlugin (*report);
            juce::Logger::writeToLog (
                "PLUGIN_SCAN skip_system_component path=" + fileOrIdentifier);
            // Defesa adicional. O fluxo normal filtra antes de construir o
            // PluginDirectoryScanner; true evita uma blacklist persistente se
            // algum chamador futuro entregar este identificador diretamente.
            return true;
        }

        const auto discoveryEnvironment = getEnvironment (fileOrIdentifier, nullptr);

        if (! discoveryEnvironment.architectureCompatible)
        {
            auto report = makeArchitectureReport (
                format.getName(), fileOrIdentifier, nullptr,
                discoveryEnvironment);
            reportWriter.appendPlugin (*report);
            return true;   // incompatibilidade nao e crash do plugin
        }

        if (dynamic_cast<juce::VST3PluginFormatHeadless*> (&format) != nullptr)
            return findVST3PluginTypesFor (
                format, result, fileOrIdentifier, discoveryEnvironment);

        ScanRequest discovery;
        discovery.phase = ScanPhase::discovery;
        discovery.token = makeScanToken();
        discovery.format = format.getName();
        discovery.identifier = fileOrIdentifier;
        discovery.environmentXml = environmentToXml (
            discoveryEnvironment)->toString (
                juce::XmlElement::TextFormat().singleLine());

        auto discoveryAttempt = runPhase (discovery);
        noteInconclusiveAttempt (discoveryAttempt);

        if (discoveryAttempt.finish == PhaseFinish::cancelled
             && discoveryAttempt.workerReaped)
            return true;

        if (! discoveryAttempt.workerReaped
             || discoveryAttempt.finish != PhaseFinish::result
             || discoveryAttempt.xml == nullptr
             || discoveryAttempt.xml->getStringAttribute ("status") != "discovered")
        {
            auto report = reportForAttempt (discovery, discoveryAttempt,
                                            discoveryEnvironment, nullptr);
            reportWriter.appendPlugin (*report);
            return ! discoveryAttempt.workerReaped
                || ! deveEntrarNaBlacklistPersistente (discoveryAttempt.finish);
        }

        auto* descriptions = discoveryAttempt.xml->getChildByName ("DESCRIPTIONS");

        if (descriptions == nullptr)
        {
            scanHadInconclusive.store (true);
            auto report = reportForAttempt (discovery, discoveryAttempt,
                                            discoveryEnvironment, nullptr);
            setReportFailure (*report, "host_internal_error", "protocol",
                              "missing_descriptions",
                              "discovery result omitted DESCRIPTIONS");
            reportWriter.appendPlugin (*report);
            return true;
        }

        int discovered = 0;
        int validated = 0;
        CandidateValidationSummary validationSummary;

        for (const auto* item : descriptions->getChildIterator())
        {
            if (shouldExit())
                return true;

            juce::PluginDescription description;

            if (! description.loadFromXml (*item))
            {
                scanHadInconclusive.store (true);
                ++validationSummary.retryableOrSkipped;
                continue;
            }

            ++discovered;
            const auto environment = getEnvironment (fileOrIdentifier,
                                                      &description);

            if (! environment.architectureCompatible)
            {
                auto report = makeArchitectureReport (
                    format.getName(), fileOrIdentifier, &description,
                    environment);
                reportWriter.appendPlugin (*report);
                ++validationSummary.retryableOrSkipped;
                continue;
            }

            ScanRequest validation;
            validation.phase = ScanPhase::validation;
            validation.token = makeScanToken();
            validation.format = format.getName();
            validation.identifier = fileOrIdentifier;
            validation.descriptionXml = item->toString (
                juce::XmlElement::TextFormat().singleLine());
            validation.environmentXml = environmentToXml (environment)->toString (
                juce::XmlElement::TextFormat().singleLine());

            auto validationAttempt = runPhase (validation);
            noteInconclusiveAttempt (validationAttempt);

            if (validationAttempt.finish == PhaseFinish::cancelled
                 && validationAttempt.workerReaped)
                return true;

            auto report = reportForAttempt (validation, validationAttempt,
                                            environment, &description);
            reportWriter.appendPlugin (*report);

            if (report->getStringAttribute ("status") == "loaded_success")
            {
                result.add (new juce::PluginDescription (description));
                ++validated;
                ++validationSummary.loaded;
            }
            else if (validationAttempt.workerReaped
                     && deveEntrarNaBlacklistPersistente (
                         validationAttempt.finish))
                ++validationSummary.fatal;
            else
                ++validationSummary.retryableOrSkipped;
        }

        juce::Logger::writeToLog (
            "PLUGIN_SCAN candidate_done format=" + format.getName()
            + " path=" + fileOrIdentifier
            + " discovered=" + juce::String (discovered)
            + " validated=" + juce::String (validated)
            + " fatal=" + juce::String (validationSummary.fatal)
            + " retryable_or_skipped="
            + juce::String (validationSummary.retryableOrSkipped));

        // false significa exclusivamente crash observado em todas as classes.
        // Timeout, arquitetura, protocolo, licenca ou carga ficam no
        // JSONL/failedFiles e podem ser tentados de novo sem limpar blacklist.
        // Em shells, qualquer classe valida, retryable/inconclusiva ou pulada
        // por arquitetura preserva o bundle inteiro. A blacklist por ARQUIVO
        // so e segura quando todas as classes observadas foram fatais.
        return ! deveEntrarNaBlacklistPersistente (validationSummary);
    }

    void scanFinished() override
    {
        reportWriter.appendSummary (currentFormat);
        juce::Logger::writeToLog (
            "PLUGIN_SCAN finished format=" + currentFormat
            + " report=" + reportWriter.getFile().getFullPathName());
    }

    bool consumeLastScanWasInconclusive() override
    {
        return scanHadInconclusive.exchange (false);
    }

private:
    using PhaseFinish = ChurchDAWPluginScan::PhaseFinish;

    struct PhaseAttempt
    {
        PhaseFinish finish = PhaseFinish::launchFailed;
        std::unique_ptr<juce::XmlElement> xml;
        ScannerSuperprocess::Progress progress;
        juce::String stack;
        int elapsedMs = 0;
        bool workerReaped = true;
    };

    static bool reachedVST3AudioClass (const PhaseAttempt& attempt)
    {
        for (const auto& event : attempt.progress.events)
            if (event != nullptr
                 && event->getStringAttribute ("stage") == "discovery_class"
                 && event->getStringAttribute ("call")
                        == "VST3 audio class selected")
                return true;

        return false;
    }

    static bool descriptionBelongsToCandidate (
        const juce::String& descriptionIdentifier,
        const juce::String& candidateIdentifier)
    {
        if (descriptionIdentifier == candidateIdentifier)
            return true;

        const juce::File candidate { candidateIdentifier };
        return candidate.isDirectory()
            && juce::File (descriptionIdentifier).isAChildOf (candidate);
    }

    bool findVST3PluginTypesFor (
        juce::AudioPluginFormat& format,
        juce::OwnedArray<juce::PluginDescription>& result,
        const juce::String& fileOrIdentifier,
        const EnvironmentReport& environment)
    {
        ScanRequest inventory;
        inventory.phase = ScanPhase::vst3Inventory;
        inventory.token = makeScanToken();
        inventory.format = format.getName();
        inventory.identifier = fileOrIdentifier;
        inventory.environmentXml = environmentToXml (environment)->toString (
            juce::XmlElement::TextFormat().singleLine());

        auto inventoryAttempt = runPhase (inventory);
        noteInconclusiveAttempt (inventoryAttempt);

        if (inventoryAttempt.finish == PhaseFinish::cancelled
             && inventoryAttempt.workerReaped)
            return true;

        if (! inventoryAttempt.workerReaped
             || inventoryAttempt.finish != PhaseFinish::result
             || inventoryAttempt.xml == nullptr
             || inventoryAttempt.xml->getStringAttribute ("status")
                    != "inventoried")
        {
            auto report = reportForAttempt (
                inventory, inventoryAttempt, environment, nullptr);
            reportWriter.appendPlugin (*report);
            return ! inventoryAttempt.workerReaped
                || ! deveEntrarNaBlacklistPersistente (
                    inventoryAttempt.finish);
        }

        constexpr int maxFactoryClasses = 4096;
        const int classCount = inventoryAttempt.xml->getIntAttribute (
            "classCount", -1);

        if (! juce::isPositiveAndBelow (classCount,
                                        maxFactoryClasses + 1)
             && classCount != 0)
        {
            scanHadInconclusive.store (true);
            auto report = reportForAttempt (
                inventory, inventoryAttempt, environment, nullptr);
            setReportFailure (*report, "host_internal_error", "protocol",
                              "invalid_vst3_class_count",
                              "VST3 factory returned an invalid class count");
            reportWriter.appendPlugin (*report);
            return true;
        }

        CandidateValidationAccumulator accumulator { result };
        int audioClasses = 0;

        for (int classIndex = 0; classIndex < classCount; ++classIndex)
        {
            if (shouldExit())
                return true;

            ScanRequest validation;
            validation.phase = ScanPhase::vst3ClassValidation;
            validation.token = makeScanToken();
            validation.format = format.getName();
            validation.identifier = fileOrIdentifier;
            validation.classIndex = classIndex;
            validation.environmentXml = inventory.environmentXml;

            auto attempt = runPhase (validation);
            noteInconclusiveAttempt (attempt);

            if (attempt.finish == PhaseFinish::cancelled
                 && attempt.workerReaped)
                return true;

            const bool reachedAudioClass = reachedVST3AudioClass (attempt);

            if (reachedAudioClass)
                ++audioClasses;
            else if (attempt.finish == PhaseFinish::crash)
                scanHadInconclusive.store (true);

            if (attempt.workerReaped
                 && attempt.finish == PhaseFinish::result
                 && attempt.xml != nullptr
                 && attempt.xml->getStringAttribute ("status")
                        == "class_skipped")
                continue;

            juce::PluginDescription validatedDescription;
            const bool loaded = attempt.workerReaped
                && attempt.finish == PhaseFinish::result
                && attempt.xml != nullptr
                && attempt.xml->getStringAttribute ("status")
                       == "loaded_success"
                && [&]
                   {
                       if (const auto* plugin =
                               attempt.xml->getChildByName ("PLUGIN"))
                           return validatedDescription.loadFromXml (*plugin)
                               && descriptionBelongsToCandidate (
                                      validatedDescription.fileOrIdentifier,
                                      fileOrIdentifier)
                               && validatedDescription.pluginFormatName
                                      == format.getName();

                       return false;
                   }();

            auto report = reportForAttempt (
                validation, attempt, environment,
                loaded ? &validatedDescription : nullptr);

            if (attempt.xml != nullptr
                 && attempt.xml->getStringAttribute ("status")
                        == "loaded_success"
                 && ! loaded)
            {
                scanHadInconclusive.store (true);
                setReportFailure (*report, "host_internal_error", "protocol",
                                  "invalid_validated_description",
                                  "validated VST3 class omitted or changed its identity");
            }

            reportWriter.appendPlugin (*report);

            if (loaded)
                accumulator.addLoaded (validatedDescription);
            else
                accumulator.addFailure (
                    attempt.finish,
                    attempt.workerReaped && reachedAudioClass);

            // Depois que um filho nao pode ser colhido, runPhase fecha para as
            // proximas classes. Pare aqui para nao fabricar centenas de falhas
            // de transporte; as classes ja aprovadas continuam preservadas.
            if (! attempt.workerReaped)
                break;
        }

        const auto& summary = accumulator.getSummary();
        juce::Logger::writeToLog (
            "PLUGIN_SCAN candidate_done format=" + format.getName()
            + " path=" + fileOrIdentifier
            + " factory_classes=" + juce::String (classCount)
            + " audio_classes=" + juce::String (audioClasses)
            + " validated=" + juce::String (summary.loaded)
            + " fatal=" + juce::String (summary.fatal)
            + " retryable_or_skipped="
            + juce::String (summary.retryableOrSkipped));

        return ! deveEntrarNaBlacklistPersistente (summary);
    }

    void noteInconclusiveAttempt (const PhaseAttempt& attempt)
    {
        const bool inconclusiveFinish =
            ehTentativaInconclusiva (attempt.finish);
        const bool protocolFailure = attempt.finish == PhaseFinish::result
                                  && (attempt.xml == nullptr
                                      || attempt.xml->getStringAttribute ("status")
                                             == "host_internal_error");

        if (inconclusiveFinish || protocolFailure || ! attempt.workerReaped)
            scanHadInconclusive.store (true);
    }

    static int timeoutForPluginMs()
    {
        const auto configured = juce::SystemStats::getEnvironmentVariable (
            "CHURCHDAW_PLUGIN_SCAN_TIMEOUT_MS", {}).getIntValue();
        return configured > 0 ? juce::jlimit (10000, 300000, configured)
                              : 120000;
    }

    PhaseAttempt runPhase (const ScanRequest& request)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        PhaseAttempt attempt;

        if (childReapFailed.load())
        {
            attempt.workerReaped = false;
            return attempt;
        }

        ScannerSuperprocess process;

        if (! process.wasLaunched())
        {
            attempt.finish = PhaseFinish::launchFailed;
            attempt.workerReaped = process.stopAndReap();

            if (! attempt.workerReaped)
                childReapFailed.store (true);

            return attempt;
        }

        const auto block = serialiseRequest (request);

        if (! process.sendMessageToWorker (block))
        {
            attempt.finish = PhaseFinish::sendFailed;
            attempt.workerReaped = process.stopAndReap();

            if (! attempt.workerReaped)
                childReapFailed.store (true);

            return attempt;
        }

        const auto timeoutMs = timeoutForPluginMs();

        for (;;)
        {
            if (shouldExit())
            {
                attempt.finish = PhaseFinish::cancelled;
                break;
            }

            const auto elapsed = juce::roundToInt (
                juce::Time::getMillisecondCounterHiRes() - started);

            if (elapsed >= timeoutMs)
            {
                attempt.finish = PhaseFinish::timeout;
                break;
            }

            auto response = process.getResponse();

            if (response.state == ScannerSuperprocess::State::pollTimeout)
                continue;

            if (response.state == ScannerSuperprocess::State::connectionLost)
            {
                attempt.finish = PhaseFinish::crash;
                break;
            }

            attempt.finish = PhaseFinish::result;
            attempt.xml = std::move (response.xml);
            break;
        }

        attempt.elapsedMs = juce::roundToInt (
            juce::Time::getMillisecondCounterHiRes() - started);
        attempt.progress = process.takeProgress();

        if (attempt.finish == PhaseFinish::crash)
            attempt.stack = readCrashStack (request.token);

        attempt.workerReaped = process.stopAndReap();

        if (! attempt.workerReaped)
            childReapFailed.store (true);

        return attempt;
    }

    EnvironmentReport getEnvironment (
        const juce::String& identifier,
        const juce::PluginDescription* description)
    {
        const auto pathExists = juce::File (identifier).exists();
        const auto key = identifier
            + (pathExists || description == nullptr
                   ? juce::String{}
                   : "|" + description->name + "|"
                       + description->manufacturerName);

        const auto found = environmentCache.find (key);

        if (found != environmentCache.end())
            return found->second;

        const auto report = collectEnvironment (identifier, description);
        environmentCache.emplace (key, report);
        return report;
    }

    static void copyEnvironment (juce::XmlElement& report,
                                 const EnvironmentReport& environment)
    {
        environment.writeTo (report);
    }

    static void copyDescription (juce::XmlElement& report,
                                 const juce::PluginDescription* description)
    {
        if (description == nullptr)
            return;

        report.setAttribute ("name", description->name);
        report.setAttribute ("manufacturer", description->manufacturerName);
        report.setAttribute ("version", description->version);
        report.setAttribute ("category", description->category);
    }

    static void setReportFailure (juce::XmlElement& report,
                                  const juce::String& status,
                                  const juce::String& stage,
                                  const juce::String& code,
                                  const juce::String& message,
                                  const juce::String& call = {})
    {
        report.setAttribute ("status", status);
        report.setAttribute ("failureStage", stage);
        report.setAttribute ("failureCall", call);
        report.setAttribute ("errorCode", code);
        report.setAttribute ("errorMessage", message);
    }

    static void appendProgress (juce::XmlElement& report,
                                ScannerSuperprocess::Progress& progress)
    {
        for (auto& source : progress.events)
        {
            if (source == nullptr)
                continue;

            source->setTagName ("EVENT");
            report.addChildElement (source.release());
        }
    }

    std::unique_ptr<juce::XmlElement> reportForAttempt (
        const ScanRequest& request,
        PhaseAttempt& attempt,
        const EnvironmentReport& environment,
        const juce::PluginDescription* description)
    {
        std::unique_ptr<juce::XmlElement> report;

        if (attempt.xml != nullptr)
            report = std::make_unique<juce::XmlElement> (*attempt.xml);
        else
        {
            report = std::make_unique<juce::XmlElement> ("PLUGIN_REPORT");
            report->setAttribute ("token", request.token);
            report->setAttribute ("phase", toString (request.phase));
            report->setAttribute ("format", request.format);
            report->setAttribute ("path", request.identifier);
        }

        report->setTagName ("PLUGIN_REPORT");
        report->setAttribute ("elapsedMs", attempt.elapsedMs);

        if (request.classIndex >= 0)
            report->setAttribute ("classIndex", request.classIndex);

        copyEnvironment (*report, environment);
        copyDescription (*report, description);

        switch (attempt.finish)
        {
            case PhaseFinish::result:
                if (attempt.xml == nullptr)
                    setReportFailure (*report, "host_internal_error", "protocol",
                                      "empty_result",
                                      "worker returned no XML result");
                break;

            case PhaseFinish::crash:
                setReportFailure (*report, "crash",
                                  attempt.progress.lastStage.isNotEmpty()
                                      ? attempt.progress.lastStage : "worker_startup",
                                  attempt.progress.lastCode.isNotEmpty()
                                      ? attempt.progress.lastCode : "child_connection_lost",
                                  "scanner child terminated while inside plugin code",
                                  attempt.progress.lastCall);
                report->setAttribute ("stack",
                                      attempt.stack.isNotEmpty()
                                          ? attempt.stack
                                          : "stack unavailable; inspect the macOS .ips crash report");
                break;

            case PhaseFinish::timeout:
                setReportFailure (*report, "timeout",
                                  attempt.progress.lastStage.isNotEmpty()
                                      ? attempt.progress.lastStage : "worker_startup",
                                  "timeout_" + juce::String (timeoutForPluginMs()) + "ms",
                                  "plugin did not return before the per-plugin deadline",
                                  attempt.progress.lastCall);
                report->setAttribute ("stack",
                                      "unavailable: process was alive but unresponsive");
                break;

            case PhaseFinish::cancelled:
                setReportFailure (*report, "cancelled", "cancelled",
                                  "user_cancelled", "scan cancelled by user");
                break;

            case PhaseFinish::launchFailed:
                setReportFailure (*report, "host_internal_error", "worker_startup",
                                  "child_launch_failed",
                                  "ChurchDAW could not launch its scanner subprocess");
                break;

            case PhaseFinish::sendFailed:
                setReportFailure (*report, "host_internal_error", "ipc",
                                  "request_send_failed",
                                  "ChurchDAW could not send the scan request to its child");
                break;
        }

        if (! attempt.workerReaped)
            setReportFailure (*report, "host_internal_error", "worker_shutdown",
                              "child_reap_failed",
                              "scanner child could not be terminated and reaped");

        appendProgress (*report, attempt.progress);

        juce::Logger::writeToLog (
            "PLUGIN_SCAN result status=" + report->getStringAttribute ("status")
            + " format=" + report->getStringAttribute ("format")
            + " name=\"" + report->getStringAttribute ("name") + "\""
            + " stage=" + report->getStringAttribute ("failureStage")
            + " call=\"" + report->getStringAttribute ("failureCall") + "\""
            + " code=" + report->getStringAttribute ("errorCode")
            + " elapsedMs=" + juce::String (attempt.elapsedMs));
        return report;
    }

    static std::unique_ptr<juce::XmlElement> makeArchitectureReport (
        const juce::String& format,
        const juce::String& identifier,
        const juce::PluginDescription* description,
        const EnvironmentReport& environment)
    {
        auto report = std::make_unique<juce::XmlElement> ("PLUGIN_REPORT");
        report->setAttribute ("token", makeScanToken());
        report->setAttribute ("phase", "preflight");
        report->setAttribute ("format", format);
        report->setAttribute ("path", identifier);
        copyEnvironment (*report, environment);
        copyDescription (*report, description);
        setReportFailure (*report, "architecture_incompatible", "preflight",
                          "architecture_mismatch",
                          "plugin architecture " + environment.pluginArchitecture
                            + " cannot load in process "
                            + environment.processArchitecture
                            + (environment.rosetta ? " (Rosetta)" : " (native)"));
        report->setAttribute ("initializationTimeMs", "0");
        report->setAttribute ("totalTimeMs", "0");
        report->setAttribute ("stack", "not applicable: rejected before loading");
        return report;
    }

    static std::unique_ptr<juce::XmlElement> makePolicySkipReport (
        const juce::String& format,
        const juce::String& identifier)
    {
        auto report = std::make_unique<juce::XmlElement> ("PLUGIN_REPORT");
        report->setAttribute ("token", makeScanToken());
        report->setAttribute ("phase", "preflight");
        report->setAttribute ("format", format);
        report->setAttribute ("path", identifier);
        setReportFailure (*report, "skipped_system_component", "discovery",
                          "host_policy_system_component",
                          "non-musical Apple system Audio Unit intentionally skipped");
        report->setAttribute ("architecture", "Apple system component");
        report->setAttribute ("initializationTimeMs", "0");
        report->setAttribute ("totalTimeMs", "0");
        report->setAttribute ("stack", "not applicable: plugin code was not loaded");
        return report;
    }

    ReportWriter reportWriter;
    std::map<juce::String, EnvironmentReport> environmentCache;
    juce::String currentFormat;
    std::atomic<bool> scanHadInconclusive { false };
    std::atomic<bool> childReapFailed { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchDAWPluginScanner)
};

} // namespace ChurchDAWPluginScan
