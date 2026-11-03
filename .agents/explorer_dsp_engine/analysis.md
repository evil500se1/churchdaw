# ChurchDAW C++ DSP Audio Architecture Analysis

**Agent**: `explorer_dsp_engine`  
**Target Repository**: `D:\ChurchDAW_Source`  
**Date**: 2026-08-05  

---

## 1. Executive Summary & Core Architectural Overview

ChurchDAW is a specialized, real-time C++ Digital Audio Workstation and virtual instrument environment built on **JUCE 7 (C++17)**, optimized for live worship performance across Windows, macOS, iOS, and Android. The core DSP architecture is built around JUCE's `AudioProcessorGraph` managing real-time audio generation and signal routing across instrument layers, insert/send effect chains, background continuous pads, and master processing.

### Key Architectural Characteristics:
1. **Multi-Layer Instrument Core (`TimbresProcessor`)**: Houses 14 concurrent layers/channels (`numLayers = 14`, `layers[0..13]`; 4 primary live performance layers + 10 additional channel layers). Integrates hybrid synthesis (sample streaming with Hermite interpolation, tonewheel/pipe organ additive modeling, transistor organ divider banks, and worship pad synths).
2. **Graph-Based Processing Topology (`juce::AudioProcessorGraph`)**: Orchestrated by `MainComponent`. Audio and MIDI inputs flow through dedicated graph nodes (`audioInNode`, `midiInNode`), instrument layer processors (`instrument`), insert effect chains (`fxChain` / `slots[1..3]`), continuous ambient pads (`padNode`), and master bus processing (`masterNode`) to physical DAC outputs (`audioOutNode`).
3. **Hard Real-Time Zero-Allocation & Lock-Free Design**:
   - Zero memory allocation (`new`, `delete`, `malloc`, `std::vector::resize`, `juce::String`) on the audio callback thread (`processBlock`).
   - Lock-free parameter synchronization between UI and Audio threads using `std::atomic<float>`, `std::atomic<bool>`, `juce::SmoothedValue<float>`, and single-producer single-consumer (SPSC) ring buffers.
   - Pre-allocated audio buffers (`buffer.setSize(..., avoidReallocating = true)`).
4. **Zero-CPU Dynamic Bypass System**:
   - AudioProcessorGraph node bypass via `node->setBypassed(true)` routes audio buffers directly from input pins to output pins with zero DSP computation, consuming **0% CPU** when an effect is bypassed.
   - Internal DSP bypass mechanisms (`std::atomic<bool>` checks at top of `processBlock` or internal sub-modules) ensure instant CPU drop without buffer reallocation or click/pop artifacts.
5. **Cross-Platform Host Wiring & Mobile Fallback (`CHURCHDAW_HOSTS_PLUGINS`)**:
   - Desktop targets (Windows/macOS) enable VST3 and AU hosting via `CHURCHDAW_HOSTS_PLUGINS=1` and `juce::AudioPluginFormatManager`.
   - Android/iOS targets disable dynamic plugin hosting (`CHURCHDAW_HOSTS_PLUGINS=0`). On Android, `formatManager` is intentionally left empty, guaranteeing zero `dlopen` attempts on unsupported Windows `.dll` / `.vst3` binary files and allowing safe fallback to embedded C++ `AudioProcessor` nodes or bypassed states.

---

## 2. DSP Audio Subsystems & C++ Class Mapping

| Component / Class | File Location | Key Architectural Responsibilities |
|---|---|---|
| `MainComponent` | `Source/MainComponent.h`<br>`Source/MainComponent.cpp` | Top-level host component. Owns `juce::AudioProcessorGraph graph`, `AudioDeviceManager`, node creation, slot state management, and graph reconnection (`rebuildGraphConnections`). |
| `TimbresProcessor` | `Source/TimbresProcessor.h` | 14-layer multi-instrument processor (`juce::AudioProcessor`). Controls voice allocation, sample RAM preload + background disk streaming (`SampleStreamer`), velocity remapping, pitch bend guard, layer EQ, rotary speaker, layer compression, and 100% wet reverb send bus. |
| `MasterProcessor` | `Source/MasterProcessor.h` | Master output processor (`juce::AudioProcessor`). Implements 4-band Auto EQ, "Worship Punch" parallel compression/saturation, master gain ramp (`juce::SmoothedValue`), peak level monitoring, and 8x true-peak `StereoLookaheadLimiter`. |
| `PadProcessor` | `Source/PadProcessor.h` | Background continuous worship pad synth (`juce::AudioProcessor, juce::Thread`). Manages key crossfading (Keys A..G#) and TPT zero-delay cutoff filtering (`PoloTPT`). |
| `WorshipReverb` | `Source/WorshipReverb.h` | 100% wet aux send bus reverb (`ChurchDAWReverb::FDNReverb`). 8-line Feedback Delay Network (FDN) with Hadamard feedback matrix, dual Pitch Shimmer (+12/+7 semitones), 4 allpass diffusers, 3-band T60 decay dampening, orthogonal spatial rotation, and dynamic ducking follower. |
| `ChannelRotarySpeaker` | `Source/ChannelRotarySpeaker.h` | Dual-rotor Leslie speaker cabinet simulator (`ChurchDAWRotary::Speaker`) with 800 Hz crossover, fractional Doppler delay lines, phase modulation, and inertia physics. |
| `ChannelToneShaper` | `Source/ChannelToneShaper.h` | 3-band musical channel EQ (`ChurchDAWChannelTone::ThreeBandEq`) featuring Low shelf, Mid bell, and High shelf filters with parameter smoothing. |
| `StereoLookaheadLimiter` | `Source/StereoLookaheadLimiter.h` | Master peak limiter with 8x true-peak FIR equiripple oversampling detector, 3 ms lookahead delay line, stereo-linked peak detection, and -1.0 dBFS output ceiling. |
| `ChurchOrganCore` / `ChurchOrganLayerProcessor` | `Source/ChurchOrgan/ChurchOrganCore.h`<br>`ChurchOrganLayerProcessor.h` | 4-family organ synthesis engine (Tonewheel B3, Transistor, Pipe, Worship Hybrid) with 9 physical drawbars, Leslie `RotaryCabinet`, and `ScannerVibrato`. |
| `PluginScanSubprocessV2` | `Source/PluginScanSubprocessV2.h` | Out-of-process VST3/AU scanner using `ChildProcessWorker` for isolated plugin validation without crashing the main application. |

---

## 3. Audio Signal Flow & Node Topology

### 3.1 Signal Flow Diagram

```
[ Physical ASIO / Oboe Hardware Audio Input ]
                    │
                    ▼ (inputToggle enabled)
            [ audioInNode ] ──────┐
                                  │
[ Physical MIDI Controller ] ─► [ midiInNode ]
                                      │
                                      ▼ (MIDI Note/CC Dispatch)
                          ┌───────────────────────┐
                          │   TimbresProcessor    │
                          │ (instrument - Slot 0) │
                          └───────────┬───────────┘
                                      │
         ┌────────────────────────────┼────────────────────────────┐
         ▼                            ▼                            ▼
  [ Sample Voices ]         [ Church Organ Core ]         [ Layer DSP Chain ]
  (RAM Preload +            (Pipe/Tonewheel/Transist/     (ChannelToneShaper EQ,
   SampleStreamer)           Worship Hybrid Engine)        ChannelRotarySpeaker)
         │                            │                            │
         └────────────────────────────┼────────────────────────────┘
                                      │
                                      ├───► [ Aux Reverb Bus ] ─► [ WorshipReverb ]
                                      │                                 │ (100% Wet)
                                      ▼                                 │
                               [ Layer Summing ] ◄──────────────────────┘
                                      │
                                      ▼
                        ┌───────────────────────────┐
                        │   fxChain (Slots 1..3)    │
                        │ (Insert VST3/AU / Native) │
                        └─────────────┬─────────────┘
                                      │
  [ PadProcessor (padNode) ] ─────────┼────────────────────────┐
  (Continuous Worship Pad Synth)      │                        │
                                      ▼                        │
                           ┌────────────────────┐              │
                           │  MasterProcessor   │ ◄────────────┘
                           │    (masterNode)    │
                           └──────────┬─────────┘
                                      │
                                      ├─► [ 4-Band Mastering Auto EQ ]
                                      ├─► [ Worship Punch (Parallel Comp + Saturation) ]
                                      ├─► [ Master Gain (SmoothedValue Ramp) ]
                                      └─► [ StereoLookaheadLimiter (-1.0 dBFS Ceiling) ]
                                      │
                                      ▼
                               [ audioOutNode ]
                                      │
                                      ▼
                      [ Hardware Output DAC (ASIO/Oboe) ]
```

### 3.2 Routing Logic in `MainComponent::rebuildGraphConnections`

Located in `Source/MainComponent.cpp:7019-7225`, graph connection building follows a strict transactional pattern:

1. **Transaction Reset**:
   ```cpp
   const auto previousConnections = graph.getConnections();
   for (const auto& c : previousConnections)
       graph.removeConnection(c, juce::AudioProcessorGraph::UpdateKind::none);
   ```
2. **Node ID Lookup**:
   - `instrument` = `nodeForSlot(0)` (`TimbresProcessor` node)
   - `fxChain` = array of active node IDs for `slots[1..3]`
   - `firstDest` = `fxChain.isEmpty() ? masterNode : fxChain.getFirst()`
3. **Audio Connections**:
   - `instrument` -> `firstDest`
   - `audioInNode` (if `inputToggle.getToggleState()` is true) -> `firstDest`
   - Series effect chaining: `fxChain[k]` -> `fxChain[k+1]` for all $k \in [0, \text{fxChain.size()}-2]$
   - `fxChain.getLast()` -> `masterNode`
   - `padNode` -> `masterNode` (summed directly into master input)
   - `masterNode` -> `audioOutNode`
4. **MIDI Connections**:
   - `midiInNode` -> `instrument`
   - `midiInNode` -> `fxChain[k]` (for any effect node where `node->getProcessor()->acceptsMidi()` is true)
5. **Rollback & Atomic Swap**:
   - If any required connection fails (`!requiredConnectionsOk`), the graph removes partial connections and restores `previousConnections`.
   - All connection edits use `UpdateKind::none`, deferring rendering topology update until complete, ensuring the audio thread continuously processes audio without interruption.

---

## 4. Effect Chaining & Dynamic Zero-CPU Bypass Mechanism

### 4.1 AudioProcessorGraph Node Bypass
JUCE's `AudioProcessorGraph::Node` provides native bypassing via `node->setBypassed(bool)`:
- **Execution Path**:
  - When `node->setBypassed(true)` is called, the graph's internal renderer detects the bypass flag.
  - The renderer skips calling `AudioProcessor::processBlock(buffer, midi)` for that node entirely.
  - The input audio buffer for the node is passed directly to output audio connections via zero-copy pointer redirection or direct buffer copy (`copyFrom`).
- **CPU Consumption**:
  - Because `processBlock` is omitted, no floating-point filter calculations, FFTs, convolutions, or plugin callbacks execute.
  - **Result**: Exactly **0% CPU consumption** for the bypassed node.

### 4.2 Module-Internal DSP Bypass Control
For internal processors (`MasterProcessor`, `ChannelToneShaper`, `ChannelRotarySpeaker`, `WorshipReverb`), atomic boolean switches guard execution loops:
- **MasterProcessor (`Source/MasterProcessor.h:160,173`)**:
  ```cpp
  if (autoEqOn.load()) { /* process 4-band IIR filters */ }
  if (punchOn.load())  { /* process parallel compression + saturation */ }
  ```
- **ChannelToneShaper (`Source/ChannelToneShaper.h:83`)**:
  - Checks if gain coefficients equal 0 dB. If bypassed, skips IIR filter processing per channel, preserving bit-exact transparency and zero CPU overhead.
- **Insert Plugin Guard in TimbresProcessor (`Source/TimbresProcessor.h:4678-4690`)**:
  - If an insert plugin fails during `prepareToPlay`, it is flagged in `insertPrepared[slot] = false`. The audio callback bypasses calling `processBlock` on that insert instance, preventing audio thread crashes.

---

## 5. Native VST3/AU Hosting & Safe Android Cross-Platform Fallback

### 5.1 Host Definition Strategy (`CHURCHDAW_HOSTS_PLUGINS`)
Plugin hosting capabilities are conditionally controlled by the `CHURCHDAW_HOSTS_PLUGINS` macro in `Source/AppPaths.h:16-22`:

```cpp
#ifndef CHURCHDAW_HOSTS_PLUGINS
 #if JUCE_ANDROID || JUCE_IOS
  #define CHURCHDAW_HOSTS_PLUGINS 0
 #else
  #define CHURCHDAW_HOSTS_PLUGINS 1
 #endif
#endif
```

- **Desktop (Windows / macOS)**:
  - `CHURCHDAW_HOSTS_PLUGINS = 1`
  - `JUCE_PLUGINHOST_VST3 = 1` (Windows/macOS)
  - `JUCE_PLUGINHOST_AU = 1` (macOS)
  - `formatManager.addDefaultFormats()` registers `VST3PluginFormat` and `AudioUnitPluginFormat`.
  - Plugins are instantiated asynchronously via `formatManager.createPluginInstanceAsync(...)` and wrapped as `AudioProcessorGraph::Node`s inside `slots[1..3]`.

- **Mobile (Android / iOS)**:
  - `CHURCHDAW_HOSTS_PLUGINS = 0`
  - `JUCE_PLUGINHOST_VST3 = 0`
  - `JUCE_PLUGINHOST_AU = 0`
  - Configured explicitly in `ChurchDAW.jucer` and `CMakeLists.txt:717,769`.

### 5.2 Safe Android Fallback Mechanism
Mobile operating systems (Android ARM64) cannot load desktop x86_64 or macOS ARM VST3/DLL binaries due to ABI incompatibility, lack of desktop shared library standards, and OS security sandboxing.

ChurchDAW handles this safely through a multi-tier fallback architecture:
1. **Empty Format Manager (`Source/MainComponent.cpp:462-465`)**:
   - On Android (`#if JUCE_ANDROID`), `formatManager` is intentionally initialized without registering any plugin formats.
   - `knownPlugins` queries return an empty list (`0 plugins available`).
2. **Preset / Project File Compatibility**:
   - When a preset file saved on Desktop (containing third-party VST3 plugin identifiers) is loaded on Android:
     - `MainComponent` attempts to look up the plugin identifier in `knownPlugins`.
     - Because `knownPlugins` is empty on Android, the lookup returns `nullptr`.
     - The slot marks the identifier as `pendingIdentifier` without throwing an exception or attempting a dynamic library load (`dlopen`).
     - The audio graph retains routing integrity by bypassing the slot or replacing it with an embedded native C++ `AudioProcessor` node.
3. **Embedded Native C++ AudioProcessor Wiring**:
   - Internal C++ processors (e.g. `MasterProcessor`, `PadProcessor`, `TimbresProcessor`, or an in-tree compiled plugin like Sabrina) are compiled directly into the binary target as subclasses of `juce::AudioProcessor`.
   - Node creation in `AudioProcessorGraph` uses direct instantiation:
     ```cpp
     auto nodePtr = graph.addNode(std::make_unique<SabrinaProcessor>());
     ```
   - This native C++ node operates seamlessly on Android, iOS, Windows, and macOS without requiring any dynamic VST3/DLL file loading.

---

## 6. Recommendations for Sabrina Plugin Integration

Based on the architectural findings, integrating the "Sabrina" plugin into ChurchDAW requires the following dual-path implementation:

1. **Desktop Path (VST3 Hosting)**:
   - On Windows/macOS (`#if CHURCHDAW_HOSTS_PLUGINS`), copy the compiled VST3 binary (`Sabrina.vst3`) from `Downloads` into the project resource/plugin directory.
   - Instantiate `Sabrina.vst3` via `formatManager.createPluginInstanceAsync` and assign its node to a dedicated slot in `AudioProcessorGraph` (or inside `TimbresProcessor` insert slot).
2. **Mobile Path (Native C++ Embedded Fallback on Android)**:
   - On Android (`#if JUCE_ANDROID`), compile Sabrina's C++ `AudioProcessor` source code directly into the ChurchDAW binary target.
   - Instantiate the native C++ instance directly (`graph.addNode(std::make_unique<SabrinaAudioProcessor>())`).
3. **Toggle & Zero-CPU Bypass**:
   - Expose a UI toggle button (`juce::ToggleButton` / `juce::TextButton`) on `NordLookAndFeel`.
   - When toggled OFF (Bypass active), invoke `node->setBypassed(true)` on the Sabrina graph node.
   - This immediately suspends call execution to Sabrina's `processBlock`, dropping its CPU usage to **0%**.

