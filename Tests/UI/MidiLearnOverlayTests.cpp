#include "MidiLearnConsoleOverlay.h"
#include <iostream>

int runMidiLearnOverlayTests()
{
    int failures = 0;
    auto expect = [&] (bool ok, const char* message)
    {
        if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    juce::Component parent, nested;
    juce::Slider fader;
    juce::TextButton mute { "M" }, finish { "Concluir" };
    MidiLearnConsoleOverlay overlay;
    parent.setBounds (40, 60, 600, 400);
    parent.setVisible (true);
    parent.addAndMakeVisible (nested);
    nested.setBounds (50, 60, 300, 300);
    nested.addAndMakeVisible (fader);
    fader.setBounds (10, 10, 40, 200);
    fader.setRange (0, 1);
    fader.setValue (0.4);
    nested.addAndMakeVisible (mute);
    mute.setBounds (80, 10, 40, 30);
    parent.addAndMakeVisible (finish);
    finish.setBounds (450, 10, 120, 30);
    parent.addAndMakeVisible (overlay);
    overlay.setBounds (parent.getLocalBounds());
    overlay.shouldPassThrough = [&] (juce::Point<int> screen)
    { return finish.getScreenBounds().contains (screen); };
    juce::Component* hit = nullptr;
    overlay.onControlClicked = [&] (juce::Component* c, auto, auto) { hit = c; };
    const auto click = [&] (juce::Point<int> point)
    {
        auto time = juce::Time::getCurrentTime();
        juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(),
            point.toFloat(), juce::ModifierKeys::leftButtonModifier,
            1.0f, 0, 0, 0, 0, &overlay, &overlay, time, point.toFloat(), time, 1, false);
        overlay.mouseDown (event);
    };
    expect (parent.getComponentAt (70, 100) == &overlay, "overlay must intercept learn clicks");
    click ({70, 100});
    expect (hit == &fader || fader.isParentOf (hit), "learn click must reach nested fader, not overlay");
    expect (fader.getValue() == 0.4, "selecting fader must not change audio level");
    bool triggered = false;
    mute.onClick = [&] { triggered = true; };
    click ({140, 80});
    expect (hit == &mute, "learn click must resolve a button");
    expect (! triggered, "selecting a button must not perform its action");
    expect (parent.getComponentAt (70, 100) == &overlay, "overlay must intercept again after lookup");
    expect (parent.getComponentAt (470, 20) == &finish, "finish must remain clickable through overlay");
    click ({380, 300});
    expect (hit == &parent, "blank area must resolve to background, not stale target");
    overlay.setInterceptsMouseClicks (false, false);
    expect (! overlay.hitTest (70, 100), "custom hitTest must respect disabled interception");
    overlay.setInterceptsMouseClicks (true, true);
    overlay.setTarget (&fader, "Volume");
    expect (overlay.hasActiveTarget(), "selection must be highlighted");
    overlay.clearTarget();
    expect (! overlay.hasActiveTarget(), "clearing selection must remove highlight");
    if (! failures) std::cout << "MIDI_LEARN_OVERLAY_OK\n";
    return failures ? 1 : 0;
}
