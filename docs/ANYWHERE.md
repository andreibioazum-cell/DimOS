# DimOS Anywhere — та же ОС на ПК, планшете и телефоне, плюс Lua без GUI

DimOS 14 изначально грузится по BIOS на PC-совместимой машине. **DimOS
Anywhere** — это второй способ запустить *тот же самый* код: ядро
(`src/kernel/*.c`) компилируется как обычная программа и работает на
**Windows-ПК, Linux-ПК и Android** (телефон, планшет, ТВ-бокс с браузером).

Ничего не переписано: оболочка с экраном блокировки, домашней сеткой, доком,
Control Centre и **всеми девятью приложениями**, драйвер FAT12, TrueType-
растеризатор и настройки — это одни и те же файлы. Меняется только «железо»
под ними: вместо портов реальной машины стоит слой `src/host/platform.c`,
который изображает 8042-контроллер, IDE-контроллер, PIT, CMOS-часы и VBE-
фреймбуфер — но теперь поверх файла на диске и часов хоста.

```
                 ┌──────────────────────────────────────────┐
   PC / Windows  │  src/kernel/  (без изменений)            │
   Android       │  gui.c  app_*.c  gfx.c  volume.c  ttf     │
   терминал      │──────────────────────────────────────────│
                 │  src/host/   платформа и фронтенды        │
                 │  platform.c  ui.c  http.c  lua_engine.c   │
                 └──────────────────────────────────────────┘
```

## Что поддерживается, а что нет

| Хост | Поддержка | Как |
| --- | --- | --- |
| Linux x86-64 / ARM64 | да | gcc или clang, `./build-anywhere.sh` |
| Windows x86-64 | да | MinGW-w64 (gcc) или clang, `build-anywhere.sh` из MSYS2/Termux-подобной оболочки |
| Android (ARM64, x86-64) | да | Termux `clang` или NDK, `CC=<triple>-clang ./build-anywhere.sh` |
| ARM64 кросс-сборка без тулчейна | да | `./build-anywhere.sh --target aarch64-linux-musl` (через `zig`, см. ниже) |
| **macOS** | **нет, намеренно** | `build-anywhere.sh` отказывается собирать на Darwin |

## Сборка

```bash
./build-anywhere.sh          # -> bin/dimos-anywhere (Linux/Android/Windows)
make anywhere                # то же через Makefile
./build-anywhere.sh -j 8     # восемь параллельных компиляций
JOBS=1 ./build-anywhere.sh   # по одной, чтобы читать ошибки компилятора
```

Сборка **инкрементальная**: объекты лежат в `bin/anywhere/objects` и
пересобираются только при изменении исходника, любого заголовка или самого
компилятора/флагов (отпечаток в `bin/anywhere/flags.stamp`). Что осталось —
компилируется параллельно, по одному заданию на ядро.

| Сценарий | 2 ядра (как в песочнице) |
| --- | --- |
| чистая сборка | ~3.1 с |
| без изменений | **~0.17 с** |
| правка одного `.c` | ~0.25 с (0.07 с компиляция + линковка) |
| `--quick` чистая сборка | ~1.4 с |

Почему так: Lua (27 файлов) — самая дорогая часть, поэтому он собирается с
`-O1`, а ядро остаётся на `-O2` (его рисует 60 кадров в секунду). `--quick`
опускает оба до `-O0` — для быстрых итераций по интерфейсу, бинарник при
этом чуть медленнее и крупнее. Если в системе есть `ccache`, он
подхватывается автоматически (`USE_CCACHE=0` отключает). `--stats` печатает,
сколько файлов каждой группы осталось собрать; `--clean` забывает всё.

Скрипт собирает три группы файлов:

1. `src/kernel/*.c` — само ядро DimOS (freestanding C11, без libc);
2. `src/host/*.c` — платформа, веб-интерфейс, PNG-вывод и Lua-мост;
3. `third_party/lua/*.c` — **Lua 5.4.7** (ядро, `lauxlib` и библиотеки
   `base`, `table`, `string`, `math`, `coroutine`, `utf8`).

### ARM64 одной командой: `--target`

`--target TRIPLE` собирает те же исходники через `zig cc` — у него внутри
clang, lld и заголовки libc для нужной платформы, поэтому отдельный тулчейн
не нужен:

```bash
pip install ziglang                     # ставит пакет ziglang и обёртку python-zig
./build-anywhere.sh --target aarch64-linux-musl    # -> bin/dimos-anywhere-aarch64-linux-musl
./build-anywhere.sh --target x86_64-windows-gnu    # -> exe для Windows
```

Скрипт сам находит компилятор: `zig` в `PATH`, иначе `python-zig`, иначе
бинарник внутри пакета `ziglang` — поэтому `pip install ziglang` достаточно,
ничего дописывать в `PATH` не нужно.

Для `aarch64-linux-musl` бинарник получается **статическим** (около 740 КБ),
поэтому он запускается на любом 64-битном Android и Linux без подбора версии
системной библиотеки: Android-ядро умеет запускать статические ELF-файлы, а
musl внутри файла ни от чего не зависит. Готовый файл вместе со шрифтом и
томом лежит в `release/Phone` (см. `tools/release.sh`).

Кросс-сборка под Android через NDK:

```bash
NDK=$HOME/android-ndk
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  ./build-anywhere.sh
# затем: adb push bin/dimos-anywhere /data/local/tmp/ и запуск в Termux/через adb shell
```

В Termux всё проще — там обычный clang, и `./build-anywhere.sh` работает как
на Linux.

## Три папки для скачивания: PC, Emulator, Phone

`tools/release.sh` (или `make release`) раскладывает собранное по трём
папкам:

| Папка | Содержимое | Кому |
| --- | --- | --- |
| `release/PC` | `dimos.iso`, `dimos.img`, `dimos.hdd`, `README.txt` | настоящему ПК: флешка или CD-R, загрузка через BIOS/Legacy |
| `release/Emulator` | те же образы + свой `README.txt` с командами QEMU/VirtualBox/VMware | виртуальной машине (профиль эмулятора, область вывода 640×480) |
| `release/Phone` | `dimos-anywhere` (ARM64, статический), `dimos.hdd` (8 МБ, с HELLO.LUA и SYSINFO.LUA), `fonts/font.ttf`, `run.sh`, `README.txt` | телефону или планшету на Android/Linux ARM64 |

```bash
./build-anywhere.sh --target aarch64-linux-musl   # рантайм для телефона
tools/release.sh phone                            # собрать release/Phone
tools/release.sh all                              # всё, что уже собрано
```

`tools/release.sh` ничего не компилирует: образы PC/Emulator он берёт из
`disk_img/` (их делают `make iso` и `make emulator`), а для Phone — готовый
AArch64-рантайм, шрифт и том, который создаёт и заполняет host-сборка того же
ядра. Если чего-то нет, шаг честно предупреждает и пропускается, а CI
собирает все три папки и складывает их в `DimOS-<sha>.zip`.

## Запуск: оболочка в браузере

```bash
bin/dimos-anywhere
# DimOS Anywhere 1.0 -- the DimOS 14 shell on this machine
#   volume  dimos.hdd, 8176 KiB free, 4 files
#   open    http://localhost:8080/
```

Откройте `http://localhost:8080/` на этом же ПК, или
`http://<ip-этого-устройства>:8080/` на телефоне/планшете в той же Wi-Fi —
и вы получите **телефон**: экран блокировки, домашнюю сетку, док, Control
Centre и все приложения. Тап по экрану — палец, долгое нажатие — правый клик
(так ставятся флажки в «Сапёре»), колесо мыши — прокрутка. На телефоне
страница занимает весь экран, масштабируется под свой размер и работает
пальцем.

Полезные ключи:

```
--disk PATH      образ тома (по умолчанию dimos.hdd)
--size MIB       размер нового образа (по умолчанию 8)
--port N         порт веб-интерфейса (0 — свободный)
--bind ADDRESS   адрес прослушивания (0.0.0.0 — доступ с телефона)
--no-http        без веб-сервера (только ядро и терминал)
--display WxH    притвориться панелью WxH (например 1920x1080 или 1220x2712)
--font PATH      запасной TrueType, если на томе нет FONT.TTF
--shot FILE --after MS   один кадр в PNG и выход
```

`dimos.hdd` — **настоящий FAT12-том**. Его можно смонтировать в Linux,
открыть `mtools`, скопировать файлы и даже загрузить в QEMU: это тот же
формат, что у образов из `make iso`.

## Lua: приложения без GUI

Требование простое: приложение на Lua должно запускаться и **ничего не
рисовать**. Поэтому у Lua нет доступа к фреймбуферу, виджетам и событиям
экрана — приложение разговаривает с системой через модуль `dim`:

```lua
-- HELLO.LUA: печатает время, пишет файл, перечисляет том
local clock = dim.clock()
dim.print("Hello from " .. _VERSION .. " on " .. dim.platform)
dim.print(string.format("It is %02d:%02d on %04d-%02d-%02d",
    clock.hour, clock.minute, clock.year, clock.month, clock.day))

if dim.file.write("HELLO.TXT", "written by lua\n") then
    dim.print("wrote HELLO.TXT")
end
for _, file in ipairs(dim.file.list()) do
    dim.print(string.format("  %-12s %6d bytes", file.name, file.size))
end
```

Запуск:

```bash
bin/dimos-anywhere run HELLO.LUA          # программа, без GUI, код возврата приложения
bin/dimos-anywhere lua HELLO.LUA          # то же через интерпретатор
bin/dimos-anywhere lua -e 'print(dim.platform, #dim.file.list())'
bin/dimos-anywhere lua                    # REPL, строка с "=" печатает значение
bin/dimos-anywhere apps                   # какие .LUA лежат на томе
```

Те же приложения запускаются **из браузера**: блок «Lua console» под экраном
телефона умеет выбрать файл с тома, загрузить исходник, отредактировать и
выполнить — вывод возвращается на страницу. Плюс `POST /lua` для скриптов.

### Модуль `dim`

| Функция | Что делает |
| --- | --- |
| `dim.print(...)`, `dim.write(...)` | вывод в консоль (его же получает браузер) |
| `dim.log(text)` | строка в stderr процесса |
| `dim.sleep(ms)` | пауза без загрузки ядра |
| `dim.millis()`, `dim.time()` | мс с загрузки и секунды эпохи |
| `dim.clock()` | `year, month, day, hour, minute, second` из RTC/часов хоста |
| `dim.file.list()` | массив `{ name = "FILE.TXT", size = 123 }` с тома DimOS |
| `dim.file.read(name)` | содержимое файла (или `nil, err`) |
| `dim.file.write(name, data)` | записать файл 8.3 на том (тот же FAT12) |
| `dim.file.remove(name)`, `dim.file.rename(a, b)` | удалить / переименовать |
| `dim.info()` | `screen{width,height}`, `disk{path,bytes,free_bytes,model,mounted,filesystem}`, `theme`, `brightness`, `volume`, `files`, `apps`, `lua`, `kernel`, `platform` |
| `dim.theme([n])`, `dim.brightness([n])`, `dim.volume([n])` | прочитать или изменить (и сохранить в `CONFIG.CFG`) |
| `dim.exit(code)` | выйти с кодом |
| `dim.require(name)`, `dofile(name)` | загрузить `.LUA` с **тома** (не с диска хоста) |

`dim.version` — версия DimOS, `dim.kernel` — `"DimOS 14"`,
`dim.platform` — `"linux"`, `"windows"` или `"android"`.

Сознательно **не загружены** `io`, `os`, `debug` и динамический `require`:
файлы идут через `dim.file`, то есть на том DimOS, а не на машину, где
случайно запущен Lua-код. Это и есть правило «Lua-приложение без GUI»:
приложение видит систему, а не рабочий стол хоста.

## Как это проверяется

```bash
make anywhere-test        # то же: bin/dimos-anywhere self-test
```

Тест ведёт **настоящую оболочку**: сам разблокирует экран, открывает Files,
Terminal и Settings, печатает в терминале `DISK` и `SAVE SELFTEST.TXT ...`,
проверяет, что файл действительно появился на томе, и в это же время
запускает Lua-скрипт, который читает `dim.info()`, видит девять приложений и
пишет `LUA.TXT` на тот же том. Скриншоты кадров складываются в
`bin/anywhere-shots/`.

Сам тест занимает ~1.6 с: каждая ступень ждёт несколько кадров, а не
фиксированные 260 мс, и не проверяет результат, пока хост ещё печатает текст.

```
  PASS  the kernel booted and composited a frame
  PASS  the home screen drew after unlocking
  PASS  Files listed the FAT12 volume
  PASS  the volume carries the seeded README.TXT
  PASS  the terminal accepted keys from the host
  PASS  the terminal wrote SELFTEST.TXT through the IDE driver
  PASS  Settings opened
  PASS  Lua 5.4 runs inside the hosted shell
  PASS  Lua can see the nine applications
  PASS  Lua wrote LUA.TXT to the FAT12 volume
  PASS  the shell returned to the lock screen
SELF-TEST PASSED
```

Отдельные снимки:

```bash
bin/dimos-anywhere --disk bin/ci.hdd --display 2712x1220 \
    --shot /tmp/panel.png --after 400
```

## Устройство

| Файл | Роль |
| --- | --- |
| `src/host/platform.c` | «железо»: порты 0x60/0x64 (PS/2), IDE на 0x1F0 (диск = файл), PIT (монотонные часы), CMOS (сегодняшняя дата), фреймбуфер, четыре окна FAT12 и RAM-диск |
| `src/host/ui.c` | кадр → тайлы 16×16, PNG-энкодер (без зависимостей) |
| `src/host/http.c` | встроенный сервер: страница-«телефон», `/frame`, `/input`, `/info`, `/lua` |
| `src/host/lua_engine.c` | Lua 5.4.7 и модуль `dim` |
| `src/host/main.c` | командная строка, `run`/`lua`/`info`/`apps`/`self-test`, сценарии скриншотов |
| `src/host/dimos_host.h` | единственный заголовок, который знает про хост |
| `build-anywhere.sh` | сборка (и отказ на macOS); инкрементальная, `-j N`, `--quick` |

В ядре появилось ровно несколько `#ifdef DIMOS_HOST`:

* `dimos_physical()` / `dimos_address()` — вместо жёсткого `(u32)` указателя,
  чтобы 64-битный процесс мог работать с адресами (и с фреймбуфером на ARM64);
* `fs.c` — четыре окна FAT12 и RAM-диск берутся у хоста;
* `kernel.c` — часы от хоста (вместо PIT), BIOS-память заглушена, restart и
  shutdown завершают процесс, а `gui_run()` отдаёт готовые кадры наружу;
* `app_about.c` — Settings честно показывает `hosted, linux/android/windows`.

Пока оболочка ждёт следующий кадр (60 Гц), хост обслуживает браузер и кормит
очередь клавиатуры — поэтому бездействующий DimOS занимает ~3 % одного ядра,
а не 100 %, и может жить на телефоне.

## Ограничения

* Веб-интерфейс — это то, что заменяет экран: без браузера оболочка работает,
  но смотреть на неё нечем (`--no-http` нужен для тестов и скриптов).
* Звук PC-speaker в хостовой сборке не воспроизводится (порты пишутся в
  никуда), изображение и ввод — полностью.
* `dim.file.*` работает с именами 8.3 в корне тома, как и весь DimOS.
* macOS не поддерживается намеренно: цель — Android и ПК.
