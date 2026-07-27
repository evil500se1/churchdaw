#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "MasterProcessor.h"
#include "LevelMeter.h"
#include "DeviceInfoBar.h"
#include "PadProcessor.h"
#include "PadPanel.h"
#include "LaunchkeyControl.h"
#include "OledDisplay.h"

#include <atomic>
#include <map>
#include <memory>

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

private:
    static constexpr int numFxSlots = 3;
    static constexpr int numSlots   = numFxSlots + 1;   // slot 0 = instrumento

    static constexpr int headerHeight    = 52;
    // faixa propria do painel de programa, abaixo do cabecalho. Ele NAO e peca de
    // cabecalho: e um bloco centralizado, como o display no painel do Electro 7.
    static constexpr int programBandHeight = 152;   // +16 para o respiro interno do visor
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
        juce::Array<juce::PluginDescription> choices;
        std::atomic<juce::AudioProcessor*> proc { nullptr };   // para identificar o plugin tocado no MIDI Learn
    };

    // ---- motor de audio ----
    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::NodeID audioInNode, audioOutNode, midiInNode, masterNode, padNode;
    MasterProcessor* masterProcessor = nullptr;   // pertence ao grafo
    PadProcessor* padProcessor = nullptr;         // pad continuo (pertence ao grafo)

    juce::ApplicationProperties appProperties;
    bool suppressAudioStateSave = true;   // nao salvar estados transitorios durante a inicializacao
    bool audioDialogOpen = false;         // config de audio so e salva com o dialogo aberto (escolha do usuario)

    friend struct AudioSettingsHolder;

    void saveAudioStateNow();
    void restartAudioDevice();   // recupera o driver ASIO sem desplugar (anti-pipoco)

    // ---- interface ----
    juce::TextButton audioSettingsButton { "Audio / MIDI" };
    juce::TextButton scanButton { "Plugins" };
    juce::TextButton saveButton { "Salvar" };
    juce::TextButton padButton { "Pad" };   // mostra/oculta o card do Pad Continuo
    std::unique_ptr<PadPanel> padPanel;     // card embutido (nao e janela separada)
    // ultima posicao do card do Pad setada por codigo: se a atual difere, o
    // usuario arrastou e o resized() nao pode re-centralizar
    juce::Rectangle<int> padPanelHome;
    std::unique_ptr<juce::Component> audioPanel;   // card Audio/MIDI embutido
    std::unique_ptr<juce::Component> aboutPanel;   // card "Sobre" (clique na marca)
    bool brandHover = false;                       // marca do header sob o mouse
    juce::TextButton instrumentsButton { "Instrumentos" };   // volta da tela cheia do Church Live
    juce::ToggleButton inputToggle { "Entrada de audio (guitarra/mic)" };   // agora vive no dialogo Audio/MIDI
    DeviceInfoBar deviceBar;   // chips premium: controlador MIDI + interface de audio
    Slot slots[numSlots];

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
    struct CCTarget { int slot = -2; int param = -1; int insert = -1; juce::String paramName; };

    std::map<int, CCTarget> ccMappings;                   // numero do CC -> alvo
    juce::CriticalSection mapLock;
    std::atomic<bool> midiLearnActive { false };
    std::atomic<int> lastTouchedSlot { -2 };
    std::atomic<int> lastTouchedParam { -1 };
    std::atomic<int> lastTouchedInsert { -1 };   // insert do Church Live tocado
    std::atomic<int> lastMappedCC { -1 };
    std::atomic<juce::uint32> suppressTouchUntilMs { 0 }; // ignora "ecos" de parametro apos mapear/aplicar
    // era um bool cru: escrito na thread de mensagens e lido na thread de audio
    std::atomic<bool> applyingMappedCC { false };
    juce::String lastTouchedParamName;                    // protegido por mapLock

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
    juce::Label learnStatus;

    juce::Image brandIcon;
    juce::Label statusInfo;   // taxa / buffer / CPU / MIDI (agora dentro do visor)
    OledDisplay oled;             // visor de programa, como o OLED do Stage 4

    // geometria calculada no resized() e reaproveitada no paint()
    juce::Rectangle<int> slotCardBounds[numSlots];
    juce::Rectangle<int> masterCardBounds;
    int slotsRowActual = slotsRowHeight;   // a fileira cresce quando sobra um card so

    std::map<juce::uint32, std::unique_ptr<PluginWindow>> pluginWindows;

    double getCurrentSampleRate() const;
    int getCurrentBlockSize() const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshSlotChoices();
    void slotSelectionChanged (int slotIndex);
    void loadPlugin (int slotIndex, const juce::PluginDescription& desc, const juce::MemoryBlock* state);
    void loadInternalTimbres (int slotIndex, const juce::MemoryBlock* state, int libraryIndex);
    void unloadSlot (int slotIndex);
    void openEditorForSlot (int slotIndex);
    void dockNode (juce::AudioProcessorGraph::NodeID nodeID);
    void dockNodeNow (juce::AudioProcessorGraph::NodeID nodeID);
    void undockEditor();
    void updateDockLayout();
    void layoutDockHolder();
    void openFloatingWindowForNode (juce::AudioProcessorGraph::NodeID nodeID);
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void audioProcessorParameterChanged (juce::AudioProcessor*, int parameterIndex, float newValue) override;
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void applyCCTarget (CCTarget target, float value01, int ccNumber = -1);

    // estado de pickup por CC: enquanto o fader fisico nao alcanca o valor atual
    // do parametro, o movimento e ignorado (evita salto no meio do louvor)
    struct CCPickup { float last = 0.0f; bool hasLast = false; bool engaged = false; };
    std::map<int, CCPickup> ccPickup;             // exclusivo da message thread
    // a thread de MIDI nao mexe no map: pede o reset por aqui e o clear
    // (mapa inteiro — o slot unico coalesce pedidos) e no handleAsyncUpdate
    std::atomic<int> ccPickupToClear { -1 };
    bool ccHasPickedUp (int cc, float incoming, float current);
    void saveMidiMap();
    void restoreMidiMap();

    void rebuildGraphConnections();
    void showAudioSettings();
    void showAbout();
    juce::Rectangle<int> brandArea() const;   // area clicavel da marca no header
    void showPluginScanner();
    void showPadWindow();
    void saveChainState();
    void restoreChainState();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
