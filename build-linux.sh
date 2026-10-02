#!/usr/bin/env bash

# Build the DimOS images: bootloader plus the graphical C kernel.
#
# The primary artifact is dimos.hdd, a hard disk image: the bootable FAT12
# volume occupies the first 1.44 MB and the rest is zero padding. It boots as
# an IDE/USB disk in QEMU, on the v86 website (hard disk slot) and on real
# hardware, because the loader uses LBA and keeps the BIOS drive unit from DL.
# dimos.iso is the same system as a bootable El Torito CD for real PCs, and
# dimos.img is the raw 1.44M floppy volume.

set -Eeuo pipefail

readonly FLOPPY_SIZE_BYTES=1474560
readonly HDD_SIZE_BYTES=8388608
# The loader follows KERNEL.BIN's FAT12 chain from 2000:0000, so the only
# real ceiling is the preloaded data window at 0x30000: 126 sectors.
readonly MAX_KERNEL_LOADER_BYTES=64512
readonly BOOT_IMAGE="disk_img/dimos.img"
# FAT12 image + zero padding: boots as an IDE/USB hard disk in QEMU, v86 and
# on real PCs.
readonly HDD_IMAGE="disk_img/dimos.hdd"
# Kept as a blank compatibility disk for existing release automation.
readonly SECOND_FLOPPY_IMAGE="disk_img/FLOPPY2.img"
readonly ISO_IMAGE="disk_img/dimos.iso"
readonly IMAGE_CHECKER="bin/dimos-image-check"
readonly KERNEL_ENTRY_OBJECT="bin/kernel-entry.o"
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
    local source
    local objects=()

    log_info "Assembling BIOS-to-x86-64 long-mode entry"
    nasm -f elf64 ${KERNEL_ASMFLAGS:-} src/kernel/kernel.asm -o "$KERNEL_ENTRY_OBJECT"

    for source in src/kernel/*.c; do
        objects+=("bin/$(basename "${source%.c}").o")
        "$compiler" \
            -m64 -march=x86-64 -mcmodel=small -std=c11 -Os ${KERNEL_CFLAGS:-} \
            -Wall -Wextra -Wpedantic -Werror \
            -ffreestanding -fno-builtin -fno-pic -fno-pie \
            -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-unwind-tables -mno-red-zone -mgeneral-regs-only \
            -c "$source" -o "bin/$(basename "${source%.c}").o"
    done
    log_ok "Compiled ${#objects[@]} x86-64 C files"

    log_info "Linking flat 64-bit long-mode kernel"
    "$linker" -m elf_x86_64 --build-id=none -nostdlib \
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

# The desktop font: any TrueType file dropped into fonts/font.ttf ships on
# the disk as FONT.TTF and the kernel rasterizes it at boot (font_ttf.c).
# It must fit inside the 384 data sectors the bootloader preloads,
# together with the kernel and native wallpaper that sit next to it.
if [[ -f fonts/font.ttf ]]; then
    font_size=$(file_size fonts/font.ttf)
    kernel_sectors=$(( (kernel_size + 511) / 512 ))
    font_limit=$(( (384 - kernel_sectors) * 512 ))
    (( font_size <= font_limit )) || fail \
"fonts/font.ttf is too big: $font_size bytes, but only $font_limit fit into
the preloaded disk window next to the kernel. Subset the font to ASCII, e.g.:
  pyftsubset yourfont.ttf --unicodes=U+0020-007E --no-hinting --output-file=fonts/font.ttf"
    mcopy -i "$BOOT_IMAGE" fonts/font.ttf ::/FONT.TTF
    log_ok "Desktop font: fonts/font.ttf ($font_size bytes) -> FONT.TTF"
else
    log_info "fonts/font.ttf not found -- the kernel will use the BIOS font"
fi

# Ship exactly one native-resolution wallpaper. Both source PNGs stay in the
# repository for editing, but each boot image pays for only its own profile.
if [[ " ${KERNEL_CFLAGS:-} " == *" -DDIMOS_EMULATOR "* ]]; then
    wallpaper_source="images/wallpaper-emulator.png"
    wallpaper_profile="640x480 emulator"
else
    wallpaper_source="images/wallpaper-pc.png"
    wallpaper_profile="1920x1080 PC"
fi
require_file "$wallpaper_source"
mcopy -i "$BOOT_IMAGE" "$wallpaper_source" ::/WALLPAPER.PNG
wallpaper_size=$(file_size "$wallpaper_source")
font_size_on_disk=0
[[ ! -f fonts/font.ttf ]] || font_size_on_disk=$(file_size fonts/font.ttf)
preload_sectors=$(( (kernel_size + 511) / 512 +
                    (font_size_on_disk + 511) / 512 +
                    (wallpaper_size + 511) / 512 ))
(( preload_sectors <= 384 )) || fail \
    "Kernel, font and wallpaper need $preload_sectors sectors; boot preload holds 384"
log_ok "Native wallpaper: $wallpaper_source ($wallpaper_profile, $wallpaper_size bytes)"

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
