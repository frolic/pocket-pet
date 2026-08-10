#!/usr/bin/env python3
"""Remote-drive the watch over USB serial: screenshots and synthetic taps.

Usage:
  tools/watch_remote.py snap out.png       composite screen+top-layer snapshot
  tools/watch_remote.py tap X Y            inject a touch at panel coords
"""
import base64
import sys
import serial
from PIL import Image

PORT = "/dev/cu.usbmodem2101"
WIDTH, HEIGHT = 410, 502


def read_blob(port, tag):
    """Offset-tagged transfer: any dropped line is detected and zero-filled,
    and the caller learns how many bytes were lost."""
    size = None
    while True:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith(f"{tag} begin"):
            size = int(line.split()[-1])
            break
    data = bytearray(size)
    received = 0
    while True:
        line = port.readline().decode(errors="replace").strip()
        if line == f"{tag} end":
            break
        if not line.startswith("@") or ":" not in line:
            continue
        offset_str, payload = line[1:].split(":", 1)
        try:
            offset = int(offset_str)
            chunk = base64.b64decode(payload)
        except Exception:
            continue
        if offset + len(chunk) <= size:
            data[offset:offset + len(chunk)] = chunk
            received += len(chunk)
    lost = size - received
    if lost:
        print(f"{tag}: WARNING {lost} bytes lost in transfer")
    return bytes(data)


def snap(out_path):
    port = serial.Serial(PORT, 115200, timeout=5)
    port.write(b"snap\n")
    port.flush()
    screen_raw = read_blob(port, "SNAPSCREEN")
    top_raw = read_blob(port, "SNAPTOP")
    port.close()

    stride = len(screen_raw) // HEIGHT
    screen = Image.frombytes("RGB", (WIDTH, HEIGHT), b"".join(
        bytes((
            (int.from_bytes(screen_raw[r * stride + c * 2:r * stride + c * 2 + 2], "little") >> 11 << 3) & 0xFF,
            (int.from_bytes(screen_raw[r * stride + c * 2:r * stride + c * 2 + 2], "little") >> 5 << 2) & 0xFF,
            (int.from_bytes(screen_raw[r * stride + c * 2:r * stride + c * 2 + 2], "little") << 3) & 0xFF,
        ))
        for r in range(HEIGHT) for c in range(WIDTH)))

    top_stride = len(top_raw) // HEIGHT
    top = Image.frombuffer("RGBA", (WIDTH, HEIGHT), bytes(
        b for r in range(HEIGHT) for c in range(WIDTH)
        for b in (top_raw[r * top_stride + c * 4 + 2],
                  top_raw[r * top_stride + c * 4 + 1],
                  top_raw[r * top_stride + c * 4 + 0],
                  top_raw[r * top_stride + c * 4 + 3])))
    screen = screen.convert("RGBA")
    screen.alpha_composite(top)
    screen.convert("RGB").save(out_path)
    print(f"saved {out_path}")


def tap(x, y):
    port = serial.Serial(PORT, 115200, timeout=3)
    port.write(f"tap {x} {y}\n".encode())
    port.flush()
    end = 20
    for _ in range(end):
        line = port.readline().decode(errors="replace").strip()
        if line.startswith("tap:"):
            print(line)
            break
    port.close()


if __name__ == "__main__":
    if sys.argv[1] == "snap":
        snap(sys.argv[2])
    elif sys.argv[1] == "tap":
        tap(int(sys.argv[2]), int(sys.argv[3]))
