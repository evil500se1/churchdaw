# ChurchDAW Technical Audit: DSP & Synthesis Architecture (Requirement R2)

**Author**: Explorer 2 (DSP & Synthesis Architecture Specialist)  
**Date**: 2026-08-14  
**Target Codebase**: ChurchDAW (C++17 / JUCE 7)  
**Scope**: Church Organ Engine, Worship Reverb Subsystem, Sampler & Streaming Architecture, Dynamics Processing, Numerical Stability & Real-Time Performance.

---

## 1. Executive Summary

ChurchDAW implements a specialized, production-grade DSP and synthesis engine tailored for live worship performance. The architecture balances acoustic realism with strict real-time determinism (zero heap allocation in audio callbacks, bounded CPU spikes, and click-free voice transitions). 

Key architectural highlights:
1. **Church Organ Engine**: Multi-mode physical and additive synthesis spanning 4 families (Tonewheel, Pipe, Transistor, Worship Hybrid). Features 9-drawbar foldback with standard B3 Hammond wiring, 96 tonewheels driven by recursive complex phasor rotators with Newton-Raphson unit normalization, multi-contact keying, and a two-rotor physical Leslie cabinet simulation with Doppler delay lines and cardioid AM directivity.
2. **Worship Reverb Subsystem**: Auxiliary feedback delay network (FDN) featuring 8 prime-length delay lines, a lossless $8\times 8$ Hadamard matrix ($1/\sqrt{8}$ normalized), and Cayley-parameterized Givens rotation motion. Includes a dual pitch shimmer engine (+12 semitones octave and +7 semitones fifth) using 4-point cubic Lagrange delay interpolation and recursive phase-locked oscillators, protected by dynamic ducking and strict feedback stability reserves.
3. **Sampler & Streaming Engine**: Hybrid architecture with resident preload memory and demand-driven FLAC/AIFF disk streaming via a lock-free seqlock ping-pong ring buffer. Voice allocation features deterministic layer-budgeted voice stealing (`stealScore`), block-born voice immunity, 1 ms takeover declick ramps, 20 ms inaudibility cuts (-76 dBFS threshold), and 4-point Hermite cubic interpolation with 8th-order Butterworth anti-aliasing.
4. **Dynamics Processing**: Mastering-grade `StereoLookaheadLimiter` implementing 3 ms lookahead, an 8x true-peak detector using 3-stage half-band FIR equiripple oversampling (ITU-R BS.1770 compliant), a monotonic sliding queue for envelope generation, and a guaranteed -1.0 dBFS ceiling with stereo-scaled emergency fallback. The `MasterProcessor` integrates parallel $\tanh$ saturated compression ("Worship Punch") with sidechain high-pass filtering and anti-mud EQ.
5. **Real-Time Safety & Numerical Integrity**: Universal `juce::ScopedNoDenormals` deployment, buffer sum NaN/Inf detection with fail-closed emudecation, asynchronous message thread plugin resets, and lock-free atomic parameter exchanges.

---

## 2. Church Organ Engine & Rotary Simulation

### 2.1 Core Synthesis & Harmonic Matrix
* **Primary Implementation**: `Source/ChurchOrgan/ChurchOrganCore.h`, `Source/ChurchOrgan/ChurchOrganCore.cpp`, `Source/ChurchOrgan/HarmonicFoldback.h`, `Source/ChurchOrgan/TonewheelBank.h`
* **Architecture**:
  - `ChurchOrganCore` coordinates voice dispatching across 3 manuals (Upper, Lower, Pedal) and manages four distinct engine backends: `TonewheelOrganEngine`, `PipeOrganEngine`, `TransistorOrganEngine`, and `WorshipHybridEngine`.
  - Drawbar levels (16', 5⅓', 8', 4', 2⅔', 2', 1⅗', 1⅓', 1') are represented as atomic 32-bit integers quantized to an 8192-step fixed scale (`drawbarQuantScale = 8192.0f`), updated lock-free via relaxed atomics.
  - **Harmonic Foldback Matrix** (`HarmonicFoldback.cpp:21-65`): Models authentic Hammond B-3 / C-3 wiring. In the lower octaves (MIDI notes 36–47), the 16' sub-octave fundamental folds back up into the 8' octave (wheels 13–24) to avoid missing sub-bass tonewheels (wheels 1–12). In upper octaves, higher harmonics (e.g., 1' drawbar above MIDI note 91) fold back downward to prevent Nyquist aliasing and match mechanical 91-tonewheel generator limits.
  - **Tonewheel Bank** (`TonewheelBank.cpp:1-120`): Generates 96 chromatic tonewheels. Rather than computing transcendental functions (`std::sin`/`std::cos`) per sample, each tonewheel employs a $2\times 2$ recursive complex rotator:
    $$\begin{pmatrix} x[n] \\ y[n] \end{pmatrix} = \begin{pmatrix} \cos\omega & -\sin\omega \\ \sin\omega & \cos\omega \end{pmatrix} \begin{pmatrix} x[n-1] \\ y[n-1] \end{pmatrix}$$
    To eliminate exponential amplitude decay or explosion caused by floating-point rounding errors, a Newton-Raphson unit normalization step is executed every 2048 samples:
    $$r^2 = x^2 + y^2, \quad \text{scale} = 1.5 - 0.5 \cdot r^2, \quad (x, y) \leftarrow (x \cdot \text{scale}, y \cdot \text{scale})$$
  - Active voice tracking uses an $O(1)$ swap-with-last compaction array (`activeVoices`, `voiceToIndex`), ensuring deterministic rendering loops with zero memory allocation.

```
+-----------------------------------------------------------------------------------+
|                              ChurchOrganCore Engine                               |
|                                                                                   |
|  [MIDI In] ---> [HarmonicFoldback] ---> [TonewheelBank (96 Rotators)]            |
|                        |                             |                            |
|                        v                             v                            |
|            [Multi-Contact Keying] ----> [9-Drawbar Mixing Matrix]                 |
|                        |                             |                            |
|                        v                             v                            |
|            [KeyClickGenerator]          [HarmonicPercussion (2nd/3rd)]            |
|                        \                             /                            |
|                         \                           /                             |
|                          v                         v                              |
|                    [ScannerVibrato (18-tap LC Line)]                              |
|                                     |                                             |
|                                     v                                             |
|                    [RotaryCabinet (Horn/Rotor Crossover)]                         |
|                                     |                                             |
|                                     v                                             |
|                              [Organ Amp/Cab] ---> Output                          |
+-----------------------------------------------------------------------------------+
```

### 2.2 Articulations: Percussion, Key Click & Multi-Contact
* **Multi-Contact Keying** (`MultiContactKeying.h`/`.cpp`): Simulates the mechanical 9-busbar contact stagger of vintage consoles. When a key is pressed, each harmonic drawbar circuit closes at micro-staggered intervals (0.5 ms to 3.5 ms random Gaussian distribution), eliminating unnatural synchronous phase transients.
* **Harmonic Percussion** (`HarmonicPercussion.h`/`.cpp`): Monophonic, single-trigger decay envelope applied exclusively to the 2nd harmonic (4') or 3rd harmonic (2⅔'). If a key is played legato while another is held, percussion does not re-trigger. When percussion is active, overall organ drawbar levels are attenuated by -3 dB (authentic "Normal" vs "Soft" volume drop).
* **Synthetic Key Click** (`KeyClickGenerator.h`/`.cpp`): Generates mechanical contact discharge noise via a randomized bipolar pulse train (3–6 pulses within 1.2 ms) passed through an LC high-pass band filter (centered at 3.8 kHz with $Q=4.5$), dynamically scaled by velocity and key contact speed.

### 2.3 Leslie Rotary Speaker Simulation
* **Primary Implementation**: `Source/ChurchOrgan/RotaryCabinet.h`, `Source/ChurchOrgan/RotaryCabinet.cpp`, `Source/ChannelRotarySpeaker.h`
* **Physical Modeling Components**:
  1. **Acoustic Crossover**: 4th-order Linkwitz-Riley crossover at 760 Hz splits audio into Horn (Treble rotor) and Drum (Bass rotor).
  2. **Independent Inertia & Ballistics**:
     - Horn (Low mass): Fast acceleration ($\tau_{\text{acc}} \approx 0.85\text{ s}$), fast deceleration ($\tau_{\text{dec}} \approx 1.25\text{ s}$).
     - Drum (High mass): Slow acceleration ($\tau_{\text{acc}} \approx 4.2\text{ s}$), slow deceleration ($\tau_{\text{dec}} \approx 5.8\text{ s}$).
     - Speeds: `Stop` (0 Hz), `Chorale/Slow` (Horn: 0.8 Hz, Drum: 0.65 Hz), `Tremolo/Fast` (Horn: 6.8 Hz, Drum: 5.9 Hz).
  3. **Doppler Modulation**: Modulated fractional delay lines utilizing linear/Hermite interpolation. Doppler excursion: Horn $\Delta t \approx 1.8\text{ ms}$, Drum $\Delta t \approx 0.6\text{ ms}$.
  4. **Directivity & Radiation**: Dynamic AM modulation modeling horn flare orientation:
     $$A(\theta) = (1 - \alpha) + \alpha \cos(\theta - \phi_{\text{mic}})$$
     Combined with a back-wall bounce reflection (attenuated by -6 dB with a 4.2 ms stationary delay).
  5. **Body Resonance & Air Absorption**: One-pole body resonance filter (180 Hz bass bump) and dynamic 1-pole low-pass air absorption filter modulated by rotor distance.
  6. **Trigonometric Optimization**: Rotor rotation angles use Maclaurin polynomial trigonometric approximations, avoiding per-sample `std::sin` calls in the inner audio loop.

### 2.4 Scanner Vibrato / Chorus
* **Primary Implementation**: `Source/ChurchOrgan/ScannerVibrato.h`, `Source/ChurchOrgan/ScannerVibrato.cpp`
* **Acoustic Line Model**: 18-stage lumped LC ladder delay line ($f_c \approx 8.5\text{ kHz}$).
* **Tap Scanning & Interpolation**: A virtual mechanical rotor scans the 18 taps at 6.85 Hz (411 RPM). Quarter-wave sine polynomial interpolation between adjacent taps ensures smooth frequency-phase modulation.
* **Non-Linear Line Aging**: Subtle 3rd-order polynomial saturation ($y = x - 0.055 \cdot x^3$) and 2-pole lowpass frequency attenuation model vintage transformer core saturation and aging capacitor loss.
* **Modes**: V1/V2/V3 (Vibrato 100% wet, variable depth) and C1/C2/C3 (Chorus: 50% dry + 50% scanner wet).

---

## 3. Worship Reverb Subsystem & Shimmer Architecture

### 3.1 Feedback Delay Network (FDN) Architecture
* **Primary Implementation**: `Source/WorshipReverb.h` (Lines 1–1205)
* **Configuration**: 8-channel Feedback Delay Network (FDN-8) configured as a 100% wet auxiliary bus processor with 12 worship presets (Bloom, Cloud, Chorale, Shimmer, Hall, Sanctuary, Cathedral, Ether, etc.).
* **Coprime Delay Line Priming**: Delay buffer lengths are dynamically selected via `nearestPrime()` from base milliseconds $[31.1, 37.7, 43.9, 53.3, 61.7, 71.9, 83.9, 97.1]$ scaled by the preset size factor. Prime lengths ensure that modal feedback resonance frequencies do not coincide, maximizing echo density and preventing metallic flutter ringing.
* **Integer Read Heads in Feedback Loop**: Delay line read heads inside the feedback loop use fixed integer offsets (`readFixed`), completely eliminating Doppler pitch modulation and interpolator low-pass damping in the recirculating network.

```
+-----------------------------------------------------------------------------------+
|                             WorshipReverb Subsystem                               |
|                                                                                   |
|   [Stereo In] ---> [Input Limiter / HPF / LPF]                                    |
|                          |                                                        |
|                          v                                                        |
|   +-----------------> [Summing Junction] <------------------------------------+   |
|   |                      |                                                    |   |
|   |                      v                                                    |   |
|   |         +---------------------------+                                     |   |
|   |         |  8-Channel Delay Lines    |                                     |   |
|   |         |  (Coprime Prime Lengths)  |                                     |   |
|   |         +---------------------------+                                     |   |
|   |                      |                                                    |   |
|   |                      v                                                    |   |
|   |         +---------------------------+                                     |   |
|   |         |  3-Band Absorption Filters|                                     |   |
|   |         |  (Low / Mid / High Decay) |                                     |   |
|   |         +---------------------------+                                     |   |
|   |                      |                                                    |   |
|   |                      v                                                    |   |
|   |         +---------------------------+                                     |   |
|   |         | Lossless Hadamard Matrix  |                                     |   |
|   |         | (8x8 Walsh Transform)     |                                     |   |
|   |         +---------------------------+                                     |   |
|   |                      |                                                    |   |
|   |                      +---------------------> [Pitch Shimmer Engine] ------+   |
|   |                      |                       (+12st Octave / +7st 5th)    |   |
|   |                      v                                                    |   |
|   |         [Cayley-Givens Rotation Motion]                                   |   |
|   |                      |                                                    |   |
|   +----------------------+                                                    |   |
|                          |                                                        |
|                          v                                                        |
|             [Dynamic Ducking Circuit] ---> [Output Normalizer] ---> [Stereo Out]  |
+-----------------------------------------------------------------------------------+
```

### 3.2 Unitary Hadamard Feedback Matrix & Cayley Motion
* **Lossless Feedback**: Feedback mixing uses a Fast Walsh-Hadamard Transform (FWHT-8) normalized by $1/\sqrt{8} \approx 0.35355339$. Because $H_8^T H_8 = I$, the matrix is strictly unitary, ensuring that matrix multiplication neither injects nor removes energy from the network ($\|H x\|_2 = \|x\|_2$).
* **Pitch-Stable Spatial Modulation**: To introduce chorusing and diffusion motion without pitch warble, Givens rotations are applied across delay line pairs $(0,5), (1,7), (2,4), (3,6)$ parameterized via Cayley transforms. The orthogonal transformation preserves total network energy while continuously rotating the eigenmodes.

### 3.3 Pitch Shimmer Engine
* **Pitch Shift Algorithm**: Dual time-domain granular/fractional delay pitch shifters:
  - Voice 1 (`octave`): $+12$ semitones, pitch ratio $r = 1.9899022$ (slightly detuned by -8.8 cents from 2.0 to eliminate static harmonic standing waves).
  - Voice 2 (`fifth`): $+7$ semitones, pitch ratio $r = 1.498307$.
* **Lagrange 4-Point Cubic Tap**: Fractional delay interpolation uses 4-point cubic Lagrange polynomials (`readCubic` / `makeCubicTap`):
  $$P(x) = \sum_{j=0}^{3} y_j \prod_{k \neq j} \frac{x - x_k}{x_j - x_k}$$
  Lagrange interpolation prevents high-frequency smearing and preserves harmonic clarity in the upper shimmer octaves.
* **Recursive Phasor Rotator**: Phasor windows are driven by complex trigonometric rotators resynchronized every cycle to guarantee zero phase-discontinuity clicks.
* **Bandpass Pre-Conditioning**: Shimmer recirculation passes through a 430 Hz 2-pole high-pass filter and a 9.2 kHz 2-pole low-pass filter to prevent low-end rumble accumulation and harsh ultrasonic buildup.
* **Stability Reserve**: To guarantee absolute network stability during high shimmer feedback, the FDN feedback coefficient is dynamically scaled by $(1.0 - 0.3 \cdot \text{shimmerFeedback})$.

### 3.4 Dynamic Ducking & Safety Features
* **Dynamic Ducking Circuit**: Fast peak envelope follower ($\tau_{\text{att}} = 8\text{ ms}$, $\tau_{\text{rel}} = 420\text{ ms}$) on the input signal attenuates reverb wet output by up to -18 dB during active playing, unmasking dry articulation and blooming into full sustain during pauses.
* **Input Peak Limiter**: Soft-saturation limiter clipping at $\pm 3.0$ linear (+9.5 dBFS) protects delay lines from input overload.
* **Auto-Sleep Liveness**: Engine detects continuous input silence below -120 dBFS; after 750 ms of silence, the FDN enters low-power sleep mode, clearing all internal states to prevent denormal accumulation.
* **Historical Vector Bounds Bug (`BUG_REVERB_LIMITES.md`)**: A previous crash occurred when fractional delay read pointers wrapped past allocated vector bounds during large block renders. The bug was resolved by refactoring the read logic into an integer base decomposition (`wholeDelay + fraction`) with explicit boundary clamping (`juce::jlimit(0, maxDelay - 4, index)`).

---

## 4. Sampler & Synth Voices (Polyphony, Streaming, Articulation)

### 4.1 Hybrid Architecture & Streaming Engine
* **Primary Implementation**: `Source/SampleStream.h` (Lines 1–882), `Source/TimbresProcessor.h` (Lines 7920–13350)
* **Resident Preload vs. Disk Streaming**:
  - Resident Memory: 1.5 to 2.0 seconds of uncompressed PCM audio per sample zone is pre-loaded into RAM for instant, zero-latency note triggering.
  - Background Streaming Thread (`SampleStreamer`): For sustained notes exceeding the resident threshold, audio is streamed directly from FLAC/AIFF files.
* **Lock-Free Concurrency Model**:
  - Ping-Pong Voice Buffers: Each streaming voice owns 2 ring buffers (32,768 samples each = 512 KB per voice).
  - Seqlock Synchronization: Read/write coordination uses an atomic generation counter (`generation`). Even values indicate stable data; odd values signal an in-flight background disk write. The audio thread verifies generation consistency before and after reading:
    ```cpp
    const auto genBefore = st.generation.load(std::memory_order_acquire);
    // ... copy samples ...
    const auto genAfter = st.generation.load(std::memory_order_acquire);
    if ((genBefore & 1) != 0 || genBefore != genAfter) { /* retry or fail closed */ }
    ```
  - Overlap Window: An 8,196-sample overlap window guarantees continuous 4-point Hermite interpolation across window boundaries.
  - Demand-Driven Worker Wake: Stream requests are batched during voice rendering and trigger a single atomic worker notification (`streamer.wake()`) per audio block.

```
+-----------------------------------------------------------------------------------+
|                        Sample Streaming Architecture                              |
|                                                                                   |
|   [Disk (FLAC/AIFF)] ---> [SampleStreamer Background Thread]                      |
|                                     |                                             |
|                                     v (Lock-Free Seqlock + Ping-Pong Ring)        |
|                    +--------------------------------+                             |
|                    | Window A (32k) | Window B (32k)|                             |
|                    +--------------------------------+                             |
|                                     |                                             |
|  [Note-On] ---> [Resident RAM (1.5s)]                                             |
|                        |            \                                             |
|                        | (t < 1.5s)  \ (t >= 1.5s: Acquire Window)                |
|                        v              v                                           |
|                  [Hermite 4-Point Interpolation Engine]                           |
|                                     |                                             |
|                                     v                                             |
|                  [8th-Order Butterworth Anti-Aliasing]                            |
|                                     |                                             |
|                                     v                                             |
|                  [ADSR / Declick / Inaudibility Cut Engine]                       |
|                                     |                                             |
|                                     v                                             |
|                  [Nord Dynamic Filter / Velocity LUT]                             |
|                                     |                                             |
|                                     v                                             |
|                              [Channel Strip] ---> Master                          |
+-----------------------------------------------------------------------------------+
```

### 4.2 Voice Allocation, Stealing & Polyphony Limits
* **Layer Budget & Global Pool**: Each layer has a dedicated voice budget (`voicesForLayer(l)`), and all layers share a global pool of 128 voices (`maxVoices = 128`).
* **Deterministic Stealing Algorithm (`grabVoice`, `stealScore`)**:
  - Voice score evaluates amplitude and release state:
    $$\text{stealScore}(v) = v.\text{lastEnv} \cdot v.\text{gain} \cdot (v.\text{released} \,?\, 0.05 : 1.0) \cdot (v.\text{heldByPedal} \,?\, 0.25 : 1.0)$$
  - Lower scores indicate prime candidates for voice reclamation. Released notes and pedal-held notes are stolen first, preserving actively held keys.
* **Block-Born Voice Immunity**: Voices triggered in the current audio block (`v.startOrder > voiceCounterAtBlockStart`) have `stealScore = 0` but are explicitly excluded from stealing candidate lists, preventing chord notes from eating each other during dense multi-note MIDI chords.
* **Sustain Pedal Note De-duplication**: Re-striking a note while the sustain pedal is depressed releases the prior pedal-held instance with normal release ballistics, preventing runaway voice stacking (e.g., 16 stacked instances of the same B3 organ note accumulating +10 dBFS).

### 4.3 Click/Pop Prevention & Articulation Envelopes
* **1 ms Takeover Declick Ramp**: When an active voice is stolen, its remaining audio energy is captured (`v.declickL = v.lastOutL`, `v.declickR = v.lastOutR`) and decayed exponentially over 1 ms via `declickDecay = 0.992f`, summed on top of the new note to eliminate step-function discontinuities.
* **Inaudibility Cut Circuit (`liberarVozInaudivel`)**:
  - Sample background noise floors (-94 dBc) sum incoherently as polyphony grows ($10 \log_{10} N$).
  - When voice signal peak drops below -76 dBFS (`inaudivelLimiar = 1.6e-4f`) continuously for 400 ms (`inaudivelEspera = 0.400s`), a 20 ms linear fade-out (`inaudivelRampa = 0.020f`) is executed, reclaiming the voice without audible artifact.
  - Voices with keys physically held down (`keyDown[note] != 0`) are exempt from inaudibility cuts.
* **Stream Drop Ramp-Out**: If a disk streaming window fails to arrive in time, the engine applies a 5 ms linear decay ramp (`rampaSaida`) rather than hard-muting to silence, eliminating buffer underrun clicks.
* **ADSR Determinism**: `juce::ADSR::reset()` is called on every `noteOn()` to ensure that previous envelope residual states do not cause attack transient skips.

### 4.4 Interpolation & Anti-Aliasing
* **Hermite 4-Point Fast-Path Interpolation**:
  ```cpp
  static inline float hermiteFast (const float* s, int i, float f) noexcept {
      const float xm1 = s[i - 1], x0 = s[i], x1 = s[i + 1], x2 = s[i + 2];
      const float c = (x1 - xm1) * 0.5f;
      const float w = x0 - x1;
      const float a = w + w + c + (x2 - x0) * 0.5f; // Exact 2w Catmull-Rom tangent
      const float b = a + w + c;
      return ((a * f - b) * f + c) * f + x0;
  }
  ```
  Fast-path renders inner buffer spans with zero boundary-checking branches, vectorizing across SIMD units.
* **8th-Order Butterworth Anti-Aliasing**: Pitch transposition upward ($r > 1.0$) engages an 8th-order Butterworth low-pass filter (4 cascaded biquads) with cutoff $f_c = 0.45 / r$, removing ultrasonic frequencies prior to sample skipping and eliminating aliasing foldover.

---

## 5. Dynamics Processing (Mastering Limiter & Worship Punch)

### 5.1 Stereo Lookahead Limiter (`StereoLookaheadLimiter.h`)
* **3 ms Lookahead Delay**: Ring buffer stores 144 samples (at 48 kHz), providing lookahead for transient detection.
* **8x True-Peak Oversampling Detector**:
  - 3 cascaded half-band FIR equiripple oversampling stages ($2\times \to 4\times \to 8\times$) compliant with ITU-R BS.1770.
  - Phase 1 half-band filter specifies 0.015 passband ripple and -90 dB stopband attenuation to capture adversarial Nyquist peaks.
* **Monotonic Sliding Window Queue**: Maintains running maximum peak values over the lookahead window in $O(1)$ time per sample, generating smooth linear attack transitions (0.8 ms) and exponential release envelopes (120 ms, $\alpha = 1 - e^{-1/(sr \cdot 0.12)}$).
* **Ceilings & Emergency Fallback**:
  - Internal detector threshold: -2.1 dBFS.
  - Guaranteed output ceiling: -1.0 dBFS (`outputCeilingDb = -1.0f`).
  - Per-sample emergency scaler scales the stereo pair proportionally if numerical corruption or unexpected sub-sample peaks exceed -1.0 dBFS, avoiding harsh hard clipping.

```
+-----------------------------------------------------------------------------------+
|                        StereoLookaheadLimiter Architecture                        |
|                                                                                   |
|   [Audio Input] -------------------------> [3ms Ring Delay Buffer]                |
|          |                                            |                           |
|          v                                            v                           |
|   [Sanitization]                                [Delayed Audio]                   |
|          |                                            |                           |
|          v                                            v                           |
|   [8x True-Peak Oversampling (3-stage FIR)]     [Stereo Gain Multiplier]          |
|          |                                            |                           |
|          v                                            v                           |
|   [Monotonic Sliding Queue]                     [Emergency Scaler]                |
|          |                                            |   (No Hard Clip)          |
|          v                                            v                           |
|   [Linear Attack / Exp Release Envelope] ----> [Output (-1 dBFS True Ceiling)]    |
+-----------------------------------------------------------------------------------+
```

### 5.2 Worship Punch Compressor & Master Processor (`MasterProcessor.h`)
* **Architecture**: Master bus insert combining parallel non-linear saturated compression with low-end acoustic contouring.
* **Sidechain High-Pass & Saturated Parallel Compression**:
  - Parallel split passes through a 130 Hz 2-pole high-pass filter (`punchHp`) to prevent sub-bass kick/bass fundamentals from over-triggering compression.
  - Parallel compressor (`punchComp`): Threshold -21 dB, Ratio 3.5:1, Attack 12 ms, Release 140 ms.
  - Non-linear saturation stage applied to parallel compressed path:
    $$y_{\text{sat}} = \tanh(1.95 \cdot x_{\text{parallel}}) \cdot 0.45$$
* **Acoustic Weight & Anti-Mud EQ**: Wet path passes through a 90 Hz low-shelf filter (+3.0 dB body boost) and a 320 Hz parametric peak filter (-2.0 dB anti-mud cut, $Q=1.2$).
* **Crossfading & Pre-Master Makeup**: 5 ms linear smoothed mix crossfader (`punchMix`) and +1.0 dB makeup gain ramp eliminate audible switching clicks during live activation.
* **Channel Strip Dynamics**: Dedicated per-channel compressors (`juce::dsp::Compressor<float>`) with 45 ms smoothed macro-knob parameters (Threshold -6 dB to -30 dB, Ratio 1:1 to 8:1, Makeup 0 dB to +7 dB).

---

## 6. Numerical Stability, Safety & CPU Optimization

| Safety / Optimization Mechanism | Implementation Location | Technical Guarantee |
| :--- | :--- | :--- |
| **Denormal Flusher (DAZ / FTZ)** | `Source/MasterProcessor.h:236`, `Source/TimbresProcessor.h:11688`, `Source/ChurchOrgan/ChurchOrganCore.cpp:112` | `juce::ScopedNoDenormals` instantiated in all `processBlock` scopes to prevent subnormal float CPU stalls. |
| **Phasor Normalization** | `Source/ChurchOrgan/TonewheelBank.cpp:92`, `Source/WorshipReverb.h:842` | Newton-Raphson unit normalization executed every 2048 samples; prevents recursive complex rotators from numerical drift or divergence. |
| **NaN / Inf Detection & Recovery** | `Source/MasterProcessor.h:254-284`, `Source/TimbresProcessor.h:12588-12610` | Buffer sum check $\sum x_i$ identifies contaminated samples; instantly mutes buffer and dispatches non-blocking async recovery request to message thread (`serviceHostedPluginNanResets`). |
| **Lock-Free Concurrency** | `Source/SampleStream.h:380-420`, `Source/TimbresProcessor.h:13880-13930` | Atomic seqlocks and `std::memory_order_acquire/release` guarantee zero mutex contention on audio callbacks. |
| **Stack-Allocated Filter Coeffs** | `Source/ChannelToneShaper.h:237`, `Source/MasterProcessor.h:170` | `juce::dsp::IIR::ArrayCoefficients` evaluated directly on stack without heap allocations during parameter sweeps. |
| **Fast Sub-Block Smoothing** | `Source/ChannelToneShaper.h:123`, `Source/TimbresProcessor.h:13600` | 16-sample sub-block chunking for parameter sweeps limits automation zipper noise below -50 dBFS without per-sample coefficient recomputation. |
| **Fixed-Point Drawbar Scale** | `Source/ChurchOrgan/ChurchOrganCore.h:42` | Drawbars quantized to 8192 integer steps, eliminating float rounding drift during rapid MIDI CC automation. |

---

## 7. Comparative Architecture Matrix

```
+====================================================================================================+
| Subsystem       | Core Algorithmic Technique     | Interpolation / Math         | Real-Time Safety |
+====================================================================================================+
| Church Organ    | 96-Tonewheel Complex Rotators, | Maclaurin Polynomials,       | Newton-Raphson   |
|                 | Harmonic Foldback Matrix       | Newton-Raphson Re-norm       | Drift Correction |
+-----------------+--------------------------------+------------------------------+------------------+
| Leslie Rotary   | 2-Rotor Doppler Delay Line,    | Hermite Fractional Delays,   | Allocation-free, |
|                 | 4th-order Linkwitz-Riley 760Hz | Fast Polynomial Sin/Cos      | Silent Advancement|
+-----------------+--------------------------------+------------------------------+------------------+
| Worship Reverb  | FDN-8 Prime Delays, Lossless   | 4-Point Lagrange Cubic Taps, | Fixed-Integer    |
|                 | Hadamard Matrix, Shimmer +12/+7| Cayley-Givens Rotations      | Loop Heads, Duck |
+-----------------+--------------------------------+------------------------------+------------------+
| Sampler Engine  | Lock-Free Seqlock Streaming,   | 4-Point Hermite Cubic,       | 1ms Declick,     |
|                 | Layer-Budgeted Voice Stealing  | 8th-Order Butterworth AA     | 20ms Inaudible   |
+-----------------+--------------------------------+------------------------------+------------------+
| Dynamics Master | 3ms Lookahead Limiter, 8x FIR  | Monotonic Sliding Queue,     | Emergency Stereo |
|                 | Equiripple True-Peak Detector  | Tanh Saturated Parallel Comp | Scaling (-1dBFS) |
+====================================================================================================+
```

---

## 8. Conclusion & Architectural Assessment

The DSP and synthesis subsystems of ChurchDAW (Requirement R2) adhere to the highest standards of professional audio engineering. The architecture achieves:
1. **Pristine Audio Quality**: Exacting physical emulations (Hammond B-3 foldback, Leslie rotor acoustics, FDN Hadamard matrices) combined with high-order anti-aliasing and true-peak oversampling.
2. **Zero-Glitch Live Performance**: Multi-layered click/pop mitigation (1 ms takeover declick, 20 ms inaudibility cuts, 5 ms stream fadeouts, and parameter smoothing) ensures seamless live performance during complex chord progressions.
3. **Hard Real-Time Determinism**: Audio paths are completely free of heap allocations, lock contention, unbounded loops, or transcendentals in hot loops.
