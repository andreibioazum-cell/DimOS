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
#   ./build-anywhere.sh            # build, reusing what did not change
#   ./build-anywhere.sh -j 4       # four compiler jobs (default: one per core)
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

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            rm -rf "$BUILD"
            ok "removed $BUILD"
            exit 0
            ;;
        -j|--jobs)
            [[ $# -ge 2 ]] || fail "-j wants a number"
            JOBS="$2"
            shift 2
            ;;
        -j*)
            JOBS="${1#-j}"
            shift
            ;;
        --quick)
            # Iterating on the UI does not need a tuned binary: -O0 compiles
            # the kernel in 0.7 s instead of 2.5 s on the same machine.
            QUICK=1
            shift
            ;;
        --stats)
            STATS=1
            shift
            ;;
        *)
            fail "unknown option: $1 (try -j N, --quick, --stats or --clean)"
            ;;
    esac
done

refuse_macos
pick_compiler
require() { command -v "$1" >/dev/null 2>&1 || fail "required tool missing: $1"; }
require "$CC"
require "${LD:-ld}"
require "${OBJCOPY:-objcopy}"

# ccache turns a rebuild after "rm -rf bin" or a branch switch into a copy
# from its cache. It is optional: without it nothing changes except speed.
compiler=("$CC")
if [[ "${USE_CCACHE:-1}" != "0" ]] && command -v ccache >/dev/null 2>&1; then
    compiler=(ccache "$CC")
fi

mkdir -p "$BUILD/objects"
KIND="$(target_kind)"
BINARY="$HERE/bin/$NAME"
[[ "$KIND" != "windows" ]] || BINARY="$BINARY.exe"

# The kernel sources are freestanding C11 with no libc at all; on a host they
# still compile as they are. DIMOS_SANDBOX removes the interrupt and firmware
# paths, DIMOS_HOST switches the fixed physical addresses to host memory.
# The kernel's frame loop is real work, so it keeps -O2. Lua runs headless
# applications, never the compositor, and is by far the most expensive part
# to optimize (27 files, ~4.7 s at -O2 against ~2.8 s at -O1), so it gets
# -O1 unless asked otherwise. --quick turns both down for quick iterations.
[[ "${QUICK:-0}" == "1" ]] && export OPT_KERNEL="-O0" OPT_LUA="-O0"
readonly OPT_KERNEL="${OPT_KERNEL:--O2}"
readonly OPT_LUA="${OPT_LUA:--O1}"
readonly COMMON=(-std=c11 "$OPT_KERNEL" -DNDEBUG -DDIMOS_HOST -DDIMOS_SANDBOX
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
LUA_FLAGS=(-std=c99 "$OPT_LUA" -DNDEBUG -I"$LUA" -w)

# --- the compilation queue -------------------------------------------
#
# 52 translation units at -O2 take about eight seconds one after another,
# which is slow every time you touch one line. So the script keeps the
# objects between runs and only rebuilds what actually changed: a source
# file, any header it may include, or the compiler and its flags. The rest
# is compiled in parallel, one job per core by default.
#
#   ./build-anywhere.sh -j 4       four jobs
#   JOBS=1 ./build-anywhere.sh     one job, readable compiler errors
#   ./build-anywhere.sh --clean    forget everything

jobs="${JOBS:-0}"
if [[ "$jobs" == "0" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive number"

declare -a job_source=() job_kind=() job_extra=() job_object=() objects=()

# The newest header in the three include directories, found once. Comparing
# every object against it is a plain -nt test instead of a find per file,
# which matters on phones and on slow file systems.
newest_header=""
for header in "$KERNEL"/*.h "$HOST"/*.h "$LUA"/*.h; do
    [[ -f "$header" ]] || continue
    if [[ -z "$newest_header" || "$header" -nt "$newest_header" ]]; then
        newest_header="$header"
    fi
done

is_current() {
    local object="$1" source="$2"

    [[ -f "$object" && "$object" -nt "$source" ]] || return 1
    if [[ -n "$newest_header" && "$newest_header" -nt "$object" ]]; then
        return 1
    fi
    return 0
}

# A change of compiler or of flags invalidates every object: an object
# built with a different -D or C standard cannot be trusted.
stamp_file="$BUILD/flags.stamp"
stamp_now="$("${compiler[@]}" --version 2>/dev/null | head -1) | ${COMMON[*]} | ${WARNINGS[*]} | ${POSIX_DEFINE[*]} | ${LUA_FLAGS[*]}"
if [[ ! -f "$stamp_file" || "$(cat "$stamp_file")" != "$stamp_now" ]]; then
    rm -rf "$BUILD/objects"
    mkdir -p "$BUILD/objects"
    printf '%s\n' "$stamp_now" > "$stamp_file"
fi

queue() {
    local source="$1" kind="$2" extra="${3:-}"
    local object="$BUILD/objects/$(basename "${source%.c}").o"

    job_source+=("$source")
    job_kind+=("$kind")
    job_extra+=("$extra")
    job_object+=("$object")
    objects+=("$object")
}

compile_one() {
    local source="$1" kind="$2" extra="$3" object="$4"
    local -a flags

    case "$kind" in
        kernel) flags=("${KERNEL_FLAGS[@]}") ;;
        host) flags=("${HOST_FLAGS[@]}") ;;
        lua) flags=("${LUA_FLAGS[@]}") ;;
    esac
    if [[ -n "$extra" ]]; then
        flags+=("$extra")
    fi
    "${compiler[@]}" "${flags[@]}" -c "$source" -o "$object"
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
    queue "$source" kernel "$extra"
    kernel_sources+=("$name")
done

log "compiling the hosted runtime and the web front end"
for source in "$HOST"/*.c; do
    queue "$source" host
done

log "compiling Lua 5.4.7 (core, lauxlib and six standard libraries)"
lua_count=0
for source in "$LUA"/*.c; do
    queue "$source" lua
    lua_count=$((lua_count + 1))
done

# Everything that is not up to date goes into the pool.
pending_source=() pending_kind=() pending_extra=() pending_object=()
reused=0
for index in "${!job_source[@]}"; do
    object="${job_object[index]}"
    if [[ -f "$object" && "$object" -nt "${job_source[index]}" &&
          -z "$(headers_newer_than "$object")" ]]; then
        reused=$((reused + 1))
        continue
    fi
    pending_source+=("${job_source[index]}")
    pending_kind+=("${job_kind[index]}")
    pending_extra+=("${job_extra[index]}")
    pending_object+=("$object")
done

started_ns=$(date +%s%N)
if (( ${#pending_source[@]} == 0 )); then
    ok "all ${#objects[@]} files are up to date, nothing to compile"
else
    log "compiling ${#pending_source[@]} of ${#objects[@]} files with $jobs job(s)"
    running=0
    failed=0
    for index in "${!pending_source[@]}"; do
        ( compile_one "${pending_source[index]}" "${pending_kind[index]}" \
                      "${pending_extra[index]}" "${pending_object[index]}" ) &
        running=$((running + 1))
        if (( running >= jobs )); then
            wait -n || failed=1
            running=$((running - 1))
        fi
    done
    while (( running > 0 )); do
        wait -n || failed=1
        running=$((running - 1))
    done
    (( failed == 0 )) || fail "compilation failed"
    elapsed=$(( ($(date +%s%N) - started_ns) / 1000000 ))
    if (( ${#pending_source[@]} == 1 )); then
        ok "compiled 1 file in ${elapsed}ms ($reused reused)"
    else
        ok "compiled ${#pending_source[@]} files in ${elapsed}ms ($reused reused)"
    fi
    if [[ "${STATS:-0}" == "1" ]]; then
        for group in kernel host lua; do
            count=0
            for index in "${!pending_kind[@]}"; do
                [[ "${pending_kind[index]}" == "$group" ]] && count=$((count + 1))
            done
            printf '         %-7s %2d files to build\n' "$group" "$count"
        done
    fi
fi
ok "${#kernel_sources[@]} kernel files, including the shell and nine apps"
ok "platform, framebuffer, PNG, HTTP and the Lua bridge"
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
