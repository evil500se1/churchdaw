# ChurchDAW UI Component Hierarchy & Build System Analysis
**Agent**: `explorer_ui_build`  
**Date**: 2026-08-05  
**Target Repository**: `D:\ChurchDAW_Source`  
**Mission**: Map JUCE UI hierarchy (Desktop & Mobile), evaluate CMake/JUCE build configurations, and design exact UI integration and DSP bypass toggle mapping for the "Sabrina" native host plugin.

---

## 1. Executive Summary

ChurchDAW is a cross-platform Digital Audio Workstation built on C++17 and the JUCE framework. The system targets **Windows**, **macOS**, and **Android** (with iOS support planned/structured). 

- **UI Component Architecture**: Rooted in `MainComponent` (`Source/MainComponent.h`), which hosts an `AudioProcessorGraph` containing instrument, FX, pad, and master nodes, as well as embedded UI views (`TimbresEditor`, `OledDisplay`, `DeviceInfoBar`, `LiveScenePanel`, `PadPanel`, `MidiLearnPanel`, `LibrariesPanel`, `AboutPanel`).
- **Mobile vs. Desktop Adaptation**: On Mobile (Android/iOS), hardware GPU rendering via `juce::OpenGLContext` is attached to `MainComponent` to prevent heavy software-rasterized bitmap upload lag. Mobile layouts feature touch-friendly target sizes (min 48dp), two-row responsive headers, and custom stage views (`AndroidStageLayout`).
- **Plugin Hosting & Build System**: Windows and macOS use `CMakeLists.txt` with plugin hosting enabled (`CHURCHDAW_HOSTS_PLUGINS=1`, `JUCE_PLUGINHOST_VST3=1`, `JUCE_PLUGINHOST_AU=1`). Android builds use `ChurchDAW.jucer` (`ANDROIDSTUDIO` export format) where external binary VST3 plugin hosting is disabled (`CHURCHDAW_HOSTS_PLUGINS=0`) due to platform binary limits.
- **Sabrina Integration**: To support the "Sabrina" plugin across all platforms with a UI toggle button ("Sabrina Bypass" / "Sabrina On/Off"), a native C++ host wrapper and bypass state (`std::atomic<bool>`) must be bound to new UI toggle controls added to `MainComponent` header (for both Desktop and Mobile action rows) and `TimbresEditor` master controls.

---

## 2. ChurchDAW UI Component Hierarchy Analysis

### 2.1 Component Tree & File Map

```
MainComponent (juce::Component)
├── Header Bar (Grafite background, red accent)
│   ├── Brand Icon & Title ("ChurchDAW") -> opens AboutPanel
│   ├── DeviceInfoBar (MIDI controller + Audio Interface status chips)
│   ├── Status Label (statusInfo: Rate / Buffer / CPU / MIDI)
│   └── Header Action Buttons
│       ├── audioSettingsButton ("Áudio / MIDI") -> opens AudioPanel / AndroidAudioMidiCenter
│       ├── scanButton ("Plugins") [Desktop only: CHURCHDAW_HOSTS_PLUGINS=1]
│       ├── saveButton ("Salvar") -> triggers saveChainState()
│       ├── padButton ("Pad") -> toggles PadPanel card
│       ├── librariesButton ("Bibliotecas") -> opens LibrariesPanel
│       ├── instrumentsButton ("Instrumentos") -> switches full-screen Church Live view
│       ├── liveButton ("Live") -> toggles LiveScenePanel
│       └── mapButton / clearMapButton -> activates/clears MidiLearnPanel
├── Program Band / Visor Section (programBand)
│   ├── OledDisplay (Program name, Chord detection, 14-layer meters)
│   ├── MidiLearnPanel (Overlays program band when mapping MIDI)
│   └── LiveScenePanel (Overlays program band when Live mode is active)
├── Chain Slots Row (slotsRow)
│   ├── Slot 0: Instrument ("Church Live" / TimbresProcessor or hosted VSTi)
│   ├── Slot 1: EFEITO 1 (AudioProcessorGraph FX Node 1)
│   ├── Slot 2: EFEITO 2 (AudioProcessorGraph FX Node 2)
│   ├── Slot 3: EFEITO 3 (AudioProcessorGraph FX Node 3)
│   └── Master Card Strip: masterLabel, masterSlider, LevelMeter
├── Dock Viewport & Holder (dockViewport, dockHolder)
│   ├── dockedEditor: AudioProcessorEditor for selected plugin or TimbresEditor
│   └── dockTitle, dockFloatButton, dockCloseButton
└── Floating Overlay Panels (Child components / Dialogs)
    ├── PadPanel (PadProcessor controls card)
    ├── AboutPanel (Version & system info)
    ├── LibraryPanel (Sample pack downloader/catalog)
    └── AndroidAudioMidiCenter (Android full-screen Audio/MIDI manager)
```

### 2.2 Detailed Subsystem Breakdown

#### 1. `MainComponent` (`Source/MainComponent.h`, `Source/MainComponent.cpp`)
- **Role**: Root JUCE component and audio graph owner (`juce::AudioProcessorGraph graph`).
- **Graph Nodes**:
  - `audioInNode`: Audio input (guitar/mic).
  - `audioOutNode`: Master stereo audio output.
  - `midiInNode`: Combined MIDI input device callbacks.
  - `masterNode`: `MasterProcessor` (Master gain, Auto EQ, Punch/Worship saturation limiter).
  - `padNode`: `PadProcessor` (Continuous synth pad generator).
  - `slots[0..3]`: Slot 0 = Instrument (`TimbresProcessor` / VSTi); Slots 1..3 = FX slots.
- **State Management**: Persists graph state to XML via `saveChainState()` and `appProperties`.

#### 2. `TimbresProcessor` & `TimbresEditor` (`Source/TimbresProcessor.h`)
- **Role**: The primary native sampler/synth engine ("Church Live").
- **Structure**:
  - `numLayers = 14` timbre channels (Layer 0 = Piano, Layers 1..13 = Pads, Organs, Strings, etc.).
  - 4 Church Organ Families integrated (`ChurchOrganEditor.h`, `ChurchOrganLayerProcessor.h`).
  - Master FX Inserts (`numInserts = 3`), Worship Reverb (`WorshipReverb.h`), Channel Tone Shaper (`ChannelToneShaper.h`), Channel Rotary Speaker (`ChannelRotarySpeaker.h`).
- **UI Structure (`TimbresEditor`)**:
  - Horizontal scrollable channel strips (8 visible on screen at once via `canalAnteriorBtn`, `canalProximoBtn`).
  - Master controls panel: `masterDbSlider`, `gTranspSlider`, `reverbReturnSlider`, `panicButton`, `sustainToggle`, `invPedalToggle`, `masterAutoEqBtn`, `masterPunchBtn`, `reverbTypeCombo`, insert plugin slots.

#### 3. `OledDisplay` (`Source/OledDisplay.h`)
- **Role**: OLED visor displaying active preset name, detected musical chords (`ChordDetect::nameFor`), and 14-layer real-time volume levels.

#### 4. `DeviceInfoBar` (`Source/DeviceInfoBar.h`)
- **Role**: Display bar for connected MIDI devices (e.g. Launchkey) and audio interface connection status.

---

## 3. Desktop vs. Mobile Interface Layout Architecture

| Feature | Desktop (Windows / macOS) | Mobile (Android / iOS) |
| :--- | :--- | :--- |
| **GPU Rendering** | JUCE Software / Native OS rendering | `juce::OpenGLContext` attached to `MainComponent` (22% CPU vs 108% CPU in software) |
| **Header Layout** | Single/double row (height 46-96px), width-adaptive | Responsive 2-row layout in Portrait (`layoutHeaderHeight = 108px`), compact single row in short Landscape |
| **Touch Targets** | Standard mouse sizes (24-34px) | Touch-optimized (minimum 48dp target height for all buttons) |
| **Plugin Scanner** | `scanButton` visible (`CHURCHDAW_HOSTS_PLUGINS=1`) | `scanButton` hidden (`CHURCHDAW_HOSTS_PLUGINS=0`) |
| **Plugin Hosting** | Loads external VST3, AU, DLL binaries into graph | Internal native processing (`TimbresProcessor`, `ChurchOrgan`, native DSPs) |
| **Back Button** | N/A | Hardware Back Button handled in `MainComponent::handleBackButton()` |

---

## 4. Sabrina Plugin UI Mapping & DSP Bypass Design

### 4.1 Requirements Summary for Sabrina
1. **Plugin Binaries**: VST3 / AU / DLL loaded into the audio graph pipeline (or embedded native C++ processor fallback for Android ARM).
2. **DSP Bypass Toggle**: A toggle button ("Sabrina Bypass" / "Sabrina On/Off") that instantly bypasses Sabrina's DSP processing block, reducing CPU usage to 0% for Sabrina when disabled.
3. **Multi-Platform Access**: The button must be easily accessible in both Desktop and Mobile interfaces.

### 4.2 Exact JUCE UI Component Placement for Sabrina Toggle

To ensure seamless operation on both Desktop and Mobile devices, the toggle button should be placed in **two complementary UI locations**:

#### Placement A: `MainComponent` Header Bar (Global Access)
- **Component**: `juce::ToggleButton sabrinaToggle { "Sabrina" };` or `juce::TextButton sabrinaToggle { "Sabrina: ON" };`
- **Desktop Positioning**: In `MainComponent::resized()`, place `sabrinaToggle` in the top header action bar alongside `padButton`, `saveButton`, and `liveButton`.
- **Mobile Positioning (Portrait & Landscape)**:
  - In `MainComponent::resized()` under `#if JUCE_ANDROID || JUCE_IOS`, append `sabrinaToggle` to the `actions` array in `actionRow`.
  - This automatically places the Sabrina toggle in the 48dp touch action bar visible at the top of the mobile screen.

```cpp
// Header action button addition in MainComponent.h:
juce::ToggleButton sabrinaToggle { "Sabrina" };

// Data Binding in MainComponent.cpp:
sabrinaToggle.setToggleState(true, juce::dontSendNotification); // Default ON
sabrinaToggle.onClick = [this]
{
    const bool active = sabrinaToggle.getToggleState();
    if (masterProcessor != nullptr)
        masterProcessor->setSabrinaBypassed(!active);
    // Or set bypass on Sabrina node in AudioProcessorGraph
};
```

#### Placement B: `TimbresEditor` Master Utility Controls (Mixer Access)
- **Component**: `juce::TextButton sabrinaMasterBtn { "SABRINA" };`
- **Location**: Inside `TimbresEditor` master controls strip, next to `masterPunchBtn` and `masterAutoEqBtn`.
- **Styling**: Uses `NordLookAndFeel::nordRed()` when active, dark gray when bypassed.
- **Benefit**: Allows the musician to quickly bypass Sabrina directly from the main "Church Live" 14-channel mixer view without exiting full-screen mode.

### 4.3 DSP Bypass Execution Logic
When `sabrinaBypassed` is set to `true`:
1. `AudioProcessor::setBypassed(true)` is called on the Sabrina node.
2. In `processBlock`, the audio buffer is passed through directly without performing any DSP calculations, spectral transforms, or convolution buffers:
   ```cpp
   if (bypassed.load(std::memory_order_relaxed))
   {
       // Bypass mode: 0% CPU consumption
       return;
   }
   ```
3. DSP latency is reported as `0` samples when bypassed.

---

## 5. CMakeLists.txt & JUCE Build System Audit

### 5.1 CMake Build Configuration (`CMakeLists.txt`)
- **Minimum CMake**: 3.22
- **C++ Standard**: C++17 (`set(CMAKE_CXX_STANDARD 17)`)
- **MSVC Runtime**: `MultiThreaded$<$<CONFIG:Debug>:Debug>` (Static CRT, runs without external VCRUNTIME redistributable).
- **Target App**: `MinhaDAW` (Product name: `ChurchDAW`, Bundle ID: `com.churchdaw.app`).
- **Platform Flags**:
  - **macOS**: `CMAKE_OSX_ARCHITECTURES "arm64;x86_64"`, `CMAKE_OSX_DEPLOYMENT_TARGET "10.15"`, `-Werror=unguarded-availability-new`. Linked frameworks: `AudioUnit`, `AudioToolbox`, `CoreAudioKit`, `Security`.
  - **Windows**: `/utf-8`, `/STACK:33554432`. Linked libraries: `crypt32`, `bcrypt`, `wintrust`, `shlwapi`, `ole32`. ASIO support enabled if `asiosdk/` folder is present (`JUCE_ASIO=1`).
  - **iOS**: Deployment target 14.0, linked `Security.framework` and `juce_opengl`.
- **Plugin Hosting Definitions**:
  ```cmake
  if(ANDROID OR IOS)
      set(CHURCHDAW_PLUGINS 0)
  else()
      set(CHURCHDAW_PLUGINS 1)
  endif()

  target_compile_definitions(MinhaDAW PRIVATE
      CHURCHDAW_HOSTS_PLUGINS=${CHURCHDAW_PLUGINS}
      JUCE_PLUGINHOST_VST3=${CHURCHDAW_PLUGINS}
      JUCE_WEB_BROWSER=0
      JUCE_PUSH_NOTIFICATIONS=0
      JUCE_USE_CURL=0)
  ```

### 5.2 Android Build Configuration (`ChurchDAW.jucer`)
- **Exporter**: `<ANDROIDSTUDIO>` target folder `Builds/Android`.
- **Activity**: `com.churchdaw.app.ChurchDAWActivity`.
- **Architectures**: `arm64-v8a`.
- **SDK Versions**: Min SDK `24` (Android 7.0), Target SDK `36`.
- **Audio Engine**: `JUCE_USE_ANDROID_OBOE=1` (Oboe high-performance low-latency audio driver).
- **Definitions**:
  ```xml
  defines="CHURCHDAW_HOSTS_PLUGINS=0&#10;JUCE_PLUGINHOST_VST3=0&#10;JUCE_PLUGINHOST_AU=0&#10;JUCE_USE_ANDROID_OBOE=1"
  ```

### 5.3 JUCE Module Dependencies Matrix

| JUCE Module | Desktop Target (`MinhaDAW`) | Android Target (`ANDROIDSTUDIO`) | Required For |
| :--- | :---: | :---: | :--- |
| `juce_core` | Yes | Yes | Core data structures, XML, file I/O |
| `juce_events` | Yes | Yes | Message loop, async updaters, timers |
| `juce_graphics` | Yes | Yes | Vector drawing, fonts, LookAndFeel |
| `juce_data_structures` | Yes | Yes | ValueTrees, properties |
| `juce_gui_basics` | Yes | Yes | Component, Button, Slider, Label, ComboBox |
| `juce_gui_extra` | Yes | Yes | Special UI components, CodeEditor, FileChooser |
| `juce_audio_basics` | Yes | Yes | AudioBuffer, MidiMessage, MidiBuffer, Synthesiser |
| `juce_audio_devices` | Yes | Yes | AudioDeviceManager, MidiInput, MidiOutput |
| `juce_audio_formats` | Yes | Yes | WavAudioFormat, AiffAudioFormat, FlacAudioFormat |
| `juce_audio_processors` | **Yes** | **Yes** | `AudioProcessor`, `AudioProcessorGraph`, `AudioParameterFloat` |
| `juce_audio_processors_headless` | Yes | Yes | Headless plugin scanning & subprocess hosting |
| `juce_audio_utils` | Yes | Yes | AudioProcessorPlayer, AudioDeviceSelectorComponent |
| `juce_dsp` | Yes | Yes | `juce::dsp::IIR`, `juce::dsp::Compressor`, Oversampling |
| `juce_cryptography` | Yes | Yes | RSA signature verification, MD5/SHA256 |
| `juce_opengl` | iOS/Android | Yes | Hardware GPU rendering for mobile interface |

---

## 6. Conclusions & Next Steps

1. **UI Mapping Verified**: `MainComponent` and `TimbresEditor` provide clean, structured attachment points for adding the "Sabrina Bypass" / "Sabrina On/Off" toggle button on Desktop and Mobile.
2. **Cross-Platform Safety**:
   - On Desktop (Windows/macOS), Sabrina can be loaded as a hosted VST3/AU binary or compiled native module.
   - On Mobile (Android), Sabrina must use a compiled native C++ DSP implementation with an automatic bypass fallback if binary VST3 loading is not available.
3. **Build System Readiness**:
   - `juce_audio_processors` is already included in both `CMakeLists.txt` and `ChurchDAW.jucer`.
   - Build flags and target configurations are fully aligned across Windows, Mac, and Android.
