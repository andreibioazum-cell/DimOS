/*
 * config.c -- the settings that survive a reboot.
 *
 * A phone that forgets its wallpaper every time it starts is a demo, not an
 * operating system. DimOS keeps a sixteen-byte record in CONFIG.CFG on its
 * disk: the palette, the brightness and volume sliders, and a checksum that
 * makes a half-written or corrupted file harmless. Nothing here touches the
 * screen; gui.c asks for the values once at start-up and writes them back
 * whenever the user moves a slider.
 */

#include "dimos.h"

#define SETTINGS_FILE "CONFIG.CFG"
#define SETTINGS_BYTES 16u
#define SETTINGS_VERSION 1u
#define SETTINGS_MAGIC 0x534F4D44u /* "DMOS" */

static u8 settings_valid;
static u8 settings_theme;
static u8 settings_brightness;
static u8 settings_volume;

static u8 record_checksum(const u8 *bytes, u16 length) {
    u8 sum = 0xA5u;
    u16 index;

    for (index = 0u; index < length; ++index) {
        sum = (u8)(sum ^ bytes[index]);
        sum = (u8)((u8)(sum << 1u) | (u8)(sum >> 7u));
    }
    return sum;
}

void settings_load(void) {
    u8 buffer[SETTINGS_BYTES];
    u32 magic;

    settings_valid = 0u;
    settings_theme = 0u;
    settings_brightness = 13u;
    settings_volume = 10u;

    if (volume_present() == 0u) {
        return;
    }
    if (volume_read_file(SETTINGS_FILE, buffer, SETTINGS_BYTES) != SETTINGS_BYTES) {
        return;
    }

    magic = (u32)buffer[0] | ((u32)buffer[1] << 8u) |
            ((u32)buffer[2] << 16u) | ((u32)buffer[3] << 24u);
    if (magic != SETTINGS_MAGIC || buffer[4] != SETTINGS_VERSION) {
        return;
    }
    if (record_checksum(buffer, (u16)(SETTINGS_BYTES - 1u)) != buffer[15]) {
        return;
    }
    if (buffer[5] >= THEME_COUNT || buffer[6] > 20u || buffer[7] > 20u) {
        return;
    }

    settings_theme = buffer[5];
    settings_brightness = buffer[6];
    settings_volume = buffer[7];
    settings_valid = 1u;
}

u8 settings_saved(void) {
    return settings_valid;
}

u8 settings_saved_theme(void) {
    return settings_theme;
}

u8 settings_saved_brightness(void) {
    return settings_brightness;
}

u8 settings_saved_volume(void) {
    return settings_volume;
}

/* Reads the current values out of the shell and writes them to the disk.
 * Called after a slider moves, a theme changes or the phone is locked, so
 * the record always matches what is on screen. */
u8 settings_save(void) {
    u8 buffer[SETTINGS_BYTES];

    settings_theme = gfx_current_theme();
    settings_brightness = gui_brightness();
    settings_volume = gui_volume();

    if (volume_present() == 0u) {
        /* No writable volume: the settings still apply to this session. */
        settings_valid = 0u;
        return 0u;
    }

    memory_zero(buffer, SETTINGS_BYTES);
    buffer[0] = (u8)(SETTINGS_MAGIC & 0xFFu);
    buffer[1] = (u8)((SETTINGS_MAGIC >> 8u) & 0xFFu);
    buffer[2] = (u8)((SETTINGS_MAGIC >> 16u) & 0xFFu);
    buffer[3] = (u8)((SETTINGS_MAGIC >> 24u) & 0xFFu);
    buffer[4] = SETTINGS_VERSION;
    buffer[5] = settings_theme;
    buffer[6] = settings_brightness;
    buffer[7] = settings_volume;
    buffer[15] = record_checksum(buffer, (u16)(SETTINGS_BYTES - 1u));

    if (volume_write_file(SETTINGS_FILE, buffer, SETTINGS_BYTES) == 0u) {
        settings_valid = 0u;
        return 0u;
    }
    settings_valid = 1u;
    return 1u;
}

/* Puts the saved values back into the shell. Only called once, right after
 * the graphics layer exists and before the first frame. */
void settings_apply(void) {
    if (settings_valid == 0u) {
        return;
    }
    gfx_select_theme(settings_theme);
    gui_set_brightness(settings_brightness);
    gui_set_volume(settings_volume);
}

u8 settings_forget(void) {
    settings_valid = 0u;
    if (volume_present() == 0u) {
        return 0u;
    }
    return volume_delete_file(SETTINGS_FILE);
}
