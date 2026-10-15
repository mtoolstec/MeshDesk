#!/usr/bin/env python3
"""Install one declarative app onto a connected Wio Tracker L2 SD card."""

import argparse
import pathlib
import sys
import time

import serial


def wait_for_line(device: serial.Serial, prefix: bytes, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = device.readline().strip()
        if line.startswith(b"@WIOERR"):
            raise RuntimeError(line.decode(errors="replace"))
        if line.startswith(prefix):
            return line
    raise TimeoutError(f"device did not reply with {prefix.decode()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app_dir", type=pathlib.Path)
    parser.add_argument("--port", required=True, help="USB serial port, e.g. /dev/cu.usbmodem144401")
    parser.add_argument("--no-reboot", action="store_true")
    parser.add_argument("--chunk-size", type=int, default=24,
                        help="binary bytes per acknowledged USB frame (default: 24)")
    args = parser.parse_args()
    if not 1 <= args.chunk_size <= 64:
        parser.error("--chunk-size must be between 1 and 64")

    manifest = args.app_dir / "app.json"
    if not manifest.is_file():
        parser.error(f"missing {manifest}")
    app_id = args.app_dir.name
    if not app_id or any(ch not in "abcdefghijklmnopqrstuvwxyz0123456789-_" for ch in app_id.lower()):
        parser.error("app directory name may only contain letters, digits, '-' and '_'")
    manifest_payload = manifest.read_bytes()
    if not manifest_payload or len(manifest_payload) > 4096:
        parser.error("app.json must contain 1..4096 bytes")
    files = [path for path in sorted(args.app_dir.iterdir()) if path.is_file() and not path.name.startswith(".")]

    with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=3) as device:
        time.sleep(0.25)
        device.reset_input_buffer()
        device.write(b"@WIOPING\n")
        wait_for_line(device, b"@WIOPONG", 3)
        time.sleep(0.1)
        remote_dir = f"/apps/{app_id}".encode()
        device.write(b"@WIOMKDIR " + remote_dir + b"\n")
        wait_for_line(device, b"@WIOOK", 3)
        time.sleep(0.1)
        for path in files:
            payload = path.read_bytes()
            if not payload or len(payload) > 2 * 1024 * 1024:
                parser.error(f"{path.name} must contain 1..2097152 bytes")
            remote_path = remote_dir + b"/" + path.name.encode()
            device.write(b"@WIOPUT " + remote_path + b" " + str(len(payload)).encode() + b"\n")
            wait_for_line(device, b"@WIOREADY", 3)
            time.sleep(0.1)
            for offset in range(0, len(payload), args.chunk_size):
                chunk = payload[offset : offset + args.chunk_size]
                device.write(b"@WIOCHUNK " + chunk.hex().encode() + b"\n")
                device.flush()
                expected = b"@WIOOK" if offset + len(chunk) == len(payload) else b"@WIOACK"
                try:
                    result = wait_for_line(device, expected, 10)
                except (TimeoutError, RuntimeError) as error:
                    raise RuntimeError(f"{path.name} failed at byte {offset}/{len(payload)}: {error}") from error
                time.sleep(0.05)
            print(f"Installed {app_id}/{path.name}: {result.decode(errors='replace')}")
        if not args.no_reboot:
            device.write(b"@WIOREBOOT\n")
            wait_for_line(device, b"@WIOOK", 3)
            print("Device restarting; the app will appear after the SD scan.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (serial.SerialException, TimeoutError, RuntimeError) as error:
        print(f"install failed: {error}", file=sys.stderr)
        raise SystemExit(1)
