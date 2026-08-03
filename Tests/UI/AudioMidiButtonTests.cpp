#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioMidiButton.h"
#include <iostream>
#include <vector>
#include <memory>

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void testTextResponsiveness()
{
    std::cout << "[TEST] AudioMidiButton Text Responsiveness...\n";
    AudioMidiButton button;
    
    // Default text check
    expect (button.getButtonText() == "Audio / MIDI", "Default text must be 'Audio / MIDI'");

    // Simulate narrowPhone logic from MainComponent.cpp
    const juce::String audioNarrow = juce::String::fromUTF8 ("\xc3\x81udio");
    const juce::String audioWide   = juce::String::fromUTF8 ("\xc3\x81udio / MIDI");

    // 1. Narrow Phone scenario (safeContentWidth <= 760)
    bool narrowPhone = true;
    button.setButtonText (narrowPhone ? audioNarrow : audioWide);
    expect (button.getButtonText() == audioNarrow, "Narrow phone mode must set button text to 'Áudio'");

    // 2. Wide / Desktop scenario (safeContentWidth > 760)
    narrowPhone = false;
    button.setButtonText (narrowPhone ? audioNarrow : audioWide);
    expect (button.getButtonText() == audioWide, "Wide mode must set button text to 'Áudio / MIDI'");
}

void testActiveSupplierAndState()
{
    std::cout << "[TEST] AudioMidiButton Active Supplier & State...\n";
    AudioMidiButton button;
    
    expect (! button.isPanelActive(), "Button should be inactive by default");

    // Direct active setting
    button.setActive (true);
    expect (button.isPanelActive(), "Button should be active after setActive(true)");
    button.setActive (false);
    expect (! button.isPanelActive(), "Button should be inactive after setActive(false)");

    // Supplier setting
    std::unique_ptr<juce::Component> simulatedAudioPanel;
    button.setActiveSupplier ([&simulatedAudioPanel] { return simulatedAudioPanel != nullptr; });

    expect (! button.isPanelActive(), "Supplier should report inactive when panel is nullptr");

    simulatedAudioPanel = std::make_unique<juce::Component>();
    expect (button.isPanelActive(), "Supplier should report active when panel exists");

    simulatedAudioPanel.reset();
    expect (! button.isPanelActive(), "Supplier should report inactive after panel reset");
}

void testPaintingSafetyUnderVaryingBounds()
{
    std::cout << "[TEST] AudioMidiButton Painting Safety & Edge Bounds...\n";
    AudioMidiButton button;

    const juce::String audioNarrow = juce::String::fromUTF8 ("\xc3\x81udio");
    const juce::String audioWide   = juce::String::fromUTF8 ("\xc3\x81udio / MIDI");

    const std::vector<std::pair<int, int>> testSizes = {
        { 0, 0 },
        { 1, 1 },
        { 10, 10 },
        { 50, 20 },
        { 68, 32 },  // Mobile narrow phone portrait
        { 70, 32 },  // Mobile narrow phone landscape
        { 84, 32 },  // Threshold < 85
        { 88, 32 },  // Fallback (85 <= w < 90) - MIDI icon skipped, wide text
        { 90, 32 },  // Minimum MIDI icon threshold
        { 96, 36 },  // Mobile compact layout
        { 104, 36 }, // Desktop standard layout
        { 200, 50 }, // Wide header
        { 1000, 100 } // Ultra wide
    };

    for (const auto& size : testSizes)
    {
        const int w = size.first;
        const int h = size.second;

        juce::Image testImage (juce::Image::ARGB, juce::jmax (1, w), juce::jmax (1, h), true);
        juce::Graphics g (testImage);

        button.setBounds (0, 0, w, h);

        // Test with narrow text
        button.setButtonText (audioNarrow);
        for (bool active : { false, true })
        {
            button.setActive (active);
            for (bool highlight : { false, true })
            {
                for (bool down : { false, true })
                {
                    button.paintButton (g, highlight, down);
                }
            }
        }

        // Test with wide text
        button.setButtonText (audioWide);
        for (bool active : { false, true })
        {
            button.setActive (active);
            for (bool highlight : { false, true })
            {
                for (bool down : { false, true })
                {
                    button.paintButton (g, highlight, down);
                }
            }
        }
    }
}

void testRapidClickingAndDestruction()
{
    std::cout << "[TEST] AudioMidiButton Rapid Clicking & Component Lifecycle...\n";
    
    AudioMidiButton button ("Audio / MIDI");
    std::unique_ptr<juce::Component> simulatedAudioPanel;
    
    button.setActiveSupplier ([&simulatedAudioPanel] { return simulatedAudioPanel != nullptr; });

    juce::Image testImage (juce::Image::ARGB, 104, 36, true);
    juce::Graphics g (testImage);

    button.setBounds (0, 0, 104, 36);

    // Simulate 10,000 rapid toggle clicks
    constexpr int iterations = 10000;
    for (int i = 0; i < iterations; ++i)
    {
        // Simulated onClick handler (matches MainComponent::showAudioSettings toggle logic)
        if (simulatedAudioPanel != nullptr)
        {
            simulatedAudioPanel.reset();
        }
        else
        {
            simulatedAudioPanel = std::make_unique<juce::Component>();
        }

        // Verify state
        const bool expectedActive = (simulatedAudioPanel != nullptr);
        expect (button.isPanelActive() == expectedActive, "Panel active state mismatch during rapid clicking");

        // Render button under new state
        button.paintButton (g, (i % 2 == 0), (i % 3 == 0));
    }

    expect (simulatedAudioPanel == nullptr || simulatedAudioPanel != nullptr, "Sanity check");
}
}

int main()
{
    // Initialise JUCE GUI subsystems for graphics test
    juce::ScopedJuceInitialiser_GUI juceInit;

    testTextResponsiveness();
    testActiveSupplierAndState();
    testPaintingSafetyUnderVaryingBounds();
    testRapidClickingAndDestruction();

    if (failures != 0)
    {
        std::cerr << "\n*** " << failures << " TEST FAILURES ENCOUNTERED ***\n";
        return 1;
    }

    std::cout << "\nAll AudioMidiButton empirical tests passed successfully!\n";
    return 0;
}
