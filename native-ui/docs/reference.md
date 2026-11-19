# Reference behaviour

The acceptance target is the 6.60 XMB itself. This file records what was
measured from the real shell and where the measurements came from, so a later
change can be checked against the same source instead of an impression.

Firmware and reference frames remain local. Reproduce them with
`native-ui/tools/reference_capture.py` and a flashed flash0 dump; do not commit
either.

## Capturing the shell

The headless build of this fork can boot `vshmain.prx` and expose the WebSocket
debugger. The script starts it with `--vsh --nand=<dir> --graphics=software
--debugger=PORT`, runs to a fixed emulated time with `cpu.runUntilTime`, saves
`gpu.buffer.screenshot` (PNG data URI) and sends `input.buttons.press` for the
D-pad. Every run stops on an absolute emulated timestamp, so the same script
reaches the same frame.

Observed on a Lenovo TB710FU:

- A software-rendered VSH boot reaches the interactive menu after roughly
  15-20 emulated seconds; 25 s is a reliable first capture time.
- `gpu.buffer.screenshot` returns **480x272** ARGB/PNG while the CPU is stopped
  by `cpu.runUntilTime`. The older `--screenshot-save` path saves the
  framebuffer with its 512-pixel stride, which is useful for the raw image but
  not for pixel comparison.
- `--screenshot-at` works too for unattended captures, but only at absolute
  emulated times and without input.

## Game column, measured

Coordinates are PSP screen pixels, origin top-left, from the 480x272 capture.
They are measurements from rendered frames, not yet values recovered from the
plugin's data.

| Element | Measurement |
| --- | --- |
| Category icon row | icon centres at y ≈ 71 |
| Video / Game / Network / PSN centres | x ≈ 24 (clipped), 109.5, 195.5, 274 |
| Category icon spacing | ≈ 86 px, then ≈ 78.5 px |
| Selected category icon | larger than its neighbours; ≈ 53x36 for Game |
| Selected category label | centred under the icon, ≈ 39x13 box for "Game" |
| Clock | top-right, x ≈ 340-444, y ≈ 4-20 |
| Battery | x ≈ 449-477, y ≈ 4-20 |
| Game list icon | ≈ 32x24 at x ≈ 94, selected item icon y ≈ 124 |
| Game list title | starts at x ≈ 140, cap height ≈ 16 px |
| Item spacing | ≈ 66 px between item icons |
| Item focus | soft white glow around the icon, not a rectangle around the row |
| Subtitle | second line under the title; a thin white rule runs under the block |

The category row is not centred on the screen. The selected icon sits at
x ≈ 109 and the row shifts horizontally when the category changes, while the
selected icon stays at that anchor. Icons of unselected categories are dimmed,
and only the selected category shows its label.

The Game column's selected item changes what is behind it: with a UMD game
selected the game's PIC1 fills the screen, its preview plays in a small
rectangle at the left, and its icon is drawn at the top-left (partially clipped
by the screen edge). Without a disc, the column shows its utility items on the
wave background. The top-left icon changes with the selected item's media
(disc, memory card, update), so it is part of the item view rather than a
fixed status bar.

Navigation:

- Up/Down moves the selection between items. The icon, title and subtitle
  cross-fade and slide; the outgoing row fades rather than disappearing.
- Left/Right changes category. The entire category row slides one slot, the
  selected icon and label animate, and the new column's items fade in.
- The wave background keeps moving during both.
- The XMB sound events (`snd_cursor`, `snd_decide`, `snd_cancel`,
  `snd_category_decide`, `snd_option`) come from `system_plugin.rco`; they are
  already extracted into the pack.

## What the index file is

`sceResmgr` decrypts `flash0:/vsh/etc/index_02g.dat` through PRX type 9. The
decrypted 159 bytes are build metadata, not an item list:

```text
release:6.60:
build:...
system:...
vsh:...
target:1:WorldWide
```

The item labels and page structure therefore come from the `.rco` object trees
and the plugin code, not from this index.

## Assets confirmed against frames

The category and item icons decoded from `topmenu_icon.rco` are the same
shapes seen on screen (gamepad, globe, camera, music note, film, UMD disc,
memory card, folder, update arrows, `www`). `system_plugin.rco` holds the
shared focus/scroll/arrow textures; `game_plugin.rco` holds the list and
loading panels; `system_plugin_bg.rco` holds the wave model and its curves.

Text is rendered from the firmware's PGF fonts. PPSSPP's `Core/Font/PGF.cpp`
is the reference parser for the glyph payload and metrics; the XMB's strings
are short enough to prepare a glyph atlas offline once that parser is ported.

## Still to measure from the original

- exact animation curves and durations for item focus, list scroll, category
  slide and wave parameters (plugin code / RCO animation data);
- the item icon selection rules (which `topmenu_icon` entry each label uses);
- font size and positioning per row, plus the shadow offsets;
- the game-background layout once a UMD is selected (preview rectangle, title
  art, icon placement).
