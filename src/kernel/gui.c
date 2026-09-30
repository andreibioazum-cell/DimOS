/*
 * gui.c -- the DimXfce desktop shell.
 *
 * This is a love letter to Xfce, squeezed into 320x200 pixels: a dark
 * xfce4-panel across the top with the little mouse logo that opens a
 * Whisker menu, a shining gradient wallpaper the way xfdesktop draws it,
 * windows with xfwm4 style decorations (shade / minimize / close), and a
 * plank like dock of launchers at the bottom. Right click on the desktop
 * opens the menu, and long pressing the desktop button rolls the open
 * window up into its title bar -- two of Xfce's signature gestures.
 *
 * Everything on screen is either a picture or a button, and every button
 * announces where it is while it is being drawn. A click is then simply
 * matched against that list, which keeps the applications free of any
 * event arithmetic: they draw a button, they get told when it is clicked.
 *
 * Nothing in DimOS needs a keyboard. The pointer does everything, the arrow
 * keys and Enter walk the same buttons for people who have a keyboard, and
 * Escape always goes back to the desktop.
 */

#include "dimos.h"

/* 1,193,182 PIT clocks per second / 60, rounded up. Waiting against the raw
 * hardware counter gives a strict maximum of 60 FPS without the old 10 ms
 * clock rounding the compositor down to 50 or 25 FPS. */
#define FRAME_TIMER_COUNTS 19887ull

/* Ids the window manager keeps for its own buttons. Applications use ids
 * below this range. */
#define HOTSPOT_ICON_BASE 0x8000u  /* desktop icons                   */
#define HOTSPOT_DOCK_BASE 0x8100u  /* the dock's launchers            */
#define HOTSPOT_HOME 0x8200u       /* the mouse: opens the menu       */
#define HOTSPOT_CLOSE 0x8201u
#define HOTSPOT_SHADE 0x8202u      /* roll the window up like a blind */
#define HOTSPOT_MINIMIZE 0x8203u
#define HOTSPOT_MENU_BASE 0x8210u
#define HOTSPOT_MENU_ABOUT 0x8230u
#define HOTSPOT_POWER 0x8231u      /* the panel's restart button      */
#define HOTSPOT_SHRINK 0x8232u     /* the dock's show-desktop cell    */
#define HOTSPOT_DESKTOP 0x8233u    /* empty wallpaper                 */
#define HOTSPOT_MENU_BODY 0x8234u  /* the Whisker menu's own pixels   */
#define HOTSPOT_CLASS_BASE 0x8240u /* the Favourites filter buttons   */

/* The wallpaper mouse's coat: soft grey body, warm tail. */
#define GFX_MOUSE_BODY COLOR_LIGHT_GRAY
#define GFX_MOUSE_TAIL COLOR_ACCENT

/* The Xfce mouse: the logo of the whole project, and the face of our
 * little cheese lover. The body is art, the tail is a pair of strokes. */
static const char *const mouse_logo[10] = {
    "...XXX.........",
    "..XXXXX........",
    ".XXoXXXXX......",
    "XXXXXXXXXX.....",
    "XXXXXXXXXXXX...",
    "XXXXXXXXXXXXX..",
    ".XXXXXXXXXXX...",
    "..XXXXXXXXX....",
    "...XXXXX.......",
    "....XX........."
};

/* One logo, two places: the panel button and the menu header. */
static void draw_mouse_logo(s16 x, s16 y) {
    gfx_picture(x, y, mouse_logo, 10u, COLOR_WHITE, GFX_MOUSE_TAIL);
    gfx_line((s16)(x + 11), (s16)(y + 6), (s16)(x + 15), (s16)(y + 3),
             GFX_MOUSE_TAIL);
    gfx_line((s16)(x + 15), (s16)(y + 3), (s16)(x + 18), (s16)(y + 1),
             GFX_MOUSE_TAIL);
}

/* A tiny monitor for the dock's "show desktop" cell. */
static const char *const shrink_icon[8] = {
    "..........",
    "XXXXXXXXXX",
    "X........X",
    "X........X",
    "XXXXXXXXXX",
    "...XXXX...",
    "...X..X...",
    "...XXXX..."
};

/* Desktop icons: two Xfce style columns pinned to the left edge. */
#define ICON_COLUMN_WIDTH 60
#define ICON_ROW_HEIGHT 32
#define ICON_COLUMNS 2
#define ICON_LEFT 6u
#define ICON_TOP (DESKTOP_TOP + 4u)

/* The Whisker menu's Favourites column: all apps, only the games, or
 * everything but the games. */
#define MENU_CLASS_ALL 0u
#define MENU_CLASS_GAMES 1u
#define MENU_CLASS_TOOLS 2u
#define MENU_CLASS_COUNT 3u

#define MENU_CELL_WIDTH 104
#define MENU_CELL_HEIGHT 38
#define MENU_GRID_TOP (MENU_Y + 22)

typedef struct {
    s16 x;
    s16 y;
    s16 width;
    s16 height;
    u16 id;
} Hotspot;

const Application *const application_list[] = {
    &application_files,
    &application_snake,
    &application_mines,
    &application_paint,
    &application_calculator,
    &application_music,
    &application_cheesy,
    &application_about,
    &application_terminal
};

const u16 application_count =
    (u16)(sizeof(application_list) / sizeof(application_list[0]));

static Hotspot hotspots[HOTSPOT_LIMIT];
static u16 hotspot_count;
static u16 focused_id = HOTSPOT_NONE;
static u16 pressed_id = HOTSPOT_NONE;
static u16 active_application = 0xFFFFu;
static u16 selected_icon;
static u8 arrows_reserved;
static char status_line[40];

/* DimXfce state: the Whisker menu, a window rolled up into its title bar,
 * the gradient of the current window's title bar, and a tooltip that pops
 * while the pointer is held on the panel or dock chrome. */
static u8 menu_open;
static u8 menu_class;
static u8 window_rolled;
static u8 title_top_color = COLOR_SELECTION;
static u16 last_application = 0xFFFFu;
static u16 panel_tooltip = HOTSPOT_NONE;
static const char *tooltip_text;
static s16 tooltip_x;
static s16 tooltip_y;

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
 * panel is the one that gets the click. */
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

void gui_focus(u16 id) {
    focused_id = id;
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

    /* Nothing in that direction: wrap around to the first button. */
    if (best_id == HOTSPOT_NONE) {
        best_id = hotspots[0].id;
    }
    focused_id = best_id;
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
    const u8 raised = (pressed_id == id) ? 0u : 1u;
    const s16 text_y = (s16)(y + (height - 8) / 2);

    gfx_fill(x, y, width, height, face);
    gfx_raised_box(x, y, width, height, raised);
    gfx_text_centered((s16)(x + 1), text_y, (s16)(width - 2), label, text);

    if (focused_id == id) {
        gfx_outline((s16)(x + 2), (s16)(y + 2), (s16)(width - 4),
                    (s16)(height - 4), COLOR_BLACK);
    }
    gui_hotspot(x, y, width, height, id);
}

void gui_button(s16 x, s16 y, s16 width, s16 height, const char *label, u16 id) {
    gui_button_colored(x, y, width, height, label, id, COLOR_FACE, COLOR_BLACK);
}

void gui_switch(s16 x, s16 y, s16 width, s16 height, const char *label,
                u16 id, u8 on) {
    const u8 raised = (pressed_id == id) ? 0u : 1u;

    gfx_fill(x, y, width, height, COLOR_FACE);
    gfx_raised_box(x, y, width, height, raised);

    /* A small sunken lamp shows the state. */
    gfx_fill((s16)(x + 3), (s16)(y + (height - 8) / 2), 9, 8,
             (on != 0u) ? COLOR_GOOD : COLOR_TEXT_FIELD);
    gfx_outline((s16)(x + 3), (s16)(y + (height - 8) / 2), 9, 8, COLOR_SHADOW);
    gfx_text((s16)(x + 15), (s16)(y + (height - 8) / 2), label, COLOR_BLACK);

    if (focused_id == id) {
        gfx_outline((s16)(x + 2), (s16)(y + 2), (s16)(width - 4),
                    (s16)(height - 4), COLOR_BLACK);
    }
    gui_hotspot(x, y, width, height, id);
}

void gui_message_bar(const char *text) {
    const s16 y = (s16)(WINDOW_TOP + WINDOW_INNER_HEIGHT - 11);
    char shown[40];
    u16 length = 0u;

    gfx_fill((s16)WINDOW_LEFT, y, (s16)WINDOW_INNER_WIDTH, 11, COLOR_FACE);
    gfx_panel((s16)WINDOW_LEFT, y, (s16)WINDOW_INNER_WIDTH, 11);

    /* Every glyph is 8 pixels wide; stop before the text would leave the
     * bar so long messages are clipped instead of overpainting the frame. */
    while (text[length] != '\0' && length < (u16)(sizeof(shown) - 1) &&
           (u16)((length + 1u) * 8u) <= (u16)(WINDOW_INNER_WIDTH - 8u)) {
        shown[length] = text[length];
        ++length;
    }
    shown[length] = '\0';

    gfx_text((s16)(WINDOW_LEFT + 4), (s16)(y + 2), shown, COLOR_BLACK);
    text_copy(status_line, shown, (u16)sizeof(status_line));
}

/* xfwm4 decorations: a dark gradient title bar, a deep frame, and the
 * signature three buttons on the right -- roll up (shade), minimize and
 * close. One Application may re-tint its title bar for style: the black
 * record window of the Music app does, like a real media player that
 * ships its own theme. */
void gui_window_title_color(u8 top_color) {
    title_top_color = top_color;
}

static void draw_window_chrome(const char *title, u8 interior) {
    const s16 buttons_right = (s16)(WINDOW_X + WINDOW_WIDTH - 4);

    gfx_fill((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH, (s16)WINDOW_HEIGHT,
             (interior != 0u) ? COLOR_FACE : COLOR_DEEP);
    gfx_outline((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH,
                (s16)WINDOW_HEIGHT, COLOR_DEEP);

    /* The title bar itself: xfwm4's gradient, one pixel of shade left and
     * right so the dark frame stays visible. */
    gfx_gradient_vertical((s16)(WINDOW_X + 1), (s16)(WINDOW_Y + 1),
                          (s16)(WINDOW_WIDTH - 2), 10,
                          title_top_color, COLOR_DEEP);
    gfx_text((s16)(WINDOW_X + 5), (s16)(WINDOW_Y + 2), title, COLOR_WHITE);

    /* Roll up, minimize, close -- flat xfwm4 style buttons. */
    gui_button_colored((s16)(buttons_right - 35), (s16)(WINDOW_Y + 2), 11, 8,
                       "^", HOTSPOT_SHADE, (interior != 0u) ? COLOR_FACE : COLOR_PANEL,
                       (interior != 0u) ? COLOR_BLACK : COLOR_WHITE);
    if (interior != 0u) {
        gui_button_colored((s16)(buttons_right - 23), (s16)(WINDOW_Y + 2), 11, 8,
                           "_", HOTSPOT_MINIMIZE, COLOR_FACE, COLOR_BLACK);
        gui_button_colored((s16)(buttons_right - 11), (s16)(WINDOW_Y + 2), 11, 8,
                           "X", HOTSPOT_CLOSE, COLOR_ALERT, COLOR_WHITE);
        gfx_horizontal_line((s16)(WINDOW_X + 1), (s16)(WINDOW_Y + 11),
                            (s16)(WINDOW_WIDTH - 2), COLOR_SHADOW);
    }
}

void gui_window_frame(const char *title) {
    draw_window_chrome(title, 1u);
}

/* ------------------------------------------------------------------ */
/* Desktop, panels and the Whisker menu                                */
/* ------------------------------------------------------------------ */

static void clock_text(char *out, u16 capacity) {
    out[0] = '\0';
    if (clock_hours() < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, clock_hours(), capacity);
    text_append_character(out, ':', capacity);
    if (clock_minutes() < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, clock_minutes(), capacity);
}

/* Labelled text with the soft shadow xfdesktop paints behind icon names,
 * so captions stay readable over any part of the wallpaper. */
static void text_shadowed(s16 x, s16 y, const char *text, u8 color) {
    gfx_text((s16)(x + 1), (s16)(y + 1), text, COLOR_BLACK);
    gfx_text(x, y, text, color);
}

/* The mascot on the empty wallpaper: the big Xfce mouse, assembled from
 * ellipses and a bezier tail because ASCII art mice keep coming out
 * square. Facing left, admiring the icon columns. */
static void draw_wallpaper_mouse(void) {
    s16 step;

    /* A soft contact shadow, then body, head, ear, nose and eye. */
    gfx_ellipse_fill(252, 142, 32, 5, COLOR_DEEP);
    gfx_ellipse_fill(252, 124, 30, 18, GFX_MOUSE_BODY);
    gfx_ellipse_fill(222, 112, 16, 13, GFX_MOUSE_BODY);
    gfx_ellipse_fill(242, 94, 11, 10, GFX_MOUSE_BODY);
    gfx_ellipse_fill(243, 95, 6, 5, COLOR_DARK_GRAY);
    gfx_ellipse_fill(206, 115, 3, 2, COLOR_DEEP);
    gfx_circle(220, 106, 2, COLOR_DEEP, 1u);

    /* The curling tail: a quadratic bezier from the body's back to the
     * upper right, dotted with little accent circles. */
    for (step = 0; step <= 40; ++step) {
        const s32 t = step;
        const s32 u = 40 - step;
        const s16 px = (s16)((u * u * 280 + 2 * u * t * 300 + t * t * 306) /
                             1600);
        const s16 py = (s16)((u * u * 122 + 2 * u * t * 120 + t * t * 86) /
                             1600);

        gfx_circle(px, py, 1, GFX_MOUSE_TAIL, 1u);
    }
}

static void draw_wallpaper(void) {
    /* The Xubuntu night sky: a graphite blue that deepens towards the
     * bottom of the screen, painted out of colour cube mixtures. */
    gfx_gradient_vertical(0, (s16)DESKTOP_TOP, (s16)SCREEN_WIDTH,
                          (s16)DESKTOP_HEIGHT, COLOR_PANEL, COLOR_DEEP);
    draw_wallpaper_mouse();
}

static void draw_desktop_icons(void) {
    u16 index;

    /* The wallpaper itself is one big hotspot, so a click on empty space
     * can deselect and a right click can open the menu. It goes FIRST:
     * the icons drawn after it stand in front and win every overlap. */
    gui_hotspot(0, (s16)DESKTOP_TOP, (s16)SCREEN_WIDTH, (s16)DESKTOP_HEIGHT,
                HOTSPOT_DESKTOP);

    for (index = 0u; index < application_count; ++index) {
        const s16 column = (s16)(index % ICON_COLUMNS);
        const s16 row = (s16)(index / ICON_COLUMNS);
        const s16 cell_x = (s16)(ICON_LEFT + (s16)(column * ICON_COLUMN_WIDTH));
        const s16 cell_y = (s16)(ICON_TOP + (s16)(row * ICON_ROW_HEIGHT));
        const s16 icon_x = (s16)(cell_x + (ICON_COLUMN_WIDTH - 16) / 2 - 2);

        /* A dark copy one pixel down and right gives the icon some depth. */
        gfx_picture((s16)(icon_x + 1), (s16)(cell_y + 1),
                    application_list[index]->icon, 16u, COLOR_BLACK, COLOR_BLACK);
        gfx_picture(icon_x, cell_y, application_list[index]->icon, 16u,
                    application_list[index]->color, COLOR_HILITE);

        {
            const u16 width = gfx_text_width(application_list[index]->dock_label);
            s16 label_x = (s16)(cell_x +
                                (ICON_COLUMN_WIDTH - (s16)width) / 2 - 2);

            if (label_x < 0) {
                label_x = 0;
            }
            if (selected_icon == index) {
                gfx_fill((s16)(label_x - 2), (s16)(cell_y + 18),
                         (s16)(width + 4), 10, COLOR_SELECTION);
            }
            text_shadowed(label_x, (s16)(cell_y + 19),
                          application_list[index]->dock_label, COLOR_WHITE);
        }
        gui_hotspot(cell_x, cell_y, (s16)(ICON_COLUMN_WIDTH - 4), 30,
                    (u16)(HOTSPOT_ICON_BASE + index));
    }

    text_shadowed(174, (s16)(DESKTOP_TOP + DESKTOP_HEIGHT - 29),
                  "Right click: menu!", COLOR_HILITE);
    text_shadowed(246, (s16)(DESKTOP_TOP + DESKTOP_HEIGHT - 17),
                  "am-nyam!", COLOR_ACCENT);
}

/* The dock's launchers: Xubuntu's bottom panel, centered like plank. A
 * small accent dot marks the window that is open right now. */
static void draw_dock(void) {
    const s16 cell = 24;
    const s16 icons_width = (s16)((s16)application_count * cell);
    const s16 left = (s16)((SCREEN_WIDTH - icons_width) / 2 + 16);
    s16 x = (s16)(left - 21);
    u16 index;

    gfx_gradient_vertical(0, (s16)TASK_BAR_TOP, (s16)SCREEN_WIDTH,
                          (s16)TASK_BAR_HEIGHT, COLOR_PANEL, COLOR_DEEP);
    gfx_horizontal_line(0, (s16)TASK_BAR_TOP, (s16)SCREEN_WIDTH, COLOR_SHADOW);

    /* First cell: show/hide the desktop, the little monitor icon plank
     * and the Xfce "Desktop" applet both carry. */
    if (pressed_id == HOTSPOT_SHRINK) {
        gfx_fill(x, (s16)(TASK_BAR_TOP + 2), 18, 16, COLOR_DEEP);
    }
    gfx_picture_opaque((s16)(x + 4), (s16)(TASK_BAR_TOP + 4), shrink_icon, 8u,
                       (active_application == 0xFFFFu) ? COLOR_ACCENT
                                                       : COLOR_HILITE,
                       COLOR_PANEL);
    gui_hotspot(x, (s16)(TASK_BAR_TOP + 1), 18, 18, HOTSPOT_SHRINK);
    gfx_vertical_line((s16)(x + 20), (s16)(TASK_BAR_TOP + 3), 14, COLOR_SHADOW);

    for (index = 0u; index < application_count; ++index) {
        x = (s16)(left + (s16)(index * (u16)cell));

        if (pressed_id == (u16)(HOTSPOT_DOCK_BASE + index)) {
            /* Pressed like plank presses: the whole cell sinks a touch. */
            gfx_fill((s16)(x + 1), (s16)(TASK_BAR_TOP + 3), 20, 15, COLOR_DEEP);
        } else {
            gfx_fill((s16)(x + 3), (s16)(TASK_BAR_TOP + 2), 16, 16,
                     COLOR_PANEL);
        }
        gfx_picture((s16)(x + 3), (s16)(TASK_BAR_TOP + 2),
                    application_list[index]->icon, 16u,
                    application_list[index]->color, COLOR_HILITE);
        if (active_application == index) {
            /* plank's running dot: a warm accent lamp under the icon. */
            gfx_fill((s16)(x + 9), (s16)(TASK_BAR_TOP + 18 - 1), 5, 2,
                     COLOR_ACCENT);
        }
        if (focused_id == (u16)(HOTSPOT_DOCK_BASE + index)) {
            gfx_outline((s16)(x + 1), (s16)(TASK_BAR_TOP + 1), 21, 17,
                        COLOR_ACCENT);
        }
        gui_hotspot((s16)(x + 1), (s16)(TASK_BAR_TOP + 1), 22, 18,
                    (u16)(HOTSPOT_DOCK_BASE + index));
    }
}

/* The xfce4-panel itself: mouse menu button on the left, the open
 * window's name in the middle (DimXfce's version of xfce4-taskbar), the
 * clock plugin and a power button on the right. */
static void draw_top_panel(void) {
    char clock_line[8];

    gfx_gradient_vertical(0, 0, (s16)SCREEN_WIDTH, (s16)TITLE_BAR_HEIGHT,
                          COLOR_SHADOW, COLOR_PANEL);
    gfx_horizontal_line(0, (s16)(TITLE_BAR_HEIGHT - 1), (s16)SCREEN_WIDTH,
                        COLOR_BLACK);

    /* The Whisker menu button. It keeps the classic shape -- logo plus
     * caption -- and glows while the menu is open. */
    gui_button_colored(2, 2, 56, 10, "", HOTSPOT_HOME,
                       (menu_open != 0u) ? COLOR_SELECTION : COLOR_PANEL,
                       COLOR_WHITE);
    draw_mouse_logo(6, 2);
    gfx_text(26, 3, "Menu", COLOR_WHITE);

    if (active_application != 0xFFFFu) {
        const char *title = application_list[active_application]->title;

        gfx_text_centered(64, 3, 168, title, (window_rolled != 0u)
                                                 ? COLOR_ACCENT : COLOR_WHITE);
    } else {
        gfx_text_centered(64, 3, 168, "DimXfce", COLOR_HILITE);
    }

    clock_text(clock_line, (u16)sizeof(clock_line));
    gfx_text(252, 3, clock_line, COLOR_ACCENT);
    gfx_vertical_line(246, 2, 11, COLOR_BLACK);

    /* The session menu of a real panel, boiled down to the one action
     * this machine can actually perform: start over. */
    gui_button_colored(296, 2, 22, 10, "", HOTSPOT_POWER,
                       (pressed_id == HOTSPOT_POWER) ? COLOR_ALERT : COLOR_PANEL,
                       COLOR_WHITE);
    gfx_circle(307, 6, 3, COLOR_WHITE, 0u);
    gfx_vertical_line(307, 2, 4, COLOR_WHITE);
}

/* The Whisker menu: application buttons in a grid, Favourites filters on
 * the bottom left, About and Restart on the bottom right. */
static const char *const menu_class_names[MENU_CLASS_COUNT] = {
    "All", "Games", "Tools"
};

/* Is this application a member of the Favourites class on screen? The
 * games are found by name, because the list order may change; membership
 * follows the app, not its slot. */
static u8 app_is_game(u16 index) {
    const char *title = application_list[index]->title;

    return (u8)(text_equal(title, "Snake") != 0u ||
                text_equal(title, "Mines") != 0u ||
                text_equal(title, "Cheesy Balls") != 0u);
}

static u8 menu_class_has(u16 index) {
    if (menu_class == MENU_CLASS_ALL) {
        return 1u;
    }
    if (menu_class == MENU_CLASS_GAMES) {
        return app_is_game(index);
    }
    return (u8)(app_is_game(index) == 0u);
}

static void draw_menu(void) {
    u16 index;

    /* The menu body first: the item buttons drawn after it win every
     * click, and the clicks that miss an item land here harmlessly
     * instead of waking the window that hides beneath the menu. */
    gui_hotspot((s16)MENU_X, (s16)MENU_Y, (s16)MENU_WIDTH, (s16)MENU_HEIGHT,
                HOTSPOT_MENU_BODY);

    gfx_gradient_vertical((s16)MENU_X, (s16)MENU_Y, (s16)MENU_WIDTH,
                          (s16)MENU_HEIGHT, COLOR_PANEL, COLOR_DEEP);
    gfx_outline((s16)MENU_X, (s16)MENU_Y, (s16)MENU_WIDTH, (s16)MENU_HEIGHT,
                COLOR_BLACK);
    gfx_outline((s16)(MENU_X + 1), (s16)(MENU_Y + 1), (s16)(MENU_WIDTH - 2),
                (s16)(MENU_HEIGHT - 2), COLOR_SHADOW);

    draw_mouse_logo((s16)(MENU_X + 4), (s16)(MENU_Y + 4));
    gfx_text((s16)(MENU_X + 28), (s16)(MENU_Y + 4), "Whisker Menu", COLOR_WHITE);
    gfx_text((s16)(MENU_X + 28), (s16)(MENU_Y + 13), "all apps + cheese",
             COLOR_ACCENT);
    gfx_horizontal_line((s16)(MENU_X + 4), (s16)(MENU_Y + 21),
                        (s16)(MENU_WIDTH - 8), COLOR_SHADOW);

    for (index = 0u; index < application_count; ++index) {
        const s16 column = (s16)(index % 3u);
        const s16 row = (s16)(index / 3u);
        const s16 cell_x = (s16)(MENU_X + 4 + column * MENU_CELL_WIDTH);
        const s16 cell_y = (s16)(MENU_GRID_TOP + row * MENU_CELL_HEIGHT);
        const u16 id = (u16)(HOTSPOT_MENU_BASE + index);
        const u8 shown = menu_class_has(index);

        if (shown == 0u) {
            continue;
        }
        if (focused_id == id) {
            gfx_fill(cell_x, cell_y, (s16)(MENU_CELL_WIDTH - 3),
                     (s16)(MENU_CELL_HEIGHT - 3), COLOR_SELECTION);
        }
        gfx_picture((s16)(cell_x + (MENU_CELL_WIDTH - 3 - 16) / 2),
                    (s16)(cell_y + 2), application_list[index]->icon, 16u,
                    application_list[index]->color, COLOR_HILITE);
        gfx_text_centered(cell_x, (s16)(cell_y + 20),
                          (s16)(MENU_CELL_WIDTH - 3),
                          application_list[index]->title,
                          (active_application == index) ? COLOR_ACCENT
                                                        : COLOR_WHITE);
        if (active_application == index) {
            gfx_fill((s16)(cell_x + (MENU_CELL_WIDTH - 3) / 2 - 3),
                     (s16)(cell_y + MENU_CELL_HEIGHT - 7), 5, 2, COLOR_ACCENT);
        }
        gui_hotspot(cell_x, cell_y, (s16)(MENU_CELL_WIDTH - 3),
                    (s16)(MENU_CELL_HEIGHT - 5), id);
    }

    /* Favourites filters, bottom left. */
    for (index = 0u; index < MENU_CLASS_COUNT; ++index) {
        const s16 x = (s16)(MENU_X + 5 + (s16)index * 44);

        if (menu_class == index) {
            gfx_fill(x, (s16)(MENU_Y + MENU_HEIGHT - 15), 40, 11,
                     COLOR_SELECTION);
        }
        gfx_outline(x, (s16)(MENU_Y + MENU_HEIGHT - 15), 40, 11, COLOR_SHADOW);
        gfx_text_centered(x, (s16)(MENU_Y + MENU_HEIGHT - 13), 40,
                          menu_class_names[index],
                          (menu_class == index) ? COLOR_WHITE : COLOR_HILITE);
        gui_hotspot(x, (s16)(MENU_Y + MENU_HEIGHT - 15), 40, 11,
                    (u16)(HOTSPOT_CLASS_BASE + index));
    }

    /* The Whisker power strip, bottom right. */
    gui_button_colored((s16)(MENU_X + MENU_WIDTH - 102),
                       (s16)(MENU_Y + MENU_HEIGHT - 15), 46, 11, "About",
                       HOTSPOT_MENU_ABOUT, COLOR_PANEL, COLOR_WHITE);
    gui_button_colored((s16)(MENU_X + MENU_WIDTH - 52),
                       (s16)(MENU_Y + MENU_HEIGHT - 15), 47, 11, "Restart",
                       HOTSPOT_POWER, COLOR_ALERT, COLOR_WHITE);
}

/* The little bubble that explains the chrome while the pointer is held
 * down: panel buttons and the dock's show-desktop cell. */
static void draw_tooltip(void) {
    const u16 width = gfx_text_width(tooltip_text);
    const s16 width_px = (s16)(width + 6);
    s16 x = (s16)(tooltip_x - (s16)(width_px / 2));

    if (x < 2) {
        x = 2;
    }
    if ((s16)(x + width_px) > (s16)(SCREEN_WIDTH - 2)) {
        x = (s16)(SCREEN_WIDTH - 2 - (s16)width_px);
    }
    gfx_fill(x, tooltip_y, width_px, 11, COLOR_ACCENT);
    gfx_outline(x, tooltip_y, width_px, 11, COLOR_BLACK);
    gfx_text((s16)(x + 3), (s16)(tooltip_y + 2), tooltip_text, COLOR_BLACK);
}

static void tooltip_for(u16 id) {
    panel_tooltip = id;
    tooltip_y = (s16)(TITLE_BAR_HEIGHT + 1);
    if (id == HOTSPOT_HOME) {
        /* Xfce's own name for it: the menu with the whiskers. */
        tooltip_text = (menu_open != 0u) ? "Close the menu" : "Whisker menu";
        tooltip_x = 30;
    } else if (id == HOTSPOT_POWER) {
        tooltip_text = "Restart DimXfce";
        tooltip_x = 305;
    } else if (id == HOTSPOT_SHRINK) {
        tooltip_text = (active_application != 0xFFFFu) ? "Show the desktop"
                                                       : "Restore the window";
        tooltip_x = 32;
        tooltip_y = (s16)(TASK_BAR_TOP - 13);
    } else if (id == HOTSPOT_SHADE) {
        tooltip_text = "Roll up / unroll";
        tooltip_x = (s16)(WINDOW_X + WINDOW_WIDTH - 44);
        tooltip_y = (s16)(WINDOW_Y + 13);
    } else {
        tooltip_text = "";
    }
}

static void draw_frame(void) {
    gfx_clear(COLOR_DESKTOP);
    gui_hotspots_reset();

    if (active_application == 0xFFFFu) {
        draw_wallpaper();
        draw_desktop_icons();
    } else if (window_rolled != 0u) {
        /* Rolled up over the desktop: just the blind slat, Xfce style.
         * The whole slat is the shade button, so one click unrolls it. */
        draw_wallpaper();
        gfx_fill((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH, 12,
                 COLOR_DEEP);
        gfx_gradient_vertical((s16)(WINDOW_X + 1), (s16)(WINDOW_Y + 1),
                              (s16)(WINDOW_WIDTH - 2), 10,
                              title_top_color, COLOR_DEEP);
        gfx_text((s16)(WINDOW_X + 5), (s16)(WINDOW_Y + 2),
                 application_list[active_application]->title, COLOR_WHITE);
        gfx_text((s16)(WINDOW_X + WINDOW_WIDTH - 60), (s16)(WINDOW_Y + 2),
                 "(rolled)", COLOR_ACCENT);
        gfx_outline((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH, 12,
                    COLOR_BLACK);
        gui_hotspot((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH, 12,
                    HOTSPOT_SHADE);
    } else {
        const Application *app = application_list[active_application];

        gui_window_frame(app->title);
        app->draw();
    }

    if (panel_tooltip != HOTSPOT_NONE && tooltip_text[0] != '\0') {
        draw_tooltip();
    }
    draw_dock();
    draw_top_panel();
    if (menu_open != 0u) {
        draw_menu();
    }
    gfx_draw_pointer(input_pointer_x(), input_pointer_y());
}

/* ------------------------------------------------------------------ */
/* Opening and closing                                                 */
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

u8 gui_menu_is_open(void) {
    return menu_open;
}

void gui_menu_toggle(void) {
    menu_open = (u8)(menu_open == 0u);
    panel_tooltip = HOTSPOT_NONE;
}

/* The dock's first cell: minimize everything, or bring the window back.
 * With no window anywhere it opens the menu, like a second home. */
void gui_show_desktop(void) {
    if (active_application != 0xFFFFu) {
        gui_go_home();
    } else if (last_application != 0xFFFFu) {
        gui_open(last_application);
    } else {
        gui_menu_toggle();
    }
    panel_tooltip = HOTSPOT_NONE;
}

void gui_open(u16 index) {
    if (index >= application_count) {
        return;
    }
    active_application = index;
    last_application = index;
    pressed_id = HOTSPOT_NONE;
    focused_id = HOTSPOT_NONE;
    arrows_reserved = 0u;
    status_line[0] = '\0';
    menu_open = 0u;
    window_rolled = 0u;
    panel_tooltip = HOTSPOT_NONE;
    title_top_color = COLOR_SELECTION;
    application_list[index]->open();
    sound_beep();
}

void gui_go_home(void) {
    active_application = 0xFFFFu;
    pressed_id = HOTSPOT_NONE;
    focused_id = HOTSPOT_NONE;
    arrows_reserved = 0u;
    window_rolled = 0u;
    panel_tooltip = HOTSPOT_NONE;
    sound_stop();
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void deliver_to_application(u8 type, u16 id, const Event *event) {
    Event forwarded;

    if (active_application == 0xFFFFu || window_rolled != 0u) {
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

/* Window manager buttons first; anything else belongs to the application. */
static void activate(u16 id, u8 right_button, const Event *event) {
    panel_tooltip = HOTSPOT_NONE;

    if (id == HOTSPOT_HOME) {
        gui_menu_toggle();
        return;
    }
    if (id == HOTSPOT_POWER) {
        system_restart();
        return;
    }
    if (id == HOTSPOT_SHRINK) {
        gui_show_desktop();
        return;
    }
    if (id == HOTSPOT_MENU_ABOUT) {
        /* Found by name, so the menu keeps working if apps are reordered. */
        u16 index;

        for (index = 0u; index < application_count; ++index) {
            if (text_equal(application_list[index]->title, "About") != 0u) {
                gui_open(index);
                return;
            }
        }
        return;
    }
    if (id == HOTSPOT_SHADE || id == HOTSPOT_MINIMIZE) {
        /* Roll the window up into the title bar, or minimize it... to the
         * desktop, which is all a window of one can do. The Xfce shade
         * button affects the frame, minimize sends the app to the dock. */
        if (id == HOTSPOT_SHADE && active_application != 0xFFFFu) {
            window_rolled = (u8)(window_rolled == 0u);
        } else {
            gui_go_home();
        }
        return;
    }
    if (id == HOTSPOT_CLOSE) {
        gui_go_home();
        return;
    }
    if (id >= HOTSPOT_MENU_BASE &&
        id < (u16)(HOTSPOT_MENU_BASE + application_count)) {
        gui_open((u16)(id - HOTSPOT_MENU_BASE));
        return;
    }
    if (id >= HOTSPOT_DOCK_BASE &&
        id < (u16)(HOTSPOT_DOCK_BASE + application_count)) {
        const u16 index = (u16)(id - HOTSPOT_DOCK_BASE);

        if (active_application == index) {
            /* Tapping the open app's dock icon hides it to the desktop,
             * the way plank minimizes a focused window. */
            window_rolled = 0u;
            gui_go_home();
        } else {
            gui_open(index);
        }
        return;
    }
    if (id == HOTSPOT_MENU_BODY) {
        return; /* a miss inside the menu must not wake the window below */
    }
    if (id >= HOTSPOT_CLASS_BASE &&
        id < (u16)(HOTSPOT_CLASS_BASE + MENU_CLASS_COUNT)) {
        menu_class = (u8)(id - HOTSPOT_CLASS_BASE);
        return;
    }
    if (id == HOTSPOT_DESKTOP) {
        if (right_button != 0u) {
            /* xfdesktop's best trick: the whole menu, anywhere. */
            menu_open = 1u;
        } else if (menu_open != 0u) {
            menu_open = 0u; /* a plain click outside closes the menu */
        }
        return;
    }
    if (active_application == 0xFFFFu) {
        if (id >= HOTSPOT_ICON_BASE &&
            id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
            gui_open((u16)(id - HOTSPOT_ICON_BASE));
        }
        return;
    }
    if (right_button != 0u) {
        if (id != HOTSPOT_NONE) {
            deliver_to_application(EVENT_RIGHT_CLICK, id, event);
        }
        return;
    }
    /* Clicks that miss every button still reach the application, because
     * drawing boards need to know when the pointer was released. */
    deliver_to_application(EVENT_CLICK, id, event);
}

static void handle_event(const Event *event) {
    switch (event->type) {
        case EVENT_KEY:
            if (event->key == KEY_ESCAPE) {
                if (menu_open != 0u) {
                    menu_open = 0u;
                    return;
                }
                if (active_application != 0xFFFFu) {
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
                activate(focused_id, 0u, (const Event *)0);
                return;
            }
            if (menu_open == 0u && active_application != 0xFFFFu &&
                window_rolled == 0u) {
                application_list[active_application]->event(event);
            }
            return;

        case EVENT_PRESS:
            pressed_id = gui_hotspot_at(event->x, event->y);
            if (pressed_id == HOTSPOT_HOME || pressed_id == HOTSPOT_POWER ||
                pressed_id == HOTSPOT_SHRINK || pressed_id == HOTSPOT_SHADE) {
                tooltip_for(pressed_id);
            }
            if (active_application == 0xFFFFu &&
                pressed_id >= HOTSPOT_ICON_BASE &&
                pressed_id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
                selected_icon = (u16)(pressed_id - HOTSPOT_ICON_BASE);
            }
            if (pressed_id != HOTSPOT_NONE) {
                focused_id = pressed_id;
            }
            if (menu_open == 0u) {
                deliver_to_application(EVENT_PRESS, pressed_id, event);
            }
            return;

        case EVENT_CLICK:
            pressed_id = HOTSPOT_NONE;
            activate(gui_hotspot_at(event->x, event->y), 0u, event);
            return;

        case EVENT_RIGHT_CLICK:
            activate(gui_hotspot_at(event->x, event->y), 1u, event);
            return;

        case EVENT_DRAG:
            /* Dragging across the chrome keeps the tooltip out of the
             * way of the click that follows. */
            if (pressed_id != HOTSPOT_NONE &&
                gui_hotspot_at(input_pointer_x(), input_pointer_y()) !=
                    pressed_id) {
                panel_tooltip = HOTSPOT_NONE;
            }
            /* fall through */
        case EVENT_MOVE:
            pressed_id = HOTSPOT_NONE;
            if (menu_open == 0u) {
                deliver_to_application(event->type, HOTSPOT_NONE, event);
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
    while (timer_counter() < *deadline) {
        timer_update();
        input_poll();
    }
    *deadline += FRAME_TIMER_COUNTS;
}

void gui_run(void) {
    Event event;
    u64 next_frame;

    selected_icon = 0u;
    active_application = 0xFFFFu;
    sound_play_startup();
    timer_update();
    next_frame = timer_counter() + FRAME_TIMER_COUNTS;

    for (;;) {
        u16 ticks;

        timer_update();
        ticks = timer_take_ticks();

        input_poll();
        input_advance(ticks);

        while (input_next_event(&event) != 0u) {
            handle_event(&event);
        }

        if (active_application != 0xFFFFu && window_rolled == 0u) {
            const Application *app = application_list[active_application];

            if (app->update != 0) {
                app->update((u16)(ticks * TICK_MILLISECONDS));
            }
        }
        sound_update(ticks);

        draw_frame();
        gfx_show();

        wait_for_next_frame(&next_frame);
    }
}
