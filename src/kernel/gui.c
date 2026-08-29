/*
 * gui.c -- the desktop, the task bar and the widget helpers.
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

/* One frame every 40 ms is 25 pictures per second: smooth enough for the
 * games, light enough for a slow emulator. */
#define FRAME_MILLISECONDS 40u

/* Ids the window manager keeps for its own buttons. Applications use ids
 * below this range. */
#define HOTSPOT_ICON_BASE 0x8000u
#define HOTSPOT_TASK_BASE 0x8100u
#define HOTSPOT_HOME 0x8200u
#define HOTSPOT_CLOSE 0x8201u

#define ICON_COLUMNS 4u
#define ICON_CELL_WIDTH (s16)(SCREEN_WIDTH / ICON_COLUMNS)
#define ICON_CELL_HEIGHT 62

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

void gui_window_frame(const char *title) {
    /* The window itself. */
    gfx_fill((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH, (s16)WINDOW_HEIGHT,
             COLOR_FACE);
    gfx_outline((s16)WINDOW_X, (s16)WINDOW_Y, (s16)WINDOW_WIDTH,
                (s16)WINDOW_HEIGHT, COLOR_BLACK);
    gfx_raised_box((s16)(WINDOW_X + 1), (s16)(WINDOW_Y + 1),
                   (s16)(WINDOW_WIDTH - 2), (s16)(WINDOW_HEIGHT - 2), 1u);

    /* Title bar with a close box on the right. */
    gfx_fill((s16)(WINDOW_X + 3), (s16)(WINDOW_Y + 3), (s16)(WINDOW_WIDTH - 6), 10,
             COLOR_TITLE_BAR);
    gfx_text((s16)(WINDOW_X + 6), (s16)(WINDOW_Y + 4), title, COLOR_WHITE);
    gui_button_colored((s16)(WINDOW_X + WINDOW_WIDTH - 16), (s16)(WINDOW_Y + 3),
                       13, 10, "x", HOTSPOT_CLOSE, COLOR_FACE, COLOR_BLACK);
}

/* ------------------------------------------------------------------ */
/* Desktop, task bar and title bar                                     */
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

static void draw_title_bar(void) {
    char buffer[24];

    gfx_fill(0, 0, (s16)SCREEN_WIDTH, (s16)TITLE_BAR_HEIGHT, COLOR_TITLE_BAR);
    gfx_horizontal_line(0, (s16)(TITLE_BAR_HEIGHT - 1), (s16)SCREEN_WIDTH, COLOR_BLACK);

    gui_button_colored(2, 2, 44, 8, "DimOS", HOTSPOT_HOME, COLOR_FACE, COLOR_BLACK);

    if (active_application != 0xFFFFu) {
        gfx_text(52, 2, application_list[active_application]->title, COLOR_WHITE);
    } else {
        gfx_text(52, 2, "Desktop", COLOR_WHITE);
    }

    clock_text(buffer, (u16)sizeof(buffer));
    gfx_text((s16)(SCREEN_WIDTH - (s16)gfx_text_width(buffer) - 4), 2, buffer,
             COLOR_YELLOW);
}

static void draw_task_bar(void) {
    u16 index;
    const s16 button_width = (s16)((SCREEN_WIDTH - 16) / application_count);

    gfx_fill(0, (s16)TASK_BAR_TOP, (s16)SCREEN_WIDTH, (s16)TASK_BAR_HEIGHT, COLOR_FACE);
    gfx_horizontal_line(0, (s16)TASK_BAR_TOP, (s16)SCREEN_WIDTH, COLOR_HILITE);
    gfx_horizontal_line(0, (s16)(TASK_BAR_TOP + 1), (s16)SCREEN_WIDTH, COLOR_SHADOW);

    for (index = 0u; index < application_count; ++index) {
        const s16 x = (s16)(8 + (s16)(index * (u16)button_width));
        const u8 current = (active_application == index) ? 1u : 0u;

        if (current != 0u) {
            gui_button_colored(x, (s16)(TASK_BAR_TOP + 3), button_width, 12,
                               application_list[index]->task_label,
                               (u16)(HOTSPOT_TASK_BASE + index),
                               COLOR_SELECTION, COLOR_WHITE);
        } else {
            gui_button(x, (s16)(TASK_BAR_TOP + 3), button_width, 12,
                       application_list[index]->task_label,
                       (u16)(HOTSPOT_TASK_BASE + index));
        }
    }
}

static void draw_desktop_icons(void) {
    u16 index;

    for (index = 0u; index < application_count; ++index) {
        const s16 column = (s16)(index % ICON_COLUMNS);
        const s16 row = (s16)(index / ICON_COLUMNS);
        const s16 cell_x = (s16)(column * ICON_CELL_WIDTH);
        const s16 cell_y = (s16)(DESKTOP_TOP + 12 + (s16)(row * ICON_CELL_HEIGHT));
        const s16 icon_x = (s16)(cell_x + (ICON_CELL_WIDTH - 16) / 2);
        const u16 label_width = gfx_text_width(application_list[index]->title);
        const s16 label_x = (s16)(cell_x + (ICON_CELL_WIDTH - (s16)label_width) / 2);

        if (selected_icon == index) {
            gfx_fill((s16)(label_x - 2), (s16)(cell_y + 20), (s16)(label_width + 4),
                     10, COLOR_SELECTION);
            gfx_text(label_x, (s16)(cell_y + 21), application_list[index]->title,
                     COLOR_WHITE);
        } else {
            gfx_text(label_x, (s16)(cell_y + 21), application_list[index]->title,
                     COLOR_WHITE);
        }

        /* A dark copy one pixel down and right gives the icon some depth. */
        gfx_picture((s16)(icon_x + 1), (s16)(cell_y + 1),
                    application_list[index]->icon, 16u, COLOR_BLACK, COLOR_BLACK);
        gfx_picture(icon_x, cell_y, application_list[index]->icon, 16u,
                    application_list[index]->color, COLOR_HILITE);

        gui_hotspot(cell_x, cell_y, ICON_CELL_WIDTH, 34,
                    (u16)(HOTSPOT_ICON_BASE + index));
    }

    gfx_text_centered(0, (s16)(DESKTOP_TOP + DESKTOP_HEIGHT - 14),
                      (s16)SCREEN_WIDTH,
                      "Point and click - no typing needed", COLOR_WHITE);
}

static void draw_frame(void) {
    gfx_clear(COLOR_DESKTOP);
    gui_hotspots_reset();

    if (active_application == 0xFFFFu) {
        draw_desktop_icons();
    } else {
        const Application *app = application_list[active_application];

        gui_window_frame(app->title);
        app->draw();
    }

    draw_task_bar();
    draw_title_bar();
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

void gui_open(u16 index) {
    if (index >= application_count) {
        return;
    }
    active_application = index;
    pressed_id = HOTSPOT_NONE;
    focused_id = HOTSPOT_NONE;
    arrows_reserved = 0u;
    status_line[0] = '\0';
    application_list[index]->open();
    sound_beep();
}

void gui_go_home(void) {
    active_application = 0xFFFFu;
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

/* Window manager buttons first; anything else belongs to the application. */
static void activate(u16 id, u8 right_button, const Event *event) {
    if (id == HOTSPOT_HOME || id == HOTSPOT_CLOSE) {
        gui_go_home();
        return;
    }
    if (id >= HOTSPOT_TASK_BASE && id < (u16)(HOTSPOT_TASK_BASE + application_count)) {
        const u16 index = (u16)(id - HOTSPOT_TASK_BASE);

        if (active_application == index) {
            gui_go_home();
        } else {
            gui_open(index);
        }
        return;
    }
    if (active_application == 0xFFFFu) {
        if (id >= HOTSPOT_ICON_BASE && id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
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
            if (active_application != 0xFFFFu) {
                application_list[active_application]->event(event);
            }
            return;

        case EVENT_PRESS:
            pressed_id = gui_hotspot_at(event->x, event->y);
            if (active_application == 0xFFFFu &&
                pressed_id >= HOTSPOT_ICON_BASE &&
                pressed_id < (u16)(HOTSPOT_ICON_BASE + application_count)) {
                selected_icon = (u16)(pressed_id - HOTSPOT_ICON_BASE);
            }
            if (pressed_id != HOTSPOT_NONE) {
                focused_id = pressed_id;
            }
            deliver_to_application(EVENT_PRESS, pressed_id, event);
            return;

        case EVENT_CLICK:
            pressed_id = HOTSPOT_NONE;
            activate(gui_hotspot_at(event->x, event->y), 0u, event);
            return;

        case EVENT_RIGHT_CLICK:
            activate(gui_hotspot_at(event->x, event->y), 1u, event);
            return;

        case EVENT_DRAG:
        case EVENT_MOVE:
            pressed_id = HOTSPOT_NONE;
            deliver_to_application(event->type, HOTSPOT_NONE, event);
            return;

        default:
            return;
    }
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static void wait_for_next_frame(u32 started_at) {
    while ((time_milliseconds() - started_at) < FRAME_MILLISECONDS) {
        timer_update();
        input_poll();
    }
}

void gui_run(void) {
    Event event;

    selected_icon = 0u;
    active_application = 0xFFFFu;
    sound_play_startup();

    for (;;) {
        u16 ticks;

        timer_update();
        ticks = timer_take_ticks();

        input_poll();
        input_advance(ticks);

        while (input_next_event(&event) != 0u) {
            handle_event(&event);
        }

        if (active_application != 0xFFFFu) {
            const Application *app = application_list[active_application];

            if (app->update != 0) {
                app->update((u16)(ticks * TICK_MILLISECONDS));
            }
        }
        sound_update(ticks);

        draw_frame();
        gfx_show();

        wait_for_next_frame(time_milliseconds());
    }
}
