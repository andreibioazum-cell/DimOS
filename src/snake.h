/*
 * snake.h -- the one application Sokol OS ships with.
 *
 * The board is a fixed 30x18 grid of 28 pixel cells, drawn by the software
 * renderer; the game logic itself never touches a pixel, which is what lets
 * the headless renderer play it too.
 */
#ifndef SOKOLOS_SNAKE_H
#define SOKOLOS_SNAKE_H

#include <stdint.h>

#include "draw.h"

#define SNAKE_COLS 30
#define SNAKE_ROWS 18
#define SNAKE_CELL 28
#define SNAKE_BOARD_X 220
#define SNAKE_BOARD_Y 132
#define SNAKE_BOARD_W (SNAKE_COLS * SNAKE_CELL)
#define SNAKE_BOARD_H (SNAKE_ROWS * SNAKE_CELL)
#define SNAKE_MAX_CELLS (SNAKE_COLS * SNAKE_ROWS)
#define SNAKE_MAX_PARTICLES 96

typedef enum {
    SNAKE_STATE_PLAYING = 0,
    SNAKE_STATE_DEAD = 1,
    SNAKE_STATE_PAUSED = 2
} SnakeState;

typedef enum {
    SNAKE_DIR_UP = 0,
    SNAKE_DIR_DOWN = 1,
    SNAKE_DIR_LEFT = 2,
    SNAKE_DIR_RIGHT = 3
} SnakeDirection;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    float life;
    float max_life;
    float size;
    uint32_t color;
} SnakeParticle;

typedef struct {
    int body_x[SNAKE_MAX_CELLS];
    int body_y[SNAKE_MAX_CELLS];
    int length;
    int dir_x;
    int dir_y;
    int queue_x[4];
    int queue_y[4];
    int queue_count;
    int food_x;
    int food_y;
    int score;
    int best;
    SnakeState state;
    float interval;
    float accumulator;
    float flash;
    float pulse;
    float death_time;
    uint32_t rng;
    SnakeParticle particles[SNAKE_MAX_PARTICLES];
    int particle_count;
} Snake;

typedef enum {
    SNAKE_INPUT_NONE = 0,
    SNAKE_INPUT_UP,
    SNAKE_INPUT_DOWN,
    SNAKE_INPUT_LEFT,
    SNAKE_INPUT_RIGHT,
    SNAKE_INPUT_PAUSE,
    SNAKE_INPUT_RESTART
} SnakeInput;

void snake_reset(Snake *snake, int keep_best);
void snake_update(Snake *snake, float dt);
/* Returns 1 when the input was consumed by the game. */
int snake_handle_input(Snake *snake, SnakeInput input);
/* Static board: frame, shadow and grid, painted once per screen. */
void snake_draw_background(Canvas *c);
/* Everything that moves: apple, body, particles and the overlays. */
void snake_draw(Canvas *c, const Snake *snake, float time);

#endif
