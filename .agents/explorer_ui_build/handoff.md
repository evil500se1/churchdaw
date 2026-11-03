# Handoff Report — explorer_ui_build

**Agent**: `explorer_ui_build`  
**Date**: 2026-08-05  
**Working Directory**: `D:\ChurchDAW_Source\.agents\explorer_ui_build`  
**Target Project**: `D:\ChurchDAW_Source`  
**Handoff Type**: Hard Handoff (Task Complete)

---

## 1. Observation

Direct observations from examining `D:\ChurchDAW_Source` codebase, UI source files, and build configuration files:

1. **Root UI Component Hierarchy (`Source/MainComponent.h` & `Source/MainComponent.cpp`)**:
   - `MainComponent` inherits from `juce::Component` (`MainComponent.h:42`) and owns `juce::AudioProcessorGraph graph` (`MainComponent.h:154`).
   - Main layout areas defined in `MainComponent.h`:
     - `headerHeight = 64`, `mobileHeaderHeight = 38` (`MainComponent.h:84-85`).
     - `programBandHeight = 112`, `mobileProgramBandHeight = 44` (`MainComponent.h:88-89`).
     - `slotsRowHeight = 168`, `masterStripWidth = 150` (`MainComponent.h:92,94`).
   - Header controls (`MainComponent.h:178-185`): `audioSettingsButton`, `scanButton`, `saveButton`, `padButton`, `librariesButton`, `instrumentsButton`, `liveButton`, `mapButton`, `clearMapButton`.
   - Graph slots (`MainComponent.h:81-82, 195`): `numSlots = 4` (Slot 0 = Instrument `TimbresProcessor`, Slots 1..3 = FX Slots).
   - Embedded plugin viewport (`MainComponent.h:212`): `juce::Viewport dockViewport;`.

2. **Mobile GPU Acceleration & Layout Adaptation (`Source/MainComponent.cpp`)**:
   - Lines 263-278: On Android/iOS, GPU rendering is attached via `glContext.attachTo(*this)`.
   - Lines 2006-2009: `mobilePortraitLayout = safeContentWidth < safeContentHeight;`
   - Lines 2145-2210: Responsive two-row header for Mobile Portrait layout with touch action bar items (`actions` array).

3. **Church Live Native Mixer (`Source/TimbresProcessor.h`)**:
   - Lines 207: `static constexpr int numLayers = 14;` (14-channel sampler engine).
   - Lines 17488-17505: `TimbresEditor` master controls strip containing `masterDbSlider`, `gTranspSlider`, `reverbReturnSlider`, `panicButton`, `sustainToggle`, `invPedalToggle`, `masterAutoEqBtn`, `masterPunchBtn`, `reverbTypeCombo`.

4. **Build System & Compiler Flags (`CMakeLists.txt` & `ChurchDAW.jucer`)**:
   - `CMakeLists.txt:1` minimum CMake 3.22; `line 21` C++17 standard (`set(CMAKE_CXX_STANDARD 17)`).
   - `CMakeLists.txt:47` MSVC runtime: `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`.
   - `CMakeLists.txt:489-494` target GUI app `MinhaDAW` (product name "ChurchDAW").
   - `CMakeLists.txt:645-653` plugin hosting definition:
     ```cmake
     target_compile_definitions(MinhaDAW PRIVATE
         CHURCHDAW_HOSTS_PLUGINS=${CHURCHDAW_PLUGINS}
         JUCE_PLUGINHOST_VST3=${CHURCHDAW_PLUGINS}
         JUCE_WEB_BROWSER=0
         JUCE_PUSH_NOTIFICATIONS=0
         JUCE_USE_CURL=0)
     ```
   - `CMakeLists.txt:692-702`: `target_link_libraries(MinhaDAW PRIVATE ChurchAssets juce::juce_cryptography juce::juce_dsp juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_devices juce::juce_gui_extra)`.
   - `ChurchDAW.jucer:16-21`: `ANDROIDSTUDIO` export format, defines `CHURCHDAW_HOSTS_PLUGINS=0`, `JUCE_PLUGINHOST_VST3=0`, `JUCE_USE_ANDROID_OBOE=1`. Includes modules `juce_audio_processors`, `juce_audio_processors_headless`, `juce_dsp`.

---

## 2. Logic Chain

1. **From Observation 1 & 2**: `MainComponent` is the root UI component and owner of the JUCE `AudioProcessorGraph`. Header action buttons are positioned dynamically in `MainComponent::resized()`. On Mobile, actions are arranged into a dedicated 48dp touch action bar (`actionRow`).
   - *Inference*: Adding a `juce::ToggleButton` or `juce::TextButton` ("Sabrina Bypass" / "Sabrina On/Off") to `MainComponent` header controls guarantees visibility and interaction across both Desktop and Mobile interfaces.

2. **From Observation 3**: `TimbresEditor` (the main mixer interface for Church Live) contains a master controls section with utility buttons (`masterPunchBtn`, `masterAutoEqBtn`, `reverbTypeCombo`).
   - *Inference*: Placing a secondary "SABRINA" toggle button in `TimbresEditor` master controls allows instant bypass/enable operation directly from the live performance mixer view.

3. **From Observation 4**: Cross-platform target flags, compiler settings, and JUCE module dependencies (`juce_audio_processors`, `juce_dsp`, `juce_gui_extra`, `juce_gui_basics`) are already declared and linked in `CMakeLists.txt` (for Desktop Windows/macOS) and `ChurchDAW.jucer` (for Mobile Android).
   - *Inference*: No new external CMake or Projucer library dependencies are required to bind a JUCE toggle button to an atomic bypass flag or `juce::AudioProcessor::setBypassed()`. On Android, where binary VST3 plugin hosting is disabled (`CHURCHDAW_HOSTS_PLUGINS=0`), a C++ native DSP fallback or safe bypass ensures clean cross-platform compilation.

---

## 3. Caveats

- **Binary Plugin Format on Android**: Android ARM devices cannot run x86_64 / MSVC DLL / macOS VST3 binaries directly. The implementation for Android must use conditional compilation (`#if JUCE_ANDROID` or `CHURCHDAW_HOSTS_PLUGINS`) or a native C++ DSP implementation so that the bypass toggle operates cleanly without runtime crash or compilation failure.
- **Source Code Read-Only Constraint**: As an explorer subagent, no edits were made to C++ files in `Source/`. All findings and proposed code changes are fully documented in `analysis.md` and this report.

---

## 4. Conclusion

The ChurchDAW UI component hierarchy and build system have been mapped completely:
1. `MainComponent` and `TimbresEditor` are the ideal UI mounting locations for the "Sabrina Bypass" / "Sabrina On/Off" toggle button.
2. The UI toggle button should be placed in `MainComponent`'s header action bar (rendering across Desktop & Mobile) and `TimbresEditor`'s master controls strip.
3. The DSP bypass state should be stored as an atomic boolean (`std::atomic<bool> sabrinaBypassed`) that bypasses audio processing in 0ms, reducing Sabrina's CPU utilization to 0% when disabled.
4. The CMake and Projucer build setups are fully cross-platform ready with `juce_audio_processors` and `juce_dsp` already linked for Windows, macOS, and Android.

---

## 5. Verification Method

To verify these findings independently:

1. **Inspect UI Hierarchy & Header Buttons**:
   - Open `Source/MainComponent.h` lines 175–190 and `Source/MainComponent.cpp` lines 2050–2210 to confirm header action bar layout.
   - Open `Source/TimbresProcessor.h` lines 17488–17540 to confirm master controls buttons in `TimbresEditor`.

2. **Inspect Build Configuration**:
   - Open `CMakeLists.txt` lines 645–702 to verify `MinhaDAW` definitions and linked JUCE modules.
   - Open `ChurchDAW.jucer` lines 16–21 and 180–195 to verify Android Studio exporter settings and JUCE module includes.

3. **Validate Build & Tests (when building)**:
   - Run CMake configure: `cmake -B build -S . -DCHURCHDAW_BUILD_AUDIO_TESTS=ON`
   - Run CTest test suite: `ctest --test-dir build --output-on-failure`
