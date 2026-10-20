/*
 * app_about.c -- Settings: what this phone is made of.
 *
 * The numbers here are read from the machine itself: the BIOS memory count,
 * the clock chip, the boot sector of the disk DimOS started from, and the
 * state of the mouse driver. Appearance, sound, restart and lock live on
 * the right, like an iOS settings page.
 */

#include "dimos.h"

#define ID_THEME 1u
#define ID_RESTART 2u
#define ID_TEST_SOUND 3u
#define ID_LOCK 4u

#define ROWS_X (s16)APP_LEFT
#define ROWS_Y 74
#define ROWS_WIDTH 420
#define ROW_HEIGHT 33
#define SIDE_X 470
#define SIDE_WIDTH 348

static const char *const icon[16] = {
    "................",
    "......XX........",
    "......XX........",
    "..XXX.XX.XXX....",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    ".XXXX.XX.XXXX...",
    "..XXX.XX.XXX....",
    "......XX........",
    "......XX........",
    "................",
    "................",
};

static const char *const theme_names[THEME_COUNT] = {
    "Colour", "Green", "Amber", "Mono"
};

static void two_digits(char *out, u16 capacity, u8 value) {
    if (value < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, value, capacity);
}

static void about_open(void) {
    /* Nothing to prepare; every number is read while drawing. */
}

static void about_video_value(char *value, u16 capacity) {
    value[0] = '\0';
    if (video_backend != VIDEO_BACKEND_VBE) {
        text_append(value, "VGA fallback", capacity);
        return;
    }
    text_append_number(value, video_width, capacity);
    text_append_character(value, 'x', capacity);
    text_append_number(value, video_height, capacity);
    text_append(value, (video_bits_per_pixel == 32u) ? " x32" : " x16",
                capacity);
}

static void about_draw(void) {
    char value[32];
    u16 row;

    gui_window_frame("Settings");

    /* The group of machine facts. */
    gui_section_label(ROWS_X, 64, "THIS PHONE");
    gui_card(ROWS_X, ROWS_Y, ROWS_WIDTH,
             (s16)(ROW_HEIGHT * 8 + 4));

    for (row = 0u; row < 8u; ++row) {
        const char *label = "";
        const char *shown = "";

        value[0] = '\0';
        switch (row) {
            case 0u:
                label = "Model";
                shown = "DimOS 14";
                break;
            case 1u:
                label = "Chip";
                shown = "i686 protected mode";
                break;
            case 2u:
                label = "Display";
                about_video_value(value, (u16)sizeof(value));
                shown = value;
                break;
            case 3u:
                label = "Storage";
                text_append_number(value, ram_disk_sectors() / 2u,
                                   (u16)sizeof(value));
                text_append(value, " KiB RAM disk", (u16)sizeof(value));
                shown = value;
                break;
            case 4u:
                label = "Memory";
                text_append_number(value, (u32)bios_read_word(0x00000413u),
                                   (u16)sizeof(value));
                text_append(value, " KiB", (u16)sizeof(value));
                shown = value;
                break;
            case 5u:
                label = "Files";
                text_append_number(value, file_system_visible_count(),
                                   (u16)sizeof(value));
                text_append(value, " on the boot disk", (u16)sizeof(value));
                shown = value;
                break;
            case 6u:
                label = "Mouse";
                shown = (input_mouse_available() != 0u)
                            ? "PS/2 pointer found" : "no pointer";
                break;
            default:
                label = "Clock";
                two_digits(value, (u16)sizeof(value), clock_hours());
                text_append_character(value, ':', (u16)sizeof(value));
                two_digits(value, (u16)sizeof(value), clock_minutes());
                text_append_character(value, ':', (u16)sizeof(value));
                two_digits(value, (u16)sizeof(value), clock_seconds());
                shown = value;
                break;
        }
        gui_list_row(ROWS_X, (s16)(ROWS_Y + 2 + (s16)row * ROW_HEIGHT),
                     ROWS_WIDTH, ROW_HEIGHT, label, shown,
                     (u8)(row == 7u));
    }

    /* Appearance. */
    gui_section_label(SIDE_X, 64, "APPEARANCE");
    gui_card(SIDE_X, ROWS_Y, SIDE_WIDTH, 116);
    gfx_text_size((s16)(SIDE_X + 16), 88, "Theme", COLOR_DEEP, 12);
    gfx_text_size_right((s16)(SIDE_X + SIDE_WIDTH - 16), 89,
                        theme_names[gfx_current_theme()], COLOR_DISABLED, 11);
    gui_button((s16)(SIDE_X + 16), 116, (s16)(SIDE_WIDTH - 32), 40,
               "Change theme", ID_THEME);

    /* Sound. */
    gui_section_label(SIDE_X, 196, "SOUND");
    gui_card(SIDE_X, 206, SIDE_WIDTH, 66);
    gfx_text_size((s16)(SIDE_X + 16), 220, "PC speaker", COLOR_DEEP, 12);
    gui_button_colored((s16)(SIDE_X + 170), 214, 162, 34, "Play chime",
                       ID_TEST_SOUND, COLOR_PANEL, COLOR_DEEP);

    /* Power. */
    gui_section_label(SIDE_X, 280, "POWER");
    gfx_round_fill(SIDE_X, 290, SIDE_WIDTH, 58, 13, COLOR_FACE);
    gui_button_colored((s16)(SIDE_X + 12), 298, 150, 42, "Lock", ID_LOCK,
                       COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(SIDE_X + 174), 298, 162, 42, "Restart",
                       ID_RESTART, COLOR_ALERT, COLOR_FACE);

    gfx_text_size(ROWS_X, 348, "DimOS 14 - an iOS 14 inspired shell for i686",
                  COLOR_DISABLED, 9);
}

static void about_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }
    if (event->key == ID_THEME) {
        gfx_select_theme((u8)((gfx_current_theme() + 1u) % THEME_COUNT));
        return;
    }
    if (event->key == ID_TEST_SOUND) {
        sound_play_startup();
        return;
    }
    if (event->key == ID_LOCK) {
        gui_lock();
        return;
    }
    if (event->key == ID_RESTART) {
        system_restart();
    }
}

const Application application_about = {
    "Settings",
    "Settings",
    icon,
    COLOR_DISABLED,
    about_open,
    about_draw,
    about_event,
    0
};
