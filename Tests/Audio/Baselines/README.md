# PadSynth canonical audio baseline

`padsynth-v1-48k.cdpcap` is the immutable, pre-optimization reference for the
deterministic PadSynth scenario. It was generated from the production DSP on
macOS arm64 with Apple Clang 21.0.0 in a Release `-O3 -DNDEBUG` build.

- Format: ChurchDAW deterministic capture v2, little-endian float32.
- Scenario: v1, 48 kHz, stereo, 131072 frames, canonical block size 128.
- Size: 1048624 bytes.
- SHA-256: `14091235134b378621bb33a43958f5fa7f0ad40c4fd4f256d4547dda22609c6c`.
- Content fingerprint: `fdb2f8f5388ee9ba`.

The registered CTest never generates or updates this file. The probe also
refuses to overwrite an existing baseline. A replacement must therefore be an
explicit, reviewed operation after preserving the old artifact.

## Gates

Repeated renders and the 128/256/512-sample renders from one executable must
be raw-float bit identical, including the distinction between `+0.0f` and
`-0.0f`.

Cross-toolchain comparison with this arm64 baseline permits at most:

- maximum absolute error: `3.0e-5` (about -90.46 dBFS);
- null RMS: `2.0e-6` (about -113.98 dBFS).

These limits are slightly above the measured arm64/x86_64 differences
(`2.6822e-5` max and `1.712e-6` RMS, about -115.33 dBFS null) while remaining
tight enough to reject an audible DSP change.

The capture stores the musically active deterministic segment. During every
self-test, processing continues normally to 851968 frames (17.749 seconds).
There is no test-only engine/effect bypass, reset, panic, or clear in this
horizon, and production keeps its original `1.0e-7` idle-tail policy unchanged.

The final one-second stuck-voice/effect-tail diagnostic is deliberately
separate from the musical-equivalence gates above. It accepts bitwise zero, or
on another architecture a finite numerical residue no greater than:

- peak: `2.5e-7` (about -132.04 dBFS);
- RMS: `5.0e-8` (about -146.02 dBFS).

The final RMS must also be at least 80 dB below the measured released-tail
region, and no sample in the final window may exceed the peak floor. The limits
cover the measured x86_64/Rosetta JUCE-reverb residue (`1.824544e-7` peak,
`3.76727e-8` RMS) with a small fixed margin. They do not relax same-build,
128/256/512 bit identity or the canonical musical baseline contract.

The probe enforces the versioned identity (scenario 1, 48 kHz, stereo, 128
canonical block size, 131072 frames, 1048624 bytes, fingerprint
`fdb2f8f5388ee9ba`). The artifact SHA-256 is also fixed by this contract:
`14091235134b378621bb33a43958f5fa7f0ad40c4fd4f256d4547dda22609c6c`.
Changing any of these values requires an explicit reviewed baseline-contract
update; tests never regenerate this file implicitly.

The infrastructure self-test verifies the SHA through JUCE from both ASCII
and Unicode filesystem paths. Windows uses the native UTF-16 path; POSIX paths
are decoded explicitly as UTF-8, avoiding locale-dependent `path.string()`.

## Explicit creation command

After the infrastructure and non-baseline self-tests pass, and only when the
file does not exist:

```sh
ChurchDAWPadSynthRenderProbe \
  --generate-baseline Tests/Audio/Baselines/padsynth-v1-48k.cdpcap
```
