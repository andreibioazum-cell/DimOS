#!/usr/bin/env bash

# Assembles the download folders, one per kind of machine DimOS runs on:
#
#   release/PC        a real PC: the BIOS images, to write to a stick or a
#                     CD-R, and to boot on actual hardware
#   release/Emulator  the same OS in QEMU, VirtualBox or VMware
#   release/Phone     an Android phone or tablet: the AArch64 runtime, the
#                     TrueType font and a ready 8 MiB volume
#
# Nothing here compiles anything. Build the images with `make iso` or
# `make emulator`, build the runtime with `./build-anywhere.sh --target
# aarch64-linux-musl`, then run this script to collect the results.
#
# Usage:
#   tools/release.sh all        # every folder that has its artifacts
#   tools/release.sh pc|emulator|phone
#   tools/release.sh --out DIR  # somewhere other than release/

set -Eeuo pipefail

readonly HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly IMAGES="$HERE/disk_img"
OUT="$HERE/release"

log() { printf '[ INFO ] %s\n' "$1"; }
ok() { printf '[  OK  ] %s\n' "$1"; }
warn() { printf '[ WARN ] %s\n' "$1" >&2; }
fail() { printf '[ FAILED ] %s\n' "$1" >&2; exit 1; }

wanted=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out)
            [[ $# -ge 2 ]] || fail "--out wants a directory"
            OUT="$2"
            shift 2
            ;;
        --out=*)
            OUT="${1#--out=}"
            shift
            ;;
        -h|--help)
            sed -n '4,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        all|pc|emulator|phone)
            wanted+=("$1")
            shift
            ;;
        *)
            fail "unknown option: $1 (try all, pc, emulator, phone)"
            ;;
    esac
done
[[ ${#wanted[@]} -gt 0 ]] || wanted=(all)
for kind in "${wanted[@]}"; do
    [[ "$kind" == "all" ]] && wanted=(pc emulator phone) && break
done

mkdir -p "$OUT"

# --- PC and Emulator ---------------------------------------------------
#
# Both folders carry the same three files: an El Torito ISO for a CD-R, a
# raw image with the boot sector already in place, and the FAT12 volume the
# shell writes to. Only the build flags differ (the emulator profile picks
# the smaller 640x480 output area), so the copy is the same and the notes
# are not.

have_images() {
    [[ -s "$IMAGES/dimos.iso" && -s "$IMAGES/dimos.img" && -s "$IMAGES/dimos.hdd" ]]
}

copy_images() {
    local folder="$1"

    mkdir -p "$OUT/$folder"
    cp "$IMAGES/dimos.iso" "$IMAGES/dimos.img" "$IMAGES/dimos.hdd" "$OUT/$folder/"
    ok "$folder: dimos.iso, dimos.img, dimos.hdd"
}

release_pc() {
    if ! have_images; then
        warn "PC: no images in disk_img (run make iso first)"
        return
    fi
    copy_images PC
    cat > "$OUT/PC/README.txt" <<'EOF'
DimOS 14 для обычного ПК
========================

Три файла — это одна и та же система, записанная тремя способами:

  dimos.img   записать на флешку как есть: dd if=dimos.img of=/dev/sdX bs=1M
              (Windows: Rufus, «режим DD»). Флешка загрузится как есть.
  dimos.iso   записать на CD-R или подключить как CD в виртуальной машине.
  dimos.hdd   8 МБ том FAT12: файлы, которые DimOS пишет, живут здесь.
              Его же можно подключить вторым диском.

Как загрузиться
  1. Запишите dimos.img на флешку (или dimos.iso на диск).
  2. В BIOS/UEFI включите режим Legacy/CSM: DimOS — это 32-битное ядро,
     которое стартует через обычный BIOS-сектор, без UEFI.
  3. Загрузитесь с флешки. Через пару секунд появится экран блокировки
     телефона: нажмите в любом месте, чтобы разблокировать.

Управление
  мышь — палец, ЛКМ — касание, ПКМ — долгое нажатие,
  Esc — жест «домой», стрелки и Enter ходят по кнопкам.

Разрешение: телефона 390x844 рисуется в собственный буфер и растягивается
на любой режим VBE без искажений (1920x1080, 1366x768 и так далее).
EOF
}

release_emulator() {
    if ! have_images; then
        warn "emulator: no images in disk_img (run make emulator first)"
        return
    fi
    copy_images Emulator
    cat > "$OUT/Emulator/README.txt" <<'EOF'
DimOS 14 в эмуляторе
====================

QEMU (самый быстрый способ посмотреть):
  qemu-system-x86_64 -machine pc -m 32M \
      -drive file=dimos.hdd,format=raw,if=ide \
      -boot c -serial none

  Совет: добавьте -display gtk и -full-screen, чтобы получить «телефон»
  на весь экран. Экран DimOS — 390x844, он сам впишется по центру.

VirtualBox
  Создайте машину типа «Other/Unknown (32-bit)», 32 МБ ОЗУ, без UEFI.
  Подключите dimos.iso как CD или dimos.img как жёсткий диск.

VMware / Parallels
  Новая машина, «Other 32-bit», BIOS (не UEFI), 32 МБ ОЗУ,
  dimos.iso как CD-ROM.

Что внутри
  dimos.iso   загрузочный образ для CD
  dimos.img   тот же образ как жёсткий диск (boot sector + ядро)
  dimos.hdd   том FAT12, который DimOS читает и пишет

Профиль эмулятора выбирает меньшую область вывода (640x480), поэтому
эмулятор не перерисовывает лишние мегапиксели каждый кадр.
EOF
}

# --- Phone -------------------------------------------------------------
#
# The phone folder is the hosted runtime: the very same kernel C files
# compiled as an ordinary program, AArch64 and statically linked with musl,
# so it starts on any ARM64 Android phone or Linux tablet without a libc
# that matches the build machine. The browser becomes the phone screen.

release_phone() {
    local binary="$HERE/bin/dimos-anywhere-aarch64-linux-musl"
    local host_binary="$HERE/bin/dimos-anywhere"

    if [[ ! -s "$binary" ]]; then
        warn "phone: no AArch64 runtime (run ./build-anywhere.sh --target aarch64-linux-musl)"
        return
    fi
    mkdir -p "$OUT/Phone/fonts"
    cp "$binary" "$OUT/Phone/dimos-anywhere"
    chmod +x "$OUT/Phone/dimos-anywhere"
    if [[ -s "$HERE/fonts/font.ttf" ]]; then
        cp "$HERE/fonts/font.ttf" "$OUT/Phone/fonts/font.ttf"
    else
        warn "phone: fonts/font.ttf is missing, text falls back to 8x8"
    fi

    # A ready volume, so the first start is not a blank disk. The host
    # binary of the same kernel formats and seeds it; if only the cross
    # build exists, the runtime makes it on first start instead.
    if [[ -x "$host_binary" ]]; then
        rm -f "$OUT/Phone/dimos.hdd"
        ( cd "$OUT/Phone" && "$host_binary" --disk dimos.hdd --no-http info >/dev/null )
        ok "phone: dimos.hdd ($(wc -c < "$OUT/Phone/dimos.hdd" | tr -d '[:space:]') bytes, seeded)"
    else
        warn "phone: no host runtime, dimos.hdd will be created on first start"
    fi

    cat > "$OUT/Phone/run.sh" <<'EOF'
#!/bin/sh
# Starts DimOS on this phone or tablet and prints the address to open.
set -e
cd "$(dirname "$0")"
exec ./dimos-anywhere --port 8080 "$@"
EOF
    chmod +x "$OUT/Phone/run.sh"

    cat > "$OUT/Phone/README.txt" <<'EOF'
DimOS 14 на телефоне или планшете
=================================

Здесь лежит та же самая DimOS, скомпилированная как обычная программа для
ARM64 (AArch64) и слинкованная статически. Поэтому она запускается на любом
64-битном Android-телефоне и на любом 64-битном Linux без подбора версии
системных библиотек.

Что внутри
  dimos-anywhere   сама система (ARM64, статическая, ~740 КБ)
  dimos.hdd        готовый том на 8 МБ: README, HELLO.LUA, SYSINFO.LUA
  fonts/font.ttf   шрифт интерфейса (без него текст рисуется 8x8)
  run.sh           запуск одной командой

Запуск в Termux (Android)
  1. Установите Termux из F-Droid.
  2. Скопируйте эту папку в память телефона и выполните:
       termux-setup-storage
       cd ~/storage/shared/Phone     # или любая папка, куда её положили
       chmod +x dimos-anywhere run.sh
       ./run.sh
  3. Откройте в браузере телефона http://localhost:8080
     Экран 390x844 — это ровно телефон в портретной ориентации.

По локальной сети
  Тот же двоичный файл работает и на компьютере с Linux ARM64; запустите
  его с --bind 0.0.0.0 и откройте http://<адрес-компьютера>:8080 с телефона.

Приложения Lua без графики
  ./dimos-anywhere run HELLO.LUA        выполнить приложение с тома
  ./dimos-anywhere lua -e 'print(dim.platform)'
  ./dimos-anywhere apps               список .LUA на томе

Что это значит для «64 бита»
  DimOS состоит из двух частей. Ядро (src/kernel) — это настоящая
  операционная система для 32-битного ПК: свой загрузочный сектор, VBE,
  драйвер IDE, FAT12, свой растеризатор TrueType. Hosted-сборка берёт те же
  самые файлы ядра и запускает их как процесс на телефоне или ПК — именно
  она лежит здесь, и теперь она собирается под архитектуру ARM64, где
  указатели и адресное пространство 64-битные.

  Полноценное bare-metal ядро под AArch64 (QEMU -M virt, переход EL2/EL3 ->
  EL1, таблицы страниц MMU, драйвер virtio-blk вместо IDE, LittleFS вместо
  FAT12) — отдельный большой проект: у него другой загрузчик, другой
  контроллер диска и другой формат тома, и ни один файл текущего ядра для
  него не переиспользуется как есть. Текущая DimOS честно работает на ПК
  (BIOS/i686) и на телефоне (ARM64, hosted), и это то, что лежит в папках.
EOF
    ok "phone: dimos-anywhere (AArch64), fonts/font.ttf, run.sh, README.txt"
}

for kind in "${wanted[@]}"; do
    case "$kind" in
        pc) release_pc ;;
        emulator) release_emulator ;;
        phone) release_phone ;;
    esac
done

log "folders in $OUT:"
( cd "$OUT" && for folder in */; do
    printf '         %-10s %s\n' "${folder%/}" \
        "$(find "$folder" -maxdepth 1 -type f | wc -l | tr -d '[:space:]') files"
done )
