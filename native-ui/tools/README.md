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
