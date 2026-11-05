#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "AppPaths.h"   // define CHURCHDAW_HOSTS_PLUGINS
#include "ChurchDAWAudioActivity.h"
#include "PianoVisualState.h"
#include "PerformanceDock.h"
#include "Studio/StudioCaptureState.h"
#include "Plugins/Sabrina/SabrinaProcessor.h"

#if JUCE_ANDROID || JUCE_IOS
 #include <juce_opengl/juce_opengl.h>
#endif

#include "MasterProcessor.h"
#include "LevelMeter.h"
#include "DeviceInfoBar.h"
#include "ChurchLiveSkin.h"
#include "AudioMidiButton.h"
#include "PadProcessor.h"
#include "PadPanel.h"
#include "LaunchkeyControl.h"
#include "MidiLearnPanel.h"
#include "MidiMappingPanel.h"
#include "MidiLearnConsoleOverlay.h"
#include "MidiLearnTargetTag.h"
#include "MidiMappingSafety.h"
#include "OledDisplay.h"
#include "LiveScenes.h"
#include "LiveScenePanel.h"
#include "ScenesPage.h"
#include "ChurchDAWRemoteServer.h"
#include "ChurchDAWPlaybackEngine.h"
#include "FinalOutputGuard.h"
#include "DiagnosticTestTone.h"

// O controle remoto ainda nao possui uma tela de pareamento segura no app.
// Releases ficam sem listener de rede ate a feature ser habilitada
// explicitamente junto com o fluxo de pareamento.
#ifndef CHURCHDAW_REMOTE_CONTROL_ENABLED
 #define CHURCHDAW_REMOTE_CONTROL_ENABLED 0
#endif

#if CHURCHDAW_HOSTS_PLUGINS
 #include "PluginScanSubprocessV2.h"
#endif

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <vector>

class PluginWindow;
class TimbresProcessor;   // definido em TimbresProcessor.h (incluido so no .cpp)
class PadSynthProcessor;
class PadEditorPanel;
class PerformanceSessionPanel;

namespace ChurchDAWAudioInputPolicy
{
inline bool isExplicitlyAuthorised (const juce::XmlElement* chain) noexcept
{
    return chain != nullptr
        && chain->hasTagName ("CHAIN")
        && chain->getBoolAttribute ("inputEnabled", false);
}

// AudioDeviceManager::initialise() restaura o XML literalmente. Quando
// audioDeviceInChans nao existe, o JUCE usa "11" (duas entradas), mesmo que o
// chamador tenha pedido initialise (0, 2, ...). Grave a intencao no proprio
// snapshot sem apagar o dispositivo de entrada escolhido pelo usuario.
inline void applyToSavedSetup (juce::XmlElement* audioSetup,
                               bool authorised)
{
    if (audioSetup != nullptr && ! authorised)
        audioSetup->setAttribute ("audioDeviceInChans", "0");
}

inline void disableRuntimeChannels (
    juce::AudioDeviceManager::AudioDeviceSetup& setup) noexcept
{
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
}

inline bool runtimeSetupIsBlocked (
    const juce::AudioDeviceManager::AudioDeviceSetup& setup) noexcept
{
    return ! setup.useDefaultInputChannels && setup.inputChannels.isZero();
}
}

//==============================================================================
// Componente principal: motor de audio (grafo de plugins) + interface.
//
// Cadeia fixa:  [MIDI] -> Instrumento -> FX1 -> FX2 -> FX3 -> Master -> [Saida]
//               [Entrada de audio] ---^ (opcional, para guitarra/mic)
//==============================================================================
class MainComponent : public juce::Component,
                      private juce::ChangeListener,
                      private juce::ComponentListener,
                      private juce::MidiInputCallback,
                      private juce::AudioIODeviceCallback,
                      private juce::AudioProcessorListener,
                      private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    explicit MainComponent (
        bool safeMode = false,
        ChurchDAWMidiMapping::DeferredOwnerTeardownGate*
            ownerTeardownGate = nullptr);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    void closeWindowForNode (juce::AudioProcessorGraph::NodeID nodeID);
    void showPadWindow();
    void togglePadContinuous();
    void toggleMidiLearn();
    void startMidiLearn();
    void finishMidiLearn (bool saveMappings = true);

    // ---- estado de carregamento (a splash so sai quando isto fica pronto) ----
    bool isStillLoading() const;
    bool isSafeMode() const noexcept { return safeMode; }
    juce::String getLoadingDetail() const;
    float getLoadingProgress() const;

    // Botao BACK fisico do Android. Fecha a camada de cima (organ ->
    // mixer; Audio/MIDI -> mixer; Bibliotecas -> mixer) e devolve true.
    // False = nada aberto, o chamador decide se o app sai.
    bool handleBackButton();

    // O demo encerra com um fade do master antes de destruir o grafo. Nunca
    // corte uma cauda de reverb ou nota sustentada na troca para o gate.
    void beginLicenseFadeOut();

    // Called only by the application-owned teardown coordinator. It closes
    // admission before the MainComponent can be deleted; readiness is polled
    // asynchronously by that stable owner, never by this component's dtor.
    void beginOwnerTeardown() noexcept;
    bool ownerTeardownIsReady() const noexcept;

    // Hot-plug, sleep/resume e troca de rota podem perder o pitch-wheel center
    // ou um note-off. Recentraliza todos os 16 canais no proximo bloco. O
    // hot-plug pode preservar/reaplicar o OR dos pedais que continuam ligados;
    // PANIC deliberado limpa tudo.
    void resetMidiTransportState (bool preservePhysicalSustain = false);
    void handleMemoryWarning();
   #if JUCE_IOS || JUCE_ANDROID
    void suspendChurchCam();
    void resumeChurchCam();
    void ensureMobileAudioRunningAfterResume();
   #endif

private:
    const bool safeMode = false;
    ChurchDAWMidiMapping::DeferredOwnerTeardownGate* const
        ownerTeardownGate = nullptr;
    // SafePointer remains valid throughout the derived destructor body. Any
    // queued callback that can be pumped by a hosted-plugin destructor must
    // also observe this gate before touching graph/processors.
    std::atomic<bool> componentShutdownStarted { false };
    static constexpr int numFxSlots = 3;
    static constexpr int numSlots   = numFxSlots + 1;   // slot 0 = instrumento

    static constexpr int headerHeight    = 64;
    static constexpr int mobileHeaderHeight = 38;
    // faixa propria do painel de programa, abaixo do cabecalho. Ele NAO e peca de
    // cabecalho: e um bloco centralizado, como o display no painel do Electro 7.
    static constexpr int programBandHeight = 112;   // visor 29% mais baixo, sem perder as 5 linhas
    static constexpr int compactProgramBandHeight = 28;
    static constexpr int mobileProgramBandHeight = 44;
    juce::Rectangle<int> programBand;   // chapa de chassi onde o visor se apoia
    juce::String programLibrary;        // biblioteca do canal em foco (serigrafia direita)
    static constexpr int slotsRowHeight  = 168;
    static constexpr int dockHeaderHeight = 30;
    static constexpr int masterStripWidth = 150;

    struct Slot
    {
        juce::Label label;
        juce::ComboBox box;
        juce::TextButton openButton { "Abrir interface" };
        juce::AudioProcessorGraph::NodeID nodeID {};
        juce::String pluginIdentifier;
        // Canonical mapping identity, prepared on load/commit and read under
        // mapLock. Parameter/MIDI callbacks never inspect PluginDescription.
        juce::String mappingIdentity;
        // plugin salvo mas ausente/falho: re-emitido no saveChainState pra nao
        // apagar o setup; limpo quando qualquer coisa carrega no slot
        juce::String pendingIdentifier;
        juce::String pendingState;   // base64
        juce::String loadingIdentifier;   // pedido async ainda nao confirmado
        juce::Array<juce::PluginDescription> choices;
        std::atomic<juce::AudioProcessor*> proc { nullptr };   // para identificar o plugin tocado no MIDI Learn
        // Cached when an internal TimbresProcessor is installed. Parameter
        // callbacks can synchronise the Pad from newValue after raw-pointer
        // validation, without dynamic_cast/dereferencing the callback source.
        std::atomic<int> globalTransposeParameterIndex { -1 };
        ChurchDAWMidiMapping::SinglePendingRetirementSlot
            pendingNodeRetirement;
        juce::uint64 loadGeneration = 0;   // invalida callbacks async antigos (AUv3 inclusive)
    };

    // ---- motor de audio ----
   #if JUCE_ANDROID
    // DESENHO PELA GPU.
    //
    // Sem isto o JUCE rasteriza a janela inteira por SOFTWARE e envia o
    // resultado como textura a cada quadro. Numa tela de 2460x1080 sao 10,6 MB
    // por quadro. Medido no aparelho com dumpsys gfxinfo:
    //
    //     Janky frames: 261 de 261 (100%)
    //     50o percentil: 250 ms por quadro     -> 4 FPS
    //     Slow UI thread: 261      (todos)
    //     Slow bitmap uploads: 261 (todos)
    //     GPU: 3-4 ms              <- a GPU estava OCIOSA
    //
    // Era esta a lentidao ao navegar entre menus, e nao falta de maquina: o
    // trabalho estava indo todo para a CPU enquanto a GPU nao fazia nada.
    // NOTA: glContext fica declarado por ULTIMO na classe para ser destruido
    // ANTES de qualquer componente filho ou processador durante o teardown.
   #endif

    juce::AudioDeviceManager deviceManager;

    // Reexecuta a habilitacao das entradas MIDI quando o sistema avisa que a
    // lista mudou -- no Android a descoberta e assincrona e a lista esta VAZIA
    // no momento em que o construtor pergunta.
    void refreshMidiInputs();
    juce::MidiDeviceListConnection midiListConnection;

    // Entradas MIDI ligadas na ultima varredura. Serve para distinguir
    // "apareceu um aparelho" (nao mexe no som) de "sumiu um aparelho"
    // (ai sim pode haver nota presa e o panico se justifica).
    juce::StringArray entradasMidiConhecidas;
    // Lista CRUA (incluindo portas DAW) da ultima varredura. O poll de
    // reconexao compara esta lista com a atual para detectar hot-plug que o
    // watcher WinRT perde; entradasMidiConhecidas e a versao FILTRADA (sem
    // portas DAW) e nao pode ser a base da comparacao.
    juce::StringArray ultimaListaMidiCrua;
    // Nome do aparelho -> identificador MIDI. No Android o identificador e
    // efemero (muda a cada replug/reinicio), entao os mapas salvos por id
    // morriam. Este mapa permite reescrever os sourceIds quando o mesmo
    // aparelho (mesmo NOME) volta com um id novo.
    std::map<juce::String, juce::String> nomeParaIdMidi;
    // Hot-plug rewrites this vector while MIDI callbacks compare physical
    // controls against it. Every access is therefore protected by mapLock.
    std::vector<ChurchDAWMidiMapping::MidiSourceDescriptor> midiSourcesCached;
    std::set<juce::String> automaticControllerProfilesApplied;
    bool automaticControllerProfilesSuspended = false;
    bool midiMapsLoaded = false;
    ChurchDAWMidiMapping::MidiMapLifecycleGate midiMapLifecycle;
    std::unique_ptr<ChurchDAWMidiMapping::MidiMapPairFileLock>
        midiMapFileLock;
    // Digest of the exact CC/note primary + last-good quartet loaded by this
    // app instance. The process lock serialises writers; this revision guard
    // also stops an older, still-open instance from overwriting newer maps.
    ChurchDAWMidiMapping::MidiMapPairRevision midiMapDiskRevision;
    ChurchDAWMidiMapping::MidiDirtyEpoch ccDirtyEpoch;
    ChurchDAWMidiMapping::MidiDirtyEpoch noteDirtyEpoch;
    std::atomic<bool> midiMapTransactionInProgress { false };
    std::atomic<juce::uint64> midiMappingGeneration { 1 };
    bool midiMapPairRecoveryFailed = false;
    std::atomic<bool> midiMapPersistenceError { false };
    std::atomic<int> ccSaveRescheduleCount { 0 };
    std::atomic<int> noteSaveRescheduleCount { 0 };
    bool pendingLegacyCCMigration = false;
    bool pendingLegacyNoteMigration = false;
    std::shared_ptr<std::atomic<bool>> midiCallbackLifetime {
        std::make_shared<std::atomic<bool>> (true) };
    juce::CriticalSection midiCallbackBarrier;
    ChurchDAWMidiMapping::ParameterCallbackLifetimeGate
        parameterCallbackGate;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    // IDs da blacklist antiga permanecem indisponiveis enquanto a validacao
    // isolada ainda nao terminou. Isto fecha a janela entre removeFromBlacklist
    // e o resultado do subprocesso, inclusive depois de cancelar/fechar o app.
    juce::StringArray pluginScanQuarantine;
   #if CHURCHDAW_HOSTS_PLUGINS
    // O conteudo do dialogo referencia members deste MainComponent. Ownership
    // explicito garante que ele morra sincronicamente antes desses members.
    std::unique_ptr<juce::DialogWindow> pluginScannerDialog;
   #endif
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    ChurchDAWPlaybackEngine playbackEngine;
    FinalOutputGuard finalOutputGuard;
    DiagnosticTestTone diagnosticTestTone;
    juce::AudioProcessorGraph::NodeID audioInNode, audioOutNode, midiInNode,
                                      masterNode, padNode, padSynthNode;
    MasterProcessor* masterProcessor = nullptr;   // pertence ao grafo
    PadProcessor* padProcessor = nullptr;         // pad continuo (pertence ao grafo)
    PadSynthProcessor* padSynthProcessor = nullptr; // motor do PAD EDITOR (pertence ao grafo)

    juce::ApplicationProperties appProperties;
    bool suppressAudioStateSave = true;   // nao salvar estados transitorios durante a inicializacao
    bool audioDialogOpen = false;         // config de audio so e salva com o dialogo aberto (escolha do usuario)
    bool launchkeyMappingEnabled = true;  // mapeamento automatico da superficie DAW
    std::atomic<bool> audioInputAuthorised { false };

    // Sonda da saida FINAL do callback. Acumula somente apos 5 s sem nota/CC e
    // com entrada fisica desautorizada; o timer publica pico/RMS a cada 10 s.
    // Uma atualizacao atomica por bloco, sem lock nem alocacao na thread de audio.
    std::atomic<float> idleOutputPeak { 0.0f };
    std::atomic<double> idleOutputSquareSum { 0.0 };
    std::atomic<juce::uint64> idleOutputSampleCount { 0 };
    std::atomic<juce::uint64> idleOutputNonFiniteCount { 0 };
    std::atomic<juce::uint64> blockedInputCallbackCount { 0 };
    std::atomic<int> blockedInputChannelMaximum { 0 };

   #if CHURCHDAW_AUDIO_INSTRUMENTATION
    std::atomic<juce::int64> diagPlayerTicks { 0 };
    std::atomic<juce::int64> diagPlaybackTicks { 0 };
    std::atomic<juce::uint64> diagCallbackSamples { 0 };
   #endif

   #if JUCE_ANDROID
    void prepareAndroidAudioThread() noexcept;
    bool audioAffinitySet = false;
   #endif

    friend struct AudioSettingsHolder;
    friend class AndroidAudioMidiCenter;

    void saveAudioStateNow();

    // Promove o setup de audio atual a "ultima configuracao boa". So roda
    // depois de o app sobreviver alguns segundos com o device aberto.
    void promoteGoodAudioNow();
    int audioGoodPromoTicks = 0;

    // Saturador nativo no barramento master. NASCE EM BYPASS: enquanto o
    // musico nao ligar, o som e identico ao de antes (o processBlock nem
    // toca no buffer).
    juce::AudioProcessorGraph::NodeID sabrinaNode {};
    SabrinaProcessor* sabrinaProcessor = nullptr;
    bool sabrinaEnabled = false;
    void setSabrinaEnabled (bool enabled);
    void restartAudioDevice();   // recupera o driver ASIO sem desplugar (anti-pipoco)
    bool setAudioInputChannelsEnabled (bool enabled);
    bool reconcileAudioInputPolicy();
   #if ! (JUCE_ANDROID || JUCE_IOS)
    void tuneDesktopFirstRunAudio();   // ASIO/Low Latency + ~10 ms na 1a saida vista
   #endif
   #if JUCE_WINDOWS || JUCE_MAC
    // App aberto sem saida (fone/caixa desplugados) conecta sozinho quando uma
    // saida aparece. Uma tentativa por mudanca real da lista de saidas.
    void reconnectAudioOutputIfAvailable();
    juce::String lastAutoReconnectOutputs;
   #endif

    void audioDeviceIOCallbackWithContext (
        const float* const* inputChannelData, int numInputChannels,
        float* const* outputChannelData, int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;
    juce::String lastAudioDriverError;
    void showPlaybackFileChooser();
    void startPlayback();
    void stopPlayback();
    void startDiagnosticOutputTest() noexcept { diagnosticTestTone.requestStart(); }
    void stopDiagnosticOutputTest() noexcept { diagnosticTestTone.requestStop(); }

   #if JUCE_ANDROID
    void pollAndroidAudioTopology();
    void reconcileAndroidAudioTopology();
    void updateAndroidPreferredUsbSetupFromCurrent();
   #endif

    // ---- interface ----
    AudioMidiButton audioSettingsButton { "Audio / MIDI" };
    std::shared_ptr<PianoVisualState> pianoVisualState = std::make_shared<PianoVisualState>();
    PerformanceDock performanceDock { pianoVisualState };
    std::shared_ptr<churchstudio::AudioTap> studioAudioTap = std::make_shared<churchstudio::AudioTap>();
   #if JUCE_IOS || JUCE_ANDROID
    std::shared_ptr<churchstudio::AudioTap> studioMicrophoneTap = std::make_shared<churchstudio::AudioTap>();
    std::atomic<bool> studioMicRequested { false };
    juce::Result setStudioMicrophoneEnabled (bool enabled, int channel);
   #endif
    juce::TextButton liveCamButton { "Live Cam" };
    juce::Component::SafePointer<juce::DocumentWindow> liveCamWindow;
    juce::TextButton scanButton { "Plugins" };
    juce::TextButton saveButton { "Salvar" };
    // Saturador nativo do barramento master. Nasce em bypass: o musico liga
    // quando quiser mais corpo no som.
    juce::TextButton sabrinaButton { "Sabrina" };
    juce::Colour corDoSalvoNormal;   // verde do "Salvo"; guardado para voltar apos um "FALHOU"
    int geracaoDoAvisoDeSalvar = 0;  // so o clique mais recente manda no texto/cor do botao
    juce::TextButton padButton { "Pad" };   // abre o workspace completo PAD EDITOR
    juce::TextButton librariesButton { "Bibliotecas" };
    std::unique_ptr<PadPanel> padPanel;     // card embutido (nao e janela separada)
    std::unique_ptr<PadEditorPanel> padEditorPanel; // pagina premium do sintetizador de pads
    std::unique_ptr<PerformanceSessionPanel> performanceSessionPanel;
    // Pagina CENAS do desktop (botao do trilho que substituiu "Instrumentos").
    std::unique_ptr<ScenesPage> scenesPage;
    bool scenesPageVisible = false;
    bool padEditorVisible = false;
    // ultima posicao do card do Pad setada por codigo: se a atual difere, o
    // usuario arrastou e o resized() nao pode re-centralizar
    juce::Rectangle<int> padPanelHome;
    std::unique_ptr<juce::Component> audioPanel;   // card Audio/MIDI embutido
    std::unique_ptr<juce::Component> aboutPanel;   // card "Sobre" (clique na marca)
    std::unique_ptr<juce::Component> librariesPanel; // catalogo/download de packs
    bool brandHover = false;                       // marca do header sob o mouse
    juce::TextButton instrumentsButton { "Instrumentos" };   // volta da tela cheia do Church Live
    juce::ToggleButton inputToggle { "Entrada de audio (guitarra/mic)" };   // agora vive no dialogo Audio/MIDI
    DeviceInfoBar deviceBar;   // chips premium: controlador MIDI + interface de audio
    std::array<juce::Slider, 4> desktopMacroSliders;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, 4>
        desktopMacroAttachments;
    Slot slots[numSlots];
    // SafePointer so e invalidado no destrutor da classe-base Component. Alguns
    // plugins bombeiam mensagens durante a propria destruicao, portanto este
    // sentinela precisa ser desligado no INICIO de ~MainComponent.
    std::shared_ptr<std::atomic<bool>> pluginLoadLifetime {
        std::make_shared<std::atomic<bool>> (true) };

    juce::Label masterLabel;
    juce::Slider masterSlider;
    std::unique_ptr<LevelMeter> meter;

    // ---- painel de plugin embutido na janela ----
    juce::AudioProcessorGraph::NodeID dockedNodeID {};
    juce::AudioProcessorGraph::NodeID pendingDockNode {};
    bool dockedNative = false;   // Church Live embutido: sem barra, preenche tudo
    std::unique_ptr<juce::AudioProcessorEditor> dockedEditor;
    juce::Viewport dockViewport;
    juce::Component dockHolder;   // centraliza o editor dentro do viewport
    juce::Label dockTitle;
    juce::TextButton dockFloatButton { "Destacar" };
    juce::TextButton dockCloseButton { "Fechar" };

    // ---- mapeamento MIDI (knobs/faders do teclado -> parametros) ----
    // slot -1 = fader Master; 0..numSlots-1 = slot da cadeia.
    // insert >= 0 aponta pra um plugin hospedado DENTRO do Church Live
    // (MASTER FX) — sem isso o BigSky ficava fora do alcance do MIDI Learn.
    // paramName e a ancora de verdade: o indice sozinho aponta pra "o parametro
    // numero N do que estiver ali", e isso muda quando o plugin do slot e trocado.
    // Identificador de "slot" do Pad Continuo no mapeamento MIDI. Ele nao e um
    // slot de verdade (e um no proprio do grafo), entao ganha um numero fora
    // da faixa, no mesmo espirito do -1 usado pelo master.
    static constexpr int padSlotId = -3;
    // O sintetizador do PAD EDITOR tambem e um no proprio do grafo. Usa um
    // namespace separado do Pad Continuo para que os mapas sobrevivam a
    // reinicios sem depender da ordem dos nos internos.
    static constexpr int padSynthSlotId = -4;

    // MAPEAMENTO POR NOTA (pedido de cliente, 01/08/2026).
    //
    // Uma tecla do controlador vira comando em vez de nota: trocar o tom do
    // Pad Continuo com a mao esquerda, sem ir na tela no meio do culto.
    // VALORES EXPLICITOS: estes numeros vao para o disco.
    //
    // O saveNoteMap grava (int) kind cru. Sem numeros fixos, inserir uma acao
    // nova no meio do enum re-apontaria em silencio o mapa de TODOS os
    // clientes que ja gravaram — a tecla que trocava o tom do Pad passaria a
    // fazer outra coisa, sem nada quebrar e sem nada avisar. Acoes novas so
    // podem ser acrescentadas NO FIM, com numero proprio.
    enum class NoteActionKind
    {
        none = 0,
        padKey = 1,
        padOff = 2,
        cena = 3,
        panic = 4,
        nextScene = 5,
        previousScene = 6,
        padSynthProgram = 7,
        tapTempo = 8,
        sustainToggle = 9,
        padSynthEngineToggle = 10
    };

    struct NoteAction
    {
        NoteActionKind kind = NoteActionKind::none;
        int arg = -1;             // padKey: 0..11 | cena: indice 0..numeroDeCenas-1
                                  // (padOff e panic ignoram arg)
    };

    // Mapa proprio, com a mesma chave e o mesmo lock dos CCs. Alargar a chave
    // dos CCs reordenaria o mapa ja gravado dos clientes; um mapa paralelo
    // nao toca em nada do que existe.
    //
    // Tipo escrito por extenso porque o apelido MidiControlKey so aparece
    // mais abaixo neste mesmo cabecalho.
    std::map<ChurchDAWMidiMapping::ControllerKey, NoteAction> noteMappings;

    // 128 bits: "existe algum mapeamento para esta nota?". Lido na thread de
    // MIDI a cada tecla, por isso atomico e sem lock. Reconstruido na thread
    // de mensagens sempre que noteMappings muda.
    std::atomic<juce::uint32> mascaraDeNotasMapeadas[4] { {}, {}, {}, {} };

    // Note-ons suprimidos, incluindo a origem e profundidade. O ledger tem
    // capacidade fixa e nao aloca no callback MIDI; o midiCallbackBarrier
    // serializa as entradas que o atualizam. A profundidade impede o primeiro
    // OFF de liberar dois ONs repetidos da mesma tecla/origem.
    // A chave completa impede o note-off de outro teclado de fechar a nota
    // consumida do primeiro.
    ChurchDAWMidiMapping::SuppressedMidiNoteLedger<256, 255>
        notasSuprimidas;

    // Espelho atomico do conjunto acima, por NUMERO de nota. Existe para o
    // note-off poder desistir sem percorrer o ledger e — principalmente — sem
    // montar a chave, que custa uma alocacao de heap na thread de MIDI.
    std::atomic<juce::uint32> mascaraDeNotasSuprimidas[4] { {}, {}, {}, {} };

    // Alvo escolhido no painel do Pad enquanto o modo Learn espera a proxima
    // tecla fisica. Estes campos, assim como o resultado mais recente, sao
    // acessados pela thread MIDI sob mapLock.
    NoteAction pendingNoteAction;
    NoteAction lastMappedNoteAction;
    ChurchDAWMidiMapping::ControllerKey lastMappedNoteKey;
    std::atomic<int> lastMappedNote { -1 };
    std::atomic<int> lastNoteActionRejected { -1 };
    juce::String motivoDaRecusaDeNota; // protegido por mapLock

    // ---- LIVE MODE: 10 cenas ----
    ChurchDAWLive::Cena cenas[ChurchDAWLive::numeroDeCenas];
    ChurchDAWLive::Setlist setlist;
    int cenaAtiva = -1;
    // Slot vazio cujo motor ja foi aberto do zero e aguarda o segundo gesto
    // SALVAR. Nao e cenaAtiva porque ainda nao existe snapshot persistido.
    int cenaEmPreparacao = -1;

    struct SceneHistoryEntry
    {
        std::array<ChurchDAWLive::Cena, ChurchDAWLive::numeroDeCenas> cenas;
        ChurchDAWLive::Setlist setlist;
        int cenaAtiva = -1;
        int cenaEmPreparacao = -1;
        juce::String fingerprint;
    };
    std::vector<SceneHistoryEntry> sceneHistory;
    int sceneHistoryCursor = -1;
    bool applyingSceneHistory = false;
    std::unique_ptr<ChurchDAWRemoteServer> remoteServer;
    juce::String remoteToken;
    std::unique_ptr<juce::FileChooser> playbackFileChooser;

    // Cada pedido de cena feito pelo musico ganha uma geracao. As reaplicacoes
    // adiadas (timbre carregando) so valem se a geracao ainda for a delas —
    // assim a cena abandonada nunca vence a que ele escolheu depois.
    int geracaoDaCena = 0;
    ChurchDAWLive::ControleDeGravacaoDoSetupDuranteCena controleDeGravacaoDoSetup;

    struct EstadoContinuoAntesDaCena
    {
        bool valido = false;
        int geracao = -1;
        std::array<float, ChurchDAWLive::numeroDeCanais> volumes {};
        std::array<float, ChurchDAWLive::numeroDeCanais> reverbs {};
        float volumeGeral = 1.0f;
        float retornoReverb = 0.72f;
        float masterDb = 0.0f;
        float padVolume = 0.9f;
        float padCorte = 1.0f;
    } estadoAntesDaTransicao;

    int cenaPendenteNoStartup = -1;
    LiveScenePanel liveScenePanel;
    juce::TextButton liveButton { "Live" };

    ChurchDAWLive::Resultado capturarCena (int indice);

    // tentativa: so o reagendamento interno usa. Uma cena pedida enquanto um
    // timbre carrega se aplica sozinha quando a carga termina, em vez de
    // sumir sem explicacao.
    ChurchDAWLive::Resultado aplicarCena (int indice, int tentativa = 0,
                                          bool paginaDoMotorJaSolicitada = false);
    void limparMotorLiveParaPaginaVazia (TimbresProcessor& live);
    bool zerarTimbresDoProjeto();
    void pedirConfirmacaoParaZerarTimbres();
    void reconciliarCenaAtivaNoStartup (int tentativa = 0);
    void agendarFimDaSupressaoDeGravacaoDaCena (int geracao);
    void capturarInicioDaTransicao (TimbresProcessor& live, int geracao);
    bool iniciarTransicaoDaCena (const ChurchDAWLive::Cena& cena, int geracao);
    void aplicarFrameDaTransicao (const EstadoContinuoAntesDaCena& inicio,
                                  const ChurchDAWLive::Cena& destino,
                                  float progresso);
    struct TransicaoEmCurso
    {
        EstadoContinuoAntesDaCena inicio;
        ChurchDAWLive::Cena destino;
        int geracao = -1;
        double inicioMs = 0.0;
        double duracaoMs = 0.0;
    };
    void continuarTransicaoDaCena (
        const std::shared_ptr<TransicaoEmCurso>& transicao);

    void renomearCena (int indice, juce::String nome);
    void apagarCena (int indice);
    void atualizarMetadadosDaCena (int indice,
                                   const ChurchDAWLive::Cena& novosDados);
    void atualizarSetlist (const ChurchDAWLive::Setlist& novoSetlist);
    void captureSceneHistoryPoint();
    void undoSceneHistory();
    void redoSceneHistory();
    void applySceneHistoryPoint (const SceneHistoryEntry& point);
    juce::String sceneHistoryFingerprint() const;
    bool salvarCenas();   // true = arquivo realmente gravado
    void restaurarCenas();

    // Arquivo de cenas em disco e de uma versao mais nova que esta: nao le e
    // NAO GRAVA, pelo mesmo motivo do mapa de notas.
    bool esquemaDeCenasDesconhecido = false;
    // Nao e const: getUserSettings() do JUCE nao e const.
    juce::File arquivoDeCenas();
    juce::File pastaDePaginasDeCenas();
    juce::File arquivoDaPaginaDeCena (const juce::String& estadoId);
    bool gravarPaginaDeCena (const juce::MemoryBlock& estado,
                             juce::String& estadoId);
    bool carregarPaginaDeCena (const juce::String& estadoId,
                               juce::MemoryBlock& estado);
    bool assegurarPaginaDaCenaAtiva (TimbresProcessor& live);
    void removerPaginaDeCenaSeOrfa (const juce::String& estadoId);

    bool notaFoiConsumida (juce::MidiInput* source, const juce::MidiMessage& message);

    ChurchDAWMidiMapping::ControllerKey montarChaveDeNota (
        juce::MidiInput* source, const juce::MidiMessage& message, int nota) const;

    void executarAcaoDeNota (NoteAction acao);
    void performGlobalPanic (bool padSynthAlreadyRequested = false);
    void reconstruirMascaraDeNotas();
    void reconstruirMascaraDeNotasUnderMapLock();
    void atualizarEtiquetasDoPad();   // mostra no teclado do Pad qual tecla comanda cada tom
    void reconstruirMascaraDeSuprimidas();   // chamar com o mapLock tomado
    bool reservarNotaSuprimidaUnderMapLock (
        const ChurchDAWMidiMapping::ControllerKey& key);
    juce::File arquivoDoMapaDeCCs();
    void bloquearQuartetoMidiDesconhecido (const juce::String& contexto);
    bool exigirQuartetoMidiInspecionavel (const juce::String& contexto);
    void avisarSobreArquivosDeVersaoFutura();   // conta na TELA o que nao pode ser lido
    void guardarCopiaDeArquivoIlegivel (const juce::File& arquivo, const juce::String& rotulo);

    // Arquivo de CCs em disco e de uma versao mais nova: nao le e nao grava.
    bool esquemaDeCCsDesconhecido = false;
    bool bloquearSalvamentoCCVazio = false;

    bool saveNoteMap (bool autorizarVazioExplicito = false,
                      bool permitirConflitosDeReparo = false);
    bool saveMidiMapPair();
    void finishMidiMapDirtyEpochsAfterPairCommit (
        juce::uint64 ccEpoch, bool ccWasWritten,
        juce::uint64 noteEpoch, bool noteWasWritten);

    // Arquivo de notas em disco e de uma versao mais nova que esta. Enquanto
    // for true, o app nao le e NAO GRAVA — ler com as regras erradas ja seria
    // ruim, mas gravar por cima destruiria o que a versao nova guardou.
    bool esquemaDeNotasDesconhecido = false;
    bool bloquearSalvamentoNotasVazio = false;
    void restoreNoteMap();
    juce::File arquivoDoMapaDeNotas();

    // Nome do aparelho a partir de um sourceId, e resolucao do sourceId salvo
    // para o id ATUAL (o id do Android e efemero e muda no replug/reinicio).
    juce::String nomeDoAparelho (const juce::String& sourceId) const;
    juce::String resolverSourceId (const juce::String& sourceId,
                                    const juce::String& sourceName) const;
    void reindexMidiMappingsAfterRefresh (
        const juce::Array<juce::MidiDeviceInfo>& midiIns);

    using CCTarget = ChurchDAWMidiMapping::PersistedCCTarget;

    using MidiControlKey = ChurchDAWMidiMapping::ControllerKey;
    std::map<MidiControlKey, CCTarget> ccMappings;        // dispositivo + canal + CC -> alvo

    static constexpr size_t controllerRouteCapacity = 128;
    struct ControllerRouteDescriptor
    {
        MidiControlKey control;
        CCTarget target;
        juce::uint64 generation = 0;
        bool occupied = false;
    };
    ChurchDAWMidiMapping::FixedControllerRouteDispatcher<
        controllerRouteCapacity> controllerRoutes;
    std::array<ControllerRouteDescriptor, controllerRouteCapacity>
        controllerRouteDescriptors {};
    juce::uint64 publishedControllerRouteMappingGeneration = 0;
    bool controllerRouteOverflowReported = false;

    void bumpMidiMappingGeneration();
    void rebuildControllerRoutesIfNeeded (bool force = false);
    void consumeControllerRouteEvents();
    bool validateCurrentParameterCommand (
        const MidiMappingPanel::Command& command,
        const CCTarget& currentTarget,
        juce::String& failureReason) const;

    //==========================================================================
    // PAD CONTINUO ACIONADO POR CC — o "pad de borracha" (07/08/2026).
    //
    // TERCEIRO MAPA, e nao um campo novo nos dois que ja existem:
    //  - noteMappings tem a acao certa (padKey/arg) mas a chave dele e por NOTA:
    //    o arquivo grava o atributo "note" e monta a chave sempre com
    //    isNote = false. Nao ha como distinguir "nota 36" de "CC 36" la dentro.
    //  - ccMappings tem a chave certa, mas o VALOR dele so sabe descrever um
    //    parametro de AudioProcessor (slot/param/insert/nome/id). Alargar aquele
    //    struct mexeria no arquivo gravado de todos os clientes.
    //
    // O tom do Pad nao e um AudioProcessorParameter (e o atomico requestedKey),
    // entao ele nunca teria como caber no caminho de CC existente.
    //==========================================================================
    struct AcaoDeCC
    {
        NoteAction acao;   // padKey (arg 0..11) ou padOff

        ChurchDAWMidiMapping::ModoDeBotaoCC modo =
            ChurchDAWMidiMapping::ModoDeBotaoCC::alternar;

        // Estado do CONTROLE FISICO (nivel alto/baixo, alcance aprendido). NAO e
        // o estado do Pad — o estado do Pad e lido de padProcessor->getKey() na
        // hora de agir, justamente para que uma cena, a tela ou uma tecla MIDI
        // que troquem o tom por fora nunca dessincronizem este mapa.
        // Vive so em memoria: nunca vai para o disco.
        ChurchDAWMidiMapping::EstadoDeBotaoCC estado;

        // Criado NESTA sessao de Learn. Sem esta marca, a segunda mensagem do
        // mesmo gesto (o "soltei" de um pad momentaneo) caia no gesto de
        // DESMAPEAR e apagava o mapeamento recem-criado. Runtime; nunca gravado.
        bool aprendidoNesteLearn = false;
    };

    std::map<MidiControlKey, AcaoDeCC> ccActionMappings;

    // Snapshot apenas de ROTEAMENTO/RUNTIME enquanto uma operacao atomica
    // troca os mapas. Ele nao executa acoes, mas conserva quais controles eram
    // comandos antes da operacao e continua atualizando a borda fisica deles.
    // Assim um release recebido durante Importar/Limpar nao se perde num
    // rollback, e teclas usadas como parametros continuam mudas ate o commit.
    std::map<MidiControlKey, AcaoDeCC> ccActionsAtMapTransactionStart;
    std::set<MidiControlKey> notesSuppressedByMapTransaction;

    // FIFO fixa: preserva, sem coalescer, as primeiras 64 decisoes acionaveis
    // (diferentes de nada) na ordem exata em que chegaram. Se uma 65a decisao
    // aparecer, nenhum prefixo parcial e executado;
    // finishMidiMapRoutingTransaction aplica o PANIC fail-safe.
    ChurchDAWMidiMapping::DeferredMidiActionQueue<64>
        deferredCCDecisions;

    void beginMidiMapRoutingTransaction();
    void finishMidiMapRoutingTransaction();
    void resetMidiLearnTargetUnderCallbackBarrier (bool enabled);
    void consumeMidiLearnParameterMailbox();
    void consumeMidiLearnControlEvents();
    void clearMidiLearnParameterTargetUnderMapLock();

    class MidiLearnTouchTimer final : private juce::Timer
    {
    public:
        explicit MidiLearnTouchTimer (MainComponent& component)
            : owner (component) {}

        void setRunning (bool running)
        {
            if (running)
                startTimer (30);
            else
                stopTimer();
        }

    private:
        void timerCallback() override
        {
            owner.consumeMidiLearnParameterMailbox();
        }

        MainComponent& owner;
    };

    // 128 bits: "existe alguma ACAO ligada a este numero de CC?". Lido na thread
    // de MIDI a cada mensagem, por isso atomico e sem lock — um knob mandando
    // dezenas de CCs por segundo nao pode tocar no std::map.
    std::atomic<juce::uint32> mascaraDeCCsDeAcao[4] { {}, {}, {}, {} };

    // Roda na thread de MIDI. true = o CC virou comando e NAO segue para o motor.
    bool ccDeAcaoFoiConsumido (int cc, int value,
                               const juce::String& sourceId, int channel,
                               const juce::String& sourceName = {});
    void aplicarDecisaoDeBotaoNoTom (NoteAction acao,
                                     ChurchDAWMidiMapping::DecisaoDeBotaoCC decisao);
    void reconstruirMascaraDeCCsDeAcao();
    void reconstruirMascaraDeCCsDeAcaoUnderMapLock();
    bool limparBotoesDeCCVazios();        // ao sair do Learn; true = algo mudou
    ChurchDAWMidiMapping::ModoDeBotaoCC modoDoBotaoDoTom (int tom) const;
    void definirModoDoBotaoDoTom (int tom, ChurchDAWMidiMapping::ModoDeBotaoCC modo);
    bool tomTemBotaoDeCC (int tom) const;
    void atualizarBotoesDeCCDoPad();

    std::atomic<int> lastMappedCCAcao { -1 };       // CC recem aprendido como comando
    std::atomic<int> lastCCAcaoRecusada { -1 };     // CC recusado (parece knob / ja e parametro)
    std::atomic<int> lastCCAcaoDesmapeada { -1 };
    MidiControlKey lastMappedCCAcaoKey;             // protegido por mapLock
    NoteAction lastMappedCCAcaoAction;              // protegido por mapLock
    juce::String motivoDaRecusaDeCCAcao;            // protegido por mapLock

    mutable juce::CriticalSection mapLock;
    std::atomic<bool> midiLearnActive { false };
    std::atomic<juce::uint64> parameterLearnGeneration { 1 };
    std::atomic<juce::uint64> parameterLearnSelectionGeneration { 1 };
    // Changes only when a processor/insert route changes. Insert identity
    // caches are tagged with this generation to reject pointer ABA reuse.
    std::atomic<juce::uint64> parameterTargetGeneration { 1 };
    ChurchDAWMidiMapping::ParameterTouchMailbox<32>
        parameterTouchMailbox;
    ChurchDAWMidiMapping::FixedMidiLearnControlQueue<16>
        midiLearnControlQueue;
    bool midiLearnQueueOverflowReported = false;
    juce::uint64 parameterTouchDropsReported = 0; // message thread only
    MidiLearnTouchTimer midiLearnTouchTimer;
    std::atomic<int> lastTouchedSlot { -2 };
    std::atomic<int> lastTouchedParam { -1 };
    std::atomic<int> lastTouchedInsert { -1 };   // insert do Church Live tocado
    std::atomic<juce::uint64> lastTouchedTopologyGeneration { 0 };
    std::atomic<bool> lastTouchedParameterIdentityUnique { false };
    std::atomic<int> lastMappedCC { -1 };
    std::atomic<int> lastParameterMappingRejectedCC { -1 };
    juce::String motivoDaRecusaDeParametro; // protegido por mapLock
    MidiControlKey lastLearnInputKey;
    int lastLearnInputValue = -1;
    MidiControlKey lastMappedKey;                         // protegido por mapLock
    ChurchDAWMidiMapping::SourceAwareSustainState sourceAwareSustain; // protegido por mapLock
    // Estado bruto persistente dos pedais/switches 64..69. Uma transacao pode
    // comecar depois do ON; copiar este ledger garante que uma nova acao nao
    // engula apenas o OFF e deixe o instrumento preso.
    ChurchDAWMidiMapping::RawCCTransactionGestureLatch rawCCPhysicalGestures;
    // Controles que atravessaram crus enquanto uma transacao trocava os mapas.
    // O restante do gesto tambem deve atravessar cru, mesmo se a importacao
    // instalar uma acao para o mesmo CC antes do release.
    ChurchDAWMidiMapping::RawCCTransactionGestureLatch
        rawCCGesturesDuringMapTransaction; // protegido por mapLock
    std::atomic<bool> rawCCTransactionGesturePending { false };
    std::atomic<juce::uint32> lastMidiActivityMs { 0 };
    std::atomic<juce::uint32> lastLearnedAtMs { 0 };
    std::atomic<juce::uint32> suppressTouchUntilMs { 0 }; // ignora "ecos" de parametro apos mapear/aplicar
    // era um bool cru: escrito na thread de mensagens e lido na thread de audio
    std::atomic<bool> applyingMappedCC { false };
    juce::String lastTouchedParamName;                    // protegido por mapLock
    juce::String lastTouchedParamId;                      // protegido por mapLock
    juce::String lastTouchedPluginId;                     // protegido por mapLock
    struct InsertMappingIdentitySnapshot
    {
        juce::AudioProcessor* processor = nullptr;
        juce::String identity;
        juce::uint64 topologyGeneration = 0;
    };
    static constexpr int numHostedInsertMappingCaches = 2;
    InsertMappingIdentitySnapshot
        insertMappingIdentityCache[numSlots][numHostedInsertMappingCaches];
    ChurchDAWMidiMapping::ParameterCallbackRoute
        insertParameterCallbackRoutes[numSlots]
                                     [numHostedInsertMappingCaches];

    // Launchkey em modo DAW: faders/knobs mapeados sozinhos + nome no visor
    std::unique_ptr<LaunchkeyControl> launchkey;
    void setLaunchkeyMappingEnabled (bool enabled);
    void setupLaunchkey();
    void applyAutomaticControllerProfile (const juce::MidiDeviceInfo& device);
    void refreshLaunchkeyNames();
    TimbresProcessor* getChurchLive() const;

    // pads de tom do Pad Continuo: 7 naturais + um modificador "#"
    bool padSharp = false;

    static int padNatural (int i)     { static const int n[7] = { 0, 2, 4, 5, 7, 9, 11 }; return n[i]; }
    static bool padHasSharp (int i)   { return i != 2 && i != 6; }   // E e B nao tem sustenido

    juce::TextButton mapButton { "Mapear MIDI" };
    juce::TextButton clearMapButton { "Limpar map." };
    MidiLearnPanel midiLearnPanel;
    MidiMappingPanel midiMappingPanel;
    bool liveModeBeforeMidiLearn = false;
    MidiLearnConsoleOverlay midiLearnConsoleOverlay;
    std::unique_ptr<juce::FileChooser> midiMapFileChooser;
    std::unique_ptr<juce::FileChooser> sessionBundleFileChooser;
    // Pagina completa continua disponivel para editar/importar mapas. Neste
    // modo rapido ela sai da frente, mas o Learn permanece armado enquanto o
    // musico navega pelas telas e toca os controles reais.
    bool quickMidiMappingMode = false;
    bool clearMapConfirmationArmed = false;
    int lastMidiDiagnosticCC = -1;
    int lastMidiDiagnosticChannel = -1;
    juce::String lastMidiDiagnosticSource;
    int lastMidiDiagnosticState = -1;
    juce::uint32 lastMidiDiagnosticMs = 0;
    juce::uint32 lastLaunchkeyRetryMs = 0;

    void refreshMidiMappingPanel();
    void armMidiMappingCommand (const MidiMappingPanel::Command& command);
    void saveMidiMappingCommand (const MidiMappingPanel::Command& command);
    void deleteMidiMappingCommand (const MidiMappingPanel::Command& command);
    void setMidiLearningFromPage (bool enabled);
    void beginQuickMidiMapping();
    bool armMidiLearnParameter (juce::AudioProcessor* processor,
                                int parameterIndex,
                                juce::Component* visualTarget = nullptr,
                                const juce::String& customName = {});
    bool armMidiLearnAction (int actionKind,
                             int arg = -1,
                             const juce::String& displayName = {},
                             juce::Component* visualTarget = nullptr,
                             juce::Rectangle<int> visualBounds = {});
    void importMidiMappings();
    void exportMidiMappings();
    void exportCultSession();
    void importCultSession();
    void activateEmergencyPiano();
    void runStageDiagnostic();
    void showAutoPatchBuilder();
    juce::String handleRemoteRequest (const juce::String& path);

    // Sombra do visor, desenhada uma vez.
    //
    // MEDIDO no TANK 3 (30/07/2026): MainComponent::paint custava 40,8 ms, e
    // TimbresEditor::paint mais 24,1 -- os ~65 ms por quadro que davam os
    // 15,3 fps medidos durante rolagem. DropShadow::drawForPath faz desfoque
    // gaussiano NA CPU a cada pintura, e este e de raio 16.
    //
    // A forma e um retangulo arredondado que so muda quando o layout muda.
    // Entao o desfoque passa a ser feito uma vez e o resultado e apenas
    // copiado. Invalidacao: o proprio retangulo de origem.
    juce::Image oledShadowCache;
    juce::Rectangle<int> oledShadowSource;
    static constexpr int oledShadowPad = 24;   // raio 16 + deslocamento 3 + folga

    // Mesma historia para a sombra da chapa do icone: raio menor, area menor,
    // mas ainda um desfoque de CPU por quadro. Chave inclui o raio porque ele
    // muda entre header compacto e normal.
    juce::Image iconShadowCache;
    juce::Rectangle<int> iconShadowSource;
    int iconShadowRadius = 0;
    static constexpr int iconShadowPad = 16;

    // Lista de entradas MIDI em cache.
    //
    // MEDIDO no TANK 3 (30/07/2026): timerCallback custava 158-189 ms POR
    // SEGUNDO, e o timer e de 1 Hz -- ou seja, a interface congelava quase um
    // quinto de cada segundo. A causa era chamar getAvailableDevices() DUAS
    // vezes por tique: no Android isso atravessa JNI e faz o servico de MIDI do
    // sistema enumerar o USB inteiro.
    //
    // Hot-plug continua funcionando: 4 s e mais rapido do que qualquer musico
    // consegue plugar e tocar, e o laco de reabertura do Launchkey ja opera em
    // 5 s. Quem some/aparece e detectado no proximo refresh.
    juce::Array<juce::MidiDeviceInfo> cachedMidiInputs;
    juce::uint32 cachedMidiInputsAtMs = 0;
    static constexpr juce::uint32 midiInputCacheMs = 4000;

    const juce::Array<juce::MidiDeviceInfo>& midiInputsCached (juce::uint32 nowMs)
    {
        if (cachedMidiInputsAtMs == 0
             || nowMs - cachedMidiInputsAtMs >= midiInputCacheMs)
        {
            cachedMidiInputs = juce::MidiInput::getAvailableDevices();
            cachedMidiInputsAtMs = nowMs;
        }

        return cachedMidiInputs;
    }

   #if JUCE_ANDROID || JUCE_IOS
    // O mobile parte de ~12 ms e so aumenta se o contador real de xruns
    // confirmar falta de prazo. Mantido na message thread.
    int androidLastXRunCount = 0;
    juce::uint32 androidLastBufferTuneMs = 0;
    int androidPreferredBufferSize = 0;
    int androidXRunPressure = 0;
    int androidStableBufferWindows = 0;
    // A abertura/reabertura do AAudio/CoreAudio pode registrar 1-2 underruns
    // enquanto a HAL ainda enche o primeiro buffer. A primeira janela do tuner
    // serve so para estabelecer a linha de base; depois disso todo xrun conta.
    std::atomic<bool> androidBufferTunerWarmupPending { true };
    // No backend Oboe do JUCE, getAvailableBufferSizes() abre um stream
    // temporario para descobrir FramesPerBurst. Fazer isso no tuner periodico
    // disputa a rota ativa e pode causar underrun. A lista so muda quando a
    // topologia de audio muda, portanto e capturada uma vez por rota.
    juce::Array<int> androidAvailableBufferSizes;
   #endif

   #if JUCE_ANDROID
    // AudioDeviceCallback Java so incrementa a geracao. Toda enumeracao,
    // fechamento e reabertura acontece aqui, no timer da message thread.
    juce::int64 androidAudioTopologyGeneration = 0;
    juce::int64 androidPendingAudioTopologyGeneration = 0;
    bool androidAudioReconcileScheduled = false;
    bool androidForceAudioReconcile = false;
    juce::String androidLastUsbAudioTopology;
    bool androidUsbAudioTopologyInitialized = false;

    // Se a interface USB some, a rota padrao assume sem apagar a preferencia.
    // Quando o mesmo nome volta com outro AudioDeviceInfo.id, o scan novo o
    // resolve e esta configuracao pode ser reaplicada automaticamente.
    juce::AudioDeviceManager::AudioDeviceSetup androidPreferredUsbSetup;
    bool androidHasPreferredUsbSetup = false;
   #endif

    juce::Image brandIcon;
    juce::Image desktopBrandMark;
    juce::Label statusInfo;   // taxa / buffer / CPU / MIDI (agora dentro do visor)
    juce::Rectangle<int> engineStatusBounds;   // pill completo: titulo + valor tecnico
    OledDisplay oled;             // visor de programa, como o OLED do Stage 4
    juce::TooltipWindow tooltipWindow { this, 650 };   // uma unica fonte para toda a janela

    // geometria calculada no resized() e reaproveitada no paint()
    int layoutHeaderHeight = headerHeight;
    int layoutProgramBandHeight = programBandHeight;
    int layoutBrandWidth = 228;
    int layoutSafeLeft = 0;
    int layoutSafeRight = 0;
    int layoutSafeTop = 0;
    int layoutSafeBottom = 0;
    bool mobilePortraitLayout = false;
    bool mobileShortLandscapeLayout = false;
   #if CHURCHDAW_MOBILE_SHELL
    // Shell Church Live em paisagem: trilho de navegacao a esquerda + barra
    // superior unica (ChurchLiveSkin.h). So camada visual/geometria.
    bool shellSkin = false;
    bool shellSkinWasOn = false;
    juce::Rectangle<int> railBounds, railLogoBounds;
    ChurchLiveSkin::Wordmark shellWordmark;
   #endif
    bool mobileTwoRowHeader = false;
    bool compactHeaderLayout = false;
    bool narrowHeaderLayout = false;
    bool desktopHeaderTwoRows = false;
    bool compactProgramBand = false;
    bool desktopReferenceLayout = false;
    // Trilho de navegacao a esquerda no desktop (DesktopHeaderLayout::railWidth).
    bool desktopRail = false;
    bool desktopRailWasOn = false;
    juce::Rectangle<int> desktopRailBounds;
    void layoutDesktopRail (bool learning);
    void clearDesktopRail();
    enum class DesktopWorkspace { mixer, performance, session };
    DesktopWorkspace desktopWorkspace = DesktopWorkspace::mixer;
    juce::TextButton undoMixButton, redoMixButton;
    int desktopSidebarWidth = 0;
    int desktopBottomBarHeight = 0;
    juce::Rectangle<int> desktopSidebarBounds;
    juce::Rectangle<int> desktopBottomBarBounds;
    juce::Rectangle<int> desktopHeaderExtrasBounds;
    juce::Rectangle<int> desktopBpmBounds;
    juce::Rectangle<int> desktopTapBounds;
    juce::Rectangle<int> desktopSettingsBounds;
    juce::Rectangle<int> desktopLiveStatusBounds;
    juce::Rectangle<int> desktopMixerTabBounds;
    juce::Rectangle<int> desktopPerformanceTabBounds;
    juce::Rectangle<int> desktopSessionTabBounds;
    juce::Rectangle<int> desktopSceneBounds[ChurchDAWLive::numeroDeCenas];
    int desktopSceneIndexAtRow[ChurchDAWLive::numeroDeCenas] {};
    juce::Rectangle<int> desktopSceneAddBounds;
    juce::Rectangle<int> desktopFavouriteBounds[5];
    juce::Rectangle<int> desktopSaveBounds;
    juce::Rectangle<int> desktopResetBounds;
    juce::Rectangle<int> desktopAudioBounds;
    int qaOledChannel = 0;
    bool qaOledSelectionApplied = false;
    juce::Rectangle<int> layoutContentBounds;
    juce::Rectangle<int> layoutDockHeaderBounds;
    juce::Rectangle<int> slotCardBounds[numSlots];
    juce::Rectangle<int> masterCardBounds;
    int slotsRowActual = slotsRowHeight;   // a fileira cresce quando sobra um card so

    std::map<juce::uint32, std::unique_ptr<PluginWindow>> pluginWindows;

    double getCurrentSampleRate() const;
    int getCurrentBlockSize() const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshSlotChoices();
    void slotSelectionChanged (int slotIndex);
    void loadPlugin (int slotIndex, const juce::PluginDescription& desc,
                     const juce::MemoryBlock* state,
                     bool openEditorAfterLoad = false,
                     bool saveAfterLoad = false);
    void loadInternalTimbres (int slotIndex, const juce::MemoryBlock* state, int libraryIndex);
    void unloadSlot (int slotIndex);
    void removeNodeForSlot (int slotIndex, juce::AudioProcessorGraph::NodeID nodeID);
    void openEditorForSlot (int slotIndex);
    void dockNode (juce::AudioProcessorGraph::NodeID nodeID);
    void dockNodeNow (juce::AudioProcessorGraph::NodeID nodeID);
    void undockEditor();
    void updateDockLayout();
    void layoutDockHolder();
    void openFloatingWindowForNode (juce::AudioProcessorGraph::NodeID nodeID);
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    bool processControllerCC (int cc, int value,
                              const juce::String& sourceId, int channel,
                              bool isNote = false,
                              const juce::String& sourceName = {});
    void audioProcessorParameterChanged (juce::AudioProcessor*, int parameterIndex, float newValue) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void applyCCTarget (CCTarget target, float value01,
                        MidiControlKey control = {});
    juce::String describeCCTarget (const CCTarget&) const;
    juce::String cachedProcessorMappingIdentity (int slot,
                                                 int insert = -1,
                                                 const juce::AudioProcessor* expected = nullptr) const;
    juce::AudioProcessor* resolveCurrentMidiMappingProcessor (
        int slot, int insert, juce::String& identity) const;
    void refreshInsertMappingIdentityCacheForSlot (int slot);
    void retireInsertParameterListener (TimbresProcessor* owner,
                                        int insert,
                                        juce::AudioPluginInstance* processor);
    bool validatedCallbackTopologyGeneration (
        const juce::AudioProcessor* processor,
        juce::uint64& topologyGeneration) const noexcept;
    int getMidiMappingCount() const;

    // estado de pickup por CC: enquanto o fader fisico nao alcanca o valor atual
    // do parametro, o movimento e ignorado (evita salto no meio do louvor)
    struct CCPickup { float last = 0.0f; bool hasLast = false; bool engaged = false; };
    std::map<MidiControlKey, CCPickup> ccPickup;  // exclusivo da message thread
    // a thread de MIDI nao mexe no map: pede o reset por aqui e o clear
    // (mapa inteiro — o slot unico coalesce pedidos) e no handleAsyncUpdate
    std::atomic<int> ccPickupToClear { -1 };
    bool ccHasPickedUp (const MidiControlKey&, float incoming, float current);
    bool saveMidiMap (bool autorizarVazioExplicito = false,
                      bool permitirConflitosDeReparo = false);
    void restoreMidiMap();

    bool rebuildGraphConnections (
        int replacementSlot = -1,
        juce::AudioProcessorGraph::NodeID replacementNode = {});
    void showAudioSettings();
    void openSidebarTimbreCategory (int categoryIndex);
    void showAbout();
    void showLibraries();
    void rescanInstalledLibraries();
    juce::Rectangle<int> brandArea() const;   // area clicavel da marca no header
   #if CHURCHDAW_MOBILE_SHELL
    void paintShellSkin (juce::Graphics& g);
    void layoutShellSkin (juce::Rectangle<int> headerRow, const ChurchLiveSkin::Metrics& m, bool learning, bool churchFull);
    void clearShellSkin();
    void placeZoomedPanel (juce::Component& panel);
   #endif
    // Marca no trilho a tela aberta (Audio/Bibliotecas sao paineis sobrepostos).
    void refreshRailActive();
    void showPluginScanner();
    void showPadEditor (int sourceChannel = -1,
                        const juce::String& sourceName = {});
    void closePadEditor();
    void showScenesPage();
    void closeScenesPage();
    void showDesktopWorkspace (DesktopWorkspace workspace);
    void refreshPerformanceSessionPanel();
    // true = o ARQUIVO foi gravado. Plugin que se recusou a serializar nao
    // derruba o retorno (seria falha permanente por causa de um VST que o
    // musico nem usa) — o nome dele sai em pluginsQueNaoSalvaram.
    bool saveChainState();
    juce::StringArray pluginsQueNaoSalvaram;
    bool jaAvisouSobrePluginSemEstado = false;   // o modal e uma vez por sessao, nao por clique
    void restoreChainState();

    // Grava o setup pouco depois da ultima mudanca, para um fechamento
    // anormal nao levar embora o que o musico montou.
    void agendarGravacaoDoSetup();

   #if JUCE_ANDROID
    juce::OpenGLContext glContext;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
