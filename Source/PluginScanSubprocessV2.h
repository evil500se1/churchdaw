#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginScanDiagnostics.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

// Scanner ChurchDAW v2
//
// Cada candidato passa por dois processos descartaveis:
//   1. discovery: bundle/componente -> factory -> PluginDescription(s)
//   2. validation: uma descricao -> create -> initialize -> activate -> editor
//                  -> release -> unload
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

            bool hasEditor = false;

            {
                juce::PluginHostDiagnostics::ScopedCall query (
                    "createEditor", "AudioProcessor::hasEditor", false,
                    description.name);
                hasEditor = instance->hasEditor();
                query.finish (true, hasEditor ? 1 : 0,
                              hasEditor ? "editor advertised"
                                        : "no editor advertised");
            }

            if (hasEditor)
            {
                juce::PluginHostDiagnostics::ScopedCall createEditor (
                    "createEditor", "AudioProcessor::createEditorAndMakeActive",
                    true, description.name);
                std::unique_ptr<juce::AudioProcessorEditor> editor (
                    instance->createEditorAndMakeActive());
                createEditor.finish (editor != nullptr,
                                     editor != nullptr ? 0 : -1,
                                     editor != nullptr
                                         ? "editor created"
                                         : "hasEditor=true but createEditor returned null");

                if (editor == nullptr)
                {
                    setFailureFromLastEvent (*result, "plugin_internal_error",
                                             "createEditor", "editor_null",
                                             "plugin advertised an editor but returned null");
                    return false;
                }

                {
                    juce::PluginHostDiagnostics::ScopedCall destroyEditor (
                        "unload", "AudioProcessorEditor destructor", false,
                        description.name);
                    editor.reset();
                    destroyEditor.finish (true, 0, description.name);
                }
            }

            {
                juce::PluginHostDiagnostics::ScopedCall release (
                    "unload", "AudioProcessor::releaseResources", false,
                    description.name);
                instance->releaseResources();
                release.finish (true, 0, description.name);
            }

            return true;
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
                result = request.phase == ScanPhase::discovery
                             ? discover (request, *format)
                             : validate (request, *format);
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
        killWorkerProcess();
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScannerSuperprocess)
};

//==============================================================================
inline bool ehAudioUnitDeSistema (const juce::String& fileOrIdentifier)
{
    if (! fileOrIdentifier.startsWithIgnoreCase ("AudioUnit:")
         || ! fileOrIdentifier.endsWithIgnoreCase (",appl"))
        return false;

    for (const auto* category : { "AudioUnit:Mixers/", "AudioUnit:Panners/" })
        if (fileOrIdentifier.startsWithIgnoreCase (category))
            return true;

    // Componentes Apple que abrem rede, fala ou arquivos e nao sao efeitos
    // musicais do ChurchDAW. EQ/dynamics/limiter da Apple ficam permitidos.
    for (const auto* subtype : { "aufx,rvb2,", "augn,ttsp,", "augn,nrcv,",
                                 "augn,nsnd,", "augn,sspl,", "augn,afpl," })
        if (fileOrIdentifier.containsIgnoreCase (subtype))
            return true;

    return false;
}

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
            return false;
        }

        const auto discoveryEnvironment = getEnvironment (fileOrIdentifier, nullptr);

        if (! discoveryEnvironment.architectureCompatible)
        {
            auto report = makeArchitectureReport (
                format.getName(), fileOrIdentifier, nullptr,
                discoveryEnvironment);
            reportWriter.appendPlugin (*report);
            return false;
        }

        ScanRequest discovery;
        discovery.phase = ScanPhase::discovery;
        discovery.token = makeScanToken();
        discovery.format = format.getName();
        discovery.identifier = fileOrIdentifier;
        discovery.environmentXml = environmentToXml (
            discoveryEnvironment)->toString (
                juce::XmlElement::TextFormat().singleLine());

        auto discoveryAttempt = runPhase (discovery);

        if (discoveryAttempt.finish == PhaseFinish::cancelled)
            return true;

        if (discoveryAttempt.finish != PhaseFinish::result
             || discoveryAttempt.xml == nullptr
             || discoveryAttempt.xml->getStringAttribute ("status") != "discovered")
        {
            auto report = reportForAttempt (discovery, discoveryAttempt,
                                            discoveryEnvironment, nullptr);
            reportWriter.appendPlugin (*report);
            return false;
        }

        auto* descriptions = discoveryAttempt.xml->getChildByName ("DESCRIPTIONS");

        if (descriptions == nullptr)
        {
            auto report = reportForAttempt (discovery, discoveryAttempt,
                                            discoveryEnvironment, nullptr);
            setReportFailure (*report, "host_internal_error", "protocol",
                              "missing_descriptions",
                              "discovery result omitted DESCRIPTIONS");
            reportWriter.appendPlugin (*report);
            return false;
        }

        int discovered = 0;
        int validated = 0;

        for (const auto* item : descriptions->getChildIterator())
        {
            if (shouldExit())
                return true;

            juce::PluginDescription description;

            if (! description.loadFromXml (*item))
                continue;

            ++discovered;
            const auto environment = getEnvironment (fileOrIdentifier,
                                                      &description);

            if (! environment.architectureCompatible)
            {
                auto report = makeArchitectureReport (
                    format.getName(), fileOrIdentifier, &description,
                    environment);
                reportWriter.appendPlugin (*report);
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

            if (validationAttempt.finish == PhaseFinish::cancelled)
                return true;

            auto report = reportForAttempt (validation, validationAttempt,
                                            environment, &description);
            reportWriter.appendPlugin (*report);

            if (report->getStringAttribute ("status") == "loaded_success")
            {
                result.add (new juce::PluginDescription (description));
                ++validated;
            }
        }

        juce::Logger::writeToLog (
            "PLUGIN_SCAN candidate_done format=" + format.getName()
            + " path=" + fileOrIdentifier
            + " discovered=" + juce::String (discovered)
            + " validated=" + juce::String (validated));

        // false faz o JUCE bloquear somente o bundle no qual nenhuma classe
        // sobreviveu. Em shells, qualquer classe valida preserva o bundle e as
        // classes defeituosas continuam documentadas no JSONL.
        return validated > 0;
    }

    void scanFinished() override
    {
        reportWriter.appendSummary (currentFormat);
        juce::Logger::writeToLog (
            "PLUGIN_SCAN finished format=" + currentFormat
            + " report=" + reportWriter.getFile().getFullPathName());
    }

private:
    enum class PhaseFinish
    {
        result,
        crash,
        timeout,
        cancelled,
        launchFailed,
        sendFailed
    };

    struct PhaseAttempt
    {
        PhaseFinish finish = PhaseFinish::launchFailed;
        std::unique_ptr<juce::XmlElement> xml;
        ScannerSuperprocess::Progress progress;
        juce::String stack;
        int elapsedMs = 0;
    };

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
        ScannerSuperprocess process;
        PhaseAttempt attempt;

        if (! process.wasLaunched())
        {
            attempt.finish = PhaseFinish::launchFailed;
            return attempt;
        }

        const auto block = serialiseRequest (request);

        if (! process.sendMessageToWorker (block))
        {
            attempt.finish = PhaseFinish::sendFailed;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchDAWPluginScanner)
};

} // namespace ChurchDAWPluginScan
