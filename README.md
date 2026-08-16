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

## Running DimOS

### Requirements

- NASM
- `mtools`
- QEMU (optional, for `run-linux.sh`)

### Build and run on Linux

```bash
./build-linux.sh
./run-linux.sh
```

The build script creates a bootable FAT12 disk image. See the script output for its exact location and available build options.

## Documentation

- [Kernel API](docs/API.md)
- [Bootloader](docs/Bootloader.md)
- [Configuration](docs/CONFIGURATION.md)
- [Contributing](CONTRIBUTING.md)

## License and notices

DimOS retains the repository's existing license and third-party notices. See [LICENSE.TXT](LICENSE.TXT) and the notices embedded with the relevant source files.
