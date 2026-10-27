#!/usr/bin/env bash

# Build DimOS Anywhere: the same kernel, the same shell and the same nine
# applications, compiled as an ordinary program for a PC, a tablet or a
# phone -- plus Lua 5.4 for headless applications.
#
# Supported hosts, on purpose:
#   Linux x86-64 / ARM64      gcc or clang
#   Windows x86-64            MinGW-w64 (gcc) or clang with MSVC headers
#   Android ARM64 / x86-64    Termux clang, or the NDK with CC=<triple>-clang
#
# macOS is not a target: DimOS runs on Android and PC only. The script
# refuses Darwin instead of producing a build nobody promised to keep
# working.
#
# Usage:
#   ./build-anywhere.sh            # build bin/dimos-anywhere
#   ./build-anywhere.sh --clean    # remove bin/anywhere first
#   CC=clang ./build-anywhere.sh   # pick a compiler
#   NATIVE_FLAGS="-march=native" ./build-anywhere.sh

set -Eeuo pipefail

readonly NAME="dimos-anywhere"
readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD="$HERE/bin/anywhere"
readonly KERNEL="$HERE/src/kernel"
readonly HOST="$HERE/src/host"
readonly LUA="$HERE/third_party/lua"
readonly WARNINGS=(-Wall -Wextra -Wpedantic -Werror)

cd "$HERE"

log() { printf '[ INFO ] %s\n' "$1"; }
ok() { printf '[  OK  ] %s\n' "$1"; }
fail() { printf '[ FAILED ] %s\n' "$1" >&2; exit 1; }

refuse_macos() {
    [[ "$(uname -s)" != "Darwin" ]] || fail \
"DimOS targets Android and PC. macOS is deliberately not supported: use a
Linux PC, a Windows PC, or run the runtime on an Android phone or tablet
(for example in Termux) and open the page in that device's browser."
}

pick_compiler() {
    if [[ -n "${CC:-}" ]]; then
        return
    fi
    if command -v cc >/dev/null 2>&1; then
        CC=cc
    elif command -v gcc >/dev/null 2>&1; then
        CC=gcc
    elif command -v clang >/dev/null 2>&1; then
        CC=clang
    else
        fail "no C compiler found (set CC=...)"
    fi
}

target_kind() {
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) echo windows ;;
        Linux*) echo linux ;;
        *) echo other ;;
    esac
}

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$BUILD"
    ok "removed $BUILD"
    exit 0
fi

refuse_macos
pick_compiler
require() { command -v "$1" >/dev/null 2>&1 || fail "required tool missing: $1"; }
require "$CC"
require "${LD:-ld}"
require "${OBJCOPY:-objcopy}"

mkdir -p "$BUILD/objects"
KIND="$(target_kind)"
BINARY="$HERE/bin/$NAME"
[[ "$KIND" != "windows" ]] || BINARY="$BINARY.exe"

# The kernel sources are freestanding C11 with no libc at all; on a host they
# still compile as they are. DIMOS_SANDBOX removes the interrupt and firmware
# paths, DIMOS_HOST switches the fixed physical addresses to host memory.
readonly COMMON=(-std=c11 -O2 -DNDEBUG -DDIMOS_HOST -DDIMOS_SANDBOX
                 -I"$KERNEL" -I"$HOST" -I"$LUA")
POSIX_DEFINE=()
[[ "$(target_kind)" == "linux" ]] && POSIX_DEFINE=(-D_POSIX_C_SOURCE=200809L)
KERNEL_FLAGS=("${COMMON[@]}" "${WARNINGS[@]}" -Wno-unused-parameter)
HOST_POSIX=("${POSIX_DEFINE[@]}")
# -Wno-overlength-strings: the browser page is one long string constant.
# C99 only requires 4095 characters; every compiler DimOS supports is happy
# with the ~6 KiB of HTML below.
HOST_FLAGS=("${COMMON[@]}" "${WARNINGS[@]}" -Wno-overlength-strings
            "${HOST_POSIX[@]}")
LUA_FLAGS=(-std=c99 -O2 -DNDEBUG -I"$LUA" -w)

objects=()

compile() {
    local source="$1" flags_name="$2" extra="${3:-}"
    local object="$BUILD/objects/$(basename "${source%.c}").o"
    local -a flags

    case "$flags_name" in
        kernel) flags=("${KERNEL_FLAGS[@]}") ;;
        host) flags=("${HOST_FLAGS[@]}") ;;
        lua) flags=("${LUA_FLAGS[@]}") ;;
    esac
    # shellcheck disable=SC2086
    "$CC" "${flags[@]}" $extra -c "$source" -o "$object"
    objects+=("$object")
}

log "compiling the DimOS kernel sources for the host"
kernel_sources=()
for source in "$KERNEL"/*.c; do
    name="$(basename "$source")"
    extra=""
    if [[ "$name" == "font_ttf.c" ]]; then
        # The rasterizer is built the way tools/font_preview.c builds it;
        # platform.c supplies the work area and the FONT.TTF file.
        extra="-DDIMOS_HOST_TEST"
    fi
    compile "$source" kernel "$extra"
    kernel_sources+=("$name")
done
ok "${#kernel_sources[@]} kernel files, including the shell and nine apps"

log "compiling the hosted runtime and the web front end"
for source in "$HOST"/*.c; do
    compile "$source" host
done
ok "platform, framebuffer, PNG, HTTP and the Lua bridge"

log "compiling Lua 5.4.7 (core, lauxlib and six standard libraries)"
lua_count=0
for source in "$LUA"/*.c; do
    compile "$source" lua
    lua_count=$((lua_count + 1))
done
ok "$lua_count Lua files (io, os, debug and the package loader are left out)"

log "linking $BINARY"
link_flags=(-o "$BINARY")
if [[ "$KIND" == "linux" ]]; then
    link_flags+=(-lm)
elif [[ "$KIND" == "windows" ]]; then
    link_flags+=(-lws2_32)
fi
"$CC" "${objects[@]}" "${link_flags[@]}"
ok "built $(basename "$BINARY") ($(wc -c < "$BINARY" | tr -d '[:space:]') bytes)"

"$BINARY" --version >/dev/null || fail "the binary does not start"
ok "$("$BINARY" --version)"

cat <<EOF

Try it:
  $BINARY                     # phone shell at http://localhost:8080
  $BINARY self-test           # drive the real shell, write screenshots
  $BINARY run HELLO.LUA       # a Lua application, no GUI
  $BINARY lua -e 'print(dim.platform)'
EOF
