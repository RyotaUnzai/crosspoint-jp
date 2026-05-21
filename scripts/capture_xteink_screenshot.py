#!/usr/bin/env python3
"""Capture the connected XTEINK framebuffer over the firmware serial protocol."""

from __future__ import annotations

import argparse
import glob
import platform
import sys
import time
from datetime import datetime
from pathlib import Path

import serial

try:
    from PIL import Image
except ImportError:
    Image = None


KNOWN_GEOMETRIES = {
    48000: (800, 480),
    52272: (792, 528),
}


def auto_detect_port() -> str | None:
    if platform.system() in ("Darwin", "Linux"):
        ports = sorted(glob.glob("/dev/tty.usbmodem*" if platform.system() == "Darwin" else "/dev/ttyACM*"))
        return ports[0] if ports else None

    if platform.system() == "Windows":
        from serial.tools import list_ports

        patterns = ("CP210x", "CH340", "USB Serial")
        for port in list_ports.comports():
            if any(pattern in port.description for pattern in patterns) or port.hwid.startswith("USB VID:PID=303A:1001"):
                return port.device
    return None


def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.write_timeout = 2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def wait_for_marker(ser: serial.Serial, timeout: float) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print(line)
        if line.startswith("SCREENSHOT_START:"):
            return int(line.split(":", 1)[1])
    raise TimeoutError("SCREENSHOT_START was not received")


def read_exact(ser: serial.Serial, size: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < size and time.monotonic() < deadline:
        chunk = ser.read(size - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != size:
        raise TimeoutError(f"Expected {size} bytes, received {len(data)} bytes")
    return bytes(data)


def save_capture(data: bytes, output_dir: Path, rotate: int) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    raw_path = output_dir / f"xteink-screenshot-{stamp}.raw"
    raw_path.write_bytes(data)

    geometry = KNOWN_GEOMETRIES.get(len(data))
    if geometry is None or Image is None:
        return raw_path

    image = Image.frombytes("1", geometry, data)
    if rotate:
        image = image.rotate(-rotate, expand=True)

    bmp_path = output_dir / f"xteink-screenshot-{stamp}.bmp"
    image.save(bmp_path)
    return bmp_path


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Capture an XTEINK screenshot over serial.")
    parser.add_argument("port", nargs="?", default=None, help="Serial port, for example COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--delay", type=float, default=0, help="Seconds to wait before sending the request")
    parser.add_argument("--wait-for-enter", action="store_true", help="Wait for Enter before sending the request")
    parser.add_argument("--timeout", type=float, default=30, help="Seconds to wait for each response phase")
    parser.add_argument("--rotate", type=int, default=90, choices=[0, 90, 180, 270])
    parser.add_argument("--output-dir", type=Path, default=repo_root / "Screenshots")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    if not port:
        print("No serial port found. Pass one explicitly, for example: COM3", file=sys.stderr)
        return 2

    print(f"Opening {port}...")
    with open_serial(port, args.baud) as ser:
        if args.delay > 0:
            print(f"Waiting {args.delay:g} seconds...")
            time.sleep(args.delay)
        if args.wait_for_enter:
            input("Navigate the XTEINK to the desired screen, then press Enter to capture...")

        ser.reset_input_buffer()
        print("Sending CMD:SCREENSHOT")
        ser.write(b"CMD:SCREENSHOT\n")
        ser.flush()

        size = wait_for_marker(ser, args.timeout)
        print(f"Receiving {size} bytes...")
        data = read_exact(ser, size, args.timeout)
        saved_path = save_capture(data, args.output_dir, args.rotate)
        print(f"Saved {saved_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
