# DimOS Minimal

Минимальная 32-битная x86-система с командной строкой и встроенной игрой «Змейка». Загрузчик читает `KERNEL.BIN` из FAT12, переводит процессор в protected mode и запускает freestanding-ядро без стандартной библиотеки.

## Команды

- `HELP` — показать список команд;
- `CLEAR` или `CLS` — очистить экран;
- `SNAKE` — запустить игру;
- `DIR` — показать реальные файлы FAT12 на диске;
- `TYPE ИМЯ` — прочитать файл (например, `TYPE README.TXT`);
- `DEL ИМЯ` — удалить любой файл в текущем сеансе, включая системные файлы.

В игре используются `WASD` или стрелки. `N` начинает новую игру, `Esc` возвращает в командную строку.

## Запуск на copy.sh/v86

Сначала соберите образ:

```bash
make iso
```

Затем:

1. Откройте [v86 debug](https://copy.sh/v86/debug.html).
2. В строке **Floppy disk image** выберите `disk_img/dimos.img`.
3. Установите **Boot order** в `Floppy / CD / Hard Disk`.
4. Нажмите **Start Emulation**.
5. После загрузки один раз щёлкните по VGA-экрану эмулятора, чтобы браузер передал ему фокус клавиатуры.

Образ обрабатывается браузером локально и не загружается на сервер. Ядро читает scan-коды напрямую из виртуального PS/2-контроллера, не используя BIOS `int 16h`, поэтому ввод продолжает работать после перехода в protected mode в v86.

## Где C, а где ассемблер

Основная логика находится в `src/kernel/kernel.c`: VGA-консоль, ввод команд, декодирование клавиатуры, PIT-таймер и игра. Ассемблер оставлен только там, где он действительно нужен:

- `src/bootloader/boot.asm` — BIOS-загрузка FAT12;
- `src/kernel/kernel.asm` — вход из real mode, GDT/protected mode и инструкции портового I/O.

Размещение секций ядра задаёт `src/kernel/linker.ld`.

## Сборка

Требуются `gcc` с поддержкой `-m32`, GNU `ld`/`objcopy`, `nasm`, `dosfstools`, `mtools`, `xorriso`, `g++` и `make`. Системные 32-битные библиотеки не нужны: ядро собирается с `-ffreestanding` и линкуется без libc.

```bash
make iso
make verify
```

Результаты:

- `disk_img/dimos.img` — загрузочная FAT12-дискета для v86 и QEMU;
- `disk_img/dimos.iso` — загрузочный ISO;
- `bin/KERNEL.ELF` и `bin/KERNEL.MAP` — отладочные артефакты;
- `bin/KERNEL.BIN` — плоское ядро, которое помещается в образ.

В образе есть `KERNEL.BIN` и обычный пользовательский `README.TXT`. Файловый менеджер читает FAT12-каталог и данные с диска; удаление не содержит специальных сценариев и действует одинаково для всех файлов. `DEL` намеренно действует только до перезагрузки: запись секторов пока не выполняется.

## Запуск в QEMU

```bash
./run-linux.sh
```

Или напрямую:

```bash
qemu-system-x86_64 -drive format=raw,file=disk_img/dimos.img,if=floppy
```

### Termux / proot-distro

```bash
apt update
apt install -y build-essential binutils nasm dosfstools mtools xorriso qemu-system-x86 git
make iso
qemu-system-x86_64 \
  -display vnc=:0 \
  -drive format=raw,file=disk_img/dimos.img,if=floppy,index=0
```

После запуска подключитесь VNC-клиентом к `127.0.0.1:5900`.
