#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>   // logger de stdout do self-test de CI
#include <thread>

#include "NordLookAndFeel.h"
#include "MainComponent.h"
#include "ChurchDAWAudioActivity.h"
#include "WindowsAudioPerformance.h"
#include "StartupPanel.h"
#include "BinaryData.h"

#if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
 #include "DesktopLoginPanel.h"
#endif

#if JUCE_ANDROID
 #include "AndroidLoginPanel.h"
#endif

#if JUCE_WINDOWS || JUCE_MAC
 #include "DesktopUpdater.h"
#endif

namespace
{
juce::Image loadChurchDAWBrandImage()
{
   #if JUCE_ANDROID
    // Use a copia de interface em 512 px. A arte mestre de 1254 px continua
    // sendo o icone instalado, mas alguns drivers Android a reduziam para preto
    // em componentes pequenos depois da splash.
    return juce::ImageCache::getFromMemory (BinaryData::appicon512_png,
                                            BinaryData::appicon512_pngSize);
   #else
    return juce::ImageCache::getFromMemory (BinaryData::logo_round_png,
                                            BinaryData::logo_round_pngSize);
   #endif
}
}

#if JUCE_ANDROID
static int getInstalledAndroidVersionCode();
static int queryImmediateAndroidUpdateEligibility (int requiredVersionCode);
#endif

//==============================================================================
// Tela de abertura: icone completo, nome e barra fina vermelha
//==============================================================================
class SplashWindow : public juce::TopLevelWindow
{
public:
    SplashWindow() : TopLevelWindow ("ChurchDAW", true)
    {
        icon = loadChurchDAWBrandImage();
        setOpaque (true);
        centreWithSize (460, 400);
        setAlwaysOnTop (true);
        setDropShadowEnabled (true);
        setVisible (true);
    }

    void setStatus (const juce::String& text)
    {
        if (status != text)
        {
            status = text;
            repaint();
        }
    }

    // 0..1 — progresso de verdade: a splash so sai quando chega ao fim
    void setProgress (float p)
    {
        p = juce::jlimit (0.0f, 1.0f, p);

        if (std::abs (p - progress) > 0.002f)
        {
            progress = p;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0d10));

        auto area = getLocalBounds();
        auto logoArea = area.removeFromTop (296).withSizeKeepingCentre (280, 280);

        if (icon.isValid())
        {
            // a arte tem fundo PRETO chapado (sem transparencia): recorta no
            // circulo pra nao aparecer um quadrado atras do logo
            juce::Path circle;
            circle.addEllipse (logoArea.toFloat());

            juce::Graphics::ScopedSaveState clipped (g);
            g.reduceClipRegion (circle);
            g.setOpacity (1.0f);   // imagem herda o alpha do ultimo setColour
            g.drawImageWithin (icon, logoArea.getX(), logoArea.getY(),
                               logoArea.getWidth(), logoArea.getHeight(),
                               juce::RectanglePlacement::centred);
        }

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (status, area.removeFromTop (24), juce::Justification::centred);

        // barra de progresso REAL (antes era um 72% pintado na mao)
        auto bar = area.removeFromTop (16)
                       .withSizeKeepingCentre (juce::roundToInt ((float) getWidth() * 0.62f), 4);

        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.fillRoundedRectangle (bar.toFloat(), 2.0f);

        auto done = bar.toFloat().withWidth ((float) bar.getWidth() * progress);

        if (done.getWidth() > 2.0f)
        {
            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed(), done.getX(), 0.0f,
                                                     NordLookAndFeel::nordRedBright(), done.getRight(), 0.0f, false));
            g.fillRoundedRectangle (done, 2.0f);
        }

        // proprietario
        area.removeFromTop (12);

        g.setColour (juce::Colours::white.withAlpha (0.42f));
        g.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
        g.drawText ("Igor Teixeira", area.removeFromTop (18), juce::Justification::centred);

        g.setColour (juce::Colours::white.withAlpha (0.26f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (juce::String::fromUTF8 ("WhatsApp (62) 98127-8461"),
                    area.removeFromTop (16), juce::Justification::centred);
    }

private:
    juce::Image icon;
    juce::String status { juce::String::fromUTF8 ("Iniciando...") };
    float progress = 0.04f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplashWindow)
};

//==============================================================================
// Janela leve que existe antes do motor de audio. Aqui ficam a verificacao de
// versao e, na primeira abertura, a opcao de baixar os samples.
//==============================================================================
#if JUCE_ANDROID
static juce::Rectangle<int> currentAndroidWindowBounds()
{
    auto* env = juce::getEnv();
    const auto activity = juce::getCurrentActivity();
    if (env == nullptr || activity == nullptr)
        return {};

    static jclass cachedClass = nullptr;
    static jmethodID cachedMethod = nullptr;

    if (cachedClass == nullptr || cachedMethod == nullptr)
    {
        const auto localClazz = ChurchDAWServices::findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWLicenseBridge");
        if (localClazz == nullptr)
        {
            env->ExceptionClear();
            return {};
        }

        const auto method = env->GetStaticMethodID (
            localClazz, "getWindowBoundsDp",
            "(Landroid/app/Activity;)J");
        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (localClazz);
            return {};
        }

        if (cachedClass == nullptr)
            cachedClass = static_cast<jclass> (env->NewGlobalRef (localClazz));
        cachedMethod = method;
        env->DeleteLocalRef (localClazz);
    }

    if (cachedClass == nullptr || cachedMethod == nullptr)
        return {};

    const auto packed = static_cast<juce::uint64> (
        env->CallStaticLongMethod (
            cachedClass, cachedMethod, activity.get()));
    const bool ok = ! env->ExceptionCheck();
    if (! ok)
    {
        env->ExceptionClear();
        return {};
    }

    const auto x = static_cast<int> (
        (packed >> 48) & 0xffffULL);
    const auto y = static_cast<int> (
        (packed >> 32) & 0xffffULL);
    const auto width = static_cast<int> (
        (packed >> 16) & 0xffffULL);
    const auto height = static_cast<int> (
        packed & 0xffffULL);
    return width > 0 && height > 0
        ? juce::Rectangle<int> (x, y, width, height)
        : juce::Rectangle<int>();
}
#endif

class StartupWindow : public juce::TopLevelWindow,
                      private juce::Timer
{
public:
    explicit StartupWindow (juce::Image logo)
        : TopLevelWindow ("ChurchDAW", true),
          panel (std::move (logo))
    {
        addAndMakeVisible (panel);
        setOpaque (true);
        setAlwaysOnTop (true);

       #if JUCE_ANDROID || JUCE_IOS
       #if JUCE_ANDROID
        const auto bounds = currentAndroidWindowBounds();
        setBounds (! bounds.isEmpty()
                       ? bounds
                       : juce::Rectangle<int> (0, 0, 360, 800));
       #else
        if (auto* display =
                juce::Desktop::getInstance()
                    .getDisplays().getPrimaryDisplay())
            setBounds (display->logicalBounds.toNearestInt());
       #endif
       #else
        centreWithSize (820, 540);
        setDropShadowEnabled (true);
       #endif

        setVisible (true);
        toFront (true);
       #if JUCE_ANDROID
        startTimer (250);
       #endif
    }

    void resized() override
    {
       #if JUCE_IOS
        if (iosMainContent != nullptr)
        {
            iosMainContent->setBounds (getLocalBounds());
            return;
        }

        auto safeBounds = getLocalBounds();
        if (const auto* display =
                juce::Desktop::getInstance()
                    .getDisplays().getDisplayForRect (getBounds()))
            safeBounds = display->safeAreaInsets
                .subtractedFrom (safeBounds);
        panel.setBounds (safeBounds);
       #elif JUCE_ANDROID
        panel.setBounds (getLocalBounds());
       #else
        auto safeBounds = getLocalBounds();
        if (const auto* display =
                juce::Desktop::getInstance()
                    .getDisplays().getDisplayForRect (getBounds()))
            safeBounds = display->safeAreaInsets
                .subtractedFrom (safeBounds);
        panel.setBounds (safeBounds);
       #endif
    }

    StartupPanel panel;

   #if JUCE_IOS
    void showMainContent (
        bool safeMode,
        ChurchDAWMidiMapping::DeferredOwnerTeardownGate* teardownGate)
    {
        if (iosMainContent != nullptr)
            return;

        // Mantem o mesmo UIWindow/UIViewController usado pelo StartupPanel.
        // Assim o iPadOS nao precisa iniciar outra transicao de aparencia para
        // mostrar a DAW depois de validar policy, catalogo e biblioteca.
        setAlwaysOnTop (false);
        iosMainContent = std::make_unique<MainComponent> (
            safeMode, teardownGate);
        panel.setVisible (false);
        addAndMakeVisible (*iosMainContent);
        iosMainContent->toFront (false);
        resized();
    }

    MainComponent* content() const noexcept
    {
        return iosMainContent.get();
    }

    void clearMainContent()
    {
        iosMainContent.reset();
    }
   #endif

private:
    void timerCallback() override
    {
       #if JUCE_ANDROID
        const auto bounds = currentAndroidWindowBounds();
        if (! bounds.isEmpty() && getBounds() != bounds)
            setBounds (bounds);
       #endif
    }

   #if JUCE_IOS
    std::unique_ptr<MainComponent> iosMainContent;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StartupWindow)
};

#if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
class DesktopLoginWindow final : public juce::DocumentWindow
{
public:
    explicit DesktopLoginWindow (juce::Image logo)
        : juce::DocumentWindow ("ChurchDAW - Acesso",
                                NordLookAndFeel::background(),
                                juce::DocumentWindow::closeButton),
          panel (std::move (logo))
    {
        setUsingNativeTitleBar (false);
        setTitleBarHeight (30);
        setTitleBarTextCentred (false);
        setIcon (loadChurchDAWBrandImage());
        setContentNonOwned (&panel, false);
        setResizable (true, true);
        setResizeLimits (760, 580, 1600, 1000);
        centreWithSize (1040, 680);
        setVisible (true);
        toFront (true);
        panel.grabKeyboardFocus();
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    DesktopLoginPanel panel;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DesktopLoginWindow)
};
#endif

#if JUCE_ANDROID
class AndroidLoginWindow final : public juce::TopLevelWindow,
                                 private juce::Timer
{
public:
    explicit AndroidLoginWindow (juce::Image logo)
        : juce::TopLevelWindow ("ChurchDAW - Acesso", true),
          panel (std::move (logo))
    {
        addAndMakeVisible (panel);
        setOpaque (true);
        setAlwaysOnTop (true);
        const auto bounds = currentAndroidWindowBounds();
        setBounds (! bounds.isEmpty()
                       ? bounds
                       : juce::Rectangle<int> (0, 0, 360, 800));
        setVisible (true);
        toFront (true);
        startTimer (250);
    }

    void resized() override
    {
        panel.setBounds (getLocalBounds());
    }

    AndroidLoginPanel panel;

private:
    void timerCallback() override
    {
        const auto bounds = currentAndroidWindowBounds();
        if (! bounds.isEmpty() && getBounds() != bounds)
            setBounds (bounds);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (
        AndroidLoginWindow)
};
#endif

class ChurchDAWStartupCheckThread final : public juce::Thread
{
public:
    using Callback = std::function<void (ChurchDAWServices::StartupCheck)>;

    explicit ChurchDAWStartupCheckThread (Callback callbackToUse)
        : juce::Thread ("ChurchDAW startup check"),
          callback (std::move (callbackToUse))
    {
    }

    ChurchDAWStartupCheckThread (
        ChurchDAWServices::StartupCheck existingCheck,
        Callback callbackToUse)
        : juce::Thread ("ChurchDAW local library check"),
          callback (std::move (callbackToUse)),
          seededCheck (std::move (existingCheck)),
          useSeededCheck (true)
    {
    }

    ~ChurchDAWStartupCheckThread() override
    {
        cancel();
        stopThread (30000);
    }

    void cancel() noexcept
    {
        cancelled.store (true, std::memory_order_release);
        signalThreadShouldExit();
    }

    void run() override
    {
        auto check = useSeededCheck
                         ? std::move (seededCheck)
                         : ChurchDAWServices::checkRemoteManifest();

        if (threadShouldExit() || cancelled.load (std::memory_order_acquire))
            return;

        check.localLibraryReady =
            ChurchDAWServices::hasAnyInstalledLibrary();
        check.localLibraryChecked = true;

       #if JUCE_ANDROID
        check.installedVersionCode = getInstalledAndroidVersionCode();

        if (check.updatePolicy.valid
             && check.updatePolicy.minimumVersionCode
                    > check.installedVersionCode)
            check.immediateUpdateEligibility =
                queryImmediateAndroidUpdateEligibility (
                    check.updatePolicy.minimumVersionCode);
       #endif

        if (threadShouldExit() || cancelled.load (std::memory_order_acquire))
            return;

        auto deliver = callback;
        juce::MessageManager::callAsync ([deliver = std::move (deliver),
                                          check = std::move (check)] () mutable
        {
            if (deliver != nullptr)
                deliver (std::move (check));
        });
    }

private:
    Callback callback;
    ChurchDAWServices::StartupCheck seededCheck;
    bool useSeededCheck = false;
    std::atomic<bool> cancelled { false };
};

class ChurchDAWUpdatePolicyCheckThread final : public juce::Thread
{
public:
    using Callback = std::function<void (ChurchDAWServices::UpdatePolicyCheck)>;

    explicit ChurchDAWUpdatePolicyCheckThread (Callback callbackToUse)
        : juce::Thread ("ChurchDAW foreground update check"),
          callback (std::move (callbackToUse))
    {
    }

    ~ChurchDAWUpdatePolicyCheckThread() override
    {
        cancel();
        stopThread (20000);
    }

    void cancel() noexcept
    {
        cancelled.store (true, std::memory_order_release);
        signalThreadShouldExit();
    }

    void run() override
    {
        auto check = ChurchDAWServices::checkRemoteUpdatePolicy();

        if (threadShouldExit() || cancelled.load (std::memory_order_acquire))
            return;

        check.localLibraryReady =
            ChurchDAWServices::hasAnyInstalledLibrary();
        check.localLibraryChecked = true;

       #if JUCE_ANDROID
        check.installedVersionCode = getInstalledAndroidVersionCode();

        if (check.policy.valid
             && check.policy.minimumVersionCode > check.installedVersionCode)
            check.immediateUpdateEligibility =
                queryImmediateAndroidUpdateEligibility (
                    check.policy.minimumVersionCode);
       #endif

        if (threadShouldExit() || cancelled.load (std::memory_order_acquire))
            return;

        auto deliver = callback;
        juce::MessageManager::callAsync ([deliver = std::move (deliver),
                                          check = std::move (check)] () mutable
        {
            if (deliver != nullptr)
                deliver (std::move (check));
        });
    }

private:
    Callback callback;
    std::atomic<bool> cancelled { false };
};

template <typename CheckThread>
static void retireCheckThreadNonBlocking (std::unique_ptr<CheckThread>& holder)
{
    if (holder == nullptr)
        return;

    holder->cancel();

    if (! holder->isThreadRunning() || holder->waitForThreadToExit (40))
    {
        holder.reset();
        return;
    }

    std::thread ([dead = std::move (holder)] () mutable
    {
        dead.reset();
    }).detach();
}

#if JUCE_ANDROID
static int getInstalledAndroidVersionCode()
{
    auto* env = juce::getEnv();
    const auto activity = juce::getCurrentActivity();

    if (env == nullptr || activity == nullptr)
        return 0;

    const auto clazz = ChurchDAWServices::findAndroidAppClass (
        env, activity.get(),
        "com.churchdaw.app.ChurchDAWUpdateBridge");

    if (clazz == nullptr)
    {
        env->ExceptionClear();
        return 0;
    }

    const auto method = env->GetStaticMethodID (
        clazz, "getInstalledVersionCode", "(Landroid/app/Activity;)I");

    if (method == nullptr)
    {
        env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return 0;
    }

    const auto versionCode = (int) env->CallStaticIntMethod (clazz, method, activity.get());
    const bool ok = ! env->ExceptionCheck() && versionCode > 0;

    if (! ok && env->ExceptionCheck())
        env->ExceptionClear();

    env->DeleteLocalRef (clazz);
    return ok ? versionCode : 0;
}

enum AndroidUpdateEligibility
{
    eligibilityQueryFailed = 0,
    eligibilityConfirmed = 1,
    eligibilityNotRequired = 2,
    eligibilityWebsiteUnavailable = 3
};

static juce::String androidUpdateEligibilityDiagnostic (int status)
{
    switch (status)
    {
        case eligibilityConfirmed:
            return "atualizacao oficial confirmada pela policy do ChurchDAW";
        case eligibilityNotRequired:
            return "versao instalada ja suficiente";
        case eligibilityWebsiteUnavailable:
            return "distribuicao oficial indisponivel";
        default:
            return "consulta de atualizacao indisponivel";
    }
}

static int queryImmediateAndroidUpdateEligibility (int requiredVersionCode)
{
    auto* env = juce::getEnv();
    const auto activity = juce::getCurrentActivity();

    if (env == nullptr || activity == nullptr)
        return eligibilityQueryFailed;

    const auto clazz = ChurchDAWServices::findAndroidAppClass (
        env, activity.get(),
        "com.churchdaw.app.ChurchDAWUpdateBridge");

    if (clazz == nullptr)
    {
        env->ExceptionClear();
        return eligibilityQueryFailed;
    }

    const auto method = env->GetStaticMethodID (
        clazz, "queryImmediateUpdateEligibilityBlocking",
        "(Landroid/app/Activity;I)I");

    if (method == nullptr)
    {
        env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return eligibilityQueryFailed;
    }

    const int status = (int) env->CallStaticIntMethod (
        clazz, method, activity.get(),
        (jint) juce::jlimit (1, 1000000000, requiredVersionCode));
    const bool ok = ! env->ExceptionCheck()
                 && status >= eligibilityQueryFailed
                 && status <= eligibilityWebsiteUnavailable;

    if (! ok && env->ExceptionCheck())
        env->ExceptionClear();

    env->DeleteLocalRef (clazz);
    return ok ? status : eligibilityQueryFailed;
}

static juce::File requiredAndroidVersionFloorFile()
{
    return ChurchDAWServices::serviceFolder()
        .getChildFile ("confirmed-android-version.floor");
}

static int loadRequiredAndroidVersionFloor()
{
    const auto text = requiredAndroidVersionFloorFile().loadFileAsString().trim();

    if (text.isEmpty() || ! text.containsOnly ("0123456789"))
        return 0;

    const auto value = text.getLargeIntValue();
    return value > 0 && value <= 1000000000 ? (int) value : 0;
}

static int setConfirmedAndroidVersionFloor (int minimumVersionCode)
{
    const auto file = requiredAndroidVersionFloorFile();

    if (minimumVersionCode <= 1)
    {
        if (file.existsAsFile() && ! file.deleteFile())
            return loadRequiredAndroidVersionFloor();

        ChurchDAWServices::setConfirmedWebsiteUpdateFloor (1);
        return 0;
    }

    file.getParentDirectory().createDirectory();

    if (! file.replaceWithText (juce::String (minimumVersionCode)))
        return loadRequiredAndroidVersionFloor();

    ChurchDAWServices::setConfirmedWebsiteUpdateFloor (minimumVersionCode);
    return minimumVersionCode;
}

static int effectiveRequiredAndroidVersion (
    const ChurchDAWServices::UpdatePolicy& policy,
    bool policyWasFetchedLive,
    int installedVersionCode,
    int playEligibility)
{
    int confirmedMinimum = loadRequiredAndroidVersionFloor();

    if (confirmedMinimum > 0 && installedVersionCode >= confirmedMinimum)
        confirmedMinimum = setConfirmedAndroidVersionFloor (0);

    // Somente uma resposta LIVE, assinada e explicitamente marcada como
    // rollback pode reduzir/remover um piso confirmado. O contador anti-replay
    // e o serial revogado ja foram validados no ChurchDAWServices. Cache e uma
    // politica normal nunca revogam.
    if (policyWasFetchedLive
         && policy.valid
         && policy.cryptographicallyVerified
         && policy.mode == "rollback")
    {
        if (policy.minimumVersionCode <= installedVersionCode)
            confirmedMinimum = setConfirmedAndroidVersionFloor (0);
        else if (confirmedMinimum > policy.minimumVersionCode)
        {
            confirmedMinimum = setConfirmedAndroidVersionFloor (
                policy.minimumVersionCode);
        }
    }

    // Um piso novo e aceito somente de policy RSA valida. Antes de elevar o
    // minimo, o release publica e verifica o APK assinado no churchdaw.com; se
    // esta instalacao veio da Play, a bridge usa a ficha oficial da loja.
    if (policy.valid
         && policy.cryptographicallyVerified
         && policy.minimumVersionCode > installedVersionCode
         && policy.minimumVersionCode > confirmedMinimum
         && playEligibility == eligibilityConfirmed)
        confirmedMinimum = setConfirmedAndroidVersionFloor (
            policy.minimumVersionCode);

    return confirmedMinimum;
}

static ChurchDAWServices::UpdatePolicy policyForRequiredGate (
    ChurchDAWServices::UpdatePolicy policy,
    int effectiveMinimum)
{
    const auto trustedDownloadUrl =
        ChurchDAWServices::requiredUpdateDownloadUrl (
            policy, effectiveMinimum);

    if (! policy.valid || policy.latestVersionCode < effectiveMinimum)
    {
        policy.valid = true;
        policy.schemaVersion = 1;
        policy.latestVersionCode = effectiveMinimum;
        policy.latestVersionName = juce::String (effectiveMinimum);
        policy.updateMessage = juce::String::fromUTF8 (
            "Uma nova versao do ChurchDAW e necessaria para continuar. "
            "Conecte-se a internet e confirme a atualizacao aqui mesmo.");
    }

    policy.minimumVersionCode = effectiveMinimum;

    // O binario antigo nao conhece antecipadamente o nome da proxima versao.
    // Preserve a URL que veio dentro da policy RSA; usar sempre o fallback
    // compilado faria v1.5.1 tentar baixar v1.5.1 ao receber o piso v1.5.2.
    policy.playStoreUrl = trustedDownloadUrl;

    return policy;
}
#endif

#if JUCE_WINDOWS || JUCE_MAC
// GUARDA DE ABERTURA (crash-loop breaker). O boot restaura o setup de audio
// e a cadeia de plugins de terceiros DENTRO do proprio processo: um driver
// (ASIO4ALL!) ou plugin podre mata o app antes de qualquer janela. O padrao
// dos browsers: no boot escrevemos boot-recovery.flag junto ao ChurchDAW.settings
// e a apagamos no fechamento limpo. Marker presente na abertura seguinte =
// o run anterior morreu: entre em MODO SEGURO e ponha o setup suspeito em
// quarentena (backup *.recovered-<data>.xml fora do
// ChurchDAW.settings), quebrando
// o loop de crash. O diagnostico fica no ChurchDAW.log (linhas RECOVERY:).
static juce::File churchDAWBootMarkerFile()
{
    const auto qaSettingsDir = juce::SystemStats::getEnvironmentVariable (
                                   "CHURCHDAW_QA_SETTINGS_DIR", {})
                                   .trim();
    const bool qaDirValid = qaSettingsDir.isNotEmpty()
        && juce::File::isAbsolutePath (qaSettingsDir);

    return juce::File::getSpecialLocation (
               juce::File::userApplicationDataDirectory)
        .getChildFile (qaDirValid ? qaSettingsDir : "ChurchDAW")
        .getChildFile ("boot-recovery.flag");
}

// Move a chave <key> do ChurchDAW.settings para um arquivo irmao de backup e
// remove-a do settings. Arquivo real do PropertiesFile: ChurchDAW.settings. Nao altera nada se o settings ou a chave nao existem.
// Quantos fechamentos anormais SEGUIDOS ja aconteceram. Um isolado nao diz
// nada sobre a culpa do setup de audio: e muito mais provavel queda de luz,
// reboot do Windows ou o proprio update do que um driver podre.
static juce::File churchDAWBootStrikeFile()
{
    return churchDAWBootMarkerFile().getSiblingFile ("boot-recovery.strikes");
}

static int churchDAWRecoveryStrikes()
{
    const auto f = churchDAWBootStrikeFile();
    if (! f.existsAsFile())
        return 0;

    return juce::jlimit (0, 9, f.loadFileAsString().trim().getIntValue());
}

static void churchDAWSetRecoveryStrikes (int strikes)
{
    const auto f = churchDAWBootStrikeFile();

    if (strikes <= 0)
    {
        f.deleteFile();
        return;
    }

    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::String (strikes));
}

static void churchDAWQuarantineSetting (const juce::File& settingsFile,
                                        const juce::String& key,
                                        const juce::String& stamp)
{
    auto xml = juce::parseXML (settingsFile);
    if (xml == nullptr)
        return;

    auto* child = xml->getChildByName (key);
    if (child == nullptr)
        return;

    const auto backup = settingsFile.getSiblingFile (
        key + ".recovered-" + stamp + ".xml");
    if (! backup.existsAsFile())
        juce::XmlElement (*child).writeTo (backup);

    xml->removeChildElement (child, true);
    xml->writeTo (settingsFile);
}

// ULTIMA CONFIGURACAO BOA. O crash-loop breaker precisava jogar o musico no
// audio padrao depois de um driver podre: ele abria, mas sem a interface de
// sempre. Aqui guardamos uma copia do setup que ja rodou de verdade
// (audioSetupGood) para que a recuperacao devolva a configuração que o
// musico usava, e nao o padrao de fabrica.
//
// O par e o flag audioSetupPending: ele marca que o setup atual ainda NAO
// foi comprovado. Se o app morrer com pendencia, o boot seguinte descarta o
// suspeito e restaura o bom -- o usuario nao precisa reescolher nada.
static bool churchDAWCopySetting (const juce::File& settingsFile,
                                  const juce::String& fromKey,
                                  const juce::String& toKey)
{
    auto xml = juce::parseXML (settingsFile);
    if (xml == nullptr)
        return false;

    auto* origem = xml->getChildByName (fromKey);
    if (origem == nullptr)
        return false;

    if (auto* antigo = xml->getChildByName (toKey))
        xml->removeChildElement (antigo, true);

    xml->addChildElement (new juce::XmlElement (*origem));
    return xml->writeTo (settingsFile);
}

// Promove o setup atual a "bom" e limpa a pendencia. Chamado no fechamento
// limpo e depois de alguns segundos de vida com audio de pe.
static void churchDAWPromoteGoodAudio (const juce::File& settingsFile,
                                       bool audioVivo)
{
    if (! audioVivo)
        return;

    auto xml = juce::parseXML (settingsFile);
    if (xml == nullptr)
        return;
    if (xml->getChildByName ("audioSetup") == nullptr)
        return;
    if (xml->getIntAttribute ("audioSetupHealthy", 0) != 1)
        return;

    if (churchDAWCopySetting (settingsFile, "audioSetup", "audioSetupGood"))
    {
        auto xml2 = juce::parseXML (settingsFile);
        if (xml2 != nullptr)
        {
            xml2->setAttribute ("audioSetupPending", 0);
            xml2->writeTo (settingsFile);
        }
        juce::Logger::writeToLog (
            "AUDIO_GOOD: setup de audio atual promovido a ultima configuracao boa");
    }
}

// Descarta o setup pendente (o suspeito) e restaura o ultimo bom, se houver.
// Devolve uma msg curta para a UI.
static juce::String churchDAWDiscardPendingAudio (
    const juce::File& settingsFile, const juce::String& stamp)
{
    churchDAWQuarantineSetting (settingsFile, "audioSetup", stamp);

    auto xml = juce::parseXML (settingsFile);
    if (xml != nullptr)
    {
        xml->setAttribute ("audioSetupPending", 0);
        xml->writeTo (settingsFile);
    }

    if (churchDAWCopySetting (settingsFile, "audioSetupGood", "audioSetup"))
    {
        juce::Logger::writeToLog (
            "RECOVERY: setup de audio pendente descartado; ultima configuracao "
            "boa restaurada (backup em audioSetup.recovered-" + stamp + ".xml)");
        return "restaurada";
    }

    juce::Logger::writeToLog (
        "RECOVERY: setup de audio pendente descartado; sem configuracao boa "
        "conhecida, abrindo com o audio padrao (backup em audioSetup.recovered-"
        + stamp + ".xml)");
    return "descartada";
}
#endif

//==============================================================================
class ChurchDAWApplication : public juce::JUCEApplication,
                             private juce::AsyncUpdater,
                             private juce::Timer
{
public:
    ChurchDAWApplication()
    {
        mainWindowTeardownGate.setDrainReadyCallback (
            this, [] (void* context) noexcept
            {
                static_cast<ChurchDAWApplication*> (context)
                    ->triggerAsyncUpdate();
            });
    }

    const juce::String getApplicationName() override { return "ChurchDAW"; }
    const juce::String getApplicationVersion() override
    {
       #if JUCE_ANDROID
        return ChurchDAWServices::currentVersionName;
       #else
        // Android e desktop possuem ciclos de versao independentes. No
        // desktop, use a mesma versao do bundle/EXE gerada pelo CMake para que
        // a tela Sobre e os logs nunca herdem por engano o versionName do APK.
        return CHURCHDAW_APP_VERSION;
       #endif
    }
    bool moreThanOneInstanceAllowed() override
    {
       #if CHURCHDAW_HOSTS_PLUGINS
        // O scanner relanca ESTE executavel como processo filho. O JUCE testa
        // instancia unica em initialiseApp() ANTES de chamar initialise()
        // (juce_ApplicationBase.cpp:287-293), entao com "false" fixo o filho
        // morria sem nunca conectar ao pipe: cada plugin esperava o timeout
        // de 8 s e ia direto para a lista de bloqueados. O resultado era o
        // app congelado por minutos e terminando com a lista VAZIA — foi
        // isto que fez o Pianoteq e o BigSky "sumirem" e o Korg Vox
        // "travar" para os clientes.
        //
        // Segunda instancia continua proibida para o musico: so a linha de
        // comando do scanner libera. getCommandLineParameters() e estatico e
        // ja esta disponivel aqui.
        if (juce::JUCEApplicationBase::getCommandLineParameters()
                .contains (ChurchDAWPluginScan::processUID))
            return true;
       #endif

        return false;
    }

    void initialise (const juce::String& commandLine) override
    {
       #if JUCE_ANDROID
        juce::Logger::writeToLog (
            "CHURCHDAW_RELEASE_LICENSE_GATE=ANDROID_ENABLED");
       #elif CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        juce::Logger::writeToLog (
            "CHURCHDAW_RELEASE_LICENSE_GATE=DESKTOP_ENABLED");
       #elif JUCE_WINDOWS || JUCE_MAC
        juce::Logger::writeToLog (
            "CHURCHDAW_RELEASE_LICENSE_GATE=DESKTOP_DISABLED_TEST_ONLY");
       #endif

        safeModeRequested = commandLine.containsIgnoreCase ("--churchdaw-safe-mode");
        if (safeModeRequested)
            juce::Logger::writeToLog (
                "SAFE_MODE: solicitado pela linha de comando");
       #if CHURCHDAW_HOSTS_PLUGINS
        // O ChurchDAW relanca A SI MESMO como processo de varredura: quando
        // a linha de comando traz o canal do scanner, esta instancia NAO e
        // o app -- e o sacrificavel que abre um plugin de cada vez. Sem
        // janela, sem audio, sem licenca. Se o plugin travar, morre so ele
        // e o app segue a lista (ver PluginScanSubprocess.h).
        //
        // O TESTE DA LINHA DE COMANDO VEM PRIMEIRO, e nao e detalhe: o
        // construtor de PluginScannerSubprocess chama
        // addDefaultFormatsToManager, que no macOS abre o registro de
        // Audio Units do sistema. Num Mac com muitos plugins isso demora
        // (ou trava) -- e como este bloco roda antes do logger, o app
        // ficava sem janela e sem UMA linha de log, so o icone pulando no
        // Dock. Foi o que travou o Mac de um cliente em 31/07/2026.
        if (commandLine.contains (ChurchDAWPluginScan::processUID))
        {
            auto scanner = std::make_unique<
                ChurchDAWPluginScan::PluginScannerSubprocess>();

            if (scanner->initialiseFromCommandLine (
                    commandLine, ChurchDAWPluginScan::processUID))
            {
                pluginScannerWorker = std::move (scanner);
                return;
            }
        }
       #endif

        // REGISTRO EM ARQUIVO.
        //
        // No Android o Logger cai no logcat e da para ler com o cabo. No
        // desktop nao havia logger nenhum: todo juce::Logger::writeToLog do
        // app -- falha de atualizacao, limpeza de parte que nao deu, protecao
        // de NaN -- ia para lugar nenhum. Quando um cliente relata problema,
        // nao existe o que pedir para ele mandar.
        //
        // Descoberto em 28/07/2026 tentando entender por que uma sessao de 87
        // minutos terminou: nao havia registro de nada.
        //
        // Vai para %APPDATA%\ChurchDAW no Windows e ~/Library/Logs/ChurchDAW
        // no Mac, com rodizio em 512 KB para nao crescer sem fim no PC da
        // igreja.
        // No self-test de CI o log TEM de sair no stdout: com o FileLogger
        // (ou com logger nenhum, que cai em outputDebugString) o
        // mac-selftest.log do runner ficou com UMA linha em 30/07, os greps
        // do workflow perderam a evidencia e o diagnostico do shutdown
        // ficou cego. Cliente real continua com o arquivo rotativo.
        const bool ciSelfTest = juce::SystemStats::getEnvironmentVariable (
            "CHURCHDAW_SELFTEST", {}).isNotEmpty();

        if (ciSelfTest)
        {
            struct StdoutLogger final : public juce::Logger
            {
                void logMessage (const juce::String& message) override
                {
                    std::cout << message << std::endl;
                }
            };

            stdoutLogger = std::make_unique<StdoutLogger>();
            juce::Logger::setCurrentLogger (stdoutLogger.get());
        }
        else
        {
            fileLogger.reset (juce::FileLogger::createDefaultAppLogger (
                "ChurchDAW", "ChurchDAW.log",
                juce::String ("ChurchDAW ")
                    + getApplicationVersion() + " iniciado",
                512 * 1024));
            juce::Logger::setCurrentLogger (fileLogger.get());
        }

        // Queda sem rastro era o pior caso de suporte: o log parava numa linha
        // qualquer e ninguem sabia ONDE o app morreu. Agora a pilha vai para o
        // mesmo ChurchDAW.log que o cliente ja sabe mandar. No Windows o JUCE
        // simboliza pelo .pdb ao lado do exe (quando existe); sem ele ainda
        // sai modulo + deslocamento. Roda na thread que falhou, com o processo
        // ja condenado: melhor esforco, sem nada alem de escrever a linha.
        juce::SystemStats::setApplicationCrashHandler ([] (void*)
        {
            juce::Logger::writeToLog ("CRASH: excecao nao tratada; pilha:\n"
                                      + juce::SystemStats::getStackBacktrace());
        });

       #if JUCE_WINDOWS
        // Antes de nascer qualquer thread de audio: tira o processo do EcoQoS
        // do Windows 11. Sem isto, com o ChurchDAW atras do software de
        // projecao, o sistema pode empurrar o motor para nucleos de eficiencia
        // e o som pipoca sem a CPU estar cheia (ver WindowsAudioPerformance.h).
        juce::Logger::writeToLog (
            juce::String ("WINDOWS_QOS high_qos=")
            + (ChurchDAWWindowsPerformance::optProcessOutOfPowerThrottling() ? "1" : "0"));
       #endif

       #if JUCE_WINDOWS || JUCE_MAC
        // GUARDA DE ABERTURA (crash-loop breaker): veja churchDAWBootMarkerFile.
        // Marker vivo = fechamento anterior anormal (crash de driver/plugin,
        // queda de luz, kill). Reage: MODO SEGURO (safeModeRequested ja pula
        // audioSetup, cadeia e plugins de terceiros em MainComponent) e
        // QUARENTENA do setup suspeito, para o proximo boot limpo nao
        // restaurar de novo o que matou o processo e recriar o loop.
        const auto bootMarkerFile = churchDAWBootMarkerFile();
        if (bootMarkerFile.existsAsFile())
        {
            juce::Logger::writeToLog (
                "RECOVERY: fechamento anterior terminou de forma anormal; "
                "iniciando em MODO SEGURO (audio padrao, sem restaurar "
                "cadeia/plugins; reescolha o driver em Configuracoes de audio)");
            safeModeRequested = true;

            const auto stamp = juce::Time::getCurrentTime()
                                   .formatted ("%Y%m%d-%H%M%S");
            const auto settingsFile = bootMarkerFile.getParentDirectory()
                                          .getChildFile ("ChurchDAW.settings");
            auto xmlPendente = juce::parseXML (settingsFile);
            const bool audioPendente = xmlPendente != nullptr
                && xmlPendente->getIntAttribute ("audioSetupPending", 0) == 1;

            if (audioPendente)
            {
                // O suspeito e o setup novo que o musico acabou de escolher:
                // devolvemos a ultima configuracao que comprovadamente rodou.
                recoveryNotice = churchDAWDiscardPendingAudio (settingsFile, stamp);

                if (recoveryNotice == "restaurada")
                {
                    // Aqui o modo seguro seria um tiro no pe: ele ignora o
                    // setup de audio -- exatamente o que acabamos de restaurar.
                    // Com a configuracao boa de volta, abrimos normal.
                    safeModeRequested = false;
                    juce::Logger::writeToLog (
                        "RECOVERY: abrindo normalmente com a ultima configuracao boa "
                        "(modo seguro desligado)");
                }
            }
            else
            {
                // Nada foi trocado pelo musico, entao um fechamento anormal
                // sozinho NAO e prova contra o setup de audio. So no segundo
                // seguido a gente mexe na configuracao.
                const int strikes = churchDAWRecoveryStrikes() + 1;
                churchDAWSetRecoveryStrikes (strikes);

                if (strikes < 2)
                {
                    safeModeRequested = false;
                    juce::Logger::writeToLog (
                        "RECOVERY: fechamento anormal (" + juce::String (strikes)
                        + "/2); abrindo com a configuracao salva, sem quarentena. "
                          "Se o app cair de novo no proximo boot, o setup entra "
                          "em quarentena.");
                }
                else if (churchDAWCopySetting (settingsFile, "audioSetupGood",
                                               "audioSetup"))
                {
                    // Segunda queda seguida: o suspeito e o setup atual, mas
                    // em vez de jogar o musico no padrao de fabrica devolvemos
                    // a ultima configuracao que comprovadamente abriu.
                    churchDAWQuarantineSetting (settingsFile, "chain", stamp);
                    churchDAWSetRecoveryStrikes (0);
                    safeModeRequested = false;
                    juce::Logger::writeToLog (
                        "RECOVERY: segunda queda seguida; ultima configuracao "
                        "boa restaurada (backup em audioSetup.recovered-"
                        + stamp + ".xml)");
                    recoveryNotice = "restaurada";
                }
                else
                {
                    churchDAWQuarantineSetting (settingsFile, "audioSetup", stamp);
                    churchDAWQuarantineSetting (settingsFile, "chain", stamp);
                    churchDAWSetRecoveryStrikes (0);
                    juce::Logger::writeToLog (
                        "RECOVERY: segunda queda seguida e sem configuracao boa "
                        "conhecida; audioSetup/chain anteriores preservados em "
                        "*.recovered-" + stamp + ".xml");
                    recoveryNotice = "quarentena";
                }
            }
        }
        bootMarkerFile.getParentDirectory().createDirectory();
        bootMarkerFile.replaceWithText (
            juce::Time::getCurrentTime().toISO8601 (true));

        // O aviso de recuperacao espera a janela existir para nao nascer
        // atras dela. A promocao do setup de audio acontece no MainComponent,
        // que conhece o estado real do device.
        confirmacaoAudioTimer.startTimer (3000);
       #endif

        juce::LookAndFeel::setDefaultLookAndFeel (&nordLookAndFeel);

        // traduz os textos internos do JUCE (seletor de audio/MIDI e janela de plugins) pra pt-BR
        NordLookAndFeel::installPortugueseStrings();

       #if JUCE_ANDROID && defined (CHURCHDAW_ANDROID_UI_QA)
        // SOMENTE matriz visual local: permite abrir o mixer no emulador sem
        // licenca/biblioteca. A macro nunca existe no .jucer oficial e este
        // bloco e removido depois das capturas.
        beginMainWindow();
       #elif JUCE_WINDOWS || JUCE_MAC
        // Atualizacao obrigatoria ANTES de tudo. Vem antes ate do login porque
        // a propria correcao pode estar no login, e porque exigir que o musico
        // entre na conta so para ser informado de que precisa atualizar e
        // trabalho a toa.
        //
        // Este e o UNICO ponto onde o portao pode bloquear: aqui nao existe
        // motor de audio nem nota soando. Ver a nota sobre culto ao vivo em
        // DesktopUpdater.h.
        auto guard = applicationAlive;
        ChurchDAWDesktopUpdate::runStartupUpdateGate (
            [this, guard] (bool shouldQuit)
            {
                if (! guard->load())
                    return;

                if (shouldQuit)
                {
                    quit();   // instalador rodando: precisamos soltar os arquivos
                    return;
                }

                beginStartupAfterUpdateGate();
            });
       #elif JUCE_ANDROID
        beginAndroidStartupUpdateGate();
       #else
        beginStartupAfterUpdateGate();
       #endif
    }

   #if JUCE_ANDROID
    void beginAndroidStartupUpdateGate()
    {
        if (startupWindow != nullptr || androidLoginWindow != nullptr
             || mainWindow != nullptr || startupUpdateCheck != nullptr)
            return;

        auto logo = loadChurchDAWBrandImage();
        startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        startupWindow->panel.showChecking();

        auto guard = applicationAlive;
        startupUpdateCheck =
            std::make_unique<ChurchDAWUpdatePolicyCheckThread> (
                [this, guard] (ChurchDAWServices::UpdatePolicyCheck check)
                {
                    if (guard->load())
                        handleAndroidStartupUpdateGate (std::move (check));
                });
        startupUpdateCheck->startThread();
    }

    void handleAndroidStartupUpdateGate (
        ChurchDAWServices::UpdatePolicyCheck check)
    {
        startupUpdateCheck = nullptr;

        if (startupWindow == nullptr)
            return;

        if (check.localLibraryChecked)
            androidLocalLibraryReady = check.localLibraryReady;

        const int installedVersionCode = check.installedVersionCode > 0
                                           ? check.installedVersionCode
                                           : getInstalledAndroidVersionCode();
        const int effectiveMinimum = effectiveRequiredAndroidVersion (
            check.policy,
            check.reachedServer,
            installedVersionCode,
            check.immediateUpdateEligibility);

        const auto updateDecision =
            ChurchDAWServices::decideAndroidUpdatePrompt (
                check.policy, installedVersionCode, effectiveMinimum);

        if (updateDecision
             == ChurchDAWServices::AndroidUpdatePromptDecision::requiredUpdate)
        {
            foregroundRevalidationPending = false;
            requiredUpdateGateActive = true;
            requiredUpdateFloor = effectiveMinimum;
            startupWindow->panel.showRequiredUpdate (
                policyForRequiredGate (
                    std::move (check.policy), effectiveMinimum));
            return;
        }

        requiredUpdateGateActive = false;
        requiredUpdateFloor = 0;

        if (updateDecision
             == ChurchDAWServices::AndroidUpdatePromptDecision::optionalUpdate)
        {
            // Esta oferta e consumida uma unica vez neste cold start. "Depois"
            // segue exatamente para o gate de licenca/biblioteca que ja existia,
            // sem consultar a mesma policy de novo e sem criar loop.
            auto guard = applicationAlive;
            startupWindow->panel.onContinue = [this, guard]
            {
                juce::MessageManager::callAsync ([this, guard]
                {
                    if (! guard->load() || startupWindow == nullptr)
                        return;

                    startupWindow = nullptr;
                    beginAndroidLicenseGate();
                });
            };
            startupWindow->panel.showOptionalUpdate (std::move (check.policy));
            return;
        }

        startupWindow = nullptr;
        beginAndroidLicenseGate();
    }
   #endif

    void beginStartupAfterUpdateGate()
    {
       #if JUCE_ANDROID
        beginAndroidLicenseGate();
       #elif CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        beginDesktopLicenseGate();
       #else
        beginPostLicenseStartup();
       #endif
    }

    void beginPostLicenseStartup()
    {
        if (startupWindow != nullptr || mainWindow != nullptr)
            return;

       #if JUCE_MAC
        // Self-test de CI nao tem quem clique: quando o catalogo nao valida
        // (runner sem acesso, host de fallback em 301...), a tela de
        // primeira configuracao fica esperando "Abrir sem baixar" para
        // sempre — o audio nunca inicia, o quit por amostras nunca dispara
        // e o workflow acusava "APP NAO ENCERROU LIMPO" sem dizer por que.
        // Provado pelo print tela_20s.png do run de 30/07.
        if (juce::SystemStats::getEnvironmentVariable (
                "CHURCHDAW_SELFTEST", {}).isNotEmpty())
        {
            juce::Logger::writeToLog (
                "SELFTEST: pulando a primeira configuracao");
            requestMainWindowAfterLicense();
            return;
        }
       #endif

        auto logo = loadChurchDAWBrandImage();
        startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        startupWindow->panel.showChecking();
        startupWindow->panel.onContinue = [this]
        {
            requestMainWindowAfterLicense();
        };
        startStartupCheck();
    }

   #if JUCE_ANDROID
    void beginAndroidLicenseGate()
    {
        if (androidLicenseClient == nullptr)
        {
            androidLicenseClient =
                std::make_unique<
                    ChurchDAWAndroidLicensing::Client>();
            auto guard = applicationAlive;
            androidLicenseClient->onSnapshot =
                [this, guard] (
                    const ChurchDAWAndroidLicensing::Snapshot& next)
                {
                    if (! guard->load())
                        return;

                    if (androidLoginWindow != nullptr)
                        androidLoginWindow->panel.applySnapshot (next);

                    if (androidTrialReserveWaiting)
                    {
                        if (next.canPrepareApp && ! next.busy)
                        {
                            androidTrialReserveWaiting = false;
                            auto delayedGuard = applicationAlive;
                            juce::Timer::callAfterDelay (
                                100, [this, delayedGuard]
                                {
                                    if (! delayedGuard->load()
                                        || androidLicenseClient == nullptr
                                        || ! androidTrialPreparationPending)
                                        return;

                                    androidTrialStartWaiting = true;
                                    if (startupWindow != nullptr)
                                        startupWindow->panel.showChecking();
                                    androidLicenseClient->startReservedTrial();
                                });
                        }
                        else if (! next.busy)
                        {
                            androidTrialReserveWaiting = false;
                            androidTrialPreparationPending = false;
                            androidLicenseGatePassed = false;
                            startupWindow = nullptr;
                            showAndroidLoginWindow();
                        }
                        return;
                    }

                    if (androidTrialStartWaiting)
                    {
                        if (next.canOpenApp)
                        {
                            androidTrialStartWaiting = false;
                            androidTrialPreparationPending = false;
                            androidTrialActive = next.isTrial;
                            androidTrialExpiresAt =
                                next.trialExpiresAt;
                            ChurchDAWServices::
                                setLibraryAuthorizationToken (
                                    androidLicenseClient
                                        ->libraryAuthorizationToken());
                            beginMainWindow();
                        }
                        else if (! next.busy)
                        {
                            androidTrialStartWaiting = false;
                            androidTrialPreparationPending = false;
                            androidLicenseGatePassed = false;
                            startupWindow = nullptr;
                            showAndroidLoginWindow();
                        }
                        return;
                    }

                    if (! next.canPrepareApp
                        || androidLicenseGatePassed)
                        return;

                    androidLicenseGatePassed = true;
                    androidTrialPreparationPending =
                        ! next.canOpenApp;
                    androidTrialActive = next.isTrial;
                    androidTrialExpiresAt =
                        next.trialExpiresAt;
                    ChurchDAWServices::
                        setLibraryAuthorizationToken (
                            androidLicenseClient
                                ->libraryAuthorizationToken());

                    juce::Timer::callAfterDelay (
                        220, [this, guard]
                        {
                            if (! guard->load())
                                return;
                            androidLoginWindow = nullptr;
                            beginPostLicenseStartup();
                        });
                };
        }

        showAndroidLoginWindow();
        androidLicenseClient->resumeSavedSession();
    }

    void showAndroidLoginWindow()
    {
        if (androidLoginWindow == nullptr)
        {
            androidLoginWindow =
                std::make_unique<AndroidLoginWindow> (
                    loadChurchDAWBrandImage());
            androidLoginWindow->panel.onLogin =
                [this] (juce::String email,
                        juce::String password)
                {
                    if (androidLicenseClient != nullptr)
                        androidLicenseClient->login (
                            std::move (email),
                            std::move (password));
                };
            androidLoginWindow->panel.onTrial = [this]
            {
                beginAndroidTrialLibraryPreparation();
            };
            androidLoginWindow->panel.onForgotPassword =
                [this] (juce::String email)
                {
                    if (androidLicenseClient != nullptr)
                        androidLicenseClient
                            ->requestPasswordReset (
                                std::move (email));
                };
            androidLoginWindow->panel.onManageActivation = []
            {
                ChurchDAWAndroidLicensing::Client::
                    accountPortalUrl().launchInDefaultBrowser();
            };
            if (androidLicenseClient != nullptr)
                androidLoginWindow->panel.applySnapshot (
                    androidLicenseClient->currentSnapshot());

            androidLoginWindow->panel.setLibraryState (
                androidLocalLibraryReady
                    ? AndroidLoginPanel::LibraryState::ready
                    : AndroidLoginPanel::LibraryState::notStarted);
        }
    }

    void beginAndroidTrialLibraryPreparation()
    {
        if (androidLicenseClient == nullptr
            || androidLicenseGatePassed)
            return;

        // A biblioteca Church Live e publica e pode levar horas em redes
        // lentas. Nao reserve nem inicie a demonstracao antes de o instalador
        // confirmar todos os segmentos e o motor de audio estar pronto.
        androidLicenseGatePassed = true;
        androidTrialPreparationPending = true;
        androidTrialReserveWaiting = false;
        androidTrialStartWaiting = false;
        androidTrialActive = false;
        androidTrialExpiresAt = 0;
        ChurchDAWServices::setLibraryAuthorizationToken ({});

        auto guard = applicationAlive;
        juce::Timer::callAfterDelay (220, [this, guard]
        {
            if (! guard->load())
                return;

            androidLoginWindow = nullptr;
            beginPostLicenseStartup();
        });
    }
   #endif

    void requestMainWindowAfterLicense()
    {
       #if JUCE_ANDROID
        if (androidTrialPreparationPending)
        {
            if (androidLicenseClient == nullptr)
            {
                startupWindow = nullptr;
                androidLicenseGatePassed = false;
                beginAndroidLicenseGate();
                return;
            }
            // No fluxo de demo o StartupPanel so libera onContinue depois de
            // confirmar o marker final do pack. Mantemos o snapshot local
            // coerente caso a reserva da demo falhe e o login reapareca.
            androidLocalLibraryReady = true;
            androidTrialReserveWaiting = true;
            startupWindow->panel.showChecking();
            androidLicenseClient->reserveTrial();
            return;
        }
       #endif
        beginMainWindow();
    }

   #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
    void beginDesktopLicenseGate()
    {
        ++desktopLicenseGateEpoch;
        if (licenseClient == nullptr)
        {
            licenseClient =
                std::make_unique<ChurchDAWDesktopLicensing::Client>();
            auto guard = applicationAlive;
            licenseClient->onSnapshot =
                [this, guard] (
                    const ChurchDAWDesktopLicensing::Snapshot& snapshot)
                {
                    if (! guard->load())
                        return;

                    handleDesktopLicenseSnapshot (snapshot);
                };
        }

        licenseGatePassed = false;
        desktopTerminalTransitionPending = false;
        showDesktopLoginWindow();
        licenseClient->resumeSavedSession();
    }

    void showDesktopLoginWindow()
    {
        if (loginWindow != nullptr || licenseClient == nullptr)
            return;

        loginWindow = std::make_unique<DesktopLoginWindow> (
            loadChurchDAWBrandImage());
        loginWindow->panel.onLogin =
                [this] (juce::String identifier,
                        juce::String password,
                        bool remember)
                {
                    if (licenseClient != nullptr)
                        licenseClient->login (std::move (identifier),
                                              std::move (password),
                                          remember);
            };
        loginWindow->panel.onForgotPassword =
            [this] (juce::String email)
            {
                if (licenseClient != nullptr)
                    licenseClient->requestPasswordReset (std::move (email));
            };
        loginWindow->panel.onBuy = []
        {
            ChurchDAWDesktopLicensing::Client::purchaseUrl()
                .launchInDefaultBrowser();
        };
        loginWindow->panel.onManageActivation = []
        {
            ChurchDAWDesktopLicensing::Client::accountPortalUrl()
                .launchInDefaultBrowser();
        };
        loginWindow->panel.onUpdateClient = []
        {
            ChurchDAWDesktopLicensing::Client::downloadUrl()
                .launchInDefaultBrowser();
        };
        loginWindow->panel.applySnapshot (
            licenseClient->currentSnapshot());
    }

    bool desktopLicenseGateAllowsOpen (juce::uint64 expectedEpoch) const
    {
        if (licenseClient == nullptr)
            return false;

        return ChurchDAWDesktopLicensing::gateEpochAllowsOpen (
            expectedEpoch,
            desktopLicenseGateEpoch,
            licenseGatePassed,
            desktopTerminalTransitionPending,
            licenseClient->currentSnapshot());
    }

    void handleDesktopLicenseSnapshot (
        const ChurchDAWDesktopLicensing::Snapshot& snapshot)
    {
        if (loginWindow != nullptr)
            loginWindow->panel.applySnapshot (snapshot);

       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        avisarSeLicencaPertoDeVencer (snapshot);
       #endif

        if (snapshot.requiresAppClosure)
        {
            handleDesktopRuntimeTermination (snapshot);
            return;
        }

        if (! snapshot.canOpenApp || licenseGatePassed
            || desktopTerminalTransitionPending)
            return;

        licenseGatePassed = true;
        const auto openEpoch = ++desktopLicenseGateEpoch;
        const bool reopenMainWindow = desktopReturnToMainAfterLogin;
        auto guard = applicationAlive;
        juce::Timer::callAfterDelay (
            280, [this, guard, reopenMainWindow, openEpoch]
        {
            if (! guard->load()
                || ! desktopLicenseGateAllowsOpen (openEpoch))
                return;

            loginWindow = nullptr;
            desktopReturnToMainAfterLogin = false;
            if (reopenMainWindow)
                beginMainWindow();
            else
                beginPostLicenseStartup();
        });
    }

    void mostrarAvisoLicenca (const juce::String& titulo,
                              const juce::String& corpo)
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            titulo, corpo, juce::String::fromUTF8 ("Entendi"));
    }

    // Avisa ANTES de vencer: hoje o musico so descobre que a licenca venceu
    // quando o app ja fechou a janela. Cinco dias de antecedencia dao tempo de
    // conectar a igreja na internet sem correria.
    void avisarSeLicencaPertoDeVencer (
        const ChurchDAWDesktopLicensing::Snapshot& snapshot)
    {
        if (licenseExpiryWarned || snapshot.offlineUntil <= 0
            || ! snapshot.canOpenApp)
            return;

        const auto agoraSegundos = juce::Time::currentTimeMillis() / 1000;
        const auto restante = snapshot.offlineUntil - agoraSegundos;
        if (restante <= 0 || restante > 5LL * 86400LL)
            return;

        licenseExpiryWarned = true;
        const int dias = (int) ((restante + 86399LL) / 86400LL);
        mostrarAvisoLicenca (
            "Conecte a internet para renovar",
            "Faltam cerca de " + juce::String (dias)
                + " dia(s) para o ChurchDAW exigir uma nova validacao da "
                  "licenca. Abra o app conectado por alguns minutos e a "
                  "validacao se renova sem pressa.");
    }

    // Adia o encerramento por licenca enquanto houver som saindo. O gate de
    // licenca roda em thread propria e NAO depende de rede, entao sem isto a
    // janela fechava no meio do culto exatamente no dia do vencimento.
    bool adiarEncerramentoSeHouverSom (
        const ChurchDAWDesktopLicensing::Snapshot& snapshot)
    {
        const auto agora = juce::Time::currentTimeMillis();

        if (! ChurchDAWAudioActivity::audibleWithin (15000))
        {
            desktopTerminationDeferredAtMs = 0;
            return false;
        }

        if (desktopTerminationDeferredAtMs <= 0)
            desktopTerminationDeferredAtMs = agora;

        // Toleranca: meia hora de som cobre o resto de um culto sem permitir
        // uso indefinido depois do vencimento.
        if ((agora - desktopTerminationDeferredAtMs) >= (30LL * 60LL * 1000LL))
            return false;

        if (desktopTerminationDeferredAtMs != agora)
            return true;   // reavaliacao ja agendada

        mostrarAvisoLicenca (
            "Licenca vencida — vamos fechar quando o som parar",
            "Ainda ha som saindo, entao o ChurchDAW nao fecha agora. Conecte a "
            "internet para renovar; se nao renovar, o app encerra assim que o "
            "audio ficar em silencio.");

        auto guard = applicationAlive;
        const auto epoch = desktopLicenseGateEpoch;
        juce::Timer::callAfterDelay (
            30000,
            [this, guard, epoch, snap = snapshot]
            {
                if (! guard->load() || epoch != desktopLicenseGateEpoch)
                    return;
                handleDesktopRuntimeTermination (snap);
            });
        return true;
    }

    void handleDesktopRuntimeTermination (
        ChurchDAWDesktopLicensing::Snapshot terminalSnapshot)
    {
       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        if (adiarEncerramentoSeHouverSom (terminalSnapshot))
            return;
       #endif

        if (desktopTerminalTransitionPending)
        {
            licenseGatePassed = false;
            return;
        }

        const bool hadMainSurface = mainWindow != nullptr
                                 || startingMainWindow
                                 || splash != nullptr;
        const auto transitionEpoch = ++desktopLicenseGateEpoch;
        licenseGatePassed = false;
        desktopTerminalTransitionPending = true;
        desktopReturnToMainAfterLogin = hadMainSurface;
        startingMainWindow = false;
        const bool needsFade = mainWindow != nullptr
                            && mainWindow->content() != nullptr;
        if (needsFade)
            mainWindow->content()->beginLicenseFadeOut();

        auto guard = applicationAlive;
        juce::Timer::callAfterDelay (
            needsFade ? 900 : 50,
            [this, guard, transitionEpoch,
             terminalSnapshot = std::move (terminalSnapshot)]
            {
                if (! guard->load()
                    || transitionEpoch != desktopLicenseGateEpoch)
                    return;

                gate = nullptr;
                splash = nullptr;
                startupWindow = nullptr;
                requestMainWindowTeardown (
                    [this, terminalSnapshot = std::move (terminalSnapshot)]
                    {
                        startingMainWindow = false;
                        licenseGatePassed = false;
                        desktopTerminalTransitionPending = false;
                        showDesktopLoginWindow();
                        if (loginWindow != nullptr)
                            loginWindow->panel.applySnapshot (
                                terminalSnapshot);
                    });
            });
    }

    void handleDesktopUserLogout()
    {
        if (licenseClient == nullptr || desktopTerminalTransitionPending)
            return;

        const auto transitionEpoch = ++desktopLicenseGateEpoch;
        licenseGatePassed = false;
        desktopTerminalTransitionPending = true;
        desktopReturnToMainAfterLogin = false;
        startingMainWindow = false;

        const bool needsFade = mainWindow != nullptr
                            && mainWindow->content() != nullptr;
        if (needsFade)
            mainWindow->content()->beginLicenseFadeOut();

        // O cliente invalida runtime/cofre local antes de qualquer rede. O POST
        // remoto usa apenas uma copia transitoria e segue em segundo plano.
        licenseClient->signOut();

        auto guard = applicationAlive;
        juce::Timer::callAfterDelay (
            needsFade ? 900 : 50,
            [this, guard, transitionEpoch]
            {
                if (! guard->load()
                    || transitionEpoch != desktopLicenseGateEpoch)
                    return;

                gate = nullptr;
                splash = nullptr;
                startupWindow = nullptr;
                requestMainWindowTeardown (
                    [this]
                    {
                        startingMainWindow = false;
                        desktopTerminalTransitionPending = false;
                        showDesktopLoginWindow();
                        if (loginWindow != nullptr
                             && licenseClient != nullptr)
                            loginWindow->panel.applySnapshot (
                                licenseClient->currentSnapshot());
                    });
            });
    }
   #endif

    void startStartupCheck()
    {
        retireCheckThreadNonBlocking (startupCheck);
        auto guard = applicationAlive;
        startupCheck = std::make_unique<ChurchDAWStartupCheckThread> (
            [this, guard] (ChurchDAWServices::StartupCheck check)
            {
                if (guard->load())
                    handleStartupCheck (std::move (check));
            });
        startupCheck->startThread();
    }

    void recheckLocalLibrary (
        ChurchDAWServices::StartupCheck check)
    {
        retireCheckThreadNonBlocking (startupCheck);
        auto guard = applicationAlive;
        startupCheck = std::make_unique<ChurchDAWStartupCheckThread> (
            std::move (check),
            [this, guard] (ChurchDAWServices::StartupCheck refreshed)
            {
                if (guard->load())
                    handleStartupCheck (std::move (refreshed));
            });
        startupCheck->startThread();
    }

    void handleStartupCheck (ChurchDAWServices::StartupCheck check)
    {
        if (startupWindow == nullptr)
            return;

        auto safe = juce::Component::SafePointer<StartupPanel> (&startupWindow->panel);

        // Permite que a tela de primeira configuracao repita a checagem quando
        // o servidor nao respondeu, em vez de deixar o cliente sem nenhum botao.
        startupWindow->panel.onRetryCatalog = [this]
        {
            if (applicationAlive->load())
                startStartupCheck();
        };

       #if JUCE_ANDROID
        if (check.localLibraryChecked)
            androidLocalLibraryReady = check.localLibraryReady;

        // A politica pequena e independente do catalogo e a autoridade do gate.
        // O PackageManager informa o versionCode realmente instalado pela loja.
        const int installedVersionCode = check.installedVersionCode > 0
                                           ? check.installedVersionCode
                                           : getInstalledAndroidVersionCode();
        const int effectiveMinimum = effectiveRequiredAndroidVersion (
            check.updatePolicy,
            check.reachedUpdateServer,
            installedVersionCode,
            check.immediateUpdateEligibility);

        if (check.updatePolicy.valid
             && check.updatePolicy.minimumVersionCode > installedVersionCode
             && installedVersionCode >= effectiveMinimum)
            juce::Logger::writeToLog (
                "UPDATE CANDIDATO NAO ARMADO: "
                + androidUpdateEligibilityDiagnostic (
                    check.immediateUpdateEligibility));

        if (installedVersionCode < effectiveMinimum)
        {
            foregroundRevalidationPending = false;
            requiredUpdateGateActive = true;
            requiredUpdateFloor = effectiveMinimum;
            startupWindow->panel.showRequiredUpdate (
                policyForRequiredGate (
                    std::move (check.updatePolicy), effectiveMinimum));
            return;
        }

        requiredUpdateGateActive = false;
        requiredUpdateFloor = 0;
       #endif

       #if JUCE_ANDROID
        // O Android nao permite descobrir com seguranca se os FLAC/WAV antigos
        // existem antes da permissao. Portanto esta e uma oferta opcional, sem
        // sondar Music/ChurchDAW e sem afirmar que arquivos foram encontrados.
        if (! legacyAccessWasHandled
            && ! ChurchDAWServices::legacyLibraryPromptWasHandled()
            && ! juce::RuntimePermissions::isGranted (
                    juce::RuntimePermissions::readMediaAudio))
        {
            auto lifetime = applicationAlive;
            startupWindow->panel.showLegacyAccess (
                [this, check, lifetime, safe]
                {
                    if (! lifetime->load() || safe.getComponent() == nullptr)
                        return;

                    juce::RuntimePermissions::request (
                        juce::RuntimePermissions::readMediaAudio,
                        [this, check, lifetime, safe] (bool granted) mutable
                        {
                            if (! lifetime->load() || safe.getComponent() == nullptr)
                                return;

                            legacyAccessWasHandled = true;
                            ChurchDAWServices::rememberLegacyLibraryPromptHandled();

                            if (! granted)
                            {
                                juce::Logger::writeToLog (
                                    "BIBLIOTECA LEGADA: acesso negado; "
                                    "o acesso pode ser solicitado depois em Bibliotecas");
                                handleStartupCheck (std::move (check));
                            }
                            else
                            {
                                // A primeira sondagem ocorreu antes da permissao
                                // e, corretamente, ignorou Music/ChurchDAW.
                                // Revalida apenas o disco em background; nao
                                // repete policy/catalogo nem trava a UI.
                                recheckLocalLibrary (std::move (check));
                            }
                        });
                },
                [this, check, lifetime, safe] () mutable
                {
                    if (! lifetime->load() || safe.getComponent() == nullptr)
                        return;

                    legacyAccessWasHandled = true;
                    ChurchDAWServices::rememberLegacyLibraryPromptHandled();
                    handleStartupCheck (std::move (check));
                });
            return;
        }
       #endif

        const bool hasInstalledLibrary =
           #if JUCE_ANDROID
            androidLocalLibraryReady;
           #else
            check.localLibraryChecked
                ? check.localLibraryReady
                : false;
           #endif
        // Android: entrar SEM baixar e baixar so o que quiser depois
        // (timbres 1 a 1 no seletor, ou pack completo em Bibliotecas).
        // "Abrir sem baixar" grava starterChoice e as aberturas seguintes
        // nao pedem de novo. Demo/trial tambem nao obriga biblioteca.
        const bool mayContinueWithoutLibrary =
            ChurchDAWServices::starterChoiceWasCompleted();

        if (hasInstalledLibrary || mayContinueWithoutLibrary)
        {
            startupWindow->panel.showReady();
            juce::Timer::callAfterDelay (350, [safe]
            {
                if (auto* ready = safe.getComponent())
                    if (ready->onContinue != nullptr)
                        ready->onContinue();
            });
            return;
        }

        // Primeira vez: download opcional + "Abrir sem baixar" sempre visivel.
        // requireInstalledLibrary = false em todas as plataformas Android.
        startupWindow->panel.showStarterChoice (
            check.manifest.starterPack(),
            check.reachedServer
           #if JUCE_ANDROID
            , false
           #endif
            );
    }

    MainComponent* currentMainContent() const noexcept
    {
       #if JUCE_IOS
        return startupWindow != nullptr ? startupWindow->content() : nullptr;
       #else
        return mainWindow != nullptr ? mainWindow->content() : nullptr;
       #endif
    }

    // Every runtime owner transition enters here. The application owns both
    // the gate and the single continuation, so they outlive MainComponent and
    // MainWindow. Multiple requests coalesce; the first continuation wins,
    // while a later terminal request upgrades the same completion to quit.
    void requestMainWindowTeardown (
        std::function<void()> continuation = {},
        bool quitAfterTeardown = false)
    {
        jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

        if (mainWindowTeardownPending)
        {
            quitAfterMainWindowTeardown =
                quitAfterMainWindowTeardown || quitAfterTeardown;
            return;
        }

        mainWindowTeardownPending = true;
        quitAfterMainWindowTeardown = quitAfterTeardown;
        mainWindowTeardownContinuation = std::move (continuation);
        (void) mainWindowTeardownGate.requestClose();

        if (auto* content = currentMainContent())
            content->beginOwnerTeardown();
    }

    bool mainWindowTeardownCanRun() const noexcept
    {
        if (! mainWindowTeardownGate.isClosedAndDrained())
            return false;
        if (auto* content = currentMainContent())
            return content->ownerTeardownIsReady();
        return true;
    }

    void destroyCurrentMainSurfaceAfterDrain()
    {
        jassert (mainWindowTeardownCanRun());
       #if JUCE_IOS
        if (startupWindow != nullptr)
            startupWindow->clearMainContent();
       #else
        mainWindow = nullptr; // fecha callbacks de audio/MIDI
       #endif
    }

    void handleAsyncUpdate() override
    {
        (void) mainWindowTeardownGate.consumeScheduledDrain();
        if (! mainWindowTeardownPending)
            return;

        if (! mainWindowTeardownCanRun())
        {
            startTimer (1);
            return;
        }

        stopTimer();
        destroyCurrentMainSurfaceAfterDrain();
        const bool reset = mainWindowTeardownGate.resetAfterDrain();
        jassert (reset);
        juce::ignoreUnused (reset);

        auto continuation = std::move (mainWindowTeardownContinuation);
        const bool shouldQuit = quitAfterMainWindowTeardown;
        mainWindowTeardownContinuation = {};
        mainWindowTeardownPending = false;
        quitAfterMainWindowTeardown = false;

        if (continuation != nullptr)
            continuation();
        if (shouldQuit)
            quit();
    }

    void timerCallback() override
    {
        if (! mainWindowTeardownPending)
        {
            stopTimer();
            return;
        }

        if (mainWindowTeardownCanRun())
        {
            stopTimer();
            triggerAsyncUpdate();
        }
    }

    void beginMainWindow()
    {
        juce::uint64 expectedDesktopLicenseEpoch = 0;
       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        expectedDesktopLicenseEpoch = desktopLicenseGateEpoch;
        if (! desktopLicenseGateAllowsOpen (expectedDesktopLicenseEpoch))
            return;
       #endif

        if (currentMainContent() != nullptr || startingMainWindow)
            return;

       #if JUCE_ANDROID
        // Se o app voltou do background durante qualquer etapa da abertura,
        // nunca transforme um resultado anterior ao retorno em acesso a DAW.
        // Refaca a politica/catalogo primeiro e so depois tente criar a janela.
        if (foregroundRevalidationPending)
        {
            beginStartupRevalidationBeforeMain();
            return;
        }
       #endif

        startingMainWindow = true;
        auto guard = applicationAlive;

        // A solicitacao nasce dentro de um botao/timer do proprio StartupPanel.
        // Destruir o painel nessa mesma pilha seria use-after-free; o proximo
        // evento faz a troca de janelas com seguranca.
        juce::MessageManager::callAsync (
            [this, guard, expectedDesktopLicenseEpoch]
        {
            if (! guard->load())
                return;

           #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
            if (! desktopLicenseGateAllowsOpen (
                    expectedDesktopLicenseEpoch))
            {
                startingMainWindow = false;
                return;
            }
           #endif

           #if JUCE_IOS
            // Cada TopLevelWindow vira um UIWindow/UIViewController nativo no
            // iOS. O StartupWindow continua como a unica janela raiz; depois
            // dos gates, apenas seu conteudo interno e trocado pela DAW.
            splash = nullptr;
            constexpr int nativeWindowTransitionDelayMs = 50;
           #else
            splash = std::make_unique<SplashWindow>();
            constexpr int nativeWindowTransitionDelayMs = 100;
            startupWindow = nullptr;
           #endif

            // Deixa a troca de janela terminar antes do construtor pesado.
            juce::Timer::callAfterDelay (
                nativeWindowTransitionDelayMs,
                [this, guard, expectedDesktopLicenseEpoch]
            {
                if (! guard->load())
                    return;

               #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
                if (! desktopLicenseGateAllowsOpen (
                        expectedDesktopLicenseEpoch))
                {
                    startingMainWindow = false;
                    splash = nullptr;
                    return;
                }
               #endif

                createMainWindow (expectedDesktopLicenseEpoch);
            });
        });
    }

    void createMainWindow (juce::uint64 expectedDesktopLicenseEpoch = 0)
    {
       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        if (! desktopLicenseGateAllowsOpen (expectedDesktopLicenseEpoch))
        {
            startingMainWindow = false;
            splash = nullptr;
            return;
        }
       #else
        juce::ignoreUnused (expectedDesktopLicenseEpoch);
       #endif

       #if JUCE_ANDROID
        // Cobre o intervalo entre beginMainWindow() e este callback (splash).
        if (foregroundRevalidationPending)
        {
            startingMainWindow = false;
            splash = nullptr;
            beginStartupRevalidationBeforeMain();
            return;
        }
       #endif

       #if JUCE_IOS
        if (startupWindow == nullptr)
        {
            startingMainWindow = false;
            return;
        }
        startupWindow->showMainContent (
            safeModeRequested, &mainWindowTeardownGate);
       #elif CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        auto guard = applicationAlive;
        mainWindow = std::make_unique<MainWindow> (
            getApplicationName(),
            [this, guard]
            {
                if (guard->load())
                    handleDesktopUserLogout();
            }, safeModeRequested, &mainWindowTeardownGate);
       #else
        mainWindow = std::make_unique<MainWindow> (
            getApplicationName(), std::function<void()> {}, safeModeRequested,
            &mainWindowTeardownGate);
       #endif

       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        if (! desktopLicenseGateAllowsOpen (expectedDesktopLicenseEpoch))
        {
            requestMainWindowTeardown (
                [this]
                {
                    startingMainWindow = false;
                    splash = nullptr;
                });
            return;
        }
       #endif

       #if JUCE_ANDROID
        if (androidTrialActive && androidTrialExpiresAt > 0)
            demoExpiryGuard = std::make_unique<DemoExpiryGuard> (
                *this, androidTrialExpiresAt);
       #endif

        if (splash != nullptr && currentMainContent() != nullptr)
            gate = std::make_unique<LoadGate> (*splash, *currentMainContent(),
                                               [this] { splash = nullptr; });
        else
            juce::Timer::callAfterDelay (900, [this] { splash = nullptr; });
    }

    void suspended() override
    {
       #if JUCE_IOS || JUCE_ANDROID
        if (auto* content = currentMainContent()) content->suspendChurchCam();
       #endif
       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        if (licenseClient != nullptr)
            licenseClient->applicationSuspended();
       #endif

       #if JUCE_ANDROID
        foregroundReturnPending = true;
        // Marque no momento em que o app sai: um cold check/timer pode terminar
        // enquanto a Activity ainda esta em segundo plano.
        foregroundRevalidationPending = true;
       #endif
    }

    void resumed() override
    {
       #if JUCE_IOS || JUCE_ANDROID
        if (auto* content = currentMainContent())
        {
            content->resumeChurchCam();
           #if JUCE_IOS
            content->ensureMobileAudioRunningAfterResume();
           #endif
        }
       #endif
        // Em macOS/Windows/Android o controlador pode voltar do sleep sem o
        // evento que recentraliza pitch bend. O reset e enfileirado no motor e
        // ocorre no proximo bloco, antes de o musico voltar a tocar.
        if (auto* content = currentMainContent())
            content->resetMidiTransportState();

       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        // Sleep pode atravessar o prazo de lease ou esconder uma substituicao
        // de sessao. A thread unica acorda e faz um status imediato.
        if (licenseClient != nullptr)
            licenseClient->applicationResumed();
       #endif

       #if JUCE_ANDROID
        if (! foregroundReturnPending)
            return;

        foregroundReturnPending = false;
        auto guard = applicationAlive;

        // O callback de lifecycle vem da Activity. Enfileirar a troca de janelas
        // mantem toda a UI no mesmo fluxo do MessageManager do JUCE.
        juce::MessageManager::callAsync ([this, guard]
        {
            if (guard->load())
                handleAndroidForegroundReturn();
        });
       #endif
    }

    void handleAndroidForegroundReturn()
    {
       #if JUCE_ANDROID
        if (! requiredUpdateGateActive)
        {
            if (mainWindow == nullptr || startupWindow != nullptr
                 || foregroundUpdateCheck != nullptr)
            {
                // Pode ser cold check, escolha/instalacao inicial, splash ou uma
                // consulta de foreground ainda em andamento. O gate sera
                // revalidado antes de liberar/criar a MainWindow.
                foregroundRevalidationPending = true;
                return;
            }

            foregroundRevalidationPending = false;
            beginForegroundUpdateCheck();
            return;
        }

        const int installedVersionCode = getInstalledAndroidVersionCode();

        if (installedVersionCode < requiredUpdateFloor)
            return;

        // O instalador do Android normalmente reinicia o processo. Se concluir
        // sem mata-lo, confirme o versionCode local e refaca o cold
        // check para liberar o gate sem exigir que o usuario feche o app.
        ChurchDAWServices::UpdatePolicy noNewPolicy;
        effectiveRequiredAndroidVersion (
            noNewPolicy, false, installedVersionCode, eligibilityQueryFailed);
        foregroundRevalidationPending = false;
        requiredUpdateGateActive = false;
        requiredUpdateFloor = 0;

        if (startupWindow == nullptr)
        {
            auto logo = loadChurchDAWBrandImage();
            startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        }

        startupWindow->panel.onContinue = [this]
        {
            if (androidLicenseGatePassed)
                requestMainWindowAfterLicense();
            else
                beginStartupAfterUpdateGate();
        };
        startupWindow->panel.showChecking();
        startStartupCheck();
       #endif
    }

    void beginStartupRevalidationBeforeMain()
    {
       #if JUCE_ANDROID
        foregroundRevalidationPending = false;
        startingMainWindow = false;
        splash = nullptr;

        if (! androidLicenseGatePassed)
        {
            startupWindow = nullptr;
            beginStartupAfterUpdateGate();
            return;
        }

        if (startupWindow == nullptr)
        {
            auto logo = loadChurchDAWBrandImage();
            startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        }

        startupWindow->panel.onContinue = [this] { requestMainWindowAfterLicense(); };
        startupWindow->panel.showChecking();
        startStartupCheck();
       #endif
    }

    void beginForegroundUpdateCheck()
    {
       #if JUCE_ANDROID
        if (mainWindow == nullptr || startupWindow != nullptr
             || foregroundUpdateCheck != nullptr)
            return;

        // A DAW NAO pode sumir atras de uma checagem de rede toda vez que o
        // app volta ao primeiro plano no meio do culto. A verificacao roda em
        // segundo plano e a janela principal continua visivel e utilizavel;
        // so se o piso exigir update e que o gate aparece (handleForeground
        // UpdateCheck cuida disso). Antes, setEnabled(false)/setVisible(false)
        // escondiam a DAW inteira durante a consulta.
        auto guard = applicationAlive;
        startForegroundUpdateCheckThread (std::move (guard));
       #endif
    }

    void startForegroundUpdateCheckThread (
        std::shared_ptr<std::atomic<bool>> guard)
    {
       #if JUCE_ANDROID
        retireCheckThreadNonBlocking (foregroundUpdateCheck);
        foregroundUpdateCheck =
            std::make_unique<ChurchDAWUpdatePolicyCheckThread> (
                [this, guard] (ChurchDAWServices::UpdatePolicyCheck check)
                {
                    if (guard->load())
                        handleForegroundUpdateCheck (std::move (check));
                });
        foregroundUpdateCheck->startThread();
       #else
        juce::ignoreUnused (guard);
       #endif
    }

    void handleForegroundUpdateCheck (
        ChurchDAWServices::UpdatePolicyCheck check)
    {
       #if JUCE_ANDROID
        foregroundUpdateCheck = nullptr;

        if (mainWindow == nullptr)
            return;

        const int installedVersionCode = check.installedVersionCode > 0
                                           ? check.installedVersionCode
                                           : getInstalledAndroidVersionCode();
        const int effectiveMinimum = effectiveRequiredAndroidVersion (
            check.policy,
            check.reachedServer,
            installedVersionCode,
            check.immediateUpdateEligibility);

        if (check.policy.valid
             && check.policy.minimumVersionCode > installedVersionCode
             && installedVersionCode >= effectiveMinimum)
            juce::Logger::writeToLog (
                "UPDATE CANDIDATO NAO ARMADO NO FOREGROUND: "
                + androidUpdateEligibilityDiagnostic (
                    check.immediateUpdateEligibility));

        if (installedVersionCode < effectiveMinimum)
        {
            // Update obrigatorio: so AGORA a DAW sai de cena e o gate aparece.
            // A janela principal e destruida (fecha callbacks de audio/MIDI)
            // e o StartupWindow mostra o piso exigido.
            foregroundRevalidationPending = false;
            requiredUpdateGateActive = true;
            requiredUpdateFloor = effectiveMinimum;
            gate = nullptr;
            splash = nullptr;
            requestMainWindowTeardown (
                [this, effectiveMinimum,
                 policy = std::move (check.policy)] () mutable
                {
                    startingMainWindow = false;
                    if (startupWindow == nullptr)
                    {
                        auto logo = loadChurchDAWBrandImage();
                        startupWindow = std::make_unique<StartupWindow> (
                            std::move (logo));
                    }

                    startupWindow->panel.showRequiredUpdate (
                        policyForRequiredGate (
                            std::move (policy), effectiveMinimum));
                });
            return;
        }

        if (foregroundRevalidationPending)
        {
            // Outro suspend/resume ocorreu enquanto esta resposta estava em
            // voo. Mantenha a consulta e faca mais uma vez.
            foregroundRevalidationPending = false;
            startForegroundUpdateCheckThread (applicationAlive);
            return;
        }

        requiredUpdateGateActive = false;
        requiredUpdateFloor = 0;
        startupWindow = nullptr;
        mainWindow->setEnabled (true);
        mainWindow->setVisible (true);
        mainWindow->toFront (true);
       #else
        juce::ignoreUnused (check);
       #endif
    }

    // Confirma que o setup atual sobreviveu a vida real: promove a ultima
    // configuracao boa alguns segundos depois de abrir, e mostra o aviso de
    // recuperacao quando o boot anterior morreu.
    class ConfirmacaoAudioTimer : public juce::Timer
    {
    public:
        explicit ConfirmacaoAudioTimer (ChurchDAWApplication& dono) : owner (dono) {}
        void timerCallback() override
        {
            stopTimer();
            owner.mostrarAvisoRecuperacao();
        }
    private:
        ChurchDAWApplication& owner;
    };

    juce::String recoveryNotice;
    ConfirmacaoAudioTimer confirmacaoAudioTimer { *this };

    void mostrarAvisoRecuperacao()
    {
        if (recoveryNotice.isEmpty())
            return;

        juce::String titulo, corpo;
        if (recoveryNotice == "restaurada")
        {
            titulo = "Abrimos com o seu audio anterior";
            corpo = "O driver escolhido na ultima vez travou a abertura, entao "
                    "voltamos para a configuracao que funcionava antes. Se quiser "
                    "tentar de novo, abra Configuracoes de audio.";
        }
        else if (recoveryNotice == "descartada")
        {
            titulo = "Abrimos com o audio padrao";
            corpo = "O driver escolhido na ultima vez travou a abertura e nao "
                    "tinha uma configuracao anterior salva. Escolha o driver em "
                    "Configuracoes de audio.";
        }
        else
        {
            titulo = "Abrimos em modo seguro";
            corpo = "A ultima abertura terminou de forma anormal, entao o setup "
                    "de audio e a cadeia de plugins foram preservados em arquivos "
                    "*.recovered-* e nao foram restaurados. Reescolha o driver em "
                    "Configuracoes de audio.";
        }

        recoveryNotice = juce::String();
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8 (titulo.toRawUTF8()),
            juce::String::fromUTF8 (corpo.toRawUTF8()),
            juce::String::fromUTF8 ("Entendi"));
    }

   #if JUCE_WINDOWS || JUCE_MAC
    juce::File churchDAWSettingsFile() const
    {
        return churchDAWBootMarkerFile().getParentDirectory()
            .getChildFile ("ChurchDAW.settings");
    }
   #endif

    void shutdown() override
    {
       #if JUCE_WINDOWS || JUCE_MAC
        // Fechamento chegou ao fim: libera a guarda. O proximo boot restaura
        // o estado normal do musico. Crash aqui embaixo NAO re-apaga o
        // marker, entao o boot seguinte entra em MODO SEGURO por seguranca.
        // Fechamento limpo com audio de pe = o setup atual e confiavel.
        auto xmlFim = juce::parseXML (churchDAWSettingsFile());
        churchDAWPromoteGoodAudio (churchDAWSettingsFile(),
                                   xmlFim != nullptr
                                       && xmlFim->getIntAttribute ("audioSetupHealthy", 0) == 1);

        churchDAWBootMarkerFile().deleteFile();
        churchDAWSetRecoveryStrikes (0);
       #endif

        applicationAlive->store (false);
        cancelPendingUpdate();
        stopTimer();

        // Normal quit has already gone through requestMainWindowTeardown and
        // therefore arrives with no content. appWillTerminateByForce may call
        // shutdown directly on Apple platforms. In that process-terminal-only
        // case, never spin or destroy through an active vendor frame: leak the
        // single top-level surface and let process termination reclaim it.
        if (auto* content = currentMainContent())
        {
            (void) mainWindowTeardownGate.requestClose();
            content->beginOwnerTeardown();
            if (mainWindowTeardownCanRun())
            {
                destroyCurrentMainSurfaceAfterDrain();
                (void) mainWindowTeardownGate.resetAfterDrain();
            }
            else
            {
                juce::Logger::writeToLog (
                    "SHUTDOWN: process-terminal surface retained because a plug-in frame is active");
               #if JUCE_IOS
                (void) startupWindow.release();
               #else
                (void) mainWindow.release();
               #endif
            }
        }

        mainWindowTeardownPending = false;
        quitAfterMainWindowTeardown = false;
        mainWindowTeardownContinuation = {};
       #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
        loginWindow = nullptr;
        licenseClient = nullptr;
       #endif
       #if JUCE_ANDROID
        demoExpiryGuard = nullptr;
        androidLoginWindow = nullptr;
        androidLicenseClient = nullptr;
        ChurchDAWServices::setLibraryAuthorizationToken ({});
       #endif
        retireCheckThreadNonBlocking (startupCheck);
        retireCheckThreadNonBlocking (startupUpdateCheck);
        retireCheckThreadNonBlocking (foregroundUpdateCheck);
        gate = nullptr;          // para o timer antes de soltar a janela que ele consulta
        splash = nullptr;
        startupWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

        // O encerramento fica registrado: assim, da proxima vez que o app
        // sumir sozinho, o log diz se foi saida limpa ou queda.
        juce::Logger::writeToLog ("ChurchDAW encerrado normalmente");
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger = nullptr;
    }

    void memoryWarningReceived() override
    {
        juce::Logger::writeToLog ("MEMORY_WARNING: liberando caches descartaveis");
        if (auto* content = currentMainContent())
            content->handleMemoryWarning();
        juce::ImageCache::releaseUnusedImages();
    }

    void systemRequestedQuit() override
    {
        juce::Logger::writeToLog (
            "SHUTDOWN_REQUEST: JUCEApplication::systemRequestedQuit");
        requestMainWindowTeardown ({}, true);
    }

   #if JUCE_ANDROID
    // BACK fisico. Antes ele SAIA do app direto — fatal no meio do culto.
    // Regra: fecha a camada de cima (organ/Audio-MIDI/Bibliotecas); na tela
    // principal, so um segundo BACK em ate 2 s realmente sai. Teclado e
    // popup nunca chegam aqui (o proprio Android e o PopupMenu tratam).
    bool backButtonPressed() override
    {
        if (mainWindow != nullptr)
            if (auto* component = mainWindow->content())
                if (component->handleBackButton())
                {
                    lastBackPressMs = 0;
                    return true;
                }

        const auto now = juce::Time::getMillisecondCounter();

        if (lastBackPressMs != 0 && now - lastBackPressMs < 2000)
            return false;   // segundo BACK confirmado: comportamento padrao

        lastBackPressMs = now;
        return true;        // primeiro BACK na tela principal: segura
    }

    juce::uint32 lastBackPressMs = 0;
   #endif

    class MainWindow : public juce::DocumentWindow,
                       private juce::Timer
    {
    public:
        explicit MainWindow (
            const juce::String& name,
            std::function<void()> logoutActionToUse = {},
            bool safeModeToUse = false,
            ChurchDAWMidiMapping::DeferredOwnerTeardownGate*
                teardownGate = nullptr)
            : DocumentWindow (name,
                              NordLookAndFeel::background(),
                              DocumentWindow::allButtons),
              logoutAction (std::move (logoutActionToUse)),
              safeMode (safeModeToUse)
        {
            setUsingNativeTitleBar (false);
           #if JUCE_ANDROID || JUCE_IOS
            // Mobile ja possui o header touch do ChurchDAW. A titlebar da
            // DocumentWindow acrescentava outros 30 dp com icone, titulo e
            // botoes de desktop; no iPad ela ainda dividia o mesmo espaco com
            // relogio/Wi-Fi do sistema, deixando todos os textos sobrepostos.
            setTitleBarButtonsRequired (0, false);
            setTitleBarHeight (0);
           #else
            setTitleBarHeight (30);
            setTitleBarTextCentred (false);
            setIcon (loadChurchDAWBrandImage());
           #endif
            if (logoutAction != nullptr)
            {
                juce::Component::addAndMakeVisible (logoutButton);
                logoutButton.setTooltip (
                    "Encerra esta sessao e volta para o login");
                logoutButton.setColour (
                    juce::TextButton::buttonColourId,
                    juce::Colours::transparentBlack);
                logoutButton.setColour (
                    juce::TextButton::buttonOnColourId,
                    NordLookAndFeel::nordRed().withAlpha (0.55f));
                logoutButton.setColour (
                    juce::TextButton::textColourOffId,
                    NordLookAndFeel::txtSecondary());
                logoutButton.setColour (
                    juce::TextButton::textColourOnId,
                    juce::Colours::white);
                logoutButton.onClick = [this]
                {
                    if (logoutAction != nullptr)
                        logoutAction();
                };
            }
            holder = new ScaledHolder (safeMode, teardownGate);
            setContentOwned (holder, true);
            startTimer (250);
           #if JUCE_ANDROID || JUCE_IOS
            // A janela mobile ocupa a tela do aparelho e nao pode ser
            // redimensionada pelo usuario. Manter o resizer de desktop ativo
            // desenhava uma alca diagonal sobre o ultimo canal no Android.
            setResizable (false, false);
           #else
            setResizable (true, true);
            // Limite provisório até o peer nativo informar a moldura. O mínimo
            // final é derivado da work area logo abaixo.
            setResizeLimits (760, 480, 32000, 32000);
            centreWithSize (getWidth(), getHeight());
           #endif

            // Abre ocupando a AREA UTIL da tela. setFullScreen(true) dimensionava a
            // janela pela tela INTEIRA: a barra de titulo empurrava o conteudo pra
            // baixo e a barra de tarefas comia o resto — o fim da regua master
            // (Auto EQ Master / Punch) ficava fora da tela. userArea ja desconta a
            // barra de tarefas, entao tudo cabe em qualquer escala de DPI.
           #if JUCE_ANDROID || JUCE_IOS
           #if JUCE_ANDROID
            const auto bounds = currentAndroidWindowBounds();
            setBounds (! bounds.isEmpty()
                           ? bounds
                           : juce::Rectangle<int> (
                                 0, 0, 800, 360));
            startTimer (250);
           #else
            setFullScreen (true);
           #endif
            // No iOS, tornar o controller visivel e imediatamente depois
            // alterna-lo para fullscreen sobrepunha duas transicoes UIKit
            // ("Unbalanced calls to begin/end appearance transitions"). A
            // geometria deve estar pronta antes da primeira apresentacao.
            setVisible (true);
           #else
            setVisible (true);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                // userBounds e a area para o CONTEUDO. Com barra de titulo
                // NATIVA o JUCE posiciona a moldura acima dela — a janela
                // nascia em (-7,-30) e a barra com o X ficava FORA da tela:
                // o dono relatou em 30/07 que nao havia como fechar o
                // programa. setBoundsConstrained respeita a moldura e o
                // gerenciador de janelas, entao a barra fica sempre visivel.
                const auto workArea = display->userBounds.getLargestIntegerWithin();
                auto usable = workArea;
                juce::BorderSize<int> nativeFrame;

                // O bounds da DocumentWindow representa o conteudo. A moldura
                // nativa e somada por fora; sem desconta-la, 680 px de work area
                // viravam ~711 px de janela e o rodape acabava atras da taskbar.
                if (auto* peer = getPeer())
                {
                    nativeFrame = peer->getFrameSize();
                    usable = nativeFrame.subtractedFrom (usable);
                }

                auto targetContentBounds = usable;
                int minimumContentWidth = juce::jmin (960, usable.getWidth());
                int minimumContentHeight = juce::jmin (600, usable.getHeight());
                const auto qaWindow = juce::SystemStats::getEnvironmentVariable (
                    "CHURCHDAW_QA_WINDOW", {}).trim().toLowerCase();
                const int separator = qaWindow.indexOfChar ('x');

                if (separator > 0)
                {
                    const int requestedOuterWidth = qaWindow.substring (0, separator).getIntValue();
                    const int requestedOuterHeight = qaWindow.substring (separator + 1).getIntValue();

                    if (requestedOuterWidth > 0 && requestedOuterHeight > 0)
                    {
                        const int outerWidth = juce::jmin (requestedOuterWidth,
                                                          workArea.getWidth());
                        const int outerHeight = juce::jmin (requestedOuterHeight,
                                                           workArea.getHeight());
                        const auto targetOuter = juce::Rectangle<int> (outerWidth, outerHeight)
                                                     .withCentre (workArea.getCentre());
                        targetContentBounds = nativeFrame.subtractedFrom (targetOuter);
                        // No hardware 960x600 real, userBounds tambem reduziria os
                        // limites para este mesmo client. O override reproduz esse
                        // viewport numa tela maior sem o limitador expandir a QA.
                        minimumContentWidth = juce::jmin (minimumContentWidth,
                                                          targetContentBounds.getWidth());
                        minimumContentHeight = juce::jmin (minimumContentHeight,
                                                           targetContentBounds.getHeight());
                    }
                }

                setResizeLimits (minimumContentWidth, minimumContentHeight,
                                 32000, 32000);
                setBoundsConstrained (targetContentBounds);

                const auto actualClient = getBounds();
                const auto actualOuter = nativeFrame.addedTo (actualClient);
                const auto dimensions = juce::String ("WINDOW: outer=")
                    + juce::String (actualOuter.getWidth()) + "x"
                    + juce::String (actualOuter.getHeight()) + " client="
                    + juce::String (actualClient.getWidth()) + "x"
                    + juce::String (actualClient.getHeight()) + " workArea="
                    + juce::String (workArea.getWidth()) + "x"
                    + juce::String (workArea.getHeight());
                juce::Logger::writeToLog (dimensions);

                // Facilita a evidencia visual automatizada sem alterar o produto:
                // o titulo detalhado so aparece quando o QA pede um tamanho.
                if (qaWindow.isNotEmpty())
                    setName ("ChurchDAW [QA " + dimensions.fromFirstOccurrenceOf (
                                 "WINDOW: ", false, false) + "]");
            }
            else
            {
                setFullScreen (true);
            }
           #endif
        }

        void closeButtonPressed() override
        {
            juce::Logger::writeToLog (
                "SHUTDOWN_REQUEST: MainWindow::closeButtonPressed");
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void resized() override
        {
            juce::DocumentWindow::resized();
            if (logoutButton.isVisible())
            {
                constexpr int buttonWidth = 128;
                logoutButton.setBounds (
                    juce::jmax (4, getWidth() - buttonWidth - 112),
                    3, buttonWidth, 24);
                logoutButton.toFront (false);
            }
        }

        MainComponent* content() const
        {
            return holder != nullptr ? &holder->inner : nullptr;
        }

    private:
        void timerCallback() override
        {
           #if JUCE_ANDROID
            const auto bounds = currentAndroidWindowBounds();
            if (! bounds.isEmpty() && getBounds() != bounds)
                setBounds (bounds);
           #endif

           #if ! JUCE_ANDROID && ! JUCE_IOS
            const juce::File trigger ("/tmp/churchdaw_screenshot_trigger");
            if (trigger.existsAsFile())
            {
                trigger.deleteFile();
                auto snap = createComponentSnapshot (getLocalBounds(), true, 1.0f);
                juce::File out ("/Users/igorteixeira/.gemini/antigravity/scratch/churchdaw_app.png");
                out.deleteFile();
                juce::FileOutputStream stream (out);
                if (stream.openedOk())
                {
                    juce::PNGImageFormat png;
                    png.writeImageToStream (snap, stream);
                    juce::Logger::writeToLog ("SCREENSHOT: salvo em " + out.getFullPathName());
                }
            }

            const juce::File padTrigger ("/tmp/churchdaw_pad_trigger");
            if (padTrigger.existsAsFile())
            {
                padTrigger.deleteFile();
                if (holder != nullptr)
                    holder->inner.showPadWindow();
            }

            const juce::File midiMapTrigger ("/tmp/churchdaw_midi_map_trigger");
            if (midiMapTrigger.existsAsFile())
            {
                midiMapTrigger.deleteFile();
                if (holder != nullptr)
                    holder->inner.toggleMidiLearn();
            }
           #endif
        }

        // O layout foi desenhado para ~1280x672 px LOGICOS (tela 1920 no
        // Windows a 150% de DPI, onde o sistema ja entrega essa medida). Num
        // monitor a 100% -- caso tipico do macOS -- a janela tem 1920 px
        // logicos e a interface inteira encolhia para 2/3 do tamanho. Este
        // involucro da zoom no MainComponent ate a janela "valer" 1280x672,
        // reproduzindo em qualquer tela o mesmo tamanho visual do Windows.
        // No Windows a 150% a conta da 1.0 e nada muda.
        struct ScaledHolder : public juce::Component
        {
            explicit ScaledHolder (
                bool safeModeToUse,
                ChurchDAWMidiMapping::DeferredOwnerTeardownGate*
                    teardownGate)
                : inner (safeModeToUse, teardownGate)
            {
                addAndMakeVisible (inner);
            }

            void resized() override
            {
               #if JUCE_ANDROID || JUCE_IOS
                // No celular o dp do JUCE ja normaliza o tamanho fisico.
                // Encolher pela referencia 1280x672 do desktop deixava a UI
                // a 58% na paisagem (card de 760 ocupando 443 dp reais, 54%
                // da largura util) e a 32% no retrato -- e tornava
                // inalcancaveis os ramos moveis desenhados em dp
                // (mobilePortraitLayout, narrowPhone, paisagem curta), pois
                // o MainComponent nunca via menos de 672 de altura logica.
                // Medido no Redmi 406x904 via uiautomator em 30/07/2026.
                const float s = 1.0f;
               #else
                // Em janelas menores que a referencia, o MainComponent precisa
                // receber o tamanho fisico real para que seus breakpoints facam
                // reflow. Encolher abaixo de 1x mantinha o conteudo artificialmente
                // em >= 1280x672 e transformava 960x600 numa miniatura ilegivel.
                // Em telas maiores ainda ampliamos como antes, preservando a
                // densidade visual pretendida em monitores de alta resolucao.
                const float s = juce::jmax (1.0f,
                                            juce::jmin (getWidth()  / 1280.0f,
                                                        getHeight() / 672.0f));
               #endif
                inner.setTransform (juce::AffineTransform::scale (s));
                inner.setBounds (0, 0,
                                 (int) std::ceil (getWidth()  / s),
                                 (int) std::ceil (getHeight() / s));
            }

            MainComponent inner;
        };

        ScaledHolder* holder = nullptr;
        const bool safeMode = false;
        std::function<void()> logoutAction;
        juce::TextButton logoutButton { "Sair da conta" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    //==============================================================================
   #if JUCE_ANDROID
    class DemoExpiryGuard final : private juce::Timer
    {
    public:
        DemoExpiryGuard (
            ChurchDAWApplication& applicationToUse,
            juce::int64 expiresAtToUse)
            : application (applicationToUse),
              expiresAt (expiresAtToUse)
        {
            startTimer (1000);
        }

    private:
        void timerCallback() override
        {
            if (application.androidLicenseClient == nullptr)
                return;
            const auto now =
                application.androidLicenseClient
                    ->trustedNowSeconds();
            if (now <= 0)
            {
                stopTimer();
                application.handleDemoExpired();
                return;
            }
            const auto remaining = expiresAt - now;
            if (! warned && remaining > 0 && remaining <= 120)
            {
                warned = true;
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::InfoIcon,
                    "ChurchDAW Demo",
                    "Faltam 2 minutos. Seu audio continuara "
                    "normal ate o fim da demonstracao.");
            }
            if (remaining <= 0)
            {
                stopTimer();
                application.handleDemoExpired();
            }
        }

        ChurchDAWApplication& application;
        juce::int64 expiresAt = 0;
        bool warned = false;
    };

    void handleDemoExpired()
    {
        demoExpiryGuard = nullptr;
        if (mainWindow != nullptr
            && mainWindow->content() != nullptr)
            mainWindow->content()->beginLicenseFadeOut();

        auto guard = applicationAlive;
        juce::Timer::callAfterDelay (900, [this, guard]
        {
            if (! guard->load())
                return;
            gate = nullptr;
            splash = nullptr;
            startupWindow = nullptr;
            requestMainWindowTeardown (
                [this]
                {
                    startingMainWindow = false;
                    androidTrialActive = false;
                    androidTrialPreparationPending = false;
                    androidTrialReserveWaiting = false;
                    androidTrialStartWaiting = false;
                    androidTrialExpiresAt = 0;
                    androidLicenseGatePassed = false;
                    ChurchDAWServices::setLibraryAuthorizationToken ({});
                    if (androidLicenseClient != nullptr)
                        androidLicenseClient->signOut();
                    showAndroidLoginWindow();
                });
        });
    }
   #endif

    //==============================================================================
    // Segura a splash ate a DAW estar REALMENTE pronta: bibliotecas escaneadas e
    // samples decodificados. Antes a splash saia por tempo fixo (900 ms) e a janela
    // aparecia com os canais ainda vazios — quem tocasse nesse intervalo nao ouvia nada.
    class LoadGate : private juce::Timer
    {
    public:
        LoadGate (SplashWindow& s, MainComponent& c, std::function<void()> whenReady)
            : splash (&s), content (c), onReady (std::move (whenReady))
        {
            startTimer (tickMs);
        }

    private:
        void timerCallback() override
        {
            auto* sp = splash.getComponent();

            if (sp == nullptr)   // splash sumiu por outro caminho
            {
                stopTimer();
                return;
            }

            elapsedMs += tickMs;

            const bool busy = content.isStillLoading();

            // a barra persegue o alvo da fase atual, entao ela sempre anda um
            // pouco em vez de congelar num numero. Ja pronto, corre mais rapido
            // pra chegar visivelmente ao fim antes da splash sair.
            progress += (content.getLoadingProgress() - progress) * (busy ? 0.08f : 0.30f);
            sp->setProgress (progress);

            // "Tudo pronto" com a barra em 85% e uma contradicao visual na
            // primeira tela do app -- o texto so muda quando a barra chegou la
            sp->setStatus (busy            ? content.getLoadingDetail()
                         : progress < 0.97f ? juce::String::fromUTF8 ("Finalizando...")
                                            : juce::String::fromUTF8 ("Tudo pronto"));

            // Exige varios ciclos parados EM SEQUENCIA: entre "biblioteca escaneada"
            // e "samples comecam a decodificar" existe um respiro em que nada parece
            // ocupado, e fechar ali entregaria a DAW pela metade.
            idleTicks = busy ? 0 : idleTicks + 1;

            if ((idleTicks >= idleTicksNeeded && progress >= 0.97f) || elapsedMs >= maxWaitMs)
            {
                stopTimer();

                if (onReady != nullptr)
                    onReady();
            }
        }

        static constexpr int tickMs = 100;
        static constexpr int idleTicksNeeded = 9;    // ~0,9 s parado sem nada pendente
        static constexpr int maxWaitMs = 90000;      // rede de seguranca: nunca trava na splash

        juce::Component::SafePointer<SplashWindow> splash;
        MainComponent& content;
        std::function<void()> onReady;
        float progress = 0.04f;
        int idleTicks = 0, elapsedMs = 0;
    };

    // Declarado ANTES do resto: e o primeiro a nascer e o ultimo a morrer, para
    // registrar tambem o que acontece durante a destruicao dos outros membros.
    std::unique_ptr<juce::FileLogger> fileLogger;
    std::unique_ptr<juce::Logger> stdoutLogger;   // so no self-test de CI

   #if CHURCHDAW_HOSTS_PLUGINS
    // Preenchido SOMENTE quando este processo e o filho que varre plugins.
    std::unique_ptr<ChurchDAWPluginScan::PluginScannerSubprocess>
        pluginScannerWorker;
   #endif
    NordLookAndFeel nordLookAndFeel;
   #if JUCE_ANDROID
    std::unique_ptr<AndroidLoginWindow> androidLoginWindow;
    std::unique_ptr<ChurchDAWAndroidLicensing::Client>
        androidLicenseClient;
    std::unique_ptr<DemoExpiryGuard> demoExpiryGuard;
    bool androidLocalLibraryReady = false;
    bool androidLicenseGatePassed = false;
    bool androidTrialPreparationPending = false;
    bool androidTrialReserveWaiting = false;
    bool androidTrialStartWaiting = false;
    bool androidTrialActive = false;
    juce::int64 androidTrialExpiresAt = 0;
   #endif
   #if CHURCHDAW_DESKTOP_LICENSING && (JUCE_WINDOWS || JUCE_MAC)
    std::unique_ptr<DesktopLoginWindow> loginWindow;
    std::unique_ptr<ChurchDAWDesktopLicensing::Client> licenseClient;
    juce::uint64 desktopLicenseGateEpoch = 0;
    bool licenseGatePassed = false;
    bool desktopTerminalTransitionPending = false;
    bool desktopReturnToMainAfterLogin = false;
    // Licenca vencida com som tocando: adia o encerramento ate o silencio
    // (no maximo 30 min) e avisa quando a janela offline esta perto do fim.
    juce::int64 desktopTerminationDeferredAtMs = 0;
    bool licenseExpiryWarned = false;
   #endif
    ChurchDAWMidiMapping::DeferredOwnerTeardownGate
        mainWindowTeardownGate;
    std::function<void()> mainWindowTeardownContinuation;
    bool mainWindowTeardownPending = false;
    bool quitAfterMainWindowTeardown = false;
    std::unique_ptr<SplashWindow> splash;
    std::unique_ptr<StartupWindow> startupWindow;
    std::unique_ptr<ChurchDAWStartupCheckThread> startupCheck;
    std::unique_ptr<ChurchDAWUpdatePolicyCheckThread> startupUpdateCheck;
    std::unique_ptr<ChurchDAWUpdatePolicyCheckThread> foregroundUpdateCheck;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<LoadGate> gate;
    std::shared_ptr<std::atomic<bool>> applicationAlive =
        std::make_shared<std::atomic<bool>> (true);
    bool startingMainWindow = false;
    bool safeModeRequested = false;
    bool legacyAccessWasHandled = false;
    bool foregroundReturnPending = false;
    bool foregroundRevalidationPending = false;
    bool requiredUpdateGateActive = false;
    int requiredUpdateFloor = 0;
};

START_JUCE_APPLICATION (ChurchDAWApplication)
