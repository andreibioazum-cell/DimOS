/*
 * gui.c -- the DimOS phone shell.
 *
 * DimOS presents itself as a pocket computer: a lock screen with a big
 * clock, a home screen of rounded app icons over an iOS 14 style
 * wallpaper, a translucent dock, a status bar and a Control Centre. Pages
 * are drawn edge to edge with a navigation bar and a home indicator, the
 * way a phone application is laid out, and every control is a touch-sized
 * hotspot so the same build works on a real touch panel, in a virtual
 * machine and under a PS/2 mouse on a PC.
 *
 * Everything on screen is either a shape or a button, and every button
 * announces where it is while it is being drawn. A tap is then simply
 * matched against that list, which keeps the applications free of any
 * event arithmetic: they draw a button, they get told when it is tapped.
 *
 * Nothing in DimOS needs a keyboard. The pointer does everything, arrow
 * keys and Enter walk the same buttons for people who have a keyboard,
 * Escape is the home gesture, and the terminal brings its own keyboard.
 */

#include "dimos.h"

/* 1,193,182 PIT clocks per second / 60, rounded up. Waiting against the raw
 * hardware counter gives a strict maximum of 60 FPS without the old 10 ms
 * clock rounding the compositor down to 50 or 25 FPS. */
#define FRAME_TIMER_COUNTS 19886ull

/* Ids the shell keeps for its own controls. Applications use small ids. */
#define HOTSPOT_ICON_BASE 0x8000u
#define HOTSPOT_DOCK_BASE 0x8100u
#define HOTSPOT_BACK 0x8200u
#define HOTSPOT_HOME_BAR 0x8201u
#define HOTSPOT_STATUS_RIGHT 0x8202u
#define HOTSPOT_DESKTOP 0x8203u
#define HOTSPOT_CC_OUTSIDE 0x8204u
#define HOTSPOT_CC_TOGGLE_BASE 0x8210u /* 4 connectivity toggles        */
#define HOTSPOT_CC_BRIGHTNESS 0x8220u
#define HOTSPOT_CC_VOLUME 0x8221u
#define HOTSPOT_CC_LOCK 0x8222u
#define HOTSPOT_CC_THEME 0x8223u
#define HOTSPOT_CC_MUSIC 0x8224u

/* Home screen geometry from dimos.h: four columns, three rows, and a dock
 * with four of the applications repeated, exactly like a phone. */
#define ICON_LABEL_HEIGHT 10u

/* Control Centre panel: a sheet that hangs from the top edge and is wide
 * enough to read on a phone, with everything stacked vertically. */
#define CC_PANEL_X 14
#define CC_PANEL_Y (-18)
#define CC_PANEL_WIDTH ((s16)(SCREEN_WIDTH - 2 * 14))
#define CC_PANEL_HEIGHT 520
#define CC_MARGIN 24
#define CC_INNER_WIDTH ((s16)(CC_PANEL_WIDTH - 2 * CC_MARGIN))

typedef struct {
    s16 x;
    s16 y;
    s16 width;
    s16 height;
    u16 id;
} Hotspot;

const Application *const application_list[] = {
    &application_files,
    &application_music,
    &application_calculator,
    &application_paint,
    &application_snake,
    &application_mines,
    &application_cheesy,
    &application_terminal,
    &application_about
};

const u16 application_count =
    (u16)(sizeof(application_list) / sizeof(application_list[0]));

/* Four applications live in the dock as well. */
static const u8 dock_apps[DOCK_SLOTS] = { 0u, 1u, 2u, 8u };

static Hotspot hotspots[HOTSPOT_LIMIT];
static u16 hotspot_count;
static u16 focused_id = HOTSPOT_NONE;
/* A ring around the focused button belongs to keyboard navigation only: a
 * finger tap moves the focus too, but it should not leave a halo behind. */
static u8 focus_ring;
static u16 pressed_id = HOTSPOT_NONE;
static u16 active_application = 0xFFFFu;
static u16 selected_icon;
static u8 arrows_reserved;
static char status_line[48];

/* Shell state. `locked` is the lock screen that greets the user, `control`
 * is the Control Centre sheet, the four switches are the connectivity
 * toggles and the two levels are the brightness and volume sliders. */
static u8 locked = 1u;
static u8 control_open;
static u8 toggles[4];
static u8 brightness = 13u;
static u8 volume = 10u;
static u8 status_light;
static u8 frame_dark;
static u8 title_accent = COLOR_SELECTION;

/* The wallpaper is generated once at four fifths of the screen size and
 * stretched over the canvas with a precomputed sample map, which keeps the
 * soft gradient cheap to repaint. Orderly dithering hides the steps of the
 * indexed palette. */
#define WALLPAPER_WIDTH ((SCREEN_WIDTH * 4u) / 5u)
#define WALLPAPER_HEIGHT ((SCREEN_HEIGHT * 4u) / 5u)
static u8 wallpaper_cache[WALLPAPER_WIDTH * WALLPAPER_HEIGHT];
static u16 wallpaper_x_map[SCREEN_WIDTH];
static u16 wallpaper_y_map[SCREEN_HEIGHT];
static u8 wallpaper_ready;

/* A 4x4 Bayer matrix, scaled to +-24 levels before quantizing. */
static const s8 wallpaper_dither[16] = {
    -10, 4, -6, 8,
    10, -4, 14, 0,
    -8, 6, -12, 2,
    8, -2, 12, -6
};

/* ------------------------------------------------------------------ */
/* Hotspots                                                            */
/* ------------------------------------------------------------------ */

void gui_hotspots_reset(void) {
    hotspot_count = 0u;
}

void gui_hotspot(s16 x, s16 y, s16 width, s16 height, u16 id) {
    if (hotspot_count >= HOTSPOT_LIMIT) {
        return;
    }
    hotspots[hotspot_count].x = x;
    hotspots[hotspot_count].y = y;
    hotspots[hotspot_count].width = width;
    hotspots[hotspot_count].height = height;
    hotspots[hotspot_count].id = id;
    ++hotspot_count;
}

/* The last registered hotspot wins, so a small button drawn on top of a big
 * panel is the one that gets the tap. */
u16 gui_hotspot_at(s16 x, s16 y) {
    u16 index = hotspot_count;

    while (index != 0u) {
        const Hotspot *spot;

        --index;
        spot = &hotspots[index];
        if (x >= spot->x && x < (s16)(spot->x + spot->width) &&
            y >= spot->y && y < (s16)(spot->y + spot->height)) {
            return spot->id;
        }
    }
    return HOTSPOT_NONE;
}

u8 gui_has_focus(u16 id) {
    return (u8)(focused_id == id);
}

u8 gui_is_pressed(u16 id) {
    return (u8)(pressed_id == id);
}

void gui_focus(u16 id) {
    focused_id = id;
    focus_ring = 0u;
}

/* Move the keyboard focus to the nearest hotspot in the given direction. */
void gui_move_focus(s16 step_x, s16 step_y) {
    u16 index;
    s16 from_x = (s16)(SCREEN_WIDTH / 2u);
    s16 from_y = (s16)(SCREEN_HEIGHT / 2u);
    u16 best_id = HOTSPOT_NONE;
    s32 best_distance = 0;

    if (hotspot_count == 0u) {
        return;
    }
    for (index = 0u; index < hotspot_count; ++index) {
        if (hotspots[index].id == focused_id) {
            from_x = (s16)(hotspots[index].x + hotspots[index].width / 2);
            from_y = (s16)(hotspots[index].y + hotspots[index].height / 2);
            break;
        }
    }
    for (index = 0u; index < hotspot_count; ++index) {
        const s16 center_x = (s16)(hotspots[index].x + hotspots[index].width / 2);
        const s16 center_y = (s16)(hotspots[index].y + hotspots[index].height / 2);
        const s16 delta_x = (s16)(center_x - from_x);
        const s16 delta_y = (s16)(center_y - from_y);
        s32 distance;

        if (step_x > 0 && delta_x <= 0) {
            continue;
        }
        if (step_x < 0 && delta_x >= 0) {
            continue;
        }
        if (step_y > 0 && delta_y <= 0) {
            continue;
        }
        if (step_y < 0 && delta_y >= 0) {
            continue;
        }
        distance = (s32)(delta_x < 0 ? -delta_x : delta_x) +
                   (s32)(delta_y < 0 ? -delta_y : delta_y);
        if (best_id == HOTSPOT_NONE || distance < best_distance) {
            best_id = hotspots[index].id;
            best_distance = distance;
        }
    }
    if (best_id == HOTSPOT_NONE) {
        best_id = hotspots[0].id;
    }
    focused_id = best_id;
    focus_ring = 1u;
}

/* Games keep the arrow keys for themselves; everything else uses them to
 * walk the buttons. */
void gui_reserve_arrow_keys(u8 reserve) {
    arrows_reserved = reserve;
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

void gui_button_colored(s16 x, s16 y, s16 width, s16 height, const char *label,
                        u16 id, u8 face, u8 text) {
    const u8 pressed = (pressed_id == id) ? 1u : 0u;
    const u16 text_height = (height >= 30) ? 12u : 10u;

    gfx_round_fill(x, y, width, height, 9,
                   (pressed != 0u) ? gfx_shade(face, COLOR_BLACK, 3) : face);
    gfx_text_size_centered((s16)(x + 2),
                           (s16)(y + (height - (s16)text_height) / 2),
                           (s16)(width - 4), label,
                           (pressed != 0u) ? COLOR_WHITE : text,
                           text_height);
    if (focus_ring != 0u && focused_id == id) {
        gfx_round_outline((s16)(x - 2), (s16)(y - 2), (s16)(width + 4),
                          (s16)(height + 4), 11, COLOR_WHITE);
    }
    gui_hotspot(x, y, width, height, id);
}

void gui_button(s16 x, s16 y, s16 width, s16 height, const char *label,
                u16 id) {
    gui_button_colored(x, y, width, height, label, id, COLOR_SELECTION,
                       COLOR_WHITE);
}

/* The iOS switch: a pill that turns green when it is on. */
void gui_switch(s16 x, s16 y, s16 width, s16 height, const char *label,
                u16 id, u8 on) {
    const s16 switch_width = 42;
    const s16 switch_height = 24;
    const s16 switch_x = (s16)(x + width - switch_width);
    const s16 switch_y = (s16)(y + (height - switch_height) / 2);
    const s16 knob_x = (on != 0u)
                           ? (s16)(switch_x + switch_width - 11)
                           : (s16)(switch_x + 11);

    gfx_text_size(x, (s16)(y + (height - 12) / 2), label, COLOR_DEEP, 12);
    gfx_round_fill(switch_x, switch_y, switch_width, switch_height, 12,
                   (on != 0u) ? COLOR_GOOD : COLOR_SHADOW);
    gfx_round_fill((s16)(knob_x - 10), (s16)(switch_y + 2), 20, 20, 10,
                   COLOR_FACE);
    if (focus_ring != 0u && focused_id == id) {
        gfx_round_outline((s16)(switch_x - 3), (s16)(switch_y - 3),
                          (s16)(switch_width + 6), (s16)(switch_height + 6),
                          15, COLOR_SELECTION);
    }
    gui_hotspot(x, y, width, height, id);
}

/* A grouped white card, the background of every settings list. */
void gui_card(s16 x, s16 y, s16 width, s16 height) {
    gfx_round_fill(x, y, width, height, 13, COLOR_FACE);
    gfx_round_outline(x, y, width, height, 13,
                      gfx_shade(COLOR_GRID, COLOR_FACE, 10));
}

void gui_section_label(s16 x, s16 y, const char *text) {
    gfx_text_size(x, y, text, COLOR_DISABLED, 9);
}

void gui_list_row(s16 x, s16 y, s16 width, s16 height, const char *label,
                  const char *value, u8 last) {
    gfx_text_size((s16)(x + 16), (s16)(y + (height - 12) / 2), label,
                  COLOR_DEEP, 12);
    if (value != (const char *)0 && value[0] != '\0') {
        gfx_text_size_right((s16)(x + width - 16),
                            (s16)(y + (height - 11) / 2), value,
                            COLOR_DISABLED, 11);
    }
    if (last == 0u) {
        gfx_horizontal_line((s16)(x + 16), (s16)(y + height - 1),
                            (s16)(width - 32),
                            gfx_shade(COLOR_GRID, COLOR_FACE, 8));
    }
}

/* The small caption at the bottom of an application page. */
void gui_message_bar(const char *text) {
    gfx_text_size(APP_LEFT, (s16)(SCREEN_HEIGHT - 24), text, COLOR_DISABLED,
                  9);
    text_copy(status_line, text, (u16)sizeof(status_line));
}

/* Applications that paint a dark page ask for a light status bar. */
void gui_status_light(u8 light) {
    status_light = light;
}

void gui_window_title_color(u8 top_color) {
    title_accent = top_color;
}

static void draw_status_bar(u8 light);
static void draw_home_indicator(u8 light);

/* The navigation bar of an application page: a back control on the left,
 * the title in the middle and a hairline at the bottom. */
static void app_page(const char *title, u8 background) {
    gfx_fill(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT, background);
    gfx_text_size(APP_LEFT, (s16)(STATUS_BAR_HEIGHT + 11), "<", title_accent,
                  11);
    gfx_text_size((s16)(APP_LEFT + 12), (s16)(STATUS_BAR_HEIGHT + 12),
                  "Home", title_accent, 11);
    gfx_text_size_centered(0, (s16)(STATUS_BAR_HEIGHT + 9), (s16)SCREEN_WIDTH,
                           title, COLOR_DEEP, 17);
    gfx_horizontal_line(APP_LEFT, (s16)(APP_TOP - 4), (s16)APP_WIDTH,
                        gfx_shade(COLOR_GRID, background, 8));
    gui_hotspot((s16)(APP_LEFT - 8), (s16)STATUS_BAR_HEIGHT,
                (s16)100, (s16)NAV_BAR_HEIGHT, HOTSPOT_BACK);
}

void gui_window_frame(const char *title) {
    frame_dark = 0u;
    status_light = 0u;
    title_accent = COLOR_SELECTION;
    app_page(title, COLOR_PANEL);
}

void gui_window_frame_dark(const char *title, u8 background) {
    frame_dark = 1u;
    status_light = 1u;
    title_accent = COLOR_WIN_LIGHT;
    app_page(title, background);
    /* The dark page needs light navigation text. */
    gfx_text_size_centered(0, (s16)(STATUS_BAR_HEIGHT + 9), (s16)SCREEN_WIDTH,
                           title, COLOR_FACE, 17);
    gfx_text_size(APP_LEFT, (s16)(STATUS_BAR_HEIGHT + 11), "<",
                  COLOR_WIN_LIGHT, 11);
    gfx_text_size((s16)(APP_LEFT + 12), (s16)(STATUS_BAR_HEIGHT + 12),
                  "Home", COLOR_WIN_LIGHT, 11);
    gfx_horizontal_line(APP_LEFT, (s16)(APP_TOP - 4), (s16)APP_WIDTH,
                        gfx_shade(COLOR_DEEP, background, 8));
}

/* ------------------------------------------------------------------ */
/* Wallpaper                                                           */
/* ------------------------------------------------------------------ */

static s16 clamp_channel(s32 value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (s16)value;
}

static void add_blob(s32 *red, s32 *green, s32 *blue, s32 x, s32 y,
                     s32 center_x, s32 center_y, s32 radius, s32 weight,
                     s32 tint_red, s32 tint_green, s32 tint_blue) {
    const s32 delta_x = x - center_x;
    const s32 delta_y = y - center_y;
    const s32 distance = delta_x * delta_x + delta_y * delta_y;
    const s32 square = radius * radius;

    if (distance < square) {
        const s32 amount = ((square - distance) * weight) / square;

        *red += ((tint_red - *red) * amount) / 256;
        *green += ((tint_green - *green) * amount) / 256;
        *blue += ((tint_blue - *blue) * amount) / 256;
    }
}

/* A soft multi-colour gradient in the spirit of the iOS 14 wallpaper:
 * a deep indigo base with pink, cyan and violet light bleeding through
 * it, darker in the corners so white text always reads. */
static void wallpaper_build(void) {
    u16 y;

    for (y = 0u; y < WALLPAPER_HEIGHT; ++y) {
        const s32 py = (s32)y * 256 / (s32)(WALLPAPER_HEIGHT - 1u);
        u16 x;

        for (x = 0u; x < WALLPAPER_WIDTH; ++x) {
            const s32 px = (s32)x * 256 / (s32)(WALLPAPER_WIDTH - 1u);
            s32 red = 26 + (104 - 26) * py / 256;
            s32 green = 24 + (54 - 24) * py / 256;
            s32 blue = 84 + (168 - 84) * py / 256;

            add_blob(&red, &green, &blue, px, py, 176, 26, 230, 190,
                     255, 112, 196);
            add_blob(&red, &green, &blue, px, py, 44, 150, 190, 150,
                     58, 190, 255);
            add_blob(&red, &green, &blue, px, py, 300, 214, 250, 170,
                     136, 84, 255);
            add_blob(&red, &green, &blue, px, py, 380, 40, 170, 150,
                     18, 18, 74);
            add_blob(&red, &green, &blue, px, py, 236, 256, 210, 130,
                     38, 28, 96);

            {
                const s16 shift = wallpaper_dither[(y & 3u) * 4u + (x & 3u)];

                wallpaper_cache[(u32)y * WALLPAPER_WIDTH + x] =
                    gfx_color_from_rgb(
                        (u8)clamp_channel(red + shift),
                        (u8)clamp_channel(green + shift),
                        (u8)clamp_channel(blue + shift));
            }
        }
    }
    for (y = 0u; y < SCREEN_WIDTH; ++y) {
        wallpaper_x_map[y] =
            (u16)(((u32)y * (WALLPAPER_WIDTH - 1u)) / (SCREEN_WIDTH - 1u));
    }
    for (y = 0u; y < SCREEN_HEIGHT; ++y) {
        wallpaper_y_map[y] =
            (u16)(((u32)y * (WALLPAPER_HEIGHT - 1u)) / (SCREEN_HEIGHT - 1u));
    }
    wallpaper_ready = 1u;
}

static void draw_wallpaper(void) {
    u16 y;

    if (wallpaper_ready == 0u) {
        wallpaper_build();
    }
    for (y = 0u; y < SCREEN_HEIGHT; ++y) {
        const u8 *source = wallpaper_cache +
                           (u32)wallpaper_y_map[y] * WALLPAPER_WIDTH;
        u8 *target = screen + (u32)y * SCREEN_WIDTH;
        u16 x;

        for (x = 0u; x < SCREEN_WIDTH; ++x) {
            target[x] = source[wallpaper_x_map[x]];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Status bar, dock and home indicator                                 */
/* ------------------------------------------------------------------ */

static void draw_signal(u8 light) {
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;
    const u16 heights[4] = { 3u, 5u, 7u, 9u };
    u8 index;

    for (index = 0u; index < 4u; ++index) {
        gfx_fill((s16)(SCREEN_WIDTH - 126 + index * 5),
                 (s16)(16 - heights[index]), 3, (s16)heights[index], color);
    }
}

static void draw_wifi(u8 light) {
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;
    const s16 center_x = (s16)(SCREEN_WIDTH - 69);
    const s16 base_y = 16;

    gfx_fill((s16)(center_x - 1), (s16)(base_y - 2), 3, 3, color);
    gfx_line((s16)(center_x - 7), (s16)(base_y - 7),
             (s16)(center_x - 4), (s16)(base_y - 10), color);
    gfx_line((s16)(center_x - 4), (s16)(base_y - 10),
             (s16)center_x, (s16)(base_y - 11), color);
    gfx_line((s16)center_x, (s16)(base_y - 11),
             (s16)(center_x + 4), (s16)(base_y - 10), color);
    gfx_line((s16)(center_x + 4), (s16)(base_y - 10),
             (s16)(center_x + 7), (s16)(base_y - 7), color);
    gfx_line((s16)(center_x - 4), (s16)(base_y - 5),
             (s16)(center_x - 2), (s16)(base_y - 7), color);
    gfx_line((s16)(center_x - 2), (s16)(base_y - 7),
             (s16)center_x, (s16)(base_y - 8), color);
    gfx_line((s16)center_x, (s16)(base_y - 8),
             (s16)(center_x + 2), (s16)(base_y - 7), color);
    gfx_line((s16)(center_x + 2), (s16)(base_y - 7),
             (s16)(center_x + 4), (s16)(base_y - 5), color);
}

static void draw_battery(u8 light) {
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;
    const s16 x = (s16)(SCREEN_WIDTH - 50);
    const s16 y = 6;

    gfx_round_outline(x, y, 24, 12, 4, color);
    gfx_fill((s16)(x + 25), (s16)(y + 4), 2, 4, color);
    gfx_round_fill((s16)(x + 2), (s16)(y + 2), 14, 8, 2, light != 0u
                                                           ? COLOR_FACE
                                                           : COLOR_DEEP);
}

static void clock_text(char *out, u16 capacity, u8 short_hours) {
    const u8 hours = clock_hours();
    u8 shown = hours;

    out[0] = '\0';
    if (short_hours != 0u) {
        shown = (u8)(hours % 12u);
        if (shown == 0u) {
            shown = 12u;
        }
    } else if (hours < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, shown, capacity);
    text_append_character(out, ':', capacity);
    if (clock_minutes() < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, clock_minutes(), capacity);
}

static void draw_status_bar(u8 light) {
    char clock_line[8];

    clock_text(clock_line, (u16)sizeof(clock_line), 1u);
    gfx_text_size(24, 5, clock_line, (light != 0u) ? COLOR_FACE : COLOR_DEEP,
                  11);
    gfx_text_size((s16)(SCREEN_WIDTH - 100), 6, "5G",
                  (light != 0u) ? COLOR_FACE : COLOR_DEEP, 9);
    draw_signal(light);
    draw_wifi(light);
    draw_battery(light);
}

static void draw_home_indicator(u8 light) {
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;

    gfx_round_fill((s16)((SCREEN_WIDTH - HOME_INDICATOR_WIDTH) / 2),
                   (s16)HOME_INDICATOR_Y, (s16)HOME_INDICATOR_WIDTH, 4, 2,
                   color);
    gui_hotspot((s16)((SCREEN_WIDTH - 220) / 2), (s16)(HOME_INDICATOR_Y - 8),
                220, 18, HOTSPOT_HOME_BAR);
}

/* ------------------------------------------------------------------ */
/* App icons                                                           */
/* ------------------------------------------------------------------ */

static void app_icon(s16 x, s16 y, const Application *app, s16 size,
                     u8 glyph_scale) {
    const s16 radius = (s16)(size / 4);

    gfx_round_fill(x, y, size, size, radius,
                   (pressed_id != HOTSPOT_NONE &&
                    pressed_id >= HOTSPOT_DOCK_BASE &&
                    pressed_id < (u16)(HOTSPOT_DOCK_BASE + DOCK_SLOTS))
                       ? gfx_shade(app->color, COLOR_BLACK, 3)
                       : app->color);
    /* A gentle light from above makes the flat tint read as a glass tile. */
    gfx_round_gloss(x, y, size, size, radius);
    gfx_glyph((s16)(x + (size - (s16)(16 * glyph_scale)) / 2),
              (s16)(y + (size - (s16)(16 * glyph_scale)) / 2), app->icon, 16u,
              glyph_scale, COLOR_FACE);
}

static void home_icon(s16 column, s16 row, u16 index) {
    const Application *app = application_list[index];
    const s16 x = (s16)(ICON_GRID_LEFT + column * (s16)ICON_COLUMN_STEP);
    const s16 y = (s16)(ICON_GRID_TOP + row * (s16)ICON_ROW_STEP);
    const u16 id = (u16)(HOTSPOT_ICON_BASE + index);
    const u8 selected = (u8)(selected_icon == index);

    app_icon(x, y, app, (s16)ICON_SIZE, 3);

    /* The label sits on the wallpaper, so it gets a shadow. */
    if (selected != 0u) {
        gfx_round_fill((s16)(x - 6), (s16)(y + ICON_SIZE + 3),
                       (s16)(ICON_SIZE + 12), 16, 6, COLOR_SELECTION);
    }
    gfx_text_size_shadowed(
        (s16)(x + (s16)(ICON_SIZE - (s16)gfx_text_size_width(
                                            app->short_label,
                                            ICON_LABEL_HEIGHT)) / 2),
        (s16)(y + ICON_SIZE + 7), app->short_label, COLOR_FACE,
        ICON_LABEL_HEIGHT);
    gui_hotspot((s16)(x - 8), (s16)(y - 4), (s16)(ICON_SIZE + 16),
                (s16)(ICON_SIZE + 26), id);
}

static void draw_home_screen(void) {
    u8 index;

    draw_wallpaper();

    for (index = 0u; index < application_count &&
                    index < ICON_COLUMNS * ICON_ROWS; ++index) {
        home_icon((s16)(index % ICON_COLUMNS), (s16)(index / ICON_COLUMNS),
                  index);
    }

    /* Page dots, as every phone home screen has. */
    gfx_round_fill((s16)(SCREEN_WIDTH / 2 - 22), (s16)PAGE_DOTS_Y, 6, 6, 3,
                   COLOR_FACE);
    gfx_round_fill_alpha((s16)(SCREEN_WIDTH / 2), (s16)PAGE_DOTS_Y, 6, 6, 3,
                         COLOR_FACE, 6u);
    gfx_round_fill_alpha((s16)(SCREEN_WIDTH / 2 + 22), (s16)PAGE_DOTS_Y, 6, 6,
                         3, COLOR_FACE, 6u);

    /* The translucent dock, sized from the icons it holds. */
    gfx_round_fill_alpha((s16)DOCK_X, (s16)DOCK_Y, (s16)DOCK_WIDTH,
                         (s16)DOCK_HEIGHT, 24, COLOR_FACE, 4u);
    for (index = 0u; index < DOCK_SLOTS; ++index) {
        const u16 app = dock_apps[index];
        const s16 x = (s16)(DOCK_X + DOCK_PADDING +
                            index * (DOCK_ICON_SIZE + DOCK_GAP));

        app_icon(x, (s16)(DOCK_Y + DOCK_PADDING), application_list[app],
                 (s16)DOCK_ICON_SIZE, 2);
        gui_hotspot((s16)(x - 6), (s16)(DOCK_Y + 6),
                    (s16)(DOCK_ICON_SIZE + 12), (s16)(DOCK_HEIGHT - 12),
                    (u16)(HOTSPOT_DOCK_BASE + index));
    }

    draw_status_bar(1u);
    draw_home_indicator(1u);
}

/* ------------------------------------------------------------------ */
/* Lock screen                                                         */
/* ------------------------------------------------------------------ */

static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *const weekday_names[7] = {
    "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday"
};

static const char *weekday_for(u16 year, u8 month, u8 day) {
    u32 y = year;
    u32 m = month;

    if (m < 3u) {
        m += 12u;
        --y;
    }
    {
        const u32 k = y % 100u;
        const u32 j = y / 100u;
        const u32 h = (day + (13u * (m + 1u)) / 5u + k + k / 4u + j / 4u +
                       5u * j) % 7u;

        return weekday_names[h];
    }
}

static void draw_date_line(char *out, u16 capacity) {
    const char *weekday = weekday_for(clock_year(), clock_month(), clock_day());
    u8 month = clock_month();

    out[0] = '\0';
    text_append(out, weekday, capacity);
    text_append(out, ", ", capacity);
    text_append_number(out, clock_day(), capacity);
    text_append(out, " ", capacity);
    text_append(out, month_names[(month >= 1u && month <= 12u) ? month - 1u
                                                                  : 0u],
                capacity);
}

static void draw_lock_icon(s16 center_x, s16 top) {
    gfx_round_outline((s16)(center_x - 6), (s16)top, 12, 12, 6, COLOR_FACE);
    gfx_round_fill((s16)(center_x - 9), (s16)(top + 8), 18, 14, 4, COLOR_FACE);
    gfx_circle((s16)center_x, (s16)(top + 15), 2, COLOR_DEEP, 1u);
    gfx_fill((s16)center_x, (s16)(top + 15), 1, 3, COLOR_DEEP);
}

static void draw_lock_screen(void) {
    char time_line[8];
    char date_line[48];
    u16 row;

    draw_wallpaper();
    /* Dim the wallpaper so the white clock always has contrast. */
    for (row = 0u; row < SCREEN_HEIGHT; ++row) {
        s16 column;

        for (column = 0; column < (s16)SCREEN_WIDTH; ++column) {
            gfx_pixel_blend(column, (s16)row, COLOR_WIN_BG, 3u);
        }
    }

    draw_status_bar(1u);
    draw_lock_icon((s16)(SCREEN_WIDTH / 2), 74);

    /* The clock sits where a phone puts it: high on the screen, with the
     * date right underneath, and the hint down by the home indicator. */
    clock_text(time_line, (u16)sizeof(time_line), 0u);
    gfx_text_size_centered(0, 120, (s16)SCREEN_WIDTH, time_line, COLOR_FACE, 76);
    draw_date_line(date_line, (u16)sizeof(date_line));
    gfx_text_size_centered(0, 214, (s16)SCREEN_WIDTH, date_line, COLOR_FACE, 15);

    gfx_text_size_centered(0, (s16)(SCREEN_HEIGHT - 96), (s16)SCREEN_WIDTH,
                           "Tap or swipe up to open", COLOR_FACE, 11);
    gfx_text_size_centered(0, (s16)(SCREEN_HEIGHT - 76), (s16)SCREEN_WIDTH,
                           "DimOS 14 - a small phone in your PC", COLOR_FACE,
                           9);
    draw_home_indicator(1u);
}

/* ------------------------------------------------------------------ */
/* Control Centre                                                      */
/* ------------------------------------------------------------------ */

static const char *const toggle_names[4] = {
    "Airplane", "Wi-Fi", "Bluetooth", "Focus"
};

static const char *const toggle_arts[4][8] = {
    {   /* airplane */
        "....XX....", "....XX....", "..XXXXXX..", "XXXXXXXXXX",
        "XXXXXXXXXX", "..XXXXXX..", "....XX....", "....XX...."
    },
    {   /* Wi-Fi: a rising signal */
        "..........", "......XX..", "....XXXX..", "..XXXXXX..",
        "XXXXXXXX..", "..XXXXXX..", "....XXXX..", "......XX.."
    },
    {   /* Bluetooth rune */
        "...XX.....", "....XX....", ".X...XX...", "..X...XX..",
        "..X...XX..", ".X...XX...", "....XX....", "...XX....."
    },
    {   /* Focus: a moon */
        "...XXX....", "..XX......", ".XX.......", ".XX.......",
        ".XX.......", ".XX.......", "..XX......", "...XXX...."
    }
};

static void draw_cc_toggle(s16 x, s16 y, u8 index) {
    const u8 on = toggles[index];
    const u8 color = (on != 0u) ? COLOR_SELECTION : COLOR_SHADOW;
    const u8 ink = (on != 0u) ? COLOR_FACE : COLOR_DEEP;

    gfx_circle(x, y, 22, color, 1u);
    gfx_glyph((s16)(x - 10), (s16)(y - 10), toggle_arts[index], 8u, 2, ink);
    gfx_text_size_centered((s16)(x - 40), (s16)(y + 26), 80,
                           toggle_names[index], COLOR_DEEP, 8);
    gui_hotspot((s16)(x - 26), (s16)(y - 26), 52, 70,
                (u16)(HOTSPOT_CC_TOGGLE_BASE + index));
}

static void draw_slider(s16 x, s16 y, s16 width, u8 level, const char *name,
                        u16 id) {
    const s16 capsule_y = (s16)(y + 34);
    const s16 knob_x = (s16)(x + 8 +
        (s16)(((s32)level * (width - 16)) / 16));

    gfx_text_size(x, y, name, COLOR_DEEP, 9);
    gfx_round_fill(x, capsule_y, width, 14, 7, COLOR_SHADOW);
    gfx_round_fill(x, capsule_y, (s16)(knob_x - x), 14, 7, COLOR_FACE);
    gfx_circle(knob_x, (s16)(capsule_y + 7), 10, COLOR_FACE, 1u);
    gfx_circle(knob_x, (s16)(capsule_y + 7), 10, COLOR_SHADOW, 0u);
    gui_hotspot(x, (s16)(y - 4), width, 60, id);
}

static void draw_control_centre(void) {
    const s16 panel_x = CC_PANEL_X;
    const s16 panel_y = CC_PANEL_Y;
    const s16 panel_width = CC_PANEL_WIDTH;
    const s16 panel_height = CC_PANEL_HEIGHT;
    const s16 left = (s16)(panel_x + CC_MARGIN);
    u8 index;

    /* A full-screen hotspot underneath the sheet: a tap anywhere outside
     * the panel closes it without touching the home screen below. */
    gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                HOTSPOT_CC_OUTSIDE);

    gfx_round_fill_alpha(panel_x, panel_y, panel_width, panel_height, 30,
                         COLOR_FACE, 13u);
    gfx_text_size(left, 30, "Control Centre", COLOR_DEEP, 11);
    gfx_text_size_right((s16)(panel_x + panel_width - CC_MARGIN), 33,
                        "DimOS 14", COLOR_DISABLED, 9);

    /* Connectivity: four round switches in a 2x2 block, the shape a phone
     * shows them in. */
    for (index = 0u; index < 4u; ++index) {
        draw_cc_toggle((s16)(left + 48 + (s16)(index % 2u) * 96),
                       (s16)(96 + (s16)(index / 2u) * 86), index);
    }

    /* Brightness and volume, one above the other on a narrow screen. */
    draw_slider(left, 212, CC_INNER_WIDTH, brightness, "Brightness",
                HOTSPOT_CC_BRIGHTNESS);
    draw_slider(left, 282, CC_INNER_WIDTH, volume, "Volume",
                HOTSPOT_CC_VOLUME);

    /* Now playing. */
    gfx_text_size(left, 346, "Now playing", COLOR_DEEP, 9);
    gfx_text_size(left, 362, "Chime", COLOR_DEEP, 13);
    gfx_text_size(left, 382,
                  (sound_is_playing() != 0u) ? "Touch to stop" : "Touch to play",
                  COLOR_DISABLED, 9);
    gfx_circle((s16)(left + 12), 418, 16, COLOR_SELECTION, 1u);
    if (sound_is_playing() != 0u) {
        gfx_fill((s16)(left + 6), 412, 12, 12, COLOR_FACE);
    } else {
        u8 step;

        for (step = 0u; step < 12u; ++step) {
            const u8 half = (step < 6u) ? step : (u8)(11u - step);

            gfx_vertical_line((s16)(left + 8 + step),
                              (s16)(412 + 6 - half), (s16)(half * 2 + 1),
                              COLOR_FACE);
        }
    }
    gui_hotspot((s16)(left - 12), 396, 180, 44, HOTSPOT_CC_MUSIC);

    /* Appearance. */
    gfx_text_size(left, 452, "Appearance", COLOR_DEEP, 9);
    gui_button_colored(left, 468, 156, 28, "Change theme", HOTSPOT_CC_THEME,
                       COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(left + 168), 468, 146, 28, "Lock screen",
                       HOTSPOT_CC_LOCK, COLOR_DEEP, COLOR_FACE);

    gfx_text_size(left, (s16)(panel_height - 12),
                  "Tap the wallpaper to go back", COLOR_DISABLED, 9);
}

/* ------------------------------------------------------------------ */
/* Frame composition                                                   */
/* ------------------------------------------------------------------ */

static void draw_frame(void) {
    gui_hotspots_reset();
    gfx_begin_frame();

    if (locked != 0u) {
        gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                    HOTSPOT_DESKTOP);
        draw_lock_screen();
    } else if (active_application == 0xFFFFu) {
        gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                    HOTSPOT_DESKTOP);
        draw_home_screen();
        gui_hotspot((s16)(SCREEN_WIDTH - 190), 0, 190,
                    (s16)STATUS_BAR_HEIGHT, HOTSPOT_STATUS_RIGHT);
        if (control_open != 0u) {
            draw_control_centre();
        }
    } else {
        const Application *app = application_list[active_application];

        /* Every page may choose its own status bar; start from the light
         * default so a dark app cannot tint the next one. */
        status_light = 0u;
        title_accent = COLOR_SELECTION;
        app->draw();
        draw_status_bar(status_light);
        draw_home_indicator(status_light);
        gui_hotspot((s16)(SCREEN_WIDTH - 190), 0, 190,
                    (s16)STATUS_BAR_HEIGHT, HOTSPOT_STATUS_RIGHT);
        if (control_open != 0u) {
            draw_control_centre();
        }
    }

    gfx_draw_pointer(input_pointer_x(), input_pointer_y());
    (void)frame_dark;
}

/* ------------------------------------------------------------------ */
/* Opening, closing and unlocking                                      */
/* ------------------------------------------------------------------ */

u16 gui_active_application(void) {
    return active_application;
}

u16 gui_application_count(void) {
    return application_count;
}

const Application *gui_application(u16 index) {
    if (index >= application_count) {
        return (const Application *)0;
    }
    return application_list[index];
}

void gui_open(u16 index) {
    if (index >= application_count) {
        return;
    }
    active_application = index;
    pressed_id = HOTSPOT_NONE;
    focused_id = HOTSPOT_NONE;
    arrows_reserved = 0u;
    focus_ring = 0u;
    status_line[0] = '\0';
    control_open = 0u;
    title_accent = COLOR_SELECTION;
    application_list[index]->open();
    sound_beep();
}

void gui_lock(void) {
    gui_go_home();
    locked = 1u;
}

void gui_go_home(void) {
    active_application = 0xFFFFu;
    control_open = 0u;
    pressed_id = HOTSPOT_NONE;
    focused_id = HOTSPOT_NONE;
    arrows_reserved = 0u;
    sound_stop();
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void deliver_to_application(u8 type, u16 id, const Event *event) {
    Event forwarded;

    if (active_application == 0xFFFFu) {
        return;
    }
    forwarded.type = type;
    forwarded.key = id;
    forwarded.buttons = input_pointer_buttons();
    forwarded.x = input_pointer_x();
    forwarded.y = input_pointer_y();
    if (event != (const Event *)0) {
        forwarded.x = event->x;
        forwarded.y = event->y;
    }
    application_list[active_application]->event(&forwarded);
}

u8 gui_brightness(void) {
    return brightness;
}

void gui_set_brightness(u8 level) {
    brightness = (u8)(level > 16u ? 16u : level);
}

u8 gui_volume(void) {
    return volume;
}

void gui_set_volume(u8 level) {
    volume = (u8)(level > 16u ? 16u : level);
}

static void set_level(u16 id, s16 x, s16 origin, s16 width) {
    s32 level = ((s32)(x - 8 - origin) * 16) / (width - 16);
    u8 changed;

    if (level < 0) {
        level = 0;
    }
    if (level > 16) {
        level = 16;
    }
    if (id == HOTSPOT_CC_BRIGHTNESS) {
        changed = (u8)(brightness != (u8)level);
        brightness = (u8)level;
    } else {
        changed = (u8)(volume != (u8)level);
        volume = (u8)level;
    }
    /* Persist once the slider settles: the record is 16 bytes, but writing
     * it on every pixel of a drag would be wasteful on a real disk. */
    if (changed != 0u) {
        settings_save();
    }
}

static void activate(u16 id, const Event *event) {
    if (locked != 0u) {
        locked = 0u;
        sound_beep();
        return;
    }
    if (id == HOTSPOT_CC_OUTSIDE) {
        control_open = 0u;
        return;
    }
    if (control_open != 0u) {
        if (id >= HOTSPOT_CC_TOGGLE_BASE &&
            id < (u16)(HOTSPOT_CC_TOGGLE_BASE + 4u)) {
            const u8 index = (u8)(id - HOTSPOT_CC_TOGGLE_BASE);

            toggles[index] = (u8)(toggles[index] == 0u);
            sound_beep();
            return;
        }
        if (id == HOTSPOT_CC_BRIGHTNESS || id == HOTSPOT_CC_VOLUME) {
            if (event != (const Event *)0) {
                set_level(id, event->x, (s16)(CC_PANEL_X + 266), 250);
            }
            return;
        }
        if (id == HOTSPOT_CC_THEME) {
            gfx_select_theme((u8)((gfx_current_theme() + 1u) % THEME_COUNT));
            settings_save();
            return;
        }
        if (id == HOTSPOT_CC_LOCK) {
            control_open = 0u;
            gui_go_home();
            locked = 1u;
            sound_alert();
            return;
        }
        if (id == HOTSPOT_CC_MUSIC) {
            if (sound_is_playing() != 0u) {
                sound_stop();
            } else {
                sound_play_waltz();
            }
            return;
        }
        /* A tap on the dimmed wallpaper closes Control Centre. */
        if (id == HOTSPOT_DESKTOP || id == HOTSPOT_HOME_BAR) {
            control_open = 0u;
        }
        return;
    }
    if (id == HOTSPOT_STATUS_RIGHT) {
        control_open = (u8)(control_open == 0u);
        return;
    }
    if (id == HOTSPOT_BACK || id == HOTSPOT_HOME_BAR) {
        gui_go_home();
        return;
    }
    if (id >= HOTSPOT_ICON_BASE &&
        id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
        gui_open((u16)(id - HOTSPOT_ICON_BASE));
        return;
    }
    if (id >= HOTSPOT_DOCK_BASE && id < (u16)(HOTSPOT_DOCK_BASE + DOCK_SLOTS)) {
        gui_open(dock_apps[id - HOTSPOT_DOCK_BASE]);
        return;
    }
    if (active_application != 0xFFFFu) {
        deliver_to_application(EVENT_CLICK, id, event);
    }
}

static void handle_event(const Event *event) {
    if (locked != 0u) {
        if (event->type == EVENT_CLICK || event->type == EVENT_DRAG ||
            event->type == EVENT_RIGHT_CLICK ||
            (event->type == EVENT_KEY &&
             (event->key == KEY_ENTER || event->key == KEY_ESCAPE ||
              event->key == (u16)' '))) {
            locked = 0u;
            sound_beep();
        }
        return;
    }

    switch (event->type) {
        case EVENT_KEY:
            if (event->key == KEY_ESCAPE) {
                if (control_open != 0u) {
                    control_open = 0u;
                } else if (active_application != 0xFFFFu) {
                    gui_go_home();
                }
                return;
            }
            if (event->key == KEY_TAB) {
                gui_move_focus(1, 0);
                return;
            }
            if (arrows_reserved == 0u) {
                if (event->key == KEY_ARROW_LEFT) {
                    gui_move_focus(-1, 0);
                    return;
                }
                if (event->key == KEY_ARROW_RIGHT) {
                    gui_move_focus(1, 0);
                    return;
                }
                if (event->key == KEY_ARROW_UP) {
                    gui_move_focus(0, -1);
                    return;
                }
                if (event->key == KEY_ARROW_DOWN) {
                    gui_move_focus(0, 1);
                    return;
                }
            }
            if (event->key == KEY_ENTER && focused_id != HOTSPOT_NONE) {
                activate(focused_id, (const Event *)0);
                return;
            }
            if (active_application != 0xFFFFu && control_open == 0u) {
                application_list[active_application]->event(event);
            }
            return;

        case EVENT_PRESS:
            pressed_id = gui_hotspot_at(event->x, event->y);
            if (pressed_id != HOTSPOT_NONE) {
                focused_id = pressed_id;
            }
            focus_ring = 0u;
            if (active_application == 0xFFFFu &&
                pressed_id >= HOTSPOT_ICON_BASE &&
                pressed_id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
                selected_icon = (u16)(pressed_id - HOTSPOT_ICON_BASE);
            }
            if (active_application != 0xFFFFu && control_open == 0u) {
                deliver_to_application(EVENT_PRESS, pressed_id, event);
            }
            return;

        case EVENT_CLICK:
            pressed_id = HOTSPOT_NONE;
            activate(gui_hotspot_at(event->x, event->y), event);
            return;

        case EVENT_RIGHT_CLICK: {
            const u16 id = gui_hotspot_at(event->x, event->y);

            if (active_application == 0xFFFFu &&
                (id == HOTSPOT_DESKTOP || id == HOTSPOT_NONE)) {
                control_open = (u8)(control_open == 0u);
                return;
            }
            if (control_open == 0u) {
                activate(id, event);
            }
            return;
        }

        case EVENT_DRAG:
        case EVENT_MOVE:
            if (active_application != 0xFFFFu && control_open == 0u) {
                deliver_to_application(event->type, HOTSPOT_NONE, event);
            }
            return;

        case EVENT_WHEEL:
            if (active_application != 0xFFFFu && control_open == 0u) {
                deliver_to_application(EVENT_WHEEL, HOTSPOT_NONE, event);
            }
            return;

        default:
            return;
    }
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static void wait_for_next_frame(u64 *deadline) {
    u64 now;

    timer_update();
    now = timer_counter();
    if (now >= *deadline) {
        /* Rendering missed its slot. Start a fresh interval instead of trying
         * to catch up with a burst that could exceed the 60 FPS ceiling. */
        *deadline = now + FRAME_TIMER_COUNTS;
        return;
    }
#ifndef DIMOS_SANDBOX
    /* The assembly helper checks the deadline with interrupts disabled before
     * halting, avoiding the check-then-sleep race that could skip a full frame. */
    input_poll();
    timer_update();
    cpu_wait_until(*deadline);
#else
    while (timer_counter() < *deadline) {
        input_poll();
        timer_update();
#ifdef DIMOS_HOST
        /* The hosted build sleeps here and answers its web clients, so an
         * idle phone does not spin a CPU core at 100 %. */
        host_wait_idle();
#endif
    }
#endif
    *deadline += FRAME_TIMER_COUNTS;
}

void gui_run(void) {
    Event event;
    u64 next_frame;
    u8 redraw = 1u;
    u8 pointer_only = 0u;
    u8 shown_minute;

    /* Fit the phone canvas into any physical display: a real panel, a PC
     * monitor, or a virtual machine's framebuffer. */
    gfx_set_output_resolution(video_width, video_height);
    selected_icon = 0u;
    active_application = 0xFFFFu;
    locked = 1u;
    control_open = 0u;
    toggles[0] = 0u;
    toggles[1] = 1u;
    toggles[2] = 1u;
    toggles[3] = 0u;
    sound_play_startup();
    timer_update();
    next_frame = timer_counter() + FRAME_TIMER_COUNTS;
    shown_minute = clock_minutes();

    for (;;) {
        u16 ticks;

        timer_update();
        ticks = timer_take_ticks();

        input_poll();
        input_advance(ticks);

        /* A hover has no visual state in this shell. Coalesce all unpressed
         * mouse packets into one tiny cursor update, rather than rebuilding
         * the wallpaper, the icons and every label for each packet. */
        pointer_only = 0u;
        while (input_next_event(&event) != 0u) {
            const u8 is_plain_move = (u8)(event.type == EVENT_MOVE &&
                                          event.buttons == 0u);

            handle_event(&event);
            if (is_plain_move != 0u) {
                pointer_only = 1u;
            } else {
                redraw = 1u;
                pointer_only = 0u;
            }
        }

        if (active_application != 0xFFFFu && locked == 0u) {
            const Application *app = application_list[active_application];

            if (app->update != 0) {
                app->update((u16)(ticks * TICK_MILLISECONDS));
                redraw = 1u;
            }
        }
        sound_update(ticks);

        /* The only changing element while idle is the clock. */
        if (clock_minutes() != shown_minute) {
            shown_minute = clock_minutes();
            redraw = 1u;
        }

        if (redraw != 0u) {
            draw_frame();
            gfx_show();
#ifdef DIMOS_HOST
            host_present_frame();
#endif
            redraw = 0u;
        } else if (pointer_only != 0u) {
            gfx_draw_pointer(input_pointer_x(), input_pointer_y());
            gfx_show();
#ifdef DIMOS_HOST
            host_present_frame();
#endif
        }

        wait_for_next_frame(&next_frame);
    }
}
