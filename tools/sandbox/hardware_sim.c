/*
 * tools/sandbox/hardware_sim.c -- a pretend computer for the test runner.
 *
 * This file is NOT part of DimOS. It only exists so the kernel can be run
 * and screenshotted on a machine that has no assembler and no emulator
 * installed: it stands in for src/kernel/kernel.asm by providing
 * port_read_byte() / port_write_byte() / bios_font_address, and behind those
 * two functions it simulates the pieces of hardware the kernel talks to:
 *
 *   - the timer chip, driven by a clock the test runner advances by hand;
 *   - the PS/2 controller, fed scan codes and mouse packets by the runner;
 *   - the VGA palette registers, remembered so the runner can read them back;
 *   - the clock chip and the speaker;
 *   - an IDE controller whose disk is a megabyte of the runner's memory, so
 *     volume.c formats a real FAT12 volume and files survive a reboot.
 *
 * The runner and the simulated hardware share a block of memory at
 * 0x200000 (the protected-mode kernel uses 0x100000..0x1FFFFF for BSS): the
 * runner writes input and time there, and the kernel writes the palette there.
 */

#include <stddef.h>

#include "dimos.h"

typedef struct {
    volatile u32 milliseconds;      /* the simulated clock                    */
    volatile u32 keyboard_count;    /* scan codes waiting to be read          */
    volatile u8 keyboard[256];
    volatile u32 mouse_count;       /* mouse packet bytes waiting             */
    volatile u8 mouse[256];
    volatile u8 palette[256][3];    /* the last palette the kernel wrote      */
    volatile u32 speaker_hertz;     /* what the speaker is playing            */
    volatile u32 restarts;          /* set when the kernel asked for a reset  */
    volatile u32 debug_count;       /* bytes the kernel wrote to port 0xE9     */
    volatile u8 debug[256];         /* the Bochs/QEMU debug console           */
} Simulator;

#define SIMULATOR ((volatile Simulator *)(u32)0x00200000u)

/* The test runner reads and writes this block straight out of memory, so the
 * two sides have to agree on where every field sits. These checks fail the
 * build if that ever stops being true. */
typedef char check_mouse_count_offset[(offsetof(Simulator, mouse_count) == 0x108u) ? 1 : -1];
typedef char check_palette_offset[(offsetof(Simulator, palette) == 0x20Cu) ? 1 : -1];
typedef char check_restarts_offset[(offsetof(Simulator, restarts) == 0x510u) ? 1 : -1];

/* The address the test runner copied an 8x8 font to. kernel.asm would have
 * stored the video BIOS font address here. Kept in .data (not .bss) so the
 * test runner can fill it in before kernel_main() clears the BSS; on real
 * hardware kernel.asm writes it again right after that. */
__attribute__((section(".data"))) u32 bios_font_address;

/* The real-mode entry normally fills these after probing VBE. Exercise the
 * preferred full-HD XRGB8888 presenter in every visual-tour frame; 0x900000
 * sits just above the kernel's 4 MiB RAM disk in the simulated physical map. */
__attribute__((section(".data"))) u8 video_backend = VIDEO_BACKEND_VBE;
__attribute__((section(".data"))) u32 video_framebuffer_address = 0x00900000u;
__attribute__((section(".data"))) u16 video_pitch = 1920u * 4u;
__attribute__((section(".data"))) u16 video_width = 1920u;
__attribute__((section(".data"))) u16 video_height = 1080u;
__attribute__((section(".data"))) u8 video_bits_per_pixel = 32u;

/* IDE controller. The disk is not a file: it is the last megabyte of the
 * simulated physical memory, which the runner can read, write and keep
 * between runs. */
#define SIMULATOR_DISK_ADDRESS 0x01100000u
#define SIMULATOR_DISK_BYTES (1024u * 1024u)
#define SIMULATOR_DISK_SECTORS (SIMULATOR_DISK_BYTES / 512u)

#define IDE_STATUS_ERROR 0x01u
#define IDE_STATUS_DRIVE_READY 0x40u
#define IDE_STATUS_BUSY 0x80u
#define IDE_STATUS_DATA_REQUEST 0x08u

static u8 ide_status = IDE_STATUS_DRIVE_READY;
static u8 ide_error;
static u16 ide_count;
static u8 ide_lba_low;
static u8 ide_lba_mid;
static u8 ide_lba_high;
static u8 ide_drive;
static u8 ide_buffer[512];
static u16 ide_position;
static u8 ide_direction; /* 0 idle, 1 sending to the CPU, 2 taking from it */
static u8 ide_sectors_left;
static u32 ide_next_lba;

static u8 *ide_disk(void) {
    return (u8 *)(u32)SIMULATOR_DISK_ADDRESS;
}

static u32 ide_lba(void) {
    return (u32)ide_lba_low | ((u32)ide_lba_mid << 8u) |
           ((u32)ide_lba_high << 16u) | ((u32)(ide_drive & 0x0Fu) << 24u);
}

static void ide_load_sector(u32 lba) {
    const u8 *source = ide_disk() + lba * 512u;

    memory_copy(ide_buffer, source, 512u);
    ide_position = 0u;
    ide_direction = 1u;
    ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_DATA_REQUEST;
}

static void ide_store_sector(u32 lba) {
    u8 *target = ide_disk() + lba * 512u;

    memory_copy(target, ide_buffer, 512u);
}

static void ide_identify(void) {
    u16 index;
    const char *model = "DIMOS sandbox disk                      ";

    memory_zero(ide_buffer, 512u);
    for (index = 0u; index < 40u; ++index) {
        const char *pair = model + index * 2u;

        ide_buffer[27u * 2u + index * 2u] = (u8)pair[0];
        ide_buffer[27u * 2u + index * 2u + 1u] = (u8)pair[1];
    }
    ide_buffer[49u * 2u] = 0x00u;      /* LBA supported */
    ide_buffer[49u * 2u + 1u] = 0x02u;
    ide_buffer[60u * 2u] = (u8)(SIMULATOR_DISK_SECTORS & 0xFFu);
    ide_buffer[60u * 2u + 1u] = (u8)((SIMULATOR_DISK_SECTORS >> 8u) & 0xFFu);
    ide_buffer[61u * 2u] = (u8)((SIMULATOR_DISK_SECTORS >> 16u) & 0xFFu);
    ide_buffer[61u * 2u + 1u] = (u8)((SIMULATOR_DISK_SECTORS >> 24u) & 0xFFu);
    ide_position = 0u;
    ide_sectors_left = 1u;
    ide_direction = 1u;
    ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_DATA_REQUEST;
}

static void ide_command(u8 command) {
    ide_error = 0u;
    switch (command) {
        case 0xECu: /* IDENTIFY */
            ide_identify();
            return;
        case 0x20u: /* READ SECTORS */
            ide_sectors_left = (u8)(ide_count & 0xFFu);
            if (ide_sectors_left == 0u) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                return;
            }
            ide_next_lba = ide_lba();
            if (ide_next_lba >= SIMULATOR_DISK_SECTORS) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                ide_error = 0x10u;
                return;
            }
            ide_load_sector(ide_next_lba);
            return;
        case 0x30u: /* WRITE SECTORS */
            ide_sectors_left = (u8)(ide_count & 0xFFu);
            if (ide_sectors_left == 0u) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                return;
            }
            ide_next_lba = ide_lba();
            if (ide_next_lba >= SIMULATOR_DISK_SECTORS) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                ide_error = 0x10u;
                return;
            }
            ide_position = 0u;
            ide_direction = 2u;
            ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_DATA_REQUEST;
            return;
        case 0xE7u: /* FLUSH CACHE */
            ide_direction = 0u;
            ide_status = IDE_STATUS_DRIVE_READY;
            return;
        default:
            ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
            ide_error = 0x04u;
            return;
    }
}

static u16 ide_read_word(void) {
    u16 value;

    if (ide_direction != 1u) {
        return 0u;
    }
    value = (u16)((u16)ide_buffer[ide_position] |
                  (u16)((u16)ide_buffer[ide_position + 1u] << 8u));
    ide_position = (u16)(ide_position + 2u);
    if (ide_position >= 512u) {
        ide_position = 0u;
        if (ide_sectors_left > 1u) {
            --ide_sectors_left;
            ++ide_next_lba;
            ide_load_sector(ide_next_lba);
        } else {
            ide_sectors_left = 0u;
            ide_direction = 0u;
            ide_status = IDE_STATUS_DRIVE_READY;
        }
    }
    return value;
}

static void ide_write_word(u16 value) {
    if (ide_direction != 2u) {
        return;
    }
    ide_buffer[ide_position] = (u8)(value & 0xFFu);
    ide_buffer[ide_position + 1u] = (u8)(value >> 8u);
    ide_position = (u16)(ide_position + 2u);
    if (ide_position >= 512u) {
        ide_store_sector(ide_next_lba);
        ide_position = 0u;
        if (ide_sectors_left > 1u) {
            --ide_sectors_left;
            ++ide_next_lba;
        } else {
            ide_sectors_left = 0u;
            ide_direction = 0u;
            ide_status = IDE_STATUS_DRIVE_READY;
        }
    }
}

/* Timer chip. */
#define SIMULATOR_TIMER_DIVISOR 65535u
#define COUNTS_PER_MILLISECOND 1193u

static u8 timer_low_latched;
static u8 timer_high_latched;
static u8 timer_bytes_left;
static u8 timer_divisor_low;
static u8 timer_divisor_high;

/* PS/2 controller. */
static u8 controller_expect_mouse_command;
static u8 controller_acknowledgement_waiting;
static u8 controller_mouse_id_waiting;

/* VGA palette. */
static u8 palette_index;
static u8 palette_component;

/* Clock chip. */
static u8 cmos_index;

/* Speaker. */
static u8 speaker_state;

/* The kernel latches the counter before reading it, so remember which half
 * of the latched value comes next. Latching is also what makes the simulated
 * clock move: one millisecond per latch, so the 60 Hz limiter spins about
 * seventeen times and then carries on, exactly as it would on real hardware. */
static u16 current_timer_counter(void) {
    const u32 counts = (SIMULATOR->milliseconds % 1000u) * COUNTS_PER_MILLISECOND;

    ++SIMULATOR->milliseconds;
    return (u16)(SIMULATOR_TIMER_DIVISOR - (u16)(counts % SIMULATOR_TIMER_DIVISOR));
}

static void latch_timer(void) {
    const u16 value = current_timer_counter();

    timer_low_latched = (u8)(value & 0xFFu);
    timer_high_latched = (u8)(value >> 8u);
    timer_bytes_left = 2u;
}

u8 port_read_byte(u16 port) {
    switch (port) {
        case PORT_KEYBOARD_STATUS: {
            u8 status = 0u;

            if (controller_acknowledgement_waiting != 0u ||
                controller_mouse_id_waiting != 0u) {
                status |= 0x01u | 0x20u; /* an auxiliary-device reply */
            } else if (SIMULATOR->keyboard_count != 0u) {
                status |= 0x01u;
            } else if (SIMULATOR->mouse_count != 0u) {
                status |= 0x01u | 0x20u; /* a mouse packet byte */
            }
            return status;
        }
        case PORT_KEYBOARD_DATA:
            if (controller_acknowledgement_waiting != 0u) {
                controller_acknowledgement_waiting = 0u;
                return 0xFAu; /* every mouse command is acknowledged */
            }
            if (controller_mouse_id_waiting != 0u) {
                controller_mouse_id_waiting = 0u;
                return 0u; /* standard three-byte PS/2 mouse */
            }
            if (SIMULATOR->keyboard_count != 0u) {
                const u8 value = SIMULATOR->keyboard[0];
                u32 index;

                for (index = 1u; index < SIMULATOR->keyboard_count; ++index) {
                    SIMULATOR->keyboard[index - 1u] = SIMULATOR->keyboard[index];
                }
                --SIMULATOR->keyboard_count;
                return value;
            }
            if (SIMULATOR->mouse_count != 0u) {
                const u8 value = SIMULATOR->mouse[0];
                u32 index;

                for (index = 1u; index < SIMULATOR->mouse_count; ++index) {
                    SIMULATOR->mouse[index - 1u] = SIMULATOR->mouse[index];
                }
                --SIMULATOR->mouse_count;
                return value;
            }
            return 0u;

        case PORT_PIT_CHANNEL_0:
            if (timer_bytes_left == 2u) {
                timer_bytes_left = 1u;
                return timer_low_latched;
            }
            timer_bytes_left = 0u;
            return timer_high_latched;

        case PORT_SPEAKER:
            return speaker_state;

        case 0x01F0u:
            return (u8)ide_read_word();
        case 0x01F1u:
            return ide_error;
        case 0x01F7u:
        case 0x03F6u:
            return ide_status;

        case PORT_CMOS_DATA:
            /* A fixed date: 2026-08-29, 12:34:56, in binary coded decimal. */
            switch (cmos_index) {
                case 0x00u: return 0x56u;
                case 0x02u: return 0x34u;
                case 0x04u: return 0x12u;
                case 0x07u: return 0x29u;
                case 0x08u: return 0x08u;
                case 0x09u: return 0x26u;
                case 0x32u: return 0x20u;
                case 0x0Bu: return 0x02u; /* 24 hour clock, decimal digits */
                default: return 0u;
            }

        default:
            return 0u;
    }
}

u16 port_read_word(u16 port) {
    if (port == 0x01F0u) {
        return ide_read_word();
    }
    return (u16)(port_read_byte(port) | ((u16)port_read_byte(port) << 8u));
}

void port_write_word(u16 port, u16 value) {
    if (port == 0x01F0u) {
        ide_write_word(value);
        return;
    }
    port_write_byte(port, (u8)(value & 0xFFu));
    port_write_byte(port, (u8)(value >> 8u));
}

void port_write_byte(u16 port, u8 value) {
    switch (port) {
        case PORT_KEYBOARD_DATA:
            if (controller_expect_mouse_command != 0u) {
                controller_expect_mouse_command = 0u;
                controller_acknowledgement_waiting = 1u;
                if (value == 0xF2u) {
                    controller_mouse_id_waiting = 1u;
                }
            }
            return;

        case PORT_KEYBOARD_STATUS:
            if (value == 0xD4u) {
                controller_expect_mouse_command = 1u;
            } else if (value == PORT_KEYBOARD_RESET) {
                ++SIMULATOR->restarts;
            }
            return;

        case PORT_PIT_COMMAND:
            if ((value & 0xC0u) == 0x00u) {
                latch_timer();
            }
            return;

        case PORT_PIT_CHANNEL_0:
            if (timer_divisor_low == 0u && timer_divisor_high == 0u) {
                timer_divisor_low = value;
            } else {
                timer_divisor_high = value;
            }
            return;

        case PORT_PIT_CHANNEL_2:
            return;

        case PORT_SPEAKER:
            speaker_state = value;
            if ((value & 0x03u) == 0x03u) {
                SIMULATOR->speaker_hertz =
                    (u32)((u16)timer_divisor_low | (u16)((u16)timer_divisor_high << 8u));
            } else {
                SIMULATOR->speaker_hertz = 0u;
            }
            return;

        case PORT_VGA_DAC_WRITE_INDEX:
            palette_index = value;
            palette_component = 0u;
            return;

        case PORT_VGA_DAC_DATA:
            if (palette_component < 3u) {
                SIMULATOR->palette[palette_index][palette_component] = value;
            }
            palette_component = (u8)((palette_component + 1u) % 3u);
            return;

        case 0x01F2u:
            ide_count = value;
            return;
        case 0x01F3u:
            ide_lba_low = value;
            return;
        case 0x01F4u:
            ide_lba_mid = value;
            return;
        case 0x01F5u:
            ide_lba_high = value;
            return;
        case 0x01F6u:
            ide_drive = value;
            return;
        case 0x01F7u:
            ide_command(value);
            return;
        case 0x03F6u:
            return;

        case 0x00E9u:
            /* The debug console CI greps. Keep the last 256 bytes. */
            if (SIMULATOR->debug_count < 256u) {
                SIMULATOR->debug[SIMULATOR->debug_count] = value;
                ++SIMULATOR->debug_count;
            }
            return;

        case PORT_CMOS_ADDRESS:
            cmos_index = value;
            return;

        default:
            return;
    }
}
