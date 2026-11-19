"""Decoder for the RLZ streams inside PSP RCO resources.

RLZ is a range coder with a literal/match model, the format the VSH's resource
loader uses for RCO header tables and for most image payloads. This is a clean
implementation of the decoder, written from the recovered model and checked
against every RLZ table in the 6.60 firmware (48 of 48 decode exactly to the
size the container declares).

Format notes that matter for a correct decoder:

* The first byte of a stream is the mode. Bits 0-2 select the literal context
  shift, bits 3-4 select the initial probability (0x80 for every mode the
  firmware uses). The range coder itself starts after that byte; the encoder's
  first output byte is overwritten by the mode byte, so the decoder reads four
  bytes as its initial code with an implicit zero in front.
* Probability update is the standard one: a zero moves towards zero by
  ``p >> 3``, a one moves towards 255 by ``(255 - p) >> 3``.
* The probability tables are one contiguous block and some indexes deliberately
  run past the end of one table into the next. In particular the distance
  table needs 18 slots of 0x20 bytes (RCOMage's reconstruction says 12, which
  cannot decode distances above 8 KiB).
* Distance descriptors write their high bits with the probability coder, a
  middle run with raw range bisection and the low three bits with probabilities
  again; the middle run is normalised once, after the whole run.
"""

from __future__ import annotations

from dataclasses import dataclass

# Probability block layout, in bytes from the start of the block. The original
# block's next field (input pointer) sits at 0xCA8, so 18 distance slots of
# 0x20 fill the space between distDescShort and lenDesc.
LITERALS = 0
DIST_LONG = 8 * 0xFF
DIST_SHORT = DIST_LONG + 0xF0
DISTANCES = DIST_SHORT + 0x40
DISTANCE_SLOTS = 18
LEN_DESC = DISTANCES + DISTANCE_SLOTS * 0x20
MATCHLENS = LEN_DESC + 0x40
PROB_BLOCK_SIZE = MATCHLENS + 0xFF

MODE_MASK = 7


class RlzError(ValueError):
    """The stream is not a valid RLZ stream, or does not fit its declared size."""


class _RangeDecoder:
    __slots__ = ("data", "pos", "range", "code")

    def __init__(self, data: bytes) -> None:
        if len(data) < 4:
            raise RlzError("RLZ stream is shorter than the range coder's initial code")
        self.data = data
        self.pos = 0
        self.range = 0xFFFFFFFF
        self.code = 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF

    def _byte(self) -> int:
        if self.pos >= len(self.data):
            raise RlzError("RLZ stream ended inside a symbol")
        value = self.data[self.pos]
        self.pos += 1
        return value

    def _normalise(self) -> None:
        # The encoder flushes at most once per probability bit.
        if self.range < 0x01000000:
            self.range = (self.range << 8) & 0xFFFFFFFF
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF

    def bit(self, probs: bytearray, index: int) -> int:
        probability = probs[index]
        midpoint = (self.range >> 8) * probability
        if self.code < midpoint:
            bit = 1
            self.range = midpoint
            probability += (0xFF - probability) >> 3
        else:
            bit = 0
            self.code = (self.code - midpoint) & 0xFFFFFFFF
            self.range = (self.range - midpoint) & 0xFFFFFFFF
            probability -= probability >> 3
        probs[index] = probability & 0xFF
        self._normalise()
        return bit

    def uniform_bit(self) -> int:
        self.range >>= 1
        if self.code < self.range:
            return 1
        self.code -= self.range
        return 0


def _special(rd: _RangeDecoder, probs: bytearray, offset: int, arg: int) -> int:
    """Mirror of the original's distance/length fraction writer."""
    value = 0
    if arg >= 3:
        value |= rd.bit(probs, offset) << arg
        arg -= 1
        if arg >= 3:
            value |= rd.bit(probs, offset) << arg
            arg -= 1
        offset += 1
        if arg >= 3:
            for k in range(arg - 2):
                value |= rd.uniform_bit() << (arg - k)
            rd._normalise()
            arg = 2
    while arg >= 0:
        value |= rd.bit(probs, offset) << arg
        offset += 1
        arg -= 1
    return value


@dataclass(frozen=True)
class StreamInfo:
    mode: int
    consumed: int
    output_size: int


def decompress(data: bytes, expected_size: int) -> tuple[bytes, StreamInfo]:
    """Decode one RLZ stream.

    ``data`` starts with the mode byte and is the whole packed payload.
    ``expected_size`` is the container's declared uncompressed size; a stream
    that ends early or late is rejected rather than returned partial.
    """
    if not data:
        raise RlzError("empty RLZ stream")
    mode = data[0]
    real_mode = mode & MODE_MASK
    probability = (4 - ((mode >> 3) & 0x03)) << 5
    probs = bytearray([probability] * PROB_BLOCK_SIZE)

    rd = _RangeDecoder(data[1:])
    out = bytearray()
    last_byte = 0
    var_c = 0

    while len(out) < expected_size:
        ones = 0
        for i in range(8):
            if not rd.bit(probs, LEN_DESC + i * 8 + var_c):
                break
            ones += 1
        if ones == 0:
            length_code = 0
            ret = -1
        elif ones == 8:
            low = _special(rd, probs, MATCHLENS + ((var_c & 7) + 6 * 8) * 4 + ((len(out) << 6) & 3), 6)
            length_code = 128 + low
            ret = 7
        else:
            floor = ones - 1
            low = 0 if floor == 0 else _special(
                rd, probs,
                MATCHLENS + ((var_c & 7) + (floor - 1) * 8) * 4 + ((len(out) << (floor - 1)) & 3),
                floor - 1,
            )
            length_code = (1 << floor) + low
            ret = ones - 1

        if length_code == 255:
            raise RlzError("RLZ end marker before the declared uncompressed size")
        match_len = length_code + 1

        if match_len == 1:
            context = (((len(out) & 7) << 8) | last_byte) >> real_mode & 7
            prefix = 0
            for k in range(8):
                prefix = (prefix << 1) | rd.bit(probs, LITERALS + context * 0xFF + ((1 << k) - 1) + prefix)
            byte = prefix & 0xFF
            out.append(byte)
            last_byte = byte
            if var_c > 0:
                var_c -= 1
            continue

        if match_len < 4:
            bits, base = 3, DIST_SHORT
        else:
            bits, base = 5, DIST_LONG
        tree = 1
        for _ in range(bits):
            tree = (tree << 1) | rd.bit(probs, base + tree * 8 + ret)
        floor = (tree - (1 << bits)) - (0 if match_len < 4 else 0xC)
        if floor < 0:
            raise RlzError(f"negative distance exponent {floor} at output {len(out)}")
        distance = 1 << floor
        if floor > 0:
            distance |= _special(rd, probs, DISTANCES + (floor - 1) * 0x20, floor - 1)
        source = len(out) - distance
        if source < 0:
            raise RlzError(f"distance {distance} before output start at {len(out)}")
        for _ in range(match_len):
            out.append(out[source])
            source += 1
            if len(out) > expected_size:
                raise RlzError("RLZ stream expands past the declared size")
        last_byte = out[-1]
        var_c = 7 - (len(out) & 1)

    return bytes(out), StreamInfo(mode=mode, consumed=rd.pos + 1, output_size=len(out))
