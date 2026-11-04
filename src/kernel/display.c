/*
 * display.c -- the phone panel DimOS pretends to have.
 *
 * The shell draws a 844x390 logical canvas (a 390x844 point phone screen
 * rotated into landscape). A real phone does not just have "some" pixels:
 * it has a native panel, a point grid, a pixel density, a scale factor, a
 * refresh rate, rounded corners, a camera cutout and a safe area. This file
 * keeps those numbers together for four real device classes, derives the
 * others from them and lets Settings or the terminal switch the panel.
 *
 * Nothing here invents a new framebuffer: gfx.c still fits the canvas into
 * whatever VBE mode the BIOS offered. The profile says what that mode is
 * supposed to represent, how fast the compositor should run and where the
 * status bar must stay clear of the notch.
 */

#include "dimos.h"

/* name, slug, native portrait pixels, logical portrait points, ppi, Hz,
 * diagonal in tenths of an inch, scale in tenths, aspect (x10 : y),
 * cutout, corner radius, portrait top/bottom/side safe insets in points. */
static const PhoneProfile profiles[DISPLAY_PROFILE_COUNT] = {
    { "iPhone 14", "IPHONE14", 1170u, 2532u, 390u, 844u, 460u, 60u, 61u,
      30u, 195u, 9u, PANEL_NOTCH, 40u, 47u, 34u, 0u },
    { "iPhone 15 Pro", "IPHONE15PRO", 1179u, 2556u, 393u, 852u, 460u, 120u,
      61u, 30u, 195u, 9u, PANEL_ISLAND, 44u, 59u, 34u, 0u },
    { "1.5K Phone", "1.5K", 1220u, 2712u, 407u, 904u, 452u, 120u, 64u,
      30u, 200u, 9u, PANEL_HOLE, 30u, 28u, 24u, 0u },
    { "FHD+ Phone", "FHD", 1080u, 2400u, 360u, 800u, 402u, 60u, 65u,
      30u, 200u, 9u, PANEL_HOLE, 26u, 24u, 16u, 0u }
};

/* The default is the 1170x2532 class: rotated it is 2532x1170, exactly
 * three times the 844x390 canvas, so a phone-native mode needs no letterbox
 * at all. Kept in .data-like state so the sandbox can read it back. */
static u8 display_active;

void display_init(void) {
    display_active = 0u;
}

u8 display_profile_count(void) {
    return DISPLAY_PROFILE_COUNT;
}

u8 display_profile_index(void) {
    return display_active;
}

const PhoneProfile *display_profile(u8 index) {
    if (index >= DISPLAY_PROFILE_COUNT) {
        return (const PhoneProfile *)0;
    }
    return &profiles[index];
}

const PhoneProfile *display_current(void) {
    return &profiles[display_active];
}

/* Selecting a panel also reprograms the PIT: a 120 Hz phone really does
 * compose at 120 Hz, while a 60 Hz one keeps the old 60 Hz cadence. The
 * caller persists the choice with settings_save(). */
u8 display_select_profile(u8 index) {
    const PhoneProfile *panel;

    if (index >= DISPLAY_PROFILE_COUNT || index == display_active) {
        return 0u;
    }
    panel = &profiles[index];
    display_active = index;
    timer_set_refresh_hz((u8)panel->refresh_hz);
    return 1u;
}

u8 display_next_profile(void) {
    return display_select_profile(
        (u8)((display_active + 1u) % DISPLAY_PROFILE_COUNT));
}

u8 display_select_named(const char *name) {
    u8 index;

    if (name == (const char *)0 || name[0] == '\0') {
        return 0u;
    }
    if (name[0] >= '1' && name[0] <= (char)('0' + DISPLAY_PROFILE_COUNT) &&
        name[1] == '\0') {
        return display_select_profile((u8)(name[0] - '1'));
    }
    for (index = 0u; index < DISPLAY_PROFILE_COUNT; ++index) {
        if (text_equal_ignore_case(name, profiles[index].slug) != 0u) {
            return display_select_profile(index);
        }
    }
    return 0u;
}

u16 display_ppi(void) {
    return profiles[display_active].ppi;
}

/* Logical density: one point is scale/10 physical pixels, so a 460 ppi
 * @3x panel is a 153 dpi point grid. */
u16 display_dpi(void) {
    const PhoneProfile *panel = &profiles[display_active];
    return (u16)(((u32)panel->ppi * 10u) / panel->scale_10);
}

u16 display_refresh_hz(void) {
    return profiles[display_active].refresh_hz;
}

u16 display_landscape_width(void) {
    return profiles[display_active].pixels_y;
}

u16 display_landscape_height(void) {
    return profiles[display_active].pixels_x;
}

/* In landscape the portrait top inset (the notch) becomes the left inset
 * and the portrait home-indicator inset becomes the right one. */
u8 display_safe_left(void) {
    return profiles[display_active].inset_top_pt;
}

u8 display_safe_right(void) {
    return profiles[display_active].inset_bottom_pt;
}

u8 display_safe_top(void) {
    return profiles[display_active].inset_side_pt;
}

u8 display_safe_bottom(void) {
    return profiles[display_active].inset_side_pt;
}

/* Uniform fit of the logical canvas into a physical mode, without ever
 * stretching it. The same arithmetic gfx_set_output_resolution() uses. */
void display_fit(u16 width, u16 height, u16 *render_width,
                 u16 *render_height, u16 *margin_x, u16 *margin_y) {
    u32 fit_width = width;
    u32 fit_height = height;

    if (fit_width * SCREEN_HEIGHT > fit_height * SCREEN_WIDTH) {
        fit_width = fit_height * SCREEN_WIDTH / SCREEN_HEIGHT;
    } else {
        fit_height = fit_width * SCREEN_HEIGHT / SCREEN_WIDTH;
    }
    if (render_width != (u16 *)0) {
        *render_width = (u16)fit_width;
    }
    if (render_height != (u16 *)0) {
        *render_height = (u16)fit_height;
    }
    if (margin_x != (u16 *)0) {
        *margin_x = (u16)((width - fit_width) / 2u);
    }
    if (margin_y != (u16 *)0) {
        *margin_y = (u16)((height - fit_height) / 2u);
    }
}

static void append_decimal(char *out, u16 capacity, u32 tenths) {
    text_append_number(out, tenths / 10u, capacity);
    text_append_character(out, '.', capacity);
    text_append_number(out, tenths % 10u, capacity);
}

void display_panel_line(char *out, u16 capacity) {
    const PhoneProfile *panel = &profiles[display_active];

    out[0] = '\0';
    text_append(out, panel->name, capacity);
    text_append(out, "  ", capacity);
    text_append_number(out, panel->pixels_x, capacity);
    text_append_character(out, 'x', capacity);
    text_append_number(out, panel->pixels_y, capacity);
    text_append(out, "  ", capacity);
    text_append_number(out, panel->ppi, capacity);
    text_append(out, " ppi  ", capacity);
    text_append_number(out, panel->refresh_hz, capacity);
    text_append(out, " Hz", capacity);
}

static const char *const row_labels[8] = {
    "Model", "Resolution", "Logical", "Density",
    "Scale", "Aspect", "Refresh", "Safe area"
};

const char *display_row_label(u8 row) {
    return (row < 8u) ? row_labels[row] : "";
}

void display_row_value(u8 row, char *out, u16 capacity) {
    const PhoneProfile *panel = &profiles[display_active];

    out[0] = '\0';
    switch (row) {
        case 0u:
            text_append(out, panel->name, capacity);
            break;
        case 1u:
            text_append_number(out, panel->pixels_x, capacity);
            text_append(out, " x ", capacity);
            text_append_number(out, panel->pixels_y, capacity);
            text_append(out, " px", capacity);
            break;
        case 2u:
            text_append_number(out, panel->points_x, capacity);
            text_append(out, " x ", capacity);
            text_append_number(out, panel->points_y, capacity);
            text_append(out, " pt", capacity);
            break;
        case 3u:
            text_append_number(out, panel->ppi, capacity);
            text_append(out, " ppi / ", capacity);
            text_append_number(out, display_dpi(), capacity);
            text_append(out, " dpi", capacity);
            break;
        case 4u:
            text_append_character(out, '@', capacity);
            append_decimal(out, capacity, panel->scale_10);
            text_append(out, "x -> ", capacity);
            text_append_number(out, panel->pixels_y, capacity);
            text_append_character(out, 'x', capacity);
            text_append_number(out, panel->pixels_x, capacity);
            break;
        case 5u:
            append_decimal(out, capacity, panel->aspect_x10);
            text_append_character(out, ':', capacity);
            text_append_number(out, panel->aspect_y, capacity);
            text_append(out, ", ", capacity);
            append_decimal(out, capacity, panel->diagonal_10);
            text_append(out, " in", capacity);
            break;
        case 6u:
            text_append_number(out, panel->refresh_hz, capacity);
            text_append(out, " Hz / ", capacity);
            append_decimal(out, capacity,
                           (10000u + panel->refresh_hz / 2u) /
                           panel->refresh_hz);
            text_append(out, " ms", capacity);
            break;
        default:
            text_append(out, "L/R ", capacity);
            text_append_number(out, panel->inset_top_pt, capacity);
            text_append_character(out, '/', capacity);
            text_append_number(out, panel->inset_bottom_pt, capacity);
            text_append(out, " pt, T/B ", capacity);
            text_append_number(out, panel->inset_side_pt, capacity);
            break;
    }
}

/* A short line for Control Centre: what the physical mode is doing. */
void display_output_line(char *out, u16 capacity) {
    const u8 vbe = (u8)(video_backend == VIDEO_BACKEND_VBE);
    const u16 width = vbe ? video_width : 320u;
    const u16 height = vbe ? video_height : 200u;
    u16 render_width;
    u16 render_height;
    u16 margin_x;
    u16 margin_y;

    out[0] = '\0';
    display_fit(width, height, &render_width, &render_height,
                &margin_x, &margin_y);
    text_append_number(out, width, capacity);
    text_append_character(out, 'x', capacity);
    text_append_number(out, height, capacity);
    text_append(out, " -> ", capacity);
    text_append_number(out, render_width, capacity);
    text_append_character(out, 'x', capacity);
    text_append_number(out, render_height, capacity);
    text_append(out, " +", capacity);
    text_append_number(out, margin_x, capacity);
    text_append(out, "/+", capacity);
    text_append_number(out, margin_y, capacity);
}

/* Where the camera cutout sits on the landscape canvas. The top centre is
 * empty in every status bar, so the notch never covers the clock or icons. */
void display_cutout(s16 *x, s16 *y, s16 *width, s16 *height) {
    const PhoneProfile *panel = &profiles[display_active];

    *x = 0;
    *y = 0;
    *width = 0;
    *height = 0;
    switch (panel->cutout) {
        case PANEL_NOTCH:
            *width = 150;
            *height = 18;
            break;
        case PANEL_ISLAND:
            *width = 108;
            *height = 16;
            break;
        case PANEL_HOLE:
            *width = 14;
            *height = 14;
            break;
        default:
            return;
    }
    *x = (s16)((SCREEN_WIDTH - *width) / 2u);
    *y = 2;
}

/* Rounded screen corners and the camera cutout, composited last so every
 * page -- home, app and Control Centre -- looks like one phone screen. */
void display_draw_bezel(void) {
    const PhoneProfile *panel = &profiles[display_active];
    const s16 radius = (s16)(panel->radius_pt / 3u);
    s16 x;
    s16 y;
    s16 width;
    s16 height;

    for (y = 0; y < radius; ++y) {
        for (x = 0; x < radius; ++x) {
            const s16 dx = (s16)(radius - x);
            const s16 dy = (s16)(radius - y);

            if ((s32)dx * dx + (s32)dy * dy <= (s32)radius * radius) {
                continue;
            }
            gfx_pixel(x, y, COLOR_BLACK);
            gfx_pixel((s16)(SCREEN_WIDTH - 1 - x), y, COLOR_BLACK);
            gfx_pixel(x, (s16)(SCREEN_HEIGHT - 1 - y), COLOR_BLACK);
            gfx_pixel((s16)(SCREEN_WIDTH - 1 - x),
                      (s16)(SCREEN_HEIGHT - 1 - y), COLOR_BLACK);
        }
    }

    display_cutout(&x, &y, &width, &height);
    if (width == 0) {
        return;
    }
    if (panel->cutout == PANEL_HOLE) {
        gfx_circle((s16)(x + width / 2), (s16)(y + height / 2),
                   (s16)(width / 2), COLOR_BLACK, 1u);
    } else {
        gfx_round_fill(x, y, width, height, (s16)(height / 2), COLOR_BLACK);
    }
}

/* A little portrait phone in Settings: outline, cutout and safe area. */
void display_draw_mini(s16 x, s16 y, s16 width, s16 height) {
    const PhoneProfile *panel = &profiles[display_active];
    const s16 top = (s16)(y + panel->inset_top_pt * height /
                          panel->points_y);
    const s16 bottom = (s16)(panel->inset_bottom_pt * height /
                             panel->points_y);
    const s16 side = (s16)(panel->inset_side_pt * width / panel->points_x);

    gfx_round_outline(x, y, width, height, 8, COLOR_DISABLED);
    gfx_outline((s16)(x + side + 2), top, (s16)(width - 2 * side - 4),
                (s16)(height - (top - y) - bottom - 2), COLOR_SHADOW);
    if (panel->cutout == PANEL_HOLE) {
        gfx_circle((s16)(x + width / 2), (s16)(y + 6), 3, COLOR_DEEP, 1u);
    } else if (panel->cutout != PANEL_NONE) {
        gfx_round_fill((s16)(x + width / 2 - 9), y, 18, 6, 3, COLOR_DEEP);
    }
}
