# DimOS Snake — live-система, в которой одна программа: Змейка

Образ собирается archiso на базе Arch Linux, но вся система — это одна
программа. Загрузка идёт сразу в Змейку на первой консоли (tty1), без меню,
без входа в систему и без рабочего стола. После выхода (клавиша Q) игра
запускается снова.

## Состав

```
plasma/
  README.md                       этот файл
  build-iso.sh                    сборка ISO (запускать на Arch Linux от root)
  snake/snake.c                   Змейка: чистый C (C11, termios, ANSI), без библиотек
  archiso/
    packages.x86_64               минимальный набор пакетов
    airootfs/
      root/.bash_profile          запускает snake на tty1 и перезапускает после выхода
      root/.zprofile              то же для zsh (root в releng использует zsh)
      usr/local/bin/snake         сюда кладётся скомпилированная программа
      etc/vconsole.conf           консольный шрифт Terminus
      etc/mkinitcpio.conf.d/      initramfs сжат xz -9e (самый маленький .img)
```

## Управление

- Стрелки или W A S D — движение.
- R — начать заново после проигрыша.
- Q — выход (игра перезапустится сама).

## Сборка

Программа компилируется перед сборкой образа:

```
cd plasma
gcc -std=c11 -Wall -Wextra -Werror -O2 -o archiso/airootfs/usr/local/bin/snake snake/snake.c
sudo ./build-iso.sh
```

Готовый ISO появится в `plasma/out/`. На GitHub это делает workflow
`Build DimOS Plasma 6.8 ISO (x86_64)` (файл `.github/workflows/plasma-iso.yml`),
артефакт называется `dimos-plasma-x86_64`.

## Честно

- Образ собирается в CI, но **загрузку не проверял**: в песочнице нет QEMU и
  доступа к зеркалам Arch.
- Plasma, браузер, шрифты, обои и приложения из этой ветки убраны. Их история
  остаётся в git (коммиты до этого изменения).
- Змейка текстовая: она работает в консоли, а не в графическом окне.
