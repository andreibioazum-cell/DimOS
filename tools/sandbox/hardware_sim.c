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
 *   - the clock chip and the speaker.
 *
 * The runner and the simulated hardware share a block of memory at
 * 0x100000: the runner writes input and the time there, the kernel writes the
 * palette there.
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
} Simulator;

#define SIMULATOR ((volatile Simulator *)0x00100000u)

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

/* Timer chip. */
#define SIMULATOR_TIMER_DIVISOR 11932u
#define COUNTS_PER_MILLISECOND 1193u

static u8 timer_low_latched;
static u8 timer_high_latched;
static u8 timer_bytes_left;
static u8 timer_divisor_low;
static u8 timer_divisor_high;

/* PS/2 controller. */
static u8 controller_expect_mouse_command;
static u8 controller_acknowledgement_waiting;

/* VGA palette. */
static u8 palette_index;
static u8 palette_component;

/* Clock chip. */
static u8 cmos_index;

/* Speaker. */
static u8 speaker_state;

/* The kernel latches the counter before reading it, so remember which half
 * of the latched value comes next. Latching is also what makes the simulated
 * clock move: one millisecond per latch, so a frame that waits 40 ms spins
 * forty times and then carries on, exactly as it would on real hardware. */
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
                SIMULATOR->keyboard_count != 0u) {
                status |= 0x01u; /* a byte is waiting */
            } else if (SIMULATOR->mouse_count != 0u) {
                status |= 0x01u | 0x20u; /* ... and it came from the mouse */
            }
            return status;
        }
        case PORT_KEYBOARD_DATA:
            if (controller_acknowledgement_waiting != 0u) {
                controller_acknowledgement_waiting = 0u;
                return 0xFAu; /* every mouse command is acknowledged */
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

void port_write_byte(u16 port, u8 value) {
    switch (port) {
        case PORT_KEYBOARD_DATA:
            if (controller_expect_mouse_command != 0u) {
                controller_expect_mouse_command = 0u;
                controller_acknowledgement_waiting = 1u;
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

        case PORT_CMOS_ADDRESS:
            cmos_index = value;
            return;

        default:
            return;
    }
}
