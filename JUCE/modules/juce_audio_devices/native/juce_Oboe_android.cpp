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

#ifndef JUCE_OBOE_LOG_ENABLED
 #define JUCE_OBOE_LOG_ENABLED 1
#endif

#if JUCE_OBOE_LOG_ENABLED
 #define JUCE_OBOE_LOG(x) DBG(x)
#else
 #define JUCE_OBOE_LOG(x) {}
#endif

namespace juce
{

template <typename OboeDataFormat>  struct OboeAudioIODeviceBufferHelpers {};

template <>
struct OboeAudioIODeviceBufferHelpers<int16>
{
    static oboe::AudioFormat oboeAudioFormat() { return oboe::AudioFormat::I16; }

    static constexpr int bitDepth() { return 16; }

    static bool referAudioBufferDirectlyToOboeIfPossible (int16*, AudioBuffer<float>&, int)  { return false; }

    using NativeInt16   = AudioData::Format<AudioData::Int16, AudioData::NativeEndian>;
    using NativeFloat32 = AudioData::Format<AudioData::Float32, AudioData::NativeEndian>;

    static void convertFromOboe (const int16* srcInterleaved, AudioBuffer<float>& audioBuffer, int numSamples)
    {
        const auto numChannels = audioBuffer.getNumChannels();

        AudioData::deinterleaveSamples (AudioData::InterleavedSource<NativeInt16>    { reinterpret_cast<const uint16*> (srcInterleaved), numChannels },
                                        AudioData::NonInterleavedDest<NativeFloat32> { audioBuffer.getArrayOfWritePointers(),            numChannels },
                                        numSamples);
    }

    static void convertToOboe (const AudioBuffer<float>& audioBuffer, int16* dstInterleaved, int numSamples)
    {
        const auto numChannels = audioBuffer.getNumChannels();

        AudioData::interleaveSamples (AudioData::NonInterleavedSource<NativeFloat32> { audioBuffer.getArrayOfReadPointers(),       numChannels },
                                      AudioData::InterleavedDest<NativeInt16>        { reinterpret_cast<uint16*> (dstInterleaved), numChannels },
                                      numSamples);
    }
};

template <>
struct OboeAudioIODeviceBufferHelpers<float>
{
    static oboe::AudioFormat oboeAudioFormat() { return oboe::AudioFormat::Float; }

    static constexpr int bitDepth() { return 32; }

    static bool referAudioBufferDirectlyToOboeIfPossible (float* nativeBuffer, AudioBuffer<float>& audioBuffer, int numSamples)
    {
        if (audioBuffer.getNumChannels() == 1)
        {
            audioBuffer.setDataToReferTo (&nativeBuffer, 1, numSamples);
            return true;
        }

        return false;
    }

    using Format = AudioData::Format<AudioData::Float32, AudioData::NativeEndian>;

    static void convertFromOboe (const float* srcInterleaved, AudioBuffer<float>& audioBuffer, int numSamples)
    {
        auto numChannels = audioBuffer.getNumChannels();

        if (numChannels > 0)
        {
            // No need to convert, we instructed the buffer to point to the src data directly already
            jassert (audioBuffer.getWritePointer (0) != srcInterleaved);

            AudioData::deinterleaveSamples (AudioData::InterleavedSource<Format>  { srcInterleaved,                        numChannels },
                                            AudioData::NonInterleavedDest<Format> { audioBuffer.getArrayOfWritePointers(), numChannels },
                                            numSamples);
        }
    }

    static void convertToOboe (const AudioBuffer<float>& audioBuffer, float* dstInterleaved, int numSamples)
    {
        auto numChannels = audioBuffer.getNumChannels();

        if (numChannels > 0)
        {
            // No need to convert, we instructed the buffer to point to the src data directly already
            jassert (audioBuffer.getReadPointer (0) != dstInterleaved);

            AudioData::interleaveSamples (AudioData::NonInterleavedSource<Format> { audioBuffer.getArrayOfReadPointers(), numChannels },
                                          AudioData::InterleavedDest<Format>      { dstInterleaved,                       numChannels },
                                          numSamples);
        }
    }
};

template <typename Type>
static String getOboeString (const Type& value)
{
    return String (oboe::convertToText (value));
}

//==============================================================================
class OboeAudioIODevice final : public AudioIODevice
{
public:
    //==============================================================================
    OboeAudioIODevice (const String& deviceName,
                       int inputDeviceIdToUse,
                       const Array<int>& supportedInputSampleRatesToUse,
                       int maxNumInputChannelsToUse,
                       int outputDeviceIdToUse,
                       const Array<int>& supportedOutputSampleRatesToUse,
                       int maxNumOutputChannelsToUse)
        : AudioIODevice (deviceName, oboeTypeName),
          inputDeviceId (inputDeviceIdToUse),
          supportedInputSampleRates (supportedInputSampleRatesToUse),
          maxNumInputChannels (maxNumInputChannelsToUse),
          outputDeviceId (outputDeviceIdToUse),
          supportedOutputSampleRates (supportedOutputSampleRatesToUse),
          maxNumOutputChannels (maxNumOutputChannelsToUse)
    {
    }

    ~OboeAudioIODevice() override
    {
        close();
    }

    StringArray getOutputChannelNames() override    { return getChannelNames (false); }
    StringArray getInputChannelNames() override     { return getChannelNames (true); }

    Array<double> getAvailableSampleRates() override
    {
        Array<double> result;

        auto inputSampleRates  = getAvailableSampleRates (true);
        auto outputSampleRates = getAvailableSampleRates (false);

        if (inputDeviceId == -1)
        {
            for (auto& sr : outputSampleRates)
                result.add (sr);
        }
        else if (outputDeviceId == -1)
        {
            for (auto& sr : inputSampleRates)
                result.add (sr);
        }
        else
        {
            // For best performance, the same sample rate should be used for input and output,
            for (auto& inputSampleRate : inputSampleRates)
            {
                if (outputSampleRates.contains (inputSampleRate))
                    result.add (inputSampleRate);
            }
        }

        // either invalid device was requested or its input&output don't have compatible sample rate
        jassert (result.size() > 0);
        return result;
    }

    Array<int> getAvailableBufferSizes() override
    {
        return AndroidHighPerformanceAudioHelpers::getAvailableBufferSizes (getNativeBufferSize(), getAvailableSampleRates());
    }

    String open (const BigInteger& inputChannels, const BigInteger& outputChannels,
                 double requestedSampleRate, int bufferSize) override
    {
        close();

        lastError.clear();

        sampleRate = (int) (requestedSampleRate > 0 ? requestedSampleRate : AndroidHighPerformanceAudioHelpers::getNativeSampleRate());
        actualBufferSize = (bufferSize <= 0) ? getDefaultBufferSize() : bufferSize;

        // The device may report no max, claiming "no limits". Pick sensible defaults.
        int maxOutChans = maxNumOutputChannels > 0 ? maxNumOutputChannels : 2;
        int maxInChans  = maxNumInputChannels  > 0 ? maxNumInputChannels : 1;

        activeOutputChans = outputChannels;
        activeOutputChans.setRange (maxOutChans,
                                    activeOutputChans.getHighestBit() + 1 - maxOutChans,
                                    false);

        activeInputChans = inputChannels;
        activeInputChans.setRange (maxInChans,
                                   activeInputChans.getHighestBit() + 1 - maxInChans,
                                   false);

        int numOutputChans = activeOutputChans.countNumberOfSetBits();
        int numInputChans = activeInputChans.countNumberOfSetBits();

        if (numInputChans > 0 && (! RuntimePermissions::isGranted (RuntimePermissions::recordAudio)))
        {
            // If you hit this assert, you probably forgot to get RuntimePermissions::recordAudio
            // before trying to open an audio input device. This is not going to work!
            jassertfalse;
            lastError = "Error opening Oboe input device: the app was not granted android.permission.RECORD_AUDIO";
        }

        // At least one output channel should be set!
        jassert (numOutputChans >= 0);

        session.reset (OboeSessionBase::create (*this,
                                                inputDeviceId, outputDeviceId,
                                                numInputChans, numOutputChans,
                                                sampleRate, actualBufferSize));

        deviceOpen = session != nullptr;

        if (! deviceOpen)
            lastError = "Failed to create audio session";

        return lastError;
    }

    void close() override                               { stop(); }
    int getOutputLatencyInSamples() override            { return session->getOutputLatencyInSamples(); }
    int getInputLatencyInSamples() override             { return session->getInputLatencyInSamples(); }
    bool isOpen() override                              { return deviceOpen; }
    int getCurrentBufferSizeSamples() override          { return actualBufferSize; }
    int getCurrentBitDepth() override                   { return session->getCurrentBitDepth(); }
    BigInteger getActiveOutputChannels() const override { return activeOutputChans; }
    BigInteger getActiveInputChannels() const override  { return activeInputChans; }
    String getLastError() override                      { return lastError; }
    bool isPlaying() override                           { return callback.get() != nullptr; }
    int getXRunCount() const noexcept override          { return session->getXRunCount(); }

    int getDefaultBufferSize() override
    {
        return AndroidHighPerformanceAudioHelpers::getDefaultBufferSize (getNativeBufferSize(), getCurrentSampleRate());
    }

    double getCurrentSampleRate() override
    {
        return (sampleRate == 0.0 ? AndroidHighPerformanceAudioHelpers::getNativeSampleRate() : sampleRate);
    }

    void start (AudioIODeviceCallback* newCallback) override
    {
        if (callback.get() != newCallback)
        {
            if (newCallback != nullptr)
                newCallback->audioDeviceAboutToStart (this);

            AudioIODeviceCallback* oldCallback = callback.get();

            if (oldCallback != nullptr)
            {
                // already running
                if (newCallback == nullptr)
                    stop();
                else
                    setCallback (newCallback);

                oldCallback->audioDeviceStopped();
            }
            else
            {
                jassert (newCallback != nullptr);

                // session hasn't started yet
                setCallback (newCallback);
                running = true;

                session->start();
            }

            callback = newCallback;
        }
    }

    void stop() override
    {
        if (session != nullptr)
            session->stop();

        running = false;
        setCallback (nullptr);
    }

    bool setAudioPreprocessingEnabled (bool) override
    {
        // Oboe does not expose this setting, yet it may use preprocessing
        // for older APIs running OpenSL
        return false;
    }

    static const char* const oboeTypeName;

private:
    StringArray getChannelNames (bool forInput)
    {
        auto& deviceId = forInput ? inputDeviceId : outputDeviceId;
        auto& numChannels = forInput ? maxNumInputChannels : maxNumOutputChannels;

        // If the device id is unknown (on olders APIs) or if the device claims to
        // support "any" channel count, use a sensible default
        if (deviceId == -1 || numChannels == -1)
            return forInput ? StringArray ("Input") : StringArray ("Left", "Right");

        StringArray names;

        for (int i = 0; i < numChannels; ++i)
            names.add ("Channel " + String (i + 1));

        return names;
    }

    Array<int> getAvailableSampleRates (bool forInput)
    {
        auto& supportedSampleRates = forInput
            ? supportedInputSampleRates
            : supportedOutputSampleRates;

        if (! supportedSampleRates.isEmpty())
            return supportedSampleRates;

        // device claims that it supports "any" sample rate, use
        // standard ones then
        return getDefaultSampleRates();
    }

    static Array<int> getDefaultSampleRates()
    {
        static const int standardRates[] = { 8000, 11025, 12000, 16000,
                                            22050, 24000, 32000, 44100, 48000 };

        Array<int> rates (standardRates, numElementsInArray (standardRates));

        // make sure the native sample rate is part of the list
        int native = (int) AndroidHighPerformanceAudioHelpers::getNativeSampleRate();

        if (native != 0 && ! rates.contains (native))
            rates.add (native);

        return rates;
    }

    static int getNativeBufferSize()
    {
        auto bufferSizeHint = AndroidHighPerformanceAudioHelpers::getNativeBufferSizeHint();

        // providing a callback is required on some devices to get a FAST track, so we pass an
        // empty one to the temp stream to get the best available buffer size
        struct DummyCallback final : public oboe::AudioStreamCallback
        {
            oboe::DataCallbackResult onAudioReady (oboe::AudioStream*, void*, int32_t) override  { return oboe::DataCallbackResult::Stop; }
        };

        DummyCallback callback;

        // NB: Exclusive mode could be rejected if a device is already opened in that mode, so to get
        //     reliable results, only use this function when a device is closed.
        //     We initially try to open a stream with a buffer size returned from
        //     android.media.property.OUTPUT_FRAMES_PER_BUFFER property, but then we verify the actual
        //     size after the stream is open.
        OboeAudioIODevice::OboeStream tempStream (oboe::kUnspecified,
                                                  oboe::Direction::Output,
                                                  oboe::SharingMode::Exclusive,
                                                  2,
                                                  oboe::AudioFormat::Float,
                                                  (int) AndroidHighPerformanceAudioHelpers::getNativeSampleRate(),
                                                  bufferSizeHint,
                                                  &callback);

        if (auto nativeStream = tempStream.getNativeStream())
            return nativeStream->getFramesPerBurst();

        return bufferSizeHint;
    }

    void setCallback (AudioIODeviceCallback* callbackToUse)
    {
        if (! running)
        {
            callback.set (callbackToUse);
            return;
        }

        // Setting nullptr callback is allowed only when playback is stopped.
        jassert (callbackToUse != nullptr);

        for (;;)
        {
            auto old = callback.get();

            if (old == callbackToUse)
                break;

            // If old is nullptr, then it means that it's currently being used!
            if (old != nullptr && callback.compareAndSetBool (callbackToUse, old))
                break;

            Thread::sleep (1);
        }
    }

    void process (const float* const* inputChannelData, int numInputChannels,
                  float* const* outputChannelData, int numOutputChannels, int32_t numFrames)
    {
        if (auto* cb = callback.exchange (nullptr))
        {
            cb->audioDeviceIOCallbackWithContext (inputChannelData,
                                                  numInputChannels,
                                                  outputChannelData,
                                                  numOutputChannels,
                                                  numFrames,
                                                  {});
            callback.set (cb);
        }
        else
        {
            for (int i = 0; i < numOutputChannels; ++i)
                zeromem (outputChannelData[i], (size_t) (numFrames) * sizeof (float));
        }
    }

    //==============================================================================
    class OboeStream
    {
    public:
        OboeStream (int deviceId, oboe::Direction direction,
                    oboe::SharingMode sharingMode,
                    int channelCount, oboe::AudioFormat format,
                    int32 sampleRateIn, int32 bufferSize,
                    oboe::AudioStreamCallback* callbackIn = nullptr)
        {
            open (deviceId, direction, sharingMode, channelCount,
                  format, sampleRateIn, bufferSize, callbackIn);
        }

        ~OboeStream()
        {
            close();
        }

        bool openedOk() const noexcept
        {
            return openResult == oboe::Result::OK;
        }

        void start()
        {
            jassert (openedOk());

            if (openedOk() && stream != nullptr)
            {
                auto expectedState = oboe::StreamState::Starting;
                auto nextState = oboe::StreamState::Started;
                int64 timeoutNanos = 1000 * oboe::kNanosPerMillisecond;

                [[maybe_unused]] auto startResult = stream->requestStart();
                JUCE_OBOE_LOG ("Requested Oboe stream start with result: " + getOboeString (startResult));

                startResult = stream->waitForStateChange (expectedState, &nextState, timeoutNanos);

                JUCE_OBOE_LOG ("Starting Oboe stream with result: " + getOboeString (startResult)
                                 + "\nUses AAudio = " + String ((int) stream->usesAAudio())
                                 + "\nDirection = " + getOboeString (stream->getDirection())
                                 + "\nSharingMode = " + getOboeString (stream->getSharingMode())
                                 + "\nChannelCount = " + String (stream->getChannelCount())
                                 + "\nFormat = " + getOboeString (stream->getFormat())
                                 + "\nSampleRate = " + String (stream->getSampleRate())
                                 + "\nBufferSizeInFrames = " + String (stream->getBufferSizeInFrames())
                                 + "\nBufferCapacityInFrames = " + String (stream->getBufferCapacityInFrames())
                                 + "\nFramesPerBurst = " + String (stream->getFramesPerBurst())
                                 + "\nFramesPerCallback = " + String (stream->getFramesPerCallback())
                                 + "\nBytesPerFrame = " + String (stream->getBytesPerFrame())
                                 + "\nBytesPerSample = " + String (stream->getBytesPerSample())
                                 + "\nPerformanceMode = " + getOboeString (stream->getPerformanceMode())
                                 + "\ngetDeviceId = " + String (stream->getDeviceId()));
            }
        }

        std::shared_ptr<oboe::AudioStream> getNativeStream() const
        {
            jassert (openedOk());
            return stream;
        }

        int getXRunCount() const
        {
            if (stream != nullptr)
            {
                auto count = stream->getXRunCount();

                if (count)
                    return count.value();

                JUCE_OBOE_LOG ("Failed to get Xrun count: " + getOboeString (count.error()));
            }

            return 0;
        }

    private:
        void open (int deviceId, oboe::Direction direction,
                   oboe::SharingMode sharingMode,
                   int channelCount, oboe::AudioFormat format,
                   int32 newSampleRate, int32 newBufferSize,
                   oboe::AudioStreamCallback* newCallback = nullptr)
        {
            oboe::DefaultStreamValues::FramesPerBurst = AndroidHighPerformanceAudioHelpers::getNativeBufferSizeHint();

            oboe::AudioStreamBuilder builder;

            if (deviceId != -1)
                builder.setDeviceId (deviceId);

            // Note: letting OS to choose the buffer capacity & frames per callback.
            builder.setDirection (direction);
            builder.setSharingMode (sharingMode);
            builder.setChannelCount (channelCount);
            builder.setFormat (format);
            builder.setSampleRate (newSampleRate);
            builder.setPerformanceMode (oboe::PerformanceMode::LowLatency);

            // [ChurchDAW] TAMANHO DO BLOCO ENTREGUE AO APLICATIVO.
            //
            // Sem esta linha o JUCE define apenas setBufferSizeInFrames, que e a
            // CAPACIDADE do stream (latencia) -- e nao o tamanho do bloco que
            // chega no callback. O AAudio entao entrega sempre a rajada nativa,
            // 256 quadros neste aparelho, mesmo com o app pedindo 1024.
            //
            // O efeito e caro: o custo FIXO por chamada (percorrer o grafo, cinco
            // camadas, efeitos, master) passa a ser pago 188 vezes por segundo em
            // vez de 47. Medido no aparelho, com 23 vozes: media de 75% do prazo,
            // picos de 116% e 54 blocos perdidos por segundo. Com o bloco maior o
            // mesmo trabalho cabe folgado.
            //
            // MODIFICACAO NA COPIA LOCAL DO JUCE. Se um dia atualizarem o JUCE,
            // este bloco precisa ser reaplicado.
            //
            // REATIVADO (2026-08-11), desta vez COMPLETO. A primeira tentativa
            // (so setFramesPerCallback) morreu porque a capacidade ficou no
            // default (2048): blocos de 1024 deixavam margem de DOIS blocos, a
            // margem esvaziava e o stream morria em silencio. O ingrediente
            // que faltava e a capacidade explicita de 4x o bloco: com ela o
            // track legacy tem folga real para jitter de agendamento.
            //
            // Sem este bloco o AAudio entrega SEMPRE a rajada nativa (256
            // quadros aqui = 5,3 ms de prazo), independente do buffer pedido:
            // "2048" so engordava o colchao. Medido no aparelho (debug): media
            // de 252% do prazo em IDLE com blocos de 256 — todo callback
            // estourado. O custo fixo do grafo (15 slots, camadas, master) se
            // paga 188 vezes/s em vez de 23. Com o bloco seguindo o tamanho
            // pedido, o mesmo trabalho cabe com folga e o seletor de buffer
            // do app passa a significar o que promete.
            if (newBufferSize > 0)
            {
                builder.setFramesPerCallback (newBufferSize);
                builder.setBufferCapacityInFrames (jmax (newBufferSize * 4, (int32) 2048));
            }

           #if JUCE_USE_ANDROID_OBOE_STABILIZED_CALLBACK
            if (newCallback != nullptr)
            {
                stabilizedCallback = std::make_unique<oboe::StabilizedCallback> (newCallback);
                builder.setCallback (stabilizedCallback.get());
            }
           #else
            builder.setCallback (newCallback);
           #endif

            JUCE_OBOE_LOG (String ("Preparing Oboe stream with params:")
                 + "\nAAudio supported = " + String (int (builder.isAAudioSupported()))
                 + "\nAPI = " + getOboeString (builder.getAudioApi())
                 + "\nDeviceId = " + String (deviceId)
                 + "\nDirection = " + getOboeString (direction)
                 + "\nSharingMode = " + getOboeString (sharingMode)
                 + "\nChannelCount = " + String (channelCount)
                 + "\nFormat = " + getOboeString (format)
                 + "\nSampleRate = " + String (newSampleRate)
                 + "\nPerformanceMode = " + getOboeString (oboe::PerformanceMode::LowLatency));

            openResult = builder.openStream (stream);
            JUCE_OBOE_LOG ("Building Oboe stream with result: " + getOboeString (openResult)
                 + "\nStream state = " + (stream != nullptr ? getOboeString (stream->getState()) : String ("?")));

            if (stream != nullptr && newBufferSize != 0)
            {
                const auto capacity = jmax (newBufferSize, stream->getBufferCapacityInFrames());
                const auto burst = jmax ((int32) 1, stream->getFramesPerBurst());
                const auto targetActiveFrames = jmin (capacity, jmax (newBufferSize * 2, burst * 3));
                JUCE_OBOE_LOG ("Setting the bufferSizeInFrames to " + String (targetActiveFrames)
                               + " (callback=" + String (newBufferSize)
                               + ", burst=" + String (burst)
                               + ", capacity=" + String (capacity) + ")");
                stream->setBufferSizeInFrames (targetActiveFrames);
            }

            JUCE_OBOE_LOG (String ("Stream details:")
                 + "\nUses AAudio = " + (stream != nullptr ? String ((int) stream->usesAAudio()) : String ("?"))
                 + "\nDeviceId = " + (stream != nullptr ? String (stream->getDeviceId()) : String ("?"))
                 + "\nDirection = " + (stream != nullptr ? getOboeString (stream->getDirection()) : String ("?"))
                 + "\nSharingMode = " + (stream != nullptr ? getOboeString (stream->getSharingMode()) : String ("?"))
                 + "\nChannelCount = " + (stream != nullptr ? String (stream->getChannelCount()) : String ("?"))
                 + "\nFormat = " + (stream != nullptr ? getOboeString (stream->getFormat()) : String ("?"))
                 + "\nSampleRate = " + (stream != nullptr ? String (stream->getSampleRate()) : String ("?"))
                 + "\nBufferSizeInFrames = " + (stream != nullptr ? String (stream->getBufferSizeInFrames()) : String ("?"))
                 + "\nBufferCapacityInFrames = " + (stream != nullptr ? String (stream->getBufferCapacityInFrames()) : String ("?"))
                 + "\nFramesPerBurst = " + (stream != nullptr ? String (stream->getFramesPerBurst()) : String ("?"))
                 + "\nFramesPerCallback = " + (stream != nullptr ? String (stream->getFramesPerCallback()) : String ("?"))
                 + "\nBytesPerFrame = " + (stream != nullptr ? String (stream->getBytesPerFrame()) : String ("?"))
                 + "\nBytesPerSample = " + (stream != nullptr ? String (stream->getBytesPerSample()) : String ("?"))
                 + "\nPerformanceMode = " + (stream != nullptr ? getOboeString (stream->getPerformanceMode()) : String ("?")));
        }

        void close()
        {
            if (stream != nullptr)
            {
                [[maybe_unused]] oboe::Result result = stream->close();
                JUCE_OBOE_LOG ("Requested Oboe stream close with result: " + getOboeString (result));
            }
        }

        std::shared_ptr<oboe::AudioStream> stream;
       #if JUCE_USE_ANDROID_OBOE_STABILIZED_CALLBACK
        std::unique_ptr<oboe::StabilizedCallback> stabilizedCallback;
       #endif
        oboe::Result openResult;
    };

    //==============================================================================
    class OboeSessionBase : protected oboe::AudioStreamCallback
    {
    public:
        static OboeSessionBase* create (OboeAudioIODevice& owner,
                                        int inputDeviceId, int outputDeviceId,
                                        int numInputChannels, int numOutputChannels,
                                        int sampleRate, int bufferSize);

        virtual void start() = 0;
        virtual void stop() = 0;
        virtual int getOutputLatencyInSamples() = 0;
        virtual int getInputLatencyInSamples() = 0;

        bool openedOk() const noexcept
        {
            if (inputStream != nullptr && ! inputStream->openedOk())
                return false;

            return outputStream != nullptr && outputStream->openedOk();
        }

        int getCurrentBitDepth() const noexcept { return bitDepth; }

        int getXRunCount() const
        {
            int inputXRunCount  = jmax (0, inputStream  != nullptr ? inputStream->getXRunCount() : 0);
            int outputXRunCount = jmax (0, outputStream != nullptr ? outputStream->getXRunCount() : 0);

            return inputXRunCount + outputXRunCount;
        }

    protected:
        OboeSessionBase (OboeAudioIODevice& ownerToUse,
                         int inputDeviceIdToUse, int outputDeviceIdToUse,
                         int numInputChannelsToUse, int numOutputChannelsToUse,
                         int sampleRateToUse, int bufferSizeToUse,
                         oboe::AudioFormat streamFormatToUse,
                         int bitDepthToUse)
            : owner (ownerToUse),
              inputDeviceId (inputDeviceIdToUse),
              outputDeviceId (outputDeviceIdToUse),
              numInputChannels (numInputChannelsToUse),
              numOutputChannels (numOutputChannelsToUse),
              sampleRate (sampleRateToUse),
              bufferSize (bufferSizeToUse),
              streamFormat (streamFormatToUse),
              bitDepth (bitDepthToUse)
        {
            openStreams();
        }

        // Not strictly required as these should not change, but recommended by Google anyway
        void checkStreamSetup (OboeStream* stream,
                               [[maybe_unused]] int deviceId,
                               [[maybe_unused]] int numChannels,
                               [[maybe_unused]] int expectedSampleRate,
                               [[maybe_unused]] int expectedBufferSize,
                               oboe::AudioFormat format)
        {
            if ([[maybe_unused]] auto nativeStream = stream != nullptr ? stream->getNativeStream() : nullptr)
            {
                jassert (numChannels == 0 || numChannels == nativeStream->getChannelCount());
                jassert (expectedSampleRate == 0 || expectedSampleRate == nativeStream->getSampleRate());
                jassert (format == nativeStream->getFormat());
            }
        }

        int getBufferCapacityInFrames (bool forInput) const
        {
            auto& ptr = forInput ? inputStream : outputStream;

            if (ptr == nullptr || ! ptr->openedOk())
                return 0;

            if (auto nativeStream = ptr->getNativeStream())
                return nativeStream->getBufferCapacityInFrames();

            return 0;
        }

        void openStreams()
        {
            outputStream = std::make_unique<OboeStream> (outputDeviceId,
                                                         oboe::Direction::Output,
                                                         oboe::SharingMode::Exclusive,
                                                         numOutputChannels,
                                                         streamFormat,
                                                         sampleRate,
                                                         bufferSize,
                                                         static_cast<AudioStreamCallback*> (this));

            checkStreamSetup (outputStream.get(), outputDeviceId, numOutputChannels,
                              sampleRate, bufferSize, streamFormat);

            if (numInputChannels <= 0)
                return;

            inputStream = std::make_unique<OboeStream> (inputDeviceId,
                                                        oboe::Direction::Input,
                                                        oboe::SharingMode::Exclusive,
                                                        numInputChannels,
                                                        streamFormat,
                                                        sampleRate,
                                                        bufferSize,
                                                        nullptr);

            checkStreamSetup (inputStream.get(), inputDeviceId, numInputChannels,
                              sampleRate, bufferSize, streamFormat);

            if (! inputStream->openedOk() || ! outputStream->openedOk())
                return;

            const auto getSampleRate = [] (auto nativeStream)
            {
                return nativeStream != nullptr ? nativeStream->getSampleRate() : 0;
            };
            // Input & output sample rates should match!
            jassert (getSampleRate (inputStream->getNativeStream())
                     == getSampleRate (outputStream->getNativeStream()));
        }

        OboeAudioIODevice& owner;
        int inputDeviceId, outputDeviceId;
        int numInputChannels, numOutputChannels;
        int sampleRate;
        int bufferSize;
        oboe::AudioFormat streamFormat;
        int bitDepth;

        std::unique_ptr<OboeStream> inputStream, outputStream;
    };

    //==============================================================================
    template <typename SampleType>
    class OboeSessionImpl final : public OboeSessionBase
    {
    public:
        OboeSessionImpl (OboeAudioIODevice& ownerToUse,
                         int inputDeviceIdIn, int outputDeviceIdIn,
                         int numInputChannelsToUse, int numOutputChannelsToUse,
                         int sampleRateToUse, int bufferSizeToUse)
            : OboeSessionBase (ownerToUse,
                               inputDeviceIdIn, outputDeviceIdIn,
                               numInputChannelsToUse, numOutputChannelsToUse,
                               sampleRateToUse, bufferSizeToUse,
                               OboeAudioIODeviceBufferHelpers<SampleType>::oboeAudioFormat(),
                               OboeAudioIODeviceBufferHelpers<SampleType>::bitDepth()),
              inputStreamNativeBuffer (static_cast<size_t> (numInputChannelsToUse * getBufferCapacityInFrames (true))),
              inputStreamSampleBuffer (numInputChannels, getBufferCapacityInFrames (true)),
              outputStreamSampleBuffer (numOutputChannels, getBufferCapacityInFrames (false))
        {
        }

        void start() override
        {
            if (inputStream != nullptr)
                inputStream->start();

            outputStream->start();

            isInputLatencyDetectionSupported  = isLatencyDetectionSupported (inputStream.get());
            isOutputLatencyDetectionSupported = isLatencyDetectionSupported (outputStream.get());
        }

        void stop() override
        {
            const SpinLock::ScopedLockType lock { audioCallbackMutex };

            destroyStreams();
        }

        int getOutputLatencyInSamples() override    { return outputLatency; }
        int getInputLatencyInSamples() override     { return inputLatency; }

    private:
        bool isLatencyDetectionSupported (OboeStream* stream)
        {
            if (stream == nullptr || ! openedOk())
                return false;

            if (auto ptr = stream->getNativeStream())
                return ptr->getTimestamp (CLOCK_MONOTONIC, nullptr, nullptr) != oboe::Result::ErrorUnimplemented;

            return false;
        }

        oboe::DataCallbackResult onAudioReady (oboe::AudioStream* stream, void* audioData, int32_t numFrames) override
        {
            const SpinLock::ScopedTryLockType lock { audioCallbackMutex };

            if (lock.isLocked())
            {
                if (stream == nullptr)
                    return oboe::DataCallbackResult::Stop;

                // only output stream should be the master stream receiving callbacks
                jassert (stream->getDirection() == oboe::Direction::Output && stream == outputStream->getNativeStream().get());

                // Read input from Oboe
                const auto expandedBufferSize = jmax (inputStreamNativeBuffer.size(),
                                                      static_cast<size_t> (numInputChannels * jmax (bufferSize, numFrames)));
                inputStreamNativeBuffer.resize (expandedBufferSize);

                if (inputStream != nullptr)
                {
                    auto nativeInputStream = inputStream->getNativeStream();

                    if (nativeInputStream->getFormat() != oboe::AudioFormat::I16 && nativeInputStream->getFormat() != oboe::AudioFormat::Float)
                    {
                        JUCE_OBOE_LOG ("Unsupported input stream audio format: " + getOboeString (nativeInputStream->getFormat()));
                        jassertfalse;
                        return oboe::DataCallbackResult::Continue;
                    }

                    auto result = nativeInputStream->read (inputStreamNativeBuffer.data(), numFrames, 0);

                    if (result)
                    {
                        auto referringDirectlyToOboeData = OboeAudioIODeviceBufferHelpers<SampleType>
                                                             ::referAudioBufferDirectlyToOboeIfPossible (inputStreamNativeBuffer.data(),
                                                                                                         inputStreamSampleBuffer,
                                                                                                         result.value());

                        if (! referringDirectlyToOboeData)
                            OboeAudioIODeviceBufferHelpers<SampleType>::convertFromOboe (inputStreamNativeBuffer.data(), inputStreamSampleBuffer, result.value());
                    }
                    else
                    {
                        JUCE_OBOE_LOG ("Failed to read from input stream: " + getOboeString (result.error()));
                    }

                    if (isInputLatencyDetectionSupported)
                        inputLatency = getLatencyFor (*inputStream);
                }

                // Setup output buffer
                auto referringDirectlyToOboeData = OboeAudioIODeviceBufferHelpers<SampleType>
                                                     ::referAudioBufferDirectlyToOboeIfPossible (static_cast<SampleType*> (audioData),
                                                                                                 outputStreamSampleBuffer,
                                                                                                 numFrames);

                if (! referringDirectlyToOboeData)
                    outputStreamSampleBuffer.clear();

                // Process
                // NB: the number of samples read from the input can potentially differ from numFrames.
                owner.process (inputStreamSampleBuffer.getArrayOfReadPointers(), numInputChannels,
                               outputStreamSampleBuffer.getArrayOfWritePointers(), numOutputChannels,
                               numFrames);

                // Write output to Oboe
                if (! referringDirectlyToOboeData)
                    OboeAudioIODeviceBufferHelpers<SampleType>::convertToOboe (outputStreamSampleBuffer, static_cast<SampleType*> (audioData), numFrames);

                if (isOutputLatencyDetectionSupported)
                    outputLatency = getLatencyFor (*outputStream);
            }

            return oboe::DataCallbackResult::Continue;
        }

        void printStreamDebugInfo ([[maybe_unused]] oboe::AudioStream* stream)
        {
            JUCE_OBOE_LOG ("\nUses AAudio = " + (stream != nullptr ? String ((int) stream->usesAAudio()) : String ("?"))
                 + "\nDirection = " + (stream != nullptr ? getOboeString (stream->getDirection()) : String ("?"))
                 + "\nSharingMode = " + (stream != nullptr ? getOboeString (stream->getSharingMode()) : String ("?"))
                 + "\nChannelCount = " + (stream != nullptr ? String (stream->getChannelCount()) : String ("?"))
                 + "\nFormat = " + (stream != nullptr ? getOboeString (stream->getFormat()) : String ("?"))
                 + "\nSampleRate = " + (stream != nullptr ? String (stream->getSampleRate()) : String ("?"))
                 + "\nBufferSizeInFrames = " + (stream != nullptr ? String (stream->getBufferSizeInFrames()) : String ("?"))
                 + "\nBufferCapacityInFrames = " + (stream != nullptr ? String (stream->getBufferCapacityInFrames()) : String ("?"))
                 + "\nFramesPerBurst = " + (stream != nullptr ? String (stream->getFramesPerBurst()) : String ("?"))
                 + "\nFramesPerCallback = " + (stream != nullptr ? String (stream->getFramesPerCallback()) : String ("?"))
                 + "\nBytesPerFrame = " + (stream != nullptr ? String (stream->getBytesPerFrame()) : String ("?"))
                 + "\nBytesPerSample = " + (stream != nullptr ? String (stream->getBytesPerSample()) : String ("?"))
                 + "\nPerformanceMode = " + (stream != nullptr ? getOboeString (stream->getPerformanceMode()) : String ("?"))
                 + "\ngetDeviceId = " + (stream != nullptr ? String (stream->getDeviceId()) : String ("?")));
        }

        int getLatencyFor (OboeStream& stream)
        {
            auto nativeStream = stream.getNativeStream();

            if (auto latency = nativeStream->calculateLatencyMillis())
                return static_cast<int> ((latency.value() * sampleRate) / 1000);

            // Get the time that a known audio frame was presented.
            int64_t hardwareFrameIndex = 0;
            int64_t hardwareFrameHardwareTime = 0;

            auto result = nativeStream->getTimestamp (CLOCK_MONOTONIC,
                                                      &hardwareFrameIndex,
                                                      &hardwareFrameHardwareTime);

            if (result != oboe::Result::OK)
                return 0;

            // Get counter closest to the app.
            const bool isOutput = nativeStream->getDirection() == oboe::Direction::Output;
            const int64_t appFrameIndex = isOutput ? nativeStream->getFramesWritten() : nativeStream->getFramesRead();

            // Assume that the next frame will be processed at the current time
            int64_t appFrameAppTime = getCurrentTimeNanos();

            // Calculate the number of frames between app and hardware
            int64_t frameIndexDelta = appFrameIndex - hardwareFrameIndex;

            // Calculate the time which the next frame will be or was presented
            int64_t frameTimeDelta = (frameIndexDelta * oboe::kNanosPerSecond) / sampleRate;
            int64_t appFrameHardwareTime = hardwareFrameHardwareTime + frameTimeDelta;

            // Calculate latency as a difference in time between when the current frame is at the app
            // and when it is at the hardware.
            auto latencyNanos = isOutput ? (appFrameHardwareTime - appFrameAppTime) : (appFrameAppTime - appFrameHardwareTime);
            return static_cast<int> ((latencyNanos  * sampleRate) / oboe::kNanosPerSecond);
        }

        int64_t getCurrentTimeNanos()
        {
            timespec time;

            if (clock_gettime (CLOCK_MONOTONIC, &time) < 0)
                return -1;

            return time.tv_sec * oboe::kNanosPerSecond + time.tv_nsec;
        }

        void onErrorBeforeClose (oboe::AudioStream* stream, [[maybe_unused]] oboe::Result error) override
        {
            // only output stream should be the master stream receiving callbacks
            jassert (stream->getDirection() == oboe::Direction::Output);

            JUCE_OBOE_LOG ("Oboe stream onErrorBeforeClose(): " + getOboeString (error));
            printStreamDebugInfo (stream);
        }

        void onErrorAfterClose (oboe::AudioStream* stream, oboe::Result error) override
        {
            // only output stream should be the master stream receiving callbacks
            jassert (stream->getDirection() == oboe::Direction::Output);

            JUCE_OBOE_LOG ("Oboe stream onErrorAfterClose(): " + getOboeString (error));

            const SpinLock::ScopedTryLockType streamRestartLock { streamRestartMutex };

            if (! streamRestartLock.isLocked())
                return;

            const SpinLock::ScopedLockType audioCallbackLock { audioCallbackMutex };

            destroyStreams();

            if (error != oboe::Result::ErrorDisconnected)
                return;

            openStreams();

            // Um AudioDeviceInfo recebe um ID novo quando a interface USB e
            // reconectada. Reabrir imediatamente com o ID antigo pode falhar
            // e deixar o aplicativo mudo ate ser reiniciado. Preserve a
            // primeira tentativa (ela evita uma troca desnecessaria quando o
            // ID ainda e valido), mas caia para a rota padrao do sistema se o
            // dispositivo explicito desapareceu. A message thread fara o
            // rescan e voltara ao ID novo assim que receber a nova geracao.
            if (! openedOk()
                 && ((numInputChannels > 0 && inputDeviceId != oboe::kUnspecified)
                      || outputDeviceId != oboe::kUnspecified))
            {
                JUCE_OBOE_LOG ("AUDIO_ANDROID disconnect_reopen_failed; fallback=system_default");
                destroyStreams();
                inputDeviceId = oboe::kUnspecified;
                outputDeviceId = oboe::kUnspecified;
                openStreams();
            }

            if (openedOk())
            {
                JUCE_OBOE_LOG ("AUDIO_ANDROID disconnect_reopen_succeeded; output_device_id="
                               + String (outputDeviceId));
                start();
            }
            else
            {
                JUCE_OBOE_LOG ("AUDIO_ANDROID disconnect_reopen_failed; fallback_open_failed=1");
            }
        }

        void destroyStreams()
        {
            inputStream = nullptr;
            outputStream = nullptr;
        }

        std::vector<SampleType> inputStreamNativeBuffer;
        AudioBuffer<float> inputStreamSampleBuffer,
                           outputStreamSampleBuffer;
        SpinLock audioCallbackMutex, streamRestartMutex;

        bool isInputLatencyDetectionSupported = false;
        int inputLatency = -1;

        bool isOutputLatencyDetectionSupported = false;
        int outputLatency = -1;
    };

    //==============================================================================
    friend class OboeAudioIODeviceType;
    friend class OboeRealtimeThread;

    //==============================================================================
    int actualBufferSize = 0, sampleRate = 0;
    bool deviceOpen = false;
    String lastError;
    BigInteger activeOutputChans, activeInputChans;
    Atomic<AudioIODeviceCallback*> callback { nullptr };

    int inputDeviceId;
    Array<int> supportedInputSampleRates;
    int maxNumInputChannels;
    int outputDeviceId;
    Array<int> supportedOutputSampleRates;
    int maxNumOutputChannels;

    std::unique_ptr<OboeSessionBase> session;

    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OboeAudioIODevice)
};

//==============================================================================
OboeAudioIODevice::OboeSessionBase* OboeAudioIODevice::OboeSessionBase::create (OboeAudioIODevice& owner,
                                                                                int inputDeviceId,
                                                                                int outputDeviceId,
                                                                                int numInputChannels,
                                                                                int numOutputChannels,
                                                                                int sampleRate,
                                                                                int bufferSize)
{

    // SDK versions 21 and higher should natively support floating point...
    std::unique_ptr<OboeSessionBase> session = std::make_unique<OboeSessionImpl<float>> (owner,
                                                                                         inputDeviceId,
                                                                                         outputDeviceId,
                                                                                         numInputChannels,
                                                                                         numOutputChannels,
                                                                                         sampleRate,
                                                                                         bufferSize);

    // ...however, some devices lie so re-try without floating point
    if (session != nullptr && (! session->openedOk()))
        session.reset();

    if (session == nullptr)
    {
        session.reset (new OboeSessionImpl<int16> (owner, inputDeviceId, outputDeviceId,
                                                   numInputChannels, numOutputChannels, sampleRate, bufferSize));

        if (session != nullptr && (! session->openedOk()))
            session.reset();
    }

    return session.release();
}

//==============================================================================
class OboeAudioIODeviceType final : public AudioIODeviceType
{
public:
    OboeAudioIODeviceType()
        : AudioIODeviceType (OboeAudioIODevice::oboeTypeName)
    {
        refreshDeviceCache (false);
    }

    //==============================================================================
    void scanForDevices() override
    {
        refreshDeviceCache (true);
    }

    StringArray getDeviceNames (bool wantInputNames) const override
    {
        StringArray names;

        for (auto& device : wantInputNames ? inputDevices : outputDevices)
            names.add (device.name);

        return names;
    }

    int getDefaultDeviceIndex (bool) const override
    {
        return 0;
    }

    int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
    {
        if (auto oboeDevice = static_cast<OboeAudioIODevice*> (device))
        {
            auto oboeDeviceId = asInput ? oboeDevice->inputDeviceId
                                        : oboeDevice->outputDeviceId;

            auto& devices = asInput ? inputDevices : outputDevices;

            for (int i = 0; i < devices.size(); ++i)
                if (devices.getReference (i).id == oboeDeviceId)
                    return i;
        }

        return -1;
    }

    bool hasSeparateInputsAndOutputs() const override  { return true; }

    AudioIODevice* createDevice (const String& outputDeviceName,
                                 const String& inputDeviceName) override
    {
        auto outputDeviceInfo = getDeviceInfoForName (outputDeviceName, false);
        auto inputDeviceInfo  = getDeviceInfoForName (inputDeviceName, true);

        if (outputDeviceInfo.id < 0 && inputDeviceInfo.id < 0)
            return nullptr;

        auto& name = outputDeviceInfo.name.isNotEmpty() ? outputDeviceInfo.name
                                                        : inputDeviceInfo.name;

        return new OboeAudioIODevice (name,
                                      inputDeviceInfo.id, inputDeviceInfo.sampleRates,
                                      inputDeviceInfo.numChannels,
                                      outputDeviceInfo.id, outputDeviceInfo.sampleRates,
                                      outputDeviceInfo.numChannels);
    }

    static bool isOboeAvailable()
    {
       #if JUCE_USE_ANDROID_OBOE
        return true;
       #else
        return false;
       #endif
    }

 private:
    struct DeviceInfo
    {
        String name;
        int id = -1;
        int type = 0;
        Array<int> sampleRates;
        int numChannels = -1;
    };

    static bool intArraysMatch (const Array<int>& first, const Array<int>& second)
    {
        if (first.size() != second.size())
            return false;

        for (int i = 0; i < first.size(); ++i)
            if (first.getUnchecked (i) != second.getUnchecked (i))
                return false;

        return true;
    }

    static bool deviceListsMatch (const Array<DeviceInfo>& first,
                                  const Array<DeviceInfo>& second)
    {
        if (first.size() != second.size())
            return false;

        for (int i = 0; i < first.size(); ++i)
        {
            const auto& a = first.getReference (i);
            const auto& b = second.getReference (i);

            if (a.id != b.id
                 || a.type != b.type
                 || a.name != b.name
                 || a.numChannels != b.numChannels
                 || ! intArraysMatch (a.sampleRates, b.sampleRates))
                return false;
        }

        return true;
    }

    static void sortDevices (Array<DeviceInfo>& devices)
    {
        std::sort (devices.begin(), devices.end(), [] (const auto& a, const auto& b)
        {
            if (a.id != b.id)       return a.id < b.id;
            if (a.type != b.type)   return a.type < b.type;
            return a.name.compare (b.name) < 0;
        });
    }

    void refreshDeviceCache (bool notifyListeners)
    {
        Array<DeviceInfo> newInputs, newOutputs;
        checkAvailableDevices (newInputs, newOutputs);
        sortDevices (newInputs);
        sortDevices (newOutputs);

        if (deviceListsMatch (inputDevices, newInputs)
             && deviceListsMatch (outputDevices, newOutputs))
            return;

        inputDevices = std::move (newInputs);
        outputDevices = std::move (newOutputs);

        JUCE_OBOE_LOG ("AUDIO_ANDROID topology_changed; inputs="
                       + String (inputDevices.size())
                       + "; outputs=" + String (outputDevices.size()));

        if (notifyListeners)
            callDeviceChangeListeners();
    }

    void checkAvailableDevices (Array<DeviceInfo>& foundInputs,
                                Array<DeviceInfo>& foundOutputs)
    {
        auto sampleRates = OboeAudioIODevice::getDefaultSampleRates();

        foundInputs .add ({ "System Default (Input)",  oboe::kUnspecified, 0, sampleRates, 1 });
        foundOutputs.add ({ "System Default (Output)", oboe::kUnspecified, 0, sampleRates, 2 });

        if (! supportsDevicesInfo())
            return;

        auto* env = getEnv();

        LocalRef<jclass> audioManagerClass { env->FindClass ("android/media/AudioManager") };

        // We should be really entering here only if API supports it.
        jassert (audioManagerClass != nullptr);

        if (audioManagerClass == nullptr)
            return;

        auto audioManager = LocalRef<jobject> (env->CallObjectMethod (getAppContext().get(),
                                                                      AndroidContext.getSystemService,
                                                                      javaString ("audio").get()));

        if (audioManager == nullptr)
            return;

        static jmethodID getDevicesMethod = env->GetMethodID (audioManagerClass, "getDevices",
                                                              "(I)[Landroid/media/AudioDeviceInfo;");

        static constexpr int allDevices = 3;
        auto devices = LocalRef<jobjectArray> ((jobjectArray) env->CallObjectMethod (audioManager,
                                                                                     getDevicesMethod,
                                                                                     allDevices));

        if (devices == nullptr)
            return;

        const int numDevices = env->GetArrayLength (devices.get());

        for (int i = 0; i < numDevices; ++i)
        {
            auto device = LocalRef<jobject> ((jobject) env->GetObjectArrayElement (devices.get(), i));
            addDevice (device, env, foundInputs, foundOutputs);
        }

        JUCE_OBOE_LOG ("-----InputDevices:");

        for ([[maybe_unused]] auto& device : foundInputs)
        {
            JUCE_OBOE_LOG ("name = " << device.name);
            JUCE_OBOE_LOG ("id = " << String (device.id));
            JUCE_OBOE_LOG ("sample rates size = " << String (device.sampleRates.size()));
            JUCE_OBOE_LOG ("num channels = " + String (device.numChannels));
        }

        JUCE_OBOE_LOG ("-----OutputDevices:");

        for ([[maybe_unused]] auto& device : foundOutputs)
        {
            JUCE_OBOE_LOG ("name = " << device.name);
            JUCE_OBOE_LOG ("id = " << String (device.id));
            JUCE_OBOE_LOG ("sample rates size = " << String (device.sampleRates.size()));
            JUCE_OBOE_LOG ("num channels = " + String (device.numChannels));
        }
    }

    bool supportsDevicesInfo() const
    {
        return true;
    }

    void addDevice (const LocalRef<jobject>& device, JNIEnv* env,
                    Array<DeviceInfo>& foundInputs,
                    Array<DeviceInfo>& foundOutputs)
    {
        LocalRef<jclass> deviceClass { env->FindClass ("android/media/AudioDeviceInfo") };

        jmethodID getProductNameMethod = env->GetMethodID (deviceClass, "getProductName",
                                                           "()Ljava/lang/CharSequence;");

        jmethodID getTypeMethod          = env->GetMethodID (deviceClass, "getType", "()I");
        jmethodID getIdMethod            = env->GetMethodID (deviceClass, "getId", "()I");
        jmethodID getSampleRatesMethod   = env->GetMethodID (deviceClass, "getSampleRates", "()[I");
        jmethodID getChannelCountsMethod = env->GetMethodID (deviceClass, "getChannelCounts", "()[I");
        jmethodID isSourceMethod         = env->GetMethodID (deviceClass, "isSource", "()Z");

        const auto type = env->CallIntMethod (device, getTypeMethod);
        auto deviceTypeString = deviceTypeToString (type);

        if (deviceTypeString.isEmpty()) // unknown device
            return;

        auto name = juceString ((jstring) env->CallObjectMethod (device, getProductNameMethod)) + " " + deviceTypeString;
        auto id = env->CallIntMethod (device, getIdMethod);

        auto jSampleRates = LocalRef<jintArray> ((jintArray) env->CallObjectMethod (device, getSampleRatesMethod));
        auto sampleRates = jintArrayToJuceArray (jSampleRates);

        auto jChannelCounts = LocalRef<jintArray> ((jintArray) env->CallObjectMethod (device, getChannelCountsMethod));
        auto channelCounts = jintArrayToJuceArray (jChannelCounts);
        int numChannels = channelCounts.isEmpty() ? -1 : channelCounts.getLast();

        auto isInput  = env->CallBooleanMethod (device, isSourceMethod);
        auto& devices = isInput ? foundInputs : foundOutputs;

        devices.add ({ name, id, type, sampleRates, numChannels });
    }

    static String deviceTypeToString (int type)
    {
        switch (type)
        {
            case 0:   return {};
            case 1:   return "built-in earphone speaker";
            case 2:   return "built-in speaker";
            case 3:   return "wired headset";
            case 4:   return "wired headphones";
            case 5:   return "line analog";
            case 6:   return "line digital";
            case 7:   return "Bluetooth device typically used for telephony";
            case 8:   return "Bluetooth device supporting the A2DP profile";
            case 9:   return "HDMI";
            case 10:  return "HDMI audio return channel";
            case 11:  return "USB device";
            case 12:  return "USB accessory";
            case 13:  return "DOCK";
            case 14:  return "FM";
            case 15:  return "built-in microphone";
            case 16:  return "FM tuner";
            case 17:  return "TV tuner";
            case 18:  return "telephony";
            case 19:  return "auxiliary line-level connectors";
            case 20:  return "IP";
            case 21:  return "BUS";
            case 22:  return "USB headset";
            case 23:  return "hearing aid";
            case 24:  return "built-in speaker safe";
            case 25:  return "remote submix";
            case 26:  return "BLE headset";
            case 27:  return "BLE speaker";
            case 28:  return "echo reference";
            case 29:  return "HDMI eARC";
            case 30:  return "BLE broadcast";
            default:  jassertfalse; return {}; // type not supported yet, needs to be added!
        }
    }

    static Array<int> jintArrayToJuceArray (const LocalRef<jintArray>& jArray)
    {
        auto* env = getEnv();

        jint* jArrayElems = env->GetIntArrayElements (jArray, nullptr);
        int numElems = env->GetArrayLength (jArray);

        Array<int> juceArray;

        for (int s = 0; s < numElems; ++s)
            juceArray.add (jArrayElems[s]);

        env->ReleaseIntArrayElements (jArray, jArrayElems, 0);
        return juceArray;
    }

    DeviceInfo getDeviceInfoForName (const String& name, bool isInput)
    {
        if (name.isNotEmpty())
        {
            for (auto& device : isInput ? inputDevices : outputDevices)
            {
                if (device.name == name)
                    return device;
            }
        }

        return {};
    }

    Array<DeviceInfo> inputDevices, outputDevices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OboeAudioIODeviceType)
};

const char* const OboeAudioIODevice::oboeTypeName = "Android Oboe";

bool isOboeAvailable()  { return OboeAudioIODeviceType::isOboeAvailable(); }

//==============================================================================
class OboeRealtimeThread final : private oboe::AudioStreamCallback
{
    using OboeStream = OboeAudioIODevice::OboeStream;

public:
    OboeRealtimeThread()
        : testStream (new OboeStream (oboe::kUnspecified,
                                      oboe::Direction::Output,
                                      oboe::SharingMode::Exclusive,
                                      1,
                                      oboe::AudioFormat::Float,
                                      (int) AndroidHighPerformanceAudioHelpers::getNativeSampleRate(),
                                      OboeAudioIODevice::getNativeBufferSize(),
                                      this)),
          formatUsed (oboe::AudioFormat::Float)
    {
        // Fallback to I16 stream format if Float has not worked
        if (! testStream->openedOk())
        {
            testStream.reset (new OboeStream (oboe::kUnspecified,
                                              oboe::Direction::Output,
                                              oboe::SharingMode::Exclusive,
                                              1,
                                              oboe::AudioFormat::I16,
                                              (int) AndroidHighPerformanceAudioHelpers::getNativeSampleRate(),
                                              OboeAudioIODevice::getNativeBufferSize(),
                                              this));

            formatUsed = oboe::AudioFormat::I16;
        }

        parentThreadID = pthread_self();

        pthread_cond_init (&threadReady, nullptr);
        pthread_mutex_init (&threadReadyMutex, nullptr);
    }

    bool isOk() const
    {
        return testStream != nullptr && testStream->openedOk();
    }

    pthread_t startThread (void*(*entry) (void*), void* userPtr)
    {
        pthread_mutex_lock (&threadReadyMutex);

        threadEntryProc = entry;
        threadUserPtr  = userPtr;

        testStream->start();

        pthread_cond_wait (&threadReady, &threadReadyMutex);
        pthread_mutex_unlock (&threadReadyMutex);

        return realtimeThreadID;
    }

    oboe::DataCallbackResult onAudioReady (oboe::AudioStream*, void*, int32_t) override
    {
        // When running with OpenSL, the first callback will come on the parent thread.
        if (threadEntryProc != nullptr && ! pthread_equal (parentThreadID, pthread_self()))
        {
            pthread_mutex_lock (&threadReadyMutex);

            realtimeThreadID = pthread_self();

            pthread_cond_signal (&threadReady);
            pthread_mutex_unlock (&threadReadyMutex);

            threadEntryProc (threadUserPtr);
            threadEntryProc = nullptr;

            MessageManager::callAsync ([this]() { delete this; });

            return oboe::DataCallbackResult::Stop;
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorBeforeClose (oboe::AudioStream*, [[maybe_unused]] oboe::Result error) override
    {
        JUCE_OBOE_LOG ("OboeRealtimeThread: Oboe stream onErrorBeforeClose(): " + getOboeString (error));
        jassertfalse;  // Should never get here!
    }

    void onErrorAfterClose (oboe::AudioStream*, [[maybe_unused]] oboe::Result error) override
    {
        JUCE_OBOE_LOG ("OboeRealtimeThread: Oboe stream onErrorAfterClose(): " + getOboeString (error));
        jassertfalse;  // Should never get here!
    }

private:
    //==============================================================================
    void* (*threadEntryProc) (void*) = nullptr;
    void* threadUserPtr = nullptr;

    pthread_cond_t  threadReady;
    pthread_mutex_t threadReadyMutex;
    pthread_t       parentThreadID, realtimeThreadID;

    std::unique_ptr<OboeStream> testStream;
    oboe::AudioFormat formatUsed;
};

//==============================================================================
RealtimeThreadFactory getAndroidRealtimeThreadFactory()
{
    return [] (void* (*entry) (void*), void* userPtr) -> pthread_t
    {
        auto thread = std::make_unique<OboeRealtimeThread>();

        if (! thread->isOk())
            return {};

        auto threadID = thread->startThread (entry, userPtr);

        // the thread will de-allocate itself
        thread.release();

        return threadID;
    };
}

} // namespace juce
