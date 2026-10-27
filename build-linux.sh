#!/usr/bin/env bash

# Build the DimOS images: bootloader plus the graphical C kernel.
#
# The primary artifact is dimos.hdd, a hard disk image: the bootable FAT12
# volume occupies the first 1.44 MB and the rest is zero padding. It boots as
# an IDE/USB disk in QEMU and on real hardware, because the loader uses LBA
# and keeps the BIOS drive unit from DL.
# dimos.iso is the same system as a bootable El Torito CD for real PCs, and
# dimos.img is the raw 1.44M floppy volume.

set -Eeuo pipefail

readonly FLOPPY_SIZE_BYTES=1474560
readonly HDD_SIZE_BYTES=8388608
# The loader follows KERNEL.BIN's FAT12 chain from 2000:0000, so the only
# real ceiling is the preloaded data window at 0x30000: 126 sectors.
readonly MAX_KERNEL_LOADER_BYTES=64512
readonly BOOT_IMAGE="disk_img/dimos.img"
# FAT12 image + zero padding: boots as an IDE/USB hard disk in QEMU and
# on real PCs.
readonly HDD_IMAGE="disk_img/dimos.hdd"
# Kept as a blank compatibility disk for existing release automation.
readonly SECOND_FLOPPY_IMAGE="disk_img/FLOPPY2.img"
readonly ISO_IMAGE="disk_img/dimos.iso"
readonly IMAGE_CHECKER="bin/dimos-image-check"
readonly KERNEL_ENTRY_OBJECT="bin/kernel-entry.o"
readonly KERNEL_ELF="bin/KERNEL.ELF"
readonly KERNEL_MAP="bin/KERNEL.MAP"
# The compiler flags and compiler that produced bin/*.o.
readonly KERNEL_STAMP="bin/.kernel-flags"

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
  -j N, --jobs N         Compile N files at once (default: one per core)
  --quiet                Print only errors
  --no-boot-recompile    Reuse bin/BOOT.BIN
  --no-kernel-recompile  Reuse bin/KERNEL.BIN
  --no-iso               Do not build the El Torito ISO (HDD image still built)
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
        -j|--jobs)
            JOBS="${2:-}"
            [[ -n "$JOBS" ]] || fail "-j wants a number"
            shift
            ;;
        -j*) JOBS="${argument#-j}" ;;
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

# How many compiler jobs to run at once. The kernel is twenty independent
# translation units, so this is free speed: 1.9 s becomes 1.1 s on two cores
# and half a second on a laptop. One job keeps the output readable.
jobs="${JOBS:-0}"
if [[ "$jobs" == "0" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "JOBS must be a positive number"
# `wait -n` needs bash 4.3; older shells simply compile one file at a time.
if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
    jobs=1
fi

build_kernel() {
    local compiler=${CC:-gcc}
    local linker=${LD:-ld}
    local object_copy=${OBJCOPY:-objcopy}
    local source
    local objects=()
    local flags=(-m32 -march=i686 -std=c11 -Os ${KERNEL_CFLAGS:-}
                 -Wall -Wextra -Wpedantic -Werror
                 -ffreestanding -fno-builtin -fno-pic -fno-pie
                 -ffunction-sections -fdata-sections
                 -fno-stack-protector -fno-asynchronous-unwind-tables
                 -fno-unwind-tables -mno-red-zone -mgeneral-regs-only)

    # Only what changed is rebuilt. A new compiler or new flags invalidate
    # every object: an object is not a portable thing across -D changes.
    # This has to happen before the entry object is assembled, because the
    # invalidation sweeps bin/*.o.
    newest_header=""
    for header in src/kernel/*.h; do
        [[ -f "$header" ]] || continue
        if [[ -z "$newest_header" || "$header" -nt "$newest_header" ]]; then
            newest_header="$header"
        fi
    done
    stamp="$KERNEL_STAMP"
    stamp_now="$("$compiler" --version 2>/dev/null | head -1) | ${flags[*]}"
    if [[ ! -f "$stamp" || "$(cat "$stamp")" != "$stamp_now" ]]; then
        rm -f bin/*.o
        printf '%s\n' "$stamp_now" > "$stamp"
    fi

    log_info "Assembling BIOS-to-32-bit protected-mode entry"
    nasm -f elf32 ${KERNEL_ASMFLAGS:-} src/kernel/kernel.asm -o "$KERNEL_ENTRY_OBJECT"

    pending=()
    for source in src/kernel/*.c; do
        object="bin/$(basename "${source%.c}").o"
        objects+=("$object")
        if [[ -f "$object" && "$object" -nt "$source" &&
              ( -z "$newest_header" || "$object" -nt "$newest_header" ) ]]; then
            continue
        fi
        pending+=("$source")
    done

    if (( ${#pending[@]} == 0 )); then
        log_ok "All ${#objects[@]} i686 C files are up to date"
    else
        log_info "Compiling ${#pending[@]} of ${#objects[@]} i686 C files with $jobs job(s)"
        started=$(date +%s%N)
        running=0
        failed=0
        if (( jobs == 1 )); then
            # One job: the compiler's own messages stay in order.
            for source in "${pending[@]}"; do
                "$compiler" "${flags[@]}" -c "$source" \
                    -o "bin/$(basename "${source%.c}").o" || failed=1
            done
        else
            for source in "${pending[@]}"; do
                "$compiler" "${flags[@]}" -c "$source" \
                    -o "bin/$(basename "${source%.c}").o" &
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
        fi
        (( failed == 0 )) || fail "A kernel file did not compile"
        log_ok "Compiled ${#pending[@]} i686 C files in $(( ($(date +%s%N) - started) / 1000000 )) ms"
    fi

    log_info "Linking flat 32-bit protected-mode kernel"
    "$linker" -m elf_i386 --build-id=none -nostdlib --gc-sections \
        -T src/kernel/linker.ld -Map="$KERNEL_MAP" \
        "$KERNEL_ENTRY_OBJECT" "${objects[@]}" -o "$KERNEL_ELF"
    "$object_copy" -O binary "$KERNEL_ELF" bin/KERNEL.BIN
}

build_image_checker() {
    local compiler=${CC:-gcc}
    if [[ ! -x "$IMAGE_CHECKER" || tools/image_check.c -nt "$IMAGE_CHECKER" ]]; then
        log_info "Compiling image checker"
        "$compiler" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
            tools/image_check.c -o "$IMAGE_CHECKER"
    fi
}

create_hdd_image() {
    # The bootloader reads with LBA (int 13h AH=42h) and keeps the BIOS boot
    # unit in DL, so the same FAT12 volume boots unchanged as drive 0x80.
    # Keep the volume in the first 1.44 MB so the fixed BPB/FAT/root layout
    # still matches, and pad the rest with zeroes so ATA/SeaBIOS sees an
    # ordinary hard disk instead of a floppy.
    cp "$BOOT_IMAGE" "$HDD_IMAGE"
    truncate -s "$HDD_SIZE_BYTES" "$HDD_IMAGE"
    log_ok "Created $HDD_IMAGE ($(file_size "$HDD_IMAGE") bytes)"
}

create_iso_image() {
    local staging_directory="disk_img/iso-root"
    rm -rf "$staging_directory"
    mkdir -p "$staging_directory"
    cp "$BOOT_IMAGE" "$staging_directory/dimos.img"

    # Floppy-emulation El Torito: SeaBIOS maps this 1.44M image as
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

for command in nasm mkfs.vfat mcopy mdir truncate; do
    require_command "$command"
done
require_command "${CC:-gcc}"
require_command "${LD:-ld}"
require_command "${OBJCOPY:-objcopy}"
(( ! BUILD_ISO )) || require_command xorriso

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
truncate -s "$FLOPPY_SIZE_BYTES" "$BOOT_IMAGE"
mkfs.vfat -F 12 -n DIMOS "$BOOT_IMAGE" >/dev/null

# The old workflow publishes this name; it is intentionally an empty FAT12 disk.
truncate -s "$FLOPPY_SIZE_BYTES" "$SECOND_FLOPPY_IMAGE"
mkfs.vfat -F 12 -n EMPTY "$SECOND_FLOPPY_IMAGE" >/dev/null

dd if=bin/BOOT.BIN of="$BOOT_IMAGE" conv=notrunc status=none
mcopy -i "$BOOT_IMAGE" bin/KERNEL.BIN ::/

# The system font: any TrueType file dropped into fonts/font.ttf ships on
# the disk as FONT.TTF and the kernel rasterizes it at boot (font_ttf.c).
# It must fit inside the 256 data sectors the bootloader preloads,
# together with the kernel that sits in front of it.
if [[ -f fonts/font.ttf ]]; then
    font_size=$(file_size fonts/font.ttf)
    kernel_sectors=$(( (kernel_size + 511) / 512 ))
    font_limit=$(( (256 - kernel_sectors) * 512 ))
    (( font_size <= font_limit )) || fail \
"fonts/font.ttf is too big: $font_size bytes, but only $font_limit fit into
the preloaded disk window next to the kernel. Subset the font to ASCII, e.g.:
  pyftsubset yourfont.ttf --unicodes=U+0020-007E --no-hinting --output-file=fonts/font.ttf"
    mcopy -i "$BOOT_IMAGE" fonts/font.ttf ::/FONT.TTF
    log_ok "System font: fonts/font.ttf ($font_size bytes) -> FONT.TTF"
else
    log_info "fonts/font.ttf not found -- the kernel will use the BIOS font"
fi

# Ship a small ordinary file so the file manager has a real FAT12 file to inspect.
if [[ -d files ]]; then
    for user_file in files/*; do
        [[ -f "$user_file" ]] || continue
        mcopy -i "$BOOT_IMAGE" "$user_file" ::/
    done
fi

create_hdd_image

if (( ! QUIET )); then
    printf '\nDisk contents (kernel plus user files):\n'
    mdir -i "$BOOT_IMAGE" ::/
fi

if (( BUILD_ISO )); then
    create_iso_image
fi

checker_arguments=(bin/BOOT.BIN bin/KERNEL.BIN "$BOOT_IMAGE" "$HDD_IMAGE")
(( ! BUILD_ISO )) || checker_arguments+=("$ISO_IMAGE")
"$IMAGE_CHECKER" "${checker_arguments[@]}"

log_ok "Floppy image: $BOOT_IMAGE"
log_ok "Hard disk image: $HDD_IMAGE"
(( ! BUILD_ISO )) || log_ok "ISO image: $ISO_IMAGE"
