#!/usr/bin/env bash
#
# Build the DimOS live image.
#
# The result is a normal Arch Linux live system whose only user-visible
# program is the DimOS binary (rendered with sokol). It boots into the DimOS
# desktop directly: no boot menu, no login prompt, no window decorations.
#
#   ./iso/build-iso.sh
#
# Outputs:
#   dist/dimos.iso   bootable CD/DVD image (BIOS + UEFI)
#   dist/dimos.img   the same image as a raw disk: dd it to a USB stick
#
# Runs on Arch Linux (as root, with the `archiso` package) or on any Linux
# with Docker, in which case it re-executes itself inside an Arch container.
set -Eeuo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK="$HERE/work"
PROFILE="$WORK/profile"
OUT="$WORK/out"
DIST="$ROOT/dist"
RELENG="/usr/share/archiso/configs/releng"
APP_BIN="$HERE/airootfs/usr/local/bin/dimos"

log() { printf '\n==> %s\n' "$*"; }

# --------------------------------------------------------------------------
# 0. If this is not Arch, run the whole thing inside an Arch container.
# --------------------------------------------------------------------------
if ! command -v mkarchiso >/dev/null 2>&1; then
    if command -v docker >/dev/null 2>&1; then
        log "mkarchiso not found: building inside an archlinux container"
        exec docker run --rm --privileged \
            -v "$ROOT:/work" -w /work/iso \
            archlinux:latest bash -lc '
                pacman -Syu --noconfirm --needed \
                    archiso base-devel python libx11 libxi libxcursor mesa >/dev/null
                ./build-iso.sh
            '
    fi
    cat >&2 <<'MSG'
This script needs archiso (Arch Linux) or Docker.

  Arch Linux:  sudo pacman -S --needed archiso base-devel python libx11 libxi libxcursor mesa
               sudo ./iso/build-iso.sh

  Any Linux:   install Docker, then run ./iso/build-iso.sh (it will pull
               archlinux:latest and build there)
MSG
    exit 1
fi

if [[ $EUID -ne 0 ]]; then
    printf 'Run as root (mkarchiso needs to mount the image): sudo %s\n' "$0" >&2
    exit 1
fi

[[ -d "$RELENG" ]] || { printf 'missing %s -- install the archiso package\n' "$RELENG" >&2; exit 1; }

# --------------------------------------------------------------------------
# 1. Build the one program the image runs.
# --------------------------------------------------------------------------
log "Compiling the DimOS binary for the live image"
mkdir -p "$ROOT/build" "$(dirname "$APP_BIN")"
python3 "$ROOT/tools/embed_font.py" "$ROOT/fonts/font.ttf" "$ROOT/build/font_data.h"
cc -std=c11 -O2 \
    -I"$ROOT/src" -I"$ROOT/third_party/sokol" -I"$ROOT/third_party/stb" -I"$ROOT/build" \
    "$ROOT/src/app.c" "$ROOT/src/snake.c" "$ROOT/src/draw.c" "$ROOT/src/platform_sokol.c" \
    -o "$APP_BIN" \
    -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
chmod 755 "$APP_BIN"
printf '    %s (%s)\n' "$APP_BIN" "$(du -h "$APP_BIN" | cut -f1)"

# --------------------------------------------------------------------------
# 2. Assemble the archiso profile: releng + our package list + our overlay.
# --------------------------------------------------------------------------
log "Preparing the archiso profile"
rm -rf "$WORK"
mkdir -p "$PROFILE" "$OUT" "$DIST"
cp -r "$RELENG"/. "$PROFILE"/
cp "$HERE/packages.x86_64" "$PROFILE/packages.x86_64"
cp -r "$HERE/airootfs/." "$PROFILE/airootfs/" 2>/dev/null || mkdir -p "$PROFILE/airootfs"
chmod 755 "$PROFILE/airootfs/root/.xinitrc" "$PROFILE/airootfs/root/.bash_profile" "$PROFILE/airootfs/root/.zprofile" 2>/dev/null || true

sed -i 's/^iso_name=.*/iso_name="dimos"/' "$PROFILE/profiledef.sh"
sed -i 's/^iso_label=.*/iso_label="DIMOS"/' "$PROFILE/profiledef.sh"
sed -i 's/^iso_application=.*/iso_application="DimOS live"/' "$PROFILE/profiledef.sh"
# zstd at a high level: fast enough to build on CI, small enough to download.
sed -i "s/^airootfs_image_tool_options=.*/airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')/" \
    "$PROFILE/profiledef.sh"

# Kernel command line: keep the console visible (the Arch boot log is the
# "startup screen") and force the safe VESA path so the image also boots on
# machines without a KMS driver.
find "$PROFILE/syslinux" -type f -name '*.cfg' -exec \
    sed -i -E 's/^([[:space:]]*APPEND.*)$/\1 nomodeset console=tty0 console=ttyS0,115200/' {} +
find "$PROFILE/efiboot" -type f -name '*.conf' -exec \
    sed -i -E 's/^(options .*)$/\1 nomodeset console=tty0 console=ttyS0,115200/' {} +

# No boot menu: go straight to the kernel.
find "$PROFILE/syslinux" -type f -name '*.cfg' -exec \
    sed -i -E 's/^[[:space:]]*TIMEOUT[[:space:]].*$/TIMEOUT 1/; s/^[[:space:]]*PROMPT[[:space:]].*$/PROMPT 0/' {} +
if [[ -f "$PROFILE/efiboot/loader/loader.conf" ]]; then
    sed -i -E 's/^[[:space:]]*timeout[[:space:]].*/timeout 0/' "$PROFILE/efiboot/loader/loader.conf"
    grep -q '^timeout' "$PROFILE/efiboot/loader/loader.conf" || echo 'timeout 0' >> "$PROFILE/efiboot/loader/loader.conf"
fi

# --------------------------------------------------------------------------
# 3. Build.
# --------------------------------------------------------------------------
log "Running mkarchiso (this takes a few minutes)"
mkarchiso -v -w "$WORK/iso" -o "$OUT" "$PROFILE"

ISO_SOURCE="$(find "$OUT" -maxdepth 1 -name '*.iso' -print -quit)"
[[ -n "$ISO_SOURCE" ]] || { echo "mkarchiso produced no ISO" >&2; exit 1; }

log "Copying artifacts"
cp -f "$ISO_SOURCE" "$DIST/dimos.iso"
# The archiso image is isohybrid: the same bytes boot from a CD/DVD *and*
# from a USB stick or a raw disk, so dimos.img is that same image.
cp -f "$ISO_SOURCE" "$DIST/dimos.img"

printf '\n  dist/dimos.iso   %s\n' "$(du -h "$DIST/dimos.iso" | cut -f1)"
printf '  dist/dimos.img   %s  (dd to a USB stick)\n' "$(du -h "$DIST/dimos.img" | cut -f1)"
printf '\n  test it: qemu-system-x86_64 -m 2048 -enable-kvm -cdrom dist/dimos.iso\n'
