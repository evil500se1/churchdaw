## 2026-08-04T15:48:32Z
<USER_REQUEST>
You are teamwork_preview_worker 1. Your working directory is D:\ChurchDAW_Source\.agents\worker_doc_writer.

Your task is to synthesize all technical findings and author the master architectural guide at D:\ChurchDAW_Source\CHURCHDAW_ARCHITECTURE_GUIDE.md.

Step 1: Read D:\ChurchDAW_Source\.agents\ORIGINAL_REQUEST.md.
Step 2: Read all three survey handoff reports:
- D:\ChurchDAW_Source\.agents\explorer_audio_dsp\handoff.md
- D:\ChurchDAW_Source\.agents\explorer_ui_assets\handoff.md
- D:\ChurchDAW_Source\.agents\explorer_build_history\handoff.md

Step 3: Create D:\ChurchDAW_Source\CHURCHDAW_ARCHITECTURE_GUIDE.md containing a comprehensive, highly detailed architectural guide formatted cleanly in Markdown. Ensure it covers:

# CHURCHDAW ARCHITECTURE GUIDE

1. **System Overview & Architectural Principles**
   - High-level vision of ChurchDAW (C++ / JUCE 7 DAW for live worship performance).
   - Supported target platforms (Windows, macOS, iOS, Android).
   - Real-time zero-allocation audio processing philosophy & thread safety.
   - Dark Nord synthesizer GUI design system philosophy.
   - Dual build system rationale (CMake for Desktop/iOS, Projucer for Android).

2. **Subsystem Architecture & C++ Module Mapping**
   - Comprehensive table & mapping of all C++ classes in `Source/` and `Source/ChurchOrgan/`:
     - Audio Engine & Processing: `MainComponent`, `TimbresProcessor`, `MasterProcessor`, `PadProcessor`, `StereoLookaheadLimiter`, `SampleStreamer`, `SamplePack`, `TimbreCatalog`.
     - DSP Modules: `WorshipReverb`, `ChannelRotarySpeaker`, `ChannelToneShaper`.
     - Native Church Organ Synthesis Core (`Source/ChurchOrgan/`): `ChurchOrganCore`, `ChurchOrganLayerProcessor`, `PipeOrganEngine`, `TonewheelOrganEngine`, `TransistorOrganEngine`, `WorshipHybridEngine` / `WorshipLayerEngine`, `RotaryCabinet`, `ScannerVibrato`, `OrganAmpCab`, `TonewheelBank`, `TransistorDividerBank`, `KeyClickGenerator`, `MultiContactKeying`, `HarmonicPercussion`, `AutoMixContext`.
     - User Interface (UI) Layer: `NordLookAndFeel`, `OledDisplay`, `LevelMeter`, `DeviceInfoBar`, `AudioMidiButton`, `PadPanel`, `LiveScenePanel`, `TimbrePicker`, `MidiLearnPanel`, `LibraryPanel`, `ChurchOrganEditor`, `StartupPanel`, `AboutPanel`, `AndroidAudioMidiCenter`, `DesktopLoginPanel` / `AndroidLoginPanel`.
     - Visual Assets: Glyph files (`glyph_piano.png` .. `glyph_lead.png`), app icons, round logo, vector rendering fallbacks (`drawChurchDAWMark`).

3. **Audio Signal Flow & Routing Diagrams**
   - Detailed step-by-step text description of audio signal flow from physical ASIO/hardware audio inputs & physical MIDI inputs through the entire graph.
   - ASCII Architecture & Signal Flow Diagram.
   - Step 1: Input & MIDI handling (`midiInNode`, `audioInNode`, velocity remapping, pitch bend guard).
   - Step 2: Voice rendering & synthesis (`TimbresProcessor::renderVoices`, RAM preload + `SampleStreamer` disk streaming, `ChurchOrganCore` engine synthesis).
   - Step 3: Layer DSP processing (`ChannelToneShaper` 3-band EQ, `ChannelRotarySpeaker`, layer compression, delay).
   - Step 4: Aux Reverb send/return bus (`WorshipReverb` 100% wet FDN matrix, Hadamard feedback, dual Pitch Shimmer +12/+7 semitones, dynamic ducking follower).
   - Step 5: Insert FX slots & continuous ambient pad (`PadProcessor` TPT zero-delay filter).
   - Step 6: Master bus processing (`MasterProcessor` Auto EQ 4-band mastering curve, Worship Punch parallel compression/saturation, `StereoLookaheadLimiter` 8x true-peak FIR oversampling, 3ms lookahead, -1.0 dBFS ceiling).
   - Step 7: Physical DAC output (`audioOutNode`).

4. **Build System, Dependencies & Target Environments**
   - Dual build system strategy: Root CMake 3.22 (Windows MSVC, macOS Apple Clang, iOS) vs. Projucer (`ChurchDAW.jucer` for Android Studio Gradle).
   - JUCE Framework integration: 15 core embedded modules (`juce_audio_basics`, `juce_audio_processors`, `juce_dsp`, etc.) and disabled feature flags (`JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`).
   - Steinberg ASIO SDK 2.3 integration (`JUCE_ASIO=1`, `asiosdk/common`).
   - Feature flags breakdown: `CHURCHDAW_HOSTS_PLUGINS`, `CHURCHDAW_ENABLE_DESKTOP_LICENSING`, `CHURCHDAW_BUILD_AUDIO_TESTS`, `CHURCHDAW_BUILD_INSTALLER`.
   - Target environment differences table (Desktop vs. Mobile: toolchain, plugin hosting, licensing, updater, audio driver, stack allocation).

5. **Development History & Codebase Evolution Notes**
   - In-depth analysis of identified history/notes files:
     - `BUG_REVERB_LIMITES.md`: Out-of-bounds indexing in `WorshipReverb.h:505`, ULP rounding root cause, integer decomposition fix, AddressSanitizer regression probe `ChurchDAWReverbBoundsProbe`.
     - `ATUALIZACAO_DESKTOP.md`: Desktop auto-updater architecture (`DesktopUpdater.cpp`), dual RS256 RSA-3072 + Authenticode/Gatekeeper security model, non-blocking execution, unified catalog fix (`/app/android/manifest.json`).
     - `KIMI_NOTAS.md`: Tonewheel/Worship `RotaryCabinet` vs. `ChannelRotarySpeaker` DSP routing, `drainPendingLayerLoads` async state restore, macOS transpose fix.
     - Context from `PROJECT.md`, `analysis.md`, `handoff.md`, `TEST_READY.md`, `Packaging/AndroidDistribution.md`, `Design/ChurchOrganEngine/ARCHITECTURE.md`.

6. **Maintenance & Architectural Recommendations**
   - Real-time audio thread rules: Zero heap allocation, pre-allocated buffers with `avoidReallocating`, atomic/smoothed parameter handling.
   - UI rendering optimizations: Throttling level meters (10 Hz mobile / 30 Hz desktop), vector fallbacks (`drawChurchDAWMark`), drop shadow caching.
   - Subprocess plugin scanning safety: Out-of-process isolation via `ChildProcessWorker`.
   - Maintenance checklist for future ChurchDAW contributors.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Write your handoff report to D:\ChurchDAW_Source\.agents\worker_doc_writer\handoff.md and notify the orchestrator when complete.
</USER_REQUEST>
