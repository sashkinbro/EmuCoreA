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
  textures are created lazily, one per codepoint; the shadow pass reuses the
  same glyph with a 1-pixel offset.
- `xmb_renderer.cpp`: GLES 3 shader, one dynamic quad buffer, a texture cache,
  and the measured category/item anchors from `scene/categories.json`.
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

## Deliberately not here yet

- The wave background (`system_plugin_bg.rco` model and curves).
- Focus glow, shadows, reflections, icon cross-fades and row scrolling.
- The clock, battery and media indicators.
- Game backgrounds and preview playback when a game is selected.
- Exact per-element font sizes for every text style and the real shadow
  rendering (the glyphs are exact, the scale/baseline of each style is still
  being matched).

Those are parity work, and none of them is represented by an approximation in
the scene. The provisional flat colour is visibly different from the reference
and is listed here instead of being called done.

## Running the preview

```text
gradlew :native-ui:preview:installDebug
adb shell am start -n com.sbro.emucorea.xmb.preview/.MainActivity
```

The preview installs the pack on first launch, logs
`scene initialized with N textures` and `nativeInit ok`, and shows the current
scene fullscreen. Frames are compared against `native-ui/reference` captured by
`tools/reference_capture.py`.
