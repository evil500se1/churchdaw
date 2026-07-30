#include "MidiMappingSafety.h"
#include "PerformanceMidiSafety.h"

#include <iostream>
#include <map>
#include <memory>

namespace
{
    [[noreturn]] void fail (const char* message)
    {
        std::cerr << "MIDI_MAPPING_SAFETY_FAIL: " << message << std::endl;
        std::exit (1);
    }

    void expect (bool condition, const char* message)
    {
        if (! condition)
            fail (message);
    }
}

int main()
{
    using Float = juce::AudioParameterFloat;

    auto volume = std::make_unique<Float> (
        juce::ParameterID { "vol0", 1 }, "Canal 1 Volume", 0.0f, 1.0f, 1.0f);
    auto attack = std::make_unique<Float> (
        juce::ParameterID { "attack0", 1 }, "Canal 1 Attack", 0.0f, 2.0f, 0.003f);
    auto transpose = std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "transp0", 1 }, "Canal 1 Transpor", -24, 24, 0);

    // Simula uma versao nova que inseriu Attack antes de Transpose.
    juce::Array<juce::AudioProcessorParameter*> reordered {
        volume.get(), attack.get(), transpose.get()
    };

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "transp0", "Canal 1 Transpor") == -1,
            "channel transpose must never be a MIDI Learn target");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "vol0", "Canal 1 Volume") == 0,
            "volume ID must not retarget to transpose");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, {}, "Canal 1 Transpor") == -1,
            "named pre-ID transpose mappings must be rejected");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, {}, {}) == -1,
            "index-only mappings must never resolve");
    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "missing-id", "Canal 1 Transpor") == -1,
            "orphan stable ID must not fall back to a generic name");

    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (0, {}, {}),
            "instrument index-only target must be rejected");
    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (0, "transp0", {}),
            "channel transpose stable-ID target must be rejected");
    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (0, "gtransp", {}),
            "global transpose stable-ID target must be rejected");
    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (
                0, {}, "Canal 1 Transpor"),
            "legacy named transpose target must be rejected");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, "vol0", {}),
            "ordinary stable-ID target must remain accepted");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, {}, "Canal 1 Volume"),
            "named legacy target must be accepted");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (-1, {}, {}),
            "master mapping has no mutable instrument index");

    expect (! ChurchDAWMidiMapping::isMappableController (64),
            "sustain pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (66),
            "sostenuto pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (67),
            "soft pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (121),
            "channel-mode reset must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (101),
            "RPN select must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (6),
            "Data Entry must never restore as a parameter map");
    expect (ChurchDAWMidiMapping::isMappableController (28),
            "ordinary encoder CC must remain mappable");

    const auto route8 = ChurchDAWMidiMapping::launchkeyEncoderRoute (7);
    expect (route8.target
                == ChurchDAWMidiMapping::LaunchkeyEncoderTarget::none,
            "Launchkey knob 8 must never have a fixed transpose route");

    using Key = ChurchDAWMidiMapping::ControllerKey;
    std::map<Key, juce::String> isolatedMappings;
    const Key keyboardA { "device-A", 1, 28 };
    const Key keyboardAChannel2 { "device-A", 2, 28 };
    const Key keyboardB { "device-B", 1, 28 };
    const Key launchkeyDaw { "churchdaw:launchkey-daw", 1, 28 };
    isolatedMappings[keyboardA] = "transp0";

    expect (isolatedMappings.find (keyboardA) != isolatedMappings.end(),
            "exact source/channel/CC mapping missing");
    expect (isolatedMappings.find (keyboardAChannel2) == isolatedMappings.end(),
            "same device on another channel leaked into transpose");
    expect (isolatedMappings.find (keyboardB) == isolatedMappings.end(),
            "another device with same CC leaked into transpose");
    expect (isolatedMappings.find (launchkeyDaw) == isolatedMappings.end(),
            "Launchkey DAW port collided with musical MIDI mapping");

    // Soak deterministico: um milhao de eventos com CC igual, mas origem/canal
    // diferente, nao pode encontrar o alvo de transpose.
    for (int event = 0; event < 1000000; ++event)
    {
        const Key incoming {
            (event & 1) == 0 ? "device-B" : "churchdaw:launchkey-daw",
            1 + (event % 16),
            28
        };
        expect (isolatedMappings.find (incoming) == isolatedMappings.end(),
                "transpose source isolation failed during soak");
    }

    juce::MidiBuffer panic;
    ChurchDAWPerformanceMidiSafety::appendFullPanic (panic, 0);

    int pitchCentres = 0;
    int controllerResets = 0;
    bool channelSeen[17] {};

    for (const auto meta : panic)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel())
        {
            expect (message.getPitchWheelValue() == 8192,
                    "panic pitch wheel must be centered");
            ++pitchCentres;
            channelSeen[message.getChannel()] = true;
        }

        if (message.isController() && message.getControllerNumber() == 121)
            ++controllerResets;
    }

    expect (pitchCentres == 16, "panic must center pitch on all 16 channels");
    expect (controllerResets == 16, "panic must reset controllers on all 16 channels");

    for (int channel = 1; channel <= 16; ++channel)
        expect (channelSeen[channel], "panic missed a MIDI channel");

    // A mesma nota em dois canais pode ter sido ligada com transposes
    // diferentes. Cada note-off precisa voltar ao deslocamento do seu proprio
    // note-on; compartilhar somente [layer][note] deixava nota presa no VST/AU.
    ChurchDAWPerformanceMidiSafety::NoteTransposeLedger<5> transposeLedger;
    expect (transposeLedger.recordNoteOn (0, 1, 60, 72),
            "channel 1 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.recordNoteOn (0, 2, 60, 55),
            "channel 2 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.translatedNoteOff (0, 1, 60) == 72,
            "channel 1 note-off lost its original transpose");
    expect (transposeLedger.translatedNoteOff (0, 2, 60) == 55,
            "channel 2 note-off collided with channel 1");
    expect (transposeLedger.translatedNoteOff (1, 1, 60) == 60,
            "transpose ledger leaked across layers");

    // Se o Note-Off chegar em outro canal por causa de transporte host/MIDI,
    // devemos recuperar o deslocamento da mesma nota no layer e evitar tom preso.
    expect (transposeLedger.recordNoteOn (0, 1, 64, 76),
            "channel 1 transpose note-on for channel-mismatch fallback test");
    expect (transposeLedger.translatedNoteOff (0, 2, 64) == 76,
            "channel-mismatch note-off must recover stored transpose");

    // Retrigger sobreposto NA MESMA layer/canal/nota: um unico offset
    // sobrescrevia o primeiro note-on e deixava a voz antiga presa.
    expect (transposeLedger.recordNoteOn (3, 4, 64, 76),
            "overlapping transpose note-on #1 was rejected");
    expect (transposeLedger.recordNoteOn (3, 4, 64, 59),
            "overlapping transpose note-on #2 was rejected");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 76,
            "overlapping note-on lost FIFO transpose #1");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 59,
            "overlapping note-on lost FIFO transpose #2");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 64,
            "consumed ledger entry was not cleared");

    // O nono retrigger nao pode substituir nenhuma das oito traducoes ainda
    // pendentes. O chamador recebe false e pode manter a nota sem transpose.
    transposeLedger.clear();

    for (std::size_t retrigger = 0;
         retrigger
             < ChurchDAWPerformanceMidiSafety::
                   NoteTransposeLedger<5>::maxOverlappingNoteOns;
         ++retrigger)
    {
        expect (transposeLedger.recordNoteOn (
                    4, 16, 40, 48 + (int) retrigger),
                "ledger rejected a retrigger before reaching capacity");
    }

    expect (! transposeLedger.recordNoteOn (4, 16, 40, 96),
            "ninth retrigger must fail instead of overwriting the FIFO");

    for (std::size_t retrigger = 0;
         retrigger
             < ChurchDAWPerformanceMidiSafety::
                   NoteTransposeLedger<5>::maxOverlappingNoteOns;
         ++retrigger)
    {
        expect (transposeLedger.translatedNoteOff (4, 16, 40)
                    == 48 + (int) retrigger,
                "full ledger did not preserve an accepted FIFO entry");
    }

    expect (transposeLedger.translatedNoteOff (4, 16, 40) == 40,
            "rejected ninth retrigger unexpectedly entered the ledger");
    expect (transposeLedger.recordNoteOn (4, 16, 40, 91),
            "drained ledger did not accept a new retrigger");
    transposeLedger.clearLayer (4);
    expect (transposeLedger.translatedNoteOff (4, 16, 40) == 40,
            "clearLayer did not discard a post-capacity entry");
    expect (! transposeLedger.recordNoteOn (-1, 1, 60, 72),
            "invalid ledger coordinates must fail explicitly");

    expect (transposeLedger.recordNoteOn (1, 3, 48, 60),
            "layer 1 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.recordNoteOn (2, 3, 48, 36),
            "layer 2 transpose note-on was unexpectedly rejected");
    transposeLedger.clearLayer (1);
    expect (transposeLedger.translatedNoteOff (1, 3, 48) == 48,
            "layer swap retained a stale transpose entry");
    expect (transposeLedger.translatedNoteOff (2, 3, 48) == 36,
            "layer-local clear leaked into another channel strip");

    transposeLedger.clear();
    expect (transposeLedger.translatedNoteOff (0, 1, 60) == 60,
            "panic/reset did not clear note transpose ledger");

    // O AU/VST recebe um bend durante a frase, mas o teclado perde o retorno
    // ao centro. A proxima frase precisa comecar centrada.
    ChurchDAWPerformanceMidiSafety::PitchBendGuard pitchGuard;
    juce::MidiBuffer pitchSource;
    juce::MidiBuffer pitchOutput;
    pitchSource.ensureSize (64 * 1024);
    pitchOutput.ensureSize (64 * 1024);

    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (1, 12000), 8);
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 60), 24);
    pitchGuard.process (pitchSource, pitchOutput);

    int centresBeforeFirstPhrase = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++centresBeforeFirstPhrase;

    expect (centresBeforeFirstPhrase == 0,
            "intentional bend during held note was overwritten");

    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 5);
    pitchGuard.process (pitchSource, pitchOutput);

    bool sawCentreBeforeNextNote = false;
    bool sawNextNote = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue
             && ! sawNextNote)
            sawCentreBeforeNextNote = true;
        else if (message.isNoteOn())
            sawNextNote = true;
    }

    expect (sawCentreBeforeNextNote && sawNextNote,
            "stale bend was not centered before next phrase");

    // Pitch Wheel novo durante o silencio e um pre-bend explicito e deve
    // sobreviver ate o ataque seguinte.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
    pitchGuard.process (pitchSource, pitchOutput);
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (1, 10000), 2);
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 6);
    pitchGuard.process (pitchSource, pitchOutput);

    int preBendCentres = 0;
    bool sawExplicitPreBend = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++preBendCentres;

        if (message.isPitchWheel() && message.getPitchWheelValue() == 10000)
            sawExplicitPreBend = true;
    }

    expect (sawExplicitPreBend && preBendCentres == 0,
            "explicit pre-bend was incorrectly centered");

    // Legato no mesmo sample nao cria uma nova frase nem um degrau de pitch.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 67), 12);
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 12);
    pitchGuard.process (pitchSource, pitchOutput);

    int sameSampleCentres = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++sameSampleCentres;

    expect (sameSampleCentres == 0,
            "same-sample legato was treated as a stale new phrase");

    // Sustain mantem a frase viva mesmo sem teclas fisicamente pressionadas.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOn (2, 55, (juce::uint8) 100), 0);
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (2, 11000), 2);
    pitchSource.addEvent (juce::MidiMessage::controllerEvent (2, 64, 127), 4);
    pitchSource.addEvent (juce::MidiMessage::noteOff (2, 55), 6);
    pitchSource.addEvent (juce::MidiMessage::noteOn (2, 59, (juce::uint8) 100), 10);
    pitchGuard.process (pitchSource, pitchOutput);

    int sustainCentres = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++sustainCentres;

    expect (sustainCentres == 0,
            "sustain-held phrase was incorrectly centered");

    // CC120/123 sao fronteiras GLOBAIS. Um PANIC originado no canal 4 deve
    // centralizar e limpar tambem o bend+sustain herdado no canal 1. Todos os
    // eventos ordinarios do sample preservam a ordem; os 16 centros e os
    // all-off coalescidos ficam no fim.
    const auto exerciseGlobalAllOff = [&] (int allOffController,
                                           bool sameSample,
                                           bool allOffBeforeNote)
    {
        constexpr int boundarySample = 8;
        const int newNoteSample = sameSample ? boundarySample : 9;

        pitchGuard.reset();
        pitchSource.clear();
        pitchSource.addEvent (
            juce::MidiMessage::noteOn (
                1, 50, (juce::uint8) 100), 0);
        pitchSource.addEvent (
            juce::MidiMessage::pitchWheel (
                1, 13000), 2);
        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                1, 64, 127), 3);
        pitchSource.addEvent (
            juce::MidiMessage::noteOff (1, 50), 4);
        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                2, 11, 31), boundarySample);

        const auto addDuplicateAllOff = [&]
        {
            pitchSource.addEvent (
                juce::MidiMessage::controllerEvent (
                    4, allOffController, 0), boundarySample);
            pitchSource.addEvent (
                juce::MidiMessage::controllerEvent (
                    4, allOffController, 0), boundarySample);
        };

        if (! sameSample || allOffBeforeNote)
            addDuplicateAllOff();

        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                2, 11, 63), boundarySample);
        pitchSource.addEvent (
            juce::MidiMessage::noteOn (
                1, 57, (juce::uint8) 100), newNoteSample);

        if (sameSample && ! allOffBeforeNote)
            addDuplicateAllOff();

        pitchGuard.process (pitchSource, pitchOutput);

        int eventIndex = 0;
        int markerOneIndex = -1;
        int markerTwoIndex = -1;
        int newNoteIndex = -1;
        int firstCentreIndex = -1;
        int lastCentreIndex = -1;
        int lastOrdinaryBoundaryIndex = -1;
        int allOffIndex = -1;
        int lastBoundaryEventIndex = -1;
        int centreCount = 0;
        int allOffCount = 0;
        bool centreChannels[17] {};

        for (const auto meta : pitchOutput)
        {
            const auto message = meta.getMessage();
            const bool isBoundaryCentre =
                meta.samplePosition == boundarySample
                && message.isPitchWheel()
                && message.getPitchWheelValue()
                     == ChurchDAWPerformanceMidiSafety::
                            PitchBendGuard::centreValue;
            const bool isExpectedAllOff =
                meta.samplePosition == boundarySample
                && message.isController()
                && message.getControllerNumber() == allOffController;

            if (meta.samplePosition == boundarySample)
                lastBoundaryEventIndex = eventIndex;

            if (isBoundaryCentre)
            {
                if (firstCentreIndex < 0)
                    firstCentreIndex = eventIndex;

                lastCentreIndex = eventIndex;
                ++centreCount;
                centreChannels[message.getChannel()] = true;
            }
            else if (isExpectedAllOff)
            {
                allOffIndex = eventIndex;
                ++allOffCount;
                expect (message.getChannel() == 4,
                        "global all-off changed the source MIDI channel");
            }
            else if (meta.samplePosition == boundarySample)
            {
                lastOrdinaryBoundaryIndex = eventIndex;
            }

            if (message.isController()
                 && message.getChannel() == 2
                 && message.getControllerNumber() == 11)
            {
                if (message.getControllerValue() == 31)
                    markerOneIndex = eventIndex;
                else if (message.getControllerValue() == 63)
                    markerTwoIndex = eventIndex;
            }

            if (message.isNoteOn()
                 && message.getChannel() == 1
                 && message.getNoteNumber() == 57)
                newNoteIndex = eventIndex;

            ++eventIndex;
        }

        expect (markerOneIndex >= 0 && markerTwoIndex > markerOneIndex,
                "ordinary same-sample event order was not preserved");
        expect (centreCount == 16,
                "global all-off must emit 16 real pitch centres");
        expect (firstCentreIndex > lastOrdinaryBoundaryIndex,
                "pitch centres must follow ordinary boundary-sample events");
        expect (allOffCount == 1,
                "duplicate all-off was not coalesced per channel/controller");
        expect (allOffIndex > lastCentreIndex,
                "all-off must follow every global pitch centre");
        expect (allOffIndex == lastBoundaryEventIndex,
                "all-off was not the final event in its sample");
        expect (newNoteIndex >= 0,
                "cross-channel all-off/new-note stream was incomplete");

        if (sameSample)
            expect (newNoteIndex < firstCentreIndex,
                    "same-sample note was not kept before the all-off boundary");
        else
            expect (newNoteIndex > allOffIndex,
                    "next-sample note did not follow the all-off boundary");

        for (int channel = 1; channel <= 16; ++channel)
            expect (centreChannels[channel],
                    "global all-off missed a pitch-centre channel");

        if (sameSample)
        {
            // A nota do canal 1 ocorreu antes do boundary adiado e precisa ter
            // sido descartada junto com sustain. Depois do reset, o note-off
            // abaixo e orfao e encerra o pre-bend; sem reset ele encontraria a
            // tecla+sustain antigos e nao produziria o centro esperado.
            pitchSource.clear();
            pitchSource.addEvent (
                juce::MidiMessage::pitchWheel (1, 10000), 0);
            pitchSource.addEvent (
                juce::MidiMessage::noteOff (1, 57), 1);
            pitchSource.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, (juce::uint8) 100), 3);
            pitchGuard.process (pitchSource, pitchOutput);

            int postBoundaryCentres = 0;
            bool sawPostBoundaryPreBend = false;

            for (const auto meta : pitchOutput)
            {
                const auto message = meta.getMessage();

                if (message.isPitchWheel()
                     && message.getPitchWheelValue()
                          == ChurchDAWPerformanceMidiSafety::
                                 PitchBendGuard::centreValue)
                    ++postBoundaryCentres;

                if (message.isPitchWheel()
                     && message.getPitchWheelValue() == 10000)
                    sawPostBoundaryPreBend = true;
            }

            expect (sawPostBoundaryPreBend
                     && postBoundaryCentres == 1,
                    "global all-off did not reset channel 1 state");
        }
    };

    for (const int allOffController : { 120, 123 })
    {
        exerciseGlobalAllOff (allOffController, false, true);
        exerciseGlobalAllOff (allOffController, true, true);
        exerciseGlobalAllOff (allOffController, true, false);
    }

    const auto countCentres = [&]
    {
        int count = 0;

        for (const auto meta : pitchOutput)
            if (meta.getMessage().isPitchWheel()
                 && meta.getMessage().getPitchWheelValue()
                      == ChurchDAWPerformanceMidiSafety::
                             PitchBendGuard::centreValue)
                ++count;

        return count;
    };

    // O sustain da interface existe fora do fluxo MIDI. Enquanto ligado, um
    // note-off não encerra a frase; na borda ON->OFF entre callbacks, a próxima
    // nota deve receber centro mesmo sem aparecer CC64 no bloco.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 12000), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 60), 4);
    pitchGuard.process (
        pitchSource, pitchOutput, true, 0);
    expect (countCentres() == 0,
            "UI global sustain ended a still-sustained phrase");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 62, (juce::uint8) 100), 5);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 62), 7);
    pitchGuard.process (
        pitchSource, pitchOutput, true, 0);
    expect (countCentres() == 0,
            "UI global sustain recentered a continued phrase");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 100), 0);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 0);
    expect (countCentres() == 1,
            "UI sustain ON->OFF did not close the previous phrase");

    // O pedal físico também é global no host. CC64 no canal 4 precisa manter a
    // voz/bend do canal 1 vivo; a soltura global então encerra a frase de todos.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 50, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 12500), 1);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 127), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 50), 3);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 52, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 0);
    expect (countCentres() == 0,
            "cross-channel CC64 did not sustain channel 1");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 52), 0);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 0), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 55, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 1,
            "cross-channel CC64 OFF did not close channel 1 phrase");

    // Reprepare zera o espelho interno, mas não move fisicamente o pedal. O
    // estado de início informado pelo host deve sobreviver sem retunar a voz.
    pitchGuard.requestPitchCentresOnNextBlock();
    pitchSource.clear();
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 16,
            "reprepare did not emit all pitch centres");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 57, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 11000), 1);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 57), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 59, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 0,
            "physical sustain held across reprepare was forgotten");

    // Reabertura da rota centraliza todos os canais no primeiro bloco real.
    pitchGuard.requestPitchCentresOnNextBlock();
    pitchSource.clear();
    pitchGuard.process (pitchSource, pitchOutput);

    int reconnectCentres = 0;
    bool reconnectChannels[17] {};

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
        {
            ++reconnectCentres;
            reconnectChannels[message.getChannel()] = true;
        }
    }

    expect (reconnectCentres == 16,
            "reconnection must center all 16 MIDI channels");

    for (int channel = 1; channel <= 16; ++channel)
        expect (reconnectChannels[channel],
                "reconnection missed a MIDI channel");

    // CC121 tambem recebe centro explicito para plugins que implementam o
    // reset de controladores de forma incompleta.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::controllerEvent (3, 121, 0), 7);
    pitchGuard.process (pitchSource, pitchOutput);

    bool cc121Centre = false;

    for (const auto meta : pitchOutput)
        if (meta.samplePosition == 7
             && meta.getMessage().isPitchWheel()
             && meta.getMessage().getChannel() == 3
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            cc121Centre = true;

    expect (cc121Centre,
            "CC121 reset did not explicitly center pitch");

    // CC121 zera controladores, mas não encerra teclas. Se o guard esquecer a
    // nota 60 ainda pressionada, o NoteOff62 abaixo cria um falso fim de frase
    // e recentraliza abruptamente o ataque 64 (e a nota 60 que segue soando).
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            1, 121, 0), 1);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 9000), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 62, (juce::uint8) 100), 3);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 62), 4);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput);

    int spuriousCentresBefore64 = 0;
    bool sawBentAttack64 = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (meta.samplePosition == 5
             && message.isPitchWheel()
             && message.getPitchWheelValue()
                  == ChurchDAWPerformanceMidiSafety::
                         PitchBendGuard::centreValue)
            ++spuriousCentresBefore64;

        if (meta.samplePosition == 5
             && message.isNoteOn()
             && message.getChannel() == 1
             && message.getNoteNumber() == 64)
            sawBentAttack64 = true;
    }

    expect (sawBentAttack64
             && spuriousCentresBefore64 == 0,
            "CC121 forgot a held note and centered a continued phrase");

    // Soltar as duas teclas reais deve encerrar a frase normalmente; só então
    // a próxima nota recebe o centro que remove o bend herdado de 9000.
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 64), 0);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 60), 1);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 65, (juce::uint8) 100), 3);
    pitchGuard.process (
        pitchSource, pitchOutput);

    int centresAfterRealRelease = 0;
    bool sawNextPhrase65 = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (meta.samplePosition == 3
             && message.isPitchWheel()
             && message.getPitchWheelValue()
                  == ChurchDAWPerformanceMidiSafety::
                         PitchBendGuard::centreValue)
            ++centresAfterRealRelease;

        if (meta.samplePosition == 3
             && message.isNoteOn()
             && message.getChannel() == 1
             && message.getNoteNumber() == 65)
            sawNextPhrase65 = true;
    }

    expect (sawNextPhrase65
             && centresAfterRealRelease == 1,
            "held-note tracking did not close after the real releases");

    std::cout << "MIDI_MAPPING_SAFETY_OK" << std::endl;
    return 0;
}
