#include "Piano3DPanel.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <iostream>

// Separate manual preview: never opens an audio device or reads/writes the
// musician's ChurchDAW configuration. CoreMIDI input can be shared on macOS.
class Preview final : public juce::JUCEApplication, private juce::MidiInputCallback
{
public:
    const juce::String getApplicationName() override { return "ChurchDAW Piano 3D Preview"; }
    const juce::String getApplicationVersion() override { return "0.1"; }
    void initialise (const juce::String& args) override
    {
        if (args.contains ("--self-test"))
        {
            PianoVisualState t;
            int failures = 0;
            auto check = [&] (bool b) { if (! b) ++failures; };
            check (t.channels[60].load() == 0);
            t.observe (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
            t.observe (juce::MidiMessage::noteOn (2, 60, (juce::uint8) 80));
            check (t.channels[60].load() == 3 && t.strikes[60].load() == 2);
            t.observe (juce::MidiMessage::noteOff (1, 60));
            check (t.channels[60].load() == 2);
            t.observe (juce::MidiMessage::controllerEvent (2, 64, 127));
            t.observe (juce::MidiMessage::noteOff (2, 60));
            check (t.channels[60].load() == 0); // Sustain never holds a physical key down.
            for (int n = 21; n <= 108; ++n)
                t.observe (juce::MidiMessage::noteOn (16, n, (juce::uint8) 127));
            t.observe (juce::MidiMessage::allNotesOff (1));
            check (t.channels[21].load() == 32768 && t.channels[108].load() == 32768);
            t.observe (juce::MidiMessage::allSoundOff (16));
            for (int n = 21; n <= 108; ++n) check (t.channels[(size_t)n].load() == 0);
            t.observe (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
            t.observe (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0));
            check (t.channels[60].load() == 0);
            t.observe (juce::MidiMessage::noteOn (1, 108, (juce::uint8) 100));
            t.reset(); check (t.channels[108].load() == 0);
            std::cout << "Piano telemetry: " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
            setApplicationReturnValue (failures == 0 ? 0 : 1); quit(); return;
        }
        state = std::make_shared<PianoVisualState>();
        window = std::make_unique<Window> (state);
        for (auto& device : juce::MidiInput::getAvailableDevices())
        {
            if (device.name.containsIgnoreCase ("DAW")) continue;
            if (auto input = juce::MidiInput::openDevice (device.identifier, this))
            {
                juce::Logger::writeToLog ("Piano 3D input: " + device.name);
                input->start(); inputs.push_back (std::move (input));
            }
        }
    }
    void shutdown() override { inputs.clear(); window.reset(); state.reset(); }
    void systemRequestedQuit() override { quit(); }
private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override { state->observe (m); }
    struct Content final : juce::Component
    {
        explicit Content (std::shared_ptr<PianoVisualState> s) : state (s), piano (s)
        {
            addAndMakeVisible (piano); addAndMakeVisible (test);
            test.onClick = [this]
            {
                testing = ! testing;
                for (int note : {48, 60, 64, 67, 72})
                    state->observe (testing ? juce::MidiMessage::noteOn (15, note, (juce::uint8) 100)
                                            : juce::MidiMessage::noteOff (15, note));
                test.setButtonText (testing ? "Soltar teclas de teste (sem som)" : "Testar movimento das teclas (sem som)");
            };
            setSize (980, 730);
        }
        void resized() override
        {
            auto r = getLocalBounds(); test.setBounds (r.removeFromBottom (36).reduced (12, 4)); piano.setBounds (r);
        }
        std::shared_ptr<PianoVisualState> state;
        Piano3DPanel piano;
        juce::TextButton test { "Testar movimento das teclas (sem som)" };
        bool testing = false;
    };
    struct Window final : juce::DocumentWindow
    {
        explicit Window (std::shared_ptr<PianoVisualState> state)
          : DocumentWindow (juce::String::fromUTF8 ("ChurchDAW / Piano 3D — prévia MIDI"), juce::Colour (0xff13171f), allButtons)
        {
            setUsingNativeTitleBar (true); setContentOwned (new Content (std::move (state)), true);
            setResizable (true, false); setResizeLimits (820, 480, 2560, 1600); centreWithSize (980, 730); setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::shared_ptr<PianoVisualState> state;
    std::unique_ptr<Window> window;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
};
START_JUCE_APPLICATION (Preview)
