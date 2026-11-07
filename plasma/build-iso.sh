#!/usr/bin/env bash
# Builds the DimOS live ISO (Snake only) with archiso. Run as root on Arch Linux.
# The snake binary must already be in archiso/airootfs/usr/local/bin/snake
# (the CI workflow compiles it from snake/snake.c).
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
[[ -x "$HERE/archiso/airootfs/usr/local/bin/snake" ]] || { echo "Compile snake first (see README)" >&2; exit 1; }

rm -rf "$WORK" "$OUT"
mkdir -p "$PROFILE" "$OUT"
cp -r "$RELENG"/. "$PROFILE"/

# Our package list replaces releng's; our overlay goes on top of airootfs.
cp "$HERE/archiso/packages.x86_64" "$PROFILE/packages.x86_64"
cp -r "$HERE/archiso/airootfs/." "$PROFILE/airootfs/"

sed -i 's/^iso_name=.*/iso_name="dimos-snake"/' "$PROFILE/profiledef.sh"
sed -i 's/^iso_application=.*/iso_application="DimOS Snake live"/' "$PROFILE/profiledef.sh"

# Fast squashfs compression that still gives a small image.
sed -i "s/^airootfs_image_tool_options=.*/airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')/" "$PROFILE/profiledef.sh"

# Kernel console: full messages on screen and COM1 (for diagnostics).
find "$PROFILE/syslinux" -type f -name '*.cfg' \
    -exec sed -i -E 's/^([[:space:]]*APPEND.*)$/\1 nomodeset console=tty0 console=ttyS0,115200/' {} +
find "$PROFILE/efiboot" -type f -name '*.conf' \
    -exec sed -i -E 's/^(options .*)$/\1 nomodeset console=tty0 console=ttyS0,115200/' {} +

# No boot menu: boot the default entry straight away.
find "$PROFILE/syslinux" -type f -name '*.cfg' \
    -exec sed -i -E 's/^[[:space:]]*TIMEOUT[[:space:]].*$/TIMEOUT 1/; s/^[[:space:]]*PROMPT[[:space:]].*$/PROMPT 0/' {} +
if [[ -f "$PROFILE/efiboot/loader/loader.conf" ]]; then
    sed -i -E 's/^[[:space:]]*timeout[[:space:]].*$/timeout 0/' "$PROFILE/efiboot/loader/loader.conf"
    grep -q '^timeout' "$PROFILE/efiboot/loader/loader.conf" || echo 'timeout 0' >> "$PROFILE/efiboot/loader/loader.conf"
fi

mkarchiso -v -w "$WORK/iso" -o "$OUT" "$PROFILE"
echo "ISO written to $OUT"
