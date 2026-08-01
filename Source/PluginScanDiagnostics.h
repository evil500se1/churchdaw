#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <map>
#include <mutex>

#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include <cerrno>
 #include <csignal>
 #include <cstring>
 #include <execinfo.h>
 #include <fcntl.h>
 #include <limits.h>
 #include <unistd.h>

 #if JUCE_MAC
  #include <sys/sysctl.h>
 #endif
#endif

namespace ChurchDAWPluginScan
{

inline constexpr int protocolVersion = 2;

enum class ScanPhase : int
{
    discovery = 1,
    validation = 2
};

inline juce::String toString (ScanPhase phase)
{
    return phase == ScanPhase::discovery ? "discovery" : "validation";
}

struct ScanRequest
{
    ScanPhase phase = ScanPhase::discovery;
    juce::String token;
    juce::String format;
    juce::String identifier;
    juce::String descriptionXml;
    juce::String environmentXml;
};

inline juce::MemoryBlock serialiseRequest (const ScanRequest& request)
{
    juce::MemoryBlock block;
    juce::MemoryOutputStream stream { block, true };
    stream.writeInt (protocolVersion);
    stream.writeInt ((int) request.phase);
    stream.writeString (request.token);
    stream.writeString (request.format);
    stream.writeString (request.identifier);
    stream.writeString (request.descriptionXml);
    stream.writeString (request.environmentXml);
    stream.flush();
    return block;
}

inline bool deserialiseRequest (const juce::MemoryBlock& block,
                                ScanRequest& request,
                                juce::String& error)
{
    juce::MemoryInputStream stream { block, false };

    if (stream.readInt() != protocolVersion)
    {
        error = "unsupported scanner protocol";
        return false;
    }

    const auto rawPhase = stream.readInt();

    if (rawPhase != (int) ScanPhase::discovery
         && rawPhase != (int) ScanPhase::validation)
    {
        error = "invalid scanner phase";
        return false;
    }

    request.phase = (ScanPhase) rawPhase;
    request.token = stream.readString();
    request.format = stream.readString();
    request.identifier = stream.readString();
    request.descriptionXml = stream.readString();
    request.environmentXml = stream.readString();

    if (request.token.isEmpty() || request.format.isEmpty()
         || request.identifier.isEmpty())
    {
        error = "scanner request is missing token, format or identifier";
        return false;
    }

    return true;
}

inline juce::File getReportDirectory()
{
   #if JUCE_MAC
    auto directory = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                         .getChildFile ("Library")
                         .getChildFile ("Logs")
                         .getChildFile ("ChurchDAW")
                         .getChildFile ("PluginScan");
   #else
    auto directory = juce::File::getSpecialLocation (
                         juce::File::userApplicationDataDirectory)
                         .getChildFile ("ChurchDAW")
                         .getChildFile ("PluginScan");
   #endif

    directory.createDirectory();
    return directory;
}

inline juce::String makeScanToken()
{
    const auto random = juce::Random::getSystemRandom().nextInt64();
    return juce::String::toHexString ((juce::int64) juce::Time::currentTimeMillis())
         + "-" + juce::String::toHexString (random);
}

inline juce::String currentProcessArchitecture()
{
   #if JUCE_ARM && JUCE_64BIT
    return "arm64";
   #elif JUCE_ARM
    return "arm";
   #elif JUCE_INTEL && JUCE_64BIT
    return "x86_64";
   #elif JUCE_INTEL
    return "x86";
   #else
    return "unknown";
   #endif
}

inline bool processIsTranslatedByRosetta()
{
   #if JUCE_MAC
    int translated = 0;
    size_t size = sizeof (translated);
    return sysctlbyname ("sysctl.proc_translated", &translated, &size,
                         nullptr, 0) == 0 && translated == 1;
   #else
    return false;
   #endif
}

struct CommandResult
{
    bool launched = false;
    bool timedOut = false;
    juce::uint32 exitCode = 0;
    juce::String output;
};

inline CommandResult runCommand (const juce::StringArray& arguments,
                                 int timeoutMs = 5000)
{
    CommandResult result;
    juce::ChildProcess process;
    result.launched = process.start (arguments,
                                     juce::ChildProcess::wantStdOut
                                       | juce::ChildProcess::wantStdErr);

    if (! result.launched)
        return result;

    if (! process.waitForProcessToFinish (timeoutMs))
    {
        result.timedOut = true;
        process.kill();
        process.waitForProcessToFinish (1000);
    }

    result.output = process.readAllProcessOutput().trim();
    result.exitCode = process.getExitCode();
    return result;
}

inline juce::String normaliseArchitectureOutput (juce::String text)
{
    text = text.toLowerCase();
    const bool arm64 = text.containsWholeWord ("arm64")
                    || text.containsWholeWord ("arm64e");
    const bool x64 = text.containsWholeWord ("x86_64");

    if (arm64 && x64)
        return "Universal (arm64+x86_64)";
    if (arm64)
        return "arm64";
    if (x64)
        return "x86_64";
    if (text.containsWholeWord ("i386") || text.containsWholeWord ("x86"))
        return "x86 (32-bit)";

    return text.isNotEmpty() ? "unknown: " + text.substring (0, 240)
                             : "unknown";
}

inline juce::File resolveBundleExecutable (const juce::File& bundleOrBinary)
{
    if (bundleOrBinary.existsAsFile())
        return bundleOrBinary;

    if (! bundleOrBinary.isDirectory())
        return {};

    const auto macOSDirectory = bundleOrBinary.getChildFile ("Contents")
                                               .getChildFile ("MacOS");
    auto preferred = macOSDirectory.getChildFile (
        bundleOrBinary.getFileNameWithoutExtension());

    if (preferred.existsAsFile())
        return preferred;

    juce::Array<juce::File> files;
    macOSDirectory.findChildFiles (files, juce::File::findFiles, false);
    return files.isEmpty() ? juce::File{} : files.getFirst();
}

inline juce::String normaliseNameForMatching (juce::String text)
{
    juce::String result;

    for (const auto c : text.toLowerCase())
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            result += c;

    return result;
}

inline juce::File resolveAudioUnitBundle (const juce::PluginDescription& desc)
{
   #if JUCE_MAC
    juce::Array<juce::File> roots {
        juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Library/Audio/Plug-Ins/Components"),
        juce::File ("/Library/Audio/Plug-Ins/Components"),
        juce::File ("/System/Library/Audio/Plug-Ins/Components"),
        juce::File ("/System/Library/Components")
    };

    const auto wantedName = normaliseNameForMatching (
        desc.descriptiveName.isNotEmpty() ? desc.descriptiveName : desc.name);
    const auto wantedManufacturer = normaliseNameForMatching (desc.manufacturerName);
    juce::File best;
    int bestScore = 0;

    for (const auto& root : roots)
    {
        if (! root.isDirectory())
            continue;

        juce::Array<juce::File> bundles;
        root.findChildFiles (bundles, juce::File::findDirectories, true,
                             "*.component");

        for (const auto& bundle : bundles)
        {
            const auto candidate = normaliseNameForMatching (
                bundle.getFileNameWithoutExtension());
            int score = 0;

            if (candidate == wantedName)
                score += 100;
            else if (candidate.contains (wantedName)
                      || wantedName.contains (candidate))
                score += 50;

            if (wantedManufacturer.isNotEmpty()
                 && candidate.contains (wantedManufacturer))
                score += 10;

            if (score > bestScore)
            {
                best = bundle;
                bestScore = score;
            }
        }
    }

    return bestScore > 0 ? best : juce::File{};
   #else
    juce::ignoreUnused (desc);
    return {};
   #endif
}

struct EnvironmentReport
{
    juce::String requestedPath;
    juce::String resolvedPath;
    juce::String executablePath;
    juce::String pluginArchitecture = "unknown";
    juce::String hostArchitectures = currentProcessArchitecture();
    juce::String processArchitecture = currentProcessArchitecture();
    juce::String signature = "not checked on this platform";
    juce::String notarization = "not checked on this platform";
    juce::String permissions = "unknown";
    juce::String quarantine = "unknown";
    juce::String hostHardenedRuntime = "unknown";
    juce::String hostSandbox = "unknown";
    juce::String libraryValidation = "unknown";
    bool rosetta = processIsTranslatedByRosetta();
    bool architectureCompatible = true;

    void writeTo (juce::XmlElement& xml) const
    {
        xml.setAttribute ("requestedPath", requestedPath);
        xml.setAttribute ("resolvedPath", resolvedPath);
        xml.setAttribute ("executablePath", executablePath);
        xml.setAttribute ("architecture", pluginArchitecture);
        xml.setAttribute ("hostArchitectures", hostArchitectures);
        xml.setAttribute ("processArchitecture", processArchitecture);
        xml.setAttribute ("rosetta", rosetta);
        xml.setAttribute ("architectureCompatible", architectureCompatible);
        xml.setAttribute ("signature", signature);
        xml.setAttribute ("notarization", notarization);
        xml.setAttribute ("permissions", permissions);
        xml.setAttribute ("quarantine", quarantine);
        xml.setAttribute ("hostHardenedRuntime", hostHardenedRuntime);
        xml.setAttribute ("hostSandbox", hostSandbox);
        xml.setAttribute ("libraryValidation", libraryValidation);
    }

    static EnvironmentReport fromXml (const juce::XmlElement& xml)
    {
        EnvironmentReport report;
        report.requestedPath = xml.getStringAttribute ("requestedPath");
        report.resolvedPath = xml.getStringAttribute ("resolvedPath");
        report.executablePath = xml.getStringAttribute ("executablePath");
        report.pluginArchitecture = xml.getStringAttribute ("architecture", "unknown");
        report.hostArchitectures = xml.getStringAttribute ("hostArchitectures", "unknown");
        report.processArchitecture = xml.getStringAttribute ("processArchitecture", "unknown");
        report.rosetta = xml.getBoolAttribute ("rosetta");
        report.architectureCompatible = xml.getBoolAttribute (
            "architectureCompatible", true);
        report.signature = xml.getStringAttribute ("signature", "unknown");
        report.notarization = xml.getStringAttribute ("notarization", "unknown");
        report.permissions = xml.getStringAttribute ("permissions", "unknown");
        report.quarantine = xml.getStringAttribute ("quarantine", "unknown");
        report.hostHardenedRuntime = xml.getStringAttribute (
            "hostHardenedRuntime", "unknown");
        report.hostSandbox = xml.getStringAttribute ("hostSandbox", "unknown");
        report.libraryValidation = xml.getStringAttribute (
            "libraryValidation", "unknown");
        return report;
    }
};

inline juce::String compactCommandStatus (const CommandResult& result,
                                          const juce::String& success,
                                          const juce::String& failure)
{
    if (! result.launched)
        return "tool unavailable";
    if (result.timedOut)
        return "check timed out";
    if (result.exitCode == 0)
        return result.output.isNotEmpty() ? success + ": " + result.output
                                          : success;

    return failure + " (exit=" + juce::String (result.exitCode) + ")"
         + (result.output.isNotEmpty() ? ": " + result.output : juce::String{});
}

inline EnvironmentReport collectEnvironment (
    const juce::String& identifier,
    const juce::PluginDescription* description = nullptr)
{
    EnvironmentReport report;
    report.requestedPath = identifier;
    juce::File bundle (identifier);

    if (! bundle.exists() && description != nullptr
         && description->pluginFormatName.containsIgnoreCase ("AudioUnit"))
        bundle = resolveAudioUnitBundle (*description);

    if (bundle.exists())
        report.resolvedPath = bundle.getFullPathName();

    const auto executable = resolveBundleExecutable (bundle);

    if (executable.existsAsFile())
        report.executablePath = executable.getFullPathName();

   #if JUCE_MAC
    const auto hostExecutable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile);
    const auto hostLipo = runCommand ({ "/usr/bin/lipo", "-archs",
                                       hostExecutable.getFullPathName() });

    if (hostLipo.launched && ! hostLipo.timedOut && hostLipo.exitCode == 0)
        report.hostArchitectures = normaliseArchitectureOutput (hostLipo.output);

    if (executable.existsAsFile())
    {
        const auto lipo = runCommand ({ "/usr/bin/lipo", "-archs",
                                        executable.getFullPathName() });

        if (lipo.launched && ! lipo.timedOut && lipo.exitCode == 0)
            report.pluginArchitecture = normaliseArchitectureOutput (lipo.output);
        else
        {
            const auto file = runCommand ({ "/usr/bin/file",
                                            executable.getFullPathName() });
            report.pluginArchitecture = normaliseArchitectureOutput (file.output);
        }

        const auto lowerArchitecture = report.pluginArchitecture.toLowerCase();
        report.architectureCompatible =
            lowerArchitecture.startsWith ("unknown")
             || lowerArchitecture.contains (report.processArchitecture.toLowerCase());

        const bool readable = executable.hasReadAccess();
        const bool executableByProcess = ::access (
            executable.getFullPathName().toRawUTF8(), X_OK) == 0;
        report.permissions = juce::String (readable ? "readable" : "not-readable")
                           + ", "
                           + (executableByProcess ? "executable" : "not-executable");

        const auto signedTarget = bundle.exists() ? bundle : executable;
        const auto signature = runCommand ({ "/usr/bin/codesign", "--verify",
                                             "--strict", "--verbose=2",
                                             signedTarget.getFullPathName() });
        report.signature = compactCommandStatus (signature, "valid", "invalid");

        const auto notarization = runCommand ({ "/usr/sbin/spctl", "--assess",
                                                "--type", "execute", "--verbose=4",
                                                signedTarget.getFullPathName() }, 10000);
        report.notarization = compactCommandStatus (
            notarization, "accepted by Gatekeeper", "not accepted by Gatekeeper");

        const auto quarantine = runCommand ({ "/usr/bin/xattr", "-p",
                                              "com.apple.quarantine",
                                              signedTarget.getFullPathName() });
        report.quarantine = quarantine.exitCode == 0
                                ? "present: " + quarantine.output
                                : "absent";
    }
    else
    {
        report.permissions = bundle.exists() && bundle.hasReadAccess()
                                 ? "bundle readable; executable unresolved"
                                 : "path unresolved";
    }

    const auto hostDetails = runCommand ({ "/usr/bin/codesign", "-dv",
                                           "--verbose=4",
                                           hostExecutable.getFullPathName() });
    const auto details = hostDetails.output.toLowerCase();
    report.hostHardenedRuntime = details.contains ("runtime")
                                     ? "enabled" : "not detected";

    const auto entitlements = runCommand ({ "/usr/bin/codesign", "-d",
                                            "--entitlements", ":-",
                                            hostExecutable.getFullPathName() });
    const auto entitlementText = entitlements.output.toLowerCase();
    report.hostSandbox = entitlementText.contains (
        "com.apple.security.app-sandbox") ? "enabled" : "disabled";
    report.libraryValidation = entitlementText.contains (
        "com.apple.security.cs.disable-library-validation")
            ? "disabled (third-party plug-ins allowed)"
            : "enabled or entitlement missing";
   #else
    if (executable.existsAsFile())
    {
        report.pluginArchitecture = currentProcessArchitecture();
        report.permissions = executable.hasReadAccess() ? "readable" : "not-readable";
    }
   #endif

    return report;
}

inline std::unique_ptr<juce::XmlElement> environmentToXml (
    const EnvironmentReport& environment)
{
    auto xml = std::make_unique<juce::XmlElement> ("ENVIRONMENT");
    environment.writeTo (*xml);
    return xml;
}

//==============================================================================
// A crash cannot travel over the IPC channel. The signal handler writes a small
// sidecar containing raw return addresses; the coordinator folds it into the
// per-plugin JSON report after the worker exits.
namespace CrashCapture
{
#if JUCE_MAC || JUCE_LINUX || JUCE_BSD
inline char crashPath[PATH_MAX] = {};
inline char activeStage[128] = {};
inline char activeCall[256] = {};
inline volatile std::sig_atomic_t handlingSignal = 0;

inline void writeSignalSafe (int fd, const char* text)
{
    if (text != nullptr)
        ::write (fd, text, std::strlen (text));
}

inline void writeIntegerSignalSafe (int fd, int value)
{
    char buffer[32];
    int index = (int) sizeof (buffer);
    bool negative = value < 0;
    unsigned int remaining = negative ? (unsigned int) -value
                                      : (unsigned int) value;

    do
    {
        buffer[--index] = (char) ('0' + remaining % 10);
        remaining /= 10;
    }
    while (remaining != 0 && index > 1);

    if (negative)
        buffer[--index] = '-';

    ::write (fd, buffer + index, sizeof (buffer) - (size_t) index);
}

inline void signalHandler (int signalNumber, siginfo_t*, void*)
{
    if (handlingSignal != 0)
        _exit (128 + signalNumber);

    handlingSignal = 1;
    const int fd = ::open (crashPath, O_CREAT | O_WRONLY | O_TRUNC, 0600);

    if (fd >= 0)
    {
        writeSignalSafe (fd, "signal=");
        writeIntegerSignalSafe (fd, signalNumber);
        writeSignalSafe (fd, "\nstage=");
        writeSignalSafe (fd, activeStage);
        writeSignalSafe (fd, "\ncall=");
        writeSignalSafe (fd, activeCall);
        writeSignalSafe (fd, "\nstack:\n");
        void* frames[96];
        const auto count = ::backtrace (frames, (int) std::size (frames));
        ::backtrace_symbols_fd (frames, count, fd);
        ::close (fd);
    }

    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset (&action.sa_mask);
    sigaction (signalNumber, &action, nullptr);
    ::raise (signalNumber);
}

inline void install()
{
    struct sigaction action {};
    action.sa_sigaction = signalHandler;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;

    for (const auto signalNumber : { SIGSEGV, SIGBUS, SIGABRT,
                                     SIGILL, SIGFPE, SIGTRAP })
        sigaction (signalNumber, &action, nullptr);
}

inline void setTarget (const juce::String& token)
{
    const auto path = getReportDirectory()
                          .getChildFile (token + ".crash.txt")
                          .getFullPathName();
    path.copyToUTF8 (crashPath, sizeof (crashPath));
}

inline void setActiveCall (const juce::String& stage,
                           const juce::String& call)
{
    stage.copyToUTF8 (activeStage, sizeof (activeStage));
    call.copyToUTF8 (activeCall, sizeof (activeCall));
}
#else
inline void install() {}
inline void setTarget (const juce::String&) {}
inline void setActiveCall (const juce::String&, const juce::String&) {}
#endif
}

inline juce::String readCrashStack (const juce::String& token)
{
    const auto crashFile = getReportDirectory().getChildFile (
        token + ".crash.txt");
    return crashFile.existsAsFile() ? crashFile.loadFileAsString()
                                    : juce::String{};
}

//==============================================================================
class ReportWriter
{
public:
    ReportWriter()
    {
        const auto timestamp = juce::Time::getCurrentTime().formatted (
            "%Y%m%d-%H%M%S");
        sessionId = timestamp + "-" + makeScanToken().fromLastOccurrenceOf (
            "-", false, false);
        reportFile = getReportDirectory().getChildFile (
            "PluginScan-" + sessionId + ".jsonl");
    }

    const juce::File& getFile() const noexcept { return reportFile; }
    const juce::String& getSessionId() const noexcept { return sessionId; }

    void appendPlugin (const juce::XmlElement& report)
    {
        juce::DynamicObject::Ptr object (new juce::DynamicObject());
        object->setProperty ("recordType", "plugin");
        object->setProperty ("sessionId", sessionId);
        object->setProperty ("recordedAtUtc",
                             juce::Time::getCurrentTime().toISO8601 (true));

        copyAttributes (report, *object);
        juce::Array<juce::var> events;

        for (const auto* child : report.getChildIterator())
        {
            if (! child->hasTagName ("EVENT"))
                continue;

            juce::DynamicObject::Ptr event (new juce::DynamicObject());
            copyAttributes (*child, *event);
            events.add (event.get());
        }

        object->setProperty ("events", std::move (events));
        appendJson (object.get());

        const std::lock_guard<std::mutex> lock (mutex);
        ++counts[report.getStringAttribute ("status", "unknown")];
    }

    void appendSummary (const juce::String& format)
    {
        juce::DynamicObject::Ptr object (new juce::DynamicObject());
        object->setProperty ("recordType", "summary");
        object->setProperty ("sessionId", sessionId);
        object->setProperty ("format", format);
        object->setProperty ("recordedAtUtc",
                             juce::Time::getCurrentTime().toISO8601 (true));

        {
            const std::lock_guard<std::mutex> lock (mutex);
            juce::DynamicObject::Ptr summary (new juce::DynamicObject());

            for (const auto& [status, count] : counts)
                summary->setProperty (status, count);

            object->setProperty ("counts", summary.get());
        }

        appendJson (object.get());
    }

private:
    static void copyAttributes (const juce::XmlElement& xml,
                                juce::DynamicObject& object)
    {
        for (int i = 0; i < xml.getNumAttributes(); ++i)
            object.setProperty (xml.getAttributeName (i),
                                xml.getAttributeValue (i));
    }

    void appendJson (const juce::var& value)
    {
        // JSON Lines exige exatamente um objeto por linha; pretty-print criaria
        // registros de varias linhas e quebraria ferramentas de suporte.
        const auto line = juce::JSON::toString (value, false) + "\n";
        const std::lock_guard<std::mutex> lock (fileMutex);
        reportFile.appendText (line, false, false, "\n");
    }

    juce::File reportFile;
    juce::String sessionId;
    std::mutex mutex;
    std::mutex fileMutex;
    std::map<juce::String, int> counts;
};

inline juce::String diagnosticEventKindToString (
    juce::PluginHostDiagnostics::EventKind kind)
{
    switch (kind)
    {
        case juce::PluginHostDiagnostics::EventKind::begin: return "begin";
        case juce::PluginHostDiagnostics::EventKind::end:   return "end";
        case juce::PluginHostDiagnostics::EventKind::info:  return "info";
    }

    return "unknown";
}

inline std::unique_ptr<juce::XmlElement> diagnosticEventToXml (
    const juce::PluginHostDiagnostics::Event& event,
    const ScanRequest& request)
{
    auto xml = std::make_unique<juce::XmlElement> ("PROGRESS");
    xml->setAttribute ("protocol", protocolVersion);
    xml->setAttribute ("token", request.token);
    xml->setAttribute ("phase", toString (request.phase));
    xml->setAttribute ("stage", event.stage);
    xml->setAttribute ("call", event.call);
    xml->setAttribute ("event", diagnosticEventKindToString (event.kind));
    xml->setAttribute ("success", event.success);
    xml->setAttribute ("critical", event.critical);
    xml->setAttribute ("code", juce::String (event.resultCode));
    xml->setAttribute ("elapsedMs", juce::String (event.elapsedMs));
    xml->setAttribute ("detail", event.detail);
    return xml;
}

} // namespace ChurchDAWPluginScan
