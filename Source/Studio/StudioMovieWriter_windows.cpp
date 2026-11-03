#include "StudioMovieWriter.h"
#include <juce_audio_formats/juce_audio_formats.h>

#if JUCE_WINDOWS
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <limits>

namespace churchstudio
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr LONGLONG ticksPerSecond = 10000000;
constexpr int audioRate = 48000, audioBlock = 1024;

juce::Result failure (const juce::String& operation, HRESULT hr)
{
    wchar_t message[512] {};
    FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                    static_cast<DWORD> (hr), 0, message, 512, nullptr);
    auto detail = juce::String (message).trim();
    return juce::Result::fail (operation + " (Windows 0x"
        + juce::String::toHexString (static_cast<juce::uint32> (hr)).paddedLeft ('0', 8)
        + ")" + (detail.isNotEmpty() ? ": " + detail : "."));
}

// Bind the optional Windows media runtime lazily. Windows N without its Media
// Feature Pack must still be able to start the DAW and use its audio engine.
struct MediaApi
{
    HMODULE platform = nullptr, readWrite = nullptr;
    decltype (&::MFStartup) startup = nullptr;
    decltype (&::MFShutdown) shutdown = nullptr;
    decltype (&::MFCreateAttributes) createAttributes = nullptr;
    decltype (&::MFCreateMediaType) createMediaType = nullptr;
    decltype (&::MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    decltype (&::MFCreateSample) createSample = nullptr;
    decltype (&::MFCreateSinkWriterFromURL) createSink = nullptr;
    decltype (&::MFCreateSourceReaderFromURL) createReader = nullptr;

    MediaApi()
    {
        platform = LoadLibraryExW (L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        readWrite = LoadLibraryExW (L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (platform == nullptr || readWrite == nullptr) return;
        startup = reinterpret_cast<decltype (startup)> (GetProcAddress (platform, "MFStartup"));
        shutdown = reinterpret_cast<decltype (shutdown)> (GetProcAddress (platform, "MFShutdown"));
        createAttributes = reinterpret_cast<decltype (createAttributes)> (GetProcAddress (platform, "MFCreateAttributes"));
        createMediaType = reinterpret_cast<decltype (createMediaType)> (GetProcAddress (platform, "MFCreateMediaType"));
        createMemoryBuffer = reinterpret_cast<decltype (createMemoryBuffer)> (GetProcAddress (platform, "MFCreateMemoryBuffer"));
        createSample = reinterpret_cast<decltype (createSample)> (GetProcAddress (platform, "MFCreateSample"));
        createSink = reinterpret_cast<decltype (createSink)> (GetProcAddress (readWrite, "MFCreateSinkWriterFromURL"));
        createReader = reinterpret_cast<decltype (createReader)> (GetProcAddress (readWrite, "MFCreateSourceReaderFromURL"));
    }
    ~MediaApi()
    {
        if (readWrite != nullptr) FreeLibrary (readWrite);
        if (platform != nullptr) FreeLibrary (platform);
    }
    bool available() const noexcept
    {
        return startup && shutdown && createAttributes && createMediaType
            && createMemoryBuffer && createSample && createSink && createReader;
    }
};

MediaApi& mediaApi() { static MediaApi api; return api; }

struct MediaRuntime
{
    const DWORD thread = GetCurrentThreadId();
    bool uninitialiseCom = false, started = false;
    HRESULT status = HRESULT_FROM_WIN32 (ERROR_MOD_NOT_FOUND);

    MediaRuntime()
    {
        if (! mediaApi().available()) return;
        status = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        uninitialiseCom = SUCCEEDED (status);
        // An existing STA is valid COM too; do not unbalance its ownership.
        if (FAILED (status) && status != RPC_E_CHANGED_MODE) return;
        status = mediaApi().startup (MF_VERSION, MFSTARTUP_FULL);
        started = SUCCEEDED (status);
    }
    ~MediaRuntime()
    {
        jassert (thread == GetCurrentThreadId());
        if (started) mediaApi().shutdown();
        if (uninitialiseCom && thread == GetCurrentThreadId()) CoUninitialize();
    }
    juce::Result check() const
    {
        if (! mediaApi().available())
            return juce::Result::fail ("O Windows precisa do Media Feature Pack para gravar MP4. Instale-o nos Recursos opcionais do Windows.");
        if (thread != GetCurrentThreadId())
            return juce::Result::fail ("O codificador foi chamado fora da thread de gravacao.");
        return SUCCEEDED (status) ? juce::Result::ok() : failure ("Nao foi possivel iniciar o codificador", status);
    }
};

bool validTime (double seconds)
{
    return std::isfinite (seconds) && seconds >= 0
        && seconds < static_cast<double> (std::numeric_limits<LONGLONG>::max() / ticksPerSecond - 1);
}
LONGLONG toTicks (double seconds) { return static_cast<LONGLONG> (std::llround (seconds * ticksPerSecond)); }
LONGLONG audioTicks (juce::int64 frames)
{
    return (frames / audioRate) * ticksPerSecond + (frames % audioRate) * ticksPerSecond / audioRate;
}

HRESULT createSink (const juce::File& file, ComPtr<IMFSinkWriter>& writer)
{
    ComPtr<IMFAttributes> attributes;
    auto hr = mediaApi().createAttributes (attributes.GetAddressOf(), 2);
    // Recorder's intermediate file is named video.mov on every platform.
    if (SUCCEEDED (hr)) hr = attributes->SetGUID (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED (hr)) hr = attributes->SetUINT32 (MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if (SUCCEEDED (hr)) hr = mediaApi().createSink (file.getFullPathName().toWideCharPointer(), nullptr,
                                                   attributes.Get(), writer.GetAddressOf());
    return hr;
}

HRESULT videoType (ComPtr<IMFMediaType>& type, int width, int height, bool compressed)
{
    auto hr = mediaApi().createMediaType (type.GetAddressOf());
    if (SUCCEEDED (hr)) hr = type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED (hr)) hr = type->SetGUID (MF_MT_SUBTYPE, compressed ? MFVideoFormat_H264 : MFVideoFormat_NV12);
    if (SUCCEEDED (hr)) hr = MFSetAttributeSize (type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32> (width), static_cast<UINT32> (height));
    if (SUCCEEDED (hr)) hr = MFSetAttributeRatio (type.Get(), MF_MT_FRAME_RATE, 30, 1);
    if (SUCCEEDED (hr)) hr = MFSetAttributeRatio (type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (compressed)
    {
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_AVG_BITRATE, width > 1280 ? 12000000 : 6000000);
        // Baseline forbids B pictures. With Main, the Windows encoder/muxer
        // reordered I/P/B samples and shifted every presentation timestamp by
        // one frame to avoid negative DTS, delaying video against the audio.
        // Negotiate this before the encoder is created, so PTS starts at the
        // supplied time even when a vendor's optional codec controls differ.
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_MPEG2_PROFILE, 66); // H.264 Baseline.
    }
    else
    {
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_DEFAULT_STRIDE, static_cast<UINT32> (width));
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_SAMPLE_SIZE, static_cast<UINT32> (width * height * 3 / 2));
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        if (SUCCEEDED (hr)) hr = type->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    return hr;
}

HRESULT makeSample (ComPtr<IMFMediaBuffer>& buffer, DWORD bytes, LONGLONG time,
                    LONGLONG duration, ComPtr<IMFSample>& sample)
{
    auto hr = buffer->SetCurrentLength (bytes);
    if (SUCCEEDED (hr)) hr = mediaApi().createSample (sample.GetAddressOf());
    if (SUCCEEDED (hr)) hr = sample->AddBuffer (buffer.Get());
    if (SUCCEEDED (hr)) hr = sample->SetSampleTime (time);
    if (SUCCEEDED (hr)) hr = sample->SetSampleDuration (duration);
    return hr;
}

HRESULT imageSample (const juce::Image& image, LONGLONG time, ComPtr<IMFSample>& sample)
{
    const int width = image.getWidth(), height = image.getHeight();
    const DWORD bytes = static_cast<DWORD> (width * height * 3 / 2);
    ComPtr<IMFMediaBuffer> buffer;
    auto hr = mediaApi().createMemoryBuffer (bytes, buffer.GetAddressOf());
    BYTE* output = nullptr;
    if (SUCCEEDED (hr)) hr = buffer->Lock (&output, nullptr, nullptr);
    if (FAILED (hr)) return hr;
    {
        juce::Image::BitmapData source (image, juce::Image::BitmapData::readOnly);
        auto* uv = output + static_cast<size_t> (width) * height;
        // Explicit top-down NV12 avoids RGB's bottom-up DIB convention and
        // driver-specific colour converters. Honour the JUCE source row stride.
        for (int y = 0; y < height; y += 2)
        {
            const auto* upper = reinterpret_cast<const juce::PixelARGB*> (source.getLinePointer (y));
            const auto* lower = reinterpret_cast<const juce::PixelARGB*> (source.getLinePointer (y + 1));
            for (int x = 0; x < width; x += 2)
            {
                const juce::PixelARGB pixels[] = { upper[x], upper[x + 1], lower[x], lower[x + 1] };
                int red = 0, green = 0, blue = 0;
                for (int p = 0; p < 4; ++p)
                {
                    const int r = pixels[p].getRed(), g = pixels[p].getGreen(), b = pixels[p].getBlue();
                    red += r; green += g; blue += b;
                    output[static_cast<size_t> (y + p / 2) * width + x + p % 2]
                        = static_cast<BYTE> (16 + ((11966 * r + 40254 * g + 4064 * b + 32768) >> 16));
                }
                red = (red + 2) / 4; green = (green + 2) / 4; blue = (blue + 2) / 4;
                uv[static_cast<size_t> (y / 2) * width + x]
                    = static_cast<BYTE> (juce::jlimit (16, 240, 128 + ((-6596 * red - 22189 * green + 28785 * blue + 32768) >> 16)));
                uv[static_cast<size_t> (y / 2) * width + x + 1]
                    = static_cast<BYTE> (juce::jlimit (16, 240, 128 + ((28785 * red - 26145 * green - 2640 * blue + 32768) >> 16)));
            }
        }
    }
    hr = buffer->Unlock();
    if (SUCCEEDED (hr)) hr = makeSample (buffer, bytes, time, ticksPerSecond / 30, sample);
    return hr;
}

// AudioFormatReaderSource deliberately ignores disk-read failures. Export must
// report them, rather than silently replace a damaged recorded track with zeros.
struct RecordedAudioSource final : juce::AudioSource
{
    explicit RecordedAudioSource (std::unique_ptr<juce::AudioFormatReader> r) : reader (std::move (r)) {}
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        info.clearActiveBufferRegion();
        const int count = static_cast<int> (juce::jlimit<juce::int64> (0, info.numSamples, reader->lengthInSamples - position));
        if (count > 0 && ! reader->read (info.buffer, info.startSample, count, position, true, true)) failed = true;
        position += info.numSamples;
    }
    std::unique_ptr<juce::AudioFormatReader> reader;
    juce::int64 position = 0;
    bool failed = false;
};

struct MixTrack
{
    MixTrack (std::unique_ptr<juce::AudioFormatReader> reader, const AudioFile& file)
        : source (std::move (reader)), resampler (&source, false, 2), gain (file.gain),
          start (static_cast<juce::int64> (std::llround (juce::jmax (0.0, file.offsetSeconds) * audioRate)))
    {
        resampler.setResamplingRatio (source.reader->sampleRate / audioRate);
        resampler.prepareToPlay (audioBlock, audioRate);
    }
    ~MixTrack() { resampler.releaseResources(); }
    RecordedAudioSource source;
    juce::ResamplingAudioSource resampler;
    float gain;
    juce::int64 start;
};

HRESULT addAudioStream (IMFSinkWriter& writer, DWORD& stream)
{
    ComPtr<IMFMediaType> output, input;
    auto hr = mediaApi().createMediaType (output.GetAddressOf());
    if (SUCCEEDED (hr)) hr = output->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED (hr)) hr = output->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, audioRate);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AAC_PAYLOAD_TYPE, 0);
    if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (SUCCEEDED (hr)) hr = writer.AddStream (output.Get(), &stream);
    if (SUCCEEDED (hr)) hr = mediaApi().createMediaType (input.GetAddressOf());
    if (SUCCEEDED (hr)) hr = input->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED (hr)) hr = input->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, audioRate);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioRate * 4);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED (hr)) hr = writer.SetInputMediaType (stream, input.Get(), nullptr);
    return hr;
}
} // namespace

struct MovieWriter::Impl
{
    // COM/MF must outlive every media object, including on an error return.
    MediaRuntime runtime;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFSample> pendingFrame;
    DWORD videoStream = 0;
    int width = 0, height = 0;
    LONGLONG lastTime = -1;
};

MovieWriter::MovieWriter() : impl (new Impl) {}
MovieWriter::~MovieWriter() = default;
bool MovieWriter::supported() { return mediaApi().available(); }

juce::Result MovieWriter::open (const juce::File& file, int width, int height)
{
    if (auto result = impl->runtime.check(); result.failed()) return result;
    if (impl->writer) return juce::Result::fail ("Ja existe uma gravacao aberta.");
    if (width < 2 || height < 2 || width > 8192 || height > 8192 || width % 2 != 0 || height % 2 != 0)
        return juce::Result::fail ("Dimensoes de video invalidas para H.264.");
    if (file.exists()) return juce::Result::fail ("O arquivo de video ja existe.");
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFMediaType> output, input;
    DWORD stream = 0;
    auto hr = createSink (file, writer);
    if (SUCCEEDED (hr)) hr = videoType (output, width, height, true);
    if (SUCCEEDED (hr)) hr = writer->AddStream (output.Get(), &stream);
    if (SUCCEEDED (hr)) hr = videoType (input, width, height, false);
    if (SUCCEEDED (hr)) hr = writer->SetInputMediaType (stream, input.Get(), nullptr);
    if (SUCCEEDED (hr)) hr = writer->BeginWriting();
    if (FAILED (hr)) return failure ("Nao foi possivel abrir a gravacao H.264", hr);
    impl->writer = std::move (writer);
    impl->videoStream = stream;
    impl->width = width; impl->height = height; impl->lastTime = -1;
    impl->pendingFrame.Reset();
    return juce::Result::ok();
}

juce::Result MovieWriter::append (const juce::Image& image, double seconds)
{
    if (auto result = impl->runtime.check(); result.failed()) return result;
    if (! impl->writer) return juce::Result::fail ("Gravacao nao iniciada.");
    if (! validTime (seconds)) return juce::Result::fail ("Tempo de quadro invalido.");
    const auto time = toTicks (seconds);
    if (time <= impl->lastTime) return juce::Result::ok();
    if (image.getWidth() != impl->width || image.getHeight() != impl->height || image.getFormat() != juce::Image::ARGB)
        return juce::Result::fail ("Dimensoes ou formato do quadro invalidos.");
    ComPtr<IMFSample> sample;
    auto hr = imageSample (image, time, sample);
    if (SUCCEEDED (hr) && impl->pendingFrame)
    {
        hr = impl->pendingFrame->SetSampleDuration (time - impl->lastTime);
        if (SUCCEEDED (hr)) hr = impl->writer->WriteSample (impl->videoStream, impl->pendingFrame.Get());
    }
    if (FAILED (hr)) return failure ("Falha ao codificar um quadro de video", hr);
    impl->pendingFrame = std::move (sample);
    impl->lastTime = time;
    return juce::Result::ok();
}

juce::Result MovieWriter::finish (double seconds)
{
    if (auto result = impl->runtime.check(); result.failed()) return result;
    if (! impl->writer) return juce::Result::fail ("Gravacao nao iniciada.");
    if (! impl->pendingFrame) return juce::Result::fail ("O video nao possui quadros validos.");
    if (! validTime (seconds) || toTicks (seconds) <= impl->lastTime)
        return juce::Result::fail ("Duracao de video invalida.");
    auto hr = impl->pendingFrame->SetSampleDuration (toTicks (seconds) - impl->lastTime);
    if (SUCCEEDED (hr)) hr = impl->writer->WriteSample (impl->videoStream, impl->pendingFrame.Get());
    impl->pendingFrame.Reset();
    if (SUCCEEDED (hr)) hr = impl->writer->Finalize();
    // Release the file handle now: combine runs while this MovieWriter is alive.
    impl->writer.Reset();
    return SUCCEEDED (hr) ? juce::Result::ok() : failure ("Nao foi possivel finalizar o video", hr);
}

juce::Result MovieWriter::combine (const juce::File& video, const std::vector<AudioFile>& audio,
                                  const juce::File& destination, double duration)
{
    MediaRuntime runtime;
    if (auto result = runtime.check(); result.failed()) return result;
    if (! video.existsAsFile()) return juce::Result::fail ("O arquivo de video nao foi encontrado.");
    if (destination.exists() || destination == video) return juce::Result::fail ("O arquivo de destino ja existe.");
    if (! validTime (duration) || duration <= 0) return juce::Result::fail ("Duracao de exportacao invalida.");
    const auto endTime = toTicks (duration);
    const auto totalAudioFrames = static_cast<juce::int64> (std::llround (duration * audioRate));
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<std::unique_ptr<MixTrack>> tracks;
    double maximum = 0;
    for (const auto& file : audio)
    {
        if (! std::isfinite (file.gain) || ! std::isfinite (file.peak) || ! std::isfinite (file.offsetSeconds))
            return juce::Result::fail ("Parametros de audio invalidos.");
        if (file.gain <= 0 || file.offsetSeconds >= duration) continue;
        if (! file.file.existsAsFile()) return juce::Result::fail ("Faixa de audio nao encontrada: " + file.file.getFileName());
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file.file));
        if (! reader || ! std::isfinite (reader->sampleRate) || reader->sampleRate < 1 || reader->sampleRate > 768000
            || reader->numChannels == 0 || reader->lengthInSamples <= 0)
            return juce::Result::fail ("Faixa de audio invalida: " + file.file.getFileName());
        maximum += static_cast<double> (file.gain) * juce::jmax (0.0f, file.peak);
        tracks.push_back (std::make_unique<MixTrack> (std::move (reader), file));
    }
    const float protection = maximum > 0.98 ? static_cast<float> (0.98 / maximum) : 1.0f;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> videoMedia;
    ComPtr<IMFSinkWriter> writer;
    auto hr = mediaApi().createReader (video.getFullPathName().toWideCharPointer(), nullptr, reader.GetAddressOf());
    if (SUCCEEDED (hr)) hr = reader->SetStreamSelection (MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED (hr)) hr = reader->SetStreamSelection (MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (SUCCEEDED (hr)) hr = reader->GetNativeMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, videoMedia.GetAddressOf());
    GUID subtype {};
    if (SUCCEEDED (hr)) hr = videoMedia->GetGUID (MF_MT_SUBTYPE, &subtype);
    if (SUCCEEDED (hr) && subtype != MFVideoFormat_H264) hr = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED (hr)) hr = reader->SetCurrentMediaType (MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, videoMedia.Get());
    if (SUCCEEDED (hr)) hr = createSink (destination, writer);
    DWORD videoStream = 0, audioStream = 0;
    // Copy compressed H.264 samples and their decoder configuration unchanged;
    // exporting audio must not encode the already-compressed picture again.
    if (SUCCEEDED (hr)) hr = writer->AddStream (videoMedia.Get(), &videoStream);
    if (SUCCEEDED (hr)) hr = writer->SetInputMediaType (videoStream, videoMedia.Get(), nullptr);
    if (SUCCEEDED (hr) && ! tracks.empty()) hr = addAudioStream (*writer.Get(), audioStream);
    if (SUCCEEDED (hr)) hr = writer->BeginWriting();
    if (FAILED (hr)) return failure ("Nao foi possivel preparar a exportacao MP4", hr);

    juce::AudioBuffer<float> mixed (2, audioBlock), temporary (2, audioBlock);
    juce::int64 audioPosition = 0;
    LONGLONG videoTime = 0;
    ComPtr<IMFSample> videoSample;
    bool videoEnded = false, wroteVideo = false;
    while (! videoEnded || videoSample || (! tracks.empty() && audioPosition < totalAudioFrames))
    {
        if (! videoEnded && ! videoSample)
        {
            DWORD flags = 0;
            hr = reader->ReadSample (MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags,
                                     &videoTime, videoSample.GetAddressOf());
            if (FAILED (hr)) return failure ("Falha ao ler o video gravado", hr);
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) return failure ("Falha na leitura do video", E_FAIL);
            if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0)
                return failure ("O formato do video mudou durante a exportacao", MF_E_INVALIDMEDIATYPE);
            videoEnded = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (videoSample && videoTime >= endTime) { videoSample.Reset(); videoEnded = true; }
            if (! videoSample && ! videoEnded) continue;
        }
        const bool writeAudio = ! tracks.empty() && audioPosition < totalAudioFrames
                             && (! videoSample || audioTicks (audioPosition) <= videoTime);
        if (writeAudio)
        {
            const int count = static_cast<int> (juce::jmin<juce::int64> (audioBlock, totalAudioFrames - audioPosition));
            mixed.clear();
            for (auto& track : tracks)
            {
                const int offset = static_cast<int> (juce::jlimit<juce::int64> (0, count, track->start - audioPosition));
                if (offset == count) continue;
                temporary.clear();
                track->resampler.getNextAudioBlock ({ &temporary, offset, count - offset });
                if (track->source.failed) return juce::Result::fail ("Falha ao ler uma faixa de audio gravada.");
                for (int channel = 0; channel < 2; ++channel)
                    mixed.addFrom (channel, offset, temporary, channel, offset, count - offset, track->gain * protection);
            }
            ComPtr<IMFMediaBuffer> buffer;
            const DWORD bytes = static_cast<DWORD> (count * 4);
            hr = mediaApi().createMemoryBuffer (bytes, buffer.GetAddressOf());
            BYTE* data = nullptr;
            if (SUCCEEDED (hr)) hr = buffer->Lock (&data, nullptr, nullptr);
            if (FAILED (hr)) return failure ("Sem memoria para o audio da exportacao", hr);
            auto* pcm = reinterpret_cast<std::int16_t*> (data);
            for (int frame = 0; frame < count; ++frame)
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto value = mixed.getSample (channel, frame);
                    pcm[frame * 2 + channel] = static_cast<std::int16_t> (std::lround (
                        juce::jlimit (-0.98f, 0.98f, std::isfinite (value) ? value : 0.0f) * 32767.0f));
                }
            hr = buffer->Unlock();
            ComPtr<IMFSample> sample;
            const auto start = audioTicks (audioPosition), end = audioTicks (audioPosition + count);
            if (SUCCEEDED (hr)) hr = makeSample (buffer, bytes, start, end - start, sample);
            if (SUCCEEDED (hr)) hr = writer->WriteSample (audioStream, sample.Get());
            if (FAILED (hr)) return failure ("Falha ao codificar o audio AAC", hr);
            audioPosition += count;
        }
        else if (videoSample)
        {
            hr = writer->WriteSample (videoStream, videoSample.Get());
            if (FAILED (hr)) return failure ("Falha ao exportar o video H.264", hr);
            videoSample.Reset();
            wroteVideo = true;
        }
    }
    if (! wroteVideo) return juce::Result::fail ("O video nao possui quadros validos.");
    hr = writer->Finalize();
    writer.Reset();
    reader.Reset();
    return SUCCEEDED (hr) ? juce::Result::ok() : failure ("Nao foi possivel finalizar o MP4", hr);
}

void requestMediaPermission (bool, std::function<void(bool)> callback)
{
    // Desktop Win32 access is governed by Windows camera/microphone privacy
    // settings. JUCE reports a denied or unavailable device from openDeviceAsync.
    if (callback) callback (true);
}
} // namespace churchstudio
#endif
