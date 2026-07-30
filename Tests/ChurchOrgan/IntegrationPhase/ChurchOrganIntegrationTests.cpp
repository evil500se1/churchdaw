#include "ChurchOrganLayerProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_MSC_VER)
 #include <malloc.h>
#endif

namespace
{
    std::atomic<bool> trackAllocations { false };
    std::atomic<std::uint64_t> trackedAllocations { 0 };

    void countAllocation() noexcept
    {
        if (trackAllocations.load (std::memory_order_relaxed))
            trackedAllocations.fetch_add (1, std::memory_order_relaxed);
    }

    void* allocateUnaligned (std::size_t size)
    {
        countAllocation();
        if (auto* memory = std::malloc (std::max<std::size_t> (size, 1)))
            return memory;
        throw std::bad_alloc {};
    }

    void* allocateAligned (std::size_t size, std::size_t alignment)
    {
        countAllocation();
#if defined(_MSC_VER)
        if (auto* memory = _aligned_malloc (
                std::max<std::size_t> (size, 1), alignment))
        {
            return memory;
        }
#else
        void* memory = nullptr;
        if (posix_memalign (
                &memory,
                alignment,
                std::max<std::size_t> (size, 1)) == 0)
        {
            return memory;
        }
#endif
        throw std::bad_alloc {};
    }

    void freeAligned (void* memory) noexcept
    {
#if defined(_MSC_VER)
        _aligned_free (memory);
#else
        std::free (memory);
#endif
    }
}

void* operator new (std::size_t size)
{
    return allocateUnaligned (size);
}

void* operator new[] (std::size_t size)
{
    return allocateUnaligned (size);
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void* operator new (
    std::size_t size,
    const std::nothrow_t&) noexcept
{
    try
    {
        return allocateUnaligned (size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[] (
    std::size_t size,
    const std::nothrow_t&) noexcept
{
    try
    {
        return allocateUnaligned (size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete (
    void* memory,
    const std::nothrow_t&) noexcept
{
    std::free (memory);
}

void operator delete[] (
    void* memory,
    const std::nothrow_t&) noexcept
{
    std::free (memory);
}

#if defined(__cpp_aligned_new)
void* operator new (std::size_t size, std::align_val_t alignment)
{
    return allocateAligned (
        size, static_cast<std::size_t> (alignment));
}

void* operator new[] (std::size_t size, std::align_val_t alignment)
{
    return allocateAligned (
        size, static_cast<std::size_t> (alignment));
}

void operator delete (
    void* memory,
    std::align_val_t) noexcept
{
    freeAligned (memory);
}

void operator delete[] (
    void* memory,
    std::align_val_t) noexcept
{
    freeAligned (memory);
}

void operator delete (
    void* memory,
    std::size_t,
    std::align_val_t) noexcept
{
    freeAligned (memory);
}

void operator delete[] (
    void* memory,
    std::size_t,
    std::align_val_t) noexcept
{
    freeAligned (memory);
}
#endif

namespace
{
    using churchorgan::ChurchOrganFamily;
    using churchorgan::ChurchOrganLayerPrepareSpec;
    using churchorgan::ChurchOrganLayerProcessor;
    using churchorgan::ChurchOrganLayerQuality;
    using churchorgan::Manual;
    using churchorgan::OrganParameterId;

    constexpr double testSampleRate = 48000.0;
    constexpr std::uint64_t testSeed = 0x1020304050607080ULL;

    class TestRunner
    {
    public:
        void section (const std::string& name)
        {
            std::cout << "\n[" << name << "]\n";
        }

        void expect (bool condition, const std::string& message)
        {
            ++checks_;
            if (! condition)
            {
                ++failures_;
                std::cerr << "FAIL: " << message << '\n';
            }
        }

        template <typename Actual, typename Expected>
        void expectEqual (
            const Actual& actual,
            const Expected& expected,
            const std::string& message)
        {
            expect (actual == expected, message);
        }

        void expectNear (
            double actual,
            double expected,
            double tolerance,
            const std::string& message)
        {
            expect (
                std::isfinite (actual)
                    && std::abs (actual - expected) <= tolerance,
                message);
        }

        int finish() const
        {
            std::cout << "\nIntegration result: "
                      << (checks_ - failures_) << '/' << checks_
                      << " checks passed\n";
            return failures_ == 0 ? 0 : 1;
        }

    private:
        int checks_ = 0;
        int failures_ = 0;
    };

    struct AudioStats
    {
        double rms = 0.0;
        float peak = 0.0f;
        bool finite = true;
    };

    AudioStats measure (
        const juce::AudioBuffer<float>& audio,
        int startSample = 0,
        int requestedSamples = -1,
        int requestedChannels = 2)
    {
        AudioStats result;
        const auto channels =
            std::min (requestedChannels, audio.getNumChannels());
        const auto available =
            std::max (0, audio.getNumSamples() - startSample);
        const auto samples =
            requestedSamples < 0
                ? available
                : std::min (available, requestedSamples);
        double squareSum = 0.0;
        std::uint64_t count = 0;
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* data = audio.getReadPointer (
                channel, startSample);
            for (int sample = 0; sample < samples; ++sample)
            {
                const auto value = data[sample];
                result.finite =
                    result.finite && std::isfinite (value);
                if (std::isfinite (value))
                {
                    result.peak =
                        std::max (result.peak, std::abs (value));
                    squareSum +=
                        static_cast<double> (value)
                        * static_cast<double> (value);
                }
                ++count;
            }
        }
        result.rms =
            count != 0
                ? std::sqrt (
                    squareSum / static_cast<double> (count))
                : 0.0;
        return result;
    }

    float maximumAbsoluteDifference (
        const juce::AudioBuffer<float>& first,
        const juce::AudioBuffer<float>& second)
    {
        auto difference = 0.0f;
        const auto channels =
            std::min (first.getNumChannels(), second.getNumChannels());
        const auto samples =
            std::min (first.getNumSamples(), second.getNumSamples());
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto* a = first.getReadPointer (channel);
            const auto* b = second.getReadPointer (channel);
            for (int sample = 0; sample < samples; ++sample)
                difference = std::max (
                    difference, std::abs (a[sample] - b[sample]));
        }
        return difference;
    }

    bool channelIsZero (
        const juce::AudioBuffer<float>& audio,
        int channel,
        float tolerance = 0.0f)
    {
        if (channel < 0 || channel >= audio.getNumChannels())
            return false;
        const auto* data = audio.getReadPointer (channel);
        for (int sample = 0;
             sample < audio.getNumSamples();
             ++sample)
        {
            if (std::abs (data[sample]) > tolerance)
                return false;
        }
        return true;
    }

    const char* familyName (ChurchOrganFamily family)
    {
        switch (family)
        {
            case ChurchOrganFamily::tonewheel:
                return "Tonewheel";
            case ChurchOrganFamily::transistor:
                return "Transistor";
            case ChurchOrganFamily::pipe:
                return "Pipe";
            case ChurchOrganFamily::worshipHybrid:
                return "WorshipHybrid";
        }
        return "Invalid";
    }

    std::unique_ptr<ChurchOrganLayerProcessor> makeProcessor (
        ChurchOrganFamily family,
        std::size_t maximumBlock = 8192,
        ChurchOrganLayerQuality quality =
            ChurchOrganLayerQuality::standard,
        double rate = testSampleRate,
        std::uint64_t seed = testSeed)
    {
        auto processor =
            std::make_unique<ChurchOrganLayerProcessor>();
        ChurchOrganLayerPrepareSpec spec;
        spec.sampleRate = rate;
        spec.maximumBlockSize = maximumBlock;
        spec.deterministicSeed = seed;
        spec.quality = quality;
        spec.initialFamily = family;
        if (! processor->prepare (spec))
            return {};
        return processor;
    }

    void addThreeManualChord (
        juce::MidiBuffer& midi,
        int offset = 0)
    {
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, static_cast<juce::uint8> (112)),
            offset);
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, 67, static_cast<juce::uint8> (106)),
            offset + 1);
        midi.addEvent (
            juce::MidiMessage::noteOn (
                2, 48, static_cast<juce::uint8> (102)),
            offset + 2);
        midi.addEvent (
            juce::MidiMessage::noteOn (
                3, 36, static_cast<juce::uint8> (116)),
            offset + 3);
    }

    bool drainUntilInactive (
        ChurchOrganLayerProcessor& processor,
        juce::AudioBuffer<float>& audio,
        int maximumBlocks = 512)
    {
        juce::MidiBuffer empty;
        for (int block = 0; block < maximumBlocks; ++block)
        {
            if (processor.diagnostics().activeNotes == 0)
                return true;
            if (! processor.processBlock (audio, empty))
                return false;
        }
        return processor.diagnostics().activeNotes == 0;
    }

    void testPrepareAndContracts (TestRunner& test)
    {
        test.section ("prepare, topology and RT contract");
        auto processor =
            std::make_unique<ChurchOrganLayerProcessor>();

        ChurchOrganLayerPrepareSpec invalid;
        invalid.sampleRate = 1000.0;
        test.expect (
            ! processor->prepare (invalid),
            "rejects invalid sample rate");
        invalid = {};
        invalid.maximumBlockSize = 8193;
        test.expect (
            ! processor->prepare (invalid),
            "rejects host blocks above 8192");

        for (const auto rate :
             std::array<double, 4> {{ 44100.0, 48000.0,
                                      96000.0, 192000.0 }})
        {
            ChurchOrganLayerPrepareSpec spec;
            spec.sampleRate = rate;
            spec.maximumBlockSize = 64;
            spec.deterministicSeed = testSeed;
            test.expect (
                processor->prepare (spec),
                "prepares supported sample rate "
                    + std::to_string (static_cast<int> (rate)));
        }

        test.expectEqual (
            ChurchOrganLayerProcessor::maximumEngineBlockCapacity,
            std::size_t { 4096 },
            "frozen engines retain 4096-sample capacity");
        test.expectEqual (
            ChurchOrganLayerProcessor::maximumBlockCapacity,
            std::size_t { 8192 },
            "adapter accepts 8192-sample logical blocks");
        test.expect (
            ChurchOrganLayerProcessor::processUsesFixedStorage,
            "process advertises fixed storage");
        test.expect (
            ! ChurchOrganLayerProcessor::processUsesLocks,
            "process advertises no locks");
        test.expect (
            ! ChurchOrganLayerProcessor::appliesPolyphonyHeadroom,
            "adapter has no automatic polyphony headroom");
        test.expect (
            ! ChurchOrganLayerProcessor::hasPermanentLimiterOrAgc,
            "adapter has no permanent limiter or AGC");
        test.expect (
            ! ChurchOrganLayerProcessor::pitchBendSupported,
            "unsupported pitch is explicit in the API");

        auto full = makeProcessor (
            ChurchOrganFamily::tonewheel, 8192);
        test.expect (full != nullptr, "prepares full logical block");
        if (full == nullptr)
            return;

        juce::AudioBuffer<float> tooLarge (2, 8193);
        tooLarge.setSample (0, 0, 1.0f);
        juce::MidiBuffer empty;
        test.expect (
            ! full->processBlock (tooLarge, empty),
            "rejects a runtime block above prepared topology");
        test.expect (
            measure (tooLarge).peak == 0.0f,
            "invalid runtime block is cleared");

        juce::AudioBuffer<float> alias (2, 64);
        test.expect (
            ! full->processBlock (alias, empty, &alias),
            "rejects main/send alias");
    }

    void testEveryFamilyAndChannelMapping (TestRunner& test)
    {
        test.section ("four families, mono/stereo and MIDI manuals");
        const std::array<ChurchOrganFamily, 4> families {{
            ChurchOrganFamily::tonewheel,
            ChurchOrganFamily::transistor,
            ChurchOrganFamily::pipe,
            ChurchOrganFamily::worshipHybrid
        }};

        for (const auto family : families)
        {
            auto processor = makeProcessor (family, 4096);
            test.expect (
                processor != nullptr,
                std::string ("prepare ") + familyName (family));
            if (processor == nullptr)
                continue;

            juce::AudioBuffer<float> stereo (3, 4096);
            juce::AudioBuffer<float> send (3, 4096);
            stereo.clear();
            send.clear();
            for (int channel = 0; channel < 3; ++channel)
                for (int sample = 0; sample < 4096; ++sample)
                    stereo.setSample (
                        channel,
                        sample,
                        std::numeric_limits<float>::quiet_NaN());

            juce::MidiBuffer midi;
            addThreeManualChord (midi, 7);
            test.expect (
                processor->processBlock (stereo, midi, &send),
                std::string ("stereo render ") + familyName (family));

            const auto mainStats = measure (stereo);
            const auto sendStats = measure (send);
            test.expect (
                mainStats.finite && mainStats.peak > 1.0e-6f,
                std::string ("finite audible output ")
                    + familyName (family));
            test.expect (
                channelIsZero (stereo, 2),
                std::string ("clears extra main channel ")
                    + familyName (family));
            test.expect (
                channelIsZero (send, 2),
                std::string ("clears extra send channel ")
                    + familyName (family));
            if (family == ChurchOrganFamily::worshipHybrid)
            {
                test.expect (
                    sendStats.finite && sendStats.peak > 1.0e-8f,
                    "Hybrid publishes an audible reverb send");
            }
            else
            {
                test.expect (
                    sendStats.peak == 0.0f,
                    std::string ("non-Hybrid send is zero ")
                        + familyName (family));
            }

            const auto diagnostics = processor->diagnostics();
            test.expect (
                diagnostics.prepared
                    && diagnostics.renderedSamples == 4096,
                std::string ("diagnostics publish ")
                    + familyName (family));
            if (family == ChurchOrganFamily::transistor)
            {
                test.expectEqual (
                    diagnostics.activeNotes,
                    std::uint32_t { 2 },
                    "Transistor plays both Upper notes");
                test.expectEqual (
                    diagnostics.ignoredChannelEvents,
                    std::uint64_t { 2 },
                    "Transistor explicitly ignores Lower/Pedal");
            }
            else
            {
                test.expect (
                    diagnostics.activeNotes >= 4,
                    std::string ("Upper/Lower/Pedal mapped ")
                        + familyName (family));
            }

            processor->setProcessingSuspended (true);
            test.expect (
                processor->reset(),
                std::string ("suspended reset ")
                    + familyName (family));
            processor->setProcessingSuspended (false);

            juce::AudioBuffer<float> mono (1, 1024);
            juce::MidiBuffer monoMidi;
            monoMidi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 64, static_cast<juce::uint8> (100)),
                11);
            test.expect (
                processor->processBlock (mono, monoMidi),
                std::string ("mono render ") + familyName (family));
            const auto monoStats = measure (mono, 0, -1, 1);
            test.expect (
                monoStats.finite && monoStats.peak > 1.0e-7f,
                std::string ("finite audible mono ")
                    + familyName (family));
        }
    }

    void testChunkingAndSampleAccuracy (TestRunner& test)
    {
        test.section ("8192 chunking and sample-accurate MIDI");

        auto onset = makeProcessor (
            ChurchOrganFamily::transistor, 8192);
        test.expect (onset != nullptr, "prepare onset processor");
        if (onset == nullptr)
            return;
        juce::AudioBuffer<float> onsetAudio (2, 8192);
        juce::MidiBuffer onsetMidi;
        constexpr int onsetSample = 4107;
        onsetMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, static_cast<juce::uint8> (127)),
            onsetSample);
        test.expect (
            onset->processBlock (onsetAudio, onsetMidi),
            "renders event after internal 4096 boundary");
        const auto before =
            measure (onsetAudio, 0, onsetSample);
        const auto after =
            measure (
                onsetAudio,
                onsetSample,
                onsetAudio.getNumSamples() - onsetSample);
        test.expect (
            before.peak == 0.0f,
            "no sound before sample-accurate note-on");
        test.expect (
            after.peak > 1.0e-6f,
            "sound begins after sample-accurate note-on");

        auto whole = makeProcessor (
            ChurchOrganFamily::transistor, 8192);
        auto split = makeProcessor (
            ChurchOrganFamily::transistor, 8192);
        test.expect (
            whole != nullptr && split != nullptr,
            "prepare deterministic chunk comparison");
        if (whole == nullptr || split == nullptr)
            return;

        juce::AudioBuffer<float> wholeAudio (2, 8192);
        juce::MidiBuffer wholeMidi;
        wholeMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 55, static_cast<juce::uint8> (110)),
            17);
        wholeMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 64, static_cast<juce::uint8> (101)),
            4096);
        wholeMidi.addEvent (
            juce::MidiMessage::controllerEvent (1, 11, 73),
            5003);
        wholeMidi.addEvent (
            juce::MidiMessage::noteOff (1, 55),
            7000);
        test.expect (
            whole->processBlock (wholeAudio, wholeMidi),
            "single 8192 render");

        juce::AudioBuffer<float> splitFirst (2, 4096);
        juce::AudioBuffer<float> splitSecond (2, 4096);
        juce::MidiBuffer firstMidi;
        firstMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 55, static_cast<juce::uint8> (110)),
            17);
        juce::MidiBuffer secondMidi;
        secondMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 64, static_cast<juce::uint8> (101)),
            0);
        secondMidi.addEvent (
            juce::MidiMessage::controllerEvent (1, 11, 73),
            5003 - 4096);
        secondMidi.addEvent (
            juce::MidiMessage::noteOff (1, 55),
            7000 - 4096);
        test.expect (
            split->processBlock (splitFirst, firstMidi)
                && split->processBlock (splitSecond, secondMidi),
            "two explicit 4096 renders");

        juce::AudioBuffer<float> recombined (2, 8192);
        for (int channel = 0; channel < 2; ++channel)
        {
            recombined.copyFrom (
                channel, 0, splitFirst, channel, 0, 4096);
            recombined.copyFrom (
                channel, 4096, splitSecond, channel, 0, 4096);
        }
        test.expect (
            maximumAbsoluteDifference (
                wholeAudio, recombined) == 0.0f,
            "internal chunking is bit-identical to host split");
    }

    void testControllersAndSustain (TestRunner& test)
    {
        test.section ("CC7/11/64/120/121/123 and pitch diagnostics");

        auto reference = makeProcessor (
            ChurchOrganFamily::transistor, 4096);
        auto controlled = makeProcessor (
            ChurchOrganFamily::transistor, 4096);
        test.expect (
            reference != nullptr && controlled != nullptr,
            "prepare controller pair");
        if (reference == nullptr || controlled == nullptr)
            return;

        juce::AudioBuffer<float> referenceAudio (2, 4096);
        juce::AudioBuffer<float> controlledAudio (2, 4096);
        juce::MidiBuffer referenceMidi;
        referenceMidi.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, static_cast<juce::uint8> (120)),
            0);
        auto controlledMidi = referenceMidi;
        controlledMidi.addEvent (
            juce::MidiMessage::controllerEvent (1, 7, 0),
            512);
        test.expect (
            reference->processBlock (
                referenceAudio, referenceMidi)
                && controlled->processBlock (
                    controlledAudio, controlledMidi),
            "controller comparison renders");

        auto preDifference = 0.0f;
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto* a = referenceAudio.getReadPointer (channel);
            const auto* b = controlledAudio.getReadPointer (channel);
            for (int sample = 0; sample < 512; ++sample)
                preDifference = std::max (
                    preDifference,
                    std::abs (a[sample] - b[sample]));
        }
        test.expect (
            preDifference == 0.0f,
            "CC7 does not affect samples before its offset");
        test.expect (
            measure (controlledAudio, 2048, 2048).rms
                < measure (referenceAudio, 2048, 2048).rms * 0.25,
            "CC7 smoothly attenuates after its offset");
        test.expectNear (
            controlled->diagnostics().midiVolume,
            0.0,
            1.0e-7,
            "CC7 target persists in diagnostics");

        juce::AudioBuffer<float> persistence (2, 4096);
        juce::MidiBuffer noMidi;
        test.expect (
            controlled->processBlock (persistence, noMidi),
            "renders next controller block");
        test.expect (
            measure (persistence).rms
                < measure (controlledAudio, 2048, 2048).rms,
            "CC7 target persists across blocks");

        const std::array<ChurchOrganFamily, 4> families {{
            ChurchOrganFamily::tonewheel,
            ChurchOrganFamily::transistor,
            ChurchOrganFamily::pipe,
            ChurchOrganFamily::worshipHybrid
        }};
        for (const auto family : families)
        {
            auto processor = makeProcessor (family, 512);
            if (processor == nullptr)
            {
                test.expect (
                    false,
                    std::string ("prepare sustain ")
                        + familyName (family));
                continue;
            }
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer sustainMidi;
            sustainMidi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, static_cast<juce::uint8> (110)),
                0);
            sustainMidi.addEvent (
                juce::MidiMessage::controllerEvent (1, 64, 127),
                64);
            sustainMidi.addEvent (
                juce::MidiMessage::noteOff (1, 60),
                128);
            test.expect (
                processor->processBlock (audio, sustainMidi),
                std::string ("sustain-down render ")
                    + familyName (family));
            test.expect (
                processor->diagnostics().activeNotes >= 1,
                std::string ("sustain holds note ")
                    + familyName (family));

            juce::MidiBuffer releaseMidi;
            releaseMidi.addEvent (
                juce::MidiMessage::controllerEvent (1, 64, 0),
                0);
            test.expect (
                processor->processBlock (audio, releaseMidi),
                std::string ("sustain-up render ")
                    + familyName (family));
            test.expect (
                drainUntilInactive (*processor, audio),
                std::string (
                    "sustain-up reaches the native release tail ")
                    + familyName (family));
        }

        for (const auto family : families)
        {
            auto processor = makeProcessor (family, 512);
            test.expect (
                processor != nullptr,
                std::string ("prepare CC121 ")
                    + familyName (family));
            if (processor == nullptr)
                continue;

            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer held;
            held.addEvent (
                juce::MidiMessage::noteOn (
                    1, 62, static_cast<juce::uint8> (110)),
                0);
            held.addEvent (
                juce::MidiMessage::controllerEvent (1, 11, 9),
                1);
            held.addEvent (
                juce::MidiMessage::controllerEvent (1, 64, 127),
                2);
            held.addEvent (
                juce::MidiMessage::noteOff (1, 62),
                3);
            test.expect (
                processor->processBlock (audio, held),
                std::string ("CC121 setup render ")
                    + familyName (family));
            test.expect (
                processor->diagnostics().activeNotes >= 1,
                std::string ("CC121 setup is sustained ")
                    + familyName (family));
            test.expectNear (
                processor->diagnostics().midiExpression,
                9.0 / 127.0,
                1.0e-6,
                std::string ("expression lowered before CC121 ")
                    + familyName (family));

            juce::MidiBuffer resetControllers;
            resetControllers.addEvent (
                juce::MidiMessage::controllerEvent (1, 121, 0),
                37);
            test.expect (
                processor->processBlock (
                    audio, resetControllers),
                std::string ("CC121 render ")
                    + familyName (family));
            test.expectNear (
                processor->diagnostics().midiExpression,
                1.0,
                1.0e-7,
                std::string ("CC121 restores expression ")
                    + familyName (family));
            test.expect (
                drainUntilInactive (*processor, audio),
                std::string (
                    "CC121 releases sustain through native tail ")
                    + familyName (family));
        }

        for (const auto family : families)
        {
            auto sameOffset = makeProcessor (family, 256);
            test.expect (
                sameOffset != nullptr,
                std::string ("prepare equal-offset ordering ")
                    + familyName (family));
            if (sameOffset == nullptr)
                continue;

            juce::AudioBuffer<float> audio (2, 256);
            juce::MidiBuffer start;
            start.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, static_cast<juce::uint8> (112)),
                0);
            test.expect (
                sameOffset->processBlock (audio, start),
                std::string ("equal-offset ordering setup ")
                    + familyName (family));

            juce::MidiBuffer pedalThenOff;
            pedalThenOff.addEvent (
                juce::MidiMessage::controllerEvent (
                    1, 64, 127),
                32);
            pedalThenOff.addEvent (
                juce::MidiMessage::noteOff (1, 60),
                32);
            test.expect (
                sameOffset->processBlock (
                    audio, pedalThenOff),
                std::string (
                    "pedal-down then note-off at one sample renders ")
                    + familyName (family));

            juce::MidiBuffer empty;
            auto heldAcrossTailWindow = true;
            for (int block = 0; block < 64; ++block)
            {
                heldAcrossTailWindow =
                    heldAcrossTailWindow
                    && sameOffset->processBlock (audio, empty);
            }
            test.expect (
                heldAcrossTailWindow
                    && sameOffset->diagnostics().activeNotes >= 1,
                std::string (
                    "equal-offset source order preserves pedal hold ")
                    + familyName (family));

            juce::MidiBuffer pedalUp;
            pedalUp.addEvent (
                juce::MidiMessage::controllerEvent (1, 64, 0),
                0);
            test.expect (
                sameOffset->processBlock (audio, pedalUp)
                    && drainUntilInactive (
                        *sameOffset, audio),
                std::string (
                    "held equal-offset note releases on pedal-up ")
                    + familyName (family));
        }

        for (const auto family : families)
        {
            auto zeroLength = makeProcessor (family, 256);
            test.expect (
                zeroLength != nullptr,
                std::string ("prepare zero-length ordering ")
                    + familyName (family));
            if (zeroLength == nullptr)
                continue;

            juce::AudioBuffer<float> audio (2, 256);
            juce::MidiBuffer midi;
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 65, static_cast<juce::uint8> (120)),
                80);
            midi.addEvent (
                juce::MidiMessage::noteOff (1, 65),
                80);
            test.expect (
                zeroLength->processBlock (audio, midi)
                    && drainUntilInactive (
                        *zeroLength, audio),
                std::string (
                    "note-on then note-off at one sample cannot stick ")
                    + familyName (family));
        }

        for (const auto family : families)
        {
            for (const auto allOffController :
                 std::array<int, 2> {{ 120, 123 }})
            {
                for (const auto allOffFirst : { false, true })
                {
                    auto allOffSafety = makeProcessor (family, 256);
                    test.expect (
                        allOffSafety != nullptr,
                        std::string ("prepare all-off ordering ")
                            + familyName (family));
                    if (allOffSafety == nullptr)
                        continue;

                    juce::AudioBuffer<float> audio (2, 256);
                    juce::MidiBuffer midi;
                    const auto allOff =
                        juce::MidiMessage::controllerEvent (
                            1, allOffController, 0);
                    const auto noteOn =
                        juce::MidiMessage::noteOn (
                            1,
                            69,
                            static_cast<juce::uint8> (120));
                    if (allOffFirst)
                    {
                        midi.addEvent (allOff, 64);
                        midi.addEvent (noteOn, 64);
                    }
                    else
                    {
                        midi.addEvent (noteOn, 64);
                        midi.addEvent (allOff, 64);
                    }
                    test.expect (
                        allOffSafety->processBlock (audio, midi)
                            && drainUntilInactive (
                                *allOffSafety, audio),
                        std::string (
                            allOffController == 120
                                ? "all-sound-off wins its sample "
                                : "all-notes-off wins its sample ")
                            + (allOffFirst
                                ? "when received first "
                                : "when received last ")
                            + familyName (family));
                }
            }
        }

        auto panic = makeProcessor (
            ChurchOrganFamily::tonewheel, 512);
        test.expect (panic != nullptr, "prepare panic processor");
        if (panic != nullptr)
        {
            juce::AudioBuffer<float> audio (2, 512);
            juce::MidiBuffer midi;
            addThreeManualChord (midi, 0);
            midi.addEvent (
                juce::MidiMessage::controllerEvent (1, 120, 0),
                100);
            midi.addEvent (
                juce::MidiMessage::controllerEvent (2, 123, 0),
                101);
            midi.addEvent (
                juce::MidiMessage::controllerEvent (3, 123, 0),
                102);
            midi.addEvent (
                juce::MidiMessage::pitchWheel (1, 9000),
                80);
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    4, 72, static_cast<juce::uint8> (100)),
                40);
            test.expect (
                panic->processBlock (audio, midi),
                "panic and unsupported messages render");
            const auto diagnostics = panic->diagnostics();
            test.expectEqual (
                diagnostics.unsupportedPitchEvents,
                std::uint64_t { 1 },
                "pitch bend is counted as unsupported");
            test.expectEqual (
                diagnostics.ignoredChannelEvents,
                std::uint64_t { 1 },
                "channel four is explicitly ignored");
            test.expect (
                drainUntilInactive (*panic, audio),
                "CC120/123 reach silence after native release tails");
        }
    }

    void testSharedPerformanceEnvelope (TestRunner& test)
    {
        test.section (
            "shared channel Attack/Release drives all native families");
        const std::array<ChurchOrganFamily, 4> families {{
            ChurchOrganFamily::tonewheel,
            ChurchOrganFamily::transistor,
            ChurchOrganFamily::pipe,
            ChurchOrganFamily::worshipHybrid
        }};
        constexpr int blockSize = 2048;

        test.expectNear (
            churchorgan::descriptor (
                OrganParameterId::performanceAttackMs).defaultValue,
            0.0,
            0.0,
            "performance attack defaults to native articulation");
        test.expectNear (
            churchorgan::descriptor (
                OrganParameterId::performanceReleaseMs).defaultValue,
            0.0,
            0.0,
            "performance release defaults to native articulation");

        for (const auto family : families)
        {
            const auto name = std::string (familyName (family));

            auto native = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 31);
            auto defaultBridge = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 31);
            test.expect (
                native != nullptr && defaultBridge != nullptr,
                name + " prepares default-compatibility pair");
            if (native == nullptr || defaultBridge == nullptr)
                continue;

            defaultBridge->setChannelPerformanceEnvelopeSeconds (
                ChurchOrganLayerProcessor::nativeChannelAttackSeconds,
                ChurchOrganLayerProcessor::nativeChannelReleaseSeconds);
            test.expectNear (
                defaultBridge->parameters().getPlain (
                    OrganParameterId::performanceAttackMs),
                0.0,
                0.0,
                name + " maps default channel attack to native");
            test.expectNear (
                defaultBridge->parameters().getPlain (
                    OrganParameterId::performanceReleaseMs),
                0.0,
                0.0,
                name + " maps default channel release to native");

            juce::MidiBuffer defaultMidi;
            defaultMidi.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, static_cast<juce::uint8> (112)),
                0);
            juce::AudioBuffer<float> nativeAudio (2, blockSize);
            juce::AudioBuffer<float> bridgedAudio (2, blockSize);
            nativeAudio.clear();
            bridgedAudio.clear();
            test.expect (
                native->processBlock (nativeAudio, defaultMidi)
                    && defaultBridge->processBlock (
                        bridgedAudio, defaultMidi),
                name + " renders default compatibility pair");
            test.expectNear (
                maximumAbsoluteDifference (
                    nativeAudio, bridgedAudio),
                0.0,
                1.0e-7,
                name + " native default remains sample-compatible");

            auto fastAttack = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 47);
            auto slowAttack = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 47);
            test.expect (
                fastAttack != nullptr && slowAttack != nullptr,
                name + " prepares attack pair");
            if (fastAttack == nullptr || slowAttack == nullptr)
                continue;

            fastAttack->setParameterPlain (
                OrganParameterId::performanceAttackMs, 1.0f);
            slowAttack->setParameterPlain (
                OrganParameterId::performanceAttackMs, 1200.0f);
            juce::AudioBuffer<float> fastAudio (2, blockSize);
            juce::AudioBuffer<float> slowAudio (2, blockSize);
            fastAudio.clear();
            slowAudio.clear();
            test.expect (
                fastAttack->processBlock (fastAudio, defaultMidi)
                    && slowAttack->processBlock (
                        slowAudio, defaultMidi),
                name + " renders attack comparison");
            const auto fastAttackStats = measure (fastAudio);
            const auto slowAttackStats = measure (slowAudio);
            test.expect (
                fastAttackStats.finite
                    && slowAttackStats.finite
                    && fastAttackStats.rms > 1.0e-7,
                name + " attack comparison is finite and audible");
            test.expect (
                slowAttackStats.rms
                    < fastAttackStats.rms * 0.72,
                name + " long Attack measurably slows onset");

            auto shortRelease = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 63);
            auto longRelease = makeProcessor (
                family, blockSize,
                ChurchOrganLayerQuality::standard,
                testSampleRate, testSeed + 63);
            test.expect (
                shortRelease != nullptr && longRelease != nullptr,
                name + " prepares release pair");
            if (shortRelease == nullptr || longRelease == nullptr)
                continue;

            for (auto* processor :
                 { shortRelease.get(), longRelease.get() })
            {
                processor->setParameterPlain (
                    OrganParameterId::performanceAttackMs, 1.0f);
            }
            shortRelease->setParameterPlain (
                OrganParameterId::performanceReleaseMs, 40.0f);
            longRelease->setParameterPlain (
                OrganParameterId::performanceReleaseMs, 2500.0f);

            juce::AudioBuffer<float> shortAudio (2, blockSize);
            juce::AudioBuffer<float> longAudio (2, blockSize);
            shortAudio.clear();
            longAudio.clear();
            test.expect (
                shortRelease->processBlock (
                    shortAudio, defaultMidi)
                    && longRelease->processBlock (
                        longAudio, defaultMidi),
                name + " starts release comparison");
            juce::MidiBuffer empty;
            for (int warm = 0; warm < 8; ++warm)
            {
                shortRelease->processBlock (shortAudio, empty);
                longRelease->processBlock (longAudio, empty);
            }

            juce::MidiBuffer noteOff;
            noteOff.addEvent (
                juce::MidiMessage::noteOff (1, 60), 0);
            shortRelease->processBlock (shortAudio, noteOff);
            longRelease->processBlock (longAudio, noteOff);
            for (int tail = 0; tail < 12; ++tail)
            {
                shortRelease->processBlock (shortAudio, empty);
                longRelease->processBlock (longAudio, empty);
            }
            const auto shortTail = measure (shortAudio);
            const auto longTail = measure (longAudio);
            test.expect (
                shortTail.finite && longTail.finite
                    && longTail.rms > 1.0e-7,
                name + " release comparison is finite and audible");
            test.expect (
                longTail.rms
                    > shortTail.rms * 4.0 + 1.0e-8,
                name + " long Release measurably preserves tail");

            std::cout
                << name
                << " attack_fast_rms=" << fastAttackStats.rms
                << " attack_slow_rms=" << slowAttackStats.rms
                << " release_short_rms=" << shortTail.rms
                << " release_long_rms=" << longTail.rms
                << '\n';
        }
    }

    void testFamilySwitchAndState (TestRunner& test)
    {
        test.section ("suspended family switch and versioned state");
        auto source = makeProcessor (
            ChurchOrganFamily::tonewheel,
            2048,
            ChurchOrganLayerQuality::ultra,
            testSampleRate,
            0x1122334455667788ULL);
        test.expect (source != nullptr, "prepare state source");
        if (source == nullptr)
            return;

        test.expect (
            ! source->selectFamilyWhileSuspended (
                ChurchOrganFamily::pipe),
            "family switch is rejected while callback is enabled");
        source->setProcessingSuspended (true);
        test.expect (
            source->selectFamilyWhileSuspended (
                ChurchOrganFamily::pipe),
            "family switch succeeds while suspended");
        test.expect (
            ! source->selectFamilyWhileSuspended (
                static_cast<ChurchOrganFamily> (99)),
            "invalid family is rejected");

        source->setFamilyOutputTrim (
            ChurchOrganFamily::tonewheel, 0.91f);
        source->setFamilyOutputTrim (
            ChurchOrganFamily::transistor, 0.82f);
        source->setFamilyOutputTrim (
            ChurchOrganFamily::pipe, 0.73f);
        source->setFamilyOutputTrim (
            ChurchOrganFamily::worshipHybrid, 0.64f);
        source->setParameterPlain (
            OrganParameterId::masterGainDb, -3.25f);
        source->setChannelPerformanceEnvelopeSeconds (
            0.731f, 4.321f);
        source->setPipeRankLevel (
            churchorgan::PipeOrganManual::upper,
            churchorgan::PipeOrganRank::trumpet,
            0.44f);
        source->setPipeTemperament (
            churchorgan::PipeOrganTemperament::gentleMeantone);

        juce::MemoryBlock encoded;
        source->getStateInformation (encoded);
        test.expect (
            encoded.getSize() > 64,
            "schema-2 state serializes to binary");

        auto destination = makeProcessor (
            ChurchOrganFamily::transistor,
            2048,
            ChurchOrganLayerQuality::eco,
            testSampleRate,
            7);
        test.expect (destination != nullptr, "prepare state destination");
        if (destination == nullptr)
            return;

        test.expect (
            ! destination->setStateInformation (
                encoded.getData(), encoded.getSize()),
            "state restore is rejected while running");
        destination->setProcessingSuspended (true);
        test.expect (
            destination->setStateInformation (
                encoded.getData(), encoded.getSize()),
            "schema-2 state restores while suspended");
        test.expect (
            destination->isProcessingSuspended(),
            "restore leaves processing suspended");
        test.expectEqual (
            destination->activeFamily(),
            ChurchOrganFamily::pipe,
            "family round-trips");
        test.expectEqual (
            destination->prepareSpec().quality,
            ChurchOrganLayerQuality::ultra,
            "quality round-trips");
        test.expectEqual (
            destination->prepareSpec().deterministicSeed,
            std::uint64_t { 0x1122334455667788ULL },
            "deterministic seed round-trips");
        test.expectNear (
            destination->parameters().getPlain (
                OrganParameterId::masterGainDb),
            -3.25,
            1.0e-6,
            "general parameter round-trips");
        test.expectNear (
            destination->parameters().getPlain (
                OrganParameterId::performanceAttackMs),
            731.0,
            1.0e-4,
            "shared performance attack round-trips");
        test.expectNear (
            destination->parameters().getPlain (
                OrganParameterId::performanceReleaseMs),
            4321.0,
            1.0e-4,
            "shared performance release round-trips");
        test.expectNear (
            destination->familyOutputTrim (
                ChurchOrganFamily::pipe),
            0.73,
            1.0e-6,
            "per-family trim round-trips");

        auto future = source->createState();
        future.setProperty ("schema", 999, nullptr);
        test.expect (
            ! destination->restoreState (future),
            "future state schema is rejected");

        juce::ValueTree legacy { "ChurchOrganLayerState" };
        legacy.setProperty ("schema", 1, nullptr);
        legacy.setProperty (
            "family",
            static_cast<int> (
                ChurchOrganFamily::transistor),
            nullptr);
        legacy.setProperty (
            "quality",
            static_cast<int> (
                ChurchOrganLayerQuality::eco),
            nullptr);
        legacy.setProperty ("outputGain", 0.67f, nullptr);
        legacy.setProperty ("midiVolume", 0.50f, nullptr);
        legacy.setProperty ("midiExpression", 0.80f, nullptr);
        legacy.setProperty (
            "seed",
            static_cast<juce::int64> (1234567),
            nullptr);
        test.expect (
            destination->restoreState (legacy),
            "schema-1 state migrates");
        test.expectEqual (
            destination->activeFamily(),
            ChurchOrganFamily::transistor,
            "legacy family migrates");
        test.expectEqual (
            destination->prepareSpec().quality,
            ChurchOrganLayerQuality::eco,
            "legacy quality migrates");
        test.expectNear (
            destination->parameters().getPlain (
                OrganParameterId::performanceAttackMs),
            0.0,
            0.0,
            "legacy state migrates attack to native articulation");
        test.expectNear (
            destination->parameters().getPlain (
                OrganParameterId::performanceReleaseMs),
            0.0,
            0.0,
            "legacy state migrates release to native articulation");
        for (std::size_t index = 0;
             index < ChurchOrganLayerProcessor::familyCount;
             ++index)
        {
            test.expectNear (
                destination->familyOutputTrim (
                    static_cast<ChurchOrganFamily> (index)),
                0.67,
                1.0e-6,
                "legacy outputGain migrates to family trim");
        }
        test.expectNear (
            destination->diagnostics().midiVolume,
            0.50,
            1.0e-6,
            "legacy MIDI volume migrates");

        destination->setProcessingSuspended (false);
        juce::AudioBuffer<float> audio (2, 512);
        juce::MidiBuffer midi;
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, 60, static_cast<juce::uint8> (120)),
            0);
        test.expect (
            destination->processBlock (audio, midi),
            "restored state resumes rendering");
        test.expect (
            measure (audio).finite && measure (audio).peak > 0.0f,
            "restored state produces finite audio");
    }

    void testTonewheelUnityAndCalibration (TestRunner& test)
    {
        test.section ("Tonewheel unity path and native loudness");
        std::cout << "notes       RMS          peak     adapter/direct max error\n";
        for (const int noteCount :
             std::array<int, 4> {{ 1, 4, 8, 16 }})
        {
            auto throughAdapter = makeProcessor (
                ChurchOrganFamily::tonewheel, 8192);
            auto direct = makeProcessor (
                ChurchOrganFamily::tonewheel, 8192);
            test.expect (
                throughAdapter != nullptr && direct != nullptr,
                "prepare Tonewheel unity pair");
            if (throughAdapter == nullptr || direct == nullptr)
                continue;

            juce::MidiBuffer midi;
            for (int index = 0; index < noteCount; ++index)
            {
                const auto note = 48 + index;
                midi.addEvent (
                    juce::MidiMessage::noteOn (
                        1,
                        note,
                        static_cast<juce::uint8> (112)),
                    0);
                test.expect (
                    direct->tonewheelEngine().queueNoteOn (
                        Manual::upper, note, 112, 0),
                    "direct Tonewheel event accepted");
            }

            juce::AudioBuffer<float> adapterAudio (2, 8192);
            juce::AudioBuffer<float> directAudio (2, 8192);
            juce::MidiBuffer empty;
            test.expect (
                throughAdapter->processBlock (
                    adapterAudio, midi)
                    && direct->processBlock (
                        directAudio, empty),
                "Tonewheel calibration pair renders");
            const auto difference =
                maximumAbsoluteDifference (
                    adapterAudio, directAudio);
            const auto stats = measure (
                adapterAudio, 2048, 6144);
            test.expect (
                difference == 0.0f,
                "adapter adds no duplicate Tonewheel trim");
            test.expect (
                stats.finite && stats.rms > 1.0e-7
                    && stats.peak > 1.0e-6f,
                "Tonewheel calibration output is finite/audible");
            test.expectNear (
                throughAdapter->familyOutputTrim (
                    ChurchOrganFamily::tonewheel),
                1.0,
                0.0,
                "Tonewheel adapter trim defaults to unity");

            std::cout << std::setw (5) << noteCount
                      << "  " << std::fixed << std::setprecision (8)
                      << std::setw (11) << stats.rms
                      << "  " << std::setw (11) << stats.peak
                      << "  " << std::scientific
                      << difference << '\n';
        }
    }

    void testNoAllocationInProcess (TestRunner& test)
    {
        test.section ("zero-allocation audio callback");
        auto processor = makeProcessor (
            ChurchOrganFamily::worshipHybrid, 8192);
        test.expect (processor != nullptr, "prepare RT processor");
        if (processor == nullptr)
            return;

        juce::AudioBuffer<float> audio (2, 8192);
        juce::AudioBuffer<float> send (2, 8192);
        juce::MidiBuffer warmMidi;
        addThreeManualChord (warmMidi, 0);
        test.expect (
            processor->processBlock (audio, warmMidi, &send),
            "RT path warm-up");
        // Force the new coefficients to be published inside the measured
        // callback, proving the handoff itself remains allocation-free.
        processor->setChannelPerformanceEnvelopeSeconds (
            0.72f, 2.4f);

        juce::MidiBuffer midi;
        for (int index = 0; index < 48; ++index)
        {
            const auto channel = 1 + (index % 3);
            const auto note = 36 + (index % 48);
            midi.addEvent (
                juce::MidiMessage::noteOn (
                    channel,
                    note,
                    static_cast<juce::uint8> (
                        70 + (index % 50))),
                100 + index * 127);
            midi.addEvent (
                juce::MidiMessage::noteOff (
                    channel, note),
                110 + index * 127);
        }
        const std::array<juce::uint8, 8> sysex {{
            0x7d, 0x01, 0x02, 0x03,
            0x04, 0x05, 0x06, 0x07
        }};
        midi.addEvent (
            juce::MidiMessage::createSysExMessage (
                sysex.data(),
                static_cast<int> (sysex.size())),
            4090);
        midi.addEvent (
            juce::MidiMessage::controllerEvent (1, 121, 0),
            7000);

        audio.clear();
        send.clear();
        trackedAllocations.store (0, std::memory_order_release);
        trackAllocations.store (true, std::memory_order_release);
        const auto rendered =
            processor->processBlock (audio, midi, &send);
        trackAllocations.store (false, std::memory_order_release);
        const auto allocations =
            trackedAllocations.load (std::memory_order_acquire);

        test.expect (rendered, "maximum RT stress block renders");
        test.expectEqual (
            allocations,
            std::uint64_t { 0 },
            "processBlock performs zero heap allocations");
        test.expect (
            measure (audio).finite && measure (send).finite,
            "RT stress output remains finite");
    }
}

int main()
{
    TestRunner test;
    testPrepareAndContracts (test);
    testEveryFamilyAndChannelMapping (test);
    testChunkingAndSampleAccuracy (test);
    testControllersAndSustain (test);
    testSharedPerformanceEnvelope (test);
    testFamilySwitchAndState (test);
    testTonewheelUnityAndCalibration (test);
    testNoAllocationInProcess (test);
    return test.finish();
}
