#!/usr/bin/env bash

# Build the DimOS workshop image: bootloader + graphical kernel.

set -Eeuo pipefail

readonly FLOPPY_SIZE_BYTES=1474560
readonly MAX_KERNEL_LOADER_BYTES=43008
readonly BOOT_IMAGE="disk_img/dimos.img"
# Kept as a blank compatibility disk for existing release automation.
readonly SECOND_FLOPPY_IMAGE="disk_img/FLOPPY2.img"
readonly ISO_IMAGE="disk_img/dimos.iso"
readonly IMAGE_CHECKER="bin/dimos-image-check"
readonly EMBEDDED_IMAGE_JS="web/dimos-image.js"
readonly KERNEL_ENTRY_OBJECT="bin/kernel-entry.o"
readonly KERNEL_C_OBJECT="bin/kernel-c.o"
readonly KERNEL_ELF="bin/KERNEL.ELF"
readonly KERNEL_MAP="bin/KERNEL.MAP"

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

assemble_bootloader() {
    log_info "Assembling src/bootloader/boot.asm -> bin/BOOT.BIN"
    nasm -f bin src/bootloader/boot.asm -o bin/BOOT.BIN
}

build_kernel() {
    local compiler=${CC:-gcc}
    local linker=${LD:-ld}
    local object_copy=${OBJCOPY:-objcopy}

    log_info "Assembling protected-mode entry"
    nasm -f elf32 src/kernel/kernel.asm -o "$KERNEL_ENTRY_OBJECT"

    log_info "Compiling C kernel"
    "$compiler" \
        -m32 -march=i386 -std=c11 -Os \
        -Wall -Wextra -Wpedantic -Werror \
        -ffreestanding -fno-builtin -fno-pic -fno-pie \
        -fno-stack-protector -fno-asynchronous-unwind-tables \
        -fno-unwind-tables -mno-mmx -mno-sse -mno-sse2 \
        -I src/kernel \
        -c src/kernel/kernel.c -o "$KERNEL_C_OBJECT"

    log_info "Linking flat kernel"
    "$linker" -m elf_i386 --build-id=none -nostdlib \
        -T src/kernel/linker.ld -Map="$KERNEL_MAP" \
        "$KERNEL_ENTRY_OBJECT" "$KERNEL_C_OBJECT" -o "$KERNEL_ELF"
    "$object_copy" -O binary "$KERNEL_ELF" bin/KERNEL.BIN
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

    # Floppy-emulation El Torito: SeaBIOS in v86 maps this 1.44M image as
    # drive 0x00. Do not pass -no-emul-boot or -boot-info-table — both break
    # the FAT12 boot sector that DimOS actually starts from.
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

for command in nasm gzip base64 sha256sum; do
    require_command "$command"
done
require_command "${CC:-gcc}"
require_command "${CXX:-g++}"
require_command "${LD:-ld}"
require_command "${OBJCOPY:-objcopy}"
require_command "${PYTHON:-python3}"
if (( BUILD_ISO )) && ! command -v xorriso >/dev/null 2>&1; then
    log_info "xorriso not found; building floppy only"
    BUILD_ISO=0
fi

mkdir -p bin disk_img
build_image_checker

if (( ! NO_BOOT_RECOMPILE )); then
    assemble_bootloader
else
    require_file bin/BOOT.BIN
fi

if (( ! NO_KERNEL_RECOMPILE )); then
    build_kernel
else
    require_file bin/KERNEL.BIN
fi

kernel_size=$(file_size bin/KERNEL.BIN)
(( kernel_size > 0 )) || fail "KERNEL.BIN is empty"
(( kernel_size <= MAX_KERNEL_LOADER_BYTES )) || \
    fail "KERNEL.BIN exceeds the bootloader limit ($kernel_size > $MAX_KERNEL_LOADER_BYTES)"
log_ok "Kernel size: $kernel_size bytes"

log_info "Creating FAT12 image"
fat_files=(bin/KERNEL.BIN)
if [[ -d files ]]; then
    for user_file in files/*; do
        [[ -f "$user_file" ]] || continue
        fat_files+=("$user_file")
    done
fi
"${PYTHON:-python3}" tools/make_fat12.py bin/BOOT.BIN "$BOOT_IMAGE" "${fat_files[@]}"

# Compatibility empty second floppy.
"${PYTHON:-python3}" -c 'from pathlib import Path; Path("disk_img/FLOPPY2.img").write_bytes(bytes(1474560))'

"$IMAGE_CHECKER" bin/BOOT.BIN bin/KERNEL.BIN "$BOOT_IMAGE"

(( ! BUILD_ISO )) || create_iso_image

# Keep the browser launcher self-contained: index.html boots this payload with
# no file picking, which is what makes "could not read the boot disk" possible.
log_info "Embedding the floppy image into $EMBEDDED_IMAGE_JS"
./tools/embed-image.sh "$BOOT_IMAGE" "$EMBEDDED_IMAGE_JS" >/dev/null
log_ok "Embedded launcher image: $EMBEDDED_IMAGE_JS"

log_ok "Floppy image: $BOOT_IMAGE"
(( ! BUILD_ISO )) || log_ok "ISO image: $ISO_IMAGE"
