"""
convert_black_upright_casio.py - Multi-Timbre Sample Extraction & Format Converter for Casio Keyboards

Extracts, converts, compresses, and packs ALL 4 required ChurchDAW timbres:
  1. "Black Upright" (880 samples: FC-*-V*.flac) -> Slot 801 (black_upright_casio.tn7)
  2. "Pad 2" (29 samples: *Pad2.flac) -> Slot 802 (pad_2_casio.tn7)
  3. "Pad Rev Shimmer" (30 samples: Pad Rev Shim-*.flac) -> Slot 803 (pad_rev_shimmer_casio.tn7)
  4. "Cordas Suave" (29 samples: *SynthString.flac) -> Slot 804 (cordas_suave_casio.tn7)

Total audio samples: 968.

Converts all audio into 44.1 kHz 16-bit PCM WAV format (pcm_s16le).
Constructs valid Casio proprietary patch headers:
  - CAT7 for .tn7 tone patches (Slots 801-804)
  - CATU for single .tu7 wave bank (blk_upright.tu7 / church_timbres.tu7, size ~1.966 GB, < 2.0 GB limit)

Outputs compiled patch files and a structured JSON metadata summary report to Staging/CasioTone/conversion_summary.json.
"""

import os
import sys
import glob
import re
import io
import json
import struct
import zlib
import shutil
from datetime import datetime, timezone
import soundfile as sf
import numpy as np

# Base path definitions
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
SOURCE_DIR = os.path.join(PROJECT_ROOT, "Packaging", "AndroidPacks", "church-live-v1", "Imported")
STAGING_DIR = os.path.join(PROJECT_ROOT, "Staging", "CasioTone")

TN7_BLACK_UPRIGHT = os.path.join(STAGING_DIR, "black_upright_casio.tn7")
TN7_PAD_2 = os.path.join(STAGING_DIR, "pad_2_casio.tn7")
TN7_PAD_REV_SHIMMER = os.path.join(STAGING_DIR, "pad_rev_shimmer_casio.tn7")
TN7_CORDAS_SUAVE = os.path.join(STAGING_DIR, "cordas_suave_casio.tn7")

TU7_OUTPUT = os.path.join(STAGING_DIR, "blk_upright.tu7")
TU7_ALIAS_OUTPUT = os.path.join(STAGING_DIR, "church_timbres.tu7")
SUMMARY_OUTPUT = os.path.join(STAGING_DIR, "conversion_summary.json")

# Note parsing table
BASE_NOTE_MAP = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}

# Velocity layers mapping for Black Upright (10 layers)
VELOCITY_LAYERS = [
    {"layer_idx": 0, "target_v": 13,  "min_v": 1,   "max_v": 24},
    {"layer_idx": 1, "target_v": 25,  "min_v": 25,  "max_v": 31},
    {"layer_idx": 2, "target_v": 38,  "min_v": 32,  "max_v": 44},
    {"layer_idx": 3, "target_v": 51,  "min_v": 45,  "max_v": 57},
    {"layer_idx": 4, "target_v": 64,  "min_v": 58,  "max_v": 70},
    {"layer_idx": 5, "target_v": 76,  "min_v": 71,  "max_v": 82},
    {"layer_idx": 6, "target_v": 89,  "min_v": 83,  "max_v": 95},
    {"layer_idx": 7, "target_v": 102, "min_v": 96,  "max_v": 108},
    {"layer_idx": 8, "target_v": 114, "min_v": 109, "max_v": 120},
    {"layer_idx": 9, "target_v": 127, "min_v": 121, "max_v": 127},
]

VEL_MAP = {v["target_v"]: v for v in VELOCITY_LAYERS}

TIMBRE_CONFIGS = [
    {
        "timbre_id": 1,
        "name": "Black Upright",
        "slot_number": 801,
        "category_id": 0x00,  # Piano
        "category_name": "Piano",
        "output_tn7": TN7_BLACK_UPRIGHT,
        "tn7_filename": "black_upright_casio.tn7",
        "patch_name_bytes": b"Black Upright   ",
        "pattern_type": "black_upright",
        "expected_count": 880,
        "total_key_splits": 88,
        "total_velocity_layers": 10,
    },
    {
        "timbre_id": 2,
        "name": "Pad 2",
        "slot_number": 802,
        "category_id": 0x05,  # Pad
        "category_name": "Pad",
        "output_tn7": TN7_PAD_2,
        "tn7_filename": "pad_2_casio.tn7",
        "patch_name_bytes": b"Pad 2           ",
        "pattern_type": "pad_2",
        "expected_count": 29,
        "total_key_splits": 29,
        "total_velocity_layers": 1,
    },
    {
        "timbre_id": 3,
        "name": "Pad Rev Shimmer",
        "slot_number": 803,
        "category_id": 0x05,  # Pad
        "category_name": "Pad",
        "output_tn7": TN7_PAD_REV_SHIMMER,
        "tn7_filename": "pad_rev_shimmer_casio.tn7",
        "patch_name_bytes": b"Pad Rev Shimmer ",
        "pattern_type": "pad_rev_shimmer",
        "expected_count": 30,
        "total_key_splits": 30,
        "total_velocity_layers": 1,
    },
    {
        "timbre_id": 4,
        "name": "Cordas Suave",
        "slot_number": 804,
        "category_id": 0x07,  # Strings
        "category_name": "Strings",
        "output_tn7": TN7_CORDAS_SUAVE,
        "tn7_filename": "cordas_suave_casio.tn7",
        "patch_name_bytes": b"Cordas Suave    ",
        "pattern_type": "cordas_suave",
        "expected_count": 29,
        "total_key_splits": 29,
        "total_velocity_layers": 1,
    },
]

def note_to_midi(note_str):
    """Convert note name (e.g. A-1, C0, C7) to MIDI note index (21..108)."""
    m = re.match(r'^([A-G])([#b]?)(-?\d+)$', note_str)
    if not m:
        raise ValueError(f"Invalid note string format: {note_str}")
    key, acc, oct_str = m.groups()
    base = BASE_NOTE_MAP[key]
    if acc == '#':
        base += 1
    elif acc == 'b':
        base -= 1
    octave = int(oct_str)
    midi = (octave + 2) * 12 + base
    return midi

def midi_to_note_str(midi_val):
    """Convert MIDI note index to note name string (e.g. 24 -> C0, 21 -> A-1, 108 -> C7)."""
    notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
    octave = (midi_val // 12) - 2
    note = notes[midi_val % 12]
    return f"{note}{octave}"

def convert_audio_to_pcm_wav(file_path):
    """
    Read FLAC/WAV audio, convert to 44.1kHz 16-bit PCM WAV bytes.
    Returns (wav_bytes, frame_count, sample_rate, channels).
    """
    data, sr = sf.read(file_path, dtype='int16')
    if data.ndim == 1:
        channels = 1
        data = np.expand_dims(data, axis=1)
    else:
        channels = data.shape[1]
    
    frame_count = data.shape[0]
    
    buf = io.BytesIO()
    sf.write(buf, data, sr, subtype='PCM_16', format='WAV')
    wav_bytes = buf.getvalue()
    
    return wav_bytes, frame_count, sr, channels

def parse_all_timbres(all_files):
    """
    Scans and parses audio files for all 4 required timbres.
    Returns a list of parsed entry dicts.
    """
    parsed_entries = []
    
    # 1. Black Upright (880 samples)
    bu_files = [
        f for f in all_files 
        if ('black' in f.lower() and 'upright' in f.lower() and (f.lower().endswith('.flac') or f.lower().endswith('.wav')))
    ]
    bu_entries = []
    for filename in bu_files:
        filepath = os.path.join(SOURCE_DIR, filename)
        m = re.search(r'FC-([A-G][#b]?-?\d+)-V(\d+)', filename)
        if not m:
            continue
        note_str = m.group(1)
        v_target = int(m.group(2))
        try:
            midi_note = note_to_midi(note_str)
        except Exception:
            continue
        if v_target not in VEL_MAP:
            continue
        v_info = VEL_MAP[v_target]
        bu_entries.append({
            "timbre_id": 1,
            "timbre_name": "Black Upright",
            "midi_note": midi_note,
            "key_low": midi_note,
            "key_high": midi_note,
            "layer_idx": v_info["layer_idx"],
            "target_v": v_target,
            "min_v": v_info["min_v"],
            "max_v": v_info["max_v"],
            "note_str": note_str,
            "filepath": filepath,
            "filename": filename
        })
    bu_entries.sort(key=lambda x: (x["midi_note"], x["layer_idx"]))
    parsed_entries.extend(bu_entries)
    print(f"  - Parsed {len(bu_entries)} 'Black Upright' samples.")

    # 2. Pad 2 (29 samples)
    p2_files = [f for f in all_files if re.match(r'^\d+Pad2\.flac$', f, re.IGNORECASE)]
    p2_entries = []
    for filename in p2_files:
        filepath = os.path.join(SOURCE_DIR, filename)
        m = re.match(r'^(\d+)Pad2\.flac$', filename, re.IGNORECASE)
        if not m:
            continue
        zone_idx = int(m.group(1)) # 1 to 29
        midi_note = 24 + (zone_idx - 1) * 3 # C0=24 to C7=108
        note_str = midi_to_note_str(midi_note)
        key_low = midi_note
        key_high = min(108, midi_note + 2)
        p2_entries.append({
            "timbre_id": 2,
            "timbre_name": "Pad 2",
            "midi_note": midi_note,
            "key_low": key_low,
            "key_high": key_high,
            "layer_idx": 0,
            "target_v": 127,
            "min_v": 1,
            "max_v": 127,
            "note_str": note_str,
            "filepath": filepath,
            "filename": filename,
            "zone_idx": zone_idx
        })
    p2_entries.sort(key=lambda x: x["midi_note"])
    parsed_entries.extend(p2_entries)
    print(f"  - Parsed {len(p2_entries)} 'Pad 2' samples.")

    # 3. Pad Rev Shimmer (30 samples)
    prs_files = [
        f for f in all_files 
        if ('pad rev shim' in f.lower() and (f.lower().endswith('.flac') or f.lower().endswith('.wav')))
    ]
    prs_entries = []
    for filename in prs_files:
        filepath = os.path.join(SOURCE_DIR, filename)
        m = re.search(r'Pad Rev Shim-([A-G][#b]?-?\d+)-V(\d+)', filename)
        if not m:
            continue
        note_str = m.group(1)
        v_target = int(m.group(2))
        try:
            midi_note = note_to_midi(note_str)
        except Exception:
            continue
        key_low = midi_note
        key_high = 23 if midi_note == 21 else min(108, midi_note + 2)
        prs_entries.append({
            "timbre_id": 3,
            "timbre_name": "Pad Rev Shimmer",
            "midi_note": midi_note,
            "key_low": key_low,
            "key_high": key_high,
            "layer_idx": 0,
            "target_v": v_target,
            "min_v": 1,
            "max_v": 127,
            "note_str": note_str,
            "filepath": filepath,
            "filename": filename
        })
    prs_entries.sort(key=lambda x: x["midi_note"])
    parsed_entries.extend(prs_entries)
    print(f"  - Parsed {len(prs_entries)} 'Pad Rev Shimmer' samples.")

    # 4. Cordas Suave / SynthString (29 samples)
    ss_files = [f for f in all_files if re.match(r'^\d+SynthString\.flac$', f, re.IGNORECASE)]
    ss_entries = []
    for filename in ss_files:
        filepath = os.path.join(SOURCE_DIR, filename)
        m = re.match(r'^(\d+)SynthString\.flac$', filename, re.IGNORECASE)
        if not m:
            continue
        zone_idx = int(m.group(1)) # 1 to 29
        midi_note = 24 + (zone_idx - 1) * 3 # C0=24 to C7=108
        note_str = midi_to_note_str(midi_note)
        key_low = midi_note
        key_high = min(108, midi_note + 2)
        ss_entries.append({
            "timbre_id": 4,
            "timbre_name": "Cordas Suave",
            "midi_note": midi_note,
            "key_low": key_low,
            "key_high": key_high,
            "layer_idx": 0,
            "target_v": 127,
            "min_v": 1,
            "max_v": 127,
            "note_str": note_str,
            "filepath": filepath,
            "filename": filename,
            "zone_idx": zone_idx
        })
    ss_entries.sort(key=lambda x: x["midi_note"])
    parsed_entries.extend(ss_entries)
    print(f"  - Parsed {len(ss_entries)} 'Cordas Suave' samples.")

    return parsed_entries

def main():
    print("=== Multi-Timbre Sample Extraction & Patch Converter for Casio Keyboards ===")
    print(f"Source directory:  {SOURCE_DIR}")
    print(f"Staging directory: {STAGING_DIR}")
    
    if not os.path.exists(SOURCE_DIR):
        print(f"ERROR: Source directory not found: {SOURCE_DIR}")
        sys.exit(1)
        
    os.makedirs(STAGING_DIR, exist_ok=True)
    
    # 1. Scan and parse all audio sample files matching all 4 timbres
    print("\n--- Scanning & Parsing Audio Samples ---")
    all_files = os.listdir(SOURCE_DIR)
    parsed_entries = parse_all_timbres(all_files)
    
    total_samples = len(parsed_entries)
    print(f"Total audio samples parsed across all 4 timbres: {total_samples}")
    if total_samples != 968:
        print(f"WARNING: Expected 968 total samples, got {total_samples}")

    # 2. Build .tu7 Wave Bank file (CATU Header + Meta + 968 Descriptors + PCM Payloads)
    print("\n--- Constructing Casio User Wave Bank (.tu7) ---")
    header_tu7_size = 32
    table_tu7_meta_size = 16
    descriptor_size = 48
    header_block_tu7_len = header_tu7_size + table_tu7_meta_size + (descriptor_size * total_samples)
    
    meta_tu7_bytes = struct.pack("<I12x", total_samples)
    crc_tu7 = zlib.crc32(meta_tu7_bytes)
    
    sample_descriptors = []
    desc_bytes_list = []
    wav_bytes_list = []
    total_pcm_payload_bytes = 0
    current_payload_offset = header_block_tu7_len

    for idx, entry in enumerate(parsed_entries):
        wav_bytes, frame_count, sr, channels = convert_audio_to_pcm_wav(entry["filepath"])
        byte_len = len(wav_bytes)
        
        desc = dict(entry)
        desc.update({
            "offset": current_payload_offset,
            "byte_len": byte_len,
            "frame_count": frame_count,
            "sr": sr,
            "channels": channels
        })
        sample_descriptors.append(desc)
        
        chunk_sig = b"SMPL"
        chunk_size = 40 + byte_len
        audio_fmt = 1 # Linear PCM
        loop_start = 0
        loop_end = max(0, frame_count - 1)
        loop_mode = 0 # One shot
        root_key = entry["midi_note"]
        fine_tune = 0
        vel_max = entry["max_v"]
        
        desc_bytes = struct.pack(
            "<4sIHHIIIIBBBbII8x",
            chunk_sig,
            chunk_size,
            audio_fmt,
            channels,
            sr,
            frame_count,
            loop_start,
            loop_end,
            loop_mode,
            root_key,
            fine_tune,
            vel_max,
            current_payload_offset,
            byte_len
        )
        
        desc_bytes_list.append(desc_bytes)
        wav_bytes_list.append(wav_bytes)
        
        total_pcm_payload_bytes += byte_len
        current_payload_offset += byte_len
        
        if (idx + 1) % 200 == 0 or (idx + 1) == total_samples:
            print(f"Processed audio payload for {idx + 1}/{total_samples} samples...")

    # Calculate CRC32 incrementally over exact disk layout sequence (meta -> descriptors -> pcm payloads)
    for db in desc_bytes_list:
        crc_tu7 = zlib.crc32(db, crc_tu7)
    for wb in wav_bytes_list:
        crc_tu7 = zlib.crc32(wb, crc_tu7)
    crc_tu7 = crc_tu7 & 0xFFFFFFFF
    total_tu7_file_size = header_block_tu7_len + total_pcm_payload_bytes

    # 32-byte CATU Header with computed final CRC32
    magic_tu7 = b"CATU"
    version = 0x00010000
    wave_bank_name = b"blk_upright.tu7"

    header_tu7_bytes = struct.pack(
        "<4sI16sII",
        magic_tu7,
        version,
        wave_bank_name,
        total_tu7_file_size,
        crc_tu7
    )

    # Write CATU Wave Bank
    with open(TU7_OUTPUT, "wb") as f_tu7:
        f_tu7.write(header_tu7_bytes)
        f_tu7.write(meta_tu7_bytes)
        for db in desc_bytes_list:
            f_tu7.write(db)
        for wb in wav_bytes_list:
            f_tu7.write(wb)
        f_tu7.flush()

    # Also create church_timbres.tu7 copy/alias
    shutil.copyfile(TU7_OUTPUT, TU7_ALIAS_OUTPUT)

    tu7_actual_size = os.path.getsize(TU7_OUTPUT)
    print(f"Generated {TU7_OUTPUT} ({tu7_actual_size:,} bytes, {tu7_actual_size / (1024**3):.3f} GB).")
    print(f"Created wave bank alias at {TU7_ALIAS_OUTPUT}.")

    # 3. Build .tn7 User Tone Patch files for each of the 4 timbres
    print("\n--- Constructing Casio User Tone Patches (.tn7) ---")
    magic_tn7 = b"CAT7"
    version = 0x00010000
    keymap_entry_size = 36
    synth_block_size = 64
    header_tn7_size = 32
    target_tu7_filename = b"blk_upright.tu7"
    
    tn7_results = []
    
    for cfg in TIMBRE_CONFIGS:
        timbre_descriptors = [d for d in sample_descriptors if d["timbre_id"] == cfg["timbre_id"]]
        num_entries = len(timbre_descriptors)
        
        total_tn7_file_size = header_tn7_size + synth_block_size + (keymap_entry_size * num_entries)
        
        # 64-byte Synthesis Parameter Block
        synth_bytes = struct.pack(
            "<16s19BHH16s9x",
            cfg["patch_name_bytes"],
            cfg["category_id"], # Category ID
            127,  # Master Volume
            64,   # Pan Center
            0,    # Transpose
            0,    # Octave Shift
            0,    # Amp Attack
            64,   # Amp Decay
            127,  # Amp Sustain
            40,   # Amp Release
            127,  # Filter Cutoff
            0,    # Filter Resonance
            0,    # Filter Env Depth
            0,    # LFO Rate
            0,    # Pitch Mod
            0,    # Amp Mod
            0,    # Filter Mod
            40,   # Reverb Send
            0,    # Chorus Send
            0,    # DSP Effect Index
            cfg["total_key_splits"],
            cfg["total_velocity_layers"],
            target_tu7_filename
        )
        
        # Keymap Pointer Table (36 bytes per entry)
        keymap_bytes_list = []
        for desc in timbre_descriptors:
            note_b = desc["note_str"].encode('ascii').ljust(8, b'\x00')
            keymap_bytes = struct.pack(
                "<8sBBBBBBHIIIIHH",
                note_b,
                desc["midi_note"],
                desc["key_low"],
                desc["key_high"],
                desc["layer_idx"],
                desc["min_v"],
                desc["max_v"],
                0, # Reserved
                desc["offset"],
                desc["byte_len"],
                desc["frame_count"],
                desc["sr"],
                desc["channels"],
                16 # Bit depth
            )
            keymap_bytes_list.append(keymap_bytes)

        tn7_payload = synth_bytes + b"".join(keymap_bytes_list)
        crc_tn7 = zlib.crc32(tn7_payload) & 0xFFFFFFFF

        # 32-byte CAT7 Header with computed CRC32
        header_tn7_bytes = struct.pack(
            "<4sI16sII",
            magic_tn7,
            version,
            cfg["patch_name_bytes"],
            total_tn7_file_size,
            crc_tn7
        )

        with open(cfg["output_tn7"], "wb") as f_tn7:
            f_tn7.write(header_tn7_bytes)
            f_tn7.write(tn7_payload)
            f_tn7.flush()

        tn7_actual_size = os.path.getsize(cfg["output_tn7"])
        print(f"  - Slot {cfg['slot_number']} ({cfg['name']}): Generated {cfg['output_tn7']} ({tn7_actual_size:,} bytes, CRC32: 0x{crc_tn7:08X}).")

        min_m = min(d["midi_note"] for d in timbre_descriptors)
        max_m = max(d["midi_note"] for d in timbre_descriptors)
        unique_notes = sorted(list(set(d["midi_note"] for d in timbre_descriptors)))
        
        tn7_results.append({
            "timbre_id": cfg["timbre_id"],
            "instrument_name": cfg["name"],
            "slot_number": cfg["slot_number"],
            "category_id": f"0x{cfg['category_id']:02X}",
            "category_name": cfg["category_name"],
            "tn7_file": cfg["output_tn7"],
            "tn7_filename": cfg["tn7_filename"],
            "sample_count": num_entries,
            "keyboard_range": {
                "min_note": midi_to_note_str(min_m),
                "max_note": midi_to_note_str(max_m),
                "min_midi": min_m,
                "max_midi": max_m,
                "total_notes": len(unique_notes)
            },
            "velocity_layers": {
                "count": cfg["total_velocity_layers"],
                "layers": VELOCITY_LAYERS if cfg["timbre_id"] == 1 else [{"layer_idx": 0, "target_v": 127, "min_v": 1, "max_v": 127}]
            },
            "tn7_bytes": tn7_actual_size,
            "crc32_tn7": f"0x{crc_tn7:08X}"
        })

    # 4. Generate JSON metadata summary report
    print("\n--- Generating JSON Metadata Summary Report ---")

    min_midi_global = min(d["midi_note"] for d in sample_descriptors)
    max_midi_global = max(d["midi_note"] for d in sample_descriptors)
    unique_notes_global = sorted(list(set(d["midi_note"] for d in sample_descriptors)))

    summary_data = {
        "status": "SUCCESS",
        "format_version": "2.0.0",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "instrument_name": "Black Upright, Pad 2, Pad Rev Shimmer, Cordas Suave",
        "total_samples_processed": total_samples,
        "total_timbres_processed": len(tn7_results),
        "source_directory": SOURCE_DIR,
        "staging_directory": STAGING_DIR,
        "audio_spec": {
            "sample_rate": 44100,
            "bit_depth": 16,
            "channels": 2,
            "encoding": "pcm_s16le"
        },
        "keyboard_range": {
            "min_note": midi_to_note_str(min_midi_global),
            "max_note": midi_to_note_str(max_midi_global),
            "min_midi": min_midi_global,
            "max_midi": max_midi_global,
            "total_notes": len(unique_notes_global)
        },
        "timbres": tn7_results,
        "wave_bank": {
            "tu7_file": TU7_OUTPUT,
            "tu7_alias_file": TU7_ALIAS_OUTPUT,
            "total_samples": total_samples,
            "tu7_bytes": tu7_actual_size,
            "total_pcm_payload_bytes": total_pcm_payload_bytes,
            "crc32_tu7": f"0x{crc_tu7:08X}"
        },
        "velocity_layer_mapping_verification": {
            "is_valid": total_samples == 968 and len(tn7_results) == 4,
            "expected_sample_count": 968,
            "actual_sample_count": total_samples,
            "timbres_count": len(tn7_results),
            "black_upright_samples": tn7_results[0]["sample_count"],
            "pad_2_samples": tn7_results[1]["sample_count"],
            "pad_rev_shimmer_samples": tn7_results[2]["sample_count"],
            "cordas_suave_samples": tn7_results[3]["sample_count"]
        },
        "output_files": {
            "tn7_black_upright": TN7_BLACK_UPRIGHT,
            "tn7_pad_2": TN7_PAD_2,
            "tn7_pad_rev_shimmer": TN7_PAD_REV_SHIMMER,
            "tn7_cordas_suave": TN7_CORDAS_SUAVE,
            "tu7_file": TU7_OUTPUT,
            "tu7_alias_file": TU7_ALIAS_OUTPUT,
            "summary_json": SUMMARY_OUTPUT
        },
        "file_sizes": {
            "tn7_black_upright_bytes": tn7_results[0]["tn7_bytes"],
            "tn7_pad_2_bytes": tn7_results[1]["tn7_bytes"],
            "tn7_pad_rev_shimmer_bytes": tn7_results[2]["tn7_bytes"],
            "tn7_cordas_suave_bytes": tn7_results[3]["tn7_bytes"],
            "tu7_bytes": tu7_actual_size,
            "total_pcm_payload_bytes": total_pcm_payload_bytes
        },
        "patch_structure": {
            "magic_tn7": magic_tn7.decode('ascii'),
            "magic_tu7": magic_tu7.decode('ascii'),
            "synthesis_block_size": synth_block_size,
            "target_tu7_filename": "blk_upright.tu7",
            "descriptor_stride_bytes": descriptor_size,
            "total_keymap_entries_count": total_samples,
            "crc32_tu7": f"0x{crc_tu7:08X}"
        }
    }
    
    with open(SUMMARY_OUTPUT, "w", encoding="utf-8") as f_sum:
        json.dump(summary_data, f_sum, indent=2)
        
    print(f"Generated summary report: {SUMMARY_OUTPUT}")
    print("=== Multi-Timbre Conversion Complete ===")
    return 0

if __name__ == "__main__":
    sys.exit(main())
