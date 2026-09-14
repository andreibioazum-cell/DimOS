#!/usr/bin/env python3
"""Prove the GPU rasterizer draws what the kernel's own rasterizer drew.

The claim being tested is exact, not approximate: for the same scene, the
palette index of every one of the 64000 pixels must be identical whether
the frame was rasterized by src/kernel/gfx.c on the processor or by
tools/vkviewer/shaders/raster.comp on the GPU.

Each scene is run twice, from the same script:

    software    the unmodified kernel. The finished frame is read out of
                the back buffer at the moment gfx_show() is entered --
                the instant the frame is complete.
    recorded    the same kernel with gfx.c replaced by gfx_record.c, so
                nothing is rasterized at all. The command list is read
                out at that same instant, handed to bin/dimos-vkraster,
                and the indices the compute shader wrote are read back.

Both sides are therefore the same frame, and the comparison is a plain
byte for byte difference.

One scene is excluded. The terminal's DIR listing prints the size of
KERNEL.BIN, and the two kernels are different binaries -- the recording
one has no rasterizer in it, so it is about 1.5 KB smaller. The two runs
would be drawing genuinely different text, which says nothing about the
rasterizer. Pass --scene terminal to look at it anyway.

Usage:
    python3 tools/vkviewer/verify.py [--scene NAME ...]
"""

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SANDBOX = os.path.join(REPO, "tools", "sandbox")

sys.path.insert(0, SANDBOX)
import run_kernel as kernel  # noqa: E402
import capture_commands as recorder  # noqa: E402

BACK_BUFFER = 0x00060000
RASTERIZER = os.path.join(REPO, "bin", "dimos-vkraster")


def software_frame(scene):
    """The frame the kernel's own rasterizer produced."""
    flat, elf = kernel.build()
    machine = kernel.Machine(flat, kernel.symbols(elf))
    kernel.plant_boot_disk(machine, flat)
    machine.set_clock(0)
    recorder.SCENES[scene](machine)
    machine.frames_run(2)
    return bytes(machine.mu.mem_read(BACK_BUFFER, 320 * 200))


def recorded_frame(scene, directory):
    """The frame the GPU produced from the command list."""
    flat, elf = recorder.build_recording_kernel()
    table = kernel.symbols(elf)
    machine = recorder.RecordingMachine(flat, table)
    kernel.plant_boot_disk(machine, flat)
    machine.set_clock(0)
    recorder.SCENES[scene](machine)
    recorder.dump(machine, table, directory, scene)

    prefix = os.path.join(directory, scene)
    indices = os.path.join(directory, scene + ".gpu.bin")
    command = [
        RASTERIZER,
        "--commands", prefix + ".commands.bin",
        "--palette", prefix + ".palette.bin",
        "--base-palette", prefix + ".base.bin",
        "--glyphs", prefix + ".glyphs.bin",
        "--art", prefix + ".art.bin",
        "--blit", prefix + ".blit.bin",
        "--dump-indices", indices,
        "--out", prefix + ".gpu.png",
        "--scale", "4",
    ]
    result = subprocess.run(command, capture_output=True, text=True, cwd=REPO)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise SystemExit(f"{RASTERIZER} failed")
    for line in result.stdout.splitlines():
        if "rasterizing on" in line or "commands," in line:
            print("   ", line.strip())
    with open(indices, "rb") as handle:
        return handle.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", action="append", choices=sorted(recorder.SCENES))
    parser.add_argument("--out", default=os.path.join(HERE, "captures"))
    options = parser.parse_args()

    if not os.path.exists(RASTERIZER):
        raise SystemExit(f"{RASTERIZER} is missing -- run tools/vkviewer/build.sh")
    os.makedirs(options.out, exist_ok=True)
    # Every scene except the one whose content depends on the size of the
    # kernel binary. See the note at the top of this file.
    wanted = options.scene if options.scene else [
        name for name in sorted(recorder.SCENES) if name != "terminal"]

    failures = 0
    print("Comparing the GPU rasterizer against the kernel's own, pixel by pixel.\n")
    for scene in wanted:
        print(f"{scene}:")
        reference = software_frame(scene)
        produced = recorded_frame(scene, options.out)

        if len(reference) != len(produced):
            print(f"    FAIL: {len(reference)} bytes against {len(produced)}")
            failures += 1
            continue
        wrong = [i for i in range(len(reference)) if reference[i] != produced[i]]
        if not wrong:
            print(f"    identical: all {len(reference)} pixels match\n")
        else:
            failures += 1
            print(f"    FAIL: {len(wrong)} of {len(reference)} pixels differ")
            for offset in wrong[:8]:
                print(f"      ({offset % 320},{offset // 320}): "
                      f"kernel {reference[offset]}, GPU {produced[offset]}")
            print()

    if failures:
        raise SystemExit(f"{failures} scene(s) differ")
    print("Every scene matches the software rasterizer exactly.")


if __name__ == "__main__":
    main()
