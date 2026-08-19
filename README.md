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

Образ системы уже вшит в репозиторий — файл `web/dimos-image.js` (около 8 КБ:
дискета на 1440 КБ почти вся из нулей и отлично жмётся gzip). Поэтому собирать
и выбирать `.img` руками не нужно вообще:

```bash
python3 -m http.server 8080
```

Откройте [http://127.0.0.1:8080/index.html](http://127.0.0.1:8080/index.html)
и нажмите **Start**. Затем один раз щёлкните по чёрному экрану, чтобы
клавиатура ушла в эмулятор, и наберите `HELP`.

Лаунчер сам распаковывает встроенный образ и всегда кладёт его в слот
**floppy** с порядком загрузки `Floppy / CD / Hard Disk`, так что вставить
образ «не туда» невозможно. Кнопка **Свой образ…** остаётся для отладки:
файл на 1440 КБ уйдёт на floppy, загрузочный ISO — на CD.

### «Boot failed: could not read the boot disk»

Эту строку печатает SeaBIOS, а не DimOS: значит эмулятор не нашёл загрузочный
сектор там, куда смотрел. Причина почти всегда одна из трёх:

- открыта страница `copy.sh/v86/debug.html`, и `dimos.img` вставлен в слот
  **CD image** или **Hard disk** вместо **Floppy disk image**;
- выбран старый `dimos.img` из прошлой сборки или пустой `FLOPPY2.img`;
- образ вообще не собран, а страница ждёт файл.

Встроенный лаунчер из `index.html` исключает все три случая. Если всё же
нужна debug-страница v86: `dimos.img` — только в **Floppy disk image**,
**CD image** и **Hard disk** оставить пустыми, **Boot order** =
`Floppy / CD / Hard Disk`.

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
make verify-embedded-image
```

`make verify-embedded-image` проверяет именно тот образ, который грузится в
браузере: распаковывает `web/dimos-image.js` так же, как это делает страница, и
сверяет загрузочный сектор с `bin/BOOT.BIN`, а `KERNEL.BIN` внутри FAT12 — с
`bin/KERNEL.BIN`. Побайтовое сравнение со свежей сборкой тут не годится:
`mkfs.vfat` пишет случайный серийный номер тома, а `mcopy` — текущее время. Если
забыть закоммитить обновлённый `web/dimos-image.js`, страница молча загрузит
старое ядро — эту проверку выполняет и CI.

Результаты:

- `disk_img/dimos.img` — загрузочная FAT12-дискета для лаунчера и QEMU;
- `disk_img/dimos.iso` — El Torito ISO с эмуляцией 1.44M floppy;
- `web/dimos-image.js` — тот же образ в gzip+base64 для браузерного лаунчера
  (обновляется автоматически, отдельно можно пересобрать через
  `./tools/embed-image.sh`);
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
