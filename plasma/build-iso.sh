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

# Maximum squashfs compression: xz preset 9 extreme with the x86 BCJ filter.
sed -i "s/^airootfs_image_tool_options=.*/airootfs_image_tool_options=('-comp' 'xz' '-Xbcj' 'x86' '-Xpreset' '9e' '-b' '1M' '-Xdict-size' '1M')/" "$PROFILE/profiledef.sh"
cp -r "$HERE/archiso/airootfs/." "$PROFILE/airootfs/"

# Give the image its own name in the ISO metadata.
sed -i 's/^iso_name=.*/iso_name="dimos-plasma"/' "$PROFILE/profiledef.sh"
sed -i 's/^iso_application=.*/iso_application="DimOS Plasma 6.8 live"/' "$PROFILE/profiledef.sh"

mkarchiso -v -w "$WORK/iso" -o "$OUT" "$PROFILE"
echo "ISO written to $OUT"
