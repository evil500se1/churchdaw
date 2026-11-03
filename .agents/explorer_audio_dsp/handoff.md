# Technical Exploration Handoff Report — Audio Engine & DSP Processing in ChurchDAW

## 1. Observation

### 1.1 Audio Engine File Structure & Inventory
The audio engine and DSP processing architecture of ChurchDAW is situated in `D:\ChurchDAW_Source\Source` and subdirectories (`Source/ChurchOrgan`, `Tests/Audio`, `Tests/ChurchOrgan`). The key C++ files and header components comprise:

- **Host & Graph Management**:
  - `Source/MainComponent.h` & `Source/MainComponent.cpp`: Application host, `juce::AudioDeviceManager` integration, `juce::AudioProcessorGraph` setup, graph node connections, and ASIO/WASAPI/CoreAudio/AAudio setup.
  - `Source/TimbresProcessor.h`: Primary audio engine processor (`TimbresProcessor : public juce::AudioProcessor`). Manages 4 instrument layers, sample voice rendering, disk streaming, per-layer DSP, insert plugin hosting, and WorshipReverb send bus.
  - `Source/MasterProcessor.h`: Master output bus processor (`MasterProcessor : public juce::AudioProcessor`). Manages master volume, Auto EQ, Worship Punch parallel compression, and `StereoLookaheadLimiter`.
  - `Source/PadProcessor.h`: Dedicated ambient continuous pad synth (`PadProcessor : public juce::AudioProcessor, private juce::Thread`). Streams background worship pads with TPT zero-delay filter.
  - `Source/StereoLookaheadLimiter.h`: Brickwall peak limiter (`StereoLookaheadLimiter`) with 8x true-peak FIR oversampling detector and 3 ms lookahead.

- **DSP Effect Modules**:
  - `Source/WorshipReverb.h`: Native Worship/Pro reverb algorithm (`WorshipReverb`). 100% wet send bus, 12 algorithm presets, 8-line FDN matrix with Hadamard feedback, dual Pitch Shimmer (+12/+7 semitones), 4 allpass diffusers, predelay, 3-band T60 decay, orthogonal spatial rotation, dynamic ducking follower.
  - `Source/ChannelRotarySpeaker.h`: Dual-rotor Leslie speaker cabinet emulator (`ChurchDAWRotary::Speaker`) for sampled, transistor, and pipe organ layers.
  - `Source/ChannelToneShaper.h`: 3-band musical channel EQ (`ChurchDAWChannelTone::ThreeBandEq`) with parameter smoothing (`SmoothedValue`).

- **Church Organ Subsystem (`Source/ChurchOrgan/`)**:
  - `ChurchOrganCore.h` / `.cpp`: Core organ engine dispatcher.
  - `ChurchOrganLayerProcessor.h` / `.cpp`: `AudioProcessor` adapter interfacing `ChurchOrganCore` with `TimbresProcessor`.
  - `PipeOrganEngine.h` / `.cpp`: Physical/additive modeling pipe organ engine.
  - `TonewheelOrganEngine.h` / `.cpp`: Hammond B3-style tonewheel organ engine (9 drawbars per manual, scanner vibrato/chorus, crosstalk, foldback, key click).
  - `TransistorOrganEngine.h` / `.cpp`: Vox Continental / Farfisa style combo organ engine.
  - `WorshipLayerEngine.h` / `.cpp` & `WorshipHybridEngine.h` / `.cpp`: Synth layer engine & hybrid organ-pad synth engine.
  - `RotaryCabinet.h` / `.cpp`: Native organ Leslie cabinet DSP embedded inside `ChurchOrganCore` (dual rotor, Doppler delay lines, horn/drum physics, air absorption lowpass, adaptive wet loudness calibration).
  - `ScannerVibrato.h` / `.cpp`: Electromechanical scanner vibrato/chorus simulation (V1-V3, C1-C3).
  - `OrganAmpCab.h` / `.cpp`: Tube amplifier overdrive & cabinet impulse response/filter DSP.
  - `TonewheelBank.h` / `.cpp` & `TonewheelCharacter.h` / `.cpp`: 91 tonewheel generator bank, phase alignment, crosstalk/foldback/leakage emulation.
  - `TransistorDividerBank.h` / `.cpp`: Frequency divider bank DSP for transistor organ.
  - `KeyClickGenerator.h` / `.cpp`, `MultiContactKeying.h` / `.cpp`, `HarmonicPercussion.h` / `.cpp`, `HarmonicFoldback.h` / `.cpp`: Organ mechanical noise, contact bounce, harmonic percussion, and foldback DSP.
  - `AutoMixContext.h` / `.cpp`: Organ auto-mixing and dynamic gain leveling context.

- **Sample Streaming & Memory**:
  - `Source/SampleStream.h`: Background thread sample streaming engine (`SampleStreamer : private juce::Thread`) with RAM preload buffer and windowed disk reads (`windowSamples`, `windowOverlap`).
  - `Source/SamplePack.h`: Encrypted pack file reader (ChaCha20 encryption, SHA-256 integrity verification).
  - `Source/TimbreCatalog.h` & `Source/TimbreToneMemory.h`: Timbre catalog metadata & user preset memory store.

---

### 1.2 Class Mapping Table

| C++ Class Name | File Path | Primary Responsibilities |
|---|---|---|
| `MainComponent` | `Source/MainComponent.h` | Application host, `AudioDeviceManager` binding, `AudioProcessorGraph` node setup & routing, ASIO device lifecycle, MIDI input listener. |
| `TimbresProcessor` | `Source/TimbresProcessor.h` | Main multi-layer instrument processor & mixer. Handles 4 layers, sample voice allocation, organ engine dispatch, velocity curve remapping, layer EQ, rotary, compression, insert VST/AU hosting, and reverb send. |
| `MasterProcessor` | `Source/MasterProcessor.h` | Master bus output processor. Controls master gain, Auto EQ mastering curve, Worship Punch parallel compression/saturation, and peak limiter. |
| `PadProcessor` | `Source/PadProcessor.h` | Background continuous worship pad generator. Asynchronous WAV loader (`PadLoader` thread), key crossfading, TPT zero-delay cutoff filter. |
| `StereoLookaheadLimiter` | `Source/StereoLookaheadLimiter.h` | Brickwall peak limiter. 8x true-peak FIR equiripple detector, 3 ms lookahead delay, -1.0 dBFS output ceiling, -2.1 dBFS detector ceiling, stereo-linked detection. |
| `WorshipReverb` | `Source/WorshipReverb.h` | Native Worship/Pro reverb algorithm (100% wet send bus). 12 algorithm presets, 8-line FDN matrix, Hadamard feedback, 4-stage allpass diffusers, dual Pitch Shimmer (+12/+7 semitones), dynamic ducking. |
| `ChurchDAWRotary::Speaker` | `Source/ChannelRotarySpeaker.h` | Dual-rotor Leslie speaker cabinet DSP (Horn & Drum crossover 800Hz, Doppler fractional delay, amplitude/phase modulation, Slow/Stop/Fast inertia). |
| `ChurchDAWChannelTone::ThreeBandEq` | `Source/ChannelToneShaper.h` | 3-band musical channel EQ (Low shelf, Mid bell, High shelf) with parameter smoothing (`SmoothedValue`). |
| `ChurchOrganCore` | `Source/ChurchOrgan/ChurchOrganCore.h` | Core facade for Church Organ synthesis (Pipe, Tonewheel, Transistor, Worship Hybrid). |
| `churchorgan::ChurchOrganLayerProcessor` | `Source/ChurchOrgan/ChurchOrganLayerProcessor.h` | `AudioProcessor` adapter interfacing `ChurchOrganCore` with `TimbresProcessor`. |
| `PipeOrganEngine` | `Source/ChurchOrgan/PipeOrganEngine.h` | Physical/additive pipe organ synthesis DSP with multi-rank stop combinations. |
| `TonewheelOrganEngine` | `Source/ChurchOrgan/TonewheelOrganEngine.h` | Hammond B3-style tonewheel organ engine (9 drawbars, scanner vibrato/chorus, crosstalk, foldback, key click). |
| `TransistorOrganEngine` | `Source/ChurchOrgan/TransistorOrganEngine.h` | Vox Continental / Farfisa style transistor combo organ DSP (frequency divider banks, tab switches, vibrato). |
| `WorshipHybridEngine` / `WorshipLayerEngine` | `Source/ChurchOrgan/WorshipHybridEngine.h` | Synth layer engine & hybrid organ-pad synth engine. |
| `RotaryCabinet` | `Source/ChurchOrgan/RotaryCabinet.h` | Native organ Leslie cabinet DSP embedded inside `ChurchOrganCore` (dual rotor, Doppler delay lines, horn/drum physics, air absorption lowpass, adaptive wet loudness calibration). |
| `ScannerVibrato` | `Source/ChurchOrgan/ScannerVibrato.h` | Electromechanical scanner vibrato/chorus simulation (V1-V3, C1-C3). |
| `OrganAmpCab` | `Source/ChurchOrgan/OrganAmpCab.h` | Tube amplifier overdrive & cabinet impulse response/filter DSP. |
| `SampleStreamer` | `Source/SampleStream.h` | Disk-streaming engine running on background worker thread (`SampleStreamer : private juce::Thread`) with RAM preload buffer. |

---

## 2. Logic Chain

### 2.1 Complete Audio Signal Flow (Step-by-Step)

```
[ ASIO / Hardware Audio Inputs ] (Mic/Guitar)
             │
             ▼ (inputToggle gate in MainComponent)
[ audioInNode ] ──────┐
                      │
[ Physical MIDI Inputs ] ─► [ midiInNode ]
                                  │
                                  ▼ (MIDI NoteOn/Off, CC, Velocity Curve Remapping)
                      ┌──────────────────────────────────────┐
                      │           TimbresProcessor           │
                      │  (4 Instrument Layers: Piano, EP,    │
                      │      Synth/Pad, Church Organ)       │
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
                                         ▼ (Insert VST/AU FX Chain: Slots 1..3)
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

1. **Audio Input & MIDI Processing**:
   - `juce::AudioDeviceManager` opens driver buffer (e.g., 64–512 samples at 44.1/48 kHz).
   - Audio inputs enter via `audioInNode` (`AudioGraphIOProcessor::audioInputNode`).
   - MIDI events enter `midiInNode` (`AudioGraphIOProcessor::midiInputNode`) and reach `TimbresProcessor::processBlock`.
   - `TimbresProcessor` remaps note velocities via `mapVelocity` using the configured `velCurve` (Natural, Hard, Soft, Linear).
   - `PitchBendGuard` centers pitch bend and safely processes CC64 (sustain), CC120/123 (all sound/notes off), and global Panic.

2. **Instrument Layer & Synthesis Rendering (`renderVoices`)**:
   - `TimbresProcessor` evaluates 4 layers (`layers[0..3]`).
   - **Sampled Instrument Layers**: Rendered via `Voice` array. Transient start is read directly from RAM (`preloadLen`). Sustained sample tail is fetched asynchronously by `SampleStreamer` thread in windowed blocks (`windowSamples`, `windowOverlap`). Hermite cubic interpolation (`hermiteFast`) resamples pitch.
   - **Church Organ Subsystem**: Rendered via `ChurchOrganLayerProcessor` / `ChurchOrganCore`. Depending on mode, triggers `PipeOrganEngine`, `TonewheelOrganEngine`, `TransistorOrganEngine`, or `WorshipHybridEngine`. Integrated Leslie simulation is applied via native `RotaryCabinet` or `ChannelRotarySpeaker`.
   - **Layer DSP Processing**: Each layer passes through `ChannelToneShaper` (3-band EQ), `ChannelRotarySpeaker` (if enabled), `comps[l]` (dynamic compressor), `manualEqs[l]` (parametric EQ), and `delayBufs[l]` (delay buffer).

3. **Reverb Send / Return Bus**:
   - Layers output their wet reverb send proportion to `reverbBus`.
   - `WorshipReverb::process` processes 100% wet audio on `reverbBus` through: 95 Hz Input HPF -> Stereo Predelay -> 4 Allpass Diffusers -> 8-line FDN Matrix -> 3-Band T60 Decay Dampening -> Orthogonal Matrix Motion -> Dual Pitch Shimmer (+12/+7 semitones) -> Dynamic Ducking Follower -> Stereo Width -> 14.5 kHz LPF.
   - `reverbReturnSmooth` applies a smoothed return ramp and adds `reverbBus` into the main buffer.

4. **Insert FX Slots & Pad Engine**:
   - Audio flows through up to 3 insert FX slots (`inserts[0..2]`).
   - Concurrently, `PadProcessor` (`padNode`) streams continuous background ambient pad WAVs (A..G + accidentals) with smooth key crossfading and TPT lowpass filtering (`PoloTPT`).

5. **Master Bus Processing (`MasterProcessor`)**:
   - Audio enters `MasterProcessor::processBlock`:
     - **Auto EQ**: 4-band mastering EQ (Low shelf 110 Hz +1.5 dB, Mid peak 450 Hz -1.5 dB, High peak 3000 Hz -1.0 dB, High shelf 11000 Hz +2.0 dB).
     - **Worship Punch**: Parallel compression path (HPF 130 Hz -> Compressor 3.5:1 -> Tanh saturation `tanh(p * 1.95) * 0.45` -> Low shelf 90 Hz +3.0 dB -> Mid peak 320 Hz -2.0 dB) summed back to master.
     - **Master Gain**: Master volume scaling (`dbParam`, -60 dB to +10 dB) using `juce::SmoothedValue`.
     - **Stereo Lookahead Limiter**: `StereoLookaheadLimiter::process` applies 8x true-peak FIR equiripple oversampling detector, 3 ms lookahead delay, -1.0 dBFS output ceiling, and -2.1 dBFS detector ceiling.
     - **Output Metering**: Peak levels recorded in `levels[2]` for UI meters.

6. **Physical DAC Output**:
   - Output flows from `masterNode` to `audioOutNode` (`AudioGraphIOProcessor::audioOutputNode`) -> `juce::AudioProcessorPlayer` -> Physical ASIO hardware outputs 1 & 2.

---

### 2.2 DSP Constraints, Buffer Management & Threading

1. **Real-Time (RT) Zero-Allocation Rule**:
   - All audio callbacks (`processBlock`, `renderVoices`, `StereoLookaheadLimiter::process`, `WorshipReverb::process`) strictly avoid heap memory allocations (`new`/`delete`, `std::vector::resize`, `juce::String` creation, mutex locking).
   - Buffers are pre-allocated during `prepareToPlay` / `prepare` (up to 8192 samples). Block size changes use `buffer.setSize(2, numSamples, false, false, true)` with `avoidReallocating = true`.

2. **Reverb Index Bounds & Fractional Delay (`BUG_REVERB_LIMITES.md`)**:
   - In `WorshipReverb.h`, fractional delay line interpolation was fixed to eliminate float rounding errors (`writeIndex - delay` rounding to `data.size()`).
   - `WorshipReverb::FractionalDelay::read` separates integer and fractional delay components (`wholeDelay + fraction`) and strictly bounds integer indexing within `[0, count - 1]`, preventing MSVC Debug `vector subscript out of range` and AddressSanitizer `container-overflow`.

3. **Sample Disk Streaming (`SampleStream.h`)**:
   - Disk sample playback is decoupled from the audio callback via background thread `SampleStreamer` (`juce::Thread`).
   - Initial sample attack (~0.5s) resides in RAM (`preloadLen`). When playback nears the end of preload, `SampleStreamer::VoiceStream` requests windowed reads (`windowSamples = 65536`, `windowOverlap = 8192`) on the worker thread.

4. **Threading & Lock-Free State Management**:
   - GUI-to-audio thread parameter transfers use `std::atomic` or `juce::SmoothedValue`.
   - Subprocess plugin scanning (`PluginScanSubprocessV2.h`) isolates third-party VST/AU plugin scanning into a separate worker process (`ChildProcessWorker`), keeping the main DAW process safe from plugin crashes.

5. **Lookahead Latency Compensation**:
   - `StereoLookaheadLimiter` introduces 3 ms of lookahead latency (`lookaheadSamples`), which is reported to JUCE via `setLatencySamples()` for host latency compensation.

---

## 3. Caveats

1. **Subprocess Plugin Scan Boundaries**: Third-party VST3/AU insert plugins hosted in `slots` run within the main audio graph thread once loaded, but candidate validation and initial scanning run out-of-process via `PluginScannerSubprocessV2.h`.
2. **Audio Instrumentation Overheads**: `CHURCHDAW_AUDIO_INSTRUMENTATION` is disabled by default (`0`). Enabling it in CMake causes `juce::Logger::writeToLog` calls inside the audio callback once per second for debugging.

---

## 4. Conclusion

ChurchDAW implements a robust, modular, high-performance C++/JUCE audio engine architecture optimized for live worship performance. The architecture combines disk-streaming sample playback (`SampleStreamer`), physical/additive organ synthesis engines (`ChurchOrganCore`), studio-grade Worship Reverb (`WorshipReverb`), rotary cabinet emulations (`ChannelRotarySpeaker` & `RotaryCabinet`), and a mastering-grade master bus (`MasterProcessor` with `StereoLookaheadLimiter`). The audio callback is strictly zero-allocation, lock-free, and thread-safe.

---

## 5. Verification Method

To independently verify the audio engine and DSP architecture findings:

1. **Inspect Core Files**:
   - View `Source/TimbresProcessor.h` (line 4885 `processBlock`, line 9050 `renderVoices`).
   - View `Source/MasterProcessor.h` (line 153 `processBlock`).
   - View `Source/WorshipReverb.h` (line 61 `prepare`, line 466 `FractionalDelay`).
   - View `Source/StereoLookaheadLimiter.h` (line 26 `outputCeilingDb`, line 108 `process`).
   - View `Source/MainComponent.cpp` (lines 468-490 audio graph creation, lines 7008-7155 `rebuildGraph` connection routing).

2. **Execute DSP Unit & Probe Tests**:
   Build and run the audio DSP test suite via CMake:
   ```powershell
   cmake -S . -B build -DCHURCHDAW_BUILD_AUDIO_TESTS=ON
   cmake --build build --config Debug --target ChurchDAWReverbBoundsProbe
   .\build\ChurchDAWReverbBoundsProbe_artefacts\Debug\ChurchDAWReverbBoundsProbe.exe
   ```
   Confirm all reverb bounds, multi-preset transitions, and buffer size variations pass deterministically without memory or subscript assertions.
