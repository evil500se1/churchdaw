#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "AppPaths.h"   // define CHURCHDAW_HOSTS_PLUGINS

#if JUCE_ANDROID || JUCE_IOS
 #include <juce_opengl/juce_opengl.h>
#endif

#include "MasterProcessor.h"
#include "LevelMeter.h"
#include "DeviceInfoBar.h"
#include "AudioMidiButton.h"
#include "PadProcessor.h"
#include "PadPanel.h"
#include "LaunchkeyControl.h"
#include "MidiLearnPanel.h"
#include "MidiMappingSafety.h"
#include "OledDisplay.h"
#include "LiveScenes.h"
#include "LiveScenePanel.h"

#if CHURCHDAW_HOSTS_PLUGINS
 #include "PluginScanSubprocessV2.h"
#endif

#include <atomic>
#include <map>
#include <memory>
#include <set>

class PluginWindow;
class TimbresProcessor;   // definido em TimbresProcessor.h (incluido so no .cpp)

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
                      private juce::AudioProcessorListener,
                      private juce::AsyncUpdater,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    void closeWindowForNode (juce::AudioProcessorGraph::NodeID nodeID);

    // ---- estado de carregamento (a splash so sai quando isto fica pronto) ----
    bool isStillLoading() const;
    juce::String getLoadingDetail() const;
    float getLoadingProgress() const;

    // Botao BACK fisico do Android. Fecha a camada de cima (organ ->
    // mixer; Audio/MIDI -> mixer; Bibliotecas -> mixer) e devolve true.
    // False = nada aberto, o chamador decide se o app sai.
    bool handleBackButton();

    // O demo encerra com um fade do master antes de destruir o grafo. Nunca
    // corte uma cauda de reverb ou nota sustentada na troca para o gate.
    void beginLicenseFadeOut();

    // Hot-plug, sleep/resume e troca de rota podem perder o pitch-wheel center
    // ou um note-off. Recentraliza todos os 16 canais no proximo bloco.
    void resetMidiTransportState();

private:
    static constexpr int numFxSlots = 3;
    static constexpr int numSlots   = numFxSlots + 1;   // slot 0 = instrumento

    static constexpr int headerHeight    = 64;
    static constexpr int mobileHeaderHeight = 38;
    // faixa propria do painel de programa, abaixo do cabecalho. Ele NAO e peca de
    // cabecalho: e um bloco centralizado, como o display no painel do Electro 7.
    static constexpr int programBandHeight = 112;   // visor 29% mais baixo, sem perder as 5 linhas
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
        // plugin salvo mas ausente/falho: re-emitido no saveChainState pra nao
        // apagar o setup; limpo quando qualquer coisa carrega no slot
        juce::String pendingIdentifier;
        juce::String pendingState;   // base64
        juce::String loadingIdentifier;   // pedido async ainda nao confirmado
        juce::Array<juce::PluginDescription> choices;
        std::atomic<juce::AudioProcessor*> proc { nullptr };   // para identificar o plugin tocado no MIDI Learn
        juce::uint64 loadGeneration = 0;   // invalida callbacks async antigos (AUv3 inclusive)
    };

    // ---- motor de audio ----
   #if JUCE_ANDROID || JUCE_IOS
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
    juce::OpenGLContext glContext;
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
    juce::AudioProcessorGraph::NodeID audioInNode, audioOutNode, midiInNode, masterNode, padNode;
    MasterProcessor* masterProcessor = nullptr;   // pertence ao grafo
    PadProcessor* padProcessor = nullptr;         // pad continuo (pertence ao grafo)

    juce::ApplicationProperties appProperties;
    bool suppressAudioStateSave = true;   // nao salvar estados transitorios durante a inicializacao
    bool audioDialogOpen = false;         // config de audio so e salva com o dialogo aberto (escolha do usuario)

    friend struct AudioSettingsHolder;
    friend class AndroidAudioMidiCenter;

    void saveAudioStateNow();
    void restartAudioDevice();   // recupera o driver ASIO sem desplugar (anti-pipoco)

   #if JUCE_ANDROID
    void pollAndroidAudioTopology();
    void reconcileAndroidAudioTopology();
    void updateAndroidPreferredUsbSetupFromCurrent();
   #endif

    // ---- interface ----
    AudioMidiButton audioSettingsButton { "Audio / MIDI" };
    juce::TextButton scanButton { "Plugins" };
    juce::TextButton saveButton { "Salvar" };
    juce::Colour corDoSalvoNormal;   // verde do "Salvo"; guardado para voltar apos um "FALHOU"
    int geracaoDoAvisoDeSalvar = 0;  // so o clique mais recente manda no texto/cor do botao
    juce::TextButton padButton { "Pad" };   // mostra/oculta o card do Pad Continuo
    juce::TextButton librariesButton { "Bibliotecas" };
    std::unique_ptr<PadPanel> padPanel;     // card embutido (nao e janela separada)
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
    enum class NoteActionKind { none = 0, padKey = 1, padOff = 2 };

    struct NoteAction
    {
        NoteActionKind kind = NoteActionKind::none;
        int arg = -1;             // padKey: 0..11
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

    // Note-ons suprimidos, incluindo a origem. Mais de uma entrada MIDI pode
    // chamar o callback ao mesmo tempo; por isso o conjunto usa o mapLock.
    // A chave completa impede o note-off de outro teclado de fechar a nota
    // consumida do primeiro.
    std::set<ChurchDAWMidiMapping::ControllerKey> notasSuprimidas;

    // Espelho atomico do conjunto acima, por NUMERO de nota. Existe para o
    // note-off poder desistir sem pegar lock e — principalmente — sem montar a
    // chave, que custa uma alocacao de heap na thread de MIDI.
    std::atomic<juce::uint32> mascaraDeNotasSuprimidas[4] { {}, {}, {}, {} };

    // Alvo escolhido no painel do Pad enquanto o modo Learn espera a proxima
    // tecla fisica. Estes campos, assim como o resultado mais recente, sao
    // acessados pela thread MIDI sob mapLock.
    NoteAction pendingNoteAction;
    NoteAction lastMappedNoteAction;
    ChurchDAWMidiMapping::ControllerKey lastMappedNoteKey;
    std::atomic<int> lastMappedNote { -1 };

    // ---- LIVE MODE: 10 cenas ----
    ChurchDAWLive::Cena cenas[ChurchDAWLive::numeroDeCenas];
    int cenaAtiva = -1;

    // Cada pedido de cena feito pelo musico ganha uma geracao. As reaplicacoes
    // adiadas (timbre carregando) so valem se a geracao ainda for a delas —
    // assim a cena abandonada nunca vence a que ele escolheu depois.
    int geracaoDaCena = 0;
    ChurchDAWLive::ControleDeGravacaoDoSetupDuranteCena controleDeGravacaoDoSetup;
    int cenaPendenteNoStartup = -1;
    LiveScenePanel liveScenePanel;
    juce::TextButton liveButton { "Live" };

    ChurchDAWLive::Resultado capturarCena (int indice);

    // tentativa: so o reagendamento interno usa. Uma cena pedida enquanto um
    // timbre carrega se aplica sozinha quando a carga termina, em vez de
    // sumir sem explicacao.
    ChurchDAWLive::Resultado aplicarCena (int indice, int tentativa = 0,
                                          bool paginaDoMotorJaSolicitada = false);
    void reconciliarCenaAtivaNoStartup (int tentativa = 0);
    void agendarFimDaSupressaoDeGravacaoDaCena (int geracao);

    void renomearCena (int indice, juce::String nome);
    void apagarCena (int indice);
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
    void reconstruirMascaraDeNotas();
    void atualizarEtiquetasDoPad();   // mostra no teclado do Pad qual tecla comanda cada tom
    void reconstruirMascaraDeSuprimidas();   // chamar com o mapLock tomado
    juce::File arquivoDoMapaDeCCs();
    void avisarSobreArquivosDeVersaoFutura();   // conta na TELA o que nao pode ser lido
    void guardarCopiaDeArquivoIlegivel (const juce::File& arquivo, const juce::String& rotulo);

    // Arquivo de CCs em disco e de uma versao mais nova: nao le e nao grava.
    bool esquemaDeCCsDesconhecido = false;

    bool saveNoteMap();   // true = arquivo novo realmente gravado

    // Arquivo de notas em disco e de uma versao mais nova que esta. Enquanto
    // for true, o app nao le e NAO GRAVA — ler com as regras erradas ja seria
    // ruim, mas gravar por cima destruiria o que a versao nova guardou.
    bool esquemaDeNotasDesconhecido = false;
    void restoreNoteMap();
    juce::File arquivoDoMapaDeNotas();

    struct CCTarget
    {
        int slot = -2;
        int param = -1;       // somente diagnostico/migracao; nunca resolve sozinho
        int insert = -1;
        juce::String paramName;
        juce::String paramId; // ancora estavel entre versoes/reordenacoes
    };

    using MidiControlKey = ChurchDAWMidiMapping::ControllerKey;
    std::map<MidiControlKey, CCTarget> ccMappings;        // dispositivo + canal + CC -> alvo
    mutable juce::CriticalSection mapLock;
    std::atomic<bool> midiLearnActive { false };
    std::atomic<int> lastTouchedSlot { -2 };
    std::atomic<int> lastTouchedParam { -1 };
    std::atomic<int> lastTouchedInsert { -1 };   // insert do Church Live tocado
    std::atomic<int> lastMappedCC { -1 };
    MidiControlKey lastMappedKey;                         // protegido por mapLock
    std::atomic<juce::uint32> lastMidiActivityMs { 0 };
    std::atomic<juce::uint32> lastLearnedAtMs { 0 };
    std::atomic<juce::uint32> suppressTouchUntilMs { 0 }; // ignora "ecos" de parametro apos mapear/aplicar
    // era um bool cru: escrito na thread de mensagens e lido na thread de audio
    std::atomic<bool> applyingMappedCC { false };
    juce::String lastTouchedParamName;                    // protegido por mapLock
    juce::String lastTouchedParamId;                      // protegido por mapLock

    // Launchkey em modo DAW: faders/knobs mapeados sozinhos + nome no visor
    std::unique_ptr<LaunchkeyControl> launchkey;
    void setupLaunchkey();
    void refreshLaunchkeyNames();
    TimbresProcessor* getChurchLive() const;

    // pads de tom do Pad Continuo: 7 naturais + um modificador "#"
    bool padSharp = false;

    static int padNatural (int i)     { static const int n[7] = { 0, 2, 4, 5, 7, 9, 11 }; return n[i]; }
    static bool padHasSharp (int i)   { return i != 2 && i != 6; }   // E e B nao tem sustenido

    juce::TextButton mapButton { "Mapear MIDI" };
    juce::TextButton clearMapButton { "Limpar map." };
    MidiLearnPanel midiLearnPanel;
    bool clearMapConfirmationArmed = false;
    int lastMidiDiagnosticCC = -1;
    int lastMidiDiagnosticChannel = -1;
    juce::String lastMidiDiagnosticSource;
    int lastMidiDiagnosticState = -1;
    juce::uint32 lastMidiDiagnosticMs = 0;
    juce::uint32 lastLaunchkeyRetryMs = 0;

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

   #if JUCE_ANDROID
    // O Android parte de 2 bursts (512 no TANK3) e so aumenta se o contador
    // real de xruns confirmar falta de prazo. Mantido na message thread.
    int androidLastXRunCount = 0;
    juce::uint32 androidLastBufferTuneMs = 0;

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
    bool compactHeaderLayout = false;
    bool narrowHeaderLayout = false;
    bool desktopHeaderTwoRows = false;
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
                              const juce::String& sourceId, int channel);
    void audioProcessorParameterChanged (juce::AudioProcessor*, int parameterIndex, float newValue) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void applyCCTarget (CCTarget target, float value01,
                        MidiControlKey control = {});
    juce::String describeCCTarget (const CCTarget&) const;
    int getMidiMappingCount() const;

    // estado de pickup por CC: enquanto o fader fisico nao alcanca o valor atual
    // do parametro, o movimento e ignorado (evita salto no meio do louvor)
    struct CCPickup { float last = 0.0f; bool hasLast = false; bool engaged = false; };
    std::map<MidiControlKey, CCPickup> ccPickup;  // exclusivo da message thread
    // a thread de MIDI nao mexe no map: pede o reset por aqui e o clear
    // (mapa inteiro — o slot unico coalesce pedidos) e no handleAsyncUpdate
    std::atomic<int> ccPickupToClear { -1 };
    bool ccHasPickedUp (const MidiControlKey&, float incoming, float current);
    bool saveMidiMap();   // true = arquivo realmente gravado
    void restoreMidiMap();

    bool rebuildGraphConnections (
        int replacementSlot = -1,
        juce::AudioProcessorGraph::NodeID replacementNode = {});
    void showAudioSettings();
    void showAbout();
    void showLibraries();
    void rescanInstalledLibraries();
    juce::Rectangle<int> brandArea() const;   // area clicavel da marca no header
    void showPluginScanner();
    void showPadWindow();
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
