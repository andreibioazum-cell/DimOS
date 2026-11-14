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
    "-ffunction-sections", "-fdata-sections",
    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables", "-mno-red-zone", "-mgeneral-regs-only",
    "-DDIMOS_SANDBOX", "-I", os.path.join(REPO, "src", "kernel"),
]

# Scan code set 1, make codes, for the keys the tests type.
SCAN_CODES = {
    "a": 0x1E, "b": 0x30, "c": 0x2E, "d": 0x20, "e": 0x12, "f": 0x21,
    "g": 0x22, "h": 0x23, "i": 0x17, "j": 0x24, "k": 0x25, "l": 0x26,
    "m": 0x32, "n": 0x31, "o": 0x18, "p": 0x19, "q": 0x10, "r": 0x13,
    "s": 0x1F, "t": 0x14, "u": 0x16, "v": 0x2F, "w": 0x11, "x": 0x2D,
    "y": 0x15, "z": 0x2C, ".": 0x34, "1": 0x02, "2": 0x03, "3": 0x04,
    " ": 0x39, "\n": 0x1C, "\b": 0x0E,
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
    run(["ld", "-m", "elf_i386", "--build-id=none", "-nostdlib", "--gc-sections",
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
    def __init__(self, flat, table, display=None):
        self.symbols = table
        self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        # Protected mode addresses this low physical range directly. It
        # includes the 1 MiB BSS window, simulator block, font workspace,
        # RAM disk and a framebuffer big enough for 4K.
        self.mu.mem_map(0x00000000, 0x04000000)

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
        self.at_boundary = False
        self.frame_hook = self._add_frame_hook()

        self.mu.reg_write(UC_X86_REG_ESP, 0x00090000)
        # kernel.asm would have jumped here after entering protected mode.
        self.mu.reg_write(UC_X86_REG_EIP, table["kernel_main"])
        self.pointer_x = 160
        self.pointer_y = 100
        self.display = display
        if display is not None:
            self.set_display(*display)

    def set_display(self, width, height):
        """Pretends the panel is another resolution (a 1.5K phone, say)."""
        self.mu.mem_write(self.symbols["video_width"], struct.pack("<H", width))
        self.mu.mem_write(self.symbols["video_height"], struct.pack("<H", height))
        self.mu.mem_write(self.symbols["video_pitch"], struct.pack("<H", width * 4))
        self.mu.mem_write(self.symbols["video_framebuffer_address"],
                          struct.pack("<I", VBE_FRAME_BUFFER))

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

    def debug_text(self):
        """What the kernel wrote to the Bochs/QEMU debug port."""
        count = struct.unpack("<I", bytes(self._read(0x514, 4)))[0]
        return bytes(self._read(0x518, min(count, 256))).decode("ascii", "replace")

    def byte(self, symbol):
        return self.mu.mem_read(self.symbols[symbol], 1)[0]

    def memory_block(self, address, length):
        return bytes(self.mu.mem_read(address, length))

    # -- input ------------------------------------------------------------
    def send_keyboard(self, codes):
        count = struct.unpack("<I", bytes(self._read(4, 4)))[0]
        for code in codes:
            self._write(4 + 4 + int(count), bytes([code]))
            count += 1
        self._write(4, struct.pack("<I", count))

    def type_text(self, text):
        for character in text:
            make = SCAN_CODES[character.lower()]
            self.send_keyboard([make, make | 0x80])
            self.frames_run(1)

    def send_mouse(self, packets):
        base = 0x108
        count = struct.unpack("<I", bytes(self._read(base, 4)))[0]
        raw = b"".join(bytes(p) for p in packets)
        for index, byte in enumerate(raw):
            self._write(base + 4 + int(count) + index, bytes([byte]))
        self._write(base, struct.pack("<I", count + len(raw)))

    def mouse_pending(self):
        return struct.unpack("<I", bytes(self._read(0x108, 4)))[0]

    def drain_mouse(self):
        """Runs frames until the guest has eaten every queued mouse byte.

        A packet is three bytes and the kernel drains the controller once
        per frame, so a burst of moves can leave bytes queued; sending a
        button packet before they are gone would put the press behind the
        motion and the click would land at the old place.
        """
        for _ in range(40):
            if self.mouse_pending() == 0:
                break
            self.frames_run(1)
        return self.mouse_pending()

    def kernel_pointer(self):
        """Where the guest thinks the pointer is, straight out of its memory.

        A PS/2 driver resynchronises by itself when a byte goes missing, so
        the honest way to steer it is to read the position back after every
        packet and correct from there instead of trusting a mirror kept here.
        """
        x = struct.unpack("<h", bytes(self.mu.mem_read(self.symbols["pointer_x"], 2)))[0]
        y = struct.unpack("<h", bytes(self.mu.mem_read(self.symbols["pointer_y"], 2)))[0]
        return x, y

    def move_to(self, x, y):
        """Drags the pointer to a position, the way a finger would.

        Packets are batched: the controller holds them in a queue and the
        kernel reads 64 bytes per frame, so twenty packets per frame moves
        the pointer just as accurately as one, far faster.
        """
        for _ in range(12):
            pointer_x, pointer_y = self.kernel_pointer()
            if pointer_x == x and pointer_y == y:
                break
            packets = []
            for _ in range(20):
                dx = max(-100, min(100, x - pointer_x))
                # The mouse reports "up" as positive, the screen counts down.
                dy = max(-100, min(100, pointer_y - y))
                if dx == 0 and dy == 0:
                    break
                flags = 0x08
                if dx < 0:
                    flags |= 0x10
                if dy < 0:
                    flags |= 0x20
                packets.append((flags, dx & 0xFF, dy & 0xFF))
                pointer_x += dx
                pointer_y -= dy
            if not packets:
                break
            self.send_mouse(packets)
            self.frames_run(1)
        self.drain_mouse()
        self.pointer_x, self.pointer_y = self.kernel_pointer()

    def click(self, x, y):
        self.move_to(x, y)
        self.send_mouse([(0x09, 0, 0)])   # left button down
        self.drain_mouse()
        self.send_mouse([(0x08, 0, 0)])   # left button up
        self.drain_mouse()
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
        self.drain_mouse()
        for step in range(1, steps + 1):
            x = x0 + (x1 - x0) * step // steps
            y = y0 + (y1 - y0) * step // steps
            pointer_x, pointer_y = self.kernel_pointer()
            dx = x - pointer_x
            dy = pointer_y - y
            flags = 0x09
            if dx < 0:
                flags |= 0x10
            if dy < 0:
                flags |= 0x20
            self.send_mouse([(flags, dx & 0xFF, dy & 0xFF)])
            self.pointer_x = x
            self.pointer_y = y
            self.drain_mouse()
        self.send_mouse([(0x08, 0, 0)])
        self.drain_mouse()
        self.frames_run(2)

    # -- running ----------------------------------------------------------
    def _add_frame_hook(self):
        return self.mu.hook_add(UC_HOOK_CODE, self._on_frame,
                                begin=self.symbols["gfx_show"],
                                end=self.symbols["gfx_show"])

    def _on_frame(self, mu, address, size, user_data):
        self.frames += 1
        self.at_boundary = True
        if self.frames >= self.wanted_frames:
            mu.emu_stop()

    def frames_run(self, count):
        """Runs `count` complete frames.

        The hook fires just before gfx_show() executes, so an emulator
        stopped there is still sitting on the entry instruction. Without
        stepping over it first, the next emu_start() would hit the same
        address, count it as a frame and stop again -- which is why a
        frames_run(1) used to make no progress at all.
        """
        self.wanted_frames = self.frames + count
        if self.at_boundary:
            self.mu.hook_del(self.frame_hook)
            self.mu.emu_start(self.mu.reg_read(UC_X86_REG_EIP), 0, count=1)
            self.frame_hook = self._add_frame_hook()
            self.at_boundary = False
        if self.frames < self.wanted_frames:
            self.mu.emu_start(self.mu.reg_read(UC_X86_REG_EIP), 0,
                              count=200_000_000)

    def screenshot(self, name, scale=2):
        # Events are drained one loop pass after they arrive, so let the
        # guest finish the redraw triggered by the last click before the
        # picture is taken.
        self.frames_run(2)
        backend = self.mu.mem_read(self.symbols["video_backend"], 1)[0]
        if backend == 1:
            width = struct.unpack("<H", bytes(self.mu.mem_read(
                self.symbols["video_width"], 2)))[0]
            height = struct.unpack("<H", bytes(self.mu.mem_read(
                self.symbols["video_height"], 2)))[0]
            pitch = struct.unpack("<H", bytes(self.mu.mem_read(
                self.symbols["video_pitch"], 2)))[0]
            raw = bytes(self.mu.mem_read(VBE_FRAME_BUFFER, pitch * height))
            # Little-endian XRGB8888 is byte-ordered B,G,R,X in memory. Pillow's
            # raw decoder avoids a two-million-element Python conversion loop.
            image = Image.frombytes("RGB", (pitch // 4, height), raw, "raw", "BGRX")
            if pitch // 4 != width:
                image = image.crop((0, 0, width, height))
            # The shell fits its 844x390 phone screen into the display and
            # letterboxes the rest; crop straight to the phone for the tour.
            screen_width, screen_height = 844, 390
            scale = min(width / screen_width, height / screen_height)
            render_width = int(screen_width * scale)
            render_height = int(screen_height * scale)
            left = (width - render_width) // 2
            top = (height - render_height) // 2
            image = image.crop((left, top, left + render_width,
                                top + render_height))
            image = image.resize((screen_width * 2, screen_height * 2),
                                 Image.LANCZOS)
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
    "DIMOS 14 - A SMALL PHONE SHELL FOR A PC.\\r\\n"
    "TAP AN ICON TO OPEN AN APP; THE BAR AT THE BOTTOM\\r\\n"
    "IS THE HOME GESTURE. THE STATUS BAR OPENS CONTROL\\r\\n"
    "CENTRE, WITH TOGGLES, SLIDERS AND THE LOCK BUTTON.\\r\\n"
    "CHEESY SERVES CHEESE BALLS WITH KETCHUP. NYAM!\\r\\n"
)
NOTES_TEXT = (
    "DIMOS BOOTS FROM A 512 BYTE SECTOR THAT LOADS KERNEL.BIN.\\r\\n"
    "C DRAWS THE SHELL; ASSEMBLY HANDLES BIOS AND PIT IRQ0.\\r\\n"
    "THE 844 BY 390 CANVAS IS AN IPHONE LANDSCAPE SCREEN,\\r\\n"
    "SCALED INTO ANY VBE MODE WITHOUT STRETCHING IT.\\r\\n"
    "TTF TEXT IS ANTIALIASED AT EVERY SIZE THE SHELL ASKS FOR.\\r\\n"
    "THIS FILE IS LONGER THAN ONE 512 BYTE CLUSTER, SO READING\\r\\n"
    "IT FOLLOWS A TWO LINK FAT12 CHAIN: CLUSTER 42 THEN 43.\\r\\n"
    "DELETE HIDES A FILE UNTIL REBOOT; KERNEL.BIN IS PROTECTED.\\r\\n"
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


# ----------------------------------------------------------------------
# The IDE disk. The simulated controller keeps its disk in the last megabyte
# of the sandbox's address space, so this is the exact byte the kernel reads
# and writes; a second Machine handed the same bytes is a reboot.
# ----------------------------------------------------------------------

DISK_ADDRESS = 0x01100000
DISK_BYTES = 1024 * 1024


def disk_image(files):
    """Builds a FAT12 volume, the way another system would hand one to DimOS.

    `files` is a list of (name, bytes). Geometry: 512-byte sectors, one
    sector per cluster, two 8-sector FATs and a 224-entry root directory.
    """
    total = DISK_BYTES // 512
    root_entries, fat_sectors = 224, 8
    root_start = 1 + 2 * fat_sectors
    data_start = root_start + root_entries * 32 // 512
    image = bytearray(DISK_BYTES)

    def write(offset, raw):
        image[offset:offset + len(raw)] = raw

    def word(offset, value):
        write(offset, struct.pack("<H", value))

    def long_(offset, value):
        write(offset, struct.pack("<I", value))

    write(0, b"\xEB\x3C\x90DIMOS14 ")
    word(11, 512)
    image[13] = 1                     # sectors per cluster
    word(14, 1)                       # reserved sectors
    image[16] = 2                     # two FATs
    word(17, root_entries)
    word(19, total)
    image[21] = 0xF0
    word(22, fat_sectors)
    word(24, 18)
    word(26, 2)
    image[38] = 0x29
    long_(39, 0x44494D4F)
    write(43, b"DIMOS 14   ")
    write(54, b"FAT12   ")
    write(510, b"\x55\xAA")

    fat = bytearray(fat_sectors * 512)
    fat[0:3] = b"\xF0\xFF\xFF"
    cluster = 2
    directory = bytearray(root_entries * 32)
    payload = bytearray(512 * 512)

    for slot, (name, data) in enumerate(files):
        needed = max(1, (len(data) + 511) // 512)
        for index in range(needed):
            value = cluster + index
            link = 0xFFF if index == needed - 1 else value + 1
            offset = value * 3 // 2
            if value % 2 == 0:
                fat[offset] = link & 0xFF
                fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((link >> 8) & 0x0F)
            else:
                fat[offset] = (fat[offset] & 0x0F) | ((link & 0x0F) << 4)
                fat[offset + 1] = (link >> 4) & 0xFF
        start = (cluster - 2) * 512
        payload[start:start + len(data)] = data
        record = bytearray(32)
        base, _, extension = name.partition(".")
        record[0:11] = (base.ljust(8) + extension.ljust(3)).encode("ascii")
        record[11] = 0x20
        record[26] = cluster & 0xFF
        record[27] = (cluster >> 8) & 0xFF
        record[28:32] = len(data).to_bytes(4, "little")
        directory[slot * 32:(slot + 1) * 32] = record
        cluster += needed

    for copy in range(2):
        write((1 + copy * fat_sectors) * 512, bytes(fat))
    write(root_start * 512, bytes(directory))
    write(data_start * 512, bytes(payload))
    return image


def config_record(theme, brightness, volume):
    """CONFIG.CFG exactly as config.c writes it."""
    record = bytearray(16)
    record[0:4] = struct.pack("<I", 0x534F4D44)
    record[4] = 1
    record[5] = theme
    record[6] = brightness
    record[7] = volume
    checksum = 0xA5
    for byte in record[:15]:
        checksum ^= byte
        checksum = ((checksum << 1) | (checksum >> 7)) & 0xFF
    record[15] = checksum
    return bytes(record)


# ----------------------------------------------------------------------
# Positions on screen, worked out from the layout constants in dimos.h and
# the application sources. Everything the tour taps is computed, not
# guessed, so a layout change makes the tour miss instead of lie.
# ----------------------------------------------------------------------

SCREEN_WIDTH = 844
SCREEN_HEIGHT = 390
ICON_GRID_LEFT = (SCREEN_WIDTH - 4 * 112 - 64) // 2


def home_icon(index):
    column = index % 5
    row = index // 5
    return (ICON_GRID_LEFT + column * 112 + 32, 46 + row * 108 + 32)


def dock_icon(index):
    return ((SCREEN_WIDTH - 348) // 2 + 26 + index * 80 + 26, 337)


def calc_key(row, column):
    left = (SCREEN_WIDTH - (4 * 96 + 12)) // 2
    return (left + column * 96 + 42, 146 + row * 56 + 23)


def term_key(row, column):
    return (37 + column * 70 + 32, 226 + row * 34 + 15)


HOME_BAR = (SCREEN_WIDTH // 2, 381)
STATUS_RIGHT = (SCREEN_WIDTH - 40, 10)
CC_WIFI = (26 + 78 + 86, 78)
CC_LOCK = (26 + 566 + 75, 196 + 13)


def tour():
    flat, elf = build()
    table = symbols(elf)
    display = None
    if "--display" in sys.argv:
        position = sys.argv.index("--display")
        width, height = sys.argv[position + 1].lower().split("x")
        display = (int(width), int(height))
    print("  kernel_main at", hex(table["kernel_main"]))
    machine = Machine(flat, table, display=display)
    plant_boot_disk(machine, flat)

    print("\nBoot: the lock screen")
    machine.frames_run(3)
    machine.screenshot("01-lock-screen")

    print("\nTap to unlock: the home screen")
    machine.click(*HOME_BAR)
    machine.screenshot("02-home-screen")

    print("\nControl Centre from the status bar")
    machine.click(*STATUS_RIGHT)
    machine.screenshot("03-control-centre")
    machine.click(*CC_WIFI)
    machine.screenshot("04-control-centre-wifi")
    machine.click(760, 320)
    machine.frames_run(2)

    print("\nFiles: the disk listing, then a text file")
    machine.click(*home_icon(0))
    machine.screenshot("05-files")
    machine.click(150, 92)
    machine.click(694, 96)
    machine.screenshot("06-file-viewer")
    machine.click(*HOME_BAR)

    print("\nMusic: a phone player with live progress")
    machine.click(*home_icon(1))
    machine.screenshot("07-music")
    machine.click(404, 224)
    machine.frames_run(30)
    machine.screenshot("08-music-playing")
    machine.click(*HOME_BAR)

    print("\nCalculator: 7 + 8 =")
    machine.click(*home_icon(2))
    machine.click(*calc_key(0, 0))
    machine.click(*calc_key(3, 3))
    machine.click(*calc_key(0, 1))
    machine.click(*calc_key(3, 2))
    machine.screenshot("09-calculator")
    machine.click(*HOME_BAR)

    print("\nPaint: drag a stroke")
    machine.click(*home_icon(3))
    machine.drag(120, 160, 420, 280)
    machine.drag(420, 280, 300, 120)
    machine.screenshot("10-paint")
    machine.click(*HOME_BAR)

    print("\nSnake: steer with the on-screen pad")
    machine.click(*home_icon(4))
    machine.screenshot("11-snake")
    machine.click(492, 175)
    machine.frames_run(20)
    machine.click(424, 233)
    machine.frames_run(20)
    machine.screenshot("12-snake-moving")
    machine.click(*HOME_BAR)

    print("\nMines: open a square and flag another")
    machine.click(*home_icon(5))
    machine.click(39, 87)
    machine.right_click(117, 165)
    machine.screenshot("13-mines")
    machine.click(*HOME_BAR)

    print("\nCheesy Balls: two chomps")
    machine.click(*home_icon(6))
    machine.click(620, 228)
    machine.click(620, 228)
    machine.frames_run(6)
    machine.screenshot("14-cheesy")
    machine.click(*HOME_BAR)

    print("\nTerminal: type DIR on the iOS-style keyboard")
    machine.click(*home_icon(7))
    machine.screenshot("15-terminal")
    machine.click(*term_key(2, 2))   # D
    machine.click(*term_key(1, 7))   # I
    machine.click(*term_key(1, 3))   # R
    machine.click(*term_key(2, 10))  # ENT
    machine.frames_run(2)
    machine.screenshot("16-terminal-dir")
    machine.click(*HOME_BAR)

    print("\nSettings, and the theme cycle")
    machine.click(*home_icon(8))
    machine.screenshot("17-settings")
    machine.click(644, 136)
    machine.screenshot("18-settings-green")
    machine.click(644, 136)
    machine.click(644, 136)
    machine.click(644, 136)
    machine.click(*HOME_BAR)
    machine.frames_run(2)

    print("\nLock the phone from Control Centre")
    machine.click(*STATUS_RIGHT)
    machine.screenshot("19-control-centre-again")
    machine.click(*CC_LOCK)
    machine.screenshot("20-locked")
    machine.click(*HOME_BAR)
    machine.frames_run(2)

    print("\nThe dock launches Calculator again")
    machine.click(*dock_icon(2))
    machine.screenshot("21-dock-calculator")
    machine.send_keyboard([0x01, 0x81])   # Escape is the home gesture
    machine.frames_run(2)
    machine.screenshot("22-home-again")

    clock = machine.clock()
    print(f"\nSimulated clock after the tour: {clock} ms")
    print(f"Restart requests: {machine.restarts()}")


def terminal_history(machine):
    """Every row of the terminal's scrollback, as text."""
    rows = 40
    columns = 61
    raw = machine.memory_block(machine.symbols["history"], rows * columns)
    return "\n".join(raw[row * columns:(row + 1) * columns]
                     .split(b"\0")[0].decode("ascii", "replace")
                     for row in range(rows))


def type_command(machine, text):
    machine.type_text(text + "\n")
    machine.frames_run(3)


def disk_test():
    """Boots on a real FAT12 volume, saves a file, reboots, reads it back.

    This is the test that says DimOS is an operating system and not a
    slideshow: the kernel has to parse another system's file system, write
    new sectors through the IDE controller, and survive a power cycle with
    both the file and its saved settings intact.
    """
    flat, elf = build()
    table = symbols(elf)

    print("\nDisk: a FAT12 volume written by another system")
    planted = disk_image([("CONFIG.CFG", config_record(1, 7, 11))])
    machine = Machine(flat, table)
    plant_boot_disk(machine, flat)
    machine.mu.mem_write(DISK_ADDRESS, bytes(planted))
    machine.frames_run(3)

    markers = machine.debug_text()
    assert "DIMOS:DISK:VOLUME" in markers, markers
    assert "DIMOS:READY:VBE" in markers, markers
    assert machine.byte("settings_valid") == 1, "CONFIG.CFG was not read"
    assert machine.byte("settings_theme") == 1, "the saved theme was ignored"
    assert machine.byte("settings_brightness") == 7
    print("  mounted the volume, applied the saved theme and brightness")
    machine.screenshot("23-disk-saved-settings")

    print("\nTerminal: write a file through the IDE controller")
    machine.click(*HOME_BAR)
    machine.click(*home_icon(7))
    type_command(machine, "SAVE HELLO.TXT WRITTEN BY DIMOS")
    machine.screenshot("24-terminal-save")
    assert "Saved" in terminal_history(machine), terminal_history(machine)

    written = machine.memory_block(DISK_ADDRESS, DISK_BYTES)
    assert b"HELLO   TXT" in written, "no directory entry for HELLO.TXT"
    assert b"written by dimos" in written.lower(), \
        "the file data never reached the disk"
    print("  the directory entry and the data are on the disk")

    print("\nReboot: the same disk, a fresh kernel")
    second = Machine(flat, table)
    plant_boot_disk(second, flat)
    second.mu.mem_write(DISK_ADDRESS, written)
    second.frames_run(3)
    assert second.byte("settings_valid") == 1, "settings did not survive a reboot"
    assert second.byte("settings_theme") == 1, "the theme did not survive a reboot"
    assert second.byte("settings_brightness") == 7
    second.screenshot("25-reboot-lock")
    second.click(*HOME_BAR)
    second.click(*home_icon(7))
    type_command(second, "DIR")
    type_command(second, "LOAD HELLO.TXT")
    history = terminal_history(second)
    assert "HELLO.TXT" in history, history
    assert "written by dimos" in history.lower(), history
    second.screenshot("26-reboot-load")
    print("  the file and the settings were still there")

    print("\nFormat: a blank disk gets a volume from the kernel itself")
    blank = Machine(flat, table)
    plant_boot_disk(blank, flat)
    blank.mu.mem_write(DISK_ADDRESS, bytes(DISK_BYTES))
    blank.frames_run(3)
    assert "DIMOS:DISK:BLANK" in blank.debug_text(), blank.debug_text()
    blank.click(*HOME_BAR)
    blank.click(*home_icon(7))
    type_command(blank, "FORMAT YES")
    assert "Formatted" in terminal_history(blank), terminal_history(blank)
    formatted = blank.memory_block(DISK_ADDRESS, 512)
    assert formatted[54:62] == b"FAT12   ", formatted[54:62]
    assert formatted[510:512] == b"\x55\xAA"
    blank.screenshot("27-format")
    print("  wrote a fresh BPB, both FATs and an empty root directory")


def display_test(width, height):
    """Boots on a pretend panel of a given size and screenshots the result."""
    flat, elf = build()
    machine = Machine(flat, symbols(elf), display=(width, height))
    plant_boot_disk(machine, flat)
    machine.frames_run(3)
    machine.screenshot(f"display-{width}x{height}")
    machine.click(*HOME_BAR)
    machine.frames_run(4)
    machine.screenshot(f"display-{width}x{height}-home")

    # The canvas has to be centred and letterboxed, never stretched.
    margin_x = (width - min(width, height * 844 // 390)) // 2
    print(f"  {width}x{height}: canvas scaled to "
          f"{min(width, height * 844 // 390)} px wide, "
          f"{margin_x} px letterbox on each side")


def smoke(frames):
    """Boots the kernel and only checks that it survives a few frames."""
    flat, elf = build()
    # 1920x1080 unless asked otherwise: this test only has to prove the
    # kernel survives, and the 1.5K panel has its own run below.
    display = None
    if "--display" in sys.argv:
        position = sys.argv.index("--display")
        width, height = sys.argv[position + 1].lower().split("x")
        display = (int(width), int(height))
    machine = Machine(flat, symbols(elf), display=display)
    plant_boot_disk(machine, flat)
    machine.frames_run(frames)
    print(f"Smoke test: {frames} frames, clock {machine.clock()} ms, "
          f"restarts {machine.restarts()}")
    if machine.restarts() != 0:
        raise SystemExit("the kernel asked the machine to restart")
    machine.screenshot("00-smoke")


def main():
    if "--build" in sys.argv:
        build()
        return
    if Uc is None:
        raise SystemExit("the sandbox needs: pip install unicorn pillow")
    if "--disk" in sys.argv:
        disk_test()
        return
    if "--display" in sys.argv and "--frames" not in sys.argv and "--disk" not in sys.argv:
        position = sys.argv.index("--display")
        width, height = sys.argv[position + 1].lower().split("x")
        display_test(int(width), int(height))
        return
    if "--frames" in sys.argv:
        position = sys.argv.index("--frames")
        smoke(int(sys.argv[position + 1]) if len(sys.argv) > position + 1 else 60)
        return
    tour()


if __name__ == "__main__":
    main()
