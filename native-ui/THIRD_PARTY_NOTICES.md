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
