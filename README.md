## Запуск в Termux через proot-distro

Десктопный `run-linux.sh` использует GTK + PulseAudio, которых в Termux нет — поэтому окно QEMU выводится через VNC. Нужен [Termux из F-Droid](https://f-droid.org/packages/com.termux/) и любой VNC-клиент на Android (bVNC, RealVNC).

**0. Подготовка образа (один раз)** — баннер при старте убирается так:

```bash
# в корне репозитория
sed -i 's/^LOGO=.*/LOGO=FALSE/' src/kernel/configs/SYSTEM.CFG
```

**1. Termux (хост):**

```bash
pkg update && pkg upgrade -y
pkg install x11-repo          # обязательно — tigervnc лежит в x11-repo, без него 'E: Unable to locate package tigervnc'
pkg install proot-distro tigervnc git
vncpasswd                     # задайте пароль 6–8 символов
vncserver -localhost no -geometry 1024x768 -depth 24 :1
```

**2. Внутри proot-distro (Ubuntu):**

```bash
proot-distro install ubuntu
proot-distro login ubuntu

apt update && apt install -y build-essential nasm dosfstools mtools xorriso qemu-system-x86 git
cd ~
git clone https://github.com/PRoX2011/DimOS.git
cd DimOS
make iso
```

**3. Запуск QEMU (всё ещё внутри proot-distro):**

```bash
mkdir -p lpt
qemu-system-x86_64 \
    -display vnc=127.0.0.1:0 \
    -fda disk_img/dimos.img \
    -machine pcspk-audiodev=snd0 \
    -device adlib,audiodev=snd0 \
    -audiodev none,id=snd0 \
    -drive format=raw,file=disk_img/FLOPPY2.img,if=floppy,index=1 \
    -parallel file:lpt/output.txt
```

**4. VNC-клиент на Android:** `127.0.0.1:5900`, пароль из `vncpasswd`.

Отличия от `run-linux.sh`: `-display gtk` → `-display vnc=…`, `-audiodev pa` → `-audiodev none` (PulseAudio в proot недоступен). KVM в proot нет — QEMU работает через TCG, для DimOS этого хватает.
