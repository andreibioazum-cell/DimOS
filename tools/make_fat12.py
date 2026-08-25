#!/usr/bin/env python3
"""Build a 1.44M FAT12 floppy that matches DimOS image_inspector."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

FLOPPY = 1_474_560
SECTOR = 512
RESERVED = 1
FAT_COUNT = 2
SECTORS_PER_FAT = 9
ROOT_ENTRIES = 224
TOTAL_SECTORS = 2880
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
DATA_LBA = RESERVED + FAT_COUNT * SECTORS_PER_FAT + ROOT_SECTORS


def fat12_set(fat: bytearray, cluster: int, value: int) -> None:
    offset = cluster + cluster // 2
    pair = fat[offset] | (fat[offset + 1] << 8)
    if cluster & 1:
        pair = (pair & 0x000F) | ((value & 0x0FFF) << 4)
    else:
        pair = (pair & 0xF000) | (value & 0x0FFF)
    fat[offset] = pair & 0xFF
    fat[offset + 1] = (pair >> 8) & 0xFF


def fat_name(filename: str) -> bytes:
    stem, dot, ext = filename.upper().partition(".")
    if not dot:
        ext = ""
    return (stem[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")


def put_file(image: bytearray, fat: bytearray, root: bytearray,
             name: str, data: bytes, next_cluster: int, next_entry: int) -> tuple[int, int]:
    clusters_needed = max(1, (len(data) + SECTOR - 1) // SECTOR) if data else 1
    first = next_cluster
    remaining = data
    cluster = next_cluster
    for index in range(clusters_needed):
        chunk = remaining[:SECTOR]
        remaining = remaining[SECTOR:]
        lba = DATA_LBA + (cluster - 2)
        start = lba * SECTOR
        image[start : start + len(chunk)] = chunk
        nxt = 0xFFF if index == clusters_needed - 1 else cluster + 1
        fat12_set(fat, cluster, nxt)
        cluster += 1
    entry = bytearray(32)
    entry[0:11] = fat_name(name)
    entry[11] = 0x20
    struct.pack_into("<H", entry, 26, first)
    struct.pack_into("<I", entry, 28, len(data))
    off = next_entry * 32
    root[off : off + 32] = entry
    return cluster, next_entry + 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("boot")
    parser.add_argument("output")
    parser.add_argument("files", nargs="+", help="name=path pairs or bare files")
    args = parser.parse_args()

    boot = pathlib.Path(args.boot).read_bytes()
    if len(boot) != SECTOR:
        print("BOOT.BIN must be 512 bytes", file=sys.stderr)
        return 1

    image = bytearray(FLOPPY)
    image[0:SECTOR] = boot

    fat = bytearray(SECTORS_PER_FAT * SECTOR)
    fat12_set(fat, 0, 0xFF0)
    fat12_set(fat, 1, 0xFFF)
    root = bytearray(ROOT_SECTORS * SECTOR)
    cluster = 2
    entry = 0

    for spec in args.files:
        if "=" in spec:
            name, path = spec.split("=", 1)
        else:
            path = spec
            name = pathlib.Path(path).name
        data = pathlib.Path(path).read_bytes()
        cluster, entry = put_file(image, fat, root, name, data, cluster, entry)

    fat_off = RESERVED * SECTOR
    image[fat_off : fat_off + len(fat)] = fat
    image[fat_off + len(fat) : fat_off + 2 * len(fat)] = fat
    root_off = (RESERVED + FAT_COUNT * SECTORS_PER_FAT) * SECTOR
    image[root_off : root_off + len(root)] = root

    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f"Wrote {out} ({len(image)} bytes, {entry} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
