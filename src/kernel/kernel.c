/*
 * kernel.c -- start up, time keeping, and the small helpers every other file
 * uses (memory, text, restart).
 *
 * The bootloader loads this code at 0x20000 and kernel.asm selects the video
 * hardware and enters a flat 32-bit protected-mode environment before calling
 * kernel_main. From here on everything is plain C.
 */

#include "dimos.h"

/* Provided by the linker script: the uninitialized globals of the kernel.
 * A flat binary does not carry them, so they have to be cleared by hand. */
extern char __bss_start[];
extern char __bss_end[];

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void memory_copy(void *destination, const void *source, u32 length) {
    u8 *target = (u8 *)destination;
    const u8 *from = (const u8 *)source;
    u32 index;

    for (index = 0u; index < length; ++index) {
        target[index] = from[index];
    }
}

void memory_zero(void *destination, u32 length) {
    u8 *target = (u8 *)destination;
    u32 index;

    for (index = 0u; index < length; ++index) {
        target[index] = 0u;
    }
}

/* Reads a 16 bit value the BIOS left somewhere in low memory, for example
 * the memory size word at 0x413. The address is a parameter so the compiler
 * cannot fold the access away. */
u16 bios_read_word(u32 address) {
#ifdef DIMOS_HOST
    /* A hosted build has no BIOS and no low-memory data area. Report the
     * classic 640 KiB of conventional memory that Settings shows. */
    (void)address;
    return 640u;
#else
    const volatile u16 *place = dimos_physical(const volatile u16 *, address);

    return *place;
#endif
}

u8 memory_equal(const void *a, const void *b, u32 length) {
    const u8 *left = (const u8 *)a;
    const u8 *right = (const u8 *)b;
    u32 index;

    for (index = 0u; index < length; ++index) {
        if (left[index] != right[index]) {
            return 0u;
        }
    }
    return 1u;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

u8 character_to_upper(u8 character) {
    if (character >= (u8)'a' && character <= (u8)'z') {
        return (u8)(character - (u8)('a' - 'A'));
    }
    return character;
}

u16 text_length(const char *text) {
    u16 length = 0u;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

u8 text_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (u8)(*a == *b);
}

u8 text_equal_ignore_case(const char *a, const char *b) {
    while (*a != '\0' && character_to_upper((u8)*a) == character_to_upper((u8)*b)) {
        ++a;
        ++b;
    }
    return (u8)(*a == *b);
}

void text_copy(char *destination, const char *source, u16 capacity) {
    u16 index = 0u;

    if (capacity == 0u) {
        return;
    }
    while (source[index] != '\0' && index < (u16)(capacity - 1u)) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void text_append(char *destination, const char *source, u16 capacity) {
    u16 index = text_length(destination);

    while (*source != '\0' && index < (u16)(capacity - 1u)) {
        destination[index] = *source;
        ++index;
        ++source;
    }
    destination[index] = '\0';
}

void text_append_character(char *destination, char character, u16 capacity) {
    const u16 index = text_length(destination);

    if (index < (u16)(capacity - 1u)) {
        destination[index] = character;
        destination[index + 1u] = '\0';
    }
}

/* Writes the number as decimal digits and returns how many were written. */
u16 text_append_number(char *destination, u32 value, u16 capacity) {
    char digits[10];
    u16 count = 0u;
    u16 written = 0u;

    if (value == 0u) {
        text_append_character(destination, '0', capacity);
        return 1u;
    }
    while (value != 0u && count < 10u) {
        digits[count] = (char)('0' + (char)(value % 10u));
        value /= 10u;
        ++count;
    }
    while (count != 0u) {
        --count;
        text_append_character(destination, digits[count], capacity);
        ++written;
    }
    return written;
}

void text_trim(char *text) {
    u16 start = 0u;
    u16 end = text_length(text);

    while (text[start] == ' ') {
        ++start;
    }
    while (end > start && text[end - 1u] == ' ') {
        --end;
    }
    text[end] = '\0';
    if (start != 0u) {
        u16 index = start;
        while (index <= end) {
            text[index - start] = text[index];
            ++index;
        }
    }
}

void text_pad_right(char *destination, u16 width, u16 capacity) {
    u16 length = text_length(destination);

    while (length < width && length < (u16)(capacity - 1u)) {
        destination[length] = ' ';
        ++length;
    }
    destination[length] = '\0';
}

/* ------------------------------------------------------------------ */
/* The programmable interval timer                                     */
/* ------------------------------------------------------------------ */

#define TIMER_INPUT_FREQUENCY 1193182u
#define TIMER_TICK_COUNTS 11932u
#define TIMER_LATCH_CHANNEL_0 0x00u
#ifdef DIMOS_HOST
/* The hosted build has no PIT and no interrupts: the clock is the host's
 * monotonic timer, converted to the same input counts a PIT would give. */
#define TIMER_DIVISOR 19886u
u64 host_timer_counts(void);
static u64 timer_last_counts;
#elif !defined(DIMOS_SANDBOX)
/* One IRQ per display interval. HLT sleeps between frames instead of
 * spinning on the PIT, while maintaining the compositor's 60 Hz cadence. */
#define TIMER_DIVISOR 19886u
extern void timer_interrupts_init(void);
volatile u32 timer_irq_low;
volatile u32 timer_irq_high;
static u64 timer_last_counts;
#else
/* The Unicorn simulator advances the PIT on reads and has no PIC/IDT. */
#define TIMER_DIVISOR 65535u
static u16 timer_last_counter;
static u64 elapsed_input_counts;
#endif

static u64 timer_leftover_counts;
static u32 elapsed_milliseconds;
static u16 ticks_waiting;

#if defined(DIMOS_SANDBOX) && !defined(DIMOS_HOST)
static u16 timer_read_counter(void) {
    u8 low;
    u8 high;

    port_write_byte(PORT_PIT_COMMAND, TIMER_LATCH_CHANNEL_0);
    low = port_read_byte(PORT_PIT_CHANNEL_0);
    high = port_read_byte(PORT_PIT_CHANNEL_0);
    return (u16)((u16)low | (u16)((u16)high << 8u));
}
#endif

void timer_init(void) {
#ifdef DIMOS_HOST
    timer_last_counts = host_timer_counts();
#elif defined(DIMOS_SANDBOX)
    /* Channel 0, rate generator, both bytes, binary counting. */
    port_write_byte(PORT_PIT_COMMAND, 0x34u);
    port_write_byte(PORT_PIT_CHANNEL_0, (u8)(TIMER_DIVISOR & 0xFFu));
    port_write_byte(PORT_PIT_CHANNEL_0, (u8)(TIMER_DIVISOR >> 8u));
    timer_last_counter = timer_read_counter();
    elapsed_input_counts = 0u;
#else
    timer_irq_low = 0u;
    timer_irq_high = 0u;
    /* Channel 0, square-wave generator, approximately 60 interrupts/sec. */
    port_write_byte(PORT_PIT_COMMAND, 0x36u);
    port_write_byte(PORT_PIT_CHANNEL_0, (u8)(TIMER_DIVISOR & 0xFFu));
    port_write_byte(PORT_PIT_CHANNEL_0, (u8)(TIMER_DIVISOR >> 8u));
    timer_interrupts_init();
    timer_last_counts = 0u;
#endif
    timer_leftover_counts = 0u;
    elapsed_milliseconds = 0u;
    ticks_waiting = 0u;
}

void timer_update(void) {
#ifdef DIMOS_HOST
    const u64 current = host_timer_counts();

    timer_leftover_counts += current - timer_last_counts;
    timer_last_counts = current;
#elif defined(DIMOS_SANDBOX)
    const u16 current = timer_read_counter();
    u16 elapsed;

    /* The simulated PIT counts down and wraps around on each read period. */
    if (timer_last_counter >= current) {
        elapsed = (u16)(timer_last_counter - current);
    } else {
        elapsed = (u16)(timer_last_counter + (TIMER_DIVISOR - current));
    }
    timer_last_counter = current;
    elapsed_input_counts += (u64)elapsed;
    timer_leftover_counts += elapsed;
#else
    const u64 current = timer_counter();
    const u64 elapsed = current - timer_last_counts;

    timer_last_counts = current;
    timer_leftover_counts += elapsed;
#endif

    while (timer_leftover_counts >= TIMER_TICK_COUNTS) {
        timer_leftover_counts -= TIMER_TICK_COUNTS;
        elapsed_milliseconds += TICK_MILLISECONDS;
        ++ticks_waiting;
    }
}

u32 time_milliseconds(void) {
    return elapsed_milliseconds;
}

/* Raw PIT input clocks retain sub-millisecond precision for frame pacing. */
u64 timer_counter(void) {
#ifdef DIMOS_HOST
    return host_timer_counts();
#elif defined(DIMOS_SANDBOX)
    return elapsed_input_counts;
#else
    u32 low_before;
    u32 high;
    u32 low_after;
    do {
        low_before = timer_irq_low;
        high = timer_irq_high;
        low_after = timer_irq_low;
    } while (low_before != low_after);
    return ((u64)high << 32u) | low_before;
#endif
}

u16 timer_take_ticks(void) {
    const u16 ticks = ticks_waiting;

    ticks_waiting = 0u;
    return ticks;
}

void time_wait(u32 milliseconds) {
#ifdef DIMOS_HOST
    /* Sleep for real instead of spinning a whole CPU core on a phone. */
    host_sleep_milliseconds(milliseconds);
    timer_update();
#else
    const u32 start = elapsed_milliseconds;

    while ((elapsed_milliseconds - start) < milliseconds) {
        timer_update();
        input_poll();
#ifndef DIMOS_SANDBOX
        cpu_halt();
#endif
    }
#endif
}

/* ------------------------------------------------------------------ */
/* The clock chip                                                      */
/* ------------------------------------------------------------------ */

#define CMOS_SECONDS 0x00u
#define CMOS_MINUTES 0x02u
#define CMOS_HOURS 0x04u
#define CMOS_DAY 0x07u
#define CMOS_MONTH 0x08u
#define CMOS_YEAR 0x09u
#define CMOS_CENTURY 0x32u
#define CMOS_STATUS_B 0x0Bu

#define CMOS_BINARY_MODE 0x04u
#define CMOS_TWENTY_FOUR_HOURS 0x02u
#define CMOS_HOUR_IS_PM 0x80u

static u8 stored_seconds;
static u8 stored_minutes;
static u8 stored_hours;
static u8 stored_day;
static u8 stored_month;
static u16 stored_year;
static u32 seconds_at_boot;

static u8 cmos_read(u8 index) {
    port_write_byte(PORT_CMOS_ADDRESS, index);
    return port_read_byte(PORT_CMOS_DATA);
}

static u8 cmos_value(u8 raw, u8 binary_mode) {
    if (binary_mode != 0u) {
        return raw;
    }
    /* The chip normally answers in binary coded decimal: 0x25 means 25. */
    return (u8)(((raw >> 4) * 10u) + (raw & 0x0Fu));
}

void clock_init(void) {
    const u8 status = cmos_read(CMOS_STATUS_B);
    const u8 binary = (u8)((status & CMOS_BINARY_MODE) != 0u);
    u8 hours = cmos_value(cmos_read(CMOS_HOURS), binary);
    u8 century;

    if ((status & CMOS_TWENTY_FOUR_HOURS) == 0u &&
        (hours & CMOS_HOUR_IS_PM) != 0u) {
        hours = (u8)((hours & (u8)~CMOS_HOUR_IS_PM) + 12u);
    }

    stored_seconds = cmos_value(cmos_read(CMOS_SECONDS), binary);
    stored_minutes = cmos_value(cmos_read(CMOS_MINUTES), binary);
    stored_hours = hours;
    stored_day = cmos_value(cmos_read(CMOS_DAY), binary);
    stored_month = cmos_value(cmos_read(CMOS_MONTH), binary);

    century = cmos_read(CMOS_CENTURY);
    if (century >= 0x19u && century <= 0x21u) {
        century = (u8)(((century >> 4) * 10u) + (century & 0x0Fu));
    } else {
        century = 20u; /* no century register: assume the 2000s */
    }
    stored_year = (u16)((u16)century * 100u + cmos_value(cmos_read(CMOS_YEAR), binary));

    if (stored_hours > 23u || stored_minutes > 59u || stored_seconds > 59u ||
        stored_day < 1u || stored_day > 31u || stored_month < 1u || stored_month > 12u) {
        /* The clock chip was unset; start from a friendly default. */
        stored_hours = 12u;
        stored_minutes = 0u;
        stored_seconds = 0u;
        stored_day = 1u;
        stored_month = 1u;
        stored_year = 2025u;
    }

    seconds_at_boot = ((u32)stored_hours * 3600u) +
                      ((u32)stored_minutes * 60u) + (u32)stored_seconds;
}

static u32 clock_total_seconds(void) {
    return seconds_at_boot + (elapsed_milliseconds / 1000u);
}

u8 clock_seconds(void) {
    return (u8)(clock_total_seconds() % 60u);
}

u8 clock_minutes(void) {
    return (u8)((clock_total_seconds() / 60u) % 60u);
}

u8 clock_hours(void) {
    return (u8)((clock_total_seconds() / 3600u) % 24u);
}

u8 clock_day(void) {
    return stored_day;
}

u8 clock_month(void) {
    return stored_month;
}

u16 clock_year(void) {
    return stored_year;
}

/* ------------------------------------------------------------------ */
/* Restart                                                             */
/* ------------------------------------------------------------------ */

void system_restart(void) {
    u32 guard;

    sound_stop();
#ifdef DIMOS_HOST
    /* Restart in a hosted build means "start me again", which the host
     * reports to whoever launched the runtime. */
    host_restart_process();
#endif

    /* The keyboard controller can reset the whole machine. Wait until it is
     * ready for a command, then ask for the reset pulse. */
    for (guard = 0u; guard < 200000u; ++guard) {
        if ((port_read_byte(PORT_KEYBOARD_STATUS) & 0x02u) == 0u) {
            break;
        }
    }
    port_write_byte(PORT_KEYBOARD_STATUS, PORT_KEYBOARD_RESET);

    for (;;) {
        /* Nothing sensible left to do if the reset did not take. */
    }
}

/* ------------------------------------------------------------------ */
/* Start up                                                            */
/* ------------------------------------------------------------------ */

/* Port E9 is the traditional Bochs/QEMU debug console. Real hardware simply
 * ignores it; CI uses the marker to prove the built image reached protected
 * mode C, initialized the selected framebuffer and did not triple-fault. */
static void debug_marker(const char *text) {
    while (*text != '\0') {
        port_write_byte(0x00E9u, (u8)*text);
        ++text;
    }
}

u8 system_shutdown(void) {
    sound_stop();
#ifdef DIMOS_HOST
    host_exit_process(0);
    return 1u;
#elif defined(DIMOS_SANDBOX)
    debug_marker("DIMOS:SHUTDOWN\n");
    return 0u; /* the simulator has no ACPI power controller */
#else
    /* ACPI S5 powers down machines whose firmware exposes the standard tables.
     * If unavailable, leave the desktop running so the user can still restart. */
    if (acpi_power_off() == 0u) {
        return 0u;
    }
    cpu_halt_forever();
    return 1u;
#endif
}

void kernel_main(void) {
#ifndef DIMOS_HOST
    /* The flat binary carries no BSS, so the kernel clears it by hand. The
     * hosted build is an ordinary program and its loader already did. */
    memory_zero(__bss_start, (u32)(__bss_end - __bss_start));
#endif
    debug_marker("DIMOS:PROTECTED\n");

    timer_init();
    ram_disk_init();
    file_system_init();
    clock_init();

    /* The disk comes up before the screen: it decides whether DimOS can
     * remember anything, and the saved settings are applied to the very
     * first frame the user sees. */
    if (volume_init() != 0u) {
        debug_marker("DIMOS:DISK:VOLUME\n");
        settings_load();
    } else if (ata_present() != 0u) {
        debug_marker("DIMOS:DISK:BLANK\n");
    } else {
        debug_marker("DIMOS:DISK:NONE\n");
    }

#ifdef DIMOS_HOST
    /* Give the hosted runtime its chance to seed a fresh volume (a README
     * and the bundled Lua applications) and to refresh the windows fs.c
     * reads, before the font and the file manager look at them. */
    host_after_mount();
#endif

    gfx_init();
    settings_apply();
    input_init();
    sound_init();
    if (video_backend == VIDEO_BACKEND_VBE) {
        debug_marker("DIMOS:READY:VBE\n");
    } else {
        debug_marker("DIMOS:READY:VGA\n");
    }
#ifdef DIMOS_HOST
    /* The disk is mounted, the font is rasterized and the first frame can be
     * drawn: a good moment for the hosted runtime to say hello. */
    host_shell_ready();
#endif

    gui_run();

    for (;;) {
        /* gui_run never returns. */
    }
}
