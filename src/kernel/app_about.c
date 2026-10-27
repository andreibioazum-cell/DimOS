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

/* Everything is one column in portrait: the facts, then appearance, sound
 * and power, each group in its own card. */
#define ROWS_X (s16)APP_LEFT
#define ROWS_Y 74
#define ROWS_WIDTH (s16)APP_WIDTH
#define ROW_HEIGHT 40
#define SIDE_X (s16)APP_LEFT
#define SIDE_WIDTH (s16)APP_WIDTH
#define SIDE_TOP (s16)(ROWS_Y + ROW_HEIGHT * 8 + 28)

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
#ifdef DIMOS_HOST
                /* The hosted build of the very same kernel: say what it is
                 * actually running on instead of claiming to be a PC. */
                text_copy(value, "hosted, ", (u16)sizeof(value));
                text_append(value, host_platform_name(), (u16)sizeof(value));
                shown = value;
#else
                shown = "i686 protected mode";
#endif
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
                    text_append(value, "this session only", (u16)sizeof(value));
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
        gui_list_row(ROWS_X, (s16)(ROWS_Y + 2 + (s16)row * ROW_HEIGHT),
                     ROWS_WIDTH, ROW_HEIGHT, label, shown,
                     (u8)(row == 7u));
    }

    /* Appearance. */
    gui_section_label(SIDE_X, (s16)(SIDE_TOP - 10), "APPEARANCE");
    gui_card(SIDE_X, SIDE_TOP, SIDE_WIDTH, 104);
    gfx_text_size((s16)(SIDE_X + 16), (s16)(SIDE_TOP + 14), "Theme",
                  COLOR_DEEP, 12);
    gfx_text_size_right((s16)(SIDE_X + SIDE_WIDTH - 16),
                        (s16)(SIDE_TOP + 15),
                        theme_names[gfx_current_theme()], COLOR_DISABLED, 11);
    gui_button((s16)(SIDE_X + 16), (s16)(SIDE_TOP + 42),
               (s16)(SIDE_WIDTH - 32), 48, "Change theme", ID_THEME);

    /* Sound. */
    gui_section_label(SIDE_X, (s16)(SIDE_TOP + 118), "SOUND");
    gui_card(SIDE_X, (s16)(SIDE_TOP + 128), SIDE_WIDTH, 78);
    gfx_text_size((s16)(SIDE_X + 16), (s16)(SIDE_TOP + 142), "PC speaker",
                  COLOR_DEEP, 12);
    gui_button_colored((s16)(SIDE_X + 16), (s16)(SIDE_TOP + 162),
                       (s16)(SIDE_WIDTH - 32), 36, "Play chime",
                       ID_TEST_SOUND, COLOR_FACE, COLOR_DEEP);

    /* Power. */
    gui_section_label(SIDE_X, (s16)(SIDE_TOP + 220), "POWER");
    gfx_round_fill(SIDE_X, (s16)(SIDE_TOP + 230), SIDE_WIDTH, 64, 13,
                   COLOR_FACE);
    gui_button_colored((s16)(SIDE_X + 12), (s16)(SIDE_TOP + 240),
                       (s16)((SIDE_WIDTH - 32) / 2), 44, "Lock", ID_LOCK,
                       COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(SIDE_X + 20 + (SIDE_WIDTH - 32) / 2),
                       (s16)(SIDE_TOP + 240), (s16)((SIDE_WIDTH - 32) / 2), 44,
                       "Restart", ID_RESTART, COLOR_ALERT, COLOR_FACE);

#ifdef DIMOS_HOST
    gfx_text_size(ROWS_X, (s16)(SIDE_TOP + 306),
                  "DimOS 14 - an iOS 14 inspired shell, hosted", COLOR_DISABLED,
                  9);
    gfx_text_size(ROWS_X, (s16)(SIDE_TOP + 320), "on this machine.",
                  COLOR_DISABLED, 9);
#else
    gfx_text_size(ROWS_X, (s16)(SIDE_TOP + 306),
                  "DimOS 14 - an iOS 14 inspired shell", COLOR_DISABLED, 9);
    gfx_text_size(ROWS_X, (s16)(SIDE_TOP + 320), "for i686.", COLOR_DISABLED,
                  9);
#endif
}

static void about_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }
    if (event->key == ID_THEME) {
        gfx_select_theme((u8)((gfx_current_theme() + 1u) % THEME_COUNT));
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
