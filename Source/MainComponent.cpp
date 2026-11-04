#include "MainComponent.h"
#include "Studio/StudioPanel.h"
#if CHURCHDAW_MOBILE_SHELL
static constexpr bool churchstudioMobileHeader = true;
#else
static constexpr bool churchstudioMobileHeader = false;
#endif
#include "NordLookAndFeel.h"
#include "ChurchDAWArtwork.h"
#include "DesktopHeaderLayout.h"
#include "PluginGalleryLayout.h"
#include "PluginProductIdentity.h"
#include "PluginPreviewLifecycle.h"
#include "GuardedAudioDeviceSelector.h"
#include "PluginCandidateSearch.h"
#include "TimbresProcessor.h"
#include "PadSynthProcessor.h"
#include "PadEditorPanel.h"
#include "PerformanceSessionPanel.h"
#include "ChordDetect.h"
#include "AboutPanel.h"
#include "LibraryPanel.h"
#include "MidiMappingSafety.h"
#include "AndroidCultoConnectivity.h"
#if JUCE_ANDROID
 #include "AndroidAudioMidiCenter.h"
 #include "AndroidPerformance.h"
 #include <unistd.h>
#endif
#if JUCE_ANDROID || JUCE_IOS
 #include "MobileAudioBufferPolicy.h"
#endif
#include "UiProfiler.h"
#include "BinaryData.h"

#include <cmath>

static_assert (TimbresProcessor::numInserts == 2,
               "MainComponent insert identity cache must cover every insert");

// Instrumentos internos (sampler de timbres). Os rotulos sao os da CASA: os
// acervos de origem nao aparecem na interface -- e clausula do acordo com o
// autor do acervo de pianos e evita colar marca de terceiro num produto a venda.
static constexpr int numInternalLibs = 4;
static constexpr int timbresComboId = 900000;   // +libraryIndex (0..3)
static const char* const timbresIdentifiers[numInternalLibs] = {
    "internal:timbres", "internal:timbres2", "internal:timbres3", "internal:timbres4" };
static const char* const timbresLabels[numInternalLibs] = {
    "Church Live (nativo)",
    "Church Live 2 (nativo)",
    "Church Live 3 - Pads (nativo)",
    "Church Live 4 (nativo)" };

// Handshake de vida entre a thread de MIDI e a message thread. A thread de MIDI
// resolve o TimbresProcessor por ponteiro cru (slots[i].proc.load() dentro de
// getChurchLive) e chama metodos nele (getForcedMidiChannelForDevice); o atomic
// so ordena a leitura do ponteiro, nao o tempo de vida do objeto. Sem este lock,
// unloadSlot/ troca de slot destroem o processador em paralelo e a thread de
// MIDI desreferencia memoria liberada — crash ao trocar o som do slot 0 com o
// teclado ainda mandando notas (pedal, aftertouch, active sensing). A thread de
// MIDI pega o lock no roteamento de canal de handleIncomingMidiMessage;
// unloadSlot e a troca assincrona pegam o mesmo lock antes de graph.removeNode,
// garantindo que nenhum callback de MIDI esteja dentro do trecho critico.
// CriticalSection e reentrante e so e disputado numa troca de slot (gesto
// manual, nao no meio de tocar) — bloqueio breve e aceitavel frente ao crash.
namespace { juce::CriticalSection liveHandshakeLock; }

#if JUCE_ANDROID
static bool isProbablyRunningOnAndroidEmulator()
{
    auto* env = juce::getEnv();
    if (env == nullptr)
        return false;

    juce::LocalRef<jclass> bridge (
        env->FindClass (
            "com/churchdaw/app/ChurchDAWLicenseBridge"));
    if (bridge.get() == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return false;
    }

    const auto method = env->GetStaticMethodID (
        bridge.get(), "isProbablyEmulator", "()Z");
    if (method == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return false;
    }

    const bool emulator =
        env->CallStaticBooleanMethod (bridge.get(), method) == JNI_TRUE;
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return false;
    }

    return emulator;
}

static juce::LocalRef<jclass> findAndroidAudioDeviceBridge (JNIEnv* env)
{
    juce::LocalRef<jclass> bridge (
        env->FindClass ("com/churchdaw/app/ChurchDAWAudioDeviceBridge"));

    if (bridge.get() == nullptr && env->ExceptionCheck())
        env->ExceptionClear();

    return bridge;
}

static juce::int64 getAndroidAudioTopologyGeneration()
{
    auto* env = juce::getEnv();
    if (env == nullptr)
        return 0;

    auto bridge = findAndroidAudioDeviceBridge (env);
    if (bridge.get() == nullptr)
        return 0;

    const auto method = env->GetStaticMethodID (
        bridge.get(), "getTopologyGeneration", "()J");

    if (method == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return 0;
    }

    const auto generation = static_cast<juce::int64> (
        env->CallStaticLongMethod (bridge.get(), method));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return 0;
    }

    return generation;
}

static juce::String getAndroidAudioTopologySnapshot()
{
    auto* env = juce::getEnv();
    if (env == nullptr)
        return "jni=unavailable";

    auto bridge = findAndroidAudioDeviceBridge (env);
    if (bridge.get() == nullptr)
        return "bridge=unavailable";

    const auto method = env->GetStaticMethodID (
        bridge.get(), "getSanitizedSnapshot", "()Ljava/lang/String;");

    if (method == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return "snapshot_method=unavailable";
    }

    juce::LocalRef<jstring> snapshot (static_cast<jstring> (
        env->CallStaticObjectMethod (bridge.get(), method)));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return "snapshot=error";
    }

    return snapshot.get() != nullptr ? juce::juceString (snapshot.get())
                                     : juce::String ("snapshot=empty");
}

static juce::String getAndroidUsbAudioTopologyFingerprint()
{
    auto* env = juce::getEnv();
    if (env == nullptr)
        return "jni=unavailable";

    auto bridge = findAndroidAudioDeviceBridge (env);
    if (bridge.get() == nullptr)
        return "bridge=unavailable";

    const auto method = env->GetStaticMethodID (
        bridge.get(), "getUsbTopologyFingerprint", "()Ljava/lang/String;");

    if (method == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return "fingerprint_method=unavailable";
    }

    juce::LocalRef<jstring> fingerprint (static_cast<jstring> (
        env->CallStaticObjectMethod (bridge.get(), method)));

    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return "fingerprint=error";
    }

    return fingerprint.get() != nullptr ? juce::juceString (fingerprint.get())
                                        : juce::String ("fingerprint=empty");
}

static bool isAndroidUsbAudioDeviceName (const juce::String& name)
{
    return name.containsIgnoreCase (" USB device")
        || name.containsIgnoreCase (" USB accessory")
        || name.containsIgnoreCase (" USB headset");
}
#endif

//==============================================================================
// ao destruir um editor manualmente, o processador PRECISA ser avisado;
// sem isso o proximo createEditorIfNeeded devolve um ponteiro morto
static void notifyProcessorEditorDeleted (juce::AudioProcessorEditor* editor)
{
    if (editor != nullptr)
        if (auto* ap = editor->getAudioProcessor())
            if (ap->getActiveEditor() == editor)
                ap->editorBeingDeleted (editor);
}

//==============================================================================
// Janela flutuante que hospeda a interface grafica de um plugin
//==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessorGraph::Node* node, std::function<void()> onClosed)
        : DocumentWindow (node->getProcessor()->getName(),
                          NordLookAndFeel::background(),
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          closedCallback (std::move (onClosed))
    {
        setUsingNativeTitleBar (false);
        setTitleBarHeight (30);
        setTitleBarTextCentred (false);

        // Sem setResizable a janela nasce travada no tamanho que o plugin pede —
        // um VSTi alto (Kontakt ~1000x750) ficava com o rodape fora da tela e
        // sem como encolher. Respeita o flag do editor do plugin quando ele
        // existe; o editor generico e sempre redimensionavel.
        if (auto* editor = node->getProcessor()->createEditorIfNeeded())
        {
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            setResizeLimits (200, 150, 3840, 2160);
        }
        else
        {
            setContentOwned (new juce::GenericAudioProcessorEditor (*node->getProcessor()), true);
            setResizable (true, false);
            setResizeLimits (200, 150, 2560, 1600);
        }

        setVisible (true);
        toFront (true);
    }

    ~PluginWindow() override
    {
        notifyProcessorEditorDeleted (dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()));
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        if (closedCallback)
            closedCallback();
    }

private:
    std::function<void()> closedCallback;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
};

//==============================================================================
MainComponent::MainComponent (
    bool safeModeToUse,
    ChurchDAWMidiMapping::DeferredOwnerTeardownGate*
        ownerTeardownGateToUse)
    : safeMode (safeModeToUse),
      ownerTeardownGate (ownerTeardownGateToUse),
      midiLearnTouchTimer (*this)
{
    setOpaque (true);

    if (safeMode)
        juce::Logger::writeToLog (
            "SAFE_MODE: ChurchDAW iniciou sem restaurar plugins de terceiros");

   #if JUCE_ANDROID
    // Liga o desenho pela GPU antes de qualquer componente existir, para nenhum
    // quadro chegar a passar pelo caminho de software. continuousRepainting
    // fica DESLIGADO de proposito: quem manda redesenhar continuam sendo os
    // medidores e o visor, cada um na sua cadencia -- redesenhar a 60 Hz sem
    // necessidade so trocaria um desperdicio por outro.
    // Ligado sempre. O A/B foi feito na MESMA build de release, so trocando
    // este caminho, navegando entre os mesmos menus:
    //
    //     software (padrao do JUCE) : 108% de CPU de interface
    //     GPU                       :  22%
    //
    // E antes disso, com dumpsys gfxinfo na release por software:
    //     Janky frames 118/118 (100%), 50o percentil 250 ms por quadro,
    //     "Slow bitmap uploads" em TODOS os quadros, GPU ociosa em 3-4 ms.
    //
    // O motivo: sem contexto GL o JUCE rasteriza a janela inteira por software
    // e envia como textura a cada quadro -- 2460x1080x4 = 10,6 MB por quadro,
    // na thread da interface. Era essa a lentidao ao navegar entre menus.
    //
    // Conferido que NAO era efeito do -O0: a medicao acima e da build release.
   #if JUCE_ANDROID
    if (! isProbablyRunningOnAndroidEmulator())
    {
        glContext.setContinuousRepainting (false);
        glContext.attachTo (*this);
    }
    else
    {
        // A imagem arm64 traduzida dos AVDs x86 pode perder os entry points de
        // textura durante a inicializacao do grafo e cair em
        // OpenGLTexture::create. Software aqui deixa a matriz automatizada
        // deterministica; aparelhos reais continuam no caminho GPU medido.
        juce::Logger::writeToLog (
            "ANDROID UI: renderer de software no emulador");
    }
   #else
    glContext.setContinuousRepainting (false);
    glContext.attachTo (*this);
   #endif
   #endif

    juce::PropertiesFile::Options options;
    options.applicationName = "ChurchDAW";
    options.filenameSuffix = "settings";
    options.folderName = "ChurchDAW";
    options.osxLibrarySubFolder = "Application Support";

    // O Windows resolve userApplicationDataDirectory pela Known Folder API,
    // portanto trocar APPDATA no processo nao isola os arquivos do JUCE. Este
    // override explicito e usado apenas pelos testes manuais/automatizados de
    // QA para que cenas, mapeamentos e a cadeia nunca encostem no perfil real.
    const auto qaSettingsDir = juce::SystemStats::getEnvironmentVariable (
                                   "CHURCHDAW_QA_SETTINGS_DIR", {})
                                   .trim();
    if (qaSettingsDir.isNotEmpty()
        && juce::File::isAbsolutePath (qaSettingsDir))
    {
        options.folderName = qaSettingsDir;
        juce::Logger::writeToLog ("QA: configuracoes isoladas em "
                                  + qaSettingsDir);
    }

    appProperties.setStorageParameters (options);
    launchkeyMappingEnabled = appProperties.getUserSettings()->getBoolValue (
        "launchkeyMappingEnabled", true);

   #if CHURCHDAW_HOSTS_PLUGINS
    if (! safeMode)
    {
    juce::addDefaultFormatsToManager (formatManager);

   #if ! JUCE_IOS
    // Cada plugin e aberto num processo filho. Um VST que trava (o "Piano
    // V3" no Mac do cliente, 31/07) derruba so o filho: a varredura marca
    // aquele arquivo e continua, em vez de fechar o ChurchDAW no meio.
    //
    // No iOS NAO ha processo filho — e nao precisa: AUv3 roda em extensao
    // isolada por desenho da Apple, entao o scan in-process ja e seguro.
    knownPlugins.setCustomScanner (
        std::make_unique<ChurchDAWPluginScan::ChurchDAWPluginScanner>());
   #endif

    if (auto xml = appProperties.getUserSettings()->getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    auto* pluginSettings = appProperties.getUserSettings();
    const int migratedPluginRoles =
        ChurchDAWPluginScan::normalizarPapeisDaListaConhecida (
            knownPlugins);
    const auto deadMansPedal = pluginSettings->getFile()
                                   .getSiblingFile ("PluginsComProblema.txt");

    // Versoes antigas entregavam Audio Units de infraestrutura da Apple ao
    // scanner. Esses IDs sao falsos positivos deterministas e podem sair ja.
    const int migratedBlacklist =
        ChurchDAWPluginScan::removerAudioUnitsDeSistemaDaBlacklist (knownPlugins);
    const int migratedDeadMansResult =
        ChurchDAWPluginScan::removerAudioUnitsDeSistemaDoDeadMansPedal (deadMansPedal);
    const bool policyDeadMansWriteOk = migratedDeadMansResult >= 0;
    const int migratedDeadMans = juce::jmax (0, migratedDeadMansResult);

    if (migratedBlacklist > 0 || migratedPluginRoles > 0)
    {
        if (auto migratedXml = knownPlugins.createXml())
            pluginSettings->setValue ("pluginList", migratedXml.get());
    }

    // BigSky follows the same isolated scan/quarantine policy as every AU.
    // Probing it here instantiated third-party code on the startup message
    // thread, before the saved blacklist/dead-man barrier had been restored.

    // Blacklists anteriores misturavam crash real com arquitetura, licenca,
    // timeout e falha do host. NAO a libera no startup: um tipo crashante
    // salvo continuaria no combo antes do scan. Apenas guarda backup/marker;
    // a remocao acontece
    // dentro do dialogo imediatamente antes da revalidacao isolada.
    constexpr int blacklistSemanticsVersion = 3;
    int legacyBlacklistCount = 0;
    int legacyDeadMansCount = 0;

    pluginScanQuarantine.addLines (
        pluginSettings->getValue ("pluginBlacklistMigrationQuarantine"));

    // O dead-man nao e apenas legado: o JUCE grava nele o candidato ANTES de
    // chama-lo. Se o app/OS cair no meio de um scan v2, esse ID precisa entrar
    // na barreira ja neste boot, antes de restoreChainState tentar instancia-lo.
    juce::StringArray startupDeadMans;
    bool startupDeadMansReadOk = ! deadMansPedal.existsAsFile();

    if (deadMansPedal.existsAsFile())
        if (auto input = deadMansPedal.createInputStream())
        {
            startupDeadMans.addLines (input->readEntireStreamAsString());
            startupDeadMansReadOk = true;
        }
    startupDeadMans.removeEmptyStrings();

    for (int i = startupDeadMans.size(); --i >= 0;)
        if (ChurchDAWPluginScan::ehAudioUnitDeSistema (
                startupDeadMans[i]))
            startupDeadMans.remove (i);

    startupDeadMans.removeDuplicates (true);

    for (const auto& identifier : startupDeadMans)
        knownPlugins.addToBlacklist (identifier);

    pluginScanQuarantine.addArray (startupDeadMans);

    if (! startupDeadMans.isEmpty()
         || ! startupDeadMansReadOk
         || ! policyDeadMansWriteOk)
    {
        const bool crashBackupReady = startupDeadMans.isEmpty()
            || deadMansPedal.getSiblingFile (
                   "PluginsComProblema-crash-backup.txt")
                   .replaceWithText (
                       startupDeadMans.joinIntoString ("\n"), true, true);

        pluginScanQuarantine.removeEmptyStrings();
        pluginScanQuarantine.removeDuplicates (true);
        pluginSettings->setValue (
            "pluginBlacklistMigrationQuarantine",
            pluginScanQuarantine.joinIntoString ("\n"));
        pluginSettings->setValue (
            "pluginBlacklistMigrationPending", true);
        pluginSettings->setValue (
            "pluginBlacklistMigrationBackupsReady",
            crashBackupReady && startupDeadMansReadOk
                && policyDeadMansWriteOk);

        if (auto blockedXml = knownPlugins.createXml())
            pluginSettings->setValue ("pluginList", blockedXml.get());

        juce::Logger::writeToLog (
            "PLUGIN_SCAN startup_dead_man_quarantined count="
            + juce::String (startupDeadMans.size())
            + " read_ok=" + juce::String (startupDeadMansReadOk ? 1 : 0));
    }

    if (ChurchDAWPluginScan::precisaRevalidarBlacklistLegada (
            pluginSettings->getIntValue ("pluginBlacklistSemanticsVersion", 0)))
    {
        const auto legacyBlacklist = knownPlugins.getBlacklistedFiles();
        legacyBlacklistCount = legacyBlacklist.size();
        pluginScanQuarantine.addArray (legacyBlacklist);

        auto legacyDeadMans = startupDeadMans;
        legacyDeadMansCount = legacyDeadMans.size();
        pluginScanQuarantine.addArray (legacyDeadMans);
        pluginScanQuarantine.removeEmptyStrings();
        pluginScanQuarantine.removeDuplicates (true);

        // Itens de infraestrutura Apple ja foram classificados por politica e
        // nao precisam entrar na quarentena de terceiros. Se a regravacao do
        // dead-man falhou, entretanto, a migracao inteira permanece bloqueada.
        for (int i = pluginScanQuarantine.size(); --i >= 0;)
            if (ChurchDAWPluginScan::ehAudioUnitDeSistema (
                    pluginScanQuarantine[i]))
                pluginScanQuarantine.remove (i);

        bool backupsReady = policyDeadMansWriteOk
                         && startupDeadMansReadOk;

        if (! pluginScanQuarantine.isEmpty())
        {
            const auto disabledBackup = pluginSettings->getFile()
                .getSiblingFile ("PluginsDesativados-v1-backup.txt");
            backupsReady = disabledBackup.replaceWithText (
                               pluginScanQuarantine.joinIntoString ("\n"),
                               true, true)
                        && backupsReady;

            if (! legacyDeadMans.isEmpty())
                backupsReady = deadMansPedal
                    .getSiblingFile ("PluginsComProblema-v1-backup.txt")
                    .replaceWithText (legacyDeadMans.joinIntoString ("\n"),
                                      true, true)
                            && backupsReady;
        }

        const bool needsRetry = ! pluginScanQuarantine.isEmpty()
                             || ! policyDeadMansWriteOk;
        pluginSettings->setValue ("pluginBlacklistMigrationQuarantine",
                                  pluginScanQuarantine.joinIntoString ("\n"));
        pluginSettings->setValue ("pluginBlacklistMigrationPending", needsRetry);
        pluginSettings->setValue ("pluginBlacklistMigrationBackupsReady",
                                  backupsReady);

        if (! needsRetry)
            pluginSettings->setValue ("pluginBlacklistSemanticsVersion",
                                      blacklistSemanticsVersion);

        if (needsRetry && ! backupsReady)
            juce::Logger::writeToLog (
                "PLUGIN_SCAN legacy_migration_blocked reason=backup_or_dead_man_write_failed");
    }

    pluginScanQuarantine.removeEmptyStrings();
    pluginScanQuarantine.removeDuplicates (true);

    pluginSettings->saveIfNeeded();

    if (legacyBlacklistCount > 0 || legacyDeadMansCount > 0
         || migratedBlacklist > 0 || migratedDeadMans > 0
         || migratedPluginRoles > 0)
        juce::Logger::writeToLog (
            "PLUGIN_SCAN legacy_blacklist_pending retry="
            + juce::String (legacyBlacklistCount)
            + " legacy_dead_mans=" + juce::String (legacyDeadMansCount)
            + " policy_blacklist=" + juce::String (migratedBlacklist)
            + " policy_dead_mans=" + juce::String (migratedDeadMans)
            + " role_migrations=" + juce::String (migratedPluginRoles));

    knownPlugins.addChangeListener (this);
    }
   #endif
    // No celular o formatManager fica sem formato nenhum de proposito. O resto
    // do codigo continua consultando knownPlugins normalmente -- ela so responde
    // "nenhum plugin", que e a verdade da plataforma. Por isso nao ha #if
    // espalhado pelo arquivo inteiro.

    // ---- monta o grafo de audio ----
    using IOProc = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    audioInNode  = graph.addNode (std::make_unique<IOProc> (IOProc::audioInputNode))->nodeID;
    audioOutNode = graph.addNode (std::make_unique<IOProc> (IOProc::audioOutputNode))->nodeID;
    midiInNode   = graph.addNode (std::make_unique<IOProc> (IOProc::midiInputNode))->nodeID;

    {
        auto masterNodePtr = graph.addNode (std::make_unique<MasterProcessor>());
        masterNode = masterNodePtr->nodeID;
        masterProcessor = static_cast<MasterProcessor*> (masterNodePtr->getProcessor());
    }

    {
        auto sabrinaNodePtr = graph.addNode (std::make_unique<SabrinaProcessor>());
        sabrinaNode = sabrinaNodePtr->nodeID;
        sabrinaProcessor = static_cast<SabrinaProcessor*> (sabrinaNodePtr->getProcessor());
        sabrinaProcessor->setSabrinaBypassed (! sabrinaEnabled);
    }

    {
        auto padNodePtr = graph.addNode (std::make_unique<PadProcessor>());
        padNode = padNodePtr->nodeID;
        padProcessor = static_cast<PadProcessor*> (padNodePtr->getProcessor());

        // Escutar o Pad e o que permite o MIDI Learn saber que o musico
        // encostou no corte ou no volume dele. Sem isto os knobs do Pad
        // ficavam invisiveis para o mapeamento. Removido no destrutor.
        //
        // Qualificado porque PadProcessor herda de AudioProcessor E de
        // juce::Thread, e as duas bases oferecem addListener.
        padProcessor->juce::AudioProcessor::addListener (this);
    }

    {
        auto padSynthNodePtr = graph.addNode (
            std::make_unique<PadSynthProcessor>());
        padSynthNode = padSynthNodePtr->nodeID;
        padSynthProcessor = static_cast<PadSynthProcessor*> (
            padSynthNodePtr->getProcessor());
        padSynthProcessor->addListener (this);
    }

    // Os quatro knobs da barra lateral parecem parte da referencia e agora
    // sao os mesmos parametros reais usados em Performance/PAD Editor.
    {
        const std::array<juce::AudioParameterFloat*, 4> macroParameters {
            padSynthProcessor->macroAmbience, padSynthProcessor->macroReverb,
            padSynthProcessor->macroDelay, padSynthProcessor->macroMaster
        };
        static constexpr const char* macroNames[] {
            "Ambi\xc3\xaancia", "Reverb", "Delay", "Master"
        };
        for (int i = 0; i < 4; ++i)
        {
            auto& slider = desktopMacroSliders[(size_t) i];
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setRange (0.0, 1.0, 0.001);
            slider.setDoubleClickReturnValue (true, 0.5);
            slider.setColour (juce::Slider::rotarySliderFillColourId,
                              NordLookAndFeel::nordRedBright());
            slider.setColour (juce::Slider::rotarySliderOutlineColourId,
                              juce::Colour (0xff343a44));
            slider.setMouseCursor (juce::MouseCursor::PointingHandCursor);
            slider.setTooltip (juce::String::fromUTF8 (macroNames[i]));
            addChildComponent (slider);
            desktopMacroAttachments[(size_t) i] =
                std::make_unique<juce::SliderParameterAttachment> (
                    *macroParameters[(size_t) i], slider);
            ChurchDAWMidiLearnTarget::tag (
                slider, *padSynthProcessor, *macroParameters[(size_t) i]);
        }
    }

    player.setProcessor (&graph);

   #if ! CHURCHDAW_UI_PREVIEW
    auto savedAudio = safeMode
        ? std::unique_ptr<juce::XmlElement>()
        : appProperties.getUserSettings()->getXmlValue ("audioSetup");

    // ZERO canais de ENTRADA no celular, e nao e economia: e o que faz haver som.
    //
    // Pedir entrada obriga o Android a exigir RECORD_AUDIO, e sem essa permissao
    // ele nao recusa so a entrada -- recusa o STREAM INTEIRO. O proprio Oboe
    // avisa no log: "AudioStreamAAudio.open() may have failed due to lack of
    // audio recording permission", e o app fica mudo, sem erro visivel na tela.
    //
    // O ChurchDAW e um sampler: ele nao grava nada. Pedir microfone seria pedir
    // ao comprador uma permissao que o app nao usa -- alem de assustar, e o tipo
    // de coisa que faz a Play Store barrar.
   #if JUCE_ANDROID || JUCE_IOS
    const bool initialAudioInputEnabled = false;
    const int numInputs = 0;
   #else
    // Nao basta desconectar o node do grafo: abrir duas entradas no ASIO4ALL
    // pode ativar monitoramento direto no driver/interface, fora do ChurchDAW,
    // e produzir microfonia mesmo com o master digital em silencio. Abra canais
    // fisicos somente quando o setup salvo pediu explicitamente a entrada.
    const auto savedChainForAudio = safeMode
        ? std::unique_ptr<juce::XmlElement>()
        : appProperties.getUserSettings()->getXmlValue ("chain");
    const bool initialAudioInputEnabled =
        ChurchDAWAudioInputPolicy::isExplicitlyAuthorised (
            savedChainForAudio.get());
    const int numInputs = initialAudioInputEnabled ? 2 : 0;
   #endif

    audioInputAuthorised.store (initialAudioInputEnabled,
                                std::memory_order_release);
    ChurchDAWAudioInputPolicy::applyToSavedSetup (
        savedAudio.get(), initialAudioInputEnabled);

    const auto audioInitialiseError =
        deviceManager.initialise (numInputs, 2, savedAudio.get(), true);

   #if ! JUCE_ANDROID && ! JUCE_IOS
    // Cinto e suspensorio: alguns drivers normalizam o setup durante open().
    // Reaplique a politica ao estado REAL antes de registrar o callback.
    const bool startupInputPolicyOk = reconcileAudioInputPolicy();
    const auto startupSetup = deviceManager.getAudioDeviceSetup();
    const auto* startupDevice = deviceManager.getCurrentAudioDevice();

   #if JUCE_WINDOWS || JUCE_MAC
    // Sinal para a promocao: sem device aberto nao ha o que promover.
    if (audioInitialiseError.isEmpty() && startupDevice != nullptr)
        appProperties.getUserSettings()->setValue ("audioSetupHealthy", 1);
    else
        appProperties.getUserSettings()->setValue ("audioSetupHealthy", 0);
   #endif
    const int startupActiveInputs = startupDevice != nullptr
        ? startupDevice->getActiveInputChannels().countNumberOfSetBits()
        : 0;
    juce::Logger::writeToLog (
        "AUDIO_STARTUP input_authorised="
        + juce::String (initialAudioInputEnabled ? 1 : 0)
        + " setup_inputs="
        + juce::String (startupSetup.inputChannels.countNumberOfSetBits())
        + " active_inputs=" + juce::String (startupActiveInputs)
        + " callback_input_forwarding="
        + juce::String (initialAudioInputEnabled ? 1 : 0)
        + " policy_ok=" + juce::String (startupInputPolicyOk ? 1 : 0)
        + (audioInitialiseError.isEmpty()
               ? juce::String()
               : " initialise_error=\"" + audioInitialiseError + "\""));
   #endif

   #if JUCE_IOS
    if (audioInitialiseError.isNotEmpty())
        juce::Logger::writeToLog (
            "AUDIO_IOS initialise_failed error=\"" + audioInitialiseError + "\"");
   #else
    juce::ignoreUnused (audioInitialiseError);
   #endif

   #if JUCE_ANDROID
    // O manager pode cair para System Default quando o USB salvo nao esta
    // conectado na largada. Guarde o nome solicitado para restaura-lo quando
    // a bridge anunciar que a interface reapareceu.
    if (savedAudio != nullptr)
    {
        const auto savedOutput = savedAudio->getStringAttribute ("audioOutputDeviceName");
        if (isAndroidUsbAudioDeviceName (savedOutput))
        {
            androidPreferredUsbSetup = deviceManager.getAudioDeviceSetup();
            androidPreferredUsbSetup.outputDeviceName = savedOutput;
            androidHasPreferredUsbSetup = true;
        }
    }

    updateAndroidPreferredUsbSetupFromCurrent();
   #endif

   #if JUCE_ANDROID || JUCE_IOS
    // Sem isto o app abre com 2048 samples (42,7 ms a 48 kHz) -- atraso que o
    // tecladista SENTE entre apertar a tecla e ouvir. O numero nao vem de
    // limitacao do aparelho: vem do padrao herdado do PC.
    //
    // O aparelho de teste (TANK 3, sem "Pro Audio", sem FastMixer) mesmo assim
    // negociou AAudio em LowLatency com rajada de 256 quadros = 5,3 ms. Pedir
    // exatamente a rajada e o menor buffer que nao produz falha de audio:
    // menor que ela o sistema so junta rajadas de novo, e maior e atraso puro.
    // NAO usar dev->getDefaultBufferSize() aqui: o JUCE mira 40 ms de proposito
    // nesse metodo (juce_HighPerformanceAudioHelpers_android.h:127), o que no
    // TANK 3 da 2048 samples = 42,7 ms. Foi exatamente o que o painel mostrou.
    //
    // A lista de getAvailableBufferSizes() comeca no tamanho NATIVO da rajada
    // (256 aqui = 5,3 ms) e sobe de rajada em rajada. Escolhemos a primeira que
    // chega a ~12 ms: 5–8 ms nao e audivelmente melhor no teclado de culto, e
    // a folga importa porque o app le samples do disco enquanto toca -- xrun
    // incomoda MUITO mais que 4 ms de atraso. Quem quiser apertar usa Rapida.
    // So vale como PADRAO DE PRIMEIRA ABERTURA. Se ja existe buffer salvo, e
    // escolha do usuario e nao se mexe -- alem disso o reforco logo abaixo
    // (o que existe por causa dos drivers ASIO teimosos) reaplica o valor salvo
    // depois daqui e desfaria qualquer coisa que fizessemos por cima dele.
    if (auto* dev = appProperties.getUserSettings()->getIntValue ("bufferSize", 0) == 0
                        ? deviceManager.getCurrentAudioDevice() : nullptr)
    {
        const auto sizes = dev->getAvailableBufferSizes();
       #if JUCE_ANDROID || JUCE_IOS
        androidAvailableBufferSizes = sizes;
       #endif
        const auto rate  = dev->getCurrentSampleRate() > 0.0 ? dev->getCurrentSampleRate() : 48000.0;

        const int chosen = ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
            sizes.getRawDataPointer(), sizes.size(), rate, 12.0);

        if (auto setup = deviceManager.getAudioDeviceSetup();
            chosen > 0 && setup.bufferSize != chosen)
        {
            setup.bufferSize = chosen;
            const auto error = deviceManager.setAudioDeviceSetup (setup, true);

           #if JUCE_IOS
            if (error.isNotEmpty())
                juce::Logger::writeToLog (
                    "AUDIO_IOS default_buffer_setup_failed requested="
                    + juce::String (chosen) + " error=\"" + error + "\"");
           #else
            juce::ignoreUnused (error);
           #endif
        }
    }
   #endif

   #if ! (JUCE_ANDROID || JUCE_IOS)
    // Desktop: na primeira abertura (sem buffer salvo) o JUCE cai no WASAPI
    // compartilhado, que oferece um UNICO tamanho de buffer e ainda soma o
    // buffer do mixer do sistema — atraso que o tecladista sente e o painel
    // Audio/MIDI nao deixa reduzir. Se ha ASIO (ou Windows Audio em modo Low
    // Latency, que monta lista real de buffers), preferimos esse tipo e
    // aplicamos o mesmo piso de ~10 ms do mobile. So vale como PADRAO DE
    // PRIMEIRA ABERTURA; escolha salva do usuario nunca e tocada — todo o
    // scan/corre aqui acontece ANTES de addAudioCallback, fora do caminho de
    // audio. setCurrentAudioDeviceType reabre o driver, entao so fazemos isso
    // quando ainda nao existe buffer persistido.
    //
    // "Ja tentei o ajuste" e "tenho buffer salvo" sao coisas DIFERENTES: se o
    // driver preferido nao abrir, nada e persistido e o bloco repetiria em toda
    // abertura, forcando ASIO por cima da escolha que o musico fez depois. Por
    // isso a marca propria e gravada ANTES de qualquer troca, e o tipo
    // anterior volta quando o preferido nao abre nenhum dispositivo.
    tuneDesktopFirstRunAudio();
   #endif
    // MainComponent e a barreira entre o driver e o AudioProcessorPlayer: alem
    // de medir a saida final, nunca encaminha ponteiros de entrada sem a
    // autorizacao persistida pelo toggle de guitarra/mic.
    deviceManager.addAudioCallback (this);
    deviceManager.addChangeListener (this);
   #endif

    masterProcessor->addChangeListener (this);   // sincroniza o fader com o Church Live
    masterProcessor->addListener (this);         // MIDI Learn do master por parametro

    midiMapFileLock =
        std::make_unique<ChurchDAWMidiMapping::MidiMapPairFileLock> (
            arquivoDoMapaDeCCs(), arquivoDoMapaDeNotas());

    // Recovery, both restores and the loaded revision are one indivisible
    // startup snapshot. Without this outer lock, another ChurchDAW process
    // could save between CC and note restore and this instance would combine
    // two different generations before its first screen was shown.
    {
        ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess startupAccess (
            *midiMapFileLock, 5000);
        if (! startupAccess.isLocked())
        {
            midiMapPairRecoveryFailed = true;
            juce::Logger::writeToLog (
                "MIDI MAP: nao foi possivel obter snapshot exclusivo de inicializacao");
        }
        else
        {
            if (exigirQuartetoMidiInspecionavel ("startup antes do journal"))
            {
                midiMapPairRecoveryFailed =
                    ! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                        arquivoDoMapaDeCCs(), arquivoDoMapaDeNotas(),
                        *midiMapFileLock);
            }

            if (! midiMapPairRecoveryFailed)
                (void) exigirQuartetoMidiInspecionavel (
                    "startup depois do journal");

            if (! midiMapPairRecoveryFailed
                 && ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
                        arquivoDoMapaDeCCs(), arquivoDoMapaDeNotas()))
            {
                // Future ownership is pair-wide. Do not repair/load the older
                // half while the opposite half belongs to a newer ChurchDAW.
                esquemaDeCCsDesconhecido = true;
                esquemaDeNotasDesconhecido = true;
                midiMapPairRecoveryFailed = true;
                juce::Logger::writeToLog (
                    "MIDI MAP: schema futuro detectado no par; ambos restores bloqueados");
            }

            if (! midiMapPairRecoveryFailed)
            {
                // Restaura antes de qualquer discovery. O cache de devices
                // continua vazio; o refresh posterior faz rekey/perfis.
                restoreMidiMap();
                if (! midiMapPairRecoveryFailed)
                {
                    midiMapLifecycle.ccLoaded = true;
                    restoreNoteMap();
                }
                if (! midiMapPairRecoveryFailed)
                {
                    midiMapLifecycle.noteLoaded = true;
                    if (! midiMapDiskRevision.refresh (
                            arquivoDoMapaDeCCs(), arquivoDoMapaDeNotas()))
                        midiMapPairRecoveryFailed = true;
                    else
                        midiMapsLoaded = true;
                }
            }
        }
    }

    if (midiMapPairRecoveryFailed)
    {
        midiMapPersistenceError.store (true, std::memory_order_release);
        juce::Logger::writeToLog (
            "MIDI MAP: snapshot/journal do par invalido; gravacao suspensa para proteger os mapas");
    }

    {
        const juce::ScopedLock sl (mapLock);
        const bool noteConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                ccMappings, noteMappings);
        const bool ccConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                ccMappings, ccActionMappings,
                [] (const AcaoDeCC& action)
                {
                    return action.acao.kind != NoteActionKind::none;
                });
        if (noteConflict || ccConflict)
        {
            automaticControllerProfilesSuspended = true;
            juce::Logger::writeToLog (
                "MIDI MAP: conflito nota/comando detectado; mapas comuns suspensos, "
                "mas Limpar/Importar continuam disponiveis para reparar");
        }
    }

    if (pendingLegacyCCMigration && saveMidiMap())
    {
        // saveMidiMap only returns true after primary + last-good were both
        // validated under the pair's interprocess lock.
        appProperties.getUserSettings()->removeValue ("midiMap");
        pendingLegacyCCMigration = false;
    }
    if (pendingLegacyNoteMigration && saveNoteMap())
    {
        appProperties.getUserSettings()->removeValue ("midiNoteMap");
        pendingLegacyNoteMigration = false;
    }

    // Habilita os dispositivos MIDI conectados, MENOS a porta DAW do Launchkey
    // quando o mapeamento automatico estiver ligado.
    //
    // Ela nao e um teclado: em modo DAW carrega pads e controles de superficie.
    // Deixando ela entrar como MIDI comum, apertar um pad tocava uma NOTA no
    // instrumento. Pior: o Windows abre porta MIDI em modo exclusivo, entao com o
    // deviceManager segurando a porta o LaunchkeyControl nao conseguia abri-la e
    // os faders/knobs/pads nem chegavam ao mapeamento.
    refreshMidiInputs();
    rebuildControllerRoutesIfNeeded (true);

    // ... E DE NOVO SEMPRE QUE A LISTA MUDAR.
    //
    // Chamar so aqui, no construtor, e o que fazia o teclado nao tocar no
    // Android: la a descoberta de MIDI e ASSINCRONA -- o sistema avisa por
    // callback depois. Na hora em que o construtor pergunta, a lista ainda esta
    // VAZIA (comprovado no log do aparelho: "MIDI: 0 entrada(s)"), mesmo com o
    // Launchkey plugado e o proprio Android ja enxergando ele. Segundos depois
    // a porta aparece -- e ninguem mais habilitava ela.
    //
    // No Windows isso nunca apareceu porque a lista ja esta pronta na largada.
    // O ganho vale para o PC tambem: plugar o teclado com o app ABERTO passa a
    // funcionar sozinho, sem reiniciar.
   #if ! CHURCHDAW_UI_PREVIEW
    const auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    const auto midiLifetime = midiCallbackLifetime;
    midiListConnection = juce::MidiDeviceListConnection::make ([safeThis, midiLifetime]
    {
        if (! midiLifetime->load())
            return;
        juce::MessageManager::callAsync ([safeThis, midiLifetime]
        {
            if (midiLifetime->load())
                if (auto* c = safeThis.getComponent())
                c->refreshMidiInputs();
        });
    });
   #endif

    // O MainComponent e o UNICO destinatario do MIDI, e repassa ao player.
    //
    // Antes os dois eram registrados e cada um recebia uma COPIA — o
    // MainComponent era so observador, e a nota ja tinha ido para o grafo
    // quando ele a via. Assim era impossivel uma tecla mapeada trocar o tom
    // do Pad sem tambem disparar voz nos canais (pedido de cliente, 01/08).
    //
    // O repasse e identico ao que o player fazia sozinho: o proprio
    // AudioProcessorPlayer::handleIncomingMidiMessage so enfileira no
    // getMidiMessageCollector(), que continua publico e sendo esvaziado
    // pelo player, que segue registrado como callback de AUDIO.
    restaurarCenas();

    liveScenePanel.onCarregarCena = [this] (int i)
    {
        const auto r = aplicarCena (i);
        liveScenePanel.atualizar (cenas, cenaAtiva);
        return r;
    };

    liveScenePanel.onGravarCena = [this] (int i)
    {
        const auto r = capturarCena (i);
        liveScenePanel.atualizar (cenas, cenaAtiva);
        return r;
    };

    liveScenePanel.onRenomearCena = [this] (int i, juce::String n) { renomearCena (i, std::move (n)); };
    liveScenePanel.onApagarCena   = [this] (int i) { apagarCena (i); };

    liveScenePanel.atualizar (cenas, cenaAtiva);
    addChildComponent (liveScenePanel);

    avisarSobreArquivosDeVersaoFutura();

    liveButton.setWantsKeyboardFocus (false);
    liveButton.setClickingTogglesState (true);
    liveButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    // Live e um modo de trabalho; o vermelho fica reservado para a pagina
    // principal Instrumentos. O azul-teal evita dois botoes parecendo ativos
    // iguais e diferencia navegacao de estado de emergencia/gravacao.
    liveButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3b6f8e));
    liveButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.88f));
    liveButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    liveButton.setTooltip ("Abrir ou fechar o modo Live de cenas");
    liveButton.onClick = [this]
    {
        const bool aberto = liveButton.getToggleState();

       #if ! CHURCHDAW_MOBILE_SHELL
        // No shell desktop da referencia, LIVE e uma pagina principal (com
        // MIXER / PERFORMANCE / SESSAO logo abaixo), nao o painel legado de
        // dez botoes que ocupava a faixa de programa. As cenas ja vivem na
        // sidebar fixa; manter os dois lugares repetia informacao e escondia a
        // subnavegacao nova. Em layouts menores conservamos o fluxo touch
        // antigo, onde essa faixa ainda e o seletor de cenas adequado.
        if (desktopReferenceLayout)
        {
            liveScenePanel.setVisible (false);

            if (mapButton.getToggleState() || midiLearnActive.load())
                finishMidiLearn (true);
            if (padEditorVisible)
                closePadEditor();

            showDesktopWorkspace (DesktopWorkspace::mixer);
            oled.setVisible (false);

            if ((! dockedNative || dockedEditor == nullptr) && slots[0].nodeID.uid != 0)
                openEditorForSlot (0);

            liveButton.setToggleState (true, juce::dontSendNotification);
            resized();
            repaint();
            return;
        }
       #endif

        if (aberto && desktopWorkspace != DesktopWorkspace::mixer)
            showDesktopWorkspace (DesktopWorkspace::mixer);

        if (aberto && padEditorVisible)
            closePadEditor();

        // Nunca com o aprendizado ligado: os dois disputam a mesma faixa.
        if (aberto && (mapButton.getToggleState() || midiLearnActive.load()))
            finishMidiLearn (true);

        liveScenePanel.setVisible (aberto);
        oled.setVisible (! aberto && ! mapButton.getToggleState());

        if (aberto)
            liveScenePanel.atualizar (cenas, cenaAtiva);

        // No celular a faixa CRESCE quando as cenas entram (ver resized), entao
        // abrir ou fechar o Live tem de refazer o layout — no computador isto e
        // so um recalculo barato.
        resized();
    };
    addAndMakeVisible (liveButton);

    for (auto* button : { &undoMixButton, &redoMixButton })
    {
        button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff191d24));
        button->setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtSecondary());
        addChildComponent (*button);
    }
    undoMixButton.setButtonText ("Desfazer");
    redoMixButton.setButtonText ("Refazer");
    undoMixButton.setTooltip ("Desfazer o ultimo ajuste de canal");
    redoMixButton.setTooltip ("Refazer ajuste de canal");
    undoMixButton.onClick = [this]
    { if (auto* editor = dynamic_cast<TimbresEditor*> (dockedEditor.get())) editor->undoParameterEdit(); };
    redoMixButton.onClick = [this]
    { if (auto* editor = dynamic_cast<TimbresEditor*> (dockedEditor.get())) editor->redoParameterEdit(); };

    deviceManager.addMidiInputDeviceCallback ({}, this);

    // alguns drivers ASIO abrem com o buffer padrao deles, ignorando o estado
    // salvo; reforca aqui a taxa e o buffer que o usuario escolheu
    {
        int wantedBuffer = appProperties.getUserSettings()->getIntValue ("bufferSize", 0);

       #if JUCE_ANDROID || JUCE_IOS
        // Piso fluido pro culto: >= 12 ms no menor tamanho OFERECIDO pelo
        // aparelho. Em TANK3 costuma cair em 512–768. Nao forca numero que o
        // driver nao lista. O tuner sobe em silencio se houver xrun.
        int preferredMobileBuffer = 0;

        if (auto* mobileDevice = deviceManager.getCurrentAudioDevice())
        {
           #if JUCE_ANDROID || JUCE_IOS
            // Reuse a leitura feita acima quando houver; do contrario, faca uma
            // unica leitura para a rota atual e guarde-a para o tuner.
            if (androidAvailableBufferSizes.isEmpty())
                androidAvailableBufferSizes =
                    mobileDevice->getAvailableBufferSizes();
            const auto& available = androidAvailableBufferSizes;
           #else
            const auto available = mobileDevice->getAvailableBufferSizes();
           #endif
            const double rate = mobileDevice->getCurrentSampleRate() > 0.0
                                    ? mobileDevice->getCurrentSampleRate()
                                    : 48000.0;

            preferredMobileBuffer =
                ChurchDAWMobileAudio::chooseClosestBufferForTargetMs (
                    available.getRawDataPointer(), available.size(), rate, 12.0);
        }

        if (preferredMobileBuffer > 0)
        {
           #if JUCE_ANDROID
            // Schema 3 separa a escolha do musico do degrau temporario usado
            // pelo tuner. Builds anteriores gravavam cada aumento por xrun em
            // "bufferSize" e o aparelho reabria para sempre com latencia alta.
            auto* settings = appProperties.getUserSettings();
            if (settings->getIntValue ("androidAdaptiveBufferSchema", 0) < 3)
            {
                // Um preset existente e uma escolha explicita. Sem preset, o
                // valor legado pode ser apenas o tuner antigo em 2048; cura-o
                // voltando ao perfil Natural (~12 ms).
                androidPreferredBufferSize =
                    settings->containsKey ("androidPerfPreset")
                        && wantedBuffer > 0
                            ? wantedBuffer : preferredMobileBuffer;
                settings->setValue ("androidAdaptiveBufferSchema", 3);
                settings->setValue ("androidUserBufferSize",
                                    androidPreferredBufferSize);
                settings->setValue ("bufferSize",
                                    androidPreferredBufferSize);
            }
            else
                androidPreferredBufferSize = settings->getIntValue (
                    "androidUserBufferSize",
                    wantedBuffer > 0 ? wantedBuffer : preferredMobileBuffer);

            if (androidPreferredBufferSize <= 0)
                androidPreferredBufferSize = preferredMobileBuffer;

            // Perfil Rapida (~8 ms) e intencional e nao recebe o piso Natural.
            wantedBuffer = androidPreferredBufferSize;
           #else
            // Builds anteriores escolheram o primeiro buffer ACIMA de 12 ms.
            // Na lista esparsa do iPad isso gravou 1024 / 21,33 ms. O iOS nao
            // oferece seletor manual de buffer, portanto esta migracao unica
            // pode corrigir o legado com seguranca; depois dela respeitamos o
            // estado persistido e apenas o driver/tuner decide novos ajustes.
            auto* settings = appProperties.getUserSettings();
            if (settings->getIntValue ("iosMobileBufferSchema", 0) < 1)
            {
                wantedBuffer = preferredMobileBuffer;
                settings->setValue ("bufferSize", wantedBuffer);
                settings->setValue ("iosMobileBufferSchema", 1);
                settings->saveIfNeeded();
            }
            else if (wantedBuffer <= 0)
                wantedBuffer = preferredMobileBuffer;
            androidPreferredBufferSize = wantedBuffer > 0 ? wantedBuffer : preferredMobileBuffer;
           #endif
        }
       #endif
        const double wantedRate = appProperties.getUserSettings()->getDoubleValue ("sampleRate", 0.0);

        if (wantedBuffer > 0)
        {
            auto setup = deviceManager.getAudioDeviceSetup();

            if (setup.bufferSize != wantedBuffer
                 || (wantedRate > 0 && ! juce::approximatelyEqual (setup.sampleRate, wantedRate)))
            {
                setup.bufferSize = wantedBuffer;

                if (wantedRate > 0)
                    setup.sampleRate = wantedRate;

                const auto err = deviceManager.setAudioDeviceSetup (setup, true);

               #if JUCE_ANDROID
                // O pedido de buffer estava sendo IGNORADO em silencio: o app
                // seguia em 256 amostras, e a 188 chamadas por segundo o custo
                // fixo do grafo se paga 188 vezes -- medido, isso levou a media
                // a 75% do prazo e produziu 54 blocos perdidos por segundo.
                // Registrar o que foi PEDIDO, o que voltou e o erro e a unica
                // forma de saber quem esta mandando aqui.
                juce::Logger::writeToLog ("AUDIO: buffer pedido " + juce::String (wantedBuffer)
                    + " -> obtido " + juce::String (deviceManager.getAudioDeviceSetup().bufferSize)
                    + (err.isEmpty() ? juce::String (" (sem erro)") : "  ERRO: " + err));
               #elif JUCE_IOS
                if (err.isNotEmpty())
                    juce::Logger::writeToLog (
                        "AUDIO_IOS saved_setup_failed requested_buffer="
                        + juce::String (wantedBuffer)
                        + " requested_rate=" + juce::String (wantedRate, 0)
                        + " error=\"" + err + "\"");
               #endif
            }
        }
    }

   #if JUCE_ANDROID || JUCE_IOS
    androidLastXRunCount = deviceManager.getCurrentAudioDevice() != nullptr
        ? juce::jmax (0, deviceManager.getCurrentAudioDevice()->getXRunCount())
        : 0;
    androidLastBufferTuneMs = juce::Time::getMillisecondCounter();
    androidXRunPressure = 0;
    androidStableBufferWindows = 0;
   #endif

   #if JUCE_ANDROID || JUCE_IOS
    // CASAR A TAXA COM A BIBLIOTECA (44.100 Hz).
    //
    // O aparelho abre em 48.000 e os samples sao 44.100. Com taxas diferentes a
    // razao de leitura NUNCA e 1.0 -- nem na nota raiz -- e o atalho rapido do
    // motor (leitura direta, sem interpolar) deixa de existir. Resultado: TODA
    // amostra de TODA voz passa pela interpolacao cubica de 4 pontos, nos dois
    // canais. Foi isso que levou a carga a 75% com 23 vozes e produziu 54 blocos
    // perdidos por segundo.
    //
    // Casando a taxa, a nota raiz volta a ser copia direta -- mais barato E mais
    // limpo, porque deixa de haver reamostragem no caminho.
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        const auto rates = dev->getAvailableSampleRates();

        // DESLIGADO -- a ideia era boa e o resultado foi pior.
        //
        // Casar a taxa com a biblioteca (44.100) de fato eliminaria a
        // reamostragem dentro do motor. So que a taxa NATIVA deste aparelho e
        // 48.000: pedir 44.100 nao remove a reamostragem, apenas a transfere
        // para o ANDROID -- e a dele bagunca o ritmo das chamadas. Medido:
        // "CHAMADA atrasada 15x por segundo, pior intervalo 1,90x", que antes
        // era 0x e 1,2x.
        //
        // O caminho certo para eliminar a reamostragem e converter a BIBLIOTECA
        // para 48 kHz, nao mexer no aparelho.
        if (false)
        {
            auto setup = deviceManager.getAudioDeviceSetup();
            setup.sampleRate = 44100.0;

            const auto err = deviceManager.setAudioDeviceSetup (setup, true);

            juce::Logger::writeToLog ("AUDIO: taxa 44100 pedida -> "
                + juce::String (deviceManager.getAudioDeviceSetup().sampleRate, 0)
                + (err.isEmpty() ? juce::String (" (sem erro)") : "  ERRO: " + err));
        }

        juce::Logger::writeToLog ("AUDIO: buffer ATUAL " + juce::String (dev->getCurrentBufferSizeSamples())
                                   + "  taxa " + juce::String (dev->getCurrentSampleRate(), 0));
    }
   #endif

    // ---- cabecalho ----
    addAndMakeVisible (liveCamButton);
   #if CHURCHDAW_MOBILE_SHELL
    liveCamButton.setButtonText ("ChurchCam");
   #endif
    liveCamButton.setComponentID ("navLiveCam");
    liveCamButton.setTooltip (juce::String::fromUTF8 ("Live Cam: grave a DAW, sua webcam e o áudio da sessão."));
    liveCamButton.onClick = [this]
    {
        if (auto* window = dynamic_cast<churchstudio::Window*> (liveCamWindow.getComponent())) { window->showStudio(); return; }
        // A nonmodal desktop window keeps every mixer control usable while
        // recording and stays outside the DAW component snapshot.
        liveCamWindow = new churchstudio::Window (pianoVisualState, studioAudioTap, &deviceManager, this, appProperties.getUserSettings()
           #if JUCE_IOS || JUCE_ANDROID
            , studioMicrophoneTap, [safe = juce::Component::SafePointer<MainComponent> (this)] (bool enabled, int channel)
            { return safe ? safe->setStudioMicrophoneEnabled (enabled, channel) : juce::Result::fail ("DAW encerrada."); }
           #endif
        );
        liveCamButton.setToggleState (true, juce::dontSendNotification);
        if (auto* liveWindow = dynamic_cast<churchstudio::Window*> (liveCamWindow.getComponent()))
        {
            auto safe = juce::Component::SafePointer<MainComponent> (this);
            liveWindow->onRecordingChanged = [safe] (bool active) { if (safe) { safe->liveCamButton.getProperties().set ("recording", active); safe->liveCamButton.repaint(); } };
            liveWindow->onClosed = [safe] { if (safe) { safe->liveCamButton.setToggleState (false, juce::dontSendNotification); safe->liveCamButton.getProperties().set ("recording", false); } };
        }
        liveCamWindow->toFront (true);
    };
    addAndMakeVisible (audioSettingsButton);
    audioSettingsButton.setButtonText (juce::String::fromUTF8 ("\xc3\x81udio / MIDI"));   // Áudio / MIDI
    audioSettingsButton.setActiveSupplier ([this] { return audioPanel != nullptr; });
    audioSettingsButton.onClick = [this] { showAudioSettings(); };

    addAndMakeVisible (librariesButton);
    librariesButton.setButtonText ("Bibliotecas");
    // Bibliotecas e uma acao, nao a pagina ativa. O antigo fundo vinho + texto
    // vermelho fazia ela competir com "Instrumentos" e as duas pareciam
    // selecionadas ao mesmo tempo no celular.
    librariesButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff24272e));
    librariesButton.setColour (juce::TextButton::textColourOffId,
                               juce::Colours::white.withAlpha (0.86f));
    librariesButton.onClick = [this] { showLibraries(); };

   #if CHURCHDAW_HOSTS_PLUGINS
    addAndMakeVisible (scanButton);
    scanButton.onClick = [this] { showPluginScanner(); };
   #endif

    addAndMakeVisible (padButton);
    // O Pad abre um painel flutuante; nao e uma segunda pagina de navegacao.
    // Use ambar no estado aberto para ele nao competir com o vermelho primario
    // de Instrumentos nem fazer as duas acoes parecerem selecionadas.
    padButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x89 Pad"));   // ◉ Pad
    padButton.setColour (juce::TextButton::buttonOnColourId,
                         NordLookAndFeel::amberSolo().darker (0.30f));
    padButton.setColour (juce::TextButton::textColourOnId,
                         juce::Colours::black.withAlpha (0.88f));
    // O Pad agora e uma pagina de produto completa. O antigo Pad Continuo
    // permanece disponivel como ferramenta rapida dentro do editor.
    padButton.setButtonText ("Pad");
    padButton.setClickingTogglesState (true);
    padButton.setColour (juce::TextButton::buttonOnColourId,
                         juce::Colour (0xffc4112e));
    padButton.setColour (juce::TextButton::textColourOnId,
                         juce::Colours::white);
    padButton.setTooltip (juce::String::fromUTF8 ("Abrir ou fechar o Pad Contínuo"));
    padButton.onClick = [this]
    {
        showPadWindow();
    };

    // card do Pad Continuo EMBUTIDO na DAW (oculto ate clicar em "Pad")
    padPanel = std::make_unique<PadPanel> (padProcessor);
    padPanel->onClose = [this]
    {
        if (padPanel != nullptr)
            padPanel->setVisible (false);

        if (! padEditorVisible)
            padButton.setToggleState (false, juce::dontSendNotification);

        // FECHAR O CARD DESISTE DO MAPEAMENTO.
        //
        // O alvo da nota e escolhido tocando um tom AQUI dentro. Se o musico
        // escolhe "Pad D", se arrepende e fecha o card, o alvo continuava
        // armado — invisivel, sem nada na tela dizendo isso — e a proxima
        // tecla em que ele encostasse virava comando e parava de tocar. Fechar
        // e o gesto universal de desistir; tem de desistir de verdade.
        {
            const juce::ScopedLock sl (mapLock);
            pendingNoteAction = {};
        }

        if (midiLearnActive.load())
            midiLearnPanel.showChooseTarget();
    };

    // O mapa de notas e restaurado la em cima, ANTES deste painel existir —
    // entao a primeira pintura das etiquetas tem de acontecer aqui, senao o
    // teclado abriria sem mostrar as teclas ja mapeadas do cliente.
    atualizarEtiquetasDoPad();
    atualizarBotoesDeCCDoPad();

    padPanel->onTrocarModoDoBotaoDeCC = [this] (int tom, bool comTrava)
    {
        if (! juce::isPositiveAndBelow (tom, 12))
            return;

        definirModoDoBotaoDoTom (
            tom, comTrava ? ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel
                          : ChurchDAWMidiMapping::ModoDeBotaoCC::alternar);

        juce::Logger::writeToLog (
            "MIDI MAP: botao de CC do tom " + juce::String (PadPanel::noteName (tom))
            + (comTrava ? " passou a seguir o nivel (pad com trava)"
                        : " voltou a alternar no aperto"));
    };

    padPanel->onMidiLearnTarget = [this] (int key)
    {
        if (! midiLearnActive.load())
            return;

        {
            const juce::ScopedLock sl (mapLock);
            pendingNoteAction = key >= 0 ? NoteAction { NoteActionKind::padKey, key }
                                         : NoteAction { NoteActionKind::padOff, -1 };
        }

        const auto target = key >= 0 ? "Pad " + juce::String (PadPanel::noteName (key))
                                     : juce::String ("Pad Parar");
        midiLearnPanel.showPlayNote (target);
        juce::Logger::writeToLog ("MIDI MAP: aguardando nota para " + target);
    };

    // APRENDER MIDI DIRETO DO MENU DO TOM — um gesto so.
    //
    // Sem isto o item "Aprender MIDI para este tom" nascia CINZA: o painel so o
    // habilita quando ha este callback ou quando o Mapear MIDI global ja esta
    // ligado. Ou seja, o item principal do modo "Editar" — que existe justamente
    // para o tablet, onde nao ha clique direito — vinha desligado.
    //
    // A ORDEM IMPORTA: ligar o modo limpa o pendingNoteAction (e o que o clique
    // no mapButton faz), entao o alvo tem de ser armado DEPOIS de ligar.
    padPanel->onPedirAprenderTom = [this] (int tom)
    {
        if (! juce::isPositiveAndBelow (tom, 12))
            return;

        if (! midiLearnActive.load())
            startMidiLearn();

        // Se o modo nao ligou por qualquer motivo, nao arma alvo nenhum: um
        // alvo armado sem o modo ligado seria uma tecla comendo notas sem nada
        // na tela dizendo isso.
        if (! midiLearnActive.load())
            return;

        {
            const juce::ScopedLock sl (mapLock);
            pendingNoteAction = NoteAction { NoteActionKind::padKey, tom };
        }

        const auto target = "Pad " + juce::String (PadPanel::noteName (tom));
        midiLearnPanel.showPlayNote (target);
        juce::Logger::writeToLog ("MIDI MAP: aguardando nota para " + target
                                   + " (pedido pelo menu do tom)");
    };

    addChildComponent (*padPanel);

    if (padSynthProcessor != nullptr)
    {
        padEditorPanel = std::make_unique<PadEditorPanel> (*padSynthProcessor);
        padEditorPanel->onClose = [this] { closePadEditor(); };
        padEditorPanel->onQuickPadContinuo = [this] { showPadWindow(); };
        padEditorPanel->onStateChanged = [this]
        {
            (void) saveChainState();
        };
        addChildComponent (*padEditorPanel);

        performanceSessionPanel =
            std::make_unique<PerformanceSessionPanel> (*padSynthProcessor);
        if (padProcessor != nullptr)
            for (auto* parameter : padProcessor->getParameters())
                if (ChurchDAWMidiMapping::stableParameterId (parameter)
                        == "padVolume")
                {
                    performanceSessionPanel->setPadVolumeMidiLearnTarget (
                        padProcessor, parameter);
                    break;
                }
        performanceSessionPanel->onLoadScene = [this] (int scene)
        {
            (void) aplicarCena (scene);
            refreshPerformanceSessionPanel();
        };
        performanceSessionPanel->onCaptureScene = [this] (int scene)
        {
            (void) capturarCena (scene);
            liveScenePanel.atualizar (cenas, cenaAtiva);
            refreshPerformanceSessionPanel();
        };
        performanceSessionPanel->onDeleteScene = [this] (int scene)
        {
            apagarCena (scene);
        };
        performanceSessionPanel->onSceneMetadataChanged =
            [this] (int scene, const ChurchDAWLive::Cena& metadata)
            {
                atualizarMetadadosDaCena (scene, metadata);
            };
        performanceSessionPanel->onSetlistChanged =
            [this] (const ChurchDAWLive::Setlist& newSetlist)
            {
                atualizarSetlist (newSetlist);
            };
        performanceSessionPanel->onExportSession = [this]
        {
            exportCultSession();
        };
        performanceSessionPanel->onImportSession = [this]
        {
            importCultSession();
        };
        performanceSessionPanel->onUndo = [this]
        {
            undoSceneHistory();
        };
        performanceSessionPanel->onRedo = [this]
        {
            redoSceneHistory();
        };
        performanceSessionPanel->onPanic = [this]
        {
            // The panel requests Pad Synth panic immediately before invoking
            // this callback; complete the global operation without duplicating
            // that processor request.
            performGlobalPanic (true);
        };
        performanceSessionPanel->onEmergencyPiano = [this]
        {
            activateEmergencyPiano();
        };
        performanceSessionPanel->onStageTest = [this]
        {
            runStageDiagnostic();
        };
        performanceSessionPanel->onAutoPatch = [this]
        {
            showAutoPatchBuilder();
        };
        performanceSessionPanel->onPlayback = [this]
        {
            showPlaybackFileChooser();
        };
        performanceSessionPanel->onQuickPad = [this] { showPadWindow(); };
        performanceSessionPanel->onSustainChanged = [this] (bool enabled)
        {
            if (auto* live = getChurchLive()) live->setGlobalSustain (enabled);
            agendarGravacaoDoSetup();
        };
        performanceSessionPanel->onTransposeChanged = [this] (int semitones)
        {
            if (auto* live = getChurchLive())
                if (auto* parameter = live->getTransposeGeralParam())
                    *parameter = semitones;
            if (padProcessor != nullptr) padProcessor->setTransposeSemis (semitones);
            agendarGravacaoDoSetup();
        };
        performanceSessionPanel->onPadVolumeChanged = [this] (float gain)
        {
            if (padProcessor != nullptr) padProcessor->setGain (gain);
            agendarGravacaoDoSetup();
        };
        performanceSessionPanel->onChannelSelected = [this] (int layer)
        {
            if (auto* live = getChurchLive()) live->setSelectedLayer (layer);
            showDesktopWorkspace (DesktopWorkspace::mixer);
        };
        addChildComponent (*performanceSessionPanel);
        refreshPerformanceSessionPanel();
    }

    addAndMakeVisible (saveButton);

    addAndMakeVisible (sabrinaButton);
    sabrinaButton.setClickingTogglesState (true);
    sabrinaButton.setToggleState (sabrinaEnabled,
                                  juce::dontSendNotification);
    sabrinaButton.setTooltip (
        "Saturacao suave no barramento master (Sabrina). Desligado = som igual "
        "ao original.");
    sabrinaButton.onClick = [this]
    {
        setSabrinaEnabled (! sabrinaEnabled);
        sabrinaButton.setToggleState (sabrinaEnabled,
                                      juce::dontSendNotification);
    };
    // vira um indicador de confianca: verde "Salvo" = autosave ligado, seu setup
    // esta guardado. Clicar forca salvar tudo e pisca "Salvo!"
    // Guardado para poder VOLTAR ao verde depois de um "FALHOU" vermelho.
   #if CHURCHDAW_MOBILE_SHELL
    corDoSalvoNormal = juce::Colour (0xff5fd08a).withAlpha (0.82f);
   #else
    corDoSalvoNormal = juce::Colour (0xff5fd08a);
   #endif

   #if CHURCHDAW_MOBILE_SHELL
    saveButton.setButtonText ("Salvo");
    saveButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    saveButton.setColour (juce::TextButton::textColourOffId, corDoSalvoNormal);
   #else
    saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
    saveButton.setColour (juce::TextButton::buttonColourId,
                          juce::Colours::transparentBlack);
    saveButton.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colours::transparentBlack);
    saveButton.setColour (juce::TextButton::textColourOffId, corDoSalvoNormal);
   #endif
    if (midiMapPersistenceError.load (std::memory_order_acquire))
    {
       #if CHURCHDAW_MOBILE_SHELL
        saveButton.setButtonText ("FALHOU");
       #else
        saveButton.setButtonText (
            juce::String::fromUTF8 ("\xe2\x9a\xa0 FALHOU"));
       #endif
        saveButton.setColour (juce::TextButton::textColourOffId,
                              NordLookAndFeel::nordRedBright());
    }
    saveButton.setTooltip (
        "Salvar a cena atual e, junto, todo o projeto");
    saveButton.onClick = [this]
    {
        // O cliente le este botao como SALVAR A CENA. Antes ele guardava apenas
        // o setup global e reescrevia o XML das cenas sem recapturar o som: a
        // luz ficava verde, mas reabrir a cena devolvia o snapshot antigo.
        //
        // Uma pagina nova ganha precedencia; fora dela, recaptura a cena que
        // esta no ar. Sem contexto de cena continua sendo o save geral normal.
        const int destinoDaCena = ChurchDAWLive::destinoDoSalvarSuperior (
            cenaEmPreparacao, cenaAtiva, cenas);
        ChurchDAWLive::Resultado resultadoDaCena = ChurchDAWLive::Resultado::ok;

        if (destinoDaCena >= 0)
        {
            resultadoDaCena = capturarCena (destinoDaCena);
            liveScenePanel.atualizar (cenas, cenaAtiva);
            refreshPerformanceSessionPanel();

            if (resultadoDaCena != ChurchDAWLive::Resultado::ok)
                liveScenePanel.avisar (ChurchDAWLive::avisoDaGravacao (
                    resultadoDaCena,
                    ChurchDAWLive::OrigemDaGravacao::botaoSalvar,
                    destinoDaCena));
        }

        const bool cenaAtualOk = resultadoDaCena == ChurchDAWLive::Resultado::ok;

        // Salva TUDO agora: cena, cadeia, mapeamento MIDI e audio.
        const bool setupOk = saveChainState();
        const bool midiMapsOk = saveMidiMapPair();
        const bool ccsOk = midiMapsOk;
        const bool notasOk = midiMapsOk;
        const bool cenasOk = salvarCenas();
        saveAudioStateNow();

        // Este saveIfNeeded fecha o que o saveAudioStateNow deixou pendente —
        // driver, taxa e buffer, ou seja, a configuracao que decide se SAI SOM
        // no domingo. O retorno estava sendo descartado, e o botao piscava
        // verde mesmo se ela nao tivesse ido para o disco.
        const bool audioOk = appProperties.getUserSettings()->saveIfNeeded();

        // O BOTAO E UM INDICADOR DE CONFIANCA: ele nao pode dizer "Salvo!" se
        // alguma coisa nao foi para o disco. Antes ele piscava verde de
        // qualquer jeito, e o musico ia para o culto acreditando num setup que
        // nao existia mais.
        //
        // MAS: pular um arquivo de versao futura e RECUSA DELIBERADA, ja
        // avisada em modal na abertura — nao e falha. Contando como falha, o
        // botao ficava vermelho para sempre nessa maquina e o musico perdia a
        // unica coisa que o indicador serve para dizer: a diferenca entre
        // "estou protegendo um arquivo de proposito" e "meu disco esta
        // morrendo e o setup NAO esta sendo gravado".
        const bool ccsFalhou   = ! ccsOk   && ! esquemaDeCCsDesconhecido;
        const bool notasFalhou = ! notasOk && ! esquemaDeNotasDesconhecido;
        bool arquivoDeCenasIlegivel = false;

        if (! cenasOk && esquemaDeCenasDesconhecido)
        {
            const auto arquivo = arquivoDeCenas();
            auto xml = juce::parseXML (arquivo);
            arquivoDeCenasIlegivel = arquivo.existsAsFile()
                                  && (xml == nullptr
                                       || ! xml->hasTagName ("LIVESCENES"));
        }

        // Versao futura e recusa deliberada; arquivo danificado e falha real.
        // Ambos usam a mesma trava contra overwrite, mas so o primeiro pode
        // manter o indicador verde sem mentir ao musico.
        const bool cenasFalhou = ! cenasOk
                              && (! esquemaDeCenasDesconhecido
                                   || arquivoDeCenasIlegivel);

        const bool tudoOk = cenaAtualOk && setupOk && audioOk
                              && ! ccsFalhou && ! notasFalhou && ! cenasFalhou;
        // An error remains visible until a later complete save succeeds. A
        // timer must never turn a failed/recovery-blocked project green.
        midiMapPersistenceError.store (! tudoOk, std::memory_order_release);

       #if CHURCHDAW_MOBILE_SHELL
        saveButton.setButtonText (tudoOk ? "Salvo!" : "FALHOU");
       #else
        saveButton.setButtonText (tudoOk ? juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo!")
                                         : juce::String::fromUTF8 ("\xe2\x9a\xa0 FALHOU"));
       #endif

        // A COR TEM DE CONCORDAR COM A PALAVRA. "FALHOU" escrito no verde de
        // "Salvo" e pior que nao avisar nada: o olho le a cor antes do texto.
        saveButton.setColour (juce::TextButton::textColourOffId,
                              tudoOk ? corDoSalvoNormal : NordLookAndFeel::nordRedBright());

        if (! tudoOk)
            juce::Logger::writeToLog (juce::String ("SALVAR: falhou em ")
                                      + (cenaAtualOk ? "" : "cena atual ")
                                      + (setupOk      ? "" : "setup ")
                                      + (ccsFalhou    ? "mapa de CCs " : "")
                                      + (notasFalhou  ? "mapa de notas " : "")
                                      + (cenasFalhou  ? "cenas " : "")
                                      + (audioOk      ? "" : "config de audio"));

        // Plugin que nao entregou o estado NAO derruba o botao (seria falha
        // permanente), mas o musico precisa saber QUAL — senao ele descobre no
        // domingo que os ajustes daquele efeito voltaram ao padrao.
        // UMA VEZ POR SESSAO, e nao por clique. Um VST antigo que falha sempre
        // abria um modal a cada toque no Salvar: tres cliques nervosos antes do
        // culto = tres janelas empilhadas sobre a mesa, cada uma exigindo
        // "Entendi", com o mixer bloqueado ate a ultima.
        if (! pluginsQueNaoSalvaram.isEmpty() && ! jaAvisouSobrePluginSemEstado)
        {
            jaAvisouSobrePluginSemEstado = true;

            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8 ("Ajustes de plugin n\xc3\xa3o guardados"),
                juce::String::fromUTF8 ("O setup foi salvo, mas ")
                    + pluginsQueNaoSalvaram.joinIntoString (", ")
                    + juce::String::fromUTF8 (
                        " n\xc3\xa3o entregou os ajustes internos.\n\nEsse plugin vai "
                        "reabrir no estado padr\xc3\xa3o. O resto do setup est\xc3\xa1 "
                        "guardado normalmente."),
                "Entendi");
        }

        // GERACAO, como no agendarGravacaoDoSetup.
        //
        // Sem ela, dois cliques proximos deixavam DOIS timers no ar e o do
        // primeiro (sucesso, 1,4 s) apagava o aviso do segundo (falha, 3 s):
        // o "FALHOU" durava 100 ms e virava "✓ Salvo" verde. O musico ia para
        // o culto vendo verde com o setup nao gravado — exatamente o oposto do
        // que este indicador promete.
        const auto minhaGeracao = ++geracaoDoAvisoDeSalvar;

        juce::Component::SafePointer<MainComponent> safe (this);
        juce::Timer::callAfterDelay (tudoOk ? 1400 : 3000, [safe, minhaGeracao]
        {
            if (auto* c = safe.getComponent())
            {
                if (c->geracaoDoAvisoDeSalvar != minhaGeracao)
                    return;   // um clique mais novo manda na tela
                if (c->midiMapPersistenceError.load (
                        std::memory_order_acquire))
                    return;

               #if CHURCHDAW_MOBILE_SHELL
                c->saveButton.setButtonText ("Salvo");
               #else
                c->saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
               #endif
                c->saveButton.setColour (juce::TextButton::textColourOffId, c->corDoSalvoNormal);
            }
        });
    };

    addChildComponent (instrumentsButton);
    // "pagina ativa": quando a tela cheia do Church Live esta aberta este botao
    // fica destacado em vermelho (voce esta aqui) e clicar volta pra cadeia
    instrumentsButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x89 Instrumentos"));   // ◉
    instrumentsButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc4112e));
    instrumentsButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    instrumentsButton.onClick = [this]
    {
        // Instrumentos e Mapear MIDI sao destinos exclusivos no shell desktop.
        // Fechar pelo proprio botao de Instrumentos tambem executa toda a
        if (mapButton.getToggleState() || midiLearnActive.load())
            finishMidiLearn (true);

        if (padEditorVisible)
            closePadEditor();

        liveScenePanel.setVisible (false);

        if (desktopWorkspace != DesktopWorkspace::mixer)
            showDesktopWorkspace (DesktopWorkspace::mixer);

        if ((! dockedNative || dockedEditor == nullptr) && slots[0].nodeID.uid != 0)
            openEditorForSlot (0);

        resized();
        repaint();
    };

    instrumentsButton.setComponentID ("navInstruments");
    liveButton.setComponentID ("navLive");
    mapButton.setComponentID ("navMidi");
    saveButton.setComponentID ("navSave");
    padButton.setComponentID ("navPad");
    scanButton.setComponentID ("navPlugins");
    librariesButton.setComponentID ("navLibraries");
    audioSettingsButton.setComponentID ("headerNav");
   #if CHURCHDAW_MOBILE_SHELL
    for (auto* button : std::initializer_list<juce::Button*> { &instrumentsButton, &liveButton, &mapButton, &saveButton, &padButton, &scanButton, &librariesButton, &liveCamButton })
        button->getProperties().set ("churchTouchNav", true);
   #endif

    // Clique de mouse nao leva o foco do teclado para a navegacao. O anel de
    // foco vermelho fazia a aba clicada (ex.: Live Cam) parecer uma SEGUNDA aba
    // ativa ao lado da pagina atual, e um Espaco/Enter depois do clique
    // reapertava o botao -- reabrindo a janela no meio do culto. Tab continua
    // focando os botoes: a navegacao por teclado segue acessivel.
    for (auto* button : std::initializer_list<juce::Button*> { &instrumentsButton, &liveButton, &mapButton, &saveButton, &padButton, &scanButton, &librariesButton, &audioSettingsButton, &liveCamButton })
        button->setMouseClickGrabsKeyboardFocus (false);

    // chips premium do header (controlador MIDI + interface); a entrada de
    // guitarra/mic saiu do header e agora mora no dialogo Audio/MIDI
    addAndMakeVisible (deviceBar);
    addChildComponent (performanceDock);
    deviceBar.onOpenSettings = [this]
    {
        if (audioSettingsButton.onClick) audioSettingsButton.onClick();
    };
    inputToggle.setButtonText (juce::String::fromUTF8 ("Entrada de \xc3\xa1udio (guitarra/mic)"));   // áudio
    inputToggle.onClick = [this]
    {
        const bool requested = inputToggle.getToggleState();
        audioInputAuthorised.store (requested, std::memory_order_release);

        if (! setAudioInputChannelsEnabled (requested) && requested)
        {
            audioInputAuthorised.store (false, std::memory_order_release);
            inputToggle.setToggleState (false, juce::dontSendNotification);
        }

        rebuildGraphConnections();
        saveChainState();
    };

    addAndMakeVisible (mapButton);
    mapButton.setClickingTogglesState (false);
    // No desktop este botao abre uma pagina completa e segue a mesma linguagem
    // de navegacao ativa do restante do produto. O estado de aprendizado vive
    // dentro da pagina e continua distinto pelo pulso/status vermelho.
    mapButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2c171d));
    mapButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xfff5f5f6));
    mapButton.onClick = [this]
    {
        if (midiLearnActive.load() || mapButton.getToggleState())
            finishMidiLearn (true);
        else
            startMidiLearn();
    };

    addChildComponent (clearMapButton);
    clearMapButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff312b22));
    clearMapButton.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::oledAmber());
    clearMapButton.onClick = [this]
    {
        // Dois toques: nao existe "desfazer" depois que o mapa e salvo. Um unico
        // toque acidental no palco nao pode apagar o setup inteiro.
        if (! clearMapConfirmationArmed)
        {
            clearMapConfirmationArmed = true;
            clearMapButton.setButtonText ("Confirmar");
            midiLearnPanel.showClearConfirmation();

            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay (4000, [safe]
            {
                if (auto* c = safe.getComponent())
                    if (c->clearMapConfirmationArmed)
                    {
                        c->clearMapConfirmationArmed = false;
                        c->clearMapButton.setButtonText ("Limpar mapas");
                        c->midiLearnPanel.showChooseTarget();
                    }
            });
            return;
        }

        const auto ccFile = arquivoDoMapaDeCCs();
        const auto noteFile = arquivoDoMapaDeNotas();
        ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
            ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);

        decltype (ccMappings) previousCC;
        decltype (ccActionMappings) previousActions;
        decltype (noteMappings) previousNotes;
        NoteAction previousPendingAction;
        juce::String previousTouchedName;
        juce::String previousTouchedId;
        juce::String previousTouchedPluginId;
        int previousTouchedSlot = -2;
        int previousTouchedParam = -1;
        int previousTouchedInsert = -1;
        bool previousTouchedIdentityUnique = false;
        const bool previousUnknownCC = esquemaDeCCsDesconhecido;
        const bool previousUnknownNotes = esquemaDeNotasDesconhecido;
        const bool previousBlockedCC = bloquearSalvamentoCCVazio;
        const bool previousBlockedNotes = bloquearSalvamentoNotasVazio;
        const bool previousProfilesSuspended = automaticControllerProfilesSuspended;

        bool checkpointReady = false;
        bool apagouNoDisco = false;
        bool rollbackOk = true;
        bool memoryMutated = false;
        bool ccWritten = false;
        bool noteWritten = false;
        juce::uint64 ccEpoch = 0;
        juce::uint64 noteEpoch = 0;

        beginMidiMapRoutingTransaction();

        checkpointReady = diskRollback.capture ("before-clear");
        if (! checkpointReady)
            rollbackOk = false;
        if (checkpointReady && diskRollback.targetsUnchangedSinceCapture())
        {
            {
                const juce::ScopedLock sl (mapLock);
                previousCC = ccMappings;
                previousActions = ccActionMappings;
                previousNotes = noteMappings;
                previousPendingAction = pendingNoteAction;
                previousTouchedName = lastTouchedParamName;
                previousTouchedId = lastTouchedParamId;
                previousTouchedPluginId = lastTouchedPluginId;
                previousTouchedSlot =
                    lastTouchedSlot.load (std::memory_order_relaxed);
                previousTouchedParam =
                    lastTouchedParam.load (std::memory_order_relaxed);
                previousTouchedInsert =
                    lastTouchedInsert.load (std::memory_order_relaxed);
                previousTouchedIdentityUnique =
                    lastTouchedParameterIdentityUnique.load (
                        std::memory_order_relaxed);

                ccMappings.clear();
                noteMappings.clear();
                ccActionMappings.clear();
                pendingNoteAction = {};
                lastTouchedParamName.clear();
                lastTouchedParamId.clear();
                lastTouchedPluginId.clear();
                lastTouchedParameterIdentityUnique.store (
                    false, std::memory_order_relaxed);
                ccDirtyEpoch.markDirty();
                noteDirtyEpoch.markDirty();
                memoryMutated = true;
            }

            ccEpoch = ccDirtyEpoch.snapshot();
            noteEpoch = noteDirtyEpoch.snapshot();
            ccWritten = saveMidiMap (true);
            noteWritten = ccWritten && saveNoteMap (true);
            apagouNoDisco = ccWritten && noteWritten
                         && diskRollback.commit();
        }

        if (checkpointReady && ! memoryMutated)
        {
            // Another writer changed a target after the checkpoint. We did not
            // touch memory or disk; close our journal without overwriting it.
            rollbackOk = diskRollback.closeWithoutMutation();
        }
        else if (checkpointReady && ! apagouNoDisco)
        {
            rollbackOk =
                ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                    diskRollback, ccFile, noteFile);
            const juce::ScopedLock sl (mapLock);
            ccMappings = std::move (previousCC);
            ccActionMappings = std::move (previousActions);
            noteMappings = std::move (previousNotes);
            pendingNoteAction = previousPendingAction;
            lastTouchedParamName = previousTouchedName;
            lastTouchedParamId = previousTouchedId;
            lastTouchedPluginId = previousTouchedPluginId;
            lastTouchedSlot.store (
                previousTouchedSlot, std::memory_order_relaxed);
            lastTouchedParam.store (
                previousTouchedParam, std::memory_order_relaxed);
            lastTouchedInsert.store (
                previousTouchedInsert, std::memory_order_relaxed);
            lastTouchedParameterIdentityUnique.store (
                previousTouchedIdentityUnique, std::memory_order_relaxed);
            esquemaDeCCsDesconhecido = previousUnknownCC;
            esquemaDeNotasDesconhecido = previousUnknownNotes;
            bloquearSalvamentoCCVazio = previousBlockedCC;
            bloquearSalvamentoNotasVazio = previousBlockedNotes;
            automaticControllerProfilesSuspended = previousProfilesSuspended;
            // A tentativa do usuario foi desfeita por inteiro. Nao agende um
            // segundo save: se diskStillOwned ficou falso, esse retry capturaria
            // o arquivo da outra instancia como baseline e o sobrescreveria.
            bumpMidiMappingGeneration();
        }
        else if (apagouNoDisco)
        {
            automaticControllerProfilesSuspended = false;
        }

        if (apagouNoDisco)
        {
            finishMidiMapDirtyEpochsAfterPairCommit (
                ccEpoch, ccWritten, noteEpoch, noteWritten);
            bumpMidiMappingGeneration();
            cancelPendingUpdate();
            lastMappedCC.store (-1);
            lastMappedNote.store (-1);
            lastMappedCCAcao.store (-1);
            lastCCAcaoRecusada.store (-1);
            lastCCAcaoDesmapeada.store (-1);
            lastNoteActionRejected.store (-1);
            lastParameterMappingRejectedCC.store (-1);
            {
                const juce::ScopedLock sl (mapLock);
                lastTouchedSlot.store (-2, std::memory_order_relaxed);
                lastTouchedParam.store (-1, std::memory_order_relaxed);
                lastTouchedInsert.store (-1, std::memory_order_relaxed);
                lastTouchedParamName.clear();
                lastTouchedParamId.clear();
                lastTouchedPluginId.clear();
                lastTouchedParameterIdentityUnique.store (
                    false, std::memory_order_relaxed);
            }
        }
        finishMidiMapRoutingTransaction();

        if (! checkpointReady
             && diskRollback.hadUninspectableTargetConflict())
            bloquearQuartetoMidiDesconhecido (
                "checkpoint de Limpar mapas");
        if (! rollbackOk || diskRollback.preservedExternalChanges())
            midiMapPairRecoveryFailed = true;
        reconstruirMascaraDeNotas();
        reconstruirMascaraDeCCsDeAcao();

        atualizarEtiquetasDoPad();
        atualizarBotoesDeCCDoPad();
        clearMapConfirmationArmed = false;
        clearMapButton.setButtonText ("Limpar mapas");
        ccPickup.clear();

        const int currentMappingCount = getMidiMappingCount();
        deviceBar.setMappingCount (currentMappingCount);
        midiLearnPanel.setMappingCount (currentMappingCount);

        // Se o disco recusou, o mapa antigo VOLTA INTEIRO na proxima abertura —
        // inclusive a tecla mapeada por engano que motivou a limpeza. O musico
        // precisa saber disso agora, e nao no domingo.
        if (checkpointReady && apagouNoDisco)
        {
            midiLearnPanel.showChooseTarget();
            if (midiMappingPanel.isVisible())
                midiMappingPanel.setStatus (
                    "Todos os mapeamentos MIDI foram apagados e salvos",
                    MidiMappingPanel::Status::success);
            juce::Logger::writeToLog ("MIDI MAP: todos os mapeamentos foram apagados");
        }
        else
        {
            midiLearnPanel.showAviso (
                juce::String::fromUTF8 (
                    "N\xc3\xa3o foi poss\xc3\xadvel apagar com seguran\xc3\xa7" "a"),
                rollbackOk
                    ? juce::String::fromUTF8 ("Os mapas anteriores foram restaurados")
                    : juce::String::fromUTF8 ("Falha grave no rollback; preserve os backups"));
            if (midiMappingPanel.isVisible())
                midiMappingPanel.setStatus (
                    rollbackOk
                        ? "Falha ao apagar; os mapeamentos anteriores foram restaurados"
                        : "Falha ao apagar e restaurar arquivos; verifique os backups",
                    rollbackOk ? MidiMappingPanel::Status::warning
                               : MidiMappingPanel::Status::error);
            juce::Logger::writeToLog (
                rollbackOk
                    ? "MIDI MAP: limpeza falhou; estado anterior restaurado"
                    : "MIDI MAP: ERRO GRAVE no rollback da limpeza");
        }
    };

    addChildComponent (midiLearnPanel);

    // Pagina desktop completa de gerenciamento MIDI. O motor de captura,
    // seguranca, pickup e persistencia continua neste MainComponent; o painel
    // apenas apresenta alvos reais e devolve a intencao do usuario.
    midiMappingPanel.onLearningChanged = [this] (bool enabled)
    {
        setMidiLearningFromPage (enabled);
    };
    midiMappingPanel.onDetectDevices = [this]
    {
        refreshMidiInputs();
        cachedMidiInputsAtMs = 0;
        const auto& devices = midiInputsCached (
            juce::Time::getMillisecondCounter());
        midiMappingPanel.setStatus (
            devices.isEmpty() ? juce::String ("Nenhum controlador MIDI encontrado")
                              : juce::String (devices.size()) + " dispositivo(s) MIDI encontrado(s)",
            devices.isEmpty() ? MidiMappingPanel::Status::warning
                              : MidiMappingPanel::Status::success);
    };
    midiMappingPanel.onQuickMapping = [this]
    {
        beginQuickMidiMapping();
    };
    midiMappingPanel.onImport = [this] { importMidiMappings(); };
    midiMappingPanel.onExport = [this] { exportMidiMappings(); };
    midiMappingPanel.onClearRequested = [this] (bool confirmed)
    {
        if (! confirmed)
        {
            if (! clearMapConfirmationArmed)
                clearMapButton.triggerClick();
            return;
        }

        if (! clearMapConfirmationArmed)
            clearMapButton.triggerClick();
        if (clearMapConfirmationArmed)
            clearMapButton.triggerClick();

        refreshMidiMappingPanel();
    };
    midiMappingPanel.onCommandSelected = [this] (const MidiMappingPanel::Command& command)
    {
        if (midiLearnActive.load())
            armMidiMappingCommand (command);
    };
    midiMappingPanel.onDeleteCommand = [this] (const MidiMappingPanel::Command& command)
    {
        deleteMidiMappingCommand (command);
    };
    midiMappingPanel.onTestCommand = [this] (const MidiMappingPanel::Command& command)
    {
        // armMidiMappingCommand decide o status: sucesso mostra a instrucao de
        // aprendizado e falha (alvo stale/indisponivel) mostra o motivo real.
        // Sobrescrever aqui com "Alvo pronto" mentia quando o arm falhava.
        armMidiMappingCommand (command);
    };
    midiMappingPanel.onSaveCommand = [this] (const MidiMappingPanel::Command& command)
    {
        saveMidiMappingCommand (command);
    };
    addChildComponent (midiMappingPanel);

    addChildComponent (midiLearnConsoleOverlay);
    midiLearnConsoleOverlay.shouldPassThrough = [this] (juce::Point<int> screenPos)
    {
        if (mapButton.isShowing() && mapButton.getScreenBounds().contains (screenPos))
            return true;
        if (clearMapButton.isShowing() && clearMapButton.getScreenBounds().contains (screenPos))
            return true;
        if (midiLearnPanel.isShowing() && midiLearnPanel.getScreenBounds().contains (screenPos))
            return true;
        return false;
    };
    midiLearnConsoleOverlay.onControlClicked = [this] (juce::Component* hit,
                                                       juce::Point<int> localPos,
                                                       juce::Point<int> screenPos)
    {
        juce::ignoreUnused (localPos);
        if (! midiLearnActive.load())
            return;

        if (hit == &mapButton || mapButton.isParentOf (hit)
            || (mapButton.isShowing() && mapButton.getScreenBounds().contains (screenPos)))
        {
            finishMidiLearn (true);
            return;
        }

        if (hit == &clearMapButton || clearMapButton.isParentOf (hit)
            || (clearMapButton.isShowing() && clearMapButton.getScreenBounds().contains (screenPos)))
        {
            clearMapButton.triggerClick();
            return;
        }

        juce::AudioProcessor* processor = nullptr;
        int parameterIndex = -1;
        int actionKind = 0;
        int actionArg = -1;
        juce::String actionDisplayName;
        juce::Component* targetControl = nullptr;

        for (auto* c = hit; c != nullptr && c != this && c != &midiLearnConsoleOverlay; c = c->getParentComponent())
        {
            if (ChurchDAWMidiLearnTarget::read (*c, processor, parameterIndex))
            {
                targetControl = c;
                break;
            }
            if (ChurchDAWMidiLearnTarget::readAction (*c, actionKind, actionArg, actionDisplayName))
            {
                targetControl = c;
                break;
            }
        }

        if (targetControl != nullptr && processor != nullptr && parameterIndex >= 0)
        {
            juce::String customName = ChurchDAWMidiLearnTarget::readDisplayName (*targetControl);
            armMidiLearnParameter (processor, parameterIndex, targetControl, customName);
            return;
        }

        if (targetControl != nullptr && actionKind > 0)
        {
            armMidiLearnAction (actionKind, actionArg, actionDisplayName, targetControl);
            return;
        }

        if (padPanel != nullptr && padPanel->isShowing() && padPanel->getScreenBounds().contains (screenPos))
        {
            const auto padPt = padPanel->getLocalPoint (nullptr, screenPos).toFloat();
            const int tom = padPanel->getTomAt (padPt);
            if (tom >= 0)
            {
                const auto name = "Pad " + juce::String (PadPanel::noteName (tom));
                const auto cellBounds = padPanel->getCellBounds (tom).getSmallestIntegerContainer();
                const auto boundsInOverlay = midiLearnConsoleOverlay.getLocalArea (padPanel.get(), cellBounds);
                armMidiLearnAction ((int) NoteActionKind::padKey, tom, name, nullptr, boundsInOverlay);
                return;
            }
        }

       #if ! CHURCHDAW_MOBILE_SHELL
        if (desktopReferenceLayout)
        {
            const auto mainPt = getLocalPoint (nullptr, screenPos);
            for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            {
                if (desktopSceneBounds[i].contains (mainPt))
                {
                    const int sceneIdx = desktopSceneIndexAtRow[i];
                    if (juce::isPositiveAndBelow (sceneIdx, ChurchDAWLive::numeroDeCenas))
                    {
                        const auto name = "Cena " + juce::String (sceneIdx + 1);
                        const auto boundsInOverlay = midiLearnConsoleOverlay.getLocalArea (this, desktopSceneBounds[i]);
                        armMidiLearnAction ((int) NoteActionKind::cena, sceneIdx, name, nullptr, boundsInOverlay);
                        return;
                    }
                }
            }

            if (desktopTapBounds.contains (mainPt))
            {
                const auto boundsInOverlay = midiLearnConsoleOverlay.getLocalArea (this, desktopTapBounds);
                armMidiLearnAction ((int) NoteActionKind::tapTempo, -1, "Tap Tempo", nullptr, boundsInOverlay);
                return;
            }
        }
       #endif

        // A blank click cancels the actual pending target as well as its UI.
        // Otherwise the next MIDI message silently maps the previous control.
        resetMidiLearnTargetUnderCallbackBarrier (true);
        midiLearnConsoleOverlay.clearTarget();
        midiLearnPanel.showChooseTarget();
    };

    setWantsKeyboardFocus (true);

   #if JUCE_ANDROID
    // O mixer e acelerado por OpenGL no Android. A arte ARGB do icone instalado
    // funciona na splash, mas alguns drivers a transformam em uma textura preta
    // dentro desse contexto. A marca RGB redonda e visualmente equivalente e ja
    // foi validada tanto no emulador quanto no aparelho fisico.
    brandIcon = juce::ImageFileFormat::loadFrom (BinaryData::logo_round_png,
                                                 (size_t) BinaryData::logo_round_pngSize);
   #elif JUCE_IOS
    brandIcon = juce::ImageCache::getFromMemory (BinaryData::logo_round_png,
                                                 BinaryData::logo_round_pngSize);
   #else
    // A marca do desktop usa a mesma arte oficial enviada pelo usuário,
    // incluindo o círculo, a igreja, as teclas e o wordmark ChurchDAW.
    brandIcon = juce::ImageCache::getFromMemory (BinaryData::logo_round_png,
                                                 BinaryData::logo_round_pngSize);
   #endif

   #if ! CHURCHDAW_MOBILE_SHELL
    // A arte oficial veio em JPG/PNG com fundo preto incorporado. Para a
    // sidebar atmosferica esse retangulo denunciava o arquivo. Criamos uma
    // copia com alpha derivado da luminosidade e preservamos vermelho/branco;
    // a igreja passa a nascer diretamente da textura do shell.
    if (brandIcon.isValid())
    {
        desktopBrandMark = juce::Image (juce::Image::ARGB,
                                        brandIcon.getWidth(),
                                        brandIcon.getHeight(), true);
        for (int y = 0; y < desktopBrandMark.getHeight(); ++y)
            for (int x = 0; x < desktopBrandMark.getWidth(); ++x)
            {
                const auto c = brandIcon.getPixelAt (x, y);
                const float red = c.getFloatRed();
                const float light = juce::jmax (red,
                    juce::jmax (c.getFloatGreen(), c.getFloatBlue()));
                const float lifted = juce::jlimit (0.0f, 1.0f,
                                                   (light - 0.080f) / 0.32f);
                float alpha = lifted;
                if (red > c.getFloatGreen() * 1.20f
                     && red > c.getFloatBlue() * 1.16f)
                    alpha = juce::jmax (alpha,
                                        juce::jlimit (0.0f, 1.0f, red * 1.18f));

                // Feather real nas bordas visiveis da arte. A transparencia
                // por luminosidade remove o preto, mas uma borda abrupta ainda
                // denunciava o bitmap. O smoothstep preserva o miolo nitido e
                // faz igreja/waveform nascerem diretamente do fundo.
                const float edgeX = juce::jmin (
                    (float) (x + 1) / ((float) desktopBrandMark.getWidth() * 0.18f),
                    (float) (desktopBrandMark.getWidth() - x)
                        / ((float) desktopBrandMark.getWidth() * 0.18f));
                const float visibleBottom = (float) desktopBrandMark.getHeight() * 0.60f;
                const float edgeTop = (float) (y + 1)
                    / ((float) desktopBrandMark.getHeight() * 0.090f);
                const float edgeBottom = (visibleBottom - (float) y)
                    / ((float) desktopBrandMark.getHeight() * 0.090f);
                const float edge = juce::jlimit (0.0f, 1.0f,
                    juce::jmin (edgeX, juce::jmin (edgeTop, edgeBottom)));
                const float feather = edge * edge * (3.0f - 2.0f * edge);
                desktopBrandMark.setPixelAt (x, y,
                    c.withAlpha (alpha * feather * c.getFloatAlpha()));
            }
    }
   #endif

    statusInfo.setFont (juce::Font (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 9.5f,
        juce::Font::bold)));
    statusInfo.setJustificationType (juce::Justification::centred);   // centrado no chip de vidro
    statusInfo.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.86f));
    addAndMakeVisible (statusInfo);
    addAndMakeVisible (oled);
    oled.fetchChord = [this]() -> juce::String
    {
        if (auto* live = getChurchLive())
        {
            bool held[128] = {};
            live->getHeldNotes (held);
            return ChordDetect::nameFor (held);
        }

        return {};
    };
    qaOledChannel = juce::SystemStats::getEnvironmentVariable (
                        "CHURCHDAW_QA_OLED_CHANNEL", {}).getIntValue();

    oled.fetchLevel = [this] (int l) -> float
    {
        // Prova de integridade opt-in: permite confirmar visualmente que os
        // indices 8 e 9 nao compartilham o mesmo slot de medidor. Fora do QA,
        // o caminho e exatamente o nivel real do Church Live.
        if (qaOledChannel == 9)
        {
            if (l == 7) return 0.12f;
            if (l == 8) return 0.88f;
        }

        if (auto* live = getChurchLive())
            return live->getLayerLevelShown (l);

        return 0.0f;
    };
   #if CHURCHDAW_UI_PREVIEW
    deviceBar.setControllerName ("Nenhum", false);
    deviceBar.setInterfaceName (juce::String::fromUTF8 ("Prévia sem áudio"), false);
    statusInfo.setText (juce::String::fromUTF8 ("Áudio não iniciado"), juce::dontSendNotification);
   #else
    startTimer (1000);
   #endif

   #if CHURCHDAW_REMOTE_CONTROL_ENABLED
    // Nunca derive a credencial de um identificador previsivel do aparelho e
    // nunca a publique em log ou endpoint. Enquanto nao existir pareamento na
    // UI, uma ativacao interna deve fornecer manualmente um segredo forte.
    remoteToken = juce::SystemStats::getEnvironmentVariable (
                      "CHURCHDAW_REMOTE_TOKEN", {}).trim();
    if (remoteToken.length() >= 16)
    {
        remoteServer = std::make_unique<ChurchDAWRemoteServer>();
        remoteServer->setHandler (
            [safe = juce::Component::SafePointer<MainComponent> (this)]
            (const juce::String& path)
            {
                if (auto* self = safe.getComponent())
                    return self->handleRemoteRequest (path);
                return juce::String ("{\"error\":\"offline\"}");
            });
        if (remoteServer->start())
            juce::Logger::writeToLog (
                "REMOTE: servidor local ativo com pareamento manual");
    }
    else
    {
        remoteToken.clear();
        juce::Logger::writeToLog (
            "REMOTE: desativado; token manual forte nao configurado");
    }
   #endif

    // ---- slots da cadeia ----
    const char* slotNames[numSlots] = { "INSTRUMENTO", "EFEITO 1", "EFEITO 2", "EFEITO 3" };

    for (int i = 0; i < numSlots; ++i)
    {
        auto& s = slots[i];

        s.label.setText (slotNames[i], juce::dontSendNotification);
        s.label.setJustificationType (juce::Justification::centred);
        s.label.setFont (juce::Font (juce::FontOptions (13.0f)));
        s.label.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (s.label);

        s.box.setTextWhenNothingSelected ("(vazio)");
        // combo focado troca de item com as setas do teclado do PC — num slot
        // isso DESCARREGA/CARREGA plugin sem querer; mesmo perigo do slider
        s.box.setWantsKeyboardFocus (false);
        s.box.onChange = [this, i] { slotSelectionChanged (i); };
        addAndMakeVisible (s.box);

        s.openButton.onClick = [this, i] { openEditorForSlot (i); };
        s.openButton.setEnabled (false);
        addAndMakeVisible (s.openButton);
    }

    // ---- secao master ----
    masterLabel.setText ("MASTER", juce::dontSendNotification);
    masterLabel.setJustificationType (juce::Justification::centred);
    masterLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    masterLabel.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
    addAndMakeVisible (masterLabel);

    masterSlider.setSliderStyle (juce::Slider::LinearVertical);
    // NUNCA foco de teclado em slider: o Slider do JUCE muda o valor com as
    // setas do teclado do PC quando focado — bug gravissimo ao vivo (seta
    // mudava fader/transpose sem o musico perceber)
    masterSlider.setWantsKeyboardFocus (false);
    masterSlider.setRange (MasterProcessor::minusInfinityDb, MasterProcessor::maxGainDb, 0.1);
    masterSlider.setValue (0.0, juce::dontSendNotification);
    // -6 acompanha o masterDbSlider do Church Live: unidade a ~66% do
    // curso nas duas reguas do master (consistencia apontada pelo dono).
    masterSlider.setSkewFactorFromMidPoint (-6.0);
    masterSlider.setDoubleClickReturnValue (true, 0.0);
    masterSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 17);
    masterSlider.setTextValueSuffix (" dB");
    masterSlider.onValueChange = [this]
    {
        masterProcessor->setMasterDb ((float) masterSlider.getValue());

        if (midiLearnActive.load() && ! applyingMappedCC)
        {
            const juce::ScopedLock sl (mapLock);
            lastTouchedSlot.store (-1, std::memory_order_relaxed);
            lastTouchedParam.store (-1, std::memory_order_relaxed);
            lastTouchedInsert.store (-1, std::memory_order_relaxed);
            lastTouchedParamName = "Master";
            lastTouchedParamId = "master";
            lastTouchedPluginId =
                ChurchDAWMidiMapping::masterProcessorIdentity();
            lastTouchedParameterIdentityUnique.store (
                true, std::memory_order_relaxed);
            pendingNoteAction = {};
            triggerAsyncUpdate();
        }
    };
    masterSlider.onDragEnd = [this] { agendarGravacaoDoSetup(); };
    addAndMakeVisible (masterSlider);
    if (masterProcessor != nullptr
         && ! masterProcessor->getParameters().isEmpty())
        ChurchDAWMidiLearnTarget::tag (
            masterSlider, *masterProcessor,
            *masterProcessor->getParameters().getFirst());

    // MESMO CONTRATO DA REGUA DO CHURCH LIVE (ver TimbresProcessor, criacao do
    // masterMeter): sao dois medidores do MESMO barramento em duas telas, e ate
    // aqui cada um contava uma historia. Este lia o nivel da caixa do outro e
    // tratava o clip como luz momentanea; agora os dois leem a MESMA trava nao
    // destrutiva, mostram o mesmo ambar de limiter e cada um tem sua propria
    // caixa de nivel (levelReaderChain / levelReaderMaster).
    meter = std::make_unique<LevelMeter> (2,
        [mp = masterProcessor] (int ch)
        {
            return mp->readAndResetLevel (ch, MasterProcessor::levelReaderChain);
        },
        // ALERTA (vermelho, travado): sobrecarga de gain-staging na entrada do
        // limiter. NAO e "passou de 0 dBFS" -- isso e o trabalho normal do
        // brick-wall. Le sem zerar, entao nao disputa o evento com a outra regua.
        [mp = masterProcessor] { return mp->getClipLatched(); });
    // ...e o clique apaga a trava na FONTE, senao ela reacenderia no proximo tick.
    meter->setClipClearFn ([mp = masterProcessor] { mp->clearClipLatch(); });
    // INFORMATIVO (ambar, momentaneo): o limiter esta segurando AGORA.
    meter->setLimiterActivityFn (
        [mp = masterProcessor] { return mp->getLimiterGainReductionDb() < -0.5f; });
    meter->setClipRole (LevelMeter::ClipRole::channelClip);
    addAndMakeVisible (*meter);

    // ---- painel de plugin embutido ----
    dockTitle.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (dockTitle);
    addChildComponent (dockViewport);
    dockViewport.setViewedComponent (&dockHolder, false);
   #if ! CHURCHDAW_MOBILE_SHELL
    dockViewport.setScrollBarThickness (8);
   #endif
    addChildComponent (dockFloatButton);
    addChildComponent (dockCloseButton);

    dockFloatButton.onClick = [this]
    {
        const auto id = dockedNodeID;
        undockEditor();
        openFloatingWindowForNode (id);
    };

    dockCloseButton.onClick = [this] { undockEditor(); };

    refreshSlotChoices();
    if (! safeMode)
        restoreChainState();
    else
        juce::Logger::writeToLog (
            "SAFE_MODE: cadeia salva ignorada; usando instrumentos internos neutros");

    if (slots[0].nodeID.uid == 0)
    {
        loadInternalTimbres (0, nullptr, 0);
        slots[0].box.setSelectedId (timbresComboId, juce::dontSendNotification);
        refreshSlotChoices();
        if (slots[0].nodeID.uid != 0)
            openEditorForSlot (0);
    }

    rebuildGraphConnections();

    // O XML so informa qual pagina estava ativa; ele nao prova que a pagina
    // correspondente chegou ao motor neste processo. Reconcilia depois que o
    // construtor terminou e deixa o painel sem destaque ate o SHA ser aplicado.
    if (cenaPendenteNoStartup >= 0)
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (auto* self = safe.getComponent())
                    if (! self->componentShutdownStarted.load (
                            std::memory_order_acquire))
                        self->reconciliarCenaAtivaNoStartup();
            });

    setSize (1280, 720);
    suppressAudioStateSave = false;

    // Launchkey: entra em modo DAW e ja mapeia faders/knobs (sem MIDI Learn).
    // Vem depois do restoreChainState pra os nomes dos timbres irem pro visor.
    setupLaunchkey();

    // setup salvo com instrumento no slot 0: abre direto na mesa do Church Live
    // (sem precisar reselecionar o instrumento a cada abertura)
    if (slots[0].nodeID.uid != 0)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* c = safe.getComponent())
                c->openEditorForSlot (0);
        });

    // Recebe mouseDown dos controles descendentes. No modo MIDI rapido isso
    // permite armar um knob/fader com um clique simples, mesmo quando o valor
    // nao mudou e SliderParameterAttachment nao emitiu callback de parametro.
    addMouseListener (this, true);

    // O escaneador de plugins nunca abre sozinho. Em uma instalacao limpa ele
    // cobria a mesa inteira logo na primeira abertura e fazia o ChurchDAW parecer
    // quebrado, especialmente no macOS. O usuario continua podendo abri-lo pelo
    // botao "Plugins" / "Escanear VST3" quando realmente quiser instalar VST/AU.
}

MainComponent::~MainComponent()
{
   #if JUCE_ANDROID
    glContext.detach();
   #endif
    stopTimer();
    oled.fetchChord = nullptr;
    oled.fetchLevel = nullptr;
    if (liveCamWindow != nullptr) delete liveCamWindow.getComponent();
    beginOwnerTeardown();
    jassert (ownerTeardownGate == nullptr
             || ownerTeardownGate->isClosedAndDrained());
    jassert (parameterCallbackGate.isDrained());
    midiLearnTouchTimer.setRunning (false);
    parameterLearnGeneration.fetch_add (1, std::memory_order_acq_rel);
    parameterTouchMailbox.clear();
   #if CHURCHDAW_REMOTE_CONTROL_ENABLED
    if (remoteServer != nullptr)
        remoteServer->stop();
   #endif
    removeMouseListener (this);
    pluginLoadLifetime->store (false);
    midiCallbackLifetime->store (false);
    // Launchkey owns a separate MidiInput + AsyncUpdater. Destroy it while all
    // processors are still alive so no queued surface callback can be pumped
    // by plugin teardown later in this destructor.
    launchkey.reset();

   #if CHURCHDAW_HOSTS_PLUGINS
    // PluginListHolder guarda referencias para formatManager, knownPlugins,
    // appProperties e pluginScanQuarantine. launchAsync() normalmente o apaga
    // depois de encerrar o modal; durante o shutdown precisamos destrui-lo
    // AGORA, antes que qualquer uma dessas referencias deixe de existir.
    pluginScannerDialog.reset();
   #endif

    // PRIMEIRA COISA A MORRER: o aviso de mudanca de dispositivos MIDI.
    //
    // O callback dele reposta um refreshMidiInputs() via callAsync com
    // SafePointer, mas o WeakReference so e zerado em ~Component — depois do
    // corpo deste destrutor e depois dos membros. Como graph.clear() abaixo
    // destroi VST3/AU hospedados e isso bombeia a fila de mensagens no macOS,
    // um refresh enfileirado podia rodar contra um objeto meio destruido.
    midiListConnection = {};

    // Nenhum callback pode intercalar com o snapshot síncrono final.
    deviceManager.removeMidiInputDeviceCallback ({}, this);
    {
        const juce::ScopedLock drain (midiCallbackBarrier);
        // A aquisição só termina quando qualquer handleIncomingMidiMessage
        // em voo deixou a barreira; mapLock não é tomado enquanto se espera.
    }
    cancelPendingUpdate();
    if (midiMapsLoaded)
    {
        const auto ccFile = arquivoDoMapaDeCCs();
        const auto noteFile = arquivoDoMapaDeNotas();
        bool flushCompleted = false;
        bool transactionClosed = false;

        if (midiMapFileLock != nullptr)
        {
            ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
                ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);
            const bool checkpointReady = diskRollback.capture (
                "shutdown-flush");
            const bool targetsUnchanged = checkpointReady
                && diskRollback.targetsUnchangedSinceCapture();

            if (targetsUnchanged)
            {
                const auto ccEpoch = ccDirtyEpoch.snapshot();
                const auto noteEpoch = noteDirtyEpoch.snapshot();
                const bool ccsOk = saveMidiMap();
                const bool notesOk = ccsOk && saveNoteMap();
                if (ccsOk && notesOk)
                {
                    transactionClosed = diskRollback.commit();
                    flushCompleted = transactionClosed;
                    if (flushCompleted)
                        finishMidiMapDirtyEpochsAfterPairCommit (
                            ccEpoch, true, noteEpoch, true);
                    if (! transactionClosed)
                        transactionClosed =
                            ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                                diskRollback, ccFile, noteFile);
                }
                else
                    transactionClosed =
                        ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                            diskRollback, ccFile, noteFile);
            }
            else if (checkpointReady)
            {
                // Nenhum arquivo nosso foi alterado. Fecha o checkpoint sem
                // reescrever um par que mudou fora deste snapshot.
                transactionClosed = diskRollback.closeWithoutMutation();
            }

            if (diskRollback.preservedExternalChanges())
                midiMapPairRecoveryFailed = true;
            if (! checkpointReady
                 && diskRollback.hadUninspectableTargetConflict())
                bloquearQuartetoMidiDesconhecido (
                    "checkpoint do flush de encerramento");
        }

        if (! flushCompleted)
            juce::Logger::writeToLog (
                "MIDI MAP: flush final do par CC/notas nao foi concluido");
        if (! transactionClosed)
            midiMapPairRecoveryFailed = true;
    }

    librariesPanel.reset();  // encerra rede/download antes de desmontar os processadores
    aboutPanel.reset();
    saveChainState();

    // O editor guarda attachments/referencia para o processador do grafo.
    // Destrua-o enquanto o PadSynthProcessor ainda esta vivo.
    performanceSessionPanel.reset();
    padEditorPanel.reset();
    padPanel.reset();
    meter.reset();

    masterProcessor->removeChangeListener (this);
    masterProcessor->removeListener (this);

    if (padProcessor != nullptr)
        padProcessor->juce::AudioProcessor::removeListener (this);   // par do addListener do construtor

    if (padSynthProcessor != nullptr)
        padSynthProcessor->removeListener (this);

    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    knownPlugins.removeChangeListener (this);
    player.setProcessor (nullptr);

    if (dockedEditor != nullptr)
    {
        dockedEditor->removeComponentListener (this);
        notifyProcessorEditorDeleted (dockedEditor.get());
        dockedEditor.reset();
    }

    audioPanel.reset();      // o attachment do card Audio/MIDI aponta pra parametro do grafo
    pluginWindows.clear();   // destroi os editores antes dos processadores
    for (auto& slot : slots)
        if (auto* live = dynamic_cast<TimbresProcessor*> (slot.proc.load()))
            live->setMidiMappingTopologyGenerationCounter (nullptr);

    // Os quatro knobs macro da sidebar sao SliderParameterAttachment dos
    // parametros do PadSynthProcessor. O graph.clear() abaixo destroi o
    // processador e, com ele, os parametros; se os attachments ainda estiverem
    // vivos, ~ParameterAttachment chama removeListener num AudioProcessorParameter
    // ja liberado -- 0xc0000005 em RtlEnterCriticalSection em TODO fechamento.
    for (auto& attachment : desktopMacroAttachments)
        attachment.reset();

    // Handshake de vida (ver liveHandshakeLock no topo do arquivo):
    // removeMidiInputDeviceCallback acima nao promete drenar um callback de MIDI
    // em voo ate o fim — um handleIncomingMidiMessage prestes a desreferenciar o
    // TimbresProcessor ainda poderia estar rodando. So destroi os nos do grafo
    // depois de adquirir o lock, espelhando unloadSlot.
    {
        const juce::ScopedLock sl (liveHandshakeLock);
        for (auto& slot : slots)
        {
            slot.proc.store (nullptr);
            slot.nodeID = {};
        }
        masterProcessor = nullptr;
        padProcessor = nullptr;
        padSynthProcessor = nullptr;
        sabrinaProcessor = nullptr;
        graph.clear();
    }

    appProperties.getUserSettings()->saveIfNeeded();
    appProperties.closeFiles();
}

void MainComponent::beginLicenseFadeOut()
{
    if (masterProcessor != nullptr)
        masterProcessor->setMasterDb (MasterProcessor::minusInfinityDb);
}

void MainComponent::beginOwnerTeardown() noexcept
{
    componentShutdownStarted.store (true, std::memory_order_release);
    midiLearnActive.store (false, std::memory_order_release);
    midiCallbackLifetime->store (false, std::memory_order_release);
    parameterCallbackGate.closePermanently();
}

bool MainComponent::ownerTeardownIsReady() const noexcept
{
    return parameterCallbackGate.isDrained();
}


//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    CHURCHDAW_UI_SCOPE ("MainComponent::paint");
    g.fillAll (NordLookAndFeel::background());

    // ---- header GRAFITE com acento vermelho ----
    // Era uma chapa vermelha saturada de ponta a ponta: o elemento de maior
    // contraste da tela ficava na moldura, nao no mixer. Invertido — fundo escuro
    // e o vermelho volta a ser acento (filete, wordmark, botao ativo).
    // O material do header cobre o recorte superior (full bleed), mas toda a
    // geometria clicavel comeca em layoutContentBounds.getY().
    auto headerArea = juce::Rectangle<int> (
        layoutContentBounds.getX(), 0, layoutContentBounds.getWidth(),
        juce::jmin (getHeight(), layoutSafeTop + layoutHeaderHeight));
    const int headerContentY = layoutContentBounds.getY();
    const bool compactHeader = compactHeaderLayout;
    const bool narrowHeader = narrowHeaderLayout;
    if (desktopReferenceLayout)
    {
        g.setColour (juce::Colour (0xff16191e));
        g.fillRect (headerArea);
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        g.drawHorizontalLine (headerContentY + 56,
                              (float) headerArea.getX() + 18.0f,
                              (float) headerArea.getRight() - 18.0f);
    }
    else
    {
        g.setColour (NordLookAndFeel::bgApp());
        g.fillRect (headerArea);
        ChurchDAWArtwork::texture (g, headerArea.toFloat(), 0.35f);
    }
    g.setColour (NordLookAndFeel::borderSoft());
    g.fillRect (headerArea.withTop (headerArea.getBottom() - 1));

    if (desktopReferenceLayout)
    {
        if (brandHover)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (brandArea().toFloat(), 6.0f);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawRoundedRectangle (brandArea().toFloat().reduced (0.5f), 6.0f, 1.0f);
        }

        // Marca horizontal CHURCHDAW no canto superior esquerdo do header
        auto brandFont = juce::Font (juce::FontOptions (brandArea().getWidth() < 180 ? 23.0f : 25.0f, juce::Font::bold));
        g.setFont (brandFont);
        juce::GlyphArrangement brandMeasure;
        brandMeasure.addLineOfText (brandFont, "CHURCH", 0.0f, 0.0f);
        const int churchW = juce::roundToInt (
            brandMeasure.getBoundingBox (0, -1, true).getWidth());
        juce::GlyphArrangement dawMeasure;
        dawMeasure.addLineOfText (brandFont, "DAW", 0.0f, 0.0f);
        const int dawW = juce::roundToInt (
            dawMeasure.getBoundingBox (0, -1, true).getWidth());

        const int brandX = brandArea().getX();
        const int brandY = brandArea().getCentreY() - 16;
        g.setColour (juce::Colours::white);
        g.drawText ("CHURCH", brandX, brandY, churchW + 2, 32, juce::Justification::centredLeft);
        g.setColour (NordLookAndFeel::nordRedBright());
        g.drawText ("DAW", brandX + churchW, brandY, dawW + 4, 32, juce::Justification::centredLeft);

        // Os tres modulos tecnicos desenham suas proprias chapas elevadas.
        // Aqui fica apenas a luz ambiente que os une, sem uma quarta caixa.
    }
    else
    {
    // Marca: o icone ja e um "app icon" vermelho vivo com forma propria — nao
    // precisa de chapa atras, so de uma sombra pra pousar no header.
    //
    // ATENCAO: g.drawImageWithin herda o ALPHA do ultimo g.setColour. O codigo
    // antigo pintava um halo com alpha 0.18 e desenhava o logo logo em seguida,
    // entao a marca do produto aparecia a 18% de opacidade — um fantasma. Sempre
    // setOpacity(1.0f) antes de desenhar imagem.
    // a marca inteira e clicavel (abre o "Sobre") — acende de leve sob o mouse
    if (brandHover)
    {
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (brandArea().toFloat(), 9.0f);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (brandArea().toFloat().reduced (0.5f), 9.0f, 1.0f);
    }

   #if JUCE_ANDROID
    {
        const float iconSize = mobileShortLandscapeLayout ? 34.0f
                                                          : compactHeader ? 46.0f : 82.0f;
        const float iconX = (float) layoutSafeLeft
                          + (mobileShortLandscapeLayout ? 5.0f
                                                       : compactHeader ? 8.0f
                                                                       : (narrowHeader ? 12.0f : 17.0f));
        const float iconRowH = mobileTwoRowHeader ? 48.0f
                                                  : (float) layoutHeaderHeight;
        const float iconY = (float) headerContentY
                          + (iconRowH - iconSize) * 0.5f;
        NordLookAndFeel::drawChurchDAWMark (
            g, juce::Rectangle<float> (iconX, iconY, iconSize, iconSize));
    }
   #else
    if (brandIcon.isValid())
    {
        const float iconSize = mobileShortLandscapeLayout ? 24.0f
                                                          : compactHeader ? 32.0f : 46.0f;
        const float iconX = (float) layoutSafeLeft
                          + (mobileShortLandscapeLayout ? 5.0f
                                                       : compactHeader ? 8.0f
                                                                       : (narrowHeader ? 12.0f : 17.0f));
        const float iconRowH = mobileTwoRowHeader ? 48.0f
                                                  : (float) layoutHeaderHeight;
        const float iconY = (float) headerContentY
                          + (iconRowH - iconSize) * 0.5f;
        const auto box = juce::Rectangle<float> (iconX, iconY, iconSize, iconSize);
        const auto boxInt = box.getSmallestIntegerContainer();
        const int plateRadius = compactHeader ? 5 : 8;

        if (! boxInt.isEmpty())
        {
            if (iconShadowCache.isNull()
                 || iconShadowSource != boxInt
                 || iconShadowRadius != plateRadius)
            {
                iconShadowCache = juce::Image (
                    juce::Image::ARGB,
                    boxInt.getWidth()  + iconShadowPad * 2,
                    boxInt.getHeight() + iconShadowPad * 2,
                    true);

                juce::Graphics sg (iconShadowCache);
                juce::Path plate;
                plate.addRoundedRectangle (
                    juce::Rectangle<float> ((float) iconShadowPad,
                                            (float) iconShadowPad,
                                            (float) boxInt.getWidth(),
                                            (float) boxInt.getHeight()),
                    compactHeader ? 9.0f : 11.0f);
                juce::DropShadow (juce::Colours::black.withAlpha (0.55f),
                                  plateRadius,
                                  { 0, compactHeader ? 1 : 2 }).drawForPath (sg, plate);
                iconShadowSource = boxInt;
                iconShadowRadius = plateRadius;
            }

            g.drawImageAt (iconShadowCache,
                           boxInt.getX() - iconShadowPad,
                           boxInt.getY() - iconShadowPad);
        }

        // O PNG do icone tem fundo PRETO (nao transparente). Recorta nos cantos
        // arredondados e desenha um pouco maior, pra moldura preta cair fora do
        // recorte em vez de virar um quadrado escuro no header grafite.
        juce::Graphics::ScopedSaveState clipped (g);
        juce::Path iconPlate;
        iconPlate.addRoundedRectangle (box, compactHeader ? 9.0f : 11.0f);
        g.reduceClipRegion (iconPlate);
        g.setOpacity (1.0f);

        const auto draw = box.expanded (compactHeader ? 1.5f : 3.0f).toNearestInt();
        const auto markSource = juce::Rectangle<int> (
            0, 0, brandIcon.getWidth(),
            juce::jmax (1, juce::roundToInt (brandIcon.getHeight() * 0.73f)));
        const auto mark = brandIcon.getClippedImage (markSource);
        g.drawImageWithin (mark, draw.getX(), draw.getY(), draw.getWidth(), draw.getHeight(),
                           juce::RectanglePlacement::centred);
    }
   #endif

    if (! narrowHeader)
    {
        auto titleFont = juce::Font (juce::FontOptions (
                                mobileShortLandscapeLayout ? 15.0f
                                : compactHeader ? 20.0f : 28.0f))
                             .boldened().italicised();
        g.setFont (titleFont);

        juce::GlyphArrangement measure;
        measure.addLineOfText (titleFont, "CHURCH", 0.0f, 0.0f);
        const int churchW = (int) std::ceil (measure.getBoundingBox (0, -1, true).getWidth());
        const int textX = layoutSafeLeft + (mobileShortLandscapeLayout ? 48
                                                                       : compactHeader ? 62
                                                                                       : 17 + 82 + 8);

        const int titleRowHeight = mobileTwoRowHeader ? 48 : layoutHeaderHeight;
        g.setColour (juce::Colours::white);
        g.drawText ("CHURCH", textX, headerContentY, churchW + 4, titleRowHeight,
                    juce::Justification::centredLeft);
        g.setColour (NordLookAndFeel::nordRedBright());   // "DAW" carrega a cor da marca
        g.drawText ("DAW", textX + churchW, headerContentY, 200, titleRowHeight,
                    juce::Justification::centredLeft);

        if (! compactHeader)
        {
            g.setColour (juce::Colours::white.withAlpha (0.68f));
            g.setFont (NordLookAndFeel::stageLabel (8.0f));
            g.drawFittedText (juce::String::fromUTF8 ("DAW PARA IGREJAS"),
                              juce::Rectangle<int> (textX, headerContentY + 57,
                                                    juce::jmax (1, layoutBrandWidth - textX + layoutSafeLeft - 8), 14),
                              juce::Justification::centredLeft, 1, 0.8f);
        }
    }

    // separador vertical apos a marca
    const int separatorX = layoutSafeLeft + layoutBrandWidth + (compactHeader ? 4 : 8);
    const int separatorInset = mobileShortLandscapeLayout ? 4
                                                          : compactHeader ? 6 : 11;
    const int separatorY = headerContentY + separatorInset;
    const int separatorH = juce::jmax (
        0, (mobileTwoRowHeader ? 48 : layoutHeaderHeight) - separatorInset * 2);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (separatorX, separatorY, 1, separatorH);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (separatorX + 1, separatorY, 1, separatorH);
    }

    // chip de vidro atras do status (kHz / buffer / CPU) — combina com os chips
    // de dispositivo do header
    // No mixer a faixa serve somente como separador de contexto. Canal, estado
    // e timbre ja aparecem nas proprias reguas e nao devem ocupar este espaco.
    if (compactProgramBand && ! programBand.isEmpty())
    {
        auto strip = programBand.toFloat();
        g.setColour (NordLookAndFeel::bgApp());
        g.fillRect (strip);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.fillRect (strip.getX(), strip.getY(), strip.getWidth(), 1.0f);
        g.setColour (desktopReferenceLayout ? juce::Colours::transparentBlack
                                            : NordLookAndFeel::nordRed().withAlpha (0.78f));
        g.fillRect (strip.getX(), strip.getBottom() - 1.0f, strip.getWidth(), 1.0f);

        if (desktopReferenceLayout)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRect (strip.getX(), strip.getBottom() - 1.0f, strip.getWidth(), 1.0f);

            auto mixerHdr = desktopMixerTabBounds;
            g.setColour (NordLookAndFeel::txtSecondary().withAlpha (0.65f));
            g.fillRoundedRectangle ((float) mixerHdr.getX(),
                                    (float) mixerHdr.getCentreY() - 3.5f,
                                    2.0f, 7.0f, 1.0f);
            mixerHdr.removeFromLeft (8);
            g.setColour (NordLookAndFeel::txtPrimary().withAlpha (0.90f));
            g.setFont (NordLookAndFeel::stageLabel (9.6f));
            g.drawText ("MESA AO VIVO", mixerHdr, juce::Justification::centredLeft);
            const int referenceMasterW = juce::jlimit (
                232, 292, juce::roundToInt (programBand.getWidth() * 0.239f));
            const int referenceMasterX = programBand.getRight() - referenceMasterW;
            if (desktopWorkspace == DesktopWorkspace::mixer)
            {
                // Na referencia a coluna MASTER nasce na propria subnavegacao:
                // somente o cabecalho sobe; os modulos comecam alinhados aos
                // channel strips. Essa continuidade evita que o master pareca um
                // oitavo canal comum e devolve a hierarquia visual do mockup.
                auto masterHeader = juce::Rectangle<int> (
                    referenceMasterX + 18, programBand.getY(),
                    referenceMasterW - 36, programBand.getHeight());
                g.setColour (NordLookAndFeel::nordRed());
                g.fillRoundedRectangle ((float) masterHeader.getX(),
                                        (float) masterHeader.getCentreY() - 3.5f,
                                        2.0f, 7.0f, 1.0f);
                masterHeader.removeFromLeft (8);
                g.setColour (NordLookAndFeel::txtPrimary().withAlpha (0.90f));
                g.setFont (NordLookAndFeel::stageLabel (9.6f));
                g.drawText ("MASTER", masterHeader, juce::Justification::centredLeft);
            }
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.72f));
            g.setFont (NordLookAndFeel::stageLabel (10.0f));
            g.drawFittedText ("MIXER", programBand.reduced (18, 0),
                              juce::Justification::centredLeft, 1, 0.82f);
        }
    }

    if (! compactProgramBand && ! programBand.isEmpty()
         && (oled.isVisible() || midiLearnPanel.isVisible() || liveScenePanel.isVisible()))
    {
        auto plate = programBand.toFloat();

        g.setColour (NordLookAndFeel::bgCard());
        g.fillRect (plate);
        g.setColour (NordLookAndFeel::borderSoft());
        g.fillRect (plate.getX(), plate.getY(), plate.getWidth(), 1.0f);

        // No Learn o fio ambar avisa que o modo esta armado; fora dele volta a
        // assinatura vermelha da marca.
        g.setColour ((midiLearnPanel.isVisible() ? NordLookAndFeel::oledAmber()
                                                 : NordLookAndFeel::nordRed()).withAlpha (0.85f));
        g.fillRect (plate.getX(), plate.getBottom() - 1.0f, plate.getWidth(), 1.0f);

        // sombra projetada do visor sobre a chapa (ele esta EMBUTIDO, nao colado)
        // O desfoque vive em cache -- ver oledShadowCache no cabecalho.
        const auto ob = midiLearnPanel.isVisible() ? midiLearnPanel.getBounds()
                                                   : oled.getBounds();

        if (! ob.isEmpty())
        {
            if (oledShadowCache.isNull() || oledShadowSource != ob)
            {
                oledShadowCache = juce::Image (
                    juce::Image::ARGB,
                    ob.getWidth()  + oledShadowPad * 2,
                    ob.getHeight() + oledShadowPad * 2,
                    true);

                juce::Graphics sg (oledShadowCache);
                juce::Path op;
                op.addRoundedRectangle (
                    juce::Rectangle<float> ((float) oledShadowPad,
                                            (float) oledShadowPad,
                                            (float) ob.getWidth(),
                                            (float) ob.getHeight()),
                    7.0f);
                juce::DropShadow (juce::Colours::black.withAlpha (0.75f),
                                  16, { 0, 3 }).drawForPath (sg, op);
                oledShadowSource = ob;
            }

            g.drawImageAt (oledShadowCache,
                           ob.getX() - oledShadowPad,
                           ob.getY() - oledShadowPad);
        }

        // (a serigrafia "PROGRAMA" / nome da biblioteca saiu das laterais: pesava
        // a chapa sem informar nada que o visor ja nao mostrasse)
    }


    if (auto sb = engineStatusBounds; ! sb.isEmpty())
    {
        if (desktopReferenceLayout)
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawVerticalLine (sb.getX(), (float) sb.getY() + 8.0f,
                                (float) sb.getBottom() - 8.0f);
            auto chip = juce::Rectangle<float> ((float) sb.getX() + 11.0f,
                (float) sb.getCentreY() - 7.0f, 14.0f, 14.0f);
            g.setColour (NordLookAndFeel::txtSecondary());
            g.drawRoundedRectangle (chip, 2.0f, 1.25f);
            g.drawRect (chip.reduced (4.0f), 1.0f);
            for (int pin = 0; pin < 3; ++pin)
            {
                const float step = 3.0f + 4.0f * (float) pin;
                g.drawLine (chip.getX() + step, chip.getY() - 3.0f,
                            chip.getX() + step, chip.getY(), 1.0f);
                g.drawLine (chip.getX() + step, chip.getBottom(),
                            chip.getX() + step, chip.getBottom() + 3.0f, 1.0f);
            }
            g.setFont (NordLookAndFeel::stageLabel (8.0f));
            g.setColour (NordLookAndFeel::txtDisabled());
            g.drawText ("ENGINE", sb.withTrimmedLeft (38).withTrimmedTop (6)
                                       .withHeight (14), juce::Justification::centredLeft);
        }
        else
        {
            auto chip = sb.toFloat().reduced (0.0f, 1.0f);
            g.setColour (NordLookAndFeel::bgCard());
            g.fillRoundedRectangle (chip, 7.0f);
            g.setColour (NordLookAndFeel::borderSoft());
            g.drawRoundedRectangle (chip.reduced (0.5f), 7.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.64f));
            g.setFont (NordLookAndFeel::stageLabel (8.0f));
            g.drawText (juce::String::fromUTF8 ("ÁUDIO ENGINE"),
                        sb.reduced (8, 2).removeFromTop (10),
                        juce::Justification::centred, false);
        }
    }

    // ---- cartoes (vidro premium compartilhado) ----
    auto drawCard = [&g] (juce::Rectangle<int> r)
    {
        if (r.isEmpty())
            return;

        NordLookAndFeel::drawGlassCard (g, r.toFloat(), NordLookAndFeel::radiusCard,
                                        false, NordLookAndFeel::nordRed(), false, false);
    };

    for (int i = 0; i < numSlots; ++i)
        drawCard (slotCardBounds[i]);

    drawCard (masterCardBounds);

    // setas entre os cartoes
    g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.85f));

    for (int i = 0; i + 1 < numSlots; ++i)
    {
        if (slotCardBounds[i].isEmpty() || slotCardBounds[i + 1].isEmpty())
            continue;

        const float x = (float) (slotCardBounds[i].getRight() + slotCardBounds[i + 1].getX()) * 0.5f;
        const float y = (float) slotCardBounds[i].getCentreY();
        g.drawArrow (juce::Line<float> (x - 7.0f, y, x + 7.0f, y), 2.0f, 8.0f, 8.0f);
    }

    // cabecalho do painel embutido
    if (dockedEditor != nullptr && ! dockedNative)
    {
        auto strip = getLocalBounds();
        auto dockHeader = layoutDockHeaderBounds;
        g.setColour (NordLookAndFeel::panel());
        g.fillRect (dockHeader);
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRect (dockHeader.removeFromBottom (2));
    }
    else if (dockedEditor == nullptr)
    {
        // Area vazia. A marca d'agua tinha 300px e, com o resto da tela vazia,
        // ela virava o elemento dominante — o recado ficava perdido embaixo dela.
        // Agora o emblema e discreto e o texto ocupa o centro optico.
        auto rest = layoutContentBounds;
        rest.removeFromTop (layoutHeaderHeight + layoutProgramBandHeight + slotsRowActual);

        if (rest.getHeight() > 150)
        {
            const int size = juce::jlimit (120, 190, rest.getHeight() - 210);
            auto iconBox = rest.withSizeKeepingCentre (size, size)
                               .translated (0, -(size / 3 + 10));

           #if JUCE_ANDROID
            g.setOpacity (0.055f);
            NordLookAndFeel::drawChurchDAWMark (g, iconBox.toFloat());
            g.setOpacity (1.0f);
           #else
            if (brandIcon.isValid())
            {
                g.setOpacity (0.05f);
                g.drawImageWithin (brandIcon, iconBox.getX(), iconBox.getY(), size, size,
                                   juce::RectanglePlacement::centred);
                g.setOpacity (1.0f);
            }
           #endif

            g.setColour (juce::Colours::white.withAlpha (0.42f));
            g.setFont (juce::Font (juce::FontOptions (19.0f)).boldened());
            g.drawText ("Pronto para tocar",
                        rest.getX(), iconBox.getBottom() + 16, rest.getWidth(), 24,
                        juce::Justification::centred);

            g.setColour (juce::Colours::white.withAlpha (0.26f));
            g.setFont (juce::Font (juce::FontOptions (13.5f)));
            g.drawText (juce::String::fromUTF8 ("Abra o Church Live no card acima para montar a mesa"),
                        rest.getX(), iconBox.getBottom() + 44, rest.getWidth(), 20,
                        juce::Justification::centred);
        }
    }
}

// Habilita todas as entradas MIDI conectadas, MENOS a porta DAW do Launchkey.
// Chamada na largada e a cada mudanca na lista de dispositivos.
void MainComponent::refreshMidiInputs()
{
   #if CHURCHDAW_UI_PREVIEW
    return; // Isolated visual harness must not open the musician's MIDI ports.
   #endif
    const auto midiIns = juce::MidiInput::getAvailableDevices();

    std::vector<ChurchDAWMidiMapping::MidiSourceDescriptor> refreshedSources;
    refreshedSources.reserve ((size_t) midiIns.size());
    for (const auto& device : midiIns)
        refreshedSources.push_back ({ device.identifier, device.name });

    {
        // A MIDI callback can be comparing a learned control while a hot-plug
        // notification arrives on the message thread. Swap the complete cache
        // under the same lock used by every callback-side traversal so no
        // iterator can outlive the old vector storage.
        const juce::ScopedLock sl (mapLock);
        midiSourcesCached = std::move (refreshedSources);
    }

    if (midiMapsLoaded)
        reindexMidiMappingsAfterRefresh (midiIns);

    int launchkeyDawPortCount = 0;
    for (const auto& device : midiIns)
        if (LaunchkeyControl::isDawPort (device.name))
            ++launchkeyDawPortCount;

    const auto launchkeyPolicy =
        ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
            launchkeyMappingEnabled,
            midiIns.size(),
            launchkeyDawPortCount);

    // A CONTAGEM e registrada mesmo sendo zero, de proposito: sem ela, "nenhuma
    // linha no log" e ambiguo -- pode ser lista vazia ou log que nao chega. Foi
    // essa linha que revelou o problema do Android ("MIDI: 0 entrada(s)").
    juce::Logger::writeToLog ("MIDI: " + juce::String (midiIns.size()) + " entrada(s)");

    // REDE DE SEGURANCA: nunca ficar sem NENHUMA entrada.
    //
    // O filtro de porta DAW e uma heuristica de NOME, e no Android o nome
    // entregue e o do aparelho inteiro — um padrao infeliz derruba todas as
    // portas de uma vez. Foi o que aconteceu na 1.5.4 com "MIDI Int", e o
    // cliente ficou sem teclado nenhum. Se o filtro fosse desligar tudo, ele
    // esta errado: e melhor um pad tocar nota errada do que o instrumento
    // ficar mudo no meio do culto.
    const bool filtroDerrubariaTudo =
        launchkeyPolicy.failSafeKeepingDawPort;

    if (filtroDerrubariaTudo)
        juce::Logger::writeToLog (
            juce::String::fromUTF8 (
                u8"MIDI: o filtro de porta DAW desligaria TODAS as entradas — ignorado "
                u8"nesta varredura para o teclado nao ficar mudo"));

    // A LISTA REGISTRADA TEM DE SER A QUE FOI LIGADA DE VERDADE.
    //
    // Faltava aqui a mesma excecao do laco de baixo: quando filtroDerrubariaTudo
    // vale, o app LIGA todas as portas e esta lista registrava ZERO. Duas
    // consequencias, e a segunda e grave:
    //
    //  1. As etiquetas do Pad, que cruzam o mapa com esta lista, apagavam todas
    //     com o teclado plugado e tocando.
    //  2. Com a lista vazia, o laco que detecta porta que SUMIU nunca roda,
    //     alguemSaiu nunca fica true e resetMidiTransportState NUNCA e chamado.
    //     Cabo solto com um acorde seguro e o pitch bend fora do centro =
    //     nota presa e bend torto ate o PANIC manual. O panico existe
    //     exatamente para isso.
    juce::StringArray agora;
    for (auto& d : midiIns)
        if (launchkeyPolicy.shouldEnableAsCommonInput (
                LaunchkeyControl::isDawPort (d.name)))
            agora.add (d.identifier);

    // O sustain global guarda a origem fisica para aplicar OR entre portas.
    // Remova somente as origens que realmente desapareceram; uma porta ainda
    // conectada com o pedal embaixo precisa sobreviver ao panic de hot-plug e
    // ser reaplicada logo depois dele.
    {
        juce::StringArray fontesAtivas;
        for (const auto& id : agora)
            fontesAtivas.add (
                ChurchDAWMidiMapping::normalizeMidiSourceId (id));

        const juce::ScopedLock sl (mapLock);
        sourceAwareSustain.retainOnlySources (fontesAtivas);
    }

    // Feche primeiro os identificadores que desapareceram. Em especial no
    // Android, apenas observar a lista nova nao solta necessariamente a porta
    // antiga dentro do AudioDeviceManager. Se o mesmo teclado voltar com outro
    // identificador, a conexao fantasma ficava aberta e cada hot-plug acumulava
    // mais uma porta no MidiService.
    bool alguemSaiu = false;
    for (const auto& anterior : entradasMidiConhecidas)
        if (! agora.contains (anterior))
        {
            deviceManager.setMidiInputDeviceEnabled (anterior, false);
            alguemSaiu = true;
            juce::Logger::writeToLog (
                "MIDI IN removida e fechada: id=" + anterior);
        }

    for (auto& d : midiIns)
    {
        const bool daw = ! launchkeyPolicy.shouldEnableAsCommonInput (
            LaunchkeyControl::isDawPort (d.name));
        deviceManager.setMidiInputDeviceEnabled (d.identifier, ! daw);

        // O filtro de porta DAW procura "MIDIIN2"/"MIDIOUT2"/"DAW", que sao
        // convencoes de nome do WINDOWS. Em outro sistema o mesmo teclado chega
        // com outro nome, entao registrar o nome REAL aqui e o que permite
        // descobrir se ele caiu no lado certo do filtro.
        juce::Logger::writeToLog ("MIDI IN: \"" + d.name + "\"  id=" + d.identifier
                                   + (daw ? "  -> DAW (desligada)" : "  -> LIGADA"));

        // No Android o identificador e efemero (muda a cada replug/reinicio).
        // Guarda nome -> id para reescrever os mapas quando o mesmo aparelho
        // voltar com um id novo.
        if (d.name.isNotEmpty())
            nomeParaIdMidi[d.name] = d.identifier;
    }

    if (midiMapsLoaded)
        for (const auto& device : midiIns)
            applyAutomaticControllerProfile (device);

    // PANICO SO QUANDO ALGO SAIU.
    //
    // Um cabo retirado com pitch bend fora do centro nunca envia a volta para
    // 8192, e notas seguradas ficariam presas — por isso o panico existe. Mas
    // panicar a CADA mudanca de lista cortava o som quando um aparelho apenas
    // APARECIA, e isso e um evento comum: no Android a descoberta e assincrona
    // (o proprio JUCE agenda +2 s para Bluetooth) e no macOS parear qualquer
    // coisa dispara uma rajada de eventos de endpoint. O musico via o som
    // morrer sozinho e concluia que o app tinha perdido o teclado.
    const bool primeiraVez = entradasMidiConhecidas.isEmpty() && agora.isEmpty();
    const bool listaMudou = entradasMidiConhecidas != agora;
    entradasMidiConhecidas = agora;

    // Mantem a lista CRUA (com portas DAW) para o poll de reconexao comparar
    // cru-com-cru. Sem isto o poll comparava a lista crua com a filtrada e
    // nunca batiam, gerando refresh em loop.
    {
        juce::StringArray crua;

        for (const auto& d : midiIns)
            crua.add (d.identifier);

        ultimaListaMidiCrua = crua;
    }

    if (alguemSaiu && ! primeiraVez)
        resetMidiTransportState (true);

    // As etiquetas do Pad dizem QUAL tecla comanda cada tom, e apagam quando o
    // teclado daquele mapa nao esta ligado. Trocar de controlador e exatamente
    // o momento em que essa informacao muda — sem isto, o painel continuaria
    // prometendo uma tecla do teclado que ficou em casa.
    if (listaMudou)
        atualizarEtiquetasDoPad();
}

void MainComponent::reindexMidiMappingsAfterRefresh (
    const juce::Array<juce::MidiDeviceInfo>& midiIns)
{
    std::vector<ChurchDAWMidiMapping::MidiSourceDescriptor> available;
    available.reserve ((size_t) midiIns.size());
    for (const auto& device : midiIns)
        available.push_back ({ device.identifier, device.name });

    const auto ccFile = arquivoDoMapaDeCCs();
    const auto noteFile = arquivoDoMapaDeNotas();
    const auto ccLastGood = ccFile.getSiblingFile (
        ccFile.getFileName() + ".last-good");
    const auto noteLastGood = noteFile.getSiblingFile (
        noteFile.getFileName() + ".last-good");
    ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
        ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);

    decltype (ccMappings) previousCC;
    decltype (ccActionMappings) previousActions;
    decltype (noteMappings) previousNotes;
    const bool previousProfilesSuspended = automaticControllerProfilesSuspended;

    ChurchDAWMidiMapping::MidiRekeyResult ccResult;
    ChurchDAWMidiMapping::MidiRekeyResult actionResult;
    ChurchDAWMidiMapping::MidiRekeyResult noteResult;
    bool hasUnresolvedUnnamedSource = false;
    bool pairConflictAfterRekey = false;
    bool changed = false;
    bool memoryMutated = false;
    bool committed = false;
    bool rollbackOk = true;

    beginMidiMapRoutingTransaction();

    const bool checkpointReady = ! midiMapPairRecoveryFailed
        && diskRollback.capture ("before-android-reindex");
    const bool futureSchemaProtected =
        ChurchDAWMidiMapping::midiMapFilesHaveFutureSchema (
            ccFile, ccLastGood, "MIDIMAP")
        || ChurchDAWMidiMapping::midiMapFilesHaveFutureSchema (
            noteFile, noteLastGood, "NOTEMAP");
    const bool diskStillOwned = checkpointReady
        && diskRollback.targetsUnchangedSinceCapture()
        && ! futureSchemaProtected;

    if (diskStillOwned)
    {
        // A copia/rebuild/swap dos tres mapas e uma unica transacao de memoria.
        // A lista available foi capturada antes; nao ha enumeracao, I/O ou log
        // enquanto mapLock esta tomado.
        const juce::ScopedLock sl (mapLock);
        previousCC = ccMappings;
        previousActions = ccActionMappings;
        previousNotes = noteMappings;

        ccResult = ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            ccMappings, available);
        actionResult = ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            ccActionMappings, available);
        noteResult = ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            noteMappings, available);

        const bool noteConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                ccMappings, noteMappings);
        const bool ccConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                ccMappings, ccActionMappings,
                [] (const AcaoDeCC& action)
                {
                    return action.acao.kind != NoteActionKind::none;
                });
        pairConflictAfterRekey = noteConflict || ccConflict;
        if (pairConflictAfterRekey)
        {
            ccMappings = previousCC;
            ccActionMappings = previousActions;
            noteMappings = previousNotes;
            ccResult = {};
            actionResult = {};
            noteResult = {};
        }

        hasUnresolvedUnnamedSource =
            ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                ccMappings, available)
            || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                ccActionMappings, available)
            || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                noteMappings, available);
        changed = ccResult.moved > 0 || actionResult.moved > 0
               || noteResult.moved > 0;
        memoryMutated = changed;
    }

    if (! diskStillOwned)
    {
        rollbackOk = checkpointReady
            && diskRollback.closeWithoutMutation();
        if (! checkpointReady
             && diskRollback.hadUninspectableTargetConflict())
            bloquearQuartetoMidiDesconhecido (
                "checkpoint do reindex MIDI");
        if (! rollbackOk
             || diskRollback.preservedExternalChanges())
            midiMapPairRecoveryFailed = true;
        finishMidiMapRoutingTransaction();
        juce::Logger::writeToLog (
            futureSchemaProtected
                ? "MIDI MAP: rekey Android ignorado para preservar schema futuro"
                : "MIDI MAP: rekey Android cancelado porque os arquivos mudaram");
        return;
    }

    if (pairConflictAfterRekey)
    {
        automaticControllerProfilesSuspended = true;
        rollbackOk = diskRollback.closeWithoutMutation();
        if (! rollbackOk)
            midiMapPairRecoveryFailed = true;
        finishMidiMapRoutingTransaction();
        juce::Logger::writeToLog (
            "MIDI MAP: rekey Android cancelado porque criaria conflito entre controles");
        return;
    }

    automaticControllerProfilesSuspended = hasUnresolvedUnnamedSource;

    if (! changed)
    {
        const bool transactionClosed =
            diskRollback.closeWithoutMutation();
        if (! transactionClosed)
            midiMapPairRecoveryFailed = true;
        finishMidiMapRoutingTransaction();
        return;
    }

    if (ccResult.moved > 0 || actionResult.moved > 0)
        ccDirtyEpoch.markDirty();
    if (noteResult.moved > 0)
        noteDirtyEpoch.markDirty();

    if (ccResult.ambiguous + actionResult.ambiguous + noteResult.ambiguous > 0)
        juce::Logger::writeToLog ("MIDI MAP: nomes duplicados mantiveram entradas stale");
    if (ccResult.collisions + actionResult.collisions + noteResult.collisions > 0)
        juce::Logger::writeToLog ("MIDI MAP: colisões preservadas durante rekey");

    const auto ccEpoch = ccDirtyEpoch.snapshot();
    const auto noteEpoch = noteDirtyEpoch.snapshot();
    const bool ccSaved = saveMidiMap();
    const bool noteSaved = ccSaved && saveNoteMap();
    committed = ccSaved && noteSaved && diskRollback.commit();

    if (committed)
        finishMidiMapDirtyEpochsAfterPairCommit (
            ccEpoch, ccSaved, noteEpoch, noteSaved);

    if (! committed)
    {
        rollbackOk =
            ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                diskRollback, ccFile, noteFile);
        const juce::ScopedLock sl (mapLock);
        ccMappings = std::move (previousCC);
        ccActionMappings = std::move (previousActions);
        noteMappings = std::move (previousNotes);
        automaticControllerProfilesSuspended = previousProfilesSuspended;
    }

    // Success publishes the new source IDs; rollback invalidates any callback
    // that copied the temporary in-memory generation.
    if (memoryMutated)
        bumpMidiMappingGeneration();
    finishMidiMapRoutingTransaction();

    if (! checkpointReady
         && diskRollback.hadUninspectableTargetConflict())
        bloquearQuartetoMidiDesconhecido (
            "checkpoint de comando MIDI");
    if (! rollbackOk || diskRollback.preservedExternalChanges())
        midiMapPairRecoveryFailed = true;

    reconstruirMascaraDeNotas();
    reconstruirMascaraDeCCsDeAcao();
    atualizarEtiquetasDoPad();
    atualizarBotoesDeCCDoPad();

    juce::Logger::writeToLog (
        committed
            ? "MIDI MAP: reindex Android salvo como transacao completa"
            : rollbackOk
                ? "MIDI MAP: reindex Android falhou; par anterior restaurado"
                : "MIDI MAP: reindex Android falhou e o rollback ficou bloqueado");
}

void MainComponent::resetMidiTransportState (
    bool preservePhysicalSustain)
{
    bool reapplyPhysicalSustain = false;

    {
        const juce::ScopedLock sl (mapLock);
        notasSuprimidas.clear();
        reconstruirMascaraDeSuprimidas();
        rawCCGesturesDuringMapTransaction.clear();
        rawCCTransactionGesturePending.store (
            false, std::memory_order_release);

        if (preservePhysicalSustain)
            reapplyPhysicalSustain = sourceAwareSustain.isDown();
        else
        {
            sourceAwareSustain.clear();
            rawCCPhysicalGestures.clear();
        }
    }

    // O panic direto abaixo atende o instrumento nativo. Injete tambem a
    // sequencia MIDI completa no grafo: se o slot 1 estiver com um VST3/AU,
    // ele nao conhece TimbresProcessor::panic() e ficaria com nota/bend preso
    // depois de hot-plug ou resume.
    pianoVisualState->reset();
    juce::MidiBuffer panic;
    panic.ensureSize (4096);
    ChurchDAWPerformanceMidiSafety::appendFullPanic (panic, 0);

    // metadata.getMessage() devolve timestamp = posicao no buffer = 0 aqui, o
    // mesmo defeito do roteamento: o coletor carimbaria samplePosition negativa
    // gigante. Selo com o relogio de alta resolucao, espelhando o que o proprio
    // MidiMessageCollector usa quando injeta mensagens internas.
    const double panicTs = juce::Time::getMillisecondCounterHiRes() * 0.001;

    for (const auto metadata : panic)
    {
        auto msg = metadata.getMessage();
        msg.setTimeStamp (panicTs);
        player.getMidiMessageCollector().addMessageToQueue (msg);
    }

    // O panic zera CC64 em todos os motores. Se ele foi provocado por hot-plug
    // de OUTRA porta, o pedal que continua fisicamente embaixo nao enviara um
    // novo 127 sozinho. Reaplique depois da fronteira terminal, com folga de
    // 5 ms para nunca cair no mesmo sample dos CC120/123 coalescidos.
    if (reapplyPhysicalSustain)
        for (int channel = 1; channel <= 16; ++channel)
        {
            auto sustain = juce::MidiMessage::controllerEvent (
                channel, 64, 127);
            sustain.setTimeStamp (panicTs + 0.005);
            if (auto* live = getChurchLive())
                pianoVisualState->observeHost (sustain, live->getPedalInverted());
            else
                pianoVisualState->observe (sustain);
            player.getMidiMessageCollector().addMessageToQueue (
                sustain);
        }

    if (auto* live = getChurchLive())
        live->panic();

    juce::Logger::writeToLog (
        "MIDI SAFETY: transporte recentralizado apos hot-plug/resume");
}

void MainComponent::performGlobalPanic (bool padSynthAlreadyRequested)
{
    // PANIC is one global operation. Clearing only Church Live leaves hosted
    // VST/AU notes and the continuous Pad sounding; clearing only the graph
    // leaves callback ledgers/sustain able to swallow the next release.
    ChurchDAWPerformanceMidiSafety::performGlobalPanic (
        [this]
        {
            if (padProcessor != nullptr)
                padProcessor->setKey (-1);
        },
        [this]
        {
            if (padPanel != nullptr)
                padPanel->refreshState();
        },
        [this]
        {
            if (padSynthProcessor != nullptr)
                padSynthProcessor->requestPanic();
        },
        [this]
        {
            resetMidiTransportState (false);
        },
        padSynthAlreadyRequested);
}

void MainComponent::resized()
{
    CHURCHDAW_UI_SCOPE ("MainComponent::resized");
    engineStatusBounds = {};
    const bool churchFull = (dockedNative && dockedEditor != nullptr);
    instrumentsButton.setVisible (churchFull);

    // ---- MODO COMPACTO: celular deitado ----
    //
    // Nao e "o mesmo layout menor". Medido no aparelho de teste: a tela util do
    // app em paisagem sao 2350x1080 px fisicos a 480 dpi, e o JUCE desenha em
    // escala 3x -> sobram 820 x 360 pixels LOGICOS. O layout de PC pede
    //     cabecalho 52 + faixa de programa 152 + fileira de slots 236 = 440
    // ou seja, 80 px a mais do que a tela inteira tem de altura, ANTES da mesa.
    // Foi por isso que os controles de canal ficaram abaixo da tela.
    //
    // Encolher a escala nao resolve: para caber tudo, o texto iria a ~1,6 mm de
    // altura -- ilegivel de pe, tocando. Entao a altura e disputada de outro
    // jeito: quem nao e essencial para TOCAR sai da tela.
    //
    // Android/iOS usam a interface touch tambem no tablet. Antes o editor interno
    // ja mudava para a mesa movel, mas este conteiner tratava 600 dp como desktop
    // e lhe impunha o minimo de 1120x400: resultado, barras de rolagem nos dois
    // eixos e o master cortado. Os dois niveis agora tomam a mesma decisao.
   #if CHURCHDAW_MOBILE_SHELL
    const bool compact = true;

    // JUCE permite desenhar atras das barras e recortes do sistema no mobile.
    // O fundo pode ocupar essa area, mas logo e botoes precisam ficar dentro da
    // safe area para nao sumirem sob camera/notch em aparelhos diferentes.
    layoutSafeLeft = 0;
    layoutSafeRight = 0;
    layoutSafeTop = 0;
    layoutSafeBottom = 0;

    if (const auto* display =
            juce::Desktop::getInstance().getDisplays().getDisplayForRect (
                getScreenBounds()))
    {
       #if JUCE_IOS
        layoutSafeLeft = juce::jmax (0, display->safeAreaInsets.getLeft());
        layoutSafeRight = juce::jmax (0, display->safeAreaInsets.getRight());
        layoutSafeTop = juce::jmax (0, display->safeAreaInsets.getTop());
        layoutSafeBottom = juce::jmax (0, display->safeAreaInsets.getBottom());
       #else
        layoutSafeLeft = juce::jlimit (0, 80, display->safeAreaInsets.getLeft());
        layoutSafeRight = juce::jlimit (0, 80, display->safeAreaInsets.getRight());
        layoutSafeTop = juce::jlimit (0, 64, display->safeAreaInsets.getTop());
        layoutSafeBottom = juce::jlimit (0, 64, display->safeAreaInsets.getBottom());
       #endif
    }
   #else
    const bool compact = getHeight() < 600;
    layoutSafeLeft = 0;
    layoutSafeRight = 0;
    layoutSafeTop = 0;
    layoutSafeBottom = 0;
   #endif

    // O fundo continua full bleed, mas nenhum controle interativo entra em
    // notch, recorte, barra de status ou barra gestual. Aplicar apenas esquerda
    // e direita (como antes) ainda deixava o mixer sob a alca de gesto inferior.
   #if CHURCHDAW_MOBILE_SHELL
    // Perfil horizontal baixo medido no TANK3. Mantemos todos os safe insets:
    // o topo de 24 dp faz parte da area protegida do aparelho, enquanto o
    // conteudo visual abaixo dele e compactado de verdade.
    // Mesmos limites do TimbresEditor. Deixar o container em >=700/2:1 e o
    // editor em >=540/1,5:1 fazia 568x320 e 640x360 receberem DOIS layouts
    // incompatíveis: o header alto consumia a janela enquanto a mesa tentava
    // caber em paisagem curta. O resultado era o ENV/EQ abaixo da tela.
    // 560: paisagem de TABLET (1153x506 na matriz) tambem usa o layout de
    // console — com 400, o tablet caia no card empilhado de retrato e
    // cortava os knobs abaixo da dobra (matriz de 30/07). O retrato
    // continua fora pela proporcao 3:2.
    mobileShortLandscapeLayout = getWidth() >= 540
                              && getHeight() <= 560
                              && getWidth() >= getHeight() * 3 / 2;
   #else
    mobileShortLandscapeLayout = false;
   #endif

    layoutContentBounds = getLocalBounds()
                              .withTrimmedLeft (layoutSafeLeft)
                              .withTrimmedRight (layoutSafeRight)
                              .withTrimmedTop (layoutSafeTop)
                              .withTrimmedBottom (layoutSafeBottom);

    if (layoutContentBounds.getWidth() < 1 || layoutContentBounds.getHeight() < 1)
        layoutContentBounds = getLocalBounds();

   #if ! CHURCHDAW_MOBILE_SHELL
    // A referencia desktop usa uma moldura de console completa: a marca e as
    // cenas vivem numa coluna fixa, enquanto header, mixer e rodape ocupam a
    // area principal. Em janelas menores voltamos ao layout responsivo atual.
    // A nova identidade e o shell principal do aplicativo inteiro, nao apenas
    // um estado temporario do Church Live. Assim a navegacao nao muda de forma
    // ao abrir/fechar paineis e o desktop sempre conserva sidebar, header e
    // rodape da referencia.
    // O ScaledHolder normaliza uma janela Full HD para cerca de 1280x672
    // unidades logicas. Estes limites portanto representam o desktop grande
    // real, e nao os pixels fisicos do monitor.
    // Keep the same shell at the normal resizable desktop minimum. The header
    // contracts its technical regions instead of switching to a cramped legacy
    // layout when a native window border subtracts one or two pixels.
    desktopReferenceLayout = DesktopHeaderLayout::supports (layoutContentBounds);
    // FULL SCREEN LIVE MIXER: Sem menu lateral, tudo na cara e com máxima largura para os canais
    desktopSidebarWidth = 0;
    desktopBottomBarHeight = desktopReferenceLayout ? 62 : 0;

    if (desktopReferenceLayout)
    {
        // No shell desktop estes sao dois destinos reais e ambos permanecem
        // visiveis. O destaque segue a pagina que realmente esta na frente:
        // Mapear MIDI e PAD nao podem deixar Instrumentos parecendo ativo so
        // porque a cadeia continua montada por baixo.
        const bool instrumentsActive = ! churchFull
                                    && (! mapButton.getToggleState()
                                         || quickMidiMappingMode)
                                    && ! padEditorVisible;
        instrumentsButton.setVisible (true);
        instrumentsButton.setToggleState (instrumentsActive, juce::dontSendNotification);
        liveButton.setToggleState (churchFull && ! padEditorVisible
                                    && (! mapButton.getToggleState() || quickMidiMappingMode),
                                   juce::dontSendNotification);
        instrumentsButton.setColour (juce::TextButton::buttonColourId,
                                     instrumentsActive ? NordLookAndFeel::nordRedDark()
                                                       : NordLookAndFeel::bgControl());
        liveButton.setColour (juce::TextButton::buttonColourId,
                              NordLookAndFeel::bgControl());
        liveButton.setColour (juce::TextButton::buttonOnColourId,
                              NordLookAndFeel::nordRed());
        instrumentsButton.setButtonText ("Instrumentos");
        liveButton.setButtonText ("Live");
        const bool learnModeActive = mapButton.getToggleState() || midiLearnActive.load() || quickMidiMappingMode;
        mapButton.setButtonText (learnModeActive ? "Concluir MIDI" : "Mapear MIDI");
        padButton.setButtonText ("Pad");
        scanButton.setButtonText ("Plugins");
        librariesButton.setButtonText ("Bibliotecas");
        audioSettingsButton.setButtonText (juce::String::fromUTF8 ("\xc3\x81udio"));
    }
    else
    {
        instrumentsButton.setToggleState (false, juce::dontSendNotification);
        statusInfo.setFont (juce::Font (juce::FontOptions (
            juce::Font::getDefaultMonospacedFontName(), 9.5f, juce::Font::bold)));
        statusInfo.setJustificationType (juce::Justification::centred);
        instrumentsButton.setColour (juce::TextButton::buttonColourId,
                                     juce::Colour (0xffc4112e));
        liveButton.setColour (juce::TextButton::buttonColourId,
                              juce::Colours::transparentBlack);
    }

    if (desktopReferenceLayout)
    {
        desktopSidebarBounds = juce::Rectangle<int> (
            layoutContentBounds.getX(), layoutContentBounds.getY(),
            desktopSidebarWidth, layoutContentBounds.getHeight());
        desktopBottomBarBounds = juce::Rectangle<int> (
            layoutContentBounds.getX() + desktopSidebarWidth,
            layoutContentBounds.getBottom() - desktopBottomBarHeight,
            layoutContentBounds.getWidth() - desktopSidebarWidth,
            desktopBottomBarHeight);
        layoutContentBounds = layoutContentBounds
                                  .withTrimmedLeft (desktopSidebarWidth)
                                  .withTrimmedBottom (desktopBottomBarHeight);
    }
    else
    {
        desktopSidebarBounds = {};
        desktopBottomBarBounds = {};
    }
   #else
    desktopReferenceLayout = false;
    desktopSidebarWidth = 0;
    desktopBottomBarHeight = 0;
    desktopSidebarBounds = {};
    desktopBottomBarBounds = {};
   #endif

    getProperties().set ("churchDesktopShell", desktopReferenceLayout);
    performanceDock.setVisible (desktopReferenceLayout);
    performanceDock.setBounds (desktopBottomBarBounds);

    auto area = layoutContentBounds;

    // 720 dp e uma largura comum em aparelhos 20:9. Tratar exatamente 720 como
    // layout largo deixava a wordmark ocupar o espaco do chip MIDI.
    const int safeContentWidth = juce::jmax (1, layoutContentBounds.getWidth());
    const int safeContentHeight = juce::jmax (1, layoutContentBounds.getHeight());
   #if CHURCHDAW_MOBILE_SHELL
    mobilePortraitLayout = safeContentWidth < safeContentHeight;
   #else
    mobilePortraitLayout = false;
   #endif
    const bool narrowPhone = compact && safeContentWidth <= 760;
   #if CHURCHDAW_MOBILE_SHELL
    // Tablets em paisagem tem largura suficiente para marca e destinos por
    // extenso. Telefones em paisagem (>=680 dp livres) e tablets em paisagem
    // usam uma unica fileira de 52 dp para devolver +56 dp de altura a mesa;
    // retrato e split-view estreito (<680 dp) usam duas fileiras de 48 dp.
    const bool wideMobileTablet = ! mobilePortraitLayout
                               && ! mobileShortLandscapeLayout
                               && safeContentWidth >= 1080;
    mobileTwoRowHeader = churchstudioMobileHeader
                      && (mobilePortraitLayout || safeContentWidth < 680);
    narrowHeaderLayout = narrowPhone
                      || (! mobileTwoRowHeader && safeContentWidth < 900);
   #else
    mobileTwoRowHeader = false;
    narrowHeaderLayout = narrowPhone;
   #endif

   #if CHURCHDAW_MOBILE_SHELL
    layoutHeaderHeight = mobileTwoRowHeader ? 108 : 52;
    // A FAIXA CRESCE PARA AS CENAS.
    //
    // O visor mostra texto e cabe em 52 px; as dez cenas sao ALVOS DE DEDO e
    // nao cabem. Com 52 px, dez botoes lado a lado num celular de 360 dp davam
    // 28 px de largura — no escuro, isso e tocar na cena errada, e a cena
    // errada no meio do culto e a musica errada. Com 104 px o painel passa a
    // usar duas fileiras de cinco: 57 px de largura por 40 de altura, que ja e
    // um alvo de verdade.
    //
    // So cresce quando as cenas estao na tela: fora do Live Mode a faixa
    // continua exatamente como era, e o resto do layout nao muda.
    const bool cenasNaFaixa = liveScenePanel.isVisible();

    layoutProgramBandHeight = mobilePortraitLayout ? (cenasNaFaixa ? 104 : 52)
                                                   : mobileShortLandscapeLayout ? (cenasNaFaixa ? 56 : 32)
                                                                                : mobileProgramBandHeight;
    layoutBrandWidth = mobileTwoRowHeader ? (narrowPhone ? 58 : 166)
                     : safeContentWidth < 900 ? 48
                     : wideMobileTablet ? 214 : 158;
    desktopHeaderTwoRows = false;
   #else
    desktopHeaderTwoRows = desktopReferenceLayout || safeContentWidth < 1160;
    layoutHeaderHeight = desktopReferenceLayout ? DesktopHeaderLayout::height
                       : compact ? (desktopHeaderTwoRows ? 82 : 46)
                                 : desktopHeaderTwoRows ? 96 : headerHeight;
    layoutProgramBandHeight = desktopReferenceLayout
                                      ? (midiLearnActive.load() ? 66
                                         : liveScenePanel.isVisible() ? 90 : 30)
                                      : compact ? 60
                                      : desktopHeaderTwoRows ? 96
                                                             : programBandHeight;
    // A marca agora ocupa uma zona horizontal: simbolo da igreja + wordmark.
    layoutBrandWidth = desktopReferenceLayout ? 0
                     : narrowPhone ? 70 : compact ? 300 : 360;
   #endif
    compactHeaderLayout = compact;

    const int headerH = layoutHeaderHeight;
   #if CHURCHDAW_MOBILE_SHELL
    compactProgramBand = false;
   #else
    // Com a mesa Church Live aberta, o visor grande repete o canal/timbre que
    // as reguas ja mostram. Conservamos uma faixa curta para contexto, mas
    // devolvemos a maior parte dessa altura ao mixer.
    compactProgramBand = churchFull
                      && ! liveScenePanel.isVisible()
                      && ! midiLearnPanel.isVisible();
   #endif

    if (compactProgramBand)
        layoutProgramBandHeight = desktopReferenceLayout ? 34
                                                         : compactProgramBandHeight;

    const int bandH = layoutProgramBandHeight;

    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
    {
        desktopSceneBounds[i] = {};
        desktopSceneIndexAtRow[i] = i;
    }

    desktopSaveBounds = {};
    desktopResetBounds = {};
    desktopAudioBounds = {};
    desktopHeaderExtrasBounds = {};
    desktopBpmBounds = {};
    desktopTapBounds = {};
    desktopSettingsBounds = {};
    desktopLiveStatusBounds = {};
    desktopMixerTabBounds = {};
    desktopPerformanceTabBounds = {};
    desktopSessionTabBounds = {};
    desktopSceneAddBounds = {};
    for (auto& bounds : desktopFavouriteBounds)
        bounds = {};

    if (desktopReferenceLayout && desktopSidebarWidth > 0 && ! desktopSidebarBounds.isEmpty())
    {
        const int sideX = desktopSidebarBounds.getX() + 12;
        const int sideW = desktopSidebarBounds.getWidth() - 24;
        const int firstSceneY = desktopSidebarBounds.getY() + 76;

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            desktopSceneBounds[i] = { sideX, firstSceneY + i * 19,
                                      sideW, 18 };
            if (desktopWorkspace != DesktopWorkspace::mixer
                 && ! setlist.vazio())
                desktopSceneIndexAtRow[i] = i < setlist.tamanho
                                              ? setlist.ordem[(size_t) i]
                                              : -1;
        }

        auto actions = desktopBottomBarBounds.reduced (12, 8);
        desktopAudioBounds = actions.removeFromRight (228);
        actions.removeFromRight (10);
        desktopResetBounds = actions.removeFromRight (88);
        actions.removeFromRight (8);
        desktopSaveBounds = actions.removeFromRight (104);

        const int macrosTop = desktopSidebarBounds.getY() + 42 + 230 + 10 + 140 + 10;
        auto macroCard = juce::Rectangle<int> (
            desktopSidebarBounds.getX() + 12,
            macrosTop,
            desktopSidebarBounds.getWidth() - 24, 116);
        const int macroW = macroCard.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            auto cell = juce::Rectangle<int> (
                macroCard.getX() + i * macroW, macroCard.getY() + 40,
                macroW, juce::jmin (68, macroCard.getHeight() - 36));
            desktopMacroSliders[(size_t) i].setBounds (
                cell.removeFromTop (38).withSizeKeepingCentre (34, 34));
            desktopMacroSliders[(size_t) i].setVisible (true);
        }
    }
    else
    {
        for (auto& slider : desktopMacroSliders)
        {
            slider.setVisible (false);
            slider.setBounds ({});
        }
    }

    // larguras enxutas: cada pixel economizado aqui vira nome de aparelho legivel
    // nos chips (antes saia "Launchkey..." / "Focus...")
    // 44 dp de altura no touch. Antes o recuo vertical de 7 deixava apenas
    // 32 dp: bonito no print, pequeno demais para acertar rapido no palco.
    auto headerRow = area.removeFromTop (juce::jmin (headerH, area.getHeight()));
    auto headerControls = desktopReferenceLayout
        ? headerRow.reduced (18, 0)
        : headerRow.reduced (compact ? 8 : 12,
                             mobileShortLandscapeLayout ? 2
                                                        : compact ? 4 : 11);

    // PAINEL DE PROGRAMA: faixa propria logo abaixo do cabecalho, centralizado na
    // largura. Estava enfiado no cabecalho e ficava apertado e feio -- ele e um
    // bloco de painel, nao um chip de barra de ferramentas.
    {
        programBand = area.removeFromTop (bandH);

        if (desktopReferenceLayout)
        {
            auto tabs = programBand.reduced (18, 4);
            desktopMixerTabBounds = tabs.removeFromLeft (110);
            desktopPerformanceTabBounds = {};
            desktopSessionTabBounds = {};
        }

        // No compacto o visor usa a LARGURA TODA e quase toda a altura da faixa:
        // com 60 px nao ha o que sobrar para margem decorativa. Ele continua
        // sendo o elemento que se le de longe -- so deixa de ser um bloco de
        // 152 px de altura mostrando o mesmo texto.
        oled.setBounds (compact ? programBand.reduced (
                                      mobileShortLandscapeLayout ? 6 : 8,
                                      mobileShortLandscapeLayout ? 2 : 3)
                                : programBand.withSizeKeepingCentre (juce::jmin (860, programBand.getWidth() - 48),
                                                                     programBand.getHeight() - 16));
        if (desktopReferenceLayout)
        {
            auto quickBanner = programBand.reduced (10, 5);
            midiLearnPanel.setBounds (quickBanner);
        }
        else
        {
            midiLearnPanel.setBounds (compact ? programBand.reduced (
                                                    mobileShortLandscapeLayout ? 6 : 8,
                                                    mobileShortLandscapeLayout ? 2 : 3)
                                              : programBand.withSizeKeepingCentre (
                                                    juce::jmin (960, programBand.getWidth() - 48),
                                                    programBand.getHeight() - 22));
        }

        // As cenas ocupam o MESMO retangulo do visor e do MIDI Learn: e uma
        // superficie que ja alterna ocupantes, entao o Live Mode entra sem
        // disputar largura no cabecalho, que no celular ja esta no limite.
        liveScenePanel.setBounds (compact ? programBand.reduced (
                                                mobileShortLandscapeLayout ? 6 : 8,
                                                mobileShortLandscapeLayout ? 2 : 3)
                                          : programBand.withSizeKeepingCentre (juce::jmin (960, programBand.getWidth() - 48),
                                                                               programBand.getHeight() - 22));
    }
    const bool learning = mapButton.getToggleState() || midiLearnActive.load();

    midiLearnPanel.setVisible (learning);
    if (learning)
        oled.setVisible (false);

    if (compactProgramBand)
        oled.setVisible (false);
    else if (churchFull && ! learning && ! liveScenePanel.isVisible())
        oled.setVisible (true);
    const int gap = compact ? 4 : 7;

   #if CHURCHDAW_MOBILE_SHELL
    // Orcamento de largura do header mobile (800 dp, Church Live aberto):
    // 166 marca + 112 dispositivo + 474 acoes/gaps + 22 margens = 774.
    // Em 720 dp entra o ramo narrow (58 + 112 + 420 + 22 = 612).
    // Assim o chip MIDI nunca e espremido a poucos pixels nem invade botoes.
    const int audioW = wideMobileTablet ? 96 : narrowPhone ? 68 : 88;
    const int librariesW = wideMobileTablet ? 116 : narrowPhone ? 64 : 88;
    const int scanW = wideMobileTablet ? 90 : narrowPhone ? 68 : 72;
    const int padW = wideMobileTablet ? 66 : narrowPhone ? 52 : 54;
    const int saveW = wideMobileTablet ? 72
                    : mobileShortLandscapeLayout ? 64
                    : narrowPhone ? 58 : 54;
    const int mapW = wideMobileTablet ? 110 : narrowPhone ? 76 : 82;
    const int clearW = wideMobileTablet ? 102 : narrowPhone ? 78 : 82;
    const int instrumentsW = wideMobileTablet ? 132 : narrowPhone ? 82 : 88;
    const int liveW = wideMobileTablet ? 70 : 62;
    const int deviceW = wideMobileTablet ? 132 : 112;
   #else
    const int audioW = narrowPhone ? 70 : compact ? 96 : 104;
    const int librariesW = narrowPhone ? 64 : compact ? 104 : 112;
    const int scanW = compact ? 76 : 84;
    const int padW = narrowPhone ? 58 : compact ? 64 : 70;
    const int saveW = narrowPhone ? 70 : compact ? 78 : 86;
    const int mapW = narrowPhone ? 78 : compact ? 94 : 100;
    const int clearW = narrowPhone ? 82 : compact ? 90 : 96;
    const int instrumentsW = narrowPhone ? 84 : compact ? 96 : 108;
    const int deviceW = narrowPhone ? 116 : compact ? 128 : 392;
   #endif

    // O texto completo virava "Áudio ..." em larguras intermediárias. O
    // tooltip mantém o significado e o botão permanece legível.
    audioSettingsButton.setButtonText (juce::String::fromUTF8 (
        desktopReferenceLayout ? "\xc3\x81udio" : "\xc3\x81udio"));
    audioSettingsButton.setTooltip (juce::String::fromUTF8 (
        "Abrir configura\xc3\xa7\xc3\xb5" "es de \xc3\xa1udio e MIDI"));
    librariesButton.setButtonText (desktopReferenceLayout ? "Bibliotecas"
                                     : mobileShortLandscapeLayout ? "Biblioteca"
                                     : "Bibliotecas");
    instrumentsButton.setButtonText (desktopReferenceLayout ? "Instrumentos"
                                         : (narrowPhone || mobileShortLandscapeLayout) ? "Timbres"
                                         : "Instrumentos");
    liveButton.setButtonText (desktopReferenceLayout ? "Live" : "Live");
    mapButton.setButtonText (desktopReferenceLayout
                                     ? learning ? "Concluir MIDI"
                                                : "Mapear MIDI"
                                     : learning ? "Concluir"
                                     : (narrowPhone || mobileShortLandscapeLayout)
                                           ? "Mapear" : "Mapear MIDI");
    padButton.setButtonText (desktopReferenceLayout ? "Pad" : "Pad");
    scanButton.setButtonText (desktopReferenceLayout ? "Plugins" : "Plugins");

    if (! clearMapConfirmationArmed)
        clearMapButton.setButtonText (narrowPhone ? "Limpar" : "Limpar mapas");

    // No celular o cabecalho e uma barra de acao, nao um painel de configuracao.
    // Durante o Learn, botoes sem relacao com a tarefa saem temporariamente e o
    // usuario fica com Instrumentos + Limpar + Concluir. Ao sair, tudo volta.
    audioSettingsButton.setVisible (! learning);
    librariesButton.setVisible (! learning);
    saveButton.setVisible (! learning);
   #if CHURCHDAW_MOBILE_SHELL
    liveCamButton.setVisible (! learning);
   #endif
   #if JUCE_IOS
    scanButton.setVisible (! learning);
   #endif
    liveButton.setVisible (! learning);
    statusInfo.setVisible ((! compact || desktopHeaderTwoRows) && ! learning);

   #if CHURCHDAW_HOSTS_PLUGINS
    scanButton.setVisible (! learning);
   #endif

   #if CHURCHDAW_MOBILE_SHELL
    if (mobileTwoRowHeader)
    {
        // Retrato/freeform: primeira linha conserva marca, dispositivos e o
        // estado de salvamento; segunda linha e uma barra de acoes de 48 dp.
        // Todos os itens ganham a mesma largura, evitando botoes negativos ou
        // sobrepostos quando a janela fica estreita no Android 16.
        auto identityRow = headerControls.removeFromTop (48);
        headerControls.removeFromTop (4);
        auto actionRow = headerControls.removeFromTop (48);

        identityRow.removeFromLeft (juce::jmin (layoutBrandWidth, identityRow.getWidth()));
        saveButton.setBounds (learning ? juce::Rectangle<int>()
                                      : identityRow.removeFromRight (
                                            juce::jmin (64, identityRow.getWidth())));
        identityRow.removeFromRight (learning ? 0 : gap);
        deviceBar.setCompact (true);
        deviceBar.setIntegratedRack (false);
        deviceBar.setVisible (true);
        deviceBar.setBounds (identityRow);
        statusInfo.setBounds ({});

        // Zera ANTES de distribuir. Sem isto, um botao que sai da barra nesta
        // orientacao guarda os bounds da anterior: girar o tablet de paisagem
        // para retrato deixava o Live como um botao invisivel porem clicavel
        // por cima da fileira de identidade.
        liveButton.setBounds ({});

        juce::Array<juce::Button*> actions;

        if (churchFull)
            actions.add (&instrumentsButton);

        if (learning)
        {
            actions.add (&clearMapButton);
            actions.add (&mapButton);
        }
        else
        {
            // O Live entra na barra de acoes do RETRATO. Faltava aqui, e o
            // resultado era o modo simplesmente inalcancavel no celular em pe
            // — que e como a maioria dos musicos segura o aparelho.
            actions.add (&liveButton);
            actions.add (&mapButton);
            actions.add (&padButton);
           #if JUCE_IOS
            actions.add (&scanButton);
           #endif
           #if CHURCHDAW_MOBILE_SHELL
            actions.add (&liveCamButton);
           #endif
            actions.add (&librariesButton);
            actions.add (&audioSettingsButton);
        }

        const int actionGap = 4;
        const int available = juce::jmax (1, actionRow.getWidth()
                                            - actionGap * juce::jmax (0, actions.size() - 1));
        const int itemW = juce::jmax (1, available / juce::jmax (1, actions.size()));

        for (int i = 0; i < actions.size(); ++i)
        {
            const int w = i == actions.size() - 1 ? actionRow.getWidth() : itemW;
            actions.getUnchecked (i)->setBounds (actionRow.removeFromLeft (w));

            if (i < actions.size() - 1)
                actionRow.removeFromLeft (actionGap);
        }

        clearMapButton.setBounds (learning ? clearMapButton.getBounds()
                                          : juce::Rectangle<int>());
    }
    else
   #endif
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        // Desktop premium: quatro grupos com orcamentos independentes. A versao
        // anterior removia todos os botoes da direita com larguras fixas e so
        // depois entregava o que sobrasse aos cards de dispositivo; em 1280 px
        // isso podia ser zero. Aqui cada grupo recebe largura antes do layout.
        headerControls.removeFromLeft (layoutBrandWidth);
        headerControls.removeFromRight (8);

        for (juce::Button* button : std::initializer_list<juce::Button*> { &instrumentsButton, &liveButton, &mapButton,
                                                                            &clearMapButton, &saveButton, &padButton,
                                                                            &scanButton, &liveCamButton, &librariesButton,
                                                                            &audioSettingsButton })
            button->setBounds ({});

        auto layoutNavigation = [&] (juce::Rectangle<int> row)
        {
            juce::Array<juce::Button*> buttons;
            juce::Array<int> weights;
            auto add = [&] (juce::Button& button, int weight)
            {
                buttons.add (&button);
                weights.add (weight);
            };

            if (churchFull || desktopReferenceLayout)
                // O destino primario precisa caber por extenso mesmo na work
                // area de 1280; com o peso antigo virava "Instrument...".
                add (instrumentsButton, 140);

            if (learning)
            {
                add (clearMapButton, 96);
                add (mapButton, 92);
            }
            else
            {
                add (liveButton, desktopReferenceLayout ? 80 : 54);
                add (mapButton, desktopReferenceLayout ? 136 : 96);
                if (! desktopReferenceLayout) add (saveButton, 70);
                add (padButton, desktopReferenceLayout ? 70 : 54);

               #if CHURCHDAW_HOSTS_PLUGINS
                add (scanButton, desktopReferenceLayout ? 92 : 72);
               #endif

                add (liveCamButton, desktopReferenceLayout ? 108 : 106);
                add (librariesButton, desktopReferenceLayout ? 116 : 92);
                add (audioSettingsButton, 100);
            }

            // Uma barra de navegacao continua: a separacao vem da luz/estado
            // interno de cada destino, nao de sete frestas pretas.
            const int navGap = desktopReferenceLayout ? 5 : 0;
            const int gapTotal = navGap * juce::jmax (0, buttons.size() - 1);
            const int usable = juce::jmax (1, row.getWidth() - gapTotal);
            int totalWeight = 0;

            for (auto weight : weights)
                totalWeight += weight;

            for (int i = 0; i < buttons.size(); ++i)
            {
                const int w = i == buttons.size() - 1
                    ? row.getWidth()
                    : juce::jmax (1, juce::roundToInt (
                        (double) usable * (double) weights.getUnchecked (i)
                            / (double) juce::jmax (1, totalWeight)));
                buttons.getUnchecked (i)->setBounds (row.removeFromLeft (w));

                if (i < buttons.size() - 1)
                    row.removeFromLeft (navGap);
            }
        };

        deviceBar.setCompact (compact && ! desktopHeaderTwoRows);
        deviceBar.setIntegratedRack (desktopReferenceLayout);
        deviceBar.setVisible (true);

        if (desktopReferenceLayout)
        {
            const auto header = DesktopHeaderLayout::calculate (headerRow);
            deviceBar.setCompact (false);
            deviceBar.setBounds (header.devices);
            engineStatusBounds = header.engine;
            statusInfo.setFont (juce::Font (juce::FontOptions (11.0f)));
            statusInfo.setJustificationType (juce::Justification::centredLeft);
            statusInfo.setBounds (header.engine.withTrimmedLeft (38)
                                                   .withTrimmedTop (23)
                                                   .withTrimmedBottom (7));
            saveButton.setBounds (learning ? juce::Rectangle<int>() : header.save);
            layoutNavigation (header.navigation);
        }
        else if (desktopHeaderTwoRows && ! learning)
        {
            constexpr int rowGap = 6;
            auto statusRow = headerControls.removeFromTop (
                (headerControls.getHeight() - rowGap) / 2);
            headerControls.removeFromTop (rowGap);
            auto navigationRow = headerControls;

            auto engine = statusRow.removeFromRight (
                juce::jmin (176, statusRow.getWidth()));
            engineStatusBounds = engine;
            statusInfo.setBounds (engine.withTrimmedTop (10).reduced (4, 0));
            statusRow.removeFromRight (10);
            deviceBar.setBounds (statusRow.removeFromLeft (
                juce::jmin (420, statusRow.getWidth())));
            layoutNavigation (navigationRow);
        }
        else
        {
            const int navW = learning
                ? juce::jlimit (250, 390,
                                juce::roundToInt (headerControls.getWidth() * 0.36f))
                : compact
                    ? juce::jlimit (420, 760,
                                    juce::roundToInt (headerControls.getWidth() * 0.70f))
                    : juce::jlimit (500, 600,
                                    juce::roundToInt (headerControls.getWidth() * 0.54f));
            auto navigationRow = headerControls.removeFromRight (
                juce::jmin (navW, headerControls.getWidth()));
            headerControls.removeFromRight (juce::jmin (12, headerControls.getWidth()));

            if (! compact && ! learning)
            {
                engineStatusBounds = headerControls.removeFromRight (
                    juce::jmin (176, headerControls.getWidth()));
                statusInfo.setBounds (
                    engineStatusBounds.withTrimmedTop (10).reduced (4, 0));
                headerControls.removeFromRight (
                    juce::jmin (10, headerControls.getWidth()));
            }
            else
            {
                statusInfo.setBounds ({});
            }

            deviceBar.setBounds (headerControls);
            layoutNavigation (navigationRow);
        }
       #else
        for (juce::Button* button : std::initializer_list<juce::Button*> {
                 &instrumentsButton, &liveButton, &mapButton, &clearMapButton,
                 &saveButton, &sabrinaButton, &padButton, &scanButton,
                 &liveCamButton, &librariesButton, &audioSettingsButton })
            button->setBounds ({});

        headerControls.removeFromLeft (
            juce::jmin (layoutBrandWidth + (narrowHeaderLayout ? 8 : 12),
                        headerControls.getWidth()));
        headerControls.removeFromRight (juce::jmin (4, headerControls.getWidth()));

        juce::Array<juce::Button*> navButtons;
        juce::Array<int> navWeights;
        auto addMobileNav = [&] (juce::Button& button, int weight)
        {
            navButtons.add (&button);
            navWeights.add (weight);
        };

        if (churchFull)
            addMobileNav (instrumentsButton, wideMobileTablet ? 118 : 74);

        if (learning)
        {
            addMobileNav (clearMapButton, 92);
            addMobileNav (mapButton, 88);
        }
        else
        {
            addMobileNav (liveButton, 52);
            addMobileNav (mapButton, wideMobileTablet ? 96 : 68);
            addMobileNav (saveButton, 60);
            addMobileNav (padButton, 50);
           #if JUCE_IOS || CHURCHDAW_HOSTS_PLUGINS
            addMobileNav (scanButton, 68);
           #endif
            addMobileNav (liveCamButton, 66);
            addMobileNav (librariesButton, wideMobileTablet ? 104 : 78);
            addMobileNav (audioSettingsButton, 62);
        }

        const int navGap = 4;
        int totalWeight = 0;
        for (const auto w : navWeights)
            totalWeight += w;
        const int idealNavW = totalWeight
                            + navGap * juce::jmax (0, navButtons.size() - 1);
        const int minDeviceW = learning ? 140
                             : wideMobileTablet ? 148
                             : safeContentWidth >= 900 ? 116 : 94;
        const int maxNavAvailable = juce::jmax (
            1, headerControls.getWidth() - minDeviceW - 6);
        const int actualNavW = juce::jmin (idealNavW, maxNavAvailable);
        auto navRow = headerControls.removeFromRight (
            juce::jmin (actualNavW, headerControls.getWidth()));
        headerControls.removeFromRight (juce::jmin (6, headerControls.getWidth()));

        statusInfo.setBounds ({});
        deviceBar.setCompact (true);
        deviceBar.setIntegratedRack (false);
        deviceBar.setVisible (true);
        deviceBar.setBounds (headerControls.removeFromLeft (
            juce::jmin (wideMobileTablet ? 240 : 188, headerControls.getWidth())));

        const int usableNav = juce::jmax (
            1, navRow.getWidth() - navGap * juce::jmax (0, navButtons.size() - 1));
        for (int i = 0; i < navButtons.size(); ++i)
        {
            const int w = i == navButtons.size() - 1
                ? navRow.getWidth()
                : juce::jmax (1, juce::roundToInt (
                      (double) usableNav * (double) navWeights.getUnchecked (i)
                      / (double) juce::jmax (1, totalWeight)));
            navButtons.getUnchecked (i)->setBounds (navRow.removeFromLeft (w));
            if (i < navButtons.size() - 1)
                navRow.removeFromLeft (juce::jmin (navGap, navRow.getWidth()));
        }
       #endif
    }

    // Church Live em tela cheia: a fileira de cima some inteira e a mesa
    // ganha toda a altura da janela
    masterLabel.setVisible (! churchFull);
    masterSlider.setVisible (! churchFull);

    if (meter != nullptr)
        meter->setVisible (! churchFull);

    if (churchFull)
    {
        slotsRowActual = 0;
        masterCardBounds = {};

        for (int i = 0; i < numSlots; ++i)
        {
            slots[i].label.setVisible (false);
            slots[i].box.setVisible (false);
            slots[i].openButton.setVisible (false);
            slotCardBounds[i] = {};
        }
    }
    else
    {
    // fileira de efeitos se aposenta quando o Church Live assume (efeitos
    // passam a viver dentro dele); volta se algum FX externo estiver em uso
    bool fxRetired = slots[0].pluginIdentifier.startsWith ("internal:timbres");

    for (int i = 1; fxRetired && i < numSlots; ++i)
        if (slots[i].nodeID.uid != 0)
            fxRetired = false;

    // Sobrando UM card so, a fileira pode ser mais alta — e assim a regua master
    // deixa de ser um quadradinho de 150x148 com o fader cortado pela metade.
    //
    // No compacto ela NAO some, so encolhe. Sumir seria uma armadilha: enquanto o
    // Church Live nao esta aberto, este card e o UNICO caminho para abrir ele --
    // esconde-lo deixaria o usuario preso numa tela sem saida. Assim que o Church
    // Live assume (churchFull), a fileira inteira se aposenta pelo caminho normal
    // e a mesa fica com a altura toda.
    slotsRowActual = compact ? 118
                             : (fxRetired ? 236 : slotsRowHeight);

    auto slotsRow = area.removeFromTop (slotsRowActual).reduced (12, 10);

    {
        masterCardBounds = slotsRow.removeFromRight (compact ? 168 : (fxRetired ? 190 : masterStripWidth));
        slotsRow.removeFromRight (10);

        auto inner = masterCardBounds.reduced (compact ? 8 : 12);

        // No compacto o rotulo "MASTER" sai: sobrou espaco para o fader OU para
        // a palavra, e num painel de 118 px o fader e o que se usa tocando.
        if (compact)
        {
            masterLabel.setBounds ({});
        }
        else
        {
            masterLabel.setBounds (inner.removeFromTop (20));
            inner.removeFromTop (6);
        }

        meter->setBounds (inner.removeFromLeft (juce::jmin (compact ? 46 : 64, inner.getWidth() / 2)));
        inner.removeFromLeft (8);
        masterSlider.setBounds (inner);
    }

    slots[0].label.setVisible (true);
    slots[0].box.setVisible (true);
    slots[0].openButton.setVisible (true);

    for (int i = 1; i < numSlots; ++i)
    {
        slots[i].label.setVisible (! fxRetired);
        slots[i].box.setVisible (! fxRetired);
        slots[i].openButton.setVisible (! fxRetired);
    }

    const int visibleSlots = fxRetired ? 1 : numSlots;

    if (fxRetired)
    {
        // um card so fica CENTRADO na faixa; antes ficava encostado no canto
        // esquerdo com a tela inteira vazia do lado
        slotCardBounds[0] = slotsRow.withSizeKeepingCentre (juce::jmin (slotsRow.getWidth(), 470),
                                                            slotsRow.getHeight());

        for (int i = 1; i < numSlots; ++i)
            slotCardBounds[i] = {};
    }
    else
    {
        const int w = slotsRow.getWidth() / visibleSlots;

        for (int i = 0; i < numSlots; ++i)
            slotCardBounds[i] = slotsRow.removeFromLeft (w).reduced (6, 0);
    }

    for (int i = 0; i < visibleSlots; ++i)
    {
        auto inner = slotCardBounds[i].reduced (compact ? 8 : 14);

        if (compact)
        {
            // Sem o rotulo: "INSTRUMENTO" escrito em cima de uma caixa que ja
            // mostra o nome do instrumento e texto redundante gastando 26 px de
            // uma tela que tem 360. As duas caixas ficam com 34 px cada -- acima
            // do minimo de toque do Android (48 dp), porque aqui 34 px logicos
            // sao 102 px fisicos = 5,4 mm, e o dedo acerta.
            slots[i].label.setBounds ({});
            inner = inner.withSizeKeepingCentre (inner.getWidth(), 76);
            slots[i].box.setBounds (inner.removeFromTop (34));
            inner.removeFromTop (8);
            slots[i].openButton.setBounds (inner.removeFromTop (34));
        }
        else
        {
            inner = inner.withSizeKeepingCentre (inner.getWidth(), 92);
            slots[i].label.setBounds (inner.removeFromTop (18));
            inner.removeFromTop (8);
            slots[i].box.setBounds (inner.removeFromTop (28));
            inner.removeFromTop (10);
            slots[i].openButton.setBounds (inner.removeFromTop (28));
        }
    }
    }

    // painel de plugin embutido ocupa o resto
    layoutDockHeaderBounds = {};

    if (dockedEditor != nullptr)
    {
        if (! dockedNative)
        {
            auto dockHeader = area.removeFromTop (dockHeaderHeight);
            layoutDockHeaderBounds = dockHeader;
            dockCloseButton.setBounds (dockHeader.removeFromRight (74).reduced (4, 3));
            dockFloatButton.setBounds (dockHeader.removeFromRight (90).reduced (4, 3));
            dockTitle.setBounds (dockHeader.reduced (14, 0));
        }

        dockViewport.setBounds (area);

        // o Church Live nativo preenche a area visivel; se a janela ficar
        // pequena demais (< 1120), o editor mantem um tamanho minimo confortavel
        // (6 colunas + master + controles) e o viewport ganha rolagem — nada some
        // A mesa touch usa paginas de foco. Em janelas com menos de 520 dp de
        // altura, rolagem vertical e intencional para preservar alvos de 48 dp;
        // nunca existe rolagem horizontal no editor nativo.
        if (dockedNative && desktopReferenceLayout)
        {
            // Preserve usable channel and master fader travel when the header,
            // MIDI mapping row and performance dock leave less than 480 px.
            dockViewport.setScrollBarsShown (area.getHeight() < 480, false);
        }
        else if (dockedNative && compact)
        {
           #if CHURCHDAW_MOBILE_SHELL
            if (mobileShortLandscapeLayout)
                dockViewport.setViewPosition (0, 0);

            // A pagina touch tem altura minima para manter cada acao essencial
            // em 48 dp. Em paisagem baixa ela rola verticalmente; em retrato e
            // tablet normalmente cabe inteira. E preferivel uma pagina legivel
            // a achatar PANIC, M/S e seletores em faixas de 16--30 dp.
            const bool needsVerticalPage = ! mobileShortLandscapeLayout
                                        && area.getHeight() < 520;
            dockViewport.setScrollBarsShown (needsVerticalPage, false);
           #else
            // No desktop compacto o mouse/teclado nao exigem o piso touch de
            // 520 dp. O editor ja possui uma grade vertical compacta e deve
            // receber exatamente o viewport para manter o rodape visivel.
            dockViewport.setViewPosition (0, 0);
            dockViewport.setScrollBarsShown (false, false);
           #endif
        }
        else
            dockViewport.setScrollBarsShown (! compact, true);

        // Desktop shell keeps 480 px for the master controls and scrolls when
        // necessary; legacy desktop and mobile retain their existing sizing.
        if (dockedNative)
        {
            // O minimo de 1120x400 e do PC e no celular era exatamente o que
            // cortava a mesa: a tela tem 820x360 logicos, entao o editor nascia
            // MAIOR que a area visivel nos dois eixos e o viewport respondia com
            // barra de rolagem horizontal E vertical -- os controles de canal
            // ficavam abaixo da tela, que foi o defeito visto no aparelho.
            //
            // No compacto a largura vale EXATAMENTE a area disponivel e a altura
            // tem um piso de 520 dp. Isso atende portrait/freeform sem esmagar
            // componentes; em paisagem baixa a pagina rola, mantendo todos os
            // comandos acessiveis e nenhum mixer sob a barra gestual.
           #if CHURCHDAW_MOBILE_SHELL
            dockedEditor->setSize (
                compact ? area.getWidth() : juce::jmax (1120, area.getWidth()),
                compact ? (mobileShortLandscapeLayout
                               ? area.getHeight()
                               : juce::jmax (520, area.getHeight()))
                        : juce::jmax (400, area.getHeight()));
           #else
            {
                const int editorHeight = desktopReferenceLayout
                    ? juce::jmax (480, area.getHeight())
                    : compact ? area.getHeight() : juce::jmax (400, area.getHeight());

                // JUCE resolves scrollbar visibility from the viewed holder.
                // Give it the target height before reading the visible width,
                // so a newly shown vertical bar cannot cover the master strip.
                if (desktopReferenceLayout)
                    dockHolder.setSize (area.getWidth(), editorHeight);

                const int visibleW = juce::jmax (1, dockViewport.getMaximumVisibleWidth());
                dockedEditor->setSize (
                    compact && ! desktopReferenceLayout ? area.getWidth() : visibleW,
                    editorHeight);
            }
           #endif
        }

        layoutDockHolder();
    }

    // Audio/MIDI overlay follows the window and keeps the controls centered.
    if (audioPanel != nullptr)
    {
       #if CHURCHDAW_MOBILE_SHELL
        audioPanel->setBounds (layoutContentBounds.isEmpty() ? getLocalBounds()
                                                             : layoutContentBounds);
       #else
        audioPanel->setBounds (getLocalBounds());
       #endif
        audioPanel->toFront (false);
    }

    if (librariesPanel != nullptr)
    {
       #if CHURCHDAW_MOBILE_SHELL
        librariesPanel->setBounds (layoutContentBounds.isEmpty() ? getLocalBounds()
                                                                 : layoutContentBounds);
       #else
        librariesPanel->setBounds (getLocalBounds());
       #endif
    }

    // card do Pad Continuo: centrado, um pouco abaixo do meio (flutua no mixer)
    if (padPanel != nullptr)
    {
        const bool seguePosicaoAutomatica = padPanelHome.isEmpty()
                                          || padPanel->getBounds() == padPanelHome;
        const int margem = compact ? 8 : 16;
        const int topoDisponivel = layoutContentBounds.getY() + headerH + (compact ? 4 : 12);
        auto disponivel = juce::Rectangle<int> (
            layoutContentBounds.getX() + margem,
            topoDisponivel,
            juce::jmax (1, layoutContentBounds.getWidth() - margem * 2),
            juce::jmax (1, layoutContentBounds.getBottom() - topoDisponivel - margem));
        // O TAMANHO VEM DO PROPRIO PAINEL, nao de dois numeros cravados aqui.
        //
        // 560x318 era o teto herdado do teclado de piano desenhado a mao. Com a
        // grade de 12 tons ele deixava cada celula com ~35 px de altura — abaixo
        // do alvo de toque de 44 px e menos que as teclas antigas entregavam, o
        // que num tablet no palco vira tom errado no meio do louvor. O PadPanel
        // publica o tamanho que a grade pede exatamente para este ponto; se a
        // janela for menor, os jmin continuam cortando e o painel se adapta.
        const int larguraPad = juce::jmin (PadPanel::larguraPreferida, disponivel.getWidth());
        const int alturaPad = juce::jmin (PadPanel::alturaPreferida, disponivel.getHeight());

        // so re-centraliza enquanto o usuario nao arrastou o card (a posicao
        // atual ainda e a ultima que o codigo setou)
        if (seguePosicaoAutomatica)
        {
            auto pb = disponivel.withSizeKeepingCentre (larguraPad, alturaPad);
            padPanel->setBounds (pb);
            padPanelHome = pb;
        }
        else
        {
            // arrastado: respeita a posicao escolhida, so puxa de volta pra
            // dentro da janela se o resize deixou o card pra fora
            auto pb = padPanel->getBounds().withSizeKeepingCentre (larguraPad, alturaPad);
            pb.setX (juce::jlimit (disponivel.getX(),
                                   juce::jmax (disponivel.getX(),
                                               disponivel.getRight() - pb.getWidth()),
                                   pb.getX()));
            pb.setY (juce::jlimit (disponivel.getY(),
                                   juce::jmax (disponivel.getY(),
                                               disponivel.getBottom() - pb.getHeight()),
                                   pb.getY()));
            padPanel->setBounds (pb);
        }
    }

    midiMappingPanel.setVisible (false);
    midiMappingPanel.setBounds ({});

    const int contentTop = juce::jmax (headerRow.getBottom(), programBand.isEmpty() ? layoutContentBounds.getY() : programBand.getBottom());
    const auto overlayBounds = juce::Rectangle<int> (
        0, contentTop, getWidth(), juce::jmax (1, getHeight() - contentTop));
    midiLearnConsoleOverlay.setBounds (overlayBounds);
    if (midiLearnConsoleOverlay.isVisible())
        midiLearnConsoleOverlay.toFront (false);

    if (padEditorPanel != nullptr)
    {
        const auto padWorkspace = juce::Rectangle<int> (
            layoutContentBounds.getX(), programBand.getY(),
            layoutContentBounds.getWidth(),
            juce::jmax (1, layoutContentBounds.getBottom() - programBand.getY()));
        padEditorPanel->setBounds (padWorkspace);
        padEditorPanel->setVisible (padEditorVisible);

        if (padEditorVisible)
            padEditorPanel->toFront (false);
    }

    if (performanceSessionPanel != nullptr)
    {
        performanceSessionPanel->setBounds ({});
        performanceSessionPanel->setVisible (false);
    }
    auto editActions = programBand.reduced (18, 7).withTrimmedLeft (118);
    undoMixButton.setBounds (editActions.removeFromLeft (78)); editActions.removeFromLeft (5);
    redoMixButton.setBounds (editActions.removeFromLeft (74));
    undoMixButton.setVisible (desktopReferenceLayout && ! learning && ! padEditorVisible);
    redoMixButton.setVisible (undoMixButton.isVisible());

    // A workspace must stay below the settings/catalogue cards on resize.
    for (auto* overlay : { audioPanel.get(), aboutPanel.get(), librariesPanel.get() })
        if (overlay != nullptr && overlay->isVisible()) overlay->toFront (false);


    // O aviso do modo rapido precisa ficar acima tanto do mixer quanto da aba
    // de samples, mas ocupa somente a parte direita da subbarra e nao bloqueia
    // a navegacao de workspace. Os botoes de comando permanecem na frente de tudo.
    if (learning)
    {
        if (midiLearnConsoleOverlay.isVisible())
            midiLearnConsoleOverlay.toFront (false);
        midiLearnPanel.toFront (false);
        clearMapButton.toFront (false);
        mapButton.toFront (false);
    }

   #if JUCE_ANDROID
    // O contexto OpenGL desenha somente quando solicitado. Uma mudanca de
    // geometria precisa invalidar a hierarquia inteira antes de pedir o novo
    // quadro; caso contrario, controles estaticos do header podem conservar o
    // frame anterior ate o primeiro toque do usuario.
    repaint();
    glContext.triggerRepaint();
   #endif
}

//==============================================================================
double MainComponent::getCurrentSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 44100.0;
}

int MainComponent::getCurrentBlockSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    return 512;
}

void MainComponent::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
   #if JUCE_ANDROID
    // AAudio recria a thread ao trocar buffer/taxa. A afinidade pertence ao
    // tid antigo e precisa ser aplicada novamente. O ADPF fica sob ownership
    // do proprio Oboe, evitando duas sessoes concorrentes para a mesma thread.
    audioAffinitySet = false;
    churchdaw::androidPerformance::prepareFastCoreMask();
   #endif
   #if JUCE_ANDROID || JUCE_IOS
    androidBufferTunerWarmupPending.store (true,
                                            std::memory_order_release);
   #endif

    idleOutputPeak.store (0.0f, std::memory_order_relaxed);
    idleOutputSquareSum.store (0.0, std::memory_order_relaxed);
    idleOutputSampleCount.store (0, std::memory_order_relaxed);
    idleOutputNonFiniteCount.store (0, std::memory_order_relaxed);
    blockedInputCallbackCount.store (0, std::memory_order_relaxed);
    blockedInputChannelMaximum.store (0, std::memory_order_relaxed);
    studioAudioTap->prepare (device != nullptr ? device->getCurrentSampleRate() : 48000.0);
   #if JUCE_IOS || JUCE_ANDROID
    studioMicrophoneTap->prepare (device != nullptr ? device->getCurrentSampleRate() : 48000.0);
   #endif
    player.audioDeviceAboutToStart (device);
    playbackEngine.prepareToPlay (device != nullptr
                                      ? device->getCurrentBufferSizeSamples() : 512,
                                  device != nullptr
                                      ? device->getCurrentSampleRate() : 44100.0);
    diagnosticTestTone.prepare (device != nullptr ? device->getCurrentSampleRate() : 44100.0);
    finalOutputGuard.prepare (device != nullptr ? device->getCurrentSampleRate() : 44100.0,
                              device != nullptr ? device->getCurrentBufferSizeSamples() : 512,
                              device != nullptr ? device->getActiveOutputChannels().countNumberOfSetBits() : 2);
}

#if JUCE_IOS || JUCE_ANDROID
juce::Result MainComponent::setStudioMicrophoneEnabled (bool enabled, int channel)
{
    // Only the explicit ChurchCam mic action calls this. Camera capture owns
    // no audio session and this input remains absent from the main graph.
    studioMicRequested.store (false, std::memory_order_release);
    const auto previous = deviceManager.getAudioDeviceSetup();
    auto setup = previous;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (enabled)
    {
        if (deviceManager.getCurrentAudioDevice() == nullptr)
            return juce::Result::fail ("Ative o audio da DAW primeiro.");
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            const auto names = type->getDeviceNames (true);
            const auto defaultIndex = type->getDefaultDeviceIndex (true);
            if (names.contains (setup.outputDeviceName)) setup.inputDeviceName = setup.outputDeviceName;
            else if (juce::isPositiveAndBelow (defaultIndex, names.size())) setup.inputDeviceName = names[defaultIndex];
            else if (! names.isEmpty()) setup.inputDeviceName = names[0];
            else return juce::Result::fail ("Nenhuma entrada de microfone disponivel.");
        }
        setup.inputChannels.setBit (juce::jlimit (0, 1, channel));
    }
    auto error = deviceManager.setAudioDeviceSetup (setup, false);
    auto* device = deviceManager.getCurrentAudioDevice();
    if (enabled && error.isEmpty() && (device == nullptr || ! device->getActiveInputChannels()[juce::jlimit (0, 1, channel)]))
        error = "Essa entrada de microfone nao esta disponivel. Selecione Entrada 1 ou conecte sua interface.";
    if (error.isNotEmpty())
    {
        if (enabled)deviceManager.setAudioDeviceSetup (previous, false);
        return juce::Result::fail (error);
    }
    studioMicRequested.store (enabled, std::memory_order_release);
    return juce::Result::ok();
}
void MainComponent::suspendChurchCam()
{
    if (auto* window = dynamic_cast<churchstudio::Window*> (liveCamWindow.getComponent()))window->suspendCapture();
}
void MainComponent::resumeChurchCam()
{
    if (auto* window = dynamic_cast<churchstudio::Window*> (liveCamWindow.getComponent()))window->resumeCapture();
}
void MainComponent::ensureMobileAudioRunningAfterResume()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr || ! device->isPlaying())
    {
        juce::Logger::writeToLog ("AUDIO_MOBILE: reabrindo dispositivo apos interrupcao do sistema");
        deviceManager.restartLastAudioDevice();
    }
}
#endif

void MainComponent::handleMemoryWarning()
{
    if (auto* live = getChurchLive())
        live->audition.stop (true);
}

void MainComponent::audioDeviceStopped()
{
    playbackEngine.releaseResources();
    player.audioDeviceStopped();
    diagnosticTestTone.reset();
    finalOutputGuard.release();
}

void MainComponent::audioDeviceError (const juce::String& message)
{
    juce::Logger::writeToLog ("AUDIO_DRIVER error=" + message);
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this),
         lifetime = pluginLoadLifetime, message]
        {
            if (! lifetime->load()) return;
            if (auto* self = safe.getComponent())
            {
                self->lastAudioDriverError = message;
                if (auto* live = self->getChurchLive()) live->audition.stop (true);
                self->statusInfo.setTooltip (message + " / Abra Audio para reabrir a interface.");
            }
        });
}

#if JUCE_ANDROID
void MainComponent::prepareAndroidAudioThread() noexcept
{
    if (! audioAffinitySet)
    {
        audioAffinitySet = true;
        if (churchdaw::androidPerformance::pinCurrentThreadToFastCores())
            juce::Logger::outputDebugString (
                "AUDIO: callback fixado nos nucleos rapidos");
    }
}
#endif

void MainComponent::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context)
{
   #if JUCE_ANDROID
    prepareAndroidAudioThread();
   #endif

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    const auto callbackStartTicks = juce::Time::getHighResolutionTicks();
   #endif

   #if JUCE_IOS || JUCE_ANDROID
    // The opted-in microphone feeds the movie only. It never enters the
    // instrument graph or loudspeakers, even when the route changes.
    const bool movieInputAuthorised = studioMicRequested.load (std::memory_order_acquire);
    if (movieInputAuthorised)
        studioMicrophoneTap->push (inputChannelData, numInputChannels, numSamples);
    const bool inputAllowed = false;
   #else
    const bool inputAllowed =
        audioInputAuthorised.load (std::memory_order_acquire);
   #endif

    if (! inputAllowed && numInputChannels > 0
       #if JUCE_IOS || JUCE_ANDROID
        && ! movieInputAuthorised
       #endif
    )
    {
        blockedInputCallbackCount.fetch_add (1, std::memory_order_relaxed);

        int observed = blockedInputChannelMaximum.load (
            std::memory_order_relaxed);
        while (numInputChannels > observed
                && ! blockedInputChannelMaximum.compare_exchange_weak (
                    observed, numInputChannels,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
        {
        }
    }

    // Mesmo que um driver/hotplug entregue canais antes da correcao do setup,
    // o grafo recebe zero ponteiros e zero canais enquanto nao houver permissao.
    player.audioDeviceIOCallbackWithContext (
        inputAllowed ? inputChannelData : nullptr,
        inputAllowed ? numInputChannels : 0,
        outputChannelData, numOutputChannels, numSamples, context);

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    const auto diagAfterPlayer = juce::Time::getHighResolutionTicks();
   #endif

    playbackEngine.getNextAudioBlock (outputChannelData, numOutputChannels,
                                      numSamples);

    diagnosticTestTone.processAdd (outputChannelData, numOutputChannels, numSamples);

    // Preserve independent playback transport; protect the complete mix before
    // recording and physical output. The established lookahead adds about 3 ms.
    finalOutputGuard.process (outputChannelData, numOutputChannels, numSamples);

    studioAudioTap->push (outputChannelData, numOutputChannels, numSamples);

   #if JUCE_WINDOWS || JUCE_MAC
    // "Tem som saindo": alimenta o sinal que impede o encerramento por licenca
    // no meio de um culto. Um peak por bloco, sem alocacao.
    ChurchDAWAudioActivity::noteBlock (outputChannelData, numOutputChannels,
                                       numSamples);
   #endif

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    const auto callbackEndTicks = juce::Time::getHighResolutionTicks();
   #endif

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    diagPlayerTicks.fetch_add (
        diagAfterPlayer - callbackStartTicks, std::memory_order_relaxed);
    diagPlaybackTicks.fetch_add (
        callbackEndTicks - diagAfterPlayer, std::memory_order_relaxed);
    diagCallbackSamples.fetch_add (
        (juce::uint64) juce::jmax (0, numSamples),
        std::memory_order_relaxed);
   #endif

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    // Sonda de laboratorio. Ela percorre a saida inteira e acumula RMS para
    // provar silencio/NaN durante QA, mas nao participa de nenhuma decisao do
    // motor. Mantê-la no binario comercial gastava CPU em todo callback — em
    // especial enquanto um Pad Continuo tocava sem MIDI — sem beneficio para
    // o cliente.
    const auto now = juce::Time::getMillisecondCounter();
    if (inputAllowed || now - lastMidiActivityMs.load (
                            std::memory_order_relaxed) < 5000)
        return;

    float blockPeak = 0.0f;
    double blockSquareSum = 0.0;
    juce::uint64 finiteSamples = 0;
    juce::uint64 nonFiniteSamples = 0;

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (const auto* samples = outputChannelData[channel])
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const float value = samples[sample];
                if (! std::isfinite (value))
                {
                    ++nonFiniteSamples;
                    continue;
                }

                blockPeak = juce::jmax (blockPeak, std::abs (value));
                blockSquareSum += (double) value * (double) value;
                ++finiteSamples;
            }

    float observedPeak = idleOutputPeak.load (std::memory_order_relaxed);
    while (blockPeak > observedPeak
            && ! idleOutputPeak.compare_exchange_weak (
                observedPeak, blockPeak,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
    {
    }

    double observedSquares = idleOutputSquareSum.load (
        std::memory_order_relaxed);
    while (! idleOutputSquareSum.compare_exchange_weak (
        observedSquares, observedSquares + blockSquareSum,
        std::memory_order_relaxed,
        std::memory_order_relaxed))
    {
    }

    idleOutputSampleCount.fetch_add (finiteSamples,
                                     std::memory_order_relaxed);
    idleOutputNonFiniteCount.fetch_add (nonFiniteSamples,
                                        std::memory_order_relaxed);
   #endif
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &knownPlugins)
    {
        if (auto xml = knownPlugins.createXml())
            appProperties.getUserSettings()->setValue ("pluginList", xml.get());

        refreshSlotChoices();
    }
    else if (source == &deviceManager)
    {
        const bool inputPolicyOk = reconcileAudioInputPolicy();

       #if JUCE_ANDROID
        // Mudanca feita dentro do painel e escolha explicita do usuario. Um
        // fallback automatico acontece com o painel fechado e nao apaga o USB
        // que deve ser restaurado quando voltar.
        if (audioDialogOpen)
            updateAndroidPreferredUsbSetupFromCurrent();
       #endif

       #if ! JUCE_ANDROID && ! JUCE_IOS
        // Diagnostico de travamento ao trocar de driver no desktop (ASIO4ALL em
        // especial): snapshot logo apos a troca. Se o processo morrer DURANTE a
        // abertura, esta linha nao chega a ser escrita -- nesse caso a ultima
        // linha do log e o snapshot PRE-selecao do timer do painel (AUDIO_DIALOG),
        // e so entre as duas se fecha o ponto exato da queda. Leitura only, nao
        // muda nada no comportamento.
        {
            const auto setup = deviceManager.getAudioDeviceSetup();
            const auto* type = deviceManager.getCurrentDeviceTypeObject();
            const auto* dev = deviceManager.getCurrentAudioDevice();
            juce::Logger::writeToLog (
                "AUDIO_CHANGE type=\""
                + (type != nullptr ? type->getTypeName() : juce::String())
                + "\" output=\"" + setup.outputDeviceName
                + "\" input=\"" + setup.inputDeviceName
                + "\" inChannels=" + juce::String (setup.inputChannels.countNumberOfSetBits())
                + " inputAuthorised="
                + juce::String (audioInputAuthorised.load (
                                    std::memory_order_acquire) ? 1 : 0)
                + " inputPolicyOk=" + juce::String (inputPolicyOk ? 1 : 0)
                + " buffer=" + juce::String (setup.bufferSize)
                + " rate=" + juce::String (setup.sampleRate, 0)
                + (dev != nullptr ? juce::String (" open=yes \"") + dev->getName() + "\""
                                   : juce::String (" open=NO")));
        }
       #endif

       #if JUCE_WINDOWS || JUCE_MAC
        // Com o painel de Audio aberto a escolha e do musico; fechado, uma saida
        // que aparece (fone plugado, interface USB ligada) e usada na hora.
        if (deviceManager.getCurrentAudioDevice() == nullptr && ! audioDialogOpen)
            reconnectAudioOutputIfAvailable();
       #endif

        // so grava enquanto o dialogo de audio esta aberto: um fallback de driver
        // na inicializacao nao pode sobrescrever a escolha do usuario
        if (! suppressAudioStateSave && audioDialogOpen)
            saveAudioStateNow();

        rebuildGraphConnections();
        return;
    }
    else if (source == masterProcessor)
    {
        masterSlider.setValue (masterProcessor->getMasterDb(), juce::dontSendNotification);
    }
    else if (source == getChurchLive())
    {
        // A single insert swap advances the global topology generation. Refresh
        // every fixed cache in the same message-thread turn so unrelated active
        // insert mappings do not remain conservatively pending until the 1 s
        // maintenance timer.
        for (int slot = 0; slot < numSlots; ++slot)
            refreshInsertMappingIdentityCacheForSlot (slot);

        // O setup do Church Live so ia para o disco no botao Salvar ou no
        // destrutor. Se o app morre antes disso — e ele morria varrendo
        // plugins — o cliente reabre SEM o setup dele, e a primeira
        // execucao roda de novo o preset de fabrica: piano no canal 1 e
        // pad no canal 2 que ele nunca escolheu. Foi a reclamacao do Mac
        // em 31/07/2026.
        //
        // Grava com atraso porque este callback tambem chega quando o
        // musico so mexe num fader; escrever XML a cada toque durante o
        // culto seria pior que o problema.
        agendarGravacaoDoSetup();
    }

    appProperties.getUserSettings()->saveIfNeeded();
}

void MainComponent::agendarGravacaoDoSetup()
{
    // setStateInformation() e os callbacks assíncronos dos plugins disparam o
    // mesmo ChangeBroadcaster usado por um fader manual. Durante uma troca de
    // cena, salvar aqui gravaria uma pagina pela metade como setup global.
    const auto token = controleDeGravacaoDoSetup.agendarSePermitido (
        geracaoDaCena);

    if (token == 0)
        return;

    // Uma geracao por pedido: se chegarem dez mudancas seguidas, so a
    // ultima grava. Component::SafePointer cobre o app fechando no meio.
    juce::Timer::callAfterDelay (
        2000,
        [safe = juce::Component::SafePointer<MainComponent> (this), token]
        {
            if (auto* self = safe.getComponent())
            {
                if (! self->controleDeGravacaoDoSetup.timerAindaValido (
                        token, self->geracaoDaCena))
                    return;

                const auto nowMs = juce::Time::getMillisecondCounter();
                const bool midiActive =
                    nowMs - self->lastMidiActivityMs.load (std::memory_order_relaxed) < 2500;
                if (midiActive || ChurchDAWAudioActivity::audibleWithin (1200))
                {
                    self->agendarGravacaoDoSetup();
                    return;
                }

                self->saveChainState();
            }
        });
}

void MainComponent::agendarFimDaSupressaoDeGravacaoDaCena (int geracao)
{
    // ChangeBroadcaster entrega notificacoes de forma assíncrona. Soltar no
    // proximo giro deixa passar primeiro as que ja estavam na fila, sem engolir
    // o primeiro ajuste manual feito depois de a cena aparecer pronta.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this), geracao]
        {
            if (auto* self = safe.getComponent())
                (void) self->controleDeGravacaoDoSetup.encerrarSupressao (
                    geracao, self->geracaoDaCena);
        });
}

void MainComponent::capturarInicioDaTransicao (TimbresProcessor& live,
                                                int geracao)
{
    EstadoContinuoAntesDaCena inicio;
    inicio.valido = true;
    inicio.geracao = geracao;

    for (int layer = 0; layer < ChurchDAWLive::numeroDeCanais; ++layer)
    {
        if (auto* p = live.getLayerVolParam (layer))
            inicio.volumes[(size_t) layer] = p->getValue();
        if (auto* p = live.getLayerReverbSendParam (layer))
            inicio.reverbs[(size_t) layer] = p->get();
    }

    if (auto* p = live.getVolumeGeralParam())
        inicio.volumeGeral = p->get();
    if (auto* p = live.getRetornoDeReverbParam())
        inicio.retornoReverb = p->get();
    if (masterProcessor != nullptr)
        inicio.masterDb = masterProcessor->getMasterDb();
    if (padProcessor != nullptr)
    {
        inicio.padVolume = padProcessor->getGain();
        inicio.padCorte = padProcessor->getCutoff();
    }

    estadoAntesDaTransicao = inicio;
}

void MainComponent::aplicarFrameDaTransicao (
    const EstadoContinuoAntesDaCena& inicio,
    const ChurchDAWLive::Cena& destino,
    float progresso)
{
    auto* live = getChurchLive();
    if (live == nullptr)
        return;

    const float t = juce::jlimit (0.0f, 1.0f, progresso);
    const float eased = t * t * (3.0f - 2.0f * t);
    const auto mix = [eased] (float from, float to)
    {
        return from + (to - from) * eased;
    };

    for (int layer = 0; layer < ChurchDAWLive::numeroDeCanais; ++layer)
    {
        if (auto* p = live->getLayerVolParam (layer))
            p->setValueNotifyingHost (juce::jlimit (
                0.0f, 1.0f, mix (inicio.volumes[(size_t) layer],
                                 destino.canais[layer].volume)));
        if (auto* p = live->getLayerReverbSendParam (layer))
            *p = mix (inicio.reverbs[(size_t) layer],
                      destino.canais[layer].envioDeReverb);
    }

    if (destino.temGlobais)
    {
        if (auto* p = live->getVolumeGeralParam())
            *p = mix (inicio.volumeGeral, destino.volumeGeral);
        if (auto* p = live->getRetornoDeReverbParam())
            *p = mix (inicio.retornoReverb, destino.retornoDeReverb);
    }

    if (padProcessor != nullptr)
    {
        padProcessor->setGain (mix (inicio.padVolume, destino.padVolume));
        padProcessor->setCutoff (mix (inicio.padCorte, destino.padCorte));
    }

    if (destino.temMaster && masterProcessor != nullptr)
    {
        const float db = mix (inicio.masterDb, destino.masterDb);
        masterProcessor->setMasterDb (db);
        masterSlider.setValue (db, juce::dontSendNotification);
    }
}

bool MainComponent::iniciarTransicaoDaCena (
    const ChurchDAWLive::Cena& cena, int geracao)
{
    const auto inicio = estadoAntesDaTransicao;
    const double duration = juce::jlimit (0.0, 15.0,
                                         cena.transicaoSegundos);
    if (! inicio.valido || inicio.geracao != geracao || duration < 0.04)
        return false;

    // O restore da pagina pode ter escrito os valores de destino. Recoloca o
    // ponto de partida no mesmo giro da message thread, antes de o proximo
    // bloco de audio observar a mudanca, e entao percorre a curva escolhida.
    aplicarFrameDaTransicao (inicio, cena, 0.0f);

    auto transition = std::make_shared<TransicaoEmCurso>();
    transition->inicio = inicio;
    transition->destino = cena;
    transition->geracao = geracao;
    transition->inicioMs = juce::Time::getMillisecondCounterHiRes();
    transition->duracaoMs = duration * 1000.0;
    continuarTransicaoDaCena (transition);
    return true;
}

void MainComponent::continuarTransicaoDaCena (
    const std::shared_ptr<TransicaoEmCurso>& transicao)
{
    if (transicao == nullptr || geracaoDaCena != transicao->geracao)
        return;

    const auto elapsed = juce::Time::getMillisecondCounterHiRes()
                       - transicao->inicioMs;
    const float progress = (float) juce::jlimit (
        0.0, 1.0, elapsed / transicao->duracaoMs);
    aplicarFrameDaTransicao (transicao->inicio, transicao->destino, progress);

    if (progress >= 1.0f)
    {
        ccPickup.clear();
        if (padPanel != nullptr)
            padPanel->refreshState();
        refreshPerformanceSessionPanel();
        agendarFimDaSupressaoDeGravacaoDaCena (transicao->geracao);
        return;
    }

    juce::Timer::callAfterDelay (
        16,
        [safe = juce::Component::SafePointer<MainComponent> (this), transicao]
        {
            if (auto* self = safe.getComponent())
                self->continuarTransicaoDaCena (transicao);
        });
}

void MainComponent::refreshSlotChoices()
{
    const auto types = knownPlugins.getTypes();
    for (int i = 0; i < numSlots; ++i)
    {
        auto& s = slots[i];

        s.choices.clear();
        s.box.clear (juce::dontSendNotification);
        s.box.addItem ("(vazio)", 1);

        // uma entrada nativa unica: o Church Live enxerga TODAS as bibliotecas
        // (a escolha de biblioteca e por canal, dentro do proprio instrumento)
        if (i == 0)
            s.box.addItem ("Church Live (nativo)", timbresComboId);

        const bool wantInstrument = (i == 0);

        for (const auto& d : types)
            if (ChurchDAWPluginScan::descricaoDeclaraInstrumento (d)
                    == wantInstrument
                && ! ChurchDAWPluginScan::estaNaBlacklist (
                        knownPlugins, d.fileOrIdentifier, true)
                && ! pluginScanQuarantine.contains (d.fileOrIdentifier, true))
                s.choices.add (d);

        for (int c = 0; c < s.choices.size(); ++c)
        {
            const auto& choice = s.choices.getReference (c);
            juce::String label = choice.name;

            if (choice.pluginFormatName.isNotEmpty())
                label += " \xC2\xB7 " + choice.pluginFormatName;

            if (choice.manufacturerName.isNotEmpty())
                label += " \xC2\xB7 " + choice.manufacturerName;

            s.box.addItem (label, c + 2);
        }

        // qualquer instrumento interno (inclusive setups antigos) usa a entrada unica
        const auto identifierToShow = s.loadingIdentifier.isNotEmpty()
                                      ? s.loadingIdentifier
                                      : s.pluginIdentifier;
        bool matchedInternal = identifierToShow.startsWith ("internal:timbres");

        if (matchedInternal)
            s.box.setSelectedId (timbresComboId, juce::dontSendNotification);
        else if (identifierToShow.isNotEmpty())
            for (int c = 0; c < s.choices.size(); ++c)
                if (s.choices.getReference (c).createIdentifierString() == identifierToShow)
                    s.box.setSelectedId (c + 2, juce::dontSendNotification);
    }
}

void MainComponent::slotSelectionChanged (int slotIndex)
{
    auto& s = slots[slotIndex];
    const int id = s.box.getSelectedId();

    if (id <= 1)
    {
        unloadSlot (slotIndex);
        s.pluginIdentifier.clear();
        s.pendingIdentifier.clear();   // "(vazio)" explicito ganha da auto-cura
        s.pendingState.clear();
        saveChainState();
        return;
    }

    if (id == timbresComboId)
    {
        if (s.nodeID.uid != 0 && s.pluginIdentifier.startsWith ("internal:timbres"))
        {
            ++s.loadGeneration;
            s.loadingIdentifier.clear();
            return;   // ja carregado
        }

        loadInternalTimbres (slotIndex, nullptr, 0);
        saveChainState();

        if (s.nodeID.uid != 0)
            openEditorForSlot (slotIndex);

        return;
    }

    const auto& desc = s.choices.getReference (id - 2);

    if (s.nodeID.uid != 0 && desc.createIdentifierString() == s.pluginIdentifier)
    {
        ++s.loadGeneration;
        s.loadingIdentifier.clear();
        return;   // ja carregado
    }

    // rescan na mesma sessao: se o escolhido e o proprio plugin pendente,
    // restaura o estado preservado em vez de carregar com defaults
    juce::MemoryBlock pendingBlock;
    const juce::MemoryBlock* stateToUse = nullptr;

    if (s.pendingState.isNotEmpty()
        && desc.createIdentifierString() == s.pendingIdentifier
        && pendingBlock.fromBase64Encoding (s.pendingState))
        stateToUse = &pendingBlock;

    // AUv3 exige criacao assincrona. A troca so e confirmada quando a nova
    // instancia existe; ate la o plugin anterior continua tocando.
    loadPlugin (slotIndex, desc, stateToUse, true, true);
}

void MainComponent::loadPlugin (int slotIndex,
                                const juce::PluginDescription& desc,
                                const juce::MemoryBlock* state,
                                bool openEditorAfterLoad,
                                bool saveAfterLoad)
{
    auto& s = slots[slotIndex];
    if (s.pendingNodeRetirement.hasPending())
    {
        juce::Logger::writeToLog (
            "PLUGIN LOAD: slot aguardando retirada segura do no anterior");
        return;
    }
    const auto requestedIdentifier = desc.createIdentifierString();
    const auto requestedMappingIdentity =
        ChurchDAWMidiMapping::hostedPluginIdentityForDescription (desc);

    if (ChurchDAWPluginScan::estaNaBlacklist (
            knownPlugins, desc.fileOrIdentifier, true)
         || pluginScanQuarantine.contains (desc.fileOrIdentifier, true))
    {
        juce::Logger::writeToLog (
            "PLUGIN LOAD: pedido bloqueado por quarentena "
            + desc.fileOrIdentifier);
        return;
    }

    const auto generation = ++s.loadGeneration;
    const bool hadProcessorAtRequest = s.nodeID.uid != 0;
    s.loadingIdentifier = requestedIdentifier;

    juce::MemoryBlock stateCopy;
    const bool hasState = state != nullptr && state->getSize() > 0;

    if (hasState)
        stateCopy = *state;

    // Sem plugin ativo, o setup salvo continua reemitindo esta solicitacao
    // mesmo se o aplicativo for fechado enquanto o AUv3 ainda esta abrindo.
    // Uma pendencia anterior COM estado so e substituida depois do sucesso.
    if (! hadProcessorAtRequest
        && (hasState || s.pendingIdentifier == requestedIdentifier
                     || s.pendingState.isEmpty()))
    {
        s.pendingIdentifier = requestedIdentifier;

        if (hasState)
            s.pendingState = stateCopy.toBase64Encoding();

        s.pluginIdentifier.clear();
    }

    const auto lifetime = pluginLoadLifetime;
    const auto safe = juce::Component::SafePointer<MainComponent> (this);

    formatManager.createPluginInstanceAsync (
        desc, getCurrentSampleRate(), getCurrentBlockSize(),
        [safe, lifetime, slotIndex, desc, requestedIdentifier,
         requestedMappingIdentity, generation,
         stateCopy = std::move (stateCopy), hasState,
         openEditorAfterLoad, saveAfterLoad]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) mutable
        {
            if (! lifetime->load())
                return;

            auto* self = safe.getComponent();
            if (self == nullptr || slotIndex < 0 || slotIndex >= numSlots)
                return;

            auto& slot = self->slots[slotIndex];
            if (slot.loadGeneration != generation)
                return;

            const auto restoreCurrentSelection = [&slot]
            {
                if (slot.pluginIdentifier.startsWith ("internal:timbres"))
                {
                    slot.box.setSelectedId (timbresComboId, juce::dontSendNotification);
                    return;
                }

                for (int c = 0; c < slot.choices.size(); ++c)
                    if (slot.choices.getReference (c).createIdentifierString()
                        == slot.pluginIdentifier)
                    {
                        slot.box.setSelectedId (c + 2, juce::dontSendNotification);
                        return;
                    }

                // 0 e intencional: escolher "(vazio)" ainda dispara onChange.
                slot.box.setSelectedId (0, juce::dontSendNotification);
            };

            const auto pluginWasBlocked = [self, &desc]
            {
                return ChurchDAWPluginScan::estaNaBlacklist (
                           self->knownPlugins,
                           desc.fileOrIdentifier, true)
                    || self->pluginScanQuarantine.contains (
                           desc.fileOrIdentifier, true);
            };

            const auto rejectBlockedCandidate = [&]
            {
                slot.loadingIdentifier.clear();
                restoreCurrentSelection();
                juce::Logger::writeToLog (
                    "PLUGIN LOAD: candidato entrou em quarentena durante carga "
                    + desc.fileOrIdentifier);
            };

            if (pluginWasBlocked())
            {
                rejectBlockedCandidate();
                return;
            }

            if (instance == nullptr)
            {
                slot.loadingIdentifier.clear();

                if (slot.nodeID.uid == 0)
                {
                    slot.pluginIdentifier.clear();

                    if (hasState)
                    {
                        slot.pendingIdentifier = requestedIdentifier;
                        slot.pendingState = stateCopy.toBase64Encoding();
                    }
                    else if (slot.pendingIdentifier == requestedIdentifier
                              || slot.pendingState.isEmpty())
                    {
                        slot.pendingIdentifier = requestedIdentifier;
                    }
                }

                restoreCurrentSelection();
                juce::Logger::writeToLog ("PLUGIN LOAD: " + desc.name + ": " + error);

                if (openEditorAfterLoad || saveAfterLoad)
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Erro ao carregar plugin",
                        desc.name + ":\n" + error);

                if (saveAfterLoad)
                    self->saveChainState();

                return;
            }

            if (hasState)
            {
                try
                {
                    instance->setStateInformation (stateCopy.getData(), (int) stateCopy.getSize());
                }
                catch (...)
                {
                    if (! lifetime->load() || safe.getComponent() == nullptr)
                        return;

                    if (slot.loadGeneration != generation)
                        return;

                    slot.loadingIdentifier.clear();
                    restoreCurrentSelection();
                    const auto stateError = desc.name
                        + ": o plugin rejeitou o estado salvo; o plugin anterior foi preservado.";
                    juce::Logger::writeToLog ("PLUGIN LOAD: " + stateError);

                    if (openEditorAfterLoad || saveAfterLoad)
                        juce::AlertWindow::showMessageBoxAsync (
                            juce::MessageBoxIconType::WarningIcon,
                            "Erro ao restaurar plugin",
                            stateError);

                    if (saveAfterLoad)
                        self->saveChainState();

                    return;
                }
            }

            if (! lifetime->load()
                || safe.getComponent() == nullptr
                || slot.loadGeneration != generation)
                return;

            if (slot.pendingNodeRetirement.hasPending())
            {
                slot.loadingIdentifier.clear();
                restoreCurrentSelection();
                return;
            }

            const auto inputChannels = instance->getTotalNumInputChannels();
            const auto outputChannels = instance->getTotalNumOutputChannels();
            const ChurchDAWPluginScan::RuntimePluginRoleEvidence
                roleEvidence {
                    instance->acceptsMidi(), inputChannels, outputChannels
                };
            const bool runtimeIsInstrument =
                ChurchDAWPluginScan::deveSerTratadoComoInstrumento (
                    desc, roleEvidence);
            juce::String incompatibleReason;

            if (runtimeIsInstrument != (slotIndex == 0))
            {
                incompatibleReason = slotIndex == 0
                    ? "o plugin foi identificado como efeito, nao instrumento"
                    : "o plugin foi identificado como instrumento, nao efeito";
            }
            else if (slotIndex == 0)
            {
                if (outputChannels <= 0)
                    incompatibleReason = "o instrumento nao oferece saida de audio";
                else if (! roleEvidence.acceptsMidi)
                    incompatibleReason = "o instrumento nao aceita MIDI";
            }
            else if (inputChannels <= 0 || outputChannels <= 0)
            {
                incompatibleReason = "o efeito precisa oferecer entrada e saida de audio";
            }

            // Os getters acima sao codigo virtual de terceiros. Um AU pode
            // bombear a message thread; nunca publica o resultado de um pedido
            // que ficou obsoleto enquanto consultavamos sua configuracao.
            if (! lifetime->load() || safe.getComponent() == nullptr
                || slot.loadGeneration != generation)
                return;

            if (incompatibleReason.isNotEmpty())
            {
                slot.loadingIdentifier.clear();
                restoreCurrentSelection();
                const auto routingError = desc.name + ": " + incompatibleReason
                    + "; o conteudo anterior do slot foi preservado.";
                juce::Logger::writeToLog ("PLUGIN LOAD: " + routingError);

                if (openEditorAfterLoad || saveAfterLoad)
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Plugin incompativel com o slot",
                        routingError);

                if (saveAfterLoad)
                    self->saveChainState();

                return;
            }

            if (pluginWasBlocked())
            {
                rejectBlockedCandidate();
                return;
            }

            // Primeiro prova que o novo processador entra no grafo; somente
            // depois remove o anterior. Assim uma falha nao silencia o culto.
            auto newNode = self->graph.addNode (std::move (instance));

            if (! lifetime->load() || safe.getComponent() == nullptr)
                return;

            if (slot.loadGeneration != generation)
            {
                if (newNode != nullptr)
                {
                    self->graph.removeNode (newNode->nodeID);
                    self->rebuildGraphConnections();
                }

                return;
            }

            if (newNode == nullptr)
            {
                slot.loadingIdentifier.clear();
                restoreCurrentSelection();
                const auto graphError = desc.name
                    + ": nao foi possivel inserir o plugin no grafo de audio.";
                juce::Logger::writeToLog ("PLUGIN LOAD: " + graphError);

                if (openEditorAfterLoad || saveAfterLoad)
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Erro ao carregar plugin",
                        graphError);

                if (saveAfterLoad)
                    self->saveChainState();

                return;
            }

            const auto oldNodeID = slot.nodeID;

            // Prova o candidato usando uma visao local do slot. O estado
            // publicado continua apontando para o plugin antigo enquanto o
            // grafo transacional valida todas as bordas.
            if (! self->rebuildGraphConnections (slotIndex, newNode->nodeID))
            {
                if (! lifetime->load() || safe.getComponent() == nullptr)
                    return;

                if (slot.loadGeneration != generation)
                {
                    self->rebuildGraphConnections();
                    self->graph.removeNode (newNode->nodeID);
                    return;
                }

                const bool previousRouteOk = self->rebuildGraphConnections();
                self->graph.removeNode (newNode->nodeID);

                if (! lifetime->load() || safe.getComponent() == nullptr
                    || slot.loadGeneration != generation)
                    return;

                slot.loadingIdentifier.clear();
                restoreCurrentSelection();
                const auto graphError = desc.name
                    + ": a configuracao de canais nao pode ser ligada ao mixer; "
                    + (previousRouteOk
                        ? "o plugin anterior foi preservado."
                        : "o plugin anterior continua selecionado, mas a rota de audio "
                          "tambem precisa ser revisada.");
                juce::Logger::writeToLog (
                    "PLUGIN LOAD: " + graphError
                    + " previous_route_ok=" + juce::String (previousRouteOk ? 1 : 0));

                if (openEditorAfterLoad || saveAfterLoad)
                    juce::AlertWindow::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Erro ao rotear plugin",
                        graphError);

                if (saveAfterLoad)
                    self->saveChainState();

                return;
            }

            if (! lifetime->load() || safe.getComponent() == nullptr)
                return;

            if (slot.loadGeneration != generation)
            {
                self->rebuildGraphConnections();
                self->graph.removeNode (newNode->nodeID);
                return;
            }

            if (pluginWasBlocked())
            {
                self->rebuildGraphConnections();
                self->graph.removeNode (newNode->nodeID);
                rejectBlockedCandidate();
                return;
            }

            // Reserve the sole physical retirement slot before publishing the
            // replacement. If the old Church Live is suspended in a nested
            // plug-in frame, a second replacement fails here without changing
            // the current node or growing a chain of deferred graph nodes.
            if (oldNodeID.uid != 0)
                if (auto oldNode = self->graph.getNodeForId (oldNodeID))
                    if (auto* oldLive = dynamic_cast<TimbresProcessor*> (
                            oldNode->getProcessor());
                        oldLive != nullptr
                         && ! oldLive->canDestroyWithoutBlockingOnMidiMapping()
                         && ! slot.pendingNodeRetirement.tryReserve (
                                oldNodeID.uid))
                    {
                        self->rebuildGraphConnections();
                        self->graph.removeNode (newNode->nodeID);
                        slot.loadingIdentifier.clear();
                        restoreCurrentSelection();
                        return;
                    }

            slot.nodeID = newNode->nodeID;
            slot.pluginIdentifier = requestedIdentifier;
            {
                const juce::ScopedLock mappingLock (self->mapLock);
                slot.mappingIdentity = requestedMappingIdentity;
                slot.proc.store (newNode->getProcessor());
                self->parameterTargetGeneration.fetch_add (
                    1, std::memory_order_acq_rel);
            }
            slot.pendingIdentifier.clear();
            slot.pendingState.clear();
            slot.loadingIdentifier.clear();
            slot.openButton.setEnabled (true);
            newNode->getProcessor()->addListener (self);
            for (int cacheSlot = 0; cacheSlot < numSlots; ++cacheSlot)
                self->refreshInsertMappingIdentityCacheForSlot (cacheSlot);

            for (int c = 0; c < slot.choices.size(); ++c)
                if (slot.choices.getReference (c).createIdentifierString()
                    == slot.pluginIdentifier)
                    slot.box.setSelectedId (c + 2, juce::dontSendNotification);

            // Handshake de vida (ver liveHandshakeLock no topo do arquivo): o
            // no antigo era o TimbresProcessor que a thread de MIDI pode estar
            // desreferenciando em handleIncomingMidiMessage. So destroi depois
            // de adquirir o lock.
            if (oldNodeID.uid != 0)
            {
                const juce::ScopedLock sl (liveHandshakeLock);
                self->removeNodeForSlot (slotIndex, oldNodeID);
            }

            // Destrutores de plugins de terceiros podem bombear a message
            // thread. Se houve outra escolha durante o fechamento do antigo,
            // a nova geracao ja assumiu o slot e nao deve ser sobrescrita.
            if (! lifetime->load() || safe.getComponent() == nullptr
                || slot.loadGeneration != generation)
                return;

            if (saveAfterLoad)
                self->saveChainState();

            if (openEditorAfterLoad)
                self->openEditorForSlot (slotIndex);
        });
}

void MainComponent::loadInternalTimbres (int slotIndex, const juce::MemoryBlock* state, int libraryIndex)
{
    if (slots[slotIndex].pendingNodeRetirement.hasPending())
        return;

    unloadSlot (slotIndex);
    const auto loadGeneration = slots[slotIndex].loadGeneration;

    auto instance = std::make_unique<TimbresProcessor>();
    instance->setMidiMappingTopologyGenerationCounter (
        &parameterTargetGeneration);
    instance->setMidiMappingParameterCallbackGate (
        &parameterCallbackGate);
    auto* const internalTimbres = instance.get();
    instance->setInsertParameterListenerWillRetireCallback (
        [safe = juce::Component::SafePointer<MainComponent> (this),
         internalTimbres] (int insert,
                           juce::AudioPluginInstance* processor)
        {
            if (auto* self = safe.getComponent())
                self->retireInsertParameterListener (
                    internalTimbres, insert, processor);
        });
    instance->setExternalMaster (masterProcessor);                    // master grandao dentro do Church Live
    instance->setPluginResources (&knownPlugins, &formatManager,
                                  &pluginScanQuarantine);     // plugins hospedados no canal master
    instance->setGlobalPanicRequestCallback (
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safe.getComponent())
                if (! self->componentShutdownStarted.load (
                        std::memory_order_acquire))
                    self->performGlobalPanic();
        });
    instance->setOpenPadEditorCallback (
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        (int layer, const juce::String& name)
        {
            if (auto* self = safe.getComponent())
                self->showPadEditor (layer, name);
        });

    if (libraryIndex == 1)
        instance->setLibraryRoot (TimbresProcessor::redkeysSamplesFolder());
    else if (libraryIndex == 2)
        instance->setLibraryRoot (TimbresProcessor::worshipSamplesFolder());
    else if (libraryIndex == 3)
        instance->setLibraryRoot (TimbresProcessor::baselabSamplesFolder());

    if (state != nullptr && state->getSize() > 0)
        instance->setStateInformation (state->getData(), (int) state->getSize());

    auto node = graph.addNode (std::move (instance));

    if (node == nullptr)
        return;

    if (slots[slotIndex].loadGeneration != loadGeneration)
    {
        graph.removeNode (node->nodeID);
        rebuildGraphConnections();
        return;
    }

    auto& s = slots[slotIndex];
    s.nodeID = node->nodeID;
    s.pluginIdentifier = timbresIdentifiers[libraryIndex];
    {
        const juce::ScopedLock mappingLock (mapLock);
        s.mappingIdentity =
            ChurchDAWMidiMapping::internalSlotProcessorIdentity (
                s.pluginIdentifier);
        s.globalTransposeParameterIndex.store (
            internalTimbres->getTransposeGeralParam() != nullptr
                ? internalTimbres->getTransposeGeralParam()
                      ->getParameterIndex()
                : -1,
            std::memory_order_release);
        s.proc.store (node->getProcessor(), std::memory_order_release);
        parameterTargetGeneration.fetch_add (1, std::memory_order_acq_rel);
    }
    s.pendingIdentifier.clear();
    s.pendingState.clear();
    s.openButton.setEnabled (true);
    node->getProcessor()->addListener (this);

    // Escutar o Church Live e o que permite gravar o setup logo depois de
    // o musico escolher um timbre, em vez de so no botao Salvar. O
    // unloadSlot tira o listener antes de destruir o processador.
    if (auto* live = dynamic_cast<TimbresProcessor*> (node->getProcessor()))
        live->addChangeListener (this);
    // O combo tem UMA entrada nativa so ("Church Live (nativo)", id timbresComboId)
    // desde que o instrumento passou a enxergar todas as bibliotecas. Selecionar
    // timbresComboId + libraryIndex apontava pra um item inexistente quando a
    // biblioteca nao era a primeira: o ComboBox deselecionava e a tela da cadeia
    // exibia "(vazio)" com o Church Live carregado e tocando.
    s.box.setSelectedId (timbresComboId, juce::dontSendNotification);

    rebuildGraphConnections();
    for (int cacheSlot = 0; cacheSlot < numSlots; ++cacheSlot)
        refreshInsertMappingIdentityCacheForSlot (cacheSlot);
}

void MainComponent::unloadSlot (int slotIndex)
{
    auto& s = slots[slotIndex];
    if (s.pendingNodeRetirement.hasPending())
        return;

    if (auto* live = dynamic_cast<TimbresProcessor*> (s.proc.load()))
        live->setMidiMappingTopologyGenerationCounter (nullptr);
    ++s.loadGeneration;   // cancela qualquer AUv3 que ainda esteja abrindo
    s.loadingIdentifier.clear();
    s.openButton.setEnabled (false);
    const auto nodeToRemove = s.nodeID;
    {
        const juce::ScopedLock mappingLock (mapLock);
        s.globalTransposeParameterIndex.store (
            -1, std::memory_order_release);
        s.mappingIdentity.clear();
        for (auto& snapshot : insertMappingIdentityCache[slotIndex])
        {
            snapshot.processor = nullptr;
            snapshot.identity.clear();
            snapshot.topologyGeneration = 0;
        }
        parameterTargetGeneration.fetch_add (1, std::memory_order_acq_rel);
    }

    // Desanexa o slot ANTES de destruir editores. Plugins reais podem bombear
    // a fila de mensagens ao fechar; uma escolha reentrante passa a pertencer
    // a uma nova geracao e nunca pode ser removida por este unload antigo.
    s.nodeID = {};

    // Handshake de vida com a thread de MIDI (ver liveHandshakeLock no topo do
    // arquivo): so chama graph.removeNode (que destroi o processador) depois de
    // adquirir o lock, garantindo que nenhum callback de MIDI esteja dentro do
    // roteamento de canal em handleIncomingMidiMessage. A troca de slot e um
    // gesto manual — o bloqueio breve da thread de MIDI aqui e aceitavel frente
    // ao crash de desreferenciar o objeto destruido.
    {
        const juce::ScopedLock sl (liveHandshakeLock);
        s.proc.store (nullptr);

        if (nodeToRemove.uid != 0)
            removeNodeForSlot (slotIndex, nodeToRemove);
    }

    rebuildGraphConnections();
    for (int cacheSlot = 0; cacheSlot < numSlots; ++cacheSlot)
        refreshInsertMappingIdentityCacheForSlot (cacheSlot);
}

void MainComponent::removeNodeForSlot (int slotIndex,
                                       juce::AudioProcessorGraph::NodeID nodeID)
{
    if (nodeID.uid == 0)
        return;

    if (auto node = graph.getNodeForId (nodeID))
    {
        if (auto* live = dynamic_cast<TimbresProcessor*> (
                node->getProcessor());
            live != nullptr
             && ! live->canDestroyWithoutBlockingOnMidiMapping())
        {
            const bool reserved = slots[slotIndex]
                .pendingNodeRetirement.tryReserve (nodeID.uid);
            jassert (reserved);
            juce::ignoreUnused (reserved);
            return;
        }

        node->getProcessor()->removeListener (this);

        if (auto* live = dynamic_cast<TimbresProcessor*> (node->getProcessor()))
        {
            live->removeChangeListener (this);

            if (slotIndex == 0 && audioPanel != nullptr)
                audioPanel.reset();
        }
    }

    if (dockedNodeID == nodeID)
        undockEditor();

    closeWindowForNode (nodeID);
    graph.removeNode (nodeID);
    (void) slots[slotIndex].pendingNodeRetirement.clearIf (nodeID.uid);
}

void MainComponent::openEditorForSlot (int slotIndex)
{
    const auto nodeID = slots[slotIndex].nodeID;

    if (nodeID.uid == 0)
        return;

    // se o usuario destacou a janela, so traz pra frente; senao embute no painel
    if (auto existing = pluginWindows.find (nodeID.uid); existing != pluginWindows.end())
    {
        existing->second->toFront (true);
        return;
    }

    dockNode (nodeID);
}

void MainComponent::dockNode (juce::AudioProcessorGraph::NodeID nodeID)
{
    if (nodeID.uid == 0)
        return;

    if (dockedNodeID == nodeID && dockedEditor != nullptr)
        return;

    closeWindowForNode (nodeID);
    undockEditor();
    dockHolder.setSize (1, 1);

    // a janela nativa do plugin anterior precisa de um ciclo do Windows para
    // morrer; criar o proximo editor no mesmo instante deixa lixo visual
    pendingDockNode = nodeID;
    auto safe = juce::Component::SafePointer<MainComponent> (this);

    juce::MessageManager::callAsync ([safe]
    {
        if (auto* c = safe.getComponent())
        {
            const auto id = c->pendingDockNode;
            c->pendingDockNode = {};

            if (id.uid != 0)
                c->dockNodeNow (id);
        }
    });
}

void MainComponent::dockNodeNow (juce::AudioProcessorGraph::NodeID nodeID)
{
    if (dockedNodeID == nodeID && dockedEditor != nullptr)
        return;

    undockEditor();

    if (auto* node = graph.getNodeForId (nodeID))
    {
        auto* proc = node->getProcessor();

        std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditorIfNeeded());

        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (*proc);

        dockedEditor = std::move (editor);

        if (auto* timbres = dynamic_cast<TimbresEditor*> (dockedEditor.get()))
        {
            auto safe = juce::Component::SafePointer<MainComponent> (this);
            timbres->setOpenLibrariesCallback ([safe]
            {
                if (auto* component = safe.getComponent())
                    component->showLibraries();
            });
        }

        dockedEditor->addComponentListener (this);
        dockHolder.addAndMakeVisible (*dockedEditor);
        dockedNodeID = nodeID;
        dockedNative = (dynamic_cast<TimbresProcessor*> (proc) != nullptr);
        dockTitle.setText (proc->getName(), juce::dontSendNotification);

       #if ! CHURCHDAW_MOBILE_SHELL
        if (dockedNative && desktopReferenceLayout)
        {
            // O destaque do header acompanha a pagina realmente aberta. A
            // mudanca e silenciosa para nao reentrar no callback que acabou de
            // criar o editor.
            liveButton.setToggleState (true, juce::dontSendNotification);
            liveScenePanel.setVisible (false);
        }
       #endif

        dockTitle.setVisible (! dockedNative);
        dockFloatButton.setVisible (! dockedNative);
        dockCloseButton.setVisible (! dockedNative);
        dockViewport.setVisible (true);

        updateDockLayout();
        resized();
        repaint();

        // repintura extra depois que a janela nativa do plugin assenta
        auto safe = juce::Component::SafePointer<MainComponent> (this);

        juce::MessageManager::callAsync ([safe]
        {
            if (auto* c = safe.getComponent())
            {
                c->resized();
                c->repaint();

                if (c->dockedEditor != nullptr)
                    c->dockedEditor->repaint();
            }
        });
    }
}

void MainComponent::undockEditor()
{
    const bool wasNative = dockedNative;

    if (dockedEditor != nullptr)
    {
        dockedEditor->removeComponentListener (this);
        notifyProcessorEditorDeleted (dockedEditor.get());
        dockedEditor.reset();
    }

    dockedNodeID = {};
    dockedNative = false;
    dockTitle.setVisible (false);
    dockViewport.setVisible (false);
    dockFloatButton.setVisible (false);
    dockCloseButton.setVisible (false);

   #if ! CHURCHDAW_MOBILE_SHELL
    if (wasNative && desktopReferenceLayout)
        liveButton.setToggleState (false, juce::dontSendNotification);
   #endif

    updateDockLayout();
    repaint();
}

void MainComponent::updateDockLayout()
{
    resized();
    repaint();
}

void MainComponent::layoutDockHolder()
{
    if (dockedEditor == nullptr)
        return;

    const int w = juce::jmax (dockViewport.getMaximumVisibleWidth(), dockedEditor->getWidth());
    const int h = juce::jmax (dockViewport.getMaximumVisibleHeight(), dockedEditor->getHeight());

    dockHolder.setSize (w, h);
    dockedEditor->setCentrePosition (w / 2, h / 2);
}

void MainComponent::openFloatingWindowForNode (juce::AudioProcessorGraph::NodeID nodeID)
{
    if (nodeID.uid == 0)
        return;

    if (auto existing = pluginWindows.find (nodeID.uid); existing != pluginWindows.end())
    {
        existing->second->toFront (true);
        return;
    }

    if (auto* node = graph.getNodeForId (nodeID))
    {
        auto safe = juce::Component::SafePointer<MainComponent> (this);

        auto window = std::make_unique<PluginWindow> (node, [safe, nodeID]
        {
            // adiado: nao destruir a janela de dentro do proprio callback dela
            juce::MessageManager::callAsync ([safe, nodeID]
            {
                if (auto* c = safe.getComponent())
                    c->closeWindowForNode (nodeID);
            });
        });

        const int offset = 40 * ((int) pluginWindows.size() % 5);

        // Antes a janela nascia em coordenada de tela absoluta (140,140): num
        // setup de dois monitores abria no monitor PRIMARIO, atras da tela que o
        // musico olha. Agora posiciona relativo a janela principal da mesa e
        // recorta pela area util do monitor em que ela esta — sempre visivel e
        // no mesmo display do musico.
        auto host = getScreenBounds();
        auto place = juce::Rectangle<int> (window->getWidth(), window->getHeight())
                        .withPosition (host.getX() + 40 + offset,
                                       host.getY() + 40 + offset);

        if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (host))
            place = place.constrainedWithin (d->userArea);

        window->setBoundsConstrained (place);
        pluginWindows[nodeID.uid] = std::move (window);
    }
}

void MainComponent::componentMovedOrResized (juce::Component& component, bool, bool wasResized)
{
    if (wasResized && dockedEditor != nullptr && &component == dockedEditor.get())
        layoutDockHolder();
}

void MainComponent::closeWindowForNode (juce::AudioProcessorGraph::NodeID nodeID)
{
    pluginWindows.erase (nodeID.uid);
}

//==============================================================================
// Launchkey MK4 em modo DAW
//
// Mapa fixo (nao precisa aprender nada):
//   faders 1-5 -> volume dos canais 1-5      knobs 1-5 -> COMP dos canais 1-5
//   fader 9    -> MASTER                     knob 6    -> MIX do BigSky
//                                            knob 8    -> livre (MIDI Learn)
//==============================================================================
TimbresProcessor* MainComponent::getChurchLive() const
{
    for (int i = 0; i < numSlots; ++i)
        if (auto* t = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
            return t;

    return nullptr;
}

void MainComponent::setLaunchkeyMappingEnabled (bool enabled)
{
    if (launchkeyMappingEnabled == enabled)
        return;

    launchkeyMappingEnabled = enabled;
    appProperties.getUserSettings()->setValue (
        "launchkeyMappingEnabled", enabled);
    appProperties.getUserSettings()->saveIfNeeded();

    // Solta primeiro a porta exclusiva; depois a varredura reabre a entrada
    // comum quando o mapeamento foi desligado.
    launchkey.reset();
    refreshMidiInputs();

    if (launchkeyMappingEnabled)
    {
        lastLaunchkeyRetryMs = juce::Time::getMillisecondCounter();
        setupLaunchkey();
    }
}

void MainComponent::applyAutomaticControllerProfile (
    const juce::MidiDeviceInfo& device)
{
    if (LaunchkeyControl::isDawPort (device.name))
        return;

    const auto name = device.name.toLowerCase();
    juce::String profile;
    if (name.contains ("arturia")) profile = "Arturia";
    else if (name.contains ("m-audio") || name.contains ("m audio")) profile = "M-Audio";
    else if (name.contains ("studiologic") || name.contains ("n-key")) profile = "Studiologic";
    else if (name.contains ("native instruments") || name.contains ("komplete")) profile = "Native Instruments";
    else return;

    const auto sourceId =
        ChurchDAWMidiMapping::normalizeMidiSourceId (device.identifier);
    if (! midiMapsLoaded
         || automaticControllerProfilesApplied.count (sourceId) != 0)
        return;

    // Se o mapa principal e o last-good falharam, defaults automaticos nao
    // podem transformar um estado vazio em snapshot "valido" e sobrescrever
    // a unica evidencia recuperavel do cliente. Um novo Learn manual ou o
    // comando Limpar mapas resolve a situacao de forma explicita.
    if (esquemaDeCCsDesconhecido
         || bloquearSalvamentoCCVazio
         || automaticControllerProfilesSuspended)
    {
        juce::Logger::writeToLog (
            "MIDI_PROFILE: perfil automatico suspenso para preservar mapa existente");
        return;
    }

    beginMidiMapRoutingTransaction();
    bool changed = false;
    std::vector<MidiControlKey> insertedKeys;
    {
        const juce::ScopedLock sl (mapLock);
        // Convenção comum e segura: CC7 é volume principal. O mapeamento só
        // entra se o músico ainda não criou algo para aquele controle.
        const MidiControlKey volumeKey { sourceId, 1, 7, false, device.name };
        if (ChurchDAWMidiMapping::shouldAddAutomaticMapping (
                ccMappings, ccActionMappings, sourceId, device.name, 1, 7, false)
             && masterProcessor != nullptr
             && ! masterProcessor->getParameters().isEmpty())
        {
            const auto result = ccMappings.emplace (
                volumeKey, CCTarget {
                    -1, -1, -1, "Master", "master",
                    ChurchDAWMidiMapping::masterProcessorIdentity() });
            if (result.second)
            {
                insertedKeys.push_back (volumeKey);
                changed = true;
            }
        }

        // CC1 vira envio de reverb do primeiro layer quando o Church Live
        // está presente; não substitui um mapa existente e pode ser alterado
        // pelo MIDI Learn depois.
        if (auto* live = getChurchLive())
            if (auto* parameter = live->getLayerReverbSendParam (0))
            {
                const MidiControlKey ambienceKey { sourceId, 1, 1, false, device.name };
                if (ChurchDAWMidiMapping::shouldAddAutomaticMapping (
                        ccMappings, ccActionMappings, sourceId, device.name, 1, 1, false))
                {
                    const auto result = ccMappings.emplace (
                        ambienceKey,
                        CCTarget { 0,
                                   live->getParameters().indexOf (parameter),
                                   -1, parameter->getName (32),
                                   ChurchDAWMidiMapping::stableParameterId (parameter),
                                   slots[0].mappingIdentity });
                    if (result.second)
                    {
                        insertedKeys.push_back (ambienceKey);
                        changed = true;
                    }
                }
            }
    }

    if (changed)
    {
        ccDirtyEpoch.markDirty();
        const auto saved = saveMidiMap();
        if (saved)
        {
            automaticControllerProfilesApplied.insert (sourceId);
            bumpMidiMappingGeneration();
        }
        else
        {
            // A profile is a convenience, not user data. If persistence is
            // unavailable, roll it back from this session as well; otherwise
            // it can control the stage despite never having been accepted.
            const juce::ScopedLock sl (mapLock);
            for (const auto& key : insertedKeys)
                ccMappings.erase (key);
            // Um callback pode ter copiado um alvo temporario durante o I/O.
            // Invalide esse trabalho depois que o perfil sai da memoria.
            bumpMidiMappingGeneration();
        }
        refreshMidiMappingPanel();
        juce::Logger::writeToLog (
            "MIDI_PROFILE: " + profile + " aplicado a " + device.name
            + (saved ? " e salvo" : " (nao foi possivel salvar)"));
    }
    else
    {
        automaticControllerProfilesApplied.insert (sourceId);
        juce::Logger::writeToLog (
            "MIDI_PROFILE: " + profile + " detectado; mapas existentes preservados");
    }
    finishMidiMapRoutingTransaction();
}

void MainComponent::setupLaunchkey()
{
   #if CHURCHDAW_UI_PREVIEW
    return;
   #endif
    if (! launchkeyMappingEnabled)
    {
        launchkey.reset();
        return;
    }

    // Reconexao tambem acontece com o app aberto. Soltar o objeto antigo antes
    // de abrir o novo e obrigatorio no Windows, onde a porta MIDI e exclusiva.
    launchkey.reset();
    launchkey = std::make_unique<LaunchkeyControl>();

    if (! launchkey->isConnected())
    {
        juce::Logger::writeToLog ("LAUNCHKEY: porta DAW ainda nao disponivel");
        return;
    }

    juce::Logger::writeToLog ("LAUNCHKEY: porta DAW aberta"
                              + juce::String (launchkey->isListening() ? " (entrada e saida)"
                                                                      : " (somente saida)"));

    const auto launchkeyLifetime = midiCallbackLifetime;
    launchkey->onFader = [this, launchkeyLifetime] (int index, float v)
    {
        if (! launchkeyLifetime->load())
            return;

        static const juce::String sourceId ("churchdaw:launchkey-daw");

        // A porta DAW agora passa pelo MESMO MIDI Learn dos outros teclados.
        // Se este CC foi aprendido ele prevalece; sem mapa, continua valendo a
        // superficie pronta de fabrica logo abaixo.
        if (processControllerCC (LaunchkeyControl::faderCC0 + index,
                                 juce::roundToInt (v * 127.0f),
                                 sourceId, 1, false, "Launchkey DAW"))
            return;

        if (index == 8)   // fader 9 = master
        {
            masterSlider.setValue (masterSlider.proportionOfLengthToValue ((double) v),
                                   juce::sendNotificationSync);
            return;
        }

        if (auto* live = getChurchLive())
            if (auto* p = live->getLayerVolParam (index))
                p->setValueNotifyingHost (v);
    };

    launchkey->onEncoder = [this, launchkeyLifetime] (int index, float v)
    {
        if (! launchkeyLifetime->load())
            return;

        static const juce::String sourceId ("churchdaw:launchkey-daw");

        if (processControllerCC (LaunchkeyControl::encoderCC0 + index,
                                 juce::roundToInt (v * 127.0f),
                                 sourceId, 1, false, "Launchkey DAW"))
            return;

        auto* live = getChurchLive();
        const auto route =
            ChurchDAWMidiMapping::launchkeyEncoderRoute (index);

        if (route.target
                == ChurchDAWMidiMapping::LaunchkeyEncoderTarget::reverbReturn)
        {
            if (live != nullptr)
                if (auto* p = live->getReverbReturnParam())
                    p->setValueNotifyingHost (v);

            return;
        }

        if (route.target
                == ChurchDAWMidiMapping::LaunchkeyEncoderTarget::padVolume)
        {
            if (padProcessor != nullptr)
            {
                padProcessor->setGain (v * 1.5f);

                if (padPanel != nullptr)
                    padPanel->refreshState();
            }

            return;
        }

        if (route.target
                == ChurchDAWMidiMapping::LaunchkeyEncoderTarget::channelComp)
        {
            if (live != nullptr)
                if (auto* p = live->getLayerCompParam (route.layer))
                    p->setValueNotifyingHost (v);

            return;
        }

        // O knob 8 era Transpose global por padrao. Um toque acidental alterava
        // TODOS os canais ativos e a mudanca permanecia no setup — exatamente o
        // tipo de falha que nao pode existir ao vivo. Ele agora so controla algo
        // quando o musico o mapeia explicitamente pelo MIDI Learn (processado no
        // inicio deste callback).
    };

    // ---- pads ----
    // cima  (96..102): tons naturais do Pad Continuo   103: parar o pad
    // baixo (112..116): liga/desliga canais 1-5        119: PANIC
    launchkey->onPad = [this, launchkeyLifetime] (int note)
    {
        if (! launchkeyLifetime->load())
            return;

        if (note >= LaunchkeyControl::padTop0 && note < LaunchkeyControl::padTop0 + 7)
        {
            if (padProcessor != nullptr)
            {
                const int i = note - LaunchkeyControl::padTop0;
                int tom = padNatural (i);

                // com o modificador "#" ligado, os pads que TEM sustenido sobem
                // um semitom (E e B nao tem: continuam naturais)
                if (padSharp && padHasSharp (i))
                    ++tom;

                padProcessor->setKey (padProcessor->getKey() == tom ? -1 : tom);

                if (padPanel != nullptr)
                    padPanel->refreshState();
            }

            return;
        }

        if (note == LaunchkeyControl::padTop0 + 7)   // pad 8 = modificador #
        {
            padSharp = ! padSharp;
            refreshLaunchkeyNames();
            return;
        }

        auto* live = getChurchLive();

        if (note == LaunchkeyControl::padBottom0 + 5)   // parar o pad
        {
            if (padProcessor != nullptr)
            {
                padProcessor->setKey (-1);

                if (padPanel != nullptr)
                    padPanel->refreshState();
            }

            return;
        }

        if (note >= LaunchkeyControl::padBottom0 && note < LaunchkeyControl::padBottom0 + 5)
        {
            const int canal = note - LaunchkeyControl::padBottom0;

            if (live != nullptr)
                if (auto* p = live->getLayerOnParam (canal))
                    p->setValueNotifyingHost (live->getLayerOn (canal) ? 0.0f : 1.0f);

            return;
        }

        if (note == LaunchkeyControl::padBottom0 + 7)
        {
            performGlobalPanic();
            return;
        }
    };

    launchkey->showLogo ("ChurchDAW");
    refreshLaunchkeyNames();
}

// Os nomes so sao reenviados quando MUDAM (o proprio LaunchkeyControl compara),
// entao chamar isto do timer de 1 s nao pesa na porta MIDI.
void MainComponent::refreshLaunchkeyNames()
{
    if (launchkey == nullptr || ! launchkey->isConnected())
        return;

    auto* live = getChurchLive();

    for (int i = 0; i < TimbresProcessor::numLayers; ++i)
    {
        juce::String timbre;

        if (live != nullptr)
        {
            timbre = live->getLayerTimbre (i);

            if (timbre.isEmpty())
                timbre = live->getLayerInstrumentName (i);

            timbre = TimbresProcessor::prettyLabel (timbre);
        }

        const juce::String canal ("CANAL " + juce::String (i + 1));

        launchkey->setFaderName (i, timbre.isNotEmpty() ? canal + " " + timbre : canal + " vazio");
        launchkey->setEncoderName (i + 2, "COMP " + juce::String (i + 1));   // knobs 3-7
    }

    launchkey->setFaderName (8, "MASTER");
    launchkey->setEncoderName (0, "REVERB RETURN");
    launchkey->setEncoderName (1, "PAD VOL");
    launchkey->setEncoderName (7, "LIVRE - MAPEAR");

    // display fixo: o tom do Pad Continuo (e o que muda de musica pra musica)
    {
        static const char* nomes[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        const int tom = padProcessor != nullptr ? padProcessor->getKey() : -1;

        launchkey->showStationary ("ChurchDAW", "PAD",
                                   tom >= 0 ? juce::String ("Tom ") + nomes[tom]
                                            : juce::String (padSharp ? "# ligado" : "desligado"));
    }

    // ---- cores dos pads (o LaunchkeyControl ignora o que nao mudou) ----
    const int tomAtual = padProcessor != nullptr ? padProcessor->getKey() : -1;

    for (int i = 0; i < 7; ++i)
    {
        // aceso tambem quando o que toca e o SUSTENIDO deste pad (C# acende o C,
        // junto com o pad "#": le-se "C" + "#")
        const bool tocando = (tomAtual == padNatural (i))
                              || (padHasSharp (i) && tomAtual == padNatural (i) + 1);

        // azul avisa quais pads viram sustenido enquanto o "#" esta ligado
        const int cor = tocando ? LaunchkeyControl::green
                      : (padSharp && padHasSharp (i)) ? LaunchkeyControl::blue
                                                      : LaunchkeyControl::dim;

        launchkey->setPad (LaunchkeyControl::padTop0 + i, cor,
                           tocando ? LaunchkeyControl::pulsing : LaunchkeyControl::staticOn);
    }

    launchkey->setPad (LaunchkeyControl::padTop0 + 7,
                       padSharp ? LaunchkeyControl::orange : LaunchkeyControl::dim);

    for (int i = 0; i < TimbresProcessor::numLayers; ++i)
    {
        int cor = LaunchkeyControl::off;

        if (live != nullptr)
        {
            const bool cheio = live->getLayerTimbre (i).isNotEmpty()
                                || live->getLayerInstrument (i) != nullptr;

            if (cheio)
                cor = live->getLayerOn (i) ? LaunchkeyControl::red : LaunchkeyControl::dim;
        }

        launchkey->setPad (LaunchkeyControl::padBottom0 + i, cor);
    }

    launchkey->setPad (LaunchkeyControl::padBottom0 + 5,      // parar o pad
                       tomAtual >= 0 ? LaunchkeyControl::red : LaunchkeyControl::dim);
    launchkey->setPad (LaunchkeyControl::padBottom0 + 6, LaunchkeyControl::off);
    launchkey->setPad (LaunchkeyControl::padBottom0 + 7, LaunchkeyControl::orange);   // PANIC
}

//==============================================================================
// MIDI Learn: knobs/faders do teclado controlam parametros de plugin e o Master
//==============================================================================
void MainComponent::handleIncomingMidiMessage (juce::MidiInput* source,
                                               const juce::MidiMessage& message)
{
    const juce::ScopedLock callbackBarrier (midiCallbackBarrier);
    bool normalizeSustain = false;
    bool aggregateSustainDown = false;
    bool reassertSustainAfterBoundary = false;

    // roda na thread de MIDI - nao mexer em componentes daqui
    // O LED do header representa execucao musical, nao so knobs aprendidos.
    // Atomico e barato: note-on/off agora acende o mesmo pulso visual dos CCs.
    if (message.isNoteOnOrOff())
        lastMidiActivityMs.store (juce::Time::getMillisecondCounter());

    // Logging de cada toque faz formatacao, lock e I/O de arquivo na thread
    // MIDI. Em acordes densos isso disputa o callback de audio e produz um
    // atraso exatamente por evento. Fica disponivel apenas para builds de
    // diagnostico explicitamente preparados para inspecao de MIDI.
#if defined (CHURCHDAW_MIDI_EVENT_LOGGING) && CHURCHDAW_MIDI_EVENT_LOGGING
    if (message.isNoteOn())
        juce::Logger::writeToLog (
            "TOQUE: nota " + juce::String (message.getNoteNumber())
            + " vel " + juce::String (message.getVelocity())
            + " ch " + juce::String (message.getChannel()));
    else if (message.isController())
        juce::Logger::writeToLog (
            "TOQUE: CC " + juce::String (message.getControllerNumber())
            + " val " + juce::String (message.getControllerValue())
            + " ch " + juce::String (message.getChannel()));
#endif

    if (message.isController())
    {
        const juce::String sourceId = ChurchDAWMidiMapping::normalizeMidiSourceId (
            source != nullptr ? source->getIdentifier()
                              : juce::String ("unknown"));
        const juce::String sourceName =
            source != nullptr ? source->getName() : juce::String();
        const int controller = message.getControllerNumber();

        // Alguns controladores encerram a frase somente com All Sound Off /
        // All Notes Off, sem emitir um note-off individual. O instrumento
        // recebe o CC logo abaixo; o ledger de teclas de comando tambem deve
        // esquecer as notas desse aparelho/canal para nao carregar latches
        // obsoletos para a proxima frase.
        if (controller == 120 || controller == 123)
        {
            const int channel = juce::jlimit (1, 16, message.getChannel());
            const juce::ScopedLock sl (mapLock);
            notasSuprimidas.clearSourceChannel (sourceId, channel);
            reconstruirMascaraDeSuprimidas();
        }

        // A ORDEM AQUI E O RECURSO INTEIRO.
        //
        // O pad de borracha que manda CC caia direto no processControllerCC, e
        // aquele caminho so sabe transformar CC em VALOR DE PARAMETRO continuo —
        // ele nem olha o alvo que o painel do Pad armou. Por isso o mapeamento
        // do Pad por CC nunca acontecia, em circunstancia nenhuma: nao havia
        // bloqueio deliberado, simplesmente nao existia fio ligando os dois.
        //
        // E quando o CC VIRA comando, ele NAO segue para o motor. Sem esse
        // return, um pedal mapeado como "Pad Sol" tambem pisaria o sustain do
        // piano no mesmo gesto.
        if (ccDeAcaoFoiConsumido (controller, message.getControllerValue(),
                                  sourceId, message.getChannel(), sourceName))
            return;

        {
            const juce::ScopedLock stateLock (mapLock);
            rawCCPhysicalGestures.observeRawDuringTransaction (
                { sourceId,
                  juce::jlimit (1, 16, message.getChannel()),
                  controller, false, sourceName },
                message.getControllerValue());
            if (controller == 121)
                rawCCPhysicalGestures.clearSourceChannel (
                    sourceId,
                    juce::jlimit (1, 16, message.getChannel()));
        }

        processControllerCC (
            controller, message.getControllerValue(),
            sourceId, message.getChannel(), false, sourceName);

        // O CC11 passa primeiro pelo Learn acima, portanto um pedal de
        // expressao ainda pode comandar qualquer fader/knob escolhido pelo
        // usuario. Sem mapa, porem, ele termina aqui: encaminha-lo tambem ao
        // piano fazia o proprio instrumento mudar de expressao por fora da
        // mesa, que e exatamente a interferencia relatada pelo cliente.
        if (! ChurchDAWMidiMapping::shouldForwardRawControllerToInstruments (
                controller))
            return;

        if (controller == 64)
        {
            ChurchDAWMidiMapping::SourceAwareSustainState::Decision decision;
            {
                const juce::ScopedLock stateLock (mapLock);
                decision = sourceAwareSustain.update (
                    sourceId,
                    message.getChannel(),
                    message.getControllerValue() >= 64);
            }

            // O agregado nao mudou: outra porta ainda segura o pedal (ou este
            // aparelho repetiu o mesmo valor). Encaminhar o OFF individual
            // recriaria exatamente o corte "last OFF wins".
            if (! decision.forwardTransition)
                return;

            normalizeSustain = true;
            aggregateSustainDown = decision.aggregateDown;
        }
        else if (controller == 121)
        {
            // Reset All Controllers pertence a esta origem/canal. O motor
            // recebe o reset normalmente, mas o sustain agregado das outras
            // portas precisa ser reafirmado depois da fronteira.
            const juce::ScopedLock stateLock (mapLock);
            sourceAwareSustain.update (
                sourceId, message.getChannel(), false);
            reassertSustainAfterBoundary =
                sourceAwareSustain.isDown();
        }
        else if (controller == 120 || controller == 123)
        {
            // O host trata estes CCs como PANIC global e zera seu latch de
            // sustain. O pedal fisico, porem, continua abaixado e nao manda um
            // segundo 127; restaure-o logo apos o boundary.
            const juce::ScopedLock stateLock (mapLock);
            reassertSustainAfterBoundary =
                sourceAwareSustain.isDown();
        }
    }
    else if (message.isNoteOnOrOff() || message.isAftertouch())
    {
        if (notaFoiConsumida (source, message))
            return;   // tecla de comando: NAO vira nota no instrumento

        if (message.isNoteOnOrOff())
        {
            bool consumed = false;
            // P1-002: Ignorar Note-Off para evitar que alvos contínuos vão a zero ao soltar a nota.
            // sourceId so e montado em note-on: note-off nao consome mapeamento, e
            // montar a String aqui seria alocacao de heap morta em toda tecla solta
            // (o proprio arquivo documenta esse defeito em montarChaveDeNota).
            if (message.isNoteOn())
            {
                const auto suppressed = montarChaveDeNota (
                    source, message, message.getNoteNumber());
                bool debtReserved = false;
                {
                    const juce::ScopedLock stateLock (mapLock);
                    debtReserved = reservarNotaSuprimidaUnderMapLock (
                        suppressed);
                }

                // Reserve the paired OFF before processControllerCC can learn,
                // move a parameter or consume this ON. At capacity/depth limit
                // the complete gesture fails open and reaches the instrument.
                if (debtReserved)
                {
                    consumed = processControllerCC (
                        message.getNoteNumber(), message.getVelocity(),
                        suppressed.sourceId, message.getChannel(), true,
                        source != nullptr ? source->getName() : juce::String());
                    if (! consumed)
                    {
                        const juce::ScopedLock stateLock (mapLock);
                        if (notasSuprimidas.cancelAcquire (suppressed)
                              == ChurchDAWMidiMapping::SuppressedNoteRelease::releasedLast)
                            reconstruirMascaraDeSuprimidas();
                    }
                }
            }

            // P1-003: Se foi mapeada como CC, não tocar a nota original!
            if (consumed)
                return;
        }
    }

    // Tudo que nao foi consumido segue para o motor. Este repasse substitui o
    // registro direto do player como callback de MIDI.
    //
    // Roteamento 2 teclados: se o dispositivo esta amarrado a um canal do
    // Church Live (piano vs pad), reescreve o canal MIDI da mensagem antes de
    // enfileirar — teclados USB quase sempre mandam tudo no canal 1.
    //
    // Handshake de vida (ver liveHandshakeLock no topo do arquivo): a thread de
    // MIDI desreferencia o TimbresProcessor por ponteiro cru logo abaixo. O lock
    // garante que unloadSlot/ troca de slot nao destroem o objeto no meio deste
    // trecho — so e disputado numa troca de slot, nunca no meio do tocar.
    const juce::ScopedLock sl (liveHandshakeLock);

    juce::MidiMessage routed = message;

    if (normalizeSustain)
        routed = juce::MidiMessage::controllerEvent (
            message.getChannel(), 64,
            aggregateSustainDown ? 127 : 0);

    if (source != nullptr)
        if (auto* live = getChurchLive())
        {
            const int forced = live->getForcedMidiChannelForDevice (
                source->getIdentifier());
            if (forced >= 1 && forced <= 16
                 && routed.getChannel() != forced
                 && (routed.isNoteOnOrOff() || routed.isAftertouch()
                      // Channel pressure faltava nesta lista: o ramo de
                      // reescrita dela logo abaixo era codigo morto. Sem isso a
                      // pressao do teclado do piano continuava no canal 1 e
                      // modulava o pad do outro teclado — o musico ouve "o pad
                      // respira sozinho" e nao associa ao peso da propria mao.
                      || routed.isChannelPressure()
                      || routed.isPitchWheel()
                      || (routed.isController()
                           && routed.getControllerNumber() != 64
                           && routed.getControllerNumber() != 120
                           && routed.getControllerNumber() != 121
                           && routed.getControllerNumber() != 123)))
            {
                // Reconstroi com o canal forçado (API JUCE: withChannel).
                if (routed.isNoteOn())
                    routed = juce::MidiMessage::noteOn (
                        forced, routed.getNoteNumber(), routed.getVelocity());
                else if (routed.isNoteOff())
                    routed = juce::MidiMessage::noteOff (
                        forced, routed.getNoteNumber(),
                        routed.getVelocity());
                else if (routed.isAftertouch())
                    routed = juce::MidiMessage::aftertouchChange (
                        forced, routed.getNoteNumber(),
                        routed.getAfterTouchValue());
                else if (routed.isChannelPressure())
                    routed = juce::MidiMessage::channelPressureChange (
                        forced, routed.getChannelPressureValue());
                else if (routed.isPitchWheel())
                    routed = juce::MidiMessage::pitchWheel (
                        forced, routed.getPitchWheelValue());
                else if (routed.isController())
                    routed = juce::MidiMessage::controllerEvent (
                        forced, routed.getControllerNumber(),
                        routed.getControllerValue());
            }
        }

    // As fabricas estaticas do JUCE (noteOn/noteOff/aftertouch/pitchWheel/
    // controllerEvent) criam a mensagem com timestamp ZERO. O coletor jassert-
    // ifica timestamp != 0 e deriva a samplePosition como (ts - lastCallbackTime)
    // * rate — negativo gigante depois de minutos de culto, que o jlimit grampa
    // na amostra 0 (ou estoura int32 quando o callback atrasa). Repassa o
    // carimbo real do driver tambem quando o canal foi reescrito.
    routed.setTimeStamp (message.getTimeStamp());

    if (auto* live = getChurchLive())
    {
        pianoVisualState->setHostGlobalSustain (live->getGlobalSustain());
        pianoVisualState->observeHost (routed, live->getPedalInverted());
    }
    else
        pianoVisualState->observe (routed);
    player.getMidiMessageCollector().addMessageToQueue (routed);

    if (reassertSustainAfterBoundary)
    {
        auto sustain = juce::MidiMessage::controllerEvent (
            routed.getChannel(), 64, 127);
        const double baseTimestamp = message.getTimeStamp() > 0.0
            ? message.getTimeStamp()
            : juce::Time::getMillisecondCounterHiRes() * 0.001;
        sustain.setTimeStamp (baseTimestamp + 0.005);
        if (auto* live = getChurchLive())
            pianoVisualState->observeHost (sustain, live->getPedalInverted());
        else
            pianoVisualState->observe (sustain);
        player.getMidiMessageCollector().addMessageToQueue (
            sustain);
    }
}

// Chave completa da nota (aparelho + canal + numero). So e montada depois que
// as mascaras dizem que ha algo a procurar — ela custa uma alocacao.
ChurchDAWMidiMapping::ControllerKey MainComponent::montarChaveDeNota (
    juce::MidiInput* source, const juce::MidiMessage& message, int nota) const
{
    return { ChurchDAWMidiMapping::normalizeMidiSourceId (
                 source != nullptr ? source->getIdentifier()
                                   : juce::String ("unknown")),
             juce::jlimit (1, 16, message.getChannel()),
             nota,
             false,
             source != nullptr ? source->getName() : juce::String() };
}

// Espelha notasSuprimidas numa mascara atomica por NUMERO de nota, para o
// note-off poder desistir sem lock e sem alocar. Chamar sempre com o mapLock
// tomado; o conjunto tem, na pratica, as poucas teclas de comando que estao
// pressionadas neste instante.
void MainComponent::reconstruirMascaraDeSuprimidas()
{
    juce::uint32 nova[4] = { 0, 0, 0, 0 };

    notasSuprimidas.forEach (
        [&nova] (const auto& key, std::uint16_t depth)
        {
            if (depth > 0 && juce::isPositiveAndBelow (key.cc, 128))
                nova[key.cc >> 5] |= 1u << (key.cc & 31);
        });

    for (int i = 0; i < 4; ++i)
        mascaraDeNotasSuprimidas[i].store (nova[i], std::memory_order_relaxed);
}

bool MainComponent::reservarNotaSuprimidaUnderMapLock (
    const ChurchDAWMidiMapping::ControllerKey& key)
{
    if (! notasSuprimidas.tryAcquire (key))
    {
        // Capacity/depth exhaustion latches this key fail-open. Rebuild the
        // per-note fast mask now so its ON/OFF/aftertouch can bypass this
        // ledger unless another source/channel still has a real debt.
        reconstruirMascaraDeSuprimidas();
        return false;
    }
    if (juce::isPositiveAndBelow (key.cc, 128))
        mascaraDeNotasSuprimidas[key.cc >> 5].fetch_or (
            1u << (key.cc & 31), std::memory_order_relaxed);
    return true;
}

void MainComponent::beginMidiMapRoutingTransaction()
{
    // Acquire the same barrier held for the complete MIDI callback before
    // publishing the mode. A callback therefore sees one routing generation
    // from start to finish; none can observe the flag changing halfway through
    // a note/CC gesture.
    const juce::ScopedLock callbackDrain (midiCallbackBarrier);
    midiMapTransactionInProgress.store (true, std::memory_order_release);
    // Invalidate every Learn event captured before this structural boundary.
    // MIDI producers are drained by callbackBarrier, so this queue clear is
    // complete; parameter callbacks use generations and remain non-blocking.
    parameterLearnGeneration.fetch_add (1, std::memory_order_acq_rel);
    parameterLearnSelectionGeneration.fetch_add (
        1, std::memory_order_acq_rel);
    midiLearnControlQueue.clear();
    const juce::ScopedLock mapGuard (mapLock);

    ccActionsAtMapTransactionStart = ccActionMappings;
    notesSuppressedByMapTransaction.clear();
    deferredCCDecisions.clear();

    for (const auto& mapping : noteMappings)
    {
        auto key = mapping.first;
        key.isNote = false;
        notesSuppressedByMapTransaction.insert (std::move (key));
    }

    for (const auto& mapping : ccMappings)
        if (mapping.first.isNote)
        {
            auto key = mapping.first;
            key.isNote = false;
            notesSuppressedByMapTransaction.insert (std::move (key));
        }

    // Any stateful raw pedal (64..69) may predate the transaction. Start with
    // the complete physical ledger, then let the dedicated sustain aggregate
    // seed cover devices restored before the generic ledger was populated.
    rawCCGesturesDuringMapTransaction = rawCCPhysicalGestures;
    sourceAwareSustain.seedRawTransactionGestures (
        rawCCGesturesDuringMapTransaction);
    rawCCTransactionGesturePending.store (
        rawCCGesturesDuringMapTransaction.hasPendingGesture(),
        std::memory_order_release);
}

void MainComponent::finishMidiMapRoutingTransaction()
{
    using DeferredOutput =
        ChurchDAWMidiMapping::DeferredMidiActionQueue<64>::Output;
    std::array<DeferredOutput, 64> decisions {};
    size_t decisionCount = 0;
    bool decisionOverflow = false;
    {
        const juce::ScopedLock callbackDrain (midiCallbackBarrier);
        const juce::ScopedLock mapGuard (mapLock);

        // Preserve edge-detector progress observed while disk I/O was in
        // flight, independent of whether the structural map committed or
        // rolled back.
        for (auto& destination : ccActionMappings)
            for (const auto& runtime : ccActionsAtMapTransactionStart)
                if (ChurchDAWMidiMapping::samePhysicalMidiControl (
                        destination.first, runtime.first,
                        midiSourcesCached))
                {
                    destination.second.estado = runtime.second.estado;
                    break;
                }

        decisionCount = deferredCCDecisions.copyTo (decisions);
        decisionOverflow = deferredCCDecisions.overflowed();
        deferredCCDecisions.clear();

        // Publish the new structural masks before callbacks can observe that
        // the transaction ended. Otherwise a raw ON can escape in the gap and
        // its OFF can be consumed by the freshly-installed map.
        reconstruirMascaraDeNotasUnderMapLock();
        reconstruirMascaraDeCCsDeAcaoUnderMapLock();
        ccActionsAtMapTransactionStart.clear();
        notesSuppressedByMapTransaction.clear();
        // Producers are still stopped by midiCallbackBarrier, so publish the
        // complete descriptor table before callbacks can observe the new map
        // generation. This prevents the first post-transaction CC from being
        // lost as a stale-table event.
        if (publishedControllerRouteMappingGeneration
              != midiMappingGeneration.load (std::memory_order_acquire))
            rebuildControllerRoutesIfNeeded (true);
        midiMapTransactionInProgress.store (
            false, std::memory_order_release);
    }

    // A pending AsyncUpdater callback may have returned while the transaction
    // flag was set. Re-arm the single coalesced wake after publishing routes.
    triggerAsyncUpdate();

    // Execute physical edges only after both callback/map locks are released.
    // In particular, a seguirNivel OFF received during disk I/O must still
    // stop the Pad; copying only the edge-detector state left it sounding
    // forever because no second release event exists.
    if (decisionOverflow
         && ! componentShutdownStarted.load (std::memory_order_acquire))
    {
        // A fila de decisoes pode exceder 64 ate repetindo uma unica identidade.
        // Falhe em silencio: pare o som sustentado e nao execute nenhum prefixo
        // da sequencia que ficou parcialmente representada.
        juce::Logger::writeToLog (
            "MIDI MAP: overflow ao reconciliar botoes; PANIC seguro aplicado");
        executarAcaoDeNota ({ NoteActionKind::panic, -1 });
    }
    else if (! componentShutdownStarted.load (std::memory_order_acquire))
        for (size_t i = 0; i < decisionCount; ++i)
            aplicarDecisaoDeBotaoNoTom (
                { static_cast<NoteActionKind> (decisions[i].actionKind),
                  decisions[i].argument },
                decisions[i].decision);
}

void MainComponent::resetMidiLearnTargetUnderCallbackBarrier (bool enabled)
{
    const juce::ScopedLock callbackDrain (midiCallbackBarrier);
    midiLearnTouchTimer.setRunning (false);
    midiLearnActive.store (false, std::memory_order_release);
    // Parameter callbacks publish only POD events tagged with Learn/topology
    // generations. An in-flight old callback may finish after this point, but
    // the consumer rejects its generation, so no synchronous spin or unsafe
    // clear is needed. MIDI producers, on the other hand, are drained by the
    // barrier held above, so their bounded queue can be cleared completely.
    parameterLearnGeneration.fetch_add (1, std::memory_order_acq_rel);
    midiLearnControlQueue.clear();

    {
        const juce::ScopedLock mapGuard (mapLock);
        lastTouchedSlot.store (-2, std::memory_order_relaxed);
        lastTouchedParam.store (-1, std::memory_order_relaxed);
        lastTouchedInsert.store (-1, std::memory_order_relaxed);
        lastTouchedTopologyGeneration.store (
            0, std::memory_order_relaxed);
        lastTouchedParamName.clear();
        lastTouchedParamId.clear();
        lastTouchedPluginId.clear();
        lastTouchedParameterIdentityUnique.store (
            false, std::memory_order_relaxed);
        pendingNoteAction = {};
    }

    midiLearnActive.store (enabled, std::memory_order_release);
    midiLearnTouchTimer.setRunning (enabled);
}

// Decide se esta mensagem de nota e uma TECLA DE COMANDO mapeada pelo musico.
//
// Roda na thread de MIDI, entao o caminho comum precisa ser barato: um teste
// de bit numa mascara atomica resolve 99,9% das notas sem tocar em lock
// nenhum. So no acerto e que vale a pena pegar o mapLock.
bool MainComponent::notaFoiConsumida (juce::MidiInput* source,
                                      const juce::MidiMessage& message)
{
    const bool transactionInProgress =
        midiMapTransactionInProgress.load (std::memory_order_acquire);

    const int nota = message.getNoteNumber();

    if (! juce::isPositiveAndBelow (nota, 128))
        return false;

    const int palavra = nota >> 5;
    const juce::uint32 bit = 1u << (nota & 31);

    // NADA DE ALOCAR ANTES DOS TESTES BARATOS.
    //
    // Montar "midi:" + identificador cria uma String nova — alocacao de heap —
    // e isto acontecia em TODA nota, tocada ou nao, antes de qualquer teste.
    // Um teclado com aftertouch polifonico manda centenas de mensagens por
    // segundo; era heap na thread de MIDI o tempo inteiro, mesmo sem nenhum
    // mapeamento. As mascaras atomicas abaixo respondem "nao tem nada aqui"
    // sem tocar em lock nem em memoria.
    //
    // NOTE-OFF E AFTERTOUCH SEGUEM O NOTE-ON, NAO O MAPA.
    //
    // Se o mapa mudar com a tecla pressionada, olhar o mapa aqui suprimiria
    // um note-off cujo note-on passou (nota presa no culto) ou deixaria
    // passar um note-off orfao. A memoria do que foi suprimido e a unica
    // fonte de verdade para fechar o par.
    if (! message.isNoteOn())
    {
        // Nenhuma nota com este numero esta suprimida em lugar nenhum.
        if ((mascaraDeNotasSuprimidas[palavra].load (std::memory_order_relaxed) & bit) == 0)
            return false;

        const auto chaveOff = montarChaveDeNota (source, message, nota);
        const juce::ScopedLock trava (mapLock);

        if (message.isNoteOff())
        {
            const auto release =
                notasSuprimidas.consumeRelease (chaveOff);
            if (release == ChurchDAWMidiMapping::SuppressedNoteRelease::releasedLast)
                reconstruirMascaraDeSuprimidas();
            return release == ChurchDAWMidiMapping::SuppressedNoteRelease::stillSuppressed
                || release == ChurchDAWMidiMapping::SuppressedNoteRelease::releasedLast;
        }

        return notasSuprimidas.shouldSuppressCurrentGesture (chaveOff);
    }

    // Note-on: fora do Learn, sem bit ligado nao existe mapeamento para esta
    // nota — devolve na hora, sem construir chave nenhuma.
    if (! transactionInProgress
         && ! midiLearnActive.load()
         && (mascaraDeNotasMapeadas[palavra].load (std::memory_order_relaxed) & bit) == 0)
        return false;

    const auto chave = montarChaveDeNota (source, message, nota);

    if (transactionInProgress)
    {
        const juce::ScopedLock trava (mapLock);
        if (notesSuppressedByMapTransaction.find (chave)
                == notesSuppressedByMapTransaction.end())
            return false;

        // Congela comandos E notas usadas como parâmetros durante a transação,
        // conservando o pareamento do gesto. O note-off correspondente seguirá
        // este ledger mesmo se a estrutura do mapa mudar antes da soltura.
        return reservarNotaSuprimidaUnderMapLock (chave);
    }

    // No modo Learn, tocar um tom no painel do Pad escolhe o alvo e a proxima
    // nota fisica cria o mapa. O primeiro note-on serve apenas para aprender;
    // ele e suprimido junto com seu note-off e nao dispara instrumento/Pad.
    if (midiLearnActive.load())
    {
        NoteAction aprendida;
        bool desmapeou = false;
        bool recusou = false;

        {
            const juce::ScopedLock trava (mapLock);
            aprendida = pendingNoteAction;

            if (aprendida.kind != NoteActionKind::none)
            {
                if (! reservarNotaSuprimidaUnderMapLock (chave))
                    return false;

                auto parameterKey = chave;
                parameterKey.isNote = true;
                const CCTarget* existingParameter = nullptr;
                for (const auto& mapping : ccMappings)
                    if (mapping.first.isNote
                         && ChurchDAWMidiMapping::samePhysicalMidiControl (
                                mapping.first, parameterKey,
                                midiSourcesCached))
                    {
                        existingParameter = &mapping.second;
                        break;
                    }
                if (existingParameter != nullptr)
                {
                    motivoDaRecusaDeNota =
                        existingParameter->paramName.isNotEmpty()
                            ? existingParameter->paramName
                            : juce::String::fromUTF8 ("outro parâmetro");
                    lastNoteActionRejected.store (nota, std::memory_order_relaxed);
                    recusou = true;
                }
                else
                {
                    noteMappings[chave] = aprendida;
                    noteDirtyEpoch.markDirty();

                    // O bit de MAPEADAS sobe aqui DENTRO do lock, junto com a
                    // escrita no mapa. Ligado la fora, uma reconstrucao que
                    // estivesse em curso na outra thread escreveria o snapshot
                    // velho e apagaria este bit.
                    mascaraDeNotasMapeadas[palavra].fetch_or (
                        bit, std::memory_order_relaxed);

                    lastMappedNoteKey = chave;
                    lastMappedNoteAction = aprendida;
                    lastMappedNote.store (nota, std::memory_order_relaxed);
                    bumpMidiMappingGeneration();
                    pendingNoteAction = {};
                }
            }
            else
            {
                // DESMAPEAR: no Learn, sem alvo escolhido, tocar numa tecla JA
                // mapeada devolve ela a tocar nota.
                //
                // Antes disto, mapear uma tecla era irreversivel: o unico
                // caminho de remocao era "Limpar mapas", que apaga tambem
                // todos os mapas de CC. Uma tecla mapeada por engano — e o
                // engano e facil, basta encostar nela com um alvo armado —
                // custava o mapa inteiro do controlador.
                //
                // O gesto e simetrico ao de mapear e cabe na mesma explicacao:
                // dentro do Learn, a tecla e comando; fora, e nota.
                const auto it = noteMappings.find (chave);

                if (it != noteMappings.end())
                {
                    if (! reservarNotaSuprimidaUnderMapLock (chave))
                        return false;
                    noteMappings.erase (it);
                    noteDirtyEpoch.markDirty();
                    lastMappedNoteKey = chave;
                    lastMappedNoteAction = {};
                    lastMappedNote.store (nota, std::memory_order_relaxed);
                    bumpMidiMappingGeneration();
                    desmapeou = true;
                }
            }
        }

        if (recusou)
        {
            triggerAsyncUpdate();
            return true;
        }

        if (desmapeou)
        {
            // A mascara e um filtro por NUMERO de nota e a mesma nota pode
            // seguir mapeada noutro canal ou noutro teclado. Reconstruir e a
            // unica forma correta de apagar o bit — e nao pode acontecer aqui,
            // na thread de MIDI, porque percorre o mapa inteiro.
            lastLearnedAtMs.store (juce::Time::getMillisecondCounter());
            triggerAsyncUpdate();
            return true;
        }

        if (aprendida.kind != NoteActionKind::none)
        {
            // (o bit ja subiu sob o lock, junto com a escrita no mapa)
            lastLearnedAtMs.store (juce::Time::getMillisecondCounter());
            triggerAsyncUpdate();
            return true;
        }
    }

    // Caminho rapido: sem bit ligado, nem existe mapeamento para esta nota.
    if ((mascaraDeNotasMapeadas[nota >> 5].load (std::memory_order_relaxed)
          & (1u << (nota & 31))) == 0)
        return false;

    NoteAction acao;
    juce::uint64 mappingGeneration = 0;

    {
        const juce::ScopedLock trava (mapLock);
        const auto it = noteMappings.find (chave);

        if (it == noteMappings.end())
            return false;

        acao = it->second;
        mappingGeneration = midiMappingGeneration.load (std::memory_order_relaxed);

        if (acao.kind != NoteActionKind::none)
        {
            if (! reservarNotaSuprimidaUnderMapLock (chave))
                return false;
        }
    }

    if (acao.kind == NoteActionKind::none)
        return false;

    // A acao em si mexe em UI e no motor: sai da thread de MIDI.
    const auto midiLifetime = midiCallbackLifetime;
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this),
         midiLifetime, mappingGeneration, acao]
        {
            if (! midiLifetime->load())
                return;
            if (auto* self = safe.getComponent())
                if (self->midiMappingGeneration.load (std::memory_order_acquire)
                        == mappingGeneration)
                    self->executarAcaoDeNota (acao);
        });

    return true;
}

void MainComponent::executarAcaoDeNota (NoteAction acao)
{
    // O guard do Pad vive DENTRO dos cases dele.
    //
    // No topo da funcao ele calava tambem cena e panic: num setup sem Pad
    // carregado o musico apertava a tecla do panico no meio do culto e nada
    // acontecia. Cena e panic nao dependem do Pad Continuo.
    switch (acao.kind)
    {
        case NoteActionKind::padKey:
            // SELECIONA o tom, nao alterna.
            //
            // Alternar copiava o gesto dos pads do Launchkey, mas o Launchkey
            // tem LED: ali o musico VE o estado antes de apertar. Uma tecla do
            // piano nao tem retorno nenhum, e alternar as cegas quebra
            // justamente o gesto mais comum do palco — a cena da musica ja pos
            // o Pad em Sol e o tecladista aperta a tecla "Pad Sol" para se
            // certificar. Com toggle, esse "certificar-se" DESLIGAVA o Pad no
            // primeiro acorde. Selecionar e idempotente: apertar de novo
            // confirma. Para calar o Pad existe a acao Parar, que e explicita.
            if (padProcessor == nullptr)
                return;

            padProcessor->setKey (acao.arg);
            break;

        case NoteActionKind::padOff:
            if (padProcessor == nullptr)
                return;

            padProcessor->setKey (-1);
            break;

        case NoteActionKind::cena:
            // aplicarCena valida o indice sozinho e devolve "indisponivel"
            // quando arg nao aponta cena nenhuma; nada a fazer com o resultado
            // aqui, o proprio aplicarCena ja loga e cuida da UI.
            (void) aplicarCena (acao.arg);
            return;

        case NoteActionKind::panic:
        {
            performGlobalPanic();
            return;
        }

        case NoteActionKind::nextScene:
        {
            const int next = cenaAtiva < 0 ? 0
                                           : juce::jmin (ChurchDAWLive::numeroDeCenas - 1,
                                                         cenaAtiva + 1);
            (void) aplicarCena (next);
            return;
        }

        case NoteActionKind::previousScene:
        {
            const int previous = cenaAtiva < 0 ? 0 : juce::jmax (0, cenaAtiva - 1);
            (void) aplicarCena (previous);
            return;
        }

        case NoteActionKind::padSynthProgram:
            if (padSynthProcessor != nullptr
                 && juce::isPositiveAndBelow (acao.arg,
                                               PadSynthProcessor::numPrograms))
            {
                padSynthProcessor->selectProgram (acao.arg);
                if (padEditorPanel != nullptr)
                    padEditorPanel->refreshFromProcessor();
                (void) saveChainState();
            }
            return;

        case NoteActionKind::tapTempo:
            if (performanceSessionPanel != nullptr)
                performanceSessionPanel->triggerTapTempo();
            return;

        case NoteActionKind::sustainToggle:
            if (auto* live = getChurchLive())
            {
                live->setGlobalSustain (! live->getGlobalSustain());
                refreshPerformanceSessionPanel();
                agendarGravacaoDoSetup();
            }
            return;

        case NoteActionKind::padSynthEngineToggle:
            // Compatibilidade com mapas antigos: a acao ainda pode existir no
            // XML de um cliente, mas o Pad Synth foi retirado do produto. Nunca
            // reative o motor silencioso por um footswitch esquecido.
            if (padSynthProcessor != nullptr)
            {
                padSynthProcessor->setEngineEnabled (false);
                if (padEditorPanel != nullptr)
                    padEditorPanel->refreshFromProcessor();
            }
            return;

        case NoteActionKind::none:
        default:
            return;
    }

    if (padPanel != nullptr)
        padPanel->refreshState();
}

//==============================================================================
// PAD CONTINUO ACIONADO POR CC — o "pad de borracha" (07/08/2026)
//==============================================================================
//
// A REGRA, EM UMA FRASE: no caminho de CC o Pad ALTERNA no APERTO e a mensagem
// de "soltou" nunca liga nem desliga nada. Quem tem pad com trava marca isso no
// menu do tom e ai o Pad passa a seguir o nivel do proprio controle.
//
// POR QUE NAO HA DETECCAO AUTOMATICA DE "COM TRAVA vs SEM TRAVA":
//
// A sequencia de bytes dos dois e IDENTICA, bit a bit. Um pedal momentaneo manda
// alto ao pisar e baixo ao soltar; um pedal com trava manda alto num toque e
// baixo no seguinte. A UNICA diferenca esta no TEMPO entre as duas mensagens — e
// o tempo e uma variavel do MUSICO, nao do aparelho: um pe parado em cima do
// pedal enquanto ele le a tela produz exatamente o intervalo do pedal com trava.
// A tentativa anterior tentou classificar por tempo, com a constante indo de
// 350 ms para 1000 ms e depois para 4000 ms em tres iteracoes, sem hardware para
// medir nenhuma delas, e foi revertida inteira.
//
// E O ERRO NAO E SIMETRICO:
//   - tratar um pad COM TRAVA como sem trava  -> ele responde a cada dois toques.
//     Chato, visivel na hora, o musico contorna apertando de novo. FALHA MACIA.
//   - tratar um pad SEM TRAVA como com trava  -> o tom soa SO ENQUANTO O DEDO
//     ESTA EM CIMA. O musico nao sustenta nada e nao tem como adivinhar o motivo.
//     FALHA FATAL no meio do culto.
//
// Por isso o padrao e "alterna no aperto, ignora o soltar": nesse modo e
// IMPOSSIVEL o Pad se desligar sozinho por causa de uma mensagem de release.
//
// O ESTADO DO PAD NAO E GUARDADO AQUI. A decisao "alternar" e resolvida lendo
// padProcessor->getKey() na hora, por tres motivos que valem no palco:
//   1. uma cena, a tela ou uma tecla MIDI que troquem o tom por fora nao
//      dessincronizam nada — o proximo toque no pad continua fazendo o esperado;
//   2. com o Pad em Re, apertar o pad de "Sol" LIGA em Sol, em vez de desligar;
//   3. uma mensagem perdida no cabo nao inverte o estado para o resto do culto.

bool MainComponent::ccDeAcaoFoiConsumido (int cc, int value,
                                          const juce::String& sourceId,
                                          int channel,
                                          const juce::String& sourceName)
{
    if (! juce::isPositiveAndBelow (cc, 128))
        return false;

    const int palavra = cc >> 5;
    const juce::uint32 bit = 1u << (cc & 31);
    const bool aprendendo = midiLearnActive.load();
    const bool transactionInProgress =
        midiMapTransactionInProgress.load (std::memory_order_acquire);
    const bool rawTransactionGesturePending =
        rawCCTransactionGesturePending.load (std::memory_order_acquire);

    // CAMINHO COMUM, SEM LOCK: fora do Learn, sem bit ligado nao existe comando
    // para este numero de CC. Um knob mandando dezenas de mensagens por segundo
    // sai por esta linha sem encostar no std::map.
    if (! transactionInProgress && ! rawTransactionGesturePending
         && ! aprendendo
         && (mascaraDeCCsDeAcao[palavra].load (std::memory_order_relaxed) & bit) == 0)
        return false;

    const MidiControlKey control {
        sourceId, juce::jlimit (1, 16, channel), cc, false, sourceName
    };

    if (! control.isValid())
        return false;

    const auto agora = juce::Time::getMillisecondCounter();
    {
        const juce::ScopedLock sl (mapLock);

        // Se o inicio do gesto atravessou cru durante uma transacao, o release
        // tambem precisa atravessar cru. Isto e especialmente importante para
        // CC64: consumir apenas o OFF depois de uma importacao deixaria o
        // sustain bruto permanentemente abaixado no instrumento.
        if (rawCCGesturesDuringMapTransaction
                .shouldContinueRawGesture (control, value))
        {
            rawCCTransactionGesturePending.store (
                rawCCGesturesDuringMapTransaction.hasPendingGesture(),
                std::memory_order_release);
            return false;
        }

        if (transactionInProgress)
        {
            const auto mapped = ccActionsAtMapTransactionStart.find (control);
            if (mapped != ccActionsAtMapTransactionStart.end()
                 && mapped->second.acao.kind != NoteActionKind::none)
            {
                // Mantenha o detector de borda coerente e adie a acao ate o
                // par de mapas fechar. Descartar a decisao perdia sobretudo o
                // unico OFF de um controle em modo seguirNivel.
                const auto decision =
                    ChurchDAWMidiMapping::decidirBotaoCC (
                    mapped->second.estado, mapped->second.modo, value, agora);
                if (decision
                     != ChurchDAWMidiMapping::DecisaoDeBotaoCC::nada)
                {
                    (void) deferredCCDecisions.push (
                        (int) mapped->second.acao.kind,
                        mapped->second.acao.arg,
                        decision);
                }
                return true;
            }

            rawCCGesturesDuringMapTransaction.observeRawDuringTransaction (
                control, value);
            rawCCTransactionGesturePending.store (
                rawCCGesturesDuringMapTransaction.hasPendingGesture(),
                std::memory_order_release);
            return false;
        }
    }

    // The pending-gesture slow path above may have found a different source.
    // Preserve the normal lock-free rejection for this unrelated CC.
    if (! aprendendo
         && (mascaraDeCCsDeAcao[palavra].load (std::memory_order_relaxed) & bit) == 0)
        return false;

    if (aprendendo)
    {
        NoteAction alvo;
        bool jaEParametro = false;
        juce::String nomeDoParametro;

        {
            const juce::ScopedLock sl (mapLock);
            alvo = pendingNoteAction;

            for (const auto& mapping : ccMappings)
                if (ChurchDAWMidiMapping::samePhysicalMidiControl (
                        mapping.first, control, midiSourcesCached))
                {
                    jaEParametro = true;
                    nomeDoParametro = mapping.second.paramName;
                    break;
                }
        }

        if (alvo.kind != NoteActionKind::none)
        {
            // Estrutural do protocolo (Bank Select, Data Entry, RPN/NRPN, Channel
            // Mode) nunca e gesto de musico. Note que 64/66/67 — os pedais — SAO
            // aceitos aqui, ao contrario da lista que protege parametro: e
            // exatamente em CC 64 que sai de fabrica boa parte dos footswitches.
            if (! ChurchDAWMidiMapping::isMappableAction (cc))
                return false;

            // NAO ROUBAR UM CONTROLE QUE JA TEM DONO.
            //
            // A tela pede "aperte o pad / pedal", entao a mao do musico ja esta no
            // controlador. Encostar num fader que ele mapeou mes passado criaria o
            // botao do Pad EM SILENCIO e, a partir dali, aquele CC nunca mais
            // chegaria ao processControllerCC (por causa do return la em cima): o
            // fader simplesmente parava de funcionar, com o mapa antigo vivo no
            // arquivo e ate o contador da tela subindo. O que chegava no telefone
            // era "o fader de volume parou", sem uma unica pista ligando as duas
            // coisas. Recusa, avisa com o nome do parametro, e deixa o tom armado.
            if (jaEParametro)
            {
                {
                    const juce::ScopedLock sl (mapLock);
                    motivoDaRecusaDeCCAcao =
                        nomeDoParametro.isNotEmpty()
                            ? nomeDoParametro
                            : juce::String::fromUTF8 ("outro par\xc3\xa2metro");
                    lastCCAcaoRecusada.store (cc, std::memory_order_relaxed);
                }

                triggerAsyncUpdate();
                return true;
            }

            {
                const juce::ScopedLock sl (mapLock);

                AcaoDeCC nova;
                nova.acao = alvo;
                nova.aprendidoNesteLearn = true;

                // Alimenta o alcance do controle com a mensagem que criou o mapa.
                // A decisao e descartada: o GESTO DE APRENDIZADO E MUDO — o Pad
                // nao pode tocar no momento em que o musico esta so mapeando.
                (void) ChurchDAWMidiMapping::decidirBotaoCC (
                    nova.estado, nova.modo, value, agora);

                ccActionMappings[control] = nova;
                ccDirtyEpoch.markDirty();

                // O bit sobe DENTRO do lock, junto com a escrita no mapa. Ligado
                // la fora, uma reconstrucao em curso na outra thread escreveria o
                // snapshot velho por cima e apagaria o bit — o controle ficava
                // gravado no disco e MUDO no culto.
                mascaraDeCCsDeAcao[palavra].fetch_or (bit, std::memory_order_relaxed);

                lastMappedCCAcaoKey = control;
                lastMappedCCAcaoAction = alvo;
                lastMappedCCAcao.store (cc, std::memory_order_relaxed);
                bumpMidiMappingGeneration();
                pendingNoteAction = {};
            }

            lastLearnedAtMs.store (agora);
            triggerAsyncUpdate();
            return true;
        }

        // Sem alvo do Pad armado. Se o musico armou um PARAMETRO na tela, a
        // intencao dele e esta: libera o CC e deixa o caminho antigo aprender.
        const bool alvoDeParametroArmado = lastTouchedSlot.load() != -2;

        bool existe = false;
        bool ehDesteLearn = false;
        bool virouKnob = false;
        bool desmapeou = false;

        {
            const juce::ScopedLock sl (mapLock);
            const auto it = ccActionMappings.find (control);

            if (it != ccActionMappings.end())
            {
                existe = true;
                ehDesteLearn = it->second.aprendidoNesteLearn;

                if (ehDesteLearn && ! alvoDeParametroArmado)
                {
                    // VIGIA DE "CARA DE BOTAO".
                    //
                    // Mesmo sem dono anterior, o primeiro nao-zero que chegar pode
                    // ser de um knob, de um fader ou de um pedal de expressao. No
                    // culto, cada movimento espacado daquele controle alternaria o
                    // tom. Valores DISTINTOS demais provam controle continuo.
                    (void) ChurchDAWMidiMapping::decidirBotaoCC (
                        it->second.estado, it->second.modo, value, agora);

                    if (it->second.estado.pareceKnob()
                         && it->second.acao.kind != NoteActionKind::none)
                    {
                        // ESVAZIAR, NAO APAGAR. Apagando a entrada, a mensagem
                        // seguinte (o "soltei") nao acharia mais nada, cairia no
                        // processControllerCC com o Learn AINDA LIGADO e podia ser
                        // aprendida como CC -> PARAMETRO sem ninguem ver.
                        it->second.acao = {};
                        bumpMidiMappingGeneration();
                        virouKnob = true;
                    }
                }
                else if (it->second.acao.kind != NoteActionKind::none
                          && ! alvoDeParametroArmado)
                {
                    // DESMAPEAR: dentro do Learn, sem alvo escolhido, apertar um
                    // pad JA mapeado devolve ele. Mesmo gesto do mapa de notas.
                    it->second.acao = {};
                    bumpMidiMappingGeneration();
                    desmapeou = true;
                }
                else if (alvoDeParametroArmado)
                {
                    it->second.acao = {};
                    bumpMidiMappingGeneration();
                }
            }
        }

        if (virouKnob)
        {
            {
                const juce::ScopedLock sl (mapLock);
                motivoDaRecusaDeCCAcao = juce::String::fromUTF8 (
                    "isso parece um knob / pedal de express\xc3\xa3o");
                lastCCAcaoRecusada.store (cc, std::memory_order_relaxed);
            }

            triggerAsyncUpdate();
            return true;
        }

        if (desmapeou)
        {
            lastCCAcaoDesmapeada.store (cc);
            lastLearnedAtMs.store (agora);
            triggerAsyncUpdate();
            return true;
        }

        if (existe && ! alvoDeParametroArmado)
            return true;   // entrada viva porem vazia: engole o resto do gesto

        return false;
    }

    // ---- PALCO ----
    NoteAction acao;
    juce::uint64 mappingGeneration = 0;
    auto decisao = ChurchDAWMidiMapping::DecisaoDeBotaoCC::nada;

    {
        const juce::ScopedLock sl (mapLock);
        const auto it = ccActionMappings.find (control);

        if (it == ccActionMappings.end()
             || it->second.acao.kind == NoteActionKind::none)
            return false;

        acao = it->second.acao;
        mappingGeneration = midiMappingGeneration.load (std::memory_order_relaxed);
        decisao = ChurchDAWMidiMapping::decidirBotaoCC (
            it->second.estado, it->second.modo, value, agora);
    }

    if (decisao != ChurchDAWMidiMapping::DecisaoDeBotaoCC::nada)
    {
        const auto midiLifetime = midiCallbackLifetime;
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<MainComponent> (this),
             midiLifetime, mappingGeneration, acao, decisao]
            {
                if (! midiLifetime->load())
                    return;
                if (auto* self = safe.getComponent())
                    if (self->midiMappingGeneration.load (std::memory_order_acquire)
                            == mappingGeneration)
                        self->aplicarDecisaoDeBotaoNoTom (acao, decisao);
            });
    }

    // Consumido mesmo quando a decisao foi "nada": este CC virou comando, e
    // deixa-lo seguir para o motor faria o mesmo pedal pisar o sustain do piano.
    return true;
}

void MainComponent::aplicarDecisaoDeBotaoNoTom (
    NoteAction acao, ChurchDAWMidiMapping::DecisaoDeBotaoCC decisao)
{
    using Decisao = ChurchDAWMidiMapping::DecisaoDeBotaoCC;

    if (decisao == Decisao::nada)
        return;

    // As acoes modernas (programa do PAD, TAP, sustain, engine, cenas) usam o
    // mesmo detector seguro de gesto do footswitch, mas nao dependem do Pad
    // Continuo. Executam uma vez no flanco de ligar; o release nunca dispara
    // uma segunda acao.
    if (acao.kind != NoteActionKind::padKey
         && acao.kind != NoteActionKind::padOff)
    {
        if (decisao != Decisao::desligar)
            executarAcaoDeNota (acao);
        return;
    }

    if (padProcessor == nullptr)
        return;

    if (acao.kind == NoteActionKind::padOff)
    {
        // Parar e parar: nunca alterna, e o "soltou" do modo por nivel nao
        // religa nada.
        if (decisao == Decisao::desligar)
            return;

        padProcessor->setKey (-1);
    }
    else if (acao.kind == NoteActionKind::padKey
              && juce::isPositiveAndBelow (acao.arg, 12))
    {
        const int atual = padProcessor->getKey();   // fonte unica de verdade
        const int novo = ChurchDAWMidiMapping::tomDepoisDoBotao (
            atual, acao.arg, decisao);

        if (novo == atual)
            return;

        padProcessor->setKey (novo);
    }
    else
    {
        return;
    }

    if (padPanel != nullptr)
        padPanel->refreshState();
}

// Snapshot e store sob o MESMO lock, pelo mesmo motivo da mascara de notas: com
// o store fora, a thread de MIDI podia aprender um CC entre as duas metades e o
// snapshot velho apagava o bit recem-ligado.
void MainComponent::reconstruirMascaraDeCCsDeAcao()
{
    const juce::ScopedLock sl (mapLock);
    reconstruirMascaraDeCCsDeAcaoUnderMapLock();
}

void MainComponent::reconstruirMascaraDeCCsDeAcaoUnderMapLock()
{
    juce::uint32 nova[4] = { 0, 0, 0, 0 };

    for (const auto& par : ccActionMappings)
        if (juce::isPositiveAndBelow (par.first.cc, 128)
             && par.second.acao.kind != NoteActionKind::none)
            nova[par.first.cc >> 5] |= (1u << (par.first.cc & 31));

    for (int i = 0; i < 4; ++i)
        mascaraDeCCsDeAcao[i].store (nova[i], std::memory_order_relaxed);
}

// Sai do Learn: as entradas esvaziadas (desmapeadas ou recusadas por parecerem
// knob) sao removidas de vez. Devolve true se alguma coisa mudou.
bool MainComponent::limparBotoesDeCCVazios()
{
    bool mudou = false;

    {
        const juce::ScopedLock sl (mapLock);

        for (auto it = ccActionMappings.begin(); it != ccActionMappings.end();)
        {
            if (it->second.acao.kind == NoteActionKind::none)
            {
                it = ccActionMappings.erase (it);
                mudou = true;
            }
            else
            {
                it->second.aprendidoNesteLearn = false;
                ++it;
            }
        }
    }

    if (mudou)
    {
        ccDirtyEpoch.markDirty();
        bumpMidiMappingGeneration();
        reconstruirMascaraDeCCsDeAcao();
    }

    return mudou;
}

ChurchDAWMidiMapping::ModoDeBotaoCC MainComponent::modoDoBotaoDoTom (int tom) const
{
    const juce::ScopedLock sl (mapLock);

    for (const auto& par : ccActionMappings)
        if (par.second.acao.kind == NoteActionKind::padKey
             && par.second.acao.arg == tom)
            return par.second.modo;

    return ChurchDAWMidiMapping::ModoDeBotaoCC::alternar;
}

bool MainComponent::tomTemBotaoDeCC (int tom) const
{
    const juce::ScopedLock sl (mapLock);

    for (const auto& par : ccActionMappings)
        if (par.second.acao.kind == NoteActionKind::padKey
             && par.second.acao.arg == tom)
            return true;

    return false;
}

void MainComponent::definirModoDoBotaoDoTom (
    int tom, ChurchDAWMidiMapping::ModoDeBotaoCC modo)
{
    bool mudou = false;
    {
        const juce::ScopedLock sl (mapLock);

        for (auto& par : ccActionMappings)
            if (par.second.acao.kind == NoteActionKind::padKey
                 && par.second.acao.arg == tom
                 && par.second.modo != modo)
            {
                par.second.modo = modo;
                mudou = true;

                // Trocar o modo zera alcance/nivel medidos sob a regra anterior,
                // mas o primeiro gesto no novo modo continua valendo.
                par.second.estado.esquecer();
            }
    }

    if (mudou)
    {
        ccDirtyEpoch.markDirty();
        bumpMidiMappingGeneration();
        (void) saveMidiMap();
    }
    atualizarBotoesDeCCDoPad();
}

void MainComponent::clearMidiLearnParameterTargetUnderMapLock()
{
    lastTouchedSlot.store (-2, std::memory_order_relaxed);
    lastTouchedParam.store (-1, std::memory_order_relaxed);
    lastTouchedInsert.store (-1, std::memory_order_relaxed);
    lastTouchedTopologyGeneration.store (0, std::memory_order_relaxed);
    lastTouchedParamName.clear();
    lastTouchedParamId.clear();
    lastTouchedPluginId.clear();
    lastTouchedParameterIdentityUnique.store (
        false, std::memory_order_relaxed);
    parameterLearnSelectionGeneration.fetch_add (
        1, std::memory_order_release);
}

void MainComponent::bumpMidiMappingGeneration()
{
    midiMappingGeneration.fetch_add (1, std::memory_order_release);
    // AsyncUpdater coalesces concurrent MIDI/message-thread mutations into a
    // single preallocated wake. The message thread then republishes the fixed
    // route table before applying any queued value.
    triggerAsyncUpdate();
}

void MainComponent::rebuildControllerRoutesIfNeeded (bool force)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (! force
         && midiMapTransactionInProgress.load (std::memory_order_acquire))
        return;

    const auto observedGeneration = midiMappingGeneration.load (
        std::memory_order_acquire);
    if (! force
         && publishedControllerRouteMappingGeneration == observedGeneration)
        return;

    // publishRoute retires and rewrites one slot at a time. Stop callback
    // readers across the complete table swap so no mapped control can observe
    // that internal inactive/mixed-metadata window. The established lock order
    // is callback barrier, then mapLock (both are recursive for transaction
    // callers already holding them).
    const juce::ScopedLock callbackDrain (midiCallbackBarrier);
    controllerRoutes.beginRouteUpdate();
    size_t requested = 0;
    size_t published = 0;
    juce::uint64 capturedGeneration = observedGeneration;
    {
        const juce::ScopedLock sl (mapLock);
        capturedGeneration = midiMappingGeneration.load (
            std::memory_order_relaxed);
        requested = ccMappings.size();

        for (auto& descriptor : controllerRouteDescriptors)
            descriptor = {};

        for (const auto& mapping : ccMappings)
        {
            if (published >= controllerRouteCapacity)
                break;

            const auto* bytes = mapping.first.sourceId.toRawUTF8();
            const auto length = static_cast<size_t> (
                mapping.first.sourceId.getNumBytesAsUTF8());
            const auto routeGeneration = controllerRoutes.publishRoute (
                published, bytes, length, mapping.first.channel,
                mapping.first.cc, mapping.first.isNote);
            if (routeGeneration == 0)
                continue;

            controllerRouteDescriptors[published] = {
                mapping.first, mapping.second, routeGeneration, true
            };
            ++published;
        }
    }

    controllerRoutes.finishRouteUpdate (
        capturedGeneration, requested, published);
    publishedControllerRouteMappingGeneration = capturedGeneration;

    if (controllerRoutes.overflowed())
    {
        if (! controllerRouteOverflowReported)
        {
            controllerRouteOverflowReported = true;
            juce::Logger::writeToLog (
                "MIDI MAP: capacidade fixa de 128 rotas excedida; "
                "rotas excedentes seguem fail-open");
        }
    }
    else
    {
        controllerRouteOverflowReported = false;
    }
}

void MainComponent::consumeControllerRouteEvents()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    constexpr int drainBudget = 32;
    for (int drained = 0; drained < drainBudget; ++drained)
    {
        ChurchDAWMidiMapping::ControllerRouteEvent event;
        if (! controllerRoutes.tryConsumeNext (event))
            break;

        CCTarget target;
        MidiControlKey control;
        bool current = false;
        {
            const juce::ScopedLock sl (mapLock);
            if (event.routeSlot < controllerRouteDescriptors.size())
            {
                const auto& descriptor =
                    controllerRouteDescriptors[event.routeSlot];
                const auto existing = ccMappings.find (descriptor.control);
                current = descriptor.occupied
                    && descriptor.generation == event.routeGeneration
                    && event.mappingGeneration
                        == midiMappingGeneration.load (
                            std::memory_order_relaxed)
                    && existing != ccMappings.end()
                    && ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                        existing->first, descriptor.control)
                    && ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                        existing->second, descriptor.target);
                if (current)
                {
                    target = descriptor.target;
                    control = descriptor.control;
                }
            }
        }

        if (current
             && event.mappingGeneration
                    == midiMappingGeneration.load (std::memory_order_acquire)
             && ! midiMapTransactionInProgress.load (
                    std::memory_order_acquire))
            applyCCTarget (std::move (target),
                           static_cast<float> (event.value) / 127.0f,
                           std::move (control));
    }

    if (controllerRoutes.hasPending())
        triggerAsyncUpdate();
}

void MainComponent::consumeMidiLearnControlEvents()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (componentShutdownStarted.load (std::memory_order_acquire))
        return;

    const auto generationBefore = midiMappingGeneration.load (
        std::memory_order_acquire);
    ChurchDAWMidiMapping::FixedMidiLearnControlQueue<16>::Event event;
    while (midiLearnControlQueue.tryConsume (event))
    {
        if (! midiLearnActive.load (std::memory_order_acquire)
             || event.targetGeneration
                    != parameterLearnSelectionGeneration.load (
                        std::memory_order_acquire))
            continue;

        const auto sourceId = juce::String::fromUTF8 (
            event.sourceId.data(), static_cast<int> (event.sourceIdLength));
        const auto sourceName = juce::String::fromUTF8 (
            event.sourceName.data(),
            static_cast<int> (event.sourceNameLength));
        const MidiControlKey control {
            sourceId, static_cast<int> (event.channel),
            static_cast<int> (event.number), event.isNote, sourceName
        };
        if (! control.isValid())
            continue;
        lastLearnInputKey = control;
        lastLearnInputValue = (int) event.value;


        if (event.unmapOnly)
        {
            bool removed = false;
            {
                const juce::ScopedLock sl (mapLock);
                if (event.targetGeneration
                      != parameterLearnSelectionGeneration.load (
                            std::memory_order_relaxed))
                    continue;
                const auto existing = ccMappings.find (control);
                if (existing != ccMappings.end())
                {
                    const auto persistedControl = existing->first;
                    ccMappings.erase (existing);
                    ccDirtyEpoch.markDirty();
                    lastMappedKey = persistedControl;
                    lastMappedCC.store (event.number,
                                        std::memory_order_relaxed);
                    bumpMidiMappingGeneration();
                    removed = true;
                }
            }
            if (removed)
            {
                ccPickupToClear.store (event.number,
                                       std::memory_order_release);
                lastLearnedAtMs.store (
                    juce::Time::getMillisecondCounter(),
                    std::memory_order_release);
            }
            continue;
        }

        int slot = -2;
        int parameter = -1;
        int insert = -1;
        juce::uint64 armedTopologyGeneration = 0;
        bool identityUnique = false;
        juce::String parameterName;
        juce::String parameterId;
        juce::String pluginId;
        {
            const juce::ScopedLock sl (mapLock);
            if (event.targetGeneration
                  != parameterLearnSelectionGeneration.load (
                        std::memory_order_relaxed))
                continue;
            slot = lastTouchedSlot.load (std::memory_order_relaxed);
            parameter = lastTouchedParam.load (std::memory_order_relaxed);
            insert = lastTouchedInsert.load (std::memory_order_relaxed);
            armedTopologyGeneration =
                lastTouchedTopologyGeneration.load (
                    std::memory_order_relaxed);
            identityUnique = lastTouchedParameterIdentityUnique.load (
                std::memory_order_relaxed);
            parameterName = lastTouchedParamName;
            parameterId = lastTouchedParamId;
            pluginId = lastTouchedPluginId;
        }
        if (slot == -2)
            continue;

        const auto currentTopologyGeneration =
            parameterTargetGeneration.load (std::memory_order_acquire);
        juce::String currentPluginId;
        auto* const currentProcessor =
            resolveCurrentMidiMappingProcessor (
                slot, insert, currentPluginId);
        if (! ChurchDAWMidiMapping::parameterLearnTargetStillCurrent (
                armedTopologyGeneration, currentTopologyGeneration,
                pluginId, currentPluginId, currentProcessor != nullptr)
             || currentTopologyGeneration
                    != parameterTargetGeneration.load (
                        std::memory_order_acquire))
        {
            const juce::ScopedLock sl (mapLock);
            motivoDaRecusaDeParametro =
                "O plugin mudou depois que o alvo foi selecionado";
            lastParameterMappingRejectedCC.store (
                event.number, std::memory_order_relaxed);
            if (event.targetGeneration
                  == parameterLearnSelectionGeneration.load (
                        std::memory_order_relaxed))
                clearMidiLearnParameterTargetUnderMapLock();
            continue;
        }

        if (ChurchDAWMidiMapping::isProtectedPitchTarget (
                parameterId, parameterName))
        {
            juce::Logger::writeToLog (
                "MIDI MAP: aprendizado bloqueado para alvo protegido ("
                + parameterName + ")");
            const juce::ScopedLock sl (mapLock);
            if (event.targetGeneration
                  == parameterLearnSelectionGeneration.load (
                        std::memory_order_relaxed))
                clearMidiLearnParameterTargetUnderMapLock();
            continue;
        }

        if (! identityUnique)
        {
            const juce::ScopedLock sl (mapLock);
            motivoDaRecusaDeParametro = pluginId.isEmpty()
                ? juce::String (
                    "A identidade do motor ainda nao esta pronta; mova o alvo novamente")
                : juce::String::fromUTF8 (
                    "O plugin publicou um identificador duplicado ou inválido");
            lastParameterMappingRejectedCC.store (
                event.number, std::memory_order_relaxed);
            if (event.targetGeneration
                  == parameterLearnSelectionGeneration.load (
                        std::memory_order_relaxed))
                clearMidiLearnParameterTargetUnderMapLock();
            continue;
        }

        CCTarget learned {
            slot, parameter, insert, parameterName, parameterId, pluginId
        };
        if (learned.slot == -1)
        {
            learned.paramName = "Master";
            learned.paramId = "master";
        }

        bool noteActionConflict = false;
        {
            const juce::ScopedLock sl (mapLock);
            if (event.targetGeneration
                  != parameterLearnSelectionGeneration.load (
                        std::memory_order_relaxed))
                continue;
            if (armedTopologyGeneration
                  != parameterTargetGeneration.load (
                        std::memory_order_relaxed))
                continue;

            auto noteActionKey = control;
            noteActionKey.isNote = false;
            if (control.isNote)
                for (const auto& mapping : noteMappings)
                    if (ChurchDAWMidiMapping::samePhysicalMidiControl (
                            mapping.first, noteActionKey,
                            midiSourcesCached))
                    {
                        noteActionConflict = true;
                        break;
                    }

            if (noteActionConflict)
            {
                motivoDaRecusaDeParametro =
                    "essa nota ja controla outro comando";
                lastParameterMappingRejectedCC.store (
                    event.number, std::memory_order_relaxed);
            }
            else
            {
                ccMappings[control] = learned;
                ccDirtyEpoch.markDirty();
                lastMappedKey = control;
                lastMappedCC.store (event.number,
                                    std::memory_order_relaxed);
                bumpMidiMappingGeneration();
            }
            clearMidiLearnParameterTargetUnderMapLock();
        }

        if (! noteActionConflict)
        {
            const auto now = juce::Time::getMillisecondCounter();
            ccPickupToClear.store (event.number,
                                   std::memory_order_release);
            lastLearnedAtMs.store (now, std::memory_order_release);
            suppressTouchUntilMs.store (now + 800,
                                        std::memory_order_release);
            juce::Logger::writeToLog (
                "MIDI MAP: aprendido controle "
                + juce::String (event.number) + " canal "
                + juce::String (event.channel) + " de " + sourceId
                + " -> " + (learned.paramName.isNotEmpty()
                    ? learned.paramName : juce::String ("parametro")));
        }
    }

    if (midiLearnControlQueue.overflowed())
    {
        if (! midiLearnQueueOverflowReported)
        {
            midiLearnQueueOverflowReported = true;
            juce::Logger::writeToLog (
                "MIDI MAP: FIFO fixa de Learn cheia; gesto excedente seguiu fail-open");
        }
    }
    else
    {
        midiLearnQueueOverflowReported = false;
    }

    if (generationBefore != midiMappingGeneration.load (
            std::memory_order_acquire))
        rebuildControllerRoutesIfNeeded (true);
}

// Um unico caminho para CCs vindos da porta MIDI musical e da porta DAW do
// Launchkey. Retorna true quando o evento PERTENCE ao mapa do usuario — pelo
// membership da tabela de rotas (mesmo que o valor deste evento tenha sido
// dropado por republicacao/contencao) ou porque foi aceito pela fila de Learn.
// A superficie fixa do Launchkey e a nota crua so agem quando isto e false;
// com a tabela alem da capacidade fisica, rotas excedentes seguem fail-open.
bool MainComponent::processControllerCC (int cc, int value,
                                         const juce::String& sourceId,
                                         int channel,
                                         bool isNote,
                                         const juce::String& sourceName)
{
    if (midiMapTransactionInProgress.load (std::memory_order_acquire))
        return false;

    cc = juce::jlimit (0, 127, cc);
    value = juce::jlimit (0, 127, value);
    channel = juce::jlimit (1, 16, channel);
    if (sourceId.isEmpty()
         || (! isNote
              && ! ChurchDAWMidiMapping::isMappableController (cc)))
        return false;

    lastMidiActivityMs.store (
        juce::Time::getMillisecondCounter(),
        std::memory_order_relaxed);

    // No String value, CCTarget or map lock crosses this producer boundary.
    // The caller-owned UTF-8 remains valid for this synchronous scan; accepted
    // work copies only bounded bytes/POD into preallocated cells.
    const auto* const sourceBytes = sourceId.toRawUTF8();
    const auto sourceLength = static_cast<size_t> (
        sourceId.getNumBytesAsUTF8());
    const auto* const sourceNameBytes = sourceName.toRawUTF8();
    const auto sourceNameLength = static_cast<size_t> (
        sourceName.getNumBytesAsUTF8());
    const auto mappingGeneration = midiMappingGeneration.load (
        std::memory_order_acquire);

    // Membership independe da geracao da tabela: uma rota armada na ultima
    // publicacao continua respondendo durante a propria republicacao, entao um
    // fader/tecla mapeado nunca cai na superficie de fabrica nem toca a nota
    // crua so porque o mapa acabou de mudar. O valor de um evento que caiu na
    // janela e dropado com seguranca — o proximo evento continuo se aplica.
    const bool mapped = controllerRoutes.matchesActiveRoute (
        sourceBytes, sourceLength, channel, cc, isNote);

    if (midiLearnActive.load (std::memory_order_acquire))
    {
        const auto targetGeneration =
            parameterLearnSelectionGeneration.load (
                std::memory_order_acquire);
        const bool targetArmed = lastTouchedSlot.load (
            std::memory_order_acquire) != -2;
        // Alem da capacidade fisica, a rota pode nao estar na tabela mesmo com
        // a tecla mapeada; o consumidor decide sob mapLock com o mapa exato,
        // entao no overflow o gesto de desmapear continua funcionando.
        const bool unmapOnly = ! targetArmed && isNote
            && (mapped || controllerRoutes.overflowed());

        if (targetArmed || unmapOnly)
        {
            const bool accepted = midiLearnControlQueue.tryPublish (
                sourceBytes, sourceLength,
                sourceNameBytes, sourceNameLength,
                channel, cc, value, isNote, unmapOnly,
                targetGeneration);
            if (accepted)
                triggerAsyncUpdate();
            // An overflow only says that some route did not fit; it is not
            // membership proof for this note. Still queue the exact lookup so
            // an overflowed mapping can be removed, but let unknown notes pass
            // through raw. A known indexed route or an armed target is consumed
            // only when its bounded publication succeeded.
            return ChurchDAWMidiMapping::
                midiLearnPublicationConsumesRawInput (
                    accepted, targetArmed, mapped);
        }
    }

    const auto result = controllerRoutes.tryPublish (
        sourceBytes, sourceLength, channel, cc, isNote, value,
        mappingGeneration);
    if (result
          == ChurchDAWMidiMapping::ControllerRoutePublishResult::published)
        triggerAsyncUpdate();

    return mapped
        || result
               == ChurchDAWMidiMapping::ControllerRoutePublishResult::published;
}

void MainComponent::audioProcessorParameterChanged (juce::AudioProcessor* processor,
                                                    int parameterIndex,
                                                    float newValue)
{
    auto callbackLease = parameterCallbackGate.tryEnter();
    if (! callbackLease)
        return;

    juce::uint64 callbackTopologyGeneration = 0;
    if (! validatedCallbackTopologyGeneration (
            processor, callbackTopologyGeneration))
        return;

    // Mandatory sound synchronisation is independent from the optional Learn
    // mailbox. The callback source has already been validated by raw address;
    // use JUCE's normalised newValue and cached parameter index, so no raw
    // AudioProcessor pointer is cast or dereferenced here.
    if (padProcessor != nullptr)
        for (int slot = 0; slot < numSlots; ++slot)
            if (slots[slot].proc.load (std::memory_order_acquire) == processor
                 && slots[slot].globalTransposeParameterIndex.load (
                        std::memory_order_acquire) == parameterIndex)
            {
                padProcessor->setTransposeSemis (
                    ChurchDAWMidiMapping::
                        globalTransposeSemitonesFromNormalised (newValue));
                break;
            }

    // pode chegar em qualquer thread; usar apenas atomics
    if (midiMapTransactionInProgress.load (std::memory_order_acquire)
         || ! midiLearnActive.load() || applyingMappedCC)
        return;

    // RT boundary: no names, IDs, parameter scans, map locks, String writes,
    // clocks, plug-in location scans or AsyncUpdater calls here. A full
    // mailbox drops only this Learn touch; audio and the parameter change
    // continue normally.
    (void) parameterTouchMailbox.tryPublish (
        processor, parameterIndex,
        parameterLearnGeneration.load (std::memory_order_acquire),
        callbackTopologyGeneration);
}

// deixa o nome do aparelho apresentavel: tira "MIDIIN2 (...)", sufixos de driver
// (ASIO/USB/MIDI) e numeros soltos, pra virar algo tipo "Focusrite" / "Launchkey MK3"
static juce::String prettifyDeviceName (juce::String n)
{
    if (n.containsChar ('(') && n.containsChar (')'))
        n = n.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false);

    n = n.trim();

    // O Android entrega "Focusrite - Novation Launchkey MK4 61 Output Port 1".
    // No chip compacto isso virava "Focusrite - Nova..." e escondia justamente
    // o modelo que o musico reconhece. Mantem a quantidade de teclas (49/61),
    // mas tira fabricante e o nome tecnico da porta.
    if (const int launchkeyAt = n.indexOfIgnoreCase ("Launchkey"); launchkeyAt >= 0)
    {
        n = n.substring (launchkeyAt);

        if (const int portAt = n.indexOfIgnoreCase (" Output Port"); portAt >= 0)
            n = n.substring (0, portAt);
        else if (const int portAt = n.indexOfIgnoreCase (" Input Port"); portAt >= 0)
            n = n.substring (0, portAt);

        return n.trim();
    }

    for (const char* marker : { " Output Port", " Input Port" })
        if (const int at = n.indexOfIgnoreCase (marker); at >= 0)
            n = n.substring (0, at).trim();

    for (const char* suffix : { " ASIO", " USB ASIO", " (ASIO)", " MIDI", " USB", " Audio" })
        while (n.endsWithIgnoreCase (suffix))
            n = n.dropLastCharacters ((int) juce::String (suffix).length()).trim();

    // tira um numero solto no fim (ex.: contagem de teclas "Launchkey MK4 49"),
    // mantendo modelos como "MK4" / "2i2" / "S49" intactos
    const int sp = n.lastIndexOfChar (' ');
    if (sp > 0)
    {
        const auto lastTok = n.substring (sp + 1);
        if (lastTok.isNotEmpty() && lastTok.containsOnly ("0123456789"))
            n = n.substring (0, sp).trim();
    }

    return n.isEmpty() ? juce::String() : n;
}

void MainComponent::timerCallback()
{
    // GUI toggles / scene restores can change global sustain without a MIDI
    // event. Update telemetry under the same lock order as the MIDI callback.
    // Try-lock keeps the UI from waiting on a device callback or plugin swap.
    {
        const juce::ScopedTryLock callbackLock (midiCallbackBarrier);
        if (callbackLock.isLocked())
        {
            const juce::ScopedTryLock instrumentLock (liveHandshakeLock);
            if (instrumentLock.isLocked())
                if (auto* live = getChurchLive())
                    pianoVisualState->setHostGlobalSustain (live->getGlobalSustain());
        }
    }
    CHURCHDAW_UI_SCOPE ("MainComponent::timerCallback");

    // 25s de app aberto com o device funcionando provam que o setup atual e
    // bom: a partir daqui um driver novo que derrube o app pode ser descartado
    // com seguranca no boot seguinte.
    if (audioGoodPromoTicks >= 0 && ++audioGoodPromoTicks == 25)
        promoteGoodAudioNow();
    CHURCHDAW_UI_LAP_BEGIN();

    // Subiu para antes do bloco de MIDI: a lista de entradas agora e cacheada
    // por tempo (ver midiInputsCached) e precisa do relogio aqui em cima.
    const auto nowMs = juce::Time::getMillisecondCounter();

    // Fixed-capacity graph retirement: at most one old node per physical slot
    // can wait here. No callAsync retry chain is created; this existing timer
    // destroys it only after every insert lease/callback has left.
    for (int slot = 0; slot < numSlots; ++slot)
    {
        const auto pending = slots[slot].pendingNodeRetirement.node();
        if (pending == 0)
            continue;

        const juce::AudioProcessorGraph::NodeID nodeID { pending };
        if (auto node = graph.getNodeForId (nodeID))
        {
            auto* live = dynamic_cast<TimbresProcessor*> (
                node->getProcessor());
            if (live == nullptr
                 || live->canDestroyWithoutBlockingOnMidiMapping())
                removeNodeForSlot (slot, nodeID);
        }
        else
        {
            (void) slots[slot].pendingNodeRetirement.clearIf (pending);
        }
    }

    // MASTER FX listeners: addListener e idempotente. No Android a cada 1 s
    // competia com o audio; 3 s basta pro MIDI Learn sem jank.
    static juce::uint32 lastInsertListenerScanMs = 0;
   #if JUCE_ANDROID || JUCE_IOS
    constexpr juce::uint32 insertScanPeriodMs = 3000;
   #else
    constexpr juce::uint32 insertScanPeriodMs = 1000;
   #endif
    if (nowMs - lastInsertListenerScanMs >= insertScanPeriodMs)
    {
        lastInsertListenerScanMs = nowMs;
        for (int i = 0; i < numSlots; ++i)
            refreshInsertMappingIdentityCacheForSlot (i);
    }

    CHURCHDAW_UI_LAP ("timer:listeners");

    // Cura de NaN dos plugins hospedados. A guarda do callback so limpa o buffer
    // e levanta a bandeira -- reset() de VST3/VST2 aloca, pega mutex e re-prepara
    // streams de disco, entao nao pode sair da thread de audio. Sem pedido em
    // aberto isto custa apenas alguns loads atomicos.
    for (int i = 0; i < numSlots; ++i)
        if (auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
        {
            timbres->serviceRetiredInsertProcessors();
            timbres->serviceHostedPluginNanResets();
        }

    CHURCHDAW_UI_LAP ("timer:nan-reset");

    // A porta DAW tem de ficar FORA da entrada MIDI comum. O estado de audio
    // salvo e o proprio dialogo Audio/MIDI podem reabilita-la sem querer — e ai
    // pad volta a tocar nota e a porta fica ocupada pro LaunchkeyControl.
    //
    // A MESMA rede de seguranca do refreshMidiInputs vale aqui, e por um motivo
    // pratico: sem ela, o cliente que abrisse Audio/MIDI e marcasse a entrada
    // na mao via este timer desmarcar em menos de 1 s. Nao havia contorno
    // possivel pelo usuario — foi o que tornou a regressao da 1.5.4 tao grave.
    if (launchkeyMappingEnabled)
    {
        const auto entradas = midiInputsCached (nowMs);

        int portasDaw = 0;
        for (const auto& d : entradas)
            if (LaunchkeyControl::isDawPort (d.name))
                ++portasDaw;

        const auto launchkeyPolicy =
            ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
                launchkeyMappingEnabled,
                entradas.size(),
                portasDaw);

        if (launchkeyPolicy.reserveDawPort)
            for (const auto& d : entradas)
                if (LaunchkeyControl::isDawPort (d.name)
                     && deviceManager.isMidiInputDeviceEnabled (d.identifier))
                    deviceManager.setMidiInputDeviceEnabled (d.identifier, false);
    }

    CHURCHDAW_UI_LAP ("timer:midi-daw");

    // Hot-plug real: se o teclado entrou depois do app, o objeto criado na
    // largada nao tinha portas e nunca se conectaria sozinho. Reabre no maximo
    // a cada 5 s, sem ficar brigando com drivers de porta exclusiva.

   #if JUCE_ANDROID
    pollAndroidAudioTopology();

    CHURCHDAW_UI_LAP ("timer:android-audio-topology");
   #endif

   #if JUCE_ANDROID || JUCE_IOS
    // Tuner de fluidez na message thread. Usa APENAS o contador nativo da HAL.
    // AudioDeviceManager::getXRunCount() tambem soma deadlines perdidos pelo
    // medidor de callback; aumentar a capacidade do stream nao muda o tamanho
    // do callback Oboe e, portanto, reagir a esse segundo contador so elevaria
    // a latencia em ciclo sem corrigir a carga de CPU.
    // Um xrun isolado nao reabre o device;
    // dois confirmam pressao real e sobem um degrau. O aumento e TEMPORARIO:
    // depois de 120 s limpos e sem MIDI, recupera latencia ate a preferencia
    // manual. Nunca altera sample rate, DSP ou qualidade dos samples.
    if (nowMs - androidLastBufferTuneMs >= 8000)
    {
        auto* currentDevice = deviceManager.getCurrentAudioDevice();
        const int currentXRuns = currentDevice != nullptr
            ? juce::jmax (0, currentDevice->getXRunCount()) : 0;
        const int observedNewXRuns = currentXRuns >= androidLastXRunCount
                                        ? currentXRuns - androidLastXRunCount
                                        : currentXRuns;
        const bool warmupWindow =
            androidBufferTunerWarmupPending.exchange (
                false, std::memory_order_acq_rel);
        const int newXRuns = warmupWindow ? 0 : observedNewXRuns;
        bool bufferChanged = false;
        bool deferredMidPerformance = false;

        if (warmupWindow)
        {
            // A primeira janela apos abrir a rota mede o zero real do driver.
            // Sem isto dois underruns do proprio start do AAudio faziam 256
            // subir para 512 mesmo com uma unica nota e DSP abaixo de 30%.
            androidXRunPressure = 0;
            androidStableBufferWindows = 0;

            if (observedNewXRuns > 0)
                juce::Logger::writeToLog (
                    "AUDIO: tuner mobile descartou "
                    + juce::String (observedNewXRuns)
                    + " xrun(s) de aquecimento");
        }

        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
           #if JUCE_ANDROID
            const int savedPreference = appProperties.getUserSettings()
                ->getIntValue ("androidUserBufferSize",
                               androidPreferredBufferSize);
           #else
            const int savedPreference = appProperties.getUserSettings()
                ->getIntValue ("bufferSize",
                               androidPreferredBufferSize);
           #endif
            if (savedPreference > 0)
                androidPreferredBufferSize = savedPreference;

            if (androidAvailableBufferSizes.isEmpty())
                androidAvailableBufferSizes = device->getAvailableBufferSizes();

            const int currentSize = device->getCurrentBufferSizeSamples();
            const ChurchDAWMobileAudio::BufferTunerState previousState {
                androidXRunPressure, androidStableBufferWindows };
            const auto decision = ChurchDAWMobileAudio::decideBuffer (
                currentSize,
                androidPreferredBufferSize > 0
                    ? androidPreferredBufferSize : currentSize,
                androidAvailableBufferSizes.begin(),
                androidAvailableBufferSizes.size(), newXRuns,
                nowMs - lastMidiActivityMs.load (
                            std::memory_order_relaxed) < 10000,
                previousState,
                1024);

            androidXRunPressure = warmupWindow
                ? 0 : decision.state.xrunPressure;
            androidStableBufferWindows = warmupWindow
                ? 0 : decision.state.stableWindows;

            if (decision.requestedBuffer > 0
                 && decision.requestedBuffer != currentSize)
            {
                const bool activelyPlaying =
                    (nowMs - lastMidiActivityMs.load (std::memory_order_relaxed) < 3500)
                    || ChurchDAWAudioActivity::audibleWithin (1500);

                if (activelyPlaying && newXRuns < 24)
                {
                    // NUNCA derrube/reabra o stream no meio de uma frase musical!
                    // Mantemos a pressao armada e reavaliamos em 2 s para aplicar
                    // na primeira pausa entre frases/acordes.
                    androidXRunPressure = juce::jmax (2, previousState.xrunPressure + 1);
                    deferredMidPerformance = true;
                }
                else
                {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.bufferSize = decision.requestedBuffer;
                    const auto error =
                        deviceManager.setAudioDeviceSetup (setup, false);
                    bufferChanged = error.isEmpty();

                    if (! bufferChanged)
                    {
                        // A decisao pode ser tentada novamente na proxima janela;
                        // falha de HAL nao deve apagar a evidencia acumulada.
                        androidXRunPressure = previousState.xrunPressure;
                        androidStableBufferWindows = previousState.stableWindows;
                    }

                    const bool recovering = decision.reason
                        == ChurchDAWMobileAudio::BufferDecisionReason::stableIdleRecovery;
                    juce::Logger::writeToLog (
                        "AUDIO: tuner mobile " + juce::String (currentSize)
                        + " -> " + juce::String (decision.requestedBuffer)
                        + (recovering ? " apos 120s estaveis/sem MIDI"
                                      : " apos xruns repetidos")
                        + (error.isEmpty()
                               ? juce::String (" (temporario, preferencia=")
                                    + juce::String (androidPreferredBufferSize) + ")"
                               : " ERRO: " + error));
                }
            }
        }

        if (bufferChanged)
        {
            if (auto* reopened = deviceManager.getCurrentAudioDevice())
                androidLastXRunCount = juce::jmax (0, reopened->getXRunCount());
            else
                androidLastXRunCount = 0;
        }
        else
            androidLastXRunCount = currentXRuns;
        androidLastBufferTuneMs = deferredMidPerformance ? (nowMs - 6000) : nowMs;
    }
   #endif

    CHURCHDAW_UI_LAP ("timer:android-buffer");

    // Reconexao do Launchkey: recria quando a porta DAW NAO esta disponivel
    // (desplugado) ou quando o objeto atual nao esta conectado/ouvindo. O
    // output/input antigos continuam nao-nulos apos o replug, entao o teste
    // de disponibilidade da porta e o que detecta a remocao.
    if (launchkeyMappingEnabled
         && (launchkey == nullptr
          || ! launchkey->isConnected()
          || ! launchkey->isListening()
          || ! LaunchkeyControl::isDawPortAvailable())
         && nowMs - lastLaunchkeyRetryMs >= 5000)
    {
        lastLaunchkeyRetryMs = nowMs;
        setupLaunchkey();
    }

    CHURCHDAW_UI_LAP ("timer:launchkey-setup");

    // DIAGNOSTICO PERIODICO (10 s): saude do motor para o monitor de log.
    // Mostra steals de voz, NaN, xruns e clips — os sinais de bug que nao
    // aparecem em lugar nenhum enquanto o musico toca.
    {
        static juce::uint32 lastDiagLogMs = 0;

        if (nowMs - lastDiagLogMs >= 10000)
        {
            lastDiagLogMs = nowMs;

            juce::String diag = "DIAG:";

            int timbreProcessorCount = 0;
            juce::uint32 totalSteals = 0;
            juce::uint32 totalNan = 0;
            juce::uint32 totalStreamMisses = 0;

            for (int i = 0; i < numSlots; ++i)
                if (auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
                {
                    ++timbreProcessorCount;
                    totalSteals += timbres->getVoiceStealsInLayer();
                    totalNan += timbres->getNanGuardHitCount();
                    totalStreamMisses += timbres->getStreamMissCount();
                   #if CHURCHDAW_AUDIO_INSTRUMENTATION
                    diag += " s" + juce::String (i)
                          + "_avg=" + juce::String (timbres->getDiagAverageLoadPercent(), 1)
                          + "_peak=" + juce::String (timbres->getDiagPeakLoadPercent(), 1)
                          + "_voices=" + juce::String (timbres->getDiagVoiceLoadPercent(), 1)
                          + "_fx=" + juce::String (timbres->getDiagFxLoadPercent(), 1)
                          + "_active=" + juce::String (timbres->getDiagActiveVoices());
                   #endif
                }

            diag += " timbre_processors=" + juce::String (timbreProcessorCount);
            diag += " steals=" + juce::String (totalSteals);
            diag += " nan=" + juce::String (totalNan);
            diag += " stream_misses=" + juce::String (totalStreamMisses);

            const int totalXRuns = deviceManager.getXRunCount();
            const int driverXRuns = deviceManager.getCurrentAudioDevice() != nullptr
                ? juce::jmax (0, deviceManager.getCurrentAudioDevice()->getXRunCount())
                : 0;
            diag += " xruns=" + juce::String (totalXRuns);
            diag += " driver_xruns=" + juce::String (driverXRuns);
            diag += " callback_overruns="
                + juce::String (juce::jmax (0, totalXRuns - driverXRuns));

           #if CHURCHDAW_AUDIO_INSTRUMENTATION
            const auto measuredSamples = diagCallbackSamples.exchange (
                0, std::memory_order_relaxed);
            const auto playerTicks = diagPlayerTicks.exchange (
                0, std::memory_order_relaxed);
            const auto playbackTicks = diagPlaybackTicks.exchange (
                0, std::memory_order_relaxed);
            const double diagnosticSampleRate =
                deviceManager.getCurrentAudioDevice() != nullptr
                    ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                    : 48000.0;
            const double measuredBudget = (double) measuredSamples
                / juce::jmax (1.0, diagnosticSampleRate);

            if (measuredBudget > 0.0)
            {
                diag += " graph_load=" + juce::String (
                    100.0 * juce::Time::highResolutionTicksToSeconds (playerTicks)
                        / measuredBudget, 1);
                diag += " playback_load=" + juce::String (
                    100.0 * juce::Time::highResolutionTicksToSeconds (playbackTicks)
                        / measuredBudget, 1);
            }
           #endif

            int clips = 0;

            for (int i = 0; i < numSlots; ++i)
                if (auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
                    for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                        if (timbres->readAndResetLayerClip (l))
                            ++clips;

            diag += " clips=" + juce::String (clips);

            juce::Logger::writeToLog (diag);

           #if CHURCHDAW_AUDIO_INSTRUMENTATION
            const float idlePeak = idleOutputPeak.exchange (
                0.0f, std::memory_order_relaxed);
            const double idleSquares = idleOutputSquareSum.exchange (
                0.0, std::memory_order_relaxed);
            const auto idleSamples = idleOutputSampleCount.exchange (
                0, std::memory_order_relaxed);
            const auto idleNonFinite = idleOutputNonFiniteCount.exchange (
                0, std::memory_order_relaxed);
           #else
            constexpr float idlePeak = 0.0f;
            constexpr double idleSquares = 0.0;
            constexpr juce::uint64 idleSamples = 0;
            constexpr juce::uint64 idleNonFinite = 0;
           #endif
            const auto blockedCallbacks = blockedInputCallbackCount.exchange (
                0, std::memory_order_relaxed);
            const int blockedChannels = blockedInputChannelMaximum.exchange (
                0, std::memory_order_relaxed);

            if (idleSamples > 0 || idleNonFinite > 0
                 || blockedCallbacks > 0)
            {
                double idleRms = 0.0;
               #if CHURCHDAW_AUDIO_INSTRUMENTATION
                if (idleSamples > 0)
                    idleRms = std::sqrt (idleSquares
                                         / static_cast<double> (idleSamples));
               #endif
                const auto setup = deviceManager.getAudioDeviceSetup();
                const auto* device = deviceManager.getCurrentAudioDevice();
                const int activeInputs = device != nullptr
                    ? device->getActiveInputChannels().countNumberOfSetBits()
                    : 0;

                juce::Logger::writeToLog (
                    "AUDIO_IDLE peak=" + juce::String (idlePeak, 9)
                    + " peak_dbfs=" + juce::String (
                        juce::Decibels::gainToDecibels (
                            idlePeak, -200.0f), 2)
                    + " rms=" + juce::String (idleRms, 9)
                    + " rms_dbfs=" + juce::String (
                        juce::Decibels::gainToDecibels (
                            (float) idleRms, -200.0f), 2)
                    + " samples=" + juce::String (idleSamples)
                    + " non_finite=" + juce::String (idleNonFinite)
                    + " input_authorised=" + juce::String (
                        audioInputAuthorised.load (
                            std::memory_order_acquire) ? 1 : 0)
                    + " setup_inputs=" + juce::String (
                        setup.inputChannels.countNumberOfSetBits())
                    + " active_inputs=" + juce::String (activeInputs)
                    + " blocked_input_callbacks="
                    + juce::String (blockedCallbacks)
                    + " max_blocked_input_channels="
                    + juce::String (blockedChannels));
            }
        }
    }

    CHURCHDAW_UI_LAP ("timer:diag");

    // POLL DE RECONEXAO MIDI. O watcher WinRT (MidiDeviceListConnection) as
    // vezes perde o re-add do teclado no replug — o log mostrava a remocao
    // mas nunca o retorno, e o musico tinha que fechar e abrir o app. Este
    // poll compara a lista CRUA atual com a ultima conhecida e, se mudou,
    // chama o refreshMidiInputs (que reabre a porta e reescreve os mapas por
    // nome). Compara cru-com-cru para nunca gerar refresh em loop.
    //
    // Alem da lista, verifica se os dispositivos presentes estao REALMENTE
    // habilitados: o MidiInput::openDevice pode falhar silenciosamente na
    // primeira tentativa (driver WinRT ainda inicializando apos o replug) e
    // o log mostra "LIGADA" mesmo sem a porta aberta. Se algum dispositivo
    // nao-DAW esta presente mas nao habilitado, tenta de novo.
    {
        static juce::uint32 lastMidiPollMs = 0;

        if (nowMs - lastMidiPollMs >= 2000)
        {
            lastMidiPollMs = nowMs;

            const auto& entradas = midiInputsCached (nowMs);
            juce::StringArray ids;
            bool algumPresenteNaoHabilitado = false;

            for (const auto& d : entradas)
            {
                ids.add (d.identifier);

                if (! LaunchkeyControl::isDawPort (d.name)
                     && ! deviceManager.isMidiInputDeviceEnabled (d.identifier))
                    algumPresenteNaoHabilitado = true;
            }

            if (ids != ultimaListaMidiCrua || algumPresenteNaoHabilitado)
                refreshMidiInputs();
        }
    }

    CHURCHDAW_UI_LAP ("timer:midi-poll");

    refreshLaunchkeyNames();   // so envia o que mudou

    CHURCHDAW_UI_LAP ("timer:launchkey-names");

    const juce::String sep = "  " + juce::String::charToString ((juce::juce_wchar) 0x2022) + "  ";
    juce::String txt;

    juce::String ifaceName;
    bool ifaceOn = false;

    if (auto* device = deviceManager.getCurrentAudioDevice();
        device != nullptr && device->isOpen() && device->isPlaying())
    {
        txt << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << " kHz" << sep
            << device->getCurrentBufferSizeSamples() << " smp" << sep << "CPU "
            << juce::roundToInt (deviceManager.getCpuUsage() * 100.0) << "%";

        ifaceName = prettifyDeviceName (device->getName());
        ifaceOn = true;
        lastAudioDriverError.clear();
        statusInfo.setTooltip ({});
    }
    else
    {
        txt << (lastAudioDriverError.isEmpty() ? "Sem audio / abra Audio para reabrir" : "Falha de audio / abra Audio");
    }

    if (auto* editor = dynamic_cast<TimbresEditor*> (dockedEditor.get()))
    {
        undoMixButton.setEnabled (editor->canUndoParameterEdit());
        redoMixButton.setEnabled (editor->canRedoParameterEdit());
    }
    else { undoMixButton.setEnabled (false); redoMixButton.setEnabled (false); }
    statusInfo.setText (txt, juce::dontSendNotification);

    CHURCHDAW_UI_LAP ("timer:android-audio");

    // Visor de programa no formato do Electro 7: chapa vermelha com o canal em
    // foco, nome grande do timbre dele, e embaixo a lista dos outros canais --
    // que e como a Nord mostra as camadas empilhadas sob o programa.
    if (oled.isVisible() || qaOledChannel > 0)
    {
        juce::String slot, title (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
        std::vector<OledDisplay::Row> rows;

        if (auto* live = getChurchLive())
        {
            if (! qaOledSelectionApplied
                 && juce::isPositiveAndBelow (qaOledChannel - 1,
                                              TimbresProcessor::numLayers))
            {
                live->setSelectedLayer (qaOledChannel - 1);
                qaOledSelectionApplied = true;
            }

            const int sel = live->getSelectedLayer();
            const auto* timbresEditor =
                dynamic_cast<const TimbresEditor*> (dockedEditor.get());
            const bool masterFocused =
                timbresEditor != nullptr
                && timbresEditor->isMobileMasterPageActive();

            auto nameOf = [this, live] (int l)
            {
                auto n = live->getLayerTimbre (l);

                if (n.isEmpty())
                    n = live->getLayerInstrumentName (l);

                if (n.isEmpty() && qaOledChannel == 9)
                {
                    if (l == 7) n = "QA Nivel C8";
                    if (l == 8) n = "QA Nivel C9";
                }

                // MESMO nome limpo que a regua mostra: o visor vinha com o cru
                // ("Black Upright LP4 FC") enquanto o canal ja mostrava limpo
                return TimbresProcessor::prettyLabel (n);
            };

            int loaded = 0, on = 0;

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
            {
                const bool reallyLoaded = live->getLayerTimbre (l).isNotEmpty()
                                       || live->getLayerInstrumentName (l).isNotEmpty();

                if (reallyLoaded)
                {
                    ++loaded;

                    if (live->getLayerOn (l))
                        ++on;
                }
            }

            const auto summarySeparator = juce::String::fromUTF8 (" \xc2\xb7 ");

            // A cena no ar e a informacao mais urgente do culto e a unica que o
            // visor nunca mostrava. Prefixa no liveSummary (que alimenta os tres
            // ramos do slot) so quando ha cena carregada — cenaAtiva=-1 (sem
            // cena no ar) ou cena vazia (preenchida=false) nao produzem rotulo,
            // assim o visor parado continua limpo.
            const auto cenaPrefix =
                (juce::isPositiveAndBelow (cenaAtiva, ChurchDAWLive::numeroDeCenas)
                   && cenas[cenaAtiva].preenchida)
                    ? ChurchDAWLive::rotuloDaCena (cenas[cenaAtiva], cenaAtiva) + summarySeparator
                    : juce::String();

            const auto liveSummary = cenaPrefix
                + (loaded == 0
                     ? juce::String ("Nenhum timbre")
                     : on == loaded
                         ? juce::String (on) + " canais ativos"
                         : juce::String (on) + " / " + juce::String (loaded)
                             + " canais ativos");

            // Nenhum canal em foco: em vez de deixar a coluna oca, o visor conta
            // o estado do setup. Produto premium tem que ser bonito PARADO.
            if (masterFocused)
            {
                slot = juce::String ("MASTER") + summarySeparator + liveSummary;
                title = juce::String (
                    juce::CharPointer_UTF8 ("Sa\xc3\xad" "da principal"));
            }
            else if (sel < 0)
            {
                slot = "Church Live";
                title = liveSummary;
            }
            else
            {
                slot = juce::String ("Canal ") + juce::String (sel + 1)
                        + summarySeparator + liveSummary;
                title = nameOf (sel);

                if (title.isEmpty())
                    title = "vazio";
            }

            // O visor comporta cinco linhas. Escolhemos essas cinco aqui, em vez
            // de enviar 14 e deixar o componente cortar silenciosamente depois do
            // canal 5. O canal em foco vem primeiro na prioridade; depois entram
            // canais carregados e ligados, carregados desligados e, por fim,
            // vazios. A ordenacao final conserva a leitura natural pelo numero do
            // canal sem nunca esconder uma selecao 6..14.
            std::vector<int> visibleLayers;
            visibleLayers.reserve (5);

            auto includeLayer = [&visibleLayers] (int layer)
            {
                if (layer < 0 || layer >= TimbresProcessor::numLayers
                    || visibleLayers.size() >= 5
                    || std::find (visibleLayers.begin(), visibleLayers.end(), layer)
                           != visibleLayers.end())
                    return;

                visibleLayers.push_back (layer);
            };

            if (! masterFocused)
                includeLayer (sel);

            // Um vizinho da selecao alta fornece contexto de pagina e deixa
            // verificavel que canais 9..14 usam seu proprio mini-medidor.
            if (! masterFocused && sel >= 5)
                includeLayer (sel - 1);

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                if (nameOf (l).isNotEmpty() && live->getLayerOn (l))
                    includeLayer (l);

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                if (nameOf (l).isNotEmpty())
                    includeLayer (l);

            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                includeLayer (l);

            std::sort (visibleLayers.begin(), visibleLayers.end());

            for (const int l : visibleLayers)
            {
                auto n = nameOf (l);
                const bool empty = n.isEmpty();

                if (empty)
                    n = juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"));   // travessao

                juce::String tag;
                float dbVal = -200.0f;

                // canal vazio nao ganha tag: "—    -inf" era o mesmo nada dito duas vezes.
                // PEGADINHA: getValue() devolve o valor NORMALIZADO 0..1 -- num fader
                // de ganho 0..1.5 isso mostrava "0.0 dB" com o canal em +3.5 dB. O
                // ganho de verdade vem do get() do AudioParameterFloat.
                if (! empty)
                    if (auto* vp = dynamic_cast<juce::AudioParameterFloat*> (live->getLayerVolParam (l)))
                    {
                        const float v = vp->get();
                        dbVal = v <= 0.0001f ? -200.0f : (float) juce::Decibels::gainToDecibels (v);
                        const float db = (float) juce::Decibels::gainToDecibels (v);
                        tag = v <= 0.0001f
                            ? juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e")
                            : (db >= 0.0f ? "+" : juce::String())
                                + juce::String (db, 1) + " dB";
                    }

                // traducao categoria -> pictograma do visor. Fica AQUI porque e o
                // unico ponto que enxerga o TimbresProcessor e o OledDisplay
                const bool isPlugin = live->getLayerInstrument (l) != nullptr;
                auto icon = OledDisplay::Icon::generic;

                switch (TimbreGlyph::resolveKind (isPlugin, live->getLayerInstrumentName (l),
                                                  live->getLayerTimbre (l)))
                {
                    case TimbreGlyph::piano:                                  icon = OledDisplay::Icon::piano;   break;
                    case TimbreGlyph::organ:                                  icon = OledDisplay::Icon::organ;   break;
                    case TimbreGlyph::strings:                                icon = OledDisplay::Icon::strings; break;
                    case TimbreGlyph::pad:
                    case TimbreGlyph::ambience:                               icon = OledDisplay::Icon::pad;     break;
                    case TimbreGlyph::bells:                                  icon = OledDisplay::Icon::bells;   break;
                    case TimbreGlyph::kontaktBrand:
                    case TimbreGlyph::vstBrand:                               icon = OledDisplay::Icon::plugin;  break;
                    case TimbreGlyph::nordBrand:                              icon = OledDisplay::Icon::piano;   break;
                    default:                                                  break;
                }

                rows.push_back ({ l + 1, icon, n, tag, dbVal, empty,
                                  live->getLayerOn (l),
                                  ! masterFocused && l == sel });
            }
        }
        else
        {
            slot = "CHURCHDAW";
            title = "sem Church Live";
        }

        oled.setProgram (slot, title, std::move (rows));
    }

    CHURCHDAW_UI_LAP ("timer:oled");

    // (a faixa de teclado se alimenta sozinha a 30 Hz -- este timer e de 1 Hz)

    // chips premium: nome do controlador MIDI + interface de audio
    juce::String controllerName;
    juce::String controllerTransport;
    int midiCount = 0;

    for (const auto& d : midiInputsCached (nowMs))
    {
        if (deviceManager.isMidiInputDeviceEnabled (d.identifier))
        {
            if (controllerName.isEmpty())
            {
                controllerName = prettifyDeviceName (d.name);
                controllerTransport =
                    ChurchDAWAndroidCulto::midiTransportTag (d.name);
            }

            ++midiCount;
        }
    }

    if (midiCount > 1)
        controllerName << "  +" << (midiCount - 1);

    const int mappingCount = getMidiMappingCount();
    const bool recentMidi = nowMs - lastMidiActivityMs.load() < 1100;
    deviceBar.setControllerName (controllerName, midiCount > 0);
    deviceBar.setControllerTransport (controllerTransport);
    deviceBar.setInterfaceName (ifaceName, ifaceOn);
    deviceBar.setInterfaceTransport (
        ChurchDAWAndroidCulto::audioTransportTag (ifaceName));
    deviceBar.setMappingCount (mappingCount);
    deviceBar.setMidiActivity (recentMidi);
    midiLearnPanel.setDevice (controllerName, midiCount > 0);
    midiLearnPanel.setMappingCount (mappingCount);

    if (midiMappingPanel.isVisible())
        refreshMidiMappingPanel();

    if (performanceSessionPanel != nullptr
         && performanceSessionPanel->isVisible())
        refreshPerformanceSessionPanel();
}

void MainComponent::handleAsyncUpdate()
{
    if (! midiCallbackLifetime->load())
        return;
    if (midiMapTransactionInProgress.load (std::memory_order_acquire))
        return;

    // Both callback-facing paths publish only bounded POD records. Drain Learn
    // first because it may mutate the map/generation, then publish that exact
    // generation before applying regular controller values.
    consumeMidiLearnControlEvents();
    rebuildControllerRoutesIfNeeded();
    consumeControllerRouteEvents();

    // reset de pickup pedido pela thread de MIDI (o map e so desta thread).
    // O slot atomico e unico: dois pedidos coalescidos perderiam o primeiro
    // e o CC re-mapeado pularia o soft-takeover. Limpar o mapa inteiro cobre
    // os dois — pior caso os outros CCs so re-armam o pickup (inofensivo)
    if (ccPickupToClear.exchange (-1) >= 0)
        ccPickup.clear();

    bool mappingEventWasCompleted = false;

    int rejectedNote = -1;
    juce::String rejectedNoteReason;
    {
        const juce::ScopedLock sl (mapLock);
        rejectedNote = lastNoteActionRejected.exchange (
            -1, std::memory_order_relaxed);
        if (rejectedNote >= 0)
            rejectedNoteReason = motivoDaRecusaDeNota;
    }
    if (rejectedNote >= 0)
    {
        midiLearnPanel.showAviso (
            "Nota " + juce::String (rejectedNote) + " nao foi usada",
            rejectedNoteReason + " ja usa essa nota");
        if (midiMappingPanel.isVisible())
            midiMappingPanel.setStatus (
                "Essa nota ja controla " + rejectedNoteReason,
                MidiMappingPanel::Status::warning);
        juce::Logger::writeToLog (
            "MIDI MAP: nota " + juce::String (rejectedNote)
              + " recusada porque ja controla " + rejectedNoteReason);
        mappingEventWasCompleted = true;
    }

    int mappedNote = -1;
    NoteAction mappedAction;
    MidiControlKey mappedNoteKey;
    {
        const juce::ScopedLock sl (mapLock);
        mappedNote = lastMappedNote.exchange (-1, std::memory_order_relaxed);
        if (mappedNote >= 0)
        {
            mappedNoteKey = lastMappedNoteKey;
            mappedAction = lastMappedNoteAction;
        }
    }

    if (mappedNote >= 0)
    {
        // Se o disco recusou, a tecla funciona nesta sessao e SOME na proxima
        // abertura. Dizer "PRONTO" aqui e a pior mentira do fluxo: e o unico
        // caminho que grava varias vezes seguidas, entao o musico mapeia o
        // culto inteiro achando que esta guardado.
        const bool gravouNoDisco = saveNoteMap();

        // kind == none significa DESMAPEADA (a tecla voltou a tocar nota).
        const bool desmapeada = mappedAction.kind == NoteActionKind::none;

        // A mascara e filtro por NUMERO de nota, e a mesma nota pode continuar
        // mapeada noutro canal ou noutro teclado. So a reconstrucao sabe se o
        // bit ainda deve ficar ligado — apagar o bit direto cortaria os outros.
        //
        // Reconstroi SEMPRE, nao so no desmapeamento: lastMappedNote e um slot
        // unico e a thread de MIDI pode desmapear uma tecla e mapear outra
        // antes deste callback rodar. Olhando so a ultima acao, a reconstrucao
        // do desmapeamento era perdida e sobrava um bit ligado sem mapa —
        // inofensivo no som, mas custa alocacao e lock em toda nota daquele
        // numero. Reconstruir e barato e nao depende de adivinhar a ordem.
        reconstruirMascaraDeNotas();

        // Toda mudanca no mapa — aprender OU devolver a tecla — tem de aparecer
        // no teclado do Pad na mesma hora.
        atualizarEtiquetasDoPad();

        const auto acao = desmapeada
                            ? juce::String ("volta a tocar nota")
                            : mappedAction.kind == NoteActionKind::padOff
                                ? juce::String ("Pad Parar")
                                : "Pad " + juce::String (PadPanel::noteName (mappedAction.arg));

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);

        if (desmapeada)
        {
            midiLearnPanel.showUnmappedNote (
                mappedNote,
                gravouNoDisco ? juce::String ("Mapeamento salvo automaticamente")
                              : juce::String::fromUTF8 (
                                    "Vale s\xc3\xb3 nesta sess\xc3\xa3o \xe2\x80\x94 volta ao fechar"));
            if (midiMappingPanel.isVisible())
            {
                refreshMidiMappingPanel();
                midiMappingPanel.setStatus (
                    "Nota " + juce::String (mappedNote)
                      + (gravouNoDisco
                            ? " liberada e salva automaticamente"
                            : " liberada apenas nesta sessao"),
                    gravouNoDisco ? MidiMappingPanel::Status::success
                                  : MidiMappingPanel::Status::warning);
            }
        }
        else
        {
            midiLearnPanel.showReceipt (acao, mappedNoteKey.sourceName, mappedNoteKey.channel, mappedNote, true, gravouNoDisco,
                (!(lastLearnInputKey < mappedNoteKey) && !(mappedNoteKey < lastLearnInputKey)) ? lastLearnInputValue : -1);
            midiLearnConsoleOverlay.flashSuccess (acao + " / Nota " + juce::String (mappedNote)
                + (gravouNoDisco ? " / salvo" : " / nao salvo"), gravouNoDisco);
            if (midiMappingPanel.isVisible())
            {
                refreshMidiMappingPanel();
                midiMappingPanel.showMapped (acao, mappedNoteKey.channel, mappedNote,
                                             gravouNoDisco, true);
            }
        }
        juce::Logger::writeToLog (juce::String (desmapeada ? "MIDI MAP: DESMAPEADA nota "
                                                           : "MIDI MAP: aprendida nota ")
                                  + juce::String (mappedNote)
                                  + " canal " + juce::String (mappedNoteKey.channel)
                                  + " de " + mappedNoteKey.sourceId + " -> " + acao
                                  + (gravouNoDisco ? "" : "  [NAO SALVOU]"));
        mappingEventWasCompleted = true;
    }

    // ---- BOTAO DO PAD POR CC (pad de borracha) ----
    int ccRecusado = -1;
    juce::String motivo;
    {
        const juce::ScopedLock sl (mapLock);
        ccRecusado = lastCCAcaoRecusada.exchange (
            -1, std::memory_order_relaxed);
        if (ccRecusado >= 0)
            motivo = motivoDaRecusaDeCCAcao;
    }

    if (ccRecusado >= 0)
    {
        // A ENTRADA RECUSADA CONTINUA VIVA (so que vazia) ATE O LEARN FECHAR.
        //
        // Apagando ela aqui, a mensagem seguinte do mesmo gesto — o "soltei" do
        // pad, ou o proximo passo do knob — nao acharia mais nada e cairia no
        // processControllerCC com o Learn AINDA LIGADO, onde pode virar um
        // mapeamento de PARAMETRO que o musico nunca fez e nao viu.
        atualizarBotoesDeCCDoPad();

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        midiLearnPanel.showAviso (
            juce::String::fromUTF8 ("CC ") + juce::String (ccRecusado)
                + juce::String::fromUTF8 (" n\xc3\xa3o foi usado"),
            motivo + juce::String::fromUTF8 (
                        " \xe2\x80\x94 aperte outro pad para este tom"));
        juce::Logger::writeToLog ("MIDI MAP: CC " + juce::String (ccRecusado)
                                  + " recusado como botao do Pad (" + motivo + ")");
        mappingEventWasCompleted = true;
    }

    const int ccDesmapeado = lastCCAcaoDesmapeada.exchange (-1);

    if (ccDesmapeado >= 0)
    {
        const bool gravou = saveMidiMap();
        atualizarBotoesDeCCDoPad();

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        midiLearnPanel.showAviso (
            "CC " + juce::String (ccDesmapeado)
                + juce::String::fromUTF8 (" liberado"),
            gravou ? juce::String::fromUTF8 ("Esse pad n\xc3\xa3o comanda mais o Pad")
                   : juce::String::fromUTF8 (
                         "Vale s\xc3\xb3 nesta sess\xc3\xa3o \xe2\x80\x94 volta ao fechar"));
        juce::Logger::writeToLog ("MIDI MAP: DESMAPEADO CC " + juce::String (ccDesmapeado)
                                  + " do Pad" + (gravou ? "" : "  [NAO SALVOU]"));
        mappingEventWasCompleted = true;
    }

    int ccDeAcao = -1;
    NoteAction acaoAprendida;
    MidiControlKey chave;
    {
        const juce::ScopedLock sl (mapLock);
        ccDeAcao = lastMappedCCAcao.exchange (
            -1, std::memory_order_relaxed);
        if (ccDeAcao >= 0)
        {
            chave = lastMappedCCAcaoKey;
            acaoAprendida = lastMappedCCAcaoAction;
        }
    }

    if (ccDeAcao >= 0)
    {
        // Mesma regra da nota e do CC de parametro: a tela nao confirma um mapa
        // que nao foi para o disco.
        const bool gravou = saveMidiMap();

        reconstruirMascaraDeCCsDeAcao();
        atualizarBotoesDeCCDoPad();

        const auto alvo = acaoAprendida.kind == NoteActionKind::padOff
                            ? juce::String ("Pad Parar")
                            : "Pad " + juce::String (
                                  PadPanel::noteName (acaoAprendida.arg));

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        midiLearnPanel.showReceipt (alvo, chave.sourceName, chave.channel, ccDeAcao, false, gravou,
                (!(lastLearnInputKey < chave) && !(chave < lastLearnInputKey)) ? lastLearnInputValue : -1);
        midiLearnConsoleOverlay.flashSuccess (alvo + " / CC " + juce::String (ccDeAcao)
                + (gravou ? " / salvo" : " / nao salvo"), gravou);
        if (midiMappingPanel.isVisible())
        {
            refreshMidiMappingPanel();
            midiMappingPanel.showMapped (alvo, chave.channel, ccDeAcao, gravou);
        }
        juce::Logger::writeToLog ("MIDI MAP: aprendido CC " + juce::String (ccDeAcao)
                                  + " canal " + juce::String (chave.channel)
                                  + " de " + chave.sourceId + " -> " + alvo
                                  + (gravou ? "" : "  [NAO SALVOU]"));
        mappingEventWasCompleted = true;
    }

    int rejectedParameterCC = -1;
    juce::String rejectedParameterReason;
    {
        const juce::ScopedLock sl (mapLock);
        rejectedParameterCC = lastParameterMappingRejectedCC.exchange (
            -1, std::memory_order_relaxed);
        if (rejectedParameterCC >= 0)
            rejectedParameterReason = motivoDaRecusaDeParametro;
    }
    if (rejectedParameterCC >= 0)
    {
        midiLearnPanel.showAviso (
            "Controle " + juce::String (rejectedParameterCC)
                + " nao foi mapeado",
            rejectedParameterReason);
        if (midiMappingPanel.isVisible())
            midiMappingPanel.setStatus (
                rejectedParameterReason,
                MidiMappingPanel::Status::warning);
        juce::Logger::writeToLog (
            "MIDI MAP: controle " + juce::String (rejectedParameterCC)
              + " recusado: " + rejectedParameterReason);
        mappingEventWasCompleted = true;
    }

    int mappedCC = -1;
    CCTarget mappedTarget;
    MidiControlKey mappedKey;
    bool mappingStillExists = false;
    {
        const juce::ScopedLock sl (mapLock);
        mappedCC = lastMappedCC.exchange (-1, std::memory_order_relaxed);
        if (mappedCC >= 0)
        {
            mappedKey = lastMappedKey;
            const auto it = ccMappings.find (mappedKey);
            if (it != ccMappings.end())
            {
                mappedTarget = it->second;
                mappingStillExists = true;
            }
        }
    }

    if (mappedCC >= 0)
    {
        // MESMA REGRA DA NOTA, 50 linhas acima: o painel nao pode confirmar um
        // mapa que nao foi para o disco. Este e o caminho que grava MAIS vezes
        // seguidas — o musico mapeia knob por knob no ensaio — e era o unico
        // que ainda dizia "PRONTO" sem olhar o resultado. Com o arquivo
        // recusado (versao futura, pasta somente-leitura, antivirus), ele
        // reaprendia tudo e perdia tudo de novo na proxima abertura.
        const bool gravouCC = saveMidiMap();

        const auto alvo = describeCCTarget (mappedTarget);
        const auto targetName = alvo.isNotEmpty() ? alvo : juce::String ("controle");

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        if (mappingStillExists)
        {
            midiLearnPanel.showReceipt (targetName, mappedKey.sourceName, mappedKey.channel, mappedCC, mappedKey.isNote, gravouCC,
                (!(lastLearnInputKey < mappedKey) && !(mappedKey < lastLearnInputKey)) ? lastLearnInputValue : -1);
            midiLearnConsoleOverlay.flashSuccess (targetName + " / CC " + juce::String (mappedCC)
                + (gravouCC ? " / salvo" : " / nao salvo"), gravouCC);
            if (midiMappingPanel.isVisible())
            {
                refreshMidiMappingPanel();
                midiMappingPanel.showMapped (targetName, mappedKey.channel,
                                             mappedCC, gravouCC, mappedKey.isNote);
            }
        }
        else if (midiMappingPanel.isVisible())
        {
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                gravouCC
                    ? "Mapeamento removido e salvo automaticamente"
                    : "Mapeamento removido apenas nesta sessao",
                gravouCC ? MidiMappingPanel::Status::success
                         : MidiMappingPanel::Status::warning);
        }
        mappingEventWasCompleted = true;
    }

    if (! midiLearnActive.load())
        return;

    int slot = -2;
    int param = -1;
    int insert = -1;
    juce::String touchedName;
    {
        // Slot, indice, insert e identidade formam uma unica publicacao. Ler
        // tudo sob mapLock impede que um callback de parametro mais novo seja
        // combinado com metade do alvo anterior.
        const juce::ScopedLock sl (mapLock);
        slot = lastTouchedSlot.load (std::memory_order_relaxed);
        param = lastTouchedParam.load (std::memory_order_relaxed);
        insert = lastTouchedInsert.load (std::memory_order_relaxed);
        touchedName = lastTouchedParamName;
    }

    // A confirmacao de um mapa nao deve apagar um toque de parametro mais novo
    // que foi coalescido no mesmo AsyncUpdater. Se nao existe novo alvo, a
    // mensagem de sucesso/aviso acima permanece visivel.
    if (mappingEventWasCompleted && slot == -2)
        return;

    juce::String name = touchedName;

    if (slot == -1)
        name = "Master";
    else if (slot == padSlotId && name.isNotEmpty()
              && ! name.startsWithIgnoreCase ("Pad"))
        name = "Pad  " + name;
    else if (slot >= 0 && slot < numSlots && insert >= 0)
    {
        juce::String identity;
        if (auto* processor = resolveCurrentMidiMappingProcessor (
                slot, insert, identity))
            name = processor->getName() + " " + name;
    }

    juce::ignoreUnused (param);

    if (name.isNotEmpty())
        midiLearnPanel.showMoveHardware (name);
}

// SOFT TAKEOVER (pickup). Um fader fisico nao sabe onde o parametro esta: se o
// piano esta em -6 dB e o fader do teclado esta la em cima, o primeiro toque
// joga o canal pra 0 dB no meio do louvor. Aqui o CC so "pega" o parametro
// quando o valor fisico ALCANCA o valor atual -- ate la, nao mexe em nada.
//
// Encoders relativos nao passam por isto (nao tem posicao absoluta), e o
// Launchkey ja resolve os faders dele em hardware; isto protege o resto.
bool MainComponent::ccHasPickedUp (const MidiControlKey& control,
                                   float incoming, float current)
{
    auto& st = ccPickup[control];

    if (st.engaged)
        return true;

    constexpr float tolerance = 0.02f;

    // encostou no valor atual, ou passou por cima dele desde a ultima mensagem
    const bool touching = std::abs (incoming - current) <= tolerance;
    const bool crossed = st.hasLast
                          && ((st.last <= current && incoming >= current)
                               || (st.last >= current && incoming <= current));

    st.last = incoming;
    st.hasLast = true;

    if (touching || crossed)
    {
        st.engaged = true;
        return true;
    }

    return false;
}

void MainComponent::refreshInsertMappingIdentityCacheForSlot (int slot)
{
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return;

    auto* timbres = dynamic_cast<TimbresProcessor*> (
        slots[slot].proc.load());
    for (int insertIndex = 0;
         insertIndex < TimbresProcessor::numInserts; ++insertIndex)
    {
        juce::AudioProcessor* insert = nullptr;
        juce::String identity;
        const auto generationBefore = parameterTargetGeneration.load (
            std::memory_order_acquire);
        if (timbres != nullptr)
            (void) timbres->getInsertMappingIdentitySnapshot (
                insertIndex, insert, identity);
        const auto generationAfter = parameterTargetGeneration.load (
            std::memory_order_acquire);
        if (generationBefore != generationAfter)
        {
            insert = nullptr;
            identity.clear();
        }

        {
            const juce::ScopedLock mappingLock (mapLock);
            auto& snapshot =
                insertMappingIdentityCache[slot][insertIndex];
            snapshot.processor = insert;
            snapshot.identity = identity;
            snapshot.topologyGeneration = generationAfter;
        }

        if (insert != nullptr)
        {
            insertParameterCallbackRoutes[slot][insertIndex].publish (
                insert, generationAfter);
            insert->addListener (this);
        }
        else
        {
            insertParameterCallbackRoutes[slot][insertIndex].invalidate();
        }
    }
}

void MainComponent::retireInsertParameterListener (
    TimbresProcessor* owner, int insert,
    juce::AudioPluginInstance* processor)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (processor == nullptr
         || ! juce::isPositiveAndBelow (
                insert, numHostedInsertMappingCaches))
        return;

    bool invalidated = false;
    for (int slot = 0; slot < numSlots; ++slot)
    {
        if (owner != nullptr
             && slots[slot].proc.load (std::memory_order_acquire) != owner)
            continue;

        if (! insertParameterCallbackRoutes[slot][insert].invalidate (
                processor))
            continue;

        invalidated = true;
        const juce::ScopedLock mappingLock (mapLock);
        auto& snapshot = insertMappingIdentityCache[slot][insert];
        if (snapshot.processor == processor)
        {
            snapshot.processor = nullptr;
            snapshot.identity.clear();
            snapshot.topologyGeneration = 0;
        }
    }

    // During outer TimbresProcessor destruction the slot pointer may already
    // be unpublished. Fall back to the exact raw route across all fixed slots;
    // this remains bounded and still validates before dereferencing processor.
    if (! invalidated)
        for (int slot = 0; slot < numSlots; ++slot)
            if (insertParameterCallbackRoutes[slot][insert].invalidate (
                    processor))
            {
                invalidated = true;
                const juce::ScopedLock mappingLock (mapLock);
                auto& snapshot = insertMappingIdentityCache[slot][insert];
                if (snapshot.processor == processor)
                {
                    snapshot.processor = nullptr;
                    snapshot.identity.clear();
                    snapshot.topologyGeneration = 0;
                }
            }

    if (invalidated)
        processor->removeListener (this);
}

bool MainComponent::validatedCallbackTopologyGeneration (
    const juce::AudioProcessor* processor,
    juce::uint64& topologyGeneration) const noexcept
{
    if (processor == nullptr)
        return false;

    if (processor == masterProcessor
         || processor == padProcessor
         || processor == padSynthProcessor)
    {
        topologyGeneration = parameterTargetGeneration.load (
            std::memory_order_acquire);
        return true;
    }

    for (int slot = 0; slot < numSlots; ++slot)
        if (slots[slot].proc.load (std::memory_order_acquire) == processor)
        {
            topologyGeneration = parameterTargetGeneration.load (
                std::memory_order_acquire);
            return slots[slot].proc.load (std::memory_order_acquire)
                    == processor;
        }

    for (int slot = 0; slot < numSlots; ++slot)
        for (int insert = 0;
             insert < numHostedInsertMappingCaches; ++insert)
        {
            ChurchDAWMidiMapping::ParameterCallbackRoute::Snapshot snapshot;
            if (insertParameterCallbackRoutes[slot][insert]
                    .snapshotIfCurrent (processor, snapshot))
            {
                topologyGeneration = snapshot.topologyGeneration;
                return true;
            }
        }

    return false;
}

juce::String MainComponent::cachedProcessorMappingIdentity (
    int slot, int insert, const juce::AudioProcessor* expected) const
{
    if (slot == -1)
        return ChurchDAWMidiMapping::masterProcessorIdentity();
    if (slot == padSlotId)
        return ChurchDAWMidiMapping::padProcessorIdentity();
    if (slot == padSynthSlotId)
        return ChurchDAWMidiMapping::padSynthProcessorIdentity();
    if (! juce::isPositiveAndBelow (slot, numSlots))
        return {};

    const juce::ScopedLock mappingLock (mapLock);
    if (insert >= 0)
    {
        if (! juce::isPositiveAndBelow (
                insert, TimbresProcessor::numInserts))
            return {};
        const auto& snapshot = insertMappingIdentityCache[slot][insert];
        return snapshot.topologyGeneration
                    == parameterTargetGeneration.load (
                        std::memory_order_acquire)
                && (expected == nullptr || snapshot.processor == expected)
            ? snapshot.identity : juce::String();
    }

    return expected == nullptr || slots[slot].proc.load() == expected
        ? slots[slot].mappingIdentity : juce::String();
}

juce::AudioProcessor* MainComponent::resolveCurrentMidiMappingProcessor (
    int slot, int insert, juce::String& identity) const
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    identity.clear();

    switch (ChurchDAWMidiMapping::classifyMidiTargetLocation (
        slot, insert, numSlots, padSlotId, padSynthSlotId))
    {
        case ChurchDAWMidiMapping::MidiTargetLocationKind::master:
            identity = ChurchDAWMidiMapping::masterProcessorIdentity();
            return masterProcessor;
        case ChurchDAWMidiMapping::MidiTargetLocationKind::pad:
            identity = ChurchDAWMidiMapping::padProcessorIdentity();
            return padProcessor;
        case ChurchDAWMidiMapping::MidiTargetLocationKind::padSynth:
            identity = ChurchDAWMidiMapping::padSynthProcessorIdentity();
            return padSynthProcessor;
        case ChurchDAWMidiMapping::MidiTargetLocationKind::slot:
        {
            auto* processor = slots[slot].proc.load (std::memory_order_acquire);
            identity = cachedProcessorMappingIdentity (slot, -1, processor);
            return processor;
        }
        case ChurchDAWMidiMapping::MidiTargetLocationKind::insert:
        {
            auto* live = dynamic_cast<TimbresProcessor*> (
                slots[slot].proc.load (std::memory_order_acquire));
            juce::AudioProcessor* processor = nullptr;
            if (live != nullptr)
                (void) live->getInsertMappingIdentitySnapshot (
                    insert, processor, identity);
            return processor;
        }
        case ChurchDAWMidiMapping::MidiTargetLocationKind::invalid:
            break;
    }

    return nullptr;
}

void MainComponent::consumeMidiLearnParameterMailbox()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    auto ownerFrame = ownerTeardownGate != nullptr
        ? ownerTeardownGate->tryEnter()
        : ChurchDAWMidiMapping::DeferredOwnerTeardownGate::Lease {};
    if (! ownerFrame)
        return;

    if (! midiLearnActive.load (std::memory_order_acquire)
         || componentShutdownStarted.load (std::memory_order_acquire))
        return;

    // Toque dropado por mailbox cheio e fail-open para o Learn, mas nao pode
    // ser invisivel: o musico ve "mova o alvo de novo" no log em vez de um
    // gesto que simplesmente nao aprendeu.
    if (const auto drops = parameterTouchMailbox.droppedCount();
        drops != parameterTouchDropsReported)
    {
        parameterTouchDropsReported = drops;
        juce::Logger::writeToLog (
            "MIDI MAP: mailbox de toques de parametro cheio; gesto seguiu "
            "fail-open (total " + juce::String (drops) + ")");
    }

    bool published = false;
    ChurchDAWMidiMapping::ParameterTouchEvent event;
    while (parameterTouchMailbox.tryConsume (event))
    {
        // Plug-ins echo a mapped/programmatic change shortly after it was
        // applied. The clock query belongs on the message thread, never in the
        // AudioProcessorListener callback.
        if (juce::Time::getMillisecondCounter()
              < suppressTouchUntilMs.load (std::memory_order_acquire))
            continue;

        const auto learnGeneration = parameterLearnGeneration.load (
            std::memory_order_acquire);
        const auto topologyGeneration = parameterTargetGeneration.load (
            std::memory_order_acquire);
        if (! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                event, learnGeneration, topologyGeneration))
            continue;

        auto* const touchedProcessor = reinterpret_cast<juce::AudioProcessor*> (
            event.processor);
        int slot = -2;
        int insert = -1;
        juce::String processorIdentity;
        TimbresProcessor* owningLive = nullptr;
        juce::AudioPluginInstance* leasedInsert = nullptr;

        if (touchedProcessor == masterProcessor)
        {
            slot = -1;
            processorIdentity = ChurchDAWMidiMapping::masterProcessorIdentity();
        }
        else if (touchedProcessor == padProcessor)
        {
            slot = padSlotId;
            processorIdentity = ChurchDAWMidiMapping::padProcessorIdentity();
        }
        else if (touchedProcessor == padSynthProcessor)
        {
            slot = padSynthSlotId;
            processorIdentity = ChurchDAWMidiMapping::padSynthProcessorIdentity();
        }
        else
        {
            for (int candidateSlot = 0;
                 candidateSlot < numSlots && slot == -2; ++candidateSlot)
            {
                auto* const candidate = slots[candidateSlot].proc.load (
                    std::memory_order_acquire);
                if (candidate == touchedProcessor)
                {
                    slot = candidateSlot;
                    processorIdentity = cachedProcessorMappingIdentity (
                        candidateSlot, -1, touchedProcessor);
                    owningLive = dynamic_cast<TimbresProcessor*> (candidate);
                    break;
                }

                auto* const live = dynamic_cast<TimbresProcessor*> (candidate);
                if (live == nullptr)
                    continue;

                for (int candidateInsert = 0;
                     candidateInsert < TimbresProcessor::numInserts;
                     ++candidateInsert)
                {
                    auto* currentInsert =
                        live->acquireInsertForMidiMapping (candidateInsert);
                    if (currentInsert != touchedProcessor)
                    {
                        if (currentInsert != nullptr)
                            live->releaseInsertForMidiMapping (
                                candidateInsert, currentInsert);
                        continue;
                    }

                    slot = candidateSlot;
                    insert = candidateInsert;
                    processorIdentity = cachedProcessorMappingIdentity (
                        candidateSlot, candidateInsert, currentInsert);
                    owningLive = live;
                    leasedInsert = currentInsert;
                    break;
                }
            }
        }

        const juce::ScopeGuard releaseInsertLease {
            [owningLive, leasedInsert, insert]
            {
                if (owningLive != nullptr && leasedInsert != nullptr)
                    owningLive->releaseInsertForMidiMapping (
                        insert, leasedInsert);
            }
        };

        // The stale pointer is never dereferenced before it is proven to be a
        // current route. A swap/unload also bumps the generation synchronously,
        // closing the allocator-address-reuse (ABA) case.
        if (slot == -2 || processorIdentity.isEmpty()
             || topologyGeneration != parameterTargetGeneration.load (
                    std::memory_order_acquire)
             || learnGeneration != parameterLearnGeneration.load (
                    std::memory_order_acquire)
             || (owningLive != nullptr
                  && owningLive->isProgrammaticToneChange()))
            continue;

        const auto& parameters = touchedProcessor->getParameters();
        if (! juce::isPositiveAndBelow (event.parameterIndex,
                                        parameters.size())
             || parameters[event.parameterIndex] == nullptr)
            continue;

        auto name = parameters[event.parameterIndex]->getName (32);
        auto id = ChurchDAWMidiMapping::stableParameterId (
            parameters[event.parameterIndex]);
        const bool isMaster = slot == -1;
        if (isMaster)
        {
            name = "Master";
            id = "master";
        }
        const bool uniqueIdentity = isMaster
            || ChurchDAWMidiMapping::parameterIdentityIsUnique (
                parameters, event.parameterIndex);

        {
            const juce::ScopedLock sl (mapLock);
            if (midiMapTransactionInProgress.load (
                    std::memory_order_acquire)
                 || ! midiLearnActive.load (std::memory_order_acquire)
                 || applyingMappedCC.load (std::memory_order_acquire)
                 || topologyGeneration != parameterTargetGeneration.load (
                        std::memory_order_acquire)
                 || learnGeneration != parameterLearnGeneration.load (
                        std::memory_order_acquire))
                continue;

            lastTouchedParamName = std::move (name);
            lastTouchedParamId = std::move (id);
            lastTouchedPluginId = std::move (processorIdentity);
            lastTouchedSlot.store (slot, std::memory_order_relaxed);
            lastTouchedParam.store (
                isMaster ? -1 : event.parameterIndex,
                std::memory_order_relaxed);
            lastTouchedInsert.store (insert, std::memory_order_relaxed);
            lastTouchedTopologyGeneration.store (
                topologyGeneration, std::memory_order_relaxed);
            lastTouchedParameterIdentityUnique.store (
                uniqueIdentity, std::memory_order_relaxed);
            published = true;
        }
    }

    if (published)
        triggerAsyncUpdate();
}

void MainComponent::applyCCTarget (CCTarget target, float value01,
                                   MidiControlKey control)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto ownerFrame = ownerTeardownGate != nullptr
        ? ownerTeardownGate->tryEnter()
        : ChurchDAWMidiMapping::DeferredOwnerTeardownGate::Lease {};
    if (! ownerFrame)
        return;

    suppressTouchUntilMs.store (juce::Time::getMillisecondCounter() + 600);
    applyingMappedCC = true;
    bool valid = false;
    bool applied = false;
    bool pickupPending = false;
    auto targetName = describeCCTarget (target);

    if (target.slot == -1
         && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId,
                ChurchDAWMidiMapping::masterProcessorIdentity(),
                masterProcessor != nullptr)
              == ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
    {
        valid = true;
        const float cur = (float) masterSlider.valueToProportionOfLength (masterSlider.getValue());

        if (! control.isValid()
             || ccHasPickedUp (control, value01, cur))
        {
            masterSlider.setValue (masterSlider.proportionOfLengthToValue ((double) value01),
                                   juce::sendNotificationSync);
            applied = true;
        }
        else
        {
            pickupPending = true;
        }
    }
    else if (target.slot == padSlotId
              && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                    target.pluginId,
                    ChurchDAWMidiMapping::padProcessorIdentity(),
                    padProcessor != nullptr)
                   == ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
    {
        // O PAD NAO E UM SLOT.
        //
        // Ele e um no proprio do grafo (padNode), ligado direto ao master, e
        // nunca entrou em slots[]. Sem este ramo, expor parametros no
        // PadProcessor nao servia de nada: nao havia como o MIDI Learn mirar
        // neles. Cliente pediu justamente um knob de corte mapeavel (01/08).
        if (padProcessor != nullptr)
        {
            const auto& params = padProcessor->getParameters();
            const int index = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);

            if (juce::isPositiveAndBelow (index, params.size()))
            {
                valid = true;
                auto* p = params[index];
                const float cur = p->getValue();
                // Chaves (mute/solo/on/rotary/etc.) nao tem posicao absoluta: o
                // soft-takeover as matava no primeiro toque apos cada troca de
                // cena (ccPickup.clear()), quando o valor fisico (0 ou 1) ainda
                // nao "alcançou" o alvo. Bypassa o pickup para parametros
                // discretos ou de 2 passos.
                const bool ehChave = p->isDiscrete() || p->getNumSteps() <= 2;

                if (! control.isValid() || ehChave
                     || ccHasPickedUp (control, value01, cur))
                {
                    p->setValueNotifyingHost (value01);
                    applied = true;

                    if (padPanel != nullptr)
                        padPanel->refreshFromProcessor();
                }
                else
                {
                    pickupPending = true;
                }
            }
        }
    }
    else if (target.slot == padSynthSlotId
              && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                    target.pluginId,
                    ChurchDAWMidiMapping::padSynthProcessorIdentity(),
                    padSynthProcessor != nullptr)
                   == ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
    {
        if (padSynthProcessor != nullptr)
        {
            const auto& params = padSynthProcessor->getParameters();
            const int index = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);
            if (juce::isPositiveAndBelow (index, params.size()))
            {
                // Um mapa antigo pode apontar diretamente para o bool do
                // engine. A aba e o som do Pad Synth foram desativados; os
                // quatro macros visiveis continuam mapeaveis normalmente.
                if (ChurchDAWMidiMapping::stableParameterId (params[index])
                        == "padEngineEnabled")
                {
                    padSynthProcessor->setEngineEnabled (false);
                    applyingMappedCC = false;
                    return;
                }

                valid = true;
                auto* p = params[index];
                const float cur = p->getValue();
                const bool isSwitch = p->isDiscrete() || p->getNumSteps() <= 2;
                if (! control.isValid() || isSwitch
                     || ccHasPickedUp (control, value01, cur))
                {
                    p->setValueNotifyingHost (value01);
                    applied = true;
                    if (padEditorPanel != nullptr)
                        padEditorPanel->refreshFromProcessor();
                }
                else
                    pickupPending = true;
            }
        }
    }
    else if (target.slot >= 0 && target.slot < numSlots)
    {
        juce::String currentIdentity;
        TimbresProcessor* insertOwner = nullptr;
        juce::AudioPluginInstance* leasedInsert = nullptr;
        juce::AudioProcessor* proc = nullptr;
        if (target.insert >= 0)
        {
            insertOwner = dynamic_cast<TimbresProcessor*> (
                slots[target.slot].proc.load (std::memory_order_acquire));
            if (insertOwner != nullptr)
                leasedInsert = insertOwner->acquireInsertForMidiMapping (
                    target.insert);
            proc = leasedInsert;
            currentIdentity = cachedProcessorMappingIdentity (
                target.slot, target.insert, proc);
        }
        else
        {
            proc = resolveCurrentMidiMappingProcessor (
                target.slot, -1, currentIdentity);
        }
        const juce::ScopeGuard releaseInsertLease {
            [insertOwner, leasedInsert, insert = target.insert]
            {
                if (insertOwner != nullptr && leasedInsert != nullptr)
                    insertOwner->releaseInsertForMidiMapping (
                        insert, leasedInsert);
            }
        };

        const auto pluginMatch =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId, currentIdentity, proc != nullptr);

        if (pluginMatch
                == ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
        {
            const auto& params = proc->getParameters();
            const int index = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);

            if (index >= 0)
            {
                valid = true;
                auto* p = params[index];
                const float cur = p->getValue();
                // Mesmo bypass de pickup do ramo do Pad: chaves (mute/solo/on/
                // rotaryMode) nao podem exigir que o footswitch "alcance" o
                // valor atual — o primeiro toque apos trocar de cena tem que
                // comandar.
                const bool ehChave = p->isDiscrete() || p->getNumSteps() <= 2;

                if (! control.isValid()
                     || ehChave
                     || ccHasPickedUp (control, value01, cur))
                {
                    p->setValueNotifyingHost (value01);
                    applied = true;
                }
                else
                {
                    pickupPending = true;
                }
            }
        }
    }

    applyingMappedCC = false;

    if (! control.isValid())
        return;

    const auto now = juce::Time::getMillisecondCounter();
    const int ccNumber = control.cc;
    const int state = applied ? 2 : pickupPending ? 1 : 0;
    const bool shouldLog = lastMidiDiagnosticCC != ccNumber
                            || lastMidiDiagnosticChannel != control.channel
                            || lastMidiDiagnosticSource != control.sourceId
                            || lastMidiDiagnosticState != state
                            || now - lastMidiDiagnosticMs >= 1500;

    if (shouldLog)
    {
        lastMidiDiagnosticCC = ccNumber;
        lastMidiDiagnosticChannel = control.channel;
        lastMidiDiagnosticSource = control.sourceId;
        lastMidiDiagnosticState = state;
        lastMidiDiagnosticMs = now;

        juce::Logger::writeToLog ("MIDI MAP: CC " + juce::String (ccNumber)
                                  + " canal " + juce::String (control.channel)
                                  + " de " + control.sourceId
                                  + " -> " + (targetName.isNotEmpty() ? targetName
                                                                      : juce::String ("alvo invalido"))
                                  + "  valor=" + juce::String (juce::roundToInt (value01 * 127.0f))
                                  + (applied ? "  APLICADO"
                                             : pickupPending ? "  aguardando PICKUP"
                                                             : valid ? "  ignorado" : "  ALVO AUSENTE"));
    }

    deviceBar.setMidiActivity (true);

    // Logo apos aprender chegam varios valores do mesmo movimento. Preserva a
    // confirmacao verde por um instante; no movimento seguinte a faixa comprova
    // que o mapa esta realmente comandando o parametro.
    if (midiLearnActive.load() && now - lastLearnedAtMs.load() >= 800)
    {
        if (applied)
        {
            midiLearnPanel.showActivity (targetName, ccNumber,
                                         juce::roundToInt (value01 * 127.0f));
            if (midiMappingPanel.isVisible())
                midiMappingPanel.showActivity (
                    targetName, control.channel, ccNumber,
                    juce::roundToInt (value01 * 127.0f));
        }
        else if (pickupPending)
        {
            midiLearnPanel.showPickup (targetName, ccNumber);
            if (midiMappingPanel.isVisible())
                midiMappingPanel.showPickup (targetName, control.channel, ccNumber);
        }
    }
}

juce::String MainComponent::describeCCTarget (const CCTarget& target) const
{
    if (target.slot == -1)
    {
        const auto match =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId,
                ChurchDAWMidiMapping::masterProcessorIdentity(),
                masterProcessor != nullptr);
        return match == ChurchDAWMidiMapping::ProcessorTargetMatch::ready
            ? juce::String ("Master")
            : juce::String ("Master  - revalidar mapa antigo");
    }

    // O Pad tem slot proprio (padSlotId) porque nao mora na cadeia de slots.
    // Ele ganhou caminho no applyCCTarget e no audioProcessorParameterChanged,
    // mas nao aqui — entao o mapa funcionava e mesmo assim o painel dizia
    // "controle", o log dizia "alvo invalido" e a faixa de atividade aparecia
    // sem nome. Metade de uma feature e pior que nenhuma: o usuario mapeia,
    // ve um nome generico e nao confia que pegou.
    if (target.slot == padSlotId)
    {
        juce::String nome = target.paramName;
        const auto identityMatch =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId,
                ChurchDAWMidiMapping::padProcessorIdentity(),
                padProcessor != nullptr);

        // Resolve pelo mesmo caminho dos slots, em vez de comparar com nomes
        // fixos: paramName aqui e o rotulo de exibicao, que muda se o
        // parametro for renomeado — e um literal desatualizado voltaria a
        // mostrar o generico sem ninguem perceber.
        if (padProcessor != nullptr)
        {
            const auto& params = padProcessor->getParameters();
            const int resolved = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);

            if (juce::isPositiveAndBelow (resolved, params.size()))
                nome = params[resolved]->getName (32);
        }

        if (nome.isEmpty())
            return "Pad";

        auto result = nome.startsWithIgnoreCase ("Pad") ? nome : "Pad  " + nome;
        if (identityMatch != ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
            result += "  - revalidar";
        return result;
    }

    if (target.slot == padSynthSlotId)
    {
        juce::String name = target.paramName;
        const auto identityMatch =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId,
                ChurchDAWMidiMapping::padSynthProcessorIdentity(),
                padSynthProcessor != nullptr);
        if (padSynthProcessor != nullptr)
        {
            const auto& params = padSynthProcessor->getParameters();
            const int resolved = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);
            if (juce::isPositiveAndBelow (resolved, params.size()))
                name = params[resolved]->getName (64);
        }
        auto result = name.isNotEmpty() ? name : juce::String ("PAD Engine");
        if (identityMatch != ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
            result += "  - revalidar";
        return result;
    }

    if (! juce::isPositiveAndBelow (target.slot, numSlots))
        return {};

    juce::String currentIdentity;
    auto* proc = resolveCurrentMidiMappingProcessor (
        target.slot, target.insert, currentIdentity);

    juce::String parameterName = target.paramName;
    bool parameterAvailable = false;
    const auto pluginMatch =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
            target.pluginId, currentIdentity, proc != nullptr);

    if (proc != nullptr)
    {
        if (pluginMatch
              == ChurchDAWMidiMapping::ProcessorTargetMatch::ready)
        {
            const auto& params = proc->getParameters();
            const int resolved = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);

            if (juce::isPositiveAndBelow (resolved, params.size()))
            {
                parameterName = params[resolved]->getName (32);
                parameterAvailable = true;
            }
        }
    }

    if (parameterName.isEmpty())
        parameterName = "Parametro";

    if (parameterAvailable)
        return target.insert >= 0 ? proc->getName() + "  " + parameterName
                                  : parameterName;

    return parameterName + "  - revalidar / alvo pendente";
}

int MainComponent::getMidiMappingCount() const
{
    const juce::ScopedLock sl (mapLock);

    // Entradas ESVAZIADAS (desmapeadas dentro do Learn, ainda vivas so para
    // engolir o resto do gesto) nao contam. O numero na tela e justamente o que
    // o musico olha para conferir que o gesto pegou; se ele nao baixasse depois
    // de desmapear, a tela estaria dizendo o oposto do que aconteceu.
    int botoes = 0;

    for (const auto& par : ccActionMappings)
        if (par.second.acao.kind != NoteActionKind::none)
            ++botoes;

    return (int) (ccMappings.size() + noteMappings.size()) + botoes;
}

void MainComponent::refreshMidiMappingPanel()
{
    if (! midiMappingPanel.isVisible())
        return;

    juce::Array<MidiMappingPanel::Command> commands;
    std::vector<std::pair<MidiControlKey, CCTarget>>
        consumedParameterMappings;
    auto* live = getChurchLive();
    int liveSlot = -1;

    if (live != nullptr)
        for (int slot = 0; slot < numSlots; ++slot)
            if (slots[slot].proc.load() == live)
            {
                liveSlot = slot;
                break;
            }

    auto describeDevice = [this] (const MidiControlKey& key)
    {
        auto name = prettifyDeviceName (
            ChurchDAWMidiMapping::sourceNameForPersistence (
                key.sourceName, nomeDoAparelho (key.sourceId)));
        if (name.isEmpty())
            name = key.sourceId.fromFirstOccurrenceOf ("midi:", false, false);
        return name;
    };

    auto fillParameterMapping = [this, &describeDevice,
                                 &consumedParameterMappings]
        (MidiMappingPanel::Command& command, int slot,
         juce::AudioProcessorParameter* parameter)
    {
        if (parameter == nullptr)
            return;

        const auto stableId = ChurchDAWMidiMapping::stableParameterId (parameter);
        const auto name = parameter->getName (32);
        juce::AudioProcessor* processor = nullptr;
        if (slot == -1)
            processor = masterProcessor;
        else if (slot == padSlotId)
            processor = padProcessor;
        else if (slot == padSynthSlotId)
            processor = padSynthProcessor;
        else if (juce::isPositiveAndBelow (slot, numSlots))
            processor = slots[slot].proc.load();

        CCTarget currentTarget {
            slot, processor != nullptr
                    ? processor->getParameters().indexOf (parameter) : -1,
            -1, name, stableId,
            cachedProcessorMappingIdentity (slot, -1, processor)
        };
        if (slot == -1)
        {
            currentTarget.param = -1;
            currentTarget.paramName = "Master";
            currentTarget.paramId = "master";
        }
        if (currentTarget.pluginId.isEmpty())
            return;

        command.parameterIndex = currentTarget.param;
        command.parameterInsert = currentTarget.insert;
        command.parameterId = currentTarget.paramId;
        command.parameterName = currentTarget.paramName;
        command.expectedCurrentPluginId = currentTarget.pluginId;
        command.expectedParameterEntryPresent = false;

        MidiControlKey mappedKey;
        CCTarget mappedTarget;
        bool found = false;

        {
            const juce::ScopedLock sl (mapLock);
            for (const auto& pair : ccMappings)
            {
                if (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameTarget (
                        pair.second, currentTarget))
                {
                    mappedKey = pair.first;
                    mappedTarget = pair.second;
                    found = true;
                    break;
                }
            }
        }

        if (! found)
            return;

        command.mapped = true;
        command.type = mappedKey.isNote ? "Nota" : "CC";
        command.midiChannel = mappedKey.channel;
        command.number = mappedKey.cc;
        command.device = describeDevice (mappedKey);
        command.control = (mappedKey.isNote ? "Nota " : "CC ")
                            + juce::String (mappedKey.cc);
        command.expectedParameterEntryPresent = true;
        command.persistedParameterSlot = mappedTarget.slot;
        command.persistedParameterIndex = mappedTarget.param;
        command.persistedParameterInsert = mappedTarget.insert;
        command.persistedParameterId = mappedTarget.paramId;
        command.persistedParameterName = mappedTarget.paramName;
        command.persistedPluginId = mappedTarget.pluginId;
        command.mappedSourceId = mappedKey.sourceId;
        command.mappedSourceName = mappedKey.sourceName;
        command.mappedChannel = mappedKey.channel;
        command.mappedNumber = mappedKey.cc;
        command.mappedWasNote = mappedKey.isNote;
        consumedParameterMappings.emplace_back (
            mappedKey, mappedTarget);
    };

    auto fillActionMapping = [this, &describeDevice]
        (MidiMappingPanel::Command& command, NoteAction wanted)
    {
        MidiControlKey mappedKey;
        bool found = false;
        bool isNote = false;
        bool conflict = false;
        int mappedMode = 1;

        const auto matches = [&wanted] (const NoteAction& value)
        {
            const bool argumentMatters = wanted.kind == NoteActionKind::cena
                                      || wanted.kind == NoteActionKind::padKey
                                      || wanted.kind == NoteActionKind::padSynthProgram;
            return value.kind == wanted.kind
                    && (! argumentMatters || value.arg == wanted.arg);
        };

        {
            const juce::ScopedLock sl (mapLock);
            for (const auto& pair : noteMappings)
                if (matches (pair.second))
                {
                    if (! found)
                    {
                        mappedKey = pair.first;
                        found = true;
                        isNote = true;
                    }
                    else
                        conflict = true;
                }

            for (const auto& pair : ccActionMappings)
                if (matches (pair.second.acao))
                {
                    if (! found)
                    {
                        mappedKey = pair.first;
                        found = true;
                        isNote = false;
                        mappedMode = pair.second.modo
                                       == ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel
                                     ? 4 : 3;
                    }
                    else
                        conflict = true;
                }
        }

        if (! found)
            return;

        command.mapped = true;
        command.conflict = conflict;
        command.type = isNote ? "Nota" : "CC";
        command.midiChannel = mappedKey.channel;
        command.number = mappedKey.cc;
        command.mode = mappedMode;
        command.device = describeDevice (mappedKey);
        command.control = (isNote ? "Nota " : "CC ") + juce::String (mappedKey.cc);
    };

    auto addParameter = [&commands, &fillParameterMapping]
        (juce::String name, juce::String category,
         MidiMappingPanel::TargetKind kind, int argument,
         int slot, juce::AudioProcessorParameter* parameter,
         int rowDisambiguator = -1)
    {
        MidiMappingPanel::Command command;
        command.name = std::move (name);
        command.category = std::move (category);
        command.target = kind;
        command.argument = argument;
        command.parameterSlot = slot;
        command.parameterId = ChurchDAWMidiMapping::stableParameterId (parameter);
        command.parameterName = parameter != nullptr
            ? parameter->getName (32) : juce::String();
        command.rowDisambiguator = rowDisambiguator;
        fillParameterMapping (command, slot, parameter);
        commands.add (std::move (command));
    };

    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
        addParameter ("Volume  Canal " + juce::String (layer + 1), "Mixer",
                      MidiMappingPanel::TargetKind::layerVolume, layer,
                      liveSlot, live != nullptr ? live->getLayerVolParam (layer) : nullptr);

    // Acesso rapido: estes sao os dois knobs que existem no modal Pad
    // Continuo. Eles ficavam depois de dezenas de sends/efeitos e pareciam
    // inexistentes sem uma busca exata. Logo apos os faders de canal, os tres
    // alvos reclamados pelo cliente ficam juntos na primeira tela da lista.
    juce::AudioProcessorParameter* padCutoff = nullptr;
    juce::AudioProcessorParameter* padVolume = nullptr;
    if (padProcessor != nullptr)
        for (auto* parameter : padProcessor->getParameters())
        {
            const auto id = ChurchDAWMidiMapping::stableParameterId (parameter);
            if (id == "padCutoff") padCutoff = parameter;
            if (id == "padVolume") padVolume = parameter;
        }

    addParameter (juce::String::fromUTF8 ("Volume do Pad Cont\xc3\xadnuo"), "Pad / Trigger",
                  MidiMappingPanel::TargetKind::padVolume, -1, padSlotId, padVolume);
    addParameter (juce::String::fromUTF8 ("Corte do Pad Cont\xc3\xadnuo"), "Pad / Trigger",
                  MidiMappingPanel::TargetKind::padCutoff, -1, padSlotId, padCutoff);

    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
        addParameter ("Reverb  Canal " + juce::String (layer + 1), "Efeitos",
                      MidiMappingPanel::TargetKind::layerReverb, layer,
                      liveSlot, live != nullptr ? live->getLayerReverbSendParam (layer) : nullptr);

    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
        addParameter (juce::String::fromUTF8 ("Compress\xc3\xa3o  Canal ")
                        + juce::String (layer + 1),
                      "Efeitos", MidiMappingPanel::TargetKind::layerComp, layer,
                      liveSlot, live != nullptr ? live->getLayerCompParam (layer) : nullptr);

    // A mesa da referencia expoe muito mais que os tres controles historicos.
    // Todos abaixo sao os parametros reais do Church Live; entram no mesmo
    // mecanismo de pickup, conflito, importacao e persistencia dos faders.
    for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
    {
        const auto suffix = "  Canal " + juce::String (layer + 1);
        // Com o motor ausente todos os 12 controles viram placeholder com
        // parameterId vazio; o ordinal impede a selecao restaurada de saltar
        // para o primeiro controle do canal.
        int liveControlOrdinal = 0;
        auto addLive = [&] (const juce::String& name, const juce::String& category,
                            juce::AudioProcessorParameter* parameter)
        {
            addParameter (name + suffix, category,
                          MidiMappingPanel::TargetKind::processorParameter,
                          layer, liveSlot, parameter, liveControlOrdinal++);
        };

        addLive ("Liga / desliga", "Mixer", live != nullptr ? live->getLayerOnParam (layer) : nullptr);
        addLive ("Mute", "Mixer", live != nullptr ? live->getLayerMuteParam (layer) : nullptr);
        addLive ("Solo", "Mixer", live != nullptr ? live->getLayerSoloParam (layer) : nullptr);
        addLive ("Oitava", "Instrumentos", live != nullptr ? live->getLayerOctaveParam (layer) : nullptr);
        addLive ("Transpose", "Instrumentos", live != nullptr ? live->getLayerTransposeParam (layer) : nullptr);
        addLive ("Attack", "Instrumentos", live != nullptr ? live->getLayerAttackParam (layer) : nullptr);
        addLive ("Release", "Instrumentos", live != nullptr ? live->getLayerReleaseParam (layer) : nullptr);
        addLive ("EQ grave", "Efeitos", live != nullptr ? live->getLayerLowEqParam (layer) : nullptr);
        addLive ("EQ medio", "Efeitos", live != nullptr ? live->getLayerMidEqParam (layer) : nullptr);
        addLive ("EQ agudo", "Efeitos", live != nullptr ? live->getLayerHighEqParam (layer) : nullptr);
        addLive ("Rotary modo", "Efeitos", live != nullptr ? live->getLayerRotaryModeParam (layer) : nullptr);
        addLive ("Rotary intensidade", "Efeitos", live != nullptr ? live->getLayerRotaryAmountParam (layer) : nullptr);
    }

    if (live != nullptr)
    {
        addParameter ("Transpose global", "Instrumentos",
                      MidiMappingPanel::TargetKind::processorParameter, -1,
                      liveSlot, live->getTransposeGeralParam());
        addParameter ("Retorno de reverb", "Efeitos",
                      MidiMappingPanel::TargetKind::processorParameter, -1,
                      liveSlot, live->getRetornoDeReverbParam());
    }

    auto* masterParameter = masterProcessor != nullptr
                              && ! masterProcessor->getParameters().isEmpty()
                            ? masterProcessor->getParameters()[0] : nullptr;
    addParameter ("Volume Master", "Mixer",
                  MidiMappingPanel::TargetKind::masterVolume, -1, -1, masterParameter);

    if (padSynthProcessor != nullptr)
        for (auto* parameter : padSynthProcessor->getParameters())
        {
            if (parameter == nullptr)
                continue;

            const auto id = ChurchDAWMidiMapping::stableParameterId (parameter);
            // O Pad Synth nao faz mais parte da aba PAD. Somente os quatro
            // macros que continuam visiveis na sidebar e em Performance entram
            // na central de mapeamento.
            if (! id.startsWithIgnoreCase ("padMacro"))
                continue;

            juce::String category = "Pad / Trigger";
            if (id.startsWithIgnoreCase ("padMacro")
                 || id.startsWithIgnoreCase ("padXY"))
                category = "Macros";
            else if (id.containsIgnoreCase ("Chorus")
                      || id.containsIgnoreCase ("Reverb")
                      || id.containsIgnoreCase ("Delay")
                      || id.containsIgnoreCase ("Tone"))
                category = "Efeitos";
            else if (id.startsWithIgnoreCase ("padL"))
                category = "Instrumentos";

            addParameter (parameter->getName (80), category,
                          MidiMappingPanel::TargetKind::processorParameter,
                          parameter->getParameterIndex(), padSynthSlotId,
                          parameter);
        }

    auto addAction = [&commands, &fillActionMapping]
        (juce::String name, juce::String category,
         MidiMappingPanel::TargetKind kind, int argument, NoteAction action)
    {
        MidiMappingPanel::Command command;
        command.name = std::move (name);
        command.category = std::move (category);
        command.target = kind;
        command.argument = argument;
        fillActionMapping (command, action);
        commands.add (std::move (command));
    };

    addAction ("PANIC", "Transporte", MidiMappingPanel::TargetKind::panic, -1,
               { NoteActionKind::panic, -1 });
    addAction (juce::String::fromUTF8 ("Pr\xc3\xb3xima cena"),
               juce::String::fromUTF8 ("Live / Sess\xc3\xa3o"),
               MidiMappingPanel::TargetKind::nextScene, -1,
               { NoteActionKind::nextScene, -1 });
    addAction ("Cena anterior", juce::String::fromUTF8 ("Live / Sess\xc3\xa3o"),
               MidiMappingPanel::TargetKind::previousScene, -1,
               { NoteActionKind::previousScene, -1 });

    for (int scene = 0; scene < ChurchDAWLive::numeroDeCenas; ++scene)
    {
        auto sceneName = cenas[scene].nome.trim();
        if (sceneName.isEmpty())
            sceneName = "Cena " + juce::String (scene + 1);

        addAction ("Carregar  " + sceneName,
                   juce::String::fromUTF8 ("Live / Sess\xc3\xa3o"),
                   MidiMappingPanel::TargetKind::scene, scene,
                   { NoteActionKind::cena, scene });
    }

    addAction ("Tap tempo", "Transporte",
               MidiMappingPanel::TargetKind::tapTempo, -1,
               { NoteActionKind::tapTempo, -1 });
    addAction ("Alternar sustain", "Transporte",
               MidiMappingPanel::TargetKind::sustainToggle, -1,
               { NoteActionKind::sustainToggle, -1 });

    // Every persisted parameter mapping must be manageable. Built-in rows
    // consume their exact map above; active hosted plug-ins/inserts and every
    // pending/unresolved map receive a row here instead of disappearing.
    std::vector<std::pair<MidiControlKey, CCTarget>> persistedParameterMappings;
    {
        const juce::ScopedLock sl (mapLock);
        persistedParameterMappings.reserve (ccMappings.size());
        for (const auto& mapping : ccMappings)
            persistedParameterMappings.push_back (mapping);
    }

    for (const auto& mapping : persistedParameterMappings)
    {
        const auto& key = mapping.first;
        const auto& target = mapping.second;
        const bool consumed = std::any_of (
            consumedParameterMappings.begin(),
            consumedParameterMappings.end(),
            [&mapping] (const auto& candidate)
            {
                return ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                           candidate.first, mapping.first)
                    && ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                           candidate.second, mapping.second);
            });

        juce::String currentIdentity;
        auto* processor = resolveCurrentMidiMappingProcessor (
            target.slot, target.insert, currentIdentity);
        const auto identityState =
            ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                target.pluginId, currentIdentity, processor != nullptr);

        int resolvedIndex = -1;
        if (processor != nullptr)
            resolvedIndex =
                ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                    processor->getParameters(), target.paramId,
                    target.paramName,
                    ChurchDAWMidiMapping::classifyMidiTargetLocation (
                        target.slot, target.insert, numSlots,
                        padSlotId, padSynthSlotId));
        const bool parameterResolved = processor != nullptr
            && juce::isPositiveAndBelow (
                resolvedIndex, processor->getParameters().size());
        const auto rowKind =
            ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                consumed, identityState, parameterResolved);
        if (rowKind
              == ChurchDAWMidiMapping::PersistedTargetPanelRow::consumedByBuiltInRow)
            continue;

        MidiMappingPanel::Command command;
        juce::String currentParameterName = target.paramName;
        juce::String currentParameterId = target.paramId;
        if (parameterResolved)
        {
            auto* parameter = processor->getParameters()[resolvedIndex];
            currentParameterName = parameter->getName (32);
            currentParameterId = ChurchDAWMidiMapping::stableParameterId (
                parameter);
        }

        if (rowKind == ChurchDAWMidiMapping::PersistedTargetPanelRow::active)
        {
            command.name = (processor != nullptr ? processor->getName()
                                                  : juce::String ("Plugin"))
                         + "  -  " + currentParameterName;
            command.category = target.insert >= 0
                ? "Plugins / Inserts"
                : target.pluginId.startsWith ("hosted:v2:")
                    ? "Plugins externos" : "Outros controles";
        }
        else if (rowKind
                  == ChurchDAWMidiMapping::PersistedTargetPanelRow::unavailable)
        {
            command.name = "Mapa pendente (motor indisponivel)  - "
                         + target.paramName;
            command.category = "Mapas pendentes";
        }
        else if (rowKind
                  == ChurchDAWMidiMapping::PersistedTargetPanelRow::unresolvedParameter)
        {
            command.name = "Mapa pendente (parametro nao encontrado)  - "
                         + target.paramName;
            command.category = "Mapas pendentes";
        }
        else
        {
            command.name = target.pluginId.isEmpty()
                ? "Revalidar mapa antigo  - " + target.paramName
                : "Revalidar identidade  - " + target.paramName;
            command.category = "Revalidar mapas";
        }
        command.target = MidiMappingPanel::TargetKind::processorParameter;
        command.parameterSlot = target.slot;
        command.parameterIndex = parameterResolved ? resolvedIndex
                                                   : target.param;
        command.parameterInsert = target.insert;
        command.parameterId = currentParameterId;
        command.parameterName = currentParameterName;
        command.expectedCurrentPluginId = currentIdentity;
        command.expectedParameterEntryPresent = true;
        command.persistedParameterSlot = target.slot;
        command.persistedParameterIndex = target.param;
        command.persistedParameterInsert = target.insert;
        command.persistedParameterId = target.paramId;
        command.persistedParameterName = target.paramName;
        command.persistedPluginId = target.pluginId;
        command.mappedSourceId = key.sourceId;
        command.mappedSourceName = key.sourceName;
        command.mappedChannel = key.channel;
        command.mappedNumber = key.cc;
        command.mappedWasNote = key.isNote;
        command.type = key.isNote ? "Nota" : "CC";
        command.midiChannel = key.channel;
        command.number = key.cc;
        command.device = describeDevice (key);
        command.control = (key.isNote ? "Nota " : "CC ")
                            + juce::String (key.cc);
        command.mapped = true;
        command.needsRelink = rowKind
                == ChurchDAWMidiMapping::PersistedTargetPanelRow::needsRelink
            && parameterResolved
            && currentIdentity.isNotEmpty();
        commands.add (std::move (command));
    }

    const auto& devices = midiInputsCached (juce::Time::getMillisecondCounter());
    juce::String connectedName;
    for (const auto& device : devices)
        if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
        {
            connectedName = prettifyDeviceName (device.name);
            break;
        }

    midiMappingPanel.setDevice (connectedName, connectedName.isNotEmpty());
    midiMappingPanel.setMappingCount (getMidiMappingCount());
    midiMappingPanel.setCommands (std::move (commands));
}

bool MainComponent::validateCurrentParameterCommand (
    const MidiMappingPanel::Command& command,
    const CCTarget& currentTarget,
    juce::String& failureReason) const
{
    ChurchDAWMidiMapping::ParameterCommandSnapshot expected;
    expected.selectedTarget = {
        command.parameterSlot,
        command.parameterIndex,
        command.parameterInsert,
        command.parameterName,
        command.parameterId,
        command.expectedCurrentPluginId
    };
    expected.expectedCurrentPluginId = command.expectedCurrentPluginId;
    expected.expectedEntryPresent =
        command.expectedParameterEntryPresent;
    expected.persistedControl = {
        command.mappedSourceId,
        command.mappedChannel,
        command.mappedNumber,
        command.mappedWasNote,
        command.mappedSourceName
    };
    expected.persistedTarget = {
        command.persistedParameterSlot,
        command.persistedParameterIndex,
        command.persistedParameterInsert,
        command.persistedParameterName,
        command.persistedParameterId,
        command.persistedPluginId
    };

    ChurchDAWMidiMapping::ParameterCommandObservation current;
    juce::String observedPluginId;
    auto* const observedProcessor =
        resolveCurrentMidiMappingProcessor (
            currentTarget.slot, currentTarget.insert,
            observedPluginId);
    current.currentPluginId = observedPluginId;
    current.selectedTarget = currentTarget;
    current.selectedTarget.pluginId = observedPluginId;

    if (observedProcessor != nullptr && observedPluginId.isNotEmpty())
    {
        if (currentTarget.slot == -1 && currentTarget.insert < 0)
        {
            current.selectedTarget = {
                -1, -1, -1, "Master", "master", observedPluginId
            };
        }
        else
        {
            const auto& parameters = observedProcessor->getParameters();
            const auto observedIndex =
                ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                    parameters, currentTarget.paramId,
                    currentTarget.paramName,
                    ChurchDAWMidiMapping::classifyMidiTargetLocation (
                        currentTarget.slot, currentTarget.insert,
                        numSlots, padSlotId, padSynthSlotId));
            if (juce::isPositiveAndBelow (
                    observedIndex, parameters.size())
                 && parameters[observedIndex] != nullptr)
            {
                current.selectedTarget = {
                    currentTarget.slot,
                    observedIndex,
                    currentTarget.insert,
                    parameters[observedIndex]->getName (32),
                    ChurchDAWMidiMapping::stableParameterId (
                        parameters[observedIndex]),
                    observedPluginId
                };
            }
            else
            {
                // Preserve location/plugin for diagnostics while making the
                // selected target comparison fail closed.
                current.selectedTarget.param = -1;
                current.selectedTarget.paramName.clear();
                current.selectedTarget.paramId.clear();
            }
        }
    }

    {
        const juce::ScopedLock sl (mapLock);
        if (expected.expectedEntryPresent)
        {
            const auto existing = ccMappings.find (
                expected.persistedControl);
            if (existing != ccMappings.end()
                 && ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                        existing->first, expected.persistedControl))
            {
                current.entryPresent = true;
                current.persistedControl = existing->first;
                current.persistedTarget = existing->second;
            }
        }
        else
        {
            const auto existing = std::find_if (
                ccMappings.begin(), ccMappings.end(),
                [&current] (const auto& mapping)
                {
                    return ChurchDAWMidiMapping::MidiTargetIdentityPolicy::
                        sameTarget (
                            mapping.second, current.selectedTarget);
                });
            if (existing != ccMappings.end())
            {
                current.entryPresent = true;
                current.persistedControl = existing->first;
                current.persistedTarget = existing->second;
            }
        }
    }

    const auto result =
        ChurchDAWMidiMapping::validateParameterCommandSnapshot (
            expected, current);
    switch (result)
    {
        case ChurchDAWMidiMapping::ParameterCommandValidation::valid:
            failureReason.clear();
            return true;
        case ChurchDAWMidiMapping::ParameterCommandValidation::
                currentProcessorChanged:
        case ChurchDAWMidiMapping::ParameterCommandValidation::
                selectedTargetChanged:
            failureReason =
                "O plugin ou parametro mudou desde a selecao";
            break;
        case ChurchDAWMidiMapping::ParameterCommandValidation::
                entryPresenceChanged:
        case ChurchDAWMidiMapping::ParameterCommandValidation::
                persistedEntryChanged:
            failureReason =
                "O mapa mudou desde a selecao";
            break;
    }
    return false;
}

bool MainComponent::armMidiLearnParameter (juce::AudioProcessor* processor,
                                           int parameterIndex,
                                           juce::Component* visualTarget,
                                           const juce::String& customName)
{
    if (processor == nullptr || ! midiLearnActive.load())
        return false;

    // Tags are only lookup keys. Resolve ownership before dereferencing a
    // processor: a plugin may have been replaced since its UI was created.
    int slot = -2;
    int insert = -1;
    juce::String processorIdentity;

    if (processor == masterProcessor)
    {
        slot = -1;
        processorIdentity = ChurchDAWMidiMapping::masterProcessorIdentity();
    }
    else if (processor == padProcessor)
    {
        slot = padSlotId;
        processorIdentity = ChurchDAWMidiMapping::padProcessorIdentity();
    }
    else if (processor == padSynthProcessor)
    {
        slot = padSynthSlotId;
        processorIdentity = ChurchDAWMidiMapping::padSynthProcessorIdentity();
    }
    else
    {
        for (int s = 0; s < numSlots; ++s)
        {
            if (slots[s].proc.load (std::memory_order_acquire) == processor)
            {
                slot = s;
                processorIdentity = cachedProcessorMappingIdentity (s, -1, processor);
                break;
            }
        }

        if (slot == -2)
        {
            if (auto* live = getChurchLive())
            {
                if (processor == live)
                {
                    slot = 0;
                    processorIdentity = cachedProcessorMappingIdentity (0, -1, live);
                }
                else
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        juce::AudioProcessor* insertProc = nullptr;
                        juce::String insertId;
                        if (live->getInsertMappingIdentitySnapshot (i, insertProc, insertId)
                             && insertProc == processor)
                        {
                            slot = 0;
                            insert = i;
                            processorIdentity = insertId;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (slot == -2)
        return false;

    const auto& parameters = processor->getParameters();
    if (! juce::isPositiveAndBelow (parameterIndex, parameters.size())
         || parameters[parameterIndex] == nullptr)
        return false;
    auto* param = parameters[parameterIndex];

    const bool isMaster = (slot == -1);
    juce::String name = customName.isNotEmpty() ? customName : param->getName (32);
    juce::String id = ChurchDAWMidiMapping::stableParameterId (param);
    if (isMaster)
    {
        name = "Master Volume";
        id = "master";
    }

    const bool uniqueIdentity = isMaster
        || ChurchDAWMidiMapping::parameterIdentityIsUnique (parameters, parameterIndex);

    {
        const juce::ScopedLock sl (mapLock);
        lastTouchedParamName = name;
        lastTouchedParamId = id;
        lastTouchedPluginId = processorIdentity;
        lastTouchedSlot.store (slot, std::memory_order_relaxed);
        lastTouchedParam.store (isMaster ? -1 : parameterIndex, std::memory_order_relaxed);
        lastTouchedInsert.store (insert, std::memory_order_relaxed);
        lastTouchedTopologyGeneration.store (
            parameterTargetGeneration.load (std::memory_order_acquire),
            std::memory_order_relaxed);
        lastTouchedParameterIdentityUnique.store (
            uniqueIdentity, std::memory_order_relaxed);
        pendingNoteAction = {};
        parameterLearnSelectionGeneration.fetch_add (1, std::memory_order_release);
    }

    midiLearnPanel.showMoveHardware (name);
    midiLearnConsoleOverlay.setTarget (visualTarget, name);

    juce::Logger::writeToLog ("MIDI MAP: controle selecionado: " + name
                              + " (slot=" + juce::String (slot)
                              + " param=" + juce::String (parameterIndex) + ")");
    return true;
}

bool MainComponent::armMidiLearnAction (int actionKind,
                                        int arg,
                                        const juce::String& displayName,
                                        juce::Component* visualTarget,
                                        juce::Rectangle<int> visualBounds)
{
    if (actionKind <= 0)
        return false;

    {
        const juce::ScopedLock sl (mapLock);
        lastTouchedSlot.store (-2, std::memory_order_relaxed);
        lastTouchedParam.store (-1, std::memory_order_relaxed);
        lastTouchedInsert.store (-1, std::memory_order_relaxed);
        lastTouchedParamName.clear();
        lastTouchedParamId.clear();
        lastTouchedPluginId.clear();
        lastTouchedParameterIdentityUnique.store (false, std::memory_order_relaxed);
        pendingNoteAction = { (NoteActionKind) actionKind, arg };
        parameterLearnSelectionGeneration.fetch_add (1, std::memory_order_release);
    }

    juce::String desc = displayName;
    if (desc.isEmpty())
    {
        switch ((NoteActionKind) actionKind)
        {
            case NoteActionKind::panic: desc = "Panic"; break;
            case NoteActionKind::sustainToggle: desc = "Sustain"; break;
            case NoteActionKind::padOff: desc = "Pad Parar"; break;
            case NoteActionKind::cena: desc = "Cena " + juce::String (arg + 1); break;
            case NoteActionKind::padKey: desc = "Pad " + juce::String (PadPanel::noteName (arg)); break;
            case NoteActionKind::nextScene: desc = "Próxima Cena"; break;
            case NoteActionKind::previousScene: desc = "Cena Anterior"; break;
            case NoteActionKind::tapTempo: desc = "Tap Tempo"; break;
            default: desc = "Ação"; break;
        }
    }

    midiLearnPanel.showPlayNote (desc);
    midiLearnConsoleOverlay.setTarget (visualTarget, desc, visualBounds);

    juce::Logger::writeToLog ("MIDI MAP: ação selecionada: " + desc
                              + " (kind=" + juce::String (actionKind)
                              + " arg=" + juce::String (arg) + ")");
    return true;
}

void MainComponent::armMidiMappingCommand (const MidiMappingPanel::Command& command)
{
    auto armParameter = [this, &command]
        (int slot, int insert, juce::AudioProcessor* processor,
         juce::AudioProcessorParameter* parameter)
    {
        if (processor == nullptr || parameter == nullptr)
        {
            midiMappingPanel.setStatus (
                "Este alvo fica disponivel quando o motor correspondente estiver carregado",
                MidiMappingPanel::Status::warning);
            return false;
        }

        const int index = processor->getParameters().indexOf (parameter);
        if (index < 0)
            return false;

        const bool uniqueIdentity =
            ChurchDAWMidiMapping::parameterIdentityIsUnique (
                processor->getParameters(), index);
        juce::String processorIdentity;
        auto* const currentProcessor = resolveCurrentMidiMappingProcessor (
            slot, insert, processorIdentity);
        if (processorIdentity.isEmpty() || currentProcessor != processor)
        {
            midiMappingPanel.setStatus (
                "A identidade deste motor ainda nao esta pronta; tente novamente",
                MidiMappingPanel::Status::warning);
            return false;
        }

        CCTarget currentTarget {
            slot,
            slot == -1 ? -1 : index,
            insert,
            slot == -1 ? juce::String ("Master")
                       : parameter->getName (32),
            slot == -1 ? juce::String ("master")
                       : ChurchDAWMidiMapping::stableParameterId (parameter),
            processorIdentity
        };
        juce::String staleReason;
        if (! validateCurrentParameterCommand (
                command, currentTarget, staleReason))
        {
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                staleReason + "; escolha o alvo novamente",
                MidiMappingPanel::Status::warning);
            return false;
        }

        {
            const juce::ScopedLock sl (mapLock);
            lastTouchedParamName = parameter->getName (32);
            lastTouchedParamId = ChurchDAWMidiMapping::stableParameterId (parameter);
            lastTouchedPluginId = processorIdentity;
            lastTouchedSlot.store (slot, std::memory_order_relaxed);
            lastTouchedParam.store (
                slot == -1 ? -1 : index, std::memory_order_relaxed);
            lastTouchedInsert.store (insert, std::memory_order_relaxed);
            lastTouchedTopologyGeneration.store (
                parameterTargetGeneration.load (
                    std::memory_order_acquire),
                std::memory_order_relaxed);
            lastTouchedParameterIdentityUnique.store (
                uniqueIdentity, std::memory_order_relaxed);
            pendingNoteAction = {};
        }
        midiMappingPanel.setStatus (
            midiLearnActive.load()
                ? "Mova agora o controle MIDI para  " + command.name
                : "Alvo selecionado. Ative o modo de aprendizado para mapear.",
            midiLearnActive.load() ? MidiMappingPanel::Status::learning
                                   : MidiMappingPanel::Status::neutral);
        return true;
    };

    auto* live = getChurchLive();
    int liveSlot = -1;
    if (live != nullptr)
        for (int slot = 0; slot < numSlots; ++slot)
            if (slots[slot].proc.load() == live)
            {
                liveSlot = slot;
                break;
            }

    switch (command.target)
    {
        case MidiMappingPanel::TargetKind::layerVolume:
            (void) armParameter (liveSlot, -1, live,
                                 live != nullptr ? live->getLayerVolParam (command.argument) : nullptr);
            return;
        case MidiMappingPanel::TargetKind::layerReverb:
            (void) armParameter (liveSlot, -1, live,
                                 live != nullptr ? live->getLayerReverbSendParam (command.argument) : nullptr);
            return;
        case MidiMappingPanel::TargetKind::layerComp:
            (void) armParameter (liveSlot, -1, live,
                                 live != nullptr ? live->getLayerCompParam (command.argument) : nullptr);
            return;
        case MidiMappingPanel::TargetKind::padCutoff:
        case MidiMappingPanel::TargetKind::padVolume:
            if (padProcessor != nullptr)
                for (auto* parameter : padProcessor->getParameters())
                {
                    const auto id = ChurchDAWMidiMapping::stableParameterId (parameter);
                    const bool wanted = command.target == MidiMappingPanel::TargetKind::padCutoff
                                          ? id == "padCutoff" : id == "padVolume";
                    if (wanted)
                    {
                        (void) armParameter (
                            padSlotId, -1, padProcessor, parameter);
                        return;
                    }
                }
            break;
        case MidiMappingPanel::TargetKind::masterVolume:
            if (masterProcessor != nullptr && ! masterProcessor->getParameters().isEmpty())
                (void) armParameter (
                    -1, -1, masterProcessor,
                    masterProcessor->getParameters()[0]);
            return;
        case MidiMappingPanel::TargetKind::processorParameter:
        {
            juce::String currentIdentity;
            auto* processor = resolveCurrentMidiMappingProcessor (
                command.parameterSlot, command.parameterInsert,
                currentIdentity);

            if (processor != nullptr
                 && (command.parameterId.isNotEmpty()
                      || command.parameterName.isNotEmpty()))
            {
                const auto& parameters = processor->getParameters();
                const int index =
                    ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                        parameters, command.parameterId,
                        command.parameterName,
                        ChurchDAWMidiMapping::classifyMidiTargetLocation (
                            command.parameterSlot, command.parameterInsert,
                            numSlots, padSlotId, padSynthSlotId));
                if (juce::isPositiveAndBelow (index, parameters.size()))
                {
                    (void) armParameter (
                        command.parameterSlot, command.parameterInsert,
                        processor, parameters[index]);
                    return;
                }
            }
            break;
        }
        default:
            break;
    }

    NoteAction action;
    switch (command.target)
    {
        case MidiMappingPanel::TargetKind::panic:         action = { NoteActionKind::panic, -1 }; break;
        case MidiMappingPanel::TargetKind::nextScene:     action = { NoteActionKind::nextScene, -1 }; break;
        case MidiMappingPanel::TargetKind::previousScene: action = { NoteActionKind::previousScene, -1 }; break;
        case MidiMappingPanel::TargetKind::scene:          action = { NoteActionKind::cena, command.argument }; break;
        case MidiMappingPanel::TargetKind::padProgram:     action = { NoteActionKind::padSynthProgram, command.argument }; break;
        case MidiMappingPanel::TargetKind::tapTempo:       action = { NoteActionKind::tapTempo, -1 }; break;
        case MidiMappingPanel::TargetKind::sustainToggle:  action = { NoteActionKind::sustainToggle, -1 }; break;
        case MidiMappingPanel::TargetKind::padEngineToggle: action = { NoteActionKind::padSynthEngineToggle, -1 }; break;
        default: break;
    }

    if (action.kind == NoteActionKind::none)
    {
        midiMappingPanel.setStatus ("Alvo indisponivel", MidiMappingPanel::Status::warning);
        return;
    }

    {
        const juce::ScopedLock sl (mapLock);
        lastTouchedSlot.store (-2, std::memory_order_relaxed);
        lastTouchedParam.store (-1, std::memory_order_relaxed);
        lastTouchedInsert.store (-1, std::memory_order_relaxed);
        lastTouchedParamName.clear();
        lastTouchedParamId.clear();
        lastTouchedPluginId.clear();
        lastTouchedParameterIdentityUnique.store (
            false, std::memory_order_relaxed);
        pendingNoteAction = action;
    }

    midiMappingPanel.setStatus (
        midiLearnActive.load()
            ? "Aperte uma tecla, pad ou pedal para  " + command.name
            : "Alvo selecionado. Ative o modo de aprendizado para mapear.",
        midiLearnActive.load() ? MidiMappingPanel::Status::learning
                               : MidiMappingPanel::Status::neutral);
}

void MainComponent::saveMidiMappingCommand (const MidiMappingPanel::Command& command)
{
    const int channel = juce::jlimit (1, 16, command.midiChannel);
    const int number = juce::jlimit (0, 127, command.number);
    const bool wantsNote = command.type == "Nota";

    const auto& devices = midiInputsCached (juce::Time::getMillisecondCounter());
    juce::MidiDeviceInfo selectedDevice;
    bool foundDevice = false;

    for (const auto& device : devices)
        if (device.name == command.device
             || prettifyDeviceName (device.name) == command.device)
        {
            selectedDevice = device;
            foundDevice = true;
            break;
        }

    if (! foundDevice)
        for (const auto& device : devices)
            if (deviceManager.isMidiInputDeviceEnabled (device.identifier))
            {
                selectedDevice = device;
                foundDevice = true;
                break;
            }

    if (! foundDevice)
    {
        midiMappingPanel.setStatus (
            "Conecte um controlador MIDI antes de salvar a atribuicao",
            MidiMappingPanel::Status::warning);
        return;
    }

    const auto sourceId = ChurchDAWMidiMapping::normalizeMidiSourceId (
        selectedDevice.identifier);
    const auto sourceName = selectedDevice.name;
    const MidiControlKey actionKey { sourceId, channel, number, false, sourceName };
    const MidiControlKey parameterKey { sourceId, channel, number, wantsNote, sourceName };
    const MidiControlKey originallyMappedKey {
        command.mappedSourceId, command.mappedChannel,
        command.mappedNumber, command.mappedWasNote,
        command.mappedSourceName
    };
    const CCTarget persistedCommandTarget {
        command.persistedParameterSlot,
        command.persistedParameterIndex,
        command.persistedParameterInsert,
        command.persistedParameterName,
        command.persistedParameterId,
        command.persistedPluginId
    };
    const bool explicitRelinkRequest = command.needsRelink
        && ChurchDAWMidiMapping::samePhysicalMidiControl (
            parameterKey, originallyMappedKey, midiSourcesCached);
    if (command.needsRelink && ! explicitRelinkRequest)
    {
        midiMappingPanel.setStatus (
            "Para revalidar, mantenha o mesmo dispositivo, canal e controle fisico",
            MidiMappingPanel::Status::warning);
        return;
    }

    auto* live = getChurchLive();
    int liveSlot = -1;
    if (live != nullptr)
        for (int slot = 0; slot < numSlots; ++slot)
            if (slots[slot].proc.load() == live)
            {
                liveSlot = slot;
                break;
            }

    CCTarget parameterTarget;
    bool hasParameterTarget = false;
    bool ambiguousParameterIdentity = false;

    auto resolveParameter = [this, &parameterTarget, &hasParameterTarget,
                             &ambiguousParameterIdentity]
        (int slot, int insert, juce::AudioProcessor* processor,
         juce::AudioProcessorParameter* parameter, bool master)
    {
        if (processor == nullptr || parameter == nullptr)
            return;

        const int index = processor->getParameters().indexOf (parameter);
        if (index < 0)
            return;
        if (! master
             && ! ChurchDAWMidiMapping::parameterIdentityIsUnique (
                    processor->getParameters(), index))
        {
            ambiguousParameterIdentity = true;
            return;
        }

        juce::String processorIdentity;
        auto* const currentProcessor = resolveCurrentMidiMappingProcessor (
            slot, insert, processorIdentity);
        if (processorIdentity.isEmpty() || currentProcessor != processor)
            return;

        parameterTarget = { slot, master ? -1 : index, insert,
                            master ? juce::String ("Master") : parameter->getName (32),
                            master ? juce::String ("master")
                                   : ChurchDAWMidiMapping::stableParameterId (parameter),
                            processorIdentity };
        hasParameterTarget = true;
    };

    switch (command.target)
    {
        case MidiMappingPanel::TargetKind::layerVolume:
            resolveParameter (liveSlot, -1, live,
                              live != nullptr ? live->getLayerVolParam (command.argument) : nullptr,
                              false);
            break;
        case MidiMappingPanel::TargetKind::layerReverb:
            resolveParameter (liveSlot, -1, live,
                              live != nullptr ? live->getLayerReverbSendParam (command.argument) : nullptr,
                              false);
            break;
        case MidiMappingPanel::TargetKind::layerComp:
            resolveParameter (liveSlot, -1, live,
                              live != nullptr ? live->getLayerCompParam (command.argument) : nullptr,
                              false);
            break;
        case MidiMappingPanel::TargetKind::padCutoff:
        case MidiMappingPanel::TargetKind::padVolume:
            if (padProcessor != nullptr)
                for (auto* parameter : padProcessor->getParameters())
                {
                    const auto id = ChurchDAWMidiMapping::stableParameterId (parameter);
                    if ((command.target == MidiMappingPanel::TargetKind::padCutoff && id == "padCutoff")
                         || (command.target == MidiMappingPanel::TargetKind::padVolume && id == "padVolume"))
                        resolveParameter (
                            padSlotId, -1, padProcessor, parameter, false);
                }
            break;
        case MidiMappingPanel::TargetKind::masterVolume:
            resolveParameter (-1, -1, masterProcessor,
                              masterProcessor != nullptr
                                && ! masterProcessor->getParameters().isEmpty()
                                  ? masterProcessor->getParameters()[0] : nullptr,
                              true);
            break;
        case MidiMappingPanel::TargetKind::processorParameter:
        {
            juce::String currentIdentity;
            auto* processor = resolveCurrentMidiMappingProcessor (
                command.parameterSlot, command.parameterInsert,
                currentIdentity);

            if (processor != nullptr
                 && (command.parameterId.isNotEmpty()
                      || command.parameterName.isNotEmpty()))
            {
                const auto& parameters = processor->getParameters();
                const int index =
                    ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                        parameters, command.parameterId,
                        command.parameterName,
                        ChurchDAWMidiMapping::classifyMidiTargetLocation (
                            command.parameterSlot, command.parameterInsert,
                            numSlots, padSlotId, padSynthSlotId));
                if (juce::isPositiveAndBelow (index, parameters.size()))
                    resolveParameter (
                        command.parameterSlot, command.parameterInsert,
                        processor, parameters[index],
                        command.parameterSlot == -1);
            }
            break;
        }
        default:
            break;
    }

    NoteAction actionTarget;
    if (! hasParameterTarget)
        switch (command.target)
        {
            case MidiMappingPanel::TargetKind::panic:
                actionTarget = { NoteActionKind::panic, -1 }; break;
            case MidiMappingPanel::TargetKind::nextScene:
                actionTarget = { NoteActionKind::nextScene, -1 }; break;
            case MidiMappingPanel::TargetKind::previousScene:
                actionTarget = { NoteActionKind::previousScene, -1 }; break;
            case MidiMappingPanel::TargetKind::scene:
                actionTarget = { NoteActionKind::cena, command.argument }; break;
            case MidiMappingPanel::TargetKind::padProgram:
                actionTarget = { NoteActionKind::padSynthProgram, command.argument }; break;
            case MidiMappingPanel::TargetKind::tapTempo:
                actionTarget = { NoteActionKind::tapTempo, -1 }; break;
            case MidiMappingPanel::TargetKind::sustainToggle:
                actionTarget = { NoteActionKind::sustainToggle, -1 }; break;
            case MidiMappingPanel::TargetKind::padEngineToggle:
                actionTarget = { NoteActionKind::padSynthEngineToggle, -1 }; break;
            default:
                break;
        }

    if (ambiguousParameterIdentity)
    {
        midiMappingPanel.setStatus (
            "O plugin publicou um identificador de parametro duplicado",
            MidiMappingPanel::Status::warning);
        return;
    }

    if (! hasParameterTarget && actionTarget.kind == NoteActionKind::none)
    {
        midiMappingPanel.setStatus (
            "O motor correspondente a este comando ainda nao esta carregado",
            MidiMappingPanel::Status::warning);
        return;
    }

    if (hasParameterTarget)
    {
        juce::String staleReason;
        if (! validateCurrentParameterCommand (
                command, parameterTarget, staleReason))
        {
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                staleReason + "; atualize o painel e tente novamente",
                MidiMappingPanel::Status::warning);
            return;
        }
    }

    if (command.needsRelink)
    {
        const bool compatibleExplicitRelink = hasParameterTarget
            && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::
                mayExplicitlyRelink (
                    persistedCommandTarget, parameterTarget);
        if (! compatibleExplicitRelink)
        {
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                "O parametro atual nao e compativel com o mapa pendente",
                MidiMappingPanel::Status::warning);
            return;
        }
    }

    if (hasParameterTarget && ! wantsNote
         && ! ChurchDAWMidiMapping::isMappableController (number))
    {
        midiMappingPanel.setStatus (
            "Esse CC e reservado para pedais ou controle de sistema",
            MidiMappingPanel::Status::error);
        return;
    }

    if (! hasParameterTarget && ! wantsNote
         && ! ChurchDAWMidiMapping::isMappableAction (number))
    {
        midiMappingPanel.setStatus (
            "Esse CC e reservado pelo protocolo e nao pode ser usado como botao",
            MidiMappingPanel::Status::error);
        return;
    }

    const auto sameParameter = [&parameterTarget] (const CCTarget& value)
    {
        return ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameTarget (
            value, parameterTarget);
    };
    const auto sameAction = [&actionTarget] (const NoteAction& value)
    {
        const bool argumentMatters = actionTarget.kind == NoteActionKind::cena
                                  || actionTarget.kind == NoteActionKind::padKey
                                  || actionTarget.kind == NoteActionKind::padSynthProgram;
        return value.kind == actionTarget.kind
                && (! argumentMatters || value.arg == actionTarget.arg);
    };

    bool conflict = false;
    juce::String conflictName;
    bool checkpointReady = false;
    bool committed = false;
    bool rollbackOk = true;
    bool diskStillOwned = false;
    bool savedCC = false;
    bool savedNotes = false;
    juce::uint64 ccEpoch = 0;
    juce::uint64 noteEpoch = 0;
    const auto ccFile = arquivoDoMapaDeCCs();
    const auto noteFile = arquivoDoMapaDeNotas();
    ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
        ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);
    decltype (ccMappings) previousCC;
    decltype (ccActionMappings) previousActions;
    decltype (noteMappings) previousNotes;
    const bool previousBlockedCC = bloquearSalvamentoCCVazio;
    const bool previousBlockedNotes = bloquearSalvamentoNotasVazio;
    const bool previousProfilesSuspended = automaticControllerProfilesSuspended;

    beginMidiMapRoutingTransaction();
    if (hasParameterTarget)
    {
        // Close the validation-to-mutation race: a MIDI callback admitted just
        // before beginMidiMapRoutingTransaction() may have changed the map
        // after the first UI-time check. The callback barrier is now drained,
        // so this second snapshot remains stable through the mutation below.
        juce::String staleReason;
        if (! validateCurrentParameterCommand (
                command, parameterTarget, staleReason))
        {
            finishMidiMapRoutingTransaction();
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                staleReason + "; atualize o painel e tente novamente",
                MidiMappingPanel::Status::warning);
            return;
        }
    }

    checkpointReady = diskRollback.capture ("before-command");
    if (! checkpointReady)
        rollbackOk = false;
    if (checkpointReady)
    {
        const juce::ScopedLock sl (mapLock);
        previousCC = ccMappings;
        previousActions = ccActionMappings;
        previousNotes = noteMappings;
        const auto findPhysicalWhere =
            [this] (const auto& mappings, const MidiControlKey& key,
                    const auto& predicate)
        {
            return ChurchDAWMidiMapping::findPhysicalMidiControlIf (
                mappings, key, midiSourcesCached, predicate);
        };
        const auto isExactRelinkEntry = [&] (const auto& entry)
        {
            return explicitRelinkRequest
                && ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                    entry.first, originallyMappedKey)
                && ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                    entry.second, persistedCommandTarget);
        };

        if (hasParameterTarget)
        {
            const auto parameterConflict = findPhysicalWhere (
                ccMappings, parameterKey, [&] (const auto& mapping)
                {
                    return ! sameParameter (mapping.second)
                        && ! isExactRelinkEntry (mapping);
                });
            if (parameterConflict != ccMappings.end())
            {
                conflict = true;
                conflictName = describeCCTarget (
                    parameterConflict->second);
            }

            const auto actionConflict = wantsNote
                ? findPhysicalWhere (
                    noteMappings, actionKey,
                    [] (const auto&) { return true; })
                                                  : noteMappings.end();
            if (actionConflict != noteMappings.end())
            {
                conflict = true;
                conflictName = "outro comando por nota";
            }
            if (! wantsNote)
                if (const auto it = findPhysicalWhere (
                        ccActionMappings, actionKey,
                        [] (const auto& mapping)
                        {
                            return mapping.second.acao.kind
                                != NoteActionKind::none;
                        });
                    it != ccActionMappings.end()
                )
                {
                    conflict = true;
                    conflictName = "outro comando por CC";
                }
        }
        else if (wantsNote)
        {
            if (const auto it = findPhysicalWhere (
                    noteMappings, actionKey,
                    [&] (const auto& mapping)
                    {
                        return ! sameAction (mapping.second);
                    });
                it != noteMappings.end())
            {
                conflict = true;
                conflictName = "outro comando por nota";
            }

            const MidiControlKey noteParameterKey {
                sourceId, channel, number, true, sourceName };
            if (const auto it = findPhysicalWhere (
                    ccMappings, noteParameterKey,
                    [] (const auto&) { return true; });
                it != ccMappings.end())
            {
                conflict = true;
                conflictName = describeCCTarget (it->second);
            }
        }
        else
        {
            if (const auto it = findPhysicalWhere (
                    ccActionMappings, actionKey,
                    [&] (const auto& mapping)
                    {
                        return ! sameAction (mapping.second.acao);
                    });
                it != ccActionMappings.end())
            {
                conflict = true;
                conflictName = "outro comando por CC";
            }
            if (const auto it = findPhysicalWhere (
                    ccMappings, parameterKey,
                    [] (const auto&) { return true; });
                it != ccMappings.end())
            {
                conflict = true;
                conflictName = describeCCTarget (it->second);
            }
        }

        if (! conflict && hasParameterTarget)
        {
            if (explicitRelinkRequest)
            {
                const auto exact = ccMappings.find (originallyMappedKey);
                if (exact == ccMappings.end()
                     || ! isExactRelinkEntry (*exact))
                {
                    conflict = true;
                    conflictName = "mapa alterado enquanto era revalidado";
                }
                else
                {
                    // Remove the exact old source/key before inserting the
                    // replugged key. Never leave legacy + new-source twins.
                    ccMappings.erase (exact);
                }
            }
            else
            {
                for (auto it = ccMappings.begin(); it != ccMappings.end();)
                {
                    if (sameParameter (it->second))
                        it = ccMappings.erase (it);
                    else
                        ++it;
                }
            }
            if (! conflict)
                ccMappings[parameterKey] = parameterTarget;
        }
        else if (! conflict)
        {
            for (auto it = noteMappings.begin(); it != noteMappings.end();)
                if (sameAction (it->second))
                    it = noteMappings.erase (it);
                else
                    ++it;
            for (auto it = ccActionMappings.begin(); it != ccActionMappings.end();)
                if (sameAction (it->second.acao))
                    it = ccActionMappings.erase (it);
                else
                    ++it;

            if (wantsNote)
                noteMappings[actionKey] = actionTarget;
            else
            {
                AcaoDeCC entry;
                entry.acao = actionTarget;
                entry.modo = command.mode == 4
                               ? ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel
                               : ChurchDAWMidiMapping::ModoDeBotaoCC::alternar;
                ccActionMappings[actionKey] = entry;
            }
        }
    }

    if (checkpointReady && ! conflict)
    {
        ccDirtyEpoch.markDirty();
        noteDirtyEpoch.markDirty();
        ccEpoch = ccDirtyEpoch.snapshot();
        noteEpoch = noteDirtyEpoch.snapshot();
        diskStillOwned =
            diskRollback.targetsUnchangedSinceCapture()
            && ! ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
                ccFile, noteFile);
        savedCC = diskStillOwned && saveMidiMap();
        savedNotes = savedCC && saveNoteMap();
        committed = savedCC && savedNotes && diskRollback.commit();
    }

    if (checkpointReady && (! committed || conflict))
    {
        if (diskStillOwned && ! conflict)
            rollbackOk =
                ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                    diskRollback, ccFile, noteFile);
        else
        {
            rollbackOk = diskRollback.closeWithoutMutation();
        }
        if (! conflict)
        {
            const juce::ScopedLock sl (mapLock);
            ccMappings = std::move (previousCC);
            ccActionMappings = std::move (previousActions);
            noteMappings = std::move (previousNotes);
            bloquearSalvamentoCCVazio = previousBlockedCC;
            bloquearSalvamentoNotasVazio = previousBlockedNotes;
            automaticControllerProfilesSuspended = previousProfilesSuspended;
            bumpMidiMappingGeneration();
        }
    }

    if (committed)
    {
        finishMidiMapDirtyEpochsAfterPairCommit (
            ccEpoch, savedCC, noteEpoch, savedNotes);
        bumpMidiMappingGeneration();
        cancelPendingUpdate();
        lastMappedCC.store (-1);
        lastMappedNote.store (-1);
        lastMappedCCAcao.store (-1);
        lastNoteActionRejected.store (-1);
        lastParameterMappingRejectedCC.store (-1);
    }
    finishMidiMapRoutingTransaction();

    if (! rollbackOk || diskRollback.preservedExternalChanges())
        midiMapPairRecoveryFailed = true;

    reconstruirMascaraDeNotas();
    reconstruirMascaraDeCCsDeAcao();
    atualizarEtiquetasDoPad();
    atualizarBotoesDeCCDoPad();
    ccPickup.clear();
    refreshMidiMappingPanel();

    if (conflict)
    {
        midiMappingPanel.showConflict (
            "Esse controle ja esta atribuido a  "
              + (conflictName.isNotEmpty() ? conflictName : juce::String ("outro comando")));
        return;
    }
    if (! checkpointReady || ! committed)
    {
        midiMappingPanel.setStatus (
            rollbackOk
                ? "Nao foi possivel salvar; a atribuicao anterior foi preservada"
                : "Falha grave ao restaurar os mapas; gravacao suspensa",
            MidiMappingPanel::Status::error);
        return;
    }

    midiMappingPanel.showMapped (command.name, channel, number, true, wantsNote);
}

void MainComponent::deleteMidiMappingCommand (const MidiMappingPanel::Command& command)
{
    juce::AudioProcessorParameter* parameter = nullptr;
    int targetSlot = -2;
    int targetInsert = -1;
    auto* live = getChurchLive();

    if (live != nullptr)
        for (int slot = 0; slot < numSlots; ++slot)
            if (slots[slot].proc.load() == live)
                targetSlot = slot;

    switch (command.target)
    {
        case MidiMappingPanel::TargetKind::layerVolume:
            parameter = live != nullptr ? live->getLayerVolParam (command.argument) : nullptr; break;
        case MidiMappingPanel::TargetKind::layerReverb:
            parameter = live != nullptr ? live->getLayerReverbSendParam (command.argument) : nullptr; break;
        case MidiMappingPanel::TargetKind::layerComp:
            parameter = live != nullptr ? live->getLayerCompParam (command.argument) : nullptr; break;
        case MidiMappingPanel::TargetKind::padCutoff:
        case MidiMappingPanel::TargetKind::padVolume:
            targetSlot = padSlotId;
            if (padProcessor != nullptr)
                for (auto* candidate : padProcessor->getParameters())
                {
                    const auto id = ChurchDAWMidiMapping::stableParameterId (candidate);
                    if ((command.target == MidiMappingPanel::TargetKind::padCutoff && id == "padCutoff")
                         || (command.target == MidiMappingPanel::TargetKind::padVolume && id == "padVolume"))
                        parameter = candidate;
                }
            break;
        case MidiMappingPanel::TargetKind::masterVolume:
            targetSlot = -1;
            parameter = masterProcessor != nullptr && ! masterProcessor->getParameters().isEmpty()
                          ? masterProcessor->getParameters()[0] : nullptr;
            break;
        case MidiMappingPanel::TargetKind::processorParameter:
        {
            targetSlot = command.parameterSlot;
            targetInsert = command.parameterInsert;
            juce::String currentIdentity;
            auto* processor = resolveCurrentMidiMappingProcessor (
                targetSlot, targetInsert, currentIdentity);

            if (processor != nullptr
                 && (command.parameterId.isNotEmpty()
                      || command.parameterName.isNotEmpty()))
            {
                const auto& parameters = processor->getParameters();
                const int index =
                    ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                        parameters, command.parameterId,
                        command.parameterName,
                        ChurchDAWMidiMapping::classifyMidiTargetLocation (
                            targetSlot, targetInsert, numSlots,
                            padSlotId, padSynthSlotId));
                if (juce::isPositiveAndBelow (index, parameters.size()))
                    parameter = parameters[index];
            }
            break;
        }
        default:
            break;
    }

    const bool hasPersistedParameterEntry =
        command.persistedParameterSlot != -2
        && command.mappedNumber >= 0;
    if (hasPersistedParameterEntry
         && command.expectedCurrentPluginId.isNotEmpty())
    {
        juce::String currentIdentity;
        auto* const currentProcessor =
            resolveCurrentMidiMappingProcessor (
                targetSlot, targetInsert, currentIdentity);
        const int parameterIndex =
            currentProcessor != nullptr && parameter != nullptr
                ? currentProcessor->getParameters().indexOf (parameter)
                : -1;
        if (currentProcessor == nullptr || currentIdentity.isEmpty()
             || currentIdentity != command.expectedCurrentPluginId)
        {
            refreshMidiMappingPanel();
            midiMappingPanel.setStatus (
                "O plugin mudou desde a selecao; escolha o mapa novamente",
                MidiMappingPanel::Status::warning);
            return;
        }

        // An unresolved parameter mapping must remain deletable. Its exact
        // persisted key/payload is checked atomically below; when the
        // parameter is still resolvable, retain the stronger full snapshot
        // validation as well.
        if (parameter != nullptr && parameterIndex >= 0)
        {
            const CCTarget currentTarget {
                targetSlot,
                targetSlot == -1 ? -1 : parameterIndex,
                targetInsert,
                targetSlot == -1 ? juce::String ("Master")
                                 : parameter->getName (32),
                targetSlot == -1
                    ? juce::String ("master")
                    : ChurchDAWMidiMapping::stableParameterId (parameter),
                currentIdentity
            };
            juce::String staleReason;
            if (! validateCurrentParameterCommand (
                    command, currentTarget, staleReason))
            {
                refreshMidiMappingPanel();
                midiMappingPanel.setStatus (
                    staleReason + "; escolha o mapa novamente",
                    MidiMappingPanel::Status::warning);
                return;
            }
        }
    }

    const auto ccFile = arquivoDoMapaDeCCs();
    const auto noteFile = arquivoDoMapaDeNotas();
    ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
        ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);
    decltype (ccMappings) previousCC;
    decltype (ccActionMappings) previousActions;
    decltype (noteMappings) previousNotes;
    const bool previousBlockedCC = bloquearSalvamentoCCVazio;
    const bool previousBlockedNotes = bloquearSalvamentoNotasVazio;
    const bool previousProfilesSuspended = automaticControllerProfilesSuspended;

    beginMidiMapRoutingTransaction();
    const bool checkpointReady = diskRollback.capture ("before-delete");
    const bool targetsUnchanged = checkpointReady
        && diskRollback.targetsUnchangedSinceCapture();
    if (! targetsUnchanged)
    {
        const bool closed = checkpointReady
            && diskRollback.closeWithoutMutation();
        finishMidiMapRoutingTransaction();
        if (! checkpointReady
             && diskRollback.hadUninspectableTargetConflict())
            bloquearQuartetoMidiDesconhecido (
                "checkpoint de remocao MIDI");
        if (! closed || diskRollback.preservedExternalChanges())
            midiMapPairRecoveryFailed = true;
        midiMappingPanel.setStatus (
            "Nao foi possivel iniciar a remocao com seguranca",
            MidiMappingPanel::Status::error);
        return;
    }
    {
        const juce::ScopedLock sl (mapLock);
        previousCC = ccMappings;
        previousActions = ccActionMappings;
        previousNotes = noteMappings;
    }

    bool changedCC = false;
    bool changedNote = false;

    if (hasPersistedParameterEntry)
    {
        const CCTarget persistedTarget {
            command.persistedParameterSlot,
            command.persistedParameterIndex,
            command.persistedParameterInsert,
            command.persistedParameterName,
            command.persistedParameterId,
            command.persistedPluginId
        };
        const MidiControlKey selectedStoredControl {
            command.mappedSourceId, command.mappedChannel,
            command.mappedNumber, command.mappedWasNote,
            command.mappedSourceName
        };
        const juce::ScopedLock sl (mapLock);
        const auto exact = ccMappings.find (selectedStoredControl);
        if (exact != ccMappings.end()
             && ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                    exact->first, selectedStoredControl)
             && ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                    exact->second, persistedTarget))
        {
            ccMappings.erase (exact);
            changedCC = true;
        }
    }
    else
    {
        NoteAction wanted;
        switch (command.target)
        {
            case MidiMappingPanel::TargetKind::panic:         wanted = { NoteActionKind::panic, -1 }; break;
            case MidiMappingPanel::TargetKind::nextScene:     wanted = { NoteActionKind::nextScene, -1 }; break;
            case MidiMappingPanel::TargetKind::previousScene: wanted = { NoteActionKind::previousScene, -1 }; break;
            case MidiMappingPanel::TargetKind::scene:          wanted = { NoteActionKind::cena, command.argument }; break;
            case MidiMappingPanel::TargetKind::padProgram:     wanted = { NoteActionKind::padSynthProgram, command.argument }; break;
            case MidiMappingPanel::TargetKind::tapTempo:       wanted = { NoteActionKind::tapTempo, -1 }; break;
            case MidiMappingPanel::TargetKind::sustainToggle:  wanted = { NoteActionKind::sustainToggle, -1 }; break;
            case MidiMappingPanel::TargetKind::padEngineToggle: wanted = { NoteActionKind::padSynthEngineToggle, -1 }; break;
            default: break;
        }

        const auto matches = [&wanted] (const NoteAction& value)
        {
            const bool argumentMatters = wanted.kind == NoteActionKind::cena
                                      || wanted.kind == NoteActionKind::padKey
                                      || wanted.kind == NoteActionKind::padSynthProgram;
            return value.kind == wanted.kind
                    && (! argumentMatters || value.arg == wanted.arg);
        };

        if (wanted.kind != NoteActionKind::none)
        {
            const juce::ScopedLock sl (mapLock);
            for (auto it = noteMappings.begin(); it != noteMappings.end();)
                if (matches (it->second))
                {
                    it = noteMappings.erase (it);
                    changedNote = true;
                }
                else
                    ++it;

            for (auto it = ccActionMappings.begin(); it != ccActionMappings.end();)
                if (matches (it->second.acao))
                {
                    it = ccActionMappings.erase (it);
                    changedCC = true;
                }
                else
                    ++it;
        }
    }

    if (! changedCC && ! changedNote)
    {
        const bool closed = diskRollback.closeWithoutMutation();
        finishMidiMapRoutingTransaction();
        if (! closed)
            midiMapPairRecoveryFailed = true;
        midiMappingPanel.setStatus ("Este comando ainda nao possui mapeamento",
                                    MidiMappingPanel::Status::neutral);
        return;
    }

    if (changedCC)
        ccDirtyEpoch.markDirty();
    if (changedNote)
        noteDirtyEpoch.markDirty();
    const auto ccEpoch = ccDirtyEpoch.snapshot();
    const auto noteEpoch = noteDirtyEpoch.snapshot();

    const auto activeCCAction = [] (const AcaoDeCC& action)
    {
        return action.acao.kind != NoteActionKind::none;
    };
    const int conflictsBefore =
        ChurchDAWMidiMapping::midiMapCollectionNoteConflictCount (
            previousCC, previousNotes)
        + ChurchDAWMidiMapping::midiMapCollectionCCConflictCount (
            previousCC, previousActions, activeCCAction);
    int conflictsAfter = 0;
    {
        const juce::ScopedLock sl (mapLock);
        conflictsAfter =
            ChurchDAWMidiMapping::midiMapCollectionNoteConflictCount (
                ccMappings, noteMappings)
            + ChurchDAWMidiMapping::midiMapCollectionCCConflictCount (
                ccMappings, ccActionMappings, activeCCAction);
    }

    // Um mapa legado pode conter mais de um conflito. Permitir o save somente
    // quando ESTA remocao reduziu a quantidade deixa o cliente reparar um por
    // vez, sem abrir uma porta geral para persistir novos conflitos.
    const bool repairMadeProgress = conflictsAfter < conflictsBefore;
    // Every committed journal generation must stamp both files with the same
    // transactionId, even when this command changed only one in-memory map.
    // Publishing one half would leave an apparently committed mixed pair that
    // startup must recover instead of the deletion surviving normally.
    const bool savedCC = saveMidiMap (false, repairMadeProgress);
    const bool savedNote = savedCC
        && saveNoteMap (false, repairMadeProgress);
    const bool committed = savedCC && savedNote && diskRollback.commit();
    bool rollbackOk = true;
    if (! committed)
    {
        rollbackOk =
            ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                diskRollback, ccFile, noteFile);
        const juce::ScopedLock sl (mapLock);
        ccMappings = std::move (previousCC);
        ccActionMappings = std::move (previousActions);
        noteMappings = std::move (previousNotes);
        bloquearSalvamentoCCVazio = previousBlockedCC;
        bloquearSalvamentoNotasVazio = previousBlockedNotes;
        automaticControllerProfilesSuspended = previousProfilesSuspended;
        bumpMidiMappingGeneration();
    }
    if (committed)
    {
        finishMidiMapDirtyEpochsAfterPairCommit (
            ccEpoch, savedCC, noteEpoch, savedNote);
        {
            const juce::ScopedLock sl (mapLock);
            automaticControllerProfilesSuspended =
                ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                    ccMappings, noteMappings)
                || ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                    ccMappings, ccActionMappings, activeCCAction)
                || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    ccMappings, midiSourcesCached)
                || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    ccActionMappings, midiSourcesCached)
                || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    noteMappings, midiSourcesCached);
        }
        bumpMidiMappingGeneration();
    }
    finishMidiMapRoutingTransaction();
    if (! rollbackOk || diskRollback.preservedExternalChanges())
        midiMapPairRecoveryFailed = true;
    reconstruirMascaraDeNotas();
    reconstruirMascaraDeCCsDeAcao();
    atualizarEtiquetasDoPad();
    atualizarBotoesDeCCDoPad();
    ccPickup.clear();
    refreshMidiMappingPanel();
    midiMappingPanel.setStatus (
        committed ? "Mapeamento removido"
                  : rollbackOk
                      ? "Falha ao remover; o mapeamento anterior foi restaurado"
                      : "Falha grave ao restaurar os mapas",
        committed ? MidiMappingPanel::Status::success
                  : MidiMappingPanel::Status::error);
}

void MainComponent::setMidiLearningFromPage (bool enabled)
{
    resetMidiLearnTargetUnderCallbackBarrier (enabled);

    if (! enabled)
    {
        if (limparBotoesDeCCVazios())
            (void) saveMidiMap();
        atualizarBotoesDeCCDoPad();
    }

    if (padPanel != nullptr)
        padPanel->setMidiLearnMode (enabled);

    midiMappingPanel.setLearning (enabled, false);
    midiMappingPanel.setStatus (
        enabled ? "Selecione um comando e mova um controle MIDI"
                : "Modo de aprendizado pausado",
        enabled ? MidiMappingPanel::Status::learning
                : MidiMappingPanel::Status::neutral);
}

void MainComponent::beginQuickMidiMapping()
{
   #if CHURCHDAW_MOBILE_SHELL
    // A tela compacta ja usa o fluxo direto historico; a central desktop e a
    // unica origem deste comando.
    return;
   #else
    startMidiLearn();
    quickMidiMappingMode = true;
    clearMapButton.setVisible (false);

    juce::Logger::writeToLog (
        "MIDI MAP: modo rapido aberto; toque um controle da tela e mova o MIDI");
   #endif
}

void MainComponent::exportMidiMappings()
{
    if (! saveMidiMapPair())
    {
        midiMappingPanel.setStatus (
            "Nao foi possivel preparar os mapas para exportacao",
            MidiMappingPanel::Status::error);
        return;
    }

    if (midiMapFileLock == nullptr)
    {
        midiMappingPanel.setStatus (
            "Protecao dos mapas MIDI indisponivel",
            MidiMappingPanel::Status::error);
        return;
    }

    juce::String bundleText;
    {
        const auto ccFile = arquivoDoMapaDeCCs();
        const auto noteFile = arquivoDoMapaDeNotas();
        ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess exportAccess (
            *midiMapFileLock, 5000);
        ChurchDAWMidiMapping::MidiMapPairRevision exportRevision;
        if (! exportAccess.isLocked()
             || ! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                    ccFile, {})
             || ! midiMapDiskRevision.matches (ccFile, noteFile)
             || ! exportRevision.refresh (ccFile, noteFile))
        {
            midiMappingPanel.setStatus (
                "Os mapas mudaram durante a exportacao; tente novamente",
                MidiMappingPanel::Status::error);
            return;
        }

        // Parse the exact bounded bytes read under one lock. The post-read
        // quartet revision also detects a writer that bypasses our lock and
        // changes either primary or either last-good between the two reads.
        juce::MemoryBlock ccBytes;
        juce::MemoryBlock noteBytes;
        const bool readStablePair =
            ChurchDAWMidiMapping::loadBoundedFileData (
                ccFile, ccBytes,
                ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && ChurchDAWMidiMapping::loadBoundedFileData (
                noteFile, noteBytes,
                ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && exportRevision.matches (ccFile, noteFile);
        auto ccXml = readStablePair
            ? ChurchDAWMidiMapping::parseBoundedXmlData (ccBytes) : nullptr;
        auto noteXml = readStablePair
            ? ChurchDAWMidiMapping::parseBoundedXmlData (noteBytes) : nullptr;
        const auto ccTransaction = ccXml != nullptr
            ? ccXml->getStringAttribute ("transactionId") : juce::String();
        const auto noteTransaction = noteXml != nullptr
            ? noteXml->getStringAttribute ("transactionId") : juce::String();
        if (ccXml == nullptr
             || ! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    *ccXml, "MIDIMAP", "MAP", "CCACAO")
             || noteXml == nullptr
             || ! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    *noteXml, "NOTEMAP", "NOTE")
             || ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                    *ccXml, *noteXml)
             || ccTransaction.isEmpty()
             || ccTransaction != noteTransaction)
        {
            midiMappingPanel.setStatus (
                "Os arquivos MIDI nao puderam ser validados",
                MidiMappingPanel::Status::error);
            return;
        }

        const auto exportFormat =
            juce::XmlElement::TextFormat().singleLine();
        const auto ccExportText = ccXml->toString (exportFormat);
        const auto noteExportText = noteXml->toString (exportFormat);
        if (! ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                ccExportText)
             || ! ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                noteExportText))
        {
            midiMappingPanel.setStatus (
                "Os mapas excedem os limites seguros de exportacao",
                MidiMappingPanel::Status::error);
            return;
        }

        juce::XmlElement bundle ("CHURCHDAW_MIDI_MAP_BUNDLE");
        bundle.setAttribute ("version", 1);
        bundle.setAttribute (
            "exportedAt", juce::Time::getCurrentTime().toISO8601 (true));
        bundle.createNewChildElement ("CC_MAP")->addTextElement (
            ccExportText);
        bundle.createNewChildElement ("NOTE_MAP")->addTextElement (
            noteExportText);
        bundleText = bundle.toString();
        if (! ChurchDAWMidiMapping::midiMapSerializedBundleFitsSafetyLimits (
                bundleText))
        {
            midiMappingPanel.setStatus (
                "O pacote MIDI excede o limite seguro de importacao",
                MidiMappingPanel::Status::error);
            return;
        }
    }

    auto initial = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("ChurchDAW-MIDI.churchdaw-midi.xml");
    midiMapFileChooser = std::make_unique<juce::FileChooser> (
        "Exportar mapeamentos MIDI", initial, "*.xml");

    auto safe = juce::Component::SafePointer<MainComponent> (this);
    midiMapFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode
          | juce::FileBrowserComponent::canSelectFiles
          | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, bundleText] (const juce::FileChooser& chooser)
        {
            if (auto* self = safe.getComponent())
            {
                const auto selected = chooser.getResult();
                bool saved = false;
                if (selected != juce::File())
                {
                    juce::TemporaryFile temp (selected);
                    saved = temp.getFile().replaceWithText (bundleText)
                              && temp.overwriteTargetFileWithTemporary();
                    if (saved)
                    {
                        juce::MemoryBlock expectedBytes;
                        juce::MemoryBlock writtenBytes;
                        expectedBytes.append (
                            bundleText.toRawUTF8(),
                            (size_t) bundleText.getNumBytesAsUTF8());
                        saved = ChurchDAWMidiMapping::loadBoundedFileData (
                                    selected, writtenBytes,
                                    ChurchDAWMidiMapping::maxMidiMapBundleBytes)
                            && writtenBytes == expectedBytes
                            && ChurchDAWMidiMapping::loadBoundedXmlFile (
                                   selected,
                                   ChurchDAWMidiMapping::midiMapBundleXmlLimits())
                               != nullptr;
                    }
                }

                self->midiMappingPanel.setStatus (
                    selected == juce::File() ? "Exportacao cancelada"
                      : saved ? "Mapeamentos MIDI exportados"
                              : "Falha ao exportar os mapeamentos",
                    selected == juce::File() ? MidiMappingPanel::Status::neutral
                      : saved ? MidiMappingPanel::Status::success
                              : MidiMappingPanel::Status::error);
                self->midiMapFileChooser.reset();
            }
        });
}

void MainComponent::exportCultSession()
{
    // Salva primeiro, para o arquivo compartilhado representar exatamente o
    // que esta na tela e nunca uma versao anterior do culto.
    const bool scenesSaved = salvarCenas();
    const bool chainSaved = saveChainState();
    auto scenes = juce::parseXML (arquivoDeCenas());
    auto chain = appProperties.getUserSettings()->getXmlValue ("chain");

    if (! scenesSaved || ! chainSaved || scenes == nullptr
         || ! scenes->hasTagName ("LIVESCENES") || chain == nullptr
         || ! chain->hasTagName ("CHAIN"))
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Exportar culto",
            "Nao foi possivel validar as cenas e a cadeia no disco. "
            "O arquivo compartilhavel nao foi criado.", "Entendi");
        return;
    }

    juce::XmlElement bundle ("CHURCHDAW_CULT_SESSION");
    bundle.setAttribute ("schema", 1);
    bundle.setAttribute ("exportedAt", juce::Time::getCurrentTime().toISO8601 (true));
    bundle.addChildElement (scenes.release());
    bundle.addChildElement (chain.release());
    const auto bundleText = bundle.toString();

    auto initial = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile ("ChurchDAW-Culto.churchset.xml");
    sessionBundleFileChooser = std::make_unique<juce::FileChooser> (
        "Exportar culto completo", initial, "*.churchset.xml");
    auto safe = juce::Component::SafePointer<MainComponent> (this);
    sessionBundleFileChooser->launchAsync (
        juce::FileBrowserComponent::saveMode
          | juce::FileBrowserComponent::canSelectFiles
          | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, bundleText] (const juce::FileChooser& chooser)
        {
            if (auto* self = safe.getComponent())
            {
                const auto selected = chooser.getResult();
                bool saved = false;
                if (selected != juce::File())
                {
                    juce::TemporaryFile temp (selected);
                    saved = temp.getFile().replaceWithText (bundleText)
                         && temp.overwriteTargetFileWithTemporary();
                }
                juce::AlertWindow::showMessageBoxAsync (
                    saved ? juce::MessageBoxIconType::InfoIcon
                          : juce::MessageBoxIconType::WarningIcon,
                    "Exportar culto",
                    selected == juce::File() ? "Exportacao cancelada."
                      : saved ? "Culto exportado com cenas, setlist e cadeia."
                              : "Falha ao gravar o arquivo do culto.", "Entendi");
                self->sessionBundleFileChooser.reset();
            }
        });
}

void MainComponent::activateEmergencyPiano()
{
    // A troca de emergencia e deliberadamente temporaria: o setup original
    // continua no disco para que o proximo boot possa tentar recupera-lo.
    performGlobalPanic();

    loadInternalTimbres (0, nullptr, 0);
    slots[0].box.setSelectedId (timbresComboId, juce::dontSendNotification);
    refreshSlotChoices();
    refreshPerformanceSessionPanel();
    juce::Logger::writeToLog (
        "SAFE_PIANO: Church Live nativo carregado; setup original preservado");
}

void MainComponent::runStageDiagnostic()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    const auto midiInputs = midiInputsCached (juce::Time::getMillisecondCounter());
    int enabledMidi = 0;
    for (const auto& input : midiInputs)
        if (deviceManager.isMidiInputDeviceEnabled (input.identifier))
            ++enabledMidi;

    const auto audioName = device != nullptr
        ? device->getName() : juce::String ("Nenhum dispositivo");
    const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 0.0;
    const auto blockSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
    const auto xruns = deviceManager.getXRunCount();

    int activeSlots = 0;
    for (int i = 0; i < numSlots; ++i)
        if (slots[i].proc.load() != nullptr)
            ++activeSlots;

    const bool audioOk = device != nullptr && sampleRate > 0.0 && blockSize > 0;
    const bool midiOk = enabledMidi > 0;
    const bool stable = xruns == 0;

    juce::String report;
    report << (audioOk
                   ? juce::String::fromUTF8 (u8"✅ AUDIO OK")
                   : juce::String::fromUTF8 (u8"❌ AUDIO INDISPONÍVEL")) << "\n"
           << juce::String::fromUTF8 (u8"Saída: ") << audioName << "\n"
           << juce::String::fromUTF8 (u8"Configuração: ")
           << juce::String (sampleRate, 0) << " Hz / "
           << blockSize << " samples\n\n"
           << (midiOk
                   ? juce::String::fromUTF8 (u8"✅ MIDI OK")
                   : juce::String::fromUTF8 (u8"⚠️ MIDI: nenhum controlador ativo")) << "\n"
           << juce::String::fromUTF8 (u8"Entradas ativas: ")
           << enabledMidi << " de " << midiInputs.size() << "\n"
           << juce::String::fromUTF8 (u8"Instrumentos carregados: ")
           << activeSlots << "\n\n"
           << (stable
                   ? juce::String::fromUTF8 (u8"✅ SEM XRUNS")
                   : juce::String::fromUTF8 (u8"⚠️ XRUNS DETECTADOS: ")
                         + juce::String (xruns))
           << "\n\n"
           << (safeMode
                   ? juce::String::fromUTF8 (
                         u8"MODO SEGURO: plugins externos não foram restaurados.\n")
                   : juce::String::fromUTF8 (
                         u8"Modo normal: cadeia salva habilitada.\n"))
           << (audioOk && stable
                   ? "Pronto para o culto."
                   : "Se aparecer falha, use PIANO SEGURO antes de tocar.");

    juce::Logger::writeToLog ("STAGE_TEST: " + report.replaceCharacters ("\n", " | "));
    juce::AlertWindow::showMessageBoxAsync (
        juce::AlertWindow::InfoIcon, "TESTE DE PALCO", report);
}

void MainComponent::showPlaybackFileChooser()
{
    playbackFileChooser = std::make_unique<juce::FileChooser> (
        "Abrir playback / stems (selecione ate 4 arquivos)",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.flac;*.mp3;*.aiff");
    auto safe = juce::Component::SafePointer<MainComponent> (this);
    playbackFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
          | juce::FileBrowserComponent::canSelectFiles
          | juce::FileBrowserComponent::canSelectMultipleItems,
        [safe] (const juce::FileChooser& chooser)
        {
            if (auto* self = safe.getComponent())
            {
                const auto files = chooser.getResults();
                bool loadedAny = false;
                for (int i = 0; i < juce::jmin (files.size(),
                                                ChurchDAWPlaybackEngine::maxStems); ++i)
                    loadedAny = self->playbackEngine.loadStem (i, files[i]) || loadedAny;

                if (loadedAny)
                {
                    const int scene = juce::isPositiveAndBelow (
                        self->cenaAtiva, ChurchDAWLive::numeroDeCenas)
                        ? self->cenaAtiva : 0;
                    self->playbackEngine.setBpm (self->cenas[scene].bpm);
                    self->playbackEngine.setClickEnabled (false);
                    self->startPlayback();
                    juce::Logger::writeToLog (
                        "PLAYBACK: " + juce::String (juce::jmin (
                            files.size(), ChurchDAWPlaybackEngine::maxStems))
                        + " stem(s) carregado(s)");
                }
                self->playbackFileChooser.reset();
            }
        });
}

void MainComponent::startPlayback()
{
    playbackEngine.start();
    juce::Logger::writeToLog ("PLAYBACK: iniciado");
}

void MainComponent::stopPlayback()
{
    playbackEngine.stop();
    juce::Logger::writeToLog ("PLAYBACK: parado");
}

void MainComponent::showAutoPatchBuilder()
{
    auto options = juce::MessageBoxOptions()
        .withIconType (juce::MessageBoxIconType::InfoIcon)
        .withTitle ("AUTO PATCH WORSHIP")
        .withMessage (juce::String::fromUTF8 (
            u8"Escolha a intenção musical. O ChurchDAW monta uma sugestão nativa e deixa o áudio sob seu controle."))
        .withButton (juce::String::fromUTF8 (u8"ORAÇÃO"))
        .withButton (juce::String::fromUTF8 (u8"MINISTRAÇÃO"))
        .withButton (juce::String::fromUTF8 (u8"CELEBRAÇÃO"))
        .withButton ("CANCELAR")
        .withAssociatedComponent (this);

    juce::AlertWindow::showAsync (
        options,
        [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || result < 1 || result > 3)
                return;

            // O showroom nativo é uma base determinística: piano, pad e
            // strings sem depender de plugin externo ou rede. A escolha de
            // intenção fica registrada para o próximo passo do builder e
            // também aparece no log do culto.
            if (auto* live = self->getChurchLive())
                live->requestShowroomPreset();

            const char* moods[] = { "oracao", "ministracao", "celebracao" };
            juce::Logger::writeToLog (
                "AUTO_PATCH: sugestao worship aplicada, intencao="
                + juce::String (moods[result - 1]) + " intensidade=3");
            self->refreshPerformanceSessionPanel();
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::InfoIcon,
                "AUTO PATCH WORSHIP",
                juce::String::fromUTF8 (
                    u8"Sugestão preparada: piano + pad + camadas worship.\n\nRevise volumes e confirme gravando a cena."));
        });
}

juce::String MainComponent::handleRemoteRequest (const juce::String& rawPath)
{
    auto jsonEscape = [] (juce::String value)
    {
        return value.replace ("\\", "\\\\")
                    .replace ("\"", "\\\"")
                    .replace ("\n", "\\n");
    };

    const auto path = rawPath.upToFirstOccurrenceOf ("?", false, false);
    const auto query = rawPath.fromFirstOccurrenceOf ("?", false, false);
    const auto token = query.fromFirstOccurrenceOf ("token=", false, false)
                           .upToFirstOccurrenceOf ("&", false, false);

    if (path == "/pair")
        return "{\"churchdaw\":\"remote\",\"port\":19765,"
               "\"pairing\":\"manual_token_required\","
               "\"commands\":[\"status\",\"next\",\"previous\",\"panic\",\"safe-piano\",\"scene/N\",\"playback/start\",\"playback/stop\",\"playback/click/on\",\"playback/click/off\"]}";

    if (token != remoteToken)
        return "{\"error\":\"token_required\"}";

    if (path == "/status")
    {
        juce::MessageManagerLock messageLock;
        if (! messageLock.lockWasGained())
            return "{\"error\":\"busy\"}";
        const auto index = juce::isPositiveAndBelow (cenaAtiva,
                                                     ChurchDAWLive::numeroDeCenas)
                             ? cenaAtiva : 0;
        const auto& scene = cenas[index];
        return "{\"scene\":" + juce::String (index + 1)
            + ",\"song\":\"" + jsonEscape (scene.musica)
            + "\",\"section\":\"" + jsonEscape (scene.secao)
            + "\",\"key\":\"" + jsonEscape (scene.tonalidade)
            + "\",\"bpm\":" + juce::String (scene.bpm, 2)
            + ",\"playback\":" + juce::String (playbackEngine.isPlaying() ? "true" : "false")
            + ",\"click\":" + juce::String (playbackEngine.isClickEnabled() ? "true" : "false")
            + ",\"safeMode\":" + juce::String (safeMode ? "true" : "false")
            + "}";
    }

    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this), path]
        {
            if (auto* self = safe.getComponent())
            {
                if (path == "/panic")
                {
                    self->performGlobalPanic();
                }
                else if (path == "/safe-piano")
                    self->activateEmergencyPiano();
                else if (path == "/playback/start")
                    self->startPlayback();
                else if (path == "/playback/stop")
                    self->stopPlayback();
                else if (path == "/playback/click/on")
                    self->playbackEngine.setClickEnabled (true);
                else if (path == "/playback/click/off")
                    self->playbackEngine.setClickEnabled (false);
                else if (path == "/next" || path == "/previous")
                {
                    const int direction = path == "/next" ? 1 : -1;
                    int position = self->setlist.posicao;
                    if (! juce::isPositiveAndBelow (position, self->setlist.tamanho))
                        position = direction > 0 ? -1 : self->setlist.tamanho;
                    const int targetPosition = position + direction;
                    if (juce::isPositiveAndBelow (targetPosition, self->setlist.tamanho))
                        (void) self->aplicarCena (
                            self->setlist.ordem[(size_t) targetPosition]);
                }
                else if (path.startsWith ("/scene/"))
                {
                    const int scene = path.substring (7).getIntValue() - 1;
                    if (juce::isPositiveAndBelow (scene,
                                                  ChurchDAWLive::numeroDeCenas))
                        (void) self->aplicarCena (scene);
                }
            }
        });
    return "{\"ok\":true}";
}

void MainComponent::importCultSession()
{
    auto initial = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    sessionBundleFileChooser = std::make_unique<juce::FileChooser> (
        "Importar culto completo", initial, "*.churchset.xml");
    auto safe = juce::Component::SafePointer<MainComponent> (this);
    sessionBundleFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
          | juce::FileBrowserComponent::canSelectFiles,
        [safe] (const juce::FileChooser& chooser)
        {
            if (auto* self = safe.getComponent())
            {
                const auto selected = chooser.getResult();
                auto bundle = selected != juce::File() ? juce::parseXML (selected)
                                                       : nullptr;
                auto* scenes = bundle != nullptr
                    ? bundle->getChildByName ("LIVESCENES") : nullptr;
                auto* chain = bundle != nullptr
                    ? bundle->getChildByName ("CHAIN") : nullptr;
                const bool valid = bundle != nullptr
                    && bundle->hasTagName ("CHURCHDAW_CULT_SESSION")
                    && bundle->getIntAttribute ("schema", 0) == 1
                    && scenes != nullptr && chain != nullptr
                    && scenes->getIntAttribute ("schema", 0) <= 3;

                bool imported = false;
                if (valid)
                {
                    const auto destination = self->arquivoDeCenas();
                    const auto backup = destination.getSiblingFile (
                        "live-scenes.xml.before-cult-import");
                    const bool backupOk = ! destination.existsAsFile()
                        || destination.copyFileTo (backup);
                    juce::TemporaryFile temp (destination);
                    const bool scenesOk = backupOk
                        && scenes->writeTo (temp.getFile())
                        && temp.overwriteTargetFileWithTemporary();
                    if (scenesOk)
                    {
                        self->appProperties.getUserSettings()->setValue (
                            "chain", chain);
                        imported = self->appProperties.getUserSettings()->saveIfNeeded();
                    }
                    if (! imported && backup.existsAsFile())
                        backup.copyFileTo (destination);
                }

                juce::AlertWindow::showMessageBoxAsync (
                    imported ? juce::MessageBoxIconType::InfoIcon
                             : juce::MessageBoxIconType::WarningIcon,
                    "Importar culto",
                    selected == juce::File() ? "Importacao cancelada."
                      : imported ? "Culto importado. Reinicie o ChurchDAW para aplicar a cadeia com seguranca."
                                 : "Arquivo invalido ou falha ao gravar; o culto anterior foi preservado.",
                    "Entendi");
                self->sessionBundleFileChooser.reset();
            }
        });
}

void MainComponent::importMidiMappings()
{
    auto initial = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    midiMapFileChooser = std::make_unique<juce::FileChooser> (
        "Importar mapeamentos MIDI", initial, "*.xml");

    auto safe = juce::Component::SafePointer<MainComponent> (this);
    midiMapFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe] (const juce::FileChooser& chooser)
        {
            auto* self = safe.getComponent();
            if (self == nullptr)
                return;

            const auto selected = chooser.getResult();
            if (selected == juce::File())
            {
                self->midiMappingPanel.setStatus ("Importacao cancelada",
                                                  MidiMappingPanel::Status::neutral);
                self->midiMapFileChooser.reset();
                return;
            }

            auto bundle = ChurchDAWMidiMapping::loadBoundedXmlFile (
                selected, ChurchDAWMidiMapping::midiMapBundleXmlLimits());
            int ccNodeCount = 0;
            int noteNodeCount = 0;
            if (bundle != nullptr)
                for (auto* child : bundle->getChildIterator())
                {
                    if (child->hasTagName ("CC_MAP"))
                        ++ccNodeCount;
                    else if (child->hasTagName ("NOTE_MAP"))
                        ++noteNodeCount;
                }

            auto* ccNode = bundle != nullptr ? bundle->getChildByName ("CC_MAP") : nullptr;
            auto* noteNode = bundle != nullptr ? bundle->getChildByName ("NOTE_MAP") : nullptr;
            const auto ccText = ccNode != nullptr ? ccNode->getAllSubText() : juce::String();
            const auto noteText = noteNode != nullptr ? noteNode->getAllSubText() : juce::String();
            auto ccXml = ChurchDAWMidiMapping::parseBoundedXmlText (ccText);
            auto noteXml = ChurchDAWMidiMapping::parseBoundedXmlText (noteText);

            const bool valid = bundle != nullptr
                                && bundle->hasTagName ("CHURCHDAW_MIDI_MAP_BUNDLE")
                                && ChurchDAWMidiMapping::hasStrictIntegerAttribute (
                                    *bundle, "version")
                                && bundle->getIntAttribute ("version", 0) == 1
                                && ccNodeCount == 1 && noteNodeCount == 1
                                && bundle->getNumChildElements() == 2
                                && ccXml != nullptr
                                && ChurchDAWMidiMapping::isMidiMapXmlReadable (
                                    *ccXml, "MIDIMAP", "MAP", "CCACAO")
                                && noteXml != nullptr
                                && ChurchDAWMidiMapping::isMidiMapXmlReadable (
                                    *noteXml, "NOTEMAP", "NOTE")
                                && ! ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                                    *ccXml, *noteXml);

            if (! valid)
            {
                self->midiMappingPanel.setStatus (
                    "Arquivo invalido ou criado por uma versao mais nova",
                    MidiMappingPanel::Status::error);
                self->midiMapFileChooser.reset();
                return;
            }

            const auto ccTarget = self->arquivoDoMapaDeCCs();
            const auto noteTarget = self->arquivoDoMapaDeNotas();
            const auto fileHasFutureSchema = [] (const juce::File& file,
                                                  const juce::String& root)
            {
                const auto xml =
                    ChurchDAWMidiMapping::loadBoundedXmlFile (file);
                return xml != nullptr
                    && ChurchDAWMidiMapping::hasFutureMidiMapSchema (*xml, root);
            };
            const bool protectedFutureState = self->esquemaDeCCsDesconhecido
                || self->esquemaDeNotasDesconhecido
                || fileHasFutureSchema (ccTarget, "MIDIMAP")
                || fileHasFutureSchema (noteTarget, "NOTEMAP")
                || fileHasFutureSchema (ccTarget.getSiblingFile (
                       ccTarget.getFileName() + ".last-good"), "MIDIMAP")
                || fileHasFutureSchema (noteTarget.getSiblingFile (
                       noteTarget.getFileName() + ".last-good"), "NOTEMAP");

            if (protectedFutureState)
            {
                self->midiMappingPanel.setStatus (
                    "Existe um mapa de uma versao mais nova; importacao cancelada para protege-lo",
                    MidiMappingPanel::Status::error);
                self->midiMapFileChooser.reset();
                return;
            }

            ChurchDAWMidiMapping::MidiMapPairRollback diskRollback (
                ccTarget, noteTarget, *self->midiMapFileLock,
                &self->midiMapDiskRevision);
            decltype (self->ccMappings) previousCC;
            decltype (self->ccActionMappings) previousActions;
            decltype (self->noteMappings) previousNotes;
            NoteAction previousPendingAction;
            juce::String previousTouchedName;
            juce::String previousTouchedId;
            juce::String previousTouchedPluginId;
            int previousTouchedSlot = -2;
            int previousTouchedParam = -1;
            int previousTouchedInsert = -1;
            bool previousTouchedIdentityUnique = false;
            const bool previousUnknownCC = self->esquemaDeCCsDesconhecido;
            const bool previousUnknownNotes = self->esquemaDeNotasDesconhecido;
            const bool previousBlockedCC = self->bloquearSalvamentoCCVazio;
            const bool previousBlockedNotes = self->bloquearSalvamentoNotasVazio;
            const bool previousProfilesSuspended =
                self->automaticControllerProfilesSuspended;
            const bool previousPendingCCMigration = self->pendingLegacyCCMigration;
            const bool previousPendingNoteMigration = self->pendingLegacyNoteMigration;

            bool checkpointReady = false;
            bool imported = false;
            bool rollbackOk = true;
            bool transactionStarted = false;
            bool ccWasWritten = false;
            bool noteWasWritten = false;
            juce::uint64 ccEpoch = 0;
            juce::uint64 noteEpoch = 0;

            self->beginMidiMapRoutingTransaction();

            checkpointReady = diskRollback.capture ("before-import");
            const bool protectedAfterCheckpoint =
                self->esquemaDeCCsDesconhecido
                || self->esquemaDeNotasDesconhecido
                || fileHasFutureSchema (ccTarget, "MIDIMAP")
                || fileHasFutureSchema (noteTarget, "NOTEMAP")
                || fileHasFutureSchema (ccTarget.getSiblingFile (
                       ccTarget.getFileName() + ".last-good"), "MIDIMAP")
                || fileHasFutureSchema (noteTarget.getSiblingFile (
                       noteTarget.getFileName() + ".last-good"), "NOTEMAP");
            const bool mayInstall = checkpointReady
                && diskRollback.targetsUnchangedSinceCapture()
                && ! protectedAfterCheckpoint
                && ! self->midiMapPairRecoveryFailed;

            if (mayInstall)
            {
                const auto importTransactionId =
                    self->midiMapFileLock->getActiveTransactionId();
                ccXml->setAttribute ("transactionId", importTransactionId);
                noteXml->setAttribute ("transactionId", importTransactionId);
                const auto ccInstallText = ccXml->toString (
                    juce::XmlElement::TextFormat().singleLine());
                const auto noteInstallText = noteXml->toString (
                    juce::XmlElement::TextFormat().singleLine());

                const bool installPayloadsAreInspectable =
                    ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        ccInstallText)
                    && ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        noteInstallText);

                {
                    const juce::ScopedLock sl (self->mapLock);
                    previousCC = self->ccMappings;
                    previousActions = self->ccActionMappings;
                    previousNotes = self->noteMappings;
                    previousPendingAction = self->pendingNoteAction;
                    previousTouchedName = self->lastTouchedParamName;
                    previousTouchedId = self->lastTouchedParamId;
                    previousTouchedPluginId = self->lastTouchedPluginId;
                    previousTouchedSlot = self->lastTouchedSlot.load (
                        std::memory_order_relaxed);
                    previousTouchedParam = self->lastTouchedParam.load (
                        std::memory_order_relaxed);
                    previousTouchedInsert = self->lastTouchedInsert.load (
                        std::memory_order_relaxed);
                    previousTouchedIdentityUnique =
                        self->lastTouchedParameterIdentityUnique.load (
                            std::memory_order_relaxed);
                }

                transactionStarted = installPayloadsAreInspectable;
                const bool ccInstalled = transactionStarted
                    && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccTarget, ccInstallText);
                const bool noteInstalled = ccInstalled
                    && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        noteTarget, noteInstallText);
                auto installedCC =
                    ChurchDAWMidiMapping::loadBoundedXmlFile (ccTarget);
                auto installedNotes =
                    ChurchDAWMidiMapping::loadBoundedXmlFile (noteTarget);
                const bool installedVerified = noteInstalled
                    && installedCC != nullptr
                    && ChurchDAWMidiMapping::isMidiMapXmlReadable (
                        *installedCC, "MIDIMAP", "MAP", "CCACAO")
                    && installedNotes != nullptr
                    && ChurchDAWMidiMapping::isMidiMapXmlReadable (
                        *installedNotes, "NOTEMAP", "NOTE")
                    && ! ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                        *installedCC, *installedNotes)
                    && installedCC->getStringAttribute ("transactionId")
                         == importTransactionId
                    && installedNotes->getStringAttribute ("transactionId")
                         == importTransactionId;

                if (installedVerified)
                {
                    {
                        const juce::ScopedLock sl (self->mapLock);
                        self->ccMappings.clear();
                        self->ccActionMappings.clear();
                        self->noteMappings.clear();
                        self->pendingNoteAction = {};
                        self->lastTouchedParamName.clear();
                        self->lastTouchedParamId.clear();
                        self->lastTouchedPluginId.clear();
                        self->lastTouchedParameterIdentityUnique.store (
                            false, std::memory_order_relaxed);
                    }
                    self->esquemaDeCCsDesconhecido = false;
                    self->esquemaDeNotasDesconhecido = false;
                    self->bloquearSalvamentoCCVazio = false;
                    self->bloquearSalvamentoNotasVazio = false;
                    self->pendingLegacyCCMigration = false;
                    self->pendingLegacyNoteMigration = false;

                    self->restoreMidiMap();
                    self->restoreNoteMap();

                    int restoredCC = 0;
                    int restoredNotes = 0;
                    bool pairConflictAfterRekey = false;
                    {
                        const juce::ScopedLock sl (self->mapLock);
                        restoredCC = (int) self->ccMappings.size()
                                   + (int) self->ccActionMappings.size();
                        restoredNotes = (int) self->noteMappings.size();
                        pairConflictAfterRekey =
                            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                                self->ccMappings, self->noteMappings)
                            || ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                                self->ccMappings, self->ccActionMappings,
                                [] (const AcaoDeCC& action)
                                {
                                    return action.acao.kind != NoteActionKind::none;
                                });
                    }
                    const bool countsMatch =
                        restoredCC == ccXml->getNumChildElements()
                        && restoredNotes == noteXml->getNumChildElements();

                    self->ccDirtyEpoch.markDirty();
                    self->noteDirtyEpoch.markDirty();
                    ccEpoch = self->ccDirtyEpoch.snapshot();
                    noteEpoch = self->noteDirtyEpoch.snapshot();
                    ccWasWritten = countsMatch
                        && ! pairConflictAfterRekey
                        && self->saveMidiMap();
                    noteWasWritten = ccWasWritten
                        && self->saveNoteMap();
                    auto finalCC =
                        ChurchDAWMidiMapping::loadBoundedXmlFile (ccTarget);
                    auto finalNotes =
                        ChurchDAWMidiMapping::loadBoundedXmlFile (noteTarget);
                    auto finalCCBackup =
                        ChurchDAWMidiMapping::loadBoundedXmlFile (
                        ccTarget.getSiblingFile (
                            ccTarget.getFileName() + ".last-good"));
                    auto finalNoteBackup =
                        ChurchDAWMidiMapping::loadBoundedXmlFile (
                        noteTarget.getSiblingFile (
                            noteTarget.getFileName() + ".last-good"));
                    juce::MemoryBlock finalCCBytes;
                    juce::MemoryBlock finalNoteBytes;
                    juce::MemoryBlock finalCCBackupBytes;
                    juce::MemoryBlock finalNoteBackupBytes;
                    const bool exactPairCopies =
                        ChurchDAWMidiMapping::loadBoundedFileData (
                            ccTarget, finalCCBytes,
                            ChurchDAWMidiMapping::maxMidiMapFileBytes)
                        && ChurchDAWMidiMapping::loadBoundedFileData (
                            noteTarget, finalNoteBytes,
                            ChurchDAWMidiMapping::maxMidiMapFileBytes)
                        && ChurchDAWMidiMapping::loadBoundedFileData (
                            ccTarget.getSiblingFile (
                                ccTarget.getFileName() + ".last-good"),
                            finalCCBackupBytes,
                            ChurchDAWMidiMapping::maxMidiMapFileBytes)
                        && ChurchDAWMidiMapping::loadBoundedFileData (
                            noteTarget.getSiblingFile (
                                noteTarget.getFileName() + ".last-good"),
                            finalNoteBackupBytes,
                            ChurchDAWMidiMapping::maxMidiMapFileBytes)
                        && finalCCBytes == finalCCBackupBytes
                        && finalNoteBytes == finalNoteBackupBytes;
                    imported = noteWasWritten
                        && finalCC != nullptr
                        && ChurchDAWMidiMapping::isMidiMapXmlValid (
                            *finalCC, "MIDIMAP", "MAP", "CCACAO")
                        && finalNotes != nullptr
                        && ChurchDAWMidiMapping::isMidiMapXmlValid (
                            *finalNotes, "NOTEMAP", "NOTE")
                        && ! ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                            *finalCC, *finalNotes)
                        && finalCCBackup != nullptr
                        && ChurchDAWMidiMapping::isMidiMapXmlValid (
                            *finalCCBackup, "MIDIMAP", "MAP", "CCACAO")
                        && finalNoteBackup != nullptr
                        && ChurchDAWMidiMapping::isMidiMapXmlValid (
                            *finalNoteBackup, "NOTEMAP", "NOTE")
                        && ! ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                            *finalCCBackup, *finalNoteBackup)
                        && exactPairCopies
                        && finalCC->getStringAttribute ("transactionId")
                            == importTransactionId
                        && finalNotes->getStringAttribute ("transactionId")
                            == importTransactionId;

                    imported = imported && diskRollback.commit();
                    if (imported)
                    {
                        self->finishMidiMapDirtyEpochsAfterPairCommit (
                            ccEpoch, ccWasWritten,
                            noteEpoch, noteWasWritten);
                        const juce::ScopedLock sl (self->mapLock);
                        self->automaticControllerProfilesSuspended =
                            ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                                self->ccMappings, self->midiSourcesCached)
                            || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                                self->ccActionMappings, self->midiSourcesCached)
                            || ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                                    self->noteMappings, self->midiSourcesCached);
                    }
                }
            }

            if (checkpointReady && ! transactionStarted)
            {
                // We did not change any target; close our journal while
                // preserving a concurrent/future-schema writer.
                rollbackOk = diskRollback.closeWithoutMutation();
            }
            else if (transactionStarted && ! imported)
            {
                rollbackOk =
                    ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                        diskRollback, ccTarget, noteTarget);
                const juce::ScopedLock sl (self->mapLock);
                self->ccMappings = std::move (previousCC);
                self->ccActionMappings = std::move (previousActions);
                self->noteMappings = std::move (previousNotes);
                self->pendingNoteAction = previousPendingAction;
                self->lastTouchedParamName = previousTouchedName;
                self->lastTouchedParamId = previousTouchedId;
                self->lastTouchedPluginId = previousTouchedPluginId;
                self->lastTouchedSlot.store (
                    previousTouchedSlot, std::memory_order_relaxed);
                self->lastTouchedParam.store (
                    previousTouchedParam, std::memory_order_relaxed);
                self->lastTouchedInsert.store (
                    previousTouchedInsert, std::memory_order_relaxed);
                self->lastTouchedParameterIdentityUnique.store (
                    previousTouchedIdentityUnique, std::memory_order_relaxed);
                self->esquemaDeCCsDesconhecido = previousUnknownCC;
                self->esquemaDeNotasDesconhecido = previousUnknownNotes;
                self->bloquearSalvamentoCCVazio = previousBlockedCC;
                self->bloquearSalvamentoNotasVazio = previousBlockedNotes;
                self->automaticControllerProfilesSuspended =
                    previousProfilesSuspended;
                self->pendingLegacyCCMigration = previousPendingCCMigration;
                self->pendingLegacyNoteMigration = previousPendingNoteMigration;
                self->bumpMidiMappingGeneration();
            }

            if (imported)
            {
                self->bumpMidiMappingGeneration();
                self->cancelPendingUpdate();
                self->lastMappedCC.store (-1);
                self->lastMappedNote.store (-1);
                self->lastMappedCCAcao.store (-1);
                self->lastCCAcaoRecusada.store (-1);
                self->lastCCAcaoDesmapeada.store (-1);
                self->lastNoteActionRejected.store (-1);
                self->lastParameterMappingRejectedCC.store (-1);
                {
                    const juce::ScopedLock sl (self->mapLock);
                    self->lastTouchedSlot.store (-2, std::memory_order_relaxed);
                    self->lastTouchedParam.store (-1, std::memory_order_relaxed);
                    self->lastTouchedInsert.store (-1, std::memory_order_relaxed);
                    self->lastTouchedParamName.clear();
                    self->lastTouchedParamId.clear();
                    self->lastTouchedPluginId.clear();
                    self->lastTouchedParameterIdentityUnique.store (
                        false, std::memory_order_relaxed);
                }
            }
            self->finishMidiMapRoutingTransaction();

            if (! checkpointReady
                 && diskRollback.hadUninspectableTargetConflict())
                self->bloquearQuartetoMidiDesconhecido (
                    "checkpoint de importacao MIDI");
            if (! rollbackOk
                 || diskRollback.preservedExternalChanges())
                self->midiMapPairRecoveryFailed = true;
            self->reconstruirMascaraDeNotas();
            self->reconstruirMascaraDeCCsDeAcao();

            self->ccPickup.clear();
            self->atualizarEtiquetasDoPad();
            self->atualizarBotoesDeCCDoPad();
            self->refreshMidiMappingPanel();
            const int mappingCount = self->getMidiMappingCount();
            self->midiLearnPanel.setMappingCount (mappingCount);
            self->deviceBar.setMappingCount (mappingCount);
            self->midiMappingPanel.setStatus (
                imported
                    ? "Mapeamentos MIDI importados, verificados e salvos"
                    : rollbackOk
                        ? "Falha na importacao; os mapeamentos anteriores foram restaurados"
                        : "Falha grave ao restaurar a importacao; preserve os backups",
                imported ? MidiMappingPanel::Status::success
                         : MidiMappingPanel::Status::error);
            juce::Logger::writeToLog (
                imported ? "MIDI MAP: importacao transacional concluida"
                         : rollbackOk
                             ? "MIDI MAP: importacao falhou; rollback concluido"
                             : "MIDI MAP: ERRO GRAVE no rollback da importacao");
            self->midiMapFileChooser.reset();
        });
}

//==============================================================================
// LIVE MODE — 10 cenas
//==============================================================================

// A cena tem o proprio numero de canais porque LiveScenes.h nao conhece o
// motor. Se um dia o motor crescer, a cena passaria a ignorar os canais novos
// CALADA — o musico gravaria a cena e o canal 15 simplesmente nao voltaria.
// Melhor quebrar o build aqui do que descobrir isso no culto.
static_assert (ChurchDAWLive::numeroDeCanais == TimbresProcessor::numLayers,
               "LIVE MODE: numeroDeCanais precisa acompanhar TimbresProcessor::numLayers");

ChurchDAWLive::Resultado MainComponent::capturarCena (int indice)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas))
        return ChurchDAWLive::Resultado::indisponivel;

    auto* live = getChurchLive();

    if (live == nullptr)
        return ChurchDAWLive::Resultado::indisponivel;

    // MESMA GUARDA DA APLICACAO, pelo motivo espelhado.
    //
    // Gravar durante uma carga captura um estado transitorio: um segundo
    // depois o resetLayerParametersToDefaults do restore passa por cima e o
    // motor vira outra coisa. A cena ficaria calada e errada — e teria
    // sobrescrito a anterior, que nao volta. Melhor recusar e avisar.
    if (live->isBusyLoading() || live->hasPendingStateRestore())
        return ChurchDAWLive::Resultado::carregando;

    juce::MemoryBlock paginaDoMotor;

    try
    {
        live->getStateInformation (paginaDoMotor);

        if (live->consumeLastStateCaptureHadFailure())
        {
            paginaDoMotor.reset();
            juce::Logger::writeToLog (
                "LIVE: cena " + juce::String (indice + 1)
                + " NAO gravada; estado de plugin incompleto");
            return ChurchDAWLive::Resultado::naoGravou;
        }
    }
    catch (...)
    {
        (void) live->consumeLastStateCaptureHadFailure();
        paginaDoMotor.reset();
    }

    juce::String novoEstadoId;

    if (! gravarPaginaDeCena (paginaDoMotor, novoEstadoId))
        return ChurchDAWLive::Resultado::naoGravou;

    const ChurchDAWLive::EstadoAntesDePersistirCena antes { cenas[indice], cenaAtiva };
    const int posicaoAnteriorDoSetlist = setlist.posicao;
    auto& cena = cenas[indice];
    const auto estadoIdAnterior = cena.estadoDoMotorId;
    cena.estadoDoMotorId = novoEstadoId;

    // SOLO NAO ENTRA NA CENA — ele vira mute.
    //
    // Solo e ferramenta de ensaio e e global: se fosse gravado como esta, o
    // musico ouviria UM canal ao gravar e a cena, que zera o solo ao aplicar,
    // devolveria TODOS. A cena tocaria diferente do que ele ouviu quando
    // apertou GRAVAR — o pior tipo de surpresa. Congelando o solo em mutes, a
    // cena reproduz exatamente o que estava soando.
    const bool haSolo = live->hasAnyLoadedSolo();

    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
    {
        auto& canal = cena.canais[l];

        if (auto* p = live->getLayerOnParam (l))
            canal.ligado = p->getValue() > 0.5f;

        const bool loaded = live->isLayerLoaded (l);
        const bool soloDeste = loaded
                            && live->getLayerSoloParam (l) != nullptr
                            && live->getLayerSoloParam (l)->get();

        // MUTE VENCE SOLO, igual ao motor (audible = on && !mute && (!anySolo
        // || solo)). Escrever so "! soloDeste" transformava um canal que
        // estava mudo E em solo — comparacao de dois patches no ensaio — em
        // canal audivel na cena, e ele voltava tocando.
        if (auto* p = live->getLayerMuteParam (l))
            canal.mudo = p->get()
                      || (loaded && haSolo && ! soloDeste);

        canal.solo = false;   // nunca se grava solo; ver acima

        if (auto* p = live->getLayerVolParam (l))    canal.volume = p->getValue();
        if (auto* p = live->getLayerTransposeParam (l)) canal.transpose = p->get();
        if (auto* p = live->getLayerOctaveParam (l)) canal.octave = p->get();
        if (auto* p = live->getLayerReverbSendParam (l)) canal.envioDeReverb = p->get();

        canal.rotuloDoTimbre = live->getLayerTimbre (l);
    }

    // Globais: o tecladista transpoe +2 para a musica e espera que a cena
    // devolva isso. Sem capturar, o XML gravava valores de fabrica que nunca
    // vieram do motor — mentia.
    if (auto* p = live->getVolumeGeralParam())     cena.volumeGeral = p->get();
    if (auto* p = live->getTransposeGeralParam())  cena.transposeGeral = p->get();
    if (auto* p = live->getRetornoDeReverbParam()) cena.retornoDeReverb = p->get();
    cena.temGlobais = true;

    if (masterProcessor != nullptr)
    {
        cena.masterDb = masterProcessor->getMasterDb();
        cena.temMaster = true;
        cena.masterAutoEq = masterProcessor->getAutoEq();
        cena.temMasterAutoEq = true;
        cena.masterPunch = masterProcessor->getPunch();
        cena.temMasterPunch = true;
    }

    if (padProcessor != nullptr)
    {
        cena.padTom    = padProcessor->getKey();
        cena.padVolume = padProcessor->getGain();
        cena.padCorte  = padProcessor->getCutoff();
    }

    cena.preenchida = true;
    cena.gravadaEm  = juce::Time::getCurrentTime().toISO8601 (true);

    // Gravar TORNA esta a cena corrente: o som que esta no ar agora e
    // exatamente ela. Sem isto o painel fica sem nenhuma cena acesa logo
    // depois de gravar, e no palco o musico perde a referencia de onde esta.
    cenaAtiva = indice;
    setlist.reposicionarPara (indice);

    // GRAVOU NA MEMORIA NAO E GRAVOU.
    //
    // Se o disco recusou (arquivo de versao futura, permissao, disco cheio), a
    // cena existe so nesta sessao e some ao fechar. Dizer "ok" aqui desarmaria
    // o GRAVAR e deixaria o musico confiando numa cena que nao vai estar la no
    // domingo — a mesma confianca falsa que ja custou caro no mapa de notas.
    const auto resultado = ChurchDAWLive::concluirPersistenciaDeCena (
        salvarCenas(), antes, cena, cenaAtiva);

    if (resultado != ChurchDAWLive::Resultado::ok)
    {
        setlist.posicao = posicaoAnteriorDoSetlist;
        removerPaginaDeCenaSeOrfa (novoEstadoId);
        juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1)
                                    + " NAO gravada; memoria e cena ativa restauradas");
        return resultado;
    }

    removerPaginaDeCenaSeOrfa (estadoIdAnterior);
    cenaEmPreparacao = -1;
    liveScenePanel.selecionarCenaAtiva (indice);
    refreshPerformanceSessionPanel();
    juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1) + " gravada");
    return ChurchDAWLive::Resultado::ok;
}

void MainComponent::limparMotorLiveParaPaginaVazia (TimbresProcessor& live)
{
    live.panic();

    for (int slot = 0; slot < TimbresProcessor::numInserts; ++slot)
        live.unloadInsert (slot);

    for (int l = 0; l < TimbresProcessor::numLayers; ++l)
    {
        live.clearLayer (l);

        if (live.getLayerInstrument (l) != nullptr)
            live.clearInstrumentPlugin (l);

        // clearLayer() preserva a regulagem quando o usuario apenas troca um
        // timbre. Uma pagina vazia e deliberadamente diferente: fader no
        // fundo, canal OFF e todos os knobs/rotas em estado neutro.
        live.resetClearedLayerForEmptyScene (l);
    }

    const ChurchDAWLive::Cena paginaNova;

    if (auto* p = live.getVolumeGeralParam())
        *p = paginaNova.volumeGeral;

    if (auto* p = live.getTransposeGeralParam())
        *p = paginaNova.transposeGeral;

    if (auto* p = live.getRetornoDeReverbParam())
        *p = paginaNova.retornoDeReverb;

    if (padProcessor != nullptr)
    {
        padProcessor->setKey (-1);
        padProcessor->setGain (paginaNova.padVolume);
        padProcessor->setCutoff (paginaNova.padCorte);
    }

    if (masterProcessor != nullptr)
    {
        masterProcessor->setMasterDb (paginaNova.masterDb);
        masterProcessor->setAutoEq (paginaNova.masterAutoEq);
        masterProcessor->setPunch (paginaNova.masterPunch);
        masterSlider.setValue (paginaNova.masterDb,
                               juce::dontSendNotification);
    }

    // Faders fisicos continuam na posicao da cena apagada. Exigir pickup no
    // proximo gesto impede que o primeiro toque ressuscite aquele valor.
    ccPickup.clear();

    if (padPanel != nullptr)
        padPanel->refreshState();

    refreshPerformanceSessionPanel();
}

bool MainComponent::zerarTimbresDoProjeto()
{
    auto* live = getChurchLive();

    if (live == nullptr)
        return false;

    // Primeiro persiste que nenhuma cena esta no ar. Se o disco recusar, nao
    // toca no audio: assim o proximo arranque nunca carrega automaticamente
    // uma cena antiga por cima de um setup que o usuario acreditou ter zerado.
    const int ativaAntes = cenaAtiva;
    const int preparacaoAntes = cenaEmPreparacao;
    const int posicaoAntes = setlist.posicao;

    cenaAtiva = -1;
    cenaEmPreparacao = -1;
    setlist.posicao = -1;

    if (! salvarCenas())
    {
        cenaAtiva = ativaAntes;
        cenaEmPreparacao = preparacaoAntes;
        setlist.posicao = posicaoAntes;
        return false;
    }

    ++geracaoDaCena;
    cenaPendenteNoStartup = -1;
    controleDeGravacaoDoSetup.iniciarSupressao (geracaoDaCena);

    limparMotorLiveParaPaginaVazia (*live);
    if (padSynthProcessor != nullptr)
    {
        padSynthProcessor->requestPanic();
        padSynthProcessor->setEngineEnabled (false);
    }
    resetMidiTransportState (false);
    liveScenePanel.limparSelecao();
    liveScenePanel.atualizar (cenas, cenaAtiva);
    refreshPerformanceSessionPanel();
    repaint (desktopSidebarBounds);

    // Reset e um gesto explicito de projeto, portanto o estado vazio tambem
    // precisa sobreviver ao fechamento do app. As cenas salvas permanecem no
    // banco; apenas o que esta no mixer e o setup de abertura sao zerados.
    const bool setupOk = saveChainState();
    agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaCena);

    juce::Logger::writeToLog (setupOk
        ? "LIVE: todos os timbres e canais foram zerados"
        : "LIVE: timbres zerados, mas o setup vazio NAO foi gravado");
    return setupOk;
}

void MainComponent::pedirConfirmacaoParaZerarTimbres()
{
    auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
        juce::MessageBoxIconType::WarningIcon,
        "Zerar todos os timbres",
        juce::String::fromUTF8 (
            "Isso remove os timbres e plugins dos canais e deixa todos os "
            "faders no fundo.\n\nAs cenas que ja foram salvas nao serao "
            "apagadas."),
        "Zerar", "Cancelar", this);

    // A caixa nativa nao compartilha a superficie OpenGL da interface. Assim,
    // ao fechar, ela nao deixa uma copia visual por cima dos canais zerados.
    juce::NativeMessageBox::showAsync (
        options,
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        (int resultado)
        {
            // NativeMessageBox::showAsync devolve o indice do botao: o
            // primeiro (Zerar) e zero e o segundo (Cancelar) e um.
            if (resultado != 0 || safe == nullptr)
                return;

            juce::Timer::callAfterDelay (20, [safe]
            {
                auto* self = safe.getComponent();
                if (self == nullptr)
                    return;

                const bool ok = self->zerarTimbresDoProjeto();
                self->liveScenePanel.avisar (ok
                    ? "TIMBRES ZERADOS - CENAS SALVAS PRESERVADAS"
                    : "NAO FOI POSSIVEL ZERAR COM SEGURANCA");
            });
        });
}

ChurchDAWLive::Resultado MainComponent::aplicarCena (
    int indice, int tentativa, bool paginaDoMotorJaSolicitada)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas))
        return ChurchDAWLive::Resultado::indisponivel;

    // Um pedido NOVO do musico invalida qualquer cadeia de reaplicacao ainda
    // no ar. Fica aqui em cima, e nao dentro do ramo de "carregando", para
    // valer tambem quando a cena entra de primeira: senao uma cadeia velha
    // ainda pendente aplicaria a cena abandonada por cima desta.
    if (tentativa == 0)
    {
        ++geracaoDaCena;

        // Um toque do musico preempta tambem a reconciliacao ainda pendente do
        // startup. A partir daqui esta geracao e a unica dona do recall.
        cenaPendenteNoStartup = -1;
        controleDeGravacaoDoSetup.iniciarSupressao (geracaoDaCena);
    }

    const int geracaoDaRequisicao = geracaoDaCena;

    auto* live = getChurchLive();

    if (live == nullptr)
    {
        agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
        return ChurchDAWLive::Resultado::indisponivel;
    }

    if (tentativa == 0)
        capturarInicioDaTransicao (*live, geracaoDaRequisicao);

    const auto& cena = cenas[indice];
    playbackEngine.setBpm (cena.bpm);

    const auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    const auto reagendar = [this, safeThis, indice, tentativa] (
                               bool paginaJaSolicitada)
    {
        // Adiar sem retomar era um beco: o musico tocava a cena, nada
        // acontecia, e nada mais aconteceria nunca. Uma pagina com Kontakt/AU
        // pode levar bem mais que um sample nativo; 30 s mantem o pedido vivo
        // sem criar um retry infinito se o vendor nao responder.
        constexpr int maximoDeTentativas = 120;

        // GERACAO: so a ULTIMA cena pedida pode vencer.
        //
        // Sem isto, cada toque durante um carregamento criava uma cadeia
        // propria e nenhuma cancelava a outra. O musico tocava a cena 3, via
        // que nada acontecia, tocava a cena 5 — e a cena 5 entrava, e 200 ms
        // depois a cadeia velha aplicava a 3 por cima. O app terminava na
        // musica que ele abandonou, com o painel pulando sozinho.
        //
        // Vale tambem para o caso mais comum: tocar duas vezes na mesma cena
        // (reacao natural quando "nao acontece nada") aplicava a cena duas
        // vezes, e a segunda desfazia qualquer fader mexido no intervalo.
        const auto minhaGeracao = geracaoDaCena;

        if (tentativa < maximoDeTentativas)
        {
            juce::Timer::callAfterDelay (250,
                [ref = safeThis,
                 indice, tentativa, minhaGeracao, paginaJaSolicitada]
                {
                    auto* self = ref.getComponent();

                    if (self == nullptr || self->geracaoDaCena != minhaGeracao)
                        return;   // o musico ja pediu outra cena

                    const auto resultado = self->aplicarCena (
                        indice, tentativa + 1, paginaJaSolicitada);

                    if (resultado == ChurchDAWLive::Resultado::ok
                         || resultado == ChurchDAWLive::Resultado::vazia)
                        self->liveScenePanel.atualizar (self->cenas, self->cenaAtiva);
                    else if (ChurchDAWLive::precisaAvisarResultadoTerminal (resultado))
                    {
                        // Um retry que termina na escrita do indice ativo pode
                        // falhar depois de o primeiro toque ja ter voltado ao
                        // event loop. Sem este ramo havia log/rollback, mas a
                        // faixa ficava muda e o musico acreditava que salvou.
                        self->liveScenePanel.atualizar (self->cenas, self->cenaAtiva);
                        self->liveScenePanel.avisar (
                            ChurchDAWLive::avisoDoResultado (resultado, indice));
                    }
                });
        }
        else
        {
            juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1)
                                        + " desistiu, timbre ainda carregando");

            liveScenePanel.avisar (juce::String::fromUTF8 (
                "TIMBRE DEMOROU \xe2\x80\x94 TOQUE NA CENA DE NOVO"));

            // A pagina alvo pode ter substituido parte do motor antes de um
            // plugin travar. Recarregar a cena que continuou marcada como
            // ativa evita deixar UI=A e audio=hibrido de A/B.
            const int anterior = cenaAtiva;
            bool rollbackSolicitado = false;

            if (juce::isPositiveAndBelow (anterior,
                                          ChurchDAWLive::numeroDeCenas)
                 && anterior != indice && cenas[anterior].preenchida)
            {
                rollbackSolicitado = true;
                juce::MessageManager::callAsync (
                    [ref = safeThis, anterior,
                     geracaoEsperada = minhaGeracao]
                    {
                        if (auto* self = ref.getComponent())
                        {
                            if (self->geracaoDaCena != geracaoEsperada)
                                return;

                            const auto r = self->aplicarCena (anterior);

                            if (r == ChurchDAWLive::Resultado::ok
                                 || r == ChurchDAWLive::Resultado::vazia)
                                self->liveScenePanel.atualizar (
                                    self->cenas, self->cenaAtiva);
                        }
                    });
            }
            else if (! juce::isPositiveAndBelow (
                         anterior, ChurchDAWLive::numeroDeCenas))
            {
                cenaAtiva = -1;
                liveScenePanel.atualizar (cenas, cenaAtiva);
            }

            if (! rollbackSolicitado)
                agendarFimDaSupressaoDeGravacaoDaCena (minhaGeracao);
        }

        return ChurchDAWLive::Resultado::carregando;
    };

    // Um callback atrasado de timbre/plugin restauraria defaults por cima da
    // cena. Espere antes de tocar nos controles; o snapshot desta propria cena
    // e enviado uma unica vez e depois esta mesma guarda espera o commit.
    if (ChurchDAWLive::deveAguardarCena (
            paginaDoMotorJaSolicitada,
            live->isBusyLoading(),
            live->hasPendingStateRestore()))
        return reagendar (true);

    // Cena v1 nao tinha pagina de audio. Antes de sair dela, captura o motor
    // que esta no ar; abrir a Cena 2 nunca apaga a unica copia da Cena 1.
    if (! paginaDoMotorJaSolicitada && indice != cenaAtiva)
    {
        // Uma cena legada/corrompida ainda sem pagina precisa ser capturada
        // antes da troca. Se o motor esta transitorio, esperar e obrigatorio;
        // capturar agora gravaria metade da requisicao anterior como verdade.
        bool ativaPrecisaCaptura = false;

        if (juce::isPositiveAndBelow (cenaAtiva,
                                      ChurchDAWLive::numeroDeCenas)
             && cenas[cenaAtiva].preenchida)
        {
            juce::MemoryBlock paginaAtiva;
            ativaPrecisaCaptura = ! carregarPaginaDeCena (
                cenas[cenaAtiva].estadoDoMotorId, paginaAtiva);
        }

        if (ativaPrecisaCaptura
             && (live->isBusyLoading() || live->hasPendingStateRestore()))
            return reagendar (false);

        if (! assegurarPaginaDaCenaAtiva (*live))
        {
            agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
            return ChurchDAWLive::Resultado::naoGravou;
        }
    }

    if (! cena.preenchida)
    {
        // Grava primeiro que nenhuma cena salva esta no ar. Se o disco recusar,
        // o motor permanece intacto. A pagina anterior ja tem snapshot proprio.
        const int ativaAntes = cenaAtiva;
        cenaAtiva = -1;

        if (! salvarCenas())
        {
            cenaAtiva = ativaAntes;
            agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
            return ChurchDAWLive::Resultado::naoGravou;
        }

        limparMotorLiveParaPaginaVazia (*live);
        cenaEmPreparacao = indice;
        liveScenePanel.selecionarDestinoNovo (indice);
        refreshPerformanceSessionPanel();
        repaint (desktopSidebarBounds);

        agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
        juce::Logger::writeToLog (
            "LIVE: cena " + juce::String (indice + 1)
            + " preparada como pagina vazia");
        return ChurchDAWLive::Resultado::vazia;
    }

    if (! paginaDoMotorJaSolicitada
         && ChurchDAWLive::idDeEstadoDoMotorValido (cena.estadoDoMotorId))
    {
        juce::MemoryBlock pagina;

        if (! carregarPaginaDeCena (cena.estadoDoMotorId, pagina))
        {
            agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
            return ChurchDAWLive::Resultado::paginaInvalida;
        }

        live->panic();

        try
        {
            live->setStateInformation (pagina.getData(), (int) pagina.getSize());
        }
        catch (...)
        {
            const int anterior = cenaAtiva;
            const int geracaoEsperada = geracaoDaCena;
            bool rollbackSolicitado = false;

            if (juce::isPositiveAndBelow (anterior,
                                          ChurchDAWLive::numeroDeCenas)
                 && anterior != indice && cenas[anterior].preenchida)
            {
                rollbackSolicitado = true;
                juce::MessageManager::callAsync (
                    [safe = juce::Component::SafePointer<MainComponent> (this),
                     anterior, geracaoEsperada]
                    {
                        if (auto* self = safe.getComponent())
                            if (self->geracaoDaCena == geracaoEsperada)
                                (void) self->aplicarCena (anterior);
                    });
            }

            if (! rollbackSolicitado)
                agendarFimDaSupressaoDeGravacaoDaCena (geracaoEsperada);

            return ChurchDAWLive::Resultado::paginaInvalida;
        }

        paginaDoMotorJaSolicitada = true;

        if (live->isBusyLoading() || live->hasPendingStateRestore())
            return reagendar (true);
    }

    // PASSO 1 — Pad primeiro: e o unico item que pode precisar ler do disco,
    // e ele mantem o tom antigo tocando ate o novo chegar. Disparar agora
    // esconde essa latencia atras do resto da troca.
    if (padProcessor != nullptr)
    {
        padProcessor->setGain (cena.padVolume);
        padProcessor->setCutoff (cena.padCorte);
        padProcessor->setKey (cena.padTom);
    }

    // PASSO 2 — o que so afeta nota NOVA: seguro em qualquer instante.
    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
    {
        if (auto* p = live->getLayerTransposeParam (l))
            *p = cena.canais[l].transpose;
        if (auto* p = live->getLayerOctaveParam (l))
            *p = cena.canais[l].octave;
    }

    if (cena.temGlobais)
        if (auto* p = live->getTransposeGeralParam())
            *p = cena.transposeGeral;

    // O Pad acompanha o Transpose Global como os demais instrumentos.
    if (padProcessor != nullptr)
        if (auto* p = live->getTransposeGeralParam())
            padProcessor->setTransposeSemis ((int) *p);

    // PASSO 3 — VOLUME, MUTE E SEND ANTES DE LIGAR.
    //
    // A ordem aqui e o que evita um susto audivel. Ligar primeiro e ajustar
    // depois deixa uma janela de um ou dois blocos em que a thread de audio
    // renderiza o canal recem-ligado com o volume da MUSICA ANTERIOR — que
    // pode ser muito maior — e em que todo canal calado por um solo de ensaio
    // volta a ser audivel com os valores velhos. A rampa de 20 ms limita a
    // excursao, mas em bloco de 512 a 48k ela ja percorre metade do caminho
    // num unico bloco: da para ouvir. Chegando com os valores certos ANTES do
    // canal existir, nao ha janela nenhuma.
    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
    {
        const auto& canal = cena.canais[l];

        if (auto* p = live->getLayerVolParam (l))
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, canal.volume));

        if (auto* p = live->getLayerReverbSendParam (l))
            *p = canal.envioDeReverb;

        if (auto* p = live->getLayerMuteParam (l))
            *p = canal.mudo;
    }

    // PASSO 4 — solo zerado em TODOS. O solo e global (contamina canal que a
    // cena nem citou) e e ferramenta de ensaio, nao valor de cena. A captura
    // ja converteu o solo que existia em mutes, entao nada se perde.
    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
        if (auto* p = live->getLayerSoloParam (l))
            *p = false;

    // PASSO 5 — LIGAR, agora que cada canal ja tem o volume da cena nova.
    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
        if (cena.canais[l].ligado)
            if (auto* p = live->getLayerOnParam (l))
                p->setValueNotifyingHost (1.0f);

    // Globais que faltavam: sem elas a cena devolvia o timbre certo com o
    // volume geral e o reverb da musica anterior. Cena antiga (sem o bloco
    // LIVE no arquivo) nao mexe em nada — ver o comentario de temGlobais.
    if (cena.temGlobais)
    {
        if (auto* p = live->getVolumeGeralParam())
            *p = cena.volumeGeral;

        if (auto* p = live->getRetornoDeReverbParam())
            *p = cena.retornoDeReverb;
    }

    // PASSO 6 — DESLIGAR por ultimo. E aqui que a musica anterior sai, e a
    // rampa do pos-fader faz a cauda decair em vez de ser cortada.
    for (int l = 0; l < ChurchDAWLive::numeroDeCanais; ++l)
        if (! cena.canais[l].ligado)
            if (auto* p = live->getLayerOnParam (l))
                p->setValueNotifyingHost (0.0f);

    // PASSO 7 — master, com a MESMA guarda de -infinito do restore do setup.
    //
    // Faltava aqui. Se o fade da licenca rodou, ou se o musico deixou o master
    // no fim enquanto ajustava, a cena gravava -60 dB e passava a emudecer
    // TUDO toda vez que fosse chamada — persistida, sem desfazer. O restore do
    // chain ja tratava isso como artefato; a cena tem de tratar igual.
    if (cena.temMaster && masterProcessor != nullptr)
    {
        // MASTER NO FUNDO: AVISA, NAO CORRIGE.
        //
        // Tentei corrigir para 0 dB, copiando o restore do setup. E errado
        // aqui: -60 dB e o FIM DE CURSO do fader, nao so o artefato do fade de
        // licenca. Uma cena gravada de proposito com o master no fundo — uma
        // oracao, uma passagem so de voz — voltaria com +60 dB de salto na
        // PA. Entre "a cena silenciou e o musico nao entende" e "a cena
        // estourou o sistema de som", a segunda e pior e nao tem desfazer.
        //
        // Entao aplica o que foi gravado e conta na tela o que houve.
        if (cena.masterDb <= MasterProcessor::minusInfinityDb + 0.5f)
            juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1)
                                        + " tem o master no fundo (-inf)");

        masterProcessor->setMasterDb (cena.masterDb);
        masterSlider.setValue (cena.masterDb, juce::dontSendNotification);

        if (cena.temMasterAutoEq)
            masterProcessor->setAutoEq (cena.masterAutoEq);

        if (cena.temMasterPunch)
            masterProcessor->setPunch (cena.masterPunch);
    }

    // PASSO 8 — REARMAR O SOFT-TAKEOVER.
    //
    // A cena acabou de mover volume, master e Pad por baixo dos controles
    // fisicos, que continuam parados onde a musica anterior deixou. Sem zerar
    // o pickup, o primeiro encostar no fader NAO seria um ajuste: seria um
    // salto instantaneo do valor da cena de volta para a posicao fisica do
    // fader. Fader em 100%, cena colocou o Pad em 30%, o dedo raspa no fader
    // e o Pad volta a 100% no meio do louvor. O pickup existe exatamente para
    // impedir esse salto, e trocar de cena e o momento em que ele mais
    // importa — e o unico em que TUDO se desloca de uma vez.
    //
    // Message thread: mesma regra do clearMapButton e do handleAsyncUpdate.
    ccPickup.clear();

    const int ativaAntes = cenaAtiva;
    const int posicaoAnteriorDoSetlist = setlist.posicao;
    cenaAtiva = indice;
    setlist.reposicionarPara (indice);

    // UMA fonte de verdade para a cena corrente: o mesmo XML que guarda as 10.
    // Guardar o indice numa chave separada ja custou um bug — a chave era
    // escrita aqui e ninguem a lia, entao ao reabrir o app nenhuma cena
    // aparecia acesa mesmo depois de uma noite inteira tocando numa delas.
    if (! salvarCenas())
    {
        cenaAtiva = ativaAntes;
        setlist.posicao = posicaoAnteriorDoSetlist;
        liveScenePanel.atualizar (cenas, cenaAtiva);
        bool rollbackSolicitado = false;

        if (juce::isPositiveAndBelow (ativaAntes,
                                      ChurchDAWLive::numeroDeCenas)
             && ativaAntes != indice && cenas[ativaAntes].preenchida)
        {
            rollbackSolicitado = true;
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<MainComponent> (this),
                 ativaAntes,
                 geracaoEsperada = geracaoDaCena]
                {
                    if (auto* self = safe.getComponent())
                    {
                        if (self->geracaoDaCena != geracaoEsperada)
                            return;

                        const auto r = self->aplicarCena (ativaAntes);

                        if (r == ChurchDAWLive::Resultado::ok
                             || r == ChurchDAWLive::Resultado::vazia)
                            self->liveScenePanel.atualizar (
                                self->cenas, self->cenaAtiva);
                    }
                });
        }

        if (! rollbackSolicitado)
            agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);

        juce::Logger::writeToLog (
            "LIVE: cena " + juce::String (indice + 1)
            + " aplicada, mas indice ativo NAO foi gravado; rollback solicitado");
        return ChurchDAWLive::Resultado::naoGravou;
    }

    cenaEmPreparacao = -1;
    liveScenePanel.selecionarCenaAtiva (indice);

    // NAO agendar a gravacao do setup aqui. Ja tentei, e o remedio era muito
    // pior que a doenca.
    //
    // saveChainState() chama suspendProcessing(true) em cada slot, e o
    // AudioProcessorGraph zera o buffer de no suspenso: 2 s depois de CADA
    // troca de cena o instrumento era suspenso e saia SILENCIO pela saida
    // enquanto o estado ia para o disco — com um Kontakt carregado, dezenas ou
    // centenas de milissegundos de buraco no comeco de cada musica. Trocar um
    // travamento de tela por um corte no audio ao vivo e um mau negocio.
    //
    // O que se perde sem isso: o Pad e o master que a cena deixou no ar nao
    // entram no "chain" ate o proximo salvamento normal (mexer num fader,
    // botao Salvar, fechar o app). A CENA em si esta a salvo — ela mora em
    // live-scenes.xml e e gravada na hora.

    if (padPanel != nullptr)
        padPanel->refreshState();

    refreshPerformanceSessionPanel();

    const bool transitionStarted = iniciarTransicaoDaCena (
        cena, geracaoDaRequisicao);
    if (! transitionStarted)
        agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
    juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1) + " aplicada");
    return ChurchDAWLive::Resultado::ok;
}

void MainComponent::renomearCena (int indice, juce::String nome)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas)
         || ! cenas[indice].preenchida)
        return;

    const ChurchDAWLive::EstadoAntesDePersistirCena antes { cenas[indice], cenaAtiva };
    cenas[indice].nome = nome.trim().substring (0, 42);

    const auto resultado = ChurchDAWLive::concluirPersistenciaDeCena (
        salvarCenas(), antes, cenas[indice], cenaAtiva);
    liveScenePanel.atualizar (cenas, cenaAtiva);

    // Mesma regra do capturarCena: renomear que nao chegou ao disco volta ao
    // nome antigo no proximo arranque, e o musico so descobre no domingo.
    if (resultado != ChurchDAWLive::Resultado::ok)
        liveScenePanel.avisar (
            ChurchDAWLive::avisoDoResultado (resultado, indice));

    refreshPerformanceSessionPanel();
}

void MainComponent::atualizarMetadadosDaCena (
    int indice, const ChurchDAWLive::Cena& novosDados)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas))
        return;

    const auto anterior = cenas[indice];
    auto& cena = cenas[indice];
    cena.musica = novosDados.musica.trim().substring (0, 56);
    cena.nome = novosDados.nome.trim().substring (0, 42);
    cena.tonalidade = novosDados.tonalidade.trim().substring (0, 8);
    cena.compasso = novosDados.compasso.trim().substring (0, 8);
    cena.secao = novosDados.secao.trim().substring (0, 28);
    cena.bpm = juce::jlimit (30.0, 300.0, novosDados.bpm);
    cena.duracaoSegundos = juce::jlimit (
        0.0, 86400.0, novosDados.duracaoSegundos);
    cena.transicaoSegundos = juce::jlimit (
        0.0, 15.0, novosDados.transicaoSegundos);

    if (cena.tonalidade.isEmpty()) cena.tonalidade = "C";
    if (cena.compasso.isEmpty()) cena.compasso = "4/4";
    if (cena.secao.isEmpty()) cena.secao = "Cena";

    if (! salvarCenas())
        cena = anterior;

    liveScenePanel.atualizar (cenas, cenaAtiva);
    refreshPerformanceSessionPanel();
    repaint (desktopSidebarBounds);
    repaint (desktopHeaderExtrasBounds);
}

void MainComponent::atualizarSetlist (
    const ChurchDAWLive::Setlist& novoSetlist)
{
    const auto anterior = setlist;
    setlist = novoSetlist;
    if (juce::isPositiveAndBelow (cenaAtiva, ChurchDAWLive::numeroDeCenas))
        setlist.reposicionarPara (cenaAtiva);

    if (! salvarCenas())
        setlist = anterior;

    refreshPerformanceSessionPanel();
    resized();
    repaint (desktopSidebarBounds);
}

void MainComponent::apagarCena (int indice)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas)
         || ! cenas[indice].preenchida)
        return;

    const auto rotulo = ChurchDAWLive::rotuloDaCena (cenas[indice], indice);

    // APAGAR PEDE CONFIRMACAO.
    //
    // Gravar por cima ao menos deixa uma cena no lugar; apagar nao deixa nada,
    // e nao ha desfazer. O toque longo ja e deliberado, mas o item do menu
    // fica ao lado de "Renomear" — o custo de um toque a mais e pequeno perto
    // de perder o ajuste de uma musica inteira.
    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::WarningIcon,
        "Apagar cena " + juce::String (indice + 1),
        juce::String::fromUTF8 ("A cena \"") + rotulo
            + juce::String::fromUTF8 ("\" ser\xc3\xa1 apagada. Isso n\xc3\xa3o tem como desfazer."),
        "Apagar", "Cancelar", this,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<MainComponent> (this),
             indice] (int resultado)
            {
                auto* self = safe.getComponent();

                if (resultado != 1 || self == nullptr)
                    return;

                const ChurchDAWLive::EstadoAntesDePersistirCena antes {
                    self->cenas[indice], self->cenaAtiva
                };
                const auto setlistAntes = self->setlist;
                const auto paginaAnterior =
                    self->cenas[indice].estadoDoMotorId;
                const bool eraCenaAtiva = self->cenaAtiva == indice;
                self->cenas[indice] = ChurchDAWLive::Cena{};   // volta a ser slot vazio

                for (int i = self->setlist.tamanho; --i >= 0;)
                    if (self->setlist.ordem[(size_t) i] == indice)
                        self->setlist.removerNaPosicao (i);

                // A cena apagada nao pode continuar acesa como "tocando".
                if (self->cenaAtiva == indice)
                    self->cenaAtiva = -1;

                const auto persistencia = ChurchDAWLive::concluirPersistenciaDeCena (
                    self->salvarCenas(), antes,
                    self->cenas[indice], self->cenaAtiva);
                const bool gravou = persistencia == ChurchDAWLive::Resultado::ok;
                if (! gravou)
                    self->setlist = setlistAntes;
                self->liveScenePanel.atualizar (self->cenas, self->cenaAtiva);
                self->refreshPerformanceSessionPanel();
                self->repaint (self->desktopSidebarBounds);

                if (gravou)
                {
                    self->removerPaginaDeCenaSeOrfa (paginaAnterior);

                    // Apagar um slot INATIVO nao toca no som que esta no ar.
                    // Apagar a cena ATIVA, entretanto, precisa cumprir o gesto
                    // por inteiro: remove timbres/plugins, desliga canais e
                    // deixa todos os faders no fundo. Antes o painel apagava o
                    // nome da cena, mas o mixer antigo continuava tocando.
                    if (eraCenaAtiva)
                    {
                        self->cenaEmPreparacao = -1;
                        ++self->geracaoDaCena;
                        self->cenaPendenteNoStartup = -1;
                        self->controleDeGravacaoDoSetup.iniciarSupressao (
                            self->geracaoDaCena);

                        if (auto* live = self->getChurchLive())
                            self->limparMotorLiveParaPaginaVazia (*live);

                        self->liveScenePanel.limparSelecao();

                        self->agendarFimDaSupressaoDeGravacaoDaCena (
                            self->geracaoDaCena);
                    }
                }

                // Apagar que nao foi ao disco e pior que gravar que nao foi: a
                // cena some da tela e VOLTA INTEIRA no proximo arranque.
                if (! gravou)
                    self->liveScenePanel.avisar (
                        ChurchDAWLive::avisoDoResultado (persistencia, indice));

                juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1)
                                            + (gravou ? " apagada"
                                                      : " NAO apagada; memoria restaurada"));
            }));
}

// UMA implementacao do backup de arquivo ilegivel, usada pelos tres (cenas,
// mapa de notas, mapa de CCs).
//
// Estavam copiadas tres vezes e cada copia divergia da outra — foi assim que
// duas delas ficaram sem a guarda de 0 byte e as tres ficaram com a ordem
// errada (apagar o backup antes de conseguir o novo). A regra:
//
//   1. arquivo vazio NAO vira backup (queda de energia deixa 0 byte, e copiar
//      isso por cima do backup bom apaga justamente o que se queria salvar);
//   2. move primeiro, apaga o backup antigo so se o move exigir;
//   3. se nada der certo, DIZ isso — antes a falha do move era muda e o log
//      afirmava que a copia estava guardada.
void MainComponent::guardarCopiaDeArquivoIlegivel (const juce::File& arquivo,
                                                   const juce::String& rotulo)
{
    if (! arquivo.existsAsFile())
        return;

    const auto guardado = ChurchDAWMidiMapping::uniqueCorruptCopyTarget (arquivo);

    if (arquivo.getSize() <= 0
         || ! ChurchDAWMidiMapping::atomicCopyFile (arquivo, guardado))
    {
        juce::Logger::writeToLog (rotulo + ": arquivo ILEGIVEL e sem copia nova "
                                           "(o backup anterior foi preservado)");
        return;
    }

    juce::Logger::writeToLog (rotulo + ": arquivo ILEGIVEL; copia em "
                                + guardado.getFileName());
}

// Arquivo de versao futura = a sessao abre SEM aquilo, e isso tem de aparecer
// na TELA. So o log nao serve: ninguem abre log no domingo, e o sintoma
// (nenhum knob responde, nenhuma cena existe) parece defeito do app.
void MainComponent::avisarSobreArquivosDeVersaoFutura()
{
    juce::StringArray perdidos;
    bool cenasIlegiveis = false;

    if (esquemaDeCenasDesconhecido)
    {
        const auto arquivo = arquivoDeCenas();
        auto xml = juce::parseXML (arquivo);
        cenasIlegiveis = arquivo.existsAsFile()
                      && (xml == nullptr || ! xml->hasTagName ("LIVESCENES"));
    }

    if (esquemaDeCCsDesconhecido)   perdidos.add ("os mapeamentos de knobs/faders");
    if (esquemaDeNotasDesconhecido) perdidos.add ("os mapeamentos de teclas");
    if (esquemaDeCenasDesconhecido)
        perdidos.add (cenasIlegiveis
                        ? "as 10 cenas do Live Mode (arquivo danificado)"
                        : "as 10 cenas do Live Mode");

    if (perdidos.isEmpty())
        return;

    juce::AlertWindow::showMessageBoxAsync (
        juce::MessageBoxIconType::WarningIcon,
        cenasIlegiveis
            ? juce::String::fromUTF8 ("Arquivo de cenas protegido")
            : juce::String::fromUTF8 ("Arquivo de uma vers\xc3\xa3o mais nova"),
        juce::String::fromUTF8 ("Esta vers\xc3\xa3o do ChurchDAW n\xc3\xa3o consegue ler ")
            + perdidos.joinIntoString (", ")
            + (cenasIlegiveis
                ? juce::String::fromUTF8 (
                    ".\n\nO arquivo original foi preservado e o ChurchDAW bloqueou novas "
                    "grava\xc3\xa7\xc3\xb5" "es de cenas para n\xc3\xa3o substitu\xc3\xad-lo por um banco vazio. "
                    "Guarde o original e, se existir, a c\xc3\xb3pia .corrompido para recupera\xc3\xa7\xc3\xa3o.")
                : juce::String::fromUTF8 (
                    ".\n\nEles foram gravados por uma vers\xc3\xa3o mais recente. Nada foi "
                    "apagado — o app apenas n\xc3\xa3o vai us\xc3\xa1-los nem grav\xc3\xa1-los, "
                    "para n\xc3\xa3o destruir o que est\xc3\xa1 l\xc3\xa1.\n\n"
                    "Atualize o ChurchDAW para voltar a us\xc3\xa1-los.")),
        "Entendi");
}

juce::File MainComponent::arquivoDeCenas()
{
    return appProperties.getUserSettings()->getFile().getSiblingFile ("live-scenes.xml");
}

juce::File MainComponent::pastaDePaginasDeCenas()
{
    return arquivoDeCenas().getSiblingFile ("live-scene-pages");
}

juce::File MainComponent::arquivoDaPaginaDeCena (const juce::String& estadoId)
{
    if (! ChurchDAWLive::idDeEstadoDoMotorValido (estadoId))
        return {};

    return pastaDePaginasDeCenas().getChildFile (
        estadoId.toLowerCase() + ".churchscene");
}

bool MainComponent::gravarPaginaDeCena (const juce::MemoryBlock& estado,
                                        juce::String& estadoId)
{
    estadoId.clear();

    if (! ChurchDAWLive::paginaDeCenaTemTamanhoValido (
            static_cast<std::uint64_t> (estado.getSize())))
        return false;

    estadoId = juce::SHA256 (estado).toHexString().toLowerCase();
    auto destino = arquivoDaPaginaDeCena (estadoId);

    if (destino == juce::File{})
        return false;

    auto pasta = destino.getParentDirectory();

    if (pasta.createDirectory().failed())
        return false;

    // Conteudo enderecado pelo hash: duas cenas identicas compartilham o
    // mesmo arquivo. Antes de reutilizar, confirma que nao existe um arquivo
    // truncado com o nome certo deixado por disco/queda antiga.
    if (destino.existsAsFile())
    {
        juce::MemoryBlock existente;

        if (destino.loadFileAsData (existente)
             && ! existente.isEmpty()
             && juce::SHA256 (existente).toHexString().equalsIgnoreCase (estadoId))
            return true;
    }

    juce::TemporaryFile temp (destino);
    const bool gravou = temp.getFile().replaceWithData (
                            estado.getData(), estado.getSize())
                     && temp.overwriteTargetFileWithTemporary();

    if (! gravou)
    {
        estadoId.clear();
        juce::Logger::writeToLog (
            "LIVE: FALHA ao gravar pagina de cena " + destino.getFullPathName());
    }

    return gravou;
}

bool MainComponent::carregarPaginaDeCena (const juce::String& estadoId,
                                          juce::MemoryBlock& estado)
{
    estado.reset();
    const auto arquivo = arquivoDaPaginaDeCena (estadoId);
    const auto tamanho = arquivo == juce::File{} ? 0 : arquivo.getSize();

    if (arquivo == juce::File{}
         || ! arquivo.existsAsFile()
         || ! ChurchDAWLive::paginaDeCenaTemTamanhoValido (
                static_cast<std::uint64_t> (tamanho > 0 ? tamanho : juce::int64{}))
         || ! arquivo.loadFileAsData (estado)
         || estado.isEmpty())
        return false;

    if (! juce::SHA256 (estado).toHexString().equalsIgnoreCase (estadoId))
    {
        estado.reset();
        juce::Logger::writeToLog (
            "LIVE: pagina de cena com SHA-256 invalido " + arquivo.getFileName());
        return false;
    }

    return true;
}

void MainComponent::removerPaginaDeCenaSeOrfa (const juce::String& estadoId)
{
    if (! ChurchDAWLive::idDeEstadoDoMotorValido (estadoId))
        return;

    for (const auto& cena : cenas)
        if (cena.estadoDoMotorId.equalsIgnoreCase (estadoId))
            return;

    const auto arquivo = arquivoDaPaginaDeCena (estadoId);

    if (arquivo.existsAsFile() && ! arquivo.deleteFile())
        juce::Logger::writeToLog (
            juce::String::fromUTF8 (u8"LIVE: pagina orfa não pôde ser removida ")
                + arquivo.getFileName());
}

bool MainComponent::assegurarPaginaDaCenaAtiva (TimbresProcessor& live)
{
    if (! juce::isPositiveAndBelow (cenaAtiva, ChurchDAWLive::numeroDeCenas)
         || ! cenas[cenaAtiva].preenchida)
        return true;

    auto& ativa = cenas[cenaAtiva];
    juce::MemoryBlock existente;

    if (carregarPaginaDeCena (ativa.estadoDoMotorId, existente))
        return true;

    juce::MemoryBlock estado;

    try
    {
        live.getStateInformation (estado);

        if (live.consumeLastStateCaptureHadFailure())
        {
            juce::Logger::writeToLog (
                "LIVE: pagina da cena ativa NAO capturada; estado de plugin incompleto");
            return false;
        }
    }
    catch (...)
    {
        (void) live.consumeLastStateCaptureHadFailure();
        estado.reset();
    }

    juce::String novoId;

    if (! gravarPaginaDeCena (estado, novoId))
        return false;

    const auto idAnterior = ativa.estadoDoMotorId;
    const auto antes = ativa;
    ativa.estadoDoMotorId = novoId;

    if (! salvarCenas())
    {
        ativa = antes;
        removerPaginaDeCenaSeOrfa (novoId);
        return false;
    }

    removerPaginaDeCenaSeOrfa (idAnterior);
    juce::Logger::writeToLog (
        "LIVE: cena ativa legada ganhou pagina de audio " + novoId.substring (0, 12));
    return true;
}

bool MainComponent::salvarCenas()
{
    // Mesma trava do mapa de notas: arquivo de uma versao futura ou ilegivel
    // nao se sobrescreve com as regras de hoje.
    if (esquemaDeCenasDesconhecido)
    {
        juce::Logger::writeToLog ("LIVE: gravacao ignorada, o arquivo de cenas em disco "
                                  "e de uma versao mais nova ou esta ilegivel");
        return false;
    }

    juce::XmlElement xml ("LIVESCENES");
    xml.setAttribute ("schema", 3);   // v3 normaliza o default legado do Pad Church
    xml.setAttribute ("active", cenaAtiva);

    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        ChurchDAWLive::escreverCena (xml, cenas[i], i);

    ChurchDAWLive::escreverSetlist (xml, setlist);

    // ARQUIVO PROPRIO, fora do settings.
    //
    // O settings guarda a chave "chain", e nela vai o estado binario de cada
    // plugin em base64 — com um Kontakt carregado sao varios MB. Enquanto as
    // cenas moravam la, trocar de cena obrigava a reescrever esse arquivo
    // inteiro, sincronamente, na thread da interface, no instante exato da
    // troca de musica: no tablet isso e a tela congelando no pior momento
    // possivel. Aqui o arquivo tem poucos KB e a escrita nao aparece.
    auto destino = arquivoDeCenas();
    destino.getParentDirectory().createDirectory();

    // Grava em temporario e so entao substitui: falta de energia no meio da
    // escrita nao pode deixar o musico sem as 10 cenas.
    juce::TemporaryFile temp (destino);
    const bool gravou = xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();

    if (! gravou)
        juce::Logger::writeToLog ("LIVE: FALHA ao gravar " + destino.getFullPathName());

    return gravou;
}

void MainComponent::restaurarCenas()
{
    const auto arquivo = arquivoDeCenas();
    auto xml = juce::parseXML (arquivo);
    const bool arquivoPrincipalIlegivel =
        arquivo.existsAsFile()
        && (xml == nullptr || ! xml->hasTagName ("LIVESCENES"));

    // ARQUIVO ILEGIVEL NAO PODE PARECER "NUNCA EXISTIU".
    //
    // parseXML devolve nullptr tanto para ausente quanto para truncado ou
    // corrompido, e a raiz pode ate estar errada. Seguindo calado, o app
    // abriria com 10 slots vazios e a PRIMEIRA gravacao passaria por cima do
    // arquivo — as cenas do cliente sumiriam de vez, sem uma linha de log.
    // Guardar a evidencia de lado custa nada e ainda deixa uma copia para
    // recuperar a mao.
    if (arquivoPrincipalIlegivel)
    {
        guardarCopiaDeArquivoIlegivel (arquivo, "LIVE (cenas)");
        xml = nullptr;
    }

    bool veioDoFormatoAntigo = false;

    // MIGRACAO: as primeiras versoes guardavam as cenas dentro do settings.
    // Quem ja gravou cenas la nao pode perde-las ao atualizar. Vem DEPOIS do
    // tratamento acima para que um arquivo novo corrompido ainda deixe a
    // migracao tentar resgatar o formato antigo.
    if (xml == nullptr)
    {
        xml = appProperties.getUserSettings()->getXmlValue ("liveScenes");
        veioDoFormatoAntigo = xml != nullptr && xml->hasTagName ("LIVESCENES");

        if (! veioDoFormatoAntigo)
            xml = nullptr;
    }

    // Sem arquivo e sem chave = setup antigo, sem cenas. Abre com 10 slots
    // vazios e zero efeito no audio — nenhuma cena e aplicada na largada.
    if (xml == nullptr || ! xml->hasTagName ("LIVESCENES"))
    {
        if (ChurchDAWLive::deveBloquearGravacaoAposArquivoIlegivel (
                arquivoPrincipalIlegivel, veioDoFormatoAntigo))
        {
            // Reutiliza a trava persistente ja consultada por salvarCenas(). O
            // arquivo original permanece intacto e o botao Salvar reporta a
            // recusa, em vez de substituir as cenas por um banco vazio.
            esquemaDeCenasDesconhecido = true;
            juce::Logger::writeToLog (
                "LIVE: arquivo de cenas ILEGIVEL; gravacao bloqueada para preservar o original");
        }

        return;
    }

    const int sceneSchema = xml->getIntAttribute ("schema", 1);

    if (sceneSchema > 3)
    {
        // Igual ao mapa de notas: nao le E nao grava. Sem isto, o primeiro
        // toque de GRAVAR reescreveria no formato velho o arquivo que uma
        // versao mais nova deixou.
        esquemaDeCenasDesconhecido = true;

        juce::Logger::writeToLog ("LIVE: arquivo de cenas e de uma versao mais nova "
                                  "(schema " + xml->getStringAttribute ("schema")
                                  + "); leitura e gravacao suspensas");
        return;
    }

    esquemaDeCenasDesconhecido = false;
    bool migratedLegacyPadOctave = false;

    cenaAtiva = juce::jlimit (-1, ChurchDAWLive::numeroDeCenas - 1,
                              xml->getIntAttribute ("active", -1));

    for (auto* c : xml->getChildWithTagNameIterator ("SCENE"))
    {
        const int i = c->getIntAttribute ("index", -1);

        if (juce::isPositiveAndBelow (i, ChurchDAWLive::numeroDeCenas))
        {
            std::array<int, ChurchDAWLive::numeroDeCanais> beforeOctaves {};

            for (int layer = 0; layer < ChurchDAWLive::numeroDeCanais; ++layer)
                beforeOctaves[(size_t) layer] = cenas[i].canais[layer].octave;

            ChurchDAWLive::lerCena (*c, cenas[i], sceneSchema < 3);

            for (int layer = 0; layer < ChurchDAWLive::numeroDeCanais; ++layer)
                migratedLegacyPadOctave = migratedLegacyPadOctave
                    || beforeOctaves[(size_t) layer]
                           != cenas[i].canais[layer].octave;
        }
    }

    ChurchDAWLive::lerSetlist (*xml, setlist);

    if (migratedLegacyPadOctave && ! salvarCenas())
        juce::Logger::writeToLog (
            "LIVE: migracao da oitava do Pad Church sera repetida na proxima abertura");

    // Uma cena que aponta para um slot vazio nao pode ficar acesa.
    if (cenaAtiva >= 0 && ! cenas[cenaAtiva].preenchida)
        cenaAtiva = -1;

    // Termina a migracao: grava no formato novo e so DEPOIS solta a chave
    // antiga. Nessa ordem, uma queda no meio do caminho deixa o usuario com
    // as cenas no lugar velho — nunca com nenhum dos dois.
    if (veioDoFormatoAntigo)
    {
        // So solta a chave antiga se a gravacao NOVA deu certo. Perguntar "o
        // arquivo existe?" respondia sim para o arquivo corrompido que motivou
        // a migracao — e apagava a unica copia boa das 10 cenas do culto.
        if (salvarCenas())
        {
            appProperties.getUserSettings()->removeValue ("liveScenes");
            appProperties.getUserSettings()->removeValue ("liveSceneActive");   // chave orfa antiga
            juce::Logger::writeToLog ("LIVE: cenas migradas para " + arquivoDeCenas().getFileName());
        }
        else
        {
            juce::Logger::writeToLog ("LIVE: migracao adiada, as cenas continuam no setup");
        }
    }

    // O indice do XML e apenas a INTENCAO persistida. Marcar a cena agora
    // deixaria UI=A enquanto o restoreChainState ainda entrega o audio de B.
    // A pagina pelo SHA sera aplicada depois do graph/catalogo ficarem prontos;
    // ate la nenhuma cena aparece como ativa.
    if (juce::isPositiveAndBelow (cenaAtiva, ChurchDAWLive::numeroDeCenas)
         && cenas[cenaAtiva].preenchida)
    {
        cenaPendenteNoStartup = cenaAtiva;
        cenaAtiva = -1;
    }
}

void MainComponent::reconciliarCenaAtivaNoStartup (int tentativa)
{
    if (componentShutdownStarted.load (std::memory_order_acquire))
        return;

    if (! juce::isPositiveAndBelow (cenaPendenteNoStartup,
                                    ChurchDAWLive::numeroDeCenas))
        return;

    constexpr int maximoDeTentativas = 120;   // 30 s, igual ao recall normal
    const int indice = cenaPendenteNoStartup;
    auto* live = getChurchLive();
    const auto plano = ChurchDAWLive::planejarReconciliacaoNoStartup (
        cenas[indice].preenchida,
        ChurchDAWLive::idDeEstadoDoMotorValido (
            cenas[indice].estadoDoMotorId));

    // isBusyLoading cobre o scan do catalogo, a fila de samples e criacoes
    // AU/VST em voo. hasPendingStateRestore nao entra aqui: um plugin ausente
    // pode ficar pendente indefinidamente, e a pagina da cena e justamente a
    // nova fonte de verdade que deve substituir essa intencao antiga.
    if (live == nullptr || live->isBusyLoading()
         || (plano.aplicarCenaAntesDeAnexarPagina
              && live->hasPendingStateRestore()))
    {
        if (tentativa < maximoDeTentativas)
        {
            juce::Timer::callAfterDelay (
                250,
                [safe = juce::Component::SafePointer<MainComponent> (this),
                 indice, tentativa]
                {
                    if (auto* self = safe.getComponent())
                        if (! self->componentShutdownStarted.load (
                                std::memory_order_acquire)
                             && self->cenaPendenteNoStartup == indice)
                            self->reconciliarCenaAtivaNoStartup (tentativa + 1);
                });
            return;
        }

        cenaPendenteNoStartup = -1;
        liveScenePanel.atualizar (cenas, cenaAtiva);
        liveScenePanel.avisar (ChurchDAWLive::avisoDoResultado (
            ChurchDAWLive::Resultado::indisponivel, indice));
        juce::Logger::writeToLog (
            "LIVE: cena ativa do startup nao reconciliada; motor/catalogo indisponivel");
        return;
    }

    // Limpa antes de chamar aplicarCena: um toque manual que preemptar qualquer
    // retry nao pode ser vencido depois pela intencao antiga do startup.
    cenaPendenteNoStartup = -1;
    const auto resultado = aplicarCena (indice);

    // Uma carga pode comecar entre a consulta acima e a aplicacao legada. Nesse
    // caso conserva a intencao sem acender a UI. O novo aplicarCena incrementa
    // a geracao e preempta a cadeia interna anterior antes de tentar de novo.
    if (plano.aplicarCenaAntesDeAnexarPagina
         && resultado == ChurchDAWLive::Resultado::carregando
         && tentativa < maximoDeTentativas)
    {
        cenaPendenteNoStartup = indice;
        juce::Timer::callAfterDelay (
            250,
            [safe = juce::Component::SafePointer<MainComponent> (this),
             indice, tentativa]
            {
                if (auto* self = safe.getComponent())
                    if (! self->componentShutdownStarted.load (
                            std::memory_order_acquire)
                         && self->cenaPendenteNoStartup == indice)
                        self->reconciliarCenaAtivaNoStartup (tentativa + 1);
            });
        return;
    }

    if (resultado == ChurchDAWLive::Resultado::ok
         && plano.anexarPaginaDepoisDeAplicar
         && ! assegurarPaginaDaCenaAtiva (*live))
    {
        // aplicarCena persistiu a intencao ativa antes da captura. Conserve-a
        // no disco para o proximo startup tentar novamente, mas nao afirme na
        // UI desta sessao que a pagina incompleta foi reconciliada.
        cenaAtiva = -1;
        liveScenePanel.atualizar (cenas, cenaAtiva);
        liveScenePanel.avisar (ChurchDAWLive::avisoDoResultado (
            ChurchDAWLive::Resultado::naoGravou, indice));
        return;
    }

    if (resultado == ChurchDAWLive::Resultado::ok
         || resultado == ChurchDAWLive::Resultado::vazia)
    {
        liveScenePanel.atualizar (cenas, cenaAtiva);
    }
    else if (resultado != ChurchDAWLive::Resultado::carregando
              || plano.aplicarCenaAntesDeAnexarPagina)
    {
        // aplicarCena so marca cenaAtiva depois de carregar/verificar a pagina e
        // persistir o indice. Portanto SHA ausente/corrompido permanece apagado.
        liveScenePanel.atualizar (cenas, cenaAtiva);
        liveScenePanel.avisar (ChurchDAWLive::avisoDoResultado (resultado, indice));
    }
}

//==============================================================================
// Mapa de NOTAS (tecla do controlador vira comando, nao nota tocada)
//==============================================================================
// Diz ao painel do Pad quais teclas fisicas comandam cada tom.
//
// Roda junto com a reconstrucao da mascara porque tem exatamente a mesma
// causa: o mapa de notas mudou. Assim nao ha como a tela ficar contando uma
// historia diferente do motor.
void MainComponent::atualizarEtiquetasDoPad()
{
    if (padPanel == nullptr)
        return;

    std::array<PadPanel::TeclaMapeada, 12> porTom {};
    PadPanel::TeclaMapeada paraParar;

    {
        const juce::ScopedLock sl (mapLock);

        for (const auto& par : noteMappings)
        {
            // Duas teclas no mesmo tom acontecem (dois teclados, dois canais).
            // Mostrar so a primeira mentiria menos que mostrar as duas
            // espremidas num rotulo de 8 px — entao a segunda vira "+".
            const auto etiqueta = juce::MidiMessage::getMidiNoteName (par.first.cc, true, true, 3);

            // O sourceId e "midi:" + identificador da porta; entradasMidiConhecidas
            // guarda os identificadores que estao ligados AGORA.
            const bool ligado = entradasMidiConhecidas.contains (
                par.first.sourceId.fromFirstOccurrenceOf ("midi:", false, false));

            // A TECLA MOSTRADA TEM DE SER UMA QUE FUNCIONA.
            //
            // Antes, o primeiro mapeamento que chegasse definia o rotulo e os
            // outros so viravam "+", enquanto "presente" era o OU de todos. A
            // ordem do std::map e por identificador de porta, que nao tem
            // relacao com qual teclado esta plugado — entao, no cenario que
            // motivou o recurso (mapeou em casa, no domingo levou outro), a
            // etiqueta acendia em VERMELHO mostrando a tecla de casa, que esta
            // morta, e escondia atras do "+" a unica que funciona.
            //
            // Regra: uma tecla de aparelho ligado sempre toma o rotulo.
            auto acrescentar = [&etiqueta, ligado] (PadPanel::TeclaMapeada& alvo)
            {
                const bool jaTinha = alvo.rotulo.isNotEmpty();
                const bool promove = ! jaTinha || (ligado && ! alvo.aparelhoPresente);

                if (promove)
                {
                    alvo.rotulo = etiqueta + (jaTinha ? "+" : "");
                    alvo.aparelhoPresente = ligado;
                }
                else if (! alvo.rotulo.endsWith ("+"))
                {
                    alvo.rotulo += "+";
                }
            };

            if (par.second.kind == NoteActionKind::padKey
                 && juce::isPositiveAndBelow (par.second.arg, 12))
                acrescentar (porTom[(size_t) par.second.arg]);
            else if (par.second.kind == NoteActionKind::padOff)
                acrescentar (paraParar);
        }
    }

    padPanel->setTeclasMapeadas (porTom, paraParar);
}

// Diz ao painel do Pad quais tons tem um pad de borracha / pedal de CC, e se o
// musico ja marcou que aquele pad tem trava. So isso: o painel nao precisa
// saber de nivel, de alcance aprendido nem de tipo de aparelho.
void MainComponent::atualizarBotoesDeCCDoPad()
{
    if (padPanel == nullptr)
        return;

    std::array<PadPanel::BotaoDeCC, 12> porTom {};
    PadPanel::BotaoDeCC paraParar;

    {
        const juce::ScopedLock sl (mapLock);

        for (const auto& par : ccActionMappings)
        {
            const bool comTrava =
                par.second.modo == ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel;

            if (par.second.acao.kind == NoteActionKind::padKey
                 && juce::isPositiveAndBelow (par.second.acao.arg, 12))
            {
                auto& alvo = porTom[(size_t) par.second.acao.arg];
                alvo.existe = true;
                alvo.comTrava = alvo.comTrava || comTrava;
            }
            else if (par.second.acao.kind == NoteActionKind::padOff)
            {
                paraParar.existe = true;
                paraParar.comTrava = paraParar.comTrava || comTrava;
            }
        }
    }

    padPanel->setBotoesDeCC (porTom, paraParar);
}

void MainComponent::reconstruirMascaraDeNotas()
{
    // O SNAPSHOT E OS STORE PRECISAM ESTAR SOB O MESMO LOCK.
    //
    // Com o store fora, existia esta corrida — e ela apagava um mapeamento
    // sem deixar rastro:
    //
    //   1. message thread monta o snapshot (sem a tecla nova) e SOLTA o lock;
    //   2. neste instante a thread de MIDI aprende uma tecla: grava no mapa,
    //      solta o lock e liga o bit dela;
    //   3. a message thread retoma e escreve o snapshot velho por cima —
    //      apagando o bit recem-ligado.
    //
    // O mapa no disco e a etiqueta no teclado do Pad ficavam dizendo que a
    // tecla funciona; no culto ela tocava nota. Com as duas coisas dentro do
    // lock, snapshot anterior ao insert implica store anterior ao insert, e a
    // janela deixa de existir.
    const juce::ScopedLock sl (mapLock);
    reconstruirMascaraDeNotasUnderMapLock();
}

void MainComponent::reconstruirMascaraDeNotasUnderMapLock()
{
    juce::uint32 nova[4] = { 0, 0, 0, 0 };

    for (const auto& par : noteMappings)
        if (juce::isPositiveAndBelow (par.first.cc, 128)
             && par.second.kind != NoteActionKind::none)
            nova[par.first.cc >> 5] |= (1u << (par.first.cc & 31));

    for (int i = 0; i < 4; ++i)
        mascaraDeNotasMapeadas[i].store (nova[i], std::memory_order_relaxed);
}

juce::File MainComponent::arquivoDoMapaDeNotas()
{
    return appProperties.getUserSettings()->getFile().getSiblingFile ("midi-note-map.xml");
}

bool MainComponent::exigirQuartetoMidiInspecionavel (
    const juce::String& contexto)
{
    if (ChurchDAWMidiMapping::midiMapPairTargetsInspectable (
            arquivoDoMapaDeCCs(), arquivoDoMapaDeNotas()))
        return true;

    bloquearQuartetoMidiDesconhecido (contexto);
    return false;
}

void MainComponent::bloquearQuartetoMidiDesconhecido (
    const juce::String& contexto)
{
    // "Nao consegui ler" is not evidence of corruption. Protect the complete
    // generation so an older build cannot replace an oversized/newer file (or
    // an inaccessible/non-regular path) with an older last-good snapshot.
    esquemaDeCCsDesconhecido = true;
    esquemaDeNotasDesconhecido = true;
    midiMapPairRecoveryFailed = true;
    midiMapPersistenceError.store (true, std::memory_order_release);
    juce::Logger::writeToLog (
        "MIDI MAP: quarteto nao inspecionavel em " + contexto
        + "; leitura, reparo e gravacao do par suspensos");
}

void MainComponent::finishMidiMapDirtyEpochsAfterPairCommit (
    juce::uint64 ccEpoch, bool ccWasWritten,
    juce::uint64 noteEpoch, bool noteWasWritten)
{
    // Empty autosave becomes safe only after the complete CC + NOTE generation
    // is committed. A successful half-save must never grant this permission:
    // its peer or the journal commit can still fail and roll the files back.
    ChurchDAWMidiMapping::applyMidiMapPairCommitToEmptySaveProtection (
        true, ccWasWritten, noteWasWritten,
        bloquearSalvamentoCCVazio, bloquearSalvamentoNotasVazio);

    if (ccWasWritten)
    {
        if (ccDirtyEpoch.finish (ccEpoch, true))
            ccSaveRescheduleCount.store (0);
        else if (ccSaveRescheduleCount.fetch_add (1) < 8)
            triggerAsyncUpdate();
    }

    if (noteWasWritten)
    {
        if (noteDirtyEpoch.finish (noteEpoch, true))
            noteSaveRescheduleCount.store (0);
        else if (noteSaveRescheduleCount.fetch_add (1) < 8)
            triggerAsyncUpdate();
    }
}

// Publishes CC + note maps as one durable generation. This is used by the
// global Save button and export: neither path may leave a newer CC file paired
// with an older note file if the second write or commit marker fails.
bool MainComponent::saveMidiMapPair()
{
    if (midiMapFileLock == nullptr
         || ! midiMapLifecycle.canSave()
         || midiMapPairRecoveryFailed)
        return false;

    if (! exigirQuartetoMidiInspecionavel ("inicio do save do par"))
        return false;

    const auto ccFile = arquivoDoMapaDeCCs();
    const auto noteFile = arquivoDoMapaDeNotas();
    ChurchDAWMidiMapping::MidiMapPairRollback transaction (
        ccFile, noteFile, *midiMapFileLock, &midiMapDiskRevision);

    bool saved = false;
    bool transactionClosed = false;
    bool ccWritten = false;
    bool noteWritten = false;
    juce::uint64 ccEpoch = 0;
    juce::uint64 noteEpoch = 0;
    beginMidiMapRoutingTransaction();

    const bool checkpointReady = transaction.capture ("save-midi-map-pair");
    if (checkpointReady)
    {
        if (! transaction.targetsUnchangedSinceCapture())
        {
            // A non-cooperating external writer changed a target while our
            // lock was held. We touched nothing, so preserve it and close only
            // our checkpoint without teaching this stale instance the newer
            // disk revision.
            transactionClosed = transaction.closeWithoutMutation();
        }
        else
        {
            ccEpoch = ccDirtyEpoch.snapshot();
            noteEpoch = noteDirtyEpoch.snapshot();
            ccWritten = saveMidiMap();
            noteWritten = ccWritten && saveNoteMap();
            if (ccWritten && noteWritten)
            {
                transactionClosed = transaction.commit();
                saved = transactionClosed;
            }

            // commit(false) intentionally still owns the lock. Restore while
            // it is held, before any second process can publish another journal.
            if (! saved)
                transactionClosed = transaction.restore();
        }
    }

    if (saved)
        finishMidiMapDirtyEpochsAfterPairCommit (
            ccEpoch, ccWritten, noteEpoch, noteWritten);
    finishMidiMapRoutingTransaction();

    if (! checkpointReady && transaction.hadRevisionConflict())
        midiMapPairRecoveryFailed = true;
    if (! checkpointReady && transaction.hadFutureSchemaConflict())
    {
        esquemaDeCCsDesconhecido = true;
        esquemaDeNotasDesconhecido = true;
        midiMapPairRecoveryFailed = true;
    }
    if (! checkpointReady && transaction.hadUninspectableTargetConflict())
        bloquearQuartetoMidiDesconhecido (
            "checkpoint do save do par");
    if (transaction.preservedExternalChanges())
        midiMapPairRecoveryFailed = true;
    if (checkpointReady && ! transactionClosed)
        midiMapPairRecoveryFailed = true;

    if (! saved)
        juce::Logger::writeToLog (
            transactionClosed
                ? "MIDI MAP: save do par falhou; geracao anterior restaurada"
                : "MIDI MAP: save do par nao fechou com seguranca");

    midiMapPersistenceError.store (! saved, std::memory_order_release);

    return saved;
}

// Devolve TRUE so quando o arquivo novo foi realmente gravado.
//
// Era void, e a migracao decidia apagar a chave antiga perguntando
// "o arquivo existe?" — pergunta errada: o arquivo CORROMPIDO que motivou a
// migracao tambem existe. Bastava a gravacao falhar (arquivo ainda travado por
// quem o corrompeu, permissao, disco cheio) para o app apagar a unica copia
// boa e o cliente perder todas as teclas mapeadas.
bool MainComponent::saveNoteMap (bool autorizarVazioExplicito,
                                 bool permitirConflitosDeReparo)
{
    if (! midiMapLifecycle.canSave())
        return false;

    if (midiMapPairRecoveryFailed)
    {
        juce::Logger::writeToLog (
            "MIDI MAP: notas nao gravadas porque a recuperacao do par esta bloqueada");
        return false;
    }

    if (midiMapFileLock == nullptr)
        return false;
    ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess fileAccess (
        *midiMapFileLock);
    if (! fileAccess.isLocked())
    {
        juce::Logger::writeToLog (
            "MIDI MAP: notas nao gravadas porque outra instancia esta escrevendo");
        return false;
    }

    const auto ccFile = arquivoDoMapaDeCCs();
    const auto destino = arquivoDoMapaDeNotas();
    if (! exigirQuartetoMidiInspecionavel ("save de notas"))
        return false;
    const auto activeTransactionId =
        midiMapFileLock->getActiveTransactionId();
    if (activeTransactionId.isEmpty())
    {
        // No half-map publication is allowed. The recursive pair lock remains
        // held by fileAccess until saveMidiMapPair returns.
        return saveMidiMapPair();
    }
    const bool deferDirtyEpochCompletion =
        activeTransactionId.isNotEmpty();
    if (! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
            ccFile, activeTransactionId))
    {
        midiMapPairRecoveryFailed = true;
        juce::Logger::writeToLog (
            "MIDI MAP: notas nao gravadas; outra instancia alterou o par ou deixou transacao pendente");
        return false;
    }

    {
        const juce::ScopedLock sl (mapLock);
        const bool noteConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                ccMappings, noteMappings);
        const bool ccConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                ccMappings, ccActionMappings,
                [] (const AcaoDeCC& action)
                {
                    return action.acao.kind != NoteActionKind::none;
                });
        if ((noteConflict || ccConflict) && ! permitirConflitosDeReparo)
        {
            juce::Logger::writeToLog (
                "MIDI MAP: notas nao gravadas enquanto existe conflito corrigivel; use Limpar ou Importar");
            return false;
        }
    }

    const auto backup = destino.getSiblingFile (
        destino.getFileName() + ".last-good");

    // Arquivo de uma versao futura: nao encostar. Devolve false para que a
    // migracao (e qualquer outro chamador) saiba que nada foi gravado.
    // A consulta ao disco cobre outra instancia que gravou o schema novo
    // depois que esta instancia terminou o restore de startup.
    if (esquemaDeNotasDesconhecido || esquemaDeCCsDesconhecido
         || ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
                ccFile, destino))
    {
        esquemaDeCCsDesconhecido = true;
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog ("MIDI MAP: gravacao ignorada, o arquivo em disco "
                                  "e de uma versao mais nova");
        return false;
    }

    const auto capturedEpoch = noteDirtyEpoch.snapshot();

    juce::XmlElement xml ("NOTEMAP");

    // Versao de esquema: o mapa das cenas ja tem, este nao tinha. Sem ela nao
    // ha como migrar um formato futuro sem adivinhacao.
    xml.setAttribute ("schema", 1);
    xml.setAttribute ("complete", 1);
    if (activeTransactionId.isNotEmpty())
        xml.setAttribute ("transactionId", activeTransactionId);

    const auto currentNameFor = [this] (const MidiControlKey& key)
    {
        for (const auto& source : midiSourcesCached)
            if (ChurchDAWMidiMapping::normalizeMidiSourceId (source.identifier)
                    == ChurchDAWMidiMapping::normalizeMidiSourceId (key.sourceId))
                return source.name;
        return juce::String();
    };
    {
        const juce::ScopedLock sl (mapLock);

        const bool vazio = noteMappings.empty();
        if (! ChurchDAWMidiMapping::mayWriteMidiMapSnapshot (
                bloquearSalvamentoNotasVazio, vazio,
                autorizarVazioExplicito))
        {
            juce::Logger::writeToLog (
                "MIDI MAP: autosave vazio de notas bloqueado após falha de leitura");
            return false;
        }

        xml.setAttribute ("entryCount", (int) noteMappings.size());
        xml.setAttribute ("explicitEmpty", vazio ? 1 : 0);

        for (const auto& par : noteMappings)
            ChurchDAWMidiMapping::appendPersistedNoteMapping (
                xml, par.first, (int) par.second.kind, par.second.arg,
                ChurchDAWMidiMapping::sourceNameForPersistence (
                    par.first.sourceName, currentNameFor (par.first)));
    }

    const auto serialized = xml.toString (
        juce::XmlElement::TextFormat().singleLine());
    if (! ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
            serialized))
    {
        juce::Logger::writeToLog (
            "MIDI MAP: notas nao gravadas; snapshot final excede os limites seguros");
        return false;
    }

    // ARQUIVO PROPRIO, pelo mesmo motivo das cenas.
    //
    // Isto era gravado dentro do settings — o arquivo que carrega o estado
    // binario dos plugins em base64 e chega a varios MB com um Kontakt
    // aberto — e a cada tecla aprendida. Era exatamente o arquivo do qual as
    // cenas fugiram, escrito sincronamente na thread da interface. No ensaio
    // dava um engasgo; junto com o botao Salvar durante o culto, dava a tela
    // travando.
    destino.getParentDirectory().createDirectory();

    // Building the XML can take long enough for another process to publish a
    // newer schema. Revalidate immediately before the first target mutation.
    if (! exigirQuartetoMidiInspecionavel (
            "save de notas antes da primeira escrita"))
        return false;
    if (ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
            ccFile, destino))
    {
        esquemaDeCCsDesconhecido = true;
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog (
            "MIDI MAP: schema futuro de notas apareceu durante a gravacao; snapshot preservado");
        return false;
    }
    if (backup.existsAsFile())
    {
        const auto backupXml =
            ChurchDAWMidiMapping::loadBoundedXmlFile (backup);
        if (backupXml == nullptr
             || ! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    *backupXml, "NOTEMAP", "NOTE"))
            guardarCopiaDeArquivoIlegivel (
                backup, "MIDI MAP (last-good de notas invalido)");
    }
    if (destino.existsAsFile())
    {
        if (ChurchDAWMidiMapping::promoteValidMidiMapToLastGood (
                destino, backup,
                "NOTEMAP", "NOTE"))
        {
            // valid snapshot promoted
        }
        else
            guardarCopiaDeArquivoIlegivel (destino, "MIDI MAP (notas principal inválido)");
    }

    const bool gravou =
        ChurchDAWMidiMapping::replaceFileWithTextAtomically (
            destino, serialized);
    bool backupAtualizado = false;

    if (! gravou)
        juce::Logger::writeToLog ("MIDI MAP: FALHA ao gravar " + destino.getFullPathName());
    else
    {
        backupAtualizado = ChurchDAWMidiMapping::promoteValidMidiMapToLastGood (
            destino, backup, "NOTEMAP", "NOTE");
        juce::MemoryBlock primaryBytes;
        juce::MemoryBlock backupBytes;
        juce::MemoryBlock expectedBytes;
        expectedBytes.append (serialized.toRawUTF8(),
                              (size_t) serialized.getNumBytesAsUTF8());
        backupAtualizado = backupAtualizado
            && ChurchDAWMidiMapping::loadBoundedFileData (
                   destino, primaryBytes,
                   ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && ChurchDAWMidiMapping::loadBoundedFileData (
                   backup, backupBytes,
                   ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && primaryBytes == expectedBytes
            && backupBytes == expectedBytes;
        if (! backupAtualizado)
            juce::Logger::writeToLog (
                "MIDI MAP: notas salvas, mas last-good nao foi atualizado");

    }

    // The loaded CAS is advanced only by the pair transaction's commit after
    // both halves and both last-good files have been verified.
    const bool revisionUpdated = true;

    if (gravou && ! deferDirtyEpochCompletion)
    {
        if (! noteDirtyEpoch.finish (
                capturedEpoch, backupAtualizado && revisionUpdated))
        {
            if (noteSaveRescheduleCount.fetch_add (1) < 8)
                triggerAsyncUpdate();
            else
                juce::Logger::writeToLog (
                    "MIDI MAP: dirty epoch de notas mudou durante escrita; retry limitado");
        }
        else
            noteSaveRescheduleCount.store (0);
    }

    return gravou && backupAtualizado && revisionUpdated;
}

void MainComponent::restoreNoteMap()
{
    if (midiMapFileLock == nullptr)
        return;
    ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess fileAccess (
        *midiMapFileLock, 5000);
    if (! fileAccess.isLocked())
    {
        midiMapPairRecoveryFailed = true;
        return;
    }

    if (! exigirQuartetoMidiInspecionavel ("restore de notas"))
        return;

    const auto arquivo = arquivoDoMapaDeNotas();
    juce::MemoryBlock primaryBytes;
    const auto primaryInspection =
        ChurchDAWMidiMapping::inspectMidiMapTarget (
            arquivo, primaryBytes);
    if (primaryInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::unknown)
    {
        bloquearQuartetoMidiDesconhecido (
            "leitura exata do mapa de notas");
        return;
    }
    auto xml = primaryInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::readable
        ? ChurchDAWMidiMapping::parseBoundedXmlData (primaryBytes)
        : nullptr;
    bool veioDoFormatoAntigo = false;

    if (xml != nullptr
         && ChurchDAWMidiMapping::hasFutureMidiMapSchema (*xml, "NOTEMAP"))
    {
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog ("MIDI MAP: notas de schema futuro protegidas");
        return;
    }

    const auto lastGood = arquivo.getSiblingFile (
        arquivo.getFileName() + ".last-good");
    juce::MemoryBlock backupBytes;
    const auto backupInspection =
        ChurchDAWMidiMapping::inspectMidiMapTarget (
            lastGood, backupBytes);
    if (backupInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::unknown)
    {
        bloquearQuartetoMidiDesconhecido (
            "leitura exata do last-good de notas");
        return;
    }
    if (auto backupXml = backupInspection
              == ChurchDAWMidiMapping::MidiMapTargetInspection::readable
            ? ChurchDAWMidiMapping::parseBoundedXmlData (backupBytes)
            : nullptr;
        backupXml != nullptr
         && ChurchDAWMidiMapping::hasFutureMidiMapSchema (
                *backupXml, "NOTEMAP"))
    {
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog (
            "MIDI MAP: last-good de notas pertence a schema futuro; arquivos protegidos");
        return;
    }

    const bool primaryNoteReadable = xml != nullptr
        && ChurchDAWMidiMapping::isMidiMapXmlReadable (
            *xml, "NOTEMAP", "NOTE");
    if (! primaryNoteReadable)
    {
        if (arquivo.existsAsFile())
            guardarCopiaDeArquivoIlegivel (arquivo, "MIDI MAP (notas)");
        auto fallback = ChurchDAWMidiMapping::recoverMidiMapXml (
            arquivo, lastGood, "NOTEMAP", "NOTE");
        if (fallback != nullptr)
        {
            xml = std::move (fallback);
            bloquearSalvamentoNotasVazio = false;
            juce::Logger::writeToLog (
                "MIDI MAP: notas restauradas do last-good; principal reparado quando permitido");
        }
        else
        {
            if (lastGood.existsAsFile())
                guardarCopiaDeArquivoIlegivel (
                    lastGood, "MIDI MAP (last-good de notas invalido)");
            // Nunca continue usando o ponteiro para o XML principal que acabou
            // de ser rejeitado; os loops abaixo carregariam apenas os filhos
            // conhecidos e o proximo save destruiria o restante.
            xml.reset();
            bloquearSalvamentoNotasVazio =
                ChurchDAWMidiMapping::shouldBlockEmptySaveAfterFailedRecovery (
                    arquivo.existsAsFile(), lastGood.existsAsFile());
        }
    }

    // MIGRACAO do formato antigo (chave dentro do settings). Quem ja mapeou
    // teclas nao pode perde-las ao atualizar.
    if (xml == nullptr || ! xml->hasTagName ("NOTEMAP"))
    {
        guardarCopiaDeArquivoIlegivel (arquivo, "MIDI MAP (notas)");

        xml = appProperties.getUserSettings()->getXmlValue ("midiNoteMap");
        if (xml != nullptr
             && ! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    *xml, "NOTEMAP", "NOTE"))
        {
            if (ChurchDAWMidiMapping::hasFutureMidiMapSchema (
                    *xml, "NOTEMAP"))
                esquemaDeNotasDesconhecido = true;
            else
                bloquearSalvamentoNotasVazio = true;
            juce::Logger::writeToLog (
                "MIDI MAP: chave antiga de notas invalida/desconhecida; mantida intacta");
            return;
        }

        veioDoFormatoAntigo = xml != nullptr;
        if (veioDoFormatoAntigo)
            bloquearSalvamentoNotasVazio = false;
    }

    if (xml == nullptr || ! xml->hasTagName ("NOTEMAP"))
        return;

    // ESQUEMA DO FUTURO NAO SE LE COM AS REGRAS DE HOJE.
    //
    // O atributo existia so para ser escrito. Se um build mais novo gravar
    // schema=2 e o cliente reabrir um build antigo (downgrade, ou duas
    // maquinas), ler com as regras da versao 1 e interpretar errado — e o
    // primeiro salvamento reescreveria o arquivo no formato velho, destruindo
    // o que a versao nova gravou. Melhor nao tocar em nada.
    if (xml->getIntAttribute ("schema", 1) > 1)
    {
        // TRAVA TAMBEM A GRAVACAO, nao so a leitura.
        //
        // Sozinha, a guarda era pior que nao ter guarda: ela deixava
        // noteMappings VAZIO e nada impedia o proximo salvamento — e salvar
        // acontece por rotina (botao Salvar, aprender uma tecla, Limpar mapas).
        // O arquivo do futuro virava um NOTEMAP vazio no formato velho, e sem
        // nem o .corrompido de consolo, porque o XML era valido e nunca passou
        // pelo ramo de backup.
        esquemaDeNotasDesconhecido = true;

        juce::Logger::writeToLog ("MIDI MAP: arquivo de notas e de uma versao mais nova "
                                  "(schema " + xml->getStringAttribute ("schema")
                                  + "); leitura e gravacao suspensas");
        return;
    }

    esquemaDeNotasDesconhecido = false;

    {
        const juce::ScopedLock sl (mapLock);
        noteMappings.clear();

        for (auto* n : xml->getChildWithTagNameIterator ("NOTE"))
        {
            ChurchDAWMidiMapping::PersistedNoteMapping persisted;
            if (! ChurchDAWMidiMapping::readPersistedNoteMapping (*n, persisted))
                continue;

            const int nota  = persisted.control.cc;
            const int canal = persisted.control.channel;
            const int tipo  = persisted.action;
            const int arg   = persisted.argument;
            const auto fonte = persisted.control.sourceId;

            // Validacao estrita: um XML mexido a mao nao pode fazer o app
            // sequestrar uma nota qualquer do teclado.
            if (! juce::isPositiveAndBelow (nota, 128)
                 || canal < 1 || canal > 16
                 || fonte.isEmpty())
                continue;

            NoteAction acao;

            if (tipo == (int) NoteActionKind::padKey && juce::isPositiveAndBelow (arg, 12))
                acao = { NoteActionKind::padKey, arg };
            else if (tipo == (int) NoteActionKind::padOff)
                acao = { NoteActionKind::padOff, -1 };
            else if (tipo == (int) NoteActionKind::cena
                      && juce::isPositiveAndBelow (arg, ChurchDAWLive::numeroDeCenas))
                acao = { NoteActionKind::cena, arg };
            else if (tipo == (int) NoteActionKind::panic)
                acao = { NoteActionKind::panic, -1 };
            else if (tipo == (int) NoteActionKind::nextScene)
                acao = { NoteActionKind::nextScene, -1 };
            else if (tipo == (int) NoteActionKind::previousScene)
                acao = { NoteActionKind::previousScene, -1 };
            else if (tipo == (int) NoteActionKind::padSynthProgram
                      && juce::isPositiveAndBelow (arg,
                                                   PadSynthProcessor::numPrograms))
                acao = { NoteActionKind::padSynthProgram, arg };
            else if (tipo == (int) NoteActionKind::tapTempo)
                acao = { NoteActionKind::tapTempo, -1 };
            else if (tipo == (int) NoteActionKind::sustainToggle)
                acao = { NoteActionKind::sustainToggle, -1 };
            else if (tipo == (int) NoteActionKind::padSynthEngineToggle)
                acao = { NoteActionKind::padSynthEngineToggle, -1 };
            else
                continue;

            noteMappings.emplace (
                ChurchDAWMidiMapping::ControllerKey {
                    fonte, canal, nota, false, persisted.control.sourceName },
                acao);
        }

        // Resolve IDs efemeros only after every raw key exists. Resolving one
        // child at a time followed by operator[] allowed two old IDs with the
        // same device name to collapse and silently discard one mapping.
        (void) ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            noteMappings, midiSourcesCached);
    }

    reconstruirMascaraDeNotas();
    atualizarEtiquetasDoPad();

    // Termina a migracao: grava no formato novo e so DEPOIS solta a chave
    // antiga. Nessa ordem, uma falha no meio deixa o mapa no lugar velho —
    // nunca em nenhum dos dois.
    if (veioDoFormatoAntigo)
    {
        pendingLegacyNoteMigration = true;
        juce::Logger::writeToLog ("MIDI MAP: migracao de notas agendada apos ambos restores");
    }
}

juce::File MainComponent::arquivoDoMapaDeCCs()
{
    return appProperties.getUserSettings()->getFile().getSiblingFile ("midi-cc-map.xml");
}

// Nome do aparelho a partir de um sourceId ("midi:<id>"). Usado para
// persistir o NOME (estavel) junto do id (efemero no Android) e reescrever
// o id no restore quando o mesmo aparelho voltar com um id novo.
juce::String MainComponent::nomeDoAparelho (const juce::String& sourceId) const
{
    if (! sourceId.startsWith ("midi:"))
        return {};

    const auto normalized = ChurchDAWMidiMapping::normalizeMidiSourceId (sourceId);
    const juce::ScopedLock sl (mapLock);
    for (const auto& source : midiSourcesCached)
        if (ChurchDAWMidiMapping::normalizeMidiSourceId (source.identifier)
                == normalized)
            return source.name;

    return {};
}

// Resolve o sourceId de um mapa salvo para o id ATUAL do aparelho. Se o
// aparelho (mesmo NOME) voltou com um id novo — o caso do Android no replug/
// reinicio — devolve o id novo; senao devolve o id salvo.
juce::String MainComponent::resolverSourceId (const juce::String& sourceId,
                                              const juce::String& sourceName) const
{
    const juce::ScopedLock sl (mapLock);
    return ChurchDAWMidiMapping::resolveUniqueMidiSourceId (
        sourceId, sourceName, midiSourcesCached);
}

bool MainComponent::saveMidiMap (bool autorizarVazioExplicito,
                                 bool permitirConflitosDeReparo)
{
    if (! midiMapLifecycle.canSave())
        return false;

    if (midiMapPairRecoveryFailed)
    {
        juce::Logger::writeToLog (
            "MIDI MAP: CCs nao gravados porque a recuperacao do par esta bloqueada");
        return false;
    }

    if (midiMapFileLock == nullptr)
        return false;
    ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess fileAccess (
        *midiMapFileLock);
    if (! fileAccess.isLocked())
    {
        juce::Logger::writeToLog (
            "MIDI MAP: CCs nao gravados porque outra instancia esta escrevendo");
        return false;
    }

    const auto destino = arquivoDoMapaDeCCs();
    const auto noteFile = arquivoDoMapaDeNotas();
    if (! exigirQuartetoMidiInspecionavel ("save de CCs"))
        return false;
    const auto activeTransactionId =
        midiMapFileLock->getActiveTransactionId();
    if (activeTransactionId.isEmpty())
    {
        // Standalone CC saves are promoted to the complete pair transaction.
        return saveMidiMapPair();
    }
    const bool deferDirtyEpochCompletion =
        activeTransactionId.isNotEmpty();
    if (! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
            destino, activeTransactionId))
    {
        midiMapPairRecoveryFailed = true;
        juce::Logger::writeToLog (
            "MIDI MAP: CCs nao gravados; outra instancia alterou o par ou deixou transacao pendente");
        return false;
    }

    {
        const juce::ScopedLock sl (mapLock);
        const bool noteConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                ccMappings, noteMappings);
        const bool ccConflict =
            ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                ccMappings, ccActionMappings,
                [] (const AcaoDeCC& action)
                {
                    return action.acao.kind != NoteActionKind::none;
                });
        if ((noteConflict || ccConflict) && ! permitirConflitosDeReparo)
        {
            juce::Logger::writeToLog (
                "MIDI MAP: CCs nao gravados enquanto existe conflito corrigivel; use Limpar ou Importar");
            return false;
        }
    }

    const auto backup = destino.getSiblingFile (
        destino.getFileName() + ".last-good");

    if (esquemaDeCCsDesconhecido || esquemaDeNotasDesconhecido
         || ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
                destino, noteFile))
    {
        esquemaDeCCsDesconhecido = true;
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog ("MIDI MAP: gravacao de CCs ignorada, o arquivo em disco "
                                  "e de uma versao mais nova");
        return false;
    }

    const auto capturedEpoch = ccDirtyEpoch.snapshot();

    juce::XmlElement xml ("MIDIMAP");
    xml.setAttribute ("schema", 1);
    xml.setAttribute ("complete", 1);
    if (activeTransactionId.isNotEmpty())
        xml.setAttribute ("transactionId", activeTransactionId);

    const auto currentNameFor = [this] (const MidiControlKey& key)
    {
        for (const auto& source : midiSourcesCached)
            if (ChurchDAWMidiMapping::normalizeMidiSourceId (source.identifier)
                    == ChurchDAWMidiMapping::normalizeMidiSourceId (key.sourceId))
                return source.name;
        return juce::String();
    };

    {
        const juce::ScopedLock sl (mapLock);

        int entryCount = (int) ccMappings.size();
        for (const auto& action : ccActionMappings)
            if (action.second.acao.kind != NoteActionKind::none)
                ++entryCount;
        const bool vazio = entryCount == 0;
        if (! ChurchDAWMidiMapping::mayWriteMidiMapSnapshot (
                bloquearSalvamentoCCVazio, vazio,
                autorizarVazioExplicito))
        {
            juce::Logger::writeToLog (
                "MIDI MAP: autosave vazio de CCs bloqueado após falha de leitura");
            return false;
        }

        xml.setAttribute ("entryCount", entryCount);
        xml.setAttribute ("explicitEmpty", vazio ? 1 : 0);

        for (const auto& pair : ccMappings)
            ChurchDAWMidiMapping::appendPersistedCCMapping (
                xml, pair.first, pair.second,
                ChurchDAWMidiMapping::sourceNameForPersistence (
                    pair.first.sourceName, currentNameFor (pair.first)));

        // BOTOES DO PAD POR CC — tag IRMA, e o schema CONTINUA 1.
        //
        // Nao virou valor novo no enum de acoes nem coluna nova em <MAP>: um
        // build anterior itera so por <MAP> (o proprio comentario do restore diz
        // que essa trava existe "para o dia em que houver outro filho"), entao
        // ele simplesmente ignora <CCACAO> e continua carregando os CCs do
        // cliente. E o schema fica em 1 de proposito: com schema 2 um build
        // antigo SUSPENDE leitura E gravacao, e o cliente ficaria sem NENHUM
        // mapeamento. Preco assumido: voltar uma versao e salvar perde os
        // botoes do Pad — nunca o resto do mapa.
        for (const auto& pair : ccActionMappings)
        {
            if (pair.second.acao.kind == NoteActionKind::none)
                continue;

            auto* a = xml.createNewChildElement ("CCACAO");
            a->setAttribute ("source", pair.first.sourceId);
            a->setAttribute ("channel", pair.first.channel);
            a->setAttribute ("cc", pair.first.cc);
            a->setAttribute ("action", (int) pair.second.acao.kind);
            a->setAttribute ("arg", pair.second.acao.arg);
            a->setAttribute ("modo", (int) pair.second.modo);
            a->setAttribute ("sourceName",
                             ChurchDAWMidiMapping::sourceNameForPersistence (
                                 pair.first.sourceName, currentNameFor (pair.first)));
        }
    }

    const auto serialized = xml.toString (
        juce::XmlElement::TextFormat().singleLine());
    if (! ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
            serialized))
    {
        juce::Logger::writeToLog (
            "MIDI MAP: CCs nao gravados; snapshot final excede os limites seguros");
        return false;
    }

    // ARQUIVO PROPRIO, o ultimo dos tres a sair do settings.
    //
    // Era gravado dentro do arquivo que carrega o estado binario dos plugins em
    // base64 — varios MB com um Kontakt aberto — e a cada CC aprendido. As
    // cenas sairam de la primeiro, depois o mapa de notas; este ficou para
    // tras pelo mesmo motivo de sempre: ninguem tinha olhado.
    destino.getParentDirectory().createDirectory();

    if (! exigirQuartetoMidiInspecionavel (
            "save de CCs antes da primeira escrita"))
        return false;
    if (ChurchDAWMidiMapping::midiMapPairHasFutureSchema (
            destino, noteFile))
    {
        esquemaDeCCsDesconhecido = true;
        esquemaDeNotasDesconhecido = true;
        juce::Logger::writeToLog (
            "MIDI MAP: schema futuro de CCs apareceu durante a gravacao; snapshot preservado");
        return false;
    }
    if (backup.existsAsFile())
    {
        const auto backupXml =
            ChurchDAWMidiMapping::loadBoundedXmlFile (backup);
        if (backupXml == nullptr
             || ! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    *backupXml, "MIDIMAP", "MAP", "CCACAO"))
            guardarCopiaDeArquivoIlegivel (
                backup, "MIDI MAP (last-good de CCs invalido)");
    }
    if (destino.existsAsFile())
    {
        if (ChurchDAWMidiMapping::promoteValidMidiMapToLastGood (
                destino, backup,
                "MIDIMAP", "MAP", "CCACAO"))
        {
            // valid snapshot promoted
        }
        else
            guardarCopiaDeArquivoIlegivel (destino, "MIDI MAP (CC principal inválido)");
    }

    const bool gravou =
        ChurchDAWMidiMapping::replaceFileWithTextAtomically (
            destino, serialized);
    bool backupAtualizado = false;

    if (! gravou)
        juce::Logger::writeToLog ("MIDI MAP: FALHA ao gravar " + destino.getFullPathName());
    else
    {
        backupAtualizado = ChurchDAWMidiMapping::promoteValidMidiMapToLastGood (
            destino, backup, "MIDIMAP", "MAP", "CCACAO");
        juce::MemoryBlock primaryBytes;
        juce::MemoryBlock backupBytes;
        juce::MemoryBlock expectedBytes;
        expectedBytes.append (serialized.toRawUTF8(),
                              (size_t) serialized.getNumBytesAsUTF8());
        backupAtualizado = backupAtualizado
            && ChurchDAWMidiMapping::loadBoundedFileData (
                   destino, primaryBytes,
                   ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && ChurchDAWMidiMapping::loadBoundedFileData (
                   backup, backupBytes,
                   ChurchDAWMidiMapping::maxMidiMapFileBytes)
            && primaryBytes == expectedBytes
            && backupBytes == expectedBytes;
        if (! backupAtualizado)
            juce::Logger::writeToLog (
                "MIDI MAP: CCs salvos, mas last-good nao foi atualizado");

    }

    const bool revisionUpdated = true;

    if (gravou && ! deferDirtyEpochCompletion)
    {
        if (! ccDirtyEpoch.finish (
                capturedEpoch, backupAtualizado && revisionUpdated))
        {
            if (ccSaveRescheduleCount.fetch_add (1) < 8)
                triggerAsyncUpdate();
            else
                juce::Logger::writeToLog (
                    "MIDI MAP: dirty epoch de CC mudou durante escrita; retry limitado");
        }
        else
            ccSaveRescheduleCount.store (0);
    }

    return gravou && backupAtualizado && revisionUpdated;
}

void MainComponent::restoreMidiMap()
{
    if (midiMapFileLock == nullptr)
        return;
    ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess fileAccess (
        *midiMapFileLock, 5000);
    if (! fileAccess.isLocked())
    {
        midiMapPairRecoveryFailed = true;
        return;
    }

    if (! exigirQuartetoMidiInspecionavel ("restore de CCs"))
        return;

    int restored = 0;
    int rejectedUnsafe = 0;

    const auto arquivo = arquivoDoMapaDeCCs();
    juce::MemoryBlock primaryBytes;
    const auto primaryInspection =
        ChurchDAWMidiMapping::inspectMidiMapTarget (
            arquivo, primaryBytes);
    if (primaryInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::unknown)
    {
        bloquearQuartetoMidiDesconhecido (
            "leitura exata do mapa de CCs");
        return;
    }
    auto xml = primaryInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::readable
        ? ChurchDAWMidiMapping::parseBoundedXmlData (primaryBytes)
        : nullptr;
    bool veioDoFormatoAntigo = false;

    if (xml != nullptr
         && ChurchDAWMidiMapping::hasFutureMidiMapSchema (*xml, "MIDIMAP"))
    {
        esquemaDeCCsDesconhecido = true;
        juce::Logger::writeToLog ("MIDI MAP: CCs de schema futuro protegidos");
        return;
    }

    const auto lastGood = arquivo.getSiblingFile (
        arquivo.getFileName() + ".last-good");
    juce::MemoryBlock backupBytes;
    const auto backupInspection =
        ChurchDAWMidiMapping::inspectMidiMapTarget (
            lastGood, backupBytes);
    if (backupInspection
          == ChurchDAWMidiMapping::MidiMapTargetInspection::unknown)
    {
        bloquearQuartetoMidiDesconhecido (
            "leitura exata do last-good de CCs");
        return;
    }
    if (auto backupXml = backupInspection
              == ChurchDAWMidiMapping::MidiMapTargetInspection::readable
            ? ChurchDAWMidiMapping::parseBoundedXmlData (backupBytes)
            : nullptr;
        backupXml != nullptr
         && ChurchDAWMidiMapping::hasFutureMidiMapSchema (
                *backupXml, "MIDIMAP"))
    {
        esquemaDeCCsDesconhecido = true;
        juce::Logger::writeToLog (
            "MIDI MAP: last-good de CCs pertence a schema futuro; arquivos protegidos");
        return;
    }

    const bool primaryCCReadable = xml != nullptr
        && ChurchDAWMidiMapping::isMidiMapXmlReadable (
            *xml, "MIDIMAP", "MAP", "CCACAO");
    if (! primaryCCReadable)
    {
        if (arquivo.existsAsFile())
            guardarCopiaDeArquivoIlegivel (arquivo, "MIDI MAP (CCs)");
        auto fallback = ChurchDAWMidiMapping::recoverMidiMapXml (
            arquivo, lastGood, "MIDIMAP", "MAP", "CCACAO");
        if (fallback != nullptr)
        {
            xml = std::move (fallback);
            bloquearSalvamentoCCVazio = false;
            juce::Logger::writeToLog (
                "MIDI MAP: CCs restaurados do last-good; principal reparado quando permitido");
        }
        else
        {
            if (lastGood.existsAsFile())
                guardarCopiaDeArquivoIlegivel (
                    lastGood, "MIDI MAP (last-good de CCs invalido)");
            xml.reset();
            bloquearSalvamentoCCVazio =
                ChurchDAWMidiMapping::shouldBlockEmptySaveAfterFailedRecovery (
                    arquivo.existsAsFile(), lastGood.existsAsFile());
        }
    }

    if (xml == nullptr || ! xml->hasTagName ("MIDIMAP"))
    {
        if (arquivo.existsAsFile())
        {
            guardarCopiaDeArquivoIlegivel (arquivo, "MIDI MAP (CCs)");
        }

        // MIGRACAO da chave antiga no settings.
        //
        // Exigir a RAIZ certa aqui, e nao so "parseou": um valor adulterado que
        // ainda seja XML valido marcava veioDoFormatoAntigo, nada era lido, e o
        // saveMidiMap gravava um <MIDIMAP> VAZIO com sucesso — e a chave, unica
        // copia do mapa, era apagada. O ramo .corrompido cobre o ARQUIVO, nunca
        // a chave do settings.
        xml = appProperties.getUserSettings()->getXmlValue ("midiMap");

        if (xml != nullptr
             && ! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    *xml, "MIDIMAP", "MAP", "CCACAO"))
        {
            if (ChurchDAWMidiMapping::hasFutureMidiMapSchema (
                    *xml, "MIDIMAP"))
                esquemaDeCCsDesconhecido = true;
            else
                bloquearSalvamentoCCVazio = true;
            juce::Logger::writeToLog (
                "MIDI MAP: chave antiga de CCs invalida/desconhecida; mantida intacta");
            return;
        }

        veioDoFormatoAntigo = xml != nullptr;
        if (veioDoFormatoAntigo)
            bloquearSalvamentoCCVazio = false;
    }

    if (xml != nullptr && xml->hasTagName ("MIDIMAP")
         && xml->getIntAttribute ("schema", 1) > 1)
    {
        // Nao le E nao grava, igual aos outros dois.
        esquemaDeCCsDesconhecido = true;
        juce::Logger::writeToLog ("MIDI MAP: arquivo de CCs e de uma versao mais nova "
                                  "(schema " + xml->getStringAttribute ("schema")
                                  + "); leitura e gravacao suspensas");
        return;
    }

    esquemaDeCCsDesconhecido = false;

    if (xml != nullptr)
        if (xml->hasTagName ("MIDIMAP"))
        {
            const juce::ScopedLock sl (mapLock);
            ccMappings.clear();
            ccActionMappings.clear();

            // Iterar so por <MAP>, como o mapa de notas faz por <NOTE>. Com o
            // iterador solto, qualquer filho de outro tipo reprovava na
            // validacao, virava "legado ambiguo descartado" e — porque isso
            // dispara regravacao — era APAGADO do arquivo em definitivo. Hoje
            // nao existe outro filho; isto e a trava para o dia em que existir.
            for (auto* m : xml->getChildWithTagNameIterator ("MAP"))
            {
                ChurchDAWMidiMapping::PersistedCCMapping persisted;
                if (! ChurchDAWMidiMapping::readPersistedCCMapping (
                        *m, persisted))
                    continue;
                auto target = std::move (persisted.target);
                auto control = std::move (persisted.control);
                const int cc = control.cc;

                // Mapas muito antigos guardavam somente o indice. Depois que
                // Attack/Release/EQ/Rotary foram adicionados, esse indice pode
                // apontar para outro parametro — inclusive Transpose. Descartar
                // o mapa ambiguo e seguro; reaproveita-lo e alterar o tom ao vivo.
                // Mapa sem origem/canal tambem e descartado: torna-lo wildcard
                // faria outro teclado com o mesmo CC mudar o tom.
                if (! control.isValid()
                     || (!control.isNote && ! ChurchDAWMidiMapping::isMappableController (cc))
                     || ! ChurchDAWMidiMapping::isSafeStoredTarget (
                            target.slot, target.paramId, target.paramName))
                {
                    ++rejectedUnsafe;
                    continue;
                }

                if (ccMappings.emplace (control, std::move (target)).second)
                    ++restored;
                else
                    ++rejectedUnsafe;
            }

            // Botoes do Pad por CC. Iterador proprio, validacao propria: um XML
            // mexido a mao nao pode fazer um CC qualquer trocar o tom no culto.
            for (auto* a : xml->getChildWithTagNameIterator ("CCACAO"))
            {
                const MidiControlKey control {
                    ChurchDAWMidiMapping::normalizeMidiSourceId (
                        a->getStringAttribute ("source")),
                    a->getIntAttribute ("channel", 0),
                    a->getIntAttribute ("cc", -1),
                    false,
                    a->getStringAttribute ("sourceName")
                };

                if (! control.isValid()
                     || ! ChurchDAWMidiMapping::isMappableAction (control.cc))
                    continue;

                const int tipo = a->getIntAttribute ("action", 0);
                const int arg  = a->getIntAttribute ("arg", -1);

                AcaoDeCC entrada;

                if (tipo == (int) NoteActionKind::padKey
                     && juce::isPositiveAndBelow (arg, 12))
                    entrada.acao = { NoteActionKind::padKey, arg };
                else if (tipo == (int) NoteActionKind::padOff)
                    entrada.acao = { NoteActionKind::padOff, -1 };
                else if (tipo == (int) NoteActionKind::cena
                          && juce::isPositiveAndBelow (arg, ChurchDAWLive::numeroDeCenas))
                    entrada.acao = { NoteActionKind::cena, arg };
                else if (tipo == (int) NoteActionKind::panic)
                    entrada.acao = { NoteActionKind::panic, -1 };
                else if (tipo == (int) NoteActionKind::nextScene)
                    entrada.acao = { NoteActionKind::nextScene, -1 };
                else if (tipo == (int) NoteActionKind::previousScene)
                    entrada.acao = { NoteActionKind::previousScene, -1 };
                else if (tipo == (int) NoteActionKind::padSynthProgram
                          && juce::isPositiveAndBelow (
                              arg, PadSynthProcessor::numPrograms))
                    entrada.acao = { NoteActionKind::padSynthProgram, arg };
                else if (tipo == (int) NoteActionKind::tapTempo)
                    entrada.acao = { NoteActionKind::tapTempo, -1 };
                else if (tipo == (int) NoteActionKind::sustainToggle)
                    entrada.acao = { NoteActionKind::sustainToggle, -1 };
                else if (tipo == (int) NoteActionKind::padSynthEngineToggle)
                    entrada.acao = { NoteActionKind::padSynthEngineToggle, -1 };
                else
                    continue;

                entrada.modo =
                    a->getIntAttribute ("modo", 0)
                            == (int) ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel
                        ? ChurchDAWMidiMapping::ModoDeBotaoCC::seguirNivel
                        : ChurchDAWMidiMapping::ModoDeBotaoCC::alternar;

                // Somente a curta janela de boot silencia um dump inicial. Fora
                // dela, o primeiro toque real nao exige um segundo aperto.
                entrada.estado.armarSincronizacaoDeBoot (
                    juce::Time::getMillisecondCounter());
                if (ccActionMappings.emplace (control, entrada).second)
                    ++restored;
                else
                    ++rejectedUnsafe;
            }

            // Re-key the complete maps in one pass so collisions are kept as
            // stale recoverable entries instead of being overwritten.
            (void) ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
                ccMappings, midiSourcesCached);
            (void) ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
                ccActionMappings, midiSourcesCached);
        }

    reconstruirMascaraDeCCsDeAcao();
    atualizarBotoesDeCCDoPad();

    juce::Logger::writeToLog ("MIDI MAP: " + juce::String (restored)
                              + " mapeamento(s) restaurado(s), "
                              + juce::String (rejectedUnsafe)
                              + " legado(s) ambiguo(s) descartado(s)");

    // Termina a migracao: grava no formato novo e so DEPOIS solta a chave
    // antiga. Um alvo perigoso descartado tambem obriga a regravar, para ele
    // nao voltar na proxima inicializacao.
    if (veioDoFormatoAntigo || rejectedUnsafe > 0)
    {
        if (veioDoFormatoAntigo)
        {
            pendingLegacyCCMigration = true;
            juce::Logger::writeToLog ("MIDI MAP: migracao de CCs agendada apos ambos restores");
        }
        else if (rejectedUnsafe > 0 && midiMapsLoaded)
            (void) saveMidiMap();
    }
}

//==============================================================================
bool MainComponent::rebuildGraphConnections (
    int replacementSlot,
    juce::AudioProcessorGraph::NodeID replacementNode)
{
    const auto previousConnections = graph.getConnections();

    for (const auto& c : previousConnections)
        graph.removeConnection (c, juce::AudioProcessorGraph::UpdateKind::none);

    bool requiredConnectionsOk = true;

    const auto addAudioConnection =
        [this, &requiredConnectionsOk] (juce::AudioProcessorGraph::NodeID source,
                                        int sourceChannel,
                                        juce::AudioProcessorGraph::NodeID destination,
                                        int destinationChannel,
                                        const juce::String& label,
                                        bool required)
        {
            const bool added = graph.addConnection (
                { { source, sourceChannel }, { destination, destinationChannel } },
                juce::AudioProcessorGraph::UpdateKind::none);

            if (! added)
            {
                juce::Logger::writeToLog (
                    "AUDIO_GRAPH connection_failed edge=" + label
                    + " source_channel=" + juce::String (sourceChannel)
                    + " destination_channel=" + juce::String (destinationChannel)
                    + " required=" + juce::String (required ? 1 : 0));

                if (required)
                    requiredConnectionsOk = false;
            }

            return added;
        };

    const auto connectAudio =
        [this, &addAudioConnection, &requiredConnectionsOk]
        (juce::AudioProcessorGraph::NodeID source,
         juce::AudioProcessorGraph::NodeID destination,
         const juce::String& label,
         bool required)
        {
            juce::AudioProcessorGraph::Node::Ptr sourceNode (
                graph.getNodeForId (source));
            juce::AudioProcessorGraph::Node::Ptr destinationNode (
                graph.getNodeForId (destination));

            if (sourceNode == nullptr || destinationNode == nullptr)
            {
                juce::Logger::writeToLog (
                    "AUDIO_GRAPH node_missing edge=" + label
                    + " required=" + juce::String (required ? 1 : 0));

                if (required)
                    requiredConnectionsOk = false;

                return false;
            }

            const int sourceChannels =
                sourceNode->getProcessor()->getTotalNumOutputChannels();
            const int destinationChannels =
                destinationNode->getProcessor()->getTotalNumInputChannels();

            if (sourceChannels <= 0 || destinationChannels <= 0)
            {
                juce::Logger::writeToLog (
                    "AUDIO_GRAPH incompatible_io edge=" + label
                    + " outputs=" + juce::String (sourceChannels)
                    + " inputs=" + juce::String (destinationChannels)
                    + " required=" + juce::String (required ? 1 : 0));

                if (required)
                    requiredConnectionsOk = false;

                return false;
            }

            bool connected = true;

            if (sourceChannels == 1)
            {
                connected &= addAudioConnection (source, 0, destination, 0,
                                                 label, required);

                if (destinationChannels >= 2)
                    connected &= addAudioConnection (source, 0, destination, 1,
                                                     label, required);
            }
            else if (destinationChannels == 1)
            {
                connected &= addAudioConnection (source, 0, destination, 0,
                                                 label, required);
                connected &= addAudioConnection (source, 1, destination, 0,
                                                 label, required);
            }
            else
            {
                connected &= addAudioConnection (source, 0, destination, 0,
                                                 label, required);
                connected &= addAudioConnection (source, 1, destination, 1,
                                                 label, required);
            }

            return connected;
        };

    juce::Array<juce::AudioProcessorGraph::NodeID> fxChain;

    const auto nodeForSlot =
        [this, replacementSlot, replacementNode] (int slotIndex)
        {
            return slotIndex == replacementSlot ? replacementNode
                                                : slots[slotIndex].nodeID;
        };

    for (int i = 1; i < numSlots; ++i)
        if (const auto nodeID = nodeForSlot (i); nodeID.uid != 0)
            fxChain.add (nodeID);

    const auto timbresNode = nodeForSlot (0);
    const auto instrument = timbresNode;
    const auto midiCh = juce::AudioProcessorGraph::midiChannelIndex;

    // fontes que alimentam o comeco da cadeia de efeitos
    const auto firstDest = fxChain.isEmpty() ? masterNode : fxChain.getFirst();

    if (timbresNode.uid != 0)
        connectAudio (timbresNode, firstDest, "timbres_to_chain", true);

    if (audioInputAuthorised.load (std::memory_order_acquire))
    {
        juce::AudioProcessorGraph::Node::Ptr inputNode (
            graph.getNodeForId (audioInNode));
        const bool inputIsAvailable = inputNode != nullptr
            && inputNode->getProcessor()->getTotalNumOutputChannels() > 0;
        connectAudio (audioInNode, firstDest, "device_input_to_chain",
                      inputIsAvailable);
    }

    for (int k = 0; k + 1 < fxChain.size(); ++k)
        connectAudio (fxChain[k], fxChain[k + 1], "effect_to_effect", true);

    if (! fxChain.isEmpty())
        connectAudio (fxChain.getLast(), masterNode, "effect_to_master", true);

    // pad continuo soma direto no master (passa pelo volume/medidor do master)
    if (padNode.uid != 0)
        connectAudio (padNode, masterNode, "pad_to_master", true);

    // Sintetizador polifonico do PAD EDITOR. Nasce com o parametro de engine
    // desligado, portanto a conexao permanente nao altera setups antigos.
    if (padSynthNode.uid != 0)
        connectAudio (padSynthNode, masterNode, "pad_synth_to_master", true);

    // master sempre alimenta a saida
    juce::AudioProcessorGraph::Node::Ptr outputNode (
        graph.getNodeForId (audioOutNode));
    const bool outputIsAvailable = outputNode != nullptr
        && outputNode->getProcessor()->getTotalNumInputChannels() > 0;
    // Master -> Sabrina -> saida. Com o Sabrina em bypass (padrao) o caminho
    // e o mesmo de sempre: o processBlock dele nao encosta no buffer.
    connectAudio (masterNode, sabrinaNode, "master_to_sabrina", true);
    connectAudio (sabrinaNode, audioOutNode, "sabrina_to_device_output",
                  outputIsAvailable);

    // MIDI vai para o instrumento e tambem para os efeitos (controle externo)
    if (instrument.uid != 0)
    {
        if (! graph.addConnection (
                { { midiInNode, midiCh }, { instrument, midiCh } },
                juce::AudioProcessorGraph::UpdateKind::none))
        {
            requiredConnectionsOk = false;
            juce::Logger::writeToLog (
                "AUDIO_GRAPH connection_failed edge=midi_to_instrument required=1");
        }
    }

    if (padSynthNode.uid != 0)
    {
        if (! graph.addConnection (
                { { midiInNode, midiCh }, { padSynthNode, midiCh } },
                juce::AudioProcessorGraph::UpdateKind::none))
        {
            requiredConnectionsOk = false;
            juce::Logger::writeToLog (
                "AUDIO_GRAPH connection_failed edge=midi_to_pad_synth required=1");
        }
    }

    for (const auto& fx : fxChain)
        if (juce::AudioProcessorGraph::Node::Ptr node (graph.getNodeForId (fx));
            node != nullptr && node->getProcessor()->acceptsMidi())
        {
            if (! graph.addConnection (
                    { { midiInNode, midiCh }, { fx, midiCh } },
                    juce::AudioProcessorGraph::UpdateKind::none))
                juce::Logger::writeToLog (
                    "AUDIO_GRAPH connection_failed edge=midi_to_effect required=0");
        }

    if (! requiredConnectionsOk)
    {
        for (const auto& c : graph.getConnections())
            graph.removeConnection (c, juce::AudioProcessorGraph::UpdateKind::none);

        bool restored = true;

        for (const auto& c : previousConnections)
            restored &= graph.addConnection (
                c, juce::AudioProcessorGraph::UpdateKind::none);

        juce::Logger::writeToLog (
            "AUDIO_GRAPH transaction_rollback restored="
            + juce::String (restored ? 1 : 0));
    }

    // Uma unica troca atomica do plano de renderizacao: durante toda a
    // montagem (ou rollback) a thread de audio continua usando o grafo antigo.
    graph.rebuild();

    return requiredConnectionsOk;
}

//==============================================================================
void MainComponent::saveAudioStateNow()
{
    if (auto* device = deviceManager.getCurrentAudioDevice();
        device != nullptr && device->isOpen() && device->isPlaying())
    {
        if (auto xml = deviceManager.createStateXml())
        {
            ChurchDAWAudioInputPolicy::applyToSavedSetup (
                xml.get(), audioInputAuthorised.load (
                               std::memory_order_acquire));

            // Pendencia: um setup recem-escolhido so vira "bom" depois de
            // rodar. Se ele matar o app, o boot seguinte descarta e volta
            // para a ultima configuracao comprovada.
            auto* props = appProperties.getUserSettings();
            const auto anterior = [&] {
                if (auto antigo = props->getXmlValue ("audioSetup"))
                    return antigo->toString();
                return juce::String();
            }();
            if (anterior != xml->toString())
                props->setValue ("audioSetupPending", 1);

            props->setValue ("audioSetup", xml.get());
        }

       #if JUCE_ANDROID
        // Um degrau elevado automaticamente por xruns e apenas runtime. O
        // botao Salvar nao pode transforma-lo em preferencia permanente.
        const int userBuffer = appProperties.getUserSettings()->getIntValue (
            "androidUserBufferSize", device->getCurrentBufferSizeSamples());
        appProperties.getUserSettings()->setValue ("bufferSize", userBuffer);
       #else
        appProperties.getUserSettings()->setValue (
            "bufferSize", device->getCurrentBufferSizeSamples());
       #endif
        appProperties.getUserSettings()->setValue ("sampleRate", device->getCurrentSampleRate());
        appProperties.getUserSettings()->saveIfNeeded();
    }
}

void MainComponent::setSabrinaEnabled (bool enabled)
{
    sabrinaEnabled = enabled;

    if (sabrinaProcessor != nullptr)
        sabrinaProcessor->setSabrinaBypassed (! enabled);

    juce::Logger::writeToLog (juce::String ("SABRINA: ")
                                  + (enabled ? "ligada" : "em bypass"));
}

void MainComponent::promoteGoodAudioNow()
{
   #if JUCE_WINDOWS || JUCE_MAC
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    if (auto xml = deviceManager.createStateXml())
    {
        ChurchDAWAudioInputPolicy::applyToSavedSetup (
            xml.get(), audioInputAuthorised.load (std::memory_order_acquire));

        auto* props = appProperties.getUserSettings();
        props->setValue ("audioSetupGood", xml.get());
        props->setValue ("audioSetupPending", 0);
        props->saveIfNeeded();
        juce::Logger::writeToLog (
            "AUDIO_GOOD: setup de audio atual promovido a ultima configuracao boa");
    }
   #endif
}

bool MainComponent::setAudioInputChannelsEnabled (bool enabled)
{
   #if JUCE_ANDROID || JUCE_IOS
    return ! enabled;
   #else
    if (! enabled)
        audioInputAuthorised.store (false, std::memory_order_release);

    auto setup = deviceManager.getAudioDeviceSetup();

    if (enabled)
    {
        setup.useDefaultInputChannels = false;

        if (setup.inputDeviceName.isEmpty())
        {
            if (auto* type = deviceManager.getCurrentDeviceTypeObject())
            {
                const auto names = type->getDeviceNames (true);
                const int defaultIndex = type->getDefaultDeviceIndex (true);

                if (names.contains (setup.outputDeviceName))
                    setup.inputDeviceName = setup.outputDeviceName;
                else if (juce::isPositiveAndBelow (defaultIndex, names.size()))
                    setup.inputDeviceName = names[defaultIndex];
                else if (! names.isEmpty())
                    setup.inputDeviceName = names[0];
            }
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        int available = device != nullptr
                          ? device->getInputChannelNames().size()
                          : 0;

        // Ao acrescentar agora um input separado, o dispositivo corrente foi
        // criado sem ele e ainda nao consegue enumerar seus canais. O app pede
        // no maximo o par estereo; o driver valida a mascara na reabertura.
        if (available <= 0 && setup.inputDeviceName.isNotEmpty())
            available = 2;

        if (available <= 0)
        {
            juce::Logger::writeToLog (
                "AUDIO_INPUT enable_failed reason=no_input_channels");
            return false;
        }

        juce::BigInteger selected;
        for (int channel = 0; channel < available; ++channel)
            if (setup.inputChannels[channel])
                selected.setBit (channel);

        // Preserva a selecao explicita existente; so escolhe 1/2 quando ainda
        // nao havia mascara valida (primeira ativacao ou interface nova).
        if (selected.isZero())
            selected.setRange (0, juce::jmin (2, available), true);

        setup.inputChannels = selected;
    }
    else
    {
        ChurchDAWAudioInputPolicy::disableRuntimeChannels (setup);
    }

    const auto error = deviceManager.setAudioDeviceSetup (setup, true);
    juce::Logger::writeToLog (
        "AUDIO_INPUT enabled=" + juce::String (enabled ? 1 : 0)
        + " active_channels="
        + juce::String (deviceManager.getAudioDeviceSetup()
                            .inputChannels.countNumberOfSetBits())
        + (error.isEmpty() ? juce::String()
                           : " error=\"" + error + "\""));
    return error.isEmpty();
   #endif
}

bool MainComponent::reconcileAudioInputPolicy()
{
   #if JUCE_ANDROID || JUCE_IOS
    return true;
   #else
    // Sem dispositivo aberto nao existe canal de runtime para reconciliar.
    // AudioDeviceManager::getAudioDeviceSetup() devolve o setup padrao quando
    // nao ha device aberto (useDefaultInputChannels volta a true), entao
    // aplicar setAudioDeviceSetup aqui reemite change e cai de novo neste
    // callback para sempre. Em modo seguro isso virava ~1.000 AUDIO_CHANGE/s
    // no log e o motivo de o fechamento nao terminar. A intencao salva
    // (audioDeviceInChans=0) continua valendo quando um device abrir.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return true;

    const bool authorised =
        audioInputAuthorised.load (std::memory_order_acquire);
    const auto setup = deviceManager.getAudioDeviceSetup();

    if (! authorised)
    {
        if (! ChurchDAWAudioInputPolicy::runtimeSetupIsBlocked (setup))
            if (! setAudioInputChannelsEnabled (false))
                return false;
    }
    else if (setup.useDefaultInputChannels || setup.inputChannels.isZero())
    {
        // A intencao continua verdadeira se a interface estiver ausente. Assim
        // um hotplug posterior pode reaplicar a entrada em vez de apagar uma
        // escolha que o usuario fez de proposito.
        if (! setAudioInputChannelsEnabled (true))
            return false;
    }

    const auto stable = deviceManager.getAudioDeviceSetup();
    if (! authorised
         && ! ChurchDAWAudioInputPolicy::runtimeSetupIsBlocked (stable))
        return false;

    if (const auto* device = deviceManager.getCurrentAudioDevice())
    {
        const int active =
            device->getActiveInputChannels().countNumberOfSetBits();
        if (! authorised && active != 0)
            return false;
    }

    return true;
   #endif
}

// painel do seletor de audio EMBUTIDO na DAW: card premium com sombra, acento
// vermelho, arrastavel pela barra de titulo — sem janela do Windows. Marca o
// periodo em que esta aberto (a config so e salva com ele aberto)
//
// No Android vira PAGINA full-bleed (nao card flutuante): cabecalho fixo,
// seletor JUCE no meio, rodape em secoes (MIDI / acoes). O seletor continua
// sendo o componente testado do JUCE — so o envelope visual e o layout mudam.
struct AudioSettingsHolder : public juce::Component,
                             private juce::Timer
{
   #if CHURCHDAW_MOBILE_SHELL
    static constexpr int margin = 0, headerH = 64, footerPad = 14;
   #else
    static constexpr int margin = 12, headerH = 82;
   #endif

    explicit AudioSettingsHolder (MainComponent& mc)
        : owner (&mc),
          selector (mc.deviceManager, [safe = juce::Component::SafePointer<MainComponent> (&mc)]
          {
              if (auto* host = safe.getComponent()) host->startDiagnosticOutputTest();
          })
    {
        mc.audioDialogOpen = true;
        setWantsKeyboardFocus (true);
        setTitle (juce::String::fromUTF8 ("Áudio e MIDI"));
        brandIcon = juce::ImageCache::getFromMemory (BinaryData::logo_round_png,
                                                     BinaryData::logo_round_pngSize);

       #if CHURCHDAW_MOBILE_SHELL
        selector.setItemHeight (44);
        selectorViewport.setViewedComponent (&selector, false);
        selectorViewport.setScrollBarsShown (true, false);
        selectorViewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
        selectorViewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (selectorViewport);

        sectionDevice.setText (juce::String::fromUTF8 ("DISPOSITIVO"), juce::dontSendNotification);
        sectionDevice.setFont (juce::Font (juce::FontOptions (11.0f)).boldened()
                                   .withExtraKerningFactor (0.12f));
        sectionDevice.setColour (juce::Label::textColourId,
                                 NordLookAndFeel::nordRedBright().withAlpha (0.90f));
        sectionDevice.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (sectionDevice);

        sectionMidi.setText (juce::String::fromUTF8 ("MIDI E TECLADO"), juce::dontSendNotification);
        sectionMidi.setFont (juce::Font (juce::FontOptions (11.0f)).boldened()
                                 .withExtraKerningFactor (0.12f));
        sectionMidi.setColour (juce::Label::textColourId,
                               NordLookAndFeel::nordRedBright().withAlpha (0.90f));
        sectionMidi.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (sectionMidi);

        statusChip.setText (juce::String::fromUTF8 (u8"Ajuste saída, latência e teclado"),
                            juce::dontSendNotification);
        statusChip.setFont (juce::Font (juce::FontOptions (12.5f)));
        statusChip.setColour (juce::Label::textColourId,
                              NordLookAndFeel::txtSecondary());
        statusChip.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (statusChip);
       #else
        selector.setItemHeight (30);
        selectorViewport.setViewedComponent (&selector, false);
        selectorViewport.setScrollBarsShown (true, false);
        selectorViewport.setScrollBarThickness (8);
        selectorViewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (selectorViewport);
       #endif

        // O seletor de audio e do JUCE: reescrever ele so pra ficar bonito seria
        // trocar um componente testado (troca de driver, ASIO, canais, sample rate)
        // por bugs novos num app que vai pro palco. Em vez disso, o acabamento e
        // aplicado por FORA, nos filhos dele. O timer reaplica porque o JUCE
        // recria esses filhos quando se troca o tipo de driver.
        restyleSelector (selector);
        startTimer (700);

        // No Android/iOS o motor abre com ZERO entradas: o app e instrumento,
        // nao gravador, e por isso nao pede acesso ao microfone. Mostrar ali um
        // toggle que nunca poderia funcionar era uma promessa falsa.
       #if CHURCHDAW_MOBILE_SHELL
        audioInputNote.setText (juce::String::fromUTF8 (
                                    "\xe2\x9c\x93 Sem microfone \xc2\xb7 s\xc3\xb3 instrumento"),
                                juce::dontSendNotification);
        audioInputNote.setJustificationType (juce::Justification::centredLeft);
        audioInputNote.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
        audioInputNote.setColour (juce::Label::textColourId, NordLookAndFeel::greenOk());
        addAndMakeVisible (audioInputNote);
       #else
         // No desktop a entrada de guitarra/mic e opcional e vive aqui.
         addAndMakeVisible (mc.inputToggle);
       #endif

         launchkeyToggle.setButtonText (juce::String::fromUTF8 (
             "Mapeamento autom\xc3\xa1tico do Launchkey"));
         launchkeyToggle.setTooltip (juce::String::fromUTF8 (
             "Quando desligado, o Launchkey funciona apenas como entrada MIDI comum; "
             "faders, knobs e pads deixam de controlar o ChurchDAW automaticamente."));
         launchkeyToggle.setToggleState (mc.launchkeyMappingEnabled,
                                         juce::dontSendNotification);
         launchkeyToggle.onClick = [this]
         {
             if (auto* mc2 = owner.getComponent())
                 mc2->setLaunchkeyMappingEnabled (launchkeyToggle.getToggleState());
         };
         addAndMakeVisible (launchkeyToggle);

        // RESPOSTA DO TECLADO: cada controlador manda uma velocity diferente
        // pra mesma forca. Fica aqui, e nao na regua master, porque e do mesmo
        // assunto da entrada MIDI logo acima -- calibragem do equipamento, se
        // ajusta uma vez por teclado.
       #if CHURCHDAW_MOBILE_SHELL
        velLabel.setText (juce::String::fromUTF8 ("Resposta do teclado"),
                          juce::dontSendNotification);
        velLabel.setJustificationType (juce::Justification::centredLeft);
        velLabel.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
       #else
        velLabel.setText (juce::String::fromUTF8 ("Resposta do teclado:"), juce::dontSendNotification);
        velLabel.setJustificationType (juce::Justification::centredRight);
        velLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
       #endif
        velLabel.setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
        addAndMakeVisible (velLabel);

        velCombo.setWantsKeyboardFocus (false);   // setas do PC nao trocam a curva

        if (auto* live = mc.getChurchLive())
        {
            if (auto* p = live->getVelCurveParam())
            {
                velCombo.addItemList (p->choices, 1);
                velAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (*p, velCombo);
            }
        }

        if (velAttachment == nullptr)
        {
            velCombo.addItem (juce::String::fromUTF8 ("Church Live n\xc3\xa3o carregado"), 1);
            velCombo.setSelectedId (1, juce::dontSendNotification);
            velCombo.setEnabled (false);
        }

        velCombo.setTooltip (juce::String::fromUTF8 (
            "Leve = o instrumento soa cheio com menos for\xc3\xa7""a (teclas pesadas "
            "de martelo). Pesada = exige mais for\xc3\xa7""a (teclado leve que manda "
            "velocity alta em qualquer toque). Fixa = ignora a din\xc3\xa2mica, bom "
            "para \xc3\xb3rg\xc3\xa3o e pad."));
       #if CHURCHDAW_MOBILE_SHELL
        velCombo.setColour (juce::ComboBox::backgroundColourId, NordLookAndFeel::bgControl());
        velCombo.setColour (juce::ComboBox::outlineColourId, NordLookAndFeel::borderDefault());
        velCombo.setColour (juce::ComboBox::textColourId, NordLookAndFeel::txtPrimary());
        velCombo.setColour (juce::ComboBox::arrowColourId, NordLookAndFeel::nordRedBright());
       #endif
        addAndMakeVisible (velCombo);

        // fecha o card (o destrutor salva a config)
        closeBtn.setComponentID ("timbrePickerClose");
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
       #if CHURCHDAW_MOBILE_SHELL
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a3038));
        closeBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
       #else
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
       #endif
        closeBtn.onClick = [this]
        {
            juce::Component::SafePointer<MainComponent> mc2 (owner.getComponent());
            juce::MessageManager::callAsync ([mc2]
            {
                if (auto* m = mc2.getComponent())
                {
                    m->audioPanel.reset();
                    m->audioSettingsButton.repaint();
                }
            });
        };
        addAndMakeVisible (closeBtn);

        // recupera o driver de "pipocos" SEM desplugar o cabo (e sem travar o app).
        // No Android o texto deixa claro o caso USB class-compliant.
       #if JUCE_ANDROID || CHURCHDAW_MOBILE_UI_PREVIEW
        const auto restartLabel = juce::String::fromUTF8 ("Recuperar USB");
       #else
        const auto restartLabel = juce::String::fromUTF8 (
            "\xe2\x86\xbb Reiniciar \xc3\xa1udio");   // ↻
       #endif
        restartBtn.setButtonText (restartLabel);
        restartBtn.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::bgControlHi());
        restartBtn.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRedBright());
        restartBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        restartBtn.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        restartBtn.setTooltip (
           #if JUCE_ANDROID
            juce::String::fromUTF8 (
                "Fecha e reabre a saida de audio. Use se a interface USB "
                "sumiu, voltou muda ou ficou com estalo apos plug/unplug.")
           #else
            juce::String::fromUTF8 (
                "Fecha e reabre o driver de audio sem desligar o app.")
           #endif
        );
        restartBtn.onClick = [this, restartLabel]
        {
            restartBtn.setEnabled (false);
            restartBtn.setButtonText ("...");

            juce::Component::SafePointer<AudioSettingsHolder> safe (this);
            juce::MessageManager::callAsync ([safe, restartLabel]
            {
                if (auto* h = safe.getComponent())
                    if (auto* mc2 = h->owner.getComponent())
                    {
                       #if JUCE_ANDROID
                        mc2->androidForceAudioReconcile = true;
                       #endif
                        mc2->restartAudioDevice();
                    }

                juce::Timer::callAfterDelay (600, [safe, restartLabel]
                {
                    if (auto* h = safe.getComponent())
                    {
                        h->restartBtn.setEnabled (true);
                        h->restartBtn.setButtonText (restartLabel);
                    }
                });
            });
        };
        addAndMakeVisible (restartBtn);

       #if CHURCHDAW_MOBILE_SHELL
        // MIDI BLE: permissoes + atalho para Ajustes de Bluetooth (pareamento).
        bleMidiBtn.setButtonText (juce::String::fromUTF8 ("MIDI Bluetooth"));
        bleMidiBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff252a32));
        bleMidiBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
        bleMidiBtn.setTooltip (juce::String::fromUTF8 (
            "Abre o pareamento de dispositivos MIDI Bluetooth e atualiza as "
            "portas de entrada automaticamente."));
        bleMidiBtn.onClick = [this]
        {
           #if JUCE_IOS
            if (juce::BluetoothMidiDevicePairingDialogue::isAvailable())
            {
                juce::BluetoothMidiDevicePairingDialogue::open();
                if (auto* mc2 = owner.getComponent())
                    mc2->refreshMidiInputs();
            }
           #elif JUCE_ANDROID
            // Manter LocalRef vivo: getMainActivity().get() em temporário
            // invalida o jobject e causa SIGSEGV em openBluetoothSettings.
            auto launch = [safeOwner = owner]
            {
                if (juce::BluetoothMidiDevicePairingDialogue::isAvailable()
                    && juce::RuntimePermissions::isGranted (
                           juce::RuntimePermissions::bluetoothMidi))
                {
                    if (juce::BluetoothMidiDevicePairingDialogue::open())
                    {
                        if (auto* mc2 = safeOwner.getComponent())
                            mc2->refreshMidiInputs();
                        return;
                    }
                }

                auto* env = juce::getEnv();
                juce::LocalRef<jobject> activity (juce::getMainActivity());
                if (env == nullptr || activity.get() == nullptr)
                    return;

                jclass clazz = env->FindClass ("com/churchdaw/app/ChurchDAWMidiBleBridge");
                if (clazz == nullptr)
                {
                    env->ExceptionClear();
                    return;
                }

                jmethodID mid = env->GetStaticMethodID (
                    clazz, "openBluetoothSettings",
                    "(Landroid/app/Activity;)V");
                if (mid != nullptr)
                    env->CallStaticVoidMethod (clazz, mid, activity.get());
                else
                    env->ExceptionClear();

                env->DeleteLocalRef (clazz);

                if (auto* mc2 = safeOwner.getComponent())
                    mc2->refreshMidiInputs();
            };

            if (! juce::RuntimePermissions::isGranted (
                    juce::RuntimePermissions::bluetoothMidi))
            {
                juce::RuntimePermissions::request (
                    juce::RuntimePermissions::bluetoothMidi,
                    [launch] (bool)
                    {
                        juce::MessageManager::callAsync (launch);
                    });
                return;
            }

            juce::MessageManager::callAsync (launch);
           #endif
        };
        addAndMakeVisible (bleMidiBtn);
       #endif

       #if CHURCHDAW_MOBILE_SHELL
        // O tamanho final vem de showAudioSettings(), usando a janela real.
        setSize (360, 640);
       #else
         setSize (1020, 650);
       #endif
    }

    ~AudioSettingsHolder() override
    {
        stopTimer();

        selectorViewport.setViewedComponent (nullptr, false);

        if (auto* c = owner.getComponent())
        {
            c->audioDialogOpen = false;
            c->saveAudioStateNow();
        }
    }

    void timerCallback() override
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        // Snapshot SOMENTE LEITURA do estado de audio enquanto o painel esta
        // aberto (a cada 700 ms). Quando o processo morre ao trocar de driver
        // (ASIO4ALL), a ultima linha do log mostra o que estava selecionado ANTES
        // do clique fatal -- so assim fecha o ponto exato da queda sem
        // adivinhar. Nao altera nenhum combo nem chama setAudioDeviceSetup.
        if (owner != nullptr)
        {
            auto& dm = owner->deviceManager;
            const auto setup = dm.getAudioDeviceSetup();
            const auto* type = dm.getCurrentDeviceTypeObject();
            const auto* dev = dm.getCurrentAudioDevice();
            juce::Logger::writeToLog (
                "AUDIO_DIALOG type=\""
                + (type != nullptr ? type->getTypeName() : juce::String())
                + "\" output=\"" + setup.outputDeviceName
                + "\" input=\"" + setup.inputDeviceName
                + "\" inChannels=" + juce::String (setup.inputChannels.countNumberOfSetBits())
                + " buffer=" + juce::String (setup.bufferSize)
                + " rate=" + juce::String (setup.sampleRate, 0)
                + (dev != nullptr ? juce::String (" open=yes \"") + dev->getName() + "\""
                                   : juce::String (" open=NO")));
        }
       #endif
        restyleSelector (selector);
    }

    // Acabamento por fora no seletor do JUCE: rotulos deixam de ter peso de
    // formulario e os botoes ganham hierarquia — "Resetar" e o unico perigoso,
    // e estava com a mesma aparencia de "Testar". Se o JUCE mudar os filhos, no
    // pior caso volta o visual padrao: nada quebra.
    static void restyleSelector (juce::Component& parent)
    {
       #if CHURCHDAW_MOBILE_SHELL
        const float labelPt = 12.5f;
        const juce::uint32 comboBg = 0xff22272f;
       #else
        const float labelPt = 12.0f;
        const juce::uint32 comboBg = 0xff2a2d34;
       #endif

        for (auto* child : parent.getChildren())
        {
            // o ComboBox desenha o VALOR num Label filho: apagar esse texto faria
            // o valor escolhido parecer desabilitado
            if (auto* box = dynamic_cast<juce::ComboBox*> (child))
            {
                box->setColour (juce::ComboBox::backgroundColourId, juce::Colour ((juce::uint32) comboBg));
                box->setColour (juce::ComboBox::outlineColourId, NordLookAndFeel::borderDefault());
                box->setColour (juce::ComboBox::textColourId, NordLookAndFeel::txtPrimary());
                box->setColour (juce::ComboBox::arrowColourId, NordLookAndFeel::nordRedBright());
                restyleSelector (*child);
                continue;
            }

            if (auto* label = dynamic_cast<juce::Label*> (child))
            {
                label->setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
                label->setFont (juce::Font (juce::FontOptions (labelPt)));
            }
            else if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                const auto text = button->getButtonText();

                if (text.containsIgnoreCase ("Resetar") || text.containsIgnoreCase ("Reset"))
                {
                    button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a2226));
                    button->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe08a6a));
                }
                else
                {
                    button->setColour (juce::TextButton::buttonColourId, NordLookAndFeel::bgControl());
                    button->setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
                }
            }
            else if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
            {
                toggle->setColour (juce::ToggleButton::textColourId, NordLookAndFeel::txtPrimary());
                toggle->setColour (juce::ToggleButton::tickColourId, NordLookAndFeel::nordRedBright());
                toggle->setColour (juce::ToggleButton::tickDisabledColourId,
                                   NordLookAndFeel::txtDisabled());
            }
            else if (auto* listBox = dynamic_cast<juce::ListBox*> (child))
            {
                listBox->setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1c2027));
                listBox->setColour (juce::ListBox::outlineColourId, NordLookAndFeel::borderSoft());
                listBox->setColour (juce::ListBox::textColourId, NordLookAndFeel::txtPrimary());
                listBox->setOutlineThickness (1);
            }

            restyleSelector (*child);   // as listas de canais/MIDI tem filhos proprios
        }
    }

    // No landscape o seletor JUCE (label a esquerda + combo a direita) vira
    // lixo se a largura for a tela inteira: labels num canto, controles no
    // outro, mar negro no meio. Card central estreito (~600-680) e o layout
    // natural do JUCE de novo.
    juce::Rectangle<int> contentColumn() const
    {
        auto full = getLocalBounds();
       #if CHURCHDAW_MOBILE_SHELL
        const bool shortMobile = full.getHeight() < 480 && full.getWidth() >= 540;
        const int maxW = juce::jmin (shortMobile ? 820 : 680,
                                     juce::jmax (320, full.getWidth() - (shortMobile ? 24 : 48)));
        return full.withSizeKeepingCentre (maxW, full.getHeight())
                   .reduced (0, shortMobile ? 6 : 10);
       #else
        const int width = juce::jmin (1020, juce::jmax (320, full.getWidth() - 40));
        const int height = juce::jmin (650, juce::jmax (360, full.getHeight() - 32));
        return full.withSizeKeepingCentre (width, height);
       #endif
    }

    void paint (juce::Graphics& g) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        // Scrim escuro sobre o mixer + card central premium.
        g.setColour (juce::Colours::black.withAlpha (0.72f));
        g.fillRect (getLocalBounds());

        auto col = contentColumn().toFloat();
        const float rad = 18.0f;
        {
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.fillRoundedRectangle (col.expanded (10.0f).translated (0.0f, 8.0f), rad + 8.0f);
            g.setColour (juce::Colours::black.withAlpha (0.38f));
            g.fillRoundedRectangle (col.expanded (4.0f).translated (0.0f, 4.0f), rad + 3.0f);
        }

        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff1c2028), col.getX(), col.getY(),
            juce::Colour (0xff12151a), col.getX(), col.getBottom(), false));
        g.fillRoundedRectangle (col, rad);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (col.reduced (0.5f), rad, 1.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.70f));
        g.drawRoundedRectangle (col, rad, 1.4f);

        // Barra vermelha no topo do card
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (col.getX() + 20.0f, col.getY() + 1.0f,
                                col.getWidth() - 40.0f, 3.0f, 1.5f);

        const bool shortMobile = col.getHeight() < 480.0f && col.getWidth() >= 540.0f;
        const int mobileHeadH = shortMobile ? 52 : headerH;
        auto head = col.toNearestInt().removeFromTop (mobileHeadH);
        if (brandIcon.isValid())
        {
            g.setOpacity (1.0f);
            g.drawImageWithin (brandIcon, head.getX() + 18, head.getY() + (shortMobile ? 10 : 14),
                               shortMobile ? 32 : 34, shortMobile ? 32 : 34,
                               juce::RectanglePlacement::centred);
        }
        else
        {
            NordLookAndFeel::drawChurchDAWMark (
                g, { (float) head.getX() + 16.0f, (float) head.getY() + (shortMobile ? 8.0f : 12.0f),
                     shortMobile ? 34.0f : 38.0f, shortMobile ? 34.0f : 38.0f });
        }

        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (shortMobile ? 15.5f : 17.0f)).boldened()
                       .withExtraKerningFactor (0.04f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x81UDIO / MIDI"),
                    head.withTrimmedLeft (58).withTrimmedRight (52)
                        .withTop (head.getY() + (shortMobile ? 7 : 10)).withHeight (22),
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::txtSecondary().withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (juce::String::fromUTF8 ("Sa\xc3\xad" "da, lat\xc3\xaa" "ncia e teclado"),
                    head.withTrimmedLeft (58).withTrimmedRight (52)
                        .withTop (head.getY() + (shortMobile ? 27 : 33)).withHeight (18),
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::borderDefault().withAlpha (0.8f));
        g.fillRect (head.getX() + 16, head.getBottom() - 1, head.getWidth() - 32, 1);

        // Faixa de acoes (rodape no tablet/retrato, coluna lateral no landscape compacto)
        if (! footerBand.isEmpty())
        {
            auto foot = footerBand.toFloat();
            if (shortMobile)
            {
                g.setColour (juce::Colour (0xff161a20));
                g.fillRoundedRectangle (foot, 10.0f);
                g.setColour (NordLookAndFeel::borderDefault().withAlpha (0.55f));
                g.drawRoundedRectangle (foot.reduced (0.5f), 10.0f, 1.0f);
            }
            else
            {
                g.setColour (juce::Colour (0xff161a20));
                g.fillRoundedRectangle (foot.getX(), (float) footerBand.getY(),
                                        foot.getWidth(), foot.getHeight() + 8.0f, 0.0f);
                juce::Path p;
                p.addRoundedRectangle (foot.getX(), foot.getY() - 1.0f,
                                       foot.getWidth(), foot.getHeight() + 10.0f,
                                       0.0f, 0.0f, false, false, true, true);
                g.fillPath (p);
                g.setColour (NordLookAndFeel::borderDefault().withAlpha (0.55f));
                g.fillRect (foot.getX() + 16.0f, foot.getY(), foot.getWidth() - 32.0f, 1.0f);
            }
        }
       #else
        g.fillAll (juce::Colours::black.withAlpha (0.64f));
        const auto card = contentColumn();
        g.setColour (NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (card.toFloat(), 14.0f);
        g.setColour (juce::Colours::white.withAlpha (0.085f));
        g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 14.0f, 1.0f);
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle ((float) card.getX() + 28.0f, (float) card.getY() + 1.0f,
                                (float) card.getWidth() - 56.0f, 2.5f, 1.25f);
        auto head = card.reduced (28, 0).removeFromTop (headerH);
        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (24.0f)).boldened());
        g.drawText (juce::String::fromUTF8 ("Áudio e MIDI"), head.withTrimmedTop (16).withHeight (32), juce::Justification::centredLeft);
        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (juce::String::fromUTF8 ("Seu equipamento, pronto para tocar."), head.withTrimmedTop (51).withHeight (22), juce::Justification::centredLeft);
        g.setColour (juce::Colours::white.withAlpha (0.055f));
        g.fillRect (head.getX(), head.getBottom() - 2, head.getWidth(), 1);
        auto section = [&] (juce::Rectangle<int> rect, const juce::String& title)
        {
            g.setColour (NordLookAndFeel::bgApp().withAlpha (0.55f));
            g.fillRoundedRectangle (rect.toFloat(), 10.0f);
            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.drawRoundedRectangle (rect.toFloat().reduced (0.5f), 10.0f, 1.0f);
            g.setColour (NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
            g.drawText (title, rect.reduced (20, 12).removeFromTop (20), juce::Justification::centredLeft);
        };
        if (card.getWidth() >= 850)
        {
            auto body = card.reduced (24, 20).withTrimmedTop (headerH);
            auto right = body.removeFromRight (290);
            body.removeFromRight (20);
            section (body, juce::String::fromUTF8 ("Dispositivo e conexões"));
            section (right, "Teclado e controle");
            auto hint = right.reduced (20, 0).withTrimmedTop (126).withHeight (44);
            g.setColour (NordLookAndFeel::txtDisabled());
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawMultiLineText (juce::String::fromUTF8 ("Ajuste como a força das teclas responde no instrumento."),
                                 hint.getX(), hint.getY() + 14, hint.getWidth(), juce::Justification::left, 3.0f);
            auto recoveryHint = right.reduced (20, 0).withTrimmedTop (right.getHeight() - 108);
            g.drawMultiLineText (juce::String::fromUTF8 ("Se o som parar, reabra o dispositivo mantendo suas configurações."),
                                 recoveryHint.getX(), recoveryHint.getY() + 14, recoveryHint.getWidth(), juce::Justification::left, 3.0f);
        }
       #endif
    }

    void resized() override
    {
       #if CHURCHDAW_MOBILE_SHELL
        auto col = contentColumn();
        const bool shortMobile = col.getHeight() < 480 && col.getWidth() >= 540;
        const int mobileHeadH = shortMobile ? 52 : headerH;
        closeBtn.setBounds (col.getRight() - 48, col.getY() + (shortMobile ? 6 : 12), 40, 40);

        auto body = col;
        body.removeFromTop (mobileHeadH + 6);
        body = body.reduced (shortMobile ? 14 : 18, 0).withTrimmedBottom (shortMobile ? 8 : 12);

        if (shortMobile)
        {
            const int rightW = juce::jlimit (244, 304, body.getWidth() * 43 / 100);
            footerBand = body.removeFromRight (rightW);
            body.removeFromRight (12);

            auto foot = footerBand.reduced (10, 8);
            sectionMidi.setBounds (foot.removeFromTop (16));
            foot.removeFromTop (4);
            velLabel.setJustificationType (juce::Justification::centredLeft);
            velLabel.setBounds (foot.removeFromTop (18));
            foot.removeFromTop (2);
            velCombo.setBounds (foot.removeFromTop (36));
            foot.removeFromTop (8);

            auto actions = foot.removeFromTop (40);
            const int gap = 8;
            const int w = (actions.getWidth() - gap) / 2;
            bleMidiBtn.setBounds (actions.removeFromLeft (w));
            actions.removeFromLeft (gap);
            restartBtn.setBounds (actions);

            foot.removeFromTop (6);
            launchkeyToggle.setBounds (foot.removeFromTop (34));
            foot.removeFromTop (2);
            audioInputNote.setBounds (foot.removeFromTop (20));
        }
        else
        {
            // Rodape compacto: velocity + 2 botoes + status. Sem "metade da tela".
            constexpr int footerH = 206;
            footerBand = body.removeFromBottom (juce::jmin (footerH, body.getHeight() - 120));
            auto foot = footerBand.reduced (4, 8);

            sectionMidi.setBounds (foot.removeFromTop (16));
            foot.removeFromTop (4);

            // velocity: label esquerda, combo direita (uma linha)
            auto velRow = foot.removeFromTop (40);
            velLabel.setJustificationType (juce::Justification::centredLeft);
            velLabel.setBounds (velRow.removeFromLeft (juce::jmin (150, velRow.getWidth() / 3))
                                   .withSizeKeepingCentre (juce::jmin (150, velRow.getWidth()), 22));
            velRow.removeFromLeft (8);
            velCombo.setBounds (velRow.withSizeKeepingCentre (velRow.getWidth(), 36));

            foot.removeFromTop (8);
            auto actions = foot.removeFromTop (44);
            const int gap = 10;
            const int w = (actions.getWidth() - gap) / 2;
            bleMidiBtn.setBounds (actions.removeFromLeft (w));
            actions.removeFromLeft (gap);
            restartBtn.setBounds (actions);

            foot.removeFromTop (6);
            launchkeyToggle.setBounds (foot.removeFromTop (34));
            foot.removeFromTop (4);
            audioInputNote.setBounds (foot.removeFromTop (20));
        }

        // Corpo: titulo da secao + seletor com LARGURA do card (nao da tela).
        sectionDevice.setBounds (body.removeFromTop (16));
        body.removeFromTop (shortMobile ? 2 : 4);
        statusChip.setBounds (body.removeFromTop (18));
        body.removeFromTop (4);

        selectorViewport.setBounds (body);
        // Altura do seletor ~ conteudo real (item 44 * ~6 linhas + labels).
        // Nao esticar ate o fundo — o "mar" preto some.
        const int contentH = juce::jmax (260, selector.getItemHeight() * 7 + 48);
        selector.setBounds (0, 0, juce::jmax (240, body.getWidth() - 8), contentH);
       #else
        auto card = contentColumn();
        closeBtn.setBounds (card.getRight() - 62, card.getY() + 24, 36, 36);
        auto area = card.reduced (24, 20).withTrimmedTop (headerH);
        if (card.getWidth() >= 850)
        {
            auto right = area.removeFromRight (290).reduced (20, 0);
            area.removeFromRight (20);
            selectorViewport.setBounds (area.reduced (12, 0).withTrimmedTop (42));
            selector.setSize (juce::jmax (1, selectorViewport.getWidth() - 10), 600);
            right.removeFromTop (52);
            velLabel.setJustificationType (juce::Justification::centredLeft);
            velLabel.setBounds (right.removeFromTop (24));
            right.removeFromTop (4);
            velCombo.setBounds (right.removeFromTop (36));
            right.removeFromTop (76);
            launchkeyToggle.setButtonText (juce::String::fromUTF8 ("Launchkey automático"));
            launchkeyToggle.setBounds (right.removeFromTop (44));
            right.removeFromTop (18);
            if (owner != nullptr)
                owner->inputToggle.setBounds (right.removeFromTop (44));
            restartBtn.setBounds (right.removeFromBottom (54).withTrimmedBottom (16));
        }
        else
        {
            auto launchRow = area.removeFromBottom (34);
            launchkeyToggle.setBounds (launchRow);
            auto extras = area.removeFromBottom (42);
            restartBtn.setBounds (extras.removeFromRight (158).reduced (0, 4));
            if (owner != nullptr) owner->inputToggle.setBounds (extras);
            auto vel = area.removeFromBottom (40);
            velLabel.setBounds (vel.removeFromLeft (150));
            velCombo.setBounds (vel.reduced (4));
            selectorViewport.setBounds (area);
            selector.setSize (juce::jmax (1, selectorViewport.getWidth() - 10), 600);
        }
       #endif
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        juce::ignoreUnused (g);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        {
            closeBtn.triggerClick();
            return true;
        }
        return false;
    }

    // arrasta o card pela faixa do cabecalho (desktop). No mobile a pagina
    // e full-bleed e nao se arrasta.
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = false;
        if (! contentColumn().contains (e.getPosition()))
            closeBtn.triggerClick();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::ignoreUnused (e);
       #else
        if (dragging)
            dragger.dragComponent (this, e, nullptr);
       #endif
    }

    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }

    juce::Component::SafePointer<MainComponent> owner;
    GuardedAudioDeviceSelector selector;
    juce::Viewport selectorViewport;
   #if CHURCHDAW_MOBILE_SHELL
    juce::Label sectionDevice, sectionMidi, statusChip;
    juce::Rectangle<int> footerBand;
   #endif
     juce::TextButton restartBtn, closeBtn;
     juce::ToggleButton launchkeyToggle;
   #if CHURCHDAW_MOBILE_SHELL
    juce::TextButton bleMidiBtn;
   #endif
    juce::Label velLabel, audioInputNote;
    juce::ComboBox velCombo;
    std::unique_ptr<juce::ComboBoxParameterAttachment> velAttachment;
    juce::Image brandIcon;
    juce::ComponentDragger dragger;
    bool dragging = false;
};

// fecha e reabre o dispositivo com a MESMA config salva: recupera o driver
// ASIO de estado ruim (pipocos) sem desplugar o cabo — e sem o travamento da
// re-enumeracao USB
void MainComponent::restartAudioDevice()
{
   #if JUCE_ANDROID
    // O botao manual tambem precisa renovar o cache: reabrir o mesmo objeto
    // Oboe com o mesmo ID antigo nao recupera uma interface USB reconectada.
    androidPendingAudioTopologyGeneration = juce::jmax (
        androidPendingAudioTopologyGeneration,
        juce::jmax (androidAudioTopologyGeneration,
                    getAndroidAudioTopologyGeneration()) + 1);
    androidForceAudioReconcile = true;
    androidAudioReconcileScheduled = false;
    reconcileAndroidAudioTopology();
   #else
    // restartLastAudioDevice reaplica currentSetup literalmente. Normalize-o
    // antes de fechar para que um setup antigo/default nao ressuscite entradas.
    (void) reconcileAudioInputPolicy();
    deviceManager.closeAudioDevice();
    deviceManager.restartLastAudioDevice();
    (void) reconcileAudioInputPolicy();
    auto* reopened = deviceManager.getCurrentAudioDevice();
    if (reopened == nullptr || ! reopened->isOpen() || ! reopened->isPlaying())
    {
        lastAudioDriverError = juce::String::fromUTF8 ("A interface n\xc3\xa3o foi reaberta. Confira a conex\xc3\xa3o e a sa\xc3\xad" "da escolhida.");
        juce::Logger::writeToLog ("AUDIO_RESTART failed: " + lastAudioDriverError);
        statusInfo.setTooltip (lastAudioDriverError);
    }
    else
    {
        lastAudioDriverError.clear();
        juce::Logger::writeToLog ("AUDIO_RESTART ready: " + reopened->getName());
    }
   #endif
}

#if ! (JUCE_ANDROID || JUCE_IOS)
// Ajuste de latencia da PRIMEIRA ABERTURA no desktop (ver o comentario no
// construtor): prefere ASIO ou Windows Audio Low Latency e aplica ~10 ms.
void MainComponent::tuneDesktopFirstRunAudio()
{
    auto* settings = appProperties.getUserSettings();

    if (settings->getIntValue ("bufferSize", 0) != 0
         || settings->getIntValue ("desktopFirstRunAudioTuned", 0) != 0)
        return;

    const juce::String preferredType = [this]() -> juce::String
    {
        const auto& types = deviceManager.getAvailableDeviceTypes();

        // Enumerar driver ASIO quebrado pode lancar (ou abrir modal do
        // vendor) dentro do construtor, antes de existir janela: nao pode
        // impedir o app de abrir.
        auto scanNoThrow = [] (juce::AudioIODeviceType* t) -> bool
        {
            try
            {
                t->scanForDevices();
                return ! t->getDeviceNames().isEmpty();
            }
            catch (...)
            {
                juce::Logger::writeToLog (
                    "AUDIO_DESKTOP scan_failed type=\""
                    + t->getTypeName() + "\"");
                return false;
            }
        };

        for (int i = 0; i < types.size(); ++i)
        {
            auto* t = types.getUnchecked (i);

            if (t->getTypeName() == "ASIO" && scanNoThrow (t))
                return "ASIO";
        }

        for (int i = 0; i < types.size(); ++i)
        {
            auto* t = types.getUnchecked (i);

            if (t->getTypeName() == "Windows Audio (Low Latency Mode)"
                 && scanNoThrow (t))
                return "Windows Audio (Low Latency Mode)";
        }

        return {};
    }();

    // Sem NENHUMA saida (fone desplugado na primeira abertura, PC sem caixa)
    // nao ha o que ajustar. Gravar a marca aqui fazia a primeira abertura COM
    // saida nunca mais passar por este ajuste: o app ficava para sempre no
    // WASAPI compartilhado, o de maior atraso. A marca continua sendo gravada
    // ANTES de qualquer troca de driver (protecao contra crash-loop).
    if (preferredType.isEmpty() && deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    settings->setValue ("desktopFirstRunAudioTuned", 1);

    const juce::String tipoAnterior =
        deviceManager.getCurrentAudioDeviceType();

    if (preferredType.isEmpty() || tipoAnterior == preferredType)
        return;

    deviceManager.setCurrentAudioDeviceType (preferredType, true);

    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        // O tipo preferido nao abriu nenhum dispositivo (placa ASIO
        // desconectada, em uso exclusivo, ASIO4ALL sem hardware). Sem
        // este rollback o app abriria MUDO -- e o tipo ja teria sido
        // persistido pelo "true" acima.
        juce::Logger::writeToLog (
            "AUDIO_DESKTOP preferred_type_failed preferred=\""
            + preferredType + "\" reverting_to=\""
            + tipoAnterior + "\"");
        deviceManager.setCurrentAudioDeviceType (tipoAnterior, true);
    }

    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        const auto& sizes = dev->getAvailableBufferSizes();
        const double rate = dev->getCurrentSampleRate() > 0.0
                               ? dev->getCurrentSampleRate() : 48000.0;

        int chosen = 0;
        for (const auto s : sizes)
            if (((double) s / rate) * 1000.0 >= 10.0)
            {
                chosen = s;
                break;
            }

        if (chosen == 0 && ! sizes.isEmpty())
            chosen = sizes.getLast();      // aparelho so oferece buffers curtos

        if (chosen > 0)
        {
            auto setup = deviceManager.getAudioDeviceSetup();

            if (setup.bufferSize != chosen)
            {
                setup.bufferSize = chosen;
                deviceManager.setAudioDeviceSetup (setup, true);
            }

            // persiste so o buffer; o tipo ja foi gravado por
            // setCurrentAudioDeviceType(..., true). Assim as aberturas
            // seguintes nao recaem aqui (bufferSize != 0).
            settings->setValue ("bufferSize", chosen);
        }

        juce::Logger::writeToLog (
            "AUDIO_DESKTOP first_run_tuned type=\"" + deviceManager.getCurrentAudioDeviceType()
            + "\" buffer=" + juce::String (dev->getCurrentBufferSizeSamples())
            + " rate=" + juce::String (dev->getCurrentSampleRate(), 0));
    }
}
#endif

#if JUCE_WINDOWS || JUCE_MAC
// App aberto sem saida -- o Beelink de teste abriu assim, com o fone
// desplugado e o monitor sem caixa -- ficava mudo ate o musico abrir Audio e
// escolher de novo, mesmo depois de plugar o fone. O WASAPI/CoreAudio avisa o
// AudioDeviceManager quando a lista muda; aqui usamos a saida padrao do
// sistema. Uma tentativa por mudanca REAL da lista: se falhar, nao insiste em
// loop (cada setAudioDeviceSetup gera outra notificacao).
void MainComponent::reconnectAudioOutputIfAvailable()
{
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return;

    // A lista ja chega atualizada: o proprio tipo reescaneia antes de avisar.
    // Reescanear aqui (ASIO!) seria lento e desnecessario.
    const auto outputs = type->getDeviceNames (false);
    const auto fingerprint = type->getTypeName() + "|" + outputs.joinIntoString ("|");

    if (outputs.isEmpty() || fingerprint == lastAutoReconnectOutputs)
        return;

    lastAutoReconnectOutputs = fingerprint;

    // Primeira saida que este PC oferece: e aqui que o ajuste de latencia
    // da primeira abertura acontece de verdade (Low Latency/ASIO + ~10 ms).
    tuneDesktopFirstRunAudio();

    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        // Cabo da interface esbarrado no meio do culto e plugado de volta: a
        // saida certa e a ULTIMA que o musico escolheu, nao o padrao do Windows
        // (que pode ser o HDMI do monitor). Sem escolha conhecida, o padrao.
        juce::String preferredOutput;
        if (const auto lastChoice = deviceManager.createStateXml())
            preferredOutput = lastChoice->getStringAttribute ("audioOutputDeviceName");

        if (preferredOutput.isEmpty() || ! outputs.contains (preferredOutput))
        {
            const int defaultIndex = type->getDefaultDeviceIndex (false);
            preferredOutput = outputs[juce::isPositiveAndBelow (defaultIndex, outputs.size())
                                          ? defaultIndex : 0];
        }

        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = preferredOutput;
        setup.useDefaultOutputChannels = true;

        if (! audioInputAuthorised.load (std::memory_order_acquire))
        {
            setup.inputDeviceName.clear();
            setup.inputChannels.clear();
            setup.useDefaultInputChannels = false;
        }

        const auto error = deviceManager.setAudioDeviceSetup (setup, false);
        juce::Logger::writeToLog (
            "AUDIO_AUTORECONNECT output=\"" + setup.outputDeviceName + "\""
            + (error.isEmpty() ? juce::String (" ok") : " error=\"" + error + "\""));
    }

    if (deviceManager.getCurrentAudioDevice() != nullptr)
        lastAudioDriverError.clear();
}
#endif

#if JUCE_ANDROID
void MainComponent::updateAndroidPreferredUsbSetupFromCurrent()
{
    const auto setup = deviceManager.getAudioDeviceSetup();

    if (isAndroidUsbAudioDeviceName (setup.outputDeviceName))
    {
        androidPreferredUsbSetup = setup;
        androidHasPreferredUsbSetup = true;
    }
    else if (audioDialogOpen && deviceManager.getCurrentAudioDevice() != nullptr)
    {
        // System Default escolhido com o painel aberto e uma decisao do
        // usuario, nao o fallback causado por um cabo removido.
        androidHasPreferredUsbSetup = false;
    }
}

void MainComponent::pollAndroidAudioTopology()
{
    const auto generation = getAndroidAudioTopologyGeneration();

    if (generation <= 0 || generation == androidAudioTopologyGeneration)
        return;

    androidAudioTopologyGeneration = generation;
    androidPendingAudioTopologyGeneration = generation;

    if (androidAudioReconcileScheduled)
        return;

    androidAudioReconcileScheduled = true;
    juce::Component::SafePointer<MainComponent> safe (this);

    // Attach/detach costuma chegar em pequenos lotes (entrada + saida). Uma
    // janela curta transforma todos eles em um unico fechamento/restart.
    juce::Timer::callAfterDelay (250, [safe]
    {
        if (safe != nullptr && safe->androidAudioReconcileScheduled)
            safe->reconcileAndroidAudioTopology();
    });
}

void MainComponent::reconcileAndroidAudioTopology()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    androidAudioReconcileScheduled = false;
    const auto generation = androidPendingAudioTopologyGeneration;
    const auto usbFingerprint = getAndroidUsbAudioTopologyFingerprint();
    const bool forceReconcile = androidForceAudioReconcile;
    androidForceAudioReconcile = false;

    if (androidUsbAudioTopologyInitialized
         && usbFingerprint == androidLastUsbAudioTopology
         && ! forceReconcile)
    {
        juce::Logger::writeToLog (
            "AUDIO_ANDROID topology_unchanged generation=" + juce::String (generation)
            + " usb=" + usbFingerprint);
        return;
    }

    const bool firstTopologySnapshot = ! androidUsbAudioTopologyInitialized;
    androidUsbAudioTopologyInitialized = true;
    androidLastUsbAudioTopology = usbFingerprint;

    // initialize/onResume geram uma leitura mesmo sem cabo. Nesse caso o
    // cache inicial do Oboe ja contem System Default; nao interrompa o som so
    // para confirmar que continua sem USB.
    if (firstTopologySnapshot && usbFingerprint == "usb=none" && ! forceReconcile)
    {
        juce::Logger::writeToLog (
            "AUDIO_ANDROID topology_initialized generation=" + juce::String (generation)
            + " usb=none restart=skipped");
        return;
    }

    const auto snapshot = getAndroidAudioTopologySnapshot();
    const auto setupBefore = deviceManager.getAudioDeviceSetup();

    if (isAndroidUsbAudioDeviceName (setupBefore.outputDeviceName))
    {
        androidPreferredUsbSetup = setupBefore;
        androidHasPreferredUsbSetup = true;
    }

    juce::Logger::writeToLog (
        "AUDIO_ANDROID topology_change generation=" + juce::String (generation)
        + " requested_output=\"" + setupBefore.outputDeviceName + "\" devices=" + snapshot);

    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr)
    {
        juce::Logger::writeToLog (
            "AUDIO_ANDROID open_failed generation=" + juce::String (generation)
            + " reason=no_device_type");
        return;
    }

    // Fechar antes do scan impede que o listener sincrono do JUCE tente uma
    // reabertura paralela. Depois ha exatamente uma tentativa escolhida aqui.
    deviceManager.closeAudioDevice();
    type->scanForDevices();

    const auto outputNames = type->getDeviceNames (false);
    const auto isAvailable = [&outputNames] (const juce::String& name)
    {
        return name.isNotEmpty() && outputNames.contains (name);
    };

    auto setupToOpen = setupBefore;
    juce::String selectionReason { "current" };

    if (androidHasPreferredUsbSetup
         && isAvailable (androidPreferredUsbSetup.outputDeviceName))
    {
        setupToOpen = androidPreferredUsbSetup;
        selectionReason = "usb_preferred";
    }
    else if (! isAvailable (setupToOpen.outputDeviceName))
    {
        const int defaultIndex = type->getDefaultDeviceIndex (false);

        if (juce::isPositiveAndBelow (defaultIndex, outputNames.size()))
        {
            setupToOpen.outputDeviceName = outputNames[defaultIndex];
            selectionReason = androidHasPreferredUsbSetup
                                ? juce::String ("usb_missing_fallback")
                                : juce::String ("current_missing_fallback");
        }
        else
        {
            setupToOpen.outputDeviceName.clear();
            selectionReason = "no_output_available";
        }
    }

    // ChurchDAW e sampler: nenhuma reconexao de saida deve habilitar entrada
    // ou introduzir RECORD_AUDIO por acidente.
    setupToOpen.inputDeviceName.clear();
    setupToOpen.inputChannels.clear();
    setupToOpen.useDefaultInputChannels = false;

    juce::String error;
    if (setupToOpen.outputDeviceName.isNotEmpty())
        error = deviceManager.setAudioDeviceSetup (setupToOpen, false);
    else
        error = "nenhuma saida de audio enumerada";

    // Se a interface ainda aparece mas a abertura explicita falha (energia,
    // HAL ou rate incompatível), mantenha som pela rota padrao sem apagar a
    // preferencia USB. Um novo callback tentara restaura-la.
    if (error.isNotEmpty() && ! outputNames.isEmpty())
    {
        const int defaultIndex = type->getDefaultDeviceIndex (false);

        if (juce::isPositiveAndBelow (defaultIndex, outputNames.size())
             && setupToOpen.outputDeviceName != outputNames[defaultIndex])
        {
            juce::Logger::writeToLog (
                "AUDIO_ANDROID open_failed generation=" + juce::String (generation)
                + " output=\"" + setupToOpen.outputDeviceName
                + "\" error=\"" + error + "\" fallback=system_default");

            auto fallback = setupBefore;
            fallback.outputDeviceName = outputNames[defaultIndex];
            fallback.inputDeviceName.clear();
            fallback.inputChannels.clear();
            fallback.useDefaultInputChannels = false;
            error = deviceManager.setAudioDeviceSetup (fallback, false);
            selectionReason = "open_failed_fallback";
        }
    }

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        // A topologia acabou de mudar. Esta e a unica sondagem Oboe adicional
        // permitida para a nova rota; o tuner reutiliza o resultado e nunca
        // abre streams temporarios enquanto o musico esta tocando.
        androidAvailableBufferSizes = device->getAvailableBufferSizes();
        const auto activeOutputs = device->getActiveOutputChannels().countNumberOfSetBits();
        juce::Logger::writeToLog (
            "AUDIO_ANDROID open_succeeded generation=" + juce::String (generation)
            + " reason=" + selectionReason
            + " output=\"" + device->getName() + "\""
            + " sample_rate_actual=" + juce::String (device->getCurrentSampleRate(), 0)
            + " buffer_actual=" + juce::String (device->getCurrentBufferSizeSamples())
            + " output_channels=" + juce::String (activeOutputs));

        androidLastXRunCount = juce::jmax (0, device->getXRunCount());
        androidLastBufferTuneMs = juce::Time::getMillisecondCounter();
        androidXRunPressure = 0;
        androidStableBufferWindows = 0;
    }
    else
    {
        juce::Logger::writeToLog (
            "AUDIO_ANDROID open_failed generation=" + juce::String (generation)
            + " reason=" + selectionReason + " error=\"" + error + "\"");
    }
}
#endif

bool MainComponent::handleBackButton()
{
    if (auto* window = dynamic_cast<churchstudio::Window*> (liveCamWindow.getComponent()))
    {
        window->closeButtonPressed();
        return true;
    }
    // Ordem: a camada mais "por cima" primeiro. Cada fechamento usa o
    // caminho normal do painel (o destrutor do Audio/MIDI salva a config;
    // o reset das Bibliotecas encerra rede antes de desmontar).
    if (audioPanel != nullptr)
    {
        showAudioSettings();   // toggle: fecha salvando
        return true;
    }

    if (librariesPanel != nullptr)
    {
        librariesPanel.reset();
        return true;
    }

    // O Pad NAO e criado sob demanda: nasce no construtor e vive escondido a
    // sessao inteira. Antes este ramo testava a EXISTENCIA do ponteiro (sempre
    // verdadeira) e o destrua em todo BACK — matando o Pad Continuo para sempre
    // e engolindo o gesto das camadas abaixo (MIDI Learn, orgao, Live). So
    // fecha quem esta visivel, pelo caminho normal (sem destruir), e dispara
    // onClose para desarmar o alvo de nota e o pendingNoteAction do mapeamento.
    if (padPanel != nullptr && padPanel->isVisible())
    {
        padPanel->setVisible (false);
        padButton.setToggleState (padEditorVisible,
                                  juce::dontSendNotification);
        if (padPanel->onClose != nullptr)
            padPanel->onClose();
        return true;
    }

    if (padEditorVisible)
    {
        closePadEditor();
        return true;
    }

    if (midiLearnActive.load() || mapButton.getToggleState())
    {
        // Fecha o MIDI Learn no BACK (mesmo gesto que Concluir).
        finishMidiLearn (true);
        return true;
    }

    if (auto* timbres = dynamic_cast<TimbresEditor*> (dockedEditor.get()))
        if (timbres->handleBackRequest())
            return true;

    // LIVE MODE fecha no BACK, como toda camada do app.
    //
    // Era a unica que nao fechava: o BACK devolvia false, o primeiro toque era
    // engolido para armar os 2 s do "toque de novo para sair", e como nada
    // acontecia na tela o musico tocava de novo — e o app fechava no meio do
    // culto. Um painel aberto que ignora o BACK convida exatamente ao segundo
    // toque que mata o aplicativo.
    if (liveButton.getToggleState())
    {
        liveButton.setToggleState (false, juce::sendNotificationSync);
        return true;
    }

    return false;
}

void MainComponent::showAudioSettings()
{
    // Bibliotecas e modal de verdade: nao empilha configuracao por atalho,
    // automacao ou clique que tenha sido enfileirado antes de o scrim aparecer.
    if (librariesPanel != nullptr)
        return;

    // toggle: se ja esta aberto, fecha (o destrutor salva a config)
    if (audioPanel != nullptr)
    {
        audioPanel.reset();
        audioSettingsButton.repaint();
        return;
    }

   #if JUCE_ANDROID
    // Central full-screen com abas (AUDIO / LATENCIA / MIDI-USB).
    AndroidAudioMidiCenter::Host host {
        deviceManager,
        appProperties,
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* c = safe.getComponent())
            {
                c->audioPanel.reset();
                c->audioSettingsButton.repaint();
            }
        },
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* c = safe.getComponent())
                c->restartAudioDevice();
        },
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* c = safe.getComponent())
                c->refreshMidiInputs();
        },
        [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* c = safe.getComponent())
            {
                c->androidForceAudioReconcile = true;
                c->reconcileAndroidAudioTopology();
            }
        },
        [safe = juce::Component::SafePointer<MainComponent> (this)]() -> TimbresProcessor*
        {
            return safe != nullptr ? safe->getChurchLive() : nullptr;
        },
        [safe = juce::Component::SafePointer<MainComponent> (this)] (bool enabled)
        {
            if (auto* c = safe.getComponent())
                c->setLaunchkeyMappingEnabled (enabled);
        },
        launchkeyMappingEnabled,
        audioDialogOpen
    };
    host.startOutputTest = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* owner = safe.getComponent()) owner->startDiagnosticOutputTest();
    };
    host.stopOutputTest = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* owner = safe.getComponent()) owner->stopDiagnosticOutputTest();
    };
    auto* center = new AndroidAudioMidiCenter (std::move (host));
    audioPanel.reset (center);
    audioSettingsButton.repaint();
    addAndMakeVisible (center);
    // Respeita safeAreaInsets para nao entrar sob camera/barra gestual.
    center->setBounds (layoutContentBounds.isEmpty() ? getLocalBounds()
                                                     : layoutContentBounds);
    center->toFront (true);
    center->grabKeyboardFocus();
   #else
    auto* holder = new AudioSettingsHolder (*this);
    audioPanel.reset (holder);
    audioSettingsButton.repaint();
    addAndMakeVisible (holder);

   #if JUCE_IOS
    holder->setBounds (layoutContentBounds.isEmpty() ? getLocalBounds()
                                                     : layoutContentBounds);
   #else
    holder->setBounds (getLocalBounds());
   #endif
    holder->toFront (true);
    holder->grabKeyboardFocus();
   #endif
}

//==============================================================================
// Marca do header = botao de "Sobre". Nao gasta espaco na fileira de botoes e
// e o lugar onde todo mundo procura os creditos.
juce::Rectangle<int> MainComponent::brandArea() const
{
    if (desktopReferenceLayout)
        return DesktopHeaderLayout::calculate (layoutContentBounds).brand;

    return { layoutContentBounds.getX() + 6, layoutContentBounds.getY() + 2,
             juce::jmax (1, layoutBrandWidth - 4),
             juce::jmax (1, (mobileTwoRowHeader ? 48 : layoutHeaderHeight) - 4) };
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // Eventos dos filhos chegam aqui somente por causa do listener recursivo do
    // modo rapido. Nao execute os hit-tests do shell com coordenadas locais de
    // um Slider: procure a tag do parametro no controle ou em seus pais.
    if (e.eventComponent != this)
    {
        if (e.eventComponent == &mapButton || mapButton.isParentOf (e.eventComponent))
        {
            // Button::onClick owns this transition. Finishing on mouseDown
            // made mouseUp run onClick again and reopen learn mode.
            return;
        }

        if (e.eventComponent == &clearMapButton || clearMapButton.isParentOf (e.eventComponent))
            return;

        if (midiLearnActive.load())
        {
            juce::AudioProcessor* processor = nullptr;
            int parameterIndex = -1;
            int actionKind = 0;
            int actionArg = -1;
            juce::String actionDisplayName;

            for (auto* component = e.eventComponent;
                 component != nullptr && component != this;
                 component = component->getParentComponent())
            {
                if (ChurchDAWMidiLearnTarget::read (*component, processor, parameterIndex))
                {
                    const auto name = ChurchDAWMidiLearnTarget::readDisplayName (*component);
                    armMidiLearnParameter (processor, parameterIndex, component, name);
                    return;
                }

                if (ChurchDAWMidiLearnTarget::readAction (*component, actionKind, actionArg, actionDisplayName))
                {
                    armMidiLearnAction (actionKind, actionArg, actionDisplayName, component);
                    return;
                }
            }
        }

        return;
    }

    if (desktopReferenceLayout && midiLearnActive.load())
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            if (desktopSceneBounds[i].contains (e.getPosition()))
            {
                const int sceneIdx = desktopSceneIndexAtRow[i];
                if (juce::isPositiveAndBelow (sceneIdx, ChurchDAWLive::numeroDeCenas))
                {
                    const auto name = "Cena " + juce::String (sceneIdx + 1);
                    const auto boundsInOverlay = midiLearnConsoleOverlay.getLocalArea (this, desktopSceneBounds[i]);
                    armMidiLearnAction ((int) NoteActionKind::cena, sceneIdx, name, nullptr, boundsInOverlay);
                    return;
                }
            }
        }

        if (desktopTapBounds.contains (e.getPosition()))
        {
            const auto boundsInOverlay = midiLearnConsoleOverlay.getLocalArea (this, desktopTapBounds);
            armMidiLearnAction ((int) NoteActionKind::tapTempo, -1, "Tap Tempo", nullptr, boundsInOverlay);
            return;
        }
    }

    if (desktopReferenceLayout)
    {
        if (desktopSceneAddBounds.contains (e.getPosition()))
        {
            int primeiraVazia = -1;
            for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
                if (! cenas[i].preenchida)
                {
                    primeiraVazia = i;
                    break;
                }

            if (primeiraVazia >= 0)
                (void) aplicarCena (primeiraVazia);
            else
                liveScenePanel.avisar ("TODAS AS CENAS JA ESTAO PREENCHIDAS");

            showDesktopWorkspace (DesktopWorkspace::session);
            return;
        }

        static constexpr int favouriteCategories[5] { 3, 3, 5, 5, 8 };
        for (int i = 0; i < 5; ++i)
        {
            if (desktopFavouriteBounds[i].contains (e.getPosition()))
            {
                openSidebarTimbreCategory (favouriteCategories[i]);
                return;
            }
        }

        if (desktopTapBounds.contains (e.getPosition()))
        {
            if (performanceSessionPanel != nullptr)
                performanceSessionPanel->triggerTapTempo();
            repaint (desktopHeaderExtrasBounds);
            return;
        }

        if (desktopSettingsBounds.contains (e.getPosition()))
        {
            showAudioSettings();
            return;
        }

        if (desktopMixerTabBounds.contains (e.getPosition()))
        {
            showDesktopWorkspace (DesktopWorkspace::mixer);
            return;
        }

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            if (desktopSceneBounds[i].contains (e.getPosition()))
            {
                const int sceneIndex = desktopSceneIndexAtRow[i];
                if (juce::isPositiveAndBelow (sceneIndex,
                                               ChurchDAWLive::numeroDeCenas))
                {
                    const auto resultado = aplicarCena (sceneIndex);
                    if (resultado == ChurchDAWLive::Resultado::ok
                         || resultado == ChurchDAWLive::Resultado::vazia)
                        liveScenePanel.atualizar (cenas, cenaAtiva);
                    repaint (desktopSidebarBounds);
                }
                return;
            }
        }

        if (desktopResetBounds.contains (e.getPosition()))
        {
            pedirConfirmacaoParaZerarTimbres();
            return;
        }

        if (desktopSaveBounds.contains (e.getPosition()))
        {
            saveButton.triggerClick();
            return;
        }

        if (desktopAudioBounds.contains (e.getPosition()))
        {
            showAudioSettings();
            return;
        }
    }

    if (brandArea().contains (e.getPosition()))
        showAbout();
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const bool over = brandArea().contains (e.getPosition())
                   || desktopSceneAddBounds.contains (e.getPosition())
                   || desktopSaveBounds.contains (e.getPosition())
                   || desktopResetBounds.contains (e.getPosition())
                   || desktopAudioBounds.contains (e.getPosition())
                   || desktopTapBounds.contains (e.getPosition())
                   || desktopSettingsBounds.contains (e.getPosition())
                   || desktopMixerTabBounds.contains (e.getPosition())
                   || desktopPerformanceTabBounds.contains (e.getPosition())
                   || desktopSessionTabBounds.contains (e.getPosition());

    bool overFavourite = false;
    for (const auto& bounds : desktopFavouriteBounds)
        overFavourite = overFavourite || bounds.contains (e.getPosition());

    bool overScene = false;
    for (const auto& bounds : desktopSceneBounds)
        overScene = overScene || bounds.contains (e.getPosition());

    if ((over || overFavourite || overScene) != brandHover)
    {
        brandHover = over || overFavourite || overScene;
        setMouseCursor (brandHover ? juce::MouseCursor::PointingHandCursor
                                   : juce::MouseCursor::NormalCursor);
        repaint (brandArea());
    }
}

void MainComponent::mouseExit (const juce::MouseEvent&)
{
    if (brandHover)
    {
        brandHover = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (brandArea());
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (auto* focus = juce::Component::getCurrentlyFocusedComponent())
    {
        if (dynamic_cast<juce::TextEditor*> (focus) != nullptr)
            return false;
    }

    // Stage shortcuts must not change the scene while selecting a MIDI target.
    if (midiLearnActive.load())
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        {
            finishMidiLearn (true);
            return true;
        }
        return false;
    }

    const int code = key.getKeyCode();

    // 1 a 8: Disparo instantaneo das cenas 1 a 8 no palco
    if (code >= '1' && code <= '8')
    {
        const int sceneIdx = code - '1';
        (void) aplicarCena (sceneIdx);
        return true;
    }

    // Setas esquerda/direita: Navegacao rapida entre cenas
    if (key.isKeyCode (juce::KeyPress::leftKey))
    {
        const int previous = cenaAtiva < 0 ? 0 : juce::jmax (0, cenaAtiva - 1);
        (void) aplicarCena (previous);
        return true;
    }
    if (key.isKeyCode (juce::KeyPress::rightKey))
    {
        const int next = cenaAtiva < 0 ? 0 : juce::jmin (ChurchDAWLive::numeroDeCenas - 1, cenaAtiva + 1);
        (void) aplicarCena (next);
        return true;
    }

    // Esc: Concluir MIDI Learn se ativo; caso contrario Panic global (corta todas as vozes/notas presas instantaneamente)
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        if (mapButton.getToggleState() || midiLearnActive.load())
        {
            finishMidiLearn (true);
            return true;
        }

        performGlobalPanic();
        return true;
    }

    // Espaço: Alternar reproducao continua do Pad
    if (key.isKeyCode (juce::KeyPress::spaceKey))
    {
        togglePadContinuous();
        return true;
    }

    return false;
}

void MainComponent::togglePadContinuous()
{
    if (padProcessor == nullptr)
        return;

    const int currentKey = padProcessor->getKey();
    if (currentKey >= 0)
    {
        padProcessor->setKey (-1);
    }
    else
    {
        int keyToPlay = 0;
        if (juce::isPositiveAndBelow (cenaAtiva, ChurchDAWLive::numeroDeCenas)
            && cenas[cenaAtiva].preenchida && cenas[cenaAtiva].padTom >= 0)
        {
            keyToPlay = cenas[cenaAtiva].padTom;
        }
        padProcessor->setKey (keyToPlay);
    }

    if (padPanel != nullptr)
        padPanel->refreshState();
}

void MainComponent::showAbout()
{
    if (librariesPanel != nullptr)
        return;

    if (aboutPanel != nullptr)   // clicar de novo fecha
    {
        aboutPanel.reset();
        return;
    }

    auto* p = new AboutPanel();
    aboutPanel.reset (p);

    // fechar de dentro do proprio onClick destruiria o componente com o handler
    // ainda na pilha — por isso o descarte e adiado
    p->onClose = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* c = safe.getComponent())
                c->aboutPanel.reset();
        });
    };

    addAndMakeVisible (p);

    // O card completo tem informacao e botoes no rodape. Em telefone deitado,
    // ele era maior que a tela e escondia justamente "WhatsApp" e "Fechar".
    const auto hostBounds = layoutContentBounds.isEmpty() ? getLocalBounds()
                                                          : layoutContentBounds;
    const float fitX = (float) juce::jmax (1, hostBounds.getWidth()  - 8) / (float) p->getWidth();
    const float fitY = (float) juce::jmax (1, hostBounds.getHeight() - 8) / (float) p->getHeight();
    const float panelScale = juce::jmin (1.0f, juce::jmin (fitX, fitY));

    if (panelScale < 0.999f)
        p->setTransform (juce::AffineTransform::scale (panelScale));

    const int visualW = juce::roundToInt ((float) p->getWidth()  * panelScale);
    const int visualH = juce::roundToInt ((float) p->getHeight() * panelScale);

    const int visualX = hostBounds.getX() + juce::jmax (2, (hostBounds.getWidth()  - visualW) / 2);
    const int visualY = hostBounds.getY() + juce::jmax (2, (hostBounds.getHeight() - visualH) / 2);
    p->setTopLeftPosition (juce::roundToInt ((float) visualX / panelScale),
                           juce::roundToInt ((float) visualY / panelScale));
    p->toFront (true);
}

//==============================================================================
void MainComponent::showLibraries()
{
    if (librariesPanel != nullptr)
        return;

    // Um modal por vez. Fechar o painel de audio tambem persiste a configuracao
    // pelo destrutor de AudioSettingsHolder.
    audioPanel.reset();
    audioSettingsButton.repaint();
    aboutPanel.reset();

    auto* panel = new LibraryCatalogPanel();
    librariesPanel.reset (panel);

    panel->onClose = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* component = safe.getComponent())
                component->librariesPanel.reset();
        });
    };

    panel->onLibrariesChanged = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* component = safe.getComponent())
            component->rescanInstalledLibraries();
    };

    addAndMakeVisible (panel);
   #if CHURCHDAW_MOBILE_SHELL
    panel->setBounds (layoutContentBounds.isEmpty() ? getLocalBounds()
                                                    : layoutContentBounds);
   #else
    panel->setBounds (getLocalBounds());
   #endif
    panel->grabKeyboardFocus();
    panel->toFront (true);
}

void MainComponent::rescanInstalledLibraries()
{
    for (int i = 0; i < numSlots; ++i)
        if (auto* processor = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
            processor->rescanLibrary();

    statusInfo.setText (juce::String::fromUTF8 ("Bibliotecas atualizadas"),
                        juce::dontSendNotification);
}

//==============================================================================
// Estado de carregamento consultado pela splash: ela so libera a janela quando
// as bibliotecas foram escaneadas E os samples dos canais terminaram de decodificar.
bool MainComponent::isStillLoading() const
{
    for (int i = 0; i < numSlots; ++i)
        if (auto* t = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
            if (t->isBusyLoading())
                return true;

    return false;
}

juce::String MainComponent::getLoadingDetail() const
{
    for (int i = 0; i < numSlots; ++i)
    {
        if (auto* t = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
        {
            if (! t->hasScannedLibrary())
                return juce::String::fromUTF8 ("Lendo as bibliotecas de samples...");

            if (t->isBusyLoading())
                return juce::String::fromUTF8 ("Carregando os timbres dos canais...");
        }
    }

    return juce::String::fromUTF8 ("Preparando \xc3\xa1udio e plugins...");
}

float MainComponent::getLoadingProgress() const
{
    for (int i = 0; i < numSlots; ++i)
    {
        if (auto* t = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
        {
            if (! t->hasScannedLibrary())
                return 0.5f;

            if (t->isBusyLoading())
                return 0.88f;
        }
    }

    return 1.0f;
}

//==============================================================================
#if CHURCHDAW_HOSTS_PLUGINS
// A lista de plugins do JUCE esconde o escaneamento dentro de "Opcoes...".
// Este envelope poe um botao "Escanear VST3" visivel no mesmo rodape.
struct PluginListHolder final : public juce::Component,
                                private juce::Timer
{
    struct PendingScan
    {
        juce::AudioPluginFormat* format = nullptr;
        juce::StringArray identifiers;
        bool explicitIdentifiers = false;
    };

    using LocationKind = ChurchDAWPluginScan::PluginScanLocationKind;
    using ScanLocation = ChurchDAWPluginScan::PluginScanLocation;

    class PluginManagerLookAndFeel final : public NordLookAndFeel
    {
    public:
        void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                             bool, bool) override
        {
            const bool sidebar = static_cast<bool> (button.getProperties()["pluginSidebar"]);
            auto area = button.getLocalBounds().reduced (sidebar ? 14 : 10, 2);
            const auto label = button.getButtonText();
            const auto colour = button.findColour (button.getToggleState()
                ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                    .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.40f);
            g.setColour (colour);
            g.setFont (juce::Font (juce::FontOptions (sidebar ? 13.0f : 13.5f)));
            if (sidebar && label.contains ("     "))
            {
                auto count = area.removeFromRight (28);
                g.drawText (label.upToFirstOccurrenceOf ("     ", false, false), area,
                            juce::Justification::centredLeft, true);
                g.setColour (colour.withMultipliedAlpha (0.62f));
                g.setFont (juce::Font (juce::FontOptions (11.5f)));
                g.drawText (label.fromFirstOccurrenceOf ("     ", false, false), count, juce::Justification::centredRight);
            }
            else
                g.drawFittedText (label, area, juce::Justification::centred, 1);
    }

        void positionComboBoxText (juce::ComboBox& box,
                                   juce::Label& label) override
        {
            label.setBounds (10, 1, box.getWidth() - 34, box.getHeight() - 2);
            label.setFont (juce::Font (juce::FontOptions (12.0f)));
        }
    };

    class FolderRow final : public juce::Component
    {
    public:
        std::function<void()> onSelect;
        std::function<void(bool)> onToggle;

        void setData (const ScanLocation& locationToUse, bool selectedToUse,
                      int pluginCountToUse)
        {
            location = locationToUse;
            selected = selectedToUse;
            pluginCount = pluginCountToUse;
            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.x < 42)
            {
                location.enabled = ! location.enabled;
                if (onToggle != nullptr) onToggle (location.enabled);
            }
            else if (onSelect != nullptr)
                onSelect();
        }

        void mouseEnter (const juce::MouseEvent&) override { hovered = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hovered = false; repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds().toFloat();
            if (selected || hovered)
            {
                g.setColour (selected ? juce::Colour (0xff33151c)
                                      : juce::Colour (0xff171b21));
                g.fillRect (area);
            }
            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.fillRect (area.getX(), area.getBottom() - 1.0f,
                        area.getWidth(), 1.0f);

            auto check = juce::Rectangle<float> (14.0f,
                area.getCentreY() - 8.0f, 16.0f, 16.0f);
            g.setColour (location.enabled ? juce::Colour (0xffe8294b)
                                          : juce::Colour (0xff101319));
            g.fillRoundedRectangle (check, 3.0f);
            g.setColour (location.enabled ? juce::Colour (0xffff6680)
                                          : juce::Colour (0xff3b414b));
            g.drawRoundedRectangle (check, 3.0f, 1.0f);
            if (location.enabled)
            {
                juce::Path tick;
                tick.startNewSubPath (check.getX() + 4.0f, check.getCentreY());
                tick.lineTo (check.getX() + 7.0f, check.getBottom() - 4.0f);
                tick.lineTo (check.getRight() - 3.0f, check.getY() + 4.0f);
                g.setColour (juce::Colours::white);
                g.strokePath (tick, juce::PathStrokeType (1.6f));
            }

            const auto folder = juce::Rectangle<float> (48.0f,
                area.getCentreY() - 9.0f, 20.0f, 16.0f);
            g.setColour (juce::Colour (0xffaeb4bd));
            g.drawRoundedRectangle (folder, 2.0f, 1.4f);
            g.drawLine (folder.getX() + 2.0f, folder.getY(),
                        folder.getX() + 8.0f, folder.getY(), 1.4f);

            g.setColour (juce::Colour (0xffe7e9ed));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawFittedText (location.folder.getFullPathName(),
                              78, 0, juce::jmax (20, getWidth() - 250), getHeight(),
                              juce::Justification::centredLeft, 1);

            const bool exists = location.folder.isDirectory();
            const auto dot = juce::Colour (exists ? 0xff44d477 : 0xfff1b648);
            g.setColour (dot);
            g.fillEllipse ((float) getWidth() - 185.0f,
                           area.getCentreY() - 3.0f, 6.0f, 6.0f);
            g.setColour (juce::Colour (0xffaeb4bd));
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            const auto status = exists ? (location.kind == LocationKind::system
                                               ? juce::String ("Padrao")
                                               : juce::String ("Personalizado"))
                                       : juce::String ("Nao encontrada");
            g.drawText (status, getWidth() - 171, 0, 112, getHeight(),
                        juce::Justification::centredLeft);
            g.setColour (juce::Colour (0xff737b86));
            g.drawText (juce::String (pluginCount), getWidth() - 54, 0, 42,
                        getHeight(), juce::Justification::centredRight);
        }

    private:
        ScanLocation location;
        bool selected = false, hovered = false;
        int pluginCount = 0;
    };

    struct PluginItem
    {
        juce::PluginDescription description;
        std::vector<juce::PluginDescription> variants;
        juce::String usage;
        juce::String errorIdentifier;
        bool error = false;
        bool favourite = false;
        bool active = false;

        juce::String key() const
        {
            if (error)
                return "error:" + errorIdentifier;

            return ChurchDAWPluginProduct::key (description);

        }

        juce::String displayName() const
        {
            if (! error)
                return description.descriptiveName.isNotEmpty()
                         ? description.descriptiveName
                         : description.name;

            const juce::File file (errorIdentifier);
            const auto fileName = file.getFileNameWithoutExtension();
            return fileName.isNotEmpty() ? fileName : errorIdentifier;
        }

        juce::String formatName() const
        {
            if (! error)
            {
                juce::StringArray formats;
                for (const auto& variant : variants)
                    formats.addIfNotAlreadyThere (variant.pluginFormatName == "AudioUnit" ? "AU" : variant.pluginFormatName);
                return formats.joinIntoString (" / ");
            }

            if (errorIdentifier.endsWithIgnoreCase (".dll")) return "VST2";
            if (errorIdentifier.containsIgnoreCase (".vst3")) return "VST3";
            if (errorIdentifier.containsIgnoreCase ("AudioUnit")) return "AU";
            return "Plugin";
        }

        juce::String categoryName() const
        {
            if (error) return "Falha na verificacao";
            if (description.isInstrument) return "Instrumento";
            return description.category.isNotEmpty() ? description.category
                                                      : juce::String ("Efeito");
        }
    };

    class PluginRow final : public juce::Component
    {
    public:
        PluginRow() { setWantsKeyboardFocus (true); }
        std::function<void()> onSelect;
        std::function<void()> onOpen;
        std::function<void()> onFavourite;
        std::function<void()> onMenu;

        void setData (const PluginItem& itemToUse, bool selectedToUse)
        {
            item = itemToUse;
            selected = selectedToUse;
            setTitle (item.displayName());
            repaint();
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.y < 38 && event.x >= getWidth() - 34)
            {
                if (onMenu != nullptr) onMenu();
                return;
            }
            if (event.y < 38 && event.x >= getWidth() - 68)
            {
                if (onFavourite != nullptr) onFavourite();
                return;
            }
            if (onSelect != nullptr) onSelect();
    }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            if (event.y >= 38 && ! item.error && onOpen != nullptr) onOpen();
        }
        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key.isKeyCode (juce::KeyPress::returnKey))
            {
                if (! item.error && onOpen != nullptr) onOpen();
                return true;
            }
            return false;
        }
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::button,
                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this]
                { if (onSelect != nullptr) onSelect(); }));
        }

        void mouseEnter (const juce::MouseEvent&) override
        {
            hovered = true;
            repaint();
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            hovered = false;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (selected ? juce::Colour (0xff262930) : hovered ? juce::Colour (0xff24272d) : juce::Colour (0xff1d2026));
            g.fillRoundedRectangle (bounds, 12.0f);
            auto art = bounds.reduced (10.0f, 0.0f).withTrimmedTop (31).withHeight (110);
            ChurchDAWArtwork::plugin (g, art, item.displayName(), item.description.isInstrument);
            g.setColour (hasKeyboardFocus (false) ? NordLookAndFeel::nordRedBright()
                                  : selected ? NordLookAndFeel::nordRed().withAlpha (0.60f)
                                             : juce::Colours::white.withAlpha (hovered ? 0.13f : 0.065f));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 12.0f, 1.0f);
            auto top = getLocalBounds().reduced (14, 8).removeFromTop (24);
            g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
            g.setColour (NordLookAndFeel::txtSecondary());
            g.drawText (item.formatName() == "AudioUnit" ? "AU" : item.formatName(), top.removeFromLeft (68), juce::Justification::centredLeft);
            g.setColour (item.favourite ? NordLookAndFeel::amberSolo() : NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (19.0f)));
            g.drawText (item.favourite ? juce::String::fromUTF8 ("★") : juce::String::fromUTF8 ("☆"),
                        getWidth() - 67, 7, 30, 24, juce::Justification::centred);
            g.setColour (NordLookAndFeel::txtSecondary());
            for (int i = -1; i <= 1; ++i) g.fillEllipse ((float) getWidth() - 21, 19.0f + i * 4, 2.0f, 2.0f);
            auto copy = getLocalBounds().reduced (15, 0).withTrimmedTop (147);
            g.setColour (NordLookAndFeel::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (16.0f)).boldened());
            g.drawText (item.displayName(), copy.removeFromTop (24), juce::Justification::centredLeft, true);
            g.setColour (NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            const auto maker = item.error ? juce::String::fromUTF8 (u8"Requer verificação") : item.description.manufacturerName;
            g.drawText (maker.isNotEmpty() ? maker : item.categoryName(), copy.removeFromTop (20), juce::Justification::centredLeft, true);
            if (item.error || item.active)
            {
                g.setColour (item.error ? NordLookAndFeel::redClip() : NordLookAndFeel::greenOk());
                g.fillEllipse ((float) getWidth() - 22, (float) getHeight() - 22, 5, 5);
            }
    }

    private:
        PluginItem item;
        bool selected = false;
        bool hovered = false;
    };

    class PluginPreviewWindow final : public juce::DocumentWindow
    {
    public:
        PluginPreviewWindow (const juce::String& title,
                             std::function<void()> close)
            : juce::DocumentWindow (title, juce::Colour (0xff0c1015),
                                    juce::DocumentWindow::closeButton),
              closeCallback (std::move (close))
        {
            setUsingNativeTitleBar (false);
            setResizable (true, true);
            setResizeLimits (360, 260, 1800, 1200);
        }

        void closeButtonPressed() override
        {
            // A janela de preview pode estar empilhada sobre o gerenciador,
            // que tambem e modal. Adie a destruicao para nao apagar `this`
            // enquanto o JUCE ainda esta processando o clique do titulo.
            auto callback = closeCallback;
            juce::MessageManager::callAsync (
                [callback = std::move (callback)]
                {
                    if (callback != nullptr) callback();
                });
        }

    private:
        std::function<void()> closeCallback;
    };

    PluginListHolder (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& kl,
                      const juce::File& deadMansPedal, juce::PropertiesFile* props,
                      juce::StringArray& quarantine,
                      std::function<juce::String(const juce::PluginDescription&)> isActive)
        // O 5o argumento (allowPluginsWhichRequireAsynchronousInstantiation)
        // NAO e so sobre AUv3: com ele em "false" o PluginListComponent usa
        // numThreads = 0 (juce_PluginListComponent.cpp:446) e a varredura roda
        // DENTRO do timerCallback, ou seja, na thread de mensagens. Cada plugin
        // problematico segurava a interface inteira pelos 90 s do prazo — app
        // "Nao Responde", barra parada e botao Cancelar inclicavel. Era o
        // "travou no Korg Vox" e o "AUVectorPanner a 0%" dos clientes.
        : formats (fm), knownPlugins (kl), deadMansPedalFile (deadMansPedal),
          propertiesToUse (props), persistentQuarantine (quarantine),
          pluginIsActive (std::move (isActive)),
          list (fm, kl, deadMansPedal, props, true)
    {
        // Uma thread so: os plugins ja sao abertos num processo separado, e
        // varias threads abrindo plugins ao mesmo tempo e receita de conflito.
        // Com pool != nullptr o shouldExit() do scanner passa a valer, entao
        // "Cancelar" finalmente interrompe de verdade.
        list.setNumberOfThreadsForScanning (1);
        list.setUseIntegratedScanProgress (true);

        list.setOptionsButtonText (juce::String::fromUTF8 ("Op\xc3\xa7\xc3\xb5" "es..."));
        list.getOptionsButton().onClick = [this] { showOptionsMenu(); };
        addChildComponent (list);

        rowsViewport.setViewedComponent (&rowsContainer, false);
        rowsViewport.setScrollBarsShown (true, false);
        rowsViewport.setScrollBarThickness (5);
        rowsViewport.setColour (juce::ScrollBar::thumbColourId,
                                juce::Colour (0xff7e2637));
        rowsViewport.setColour (juce::ScrollBar::trackColourId,
                                juce::Colour (0xff0d1015));
        addAndMakeVisible (rowsViewport);

        pluginViewport.setViewedComponent (&pluginRowsContainer, false);
        pluginViewport.setScrollBarsShown (true, false);
        pluginViewport.setScrollBarThickness (5);
        pluginViewport.setColour (juce::ScrollBar::thumbColourId,
                                  juce::Colour (0xff6d303d));
        pluginViewport.setColour (juce::ScrollBar::trackColourId,
                                  juce::Colour (0xff0c0f13));
        addAndMakeVisible (pluginViewport);

        styleButton (scanButton, true);
        scanButton.setButtonText ("Escanear plugins");
        scanButton.onClick = [this] { scanNow(); };
        addAndMakeVisible (scanButton);

        styleButton (cancelButton, false);
        cancelButton.setButtonText ("Fechar");
        cancelButton.onClick = [this]
        {
            if (scanInProgress || list.isScanning() || ! filaDeFormatos.isEmpty())
            {
                scanStatus = "Cancelando o escaneamento...";
                if (list.isScanning())
                    list.cancelScan();
                else
                {
                    candidateSearch.reset();
                    candidateSearchFormat = nullptr;
                    filaDeFormatos.clearQuick();
                    concluirLote (false);
                }
                repaint (footerBounds);
            }
            else if (onClose != nullptr)
                onClose();
        };
        addAndMakeVisible (cancelButton);

        styleButton (settingsButton, false);
        settingsButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9a\x99"));
        settingsButton.setTooltip (
            juce::String::fromUTF8 ("Configura\xc3\xa7\xc3\xb5" "es de plugins"));
        settingsButton.onClick = [this] { showOptionsMenu(); };
        addAndMakeVisible (settingsButton);

        styleButton (closeButton, false);
        closeButton.setButtonText ("X");
        closeButton.setTooltip ("Fechar plugins");
        closeButton.setTitle ("Fechar plugins");
        closeButton.onClick = [this]
        {
            if (list.isScanning())
                list.cancelScan();
            if (onClose != nullptr) onClose();
        };
        addAndMakeVisible (closeButton);

        styleButton (addFolderButton, false);
        addFolderButton.setButtonText ("+ Adicionar pasta");
        addFolderButton.onClick = [this] { chooseFolder (false); };
        addAndMakeVisible (addFolderButton);
       #if JUCE_IOS
        // AUv3 vem do registro do sistema, nao de pastas: no iPad a coluna
        // de locais de busca so confundia (spec do dono: tela limpa p/ AUv3).
        addFolderButton.setVisible (false);
       #endif

        styleButton (changeFolderButton, false);
        changeFolderButton.setButtonText ("ALTERAR...");
        changeFolderButton.onClick = [this] { chooseFolder (true); };
        addAndMakeVisible (changeFolderButton);

        styleButton (removeFolderButton, false);
        removeFolderButton.setButtonText ("REMOVER");
        removeFolderButton.onClick = [this] { removeSelectedLocation(); };
        addAndMakeVisible (removeFolderButton);

        for (auto* button : { &moveUpButton, &moveDownButton })
        {
            styleButton (*button, false);
            addAndMakeVisible (*button);
        }
        moveUpButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x91"));
        moveDownButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x93"));
        moveUpButton.onClick = [this] { moveSelectedLocation (-1); };
        moveDownButton.onClick = [this] { moveSelectedLocation (+1); };

        static const char* categoryNames[4] = {
            "Todos os locais", "Padrao do sistema",
            "Personalizado", "Downloads"
        };
        for (int i = 0; i < 4; ++i)
        {
            styleButton (categoryButtons[(size_t) i], false);
            categoryButtons[(size_t) i].setButtonText (categoryNames[i]);
            categoryButtons[(size_t) i].onClick = [this, i]
            {
                selectedCategory = i;
                rebuildRows();
            };
            addAndMakeVisible (categoryButtons[(size_t) i]);
        }

        static const char* pluginFilterNames[5] = {
            "Todos os plugins", "Instrumentos", "Efeitos",
            "Favoritos", "Com erro"
        };
        for (int i = 0; i < 5; ++i)
        {
            styleButton (pluginFilterButtons[(size_t) i], false);
            pluginFilterButtons[(size_t) i].setButtonText (pluginFilterNames[i]);
            pluginFilterButtons[(size_t) i].onClick = [this, i]
            {
                selectedPluginFilter = i;
                rebuildPluginRows();
            };
            addAndMakeVisible (pluginFilterButtons[(size_t) i]);
        }

        searchEditor.setTextToShowWhenEmpty (
            "Buscar plugin, fabricante ou categoria...",
            juce::Colour (0xff737b86));
        searchEditor.setColour (juce::TextEditor::backgroundColourId,
                                juce::Colour (0xff0d1116));
        searchEditor.setColour (juce::TextEditor::textColourId,
                                juce::Colour (0xffeef1f4));
        searchEditor.setColour (juce::TextEditor::outlineColourId,
                                juce::Colour (0xff2a3038));
        searchEditor.setColour (juce::TextEditor::focusedOutlineColourId,
                                juce::Colour (0xffb52643));
        searchEditor.setColour (juce::CaretComponent::caretColourId,
                                juce::Colour (0xffd8274e));
        searchEditor.setFont (juce::Font (juce::FontOptions (14.0f)));
        searchEditor.setIndents (14, 0);
        searchEditor.onTextChange = [this]
        {
            searchClearButton.setVisible (searchEditor.getText().isNotEmpty());
            rebuildPluginRows();
        };
        searchEditor.onReturnKey = [this]
        {
            if (openPluginButton.isEnabled()) openSelectedPlugin();
        };
        searchEditor.onEscapeKey = [this]
        {
            if (searchEditor.getText().isNotEmpty())
                searchEditor.setText ({}, true);
            else if (! scanInProgress && onClose != nullptr)
                onClose();
        };
        addAndMakeVisible (searchEditor);

        styleButton (searchClearButton, false);
        searchClearButton.setButtonText ("X");
        searchClearButton.setTooltip ("Limpar busca");
        searchClearButton.onClick = [this]
        {
            searchEditor.setText ({}, true);
            searchEditor.grabKeyboardFocus();
        };
        addAndMakeVisible (searchClearButton);
        searchClearButton.setVisible (false);

        sortBox.addItem ("Nome (A-Z)", 1);
        sortBox.addItem ("Fabricante", 2);
        sortBox.addItem ("Categoria", 3);
        sortBox.addItem ("Formato", 4);
        sortBox.addItem (juce::String::fromUTF8 (u8"Relevância"), 5);
        sortBox.setSelectedId (5, juce::dontSendNotification);
        sortBox.setColour (juce::ComboBox::backgroundColourId,
                           juce::Colour (0xff11161c));
        sortBox.setColour (juce::ComboBox::outlineColourId,
                           juce::Colour (0xff2a3038));
        sortBox.setColour (juce::ComboBox::textColourId,
                           juce::Colour (0xffd8dce2));
        sortBox.setColour (juce::ComboBox::arrowColourId,
                           juce::Colour (0xff8d96a1));
        sortBox.onChange = [this] { rebuildPluginRows(); };
        addAndMakeVisible (sortBox);

        styleButton (openPluginButton, true);
        openPluginButton.setButtonText ("Abrir plugin");
        openPluginButton.onClick = [this] { openSelectedPlugin(); };
        addAndMakeVisible (openPluginButton);
        formatChoice.setTitle ("Formato do plugin");
        formatChoice.setTooltip ("Escolha qual formato abrir. O plugin carregado no projeto permanece igual.");
        formatChoice.onChange = [this]
        {
            if (! juce::isPositiveAndBelow (selectedPluginIndex, (int) filteredPluginItems.size())) return;
            auto& item = filteredPluginItems[(size_t) selectedPluginIndex];
            const int index = formatChoice.getSelectedId() - 1;
            if (! juce::isPositiveAndBelow (index, (int) item.variants.size())) return;
            item.description = item.variants[(size_t) index];
            chosenPluginFormats[item.key()] = item.description.createIdentifierString();
            refreshPluginRows();
        };
        addAndMakeVisible (formatChoice);


        styleButton (revealPluginButton, false);
       #if JUCE_MAC
        revealPluginButton.setButtonText ("Mostrar no Finder");
       #else
        revealPluginButton.setButtonText ("Mostrar no Explorer");
       #endif
        revealPluginButton.onClick = [this] { revealSelectedPlugin(); };
        addAndMakeVisible (revealPluginButton);
       #if JUCE_IOS
        // Nao existe Finder/Explorer no iPad; o botao ficava eternamente
        // desabilitado ocupando lugar.
        revealPluginButton.setVisible (false);
       #endif

        styleButton (favouritePluginButton, false);
        favouritePluginButton.onClick = [this]
        {
            if (juce::isPositiveAndBelow (selectedPluginIndex,
                                           (int) filteredPluginItems.size()))
                toggleFavourite (filteredPluginItems[(size_t) selectedPluginIndex]);
        };
        addAndMakeVisible (favouritePluginButton);

        rowsViewport.setVisible (false);
        changeFolderButton.setVisible (false);
        removeFolderButton.setVisible (false);
        moveUpButton.setVisible (false);
        moveDownButton.setVisible (false);

        for (auto* button : { &scanButton, &cancelButton, &settingsButton,
                              &closeButton, &addFolderButton,
                              &changeFolderButton, &removeFolderButton,
                              &moveUpButton, &moveDownButton,
                              &openPluginButton, &revealPluginButton,
                              &favouritePluginButton, &searchClearButton })
            button->setLookAndFeel (&pluginManagerLookAndFeel);

        for (auto& button : categoryButtons)
        {
            button.setLookAndFeel (&pluginManagerLookAndFeel);
            button.getProperties().set ("pluginSidebar", true);
           #if JUCE_IOS
            // Locais de busca sao coisa de disco/pastas; AUv3 no iPad vem do
            // registro do sistema. O titulo "LOCAIS DE BUSCA" ja segue esta
            // visibilidade no paint().
            button.setVisible (false);
           #endif
        }
        for (auto& button : pluginFilterButtons)
        {
            button.setLookAndFeel (&pluginManagerLookAndFeel);
            button.getProperties().set ("pluginSidebar", true);
        }
        sortBox.setLookAndFeel (&pluginManagerLookAndFeel);

        setWantsKeyboardFocus (true);
        setSize (1240, 760);
        restoreFavourites();
        restoreLocations();
        rebuildRows();
    }

    ~PluginListHolder() override
    {
        closePluginPreview();

        // Fechar o dialogo/app durante uma prova nao pode abrir uma janela em
        // que o ID acabou de sair da blacklist mas ainda nao foi validado.
        if (revalidationBatchActive)
        {
            persistentQuarantine.addArray (revalidationTargets);
            normaliseQuarantine();
            // O ThreadPool de `list` so e cancelado na destruicao dos membros,
            // depois deste corpo. Persista a barreira agora, mas nao percorra
            // KnownPluginList enquanto o worker ainda pode altera-la.
            if (! persistQuarantineState (false))
                juce::Logger::writeToLog (
                    "PLUGIN_SCAN quarantine_persist_failed during=holder_shutdown");
        }
    }

    std::function<void()> onClose;

    static void styleButton (juce::TextButton& button, bool primary)
    {
        button.setColour (juce::TextButton::buttonColourId,
                          primary ? juce::Colour (0xff94172b)
                                  : juce::Colour (0xff171b21));
        button.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colour (0xffb31b36));
        button.setColour (juce::TextButton::textColourOffId,
                          primary ? juce::Colours::white
                                  : juce::Colour (0xffe5e7eb));
        button.setColour (juce::TextButton::textColourOnId,
                          juce::Colours::white);
    }

    void restoreLocations()
    {
        locations.clear();
        bool locationsChanged = false;
        const auto saved = propertiesToUse != nullptr
            ? propertiesToUse->getValue ("churchdawPluginScanLocations")
            : juce::String();

        if (saved.isNotEmpty())
        {
            juce::StringArray lines;
            lines.addLines (saved);
            for (const auto& line : lines)
            {
                const auto fields = juce::StringArray::fromTokens (line, "|", "");
                if (fields.size() < 3) continue;
                ScanLocation location;
                location.enabled = fields[0].getIntValue() != 0;
                location.kind = static_cast<LocationKind> (
                    juce::jlimit (0, 2, fields[1].getIntValue()));
                location.folder = juce::File (fields[2]);
                if (location.folder != juce::File{})
                {
                    const bool added =
                        ChurchDAWPluginScan::adicionarPastaDeScanSeAusente (
                            locations, location.folder, location.kind,
                            location.enabled);
                    locationsChanged |= ! added;
                }
            }
        }

        const int defaultLocationsAdded =
            ChurchDAWPluginScan::mesclarPastasPadraoDePlugins (
                locations, formats,
               #if JUCE_WINDOWS
                true
               #else
                false
               #endif
            );
        locationsChanged |= defaultLocationsAdded > 0;

        if (locationsChanged)
        {
            persistLocations();
            juce::Logger::writeToLog (
                "PLUGIN_SCAN locations_merged defaults_added="
                + juce::String (defaultLocationsAdded)
                + " total=" + juce::String (locations.size()));
        }

        selectedLocation = locations.isEmpty() ? -1 : 0;
        lastVerification = propertiesToUse != nullptr
            ? propertiesToUse->getValue ("churchdawPluginLastVerification")
            : juce::String();
    }

    void persistLocations()
    {
        if (propertiesToUse == nullptr) return;
        juce::StringArray lines;
        for (const auto& location : locations)
            lines.add (juce::String (location.enabled ? 1 : 0) + "|"
                       + juce::String ((int) location.kind) + "|"
                       + location.folder.getFullPathName());
        propertiesToUse->setValue ("churchdawPluginScanLocations",
                                   lines.joinIntoString ("\n"));
        propertiesToUse->saveIfNeeded();
    }

    void restoreFavourites()
    {
        favouritePluginKeys.clear();

        if (propertiesToUse != nullptr)
            favouritePluginKeys.addLines (
                propertiesToUse->getValue ("churchdawPluginFavourites"));

        favouritePluginKeys.removeEmptyStrings();
        favouritePluginKeys.removeDuplicates (false);
    }

    void persistFavourites()
    {
        if (propertiesToUse == nullptr) return;

        favouritePluginKeys.removeEmptyStrings();
        favouritePluginKeys.removeDuplicates (false);
        propertiesToUse->setValue ("churchdawPluginFavourites",
                                   favouritePluginKeys.joinIntoString ("\n"));
        propertiesToUse->saveIfNeeded();
    }

    bool pluginMatchesSelectedLocation (const PluginItem& item) const
    {

        if (selectedCategory == 0) return true;
        const auto wanted = static_cast<LocationKind> (selectedCategory - 1);
        const auto matches = [&] (const juce::String& identifier, const juce::String& format)
        {
            if (format.containsIgnoreCase ("AudioUnit")) return wanted == LocationKind::system;
            const juce::File file (identifier);
            for (const auto& location : locations)
                if (location.kind == wanted && (file == location.folder || file.isAChildOf (location.folder))) return true;
            return false;
        };
        if (item.error) return matches (item.errorIdentifier, {});
        for (const auto& variant : item.variants)
            if (matches (variant.fileOrIdentifier, variant.pluginFormatName)) return true;
        return false;
    }

    bool pluginMatchesSearch (const PluginItem& item) const
    {
        const auto query = searchEditor.getText().trim();
        if (query.isEmpty()) return true;

        const auto& d = item.description;
        return item.displayName().containsIgnoreCase (query)
            || d.name.containsIgnoreCase (query)
            || d.manufacturerName.containsIgnoreCase (query)
            || item.categoryName().containsIgnoreCase (query)
            || item.formatName().containsIgnoreCase (query)
            || (item.error ? item.errorIdentifier : d.fileOrIdentifier)
                   .containsIgnoreCase (query);
    }

    bool pluginMatchesFilter (const PluginItem& item) const
    {
        if (! pluginMatchesSelectedLocation (item)
             || ! pluginMatchesSearch (item))
            return false;

        switch (selectedPluginFilter)
        {
            case 1: return ! item.error && item.description.isInstrument;
            case 2: return ! item.error && ! item.description.isInstrument;
            case 3: return ! item.error && item.favourite;
            case 4: return item.error;
            default: return true;
        }
    }

    void updatePluginFilterButtons()
    {
        int instruments = 0;
        int effects = 0;
        int favourites = 0;
        int errors = 0;

        for (const auto& item : allPluginItems)
        {
            if (item.error) ++errors;
            else if (item.description.isInstrument) ++instruments;
            else ++effects;

            if (item.favourite) ++favourites;
        }

        const int counts[5] = { (int) allPluginItems.size(), instruments,
                                effects, favourites, errors };
        static const char* names[5] = { "Todos os plugins", "Instrumentos",
                                        "Efeitos", "Favoritos", "Com erro" };

        for (int i = 0; i < 5; ++i)
        {
            auto& button = pluginFilterButtons[(size_t) i];
            button.setButtonText (juce::String (names[i]) + "     "
                                  + juce::String (counts[i]));
            const bool selected = i == selectedPluginFilter;
            button.setColour (juce::TextButton::buttonColourId,
                              selected ? juce::Colour (0xff3a1821)
                                       : juce::Colour (0xff14191f));
            button.setColour (juce::TextButton::textColourOffId,
                              selected ? juce::Colour (0xfff4f5f7)
                                       : juce::Colour (0xffa9b0b9));
        }
    }

    void layoutPluginRows()
    {
        const auto layout = ChurchDAWPluginGallery::forWidth (pluginViewport.getWidth() - 8);
        pluginRowsContainer.setSize (layout.width, juce::jmax (pluginViewport.getHeight(), layout.height ((int) pluginRows.size())));
        for (int i = 0; i < (int) pluginRows.size(); ++i)
            pluginRows[(size_t) i]->setBounds (layout.item (i));
    }

    void refreshPluginRows()
    {
        for (int i = 0; i < (int) pluginRows.size(); ++i)
            pluginRows[(size_t) i]->setData (
                filteredPluginItems[(size_t) i], i == selectedPluginIndex);

        const bool hasSelection = juce::isPositiveAndBelow (
            selectedPluginIndex, (int) filteredPluginItems.size());
        const auto* selected = hasSelection
                                 ? &filteredPluginItems[(size_t) selectedPluginIndex]
                                 : nullptr;
        openPluginButton.setEnabled (selected != nullptr && ! selected->error
                                     && ! scanInProgress && ! openingPlugin);
        const juce::File selectedFile (selected != nullptr
                                        ? (selected->error
                                             ? selected->errorIdentifier
                                             : selected->description.fileOrIdentifier)
                                        : juce::String());
        revealPluginButton.setEnabled (selected != nullptr
                                       && (selectedFile.existsAsFile()
                                            || selectedFile.isDirectory()));
        favouritePluginButton.setEnabled (selected != nullptr && ! selected->error);
        favouritePluginButton.setButtonText (
            selected != nullptr && selected->favourite
              ? juce::String::fromUTF8 ("\xe2\x98\x85  Favorito")
              : juce::String::fromUTF8 ("\xe2\x98\x86  Favoritar"));
        formatChoice.clear (juce::dontSendNotification);
        if (selected != nullptr && ! selected->error)
            for (int i = 0; i < (int) selected->variants.size(); ++i)
            {
                const auto& variant = selected->variants[(size_t) i];
                formatChoice.addItem ("Formato: " + variant.pluginFormatName, i + 1);
                if (variant.createIdentifierString() == selected->description.createIdentifierString())
                    formatChoice.setSelectedId (i + 1, juce::dontSendNotification);
            }
        formatChoice.setEnabled (selected != nullptr && ! selected->error && ! scanInProgress);
        repaint (infoBounds);
    }

    void rebuildPluginRows()
    {
        juce::String selectedKey;
        if (juce::isPositiveAndBelow (selectedPluginIndex,
                                       (int) filteredPluginItems.size()))
            selectedKey = filteredPluginItems[(size_t) selectedPluginIndex].key();

        allPluginItems.clear();
        for (const auto& description : knownPlugins.getTypes())
        {
            const auto key = ChurchDAWPluginProduct::key (description);
            auto existing = std::find_if (allPluginItems.begin(), allPluginItems.end(),
                [&key] (const PluginItem& item) { return item.key() == key; });
            if (existing == allPluginItems.end())
            {
                PluginItem item;
                item.description = description;
                allPluginItems.push_back (std::move (item));
                existing = std::prev (allPluginItems.end());
            }
            auto& item = *existing;
            item.variants.push_back (description);
            item.favourite |= favouritePluginKeys.contains (description.createIdentifierString(), false);
            const auto usage = pluginIsActive != nullptr ? pluginIsActive (description) : juce::String();
            if (usage.isNotEmpty())
            {
                if (item.usage.isNotEmpty()) item.usage += " / ";
                item.usage += usage;
                item.description = description;
                item.active = true;
            }
            else if (! item.active && description.pluginFormatName == "VST3")
                item.description = description;
        }
        for (auto& item : allPluginItems)
        {
            const auto chosen = chosenPluginFormats.find (item.key());
            if (chosen != chosenPluginFormats.end())
                for (const auto& variant : item.variants)
                    if (variant.createIdentifierString() == chosen->second)
                        item.description = variant;
        }

        auto disabled = persistentQuarantine;
        disabled.addArray (knownPlugins.getBlacklistedFiles());
        disabled.removeEmptyStrings();
        disabled.removeDuplicates (true);

        for (const auto& identifier : disabled)
        {
            bool alreadyListed = false;
            for (const auto& item : allPluginItems)
                for (const auto& variant : item.variants)
                    alreadyListed |= variant.fileOrIdentifier.equalsIgnoreCase (identifier);

            if (! alreadyListed)
            {
                PluginItem item;
                item.error = true;
                item.errorIdentifier = identifier;
                item.description.name = juce::File (identifier)
                                            .getFileNameWithoutExtension();
                item.description.fileOrIdentifier = identifier;
                allPluginItems.push_back (std::move (item));
            }
        }

        const int sortMode = sortBox.getSelectedId();
        std::stable_sort (allPluginItems.begin(), allPluginItems.end(),
            [sortMode] (const PluginItem& a, const PluginItem& b)
            {
                if (a.error != b.error) return ! a.error;

                if (sortMode == 5)
                {
                    const auto priority = [] (const PluginItem& item)
                    {
                        if (item.displayName().containsIgnoreCase ("bigsky")) return 0;
                        if (item.displayName().containsIgnoreCase ("pianoteq")) return 1;
                        if (item.displayName().containsIgnoreCase ("keyscape")) return 2;
                        if (item.favourite) return 3;
                        if (item.description.manufacturerName.containsIgnoreCase ("apple")) return 10;
                        return item.description.isInstrument ? 4 : 5;
                    };
                    if (priority (a) != priority (b)) return priority (a) < priority (b);
                }
                juce::String left, right;
                switch (sortMode)
                {
                    case 2:
                        left = a.description.manufacturerName;
                        right = b.description.manufacturerName;
                        break;
                    case 3:
                        left = a.categoryName();
                        right = b.categoryName();
                        break;
                    case 4:
                        left = a.formatName();
                        right = b.formatName();
                        break;
                    default:
                        left = a.displayName();
                        right = b.displayName();
                        break;
                }

                const int compared = left.compareNatural (right, true);
                return compared == 0
                         ? a.displayName().compareNatural (b.displayName(), true) < 0
                         : compared < 0;
            });

        filteredPluginItems.clear();
        for (const auto& item : allPluginItems)
            if (pluginMatchesFilter (item))
                filteredPluginItems.push_back (item);

        selectedPluginIndex = -1;
        for (int i = 0; i < (int) filteredPluginItems.size(); ++i)
            if (filteredPluginItems[(size_t) i].key() == selectedKey)
            {
                selectedPluginIndex = i;
                break;
            }

        if (selectedPluginIndex < 0 && ! filteredPluginItems.empty())
            selectedPluginIndex = 0;

        for (auto& row : pluginRows)
            pluginRowsContainer.removeChildComponent (row.get());
        pluginRows.clear();

        for (int i = 0; i < (int) filteredPluginItems.size(); ++i)
        {
            auto row = std::make_unique<PluginRow>();
            row->onSelect = [this, i]
            {
                selectedPluginIndex = i;
                refreshPluginRows();
            };
            row->onOpen = [this, i]
            {
                selectedPluginIndex = i;
                refreshPluginRows();
                openSelectedPlugin();
            };
            row->onFavourite = [this, i]
            {
                if (juce::isPositiveAndBelow (i,
                                               (int) filteredPluginItems.size()))
                    toggleFavourite (filteredPluginItems[(size_t) i]);
            };
            row->onMenu = [this, i] { showPluginMenu (i); };
            row->setData (filteredPluginItems[(size_t) i],
                          i == selectedPluginIndex);
            pluginRowsContainer.addAndMakeVisible (*row);
            pluginRows.push_back (std::move (row));
        }

        updatePluginFilterButtons();
        layoutPluginRows();
        refreshPluginRows();
        repaint();
    }

    void toggleFavourite (const PluginItem& item)
    {
        if (item.error) return;

        for (const auto& variant : item.variants)
        {
            const auto key = variant.createIdentifierString();
            if (item.favourite) favouritePluginKeys.removeString (key, false);
            else favouritePluginKeys.addIfNotAlreadyThere (key);
        }

        persistFavourites();
        rebuildPluginRows();
    }

    void disablePlugin (const PluginItem& item)
    {
        if (item.error) return;

        const auto identifier = item.description.fileOrIdentifier;
        if (identifier.isEmpty()) return;

        persistentQuarantine.addIfNotAlreadyThere (identifier);
        knownPlugins.addToBlacklist (identifier);
        knownPlugins.removeType (item.description);
        (void) persistQuarantineState();
        rebuildPluginRows();
    }

    void showPluginMenu (int index)
    {
        if (! juce::isPositiveAndBelow (index,
                                         (int) filteredPluginItems.size()))
            return;

        const auto item = filteredPluginItems[(size_t) index];
        selectedPluginIndex = index;
        refreshPluginRows();

        juce::PopupMenu menu;
        if (item.error)
        {
            menu.addItem ("Revalidar este plugin", ! scanInProgress, false,
                          [this, identifier = item.errorIdentifier]
                          {
                              revalidateDisabledPlugins ({ identifier });
                          });
        }
        else
        {
            menu.addItem (item.favourite ? "Remover dos favoritos"
                                         : "Adicionar aos favoritos",
                          [this, item] { toggleFavourite (item); });
            menu.addItem ("Mostrar arquivo", [this] { revealSelectedPlugin(); });
            menu.addSeparator();
            menu.addItem ("Desativar com seguranca", ! scanInProgress, false,
                          [this, item] { disablePlugin (item); });
        }

        menu.showMenuAsync (
            juce::PopupMenu::Options().withDeletionCheck (*this));
    }

    bool categoryMatches (const ScanLocation& location) const noexcept
    {
        if (selectedCategory == 0) return true;
        if (selectedCategory == 1) return location.kind == LocationKind::system;
        if (selectedCategory == 2) return location.kind == LocationKind::custom;
        return location.kind == LocationKind::downloads;
    }

    int pluginCountFor (const juce::File& folder) const
    {
        int count = 0;
        for (const auto& plugin : knownPlugins.getTypes())
        {
            const juce::File file (plugin.fileOrIdentifier);
            if (file == folder || file.isAChildOf (folder)) ++count;
        }
        return count;
    }

    void rebuildRows()
    {
        for (auto& row : rows) rowsContainer.removeChildComponent (row.get());
        rows.clear();
        visibleLocationIndices.clear();

        for (int i = 0; i < locations.size(); ++i)
        {
            if (! categoryMatches (locations.getReference (i))) continue;
            auto row = std::make_unique<FolderRow>();
            row->onSelect = [this, i]
            {
                selectedLocation = i;
                refreshRows();
            };
            row->onToggle = [this, i] (bool enabled)
            {
                locations.getReference (i).enabled = enabled;
                persistLocations();
                updateCategoryButtons();
                refreshRows();
            };
            row->setData (locations.getReference (i), i == selectedLocation,
                          pluginCountFor (locations.getReference (i).folder));
            rowsContainer.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
            visibleLocationIndices.add (i);
        }

        updateCategoryButtons();
        rebuildPluginRows();
        resized();
        repaint();
    }

    void refreshRows()
    {
        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const int locationIndex = visibleLocationIndices[i];
            if (! juce::isPositiveAndBelow (locationIndex, locations.size()))
                continue;

            const auto& location = locations.getReference (locationIndex);
            rows[(size_t) i]->setData (location,
                                       locationIndex == selectedLocation,
                                       pluginCountFor (location.folder));
        }

        const bool hasSelection = juce::isPositiveAndBelow (selectedLocation,
                                                             locations.size());
        changeFolderButton.setEnabled (hasSelection && ! scanInProgress);
        removeFolderButton.setEnabled (hasSelection && ! scanInProgress);
        moveUpButton.setEnabled (hasSelection && selectedLocation > 0
                                 && ! scanInProgress);
        moveDownButton.setEnabled (hasSelection
                                   && selectedLocation + 1 < locations.size()
                                   && ! scanInProgress);
        repaint (infoBounds);
    }

    void updateCategoryButtons()
    {
        int counts[4] = { locations.size(), 0, 0, 0 };
        for (const auto& location : locations)
            ++counts[1 + (int) location.kind];
        static const char* names[4] = {
            "Todos os locais", "Padrao do sistema",
            "Personalizado", "Downloads"
        };
        for (int i = 0; i < 4; ++i)
        {
            categoryButtons[(size_t) i].setButtonText (
                juce::String (names[i]) + "     " + juce::String (counts[i]));
            categoryButtons[(size_t) i].setColour (
                juce::TextButton::buttonColourId,
                i == selectedCategory ? juce::Colour (0xff78182a)
                                      : juce::Colour (0xff15191f));
            categoryButtons[(size_t) i].setColour (
                juce::TextButton::textColourOffId,
                i == selectedCategory ? juce::Colours::white
                                      : juce::Colour (0xffaeb4bd));
        }
    }

    void chooseFolder (bool replacing)
    {
        const auto initial = juce::isPositiveAndBelow (selectedLocation,
                                                        locations.size())
            ? locations.getReference (selectedLocation).folder
            : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        folderChooser = std::make_unique<juce::FileChooser> (
            replacing ? "Alterar pasta de plugins" : "Adicionar pasta de plugins",
            initial, "*");
        auto safe = juce::Component::SafePointer<PluginListHolder> (this);
        folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectDirectories,
            [safe, replacing] (const juce::FileChooser& chooser)
            {
                if (auto* self = safe.getComponent())
                {
                    const auto folder = chooser.getResult();
                    if (folder != juce::File{})
                    {
                        if (replacing && juce::isPositiveAndBelow (
                                self->selectedLocation, self->locations.size()))
                            self->locations.getReference (
                                self->selectedLocation).folder = folder;
                        else
                        {
                            bool duplicate = false;
                            for (const auto& location : self->locations)
                                duplicate |= location.folder == folder;
                            if (! duplicate)
                            {
                                self->locations.add ({ folder,
                                    LocationKind::custom, true });
                                self->selectedLocation = self->locations.size() - 1;
                            }
                        }
                        self->persistLocations();
                        self->rebuildRows();
                    }
                    self->folderChooser.reset();
                }
            });
    }

    void removeSelectedLocation()
    {
        if (! juce::isPositiveAndBelow (selectedLocation, locations.size())) return;
        locations.remove (selectedLocation);
        selectedLocation = locations.isEmpty()
                             ? -1 : juce::jmin (selectedLocation,
                                                locations.size() - 1);
        persistLocations();
        rebuildRows();
    }

    void moveSelectedLocation (int direction)
    {
        if (! juce::isPositiveAndBelow (selectedLocation, locations.size())) return;
        const int destination = selectedLocation + direction;
        if (! juce::isPositiveAndBelow (destination, locations.size())) return;
        locations.move (selectedLocation, destination);
        selectedLocation = destination;
        persistLocations();
        rebuildRows();
    }

    juce::FileSearchPath selectedSearchPath() const
    {
        juce::FileSearchPath path;
        for (const auto& location : locations)
            if (location.enabled)
                path.add (location.folder);
        return path;
    }

    const PluginItem* selectedPlugin() const
    {
        return juce::isPositiveAndBelow (selectedPluginIndex,
                                          (int) filteredPluginItems.size())
                 ? &filteredPluginItems[(size_t) selectedPluginIndex]
                 : nullptr;
    }

    void revealSelectedPlugin()
    {
        const auto* item = selectedPlugin();
        if (item == nullptr) return;

        const juce::File file (item->error ? item->errorIdentifier
                                           : item->description.fileOrIdentifier);
        if (file.existsAsFile() || file.isDirectory())
        {
            file.revealToUser();
            return;
        }

        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Local do plugin",
            "Este formato usa um identificador do sistema e nao possui um "
            "arquivo que possa ser mostrado no Explorer ou Finder.");
    }

    void closePluginPreview()
    {
        // Detach ownership before vendor callbacks: releaseResources may pump
        // native events that close the manager or request another preview.
        auto instance = std::move (previewInstance);
        auto window = std::move (previewWindow);
        openingPlugin = false;
        openPluginButton.setButtonText ("Abrir plugin");
        if (window != nullptr)
        {
            if (const auto cleared = ChurchDAWPluginPreview::hideAndClearEditorSafely (*window, instance.get());
                cleared.failed())
                juce::Logger::writeToLog ("PLUGIN_PREVIEW close_failed error="
                                         + cleared.getErrorMessage());
            window.reset();
        }
        if (instance != nullptr)
            if (const auto released = ChurchDAWPluginPreview::releaseResourcesSafely (*instance);
                released.failed())
                juce::Logger::writeToLog ("PLUGIN_PREVIEW release_failed error="
                                         + released.getErrorMessage());
    }

    void reportPluginPreviewFailure (const juce::String& pluginName,
                                      const juce::String& error)
    {
        openingPlugin = false;
        openPluginButton.setButtonText ("Abrir plugin");
        refreshPluginRows();
        const auto detail = error.isNotEmpty() ? error
            : juce::String ("Falha sem detalhe do fabricante.");
        juce::Logger::writeToLog ("PLUGIN_PREVIEW open_failed plugin=" + pluginName
                                 + " error=" + detail);
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Nao foi possivel abrir o plugin", pluginName + "\n\n" + detail);
    }

    void openSelectedPlugin()
    {
        const auto* item = selectedPlugin();
        if (item == nullptr || item->error || openingPlugin
             || scanInProgress || list.isScanning())
            return;

        if (previewWindow != nullptr)
        {
            previewWindow->toFront (true);
            return;
        }

        openingPlugin = true;
        openPluginButton.setEnabled (false);
        openPluginButton.setButtonText ("ABRINDO...");
        const auto description = item->description;
        auto safe = juce::Component::SafePointer<PluginListHolder> (this);

        try
        {
            formats.createPluginInstanceAsync (
                description, 44100.0, 512,
                [safe, description] (
                    std::unique_ptr<juce::AudioPluginInstance> instance,
                    const juce::String& error)
                {
                    if (safe == nullptr) return;
                    std::unique_ptr<PluginPreviewWindow> window;
                    auto opened = juce::Result::fail (error.isNotEmpty() ? error
                        : juce::String ("Falha sem detalhe do fabricante."));
                    if (instance != nullptr)
                    {
                        try
                        {
                            window = std::make_unique<PluginPreviewWindow> (
                                description.name.isNotEmpty() ? description.name
                                                               : juce::String ("Plugin"),
                                [safe]
                                {
                                    if (auto* owner = safe.getComponent())
                                        owner->closePluginPreview();
                                });
                            opened = ChurchDAWPluginPreview::prepareAndAttachEditor (
                                *window, *instance, 44100.0, 512);
                        }
                        catch (...)
                        {
                            opened = juce::Result::fail (
                                ChurchDAWPluginPreview::currentExceptionMessage());
                        }
                    }

                    // Vendor initialization can pump native events. Keep the
                    // candidate local until ready and recheck the owner after
                    // returning, so closing the gallery cannot publish a late UI.
                    if (opened.failed() || safe == nullptr)
                    {
                        if (window != nullptr)
                            if (const auto cleared = ChurchDAWPluginPreview::hideAndClearEditorSafely (*window, instance.get());
                                cleared.failed())
                                juce::Logger::writeToLog ("PLUGIN_PREVIEW close_failed error="
                                                         + cleared.getErrorMessage());
                        window.reset();
                        if (instance != nullptr)
                            if (const auto released = ChurchDAWPluginPreview::releaseResourcesSafely (*instance);
                                released.failed())
                                juce::Logger::writeToLog ("PLUGIN_PREVIEW release_failed plugin="
                                    + description.name + " error=" + released.getErrorMessage());
                        instance.reset();
                        if (auto* self = safe.getComponent())
                            self->reportPluginPreviewFailure (description.name, opened.getErrorMessage());
                        return;
                    }

                    auto* self = safe.getComponent();
                    self->previewInstance = std::move (instance);
                    self->previewWindow = std::move (window);
                    self->openingPlugin = false;
                    self->openPluginButton.setButtonText ("Abrir plugin");
                    const auto shown = ChurchDAWPluginPreview::showEditorSafely (*self->previewWindow);
                    if (shown.failed())
                    {
                        if (auto* owner = safe.getComponent()) owner->closePluginPreview();
                        if (auto* owner = safe.getComponent())
                            owner->reportPluginPreviewFailure (description.name, shown.getErrorMessage());
                        return;
                    }
                    if (auto* owner = safe.getComponent()) owner->refreshPluginRows();
                });
        }
        catch (...)
        {
            if (auto* self = safe.getComponent())
                self->reportPluginPreviewFailure (description.name,
                    ChurchDAWPluginPreview::currentExceptionMessage());
        }
    }

    void showOptionsMenu()
    {
        juce::PopupMenu menu;
        menu.addItem ("Limpar lista de plugins", [this]
        {
            knownPlugins.clear();
            rebuildPluginRows();
        });
        menu.addItem ("Remover plugins selecionados", [this]
        {
            list.removeSelectedPlugins();
        });

        auto disabledIds = persistentQuarantine;
        disabledIds.addArray (knownPlugins.getBlacklistedFiles());
        disabledIds.removeEmptyStrings();
        disabledIds.removeDuplicates (true);
        const int disabledCount = disabledIds.size();
        menu.addItem (
            "Revalidar desativados (" + juce::String (disabledCount) + ")",
            disabledCount > 0 || deadMansPedalFile.existsAsFile(),
            false,
            [this] { revalidateDisabledPlugins(); });

        menu.addSeparator();

        for (auto* format : formats.getFormats())
            if (format != nullptr && format->canScanForPlugins())
                menu.addItem ("Escanear " + format->getName(), [this, format]
                {
                    scanSingleFormat (*format);
                });

        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withDeletionCheck (*this)
                .withTargetComponent (settingsButton));
    }

    bool revalidateDisabledPlugins (const juce::StringArray& quarantined = {})
    {
        if (scanInProgress || openingPlugin || list.isScanning() || ! filaDeFormatos.isEmpty())
        {
            juce::Logger::writeToLog (
                "PLUGIN_SCAN revalidate_ignored reason=batch_in_progress");
            return false;
        }

        candidateSearchErrors.clear();

        auto disabled = persistentQuarantine;
        disabled.addArray (quarantined);
        disabled.addArray (knownPlugins.getBlacklistedFiles());
        disabled.removeEmptyStrings();
        disabled.removeDuplicates (true);

        // Infraestrutura Apple nunca foi um plugin selecionavel. Retira esses
        // falsos positivos definitivamente; todos os demais IDs continuam no
        // lote explicito abaixo, inclusive VST3 fora do caminho padrao.
        for (int i = disabled.size(); --i >= 0;)
            if (ChurchDAWPluginScan::ehAudioUnitDeSistema (disabled[i]))
            {
                knownPlugins.removeFromBlacklist (disabled[i]);
                persistentQuarantine.removeString (disabled[i], true);
                disabled.remove (i);
            }

        normaliseQuarantine();

        if (! disabled.isEmpty())
        {
            const auto backup = propertiesToUse != nullptr
                ? propertiesToUse->getFile().getSiblingFile (
                      "PluginsDesativados-antes-da-revalidacao.txt")
                : juce::File{};

            if (backup == juce::File{}
                 || ! backup.replaceWithText (disabled.joinIntoString ("\n"),
                                               true, true))
            {
                const auto message =
                    "Nao foi possivel criar a copia de seguranca da lista "
                    "de plugins desativados. Nenhum plugin foi liberado.";
                juce::Logger::writeToLog (
                    "PLUGIN_SCAN revalidate_failed reason=backup_write");
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Nao foi possivel revalidar", message);
                return false;
            }

            // A quarentena e gravada ANTES de apagar blacklist/descricoes.
            // Assim, fechar ou cair a qualquer instante continua bloqueando os
            // IDs antigos no editor e na restauracao dos canais.
            persistentQuarantine.addArray (disabled);
            normaliseQuarantine();
            propertiesToUse->setValue (
                "pluginBlacklistMigrationBackupsReady", true);
        }

        // NENHUM ID sai do dead-man/blacklist antes de a barreira duravel
        // existir. Backup legivel nao substitui isto: o startup importa a
        // quarentena e o dead-man, nao um arquivo administrativo de backup.
        if (! persistQuarantineState())
        {
            const auto message =
                "Nao foi possivel gravar a quarentena de plugins. "
                "Nenhum plugin foi liberado; verifique o espaco e as "
                "permissoes da pasta de configuracao.";
            juce::Logger::writeToLog (
                "PLUGIN_SCAN revalidate_failed reason=quarantine_persist");
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Nao foi possivel revalidar", message);
            return false;
        }

        if (deadMansPedalFile.existsAsFile()
             && ! deadMansPedalFile.deleteFile()
             && ! deadMansPedalFile.replaceWithText ({}, true, true))
        {
            const auto message =
                "Nao foi possivel limpar o arquivo de seguranca do scanner:\n"
                + deadMansPedalFile.getFullPathName();
            juce::Logger::writeToLog ("PLUGIN_SCAN revalidate_failed " + message);
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon,
                "Nao foi possivel revalidar",
                message);
            return false;
        }

        if (disabled.isEmpty())
        {
            juce::Logger::writeToLog (
                "PLUGIN_SCAN user_revalidate_disabled count=0");

            if (onBatchFinished != nullptr)
            {
                auto callback = std::move (onBatchFinished);
                callback (true);
            }

            return true;
        }

        // Remove descricoes antigas antes de liberar a blacklist. Sem isto um
        // plugin v1 ficava selecionavel durante a prova e uma falha limpa
        // (arquitetura/licenca) deixava a descricao velha parecendo validada.
        const auto typesBeforeRevalidation = knownPlugins.getTypes();

        for (const auto& description : typesBeforeRevalidation)
            if (disabled.contains (description.fileOrIdentifier, true))
                knownPlugins.removeType (description);

        for (const auto& identifier : disabled)
            knownPlugins.removeFromBlacklist (identifier);

        juce::Logger::writeToLog (
            "PLUGIN_SCAN user_revalidate_disabled count="
            + juce::String (disabled.size()));

        filaDeFormatos.clearQuick();
        revalidationTargets = disabled;
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = true;
        scanInProgress = true;
        pluginsBeforeScan = knownPlugins.getNumTypes();
        scanProgress = 0.0;
        scanButton.setButtonText ("ESCANEANDO...");
        juce::StringArray assigned;

        for (auto* format : formats.getFormats())
        {
            if (format == nullptr || ! format->canScanForPlugins())
                continue;

            for (const auto& identifier : disabled)
            {
                if (assigned.contains (identifier, true))
                    continue;

                bool knownFormat = false;

                for (const auto& description : typesBeforeRevalidation)
                    if (description.fileOrIdentifier.equalsIgnoreCase (identifier)
                         && description.pluginFormatName.equalsIgnoreCase (
                                format->getName()))
                    {
                        knownFormat = true;
                        break;
                    }

                // AU identifiers already encode their format. Asking AU's
                // fileMightContainThisPluginType here calls the OS registry
                // on the message thread before the isolated validation.
                const bool matchesFormat = identifier.startsWithIgnoreCase ("AudioUnit:")
                    ? format->getName().containsIgnoreCase ("AudioUnit")
                    : format->fileMightContainThisPluginType (identifier);
                if (knownFormat || matchesFormat)
                {
                    // Um pedido por ID. O CustomScanner agrega apenas um bool
                    // inconclusivo por scan; agrupar dez VST3 faria uma falha
                    // de IPC de UM manter Kontakt/BigSky validos em quarentena.
                    // Isolando, o residual e exatamente o candidato que nao
                    // conseguiu concluir a prova.
                    filaDeFormatos.add ({ format, { identifier }, true });
                    assigned.add (identifier);
                }
            }
        }

        juce::StringArray unmatched;

        for (const auto& identifier : disabled)
            if (! assigned.contains (identifier, true))
                unmatched.add (identifier);

        if (! unmatched.isEmpty())
        {
            unresolvedTargets.addArray (unmatched);
            juce::Logger::writeToLog (
                "PLUGIN_SCAN quarantine_unmatched count="
                + juce::String (unmatched.size())
                + " ids=" + unmatched.joinIntoString (" | "));
        }

        scanButton.setEnabled (false);
        cancelButton.setButtonText ("CANCELAR");
        dispararProximoFormato();
        return true;
    }

    void scanSingleFormat (juce::AudioPluginFormat& format)
    {
        if (scanInProgress || openingPlugin || list.isScanning())
            return;

        revalidationTargets.clear();
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = false;
        candidateSearchErrors.clear();
        filaDeFormatos.add ({ &format, {}, false });
        scanInProgress = true;
        pluginsBeforeScan = knownPlugins.getNumTypes();
        scanStatus = "Preparando " + format.getName() + "...";
        scanButton.setEnabled (false);
        cancelButton.setButtonText ("CANCELAR");
        scanButton.setButtonText ("ESCANEANDO...");
        dispararProximoFormato();
    }

    void scanNow()
    {
        if (scanInProgress || openingPlugin || list.isScanning())
            return;

        // UM FORMATO DE CADA VEZ.
        //
        // PluginListComponent::scanFor faz "currentScanner = Scanner::make
        // (...)": chamar duas vezes em sequencia DESTROI o scanner anterior
        // e so o ultimo formato roda. Foi o que aconteceu no Mac do cliente
        // em 31/07 — o BigSky esta instalado apenas como Audio Unit
        // (/Library/Audio/Plug-Ins/Components/BigSky.component) e a lista
        // voltou inteira com "VST3" na coluna de formato, sem nenhum AU.
        //
        // A fila abaixo dispara o proximo formato so quando o anterior
        // termina (isScanning() == false).
        //
        // ORDEM: Audio Unit ANTES de VST3 no macOS. A pasta de VST3 do
        // cliente e a que tem os plugins problematicos (varredura longa,
        // alguns travando); com AU no fim, uma varredura de VST3 que
        // demorasse deixava o BigSky — que so existe como AU — sem nunca
        // ser procurado. AU primeiro garante que o formato nativo do
        // sistema entre na lista mesmo se o VST3 der trabalho depois.
        filaDeFormatos.clearQuick();
        candidateSearchErrors.clear();
        revalidationTargets.clear();
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = false;
        scanButton.setEnabled (false);
        cancelButton.setButtonText ("CANCELAR");
        scanInProgress = true;
        pluginsBeforeScan = knownPlugins.getNumTypes();
        scanStatus = "Preparando locais selecionados...";
        scanProgress = 0.0;
        scanButton.setButtonText ("ESCANEANDO...");

       #if ! JUCE_IOS
        // AUv3 no iOS nao vive em pastas: os componentes vem do registro do
        // sistema. Exigir "uma pasta selecionada" ali impediria o unico scan
        // que a plataforma tem.
        if (! ChurchDAWPluginScan::podeIniciarBusca (
                formats, selectedSearchPath().getNumPaths()))
        {
            scanInProgress = false;
            scanButton.setEnabled (true);
            cancelButton.setButtonText ("Fechar");
            scanButton.setButtonText ("Escanear plugins");
            scanStatus = "Selecione ao menos uma pasta de plugins";
            repaint();
            return;
        }
       #endif

        for (auto* format : formats.getFormats())
            if (format != nullptr && format->canScanForPlugins()
                 && format->getName().containsIgnoreCase ("AudioUnit"))
                filaDeFormatos.add ({ format, {}, false });

        for (auto* format : formats.getFormats())
            if (format != nullptr && format->canScanForPlugins()
                 && ! format->getName().containsIgnoreCase ("AudioUnit"))
                filaDeFormatos.add ({ format, {}, false });

        dispararProximoFormato();
    }

    void startPreparedScan (juce::AudioPluginFormat& format,
                            const juce::StringArray& identifiers)
    {
        activeScanIdentifiers = identifiers;
        preparedScanFormat = std::make_unique<ChurchDAWPluginScan::PreparedPluginFormat> (
            format, identifiers);
        scanStatus = "Escaneando " + format.getName() + " - "
                   + juce::String (identifiers.size()) + " candidato(s)";
        scanCurrentItem.clear();
        scanProgress = 0.0;
        list.scanFor (*preparedScanFormat, identifiers);
        startTimer (100);
        repaint (footerBounds);
    }

    void dispararProximoFormato()
    {
        // Only called after the preceding JUCE scanner has released its format.
        preparedScanFormat.reset();
        while (! filaDeFormatos.isEmpty())
        {
            auto request = filaDeFormatos.removeAndReturn (0);
            auto* format = request.format;

            if (format == nullptr)
                continue;

            if (request.explicitIdentifiers)
            {
                if (request.identifiers.isEmpty())
                    continue;

                juce::Logger::writeToLog (
                    "PLUGIN_SCAN explicit_revalidation format="
                    + format->getName()
                    + " count=" + juce::String (request.identifiers.size()));
                startPreparedScan (*format, request.identifiers);
            }
            else
            {
                activeScanIdentifiers.clear();

           #if ! JUCE_IOS
                const auto formatName = format->getName();
                const auto paths = selectedSearchPath();
                candidateSearchFormat = format;
                scanStatus = "Procurando plugins " + formatName + "...";
                scanCurrentItem.clear();
                scanProgress = 0.0;
                candidateSearch = std::make_unique<ChurchDAWPluginScan::AsyncCandidateSearch> (
                    [formatName, paths] (const std::atomic<bool>& cancelled)
                    {
                        return ChurchDAWPluginScan::searchPluginCandidates (
                            formatName, paths, cancelled);
                    });
           #else
                if (format->getName().containsIgnoreCase ("AudioUnit"))
                {
                    const auto allIdentifiers = format->searchPathsForPlugins (
                        format->getDefaultLocationsToSearch(), true, true);
                    const auto hostable =
                        ChurchDAWPluginScan::filtrarAudioUnitsHospedaveis (
                            allIdentifiers);

                    juce::Logger::writeToLog (
                        "PLUGIN_SCAN audio_units discovered="
                        + juce::String (allIdentifiers.size())
                        + " hostable=" + juce::String (hostable.size())
                        + " policy_skipped="
                        + juce::String (allIdentifiers.size() - hostable.size()));

                    // Lista explicita impede que os itens de politica cheguem ao
                    // PluginDirectoryScanner e virem blacklist/failedFiles.
                    if (hostable.isEmpty())
                        continue;

                    startPreparedScan (*format, hostable);
                }
                else
                {
                    scanStatus = "Escaneando " + format->getName() + "...";
                    list.scanFor (*format);
                }
           #endif
            }

            startTimer (100);
            repaint (footerBounds);
            return;
        }

        concluirLote (true);
    }

    void timerCallback() override
    {
        if (candidateSearch != nullptr)
        {
            auto result = candidateSearch->takeResult();
            if (! result.has_value()) return;

            auto* format = candidateSearchFormat;
            candidateSearch.reset();
            candidateSearchFormat = nullptr;
            if (result->error.isNotEmpty())
            {
                candidateSearchErrors.add (result->error);
                juce::Logger::writeToLog ("PLUGIN_SCAN enumeration_failed " + result->error);
                dispararProximoFormato();
                return;
            }
            const auto hostable = ChurchDAWPluginScan::filtrarAudioUnitsHospedaveis (
                result->identifiers);
            juce::Logger::writeToLog ("PLUGIN_SCAN enumeration_completed format="
                + format->getName() + " candidates=" + juce::String (hostable.size()));
            if (hostable.isEmpty())
                dispararProximoFormato();
            else
                startPreparedScan (*format, hostable);
            return;
        }

        scanProgress = list.getScanProgress();
        const auto current = list.getCurrentlyScannedPlugin();
        if (current.isNotEmpty())
            scanCurrentItem = current;
        repaint (footerBounds);

        if (! list.isScanning())
        {
            const bool wasCancelled = list.consumeLastScanWasCancelled();
            const bool wasInconclusive =
                list.consumeLastScanWasInconclusive();

            if (wasInconclusive)
            {
                if (revalidationBatchActive)
                    unresolvedTargets.addArray (activeScanIdentifiers);

                juce::Logger::writeToLog (
                    "PLUGIN_SCAN scan_inconclusive retry_count="
                    + juce::String (activeScanIdentifiers.size()));
            }

            activeScanIdentifiers.clear();

            if (wasCancelled)
            {
                filaDeFormatos.clearQuick();
                juce::Logger::writeToLog (
                    "PLUGIN_SCAN batch_cancelled remaining_formats_discarded");
                concluirLote (false);
                return;
            }

            dispararProximoFormato();
        }
    }

    void concluirLote (bool completed)
    {
        stopTimer();
        scanButton.setEnabled (true);
        cancelButton.setButtonText ("Fechar");
        scanButton.setButtonText ("Escanear plugins");
        scanInProgress = false;
        scanProgress = completed ? 1.0 : 0.0;
        const int found = juce::jmax (0, knownPlugins.getNumTypes() - pluginsBeforeScan);
        scanStatus = completed
            ? "Concluido - " + juce::String (found) + " plugin(s) novo(s)"
            : "Escaneamento cancelado";
        if (completed && ! candidateSearchErrors.isEmpty())
            scanStatus = "Busca incompleta - " + candidateSearchErrors.joinIntoString (" ");
        if (completed && candidateSearchErrors.isEmpty())
        {
            lastVerification = juce::Time::getCurrentTime().toISO8601 (true);
            if (propertiesToUse != nullptr)
            {
                propertiesToUse->setValue ("churchdawPluginLastVerification",
                                           lastVerification);
                propertiesToUse->saveIfNeeded();
            }
        }

        bool allRevalidationTargetsResolved = completed && candidateSearchErrors.isEmpty();

        if (revalidationBatchActive)
        {
            const auto quarantineDuravelAnterior = revalidationTargets;
            auto remaining = unresolvedTargets;

            if (! completed)
                remaining.addArray (revalidationTargets);

            // Crashes voltaram para a blacklist pelo CustomScanner e tambem
            // precisam continuar na barreira do editor/restauracao. Timeouts
            // entram em unresolvedTargets, sem falso "desativado" permanente.
            const auto blacklisted = knownPlugins.getBlacklistedFiles();
            for (const auto& identifier : revalidationTargets)
                if (blacklisted.contains (identifier, true))
                    remaining.addIfNotAlreadyThere (identifier);

            remaining.removeEmptyStrings();
            remaining.removeDuplicates (true);

            for (const auto& identifier : revalidationTargets)
                persistentQuarantine.removeString (identifier, true);

            persistentQuarantine.addArray (remaining);
            normaliseQuarantine();
            const bool persistiu = persistQuarantineState();
            allRevalidationTargetsResolved = persistiu && remaining.isEmpty();

            if (! persistiu)
            {
                // O arquivo em disco ainda contem a barreira completa gravada
                // antes do lote. Reponha o mesmo estado tambem na memoria,
                // senao um save posterior poderia publicar uma liberacao que
                // esta rodada nao conseguiu tornar duravel.
                persistentQuarantine.addArray (quarantineDuravelAnterior);
                normaliseQuarantine();
                (void) persistQuarantineState (false);
                juce::Logger::writeToLog (
                    "PLUGIN_SCAN quarantine_persist_failed during=batch_finish");
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Plugins continuam protegidos",
                    "A validacao terminou, mas o resultado nao pode ser "
                    "gravado. Os plugins permanecem desativados para nao "
                    "serem liberados sem seguranca.");
            }
        }

        revalidationTargets.clear();
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = false;

        if (onBatchFinished != nullptr)
        {
            auto callback = std::move (onBatchFinished);
            callback (allRevalidationTargetsResolved);
        }

        rebuildRows();
    }

    void normaliseQuarantine()
    {
        persistentQuarantine.removeEmptyStrings();
        persistentQuarantine.removeDuplicates (true);

        for (int i = persistentQuarantine.size(); --i >= 0;)
            if (ChurchDAWPluginScan::ehAudioUnitDeSistema (
                    persistentQuarantine[i]))
                persistentQuarantine.remove (i);
    }

    bool persistQuarantineState (bool includeKnownPluginList = true)
    {
        normaliseQuarantine();

        if (propertiesToUse == nullptr)
            return false;

        propertiesToUse->setValue (
            "pluginBlacklistMigrationQuarantine",
            persistentQuarantine.joinIntoString ("\n"));
        propertiesToUse->setValue (
            "pluginBlacklistMigrationPending",
            ! persistentQuarantine.isEmpty());

        if (persistentQuarantine.isEmpty())
            propertiesToUse->setValue ("pluginBlacklistSemanticsVersion", 3);

        if (includeKnownPluginList)
            if (auto xml = knownPlugins.createXml())
                propertiesToUse->setValue ("pluginList", xml.get());

        return propertiesToUse->saveIfNeeded();
    }

    static juce::String formatosDaPlataforma()
    {
       #if JUCE_IOS
        return "AUv3";
       #elif JUCE_MAC && JUCE_PLUGINHOST_VST
        return "VST2, VST3 e Audio Unit";
       #elif JUCE_MAC
        return "VST3 e Audio Unit";
       #elif JUCE_WINDOWS && JUCE_PLUGINHOST_VST
        return "VST2 e VST3";
       #else
        return "VST3";
       #endif
    }

    void paint (juce::Graphics& g) override
    {
        using L = NordLookAndFeel;
        g.fillAll (L::bgApp());
        g.setColour (L::bgCard());
        g.fillRect (headerBounds);
        g.setColour (L::borderSoft());
        g.fillRect (headerBounds.withTop (headerBounds.getBottom() - 1));
        const bool phoneCompact = headerBounds.getHeight() < 68;
        auto heading = headerBounds.reduced (phoneCompact ? 14 : 26, 0);
        if (phoneCompact)
        {
            g.setColour (L::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (20.0f)).boldened());
            g.drawText ("Plugins", heading.withWidth (118), juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (L::nordRedBright());
            g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.10f));
            g.drawText ("CHURCHDAW", heading.withTop (17).withHeight (16), juce::Justification::centredLeft);
            g.setColour (L::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (27.0f)).boldened());
            g.drawText ("Plugins", heading.withTop (34).withHeight (34), juce::Justification::centredLeft);
        }
        g.setColour (L::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.07f));
        g.drawText (juce::String::fromUTF8 (u8"COLEÇÃO"), leftBounds.reduced (phoneCompact ? 8 : 15, 0).withTrimmedTop (phoneCompact ? 4 : 10).withHeight (20), juce::Justification::centredLeft);
        if (categoryButtons[0].isVisible())
            g.drawText ("LOCAIS", leftBounds.reduced (phoneCompact ? 8 : 15, 0).withTrimmedTop (leftBounds.getHeight() < 480 ? 234 : 255).withHeight (20), juce::Justification::centredLeft);
        g.setColour (L::borderSoft());
        g.drawVerticalLine (leftBounds.getRight() + (phoneCompact ? 6 : 12), (float) leftBounds.getY(), (float) leftBounds.getBottom());
        auto banner = bannerBounds;
        g.setColour (L::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (phoneCompact ? 16.0f : 20.0f)).boldened());
        const juce::String names[] {juce::String::fromUTF8 (u8"Sua coleção"), "Instrumentos", "Efeitos", "Favoritos", juce::String::fromUTF8 (u8"Requer atenção")};
        g.drawText (names[juce::jlimit (0, 4, selectedPluginFilter)], banner.withHeight (phoneCompact ? 22 : 28), juce::Justification::centredLeft);
        g.setColour (L::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (phoneCompact ? 11.0f : 12.0f)));
        g.drawText (juce::String ((int) filteredPluginItems.size()) + juce::String::fromUTF8 (u8" plugins · ") + formatosDaPlataforma(),
                    banner.withTrimmedTop (phoneCompact ? 23 : 30).withHeight (20), juce::Justification::centredLeft);
        if (filteredPluginItems.empty())
        {
            g.setFont (juce::Font (juce::FontOptions (phoneCompact ? 14.0f : 16.0f)));
            g.drawFittedText (searchEditor.getText().isNotEmpty() ? "Nenhum plugin encontrado. Tente outro nome."
                                                               : juce::String::fromUTF8 (u8"Sua coleção começa aqui. Escaneie seus plugins."),
                              pluginViewport.getBounds().reduced (16), juce::Justification::centred, 3);
        }
        g.setColour (juce::Colour (0xff1a1d22));
        g.fillRoundedRectangle (infoBounds.toFloat(), 14.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (infoBounds.toFloat().reduced (0.5f), 14.0f, 1.0f);
        auto info = infoBounds.reduced (phoneCompact ? 12 : 20);
        g.setColour (L::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.06f));
        g.drawText ("SELECIONADO", info.removeFromTop (phoneCompact ? 18 : 22), juce::Justification::centredLeft);
        if (const auto* item = selectedPlugin())
        {
            if (infoBounds.getHeight() >= 340)
            {
                const int artH = juce::jlimit (70, 144, infoBounds.getHeight() - 425);
                auto art = info.removeFromTop (artH);
                ChurchDAWArtwork::plugin (g, art.toFloat(), item->displayName(), item->description.isInstrument);
                info.removeFromTop (18);
            }
            g.setColour (L::txtPrimary());
            g.setFont (juce::Font (juce::FontOptions (phoneCompact ? 16.5f : 22.0f)).boldened());
            g.drawFittedText (item->displayName(), info.removeFromTop (phoneCompact ? 24 : 32), juce::Justification::centredLeft, 1);
            g.setColour (L::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (phoneCompact ? 11.5f : 13.0f)));
            g.drawText (item->description.manufacturerName, info.removeFromTop (phoneCompact ? 18 : 24), juce::Justification::centredLeft, true);
            if (! phoneCompact)
                info.removeFromTop (12);
            auto status = info.removeFromTop (phoneCompact ? 20 : 24);
            g.setColour (item->error ? L::redClip() : L::greenOk());
            g.fillEllipse ((float) status.getX(), (float) status.getCentreY() - 3, 6, 6);
            g.drawText (item->error ? juce::String::fromUTF8 (u8"Requer verificação") : item->active ? "Em uso no projeto" : "Pronto para usar",
                        status.withTrimmedLeft (15), juce::Justification::centredLeft);
            info.removeFromTop (10);
            g.setColour (L::borderSoft());
            g.fillRect (info.removeFromTop (1));
            info.removeFromTop (12);
            g.setColour (L::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            const auto type = item->description.isInstrument ? juce::String ("Instrumento virtual") : juce::String::fromUTF8 (u8"Efeito de áudio");
            g.drawText (type + juce::String::fromUTF8 (u8" · ") + item->formatName(), info.removeFromTop (23), juce::Justification::centredLeft, true);
            if (info.getY() + 40 < formatChoice.getY())
            {
                g.setColour (L::txtDisabled());
                g.setFont (juce::Font (juce::FontOptions (11.5f)));
                const auto hint = item->error ? "Use o menu do card para verificar o plugin novamente."
                    : item->usage.isNotEmpty() ? "Em uso: " + item->usage
                    : juce::String::fromUTF8 (u8"Escolha o formato e abra a interface do plugin.");
                g.drawMultiLineText (hint, info.getX(), info.getY() + 15, info.getWidth(), juce::Justification::left, 3.0f);
            }
        }
        else
        {
            auto hint = info.withHeight (juce::jmax (0, formatChoice.getY() - info.getY() - 24));
            g.setColour (L::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawFittedText (juce::String::fromUTF8 ("Selecione um plugin para ver os detalhes e abrir sua interface."),
                              hint.reduced (6), juce::Justification::centred, 4);
        }
        g.setColour (L::bgApp());
        g.fillRect (footerBounds);
        g.setColour (L::borderSoft());
        g.fillRect (footerBounds.withHeight (1));
        auto status = footerBounds.reduced (26, 0);
        status.setRight (cancelButton.getX() - 18);
        g.setColour (scanInProgress ? L::nordRedBright() : L::greenOk());
        g.fillEllipse ((float) status.getX(), (float) status.getCentreY() - 3, 6, 6);
        status.removeFromLeft (16);
        g.setColour (L::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (scanInProgress ? scanStatus : juce::String ((int) allPluginItems.size()) + " produtos / " + juce::String (knownPlugins.getNumTypes()) + " formatos instalados",
                    status, juce::Justification::centredLeft, true);
        if (scanInProgress)
        {
            g.setColour (L::nordRed());
            g.fillRect (progressBounds.withWidth (juce::roundToInt (progressBounds.getWidth() * juce::jlimit (0.0, 1.0, scanProgress))));
        }
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::escapeKey))
        { cancelButton.triggerClick(); return true; }
        if (pluginRows.empty()) return false;
        const auto layout = ChurchDAWPluginGallery::forWidth (pluginViewport.getWidth() - 8);
        int step = 0;
        if (key.isKeyCode (juce::KeyPress::leftKey)) step = -1;
        if (key.isKeyCode (juce::KeyPress::rightKey)) step = 1;
        if (key.isKeyCode (juce::KeyPress::upKey)) step = -layout.columns;
        if (key.isKeyCode (juce::KeyPress::downKey)) step = layout.columns;
        if (step == 0) return false;
        selectedPluginIndex = juce::jlimit (0, (int) pluginRows.size() - 1, selectedPluginIndex + step);
        refreshPluginRows();
        auto& row = *pluginRows[(size_t) selectedPluginIndex];
        row.grabKeyboardFocus();
        const auto viewport = pluginViewport.getViewArea();
        if (row.getY() < viewport.getY()) pluginViewport.setViewPosition (0, row.getY());
        else if (row.getBottom() > viewport.getBottom())
            pluginViewport.setViewPosition (0, row.getBottom() - viewport.getHeight());
        return true;
    }

    void resized() override
    {
        list.setBounds ({}); rowsViewport.setBounds ({});
        for (auto* button : {&changeFolderButton, &removeFolderButton, &moveUpButton, &moveDownButton}) button->setBounds ({});
        auto area = getLocalBounds();
        const bool phoneCompact = getWidth() < 920 || getHeight() < 520;
        headerBounds = area.removeFromTop (phoneCompact ? 54 : 88);
        footerBounds = area.removeFromBottom (phoneCompact ? 50 : 64);
        area.reduce (phoneCompact ? 10 : 22, phoneCompact ? 8 : 20);
        const bool compact = getWidth() < 1120;
        leftBounds = area.removeFromLeft (phoneCompact ? 132 : (compact ? 166 : 182));
        area.removeFromLeft (phoneCompact ? 12 : 26);
        infoBounds = area.removeFromRight (phoneCompact ? 196 : (compact ? 250 : 274));
        area.removeFromRight (phoneCompact ? 10 : 22);
        const int bannerH = phoneCompact ? 46 : 60;
        const int bannerGap = phoneCompact ? 6 : 12;
        bannerBounds = area.removeFromTop (bannerH);
        area.removeFromTop (bannerGap);
        tableBounds = area;
        pluginViewport.setBounds (tableBounds);
        layoutPluginRows();
        auto left = leftBounds;
        left.removeFromTop (phoneCompact ? 26 : 40);
        const bool shortPanel = leftBounds.getHeight() < 480;
        const int filterBtnH = phoneCompact
            ? juce::jlimit (28, 36, (left.getHeight() - 12) / 5)
            : (shortPanel ? 34 : 38);
        const int filterGap = phoneCompact ? 3 : 4;
        for (auto& button : pluginFilterButtons)
        { button.setBounds (left.removeFromTop (filterBtnH)); left.removeFromTop (filterGap); }
        if (categoryButtons[0].isVisible())
        {
            left = leftBounds.withTrimmedTop (shortPanel ? 260 : 286);
            for (auto& button : categoryButtons)
            { button.setBounds (left.removeFromTop (shortPanel ? 27 : 30)); left.removeFromTop (3); }
        }
        else
        {
            for (auto& button : categoryButtons)
                button.setBounds ({});
        }
        addFolderButton.setBounds (addFolderButton.isVisible()
                                       ? leftBounds.withTop (leftBounds.getBottom() - 38)
                                       : juce::Rectangle<int>());
        sortBox.setBounds (bannerBounds.removeFromRight (phoneCompact ? 116 : (compact ? 133 : 154)).withHeight (phoneCompact ? 32 : 34));
        // Keep the heading's complete original bounds for paint.
        bannerBounds = tableBounds.withY (tableBounds.getY() - (bannerH + bannerGap)).withHeight (bannerH);
        const int actionsH = revealPluginButton.isVisible()
            ? (phoneCompact ? 148 : 168)
            : (phoneCompact ? 116 : 126);
        auto actions = infoBounds.reduced (phoneCompact ? 12 : 16).removeFromBottom (actionsH);
        formatChoice.setBounds (actions.removeFromTop (phoneCompact ? 30 : 32)); actions.removeFromTop (phoneCompact ? 6 : 8);
        openPluginButton.setBounds (actions.removeFromTop (phoneCompact ? 38 : 42)); actions.removeFromTop (phoneCompact ? 6 : 8);
        favouritePluginButton.setBounds (actions.removeFromTop (phoneCompact ? 34 : 36));
        if (revealPluginButton.isVisible())
        {
            actions.removeFromTop (6);
            revealPluginButton.setBounds (actions.removeFromTop (34));
        }
        else
        {
            revealPluginButton.setBounds ({});
        }
        auto foot = footerBounds.reduced (phoneCompact ? 12 : 22, phoneCompact ? 5 : 12);
        scanButton.setBounds (foot.removeFromRight (phoneCompact ? 152 : 172)); foot.removeFromRight (phoneCompact ? 8 : 10);
        cancelButton.setBounds (foot.removeFromRight (phoneCompact ? 80 : 86));
        progressBounds = footerBounds.withY (footerBounds.getBottom() - 3).withHeight (3);
        const int topBtnSize = phoneCompact ? 38 : 36;
        const int topBtnY = (headerBounds.getHeight() - topBtnSize) / 2;
        closeButton.setBounds (getWidth() - (phoneCompact ? 48 : 60), topBtnY, topBtnSize, topBtnSize);
        settingsButton.setBounds (closeButton.getX() - (topBtnSize + 6), topBtnY, topBtnSize, topBtnSize);
        const int searchX = phoneCompact ? 132 : (compact ? 225 : 265);
        const int searchH = phoneCompact ? 36 : 40;
        searchEditor.setBounds (searchX, (headerBounds.getHeight() - searchH) / 2,
                                juce::jmax (120, settingsButton.getX() - searchX - 10), searchH);
        searchClearButton.setBounds (searchEditor.getRight() - 32, searchEditor.getY() + (searchH - 28) / 2, 28, 28);
        searchClearButton.toFront (false);
        closeButton.toFront (false); cancelButton.toFront (false); scanButton.toFront (false);
        refreshRows(); refreshPluginRows();
    }

    juce::ComboBox formatChoice;
    std::map<juce::String, juce::String> chosenPluginFormats;
    PluginManagerLookAndFeel pluginManagerLookAndFeel;
    juce::AudioPluginFormatManager& formats;
    juce::KnownPluginList& knownPlugins;
    juce::File deadMansPedalFile;
    juce::PropertiesFile* propertiesToUse = nullptr;
    juce::StringArray& persistentQuarantine;
    std::function<juce::String(const juce::PluginDescription&)> pluginIsActive;
    // Declaration order matters: list must stop/destroy its scanner before
    // the adapter referenced by that scanner is destroyed.
    std::unique_ptr<ChurchDAWPluginScan::PreparedPluginFormat> preparedScanFormat;
    std::unique_ptr<ChurchDAWPluginScan::AsyncCandidateSearch> candidateSearch;
    juce::AudioPluginFormat* candidateSearchFormat = nullptr;
    juce::StringArray candidateSearchErrors;
    juce::PluginListComponent list;
    juce::TextButton scanButton, cancelButton, settingsButton, closeButton;
    juce::TextButton addFolderButton, changeFolderButton, removeFolderButton;
    juce::TextButton moveUpButton, moveDownButton;
    std::array<juce::TextButton, 4> categoryButtons;
    std::array<juce::TextButton, 5> pluginFilterButtons;
    juce::TextEditor searchEditor;
    juce::TextButton searchClearButton;
    juce::ComboBox sortBox;
    juce::TextButton openPluginButton, revealPluginButton;
    juce::TextButton favouritePluginButton;
    juce::Array<ScanLocation> locations;
    juce::Array<int> visibleLocationIndices;
    juce::Component rowsContainer;
    juce::Viewport rowsViewport;
    std::vector<std::unique_ptr<FolderRow>> rows;
    juce::Component pluginRowsContainer;
    juce::Viewport pluginViewport;
    std::vector<std::unique_ptr<PluginRow>> pluginRows;
    std::vector<PluginItem> allPluginItems;
    std::vector<PluginItem> filteredPluginItems;
    juce::StringArray favouritePluginKeys;
    std::unique_ptr<juce::FileChooser> folderChooser;
    int selectedCategory = 0;
    int selectedLocation = -1;
    int selectedPluginFilter = 0;
    int selectedPluginIndex = -1;
    juce::Rectangle<int> headerBounds, leftBounds, bannerBounds, tableBounds;
    juce::Rectangle<int> infoBounds, footerBounds, progressBounds;
    bool scanInProgress = false;
    bool openingPlugin = false;
    double scanProgress = 0.0;
    int pluginsBeforeScan = 0;
    juce::String scanStatus, scanCurrentItem, lastVerification;
    juce::Array<PendingScan> filaDeFormatos;
    juce::StringArray revalidationTargets;
    juce::StringArray unresolvedTargets;
    juce::StringArray activeScanIdentifiers;
    bool revalidationBatchActive = false;
    std::function<void(bool completed)> onBatchFinished;
    std::unique_ptr<juce::AudioPluginInstance> previewInstance;
    std::unique_ptr<PluginPreviewWindow> previewWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginListHolder)
};

void MainComponent::showPluginScanner()
{
    if (auto* dialog = pluginScannerDialog.get())
    {
        dialog->toFront (true);
        return;
    }

    const auto deadMansPedal = appProperties.getUserSettings()->getFile()
                                   .getSiblingFile ("PluginsComProblema.txt");

    const auto safeMain = juce::Component::SafePointer<MainComponent> (this);
    auto* holder = new PluginListHolder (
        formatManager, knownPlugins, deadMansPedal,
        appProperties.getUserSettings(), pluginScanQuarantine,
        [safeMain] (const juce::PluginDescription& description)
        {
            juce::StringArray usage;
            if (auto* self = safeMain.getComponent())
            {
                for (int i = 0; i < numSlots; ++i)
                    if (description.matchesIdentifierString (self->slots[i].pluginIdentifier))
                        usage.add ("Slot " + juce::String (i + 1));
                if (auto* live = self->getChurchLive())
                {
                    const auto matches = [&description] (juce::AudioPluginInstance* instance)
                    {
                        if (instance == nullptr) return false;
                        juce::PluginDescription loaded;
                        instance->fillInPluginDescription (loaded);
                        return loaded.createIdentifierString() == description.createIdentifierString();
                    };
                    for (int i = 0; i < TimbresProcessor::numLayers; ++i)
                        if (matches (live->getLayerInstrument (i)))
                            usage.add ("Canal " + juce::String (i + 1));
                    for (int i = 0; i < TimbresProcessor::numInserts; ++i)
                        if (matches (live->getInsert (i)))
                            usage.add ("Master FX " + juce::String (i + 1));
                }
            }
            return usage.joinIntoString (", ");
        });

    holder->onClose = [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (auto* self = safe.getComponent())
            if (auto* dialog = self->pluginScannerDialog.get())
                dialog->exitModalState (0);
    };

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (holder);
    o.dialogTitle = juce::String::fromUTF8 (u8"Plugins — ")
                  + PluginListHolder::formatosDaPlataforma();
    // The mixer is scaled inside ScaledHolder. Passing it here also scaled the
    // native dialog, so display-sized bounds overflowed the screen and hid X/Close.
    o.componentToCentreAround = getTopLevelComponent();
    // LaunchOptions vem com dialogBackgroundColour = lightgrey: era dai o rodape
    // e as bordas claras que destoavam do app inteiro
    o.dialogBackgroundColour = NordLookAndFeel::background();
    o.useNativeTitleBar = false;   // barra de titulo desenhada pelo JUCE = escura
    o.resizable = true;
    // launchAsync() usa deleteWhenDismissed=true e deixa a vida do holder nas
    // maos do ModalComponentManager. Aqui o Main e o dono: o X apenas encerra
    // o modal, o callback apaga a janela, e o destrutor pode apaga-la de forma
    // sincrona antes das referencias que o holder usa.
    pluginScannerDialog.reset (o.create());
    pluginScannerDialog->setTitleBarHeight (0);
   #if JUCE_IOS
    pluginScannerDialog->setResizeLimits (480, 260, 1680, 1020);
   #else
    pluginScannerDialog->setResizeLimits (980, 620, 1680, 1020);
   #endif
    pluginScannerDialog->setResizable (true, true);
    auto usable = getScreenBounds();
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (usable))
        usable = display->userArea.reduced (display->userArea.getWidth() < 960 ? 6 : 16);
    const int dialogW = juce::jmin (1240, usable.getWidth());
    const int dialogH = juce::jmin (790, usable.getHeight());
    pluginScannerDialog->setBounds (usable.withSizeKeepingCentre (dialogW, dialogH));
    pluginScannerDialog->enterModalState (
        true,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<MainComponent> (this),
             lifetime = pluginLoadLifetime] (int)
            {
                if (! lifetime->load())
                    return;

                if (auto* self = safe.getComponent())
                    self->pluginScannerDialog.reset();
            }),
        false);

    const bool needsBlacklistRevalidation =
        appProperties.getUserSettings()->getBoolValue (
            "pluginBlacklistMigrationPending", false);
    const bool migrationBackupsReady =
        appProperties.getUserSettings()->getBoolValue (
            "pluginBlacklistMigrationBackupsReady", false);

    if (needsBlacklistRevalidation && migrationBackupsReady)
    {
        // O construtor do PluginListComponent acabou de importar o dead-man
        // para KnownPluginList. Persiste a uniao ANTES de remover qualquer ID,
        // para sobreviver ate a fechamento/crash durante a revalidacao.
        pluginScanQuarantine.addArray (knownPlugins.getBlacklistedFiles());
        pluginScanQuarantine.removeEmptyStrings();
        pluginScanQuarantine.removeDuplicates (true);
        appProperties.getUserSettings()->setValue (
            "pluginBlacklistMigrationQuarantine",
            pluginScanQuarantine.joinIntoString ("\n"));
        appProperties.getUserSettings()->saveIfNeeded();

        holder->onBatchFinished =
            [safe = juce::Component::SafePointer<MainComponent> (this)] (bool completed)
            {
                if (auto* self = safe.getComponent())
                {
                    auto* settings = self->appProperties.getUserSettings();

                    settings->setValue (
                        "pluginBlacklistMigrationQuarantine",
                        self->pluginScanQuarantine.joinIntoString ("\n"));
                    settings->setValue (
                        "pluginBlacklistMigrationPending",
                        ! self->pluginScanQuarantine.isEmpty());

                    if (completed && self->pluginScanQuarantine.isEmpty())
                        settings->setValue ("pluginBlacklistSemanticsVersion", 3);

                    if (auto xml = self->knownPlugins.createXml())
                        settings->setValue ("pluginList", xml.get());

                    settings->saveIfNeeded();
                    self->refreshSlotChoices();
                }
            };

        if (! holder->revalidateDisabledPlugins (pluginScanQuarantine))
            holder->onBatchFinished = nullptr;
    }
    else if (needsBlacklistRevalidation)
    {
        juce::Logger::writeToLog (
            "PLUGIN_SCAN legacy_revalidation_blocked reason=backup_not_ready");
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::WarningIcon,
            "Plugins desativados protegidos",
            "O ChurchDAW nao conseguiu criar a copia de seguranca da lista "
            "antiga. Os plugins continuam desativados e nenhuma lista foi "
            "apagada. Verifique o acesso a pasta de configuracao e tente "
            "abrir Plugins novamente.");
    }

    // Primeira vez ou migracao para v3: ja dispara AU + VST3. O scanner
    // isolado recoloca somente crashes observados na blacklist.
    if (! needsBlacklistRevalidation && knownPlugins.getNumTypes() == 0)
        holder->scanNow();
}
#else
// Celular/tablet: a declaracao continua existindo no cabecalho, entao o metodo
// precisa existir -- mas nao ha o que escanear.
void MainComponent::showPluginScanner() {}
#endif

//==============================================================================
// (PadPanel agora vive em PadPanel.h — bloco antigo desativado)
#if 0
struct PadPanel : public juce::Component
{
    explicit PadPanel (PadProcessor* p) : pad (p)
    {
        vol.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        vol.setRange (0.0, 1.5, 0.01);
        vol.setDoubleClickReturnValue (true, 0.9);
        vol.setValue (pad != nullptr ? pad->getGain() : 0.9, juce::dontSendNotification);
        vol.onValueChange = [this] { if (pad != nullptr) pad->setGain ((float) vol.getValue()); };
        addAndMakeVisible (vol);

        volLbl.setText ("VOLUME", juce::dontSendNotification);
        volLbl.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
        volLbl.setJustificationType (juce::Justification::centred);
        volLbl.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
        addAndMakeVisible (volLbl);

        stopBtn.setButtonText ("Parar");
        stopBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8f0c21));
        stopBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        stopBtn.onClick = [this] { select (-1); };
        addAndMakeVisible (stopBtn);

        current = pad != nullptr ? pad->getKey() : -1;
        setSize (600, 362);
    }

    static const char* noteName (int i)
    {
        static const char* nm[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return nm[juce::jlimit (0, 11, i)];
    }

    void toggle (int i) { select (current == i ? -1 : i); }
    void select (int k) { current = k; if (pad != nullptr) pad->setKey (k); repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto pt = e.position;
        for (int i = 0; i < 5; ++i) if (blackRects[i].contains (pt)) { toggle (kBlack[i]); return; }
        for (int i = 0; i < 7; ++i) if (whiteRects[i].contains (pt)) { toggle (kWhite[i]); return; }
    }

    void paint (juce::Graphics& g) override
    {
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff191a1f), 0.0f, 0.0f,
                                                 juce::Colour (0xff0f0f12), 0.0f, (float) getHeight(), false));
        g.fillRect (getLocalBounds());

        g.setColour (NordLookAndFeel::nordRedBright());
        g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened().withExtraKerningFactor (0.12f));
        g.drawText (juce::String::fromUTF8 ("PAD CONT\xc3\x8dNUO"), titleBounds, juce::Justification::centred);

        // display LCD (tom tocando)
        {
            auto d = displayBounds;
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRoundedRectangle (d.translated (0, 1.5f), 9.0f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0c1512), d.getX(), d.getY(),
                                                     juce::Colour (0xff070a09), d.getX(), d.getBottom(), false));
            g.fillRoundedRectangle (d, 9.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (d.reduced (0.5f), 9.0f, 1.0f);

            g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened().withExtraKerningFactor (0.2f));
            g.setColour (NordLookAndFeel::dimText());
            g.drawText (current < 0 ? "PAD" : "TOM",
                        d.toNearestInt().reduced (14, 9).removeFromTop (12).removeFromLeft (140),
                        juce::Justification::centredLeft);

            if (current >= 0)
            {
                g.setColour (NordLookAndFeel::nordRed().withAlpha (0.35f));
                g.setFont (juce::Font (juce::FontOptions (34.0f)).boldened());
                g.drawText (noteName (current), d.toNearestInt().translated (0, 1), juce::Justification::centred);
                g.setColour (NordLookAndFeel::nordRedBright());
                g.drawText (noteName (current), d.toNearestInt(), juce::Justification::centred);

                g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
                g.setColour (juce::Colour (0xff5fd08a));
                g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8f TOCANDO"),
                            d.toNearestInt().reduced (14, 9).removeFromBottom (12).removeFromRight (120),
                            juce::Justification::centredRight);
            }
            else
            {
                g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
                g.setColour (NordLookAndFeel::dimText());
                g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94 desligado \xe2\x80\x94"),
                            d.toNearestInt(), juce::Justification::centred);
            }
        }

        for (int w = 0; w < 7; ++w) drawWhite (g, whiteRects[w], kWhite[w]);
        for (int i = 0; i < 5; ++i) drawBlack (g, blackRects[i], kBlack[i]);
    }

    void drawWhite (juce::Graphics& g, juce::Rectangle<float> r, int key)
    {
        const bool on = (key == current);

        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.translated (0, 2.0f), 5.0f);

        g.setGradientFill (on
            ? juce::ColourGradient (juce::Colour (0xffe4123a), r.getX(), r.getY(), juce::Colour (0xff8f0c21), r.getX(), r.getBottom(), false)
            : juce::ColourGradient (juce::Colour (0xfff6f6f8), r.getX(), r.getY(), juce::Colour (0xffc6cad0), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 5.0f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.45f));
            g.drawRoundedRectangle (r.expanded (1.5f), 6.0f, 2.5f);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.10f));
            g.drawLine (r.getX() + 3.0f, r.getY() + 1.0f, r.getRight() - 3.0f, r.getY() + 1.0f, 1.0f);
        }

        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        g.setColour (on ? juce::Colours::white : juce::Colour (0xff33363c));
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
        g.drawText (noteName (key), r.toNearestInt().removeFromBottom (26), juce::Justification::centred);
    }

    void drawBlack (juce::Graphics& g, juce::Rectangle<float> r, int key)
    {
        const bool on = (key == current);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (r.translated (0, 2.5f), 3.5f);

        g.setGradientFill (on
            ? juce::ColourGradient (juce::Colour (0xffe4123a), r.getX(), r.getY(), juce::Colour (0xff7c0b1d), r.getX(), r.getBottom(), false)
            : juce::ColourGradient (juce::Colour (0xff35373d), r.getX(), r.getY(), juce::Colour (0xff0e0f12), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.5f);

        g.setColour (juce::Colours::white.withAlpha (on ? 0.30f : 0.14f));   // verniz no topo
        g.fillRoundedRectangle (r.withHeight (r.getHeight() * 0.16f).reduced (2.0f, 1.5f), 2.0f);

        if (on)
        {
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.55f));
            g.drawRoundedRectangle (r.expanded (1.0f), 4.5f, 1.5f);
        }

        g.setColour (on ? juce::Colours::white : juce::Colours::white.withAlpha (0.78f));
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText (noteName (key), r.toNearestInt().removeFromBottom (18), juce::Justification::centred);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        titleBounds = area.removeFromTop (22);
        area.removeFromTop (10);
        displayBounds = area.removeFromTop (62).toFloat();
        area.removeFromTop (16);

        auto bottom = area.removeFromBottom (54);
        auto volArea = bottom.removeFromLeft (76);
        volLbl.setBounds (volArea.removeFromBottom (12));
        vol.setBounds (volArea.withSizeKeepingCentre (44, 42));
        stopBtn.setBounds (bottom.removeFromRight (110).withSizeKeepingCentre (110, 36));
        area.removeFromBottom (16);

        auto kb = area.toFloat();
        const float ww = kb.getWidth() / 7.0f;
        for (int w = 0; w < 7; ++w)
            whiteRects[w] = juce::Rectangle<float> (kb.getX() + (float) w * ww + 2.0f, kb.getY(), ww - 4.0f, kb.getHeight());

        const float bw = ww * 0.60f, bh = kb.getHeight() * 0.62f;
        static const int afterWhite[5] = { 0, 1, 3, 4, 5 };
        for (int i = 0; i < 5; ++i)
            blackRects[i] = juce::Rectangle<float> (kb.getX() + (float) (afterWhite[i] + 1) * ww - bw * 0.5f, kb.getY(), bw, bh);
    }

    PadProcessor* pad;
    juce::Label volLbl;
    juce::TextButton stopBtn;
    juce::Slider vol;
    int current = -1;

    juce::Rectangle<int> titleBounds;
    juce::Rectangle<float> displayBounds;
    juce::Rectangle<float> whiteRects[7], blackRects[5];
    static constexpr int kWhite[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int kBlack[5] = { 1, 3, 6, 8, 10 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPanel)
};
#endif  // bloco antigo do PadPanel

void MainComponent::showPadEditor (int sourceChannel,
                                   const juce::String& sourceName)
{
    if (padEditorPanel == nullptr || padSynthProcessor == nullptr)
    {
        padButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    if (desktopWorkspace != DesktopWorkspace::mixer)
        showDesktopWorkspace (DesktopWorkspace::mixer);

    if (mapButton.getToggleState() || midiLearnActive.load())
        finishMidiLearn (true);
    if (liveButton.getToggleState())
        liveButton.setToggleState (false, juce::dontSendNotification);
    liveScenePanel.setVisible (false);

    padEditorVisible = true;
    padButton.setToggleState (true, juce::dontSendNotification);
    // A central PAD sempre recebe o Church Live atual. O no pode ter sido
    // recriado por restore/troca de biblioteca desde a construcao da janela;
    // guardar o ponteiro antigo faria a lista de samples e os attachments
    // editarem um processador que ja nao toca.
    padEditorPanel->setSampleProcessor (getChurchLive());
    padEditorPanel->setSourceChannel (sourceChannel, sourceName);
    padEditorPanel->refreshFromProcessor();
    padEditorPanel->setVisible (true);
    oled.setVisible (false);
    resized();
    padEditorPanel->toFront (false);
    if (quickMidiMappingMode)
        midiLearnPanel.toFront (false);
    repaint();
}

void MainComponent::closePadEditor()
{
    if (! padEditorVisible && (padEditorPanel == nullptr
                               || ! padEditorPanel->isVisible()))
    {
        padButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    padEditorVisible = false;
    padButton.setToggleState (false, juce::dontSendNotification);
    if (padEditorPanel != nullptr)
        padEditorPanel->setVisible (false);

    if (padPanel != nullptr && padPanel->isVisible())
    {
        padPanel->setVisible (false);
        if (padPanel->onClose != nullptr)
            padPanel->onClose();
    }

    oled.setVisible (! mapButton.getToggleState()
                     && ! liveScenePanel.isVisible());
    (void) saveChainState();
    resized();
    repaint();
}

void MainComponent::showDesktopWorkspace (DesktopWorkspace workspace)
{
    juce::ignoreUnused (workspace);
    desktopWorkspace = DesktopWorkspace::mixer;
    if (performanceSessionPanel != nullptr)
        performanceSessionPanel->setVisible (false);

    if ((! dockedNative || dockedEditor == nullptr) && slots[0].nodeID.uid != 0)
        openEditorForSlot (0);

    resized();
    repaint();
}

void MainComponent::openSidebarTimbreCategory (int categoryIndex)
{
   #if CHURCHDAW_MOBILE_SHELL
    juce::ignoreUnused (categoryIndex);
   #else
    int layer = 0;
    if (auto* live = getChurchLive())
    {
        layer = juce::jmax (0, live->getSelectedLayer());
        live->setSelectedLayer (layer);
    }

    showDesktopWorkspace (DesktopWorkspace::mixer);
    openEditorForSlot (0);

    // dockNode cria o editor no proximo ciclo da message thread. Enfileirar o
    // filtro depois dele garante que o atalho lateral abra o seletor real, ja
    // na categoria pedida, tanto se a mesa estava aberta quanto se estava em
    // Performance/Sessao.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this),
         layer, categoryIndex]
        {
            if (auto* self = safe.getComponent())
                if (auto* editor = dynamic_cast<TimbresEditor*> (
                        self->dockedEditor.get()))
                    editor->openTimbrePickerForLayer (layer, categoryIndex);
        });
   #endif
}

juce::String MainComponent::sceneHistoryFingerprint() const
{
    juce::XmlElement xml ("HISTORY");
    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        ChurchDAWLive::escreverCena (xml, cenas[(size_t) i], i);
    ChurchDAWLive::escreverSetlist (xml, setlist);
    return xml.toString();
}

void MainComponent::captureSceneHistoryPoint()
{
    if (applyingSceneHistory)
        return;

    const auto fingerprint = sceneHistoryFingerprint();
    if (sceneHistoryCursor >= 0
         && sceneHistory[(size_t) sceneHistoryCursor].fingerprint == fingerprint)
        return;

    if (sceneHistoryCursor + 1 < (int) sceneHistory.size())
        sceneHistory.resize ((size_t) sceneHistoryCursor + 1);

    SceneHistoryEntry point;
    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        point.cenas[(size_t) i] = cenas[i];
    point.setlist = setlist;
    point.cenaAtiva = cenaAtiva;
    point.cenaEmPreparacao = cenaEmPreparacao;
    point.fingerprint = fingerprint;
    sceneHistory.push_back (std::move (point));

    // O histórico é uma ferramenta de ensaio, não pode crescer sem limite
    // durante uma tarde de preparação. Mantemos as 50 últimas alterações.
    constexpr size_t maxHistory = 50;
    if (sceneHistory.size() > maxHistory)
        sceneHistory.erase (sceneHistory.begin());
    sceneHistoryCursor = (int) sceneHistory.size() - 1;
}

void MainComponent::applySceneHistoryPoint (const SceneHistoryEntry& point)
{
    applyingSceneHistory = true;
    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        cenas[i] = point.cenas[(size_t) i];
    setlist = point.setlist;

    // A persistência usa a mesma escrita atômica das cenas normais. A cadeia
    // de plugins não é regravada aqui: Undo/Redo de sessão nunca pode tocar
    // em estados binários nem interromper o áudio.
    const bool saved = salvarCenas();
    liveScenePanel.atualizar (cenas, cenaAtiva);
    refreshPerformanceSessionPanel();
    applyingSceneHistory = false;

    if (! saved)
        juce::Logger::writeToLog (
            "LIVE HISTORY: alteracao aplicada em memoria, mas nao gravada no disco");
}

void MainComponent::undoSceneHistory()
{
    if (sceneHistoryCursor <= 0
         || sceneHistoryCursor >= (int) sceneHistory.size())
        return;

    --sceneHistoryCursor;
    applySceneHistoryPoint (sceneHistory[(size_t) sceneHistoryCursor]);
    juce::Logger::writeToLog ("LIVE HISTORY: desfazer");
}

void MainComponent::redoSceneHistory()
{
    if (sceneHistoryCursor < 0
         || sceneHistoryCursor + 1 >= (int) sceneHistory.size())
        return;

    ++sceneHistoryCursor;
    applySceneHistoryPoint (sceneHistory[(size_t) sceneHistoryCursor]);
    juce::Logger::writeToLog ("LIVE HISTORY: refazer");
}

void MainComponent::refreshPerformanceSessionPanel()
{
    if (performanceSessionPanel == nullptr || ! performanceSessionPanel->isVisible())
        return;

    captureSceneHistoryPoint();

    int key = -1;
    float padGain = 0.9f;
    if (padProcessor != nullptr)
    {
        key = padProcessor->getKey();
        padGain = padProcessor->getGain();
    }

    bool globalSustain = false;
    int transpose = 0;
    if (auto* live = getChurchLive())
    {
        globalSustain = live->getGlobalSustain();
        if (auto* parameter = live->getTransposeGeralParam())
            transpose = parameter->get();
    }

    performanceSessionPanel->updateState (
        cenas, setlist, cenaAtiva, cenaEmPreparacao,
        key, padGain, globalSustain, transpose);
    performanceSessionPanel->setHistoryAvailability (
        sceneHistoryCursor > 0,
        sceneHistoryCursor >= 0
            && sceneHistoryCursor + 1 < (int) sceneHistory.size());
}

void MainComponent::startMidiLearn()
{
    if (midiLearnActive.load())
        return;

    lastLearnInputValue = -1;
    // Switching workspaces may finish learning; do it before arming MIDI.
    if (desktopWorkspace != DesktopWorkspace::mixer)
        showDesktopWorkspace (DesktopWorkspace::mixer);
    if (padEditorVisible)
        closePadEditor();
    liveModeBeforeMidiLearn = liveButton.getToggleState();
    quickMidiMappingMode = false;
    resetMidiLearnTargetUnderCallbackBarrier (true);

    clearMapConfirmationArmed = false;
    clearMapButton.setButtonText (desktopReferenceLayout ? "Limpar mapas" : "Limpar");
    clearMapButton.setVisible (true);

    mapButton.setToggleState (true, juce::dontSendNotification);
    mapButton.setButtonText (desktopReferenceLayout ? "CONCLUIR MIDI" : "Concluir");
    mapButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffb32d2e));

    liveButton.setToggleState (false, juce::dontSendNotification);

    liveScenePanel.setVisible (false);
    oled.setVisible (false);
    midiLearnPanel.setVisible (true);
    midiMappingPanel.setVisible (false);
    midiMappingPanel.setLearning (false, false);
    midiLearnConsoleOverlay.setVisible (true);
    midiLearnConsoleOverlay.clearTarget();

    if (padPanel != nullptr)
        padPanel->setMidiLearnMode (true);

    midiLearnPanel.setMappingCount (getMidiMappingCount());
    midiLearnPanel.showChooseTarget();

    midiLearnConsoleOverlay.toFront (false);
    midiLearnPanel.toFront (false);
    clearMapButton.toFront (false);
    mapButton.toFront (false);

    juce::Logger::writeToLog ("MIDI MAP: modo de aprendizado visual aberto sobre o console");

    resized();
    repaint();

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (auto* c = safe.getComponent())
        {
            c->resized();
            c->repaint();
        }
    });
}

void MainComponent::finishMidiLearn (bool saveMappings)
{
    if (! midiLearnActive.load() && ! mapButton.getToggleState())
        return;

    quickMidiMappingMode = false;
    {
        const juce::ScopedLock callbackDrain (midiCallbackBarrier);
        // Commit input already received before Concluir, then disarm. Clearing
        // the queue first lost the last mapping when these events were adjacent.
        consumeMidiLearnControlEvents();
        resetMidiLearnTargetUnderCallbackBarrier (false);
    }

    if (saveMappings)
    {
        limparBotoesDeCCVazios();
        atualizarBotoesDeCCDoPad();
        // Keep CC and note files in the same durable transaction.
        const bool salvouPar = saveMidiMapPair();
        if (! salvouPar)
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon, "Mapeamentos MIDI",
                "Os mapas funcionam nesta sessao, mas nao puderam ser salvos. "
                "Verifique o acesso ao disco e tente concluir novamente.");
        (void) saveChainState();
        if (auto* userSettings = appProperties.getUserSettings())
            userSettings->saveIfNeeded();

        juce::Logger::writeToLog (salvouPar ? "MIDI MAP: mapas salvos com sucesso ao concluir"
                                            : "MIDI MAP: aviso ao salvar par ao concluir");
    }

    clearMapConfirmationArmed = false;
    clearMapButton.setButtonText (desktopReferenceLayout ? "Limpar mapas" : "Limpar");
    clearMapButton.setVisible (false);

    mapButton.setToggleState (false, juce::dontSendNotification);
    mapButton.setButtonText (desktopReferenceLayout ? "MAPEAR MIDI" : "Mapear MIDI");
    mapButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2c171d));

    liveButton.setToggleState (liveModeBeforeMidiLearn, juce::dontSendNotification);
    liveScenePanel.setVisible (! desktopReferenceLayout && liveModeBeforeMidiLearn);
    oled.setVisible (! liveScenePanel.isVisible());
    midiLearnPanel.setVisible (false);
    midiMappingPanel.setVisible (false);
    midiMappingPanel.setLearning (false, false);
    midiLearnConsoleOverlay.setVisible (false);
    midiLearnConsoleOverlay.clearTarget();

    if (padPanel != nullptr)
        padPanel->setMidiLearnMode (false);

    juce::Logger::writeToLog ("MIDI MAP: concluido com "
                              + juce::String (getMidiMappingCount()) + " mapeamento(s)");

    resized();
    repaint();

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (auto* c = safe.getComponent())
        {
            c->resized();
            c->repaint();
        }
    });
}

void MainComponent::toggleMidiLearn()
{
    if (midiLearnActive.load() || mapButton.getToggleState())
        finishMidiLearn (true);
    else
        startMidiLearn();
}

void MainComponent::showPadWindow()
{
    juce::Logger::writeToLog (
        "PAD_CONTINUO: showPadWindow visible="
        + juce::String (padPanel != nullptr && padPanel->isVisible() ? 1 : 0));

    if (padPanel == nullptr)
        return;

    const bool show = ! padPanel->isVisible();

    if (show && (padProcessor == nullptr || ! PadProcessor::padsAvailable()))
    {
        padButton.setToggleState (padEditorVisible, juce::dontSendNotification);

       #if CHURCHDAW_MOBILE_SHELL
        const auto help = juce::String::fromUTF8 (
            "Pad Cont\xc3\xadnuo n\xc3\xa3o instalado.\n\n"
            "Instale ou repare o pack Pad Cont\xc3\xadnuo em Bibliotecas "
            "e abra esta tela novamente.");
       #else
        const auto help = juce::String::fromUTF8 (
            "Pad Cont\xc3\xadnuo n\xc3\xa3o instalado.\n\n"
            "Baixe o pack em Bibliotecas para tocar agora, ou coloque seus "
            "arquivos WAV na pasta 'ChurchDAW Pads'.");
       #endif

        auto options = juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle (juce::String::fromUTF8 ("Pad Cont\xc3\xadnuo"))
            .withMessage (help)
            .withButton ("Abrir Bibliotecas")
            .withButton (juce::String::fromUTF8 ("Agora n\xc3\xa3o"))
            .withAssociatedComponent (this);

        juce::AlertWindow::showAsync (
            options,
            [safe = juce::Component::SafePointer<MainComponent> (this)] (int result)
            {
                if (result == 1)
                    if (auto* self = safe.getComponent())
                        self->showLibraries();
            });
        return;
    }

    padPanel->setVisible (show);
    padButton.setToggleState (padEditorVisible || show,
                              juce::dontSendNotification);

    if (show)
    {
        padPanel->refreshState();
        resized();
        padPanel->toFront (true);
        if (quickMidiMappingMode)
            midiLearnPanel.toFront (false);
        juce::Logger::writeToLog ("PAD_CONTINUO: painel aberto");
    }
}

//==============================================================================
bool MainComponent::saveChainState()
{
    if (safeMode)
    {
        juce::Logger::writeToLog (
            "SAFE_MODE: cadeia salva preservada; nenhuma sobrescrita permitida");
        return true;
    }

    int slotsQueNaoSalvaram = 0;
    pluginsQueNaoSalvaram.clear();

    juce::XmlElement chain ("CHAIN");
    chain.setAttribute (
        "inputEnabled",
        audioInputAuthorised.load (std::memory_order_acquire));
    chain.setAttribute ("masterDb", masterSlider.getValue());
    chain.setAttribute ("masterAutoEq", masterProcessor->getAutoEq());
    chain.setAttribute ("masterPunchSchema", 1);
    chain.setAttribute ("masterPunch", masterProcessor->getPunch());

    if (padProcessor != nullptr)
    {
        juce::MemoryBlock padState;
        padProcessor->getStateInformation (padState);

        if (! padState.isEmpty())
            chain.setAttribute ("padState", padState.toBase64Encoding());
    }

    if (padSynthProcessor != nullptr)
    {
        juce::MemoryBlock synthState;
        padSynthProcessor->getStateInformation (synthState);
        if (! synthState.isEmpty())
            chain.setAttribute ("padSynthState", synthState.toBase64Encoding());
    }

    for (int i = 0; i < numSlots; ++i)
    {
        auto* slotXml = chain.createNewChildElement ("SLOT");
        slotXml->setAttribute ("index", i);
        slotXml->setAttribute ("id", slots[i].pluginIdentifier);

        if (slots[i].nodeID.uid != 0)
        {
            if (auto* node = graph.getNodeForId (slots[i].nodeID))
            {
                juce::MemoryBlock mb;
                auto* processor = node->getProcessor();
                auto* live = dynamic_cast<TimbresProcessor*> (processor);
                const bool wasSuspended = processor->isSuspended();
                const bool shouldSuspendProcessor =
                    live == nullptr || live->requiresSuspensionForStateCapture();
                bool captured = false;

                // Plugins de terceiros e o estado XML do Church Organ exigem
                // suspensao breve. Ja um setup puramente nativo de samples/SF2
                // (caso padrao do Android/iPhone) le apenas atomics e strings sob
                // layerIdentityLock, sem jamais emudecer o som durante o autosave.
                if (shouldSuspendProcessor)
                    processor->suspendProcessing (true);

                try
                {
                    if (live != nullptr)
                    {
                        // TimbresProcessor serializa internamente troca de
                        // timbre + ENV/EQ + identidade. Nao inverta a ordem
                        // desse lock com callbackLock durante autosave.
                        processor->getStateInformation (mb);
                        captured = ! live->consumeLastStateCaptureHadFailure();
                    }
                    else
                    {
                        const juce::ScopedLock lock (
                            processor->getCallbackLock());
                        processor->getStateInformation (mb);
                        captured = true;
                    }
                }
                catch (...)
                {
                    // Um plugin defeituoso nao pode impedir o restante da cadeia
                    // de ser salvo nem deixar o audio permanentemente suspenso.
                    if (live != nullptr)
                        (void) live->consumeLastStateCaptureHadFailure();
                }

                if (shouldSuspendProcessor)
                    processor->suspendProcessing (wasSuspended);

                if (captured)
                {
                    slotXml->setAttribute ("state", mb.toBase64Encoding());
                }
                else
                {
                    // O catch acima existe para nao derrubar o resto — mas
                    // engolir sem dizer nada quer dizer que o usuario perde os
                    // ajustes DESTE plugin sem nunca saber. O nome no log e o
                    // que permite descobrir qual e o culpado.
                    ++slotsQueNaoSalvaram;
                    pluginsQueNaoSalvaram.addIfNotAlreadyThere (processor->getName());
                    juce::Logger::writeToLog ("SETUP: nao consegui salvar o estado de \""
                                                + processor->getName() + "\" (slot "
                                                + juce::String (i) + ")");
                }
            }
        }
        else if (slots[i].pendingIdentifier.isNotEmpty())
        {
            // plugin ausente nesta maquina: re-emite id/estado salvos pra nao
            // apagar o setup do usuario no proximo autosave
            slotXml->setAttribute ("id", slots[i].pendingIdentifier);
            slotXml->setAttribute ("state", slots[i].pendingState);
        }
    }

    appProperties.getUserSettings()->setValue ("chain", &chain);

    // saveIfNeeded devolve true tambem quando nao havia o que gravar — que e o
    // que queremos: "nada mudou" nao e falha. False aqui significa que o
    // arquivo REALMENTE nao foi para o disco.
    const bool escreveu = appProperties.getUserSettings()->saveIfNeeded();

    if (! escreveu)
        juce::Logger::writeToLog ("SETUP: FALHA ao gravar o arquivo de configuracao");

    // SO O ARQUIVO decide o retorno.
    //
    // Antes eu misturava aqui "o arquivo nao gravou" com "um plugin nao
    // serializou". Sao coisas diferentes: a primeira e falha dura e some
    // sozinha quando o disco volta; a segunda pode ser PERMANENTE — um VST
    // antigo que o musico deixou carregado e nem usa prende o botao em
    // "⚠ FALHOU" para sempre, sem dizer qual e o culpado, e o indicador de
    // confianca perde o valor exatamente onde ele foi criado para ter valor.
    //
    // Quem quiser saber dos plugins pergunta em pluginsQueNaoSalvaram.
    return escreveu;
}

void MainComponent::restoreChainState()
{
    auto xml = appProperties.getUserSettings()->getXmlValue ("chain");

    if (xml == nullptr || ! xml->hasTagName ("CHAIN"))
    {
        // PRIMEIRA EXECUCAO (sem setup salvo): a DAW nunca abre vazia. Sobe o
        // Church Live com o preset de fabrica piano + pad + strings -- o preset
        // em si so aplica depois do scan, e so se nada tiver sido carregado.
        slots[0].pluginIdentifier = timbresIdentifiers[0];
        loadInternalTimbres (0, nullptr, 0);

        if (auto* live = getChurchLive())
            live->requestShowroomPreset();

        return;
    }

    const bool restoredInputAuthorisation =
        xml->getBoolAttribute ("inputEnabled", false);
    audioInputAuthorised.store (restoredInputAuthorisation,
                                std::memory_order_release);
    inputToggle.setToggleState (restoredInputAuthorisation,
                                juce::dontSendNotification);

    // Estado com master em -infinito e ARTEFATO, nao intencao: o fade da
    // licenca (beginLicenseFadeOut) derrubava o master para -60 e o
    // destrutor salvava esse valor — o cliente reabria o app MUDO e achava
    // que estava quebrado. Regra do dono: padrao e 0 dB. Isso tambem cura
    // estados ja envenenados por versoes antigas na proxima abertura.
    auto masterDb = (float) xml->getDoubleAttribute ("masterDb", 0.0);

    if (masterDb <= MasterProcessor::minusInfinityDb + 0.5f)
    {
        juce::Logger::writeToLog (
            "ESTADO: masterDb salvo em -inf (fade de licenca?); "
            "restaurado para 0 dB");
        masterDb = 0.0f;
    }

    masterSlider.setValue (masterDb, juce::dontSendNotification);
    masterProcessor->setMasterDb (masterDb);
    masterProcessor->setAutoEq (xml->getBoolAttribute ("masterAutoEq", false));
    // Builds antigos gravavam Punch=true por ser o default, mesmo sem escolha
    // do usuario. Somente a nova schema representa uma escolha confiavel.
    masterProcessor->setPunch (
        xml->getIntAttribute ("masterPunchSchema", 0) >= 1
        && xml->getBoolAttribute ("masterPunch", false));

    if (padProcessor != nullptr && xml->hasAttribute ("padState"))
    {
        juce::MemoryBlock padState;

        if (padState.fromBase64Encoding (xml->getStringAttribute ("padState"))
             && ! padState.isEmpty())
            padProcessor->setStateInformation (padState.getData(),
                                                (int) padState.getSize());
    }

    if (padSynthProcessor != nullptr && xml->hasAttribute ("padSynthState"))
    {
        juce::MemoryBlock synthState;
        if (synthState.fromBase64Encoding (
                xml->getStringAttribute ("padSynthState"))
             && ! synthState.isEmpty())
            padSynthProcessor->setStateInformation (
                synthState.getData(), (int) synthState.getSize());
    }

    // Estados de versoes anteriores podem trazer o engine ligado. O editor de
    // samples nao usa este motor; mantenha-o explicitamente silencioso depois
    // de qualquer restauracao local.
    if (padSynthProcessor != nullptr)
        padSynthProcessor->setEngineEnabled (false);

    bool slotZeroWasRequested = false;

    for (auto* slotXml : xml->getChildIterator())
    {
        const int i = slotXml->getIntAttribute ("index", -1);
        const auto id = slotXml->getStringAttribute ("id");

        if (i < 0 || i >= numSlots || id.isEmpty())
            continue;

        if (i == 0)
            slotZeroWasRequested = true;

        int internalLib = -1;

        for (int lib = 0; lib < numInternalLibs; ++lib)
            if (id == timbresIdentifiers[lib])
                internalLib = lib;

        if (internalLib >= 0)
        {
            juce::MemoryBlock state;
            state.fromBase64Encoding (slotXml->getStringAttribute ("state"));

            slots[i].pluginIdentifier = id;
            loadInternalTimbres (i, &state, internalLib);
            continue;
        }

        if (auto desc = knownPlugins.getTypeForIdentifierString (id);
            desc != nullptr
            && ! ChurchDAWPluginScan::estaNaBlacklist (
                knownPlugins, desc->fileOrIdentifier, true)
            && ! pluginScanQuarantine.contains (
                desc->fileOrIdentifier, true))
        {
            juce::MemoryBlock state;
            state.fromBase64Encoding (slotXml->getStringAttribute ("state"));

            slots[i].pluginIdentifier = id;
            loadPlugin (i, *desc, &state);
        }
        else
        {
            // plugin fora da lista (desinstalado/scan pendente): guarda id e
            // estado pro saveChainState re-emitir em vez de perder o setup
            slots[i].pendingIdentifier = id;
            slots[i].pendingState = slotXml->getStringAttribute ("state");
        }
    }

    // Um CHAIN valido, mas com o slot de instrumento vazio, pode surgir apos
    // migracao, queda durante autosave ou uma versao antiga que gravava a
    // cadeia antes do Church Live terminar de subir. A interface principal da
    // ChurchDAW e a mesa: reabrir numa cadeia generica vazia contradiz o fluxo
    // "pronto para tocar" e ainda impede a cena ativa de ser reconciliada.
    // Plugins realmente ausentes continuam preservados acima como pending; o
    // fallback vale apenas para um slot 0 explicitamente vazio.
    if (! slotZeroWasRequested)
    {
        juce::Logger::writeToLog (
            "SETUP: slot de instrumento vazio; restaurando Church Live nativo");
        slots[0].pluginIdentifier = timbresIdentifiers[0];
        loadInternalTimbres (0, nullptr, 0);

        if (auto* live = getChurchLive())
            live->requestShowroomPreset();
    }
}
