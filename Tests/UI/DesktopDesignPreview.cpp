#include "MainComponent.h"
#include "TimbresProcessor.h"
#include "NordLookAndFeel.h"
#include "DesktopUpdater.h"

#include <filesystem>
#include <iostream>

int runDesktopShellTests();

#if ! defined (CHURCHDAW_UI_PREVIEW) || ! CHURCHDAW_UI_PREVIEW
 #error "DesktopDesignPreview requires the isolated visual-preview build."
#endif

// Manual inspection of the production MainComponent, in a separate application.
// Main.cpp, the updater and the licence client are deliberately not linked.
// The CHURCHDAW_UI_PREVIEW guards in MainComponent prevent audio/MIDI/network
// startup; no production setting or credential is required for this surface.
// LibraryPanel's inline catalog code references this verifier even though the
// preview never opens its online flow. Fail closed in this test-only executable
// rather than linking the production updater and its persistent state.
namespace ChurchDAWDesktopUpdate
{
bool verifySignedPolicyEnvelope (const juce::String&, const juce::String&,
                                  const juce::String&)
{
    return false;
}
}

namespace
{
juce::File canonicalDirectory (const juce::String& path)
{
    if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
        return {};

    std::error_code error;
    const auto resolved = std::filesystem::canonical (
        std::filesystem::u8path (path.toStdString()), error);
    if (error)
        return {};

    const auto utf8 = resolved.u8string();
    const juce::File result (juce::String::fromUTF8 (utf8.c_str()));
    return result.isDirectory() ? result : juce::File();
}

bool validateFixture()
{
    const auto settings = canonicalDirectory (
        juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_QA_SETTINGS_DIR", {}).trim());
    const auto data = canonicalDirectory (
        juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_QA_DATA_DIR", {}).trim());

    if (settings == juce::File() || data == juce::File() || settings == data
        || settings.getParentDirectory() != data.getParentDirectory())
    {
        std::cerr << "Preview requires existing, absolute QA_SETTINGS_DIR and QA_DATA_DIR "
                     "pointing to distinct sibling directories in an isolated fixture.\n";
        return false;
    }

    const auto root = settings.getParentDirectory();
    for (const auto protectedLocation : { juce::File::userHomeDirectory,
                                          juce::File::userApplicationDataDirectory,
                                          juce::File::commonApplicationDataDirectory,
                                          juce::File::globalApplicationsDirectory })
    {
        const auto protectedRoot = canonicalDirectory (
            juce::File::getSpecialLocation (protectedLocation).getFullPathName());
        if (protectedRoot != juce::File()
            && (root == protectedRoot
                || (protectedLocation != juce::File::userHomeDirectory
                    && root.isAChildOf (protectedRoot))))
        {
            std::cerr << "Preview fixture must stay outside application data and installed applications.\n";
            return false;
        }
    }

    const auto marker = root.getChildFile (".churchdaw-design-preview");
    if (! marker.existsAsFile() || marker.getSize() > 128
        || marker.loadFileAsString().trim() != "churchdaw-design-preview-v1")
    {
        std::cerr << "Preview fixture needs .churchdaw-design-preview containing "
                     "churchdaw-design-preview-v1. No application was started.\n";
        return false;
    }

    const auto selfTest = juce::SystemStats::getEnvironmentVariable (
        "CHURCHDAW_SELFTEST", {}).trim();
    if (selfTest.isNotEmpty() && ! selfTest.equalsIgnoreCase ("ui"))
    {
        std::cerr << "Audio self-test modes are not supported by this visual preview.\n";
        return false;
    }

    return true;
}

TimbresProcessor* findPreviewMixer (juce::Component& component)
{
    // The production processor is reached through JUCE's public editor API;
    // no private-access macro or production-only fixture hook is needed.
    if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (&component))
        if (auto* mixer = dynamic_cast<TimbresProcessor*> (editor->getAudioProcessor()))
            return mixer;

    for (int index = 0; index < component.getNumChildComponents(); ++index)
        if (auto* mixer = findPreviewMixer (*component.getChildComponent (index)))
            return mixer;

    return nullptr;
}

juce::Button* findVisibleButton (juce::Component& component, const juce::String& label)
{
    if (auto* button = dynamic_cast<juce::Button*> (&component))
        if (button->isShowing() && button->getButtonText().equalsIgnoreCase (label))
            return button;
    for (auto* child : component.getChildren())
        if (auto* button = findVisibleButton (*child, label))
            return button;
    return nullptr;
}

juce::ComboBox* findVisibleCombo (juce::Component& component, const juce::String& text)
{
    if (auto* combo = dynamic_cast<juce::ComboBox*> (&component))
        if (combo->isShowing() && combo->getText() == text)
            return combo;
    for (auto* child : component.getChildren())
        if (auto* combo = findVisibleCombo (*child, text))
            return combo;
    return nullptr;
}

juce::Component* findComponentById (juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return &component;
    for (auto* child : component.getChildren())
        if (auto* found = findComponentById (*child, id))
            return found;
    return nullptr;
}

bool validateDesktopMixerViewport (juce::Component& component)
{
    auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (&component);
    if (editor == nullptr || ! component.isShowing()
        || dynamic_cast<TimbresProcessor*> (editor->getAudioProcessor()) == nullptr
        || ! NordLookAndFeel::usesDesktopShell (component))
        return true;

    auto* viewport = component.findParentComponentOfClass<juce::Viewport>();
    if (viewport == nullptr || viewport->getViewedComponent() == nullptr)
    {
        std::cerr << "DESIGN_PREVIEW: native desktop mixer lost its viewport.\n";
        return false;
    }

    // Read logical component bounds, not screenshot dimensions: native window
    // borders and display scaling change the actual space available to the UI.
    if (component.getHeight() < 480
        || component.getWidth() != viewport->getMaximumVisibleWidth()
        || viewport->getHorizontalScrollBar().isVisible()
        || viewport->getViewPositionX() != 0)
    {
        std::cerr << "DESIGN_PREVIEW: desktop mixer does not fit its viewport: editor "
                  << component.getWidth() << "x" << component.getHeight()
                  << ", visible " << viewport->getMaximumVisibleWidth() << "x"
                  << viewport->getMaximumVisibleHeight() << "px\n";
        return false;
    }

    const auto editorBounds = viewport->getViewedComponent()->getLocalArea (
        &component, component.getLocalBounds());
    const bool needsVerticalScroll = editorBounds.getBottom()
                                  > viewport->getMaximumVisibleHeight();
    if (viewport->getVerticalScrollBar().isVisible() != needsVerticalScroll)
    {
        std::cerr << "DESIGN_PREVIEW: desktop mixer vertical overflow has no matching scrollbar.\n";
        return false;
    }

    // Exercise the real viewport and restore its position before the snapshot.
    // This verifies that the master controls remain reachable in the short
    // window and when opening MIDI mapping reduces the mixer height further.
    const auto originalPosition = viewport->getViewPosition();
    viewport->setViewPosition (0, juce::jmax (0, editorBounds.getBottom()
                                               - viewport->getMaximumVisibleHeight()));
    const bool bottomIsReachable = viewport->getViewArea().getBottom() >= editorBounds.getBottom();
    viewport->setViewPosition (originalPosition);
    if (! bottomIsReachable)
        std::cerr << "DESIGN_PREVIEW: the bottom of the native mixer cannot be reached.\n";
    return bottomIsReachable;
}

bool validateFaderTravel (juce::Component& component)
{
    if (! validateDesktopMixerViewport (component))
        return false;
    if (component.isShowing() && component.getComponentID() == "channelFader"
        && NordLookAndFeel::usesDesktopShell (component) && component.getHeight() < 100)
    {
        std::cerr << "DESIGN_PREVIEW: channel fader lost usable travel: "
                  << component.getHeight() << "px\n";
        return false;
    }
    if (component.isShowing() && component.getComponentID() == "masterFader"
        && NordLookAndFeel::usesDesktopShell (component) && component.getHeight() < 120)
    {
        std::cerr << "DESIGN_PREVIEW: master fader lost usable travel: "
                  << component.getHeight() << "px\n";
        return false;
    }
    for (int index = 0; index < component.getNumChildComponents(); ++index)
        if (! validateFaderTravel (*component.getChildComponent (index)))
            return false;
    return true;
}

bool loadShowcase (juce::Component& root)
{
    auto* mixer = findPreviewMixer (root);
    if (mixer == nullptr)
        return false;

    // These are real built-in engines, prepared/restored through their normal
    // production API. No samples, external plugins, MIDI events or fake meters.
    // The executable only starts after validateFixture accepted QA directories.
    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
        mixer->clearLayer (layer);

    const std::array<churchorgan::ChurchOrganFamily, 3> families {
        churchorgan::ChurchOrganFamily::tonewheel,
        churchorgan::ChurchOrganFamily::worshipHybrid,
        churchorgan::ChurchOrganFamily::pipe
    };
    for (size_t layer = 0; layer < families.size(); ++layer)
        if (! mixer->loadChurchOrganIntoLayer (static_cast<int> (layer), families[layer]))
            return false;

    mixer->setSelectedLayer (0);
    mixer->sendChangeMessage();
    std::cout << "DESIGN_PREVIEW: SHOWCASE loaded three native Church Organ families; "
                 "remaining channels empty; audio and MIDI remain disabled.\n";
    return true;
}

juce::Point<int> requestedSize (const juce::String& arguments)
{
    juce::String size = "1280x800";
    const auto tokens = juce::StringArray::fromTokens (arguments, true);
    for (int index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index].startsWith ("--size="))
            size = tokens[index].fromFirstOccurrenceOf ("=", false, false);
        else if (tokens[index] == "--size" && index + 1 < tokens.size())
            size = tokens[++index];
    }

    const auto parts = juce::StringArray::fromTokens (size.toLowerCase(), "x", {});
    if (parts.size() != 2
        || ! parts[0].containsOnly ("0123456789")
        || ! parts[1].containsOnly ("0123456789")
        || parts[0].getIntValue() < 640 || parts[0].getIntValue() > 3840
        || parts[1].getIntValue() < 400 || parts[1].getIntValue() > 2160)
        return {};

    return { parts[0].getIntValue(), parts[1].getIntValue() };
}

juce::String optionValue (const juce::String& arguments, const juce::String& option)
{
    const auto tokens = juce::StringArray::fromTokens (arguments, true);
    for (int index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index].startsWith (option + "="))
            return tokens[index].fromFirstOccurrenceOf ("=", false, false).unquoted();
        if (tokens[index] == option && index + 1 < tokens.size())
            return tokens[index + 1].unquoted();
    }
    return {};
}
}

class DesktopDesignPreview final : public juce::JUCEApplication,
                                   private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "ChurchDAW Design Preview"; }
    const juce::String getApplicationVersion() override { return "0.1"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& arguments) override
    {
        const auto size = requestedSize (arguments);
        if (! validateFixture() || size.isOrigin())
        {
            if (size.isOrigin())
                std::cerr << "Use --size=WIDTHxHEIGHT within 640x400 and 3840x2160.\n";
            setApplicationReturnValue (2);
            quit();
            return;
        }

        juce::Logger::setCurrentLogger (&logger);
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        NordLookAndFeel::installPortugueseStrings();

        const auto tokens = juce::StringArray::fromTokens (arguments, true);
        surface = optionValue (arguments, "--surface");
        if (surface.isNotEmpty()
            && surface != "audio" && surface != "plugins" && surface != "libraries"
            && surface != "midi" && surface != "pad" && surface != "picker"
            && surface != "master-menu" && surface != "scenes")
        {
            std::cerr << "Unknown --surface; use audio, plugins, libraries, midi, pad, picker, scenes or master-menu.\n";
            setApplicationReturnValue (2);
            quit();
            return;
        }
        // --demo-scenes: cenas de exemplo em qualquer tela (ex.: a faixa do modo Live)
        demoScenes = surface == "scenes" || tokens.contains ("--demo-scenes");
        if (demoScenes)
            writeDemoScenes();
        if (tokens.contains ("--test-shell"))
        {
            const int result = runDesktopShellTests();
            setApplicationReturnValue (result);
            if (result != 0 || ! arguments.contains ("--capture-dir"))
            {
                quit();
                return;
            }
        }

        if (arguments.contains ("--capture-dir"))
        {
            captureDirectory = canonicalDirectory (optionValue (arguments, "--capture-dir"));
            const auto fixtureRoot = canonicalDirectory (
                juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_QA_SETTINGS_DIR", {}))
                    .getParentDirectory();
            if (captureDirectory == juce::File() || ! captureDirectory.isAChildOf (fixtureRoot))
            {
                std::cerr << "--capture-dir must be an existing directory inside the isolated fixture.\n";
                setApplicationReturnValue (2);
                quit();
                return;
            }
        }

        std::cout << "DESIGN_PREVIEW: isolated UI; no licence client, updater or audio hardware.\n";
        window = std::make_unique<Window> (size, teardownGate);
        showcaseRequested = tokens.contains ("--showcase") || surface == "master-menu";
        if (captureDirectory != juce::File())
        {
            if (surface.isEmpty())
                window->setSize (captureSizes[0].x, captureSizes[0].y);
            startTimer (350);
        }
        else if (showcaseRequested)
        {
            startTimer (350);
        }
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (window != nullptr)
            window->toFront (true);
    }

    void systemRequestedQuit() override
    {
        if (window == nullptr)
        {
            quit();
            return;
        }

        if (closing)
            return;
        closing = true;
        if (surface == "master-menu")
            juce::PopupMenu::dismissAllActiveMenus();
        (void) teardownGate.requestClose();
        window->holder.inner.beginOwnerTeardown();
        startTimer (10);
    }

    void shutdown() override
    {
        stopTimer();
        window.reset();
        if (demoScenes)
        {
            // o fixture volta a nao ter cenas
            demoScenesFile().deleteFile();
            demoScenesFile().getSiblingFile ("live-scene-pages").deleteRecursively();
        }
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        if (juce::Logger::getCurrentLogger() == &logger)
            juce::Logger::setCurrentLogger (nullptr);
    }

private:
    static ScenesPage* findScenesPage (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* page = dynamic_cast<ScenesPage*> (child))
                return page;
            if (auto* found = findScenesPage (*child))
                return found;
        }
        return nullptr;
    }

    static juce::File demoScenesFile()
    {
        return juce::File (juce::SystemStats::getEnvironmentVariable ("CHURCHDAW_QA_SETTINGS_DIR", {}).trim())
                   .getChildFile ("live-scenes.xml");
    }

    // Cenas de exemplo SO para a captura --surface scenes (o fixture nao tem cenas).
    static void writeDemoScenes()
    {
        struct Canal { const char* timbre; float volume; };
        struct Demo
        {
            const char* musica; const char* secao; const char* tom; double bpm; int pad;
            std::vector<Canal> canais;
        };
        const std::vector<Demo> demos {
            { "Aleluia", "Refr\xc3\xa3o", "G", 72.0, 7, { { "Pearl Upright", 0.667f }, { "Pad2", 0.60f }, { "Prophetic Ambience", 0.45f } } },
            { "Santo", "Verso", "D", 68.0, 2, { { "Pearl Upright", 0.667f }, { "Pad Lp", 0.52f } } },
            { "Grande \xc3\xa9 o Senhor", "Intro", "A", 84.0, 9, { { "Pearl Upright", 0.60f }, { "Pad2", 0.55f }, { "Pad Lp", 0.40f }, { "Prophetic Ambience", 0.35f }, { "String Ensemble", 0.50f } } },
            { "Ao Deus que Adoro", "Cena", "E", 76.0, 4, { { "Pearl Upright", 0.667f } } },
            { "Tua Gra\xc3\xa7\x61 me Basta", "Ponte", "C", 64.0, -1, { { "Pearl Upright", 0.667f }, { "Pad2", 0.50f } } },
        };

        juce::XmlElement xml ("LIVESCENES");
        xml.setAttribute ("schema", 3);
        xml.setAttribute ("active", 0);

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            ChurchDAWLive::Cena cena;
            if (i < (int) demos.size())
            {
                const auto& d = demos[(size_t) i];
                cena.preenchida = true;
                cena.musica = juce::String::fromUTF8 (d.musica);
                cena.secao = juce::String::fromUTF8 (d.secao);
                cena.tonalidade = d.tom;
                cena.bpm = d.bpm;
                cena.padTom = d.pad;
                cena.gravadaEm = "2026-11-05T10:00:00";
                for (size_t c = 0; c < d.canais.size(); ++c)
                {
                    cena.canais[c].rotuloDoTimbre = d.canais[c].timbre;
                    cena.canais[c].volume = d.canais[c].volume;
                }
            }
            ChurchDAWLive::escreverCena (xml, cena, i);
        }

        ChurchDAWLive::Setlist setlist;
        ChurchDAWLive::escreverSetlist (xml, setlist);
        (void) xml.writeTo (demoScenesFile());
    }

    struct StdoutLogger final : juce::Logger
    {
        void logMessage (const juce::String& message) override
        {
            std::cout << message << std::endl;
        }
    };

    struct ScaledHolder final : juce::Component
    {
        explicit ScaledHolder (ChurchDAWMidiMapping::DeferredOwnerTeardownGate& gate)
            : inner (true, &gate)
        {
            addAndMakeVisible (inner);
        }

        void resized() override
        {
            const float scale = juce::jmax (1.0f, juce::jmin (getWidth() / 1280.0f,
                                                             getHeight() / 672.0f));
            inner.setTransform (juce::AffineTransform::scale (scale));
            inner.setBounds (0, 0, static_cast<int> (std::ceil (getWidth() / scale)),
                                   static_cast<int> (std::ceil (getHeight() / scale)));
        }

        MainComponent inner;
    };

    struct Window final : juce::DocumentWindow
    {
        Window (juce::Point<int> size, ChurchDAWMidiMapping::DeferredOwnerTeardownGate& gate)
            : DocumentWindow ("ChurchDAW Design Preview", NordLookAndFeel::background(),
                              juce::DocumentWindow::allButtons), holder (gate)
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (30);
            setTitleBarTextCentred (false);
            setContentNonOwned (&holder, false);
            setResizable (true, true);
            setResizeLimits (640, 400, 3840, 2160);
            centreWithSize (size.x, size.y);
            setVisible (true);
            toFront (true);
            std::cout << "DESIGN_PREVIEW: window=" << getWidth() << "x" << getHeight()
                      << " content=" << holder.getWidth() << "x" << holder.getHeight() << '\n';
        }

        ~Window() override { clearContentComponent(); }
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        ScaledHolder holder;
    };

    void timerCallback() override
    {
        if (closing)
        {
            if (window != nullptr && teardownGate.isClosedAndDrained()
                && window->holder.inner.ownerTeardownIsReady())
            {
                stopTimer();
                window.reset();
                std::cout << "DESIGN_PREVIEW: CLOSED_CLEANLY\n";
                quit();
            }
            return;
        }

        if (window == nullptr
            || (captureDirectory == juce::File() && (! showcaseRequested || showcaseLoaded)))
            return;

        if (window->holder.inner.isStillLoading())
        {
            if (++captureWaitTicks < 80)
                return;
            std::cerr << "DESIGN_PREVIEW: initial UI did not settle within 28 seconds.\n";
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        if (showcaseRequested && ! showcaseLoaded)
        {
            if (! loadShowcase (window->holder.inner))
            {
                std::cerr << "DESIGN_PREVIEW: native showcase could not be loaded.\n";
                setApplicationReturnValue (1);
                systemRequestedQuit();
                return;
            }
            showcaseLoaded = true;
            // Let the processor's ordinary asynchronous UI notifications settle
            // before reading component geometry or capturing a bitmap.
            window->holder.inner.repaint();
            if (captureDirectory == juce::File())
                stopTimer();
            return;
        }

        if (! validateFaderTravel (window->holder.inner))
        {
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        if (surface == "master-menu" && ! surfaceOpened)
        {
            if (auto* combo = findVisibleCombo (window->holder.inner, "Cloud"))
            {
                auto& desktop = juce::Desktop::getInstance();
                for (int index = 0; index < desktop.getNumComponents(); ++index)
                    desktopBeforePopup.add (desktop.getComponent (index));
                // A CLI-launched macOS GUI may not own foreground focus.
                // Request it explicitly, then capture synchronously: JUCE
                // builds the real popup inside showPopup(), while its later
                // focus timer can dismiss a background menu within 10 ms.
                juce::Process::makeForegroundProcess();
                window->toFront (true);
                std::cout << "DESIGN_PREVIEW: master selector enabled=" << combo->isEnabled()
                          << " items=" << combo->getNumItems()
                          << " foreground=" << juce::Process::isForegroundProcess() << '\n';
                combo->showPopup();
                surfaceOpened = true;
            }
            else
            {
                std::cerr << "DESIGN_PREVIEW: visible master Cloud selector was not found.\n";
                setApplicationReturnValue (1);
                systemRequestedQuit();
                return;
            }
        }

        if (surface.isNotEmpty() && ! surfaceOpened)
        {
            const auto label = surface == "audio" ? juce::String::fromUTF8 ("Áudio")
                             : surface == "plugins" ? juce::String ("Plugins")
                             : surface == "libraries" ? juce::String ("Bibliotecas")
                             : surface == "midi" ? juce::String ("Mapear MIDI")
                             : surface == "scenes" ? juce::String ("Cenas")
                             : surface == "pad" ? juce::String ("Pad") : juce::String ("Adicionar");
            if (auto* button = findVisibleButton (window->holder.inner, label))
            {
                button->triggerClick();
                surfaceOpened = true;
                if (surface == "scenes")
                    if (auto* page = findScenesPage (window->holder.inner))
                    {
                        const auto args = juce::JUCEApplication::getCommandLineParameters();
                        // --load-scene=N passa pelo MESMO callback do toque na lista (motor real).
                        if (optionValue (args, "--load-scene").isNotEmpty())
                        {
                            const int n = optionValue (args, "--load-scene").getIntValue();
                            const auto r = page->onCarregar != nullptr ? page->onCarregar (n) : ChurchDAWLive::Resultado::indisponivel;
                            std::cout << "DESIGN_PREVIEW: load-scene " << n << " resultado=" << (int) r << '\n';
                        }
                        if (optionValue (args, "--select-scene").isNotEmpty())
                            page->selecionar (optionValue (args, "--select-scene").getIntValue());
                    }
                // These use the same callbacks as the UI, with the ordinary
                // async open/resize allowed to finish before the snapshot.
                startTimer (700);
                return;
            }
            std::cerr << "DESIGN_PREVIEW: could not open surface " << surface << '\n';
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        juce::Component* captureSource = window.get();
        if (surface == "plugins")
        {
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
                if (auto* dialog = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)))
                    if (dialog->isVisible())
                        captureSource = dialog;
            if (captureSource == window.get())
            {
                std::cerr << "DESIGN_PREVIEW: plugins dialog did not open.\n";
                setApplicationReturnValue (1);
                systemRequestedQuit();
                return;
            }
        }
        if (surface == "master-menu")
        {
            // Modal registration works for both detached and parented menus.
            // No message-loop yield occurs between showPopup() and snapshot.
            if (auto* modal = juce::Component::getCurrentlyModalComponent())
            {
                std::cout << "DESIGN_PREVIEW: immediate popup name=" << modal->getName()
                          << " visible=" << modal->isVisible()
                          << " desktop=" << modal->isOnDesktop() << '\n';
                if (modal->isVisible() && modal->getName() == "menu")
                    captureSource = modal;
            }
            auto& desktop = juce::Desktop::getInstance();
            for (int index = 0; index < desktop.getNumComponents(); ++index)
            {
                auto* component = desktop.getComponent (index);
                // JUCE MenuWindow is a detached desktop component named menu;
                // capture it directly because main-window snapshots omit it.
                if (component->isVisible() && component->getName() == "menu"
                    && ! desktopBeforePopup.contains (component))
                    captureSource = component;
            }
            if (captureSource == window.get())
            {
                std::cerr << "DESIGN_PREVIEW: master selector popup did not open.\n";
                setApplicationReturnValue (1);
                systemRequestedQuit();
                return;
            }
        }
        const auto image = captureSource->createComponentSnapshot (captureSource->getLocalBounds(), true, 1.0f);
        const auto dimensions = juce::String (captureSource->getWidth()) + "x" + juce::String (captureSource->getHeight());
        const auto output = captureDirectory.getChildFile (
            (surface.isNotEmpty() ? surface + "-" : juce::String (showcaseRequested ? "showcase-" : "desktop-"))
                + dimensions + ".png");
        bool written = false;
        {
            juce::FileOutputStream stream (output);
            if (stream.openedOk() && image.isValid())
            {
                stream.setPosition (0);
                stream.truncate();
                written = juce::PNGImageFormat().writeImageToStream (image, stream);
                stream.flush();
            }
        }
        if (! written)
        {
            std::cerr << "DESIGN_PREVIEW: failed to save snapshot " << dimensions << '\n';
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        std::cout << "DESIGN_PREVIEW: CAPTURED " << output.getFullPathName() << '\n';
        if (surface.isNotEmpty() || ++captureIndex == captureSizes.size())
        {
            systemRequestedQuit();
            return;
        }

        window->setSize (captureSizes[captureIndex].x, captureSizes[captureIndex].y);
        captureWaitTicks = 0;
    }

    StdoutLogger logger;
    NordLookAndFeel lookAndFeel;
    ChurchDAWMidiMapping::DeferredOwnerTeardownGate teardownGate;
    std::unique_ptr<Window> window;
    const std::array<juce::Point<int>, 8> captureSizes { {
        { 902, 620 }, { 960, 640 }, { 1002, 704 }, { 1180, 800 },
        { 1182, 704 }, { 1280, 704 }, { 1280, 800 }, { 1600, 900 }
    } };
    juce::File captureDirectory;
    juce::String surface;
    bool demoScenes = false;
    juce::Array<juce::Component::SafePointer<juce::Component>> desktopBeforePopup;
    size_t captureIndex = 0;
    int captureWaitTicks = 0;
    bool closing = false;
    bool showcaseRequested = false;
    bool showcaseLoaded = false;
    bool surfaceOpened = false;
};

START_JUCE_APPLICATION (DesktopDesignPreview)
