# legacy/ — прежняя DimOS (bare metal)

Здесь сохранён предыдущий проект: **DimOS 14**, 32-битная система на голом
железе. Загрузочный сектор на NASM, переход в protected mode, выбор
VBE-режима в реальном режиме, собственный драйвер IDE, читаемая и
записываемая FAT12, PS/2-мышь, системный динамик и телефонная оболочка в
стиле iOS 14 с девятью приложениями.

Корень репозитория теперь занимает **Sokol OS** — одна программа на C с
одним окном и одной игрой. Этот каталог оставлен как история и как источник
данных: например, `tools/make_mono_font.py` в корне берёт таблицу глифов
именно из `legacy/tools/sandbox/font8x8.py`.

## Как собрать legacy-версию

Нужны `nasm`, `gcc -m32`, `dosfstools`, `mtools`, `xorriso`, `qemu-system-x86_64`.
Скрипты работают с путями относительно своего каталога, поэтому запускать их
нужно из `legacy/`:

```
cd legacy
./build-linux.sh     # disk_img/dimos.img, dimos.hdd, dimos.iso
./run-linux.sh       # QEMU с панелью 2712x1220
```

CI в корне репозитория собирает только Sokol OS; сборка DimOS осталась
рабочей, но больше не проверяется автоматически.

## Что внутри

```
legacy/
  build-linux.sh   run-linux.sh   Makefile
  src/bootloader/  src/kernel/           загрузчик, ядро, оболочка, приложения
  tools/           image_check.c, make_disk.py, sandbox/ (Unicorn-прогон ядра)
  images/  assets/  files/               иконки, обои и содержимое образа
```
