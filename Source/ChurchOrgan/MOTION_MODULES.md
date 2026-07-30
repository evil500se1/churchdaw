# Church Organ Engine - Character and Motion DSP

Status: isolated clean-room implementation. These modules are not connected to
the ChurchDAW host or to `ChurchOrganCore` yet.

## Signal path

```text
dry organ
  -> ScannerVibrato (optional)
  -> OrganAmpCab
  -> RotaryCabinet (optional)
  -> protected stereo output
```

All three processors are C++17, JUCE-free, deterministic for equal state and
use fixed storage after `prepare()`. They contain no lock, heap operation,
filesystem access, logging or operating-system call in `process()`.

`setParameters()` is allocation-free and may be called on the audio thread.
The host must not call it concurrently from another thread. A future adapter
should deliver parameter snapshots or commands on the audio callback.

## ScannerVibrato

Purpose: scanner-specific vibrato/chorus rather than a generic stereo chorus.

Algorithm:

- one continuously rotating virtual contact;
- 18 calibrated phase-line taps;
- equal-power interpolation between adjacent contacts;
- four-point fractional-delay interpolation inside each tap;
- coherent left/right scanner position with deliberately limited phase spread;
- two-pole ageing bandwidth and optional deterministic low-level noise;
- smoothed rate, excursion, mix, spread, colour, noise and output.

Modes: `vibrato1`, `vibrato2`, `vibrato3`, `chorus1`, `chorus2`,
`chorus3`, and `bypass`. The `smooth` and `deep` characters adjust the
calibrated rate, excursion and bandwidth without referencing a commercial
product. Instantiate one processor per manual for independent Upper, Lower or
Pedal routing.

Nominal wet latency is 4.15 ms. Bypass is sample transparent when selected
before `prepare()`; live changes use a 28 ms wet crossfade.

`worshipIntensity` slightly deepens the phase line, warms its bandwidth and
reduces the rate. It intentionally avoids exaggerated modern stereo motion.

## OrganAmpCab

Purpose: transformer/preamp/power-stage colour and compact cabinet tone with
bounded level.

Algorithm:

- low-frequency tightening and frequency-dependent pre-emphasis;
- asymmetric dual-branch hyperbolic saturation;
- envelope-derived sag with separate attack/release;
- selectable 1x, 2x, 4x or 8x nonlinear processing;
- two-pole anti-alias filtering at the oversampled rate;
- post-nonlinearity DC blocker;
- two-pole cabinet softening, low body resonance and residual presence;
- optional static auto gain and a continuously differentiable peak knee.

Profiles are original and generic: `clean`, `warm`, `driven` and
`worshipSmooth`. Drive does not directly become an output gain. With default
peak protection, output is bounded below 0.985 full scale even for deliberately
over-range input. Oversampling is chosen in `prepare()` so quality changes do
not mutate render storage.

The objective 9 kHz nonlinear probe in the isolated test compares the aliases
at 3, 15 and 21 kHz between direct-rate and 8x paths. The acceptance gate
requires the 8x sum to be below 72 percent of direct-rate alias energy.

`worshipIntensity` adds modest warmth, sag and cabinet damping while preserving
headroom. The amp declares zero integer latency; its oversampled low-pass stages
still have their normal sub-sample group delay.

## RotaryCabinet

Purpose: stereo rotating cabinet with separate treble horn and bass rotor.

Algorithm:

- complementary crossover into independent horn and drum paths;
- continuous independent angle and speed state;
- natural exponential acceleration/deceleration, with a separate brake ramp;
- propagation delay derived from rotor radius, microphone angle and the speed
  of sound, producing speed-dependent Doppler shift;
- rotor-specific directional radiation for amplitude modulation;
- cubic fractional-delay reads for both virtual microphones;
- cabinet body, distance-dependent air loss and two short early-reflection
  paths;
- controlled preservation of an already-stereo input.

Modes: `stop`, `slow`, `fast`, and `brake`.

Perspectives: `stageClose`, `balanced`, `openRoom`, `vintageMono`, and
`wideWorship`. They are original parameter calibrations, not impulse responses
or copied response curves. Horn and drum expose independent slow/fast speed,
acceleration and deceleration. Other controls include crossover, Doppler,
directional amplitude, distance, width, resonance, air, early level, direction
and output.

The nominal propagation latency begins at 2.35 ms and increases by up to
1.35 ms with virtual microphone distance. The integrated early reflections are
short cabinet-space cues; main reverb remains a separate ChurchDAW effect.

`worshipIntensity` broadens the perspective, slightly relaxes rotor speed and
adds restrained distance/air/early energy.

## Isolated build and tests

```powershell
cmake -S Tests/ChurchOrgan/MotionModules `
      -B build-organ-motion-agent `
      -G "Visual Studio 17 2022" -A x64
cmake --build build-organ-motion-agent --config Release --parallel
ctest --test-dir build-organ-motion-agent -C Release --output-on-failure
```

The correctness executable covers:

- invalid prepare specifications;
- transparent scanner bypass;
- scanner stereo motion and bounded output;
- amp 8x alias reduction, DC rejection and extreme-input peak protection;
- independent rotary inertia, fast ramp, brake and stereo geometry;
- deterministic render and mono perspective;
- 44.1, 48, 96 and 192 kHz with invalid-sample sanitisation;
- zero allocations across scanner + amp + rotary `process()` calls.

The benchmark renders a 30-second worship chain at 48 kHz for mobile 2x,
desktop 4x and ultra 8x profiles.

## Known limits before product integration

- The modules have objective tests but still require calibrated listening,
  recordings and blinded perceptual review on studio monitors and headphones.
- Android and macOS CPU figures are not measured yet.
- The crossover is a low-cost complementary one-pole split; a later listening
  gate may justify a phase-aligned higher-order option in desktop/ultra modes.
- Oversampling changes require a safe host-level dual-instance transition if
  changed while audio is running.
- Scanner manual routing, preset migration, UI/MIDI mapping, suspend/resume and
  tail-preserving bypass belong to the future host adapter.
- No room reverb, worship pad layer or auto-mix is included here.
