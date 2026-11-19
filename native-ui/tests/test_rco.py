"""Tests for the PSP RCO reader and its GIM/VAG payload decoders.

Most assertions use the local firmware dump and skip when it is absent. The
synthetic VAG test keeps the ADPCM path covered in a clean checkout.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "native-ui" / "tools"))

from psp import gim, rco, vag  # noqa: E402

RESOURCE_DIR = REPO / "native-ui" / "firmware" / "flash0" / "vsh" / "resource"


@unittest.skipUnless(RESOURCE_DIR.is_dir(), "local firmware dump is not present")
class FirmwareResourceTest(unittest.TestCase):
    def test_game_plugin_images(self) -> None:
        document = rco.open_rco(RESOURCE_DIR / "game_plugin.rco")
        images = document.images()
        self.assertEqual(len(images), 20)
        labels = {image.label for image in images}
        self.assertIn("tex_loading", labels)
        for image in images:
            self.assertEqual(image.format, 5)
            self.assertTrue(image.data.startswith(b"MIG.00.1PSP"), image.label)

    def test_system_plugin_sounds(self) -> None:
        document = rco.open_rco(RESOURCE_DIR / "system_plugin.rco")
        sounds = {sound.label: sound for sound in document.sounds()}
        self.assertEqual(
            set(sounds),
            {"snd_cursor", "snd_decide", "snd_cancel", "snd_category_decide",
             "snd_option", "snd_error", "snd_system_ok", "snd_system_ng"},
        )
        cursor = vag.decode(sounds["snd_cursor"].channels[0].data)
        self.assertEqual(cursor.sample_rate, 44100)
        self.assertEqual(len(cursor.samples) // 2, 1848)
        self.assertTrue(sounds["snd_cursor"].channels[0].data.startswith(b"VAGp"))
        error = vag.decode(sounds["snd_error"].channels[0].data)
        self.assertEqual(len(error.samples) // 2, 112)

    def test_gim_decodes_a_category_icon(self) -> None:
        document = rco.open_rco(RESOURCE_DIR / "topmenu_icon.rco")
        first = document.images()[0]
        self.assertEqual(gim.info(first.data), {
            "format": 5, "pixel_order": 1, "width": 51, "height": 41, "bpp": 8,
        })
        image = gim.decode(first.data)
        self.assertEqual((image.width, image.height), (51, 41))
        self.assertEqual(len(image.pixels), 51 * 41 * 4)
        # The decoded sheet must not be a flat block: an XMB icon has several
        # alpha levels.
        alphas = set(image.pixels[3::4])
        self.assertGreater(len(alphas), 4)

    def test_model_payload_is_gmo(self) -> None:
        document = rco.open_rco(RESOURCE_DIR / "system_plugin_bg.rco")
        models = document.models()
        self.assertEqual(len(models), 1)
        self.assertTrue(models[0].data.startswith(b"OMG.00.1PSP"))

    def test_every_selected_rco_opens(self) -> None:
        names = ("system_plugin.rco", "system_plugin_bg.rco", "topmenu_plugin.rco",
                 "topmenu_icon.rco", "game_plugin.rco", "savedata_plugin.rco",
                 "opening_plugin.rco", "impose_plugin.rco")
        for name in names:
            document = rco.open_rco(RESOURCE_DIR / name)
            self.assertGreater(len(document.tables), 0, name)


class VagTest(unittest.TestCase):
    def _vag(self, payload: bytes) -> bytes:
        header = bytearray(0x30)
        header[0:4] = b"VAGp"
        struct.pack_into(">I", header, 0x04, 4)
        struct.pack_into(">I", header, 0x0C, len(payload))
        struct.pack_into(">I", header, 0x10, 44100)
        header[0x20:0x20 + 8] = b"test.aif"
        return bytes(header) + payload

    def test_silence(self) -> None:
        sound = vag.decode(self._vag(bytes(16)))
        self.assertEqual(sound.sample_rate, 44100)
        self.assertEqual(len(sound.samples), 28 * 2)
        self.assertEqual(sound.samples, bytes(28 * 2))

    def test_bad_header(self) -> None:
        with self.assertRaises(vag.VagError):
            vag.decode(b"not a vag")
        with self.assertRaises(vag.VagError):
            vag.decode(self._vag(bytes(15)))


class GimTest(unittest.TestCase):
    def test_rejects_foreign_data(self) -> None:
        with self.assertRaises(gim.GimError):
            gim.decode(b"\x89PNG\r\n\x1a\n" + bytes(32))


if __name__ == "__main__":
    unittest.main()
