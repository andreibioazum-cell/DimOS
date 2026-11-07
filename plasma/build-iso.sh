#!/usr/bin/env bash
# Builds the DimOS Plasma 6.8 live ISO with archiso (run on Arch Linux).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/work"
OUT="$HERE/out"
PROFILE="$WORK/profile"
RELENG=/usr/share/archiso/configs/releng

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo ./build-iso.sh" >&2
    exit 1
fi

command -v mkarchiso >/dev/null || { echo "Install archiso first: pacman -S archiso" >&2; exit 1; }

# Fail early if the Plasma version in the repositories is not 6.8.x.
PLASMA_VER="$(pacman -Si plasma-desktop 2>/dev/null | awk -F': ' '/^Version/{print $2}' | cut -d- -f1)"
if [[ "$PLASMA_VER" != 6.8.* ]]; then
    echo "plasma-desktop in repositories is '${PLASMA_VER:-unknown}', expected 6.8.x" >&2
    echo "Run 'pacman -Sy' and try again." >&2
    exit 1
fi

rm -rf "$WORK" "$OUT"
mkdir -p "$PROFILE" "$OUT"
cp -r "$RELENG"/. "$PROFILE"/

# Use our own package list only: releng's list pulls in dozens of rescue tools
# (clonezilla, ddrescue, cloud-init, ...) that make the ISO slow to build and big.
# The overlay goes on top of airootfs.
cp "$HERE/archiso/packages.x86_64" "$PROFILE/packages.x86_64"

# squashfs compression: zstd level 19 (multithreaded), smallest size we can build in time.
sed -i "s/^airootfs_image_tool_options=.*/airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')/" "$PROFILE/profiledef.sh"
cp -r "$HERE/archiso/airootfs/." "$PROFILE/airootfs/"

# Give the image its own name in the ISO metadata.
sed -i 's/^iso_name=.*/iso_name="dimos-plasma"/' "$PROFILE/profiledef.sh"
sed -i 's/^iso_application=.*/iso_application="DimOS Plasma 6.8 live"/' "$PROFILE/profiledef.sh"

# Boot menus: show our name instead of the stock Arch branding.
find "$PROFILE/syslinux" "$PROFILE/efiboot" -type f \( -name '*.cfg' -o -name '*.conf' \) \
    -exec sed -i 's/Arch Linux/DimOS Plasma/g' {} +

# Kernel: nomodeset (generic framebuffer); full kernel messages on screen and COM1
# so the place where boot stops is visible.
find "$PROFILE/syslinux" -type f -name '*.cfg' \
    -exec sed -i -E 's/^([[:space:]]*APPEND.*)$/\1 nomodeset loglevel=7 console=tty0 console=ttyS0,115200/' {} +
find "$PROFILE/efiboot" -type f -name '*.conf' \
    -exec sed -i -E 's/^(options .*)$/\1 nomodeset loglevel=7 console=tty0 console=ttyS0,115200/' {} +

# No boot menu: boot the default entry straight away (no waiting on a choice).
find "$PROFILE/syslinux" -type f -name '*.cfg' \
    -exec sed -i -E 's/^[[:space:]]*TIMEOUT[[:space:]].*$/TIMEOUT 1/; s/^[[:space:]]*PROMPT[[:space:]].*$/PROMPT 0/' {} +
if [[ -f "$PROFILE/efiboot/loader/loader.conf" ]]; then
    sed -i -E 's/^[[:space:]]*timeout[[:space:]].*$/timeout 0/' "$PROFILE/efiboot/loader/loader.conf"
    grep -q '^timeout' "$PROFILE/efiboot/loader/loader.conf" || echo 'timeout 0' >> "$PROFILE/efiboot/loader/loader.conf"
fi

mkarchiso -v -w "$WORK/iso" -o "$OUT" "$PROFILE"
echo "ISO written to $OUT"
