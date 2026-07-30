#include "MainComponent.h"
#include "NordLookAndFeel.h"
#include "TimbresProcessor.h"
#include "ChordDetect.h"
#include "AboutPanel.h"
#include "LibraryPanel.h"
#include "MidiMappingSafety.h"
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

    if (auto xml = appProperties.getUserSettings()->getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);

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

    deviceManager.initialise (numInputs, 2, savedAudio.get(), true);

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
    // chega a ~8 ms em vez da menor de todas: 5,3 ms nao e audivelmente melhor
    // que 10,7 ms para quem toca teclado, e o dobro de folga importa porque este
    // app le samples do disco enquanto toca -- falha de audio incomoda MUITO
    // mais que 5 ms de atraso. Quem quiser apertar mais muda no painel.
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
            if (((double) s / rate) * 1000.0 >= 8.0)
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
            deviceManager.setAudioDeviceSetup (setup, true);
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

    deviceManager.addMidiInputDeviceCallback ({}, &player);

    restoreMidiMap();
    deviceManager.addMidiInputDeviceCallback ({}, this);   // para o MIDI Learn / mapeamentos

    // alguns drivers ASIO abrem com o buffer padrao deles, ignorando o estado
    // salvo; reforca aqui a taxa e o buffer que o usuario escolheu
    {
        int wantedBuffer = appProperties.getUserSettings()->getIntValue ("bufferSize", 0);

       #if JUCE_ANDROID || JUCE_IOS
        // Comeca no menor tamanho OFERECIDO pelo aparelho que entrega pelo
        // menos 8 ms de prazo. No TANK3 sao dois bursts/512 frames; em um
        // aparelho de burst 192 podem ser 384. Nao force um numero que o driver
        // nao oferece. O Android observa xruns reais no timer e sobe em passos
        // somente quando o aparelho precisar.
        int preferredMobileBuffer = 0;

        if (auto* mobileDevice = deviceManager.getCurrentAudioDevice())
        {
            const auto available = mobileDevice->getAvailableBufferSizes();
            const double rate = mobileDevice->getCurrentSampleRate() > 0.0
                                    ? mobileDevice->getCurrentSampleRate()
                                    : 48000.0;

            for (const int candidate : available)
                if (((double) candidate / rate) * 1000.0 >= 8.0)
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
            // A versao anterior gravava 1024 como se fosse escolha do cliente.
            // Migra uma vez para o novo perfil adaptativo; depois respeita o
            // valor estabilizado/salvo, sujeito apenas ao piso seguro.
            auto* settings = appProperties.getUserSettings();
            if (settings->getIntValue ("androidAdaptiveBufferSchema", 0) < 1)
            {
                wantedBuffer = preferredMobileBuffer;
                settings->setValue ("androidAdaptiveBufferSchema", 1);
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
    // vermelho = PAINEL ABERTO (antes era vermelho fixo, o que fazia o botao
    // parecer sempre ativo e gastava a cor de destaque a toa)
    padButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x89 Pad"));   // ◉ Pad
    padButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc4112e));
    padButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    padButton.onClick = [this] { showPadWindow(); };

    // card do Pad Continuo EMBUTIDO na DAW (oculto ate clicar em "Pad")
    padPanel = std::make_unique<PadPanel> (padProcessor);
    padPanel->onClose = [this]
    {
        if (padPanel != nullptr)
            padPanel->setVisible (false);

        padButton.setToggleState (false, juce::dontSendNotification);
    };
    addChildComponent (*padPanel);

    addAndMakeVisible (saveButton);
    // vira um indicador de confianca: verde "Salvo" = autosave ligado, seu setup
    // esta guardado. Clicar forca salvar tudo e pisca "Salvo!"
   #if JUCE_ANDROID || JUCE_IOS
    saveButton.setButtonText ("Salvo");
    saveButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    saveButton.setColour (juce::TextButton::textColourOffId,
                          juce::Colour (0xff5fd08a).withAlpha (0.82f));
   #else
    saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
    saveButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff5fd08a));
   #endif
    saveButton.onClick = [this]
    {
        // salva TUDO agora: cadeia (timbres/volumes/efeitos), mapeamento MIDI e audio
        saveChainState();
        saveMidiMap();
        saveAudioStateNow();
        appProperties.getUserSettings()->saveIfNeeded();

       #if JUCE_ANDROID || JUCE_IOS
        saveButton.setButtonText ("Salvo!");
       #else
        saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo!"));
       #endif

        juce::Component::SafePointer<MainComponent> safe (this);
        juce::Timer::callAfterDelay (1400, [safe]
        {
            if (auto* c = safe.getComponent())
               #if JUCE_ANDROID || JUCE_IOS
                c->saveButton.setButtonText ("Salvo");
               #else
                c->saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
               #endif
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
        }
        clearMapConfirmationArmed = false;
        clearMapButton.setButtonText ("Limpar mapas");
        clearMapButton.setVisible (on);
        mapButton.setButtonText (on ? "Concluir" : "Mapear MIDI");
        oled.setVisible (! on);
        midiLearnPanel.setVisible (on);

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
        }

        clearMapConfirmationArmed = false;
        clearMapButton.setButtonText ("Limpar mapas");
        ccPickup.clear();
        saveMidiMap();
        deviceBar.setMappingCount (0);
        midiLearnPanel.setMappingCount (0);
        midiLearnPanel.showChooseTarget();
        juce::Logger::writeToLog ("MIDI MAP: todos os mapeamentos foram apagados");
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

    statusInfo.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusInfo.setJustificationType (juce::Justification::centred);   // centrado no chip de vidro
    statusInfo.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.58f));
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
    oled.fetchLevel = [this] (int l) -> float
    {
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
    librariesPanel.reset();  // encerra rede/download antes de desmontar os processadores
    aboutPanel.reset();
    saveChainState();

    masterProcessor->removeChangeListener (this);
    masterProcessor->removeListener (this);

    deviceManager.removeMidiInputDeviceCallback ({}, this);
    deviceManager.removeMidiInputDeviceCallback ({}, &player);
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
    g.setColour (NordLookAndFeel::nordRed());
    g.fillRect (juce::Rectangle<int> (0, headerArea.getBottom() - 2, getWidth(), 2));

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
    if (! programBand.isEmpty() && (oled.isVisible() || midiLearnPanel.isVisible()))
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


    if (auto sb = statusInfo.getBounds(); ! sb.isEmpty())
    {
        // mesmo rebaixamento dos chips de dispositivo: dado tecnico e contexto,
        // nao acao -- nao pode ter o mesmo peso visual de um botao
        auto chip = sb.toFloat().reduced (0.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (chip, NordLookAndFeel::radiusControl);
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.drawRoundedRectangle (chip.reduced (0.5f), NordLookAndFeel::radiusControl, 1.0f);
    }

    // ---- cartoes ----
    auto drawCard = [&g] (juce::Rectangle<int> r)
    {
        if (r.isEmpty())
            return;

        auto card = r.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (card.translated (0.0f, 2.0f), 9.0f);
        g.setColour (NordLookAndFeel::panel());
        g.fillRoundedRectangle (card, 9.0f);
        g.setColour (NordLookAndFeel::panelOutline());
        g.drawRoundedRectangle (card, 9.0f, 1.0f);

        auto accent = card.reduced (12.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f);
        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (accent, 1.5f);
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

    for (auto& d : midiIns)
    {
        const bool daw = LaunchkeyControl::isDawPort (d.name);
        deviceManager.setMidiInputDeviceEnabled (d.identifier, ! daw);

        // O filtro de porta DAW procura "MIDIIN2"/"MIDIOUT2"/"DAW", que sao
        // convencoes de nome do WINDOWS. Em outro sistema o mesmo teclado chega
        // com outro nome, entao registrar o nome REAL aqui e o que permite
        // descobrir se ele caiu no lado certo do filtro.
        juce::Logger::writeToLog ("MIDI IN: \"" + d.name + "\"  id=" + d.identifier
                                   + (daw ? "  -> DAW (desligada)" : "  -> LIGADA"));
    }

    // Um cabo retirado com pitch bend fora do centro nunca envia a volta para
    // 8192; o plugin ficaria desafinado indefinidamente. A lista mudou, entao
    // encerra vozes pendentes e injeta reset/center nos 16 canais.
    resetMidiTransportState();
}

void MainComponent::resetMidiTransportState()
{
    if (auto* live = getChurchLive())
    {
        live->panic();
        juce::Logger::writeToLog (
            "MIDI SAFETY: transporte recentralizado apos hot-plug/resume");
    }
}

void MainComponent::resized()
{
    CHURCHDAW_UI_SCOPE ("MainComponent::resized");
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
    const bool compact = getHeight() < 560;
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
    layoutProgramBandHeight = mobilePortraitLayout ? 52
                                                   : mobileShortLandscapeLayout ? 32
                                                                                : mobileProgramBandHeight;
    layoutBrandWidth = narrowPhone ? 58
                                   : mobileShortLandscapeLayout ? 132 : 166;
   #else
    layoutHeaderHeight = compact ? 46 : headerHeight;
    layoutProgramBandHeight = compact ? 60 : programBandHeight;
    layoutBrandWidth = narrowPhone ? 70 : compact ? 218 : 228;
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
    librariesButton.setButtonText (narrowPhone ? "Packs" : "Bibliotecas");
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
    statusInfo.setVisible (! compact && ! learning);

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
        headerControls.removeFromLeft (layoutBrandWidth);   // marca + separador
        headerControls.removeFromRight (compact ? 6 : 14);

        if (learning)
        {
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
            if (mobileShortLandscapeLayout)
                dockViewport.setViewPosition (0, 0);

            // A pagina touch tem altura minima para manter cada acao essencial
            // em 48 dp. Em paisagem baixa ela rola verticalmente; em retrato e
            // tablet normalmente cabe inteira. E preferivel uma pagina legivel
            // a achatar PANIC, M/S e seletores em faixas de 16--30 dp.
            const bool needsVerticalPage = ! mobileShortLandscapeLayout
                                        && area.getHeight() < 520;
            dockViewport.setScrollBarsShown (needsVerticalPage, false);
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
            dockedEditor->setSize (compact ? area.getWidth() : juce::jmax (1120, area.getWidth()),
                                   compact ? (mobileShortLandscapeLayout
                                                  ? area.getHeight()
                                                  : juce::jmax (520, area.getHeight()))
                                           : juce::jmax (400, area.getHeight()));
        }

        layoutDockHolder();
    }

    // card do Pad Continuo: centrado, um pouco abaixo do meio (flutua no mixer)
    if (padPanel != nullptr)
    {
        // so re-centraliza enquanto o usuario nao arrastou o card (a posicao
        // atual ainda e a ultima que o codigo setou)
        if (padPanelHome.isEmpty() || padPanel->getBounds() == padPanelHome)
        {
            auto pb = layoutContentBounds.withSizeKeepingCentre (padPanel->getWidth(), padPanel->getHeight());
            pb.setY (juce::jmax (layoutContentBounds.getY() + headerH + 12,
                                 layoutContentBounds.getCentreY() - padPanel->getHeight() / 2 + 30));
            padPanel->setBounds (pb);
            padPanelHome = pb;
        }
        else
        {
            // arrastado: respeita a posicao escolhida, so puxa de volta pra
            // dentro da janela se o resize deixou o card pra fora
            auto pb = padPanel->getBounds();
            pb.setX (juce::jlimit (layoutContentBounds.getX(),
                                   juce::jmax (layoutContentBounds.getX(),
                                               layoutContentBounds.getRight() - pb.getWidth()),
                                   pb.getX()));
            pb.setY (juce::jlimit (layoutContentBounds.getY() + headerH,
                                   juce::jmax (layoutContentBounds.getY() + headerH,
                                               layoutContentBounds.getBottom() - pb.getHeight()),
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

    appProperties.getUserSettings()->saveIfNeeded();
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
            if (d.isInstrument == wantInstrument)
                s.choices.add (d);

        for (int c = 0; c < s.choices.size(); ++c)
            s.box.addItem (s.choices.getReference (c).name, c + 2);

        // qualquer instrumento interno (inclusive setups antigos) usa a entrada unica
        bool matchedInternal = s.pluginIdentifier.startsWith ("internal:timbres");

        if (matchedInternal)
            s.box.setSelectedId (timbresComboId, juce::dontSendNotification);
        else if (s.pluginIdentifier.isNotEmpty())
            for (int c = 0; c < s.choices.size(); ++c)
                if (s.choices.getReference (c).createIdentifierString() == s.pluginIdentifier)
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
            return;   // ja carregado

        loadInternalTimbres (slotIndex, nullptr, 0);
        saveChainState();

        if (s.nodeID.uid != 0)
            openEditorForSlot (slotIndex);

        return;
    }

    const auto& desc = s.choices.getReference (id - 2);

    if (s.nodeID.uid != 0 && desc.createIdentifierString() == s.pluginIdentifier)
        return;   // ja carregado

    // rescan na mesma sessao: se o escolhido e o proprio plugin pendente,
    // restaura o estado preservado em vez de carregar com defaults
    juce::MemoryBlock pendingBlock;
    const juce::MemoryBlock* stateToUse = nullptr;

    if (s.pendingState.isNotEmpty()
        && desc.createIdentifierString() == s.pendingIdentifier
        && pendingBlock.fromBase64Encoding (s.pendingState))
        stateToUse = &pendingBlock;

    loadPlugin (slotIndex, desc, stateToUse);
    saveChainState();

    if (s.nodeID.uid != 0)
        openEditorForSlot (slotIndex);
}

void MainComponent::loadPlugin (int slotIndex, const juce::PluginDescription& desc, const juce::MemoryBlock* state)
{
    unloadSlot (slotIndex);

    juce::String error;
    auto instance = formatManager.createPluginInstance (desc, getCurrentSampleRate(), getCurrentBlockSize(), error);

    if (instance == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Erro ao carregar plugin",
                                                desc.name + ":\n" + error);
        // combo sem selecao (0), nao "(vazio)" (1): assim escolher "(vazio)"
        // depois dispara o onChange e o usuario consegue limpar a pendencia
        slots[slotIndex].box.setSelectedId (0, juce::dontSendNotification);
        slots[slotIndex].pluginIdentifier.clear();
        // falha de carga nao apaga o salvo: guarda pro autosave re-emitir.
        // Retentativa pelo combo chega com state == nullptr — nesse caso o
        // pendingState ja preservado (mesmo id) NAO pode ser sobrescrito.
        // E se a pendencia atual e de OUTRO plugin COM estado (A falhou no
        // restore, usuario tentou B que tambem falhou), a pendencia de A e a
        // que tem valor: nada carregou, entao ela continua valendo — instalar
        // B vazio por cima destruiria o setup de A no proximo autosave
        const auto failedId = desc.createIdentifierString();

        if (state != nullptr && state->getSize() > 0)
        {
            slots[slotIndex].pendingState = state->toBase64Encoding();
            slots[slotIndex].pendingIdentifier = failedId;
        }
        else if (slots[slotIndex].pendingIdentifier == failedId
                  || slots[slotIndex].pendingState.isEmpty())
        {
            slots[slotIndex].pendingIdentifier = failedId;   // sem pendencia valiosa em risco
        }

        return;
    }

    if (state != nullptr && state->getSize() > 0)
        instance->setStateInformation (state->getData(), (int) state->getSize());

    auto node = graph.addNode (std::move (instance));

    auto& s = slots[slotIndex];
    s.nodeID = node->nodeID;
    s.pluginIdentifier = desc.createIdentifierString();
    s.pendingIdentifier.clear();   // carregou de verdade: pendente nao vale mais
    s.pendingState.clear();
    s.openButton.setEnabled (true);
    s.proc.store (node->getProcessor());
    node->getProcessor()->addListener (this);   // para o MIDI Learn detectar o controle tocado

    for (int c = 0; c < s.choices.size(); ++c)
        if (s.choices.getReference (c).createIdentifierString() == s.pluginIdentifier)
            s.box.setSelectedId (c + 2, juce::dontSendNotification);

    rebuildGraphConnections();
}

void MainComponent::loadInternalTimbres (int slotIndex, const juce::MemoryBlock* state, int libraryIndex)
{
    unloadSlot (slotIndex);

    auto instance = std::make_unique<TimbresProcessor>();
    instance->setExternalMaster (masterProcessor);                    // master grandao dentro do Church Live
    instance->setPluginResources (&knownPlugins, &formatManager);     // plugins hospedados no canal master

    if (libraryIndex == 1)
        instance->setLibraryRoot (TimbresProcessor::redkeysSamplesFolder());
    else if (libraryIndex == 2)
        instance->setLibraryRoot (TimbresProcessor::worshipSamplesFolder());
    else if (libraryIndex == 3)
        instance->setLibraryRoot (TimbresProcessor::baselabSamplesFolder());

    if (state != nullptr && state->getSize() > 0)
        instance->setStateInformation (state->getData(), (int) state->getSize());

    auto node = graph.addNode (std::move (instance));

    auto& s = slots[slotIndex];
    s.nodeID = node->nodeID;
    s.pluginIdentifier = timbresIdentifiers[libraryIndex];
    s.pendingIdentifier.clear();
    s.pendingState.clear();
    s.openButton.setEnabled (true);
    s.proc.store (node->getProcessor());
    node->getProcessor()->addListener (this);
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

    if (s.nodeID.uid != 0)
    {
        if (dockedNodeID == s.nodeID)
            undockEditor();

        closeWindowForNode (s.nodeID);

        if (auto* node = graph.getNodeForId (s.nodeID))
        {
            node->getProcessor()->removeListener (this);

            // o card Audio/MIDI segura um attachment pro parametro de velocity
            // do Church Live; remover o no com o card aberto deixaria o
            // attachment apontando pra memoria morta
            if (audioPanel != nullptr && dynamic_cast<TimbresProcessor*> (node->getProcessor()) != nullptr)
                audioPanel.reset();
        }

        s.proc.store (nullptr);
        graph.removeNode (s.nodeID);
        s.nodeID = {};
        rebuildGraphConnections();
    }

    s.openButton.setEnabled (false);
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

        if (note == LaunchkeyControl::padBottom0 + 7 && live != nullptr)
            live->panic();
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
    if (message.isController())
    {
        const juce::String sourceId =
            "midi:" + (source != nullptr ? source->getIdentifier()
                                         : juce::String ("unknown"));
        processControllerCC (
            message.getControllerNumber(), message.getControllerValue(),
            sourceId, message.getChannel());
    }
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
        const juce::String capturedParamName =
            lastTouchedParamName;
        const juce::String capturedParamId =
            lastTouchedParamId;

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
    // Os plugins do MASTER FX vivem DENTRO do Church Live e podem ser carregados
    // por varios caminhos (restaurar setup, escolher no combo). Em vez de espalhar
    // addListener por cada caminho, uma varredura por segundo garante que todos
    // avisem quando um controle e tocado — sem isso o MIDI Learn nao ve o BigSky.
    // addListener e idempotente, entao repetir nao custa nada.
    for (int i = 0; i < numSlots; ++i)
        if (auto* timbres = dynamic_cast<TimbresProcessor*> (slots[i].proc.load()))
            for (int ins = 0; ins < TimbresProcessor::numInserts; ++ins)
                if (auto* fx = timbres->getInsert (ins))
                    fx->addListener (this);

    CHURCHDAW_UI_LAP ("timer:listeners");

    // Subiu para antes do bloco de MIDI: a lista de entradas agora e cacheada
    // por tempo (ver midiInputsCached) e precisa do relogio aqui em cima.
    const auto nowMs = juce::Time::getMillisecondCounter();

    // A porta DAW tem de ficar FORA da entrada MIDI comum. O estado de audio
    // salvo e o proprio dialogo Audio/MIDI podem reabilita-la sem querer — e ai
    // pad volta a tocar nota e a porta fica ocupada pro LaunchkeyControl.
    for (const auto& d : midiInputsCached (nowMs))
        if (LaunchkeyControl::isDawPort (d.name) && deviceManager.isMidiInputDeviceEnabled (d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, false);

    CHURCHDAW_UI_LAP ("timer:midi-daw");

    // Hot-plug real: se o teclado entrou depois do app, o objeto criado na
    // largada nao tinha portas e nunca se conectaria sozinho. Reabre no maximo
    // a cada 5 s, sem ficar brigando com drivers de porta exclusiva.

   #if JUCE_ANDROID
    // Tuner conservador de latencia. Ele roda na message thread, nunca no
    // callback de audio, ignora um xrun isolado e apenas AUMENTA o buffer.
    // Assim a primeira resposta continua em ~10,7 ms/48 kHz, mas um aparelho
    // lento se autocorrige em passos ate 1024 em vez de ficar estalando.
    // 10 s e UMA leitura de xrun por ciclo (era 5 s com duas): o
    // getXRunCount consulta o stream Oboe e disputa lock com o callback —
    // este bloco media 47 ms na message thread (timer:android-buffer),
    // o maior ofensor de jank conhecido. O tuner continua reagindo, so
    // que em ate 10 s.
    if (nowMs - androidLastBufferTuneMs >= 10000)
    {
        const int currentXRuns = deviceManager.getXRunCount();
        const int newXRuns = currentXRuns >= androidLastXRunCount
                                ? currentXRuns - androidLastXRunCount
                                : currentXRuns;
        bool bufferChanged = false;

        if (newXRuns >= 2)
            if (auto* device = deviceManager.getCurrentAudioDevice())
            {
                const int currentSize = device->getCurrentBufferSizeSamples();
                int nextSize = 0;

                for (const int candidate : device->getAvailableBufferSizes())
                    if (candidate > currentSize && candidate <= 1024)
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

                    juce::Logger::writeToLog (
                        "AUDIO: tuner Android " + juce::String (currentSize)
                        + " -> " + juce::String (nextSize)
                        + " apos " + juce::String (newXRuns)
                        + " xruns/10s"
                        + (error.isEmpty()
                               ? juce::String (" (ok)")
                               : " ERRO: " + error));
                }
            }

        // Reusa a leitura do topo — a segunda chamada era metade do custo.
        // So reconsulta quando o buffer mudou (o contador reinicia junto
        // com o stream).
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
        // compacto de proposito: precisa caber inteiro no chip (o "smp" e o
        // "kHz / " comiam a largura e o CPU sumia no reticencias)
        txt << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << "k" << sep
            << device->getCurrentBufferSizeSamples() << sep << "CPU "
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
            const int sel = live->getSelectedLayer();
            const auto* timbresEditor =
                dynamic_cast<const TimbresEditor*> (dockedEditor.get());
            const bool masterFocused =
                timbresEditor != nullptr
                && timbresEditor->isMobileMasterPageActive();

            auto nameOf = [live] (int l)
            {
                auto n = live->getLayerTimbre (l);

                if (n.isEmpty())
                    n = live->getLayerInstrumentName (l);

                // MESMO nome limpo que a regua mostra: o visor vinha com o cru
                // ("Black Upright LP4 FC") enquanto o canal ja mostrava limpo
                return TimbresProcessor::prettyLabel (n);
            };

            // Nenhum canal em foco: em vez de deixar a coluna oca, o visor conta
            // o estado do setup. Produto premium tem que ser bonito PARADO.
            if (masterFocused)
            {
                slot = "MASTER";
                title = juce::String (
                    juce::CharPointer_UTF8 ("Sa\xc3\xad" "da principal"));
            }
            else if (sel < 0)
            {
                int loaded = 0, on = 0;

                for (int l = 0; l < TimbresProcessor::numLayers; ++l)
                    if (nameOf (l).isNotEmpty())
                    {
                        ++loaded;

                        if (live->getLayerOn (l))
                            ++on;
                    }

                slot = "CHURCH LIVE";
                title = loaded == 0 ? juce::String ("Nenhum timbre")
                                    : juce::String (on) + " de " + juce::String (loaded) + " ativos";
            }
            else
            {
                slot = "CANAL " + juce::String (sel + 1);
                title = nameOf (sel);

                if (title.isEmpty())
                    title = "vazio";
            }

            // TODOS os canais, inclusive o em foco: o painel do Electro 7 existe
            // justamente para mostrar de relance o que esta carregado em cada um
            for (int l = 0; l < TimbresProcessor::numLayers; ++l)
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
                        tag = v <= 0.0001f ? juce::String ("-inf")
                                           : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB";
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
    int midiCount = 0;

    for (const auto& d : midiInputsCached (nowMs))
    {
        if (deviceManager.isMidiInputDeviceEnabled (d.identifier))
        {
            if (controllerName.isEmpty())
                controllerName = prettifyDeviceName (d.name);

            ++midiCount;
        }
    }

    if (midiCount > 1)
        controllerName << "  +" << (midiCount - 1);

    const int mappingCount = getMidiMappingCount();
    const bool recentMidi = nowMs - lastMidiActivityMs.load() < 1100;
    deviceBar.setControllerName (controllerName, midiCount > 0);
    deviceBar.setInterfaceName (ifaceName, ifaceOn);
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

    const int mappedCC = lastMappedCC.exchange (-1);

    if (mappedCC >= 0)
    {
        saveMidiMap();

        CCTarget mappedTarget;
        MidiControlKey mappedKey;
        {
            const juce::ScopedLock sl (mapLock);
            mappedKey = lastMappedKey;
            const auto it = ccMappings.find (mappedKey);

            if (it != ccMappings.end())
                mappedTarget = it->second;
        }

        const auto targetName = describeCCTarget (mappedTarget);
        const int count = getMidiMappingCount();
        midiLearnPanel.setMappingCount (count);
        deviceBar.setMappingCount (count);
        midiLearnPanel.showMapped (targetName.isNotEmpty() ? targetName : "controle", mappedCC);
        return;
    }

    if (! midiLearnActive.load())
        return;

    const int slot = lastTouchedSlot.load();
    const int param = lastTouchedParam.load();

    juce::String name;

    if (slot == -1)
    {
        name = "Master";
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
    return (int) ccMappings.size();
}

void MainComponent::saveMidiMap()
{
    juce::XmlElement xml ("MIDIMAP");

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

    appProperties.getUserSettings()->setValue ("midiMap", &xml);
    appProperties.getUserSettings()->saveIfNeeded();
}

void MainComponent::restoreMidiMap()
{
    int restored = 0;
    int rejectedUnsafe = 0;

    if (auto xml = appProperties.getUserSettings()->getXmlValue ("midiMap"))
        if (xml->hasTagName ("MIDIMAP"))
        {
            const juce::ScopedLock sl (mapLock);

            for (auto* m : xml->getChildIterator())
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

    if (rejectedUnsafe > 0)
        saveMidiMap();   // remove o alvo perigoso da proxima inicializacao
}

//==============================================================================
void MainComponent::rebuildGraphConnections()
{
    for (const auto& c : graph.getConnections())
        graph.removeConnection (c);

    juce::Array<juce::AudioProcessorGraph::NodeID> fxChain;

    for (int i = 1; i < numSlots; ++i)
        if (slots[i].nodeID.uid != 0)
            fxChain.add (slots[i].nodeID);

    const auto instrument = slots[0].nodeID;
    const auto midiCh = juce::AudioProcessorGraph::midiChannelIndex;

    // fontes que alimentam o comeco da cadeia de efeitos
    juce::Array<juce::AudioProcessorGraph::NodeID> sources;

    if (instrument.uid != 0)
        sources.add (instrument);

    if (inputToggle.getToggleState())
        sources.add (audioInNode);

    const auto firstDest = fxChain.isEmpty() ? masterNode : fxChain.getFirst();

    for (const auto& src : sources)
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { src, ch }, { firstDest, ch } });

    for (int k = 0; k + 1 < fxChain.size(); ++k)
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { fxChain[k], ch }, { fxChain[k + 1], ch } });

    if (! fxChain.isEmpty())
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { fxChain.getLast(), ch }, { masterNode, ch } });

    // pad continuo soma direto no master (passa pelo volume/medidor do master)
    if (padNode.uid != 0)
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { padNode, ch }, { masterNode, ch } });

    // master sempre alimenta a saida
    for (int ch = 0; ch < 2; ++ch)
        graph.addConnection ({ { masterNode, ch }, { audioOutNode, ch } });

    // MIDI vai para o instrumento e tambem para os efeitos (controle externo)
    if (instrument.uid != 0)
        graph.addConnection ({ { midiInNode, midiCh }, { instrument, midiCh } });

    for (const auto& fx : fxChain)
        graph.addConnection ({ { midiInNode, midiCh }, { fx, midiCh } });
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
struct AudioSettingsHolder : public juce::Component,
                             private juce::Timer
{
    static constexpr int margin = 12, headerH = 56;

    explicit AudioSettingsHolder (MainComponent& mc)
        : owner (&mc),
          selector (mc.deviceManager, 0, 2, 0, 2, true, false, true, false)
    {
        mc.audioDialogOpen = true;
        brandIcon = juce::ImageCache::getFromMemory (BinaryData::icon_png, BinaryData::icon_pngSize);

       #if JUCE_ANDROID
        selector.setItemHeight (38);
        selectorViewport.setViewedComponent (&selector, false);
        selectorViewport.setScrollBarsShown (true, false);
        selectorViewport.setScrollOnDragEnabled (true);
        selectorViewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (selectorViewport);
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
        audioInputNote.setText (juce::String::fromUTF8 ("\xe2\x9c\x93 O app n\xc3\xa3o usa o microfone"),
                                juce::dontSendNotification);
        audioInputNote.setJustificationType (juce::Justification::centredLeft);
        audioInputNote.setFont (juce::Font (juce::FontOptions (12.0f)));
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
        velLabel.setText (juce::String::fromUTF8 ("Resposta do teclado:"), juce::dontSendNotification);
        velLabel.setJustificationType (juce::Justification::centredRight);
        velLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        velLabel.setColour (juce::Label::textColourId, NordLookAndFeel::dimText());
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
        addAndMakeVisible (velCombo);

        // fecha o card (o destrutor salva a config)
        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
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

        // recupera o driver de "pipocos" SEM desplugar o cabo (e sem travar o app)
        restartBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x86\xbb Reiniciar \xc3\xa1udio"));   // ↻
        restartBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff8f0c21));
        restartBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        restartBtn.onClick = [this]
        {
            restartBtn.setEnabled (false);
            restartBtn.setButtonText ("Reiniciando...");

            juce::Component::SafePointer<AudioSettingsHolder> safe (this);
            juce::MessageManager::callAsync ([safe]
            {
                if (auto* h = safe.getComponent())
                    if (auto* mc2 = h->owner.getComponent())
                        mc2->restartAudioDevice();

                juce::Timer::callAfterDelay (600, [safe]
                {
                    if (auto* h = safe.getComponent())
                    {
                        h->restartBtn.setEnabled (true);
                        h->restartBtn.setButtonText (juce::String::fromUTF8 ("\xe2\x86\xbb Reiniciar \xc3\xa1udio"));
                    }
                });
            });
        };
        addAndMakeVisible (restartBtn);

        // 616 deixava ~140px de buraco embaixo do seletor: o card parecia
        // inacabado. A altura agora acompanha o conteudo real do JUCE.
        // +42 = a linha da resposta do teclado, acrescentada acima da faixa de baixo
       #if JUCE_ANDROID
        // O tamanho final vem de showAudioSettings(), usando a janela real.
        // O seletor permanece em escala 1:1 e rola somente a regiao de
        // configuracao quando a altura e curta.
        setSize (352, 650);
       #else
        setSize (620 + margin * 2, 542 + margin * 2);
       #endif
    }

    ~AudioSettingsHolder() override
    {
        stopTimer();

       #if JUCE_ANDROID
        selectorViewport.setViewedComponent (nullptr, false);
       #endif

        if (auto* c = owner.getComponent())
        {
            c->audioDialogOpen = false;
            c->saveAudioStateNow();
        }
    }

    void timerCallback() override { restyleSelector (selector); }

    // Acabamento por fora no seletor do JUCE: rotulos deixam de ter peso de
    // formulario e os botoes ganham hierarquia — "Resetar" e o unico perigoso,
    // e estava com a mesma aparencia de "Testar". Se o JUCE mudar os filhos, no
    // pior caso volta o visual padrao: nada quebra.
    static void restyleSelector (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            // o ComboBox desenha o VALOR num Label filho: apagar esse texto faria
            // o valor escolhido parecer desabilitado
            if (dynamic_cast<juce::ComboBox*> (child) != nullptr)
                continue;

            if (auto* label = dynamic_cast<juce::Label*> (child))
            {
                label->setColour (juce::Label::textColourId, NordLookAndFeel::dimText().withAlpha (0.8f));
                label->setFont (juce::Font (juce::FontOptions (12.0f)));
            }
            else if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                const auto text = button->getButtonText();

                if (text.containsIgnoreCase ("Resetar"))
                {
                    button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a2226));
                    button->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe08a6a));
                }
                else
                {
                    button->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
                    button->setColour (juce::TextButton::textColourOffId, NordLookAndFeel::textColour());
                }
            }

            restyleSelector (*child);   // as listas de canais/MIDI tem filhos proprios
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced ((float) margin);
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
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
       #if JUCE_ANDROID
        closeBtn.setBounds (
            card.getRight() - 48, card.getY() + 4, 48, 48);

        auto area = card;
        area.removeFromTop (headerH + 2);

        auto bottom = area.removeFromBottom (
            juce::jmin (104, area.getHeight() / 2));
        auto deviceRow = bottom.removeFromBottom (50);
        restartBtn.setBounds (
            deviceRow.removeFromRight (
                juce::jmin (148, deviceRow.getWidth() / 2)));
        deviceRow.removeFromRight (8);
        audioInputNote.setBounds (deviceRow);

        auto velRow = bottom.reduced (0, 3);
        velLabel.setJustificationType (
            juce::Justification::centredLeft);
        velLabel.setBounds (
            velRow.removeFromLeft (
                juce::jmin (122, velRow.getWidth() / 3)));
        velRow.removeFromLeft (8);
        velCombo.setBounds (
            velRow.withTrimmedRight (2)
                .withSizeKeepingCentre (
                    velRow.getWidth(),
                    juce::jmin (44, velRow.getHeight())));

        auto viewportArea = area.reduced (8, 4);
        selectorViewport.setBounds (viewportArea);
        const int selectorWidth = juce::jmax (
            260, viewportArea.getWidth() - 14);
        selector.setBounds (
            0, 0, selectorWidth,
            juce::jmax (420, viewportArea.getHeight()));
       #else
        closeBtn.setBounds (card.getRight() - 38, card.getY() + 12, 26, 22);

        auto area = card;
        area.removeFromTop (headerH + 4);

        auto extra = area.removeFromBottom (46).reduced (20, 10);
        restartBtn.setBounds (extra.removeFromRight (160));
        extra.removeFromRight (12);

       #if JUCE_ANDROID || JUCE_IOS
        audioInputNote.setBounds (extra);
       #else
        owner->inputToggle.setBounds (extra);
       #endif

        auto velRow = area.removeFromBottom (42).reduced (20, 8);
        velLabel.setBounds (velRow.removeFromLeft (150));
        velRow.removeFromLeft (10);
        velCombo.setBounds (velRow.removeFromLeft (190).withSizeKeepingCentre (190, 26));

        selector.setBounds (area.reduced (18, 8));
       #endif
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // separador acima da secao de baixo (resposta do teclado + entrada + reiniciar)
        auto row = getLocalBounds().reduced (margin).removeFromBottom (46 + 42);
        g.setColour (NordLookAndFeel::panelOutline());
        g.fillRect (row.removeFromTop (1).reduced (14, 0));
    }

    // arrasta o card pela faixa do cabecalho
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = e.position.y < (float) (margin + headerH);
        if (dragging)
            dragger.startDraggingComponent (this, e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging)
            dragger.dragComponent (this, e, nullptr);
    }

    void mouseUp (const juce::MouseEvent&) override   { dragging = false; }

    juce::Component::SafePointer<MainComponent> owner;
    juce::AudioDeviceSelectorComponent selector;
   #if JUCE_ANDROID
    juce::Viewport selectorViewport;
   #endif
    juce::TextButton restartBtn, closeBtn;
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
    deviceManager.closeAudioDevice();
    deviceManager.restartLastAudioDevice();
}

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
        return;
    }

    auto* holder = new AudioSettingsHolder (*this);
    audioPanel.reset (holder);
    addAndMakeVisible (holder);

   #if JUCE_ANDROID
    // No celular isto e uma PAGINA, nao um card flutuante de desktop: o
    // modal de 620x650 centrado deixava texto apertado, scrollbar interna e
    // o "Reiniciar audio" espremido (revisao de design de 30/07). A pagina
    // ocupa toda a area util; so o seletor interno rola.
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

    // Component::setTransform tambem transforma a posicao do componente no
    // pai. Portanto a coordenada logica precisa ser dividida pela escala; sem
    // isso o card cabia, mas ficava puxado para o canto superior esquerdo.
    const int visualX = juce::jmax (2, (getWidth()  - visualW) / 2);
    const int visualY = juce::jmax (2, (getHeight() - visualH) / 2);
    holder->setTopLeftPosition (juce::roundToInt ((float) visualX / panelScale),
                                juce::roundToInt ((float) visualY / panelScale));
   #endif
    holder->toFront (true);
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
struct PluginListHolder final : public juce::Component
{
    PluginListHolder (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& kl,
                      const juce::File& deadMansPedal, juce::PropertiesFile* props)
        : formats (fm), list (fm, kl, deadMansPedal, props)
    {
        list.setOptionsButtonText (juce::String::fromUTF8 ("Op\xc3\xa7\xc3\xb5" "es..."));
        addAndMakeVisible (list);

        scanButton.setButtonText ("Escanear VST3");
        scanButton.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::nordRed());
        scanButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        scanButton.onClick = [this] { scanNow(); };
        addAndMakeVisible (scanButton);

        setSize (880, 470);
    }

    void scanNow()
    {
        for (auto* format : formats.getFormats())
            if (format->getName() == "VST3")
                list.scanFor (*format);
    }

    void resized() override
    {
        list.setBounds (getLocalBounds());

        // mesmo rodape do "Opcoes..." (24px, com margem de 2), do lado direito
        auto strip = getLocalBounds().reduced (2).removeFromBottom (24);
        scanButton.setBounds (strip.removeFromRight (140));
    }

    juce::AudioPluginFormatManager& formats;
    juce::PluginListComponent list;
    juce::TextButton scanButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginListHolder)
};

void MainComponent::showPluginScanner()
{
    const auto deadMansPedal = appProperties.getUserSettings()->getFile()
                                   .getSiblingFile ("PluginsComProblema.txt");

    auto* holder = new PluginListHolder (formatManager, knownPlugins, deadMansPedal,
                                         appProperties.getUserSettings());

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (holder);
    o.dialogTitle = "Plugins VST3";
    o.componentToCentreAround = this;
    // LaunchOptions vem com dialogBackgroundColour = lightgrey: era dai o rodape
    // e as bordas claras que destoavam do app inteiro
    o.dialogBackgroundColour = NordLookAndFeel::background();
    o.useNativeTitleBar = false;   // barra de titulo desenhada pelo JUCE = escura
    o.resizable = true;
    o.launchAsync();

    // primeira vez: ja dispara o escaneamento de VST3
    if (knownPlugins.getNumTypes() == 0)
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
            "Instale a biblioteca Church Live em Packs para adicionar "
            "os pads e abra esta tela novamente.");
       #else
        const auto help = juce::String::fromUTF8 (
            "N\xc3\xa3o achei os WAVs de pad. Coloque pad_C.wav ... pad_B.wav "
            "na pasta 'ChurchDAW Pads' (ou 'Pads' ao lado do app).");
       #endif

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8 ("Pad Cont\xc3\xadnuo"),
            help);
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
void MainComponent::saveChainState()
{
    juce::XmlElement chain ("CHAIN");
    chain.setAttribute ("inputEnabled", inputToggle.getToggleState());
    chain.setAttribute ("masterDb", masterSlider.getValue());
    chain.setAttribute ("masterAutoEq", masterProcessor->getAutoEq());
    chain.setAttribute ("masterPunch", masterProcessor->getPunch());

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
                    if (dynamic_cast<TimbresProcessor*> (processor) != nullptr)
                    {
                        // TimbresProcessor serializa internamente troca de
                        // timbre + ENV/EQ + identidade. Nao inverta a ordem
                        // desse lock com callbackLock durante autosave.
                        processor->getStateInformation (mb);
                    }
                    else
                    {
                        const juce::ScopedLock lock (
                            processor->getCallbackLock());
                        processor->getStateInformation (mb);
                    }
                    captured = true;
                }
                catch (...)
                {
                    // Um plugin defeituoso nao pode impedir o restante da cadeia
                    // de ser salvo nem deixar o audio permanentemente suspenso.
                }

                processor->suspendProcessing (wasSuspended);

                if (captured)
                    slotXml->setAttribute ("state", mb.toBase64Encoding());
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
    appProperties.getUserSettings()->saveIfNeeded();
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

        if (auto desc = knownPlugins.getTypeForIdentifierString (id))
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
