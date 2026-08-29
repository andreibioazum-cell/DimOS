/*
 * app_snake.c -- the snake game.
 *
 * The snake can be steered three ways: with the four buttons on the screen,
 * with the arrow keys, or by clicking anywhere in the field, which turns the
 * snake towards the click.
 */

#include "dimos.h"

#define CELL 4
#define FIELD_COLUMNS 42
#define FIELD_ROWS 25
#define FIELD_WIDTH (FIELD_COLUMNS * CELL)
#define FIELD_HEIGHT (FIELD_ROWS * CELL)
#define FIELD_X (s16)(WINDOW_LEFT + 4)
#define FIELD_Y (s16)(WINDOW_TOP + 4)
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
    "..........XX....",
    ".........XXXX...",
    "........XXoXX...",
    ".......XXXX.....",
    "......XXXX......",
    ".....XXXX.......",
    "....XXXX........",
    "...XXXX.........",
    "..XXXX..........",
    "..XXX...........",
    "................",
    "................",
    "................",
    "................",
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

static void snake_draw(void) {
    const s16 right = (s16)(FIELD_X + FIELD_WIDTH);
    u16 index;
    char buffer[16];

    gfx_fill(FIELD_X, FIELD_Y, (s16)FIELD_WIDTH, (s16)FIELD_HEIGHT, COLOR_DEEP);
    gfx_outline((s16)(FIELD_X - 1), (s16)(FIELD_Y - 1), (s16)(FIELD_WIDTH + 2),
                (s16)(FIELD_HEIGHT + 2), COLOR_SHADOW);

    gfx_fill((s16)(FIELD_X + (s16)(food_cell % FIELD_COLUMNS) * CELL),
             (s16)(FIELD_Y + (s16)(food_cell / FIELD_COLUMNS) * CELL),
             CELL, CELL, COLOR_RED);

    for (index = 0u; index < length; ++index) {
        const u8 color = (index == 0u) ? COLOR_LIGHT_GREEN : COLOR_GREEN;

        gfx_fill((s16)(FIELD_X + (s16)(cells[index] % FIELD_COLUMNS) * CELL),
                 (s16)(FIELD_Y + (s16)(cells[index] / FIELD_COLUMNS) * CELL),
                 CELL, CELL, color);
    }

    buffer[0] = '\0';
    text_append(buffer, "Score ", (u16)sizeof(buffer));
    text_append_number(buffer, score, (u16)sizeof(buffer));
    gfx_text((s16)(right + 8), (s16)(FIELD_Y), buffer, COLOR_BLACK);
    gfx_text((s16)(right + 8), (s16)(FIELD_Y + 10), "Len", COLOR_BLACK);
    buffer[0] = '\0';
    text_append_number(buffer, length, (u16)sizeof(buffer));
    gfx_text((s16)(right + 34), (s16)(FIELD_Y + 10), buffer, COLOR_BLACK);

    gui_button((s16)(right + 34), (s16)(FIELD_Y + 26), 28, 22, "^", ID_UP);
    gui_button((s16)(right + 4), (s16)(FIELD_Y + 50), 28, 22, "<", ID_LEFT);
    gui_button((s16)(right + 64), (s16)(FIELD_Y + 50), 28, 22, ">", ID_RIGHT);
    gui_button((s16)(right + 34), (s16)(FIELD_Y + 74), 28, 22, "v", ID_DOWN);
    gui_button((s16)(right + 4), (s16)(FIELD_Y + 106), 88, 14, "New game",
               ID_NEW_GAME);

    gui_hotspot(FIELD_X, FIELD_Y, (s16)FIELD_WIDTH, (s16)FIELD_HEIGHT, ID_FIELD);

    if (alive == 0u) {
        gfx_fill((s16)(FIELD_X + 30), (s16)(FIELD_Y + 36), 108, 26, COLOR_FACE);
        gfx_outline((s16)(FIELD_X + 30), (s16)(FIELD_Y + 36), 108, 26, COLOR_BLACK);
        gfx_text_centered((s16)(FIELD_X + 30), (s16)(FIELD_Y + 40), 108,
                          "GAME OVER", COLOR_RED);
        gfx_text_centered((s16)(FIELD_X + 30), (s16)(FIELD_Y + 50), 108,
                          "New game to retry", COLOR_BLACK);
    } else {
        gui_message_bar("Arrows, buttons or click the field to steer");
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
    "SNK",
    "Snake",
    icon,
    COLOR_LIGHT_GREEN,
    snake_open,
    snake_draw,
    snake_event,
    snake_update
};
