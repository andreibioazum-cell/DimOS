/*
 * app_paint.c -- a small drawing board with an iOS colour palette.
 *
 * The picture lives in the kernel's BSS, so it survives being redrawn
 * every frame. Sixteen colours, three brush sizes, and a button that
 * stores the picture on the RAM disk.
 */

#include "dimos.h"

#define CANVAS_WIDTH 566
#define CANVAS_HEIGHT 240
#define CANVAS_X (s16)APP_LEFT
#define CANVAS_Y 78
#define SWATCH 26
#define SWATCH_STEP 48
#define SWATCH_X 618
#define SWATCH_Y 84

#define ID_CANVAS 1u
#define ID_COLOR_FIRST 2u   /* 2 .. 17 */
#define ID_BRUSH_1 20u
#define ID_BRUSH_2 21u
#define ID_BRUSH_4 22u
#define ID_CLEAR 23u
#define ID_SAVE 24u

/* Sector on the RAM disk where the picture is stored. */
#define PAINT_SECTOR 3000u

static const char *const icon[16] = {
    "................",
    "...........XXX..",
    "..........XXXX..",
    ".........XXXX...",
    "........XXXX....",
    ".......XXXX.....",
    "......XXXX......",
    ".....XXXX.......",
    "....XXXX........",
    "...XXXX.........",
    "..XXXX..........",
    "..XXX...........",
    ".XXX............",
    ".XX.............",
    "................",
    "................",
};

/* The colours a painter can pick, in the order they are shown. */
static const u8 paint_colors[16] = {
    COLOR_BLACK, COLOR_DARK_GRAY, COLOR_DISABLED, COLOR_FACE,
    COLOR_ALERT, COLOR_IOS_PINK, COLOR_ACCENT, COLOR_YELLOW,
    COLOR_GOOD, COLOR_IOS_TEAL, COLOR_WIN_LIGHT, COLOR_SELECTION,
    COLOR_IOS_INDIGO, COLOR_IOS_PURPLE, COLOR_BROWN, COLOR_MAGENTA
};

/* The picture lives on the scratch RAM disk, outside the kernel BSS. */
#define canvas (ram_disk_buffer())
static u8 brush_color = COLOR_BLACK;
static u8 brush_size = 4u;
static u8 drawing;
static s16 last_x = -1;
static s16 last_y = -1;
static char message[48];

static void canvas_clear(void) {
    u32 index;

    for (index = 0u; index < (u32)(CANVAS_WIDTH * CANVAS_HEIGHT); ++index) {
        canvas[index] = COLOR_CANVAS;
    }
}

static void paint_open(void) {
    canvas_clear();
    drawing = 0u;
    last_x = -1;
    last_y = -1;
    text_copy(message, "Draw with a finger, or save the picture to RAM",
              (u16)sizeof(message));
}

static void plot(s16 x, s16 y) {
    s16 dx;
    s16 dy;

    for (dy = 0; dy < (s16)brush_size; ++dy) {
        for (dx = 0; dx < (s16)brush_size; ++dx) {
            const s16 px = (s16)(x + dx);
            const s16 py = (s16)(y + dy);

            if (px < 0 || py < 0 || px >= CANVAS_WIDTH || py >= CANVAS_HEIGHT) {
                continue;
            }
            canvas[(u16)py * CANVAS_WIDTH + (u16)px] = brush_color;
        }
    }
}

static s16 absolute_value(s16 value) {
    return (value < 0) ? (s16)-value : value;
}

/* A fast drag only reports every few pixels, so fill the gap with a straight
 * line instead of leaving a row of dots behind. */
static void stroke(s16 x, s16 y) {
    if (last_x < 0) {
        plot(x, y);
    } else {
        s16 steps = absolute_value((s16)(x - last_x));
        const s16 vertical = absolute_value((s16)(y - last_y));
        s16 step;

        if (vertical > steps) {
            steps = vertical;
        }
        for (step = 0; step <= steps; ++step) {
            s16 px = x;
            s16 py = y;

            if (steps != 0) {
                px = (s16)(last_x + (s16)(((s16)(x - last_x) * step) / steps));
                py = (s16)(last_y + (s16)(((s16)(y - last_y) * step) / steps));
            }
            plot(px, py);
        }
    }
    last_x = x;
    last_y = y;
}

static void paint_draw(void) {
    u16 row;
    u8 index;

    gui_window_frame("Paint");

    /* Copy the picture to the canvas one row at a time. */
    for (row = 0u; row < (u16)CANVAS_HEIGHT; ++row) {
        memory_copy(screen + (u32)(CANVAS_Y + (s16)row) * SCREEN_WIDTH +
                        (u32)CANVAS_X,
                    canvas + (u32)row * CANVAS_WIDTH, CANVAS_WIDTH);
    }
    gfx_round_outline((s16)(CANVAS_X - 2), (s16)(CANVAS_Y - 2),
                      (s16)(CANVAS_WIDTH + 4), (s16)(CANVAS_HEIGHT + 4), 10,
                      COLOR_GRID);
    gui_hotspot(CANVAS_X, CANVAS_Y, (s16)CANVAS_WIDTH, (s16)CANVAS_HEIGHT,
                ID_CANVAS);

    gui_section_label(SWATCH_X, 66, "COLOUR");
    for (index = 0u; index < 16u; ++index) {
        const s16 column = (s16)(index % 4u);
        const s16 line = (s16)(index / 4u);
        const s16 x = (s16)(SWATCH_X + column * SWATCH_STEP);
        const s16 y = (s16)(SWATCH_Y + line * SWATCH_STEP);

        gfx_circle((s16)(x + SWATCH / 2), (s16)(y + SWATCH / 2),
                   (s16)(SWATCH / 2), paint_colors[index], 1u);
        if (paint_colors[index] == brush_color) {
            gfx_circle((s16)(x + SWATCH / 2), (s16)(y + SWATCH / 2),
                       (s16)(SWATCH / 2 + 4), COLOR_FACE, 0u);
            gfx_circle((s16)(x + SWATCH / 2), (s16)(y + SWATCH / 2),
                       (s16)(SWATCH / 2 + 5), COLOR_DEEP, 0u);
        }
        gui_hotspot(x, y, SWATCH, SWATCH, (u16)(ID_COLOR_FIRST + index));
    }

    gui_section_label(SWATCH_X, 278, "BRUSH");
    gui_button_colored(SWATCH_X, 292, 50, 34, "1", ID_BRUSH_1,
                       (brush_size == 1u) ? COLOR_SELECTION : COLOR_FACE,
                       (brush_size == 1u) ? COLOR_FACE : COLOR_DEEP);
    gui_button_colored((s16)(SWATCH_X + 58), 292, 50, 34, "2", ID_BRUSH_2,
                       (brush_size == 2u) ? COLOR_SELECTION : COLOR_FACE,
                       (brush_size == 2u) ? COLOR_FACE : COLOR_DEEP);
    gui_button_colored((s16)(SWATCH_X + 116), 292, 50, 34, "4", ID_BRUSH_4,
                       (brush_size == 4u) ? COLOR_SELECTION : COLOR_FACE,
                       (brush_size == 4u) ? COLOR_FACE : COLOR_DEEP);

    gui_button_colored(SWATCH_X, 336, 78, 26, "Clear", ID_CLEAR,
                       COLOR_PANEL, COLOR_DEEP);
    gui_button((s16)(SWATCH_X + 86), 336, 78, 26, "Save", ID_SAVE);

    gui_message_bar(message);
}

static void paint_event(const Event *event) {
    if (event->type == EVENT_CLICK) {
        if (event->key >= ID_COLOR_FIRST &&
            event->key < (u16)(ID_COLOR_FIRST + 16u)) {
            brush_color = paint_colors[event->key - ID_COLOR_FIRST];
            text_copy(message, "Colour picked", (u16)sizeof(message));
            return;
        }
        if (event->key == ID_BRUSH_1) {
            brush_size = 1u;
        } else if (event->key == ID_BRUSH_2) {
            brush_size = 2u;
        } else if (event->key == ID_BRUSH_4) {
            brush_size = 4u;
        } else if (event->key == ID_CLEAR) {
            canvas_clear();
            text_copy(message, "Canvas cleared", (u16)sizeof(message));
        } else if (event->key == ID_SAVE) {
            const u32 sectors =
                ((u32)(CANVAS_WIDTH * CANVAS_HEIGHT) + 511u) / 512u;

            if (ram_disk_write(PAINT_SECTOR, canvas, sectors) != 0u) {
                message[0] = '\0';
                text_append(message, "Stored on the RAM disk, ",
                            (u16)sizeof(message));
                text_append_number(message, sectors, (u16)sizeof(message));
                text_append(message, " sectors", (u16)sizeof(message));
            } else {
                text_copy(message, "The RAM disk is full",
                          (u16)sizeof(message));
            }
        }
        return;
    }

    if (event->type == EVENT_PRESS) {
        if (event->x >= CANVAS_X && event->x < (s16)(CANVAS_X + CANVAS_WIDTH) &&
            event->y >= CANVAS_Y && event->y < (s16)(CANVAS_Y + CANVAS_HEIGHT)) {
            drawing = 1u;
            last_x = -1;
            stroke((s16)(event->x - CANVAS_X), (s16)(event->y - CANVAS_Y));
        }
        return;
    }
    if (event->type == EVENT_DRAG && drawing != 0u) {
        stroke((s16)(event->x - CANVAS_X), (s16)(event->y - CANVAS_Y));
        return;
    }
    if ((event->type == EVENT_CLICK || event->type == EVENT_MOVE) &&
        drawing != 0u) {
        drawing = 0u;
        last_x = -1;
        last_y = -1;
    }
}

const Application application_paint = {
    "Paint",
    "Paint",
    icon,
    COLOR_IOS_PURPLE,
    paint_open,
    paint_draw,
    paint_event,
    0
};
