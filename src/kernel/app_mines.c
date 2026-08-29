/*
 * app_mines.c -- minesweeper.
 *
 * Left click opens a square. A touch screen has no right button, so holding a
 * square down (or switching the FLAG button on) marks it instead.
 */

#include "dimos.h"

#define GRID 9
#define MINES 10
#define CELL 14
#define BOARD_WIDTH (GRID * CELL)
#define BOARD_HEIGHT (GRID * CELL)
#define BOARD_X (s16)(WINDOW_LEFT + 4)
#define BOARD_Y (s16)(WINDOW_TOP + 4)

#define HIDDEN 0u
#define OPENED 1u
#define MARKED 2u

#define ID_BOARD 1u
#define ID_FLAG_MODE 2u
#define ID_NEW_GAME 3u
#define ID_HELP 4u

static const char *const icon[16] = {
    "................",
    ".......XX.......",
    ".......XX.......",
    "..XX.XXXXXX.XX..",
    "...XX.XXXX.XX...",
    "....XXXXXXXX....",
    "...XXXXXXXXXX...",
    "..XXoXXXXXXoXX..",
    "...XXXXXXXXXX...",
    "....XXXXXXXX....",
    "...XX.XXXX.XX...",
    "..XX.XXXXXX.XX..",
    ".......XX.......",
    ".......XX.......",
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
        case 1u: return COLOR_BLUE;
        case 2u: return COLOR_GREEN;
        case 3u: return COLOR_RED;
        case 4u: return COLOR_MAGENTA;
        case 5u: return COLOR_BROWN;
        case 6u: return COLOR_CYAN;
        case 7u: return COLOR_BLACK;
        default: return COLOR_DARK_GRAY;
    }
}

static void mines_draw(void) {
    u16 row;
    u16 column;
    char buffer[20];

    for (row = 0u; row < GRID; ++row) {
        for (column = 0u; column < GRID; ++column) {
            const u16 cell = (u16)(row * GRID + column);
            const s16 x = (s16)(BOARD_X + (s16)(column * CELL));
            const s16 y = (s16)(BOARD_Y + (s16)(row * CELL));

            if (square_state[cell] == HIDDEN || square_state[cell] == MARKED) {
                gfx_fill(x, y, CELL, CELL, COLOR_FACE);
                gfx_raised_box(x, y, CELL, CELL, 1u);
                if (square_state[cell] == MARKED) {
                    gfx_text((s16)(x + 3), (s16)(y + 3), "F", COLOR_RED);
                }
            } else {
                gfx_fill(x, y, CELL, CELL, COLOR_PANEL);
                gfx_outline(x, y, CELL, CELL, COLOR_SHADOW);
                if (has_mine[cell] != 0u) {
                    gfx_circle((s16)(x + CELL / 2), (s16)(y + CELL / 2), 4,
                               COLOR_BLACK, 1u);
                } else if (neighbours[cell] != 0u) {
                    char digit[2];

                    digit[0] = (char)('0' + (char)neighbours[cell]);
                    digit[1] = '\0';
                    gfx_text((s16)(x + 3), (s16)(y + 3), digit,
                             number_color(neighbours[cell]));
                }
            }
        }
    }
    gfx_outline((s16)(BOARD_X - 2), (s16)(BOARD_Y - 2), (s16)(BOARD_WIDTH + 4),
                (s16)(BOARD_HEIGHT + 4), COLOR_SHADOW);
    gui_hotspot(BOARD_X, BOARD_Y, (s16)BOARD_WIDTH, (s16)BOARD_HEIGHT, ID_BOARD);

    buffer[0] = '\0';
    text_append(buffer, "Mines left ", (u16)sizeof(buffer));
    text_append_number(buffer, (u32)(MINES - flags_used), (u16)sizeof(buffer));
    gfx_text((s16)(BOARD_X + BOARD_WIDTH + 12), (s16)(BOARD_Y), buffer, COLOR_BLACK);

    gui_switch((s16)(BOARD_X + BOARD_WIDTH + 12), (s16)(BOARD_Y + 16), 100, 16,
               "Flag mode", ID_FLAG_MODE, flag_mode);
    gui_button((s16)(BOARD_X + BOARD_WIDTH + 12), (s16)(BOARD_Y + 40), 100, 16,
               "New game", ID_NEW_GAME);
    gui_button((s16)(BOARD_X + BOARD_WIDTH + 12), (s16)(BOARD_Y + 62), 100, 16,
               "How to play", ID_HELP);

    if (won != 0u) {
        gfx_fill((s16)(BOARD_X + 20), (s16)(BOARD_Y + 46), 100, 24, COLOR_FACE);
        gfx_outline((s16)(BOARD_X + 20), (s16)(BOARD_Y + 46), 100, 24, COLOR_BLACK);
        gfx_text_centered((s16)(BOARD_X + 20), (s16)(BOARD_Y + 50), 100, "CLEARED!",
                          COLOR_GOOD);
        gfx_text_centered((s16)(BOARD_X + 20), (s16)(BOARD_Y + 60), 100,
                          "New game to play on", COLOR_BLACK);
        return;
    }
    if (game_over != 0u) {
        gfx_fill((s16)(BOARD_X + 20), (s16)(BOARD_Y + 46), 100, 24, COLOR_FACE);
        gfx_outline((s16)(BOARD_X + 20), (s16)(BOARD_Y + 46), 100, 24, COLOR_BLACK);
        gfx_text_centered((s16)(BOARD_X + 20), (s16)(BOARD_Y + 50), 100, "BOOM",
                          COLOR_RED);
        gfx_text_centered((s16)(BOARD_X + 20), (s16)(BOARD_Y + 60), 100,
                          "New game to retry", COLOR_BLACK);
        return;
    }
    gui_message_bar("Click a square; hold it down to mark a mine");
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
    "MIN",
    "Mines",
    icon,
    COLOR_RED,
    mines_open,
    mines_draw,
    mines_event,
    0
};
