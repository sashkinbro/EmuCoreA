# Native renderer

The XMB scene is drawn by `xmb::XmbRenderer` (`android/src/main/cpp/`), a plain
C++ class with no emulator, SDL or PPSSPP dependency. It draws the firmware's
own images, wave model and PGF font from the resource tree built on the device
(see the [README](../README.md#firmware-and-resources)). This page describes
what it draws and how a frame is produced. The remaining differences from the
firmware are listed in the README.

## Path of a frame

```text
XmbScreen (app module, Compose)
  GLSurfaceView (GLES 3 context, continuous rendering)
    XmbSurfaceRenderer.onSurfaceCreated -> XmbSession.initialize -> XmbResources
                                       -> nativeInit
    onSurfaceChanged -> nativeResize
    onDrawFrame      -> nativeDraw -> xmb::XmbRenderer::Draw
```

The standalone `preview` app uses the same `XmbSurfaceRenderer`.

`XmbRenderer` owns:

- `png_image.cpp`: a small PNG reader for the 8-bit RGBA images the extractor
  writes.
- `pgf_font.cpp`: a reader for the firmware's `ltn0.pgf`: char map, compact
  metric tables, nibble-RLE glyph bitmaps and the V_ROWS-to-row-major
  transposition. Glyph textures are created lazily, one per codepoint.
  Codepoints the font lacks are rasterised by `XmbFontFallback` with the Android
  system font and cached the same way.
- `xmb_renderer.cpp`: the GLES 3 shader, one dynamic quad buffer, a texture
  cache, the wave pass and all scene drawing.

`Draw()` renders into a virtual screen 272 units high. The width is 480 for the
original aspect, 362.67 for 4:3, or follows the device aspect ratio (clamped to
360..680) in the default Full Screen mode, so units stay square. The viewport is
centred and letterboxed when the surface has another shape. Coordinates in the
scene are always these virtual pixels.

The order within a frame is: background, wave, the active page (category row
and item columns, game list, Saved Data, a Settings sub-page, or an empty media
page), status bar, touch hint bar, opening and resume transitions, toast,
dialogs, touch tutorial, then the gameboot sequence while a launch is running.

## Scene

Categories and items

- The category row sits at y = 71. The selected category is drawn at scale 1.0
  and the others smaller and dimmer. The row slides so the selected icon stays
  at its anchor.
- Eight categories: Settings, Extras, Photo, Music, Video, Game, Network and
  Online. Their items (`RebuildCategories`) carry the firmware's icons and
  labels. An item whose icon is missing from the installed firmware is removed
  from its column, except in the Game column.
- Category slides and item scrolling use a one-pole ease-out with a 45 ms and a
  60 ms time constant. The previous column fades out over 0.18 s while the new
  one fades in. The selected item gets the firmware's focus glow texture
  (`tex_3x3_focus`).
- Items without an EmuCoreA counterpart open a notice dialog (System Update,
  USB Connection, Game Sharing, Remote Play, Online Store and similar) or an
  empty media page (Photo, Music, Video, Digital Comics). Internet Browser and
  Internet Search ask for confirmation and then open the device browser.

Game column

- Memory Card opens the game list. The selected game shows its PIC1 as a
  fullscreen background that fades in once scrolling settles, its PIC0 banner
  (or the title when there is no banner), and a vertical list of ICON0 images
  with the focus glow on the selected one. A loading indicator is shown while
  the library loads, and "There are no games" when it is empty.
- The host reads ICON0, PIC0, PIC1 and SND0 from each game and passes the paths
  in `GameInfo`. Moving the selection calls `XmbHost.onGameFocused`, which
  starts that game's SND0 preview in `XmbAudio`.
- Saved Data Utility lists the app's save states with preview image, title,
  slot, date and size.
- On both lists the Options key (Triangle, Y, Menu, or a long press) opens a
  panel with Start, Delete and Information. On the save list, Confirm opens it.
  Delete asks for confirmation. Information shows title, size, version,
  required version, disc ID, region and date for a game.

Settings column

- Sub-pages for Theme, Sound, Date & Time, System, Video, Photo, Power Save,
  Security, Network, RSS, Connected Display and Bluetooth. Each row opens a
  picker in a right-hand panel. Theme changes update the background and wave
  immediately. Changes reach the host as an int array
  (`XmbHost.onSettingsChanged`), which stores them in preferences and applies
  the audio, keep-screen-on and confirm-button ones.
- System offers System Information (it names EmuCoreA, not a Sony model or
  firmware release) and Restore Default Settings with a confirmation.

Dialogs

- Quit, delete, restore-defaults and browser confirmations share one Yes/No
  dialog. Notice, system information and game information dialogs close with
  Back or Confirm. A toast shows short messages.

## Themes and wave

- The background is a gradient image from the installed firmware
  (`images/themes/`: `orig_NN`, `classic_NN`, `ext_NN`, plus the Cookie
  images). The Original theme picks the monthly colour from the date.
- At start-up `LoadThemeSwatches` reads the centre pixel of each of the 12
  monthly (`orig_00` to `orig_11`) and 22 extended (`ext_00` to `ext_21`)
  swatch images and uses it as that colour. A colour whose swatch is missing,
  and November, keep built-in values. The centre pixel of the chosen theme
  image also gives the wave tint. If a theme image cannot be loaded, a gradient
  built from measured row medians is tinted with the swatch instead.
- The wave is the firmware's `mdl_bg.gmo` (22x11 control grid, two morph
  targets, a 128x128 reflection map, a track at 30 fps), parsed by the
  vendored Vlnky reader and tessellated each frame. It is drawn with additive
  blending. The wave setting selects normal speed, slow, static or off. The
  gameboot sequence draws it too.

## Opening, resume and gameboot

- Entering the mode from the app plays the opening transition: a fade from
  black and light sweeps built from the `opening_plugin` glow textures, with
  the EmuCoreA name shimmering where the firmware shows its company emblem.
  Touch input or a key skips ahead.
- Returning from a game plays a shorter fade from black with the firmware's
  busy spinner.
- Launching a game plays the gameboot sequence: a light backdrop (a firmware
  theme gradient), the wave, glow beams, and the name EMUCOREA in tracked
  capitals with a light sweep, then a fade to black. The gameboot sound is
  extracted from `gameboot.pmf` as `snd_gameboot.at3` and decoded by
  `at3_decoder.cpp`. After 2.1 s the renderer calls `XmbHost.onGameSelected`
  and the host starts emulation. No Sony logo or name is drawn.

## Status bar

Top right: battery (firmware sheet, with a charging animation and a blink when
low), Wi-Fi, Bluetooth, AVLS and mute indicators, and the clock in the selected
date and 12/24-hour format. A memory card indicator slides in at the top left
during the opening and while the game or save lists load. On screens wider than
490 units, or in touch mode, the bar is inset from the corners. The host pushes
battery, network, Bluetooth and mute state through the `nativeSet*` calls.

## Input

`XmbHost.handleKeyEvent` maps the D-pad, Enter, A/B (swappable with the Confirm
Button setting) and Y/Menu/Space to `nativeKey` (Up, Down, Left, Right,
Confirm, Back, Options). `XmbRenderer::KeyPressed` is the single entry point for
key input.

Touch is handled in `XmbScreen` and forwarded as `nativeTouchTap(x, y)` and
`nativeTouchSwipe(direction)`:

- a swipe moves like the D-pad, so the content follows the finger. Inside
  dialogs and the options panel it moves the highlight or closes the panel;
- a tap on Yes/No, on a picker row, on the back arrow or on the hint bar acts
  on that control, and any other tap confirms;
- a long press (420 ms) opens the options panel, and a second finger acts as
  Back;
- a first-use tutorial dialog explains the gestures. Online Instruction Manuals
  shows it again.

## Audio

`XmbAudio` loads the navigation sounds (`snd_cursor`, `snd_decide`,
`snd_cancel`, `snd_category_decide`, `snd_option`, `snd_gameboot` and others)
into a `SoundPool` and plays them on the renderer's `PlaySound` events. Game
SND0 previews are decoded to WAV by the host and streamed in a gapless
`AudioTrack` loop. Key tone and background music can be switched off, and AVLS
lowers the volume.

## Running the preview

```text
gradlew :native-ui:preview:installDebug
adb shell am start -n com.sbro.emucorea.xmb.preview/.MainActivity
```

The preview has no updater unpacker. Push an unpacked flash0 first (see the
README); it installs the resources from there on start, logs
`scene initialized with N textures`, and shows the scene fullscreen. Frames are compared against `native-ui/reference`, captured by
`tools/reference_capture.py`.
