# Church Organ Engine - Worship Layer and Auto Mix

Status: isolated clean-room implementation. No ChurchDAW host, shared core,
motion module, parameter catalog or root build file is modified by this phase.

## Signal contract

```text
sample-accurate MIDI/expression events
  -> fixed worship voice pool
  -> Pad / Air / Strings / Shimmer / Octave / Texture / Reverse Bloom
  -> per-layer HP/LP, width, level, ducking and reverb send
  -> Worship Intensity curves + loudness compensation
  -> protected stereo layer + protected stereo reverb send
  -> additive output inside the existing ChurchDAW channel
```

`WorshipLayerEngine::renderAdd()` adds its result to the channel buffers. It
does not consume another ChurchDAW channel and never retains any host pointer.
The layer signal is protected below 0.68 full scale and the auxiliary reverb
send below 0.50 full scale before either is added. The final channel/master
limiter remains the host's responsibility because the engine cannot know the
level already present in an additive destination buffer.

The generator has zero declared integer latency. Its one-pole spectral filters
have their normal frequency-dependent phase delay.

## Original synthetic components

No sample, impulse response or third-party response table is used.

- `Pad`: phase-continuous fundamental pair with quality-dependent micro-detune.
- `Air`: deterministic decorrelated noise plus a restrained pitched trace,
  followed by its configured band-pass.
- `Strings`: limited fundamental, second and third harmonic mixture. Harmonics
  are disabled before crossing the safe Nyquist margin.
- `Shimmer`: upper harmonic component whose macro curve begins only in the
  highest intensity region.
- `Octave`: coherent second harmonic support.
- `Texture`: deterministic noise and ring-like tonal interaction.
- `Reverse Bloom`: a squared continuous swell inside its normal attack
  envelope; no reversed sample is involved.

Every component exposes:

- enabled/level;
- attack and release;
- lowest/highest MIDI note;
- velocity and expression response;
- stereo width;
- high-pass and low-pass frequency;
- external ducking depth;
- reverb send.

Layer levels, filters, width and sends are smoothed. Attack/release coefficient
changes never reset a live envelope. Updating a preset on the same engine
therefore preserves held notes and releases; calling `reset()` intentionally
clears them.

## Voice and event model

The implementation owns 48 fixed voice records and a 512-event fixed queue.
The prepared quality profile exposes only its declared limit:

| Quality | Simultaneous voice slots |
| --- | ---: |
| Eco | 16 |
| Standard | 32 |
| Ultra | 48 |

Two quadrature oscillators are advanced by recurrence per voice. Their
rotations are precomputed in `prepare()`, and state is periodically normalised
without a square root. Note phases, pan and noise are deterministic from the
preset seed.

Note-on, note-off, all-notes-off and expression events have sample offsets.
Events beyond the current block retain the exact remaining offset. Note-off
enters each layer's release without removing the voice. If every slot is
occupied, the allocator first chooses the quietest release; only a completely
full held pool can require a direct steal.

## Worship Intensity

The public macro is continuous from zero to one. Smooth overlap regions create
the intended musical bands:

- 0-25 percent: the layer remains nearly dry, with only a trace of Pad.
- 25-55 percent: Pad, Octave, Texture and room send open progressively.
- 55-80 percent: Strings, Air, width and spectral opening become significant.
- 80-100 percent: Shimmer enters, Reverse Bloom reaches its range and static
  headroom compensation increases.

Each layer owns an independent monotonic smooth-step curve. Spectral opening,
width and reverb expansion are also monotonic. Loudness compensation decreases
continuously, while the exported compensated perceptual-energy target remains
monotonic. The rendered five-note objective probe is required to have no
greater than a 0.5 percent downward step between the tested macro positions.

The macro never changes note allocation, key range or persisted layer values.

## AutoMixContext

`AutoMixContext` cannot receive audio buffers by API. One instance belongs to
one ChurchDAW channel and accepts only four normalised control values:

- piano envelope;
- external pad envelope;
- external strings envelope;
- master-limit proximity.

It returns bounded scalar gains for:

- internal worship layer;
- organ low-mid;
- organ mid;
- drive;
- output;
- subtle solo room lift.

Control envelopes are smoothed before a second slow gain stage. Reaction is
clamped to at least 250 ms and recovery to at least 800 ms and at least 1.35
times the reaction time. Maximum attenuation is clamped to 6 dB, with a
conservative 3 dB default. A sidechain bit mask can disable individual control
sources. The engine never changes drawbars, MIDI state or audio owned by
another channel.

`WorshipLayerEngine::setAutoMixLayerGain()` accepts the resulting worship gain.
The host adapter can use the remaining gains in its existing tonal/output
stages without introducing cross-thread audio access.

## Real-time contract

After `prepare()`:

- no heap allocation or deallocation in render/control processing;
- no mutex, wait, atomic spin, file, network, log or operating-system call;
- no dynamic container;
- no audio pointer stored after a callback;
- every event, voice and filter state has compile-time capacity;
- invalid control and destination samples are sanitised;
- parameter/event mutation must occur on the audio thread or be delivered there
  by a future host command queue.

## Isolated build and validation

```powershell
cmake -S Tests/ChurchOrgan/WorshipPhase `
      -B build-organ-worship-agent `
      -G "Visual Studio 17 2022" -A x64
cmake --build build-organ-worship-agent --config Release --parallel
ctest --test-dir build-organ-worship-agent -C Release --output-on-failure
```

The correctness harness covers:

- sample-accurate note-on/note-off and offsets spanning blocks;
- deterministic stereo/audio-send output;
- all seven synthetic components;
- monotonic macro curves and rendered loudness;
- velocity, expression, ducking, AutoMix and key range;
- headroom, DC and band-limited spectral motion under 48-note stress;
- seamless five-second release;
- 44.1, 48, 96 and 192 kHz;
- fixed event overflow and voice limits;
- AutoMix bounds, recovery and anti-pumping response;
- zero allocations in render plus AutoMix advance.

The benchmark renders 30 seconds at 48 kHz with a 128-sample block for Eco,
Standard and Ultra profiles. It includes AutoMix advancement and stereo
reverb-send generation.

Reference Windows x64 Release measurement from the isolated harness:

| Profile | Held notes | One core | Realtime |
| --- | ---: | ---: | ---: |
| Eco | 10 | 4.92% | 20.31x |
| Standard | 16 | 6.45% | 15.50x |
| Ultra | 24 | 10.76% | 9.29x |

The engine plus AutoMix owns 22,368 bytes of fixed instance storage in this
build. These figures are machine-specific acceptance references, not mobile
hardware claims.

## Limits before product integration

- Objective validation does not replace calibrated listening, recording and
  blind perceptual review.
- CPU has not yet been measured on Android or macOS hardware.
- The synthetic filters are efficient one-pole sections; a future listening
  gate may justify higher-order Ultra-only spectral options.
- A completely full held voice pool can require a direct voice steal. Normal
  preset operation should keep polyphony below the selected fixed limit.
- The auxiliary output is a send only. Main worship reverb, shimmer feedback,
  long room tails and tail migration between two engine instances belong to
  the host/effects layer.
- AutoMix produces control gains only; frequency-zone application and
  tail-preserving preset transition must be integrated by the future host
  adapter.
