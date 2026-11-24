# Application integration

How the XMB mode is wired into EmuCoreA. The native scene lives in
`:native-ui:android`; the app-side glue is in
`app/src/main/java/com/sbro/emucorea/ui/xmb/`.

## Overview

- `:app` depends on `:native-ui:android`. The XMB is a library mode of the home
  screen (`HomeScreen` shelf toggle leads to `XmbScreen`). It can also be
  opened as `XmbRoute` with the `xmb_mode` intent extra.
- `XmbScreen` hosts the GL surface in Compose, supplies the `HomeViewModel`
  library (DLC filtered out), the games' ICON0/PIC0/PIC1/SND0 and the save
  states, and forwards `XmbHost.onGameSelected(index)` to the normal emulation
  launch. The `core` module is not involved in browsing the menu.
- The mode locks the screen to landscape while it is shown.

## Firmware setup

The mode has no resources until the user's own firmware is installed (see the
[README](../README.md#firmware-and-resources)). `XmbScreen` therefore shows
`XmbFirmwareSetup` (app module, `ui/xmb/`) until `XmbResources.isInstalled()`:

1. While the library is still loading, it waits.
2. It scans the library's disc images (ISO/CSO/CHD, not archives) with
   `NativePpsspp.nativeReadUpdaterVersionFd`. That reads only the updater's
   `PARAM.SFO`. A 6.60/6.61 updater stops the scan. Otherwise the newest one wins.
3. When one is found, it is installed without asking. Progress covers the unpack
   (`nativeUnpackUpdaterFd`, `prefixFilter = "flash0:/"`, into
   `cacheDir/xmb-firmware/flash0`) and the resource build
   (`XmbResources.install`). The temporary firmware is then deleted.
4. When none is found, or the install fails, the screen explains why and offers
   **Choose file** (an `EBOOT.PBP`, a `DATA.BIN` or a disc image through the
   system picker) and **Back**.

After a successful install the screen switches to the XMB scene. On later
starts the mode opens directly. Back on the setup screen leaves the mode, except
while an install is running.

### Which firmware is accepted

- Any 6.xx updater works. After extraction, `XmbResources.alignImages` places
  every image the renderer loads at its expected path by the SHA-256 of its
  6.60 source payload (`XmbExpectedImages`), because the positional labels of
  `topmenu_icon.rco` entries shift between versions.
- A positional image that no extracted file matches belongs to a feature that
  firmware lacks. Its path is left empty and the renderer hides that entry
  (`RebuildCategories`), as that firmware's own menu would. The Game column is
  exempt because its icons are required.
- If a required image is missing (category icons, Game column icons), or the
  firmware is older than 6.00, `XmbResources.install` throws
  `UnsupportedFirmwareException`, the staging directory is discarded and the
  setup screen reports it. A source whose install failed is skipped by later
  scans in the same process.

### Background upgrade

When the installed resources came from a firmware other than 6.60/6.61,
`XmbScreen` calls `XmbFirmwareInstaller.upgradeIfBetter` once the library has
loaded:

- It runs once per process, off the main thread, and scans the library
  quietly. It does nothing if a better updater (a 6.60/6.61 one, or a newer
  version) is not found.
- When one is found it is installed with
  `XmbResources.install(keepPrevious = true)`. The tree the running XMB uses
  stays on disk and is not touched.
- The new tree is used the next time the mode opens (`current` points at it).
  The old one is removed the first time `XmbResources.installed` runs in the next
  process, together with any tree left by an older build.
- A failed upgrade is logged and leaves the current resources in place.

## Library and selection

- The Game column lists the games the app's home screen exposes (DLC excluded),
  each with its title, ICON0 icon, PIC1 background, PIC0 banner and SND0 music.
  Assets are read from the game files off the main thread (`PspGameMetadataReader`),
  cached by path, size and modification time, and pushed to native with
  `nativeSetGames`. SND0 is decoded after the visual assets so the list appears
  first. The list updates when the library changes.
- Saved Data Utility lists the app's save states (`SaveStateRepository`).
- Confirm, or Start in the options menu, plays the gameboot sequence and then
  calls `XmbHost.onGameSelected(index)`; the app navigates to `EmulationRoute`
  as it does from the other library views. Selecting a save launches the game
  with that save slot.
- Delete in the options menu calls `XmbHost.onDeleteGame` / `onDeleteSave`
  after the in-XMB confirmation; the host deletes the file or save.
- The selected category and item are stored by `XmbScreen` in the `xmb_state`
  preferences when the app pauses, the mode closes or a game launches, and
  restored with `nativeSetState` the next time the scene is created, including
  after a restart of the process. The game list and save list positions are
  stored as well.
- When emulation ends, the app returns to the XMB. A return from a game plays
  the shorter resume transition; entering the mode otherwise plays the opening.

## Settings

The Settings column is the XMB's own (see [renderer](renderer.md)). Values
come back through `XmbHost.onSettingsChanged` and are stored in the `xmb_state`
preferences by `XmbScreen`, which pushes them back to native at the next start.
The audio settings drive `XmbAudio`, "swap confirm" drives
`XmbHost.swapConfirmButton`, and keep-screen-on sets `FLAG_KEEP_SCREEN_ON` on
the window. Slideshow, parental control level, ad hoc channel and RSS auto
update are stored but nothing else reads them.

## Touch without a gamepad

When no physical gamepad is connected and touch controls are enabled, the
scene runs in touch mode and shows a hint bar and, once, a tutorial dialog.
Gestures are the whole input:

- swipe up/down: next/previous item; swipe left/right: next/previous category;
- tap: confirm, or press the control that was touched;
- long press: options menu on the game and save lists;
- two-finger tap and system Back: Back (item list, then the quit confirmation).

Gestures are recognised in `XmbScreen`'s touch listener (tap slop, move-cancel
slop and the 420 ms long press are defined there) and feed the same
`KeyPressed` path as the D-pad, so animation, sounds and selection state are
the same.

## Localisation

Labels come from the firmware's text catalogue (`text/en.txt`, built during
extraction) and from the app's `xmb_*` string resources, which `XmbScreen` pushes
to native with `nativeSetStrings` and which override the catalogue. All 212
`xmb_*` strings exist in the app's 18 string sets (`values` and 17 locale
folders), and the strings are pushed again when the app language changes.
Characters missing from the firmware font are drawn with the Android system
font.

## Lifecycle

- Pausing the app stops the SND0 preview, saves the selection and pauses the GL
  surface. Resuming restarts rendering and refreshes status and touch mode.
- Leaving the mode (quit confirmation, or Back from the setup screen) stops
  audio, clears the orientation override and unregisters the battery and status
  receivers.
- `XmbHost` listeners are installed when the screen enters composition and
  cleared when it leaves, so toggling the mode repeatedly does not accumulate
  them.
