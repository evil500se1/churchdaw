# Comprehensive Technical Analysis: Audio Engine & Real-Time Safety Audit (Requirement R1)

**Audit Target**: ChurchDAW (C++17 / JUCE 7)  
**Auditor**: Explorer 1 (Audio Engine & Real-Time Safety Specialist)  
**Date**: 2026-08-14  
**Scope**: Real-time audio path, audio callback hierarchy, voice rendering, DSP chains, sample streamers, MIDI buffer handling, locking primitives, memory allocations, I/O operations, logging, and thread safety.

---

## 1. Executive Summary

ChurchDAW is a specialized live church worship DAW and virtual instrument host designed for low-latency live performance. The core audio engine comprises a 14-layer multi-instrument processor (`TimbresProcessor`), a native 4-family organ engine (`ChurchOrgan`), a background continuous worship pad processor (`PadProcessor`), a 16-program pad synthesizer (`PadSynthProcessor`), a mastering processor with parallel punch compression (`MasterProcessor`), an 8-line FDN worship reverb (`WorshipReverb`), and an 8x true-peak brickwall lookahead limiter (`StereoLookaheadLimiter`).

While extensive architectural effort has been made to establish lock-free zero-allocation design across the DSP core (notably in `ChurchOrgan`, `WorshipReverb`, `StereoLookaheadLimiter`, and `ChannelToneShaper`), **several critical hard real-time violations were identified** in the active audio path:
1. **Critical Mutex Locking in Audio Callback (`PadProcessor.h`)**: `PadProcessor::processBlock` acquires `juce::CriticalSection voiceLock` synchronously on the audio thread, while the background sample loader thread also acquires `voiceLock` in `drainReleasePool()` to deallocate 100MB+ buffers. This creates a severe priority inversion and blocking risk.
2. **Dynamic Heap String Allocations & Synchronous File/Console Logging (`TimbresProcessor.h`)**: `TimbresProcessor::processBlock` executes `juce::Logger::writeToLog` with concatenated `juce::String` objects directly within the audio callback under Android/Instrumentation and Mac self-test configurations.
3. **Shared Pointer Lifetime Overhead on Audio Thread (`PadProcessor.h` & `SampleStream.h`)**: Atomic refcount increments and potential destructor deallocations of `std::shared_ptr<juce::AudioBuffer<float>>` and `std::shared_ptr<const Source>` occur during voice transitions in the audio thread.
4. **Kernel System Call Signaling (`SampleStream.h`, `TimbresProcessor.h`, `PadProcessor.h`)**: `SampleStreamer::wake()` and `PadProcessor::loadEvent.signal()` invoke OS condition variable / waitable event signals (`pthread_cond_broadcast` / `SetEvent`) from the audio callback.

---

## 2. Audio Entry Points & Processing Pipeline

The signal flow follows a deterministic, hierarchical processing graph managed by `juce::AudioProcessorGraph` and executed via `juce::AudioProcessorPlayer`:

```
                                  [ Hardware Audio In / ASIO / CoreAudio / Oboe ]
                                                         │
                                               [ MainComponent ]
                                   (audioDeviceIOCallbackWithContext)
                                                         │
                                           [ AudioProcessorGraph ]
                          ┌──────────────────────────────┴──────────────────────────────┐
                          ▼                                                             ▼
                 [ midiInNode ]                                                 [ audioInNode ]
                          │                                                             │
                          ▼ (MIDI Note, CC, Vel Curve Remap, PitchBendGuard)             │
                ┌──────────────────┐                                                    │
                │ TimbresProcessor │ ◄──────────────────────────────────────────────────┘
                └─────────┬────────┘
                          │
     ┌────────────────────┼────────────────────────┐
     ▼                    ▼                        ▼
[ Layer Voices 0..13 ] [ ChurchOrganCore ] [ Channel DSPs ]
(Sample Playback /    (Tonewheel, Pipe,   (ChannelToneShaper 3-Band EQ,
 RAM Preload +         Transistor,         ChannelRotarySpeaker,
 Disk Streamer)        Worship Hybrid)     Layer Comp, Delay Line)
     │                    │                        │
     └────────────────────┼────────────────────────┘
                          │
                          ├───► [ Reverb Send Bus ]
                          │              │
                          │              ▼
                          │      [ WorshipReverb ]
                          │    (8-Line FDN, Dual Shimmer,
                          │     Ducking, 12 Presets)
                          │              │
                          ▼ (Return Sum) │
                   [ Layer Summing ] ◄───┘
                          │
                          ▼ (Insert FX Slots 1..2: VST3 / AU)
                   [ timbresNode ]
                          │
  [ PadProcessor (padNode) ] ─────────────────┐
  (Continuous Pad, Dual TPT Filter)           │
                                              ▼
                                    [ MasterProcessor ]
                                       (masterNode)
                                              │
                                              ├─► [ Auto EQ (4-Band Mastering Curve) ]
                                              ├─► [ Worship Punch (Parallel Compression + Saturation) ]
                                              ├─► [ Master Gain (SmoothedValue Ramp) ]
                                              └─► [ StereoLookaheadLimiter (8x True-Peak FIR, 3ms Delay) ]
                                              │
                                              ▼
                                      [ audioOutNode ]
                                              │
                                  [ Physical Hardware DAC ]
```

### Detailed Execution Trace:

1. **Top-Level Audio Callback**:
   - `MainComponent::audioDeviceIOCallbackWithContext` (`Source/MainComponent.cpp:4497`): Validates `audioInputAuthorised` atomic, delegates to `juce::AudioProcessorPlayer`, and monitors output sample sanity/finiteness without allocating.
2. **Multi-Instrument Processing**:
   - `TimbresProcessor::processBlock` (`Source/TimbresProcessor.h:5859`):
     - Clears output buffer and establishes `juce::ScopedNoDenormals`.
     - Applies `mapVelocity` via pre-reserved `midiScratch` (`ensureSize(256 * 1024)`).
     - Dispatches panic / CC reset via `ChurchDAWPerformanceMidiSafety::PitchBendGuard`.
     - Allocates auxiliary `reverbBus` using `setSize(2, numSamples, false, false, true)` (`avoidReallocating = true`).
     - Calls `renderVoices(buffer)`.
     - Processes `WorshipReverb::process` or sends to third-party reverb insert.
     - Iterates insert VST3/AU slots (`ins->processBlock`).
     - Applies master NaN sum-check guard and finite overload clamping (`24.0f / pico`).
3. **Voice Rendering & Sample Streaming**:
   - `TimbresProcessor::renderVoices` (`Source/TimbresProcessor.h:11686`):
     - Renders synthesized Church Organ (`organ->processBlock`) and hosted instrument VSTs (`inst->processBlock`).
     - Divides sample rendering into sample-accurate sub-ranges between MIDI boundaries (`renderNativeRangeChunk`).
     - Evaluates active voices: reads transient RAM preloads for initial attack; requests asynchronous windowed reads via `SampleStreamer::VoiceStream` (`acquire`, `prefetch`, `request`).
     - Applies per-layer DSP: `comps[l]`, `manualEqs[l]`, `nordAmpEqs[l]`, `rotarySpeakers[l]`, `delayBufs[l]`, `eqs[l]`.
     - Wakes `SampleStreamer` worker thread via single `streamer.wake()` at block boundary.
4. **Continuous Worship Pad**:
   - `PadProcessor::processBlock` (`Source/PadProcessor.h:364`):
     - Acquires `voiceLock`.
     - Crossfades between ambient pad keys (Keys A through G#) with 1.6s fade curves.
     - Resamples via 2-point linear or 4-point Hermite interpolation.
     - Processes 2-pole TPT zero-delay state-variable lowpass filter (`PoloTPT`).
5. **Master Bus Finalization**:
   - `MasterProcessor::processBlock` (`Source/MasterProcessor.h:234`):
     - Evaluates NaN guard on input bus.
     - Applies 4-band mastering Auto EQ in preallocated `autoEqBuf`.
     - Executes parallel "Worship Punch" path (HPF 130 Hz, `juce::dsp::Compressor`, `std::tanh` saturation, 2-band weight EQ).
     - Ramps master volume and punch makeup gains.
     - Applies `StereoLookaheadLimiter::process` with 8x oversampled true-peak detection and -1.0 dBFS ceiling.

---

## 3. Detailed Audit of Real-Time Safety Violations

### Finding 1: Synchronous `CriticalSection` Lock in Audio Thread (`PadProcessor.h`)
- **Severity**: **CRITICAL**
- **File**: `/Users/igorteixeira/ChurchDAW/Source/PadProcessor.h`
- **Class**: `PadProcessor`
- **Method**: `processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)`
- **Exact Line**: Line 384
- **Code Observation**:
  ```cpp
  384: const juce::ScopedLock sl (voiceLock);
  ```
- **Contention Analysis**:
  - `voiceLock` is a member `juce::CriticalSection` (line 1696).
  - In `PadProcessor.h:1589`, the background thread method `drainReleasePool()` (executed inside `juce::Thread::run()`) acquires the same lock:
    ```cpp
    1589: const juce::ScopedLock sl (voiceLock);
    1590: for (int i = 0; i < releaseCount; ++i)
    1591:     local[(size_t) i] = std::move (releasePool[(size_t) i]);
    1592: releaseCount = 0;
    ```
  - While `drainReleasePool` moves the pointers into `local` before destroying the buffers at line 1600, the background thread still holds `voiceLock` during array iterations and shared pointer moves.
- **Real-Time Impact**: If the background thread is preempted by the OS scheduler while holding `voiceLock`, the real-time audio thread will block unconditionally on the mutex, missing its real-time deadline and causing immediate audio glitches/xruns.

---

### Finding 2: Direct Logging & Dynamic Heap String Allocations in Audio Path (`TimbresProcessor.h`)
- **Severity**: **HIGH**
- **File**: `/Users/igorteixeira/ChurchDAW/Source/TimbresProcessor.h`
- **Class**: `TimbresProcessor`
- **Method**: `processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)`
- **Exact Lines**: Lines 6265–6281 and Lines 6335–6341
- **Code Observation**:
  ```cpp
  6264: #if CHURCHDAW_AUDIO_INSTRUMENTATION
  6265:     juce::Logger::writeToLog ("AUDIO: media "
  6266:         + juce::String (100.0 * sumLoad / (double) blocksSeen, 1) + "%  pico "
  6267:         + juce::String (100.0 * peakLoad, 1) + "%  lentos "
  6268:         + juce::String (blocksLate) + "/" + juce::String (blocksSeen)
  6269:         + "  | CHAMADA atrasada " + juce::String (lateWakes)
  6270:         + "x  pior intervalo " + juce::String (peakGap, 2) + "x"
  6271:         + "  | PICO " + juce::String (juce::Decibels::gainToDecibels (peakOut), 1) + " dBFS"
  6272:         + (peakOut > 1.0f ? "  <<< ESTOURANDO" : juce::String())
  6273:         + "  | VOZES " + juce::String (100.0 * juce::Time::highResolutionTicksToSeconds (voicesTicks) / juce::jmax (0.001, sumBudget), 0) + "%"
  6274:         + "  FX " + juce::String (100.0 * juce::Time::highResolutionTicksToSeconds (fxTicks) / juce::jmax (0.001, sumBudget), 0) + "%"
  ...
  6280:         + "  | vozes " + juce::String (activeVoiceCount()));
  6281: #endif
  ```
  ```cpp
  6334: if (macSelfTestSeconds >= 1.0)
  6335: {
  6336:     juce::Logger::writeToLog ("MAC_SELFTEST: blocks "
  6337:         + juce::String (macSelfTestBlocks)
  6338:         + " audible " + juce::String (macSelfTestAudibleBlocks)
  ...
  6341:         + " invalid " + juce::String (macSelfTestInvalidBlocks));
  ```
- **Rationale & Analysis**:
  - `juce::String` concatenation (`+`) performs multiple heap allocations (`malloc`/`HeapAlloc`) on the audio callback thread.
  - `juce::Logger::writeToLog` acquires an internal logger `CriticalSection` and writes synchronously to disk or standard output / Android logcat (`__android_log_print`).
  - Comment at line 6259 acknowledges this violation: *"O texto e montado aqui mesmo -- nao e ideal numa thread de audio, mas acontece 1x/s e o que ele revela vale mais que o risco de um estalo a cada segundo."*
- **Real-Time Impact**: Produces periodic (1 Hz) xruns and latency spikes on Android and macOS devices when instrumentation or self-test is enabled.

---

### Finding 3: `juce::MessageManager::callAsync` Heap Allocation on Audio Thread (`TimbresProcessor.h`)
- **Severity**: **MEDIUM**
- **File**: `/Users/igorteixeira/ChurchDAW/Source/TimbresProcessor.h`
- **Class**: `TimbresProcessor`
- **Method**: `selfTestTick (juce::MidiBuffer& midi, int numSamples)`
- **Exact Line**: Line 6418
- **Code Observation**:
  ```cpp
  6418: juce::MessageManager::callAsync ([]
  6419: {
  6420:     juce::JUCEApplicationBase::quit();
  6421: });
  ```
- **Real-Time Impact**: Allocates a closure and JUCE `Message` on the heap directly from the audio thread upon self-test completion.

---

### Finding 4: Kernel System Call / Condition Variable Signal from Audio Thread (`SampleStream.h` & `PadProcessor.h`)
- **Severity**: **MEDIUM**
- **Files**:
  - `/Users/igorteixeira/ChurchDAW/Source/SampleStream.h` (Line 501)
  - `/Users/igorteixeira/ChurchDAW/Source/TimbresProcessor.h` (Lines 13271, 13342)
  - `/Users/igorteixeira/ChurchDAW/Source/PadProcessor.h` (Line 519)
- **Code Observation**:
  - `SampleStream.h:501`: `void wake() noexcept { notify(); }`
  - `TimbresProcessor.h:13342`: `if (precisaAcordarStreamer) streamer.wake();`
  - `PadProcessor.h:519`: `loadEvent.signal();`
- **Analysis**:
  `juce::Thread::notify()` and `juce::WaitableEvent::signal()` invoke kernel synchronization primitives (`pthread_cond_broadcast` / `SetEvent`). While batching calls to once per audio block significantly reduces overhead, kernel transitions introduce scheduling jitter (50–200 µs).
- **Real-Time Impact**: Minor to moderate latency jitter under high CPU load.

---

### Finding 5: `std::shared_ptr` Atomic Reference Counting and Destruction Risks (`SampleStream.h` & `PadProcessor.h`)
- **Severity**: **MEDIUM**
- **Files**:
  - `/Users/igorteixeira/ChurchDAW/Source/PadProcessor.h` (Lines 408, 433)
  - `/Users/igorteixeira/ChurchDAW/Source/SampleStream.h` (Lines 233, 241)
- **Code Observation**:
  - `PadProcessor.h:408`: `buf = it->second.buffer;` (atomic refcount increment).
  - `PadProcessor.h:433`: `slot->buffer = buf;` (atomic refcount increment).
  - `SampleStream.h:233`: `std::atomic_store_explicit (&source, std::move (s), std::memory_order_release);`
  - `SampleStream.h:241`: `std::atomic_store_explicit (&source, std::shared_ptr<const Source>(), std::memory_order_release);`
- **Analysis**:
  - Atomic reference counting operations incur cache-coherence bus invalidation (`LOCK CMPXCHG`).
  - If a voice release or source replacement decrements the last remaining reference to a `Source` or `AudioBuffer`, the destructor executes on the audio thread, freeing heap memory.
- **Real-Time Impact**: Potential deallocation stalls on the audio thread if references are not retained by parent containers.

---

## 4. Subsystem Real-Time Compliance Evaluation Table

| Subsystem / Component | Real-Time Safe? | Zero Allocation? | Lock-Free? | No I/O / Logging? | Key Notes / Exceptions |
|---|---|---|---|---|---|
| **ChurchOrgan Subsystem** (`ChurchOrganCore`, `Tonewheel`, `Pipe`, `Transistor`, `WorshipHybrid`, `RotaryCabinet`, `ScannerVibrato`, `OrganAmpCab`) | **YES (100%)** | **YES** | **YES** | **YES** | Exemplary real-time implementation. All buffers preallocated, fixed-capacity arrays, atomic parameter transfers, pure mathematical DSP. |
| **WorshipReverb** (`WorshipReverb.h`) | **YES (100%)** | **YES** | **YES** | **YES** | Completely preallocated in `prepare()`, `noexcept` process block, fixed FDN delay lines and circular diffusers, no heap operations. |
| **StereoLookaheadLimiter** (`StereoLookaheadLimiter.h`) | **YES (100%)** | **YES** | **YES** | **YES** | 8x oversampling filter, lookahead delay, and peak detection are strictly pre-sized in `prepare()`; zero callback allocations. |
| **ChannelToneShaper** (`ChannelToneShaper.h`) | **YES (100%)** | **YES** | **YES** | **YES** | Uses stack-allocated `IIR::ArrayCoefficients`, in-place coefficient updates without reallocating filter pointers. |
| **ChannelRotarySpeaker** (`ChannelRotarySpeaker.h`) | **YES (100%)** | **YES** | **YES** | **YES** | Preallocated circular buffer, smoothed interpolation, pure float/double state variables. |
| **MasterProcessor** (`MasterProcessor.h`) | **YES (100%)** | **YES** | **YES** | **YES** | Pre-reserved scratch buffers (`autoEqBuf`, `punchBuf`, `punchParallelBuf`), chunked processing, NaN recovery guards. |
| **PadSynthProcessor** (`PadSynthProcessor.h`) | **YES (100%)** | **YES** | **YES** | **YES** | Mathematical additive/subtractive synth engine, static event arrays (`RawMidiEvent, 256`), in-place reverb/chorus. |
| **SampleStreamer** (`SampleStream.h`) | **PARTIAL (90%)** | **YES (Audio)** | **YES** | **YES (Audio)** | Allocations/file reads offloaded to background thread; audio thread uses lock-free double buffering. System call on `wake()`. |
| **TimbresProcessor** (`TimbresProcessor.h`) | **PARTIAL (85%)** | **NO (Logging)** | **YES (Audio)** | **NO (Logging)** | Real-time audio rendering is clean, but `Logger::writeToLog` and `juce::String` allocations occur in instrumentation/self-test. |
| **PadProcessor** (`PadProcessor.h`) | **NO (50%)** | **NO** | **NO** | **YES** | **Violates hard real-time safety**: acquires `voiceLock` (`CriticalSection`) in `processBlock`; copies `std::shared_ptr`. |

---

## 5. Remediation Plan & Recommended Changes

### Recommendation 1: Eliminate `voiceLock` in `PadProcessor::processBlock`
- **Action**: Replace `juce::CriticalSection voiceLock` and `cacheLock` with lock-free atomic pointers (`std::atomic<juce::AudioBuffer<float>*>`) and an SPSC ring buffer (or atomic index ledger) for voice state updates.
- **Benefit**: Removes all priority inversion and thread-blocking risks from the continuous pad audio path.

### Recommendation 2: Remove or Asynchronously Queue Audio Thread Logging in `TimbresProcessor`
- **Action**: Remove `juce::Logger::writeToLog` and `juce::String` formatting from `TimbresProcessor::processBlock`. Instead, write raw numeric diagnostic metrics (`sumLoad`, `peakLoad`, `streamMisses`) into lock-free atomic variables or a lock-free SPSC diagnostics ring buffer, and format/log them on the Message Thread or a dedicated background logging timer.
- **Benefit**: Guarantees zero heap allocation and zero logging I/O in `TimbresProcessor::processBlock`.

### Recommendation 3: Transition `PadProcessor` and `SampleStreamer` to Raw / Unique Pointers with Background Deallocation Queues
- **Action**: Avoid copying `std::shared_ptr` inside the audio callback. Maintain raw buffer pointers in active voice slots, with ownership and destruction managed exclusively on background threads via lock-free pointer queues (`std::atomic<AudioBuffer<float>*>`).
- **Benefit**: Eliminates atomic reference-counting cache invalidation and ensures deallocation never occurs on the audio thread.
