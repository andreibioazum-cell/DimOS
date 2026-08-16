<div align="center">

# DimOS

**A minimal 16-bit real-mode operating system written in NASM for x86 PCs.**

[![License](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)](LICENSE.TXT)
[![Assembler](https://img.shields.io/badge/assembler-NASM-1f425f?style=for-the-badge)](https://nasm.us/)
[![Boot Mode](https://img.shields.io/badge/boot-BIOS%20Legacy-orange?style=for-the-badge)](#running-dimos)

[API documentation](docs/API.md) · [Configuration reference](docs/CONFIGURATION.md) · [Contributing](CONTRIBUTING.md)

</div>

## Overview

DimOS is a lightweight, educational operating system for BIOS-era x86 hardware. It boots directly into a command-line environment, uses FAT12 storage, and is built entirely in NASM assembly. The project is intended for exploring bootstrapping, filesystem design, interrupts, and direct hardware programming.

## Highlights

- BIOS real-mode boot and command-line environment
- FAT12 files, directories, and basic file-management commands
- Native `.COM` and `.EXE` program loading
- Configurable prompt, user account, password protection, font, theme, and timezone
- PS/2 and USB mouse support
- Built-in utilities, editors, diagnostics, games, image viewing, and media playback
- Kernel API for filesystem, disk, time, and text-output operations

## DimOS Terminal

The terminal launches programs from `BIN/` or the current directory. Run `HELP` for the built-in command reference and `INFO` for system details.

| Command | Description |
| --- | --- |
| `help` | Display the command reference |
| `info` | Show system information |
| `dir`, `cd`, `mkdir`, `deldir` | Browse and manage directories |
| `cat`, `copy`, `ren`, `del`, `touch`, `write` | Work with files |
| `view` | Display BMP images |
| `reboot`, `shut` | Restart or power off the machine |

## Building and running DimOS

### Build requirements

- NASM
- `dosfstools` and `mtools`
- A C++17 compiler (`g++` by default)
- `xorriso`
- GNU Make (optional convenience entry point)
- QEMU (optional, for `run-linux.sh`)

On Debian and Ubuntu, install the complete toolchain with:

```bash
sudo apt-get install build-essential dosfstools mtools nasm xorriso
```

### Build and run on Linux

```bash
make iso                  # or: ./build-linux.sh
./run-linux.sh
```

A successful build creates:

- `disk_img/dimos.img` — bootable 1.44 MB FAT12 floppy image;
- `disk_img/FLOPPY2.img` — writable secondary floppy used by the run script;
- `disk_img/dimos.iso` — bootable El Torito ISO containing `dimos.img`.

The C++ tool in `tools/image_inspector.cpp` validates the boot signature, FAT12 geometry, kernel loader limit, and the `KERNEL.BIN` directory entry during every build. Run `./build-linux.sh --help` to see optional build flags, or `make verify` to validate existing artifacts.

## Documentation

- [Kernel API](docs/API.md)
- [Bootloader](docs/Bootloader.md)
- [Configuration](docs/CONFIGURATION.md)
- [Contributing](CONTRIBUTING.md)

## License and notices

DimOS retains the repository's existing license and third-party notices. See [LICENSE.TXT](LICENSE.TXT) and the notices embedded with the relevant source files.
