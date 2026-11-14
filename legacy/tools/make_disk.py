#!/usr/bin/env python3
"""make_disk.py -- build the bootable DimOS disk images without mtools.

The main build (build-linux.sh) uses mkfs.vfat and mcopy. This script does
the same FAT12 layout with plain Python, so the images can also be made on
a machine that only has nasm and a C compiler -- and so the files that are
planted on the volume (the kernel, the font, the sample documents) land in
exactly the clusters the bootloader's data window expects.

Usage:
    python3 tools/make_disk.py bin/BOOT.BIN bin/KERNEL.BIN fonts/font.ttf \
        files/README.TXT disk_img/dimos.img [--hdd disk_img/dimos.hdd]

The volume is a fixed 1.44 MiB FAT12 floppy: 1 reserved sector, two 9
sector FATs, 224 root entries and 1 sector per cluster, exactly the layout
the boot sector's BPB describes and README documents.
"""

import os
import sys

SECTOR = 512
TOTAL_SECTORS = 2880
RESERVED = 1
FAT_COPIES = 2
SECTORS_PER_FAT = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
DATA_LBA = RESERVED + FAT_COPIES * SECTORS_PER_FAT + ROOT_SECTORS
CLUSTERS = TOTAL_SECTORS - DATA_LBA


def fat12_set(fat, cluster, value):
    offset = cluster * 3 // 2
    if cluster % 2 == 0:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)
    else:
        fat[offset] = (fat[offset] & 0x0F) | ((value & 0x0F) << 4)
        fat[offset + 1] = (value >> 4) & 0xFF


def short_name(name):
    """Maps a file name to the 11 byte 8.3 field of a directory entry."""
    base, _, extension = os.path.basename(name).upper().partition(".")
    base = base[:8].ljust(8)
    extension = extension[:3].ljust(3)
    return (base + extension).encode("ascii")


class Volume:
    def __init__(self, label="DIMOS"):
        self.image = bytearray(TOTAL_SECTORS * SECTOR)
        self.fat = bytearray(SECTORS_PER_FAT * SECTOR)
        self.root = bytearray(ROOT_ENTRIES * 32)
        self.next_cluster = 2
        self.entries = 0
        fat12_set(self.fat, 0, 0xFF0)
        fat12_set(self.fat, 1, 0xFFF)
        self.label = label

    def add(self, data, name):
        """Stores one file in a fresh, contiguous chain of clusters."""
        if not data:
            data = b"\0"
        clusters = (len(data) + SECTOR - 1) // SECTOR
        first = self.next_cluster
        if first + clusters - 2 >= CLUSTERS or self.entries >= ROOT_ENTRIES:
            raise SystemExit("the volume is full")
        for index in range(clusters):
            cluster = first + index
            last = index == clusters - 1
            fat12_set(self.fat, cluster, 0xFFF if last else cluster + 1)
        offset = (first - 2) * SECTOR
        self.image[DATA_LBA * SECTOR + offset:
                    DATA_LBA * SECTOR + offset + len(data)] = data
        self.next_cluster += clusters

        entry = bytearray(32)
        entry[0:11] = short_name(name)
        entry[11] = 0x20
        entry[26] = first & 0xFF
        entry[27] = (first >> 8) & 0xFF
        entry[28:32] = len(data).to_bytes(4, "little")
        self.root[self.entries * 32:self.entries * 32 + 32] = entry
        self.entries += 1
        return clusters

    def finish(self, boot, path, hdd_path=None):
        self.image[0:SECTOR] = boot.ljust(SECTOR, b"\0")
        # Keep the volume label the boot sector already carries: deriving
        # the BPB from the file keeps one source of truth for the layout.
        self.image[SECTOR:SECTOR + len(self.fat)] = self.fat
        for copy in range(1, FAT_COPIES):
            start = (RESERVED + copy * SECTORS_PER_FAT) * SECTOR
            self.image[start:start + len(self.fat)] = self.fat
        root_start = (RESERVED + FAT_COPIES * SECTORS_PER_FAT) * SECTOR
        self.image[root_start:root_start + len(self.root)] = self.root
        with open(path, "wb") as handle:
            handle.write(self.image)
        print(f"  {path}: {len(self.image)} bytes, "
              f"{self.entries} files, {self.next_cluster - 2} clusters used")
        if hdd_path:
            with open(hdd_path, "wb") as handle:
                handle.write(self.image)
                handle.write(b"\0" * (8 * 1024 * 1024 - len(self.image)))
            print(f"  {hdd_path}: 8 MiB hard disk view of the same volume")


def main():
    arguments = sys.argv[1:]
    hdd = None
    if "--hdd" in arguments:
        position = arguments.index("--hdd")
        hdd = arguments[position + 1]
        del arguments[position:position + 2]
    if len(arguments) < 3:
        raise SystemExit(__doc__)

    output = arguments[-1]
    boot_path = arguments[0]
    payload = arguments[1:-1]

    with open(boot_path, "rb") as handle:
        boot = handle.read()
    if len(boot) != SECTOR or boot[510:512] != b"\x55\xaa":
        raise SystemExit("the boot sector must be exactly 512 bytes with 55 AA")

    volume = Volume()
    print("  disk contents:")
    for path in payload:
        with open(path, "rb") as handle:
            data = handle.read()
        clusters = volume.add(data, path)
        print(f"    {os.path.basename(path).ljust(14)} {len(data):8d} bytes"
              f"  {clusters} cluster(s)")
    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    volume.finish(boot, output, hdd)


if __name__ == "__main__":
    main()
