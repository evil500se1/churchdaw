#include <juce_events/juce_events.h>

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <cerrno>
 #include <csignal>
 #include <unistd.h>
#endif

namespace
{
constexpr auto processUID = "ChurchDAWReapTest";

std::uint64_t getCurrentProcessID()
{
   #if JUCE_WINDOWS
    return static_cast<std::uint64_t> (::GetCurrentProcessId());
   #else
    return static_cast<std::uint64_t> (::getpid());
   #endif
}

class DeliberatelyHungWorker final : private juce::ChildProcessWorker
{
public:
    bool initialise (const juce::String& commandLine)
    {
        return initialiseFromCommandLine (commandLine, processUID, 30000);
    }

private:
    void handleConnectionMade() override
    {
        const auto text = "PID:" + juce::String (getCurrentProcessID());
        sendMessageToCoordinator (
            { text.toRawUTF8(), static_cast<size_t> (text.getNumBytesAsUTF8()) });
    }

    // The test intentionally does not exit on the graceful protocol message.
    // Its main thread also never dispatches JUCE messages, modelling a plugin
    // that has wedged the scanner's message thread.
    void handleConnectionLost() override {}
};

class TestCoordinator final : private juce::ChildProcessCoordinator
{
public:
    bool launch (const juce::File& executable)
    {
        return launchWorkerProcess (executable, processUID, 5000, 0);
    }

    std::uint64_t waitForWorkerPID (int timeoutMs)
    {
        std::unique_lock<std::mutex> lock { mutex };

        condition.wait_for (lock, std::chrono::milliseconds { timeoutMs },
                            [this] { return workerPID != 0; });
        return workerPID;
    }

    bool stopAndReap (int gracefulTimeoutMs, int forcedTimeoutMs)
    {
        return killWorkerProcessAndWait (gracefulTimeoutMs, forcedTimeoutMs);
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& block) override
    {
        const auto text = block.toString();

        if (! text.startsWith ("PID:"))
            return;

        const std::lock_guard<std::mutex> lock { mutex };
        workerPID = static_cast<std::uint64_t> (
            text.fromFirstOccurrenceOf (":", false, false).getLargeIntValue());
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t workerPID = 0;
};

#if JUCE_WINDOWS
class ProcessWitness final
{
public:
    explicit ProcessWitness (std::uint64_t processID)
        : handle (::OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, static_cast<DWORD> (processID)))
    {
    }

    ~ProcessWitness()
    {
        if (handle != nullptr)
            ::CloseHandle (handle);
    }

    bool isValid() const noexcept { return handle != nullptr; }

    bool isAlive() const
    {
        return handle != nullptr
            && ::WaitForSingleObject (handle, 0) == WAIT_TIMEOUT;
    }

    bool waitUntilDead (int timeoutMs) const
    {
        return handle != nullptr
            && ::WaitForSingleObject (handle, static_cast<DWORD> (timeoutMs))
                   == WAIT_OBJECT_0;
    }

private:
    HANDLE handle = nullptr;
};
#else
class ProcessWitness final
{
public:
    explicit ProcessWitness (std::uint64_t processID)
        : pid (static_cast<pid_t> (processID))
    {
    }

    bool isValid() const noexcept { return pid > 0; }

    bool isAlive() const
    {
        errno = 0;
        return ::kill (pid, 0) == 0 || errno == EPERM;
    }

    bool waitUntilDead (int timeoutMs) const
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + static_cast<juce::uint32> (timeoutMs);

        do
        {
            if (! isAlive())
                return true;

            juce::Thread::sleep (2);
        }
        while (juce::Time::getMillisecondCounter() < deadline);

        return ! isAlive();
    }

private:
    pid_t pid = 0;
};
#endif

int runHungWorker (const juce::String& commandLine)
{
    DeliberatelyHungWorker worker;

    if (! worker.initialise (commandLine))
        return 2;

    for (;;)
        juce::Thread::sleep (1000);
}

int fail (const juce::String& message)
{
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    juce::StringArray arguments;

    for (int i = 1; i < argc; ++i)
        arguments.add (juce::String::fromUTF8 (argv[i]));

    const auto commandLine = arguments.joinIntoString (" ");

    if (commandLine.startsWith ("--" + juce::String (processUID) + ":"))
        return runHungWorker (commandLine);

    TestCoordinator coordinator;
    const auto executable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile);

    if (! coordinator.launch (executable))
        return fail ("could not launch the deliberately hung worker");

    const auto pid = coordinator.waitForWorkerPID (5000);

    if (pid == 0)
        return fail ("worker did not report its PID");

    ProcessWitness witness { pid };

    if (! witness.isValid() || ! witness.isAlive())
        return fail ("OS did not observe the reported worker PID as alive");

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const bool terminated = coordinator.stopAndReap (100, 2000);
    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - started;

    if (! terminated)
        return fail ("killWorkerProcessAndWait reported a termination failure");

    if (! witness.waitUntilDead (1000))
        return fail ("worker PID remained alive after forced termination");

    if (! coordinator.stopAndReap (0, 0))
        return fail ("repeated termination was not idempotent");

    std::cout << "Connected child reap test passed: PID=" << pid
              << " forced_after_ms=" << elapsedMs << '\n';
    return 0;
}
