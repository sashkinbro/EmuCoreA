# SPDX-FileCopyrightText: 2026 SBRO
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decoder for the VAG sounds in PSP RCO containers.

A VAG file is a 0x30-byte big-endian header ("VAGp", version, payload size,
sample rate, name) followed by 16-byte PlayStation ADPCM frames. Each frame
stores a filter/shift byte, a flags byte and 28 signed 4-bit samples. The five
standard coefficient pairs are the ones the PSP's SPU uses.

The XMB's navigation sounds are short, mono and 44.1 kHz; decoding is
lossless, the source rate is kept and nothing is resampled or normalised.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

COEFFICIENTS = (
    (0, 0),
    (60, 0),
    (115, -52),
    (98, -55),
    (122, -60),
)


class VagError(ValueError):
    pass


@dataclass(frozen=True)
class Vag:
    sample_rate: int
    samples: bytes  # signed 16-bit little-endian, one channel
    loop_start: int | None
    loop_end: int | None
    name: str


def _be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def decode(data: bytes) -> Vag:
    if len(data) < 0x30 or data[:4] != b"VAGp":
        raise VagError("not a VAG file")
    version = _be32(data, 0x04)
    if version not in (4, 0x20):
        raise VagError(f"unexpected VAG version {version}")
    payload_size = _be32(data, 0x0C)
    sample_rate = _be32(data, 0x10)
    if sample_rate == 0:
        raise VagError("VAG has no sample rate")
    start = 0x30
    end = start + payload_size
    if end > len(data) or payload_size % 16:
        raise VagError("VAG payload size is invalid")
    payload = data[start:end]
    name = data[0x20:0x30].split(b"\0", 1)[0].decode("ascii", "replace")

    out = bytearray()
    previous1 = 0
    previous2 = 0
    loop_start = None
    loop_end = None
    sample_index = 0
    for frame_index in range(0, len(payload), 16):
        frame = payload[frame_index:frame_index + 16]
        header = frame[0]
        shift = header & 0x0F
        if shift > 12:
            raise VagError(f"VAG shift {shift} is invalid")
        filter_index = (header >> 4) & 0x07
        if filter_index >= len(COEFFICIENTS):
            raise VagError(f"VAG filter {filter_index} is invalid")
        coefficient1, coefficient2 = COEFFICIENTS[filter_index]
        flags = frame[1]
        if flags & 0x01:
            loop_start = sample_index
        if flags & 0x02:
            loop_end = sample_index
        for byte in frame[2:16]:
            for nibble in (byte & 0x0F, byte >> 4):
                value = nibble - 16 if nibble & 0x08 else nibble
                sample = (value << 12) >> shift
                sample += (previous1 * coefficient1 + previous2 * coefficient2) >> 6
                sample = max(-32768, min(32767, sample))
                previous2, previous1 = previous1, sample
                out += struct.pack("<h", sample)
                sample_index += 1

    return Vag(
        sample_rate=sample_rate,
        samples=bytes(out),
        loop_start=loop_start,
        loop_end=loop_end,
        name=name,
    )


def wav(vag: Vag, channels: int = 1) -> bytes:
    """Wrap decoded samples as a PCM16 WAV. ``channels`` matches the source
    container; channel files are interleaved by the caller before calling."""
    block_align = 2 * channels
    byte_rate = vag.sample_rate * block_align
    data = vag.samples
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, vag.sample_rate, byte_rate, block_align, 16)
    header += b"data" + struct.pack("<I", len(data))
    return header + data
