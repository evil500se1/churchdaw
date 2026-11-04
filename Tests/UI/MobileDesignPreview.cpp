#include "MainComponent.h"
#include "TimbresProcessor.h"
#include "NordLookAndFeel.h"
#include "DesktopUpdater.h"
#include "DesktopUpdatePublicKey.h"
#include "Studio/StudioComposition.h"

#include <juce_cryptography/juce_cryptography.h>
#include <filesystem>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <bcrypt.h>
 #include <wincrypt.h>
#elif JUCE_MAC
 #include <Security/Security.h>
#endif

#if ! defined (CHURCHDAW_UI_PREVIEW) || ! CHURCHDAW_UI_PREVIEW \
    || ! defined (CHURCHDAW_MOBILE_UI_PREVIEW) || ! CHURCHDAW_MOBILE_UI_PREVIEW
 #error "MobileDesignPreview requires CHURCHDAW_UI_PREVIEW=1 and CHURCHDAW_MOBILE_UI_PREVIEW=1."
#endif

namespace ChurchDAWDesktopUpdate
{
bool verifySignedPolicyEnvelope (const juce::String& payloadBase64,
                                 const juce::String& signatureBase64,
                                 const juce::String& keyId)
{
    const char* publicKeyBase64 = nullptr;
    if (keyId == juce::String (policyKeyId202611))
        publicKeyBase64 = updatePolicyPublicKeyDerBase64_2026_11;
    else if (keyId == juce::String (policyKeyIdCurrent))
        publicKeyBase64 = updatePolicyPublicKeyDerBase64_2026_08;
    else if (keyId == juce::String (policyKeyId))
        publicKeyBase64 = updatePolicyPublicKeyDerBase64;
    else
        return false;

    juce::MemoryOutputStream decodedPayload, decodedSignature, decodedKey;
    if (! juce::Base64::convertFromBase64 (decodedPayload, payloadBase64)
        || decodedPayload.getDataSize() == 0
        || ! juce::Base64::convertFromBase64 (decodedSignature, signatureBase64)
        || decodedSignature.getDataSize() != (size_t) signatureBytes
        || ! juce::Base64::convertFromBase64 (decodedKey, publicKeyBase64))
        return false;

    const juce::MemoryBlock message (decodedPayload.getData(), decodedPayload.getDataSize());

   #if JUCE_WINDOWS
    CERT_PUBLIC_KEY_INFO* publicKeyInfo = nullptr;
    DWORD publicKeyInfoBytes = 0;
    if (CryptDecodeObjectEx (X509_ASN_ENCODING,
                             X509_PUBLIC_KEY_INFO,
                             static_cast<const BYTE*> (decodedKey.getData()),
                             static_cast<DWORD> (decodedKey.getDataSize()),
                             CRYPT_DECODE_ALLOC_FLAG,
                             nullptr,
                             &publicKeyInfo,
                             &publicKeyInfoBytes) == FALSE)
        return false;

    BCRYPT_KEY_HANDLE key = nullptr;
    const auto imported = CryptImportPublicKeyInfoEx2 (
        X509_ASN_ENCODING, publicKeyInfo, 0, nullptr, &key);
    LocalFree (publicKeyInfo);
    if (imported == FALSE || key == nullptr)
        return false;

    const auto digest = juce::SHA256 (message).getRawData();
    BCRYPT_PKCS1_PADDING_INFO padding { BCRYPT_SHA256_ALGORITHM };
    const auto status = BCryptVerifySignature (
        key,
        &padding,
        reinterpret_cast<PUCHAR> (const_cast<void*> (digest.getData())),
        static_cast<ULONG> (digest.getSize()),
        reinterpret_cast<PUCHAR> (const_cast<void*> (decodedSignature.getData())),
        static_cast<ULONG> (decodedSignature.getDataSize()),
        BCRYPT_PAD_PKCS1);
    BCryptDestroyKey (key);
    return status >= 0;
   #elif JUCE_MAC
    auto release = [] (CFTypeRef r) { if (r != nullptr) CFRelease (r); };
    CFDataRef keyData = CFDataCreate (nullptr,
                                      static_cast<const UInt8*> (decodedKey.getData()),
                                      static_cast<CFIndex> (decodedKey.getDataSize()));
    if (keyData == nullptr)
        return false;
    const int keyBits = 3072;
    CFNumberRef bits = CFNumberCreate (nullptr, kCFNumberIntType, &keyBits);
    const void* attrKeys[]   { kSecAttrKeyType,    kSecAttrKeyClass,       kSecAttrKeySizeInBits };
    const void* attrValues[] { kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic, bits };
    CFDictionaryRef attributes = CFDictionaryCreate (nullptr, attrKeys, attrValues, 3,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
    CFErrorRef err = nullptr;
    SecKeyRef key = SecKeyCreateWithData (keyData, attributes, &err);
    release (attributes);
    release (bits);
    release (keyData);
    release (err);
    if (key == nullptr)
        return false;
    CFDataRef msgData = CFDataCreate (nullptr, static_cast<const UInt8*> (message.getData()),
                                      static_cast<CFIndex> (message.getSize()));
    CFDataRef sigData = CFDataCreate (nullptr, static_cast<const UInt8*> (decodedSignature.getData()),
                                      static_cast<CFIndex> (decodedSignature.getDataSize()));
    CFErrorRef verifyErr = nullptr;
    const bool valid = (msgData != nullptr && sigData != nullptr)
                    && SecKeyVerifySignature (key, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                                              msgData, sigData, &verifyErr);
    release (verifyErr);
    release (sigData);
    release (msgData);
    release (key);
    return valid;
   #else
    juce::ignoreUnused (message);
    return false;
   #endif
}
} // namespace ChurchDAWDesktopUpdate

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
        std::cerr << "MobilePreview requires existing, absolute QA_SETTINGS_DIR and QA_DATA_DIR "
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
            std::cerr << "MobilePreview fixture must stay outside application data and installed applications.\n";
            return false;
        }
    }

    const auto marker = root.getChildFile (".churchdaw-design-preview");
    if (! marker.existsAsFile() || marker.getSize() > 128
        || marker.loadFileAsString().trim() != "churchdaw-design-preview-v1")
    {
        std::cerr << "MobilePreview fixture needs .churchdaw-design-preview containing "
                     "churchdaw-design-preview-v1.\n";
        return false;
    }

    return true;
}

TimbresEditor* findTimbresEditor (juce::Component& component)
{
    if (auto* editor = dynamic_cast<TimbresEditor*> (&component))
        return editor;
    for (int i = 0; i < component.getNumChildComponents(); ++i)
        if (auto* editor = findTimbresEditor (*component.getChildComponent (i)))
            return editor;
    return nullptr;
}

TimbresProcessor* findPreviewMixer (juce::Component& component)
{
    if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (&component))
        if (auto* mixer = dynamic_cast<TimbresProcessor*> (editor->getAudioProcessor()))
            return mixer;
    for (int i = 0; i < component.getNumChildComponents(); ++i)
        if (auto* mixer = findPreviewMixer (*component.getChildComponent (i)))
            return mixer;
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

juce::Button* findButtonById (juce::Component& component, const juce::String& id)
{
    return dynamic_cast<juce::Button*> (findComponentById (component, id));
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

void collectCombos (juce::Component& component, std::vector<juce::ComboBox*>& out)
{
    if (auto* combo = dynamic_cast<juce::ComboBox*> (&component))
        if (combo->isVisible())
            out.push_back (combo);
    for (auto* child : component.getChildren())
        collectCombos (*child, out);
}

void collectLinearVerticalSliders (juce::Component& component, std::vector<juce::Slider*>& out)
{
    if (auto* slider = dynamic_cast<juce::Slider*> (&component))
        if (slider->getSliderStyle() == juce::Slider::LinearVertical)
            out.push_back (slider);
    for (auto* child : component.getChildren())
        collectLinearVerticalSliders (*child, out);
}

void applyAndroidAudioSelectorLabels (juce::Component& root)
{
    std::vector<juce::ComboBox*> combos;
    collectCombos (root, combos);
    for (auto* combo : combos)
    {
        const auto text = combo->getText().trim();
        if (text.containsIgnoreCase ("Windows Audio") || text.containsIgnoreCase ("DirectSound")
            || text.containsIgnoreCase ("ASIO"))
        {
            combo->setText (juce::String::fromUTF8 ("Oboe · Baixa latência"), juce::dontSendNotification);
        }
        else if (text.contains ("<<") || text.isEmpty())
        {
            combo->setText (juce::String::fromUTF8 ("Saída USB-C / Alto-falante"), juce::dontSendNotification);
        }
    }
}

void applyAndroidStudioLabels (juce::Component& studioRoot)
{
    std::vector<juce::ComboBox*> combos;
    collectCombos (studioRoot, combos);
    for (auto* combo : combos)
    {
        const auto text = combo->getText().trim();
        if (text.containsIgnoreCase ("WebCam") || text.containsIgnoreCase ("Conecte uma webcam"))
            combo->setText (juce::String::fromUTF8 ("Câmera frontal · 1080p"), juce::dontSendNotification);
    }
}

bool configureMobileShowcase (juce::Component& root)
{
    auto* mixer = findPreviewMixer (root);
    if (mixer == nullptr)
        return false;

    mixer->loadChurchOrganIntoLayer (0, churchorgan::ChurchOrganFamily::tonewheel);
    mixer->loadChurchOrganIntoLayer (1, churchorgan::ChurchOrganFamily::worshipHybrid);
    mixer->loadChurchOrganIntoLayer (2, churchorgan::ChurchOrganFamily::pipe);
    mixer->loadChurchOrganIntoLayer (3, churchorgan::ChurchOrganFamily::tonewheel);

    // Select indexed sample timbres into Channels 1 & 2 when available so the
    // showcase combines real sample layers + native ChurchOrgan engines.
    if (auto* editor = findTimbresEditor (root))
    {
        std::vector<juce::ComboBox*> combos;
        collectCombos (*editor, combos);
        std::vector<juce::ComboBox*> timbreCombos;
        for (auto* c : combos)
            if (c->getNumItems() >= 4 && (c->getText().containsIgnoreCase ("Organ")
                || c->getText().containsIgnoreCase ("Worship") || c->getText().containsIgnoreCase ("vazio")))
                timbreCombos.push_back (c);

        auto pickItemContaining = [] (juce::ComboBox* box, const char* token) -> bool
        {
            if (box == nullptr)
                return false;
            for (int i = 0; i < box->getNumItems(); ++i)
            {
                if (box->getItemText (i).containsIgnoreCase (token))
                {
                    box->setSelectedItemIndex (i, juce::sendNotificationSync);
                    return true;
                }
            }
            return false;
        };

        if (timbreCombos.size() >= 4)
        {
            pickItemContaining (timbreCombos[0], "Pad Lp");
            pickItemContaining (timbreCombos[1], "Pad Worship");
            pickItemContaining (timbreCombos[3], "Bell");
        }

        // Stagger fader positions naturally for a realistic live worship mix.
        std::vector<juce::Slider*> faders;
        collectLinearVerticalSliders (*editor, faders);
        const double dbLevels[] = { -1.4, -4.6, -3.2, -6.8 };
        for (size_t i = 0; i < std::min<size_t> (4, faders.size()); ++i)
        {
            const auto range = faders[i]->getRange();
            if (range.getStart() <= -24.0 && range.getEnd() >= 0.0)
                faders[i]->setValue (dbLevels[i], juce::sendNotificationSync);
        }
    }

    mixer->setSelectedLayer (0);
    mixer->sendChangeMessage();
    return true;
}

juce::Point<int> requestedSize (const juce::String& arguments)
{
    juce::String size = "884x390";
    const auto tokens = juce::StringArray::fromTokens (arguments, true);
    for (int i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].startsWith ("--size="))
            size = tokens[i].fromFirstOccurrenceOf ("=", false, false);
        else if (tokens[i] == "--size" && i + 1 < tokens.size())
            size = tokens[++i];
    }

    const auto parts = juce::StringArray::fromTokens (size.toLowerCase(), "x", {});
    if (parts.size() != 2
        || ! parts[0].containsOnly ("0123456789")
        || ! parts[1].containsOnly ("0123456789")
        || parts[0].getIntValue() < 320 || parts[0].getIntValue() > 2560
        || parts[1].getIntValue() < 320 || parts[1].getIntValue() > 1600)
        return {};

    return { parts[0].getIntValue(), parts[1].getIntValue() };
}

juce::String optionValue (const juce::String& arguments, const juce::String& option)
{
    const auto tokens = juce::StringArray::fromTokens (arguments, true);
    for (int i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i].startsWith (option + "="))
            return tokens[i].fromFirstOccurrenceOf ("=", false, false).unquoted();
        if (tokens[i] == option && i + 1 < tokens.size())
            return tokens[i + 1].unquoted();
    }
    return {};
}
} // namespace

class MobileDesignPreview final : public juce::JUCEApplication,
                                  private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "ChurchDAW Mobile Preview"; }
    const juce::String getApplicationVersion() override { return "1.6.35"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String& arguments) override
    {
        targetSize = requestedSize (arguments);
        if (! validateFixture() || targetSize.isOrigin())
        {
            setApplicationReturnValue (2);
            quit();
            return;
        }

        juce::Logger::setCurrentLogger (&logger);
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        NordLookAndFeel::installPortugueseStrings();

        const auto tokens = juce::StringArray::fromTokens (arguments, true);
        captureAll = tokens.contains ("--capture-all");

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

        window = std::make_unique<Window> (targetSize, teardownGate, captureAll);
        if (auto* mixer = findPreviewMixer (window->holder.inner))
            mixer->rescanLibrary();
        startTimer (550);
    }

    void closeAuxiliaryWindows()
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = desktop.getNumComponents() - 1; i >= 0; --i)
        {
            if (auto* doc = dynamic_cast<juce::DocumentWindow*> (desktop.getComponent (i)))
                if (doc != window.get())
                    delete doc;
        }
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
        closeTicks = 0;
        juce::PopupMenu::dismissAllActiveMenus();
        closeAuxiliaryWindows();
        int guard = 0;
        while (window->holder.inner.handleBackButton() && ++guard < 12) {}
        (void) teardownGate.requestClose();
        window->holder.inner.beginOwnerTeardown();
        startTimer (20);
    }

    void shutdown() override
    {
        stopTimer();
        closeAuxiliaryWindows();
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        if (juce::Logger::getCurrentLogger() == &logger)
            juce::Logger::setCurrentLogger (nullptr);
    }

private:
    struct StdoutLogger final : juce::Logger
    {
        void logMessage (const juce::String& message) override
        {
            std::cout << message << std::endl;
        }
    };

    struct MobileHolder final : juce::Component
    {
        explicit MobileHolder (ChurchDAWMidiMapping::DeferredOwnerTeardownGate& gate)
            : inner (true, &gate)
        {
            addAndMakeVisible (inner);
        }

        void resized() override
        {
            inner.setBounds (getLocalBounds());
        }

        MainComponent inner;
    };

    struct Window final : juce::DocumentWindow
    {
        Window (juce::Point<int> contentSize,
                ChurchDAWMidiMapping::DeferredOwnerTeardownGate& gate,
                bool topLeftPlacement)
            : DocumentWindow ("ChurchDAW Android Designer Preview (1.6.35)",
                              NordLookAndFeel::background(),
                              juce::DocumentWindow::allButtons),
              holder (gate)
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (28);
            setTitleBarTextCentred (false);
            setContentNonOwned (&holder, true);
            holder.setSize (contentSize.x, contentSize.y);
            setContentNonOwned (&holder, true);
            setResizable (true, true);
            setResizeLimits (360, 320, 2560, 1600);
            if (topLeftPlacement)
                setBounds (24, 24, contentSize.x, contentSize.y + getTitleBarHeight());
            else
                centreWithSize (contentSize.x, contentSize.y + getTitleBarHeight());
            setVisible (true);
            toFront (! topLeftPlacement);
        }

        ~Window() override { clearContentComponent(); }
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        MobileHolder holder;
    };

    enum class Step
    {
        palcoMixer = 0,
        editarCanal,
        seletorTimbres,
        cenasLive,
        padContinuo,
        mapearMidi,
        biblioteca,
        audioMidi,
        churchCam,
        done
    };

    bool saveSnapshot (juce::Component& target, const juce::String& filename, bool compositeChurchCam = false)
    {
        juce::Desktop::setMousePosition ({ 1880, 1040 });
        target.repaint();
        constexpr float kSnapshotScale = 2.75f;
        auto image = target.createComponentSnapshot (target.getLocalBounds(), true, kSnapshotScale);
        if (! image.isValid())
            return false;

        if (compositeChurchCam && stageMixerImage.isValid())
        {
            if (auto* preview = findComponentById (target, "liveCamKeyboard"))
            {
                const auto previewBounds = target.getLocalArea (preview, preview->getLocalBounds());
                churchstudio::VisualState visualState;
                visualState.performanceMode.store (false);
                visualState.cameraOn.store (false);
                visualState.dawImage = stageMixerImage;
                const auto brand = juce::ImageCache::getFromMemory (
                    BinaryData::logo_round_png, BinaryData::logo_round_pngSize);

                juce::Image composed (juce::Image::ARGB, 1280, 720, true);
                {
                    juce::Graphics cg (composed);
                    churchstudio::paintComposition (cg, visualState, brand);
                }

                juce::Graphics ig (image);
                ig.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                const juce::Rectangle<float> dst (
                    (float) previewBounds.getX() * kSnapshotScale,
                    (float) previewBounds.getY() * kSnapshotScale,
                    (float) previewBounds.getWidth() * kSnapshotScale,
                    (float) previewBounds.getHeight() * kSnapshotScale);
                ig.drawImage (composed, dst, juce::RectanglePlacement::centred);
            }
        }

        if (filename == "01-palco-mixer.png")
            stageMixerImage = image;

        const auto out = captureDirectory.getChildFile (filename);
        juce::FileOutputStream stream (out);
        if (! stream.openedOk())
            return false;
        stream.setPosition (0);
        stream.truncate();
        const bool ok = juce::PNGImageFormat().writeImageToStream (image, stream);
        stream.flush();
        if (ok)
            std::cout << "MOBILE_PREVIEW: CAPTURED " << out.getFullPathName()
                      << " (" << image.getWidth() << "x" << image.getHeight() << ")" << std::endl;
        return ok;
    }

    void resetToCleanStage()
    {
        closeAuxiliaryWindows();
        auto& inner = window->holder.inner;
        int guard = 0;
        while (inner.handleBackButton() && ++guard < 12) {}
        if (auto* liveBtn = findButtonById (inner, "navLive"))
            if (liveBtn->getToggleState())
                liveBtn->triggerClick();
        inner.resized();
        inner.repaint();
    }

    void prepareStep (Step step)
    {
        auto& inner = window->holder.inner;
        resetToCleanStage();

        switch (step)
        {
            case Step::palcoMixer:
                if (auto* btn = findVisibleButton (inner, "Modo palco"))
                    btn->triggerClick();
                break;

            case Step::editarCanal:
                if (auto* btn = findVisibleButton (inner, "Editar canal"))
                    btn->triggerClick();
                break;

            case Step::seletorTimbres:
                if (auto* btn = findVisibleButton (inner, "Modo palco"))
                    btn->triggerClick();
                if (auto* editor = findTimbresEditor (inner))
                    editor->openTimbrePickerForLayer (0, 0);
                break;

            case Step::cenasLive:
                if (auto* btn = findVisibleButton (inner, "Modo palco"))
                    btn->triggerClick();
                if (auto* liveBtn = findButtonById (inner, "navLive"))
                    if (! liveBtn->getToggleState())
                        liveBtn->triggerClick();
                break;

            case Step::padContinuo:
                if (auto* btn = findVisibleButton (inner, "Modo palco"))
                    btn->triggerClick();
                inner.showPadWindow();
                break;

            case Step::mapearMidi:
                if (auto* btn = findVisibleButton (inner, "Modo palco"))
                    btn->triggerClick();
                inner.startMidiLearn();
                break;

            case Step::biblioteca:
                if (auto* libBtn = findButtonById (inner, "navLibraries"))
                    libBtn->triggerClick();
                break;

            case Step::audioMidi:
                if (auto* audioBtn = findButtonById (inner, "headerNav"))
                    audioBtn->triggerClick();
                break;

            case Step::churchCam:
                if (auto* camBtn = findButtonById (inner, "navLiveCam"))
                    camBtn->triggerClick();
                break;

            case Step::done:
                break;
        }
    }

    juce::String filenameForStep (Step step) const
    {
        switch (step)
        {
            case Step::palcoMixer:     return "01-palco-mixer.png";
            case Step::editarCanal:    return "02-editar-canal.png";
            case Step::seletorTimbres: return "03-seletor-timbres.png";
            case Step::cenasLive:      return "04-cenas-live.png";
            case Step::padContinuo:    return "05-pad-continuo.png";
            case Step::mapearMidi:     return "06-mapear-midi.png";
            case Step::biblioteca:     return "07-biblioteca.png";
            case Step::audioMidi:      return "08-audio-midi.png";
            case Step::churchCam:      return "09-churchcam.png";
            case Step::done:           return {};
        }
        return {};
    }

    void timerCallback() override
    {
        if (closing)
        {
            if (window == nullptr
                || (teardownGate.isClosedAndDrained() && window->holder.inner.ownerTeardownIsReady())
                || ++closeTicks > 120)
            {
                stopTimer();
                closeAuxiliaryWindows();
                window.reset();
                std::cout << "MOBILE_PREVIEW: CLOSED_CLEANLY" << std::endl;
                quit();
            }
            return;
        }

        if (window == nullptr)
            return;

        if (window->holder.inner.isStillLoading() || ++scanWarmupTicks < 3)
        {
            if (++waitTicks < 100)
                return;
            std::cerr << "MOBILE_PREVIEW: timed out waiting for library load." << std::endl;
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        if (! showcaseLoaded)
        {
            configureMobileShowcase (window->holder.inner);
            showcaseLoaded = true;
            if (auto* btn = findVisibleButton (window->holder.inner, "Modo palco"))
                btn->triggerClick();
            if (! captureAll || captureDirectory == juce::File())
            {
                stopTimer();
                return;
            }
            startTimer (750);
            return;
        }

        if (currentStep == Step::done)
        {
            systemRequestedQuit();
            return;
        }

        if (! stepPrepared)
        {
            prepareStep (currentStep);
            stepPrepared = true;
            startTimer (currentStep == Step::biblioteca ? 1400 : 650);
            return;
        }

        juce::Component* target = &window->holder.inner;
        if (currentStep == Step::audioMidi)
        {
            applyAndroidAudioSelectorLabels (window->holder.inner);
        }
        else if (currentStep == Step::churchCam)
        {
            auto& desktop = juce::Desktop::getInstance();
            for (int i = 0; i < desktop.getNumComponents(); ++i)
            {
                if (auto* doc = dynamic_cast<juce::DocumentWindow*> (desktop.getComponent (i)))
                {
                    if (doc != window.get() && doc->isVisible() && doc->getContentComponent() != nullptr)
                    {
                        doc->getContentComponent()->setSize (targetSize.x, targetSize.y);
                        doc->setBounds (24, 24, targetSize.x, targetSize.y + doc->getTitleBarHeight());
                        applyAndroidStudioLabels (*doc->getContentComponent());
                        target = doc->getContentComponent();
                        break;
                    }
                }
            }
        }

        if (! saveSnapshot (*target, filenameForStep (currentStep), currentStep == Step::churchCam))
        {
            std::cerr << "MOBILE_PREVIEW: failed capturing " << filenameForStep (currentStep) << std::endl;
            setApplicationReturnValue (1);
            systemRequestedQuit();
            return;
        }

        currentStep = static_cast<Step> (static_cast<int> (currentStep) + 1);
        stepPrepared = false;
        startTimer (280);
    }

    StdoutLogger logger;
    NordLookAndFeel lookAndFeel;
    ChurchDAWMidiMapping::DeferredOwnerTeardownGate teardownGate;
    std::unique_ptr<Window> window;
    juce::Point<int> targetSize { 884, 390 };
    juce::File captureDirectory;
    juce::Image stageMixerImage;
    Step currentStep = Step::palcoMixer;
    int waitTicks = 0;
    int scanWarmupTicks = 0;
    int closeTicks = 0;
    bool captureAll = false;
    bool showcaseLoaded = false;
    bool stepPrepared = false;
    bool closing = false;
};

START_JUCE_APPLICATION (MobileDesignPreview)
