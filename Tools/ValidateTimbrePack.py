#!/usr/bin/env python3
"""
Tools/ValidateTimbrePack.py
Timbre Pack Validation Tool for ChurchDAW

Programmatically checks:
1. Sample file existence and path integrity (0 broken links in .sfz and .json).
2. Filename compliance with TimbresProcessor::parseSampleName() (Ableton pitch convention C3 = 60).
3. Audio file parameter integrity (44.1kHz, 24-bit PCM).
4. Key range coverage [0..127] without gaps in both .sfz and .json.
5. Exclusion of artwork files (Background.png, Wallpaper.jpg, *.png, *.jpg, *.nki).
6. Optional ZIP archive structure and catalog manifest compatibility.
"""

import sys
import os
import re
import json
import wave
import hashlib
import zipfile
import argparse
from pathlib import Path

# Ableton Pitch Convention: C3 = 60
def note_name_to_midi(n: str) -> int:
    """
    Converts note name string (e.g. 'C3', 'D#1', 'A#4') to MIDI note number.
    Uses Ableton pitch convention where C3 = 60.
    Returns -1 if invalid.
    """
    if not n:
        return -1
    
    semis = {'A': 9, 'B': 11, 'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7}
    letter = n[0].upper()
    if letter not in semis:
        return -1
    
    semi = semis[letter]
    pos = 1
    
    if pos < len(n) and n[pos] == '#':
        semi += 1
        pos += 1
    elif pos < len(n) and n[pos] == 'b':
        semi -= 1
        pos += 1
        
    oct_str = n[pos:]
    if not oct_str:
        return -1
    
    # Octave can be negative, e.g., -1, -2
    try:
        oct_val = int(oct_str)
    except ValueError:
        return -1
        
    midi = (oct_val + 2) * 12 + semi
    return midi if 0 <= midi < 128 else -1


def parse_sample_name(stem: str):
    """
    Python port of TimbresProcessor::parseSampleName().
    Splits stem by '-' looking for <Instrument>-<NoteName>-V<Velocity>-<ID>.
    Returns (instrument, midi_note, velocity) if valid, or None if invalid.
    """
    parts = stem.split('-')
    if len(parts) < 4:
        return None
    
    vel_idx = len(parts) - 2
    vel_tok = parts[vel_idx]
    
    digits = vel_tok[1:] if vel_tok.startswith('V') else vel_tok
    if not digits.isdigit():
        return None
    
    vel = int(digits)
    if not (1 <= vel <= 127):
        return None
    
    note_idx = vel_idx - 1
    note_tok = parts[note_idx]
    
    # Handle negative octaves if split into separate token
    if note_tok.isdigit() and note_idx > 0:
        note_tok = parts[note_idx - 1] + "-" + note_tok
        note_idx -= 1
        
    note = note_name_to_midi(note_tok)
    if note < 0 or note_idx < 1:
        return None
    
    instr = "-".join(parts[:note_idx]).strip()
    if not instr:
        return None
    
    return instr, note, vel


class PackReader:
    """Helper to read files from either a directory or a ZIP archive."""
    def __init__(self, target_path: Path):
        self.target_path = target_path
        self.is_zip = target_path.is_file() and target_path.suffix.lower() == '.zip'
        self._zip_file = None
        if self.is_zip:
            self._zip_file = zipfile.ZipFile(target_path, 'r')
            
    def list_files(self):
        """Returns list of relative file paths in the pack."""
        if self.is_zip:
            return [name for name in self._zip_file.namelist() if not name.endswith('/')]
        else:
            files = []
            for root, _, filenames in os.walk(self.target_path):
                for f in filenames:
                    rel = Path(root).relative_to(self.target_path) / f
                    files.append(str(rel).replace('\\', '/'))
            return files

    def read_text(self, rel_path: str) -> str:
        """Reads text content of a relative file path."""
        if self.is_zip:
            # find matching name in zip
            matches = [name for name in self._zip_file.namelist() if name.endswith(rel_path)]
            if not matches:
                raise FileNotFoundError(f"File not found in ZIP: {rel_path}")
            with self._zip_file.open(matches[0]) as f:
                return f.read().decode('utf-8')
        else:
            file_p = self.target_path / rel_path
            if not file_p.exists():
                # search recursively if path differs
                for p in self.target_path.rglob(Path(rel_path).name):
                    return p.read_text(encoding='utf-8')
                raise FileNotFoundError(f"File not found: {rel_path}")
            return file_p.read_text(encoding='utf-8')

    def open_binary(self, rel_path: str):
        """Opens a binary stream for a relative file path."""
        if self.is_zip:
            matches = [name for name in self._zip_file.namelist() if name.endswith(rel_path)]
            if not matches:
                raise FileNotFoundError(f"File not found in ZIP: {rel_path}")
            return self._zip_file.open(matches[0])
        else:
            file_p = self.target_path / rel_path
            if not file_p.exists():
                for p in self.target_path.rglob(Path(rel_path).name):
                    return open(p, 'rb')
                raise FileNotFoundError(f"File not found: {rel_path}")
            return open(file_p, 'rb')

    def close(self):
        if self._zip_file:
            self._zip_file.close()


def catalog_slug(name: str) -> str:
    """Mirror of BuildTimbreCatalog.ps1 Get-InstrumentSlug."""
    if re.search(r'(?i)pad_worship_2_0_ed_bento|pad worship 2\.0 ed bento|new pad', name):
        return 'pad_worship_2_0_ed_bento'
    s = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
    return s or 'timbre'


def find_catalog_entry(instruments, parsed_notes, target: Path, catalog_id: str = None):
    """
    Locates the catalog entry for this pack. Match order: explicit --id,
    then the pack's dominant instrument name (as slug/name/raw_label/install
    folder), then the zip's sha256 as a last resort.
    """
    if catalog_id:
        return next((i for i in instruments if i.get('id') == catalog_id), None)

    names = [instr for _, instr, _, _ in parsed_notes]
    pack_instr = max(set(names), key=names.count) if names else None
    if pack_instr:
        slug = catalog_slug(pack_instr)
        low = pack_instr.lower()
        matches = [i for i in instruments if i.get('id') == slug
                   or str(i.get('name', '')).lower() == low
                   or str(i.get('raw_label', '')).lower() == low
                   or str(i.get('install_relative', '')).replace('\\', '/').rstrip('/').rsplit('/', 1)[-1].lower() == low]
        ids = {m.get('id') for m in matches}
        if len(ids) == 1:
            return matches[0]
        if len(ids) > 1:
            print(f"  WARN: Multiple catalog ids match '{pack_instr}': {sorted(ids)}")
            return None

    if target.is_file() and target.suffix.lower() == '.zip':
        sha256 = hashlib.sha256(target.read_bytes()).hexdigest()
        return next((i for i in instruments if str(i.get('sha256', '')).lower() == sha256), None)
    return None


def validate_pack(pack_path: str, catalog_path: str = None, catalog_id: str = None) -> bool:
    target = Path(pack_path)
    if not target.exists():
        print(f"FAIL: Pack target '{pack_path}' does not exist.")
        return False

    print(f"==================================================")
    print(f" Validating Timbre Pack: {target.name}")
    print(f" Target Path: {target.resolve()}")
    print(f"==================================================")

    reader = PackReader(target)
    all_files = reader.list_files()
    file_basenames = {Path(f).name: f for f in all_files}

    passed = True

    # ----------------------------------------------------
    # Check 1: Forbidden Artwork Files Check
    # ----------------------------------------------------
    print("\n[Check 1/5] Checking Artwork File Exclusion...")
    forbidden_exts = {'.png', '.jpg', '.jpeg', '.bmp', '.nki'}
    forbidden_found = []
    for f in all_files:
        p = Path(f)
        if p.suffix.lower() in forbidden_exts or p.name in {'Background.png', 'Wallpaper.jpg'}:
            forbidden_found.append(f)

    if forbidden_found:
        print(f"  FAIL: Found forbidden artwork/Kontakt files in pack: {forbidden_found}")
        passed = False
    else:
        print("  PASS: 0 artwork/unwanted files found.")

    # ----------------------------------------------------
    # Check 2: Sample File Existence and Path Integrity
    # ----------------------------------------------------
    print("\n[Check 2/5] Checking Sample Path Integrity in SFZ and JSON...")
    sfz_files = [f for f in all_files if f.endswith('.sfz')]
    json_files = [f for f in all_files if f.endswith('.json')]

    if not sfz_files:
        print("  FAIL: No .sfz mapping file found in pack.")
        passed = False
    if not json_files:
        print("  FAIL: No .json metadata file found in pack.")
        passed = False

    referenced_samples = set()

    # Parse SFZ references
    sfz_sample_refs = []
    for sfz_rel in sfz_files:
        sfz_text = reader.read_text(sfz_rel)
        for line in sfz_text.splitlines():
            line = line.strip()
            if 'sample=' in line:
                m = re.search(r'sample=(?:"([^"]+)"|([^\r\n]+?)(?=\s+[a-zA-Z_0-9]+=|[\r\n]|$))', line)
                if m:
                    sample_file = (m.group(1) or m.group(2)).strip()
                    sfz_sample_refs.append(sample_file)
                    referenced_samples.add(sample_file)

    # Parse JSON references
    json_sample_refs = []
    for json_rel in json_files:
        json_text = reader.read_text(json_rel)
        try:
            data = json.loads(json_text)
            if 'samples' in data and isinstance(data['samples'], list):
                for item in data['samples']:
                    if 'file' in item:
                        json_sample_refs.append(item['file'])
                        referenced_samples.add(item['file'])
        except Exception as e:
            print(f"  FAIL: Invalid JSON in {json_rel}: {e}")
            passed = False

    broken_links = 0
    for sample_file in referenced_samples:
        if sample_file not in file_basenames:
            print(f"  FAIL: Broken link to sample file: '{sample_file}'")
            broken_links += 1

    if broken_links == 0 and len(referenced_samples) > 0:
        print(f"  PASS: 0 broken links found ({len(referenced_samples)} samples referenced in SFZ/JSON).")
    else:
        if len(referenced_samples) == 0:
            print("  FAIL: No sample references found in SFZ or JSON.")
        passed = False

    # ----------------------------------------------------
    # Check 3: Filename Compliance with TimbresProcessor::parseSampleName()
    # ----------------------------------------------------
    print("\n[Check 3/5] Checking Filename Compliance (parseSampleName)...")
    wav_files = [f for f in all_files if f.endswith('.wav')]
    if not wav_files:
        print("  FAIL: No .wav audio samples found.")
        passed = False

    parse_errors = 0
    parsed_notes = []
    for wav_rel in wav_files:
        stem = Path(wav_rel).stem
        res = parse_sample_name(stem)
        if res is None:
            print(f"  FAIL: Filename '{stem}.wav' failed parseSampleName()")
            parse_errors += 1
        else:
            instr, note, vel = res
            parsed_notes.append((stem, instr, note, vel))

    if parse_errors == 0 and len(wav_files) > 0:
        print(f"  PASS: All {len(wav_files)} WAV filenames comply with parseSampleName() (C3 = 60).")
    else:
        passed = False

    # ----------------------------------------------------
    # Check 4: Audio File Parameter Integrity (44.1kHz, 24-bit PCM)
    # ----------------------------------------------------
    print("\n[Check 4/5] Checking Audio File Parameter Integrity (44.1kHz, 24-bit PCM)...")
    audio_errors = 0
    for wav_rel in wav_files:
        try:
            stream = reader.open_binary(wav_rel)
            with wave.open(stream, 'rb') as w:
                nchannels = w.getnchannels()
                sampwidth = w.getsampwidth()  # bytes per sample
                framerate = w.getframerate()  # sample rate
                nframes = w.getnframes()

                errors = []
                if framerate != 44100:
                    errors.append(f"sample rate {framerate} != 44100")
                if sampwidth != 3:
                    errors.append(f"bit depth {sampwidth*8}-bit != 24-bit (sampwidth {sampwidth} != 3)")
                if nchannels < 1:
                    errors.append(f"channels {nchannels} < 1")
                if nframes <= 0:
                    errors.append("empty frames")

                if errors:
                    print(f"  FAIL: {Path(wav_rel).name} -> {', '.join(errors)}")
                    audio_errors += 1
        except Exception as e:
            print(f"  FAIL: {Path(wav_rel).name} could not be read as WAV: {e}")
            audio_errors += 1

    if audio_errors == 0 and len(wav_files) > 0:
        print(f"  PASS: All {len(wav_files)} WAV files are valid 44.1kHz, 24-bit PCM audio.")
    else:
        passed = False

    # ----------------------------------------------------
    # Check 5: Key Range Coverage [0..127] without gaps
    # ----------------------------------------------------
    print("\n[Check 5/5] Checking Key Range Coverage [0..127] without gaps...")
    
    # Check SFZ coverage
    sfz_coverage_ok = True
    for sfz_rel in sfz_files:
        sfz_text = reader.read_text(sfz_rel)
        regions = []
        for line in sfz_text.splitlines():
            if '<region>' in line:
                lokey_m = re.search(r'lokey=(\d+)', line)
                hikey_m = re.search(r'hikey=(\d+)', line)
                pitch_m = re.search(r'pitch_keycenter=(\d+)', line)
                if lokey_m and hikey_m and pitch_m:
                    regions.append({
                        'lokey': int(lokey_m.group(1)),
                        'hikey': int(hikey_m.group(1)),
                        'pitch': int(pitch_m.group(1))
                    })
        regions.sort(key=lambda r: r['lokey'])
        if not regions:
            print(f"  FAIL: No regions found in {sfz_rel}")
            sfz_coverage_ok = False
        else:
            if regions[0]['lokey'] != 0:
                print(f"  FAIL: SFZ lowest key is {regions[0]['lokey']}, expected 0.")
                sfz_coverage_ok = False
            if regions[-1]['hikey'] != 127:
                print(f"  FAIL: SFZ highest key is {regions[-1]['hikey']}, expected 127.")
                sfz_coverage_ok = False
            for i in range(len(regions) - 1):
                if regions[i+1]['lokey'] != regions[i]['hikey'] + 1:
                    print(f"  FAIL: SFZ gap/overlap between key {regions[i]['hikey']} and {regions[i+1]['lokey']}")
                    sfz_coverage_ok = False
            for r in regions:
                if not (r['lokey'] <= r['pitch'] <= r['hikey']):
                    print(f"  FAIL: SFZ pitch_keycenter {r['pitch']} outside range [{r['lokey']}..{r['hikey']}]")
                    sfz_coverage_ok = False

    # Check JSON coverage
    json_coverage_ok = True
    for json_rel in json_files:
        json_text = reader.read_text(json_rel)
        data = json.loads(json_text)
        samples = data.get('samples', [])
        samples.sort(key=lambda s: s['key_low'])
        if not samples:
            print(f"  FAIL: No samples array in {json_rel}")
            json_coverage_ok = False
        else:
            if samples[0]['key_low'] != 0:
                print(f"  FAIL: JSON lowest key is {samples[0]['key_low']}, expected 0.")
                json_coverage_ok = False
            if samples[-1]['key_high'] != 127:
                print(f"  FAIL: JSON highest key is {samples[-1]['key_high']}, expected 127.")
                json_coverage_ok = False
            for i in range(len(samples) - 1):
                if samples[i+1]['key_low'] != samples[i]['key_high'] + 1:
                    print(f"  FAIL: JSON gap/overlap between key {samples[i]['key_high']} and {samples[i+1]['key_low']}")
                    json_coverage_ok = False
            for s in samples:
                if not (s['key_low'] <= s['midi_root'] <= s['key_high']):
                    print(f"  FAIL: JSON midi_root {s['midi_root']} outside range [{s['key_low']}..{s['key_high']}]")
                    json_coverage_ok = False

    if sfz_coverage_ok and json_coverage_ok:
        print("  PASS: Key range covers [0..127] continuously with 0 gaps in SFZ and JSON.")
    else:
        passed = False

    reader.close()

    # ----------------------------------------------------
    # Optional Catalog Manifest Check
    # ----------------------------------------------------
    if catalog_path and Path(catalog_path).exists():
        print(f"\n[Optional Check] Validating Catalog Entry in {catalog_path}...")
        cat_text = Path(catalog_path).read_text(encoding='utf-8')
        cat_data = json.loads(cat_text)
        instruments = cat_data.get('instruments', []) if isinstance(cat_data, dict) else cat_data
        entry = find_catalog_entry(instruments, parsed_notes, target, catalog_id)
        if not entry:
            print("  FAIL: No catalog entry matches this pack (use --id to pick one explicitly).")
            passed = False
        else:
            print(f"  PASS: Catalog entry found for '{entry.get('name')}' (id '{entry.get('id')}').")
            if target.is_file() and target.suffix.lower() == '.zip':
                zip_bytes = target.stat().st_size
                sha256 = hashlib.sha256(target.read_bytes()).hexdigest()
                if entry.get('download_bytes') and entry['download_bytes'] != zip_bytes:
                    print(f"  FAIL: Catalog download_bytes ({entry.get('download_bytes')}) != zip size ({zip_bytes})")
                    passed = False
                if entry.get('sha256') and entry['sha256'].lower() != sha256.lower():
                    print(f"  FAIL: Catalog sha256 ({entry.get('sha256')}) != zip sha256 ({sha256})")
                    passed = False

    print("\n==================================================")
    if passed:
        print(" RESULT: 100% PASS - Timbre pack is valid!")
        print("==================================================")
        return True
    else:
        print(" RESULT: FAIL - Validation errors encountered.")
        print("==================================================")
        return False


def create_pack_zip(source_dir: Path, output_zip_path: Path, install_relative: str = "Pads/Pad Worship 2.0 Ed Bento") -> bool:
    """
    Packages source_dir into a Download Pack ZIP file.
    The internal structure of the archive will have files under install_relative.
    Excludes artwork files (Background.png, Wallpaper.jpg, .nki, etc.).
    """
    output_zip_path.parent.mkdir(parents=True, exist_ok=True)
    
    files_to_pack = []
    forbidden_exts = {'.png', '.jpg', '.jpeg', '.bmp', '.nki'}
    
    for root, _, filenames in os.walk(source_dir):
        for f in filenames:
            p = Path(f)
            if p.suffix.lower() in forbidden_exts or p.name in {'Background.png', 'Wallpaper.jpg'}:
                continue
            abs_p = Path(root) / f
            arc_name = f"{install_relative}/{f}".replace('\\', '/')
            files_to_pack.append((abs_p, arc_name))
            
    files_to_pack.sort(key=lambda x: x[1])
    
    with zipfile.ZipFile(output_zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for abs_p, arc_name in files_to_pack:
            zf.write(abs_p, arcname=arc_name)
            
    print(f"Created ZIP pack: {output_zip_path}")
    print(f"  Files packed: {len(files_to_pack)}")
    size_bytes = output_zip_path.stat().st_size
    sha256_hash = hashlib.sha256(output_zip_path.read_bytes()).hexdigest()
    print(f"  Size: {size_bytes} bytes ({size_bytes / 1024 / 1024:.2f} MB)")
    print(f"  SHA-256: {sha256_hash}")
    return True


def main():
    parser = argparse.ArgumentParser(description="Validate and Package ChurchDAW Timbre Pack")
    parser.add_argument("--pack", required=True, help="Path to pack directory or ZIP file")
    parser.add_argument("--catalog", help="Optional path to catalog.json manifest")
    parser.add_argument("--id", help="Catalog instrument id to check (default: derived from the pack's sample names)")
    parser.add_argument("--create-zip", help="Destination path to create a packaged ZIP file from --pack directory")
    parser.add_argument("--install-relative", default="Pads/Pad Worship 2.0 Ed Bento",
                        help="Internal folder used by --create-zip")
    args = parser.parse_args()

    pack_target = Path(args.pack)

    if args.create_zip:
        if not pack_target.is_dir():
            print(f"FAIL: --pack target '{args.pack}' must be a directory when using --create-zip.")
            sys.exit(1)
        dest_zip = Path(args.create_zip)
        create_pack_zip(pack_target, dest_zip, args.install_relative)
        # Validate the newly created ZIP file
        success = validate_pack(str(dest_zip), args.catalog, args.id)
        sys.exit(0 if success else 1)

    success = validate_pack(args.pack, args.catalog, args.id)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
