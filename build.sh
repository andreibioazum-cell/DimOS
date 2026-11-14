#!/usr/bin/env bash
#
# Build Sokol OS. The same script works on Linux and macOS; on Windows use
# the command in README.md (there is nothing to configure, just one .c file
# that includes sokol).
#
#   ./build.sh              release build -> bin/sokol-os
#   ./build.sh frames       render the PNG screenshots -> build/frames
#   ./build.sh run          build and start
#
set -Eeuo pipefail

cd "$(dirname "$0")"

target="${1:-app}"

case "$(uname -s)" in
    Linux)
        for tool in cc make; do
            command -v "$tool" >/dev/null 2>&1 || {
                printf 'missing build tool: %s\n' "$tool" >&2
                exit 1
            }
        done
        if [ ! -f /usr/include/X11/Xlib.h ] && ! pkg-config --exists x11 2>/dev/null; then
            printf 'note: install the window dev packages, e.g.\n'
            printf '      sudo apt install build-essential libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev\n'
        fi
        ;;
    Darwin)
        printf 'macOS: using the bundled Cocoa/OpenGL frameworks\n'
        ;;
    *)
        printf 'unknown platform %s, trying make anyway\n' "$(uname -s)"
        ;;
esac

case "$target" in
    app|all)
        make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" app
        printf '\nbuilt %s\nrun it with: ./bin/sokol-os\n' "bin/sokol-os"
        ;;
    run)
        make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" app
        exec ./bin/sokol-os
        ;;
    frames|check)
        make "$target"
        ;;
    clean)
        make clean
        ;;
    *)
        printf 'usage: %s [app|run|frames|check|clean]\n' "$0" >&2
        exit 2
        ;;
esac
