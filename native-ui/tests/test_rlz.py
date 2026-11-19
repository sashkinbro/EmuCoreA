"""Tests for the PSP RCO RLZ decoder.

The firmware fixtures are local development inputs and are never committed, so
the firmware-wide test skips when the dump is absent. The small fixture below
was produced by an encoder that reproduces the same range coder and model; it
guards the decoder's bitstream against regressions without shipping Sony data.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "native-ui" / "tools"))

from psp import rlz  # noqa: E402

FIXTURE = bytes.fromhex(
    "06ff7fb77fffffffc11e54763b53ffffdc96c6959926eaf1615cf50000"
)
FIXTURE_OUTPUT = bytes.fromhex(
    "0101000000000000280000000300000000000000000000000000000000000000"
    "04000000ffffffff00000000"
)


class RlzTest(unittest.TestCase):
    def test_known_fixture(self) -> None:
        out, info = rlz.decompress(FIXTURE, len(FIXTURE_OUTPUT))
        self.assertEqual(out, FIXTURE_OUTPUT)
        self.assertEqual(info.mode, 6)
        self.assertEqual(info.output_size, len(FIXTURE_OUTPUT))

    def test_truncated_stream_is_rejected(self) -> None:
        with self.assertRaises(rlz.RlzError):
            rlz.decompress(FIXTURE[:12], len(FIXTURE_OUTPUT))

    def test_declared_size_mismatch_is_rejected(self) -> None:
        with self.assertRaises(rlz.RlzError):
            rlz.decompress(FIXTURE, len(FIXTURE_OUTPUT) + 16)

    def test_empty_stream_is_rejected(self) -> None:
        with self.assertRaises(rlz.RlzError):
            rlz.decompress(b"", 1)


class FirmwareTablesTest(unittest.TestCase):
    RESOURCE_DIR = REPO / "native-ui" / "firmware" / "flash0" / "vsh" / "resource"

    def test_every_firmware_table_decodes_to_its_declared_size(self) -> None:
        if not self.RESOURCE_DIR.is_dir():
            self.skipTest("local firmware dump is not present")
        decoded = 0
        for path in sorted(self.RESOURCE_DIR.glob("*.rco")):
            data = path.read_bytes()
            if len(data) < 0xB0:
                continue
            compression = struct.unpack_from("<I", data, 0x0C)[0]
            if (compression >> 4) != 2:  # only RLZ tables are this decoder's job
                continue
            packed, unpacked, _ = struct.unpack_from("<III", data, 0xA4)
            self.assertLessEqual(0xB0 + packed, len(data), path.name)
            table, info = rlz.decompress(data[0xB0:0xB0 + packed], unpacked)
            self.assertEqual(len(table), unpacked, path.name)
            self.assertEqual(info.output_size, unpacked, path.name)
            # Every decompressed table starts with the main table entry.
            self.assertEqual(table[:2], b"\x01\x01", path.name)
            decoded += 1
        self.assertEqual(decoded, 48)


if __name__ == "__main__":
    unittest.main()
