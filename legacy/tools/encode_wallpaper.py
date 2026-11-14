#!/usr/bin/env python3
"""Convert the PNG desktop art into a small 16-colour kernel include.

Requires ImageMagick's `convert` command only when regenerating the checked-in
header. The kernel itself has no PNG decoder or external runtime dependency.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "assets" / "windows10_wallpaper.png"
DEFAULT_OUTPUT = ROOT / "src" / "kernel" / "wallpaper_data.h"
WIDTH = 160
HEIGHT = 100


def read_indexed_bmp(path: Path) -> tuple[list[tuple[int, int, int]], list[int]]:
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise ValueError("ImageMagick did not produce a BMP file")

    pixels_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    signed_height = struct.unpack_from("<i", data, 22)[0]
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    colors_used = struct.unpack_from("<I", data, 46)[0]
    height = abs(signed_height)

    if width != WIDTH or height != HEIGHT or bits_per_pixel != 4:
        raise ValueError(
            f"Expected {WIDTH}x{HEIGHT} 4-bit BMP, got "
            f"{width}x{height} {bits_per_pixel}-bit"
        )

    color_count = colors_used or 16
    palette_offset = 14 + header_size
    palette = []
    for index in range(color_count):
        blue, green, red, _alpha = data[palette_offset + index * 4:
                                        palette_offset + index * 4 + 4]
        palette.append((red, green, blue))
    while len(palette) < 16:
        palette.append((0, 0, 0))

    row_bytes = ((width * bits_per_pixel + 31) // 32) * 4
    indices: list[int] = []
    for y in range(height):
        source_y = height - 1 - y if signed_height > 0 else y
        row_start = pixels_offset + source_y * row_bytes
        row = data[row_start:row_start + row_bytes]
        for x in range(width):
            value = row[x // 2]
            indices.append((value >> 4) & 0x0F if x % 2 == 0 else value & 0x0F)

    return palette[:16], indices


def format_header(palette: list[tuple[int, int, int]], indices: list[int]) -> str:
    packed = [indices[index] | (indices[index + 1] << 4)
              for index in range(0, len(indices), 2)]
    lines = [
        "/* Generated from assets/windows10_wallpaper.png by tools/encode_wallpaper.py. */",
        "#ifndef DIMOS_WALLPAPER_DATA_H",
        "#define DIMOS_WALLPAPER_DATA_H",
        "",
        f"#define WINDOWS_WALLPAPER_WIDTH {WIDTH}u",
        f"#define WINDOWS_WALLPAPER_HEIGHT {HEIGHT}u",
        "static const u8 windows_wallpaper_palette[16][3] = {",
    ]
    for red, green, blue in palette:
        lines.append(f"    {{ {red}u, {green}u, {blue}u }},")
    lines.extend([
        "};",
        "static const u8 windows_wallpaper_pixels[WINDOWS_WALLPAPER_WIDTH *",
        "                                               WINDOWS_WALLPAPER_HEIGHT / 2u] = {",
    ])
    for start in range(0, len(packed), 16):
        chunk = packed[start:start + 16]
        lines.append("    " + ", ".join(f"0x{value:02X}u" for value in chunk) + ",")
    lines.extend(["};", "", "#endif", ""])
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="dimos-wallpaper-") as temporary:
        bmp = Path(temporary) / "wallpaper.bmp"
        subprocess.run([
            "convert", str(args.source), "-resize", f"{WIDTH}x{HEIGHT}!",
            "+dither", "-colors", "16", "-depth", "8", f"BMP3:{bmp}",
        ], check=True)
        palette, indices = read_indexed_bmp(bmp)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(format_header(palette, indices), encoding="utf-8")
    print(f"Wrote {args.output} ({WIDTH}x{HEIGHT}, 4-bit palette)")


if __name__ == "__main__":
    main()
