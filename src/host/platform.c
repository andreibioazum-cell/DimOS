/*
 * platform.c -- the pretend PC under the hosted runtime.
 *
 * DimOS's kernel does not know what a syscall is: it talks to the 8042
 * controller, an IDE controller, the PIT, the CMOS clock and a linear
 * framebuffer. This file is the only place that turns those into host
 * calls, so the shell, the applications, the FAT12 driver and the TrueType
 * rasterizer all run unmodified on a phone or a laptop.
 *
 * The model is deliberately the same one tools/sandbox/hardware_sim.c uses
 * for the Unicorn test run; the difference is the backing store. Here the
 * IDE disk is a real file that can be mounted on Linux or booted in QEMU,
 * the PIT is the host's monotonic clock, the CMOS chip is today's date and
 * the keyboard and mouse queues are fed by the web front end.
 */

/* clock_gettime(), nanosleep(), localtime_r() and inet_pton() are POSIX,
 * and -std=c11 hides them unless the program asks for them. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "dimos_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Memory the kernel addresses by number                               */
/* ------------------------------------------------------------------ */

#define HOST_RAM_DISK_BYTES (4u * 1024u * 1024u)
#define HOST_BOOT_WINDOW_BYTES 512u
#define HOST_FAT_WINDOW_BYTES (18u * 512u)
#define HOST_ROOT_WINDOW_BYTES (14u * 512u)
#define HOST_DATA_WINDOW_BYTES (256u * 512u)
#define HOST_FONT_AREA_BYTES (768u * 1024u)
#define HOST_FONT_WORK_BYTES (1u * 1024u * 1024u)

/* The four windows fs.c reads instead of low memory, and the 4 MiB RAM
 * disk Paint draws on. */
const u8 *host_boot_window;
const u8 *host_fat_window;
const u8 *host_root_window;
const u8 *host_data_window;
u8 *host_ram_disk;
static u8 boot_window[HOST_BOOT_WINDOW_BYTES];
static u8 fat_window[HOST_FAT_WINDOW_BYTES];
static u8 root_window[HOST_ROOT_WINDOW_BYTES];
static u8 data_window[HOST_DATA_WINDOW_BYTES];

static u8 *framebuffer;
u32 bios_font_address; /* 0: the hosted build always finds a TrueType font */

/* The kernel's video variables. kernel.asm normally fills these after
 * asking VBE for a mode; here the runtime chooses the mode itself. */
u8 video_backend = VIDEO_BACKEND_VBE;
u32 video_framebuffer_address;
u16 video_pitch;
u16 video_width;
u16 video_height;
u8 video_bits_per_pixel = 32u;

static HostOptions options;

/* ------------------------------------------------------------------ */
/* Console, clock, sleep                                               */
/* ------------------------------------------------------------------ */

void host_console(const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    fputs("dim: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

u64 host_microseconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (u64)((counter.QuadPart * 1000000LL) / frequency.QuadPart);
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000000ull + (u64)(now.tv_nsec / 1000);
#endif
}

void host_sleep(u32 milliseconds) {
    if (milliseconds == 0u) {
        return;
    }
#ifdef _WIN32
    Sleep(milliseconds);
#else
    {
        struct timespec delay;

        delay.tv_sec = (time_t)(milliseconds / 1000u);
        delay.tv_nsec = (long)((milliseconds % 1000u) * 1000000u);
        nanosleep(&delay, (struct timespec *)0);
    }
#endif
}

void host_sleep_milliseconds(u32 milliseconds) {
    host_sleep(milliseconds);
}

/* The PIT would deliver 1,193,182 counts per second. The frame limiter and
 * the millisecond tick both derive from this, so it has to look like the
 * real chip rather than like a millisecond counter. */
u64 host_timer_counts(void) {
    return (host_microseconds() * 1193182ull) / 1000000ull;
}

void host_request_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    exit(code);
}

void host_exit_process(int code) {
    host_request_exit(code);
}

void host_restart_process(void) {
    /* Whoever launched the runtime can start it again on status 42. */
    host_request_exit(42);
}

/* ------------------------------------------------------------------ */
/* IDE disk, backed by a real file                                     */
/* ------------------------------------------------------------------ */

#define IDE_STATUS_ERROR 0x01u
#define IDE_STATUS_DRIVE_READY 0x40u
#define IDE_STATUS_DATA_REQUEST 0x08u

static FILE *disk_file;
static u32 disk_sector_count;
static u8 disk_writable;
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

static u32 host_file_size(FILE *handle) {
    long size;

    if (fseek(handle, 0, SEEK_END) != 0) {
        return 0u;
    }
    size = ftell(handle);
    if (size < 0) {
        return 0u;
    }
    return (u32)size;
}

static u8 disk_read_sector(u32 lba, u8 *target) {
    if (disk_file == (FILE *)0 || lba >= disk_sector_count) {
        memory_zero(target, 512u);
        return 0u;
    }
    if (fseek(disk_file, (long)(lba * 512u), SEEK_SET) != 0) {
        memory_zero(target, 512u);
        return 0u;
    }
    if (fread(target, 1u, 512u, disk_file) != 512u) {
        memory_zero(target, 512u);
        return 0u;
    }
    return 1u;
}

static u8 disk_write_sector(u32 lba, const u8 *source) {
    if (disk_file == (FILE *)0 || disk_writable == 0u ||
        lba >= disk_sector_count) {
        return 0u;
    }
    if (fseek(disk_file, (long)(lba * 512u), SEEK_SET) != 0) {
        return 0u;
    }
    return (u8)(fwrite(source, 1u, 512u, disk_file) == 512u);
}

static int disk_create(const char *path, u32 bytes) {
    FILE *handle = fopen(path, "wb");
    u8 chunk[4096];
    u32 written = 0u;

    if (handle == (FILE *)0) {
        return 0;
    }
    memory_zero(chunk, sizeof(chunk));
    while (written < bytes) {
        u32 step = bytes - written;

        if (step > (u32)sizeof(chunk)) {
            step = (u32)sizeof(chunk);
        }
        if (fwrite(chunk, 1u, step, handle) != step) {
            fclose(handle);
            return 0;
        }
        written += step;
    }
    fclose(handle);
    return 1;
}

static int disk_open(const char *path, u32 bytes, u8 allow_format) {
    u32 size;

    disk_file = fopen(path, "r+b");
    if (disk_file == (FILE *)0 && allow_format != 0u) {
        host_console("%s not found; creating a %u KiB volume", path,
                     (unsigned)(bytes / 1024u));
        if (disk_create(path, bytes) == 0) {
            return 0;
        }
        disk_file = fopen(path, "r+b");
    }
    if (disk_file == (FILE *)0) {
        return 0;
    }
    size = host_file_size(disk_file);
    if (size < HOST_DISK_MIN_BYTES) {
        /* A zero-length or truncated image cannot hold a FAT12 volume. */
        fclose(disk_file);
        disk_file = (FILE *)0;
        return 0;
    }
    if (size > HOST_DISK_MAX_BYTES) {
        size = HOST_DISK_MAX_BYTES;
    }
    disk_sector_count = size / 512u;
    disk_writable = 1u;
    return 1;
}

static u32 ide_lba(void) {
    return (u32)ide_lba_low | ((u32)ide_lba_mid << 8u) |
           ((u32)ide_lba_high << 16u) | ((u32)(ide_drive & 0x0Fu) << 24u);
}

static void ide_load_sector(u32 lba) {
    (void)disk_read_sector(lba, ide_buffer);
    ide_position = 0u;
    ide_direction = 1u;
    ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_DATA_REQUEST;
}

static void ide_identify(void) {
    u16 index;
    const char *model = "DIMOS HOSTED VOLUME                   ";

    memory_zero(ide_buffer, 512u);
    for (index = 0u; index < 40u; ++index) {
        const char *pair = model + index * 2u;

        /* IDENTIFY keeps every character pair the other way round, which is
         * why ata.c reads the model word by word and swaps the bytes. */
        ide_buffer[27u * 2u + index * 2u] = (u8)pair[1];
        ide_buffer[27u * 2u + index * 2u + 1u] = (u8)pair[0];
    }
    ide_buffer[49u * 2u] = 0x00u; /* LBA supported */
    ide_buffer[49u * 2u + 1u] = 0x02u;
    ide_buffer[60u * 2u] = (u8)(disk_sector_count & 0xFFu);
    ide_buffer[60u * 2u + 1u] = (u8)((disk_sector_count >> 8u) & 0xFFu);
    ide_buffer[61u * 2u] = (u8)((disk_sector_count >> 16u) & 0xFFu);
    ide_buffer[61u * 2u + 1u] = (u8)((disk_sector_count >> 24u) & 0xFFu);
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
            ide_next_lba = ide_lba();
            if (ide_sectors_left == 0u || ide_next_lba >= disk_sector_count) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                ide_error = 0x10u;
                return;
            }
            ide_load_sector(ide_next_lba);
            return;
        case 0x30u: /* WRITE SECTORS */
            ide_sectors_left = (u8)(ide_count & 0xFFu);
            ide_next_lba = ide_lba();
            if (ide_sectors_left == 0u || ide_next_lba >= disk_sector_count ||
                disk_writable == 0u) {
                ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_ERROR;
                ide_error = 0x10u;
                return;
            }
            ide_position = 0u;
            ide_direction = 2u;
            ide_status = IDE_STATUS_DRIVE_READY | IDE_STATUS_DATA_REQUEST;
            return;
        case 0xE7u: /* FLUSH CACHE */
            if (disk_file != (FILE *)0) {
                fflush(disk_file);
            }
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
        (void)disk_write_sector(ide_next_lba, ide_buffer);
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

/* ------------------------------------------------------------------ */
/* PS/2 controller                                                     */
/* ------------------------------------------------------------------ */

#define HOST_KEY_QUEUE 256u
#define HOST_MOUSE_QUEUE 1024u

static u8 key_queue[HOST_KEY_QUEUE];
static u32 key_head;
static u32 key_tail;
static u8 mouse_queue[HOST_MOUSE_QUEUE];
static u32 mouse_head;
static u32 mouse_tail;

static u8 controller_expect_mouse_command;
static u8 controller_acknowledgement_waiting;
static u8 controller_mouse_id_waiting;

static void key_enqueue(u8 value) {
    const u32 next = (key_head + 1u) % HOST_KEY_QUEUE;

    if (next == key_tail) {
        return;
    }
    key_queue[key_head] = value;
    key_head = next;
}

static void mouse_enqueue(u8 value) {
    const u32 next = (mouse_head + 1u) % HOST_MOUSE_QUEUE;

    if (next == mouse_tail) {
        return;
    }
    mouse_queue[mouse_head] = value;
    mouse_head = next;
}

static u8 mouse_packet_waiting(void) {
    return (u8)(mouse_tail != mouse_head);
}

static u8 key_waiting(void) {
    return (u8)(key_tail != key_head);
}

/* The hosted mouse introduces itself as an IntelliMouse (device id 3), so
 * every packet is four bytes and the fourth one carries the wheel. dx and
 * dy are screen deltas, so a positive dy moves the pointer down; the packet
 * itself stores the vertical movement the other way round, like a real
 * mouse. */
static void mouse_packet_with_wheel(int dx, int dy, u8 buttons, s8 wheel) {
    const int packet_dy = -dy;
    u8 flags = (u8)(0x08u | (u8)(buttons & 0x07u));

    if (dx < 0) {
        flags |= 0x10u;
    }
    if (packet_dy < 0) {
        flags |= 0x20u;
    }
    mouse_enqueue(flags);
    mouse_enqueue((u8)(dx & 0xFF));
    mouse_enqueue((u8)(packet_dy & 0xFF));
    mouse_enqueue((u8)(wheel & 0x0F));
}

static void mouse_packet(int dx, int dy, u8 buttons) {
    mouse_packet_with_wheel(dx, dy, buttons, 0);
}

static u8 pointer_buttons;

/* A mirror of where the kernel thinks the pointer is.
 *
 * A PS/2 mouse only reports deltas, and the kernel applies them when it
 * polls the controller in its own frame loop -- which cannot run while the
 * runtime is the one calling in. So the runtime keeps the same arithmetic
 * the kernel does (add the delta, clamp to 844x390) and sends the resulting
 * deltas. Both sides start centred on the canvas, and the packets are never
 * dropped while the queue is short, so the mirror stays exact. */
static s16 pointer_mirror_x = (s16)(SCREEN_WIDTH / 2u);
static s16 pointer_mirror_y = (s16)(SCREEN_HEIGHT / 2u);

static void mirror_move_to(s16 x, s16 y) {
    if (x < 0) {
        x = 0;
    }
    if (x > (s16)(SCREEN_WIDTH - 1u)) {
        x = (s16)(SCREEN_WIDTH - 1u);
    }
    if (y < 0) {
        y = 0;
    }
    if (y > (s16)(SCREEN_HEIGHT - 1u)) {
        y = (s16)(SCREEN_HEIGHT - 1u);
    }
    pointer_mirror_x = x;
    pointer_mirror_y = y;
}

void host_input_move(s16 x, s16 y) {
    if (x < 0) {
        x = 0;
    }
    if (x > (s16)(SCREEN_WIDTH - 1u)) {
        x = (s16)(SCREEN_WIDTH - 1u);
    }
    if (y < 0) {
        y = 0;
    }
    if (y > (s16)(SCREEN_HEIGHT - 1u)) {
        y = (s16)(SCREEN_HEIGHT - 1u);
    }
    while (pointer_mirror_x != x || pointer_mirror_y != y) {
        const int remaining_x = (int)x - (int)pointer_mirror_x;
        const int remaining_y = (int)y - (int)pointer_mirror_y;
        int dx = remaining_x > 100 ? 100 : (remaining_x < -100 ? -100
                                                               : remaining_x);
        int dy = remaining_y > 100 ? 100 : (remaining_y < -100 ? -100
                                                               : remaining_y);

        mirror_move_to((s16)(pointer_mirror_x + dx),
                       (s16)(pointer_mirror_y + dy));
        mouse_packet(dx, dy, pointer_buttons);
    }
}

void host_input_set_buttons(s16 x, s16 y, u8 buttons) {
    host_input_move(x, y);
    pointer_buttons = (u8)(buttons & 0x07u);
    mouse_packet(0, 0, pointer_buttons);
}

void host_input_click(s16 x, s16 y, u8 buttons) {
    host_input_move(x, y);
    pointer_buttons = (u8)(buttons & 0x07u);
    mouse_packet(0, 0, pointer_buttons);
    pointer_buttons = 0u;
    mouse_packet(0, 0, 0u);
}

void host_input_wheel(s16 x, s16 y, s8 notches) {
    host_input_move(x, y);
    mouse_packet_with_wheel(0, 0, pointer_buttons, notches);
}

/* Scan code set 1. Letters map to their own code; everything that needs the
 * shift key is marked with 0x80 in the flag byte. */
typedef struct {
    u16 key;
    u8 scan;
    u8 shifted;
} KeyMapping;

static const KeyMapping key_mappings[] = {
    { 'a', 0x1E, 0 }, { 'b', 0x30, 0 }, { 'c', 0x2E, 0 }, { 'd', 0x20, 0 },
    { 'e', 0x12, 0 }, { 'f', 0x21, 0 }, { 'g', 0x22, 0 }, { 'h', 0x23, 0 },
    { 'i', 0x17, 0 }, { 'j', 0x24, 0 }, { 'k', 0x25, 0 }, { 'l', 0x26, 0 },
    { 'm', 0x32, 0 }, { 'n', 0x31, 0 }, { 'o', 0x18, 0 }, { 'p', 0x19, 0 },
    { 'q', 0x10, 0 }, { 'r', 0x13, 0 }, { 's', 0x1F, 0 }, { 't', 0x14, 0 },
    { 'u', 0x16, 0 }, { 'v', 0x2F, 0 }, { 'w', 0x11, 0 }, { 'x', 0x2D, 0 },
    { 'y', 0x15, 0 }, { 'z', 0x2C, 0 },
    { '1', 0x02, 0 }, { '2', 0x03, 0 }, { '3', 0x04, 0 }, { '4', 0x05, 0 },
    { '5', 0x06, 0 }, { '6', 0x07, 0 }, { '7', 0x08, 0 }, { '8', 0x09, 0 },
    { '9', 0x0A, 0 }, { '0', 0x0B, 0 }, { ' ', 0x39, 0 },
    { '\n', 0x1C, 0 }, { '\r', 0x1C, 0 }, { '\b', 0x0E, 0 }, { '\t', 0x0F, 0 },
    { '.', 0x34, 0 }, { ',', 0x33, 0 }, { '/', 0x35, 0 }, { ';', 0x27, 0 },
    { '\'', 0x28, 0 }, { '[', 0x1A, 0 }, { ']', 0x1B, 0 }, { '\\', 0x2B, 0 },
    { '-', 0x0C, 0 }, { '=', 0x0D, 0 }, { '`', 0x29, 0 },
    { '!', 0x02, 1 }, { '@', 0x03, 1 }, { '#', 0x04, 1 }, { '$', 0x05, 1 },
    { '%', 0x06, 1 }, { '^', 0x07, 1 }, { '&', 0x08, 1 }, { '*', 0x09, 1 },
    { '(', 0x0A, 1 }, { ')', 0x0B, 1 }, { '_', 0x0C, 1 }, { '+', 0x0D, 1 },
    { '{', 0x1A, 1 }, { '}', 0x1B, 1 }, { '|', 0x2B, 1 }, { ':', 0x27, 1 },
    { '"', 0x28, 1 }, { '<', 0x33, 1 }, { '>', 0x34, 1 }, { '?', 0x35, 1 },
    { '~', 0x29, 1 }
};

static void key_press(u8 scan, u8 shifted) {
    if (shifted != 0u) {
        key_enqueue(0x2Au);
    }
    key_enqueue(scan);
    key_enqueue((u8)(scan | 0x80u));
    if (shifted != 0u) {
        key_enqueue(0xAAu);
    }
}

void host_input_key(u16 key) {
    u32 index;
    u8 force_shift = 0u;

    /* Uppercase letters are the same scan code with shift held down. */
    if (key >= (u16)'A' && key <= (u16)'Z') {
        key = (u16)(key - (u16)'A' + (u16)'a');
        force_shift = 1u;
    }

    switch (key) {
        case KEY_ENTER: key_press(0x1Cu, 0u); return;
        case KEY_ESCAPE: key_press(0x01u, 0u); return;
        case KEY_BACKSPACE: key_press(0x0Eu, 0u); return;
        case KEY_TAB: key_press(0x0Fu, 0u); return;
        case KEY_DELETE: key_press(0x53u, 0u); return;
        case KEY_ARROW_UP: key_enqueue(0xE0u); key_press(0x48u, 0u); return;
        case KEY_ARROW_DOWN: key_enqueue(0xE0u); key_press(0x50u, 0u); return;
        case KEY_ARROW_LEFT: key_enqueue(0xE0u); key_press(0x4Bu, 0u); return;
        case KEY_ARROW_RIGHT: key_enqueue(0xE0u); key_press(0x4Du, 0u); return;
        default: break;
    }
    for (index = 0u; index < sizeof(key_mappings) / sizeof(key_mappings[0]);
         ++index) {
        if (key_mappings[index].key == key) {
            key_press(key_mappings[index].scan,
                      (u8)(key_mappings[index].shifted | force_shift));
            return;
        }
    }
}

/* Typing is paced, not dumped.
 *
 * The kernel drains the 8042 controller once per event loop pass and keeps
 * sixteen events in its own queue; pushing a whole sentence at once would
 * overrun that queue and silently drop keystrokes. So host_input_text()
 * only remembers the text and host_wait_idle() feeds one character every
 * couple of milliseconds -- faster than a person types, slow enough that
 * nothing is lost. */
static char pending_text[256];
static u32 pending_length;
static u32 pending_position;
static u64 pending_next_microseconds;

void host_input_text(const char *text) {
    /* Anything still being typed comes first, then the new text. */
    if (pending_position < pending_length) {
        pending_position = pending_length;
    }
    pending_length = 0u;
    pending_position = 0u;
    while (*text != '\0' && pending_length + 1u < sizeof(pending_text)) {
        pending_text[pending_length++] = *text++;
    }
    pending_next_microseconds = host_microseconds();
}

static void pending_text_feed(void) {
    if (pending_position >= pending_length) {
        return;
    }
    if (host_microseconds() < pending_next_microseconds) {
        return;
    }
    host_input_key((u16)(u8)pending_text[pending_position]);
    ++pending_position;
    /* Eight characters per 60 Hz frame is well inside the kernel's queue. */
    pending_next_microseconds = host_microseconds() + 2000ull;
    if (pending_position >= pending_length) {
        pending_length = 0u;
        pending_position = 0u;
    }
}

/* ------------------------------------------------------------------ */
/* The 8042 controller, the PIT, the CMOS clock, the debug port        */
/* ------------------------------------------------------------------ */

static u8 cmos_index;
static u8 cmos_cache[64];
static u8 cmos_cached;
static u32 cmos_cached_second;

static void cmos_refresh(void) {
    const time_t now = time((time_t *)0);
    const u32 second = (u32)now;
    struct tm broken;

    if (cmos_cached != 0u && second == cmos_cached_second) {
        return;
    }
#ifdef _WIN32
    {
        struct tm *local = localtime(&now);

        if (local != (struct tm *)0) {
            broken = *local;
        } else {
            memory_zero(&broken, sizeof(broken));
        }
    }
#else
    localtime_r(&now, &broken);
#endif
    memory_zero(cmos_cache, sizeof(cmos_cache));
    cmos_cache[0x00] = (u8)(((broken.tm_sec / 10) << 4) | (broken.tm_sec % 10));
    cmos_cache[0x02] = (u8)(((broken.tm_min / 10) << 4) | (broken.tm_min % 10));
    cmos_cache[0x04] = (u8)(((broken.tm_hour / 10) << 4) | (broken.tm_hour % 10));
    cmos_cache[0x07] = (u8)(((broken.tm_mday / 10) << 4) | (broken.tm_mday % 10));
    cmos_cache[0x08] = (u8)((((broken.tm_mon + 1) / 10) << 4) |
                            ((broken.tm_mon + 1) % 10));
    {
        const int year = (broken.tm_year + 1900) % 100;
        const int century = (broken.tm_year + 1900) / 100;

        cmos_cache[0x09] = (u8)(((year / 10) << 4) | (year % 10));
        cmos_cache[0x32] = (u8)(((century / 10) << 4) | (century % 10));
    }
    /* Bit 1: twenty-four hour mode. Bit 2 clear: binary coded decimal. */
    cmos_cache[0x0B] = 0x02u;
    cmos_cached = 1u;
    cmos_cached_second = second;
}

static char debug_log[1024];
static u32 debug_length;

u32 host_debug_lines(char *out, u32 capacity) {
    u32 count = debug_length;

    if (count > capacity - 1u) {
        count = capacity - 1u;
    }
    memory_copy(out, debug_log, count);
    out[count] = '\0';
    return count;
}

u8 port_read_byte(u16 port) {
    switch (port) {
        case PORT_KEYBOARD_STATUS: {
            u8 status = 0u;

            if (controller_acknowledgement_waiting != 0u ||
                controller_mouse_id_waiting != 0u) {
                status |= 0x01u | 0x20u;
            } else if (key_waiting() != 0u) {
                status |= 0x01u;
            } else if (mouse_packet_waiting() != 0u) {
                status |= 0x01u | 0x20u;
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
                return 3u; /* IntelliMouse: a fourth, wheel byte follows */
            }
            if (key_waiting() != 0u) {
                const u8 value = key_queue[key_tail];

                key_tail = (key_tail + 1u) % HOST_KEY_QUEUE;
                return value;
            }
            if (mouse_packet_waiting() != 0u) {
                const u8 value = mouse_queue[mouse_tail];

                mouse_tail = (mouse_tail + 1u) % HOST_MOUSE_QUEUE;
                return value;
            }
            return 0u;

        case PORT_PIT_CHANNEL_0:
        case PORT_PIT_CHANNEL_2:
            return 0u;

        case PORT_SPEAKER:
            return 0u;

        case 0x01F0u:
            return (u8)ide_read_word();
        case 0x01F1u:
            return ide_error;
        case 0x01F7u:
        case 0x03F6u:
            return ide_status;

        case PORT_CMOS_DATA:
            cmos_refresh();
            return (cmos_index < sizeof(cmos_cache)) ? cmos_cache[cmos_index]
                                                     : 0u;

        default:
            return 0u;
    }
}

u16 port_read_word(u16 port) {
    if (port == 0x01F0u) {
        return ide_read_word();
    }
    return (u16)(port_read_byte(port) |
                 ((u16)port_read_byte(port) << 8u));
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
            }
            return;

        case PORT_PIT_COMMAND:
        case PORT_PIT_CHANNEL_0:
        case PORT_PIT_CHANNEL_2:
        case PORT_SPEAKER:
        case PORT_VGA_DAC_WRITE_INDEX:
        case PORT_VGA_DAC_DATA:
        case 0x03F6u:
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

        case PORT_CMOS_ADDRESS:
            cmos_index = value;
            return;

        case 0x00E9u:
            if (debug_length < sizeof(debug_log) - 1u) {
                debug_log[debug_length] = (char)value;
                ++debug_length;
                debug_log[debug_length] = '\0';
            }
            return;

        default:
            return;
    }
}

void cpu_halt_forever(void) {
    for (;;) {
        host_sleep(1000u);
    }
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

u8 *host_framebuffer_pointer(void) {
    return framebuffer;
}

u16 host_display_width(void) {
    return video_width;
}

u16 host_display_height(void) {
    return video_height;
}

static int display_start(u16 width, u16 height) {
    if (width < SCREEN_WIDTH) {
        width = SCREEN_WIDTH;
    }
    if (height < SCREEN_HEIGHT) {
        height = SCREEN_HEIGHT;
    }
    framebuffer = (u8 *)calloc((size_t)width * height, 4u);
    if (framebuffer == (u8 *)0) {
        return 0;
    }
    video_width = width;
    video_height = height;
    video_pitch = (u16)(width * 4u);
    video_bits_per_pixel = 32u;
    video_backend = VIDEO_BACKEND_VBE;
    /* Only used as the "there is a framebuffer" flag in the kernel; the
     * pixels themselves are reached through dimos_framebuffer(). */
    video_framebuffer_address = 0x10000000u;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Volume windows and seeding                                          */
/* ------------------------------------------------------------------ */

/* A word from a little-endian structure, the way fs.c and volume.c read
 * the boot sector. */
static u16 sector_word(const u8 *sector, u32 offset) {
    return (u16)((u16)sector[offset] |
                 (u16)((u16)sector[offset + 1u] << 8u));
}

/* Copies the four windows fs.c reads. The layout comes from the BPB rather
 * than from the classic floppy numbers, because the hosted runtime may be
 * looking at a volume this very kernel formatted a minute ago: volume.c
 * picks its own sectors-per-cluster, so the root directory does not always
 * start at sector 19 the way it does on the boot media. */
static void windows_load(void) {
    u32 index;
    u32 fat_start = 1u;
    u32 root_start = 19u;
    u32 root_sectors = 14u;
    u32 data_start = 33u;
    u32 reserved;
    u32 fat_count;
    u32 fat_sectors;
    u32 root_entries;

    host_boot_window = boot_window;
    host_fat_window = fat_window;
    host_root_window = root_window;
    host_data_window = data_window;

    if (disk_file == (FILE *)0) {
        return;
    }
    (void)ata_read(0u, boot_window, 1u);
    if (boot_window[510] == 0x55u && boot_window[511] == 0xAAu &&
        sector_word(boot_window, 11u) == 512u) {
        reserved = sector_word(boot_window, 14u);
        fat_count = boot_window[16];
        root_entries = sector_word(boot_window, 17u);
        fat_sectors = sector_word(boot_window, 22u);
        if (reserved != 0u && fat_count != 0u && fat_sectors != 0u &&
            root_entries != 0u) {
            fat_start = reserved;
            root_start = reserved + fat_count * fat_sectors;
            root_sectors = (root_entries * 32u + 511u) / 512u;
            data_start = root_start + root_sectors;
        }
    }
    for (index = 0u; index < HOST_FAT_WINDOW_BYTES / 512u; ++index) {
        (void)ata_read(fat_start + index, fat_window + index * 512u, 1u);
    }
    if (root_sectors > HOST_ROOT_WINDOW_BYTES / 512u) {
        root_sectors = HOST_ROOT_WINDOW_BYTES / 512u;
    }
    for (index = 0u; index < root_sectors; ++index) {
        (void)ata_read(root_start + index, root_window + index * 512u, 1u);
    }
    for (index = 0u; index < HOST_DATA_WINDOW_BYTES / 512u; ++index) {
        (void)ata_read(data_start + index, data_window + index * 512u, 1u);
    }
}

u8 host_volume_is_blank(void) {
    u8 sector[512];

    if (disk_read_sector(0u, sector) == 0u) {
        return 1u;
    }
    if (sector[510] != 0x55u || sector[511] != 0xAAu) {
        return 1u;
    }
    return (u8)(memcmp(sector + 54, "FAT12   ", 8) != 0);
}

/* A short welcome note and the Lua applications that ship with the hosted
 * runtime. They are written to the volume once, so they are ordinary files
 * that the Files app, the Terminal and Lua can all see. */
static const char readme_text[] =
    "DimOS 14 -- hosted edition\n"
    "\n"
    "This is the real DimOS kernel running as a normal program on this\n"
    "machine instead of on a PC with a BIOS. Same shell, same nine apps,\n"
    "same FAT12 disk driver, same TrueType rasterizer.\n"
    "\n"
    "The browser window is the phone: tap the screen or the home bar to\n"
    "unlock, tap the rounded icons to open an application. A long press\n"
    "works as a right click, which is how Mines places flags.\n"
    "\n"
    "This volume is a real FAT12 image. Mount it on Linux, open it with\n"
    "mtools, or boot it in QEMU -- the files are the same files.\n"
    "\n"
    "Lua applications live here too: HELLO.LUA and SYSINFO.LUA. Run them\n"
    "from a terminal with\n"
    "\n"
    "    dimos-anywhere run HELLO.LUA\n"
    "\n"
    "or from the Lua box in the browser window. Lua applications have no\n"
    "GUI: they read and write files, inspect the machine, print, sleep and\n"
    "return an exit code.\n";

static const char hello_lua[] =
    "-- HELLO.LUA -- the smallest DimOS Lua application.\n"
    "-- No GUI: a Lua app talks to the system through the dim module.\n"
    "\n"
    "local clock = dim.clock()\n"
    "dim.print(\"Hello from \" .. _VERSION .. \" on \" .. dim.platform)\n"
    "dim.print(string.format(\"It is %02d:%02d on %04d-%02d-%02d\",\n"
    "    clock.hour, clock.minute, clock.year, clock.month, clock.day))\n"
    "\n"
    "local note = string.format(\"hello written by lua at %02d:%02d:%02d\\n\",\n"
    "    clock.hour, clock.minute, clock.second)\n"
    "if dim.file.write(\"HELLO.TXT\", note) then\n"
    "    dim.print(\"wrote HELLO.TXT (\" .. #note .. \" bytes) to the volume\")\n"
    "else\n"
    "    dim.print(\"could not write HELLO.TXT\")\n"
    "end\n"
    "\n"
    "for _, file in ipairs(dim.file.list()) do\n"
    "    dim.print(string.format(\"  %-12s %6d bytes\", file.name, file.size))\n"
    "end\n";

static const char sysinfo_lua[] =
    "-- SYSINFO.LUA -- everything the shell knows about this machine.\n"
    "\n"
    "local info = dim.info()\n"
    "dim.print(\"DimOS \" .. dim.version .. \" on \" .. dim.platform)\n"
    "dim.print(\"screen   \" .. info.screen.width .. \"x\" .. info.screen.height)\n"
    "dim.print(\"disk     \" .. info.disk.model)\n"
    "dim.print(string.format(\"volume   %d KiB, %d KiB free\",\n"
    "    info.disk.bytes // 1024, info.disk.free_bytes // 1024))\n"
    "dim.print(\"files    \" .. #info.files)\n"
    "dim.print(\"theme    \" .. info.theme .. \", brightness \" ..\n"
    "    info.brightness .. \", volume \" .. info.volume)\n"
    "dim.print(\"apps     \" .. table.concat(info.apps, \", \"))\n"
    "dim.print(\"lua      \" .. info.lua)\n";

static u8 volume_has(const char *name) {
    char candidate[VOLUME_NAME_LENGTH];
    u16 slot;

    for (slot = 0u; slot < volume_file_count(); ++slot) {
        if (volume_file(slot, candidate, (u32 *)0) != 0u &&
            text_equal_ignore_case(candidate, name) != 0u) {
            return 1u;
        }
    }
    return 0u;
}

static void seed_file(const char *name, const char *text, u32 length) {
    if (volume_has(name) != 0u) {
        return; /* already there: never overwrite the user's work */
    }
    (void)volume_write_file(name, text, length);
}

static void volume_seed(void) {
    u16 entry = file_system_find("FONT.TTF");

    if (volume_present() == 0u) {
        return;
    }
    if (entry == FILE_NOT_FOUND && options.font_path != (const char *)0) {
        FILE *handle = fopen(options.font_path, "rb");

        if (handle != (FILE *)0) {
            static u8 font_bytes[HOST_DATA_WINDOW_BYTES];
            const size_t length = fread(font_bytes, 1u, sizeof(font_bytes),
                                        handle);

            fclose(handle);
            if (length > 12u && length < sizeof(font_bytes)) {
                (void)volume_write_file("FONT.TTF", font_bytes, (u32)length);
            }
        }
    }
    seed_file("README.TXT", readme_text, (u32)(sizeof(readme_text) - 1u));
    seed_file("HELLO.LUA", hello_lua, (u32)(sizeof(hello_lua) - 1u));
    seed_file("SYSINFO.LUA", sysinfo_lua, (u32)(sizeof(sysinfo_lua) - 1u));
}

void host_after_mount(void) {
    if (volume_present() == 0u && ata_present() != 0u &&
        options.allow_format != 0u) {
        host_console("blank volume: formatting FAT12");
        if (volume_format() != 0u) {
            host_console("volume ready: %u KiB, %u KiB free",
                         (unsigned)(volume_total_bytes() / 1024u),
                         (unsigned)(volume_free_bytes() / 1024u));
        }
    }
    volume_seed();
    windows_load();
    file_system_init();
}

void host_present_frame(void) {
    /* The framebuffer already holds the frame; the web front end only has
     * to be told that it may look. Nothing to do here on purpose. */
}

void host_wait_idle(void) {
    host_http_poll();
    pending_text_feed();
    host_scene_tick();
    host_sleep(1u);
}

const char *host_platform_name(void) {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__unix__)
    return "unix";
#else
    return "unknown";
#endif
}

/* ------------------------------------------------------------------ */
/* The TrueType font                                                   */
/* ------------------------------------------------------------------ */

/* font_ttf.c is compiled the way tools/font_preview.c compiles it: the
 * rasterizer works on host buffers and this file supplies the file. */
extern u8 *font_ttf_work_area;

u8 font_ttf_load(void) {
    static u8 *file_area;
    static u8 *work_area;
    u32 size = 0u;
    u8 loaded = 0u;

    if (file_area == (u8 *)0) {
        file_area = (u8 *)calloc(HOST_FONT_AREA_BYTES, 1u);
    }
    if (work_area == (u8 *)0) {
        work_area = (u8 *)calloc(HOST_FONT_WORK_BYTES, 1u);
    }
    if (file_area == (u8 *)0 || work_area == (u8 *)0) {
        return 0u;
    }
    font_ttf_work_area = work_area;

    /* First choice: the font that ships on the DimOS volume. */
    {
        const u16 entry = file_system_find("FONT.TTF");

        if (entry != FILE_NOT_FOUND) {
            size = file_system_size(entry);
            if (size > 12u && size <= HOST_FONT_AREA_BYTES) {
                loaded = (u8)(file_system_read(entry, 0u, file_area, size) ==
                              size);
            }
        }
    }
    /* Otherwise: the font next to the executable, so a volume made by the
     * metal build still renders text here. */
    if (loaded == 0u && options.font_path != (const char *)0) {
        FILE *handle = fopen(options.font_path, "rb");

        if (handle != (FILE *)0) {
            size = (u32)fread(file_area, 1u, HOST_FONT_AREA_BYTES, handle);

            fclose(handle);
            loaded = (u8)(size > 12u);
        }
    }
    if (loaded == 0u) {
        return 0u;
    }
    return font_ttf_build(file_area, size);
}

/* ------------------------------------------------------------------ */
/* Start and stop                                                      */
/* ------------------------------------------------------------------ */

int host_platform_start(const HostOptions *given) {
    options = *given;
    if (options.display_width == 0u) {
        options.display_width = SCREEN_WIDTH;
    }
    if (options.display_height == 0u) {
        options.display_height = SCREEN_HEIGHT;
    }

    host_ram_disk = (u8 *)calloc(HOST_RAM_DISK_BYTES, 1u);
    if (host_ram_disk == (u8 *)0) {
        host_console("out of memory for the 4 MiB RAM disk");
        return 0;
    }
    if (display_start(options.display_width, options.display_height) == 0) {
        host_console("out of memory for the framebuffer");
        return 0;
    }
    pointer_mirror_x = (s16)(SCREEN_WIDTH / 2u);
    pointer_mirror_y = (s16)(SCREEN_HEIGHT / 2u);
    if (disk_open(options.disk_path, options.disk_bytes,
                  options.allow_format) == 0) {
        host_console("cannot open %s", options.disk_path);
        return 0;
    }
    windows_load();
    if (options.verbose != 0u) {
        host_console("disk %s: %u sectors, %s", options.disk_path,
                     (unsigned)disk_sector_count,
                     host_volume_is_blank() != 0u ? "blank" : "FAT12");
    }
    return 1;
}

void host_platform_stop(void) {
    if (disk_file != (FILE *)0) {
        fflush(disk_file);
        fclose(disk_file);
        disk_file = (FILE *)0;
    }
}

const char *host_disk_path(void) {
    return options.disk_path;
}

u32 host_disk_bytes(void) {
    return disk_sector_count * 512u;
}
