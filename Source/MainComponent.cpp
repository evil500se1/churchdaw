#include "MainComponent.h"
#include "NordLookAndFeel.h"
#include "TimbresProcessor.h"
#include "ChordDetect.h"
#include "AboutPanel.h"
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
    glContext.setContinuousRepainting (false);
    glContext.attachTo (*this);
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

    // ZERO canais de ENTRADA em todas as plataformas, e nao e economia: o
    // ChurchDAW e um sampler e o no de entrada nao participa de nenhuma conexao
    // do graph. Pedir dois canais no desktop apenas acionava a permissao de
    // microfone do macOS/Windows sem haver caminho para esse audio no motor.
    //
    // Pedir entrada obriga o Android a exigir RECORD_AUDIO, e sem essa permissao
    // ele nao recusa so a entrada -- recusa o STREAM INTEIRO. O proprio Oboe
    // avisa no log: "AudioStreamAAudio.open() may have failed due to lack of
    // audio recording permission", e o app fica mudo, sem erro visivel na tela.
    //
    // O ChurchDAW e um sampler: ele nao grava nada. Pedir microfone seria pedir
    // ao comprador uma permissao que o app nao usa -- alem de assustar, e o tipo
    // de coisa que faz a Play Store barrar.
    const int numInputs = 0;

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
        // PISO no celular. Nao e preferencia, e prazo: a 256 amostras a funcao de
        // audio e chamada 172 vezes por segundo, e o custo FIXO de cada chamada
        // (percorrer o grafo, 5 camadas, efeitos) se paga 172 vezes. A 1024 sao
        // 43. O mesmo trabalho dividido por quatro.
        //
        // Medido no aparelho tocando com 256: a thread de audio ficou em 48-66%
        // de um nucleo -- media alta demais para uma tarefa que precisa terminar
        // DENTRO de cada bloco. Media alta significa picos que estouram o prazo,
        // e cada estouro e um estalo.
        //
        // Abaixo de 512 o motor nao entrega neste hardware, entao a opcao existir
        // no menu so serviria para o comprador escolher o defeito.
        // Vale TAMBEM quando nao ha nada salvo (wantedBuffer == 0). A versao
        // anterior so agia sobre valor salvo, entao numa instalacao limpa o piso
        // simplesmente nao existia -- e o aparelho abria em 256, que e onde o
        // motor nao entrega: medido com 22 vozes, pico de 106% do prazo.
        if (wantedBuffer < 1024)
            wantedBuffer = 1024;
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
    saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
    saveButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff5fd08a));
    saveButton.onClick = [this]
    {
        // salva TUDO agora: cadeia (timbres/volumes/efeitos), mapeamento MIDI e audio
        saveChainState();
        saveMidiMap();
        saveAudioStateNow();
        appProperties.getUserSettings()->saveIfNeeded();

        saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo!"));

        juce::Component::SafePointer<MainComponent> safe (this);
        juce::Timer::callAfterDelay (1400, [safe]
        {
            if (auto* c = safe.getComponent())
                c->saveButton.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x93 Salvo"));
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
        clearMapButton.setVisible (on);
        learnStatus.setText (on ? juce::String::fromUTF8 ("Toque um controle e mova o knob")
                                : juce::String(),
                             juce::dontSendNotification);

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
    clearMapButton.onClick = [this]
    {
        {
            const juce::ScopedLock sl (mapLock);
            ccMappings.clear();
        }
        saveMidiMap();
        learnStatus.setText ("Mapeamentos apagados", juce::dontSendNotification);
    };

    learnStatus.setFont (juce::Font (juce::FontOptions (12.0f)));
    learnStatus.setJustificationType (juce::Justification::centred);
    // ambar: mesma cor do botao "Mapear MIDI" ligado, entao a instrucao se le
    // como parte do modo armado e nao como um aviso solto
    learnStatus.setColour (juce::Label::textColourId, juce::Colour (0xffe0a83a));
    addAndMakeVisible (learnStatus);

    brandIcon = juce::ImageCache::getFromMemory (BinaryData::icon_png, BinaryData::icon_pngSize);

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
    masterSlider.setSkewFactorFromMidPoint (-12.0);
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

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (NordLookAndFeel::background());

    // ---- header GRAFITE com acento vermelho ----
    // Era uma chapa vermelha saturada de ponta a ponta: o elemento de maior
    // contraste da tela ficava na moldura, nao no mixer. Invertido — fundo escuro
    // e o vermelho volta a ser acento (filete, wordmark, botao ativo).
    auto headerArea = getLocalBounds().removeFromTop (headerHeight);
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

    if (brandIcon.isValid())
    {
        const auto box = juce::Rectangle<float> (17.0f, 3.5f, 46.0f, 46.0f);
        juce::Path plate;
        plate.addRoundedRectangle (box, 11.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 8, { 0, 2 }).drawForPath (g, plate);

        // O PNG do icone tem fundo PRETO (nao transparente). Recorta nos cantos
        // arredondados e desenha um pouco maior, pra moldura preta cair fora do
        // recorte em vez de virar um quadrado escuro no header grafite.
        juce::Graphics::ScopedSaveState clipped (g);
        g.reduceClipRegion (plate);
        g.setOpacity (1.0f);

        const auto draw = box.expanded (3.0f).toNearestInt();
        g.drawImageWithin (brandIcon, draw.getX(), draw.getY(), draw.getWidth(), draw.getHeight(),
                           juce::RectanglePlacement::centred);
    }

    auto titleFont = juce::Font (juce::FontOptions (22.0f)).boldened().italicised();
    g.setFont (titleFont);

    juce::GlyphArrangement measure;
    measure.addLineOfText (titleFont, "Church", 0.0f, 0.0f);
    const int churchW = (int) std::ceil (measure.getBoundingBox (0, -1, true).getWidth());
    const int textX = 18 + 44 + 8;

    g.setColour (juce::Colours::white);
    g.drawText ("Church", textX, 0, churchW + 4, headerHeight, juce::Justification::centredLeft);
    g.setColour (NordLookAndFeel::nordRedBright());   // "DAW" carrega a cor da marca
    g.drawText ("DAW", textX + churchW, 0, 200, headerHeight, juce::Justification::centredLeft);

    // separador vertical apos a marca
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRect (236, 11, 1, headerHeight - 22);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.fillRect (237, 11, 1, headerHeight - 22);

    // chip de vidro atras do status (kHz / buffer / CPU) — combina com os chips
    // de dispositivo do header
    // CHAPA DE CHASSI do painel de programa. Na foto do Electro 7 o display esta
    // encaixado no corpo do instrumento, nao flutuando: sem esta chapa o visor
    // ficava solto num vazio preto e parecia um cartao jogado na tela.
    if (! programBand.isEmpty() && oled.isVisible())
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

        // fio vermelho na base: assinatura da marca fechando o bloco
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.85f));
        g.fillRect (plate.getX(), plate.getBottom() - 1.0f, plate.getWidth(), 1.0f);

        // sombra projetada do visor sobre a chapa (ele esta EMBUTIDO, nao colado)
        auto ob = oled.getBounds().toFloat();
        juce::Path op;
        op.addRoundedRectangle (ob, 7.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.75f), 16, { 0, 3 }).drawForPath (g, op);

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
        strip.removeFromTop (headerHeight + programBandHeight + slotsRowActual);
        auto dockHeader = strip.removeFromTop (dockHeaderHeight);
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
        auto rest = getLocalBounds();
        rest.removeFromTop (headerHeight + programBandHeight + slotsRowActual);

        if (rest.getHeight() > 150)
        {
            const int size = juce::jlimit (120, 190, rest.getHeight() - 210);
            auto iconBox = rest.withSizeKeepingCentre (size, size)
                               .translated (0, -(size / 3 + 10));

            if (brandIcon.isValid())
            {
                g.setOpacity (0.05f);
                g.drawImageWithin (brandIcon, iconBox.getX(), iconBox.getY(), size, size,
                                   juce::RectanglePlacement::centred);
                g.setOpacity (1.0f);
            }

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
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

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
    // O corte em 560 e generoso de proposito: tablet de 10" tem folga de sobra e
    // continua com o layout de PC, identico ao Windows. Quem muda e so o celular.
    const bool compact = getHeight() < 560;

    const int headerH  = compact ? 46 : headerHeight;
    const int bandH    = compact ? 60 : programBandHeight;

    // larguras enxutas: cada pixel economizado aqui vira nome de aparelho legivel
    // nos chips (antes saia "Launchkey..." / "Focus...")
    auto headerControls = area.removeFromTop (headerH).reduced (12, compact ? 7 : 11);

    // PAINEL DE PROGRAMA: faixa propria logo abaixo do cabecalho, centralizado na
    // largura. Estava enfiado no cabecalho e ficava apertado e feio -- ele e um
    // bloco de painel, nao um chip de barra de ferramentas.
    {
        programBand = area.removeFromTop (bandH);

        // No compacto o visor usa a LARGURA TODA e quase toda a altura da faixa:
        // com 60 px nao ha o que sobrar para margem decorativa. Ele continua
        // sendo o elemento que se le de longe -- so deixa de ser um bloco de
        // 152 px de altura mostrando o mesmo texto.
        oled.setBounds (compact ? programBand.reduced (10, 4)
                                : programBand.withSizeKeepingCentre (juce::jmin (860, programBand.getWidth() - 48),
                                                                     programBand.getHeight() - 16));
    }
    headerControls.removeFromLeft (228);   // espaco da marca + separador
    audioSettingsButton.setBounds (headerControls.removeFromRight (104));
    headerControls.removeFromRight (7);
    scanButton.setBounds (headerControls.removeFromRight (84));
    headerControls.removeFromRight (7);
    padButton.setBounds (headerControls.removeFromRight (70));
    headerControls.removeFromRight (7);
    saveButton.setBounds (headerControls.removeFromRight (86));
    headerControls.removeFromRight (7);
    mapButton.setBounds (headerControls.removeFromRight (100));

    if (churchFull)
    {
        headerControls.removeFromRight (8);
        instrumentsButton.setBounds (headerControls.removeFromRight (108));
    }

    headerControls.removeFromRight (14);

    // O chip de status vinha DEPOIS dos chips de dispositivo e ficava com as
    // sobras — a 1280 sobravam ~88px e o texto saia "44.1 kHz / 1...", cortando
    // justo o buffer e o CPU. Agora ele reserva a largura dele PRIMEIRO.
    const bool learning = mapButton.getToggleState();

    if (learning)
    {
        clearMapButton.setBounds (headerControls.removeFromRight (92));
        headerControls.removeFromRight (6);
    }
    else
    {
        clearMapButton.setBounds ({});
    }

    // chip de status reserva a largura dele ANTES dos chips de dispositivo
    statusInfo.setBounds (headerControls.removeFromRight (juce::jmin (152, headerControls.getWidth())));
    headerControls.removeFromRight (10);

    // o visor de programa e o elemento mais largo do cabecalho, de proposito:
    // e o que voce le de longe no palco
    // sem reduzir na vertical: o visor precisa da altura toda do cabecalho, senao
    // nao sobra linha para o NOME -- que e o unico texto que importa de longe

    // No modo de mapeamento os chips de dispositivo SOMEM: dividir o espaco com
    // eles espremia os dois (chip virava um quadradinho e a instrucao quebrava).
    // Enquanto se aprende, saber o nome do teclado nao importa — a instrucao sim.
    deviceBar.setVisible (! learning);

    if (learning)
    {
        deviceBar.setBounds ({});
    }
    else
    {
        deviceBar.setBounds (headerControls.removeFromLeft (juce::jmin (392, headerControls.getWidth())));
        headerControls.removeFromLeft (12);
    }

    learnStatus.setBounds (headerControls);

    // Church Live em tela cheia: a fileira de cima some inteira e a mesa
    // ganha toda a altura da janela
    masterLabel.setVisible (! churchFull);
    masterSlider.setVisible (! churchFull);

    if (meter != nullptr)
        meter->setVisible (! churchFull);

    if (churchFull)
    {
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
    if (dockedEditor != nullptr)
    {
        if (! dockedNative)
        {
            auto dockHeader = area.removeFromTop (dockHeaderHeight);
            dockCloseButton.setBounds (dockHeader.removeFromRight (74).reduced (4, 3));
            dockFloatButton.setBounds (dockHeader.removeFromRight (90).reduced (4, 3));
            dockTitle.setBounds (dockHeader.reduced (14, 0));
        }

        dockViewport.setBounds (area);

        // o Church Live nativo preenche a area visivel; se a janela ficar
        // pequena demais (< 1120), o editor mantem um tamanho minimo confortavel
        // (6 colunas + master + controles) e o viewport ganha rolagem — nada some
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
            // No compacto o editor passa a valer EXATAMENTE a area disponivel:
            // ele tem compactacao propria e prefere se espremer a ser cortado.
            // Rolagem lateral continua permitida (mais canais que cabem na
            // largura e legitimo); a VERTICAL e desligada, porque na mesa de som
            // o que esta embaixo -- fader, mudo, solo -- e o que se usa tocando,
            // e nao pode depender de rolar a tela no meio do culto.
            dockedEditor->setSize (compact ? area.getWidth()  : juce::jmax (1120, area.getWidth()),
                                   compact ? area.getHeight() : juce::jmax (400,  area.getHeight()));
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
            auto pb = getLocalBounds().withSizeKeepingCentre (padPanel->getWidth(), padPanel->getHeight());
            pb.setY (juce::jmax (headerH + 12, getHeight() / 2 - padPanel->getHeight() / 2 + 30));
            padPanel->setBounds (pb);
            padPanelHome = pb;
        }
        else
        {
            // arrastado: respeita a posicao escolhida, so puxa de volta pra
            // dentro da janela se o resize deixou o card pra fora
            auto pb = padPanel->getBounds();
            pb.setX (juce::jlimit (0, juce::jmax (0, getWidth() - pb.getWidth()), pb.getX()));
            pb.setY (juce::jlimit (headerH, juce::jmax (headerH, getHeight() - pb.getHeight()), pb.getY()));
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
//                                            knob 7    -> transpose global
//                                            knob 8    -> volume do Pad
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
    launchkey = std::make_unique<LaunchkeyControl>();

    if (! launchkey->isConnected())
        return;

    launchkey->onFader = [this] (int index, float v)
    {
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
        auto* live = getChurchLive();

        if (index == 0)   // knob 1 = MIX do primeiro plugin do MASTER FX (BigSky)
        {
            if (live != nullptr)
                if (auto* fx = live->getInsert (0))
                    for (auto* p : fx->getParameters())
                        if (p->getName (16).containsIgnoreCase ("mix"))
                        {
                            p->setValueNotifyingHost (v);
                            break;
                        }

            return;
        }

        if (index == 1)   // knob 2 = volume do Pad Continuo
        {
            if (padProcessor != nullptr)
            {
                padProcessor->setGain (v * 1.5f);

                if (padPanel != nullptr)
                    padPanel->refreshState();
            }

            return;
        }

        if (index >= 2 && index <= 6)   // knobs 3-7 = COMP dos canais 1-5
        {
            if (live != nullptr)
                if (auto* p = live->getLayerCompParam (index - 2))
                    p->setValueNotifyingHost (v);

            return;
        }

        if (index == 7 && live != nullptr)   // knob 8 = transpose global
            if (auto* p = live->getGlobalTranspParam())
                p->setValueNotifyingHost (v);
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
        }

        const juce::String canal ("CANAL " + juce::String (i + 1));

        launchkey->setFaderName (i, timbre.isNotEmpty() ? canal + " " + timbre : canal + " vazio");
        launchkey->setEncoderName (i + 2, "COMP " + juce::String (i + 1));   // knobs 3-7
    }

    launchkey->setFaderName (8, "MASTER");
    launchkey->setEncoderName (0, "BIGSKY MIX");
    launchkey->setEncoderName (1, "PAD VOL");
    launchkey->setEncoderName (7, "TRANSPOSE");

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
void MainComponent::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    // roda na thread de MIDI - nao mexer em componentes daqui
    if (! message.isController())
        return;

    const int cc = message.getControllerNumber();
    const float value01 = (float) message.getControllerValue() / 127.0f;

    // CCs que NAO podem virar mapeamento: 64/66/67 sao os pedais (sustain,
    // sostenuto, soft) e 120-127 sao mensagens de modo do canal (all notes off,
    // reset...). Pisar no pedal com o Learn armado sequestrava o controle
    // selecionado — e ai o pedal passava a mexer num knob no meio do culto.
    const bool learnable = (cc != 64 && cc != 66 && cc != 67 && cc < 120);

    if (midiLearnActive.load() && learnable)
    {
        const int slot = lastTouchedSlot.load();
        const int param = lastTouchedParam.load();

        if (slot != -2)
        {
            {
                const juce::ScopedLock sl (mapLock);
                ccMappings[cc] = { slot, param, lastTouchedInsert.load(), lastTouchedParamName };
            }

            // mapeamento novo = pickup zerado: o fader tem que reencontrar o valor
            // do parametro novo antes de comecar a mexer nele. O clear fica pro
            // handleAsyncUpdate: ccPickup so pode ser tocado na message thread
            ccPickupToClear.store (cc);

            lastTouchedSlot.store (-2);
            lastTouchedParam.store (-1);
            lastTouchedInsert.store (-1);
            lastMappedCC.store (cc);
            suppressTouchUntilMs.store (juce::Time::getMillisecondCounter() + 800);
            triggerAsyncUpdate();
            return;
        }
    }

    CCTarget target;
    bool hasMapping = false;

    {
        const juce::ScopedLock sl (mapLock);
        const auto it = ccMappings.find (cc);

        if (it != ccMappings.end())
        {
            target = it->second;
            hasMapping = true;
        }
    }

    if (hasMapping)
    {
        auto safe = juce::Component::SafePointer<MainComponent> (this);

        juce::MessageManager::callAsync ([safe, target, value01, cc]
        {
            if (auto* c = safe.getComponent())
                c->applyCCTarget (target, value01, cc);   // cc identifica o pickup
        });
    }
}

void MainComponent::audioProcessorParameterChanged (juce::AudioProcessor* processor, int parameterIndex, float)
{
    // pode chegar em qualquer thread; usar apenas atomics
    if (! midiLearnActive.load() || applyingMappedCC)
        return;

    // plugins ecoam a mudanca logo depois que aplicamos/mapeamos um CC;
    // sem esta janela o modo Learn pediria o knob de novo em loop
    if (juce::Time::getMillisecondCounter() < suppressTouchUntilMs.load())
        return;

    if (processor == masterProcessor)
    {
        lastTouchedSlot.store (-1);   // -1 = Master
        lastTouchedParam.store (-1);
        triggerAsyncUpdate();
        return;
    }

    for (int i = 0; i < numSlots; ++i)
    {
        if (slots[i].proc.load() == processor)
        {
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

    // A porta DAW tem de ficar FORA da entrada MIDI comum. O estado de audio
    // salvo e o proprio dialogo Audio/MIDI podem reabilita-la sem querer — e ai
    // pad volta a tocar nota e a porta fica ocupada pro LaunchkeyControl.
    for (auto& d : juce::MidiInput::getAvailableDevices())
        if (LaunchkeyControl::isDawPort (d.name) && deviceManager.isMidiInputDeviceEnabled (d.identifier))
            deviceManager.setMidiInputDeviceEnabled (d.identifier, false);

    refreshLaunchkeyNames();   // so envia o que mudou

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

    // Visor de programa no formato do Electro 7: chapa vermelha com o canal em
    // foco, nome grande do timbre dele, e embaixo a lista dos outros canais --
    // que e como a Nord mostra as camadas empilhadas sob o programa.
    {
        juce::String slot, title (juce::CharPointer_UTF8 ("\xe2\x80\x94"));
        std::vector<OledDisplay::Row> rows;

        if (auto* live = getChurchLive())
        {
            const int sel = live->getSelectedLayer();

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
            if (sel < 0)
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

                rows.push_back ({ l + 1, icon, n, tag, dbVal, empty, live->getLayerOn (l), l == sel });
            }
        }
        else
        {
            slot = "CHURCHDAW";
            title = "sem Church Live";
        }

        oled.setProgram (slot, title, std::move (rows));
    }

    // (a faixa de teclado se alimenta sozinha a 30 Hz -- este timer e de 1 Hz)

    // chips premium: nome do controlador MIDI + interface de audio
    juce::String controllerName;
    int midiCount = 0;

    for (auto& d : juce::MidiInput::getAvailableDevices())
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

    deviceBar.setControllerName (controllerName, midiCount > 0);
    deviceBar.setInterfaceName (ifaceName, ifaceOn);
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
        learnStatus.setText ("CC " + juce::String (mappedCC) + " mapeado! Toque outro controle...",
                             juce::dontSendNotification);
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
                name = (insert >= 0 ? proc->getName() + " " : juce::String()) + paramName;

                // guarda o NOME pra ancorar o mapeamento (a thread de MIDI le isto
                // sob o mesmo lock quando o knob chegar)
                const juce::ScopedLock sl (mapLock);
                lastTouchedParamName = paramName;
            }
        }
    }
    else
    {
        const juce::ScopedLock sl (mapLock);
        lastTouchedParamName.clear();
    }

    if (name.isNotEmpty())
        learnStatus.setText (name + " selecionado - mexa um knob/fader do teclado",
                             juce::dontSendNotification);
}

// SOFT TAKEOVER (pickup). Um fader fisico nao sabe onde o parametro esta: se o
// piano esta em -6 dB e o fader do teclado esta la em cima, o primeiro toque
// joga o canal pra 0 dB no meio do louvor. Aqui o CC so "pega" o parametro
// quando o valor fisico ALCANCA o valor atual -- ate la, nao mexe em nada.
//
// Encoders relativos nao passam por isto (nao tem posicao absoluta), e o
// Launchkey ja resolve os faders dele em hardware; isto protege o resto.
bool MainComponent::ccHasPickedUp (int cc, float incoming, float current)
{
    auto& st = ccPickup[cc];

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

void MainComponent::applyCCTarget (CCTarget target, float value01, int ccNumber)
{
    suppressTouchUntilMs.store (juce::Time::getMillisecondCounter() + 600);
    applyingMappedCC = true;

    if (target.slot == -1)
    {
        const float cur = (float) masterSlider.valueToProportionOfLength (masterSlider.getValue());

        if (ccNumber < 0 || ccHasPickedUp (ccNumber, value01, cur))
            masterSlider.setValue (masterSlider.proportionOfLengthToValue ((double) value01),
                                   juce::sendNotificationSync);
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
            int index = -1;

            if (target.paramName.isNotEmpty())
            {
                // caminho rapido: o indice guardado ainda aponta pro mesmo nome
                if (juce::isPositiveAndBelow (target.param, params.size())
                     && params[target.param]->getName (32) == target.paramName)
                {
                    index = target.param;
                }
                else
                {
                    // O plugin do slot mudou (ou a ordem dos parametros mudou entre
                    // versoes). Procura pelo nome: se achar, o mapeamento se cura
                    // sozinho; se NAO achar, ignora — melhor um knob que nao faz
                    // nada do que um knob que mexe no controle errado sem avisar.
                    for (int i = 0; i < params.size(); ++i)
                        if (params[i]->getName (32) == target.paramName)
                        {
                            index = i;
                            break;
                        }
                }
            }
            else if (juce::isPositiveAndBelow (target.param, params.size()))
            {
                index = target.param;   // mapeamentos antigos, salvos sem nome
            }

            if (index >= 0)
            {
                const float cur = params[index]->getValue();

                if (ccNumber < 0 || ccHasPickedUp (ccNumber, value01, cur))
                    params[index]->setValueNotifyingHost (value01);
            }
        }
    }

    applyingMappedCC = false;
}

void MainComponent::saveMidiMap()
{
    juce::XmlElement xml ("MIDIMAP");

    {
        const juce::ScopedLock sl (mapLock);

        for (const auto& pair : ccMappings)
        {
            auto* m = xml.createNewChildElement ("MAP");
            m->setAttribute ("cc", pair.first);
            m->setAttribute ("slot", pair.second.slot);
            m->setAttribute ("param", pair.second.param);
            m->setAttribute ("insert", pair.second.insert);
            m->setAttribute ("name", pair.second.paramName);
        }
    }

    appProperties.getUserSettings()->setValue ("midiMap", &xml);
    appProperties.getUserSettings()->saveIfNeeded();
}

void MainComponent::restoreMidiMap()
{
    if (auto xml = appProperties.getUserSettings()->getXmlValue ("midiMap"))
        if (xml->hasTagName ("MIDIMAP"))
        {
            const juce::ScopedLock sl (mapLock);

            for (auto* m : xml->getChildIterator())
                ccMappings[m->getIntAttribute ("cc")] = { m->getIntAttribute ("slot", -2),
                                                          m->getIntAttribute ("param", -1),
                                                          m->getIntAttribute ("insert", -1),      // mapas antigos: -1
                                                          m->getStringAttribute ("name") };       // e sem nome
        }
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

        selector.setItemHeight (26);
        addAndMakeVisible (selector);

        // O seletor de audio e do JUCE: reescrever ele so pra ficar bonito seria
        // trocar um componente testado (troca de driver, ASIO, canais, sample rate)
        // por bugs novos num app que vai pro palco. Em vez disso, o acabamento e
        // aplicado por FORA, nos filhos dele. O timer reaplica porque o JUCE
        // recria esses filhos quando se troca o tipo de driver.
        restyleSelector (selector);
        startTimer (700);

        // a entrada de guitarra/mic saiu do header premium e vive aqui agora
        addAndMakeVisible (mc.inputToggle);

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
        setSize (620 + margin * 2, 542 + margin * 2);
    }

    ~AudioSettingsHolder() override
    {
        stopTimer();

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

        g.setColour (NordLookAndFeel::dimText().withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (juce::String::fromUTF8 ("Dispositivo, lat\xc3\xaancia e entradas MIDI"),
                    head.reduced (16, 0).withTrimmedRight (36).withTrimmedTop (6),
                    juce::Justification::centredRight);

        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.55f));
        g.fillRect (head.getX() + 14, head.getBottom() - 1, head.getWidth() - 28, 1);
    }

    void resized() override
    {
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
    juce::TextButton restartBtn, closeBtn;
    juce::Label velLabel;
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

void MainComponent::showAudioSettings()
{
    // toggle: se ja esta aberto, fecha (o destrutor salva a config)
    if (audioPanel != nullptr)
    {
        audioPanel.reset();
        return;
    }

    auto* holder = new AudioSettingsHolder (*this);
    audioPanel.reset (holder);
    addAndMakeVisible (holder);

    // No celular em paisagem a area logica costuma ter so ~360 px de altura,
    // enquanto o seletor completo do JUCE precisa de 566. Antes o painel era
    // cortado pelo rodape e os controles de MIDI/reinicio ficavam inacessiveis.
    // A transformacao escala desenho e hit-testing juntos; em tablet e PC, onde
    // ha espaco, o fator permanece exatamente 1.
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
    holder->toFront (true);
}

//==============================================================================
// Marca do header = botao de "Sobre". Nao gasta espaco na fileira de botoes e
// e o lugar onde todo mundo procura os creditos.
juce::Rectangle<int> MainComponent::brandArea() const
{
    return { 12, 3, 216, headerHeight - 8 };
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
            "O Pad Cont\xc3\xadnuo ainda n\xc3\xa3o est\xc3\xa1 instalado. "
            "Copie pad_C.wav ... pad_B.wav para "
            "M\xc3\xbasica/ChurchDAW/Pads e abra o Pad novamente.");
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
                node->getProcessor()->getStateInformation (mb);
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

    const auto masterDb = (float) xml->getDoubleAttribute ("masterDb", 0.0);
    masterSlider.setValue (masterDb, juce::dontSendNotification);
    masterProcessor->setMasterDb (masterDb);
    masterProcessor->setAutoEq (xml->getBoolAttribute ("masterAutoEq", false));
    masterProcessor->setPunch (xml->getBoolAttribute ("masterPunch", false));

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
