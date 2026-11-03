# ChurchDAW Technical Audit: Concurrency, Lock-Free & Build Systems (R3 & R4)

**Specialist**: Explorer 3 (Concurrency, Lock-Free & Build Systems Specialist)  
**Date**: 2026-08-14  
**Target Codebase**: ChurchDAW (C++17 / JUCE 7)  
**Requirements Audited**: R3 (Concurrency & Lock-Free Inter-Thread Communication) & R4 (Cross-Platform Architecture & Build Systems)

---

## Executive Summary

This report delivers an exhaustive technical audit of the concurrency primitives, lock-free communication pipelines, thread synchronization boundaries, cross-platform build configurations, audio backends, and external licensing constraints across the ChurchDAW codebase.

The architecture demonstrates a high level of real-time discipline, employing lock-free atomic bitmasks, generation-based seqlocks, double-buffered sliding sample windows, and multi-reader atomic level meters. However, critical and medium vulnerabilities were identified:
1. **Critical Audio Thread Blocking (R3)**: `PadProcessor::processBlock` (`Source/PadProcessor.h:384`) acquires a `juce::CriticalSection` lock (`voiceLock`) which is also acquired by the background `PadLoader` disk streaming thread (`Source/PadProcessor.h:1589`), presenting a severe priority inversion / audio dropout hazard.
2. **Cache-Line Bouncing & False Sharing (R3)**: Unpadded sequential atomic arrays (`OrganParameterStore::values_` with 66 atomics, `TimbresProcessor::layerLevels*` with 14 atomics per array) reside on shared 64-byte L1 cache lines, causing cache invalidations across CPU cores during concurrent UI edits and audio rendering.
3. **Build Version & Configuration Divergence (R4)**: Dual build configuration drift exists between `CMakeLists.txt` (Version `1.6.15`, Version Code `10622`) and `ChurchDAW.jucer` (Version `1.6.10`, Version Code `17`), along with Android XML attribute discrepancies (`androidCpp11="1"` vs `C++17`).

---

## 1. Concurrency & Lock-Free Inter-Thread Communication (R3)

### 1.1 UI (Message Thread) <-> Audio Thread Parameter Transfer & Event Dispatching

ChurchDAW bridges the message thread (UI) and the real-time audio thread through structured parameter sinks and atomic communication channels without allocating heap objects during callback execution.

#### A. Church Organ Parameter Bridge (`ChurchOrganParameterSink`)
- **Location**: `Source/ChurchOrgan/UI/ChurchOrganViewModel.h:124-144`, `Source/ChurchOrgan/OrganParameters.h:149-178`, `Source/TimbresProcessor.h:14192-14380`.
- **Mechanism**: The UI editor (`ChurchOrganEditor`) communicates solely with an abstract `ChurchOrganParameterSink`. In production, this sink is implemented by `TimbresChurchOrganUiSink` inside `TimbresProcessor.h`.
- **Parameter Snapshotting**:
  - `OrganParameterStore` holds 66 discrete organ parameters (`OrganParameterId::count = 66`) in `std::array<std::atomic<std::uint32_t>, 66> values_`.
  - When the UI modifies a parameter (e.g., `setDrawbar`, `setParameterPlain`), it updates the atomic uint32 bits via `exchange(bits, std::memory_order_relaxed)` and increments `revision_` via `revision_.fetch_add(1, std::memory_order_release)` (`Source/ChurchOrgan/OrganParameters.cpp:368-371`).
  - The audio thread retrieves state in `ChurchOrganLayerProcessor::processBlock` (`Source/ChurchOrgan/ChurchOrganLayerProcessor.cpp:366-378`) by invoking `applyPendingConfiguration()`. It checks if `parameters_.revision()` has changed, and if so, calls `parameters_.snapshot()`.
  - **Audit Diagnosis**:
    - **Tear-Free Guarantee**: Individual 32-bit floats stored as bit patterns are guaranteed lock-free (`static_assert(std::atomic<std::uint32_t>::is_always_lock_free)`) and cannot suffer word tearing on any supported architecture (x86_64, arm64).
    - **Non-Transactional Snapshot**: `OrganParameterStore::snapshot()` iterates across all 66 values using `load(std::memory_order_relaxed)`. While every individual parameter is atomic, the overall snapshot across 66 parameters is not transactional (i.e. no seqlock retry loop inside `snapshot()`). In worship scenarios, a simultaneous preset switch from the UI might be read across one audio block as a partially updated set of drawbars for a single frame before stabilizing. This is sonically benign due to downstream parameter smoothing, but is formally a relaxed multi-word publication.

#### B. Layer Identity & Loading Mask (`loadedLayerMask`)
- **Location**: `Source/TimbresProcessor.h:2751, 2766, 7812, 7814, 13996`.
- **Mechanism**: Channel availability across the 14 layers (`numLayers = 14`) is represented by a single 32-bit atomic bitmask: `std::atomic<juce::uint32> loadedLayerMask { 0 }`.
- **Thread Synchronization**:
  - Background loading workers and UI thread set/clear layer bits using `loadedLayerMask.fetch_or(bit, std::memory_order_release)` and `loadedLayerMask.fetch_and(~bit, std::memory_order_release)`.
  - The real-time audio thread checks layer availability in `hasAnyLoadedSolo()` and `isLayerLoaded()` using `loadedLayerMask.load(std::memory_order_acquire)`.
  - **Audit Verdict**: **PASS**. Properly paired Release-Acquire semantics ensure that all pre-allocated buffer data, sample structures, and DSP state initialized before the bit publication are fully visible to the audio thread before processing.

---

### 1.2 Lock-Free SPSC Queues, Circular Buffers & Window Streaming

#### A. Sliding-Window Sample Streaming Engine (`SampleStreamer`)
- **Location**: `Source/SampleStream.h:52-450`.
- **Mechanism**: ChurchDAW implements a custom lock-free double-buffered sliding window streaming subsystem (`SampleStreamer`) to stream large multi-gigabyte piano samples from disk without loading entire wave files into RAM.
- **Concurrency Architecture**:
  1. **Seqlock Generation Counter**: Each active voice stream (`VoiceStream`) maintains `std::atomic<int> generation { 0 }`. On `restart()` or `stop()`, the audio thread increments `generation` to an odd number with `fetch_add(1, std::memory_order_acq_rel)` to signal a write-in-progress, updates pointers via `std::atomic_store_explicit`, and increments to an even number with `fetch_add(1, std::memory_order_release)`.
  2. **Double Window Access Ownership**: Two audio buffers per voice (`juce::AudioBuffer<float> buf[2]`, `windowSamples = 32768`) are managed via `std::atomic<int> windowAccess[2]`:
     - `-1`: Background worker is actively decoding/writing from disk.
     - `0`: Free / idle.
     - `+1`: Real-time audio thread is actively reading.
  3. **Audio-Thread Window Acquisition (`VoiceStream::acquire`)**:
     - The audio callback attempts a non-blocking `windowAccess[i].compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)`.
     - If the background worker is writing (`expected == -1`), the audio thread immediately bypasses that window without spinning or locking.
     - If valid data exists for the current playback span, the read pointer is provided directly.
  4. **Dynamic Prefetching (`VoiceStream::prefetch`)**:
     - Initiates the next continuous segment by executing `wantStart.compare_exchange_strong(expected, nextStart, std::memory_order_acq_rel, std::memory_order_relaxed)`.
- **Audit Verdict**: **PASS**. Fully lock-free, zero-allocation on the audio thread, and avoids race conditions via atomic state transitions and seqlock protection.

#### B. Church Organ Internal Event Dispatch Queue
- **Location**: `Source/ChurchOrgan/ChurchOrganLayerProcessor.h:466-470`, `Source/ChurchOrgan/ChurchOrganLayerProcessor.cpp:1156-1250`.
- **Mechanism**: Within `ChurchOrganLayerProcessor`, incoming MIDI events are converted to fixed-size `AdapterEvent` structures and queued in static arrays:
  - `std::array<AdapterEvent, maximumAdapterEvents> events_ {};` (`maximumAdapterEvents = 1024`).
  - `std::array<GainEvent, maximumAdapterEvents> gainEvents_ {};`
- **Audit Verdict**: **PASS**. Fixed pre-allocated storage eliminates dynamic allocation during event gathering.

---

### 1.3 `std::atomic` Usage Correctness, Memory Ordering & Race Condition Audit

| Component / Variable | File & Line | Memory Order Used | Thread Access Pattern | Correctness Assessment |
|---|---|---|---|---|
| `loadedLayerMask` | `Source/TimbresProcessor.h:7812, 2751` | `release` (write) / `acquire` (read) | UI/Worker -> Audio Thread | **Correct**: Established release-acquire barrier for layer publication. |
| `generation` (SampleStreamer) | `Source/SampleStream.h:229, 235, 283` | `acq_rel` (odd) / `release` (even) / `acquire` (read) | Audio Thread <-> Worker Thread | **Correct**: Standard lock-free seqlock implementation. |
| `windowAccess[2]` | `Source/SampleStream.h:313, 357` | `acquire` (CAS) / `release` (store) | Audio Thread <-> Worker Thread | **Correct**: Prevents simultaneous buffer read/write races. |
| `audioInputAuthorised` | `Source/MainComponent.cpp:1589, 4504` | `release` (write) / `acquire` (read) | UI Thread -> Audio Thread | **Correct**: Gating audio input hardware stream. |
| `panicRequested` | `Source/PadSynthProcessor.h:260, 363` | `acq_rel` (exchange) / `release` (store) | UI Thread -> Audio Thread | **Correct**: One-shot event consumption. |
| `mascaraDeNotasMapeadas` | `Source/MainComponent.h:360`, `cpp:6289` | `relaxed` (load/store) | UI Thread <-> MIDI Input Thread | **Safe**: Fast-path bypass is relaxed; mapped path takes `mapLock` for synchronization. |
| `OrganParameterStore::values_` | `Source/ChurchOrgan/OrganParameters.cpp:369, 405` | `relaxed` (load/store/exchange) | UI Thread -> Audio Thread | **Minor Gap**: Tear-free 32-bit floats, but lacks transactional multi-variable snapshot. |
| `MasterProcessor::levels` | `Source/MasterProcessor.h:92` | `seq_cst` (default exchange) | Audio Thread (write) / UI Thread (reset) | **Correct**: Safe per-meter independent exchange. |
| `voiceLock` in `PadProcessor` | `Source/PadProcessor.h:384, 1589` | Mutex (`ScopedLock`) | Audio Thread <-> Background Loader Thread | **CRITICAL BUG**: Audio thread blocks on background loader mutex. |

---

### 1.4 Critical Concurrency Findings

#### Finding C1: Audio Thread Blocking Lock in `PadProcessor` (Severity: HIGH)
- **Exact Location**: `Source/PadProcessor.h:384` & `Source/PadProcessor.h:1589`.
- **Code Observation**:
  ```cpp
  // Source/PadProcessor.h:384 (inside PadProcessor::processBlock - Audio Thread)
  void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
  {
      ...
      const juce::ScopedLock sl (voiceLock); // <-- ACQUIRES MUTEX ON AUDIO THREAD
      ...
  }

  // Source/PadProcessor.h:1589 (inside PadProcessor::drainReleasePool - PadLoader Worker Thread)
  void drainReleasePool()
  {
      decltype (releasePool) local;
      {
          const juce::ScopedLock sl (voiceLock); // <-- WORKER THREAD ACQUIRES SAME MUTEX
          for (int i = 0; i < releaseCount; ++i)
              local[(size_t) i] = std::move (releasePool[(size_t) i]);
          releaseCount = 0;
      }
      for (auto& buffer : local)
          buffer.reset(); // Deallocates heap memory (~100 MB)
      trimCacheToLimit();
  }
  ```
- **Logic Chain**:
  1. `PadProcessor` inherits from `juce::Thread` (`PadLoader`), executing asynchronous disk loads and release pool draining.
  2. `PadProcessor::processBlock` executes on the high-priority OS audio thread and acquires `voiceLock` (`juce::CriticalSection`) via `ScopedLock sl(voiceLock)` at line 384.
  3. `PadProcessor::drainReleasePool` executes on the low-priority background `PadLoader` thread and acquires `voiceLock` at line 1589.
  4. If the OS scheduler preempts the `PadLoader` thread while it holds `voiceLock`, or if priority inversion occurs, the real-time audio thread will block waiting for `voiceLock`, resulting in audio buffer under-runs (dropouts/clicks/xruns) in live worship performance.
- **Remediation Proposal**: Replace `voiceLock` with lock-free atomic pointer exchange (e.g. SPSC pointer queue or `std::atomic<juce::AudioBuffer<float>*>`) for voice buffer transfers, completely removing `voiceLock` from `processBlock`.

---

### 1.5 Cache-Line Bouncing & False Sharing Analysis

#### Finding C2: False Sharing in Packed Atomic Arrays (Severity: MEDIUM)
- **Exact Locations**:
  - `Source/ChurchOrgan/OrganParameters.h:175`: `std::array<std::atomic<std::uint32_t>, organParameterCount> values_` (66 atomics = 264 contiguous bytes).
  - `Source/TimbresProcessor.h:13922-13925`: `std::atomic<float> layerLevelsL[14]`, `std::atomic<float> layerLevelsR[14]`, `std::atomic<float> layerLevelShown[14]`, `std::atomic<bool> layerClipped[14]`.
- **Mechanism**:
  - On x86_64 and ARM64 processors, L1 cache lines are 64 bytes wide.
  - A 4-byte atomic (`std::atomic<uint32_t>` or `std::atomic<float>`) shares its 64-byte cache line with up to 15 adjacent atomic variables.
  - In `OrganParameters`, when the UI thread continuously writes to drawbar 1 (modifying byte offset `0..3`), the CPU core's MESI/MOESI cache coherence protocol invalidates the entire 64-byte cache line covering parameters 0 through 15 across all CPU cores.
  - Concurrently, when the audio rendering thread reads drawbars 2–8 during `snapshot()`, it incurs L1 cache misses and pipeline stalls as cache lines bounce between the UI thread core and audio thread core.
  - In `TimbresProcessor`, the audio thread stores peak levels into `layerLevelsL` and `layerLevelsR` every block (approx. every 1.3ms–11.6ms), while the UI message thread polls and resets them at 30 Hz.
- **Remediation Proposal**:
  - For parameter groups that undergo rapid UI interaction vs. audio rendering, pad cache-sensitive structures to 64 bytes (`alignas(64)` or `alignas(std::hardware_destructive_interference_size)`), or group UI-frequent parameters into separate cache lines from static configuration parameters.

---

### 1.6 Parameter Smoothing & Non-Blocking Snapshots

ChurchDAW consistently utilizes `juce::SmoothedValue` with linear and exponential ramps to eliminate zipper noise when parameters change:
1. **Master Bus**: `MasterProcessor.h` smooths master volume gain (`gain`, 50ms), auto EQ blend (`autoEqMix`, 10ms), and worship punch makeup (`punchMakeup`, 5ms).
2. **Channel DSP**: `TimbresProcessor.h` smooths channel compressor makeup (`compSuave`, 45ms), reverb return gain (`reverbReturnSmooth`), and delay mix (`delayAmtSuave`, 50ms).
3. **Worship Reverb**: `WorshipReverb.h` smooths 10 continuous DSP parameters across predelay, size, diffusion, pitch shimmer levels, ducking, and stereo width.
4. **Pad Continuous**: `PadProcessor.h` smooths pad volume (`ganhoSuave`, 50ms).

All smoothers are reset during `prepareToPlay` / `prepare` and evaluated on a per-sample basis without locking or dynamic allocations.

---

## 2. Cross-Platform Architecture & Build Systems (R4)

### 2.1 Dual Build System Strategy & Feature Parity

ChurchDAW maintains a dual build architecture dictated by platform toolchain constraints:
- **Root CMake (`CMakeLists.txt`)**: Targets macOS Desktop (Universal `arm64` + `x86_64`), Windows Desktop (MSVC `x86_64`), Linux, and iOS.
- **Projucer (`ChurchDAW.jucer`)**: Targets Android (`Builds/Android`), exporting Gradle projects configured for Android Studio and NDK `arm64-v8a`. (JUCE CMake API does not natively support Android export).

#### Build System Comparison Matrix

| Feature / Dimension | Desktop CMake (`CMakeLists.txt`) | Android Projucer (`ChurchDAW.jucer`) | iOS CMake / Projucer |
|---|---|---|---|
| **Build Toolchain** | CMake 3.22+ / Ninja / MSVC / Xcode | Projucer / Gradle 8.x / Android NDK | CMake 3.22+ / Xcode |
| **Product Version** | `1.6.15` (`project(MinhaDAW VERSION 1.6.15)`) | `1.6.10` (`version="1.6.10"`) | `1.6.15` / `14.0` Deployment Target |
| **Version Code** | `10622` (`CHURCHDAW_VERSION_CODE_FLOOR`) | `17` (`androidVersionCode="17"`) | Apple CFBundleVersion |
| **C++ Standard** | `CMAKE_CXX_STANDARD 17` (Required) | `androidCpp11="1"` (XML tag) / NDK C++17 | `CMAKE_CXX_STANDARD 17` |
| **Plugin Hosting** | `CHURCHDAW_HOSTS_PLUGINS=1` (VST3, AU) | `CHURCHDAW_HOSTS_PLUGINS=0` | `CHURCHDAW_HOSTS_PLUGINS=0` |
| **Audio Backend** | Steinberg ASIO 2.3, WASAPI, CoreAudio | Oboe (`JUCE_USE_ANDROID_OBOE=1`) | CoreAudio / Oboe |
| **Stack Allocation** | MSVC `/STACK:33554432` (32 MiB) | Default Android Pthread stack | Default Mach-O thread stack |
| **Optimization** | `juce_recommended_config_flags` (`/O2`, `-O3`) | `optimisation="5"` (`-O3`), `-funroll-loops` | `-O3` / Xcode Release |
| **Licensing Client** | Active (RSA-3072, `crypt32`/`Security`) | Active (`AndroidLicenseClient.h`) | Active (`IOSPolicySignatureVerifier.cpp`) |
| **Auto-Updater** | Native Silent Installer (`DesktopUpdater.cpp`) | HTTPS RS256 Policy (`AndroidDistribution`) | App Store / Sideload Manifest |

---

### 2.2 JUCE 7 Modules & C++17 Conformance

#### A. JUCE 7 Module Set
Both `CMakeLists.txt` and `ChurchDAW.jucer` link 15 JUCE 7 modules embedded in `JUCE/modules`:
1. `juce_audio_basics`
2. `juce_audio_devices`
3. `juce_audio_formats`
4. `juce_audio_processors`
5. `juce_audio_processors_headless`
6. `juce_audio_utils`
7. `juce_core`
8. `juce_cryptography`
9. `juce_data_structures`
10. `juce_dsp`
11. `juce_events`
12. `juce_graphics`
13. `juce_gui_basics`
14. `juce_gui_extra`
15. `juce_opengl`

#### B. Globally Disabled Bloat Modules
To minimize binary size, eliminate web engine crashes, and avoid external runtime dependencies, the following definitions are globally disabled across all build targets:
- `JUCE_WEB_BROWSER=0`
- `JUCE_PUSH_NOTIFICATIONS=0`
- `JUCE_USE_CURL=0`

#### C. C++17 Conformance
- Strict standard conformance is enforced via `set(CMAKE_CXX_STANDARD 17)` and `set(CMAKE_CXX_STANDARD_REQUIRED ON)`.
- Codebase utilizes C++17 features including `std::atomic<T>::is_always_lock_free`, `if constexpr`, `std::clamp` / `juce::jlimit`, structured bindings, and nested namespaces (`namespace churchdaw::organ`).

---

### 2.3 Compiler & Optimization Flags Audit

#### A. Microsoft Visual C++ (MSVC 2022 - Windows Desktop)
- `/utf-8`: Ensures proper UTF-8 source and execution character sets, preventing encoding corruption on Brazilian Portuguese string literals.
- `/STACK:33554432` (32 MiB): Linker flag expanding the PE image stack reserve from 1 MiB to 32 MiB. This prevents runtime stack overflow (`0xC0000409` / `0xC00000FD`) during the construction of large organ matrix tables and additive synthesis coefficient arrays in `PipeOrganEngine` and `WorshipHybridEngine`.
- Static Runtime: `CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"` statically bundles the MSVC CRT (`libcmt.lib` / `libcmtd.lib`), allowing standalone execution on church PCs without installing the Visual C++ Redistributable package.
- `/bigobj`: Applied to heavy test suites (`ChurchDAWTimbreLoadStressTests`) to accommodate large symbol tables.

#### B. Apple Clang (macOS & iOS)
- Universal Binaries: `set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")` compiles universal Mach-O binaries containing native slices for both Apple Silicon (M1/M2/M3/M4) and Intel x86_64.
- API Availability Guard: `-Werror=unguarded-availability-new` turns unguarded uses of macOS APIs newer than the `10.15` deployment target into hard compile-time errors.
- Bluetooth TCC Permissions: `BLUETOOTH_PERMISSION_ENABLED TRUE` with `NSBluetoothAlwaysUsageDescription` in `Packaging/ChurchDAW-macOS-Info.plist` prevents macOS TCC privacy crashes when CoreMIDI enumerates nearby Bluetooth LE MIDI devices.

#### C. Android Clang (Android NDK)
- `-funroll-loops`: Loops in DSP rendering and cubic interpolation are unrolled for ARM NEON execution.
- Optimization Level `5` (`-O3`): Maximum performance optimization.
- **Architectural Safety**: JUCE's legacy 32-bit ARM flags (`-mfpu=neon -mfloat-abi=hard`) are explicitly excluded, targeting pure 64-bit `arm64-v8a`.

---

### 2.4 Audio Backend Drivers & Platform Compatibility

```
                      ┌─────────────────────────────────────────┐
                      │            Audio Device Manager         │
                      │         (juce::AudioDeviceManager)      │
                      └────────────────────┬────────────────────┘
                                           │
         ┌──────────────────┬──────────────┴──────┬──────────────────┐
         │                  │                     │                  │
         ▼                  ▼                     ▼                  ▼
   [ Windows ]          [ macOS ]             [ iOS ]           [ Android ]
   - Steinberg ASIO     - CoreAudio           - CoreAudio       - Oboe (AAudio/OpenSL)
     (iasiodrv.h,         (AudioUnit,           (Deployment       (JUCE_USE_ANDROID_OBOE=1,
      JUCE_ASIO=1)         AudioToolbox)         Target 14.0)      arm64-v8a sub-10ms)
   - WASAPI             - CoreMIDI            - CoreMIDI        - Android MidiManager
   - DirectSound        - Bluetooth TCC       - Bluetooth LE      (android.software.midi)
```

1. **Windows (ASIO / WASAPI)**:
   - Steinberg ASIO SDK 2.3 is detected via `if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common/iasiodrv.h")`, activating `JUCE_ASIO=1` for ultra-low latency (<5ms) professional audio interface communication.
   - WASAPI fallback is available when ASIO hardware is absent.
2. **macOS (CoreAudio & CoreMIDI)**:
   - Uses native CoreAudio and CoreMIDI subsystems, with AudioUnit plugin hosting (`JUCE_PLUGINHOST_AU=1`) and VST3 hosting (`JUCE_PLUGINHOST_VST3=1`).
3. **Android (Google Oboe)**:
   - `JUCE_USE_ANDROID_OBOE=1` routes audio through Google Oboe, dynamically binding to AAudio on Android 8.1+ (API 27+) and OpenSL ES on older devices.
   - Declares mandatory Android manifest hardware features (`<uses-feature android:name="android.software.midi" android:required="true" />`).
4. **Linux (ALSA / JACK)**:
   - Compatible with JUCE standard ALSA and JACK backends.

---

### 2.5 External Dependencies, Asset Bundles & Licensing Considerations

1. **JUCE Framework 7.x**: Embedded under `JUCE/`. Licensed under GPLv3 / JUCE Commercial License. Global flags (`JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`) eliminate third-party browser runtime dependencies.
2. **Steinberg ASIO SDK 2.3**: Located in `asiosdk/`. Distribution of binaries linking ASIO requires compliance with the Steinberg ASIO SDK Agreement. Detection is cleanly isolated in CMake.
3. **Legacy Steinberg VST2**: VST2 hosting is disabled by default (`CHURCHDAW_ENABLE_VST2_HOSTING=OFF`) in compliance with Steinberg's licensing discontinuation (October 2018).
4. **Cryptography & Licensing Security**:
   - RS256 RSA-3072 digital signatures for update policies and lease tokens (`churchdaw-update-2026-01`).
   - Windows uses `crypt32`, `bcrypt`, and `wintrust`.
   - macOS / iOS use Apple `Security.framework`.
   - Sample packs are encrypted and integrity-verified using ChaCha20 and SHA-256 (`juce_cryptography`).
5. **Asset Bundling**:
   - 30 signed split zip libraries (~13.4 GB total) mapped via `catalog.json` and verified with serial monotonicity to prevent downgrade attacks.
   - Vector graphics fallback (`drawChurchDAWMark` in `NordLookAndFeel.h`) ensures UI rendering reliability even when mobile GPU texture uploads fail.

---

## 3. Findings Summary & Risk Classification

| ID | Category | Severity | File & Location | Description | Recommended Remediation |
|---|---|---|---|---|---|
| **F-01** | Real-Time / Concurrency | **HIGH** | `Source/PadProcessor.h:384, 1589` | `PadProcessor::processBlock` acquires `voiceLock` (`juce::CriticalSection`) shared with `PadLoader` worker thread, creating audio thread blocking risk. | Replace `voiceLock` with lock-free atomic pointer exchange or SPSC queue for voice handoff. |
| **F-02** | Concurrency / Cache | **MEDIUM** | `Source/ChurchOrgan/OrganParameters.h:175`, `Source/TimbresProcessor.h:13922` | False sharing / cache-line bouncing on packed atomic arrays (`OrganParameters::values_`, `layerLevelsL/R`) during concurrent UI and audio access. | Add `alignas(64)` padding between frequently written atomic fields. |
| **F-03** | Build Systems | **MEDIUM** | `CMakeLists.txt:83` vs `ChurchDAW.jucer:17` | Version divergence between Desktop CMake (`1.6.15` / `10622`) and Android Projucer (`1.6.10` / `17`). | Reconcile product version and version code in `ChurchDAW.jucer` to match `CMakeLists.txt`. |
| **F-04** | Build Systems | **LOW** | `ChurchDAW.jucer:135` | XML attribute specifies `androidCpp11="1"` while codebase uses C++17 standard features. | Update Projucer export setting / Gradle configuration to explicitly declare `cpp17`. |
| **F-05** | Licensing | **LOW** | `asiosdk/` | Steinberg ASIO SDK 2.3 inclusion requires active Steinberg Developer License agreement for distribution. | Verify institutional Steinberg ASIO license agreement on file. |

---

## 4. Architectural Strengths & Verification Highlights

1. **Strict Zero-Allocation Audio Path in Core Modules**: `ChurchOrganCore`, `ChurchOrganLayerProcessor`, `WorshipReverb`, `StereoLookaheadLimiter`, and `MasterProcessor` allocate all buffers during `prepareToPlay` / `prepare` and perform zero dynamic heap allocations in `processBlock`.
2. **Resilient Lock-Free Sliding Streamer**: `SampleStreamer` effectively solves mobile and desktop RAM saturation by implementing double-buffered 32k sliding windows with generation seqlocks.
3. **Defensive Denormal & Non-Finite Guards**: `MasterProcessor` and `TimbresProcessor` implement denormal suppression (`ScopedNoDenormals`) and sanitize NaN/infinite values, protecting IIR biquad filter state from permanent corruption.
4. **Hardened Multi-Reader Level Meters**: `MasterProcessor::readAndResetLevel` provides dedicated per-reader level boxes to eliminate display ballistic distortion across docked/split UI components.
