# Загрузочный образ DimOS

Профиль `archiso`, который превращает обычный Arch Linux в DimOS: система
загружается, поднимает X, запускает `openbox` и сразу разворачивает
`dimos --fullscreen --skip-boot` на весь экран. Пользователь видит только
DimOS — лог загрузки ядра и рабочий стол с одной иконкой (Змейка).

```
./iso/build-iso.sh          # или ./build.sh iso, или make iso
ls dist/                    # dimos.iso  dimos.img
```

Скрипт работает в два режима:

* на Arch Linux с установленным `archiso` — собирает напрямую;
* на любой другой системе — перезапускает себя в контейнере
  `archlinux:latest` (нужен Docker). Именно так собирает CI.

## Что внутри

| Путь | Роль |
| --- | --- |
| `build-iso.sh` | сборка бинарника, `mkarchiso`, копирование `dist/dimos.iso` и `dist/dimos.img` (контрольные суммы считает CI) |
| `packages.x86_64` | пакеты live-системы: `base`, `linux`, `xorg-server`, `xorg-xinit`, `openbox`, `mesa`, … |
| `airootfs/root/.xinitrc` | `openbox-session` с `LIBGL_ALWAYS_SOFTWARE=1` |
| `airootfs/root/.config/openbox/autostart` | запуск `dimos --fullscreen --skip-boot` |
| `airootfs/root/.config/openbox/rc.xml` | пустой менеджер без рамок, `Esc` не убивает сеанс |
| `airootfs/root/.bash_profile`, `.zprofile` | `startx` на tty1 и перезапуск сеанса при выходе |
| `airootfs/etc/systemd/system/getty@tty1.service.d/autologin.conf` | автологин `root` на первой консоли |
| `airootfs/etc/mkinitcpio.conf.d/dimos.conf` | initramfs с `xz -9e` (меньше образ) |
| `airootfs/etc/vconsole.conf` | латинская раскладка, крупный шрифт |

`dimos.img` — это побайтово тот же гибридный ISO (BIOS + UEFI), поэтому его
можно писать на флешку как сырой образ:

```
sudo dd if=dist/dimos.img of=/dev/sdX bs=4M status=progress oflag=sync
```

Проверка без записи:

```
qemu-system-x86_64 -m 2048 -enable-kvm -cdrom dist/dimos.iso
```

## Как это загружается

1. `systemd-boot` (UEFI) или `syslinux` (BIOS) с таймаутом 0 — меню не видно;
2. ядро и `initramfs`, `nomodeset` + `console=tty0` — лог идёт на экран, как
   у обычного Arch;
3. `getty@tty1` с автологином `root`, `startx` из `.bash_profile`;
4. `openbox-session` → `autostart` → `dimos --fullscreen --skip-boot`;
5. DimOS рисует рабочий стол; `Esc` на рабочем столе завершает сеанс, и
   `.bash_profile` тут же поднимает его снова (система не «зависает» в
   голой консоли).

Никакие PNG-кадры, скриншоты и отладочные артефакты при сборке не создаются:
на выходе только `dimos.iso` и `dimos.img`.
