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

namespace PluginHostDiagnostics
{
namespace
{
CriticalSection callbackLock;
Callback callback;
std::atomic<bool> callbackEnabled { false };
}

void setCallback (Callback newCallback)
{
    const ScopedLock lock (callbackLock);
    callback = std::move (newCallback);
    callbackEnabled.store (callback != nullptr, std::memory_order_release);
}

void clearCallback()
{
    const ScopedLock lock (callbackLock);
    callbackEnabled.store (false, std::memory_order_release);
    callback = {};
}

bool isEnabled() noexcept
{
    return callbackEnabled.load (std::memory_order_acquire);
}

void report (EventKind kind, const char* stage, const char* call,
             bool success, int64 resultCode, bool critical,
             int64 elapsedMs, const String& detail)
{
    if (! isEnabled())
        return;

    Callback callbackCopy;

    {
        const ScopedLock lock (callbackLock);
        callbackCopy = callback;
    }

    if (callbackCopy != nullptr)
        callbackCopy ({ kind, stage, call, detail, resultCode,
                        elapsedMs, success, critical });
}

ScopedCall::ScopedCall (const char* stageToUse, const char* callToUse,
                        bool isCritical, const String& detail)
    : stage (stageToUse), call (callToUse),
      startMs (Time::getMillisecondCounter()),
      active (isEnabled()), critical (isCritical)
{
    if (active)
        report (EventKind::begin, stage, call, true, 0, critical, 0, detail);
}

ScopedCall::~ScopedCall()
{
    if (active && ! finished)
        report (EventKind::end, stage, call, false,
                std::numeric_limits<int64>::min(), critical,
                (int64) (Time::getMillisecondCounter() - startMs),
                "call unwound without reporting a result");
}

void ScopedCall::finish (bool success, int64 resultCode, const String& detail)
{
    if (finished)
        return;

    finished = true;

    if (active)
        report (EventKind::end, stage, call, success, resultCode, critical,
                (int64) (Time::getMillisecondCounter() - startMs), detail);
}
}

AudioPluginFormat::AudioPluginFormat() {}
AudioPluginFormat::~AudioPluginFormat() {}

std::unique_ptr<AudioPluginInstance> AudioPluginFormat::createInstanceFromDescription (const PluginDescription& desc,
                                                                                       double initialSampleRate,
                                                                                       int initialBufferSize)
{
    String errorMessage;
    return createInstanceFromDescription (desc, initialSampleRate, initialBufferSize, errorMessage);
}

std::unique_ptr<AudioPluginInstance> AudioPluginFormat::createInstanceFromDescription (const PluginDescription& desc,
                                                                                       double initialSampleRate,
                                                                                       int initialBufferSize,
                                                                                       String& errorMessage)
{
    if (MessageManager::getInstance()->isThisTheMessageThread()
          && requiresUnblockedMessageThreadDuringCreation (desc))
    {
        errorMessage = NEEDS_TRANS ("This plug-in cannot be instantiated synchronously");
        return {};
    }

    WaitableEvent finishedSignal;
    std::unique_ptr<AudioPluginInstance> instance;

    auto callback = [&] (std::unique_ptr<AudioPluginInstance> p, const String& error)
    {
       errorMessage = error;
       instance = std::move (p);
       finishedSignal.signal();
    };

    if (! MessageManager::getInstance()->isThisTheMessageThread())
        createPluginInstanceAsync (desc, initialSampleRate, initialBufferSize, std::move (callback));
    else
        createPluginInstance (desc, initialSampleRate, initialBufferSize, std::move (callback));

    finishedSignal.wait();
    return instance;
}

struct AudioPluginFormat::AsyncCreateMessage final : public Message
{
    AsyncCreateMessage (const PluginDescription& d, double sr, int size, PluginCreationCallback call)
        : desc (d), sampleRate (sr), bufferSize (size), callbackToUse (std::move (call))
    {
    }

    PluginDescription desc;
    double sampleRate;
    int bufferSize;
    PluginCreationCallback callbackToUse;
};

void AudioPluginFormat::createPluginInstanceAsync (const PluginDescription& description,
                                                   double initialSampleRate, int initialBufferSize,
                                                   PluginCreationCallback callback)
{
    jassert (callback != nullptr);
    postMessage (new AsyncCreateMessage (description, initialSampleRate, initialBufferSize, std::move (callback)));
}

void AudioPluginFormat::handleMessage (const Message& message)
{
    if (auto m = dynamic_cast<const AsyncCreateMessage*> (&message))
        createPluginInstance (m->desc, m->sampleRate, m->bufferSize, std::move (m->callbackToUse));
}

} // namespace juce
