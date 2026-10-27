/*
 * app_mines.c -- minesweeper.
 *
 * Left click opens a square. A touch screen has no right button, so holding a
 * square down (or switching the FLAG button on) marks it instead.
 */

#include "dimos.h"

#define GRID 9
#define MINES 10
/* The board is as wide as the phone and as tall as it needs to be; the
 * counters and buttons sit underneath it, within thumb reach. */
#define CELL (s16)(APP_WIDTH / GRID)
#define BOARD_WIDTH (GRID * CELL)
#define BOARD_HEIGHT (GRID * CELL)
#define BOARD_X (s16)(APP_LEFT + (APP_WIDTH - BOARD_WIDTH) / 2)
#define BOARD_Y 74
#define PANEL_Y (s16)(BOARD_Y + BOARD_HEIGHT + 20)
#define BOARD_SIDE_X (s16)APP_LEFT

#define HIDDEN 0u
#define OPENED 1u
#define MARKED 2u

#define ID_BOARD 1u
#define ID_FLAG_MODE 2u
#define ID_NEW_GAME 3u
#define ID_HELP 4u

static const char *const icon[16] = {
    "................",
    "...........XX...",
    "..........XX....",
    ".........XX.....",
    "......XXXX......",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "..XXXXXXoXXXXX..",
    "..XXXXoXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    "......XXXX......",
    "................",
    "................",
};

static u8 square_state[GRID * GRID];
static u8 has_mine[GRID * GRID];
static u8 neighbours[GRID * GRID];
static u16 opened_count;
static u16 flags_used;
static u8 flag_mode;
static u8 game_over;
static u8 won;
static u32 random_state;

static u32 next_random(void) {
    random_state ^= random_state << 13u;
    random_state ^= random_state >> 17u;
    random_state ^= random_state << 5u;
    if (random_state == 0u) {
        random_state = 0x5EEDu;
    }
    return random_state;
}

static void mines_start(void) {
    u16 index;
    u16 placed = 0u;
    u8 row;
    u8 column;

    random_state = (u32)time_milliseconds() ^ 0x1234ABCDu;
    opened_count = 0u;
    flags_used = 0u;
    game_over = 0u;
    won = 0u;

    for (index = 0u; index < GRID * GRID; ++index) {
        square_state[index] = HIDDEN;
        has_mine[index] = 0u;
        neighbours[index] = 0u;
    }

    while (placed < MINES) {
        const u16 cell = (u16)(next_random() % (u32)(GRID * GRID));

        if (has_mine[cell] == 0u) {
            has_mine[cell] = 1u;
            ++placed;
        }
    }

    for (row = 0u; row < GRID; ++row) {
        for (column = 0u; column < GRID; ++column) {
            s8 delta_row;
            s8 delta_column;
            u8 count = 0u;

            for (delta_row = -1; delta_row <= 1; ++delta_row) {
                for (delta_column = -1; delta_column <= 1; ++delta_column) {
                    const s16 r = (s16)(row + delta_row);
                    const s16 c = (s16)(column + delta_column);

                    if (r < 0 || c < 0 || r >= GRID || c >= GRID) {
                        continue;
                    }
                    if (has_mine[(u16)(r * GRID + c)] != 0u) {
                        ++count;
                    }
                }
            }
            neighbours[row * GRID + column] = count;
        }
    }
}

static void mines_open(void) {
    flag_mode = 0u;
    mines_start();
}

/* Opening an empty square opens everything connected to it. The stack of
 * squares still to look at is small: the board only has 81 of them. */
static void open_square(u16 start) {
    u16 pending[GRID * GRID];
    u16 pending_count = 0u;

    if (square_state[start] != HIDDEN) {
        return;
    }
    pending[pending_count++] = start;

    while (pending_count != 0u) {
        const u16 cell = pending[--pending_count];
        const u16 row = (u16)(cell / GRID);
        const u16 column = (u16)(cell % GRID);
        s16 delta_row;
        s16 delta_column;

        if (square_state[cell] != HIDDEN) {
            continue;
        }
        square_state[cell] = OPENED;
        ++opened_count;

        if (neighbours[cell] != 0u) {
            continue;
        }
        for (delta_row = -1; delta_row <= 1; ++delta_row) {
            for (delta_column = -1; delta_column <= 1; ++delta_column) {
                const s16 r = (s16)(row + delta_row);
                const s16 c = (s16)(column + delta_column);

                if (r < 0 || c < 0 || r >= GRID || c >= GRID) {
                    continue;
                }
                if (square_state[(u16)(r * GRID + c)] == HIDDEN) {
                    pending[pending_count++] = (u16)(r * GRID + c);
                }
            }
        }
    }
}

static u8 number_color(u8 count) {
    switch (count) {
        case 1u: return COLOR_SELECTION;
        case 2u: return COLOR_GOOD;
        case 3u: return COLOR_ALERT;
        case 4u: return COLOR_IOS_INDIGO;
        case 5u: return COLOR_BROWN;
        case 6u: return COLOR_IOS_TEAL;
        case 7u: return COLOR_DEEP;
        default: return COLOR_DISABLED;
    }
}

static void mines_overlay(const char *title, const char *note, u8 color) {
    const s16 x = (s16)(BOARD_X + (BOARD_WIDTH - 200) / 2);
    const s16 y = (s16)(BOARD_Y + (BOARD_HEIGHT - 78) / 2);

    gfx_round_fill(x, y, 200, 78, 16, COLOR_FACE);
    gfx_text_size_centered(x, (s16)(y + 14), 200, title, color, 18);
    gfx_text_size_centered(x, (s16)(y + 44), 200, note, COLOR_DISABLED, 9);
}

static void mines_draw(void) {
    u16 row;
    u16 column;
    char buffer[20];

    gui_window_frame("Mines");

    for (row = 0u; row < GRID; ++row) {
        for (column = 0u; column < GRID; ++column) {
            const u16 cell = (u16)(row * GRID + column);
            const s16 x = (s16)(BOARD_X + (s16)(column * CELL));
            const s16 y = (s16)(BOARD_Y + (s16)(row * CELL));

            if (square_state[cell] == HIDDEN ||
                square_state[cell] == MARKED) {
                gfx_round_fill((s16)(x + 1), (s16)(y + 1), CELL - 3, CELL - 3,
                               6, COLOR_FACE);
                if (square_state[cell] == MARKED) {
                    /* A little flag: pole, cloth and a foot. */
                    gfx_fill((s16)(x + 8), (s16)(y + 7), 2, 12, COLOR_DEEP);
                    gfx_fill((s16)(x + 10), (s16)(y + 7), 8, 6, COLOR_ALERT);
                    gfx_fill((s16)(x + 6), (s16)(y + 19), 9, 2, COLOR_DEEP);
                } else {
                    gfx_circle((s16)(x + CELL / 2), (s16)(y + CELL / 2), 2,
                               COLOR_GRID, 1u);
                }
            } else {
                gfx_round_fill((s16)(x + 1), (s16)(y + 1), CELL - 3, CELL - 3,
                               5, COLOR_PANEL);
                if (has_mine[cell] != 0u) {
                    gfx_circle((s16)(x + CELL / 2), (s16)(y + CELL / 2), 6,
                               COLOR_DEEP, 1u);
                } else if (neighbours[cell] != 0u) {
                    char digit[2];

                    digit[0] = (char)('0' + (char)neighbours[cell]);
                    digit[1] = '\0';
                    gfx_text_size_centered(x, (s16)(y + 6), CELL, digit,
                                           number_color(neighbours[cell]), 15);
                }
            }
        }
    }
    gui_hotspot(BOARD_X, BOARD_Y, (s16)BOARD_WIDTH, (s16)BOARD_HEIGHT,
                ID_BOARD);

    gfx_text_size(BOARD_SIDE_X, (s16)(PANEL_Y - 4), "MINES LEFT",
                  COLOR_DISABLED, 9);
    buffer[0] = '\0';
    text_append_number(buffer, (u32)(MINES - flags_used), (u16)sizeof(buffer));
    gfx_text_size((s16)(BOARD_SIDE_X + 90), (s16)(PANEL_Y - 12), buffer,
                  COLOR_DEEP, 32);

    gui_switch(BOARD_SIDE_X, (s16)(PANEL_Y + 30), (s16)APP_WIDTH, 40,
               "Flag mode", ID_FLAG_MODE, flag_mode);
    gui_button(BOARD_SIDE_X, (s16)(PANEL_Y + 82), (s16)APP_WIDTH, 48,
               "New game", ID_NEW_GAME);
    gui_button_colored(BOARD_SIDE_X, (s16)(PANEL_Y + 140), (s16)APP_WIDTH, 44,
                       "How to play", ID_HELP, COLOR_FACE, COLOR_DEEP);

    if (won != 0u) {
        mines_overlay("CLEARED", "Every safe square is open", COLOR_GOOD);
        return;
    }
    if (game_over != 0u) {
        mines_overlay("BOOM", "Tap New game to retry", COLOR_ALERT);
        return;
    }
    gui_message_bar("Tap a square, or hold one to flag it");
}

static void mines_event(const Event *event) {
    u16 cell;
    u16 column;
    u16 row;

    if (event->type == EVENT_CLICK && event->key == ID_NEW_GAME) {
        mines_start();
        return;
    }
    if (event->type == EVENT_CLICK && event->key == ID_FLAG_MODE) {
        flag_mode = (u8)(flag_mode ^ 1u);
        return;
    }
    if (event->type == EVENT_CLICK && event->key == ID_HELP) {
        gui_message_bar("Open all 71 safe squares to win");
        return;
    }

    if (event->type != EVENT_CLICK && event->type != EVENT_RIGHT_CLICK) {
        return;
    }
    if (event->key != ID_BOARD || game_over != 0u || won != 0u) {
        return;
    }

    column = (u16)((event->x - BOARD_X) / CELL);
    row = (u16)((event->y - BOARD_Y) / CELL);
    if (column >= GRID || row >= GRID) {
        return;
    }
    cell = (u16)(row * GRID + column);

    if (event->type == EVENT_RIGHT_CLICK || flag_mode != 0u) {
        if (square_state[cell] == HIDDEN) {
            square_state[cell] = MARKED;
            ++flags_used;
            sound_beep();
        } else if (square_state[cell] == MARKED) {
            square_state[cell] = HIDDEN;
            --flags_used;
        }
        return;
    }

    if (square_state[cell] == MARKED) {
        square_state[cell] = HIDDEN;
        --flags_used;
        return;
    }
    if (has_mine[cell] != 0u) {
        u16 index;

        game_over = 1u;
        sound_alert();
        for (index = 0u; index < GRID * GRID; ++index) {
            if (has_mine[index] != 0u) {
                square_state[index] = OPENED;
            }
        }
        return;
    }

    open_square(cell);
    if (opened_count == (u16)(GRID * GRID - MINES)) {
        won = 1u;
        sound_play_startup();
    }
}

const Application application_mines = {
    "Mines",
    "Mines",
    icon,
    COLOR_IOS_INDIGO,
    mines_open,
    mines_draw,
    mines_event,
    0
};
