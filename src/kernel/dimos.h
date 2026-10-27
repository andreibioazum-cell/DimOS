/*
 * DimOS kernel -- declarations shared by every C file.
 *
 * DimOS is a freestanding i686 protected-mode kernel with a mobile shell:
 * a lock screen, a home screen with rounded app icons, a translucent dock,
 * a status bar and a Control Centre, in the spirit of iOS 14. The logical
 * screen is an iPhone-sized 844x390 landscape canvas that is scaled to
 * whatever VBE display the machine offers, so the phone layout is the
 * primary one while PC monitors and emulators show it letterboxed and
 * perfectly usable with a mouse. The shell needs no keyboard: everything
 * is a touch target, and the terminal brings its own keyboard.
 *
 * Coding rules for this directory:
 *   - everything is plain C11, freestanding (no libc or inline assembly);
 *     the PIT interrupt and CPU halt instruction live in kernel.asm;
 *   - assembly is confined to src/bootloader/boot.asm (the 512-byte BIOS
 *     boot sector) and src/kernel/kernel.asm (VBE discovery, protected-mode
 *     entry, the PIT IRQ/IDT and interrupt-safe HLT waits);
 *   - hardware is reached through the port I/O helpers and plain pointers.
 */

#ifndef DIMOS_DIMOS_H
#define DIMOS_DIMOS_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;

/* ------------------------------------------------------------------ */
/* Two builds of the same kernel                                       */
/* ------------------------------------------------------------------ */

/* The canonical DimOS build boots the flat kernel at 0x20000 in a 32-bit
 * protected-mode machine, so every address the code keeps in a u32 is a
 * real physical address. src/host builds the very same sources as a normal
 * program on Linux, Windows or Android, where pointers are wider: the low
 * addresses are then ordinary host mappings and this macro adds the extra
 * step through an unsigned long instead of truncating the value. */
/* The other direction: an address as a number, for pointer alignment. */
#ifdef DIMOS_HOST
#define dimos_address(pointer) ((unsigned long)(pointer))
#else
#define dimos_address(pointer) ((u32)(pointer))
#endif

#ifdef DIMOS_HOST
#define dimos_physical(type, address) ((type)(unsigned long)(address))
/* The hosted build lets the host decide where the framebuffer lives, so it
 * works on 64-bit x86 and on ARM64 phones alike. */
u8 *host_framebuffer_pointer(void);
#define dimos_framebuffer() ((volatile u8 *)host_framebuffer_pointer())
#else
#define dimos_physical(type, address) ((type)(u32)(address))
#define dimos_framebuffer() dimos_physical(volatile u8 *, video_framebuffer_address)
#endif

/* Hooks the hosted build (src/host) plugs into the shell. On the metal they
 * do not exist at all; host_present_frame() publishes a finished frame to
 * the browser clients and host_wait_idle() serves them while the phone is
 * waiting for the next 60 Hz slot. */
#ifdef DIMOS_HOST
void host_present_frame(void);
void host_wait_idle(void);
void host_after_mount(void);
void host_shell_ready(void);
void host_sleep_milliseconds(u32 milliseconds);
void host_restart_process(void);
void host_exit_process(int code);
/* "linux", "windows", "android" -- shown by Settings in a hosted build. */
const char *host_platform_name(void);
#endif

/* ------------------------------------------------------------------ */
/* Hardware ports                                                      */
/* ------------------------------------------------------------------ */

u8 port_read_byte(u16 port);
void port_write_byte(u16 port, u8 value);
u16 port_read_word(u16 port);
void port_write_word(u16 port, u16 value);
void cpu_halt_forever(void);
u8 acpi_power_off(void);

#define PORT_PIT_CHANNEL_0 0x40u
#define PORT_PIT_CHANNEL_2 0x42u
#define PORT_PIT_COMMAND 0x43u
#define PORT_SPEAKER 0x61u
#define PORT_KEYBOARD_DATA 0x60u
#define PORT_KEYBOARD_STATUS 0x64u
#define PORT_KEYBOARD_RESET 0xFEu
#define PORT_CMOS_ADDRESS 0x70u
#define PORT_CMOS_DATA 0x71u
#define PORT_VGA_DAC_WRITE_INDEX 0x3C8u
#define PORT_VGA_DAC_DATA 0x3C9u

/* ------------------------------------------------------------------ */
/* Memory helpers (kernel.c)                                           */
/* ------------------------------------------------------------------ */

void memory_copy(void *destination, const void *source, u32 length);
void memory_zero(void *destination, u32 length);
u8 memory_equal(const void *a, const void *b, u32 length);
u16 bios_read_word(u32 address);   /* a word the BIOS left in low memory */

/* ------------------------------------------------------------------ */
/* Screen layout                                                       */
/* ------------------------------------------------------------------ */

#define SCREEN_WIDTH 844u
#define SCREEN_HEIGHT 390u
#define SCREEN_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT)

/* The visible VGA frame buffer. The indexed canvas and its shadow live in
 * the kernel's own BSS: at phone resolution two of them no longer fit in
 * the 64 KiB window between the kernel and the VGA memory. */
#define VGA_FRAME_BUFFER ((volatile u8 *)0x000A0000u)

/* Video backend selected by kernel.asm while BIOS calls are still possible.
 * VBE is an actual linear framebuffer exposed by the display adapter; mode
 * 13h remains available for old VGA-only machines and minimal emulators. */
enum {
    VIDEO_BACKEND_VGA = 0u,
    VIDEO_BACKEND_VBE = 1u
};
extern u8 video_backend;
extern u32 video_framebuffer_address;
extern u16 video_pitch;
extern u16 video_width;
extern u16 video_height;
extern u8 video_bits_per_pixel;

/* Free RAM below one megabyte that applications may use as a bitmap. */
#define SCRATCH_ADDRESS 0x00050000u
#define SCRATCH_BYTES 32768u

/* Where the boot code copies the 8x8 font that ships in the video BIOS. */
#define BIOS_FONT_ADDRESS 0x0000E000u
#define BIOS_FONT_BYTES 1024u /* 128 glyphs, 8 bytes each */

/* ------------------------------------------------------------------ */
/* Phone shell layout                                                  */
/* ------------------------------------------------------------------ */

/* The status bar runs across the top of every screen. In an application
 * the navigation bar sits directly below it and the content starts under
 * that; the home indicator always floats just above the bottom edge. */
#define STATUS_BAR_HEIGHT 22u
#define NAV_BAR_HEIGHT 36u
#define APP_TOP (STATUS_BAR_HEIGHT + NAV_BAR_HEIGHT + 2u)
#define APP_LEFT 26u
#define APP_RIGHT (SCREEN_WIDTH - 26u)
#define APP_WIDTH (SCREEN_WIDTH - 2u * APP_LEFT)
#define APP_BOTTOM (SCREEN_HEIGHT - 26u)
#define APP_HEIGHT (APP_BOTTOM - APP_TOP)
#define HOME_INDICATOR_Y (SCREEN_HEIGHT - 9u)
#define HOME_INDICATOR_WIDTH 128u

/* Home screen grid: five columns of rounded icons on two rows, with the
 * dock underneath. Coordinates are derived so the whole grid stays
 * centred on any width the canvas may take. */
#define ICON_COLUMNS 5u
#define ICON_ROWS 2u
#define ICON_SIZE 64u
#define ICON_COLUMN_STEP 112u
#define ICON_ROW_STEP 108u
#define ICON_GRID_TOP 46u
#define ICON_GRID_LEFT ((s16)((SCREEN_WIDTH - (s16)(ICON_COLUMNS - 1u) * ICON_COLUMN_STEP - (s16)ICON_SIZE) / 2))
#define ICON_LABEL_GAP 8u
#define DOCK_HEIGHT 70u
#define DOCK_Y (SCREEN_HEIGHT - DOCK_HEIGHT - 18u)
#define DOCK_ICON_SIZE 52u
#define DOCK_SLOTS 4u

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

/* Palette slots. The first sixteen are the classic DOS colours, the rest are
 * the fixed roles used by the window manager. gfx.c holds the RGB values. */
enum {
    COLOR_BLACK = 0u,
    COLOR_BLUE = 1u,
    COLOR_GREEN = 2u,
    COLOR_CYAN = 3u,
    COLOR_RED = 4u,
    COLOR_MAGENTA = 5u,
    COLOR_BROWN = 6u,
    COLOR_LIGHT_GRAY = 7u,
    COLOR_DARK_GRAY = 8u,
    COLOR_LIGHT_BLUE = 9u,
    COLOR_LIGHT_GREEN = 10u,
    COLOR_LIGHT_CYAN = 11u,
    COLOR_LIGHT_RED = 12u,
    COLOR_LIGHT_MAGENTA = 13u,
    COLOR_YELLOW = 14u,
    COLOR_WHITE = 15u,
    COLOR_FACE = 16u,     /* window and button background */
    COLOR_SHADOW = 17u,   /* bottom and right edge of a raised box */
    COLOR_HILITE = 18u,   /* top and left edge of a raised box */
    COLOR_TITLE_BAR = 19u,
    COLOR_DESKTOP = 20u,
    COLOR_TEXT_FIELD = 21u,
    COLOR_SELECTION = 22u,
    COLOR_DISABLED = 23u,
    COLOR_ACCENT = 24u,
    COLOR_GOOD = 25u,
    COLOR_ALERT = 26u,
    COLOR_PANEL = 27u,
    COLOR_CANVAS = 28u,
    COLOR_GRID = 29u,
    COLOR_DEEP = 30u,
    COLOR_CURSOR = 31u,
    COLOR_TASKBAR = 32u,     /* the dock's translucent white           */
    COLOR_WIN_BG = 33u,
    COLOR_WIN_GLOW = 34u,    /* the brighter iOS tint for pressed keys */
    COLOR_WIN_LIGHT = 35u,   /* sky blue used by a few app icons       */
    COLOR_IOS_PURPLE = 36u,
    COLOR_IOS_PINK = 37u,
    COLOR_IOS_TEAL = 38u,
    COLOR_IOS_INDIGO = 39u,
    PALETTE_SIZE = 40u
};

/* Palette slots 40..255 are not roles at all: they hold a fixed 6x6x6
 * cube of red/green/blue mixtures. Anti-aliasing needs colours *between*
 * the ones above -- half ink and half window face for the edge of a
 * letter, a quarter of the icon colour where a curve grazes a pixel --
 * and the cube supplies the nearest one for any mixture. Nothing draws
 * with those slots directly; gfx_shade() picks them. */
#define COLOR_CUBE_BASE PALETTE_SIZE
#define COLOR_CUBE_STEPS 6u
#define COLOR_CUBE_COUNT (COLOR_CUBE_STEPS * COLOR_CUBE_STEPS * COLOR_CUBE_STEPS)

enum {
    THEME_COLOR = 0u, /* the DOS palette            */
    THEME_GREEN = 1u, /* green phosphor monitor     */
    THEME_AMBER = 2u, /* amber phosphor monitor     */
    THEME_MONOCHROME = 3u,
    THEME_COUNT = 4u
};

/* ------------------------------------------------------------------ */
/* gfx.c -- pixels, text, icons                                        */
/* ------------------------------------------------------------------ */

extern u8 *const screen; /* the back buffer, SCREEN_BYTES bytes */

void gfx_init(void);
void gfx_show(void); /* copy the back buffer to the video card */
void gfx_begin_frame(void); /* start a fresh full composition */
void gfx_set_output_resolution(u16 width, u16 height);
void gfx_clear(u8 color);
void gfx_pixel(s16 x, s16 y, u8 color);
u8 gfx_color_from_rgb(u8 red, u8 green, u8 blue);
void gfx_pixel_blend(s16 x, s16 y, u8 color, u8 alpha); /* alpha 0..16 */
u8 gfx_shade(u8 foreground, u8 background, u8 alpha);   /* mix two colours */
void gfx_horizontal_line(s16 x, s16 y, s16 length, u8 color);
void gfx_vertical_line(s16 x, s16 y, s16 length, u8 color);
void gfx_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 color);
void gfx_fill(s16 x, s16 y, s16 width, s16 height, u8 color);
void gfx_outline(s16 x, s16 y, s16 width, s16 height, u8 color);
void gfx_circle(s16 center_x, s16 center_y, s16 radius, u8 color, u8 filled);
void gfx_ellipse_fill(s16 center_x, s16 center_y, s16 radius_x, s16 radius_y,
                      u8 color);
void gfx_checker(s16 x, s16 y, s16 width, s16 height, u8 first, u8 second);
void gfx_gradient_vertical(s16 x, s16 y, s16 width, s16 height,
                           u8 top_color, u8 bottom_color);
void gfx_gradient_vertical_alpha(s16 x, s16 y, s16 width, s16 height,
                                 u8 top_color, u8 bottom_color, u8 alpha);
void gfx_raised_box(s16 x, s16 y, s16 width, s16 height, u8 raised);
void gfx_round_fill(s16 x, s16 y, s16 width, s16 height, s16 radius, u8 color);
void gfx_round_fill_alpha(s16 x, s16 y, s16 width, s16 height, s16 radius,
                          u8 color, u8 alpha);
void gfx_round_outline(s16 x, s16 y, s16 width, s16 height, s16 radius,
                       u8 color);
void gfx_round_gloss(s16 x, s16 y, s16 width, s16 height, s16 radius);
void gfx_round_band_alpha(s16 x, s16 y, s16 width, s16 height, s16 radius,
                          s16 band_top, s16 band_height, u8 color,
                          u8 alpha);
void gfx_glyph(s16 x, s16 y, const char *const *art, u16 rows, s16 scale,
               u8 color);
void gfx_panel(s16 x, s16 y, s16 width, s16 height);
void gfx_text(s16 x, s16 y, const char *text, u8 color);
void gfx_text_filled(s16 x, s16 y, const char *text, u8 foreground, u8 background);
void gfx_text_centered(s16 x, s16 y, s16 width, const char *text, u8 color);
void gfx_character(s16 x, s16 y, char character, u8 color);

/* Scaled, anti-aliased text for the phone UI. A glyph cell is `height`
 * pixels tall and about six tenths of that wide, which is the natural
 * proportion of the TrueType raster the kernel carries. Heights of eight
 * or less fall back to the bitmap path above. */
void gfx_text_size(s16 x, s16 y, const char *text, u8 color, u16 height);
void gfx_text_size_centered(s16 x, s16 y, s16 width, const char *text,
                            u8 color, u16 height);
void gfx_text_size_right(s16 x, s16 y, const char *text, u8 color, u16 height);
void gfx_text_size_shadowed(s16 x, s16 y, const char *text, u8 color,
                            u16 height);
u16 gfx_text_size_width(const char *text, u16 height);
void gfx_picture(s16 x, s16 y, const char *const *art, u16 rows, u8 color, u8 background);
void gfx_picture_opaque(s16 x, s16 y, const char *const *art, u16 rows, u8 on, u8 off);
void gfx_draw_pointer(s16 x, s16 y);
u16 gfx_text_width(const char *text);
void gfx_select_theme(u8 theme);
u8 gfx_current_theme(void);

/* ------------------------------------------------------------------ */
/* input.c -- PS/2 keyboard and mouse                                  */
/* ------------------------------------------------------------------ */

#define KEY_NOTHING 0u
#define KEY_ARROW_UP 0x100u
#define KEY_ARROW_RIGHT 0x101u
#define KEY_ARROW_DOWN 0x102u
#define KEY_ARROW_LEFT 0x103u
#define KEY_ENTER 0x104u
#define KEY_ESCAPE 0x105u
#define KEY_BACKSPACE 0x106u
#define KEY_TAB 0x107u
#define KEY_HOME 0x108u
#define KEY_DELETE 0x109u
#define KEY_PAGE_UP 0x10Au
#define KEY_PAGE_DOWN 0x10Bu

enum {
    EVENT_NOTHING = 0u,
    EVENT_KEY,    /* a key was pressed: event->key holds the code     */
    EVENT_PRESS,  /* left button went down at event->x / event->y     */
    EVENT_CLICK,  /* left button came up over the same place          */
    EVENT_RIGHT_CLICK, /* right button, or a long press on a touch    */
    EVENT_DRAG,   /* the pointer moved while the left button was down */
    EVENT_MOVE,   /* the pointer moved                                */
    EVENT_WHEEL   /* event->key is a signed wheel-notch delta          */
};

typedef struct {
    u8 type;
    u16 key;
    u8 buttons;
    s16 x;
    s16 y;
} Event;

void input_init(void);
void input_poll(void);            /* drain the PS/2 controller, queue events */
void input_advance(u16 ticks);    /* keep the long press timer running */
u8 input_next_event(Event *event);
s16 input_pointer_x(void);
s16 input_pointer_y(void);
u8 input_pointer_buttons(void);
u8 input_mouse_available(void);

/* ------------------------------------------------------------------ */
/* Time and clock (kernel.c)                                           */
/* ------------------------------------------------------------------ */

#define TICK_MILLISECONDS 10u

void timer_init(void);
void cpu_halt(void);              /* HLT until the next hardware interrupt */
void cpu_wait_until(u64 deadline); /* atomic, interrupt-driven PIT wait   */
void timer_update(void);          /* fold elapsed time into the clock   */
u32 time_milliseconds(void);
u64 timer_counter(void);          /* raw PIT input clocks since boot    */
u16 timer_take_ticks(void);       /* 10 ms ticks since the last call    */
void time_wait(u32 milliseconds);

void clock_init(void);            /* read the CMOS real time clock      */
u8 clock_seconds(void);
u8 clock_minutes(void);
u8 clock_hours(void);
u8 clock_day(void);
u8 clock_month(void);
u16 clock_year(void);

void system_restart(void);
u8 system_shutdown(void);

/* ------------------------------------------------------------------ */
/* Text helpers (kernel.c)                                             */
/* ------------------------------------------------------------------ */

u8 character_to_upper(u8 character);
u16 text_length(const char *text);
u8 text_equal(const char *a, const char *b);
u8 text_equal_ignore_case(const char *a, const char *b);
void text_copy(char *destination, const char *source, u16 capacity);
void text_append(char *destination, const char *source, u16 capacity);
void text_append_character(char *destination, char character, u16 capacity);
u16 text_append_number(char *destination, u32 value, u16 capacity);
void text_trim(char *text);
void text_pad_right(char *destination, u16 width, u16 capacity);

/* ------------------------------------------------------------------ */
/* fs.c -- the 4 MiB RAM disk and the FAT12 boot volume                */
/* ------------------------------------------------------------------ */

void ram_disk_init(void);
u8 ram_disk_read(u32 sector, void *buffer, u32 sector_count);
u8 ram_disk_write(u32 sector, const void *buffer, u32 sector_count);
u32 ram_disk_sectors(void);

#define FILE_ENTRY_COUNT 224u
#define FILE_NOT_FOUND 0xFFFFu
#define FILE_READ_LIMIT 32768u

u8 *ram_disk_buffer(void);              /* the 4 MiB scratch disk */
void file_system_init(void);
u16 file_system_visible_count(void);
u16 file_system_visible(u16 slot);      /* directory index of a visible slot */
void file_system_name(u16 index, char *out);
u32 file_system_size(u16 index);
u16 file_system_find(const char *name);
u32 file_system_read(u16 index, u32 offset, void *buffer, u32 length);
u8 file_system_delete(u16 index);       /* hides the file until reboot */
u8 file_system_is_protected(u16 index);
u32 file_system_volume_sectors(void);   /* straight from the boot sector */
u16 file_system_sector_bytes(void);

/* ------------------------------------------------------------------ */
/* ata.c -- the IDE disk, programmed I/O, LBA28                        */
/* ------------------------------------------------------------------ */

u8 ata_init(void);            /* 1 when a drive answered IDENTIFY      */
u8 ata_present(void);
u32 ata_sectors(void);
const char *ata_model(void);
u8 ata_read(u32 lba, void *buffer, u16 count);
u8 ata_write(u32 lba, const void *buffer, u16 count);
u8 ata_flush(void);

/* ------------------------------------------------------------------ */
/* volume.c -- the FAT12 volume DimOS can write                        */
/* ------------------------------------------------------------------ */

#define VOLUME_NAME_LENGTH 13u

u8 volume_init(void);         /* mount the disk; 0 when unformatted   */
u8 volume_format(void);       /* write a fresh FAT12 volume           */
u8 volume_present(void);
u32 volume_sectors(void);
u32 volume_total_bytes(void);
u32 volume_free_bytes(void);
u16 volume_file_count(void);
u8 volume_file(u16 slot, char *name, u32 *size);
u32 volume_read_file(const char *name, void *buffer, u32 capacity);
u8 volume_write_file(const char *name, const void *data, u32 length);
u8 volume_delete_file(const char *name);

/* ------------------------------------------------------------------ */
/* config.c -- the settings record CONFIG.CFG keeps on that volume     */
/* ------------------------------------------------------------------ */

void settings_load(void);
void settings_apply(void);    /* theme, brightness and volume         */
u8 settings_save(void);
u8 settings_saved(void);
u8 settings_saved_theme(void);
u8 settings_saved_brightness(void);
u8 settings_saved_volume(void);
u8 settings_forget(void);

/* ------------------------------------------------------------------ */
/* font_ttf.c -- a real TrueType font (FONT.TTF on the boot volume)    */
/* ------------------------------------------------------------------ */

/* Parses FONT.TTF into a compatibility 8x8 table and two coverage rasters:
 * 8x8 for the logical canvas and a native 16x16 raster for sharp VBE text.
 * The native table is rasterized straight from the outlines, never enlarged
 * from the tiny glyph bitmap. */
#define FONT_ALPHA_MAX 16u /* a pixel the outline covers completely */
#define FONT_NATIVE_GLYPH_PIXELS 16u

u8 font_ttf_load(void);                     /* 1 = the table is ready  */
u8 font_ttf_build(const u8 *file, u32 size); /* parse an in-memory TTF */
const u8 *font_ttf_table(void);             /* 128 glyphs * 8 bytes    */
const u8 *font_ttf_alpha_table(void);       /* 128 glyphs * 64 levels  */
const u8 *font_ttf_native_alpha_table(void); /* 128 glyphs * 16 * 16 */


/* ------------------------------------------------------------------ */
/* sound.c -- the PC speaker                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 hertz;  /* 0 means a pause */
    u16 ticks;  /* length in 10 ms units */
} Note;

void sound_init(void);
void sound_update(u16 ticks);
void sound_play(const Note *notes, u16 count, u8 repeat);
void sound_stop(void);
u8 sound_is_playing(void);
void sound_play_startup(void);   /* short PC-speaker startup fanfare */
void sound_play_march(void);
void sound_play_waltz(void);
void sound_play_nom(void);       /* the cheese ball chomp         */
void sound_play_burp(void);      /* twelve balls later            */
void sound_beep(void);
void sound_alert(void);

/* ------------------------------------------------------------------ */
/* gui.c -- the desktop and the widget helpers                         */
/* ------------------------------------------------------------------ */

#define HOTSPOT_NONE 0u
#define HOTSPOT_LIMIT 96u

typedef struct {
    const char *short_label;  /* compact caption for desktop shortcuts */
    const char *title;        /* full name shown in the title bar      */
    const char *const *icon;  /* 16 rows of 16 characters ASCII art    */
    u8 color;
    void (*open)(void);
    void (*draw)(void);
    void (*event)(const Event *event);
    void (*update)(u16 elapsed_ms);
} Application;

void gui_run(void);
void gui_open(u16 index);
void gui_go_home(void);
void gui_lock(void);      /* back to the lock screen */
u16 gui_active_application(void);   /* 0xFFFF when the home screen is shown */
u16 gui_application_count(void);
const Application *gui_application(u16 index);

/* Immediate mode widgets: call them from the draw handler of an
 * application. Each one paints the widget and remembers where it is, so a
 * later click can be matched against it. */
void gui_hotspots_reset(void);
void gui_hotspot(s16 x, s16 y, s16 width, s16 height, u16 id);
u16 gui_hotspot_at(s16 x, s16 y);
u8 gui_has_focus(u16 id);
u8 gui_is_pressed(u16 id);   /* the button under the finger right now */
void gui_focus(u16 id);
void gui_move_focus(s16 step_x, s16 step_y);
void gui_button(s16 x, s16 y, s16 width, s16 height, const char *label, u16 id);
void gui_button_colored(s16 x, s16 y, s16 width, s16 height, const char *label,
                        u16 id, u8 face, u8 text);
void gui_switch(s16 x, s16 y, s16 width, s16 height, const char *label, u16 id, u8 on);

/* The application screen: a white (or dark) phone page, a status bar, a
 * navigation bar with the app title and a back control, and the home
 * indicator. An application calls one of these first and draws its own
 * content between APP_TOP and APP_BOTTOM afterwards. */
void gui_window_frame(const char *title);
void gui_window_frame_dark(const char *title, u8 background);
void gui_window_title_color(u8 top_color);
void gui_message_bar(const char *text);
void gui_reserve_arrow_keys(u8 reserve);
void gui_status_light(u8 light);   /* white status bar text for dark apps */

/* The two levels Control Centre shows. Settings keeps them on the disk. */
u8 gui_brightness(void);
void gui_set_brightness(u8 level);
u8 gui_volume(void);
void gui_set_volume(u8 level);

/* Grouped iOS-style building blocks. */
void gui_card(s16 x, s16 y, s16 width, s16 height);
void gui_section_label(s16 x, s16 y, const char *text);
void gui_list_row(s16 x, s16 y, s16 width, s16 height, const char *label,
                  const char *value, u8 last);

/* ------------------------------------------------------------------ */
/* The applications                                                    */
/* ------------------------------------------------------------------ */

extern const Application application_files;
extern const Application application_snake;
extern const Application application_mines;
extern const Application application_paint;
extern const Application application_calculator;
extern const Application application_music;
extern const Application application_cheesy;
extern const Application application_about;
extern const Application application_terminal;

extern const Application *const application_list[];
extern const u16 application_count;

#endif /* DIMOS_DIMOS_H */
