/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-9-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

enum { magicCoordWorkerConnectionHeader = 0x712baf04 };

static const char* startMessage = "__ipc_st";
static const char* killMessage  = "__ipc_k_";
static const char* pingMessage  = "__ipc_p_";
enum { specialMessageSize = 8, defaultTimeoutMs = 8000 };

static bool isMessageType (const MemoryBlock& mb, const char* messageType) noexcept
{
    return mb.matches (messageType, (size_t) specialMessageSize);
}

static String getCommandLinePrefix (const String& commandLineUniqueID)
{
    return "--" + commandLineUniqueID + ":";
}

//==============================================================================
// This thread sends and receives ping messages every second, so that it
// can find out if the other process has stopped running.
struct ChildProcessPingThread : public Thread,
                                private AsyncUpdater
{
    ChildProcessPingThread (int timeout)  : Thread (SystemStats::getJUCEVersion() + ": IPC ping"), timeoutMs (timeout)
    {
        pingReceived();
    }

    void startPinging()                     { startThread (Priority::low); }

    void pingReceived() noexcept            { countdown = timeoutMs / 1000 + 1; }
    void triggerConnectionLostMessage()     { triggerAsyncUpdate(); }

    virtual bool sendPingMessage (const MemoryBlock&) = 0;
    virtual void pingFailed() = 0;

    int timeoutMs;

    using AsyncUpdater::cancelPendingUpdate;

private:
    Atomic<int> countdown;

    void handleAsyncUpdate() override   { pingFailed(); }

    void run() override
    {
        while (! threadShouldExit())
        {
            if (--countdown <= 0 || ! sendPingMessage ({ pingMessage, specialMessageSize }))
            {
                triggerConnectionLostMessage();
                break;
            }

            wait (1000);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChildProcessPingThread)
};

//==============================================================================
struct ChildProcessCoordinator::Connection final : public InterprocessConnection,
                                                   private ChildProcessPingThread
{
    Connection (ChildProcessCoordinator& m, const String& pipeName, int timeout)
        : InterprocessConnection (false, magicCoordWorkerConnectionHeader),
          ChildProcessPingThread (timeout),
          owner (m)
    {
        createPipe (pipeName, timeoutMs);
    }

    ~Connection() override
    {
        cancelPendingUpdate();
        stopThread (10000);
    }

    using ChildProcessPingThread::startPinging;

private:
    void connectionMade() override  {}
    void connectionLost() override  { owner.handleConnectionLost(); }

    bool sendPingMessage (const MemoryBlock& m) override    { return owner.sendMessageToWorker (m); }
    void pingFailed() override                              { connectionLost(); }

    void messageReceived (const MemoryBlock& m) override
    {
        pingReceived();

        if (m.getSize() != specialMessageSize || ! isMessageType (m, pingMessage))
            owner.handleMessageFromWorker (m);
    }

    ChildProcessCoordinator& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Connection)
};

//==============================================================================
ChildProcessCoordinator::ChildProcessCoordinator() = default;

ChildProcessCoordinator::~ChildProcessCoordinator()
{
    killWorkerProcess();
}

void ChildProcessCoordinator::handleConnectionLost() {}

void ChildProcessCoordinator::handleMessageFromWorker (const MemoryBlock& mb)
{
    JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
    handleMessageFromSlave (mb);
    JUCE_END_IGNORE_DEPRECATION_WARNINGS
}

bool ChildProcessCoordinator::sendMessageToWorker (const MemoryBlock& mb)
{
    if (connection != nullptr)
        return connection->sendMessage (mb);

    jassertfalse; // this can only be used when the connection is active!
    return false;
}

bool ChildProcessCoordinator::launchWorkerProcess (const File& executable, const String& commandLineUniqueID,
                                                   int timeoutMs, int streamFlags)
{
    if (! killWorkerProcessAndWait())
        return false;

    auto pipeName = "p" + String::toHexString (Random().nextInt64());

    StringArray args;
    args.add (executable.getFullPathName());
    args.add (getCommandLinePrefix (commandLineUniqueID) + pipeName);

    childProcess = [&]() -> std::shared_ptr<ChildProcess>
    {
        if ((SystemStats::getOperatingSystemType() & SystemStats::Linux) != 0)
            return ChildProcessManager::getInstance()->createAndStartManagedChildProcess (args, streamFlags);

        auto p = std::make_shared<ChildProcess>();

        if (p->start (args, streamFlags))
            return p;

        return nullptr;
    }();

    if (childProcess != nullptr)
    {
        connection.reset (new Connection (*this, pipeName, timeoutMs <= 0 ? defaultTimeoutMs : timeoutMs));

        if (connection->isConnected())
        {
            connection->startPinging();
            sendMessageToWorker ({ startMessage, specialMessageSize });
            return true;
        }

        connection.reset();

        // A process that started but never connected is still our
        // responsibility. Do not leave it behind after a launch failure.
        killWorkerProcessAndWait();
    }

    return false;
}

void ChildProcessCoordinator::killWorkerProcess()
{
    ignoreUnused (killWorkerProcessAndWait());
}

bool ChildProcessCoordinator::killWorkerProcessAndWait (int gracefulShutdownTimeoutMs,
                                                         int forcedKillTimeoutMs)
{
    if (connection != nullptr)
    {
        // Once the worker has exited, sending through the server-side named
        // pipe may wait for a new client for the full IPC timeout. There is no
        // process left to receive a graceful-shutdown request in that case, so
        // close the connection directly instead.
        if (childProcess != nullptr && childProcess->isRunning())
            sendMessageToWorker ({ killMessage, specialMessageSize });

        connection->disconnect();
        connection.reset();
    }

    if (childProcess == nullptr)
        return true;

    const auto process = childProcess;
    const auto gracefulWait = jmax (0, gracefulShutdownTimeoutMs);
    const auto forcedWait = jmax (0, forcedKillTimeoutMs);

    if (! process->waitForProcessToFinish (gracefulWait))
    {
        // kill() may race with a process that exited immediately after the
        // graceful wait. Treat that race as success only after isRunning()
        // confirms the exit; otherwise preserve the handle and fail closed.
        if (! process->kill() && process->isRunning())
            return false;

        if (! process->waitForProcessToFinish (forcedWait))
            return false;
    }

    childProcess.reset();
    return true;
}

bool ChildProcessCoordinator::isWorkerProcessRunning() const
{
    const auto process = childProcess;
    return process != nullptr && process->isRunning();
}

//==============================================================================
struct ChildProcessWorker::Connection final : public InterprocessConnection,
                                              private ChildProcessPingThread
{
    Connection (ChildProcessWorker& p, const String& pipeName, int timeout)
        : InterprocessConnection (false, magicCoordWorkerConnectionHeader),
          ChildProcessPingThread (timeout),
          owner (p)
    {
        connectToPipe (pipeName, timeoutMs);
    }

    ~Connection() override
    {
        cancelPendingUpdate();
        stopThread (10000);
        disconnect();
    }

    using ChildProcessPingThread::startPinging;

private:
    ChildProcessWorker& owner;

    void connectionMade() override  {}
    void connectionLost() override  { owner.handleConnectionLost(); }

    bool sendPingMessage (const MemoryBlock& m) override    { return owner.sendMessageToCoordinator (m); }
    void pingFailed() override                              { connectionLost(); }

    void messageReceived (const MemoryBlock& m) override
    {
        pingReceived();

        if (isMessageType (m, pingMessage))
            return;

        if (isMessageType (m, killMessage))
            return triggerConnectionLostMessage();

        if (isMessageType (m, startMessage))
            return owner.handleConnectionMade();

        owner.handleMessageFromCoordinator (m);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Connection)
};

//==============================================================================
ChildProcessWorker::ChildProcessWorker() = default;
ChildProcessWorker::~ChildProcessWorker() = default;

void ChildProcessWorker::handleConnectionMade() {}
void ChildProcessWorker::handleConnectionLost() {}

void ChildProcessWorker::handleMessageFromCoordinator (const MemoryBlock& mb)
{
    JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
    handleMessageFromMaster (mb);
    JUCE_END_IGNORE_DEPRECATION_WARNINGS
}

bool ChildProcessWorker::sendMessageToCoordinator (const MemoryBlock& mb)
{
    if (connection != nullptr)
        return connection->sendMessage (mb);

    jassertfalse; // this can only be used when the connection is active!
    return false;
}

bool ChildProcessWorker::initialiseFromCommandLine (const String& commandLine,
                                                    const String& commandLineUniqueID,
                                                    int timeoutMs)
{
    auto prefix = getCommandLinePrefix (commandLineUniqueID);

    if (commandLine.trim().startsWith (prefix))
    {
        auto pipeName = commandLine.fromFirstOccurrenceOf (prefix, false, false)
                                   .upToFirstOccurrenceOf (" ", false, false).trim();

        if (pipeName.isNotEmpty())
        {
            connection.reset (new Connection (*this, pipeName, timeoutMs <= 0 ? defaultTimeoutMs : timeoutMs));

            if (connection->isConnected())
                connection->startPinging();
            else
                connection.reset();
        }
    }

    return connection != nullptr;
}

} // namespace juce
