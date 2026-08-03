#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "AndroidAudioMidiCenter.h"
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

void testCenterLifecycleAndHostBinding()
{
    std::cout << "[TEST] AndroidAudioMidiCenter Lifecycle & Host Binding...\n";

    juce::AudioDeviceManager devManager;
    juce::ApplicationProperties appProps;
    juce::PropertiesFile::Options opts;
    opts.applicationName = "ChurchDAWTest";
    opts.filenameSuffix = ".settings";
    appProps.setStorageParameters (opts);

    bool dialogOpen = false;
    bool closeCalled = false;
    bool restartCalled = false;
    bool refreshMidiCalled = false;
    bool reconcileUsbCalled = false;

    AndroidAudioMidiCenter::Host host {
        devManager,
        appProps,
        [&] { closeCalled = true; },
        [&] { restartCalled = true; },
        [&] { refreshMidiCalled = true; },
        [&] { reconcileUsbCalled = true; },
        []() -> TimbresProcessor* { return nullptr; },
        dialogOpen
    };

    {
        auto center = std::make_unique<AndroidAudioMidiCenter> (host);
        expect (dialogOpen == true, "Host dialogOpen flag must be set to true on creation");

        center->setBounds (0, 0, 800, 600);

        // Test timer tick call
        // Note: timerCallback refreshes status and active page
    }

    expect (dialogOpen == false, "Host dialogOpen flag must be reset to false on destruction");
}

void testTabNavigationAndLayout()
{
    std::cout << "[TEST] AndroidAudioMidiCenter Tab Navigation & Multi-Screen Layout...\n";

    juce::AudioDeviceManager devManager;
    juce::ApplicationProperties appProps;
    juce::PropertiesFile::Options opts;
    opts.applicationName = "ChurchDAWTest";
    opts.filenameSuffix = ".settings";
    appProps.setStorageParameters (opts);

    bool dialogOpen = false;
    AndroidAudioMidiCenter::Host host {
        devManager, appProps, nullptr, nullptr, nullptr, nullptr, nullptr, dialogOpen
    };

    auto center = std::make_unique<AndroidAudioMidiCenter> (host);

    const std::vector<std::pair<int, int>> resolutions = {
        { 320, 480 },   // Compact phone portrait
        { 360, 640 },   // Standard phone portrait
        { 568, 320 },   // Phone landscape
        { 680, 400 },   // Wide breakpoint boundary (680)
        { 800, 1280 },  // Tablet portrait
        { 1280, 800 },  // Tablet landscape
        { 1920, 1080 }  // Desktop Full HD
    };

    for (const auto& res : resolutions)
    {
        const int w = res.first;
        const int h = res.second;

        center->setBounds (0, 0, w, h);

        juce::Image testCanvas (juce::Image::ARGB, w, h, true);
        juce::Graphics g (testCanvas);

        for (int tabIndex = 0; tabIndex < 3; ++tabIndex)
        {
            // Simulate tab selection
            center->setTab (tabIndex);
            center->resized();
            center->paint (g);
        }
    }
}

void testRapidCreationAndDestruction()
{
    std::cout << "[TEST] AndroidAudioMidiCenter Rapid Lifecycle & Async Safety...\n";

    juce::AudioDeviceManager devManager;
    juce::ApplicationProperties appProps;
    juce::PropertiesFile::Options opts;
    opts.applicationName = "ChurchDAWTest";
    opts.filenameSuffix = ".settings";
    appProps.setStorageParameters (opts);

    bool dialogOpen = false;

    for (int i = 0; i < 50; ++i)
    {
        AndroidAudioMidiCenter::Host host {
            devManager, appProps, nullptr, nullptr, nullptr, nullptr, nullptr, dialogOpen
        };

        auto center = std::make_unique<AndroidAudioMidiCenter> (host);
        center->setBounds (0, 0, 640, 480);

        juce::Image canvas (juce::Image::ARGB, 640, 480, true);
        juce::Graphics g (canvas);
        center->paint (g);

        // Immediate destruction tests destructor cleanup (stopping timer, stopping output test safely)
    }

    expect (dialogOpen == false, "dialogOpen should be false after all centers destroyed");
}

}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    testCenterLifecycleAndHostBinding();
    testTabNavigationAndLayout();
    testRapidCreationAndDestruction();

    if (failures != 0)
    {
        std::cerr << "\n*** " << failures << " TEST FAILURES ENCOUNTERED ***\n";
        return 1;
    }

    std::cout << "\nAll AndroidAudioMidiCenter empirical tests passed successfully!\n";
    return 0;
}
