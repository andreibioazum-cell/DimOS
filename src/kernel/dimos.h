/*
 * DimOS kernel -- declarations shared by every C file.
 *
 * DimOS is a 32-bit freestanding kernel with a graphical desktop in VGA mode
 * 13h (320x200, 256 colours). The desktop is "DimXfce": a full Xfce style
 * shell in miniature -- a dark top panel with the Whisker menu behind the
 * little mouse logo, a gradient xfdesktop wallpaper, xfwm4 looking window
 * decorations and an icon dock at the bottom. The whole desktop is usable
 * with a pointing device alone: nothing needs a keyboard, and the terminal
 * application has its own on-screen keyboard for the commands that do take
 * text.
 *
 * Coding rules for this directory:
 *   - everything is plain C11, freestanding (no libc, no interrupts, no
 *     paging, no inline assembly);
 *   - the only assembly in the project is src/bootloader/boot.asm (the 512
 *     byte BIOS boot sector) and src/kernel/kernel.asm (the switch into
 *     protected mode), because those two things cannot be written in C;
 *   - hardware is reached through port_read_byte()/port_write_byte() and
 *     plain pointers, both defined below.
 */

#ifndef DIMOS_DIMOS_H
#define DIMOS_DIMOS_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

/* ------------------------------------------------------------------ */
/* Hardware ports                                                      */
/* ------------------------------------------------------------------ */

u8 port_read_byte(u16 port);
void port_write_byte(u16 port, u8 value);

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

#define SCREEN_WIDTH 320u
#define SCREEN_HEIGHT 200u
#define SCREEN_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT)

/* The visible VGA frame buffer, and our own copy that we compose into. */
#define VGA_FRAME_BUFFER ((volatile u8 *)0x000A0000u)
#define BACK_BUFFER_ADDRESS 0x00060000u

/* Free RAM below one megabyte that applications may use as a bitmap. */
#define SCRATCH_ADDRESS 0x00050000u
#define SCRATCH_BYTES 32768u

/* Where the boot code copies the 8x8 font that ships in the video BIOS. */
#define BIOS_FONT_ADDRESS 0x0000E000u
#define BIOS_FONT_BYTES 1024u /* 128 glyphs, 8 bytes each */

/* Desktop chrome, DimXfce edition: the Xfce panel up top, the xfdesktop
 * area in the middle and the plank style dock at the bottom. The names of
 * the old constants remain, so the applications never notice. */
#define TITLE_BAR_HEIGHT 14u                    /* the xfce4-panel          */
#define TASK_BAR_HEIGHT 20u                     /* the dock                 */
#define DESKTOP_TOP (TITLE_BAR_HEIGHT + 1u)
#define DESKTOP_HEIGHT (SCREEN_HEIGHT - TITLE_BAR_HEIGHT - TASK_BAR_HEIGHT - 2u)
#define TASK_BAR_TOP (SCREEN_HEIGHT - TASK_BAR_HEIGHT)

/* The Whisker menu covers most of the desktop when it pops up. */
#define MENU_X 2u
#define MENU_Y (TITLE_BAR_HEIGHT + 2u)
#define MENU_WIDTH (SCREEN_WIDTH - 4u)
#define MENU_HEIGHT (SCREEN_HEIGHT - TITLE_BAR_HEIGHT - TASK_BAR_HEIGHT - 7u)

/* Application windows fill the desktop area. */
#define WINDOW_X 6u
#define WINDOW_Y (TITLE_BAR_HEIGHT + 3u)
#define WINDOW_WIDTH (SCREEN_WIDTH - 12u)
#define WINDOW_HEIGHT (DESKTOP_HEIGHT - 6u)
#define WINDOW_LEFT (WINDOW_X + 3u)
#define WINDOW_TOP (WINDOW_Y + 13u)
#define WINDOW_INNER_WIDTH (WINDOW_WIDTH - 6u)
#define WINDOW_INNER_HEIGHT (WINDOW_HEIGHT - 16u)

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
    PALETTE_SIZE = 32u
};

/* Palette slots 32..247 are not roles at all: they hold a fixed 6x6x6
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
void gfx_clear(u8 color);
void gfx_pixel(s16 x, s16 y, u8 color);
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
void gfx_raised_box(s16 x, s16 y, s16 width, s16 height, u8 raised);
void gfx_panel(s16 x, s16 y, s16 width, s16 height);
void gfx_text(s16 x, s16 y, const char *text, u8 color);
void gfx_text_filled(s16 x, s16 y, const char *text, u8 foreground, u8 background);
void gfx_text_centered(s16 x, s16 y, s16 width, const char *text, u8 color);
void gfx_character(s16 x, s16 y, char character, u8 color);
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
    EVENT_MOVE    /* the pointer moved                                */
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
void timer_update(void);          /* fold elapsed time into the clock   */
u32 time_milliseconds(void);
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
/* font_ttf.c -- a real TrueType font (FONT.TTF on the boot volume)    */
/* ------------------------------------------------------------------ */

/* Parses FONT.TTF and rasterizes ASCII twice: into an 8x8 one-bit table
 * laid out exactly like the BIOS font, and into an anti-aliased table
 * that keeps one coverage level per pixel. gfx.c draws with the second
 * one, which is what makes the text smooth instead of blocky. */
#define FONT_ALPHA_MAX 16u /* a pixel the outline covers completely */

u8 font_ttf_load(void);                     /* 1 = the table is ready  */
u8 font_ttf_build(const u8 *file, u32 size); /* parse an in-memory TTF */
const u8 *font_ttf_table(void);             /* 128 glyphs * 8 bytes    */
const u8 *font_ttf_alpha_table(void);       /* 128 glyphs * 64 levels  */


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
void sound_play_startup(void);   /* the Xfce style login fanfare  */
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
    const char *dock_label;   /* short caption for icon and dock       */
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
u16 gui_active_application(void);   /* 0xFFFF when the desktop is shown */
u16 gui_application_count(void);
const Application *gui_application(u16 index);

/* Immediate mode widgets: call them from the draw handler of an
 * application. Each one paints the widget and remembers where it is, so a
 * later click can be matched against it. */
void gui_hotspots_reset(void);
void gui_hotspot(s16 x, s16 y, s16 width, s16 height, u16 id);
u16 gui_hotspot_at(s16 x, s16 y);
u8 gui_has_focus(u16 id);
void gui_focus(u16 id);
void gui_move_focus(s16 step_x, s16 step_y);
void gui_button(s16 x, s16 y, s16 width, s16 height, const char *label, u16 id);
void gui_button_colored(s16 x, s16 y, s16 width, s16 height, const char *label,
                        u16 id, u8 face, u8 text);
void gui_switch(s16 x, s16 y, s16 width, s16 height, const char *label, u16 id, u8 on);
void gui_window_frame(const char *title);
void gui_window_title_color(u8 top_color);
void gui_message_bar(const char *text);
void gui_reserve_arrow_keys(u8 reserve);

/* The Whisker menu behind the little mouse on the top panel. */
u8 gui_menu_is_open(void);
void gui_menu_toggle(void);
void gui_show_desktop(void);

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
