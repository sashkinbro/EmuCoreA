# Native renderer

The first native scene draws the Game-column anchors with the original
`topmenu_icon` textures and the firmware's own PGF glyphs. It is a foundation,
not parity: the wave, focus/scroll/cross-fade animations and the per-game
background are still missing.

## Path of a frame

```text
MainActivity (preview)
  GLSurfaceView (GLES 3 context)
    XmbSurfaceRenderer.onSurfaceCreated -> XmbSession.initialize -> BundledResources
                                       -> nativeInit
    onSurfaceChanged -> nativeResize
    onDrawFrame      -> nativeDraw -> xmb::XmbRenderer::Draw
```

`XmbRenderer` is plain C++ with no emulator, SDL or PPSSPP dependency. It owns:

- `png_image.cpp`: a small PNG reader for the 8-bit RGBA images the prep tools
  write (all five filters, zlib via libz). The application never needs to
  decode a PNG at runtime if the pack later switches to raw textures, but this
  keeps the current pack directly usable.
- `pgf_font.cpp`: a reader for the firmware fonts, ported from the inspected
  PGF layout. It decodes the char map, the compact metric tables, the
  nibble-RLE glyph bitmaps and the V_ROWS-to-row-major transposition. Glyph
  textures are created lazily, one per codepoint. Text shadows are a one-pixel
  offset copy of the main glyph; the PGF shadow sub-glyph records are not used
  because their layout is still unresolved and reading them as main glyphs
  produced the black stripes seen in testing.
- `xmb_renderer.cpp`: GLES 3 shader, one dynamic quad buffer, a texture cache,
  and the measured category/item anchors from `scene/categories.json`.
- The wave is the firmware's own `mdl_bg.gmo` (22x11 control grid, two morph
  targets, a 128x128 reflection map, a 1500-frame track at 30 fps), parsed by
  the vendored Vlnky reader and tessellated every frame. It is drawn with the
  PSP's sphere-map/additive blend. The background ramp behind it is measured
  from the reference frames; its placement and intensity are still being
  matched and are not yet a pixel-exact wave.
- The clock uses the firmware PGF font and the battery the original
  `system_plugin_fg/tex_battery` sheet (top frame). Selected item icons get a
  low-alpha multi-sample halo.
- The viewport keeps the 480x272 aspect ratio and letterboxes on wider
  surfaces. Coordinates in the scene are always PSP pixels.

The scene currently draws:

- the category row at y = 71, with the measured x anchors for
  Video/Game/Network/PSN and extrapolated anchors for the four left slots;
- the selected category at scale 1.0 and opacity 1.0, the others at 0.8/0.55;
- the Game column's selected item icon (Saved Data Utility, EJ) at
  (110, 136) and the next item (Memory Stick, BI) one 66-pixel slot lower;
- the selected label "Game" at the matched 0.66 scale, centred at x = 109.5,
  and the item titles at the matched 0.82 scale, starting at x = 140;
- a flat provisional theme colour behind them.

The icons and glyphs are the exact decoded resources; the layout constants are
the ones measured against the real shell and recorded in
[docs/reference.md](reference.md). `scene/categories.json` is the source for
those values.

## Interaction

Physical D-pad / A / B and touch swipes are forwarded from the preview activity
to `nativeKey`. The scene keeps a category index and an item index; Up/Down
scroll the list, Left/Right move the category row, Confirm/Back are wired to
the decide/cancel sounds.

The two animations use the curve measured from sixty 60 Hz reference frames: a
one-pole ease-out with a 45 ms time constant for the category row and 60 ms for
the item scroll. The old selected icon shrinks/fades while the new one grows;
titles cross-fade with the same progress. Rows above the selected item are
hidden after the scroll settles, matching the reference's clean selection view.

Sounds come from the installed pack through Kotlin `SoundPool`; the native
scene calls `PlaySound` on the same events (cursor on list moves, category
decide on row moves, decide/cancel on Confirm/Back). Verified on the Anbernic
RG556: the preview plays `snd_cursor` and `snd_category_decide` for the D-pad
sequence and does not crash.

## Deliberately not here yet

- Exact wave placement/colour and the real theme ramp (the current geometry is
  the original mesh but the camera offset and additive strength are
  approximations).
- Real focus glow/shadow filtering (the current halo is a cheap multi-sample
  approximation), reflections and icon cross-fades.
- Game backgrounds and preview playback when a game is selected.
- The transient Memory Stick access indicator that appears at the top-left in
  the first boot frames (it is already sliding away 0.7 s later).
- Exact per-element font sizes for every text style and the real shadow
  rendering (the glyphs are exact, the scale/baseline of each style is still
  being matched).

Those are parity work, and none of them is represented by an approximation in
the scene. The provisional wave/background values are visibly different from
the reference and are listed here instead of being called done.

## Running the preview

```text
gradlew :native-ui:preview:installDebug
adb shell am start -n com.sbro.emucorea.xmb.preview/.MainActivity
```

The preview installs the pack on first launch, logs
`scene initialized with N textures` and `nativeInit ok`, and shows the current
scene fullscreen. Frames are compared against `native-ui/reference` captured by
`tools/reference_capture.py`.
