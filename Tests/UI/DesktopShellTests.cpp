#include "DesktopHeaderLayout.h"
#include "PerformanceDock.h"

#include <iostream>

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (! condition)
    {
        ++failures;
        std::cerr << "FAIL desktop shell: " << message << '\n';
    }
}

void testHeaderRegions()
{
    expect (DesktopHeaderLayout::supports ({ 0, 0, 900, 580 }),
            "the normal desktop minimum must retain the desktop shell");
    expect (DesktopHeaderLayout::supports ({ 0, 0, 1178, 769 }),
            "native borders must not revert a 1180-wide window to legacy navigation");
    expect (! DesktopHeaderLayout::supports ({ 0, 0, 899, 580 })
            && ! DesktopHeaderLayout::supports ({ 0, 0, 900, 579 }),
            "compact fallback must remain available below supported desktop bounds");
    for (const int width : { 900, 958, 1000, 1099, 1100, 1178, 1180, 1280, 1600 })
    {
        // A translated parent catches accidental assumptions about (0, 0),
        // while the smallest width represents the desktop-layout breakpoint.
        const juce::Rectangle<int> bounds (37, 23, width, DesktopHeaderLayout::height);
        const auto layout = DesktopHeaderLayout::calculate (bounds);
        const std::array<juce::Rectangle<int>, 5> regions {
            layout.brand, layout.devices, layout.engine, layout.save, layout.navigation
        };
        for (size_t index = 0; index < regions.size(); ++index)
        {
            expect (! regions[index].isEmpty(), "a header region disappeared");
            expect (bounds.contains (regions[index]), "a header region escapes its parent");
            for (size_t other = index + 1; other < regions.size(); ++other)
                expect (! regions[index].intersects (regions[other]),
                        "header controls overlap at a supported desktop width");
        }
        expect (layout.devices.getWidth() >= (width < 1100 ? 376 : 440),
                "device names lost the minimum readable two-column allocation");
        expect (layout.navigation.getY() >= layout.devices.getBottom(),
                "navigation overlaps the device information row");
    }
}

void testRailRegions()
{
    // O trilho so entra quando a janela comporta os 900 px de antes MAIS o trilho:
    // o mixer exige >= 860 px de largura propria para manter o layout de referencia.
    const int railW = DesktopHeaderLayout::railWidth (1280);
    expect (! DesktopHeaderLayout::supportsRail ({ 0, 0, 900 + railW - 1, 580 }),
            "the rail must not shrink the mixer below its reference width");
    expect (DesktopHeaderLayout::supportsRail ({ 0, 0, 900 + railW, 580 }),
            "the rail must be available as soon as the window can hold it");
    expect (! DesktopHeaderLayout::supportsRail ({ 0, 0, 1600, 579 }),
            "the rail must respect the desktop shell minimum height");

    // A largura do cabecalho ao lado do trilho e a da janela menos o trilho.
    for (const int width : { 900, 1000, 1099, 1100, 1178, 1280, 1500 })
    {
        const juce::Rectangle<int> bounds (37, 23, width, DesktopHeaderLayout::railHeaderHeight);
        const auto layout = DesktopHeaderLayout::calculateRail (bounds);
        expect (layout.navigation.isEmpty(), "the rail layout must not reserve a tab row");
        const std::array<juce::Rectangle<int>, 4> regions {
            layout.brand, layout.devices, layout.engine, layout.save
        };
        for (size_t index = 0; index < regions.size(); ++index)
        {
            expect (! regions[index].isEmpty(), "a rail header region disappeared");
            expect (bounds.contains (regions[index]), "a rail header region escapes its parent");
            for (size_t other = index + 1; other < regions.size(); ++other)
                expect (! regions[index].intersects (regions[other]),
                        "rail header controls overlap at a supported width");
        }
        expect (layout.devices.getWidth() >= (width < 1100 ? 376 : 440),
                "device names lost the minimum readable allocation beside the rail");
    }
}
void testSustainOwnership()
{
    PianoVisualState state;
    const auto held = [&state] (int note) { return state.channels[(size_t) note].load(); };
    const auto sounding = [&state] (int note) { return state.sounding[(size_t) note].load(); };

    state.observe (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
    state.observe (juce::MidiMessage::noteOn (2, 60, (juce::uint8) 100));
    state.observe (juce::MidiMessage::controllerEvent (1, 64, 127));
    state.observe (juce::MidiMessage::noteOff (1, 60));
    expect (held (60) == 2 && sounding (60) == 3,
            "a released key must rise while its pedal keeps the harmony sounding");

    state.observe (juce::MidiMessage::noteOff (2, 60));
    expect (held (60) == 0 && sounding (60) == 1,
            "one channel's pedal must not sustain another channel's released note");
    state.observe (juce::MidiMessage::controllerEvent (1, 64, 0));
    expect (sounding (60) == 0 && state.sustainChannels.load() == 0,
            "pedal release must clear the final released note");

    state.setHostGlobalSustain (true);
    state.observeHost (juce::MidiMessage::noteOn (16, 108, (juce::uint8) 100));
    state.observeHost (juce::MidiMessage::noteOff (16, 108));
    expect (held (108) == 0 && sounding (108) == 0x8000u,
            "host-global sustain must include the upper key on channel 16");
    state.observeHost (juce::MidiMessage::controllerEvent (3, 64, 127));
    state.setHostGlobalSustain (false);
    expect (sounding (108) != 0,
            "disabling global sustain must preserve a physical pedal still held");
    state.observeHost (juce::MidiMessage::controllerEvent (3, 64, 0));
    expect (sounding (108) == 0,
            "releasing the final pedal must clear the host-sustained harmony");

    state.observeHost (juce::MidiMessage::controllerEvent (1, 64, 0), true);
    state.observeHost (juce::MidiMessage::noteOn (1, 21, (juce::uint8) 100));
    state.observeHost (juce::MidiMessage::noteOff (1, 21));
    expect (held (21) == 0 && sounding (21) == 1,
            "inverted physical sustain must retain the bottom key's harmony");
    state.observeHost (juce::MidiMessage::controllerEvent (1, 64, 127), true);
    expect (sounding (21) == 0, "inverted pedal release must clear the harmony");

    state.observeHost (juce::MidiMessage::noteOn (5, 64, (juce::uint8) 100));
    state.observeHost (juce::MidiMessage::allSoundOff (1));
    expect (held (64) == 0 && sounding (64) == 0 && state.sustainChannels.load() == 0,
            "host panic must clear physical keys, harmony and pedal lamps across channels");
}

int changedPixels (const juce::Image& before, const juce::Image& after)
{
    if (before.getBounds() != after.getBounds())
        return 0;
    int count = 0;
    for (int y = 0; y < before.getHeight(); ++y)
        for (int x = 0; x < before.getWidth(); ++x)
            if (before.getPixelAt (x, y) != after.getPixelAt (x, y))
                ++count;
    return count;
}

void testDockRendering()
{
    auto state = std::make_shared<PianoVisualState>();
    PerformanceDock dock (state);
    juce::Component host;
    host.addAndMakeVisible (dock);
    host.setBounds (0, 0, 1280, 72);
    host.addToDesktop (juce::ComponentPeer::windowIsTemporary
                      | juce::ComponentPeer::windowIgnoresMouseClicks);
    host.setVisible (true);

    const auto refresh = []
    {
        juce::Thread::sleep (65);
        juce::Timer::callPendingTimersSynchronously();
    };

    for (const int width : { 900, 958, 1180, 1280 })
    {
        state->reset();
        host.setSize (width, 72);
        dock.setBounds (host.getLocalBounds());
        refresh();
        const auto idle = dock.createComponentSnapshot (dock.getLocalBounds());
        expect (idle.isValid() && idle.getWidth() == width && idle.getHeight() == 72,
                "the dock must produce a complete snapshot at supported desktop sizes");

        state->observe (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        state->observe (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100));
        state->observe (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100));
        refresh();
        const auto pressed = dock.createComponentSnapshot (dock.getLocalBounds());
        expect (changedPixels (idle, pressed) > 50,
                "incoming atomic note telemetry must visibly reach the live dock");

        state->observe (juce::MidiMessage::controllerEvent (1, 64, 127));
        for (int note : { 60, 64, 67 })
            state->observe (juce::MidiMessage::noteOff (1, note));
        refresh();
        const auto sustained = dock.createComponentSnapshot (dock.getLocalBounds());
        expect (changedPixels (pressed, sustained) > 50,
                "released physical keys must visually rise while the pedal remains held");
        expect (state->sounding[60].load() != 0 && state->channels[60].load() == 0,
                "painting must preserve the distinction between held and sounding notes");

        state->observe (juce::MidiMessage::controllerEvent (1, 64, 0));
        refresh();
        const auto released = dock.createComponentSnapshot (dock.getLocalBounds());
        expect (changedPixels (sustained, released) > 5,
                "releasing sustain must extinguish the retained-note/pedal indicators");
        expect (state->sustainChannels.load() == 0 && state->sounding[60].load() == 0,
                "painting must never mutate incoming MIDI telemetry");
    }

    host.setVisible (false);
    host.removeFromDesktop();
    host.removeChildComponent (&dock);
}
}

int runDesktopShellTests()
{
    failures = 0;
    testHeaderRegions();
    testRailRegions();
    testSustainOwnership();
    testDockRendering();
    std::cout << "Desktop shell regressions: " << (failures == 0 ? "PASS" : "FAIL")
              << " (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
