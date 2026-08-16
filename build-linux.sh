#!/usr/bin/env bash

# DimOS build script for Linux.
# Produces a bootable FAT12 floppy image and an El Torito ISO image.

set -Eeuo pipefail

readonly FLOPPY_SIZE_BYTES=1474560
readonly MAX_KERNEL_LOADER_BYTES=43008   # 0xA800: start of the kernel directory-list buffer.
readonly KERNEL_SIZE_WARN_BYTES=40960    # Warn 2 KiB before the loader limit.
readonly BOOT_IMAGE="disk_img/dimos.img"
readonly SECOND_FLOPPY_IMAGE="disk_img/FLOPPY2.img"
readonly ISO_IMAGE="disk_img/dimos.iso"
readonly IMAGE_CHECKER="bin/dimos-image-check"

QUIET=0
NO_MUSIC=0
NO_TEXT=0
NO_BOOT_RECOMPILE=0
NO_KERNEL_RECOMPILE=0
NO_PROGRAMS_RECOMPILE=0
NO_LOGO_DISPLAY=0
NO_SETUP=0
BUILD_ISO=1

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    RED=$'\033[31m'
    GREEN=$'\033[32m'
    YELLOW=$'\033[33m'
    CYAN=$'\033[36m'
    RESET=$'\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    CYAN=''
    RESET=''
fi

usage() {
    cat <<'USAGE'
Usage: ./build-linux.sh [options]

Options:
  --quiet                  Print only errors
  --no-music               Do not copy music assets
  --no-text                Do not copy documentation to the image
  --no-boot-recompile      Reuse bin/BOOT.BIN
  --no-kernel-recompile    Reuse bin/KERNEL.BIN
  --no-programs-recompile  Reuse program binaries from bin/
  --no-logo-display        Compile the kernel without the startup logo
  --no-setup               Compile programs without the first-boot setup
  --no-iso                 Build only the FAT12 images
  --dtm                    Development mode (no logo and no setup)
  -h, --help               Show this help

The legacy single-dash option names (for example, -quiet) remain supported.
USAGE
}

log_info() {
    if (( ! QUIET )); then
        printf '%s[ INFO ]%s %s\n' "$CYAN" "$RESET" "$1"
    fi
}

log_ok() {
    if (( ! QUIET )); then
        printf '%s[  OK  ]%s %s\n' "$GREEN" "$RESET" "$1"
    fi
}

log_section() {
    if (( ! QUIET )); then
        printf '\n%s========== %s ==========%s\n' "$GREEN" "$1" "$RESET"
    fi
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
        --no-music|-no-music) NO_MUSIC=1 ;;
        --no-text|--no-txt|-no-txt) NO_TEXT=1 ;;
        --no-boot-recompile|-no-boot-recomp) NO_BOOT_RECOMPILE=1 ;;
        --no-kernel-recompile|-no-kernel-recomp) NO_KERNEL_RECOMPILE=1 ;;
        --no-programs-recompile|-no-programs-recomp) NO_PROGRAMS_RECOMPILE=1 ;;
        --no-logo-display|-no-logo-display) NO_LOGO_DISPLAY=1 ;;
        --no-setup|-no-setup) NO_SETUP=1 ;;
        --no-iso) BUILD_ISO=0 ;;
        --dtm|-dtm)
            NO_SETUP=1
            NO_LOGO_DISPLAY=1
            ;;
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
    shift 2

    log_info "Assembling $source -> $output"
    nasm -f bin "$@" "$source" -o "$output"
}

copy_to_image() {
    local source=$1
    local destination=$2

    require_file "$source"
    log_info "Copying $source -> $destination"
    mcopy -i "$BOOT_IMAGE" "$source" "$destination"
}

create_directory() {
    local directory=$1
    log_info "Creating $directory"
    mmd -i "$BOOT_IMAGE" "::/$directory"
}

compile_program_group() {
    local destination=$1
    local include_directory=$2
    local definitions=$3
    shift 3
    local entries=("$@")
    local entry source output
    local nasm_options=()

    [[ -z "$include_directory" ]] || nasm_options+=("-I${include_directory}/")
    [[ -z "$definitions" ]] || nasm_options+=("-D${definitions}")

    for entry in "${entries[@]}"; do
        IFS='|' read -r source output <<< "$entry"

        if (( ! NO_PROGRAMS_RECOMPILE )); then
            assemble "$source" "bin/$output" "${nasm_options[@]}"
        else
            require_file "bin/$output"
        fi

        copy_to_image "bin/$output" "$destination"
    done
}

create_floppy_image() {
    local path=$1

    log_info "Creating FAT12 image: $path"
    truncate -s "$FLOPPY_SIZE_BYTES" "$path"
    mkfs.vfat -F 12 -n DIMOS "$path" >/dev/null
}

build_image_checker() {
    local compiler=${CXX:-g++}

    if [[ ! -x "$IMAGE_CHECKER" || tools/image_inspector.cpp -nt "$IMAGE_CHECKER" ]]; then
        log_info "Compiling C++ image checker"
        "$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
            tools/image_inspector.cpp -o "$IMAGE_CHECKER"
    fi
}

check_kernel_size() {
    local size_bytes
    size_bytes=$(file_size bin/KERNEL.BIN)

    (( size_bytes > 0 )) || fail "Kernel image is empty: bin/KERNEL.BIN"
    log_info "Kernel size: $size_bytes bytes (loader limit: $MAX_KERNEL_LOADER_BYTES bytes)"

    if (( size_bytes > KERNEL_SIZE_WARN_BYTES )); then
        printf '%s[ WARN ]%s Kernel is close to the loader limit (%s bytes).\n' \
            "$YELLOW" "$RESET" "$size_bytes" >&2
    fi

    (( size_bytes <= MAX_KERNEL_LOADER_BYTES )) || \
        fail "Kernel is too large for the bootloader ($size_bytes > $MAX_KERNEL_LOADER_BYTES bytes)"
}

create_iso_image() {
    local staging_directory="disk_img/iso-root"

    log_section "Creating bootable ISO"
    rm -rf "$staging_directory"
    mkdir -p "$staging_directory"
    cp "$BOOT_IMAGE" "$staging_directory/dimos.img"

    # A 1.44 MB boot image is recognized as El Torito floppy emulation.
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

for command in nasm mkfs.vfat mcopy mmd mdir truncate; do
    require_command "$command"
done
require_command "${CXX:-g++}"
if (( BUILD_ISO )); then
    require_command xorriso
fi

mkdir -p bin disk_img
build_image_checker

log_section "Compiling DimOS"

if (( ! NO_BOOT_RECOMPILE )); then
    assemble src/bootloader/boot.asm bin/BOOT.BIN
else
    require_file bin/BOOT.BIN
fi

if (( ! NO_KERNEL_RECOMPILE )); then
    kernel_options=()
    (( ! NO_LOGO_DISPLAY )) || kernel_options+=("-DNO_LOGO_DISPLAY")
    assemble src/kernel/kernel.asm bin/KERNEL.BIN "${kernel_options[@]}"
else
    require_file bin/KERNEL.BIN
fi
check_kernel_size

log_section "Creating disk images"
create_floppy_image "$BOOT_IMAGE"
create_floppy_image "$SECOND_FLOPPY_IMAGE"

log_info "Installing boot sector"
dd if=bin/BOOT.BIN of="$BOOT_IMAGE" conv=notrunc status=none
copy_to_image bin/KERNEL.BIN ::/

image_directories=(
    BIN.DIR
    COM.DIR
    EXE.DIR
    PLE.DIR
    BMP.DIR
    CONF.DIR
    DOCS.DIR
    MUSIC.DIR
    FONTS.DIR
    THEMES.DIR
)
for directory in "${image_directories[@]}"; do
    create_directory "$directory"
done

log_section "Copying fonts, themes, and configuration"
for file in assets/fonts/*.FNT; do
    copy_to_image "$file" ::/FONTS.DIR/
done
for file in assets/themes/*.THM; do
    copy_to_image "$file" ::/THEMES.DIR/
done

configuration_files=(
    "src/kernel/configs/USER.CFG|::/CONF.DIR/"
    "src/kernel/configs/FIRST_B.CFG|::/CONF.DIR/"
    "src/kernel/configs/PASSWORD.CFG|::/CONF.DIR/"
    "src/kernel/configs/TIMEZONE.CFG|::/CONF.DIR/"
    "src/kernel/configs/PROMPT.CFG|::/CONF.DIR/"
    "src/kernel/configs/THEME.CFG|::/CONF.DIR/"
    "src/kernel/configs/FONT.CFG|::/CONF.DIR/"
    "src/kernel/configs/SYSTEM.CFG|::/"
)
for entry in "${configuration_files[@]}"; do
    IFS='|' read -r source destination <<< "$entry"
    copy_to_image "$source" "$destination"
done

log_section "Compiling and copying programs"
root_programs=(
    "programs/autoexec.asm|AUTOEXEC.BIN"
    "programs/setup/setup.asm|SETUP.BIN"
)
root_definitions=''
(( ! NO_SETUP )) || root_definitions=NO_SETUP
compile_program_group ::/ '' "$root_definitions" "${root_programs[@]}"

bin_programs=(
    "programs/help.asm|HELP.BIN"
    "programs/grep.asm|GREP.BIN"
    "programs/head.asm|HEAD.BIN"
    "programs/tail.asm|TAIL.BIN"
    "programs/cpu.asm|CPU.BIN"
    "programs/dlist.asm|DLIST.BIN"
    "programs/theme.asm|THEME.BIN"
    "programs/fetch.asm|FETCH.BIN"
    "programs/imfplay.asm|IMFPLAY.BIN"
    "programs/wavplay.asm|WAVPLAY.BIN"
    "programs/credits.asm|CREDITS.BIN"
    "programs/hello.asm|HELLO.BIN"
    "programs/write.asm|WRITER.BIN"
    "programs/barchart.asm|BCHART.BIN"
    "programs/brainf.asm|BRAINF.BIN"
    "programs/calc.asm|CALC.BIN"
    "programs/memory.asm|MEMORY.BIN"
    "programs/mine.asm|MINE.BIN"
    "programs/piano.asm|PIANO.BIN"
    "programs/snake.asm|SNAKE.BIN"
    "programs/space.asm|SPACE.BIN"
    "programs/procentc.asm|PROCENTC.BIN"
    "programs/paint.asm|PAINT.BIN"
    "programs/pong.asm|PONG.BIN"
    "programs/hexedit.asm|HEXEDIT.BIN"
    "programs/clock.asm|CLOCK.BIN"
    "programs/mandel.asm|MANDEL.BIN"
    "programs/tetris.asm|TETRIS.BIN"
    "programs/tetris-df.asm|TETRIS2.BIN"
    "programs/chars.asm|CHARS.BIN"
    "programs/eye.asm|EYE.BIN"
    "programs/ed.asm|ED.BIN"
    "programs/fdisk.asm|FDISK.BIN"
    "programs/launch.asm|LAUNCH.BIN"
    "programs/font.asm|FONT.BIN"
    "programs/tree.asm|TREE.BIN"
    "programs/print.asm|PRINT.BIN"
    "programs/calendar.asm|CALENDAR.BIN"
    "programs/settings.asm|SETTINGS.BIN"
)
compile_program_group ::/BIN.DIR/ '' '' "${bin_programs[@]}"

com_programs=(
    "programs/COM/hello.asm|HELLO.COM"
    "programs/COM/fractal.asm|FRACTAL.COM"
    "programs/COM/clock.asm|CLOCK.COM"
)
compile_program_group ::/COM.DIR/ '' '' "${com_programs[@]}"

exe_programs=("programs/EXE/hello.asm|HELLO.EXE")
compile_program_group ::/EXE.DIR/ programs/EXE '' "${exe_programs[@]}"

ple_programs=("programs/PLE/src/hello.asm|HELLO.PLE")
compile_program_group ::/PLE.DIR/ programs/PLE '' "${ple_programs[@]}"

if (( ! NO_TEXT )); then
    log_section "Copying documentation"
    copy_to_image LICENSE.TXT ::/
    log_info "Copying project_philosophy.txt -> ::/PROJECT.TXT"
    mcopy -i "$BOOT_IMAGE" project_philosophy.txt ::/PROJECT.TXT

    documentation_files=(
        src/txt/README.TXT
        src/txt/CONFIGS.TXT
        src/txt/FILESYS.TXT
        src/txt/LIMITS.TXT
        src/txt/PROGRAMS.TXT
        src/txt/QUICKST.TXT
        src/txt/COMMANDS.TXT
        src/txt/EDMAN.TXT
    )
    for file in "${documentation_files[@]}"; do
        copy_to_image "$file" ::/DOCS.DIR/
    done
fi

log_section "Copying media assets"
image_files=(
    assets/images/logo/LOGO.BMP
    assets/images/PROX.BMP
    assets/images/PROS.BMP
    assets/images/PROS_W.BMP
    assets/images/PROS_A.BMP
    assets/images/TRAIN.BMP
    assets/images/CHILL.BMP
)
for file in "${image_files[@]}"; do
    copy_to_image "$file" ::/BMP.DIR/
done

if (( ! NO_MUSIC )); then
    music_files=(
        assets/IMF/RICK.IMF
        assets/IMF/SONIC.IMF
        "assets/IMF/HOPES&D.IMF"
        assets/IMF/RUSSIA.IMF
        assets/IMF/METRO_E.IMF
        assets/IMF/METRO_E2.IMF
        assets/IMF/GTA_VC.IMF
        assets/IMF/CYBWRLD.IMF
        assets/IMF/BIGSHOT.IMF
        assets/IMF/DF.IMF
        assets/IMF/TRUEHERO.IMF
        assets/IMF/CORE.IMF
        assets/WAV/1985.WAV
    )
    for file in "${music_files[@]}"; do
        copy_to_image "$file" ::/MUSIC.DIR/
    done
fi

log_section "Validating build artifacts"
"$IMAGE_CHECKER" bin/BOOT.BIN bin/KERNEL.BIN "$BOOT_IMAGE"

if (( ! QUIET )); then
    printf '\n%sDisk contents:%s\n' "$YELLOW" "$RESET"
    mdir -i "$BOOT_IMAGE" ::/
fi

if (( BUILD_ISO )); then
    create_iso_image
fi

log_section "Build completed successfully"
log_ok "Floppy image: $BOOT_IMAGE"
if (( BUILD_ISO )); then
    log_ok "ISO image: $ISO_IMAGE"
fi
