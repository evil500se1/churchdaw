#include <array>
#include <iostream>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../../../Source/ChurchOrgan/ChurchOrganEditor.h"
#include "../../../Source/ChurchOrgan/ChurchOrganLayout.h"
#include "../../../Source/ChurchOrgan/UI/ChurchOrganControls.h"
#include "../../../Source/ChurchOrgan/UI/ChurchOrganViewModel.h"

namespace
{
using churchdaw::organ::ChurchOrganEditor;
using churchdaw::organ::ChurchOrganLayout;
using churchdaw::organ::ChurchOrganMockViewModel;
using churchdaw::organ::ChurchOrganDrawbar;
using churchdaw::organ::ChurchOrganParameterSlider;

struct Target
{
    const char* name;
    int width;
    int height;
};

constexpr std::array<Target, 5> targets
{{
    { "android-320x568", 320, 568 },
    { "android-393x873", 393, 873 },
    { "landscape-640x360", 640, 360 },
    { "tablet-800x1280", 800, 1280 },
    { "desktop-1366x768", 1366, 768 }
}};

int failures = 0;

void expect (bool condition, const std::string& message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct WheelProbe final : juce::Component
{
    void mouseWheelMove (const juce::MouseEvent&,
                         const juce::MouseWheelDetails& wheel) override
    {
        ++events;
        lastWheel = wheel;
    }

    int events = 0;
    juce::MouseWheelDetails lastWheel {};
};

juce::MouseEvent makeMouseEvent (
    juce::Component& target,
    juce::Point<float> position,
    juce::ModifierKeys modifiers,
    juce::Time eventTime,
    juce::Point<float> mouseDownPosition,
    juce::Time mouseDownTime,
    bool wasDragged)
{
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        modifiers,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        &target,
        &target,
        eventTime,
        mouseDownPosition,
        mouseDownTime,
        1,
        wasDragged
    };
}

juce::Time millisecondsAfter (juce::Time time, int milliseconds)
{
    return juce::Time (
        time.toMilliseconds() + static_cast<juce::int64> (milliseconds));
}

void testScrollGesturesDoNotEditOrganControls()
{
    WheelProbe parent;
    parent.setBounds (0, 0, 240, 220);

    ChurchOrganDrawbar drawbar (0, "16'");
    parent.addAndMakeVisible (drawbar);
    drawbar.setBounds (0, 0, 80, 180);
    drawbar.setValue (4.0, juce::dontSendNotification);

    int drawbarChanges = 0;
    drawbar.onValueChange = [&drawbarChanges]
    {
        ++drawbarChanges;
    };

    expect (! drawbar.isScrollWheelEnabled(),
            "drawbar accepts wheel/touchpad value edits");

    const std::array<juce::MouseWheelDetails, 4> gestures
    {{
        { 0.0f, -0.22f, false, true,  false },
        { 0.0f,  1.00f, false, false, false },
        { 0.0f, -0.18f, true,  true,  false },
        { 0.7f,  0.04f, false, true,  true  }
    }};

    const auto startTime = juce::Time::getCurrentTime();
    for (size_t index = 0; index < gestures.size(); ++index)
    {
        const auto eventTime = millisecondsAfter (
            startTime, static_cast<int> (index + 1));
        auto event = makeMouseEvent (
            drawbar,
            { 40.0f, 90.0f },
            {},
            eventTime,
            { 40.0f, 90.0f },
            eventTime,
            false);
        drawbar.mouseWheelMove (event, gestures[index]);
    }

    expect (drawbar.getValue() == 4.0,
            "touchpad/wheel changed the drawbar value");
    expect (drawbarChanges == 0,
            "touchpad/wheel reached the drawbar parameter sink");
    expect (parent.events == static_cast<int> (gestures.size()),
            "drawbar swallowed page-scroll gestures");
    expect (parent.lastWheel.isInertial,
            "inertial touchpad gesture did not reach the parent");

    // Prova a cadeia real usada pela aba BARS: Slider -> conteúdo -> Viewport.
    juce::Component viewportContent;
    juce::Viewport viewport;
    ChurchOrganDrawbar viewportDrawbar (1, "5 1/3'");
    viewportContent.setSize (200, 480);
    viewportContent.addAndMakeVisible (viewportDrawbar);
    viewportDrawbar.setBounds (10, 20, 80, 180);
    viewportDrawbar.setValue (4.0, juce::dontSendNotification);
    viewport.setBounds (0, 0, 200, 120);
    viewport.setScrollBarsShown (true, false);
    viewport.setViewedComponent (&viewportContent, false);
    viewport.setViewPosition (0, 0);

    auto viewportEvent = makeMouseEvent (
        viewportDrawbar,
        { 40.0f, 90.0f },
        {},
        millisecondsAfter (startTime, 8),
        { 40.0f, 90.0f },
        millisecondsAfter (startTime, 8),
        false);
    viewportDrawbar.mouseWheelMove (
        viewportEvent,
        { 0.0f, -0.4f, false, true, false });

    expect (viewportDrawbar.getValue() == 4.0,
            "viewport scroll changed the drawbar value");
    expect (viewport.getViewPositionY() > 0,
            "touchpad gesture did not scroll the BARS viewport");
    viewport.setViewedComponent (nullptr, false);

    ChurchOrganParameterSlider parameter (
        "Drive",
        ChurchOrganParameterSlider::Presentation::rotary);
    parent.addAndMakeVisible (parameter);
    parameter.setBounds (100, 0, 100, 100);
    parameter.setValue (50.0, juce::dontSendNotification);

    int parameterChanges = 0;
    parameter.onValueChange = [&parameterChanges]
    {
        ++parameterChanges;
    };

    expect (! parameter.isScrollWheelEnabled(),
            "advanced-page slider accepts wheel/touchpad value edits");

    auto parameterEvent = makeMouseEvent (
        parameter,
        { 50.0f, 50.0f },
        {},
        millisecondsAfter (startTime, 10),
        { 50.0f, 50.0f },
        millisecondsAfter (startTime, 10),
        false);
    parameter.mouseWheelMove (
        parameterEvent,
        { 0.0f, -0.3f, false, true, false });

    expect (parameter.getValue() == 50.0,
            "touchpad/wheel changed an advanced-page parameter");
    expect (parameterChanges == 0,
            "touchpad/wheel reached an advanced-page parameter sink");
    expect (parent.events == static_cast<int> (gestures.size()) + 1,
            "advanced-page slider swallowed the page-scroll gesture");

    // A proteção é específica da roda: edição intencional por drag continua.
    const auto dragStart = millisecondsAfter (startTime, 20);
    const auto leftButton =
        juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    auto mouseDown = makeMouseEvent (
        drawbar,
        { 40.0f, 100.0f },
        leftButton,
        dragStart,
        { 40.0f, 100.0f },
        dragStart,
        false);
    drawbar.mouseDown (mouseDown);

    auto mouseDrag = makeMouseEvent (
        drawbar,
        { 40.0f, 20.0f },
        leftButton,
        millisecondsAfter (dragStart, 16),
        { 40.0f, 100.0f },
        dragStart,
        true);
    drawbar.mouseDrag (mouseDrag);
    drawbar.mouseUp (mouseDrag);

    expect (drawbar.getValue() > 4.0,
            "intentional drawbar drag stopped working");
    expect (drawbarChanges > 0,
            "intentional drawbar drag did not reach the parameter sink");

    drawbar.setValue (4.0, juce::dontSendNotification);
    const int changesBeforeKeyboard = drawbarChanges;
    expect (drawbar.keyPressed (
                juce::KeyPress (juce::KeyPress::upKey)),
            "drawbar no longer accepts keyboard arrows");
    expect (drawbar.getValue() == 5.0,
            "keyboard arrow did not increment the drawbar");
    expect (drawbarChanges == changesBeforeKeyboard + 1,
            "keyboard edit did not reach the parameter sink");

    drawbar.setValue (6.0, juce::dontSendNotification);
    const int changesBeforeDoubleClick = drawbarChanges;
    drawbar.mouseDoubleClick (mouseDown);
    expect (drawbar.getValue() == 0.0,
            "double-click no longer resets the drawbar");
    expect (drawbarChanges == changesBeforeDoubleClick + 1,
            "double-click reset did not reach the parameter sink exactly once");
}

bool containedBy (juce::Rectangle<int> inner, juce::Rectangle<int> outer)
{
    return inner.isEmpty() || outer.contains (inner);
}

void testPureLayout (const Target& target)
{
    const juce::Rectangle<int> full { 0, 0, target.width, target.height };
    const auto shell = ChurchOrganLayout::calculateShell (full);
    const auto live = ChurchOrganLayout::calculateLive (
        { 0, 0, shell.content.getWidth(), shell.content.getHeight() },
        shell.sizeClass);

    const auto prefix = std::string (target.name) + ": ";
    expect (containedBy (shell.header, full), prefix + "header escaped viewport");
    expect (containedBy (shell.navigation, full), prefix + "navigation escaped viewport");
    expect (containedBy (shell.content, full), prefix + "content escaped viewport");
    expect (! shell.header.intersects (shell.navigation),
            prefix + "header overlaps navigation");
    expect (! shell.content.intersects (shell.navigation),
            prefix + "content overlaps navigation");
    expect (containedBy (live.drawbarCard, live.content),
            prefix + "drawbar card escaped Live content");
    expect (live.visibleCardCount >= 3,
            prefix + "too few immediate Live performance controls");

    if (shell.sizeClass == ChurchOrganLayout::SizeClass::compactPortrait
        || shell.sizeClass == ChurchOrganLayout::SizeClass::compactLandscape)
    {
        using LiveCard = ChurchOrganLayout::LiveCard;
        for (const auto essential : {
                 LiveCard::rotary,
                 LiveCard::worship,
                 LiveCard::drive,
                 LiveCard::reverb,
                 LiveCard::expression })
        {
            expect (
                live.visible[static_cast<size_t> (essential)],
                prefix + "essential compact Live control is hidden");
        }
    }

    for (int first = 0; first < ChurchOrganLayout::liveCardCount; ++first)
    {
        if (! live.visible[static_cast<size_t> (first)])
            continue;

        const auto firstBounds = live.cards[static_cast<size_t> (first)];
        expect (! firstBounds.isEmpty(), prefix + "visible card has empty bounds");
        expect (containedBy (firstBounds, live.content),
                prefix + "performance card escaped Live content");
        expect (! firstBounds.intersects (live.drawbarCard),
                prefix + "performance card overlaps drawbar card");

        for (int second = first + 1;
             second < ChurchOrganLayout::liveCardCount;
             ++second)
        {
            if (! live.visible[static_cast<size_t> (second)])
                continue;
            expect (! firstBounds.intersects (
                        live.cards[static_cast<size_t> (second)]),
                    prefix + "Live cards overlap");
        }
    }

    for (int page = static_cast<int> (ChurchOrganLayout::Page::drawbars);
         page < static_cast<int> (ChurchOrganLayout::Page::count);
         ++page)
    {
        expect (ChurchOrganLayout::preferredEditingPageHeight (
                    static_cast<ChurchOrganLayout::Page> (page),
                    shell.content.getWidth(),
                    shell.content.getHeight()) >= shell.content.getHeight(),
                prefix + "advanced page shorter than viewport");
    }
}

void testEditorGeometry (const Target& target,
                         ChurchOrganMockViewModel& model,
                         ChurchOrganEditor& editor)
{
    editor.setBounds (0, 0, target.width, target.height);
    editor.showPage (ChurchOrganLayout::Page::live);

    const auto shell = editor.getShellLayoutForTesting();
    const auto prefix = std::string (target.name) + ": ";
    expect (! editor.livePageUsesViewportForTesting(),
            prefix + "Live page is attached to a viewport");
    expect (editor.getActivePage() == ChurchOrganLayout::Page::live,
            prefix + "Live page did not activate");

    for (int index = 0; index < ChurchOrganLayout::pageCount; ++index)
    {
        const auto bounds = editor.getNavigationButtonBoundsForTesting (index);
        expect (bounds.getWidth() >= shell.minimumTarget,
                prefix + "navigation target narrower than minimum");
        expect (bounds.getHeight() >= shell.minimumTarget,
                prefix + "navigation target shorter than minimum");
        expect (containedBy (bounds, shell.full),
                prefix + "navigation target escaped editor");
    }

    for (int index = 0; index < 9; ++index)
    {
        const auto bounds = editor.getLiveDrawbarBoundsForTesting (index);
        expect (! bounds.isEmpty(), prefix + "drawbar has no geometry");
        expect (containedBy (bounds, shell.full),
                prefix + "drawbar escaped editor");

        // Compact portrait uses a 5+4 bank specifically to keep each harmonic
        // reachable by touch without introducing a Live-page viewport.
        if (target.width < 480
            || shell.sizeClass
                   == ChurchOrganLayout::SizeClass::compactLandscape)
        {
            expect (bounds.getWidth() >= 48,
                    prefix + "compact drawbar target narrower than 48 px");
            expect (bounds.getHeight() >= 48,
                    prefix + "compact drawbar target shorter than 48 px");
        }
    }

    for (int page = 0; page < ChurchOrganLayout::pageCount; ++page)
    {
        editor.showPage (static_cast<ChurchOrganLayout::Page> (page));
        expect (editor.getActivePage() == static_cast<ChurchOrganLayout::Page> (page),
                prefix + "page navigation state mismatch");
    }

    editor.showPage (ChurchOrganLayout::Page::live);
    (void) model;
}

juce::File screenshotDirectoryFromArguments (int argc, char* argv[])
{
    constexpr auto prefix = "--screenshots=";
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument (argv[index]);
        if (argument.rfind (prefix, 0) == 0)
            return juce::File (argument.substr (std::char_traits<char>::length (prefix)));
    }
    return {};
}

bool writeScreenshot (ChurchOrganEditor& editor,
                      const juce::File& directory,
                      const juce::String& name)
{
    if (directory == juce::File())
        return true;

    if (! directory.exists() && ! directory.createDirectory())
        return false;

    auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
    auto file = directory.getChildFile (name + ".png");
    if (file.existsAsFile() && ! file.deleteFile())
        return false;
    auto stream = file.createOutputStream();
    if (stream == nullptr)
        return false;

    juce::PNGImageFormat format;
    return format.writeImageToStream (image, *stream);
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;

    testScrollGesturesDoNotEditOrganControls();

    const auto screenshotDirectory = screenshotDirectoryFromArguments (argc, argv);
    ChurchOrganMockViewModel model;
    ChurchOrganEditor editor (model);

    for (const auto& target : targets)
    {
        testPureLayout (target);
        testEditorGeometry (target, model, editor);

        editor.setBounds (0, 0, target.width, target.height);
        editor.showPage (ChurchOrganLayout::Page::live);
        expect (writeScreenshot (
                    editor,
                    screenshotDirectory,
                    juce::String ("live-") + target.name),
                std::string (target.name) + ": could not write Live screenshot");
    }

    editor.setBounds (0, 0, 1366, 768);
    for (int page = 0; page < ChurchOrganLayout::pageCount; ++page)
    {
        const auto pageValue = static_cast<ChurchOrganLayout::Page> (page);
        editor.showPage (pageValue);
        expect (writeScreenshot (
                    editor,
                    screenshotDirectory,
                    "desktop-" + ChurchOrganLayout::pageName (pageValue).toLowerCase()),
                "desktop: could not write page screenshot");
    }

    if (failures != 0)
    {
        std::cerr << failures << " UI phase assertion(s) failed\n";
        return 1;
    }

    std::cout << "Church Organ UI geometry passed for 320x568, 393x873, "
                 "640x360, 800x1280 and 1366x768.\n";
    if (screenshotDirectory != juce::File())
        std::cout << "Screenshots: " << screenshotDirectory.getFullPathName() << '\n';
    return 0;
}
