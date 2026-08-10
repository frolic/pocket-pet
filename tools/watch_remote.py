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
    data = bytearray()
    size = None
    while True:
        line = port.readline().decode(errors="replace").strip()
        if line.startswith(f"{tag} begin"):
            size = int(line.split()[-1])
            break
    while True:
        line = port.readline().decode(errors="replace").strip()
        if line == f"{tag} end":
            break
        if not line or " " in line:
            continue
        try:
            data.extend(base64.b64decode(line))
        except Exception:
            pass
    return bytes(data[:size])


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
