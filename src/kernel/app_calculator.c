/*
 * app_calculator.c -- a pocket calculator with iOS key styling.
 *
 * Every key is a rounded button, which makes this the one application that
 * never needed a keyboard in the first place. The arithmetic is the same
 * whole-number calculator the small DimOS always had.
 */

#include "dimos.h"

#define KEY_WIDTH 84
#define KEY_HEIGHT 46
#define KEY_STEP_X (KEY_WIDTH + 12)
#define KEY_STEP_Y (KEY_HEIGHT + 10)
#define KEY_LEFT (s16)((SCREEN_WIDTH - (4 * KEY_STEP_X + 12)) / 2)
#define KEY_TOP 146

#define ID_DIGIT_FIRST 1u   /* 1 .. 10 for '0' .. '9' */
#define ID_CLEAR 20u
#define ID_BACK 21u
#define ID_NEGATE 22u
#define ID_DOT 23u
#define ID_ADD 30u
#define ID_SUBTRACT 31u
#define ID_MULTIPLY 32u
#define ID_DIVIDE 33u
#define ID_EQUALS 34u

static const char *const icon[16] = {
    "................",
    "..XXXXXXXXXXXX..",
    "..XooooooooooX..",
    "..XoXXXXXXXXoX..",
    "..XoXXXXXXXXoX..",
    "..XooooooooooX..",
    "..XXXXXXXXXXXX..",
    "..XX.XX.XX.XXX..",
    "..XX.XX.XX.XXX..",
    "..XX.XX.XX.XXX..",
    "..XX.XX.XX.XXX..",
    "..XX.XX.XX.XXX..",
    "..XX.XX.XX.XXX..",
    "..XXXXXXXXXXXX..",
    "................",
    "................",
};

static s32 accumulator;
static s32 current;
static u8 pending_operator;
static u8 entering;
static u8 failed;
static char display[16];

static void show_display(void) {
    u32 shown;

    display[0] = '\0';
    if (failed != 0u) {
        text_copy(display, "Error", (u16)sizeof(display));
        return;
    }
    if (current < 0) {
        text_append_character(display, '-', (u16)sizeof(display));
        shown = (u32)-current;
    } else {
        shown = (u32)current;
    }
    text_append_number(display, shown, (u16)sizeof(display));
}

static void calculator_reset(void) {
    accumulator = 0;
    current = 0;
    pending_operator = 0u;
    entering = 0u;
    failed = 0u;
    show_display();
}

static void calculator_open(void) {
    calculator_reset();
}

static void apply_pending(void) {
    switch (pending_operator) {
        case (u8)'+':
            accumulator += current;
            break;
        case (u8)'-':
            accumulator -= current;
            break;
        case (u8)'*':
            accumulator *= current;
            break;
        case (u8)'/':
            if (current == 0) {
                failed = 1u;
            } else {
                accumulator /= current;
            }
            break;
        default:
            accumulator = current;
            break;
    }
    current = accumulator;
}

static void press_operator(u8 symbol) {
    if (failed != 0u) {
        return;
    }
    if (pending_operator != 0u && entering != 0u) {
        apply_pending();
    } else {
        accumulator = current;
    }
    pending_operator = symbol;
    entering = 0u;
    show_display();
}

static void press_digit(u8 digit) {
    if (failed != 0u) {
        calculator_reset();
    }
    if (entering == 0u) {
        current = 0;
        entering = 1u;
    }
    if (current > 200000000 || current < -200000000) {
        failed = 1u;
        show_display();
        return;
    }
    current = (s32)(current * 10) + (s32)digit;
    show_display();
}

/* One rounded iOS-style key: numbers are graphite, operators orange and
 * the top row a quiet light grey. */
static void key_face(s16 x, s16 y, const char *label, u16 id, u8 face,
                     u8 ink) {
    const u8 pressed = gui_is_pressed(id);

    gfx_round_fill(x, y, KEY_WIDTH, KEY_HEIGHT, 14,
                   (pressed != 0u) ? gfx_shade(face, COLOR_FACE, 3) : face);
    gfx_text_size_centered(x, (s16)(y + (KEY_HEIGHT - 16) / 2), KEY_WIDTH,
                           label, ink, 16);
    gui_hotspot(x, y, KEY_WIDTH, KEY_HEIGHT, id);
}

static void calculator_draw(void) {
    static const char *const labels[4][5] = {
        { "7", "8", "9", "/", "C" },
        { "4", "5", "6", "*", "<-" },
        { "1", "2", "3", "-", "+/-" },
        { "0", ".", "=", "+", "x" }
    };
    /* A digit key carries the id "digit + 1", so '0' is 1 and '9' is 10. */
    static const u16 ids[4][5] = {
        { 8u, 9u, 10u, ID_DIVIDE, ID_CLEAR },
        { 5u, 6u, 7u, ID_MULTIPLY, ID_BACK },
        { 2u, 3u, 4u, ID_SUBTRACT, ID_NEGATE },
        { 1u, ID_DOT, ID_EQUALS, ID_ADD, 0u }
    };
    const u8 digit_face = gfx_shade(COLOR_FACE, COLOR_BLACK, 3);
    u8 row;
    u8 column;

    gui_window_frame_dark("Calculator", COLOR_WIN_BG);

    /* The display: large, right aligned, like a real calculator. */
    gfx_text_size_right((s16)APP_RIGHT, 88, display, COLOR_FACE, 44);
    gfx_text_size_right((s16)APP_RIGHT, 132,
                        (pending_operator != 0u) ? "pending" : "",
                        COLOR_DISABLED, 9);

    for (row = 0u; row < 4u; ++row) {
        for (column = 0u; column < 5u; ++column) {
            const s16 x = (s16)(KEY_LEFT + (s16)(column * KEY_STEP_X));
            const s16 y = (s16)(KEY_TOP + (s16)(row * KEY_STEP_Y));
            const u16 id = ids[row][column];

            if (id == 0u) {
                continue; /* the bottom right corner stays empty */
            }
            if (id <= 10u) {
                key_face(x, y, labels[row][column], id, digit_face, COLOR_FACE);
            } else if (id == ID_ADD || id == ID_SUBTRACT ||
                       id == ID_MULTIPLY || id == ID_DIVIDE ||
                       id == ID_EQUALS) {
                key_face(x, y, labels[row][column], id, COLOR_ACCENT,
                         COLOR_FACE);
            } else {
                key_face(x, y, labels[row][column], id, COLOR_SHADOW,
                         COLOR_DEEP);
            }
        }
    }
}

static void calculator_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }

    if (event->key <= 10u && event->key >= 1u) {
        press_digit((u8)(event->key - 1u));
        sound_beep();
        return;
    }
    switch (event->key) {
        case ID_ADD:
            press_operator((u8)'+');
            break;
        case ID_SUBTRACT:
            press_operator((u8)'-');
            break;
        case ID_MULTIPLY:
            press_operator((u8)'*');
            break;
        case ID_DIVIDE:
            press_operator((u8)'/');
            break;
        case ID_EQUALS:
            if (pending_operator != 0u) {
                apply_pending();
                pending_operator = 0u;
                entering = 0u;
                show_display();
            }
            break;
        case ID_CLEAR:
            calculator_reset();
            break;
        case ID_BACK:
            current /= 10;
            show_display();
            break;
        case ID_NEGATE:
            current = -current;
            show_display();
            break;
        case ID_DOT:
            sound_beep();
            break;
        default:
            return;
    }
    sound_beep();
}

const Application application_calculator = {
    "Calculator",
    "Calculator",
    icon,
    COLOR_ACCENT,
    calculator_open,
    calculator_draw,
    calculator_event,
    0
};
