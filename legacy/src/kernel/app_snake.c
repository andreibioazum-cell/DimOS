/*
 * app_snake.c -- the snake game.
 *
 * The snake can be steered three ways: with the on-screen direction pad,
 * with the arrow keys, or by tapping anywhere in the field, which turns
 * the snake towards the tap. The board is a rounded card in the style of
 * a phone game.
 */

#include "dimos.h"

#define CELL 8
#define FIELD_COLUMNS 42
#define FIELD_ROWS 25
#define FIELD_WIDTH (FIELD_COLUMNS * CELL)
#define FIELD_HEIGHT (FIELD_ROWS * CELL)
#define FIELD_X (s16)APP_LEFT
#define FIELD_Y 74
#define SIDE_X (s16)(FIELD_X + FIELD_WIDTH + 36)
#define SNAKE_LIMIT 300u
#define STEP_MILLISECONDS 140u

#define ID_UP 1u
#define ID_DOWN 2u
#define ID_LEFT 3u
#define ID_RIGHT 4u
#define ID_NEW_GAME 5u
#define ID_FIELD 6u

enum { GOING_UP = 0u, GOING_RIGHT = 1u, GOING_DOWN = 2u, GOING_LEFT = 3u };

static const char *const icon[16] = {
    "................",
    "...XXXXXXX......",
    "..XXXXXXXXX.....",
    "..XX.....XXX....",
    "..XX......XX....",
    "...XXX....XX....",
    ".....XX...XX....",
    "......XX.XX.....",
    ".......XXXX.....",
    ".......XX.......",
    "......XX........",
    ".....XX.........",
    "....XXXXXX......",
    "...XXXXXXXX.....",
    "....XXXXXX......",
    "................",
};

static u16 cells[SNAKE_LIMIT];
static u16 length;
static u16 score;
static u8 direction;
static u8 wanted_direction;
static u8 alive;
static u16 step_counter;
static u32 random_state;

static u32 next_random(void) {
    random_state ^= random_state << 13u;
    random_state ^= random_state >> 17u;
    random_state ^= random_state << 5u;
    if (random_state == 0u) {
        random_state = 0xD14E05u;
    }
    return random_state;
}

static u8 cell_is_snake(u16 cell) {
    u16 index;

    for (index = 0u; index < length; ++index) {
        if (cells[index] == cell) {
            return 1u;
        }
    }
    return 0u;
}

static u16 food_cell;

static void place_food(void) {
    u16 attempt;

    for (attempt = 0u; attempt < 400u; ++attempt) {
        const u16 candidate = (u16)(next_random() % (u32)(FIELD_COLUMNS * FIELD_ROWS));

        if (cell_is_snake(candidate) == 0u) {
            food_cell = candidate;
            return;
        }
    }
    food_cell = 0u;
}

static void snake_start(void) {
    u16 index;
    u16 head = (u16)(FIELD_ROWS / 2u * FIELD_COLUMNS + FIELD_COLUMNS / 2u);

    random_state = (u32)time_milliseconds() ^ 0x9E3779B9u;
    length = 5u;
    score = 0u;
    direction = GOING_RIGHT;
    wanted_direction = GOING_RIGHT;
    alive = 1u;
    step_counter = 0u;

    for (index = 0u; index < length; ++index) {
        cells[index] = (u16)(head - index);
    }
    place_food();
    gui_reserve_arrow_keys(1u);
}

static void snake_open(void) {
    snake_start();
}

static void snake_step(void) {
    u16 head = cells[0];
    const u16 row = (u16)(head / FIELD_COLUMNS);
    const u16 column = (u16)(head % FIELD_COLUMNS);
    u16 next_row = row;
    u16 next_column = column;
    u16 next;
    u16 index;
    u8 grew = 0u;

    direction = wanted_direction;
    if (direction == GOING_UP && row != 0u) {
        next_row = (u16)(row - 1u);
    } else if (direction == GOING_DOWN && row + 1u < FIELD_ROWS) {
        next_row = (u16)(row + 1u);
    } else if (direction == GOING_LEFT && column != 0u) {
        next_column = (u16)(column - 1u);
    } else if (direction == GOING_RIGHT && column + 1u < FIELD_COLUMNS) {
        next_column = (u16)(column + 1u);
    } else {
        alive = 0u; /* ran into the wall */
        sound_alert();
        return;
    }

    next = (u16)(next_row * FIELD_COLUMNS + next_column);
    if (next == food_cell) {
        grew = 1u;
        score = (u16)(score + 10u);
        sound_beep();
    } else if (cell_is_snake(next) != 0u) {
        alive = 0u;
        sound_alert();
        return;
    }

    if (grew == 0u) {
        index = (u16)(length - 1u);
        while (index != 0u) {
            cells[index] = cells[index - 1u];
            --index;
        }
    } else if (length < SNAKE_LIMIT) {
        index = length;
        while (index != 0u) {
            cells[index] = cells[index - 1u];
            --index;
        }
        ++length;
    } else {
        index = (u16)(length - 1u);
        while (index != 0u) {
            cells[index] = cells[index - 1u];
            --index;
        }
    }
    cells[0] = next;

    if (grew != 0u) {
        place_food();
    }
}

static void snake_update(u16 elapsed) {
    if (alive == 0u) {
        return;
    }
    step_counter = (u16)(step_counter + elapsed);
    while (step_counter >= STEP_MILLISECONDS) {
        step_counter = (u16)(step_counter - STEP_MILLISECONDS);
        snake_step();
        if (alive == 0u) {
            return;
        }
    }
}

static void dpad_key(s16 x, s16 y, const char *label, u16 id) {
    const u8 pressed = gui_is_pressed(id);

    gfx_round_fill(x, y, 56, 50, 14,
                   (pressed != 0u) ? COLOR_SELECTION : COLOR_FACE);
    gfx_text_size_centered(x, (s16)(y + 17), 56, label,
                           (pressed != 0u) ? COLOR_FACE : COLOR_DEEP, 13);
    gui_hotspot(x, y, 56, 50, id);
}

static void snake_draw(void) {
    u16 index;
    char buffer[16];

    gui_window_frame("Snake");

    /* The field: a deep green rounded board with a subtle grid. */
    gfx_round_fill(FIELD_X - 6, FIELD_Y - 6, (s16)(FIELD_WIDTH + 12),
                   (s16)(FIELD_HEIGHT + 12), 16,
                   gfx_shade(COLOR_GOOD, COLOR_BLACK, 12));
    gfx_fill(FIELD_X, FIELD_Y, (s16)FIELD_WIDTH, (s16)FIELD_HEIGHT,
             gfx_shade(COLOR_GOOD, COLOR_BLACK, 10));

    for (index = 0u; index < FIELD_COLUMNS; ++index) {
        if ((index % 3u) == 0u) {
            gfx_vertical_line((s16)(FIELD_X + (s16)index * CELL), FIELD_Y,
                              (s16)FIELD_HEIGHT,
                              gfx_shade(COLOR_GOOD, COLOR_BLACK, 6));
        }
    }
    for (index = 0u; index < FIELD_ROWS; ++index) {
        if ((index % 3u) == 0u) {
            gfx_horizontal_line(FIELD_X, (s16)(FIELD_Y + (s16)index * CELL),
                                (s16)FIELD_WIDTH,
                                gfx_shade(COLOR_GOOD, COLOR_BLACK, 6));
        }
    }

    /* The apple. */
    gfx_round_fill((s16)(FIELD_X + (s16)(food_cell % FIELD_COLUMNS) * CELL + 1),
                   (s16)(FIELD_Y + (s16)(food_cell / FIELD_COLUMNS) * CELL + 1),
                   CELL - 2, CELL - 2, (CELL - 2) / 2, COLOR_ALERT);

    for (index = 0u; index < length; ++index) {
        const u8 color = (index == 0u) ? COLOR_FACE : COLOR_WIN_LIGHT;

        gfx_round_fill((s16)(FIELD_X + (s16)(cells[index] % FIELD_COLUMNS) * CELL + 1),
                       (s16)(FIELD_Y + (s16)(cells[index] / FIELD_COLUMNS) * CELL + 1),
                       CELL - 2, CELL - 2, (CELL - 2) / 2, color);
    }

    /* Score and length, as big phone-game numbers. */
    gfx_text_size(SIDE_X, 70, "SCORE", COLOR_DISABLED, 9);
    buffer[0] = '\0';
    text_append_number(buffer, score, (u16)sizeof(buffer));
    gfx_text_size(SIDE_X, 82, buffer, COLOR_DEEP, 34);

    gfx_text_size((s16)(SIDE_X + 140), 70, "LENGTH", COLOR_DISABLED, 9);
    buffer[0] = '\0';
    text_append_number(buffer, length, (u16)sizeof(buffer));
    gfx_text_size((s16)(SIDE_X + 140), 82, buffer, COLOR_DEEP, 34);

    /* A proper direction pad. */
    dpad_key((s16)(SIDE_X + 66), 150, "^", ID_UP);
    dpad_key(SIDE_X, 208, "<", ID_LEFT);
    dpad_key((s16)(SIDE_X + 66), 208, "v", ID_DOWN);
    dpad_key((s16)(SIDE_X + 132), 208, ">", ID_RIGHT);

    gui_button((s16)(SIDE_X + 66), 288, 122, 44, "New game", ID_NEW_GAME);

    gui_hotspot(FIELD_X, FIELD_Y, (s16)FIELD_WIDTH, (s16)FIELD_HEIGHT, ID_FIELD);

    if (alive == 0u) {
        gfx_round_fill((s16)(FIELD_X + 68), (s16)(FIELD_Y + 66), 200, 68, 16,
                       COLOR_FACE);
        gfx_text_size_centered((s16)(FIELD_X + 68), (s16)(FIELD_Y + 76), 200,
                               "GAME OVER", COLOR_ALERT, 18);
        gfx_text_size_centered((s16)(FIELD_X + 68), (s16)(FIELD_Y + 104), 200,
                               "Tap New game to retry", COLOR_DISABLED, 9);
    } else {
        gui_message_bar("Arrows, the direction pad, or tap the field");
    }
}

static void steer_towards(s16 x, s16 y) {
    const s16 head_x = (s16)(FIELD_X + (s16)(cells[0] % FIELD_COLUMNS) * CELL);
    const s16 head_y = (s16)(FIELD_Y + (s16)(cells[0] / FIELD_COLUMNS) * CELL);
    const s16 delta_x = (s16)(x - head_x);
    const s16 delta_y = (s16)(y - head_y);
    const s16 size_x = (s16)(delta_x < 0 ? -delta_x : delta_x);
    const s16 size_y = (s16)(delta_y < 0 ? -delta_y : delta_y);

    if (size_x > size_y) {
        if (delta_x > 0 && direction != GOING_LEFT) {
            wanted_direction = GOING_RIGHT;
        } else if (delta_x < 0 && direction != GOING_RIGHT) {
            wanted_direction = GOING_LEFT;
        }
        return;
    }
    if (delta_y > 0 && direction != GOING_UP) {
        wanted_direction = GOING_DOWN;
    } else if (delta_y < 0 && direction != GOING_DOWN) {
        wanted_direction = GOING_UP;
    }
}

static void snake_event(const Event *event) {
    if (event->type == EVENT_CLICK) {
        if (event->key == ID_NEW_GAME) {
            snake_start();
            return;
        }
        if (event->key == ID_UP && direction != GOING_DOWN) {
            wanted_direction = GOING_UP;
        } else if (event->key == ID_DOWN && direction != GOING_UP) {
            wanted_direction = GOING_DOWN;
        } else if (event->key == ID_LEFT && direction != GOING_RIGHT) {
            wanted_direction = GOING_LEFT;
        } else if (event->key == ID_RIGHT && direction != GOING_LEFT) {
            wanted_direction = GOING_RIGHT;
        } else if (event->key == ID_FIELD) {
            steer_towards(event->x, event->y);
        }
        return;
    }

    if (event->type == EVENT_KEY && alive != 0u) {
        if ((event->key == KEY_ARROW_UP || event->key == (u16)'w') &&
            direction != GOING_DOWN) {
            wanted_direction = GOING_UP;
        } else if ((event->key == KEY_ARROW_DOWN || event->key == (u16)'s') &&
                   direction != GOING_UP) {
            wanted_direction = GOING_DOWN;
        } else if ((event->key == KEY_ARROW_LEFT || event->key == (u16)'a') &&
                   direction != GOING_RIGHT) {
            wanted_direction = GOING_LEFT;
        } else if ((event->key == KEY_ARROW_RIGHT || event->key == (u16)'d') &&
                   direction != GOING_LEFT) {
            wanted_direction = GOING_RIGHT;
        } else if (event->key == (u16)'n' || event->key == KEY_ENTER) {
            snake_start();
        }
    }
}

const Application application_snake = {
    "Snake",
    "Snake",
    icon,
    COLOR_GOOD,
    snake_open,
    snake_draw,
    snake_event,
    snake_update
};
