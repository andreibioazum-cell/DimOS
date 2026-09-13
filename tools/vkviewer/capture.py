#!/usr/bin/env python3
"""Capture real DimOS frames for the Vulkan viewer.

This runs the actual kernel -- the same src/kernel/*.c that boots on
hardware -- under the sandbox emulator, drives it with clicks, and dumps
two files per frame:

    NAME.frame.bin    320*200 bytes, straight out of the VGA frame buffer
                      at 0xA0000. One byte per pixel, palette indices,
                      exactly what the kernel wrote.
    NAME.palette.bin  256*3 bytes, the VGA DAC palette the kernel
                      programmed through ports 0x3C8/0x3C9, expanded from
                      the card's 6 bits per channel to 8.

Nothing here interprets the picture: the palette lookup is the Vulkan
fragment shader's job. This script only moves bytes out of the emulated
machine.

Usage:
    python3 tools/vkviewer/capture.py [--out DIR] [--scene NAME ...]
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SANDBOX = os.path.join(REPO, "tools", "sandbox")

sys.path.insert(0, SANDBOX)
import run_kernel as kernel  # noqa: E402


def dump(machine, directory, name):
    """Writes one frame and its palette next to each other."""
    machine.frames_run(2)
    frame = bytes(machine.mu.mem_read(kernel.FRAME_BUFFER, 320 * 200))
    palette = bytearray()
    for red, green, blue in machine.palette():
        palette += bytes((red, green, blue))

    frame_path = os.path.join(directory, name + ".frame.bin")
    palette_path = os.path.join(directory, name + ".palette.bin")
    with open(frame_path, "wb") as handle:
        handle.write(frame)
    with open(palette_path, "wb") as handle:
        handle.write(bytes(palette))
    print(f"  captured {name}: {len(frame)} bytes + {len(palette)} byte palette")
    return frame_path, palette_path


def scene_desktop(machine):
    machine.frames_run(3)


def scene_paint(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(3))
    machine.drag(30, 50, 180, 120)
    machine.drag(40, 120, 200, 60)


def scene_mines(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(2))
    machine.click(76, 96)
    machine.click(104, 96)


def scene_terminal(machine):
    machine.frames_run(3)
    machine.click(*kernel.icon_center(7))
    machine.click(*kernel.term_key(2, 2))    # D
    machine.click(*kernel.term_key(1, 7))    # I
    machine.click(*kernel.term_key(1, 3))    # R
    machine.click(*kernel.term_key(2, 10))   # ENT
    machine.frames_run(2)


def scene_amber(machine):
    """The About window switched to the amber phosphor palette.

    Worth capturing because the theme lives entirely in the DAC: the
    frame buffer bytes barely change, the palette does. The shader gets
    both, so it reproduces the theme without knowing it exists.
    """
    machine.frames_run(3)
    machine.click(*kernel.icon_center(6))
    machine.click(13 + 6 + 45, 145 + 8)   # Theme -> green
    machine.click(13 + 6 + 45, 145 + 8)   # Theme -> amber


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
    parser.add_argument("--scene", action="append", choices=sorted(SCENES),
                        help="capture only these scenes (default: all)")
    options = parser.parse_args()

    os.makedirs(options.out, exist_ok=True)
    wanted = options.scene if options.scene else sorted(SCENES)

    flat, elf = kernel.build()
    table = kernel.symbols(elf)

    for name in wanted:
        print(f"\nScene: {name}")
        machine = kernel.Machine(flat, table)
        kernel.plant_boot_disk(machine, flat)
        machine.set_clock(0)
        SCENES[name](machine)
        dump(machine, options.out, name)

    print(f"\nCaptures written to {options.out}")


if __name__ == "__main__":
    main()
