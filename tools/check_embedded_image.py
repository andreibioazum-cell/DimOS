#!/usr/bin/env python3
"""Verify that web/dimos-image.js really boots the freshly built DimOS.

The launcher in index.html no longer reads disk_img/dimos.img: it gunzips the
payload committed in web/dimos-image.js. If that payload goes stale, the page
silently boots an old kernel, which is exactly the class of bug that produces
"Boot failed: could not read the boot disk" reports.

A byte-for-byte comparison against a fresh image would be useless here, because
mkfs.vfat stamps a random volume serial and mcopy stamps the current time. So
this script decodes the payload the same way the browser does and checks the
parts that are actually deterministic:

  * it is exactly a 1440 KB floppy;
  * sector 0 matches bin/BOOT.BIN, including the 0x55AA signature;
  * KERNEL.BIN, read through the real FAT12 cluster chain, matches
    bin/KERNEL.BIN byte for byte.
"""

from __future__ import annotations

import base64
import gzip
import hashlib
import re
import struct
import sys
from pathlib import Path

SECTOR = 512
FLOPPY_BYTES = 1474560


def fail(message: str) -> None:
    print(f"[ FAILED ] {message}", file=sys.stderr)
    raise SystemExit(1)


def decode_payload(js_path: Path) -> tuple[bytes, dict[str, str]]:
    text = js_path.read_text(encoding="utf-8")

    payload = re.search(r'gzipBase64:\s*"([A-Za-z0-9+/=]*)"', text)
    if not payload:
        fail(f"{js_path}: no gzipBase64 field found")

    raw = gzip.decompress(base64.b64decode(payload.group(1)))

    meta: dict[str, str] = {}
    size = re.search(r"size:\s*(\d+)", text)
    if size:
        meta["size"] = size.group(1)
    digest = re.search(r'sha256:\s*"([0-9a-f]+)"', text)
    if digest:
        meta["sha256"] = digest.group(1)
    return raw, meta


def read_fat12_file(image: bytes, name: str) -> bytes | None:
    reserved = struct.unpack_from("<H", image, 14)[0]
    num_fats = image[16]
    root_entries = struct.unpack_from("<H", image, 17)[0]
    sectors_per_fat = struct.unpack_from("<H", image, 22)[0]
    sectors_per_cluster = image[13]

    root_lba = reserved + num_fats * sectors_per_fat
    root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
    data_lba = root_lba + root_sectors

    fat = image[reserved * SECTOR : (reserved + sectors_per_fat) * SECTOR]
    root = image[root_lba * SECTOR : (root_lba + root_sectors) * SECTOR]

    target = name.ljust(11).encode("ascii")
    for index in range(root_entries):
        entry = root[index * 32 : index * 32 + 32]
        if not entry or entry[0] in (0x00, 0xE5):
            continue
        if entry[11] & 0x08:  # volume label
            continue
        if entry[0:11] != target:
            continue

        cluster = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]

        data = bytearray()
        guard = 0
        while 2 <= cluster < 0xFF0:
            guard += 1
            if guard > 4096:
                fail("FAT12 cluster chain does not terminate")
            lba = data_lba + (cluster - 2) * sectors_per_cluster
            data += image[lba * SECTOR : (lba + sectors_per_cluster) * SECTOR]

            offset = cluster + cluster // 2
            pair = struct.unpack_from("<H", fat, offset)[0]
            cluster = pair >> 4 if cluster & 1 else pair & 0x0FFF

        return bytes(data[:size])
    return None


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    js_path = root / "web" / "dimos-image.js"
    boot_path = root / "bin" / "BOOT.BIN"
    kernel_path = root / "bin" / "KERNEL.BIN"

    for path in (js_path, boot_path, kernel_path):
        if not path.is_file():
            fail(f"missing {path.relative_to(root)} (run 'make iso' first)")

    image, meta = decode_payload(js_path)

    if len(image) != FLOPPY_BYTES:
        fail(f"payload is {len(image)} bytes, expected {FLOPPY_BYTES}")

    if "size" in meta and int(meta["size"]) != len(image):
        fail(f"declared size {meta['size']} does not match {len(image)}")

    digest = hashlib.sha256(image).hexdigest()
    if "sha256" in meta and meta["sha256"] != digest:
        fail(f"declared sha256 {meta['sha256']} does not match {digest}")

    if image[510] != 0x55 or image[511] != 0xAA:
        fail("payload has no 0x55AA boot signature: SeaBIOS would refuse it")

    boot = boot_path.read_bytes()
    if image[:SECTOR] != boot:
        fail("boot sector in the payload differs from bin/BOOT.BIN (stale image)")

    kernel = kernel_path.read_bytes()
    embedded_kernel = read_fat12_file(image, "KERNEL  BIN")
    if embedded_kernel is None:
        fail("KERNEL.BIN is not present in the payload's FAT12 root directory")
    if embedded_kernel != kernel:
        fail(
            "KERNEL.BIN in the payload is stale "
            f"({len(embedded_kernel)} bytes vs {len(kernel)} bytes freshly built). "
            "Run 'make iso' and commit web/dimos-image.js."
        )

    print(
        "Validated embedded launcher image: "
        f"{len(image)} bytes, boot sector matches, KERNEL.BIN {len(kernel)} bytes, "
        f"sha256 {digest[:16]}..."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
