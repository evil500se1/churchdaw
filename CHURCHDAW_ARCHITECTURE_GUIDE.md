# CHURCHDAW ARCHITECTURE GUIDE

Master Architectural Specification & Technical System Guide  
**Version**: 1.6.0  
**Target Repository**: `D:\ChurchDAW_Source`  
**Framework**: JUCE 7 / C++17  

---

## 1. System Overview & Architectural Principles

### 1.1 High-Level Vision & Core Purpose
**ChurchDAW** is a specialized, high-performance C++ Digital Audio Workstation (DAW) and virtual instrument workstation custom-designed for live church worship performance. Unlike general-purpose studio DAWs (e.g., Pro Tools, Cubase, Reaper), ChurchDAW is engineered specifically for real-time responsiveness, seamless setlist execution, continuous worship ambient pads, zero-latency multi-layer organ/synth patch switches, and extreme operational reliability under live performance conditions.

Key architectural characteristics include:
- **Low-Latency Live Synthesizer & Sampler Engine**: Multi-layer instrument engine (14 layers/channels: `numLayers = 14`, `layers[0..13]`, including 4 primary live performance layers + 10 additional channel layers) combined with real-time continuous background worship pads.
- **Native Organ Subsystem**: Integrated clean-room 4-family organ synthesis engine (Tonewheel, Transistor, Pipe, Worship Hybrid) with 9 physical-style drawbars, Leslie rotary speaker cabinet modeling, and scanner vibrato/chorus.
- **Studio-Grade DSP Processing**: High-performance Worship Reverb (FDN matrix with dual Pitch Shimmer +12/+7 semitones and dynamic ducking), 3-band musical EQs, parallel compression ("Worship Punch"), and 8x true-peak brickwall lookahead mastering limiter.
- **Live Worship Workflows**: Instant scene management (10-scene setlist), MIDI Learn with CC Pickup protection, continuous pad key crossfading, and mobile-optimized interfaces for hardware stage setups.

### 1.2 Supported Target Platforms
ChurchDAW targets cross-platform deployment across four major operating systems, sharing a single unified C++ core engine while adapting platform-specific drivers, UI layouts, and build toolchains:

1. **Windows (Desktop)**: x86_64 architecture, Microsoft Visual C++ (MSVC), Windows Audio / WASAPI / DirectSound, and native ultra-low-latency Steinberg ASIO SDK 2.3 driver integration. Supports VST3 plugin hosting, RS256 RSA-3072 desktop licensing, and silent background updates via native PE-embedded installer (`ChurchDAWSetup.exe`).
2. **macOS (Desktop)**: Apple Silicon (`arm64`) and Intel (`x86_64`) Universal Binaries targeting macOS 10.15+ (Catalina through macOS 14+). Integrates CoreAudio, CoreMIDI, VST3 & Audio Unit (AU) plugin hosting, macOS Hardened Runtime, and Gatekeeper verification.
3. **iOS (Mobile)**: iPadOS and iOS targets (deployment target 14.0+). Built via Projucer/Xcode, linking Apple `Security.framework` and `juce_opengl`, operating with touch-optimized user interfaces and Oboe/CoreAudio backends.
4. **Android (Mobile)**: Android 7.0+ (API 24 to API 36, `arm64-v8a`). Embedded `Oboe` audio driver (`JUCE_USE_ANDROID_OBOE=1`) for sub-10ms audio output, Android `MidiManager` integration (`android.software.midi`), full-screen touch control center (`AndroidAudioMidiCenter`), and streaming asset distribution across 30 signed zip packages (~13.4 GB total).

### 1.3 Real-Time Zero-Allocation Philosophy & Thread Safety
Audio processing in ChurchDAW operates under strict hard real-time execution constraints. The fundamental rule across all audio thread routines (`processBlock`, `renderVoices`, `WorshipReverb::process`, `StereoLookaheadLimiter::process`, `ChurchOrganCore::process`) is **Zero Memory Allocation and Zero Locking**:

- **No Heap Allocation**: Calls to `new`, `delete`, `malloc`, `free`, `std::vector::resize`, `std::vector::push_back`, or `juce::String` creation are strictly forbidden on the audio callback thread.
- **Pre-Allocated Buffers**: Audio buffers, ring buffers, delay lines, and sample preloads are pre-allocated during initial setup (`prepareToPlay` / `prepare`). Resizing during buffer size changes uses `buffer.setSize(2, numSamples, false, false, true)` with `avoidReallocating = true`.
- **Lock-Free Parameter & Event Transfer**: Communication between the UI (Message Thread) and Audio Thread relies exclusively on lock-free primitives, including `std::atomic<float>`, `juce::SmoothedValue`, atomic bitmasks (`mascaraDeNotasMapeadas`), and single-producer single-consumer (SPSC) lock-free ring buffers (`ChurchOrganParameterSink`).
- **No Blocking I/O or System Calls**: Audio callbacks never execute disk I/O, network requests, thread synchronization primitives (`std::mutex`, `juce::CriticalSection`), or logging calls. Sample disk reads are completely offloaded to background worker threads (`SampleStreamer : private juce::Thread`).

### 1.4 Dark Nord Synthesizer GUI Design System Philosophy
The visual design system of ChurchDAW (`NordLookAndFeel.h`) takes direct inspiration from high-end hardware stage synthesizers (such as Nord Electro / Stage series) and professional OLED control surfaces:

- **Nord Aesthetic & Color Palette**: Built on dark graphite card backgrounds (`bgCard` `#171B22`), deep app charcoal (`bgApp` `#0C0E12`), striking brand red accents (`nordRed` `#E91845`, `nordRedBright` `#FF2F5C`), active amber highlights (`amberSolo` `#FFB020`), success status green (`greenOk` `#4FD68A`), and peak clip warning red (`redClip` `#FF1744`).
- **Instrument Category Coding**: Timbres and slots are color-coded by category for instant visual identification during live performances:
  - **Piano**: Slate Blue (`#5B8A9A`)
  - **Pad**: Soft Purple (`#8A6BA8`)
  - **Bell**: Warm Gold (`#C4A35A`)
  - **Strings**: Muted Teal (`#4A9B8C`)
  - **Organ**: Vintage Amber (`#B7773C`)
  - **Lead**: Cobalt Blue (`#4E86C5`)
  - **Plugin**: Burnt Orange (`#C9823A`)
- **Procedural Vector Fallbacks (`drawChurchDAWMark`)**: Rendered vector artwork function that draws the ChurchDAW emblem (church silhouette, steeples, cross, stained glass window, and keyboard keys) directly in code using JUCE vector paths. This bypasses GPU texture upload bugs and PNG memory overheads on mobile Android drivers.
- **UI Performance Throttling**: Level meters (`LevelMeter.h`) and OLED displays update at 30 Hz on Desktop and 10 Hz on Mobile (`#if JUCE_ANDROID || JUCE_IOS`) to prevent message-thread CPU saturation on high-resolution displays. Pre-rendered drop shadows (`oledShadowCache`) are cached to preserve GUI smoothness.

### 1.5 Dual Build System Rationale
ChurchDAW adopts a dual build system strategy to satisfy platform toolchain requirements:

- **CMake 3.22+ (Desktop & iOS)**: Root `CMakeLists.txt` manages Windows (MSVC 2022), macOS (Apple Clang universal binaries), and iOS. CMake provides clean target modularity, custom compile definition flags, MSVC stack reserve options (`/STACK:33554432`), and automated offline test suite generation (`CHURCHDAW_BUILD_AUDIO_TESTS`).
- **JUCE Projucer (`ChurchDAW.jucer` for Android)**: As documented in official JUCE specifications (`JUCE/docs/CMake API.md`), JUCE's CMake integration does not support Android target exports. Therefore, Android builds are generated via Projucer (`ChurchDAW.jucer`), which emits Android Studio Gradle projects targeting NDK ABI `arm64-v8a`, Java activity bridge `com.churchdaw.app.ChurchDAWActivity`, and Oboe audio backend (`JUCE_USE_ANDROID_OBOE=1`).
- Both build systems consume the exact same source code in `Source/` and `Source/ChurchOrgan/`, ensuring complete behavioral equivalence across all platforms.

---

## 2. Subsystem Architecture & C++ Module Mapping

### 2.1 C++ Class & Subsystem Mapping Table

| C++ Class / Header | Target Directory | Primary Architectural Responsibilities |
|---|---|---|
| **Audio Engine & Core Processing** | | |
| `MainComponent` | `Source/MainComponent.h` | Top-level host component, audio device manager binder (`juce::AudioDeviceManager`), audio processor graph manager (`juce::AudioProcessorGraph`), MIDI input callback dispatcher, and UI coordinator. |
| `TimbresProcessor` | `Source/TimbresProcessor.h` | Core 14-layer/channel multi-instrument processor (`juce::AudioProcessor`, `numLayers = 14`, `layers[0..13]`; 4 primary live performance layers + 10 additional channel layers). Controls layer voice allocation, RAM preload + disk streaming, velocity remapping, pitch bend guard, layer EQ, rotary speaker, layer compression, insert VST/AU hosting, and reverb send bus. |
| `MasterProcessor` | `Source/MasterProcessor.h` | Master output bus processor (`juce::AudioProcessor`). Implements 4-band mastering Auto EQ, "Worship Punch" parallel compression/saturation, master gain, and brickwall peak limiting. |
| `PadProcessor` | `Source/PadProcessor.h` | Background continuous worship pad synth processor (`juce::AudioProcessor, juce::Thread`). Manages key crossfading (Keys A through G#) and TPT zero-delay cutoff filtering. |
| `StereoLookaheadLimiter` | `Source/StereoLookaheadLimiter.h` | Master peak limiter using 8x true-peak FIR equiripple oversampling detector, 3 ms lookahead delay line, stereo-linked peak detection, -1.0 dBFS output ceiling, and -2.1 dBFS detector threshold. |
| `SampleStreamer` | `Source/SampleStream.h` | Background worker thread (`juce::Thread`) handling asynchronous windowed disk reads (`windowSamples = 32768`, `windowOverlap = 4096`) for long sample tails while initial transients play from RAM preload buffers. |
| `SamplePack` | `Source/SamplePack.h` | Encrypted pack reader providing ChaCha20 decryption, SHA-256 hash verification, and memory mapping for sample pack archives. |
| `TimbreCatalog` | `Source/TimbreCatalog.h` | Timbre metadata catalog, JSON index parser, instrument categories, and preset pack inventory manager. |
| **DSP Module Components** | | |
| `WorshipReverb` | `Source/WorshipReverb.h` | Native Worship/Pro reverb algorithm operating as a 100% wet send bus. Features 12 algorithm presets, 8-line Feedback Delay Network (FDN) with Hadamard feedback matrix, dual Pitch Shimmer (+12 and +7 semitones), 4 allpass diffusers, 3-band T60 decay dampening, orthogonal spatial rotation, and dynamic ducking follower. |
| `ChannelRotarySpeaker` | `Source/ChannelRotarySpeaker.h` | Dual-rotor Leslie speaker cabinet simulator (`ChurchDAWRotary::Speaker`) for sampled, transistor, and pipe organ layers (Horn/Drum crossover 800 Hz, fractional Doppler delay, phase modulation, inertia physics). |
| `ChannelToneShaper` | `Source/ChannelToneShaper.h` | 3-band musical channel EQ (`ChurchDAWChannelTone::ThreeBandEq`) featuring Low shelf, Mid bell, and High shelf filters with `juce::SmoothedValue` parameter interpolation. |
| **Church Organ Synthesis Subsystem** | `Source/ChurchOrgan/` | |
| `ChurchOrganCore` | `ChurchOrganCore.h` | Core organ engine facade and dispatcher managing clean-room organ synthesis, parameter snapshots, and family execution. |
| `ChurchOrganLayerProcessor` | `ChurchOrganLayerProcessor.h` | `juce::AudioProcessor` wrapper adapting `ChurchOrganCore` into `TimbresProcessor` layer 4 slot. |
| `PipeOrganEngine` | `PipeOrganEngine.h` | Physical/additive modeling pipe organ engine supporting multi-rank stop combinations, wind chest fluctuation, and room ambience. |
| `TonewheelOrganEngine` | `TonewheelOrganEngine.h` | Hammond B3-style electromechanical organ engine featuring 9 drawbars per manual, 91 tonewheels, crosstalk, foldback, and key click. |
| `TransistorOrganEngine` | `TransistorOrganEngine.h` | Vox Continental / Farfisa style combo organ engine with frequency divider banks, tab filters, and vibrato. |
| `WorshipHybridEngine` / `WorshipLayerEngine` | `WorshipHybridEngine.h` | Synth layer engine combining organ core with atmospheric synth pads and single-knob "Worship Intensity" macro dynamics. |
| `RotaryCabinet` | `RotaryCabinet.h` | Native organ Leslie cabinet DSP embedded inside `ChurchOrganCore` (dual rotor, Doppler delay lines, horn/drum physics, air absorption lowpass, adaptive wet loudness calibration). |
| `ScannerVibrato` | `ScannerVibrato.h` | Electromechanical scanner vibrato/chorus simulation (V1, V2, V3, C1, C2, C3 settings). |
| `OrganAmpCab` | `OrganAmpCab.h` | Tube amplifier overdrive, transformer saturation, and cabinet speaker impulse response filter DSP. |
| `TonewheelBank` | `TonewheelBank.h` | 91 tonewheel generator bank providing phase alignment, leakage, and vintage character profiles (Clean, Aged, Road). |
| `TransistorDividerBank` | `TransistorDividerBank.h` | Frequency divider bank DSP generating square/sawtooth octave synthesis for transistor organs. |
| `KeyClickGenerator` | `KeyClickGenerator.h` | Mechanical key click transient generator simulating contact bounce. |
| `MultiContactKeying` | `MultiContactKeying.h` | Multi-contact keying DSP providing micro-timing offsets across 9 drawbar contacts. |
| `HarmonicPercussion` | `HarmonicPercussion.h` | 2nd and 3rd harmonic percussion engine with Decay and Volume controls. |
| `AutoMixContext` | `AutoMixContext.h` | Dynamic organ auto-mixing context regulating headroom and spectral density during multi-note chord voicing. |
| **User Interface (UI) Layer** | | |
| `NordLookAndFeel` | `Source/NordLookAndFeel.h` | Central LookAndFeel design system. Defines color tokens (`bgApp`, `nordRed`, category colors), custom sliders, toggles, tickboxes, and vector mark fallback (`drawChurchDAWMark`). |
| `OledDisplay` | `Source/OledDisplay.h` | Hardware OLED screen emulator displaying current program bank, active channel setup, transpositions, and real-time chord detection. |
| `LevelMeter` | `Source/LevelMeter.h` | Stereo peak/RMS level meter component with platform update rate throttling (10 Hz Mobile / 30 Hz Desktop). |
| `DeviceInfoBar` | `Source/DeviceInfoBar.h` | Status bar showing connected MIDI keyboard devices, audio sample rate, buffer size, and driver state chips. |
| `AudioMidiButton` | `Source/AudioMidiButton.h` | Custom trigger button opening audio driver and MIDI configuration dialogs. |
| `PadPanel` | `Source/PadPanel.h` | Continuous Worship Pad control interface (12-key selector, TPT filter cutoff knob, pad volume slider). |
| `LiveScenePanel` | `Source/LiveScenePanel.h` | Setlist manager providing 10 scene buttons for instant patch switching during live performance. |
| `TimbrePicker` | `Source/TimbrePicker.h` | Searchable popover drawer for browsing, previewing, and selecting instrument patches across categories. |
| `MidiLearnPanel` | `Source/MidiLearnPanel.h` | 2-step interactive wizard overlay embedded in OLED display for MIDI CC/Note binding. |
| `LibraryPanel` | `Source/LibraryPanel.h` | Sample pack catalog panel displaying download progress, segment validation, and library storage details. |
| `ChurchOrganEditor` | `Source/ChurchOrgan/ChurchOrganEditor.h` | Console editor for Church Organ subsystem, housing the 9 physical drawbars, rotary controls, percussion toggles, and worship intensity slider. |
| `StartupPanel` | `Source/StartupPanel.h` | Application splash screen, initial audio setup wizard, and mandatory update notification modal. |
| `AboutPanel` | `Source/AboutPanel.h` | About dialog presenting version details, credits, system metrics, and brand logo. |
| `AndroidAudioMidiCenter` | `Source/AndroidAudioMidiCenter.h` | Android-specific full-screen touch control center for low-latency buffer tuning and USB/Bluetooth MIDI selection. |
| `DesktopLoginPanel` / `AndroidLoginPanel` | `Source/DesktopLoginPanel.h` | Account login and license lease verification interface. |
| **Visual Assets** | `Assets/` & `Installer/` | |
| Category Glyphs | `Assets/glyph_*.png` | High-resolution PNG category icons: `glyph_piano.png`, `glyph_pad.png`, `glyph_organ.png`, `glyph_strings.png`, `glyph_ambience.png`, `glyph_bells.png`, `glyph_lead.png`. |
| Application Icons | `Assets/icon*.png`, `Installer/*.ico` | Primary app launcher icons (`icon.png`, `icon-android.png`, `icon-android-chroma.png`, `logo_round.png`, `ChurchDAW.ico`, `app-icon-512.png`). |

---

## 3. Audio Signal Flow & Routing Diagrams

### 3.1 Step-by-Step Audio Signal Flow Description

```
[ Physical ASIO / Hardware Audio Inputs ] (Mic/Guitar)
             │
             ▼ (inputToggle gate in MainComponent)
[ audioInNode ] ──────┐
                      │
[ Physical MIDI Inputs ] ─► [ midiInNode ]
                                  │
                                  ▼ (MIDI NoteOn/Off, CC, Velocity Curve Remapping)
                      ┌──────────────────────────────────────┐
                      │           TimbresProcessor           │
                      │  (14 Instrument Layers/Channels:     │
                      │   numLayers = 14, layers[0..13])      │
                      └──────────────────┬───────────────────┘
                                         │
                 ┌───────────────────────┼───────────────────────┐
                 │                       │                       │
                 ▼                       ▼                       ▼
          [ Layer Voices ]      [ Church Organ Core ]     [ Layer DSP Chain ]
        (Sample Playback /     (Pipe/Tonewheel/Transist/   (ChannelToneShaper 3-Band EQ,
         RAM Preload + Disk      Worship Hybrid Engine +     ChannelRotarySpeaker,
            Streamer)           Native RotaryCabinet)      Layer Compressor, Delay)
                 │                       │                       │
                 └───────────────────────┼───────────────────────┘
                                         │
                                         ├───► [ Aux Reverb Bus ]
                                         │            │ (100% Wet Send)
                                         │            ▼
                                         │     [ WorshipReverb ]
                                         │    (FDN Matrix, Shimmer,
                                         │     Ducking, 12 Presets)
                                         │            │
                                         │ (Return Ramp)
                                         ▼            │
                                  [ Layer Summing ] ◄─┘
                                         │
                                         ▼ (Insert VST/AU FX Chain: Slots 1..2)
                                  [ timbresNode ]
                                         │
  [ PadProcessor (padNode) ] ────────────┼────────────────────────
  (Continuous Worship Pad, TPT LPF)      │
                                         ▼
                               [ MasterProcessor ]
                             (masterNode)
                                  │
                                  ├─► [ Auto EQ (4-Band Mastering Curve) ]
                                  ├─► [ Worship Punch (Parallel Compression + Saturation + Bass Weight) ]
                                  ├─► [ Master Volume Gain (SmoothedValue) ]
                                  └─► [ StereoLookaheadLimiter (8x True-Peak FIR, 3ms Lookahead, -1.0 dBFS Ceiling) ]
                                         │
                                         ▼
                                 [ audioOutNode ]
                                         │
                                         ▼
                          [ ASIO / Hardware Output DAC ]
```

#### Detailed Signal Flow Walkthrough:

1. **Step 1: Input & MIDI Handling**:
   - Audio and MIDI enter the DAW graph via `juce::AudioProcessorGraph` nodes created in `MainComponent::rebuildGraph`: `audioInNode` (`AudioGraphIOProcessor::audioInputNode`) and `midiInNode` (`AudioGraphIOProcessor::midiInputNode`).
   - Incoming MIDI messages arrive at `TimbresProcessor::processBlock`. Velocity values are remapped via `mapVelocity` according to the active layer's velocity curve (`Natural`, `Hard`, `Soft`, `Linear`).
   - Pitch bend guard logic (`PitchBendGuard`) neutralizes pitch bend glitches and guarantees safe handling of CC64 (Sustain pedal), CC120/123 (All Sound/Notes Off), and Global Panic events.

2. **Step 2: Voice Rendering & Synthesis**:
   - `TimbresProcessor::renderVoices` evaluates active voices across 14 layers/channels (`layers[0..13]`, `numLayers = 14`; 4 primary live performance layers + 10 additional channel layers).
   - **Sampled Layers**: Initial transient attacks (~0.5s) are rendered instantly from preloaded RAM buffers (`preloadLen`). The remaining sample tail is read asynchronously from disk by `SampleStreamer` worker threads (`windowSamples = 32768`, `windowOverlap = 4096`) using Hermite cubic interpolation (`hermiteFast`).
   - **Church Organ Subsystem (Layer 3)**: Dispatched to `ChurchOrganLayerProcessor` / `ChurchOrganCore`. Depending on selected organ mode, triggers `PipeOrganEngine`, `TonewheelOrganEngine`, `TransistorOrganEngine`, or `WorshipHybridEngine`. Integrated Leslie simulation is applied via native `RotaryCabinet` or `ChannelRotarySpeaker`.

3. **Step 3: Layer DSP Processing**:
   - Each individual layer buffer passes through its dedicated DSP chain:
     - `ChannelToneShaper`: 3-band musical channel EQ (Low shelf, Mid bell, High shelf) with parameter smoothing (`juce::SmoothedValue`).
     - `ChannelRotarySpeaker`: Dual-rotor Leslie speaker cabinet DSP (Horn/Drum 800 Hz crossover, fractional Doppler delay lines, Slow/Stop/Fast inertia).
     - Layer Dynamics: Dedicated per-layer compression (`comps[l]`), parametric EQ (`manualEqs[l]`), and delay buffer (`delayBufs[l]`).

4. **Step 4: Aux Reverb Send / Return Bus**:
   - Each layer writes its wet send proportion to `reverbBus`.
   - `WorshipReverb::process` processes 100% wet audio through the following DSP stages:
     - 95 Hz Input High-Pass Filter -> Stereo Predelay -> 4 Allpass Diffusers -> 8-line Feedback Delay Network (FDN) Matrix with Hadamard feedback matrix -> 3-Band T60 Decay Dampening -> Orthogonal Matrix Motion -> Dual Pitch Shimmer (+12/+7 semitones) -> Dynamic Ducking Follower -> Stereo Width -> 14.5 kHz Low-Pass Filter.
   - The processed wet reverb return is ramped via `reverbReturnSmooth` and summed back into the layer output buffer.

5. **Step 5: Insert FX Slots & Continuous Ambient Pad**:
   - Summed layer audio flows through up to 2 hosted insert FX slots (`inserts[0..1]`, `numInserts = 2`) in `timbresNode` (supporting VST3 on Windows/macOS and AU on macOS).
   - Simultaneously, `PadProcessor` (`padNode`) runs independently on a background thread, streaming continuous ambient worship pads (Keys A through G#) with smooth key crossfading and TPT zero-delay low-pass filtering (`PoloTPT`).

6. **Step 6: Master Bus Processing (`MasterProcessor`)**:
   - Audio enters `MasterProcessor::processBlock` (`masterNode`):
     - **Auto EQ**: 4-band mastering curve (Low shelf 110 Hz +1.5 dB, Mid peak 450 Hz -1.5 dB, High peak 3000 Hz -1.0 dB, High shelf 11000 Hz +2.0 dB).
     - **Worship Punch**: Parallel compression path (HPF 130 Hz -> Compressor 3.5:1 -> Tanh saturation `tanh(p * 1.95) * 0.45` -> Low shelf 90 Hz +3.0 dB -> Mid peak 320 Hz -2.0 dB) summed back to master.
     - **Master Gain**: Master volume scaling (`dbParam`, -60 dB to +10 dB) using `juce::SmoothedValue`.
     - **Stereo Lookahead Limiter**: `StereoLookaheadLimiter::process` applies an 8x true-peak FIR equiripple oversampling detector, 3 ms lookahead delay, -1.0 dBFS output ceiling, and -2.1 dBFS detector threshold.

7. **Step 7: Physical DAC Output**:
   - Output audio passes from `masterNode` to `audioOutNode` (`AudioGraphIOProcessor::audioOutputNode`) -> `juce::AudioProcessorPlayer` -> physical hardware output channels 1 & 2 via ASIO, WASAPI, CoreAudio, or Oboe.

---

## 4. Build System, Dependencies & Target Environments

### 4.1 Dual Build System Strategy
ChurchDAW utilizes a hybrid build setup:

1. **Root CMake (`CMakeLists.txt`)**:
   - Requires CMake 3.22+ targeting C++17 (`CMAKE_CXX_STANDARD 17`).
   - Project: `project(MinhaDAW VERSION 1.6.0)`, producing executable `"ChurchDAW"`.
   - **MSVC Configuration**: Applies `/utf-8` compiler flags and configures static runtime linking (`MultiThreaded$<$<CONFIG:Debug>:Debug>`). Expands stack reserve via `/STACK:33554432` (32 MiB) to prevent stack overflow (`0xC00000FD`) during heavy C++ organ matrix table constructions.
   - **macOS Configuration**: Supports deployment targets `10.15` (Intel) and `11.0` (Apple Silicon) using universal binary architectures `set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")`. Enforces `-Werror=unguarded-availability-new` and merges `Packaging/ChurchDAW-macOS-Info.plist` with Hardened Runtime entitlements. Includes Bluetooth entitlement (`NSBluetoothAlwaysUsageDescription`) to prevent macOS TCC privacy crashes during CoreMIDI enumeration.
   - **iOS Configuration**: Sets iOS deployment target 14.0, linking native iOS verification and OpenGL frameworks.

2. **Projucer / Mobile Gradle (`ChurchDAW.jucer`)**:
   - Generates Android Studio Gradle build exported for Android 7.0+ (API 24 to 36, `arm64-v8a`).
   - Configures `JUCE_USE_ANDROID_OBOE=1` for low-latency native Oboe audio callbacks.
   - Declares mandatory Android manifest features: `<uses-feature android:name="android.software.midi" android:required="true" />` and Bluetooth MIDI connect/scan permissions.

### 4.2 JUCE Framework & ASIO SDK Dependencies
- **JUCE Framework (15 Modules)**: Embedded in `JUCE/` root directory. Core modules used:
  `juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`, `juce_audio_processors`, `juce_audio_processors_headless`, `juce_audio_utils`, `juce_core`, `juce_cryptography`, `juce_data_structures`, `juce_dsp`, `juce_events`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`, `juce_opengl`.
- **Global Feature Disables**: To minimize binary size and eliminate external runtime crashes, key JUCE features are globally disabled across CMake and Projucer:
  `JUCE_WEB_BROWSER=0`, `JUCE_PUSH_NOTIFICATIONS=0`, `JUCE_USE_CURL=0`.
- **Steinberg ASIO SDK 2.3**: Located in `asiosdk/`. Automatically detected by CMake via `if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common/iasiodrv.h")`, activating `JUCE_ASIO=1` and adding `asiosdk/common` to include paths.

### 4.3 Feature Flags Breakdown

| Feature Flag | Default | Description & Behavior |
|---|---|---|
| `CHURCHDAW_HOSTS_PLUGINS` | `1` (Desktop) / `0` (Mobile) | Controls VST3 and AU third-party insert plugin hosting support. Enabled on Windows/macOS; disabled on Android/iOS. |
| `CHURCHDAW_ENABLE_DESKTOP_LICENSING` | `ON` (Desktop) | Controls desktop lease verifier, login panel, and platform credential stores (`crypt32`/`bcrypt` on Windows, `Security.framework` on macOS). |
| `CHURCHDAW_BUILD_AUDIO_TESTS` | `OFF` | Enables compilation of 16 offline audio/DSP CTest console targets (including `ChurchDAWReverbBoundsProbe`). |
| `CHURCHDAW_BUILD_INSTALLER` | `OFF` | Enables compilation of the standalone Windows installer target `ChurchDAWSetup` (`Installer/ChurchDAWSetup.cpp`). |
| `CHURCHDAW_LICENSE_API_BASE` | `"https://churchdaw.com/api/v1"` | Remote base URL for licensing and authentication services. |
| `CHURCHDAW_UPDATE_POLICY_URL` | `"https://churchdaw.com/app/desktop/update-policy-v1.json"` | Remote URL for fetching RS256-signed update policy manifests. |

### 4.4 Target Environment Differences Table

| Feature / Dimension | Desktop (Windows / macOS) | Mobile (Android / iOS) |
|---|---|---|
| **Build Toolchain** | Root CMake 3.22+ (MSVC 2022 / Apple Clang) | Projucer / Android Studio Gradle |
| **Plugin Hosting** | Enabled (`1`) — VST3 (Win/Mac), AU (Mac) | Disabled (`0`) — relies exclusively on internal synth/samples |
| **Licensing Client** | Active — RSA-3072 lease verifier, login UI | Disabled — managed via app store / device registration |
| **Updater Strategy** | Mandatory silent installer (`DesktopUpdater.cpp`) | HTTPS signed policy gate + sideload APK / Play Store |
| **Audio Backend** | Steinberg ASIO, WASAPI, CoreAudio | Oboe (`JUCE_USE_ANDROID_OBOE=1`) |
| **Stack Reserve** | Explicit 32 MiB (`/STACK:33554432`) | Standard thread stack size |
| **GUI Meter Rate** | 30 Hz refresh rate | 10 Hz refresh rate (throttled for CPU savings) |

---

## 5. Development History & Codebase Evolution Notes

### 5.1 Analysis of Key History & Note Files

#### A. `BUG_REVERB_LIMITES.md` (DSP Out-of-Bounds Indexing Fix)
- **Problem**: Intermittent application crash (`0xC0000409` / `vector subscript out of range` assertion in MSVC Debug) and inconsistent T60 decay calculations (e.g. T60 at 125 Hz fluctuating between 63.96s and 10.11s).
- **Exact Location**: `WorshipReverb.h:505` inside `FractionalDelay::read`, called by `WorshipReverb::process` line 290.
- **Diagnostics**:
  - MSVC Debug STL: `vector subscript out of range` in `<vector>` line 1941.
  - AddressSanitizer (RelWithDebInfo): `ERROR: AddressSanitizer: container-overflow READ of size 4 at 0x11fd48faf140` (index calculated to `data.size() + 8`, reading allocated heap capacity beyond logical `size()`).
- **Root Cause**: Fractional delay line wrap-around performed float arithmetic: `float position = (float) writeIndex - delay; while (position < 0.0f) position += (float) data.size();`. When `position` was negative but within 1 ULP (Unit in the Last Place) of the buffer boundary, float addition rounded up to **exactly `data.size()`**. Casting to `size_t` produced an out-of-bounds index `data.size()`.
- **Fix**: Replaced float wrap-around with integer decomposition (`wholeDelay + fraction`) and strictly bounded integer index arithmetic within `[0, count - 1]`.
- **Regression Probe**: Created `Tests/Audio/ReverbBoundsProbe.cpp` (Target `ChurchDAWReverbBoundsProbe`). Validated under Debug and AddressSanitizer across 44.1/48/88.2/96 kHz, block sizes 32 to 8192, and all 12 reverb presets.

#### B. `ATUALIZACAO_DESKTOP.md` (Desktop Auto-Updater & Unified Catalog Security)
- **Architecture**: Desktop mandatory update system implemented in `Source/DesktopUpdater.cpp` and `Installer/ChurchDAWSetup.cpp`.
- **Dual Security Model**:
  1. RS256 RSA-3072 Digital Signature over update policy JSON (shared key `churchdaw-update-2026-01`; verified via `crypt32`/`bcrypt` on Windows and `Security.framework` on macOS).
  2. Operating System Binary Signature (Authenticode `WinVerifyTrust` on Windows; Gatekeeper `spctl --assess` on macOS).
- **Core Rules**:
  - **Rule 1 (Live Worship Protection)**: Updates are never evaluated during active performance (`mayEnforceNow()` checks for active voices at startup in `Main.cpp`).
  - **Rule 2 (Anti-Brick Fallback)**: If network connections fail or signatures are unverified, the app opens normally with a non-blocking notification.
  - **Rule 3 (Loop Prevention Lock)**: The executed installer hash is saved in `%APPDATA%\ChurchDAW\update-state.json` to prevent endless update loops.
- **Windows Silent Installer (`Installer/ChurchDAWSetup.cpp`)**: Native installer targeting `%LOCALAPPDATA%\Programs\ChurchDAW` (avoiding UAC admin prompts on church PCs). Embeds `ChurchDAW.exe` inside PE `RCDATA` resource so Authenticode signature covers the embedded executable payload.
- **Unified Catalog Fix**: Discovered `verifyAndroidSignedPolicyEnvelope` previously returned `false` on non-Android, blocking desktop clients from downloading sample packs. Fixed by connecting non-Android platforms to the native RS256 verifier, establishing a single unified catalog (`/app/android/manifest.json`) across Windows, macOS, and Android.

#### C. `KIMI_NOTAS.md` (Parallel Technical Diagnostic Notes)
- **Catalog Integration Gate**: Confirmed `CATALOG_LEGACY_ORGAN` passed 4/4 runs via `ChurchDAWTimbreLoadStressTests.exe --controls-only`.
- **Async State Restore Mapping**: Analyzed `setStateInformation` scanning triggers and async layer load queuing in `drainPendingLayerLoads()`.
- **Rotary Speaker Architecture & Routing**: Documented two distinct DSP paths for rotary modulation:
  1. Sampled / Transistor / Pipe Timbres -> `ChannelRotarySpeaker::process` in `TimbresProcessor.h`.
  2. Tonewheel / Worship Organ -> Native `RotaryCabinet` embedded inside `Source/ChurchOrgan/RotaryCabinet.cpp`.
  - Detailed diagnostic of `RotaryCabinet`: Adaptive wet calibration gain (`wetCalibration_` with 3.5s smoothing), air absorption lowpass filter (`wetLeft = leftAirState_`), correlation compensation (floor 0.78), and cabinet floor (0.14+).
- **macOS Transpose Fix**: Verified fixes for accidental channel transpositions (unbound Launchkey Knob 8, filtered unsafe stored targets via `isSafeStoredTarget`, established per-layer `noteTranspose` ledger).

#### D. Context from Additional Project Documentation
- `PROJECT.md`: History of Casio patch extraction (`.tn7` tone patch, `.tu7` wave bank 1.966 GB), SysEx wipe commands, and USB MIDI bulk transfer scripts.
- `analysis.md` & `handoff.md`: Native SFZ/JSON specification standard mined for 28 KEEP instruments (Ultimate Pianos 2 catalog integration), Ableton pitch formula $C3 = 60$, continuous key range coverage `[0..127]`, and `ValidateTimbrePack.py` compliance.
- `TEST_READY.md`: E2E Milestone 4 verification suite results for `Pad Worship 2.0 ED Bento` across 5 tiers (100% pass rate across 21 E2E tests, 18 validator tests, and white-box stress scenarios).
- `Packaging/AndroidDistribution.md`: Sideload update policy (schema v2, RSA-3072 key `churchdaw-update-2026-01`, serial monotonicity) and 30-segment content library (~13.4 GB total).
- `Design/ChurchOrganEngine/ARCHITECTURE.md`: Architecture specification for clean-room 4-family organ engine (`tonewheel`, `transistor`, `pipe`, `worshipHybrid`) and single-knob "Worship Intensity" macro dynamics.

---

## 6. Maintenance & Architectural Recommendations

### 6.1 Real-Time Audio Thread Rules
Future contributors to ChurchDAW must strictly adhere to the following real-time audio coding standards:

1. **Zero Heap Allocation**: Never call `new`, `delete`, `malloc`, `free`, `std::vector::resize`, `std::string`, or `juce::String` within audio processing callbacks (`processBlock`, `renderVoices`, `process`).
2. **Pre-Allocated Buffer Sizing**: Always pre-allocate audio buffers during `prepareToPlay` / `prepare`. When handling dynamic block sizes, call `buffer.setSize(..., avoidReallocating = true)`.
3. **Lock-Free Thread Synchronization**: Never acquire `std::mutex`, `juce::CriticalSection`, or execute blocking file/network I/O on the audio thread. Use `std::atomic<T>`, `juce::SmoothedValue`, or lock-free SPSC ring buffers.
4. **Defensive Value Sanitization**: Protect DSP routines against non-finite values (`NaN`, `Inf`, subnormals) using `juce::FloatVectorOperations` or explicit sanitization checks.

### 6.2 UI Rendering Optimizations
1. **Refresh Rate Throttling**: Keep level meters (`LevelMeter.h`) and visual timers throttled to 10 Hz on mobile targets (`JUCE_ANDROID`, `JUCE_IOS`) and 30 Hz on desktop targets.
2. **Vector Graphic Fallbacks**: Utilize procedural vector drawing functions (`drawChurchDAWMark`) for complex branding marks to prevent GPU texture allocation issues on mobile OpenGL drivers.
3. **Drop Shadow Caching**: Pre-render and cache drop shadows (`oledShadowCache` in `OledDisplay.h`) rather than computing Gaussian blurs dynamically during repaints.

### 6.3 Subprocess Plugin Scanning Safety
Third-party VST3 and AU plugins pose instability risks (crashes, memory access violations, hang conditions during scanning). ChurchDAW enforces out-of-process isolation via `ChildProcessWorker` (`PluginScanSubprocessV2.h`):
- Plugin scanning and metadata extraction are executed inside an isolated worker process.
- If a third-party plugin crashes during initialization, only the worker process terminates, leaving the main ChurchDAW host process completely safe and operational.

### 6.4 Developer Maintenance Checklist
Before submitting modifications or releasing new builds of ChurchDAW, contributors should complete the following verification steps:

- [ ] **Run Build Command**: Verify full compilation under MSVC (Windows) and Apple Clang (macOS) with zero warnings (`/utf-8`, `-Werror=unguarded-availability-new`).
- [ ] **Run DSP Unit & Probe Tests**:
  ```powershell
  cmake -S . -B build -DCHURCHDAW_BUILD_AUDIO_TESTS=ON
  cmake --build build --config Debug --target ChurchDAWReverbBoundsProbe
  .\build\ChurchDAWReverbBoundsProbe_artefacts\Debug\ChurchDAWReverbBoundsProbe.exe
  ```
- [ ] **Run AddressSanitizer & STL Debug Checks**: Confirm no `container-overflow` or buffer subscript out of range assertions occur under varying sample rates (44.1 kHz to 96 kHz).
- [ ] **Verify Layout Compliance**: Ensure all source code remains in `Source/` and `Source/ChurchOrgan/`, tests in `Tests/`, assets in `Assets/`, and `.agents/` contains only agent metadata.
- [ ] **Validate Code-Signing & Update Signatures**: Confirm RS256 RSA-3072 update policy serial monotonicity and Authenticode/Gatekeeper binary compliance.
