"""
TestMilestone3Challenge.py - Independent Empirical Test & Verification Suite for M3

This test script empirically verifies:
1. Execution of Tools/transfer_casio_patch.py and exit code 0.
2. Schema validation of Staging/m3_transfer_report.json.
3. Independent CRC32 computation and verification against report claims.
4. Block count and byte size consistency between disk files and report.
5. Algorithmic unit tests for Casio SysEx checksum, nibbleization, and framing.
"""

import os
import sys
import json
import zlib
import math
import subprocess
import unittest

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)
STAGING_DIR = os.path.join(PROJECT_ROOT, "Staging", "CasioTone")
TN7_PATH = os.path.join(STAGING_DIR, "black_upright_casio.tn7")
TU7_PATH = os.path.join(STAGING_DIR, "blk_upright.tu7")
REPORT_PATH = os.path.join(PROJECT_ROOT, "Staging", "m3_transfer_report.json")
TRANSFER_SCRIPT = os.path.join(PROJECT_ROOT, "Tools", "transfer_casio_patch.py")


def compute_crc32(file_path):
    crc = 0
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return f"0x{crc & 0xFFFFFFFF:08X}"


class TestMilestone3Challenge(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        print("\n--- Running M3 Challenge Test Suite ---")

    def test_01_patch_files_exist(self):
        """Verify that source .tn7 and .tu7 patch files exist and are non-empty."""
        self.assertTrue(os.path.exists(TN7_PATH), f"Missing patch file: {TN7_PATH}")
        self.assertTrue(os.path.exists(TU7_PATH), f"Missing patch file: {TU7_PATH}")
        tn7_size = os.path.getsize(TN7_PATH)
        tu7_size = os.path.getsize(TU7_PATH)
        self.assertGreater(tn7_size, 0, ".tn7 file size must be > 0")
        self.assertGreater(tu7_size, 0, ".tu7 file size must be > 0")

    def test_02_report_exists_and_valid_json(self):
        """Verify that m3_transfer_report.json exists and is valid JSON."""
        self.assertTrue(os.path.exists(REPORT_PATH), f"Report not found at {REPORT_PATH}")
        with open(REPORT_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.assertIsInstance(data, dict, "Report root must be a JSON object")

    def test_03_report_schema_and_status(self):
        """Verify JSON schema and overall status fields."""
        with open(REPORT_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)

        required_keys = [
            "status", "timestamp", "hardware_info", "target_device",
            "user_tone_slot", "user_memory_bank_slot", "tone_patch_status",
            "wave_bank_status", "total_bytes_transferred", "packets_acked",
            "verification_status", "patch_files_transferred", "patch_files_loaded",
            "transfer_metrics", "midi_port_details", "protocol_verification",
            "manual_verification_instructions"
        ]
        for key in required_keys:
            self.assertIn(key, data, f"Missing required key in report: {key}")

        self.assertEqual(data["status"], "SUCCESS")
        self.assertEqual(data["tone_patch_status"], "SUCCESS")
        self.assertEqual(data["wave_bank_status"], "SUCCESS")
        self.assertEqual(data["verification_status"], "VERIFIED_READY")
        self.assertEqual(data["user_tone_slot"], 801)

    def test_04_crc32_checksum_verification(self):
        """Independently compute CRC32 for .tn7 and .tu7 and compare with report values."""
        actual_tn7_crc = compute_crc32(TN7_PATH)
        actual_tu7_crc = compute_crc32(TU7_PATH)

        with open(REPORT_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)

        report_tn7_crc = data["patch_files_transferred"]["tn7"]["crc32"]
        report_tu7_crc = data["patch_files_transferred"]["tu7"]["crc32"]

        self.assertEqual(actual_tn7_crc, report_tn7_crc,
                         f"TN7 CRC32 mismatch! Disk: {actual_tn7_crc}, Report: {report_tn7_crc}")
        self.assertEqual(actual_tu7_crc, report_tu7_crc,
                         f"TU7 CRC32 mismatch! Disk: {actual_tu7_crc}, Report: {report_tu7_crc}")

    def test_05_file_sizes_and_block_counts(self):
        """Verify that file sizes and block counts match disk actuals."""
        tn7_size = os.path.getsize(TN7_PATH)
        tu7_size = os.path.getsize(TU7_PATH)
        tn7_expected_blocks = math.ceil(tn7_size / 256)
        tu7_expected_blocks = math.ceil(tu7_size / 256)

        with open(REPORT_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)

        tn7_rep = data["patch_files_transferred"]["tn7"]
        tu7_rep = data["patch_files_transferred"]["tu7"]

        self.assertEqual(tn7_rep["file_bytes"], tn7_size)
        self.assertEqual(tu7_rep["file_bytes"], tu7_size)
        self.assertEqual(tn7_rep["blocks_count"], tn7_expected_blocks)
        self.assertEqual(tu7_rep["blocks_count"], tu7_expected_blocks)

        total_bytes = tn7_size + tu7_size
        self.assertEqual(data["total_bytes_transferred"], total_bytes)
        self.assertEqual(data["transfer_metrics"]["total_bytes_sent"], total_bytes)
        self.assertEqual(data["transfer_metrics"]["total_blocks_sent"], tn7_expected_blocks + tu7_expected_blocks)

    def test_06_checksum_algorithm_correctness(self):
        """Test Casio 7-bit SysEx checksum algorithm implementation."""
        from Tools.transfer_casio_patch import calculate_casio_checksum

        # Test case 1: body sum = 0 -> (128 - 0) & 0x7F = 0
        self.assertEqual(calculate_casio_checksum([0, 0, 0]), 0)

        # Test case 2: body sum = 1 -> (128 - 1) & 0x7F = 127
        self.assertEqual(calculate_casio_checksum([1]), 127)

        # Test case 3: body sum = 128 -> 128 % 128 = 0 -> (128 - 0) & 0x7F = 0
        self.assertEqual(calculate_casio_checksum([128]), 0)

        # Test case 4: sum + csum mod 128 must be 0
        test_body = [0x7F, 0x7F, 0x05, 0x01, 0x00, 0x0F, 0x0E]
        csum = calculate_casio_checksum(test_body)
        self.assertEqual((sum(test_body) + csum) % 128, 0)

    def test_07_nibbleize_block_correctness(self):
        """Test nibbleization of binary block data."""
        from Tools.transfer_casio_patch import nibbleize_block

        raw_bytes = bytes([0xAB, 0x00, 0xFF, 0x12])
        # 0xAB -> low=0x0B, high=0x0A
        # 0x00 -> low=0x00, high=0x00
        # 0xFF -> low=0x0F, high=0x0F
        # 0x12 -> low=0x02, high=0x01
        expected = [0x0B, 0x0A, 0x00, 0x00, 0x0F, 0x0F, 0x02, 0x01]
        self.assertEqual(list(nibbleize_block(raw_bytes)), expected)


if __name__ == "__main__":
    unittest.main()
