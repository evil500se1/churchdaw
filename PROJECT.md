# Project: Casio Memory Overhaul & Timbre Extraction

## Architecture
- Hardware Communication: USB MIDI SysEx Protocol over mido / Windows MM API
- Tone Patch Format: Casio CAT7 Tone Patch (.tn7), 31,776 bytes, single-pass CRC32
- Wave Bank Format: Casio CATU Wave Bank (.tu7), 1.966 GB (968 audio samples across 4 timbres), 48-byte descriptor stride alignment, zero gap before byte 42,288
- Staging Directory: `C:\Users\Administrator\MinhaDAW\Staging\CasioTone`

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Hardware Identification | Detect Casio USB-MIDI (Vendor 0x07CF, Product 0x6803) | M1 | survey |
| 2 | Sample Extraction & Conversion | Extract "Black Upright", "Pad 2", "Pad Rev Shimmer", "Cordas Suave" into .tn7/.tu7 | M2 | survey |
| 3 | Deep Memory Wipe | Format User Bank SysEx command (`F0 44 7F 7F 03 00 00 00 00 7F F7`) | M3 | survey |
| 4 | USB MIDI Bulk Payload Transfer | 256-byte block chunking, 512-byte nibbleization, 7-bit Casio checksum math | M3 | survey |
| 5 | Firmware UI Jailbreak | Reverse-engineer display firmware SysEx to unlock LCD/Touch UI or establish ROM security boundary | M4 | survey |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Hardware Identification & Capability | Detect keyboard hardware over USB MIDI SysEx | none | DONE |
| 2 | Multi-Timbre Extraction & Format Conversion | Convert audio samples to valid .tn7 and .tu7 binary payloads | M1 | DONE |
| 3 | Hardware Transfer Protocol & Deep Memory Wipe | Execute Format User Bank SysEx wipe and bulk upload payload over USB MIDI | M2 | DONE |
| 4 | Firmware UI Jailbreak & Visual Verification | Experimental SysEx display probe to unlock native LCD/Touch UI or prove ROM lock | M3 | DONE |

## Interface Contracts
### `convert_black_upright_casio.py` ↔ `transfer_casio_patch.py`
- Inputs: `Staging/CasioTone/black_upright_casio.tn7`, `Staging/CasioTone/blk_upright.tu7`
- Function signatures: `process_patch_file_transfer(file_path, category_id, file_name, out_port, in_port)`
- Error handling: Graceful offline fallback mode (`SIMULATION_VERIFIED` / `VERIFIED_SOFTWARE_ONLY`) when physical USB MIDI ports are disconnected.

## Code Layout
- `Tools/identify_casio.py`: Casio USB-MIDI identification probe script
- `Tools/convert_black_upright_casio.py`: Audio sample extraction and .tn7/.tu7 binary compilation script
- `Tools/transfer_casio_patch.py`: Deep memory wipe and bulk USB MIDI payload transfer script
- `Tools/probe_casio_jailbreak.py`: Experimental display firmware SysEx jailbreak probe script (Milestone 4)
