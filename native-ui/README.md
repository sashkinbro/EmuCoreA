# Native PSP XMB reconstruction

This directory holds EmuCoreA's XMB mode: a native reconstruction of the PSP's
Cross Media Bar with all eight categories, the game list with per-game
backgrounds and music, the in-XMB Settings column, themes, the opening and
gameboot sequences, the animated wave and the status bar.

Layout and timing are measured against the 6.60/6.61 System Software, and the
images, sounds and fonts come from the user's own copy of it at runtime. The
view is drawn by its own OpenGL ES renderer, with no emulation core, no PPSSPP
UI and no guest CPU involved. EmuCoreA is not affiliated with Sony. It does not
present itself as a Sony system: no model number, system-software release or
company emblem is shown.

The host application supplies the installed-game list and owns launching a
game. This module owns drawing, input, audio playback and the resource tree.
Browsing the menu never boots the emulator.

## Firmware and resources

**EmuCoreA does not distribute any Sony file or anything derived from one.**
The APK carries no images, sounds, fonts, models or text from the PSP system
software. The XMB's resources are built on the user's device, once, from a
firmware the user already has:

1. **A game disc in the user's library.** Most UMDs carry a firmware updater in
   `PSP_GAME/SYSDIR/UPDATE/DATA.BIN`. When the XMB mode is opened and nothing is
   installed yet, the app scans the library for one. Reading an updater's
   version only touches its `PARAM.SFO`, so the scan is quick. A 6.60/6.61
   updater ends the scan at once. Otherwise the newest one found is used. If
   one is found, it is installed without asking.
2. **A file the user picks** when no game has one: an official updater
   `EBOOT.PBP`, a bare `DATA.BIN`, or a disc image (ISO/CSO/CHD) that carries
   one.

Any 6.xx updater works. The menu icons in `topmenu_icon.rco` have positional
labels that shift between versions, so after extraction every image the
renderer loads is matched by the digest of its 6.60 source payload
(`XmbExpectedImages`) and moved to the path the renderer expects. An icon that
version doesn't have belongs to a feature it didn't have yet (SensMe on 6.31,
for example). Its entry is hidden, as that firmware's own menu would hide it.
A firmware without the category or Game-column icons (anything before 6.00)
is rejected with a message, and the next disc is tried.

The app never downloads firmware and never sends it anywhere. Decryption uses
the PSAR unpacker that is already part of the PPSSPP core
(`core/Core/Util/PSARUnpack.cpp`, GPL-2.0-or-later). The unpacked firmware is
temporary: it is deleted as soon as the resource tree is built.

```text
user's game disc / updater
  --> NativePpsspp.nativeUnpackUpdaterFd   (core PSAR unpacker, temp flash0/)
  --> XmbNative.nativeExtractResources     (RCO/GIM/VAG/PGF/theme extraction)
  --> noBackupFilesDir/native-ui/<id>/     (ready tree + manifest.json)
```

Firmware dumps used during development (`native-ui/firmware/`), the Python
reference output (`native-ui/resources/`) and reference captures
(`native-ui/reference/`) are local inputs only and are ignored by Git.

## Android host module

`:native-ui:android` is an Android library with the Kotlin host and the C++
renderer. It does not depend on `:app` or `:core-native`.

- `XmbResources` owns the installed tree. `install(context, flash0)` runs the
  native extractor into a staging directory, writes a completion marker
  (`xmb-resources-v3`) and renames the directory into
  `noBackupFilesDir/native-ui/<sha256 of manifest>/`. An interrupted install
  never looks installed. Installs are serialized in-process and with a file
  lock. Trees left by older builds, including the APK-bundled pack, are
  removed.
- `XmbSession.initialize` hands the installed root to the native side and to
  `XmbAudio`. It throws `XmbResources.MissingException` while nothing is
  installed.
- The app module's `XmbScreen` shows `XmbFirmwareSetup` instead of the scene
  until `XmbResources.isInstalled()`. `XmbFirmwareInstaller` holds the library
  scan and the unpack-then-extract sequence.

`native-ui/preview` is a dependency-free launch host for developing the scene.
It has no updater unpacker. To give it resources, push an unpacked
flash0 to its external files directory
(`adb push flash0 /sdcard/Android/data/<package>/files/flash0`). It installs
from there on start.

## Layout

```text
native-ui/
  android/    Kotlin host, JNI bridge, C++ renderer and on-device extractor
  tools/      offline reference tools: updater/RCO/GIM/VAG/PGF readers in Python
  tests/      golden-data and parser tests (use the local dump when present)
  scene/      measured category anchors (reference data, not read at runtime)
  docs/       integration, renderer and reference measurements
  preview/    standalone preview app used while developing the scene
  resources/  Python reference output, ignored
  firmware/   local firmware inputs, ignored
```

The Python tools in `tools/` are the reference implementation of the format
handling, and the oracle the C++ extractor is checked against. They are
development tools only and do not run on the device.

## Features

Navigation and input

- All eight categories (Settings, Extras, Photo, Music, Video, Game, Network,
  Online) with item columns, animated category and item scrolling, the focus
  glow, and the firmware's navigation sounds.
- D-pad, gamepad buttons (confirm/back swappable) and touch: tap, swipe, long
  press for the options menu, a two-finger tap for Back, and a first-use touch
  tutorial. Back opens a quit confirmation. Touch can be turned off in Settings.
- The mode is the home screen's library view, locks to landscape, and keeps the
  selected category and item across app restarts.

Game column

- Memory Card lists the games from the app library with their ICON0, PIC1
  background and PIC0 banner. The game's SND0 music plays while it is focused.
- Confirm, or Start in the options menu, plays the gameboot sequence and then
  launches the game through the app's normal emulation path. Closing the game
  returns to the XMB.
- The options menu offers Start, Delete and Information (title, size, version,
  required version, disc ID, region, date).
- Saved Data Utility lists the app's save states with the same options menu.

Settings column

- Theme (style, colour, background, wave), Sound (key tone, background music,
  AVLS), Date & Time, System (system information, restore defaults), Video and
  Connected Display (aspect mode, touch controls), Power Save (keep screen on)
  and the other firmware entries. Changes apply immediately and persist.
- Entries with no counterpart in EmuCoreA (slideshow, parental control level,
  ad hoc channel, RSS auto update, sequential play, WLAN power save) store the
  chosen value or only change what is displayed. System Update, Game Sharing,
  Remote Play and the online services open a short notice dialog.
- Internet Browser and Internet Search ask for confirmation and open the device
  browser.

Look and sound

- The animated wave from the firmware model, the background gradient, the 12
  themes (Original, Classic, Cookie, Piano Black and others) and the monthly
  and extended colours. Colours are read from the installed firmware's swatches.
- Opening and gameboot sequences that show the EmuCoreA name.
- Status bar: clock with date and 12/24-hour formats, battery with charging
  animation, Wi-Fi, Bluetooth, AVLS and mute indicators, memory card activity
  while loading.
- Text in the firmware's PGF fonts, with the Android system font for characters
  the firmware font lacks. All `xmb_*` strings exist in the app's 18 string
  sets.

Resources

- Built on the device from the user's own firmware; nothing from it is in the
  APK. Any 6.xx updater works, and a better one found later replaces the
  installed resources in the background.

## Known differences from the firmware

- Wave placement, brightness and the glow around the selected icon are tuned by
  eye against reference captures and are not pixel-exact.
- The November (Mahogany) theme colour is EmuCoreA's own value. A colour name
  whose swatch is missing from the firmware uses a built-in approximation.
- Text scale and baseline are set per text style from measurements. Text
  shadows are a one-pixel offset copy of the glyph.
- Characters outside the firmware's Latin font come from the Android system
  font.
- The opening sequence is composed from the firmware's glow textures rather
  than played from `opening_plugin`. The gameboot sequence uses the firmware's
  gameboot sound and backdrop gradient; its logo is replaced by the EmuCoreA
  name.
- The 480x272 frame is widened to the device's aspect ratio when the aspect
  mode is Full Screen.
- Reference captures cover 6.60. Other 6.xx firmware is handled by the digest
  matching described above but has not been compared frame by frame.

## License

The XMB mode is licensed under **GPL-2.0-or-later**, the same license as the
PPSSPP core it is built with. The full text is in [LICENSE](LICENSE). This
covers everything under `native-ui/` and the app-side XMB files in
`app/src/main/java/com/sbro/emucorea/ui/xmb/`. Every source file carries an
`SPDX-License-Identifier: GPL-2.0-or-later` header. Third-party parts and their
own licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

PSP, PlayStation and XMB are trademarks of Sony Interactive Entertainment.
EmuCoreA is not affiliated with or endorsed by Sony, and it contains no Sony
code, images, sounds or fonts.
