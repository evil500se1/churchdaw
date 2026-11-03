# Specifications & Analysis Report: Native ChurchDAW .sfz & .json Instrument Metadata

**Agent Identity**: `spec_miner_m2_2`  
**Milestone**: M2_2 — Native SFZ & JSON Specification Mining for Ultimate Pianos 2  
**Staging Inventory**: `C:\Users\Administrator\MinhaDAW\Staging\UltimatePianos2\inventory.json`  
**Target Specification Standard**: ChurchDAW Catalog v1 Standard (`Tools/ValidateTimbrePack.py`, `Packaging/BuildTimbreCatalog.ps1`)  

---

## 1. Executive Summary

This document establishes the authoritative specifications for converting and integrating **28 KEEP instruments** from the *Ultimate Pianos 2* Kontakt library into ChurchDAW's native sample format.

### Key Inventory Metrics
- **Total Instruments Discovered**: 38 instruments across 7 NKX containers (~12.53 GB raw payload).
- **Deduplicated Instruments (SKIP)**: 10 instruments matched existing ChurchDAW piano models (e.g. Amber Upright, Felt Upright, Soft Grand, White Grand, etc.).
- **New Native Instruments (KEEP)**: **28 instruments** (24 Piano/EP/Keyboard models, 4 Ambient Pad/Drone models).
- **Total Sample Files**: 20,447 WAV audio samples to be converted and mapped.
- **Key Range Standard**: 100% continuous key coverage `[0..127]` with zero gaps across both `.sfz` and `.json` mappings.
- **Ableton Pitch Standard**: C3 = 60 MIDI note convention across all sample naming and mapping definitions.

---

## 2. Native `.sfz` Opcode Format Specification

ChurchDAW instruments rely on a clean, standardized subset of the SFZ v1 format parsed by ChurchDAW's internal sampler and validated by `ValidateTimbrePack.py`.

### 2.1 File Placement & Naming
- **File Location**: `<Category>/<InstrumentName>/<InstrumentName>.sfz`
- **Example**: `Pianos/Astoria Grand XL/Astoria Grand XL.sfz` or `Pads/up2_pad_angelic/up2_pad_angelic.sfz`

### 2.2 Header & Group Opcodes
Every `.sfz` file must begin with a `<group>` block defining global envelope and playback behavior:

```sfz
<group>
ampeg_attack=<ATTACK_SECONDS>
ampeg_release=<RELEASE_SECONDS>
loop_mode=<LOOP_MODE>
```

#### Opcode Definitions:
- `ampeg_attack`: Attack time in seconds (e.g., `0.001` for pianos/EPs, `0.35` for pads).
- `ampeg_release`: Release time in seconds (e.g., `1.20` for pianos/EPs, `2.50` for pads).
- `loop_mode`: Optional. Set to `loop_continuous` for looped ambient pads; omitted for decay instruments (pianos/EPs).

### 2.3 Region Opcodes
Following `<group>`, each sample mapping region is declared using single-line `<region>` definitions:

```sfz
<region> sample=<FILENAME> pitch_keycenter=<MIDI_ROOT> lokey=<LOKEY> hikey=<HIKEY> lovel=<LOVEL> hivel=<HIVEL>
```

#### Required Opcodes & Syntax Rules:
1. `sample`: Exact relative filename of the 44.1kHz 24-bit PCM WAV audio file (e.g. `sample=Astoria Grand XL-A-1-V042-01.wav`).
2. `pitch_keycenter`: Integer MIDI note number of the un-pitched root sample ($0 \le 	ext{pitch\_keycenter} \le 127$).
3. `lokey`: Integer MIDI note number for the bottom of the key trigger range ($0 \le 	ext{lokey} \le 127$).
4. `hikey`: Integer MIDI note number for the top of the key trigger range ($0 \le 	ext{hikey} \le 127$).
5. `lovel`: Integer MIDI velocity lower bound ($0 \le 	ext{lovel} \le 127$).
6. `hivel`: Integer MIDI velocity upper bound ($0 \le 	ext{hivel} \le 127$).

### 2.4 Continuous Key Coverage `[0..127]` Rule
To prevent unmapped keys (dead notes on keyboards):
1. **Sorted Order**: Regions must be sorted primarily by `lokey` ascending, then `lovel` ascending.
2. **Bottom Bound Extension**: The region(s) corresponding to the lowest sampled root ($R_0$) MUST have `lokey=0`.
3. **Top Bound Extension**: The region(s) corresponding to the highest sampled root ($R_{K-1}$) MUST have `hikey=127`.
4. **Midpoint Boundary Division**: For adjacent root notes $R_i$ and $R_{i+1}$, the boundary key is calculated as:
   $$	ext{boundary\_high}_i = \lfloor rac{R_i + R_{i+1}}{2} floor$$
   $$	ext{lokey}_{i+1} = 	ext{boundary\_high}_i + 1$$
5. **Pitch Validity**: For every region, $lokey \le pitch\_keycenter \le hikey$ MUST hold.

---

## 3. Native `.json` Metadata Schema Specification

The JSON metadata file provides ChurchDAW UI, preset loading, and asset indexing with complete instrument attributes.

### 3.1 JSON Root Schema

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": [
    "name",
    "raw_label",
    "category",
    "install_relative",
    "sample_count",
    "samples"
  ],
  "properties": {
    "name": { "type": "string", "description": "Exact display name in DAW catalog UI" },
    "raw_label": { "type": "string", "description": "Full catalog raw label" },
    "category": { "type": "string", "enum": ["PIANOS", "PADS"] },
    "install_relative": { "type": "string", "description": "Relative path in ChurchDAW installation" },
    "default_attack": { "type": "number", "description": "Default envelope attack in seconds" },
    "default_release": { "type": "number", "description": "Default envelope release in seconds" },
    "loop_enabled": { "type": "boolean", "description": "True if pad loop continuous" },
    "sample_count": { "type": "integer", "description": "Total number of sample mappings" },
    "samples": {
      "type": "array",
      "items": {
        "type": "object",
        "required": [
          "file",
          "midi_root",
          "note_name",
          "key_low",
          "key_high",
          "vel_low",
          "vel_high"
        ],
        "properties": {
          "file": { "type": "string" },
          "midi_root": { "type": "integer", "minimum": 0, "maximum": 127 },
          "note_name": { "type": "string" },
          "key_low": { "type": "integer", "minimum": 0, "maximum": 127 },
          "key_high": { "type": "integer", "minimum": 0, "maximum": 127 },
          "vel_low": { "type": "integer", "minimum": 0, "maximum": 127 },
          "vel_high": { "type": "integer", "minimum": 0, "maximum": 127 }
        }
      }
    }
  }
}
```

---

## 4. Ableton Pitch Standard & WAV Naming Compliance

ChurchDAW enforces Ableton pitch convention (**C3 = 60**) and sample filename parsing via `TimbresProcessor::parseSampleName()`.

### 4.1 Ableton Pitch Formula
$$	ext{MIDI Note} = (	ext{Octave} + 2) 	imes 12 + 	ext{Semitone Offset}$$
Where semitones are: C=0, C#=1, D=2, D#=3, E=4, F=5, F#=6, G=7, G#=8, A=9, A#=10, B=11.

- `C3` = $(3 + 2) 	imes 12 + 0 = 60$
- `A-1` = $(-1 + 2) 	imes 12 + 9 = 21$
- `B6` = $(6 + 2) 	imes 12 + 11 = 107$

### 4.2 Standard Sample Filename Format
```
<InstrumentName>-<NoteName>-V<Velocity>-<Index>.wav
```
*Example*: `Astoria Grand XL-A-1-V042-01.wav`

---

## 5. Complete Specification Matrix for All 28 KEEP Instruments

The table below outlines the mined specification parameters for all 28 KEEP instruments:

| # | Instrument Name | Category | Source Container | Install Relative Path | Samples | Root Notes | Root Range | Lowest Key Range | Highest Key Range | Attack | Release | Loop |
|---|-----------------|----------|------------------|-----------------------|---------|------------|------------|------------------|-------------------|--------|---------|------|
| 1 | Arm Drone | PADS | Pads | `Pads/Arm Drone` | 1 | 1 | `[60..60]` | `[0..127] (Root 60)` | `[0..127] (Root 60)` | 0.35s | 2.5s | `loop_continuous` |
| 2 | Astoria Grand XL | PIANOS | Pianos | `Pianos/Astoria Grand XL` | 1077 | 65 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 3 | Bright Grand XL | PIANOS | Pianos | `Pianos/Bright Grand XL` | 981 | 72 | `[21..107]` | `[0..21] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 4 | DX Full Tines | PIANOS | Pianos | `Pianos/DX Full Tines` | 828 | 37 | `[21..105]` | `[0..22] (Root 21)` | `[105..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 5 | Drone Ocean View | PADS | Pads | `Pads/Drone Ocean View` | 12 | 1 | `[60..60]` | `[0..127] (Root 60)` | `[0..127] (Root 60)` | 0.35s | 2.5s | `loop_continuous` |
| 6 | EP1 Low Deep | PIANOS | Pianos | `Pianos/EP1 Low Deep` | 378 | 17 | `[26..105]` | `[0..30] (Root 26)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 7 | EP2 Dyno My Rhodes | PIANOS | Pianos | `Pianos/EP2 Dyno My Rhodes` | 334 | 16 | `[26..105]` | `[0..30] (Root 26)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 8 | EP3 Sparkletop '67 | PIANOS | Pianos | `Pianos/EP3 Sparkletop '67` | 300 | 16 | `[26..105]` | `[0..30] (Root 26)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 9 | EP4 Mark I | PIANOS | Pianos | `Pianos/EP4 Mark I` | 396 | 16 | `[26..105]` | `[0..30] (Root 26)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 10 | EP5 Bright | PIANOS | Pianos | `Pianos/EP5 Bright` | 612 | 28 | `[25..104]` | `[0..27] (Root 25)` | `[102..127] (Root 104)` | 0.001s | 1.2s | `none` |
| 11 | EP6 Vintage '67 | PIANOS | Pianos | `Pianos/EP6 Vintage '67` | 680 | 30 | `[24..105]` | `[0..27] (Root 24)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 12 | EP8 Nefertiti | PIANOS | Pianos | `Pianos/EP8 Nefertiti` | 966 | 44 | `[24..105]` | `[0..27] (Root 24)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 13 | Grand Imperial XL | PIANOS | Pianos | `Pianos/Grand Imperial XL` | 2915 | 85 | `[21..107]` | `[0..21] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 14 | Grand Lady D L | PIANOS | Pianos | `Pianos/Grand Lady D L` | 369 | 32 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 15 | HonkyTonk Upright L | PIANOS | Pianos | `Pianos/HonkyTonk Upright L` | 236 | 28 | `[22..107]` | `[0..23] (Root 22)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 16 | Italian Grand XL | PIANOS | Pianos | `Pianos/Italian Grand XL` | 916 | 64 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 17 | Mellow Upright XL | PIANOS | Pianos | `Pianos/Mellow Upright XL` | 899 | 62 | `[22..107]` | `[0..23] (Root 22)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 18 | Queen Upright L | PIANOS | Pianos | `Pianos/Queen Upright L` | 333 | 28 | `[21..107]` | `[0..22] (Root 21)` | `[106..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 19 | Rain Piano L | PIANOS | Pianos | `Pianos/Rain Piano L` | 261 | 25 | `[23..105]` | `[0..25] (Root 23)` | `[104..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 20 | Romantic Upright L | PIANOS | Pianos | `Pianos/Romantic Upright L` | 387 | 31 | `[22..107]` | `[0..24] (Root 22)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 21 | Royal Grand 3D XL | PIANOS | Pianos | `Pianos/Royal Grand 3D XL` | 991 | 67 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 22 | Saloon Upright L | PIANOS | Pianos | `Pianos/Saloon Upright L` | 310 | 27 | `[23..105]` | `[0..25] (Root 23)` | `[103..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 23 | Silver Grand XL | PIANOS | Pianos | `Pianos/Silver Grand XL` | 919 | 63 | `[21..108]` | `[0..22] (Root 21)` | `[108..127] (Root 108)` | 0.001s | 1.2s | `none` |
| 24 | Studio Grand 1 L | PIANOS | Pianos | `Pianos/Studio Grand 1 L` | 1177 | 66 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 25 | Velvet Grand XL | PIANOS | Pianos | `Pianos/Velvet Grand XL` | 967 | 66 | `[21..107]` | `[0..22] (Root 21)` | `[107..127] (Root 107)` | 0.001s | 1.2s | `none` |
| 26 | Wurlitzer 1 | PIANOS | Pianos | `Pianos/Wurlitzer 1` | 1074 | 49 | `[25..105]` | `[0..25] (Root 25)` | `[105..127] (Root 105)` | 0.001s | 1.2s | `none` |
| 27 | up2_pad_angelic | PADS | Pads | `Pads/up2_pad_angelic` | 168 | 28 | `[24..105]` | `[0..25] (Root 24)` | `[104..127] (Root 105)` | 0.35s | 2.5s | `loop_continuous` |
| 28 | up2_pad_space | PADS | Pads | `Pads/up2_pad_space` | 112 | 28 | `[24..105]` | `[0..25] (Root 24)` | `[104..127] (Root 105)` | 0.35s | 2.5s | `loop_continuous` |

---

## 6. Mined Deduplication Matrix (10 SKIP Instruments)

The 10 instruments listed below were identified in the staging inventory as duplicates of existing ChurchDAW catalog piano models and are skipped from conversion:

| # | Instrument Name | Category | Container | Matched ChurchDAW Model | Deduplication Action |
|---|-----------------|----------|-----------|-------------------------|----------------------|
| 1 | Amber Upright XL | PIANOS | Upright A.nkx | `Amber Upright` | **SKIP** |
| 2 | Ballad EP1 | PIANOS | UP2 Grand C and Digital.nkx | `EP Ballad` | **SKIP** |
| 3 | Black Upright L | PIANOS | Upright B.nkx | `Black Upright` | **SKIP** |
| 4 | EP7 Tines Amped | PIANOS | UP2 Electric.nkx | `Tines MK I` | **SKIP** |
| 5 | EP9 Stockholm | PIANOS | UP2 Electric.nkx | `EP Stockholm` | **SKIP** |
| 6 | Felt Upright XL | PIANOS | Upright A.nkx | `Felt Upright` | **SKIP** |
| 7 | Grand Upright XL | PIANOS | Upright B.nkx | `Grand Upright` | **SKIP** |
| 8 | Pearl Upright XL | PIANOS | Upright A.nkx | `Pearl Upright` | **SKIP** |
| 9 | Soft Grand XL | PIANOS | UP2 Grand A.nkx | `Soft Grand` | **SKIP** |
| 10 | White Grand XL | PIANOS | UP2 Grand A.nkx | `White Grand` | **SKIP** |

---

## 7. Discovered Edge Cases & Special Handling

| # | Feature / Instrument | Input Scenario | Mined Behavior & Specification |
|---|----------------------|----------------|--------------------------------|
| 1 | Single-Sample Pad (`Arm Drone`) | Single root at C3 (60) | Extend `key_low=0`, `key_high=127`, `vel_low=0`, `vel_high=127`. Set `loop_enabled=true`. |
| 2 | Drone Collection (`Drone Ocean View`) | 12 drone WAVs named by pitch (A#, A, B, C#, C, D#, D, E, F#, F, G#, G) | Map notes A3..G#3 (roots 60..71). Divide key range [0..127] into 12 continuous bands. Rename files to `Drone Ocean View-<Note>-V127-01.wav`. |
| 3 | Negative Octave Parsing (`A-1`, `A#-1`, `B-1`) | Keys 21, 22, 23 | MIDI note conversion must correctly evaluate `octave=-1`. Filename splitter must handle leading hyphen in `-1` without splitting note token incorrectly. |
| 4 | Kontakt NCW Compression | Staging audio files in `.ncw` format | Must be decompressed and converted to 24-bit 44.1kHz uncompressed PCM WAV audio during conversion. |
| 5 | Artwork File Exclusion | `Background.png`, `Wallpaper.jpg`, `.nki` patches in staging | Strictly exclude from download packs and target folders per `ValidateTimbrePack.py` Rule 1. |

---

## 8. Features Discovered Table

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | Mapping | Continuous SFZ Key Coverage | Generates `<region>` opcodes ensuring keys 0 to 127 trigger audio without dead keys | Root notes, velocity layers | Valid `.sfz` file | Rejects gaps or out-of-bound pitch centers | `ValidateTimbrePack.py` |
| 2 | Metadata | Native JSON Instrument Manifest | Creates catalog `.json` metadata file per instrument | Sample list, envelope params | Valid `.json` metadata | Fails validation if sample references broken | `ValidateTimbrePack.py` |
| 3 | Catalog | Download Pack Packaging | Bundles instrument `.sfz`, `.json`, and `.wav` files into `.zip` download pack | Instrument directory | Pack `.zip` & SHA-256 hash | Rejects forbidden artwork files (.png/.jpg) | `BuildTimbreCatalog.ps1` |
| 4 | Pitch | Ableton C3=60 Convention | Maps note names (e.g. C3=60, A-1=21) to MIDI note numbers | Pitch string stem | MIDI integer [0..127] | Returns -1 on invalid note strings | `TimbresProcessor::parseSampleName` |

---

## 9. Verification & Validation Protocol

Each converted instrument must be validated using `Tools/ValidateTimbrePack.py`:

```powershell
python Tools/ValidateTimbrePack.py --pack "Pianos/Astoria Grand XL"
```

### Expected Output:
- `Check 1: 0 artwork/unwanted files found.`
- `Check 2: 0 broken links found.`
- `Check 3: All WAV filenames comply with parseSampleName() (C3 = 60).`
- `Check 4: All WAV files are valid 44.1kHz, 24-bit PCM audio.`
- `Check 5: Key range covers [0..127] continuously with 0 gaps in SFZ and JSON.`
- `RESULT: 100% PASS - Timbre pack is valid!`
