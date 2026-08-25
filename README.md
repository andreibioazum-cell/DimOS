# DimOS workshop

32-битная x86-система с графической мастерской в VGA mode 13h. На полу
лежат деревянные кубики и тёплые подушки: их можно двигать стрелками.
Куб с приложением по Enter разворачивается в окно.

Загрузчик читает `KERNEL.BIN` из FAT12, включает 320×200×256, переводит
процессор в protected mode и запускает freestanding-ядро.

## Мастерская

- `Tab` — выбрать следующий предмет;
- стрелки — подвинуть выбранный куб или подушку;
- `Enter` — открыть приложение (`term`, `snake`, `files`, `about`);
- `Esc` — вернуться в мастерскую.

Декоративные предметы (`block`, `shaving`, `pillow`, `warm`) никуда не
открываются — ими можно просто играть.

## Команды терминала

- `HELP` — список команд;
- `CLEAR` / `CLS` — очистить окно;
- `SNAKE` — игра;
- `DIR` — файлы FAT12 на диске;
- `TYPE ИМЯ` — прочитать файл (`TYPE README.TXT`);
- `DEL ИМЯ` — скрыть пользовательский файл до перезагрузки;
- `MEM` — куча ядра;
- `ABOUT` / `VER` — о системе;
- `DESK` — назад в мастерскую;
- `ECHO текст`.

В змейке: `WASD` или стрелки, `Enter` после Game Over, `Esc` — выход.

## Запуск в браузере

Образ вшит в `web/dimos-image.js`. Собирать руками не нужно:

```bash
python3 -m http.server 8080
```

Откройте [http://127.0.0.1:8080/index.html](http://127.0.0.1:8080/index.html).
Лаунчер сам стартует и всегда кладёт образ в слот **floppy**.

Щёлкните по экрану, чтобы клавиатура ушла в эмулятор.

## Сборка

Нужны `gcc -m32`, GNU `ld`/`objcopy`, `nasm`, `g++`, `make` и `python3`.
`xorriso` нужен только для ISO.

```bash
make iso
make verify
make verify-embedded-image
```

Результаты:

- `disk_img/dimos.img` — загрузочная FAT12-дискета;
- `disk_img/dimos.iso` — El Torito ISO (если есть xorriso);
- `web/dimos-image.js` — тот же образ в gzip+base64;
- `bin/KERNEL.BIN` — плоское ядро.

`DEL` действует только до перезагрузки. `KERNEL.BIN` защищён.

## Запуск в QEMU

```bash
./run-linux.sh
```

Или:

```bash
qemu-system-x86_64 -drive format=raw,file=disk_img/dimos.img,if=floppy
```
