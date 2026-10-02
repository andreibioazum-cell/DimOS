#!/usr/bin/env python3
"""Generate DimOS' two native-resolution PNG wallpapers.

The files intentionally use the small PNG subset understood by the freestanding
kernel: 8-bit RGB, no interlace, filter None and one fixed-Huffman DEFLATE
stream.  They remain ordinary standards-compliant PNG files.
"""

from __future__ import annotations

import binascii
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "images"


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF))


def mix(a: tuple[int, int, int], b: tuple[int, int, int], n: int, d: int) -> tuple[int, int, int]:
    return tuple((a[i] * (d - n) + b[i] * n) // d for i in range(3))


def scene(width: int, height: int, wide: bool) -> bytes:
    rows = bytearray()
    horizon = height * (61 if wide else 58) // 100
    sun_x = width * (74 if wide else 76) // 100
    sun_y = height * 27 // 100
    sun_r = height * 9 // 100

    def ridge(x: int, points: tuple[tuple[int, int], ...]) -> int:
        for (x0, y0), (x1, y1) in zip(points, points[1:]):
            if x <= x1:
                return y0 + (y1 - y0) * (x - x0) // max(1, x1 - x0)
        return points[-1][1]

    back = ((0, horizon), (width * 18 // 100, height * 34 // 100),
            (width * 35 // 100, horizon), (width * 52 // 100, height * 40 // 100),
            (width * 70 // 100, horizon), (width * 86 // 100, height * 37 // 100),
            (width, horizon))
    front = ((0, horizon + height // 18), (width * 13 // 100, height * 46 // 100),
             (width * 30 // 100, horizon + height // 16),
             (width * 48 // 100, height * 41 // 100),
             (width * 64 // 100, horizon + height // 13),
             (width * 83 // 100, height * 45 // 100),
             (width, horizon + height // 14))

    for y in range(height):
        rows.append(0)  # PNG filter: None
        if y < horizon:
            base = mix((28, 42, 92), (132, 191, 220), y, horizon)
        else:
            base = mix((38, 100, 135), (13, 35, 72), y - horizon, height - horizon)

        for x in range(width):
            color = base
            dx, dy = x - sun_x, y - sun_y
            distance2 = dx * dx + dy * dy
            if distance2 < (sun_r * 2) ** 2 and y < horizon:
                glow = max(0, sun_r * 2 - int(distance2 ** 0.5))
                color = mix(color, (255, 175, 130), glow, sun_r * 2)
            if distance2 <= sun_r * sun_r:
                color = (255, 210, 151)

            back_y = ridge(x, back)
            front_y = ridge(x, front)
            if back_y <= y < horizon + height // 20:
                color = mix((83, 103, 153), (55, 82, 128), y - back_y, max(1, horizon - back_y + height // 20))
            if front_y <= y < horizon + height // 11:
                color = mix((43, 92, 119), (28, 64, 95), y - front_y, max(1, horizon - front_y + height // 11))

            if y >= horizon:
                # Calm native-resolution reflections; no random noise keeps the
                # PNG tiny enough for the boot volume.
                band = (y - horizon) // max(2, height // 180)
                if band % 9 == 2 and abs(x - sun_x) < (height - y) // 3 + width // 30:
                    color = mix(color, (241, 171, 137), 2, 5)
                elif band % 13 == 5 and ((x + band * 17) % max(20, width // 12)) < width // 32:
                    color = mix(color, (121, 183, 202), 1, 3)

            # A restrained vignette leaves desktop labels readable.
            edge = min(x, width - 1 - x)
            if edge < width // 16:
                color = mix(color, (17, 29, 64), width // 16 - edge, width // 16 * 3)
            # Four-bit channel quantization is visually smooth at these
            # resolutions and makes fixed-Huffman PNG dramatically smaller.
            rows.extend((channel & 0xF0) for channel in color)
    return bytes(rows)


def write_png(name: str, width: int, height: int, wide: bool) -> None:
    raw = scene(width, height, wide)
    compressor = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_FIXED)
    packed = compressor.compress(raw) + compressor.flush()
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", packed) + chunk(b"IEND", b"")
    path = OUT / name
    path.write_bytes(data)
    print(f"{path.relative_to(ROOT)}: {width}x{height}, {len(data)} bytes")


def main() -> None:
    write_png("wallpaper-emulator.png", 640, 480, False)
    write_png("wallpaper-pc.png", 1920, 1080, True)


if __name__ == "__main__":
    main()
