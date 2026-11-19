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

## Android host module

`:native-ui:android` is an Android library with the Kotlin host and the C++
renderer entry point. It does not depend on `:app` or `:core-native`, so
building it cannot initialize the emulation core.

`BundledResources` copies the APK asset `native-ui.zip` into a directory named
after the pack's SHA-256 under `noBackupFilesDir/native-ui/`, extracts it into a
staging directory, writes a completion marker and renames the directory into
place. Installs are serialized in-process and with a file lock; entries are
checked for absolute paths, `..`, entry count and total expansion. A partial
install is removed and retried on the next call. `XmbSession.initialize` hands
the installed root to the native side once.

The pack is produced from the ignored resource tree during the module build:

```text
python native-ui/tools/pack_resources.py
```

The ZIP is deterministic (sorted entries, fixed timestamps), so unchanged
resources always produce the same SHA-256 and are installed only once.

`native-ui/preview` is a dependency-free launch host used while the scene is
built. It installs the pack, reports the native version and root, and has no
game library or emulation core attached. Verified on a Lenovo TB710FU: the
6.60 pack installs once into `no_backup/native-ui/<sha256>/`, with 333 files
(305 images, 8 sounds), and a relaunch reuses it.

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
- [x] PSAR unpack to a local flash0 tree
- [x] RLZ table decoder, verified on all 48 RLZ tables in the 6.60 firmware
- [x] RCO/GIM/VAG parsing and resource extraction (305 images, 8 sounds,
      2 models and 16 fonts from the shell containers)
- [x] Resource pack + first-start installer
- [x] Reference oracle: the real 6.60 XMB captured through the debugger
      ([measurements](docs/reference.md))
- [x] Native GLES scene skeleton with the measured category/item anchors and
      original textures ([renderer](docs/renderer.md))
- [x] PGF font reader and native text with original glyph bitmaps
- [x] Live navigation: D-pad/touch, measured scroll/slide easing, original
      navigation sounds
- [x] Live wave mesh, background ramp, clock and battery (wave placement and
      glow are not yet pixel-exact)
- [x] Real item columns for all eight categories with cross-fades and the
      firmware focus glow
- [ ] Game column renderer parity (wave/theme exactness, transient status,
      game backgrounds)
- [ ] Host integration, lifecycle, in-XMB Settings column and localisation
      ([plan](docs/integration.md))
- [ ] Frame-for-frame comparison against the firmware XMB
- [ ] Host integration in place of the current 3D carousel

Nothing is claimed equivalent until it has been compared against the original,
and every remaining difference is written down.
