#!/usr/bin/env python3
"""Runs the real DimOS kernel here and saves screenshots of the desktop.

This sandbox has no assembler and no emulator installed, so this script does
the next best thing: it compiles the very same src/kernel/*.c files the real
build uses, links them into the flat kernel, and runs that binary on a
simulated PC (unicorn engine + tools/sandbox/hardware_sim.c). Everything the
kernel does - drawing, hit testing, the games, the file manager - is the real
code, not a copy.

Usage:
    python3 tools/sandbox/run_kernel.py            # run the tour, write PNGs
    python3 tools/sandbox/run_kernel.py --build    # only compile and link
"""

import os
import struct
import subprocess
import sys

try:
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP
    from PIL import Image
except ModuleNotFoundError:
    # --build only needs the host compiler and linker. Keep that useful in
    # minimal CI/sandboxes where the optional visual-tour packages are absent.
    Uc = None

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "out")

sys.path.insert(0, HERE)
from font8x8 import font_bytes  # noqa: E402

KERNEL_BASE = 0x00020000
FONT_ADDRESS = 0x0000E000
FRAME_BUFFER = 0x000A0000
VBE_FRAME_BUFFER = 0x00900000
SIMULATOR = 0x00200000
FONT_TTF_AREA = 0x00400000   # the kernel copies FONT.TTF here (font_ttf.c)
RAM_DISK = 0x00500000

CFLAGS = [
    "gcc", "-m32", "-march=i686", "-std=c11", "-Os",
    "-Wall", "-Wextra", "-Wpedantic", "-Werror",
    "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-mno-red-zone", "-mgeneral-regs-only",
    "-I", os.path.join(REPO, "src", "kernel"),
]

# Scan code set 1, make codes, for the keys the tests type.
SCAN_CODES = {
    "h": 0x23, "e": 0x12, "l": 0x26, "p": 0x19, " ": 0x39, "\n": 0x1C,
    "d": 0x20, "i": 0x17, "r": 0x13, "t": 0x14, "y": 0x15, "v": 0x2F,
    "m": 0x32, "s": 0x1F, "1": 0x02, "2": 0x03, "3": 0x04, "\b": 0x0E,
}

ARROW_CODES = {"left": (0x4B, 0xCB), "up": (0x48, 0xC8),
               "down": (0x50, 0xD0), "right": (0x4D, 0xCD)}


def run(command, **kwargs):
    print("  $", " ".join(command))
    return subprocess.run(command, check=True, cwd=REPO, **kwargs)


def build():
    """Compiles the kernel sources and the simulator, links a flat binary."""
    os.makedirs(BUILD, exist_ok=True)
    sources = sorted(
        os.path.join(REPO, "src", "kernel", name)
        for name in os.listdir(os.path.join(REPO, "src", "kernel"))
        if name.endswith(".c")
    )
    sources.append(os.path.join(HERE, "hardware_sim.c"))

    objects = []
    for source in sources:
        obj = os.path.join(BUILD, os.path.basename(source)[:-2] + ".o")
        run(CFLAGS + ["-c", source, "-o", obj], stdout=subprocess.DEVNULL)
        objects.append(obj)

    elf = os.path.join(BUILD, "kernel.elf")
    run(["ld", "-m", "elf_i386", "--build-id=none", "-nostdlib",
         "-e", "kernel_main",
         "-T", os.path.join(REPO, "src", "kernel", "linker.ld"),
         "-Map=" + os.path.join(BUILD, "kernel.map")] + objects + ["-o", elf],
        stdout=subprocess.DEVNULL)
    flat = os.path.join(BUILD, "kernel.bin")
    run(["objcopy", "-O", "binary", elf, flat], stdout=subprocess.DEVNULL)

    size = os.path.getsize(flat)
    # The loader walks the FAT chain into 0x20000; the data window at
    # 0x30000 caps the image at 126 sectors, as everywhere else.
    print(f"  kernel.bin: {size} bytes (loader window is 64512)")
    if size > 64512:
        raise SystemExit("the kernel no longer fits the bootloader's window")
    return flat, elf


def symbols(elf):
    out = subprocess.run(["nm", elf], check=True, capture_output=True,
                         text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


class Machine:
    def __init__(self, flat, table):
        self.symbols = table
        self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        # Protected mode addresses this low physical range directly. It
        # includes the 1 MiB BSS window, simulator block, font workspace and
        # RAM disk.
        self.mu.mem_map(0x00000000, 0x01200000)

        with open(flat, "rb") as handle:
            image = handle.read()
        self.mu.mem_write(KERNEL_BASE, image)

        # What kernel.asm does on a real machine: copy the font out of the
        # video BIOS and remember where it came from.
        self.mu.mem_write(FONT_ADDRESS, font_bytes())
        self.mu.mem_write(table["bios_font_address"],
                          struct.pack("<I", FONT_ADDRESS))

        self.frames = 0
        self.wanted_frames = 0
        self.setup_sent = False
        self.mu.hook_add(UC_HOOK_CODE, self._on_frame,
                         begin=table["gfx_show"], end=table["gfx_show"])
        self.mu.hook_add(UC_HOOK_CODE, self._on_setup,
                         begin=table["gui_setup"], end=table["gui_setup"])

        self.mu.reg_write(UC_X86_REG_ESP, 0x00090000)
        # kernel.asm would have jumped here after entering protected mode.
        self.mu.reg_write(UC_X86_REG_EIP, table["kernel_main"])
        self.pointer_x = 160
        self.pointer_y = 100

    # -- the shared block -------------------------------------------------
    def _read(self, offset, length):
        return self.mu.mem_read(SIMULATOR + offset, length)

    def _write(self, offset, data):
        self.mu.mem_write(SIMULATOR + offset, data)

    def set_clock(self, milliseconds):
        self._write(0, struct.pack("<I", milliseconds))

    def clock(self):
        return struct.unpack("<I", bytes(self._read(0, 4)))[0]

    def palette(self):
        raw = bytes(self._read(0x20C, 256 * 3))
        return [(r * 4, g * 4, b * 4) for r, g, b in
                (tuple(raw[i * 3:i * 3 + 3]) for i in range(256))]

    def restarts(self):
        return struct.unpack("<I", bytes(self._read(0x510, 4)))[0]

    # -- input ------------------------------------------------------------
    def send_keyboard(self, codes):
        count = struct.unpack("<I", bytes(self._read(4, 4)))[0]
        for code in codes:
            self._write(4 + 4 + int(count), bytes([code]))
            count += 1
        self._write(4, struct.pack("<I", count))

    def type_text(self, text):
        for character in text:
            make = SCAN_CODES[character]
            self.send_keyboard([make, make | 0x80])
            self.frames_run(1)

    def send_mouse(self, packets):
        base = 0x108
        count = struct.unpack("<I", bytes(self._read(base, 4)))[0]
        raw = b"".join(bytes(p) for p in packets)
        for index, byte in enumerate(raw):
            self._write(base + 4 + int(count) + index, bytes([byte]))
        self._write(base, struct.pack("<I", count + len(raw)))

    def move_to(self, x, y):
        """Drags the pointer to a position, the way a finger would."""
        while self.pointer_x != x or self.pointer_y != y:
            dx = max(-100, min(100, x - self.pointer_x))
            # The mouse reports "up" as positive, the screen counts down.
            dy = max(-100, min(100, self.pointer_y - y))
            flags = 0x08
            if dx < 0:
                flags |= 0x10
            if dy < 0:
                flags |= 0x20
            self.send_mouse([(flags, dx & 0xFF, dy & 0xFF)])
            self.pointer_x += dx
            self.pointer_y -= dy
            self.frames_run(1)

    def click(self, x, y):
        self.move_to(x, y)
        self.send_mouse([(0x09, 0, 0)])   # left button down
        self.frames_run(1)
        self.send_mouse([(0x08, 0, 0)])   # left button up
        self.frames_run(2)

    def right_click(self, x, y):
        self.move_to(x, y)
        self.send_mouse([(0x0A, 0, 0)])
        self.frames_run(1)
        self.send_mouse([(0x08, 0, 0)])
        self.frames_run(2)

    def drag(self, x0, y0, x1, y1, steps=10):
        """Presses at one point and slides to another with the button down."""
        self.move_to(x0, y0)
        self.send_mouse([(0x09, 0, 0)])
        self.frames_run(1)
        for step in range(1, steps + 1):
            x = x0 + (x1 - x0) * step // steps
            y = y0 + (y1 - y0) * step // steps
            dx = x - self.pointer_x
            dy = self.pointer_y - y
            flags = 0x09
            if dx < 0:
                flags |= 0x10
            if dy < 0:
                flags |= 0x20
            self.send_mouse([(flags, dx & 0xFF, dy & 0xFF)])
            self.pointer_x = x
            self.pointer_y = y
            self.frames_run(1)
        self.send_mouse([(0x08, 0, 0)])
        self.frames_run(2)

    # -- running ----------------------------------------------------------
    def _on_frame(self, mu, address, size, user_data):
        self.frames += 1
        if self.frames >= self.wanted_frames:
            mu.emu_stop()

    def _on_setup(self, mu, address, size, user_data):
        if not self.setup_sent:
            # The real wizard waits for the user. The visual-tour sandbox
            # chooses Computer and accepts its native full-HD default so the
            # existing application tour starts on the first desktop frame.
            self.send_keyboard([0x02, 0x82, 0x1C, 0x9C])
            self.setup_sent = True

    def frames_run(self, count):
        self.wanted_frames = self.frames + count
        self.mu.emu_start(self.mu.reg_read(UC_X86_REG_EIP), 0,
                          count=200_000_000)

    def screenshot(self, name, scale=2):
        # Events are drained one loop pass after they arrive, so let the
        # guest finish the redraw triggered by the last click before the
        # picture is taken.
        self.frames_run(2)
        backend = self.mu.mem_read(self.symbols["video_backend"], 1)[0]
        if backend == 1:
            raw = bytes(self.mu.mem_read(VBE_FRAME_BUFFER, 1920 * 1080 * 4))
            # Little-endian XRGB8888 is byte-ordered B,G,R,X in memory. Pillow's
            # raw decoder avoids a two-million-element Python conversion loop.
            image = Image.frombytes("RGB", (1920, 1080), raw, "raw", "BGRX")
            image = image.resize((960, 540), Image.NEAREST)
        else:
            raw = bytes(self.mu.mem_read(FRAME_BUFFER, 320 * 200))
            colors = self.palette()
            image = Image.new("RGB", (320, 200))
            image.putdata([colors[pixel] for pixel in raw])
            image = image.resize((320 * scale, 200 * scale), Image.NEAREST)
        path = os.path.join(BUILD, name + ".png")
        image.save(path)
        print(f"  saved {path}")
        return path


# ----------------------------------------------------------------------
# Positions on screen, worked out from the layout constants in dimos.h and
# the application sources. Everything the tour clicks is computed, not
# guessed, so a layout change makes the tour miss instead of lie.
# ----------------------------------------------------------------------

# The two files the sandbox disk offers. NOTES.TXT is longer than one 512
# byte cluster on purpose, so opening it also exercises the FAT12 chain.
README_TEXT = (
    "DIMOS 2.0 + DIMXFCE - THE TINY Xfce DESKTOP.\r\n"
    "THE MOUSE AT TOP LEFT HIDES THE WHISKER MENU; A\r\n"
    "RIGHT CLICK ON THE WALLPAPER WORKS TOO. THE ^ KEY\r\n"
    "ROLLS A WINDOW UP, THE DOCK MONITOR HIDES IT ALL.\r\n"
    "CHEESY SERVES CHEESE BALLS WITH KETCHUP. NYAM!\r\n"
)
NOTES_TEXT = (
    "DIMOS BOOTS FROM A 512 BYTE SECTOR THAT LOADS KERNEL.BIN.\r\n"
    "THE KERNEL IS PLAIN C: THE ONLY ASSEMBLY IS THE MODE SWITCH.\r\n"
    "VIDEO IS VGA MODE 13H, 320 BY 200 PIXELS, 256 COLOURS.\r\n"
    "THE 8 BY 8 TEXT FONT COMES FROM THE VIDEO BIOS AT BOOT.\r\n"
    "THIS FILE IS LONGER THAN ONE 512 BYTE CLUSTER, SO READING IT\r\n"
    "FOLLOWS A TWO LINK FAT12 CHAIN: CLUSTER 42 THEN CLUSTER 43.\r\n"
    "DELETE HIDES A FILE UNTIL REBOOT; KERNEL.BIN IS PROTECTED.\r\n"
    "THE RAM DISK AT 0X500000 HOLDS FOUR MEGABYTES OF SCRATCH SPACE.\r\n"
)


def plant_boot_disk(machine, flat):
    """Recreates the windows boot.asm fills on real hardware.

    The file manager reads the FAT copies at 0x7E00, the root directory at
    0x10000 and the first 256 data sectors at 0x30000. On real hardware the
    bootloader puts the disk there; the sandbox has no disk, so this helper
    synthesizes a tiny FAT12 volume with the same layout. When the repo has
    fonts/font.ttf, it ships on the volume as FONT.TTF exactly like the real
    build, so the kernel boots with the rasterized TrueType font."""
    fat = bytearray(18 * 512)

    def set12(cluster, value):
        offset = cluster * 3 // 2
        if cluster % 2 == 0:
            fat[offset] = value & 0xFF
            fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)
        else:
            fat[offset] = (fat[offset] & 0x0F) | ((value & 0x0F) << 4)
            fat[offset + 1] = (value >> 4) & 0xFF

    set12(0, 0xFF0)                    # FAT12 media byte
    set12(1, 0xFFF)                    # end of chain marker
    set12(2, 0xFFF)                    # KERNEL.BIN, first sector only
    set12(40, 0xFFF)                   # README.TXT, one cluster
    set12(42, 43)                      # NOTES.TXT, two clusters
    set12(43, 0xFFF)

    # FONT.TTF occupies a contiguous chain starting at cluster 50, the way
    # mcopy lays it out on a fresh volume.
    font_path = os.path.join(REPO, "fonts", "font.ttf")
    font = b""
    if os.path.isfile(font_path):
        with open(font_path, "rb") as handle:
            font = handle.read()
    font_clusters = (len(font) + 511) // 512
    if 50 + font_clusters > 256 + 2:
        font = b""                     # too big for the preloaded window
        font_clusters = 0
    for index in range(font_clusters):
        last = index == font_clusters - 1
        set12(50 + index, 0xFFF if last else 51 + index)

    machine.mu.mem_write(0x07E00, bytes(fat))

    def entry(name, cluster, size):
        record = bytearray(32)
        record[0:11] = name.encode("ascii")   # 8.3, space padded
        record[11] = 0x20                     # archive attribute
        record[26] = cluster & 0xFF
        record[27] = (cluster >> 8) & 0xFF
        record[28:32] = size.to_bytes(4, "little")
        return bytes(record)

    with open(flat, "rb") as handle:
        kernel = handle.read()
    readme = README_TEXT.encode("ascii")
    notes = NOTES_TEXT.encode("ascii")
    root = bytearray(224 * 32)                # first byte 0x00 = end of dir
    root[0:32] = entry("KERNEL  BIN", 2, len(kernel))
    root[32:64] = entry("README  TXT", 40, len(readme))
    root[64:96] = entry("NOTES   TXT", 42, len(notes))
    if font_clusters:
        root[96:128] = entry("FONT    TTF", 50, len(font))
    machine.mu.mem_write(0x10000, bytes(root))

    data = bytearray(256 * 512)               # cluster N sits at (N-2)*512
    data[0:512] = kernel[:512]
    data[(40 - 2) * 512:(40 - 2) * 512 + len(readme)] = readme
    data[(42 - 2) * 512:(42 - 2) * 512 + 512] = notes[:512]
    data[(43 - 2) * 512:(43 - 2) * 512 + len(notes) - 512] = notes[512:]
    if font_clusters:
        data[(50 - 2) * 512:(50 - 2) * 512 + len(font)] = font
    machine.mu.mem_write(0x30000, bytes(data))

    # The BIOS keeps the conventional memory count at 0x413 in KiB; real
    # firmware writes 640 here, the sandbox has to fake it.
    machine.mu.mem_write(0x0413, (640).to_bytes(2, "little"))


CLOSE_BOX = (304, 23)      # the red xfwm4 X: x=299..310, y=19..27
SHADE_BOX = (280, 23)     # the roll-up button left of minimize
ROLLED_SLAT = (160, 23)   # the rolled-up window's title strip
MENU_BUTTON = (24, 6)     # the little mouse at the panel's left
DOCK_SHRINK = (59, 190)   # the dock's show-desktop cell
DOCK_Y = 190

# The window's inner frame the applications draw into.
WINDOW_LEFT = 9
WINDOW_TOP = 30


def icon_center(index):
    # Desktop icons: two Xfce columns, cells 60x32 starting at (6, 19).
    column = index % 2
    row = index // 2
    return (6 + column * 60 + 28, 19 + row * 32 + 15)


def dock_center(index):
    # The compact glass dock: nine 20 px cells, centred in the pill.
    left = (320 - 9 * 20) // 2
    return (left + index * 20 + 10, DOCK_Y)


def menu_cell(index):
    # The compact launcher grid: three 52 px columns and 35 px rows.
    column = index % 3
    row = index // 3
    return (78 + column * 52 + 24, 55 + row * 35 + 15)


def menu_class_button(index):
    return (74 + 5 + index * 40 + 18, 165)


def calc_key(row, column):
    return (WINDOW_LEFT + 4 + column * 60 + 27, WINDOW_TOP + 26 + row * 25 + 11)


def term_key(row, column):
    return (WINDOW_LEFT + 4 + column * 27 + 12, WINDOW_TOP + 82 + row * 16 + 7)


def tour():
    flat, elf = build()
    table = symbols(elf)
    print("  kernel_main at", hex(table["kernel_main"]))
    machine = Machine(flat, table)
    plant_boot_disk(machine, flat)

    print("\nBooting the DimXfce desktop")
    machine.frames_run(3)
    machine.screenshot("01-desktop")

    print("\nFiles: open the disk listing, then read a file")
    machine.click(*icon_center(0))
    machine.screenshot("02-files")
    machine.click(WINDOW_LEFT + 2 + 108, WINDOW_TOP + 2 + 14 + 7)  # README.TXT
    machine.click(9 + 216 + 8 + 37, WINDOW_TOP + 2 + 7)            # Open
    machine.click(243 + 30, WINDOW_TOP + 38 + 7)   # [+] a page down
    machine.screenshot("03-file-viewer")
    machine.click(243 + 30, WINDOW_TOP + 56 + 7)   # [-] a page back
    machine.click(*CLOSE_BOX)

    print("\nSnake: steer with the on-screen pad")
    machine.click(*icon_center(1))
    machine.screenshot("04-snake")
    machine.click(229, 71)                   # up
    machine.frames_run(14)
    machine.click(199, 95)                   # left
    machine.frames_run(14)
    machine.screenshot("05-snake-moving")
    machine.click(*CLOSE_BOX)

    print("\nMines: open a square")
    machine.click(*icon_center(2))
    machine.click(76, 97)
    machine.screenshot("06-mines")
    machine.right_click(13 + 8 * 14 + 7, WINDOW_TOP + 4 + 8 * 14 + 7)
    machine.screenshot("07-mines-flag")
    machine.click(*CLOSE_BOX)

    print("\nPaint: drag a line")
    machine.click(*icon_center(3))
    machine.drag(30, 52, 180, 122)
    machine.screenshot("08-paint")
    machine.click(*CLOSE_BOX)

    print("\nCalculator: 7 + 8 =")
    machine.click(*icon_center(4))
    machine.click(*calc_key(0, 0))   # 7
    machine.click(*calc_key(3, 3))   # +
    machine.click(*calc_key(0, 1))   # 8
    machine.click(*calc_key(3, 2))   # =
    machine.screenshot("09-calculator")
    machine.click(*CLOSE_BOX)

    print("\nMusic, with the black record title bar")
    machine.click(*icon_center(5))
    machine.click(9 + 4 + 61, WINDOW_TOP + 9)  # the Chime button
    machine.frames_run(6)
    machine.screenshot("10-music")
    machine.click(*CLOSE_BOX)

    print("\nCheesy Balls: the mouse's own kitchen")
    machine.click(*icon_center(6))
    machine.click(63, WINDOW_TOP + 110)    # NYAM!
    machine.click(63, WINDOW_TOP + 110)    # NYAM!
    machine.screenshot("11-cheesy")
    machine.click(257, WINDOW_TOP + 110)   # Cook more for later
    machine.click(*CLOSE_BOX)

    print("\nAbout, then the green phosphor theme")
    machine.click(*icon_center(7))
    machine.screenshot("12-about")
    machine.click(13 + 6 + 45, 148 + 7)    # the Theme button
    machine.screenshot("13-about-green")
    machine.click(13 + 6 + 45, 148 + 7)    # and back to the colour theme
    machine.click(13 + 6 + 45, 148 + 7)
    machine.click(13 + 6 + 45, 148 + 7)
    machine.click(*CLOSE_BOX)

    print("\nTerminal: type DIR on the on-screen keyboard")
    machine.click(*icon_center(8))
    machine.screenshot("14-terminal")
    machine.click(*term_key(2, 2))   # D
    machine.click(*term_key(1, 7))   # I
    machine.click(*term_key(1, 3))   # R
    machine.click(*term_key(2, 10))  # ENT
    machine.frames_run(2)
    machine.screenshot("15-terminal-dir")

    print("\nKeyboard only: Escape closes the window")
    machine.send_keyboard([0x01, 0x81])   # make and break of Escape
    machine.frames_run(2)
    machine.screenshot("16-back-to-desktop")

    print("\nKeyboard only: arrows move the focus, Enter opens")
    machine.send_keyboard(list(ARROW_CODES["left"]))
    machine.frames_run(1)
    machine.send_keyboard(list(ARROW_CODES["up"]))
    machine.frames_run(1)
    machine.send_keyboard([0x1C, 0x9C])   # Enter opens the focused icon
    machine.frames_run(2)
    machine.screenshot("17-keyboard-open")
    machine.send_keyboard([0x01, 0x81])
    machine.frames_run(2)

    print("\nWhisker menu: the mouse button, the Games filter")
    machine.click(*MENU_BUTTON)
    machine.screenshot("18-whisker-menu")
    machine.click(*menu_class_button(1))   # Games
    machine.screenshot("19-whisker-games")
    machine.click(*menu_cell(6))           # Cheesy Balls from the menu
    machine.screenshot("20-menu-launched")

    print("\nxfwm4 tricks: roll the window up, then unroll it")
    machine.click(*SHADE_BOX)
    machine.screenshot("21-rolled-up")
    machine.click(*ROLLED_SLAT)

    print("\nThe dock breastfeeding: Calc from plank, minimize to desktop")
    machine.click(*dock_center(4))
    machine.screenshot("22-dock-launched")
    machine.click(*DOCK_SHRINK)            # show desktop
    machine.screenshot("23-show-desktop")
    machine.click(*DOCK_SHRINK)            # and bring Calc back
    machine.screenshot("24-restored")
    machine.send_keyboard([0x01, 0x81])
    machine.frames_run(2)

    print("\nRight click on the wallpaper opens the menu, xfdesktop style")
    machine.right_click(200, 60)
    machine.screenshot("25-right-click-menu")
    machine.send_keyboard([0x01, 0x81])
    machine.frames_run(2)

    clock = machine.clock()
    print(f"\nSimulated clock after the tour: {clock} ms")
    print(f"Restart requests: {machine.restarts()}")


def main():
    if "--build" in sys.argv:
        build()
        return
    if Uc is None:
        raise SystemExit("visual tour needs: pip install unicorn pillow")
    tour()


if __name__ == "__main__":
    main()
