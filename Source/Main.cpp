#include <juce_gui_extra/juce_gui_extra.h>
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

    ~ChurchDAWStartupCheckThread() override
    {
        signalThreadShouldExit();
        stopThread (30000);
    }

    void run() override
    {
        auto check = ChurchDAWServices::checkRemoteManifest();

        if (threadShouldExit())
            return;

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
        return juce::ProjectInfo::versionString;
       #endif
    }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
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
        fileLogger.reset (juce::FileLogger::createDefaultAppLogger (
            "ChurchDAW", "ChurchDAW.log",
            juce::String ("ChurchDAW ")
                + getApplicationVersion() + " iniciado",
            512 * 1024));
        juce::Logger::setCurrentLogger (fileLogger.get());

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
            "\"Deactivated after failing to initialise correctly\" = \"Desativado: falhou ao iniciar\"\n"
            "\"The following files encountered fatal errors during validation\" = \"Estes arquivos deram erro grave na verifica\xc3\xa7\xc3\xa3o\"\n"
            "\"The following files appeared to be plugin files, but failed to load correctly\" = \"Estes pareciam plugins, mas n\xc3\xa3o carregaram\"\n"), false));

       #if JUCE_WINDOWS || JUCE_MAC
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

        const int installedVersionCode = check.installedVersionCode > 0
                                           ? check.installedVersionCode
                                           : getInstalledAndroidVersionCode();
        const int effectiveMinimum = effectiveRequiredAndroidVersion (
            check.policy,
            check.reachedServer,
            installedVersionCode,
            check.immediateUpdateEligibility);

        if (installedVersionCode < effectiveMinimum)
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
                if (androidLicenseClient != nullptr)
                    androidLicenseClient->reserveTrial();
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
        }
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
            androidTrialStartWaiting = true;
            startupWindow->panel.showChecking();
            androidLicenseClient->startReservedTrial();
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

    void handleStartupCheck (ChurchDAWServices::StartupCheck check)
    {
        if (startupWindow == nullptr)
            return;

        auto safe = juce::Component::SafePointer<StartupPanel> (&startupWindow->panel);

       #if JUCE_ANDROID
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
                                juce::Logger::writeToLog (
                                    "BIBLIOTECA LEGADA: acesso negado; "
                                    "o acesso pode ser solicitado depois em Bibliotecas");

                            handleStartupCheck (std::move (check));
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
            ChurchDAWServices::hasAnyInstalledLibrary();
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
            setResizeLimits (960, 600, 32000, 32000);
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
                setBounds (display->userBounds.toNearestInt());
            else
                setFullScreen (true);
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
                const float s = juce::jmax (1.0f,
                                            juce::jmin (getWidth()  / 1280.0f,
                                                        getHeight() / 672.0f));
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
    NordLookAndFeel nordLookAndFeel;
   #if JUCE_ANDROID
    std::unique_ptr<AndroidLoginWindow> androidLoginWindow;
    std::unique_ptr<ChurchDAWAndroidLicensing::Client>
        androidLicenseClient;
    std::unique_ptr<DemoExpiryGuard> demoExpiryGuard;
    bool androidLicenseGatePassed = false;
    bool androidTrialPreparationPending = false;
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
