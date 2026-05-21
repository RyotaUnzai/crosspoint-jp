#!/usr/bin/env python3
"""Capture EPUB vertical layout diagnostics from the XTEINK serial log."""

from __future__ import annotations

import argparse
import glob
import platform
import time
from datetime import datetime
from pathlib import Path

import serial


DIAGNOSTIC_TAGS = ("VLAY", "VREN", "VPAGE", "ERS", "SCT", "EHP", "EBP", "MEM", "MAIN")
CRASH_PATTERNS = ("rst:", "Guru Meditation", "panic", "abort", "Backtrace", "Exception", "ELF file SHA256")


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
    ser.timeout = 0.2
    ser.write_timeout = 2
    ser.dtr = False
    ser.rts = False
    ser.open()
    ser.dtr = False
    ser.rts = False
    return ser


def is_diagnostic_line(line: str) -> bool:
    if any(pattern in line for pattern in CRASH_PATTERNS):
        return True
    return any(f"[{tag}]" in line or f"] {tag}:" in line or line.startswith(f"{tag}:") for tag in DIAGNOSTIC_TAGS)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Capture vertical EPUB layout diagnostics over serial.")
    parser.add_argument("port", nargs="?", default=None, help="Serial port, for example COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=90, help="How long to capture logs")
    parser.add_argument("--output-dir", type=Path, default=repo_root / "Logs")
    parser.add_argument("--all", action="store_true", help="Save every serial line, not only layout diagnostics")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    if not port:
        print("Could not auto-detect serial port. Pass it explicitly, for example COM3.")
        return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = args.output_dir / f"vertical-layout-{stamp}.log"

    print(f"Opening {port} at {args.baud} baud")
    print(f"Saving diagnostics to {log_path}")
    print("Open the EPUB on the device now. Press Ctrl-C to stop early.")

    deadline = time.monotonic() + args.seconds
    captured = 0
    with open_serial(port, args.baud) as ser, log_path.open("w", encoding="utf-8", newline="\n") as out:
        try:
            while time.monotonic() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                if args.all or is_diagnostic_line(line):
                    print(line)
                    out.write(line + "\n")
                    out.flush()
                    captured += 1
        except KeyboardInterrupt:
            pass

    print(f"Captured {captured} lines")
    print(log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
