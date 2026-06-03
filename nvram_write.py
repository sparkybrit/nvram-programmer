#!/usr/bin/env python3
"""Send a binary file to the nvram-programmer Teensy++.

Usage:
    nvram_write.py <binary_file> [--port PORT] [--baud BAUD]

Progress is printed to stdout; mismatches are printed to stderr.
"""

import argparse
import sys
import time
import serial


CHUNK = 4096
# The firmware's inter-byte timeout is 5 s; give the host side a little headroom.
READ_TIMEOUT = 6.0


def read_until(port: serial.Serial, keyword: bytes, timeout: float) -> list[str]:
    """Read lines from port, echoing to stdout, until keyword is seen or timeout."""
    deadline = time.monotonic() + timeout
    buf = b""
    lines: list[str] = []
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line:
                    print(line, flush=True)
                    lines.append(line)
            if keyword in buf:
                # Drain the rest of the current line then return
                while b"\n" not in buf:
                    c = port.read(port.in_waiting or 1)
                    if not c:
                        break
                    buf += c
                if b"\n" in buf:
                    raw, _ = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").strip()
                    if line:
                        print(line, flush=True)
                        lines.append(line)
                return lines
    return lines


def send_file(port: serial.Serial, data: bytes) -> None:
    sent = 0
    while sent < len(data):
        port.write(data[sent : sent + CHUNK])
        sent += CHUNK


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Program a Dallas DS1250 NVSRAM via the nvram-programmer Teensy++."
    )
    parser.add_argument("binary", help="Binary image file to write")
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    args = parser.parse_args()

    with open(args.binary, "rb") as f:
        data = f.read()

    print(f"Image: {args.binary} ({len(data)} bytes)", flush=True)

    port = serial.Serial(
        args.port,
        args.baud,
        timeout=READ_TIMEOUT,
        write_timeout=10,
    )
    try:
        print(f"Opened {args.port} at {args.baud} baud.", flush=True)

        # Discard any data the Teensy sent before we connected (e.g. the ready prompt).
        time.sleep(0.1)
        port.reset_input_buffer()

        # --- Write phase ---
        # The Teensy is waiting for bytes; just send the file.
        print(f"Writing {len(data)} bytes...", flush=True)
        send_file(port, data)

        # Wait for the verify prompt, echoing progress dots and status lines.
        lines = read_until(port, b"Send file again to verify", timeout=120)
        if not any("Send file again to verify" in l or "Written" in l for l in lines):
            # read_until may have returned lines without seeing the keyword;
            # check the last few for a written-bytes confirmation.
            print("ERROR: write phase did not complete as expected.", file=sys.stderr)
            return 1

        # --- Verify phase ---
        print(f"\nVerifying {len(data)} bytes...", flush=True)
        send_file(port, data)

        # Read until PASS/FAIL or the port goes quiet.
        deadline = time.monotonic() + 120
        buf = b""
        result_lines: list[str] = []
        mismatches: list[str] = []
        done = False
        while time.monotonic() < deadline:
            chunk = port.read(port.in_waiting or 1)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").strip()
                    if line:
                        print(line, flush=True)
                        result_lines.append(line)
                        if "MISMATCH" in line:
                            print(line, file=sys.stderr)
                            mismatches.append(line)
                        if "PASS" in line or "FAIL" in line:
                            done = True
            if done:
                break

        if any("FAIL" in l for l in result_lines):
            print(f"\nVerification FAILED — {len(mismatches)} mismatch(es).", file=sys.stderr)
            return 1

        if any("PASS" in l for l in result_lines):
            print("\nVerification PASSED.", flush=True)
            return 0

        print("WARNING: no PASS/FAIL received from Teensy.", file=sys.stderr)
        return 1

    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
