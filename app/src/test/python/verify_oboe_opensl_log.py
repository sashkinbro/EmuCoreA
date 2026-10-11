"""Verify actual OpenSL ES output in a focused audio-backend device test log.

Capture activeAudioRebuildsAcrossBackendAndLatencyChanges with nativeLogLevel=info.
This rejects the regression where the OpenSL ES setting silently selects AAudio.
Direct frontend AAudio streams use a different log marker and are ignored.
"""

import argparse
import codecs
from pathlib import Path
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--min-streams", type=int, default=1)
    args = parser.parse_args()
    if args.min_streams < 1:
        parser.error("--min-streams must be positive")
    raw = args.log.read_bytes()
    encoding = "utf-16" if raw.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)) else "utf-8-sig"
    recorded = raw.decode(encoding, errors="replace")
    streams = re.findall(r"Oboe: Output stream open:\s*([^,\r\n]+),", recorded)
    if len(streams) < args.min_streams:
        print(f"FAIL: expected at least {args.min_streams} Oboe output records, found {len(streams)}", file=sys.stderr)
        return 1
    wrong = [(index + 1, api.strip()) for index, api in enumerate(streams) if api.strip() != "OpenSLES"]
    if wrong:
        print(f"FAIL: requested OpenSL ES, actual stream APIs: {wrong}", file=sys.stderr)
        return 1
    print(f"PASS: {len(streams)} actual Oboe output streams used OpenSLES")
    return 0


if __name__ == "__main__":
    sys.exit(main())
