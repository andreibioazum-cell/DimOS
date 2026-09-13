# DimOS на Vulkan

Настоящий Vulkan: загрузчик Khronos, SPIR-V из Khronos glslang, реальный
ICD, `VkInstance` → `VkDevice` → `VkPipeline`. Рендерится **живой кадр
DimOS**, снятый с работающего ядра.

## Почему Vulkan снаружи ядра, а не внутри

Ограничение `KERNEL_LOADER_LIMIT = 43008` в `linker.ld` и строчка «no
paging, no interrupts» в `dimos.h` — это соглашения автора, их можно
снять: переписать `boot.asm`, грузить ядро хоть на 4 МБ. Дело не в них.

Vulkan — это интерфейс **к драйверу**, а не самостоятельная библиотека
рисования. Чтобы `vkCreateDevice` вернул хоть что-то, в системе должен
быть ICD, а ему нужно:

| Требование ICD | Состояние DimOS |
| --- | --- |
| Перечисление PCI, маппинг BAR, загрузка прошивки GPU | шина не сканируется, железо только через `in`/`out` |
| Страничные таблицы, DMA-когерентная память, GART | paging нет, плоский GDT на 4 ГиБ |
| Прерывания, fences, кольцевые буферы команд | IDT нет, `cli` навсегда, чистый polling |
| libc, malloc, потоки | freestanding, ни одной аллокации |

Софтверные ICD (SwiftShader, lavapipe) от этого не спасают: они сами
написаны на C++ поверх libc, malloc и pthreads, а lavapipe тянет ещё и
LLVM. Ни в v86, ни в QEMU нет GPU, для которого существует Vulkan-драйвер
(там Bochs VBE / virtio-gpu). Поэтому GPU стоит там, где GPU и бывает —
снаружи, а ядро остаётся тем, чем было: software renderer в 320×200.

## Что происходит

```
src/kernel/*.c              настоящее ядро, рисует как рисовало
        │                   (mode 13h, байт на пиксель, палитра через 0x3C8)
        ▼
capture.py                  гоняет ядро в эмуляторе, забирает
        │                   64000 байт из 0xA0000 + 768 байт DAC
        ▼
vkviewer.c                  VkInstance → VkPhysicalDevice → VkDevice
        │                   кадр   → VK_FORMAT_R8_UINT, device-local image
        │                   палитра → uniform buffer, 256 × vec4
        ▼
dimos.frag                  палитровая выборка, которую VGA делала
        │                   аппаратно, теперь во фрагментном шейдере
        │                   + скан-линии, апертурная маска, свечение
        │                   люминофора, кривизна трубки, виньетка
        ▼
PNG                         R8G8B8A8 attachment → readback → файл
```

Важная деталь: шейдер **не знает про темы DimOS**. Зелёный и янтарный
монитор получаются сами собой — ядро меняет палитру в DAC, а не пиксели,
и в шейдер уезжает и то и другое.

## Сборка

Нужны заголовки и загрузчик Vulkan, компилятор GLSL и любой ICD.

```
# Debian/Ubuntu
sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers

./tools/vkviewer/build.sh                      # bin/dimos-vkviewer
```

Если Vulkan стоит не в `/usr` (свой LunarG SDK, сборка из исходников):

```
VULKAN_SDK=/path/to/vulkan GLSLANG=/path/to/glslang ./tools/vkviewer/build.sh
```

Без GPU подойдёт софтверный ICD — SwiftShader от Google или lavapipe из
Mesa:

```
VK_ICD_FILENAMES=/path/to/vk_swiftshader_icd.json bin/dimos-vkviewer --list-devices
```

## Запуск

```
# снять кадры с живого ядра (нужны unicorn и Pillow)
python3 tools/vkviewer/capture.py

# отрендерить один из них
bin/dimos-vkviewer \
    --frame   tools/vkviewer/captures/desktop.frame.bin \
    --palette tools/vkviewer/captures/desktop.palette.bin \
    --out     desktop.png --scale 4
```

Сцены `capture.py`: `desktop`, `paint`, `mines`, `terminal`, `amber`.

## Ключи

| Ключ | Что делает | По умолчанию |
| --- | --- | --- |
| `--frame FILE` | 320×200 байт индексов палитры | обязателен |
| `--palette FILE` | 256×3 байта RGB из DAC | обязателен |
| `--out FILE` | куда писать PNG | `dimos-vulkan.png` |
| `--scale N` | кратность к 320×200 | `4` (1280×800) |
| `--width W --height H` | произвольный размер вместо `--scale` | — |
| `--scanline F` | глубина скан-линий | `0.45` |
| `--mask F` | сила апертурной маски | `0.22` |
| `--curvature F` | кривизна трубки | `0.10` |
| `--glow F` | свечение люминофора | `0.14` |
| `--list-devices` | показать устройства Vulkan и выйти | — |

`--scanline 0 --mask 0 --curvature 0 --glow 0` даёт чистый апскейл без
эффектов ЭЛТ.

## Формат файлов

`*.frame.bin` — ровно то, что ядро оставило в видеопамяти: 64000 байт,
строка за строкой, байт на пиксель, значение — индекс палитры.

`*.palette.bin` — 768 байт, 256 троек RGB. Видеокарта хранит по 6 бит на
канал, `capture.py` раскрывает их до 8, и в слотах 32…247 лежит тот самый
куб 6×6×6, которым ядро сглаживает текст.
