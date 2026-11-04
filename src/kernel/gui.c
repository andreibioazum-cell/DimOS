/*
 * gui.c -- the DimOS phone shell.
 *
 * DimOS presents itself as a pocket computer with no clutter: a lock
 * screen with a big clock, a plain gradient desktop and one grid of app
 * icons with their names underneath. The top strip holds only the clock,
 * a small Google search button and the battery percentage; a black strip
 * along the bottom holds the three navigation buttons (triangle, circle,
 * square) and the square shows recent apps. Applications open edge to
 * edge with no window frame; a hidden Control Centre keeps the settings.
 * Every control is a touch-sized hotspot, so the same build works on a
 * real touch panel, in a virtual machine and under a PS/2 mouse on a PC.
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

/* The frame interval comes from the selected phone panel: 60 Hz by default,
 * 120 Hz on a ProMotion-class display. timer_frame_counts() is the PIT
 * divisor for that refresh rate, so the compositor and IRQ0 always agree. */

/* Ids the shell keeps for its own controls. Applications use small ids. */
#define HOTSPOT_ICON_BASE 0x8000u
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
#define HOTSPOT_CC_PANEL 0x8225u
#define HOTSPOT_SEARCH 0x8230u
#define HOTSPOT_SEARCH_GO 0x8231u
#define HOTSPOT_NAV_RECENTS 0x8232u
#define HOTSPOT_OVERLAY_OUTSIDE 0x8233u
#define HOTSPOT_RECENT_BASE 0x8240u /* 3 recent-app cards */

/* Home screen geometry from dimos.h: five columns, two rows, nothing else. */
#define ICON_LABEL_HEIGHT 10u

/* Control Centre panel. */
#define CC_PANEL_X 26
#define CC_PANEL_Y (-24)
#define CC_PANEL_WIDTH ((s16)(SCREEN_WIDTH - 2 * 26))
#define CC_PANEL_HEIGHT 262

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

static Hotspot hotspots[HOTSPOT_LIMIT];
static u16 hotspot_count;
static u16 focused_id = HOTSPOT_NONE;
/* A ring around the focused button belongs to keyboard navigation only: a
 * finger tap moves the focus too, but it should not leave a halo behind. */
static u8 focus_ring;
static u16 pressed_id = HOTSPOT_NONE;
static u16 active_application = 0xFFFFu;
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

/* Shell state: a search sheet, an overview sheet and the three apps
 * that were opened most recently. */
static u8 search_open;
static u8 search_submitted;
static char search_query[32];
static u8 recents_open;
static u16 recent_apps[3];
static u8 recent_count;

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

/* The app caption lives in the strip just under the status bar: the old
 * bottom position would collide with the fullscreen keyboards and with the
 * black navigation strip. */
void gui_message_bar(const char *text) {
    gfx_text_size((s16)APP_LEFT, 24, text,
                  (status_light != 0u) ? COLOR_WIN_LIGHT : COLOR_DISABLED,
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
static void draw_navigation_bar(void);
static void app_icon(s16 x, s16 y, const Application *app, s16 size,
                     u8 glyph_scale) {
    gfx_round_fill(x, y, size, size, (s16)(size / 4), app->color);
    gfx_glyph((s16)(x + (size - (s16)(16 * glyph_scale)) / 2),
              (s16)(y + (size - (s16)(16 * glyph_scale)) / 2), app->icon, 16u,
              glyph_scale, COLOR_FACE);
}

/* Applications are full-screen, not windows: no title bar, no fake back
 * arrow, no card edges. Only a small caption in the status strip names the
 * running app; Back lives in the triangle button below. */
static void app_page(const char *title, u8 background, u8 caption_color) {
    gfx_fill(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT, background);
    gfx_text_size((s16)(APP_LEFT + 110), 6, title, caption_color, 9);
    (void)title_accent;
}

void gui_window_frame(const char *title) {
    frame_dark = 0u;
    status_light = 0u;
    title_accent = COLOR_SELECTION;
    app_page(title, COLOR_PANEL, COLOR_DISABLED);
}

void gui_window_frame_dark(const char *title, u8 background) {
    frame_dark = 1u;
    status_light = 1u;
    title_accent = COLOR_WIN_LIGHT;
    app_page(title, background, COLOR_WIN_LIGHT);
}

/* ------------------------------------------------------------------ */
/* Wallpaper                                                           */
/* ------------------------------------------------------------------ */

/* The desktop is just a vertical gradient: no blobs, no gloss, no noise.
 * Sixteen palette bands are enough to read as a smooth ramp, and drawing
 * them straight into the back buffer keeps the frame cheap. */
#define WALL_TOP_RED 18u
#define WALL_TOP_GREEN 24u
#define WALL_TOP_BLUE 68u
#define WALL_BOTTOM_RED 186u
#define WALL_BOTTOM_GREEN 96u
#define WALL_BOTTOM_BLUE 132u

static void draw_wallpaper(void) {
    const u8 top = gfx_color_from_rgb(WALL_TOP_RED, WALL_TOP_GREEN,
                                      WALL_TOP_BLUE);
    const u8 bottom = gfx_color_from_rgb(WALL_BOTTOM_RED,
                                         WALL_BOTTOM_GREEN,
                                         WALL_BOTTOM_BLUE);
    u8 bands[FONT_ALPHA_MAX];
    u8 band;
    u16 row;

    /* gfx_shade(foreground=bottom, background=top, alpha): alpha 0 is the
     * top colour, alpha 15 is one step short of the bottom one. */
    for (band = 0u; band < FONT_ALPHA_MAX; ++band) {
        bands[band] = gfx_shade(bottom, top, band);
    }
    for (row = 0u; row < SCREEN_HEIGHT; ++row) {
        const u8 color = bands[((u32)row * (FONT_ALPHA_MAX - 1u)) /
                               (u32)(SCREEN_HEIGHT - 1u)];
        u16 column;

        for (column = 0u; column < SCREEN_WIDTH; ++column) {
            screen[(u32)row * SCREEN_WIDTH + column] = color;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Status bar and navigation                                          */
/* ------------------------------------------------------------------ */

/* The right edge of the status strip stays clear of the rotated inset,
 * exactly like the safe area of a real phone. */
static s16 status_edge(void) {
    return (s16)(SCREEN_WIDTH - display_safe_right());
}

/* A believable charge indicator: it starts full and loses one percent per
 * simulated minute. DimOS has no battery controller, but the status strip
 * should still read like a phone's. */
static u8 charge_level(void) {
    return (u8)(100u - (u8)((time_milliseconds() / 60000u) % 41u));
}

static void draw_battery(u8 light) {
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;
    const s16 x = (s16)(status_edge() - 50);
    const s16 y = 6;
    const u8 level = charge_level();

    gfx_round_outline(x, y, 24, 12, 4, color);
    gfx_fill((s16)(x + 25), (s16)(y + 4), 2, 4, color);
    gfx_round_fill((s16)(x + 2), (s16)(y + 2),
                   (s16)(1u + (u16)level * 17u / 100u), 8, 2, color);
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

/* Just the time and the charge: no signal bars, no "5G", no Wi-Fi arcs. */
static void draw_status_bar(u8 light) {
    char clock_line[8];
    char charge_line[6];
    const u8 color = (light != 0u) ? COLOR_FACE : COLOR_DEEP;

    charge_line[0] = '\0';
    text_append_number(charge_line, charge_level(), (u16)sizeof(charge_line));
    text_append_character(charge_line, '%', (u16)sizeof(charge_line));
    gfx_text_size_right((s16)(status_edge() - 56), 6, charge_line, color, 9);
    draw_battery(light);

    clock_text(clock_line, (u16)sizeof(clock_line), 1u);
    /* The clock starts after the rotated notch inset on the left. */
    gfx_text_size((s16)(24 + display_safe_left() / 2u), 5, clock_line,
                  color, 11);
}

/* Three-button navigation: triangle = Back, circle = Home, square =
 * Overview. The black strip is drawn on every page, including over apps. */
static void draw_navigation_bar(void) {
    const s16 y = (s16)(NAV_BAR_Y + 16);
    const s16 centers[3] = {
        (s16)(SCREEN_WIDTH / 4u), (s16)(SCREEN_WIDTH / 2u),
        (s16)(3u * SCREEN_WIDTH / 4u)
    };

    gfx_fill(0, (s16)NAV_BAR_Y, (s16)SCREEN_WIDTH, 32, COLOR_BLACK);
    /* Back: a left-pointing triangle. */
    gfx_line((s16)(centers[0] - 7), y, (s16)(centers[0] + 7), (s16)(y - 8),
             COLOR_FACE);
    gfx_line((s16)(centers[0] - 7), y, (s16)(centers[0] + 7), (s16)(y + 8),
             COLOR_FACE);
    gfx_line((s16)(centers[0] + 7), (s16)(y - 8), (s16)(centers[0] + 7),
             (s16)(y + 8), COLOR_FACE);
    /* Home: a circle. */
    gfx_circle(centers[1], y, 7, COLOR_FACE, 0u);
    /* Overview: a square. */
    gfx_round_outline((s16)(centers[2] - 7), (s16)(y - 7), 14, 14, 2,
                      COLOR_FACE);

    gui_hotspot((s16)(centers[0] - 60), (s16)NAV_BAR_Y, 120, 32,
                HOTSPOT_BACK);
    gui_hotspot((s16)(centers[1] - 60), (s16)NAV_BAR_Y, 120, 32,
                HOTSPOT_HOME_BAR);
    gui_hotspot((s16)(centers[2] - 60), (s16)NAV_BAR_Y, 120, 32,
                HOTSPOT_NAV_RECENTS);
}

/* A single word in the status strip is the Google search button. No pill,
 * no border, no icon: the desktop stays just a gradient and app icons. It
 * sits left of centre, clear of the notch/island drawn at the top. */
static void draw_search_link(void) {
    const s16 x = 140;

    gfx_text_size_centered(x, 6, 88, "Google", COLOR_FACE, 10);
    gui_hotspot(x, 0, 88, (s16)STATUS_BAR_HEIGHT, HOTSPOT_SEARCH);
}

/* ------------------------------------------------------------------ */
/* Search and overview sheets                                          */
/* ------------------------------------------------------------------ */

static void draw_overlay_backdrop(void) {
    gfx_gradient_vertical(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                          COLOR_DEEP, COLOR_BLACK);
}

/* A small search sheet: typing works with a hardware keyboard, but DimOS
 * has no network stack, so the query only ever reaches this page. */
static void draw_search_overlay(void) {
    gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                HOTSPOT_OVERLAY_OUTSIDE);
    draw_overlay_backdrop();
    gfx_round_fill(90, 74, 664, 232, 22, COLOR_FACE);
    gfx_text_size(120, 92, "Google", COLOR_DEEP, 22);
    gfx_round_fill(120, 136, 604, 42, 21, COLOR_PANEL);
    gfx_text_size(140, 147,
                  (search_query[0] != '\0') ? search_query : "Search the web",
                  (search_query[0] != '\0') ? COLOR_DEEP : COLOR_DISABLED, 14);
    gfx_text_size(120, 190,
                  (search_submitted != 0u)
                      ? "No results - DimOS has no network connection"
                      : "Type a query and press Enter",
                  COLOR_DISABLED, 10);
    gui_button_colored(120, 226, 180, 44, "Search", HOTSPOT_SEARCH_GO,
                       COLOR_SELECTION, COLOR_FACE);
}

/* Overview: the last three applications, as tappable cards. */
static void draw_recents_overlay(void) {
    u8 index;

    gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                HOTSPOT_OVERLAY_OUTSIDE);
    draw_overlay_backdrop();
    gfx_text_size_centered(0, 76, (s16)SCREEN_WIDTH, "Recent apps",
                           COLOR_FACE, 18);
    if (recent_count == 0u) {
        gfx_text_size_centered(0, 180, (s16)SCREEN_WIDTH,
                               "No apps opened yet", COLOR_FACE, 13);
    }
    for (index = 0u; index < recent_count; ++index) {
        const Application *app = application_list[recent_apps[index]];
        const s16 x = (s16)(SCREEN_WIDTH / 2 - 245 + (s16)index * 170);

        gfx_round_fill(x, 116, 150, 176, 18, COLOR_FACE);
        app_icon((s16)(x + 43), 142, app, 64, 3);
        gfx_text_size_centered(x, 232, 150, app->short_label, COLOR_DEEP, 11);
        gui_hotspot(x, 116, 150, 176,
                    (u16)(HOTSPOT_RECENT_BASE + index));
    }
}

static void home_icon(s16 column, s16 row, u16 index) {
    const Application *app = application_list[index];
    const s16 x = (s16)(ICON_GRID_LEFT + column * (s16)ICON_COLUMN_STEP);
    const s16 y = (s16)(ICON_GRID_TOP + row * (s16)ICON_ROW_STEP);
    const u16 id = (u16)(HOTSPOT_ICON_BASE + index);

    app_icon(x, y, app, (s16)ICON_SIZE, 3);
    /* Plain text below the icon, with no shadow and no highlight. */
    gfx_text_size_centered((s16)(x - 24), (s16)(y + ICON_SIZE + 7),
                           (s16)(ICON_SIZE + 48), app->short_label,
                           COLOR_FACE, ICON_LABEL_HEIGHT);
    gui_hotspot((s16)(x - 8), (s16)(y - 4), (s16)(ICON_SIZE + 16),
                (s16)(ICON_SIZE + 26), id);
}

static void draw_home_screen(void) {
    u8 index;

    draw_wallpaper();
    for (index = 0u; index < application_count && index < 10u; ++index) {
        home_icon((s16)(index % ICON_COLUMNS), (s16)(index / ICON_COLUMNS),
                  index);
    }
    draw_status_bar(1u);
    draw_search_link();
    draw_navigation_bar();
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

    clock_text(time_line, (u16)sizeof(time_line), 0u);
    gfx_text_size_centered(0, 86, (s16)SCREEN_WIDTH, time_line, COLOR_FACE, 76);
    draw_date_line(date_line, (u16)sizeof(date_line));
    gfx_text_size_centered(0, 176, (s16)SCREEN_WIDTH, date_line, COLOR_FACE, 14);

    gfx_text_size_centered(0, 292, (s16)SCREEN_WIDTH, "Tap to open",
                           COLOR_FACE, 11);
    draw_navigation_bar();
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
    s16 panel_x = CC_PANEL_X;
    s16 panel_y = CC_PANEL_Y;
    s16 panel_width = CC_PANEL_WIDTH;
    s16 panel_height = CC_PANEL_HEIGHT;
    u8 index;

    /* A full-screen hotspot underneath the sheet: a tap anywhere outside
     * the panel closes it without touching the home screen below. */
    gui_hotspot(0, 0, (s16)SCREEN_WIDTH, (s16)SCREEN_HEIGHT,
                HOTSPOT_CC_OUTSIDE);

    gfx_round_fill_alpha(panel_x, panel_y, panel_width, panel_height, 30,
                         COLOR_FACE, 13u);
    gfx_text_size((s16)(panel_x + 28), 14, "Control Centre", COLOR_DEEP, 11);
    gfx_text_size_right((s16)(panel_x + panel_width - 28), 17,
                        "DimOS 14", COLOR_DISABLED, 9);

    /* Connectivity: four round switches in a 2x2 block. */
    for (index = 0u; index < 4u; ++index) {
        draw_cc_toggle((s16)(panel_x + 78 + (s16)(index % 2u) * 86),
                       (s16)(78 + (s16)(index / 2u) * 82), index);
    }

    /* Brightness and volume. */
    draw_slider((s16)(panel_x + 266), 52, 250, brightness, "Brightness",
                HOTSPOT_CC_BRIGHTNESS);
    draw_slider((s16)(panel_x + 266), 126, 250, volume, "Volume",
                HOTSPOT_CC_VOLUME);

    /* Now playing, lock and appearance. */
    gfx_text_size((s16)(panel_x + 566), 52, "Now playing", COLOR_DEEP, 9);
    gfx_text_size((s16)(panel_x + 566), 68, "Chime", COLOR_DEEP, 13);
    gfx_text_size((s16)(panel_x + 566), 88,
                  (sound_is_playing() != 0u) ? "Touch to stop" : "Touch to play",
                  COLOR_DISABLED, 9);
    gfx_circle((s16)(panel_x + 576), 118, 16, COLOR_SELECTION, 1u);
    if (sound_is_playing() != 0u) {
        gfx_fill((s16)(panel_x + 570), 112, 12, 12, COLOR_FACE);
    } else {
        u8 step;

        for (step = 0u; step < 12u; ++step) {
            const u8 half = (step < 6u) ? step : (u8)(11u - step);

            gfx_vertical_line((s16)(panel_x + 572 + step),
                              (s16)(112 + 6 - half), (s16)(half * 2 + 1),
                              COLOR_FACE);
        }
    }
    gui_hotspot((s16)(panel_x + 560), 100, 160, 40, HOTSPOT_CC_MUSIC);

    gfx_text_size((s16)(panel_x + 566), 146, "Appearance", COLOR_DEEP, 9);
    gui_button_colored((s16)(panel_x + 566), 162, 150, 26, "Change theme",
                       HOTSPOT_CC_THEME, COLOR_PANEL, COLOR_DEEP);
    gui_button_colored((s16)(panel_x + 566), 196, 150, 26, "Lock screen",
                       HOTSPOT_CC_LOCK, COLOR_DEEP, COLOR_FACE);

    /* The physical mode and how the phone canvas fits into it. */
    display_output_line(status_line, (u16)sizeof(status_line));
    gfx_text_size((s16)(panel_x + 28), 210, status_line, COLOR_DISABLED, 9);
    gfx_text_size((s16)(panel_x + 28), 226, "Tap the wallpaper to go back",
                  COLOR_DISABLED, 9);
    gui_button_colored((s16)(panel_x + 300), 204, 150, 26, "Change panel",
                       HOTSPOT_CC_PANEL, COLOR_PANEL, COLOR_DEEP);
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
        draw_navigation_bar();
        gui_hotspot((s16)(SCREEN_WIDTH - 190), 0, 190,
                    (s16)STATUS_BAR_HEIGHT, HOTSPOT_STATUS_RIGHT);
        if (control_open != 0u) {
            draw_control_centre();
        }
    }

    /* Sheets sit above everything; the navigation keys remain on top so
     * the circle always gets home and the square closes the overview. */
    if (locked == 0u && (search_open != 0u || recents_open != 0u)) {
        if (search_open != 0u) {
            draw_search_overlay();
        }
        if (recents_open != 0u) {
            draw_recents_overlay();
        }
        draw_navigation_bar();
    }

    /* Rounded corners and the camera cutout belong to the panel, not to
     * any single page, so they are composited over the finished frame. */
    display_draw_bezel();
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

static void remember_app(u16 index) {
    u8 slot;

    for (slot = 3u; slot > 1u; --slot) {
        recent_apps[slot - 1u] = recent_apps[slot - 2u];
    }
    recent_apps[0] = index;
    if (recent_count < 3u) {
        ++recent_count;
    }
}

void gui_open(u16 index) {
    if (index >= application_count) {
        return;
    }
    remember_app(index);
    search_open = 0u;
    recents_open = 0u;
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
    search_open = 0u;
    recents_open = 0u;
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
    /* Search and overview are modal sheets: only their own controls and
     * the three navigation keys are live while one of them is up. */
    if (search_open != 0u) {
        if (id == HOTSPOT_SEARCH_GO) {
            search_submitted = 1u;
            return;
        }
        if (id == HOTSPOT_NAV_RECENTS || id == HOTSPOT_BACK ||
            id == HOTSPOT_HOME_BAR) {
            search_open = 0u;
            gui_go_home();
            return;
        }
        if (id == HOTSPOT_OVERLAY_OUTSIDE) {
            search_open = 0u;
            return;
        }
        return;
    }
    if (recents_open != 0u) {
        if (id >= HOTSPOT_RECENT_BASE &&
            id < (u16)(HOTSPOT_RECENT_BASE + 3u)) {
            const u8 slot = (u8)(id - HOTSPOT_RECENT_BASE);

            if (slot < recent_count) {
                gui_open(recent_apps[slot]);
            }
            return;
        }
        if (id == HOTSPOT_NAV_RECENTS || id == HOTSPOT_OVERLAY_OUTSIDE ||
            id == HOTSPOT_BACK || id == HOTSPOT_HOME_BAR) {
            recents_open = 0u;
            gui_go_home();
            return;
        }
        return;
    }
    if (id == HOTSPOT_SEARCH) {
        search_open = 1u;
        search_submitted = 0u;
        search_query[0] = '\0';
        return;
    }
    if (id == HOTSPOT_NAV_RECENTS) {
        recents_open = 1u;
        control_open = 0u;
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
        if (id == HOTSPOT_CC_PANEL) {
            /* Switch to the next phone panel and remember it on the disk. */
            display_next_profile();
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
            /* The search sheet takes the keyboard while it is open. */
            if (search_open != 0u) {
                if (event->key == KEY_ESCAPE) {
                    search_open = 0u;
                } else if (event->key == KEY_ENTER) {
                    search_submitted = 1u;
                } else if (event->key == KEY_BACKSPACE) {
                    const u16 length = text_length(search_query);

                    if (length != 0u) {
                        search_query[length - 1u] = '\0';
                    }
                } else if (event->key >= 32u && event->key <= 126u &&
                           text_length(search_query) <
                               (u16)(sizeof(search_query) - 1u)) {
                    search_submitted = 0u;
                    text_append_character(search_query,
                                          (char)event->key,
                                          (u16)sizeof(search_query));
                }
                return;
            }
            if (event->key == KEY_ESCAPE) {
                if (recents_open != 0u) {
                    recents_open = 0u;
                } else if (control_open != 0u) {
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
        *deadline = now + timer_frame_counts();
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
    }
#endif
    *deadline += timer_frame_counts();
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
    active_application = 0xFFFFu;
    locked = 1u;
    control_open = 0u;
    toggles[0] = 0u;
    toggles[1] = 1u;
    toggles[2] = 1u;
    toggles[3] = 0u;
    sound_play_startup();
    timer_update();
    next_frame = timer_counter() + timer_frame_counts();
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
            redraw = 0u;
        } else if (pointer_only != 0u) {
            gfx_draw_pointer(input_pointer_x(), input_pointer_y());
            gfx_show();
        }

        wait_for_next_frame(&next_frame);
    }
}
