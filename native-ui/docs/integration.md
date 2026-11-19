# Application integration plan

This is the next phase after the standalone menu is at parity. It is deliberately
not started while the Game column still has known visual gaps; the task list
keeps it after the renderer work.

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

## Settings

After integration, add a settings tab/screen for the mode (the user's "3D
carousel" settings entry), with live-applied options stored in
`AppPreferences`:

- background: theme/wave on/off, theme colour, wave speed;
- clock format and date/time visibility;
- navigation sounds on/off and volume;
- icon/text scale and animation speed;
- reset to defaults.

Settings are available from both the main settings screen and the home menu;
they persist and survive process death. Existing app preferences/backup rules
apply.

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
- Toggling the mode repeatedly does not leak the GL surface, audio player or
  pack installer.
- Settings changes apply live and persist; all strings resolve in every
  supported locale.
- The renderer parity suite from the previous phase still passes.
