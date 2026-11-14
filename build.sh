#!/usr/bin/env bash
#
# Build DimOS.
#
#   ./build.sh            the desktop program  -> bin/dimos
#   ./build.sh run        build and start it
#   ./build.sh iso        live ISO + disk image -> dist/dimos.iso, dist/dimos.img
#   ./build.sh clean      remove build/, bin/ and dist/
#
set -Eeuo pipefail

cd "$(dirname "$0")"

target="${1:-app}"

case "$target" in
    iso)
        exec ./iso/build-iso.sh
        ;;
    clean)
        make clean
        ;;
    app|run)
        if [ "$(uname -s)" = "Linux" ]; then
            if [ ! -f /usr/include/X11/Xlib.h ] && ! pkg-config --exists x11 2>/dev/null; then
                printf 'note: install the window dev packages, e.g.\n'
                printf '      sudo apt install build-essential libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev\n'
            fi
        fi
        if [ "$target" = "run" ]; then
            make app
            exec ./bin/dimos
        fi
        make app
        printf '\nbuilt bin/dimos\n  ./bin/dimos               -- desktop app\n'
        printf '  ./bin/dimos --fullscreen  -- for a live system\n'
        printf '  ./build.sh iso            -- bootable ISO + IMG (needs Arch or Docker)\n'
        ;;
    *)
        printf 'usage: %s [app|run|iso|clean]\n' "$0" >&2
        exit 2
        ;;
esac
