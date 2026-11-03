#!/usr/bin/env python3
"""
Tests/TestValidateTimbrePack.py
Empirical test harness for Milestone 2: Download Pack Packaging & Validation Script.

Tests:
1. Programmatic validation of the current ``new_pad`` release artifact. CI or
   local audits provide it through ``CHURCHDAW_TIMBRE_PACK``; a checked-in
   ``Packaging/Packs/new-pad-v1.zip`` remains a supported fallback:
   - Extract archive to temp directory.
   - Validate sample headers (44.1kHz, 24-bit PCM, valid channel & frame count).
   - Validate SFZ parsing, sample reference paths, pitch keycenters, and continuous key range [0..127].
   - Validate JSON parsing, sample references, key low/high boundaries, and root notes.
   - Validate exclusion of artwork files.
   - Measure file size and calculate SHA-256 digest.

2. Empirical stress-testing of Tools/ValidateTimbrePack.py:
   - Run against valid pack (directory & ZIP) -> expect PASS.
   - Run against corrupted inputs -> expect FAIL:
     a) Extra artwork file (Background.png, Wallpaper.jpg, .nki, .jpg)
     b) Missing sample file (broken reference in SFZ/JSON)
     c) Non-compliant sample filename (violates parseSampleName)
     d) Invalid WAV header (16-bit PCM instead of 24-bit, 48kHz instead of 44.1kHz, corrupt WAV data)
     e) Key range gap in SFZ/JSON
     f) Pitch keycenter outside key range in SFZ/JSON
"""

import sys
import os
import io
import re
import json
import wave
import shutil
import tempfile
import zipfile
import hashlib
import unittest
from pathlib import Path

# Add project root to sys.path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT))

from Tools.ValidateTimbrePack import validate_pack, parse_sample_name, note_name_to_midi


CATALOG_PATH = PROJECT_ROOT / "Packaging" / "catalog.json"
CATALOG_DATA = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
CURRENT_PACK_ID = "new_pad"
CURRENT_ENTRY = next(
    item for item in CATALOG_DATA.get("instruments", [])
    if item.get("id") == CURRENT_PACK_ID
)
DEFAULT_PACK_PATH = PROJECT_ROOT / "Packaging" / "Packs" / "new-pad-v1.zip"
CURRENT_PACK_PATH = Path(os.environ.get(
    "CHURCHDAW_TIMBRE_PACK", str(DEFAULT_PACK_PATH)
)).expanduser().resolve()


class TestCurrentNewPadPackIntegrity(unittest.TestCase):
    """Programmatic checks on the currently catalogued New Pad artifact."""

    @classmethod
    def setUpClass(cls):
        cls.zip_path = CURRENT_PACK_PATH
        if not cls.zip_path.is_file():
            raise FileNotFoundError(
                "Current New Pad fixture is missing. Set CHURCHDAW_TIMBRE_PACK "
                f"or place it at {DEFAULT_PACK_PATH}"
            )
        cls.temp_dir = Path(tempfile.mkdtemp(prefix="new_pad_pack_test_"))
        
        # Unzip
        with zipfile.ZipFile(cls.zip_path, 'r') as zf:
            zf.extractall(cls.temp_dir)
            
    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.temp_dir, ignore_errors=True)

    def test_zip_file_exists_and_size(self):
        """Verify ZIP archive exists, size > 0, and record SHA-256 digest."""
        self.assertTrue(self.zip_path.exists(), f"ZIP pack path does not exist: {self.zip_path}")
        size = self.zip_path.stat().st_size
        self.assertGreater(size, 0, "ZIP pack is 0 bytes")
        sha256 = hashlib.sha256(self.zip_path.read_bytes()).hexdigest()
        self.assertEqual(size, CURRENT_ENTRY["download_bytes"])
        self.assertEqual(sha256, CURRENT_ENTRY["sha256"])
        print(f"\n[Pack Stats] File Size: {size} bytes ({size / (1024*1024):.2f} MB), SHA-256: {sha256}")

    def test_artwork_exclusion(self):
        """Verify archive contains 0 forbidden artwork files (.png, .jpg, .nki, Background.png, Wallpaper.jpg)."""
        forbidden_exts = {'.png', '.jpg', '.jpeg', '.bmp', '.nki'}
        with zipfile.ZipFile(self.zip_path, 'r') as zf:
            for name in zf.namelist():
                p = Path(name)
                self.assertNotIn(p.suffix.lower(), forbidden_exts, f"Forbidden extension found in ZIP: {name}")
                self.assertNotIn(p.name, {'Background.png', 'Wallpaper.jpg'}, f"Forbidden artwork file found in ZIP: {name}")

    def test_uncompressed_structure_and_files(self):
        """Verify uncompressed directory contains exactly 14 files (12 WAV + 1 SFZ + 1 JSON)."""
        all_files = list(self.temp_dir.rglob("*"))
        files_only = [f for f in all_files if f.is_file()]
        wav_files = [f for f in files_only if f.suffix.lower() == '.wav']
        sfz_files = [f for f in files_only if f.suffix.lower() == '.sfz']
        json_files = [f for f in files_only if f.suffix.lower() == '.json']

        self.assertEqual(len(wav_files), 12, f"Expected 12 WAV files, found {len(wav_files)}")
        self.assertEqual(len(sfz_files), 1, f"Expected 1 SFZ file, found {len(sfz_files)}")
        self.assertEqual(len(json_files), 1, f"Expected 1 JSON file, found {len(json_files)}")
        self.assertEqual(len(files_only), 14, f"Expected 14 total files in pack, found {len(files_only)}")

    def test_wav_sample_headers(self):
        """Verify all WAV files in pack are valid 44.1kHz, 24-bit PCM stereo/mono audio."""
        wav_files = list(self.temp_dir.rglob("*.wav"))
        self.assertGreater(len(wav_files), 0)

        for wav_p in wav_files:
            with wave.open(str(wav_p), 'rb') as w:
                framerate = w.getframerate()
                sampwidth = w.getsampwidth()
                channels = w.getnchannels()
                nframes = w.getnframes()

                self.assertEqual(framerate, 44100, f"{wav_p.name} sample rate is {framerate}, expected 44100")
                self.assertEqual(sampwidth, 3, f"{wav_p.name} sampwidth is {sampwidth} ({sampwidth*8}-bit), expected 3 (24-bit PCM)")
                self.assertGreater(channels, 0, f"{wav_p.name} has invalid channels: {channels}")
                self.assertGreater(nframes, 0, f"{wav_p.name} has 0 frames")

    def test_filename_parse_compliance(self):
        """Verify all 12 WAV filenames match TimbresProcessor naming convention."""
        wav_files = list(self.temp_dir.rglob("*.wav"))
        for wav_p in wav_files:
            stem = wav_p.stem
            res = parse_sample_name(stem)
            self.assertIsNotNone(res, f"Filename '{stem}' failed parseSampleName()")
            instr, midi_note, velocity = res
            self.assertGreaterEqual(midi_note, 0)
            self.assertLessEqual(midi_note, 127)
            self.assertEqual(velocity, 127)

    def test_sfz_parsing_and_key_coverage(self):
        """Parse SFZ and verify sample references, pitch centers, and continuous key range [0..127]."""
        sfz_files = list(self.temp_dir.rglob("*.sfz"))
        self.assertEqual(len(sfz_files), 1)
        sfz_p = sfz_files[0]

        sfz_text = sfz_p.read_text(encoding='utf-8')
        regions = []
        for line in sfz_text.splitlines():
            line = line.strip()
            if not line or line.startswith('//'):
                continue
            sample_m = re.search(r'sample=(?:"([^"]+)"|([^\r\n]+?)(?=\s+[a-zA-Z_0-9]+=|[\r\n]|$))', line)
            lokey_m = re.search(r'lokey=(\d+)', line)
            hikey_m = re.search(r'hikey=(\d+)', line)
            pitch_m = re.search(r'pitch_keycenter=(\d+)', line)

            if sample_m and lokey_m and hikey_m and pitch_m:
                sample_file = (sample_m.group(1) or sample_m.group(2)).strip()
                regions.append({
                    'sample': sample_file,
                    'lokey': int(lokey_m.group(1)),
                    'hikey': int(hikey_m.group(1)),
                    'pitch': int(pitch_m.group(1))
                })

        self.assertEqual(len(regions), 12, f"Expected 12 regions in SFZ, found {len(regions)}")

        # Verify referenced samples exist on disk
        wav_names = {f.name for f in self.temp_dir.rglob("*.wav")}
        for r in regions:
            self.assertIn(r['sample'], wav_names, f"SFZ references sample '{r['sample']}' which does not exist on disk")
            self.assertTrue(r['lokey'] <= r['pitch'] <= r['hikey'], f"pitch_keycenter {r['pitch']} outside range [{r['lokey']}..{r['hikey']}]")

        # Verify range continuity [0..127]
        regions.sort(key=lambda x: x['lokey'])
        self.assertEqual(regions[0]['lokey'], 0, "SFZ region low key does not start at 0")
        self.assertEqual(regions[-1]['hikey'], 127, "SFZ region high key does not end at 127")
        for i in range(len(regions) - 1):
            self.assertEqual(regions[i+1]['lokey'], regions[i]['hikey'] + 1,
                             f"Gap/Overlap between SFZ region {i} (hikey={regions[i]['hikey']}) and {i+1} (lokey={regions[i+1]['lokey']})")

    def test_json_parsing_and_key_coverage(self):
        """Parse JSON manifest and verify sample references, midi roots, and continuous key range [0..127]."""
        json_files = list(self.temp_dir.rglob("*.json"))
        self.assertEqual(len(json_files), 1)
        json_p = json_files[0]

        data = json.loads(json_p.read_text(encoding='utf-8'))
        samples = data.get('samples', [])
        self.assertEqual(len(samples), 12, f"Expected 12 samples in JSON, found {len(samples)}")

        wav_names = {f.name for f in self.temp_dir.rglob("*.wav")}
        for s in samples:
            self.assertIn(s['file'], wav_names, f"JSON references sample '{s['file']}' which does not exist on disk")
            self.assertTrue(s['key_low'] <= s['midi_root'] <= s['key_high'],
                            f"midi_root {s['midi_root']} outside range [{s['key_low']}..{s['key_high']}]")

        samples.sort(key=lambda x: x['key_low'])
        self.assertEqual(samples[0]['key_low'], 0, "JSON sample key_low does not start at 0")
        self.assertEqual(samples[-1]['key_high'], 127, "JSON sample key_high does not end at 127")
        for i in range(len(samples) - 1):
            self.assertEqual(samples[i+1]['key_low'], samples[i]['key_high'] + 1,
                             f"Gap/Overlap in JSON between sample {i} (key_high={samples[i]['key_high']}) and {i+1} (key_low={samples[i+1]['key_low']})")

    def test_catalog_manifest_registration(self):
        """Verify catalog registration in catalog.json and timbre-catalog-v1.json."""
        catalog_files = [
            PROJECT_ROOT / "catalog.json",
            PROJECT_ROOT / "Packaging" / "catalog.json",
            PROJECT_ROOT / "Packaging" / "TimbrePacks" / "timbre-catalog-v1.json"
        ]

        for cat_p in catalog_files:
            self.assertTrue(cat_p.exists(), f"Catalog file missing: {cat_p}")
            data = json.loads(cat_p.read_text(encoding='utf-8'))
            instruments = data.get('instruments', [])
            entry = next((inst for inst in instruments if inst.get('id') == CURRENT_PACK_ID), None)
            self.assertIsNotNone(entry, f"Entry '{CURRENT_PACK_ID}' missing in {cat_p.name}")
            self.assertEqual(entry.get('name'), CURRENT_ENTRY['name'])
            self.assertEqual(entry.get('raw_label'), CURRENT_ENTRY['raw_label'])
            self.assertEqual(entry.get('category'), CURRENT_ENTRY['category'])
            self.assertEqual(entry.get('install_relative'), CURRENT_ENTRY['install_relative'])
            self.assertEqual(entry.get('file_count'), CURRENT_ENTRY['file_count'])
            self.assertEqual(entry.get('download_bytes'), CURRENT_ENTRY['download_bytes'])
            self.assertEqual(entry.get('sha256'), CURRENT_ENTRY['sha256'])
            self.assertTrue(entry.get('download_url').startswith('https://'))

            # Validate pack against catalog
            val_res = validate_pack(str(self.zip_path), str(cat_p))
            self.assertTrue(val_res, f"Validation failed for zip against {cat_p.name}")



class TestValidateTimbrePackStress(unittest.TestCase):
    """Stress-test Tools/ValidateTimbrePack.py with synthetic valid and corrupted packs."""

    def setUp(self):
        self.temp_dir = Path(tempfile.mkdtemp(prefix="validator_stress_"))
        self._build_valid_pack(self.temp_dir)

    def tearDown(self):
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def _create_synthetic_wav(self, file_path: Path, sample_rate=44100, sampwidth=3, num_channels=2, duration_sec=0.1):
        """Creates a dummy valid PCM WAV file with specified parameters."""
        file_path.parent.mkdir(parents=True, exist_ok=True)
        num_frames = int(sample_rate * duration_sec)
        dummy_sample_data = bytes([0x12] * (num_frames * num_channels * sampwidth))
        
        with wave.open(str(file_path), 'wb') as w:
            w.setnchannels(num_channels)
            w.setsampwidth(sampwidth)
            w.setframerate(sample_rate)
            w.writeframes(dummy_sample_data)

    def _build_valid_pack(self, target_dir: Path):
        """Builds a minimal 100% valid pack structure in target_dir."""
        pack_folder = target_dir / "Test Pack"
        pack_folder.mkdir(parents=True, exist_ok=True)

        # 2 samples: C3 (60) and C4 (72)
        notes = [
            ("Test Pack-C3-V127-01.wav", 60, 0, 66),
            ("Test Pack-C4-V127-01.wav", 72, 67, 127),
        ]

        sfz_lines = []
        json_samples = []

        for filename, midi_note, lokey, hikey in notes:
            wav_path = pack_folder / filename
            self._create_synthetic_wav(wav_path, sample_rate=44100, sampwidth=3, num_channels=2)
            sfz_lines.append(f"<region> sample={filename} lokey={lokey} hikey={hikey} pitch_keycenter={midi_note}")
            json_samples.append({
                "file": filename,
                "midi_root": midi_note,
                "key_low": lokey,
                "key_high": hikey
            })

        sfz_path = pack_folder / "Test Pack.sfz"
        sfz_path.write_text("\n".join(sfz_lines), encoding='utf-8')

        json_path = pack_folder / "Test Pack.json"
        json_data = {"name": "Test Pack", "samples": json_samples}
        json_path.write_text(json.dumps(json_data, indent=2), encoding='utf-8')

        return pack_folder

    def _build_native_zone_pack(self, target_dir: Path):
        """Build a faithful NSMP-style pack: native 35002 Hz/16-bit + zones."""
        pack_folder = target_dir / "Native Pad"
        pack_folder.mkdir(parents=True, exist_ok=True)
        definitions = [
            ("Native Pad-C3-V127-01.wav", 60, 0, 66),
            ("Native Pad-C4-V127-01.wav", 72, 67, 108),
        ]
        zones = []
        frame_count = int(35002 * 0.1)
        for filename, root, key_lo, key_hi in definitions:
            wav_path = pack_folder / filename
            self._create_synthetic_wav(
                wav_path, sample_rate=35002, sampwidth=2, num_channels=2)
            zones.append({
                "file": filename,
                "sha256": hashlib.sha256(wav_path.read_bytes()).hexdigest(),
                "root": root,
                "keyLo": key_lo,
                "keyHi": key_hi,
                "velLo": 0,
                "velHi": 127,
                "sampleRate": 35002,
                "bitDepth": 16,
                "channels": 2,
                "frames": frame_count,
                "sourceLoop": {
                    "known": True,
                    "sourceLoops": True,
                    "startFrame": 100,
                    "endFrame": 2000,
                    "playback": True,
                },
            })
        (pack_folder / "churchdaw-timbre.json").write_text(
            json.dumps({"schema": 1, "name": "Native Pad", "zones": zones}, indent=2),
            encoding="utf-8",
        )
        return pack_folder

    def test_stress_01_valid_folder_passes(self):
        """Valid directory pack MUST pass validation."""
        pack_folder = self.temp_dir / "Test Pack"
        res = validate_pack(str(pack_folder))
        self.assertTrue(res, "Valid folder pack failed validation")

    def test_stress_02_valid_zip_passes(self):
        """Valid ZIP pack MUST pass validation."""
        pack_folder = self.temp_dir / "Test Pack"
        zip_path = self.temp_dir / "test_pack.zip"
        
        with zipfile.ZipFile(zip_path, 'w') as zf:
            for root, _, files in os.walk(pack_folder):
                for f in files:
                    abs_p = Path(root) / f
                    rel_p = abs_p.relative_to(pack_folder)
                    zf.write(abs_p, arcname=f"Pads/Test Pack/{rel_p.as_posix()}")

        res = validate_pack(str(zip_path))
        self.assertTrue(res, "Valid ZIP pack failed validation")

    def test_stress_03_corrupt_extra_artwork_fails(self):
        """Pack containing artwork file (Background.png / Wallpaper.jpg / .nki) MUST fail Check 1."""
        pack_folder = self.temp_dir / "Test Pack"
        (pack_folder / "Background.png").write_bytes(b"PNG_DUMMY_DATA")
        
        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with Background.png should fail validation")

        (pack_folder / "Background.png").unlink()
        (pack_folder / "instrument.nki").write_bytes(b"NKI_DUMMY_DATA")
        res2 = validate_pack(str(pack_folder))
        self.assertFalse(res2, "Pack with .nki file should fail validation")

    def test_stress_04_corrupt_missing_sample_fails(self):
        """SFZ/JSON referencing non-existent sample file MUST fail Check 2."""
        pack_folder = self.temp_dir / "Test Pack"
        sfz_path = pack_folder / "Test Pack.sfz"
        sfz_path.write_text("<region> sample=MissingSample.wav lokey=0 hikey=127 pitch_keycenter=60", encoding='utf-8')
        
        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with missing sample file reference should fail validation")

    def test_stress_05_corrupt_noncompliant_filename_fails(self):
        """WAV filename breaking parseSampleName() convention MUST fail Check 3."""
        pack_folder = self.temp_dir / "Test Pack"
        # Rename valid sample to non-compliant filename
        old_wav = pack_folder / "Test Pack-C3-V127-01.wav"
        new_wav = pack_folder / "bad_sample_name_c3.wav"
        old_wav.rename(new_wav)

        # Update SFZ and JSON to point to new name
        sfz_path = pack_folder / "Test Pack.sfz"
        sfz_path.write_text(sfz_path.read_text().replace("Test Pack-C3-V127-01.wav", "bad_sample_name_c3.wav"))
        
        json_path = pack_folder / "Test Pack.json"
        json_path.write_text(json_path.read_text().replace("Test Pack-C3-V127-01.wav", "bad_sample_name_c3.wav"))

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with non-compliant filename should fail validation")

    def test_stress_06_corrupt_16bit_wav_header_fails(self):
        """WAV audio with 16-bit PCM instead of 24-bit PCM MUST fail Check 4."""
        pack_folder = self.temp_dir / "Test Pack"
        wav_path = pack_folder / "Test Pack-C3-V127-01.wav"
        wav_path.unlink()
        self._create_synthetic_wav(wav_path, sample_rate=44100, sampwidth=2, num_channels=2) # sampwidth=2 is 16-bit

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with 16-bit WAV sample should fail validation")

    def test_stress_07_corrupt_48khz_wav_header_fails(self):
        """WAV audio with 48kHz sample rate instead of 44.1kHz MUST fail Check 4."""
        pack_folder = self.temp_dir / "Test Pack"
        wav_path = pack_folder / "Test Pack-C3-V127-01.wav"
        wav_path.unlink()
        self._create_synthetic_wav(wav_path, sample_rate=48000, sampwidth=3, num_channels=2)

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with 48kHz WAV sample should fail validation")

    def test_stress_08_corrupt_empty_wav_file_fails(self):
        """Corrupted/0-byte WAV sample MUST fail Check 4."""
        pack_folder = self.temp_dir / "Test Pack"
        wav_path = pack_folder / "Test Pack-C3-V127-01.wav"
        wav_path.write_bytes(b"CORRUPTED_NOT_A_WAV")

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with corrupted/invalid WAV file should fail validation")

    def test_stress_09_corrupt_key_range_gap_fails(self):
        """SFZ/JSON with key range gaps (e.g. 0..50 and 60..127) MUST fail Check 5."""
        pack_folder = self.temp_dir / "Test Pack"
        sfz_path = pack_folder / "Test Pack.sfz"
        # Gap between key 50 and 60
        sfz_lines = [
            "<region> sample=Test Pack-C3-V127-01.wav lokey=0 hikey=50 pitch_keycenter=60",
            "<region> sample=Test Pack-C4-V127-01.wav lokey=60 hikey=127 pitch_keycenter=72"
        ]
        sfz_path.write_text("\n".join(sfz_lines), encoding='utf-8')

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with key range gap should fail validation")

    def test_stress_10_corrupt_pitch_outside_key_range_fails(self):
        """SFZ with pitch_keycenter outside lokey..hikey range MUST fail Check 5."""
        pack_folder = self.temp_dir / "Test Pack"
        sfz_path = pack_folder / "Test Pack.sfz"
        sfz_lines = [
            "<region> sample=Test Pack-C3-V127-01.wav lokey=0 hikey=66 pitch_keycenter=70", # pitch 70 > hikey 66
            "<region> sample=Test Pack-C4-V127-01.wav lokey=67 hikey=127 pitch_keycenter=72"
        ]
        sfz_path.write_text("\n".join(sfz_lines), encoding='utf-8')

        res = validate_pack(str(pack_folder))
        self.assertFalse(res, "Pack with pitch_keycenter outside key range should fail validation")

    def test_stress_11_native_zone_pack_passes_without_sfz(self):
        """Native Nord rate/bit depth and modern zones must validate losslessly."""
        pack_folder = self._build_native_zone_pack(self.temp_dir)
        self.assertTrue(validate_pack(str(pack_folder)))

    def test_stress_12_native_zone_declared_format_mismatch_fails(self):
        pack_folder = self._build_native_zone_pack(self.temp_dir)
        sidecar = pack_folder / "churchdaw-timbre.json"
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        data["zones"][0]["sampleRate"] = 44100
        sidecar.write_text(json.dumps(data, indent=2), encoding="utf-8")
        self.assertFalse(validate_pack(str(pack_folder)))

    def test_stress_13_native_zone_hash_and_loop_mismatch_fail(self):
        pack_folder = self._build_native_zone_pack(self.temp_dir)
        sidecar = pack_folder / "churchdaw-timbre.json"
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        data["zones"][0]["sha256"] = "0" * 64
        data["zones"][1]["sourceLoop"]["endFrame"] = 999999
        sidecar.write_text(json.dumps(data, indent=2), encoding="utf-8")
        self.assertFalse(validate_pack(str(pack_folder)))


if __name__ == "__main__":
    unittest.main()
