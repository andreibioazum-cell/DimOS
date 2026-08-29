#!/usr/bin/env bash

set -Eeuo pipefail

# Boot from the hard disk image. The FAT12 floppy image is still built for
# release automation, but the floppy path is the one SeaBIOS fails on with
# "could not read the boot disk"; IDE/LBA reads are reliable.
IMAGE="disk_img/dimos.hdd"
FALLBACK_IMAGE="disk_img/dimos.img"

if [[ ! -f "$IMAGE" ]]; then
    if [[ -f "$FALLBACK_IMAGE" ]]; then
        # Older trees only ship the floppy image; it boots identically when
        # attached as an IDE disk (the loader uses LBA and the DL drive unit).
        IMAGE="$FALLBACK_IMAGE"
    else
        printf 'Image not found: %s\nRun "make iso" first.\n' "$IMAGE" >&2
        exit 1
    fi
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    printf 'qemu-system-x86_64 is not installed.\n' >&2
    exit 1
fi

printf 'Starting DimOS from %s (hard disk)...\n' "$IMAGE"
exec qemu-system-x86_64 \
    -display gtk \
    -boot order=c \
    -drive format=raw,file="$IMAGE",if=ide,index=0
