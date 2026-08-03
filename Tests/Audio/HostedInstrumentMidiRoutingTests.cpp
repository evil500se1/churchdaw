#include "../../Source/TimbresProcessor.h"

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct CapturedEvent
{
    juce::MidiMessage message;
    int samplePosition = 0;
};

using CapturedBlock = std::vector<CapturedEvent>;

struct PluginLifecycleState
{
    int prepareCalls = 0;
    int releaseCalls = 0;
    int destructorCalls = 0;
    int processCalls = 0;
    int stateCaptureCalls = 0;
    bool throwOnPrepare = false;
    bool throwOnRelease = false;
    bool throwOnGetState = false;
    bool sawPrepareWhileSuspended = false;
    bool sawReleaseWhileSuspended = false;
    std::function<bool()> hostIsSuspended;
    std::function<void()> onRelease;
    std::function<void()> onDestroy;
};

class CapturingPlugin final : public juce::AudioPluginInstance
{
public:
    explicit CapturingPlugin (
        int outputChannels = 2,
        std::shared_ptr<PluginLifecycleState>
            lifecycleState = {},
        juce::PluginDescription descriptionToReport = {})
        : AudioPluginInstance (
              BusesProperties().withOutput (
                  "Output",
                  outputChannels == 2
                      ? juce::AudioChannelSet::stereo()
                      : juce::AudioChannelSet::discreteChannels (
                            outputChannels),
                  true)),
          lifecycle (std::move (lifecycleState)),
          reportedDescription (
              std::move (descriptionToReport))
    {
    }

    ~CapturingPlugin() override
    {
        if (lifecycle == nullptr)
            return;

        ++lifecycle->destructorCalls;

        if (lifecycle->onDestroy != nullptr)
            lifecycle->onDestroy();
    }

    const juce::String getName() const override
    {
        return "ChurchDAW MIDI Capture";
    }

    void prepareToPlay (double sampleRate,
                        int maximumExpectedSamplesPerBlock) override
    {
        ++prepareCount;

        if (lifecycle != nullptr)
        {
            ++lifecycle->prepareCalls;
            lifecycle->sawPrepareWhileSuspended |=
                lifecycle->hostIsSuspended != nullptr
                && lifecycle->hostIsSuspended();

            if (lifecycle->throwOnPrepare)
                throw std::runtime_error (
                    "prepare failure requested by test");
        }

        lastSampleRate = sampleRate;
        lastMaximumBlockSize =
            maximumExpectedSamplesPerBlock;
    }

    void releaseResources() override
    {
        if (lifecycle == nullptr)
            return;

        ++lifecycle->releaseCalls;
        lifecycle->sawReleaseWhileSuspended |=
            lifecycle->hostIsSuspended != nullptr
            && lifecycle->hostIsSuspended();

        if (lifecycle->onRelease != nullptr)
            lifecycle->onRelease();

        if (lifecycle->throwOnRelease)
            throw std::runtime_error (
                "release failure requested by test");
    }

    void processBlock (juce::AudioBuffer<float>& audio,
                       juce::MidiBuffer& midi) override
    {
        if (lifecycle != nullptr)
            ++lifecycle->processCalls;

        CapturedBlock block;
        block.reserve ((size_t) midi.getNumEvents());

        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();
            block.push_back (
                { message, (int) metadata.samplePosition });
            applyToVoiceState (message);
        }

        capturedBlocks.push_back (std::move (block));
        lastProcessChannelCount =
            audio.getNumChannels();
        audio.clear();
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    bool acceptsMidi() const override
    {
        return true;
    }

    bool producesMidi() const override
    {
        return false;
    }

    bool hasEditor() const override
    {
        return false;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram (int) override {}

    const juce::String getProgramName (int) override
    {
        return {};
    }

    void changeProgramName (
        int, const juce::String&) override
    {
    }

    void getStateInformation (
        juce::MemoryBlock&) override
    {
        if (lifecycle == nullptr)
            return;

        ++lifecycle->stateCaptureCalls;

        if (lifecycle->throwOnGetState)
            throw std::runtime_error (
                "state capture failure requested by test");
    }

    void setStateInformation (
        const void* data, int size) override
    {
        restoredState.reset();

        if (data != nullptr && size > 0)
            restoredState.append (data, (size_t) size);
    }

    void fillInPluginDescription (
        juce::PluginDescription& description) const override
    {
        if (reportedDescription.name.isNotEmpty())
        {
            description = reportedDescription;
            return;
        }

        description.name = getName();
        description.descriptiveName = getName();
        description.pluginFormatName = "ChurchDAW Test";
        description.fileOrIdentifier =
            "churchdaw.internal.midi-capture";
        description.manufacturerName = "ChurchDAW";
        description.version = "1";
        description.uniqueId = 0x43444d49;
        description.isInstrument = true;
    }

    int getPrepareCount() const noexcept
    {
        return prepareCount;
    }

    double getLastSampleRate() const noexcept
    {
        return lastSampleRate;
    }

    int getLastMaximumBlockSize() const noexcept
    {
        return lastMaximumBlockSize;
    }

    int getLastProcessChannelCount() const noexcept
    {
        return lastProcessChannelCount;
    }

    const juce::MemoryBlock& getRestoredState() const noexcept
    {
        return restoredState;
    }

    void clearCapturedBlocks()
    {
        capturedBlocks.clear();
    }

    const CapturedBlock& lastBlock() const
    {
        jassert (! capturedBlocks.empty());
        return capturedBlocks.back();
    }

    int activeVoiceCount() const noexcept
    {
        int total = 0;

        for (const auto& channel : activeNotes)
            for (const auto count : channel)
                total += count;

        return total;
    }

    int pitchWheelForChannel (
        int midiChannel) const noexcept
    {
        if (midiChannel < 1 || midiChannel > 16)
            return 0;

        return pitchWheels[(size_t) (midiChannel - 1)];
    }

    int pitchWheelAtLastNoteOnForChannel (
        int midiChannel) const noexcept
    {
        if (midiChannel < 1 || midiChannel > 16)
            return 0;

        return pitchWheelsAtLastNoteOn[
            (size_t) (midiChannel - 1)];
    }

private:
    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    void applyToVoiceState (
        const juce::MidiMessage& message) noexcept
    {
        const int channel =
            juce::jlimit (1, 16, message.getChannel()) - 1;

        if (message.isNoteOn())
        {
            pitchWheelsAtLastNoteOn[
                (size_t) channel] =
                    pitchWheels[(size_t) channel];
            auto& count =
                activeNotes[(size_t) channel]
                           [(size_t) message.getNoteNumber()];
            count = juce::jmin (count + 1, 0x7fff);
        }
        else if (message.isNoteOff())
        {
            auto& count =
                activeNotes[(size_t) channel]
                           [(size_t) message.getNoteNumber()];
            count = juce::jmax (0, count - 1);
        }
        else if (message.isController()
                  && (message.getControllerNumber() == 120
                       || message.getControllerNumber() == 123))
        {
            activeNotes[(size_t) channel].fill (0);
        }
        else if (message.isPitchWheel())
        {
            pitchWheels[(size_t) channel] =
                message.getPitchWheelValue();
        }
    }

    std::vector<CapturedBlock> capturedBlocks;
    std::array<std::array<int, 128>, 16> activeNotes {};
    std::array<int, 16> pitchWheels {
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192
    };
    std::array<int, 16> pitchWheelsAtLastNoteOn {
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192,
        8192, 8192, 8192, 8192
    };
    std::shared_ptr<PluginLifecycleState> lifecycle;
    juce::PluginDescription reportedDescription;
    int prepareCount = 0;
    double lastSampleRate = 0.0;
    int lastMaximumBlockSize = 0;
    int lastProcessChannelCount = 0;
    juce::MemoryBlock restoredState;
};

class DeferredPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName =
        "ChurchDAW Async Test";

    juce::String getName() const override
    {
        return formatName;
    }

    bool fileMightContainThisPluginType (
        const juce::String&) override
    {
        return true;
    }

    juce::FileSearchPath getDefaultLocationsToSearch() override
    {
        return {};
    }

    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }

    void findAllTypesForFile (
        juce::OwnedArray<juce::PluginDescription>&,
        const juce::String&) override
    {
    }

    bool doesPluginStillExist (
        const juce::PluginDescription&) override
    {
        return true;
    }

    juce::String getNameOfPluginFromIdentifier (
        const juce::String& identifier) override
    {
        return identifier;
    }

    bool pluginNeedsRescanning (
        const juce::PluginDescription&) override
    {
        return false;
    }

    juce::StringArray searchPathsForPlugins (
        const juce::FileSearchPath&, bool, bool) override
    {
        return {};
    }

    bool requiresUnblockedMessageThreadDuringCreation (
        const juce::PluginDescription&) const override
    {
        return true;
    }

    int pendingCount() const noexcept
    {
        return (int) pending.size();
    }

    bool complete (
        const juce::String& name,
        std::shared_ptr<PluginLifecycleState> lifecycle)
    {
        for (size_t i = 0; i < pending.size(); ++i)
            if (pending[i].description.name == name)
            {
                auto description = pending[i].description;
                auto callback =
                    std::move (pending[i].callback);
                pending.erase (pending.begin()
                               + (std::ptrdiff_t) i);
                callback (
                    std::make_unique<CapturingPlugin> (
                        2, std::move (lifecycle),
                        std::move (description)),
                    {});
                return true;
            }

        return false;
    }

    bool fail (const juce::String& name)
    {
        for (size_t i = 0; i < pending.size(); ++i)
            if (pending[i].description.name == name)
            {
                auto callback =
                    std::move (pending[i].callback);
                pending.erase (pending.begin()
                               + (std::ptrdiff_t) i);
                callback (nullptr, "falha solicitada pelo teste");
                return true;
            }

        return false;
    }

private:
    struct Pending
    {
        juce::PluginDescription description;
        PluginCreationCallback callback;
    };

    void createPluginInstance (
        const juce::PluginDescription& description,
        double, int,
        PluginCreationCallback callback) override
    {
        pending.push_back (
            { description, std::move (callback) });
    }

    std::vector<Pending> pending;
};

template <typename Parameter>
Parameter* findParameter (
    TimbresProcessor& processor,
    const juce::String& parameterId)
{
    for (auto* parameter : processor.getParameters())
        if (auto* identified =
                dynamic_cast<
                    juce::AudioProcessorParameterWithID*> (
                    parameter);
            identified != nullptr
             && identified->paramID == parameterId)
            return dynamic_cast<Parameter*> (parameter);

    return nullptr;
}

class Fixture
{
public:
    explicit Fixture (bool globalSustainOn)
        : host (std::make_unique<TimbresProcessor>())
    {
        globalSustain =
            findParameter<juce::AudioParameterBool> (
                *host, "gsus");

        if (globalSustain == nullptr)
            return;

        *globalSustain = globalSustainOn;

        auto capture =
            std::make_unique<CapturingPlugin>();
        plugin = capture.get();
        installed = host->installInstrumentForTest (
            layer, std::move (capture));

        if (! installed)
            return;

        host->prepareToPlay (
            sampleRate, maximumBlockSize);
    }

    bool isReady() const noexcept
    {
        return installed
            && host != nullptr
            && plugin != nullptr
            && globalSustain != nullptr;
    }

    const CapturedBlock& process (
        juce::MidiBuffer midi,
        int numSamples = maximumBlockSize)
    {
        plugin->clearCapturedBlocks();
        juce::AudioBuffer<float> audio (
            2, numSamples);
        audio.clear();
        host->processBlock (audio, midi);
        return plugin->lastBlock();
    }

    const CapturedBlock& processEmpty (
        int numSamples = maximumBlockSize)
    {
        juce::MidiBuffer midi;
        return process (std::move (midi), numSamples);
    }

    static constexpr int layer = 0;
    static constexpr double sampleRate = 48000.0;
    static constexpr int maximumBlockSize = 64;

    std::unique_ptr<TimbresProcessor> host;
    CapturingPlugin* plugin = nullptr;
    juce::AudioParameterBool* globalSustain = nullptr;

private:
    bool installed = false;
};

class TestContext
{
public:
    void expect (bool condition,
                 const std::string& description)
    {
        if (condition)
            return;

        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }

    int failureCount() const noexcept
    {
        return failures;
    }

private:
    int failures = 0;
};

int controllerCount (
    const CapturedBlock& block,
    int controller,
    int value = -1,
    int samplePosition = -1)
{
    int count = 0;

    for (const auto& event : block)
        if (event.message.isController()
             && event.message.getControllerNumber()
                    == controller
             && (value < 0
                  || event.message.getControllerValue()
                         == value)
             && (samplePosition < 0
                  || event.samplePosition
                         == samplePosition))
            ++count;

    return count;
}

bool hasControllerOnEveryChannelExactlyOnce (
    const CapturedBlock& block,
    int controller,
    int value,
    int samplePosition)
{
    std::array<int, 16> perChannel {};

    for (const auto& event : block)
        if (event.samplePosition == samplePosition
             && event.message.isController()
             && event.message.getControllerNumber()
                    == controller
             && event.message.getControllerValue()
                    == value)
            ++perChannel[(size_t)
                (event.message.getChannel() - 1)];

    for (const auto count : perChannel)
        if (count != 1)
            return false;

    return true;
}

bool hasNoteOn (
    const CapturedBlock& block,
    int channel,
    int note,
    int samplePosition)
{
    for (const auto& event : block)
        if (event.samplePosition == samplePosition
             && event.message.isNoteOn()
             && event.message.getChannel() == channel
             && event.message.getNoteNumber() == note)
            return true;

    return false;
}

bool hasNoteOff (
    const CapturedBlock& block,
    int channel,
    int note,
    int samplePosition)
{
    for (const auto& event : block)
        if (event.samplePosition == samplePosition
             && event.message.isNoteOff()
             && event.message.getChannel() == channel
             && event.message.getNoteNumber() == note)
            return true;

    return false;
}

int pitchWheelCount (
    const CapturedBlock& block,
    int value,
    int samplePosition,
    int midiChannel = -1)
{
    int count = 0;

    for (const auto& event : block)
        if (event.samplePosition == samplePosition
             && event.message.isPitchWheel()
             && event.message.getPitchWheelValue()
                    == value
             && (midiChannel < 0
                  || event.message.getChannel()
                         == midiChannel))
            ++count;

    return count;
}

bool hasPitchWheelOnEveryChannelExactlyOnce (
    const CapturedBlock& block,
    int value,
    int samplePosition)
{
    std::array<int, 16> perChannel {};

    for (const auto& event : block)
        if (event.samplePosition == samplePosition
             && event.message.isPitchWheel()
             && event.message.getPitchWheelValue()
                    == value)
            ++perChannel[(size_t)
                (event.message.getChannel() - 1)];

    for (const auto count : perChannel)
        if (count != 1)
            return false;

    return true;
}

bool allNotesOffWinsAtSample (
    const CapturedBlock& block,
    int note,
    int samplePosition)
{
    int noteIndex = -1;
    int firstAllOffIndex = -1;
    int allOffCount = 0;

    for (int index = 0;
         index < (int) block.size();
         ++index)
    {
        const auto& event = block[(size_t) index];

        if (event.samplePosition != samplePosition)
            continue;

        if (event.message.isNoteOn()
             && event.message.getNoteNumber() == note)
            noteIndex = index;

        if (event.message.isController()
             && event.message.getControllerNumber() == 123)
        {
            if (firstAllOffIndex < 0)
                firstAllOffIndex = index;

            ++allOffCount;
        }
        else if (firstAllOffIndex >= 0)
        {
            // Nada comum pode aparecer depois que a fronteira all-off
            // começou na mesma amostra.
            return false;
        }
    }

    return noteIndex >= 0
        && firstAllOffIndex > noteIndex
        && allOffCount == 16;
}

bool resetControllersPrecedesPitchCentres (
    const CapturedBlock& block,
    int samplePosition)
{
    int lastResetIndex = -1;
    int firstCentreIndex = -1;
    int resetCount = 0;
    int centreCount = 0;

    for (int index = 0;
         index < (int) block.size();
         ++index)
    {
        const auto& event = block[(size_t) index];

        if (event.samplePosition != samplePosition)
            continue;

        if (event.message.isController()
             && event.message.getControllerNumber() == 121)
        {
            lastResetIndex = index;
            ++resetCount;
        }

        if (event.message.isPitchWheel()
             && event.message.getPitchWheelValue() == 8192)
        {
            if (firstCentreIndex < 0)
                firstCentreIndex = index;

            ++centreCount;
        }
    }

    return resetCount == 16
        && centreCount == 16
        && firstCentreIndex > lastResetIndex;
}

bool overflowBoundaryIsLast (
    const CapturedBlock& block,
    int samplePosition,
    int latePitchValue)
{
    int lastLatePitchIndex = -1;
    int firstCentreIndex = -1;
    int lastCentreIndex = -1;
    int firstAllOffIndex = -1;
    int allSoundOffCount = 0;
    int allNotesOffCount = 0;

    for (int index = 0;
         index < (int) block.size();
         ++index)
    {
        const auto& event = block[(size_t) index];

        if (event.samplePosition != samplePosition)
            continue;

        if (event.message.isPitchWheel())
        {
            if (event.message.getPitchWheelValue()
                    == latePitchValue)
                lastLatePitchIndex = index;

            if (event.message.getPitchWheelValue()
                    == 8192)
            {
                if (firstCentreIndex < 0)
                    firstCentreIndex = index;

                lastCentreIndex = index;
            }
        }

        if (event.message.isController()
             && (event.message.getControllerNumber() == 120
                  || event.message.getControllerNumber() == 123))
        {
            if (firstAllOffIndex < 0)
                firstAllOffIndex = index;

            if (event.message.getControllerNumber() == 120)
                ++allSoundOffCount;
            else
                ++allNotesOffCount;
        }
        else if (firstAllOffIndex >= 0)
        {
            return false;
        }
    }

    return lastLatePitchIndex >= 0
        && firstCentreIndex > lastLatePitchIndex
        && firstAllOffIndex > lastCentreIndex
        && allSoundOffCount == 16
        && allNotesOffCount == 16;
}

bool failClosedBoundaryIsLast (
    const CapturedBlock& block,
    int samplePosition)
{
    int lastCentreIndex = -1;
    int firstAllOffIndex = -1;
    int centreCount = 0;
    int sustainOffCount = 0;
    int allSoundOffCount = 0;
    int allNotesOffCount = 0;

    for (int index = 0;
         index < (int) block.size();
         ++index)
    {
        const auto& event = block[(size_t) index];

        if (event.samplePosition != samplePosition)
            continue;

        if (event.message.isPitchWheel()
             && event.message.getPitchWheelValue()
                    == 8192)
        {
            lastCentreIndex = index;
            ++centreCount;
        }

        if (event.message.isController()
             && event.message.getControllerNumber() == 64
             && event.message.getControllerValue() == 0)
            ++sustainOffCount;

        if (event.message.isController()
             && (event.message.getControllerNumber() == 120
                  || event.message.getControllerNumber() == 123))
        {
            if (firstAllOffIndex < 0)
                firstAllOffIndex = index;

            if (event.message.getControllerNumber() == 120)
                ++allSoundOffCount;
            else
                ++allNotesOffCount;
        }
        else if (firstAllOffIndex >= 0)
        {
            return false;
        }
    }

    return centreCount == 16
        && sustainOffCount == 16
        && firstAllOffIndex > lastCentreIndex
        && allSoundOffCount == 16
        && allNotesOffCount == 16;
}

void testGlobalSustainBeforeInstallAndReprepare (
    TestContext& test)
{
    Fixture fixture (true);
    test.expect (
        fixture.isReady(),
        "fixture de sustain global deve instalar o plugin");

    if (! fixture.isReady())
        return;

    const int preparesBeforeReprepare =
        fixture.plugin->getPrepareCount();
    const auto& first = fixture.processEmpty();
    test.expect (
        preparesBeforeReprepare >= 2
         && fixture.plugin->getLastSampleRate()
                == Fixture::sampleRate
         && fixture.plugin->getLastMaximumBlockSize()
                == Fixture::maximumBlockSize,
        "plugin falso deve receber prepareToPlay real");
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            first, 64, 127, 0),
        "sustain ON anterior ao install/prepare deve chegar "
        "uma vez nos 16 canais");

    fixture.host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);
    const auto& afterReprepare =
        fixture.processEmpty();
    test.expect (
        fixture.plugin->getPrepareCount()
                == preparesBeforeReprepare + 1,
        "reprepare do host deve repreparar o plugin");
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            afterReprepare, 64, 127, 0),
        "sustain global deve ressincronizar os 16 canais "
        "depois de reprepare");
}

void testLateInstalledWidePluginReservesScratch (
    TestContext& test)
{
    auto host =
        std::make_unique<TimbresProcessor>();
    host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);

    auto widePlugin =
        std::make_unique<CapturingPlugin> (16);
    auto* capture = widePlugin.get();
    const bool installed =
        host->installInstrumentForTest (
            Fixture::layer,
            std::move (widePlugin));
    test.expect (
        installed,
        "plugin de 16 saídas instalado depois do prepare deve ser aceito");

    if (! installed)
        return;

    // Este gate é deliberadamente ANTES do primeiro processBlock: setSize no
    // callback não pode descobrir que o plugin novo exige mais canais/storage.
    test.expect (
        capture->getPrepareCount() == 1
         && capture->getLastSampleRate()
                == Fixture::sampleRate
         && capture->getLastMaximumBlockSize()
                == Fixture::maximumBlockSize,
        "install pós-prepare deve preparar o plugin imediatamente");
    test.expect (
        host->getInstrumentScratchChannelsForTest()
                >= 16,
        "install pós-prepare deve reservar ao menos 16 canais antes do áudio");
    test.expect (
        host->getInstrumentScratchSamplesForTest()
                >= 8192,
        "install pós-prepare deve manter reserva de ao menos 8192 samples");

    juce::AudioBuffer<float> audio (
        2, Fixture::maximumBlockSize);
    juce::MidiBuffer midi;
    host->processBlock (audio, midi);
    test.expect (
        capture->getLastProcessChannelCount()
                >= 16,
        "plugin largo deve receber os 16 canais reservados no callback");
}

void testPhysicalSustainFanout (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de CC64 deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer sustain;
    sustain.addEvent (
        juce::MidiMessage::controllerEvent (
            11, 64, 127),
        9);
    const auto& captured =
        fixture.process (std::move (sustain));
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            captured, 64, 127, 9),
        "CC64 de qualquer canal deve ser replicado "
        "exatamente uma vez nos 16 canais");
    test.expect (
        controllerCount (
            captured, 64, 127, 9) == 16,
        "CC64 original não pode virar uma 17a cópia");
}

void testResetControllersPreservesHeldNoteDepth (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de CC121 com nota ativa deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer held;
    held.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 100),
        1);
    (void) fixture.process (
        std::move (held));
    test.expect (
        fixture.plugin->activeVoiceCount() == 1,
        "nota 60 deve estar ativa antes do CC121");

    constexpr int intentionalPitch = 9000;
    juce::MidiBuffer sequence;
    sequence.addEvent (
        juce::MidiMessage::controllerEvent (
            1, 121, 0),
        2);
    sequence.addEvent (
        juce::MidiMessage::pitchWheel (
            1, intentionalPitch),
        3);
    sequence.addEvent (
        juce::MidiMessage::noteOn (
            1, 62, (juce::uint8) 102),
        4);
    sequence.addEvent (
        juce::MidiMessage::noteOff (
            1, 62),
        5);
    sequence.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 106),
        6);
    const auto& afterReset =
        fixture.process (
            std::move (sequence));
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            afterReset, 121, 0, 2)
         && hasPitchWheelOnEveryChannelExactlyOnce (
                afterReset, 8192, 2),
        "CC121 deve ser replicado e centralizar "
        "controladores nos 16 canais");
    test.expect (
        pitchWheelCount (
            afterReset, intentionalPitch,
            3, 1) == 1
         && pitchWheelCount (
                afterReset, 8192,
                6, 1) == 0
         && hasNoteOn (
                afterReset, 1, 64, 6)
         && fixture.plugin
                ->pitchWheelAtLastNoteOnForChannel (
                    1) == intentionalPitch,
        "nota 60 ainda ativa deve impedir centro espúrio "
        "antes da nota 64");
    test.expect (
        fixture.plugin->activeVoiceCount() == 2
         && controllerCount (
                afterReset, 120) == 0
         && controllerCount (
                afterReset, 123) == 0,
        "CC121 não pode encerrar a nota 60 nem "
        "acionar fail-closed");

    juce::MidiBuffer releaseSixty;
    releaseSixty.addEvent (
        juce::MidiMessage::noteOff (
            1, 60),
        7);
    const auto& sixtyReleased =
        fixture.process (
            std::move (releaseSixty));
    test.expect (
        hasNoteOff (
            sixtyReleased, 1, 60, 7)
         && fixture.plugin->activeVoiceCount()
                == 1,
        "NoteOff60 após CC121 deve encerrar exatamente "
        "o tracking da nota 60");

    juce::MidiBuffer releaseSixtyFour;
    releaseSixtyFour.addEvent (
        juce::MidiMessage::noteOff (
            1, 64),
        8);
    (void) fixture.process (
        std::move (releaseSixtyFour));
    test.expect (
        fixture.plugin->activeVoiceCount() == 0,
        "última nota preservada deve encerrar normalmente");

    juce::MidiBuffer nextPhrase;
    nextPhrase.addEvent (
        juce::MidiMessage::noteOn (
            1, 67, (juce::uint8) 108),
        9);
    const auto& centred =
        fixture.process (
            std::move (nextPhrase));
    test.expect (
        pitchWheelCount (
            centred, 8192, 9, 1) == 1
         && fixture.plugin
                ->pitchWheelAtLastNoteOnForChannel (
                    1) == 8192,
        "depois de soltar 60 e 64, a próxima frase "
        "deve recentrar normalmente");

    fixture.host->panic();
    (void) fixture.processEmpty();
}

void testResetControllersAndGlobalReapply (
    TestContext& test)
{
    Fixture fixture (true);
    test.expect (
        fixture.isReady(),
        "fixture de CC121 deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer reset;
    reset.addEvent (
        juce::MidiMessage::controllerEvent (
            7, 121, 0),
        13);
    const auto& captured =
        fixture.process (std::move (reset));
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            captured, 121, 0, 13),
        "CC121 deve ser replicado nos 16 canais");
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            captured, 64, 127, 13),
        "CC121 deve reaplicar o sustain global na mesma amostra");
}

void testGlobalSustainPreservesIntentionalBend (
    TestContext& test)
{
    Fixture fixture (true);
    test.expect (
        fixture.isReady(),
        "fixture de bend com sustain global deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer phrase;
    phrase.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 10100),
        1);
    phrase.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 108),
        2);
    phrase.addEvent (
        juce::MidiMessage::noteOff (
            1, 60),
        3);
    phrase.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 108),
        4);
    const auto& captured =
        fixture.process (std::move (phrase));
    test.expect (
        pitchWheelCount (
            captured, 8192, 4, 1) == 0,
        "gsus ON deve preservar o bend da voz sustentada "
        "ao abrir outra nota");
    test.expect (
        hasNoteOn (captured, 1, 64, 4)
         && fixture.plugin->pitchWheelForChannel (1)
                == 10100,
        "nova nota sob gsus deve chegar ainda no bend intencional");
}

void testCrossChannelPedalPreservesBend (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de pedal cross-channel deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer phrase;
    phrase.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 127),
        0);
    phrase.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 10900),
        1);
    phrase.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 105),
        2);
    phrase.addEvent (
        juce::MidiMessage::noteOff (
            1, 60),
        3);
    phrase.addEvent (
        juce::MidiMessage::noteOn (
            1, 67, (juce::uint8) 105),
        4);
    const auto& captured =
        fixture.process (std::move (phrase));
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            captured, 64, 127, 0),
        "CC64 ch4 deve sustentar globalmente os 16 canais");
    test.expect (
        pitchWheelCount (
            captured, 8192, 4, 1) == 0
         && fixture.plugin->pitchWheelForChannel (1)
                == 10900,
        "CC64 ch4 não pode recentrar o bend legítimo do canal 1");
}

void testCrossChannelResetCentresEveryChannel (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de CC121 cross-channel deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer reset;
    reset.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 11100),
        1);
    reset.addEvent (
        juce::MidiMessage::pitchWheel (
            12, 5300),
        2);
    reset.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 121, 0),
        8);
    reset.addEvent (
        juce::MidiMessage::noteOn (
            1, 69, (juce::uint8) 110),
        9);
    const auto& captured =
        fixture.process (std::move (reset));
    test.expect (
        resetControllersPrecedesPitchCentres (
            captured, 8),
        "CC121 ch4 deve virar 16 resets antes de "
        "16 pitch-centres reais");
    test.expect (
        hasPitchWheelOnEveryChannelExactlyOnce (
            captured, 8192, 8),
        "CC121 deve centralizar todos os 16 canais");
    test.expect (
        fixture.plugin->pitchWheelForChannel (1)
                == 8192
         && fixture.plugin->pitchWheelForChannel (12)
                == 8192
         && hasNoteOn (captured, 1, 69, 9),
        "frase depois de CC121 deve abrir centrada");
}

void runSameSampleAllOffCase (
    TestContext& test,
    bool allOffInsertedFirst)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de ordem all-off deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    constexpr int sample = 17;
    constexpr int note = 62;
    juce::MidiBuffer midi;

    const auto addNote = [&]
    {
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, note, (juce::uint8) 112),
            sample);
    };
    const auto addAllOff = [&]
    {
        midi.addEvent (
            juce::MidiMessage::allNotesOff (4),
            sample);
    };

    if (allOffInsertedFirst)
    {
        addAllOff();
        addNote();
    }
    else
    {
        addNote();
        addAllOff();
    }

    const auto& captured =
        fixture.process (std::move (midi));
    test.expect (
        allNotesOffWinsAtSample (
            captured, note, sample),
        allOffInsertedFirst
            ? "CC123->noteOn na mesma amostra deve ser "
              "reordenado para noteOn->all-off"
            : "noteOn->CC123 na mesma amostra deve manter "
              "all-off por último");
    test.expect (
        fixture.plugin->activeVoiceCount() == 0,
        "all-off na mesma amostra deve matar a nota "
        "no estado real do plugin falso");
}

void testLastSampleAllOffResync (
    TestContext& test)
{
    Fixture fixture (true);
    test.expect (
        fixture.isReady(),
        "fixture de último sample deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    constexpr int lastSample =
        Fixture::maximumBlockSize - 1;
    juce::MidiBuffer midi;
    midi.addEvent (
        juce::MidiMessage::noteOn (
            1, 65, (juce::uint8) 110),
        0);
    midi.addEvent (
        juce::MidiMessage::allNotesOff (9),
        lastSample);
    const auto& boundary =
        fixture.process (std::move (midi));
    test.expect (
        controllerCount (
            boundary, 64, 127,
            lastSample) == 0,
        "all-off no último sample não pode reativar "
        "sustain depois da fronteira");
    test.expect (
        fixture.plugin->activeVoiceCount() == 0,
        "all-off no último sample deve silenciar o plugin");

    const auto& resync = fixture.processEmpty();
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            resync, 64, 127, 0),
        "mirror UNKNOWN no fim do bloco deve "
        "ressincronizar sustain no bloco seguinte");
}

void testPanic (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de PANIC deve instalar o plugin");

    if (! fixture.isReady())
        return;

    (void) fixture.processEmpty();
    juce::MidiBuffer note;
    note.addEvent (
        juce::MidiMessage::noteOn (
            3, 70, (juce::uint8) 120),
        5);
    (void) fixture.process (std::move (note));
    test.expect (
        fixture.plugin->activeVoiceCount() == 1,
        "nota de controle deve estar ativa antes do PANIC");

    fixture.host->panic();
    const auto& panicBlock =
        fixture.processEmpty();
    test.expect (
        controllerCount (
            panicBlock, 120, -1, 0) == 16,
        "PANIC deve entregar CC120 nos 16 canais");
    test.expect (
        controllerCount (
            panicBlock, 123, -1, 0) == 16,
        "PANIC deve entregar CC123 nos 16 canais");
    test.expect (
        fixture.plugin->activeVoiceCount() == 0,
        "PANIC deve zerar o estado de notas do plugin");
}

void testRetriggerDepthAndNextPhraseRecentres (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de retrigger deve instalar o plugin");

    if (! fixture.isReady())
        return;

    auto* transpose =
        findParameter<juce::AudioParameterInt> (
            *fixture.host, "transp0");
    test.expect (
        transpose != nullptr,
        "parâmetro transp0 deve existir no gate de retrigger");

    if (transpose == nullptr)
        return;

    (void) fixture.processEmpty();
    constexpr int bentPitch = 10000;
    constexpr int retriggeredNote = 60;
    constexpr int failClosedSample =
        Fixture::maximumBlockSize - 1;
    *transpose = 0;

    juce::MidiBuffer retrigger;
    retrigger.addEvent (
        juce::MidiMessage::pitchWheel (
            1, bentPitch),
        1);
    retrigger.addEvent (
        juce::MidiMessage::noteOn (
            1, retriggeredNote,
            (juce::uint8) 100),
        2);
    retrigger.addEvent (
        juce::MidiMessage::noteOn (
            1, retriggeredNote,
            (juce::uint8) 105),
        3);
    retrigger.addEvent (
        juce::MidiMessage::noteOff (
            1, retriggeredNote),
        4);
    const auto& retriggered =
        fixture.process (std::move (retrigger));
    test.expect (
        pitchWheelCount (
            retriggered, bentPitch, 1, 1)
                == 1
         && hasControllerOnEveryChannelExactlyOnce (
            retriggered, 64, 0,
            failClosedSample)
         && hasControllerOnEveryChannelExactlyOnce (
                retriggered, 120, 0,
                failClosedSample)
         && hasControllerOnEveryChannelExactlyOnce (
                retriggered, 123, 0,
                failClosedSample)
         && hasPitchWheelOnEveryChannelExactlyOnce (
                retriggered, 8192,
                failClosedSample)
         && failClosedBoundaryIsLast (
                retriggered,
                failClosedSample),
        "2x NoteOn/1x NoteOff ambíguo deve falhar "
        "fechado com sustain off, centros e CC120/123");
    test.expect (
        fixture.plugin->activeVoiceCount() == 0
         && fixture.plugin->pitchWheelForChannel (
                1) == 8192,
        "fail-closed deve zerar vozes, ledger observável "
        "e pitch do plugin");

    // O fail-closed local agenda um PANIC no callback seguinte para reconciliar
    // também o guard global e todos os outros destinos MIDI.
    const auto& reconciliation =
        fixture.processEmpty();
    test.expect (
        controllerCount (
            reconciliation, 120, -1, 0)
                == 16
         && controllerCount (
                reconciliation, 123, -1, 0)
                == 16
         && fixture.plugin->activeVoiceCount()
                == 0
         && fixture.plugin->pitchWheelForChannel (
                1) == 8192,
        "callback seguinte deve reconciliar PANIC "
        "sem deixar nota ou bend presos");

    constexpr int nextTranspose = 7;
    constexpr int nextPhraseOutput =
        retriggeredNote + nextTranspose;
    *transpose = nextTranspose;
    juce::MidiBuffer nextPhrase;
    nextPhrase.addEvent (
        juce::MidiMessage::noteOn (
            1, retriggeredNote,
            (juce::uint8) 110),
        6);
    const auto& centredPhrase =
        fixture.process (
            std::move (nextPhrase));
    test.expect (
        hasNoteOn (
                centredPhrase, 1,
                nextPhraseOutput, 6)
         && fixture.plugin
                ->pitchWheelAtLastNoteOnForChannel (
                    1) == 8192
         && fixture.plugin->pitchWheelForChannel (
                1) == 8192,
        "frase após reconciliação deve começar "
        "centrada e com ledger novo");

    juce::MidiBuffer nextRelease;
    nextRelease.addEvent (
        juce::MidiMessage::noteOff (
            1, retriggeredNote),
        7);
    const auto& released =
        fixture.process (
            std::move (nextRelease));
    test.expect (
        hasNoteOff (
            released, 1,
            nextPhraseOutput, 7)
         && fixture.plugin->activeVoiceCount()
                == 0,
        "NoteOff da nova frase deve usar somente "
        "a transposição nova e não deixar nota presa");
}

void testHotLoadPitchCentresStayOnTargetLayer (
    TestContext& test)
{
    auto host =
        std::make_unique<TimbresProcessor>();
    host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);

    auto layerB =
        std::make_unique<CapturingPlugin>();
    auto* captureB = layerB.get();
    const bool installedB =
        host->installInstrumentForTest (
            1, std::move (layerB));
    test.expect (
        installedB,
        "plugin da camada B deve ser instalado");

    if (! installedB)
        return;

    auto processHost =
        [&] (juce::MidiBuffer midi)
    {
        juce::AudioBuffer<float> audio (
            2, Fixture::maximumBlockSize);
        audio.clear();
        host->processBlock (audio, midi);
    };

    // Consome os centros de prepare/hot-load da camada B.
    {
        juce::MidiBuffer empty;
        processHost (std::move (empty));
    }

    constexpr int intentionalPitch = 10100;
    captureB->clearCapturedBlocks();
    juce::MidiBuffer bend;
    bend.addEvent (
        juce::MidiMessage::pitchWheel (
            2, intentionalPitch),
        3);
    processHost (std::move (bend));
    test.expect (
        captureB->pitchWheelForChannel (2)
                == intentionalPitch,
        "camada B deve manter o pitch intencional "
        "antes do hot-load em A");

    captureB->clearCapturedBlocks();
    auto layerA =
        std::make_unique<CapturingPlugin>();
    auto* captureA = layerA.get();
    const bool installedA =
        host->installInstrumentForTest (
            0, std::move (layerA));
    test.expect (
        installedA,
        "plugin da camada A deve ser instalado em hot-load");

    if (! installedA)
        return;

    juce::MidiBuffer empty;
    processHost (std::move (empty));
    test.expect (
        hasPitchWheelOnEveryChannelExactlyOnce (
            captureA->lastBlock(), 8192, 0),
        "hot-load deve centralizar somente o plugin novo da camada A");
    test.expect (
        pitchWheelCount (
            captureB->lastBlock(),
            8192, 0) == 0
         && captureB->pitchWheelForChannel (
                2) == intentionalPitch,
        "hot-load em A não pode injetar centros nem "
        "apagar o pitch ativo da camada B");
}

void testPluginLifecycleAndFailureContainment (
    TestContext& test)
{
    auto host =
        std::make_unique<TimbresProcessor>();
    host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);

    auto firstState =
        std::make_shared<PluginLifecycleState>();
    firstState->hostIsSuspended =
        [processor = host.get()]
        {
            return processor->isSuspended();
        };
    auto first =
        std::make_unique<CapturingPlugin> (
            2, firstState);
    auto* firstCapture = first.get();
    const bool firstInstalled =
        host->installInstrumentForTest (
            0, std::move (first));
    test.expect (
        firstInstalled
         && firstState->prepareCalls == 1
         && firstState->sawPrepareWhileSuspended
         && ! host->isSuspended(),
        "install deve preparar sob suspensão e "
        "sempre retomar o host");

    if (! firstInstalled)
        return;

    host->releaseResources();
    test.expect (
        firstState->releaseCalls == 1
         && firstState->sawReleaseWhileSuspended
         && ! host->isSuspended(),
        "release do host deve chegar uma vez ao plugin "
        "sob suspensão e retomar processamento");
    host->releaseResources();
    test.expect (
        firstState->releaseCalls == 1
         && ! host->isSuspended(),
        "release repetido não deve duplicar lifecycle "
        "nem deixar o host suspenso");

    host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);
    test.expect (
        firstState->prepareCalls == 2
         && ! host->isSuspended(),
        "reprepare após release deve preparar o plugin novamente");

    auto failingState =
        std::make_shared<PluginLifecycleState>();
    failingState->throwOnPrepare = true;
    failingState->hostIsSuspended =
        [processor = host.get()]
        {
            return processor->isSuspended();
        };
    auto failing =
        std::make_unique<CapturingPlugin> (
            2, failingState);
    const bool failedInstall =
        host->installInstrumentForTest (
            0, std::move (failing));
    test.expect (
        ! failedInstall
         && failingState->prepareCalls == 1
         && failingState->releaseCalls == 1
         && failingState->sawPrepareWhileSuspended
         && failingState->sawReleaseWhileSuspended
         && host->getLayerInstrument (0)
                == firstCapture
         && ! host->isSuspended(),
        "throw em prepare deve liberar candidato, "
        "preservar plugin anterior e restaurar suspensão");

    const int processesBefore =
        firstState->processCalls;
    {
        juce::AudioBuffer<float> audio (
            2, Fixture::maximumBlockSize);
        audio.clear();
        juce::MidiBuffer empty;
        host->processBlock (audio, empty);
    }
    test.expect (
        firstState->processCalls
                == processesBefore + 1,
        "plugin anterior deve continuar processando "
        "depois de candidato falhar");

    firstState->throwOnRelease = true;
    auto replacementState =
        std::make_shared<PluginLifecycleState>();
    replacementState->hostIsSuspended =
        [processor = host.get()]
        {
            return processor->isSuspended();
        };
    auto replacement =
        std::make_unique<CapturingPlugin> (
            2, replacementState);
    const bool replacementInstalled =
        host->installInstrumentForTest (
            0, std::move (replacement));
    test.expect (
        replacementInstalled
         && replacementState->prepareCalls == 1
         && firstState->releaseCalls == 2
         && ! host->isSuspended(),
        "throw no release do plugin substituído não pode "
        "abortar hot-swap nem prender suspensão");

    if (! replacementInstalled)
        return;

    replacementState->throwOnRelease = true;
    host->releaseResources();
    test.expect (
        replacementState->releaseCalls == 1
         && replacementState->sawReleaseWhileSuspended
         && ! host->isSuspended(),
        "throw em releaseResources deve ser contido "
        "e o host deve retomar");
    host->releaseResources();
    test.expect (
        replacementState->releaseCalls == 1
         && ! host->isSuspended(),
        "release que lançou não pode ser reenviado "
        "sem novo prepare");

    replacementState->throwOnRelease = false;
    host->prepareToPlay (
        Fixture::sampleRate,
        Fixture::maximumBlockSize);
    test.expect (
        replacementState->prepareCalls == 2
         && ! host->isSuspended(),
        "plugin deve poder ser repreparado após release contido");

    // Operações internas não possuem autorização para desfazer uma suspensão
    // que já pertencia ao host/chamador.
    host->suspendProcessing (true);
    test.expect (
        host->isSuspended(),
        "pré-condição da suspensão aninhada deve estar ativa");

    auto nestedState =
        std::make_shared<PluginLifecycleState>();
    nestedState->hostIsSuspended =
        [processor = host.get()]
        {
            return processor->isSuspended();
        };
    auto nested =
        std::make_unique<CapturingPlugin> (
            2, nestedState);
    const bool nestedInstalled =
        host->installInstrumentForTest (
            0, std::move (nested));
    test.expect (
        nestedInstalled
         && nestedState->prepareCalls == 1
         && nestedState->sawPrepareWhileSuspended
         && replacementState->releaseCalls == 2
         && host->isSuspended(),
        "hot-swap sob suspensão externa deve preparar/liberar "
        "sem retomar o host");

    host->panic();
    test.expect (
        host->isSuspended(),
        "PANIC não pode desfazer suspensão externa");

    host->releaseResources();
    test.expect (
        host->isSuspended()
         && (! nestedInstalled
              || (nestedState->releaseCalls == 1
                  && nestedState
                         ->sawReleaseWhileSuspended)),
        "releaseResources aninhado deve manter suspensão "
        "e executar callback ainda suspenso");

    // Somente o dono original da suspensão a encerra.
    host->suspendProcessing (false);
    test.expect (
        ! host->isSuspended(),
        "teste deve liberar explicitamente a suspensão externa");
}

void testTransposeLedgerOverflowFailsClosed (
    TestContext& test)
{
    Fixture fixture (false);
    test.expect (
        fixture.isReady(),
        "fixture de overflow do ledger deve instalar o plugin");

    if (! fixture.isReady())
        return;

    auto* transpose =
        findParameter<juce::AudioParameterInt> (
            *fixture.host, "transp0");
    test.expect (
        transpose != nullptr,
        "parâmetro transp0 deve existir");

    if (transpose == nullptr)
        return;

    (void) fixture.processEmpty();
    constexpr int inputNote = 60;

    juce::MidiBuffer bentSustain;
    bentSustain.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 127),
        0);
    bentSustain.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 11400),
        1);
    (void) fixture.process (
        std::move (bentSustain));

    for (int overlap = 0;
         overlap < 8;
         ++overlap)
    {
        *transpose = overlap;
        juce::MidiBuffer midi;
        midi.addEvent (
            juce::MidiMessage::noteOn (
                1, inputNote,
                (juce::uint8) 100),
            overlap);
        const auto& accepted =
            fixture.process (std::move (midi));
        test.expect (
            hasNoteOn (
                accepted, 1,
                inputNote + overlap,
                overlap),
            "os oito slots fixos do ledger devem "
            "preservar a transposição do note-on");
    }

    constexpr int overflowSample = 23;
    constexpr int followingSample = overflowSample + 1;
    constexpr int latePitchValue = 12200;
    constexpr int followingInputNote = 72;
    constexpr int followingOutputNote =
        followingInputNote + 8;

    *transpose = 8;
    juce::MidiBuffer adversarial;
    adversarial.addEvent (
        juce::MidiMessage::noteOn (
            1, inputNote,
            (juce::uint8) 100),
        overflowSample);
    adversarial.addEvent (
        juce::MidiMessage::pitchWheel (
            1, latePitchValue),
        overflowSample);
    adversarial.addEvent (
        juce::MidiMessage::noteOn (
            1, followingInputNote,
            (juce::uint8) 105),
        followingSample);
    const auto& overflow =
        fixture.process (
            std::move (adversarial));
    test.expect (
        ! hasNoteOn (
            overflow, 1, inputNote + 8,
            overflowSample),
        "9º retrigger não pode chegar órfão ao plugin");
    test.expect (
        pitchWheelCount (
            overflow, latePitchValue,
            overflowSample, 1) == 1,
        "pitch bend posterior ao 9º note-on deve "
        "permanecer antes da fronteira de overflow");
    test.expect (
        hasControllerOnEveryChannelExactlyOnce (
            overflow, 123, 0,
            overflowSample),
        "overflow deve falhar fechado com all-off global");
    test.expect (
        hasPitchWheelOnEveryChannelExactlyOnce (
            overflow, 8192,
            overflowSample)
         && overflowBoundaryIsLast (
                overflow, overflowSample,
                latePitchValue),
        "overflow deve ordenar pitch tardio, centros "
        "e então CC120/123 por último");
    test.expect (
        hasNoteOn (
            overflow, 1,
            followingOutputNote,
            followingSample)
         && fixture.plugin
                ->pitchWheelAtLastNoteOnForChannel (
                    1) == 8192
         && fixture.plugin
                ->pitchWheelForChannel (1)
                    == 8192,
        "nota diferente na amostra seguinte do mesmo "
        "callback deve chegar já centralizada");
    test.expect (
        fixture.plugin->activeVoiceCount() == 1,
        "overflow deve eliminar as oito vozes antigas "
        "e preservar somente a nota da amostra seguinte");

    // O overflow agenda o PANIC completo no callback seguinte para que o
    // estado global do guard e dos outros canais também seja refeito.
    const auto& panicRecovery =
        fixture.processEmpty();
    test.expect (
        controllerCount (
            panicRecovery, 120, -1, 0) == 16
         && controllerCount (
                panicRecovery, 123, -1, 0) == 16
         && fixture.plugin->pitchWheelForChannel (1)
                == 8192
         && fixture.plugin->activeVoiceCount() == 0,
        "callback após overflow deve entregar PANIC centrado");

    *transpose = 0;
    juce::MidiBuffer recoveredNote;
    recoveredNote.addEvent (
        juce::MidiMessage::noteOn (
            1, 72, (juce::uint8) 105),
        5);
    const auto& recovered =
        fixture.process (
            std::move (recoveredNote));
    test.expect (
        hasNoteOn (recovered, 1, 72, 5)
         && fixture.plugin->pitchWheelForChannel (1)
                == 8192,
        "nova frase depois do overflow/PANIC deve abrir centrada");

    fixture.host->panic();
    (void) fixture.processEmpty();
    test.expect (
        fixture.plugin->activeVoiceCount() == 0,
        "PANIC explícito após recuperação deve silenciar a nova nota");
}

void testAsyncPluginCreationIsTwoPhaseAndRejectsStale (
    TestContext& test)
{
    juce::AudioPluginFormatManager manager;
    auto deferred = std::make_unique<DeferredPluginFormat>();
    auto* deferredRaw = deferred.get();
    manager.addFormat (std::move (deferred));
    juce::KnownPluginList known;
    auto processor = std::make_unique<TimbresProcessor>();
    processor->setPluginResources (&known, &manager);
    processor->prepareToPlay (48000.0, 128);

    const auto makeDescription = [] (
        const juce::String& name, int uniqueId)
    {
        juce::PluginDescription description;
        description.name = name;
        description.descriptiveName = name;
        description.pluginFormatName =
            DeferredPluginFormat::formatName;
        description.fileOrIdentifier =
            "churchdaw.async." + name;
        description.manufacturerName = "ChurchDAW Tests";
        description.version = "1";
        description.uniqueId = uniqueId;
        description.isInstrument = true;
        return description;
    };

    const auto dispatchUntilPending =
        [deferredRaw] (int expected)
    {
        for (int attempt = 0;
             attempt < 100
                && deferredRaw->pendingCount() < expected;
             ++attempt)
            juce::MessageManager::getInstance()
                ->runDispatchLoopUntil (2);

        return deferredRaw->pendingCount() == expected;
    };

    const auto dispatchUntil = [] (const std::function<bool()>& ready)
    {
        for (int attempt = 0; attempt < 100 && ! ready(); ++attempt)
            juce::MessageManager::getInstance()
                ->runDispatchLoopUntil (2);

        return ready();
    };

    const auto saveProcessorXml = [&processor]
    {
        juce::MemoryBlock state;
        processor->getStateInformation (state);
        return juce::AudioProcessor::getXmlFromBinary (
            state.getData(), (int) state.getSize());
    };

    const auto findLayer = [] (
        const juce::XmlElement* root, int layer)
        -> const juce::XmlElement*
    {
        if (root != nullptr)
            for (const auto* child : root->getChildIterator())
                if (child->hasTagName ("LAYER")
                     && child->getIntAttribute ("index", -1) == layer)
                    return child;

        return nullptr;
    };

    const auto findInsert = [] (
        const juce::XmlElement* root, int slot)
        -> const juce::XmlElement*
    {
        if (root != nullptr)
            for (const auto* child : root->getChildIterator())
                if (child->hasTagName ("INSERT")
                     && child->getIntAttribute ("slot", -1) == slot)
                    return child;

        return nullptr;
    };

    const auto first = makeDescription ("Async A", 0x4101);
    auto lifecycleA =
        std::make_shared<PluginLifecycleState>();
    int firstCompletion = 0;
    test.expect (
        processor->loadInstrumentPlugin (
            0, first, true, nullptr,
            [&firstCompletion] (bool loaded, const juce::String&)
            {
                firstCompletion += loaded ? 1 : -1;
            }),
        "pedido assíncrono inicial deve ser aceito");
    test.expect (
        processor->getLayerInstrument (0) == nullptr,
        "pedido aceito não pode instalar antes do callback");
    test.expect (
        dispatchUntilPending (1),
        "formato mock deve receber pedido sem bloquear a message thread");
    test.expect (
        deferredRaw->complete ("Async A", lifecycleA),
        "callback inicial deve existir");
    test.expect (
        processor->getLayerInstrumentName (0) == "Async A"
         && firstCompletion == 1
         && lifecycleA->prepareCalls == 1,
        "primeiro callback deve preparar e instalar o instrumento");

    const auto instrumentStateCallsBeforeFailure =
        lifecycleA->stateCaptureCalls;
    lifecycleA->throwOnGetState = true;
    auto failedInstrumentCapture = saveProcessorXml();
    const auto* failedInstrumentLayer = findLayer (
        failedInstrumentCapture.get(), 0);
    test.expect (
        failedInstrumentCapture != nullptr
         && failedInstrumentLayer != nullptr
         && failedInstrumentLayer->getStringAttribute ("vstId")
                == first.createIdentifierString()
         && lifecycleA->stateCaptureCalls
                == instrumentStateCallsBeforeFailure + 1
         && processor->consumeLastStateCaptureHadFailure(),
        "throw do VST/AU deve sinalizar que o ID no snapshot nao e "
        "uma captura valida silenciosa");
    test.expect (
        ! processor->consumeLastStateCaptureHadFailure(),
        "sinal de falha da captura deve ser consumivel uma unica vez");

    lifecycleA->throwOnGetState = false;
    auto recoveredInstrumentCapture = saveProcessorXml();
    test.expect (
        recoveredInstrumentCapture != nullptr
         && lifecycleA->stateCaptureCalls
                == instrumentStateCallsBeforeFailure + 2
         && ! processor->consumeLastStateCaptureHadFailure(),
        "nova captura valida deve limpar o resultado da falha anterior");

    const auto stale = makeDescription ("Async B", 0x4201);
    const auto current = makeDescription ("Async C", 0x4301);
    const char staleBytes[] = "estado-stale";
    const char currentBytes[] = "estado-correto";
    juce::MemoryBlock staleState (
        staleBytes, sizeof (staleBytes));
    juce::MemoryBlock currentState (
        currentBytes, sizeof (currentBytes));
    auto lifecycleB =
        std::make_shared<PluginLifecycleState>();
    auto lifecycleC =
        std::make_shared<PluginLifecycleState>();
    int staleCompletion = 0;
    int currentCompletion = 0;

    test.expect (
        processor->loadInstrumentPlugin (
            0, stale, false, &staleState,
            [&staleCompletion] (bool, const juce::String&)
            {
                ++staleCompletion;
            })
         && processor->loadInstrumentPlugin (
                0, current, false, &currentState,
                [&currentCompletion] (
                    bool loaded, const juce::String&)
                {
                    currentCompletion += loaded ? 1 : -1;
                }),
        "rajada B/C deve ser aceita sem remover A");
    test.expect (
        processor->getLayerInstrumentName (0) == "Async A"
         && lifecycleA->releaseCalls == 0,
        "instrumento A deve continuar tocando enquanto B/C criam");

    juce::PluginDescription pendingInstrumentDescription;
    auto savedDuringInstrumentLoad = saveProcessorXml();
    const auto* savedPendingLayer = findLayer (
        savedDuringInstrumentLoad.get(), 0);
    test.expect (
        known.getNumTypes() == 0
         && processor->getPendingInstrumentDescription (
                0, pendingInstrumentDescription)
         && pendingInstrumentDescription.createIdentifierString()
                == current.createIdentifierString()
         && savedPendingLayer != nullptr
         && savedPendingLayer->getStringAttribute ("vstId")
                == current.createIdentifierString()
         && savedPendingLayer->getStringAttribute ("vstState")
                == currentState.toBase64Encoding(),
        "autosave/cache devem expor C pendente mesmo com KnownPluginList vazio");
    test.expect (
        dispatchUntilPending (2),
        "os dois pedidos devem chegar ao formato assíncrono");
    test.expect (
        deferredRaw->complete ("Async B", lifecycleB),
        "callback B deve existir");
    test.expect (
        processor->getLayerInstrumentName (0) == "Async A"
         && lifecycleB->prepareCalls == 0
         && lifecycleA->releaseCalls == 0
         && staleCompletion == 0,
        "callback stale deve ser ignorado antes de prepare/swap/completion");
    test.expect (
        deferredRaw->complete ("Async C", lifecycleC),
        "callback C deve existir");

    auto* installed = dynamic_cast<CapturingPlugin*> (
        processor->getLayerInstrument (0));
    test.expect (
        installed != nullptr
         && processor->getLayerInstrumentName (0) == "Async C"
         && lifecycleC->prepareCalls == 1
         && lifecycleA->releaseCalls == 1
         && currentCompletion == 1,
        "callback atual deve preparar C e fazer swap em duas fases");
    test.expect (
        installed != nullptr
         && installed->getRestoredState() == currentState,
        "estado pendente deve ser aplicado a C antes do commit");

    const auto failedInstrument =
        makeDescription ("Async Failure", 0x4401);
    int instrumentFailureCompletion = 0;
    test.expect (
        processor->loadInstrumentPlugin (
            0, failedInstrument, true, nullptr,
            [&instrumentFailureCompletion] (
                bool loaded, const juce::String&)
            {
                instrumentFailureCompletion += loaded ? 1 : -1;
            })
         && dispatchUntilPending (1)
         && deferredRaw->fail ("Async Failure"),
        "falha assíncrona de instrumento deve ser entregue");
    test.expect (
        processor->getLayerInstrument (0) == installed
         && lifecycleC->releaseCalls == 0
         && instrumentFailureCompletion == -1,
        "falha de criação deve preservar C sem liberar seu áudio");

    auto savedAfterInteractiveFailure = saveProcessorXml();
    const auto* layerAfterInteractiveFailure = findLayer (
        savedAfterInteractiveFailure.get(), 0);
    juce::PluginDescription clearedPendingDescription;
    test.expect (
        ! processor->getPendingInstrumentDescription (
            0, clearedPendingDescription)
         && layerAfterInteractiveFailure != nullptr
         && layerAfterInteractiveFailure->getStringAttribute ("vstId")
                == current.createIdentifierString(),
        "falha interativa deve limpar pending e voltar a serializar C instalado");

    // C -> E faz o commit; releaseResources de C inicia D reentrante. O fim de
    // E não pode limpar D nem chamar a completion antiga. O teste também prova
    // que o editor recebe o ponteiro ainda vivo antes de release/destruição.
    const auto reentrantInstrument =
        makeDescription ("Async D Reentrant", 0x4501);
    const auto transientInstrument =
        makeDescription ("Async E Transient", 0x4601);
    auto lifecycleD =
        std::make_shared<PluginLifecycleState>();
    auto lifecycleE =
        std::make_shared<PluginLifecycleState>();
    bool retirementSawLiveC = false;
    bool reentrantInstrumentAccepted = false;
    int transientCompletion = 0;
    int reentrantCompletion = 0;

    processor->setHostedPluginWillRetireCallback (
        [&] (TimbresProcessor::HostedPluginLocation location,
             int index, juce::AudioPluginInstance* instance)
        {
            if (location
                    == TimbresProcessor::HostedPluginLocation::instrument
                 && index == 0 && instance == installed)
                retirementSawLiveC = lifecycleC->releaseCalls == 0
                                  && lifecycleC->destructorCalls == 0;
        });
    lifecycleC->onRelease = [&]
    {
        reentrantInstrumentAccepted =
            processor->loadInstrumentPlugin (
                0, reentrantInstrument, true, nullptr,
                [&reentrantCompletion] (
                    bool loaded, const juce::String&)
                {
                    reentrantCompletion += loaded ? 1 : -1;
                });
    };

    test.expect (
        processor->loadInstrumentPlugin (
            0, transientInstrument, true, nullptr,
            [&transientCompletion] (
                bool loaded, const juce::String&)
            {
                transientCompletion += loaded ? 1 : -1;
            })
         && dispatchUntilPending (1)
         && deferredRaw->complete (
                "Async E Transient", lifecycleE),
        "E deve concluir enquanto C ainda é o instrumento antigo");
    lifecycleC->onRelease = {};
    test.expect (
        retirementSawLiveC
         && reentrantInstrumentAccepted
         && lifecycleC->releaseCalls == 1
         && lifecycleC->destructorCalls == 1
         && transientCompletion == 0
         && processor->getLayerInstrumentName (0)
                == "Async E Transient",
        "retire deve preceder release e D deve tornar a completion de E stale");

    auto savedDuringReentrantInstrument = saveProcessorXml();
    const auto* savedReentrantLayer = findLayer (
        savedDuringReentrantInstrument.get(), 0);
    test.expect (
        savedReentrantLayer != nullptr
         && savedReentrantLayer->getStringAttribute ("vstId")
                == reentrantInstrument.createIdentifierString()
         && dispatchUntilPending (1)
         && deferredRaw->complete (
                "Async D Reentrant", lifecycleD),
        "pedido D iniciado no release deve permanecer pendente e concluir");
    test.expect (
        processor->getLayerInstrumentName (0)
                == "Async D Reentrant"
         && lifecycleE->releaseCalls == 1
         && lifecycleE->destructorCalls == 1
         && reentrantCompletion == 1,
        "D deve substituir E sem perder identidade/completion");

    // Um editor de vendor ainda desmontando bloqueia o commit, mas não perde a
    // instância criada. Depois do gate, D lança em release e mesmo assim G deve
    // ficar instalado e reportar sucesso (nunca falso pós-move).
    const auto gatedInstrument =
        makeDescription ("Async G Gated", 0x4701);
    auto lifecycleG =
        std::make_shared<PluginLifecycleState>();
    int gatedCompletion = 0;
    lifecycleD->throwOnRelease = true;
    test.expect (
        processor->loadInstrumentPlugin (
            0, gatedInstrument, true, nullptr,
            [&gatedCompletion] (bool loaded, const juce::String&)
            {
                gatedCompletion += loaded ? 1 : -1;
            })
         && dispatchUntilPending (1),
        "G deve chegar ao formato antes do gate de teardown");
    processor->beginHostedPluginEditorTeardown();
    test.expect (
        deferredRaw->complete ("Async G Gated", lifecycleG)
         && lifecycleG->prepareCalls == 0
         && processor->getLayerInstrumentName (0)
                == "Async D Reentrant",
        "completion durante teardown não pode preparar/trocar sob o editor");
    processor->endHostedPluginEditorTeardown();
    test.expect (
        dispatchUntil ([&]
        {
            return processor->getLayerInstrumentName (0)
                    == "Async G Gated";
        })
         && lifecycleG->prepareCalls == 1
         && lifecycleD->releaseCalls == 1
         && lifecycleD->destructorCalls == 1
         && gatedCompletion == 1,
        "G deve instalar após teardown apesar da exceção no release de D");

    const auto restoreFailure =
        makeDescription ("Async Restore Failure", 0x4801);
    const auto interactiveFailureAfterRestore =
        makeDescription ("Async Interactive Failure 2", 0x4901);
    const char restoreFailureBytes[] = "estado-restore-pendente";
    juce::MemoryBlock restoreFailureState (
        restoreFailureBytes, sizeof (restoreFailureBytes));
    int restoreFailureCompletion = 0;
    test.expect (
        processor->loadInstrumentPlugin (
            0, restoreFailure, false, &restoreFailureState,
            [&restoreFailureCompletion] (
                bool loaded, const juce::String&)
            {
                restoreFailureCompletion += loaded ? 1 : -1;
            })
         && dispatchUntilPending (1)
         && deferredRaw->fail ("Async Restore Failure"),
        "falha de restore deve ser entregue sem apagar a intenção");
    auto savedAfterRestoreFailure = saveProcessorXml();
    const auto* layerAfterRestoreFailure = findLayer (
        savedAfterRestoreFailure.get(), 0);
    juce::PluginDescription preservedRestoreDescription;
    test.expect (
        restoreFailureCompletion == -1
         && processor->getPendingInstrumentDescription (
                0, preservedRestoreDescription)
         && preservedRestoreDescription.createIdentifierString()
                == restoreFailure.createIdentifierString()
         && layerAfterRestoreFailure != nullptr
         && layerAfterRestoreFailure->getStringAttribute ("vstId")
                == restoreFailure.createIdentifierString()
         && layerAfterRestoreFailure->getStringAttribute ("vstState")
                == restoreFailureState.toBase64Encoding(),
        "restore falho deve sobreviver no save/cache para autocura");

    test.expect (
        processor->loadInstrumentPlugin (
            0, interactiveFailureAfterRestore)
         && dispatchUntilPending (1)
         && deferredRaw->fail (
                "Async Interactive Failure 2"),
        "novo pedido interativo deve substituir o restore pendente");
    auto savedAfterSecondInteractiveFailure = saveProcessorXml();
    const auto* layerAfterSecondInteractiveFailure = findLayer (
        savedAfterSecondInteractiveFailure.get(), 0);
    test.expect (
        ! processor->getPendingInstrumentDescription (
            0, clearedPendingDescription)
         && layerAfterSecondInteractiveFailure != nullptr
         && layerAfterSecondInteractiveFailure->getStringAttribute ("vstId")
                == gatedInstrument.createIdentifierString(),
        "falha interativa posterior deve retornar ao G instalado");

    auto insertA = makeDescription ("Insert A", 0x5101);
    auto insertB = makeDescription ("Insert B", 0x5201);
    auto insertC = makeDescription ("Insert C", 0x5301);
    insertA.isInstrument = false;
    insertB.isInstrument = false;
    insertC.isInstrument = false;
    auto insertLifecycleA =
        std::make_shared<PluginLifecycleState>();
    auto insertLifecycleB =
        std::make_shared<PluginLifecycleState>();
    auto insertLifecycleC =
        std::make_shared<PluginLifecycleState>();

    test.expect (
        processor->loadInsert (0, insertA),
        "pedido do insert A deve ser aceito");
    test.expect (
        dispatchUntilPending (1)
         && deferredRaw->complete (
                "Insert A", insertLifecycleA),
        "insert A deve concluir pelo callback assíncrono");
    auto* firstInsert = processor->getInsert (0);
    test.expect (
        firstInsert != nullptr
         && insertLifecycleA->prepareCalls == 1,
        "insert A deve estar preparado e instalado");

    const auto insertStateCallsBeforeFailure =
        insertLifecycleA->stateCaptureCalls;
    insertLifecycleA->throwOnGetState = true;
    auto failedInsertCapture = saveProcessorXml();
    const auto* failedInsertXml = findInsert (
        failedInsertCapture.get(), 0);
    test.expect (
        failedInsertCapture != nullptr
         && failedInsertXml != nullptr
         && failedInsertXml->getStringAttribute ("id")
                == insertA.createIdentifierString()
         && insertLifecycleA->stateCaptureCalls
                == insertStateCallsBeforeFailure + 1
         && processor->consumeLastStateCaptureHadFailure(),
        "throw do insert deve sinalizar que o ID no snapshot nao e "
        "uma captura valida silenciosa");

    insertLifecycleA->throwOnGetState = false;
    auto recoveredInsertCapture = saveProcessorXml();
    test.expect (
        recoveredInsertCapture != nullptr
         && insertLifecycleA->stateCaptureCalls
                == insertStateCallsBeforeFailure + 2
         && ! processor->consumeLastStateCaptureHadFailure(),
        "captura valida posterior do insert deve limpar a falha anterior");

    test.expect (
        processor->loadInsert (
            0, insertB, &staleState)
         && processor->loadInsert (
                0, insertC, &currentState),
        "rajada de inserts B/C deve ser aceita");
    test.expect (
        processor->getInsert (0) == firstInsert
         && insertLifecycleA->releaseCalls == 0,
        "insert A deve permanecer no grafo durante criação B/C");

    juce::MemoryBlock savedDuringInsertLoad;
    processor->getStateInformation (savedDuringInsertLoad);
    auto savedXml = juce::AudioProcessor::getXmlFromBinary (
        savedDuringInsertLoad.getData(),
        (int) savedDuringInsertLoad.getSize());
    const juce::XmlElement* savedInsert = nullptr;

    if (savedXml != nullptr)
        for (const auto* child : savedXml->getChildIterator())
            if (child->hasTagName ("INSERT")
                 && child->getIntAttribute ("slot", -1) == 0)
            {
                savedInsert = child;
                break;
            }

    test.expect (
        savedInsert != nullptr
         && savedInsert->getStringAttribute ("id")
                == insertC.createIdentifierString()
         && savedInsert->getStringAttribute ("state")
                == currentState.toBase64Encoding(),
        "save durante two-phase deve persistir C pendente, não A antigo");
    test.expect (
        dispatchUntilPending (2)
         && deferredRaw->complete (
                "Insert B", insertLifecycleB),
        "callback do insert B deve existir");
    test.expect (
        processor->getInsert (0) == firstInsert
         && insertLifecycleB->prepareCalls == 0
         && insertLifecycleA->releaseCalls == 0,
        "insert stale B deve ser descartado antes do prepare/swap");

    juce::PluginDescription pendingInsertDescription;
    test.expect (
        processor->getPendingInsertDescription (
            0, pendingInsertDescription)
         && pendingInsertDescription.createIdentifierString()
                == insertC.createIdentifierString(),
        "descrição de C deve sobreviver fora do KnownPluginList durante o scan");

    bool retirementSawLiveInsertA = false;
    processor->setHostedPluginWillRetireCallback (
        [&] (TimbresProcessor::HostedPluginLocation location,
             int index, juce::AudioPluginInstance* instance)
        {
            if (location
                    == TimbresProcessor::HostedPluginLocation::insert
                 && index == 0 && instance == firstInsert)
                retirementSawLiveInsertA =
                    insertLifecycleA->releaseCalls == 0
                    && insertLifecycleA->destructorCalls == 0;
        });
    test.expect (
        deferredRaw->complete (
            "Insert C", insertLifecycleC),
        "callback do insert C deve existir");
    auto* finalInsert = dynamic_cast<CapturingPlugin*> (
        processor->getInsert (0));
    test.expect (
        finalInsert != nullptr
         && finalInsert != firstInsert
         && insertLifecycleC->prepareCalls == 1
         && insertLifecycleA->releaseCalls == 1
         && insertLifecycleA->destructorCalls == 1
         && retirementSawLiveInsertA
         && finalInsert->getRestoredState() == currentState,
        "insert C deve fechar editor antes de liberar A e trocar em duas fases");

    const auto failedInsert =
        makeDescription ("Insert Failure", 0x5401);
    test.expect (
        processor->loadInsert (0, failedInsert)
         && dispatchUntilPending (1)
         && deferredRaw->fail ("Insert Failure"),
        "falha assíncrona de insert deve ser entregue");
    test.expect (
        processor->getInsert (0) == finalInsert
         && insertLifecycleC->releaseCalls == 0,
        "falha de insert deve manter C no grafo");

    // O destrutor de C inicia D enquanto E termina. Assim cobrimos a segunda
    // fonte real de loop modal (destrutor, além de releaseResources): D precisa
    // permanecer no pending/save e a completion de E deve ser suprimida.
    auto reentrantInsert =
        makeDescription ("Insert D Reentrant", 0x5601);
    auto transientInsert =
        makeDescription ("Insert E Transient", 0x5701);
    reentrantInsert.isInstrument = false;
    transientInsert.isInstrument = false;
    auto insertLifecycleD =
        std::make_shared<PluginLifecycleState>();
    auto insertLifecycleE =
        std::make_shared<PluginLifecycleState>();
    bool reentrantInsertAccepted = false;
    int reentrantInsertCompletion = 0;
    int transientInsertCompletion = 0;
    insertLifecycleC->onDestroy = [&]
    {
        reentrantInsertAccepted = processor->loadInsert (
            0, reentrantInsert, nullptr,
            [&reentrantInsertCompletion] (
                bool loaded, const juce::String&)
            {
                reentrantInsertCompletion += loaded ? 1 : -1;
            });
    };

    test.expect (
        processor->loadInsert (
            0, transientInsert, nullptr,
            [&transientInsertCompletion] (
                bool loaded, const juce::String&)
            {
                transientInsertCompletion += loaded ? 1 : -1;
            })
         && dispatchUntilPending (1)
         && deferredRaw->complete (
                "Insert E Transient", insertLifecycleE),
        "insert E deve concluir e destruir C");
    insertLifecycleC->onDestroy = {};
    test.expect (
        reentrantInsertAccepted
         && insertLifecycleC->releaseCalls == 1
         && insertLifecycleC->destructorCalls == 1
         && transientInsertCompletion == 0
         && processor->getInsert (0) != finalInsert,
        "pedido D do destrutor deve tornar a completion de E stale");

    auto savedDuringReentrantInsert = saveProcessorXml();
    const auto* savedReentrantInsert = findInsert (
        savedDuringReentrantInsert.get(), 0);
    test.expect (
        savedReentrantInsert != nullptr
         && savedReentrantInsert->getStringAttribute ("id")
                == reentrantInsert.createIdentifierString()
         && dispatchUntilPending (1)
         && deferredRaw->complete (
                "Insert D Reentrant", insertLifecycleD),
        "insert D iniciado pelo destrutor deve permanecer serializável");
    test.expect (
        insertLifecycleE->releaseCalls == 1
         && insertLifecycleE->destructorCalls == 1
         && reentrantInsertCompletion == 1,
        "insert D deve substituir E e concluir apenas o pedido atual");

    // Restore substitutivo sem <INSERT>: não há herança silenciosa do projeto
    // anterior. Remova todos os nós de insert do próprio estado válido.
    auto dryRestoreXml = saveProcessorXml();

    if (dryRestoreXml != nullptr)
    {
        juce::Array<juce::XmlElement*> insertsToRemove;

        for (auto* child : dryRestoreXml->getChildIterator())
            if (child->hasTagName ("INSERT"))
                insertsToRemove.add (child);

        for (auto* child : insertsToRemove)
            dryRestoreXml->removeChildElement (child, true);
    }

    juce::MemoryBlock dryRestoreState;

    if (dryRestoreXml != nullptr)
        juce::AudioProcessor::copyXmlToBinary (
            *dryRestoreXml, dryRestoreState);

    processor->setHostedPluginWillRetireCallback ({});
    processor->setStateInformation (
        dryRestoreState.getData(),
        (int) dryRestoreState.getSize());
    test.expect (
        dryRestoreXml != nullptr
         && processor->getInsert (0) == nullptr
         && insertLifecycleD->releaseCalls == 1
         && insertLifecycleD->destructorCalls == 1,
        "restore sem INSERT deve descarregar o efeito antigo exatamente uma vez");

    // Um INSERT salvo que nao existe neste host precisa manter a pagina como
    // restore pendente para o timeout/rollback, sem fingir que ainda ha uma
    // fabrica em voo. A limpeza explicita encerra esse marcador.
    juce::XmlElement missingInsertRestore ("TIMBRES2");

    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
    {
        auto* layerXml = missingInsertRestore.createNewChildElement (
            "LAYER");
        layerXml->setAttribute ("index", layer);
    }

    auto* missingInsert = missingInsertRestore.createNewChildElement (
        "INSERT");
    missingInsert->setAttribute ("slot", 0);
    missingInsert->setAttribute (
        "id", "churchdaw.tests.missing-insert");
    missingInsert->setAttribute ("state", juce::String());

    juce::MemoryBlock missingInsertState;
    juce::AudioProcessor::copyXmlToBinary (
        missingInsertRestore, missingInsertState);
    processor->setStateInformation (
        missingInsertState.getData(),
        (int) missingInsertState.getSize());
    test.expect (
        processor->getInsert (0) == nullptr
         && processor->hasPendingStateRestore(),
        "INSERT ausente deve esvaziar o slot e manter restore pendente");

    processor->unloadInsert (0);
    test.expect (
        ! processor->hasPendingStateRestore(),
        "unload explicito deve encerrar o pending do INSERT ausente");

    const auto cancelledInstrument =
        makeDescription ("Async Cancel", 0x4501);
    auto cancelledInstrumentLifecycle =
        std::make_shared<PluginLifecycleState>();
    test.expect (
        processor->loadInstrumentPlugin (
            0, cancelledInstrument)
         && dispatchUntilPending (1),
        "instrumento cancelável deve chegar ao formato");
    processor->clearInstrumentPlugin (0);
    test.expect (
        deferredRaw->complete (
            "Async Cancel", cancelledInstrumentLifecycle),
        "callback do instrumento cancelado deve existir");
    test.expect (
        processor->getLayerInstrument (0) == nullptr
         && cancelledInstrumentLifecycle->prepareCalls == 0,
        "clear deve invalidar callback tardio do instrumento");

    const auto cancelledInsert =
        makeDescription ("Insert Cancel", 0x5501);
    auto cancelledInsertLifecycle =
        std::make_shared<PluginLifecycleState>();
    test.expect (
        processor->loadInsert (0, cancelledInsert)
         && dispatchUntilPending (1),
        "insert cancelável deve chegar ao formato");
    processor->unloadInsert (0);
    test.expect (
        deferredRaw->complete (
            "Insert Cancel", cancelledInsertLifecycle),
        "callback do insert cancelado deve existir");
    test.expect (
        processor->getInsert (0) == nullptr
         && cancelledInsertLifecycle->prepareCalls == 0,
        "unload deve invalidar callback tardio do insert");

    const auto destroyedPending =
        makeDescription ("Async Processor Destroyed", 0x5f01);
    auto destroyedPendingLifecycle =
        std::make_shared<PluginLifecycleState>();
    int destroyedPendingCompletion = 0;
    test.expect (
        processor->loadInstrumentPlugin (
            0, destroyedPending, true, nullptr,
            [&destroyedPendingCompletion] (
                bool, const juce::String&)
            {
                ++destroyedPendingCompletion;
            })
         && dispatchUntilPending (1),
        "callback pendente deve estar retido pelo formato antes do destroy");

    processor.reset();
    test.expect (
        deferredRaw->complete (
            "Async Processor Destroyed",
            destroyedPendingLifecycle)
         && destroyedPendingLifecycle->prepareCalls == 0
         && destroyedPendingLifecycle->releaseCalls == 0
         && destroyedPendingLifecycle->destructorCalls == 1
         && destroyedPendingCompletion == 0,
        "callback pós-destruição deve só destruir candidato sem acessar processor");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    TestContext test;

    testGlobalSustainBeforeInstallAndReprepare (
        test);
    testLateInstalledWidePluginReservesScratch (
        test);
    testPhysicalSustainFanout (test);
    testResetControllersPreservesHeldNoteDepth (
        test);
    testResetControllersAndGlobalReapply (test);
    testGlobalSustainPreservesIntentionalBend (
        test);
    testCrossChannelPedalPreservesBend (
        test);
    testCrossChannelResetCentresEveryChannel (
        test);
    runSameSampleAllOffCase (test, false);
    runSameSampleAllOffCase (test, true);
    testLastSampleAllOffResync (test);
    testPanic (test);
    testRetriggerDepthAndNextPhraseRecentres (
        test);
    testHotLoadPitchCentresStayOnTargetLayer (
        test);
    testPluginLifecycleAndFailureContainment (
        test);
    testAsyncPluginCreationIsTwoPhaseAndRejectsStale (
        test);
    testTransposeLedgerOverflowFailsClosed (
        test);

    if (test.failureCount() != 0)
    {
        std::cerr
            << "HOSTED_INSTRUMENT_MIDI_ROUTING_FAILED "
            << test.failureCount() << '\n';
        return 1;
    }

    std::cout
        << "HOSTED_INSTRUMENT_MIDI_ROUTING_OK\n";
    return 0;
}
