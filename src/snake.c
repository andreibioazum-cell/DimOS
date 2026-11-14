/*
 * snake.c -- Snake, the single application installed in Sokol OS.
 *
 * Classic rules: the board is walled, every apple grows the snake by one
 * cell and shortens the step interval, and the run ends when the head hits a
 * wall or the body. Everything is integer grid logic; the drawing half of
 * this file only reads the grid.
 */
#include "snake.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define SNAKE_START_LENGTH 4
#define SNAKE_START_INTERVAL 0.150f
#define SNAKE_MIN_INTERVAL 0.062f

static uint32_t random_next(uint32_t *state) {
    /* xorshift32: good enough for apples, and no libc dependency at all. */
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9E3779B9u;
    return *state;
}

static int cell_is_free(const Snake *snake, int x, int y) {
    for (int i = 0; i < snake->length; ++i) {
        if (snake->body_x[i] == x && snake->body_y[i] == y) {
            return 0;
        }
    }
    return 1;
}

static void place_food(Snake *snake) {
    const int free_cells = SNAKE_MAX_CELLS - snake->length;
    if (free_cells <= 0) {
        snake->food_x = -1;
        snake->food_y = -1;
        return;
    }
    int target = (int)(random_next(&snake->rng) % (uint32_t)free_cells);
    for (int y = 0; y < SNAKE_ROWS; ++y) {
        for (int x = 0; x < SNAKE_COLS; ++x) {
            if (!cell_is_free(snake, x, y)) {
                continue;
            }
            if (target-- == 0) {
                snake->food_x = x;
                snake->food_y = y;
                return;
            }
        }
    }
    snake->food_x = 0;
    snake->food_y = 0;
}

static void spawn_particles(Snake *snake, float x, float y, uint32_t color, int count) {
    for (int i = 0; i < count && snake->particle_count < SNAKE_MAX_PARTICLES; ++i) {
        SnakeParticle *p = &snake->particles[snake->particle_count++];
        const float angle = (float)(random_next(&snake->rng) % 628u) / 100.0f;
        const float speed = 40.0f + (float)(random_next(&snake->rng) % 90u);
        p->x = x;
        p->y = y;
        p->vx = cosf(angle) * speed;
        p->vy = sinf(angle) * speed;
        p->life = 0.45f + (float)(random_next(&snake->rng) % 40u) / 100.0f;
        p->max_life = p->life;
        p->size = 2.0f + (float)(random_next(&snake->rng) % 3u);
        p->color = color;
    }
}

void snake_reset(Snake *snake, int keep_best) {
    const int best = snake->best;
    uint32_t rng = snake->rng ? snake->rng : 0x1234567u;
    memset(snake, 0, sizeof(*snake));
    snake->best = keep_best ? best : 0;
    snake->rng = rng ^ (uint32_t)((size_t)snake >> 3);
    if (!snake->rng) {
        snake->rng = 0x2468ACE1u;
    }
    snake->length = SNAKE_START_LENGTH;
    const int head_x = SNAKE_COLS / 2;
    const int head_y = SNAKE_ROWS / 2;
    for (int i = 0; i < snake->length; ++i) {
        snake->body_x[i] = head_x - i;
        snake->body_y[i] = head_y;
    }
    snake->dir_x = 1;
    snake->dir_y = 0;
    snake->queue_count = 0;
    snake->state = SNAKE_STATE_PLAYING;
    snake->interval = SNAKE_START_INTERVAL;
    snake->accumulator = 0.0f;
    snake->score = 0;
    snake->death_time = 0.0f;
    snake->particle_count = 0;
    place_food(snake);
}

static void queue_direction(Snake *snake, int dx, int dy) {
    if (snake->queue_count >= (int)(sizeof(snake->queue_x) / sizeof(snake->queue_x[0]))) {
        return;
    }
    int last_x = snake->queue_count ? snake->queue_x[snake->queue_count - 1] : snake->dir_x;
    int last_y = snake->queue_count ? snake->queue_y[snake->queue_count - 1] : snake->dir_y;
    if (dx == last_x && dy == last_y) {
        return; /* same direction, nothing to do */
    }
    if (dx == -last_x && dy == -last_y) {
        return; /* no instant 180 degree turns */
    }
    snake->queue_x[snake->queue_count] = dx;
    snake->queue_y[snake->queue_count] = dy;
    snake->queue_count += 1;
}

static void step(Snake *snake) {
    if (snake->queue_count > 0) {
        snake->dir_x = snake->queue_x[0];
        snake->dir_y = snake->queue_y[0];
        for (int i = 1; i < snake->queue_count; ++i) {
            snake->queue_x[i - 1] = snake->queue_x[i];
            snake->queue_y[i - 1] = snake->queue_y[i];
        }
        snake->queue_count -= 1;
    }

    const int head_x = snake->body_x[0] + snake->dir_x;
    const int head_y = snake->body_y[0] + snake->dir_y;

    if (head_x < 0 || head_y < 0 || head_x >= SNAKE_COLS || head_y >= SNAKE_ROWS) {
        snake->state = SNAKE_STATE_DEAD;
        snake->death_time = 0.0f;
        if (snake->score > snake->best) {
            snake->best = snake->score;
        }
        spawn_particles(snake,
                        (float)SNAKE_BOARD_X + (float)(snake->body_x[0] * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                        (float)SNAKE_BOARD_Y + (float)(snake->body_y[0] * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                        RGB(0xF8, 0x71, 0x71), 16);
        return;
    }

    for (int i = 0; i < snake->length - 1; ++i) {
        if (snake->body_x[i] == head_x && snake->body_y[i] == head_y) {
            snake->state = SNAKE_STATE_DEAD;
            snake->death_time = 0.0f;
            if (snake->score > snake->best) {
                snake->best = snake->score;
            }
            spawn_particles(snake,
                            (float)SNAKE_BOARD_X + (float)(head_x * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                            (float)SNAKE_BOARD_Y + (float)(head_y * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                            RGB(0x4A, 0xDE, 0x80), 16);
            return;
        }
    }

    const int eating = (head_x == snake->food_x && head_y == snake->food_y);
    if (eating && snake->length < SNAKE_MAX_CELLS) {
        snake->length += 1;
    }
    for (int i = snake->length - 1; i > 0; --i) {
        snake->body_x[i] = snake->body_x[i - 1];
        snake->body_y[i] = snake->body_y[i - 1];
    }
    snake->body_x[0] = head_x;
    snake->body_y[0] = head_y;

    if (eating) {
        snake->score += 1;
        if (snake->score > snake->best) {
            snake->best = snake->score;
        }
        snake->flash = 1.0f;
        snake->interval = SNAKE_START_INTERVAL - (float)snake->score * 0.0035f;
        if (snake->interval < SNAKE_MIN_INTERVAL) {
            snake->interval = SNAKE_MIN_INTERVAL;
        }
        spawn_particles(snake,
                        (float)SNAKE_BOARD_X + (float)(head_x * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                        (float)SNAKE_BOARD_Y + (float)(head_y * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                        RGB(0xFB, 0xBF, 0x24), 12);
        place_food(snake);
    }
}

void snake_update(Snake *snake, float dt) {
    if (dt > 0.25f) {
        dt = 0.25f;
    }
    snake->pulse += dt;
    if (snake->flash > 0.0f) {
        snake->flash -= dt * 3.0f;
        if (snake->flash < 0.0f) {
            snake->flash = 0.0f;
        }
    }
    if (snake->state == SNAKE_STATE_PLAYING) {
        snake->accumulator += dt;
        int guard = 8;
        while (snake->accumulator >= snake->interval && guard-- > 0) {
            snake->accumulator -= snake->interval;
            step(snake);
            if (snake->state != SNAKE_STATE_PLAYING) {
                snake->accumulator = 0.0f;
                break;
            }
        }
    } else if (snake->state == SNAKE_STATE_DEAD) {
        snake->death_time += dt;
    }

    for (int i = 0; i < snake->particle_count; ++i) {
        SnakeParticle *p = &snake->particles[i];
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += 220.0f * dt;
        p->vx *= 0.98f;
        if (p->life <= 0.0f) {
            snake->particles[i] = snake->particles[snake->particle_count - 1];
            snake->particle_count -= 1;
            i -= 1;
        }
    }
}

int snake_handle_input(Snake *snake, SnakeInput input) {
    if (snake->state == SNAKE_STATE_DEAD) {
        if (input == SNAKE_INPUT_RESTART) {
            snake_reset(snake, 1);
            return 1;
        }
        return 0;
    }
    switch (input) {
    case SNAKE_INPUT_UP:
        queue_direction(snake, 0, -1);
        return 1;
    case SNAKE_INPUT_DOWN:
        queue_direction(snake, 0, 1);
        return 1;
    case SNAKE_INPUT_LEFT:
        queue_direction(snake, -1, 0);
        return 1;
    case SNAKE_INPUT_RIGHT:
        queue_direction(snake, 1, 0);
        return 1;
    case SNAKE_INPUT_PAUSE:
        snake->state = (snake->state == SNAKE_STATE_PAUSED) ? SNAKE_STATE_PLAYING : SNAKE_STATE_PAUSED;
        return 1;
    case SNAKE_INPUT_RESTART:
        snake_reset(snake, 1);
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_apple(Canvas *c, float cx, float cy, float phase) {
    const float pulse = 1.0f + 0.06f * sinf(phase * 6.0f);
    canvas_ring(c, cx, cy, 15.0f * pulse, 1.5f, RGB(0xF8, 0x71, 0x71), 60);
    canvas_circle_alpha(c, cx, cy, 13.0f * pulse, RGB(0xEF, 0x44, 0x44), 70);
    canvas_circle(c, cx, cy, 8.5f * pulse, RGB(0xF8, 0x71, 0x71));
    canvas_circle_alpha(c, cx - 2.4f, cy - 2.6f, 3.0f * pulse, RGB(0xFF, 0xE4, 0xE6), 190);
}

static void draw_head_eyes(Canvas *c, float cx, float cy, int dir_x, int dir_y, float radius) {
    const float offset = radius * 0.42f;
    const float forward = radius * 0.30f;
    /* Eyes sit forward of the head centre and are spread across the movement
     * axis, so the snake always looks where it is going. */
    const float ex = (float)dir_x * forward;
    const float ey = (float)dir_y * forward;
    const float sx = (dir_x != 0) ? 0.0f : offset;
    const float sy = (dir_x != 0) ? offset : 0.0f;
    canvas_circle(c, cx + ex + sx, cy + ey + sy, 2.6f, RGB(0x05, 0x2E, 0x16));
    canvas_circle(c, cx + ex - sx, cy + ey - sy, 2.6f, RGB(0x05, 0x2E, 0x16));
}

/* The frame, the grid and the shadow never change, so Sokol OS builds them
 * once into the static layer instead of repainting 400k pixels per frame. */
void snake_draw_background(Canvas *c) {
    const float bx = (float)SNAKE_BOARD_X;
    const float by = (float)SNAKE_BOARD_Y;
    const float bw = (float)SNAKE_BOARD_W;
    const float bh = (float)SNAKE_BOARD_H;

    canvas_shadow(c, bx, by, bw, bh, 20.0f, 24.0f, 90);
    canvas_round_rect(c, bx, by, bw, bh, 20.0f, RGB(0x08, 0x0F, 0x14));
    canvas_round_rect_outline(c, bx, by, bw, bh, 20.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 26);

    for (int x = 1; x < SNAKE_COLS; ++x) {
        canvas_fill_alpha(c, bx + (float)(x * SNAKE_CELL), by + 6.0f, 1.0f, bh - 12.0f, RGB(0xFF, 0xFF, 0xFF), 7);
    }
    for (int y = 1; y < SNAKE_ROWS; ++y) {
        canvas_fill_alpha(c, bx + 6.0f, by + (float)(y * SNAKE_CELL), bw - 12.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 7);
    }
}

void snake_draw(Canvas *c, const Snake *snake, float time) {
    const float bx = (float)SNAKE_BOARD_X;
    const float by = (float)SNAKE_BOARD_Y;
    const float bw = (float)SNAKE_BOARD_W;
    const float bh = (float)SNAKE_BOARD_H;

    if (snake->food_x >= 0) {
        draw_apple(c, bx + (float)(snake->food_x * SNAKE_CELL) + SNAKE_CELL * 0.5f,
                   by + (float)(snake->food_y * SNAKE_CELL) + SNAKE_CELL * 0.5f, snake->pulse);
    }

    /* Body, tail first so the head ends up on top. */
    for (int i = snake->length - 1; i >= 0; --i) {
        const float t = (snake->length > 1) ? (float)i / (float)(snake->length - 1) : 0.0f;
        const uint32_t color = color_lerp(RGB(0x86, 0xEF, 0xAC), RGB(0x12, 0x6B, 0x35), t);
        const float inset = (i == 0) ? 2.0f : 2.6f;
        const float size = (float)SNAKE_CELL - inset * 2.0f;
        const float x = bx + (float)(snake->body_x[i] * SNAKE_CELL) + inset;
        const float y = by + (float)(snake->body_y[i] * SNAKE_CELL) + inset;
        if (i == 0) {
            canvas_shadow(c, x, y, size, size, 8.0f, 10.0f, 70);
        }
        canvas_round_rect(c, x, y, size, size, (i == 0) ? 9.0f : 7.0f, color);
        if (i == 0) {
            canvas_round_rect_outline(c, x, y, size, size, 9.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 70);
        }
    }
    {
        const float cx = bx + (float)(snake->body_x[0] * SNAKE_CELL) + SNAKE_CELL * 0.5f;
        const float cy = by + (float)(snake->body_y[0] * SNAKE_CELL) + SNAKE_CELL * 0.5f;
        draw_head_eyes(c, cx, cy, snake->dir_x, snake->dir_y, (float)SNAKE_CELL * 0.5f - 2.0f);
    }

    for (int i = 0; i < snake->particle_count; ++i) {
        const SnakeParticle *p = &snake->particles[i];
        const float alpha = (p->max_life > 0.0f) ? (p->life / p->max_life) : 0.0f;
        canvas_circle_alpha(c, p->x, p->y, p->size * (0.4f + alpha * 0.8f), p->color, (uint8_t)(alpha * 220.0f));
    }

    /* Overlays. */
    if (snake->state == SNAKE_STATE_DEAD) {
        float fade = snake->death_time * 3.0f;
        if (fade > 1.0f) {
            fade = 1.0f;
        }
        canvas_round_rect_alpha(c, bx, by, bw, bh, 20.0f, RGB(0x02, 0x06, 0x08), (uint8_t)(180.0f * fade));
        if (fade > 0.35f) {
            const float alpha = (fade - 0.35f) / 0.65f;
            const float cw = 460.0f;
            const float ch = 214.0f;
            const float cx = bx + (bw - cw) * 0.5f;
            const float cy = by + (bh - ch) * 0.5f;
            canvas_shadow(c, cx, cy, cw, ch, 26.0f, 30.0f, 120);
            canvas_round_rect_gradient(c, cx, cy, cw, ch, 26.0f, RGB(0x14, 0x1D, 0x27), RGB(0x0C, 0x13, 0x1A));
            canvas_round_rect_outline(c, cx, cy, cw, ch, 26.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 30);
            text_ui_centered(c, cx + cw * 0.5f, cy + 30.0f, 34, RGB(0xF8, 0xFA, 0xFC), (uint8_t)(alpha * 255.0f), "Game over");
            char line[64];
            snprintf(line, sizeof(line), "Score %d      Best %d", snake->score, snake->best);
            text_ui_centered(c, cx + cw * 0.5f, cy + 84.0f, 18, RGB(0x94, 0xA3, 0xB8), (uint8_t)(alpha * 255.0f), line);
            text_ui_centered(c, cx + cw * 0.5f, cy + 126.0f, 15, RGB(0x86, 0xEF, 0xAC), (uint8_t)(alpha * 255.0f),
                             "Enter  play again        Esc  back to desktop");
            snprintf(line, sizeof(line), "length %d", snake->length);
            text_ui_centered(c, cx + cw * 0.5f, cy + 160.0f, 13, RGB(0x64, 0x74, 0x8B), (uint8_t)(alpha * 255.0f), line);
        }
    } else if (snake->state == SNAKE_STATE_PAUSED) {
        canvas_round_rect_alpha(c, bx, by, bw, bh, 20.0f, RGB(0x02, 0x06, 0x08), 150);
        text_ui_centered(c, bx + bw * 0.5f, by + bh * 0.5f - 40.0f, 40, RGB(0xF8, 0xFA, 0xFC), 255, "Paused");
        text_ui_centered(c, bx + bw * 0.5f, by + bh * 0.5f + 16.0f, 15, RGB(0x94, 0xA3, 0xB8), 255, "press P to continue");
    }
    (void)time;
}
