#include "MainComponent.h"
#include "NordLookAndFeel.h"
#include "TimbresProcessor.h"
#include "ChordDetect.h"
#include "AboutPanel.h"
#include "LibraryPanel.h"
#include "MidiMappingSafety.h"
#include "AndroidCultoConnectivity.h"
#if JUCE_ANDROID
 #include "AndroidAudioMidiCenter.h"
#endif
#include "UiProfiler.h"
#include "BinaryData.h"

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
        setUsingNativeTitleBar (true);

        if (auto* editor = node->getProcessor()->createEditorIfNeeded())
            setContentOwned (editor, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (*node->getProcessor()), true);

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
MainComponent::MainComponent()
{
    setOpaque (true);

   #if JUCE_ANDROID || JUCE_IOS
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
    appProperties.setStorageParameters (options);

   #if CHURCHDAW_HOSTS_PLUGINS
    juce::addDefaultFormatsToManager (formatManager);

    // Cada plugin e aberto num processo filho. Um VST que trava (o "Piano
    // V3" no Mac do cliente, 31/07) derruba so o filho: a varredura marca
    // aquele arquivo e continua, em vez de fechar o ChurchDAW no meio.
    knownPlugins.setCustomScanner (
        std::make_unique<ChurchDAWPluginScan::ChurchDAWPluginScanner>());

    if (auto xml = appProperties.getUserSettings()->getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

    auto* pluginSettings = appProperties.getUserSettings();
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

    if (migratedBlacklist > 0)
    {
        if (auto migratedXml = knownPlugins.createXml())
            pluginSettings->setValue ("pluginList", migratedXml.get());
    }

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
         || migratedBlacklist > 0 || migratedDeadMans > 0)
        juce::Logger::writeToLog (
            "PLUGIN_SCAN legacy_blacklist_pending retry="
            + juce::String (legacyBlacklistCount)
            + " legacy_dead_mans=" + juce::String (legacyDeadMansCount)
            + " policy_blacklist=" + juce::String (migratedBlacklist)
            + " policy_dead_mans=" + juce::String (migratedDeadMans));

    knownPlugins.addChangeListener (this);
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

    player.setProcessor (&graph);

    auto savedAudio = appProperties.getUserSettings()->getXmlValue ("audioSetup");

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
    const int numInputs = 0;
   #else
    // No desktop a entrada e opcional e so entra no graph quando o usuario liga
    // "Entrada de audio (guitarra/mic)" no painel Audio/MIDI.
    const int numInputs = 2;
   #endif

    const auto audioInitialiseError =
        deviceManager.initialise (numInputs, 2, savedAudio.get(), true);

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
        const auto rate  = dev->getCurrentSampleRate() > 0.0 ? dev->getCurrentSampleRate() : 48000.0;

        int chosen = 0;

        for (const auto s : sizes)
            if (((double) s / rate) * 1000.0 >= 12.0)
            {
                chosen = s;
                break;
            }

        if (chosen == 0 && ! sizes.isEmpty())
            chosen = sizes.getLast();      // aparelho so oferece buffers curtos

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
    deviceManager.addAudioCallback (&player);
    deviceManager.addChangeListener (this);

    masterProcessor->addChangeListener (this);   // sincroniza o fader com o Church Live
    masterProcessor->addListener (this);         // MIDI Learn do master por parametro

    // Habilita os dispositivos MIDI conectados, MENOS a porta DAW do Launchkey.
    //
    // Ela nao e um teclado: em modo DAW carrega pads e controles de superficie.
    // Deixando ela entrar como MIDI comum, apertar um pad tocava uma NOTA no
    // instrumento. Pior: o Windows abre porta MIDI em modo exclusivo, entao com o
    // deviceManager segurando a porta o LaunchkeyControl nao conseguia abri-la e
    // os faders/knobs/pads nem chegavam ao mapeamento.
    refreshMidiInputs();

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
    const auto safeThis = juce::Component::SafePointer<MainComponent> (this);
    midiListConnection = juce::MidiDeviceListConnection::make ([safeThis]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (auto* c = safeThis.getComponent())
                c->refreshMidiInputs();
        });
    });

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
    restoreMidiMap();
    restoreNoteMap();
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
    liveButton.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
    liveButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.88f));
    liveButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    liveButton.onClick = [this]
    {
        const bool aberto = liveButton.getToggleState();

        // Nunca com o aprendizado ligado: os dois disputam a mesma faixa.
        if (aberto && mapButton.getToggleState())
            mapButton.setToggleState (false, juce::sendNotificationSync);

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
            const auto available = mobileDevice->getAvailableBufferSizes();
            const double rate = mobileDevice->getCurrentSampleRate() > 0.0
                                    ? mobileDevice->getCurrentSampleRate()
                                    : 48000.0;

            for (const int candidate : available)
                if (((double) candidate / rate) * 1000.0 >= 12.0)
                {
                    preferredMobileBuffer = candidate;
                    break;
                }

            if (preferredMobileBuffer <= 0 && ! available.isEmpty())
                preferredMobileBuffer = available.getLast();
        }

        if (preferredMobileBuffer > 0)
        {
           #if JUCE_ANDROID
            // Schema 2: sobe o piso de 8 ms -> 12 ms e reaplica uma vez em
            // aparelhos que ficaram em buffer baixo com muitos xruns.
            auto* settings = appProperties.getUserSettings();
            if (settings->getIntValue ("androidAdaptiveBufferSchema", 0) < 2)
            {
                wantedBuffer = preferredMobileBuffer;
                settings->setValue ("androidAdaptiveBufferSchema", 2);
                settings->setValue ("bufferSize", preferredMobileBuffer);
            }
           #endif

            if (wantedBuffer < preferredMobileBuffer)
                wantedBuffer = preferredMobileBuffer;
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

   #if JUCE_ANDROID
    androidLastXRunCount = deviceManager.getXRunCount();
    androidLastBufferTuneMs = juce::Time::getMillisecondCounter();
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
    padButton.setTooltip ("Abrir ou fechar o Pad Continuo");
    padButton.onClick = [this] { showPadWindow(); };

    // card do Pad Continuo EMBUTIDO na DAW (oculto ate clicar em "Pad")
    padPanel = std::make_unique<PadPanel> (padProcessor);
    padPanel->onClose = [this]
    {
        if (padPanel != nullptr)
            padPanel->setVisible (false);

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
    addChildComponent (*padPanel);

    addAndMakeVisible (saveButton);
    // vira um indicador de confianca: verde "Salvo" = autosave ligado, seu setup
    // esta guardado. Clicar forca salvar tudo e pisca "Salvo!"
    // Guardado para poder VOLTAR ao verde depois de um "FALHOU" vermelho.
   #if JUCE_ANDROID || JUCE_IOS
    corDoSalvoNormal = juce::Colour (0xff5fd08a).withAlpha (0.82f);
   #else
    corDoSalvoNormal = juce::Colour (0xff5fd08a);
   #endif

   #if JUCE_ANDROID || JUCE_IOS
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
    saveButton.setTooltip ("Projeto salvo. Clique para salvar novamente agora.");
    saveButton.onClick = [this]
    {
        // salva TUDO agora: cadeia (timbres/volumes/efeitos), mapeamento MIDI e audio
        const bool setupOk = saveChainState();
        const bool ccsOk = saveMidiMap();
        const bool notasOk = saveNoteMap();
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

        const bool tudoOk = setupOk && audioOk
                              && ! ccsFalhou && ! notasFalhou && ! cenasFalhou;

       #if JUCE_ANDROID || JUCE_IOS
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

               #if JUCE_ANDROID || JUCE_IOS
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
    instrumentsButton.onClick = [this] { undockEditor(); };   // fecha a tela cheia do Church Live

    // chips premium do header (controlador MIDI + interface); a entrada de
    // guitarra/mic saiu do header e agora mora no dialogo Audio/MIDI
    addAndMakeVisible (deviceBar);
    inputToggle.setButtonText (juce::String::fromUTF8 ("Entrada de \xc3\xa1udio (guitarra/mic)"));   // áudio
    inputToggle.onClick = [this]
    {
        rebuildGraphConnections();
        saveChainState();
    };

    addAndMakeVisible (mapButton);
    mapButton.setClickingTogglesState (true);
    // AMBAR, nao vermelho: ligado ele ficava igualzinho ao "Instrumentos", e os
    // dois querem dizer coisas diferentes (pagina atual x modo armado). Ambar ja
    // e a cor de "atencao/temporario" no resto do app.
    mapButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0a83a));
    mapButton.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff1a1206));
    mapButton.onClick = [this]
    {
        const bool on = mapButton.getToggleState();
        midiLearnActive.store (on);
        lastTouchedSlot.store (-2);
        lastTouchedParam.store (-1);
        lastTouchedInsert.store (-1);
        {
            const juce::ScopedLock sl (mapLock);
            lastTouchedParamName.clear();
            lastTouchedParamId.clear();
            pendingNoteAction = {};
        }
        clearMapConfirmationArmed = false;
        clearMapButton.setButtonText ("Limpar mapas");
        clearMapButton.setVisible (on);
        mapButton.setButtonText (on ? "Concluir" : "Mapear MIDI");
        // Os tres ocupantes da faixa de programa sao mutuamente exclusivos.
        // Entrar no aprendizado fecha o Live Mode: sao dois modos de ensaio
        // diferentes e ver os dois ao mesmo tempo so confunde.
        if (on)
            liveButton.setToggleState (false, juce::dontSendNotification);

        liveScenePanel.setVisible (! on && liveButton.getToggleState());
        oled.setVisible (! on && ! liveScenePanel.isVisible());
        midiLearnPanel.setVisible (on);

        if (padPanel != nullptr)
            padPanel->setMidiLearnMode (on);

        if (on)
        {
            midiLearnPanel.setMappingCount (getMidiMappingCount());
            midiLearnPanel.showChooseTarget();
            juce::Logger::writeToLog ("MIDI MAP: modo de aprendizado aberto");
        }
        else
        {
            juce::Logger::writeToLog ("MIDI MAP: concluido com "
                                      + juce::String (getMidiMappingCount()) + " mapeamento(s)");
        }

        // o layout do header muda no modo de mapeamento (abre espaco pra
        // instrucao e pro "Limpar map."); adiado pra nao rearranjar componentes
        // com o handler do clique ainda na pilha
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* c = safe.getComponent())
                c->resized();
        });
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

        {
            const juce::ScopedLock sl (mapLock);
            ccMappings.clear();
            noteMappings.clear();
            pendingNoteAction = {};

            // LIMPAR O LIVRO DE NOTAS SUPRIMIDAS TAMBEM.
            //
            // Sem isto, o unico caminho de recuperacao que o app oferece podia
            // deixar uma nota tocando PARA SEMPRE: se um note-off tivesse se
            // perdido (falha de USB, Bluetooth caindo, sequenciador parado no
            // meio da nota), a entrada ficava obsoleta no livro. O musico entao
            // limpava os mapas para recuperar a tecla, tocava nela — o note-on
            // passava, mas o note-off batia na entrada velha e era engolido.
            // Tecla presa no meio do culto, com o PANIC como unica saida.
            //
            // O livro so faz sentido enquanto existem mapas. Zerando junto,
            // "Limpar mapas" volta a ser o que promete: um recomeco.
            notasSuprimidas.clear();
            reconstruirMascaraDeSuprimidas();

            lastTouchedParamName.clear();
            lastTouchedParamId.clear();
        }

        reconstruirMascaraDeNotas();
        atualizarEtiquetasDoPad();
        lastTouchedSlot.store (-2);
        lastTouchedParam.store (-1);
        lastTouchedInsert.store (-1);
        lastMappedCC.store (-1);
        lastMappedNote.store (-1);
        clearMapConfirmationArmed = false;
        clearMapButton.setButtonText ("Limpar mapas");
        ccPickup.clear();

        // Os DOIS mapas precisam ter ido para o disco; senao o que sobrou
        // volta inteiro na proxima abertura.
        const bool ccsApagados = saveMidiMap();
        const bool apagouNoDisco = saveNoteMap() && ccsApagados;
        deviceBar.setMappingCount (0);
        midiLearnPanel.setMappingCount (0);

        // Se o disco recusou, o mapa antigo VOLTA INTEIRO na proxima abertura —
        // inclusive a tecla mapeada por engano que motivou a limpeza. O musico
        // precisa saber disso agora, e nao no domingo.
        if (apagouNoDisco)
        {
            midiLearnPanel.showChooseTarget();
            juce::Logger::writeToLog ("MIDI MAP: todos os mapeamentos foram apagados");
        }
        else
        {
            midiLearnPanel.showAviso (
                juce::String::fromUTF8 ("Limpou nesta sess\xc3\xa3o, mas N\xc3\x83O no disco"),
                juce::String::fromUTF8 ("Os mapas antigos voltam ao reabrir o app"));
            juce::Logger::writeToLog ("MIDI MAP: limpeza NAO gravada; o mapa antigo "
                                      "volta na proxima abertura");
        }
    };

    addChildComponent (midiLearnPanel);

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
    brandIcon = juce::ImageCache::getFromMemory (BinaryData::icon_png,
                                                 BinaryData::icon_pngSize);
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
    startTimer (1000);

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
            lastTouchedSlot.store (-1);   // -1 = Master
            lastTouchedParam.store (-1);
            triggerAsyncUpdate();
        }
    };
    masterSlider.onDragEnd = [this] { saveChainState(); };
    addAndMakeVisible (masterSlider);

    meter = std::make_unique<LevelMeter> (2,
        [mp = masterProcessor] (int ch) { return mp->readAndResetLevel (ch); },
        [mp = masterProcessor]          { return mp->readAndResetClip(); });
    addAndMakeVisible (*meter);

    // ---- painel de plugin embutido ----
    dockTitle.setJustificationType (juce::Justification::centredLeft);
    addChildComponent (dockTitle);
    addChildComponent (dockViewport);
    dockViewport.setViewedComponent (&dockHolder, false);
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
    restoreChainState();
    rebuildGraphConnections();

    // O XML so informa qual pagina estava ativa; ele nao prova que a pagina
    // correspondente chegou ao motor neste processo. Reconcilia depois que o
    // construtor terminou e deixa o painel sem destaque ate o SHA ser aplicado.
    if (cenaPendenteNoStartup >= 0)
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (auto* self = safe.getComponent())
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

    // O escaneador de plugins nunca abre sozinho. Em uma instalacao limpa ele
    // cobria a mesa inteira logo na primeira abertura e fazia o ChurchDAW parecer
    // quebrado, especialmente no macOS. O usuario continua podendo abri-lo pelo
    // botao "Plugins" / "Escanear VST3" quando realmente quiser instalar VST/AU.
}

MainComponent::~MainComponent()
{
    pluginLoadLifetime->store (false);

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

    librariesPanel.reset();  // encerra rede/download antes de desmontar os processadores
    aboutPanel.reset();
    saveChainState();

    masterProcessor->removeChangeListener (this);
    masterProcessor->removeListener (this);

    if (padProcessor != nullptr)
        padProcessor->juce::AudioProcessor::removeListener (this);   // par do addListener do construtor

    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeAudioCallback (&player);
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
    graph.clear();

    appProperties.getUserSettings()->saveIfNeeded();
    appProperties.closeFiles();
}

void MainComponent::beginLicenseFadeOut()
{
    if (masterProcessor != nullptr)
        masterProcessor->setMasterDb (MasterProcessor::minusInfinityDb);
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
    auto headerArea = getLocalBounds().removeFromTop (
        juce::jmin (getHeight(), layoutSafeTop + layoutHeaderHeight));
    const int headerContentY = layoutContentBounds.getY();
    const bool compactHeader = compactHeaderLayout;
    const bool narrowHeader = narrowHeaderLayout;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff24272e), 0.0f, 0.0f,
                                             juce::Colour (0xff15161a), 0.0f, (float) headerArea.getBottom(), false));
    g.fillRect (headerArea);

    g.setColour (juce::Colours::white.withAlpha (0.06f));   // brilho de 1px no topo
    g.fillRect (headerArea.withHeight (1));

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<int> (0, headerArea.getBottom() - 4, getWidth(), 4));
    g.setColour (NordLookAndFeel::nordRed().withAlpha (0.86f));
    g.fillRect (juce::Rectangle<int> (0, headerArea.getBottom() - 1, getWidth(), 1));

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
        const float iconSize = mobileShortLandscapeLayout ? 24.0f
                                                          : compactHeader ? 32.0f : 46.0f;
        const float iconX = (float) layoutSafeLeft
                          + (mobileShortLandscapeLayout ? 5.0f
                                                       : compactHeader ? 8.0f
                                                                       : (narrowHeader ? 12.0f : 17.0f));
        const float iconRowH = mobilePortraitLayout ? 48.0f
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
        const float iconRowH = mobilePortraitLayout ? 48.0f
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
        g.drawImageWithin (brandIcon, draw.getX(), draw.getY(), draw.getWidth(), draw.getHeight(),
                           juce::RectanglePlacement::centred);
    }
   #endif

    if (! narrowHeader)
    {
        auto titleFont = juce::Font (juce::FontOptions (
                                mobileShortLandscapeLayout ? 15.0f
                                                           : compactHeader ? 18.0f : 22.0f))
                             .boldened().italicised();
        g.setFont (titleFont);

        juce::GlyphArrangement measure;
        measure.addLineOfText (titleFont, "Church", 0.0f, 0.0f);
        const int churchW = (int) std::ceil (measure.getBoundingBox (0, -1, true).getWidth());
        const int textX = layoutSafeLeft + (mobileShortLandscapeLayout ? 34
                                                                       : compactHeader ? 48
                                                                                       : 18 + 44 + 8);

        g.setColour (juce::Colours::white);
        g.drawText ("Church", textX, headerContentY, churchW + 4, layoutHeaderHeight,
                    juce::Justification::centredLeft);
        g.setColour (NordLookAndFeel::nordRedBright());   // "DAW" carrega a cor da marca
        g.drawText ("DAW", textX + churchW, headerContentY, 200, layoutHeaderHeight,
                    juce::Justification::centredLeft);
    }

    // separador vertical apos a marca
    const int separatorX = layoutSafeLeft + layoutBrandWidth + (compactHeader ? 4 : 8);
    const int separatorInset = mobileShortLandscapeLayout ? 4
                                                          : compactHeader ? 6 : 11;
    const int separatorY = headerContentY + separatorInset;
    const int separatorH = juce::jmax (
        0, (mobilePortraitLayout ? 48 : layoutHeaderHeight) - separatorInset * 2);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (separatorX, separatorY, 1, separatorH);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (separatorX + 1, separatorY, 1, separatorH);

    // chip de vidro atras do status (kHz / buffer / CPU) — combina com os chips
    // de dispositivo do header
    // CHAPA DE CHASSI do painel de programa. Na foto do Electro 7 o display esta
    // encaixado no corpo do instrumento, nao flutuando: sem esta chapa o visor
    // ficava solto num vazio preto e parecia um cartao jogado na tela.
    // liveScenePanel e o TERCEIRO ocupante desta faixa. Faltava aqui, e o Live
    // Mode desenhava direto sobre o fundo: um buraco retangular no meio do
    // painel, sem chapa, sem vinco e sem o fio vermelho.
    if (! programBand.isEmpty()
         && (oled.isVisible() || midiLearnPanel.isVisible() || liveScenePanel.isVisible()))
    {
        auto plate = programBand.toFloat();

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1e24), plate.getX(), plate.getY(),
                                                 juce::Colour (0xff101216), plate.getX(), plate.getBottom(),
                                                 false));
        g.fillRect (plate);

        // escovado vertical sutil: da materia a chapa sem virar textura chamativa
        g.setColour (juce::Colours::white.withAlpha (0.012f));

        for (float xx = plate.getX(); xx < plate.getRight(); xx += 3.0f)
            g.drawLine (xx, plate.getY(), xx, plate.getBottom(), 1.0f);

        // vincos de chassi em cima e embaixo
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (plate.getX(), plate.getBottom() - 2.0f, plate.getWidth(), 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
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
        auto chip = sb.toFloat().reduced (0.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.32f));
        g.fillRoundedRectangle (chip, NordLookAndFeel::radiusControl);
        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.drawRoundedRectangle (chip.reduced (0.5f), NordLookAndFeel::radiusControl, 1.0f);

        auto label = sb.reduced (8, 2).removeFromTop (10);
        g.setColour (juce::Colours::white.withAlpha (0.64f));
        g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold))
                       .withExtraKerningFactor (0.12f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x81UDIO ENGINE"), label,
                    juce::Justification::centred, false);
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
    const auto midiIns = juce::MidiInput::getAvailableDevices();

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
    int candidatasALigar = 0;
    for (auto& d : midiIns)
        if (! LaunchkeyControl::isDawPort (d.name))
            ++candidatasALigar;

    const bool filtroDerrubariaTudo = midiIns.size() > 0 && candidatasALigar == 0;

    if (filtroDerrubariaTudo)
        juce::Logger::writeToLog (
            "MIDI: o filtro de porta DAW desligaria TODAS as entradas — ignorado "
            "nesta varredura para o teclado nao ficar mudo");

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
        if (filtroDerrubariaTudo || ! LaunchkeyControl::isDawPort (d.name))
            agora.add (d.identifier);

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
        const bool daw = ! filtroDerrubariaTudo && LaunchkeyControl::isDawPort (d.name);
        deviceManager.setMidiInputDeviceEnabled (d.identifier, ! daw);

        // O filtro de porta DAW procura "MIDIIN2"/"MIDIOUT2"/"DAW", que sao
        // convencoes de nome do WINDOWS. Em outro sistema o mesmo teclado chega
        // com outro nome, entao registrar o nome REAL aqui e o que permite
        // descobrir se ele caiu no lado certo do filtro.
        juce::Logger::writeToLog ("MIDI IN: \"" + d.name + "\"  id=" + d.identifier
                                   + (daw ? "  -> DAW (desligada)" : "  -> LIGADA"));
    }

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

    if (alguemSaiu && ! primeiraVez)
        resetMidiTransportState();

    // As etiquetas do Pad dizem QUAL tecla comanda cada tom, e apagam quando o
    // teclado daquele mapa nao esta ligado. Trocar de controlador e exatamente
    // o momento em que essa informacao muda — sem isto, o painel continuaria
    // prometendo uma tecla do teclado que ficou em casa.
    if (listaMudou)
        atualizarEtiquetasDoPad();
}

void MainComponent::resetMidiTransportState()
{
    {
        const juce::ScopedLock sl (mapLock);
        notasSuprimidas.clear();
        reconstruirMascaraDeSuprimidas();
    }

    // O panic direto abaixo atende o instrumento nativo. Injete tambem a
    // sequencia MIDI completa no grafo: se o slot 1 estiver com um VST3/AU,
    // ele nao conhece TimbresProcessor::panic() e ficaria com nota/bend preso
    // depois de hot-plug ou resume.
    juce::MidiBuffer panic;
    panic.ensureSize (4096);
    ChurchDAWPerformanceMidiSafety::appendFullPanic (panic, 0);

    for (const auto metadata : panic)
        player.getMidiMessageCollector().addMessageToQueue (
            metadata.getMessage());

    if (auto* live = getChurchLive())
        live->panic();

    juce::Logger::writeToLog (
        "MIDI SAFETY: transporte recentralizado apos hot-plug/resume");
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
   #if JUCE_ANDROID || JUCE_IOS
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
        layoutSafeLeft = juce::jlimit (0, 16, display->safeAreaInsets.getLeft());
        layoutSafeRight = juce::jlimit (0, 16, display->safeAreaInsets.getRight());
        layoutSafeTop = juce::jlimit (0, 24, display->safeAreaInsets.getTop());
        layoutSafeBottom = juce::jlimit (0, 16, display->safeAreaInsets.getBottom());
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
   #if JUCE_ANDROID || JUCE_IOS
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

    auto area = layoutContentBounds;

    // 720 dp e uma largura comum em aparelhos 20:9. Tratar exatamente 720 como
    // layout largo deixava a wordmark ocupar o espaco do chip MIDI.
    const int safeContentWidth = juce::jmax (1, layoutContentBounds.getWidth());
    const int safeContentHeight = juce::jmax (1, layoutContentBounds.getHeight());
   #if JUCE_ANDROID || JUCE_IOS
    mobilePortraitLayout = safeContentWidth < safeContentHeight;
   #else
    mobilePortraitLayout = false;
   #endif
    const bool narrowPhone = compact && safeContentWidth <= 760;
    narrowHeaderLayout = narrowPhone;

   #if JUCE_ANDROID || JUCE_IOS
    // 56 dp entrega 48 dp reais de alvo apos as margens. Em retrato a barra
    // vira duas fileiras: identidade/status em cima, acoes embaixo. Assim
    // Android 16/freeform nao comprime seis botoes numa unica linha.
    layoutHeaderHeight = mobilePortraitLayout ? 108
                                              : mobileShortLandscapeLayout ? 32 : 56;
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
    layoutBrandWidth = narrowPhone ? 58
                                   : mobileShortLandscapeLayout ? 132 : 166;
    desktopHeaderTwoRows = false;
   #else
    desktopHeaderTwoRows = safeContentWidth < 1160;
    layoutHeaderHeight = compact ? (desktopHeaderTwoRows ? 82 : 46)
                                 : desktopHeaderTwoRows ? 96 : headerHeight;
    layoutProgramBandHeight = compact ? 60
                                      : desktopHeaderTwoRows ? 96
                                                             : programBandHeight;
    layoutBrandWidth = narrowPhone ? 70 : compact ? 190 : 190;
   #endif
    compactHeaderLayout = compact;

    const int headerH = layoutHeaderHeight;
    const int bandH = layoutProgramBandHeight;

    // larguras enxutas: cada pixel economizado aqui vira nome de aparelho legivel
    // nos chips (antes saia "Launchkey..." / "Focus...")
    // 44 dp de altura no touch. Antes o recuo vertical de 7 deixava apenas
    // 32 dp: bonito no print, pequeno demais para acertar rapido no palco.
    auto headerRow = area.removeFromTop (juce::jmin (headerH, area.getHeight()));
    auto headerControls = headerRow.reduced (compact ? 8 : 12,
                                             mobileShortLandscapeLayout ? 2
                                                                        : compact ? 4 : 11);

    // PAINEL DE PROGRAMA: faixa propria logo abaixo do cabecalho, centralizado na
    // largura. Estava enfiado no cabecalho e ficava apertado e feio -- ele e um
    // bloco de painel, nao um chip de barra de ferramentas.
    {
        programBand = area.removeFromTop (bandH);

        // No compacto o visor usa a LARGURA TODA e quase toda a altura da faixa:
        // com 60 px nao ha o que sobrar para margem decorativa. Ele continua
        // sendo o elemento que se le de longe -- so deixa de ser um bloco de
        // 152 px de altura mostrando o mesmo texto.
        oled.setBounds (compact ? programBand.reduced (
                                      mobileShortLandscapeLayout ? 6 : 8,
                                      mobileShortLandscapeLayout ? 2 : 3)
                                : programBand.withSizeKeepingCentre (juce::jmin (860, programBand.getWidth() - 48),
                                                                     programBand.getHeight() - 16));
        midiLearnPanel.setBounds (compact ? programBand.reduced (
                                                mobileShortLandscapeLayout ? 6 : 8,
                                                mobileShortLandscapeLayout ? 2 : 3)
                                          : programBand.withSizeKeepingCentre (juce::jmin (960, programBand.getWidth() - 48),
                                                                               programBand.getHeight() - 22));

        // As cenas ocupam o MESMO retangulo do visor e do MIDI Learn: e uma
        // superficie que ja alterna ocupantes, entao o Live Mode entra sem
        // disputar largura no cabecalho, que no celular ja esta no limite.
        liveScenePanel.setBounds (compact ? programBand.reduced (
                                                mobileShortLandscapeLayout ? 6 : 8,
                                                mobileShortLandscapeLayout ? 2 : 3)
                                          : programBand.withSizeKeepingCentre (juce::jmin (960, programBand.getWidth() - 48),
                                                                               programBand.getHeight() - 22));
    }
    const bool learning = mapButton.getToggleState();
    const int gap = compact ? 4 : 7;

   #if JUCE_ANDROID || JUCE_IOS
    // Orcamento de largura do header mobile (800 dp, Church Live aberto):
    // 166 marca + 112 dispositivo + 474 acoes/gaps + 22 margens = 774.
    // Em 720 dp entra o ramo narrow (58 + 112 + 420 + 22 = 612).
    // Assim o chip MIDI nunca e espremido a poucos pixels nem invade botoes.
    const int audioW = narrowPhone ? 68 : 88;
    const int librariesW = narrowPhone ? 64 : 88;
    const int scanW = narrowPhone ? 68 : 72;
    const int padW = narrowPhone ? 52 : 54;
    const int saveW = narrowPhone ? 58 : 54;
    const int mapW = narrowPhone ? 76 : 82;
    const int clearW = narrowPhone ? 78 : 82;
    const int instrumentsW = narrowPhone ? 82 : 88;
    const int deviceW = 112;
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

    audioSettingsButton.setButtonText (narrowPhone ? juce::String::fromUTF8 ("\xc3\x81udio")
                                                   : juce::String::fromUTF8 ("\xc3\x81udio / MIDI"));
    librariesButton.setButtonText ("Bibliotecas");
    instrumentsButton.setButtonText (narrowPhone ? "Timbres"
                                                 : juce::String::fromUTF8 ("\xe2\x97\x89 Instrumentos"));
    mapButton.setButtonText (learning ? "Concluir"
                                     : narrowPhone ? "Mapear" : "Mapear MIDI");

    if (! clearMapConfirmationArmed)
        clearMapButton.setButtonText (narrowPhone ? "Limpar" : "Limpar mapas");

    // No celular o cabecalho e uma barra de acao, nao um painel de configuracao.
    // Durante o Learn, botoes sem relacao com a tarefa saem temporariamente e o
    // usuario fica com Instrumentos + Limpar + Concluir. Ao sair, tudo volta.
    audioSettingsButton.setVisible (! learning);
    librariesButton.setVisible (! learning);
    padButton.setVisible (! learning);
    saveButton.setVisible (! learning);
    liveButton.setVisible (! learning);
    statusInfo.setVisible ((! compact || desktopHeaderTwoRows) && ! learning);

   #if CHURCHDAW_HOSTS_PLUGINS
    scanButton.setVisible (! learning);
   #endif

   #if JUCE_ANDROID || JUCE_IOS
    if (mobilePortraitLayout)
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

        scanButton.setBounds ({});
        clearMapButton.setBounds (learning ? clearMapButton.getBounds()
                                          : juce::Rectangle<int>());
    }
    else
   #endif
    {
       #if ! JUCE_ANDROID && ! JUCE_IOS
        // Desktop premium: quatro grupos com orcamentos independentes. A versao
        // anterior removia todos os botoes da direita com larguras fixas e so
        // depois entregava o que sobrasse aos cards de dispositivo; em 1280 px
        // isso podia ser zero. Aqui cada grupo recebe largura antes do layout.
        headerControls.removeFromLeft (layoutBrandWidth);
        headerControls.removeFromRight (8);

        for (juce::Button* button : std::initializer_list<juce::Button*> { &instrumentsButton, &liveButton, &mapButton,
                                                                            &clearMapButton, &saveButton, &padButton,
                                                                            &scanButton, &librariesButton,
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

            if (churchFull)
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
                add (liveButton, 54);
                add (mapButton, 96);
                add (saveButton, 70);
                add (padButton, 54);

               #if CHURCHDAW_HOSTS_PLUGINS
                add (scanButton, 72);
               #endif

                add (librariesButton, 92);
                add (audioSettingsButton, 100);
            }

            constexpr int navGap = 5;
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
        deviceBar.setVisible (true);

        if (desktopHeaderTwoRows && ! learning)
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
        headerControls.removeFromLeft (layoutBrandWidth);   // marca + separador
        headerControls.removeFromRight (compact ? 6 : 14);

        if (learning)
        {
            // Durante o aprendizado o Live Mode sai do cabecalho: os dois
            // ocupam a mesma faixa, e um botao visivel que nao pode ser usado
            // e pior que botao nenhum.
            liveButton.setBounds ({});

            mapButton.setBounds (headerControls.removeFromRight (mapW));
            headerControls.removeFromRight (gap);
            clearMapButton.setBounds (headerControls.removeFromRight (clearW));

            if (churchFull)
            {
                headerControls.removeFromRight (gap);
                instrumentsButton.setBounds (headerControls.removeFromRight (instrumentsW));
            }
        }
        else
        {
            clearMapButton.setBounds ({});

            audioSettingsButton.setBounds (headerControls.removeFromRight (audioW));
            headerControls.removeFromRight (gap);

            librariesButton.setBounds (headerControls.removeFromRight (librariesW));
            headerControls.removeFromRight (gap);

           #if CHURCHDAW_HOSTS_PLUGINS
            scanButton.setBounds (headerControls.removeFromRight (scanW));
            headerControls.removeFromRight (gap);
           #else
            // Nao reservar espaco para um botao que nem existe. Este buraco de
            // 84 px era exatamente onde o status "Launchkey conectado" sumia.
            scanButton.setBounds ({});
           #endif

            padButton.setBounds (headerControls.removeFromRight (padW));
            headerControls.removeFromRight (gap);
            saveButton.setBounds (headerControls.removeFromRight (saveW));
            headerControls.removeFromRight (gap);
            mapButton.setBounds (headerControls.removeFromRight (mapW));
            headerControls.removeFromRight (gap);
            liveButton.setBounds (headerControls.removeFromRight (62));

            if (churchFull)
            {
                headerControls.removeFromRight (gap);
                instrumentsButton.setBounds (headerControls.removeFromRight (instrumentsW));
            }
        }

        if (! compact && ! learning)
        {
            statusInfo.setBounds (headerControls.removeFromRight (juce::jmin (152, headerControls.getWidth())));
            headerControls.removeFromRight (10);
        }
        else
        {
            statusInfo.setBounds ({});
        }

        deviceBar.setCompact (compact);
        deviceBar.setVisible (true);
        deviceBar.setBounds (headerControls.removeFromLeft (juce::jmin (deviceW,
                                                                        headerControls.getWidth())));
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
        if (dockedNative && compact)
        {
           #if JUCE_ANDROID || JUCE_IOS
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

        // O minimo de 600 forcava barra de rolagem assim que o painel de programa
        // cresceu: as reguas nao encolhiam, eram CORTADAS (sumiam M/S e PANIC).
        // Com 400 elas se compactam de verdade e tudo continua na tela.
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
           #if JUCE_ANDROID || JUCE_IOS
            dockedEditor->setSize (
                compact ? area.getWidth() : juce::jmax (1120, area.getWidth()),
                compact ? (mobileShortLandscapeLayout
                               ? area.getHeight()
                               : juce::jmax (520, area.getHeight()))
                        : juce::jmax (400, area.getHeight()));
           #else
            dockedEditor->setSize (
                compact ? area.getWidth() : juce::jmax (1120, area.getWidth()),
                compact ? area.getHeight() : juce::jmax (400, area.getHeight()));
           #endif
        }

        layoutDockHolder();
    }

   #if JUCE_ANDROID
    // Central Áudio/MIDI full-screen: acompanha rotação, freeform e safe area.
    if (audioPanel != nullptr)
    {
        audioPanel->setBounds (getLocalBounds());
        audioPanel->toFront (false);
    }
   #endif

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
        const int larguraPad = juce::jmin (560, disponivel.getWidth());
        const int alturaPad = juce::jmin (318, disponivel.getHeight());

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
                + " buffer=" + juce::String (setup.bufferSize)
                + " rate=" + juce::String (setup.sampleRate, 0)
                + (dev != nullptr ? juce::String (" open=yes \"") + dev->getName() + "\""
                                   : juce::String (" open=NO")));
        }
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
                if (self->controleDeGravacaoDoSetup.timerAindaValido (
                        token, self->geracaoDaCena))
                    self->saveChainState();
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
            if (d.isInstrument == wantInstrument
                && ! knownPlugins.isBlacklisted (d.fileOrIdentifier, true)
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
    const auto requestedIdentifier = desc.createIdentifierString();

    if (knownPlugins.isBlacklisted (desc.fileOrIdentifier, true)
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
        [safe, lifetime, slotIndex, desc, requestedIdentifier, generation,
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
                return self->knownPlugins.isBlacklisted (
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

            const auto inputChannels = instance->getTotalNumInputChannels();
            const auto outputChannels = instance->getTotalNumOutputChannels();
            juce::String incompatibleReason;

            if (desc.isInstrument != (slotIndex == 0))
            {
                incompatibleReason = slotIndex == 0
                    ? "o plugin foi identificado como efeito, nao instrumento"
                    : "o plugin foi identificado como instrumento, nao efeito";
            }
            else if (slotIndex == 0)
            {
                if (outputChannels <= 0)
                    incompatibleReason = "o instrumento nao oferece saida de audio";
                else if (! instance->acceptsMidi())
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

            slot.nodeID = newNode->nodeID;
            slot.proc.store (newNode->getProcessor());
            slot.pluginIdentifier = requestedIdentifier;
            slot.pendingIdentifier.clear();
            slot.pendingState.clear();
            slot.loadingIdentifier.clear();
            slot.openButton.setEnabled (true);
            newNode->getProcessor()->addListener (self);

            for (int c = 0; c < slot.choices.size(); ++c)
                if (slot.choices.getReference (c).createIdentifierString()
                    == slot.pluginIdentifier)
                    slot.box.setSelectedId (c + 2, juce::dontSendNotification);

            if (oldNodeID.uid != 0)
                self->removeNodeForSlot (slotIndex, oldNodeID);

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
    unloadSlot (slotIndex);
    const auto loadGeneration = slots[slotIndex].loadGeneration;

    auto instance = std::make_unique<TimbresProcessor>();
    instance->setExternalMaster (masterProcessor);                    // master grandao dentro do Church Live
    instance->setPluginResources (&knownPlugins, &formatManager,
                                  &pluginScanQuarantine);     // plugins hospedados no canal master

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
    s.pendingIdentifier.clear();
    s.pendingState.clear();
    s.openButton.setEnabled (true);
    s.proc.store (node->getProcessor());
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
}

void MainComponent::unloadSlot (int slotIndex)
{
    auto& s = slots[slotIndex];
    ++s.loadGeneration;   // cancela qualquer AUv3 que ainda esteja abrindo
    s.loadingIdentifier.clear();
    s.openButton.setEnabled (false);
    const auto nodeToRemove = s.nodeID;

    // Desanexa o slot ANTES de destruir editores. Plugins reais podem bombear
    // a fila de mensagens ao fechar; uma escolha reentrante passa a pertencer
    // a uma nova geracao e nunca pode ser removida por este unload antigo.
    s.nodeID = {};
    s.proc.store (nullptr);

    if (nodeToRemove.uid != 0)
        removeNodeForSlot (slotIndex, nodeToRemove);

    rebuildGraphConnections();
}

void MainComponent::removeNodeForSlot (int slotIndex,
                                       juce::AudioProcessorGraph::NodeID nodeID)
{
    if (nodeID.uid == 0)
        return;

    if (auto node = graph.getNodeForId (nodeID))
    {
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
        window->setTopLeftPosition (140 + offset, 140 + offset);
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

void MainComponent::setupLaunchkey()
{
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

    launchkey->onFader = [this] (int index, float v)
    {
        static const juce::String sourceId ("churchdaw:launchkey-daw");

        // A porta DAW agora passa pelo MESMO MIDI Learn dos outros teclados.
        // Se este CC foi aprendido ele prevalece; sem mapa, continua valendo a
        // superficie pronta de fabrica logo abaixo.
        if (processControllerCC (LaunchkeyControl::faderCC0 + index,
                                 juce::roundToInt (v * 127.0f),
                                 sourceId, 1))
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

    launchkey->onEncoder = [this] (int index, float v)
    {
        static const juce::String sourceId ("churchdaw:launchkey-daw");

        if (processControllerCC (LaunchkeyControl::encoderCC0 + index,
                                 juce::roundToInt (v * 127.0f),
                                 sourceId, 1))
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
    launchkey->onPad = [this] (int note)
    {
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
            resetMidiTransportState();
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
    // roda na thread de MIDI - nao mexer em componentes daqui
    // O LED do header representa execucao musical, nao so knobs aprendidos.
    // Atomico e barato: note-on/off agora acende o mesmo pulso visual dos CCs.
    if (message.isNoteOnOrOff())
        lastMidiActivityMs.store (juce::Time::getMillisecondCounter());

    if (message.isController())
    {
        const juce::String sourceId =
            "midi:" + (source != nullptr ? source->getIdentifier()
                                         : juce::String ("unknown"));
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

            for (auto it = notasSuprimidas.begin(); it != notasSuprimidas.end();)
                if (it->sourceId == sourceId && it->channel == channel)
                    it = notasSuprimidas.erase (it);
                else
                    ++it;

            reconstruirMascaraDeSuprimidas();
        }

        processControllerCC (
            controller, message.getControllerValue(),
            sourceId, message.getChannel());
    }
    else if (message.isNoteOnOrOff() || message.isAftertouch())
    {
        if (notaFoiConsumida (source, message))
            return;   // tecla de comando: NAO vira nota no instrumento
    }

    // Tudo que nao foi consumido segue para o motor. Este repasse substitui o
    // registro direto do player como callback de MIDI.
    //
    // Roteamento 2 teclados: se o dispositivo esta amarrado a um canal do
    // Church Live (piano vs pad), reescreve o canal MIDI da mensagem antes de
    // enfileirar — teclados USB quase sempre mandam tudo no canal 1.
    juce::MidiMessage routed = message;
    if (source != nullptr)
        if (auto* live = getChurchLive())
        {
            const int forced = live->getForcedMidiChannelForDevice (
                source->getIdentifier());
            if (forced >= 1 && forced <= 16
                 && routed.getChannel() != forced
                 && (routed.isNoteOnOrOff() || routed.isAftertouch()
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

    player.getMidiMessageCollector().addMessageToQueue (routed);
}

// Chave completa da nota (aparelho + canal + numero). So e montada depois que
// as mascaras dizem que ha algo a procurar — ela custa uma alocacao.
ChurchDAWMidiMapping::ControllerKey MainComponent::montarChaveDeNota (
    juce::MidiInput* source, const juce::MidiMessage& message, int nota) const
{
    return { "midi:" + (source != nullptr ? source->getIdentifier()
                                          : juce::String ("unknown")),
             juce::jlimit (1, 16, message.getChannel()),
             nota };
}

// Espelha notasSuprimidas numa mascara atomica por NUMERO de nota, para o
// note-off poder desistir sem lock e sem alocar. Chamar sempre com o mapLock
// tomado; o conjunto tem, na pratica, as poucas teclas de comando que estao
// pressionadas neste instante.
void MainComponent::reconstruirMascaraDeSuprimidas()
{
    juce::uint32 nova[4] = { 0, 0, 0, 0 };

    for (const auto& k : notasSuprimidas)
        if (juce::isPositiveAndBelow (k.cc, 128))
            nova[k.cc >> 5] |= 1u << (k.cc & 31);

    for (int i = 0; i < 4; ++i)
        mascaraDeNotasSuprimidas[i].store (nova[i], std::memory_order_relaxed);
}

// Decide se esta mensagem de nota e uma TECLA DE COMANDO mapeada pelo musico.
//
// Roda na thread de MIDI, entao o caminho comum precisa ser barato: um teste
// de bit numa mascara atomica resolve 99,9% das notas sem tocar em lock
// nenhum. So no acerto e que vale a pena pegar o mapLock.
bool MainComponent::notaFoiConsumida (juce::MidiInput* source,
                                      const juce::MidiMessage& message)
{
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
        const auto it = notasSuprimidas.find (chaveOff);
        const bool eraSuprimida = it != notasSuprimidas.end();

        if (message.isNoteOff() && eraSuprimida)
        {
            notasSuprimidas.erase (it);
            reconstruirMascaraDeSuprimidas();
        }

        return eraSuprimida;
    }

    // Note-on: fora do Learn, sem bit ligado nao existe mapeamento para esta
    // nota — devolve na hora, sem construir chave nenhuma.
    if (! midiLearnActive.load()
         && (mascaraDeNotasMapeadas[palavra].load (std::memory_order_relaxed) & bit) == 0)
        return false;

    const auto chave = montarChaveDeNota (source, message, nota);

    // No modo Learn, tocar um tom no painel do Pad escolhe o alvo e a proxima
    // nota fisica cria o mapa. O primeiro note-on serve apenas para aprender;
    // ele e suprimido junto com seu note-off e nao dispara instrumento/Pad.
    if (midiLearnActive.load())
    {
        NoteAction aprendida;
        bool desmapeou = false;

        {
            const juce::ScopedLock trava (mapLock);
            aprendida = pendingNoteAction;

            if (aprendida.kind != NoteActionKind::none)
            {
                noteMappings[chave] = aprendida;
                notasSuprimidas.insert (chave);
                mascaraDeNotasSuprimidas[palavra].fetch_or (bit, std::memory_order_relaxed);

                // O bit de MAPEADAS sobe aqui DENTRO do lock, junto com a
                // escrita no mapa. Ligado la fora, uma reconstrucao que
                // estivesse em curso na outra thread escrevia o snapshot velho
                // por cima e apagava este bit — a tecla ficava gravada no
                // disco, etiquetada na tela, e muda no culto.
                mascaraDeNotasMapeadas[palavra].fetch_or (bit, std::memory_order_relaxed);

                lastMappedNoteKey = chave;
                lastMappedNoteAction = aprendida;
                pendingNoteAction = {};
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
                    noteMappings.erase (it);
                    notasSuprimidas.insert (chave);   // engole o par desta tecla
                    mascaraDeNotasSuprimidas[palavra].fetch_or (bit, std::memory_order_relaxed);
                    lastMappedNoteKey = chave;
                    lastMappedNoteAction = {};
                    desmapeou = true;
                }
            }
        }

        if (desmapeou)
        {
            // A mascara e um filtro por NUMERO de nota e a mesma nota pode
            // seguir mapeada noutro canal ou noutro teclado. Reconstruir e a
            // unica forma correta de apagar o bit — e nao pode acontecer aqui,
            // na thread de MIDI, porque percorre o mapa inteiro.
            lastMappedNote.store (nota);
            lastLearnedAtMs.store (juce::Time::getMillisecondCounter());
            triggerAsyncUpdate();
            return true;
        }

        if (aprendida.kind != NoteActionKind::none)
        {
            // (o bit ja subiu sob o lock, junto com a escrita no mapa)
            lastMappedNote.store (nota);
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

    {
        const juce::ScopedLock trava (mapLock);
        const auto it = noteMappings.find (chave);

        if (it == noteMappings.end())
            return false;

        acao = it->second;

        if (acao.kind != NoteActionKind::none)
        {
            notasSuprimidas.insert (chave);
            mascaraDeNotasSuprimidas[palavra].fetch_or (bit, std::memory_order_relaxed);
        }
    }

    if (acao.kind == NoteActionKind::none)
        return false;

    // A acao em si mexe em UI e no motor: sai da thread de MIDI.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MainComponent> (this), acao]
        {
            if (auto* self = safe.getComponent())
                self->executarAcaoDeNota (acao);
        });

    return true;
}

void MainComponent::executarAcaoDeNota (NoteAction acao)
{
    if (padProcessor == nullptr)
        return;

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
            padProcessor->setKey (acao.arg);
            break;

        case NoteActionKind::padOff:
            padProcessor->setKey (-1);
            break;

        case NoteActionKind::none:
        default:
            return;
    }

    if (padPanel != nullptr)
        padPanel->refreshState();
}

// Um unico caminho para CCs vindos da porta MIDI musical e da porta DAW do
// Launchkey. Retorna true quando o CC foi aprendido ou ja possui mapeamento;
// assim a superficie fixa do Launchkey sabe quando deve ceder ao mapa do usuario.
bool MainComponent::processControllerCC (int cc, int value,
                                         const juce::String& sourceId,
                                         int channel)
{
    cc = juce::jlimit (0, 127, cc);
    value = juce::jlimit (0, 127, value);
    const MidiControlKey control {
        sourceId, juce::jlimit (1, 16, channel), cc
    };

    if (! control.isValid()
         || ! ChurchDAWMidiMapping::isMappableController (cc))
        return false;

    const float value01 = (float) value / 127.0f;
    const auto now = juce::Time::getMillisecondCounter();
    lastMidiActivityMs.store (now);

    // CCs que NAO podem virar mapeamento: 64/66/67 sao os pedais (sustain,
    // sostenuto, soft) e 120-127 sao mensagens de modo do canal (all notes off,
    // reset...). Pisar no pedal com o Learn armado sequestrava o controle
    // selecionado — e ai o pedal passava a mexer num knob no meio do culto.
    const bool learnable = true;

    if (midiLearnActive.load() && learnable)
    {
        const int slot = lastTouchedSlot.load();
        const int param = lastTouchedParam.load();
        const bool touchOnMaster = slot == -1;

        // ESTAS DUAS COPIAS PRECISAM DO mapLock.
        //
        // Estamos na thread MIDI. Copiar juce::String incrementa o refcount de
        // um StringHolder compartilhado; se audioProcessorParameterChanged
        // reatribuir a String no mesmo instante (e o comentario dela diz que
        // "pode chegar em qualquer thread"), esta copia pode reter um holder
        // cujo refcount ja foi a zero — use-after-free. O header sempre
        // declarou estes dois campos como "protegido por mapLock"; a leitura
        // e que estava fora dele.
        juce::String capturedParamName;
        juce::String capturedParamId;
        {
            const juce::ScopedLock trava (mapLock);
            capturedParamName = lastTouchedParamName;
            capturedParamId = lastTouchedParamId;
        }

        if (slot != -2)
        {
            if (ChurchDAWMidiMapping::isProtectedPitchTarget (
                    capturedParamId,
                    capturedParamName))
            {
                juce::Logger::writeToLog ("MIDI MAP: aprendizado bloqueado para alvo protegido ("
                                          + capturedParamName
                                          + ") de "
                                          + (capturedParamName.isNotEmpty()
                                                 ? capturedParamName
                                                 : juce::String ("slot/param desconhecido"))
                                          + " no canal "
                                          + juce::String (lastTouchedInsert.load())
                                          + " / slot " + juce::String (slot)
                                          + " — use outro controle ou escolha um parâmetro não de tuning.");

                lastTouchedSlot.store (-2);
                lastTouchedParam.store (-1);
                lastTouchedInsert.store (-1);
                {
                    const juce::ScopedLock sl (mapLock);
                    lastTouchedParamName.clear();
                    lastTouchedParamId.clear();
                }

                return true;
            }

            CCTarget learned;

            {
                const juce::ScopedLock sl (mapLock);
                learned = { slot, param, lastTouchedInsert.load(),
                            capturedParamName, capturedParamId };

                if (learned.slot == -1)
                {
                    learned.paramName = "Master";
                    learned.paramId = "master";
                }

                ccMappings[control] = learned;
                lastMappedKey = control;
            }

            // mapeamento novo = pickup zerado: o fader tem que reencontrar o valor
            // do parametro novo antes de comecar a mexer nele. O clear fica pro
            // handleAsyncUpdate: ccPickup so pode ser tocado na message thread
            ccPickupToClear.store (cc);

            lastTouchedSlot.store (-2);
            lastTouchedParam.store (-1);
            lastTouchedInsert.store (-1);
            {
                const juce::ScopedLock sl (mapLock);
                lastTouchedParamName.clear();
                lastTouchedParamId.clear();
            }
            lastMappedCC.store (cc);
            lastLearnedAtMs.store (now);
            suppressTouchUntilMs.store (now + 800);
            juce::Logger::writeToLog ("MIDI MAP: aprendido CC " + juce::String (cc)
                                      + " canal " + juce::String (control.channel)
                                      + " de " + control.sourceId
                                      + " -> " + (learned.paramName.isNotEmpty()
                                                    ? learned.paramName : juce::String ("parametro")));
            triggerAsyncUpdate();
            return true;
        }
    }

    CCTarget target;
    bool hasMapping = false;

    {
        const juce::ScopedLock sl (mapLock);
        const auto it = ccMappings.find (control);

        if (it != ccMappings.end())
        {
            target = it->second;
            hasMapping = true;
        }
    }

    if (hasMapping)
    {
        auto safe = juce::Component::SafePointer<MainComponent> (this);

        juce::MessageManager::callAsync ([safe, target, value01, control]
        {
            if (auto* c = safe.getComponent())
                c->applyCCTarget (target, value01, control);
        });
    }

    return hasMapping;
}

void MainComponent::audioProcessorParameterChanged (juce::AudioProcessor* processor, int parameterIndex, float)
{
    // pode chegar em qualquer thread; usar apenas atomics
    if (! midiLearnActive.load() || applyingMappedCC)
        return;

    // Trocar/restaurar um timbre altera varios parametros de forma
    // programatica. Isso nao e um gesto do musico e nunca pode virar o
    // "ultimo controle tocado" do MIDI Learn.
    if (auto* timbres =
            dynamic_cast<TimbresProcessor*> (processor);
        timbres != nullptr
         && timbres->isProgrammaticToneChange())
        return;

    // plugins ecoam a mudanca logo depois que aplicamos/mapeamos um CC;
    // sem esta janela o modo Learn pediria o knob de novo em loop
    if (juce::Time::getMillisecondCounter() < suppressTouchUntilMs.load())
        return;

    if (processor == masterProcessor)
    {
        {
            const juce::ScopedLock sl (mapLock);
            lastTouchedParamName = "Master";
            lastTouchedParamId = "master";
        }

        lastTouchedSlot.store (-1);   // -1 = Master
        lastTouchedParam.store (-1);
        lastTouchedInsert.store (-1);
        triggerAsyncUpdate();
        return;
    }

    // Pad Continuo: nao e slot nem insert, e um no proprio do grafo. Sem este
    // ramo, mexer no corte ou no volume do Pad nao selecionava nada no modo
    // de mapeamento, e os knobs dele eram inalcancaveis por controlador.
    if (padProcessor != nullptr && processor == padProcessor)
    {
        const auto& params = processor->getParameters();

        {
            const juce::ScopedLock sl (mapLock);
            lastTouchedParamName = juce::isPositiveAndBelow (parameterIndex, params.size())
                                     ? params[parameterIndex]->getName (32)
                                     : juce::String();
            lastTouchedParamId = juce::isPositiveAndBelow (parameterIndex, params.size())
                                   ? ChurchDAWMidiMapping::stableParameterId (params[parameterIndex])
                                   : juce::String();
        }

        lastTouchedSlot.store (padSlotId);
        lastTouchedParam.store (parameterIndex);
        lastTouchedInsert.store (-1);
        triggerAsyncUpdate();
        return;
    }

    for (int i = 0; i < numSlots; ++i)
    {
        if (slots[i].proc.load() == processor)
        {
            const auto& params = processor->getParameters();

            {
                const juce::ScopedLock sl (mapLock);
                lastTouchedParamName = juce::isPositiveAndBelow (parameterIndex, params.size())
                                         ? params[parameterIndex]->getName (32)
                                         : juce::String();
                lastTouchedParamId = juce::isPositiveAndBelow (parameterIndex, params.size())
                                       ? ChurchDAWMidiMapping::stableParameterId (params[parameterIndex])
                                       : juce::String();
            }

            lastTouchedSlot.store (i);
            lastTouchedParam.store (parameterIndex);
            lastTouchedInsert.store (-1);
            triggerAsyncUpdate();
            return;
        }
    }

    // plugins hospedados DENTRO do Church Live (MASTER FX: BigSky etc.). Eles nao
    // sao nos do grafo, entao nao apareciam na varredura acima — era por isso que
    // tocar o MIX do BigSky nao selecionava nada no modo de mapeamento.
    for (int i = 0; i < numSlots; ++i)
    {
        auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load());

        if (timbres == nullptr)
            continue;

        for (int ins = 0; ins < TimbresProcessor::numInserts; ++ins)
        {
            if (timbres->getInsert (ins) == processor)
            {
                if (timbres->isProgrammaticToneChange())
                    return;

                const auto& params = processor->getParameters();

                {
                    const juce::ScopedLock sl (mapLock);
                    lastTouchedParamName = juce::isPositiveAndBelow (parameterIndex, params.size())
                                             ? params[parameterIndex]->getName (32)
                                             : juce::String();
                    lastTouchedParamId = juce::isPositiveAndBelow (parameterIndex, params.size())
                                           ? ChurchDAWMidiMapping::stableParameterId (params[parameterIndex])
                                           : juce::String();
                }

                lastTouchedSlot.store (i);
                lastTouchedParam.store (parameterIndex);
                lastTouchedInsert.store (ins);
                triggerAsyncUpdate();
                return;
            }
        }
    }
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
    CHURCHDAW_UI_SCOPE ("MainComponent::timerCallback");
    CHURCHDAW_UI_LAP_BEGIN();

    // Subiu para antes do bloco de MIDI: a lista de entradas agora e cacheada
    // por tempo (ver midiInputsCached) e precisa do relogio aqui em cima.
    const auto nowMs = juce::Time::getMillisecondCounter();

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
            if (auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
                for (int ins = 0; ins < TimbresProcessor::numInserts; ++ins)
                    if (auto* fx = timbres->getInsert (ins))
                        fx->addListener (this);
    }

    CHURCHDAW_UI_LAP ("timer:listeners");

    // A porta DAW tem de ficar FORA da entrada MIDI comum. O estado de audio
    // salvo e o proprio dialogo Audio/MIDI podem reabilita-la sem querer — e ai
    // pad volta a tocar nota e a porta fica ocupada pro LaunchkeyControl.
    //
    // A MESMA rede de seguranca do refreshMidiInputs vale aqui, e por um motivo
    // pratico: sem ela, o cliente que abrisse Audio/MIDI e marcasse a entrada
    // na mao via este timer desmarcar em menos de 1 s. Nao havia contorno
    // possivel pelo usuario — foi o que tornou a regressao da 1.5.4 tao grave.
    {
        const auto entradas = midiInputsCached (nowMs);

        int sobrariam = 0;
        for (const auto& d : entradas)
            if (! LaunchkeyControl::isDawPort (d.name))
                ++sobrariam;

        if (entradas.size() == 0 || sobrariam > 0)
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

    // Tuner de fluidez: roda na message thread, so AUMENTA buffer quando ha
    // xrun real. 1 xrun/8 s ja sobe um degrau (antes 2/10 s e teto 1024) —
    // culto multi-camada + streaming de samples precisa de folga. Teto 2048
    // se o driver oferecer. getXRunCount e caro (lock Oboe): UMA leitura/ciclo.
    if (nowMs - androidLastBufferTuneMs >= 8000)
    {
        const int currentXRuns = deviceManager.getXRunCount();
        const int newXRuns = currentXRuns >= androidLastXRunCount
                                ? currentXRuns - androidLastXRunCount
                                : currentXRuns;
        bool bufferChanged = false;

        if (newXRuns >= 1)
            if (auto* device = deviceManager.getCurrentAudioDevice())
            {
                const int currentSize = device->getCurrentBufferSizeSamples();
                int nextSize = 0;

                for (const int candidate : device->getAvailableBufferSizes())
                    if (candidate > currentSize && candidate <= 2048)
                    {
                        nextSize = candidate;
                        break;
                    }

                if (nextSize > 0)
                {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.bufferSize = nextSize;
                    const auto error =
                        deviceManager.setAudioDeviceSetup (setup, false);
                    bufferChanged = true;

                    if (error.isEmpty())
                    {
                        appProperties.getUserSettings()->setValue (
                            "bufferSize", nextSize);
                        appProperties.getUserSettings()->saveIfNeeded();
                    }

                    juce::Logger::writeToLog (
                        "AUDIO: tuner Android " + juce::String (currentSize)
                        + " -> " + juce::String (nextSize)
                        + " apos " + juce::String (newXRuns)
                        + " xruns/8s"
                        + (error.isEmpty()
                               ? juce::String (" (ok)")
                               : " ERRO: " + error));
                }
            }

        androidLastXRunCount = bufferChanged ? deviceManager.getXRunCount()
                                             : currentXRuns;
        androidLastBufferTuneMs = nowMs;
    }
   #endif

    CHURCHDAW_UI_LAP ("timer:android-buffer");

    if ((launchkey == nullptr || ! launchkey->isConnected() || ! launchkey->isListening())
         && nowMs - lastLaunchkeyRetryMs >= 5000)
    {
        lastLaunchkeyRetryMs = nowMs;
        setupLaunchkey();
    }

    CHURCHDAW_UI_LAP ("timer:launchkey-setup");

    refreshLaunchkeyNames();   // so envia o que mudou

    CHURCHDAW_UI_LAP ("timer:launchkey-names");

    const juce::String sep = "  " + juce::String::charToString ((juce::juce_wchar) 0x2022) + "  ";
    juce::String txt;

    juce::String ifaceName;
    bool ifaceOn = false;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        txt << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << " kHz" << sep
            << device->getCurrentBufferSizeSamples() << " smp" << sep << "CPU "
            << juce::roundToInt (deviceManager.getCpuUsage() * 100.0) << "%";

        ifaceName = prettifyDeviceName (device->getName());
        ifaceOn = true;
    }
    else
    {
        txt << "sem dispositivo de audio";
    }

    statusInfo.setText (txt, juce::dontSendNotification);

    CHURCHDAW_UI_LAP ("timer:android-audio");

    // Visor de programa no formato do Electro 7: chapa vermelha com o canal em
    // foco, nome grande do timbre dele, e embaixo a lista dos outros canais --
    // que e como a Nord mostra as camadas empilhadas sob o programa.
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

            const auto liveSummary = loaded == 0
                ? juce::String ("Nenhum timbre")
                : on == loaded
                    ? juce::String (on) + " canais ativos"
                    : juce::String (on) + " / " + juce::String (loaded)
                        + " canais ativos";
            const auto summarySeparator = juce::String::fromUTF8 (" \xc2\xb7 ");

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
}

void MainComponent::handleAsyncUpdate()
{
    // reset de pickup pedido pela thread de MIDI (o map e so desta thread).
    // O slot atomico e unico: dois pedidos coalescidos perderiam o primeiro
    // e o CC re-mapeado pularia o soft-takeover. Limpar o mapa inteiro cobre
    // os dois — pior caso os outros CCs so re-armam o pickup (inofensivo)
    if (ccPickupToClear.exchange (-1) >= 0)
        ccPickup.clear();

    bool noteMappingWasCompleted = false;
    const int mappedNote = lastMappedNote.exchange (-1);

    if (mappedNote >= 0)
    {
        // Se o disco recusou, a tecla funciona nesta sessao e SOME na proxima
        // abertura. Dizer "PRONTO" aqui e a pior mentira do fluxo: e o unico
        // caminho que grava varias vezes seguidas, entao o musico mapeia o
        // culto inteiro achando que esta guardado.
        const bool gravouNoDisco = saveNoteMap();

        NoteAction mappedAction;
        MidiControlKey mappedKey;
        {
            const juce::ScopedLock sl (mapLock);
            mappedKey = lastMappedNoteKey;
            mappedAction = lastMappedNoteAction;
        }

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
            midiLearnPanel.showUnmappedNote (
                mappedNote,
                gravouNoDisco ? juce::String ("Mapeamento salvo automaticamente")
                              : juce::String::fromUTF8 (
                                    "Vale s\xc3\xb3 nesta sess\xc3\xa3o \xe2\x80\x94 volta ao fechar"));
        else
            midiLearnPanel.showMappedNote (acao, mappedNote, gravouNoDisco);
        juce::Logger::writeToLog (juce::String (desmapeada ? "MIDI MAP: DESMAPEADA nota "
                                                           : "MIDI MAP: aprendida nota ")
                                  + juce::String (mappedNote)
                                  + " canal " + juce::String (mappedKey.channel)
                                  + " de " + mappedKey.sourceId + " -> " + acao
                                  + (gravouNoDisco ? "" : "  [NAO SALVOU]"));
        noteMappingWasCompleted = true;
    }

    const int mappedCC = lastMappedCC.exchange (-1);

    if (mappedCC >= 0)
    {
        // MESMA REGRA DA NOTA, 50 linhas acima: o painel nao pode confirmar um
        // mapa que nao foi para o disco. Este e o caminho que grava MAIS vezes
        // seguidas — o musico mapeia knob por knob no ensaio — e era o unico
        // que ainda dizia "PRONTO" sem olhar o resultado. Com o arquivo
        // recusado (versao futura, pasta somente-leitura, antivirus), ele
        // reaprendia tudo e perdia tudo de novo na proxima abertura.
        const bool gravouCC = saveMidiMap();

        CCTarget mappedTarget;
        MidiControlKey mappedKey;
        {
            const juce::ScopedLock sl (mapLock);
            mappedKey = lastMappedKey;
            const auto it = ccMappings.find (mappedKey);

            if (it != ccMappings.end())
                mappedTarget = it->second;
        }

        const auto alvo = describeCCTarget (mappedTarget);
        const auto targetName = alvo.isNotEmpty() ? alvo : juce::String ("controle");

        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        midiLearnPanel.showMapped (targetName, mappedCC, gravouCC);
        return;
    }

    if (noteMappingWasCompleted)
        return;

    if (! midiLearnActive.load())
        return;

    const int slot = lastTouchedSlot.load();
    const int param = lastTouchedParam.load();

    juce::String name;

    if (slot == -1)
    {
        name = "Master";
    }
    else if (slot == padSlotId)
    {
        // O PAD PRECISA DE RAMO PROPRIO AQUI TAMBEM.
        //
        // Sem ele o slot -3 caia no "else" la embaixo, que APAGA o nome do
        // parametro — a ancora que a thread de MIDI le para fechar o
        // mapeamento. O resultado, medido na tela: mexer no knob de corte
        // dentro do MIDI Learn nao saia do PASSO 1, e o knob que o cliente
        // pediu simplesmente nao era mapeavel. O caminho de aplicacao ja
        // existia; faltava o de APRENDIZADO.
        if (padProcessor != nullptr)
        {
            const auto& params = padProcessor->getParameters();

            if (juce::isPositiveAndBelow (param, params.size()))
            {
                const auto paramName = params[param]->getName (32);
                const auto paramId = ChurchDAWMidiMapping::stableParameterId (params[param]);

                // Os parametros do Pad ja se chamam "Pad Cutoff"/"Pad Volume".
                // Prefixar sempre produzia "Pad  Pad Cutoff" na tela.
                name = paramName.startsWithIgnoreCase ("Pad") ? paramName
                                                              : "Pad  " + paramName;

                const juce::ScopedLock sl (mapLock);
                lastTouchedParamName = paramName;
                lastTouchedParamId = paramId;
            }
        }
    }
    else if (slot >= 0 && slot < numSlots)
    {
        juce::AudioProcessor* proc = slots[slot].proc.load();
        const int insert = lastTouchedInsert.load();

        if (insert >= 0)
            if (auto* timbres = dynamic_cast<TimbresProcessor*> (proc))
                proc = timbres->getInsert (insert);

        if (proc != nullptr)
        {
            const auto& params = proc->getParameters();

            if (param >= 0 && param < params.size())
            {
                const auto paramName = params[param]->getName (32);
                const auto paramId = ChurchDAWMidiMapping::stableParameterId (params[param]);
                name = (insert >= 0 ? proc->getName() + " " : juce::String()) + paramName;

                // guarda o NOME pra ancorar o mapeamento (a thread de MIDI le isto
                // sob o mesmo lock quando o knob chegar)
                const juce::ScopedLock sl (mapLock);
                lastTouchedParamName = paramName;
                lastTouchedParamId = paramId;
            }
        }
    }
    else
    {
        const juce::ScopedLock sl (mapLock);
        lastTouchedParamName.clear();
        lastTouchedParamId.clear();
    }

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

void MainComponent::applyCCTarget (CCTarget target, float value01,
                                   MidiControlKey control)
{
    suppressTouchUntilMs.store (juce::Time::getMillisecondCounter() + 600);
    applyingMappedCC = true;
    bool valid = false;
    bool applied = false;
    bool pickupPending = false;
    auto targetName = describeCCTarget (target);

    if (target.slot == -1)
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
    else if (target.slot == padSlotId)
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

                if (! control.isValid() || ccHasPickedUp (control, value01, cur))
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
    else if (target.slot >= 0 && target.slot < numSlots)
    {
        juce::AudioProcessor* proc = slots[target.slot].proc.load();

        if (target.insert >= 0)
            if (auto* timbres = dynamic_cast<TimbresProcessor*> (proc))
                proc = timbres->getInsert (target.insert);   // MASTER FX do Church Live

        if (proc != nullptr)
        {
            const auto& params = proc->getParameters();
            const int index = ChurchDAWMidiMapping::resolveParameterIndex (
                params, target.paramId, target.paramName);

            if (index >= 0)
            {
                valid = true;
                const float cur = params[index]->getValue();

                if (! control.isValid()
                     || ccHasPickedUp (control, value01, cur))
                {
                    params[index]->setValueNotifyingHost (value01);
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
            midiLearnPanel.showActivity (targetName, ccNumber,
                                         juce::roundToInt (value01 * 127.0f));
        else if (pickupPending)
            midiLearnPanel.showPickup (targetName, ccNumber);
    }
}

juce::String MainComponent::describeCCTarget (const CCTarget& target) const
{
    if (target.slot == -1)
        return "Master";

    // O Pad tem slot proprio (padSlotId) porque nao mora na cadeia de slots.
    // Ele ganhou caminho no applyCCTarget e no audioProcessorParameterChanged,
    // mas nao aqui — entao o mapa funcionava e mesmo assim o painel dizia
    // "controle", o log dizia "alvo invalido" e a faixa de atividade aparecia
    // sem nome. Metade de uma feature e pior que nenhuma: o usuario mapeia,
    // ve um nome generico e nao confia que pegou.
    if (target.slot == padSlotId)
    {
        juce::String nome = target.paramName;

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

        return nome.startsWithIgnoreCase ("Pad") ? nome : "Pad  " + nome;
    }

    if (! juce::isPositiveAndBelow (target.slot, numSlots))
        return {};

    juce::AudioProcessor* proc = slots[target.slot].proc.load();

    if (target.insert >= 0)
        if (auto* timbres = dynamic_cast<TimbresProcessor*> (proc))
            proc = timbres->getInsert (target.insert);

    juce::String parameterName = target.paramName;

    if (proc != nullptr)
    {
        const auto& params = proc->getParameters();
        const int resolved = ChurchDAWMidiMapping::resolveParameterIndex (
            params, target.paramId, target.paramName);

        if (juce::isPositiveAndBelow (resolved, params.size()))
            parameterName = params[resolved]->getName (32);
    }

    if (parameterName.isEmpty())
        parameterName = "Parametro";

    if (target.insert >= 0 && proc != nullptr)
        return proc->getName() + "  " + parameterName;

    return parameterName;
}

int MainComponent::getMidiMappingCount() const
{
    const juce::ScopedLock sl (mapLock);
    return (int) (ccMappings.size() + noteMappings.size());
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
        removerPaginaDeCenaSeOrfa (novoEstadoId);
        juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1)
                                    + " NAO gravada; memoria e cena ativa restauradas");
        return resultado;
    }

    removerPaginaDeCenaSeOrfa (estadoIdAnterior);
    juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1) + " gravada");
    return ChurchDAWLive::Resultado::ok;
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

    const auto& cena = cenas[indice];

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

        live->panic();

        for (int slot = 0; slot < TimbresProcessor::numInserts; ++slot)
            live->unloadInsert (slot);

        for (int l = 0; l < TimbresProcessor::numLayers; ++l)
        {
            live->clearLayer (l);

            if (live->getLayerInstrument (l) != nullptr)
                live->clearInstrumentPlugin (l);

            if (auto* on = live->getLayerOnParam (l))
                on->setValueNotifyingHost (0.0f);

            if (auto* mute = live->getLayerMuteParam (l))
                *mute = false;

            if (auto* solo = live->getLayerSoloParam (l))
                *solo = false;
        }

        // Pagina nova nao herda os controles globais da musica anterior.
        // Como nao ha canal carregado, neutralizar agora nao produz salto de
        // audio e entrega ao cliente um ponto de partida realmente novo.
        const ChurchDAWLive::Cena paginaNova;

        if (auto* p = live->getVolumeGeralParam())
            *p = paginaNova.volumeGeral;

        if (auto* p = live->getTransposeGeralParam())
            *p = paginaNova.transposeGeral;

        if (auto* p = live->getRetornoDeReverbParam())
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

        // A pagina vazia tambem move Pad/Master por baixo dos controles MIDI.
        // Ela precisa do mesmo refresh/rearme de uma cena preenchida; sem isto
        // o card do Pad mostrava o tom anterior e o primeiro CC saltava direto
        // para a posicao fisica antiga.
        ccPickup.clear();

        if (padPanel != nullptr)
            padPanel->refreshState();

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
        if (auto* p = live->getLayerTransposeParam (l))
            *p = cena.canais[l].transpose;

    if (cena.temGlobais)
        if (auto* p = live->getTransposeGeralParam())
            *p = cena.transposeGeral;

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
    cenaAtiva = indice;

    // UMA fonte de verdade para a cena corrente: o mesmo XML que guarda as 10.
    // Guardar o indice numa chave separada ja custou um bug — a chave era
    // escrita aqui e ninguem a lia, entao ao reabrir o app nenhuma cena
    // aparecia acesa mesmo depois de uma noite inteira tocando numa delas.
    if (! salvarCenas())
    {
        cenaAtiva = ativaAntes;
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

    agendarFimDaSupressaoDeGravacaoDaCena (geracaoDaRequisicao);
    juce::Logger::writeToLog ("LIVE: cena " + juce::String (indice + 1) + " aplicada");
    return ChurchDAWLive::Resultado::ok;
}

void MainComponent::renomearCena (int indice, juce::String nome)
{
    if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas)
         || ! cenas[indice].preenchida)
        return;

    // Limite curto de proposito: o botao tem a largura de um dedo. Um nome que
    // nao cabe vira reticencias e deixa de servir para achar a musica, que e a
    // unica razao de existir o nome.
    const ChurchDAWLive::EstadoAntesDePersistirCena antes { cenas[indice], cenaAtiva };
    cenas[indice].nome = nome.trim().substring (0, 18);

    const auto resultado = ChurchDAWLive::concluirPersistenciaDeCena (
        salvarCenas(), antes, cenas[indice], cenaAtiva);
    liveScenePanel.atualizar (cenas, cenaAtiva);

    // Mesma regra do capturarCena: renomear que nao chegou ao disco volta ao
    // nome antigo no proximo arranque, e o musico so descobre no domingo.
    if (resultado != ChurchDAWLive::Resultado::ok)
        liveScenePanel.avisar (
            ChurchDAWLive::avisoDoResultado (resultado, indice));
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
                const auto paginaAnterior =
                    self->cenas[indice].estadoDoMotorId;
                self->cenas[indice] = ChurchDAWLive::Cena{};   // volta a ser slot vazio

                // A cena apagada nao pode continuar acesa como "tocando".
                if (self->cenaAtiva == indice)
                    self->cenaAtiva = -1;

                const auto persistencia = ChurchDAWLive::concluirPersistenciaDeCena (
                    self->salvarCenas(), antes,
                    self->cenas[indice], self->cenaAtiva);
                const bool gravou = persistencia == ChurchDAWLive::Resultado::ok;
                self->liveScenePanel.atualizar (self->cenas, self->cenaAtiva);

                if (gravou)
                    self->removerPaginaDeCenaSeOrfa (paginaAnterior);

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

    const auto guardado = arquivo.getSiblingFile (arquivo.getFileName() + ".corrompido");
    const auto novo = arquivo.getSiblingFile (arquivo.getFileName() + ".corrompido.novo");

    novo.deleteFile();

    if (arquivo.getSize() <= 0 || ! arquivo.copyFileTo (novo))
    {
        juce::Logger::writeToLog (rotulo + ": arquivo ILEGIVEL e sem copia nova "
                                           "(o backup anterior foi preservado)");
        return;
    }

    bool trocou = novo.moveFileTo (guardado);

    if (! trocou)
    {
        guardado.deleteFile();
        trocou = novo.moveFileTo (guardado);
    }

    if (trocou)
        juce::Logger::writeToLog (rotulo + ": arquivo ILEGIVEL; copia em "
                                    + guardado.getFileName());
    else
        juce::Logger::writeToLog (rotulo + ": arquivo ILEGIVEL e NAO consegui guardar a copia; "
                                           "ela ficou em " + novo.getFileName());
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
            "LIVE: pagina orfa nao pôde ser removida " + arquivo.getFileName());
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
    xml.setAttribute ("schema", 2);   // v2 referencia paginas de audio por SHA-256
    xml.setAttribute ("active", cenaAtiva);

    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        ChurchDAWLive::escreverCena (xml, cenas[i], i);

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

    if (xml->getIntAttribute ("schema", 1) > 2)
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

    cenaAtiva = juce::jlimit (-1, ChurchDAWLive::numeroDeCenas - 1,
                              xml->getIntAttribute ("active", -1));

    for (auto* c : xml->getChildWithTagNameIterator ("SCENE"))
    {
        const int i = c->getIntAttribute ("index", -1);

        if (juce::isPositiveAndBelow (i, ChurchDAWLive::numeroDeCenas))
            ChurchDAWLive::lerCena (*c, cenas[i]);
    }

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
                        if (self->cenaPendenteNoStartup == indice)
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
                    if (self->cenaPendenteNoStartup == indice)
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

void MainComponent::reconstruirMascaraDeNotas()
{
    juce::uint32 nova[4] = { 0, 0, 0, 0 };

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

// Devolve TRUE so quando o arquivo novo foi realmente gravado.
//
// Era void, e a migracao decidia apagar a chave antiga perguntando
// "o arquivo existe?" — pergunta errada: o arquivo CORROMPIDO que motivou a
// migracao tambem existe. Bastava a gravacao falhar (arquivo ainda travado por
// quem o corrompeu, permissao, disco cheio) para o app apagar a unica copia
// boa e o cliente perder todas as teclas mapeadas.
bool MainComponent::saveNoteMap()
{
    // Arquivo de uma versao futura: nao encostar. Devolve false para que a
    // migracao (e qualquer outro chamador) saiba que nada foi gravado.
    if (esquemaDeNotasDesconhecido)
    {
        juce::Logger::writeToLog ("MIDI MAP: gravacao ignorada, o arquivo em disco "
                                  "e de uma versao mais nova");
        return false;
    }

    juce::XmlElement xml ("NOTEMAP");

    // Versao de esquema: o mapa das cenas ja tem, este nao tinha. Sem ela nao
    // ha como migrar um formato futuro sem adivinhacao.
    xml.setAttribute ("schema", 1);

    {
        const juce::ScopedLock sl (mapLock);

        for (const auto& par : noteMappings)
        {
            auto* n = xml.createNewChildElement ("NOTE");
            n->setAttribute ("source", par.first.sourceId);
            n->setAttribute ("channel", par.first.channel);
            n->setAttribute ("note", par.first.cc);
            n->setAttribute ("action", (int) par.second.kind);
            n->setAttribute ("arg", par.second.arg);
        }
    }

    // ARQUIVO PROPRIO, pelo mesmo motivo das cenas.
    //
    // Isto era gravado dentro do settings — o arquivo que carrega o estado
    // binario dos plugins em base64 e chega a varios MB com um Kontakt
    // aberto — e a cada tecla aprendida. Era exatamente o arquivo do qual as
    // cenas fugiram, escrito sincronamente na thread da interface. No ensaio
    // dava um engasgo; junto com o botao Salvar durante o culto, dava a tela
    // travando.
    auto destino = arquivoDoMapaDeNotas();
    destino.getParentDirectory().createDirectory();

    juce::TemporaryFile temp (destino);
    const bool gravou = xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();

    if (! gravou)
        juce::Logger::writeToLog ("MIDI MAP: FALHA ao gravar " + destino.getFullPathName());

    return gravou;
}

void MainComponent::restoreNoteMap()
{
    const auto arquivo = arquivoDoMapaDeNotas();
    auto xml = juce::parseXML (arquivo);
    bool veioDoFormatoAntigo = false;

    // MIGRACAO do formato antigo (chave dentro do settings). Quem ja mapeou
    // teclas nao pode perde-las ao atualizar.
    if (xml == nullptr || ! xml->hasTagName ("NOTEMAP"))
    {
        guardarCopiaDeArquivoIlegivel (arquivo, "MIDI MAP (notas)");

        xml = appProperties.getUserSettings()->getXmlValue ("midiNoteMap");
        veioDoFormatoAntigo = xml != nullptr;
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
            const int nota  = n->getIntAttribute ("note", -1);
            const int canal = n->getIntAttribute ("channel", 0);
            const int tipo  = n->getIntAttribute ("action", 0);
            const int arg   = n->getIntAttribute ("arg", -1);
            const auto fonte = n->getStringAttribute ("source");

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
            else
                continue;

            noteMappings[ChurchDAWMidiMapping::ControllerKey { fonte, canal, nota }] = acao;
        }
    }

    reconstruirMascaraDeNotas();
    atualizarEtiquetasDoPad();

    // Termina a migracao: grava no formato novo e so DEPOIS solta a chave
    // antiga. Nessa ordem, uma falha no meio deixa o mapa no lugar velho —
    // nunca em nenhum dos dois.
    if (veioDoFormatoAntigo)
    {
        // So solta a chave antiga se a gravacao NOVA deu certo de verdade.
        // Perguntar "o arquivo existe?" respondia sim para o arquivo ilegivel
        // que motivou a migracao — e o cliente perdia o mapa inteiro.
        if (saveNoteMap())
        {
            appProperties.getUserSettings()->removeValue ("midiNoteMap");
            juce::Logger::writeToLog ("MIDI MAP: mapa de notas migrado para "
                                        + arquivoDoMapaDeNotas().getFileName());
        }
        else
        {
            juce::Logger::writeToLog ("MIDI MAP: migracao adiada, o mapa antigo "
                                      "continua no setup");
        }
    }
}

juce::File MainComponent::arquivoDoMapaDeCCs()
{
    return appProperties.getUserSettings()->getFile().getSiblingFile ("midi-cc-map.xml");
}

bool MainComponent::saveMidiMap()
{
    if (esquemaDeCCsDesconhecido)
    {
        juce::Logger::writeToLog ("MIDI MAP: gravacao de CCs ignorada, o arquivo em disco "
                                  "e de uma versao mais nova");
        return false;
    }

    juce::XmlElement xml ("MIDIMAP");
    xml.setAttribute ("schema", 1);

    {
        const juce::ScopedLock sl (mapLock);

        for (const auto& pair : ccMappings)
        {
            auto* m = xml.createNewChildElement ("MAP");
            m->setAttribute ("source", pair.first.sourceId);
            m->setAttribute ("channel", pair.first.channel);
            m->setAttribute ("cc", pair.first.cc);
            m->setAttribute ("slot", pair.second.slot);
            m->setAttribute ("param", pair.second.param);
            m->setAttribute ("insert", pair.second.insert);
            m->setAttribute ("name", pair.second.paramName);
            m->setAttribute ("id", pair.second.paramId);
        }
    }

    // ARQUIVO PROPRIO, o ultimo dos tres a sair do settings.
    //
    // Era gravado dentro do arquivo que carrega o estado binario dos plugins em
    // base64 — varios MB com um Kontakt aberto — e a cada CC aprendido. As
    // cenas sairam de la primeiro, depois o mapa de notas; este ficou para
    // tras pelo mesmo motivo de sempre: ninguem tinha olhado.
    auto destino = arquivoDoMapaDeCCs();
    destino.getParentDirectory().createDirectory();

    juce::TemporaryFile temp (destino);
    const bool gravou = xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();

    if (! gravou)
        juce::Logger::writeToLog ("MIDI MAP: FALHA ao gravar " + destino.getFullPathName());

    return gravou;
}

void MainComponent::restoreMidiMap()
{
    int restored = 0;
    int rejectedUnsafe = 0;

    const auto arquivo = arquivoDoMapaDeCCs();
    auto xml = juce::parseXML (arquivo);
    bool veioDoFormatoAntigo = false;

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

        if (xml != nullptr && ! xml->hasTagName ("MIDIMAP"))
        {
            juce::Logger::writeToLog ("MIDI MAP: chave antiga de CCs com formato irreconhecivel; "
                                      "mantida intacta");
            return;
        }

        veioDoFormatoAntigo = xml != nullptr;
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

            // Iterar so por <MAP>, como o mapa de notas faz por <NOTE>. Com o
            // iterador solto, qualquer filho de outro tipo reprovava na
            // validacao, virava "legado ambiguo descartado" e — porque isso
            // dispara regravacao — era APAGADO do arquivo em definitivo. Hoje
            // nao existe outro filho; isto e a trava para o dia em que existir.
            for (auto* m : xml->getChildWithTagNameIterator ("MAP"))
            {
                CCTarget target {
                    m->getIntAttribute ("slot", -2),
                    m->getIntAttribute ("param", -1),
                    m->getIntAttribute ("insert", -1),
                    m->getStringAttribute ("name"),
                    m->getStringAttribute ("id")
                };
                const int cc = m->getIntAttribute ("cc", -1);
                const MidiControlKey control {
                    m->getStringAttribute ("source"),
                    m->getIntAttribute ("channel", 0),
                    cc
                };

                // Mapas muito antigos guardavam somente o indice. Depois que
                // Attack/Release/EQ/Rotary foram adicionados, esse indice pode
                // apontar para outro parametro — inclusive Transpose. Descartar
                // o mapa ambiguo e seguro; reaproveita-lo e alterar o tom ao vivo.
                // Mapa sem origem/canal tambem e descartado: torna-lo wildcard
                // faria outro teclado com o mesmo CC mudar o tom.
                if (! control.isValid()
                     || ! ChurchDAWMidiMapping::isMappableController (cc)
                     || ! ChurchDAWMidiMapping::isSafeStoredTarget (
                            target.slot, target.paramId, target.paramName))
                {
                    ++rejectedUnsafe;
                    continue;
                }

                ccMappings[control] = std::move (target);
                ++restored;
            }
        }

    juce::Logger::writeToLog ("MIDI MAP: " + juce::String (restored)
                              + " mapeamento(s) restaurado(s), "
                              + juce::String (rejectedUnsafe)
                              + " legado(s) ambiguo(s) descartado(s)");

    // Termina a migracao: grava no formato novo e so DEPOIS solta a chave
    // antiga. Um alvo perigoso descartado tambem obriga a regravar, para ele
    // nao voltar na proxima inicializacao.
    if (veioDoFormatoAntigo || rejectedUnsafe > 0)
    {
        if (saveMidiMap())
        {
            if (veioDoFormatoAntigo)
            {
                appProperties.getUserSettings()->removeValue ("midiMap");
                juce::Logger::writeToLog ("MIDI MAP: mapa de CCs migrado para "
                                            + arquivoDoMapaDeCCs().getFileName());
            }
        }
        else if (veioDoFormatoAntigo)
        {
            juce::Logger::writeToLog ("MIDI MAP: migracao de CCs adiada, o mapa antigo "
                                      "continua no setup");
        }
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

    const auto instrument = nodeForSlot (0);
    const auto midiCh = juce::AudioProcessorGraph::midiChannelIndex;

    // fontes que alimentam o comeco da cadeia de efeitos
    const auto firstDest = fxChain.isEmpty() ? masterNode : fxChain.getFirst();

    if (instrument.uid != 0)
        connectAudio (instrument, firstDest, "instrument_to_chain", true);

    if (inputToggle.getToggleState())
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

    // master sempre alimenta a saida
    juce::AudioProcessorGraph::Node::Ptr outputNode (
        graph.getNodeForId (audioOutNode));
    const bool outputIsAvailable = outputNode != nullptr
        && outputNode->getProcessor()->getTotalNumInputChannels() > 0;
    connectAudio (masterNode, audioOutNode, "master_to_device_output",
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
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        if (auto xml = deviceManager.createStateXml())
            appProperties.getUserSettings()->setValue ("audioSetup", xml.get());

        appProperties.getUserSettings()->setValue ("bufferSize", device->getCurrentBufferSizeSamples());
        appProperties.getUserSettings()->setValue ("sampleRate", device->getCurrentSampleRate());
        appProperties.getUserSettings()->saveIfNeeded();
    }
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
   #if JUCE_ANDROID || JUCE_IOS
    static constexpr int margin = 0, headerH = 64, footerPad = 14;
   #else
    static constexpr int margin = 12, headerH = 56;
   #endif

    explicit AudioSettingsHolder (MainComponent& mc)
        : owner (&mc),
          selector (mc.deviceManager, 0, 2, 0, 2, true, false, true, false)
    {
        mc.audioDialogOpen = true;
        brandIcon = juce::ImageCache::getFromMemory (BinaryData::icon_png, BinaryData::icon_pngSize);

       #if JUCE_ANDROID || JUCE_IOS
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
        selector.setItemHeight (26);
        addAndMakeVisible (selector);
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
       #if JUCE_ANDROID || JUCE_IOS
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

        // RESPOSTA DO TECLADO: cada controlador manda uma velocity diferente
        // pra mesma forca. Fica aqui, e nao na regua master, porque e do mesmo
        // assunto da entrada MIDI logo acima -- calibragem do equipamento, se
        // ajusta uma vez por teclado.
       #if JUCE_ANDROID || JUCE_IOS
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
       #if JUCE_ANDROID || JUCE_IOS
        velCombo.setColour (juce::ComboBox::backgroundColourId, NordLookAndFeel::bgControl());
        velCombo.setColour (juce::ComboBox::outlineColourId, NordLookAndFeel::borderDefault());
        velCombo.setColour (juce::ComboBox::textColourId, NordLookAndFeel::txtPrimary());
        velCombo.setColour (juce::ComboBox::arrowColourId, NordLookAndFeel::nordRedBright());
       #endif
        addAndMakeVisible (velCombo);

        // fecha o card (o destrutor salva a config)
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
       #if JUCE_ANDROID || JUCE_IOS
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
                    m->audioPanel.reset();
            });
        };
        addAndMakeVisible (closeBtn);

        // recupera o driver de "pipocos" SEM desplugar o cabo (e sem travar o app).
        // No Android o texto deixa claro o caso USB class-compliant.
       #if JUCE_ANDROID
        const auto restartLabel = juce::String::fromUTF8 ("Recuperar USB");
       #else
        const auto restartLabel = juce::String::fromUTF8 (
            "\xe2\x86\xbb Reiniciar \xc3\xa1udio");   // ↻
       #endif
        restartBtn.setButtonText (restartLabel);
        restartBtn.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::nordRed());
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

       #if JUCE_ANDROID
        // MIDI BLE: permissoes + atalho para Ajustes de Bluetooth (pareamento).
        bleMidiBtn.setButtonText (juce::String::fromUTF8 ("MIDI Bluetooth"));
        bleMidiBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff252a32));
        bleMidiBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
        bleMidiBtn.setTooltip (juce::String::fromUTF8 (
            "Pede permissao de Bluetooth e abre os ajustes para parear um "
            "teclado MIDI BLE. Depois o app lista a porta automaticamente."));
        bleMidiBtn.onClick = [this]
        {
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

                jboolean requestedPerms = JNI_FALSE;
                if (jmethodID method = env->GetStaticMethodID (
                        clazz, "requestMidiBlePermissions",
                        "(Landroid/app/Activity;I)Z"))
                {
                    requestedPerms = env->CallStaticBooleanMethod (
                        clazz, method, activity.get(), (jint) 0x41DA);
                    if (env->ExceptionCheck())
                        env->ExceptionClear();
                }
                else
                {
                    env->ExceptionClear();
                }

                if (requestedPerms == JNI_FALSE)
                {
                    if (jmethodID method = env->GetStaticMethodID (
                            clazz, "openBluetoothSettings",
                            "(Landroid/app/Activity;)Z"))
                    {
                        env->CallStaticBooleanMethod (clazz, method, activity.get());
                        if (env->ExceptionCheck())
                            env->ExceptionClear();
                    }
                    else
                    {
                        env->ExceptionClear();
                    }
                }

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
        };
        addAndMakeVisible (bleMidiBtn);
       #endif

       #if JUCE_ANDROID || JUCE_IOS
        // O tamanho final vem de showAudioSettings(), usando a janela real.
        setSize (360, 640);
       #else
        setSize (620 + margin * 2, 542 + margin * 2);
       #endif
    }

    ~AudioSettingsHolder() override
    {
        stopTimer();

       #if JUCE_ANDROID || JUCE_IOS
        selectorViewport.setViewedComponent (nullptr, false);
       #endif

        if (auto* c = owner.getComponent())
        {
            c->audioDialogOpen = false;
            c->saveAudioStateNow();
        }
    }

    void timerCallback() override
    {
       #if ! JUCE_ANDROID && ! JUCE_IOS
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
       #if JUCE_ANDROID || JUCE_IOS
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
       #if JUCE_ANDROID || JUCE_IOS
        const int maxW = juce::jmin (680, juce::jmax (320, full.getWidth() - 48));
        return full.withSizeKeepingCentre (maxW, full.getHeight()).reduced (0, 10);
       #else
        return full.reduced (margin);
       #endif
    }

    void paint (juce::Graphics& g) override
    {
       #if JUCE_ANDROID || JUCE_IOS
        // Scrim escuro sobre o mixer + card central premium.
        g.setColour (juce::Colours::black.withAlpha (0.72f));
        g.fillRect (getLocalBounds());

        auto col = contentColumn().toFloat();
        const float rad = 18.0f;
        {
            juce::Path sp;
            sp.addRoundedRectangle (col, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.65f), 36, { 0, 10 })
                .drawForPath (g, sp);
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

        auto head = col.toNearestInt().removeFromTop (headerH);
        if (brandIcon.isValid())
        {
            g.setOpacity (1.0f);
            g.drawImageWithin (brandIcon, head.getX() + 18, head.getY() + 14, 34, 34,
                               juce::RectanglePlacement::centred);
        }
        else
        {
            NordLookAndFeel::drawChurchDAWMark (
                g, { (float) head.getX() + 16.0f, (float) head.getY() + 12.0f, 38.0f, 38.0f });
        }

        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (17.0f)).boldened()
                       .withExtraKerningFactor (0.04f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x81UDIO / MIDI"),
                    head.withTrimmedLeft (60).withTrimmedRight (52)
                        .withTrimmedTop (8),
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::txtSecondary().withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (juce::String::fromUTF8 ("Sa\xc3\xad" "da, lat\xc3\xaa" "ncia e teclado"),
                    head.withTrimmedLeft (60).withTrimmedRight (52)
                        .withTrimmedTop (30),
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::borderDefault().withAlpha (0.8f));
        g.fillRect (head.getX() + 16, head.getBottom() - 1, head.getWidth() - 32, 1);

        // Faixa de acoes no rodape do card
        if (! footerBand.isEmpty())
        {
            g.setColour (juce::Colour (0xff161a20));
            g.fillRoundedRectangle (footerBand.toFloat().getX(),
                                    (float) footerBand.getY(),
                                    footerBand.toFloat().getWidth(),
                                    footerBand.toFloat().getHeight() + 8.0f,
                                    0.0f);
            // arredonda so a base do card cobrindo o footer
            g.setColour (juce::Colour (0xff161a20));
            auto foot = footerBand.toFloat();
            juce::Path p;
            p.addRoundedRectangle (foot.getX(), foot.getY() - 1.0f,
                                   foot.getWidth(), foot.getHeight() + 10.0f,
                                   0.0f, 0.0f, false, false, true, true);
            g.fillPath (p);
            g.setColour (NordLookAndFeel::borderDefault().withAlpha (0.55f));
            g.fillRect (foot.getX() + 16.0f, foot.getY(), foot.getWidth() - 32.0f, 1.0f);
        }
       #else
        auto bounds = getLocalBounds().toFloat();
        auto card = bounds.reduced ((float) margin);
        const float rad = 14.0f;

        // sombra: card flutuando sobre o mixer
        {
            juce::Path sp;
            sp.addRoundedRectangle (card, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 26, { 0, 7 }).drawForPath (g, sp);
        }

        // corpo "vidro escuro" + borda da marca
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1c22), card.getX(), card.getY(),
                                                 juce::Colour (0xff101013), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, rad);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (card.getX() + 14.0f, card.getY() + 1.5f, card.getRight() - 14.0f, card.getY() + 1.5f, 1.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.45f));
        g.drawRoundedRectangle (card, rad, 1.2f);

        // acento vermelho no topo (mesma linguagem dos outros cards)
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (card.reduced (16.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f), 1.5f);

        // cabecalho: icone + titulo + subtitulo + regua
        auto head = card.toNearestInt().removeFromTop (headerH);

        if (brandIcon.isValid())
        {
            g.setOpacity (1.0f);   // imagem herda o alpha do ultimo setColour
            g.drawImageWithin (brandIcon, head.getX() + 16, head.getY() + 13, 34, 34,
                               juce::RectanglePlacement::centred);
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (17.0f)).boldened().withExtraKerningFactor (0.06f));
        g.drawText (juce::String::fromUTF8 ("\xc3\x81UDIO / MIDI"),
                    head.withTrimmedLeft (62).withTrimmedTop (6), juce::Justification::centredLeft);

        if (getWidth() >= 500)
        {
            g.setColour (NordLookAndFeel::dimText().withAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText (juce::String::fromUTF8 ("Dispositivo, lat\xc3\xaancia e entradas MIDI"),
                        head.reduced (16, 0).withTrimmedRight (36).withTrimmedTop (6),
                        juce::Justification::centredRight);
        }

        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.55f));
        g.fillRect (head.getX() + 14, head.getBottom() - 1, head.getWidth() - 28, 1);
       #endif
    }

    void resized() override
    {
       #if JUCE_ANDROID || JUCE_IOS
        auto col = contentColumn();
        closeBtn.setBounds (col.getRight() - 50, col.getY() + 12, 40, 40);

        auto body = col;
        body.removeFromTop (headerH + 6);
        body = body.reduced (18, 0).withTrimmedBottom (12);

        // Rodape compacto: velocity + 2 botoes + status. Sem "metade da tela".
        constexpr int footerH = 168;
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
       #if JUCE_ANDROID
        {
            const int gap = 10;
            const int w = (actions.getWidth() - gap) / 2;
            bleMidiBtn.setBounds (actions.removeFromLeft (w));
            actions.removeFromLeft (gap);
            restartBtn.setBounds (actions);
        }
       #else
        restartBtn.setBounds (actions);
       #endif

        foot.removeFromTop (6);
        audioInputNote.setBounds (foot.removeFromTop (20));

        // Corpo: titulo da secao + seletor com LARGURA do card (nao da tela).
        sectionDevice.setBounds (body.removeFromTop (16));
        body.removeFromTop (4);
        statusChip.setBounds (body.removeFromTop (18));
        body.removeFromTop (4);

        selectorViewport.setBounds (body);
        // Altura do seletor ~ conteudo real (item 44 * ~6 linhas + labels).
        // Nao esticar ate o fundo — o "mar" preto some.
        const int contentH = juce::jmax (260, selector.getItemHeight() * 7 + 48);
        selector.setBounds (0, 0, juce::jmax (260, body.getWidth()), contentH);
       #else
        auto card = getLocalBounds().reduced (margin);
        closeBtn.setBounds (card.getRight() - 38, card.getY() + 12, 26, 22);

        auto area = card;
        area.removeFromTop (headerH + 4);

        auto extra = area.removeFromBottom (46).reduced (20, 10);
        restartBtn.setBounds (extra.removeFromRight (160));
        extra.removeFromRight (12);
        owner->inputToggle.setBounds (extra);

        auto velRow = area.removeFromBottom (42).reduced (20, 8);
        velLabel.setBounds (velRow.removeFromLeft (150));
        velRow.removeFromLeft (10);
        velCombo.setBounds (velRow.removeFromLeft (190).withSizeKeepingCentre (190, 26));

        selector.setBounds (area.reduced (18, 8));
       #endif
    }

    void paintOverChildren (juce::Graphics& g) override
    {
       #if ! (JUCE_ANDROID || JUCE_IOS)
        // separador acima da secao de baixo (resposta do teclado + entrada + reiniciar)
        auto row = getLocalBounds().reduced (margin).removeFromBottom (46 + 42);
        g.setColour (NordLookAndFeel::panelOutline());
        g.fillRect (row.removeFromTop (1).reduced (14, 0));
       #else
        juce::ignoreUnused (g);
       #endif
    }

    // arrasta o card pela faixa do cabecalho (desktop). No mobile a pagina
    // e full-bleed e nao se arrasta.
    void mouseDown (const juce::MouseEvent& e) override
    {
       #if JUCE_ANDROID || JUCE_IOS
        juce::ignoreUnused (e);
        dragging = false;
       #else
        dragging = e.position.y < (float) (margin + headerH);
        if (dragging)
            dragger.startDraggingComponent (this, e);
       #endif
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
       #if JUCE_ANDROID || JUCE_IOS
        juce::ignoreUnused (e);
       #else
        if (dragging)
            dragger.dragComponent (this, e, nullptr);
       #endif
    }

    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }

    juce::Component::SafePointer<MainComponent> owner;
    juce::AudioDeviceSelectorComponent selector;
   #if JUCE_ANDROID || JUCE_IOS
    juce::Viewport selectorViewport;
    juce::Label sectionDevice, sectionMidi, statusChip;
    juce::Rectangle<int> footerBand;
   #endif
    juce::TextButton restartBtn, closeBtn;
   #if JUCE_ANDROID
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
    deviceManager.closeAudioDevice();
    deviceManager.restartLastAudioDevice();
   #endif
}

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
        const auto activeOutputs = device->getActiveOutputChannels().countNumberOfSetBits();
        juce::Logger::writeToLog (
            "AUDIO_ANDROID open_succeeded generation=" + juce::String (generation)
            + " reason=" + selectionReason
            + " output=\"" + device->getName() + "\""
            + " sample_rate_actual=" + juce::String (device->getCurrentSampleRate(), 0)
            + " buffer_actual=" + juce::String (device->getCurrentBufferSizeSamples())
            + " output_channels=" + juce::String (activeOutputs));

        androidLastXRunCount = deviceManager.getXRunCount();
        androidLastBufferTuneMs = juce::Time::getMillisecondCounter();
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
        audioDialogOpen
    };
    auto* center = new AndroidAudioMidiCenter (std::move (host));
    audioPanel.reset (center);
    audioSettingsButton.repaint();
    addAndMakeVisible (center);
    // Full-screen (não modal desktop). resized() do MainComponent re-aplica.
    center->setBounds (getLocalBounds());
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
    // Desktop conserva o card original e so reduz se a janela for menor.
    const float fitX = (float) juce::jmax (1, getWidth()  - 8) / (float) holder->getWidth();
    const float fitY = (float) juce::jmax (1, getHeight() - 8) / (float) holder->getHeight();
    const float panelScale = juce::jmin (1.0f, juce::jmin (fitX, fitY));

    if (panelScale < 0.999f)
        holder->setTransform (juce::AffineTransform::scale (panelScale));

    const int visualW = juce::roundToInt ((float) holder->getWidth()  * panelScale);
    const int visualH = juce::roundToInt ((float) holder->getHeight() * panelScale);

    const int visualX = juce::jmax (2, (getWidth()  - visualW) / 2);
    const int visualY = juce::jmax (2, (getHeight() - visualH) / 2);
    holder->setTopLeftPosition (juce::roundToInt ((float) visualX / panelScale),
                                juce::roundToInt ((float) visualY / panelScale));
   #endif
    holder->toFront (true);
   #endif
}

//==============================================================================
// Marca do header = botao de "Sobre". Nao gasta espaco na fileira de botoes e
// e o lugar onde todo mundo procura os creditos.
juce::Rectangle<int> MainComponent::brandArea() const
{
    return { layoutContentBounds.getX() + 6, layoutContentBounds.getY() + 2,
             juce::jmax (1, layoutBrandWidth - 4),
             juce::jmax (1, (mobilePortraitLayout ? 48 : layoutHeaderHeight) - 4) };
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    if (brandArea().contains (e.getPosition()))
        showAbout();
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const bool over = brandArea().contains (e.getPosition());

    if (over != brandHover)
    {
        brandHover = over;
        setMouseCursor (over ? juce::MouseCursor::PointingHandCursor
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
    const float fitX = (float) juce::jmax (1, getWidth()  - 8) / (float) p->getWidth();
    const float fitY = (float) juce::jmax (1, getHeight() - 8) / (float) p->getHeight();
    const float panelScale = juce::jmin (1.0f, juce::jmin (fitX, fitY));

    if (panelScale < 0.999f)
        p->setTransform (juce::AffineTransform::scale (panelScale));

    const int visualW = juce::roundToInt ((float) p->getWidth()  * panelScale);
    const int visualH = juce::roundToInt ((float) p->getHeight() * panelScale);

    const int visualX = juce::jmax (2, (getWidth()  - visualW) / 2);
    const int visualY = juce::jmax (2, (getHeight() - visualH) / 2);
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
    panel->setBounds (getLocalBounds());
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

    PluginListHolder (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& kl,
                      const juce::File& deadMansPedal, juce::PropertiesFile* props,
                      juce::StringArray& quarantine)
        // O 5o argumento (allowPluginsWhichRequireAsynchronousInstantiation)
        // NAO e so sobre AUv3: com ele em "false" o PluginListComponent usa
        // numThreads = 0 (juce_PluginListComponent.cpp:446) e a varredura roda
        // DENTRO do timerCallback, ou seja, na thread de mensagens. Cada plugin
        // problematico segurava a interface inteira pelos 90 s do prazo — app
        // "Nao Responde", barra parada e botao Cancelar inclicavel. Era o
        // "travou no Korg Vox" e o "AUVectorPanner a 0%" dos clientes.
        : formats (fm), knownPlugins (kl), deadMansPedalFile (deadMansPedal),
          propertiesToUse (props), persistentQuarantine (quarantine),
          list (fm, kl, deadMansPedal, props, true)
    {
        // Uma thread so: os plugins ja sao abertos num processo separado, e
        // varias threads abrindo plugins ao mesmo tempo e receita de conflito.
        // Com pool != nullptr o shouldExit() do scanner passa a valer, entao
        // "Cancelar" finalmente interrompe de verdade.
        list.setNumberOfThreadsForScanning (1);

        list.setOptionsButtonText (juce::String::fromUTF8 ("Op\xc3\xa7\xc3\xb5" "es..."));
        list.getOptionsButton().onClick = [this] { showOptionsMenu(); };
        addAndMakeVisible (list);

        scanButton.setButtonText ("Escanear " + formatosDaPlataforma());
        scanButton.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::nordRed());
        scanButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        scanButton.onClick = [this] { scanNow(); };
        addAndMakeVisible (scanButton);

        setSize (880, 470);
    }

    ~PluginListHolder() override
    {
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

    void showOptionsMenu()
    {
        juce::PopupMenu menu;
        menu.addItem ("Limpar lista de plugins", [this] { knownPlugins.clear(); });
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
                .withTargetComponent (list.getOptionsButton()));
    }

    bool revalidateDisabledPlugins (const juce::StringArray& quarantined = {})
    {
        if (list.isScanning() || ! filaDeFormatos.isEmpty())
        {
            juce::Logger::writeToLog (
                "PLUGIN_SCAN revalidate_ignored reason=batch_in_progress");
            return false;
        }

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

                if (knownFormat || format->fileMightContainThisPluginType (identifier))
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
        dispararProximoFormato();
        return true;
    }

    void scanSingleFormat (juce::AudioPluginFormat& format)
    {
        if (list.isScanning() || ! filaDeFormatos.isEmpty())
            return;

        revalidationTargets.clear();
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = false;
        filaDeFormatos.add ({ &format, {}, false });
        scanButton.setEnabled (false);
        dispararProximoFormato();
    }

    void scanNow()
    {
        if (list.isScanning() || ! filaDeFormatos.isEmpty())
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
        revalidationTargets.clear();
        unresolvedTargets.clear();
        activeScanIdentifiers.clear();
        revalidationBatchActive = false;
        scanButton.setEnabled (false);

        for (auto* format : formats.getFormats())
            if (format != nullptr && format->getName().containsIgnoreCase ("AudioUnit"))
                filaDeFormatos.add ({ format, {}, false });

        for (auto* format : formats.getFormats())
            if (format != nullptr && ! format->getName().containsIgnoreCase ("AudioUnit"))
                filaDeFormatos.add ({ format, {}, false });

        dispararProximoFormato();
    }

    void dispararProximoFormato()
    {
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
                activeScanIdentifiers = request.identifiers;
                list.scanFor (*format, request.identifiers);
            }
            else
            {
                activeScanIdentifiers.clear();

           #if JUCE_MAC
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

                    list.scanFor (*format, hostable);
                }
                else
           #endif
                {
                    list.scanFor (*format);
                }
            }

            startTimer (400);
            return;
        }

        concluirLote (true);
    }

    void timerCallback() override
    {
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

        bool allRevalidationTargetsResolved = completed;

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
       #if JUCE_MAC
        return "VST3 e Audio Unit";
       #else
        return "VST3";
       #endif
    }

    void resized() override
    {
        list.setBounds (getLocalBounds());

        // mesmo rodape do "Opcoes..." (24px, com margem de 2), do lado direito
        auto strip = getLocalBounds().reduced (2).removeFromBottom (24);
        scanButton.setBounds (strip.removeFromRight (140));
    }

    juce::AudioPluginFormatManager& formats;
    juce::KnownPluginList& knownPlugins;
    juce::File deadMansPedalFile;
    juce::PropertiesFile* propertiesToUse = nullptr;
    juce::StringArray& persistentQuarantine;
    juce::PluginListComponent list;
    juce::TextButton scanButton;
    juce::Array<PendingScan> filaDeFormatos;
    juce::StringArray revalidationTargets;
    juce::StringArray unresolvedTargets;
    juce::StringArray activeScanIdentifiers;
    bool revalidationBatchActive = false;
    std::function<void(bool completed)> onBatchFinished;

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

    auto* holder = new PluginListHolder (formatManager, knownPlugins, deadMansPedal,
                                         appProperties.getUserSettings(),
                                         pluginScanQuarantine);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (holder);
    o.dialogTitle = "Plugins — " + PluginListHolder::formatosDaPlataforma();
    o.componentToCentreAround = this;
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

void MainComponent::showPadWindow()
{
    if (padPanel == nullptr)
        return;

    const bool show = ! padPanel->isVisible();

    if (show && (padProcessor == nullptr || ! PadProcessor::padsAvailable()))
    {
       #if JUCE_ANDROID
        const auto help = juce::String::fromUTF8 (
            "Pad Cont\xc3\xadnuo n\xc3\xa3o instalado.\n\n"
            "Instale ou repare o pack Pad Cont\xc3\xadnuo em Bibliotecas "
            "e abra esta tela novamente.");
       #else
        const auto help = juce::String::fromUTF8 (
            "N\xc3\xa3o achei os WAVs de pad. Coloque pad_C.wav ... pad_B.wav "
            "na pasta 'ChurchDAW Pads' (ou 'Pads' ao lado do app).");
       #endif

       #if JUCE_ANDROID
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
       #else
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8 ("Pad Cont\xc3\xadnuo"), help);
       #endif
        return;
    }

    padPanel->setVisible (show);
    padButton.setToggleState (show, juce::dontSendNotification);   // botao acende so com o painel aberto

    if (show)
    {
        padPanel->refreshState();
        padPanel->toFront (true);
        resized();
    }
}

//==============================================================================
bool MainComponent::saveChainState()
{
    int slotsQueNaoSalvaram = 0;
    pluginsQueNaoSalvaram.clear();

    juce::XmlElement chain ("CHAIN");
    chain.setAttribute ("inputEnabled", inputToggle.getToggleState());
    chain.setAttribute ("masterDb", masterSlider.getValue());
    chain.setAttribute ("masterAutoEq", masterProcessor->getAutoEq());
    chain.setAttribute ("masterPunch", masterProcessor->getPunch());

    if (padProcessor != nullptr)
    {
        juce::MemoryBlock padState;
        padProcessor->getStateInformation (padState);

        if (! padState.isEmpty())
            chain.setAttribute ("padState", padState.toBase64Encoding());
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
                bool captured = false;

                // Plugins de terceiros podem tocar seu estado interno no mesmo
                // instante em que processBlock o usa. Salvar sem parar o
                // callback produzia estados corrompidos e, em alguns VST/AU,
                // crash depois de alguns minutos. A pausa e curta e o lock so
                // cobre a chamada do plugin; Base64/XML ficam fora da thread
                // critica.
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

    inputToggle.setToggleState (xml->getBoolAttribute ("inputEnabled"), juce::dontSendNotification);

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
    masterProcessor->setPunch (xml->getBoolAttribute ("masterPunch", true));

    if (padProcessor != nullptr && xml->hasAttribute ("padState"))
    {
        juce::MemoryBlock padState;

        if (padState.fromBase64Encoding (xml->getStringAttribute ("padState"))
             && ! padState.isEmpty())
            padProcessor->setStateInformation (padState.getData(),
                                                (int) padState.getSize());
    }

    for (auto* slotXml : xml->getChildIterator())
    {
        const int i = slotXml->getIntAttribute ("index", -1);
        const auto id = slotXml->getStringAttribute ("id");

        if (i < 0 || i >= numSlots || id.isEmpty())
            continue;

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
            && ! knownPlugins.isBlacklisted (
                desc->fileOrIdentifier, true)
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
}
