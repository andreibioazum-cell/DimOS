# DimOS Minimal

Минимальная 16-битная система: сразу после загрузки открывается командная строка, а единственное приложение — встроенная игра «Змейка». Старые файловые, графические, звуковые и прикладные функции в образ больше не входят.

## Единственная команда

- `SNAKE` — запустить игру.

В игре используются `WASD` или стрелки. `N` начинает новую игру, `Esc` возвращает в командную строку.

## Сборка

Требуются `nasm`, `dosfstools`, `mtools`, `xorriso`, `g++` и `make`.

```bash
make iso
```

Результаты:

- `disk_img/dimos.img` — загрузочная FAT12-дискета;
- `disk_img/dimos.iso` — загрузочный ISO.

В файловой системе образа намеренно находится только `KERNEL.BIN`: командная строка и «Змейка» встроены прямо в ядро.

## Запуск

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
apt install -y build-essential nasm dosfstools mtools xorriso qemu-system-x86 git
make iso
qemu-system-x86_64 \
  -display vnc=:0 \
  -drive format=raw,file=disk_img/dimos.img,if=floppy,index=0
```

После запуска подключитесь VNC-клиентом к `127.0.0.1:5900`.
