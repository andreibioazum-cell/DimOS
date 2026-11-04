/*
 * app_about.c -- Settings: what this phone is made of.
 *
 * The numbers here are read from the machine itself: the BIOS memory count,
 * the clock chip, the boot sector of the disk DimOS started from, and the
 * state of the mouse driver. The second tab reads the phone panel profile
 * instead (native resolution, ppi, scale, aspect, refresh, safe area).
 * Appearance, the panel picker, sound and power live on the right, like an
 * iOS settings page.
 */

#include "dimos.h"

#define ID_THEME 1u
#define ID_RESTART 2u
#define ID_TEST_SOUND 3u
#define ID_LOCK 4u
#define ID_TAB_MACHINE 5u
#define ID_TAB_DISPLAY 6u
#define ID_PANEL 7u

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

static u8 show_display;

static void two_digits(char *out, u16 capacity, u8 value) {
    if (value < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, value, capacity);
}

static void about_open(void) {
    show_display = 0u;
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
    char value[40];
    u16 row;

    gui_window_frame("Settings");

    /* Two tabs over the same grouped list: machine facts and panel facts. */
    gui_section_label(ROWS_X, 64,
                      (show_display != 0u) ? "DISPLAY & PANEL" : "THIS PHONE");
    gui_button_colored((s16)(ROWS_X + ROWS_WIDTH - 172), 58, 80, 22,
                       "Machine", ID_TAB_MACHINE,
                       (show_display != 0u) ? COLOR_PANEL : COLOR_SELECTION,
                       (show_display != 0u) ? COLOR_DEEP : COLOR_FACE);
    gui_button_colored((s16)(ROWS_X + ROWS_WIDTH - 86), 58, 80, 22,
                       "Display", ID_TAB_DISPLAY,
                       (show_display != 0u) ? COLOR_SELECTION : COLOR_PANEL,
                       (show_display != 0u) ? COLOR_FACE : COLOR_DEEP);
    gui_card(ROWS_X, ROWS_Y, ROWS_WIDTH,
             (s16)(ROW_HEIGHT * 8 + 4));

    for (row = 0u; row < 8u; ++row) {
        const char *label = "";
        const char *shown = "";

        value[0] = '\0';
        if (show_display != 0u) {
            label = display_row_label((u8)row);
            display_row_value((u8)row, value, (u16)sizeof(value));
            shown = value;
        } else {
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
                    label = "Disk";
                    shown = (ata_present() != 0u) ? ata_model() : "none found";
                    break;
                case 4u:
                    label = "Volume";
                    if (volume_present() != 0u) {
                        text_append_number(value, volume_total_bytes() / 1024u,
                                           (u16)sizeof(value));
                        text_append(value, " KiB, ", (u16)sizeof(value));
                        text_append_number(value, volume_free_bytes() / 1024u,
                                           (u16)sizeof(value));
                        text_append(value, " free", (u16)sizeof(value));
                    } else {
                        text_append(value, "not formatted", (u16)sizeof(value));
                    }
                    shown = value;
                    break;
                case 5u:
                    label = "Files";
                    if (volume_present() != 0u) {
                        text_append_number(value, volume_file_count(),
                                           (u16)sizeof(value));
                        text_append(value, " saved, ", (u16)sizeof(value));
                    }
                    text_append_number(value, file_system_visible_count(),
                                       (u16)sizeof(value));
                    text_append(value, " on the boot disk", (u16)sizeof(value));
                    shown = value;
                    break;
                case 6u:
                    label = "Saved";
                    if (settings_saved() != 0u) {
                        text_append(value, theme_names[settings_saved_theme()],
                                    (u16)sizeof(value));
                        text_append(value, ", level ", (u16)sizeof(value));
                        text_append_number(value, settings_saved_brightness(),
                                           (u16)sizeof(value));
                    } else {
                        text_append(value, "this session only",
                                    (u16)sizeof(value));
                    }
                    shown = value;
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
        }
        gui_list_row(ROWS_X, (s16)(ROWS_Y + 2 + (s16)row * ROW_HEIGHT),
                     ROWS_WIDTH, ROW_HEIGHT, label, shown,
                     (u8)(row == 7u));
    }

    /* Appearance. */
    gui_section_label(SIDE_X, 64, "APPEARANCE");
    gui_card(SIDE_X, 74, SIDE_WIDTH, 86);
    gfx_text_size((s16)(SIDE_X + 16), 86, "Theme", COLOR_DEEP, 12);
    gfx_text_size_right((s16)(SIDE_X + SIDE_WIDTH - 16), 87,
                        theme_names[gfx_current_theme()], COLOR_DISABLED, 11);
    gui_button((s16)(SIDE_X + 16), 112, (s16)(SIDE_WIDTH - 32), 36,
               "Change theme", ID_THEME);

    /* The phone panel itself, with its cutout and safe area. */
    gui_section_label(SIDE_X, 170, "PANEL PROFILE");
    gui_card(SIDE_X, 180, SIDE_WIDTH, 100);
    gfx_text_size((s16)(SIDE_X + 16), 188, display_current()->name,
                  COLOR_DEEP, 13);
    display_output_line(value, (u16)sizeof(value));
    gfx_text_size((s16)(SIDE_X + 16), 208, value, COLOR_DISABLED, 9);
    value[0] = '\0';
    text_append_number(value, display_refresh_hz(), (u16)sizeof(value));
    text_append(value, " Hz", (u16)sizeof(value));
    gfx_text_size((s16)(SIDE_X + 180), 232, value, COLOR_DEEP, 11);
    gui_button_colored((s16)(SIDE_X + 16), 226, 150, 34, "Change panel",
                       ID_PANEL, COLOR_SELECTION, COLOR_FACE);
    display_draw_mini((s16)(SIDE_X + SIDE_WIDTH - 62), 188, 40, 76);

    /* Sound and power share the last card. */
    gui_section_label(SIDE_X, 288, "SOUND & POWER");
    gfx_round_fill(SIDE_X, 298, SIDE_WIDTH, 52, 13, COLOR_FACE);
    gui_button_colored((s16)(SIDE_X + 10), 304, 112, 38, "Play chime",
                       ID_TEST_SOUND, COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(SIDE_X + 128), 304, 100, 38, "Lock",
                       ID_LOCK, COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(SIDE_X + 234), 304, 104, 38, "Restart",
                       ID_RESTART, COLOR_ALERT, COLOR_FACE);

    /* No footer: the black navigation strip owns the bottom of the page. */
}

static void about_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }
    if (event->key == ID_TAB_MACHINE) {
        show_display = 0u;
        return;
    }
    if (event->key == ID_TAB_DISPLAY) {
        show_display = 1u;
        return;
    }
    if (event->key == ID_THEME) {
        gfx_select_theme((u8)((gfx_current_theme() + 1u) % THEME_COUNT));
        settings_save();
        return;
    }
    if (event->key == ID_PANEL) {
        display_next_profile();
        settings_save();
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
