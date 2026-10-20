#!/usr/bin/env bash

set -Eeuo pipefail

# Boot from the hard disk image. The FAT12 floppy image is still built for
# release automation, but the floppy path is the one SeaBIOS fails on with
# "could not read the boot disk"; IDE/LBA reads are reliable.
IMAGE="disk_img/dimos.hdd"
FALLBACK_IMAGE="disk_img/dimos.img"

# DimOS targets a 1.5K phone panel: 2712 x 1220, which renders the 844 x 390
# canvas at 3.2x. QEMU's standard VGA adapter reports whatever resolution it
# was created with as its preferred VBE mode, so ask for exactly that panel.
# Override with DIMOS_DISPLAY=1920x1080 for a smaller window.
DISPLAY_RESOLUTION="${DIMOS_DISPLAY:-2712x1220}"
PANEL_WIDTH="${DISPLAY_RESOLUTION%x*}"
PANEL_HEIGHT="${DISPLAY_RESOLUTION#*x}"

# 2712 x 1220 x 4 bytes is 13.2 MiB, so the adapter needs a 32 MiB VRAM
# window; the QEMU default of 16 MiB cannot hold it.
PANEL_VRAM_MB=$(( (PANEL_WIDTH * PANEL_HEIGHT * 4 + 1024 * 1024 - 1) / 1024 / 1024 ))
if (( PANEL_VRAM_MB < 16 )); then
    PANEL_VRAM_MB=16
elif (( PANEL_VRAM_MB < 32 )); then
    PANEL_VRAM_MB=32
fi

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

printf 'Starting DimOS from %s on a %s panel (hard disk)...\n' \
    "$IMAGE" "$DISPLAY_RESOLUTION"
exec qemu-system-x86_64 \
    -display gtk,zoom-to-fit=on \
    -vga none \
    -device "VGA,vgamem_mb=${PANEL_VRAM_MB},xres=${PANEL_WIDTH},yres=${PANEL_HEIGHT},edid=on" \
    -boot order=c \
    -drive format=raw,file="$IMAGE",if=ide,index=0
