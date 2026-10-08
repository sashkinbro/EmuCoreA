"""Reject debug-only frontend code and assets in a built release APK."""

import argparse
import re
import subprocess
import sys
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk")
    parser.add_argument("--aapt2", required=True, help="Android SDK aapt2 executable")
    args = parser.parse_args()
    failures = []
    markers = (
        b"audio-capture.wav",
        b"debug.emucorea.",
        b"diag speed=",
        b"diag fps=",
        b"AAudio xruns (underruns)",
        b"nativeGetRuntimeState",
        b"NativeCoreDiagnostics",
        b"RuntimeTestActivity",
        b"DebugJitSelfTestActivity",
        b"DebugAllTestsActivity",
        b"DebugGamesBootActivity",
        b"NativeCoreProbeActivity",
        b"NativeCoreLifecycleInstrumentedTest",
        b"PspGeDisplayFixture",
        b"FixtureContentProvider",
    )
    with zipfile.ZipFile(args.apk) as apk:
        for name in apk.namelist():
            if name.startswith("assets/debugger/") or name.endswith("/gdbserver"):
                failures.append(f"debug asset: {name}")
            if name.endswith(".dex") or name.endswith("/libemucorea_core.so"):
                data = apk.read(name)
                for marker in markers:
                    if marker in data:
                        failures.append(f"{name}: {marker.decode()}")
    manifest = subprocess.run(
        [args.aapt2, "dump", "xmltree", args.apk, "--file", "AndroidManifest.xml"],
        check=True, capture_output=True, text=True,
    ).stdout
    for flag in ("debuggable", "testOnly"):
        for line in manifest.splitlines():
            if f"android:{flag}(" in line and not re.search(r"=\(type 0x12\)0x0$", line.strip()):
                failures.append(f"manifest enables {flag}: {line.strip()}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("Release APK: no frontend test hooks, captures, diagnostic counters or debugger assets; debuggable/testOnly disabled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
