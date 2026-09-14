#!/usr/bin/env python3
"""Run DimOS with no rasterizer and capture what it asked to be drawn.

The kernel built here is the real one -- the same applications, window
manager, font loader and input code -- except that src/kernel/gfx.c is
replaced by tools/vkviewer/gfx_record.c. That file implements the same
interface without computing a single pixel: every drawing call becomes
an entry in a command list.

So this script produces geometry, not an image:

    NAME.commands.bin   the header plus the commands of one frame
    NAME.palette.bin    the 256 entry VGA DAC palette
    NAME.glyphs.bin     the font atlas the kernel built at boot
    NAME.art.bin        the icon atlas, one byte per cell
    NAME.blit.bin       pixels an application wrote by hand (Paint only)

bin/dimos-vkraster turns those into pixels with a compute shader.

Usage:
    python3 tools/vkviewer/capture_commands.py [--scene NAME ...]
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SANDBOX = os.path.join(REPO, "tools", "sandbox")
BUILD = os.path.join(HERE, "out")

sys.path.insert(0, SANDBOX)
import run_kernel as kernel  # noqa: E402

COMMAND_AREA = 0x00200000
COMMAND_MAGIC = 0x55504744
HEADER_BYTES = 32
COMMAND_BYTES = 24
COMMAND_LIMIT = 16384

ART_SLOTS = 16
ART_ROWS = 16
ART_COLUMNS = 16
GLYPH_BYTES = 128 * 64


def build_recording_kernel():
    """Compile the kernel with the recorder instead of the rasterizer."""
    os.makedirs(BUILD, exist_ok=True)
    sources = [
        os.path.join(REPO, "src", "kernel", name)
        for name in sorted(os.listdir(os.path.join(REPO, "src", "kernel")))
        if name.endswith(".c") and name != "gfx.c"
    ]
    sources.append(os.path.join(HERE, "gfx_record.c"))
    sources.append(os.path.join(SANDBOX, "hardware_sim.c"))

    flags = kernel.CFLAGS + ["-I", HERE]
    objects = []
    for source in sources:
        obj = os.path.join(BUILD, os.path.basename(source)[:-2] + ".o")
        subprocess.run(flags + ["-c", source, "-o", obj], check=True, cwd=REPO,
                       stdout=subprocess.DEVNULL)
        objects.append(obj)

    elf = os.path.join(BUILD, "kernel-noraster.elf")
    subprocess.run(["ld", "-m", "elf_i386", "--build-id=none", "-nostdlib",
                    "-e", "kernel_main",
                    "-T", os.path.join(REPO, "src", "kernel", "linker.ld")] +
                   objects + ["-o", elf], check=True, cwd=REPO,
                   stdout=subprocess.DEVNULL)
    flat = os.path.join(BUILD, "kernel-noraster.bin")
    subprocess.run(["objcopy", "-O", "binary", elf, flat], check=True, cwd=REPO,
                   stdout=subprocess.DEVNULL)
    print(f"  kernel without gfx.c: {os.path.getsize(flat)} bytes")
    return flat, elf


class RecordingMachine(kernel.Machine):
    """The sandbox machine, with room mapped for the command list."""

    def __init__(self, flat, table):
        super().__init__(flat, table)
        # The command list, plus the blit snapshot that follows it.
        self.mu.mem_map(COMMAND_AREA, 0x00200000)


def dump(machine, table, directory, name):
    machine.frames_run(2)

    header = bytes(machine.mu.mem_read(COMMAND_AREA, HEADER_BYTES))
    magic, count, overflowed, font_mode, frames, blit_used = struct.unpack(
        "<6I", header[:24])
    if magic != COMMAND_MAGIC:
        raise SystemExit(f"no command list at {COMMAND_AREA:#x} (magic {magic:#x})")
    if overflowed:
        print("  WARNING: the frame needed more commands than the kernel's limit")
    if count > COMMAND_LIMIT:
        raise SystemExit("implausible command count")

    payload = bytes(machine.mu.mem_read(COMMAND_AREA,
                                        HEADER_BYTES + count * COMMAND_BYTES))
    with open(os.path.join(directory, name + ".commands.bin"), "wb") as handle:
        handle.write(payload)

    palette = bytearray()
    for red, green, blue in machine.palette():
        palette += bytes((red, green, blue))
    with open(os.path.join(directory, name + ".palette.bin"), "wb") as handle:
        handle.write(bytes(palette))

    # The palette gfx.c blends in: untinted, 8 bits, straight from the
    # kernel. The DAC table above is what the screen shows.
    base = bytes(machine.mu.mem_read(table["dimos_base_palette"], 256 * 3))
    with open(os.path.join(directory, name + ".base.bin"), "wb") as handle:
        handle.write(base)

    # The font atlas: the anti-aliased table when FONT.TTF was found,
    # otherwise the one bit BIOS font.
    alpha_pointer = struct.unpack(
        "<I", bytes(machine.mu.mem_read(table["dimos_font_alpha"], 4)))[0]
    bits_pointer = struct.unpack(
        "<I", bytes(machine.mu.mem_read(table["dimos_font_bits"], 4)))[0]
    if font_mode == 1 and alpha_pointer:
        glyphs = bytes(machine.mu.mem_read(alpha_pointer, GLYPH_BYTES))
    else:
        glyphs = bytes(machine.mu.mem_read(bits_pointer, 128 * 8))
    with open(os.path.join(directory, name + ".glyphs.bin"), "wb") as handle:
        handle.write(glyphs)

    art = bytes(machine.mu.mem_read(table["dimos_art_atlas"],
                                    ART_SLOTS * ART_ROWS * ART_COLUMNS))
    with open(os.path.join(directory, name + ".art.bin"), "wb") as handle:
        handle.write(art)

    # The snapshot the recorder took of the direct-write buffer, before
    # it was reset for the next frame. Only Paint writes there; every
    # other pixel stays BLIT_UNTOUCHED.
    blit = bytes(machine.mu.mem_read(COMMAND_AREA + 1024 * 1024, 320 * 200))
    with open(os.path.join(directory, name + ".blit.bin"), "wb") as handle:
        handle.write(blit)

    kinds = {}
    for index in range(count):
        offset = HEADER_BYTES + index * COMMAND_BYTES
        kind = struct.unpack("<H", payload[offset:offset + 2])[0]
        kinds[kind] = kinds.get(kind, 0) + 1
    names = {0: "clear", 1: "rect", 2: "glyph", 3: "art", 4: "circle",
             5: "line", 6: "checker", 7: "pointer", 8: "blit"}
    summary = ", ".join(f"{names.get(k, k)}={v}" for k, v in sorted(kinds.items()))
    print(f"  {name}: {count} commands ({summary}), font_mode={font_mode}, "
          f"blit={blit_used}")


def scene_desktop(machine):
    machine.frames_run(3)


def scene_paint(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(3))
    machine.drag(30, 50, 180, 120)


def scene_mines(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(2))
    machine.click(76, 96)


def scene_terminal(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(7))
    machine.click(*kernel.term_key(2, 2))
    machine.click(*kernel.term_key(1, 7))
    machine.click(*kernel.term_key(1, 3))
    machine.click(*kernel.term_key(2, 10))
    machine.frames_run(2)


def scene_amber(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(6))
    machine.click(13 + 6 + 45, 145 + 8)
    machine.click(13 + 6 + 45, 145 + 8)


SCENES = {
    "desktop": scene_desktop,
    "paint": scene_paint,
    "mines": scene_mines,
    "terminal": scene_terminal,
    "amber": scene_amber,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(HERE, "captures"))
    parser.add_argument("--scene", action="append", choices=sorted(SCENES))
    options = parser.parse_args()

    os.makedirs(options.out, exist_ok=True)
    wanted = options.scene if options.scene else sorted(SCENES)

    flat, elf = build_recording_kernel()
    table = kernel.symbols(elf)

    for name in wanted:
        print(f"\nScene: {name}")
        machine = RecordingMachine(flat, table)
        kernel.plant_boot_disk(machine, flat)
        machine.set_clock(0)
        SCENES[name](machine)
        dump(machine, table, options.out, name)

    print(f"\nCommand captures written to {options.out}")


if __name__ == "__main__":
    main()
