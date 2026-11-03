# Handoff Report: Build System, Dependencies, and Notes/History Exploration

**Agent Identity**: `teamwork_preview_explorer 3`  
**Working Directory**: `D:\ChurchDAW_Source\.agents\explorer_build_history`  
**Target Path**: `D:\ChurchDAW_Source\.agents\explorer_build_history\handoff.md`  
**Date**: 2026-08-04  

---

## 1. Observation

### 1.1 Build System & Dependencies Overview

#### A. Root CMake Configuration (`D:\ChurchDAW_Source\CMakeLists.txt`)
- **CMake Version & C++ Standard**: Requires `cmake_minimum_required(VERSION 3.22)`, targets C++17 (`CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON`).
- **Product Name & Version**: `project(MinhaDAW VERSION 1.6.0)`. Executable product name set to `"ChurchDAW"` via `juce_add_gui_app(MinhaDAW PRODUCT_NAME "ChurchDAW")`.
- **Target OS Support & Deployment Targets**:
  - Windows: MSVC runtime static linking configured via `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`. MSVC flag `/utf-8` applied across targets. Extended stack reserve `/STACK:33554432` (32 MiB) set for MSVC targets to avoid stack overflow `0xC00000FD` during frame construction of organ tables.
  - macOS: Deployment target `10.15` (Catalina) for Intel (`x86_64`) and `11.0` for Apple Silicon (`arm64`). Dual architectures configured via `set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")`. Xcode attribute `CLANG_CXX_LIBRARY "libc++"`. Strictly checks API availability with `-Werror=unguarded-availability-new`. Merges plist from `Packaging/ChurchDAW-macOS-Info.plist` with Hardened Runtime enabled (`com.apple.security.device.audio-input`, `com.apple.security.cs.disable-library-validation`). Requires `BLUETOOTH_PERMISSION_ENABLED TRUE` (`NSBluetoothAlwaysUsageDescription`) to prevent macOS TCC crash when CoreMIDI queries Bluetooth devices.
  - iOS: Deployment target `14.0`. Excludes desktop licensing and desktop updater; links `Source/IOSPolicySignatureVerifier.cpp`, `Security.framework`, and `juce_opengl`.
- **Build Options**:
  - `CHURCHDAW_ENABLE_DESKTOP_LICENSING` (default `ON`): Integrates `Source/DesktopLicenseClient.cpp`, `Source/DesktopLoginPanel.cpp`, `Source/DesktopLeaseVerifier.cpp`. Windows uses `DesktopCredentialStore_windows.cpp`, `DesktopLeaseSignature_windows.cpp`, `DesktopSecureRandom_windows.cpp` and links `crypt32`, `bcrypt`. macOS uses `.mm` native implementations linking `Security.framework`.
  - `CHURCHDAW_BUILD_AUDIO_TESTS` (default `OFF`): Builds offline DSP test suite console apps (`ChurchDAWReverbPitchTests`, `ChurchDAWMasterLimiterTests`, `ChurchDAWChannelToneTests`, `ChurchDAWMidiMappingSafetyTests`, `ChurchDAWChannelRotaryTests`, `ChurchDAWPadProcessorTests`, `ChurchDAWLibraryPackPathTests`, `ChurchDAWLiveScenesTests`, `ChurchDAWSceneFileWriteProbe`, `ChurchDAWPluginScanPolicyTests`, `ChurchDAWChildProcessReapTests`, `ChurchDAWAndroidStageLayoutTests`, `ChurchDAWTimbreLoadStressTests`, `ChurchDAWHostedInstrumentMidiRoutingTests`, `ChurchDAWZipDeleteProbe`, `ChurchDAWReverbBoundsProbe`).
  - `CHURCHDAW_BUILD_LICENSE_TESTS` (default `OFF`): Compiles `ChurchDAWLicenseVerifierTests`.
  - `CHURCHDAW_BUILD_INSTALLER` (default `OFF`): Compiles native Windows UAC-free installer `ChurchDAWSetup` (`Installer/ChurchDAWSetup.cpp`) embedding `MinhaDAW` executable as an RCDATA resource.
  - `CHURCHDAW_BUILD_UPDATE_TESTS` (default `OFF`): Compiles `ChurchDAWUpdatePolicyTests` and `ChurchDAWAndroidDistributionProbe`.
  - `CHURCHDAW_LICENSE_API_BASE`: Default `"https://churchdaw.com/api/v1"`.
  - `CHURCHDAW_UPDATE_POLICY_URL`: Default `"https://churchdaw.com/app/desktop/update-policy-v1.json"`.

#### B. Projucer / Mobile Build Configuration (`D:\ChurchDAW_Source\ChurchDAW.jucer`)
- **Rationale**: JUCE framework explicitly states in `JUCE/docs/CMake API.md` (line 6) that Android targets are not currently supported by JUCE's CMake system. Therefore, desktop (Windows/macOS) is built via CMake, while mobile targets (Android/iOS) are built via Projucer (`ChurchDAW.jucer`). Both read identical code in `Source/`.
- **Target Specifications**:
  - `JUCERPROJECT id="ChDW01" name="ChurchDAW" version="1.5.9" bundleIdentifier="com.churchdaw.app"`
  - Target SDK: Minimum SDK 24 (Android 7.0), Target SDK 36 (Android 16), `androidVersionCode="14"`.
  - NDK ABI: strictly `arm64-v8a` (avoiding invalid ARM32 NEON compiler flags like `-mfpu=neon -mfloat-abi=hard` present in legacy JUCE demos).
  - Audio Backend: `JUCE_USE_ANDROID_OBOE=1`, `JUCE_USE_ANDROID_OBOE_STABILIZED_CALLBACK=0`.
  - Hosted Plugins: `CHURCHDAW_HOSTS_PLUGINS=0`, `JUCE_PLUGINHOST_VST3=0`, `JUCE_PLUGINHOST_AU=0`.
  - App Activity & Java Sources: Custom activity `com.churchdaw.app.ChurchDAWActivity` with Java sources from `Source/AndroidJava` and `JUCE/modules/juce_gui_basics/native/javaopt/app`.
  - Android Manifest Declarations:
    - `<uses-feature android:name="android.software.midi" android:required="true" />` (mandatory for Android MidiManager to yield device handles).
    - `<uses-feature android:name="android.hardware.usb.host" android:required="false" />`
    - `<uses-feature android:name="android.hardware.wifi" android:required="false" />`
    - `<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />` and `BLUETOOTH_SCAN`.
    - `<application android:allowBackup="false" android:fullBackupContent="false" ... />`

#### C. ASIO SDK Integration (`D:\ChurchDAW_Source\asiosdk`)
- **Location**: `D:\ChurchDAW_Source\asiosdk` containing Steinberg ASIO SDK 2.3 headers (`common/iasiodrv.h`, `common/asio.h`, `common/asiodrvr.h`).
- **CMake Probe**: `if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common/iasiodrv.h")`
- **Compile Definitions & Includes**:
  - Defines `JUCE_ASIO=1` for `MinhaDAW` target.
  - Adds `asiosdk/common` to target include directories.
- **Functionality**: Enables native ultra-low-latency ASIO driver support in JUCE's `juce_audio_devices` module on Windows.

#### D. JUCE Framework Dependencies (`D:\ChurchDAW_Source\JUCE`)
- **Version & Modules**: Embedded JUCE library in `JUCE/` root. Includes 15 core modules:
  `juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`, `juce_audio_processors`, `juce_audio_processors_headless`, `juce_audio_utils`, `juce_core`, `juce_cryptography`, `juce_data_structures`, `juce_dsp`, `juce_events`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`, `juce_opengl`.
- **Global Feature Toggles**: Disables web browser (`JUCE_WEB_BROWSER=0`), push notifications (`JUCE_PUSH_NOTIFICATIONS=0`), and cURL (`JUCE_USE_CURL=0`) across CMake and Projucer to minimize binary size and eliminate external runtime dependencies.

#### E. Target Configurations (Desktop vs. Android/iOS)
| Feature / Configuration | Desktop (Windows / macOS) | Mobile (Android / iOS) |
|---|---|---|
| **Build Toolchain** | CMake 3.22+ (MSVC / Apple Clang) | Projucer / Android Studio Gradle |
| **Plugin Hosting (`CHURCHDAW_HOSTS_PLUGINS`)** | Enabled (`1`) — VST3 (Win/Mac), AU (Mac) | Disabled (`0`) — relies on internal DSP/samples |
| **Licensing Client (`CHURCHDAW_DESKTOP_LICENSING`)** | Active — RSA-3072 lease verifier, login UI | Disabled — managed via app/device auth |
| **Updater Strategy** | Mandatory self-installer (`DesktopUpdater.cpp`) | HTTPS signed policy gate + sideload APK / Store |
| **Audio Backend** | ASIO, WASAPI, DirectSound, CoreAudio | Oboe (`JUCE_USE_ANDROID_OBOE=1`) |
| **Stack Allocation** | 32 MiB (`/STACK:33554432`) | Standard thread stack |

---

### 1.2 Analysis of Notes and History Files

#### A. `BUG_REVERB_LIMITES.md` (DSP Out-of-Bounds Fix)
- **Problem**: Intermittent crash (`0xC0000409` / `vector subscript out of range` assert) and inconsistent T60 decay values (e.g. 63.96s vs 10.11s) in `Source/WorshipReverb.h`.
- **Exact Location**: `WorshipReverb.h:505` `FractionalDelay::read` called by `WorshipReverb::process` line 290.
- **Diagnostics**:
  - MSVC STL Debug: `vector subscript out of range` in `<vector>` line 1941.
  - AddressSanitizer (RelWithDebInfo): `ERROR: AddressSanitizer: container-overflow READ of size 4` (index calculated to `data.size() + 8`, reading allocated heap memory beyond logical `size()`).
- **Root Cause**: Fractional delay calculation performed wrap-around in floating-point arithmetic (`float position = (float) writeIndex - delay; while (position < 0.0f) position += (float) data.size();`). When `position` was negative but within 1 ULP of buffer boundary, addition rounded to exactly `data.size()`. Casting to integer yielded out-of-bounds index `data.size()`.
- **Fix**: Replaced float wrap-around with integer decomposition (`wholeDelay + fraction`) and strictly bounded integer index arithmetic in range `[0, count - 1]`.
- **Regression Probe**: Created `Tests/Audio/ReverbBoundsProbe.cpp` (Target `ChurchDAWReverbBoundsProbe`) tested under Debug and AddressSanitizer.

#### B. `ATUALIZACAO_DESKTOP.md` (Desktop Auto-Update & Catalog Security)
- **Architecture**: Desktop mandatory update system (`Source/DesktopUpdater.cpp` and `Installer/ChurchDAWSetup.cpp`).
- **Dual Verification Model**:
  1. RS256 RSA-3072 signature over update policy JSON (shared key `churchdaw-update-2026-01` with Android; verified via `crypt32`/`bcrypt` on Windows and `Security.framework` on macOS).
  2. Operating system binary code signature on installer (Authenticode `WinVerifyTrust` on Windows; Gatekeeper `spctl --assess` on macOS).
- **Core Rules**:
  1. Never interrupt active performance (`mayEnforceNow()` checks no active voices in `Main.cpp` at startup).
  2. Anti-brick fallback (if network/policy/installer fails, app opens normally with non-blocking notification).
  3. Loop prevention lock (records executed installer SHA-256 in `%APPDATA%\ChurchDAW\update-state.json`).
- **Windows Installer (`Installer/ChurchDAWSetup.cpp`)**: Installs to user directory `%LOCALAPPDATA%\Programs\ChurchDAW` (avoiding admin/UAC prompts on church PCs). Embeds `ChurchDAW.exe` inside RCDATA PE section so Authenticode covers payload.
- **Desktop Catalog Fix**: Discovered `verifyAndroidSignedPolicyEnvelope` previously returned `false` on non-Android, causing catalog validation to fail and hiding sample download buttons on Windows/macOS. Fixed by connecting non-Android platforms to native RS256 verifier in `DesktopUpdater.cpp`, establishing a unified catalog (`/app/android/manifest.json`).

#### C. `KIMI_NOTAS.md` (Parallel Diagnostic Notes — 30/07/2026)
- **Gate Validation**: Verified `CATALOG_LEGACY_ORGAN` passed 4/4 times using `ChurchDAWTimbreLoadStressTests.exe --controls-only`.
- **State Restore Mapping**: Analyzed `setStateInformation` scan triggering and async layer load queuing in `drainPendingLayerLoads()`.
- **Rotary Speaker Architecture**: Identified two distinct DSP paths:
  1. Sampled / Transistor / Pipe -> `ChannelRotarySpeaker::process` in `TimbresProcessor.h`.
  2. Tonewheel / Worship -> native `RotaryCabinet` in `Source/ChurchOrgan/RotaryCabinet.cpp`.
  - Detailed diagnostic of `RotaryCabinet`: gain adaptation `wetCalibration_` (smoothing 3.5s), air absorption low-pass `wetLeft = leftAirState_`, `correlationCompensation` (floor 0.78), and `cabinetFloor` (0.14+).
- **macOS Transpose Bug**: Verified fixes for unwanted transpose on Channels 1/2 (unbound Launchkey Knob 8, filtered unsafe stored targets via `isSafeStoredTarget`, established per-layer `noteTranspose` ledger).

#### D. `PROJECT.md`, `analysis.md`, `handoff.md`, `TEST_READY.md` (History & Instrumentation)
- `PROJECT.md`: History of Casio patch extraction (`.tn7` tone patch, `.tu7` wave bank 1.966 GB), SysEx wipe commands, and USB MIDI bulk transfer scripts.
- `analysis.md` & `handoff.md`: Native SFZ/JSON specification standard mined for 28 KEEP instruments (Ultimate Pianos 2 catalog integration), Ableton pitch formula $C3 = 60$, continuous key range coverage `[0..127]`, and `ValidateTimbrePack.py` compliance.
- `TEST_READY.md`: E2E Milestone 4 verification suite results for `Pad Worship 2.0 ED Bento` across 5 tiers (100% pass rate across 21 E2E tests, 18 validator tests, and white-box stress scenarios).

#### E. `Packaging/AndroidDistribution.md` (Android Sideload & Distribution)
- Outlines sideload update policy (schema v2, RSA-3072 key `churchdaw-update-2026-01`, serial monotonicity, HTTPS restrictions).
- Update gate executed before login and audio engine init.
- Content library split into 30 downloadable segments (`church-live-v1-segment-01.zip` .. `30.zip`) totaling ~13.4 GB.

#### F. `Design/ChurchOrganEngine/ARCHITECTURE.md` (Church Organ Architecture Contract)
- Defines native clean-room 4-family organ engine (`tonewheel`, `transistor`, `pipe`, `worshipHybrid`).
- Strict real-time contract (zero allocation, zero locks, zero I/O on audio thread).
- Modular design separating `ChurchOrganCore` from `ChurchOrganHostAdapter` and `OrganViewModel`.
- Introduces `Worship Intensity` macro dynamics controlling multi-destination spectral opening, width, and gain compensation.

---

## 2. Logic Chain

1. **Observation**: Root `CMakeLists.txt` defines CMake targets for Windows/macOS/iOS, while `ChurchDAW.jucer` defines Android Studio exports.
   - **Inference**: ChurchDAW uses a hybrid build system strategy. Because JUCE's CMake module explicitly does not support Android targets (`JUCE/docs/CMake API.md`), CMake handles desktop builds (Windows MSVC and macOS Apple Clang), while Projucer generates the Android Studio project.
   - **Verification**: Both build systems point to identical files in `Source/` and `Source/ChurchOrgan/`, ensuring zero code duplication across targets.

2. **Observation**: Compile options show `CHURCHDAW_HOSTS_PLUGINS=1` on desktop and `0` on mobile (`ANDROID` or `IOS`).
   - **Inference**: VST3 and AU plugin hosting (e.g. Kontakt, Massive) is restricted to desktop platforms due to mobile OS memory limits and lack of external plugin format standards on mobile. Mobile builds rely exclusively on native internal sample streaming and native synth/organ engines.

3. **Observation**: `BUG_REVERB_LIMITES.md` documents `WorshipReverb.h` out-of-bounds indexing resulting from float wrap-around ULP rounding.
   - **Inference**: Real-time DSP routines must avoid floating-point modulo/wrap logic when indexing memory buffers. Integer decomposition guarantees index bounds within `[0, count - 1]`. The creation of dedicated console test apps (`ChurchDAWReverbBoundsProbe`) integrated into CTest prevents DSP regression across release builds.

4. **Observation**: `ATUALIZACAO_DESKTOP.md` details how desktop update verification was aligned with Android's RS256 RSA-3072 signature envelope.
   - **Inference**: Unifying the update policy signature algorithm and catalog format (`/app/android/manifest.json`) across Android and Desktop enabled cross-platform security and eliminated redundant catalog maintenance, fixing a critical bug where desktop clients failed catalog verification.

5. **Observation**: Notes in `KIMI_NOTAS.md` demonstrate two distinct rotary speaker implementations (`ChannelRotarySpeaker` vs `RotaryCabinet`).
   - **Inference**: Sampled/transistor timbres process rotary modulation via channel-level DSP (`ChannelRotarySpeaker`), whereas native tonewheel/worship organ timbres use an internal physical rotary model (`RotaryCabinet`). Parameter routing must cleanly distinguish between these two paths to avoid double-processing or volume drops.

---

## 3. Caveats

- **External Hardware / Signing Keys**: Code-signing certificates (Windows PFX, macOS Developer ID, HostGator FTPS credentials, RSA private key `churchdaw-update-2026-01`) are kept outside the repository (`C:\Users\Administrator\ChurchDAW-Secrets\`). Local builds execute in un-signed or test-cert mode.
- **Android CMake Limitation**: Android targets must continue to be built via Projucer / Gradle until JUCE upstream adds native CMake support for Android.
- **ASIO SDK License**: Steinberg ASIO SDK located in `asiosdk/` requires adherence to Steinberg licensing guidelines when distributing Windows binaries with `JUCE_ASIO=1`.

---

## 4. Conclusion

ChurchDAW possesses a highly structured, cross-platform architecture:
1. **Build System**: Uses CMake 3.22 for Desktop (Windows/macOS) and iOS, and Projucer (`ChurchDAW.jucer`) for Android. Desktop builds enable VST3/AU plugin hosting, RSA-3072 desktop licensing, and self-installing updates; mobile builds run in native-only mode using Oboe audio drivers.
2. **Dependencies**: Self-contained embedded JUCE library (15 core modules), Steinberg ASIO SDK 2.3 for ultra-low latency on Windows, and zero external runtime requirements (cURL, web browser, and push notifications are disabled).
3. **Notes & Architectural Evolution History**:
   - **DSP Fixes**: Resolved float ULP rounding out-of-bounds bug in `WorshipReverb.h:505`, verified via AddressSanitizer and `ChurchDAWReverbBoundsProbe`.
   - **Desktop Security & Catalog Fix**: Developed dual RS256 + Authenticode/Gatekeeper update mechanism (`DesktopUpdater.cpp`), resolved desktop catalog verification blockage, unified catalog distribution across platforms.
   - **Organ Engine Integration**: Designed clean-room 4-family native organ engine (`Source/ChurchOrgan/`) operating under strict real-time audio thread constraints.
   - **Test & Validation Hardening**: Created 5-tier test matrix covering SFZ/JSON specifications, sample parsing (Ableton C3=60 standard), E2E catalog integration, and white-box adversarial stress testing.

---

## 5. Verification Method

To independently verify the build configuration and notes findings:

1. **Verify Root CMake & Projucer Build Targets**:
   - Inspect `CMakeLists.txt` lines 19, 49, 489, 677 to confirm version `1.6.0`, JUCE module inclusion, product name `"ChurchDAW"`, and `JUCE_ASIO=1` flag activation when `asiosdk` is present.
   - Inspect `ChurchDAW.jucer` lines 16-21, 131-151 to confirm version `1.5.9`, `arm64-v8a` target architecture, `JUCE_USE_ANDROID_OBOE=1`, and `android.software.midi` feature declaration.

2. **Verify DSP Reverb Bounds Probe Build Target**:
   - Run command:
     ```powershell
     cmake -S D:\ChurchDAW_Source -B D:\ChurchDAW_Source\build -DCHURCHDAW_BUILD_AUDIO_TESTS=ON
     cmake --build D:\ChurchDAW_Source\build --config Debug --target ChurchDAWReverbBoundsProbe
     ```
   - Execute generated probe to confirm 100% pass rate across fractional delay bounds tests.

3. **Verify Notes Files Consistency**:
   - Check file existence and paths:
     - `D:\ChurchDAW_Source\BUG_REVERB_LIMITES.md`
     - `D:\ChurchDAW_Source\ATUALIZACAO_DESKTOP.md`
     - `D:\ChurchDAW_Source\KIMI_NOTAS.md`
     - `D:\ChurchDAW_Source\PROJECT.md`
     - `D:\ChurchDAW_Source\analysis.md`
     - `D:\ChurchDAW_Source\TEST_READY.md`
