#!/usr/bin/env bash

# Build the minimal DimOS image: bootloader + command line/Snake kernel.

set -Eeuo pipefail

readonly FLOPPY_SIZE_BYTES=1474560
readonly MAX_KERNEL_LOADER_BYTES=43008
readonly BOOT_IMAGE="disk_img/dimos.img"
# Kept as a blank compatibility disk for existing release automation.
readonly SECOND_FLOPPY_IMAGE="disk_img/FLOPPY2.img"
readonly ISO_IMAGE="disk_img/dimos.iso"
readonly WEB_ISO_IMAGE="web/dimos.iso"
readonly IMAGE_CHECKER="bin/dimos-image-check"

QUIET=0
NO_BOOT_RECOMPILE=0
NO_KERNEL_RECOMPILE=0
BUILD_ISO=1

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    RED=$'\033[31m'
    GREEN=$'\033[32m'
    CYAN=$'\033[36m'
    RESET=$'\033[0m'
else
    RED=''
    GREEN=''
    CYAN=''
    RESET=''
fi

usage() {
    cat <<'USAGE'
Usage: ./build-linux.sh [options]

Options:
  --quiet                Print only errors
  --no-boot-recompile    Reuse bin/BOOT.BIN
  --no-kernel-recompile  Reuse bin/KERNEL.BIN
  --no-iso               Build only the FAT12 floppy image
  -h, --help             Show this help
USAGE
}

log_info() {
    (( QUIET )) || printf '%s[ INFO ]%s %s\n' "$CYAN" "$RESET" "$1"
}

log_ok() {
    (( QUIET )) || printf '%s[  OK  ]%s %s\n' "$GREEN" "$RESET" "$1"
}

fail() {
    printf '%s[ FAILED ]%s %s\n' "$RED" "$RESET" "$1" >&2
    exit 1
}

on_error() {
    local status=$?
    printf '%s[ FAILED ]%s Build command failed at line %s (exit %s).\n' \
        "$RED" "$RESET" "${BASH_LINENO[0]}" "$status" >&2
    exit "$status"
}
trap on_error ERR

for argument in "$@"; do
    case "$argument" in
        --quiet|-quiet) QUIET=1 ;;
        --no-boot-recompile|-no-boot-recomp) NO_BOOT_RECOMPILE=1 ;;
        --no-kernel-recompile|-no-kernel-recomp) NO_KERNEL_RECOMPILE=1 ;;
        --no-iso) BUILD_ISO=0 ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            fail "Unknown option: $argument"
            ;;
    esac
done

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "Required command not found: $1"
}

require_file() {
    [[ -f "$1" ]] || fail "Required file not found: $1"
}

file_size() {
    wc -c < "$1" | tr -d '[:space:]'
}

assemble() {
    local source=$1
    local output=$2
    log_info "Assembling $source -> $output"
    nasm -f bin "$source" -o "$output"
}

build_image_checker() {
    local compiler=${CXX:-g++}
    if [[ ! -x "$IMAGE_CHECKER" || tools/image_inspector.cpp -nt "$IMAGE_CHECKER" ]]; then
        log_info "Compiling image checker"
        "$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
            tools/image_inspector.cpp -o "$IMAGE_CHECKER"
    fi
}

create_iso_image() {
    local staging_directory="disk_img/iso-root"
    rm -rf "$staging_directory"
    mkdir -p "$staging_directory"
    cp "$BOOT_IMAGE" "$staging_directory/dimos.img"

    xorriso -as mkisofs \
        -quiet \
        -V DIMOS \
        -b dimos.img \
        -c boot.cat \
        -o "$ISO_IMAGE" \
        "$staging_directory"

    rm -rf "$staging_directory"
    log_ok "Created $ISO_IMAGE ($(file_size "$ISO_IMAGE") bytes)"
}

for command in nasm mkfs.vfat mcopy mdir truncate; do
    require_command "$command"
done
require_command "${CXX:-g++}"
(( ! BUILD_ISO )) || require_command xorriso

mkdir -p bin disk_img
build_image_checker

if (( ! NO_BOOT_RECOMPILE )); then
    assemble src/bootloader/boot.asm bin/BOOT.BIN
else
    require_file bin/BOOT.BIN
fi

if (( ! NO_KERNEL_RECOMPILE )); then
    assemble src/kernel/kernel.asm bin/KERNEL.BIN
else
    require_file bin/KERNEL.BIN
fi

kernel_size=$(file_size bin/KERNEL.BIN)
(( kernel_size > 0 )) || fail "KERNEL.BIN is empty"
(( kernel_size <= MAX_KERNEL_LOADER_BYTES )) || \
    fail "KERNEL.BIN exceeds the bootloader limit ($kernel_size > $MAX_KERNEL_LOADER_BYTES)"
log_ok "Kernel size: $kernel_size bytes"

log_info "Creating FAT12 image"
truncate -s "$FLOPPY_SIZE_BYTES" "$BOOT_IMAGE"
mkfs.vfat -F 12 -n DIMOS "$BOOT_IMAGE" >/dev/null

# The old workflow publishes this name; it is intentionally an empty FAT12 disk.
truncate -s "$FLOPPY_SIZE_BYTES" "$SECOND_FLOPPY_IMAGE"
mkfs.vfat -F 12 -n EMPTY "$SECOND_FLOPPY_IMAGE" >/dev/null

dd if=bin/BOOT.BIN of="$BOOT_IMAGE" conv=notrunc status=none
mcopy -i "$BOOT_IMAGE" bin/KERNEL.BIN ::/

"$IMAGE_CHECKER" bin/BOOT.BIN bin/KERNEL.BIN "$BOOT_IMAGE"

if (( ! QUIET )); then
    printf '\nDisk contents (intentionally only the kernel):\n'
    mdir -i "$BOOT_IMAGE" ::/
fi

(( ! BUILD_ISO )) || create_iso_image

if (( BUILD_ISO )); then
    cp "$ISO_IMAGE" "$WEB_ISO_IMAGE"
    log_ok "Web ISO image: $WEB_ISO_IMAGE"
fi

log_ok "Floppy image: $BOOT_IMAGE"
(( ! BUILD_ISO )) || log_ok "ISO image: $ISO_IMAGE"
