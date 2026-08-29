/*
 * app_paint.c -- a small drawing board.
 *
 * The picture itself lives in the scratch area of memory (SCRATCH_ADDRESS),
 * so it survives being redrawn every frame. Sixteen colours, three brush
 * sizes, and a button that stores the picture on the RAM disk.
 */

#include "dimos.h"

#define CANVAS_WIDTH 220
#define CANVAS_HEIGHT 110
#define CANVAS_X (s16)(WINDOW_LEFT + 4)
#define CANVAS_Y (s16)(WINDOW_TOP + 4)
#define SWATCH 15

#define ID_CANVAS 1u
#define ID_COLOR_FIRST 2u   /* 2 .. 17 */
#define ID_BRUSH_1 20u
#define ID_BRUSH_2 21u
#define ID_BRUSH_4 22u
#define ID_CLEAR 23u
#define ID_SAVE 24u

/* Sector on the RAM disk where the picture is stored. */
#define PAINT_SECTOR 100u

static const char *const icon[16] = {
    "................",
    ".......XX.......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    "......XXXX......",
    ".....XXXXXX.....",
    ".....XXXXXX.....",
    "....XXXXXXXX....",
    "....XXXXXXXX....",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    ".....XXXXXX.....",
};

/* The sixteen colours a painter can pick, in the order they are shown. */
static const u8 paint_colors[16] = {
    COLOR_BLACK, COLOR_BLUE, COLOR_GREEN, COLOR_CYAN,
    COLOR_RED, COLOR_MAGENTA, COLOR_BROWN, COLOR_LIGHT_GRAY,
    COLOR_DARK_GRAY, COLOR_LIGHT_BLUE, COLOR_LIGHT_GREEN, COLOR_LIGHT_CYAN,
    COLOR_LIGHT_RED, COLOR_YELLOW, COLOR_WHITE, COLOR_CANVAS
};

static u8 *const canvas = (u8 *)SCRATCH_ADDRESS;
static u8 brush_color = COLOR_BLACK;
static u8 brush_size = 2u;
static u8 drawing;
static s16 last_x = -1;
static s16 last_y = -1;
static char message[40];

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
    text_copy(message, "Draw with the pointer", (u16)sizeof(message));
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
    char buffer[16];

    /* Copy the picture to the frame buffer one row at a time. */
    for (row = 0u; row < (u16)CANVAS_HEIGHT; ++row) {
        memory_copy(screen + (u32)((s16)(CANVAS_Y + (s16)row)) * SCREEN_WIDTH +
                        (u32)CANVAS_X,
                    canvas + (u32)row * CANVAS_WIDTH, CANVAS_WIDTH);
    }
    gfx_outline((s16)(CANVAS_X - 1), (s16)(CANVAS_Y - 1), (s16)(CANVAS_WIDTH + 2),
                (s16)(CANVAS_HEIGHT + 2), COLOR_SHADOW);
    gui_hotspot(CANVAS_X, CANVAS_Y, (s16)CANVAS_WIDTH, (s16)CANVAS_HEIGHT, ID_CANVAS);

    for (index = 0u; index < 16u; ++index) {
        const s16 column = (s16)(index % 4u);
        const s16 line = (s16)(index / 4u);
        const s16 x = (s16)(CANVAS_X + CANVAS_WIDTH + 10 + (s16)(column * SWATCH));
        const s16 y = (s16)(CANVAS_Y + (s16)(line * SWATCH));

        gfx_fill(x, y, (s16)(SWATCH - 2), (s16)(SWATCH - 2), paint_colors[index]);
        gfx_outline((s16)(x - 1), (s16)(y - 1), (s16)(SWATCH - 1),
                    (s16)(SWATCH - 1),
                    (paint_colors[index] == brush_color) ? COLOR_BLACK : COLOR_SHADOW);
        gui_hotspot(x, y, (s16)(SWATCH - 2), (s16)(SWATCH - 2),
                    (u16)(ID_COLOR_FIRST + index));
    }

    gui_button((s16)(CANVAS_X + CANVAS_WIDTH + 10), (s16)(CANVAS_Y + 64), 20, 14,
               "1", ID_BRUSH_1);
    gui_button((s16)(CANVAS_X + CANVAS_WIDTH + 32), (s16)(CANVAS_Y + 64), 20, 14,
               "2", ID_BRUSH_2);
    gui_button((s16)(CANVAS_X + CANVAS_WIDTH + 54), (s16)(CANVAS_Y + 64), 20, 14,
               "4", ID_BRUSH_4);

    buffer[0] = '\0';
    text_append(buffer, "Brush ", (u16)sizeof(buffer));
    text_append_number(buffer, brush_size, (u16)sizeof(buffer));
    gfx_text((s16)(CANVAS_X + CANVAS_WIDTH + 10), (s16)(CANVAS_Y + 82), buffer,
             COLOR_BLACK);

    gui_button((s16)(CANVAS_X + CANVAS_WIDTH + 10), (s16)(CANVAS_Y + 92), 64, 14,
               "Clear", ID_CLEAR);
    gui_button((s16)(CANVAS_X + CANVAS_WIDTH + 10), (s16)(CANVAS_Y + 108), 64, 14,
               "Save", ID_SAVE);

    gui_message_bar(message);
}

static void paint_event(const Event *event) {
    if (event->type == EVENT_CLICK) {
        if (event->key >= ID_COLOR_FIRST && event->key < (u16)(ID_COLOR_FIRST + 16u)) {
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
                text_append(message, "Stored on the RAM disk, ", (u16)sizeof(message));
                text_append_number(message, sectors, (u16)sizeof(message));
                text_append(message, " sectors", (u16)sizeof(message));
            } else {
                text_copy(message, "The RAM disk is full", (u16)sizeof(message));
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
    "PNT",
    "Paint",
    icon,
    COLOR_LIGHT_MAGENTA,
    paint_open,
    paint_draw,
    paint_event,
    0
};
