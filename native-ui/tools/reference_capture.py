#!/usr/bin/env python3
"""Capture reference frames from the real PSP XMB in PPSSPPHeadless.

The shell is booted from a local 6.60 flash0 dump with the WebSocket debugger
enabled, stopped at deterministic emulated times, driven with the debugger's
button events and captured through `gpu.buffer.screenshot`. This is the oracle
the native renderer is compared against; no emulator code is changed and no
reference images are committed.

Requirements: Python 3, `websocket-client`, adb, and the headless build plus a
flash0 dump already on the device.

    python native-ui/tools/reference_capture.py --device SERIAL
"""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import websocket
except ImportError:
    print("error: pip install websocket-client", file=sys.stderr)
    raise SystemExit(2)

DEFAULT_ADB = str(Path.home() / "AppData/Local/Android/Sdk/platform-tools/adb.exe")
DEFAULT_OUT = Path(__file__).resolve().parents[1] / "reference"


class CaptureError(RuntimeError):
    pass


def adb(serial: str, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    command = [DEFAULT_ADB, "-s", serial, *args]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise CaptureError(f"adb {' '.join(args)}: {result.stderr.strip() or result.stdout.strip()}")
    return result


class Client:
    def __init__(self, port: int, timeout: float = 20.0) -> None:
        deadline = time.time() + timeout
        last: Exception | None = None
        while time.time() < deadline:
            try:
                self.ws = websocket.create_connection(
                    f"ws://127.0.0.1:{port}/debugger",
                    subprotocols=["debugger.ppsspp.org"],
                    timeout=10,
                )
                break
            except Exception as error:  # noqa: BLE001 - retry every connection error
                last = error
                time.sleep(0.25)
        else:
            raise CaptureError(f"debugger did not accept a connection: {last}")
        self.ticket = 1
        self.now = 0.0
        self.send("version", name="xmb-reference", version="1")
        self.wait("version")
        self.send("cpu.status")
        status = self.wait("cpu.status")
        self.now = float(status.get("us", 0.0))

    def send(self, event: str, **fields: object) -> None:
        self.ticket += 1
        self.ws.send(json.dumps({"event": event, "ticket": self.ticket, **fields}))

    def recv(self, timeout: float) -> dict:
        self.ws.settimeout(timeout)
        buffer = ""
        while True:
            chunk = self.ws.recv()
            if not chunk:
                continue
            buffer += chunk
            try:
                return json.loads(buffer)
            except json.JSONDecodeError:
                continue

    def wait(self, event: str, timeout: float = 120.0) -> dict:
        deadline = time.time() + timeout
        while time.time() < deadline:
            message = self.recv(max(0.5, deadline - time.time()))
            if message.get("event") == event:
                return message
        raise CaptureError(f"no {event} from the debugger")

    def run_until(self, us: float) -> None:
        self.send("cpu.runUntilTime", us=us)
        self.wait("cpu.stepping", timeout=180)
        self.now = us

    def run_for(self, us: float) -> None:
        self.run_until(self.now + us)

    def press(self, button: str, frames: int = 2, settle_us: int = 700_000) -> None:
        self.send("input.buttons.press", button=button, duration=frames)
        self.run_for(settle_us)

    def shot(self, path: Path) -> None:
        self.send("gpu.buffer.screenshot", type="uri")
        message = self.wait("gpu.buffer.screenshot", timeout=60)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(base64.b64decode(message["uri"].split(",", 1)[1]))
        print(f"  {path.name}: {message.get('width')}x{message.get('height')} at {self.now / 1e6:.2f}s")

    def close(self) -> None:
        self.ws.close()


@dataclass
class Options:
    serial: str
    out: Path
    port: int
    boot_time: float
    umd_path: str | None
    transition_frames: int
    timeout: int


def start_shell(options: Options) -> None:
    adb(options.serial, "shell", "pkill -x ppsspp_headless", check=False)
    time.sleep(0.5)
    command = (
        "cd /data/local/tmp; "
        "(./ppsspp_headless --vsh --nand=/data/local/tmp/flash-6.60 --graphics=software "
        f"--debugger={options.port} --timeout-wall={options.timeout} > xmb-capture.log 2>&1 &)"
    )
    adb(options.serial, "shell", command)
    adb(options.serial, "forward", f"tcp:{options.port}", f"tcp:{options.port}")


def stop_shell(options: Options) -> None:
    adb(options.serial, "forward", "--remove", f"tcp:{options.port}", check=False)
    adb(options.serial, "shell", "pkill -x ppsspp_headless", check=False)


def capture(options: Options) -> None:
    start_shell(options)
    client = Client(options.port)
    try:
        client.run_until(options.boot_time * 1_000_000)
        client.shot(options.out / "00_game_column.png")

        if options.umd_path:
            client.send("game.umd.insert", path=options.umd_path)
            client.wait("game.umd.insert", timeout=60)
            client.run_for(2_000_000)
            client.shot(options.out / "01_umd_selected.png")

        for index in range(1, 5):
            client.press("down")
            client.shot(options.out / f"02_down_{index}.png")
        for index in range(1, 5):
            client.press("up")
            client.shot(options.out / f"03_up_{index}.png")

        client.press("right", settle_us=120_000)
        client.shot(options.out / "04_category_0.png")
        for index in range(1, options.transition_frames + 1):
            client.run_for(120_000)
            client.shot(options.out / f"04_category_{index}.png")
        client.press("left", settle_us=120_000)
        client.shot(options.out / "05_back_0.png")
        for index in range(1, options.transition_frames + 1):
            client.run_for(120_000)
            client.shot(options.out / f"05_back_{index}.png")
    finally:
        client.close()
        stop_shell(options)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--device", required=True, help="adb serial")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--port", type=int, default=20816)
    parser.add_argument("--boot-time", type=float, default=25.0, help="emulated seconds before the first frame")
    parser.add_argument("--umd", dest="umd_path", help="device path of an ISO/CSO to insert")
    parser.add_argument("--transition-frames", type=int, default=7)
    parser.add_argument("--timeout", type=int, default=900, help="wall-clock limit for the shell process")
    args = parser.parse_args(argv)

    options = Options(
        serial=args.device,
        out=args.out,
        port=args.port,
        boot_time=args.boot_time,
        umd_path=args.umd_path,
        transition_frames=args.transition_frames,
        timeout=args.timeout,
    )
    options.out.mkdir(parents=True, exist_ok=True)
    try:
        capture(options)
    except CaptureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"reference frames in {options.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
