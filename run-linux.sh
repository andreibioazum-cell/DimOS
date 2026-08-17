#!/usr/bin/env bash

set -Eeuo pipefail

IMAGE="disk_img/dimos.img"

if [[ ! -f "$IMAGE" ]]; then
    printf 'Image not found: %s\nRun "make iso" first.\n' "$IMAGE" >&2
    exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    printf 'qemu-system-x86_64 is not installed.\n' >&2
    exit 1
fi

printf 'Starting DimOS Minimal...\n'
exec qemu-system-x86_64 \
    -display gtk \
    -drive format=raw,file="$IMAGE",if=floppy,index=0
