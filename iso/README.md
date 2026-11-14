# Загрузочный образ DimOS

Профиль `archiso`, который превращает обычный Arch Linux в DimOS: система
загружается молча, поднимает X и сразу разворачивает `dimos --fullscreen` на
весь экран. На экране нет ничего, кроме DimOS — ни меню загрузчика, ни
логотипа Arch, ни лога ядра, ни консоли, ни логина.

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
| `build-iso.sh` | сборка бинарника, переписывание загрузчика в тихий режим, `mkarchiso`, копирование `dist/dimos.iso` и `dist/dimos.img` |
| `packages.x86_64` | пакеты live-системы: `base`, `linux`, `xorg-server`, `xorg-xinit`, `openbox`, `mesa`, … |
| `airootfs/root/.xinitrc` | `openbox-session` с `LIBGL_ALWAYS_SOFTWARE=1` |
| `airootfs/root/.config/openbox/autostart` | запуск `dimos --fullscreen`; выход из приложения закрывает сеанс |
| `airootfs/root/.bash_profile`, `.zprofile` | `startx` на tty1 и перезапуск сеанса при выходе |
| `airootfs/etc/systemd/system/getty@tty1.service.d/autologin.conf` | молчаливый автологин `root` (`--noissue`) на первой консоли |
| `airootfs/etc/issue`, `etc/motd` | пустые: `agetty` ничего не печатает |
| `airootfs/etc/mkinitcpio.conf.d/dimos.conf` | initramfs на `zstd` и с урезанным набором хуков: распаковка xz на эмулированном CPU идёт минутами |
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

1. BIOS: `vesamenu` сразу переключает экран в графику (это единственный
   способ стереть баннер `ISOLINUX`, который печатается до чтения
   конфигурации), меню скрыто (`MENU HIDDEN`), строка таймаута прозрачная,
   таймаут 0,1 с. UEFI: `systemd-boot` с `timeout 0`, без `beep`, без
   редактора и без лишних пунктов;
2. параметры ядра — `quiet loglevel=3 vt.global_cursor_default=0
   systemd.show_status=false nowatchdog` (они же `%KERNEL_PARAMS%` для обоих
   загрузчиков): ядро и systemd молчат, видно только реальные ошибки.
   `nomodeset` нет намеренно — нужен KMS (в эмуляторе `bochs-drm`, на железе
   `i915`/`amdgpu`/`nouveau`), иначе X останется без драйвера;
3. `getty@tty1` с автологином `root` и `--noissue`, затем `startx` из
   `.bash_profile`;
4. `openbox-session` → `autostart` → `dimos --fullscreen`;
5. DimOS рисует рабочий стол; `Esc` на рабочем столе завершает сеанс, и
   `.bash_profile` тут же поднимает его снова.

Если графический сеанс не поднимется, `.bash_profile` не молчит: он печатает
на консоль `DimOS: the graphical session failed ...` и пробует снова; лог
приложения пишется в `/root/dimos.log`, лог X — в `/var/log/Xorg.0.log`.
Для полного лога загрузки достаточно собрать образ с
`loglevel=7 systemd.show_status=true console=ttyS0,115200` в
`kernel_params_x86_64` (эту переменную задаёт `build-iso.sh`).

## Размер

Образ — обычный Arch Linux с Xorg и Mesa (llvmpipe), поэтому он весит около
560 МиБ: из них большая часть — ядро с модулями и программный OpenGL.
`dimos.iso` и `dimos.img` — одни и те же байты, поэтому артефакт CI с обоими
файлами занимает вдвое больше.
