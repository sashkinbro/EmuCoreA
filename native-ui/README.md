# Native PSP XMB reconstruction

This directory holds the standalone reconstruction of the PSP's Cross Media Bar
(XMB), starting with the **Game** column: the vertical game list with its icons,
selection frame, info panel, navigation sounds and the animated wave background.

The target is the shipped 6.60/6.61 System Software, not an imitation of it. The
finished view must render and animate exactly like the firmware's own VSH, at
60 FPS, with no emulation core, no PPSSPP UI and no guest CPU involved at
runtime. Every animation, sound, icon, font and layout constant is traced back
to an original resource or an original instruction sequence; anything that is
still an approximation is listed as such below.

The host application supplies the installed-game list and owns launching a
game. This module owns drawing, input, audio playback and the resource pack.
Browsing the menu must never initialize the emulation core.

## Firmware and resources

Firmware dumps and extracted resources are **local development inputs** and are
never committed. `native-ui/firmware/` and `native-ui/resources/` are ignored by
Git. The ready resources are packed into one ZIP that is versioned, installed
once into app-private storage on first start, and read from there by Kotlin and
C++ (staged extraction, atomic rename, no per-launch unpacking).

Source of the original files, in order of preference:

1. A firmware updater carried by a UMD the user owns. Many PSP discs include
   `PSP_GAME/SYSDIR/UPDATE/DATA.BIN`, a `PSAR` archive. `tools/extract_psp_updater.py`
   reads it straight out of an ISO or CISO image and reports the version from
   the updater's own `PARAM.SFO`.
2. An official download-only updater (`EBOOT.PBP`), subfile 7 (`DATA.PSAR`).
3. A flash0 dump taken from the user's own PSP.

The archive is decrypted with the keys PPSSPP's PSAR/PRX decrypter contains;
that work happens offline, in a development build, and never at UI startup.
The XMB's own files live under `flash0:/vsh/resource/` (`.rco`), its fonts under
`flash0:/font/`, and its encrypted item index under `flash0:/vsh/etc/`.

## Pipeline

```text
owned UMD updater --> PSAR unpack --> flash0/ (local)
flash0/*.rco      --> RCO reader  --> GIM images, VAG sounds, PGF fonts,
                                     object/animation definitions (local)
ready files       --> pack builder --> native-ui.zip + version/hash (local)
APK asset         --> BundledResources --> app-private versioned directory
```

The runtime never reads firmware containers. Resource preparation is offline
tooling only; none of it ships in the application.

## Layout

```text
native-ui/
  psp/        independent PSP format readers (RCO, GIM, VAG, PGF)
  ui/         XMB scene, layout, animation, input, audio
  android/    Kotlin host: surface, lifecycle, pack installer, JNI bridge
  tools/      offline extraction, verification and pack building
  tests/      golden-data and parser tests
  preview/    standalone preview app used while integrating
  resources/  generated, ignored
  firmware/   generated, ignored
```

## Status

- [x] Directory, source of truth and build layout
- [x] UMD updater extraction from an owned disc image
- [ ] PSAR unpack to a local flash0 tree
- [ ] RCO/GIM/VAG parsing and resource extraction
- [ ] Resource pack + first-start installer
- [ ] Game column renderer, animations and sounds
- [ ] Frame-for-frame comparison against the firmware XMB
- [ ] Host integration in place of the current 3D carousel

Nothing is claimed equivalent until it has been compared against the original,
and every remaining difference is written down.
