#pragma once

#include <juce_core/juce_core.h>
#include <functional>

// Controle remoto local do palco. O servidor fica em uma thread separada e
// nunca chama o motor diretamente: o callback do dono deve usar callAsync.
class ChurchDAWRemoteServer final : private juce::Thread
{
public:
    using Handler = std::function<juce::String (const juce::String&)>;

    explicit ChurchDAWRemoteServer (int portToUse = 19765)
        : juce::Thread ("ChurchDAW Remote"), port (portToUse) {}

    ~ChurchDAWRemoteServer() override { stop(); }

    void setHandler (Handler newHandler)
    {
        const juce::ScopedLock lock (handlerLock);
        handler = std::move (newHandler);
    }

    bool start()
    {
        if (isThreadRunning())
            return true;
        return startThread();
    }

    void stop()
    {
        signalThreadShouldExit();
        if (server != nullptr)
            server->close();
        stopThread (1500);
        server.reset();
    }

    int getPort() const noexcept { return port; }

private:
    void run() override
    {
        auto listener = std::make_unique<juce::StreamingSocket>();
        // Escuta a rede local para permitir que um celular no mesmo Wi-Fi
        // controle o palco. As ações são protegidas por token no handler.
        if (! listener->createListener (port, {}))
            return;

        server = std::move (listener);
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> client (
                server->waitForNextConnection());
            if (client == nullptr)
                continue;

            char buffer[4096] {};
            if (! client->waitUntilReady (true, 1500))
                continue;
            // A requisição HTTP termina em CRLF e quase nunca preenche o
            // buffer inteiro; bloquear pela quantidade total faria o celular
            // esperar até o timeout sem receber resposta.
            const auto bytes = client->read (buffer, (int) sizeof (buffer) - 1,
                                             false);
            if (bytes <= 0)
                continue;
            buffer[bytes] = 0;

            const juce::String request (buffer);
            const auto firstLine = request.upToFirstOccurrenceOf ("\r\n", false, false);
            const auto path = firstLine.fromFirstOccurrenceOf (" ", false, false)
                .upToFirstOccurrenceOf (" ", false, false).trim();

            Handler local;
            {
                const juce::ScopedLock lock (handlerLock);
                local = handler;
            }
            const auto body = local != nullptr ? local (path)
                                               : "{\"error\":\"offline\"}";
            const auto response = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json; charset=utf-8\r\n"
                                  "Cache-Control: no-store\r\n"
                                  "Connection: close\r\n"
                                  "Content-Length: "
                                  + juce::String (body.getNumBytesAsUTF8())
                                  + "\r\n\r\n" + body;
            client->write (response.toRawUTF8(), response.getNumBytesAsUTF8());
            client->close();
        }
    }

    const int port;
    std::unique_ptr<juce::StreamingSocket> server;
    juce::CriticalSection handlerLock;
    Handler handler;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChurchDAWRemoteServer)
};
