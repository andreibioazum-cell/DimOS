# DimOS

**Операционная система, на экране которой одна программа — Змейка.** Всё,
что рисуется, рисует **sokol**: окно, ввод и вывод кадра. Поверх обычного
Arch Linux собирается загрузочный образ, который стартует сразу в DimOS: ни
меню, ни логов, ни консоли, ни логина — только рабочий стол.

```
./build.sh iso      # dist/dimos.iso  +  dist/dimos.img
./build.sh run      # тот же интерфейс как обычное окно на рабочем ПК
```

## Два артефакта

| Файл | Что это |
| --- | --- |
| `dist/dimos.iso` | загрузочный образ (BIOS + UEFI), пишется на CD/DVD или флешку |
| `dist/dimos.img` | те же байты как сырой образ диска: `dd if=dist/dimos.img of=/dev/sdX bs=4M` |

Оба файла — одно и то же: archiso делает гибридный образ, который грузится и
с CD, и с флешки. Внутри — Arch Linux (ядро, udev, Xorg, Mesa/llvmpipe) и
ровно одна прикладная программа, `dimos`.

## Что видно на экране

| Этап | Что происходит |
| --- | --- |
| Загрузка | ничего. Загрузчик молчит (`SILENT`, таймаут 0,1 с), ядро молчит (`quiet loglevel=0`), systemd молчит, `agetty` запускается без `/etc/issue`. Экран остаётся чёрным, пока не появится DimOS. |
| Рабочий стол | DimOS разворачивается на весь экран: обои со свечением, панель с живыми часами и иконками Wi-Fi/звука/батареи, одна иконка приложения по центру. |
| Змейка | поле 30×18, яблоки, ускорение, счёт и рекорд, пауза, «Game over», возврат на рабочий стол. |

Никакой «оболочки Arch Linux» на экране нет: ни `ISOLINUX`, ни логотипа, ни
`Arch Linux` в заголовке, ни строки `[  OK  ]`, ни `login:`.

Управление: мышь — всё, `Enter` — открыть приложение, стрелки/WASD — руль,
`P` — пауза, `R`/`Enter` — заново, `Esc` — назад (на рабочем столе — выход из
сеанса, после чего он поднимается заново).

Отладочная подсказка: если нужен лог загрузки, добавьте к параметрам ядра
`loglevel=7 systemd.show_status=true console=ttyS0,115200` (в BIOS — правкой
`syslinux.cfg` в профиле, в UEFI — через `editor 1` в `loader.conf`).

## Рендер на соколе, без 3D

`sokol` — единственная графическая зависимость: `sokol_app` даёт окно и
ввод, `sokol_gfx` выводит кадр. Внутри нет ни матриц, ни камеры, ни моделей:

* теста и записей глубины нет: `.depth = { .write_enabled = false,
  .compare = SG_COMPAREFUNC_ALWAYS }`;
* одна текстура 1280×720, один шейдер, один пайплайн, **4 вершины (64 байта)**;
* кадр: `memcpy` статического слоя + спрайты на CPU → одна
  `sg_update_image()` → `sg_draw(0, 4, 1)`;
* если на экране ничего не движется, кадр не рисуется и текстура не
  обновляется;
* канва хранится в порядке байт RGBA — ровно том, который ждёт
  `SG_PIXELFORMAT_RGBA8`, поэтому загрузка текстуры это `memcpy`.

Стоимость кадра на 1280×720 (один поток, `-O2`): рабочий стол 3,4 мс,
Змейка 1,8 мс, неподвижный рабочий стол 0 мс.

## Сборка

### Программа (любая ОС)

```
sudo apt install build-essential libx11-dev libxi-dev libxcursor-dev libgl1-mesa-dev   # Debian/Ubuntu
./build.sh run
```

macOS и Windows собираются тем же `make app` (на Windows — одна команда `cl`,
она в конце этого файла); `sokol_app` подтягивает Cocoa/OpenGL или
Win32/WGL сам.

### Загрузочный образ (ISO + IMG)

```
./build.sh iso
```

На Arch Linux: нужны `archiso base-devel python libx11 libxi libxcursor mesa`.
На любой другой системе скрипт сам перезапустится в контейнере
`archlinux:latest` (нужен Docker) — именно так это делает CI.

Что происходит:

1. `cc` собирает `dimos` под Arch (X11 + OpenGL);
2. `archiso` собирает live-систему из `iso/packages.x86_64` и `iso/airootfs`,
   попутно переписывая загрузчик в полностью беззвучный режим;
3. бинарник кладётся в `/usr/local/bin/dimos`, `root/.xinitrc` запускает X и
   `openbox-session`, а `~/.config/openbox/autostart` — сам `dimos --fullscreen`;
4. готовый образ копируется в `dist/dimos.iso` и `dist/dimos.img`.

Проверить образ без записи на флешку:

```
qemu-system-x86_64 -m 2048 -enable-kvm -cdrom dist/dimos.iso
```

## Как устроено

| Файл | Роль |
| --- | --- |
| `src/platform_sokol.c` | единственное место про окно: `sokol_app`, `sokol_gfx`, одна текстура, один пайплайн, letterbox и пересчёт координат мыши; флаг `--fullscreen` |
| `src/app.c` | сам DimOS: рабочий стол, панель, часы, статический слой, переходы, ввод |
| `src/snake.c` | Змейка: сетка, еда, рост, ускорение, частицы, оверлеи |
| `src/draw.c` | программный растеризатор: скругления, круги, дуги, тени, свечение и TrueType через stb_truetype |
| `tools/embed_font.py` | вшивает `fonts/font.ttf` в `build/font_data.h` |
| `iso/` | профиль archiso: список пакетов, тихая загрузка, автозапуск X, `dimos --fullscreen`, сборка `dist/dimos.iso` / `dimos.img` |
| `third_party/` | `sokol` (zlib, закреплён на теге `pre-separate-samplers`) и `stb_truetype` (public domain/MIT) |
| `legacy/` | прежняя bare-metal DimOS (загрузчик, VBE, FAT12, iOS-подобная оболочка) — сохранена как история |

Канва всегда 1280×720 (16:9); окно любого размера вписывает её по меньшей
стороне, координаты мыши пересчитываются той же матрицей.

## Проверки в CI

* **gcc (Linux)**: сборка `bin/dimos` и запуск настоящего окна в Xvfb на
  8 секунд — проверяется живой путь `sokol_app` + OpenGL;
* **clang (macOS)** и **MSVC (Windows)**: сборка того же кода;
* **archiso (Arch-контейнер)**: сборка `dist/dimos.iso` + `dist/dimos.img`,
  оба файла уходят артефактом `dimos-x86_64` вместе с `SHA256SUMS.txt`.

## Лицензии

Код DimOS — смотрите `LICENSE`. Сторонние компоненты — `third_party/README.md`,
шрифт — `fonts/LICENSE-Inter.txt`.

## Windows

```
python tools\embed_font.py fonts\font.ttf build\font_data.h
cl /nologo /O2 /Isrc /Ithird_party\sokol /Ithird_party\stb /Ibuild ^
   src\app.c src\snake.c src\draw.c src\platform_sokol.c ^
   /Fe:bin\dimos.exe opengl32.lib user32.lib gdi32.lib shell32.lib ole32.lib
```
