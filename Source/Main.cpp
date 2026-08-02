#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>   // logger de stdout do self-test de CI

#include "NordLookAndFeel.h"
#include "MainComponent.h"
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

    const auto clazz = ChurchDAWServices::findAndroidAppClass (
        env, activity.get(),
        "com.churchdaw.app.ChurchDAWLicenseBridge");
    if (clazz == nullptr)
    {
        env->ExceptionClear();
        return {};
    }

    const auto method = env->GetStaticMethodID (
        clazz, "getWindowBoundsDp",
        "(Landroid/app/Activity;)J");
    if (method == nullptr)
    {
        env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return {};
    }

    const auto packed = static_cast<juce::uint64> (
        env->CallStaticLongMethod (
            clazz, method, activity.get()));
    const bool ok = ! env->ExceptionCheck();
    if (! ok)
        env->ExceptionClear();
    env->DeleteLocalRef (clazz);
    if (! ok)
        return {};

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
       #if JUCE_ANDROID
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

private:
    void timerCallback() override
    {
       #if JUCE_ANDROID
        const auto bounds = currentAndroidWindowBounds();
        if (! bounds.isEmpty() && getBounds() != bounds)
            setBounds (bounds);
       #endif
    }

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
        setUsingNativeTitleBar (true);
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
        signalThreadShouldExit();
        stopThread (30000);
    }

    void run() override
    {
        auto check = useSeededCheck
                         ? std::move (seededCheck)
                         : ChurchDAWServices::checkRemoteManifest();

        if (threadShouldExit())
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

        if (threadShouldExit())
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
        signalThreadShouldExit();
        stopThread (20000);
    }

    void run() override
    {
        auto check = ChurchDAWServices::checkRemoteUpdatePolicy();

        if (threadShouldExit())
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

        if (threadShouldExit())
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
};

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
            return "APK assinado confirmado pela policy do ChurchDAW";
        case eligibilityNotRequired:
            return "versao instalada ja suficiente";
        case eligibilityWebsiteUnavailable:
            return "site de atualizacao indisponivel";
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
    // minimo, o processo de release publica o APK assinado no churchdaw.com e
    // valida sua resposta HTTPS; o gate abre esse APK para o instalador Android.
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

//==============================================================================
class ChurchDAWApplication : public juce::JUCEApplication
{
public:
    ChurchDAWApplication() = default;

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

        juce::LookAndFeel::setDefaultLookAndFeel (&nordLookAndFeel);

        // traduz os textos internos do JUCE (seletor de audio/MIDI) pra pt-BR
        juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (juce::String::fromUTF8 (
            "language: pt-BR\n"
            "countries: br\n"
            "\"Audio device type:\" = \"Tipo de driver:\"\n"
            "\"Device:\" = \"Dispositivo:\"\n"
            "\"Output:\" = \"Sa\xc3\xad" "da:\"\n"
            "\"Input:\" = \"Entrada:\"\n"
            "\"Active output channels:\" = \"Sa\xc3\xad" "das ativas:\"\n"
            "\"Active input channels:\" = \"Entradas ativas:\"\n"
            "\"Sample rate:\" = \"Taxa de amostragem:\"\n"
            "\"Audio buffer size:\" = \"Tamanho do buffer:\"\n"
            "\"Control Panel\" = \"Painel do driver\"\n"
            "\"Opens the device's own control panel\" = \"Abre o painel de controle da interface\"\n"
            "\"Reset Device\" = \"Resetar\"\n"
            "\"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel\" = \"Reinicia a interface de \xc3\xa1udio\"\n"
            "\"Test\" = \"Testar\"\n"
            "\"Plays a test tone\" = \"Toca um tom de teste\"\n"
            "\"Active MIDI inputs:\" = \"Entradas MIDI ativas:\"\n"
            "\"MIDI Output:\" = \"Sa\xc3\xad" "da MIDI:\"\n"
            "\"No MIDI inputs available\" = \"Nenhuma entrada MIDI\"\n"
            "\"(no audio output channels found)\" = \"(sem canais de sa\xc3\xad" "da)\"\n"
            "\"(no audio input channels found)\" = \"(sem canais de entrada)\"\n"
            "\"none\" = \"nenhum\"\n"
            "\"Show advanced settings...\" = \"Mostrar avan\xc3\xa7" "adas...\"\n"
            "\"Error when trying to open audio device!\" = \"Erro ao abrir o dispositivo de \xc3\xa1udio!\"\n"
            "\"OK\" = \"OK\"\n"
            // janela de plugins (PluginListComponent) — vinha toda em ingles
            "\"Name\" = \"Nome\"\n"
            "\"Format\" = \"Formato\"\n"
            "\"Category\" = \"Categoria\"\n"
            "\"Manufacturer\" = \"Fabricante\"\n"
            "\"Description\" = \"Descri\xc3\xa7\xc3\xa3o\"\n"
            "\"Options...\" = \"Op\xc3\xa7\xc3\xb5" "es...\"\n"
            "\"Scan\" = \"Escanear\"\n"
            "\"Cancel\" = \"Cancelar\"\n"
            "\"Clear list\" = \"Limpar lista\"\n"
            // o JUCE troca XFMTX pelo nome do formato DEPOIS de traduzir
            "\"Scan for new or updated XFMTX plug-ins\" = \"Escanear plugins XFMTX novos ou atualizados\"\n"
            "\"Remove all XFMTX plug-ins\" = \"Remover todos os plugins XFMTX\"\n"
            "\"Remove selected plug-in from list\" = \"Remover o plugin selecionado\"\n"
            "\"Remove plug-in from list\" = \"Remover o plugin da lista\"\n"
            "\"Remove any plug-ins whose files no longer exist\" = \"Remover plugins que n\xc3\xa3o existem mais\"\n"
            "\"Show folder containing selected plug-in\" = \"Abrir a pasta do plugin selecionado\"\n"
            "\"Show folder containing plug-in\" = \"Abrir a pasta do plugin\"\n"
            "\"Select folders to scan...\" = \"Escolha as pastas para escanear...\"\n"
            "\"Scanning for plug-ins...\" = \"Procurando plugins...\"\n"
            "\"Searching for all possible plug-in files...\" = \"Vasculhando os arquivos de plugin...\"\n"
            "\"Testing\" = \"Testando\"\n"
            "\"Scan complete\" = \"Escaneamento conclu\xc3\xad" "do\"\n"
            "\"Plugin Scanning\" = \"Escaneamento de plugins\"\n"
            "\"Deactivated after failing to initialise correctly\" = \"Desativado: consulte etapa, chamada e c\xc3\xb3" "digo no relat\xc3\xb3rio de scan\"\n"
            "\"The following files encountered fatal errors during validation\" = \"Falhas isoladas (crash, timeout ou incompatibilidade). O motivo exato foi salvo em Library/Logs/ChurchDAW/PluginScan\"\n"
            "\"The following files appeared to be plugin files, but failed to load correctly\" = \"Plugins recusados na valida\xc3\xa7\xc3\xa3o. Consulte etapa, chamada, c\xc3\xb3" "digo e stack no relat\xc3\xb3rio de scan\"\n"), false));

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
            androidLoginWindow->panel.onBuy = []
            {
                ChurchDAWAndroidLicensing::Client::
                    purchaseUrl().launchInDefaultBrowser();
            };
            androidLoginWindow->panel.onSupport = []
            {
                juce::URL (
                    "https://churchdaw.com/#duvidas")
                    .launchInDefaultBrowser();
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
        licenseClient =
            std::make_unique<ChurchDAWDesktopLicensing::Client>();
        loginWindow = std::make_unique<DesktopLoginWindow> (
            loadChurchDAWBrandImage());

        loginWindow->panel.onLogin =
            [this] (juce::String email,
                    juce::String password,
                    bool remember)
            {
                if (licenseClient != nullptr)
                    licenseClient->login (std::move (email),
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

        auto guard = applicationAlive;
        licenseClient->onSnapshot =
            [this, guard] (
                const ChurchDAWDesktopLicensing::Snapshot& snapshot)
            {
                if (! guard->load() || loginWindow == nullptr)
                    return;

                loginWindow->panel.applySnapshot (snapshot);

                if (! snapshot.canOpenApp || licenseGatePassed)
                    return;

                licenseGatePassed = true;
                juce::Timer::callAfterDelay (280, [this, guard]
                {
                    if (! guard->load())
                        return;

                    loginWindow = nullptr;
                    beginPostLicenseStartup();
                });
            };

        licenseClient->resumeSavedSession();
    }
   #endif

    void startStartupCheck()
    {
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
        const bool mayContinueWithoutLibrary =
           #if JUCE_ANDROID
            ! androidTrialPreparationPending
                && ChurchDAWServices::starterChoiceWasCompleted();
           #else
            ChurchDAWServices::starterChoiceWasCompleted();
           #endif

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

        startupWindow->panel.showStarterChoice (
            check.manifest.starterPack(),
            check.reachedServer
           #if JUCE_ANDROID
            , androidTrialPreparationPending
           #endif
        );
    }

    void beginMainWindow()
    {
        if (mainWindow != nullptr || startingMainWindow)
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
        juce::MessageManager::callAsync ([this, guard]
        {
            if (! guard->load())
                return;

            splash = std::make_unique<SplashWindow>();
            startupWindow = nullptr;

            // Deixa a splash nova chegar a tela antes do construtor pesado.
            juce::Timer::callAfterDelay (100, [this, guard]
            {
                if (guard->load())
                    createMainWindow();
            });
        });
    }

    void createMainWindow()
    {
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

        mainWindow = std::make_unique<MainWindow> (getApplicationName());

       #if JUCE_ANDROID
        if (androidTrialActive && androidTrialExpiresAt > 0)
            demoExpiryGuard = std::make_unique<DemoExpiryGuard> (
                *this, androidTrialExpiresAt);
       #endif

        if (splash != nullptr && mainWindow->content() != nullptr)
            gate = std::make_unique<LoadGate> (*splash, *mainWindow->content(),
                                               [this] { splash = nullptr; });
        else
            juce::Timer::callAfterDelay (900, [this] { splash = nullptr; });
    }

    void suspended() override
    {
       #if JUCE_ANDROID
        foregroundReturnPending = true;
        // Marque no momento em que o app sai: um cold check/timer pode terminar
        // enquanto a Activity ainda esta em segundo plano.
        foregroundRevalidationPending = true;
       #endif
    }

    void resumed() override
    {
        // Em macOS/Windows/Android o controlador pode voltar do sleep sem o
        // evento que recentraliza pitch bend. O reset e enfileirado no motor e
        // ocorre no proximo bloco, antes de o musico voltar a tocar.
        if (mainWindow != nullptr && mainWindow->content() != nullptr)
            mainWindow->content()->resetMidiTransportState();

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

        startupWindow->panel.onContinue = [this] { beginMainWindow(); };
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

        if (startupWindow == nullptr)
        {
            auto logo = loadChurchDAWBrandImage();
            startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        }

        startupWindow->panel.onContinue = [this] { beginMainWindow(); };
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

        auto logo = loadChurchDAWBrandImage();
        startupWindow = std::make_unique<StartupWindow> (std::move (logo));
        startupWindow->panel.showChecking();
        startupWindow->panel.onContinue = [this] { beginMainWindow(); };

        // Nao desmonta o motor durante uma verificacao normal: o audio pode
        // continuar estavel, mas nenhuma funcao da DAW fica acessivel. Se o piso
        // exigir update, a janela principal e destruida abaixo e o audio para.
        mainWindow->setEnabled (false);
        mainWindow->setVisible (false);

        auto guard = applicationAlive;
        startForegroundUpdateCheckThread (std::move (guard));
       #endif
    }

    void startForegroundUpdateCheckThread (
        std::shared_ptr<std::atomic<bool>> guard)
    {
       #if JUCE_ANDROID
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

        if (mainWindow == nullptr || startupWindow == nullptr)
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
            foregroundRevalidationPending = false;
            requiredUpdateGateActive = true;
            requiredUpdateFloor = effectiveMinimum;
            gate = nullptr;
            splash = nullptr;
            mainWindow = nullptr; // fecha callbacks de audio/MIDI antes do gate
            startingMainWindow = false;
            startupWindow->panel.showRequiredUpdate (
                policyForRequiredGate (
                    std::move (check.policy), effectiveMinimum));
            return;
        }

        if (foregroundRevalidationPending)
        {
            // Outro suspend/resume ocorreu enquanto esta resposta estava em
            // voo. Mantenha UI/audio inacessiveis e consulte mais uma vez.
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

    void shutdown() override
    {
        applicationAlive->store (false);
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
        startupCheck = nullptr;
        startupUpdateCheck = nullptr;
        foregroundUpdateCheck = nullptr;
        gate = nullptr;          // para o timer antes de soltar a janela que ele consulta
        splash = nullptr;
        startupWindow = nullptr;
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

        // O encerramento fica registrado: assim, da proxima vez que o app
        // sumir sozinho, o log diz se foi saida limpa ou queda.
        juce::Logger::writeToLog ("ChurchDAW encerrado normalmente");
        juce::Logger::setCurrentLogger (nullptr);
        fileLogger = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
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
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              NordLookAndFeel::background(),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            holder = new ScaledHolder();
            setContentOwned (holder, true);
            setResizable (true, true);
            // Limite provisório até o peer nativo informar a moldura. O mínimo
            // final é derivado da work area logo abaixo.
            setResizeLimits (760, 480, 32000, 32000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);

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
           #else
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
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
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
            ScaledHolder() { addAndMakeVisible (inner); }

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
            mainWindow = nullptr;
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
    bool licenseGatePassed = false;
   #endif
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
    bool legacyAccessWasHandled = false;
    bool foregroundReturnPending = false;
    bool foregroundRevalidationPending = false;
    bool requiredUpdateGateActive = false;
    int requiredUpdateFloor = 0;
};

START_JUCE_APPLICATION (ChurchDAWApplication)
