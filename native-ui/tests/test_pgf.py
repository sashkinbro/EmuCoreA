# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for the firmware PGF font reader.

The font fixture is local firmware and is skipped when absent. The expected
values were checked against the real 6.60 Boot / XMB rendering; they are
glyph metrics, not an image comparison.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "native-ui" / "tools"))

from psp.pgf import Pgf  # noqa: E402

FONT = REPO / "native-ui" / "firmware" / "flash0" / "font" / "ltn0.pgf"


@unittest.skipUnless(FONT.is_file(), "local firmware dump is not present")
class PgfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.font = Pgf(FONT)

    def test_identity(self) -> None:
        self.assertEqual(self.font.font_name, "FTT-NewRodin Pro Latin")
        self.assertEqual(self.font.first_glyph, 32)
        self.assertEqual(len(self.font.glyphs), 324)

    def test_ascii_glyph(self) -> None:
        glyph = self.font.glyph("G")
        assert glyph is not None
        self.assertEqual((glyph.width, glyph.height), (15, 16))
        self.assertEqual(glyph.left, 0)
        self.assertEqual(glyph.top, 15)
        self.assertEqual(glyph.advance_h, 1000)
        self.assertEqual(len(glyph.pixels), 15 * 16)
        self.assertEqual(max(glyph.pixels), 15)

    def test_vertical_row_glyph_is_returned_row_major(self) -> None:
        glyph = self.font.glyph("m")
        assert glyph is not None
        self.assertEqual((glyph.width, glyph.height), (16, 12))
        self.assertEqual(glyph.flags & 0x03, 0x02)  # FONT_PGF_BMP_V_ROWS
        self.assertEqual(len(glyph.pixels), 16 * 12)

    def test_advances(self) -> None:
        self.assertAlmostEqual(self.font.text_width("Game"), 58.25, places=3)
        self.assertAlmostEqual(self.font.text_width("Saved Data Utility"), 184.484375, places=3)
        self.assertAlmostEqual(self.font.text_width("Memory Stick"), 139.75, places=3)

    def test_missing_glyph(self) -> None:
        self.assertIsNone(self.font.glyph("\u2603"))  # not in the Latin font


if __name__ == "__main__":
    unittest.main()
