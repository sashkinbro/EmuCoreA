# Offline PSP resource preparation

These scripts read firmware dumped from a PSP the user owns and produce the
ready resource pack. They are development tools and never ship in the
application; the UI reads only the packed, ready files.

## Getting the firmware

`extract_psp_updater.py` pulls the updater out of a UMD image:

```text
python native-ui/tools/extract_psp_updater.py OWNED_DISC.iso native-ui/firmware/input/DATA.BIN
```

It prints the firmware version from the updater's own `PARAM.SFO`. Most UMDs
carry one; the 6.60 updater used for the current reconstruction came from a
retail disc.

The PSAR is then unpacked with PPSSPP's own tool (a development build, for the
device the dump will be read on):

```text
/data/local/tmp/ppsspp_headless --unpack-updater=/data/local/tmp/flash0 /data/local/tmp/DATA.BIN
```

and its `flash0/` tree placed under `native-ui/firmware/flash0/`. Firmware is
ignored by Git.

## PSP RCO containers

`psp/rlz.py` decodes the RLZ range-coded streams that hold RCO table trees
(mode byte, literal/match model, distance and length contexts). Recovering it
took four details beyond the public notes:

- The probability update is the standard adaptive one (`p -= p >> 3` for a
  zero, `p += (255 - p) >> 3` for a one). A widely copied reconstruction writes
  `p += 0xFF; p -= p >> 3`, which cannot decode the firmware's streams.
- The probability tables are one contiguous block, and indexes run off one
  table into the next on purpose. The distance table's real size is **18**
  slots of `0x20` bytes, not the 12 in RCOMage's structure; the next field of
  the original object starts at `0xCA8`, which fixes the size. With 12 slots,
  every table that references more than 8 KiB back desynchronizes around output
  offset 9,000.
- A distance writes its high bits with probabilities, then a run of raw range
  bisection, then three more probability bits. The raw run is normalized once
  after the whole run, not per bit.
- The stream's first byte is the mode and overwrites the range coder's first
  output byte; the decoder starts with an implicit zero followed by the next
  four bytes.

All 48 RLZ table streams in the 6.60 firmware decode to exactly the size their
container declares (`native-ui/tests/test_rlz.py` checks this when the local
dump is present).

## Ready resources

```text
python native-ui/tools/extract_psp_resources.py --flash0 native-ui/firmware/flash0 --out native-ui/resources --force
```

This reads the shell's containers (`system_plugin`, `topmenu`, `game_plugin`,
`savedata_plugin`, `opening_plugin`, `impose_plugin`), decodes every image and
sound, copies the Latin fonts and writes `manifest.json`. From the 6.60 dump
that is 305 PNG images, 8 WAV sounds, 2 GMO models and 16 PGF fonts. Every file
records its source RCO and entry label plus source/file hashes.

The GIM reader handles the formats those containers use: indexed 4/8-bit with
a palette block, RGBA8888 and the 16-bit colour formats, in both linear and
tiled (`pixel_order == 1`) layouts. The tiled layout is 128-bit-wide, 8-row
tiles over a pitch- and tile-aligned buffer; treating it as linear produces
diagonal bands instead of icons.

The VAG reader keeps the source sample rate and does a plain PSX ADPCM decode;
nothing is resampled, normalised or looped at this stage. The navigation sounds
are 44.1 kHz mono, with `snd_cursor` at 1,848 samples.

## Packing

```text
python native-ui/tools/pack_resources.py
```

Writes `native-ui/build/native-ui.zip` and `native-ui/build/native-ui-pack.json`
with the SHA-256, packed size, file count and unpacked size. The ZIP is
deterministic (sorted entries, 1980 timestamps, level 9), and the script
verifies every entry against the resource tree after writing. The Android
module's `prepareNativeUiPack` task runs this before `preBuild` and copies the
ZIP into the APK's assets; the installer uses the hash as the installed pack's
version.


