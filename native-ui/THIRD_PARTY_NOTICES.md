# Third-party notices

## Vlnky wave reader

`native-ui/android/src/main/cpp/third_party/vlnky/` contains the GMO/PGF wave
reader and RCO helpers from Vlnky's `libxmbwave`:

- upstream: https://github.com/tuxinaut/Vlnky (see `out/refs/Vlnky` in the local
  development tree)
- copyright: 2026 Pavel Švec
- license: GPL-2.0-or-later (SPDX headers are preserved in every copied file)

The files are compiled unmodified. The native renderer uses
`parsePspWaveGmo()` and `PspWaveTessellator` to read the firmware's own
`mdl_bg.gmo` control mesh, knot vectors, morph tracks and reflection texture.
The shell's colour/placement around that geometry is our own code and is not
part of Vlnky.

## PPSSPP

The PGF reader in `native-ui/tools/psp/pgf.py` and
`native-ui/android/src/main/cpp/pgf_font.cpp` is a port of the format handling
in PPSSPP's `Core/Font/PGF.cpp` (GPL-2.0-or-later). The application embeds the
same GPLv2 core fork, so the license is compatible; the implementation here is
independent of the emulator at runtime.

## ATRAC3/ATRAC3plus decoder (FFmpeg-derived)

`libemucorea_xmb.so` compiles `core/ext/at3_standalone/` (PPSSPP's standalone
extract of FFmpeg's ATRAC decoders) to play the firmware's gameboot sound and
games' SND0 music. Those files are licensed LGPL-2.1-or-later; their headers
carry the original FFmpeg copyright notices.

## PSAR unpacker and KIRK (PPSSPP)

The firmware updater is unpacked on the device by `core/Core/Util/PSARUnpack.cpp`
and the PRX/KIRK code it uses (`core/Core/ELF/PrxDecrypter.cpp`,
`core/ext/libkirk/`), all part of the GPL-2.0-or-later PPSSPP core. They are
called from `core-native` (`nativeReadUpdaterVersionFd`,
`nativeUnpackUpdaterFd`).

## PSPSDK

PPSSPP includes code under the PSPSDK BSD license; its notice is reproduced in
the repository's `LICENSE.TXT` and must accompany binary distributions.

## Own code

The format readers and the on-device extractor (`png_image.cpp`, `pgf_font.cpp`
apart from the PPSSPP-derived layout noted above, `psp_rlz.cpp`, `psp_gim.cpp`,
`psp_vag.cpp`, `xmb_extract.cpp`, `tools/psp/*.py`) were written for EmuCoreA
from the published format notes cited in `tools/README.md`. They are licensed
GPL-2.0-or-later like the rest of the XMB mode (see `LICENSE`).
