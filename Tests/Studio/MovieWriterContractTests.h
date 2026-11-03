#pragma once

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Studio/StudioMovieWriter.h"

// Direct native-writer contract test. Call with an output directory from a
// JUCE-initialised test process; no camera, audio device or message-loop pumping
// is required. All media are generated offline. Each run gets a fresh Unicode
// subdirectory, so previous captures are retained rather than overwritten.
//
// Native Result/file checks run here. Codec, timing, orientation and decoded
// audio checks belong to the independent ffprobe/Python verifier described by
// movie-writer-contract.json. A successful return alone is NOT a codec/audio
// verification. Bitrate, AAC sample rate and encoder metadata are not fixed.
namespace movie_writer_contract_detail
{
constexpr int fps = 30;
constexpr double duration = 4.0;
constexpr int frameCount = 120;
constexpr double fadeSeconds = 0.01;

inline juce::var object() { return juce::var (new juce::DynamicObject()); }
inline void set (juce::var& target, const char* key, const juce::var& value)
{
    target.getDynamicObject()->setProperty (juce::Identifier (key), value);
}
inline juce::var array (std::initializer_list<juce::var> values)
{
    juce::Array<juce::var> result;
    for (const auto& value : values) result.add (value);
    return juce::var (result);
}

struct ToneFile
{
    juce::File file;
    double sampleRate = 44100.0;
    int channels = 2;
    double seconds = 4.5;
    double amplitude = 0.72;
    std::array<double, 2> frequencies { 440.0, 880.0 };
};

struct Contract
{
    int failures = 0;
    int checks = 0;
    juce::Array<juce::var> failureDetails, inputs, outputs;

    bool check (bool success, const juce::String& description)
    {
        ++checks;
        if (! success)
        {
            ++failures;
            failureDetails.add (description);
            std::cerr << "MovieWriter contract FAIL: " << description << '\n';
        }
        return success;
    }

    bool result (const juce::Result& value, const juce::String& description)
    {
        return check (value.wasOk(), description + ": " + value.getErrorMessage());
    }

    bool nonempty (const juce::File& file)
    {
        return check (file.existsAsFile() && file.getSize() > 0,
                      "nonempty output " + file.getFullPathName());
    }

    bool makeWav (const ToneFile& tone)
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream (tone.file.createOutputStream());
        if (! check (stream != nullptr, "create fixture WAV " + tone.file.getFileName()))
            return false;

        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
            .withSampleRate (tone.sampleRate).withNumChannels (tone.channels)
            .withBitsPerSample (24));
        if (! check (writer != nullptr, "open fixture WAV writer " + tone.file.getFileName()))
            return false;

        const auto samples = (juce::int64) std::llround (tone.sampleRate * tone.seconds);
        juce::AudioBuffer<float> buffer (tone.channels, 1024);
        double measuredPeak = 0;
        for (juce::int64 start = 0; start < samples; start += buffer.getNumSamples())
        {
            const int count = (int) juce::jmin ((juce::int64) buffer.getNumSamples(), samples - start);
            for (int channel = 0; channel < tone.channels; ++channel)
                for (int sample = 0; sample < count; ++sample)
                {
                    const double time = (double) (start + sample) / tone.sampleRate;
                    const double envelope = juce::jlimit (0.0, 1.0,
                        juce::jmin (time, tone.seconds - time) / fadeSeconds);
                    const float value = (float) (tone.amplitude * envelope * std::sin (
                        juce::MathConstants<double>::twoPi * tone.frequencies[(size_t) channel] * time));
                    buffer.setSample (channel, sample, value);
                    measuredPeak = juce::jmax (measuredPeak, std::abs ((double) value));
                }
            if (! check (writer->writeFromAudioSampleBuffer (buffer, 0, count),
                         "write fixture WAV " + tone.file.getFileName()))
                return false;
        }
        if (! check (writer->flush(), "flush fixture WAV " + tone.file.getFileName()))
            return false;
        writer.reset();

        auto input = tone.file.createInputStream();
        if (! check (input != nullptr, "reopen fixture WAV " + tone.file.getFileName()))
            return false;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (input.release(), true));
        if (! check (reader != nullptr, "read fixture WAV " + tone.file.getFileName()))
            return false;
        if (! check (reader->sampleRate == tone.sampleRate
                     && (int) reader->numChannels == tone.channels && reader->lengthInSamples == samples,
                     "fixture WAV rate, channels and exact sample count " + tone.file.getFileName()))
            return false;

        auto metadata = object();
        set (metadata, "path", tone.file.getFullPathName());
        set (metadata, "sample_rate", tone.sampleRate);
        set (metadata, "channels", tone.channels);
        set (metadata, "sample_count", samples);
        set (metadata, "duration_seconds", tone.seconds);
        set (metadata, "pcm_bits", 24);
        set (metadata, "amplitude", tone.amplitude);
        set (metadata, "measured_generated_peak", measuredPeak);
        set (metadata, "edge_fade_seconds", fadeSeconds);
        set (metadata, "channel_frequencies_hz", tone.channels == 1
            ? array ({ tone.frequencies[0] }) : array ({ tone.frequencies[0], tone.frequencies[1] }));
        inputs.add (metadata);
        return true;
    }

    static juce::Image frame (int width, int height, int index)
    {
        juce::Image image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
        juce::Graphics g (image);
        const std::array<juce::Colour, 4> colours {
            juce::Colour (0xffe03030), juce::Colour (0xff30c050),
            juce::Colour (0xff3060e0), juce::Colour (0xfff0c830)
        };
        const std::array<const char*, 4> labels { "TOP LEFT / RED", "TOP RIGHT / GREEN",
                                                "BOTTOM LEFT / BLUE", "BOTTOM RIGHT / YELLOW" };
        for (int quadrant = 0; quadrant < 4; ++quadrant)
        {
            const int x = (quadrant % 2) * width / 2;
            const int y = (quadrant / 2) * height / 2;
            g.setColour (colours[(size_t) quadrant]);
            g.fillRect (x, y, width / 2, height / 2);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (height * 0.034f));
            g.drawText (labels[(size_t) quadrant], x + 24, y + 22, width / 2 - 48, 44,
                        juce::Justification::centredLeft);
        }

        // The seven-bit binary frame number can be read from decoded pixels,
        // independently of fonts/OCR. It exposes dropped or duplicated frames.
        const float scale = width / 1280.0f;
        g.setColour (juce::Colour (0xff101820));
        g.fillRect (0, height - juce::roundToInt (80 * scale), width, juce::roundToInt (80 * scale));
        for (int bit = 0; bit < 7; ++bit)
        {
            g.setColour ((index & (1 << bit)) != 0 ? juce::Colours::white : juce::Colours::black);
            g.fillRect (juce::roundToInt ((32 + bit * 36) * scale),
                        height - juce::roundToInt (55 * scale),
                        juce::roundToInt (28 * scale), juce::roundToInt (28 * scale));
        }
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (26 * scale));
        g.drawText ("FRAME " + juce::String (index).paddedLeft ('0', 3)
                    + "  /  " + juce::String ((double) index / fps, 3) + " s  /  "
                    + juce::String (width) + " x " + juce::String (height),
                    juce::roundToInt (330 * scale), height - juce::roundToInt (65 * scale),
                    width - juce::roundToInt (350 * scale), juce::roundToInt (45 * scale),
                    juce::Justification::centredLeft);
        return image;
    }

    static juce::var videoExpectation (int width, int height)
    {
        auto expected = object();
        set (expected, "codec", "h264");
        set (expected, "width", width);
        set (expected, "height", height);
        set (expected, "frame_count", frameCount);
        set (expected, "fps", fps);
        set (expected, "duration_seconds", duration);
        set (expected, "duration_tolerance_seconds", 0.08);
        set (expected, "frame_timestamp_tolerance_seconds", 0.002);
        set (expected, "rotation_degrees", 0);

        juce::Array<juce::var> probes;
        for (const auto& probe : std::array<std::array<double, 5>, 4> {{
            {{ 0.25, 0.25, 224, 48, 48 }}, {{ 0.75, 0.25, 48, 192, 80 }},
            {{ 0.25, 0.75, 48, 96, 224 }}, {{ 0.75, 0.75, 240, 200, 48 }} }})
        {
            auto point = object();
            set (point, "normalised_xy", array ({ probe[0], probe[1] }));
            set (point, "rgb", array ({ probe[2], probe[3], probe[4] }));
            probes.add (point);
        }
        set (expected, "orientation_colour_probes", juce::var (probes));
        set (expected, "rgb_absolute_tolerance", 35);

        juce::Array<juce::var> centres;
        const double scale = width / 1280.0;
        for (int bit = 0; bit < 7; ++bit)
            centres.add (array ({ (46 + bit * 36) * scale, height - 41 * scale }));
        set (expected, "frame_number_bit_centres_pixels_lsb_first", juce::var (centres));
        set (expected, "frame_number_black_white_threshold", 128);
        set (expected, "frame_number_sequence", "0..119, exactly once in presentation order");
        return expected;
    }

    bool makeVideo (const juce::File& file, int width, int height)
    {
        churchstudio::MovieWriter writer;
        if (! result (writer.open (file, width, height), "open " + file.getFileName()))
            return false;

        // A defined macOS rejection must not poison subsequent valid frames.
        const auto wrongSize = frame (width - 2, height, 0);
        check (writer.append (wrongSize, 0.0).failed(), "reject wrong-sized ARGB frame");

        bool appended = true;
        for (int index = 0; index < frameCount; ++index)
        {
            if (! result (writer.append (frame (width, height, index), (double) index / fps),
                          "append " + file.getFileName() + " frame " + juce::String (index)))
            {
                appended = false;
                break;
            }
            if (index == fps)
            {
                // A white poison frame would expose accidental acceptance in
                // colour probes/frame-number decoding. Current macOS parity:
                // finite duplicate/backward timestamps are successful no-ops.
                juce::Image poison (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
                poison.clear (poison.getBounds(), juce::Colours::white);
                check (writer.append (poison, 1.0).wasOk(), "ignore duplicate timestamp");
                check (writer.append (poison, 0.5).wasOk(), "ignore backward timestamp");
            }
        }
        const bool finished = result (writer.finish (duration), "finish " + file.getFileName());
        const bool present = nonempty (file);

        auto metadata = object();
        set (metadata, "case", "raw-" + juce::String (height) + "p");
        set (metadata, "path", file.getFullPathName());
        set (metadata, "video", videoExpectation (width, height));
        set (metadata, "expected_audio_streams", 0);
        set (metadata, "native_api_success", appended && finished && present);
        outputs.add (metadata);
        return appended && finished && present;
    }

    bool combine (const juce::File& video, const juce::File& destination,
                  const std::vector<churchstudio::AudioFile>& tracks,
                  const juce::String& name, int width, int height,
                  const ToneFile& daw, const ToneFile& mic)
    {
        const bool combined = result (churchstudio::MovieWriter::combine (
            video, tracks, destination, duration), "combine " + name);
        const bool present = nonempty (destination);
        auto metadata = object();
        set (metadata, "case", name);
        set (metadata, "path", destination.getFullPathName());
        set (metadata, "container", "mp4");
        set (metadata, "video", videoExpectation (width, height));
        set (metadata, "native_api_success", combined && present);
        set (metadata, "expected_audio_streams", tracks.empty() ? 0 : 1);
        if (! tracks.empty())
        {
            float maximum = 0;
            for (const auto& track : tracks) maximum += track.gain * track.peak;
            const float protection = maximum > 0.98f ? 0.98f / maximum : 1.0f;
            auto audio = object();
            set (audio, "codec", "aac");
            set (audio, "channels", 2);
            set (audio, "protection_multiplier", protection);
            set (audio, "declared_peak_sum_before_protection", maximum);
            set (audio, "maximum_linear_peak_before_encoding", maximum * protection);
            set (audio, "aac_decoded_peak_tolerance", 0.04);
            set (audio, "amplitude_relative_tolerance", 0.12);
            set (audio, "frequency_tolerance_hz", 2.0);
            set (audio, "onset_offset_tolerance_seconds", 0.05);
            set (audio, "daw_only_analysis_windows_seconds", array ({ array ({ 0.20, 0.80 }), array ({ 3.20, 3.80 }) }));
            set (audio, "daw_plus_mic_analysis_window_seconds", array ({ 1.20, 2.80 }));
            set (audio, "excluded_frequency_max_relative_amplitude", 0.02);

            juce::Array<juce::var> sources;
            for (size_t index = 0; index < tracks.size(); ++index)
            {
                const auto& tone = index == 0 ? daw : mic;
                auto source = object();
                set (source, "role", index == 0 ? "DAW" : "microphone");
                set (source, "path", tracks[index].file.getFullPathName());
                set (source, "gain", tracks[index].gain);
                set (source, "declared_peak", tracks[index].peak);
                set (source, "offset_seconds", tracks[index].offsetSeconds);
                set (source, "audible_until_seconds", juce::jmin (duration, tracks[index].offsetSeconds + tone.seconds));
                set (source, "expected_sine_peak_per_routed_channel", tone.amplitude * tracks[index].gain * protection);
                set (source, "left_frequency_hz", tone.frequencies[0]);
                set (source, "right_frequency_hz", tone.frequencies[(size_t) (tone.channels - 1)]);
                set (source, "routing", tone.channels == 1 ? "mono duplicated to both output channels" : "stereo preserved; no channel swap");
                sources.add (source);
            }
            set (audio, "sources", juce::var (sources));
            set (metadata, "audio", audio);
        }
        outputs.add (metadata);
        return combined && present;
    }
};
} // namespace movie_writer_contract_detail

inline int runMovieWriterContractTests (const juce::File& outputFolder)
{
    using namespace movie_writer_contract_detail;
    Contract test;
    if (! test.check (outputFolder != juce::File(), "explicit output folder")
        || ! test.result (outputFolder.createDirectory(), "create output folder"))
        return test.failures;
    const auto run = outputFolder.getNonexistentChildFile (
        juce::String::fromUTF8 (u8"Contrato áudio – vídeo"), {}, false);
    if (! test.result (run.createDirectory(), "create fresh Unicode run folder"))
        return test.failures;

    const bool supported = test.check (churchstudio::MovieWriter::supported(),
                                       "native MovieWriter must be supported (no stub fallback)");
    if (supported)
    {
        churchstudio::MovieWriter unopened;
        test.check (unopened.finish (duration).failed(), "finish before open must fail");
        const auto missingOutput = run.getChildFile ("must-not-exist.mp4");
        test.check (churchstudio::MovieWriter::combine (run.getChildFile ("missing.mov"), {},
                     missingOutput, duration).failed(), "combine missing source video must fail");
        test.check (! missingOutput.exists(), "failed missing-video combine leaves no destination");

        ToneFile daw, mic;
        daw.file = run.getChildFile (juce::String::fromUTF8 (u8"DAW estéreo 44100.wav"));
        mic.file = run.getChildFile (juce::String::fromUTF8 (u8"Microfone mono 48000.wav"));
        mic.sampleRate = 48000.0;
        mic.channels = 1;
        mic.seconds = 2.0;
        mic.amplitude = 0.70;
        mic.frequencies = { 660.0, 660.0 };
        const bool dawReady = test.makeWav (daw);
        const bool micReady = test.makeWav (mic);

        for (const int width : { 1280, 1920 })
        {
            const int height = width * 9 / 16;
            // Recorder also supplies video.mov. The native Windows backend
            // must therefore choose its container independently of extension.
            const auto video = run.getChildFile ("video-" + juce::String (height) + "p.mov");
            if (! test.makeVideo (video, width, height)) continue;
            if (width == 1280)
                test.combine (video, run.getChildFile ("video-only-720p.mp4"), {},
                              "video-only-720p", width, height, daw, mic);
            if (! dawReady || ! micReady) continue;

            const bool protectedMix = width == 1920;
            const std::vector<churchstudio::AudioFile> tracks {
                { daw.file, 0.0, protectedMix ? 1.0f : 0.25f, (float) daw.amplitude },
                { mic.file, 1.0, protectedMix ? 1.25f : 0.50f, (float) mic.amplitude }
            };
            const auto name = protectedMix ? "protected-mix-1080p" : "offset-gain-mix-720p";
            test.combine (video, run.getChildFile (juce::String (name) + ".mp4"), tracks,
                          name, width, height, daw, mic);
        }
    }

    auto report = object();
    set (report, "schema_version", 1);
    set (report, "supported", supported);
    set (report, "run_directory", run.getFullPathName());
    set (report, "native_checks", test.checks);
    set (report, "native_failures", test.failures);
    set (report, "failures", juce::var (test.failureDetails));
    set (report, "inputs", juce::var (test.inputs));
    set (report, "outputs", juce::var (test.outputs));
    set (report, "external_verification_required", true);
    set (report, "external_checks", array ({
        "Probe H264 dimensions, 120 frames, 30 fps, 4-second timeline and no rotation; container names may alias mov/mp4.",
        "Decode RGB and compare quadrant probes away from edges. Decode all seven frame-number bits: exactly 0 through 119, once each.",
        "MP4 mixes must contain AAC stereo. Do not require an encoder brand, exact bitrate, fixed audio sample rate or exact AAC packet count.",
        "Decode audio to PCM; fit 440/660/880 Hz sine/cosine components in each declared analysis window and compare amplitudes/routing.",
        "660 Hz starts at 1 second and ends at 3 seconds. It must be absent from DAW-only windows, while L440/R880 continue through the end.",
        "Compare the protected 1080p amplitudes with the declared common gain multiplier and check decoded peak with the AAC tolerance.",
        "Verify audio/video end alignment within 80 ms; allow AAC priming/padding according to stream timestamps and skip-sample metadata."
    }));
    const auto manifest = run.getChildFile ("movie-writer-contract.json");
    test.check (manifest.replaceWithText (juce::JSON::toString (report, true)), "write external verification manifest");
    std::cout << "MOVIE_WRITER_CONTRACT_REPORT=" << manifest.getFullPathName() << '\n'
              << "MovieWriter native contract: " << (test.failures == 0 ? "PASS" : "FAIL")
              << " (" << test.failures << " failures; external media verification still required)\n";
    return test.failures;
}
