# Application integration

How the XMB mode is wired into EmuCoreA. The sections below were written as the
plan and are kept as the rules the integration follows. "Current state" says
what is actually in place.

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

## Replace the active carousel

The current home screen's shelf/carousel is `HomeShelfMode.kt` behind the
`HomeLibraryViewMode.SHELF` branch chosen in `HomeScreen.kt`. The new XMB must be
an additional library mode, defaulting on for existing users only after the
migration is accepted:

- `HomeLibraryViewMode.XMB` (or a renamed shelf mode) selected by the existing
  home toggle/action.
- The mode hosts the `native-ui` GL surface inside a Compose `AndroidView`,
  fullscreen, with the same system-bar handling as the current shelf.
- Games come from the existing home/library repository and ViewModel. Each XMB
  item gets the existing title and cover/icon; no new scanner or database is
  introduced.
- The toggle and back stack use the app's existing navigation; no new activity
  unless the GL surface's lifecycle requires one.

The `core` module is not modified. The menu calls the application's existing
game-launch path.

## Lifecycle

1. Opening the app restores the last mode from `AppPreferences`. If the user
   closed the app while in XMB, XMB is shown again, with the selected item and
   category restored.
2. XMB lists every game the application currently exposes to the library; the
   list updates when the library changes (install/remove/scan) without losing
   the selection.
3. Confirm launches the game through the same path as the current home screen.
   The menu saves its state before launching.
4. When the emulation screen/process ends (Back/exit, game shutdown, process
   death), the app returns to XMB, not to the old carousel, preserving the
   selected item. If the process was killed, the launch intent/state is
   recovered from preferences and the existing emulation handoff.
5. Pause/resume of the app pauses the wave/animations but does not reset the
   selection. Audio focus follows the existing app lifecycle.
6. Back in XMB behaves like the PSP: item → category → home/leave; app close in
   XMB is a normal close and the mode is persisted.

## Other tabs

Every XMB category gets a real column and real item labels from the firmware
(system plugin, music, video, photo, network, PSN), with the original icons and
open/close animations. Items that have no implementation in EmuCoreA are
present but inert: selecting them shows the PSP-style unavailable/error sound
and a short "not available in this build" message. They must never look like an
empty placeholder screen; the shell animations, focus glow, sound and status
bar are the same as the Game column.

## Touch without a gamepad

The mode must be usable on phones/handhelds with no controller and with no
on-screen buttons. Gestures are the whole input:

- swipe up/down: previous/next item;
- swipe left/right: previous/next category;
- tap: confirm (launch/open the selected item);
- edge-back gesture / system Back: item → category → leave, same as the PSP
  Back button;
- a long swipe may repeat one step only while the finger travels, and a gesture
  must not also fire a tap.

The gestures feed the same `KeyPressed` path as the physical D-pad, so the
animation, sounds and selection state are identical. Thresholds and velocity
handling live in the host, not in the firmware renderer.

The integration is allowed to extend the existing core hand-off where it is
needed for this mode: read the selected game's ICON0/PIC1/SND0, start it through
the same path as the current home screen, and carry the return state back. It
must not change emulator behaviour, graphics settings or the core's game list;
only the glue needed for the XMB launch/metadata path.

## Current state

- `:app` depends on `:native-ui:android`. The XMB is the home screen's library
  mode (`HomeScreen` shelf toggle → `XmbScreen`). It is also reachable as
  `XmbRoute` with the `xmb_mode` intent extra.
- `XmbScreen` hosts the GL surface in Compose, supplies the `HomeViewModel`
  library (DLC filtered out), ICON0/PIC1/SND0 and save states. It forwards
  `XmbHost.onGameSelected(index)` to the normal emulation launch.
- The opening and gameboot sequences, touch gestures with the touch tutorial,
  the landscape lock and the in-XMB Settings column are implemented.
- Open: restoring the exact category/item after a cold start (the selection
  survives only while the process does).

## Opening transition

Entering the mode from the app must play the short PSP opening animation, not
cut straight to the menu. The source is `opening_plugin.rco` (already extracted
with its model and images): the short startup sequence before the XMB, not the
long cold-boot init. It is rendered by the same native scene, synchronised with
the boot sound, and must be skippable/consumed exactly like the firmware
animation. The final frame hands over to the menu without a flash.

## Game library and selection

- The Game column is populated from the application's existing library: every
  game visible in the app appears with its own title and icon/cover art.
- Selecting a game shows its PSP-style background: PIC1 image, ICON0 icon,
  SND0/ATRAC music where the game carries it, and the same focus/transition
  behaviour as a UMD item.
- Confirm launches through the existing emulation handoff. Closing the game —
  Back, in-game exit, emulator shutdown or process death — returns to the XMB
  at the exact category/item that was selected when the game started.
- The mode restores that position on a cold start; no reset to the first item.

## Settings

The settings for the mode live inside the XMB itself, as the PSP's own Settings
column did — this is the immersion the mode is for. Selecting the Settings
category shows a normal PSP column with the same icons, focus glow, scroll and
opening/closing animations. No bespoke Material settings screen is added for
the mode.

The column mixes two kinds of entries:

- real settings implemented by this app and stored in `AppPreferences`
  (background/theme, wave on/off and speed, navigation sounds/volume, clock
  format/visibility, icon/text scale, animation speed, reset to defaults);
- imitated PSP entries (the same labels/icons and full navigation, animation
  and sound behaviour, but inert or showing the PSP unavailable sound), so the
  tab feels complete rather than like a stub.

The existing app settings may offer a shortcut that opens the XMB already
positioned on its Settings category, but the authoritative UI is the in-XMB
column with the existing PSP layout.

## Localisation

The application ships `values` plus `values-ar, cs, de, es, fa, fr, hi, in, it,
ja, ko, pl, pt, ru, tr, uk, zh` (18 resources). Every new string introduced by
this work — mode name, settings labels/descriptions, item names, unavailable
messages, accessibility labels — must exist in all of them, not only
`values/strings.xml` and `values-uk`. Language selection remains the existing
`AppLocaleManager`/`AppPreferences` system (including "System" / automatic
selection). Non-Latin fonts are already handled by the firmware PGF reader's
Latin/Japanese/Korean font selection work, which must be wired to the selected
locale.

## Acceptance

- A cold start in XMB always restores XMB and the selection.
- Every game visible in the current library is visible in XMB and launches the
  same way it does from the carousel; returning always lands in XMB.
- No empty category: each tab draws real labels/icons and its opening/closing
  animation, with inert items behaving consistently.
- Toggling this mode repeatedly does not leak the GL surface, audio player or
  pack installer.
- The XMB Settings column is reachable inside the mode, contains the real
  options plus imitated PSP entries, and its changes apply live and persist.
- All strings resolve in every supported locale.
- The renderer parity suite from the previous phase still passes.
