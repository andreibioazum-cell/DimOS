# DimOS Minimal

Минимальная 32-битная x86-система с командной строкой и встроенной игрой «Змейка». Загрузчик читает `KERNEL.BIN` из FAT12, переводит процессор в protected mode и запускает freestanding-ядро без стандартной библиотеки.

## Команды

- `HELP` — показать список команд;
- `CLEAR` или `CLS` — очистить экран;
- `SNAKE` — запустить игру;
- `DIR` — показать реальные файлы FAT12 на диске;
- `TYPE ИМЯ` — прочитать файл (например, `TYPE README.TXT`);
- `DEL ИМЯ` — скрыть пользовательский файл до перезагрузки. `KERNEL.BIN` защищён.

В игре используются `WASD` или стрелки. `N` начинает новую игру, `Esc` возвращает в командную строку.

## Запуск в браузере

Соберите образ и поднимите любой HTTP-сервер из корня репозитория:

```bash
make iso
python3 -m http.server 8080
```

Откройте [http://127.0.0.1:8080/index.html](http://127.0.0.1:8080/index.html).

1. Нажмите **Выбрать образ** и укажите `disk_img/dimos.img` (1440 КБ).
2. Можно выбрать и `disk_img/dimos.iso` — лаунчер сам положит его на CD.
3. Нажмите **Start**.
4. Один раз щёлкните по экрану, чтобы клавиатура пошла в эмулятор.

Не открывайте `copy.sh/v86/debug.html`: там легко вставить ISO в слот CD и получить `Boot failed: could not read the boot disk`. Лаунчер в `index.html` выбирает floppy или CD сам.

Если debug-страница всё же нужна:

- в **Floppy disk image** только `dimos.img`;
- **CD image** и **Hard disk** пустые;
- **Boot order** = `Floppy / CD / Hard Disk`.

Образ остаётся в браузере и не уходит на сервер. Ядро читает scan-коды из PS/2, без BIOS `int 16h`.

## Где C, а где ассемблер

Основная логика находится в `src/kernel/kernel.c`: VGA-консоль, ввод команд, декодирование клавиатуры, PIT-таймер и игра. Ассемблер оставлен только там, где он действительно нужен:

- `src/bootloader/boot.asm` — BIOS-загрузка FAT12, номер диска из `DL`, чтение через LBA;
- `src/kernel/kernel.asm` — вход из real mode, GDT/protected mode и инструкции портового I/O.

Размещение секций ядра задаёт `src/kernel/linker.ld`.

## Сборка

Требуются `gcc` с поддержкой `-m32`, GNU `ld`/`objcopy`, `nasm`, `dosfstools`, `mtools`, `xorriso`, `g++` и `make`. Системные 32-битные библиотеки не нужны: ядро собирается с `-ffreestanding` и линкуется без libc.

```bash
make iso
make verify
```

Результаты:

- `disk_img/dimos.img` — загрузочная FAT12-дискета для лаунчера и QEMU;
- `disk_img/dimos.iso` — El Torito ISO с эмуляцией 1.44M floppy;
- `bin/KERNEL.ELF` и `bin/KERNEL.MAP` — отладочные артефакты;
- `bin/KERNEL.BIN` — плоское ядро, которое помещается в образ.

В образе есть `KERNEL.BIN` и обычный пользовательский `README.TXT`. `DEL` действует только до перезагрузки: сектора на диск не пишутся.

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
