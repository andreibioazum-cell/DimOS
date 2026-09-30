/*
 * app_about.c -- what this machine is doing.
 *
 * The numbers here are read from the machine itself: the BIOS memory count,
 * the clock chip, the boot sector of the disk DimOS started from, and the
 * state of the mouse driver.
 */

#include "dimos.h"

#define ID_THEME 1u
#define ID_RESTART 2u
#define ID_TEST_SOUND 3u

static const char *const icon[16] = {
    "..XXXXXXXXXXXX..",
    "..XooooooooooX..",
    "..XooooooooooX..",
    "..XXXXXXXXXXXX..",
    "..X.....XX....X.",
    "..X...........X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..X.....XX....X.",
    "..XXXXXXXXXXXX..",
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

static void info_line(s16 y, const char *label, const char *value) {
    gfx_text((s16)(WINDOW_LEFT + 6), y, label, COLOR_BLACK);
    gfx_text((s16)(WINDOW_LEFT + 120), y, value, COLOR_BLUE);
}

static void about_open(void) {
    /* Nothing to prepare; every number is read while drawing. */
}

static void about_draw(void) {
    char value[32];
    s16 y = (s16)(WINDOW_TOP + 3);

    gfx_text((s16)(WINDOW_LEFT + 6), y, "DimOS 2.0", COLOR_BLACK);
    gfx_text((s16)(WINDOW_LEFT + 120), y, "x86-64 kernel in C", COLOR_BLUE);
    y = (s16)(y + 10);

    /* xfce4-about would say the same, give or take a mouse. */
    info_line(y, "Shell", "DimXfce am-nyam shell");
    y = (s16)(y + 10);

    info_line(y, "Video",
              (video_backend == VIDEO_BACKEND_VBE && video_width == 1920u)
                  ? "VBE GPU 1920x1080 XRGB"
                  : ((video_backend == VIDEO_BACKEND_VBE)
                         ? "VBE 640x480 fallback"
                         : "VGA 320x200 fallback"));
    y = (s16)(y + 10);

    value[0] = '\0';
    text_append(value, "Theme: ", (u16)sizeof(value));
    text_append(value, theme_names[gfx_current_theme()], (u16)sizeof(value));
    info_line(y, "Palette", value);
    y = (s16)(y + 10);

    /* The BIOS writes how much memory sits below one megabyte, in KiB,
     * into its data area at 0x413. */
    value[0] = '\0';
    text_append_number(value, (u32)bios_read_word(0x00000413u), (u16)sizeof(value));
    text_append(value, " KiB", (u16)sizeof(value));
    info_line(y, "Memory", value);
    y = (s16)(y + 10);

    value[0] = '\0';
    text_append_number(value, ram_disk_sectors() / 2u, (u16)sizeof(value));
    text_append(value, " KiB RAM disk", (u16)sizeof(value));
    info_line(y, "Storage", value);
    y = (s16)(y + 10);

    value[0] = '\0';
    text_append_number(value, file_system_visible_count(), (u16)sizeof(value));
    text_append(value, " files on disk", (u16)sizeof(value));
    info_line(y, "Files", value);
    y = (s16)(y + 10);

    info_line(y, "Mouse", (input_mouse_available() != 0u) ? "PS/2 mouse found"
                                                          : "not found - keys work");
    y = (s16)(y + 10);

    value[0] = '\0';
    two_digits(value, (u16)sizeof(value), clock_hours());
    text_append_character(value, ':', (u16)sizeof(value));
    two_digits(value, (u16)sizeof(value), clock_minutes());
    text_append_character(value, ':', (u16)sizeof(value));
    two_digits(value, (u16)sizeof(value), clock_seconds());
    info_line(y, "Clock", value);
    y = (s16)(y + 10);

    value[0] = '\0';
    text_append_number(value, clock_year(), (u16)sizeof(value));
    text_append_character(value, '-', (u16)sizeof(value));
    two_digits(value, (u16)sizeof(value), clock_month());
    text_append_character(value, '-', (u16)sizeof(value));
    two_digits(value, (u16)sizeof(value), clock_day());
    info_line(y, "Date", value);
    y = (s16)(y + 10);

    value[0] = '\0';
    text_append(value, "Up ", (u16)sizeof(value));
    text_append_number(value, time_milliseconds() / 1000u, (u16)sizeof(value));
    text_append(value, " s - see Cheesy!", (u16)sizeof(value));
    info_line(y, "Session", value);
    y = (s16)(y + 12);

    gui_button((s16)(WINDOW_LEFT + 6), y, 90, 15, "Theme", ID_THEME);
    gui_button((s16)(WINDOW_LEFT + 102), y, 90, 15, "Beep", ID_TEST_SOUND);
    gui_button((s16)(WINDOW_LEFT + 198), y, 90, 15, "Restart", ID_RESTART);

    gui_message_bar("The mouse menu waits at the top left");
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
    if (event->key == ID_RESTART) {
        system_restart();
    }
}

const Application application_about = {
    "About",
    "About",
    icon,
    COLOR_LIGHT_BLUE,
    about_open,
    about_draw,
    about_event,
    0
};
