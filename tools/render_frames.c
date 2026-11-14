/*
 * render_frames.c -- run Sokol OS without a window and save PNG screenshots.
 *
 * The application core does not know anything about windows or GPUs, so the
 * exact same code that runs in the real window can be stepped here frame by
 * frame. That gives CI (and the documentation) real pictures of the boot
 * console, the desktop and the game without an X server or a GPU.
 *
 * Usage: render-frames <fonts/font.ttf> <output-directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image_write.h"

#include "app.h"

#define STEP (1.0f / 60.0f)

static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "render-frames: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    unsigned char *data = (unsigned char *)malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return data;
}

static int save_png(App *app, const char *directory, const char *name) {
    const Canvas *canvas = app_canvas(app);
    const size_t count = (size_t)canvas->width * (size_t)canvas->height;
    unsigned char *rgba = (unsigned char *)malloc(count * 4u);
    if (!rgba) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t px = canvas->pixels[i];
        rgba[i * 4 + 0] = (unsigned char)((px >> 16) & 0xFFu);
        rgba[i * 4 + 1] = (unsigned char)((px >> 8) & 0xFFu);
        rgba[i * 4 + 2] = (unsigned char)(px & 0xFFu);
        rgba[i * 4 + 3] = 255;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    const int ok = stbi_write_png(path, canvas->width, canvas->height, 4, rgba, canvas->width * 4);
    free(rgba);
    if (!ok) {
        fprintf(stderr, "render-frames: could not write %s\n", path);
    }
    return ok;
}

static void run(App *app, float seconds) {
    app_advance(app, seconds, STEP);
    app_draw_frame(app);
}

static void advance(App *app, float seconds) {
    app_advance(app, seconds, STEP);
}

static void send_key(App *app, AppKey key) {
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_KEY_DOWN;
    ev.key = key;
    app_input(app, &ev);
}

static void send_click(App *app, float x, float y) {
    InputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_MOUSE_MOVE;
    ev.x = x;
    ev.y = y;
    app_input(app, &ev);
    ev.type = INPUT_MOUSE_DOWN;
    app_input(app, &ev);
    ev.type = INPUT_MOUSE_UP;
    app_input(app, &ev);
}

/* A tiny BFS pilot: breadth-first search for the apple over the grid while
 * avoiding the body, then play the first step of that path. That keeps the
 * snake alive long enough to reach a believable score in the screenshots. */
static void steer(App *app) {
    const Snake *snake = app_snake(app);
    if (!snake || snake->state != SNAKE_STATE_PLAYING) {
        return;
    }
    enum { CELLS = SNAKE_COLS * SNAKE_ROWS };
    static int previous[CELLS];
    static int queue[CELLS];
    for (int i = 0; i < CELLS; ++i) {
        previous[i] = -2; /* -2 unvisited, -1 start */
    }
    unsigned char blocked[CELLS] = {0};
    for (int i = 0; i < snake->length; ++i) {
        const int index = snake->body_y[i] * SNAKE_COLS + snake->body_x[i];
        if (i == snake->length - 1) {
            continue; /* the tail moves away this step */
        }
        blocked[index] = 1;
    }
    const int start = snake->body_y[0] * SNAKE_COLS + snake->body_x[0];
    const int goal = snake->food_y * SNAKE_COLS + snake->food_x;
    int head = 0, tail = 0;
    queue[tail++] = start;
    previous[start] = -1;
    int found = 0;
    while (head < tail) {
        const int cell = queue[head++];
        if (cell == goal) {
            found = 1;
            break;
        }
        const int cx = cell % SNAKE_COLS;
        const int cy = cell / SNAKE_COLS;
        static const int step_x[4] = {1, -1, 0, 0};
        static const int step_y[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + step_x[d];
            const int ny = cy + step_y[d];
            if (nx < 0 || ny < 0 || nx >= SNAKE_COLS || ny >= SNAKE_ROWS) {
                continue;
            }
            const int next = ny * SNAKE_COLS + nx;
            if (previous[next] != -2 || blocked[next]) {
                continue;
            }
            previous[next] = cell;
            queue[tail++] = next;
        }
    }

    AppKey key = KEY_UNKNOWN;
    if (found) {
        int cell = goal;
        while (previous[cell] >= 0 && previous[cell] != start) {
            cell = previous[cell];
        }
        const int dx = (cell % SNAKE_COLS) - snake->body_x[0];
        const int dy = (cell / SNAKE_COLS) - snake->body_y[0];
        key = (dx > 0) ? KEY_RIGHT : (dx < 0) ? KEY_LEFT : (dy > 0) ? KEY_DOWN : KEY_UP;
    } else {
        /* No path: take any step that does not hit a wall or the body. */
        const int cx = snake->body_x[0];
        const int cy = snake->body_y[0];
        static const int step_x[4] = {1, -1, 0, 0};
        static const int step_y[4] = {0, 0, 1, -1};
        static const AppKey keys[4] = {KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP};
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + step_x[d];
            const int ny = cy + step_y[d];
            if (nx < 0 || ny < 0 || nx >= SNAKE_COLS || ny >= SNAKE_ROWS) {
                continue;
            }
            if (blocked[ny * SNAKE_COLS + nx]) {
                continue;
            }
            key = keys[d];
            break;
        }
    }
    if (key != KEY_UNKNOWN) {
        send_key(app, key);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <font.ttf> <output-directory>\n", argv[0]);
        return 2;
    }
    size_t font_size = 0;
    unsigned char *font = read_file(argv[1], &font_size);
    if (!font) {
        return 1;
    }
    App *app = app_create(font, font_size);
    if (!app) {
        fprintf(stderr, "render-frames: out of memory\n");
        free(font);
        return 1;
    }

    int saved = 0;
    advance(app, 0.55f);
    app_draw_frame(app);
    saved += save_png(app, argv[2], "00-boot-console.png");

    run(app, 1.85f);
    saved += save_png(app, argv[2], "01-boot-kernel.png");

    run(app, 4.40f); /* the login prompt has finished typing by now */
    saved += save_png(app, argv[2], "02-boot-login.png");

    for (int i = 0; i < 900 && app_screen(app) != APP_SCREEN_DESKTOP; ++i) {
        advance(app, STEP);
    }
    run(app, 0.5f);
    saved += save_png(app, argv[2], "03-desktop.png");

    send_click(app, (float)APP_WIDTH * 0.5f, 330.0f);
    run(app, 0.45f);
    saved += save_png(app, argv[2], "04-snake.png");

    /* Play for a while so the score is real, then pose. */
    int steps = 0;
    while (steps < 480 && app_snake(app)->state == SNAKE_STATE_PLAYING) {
        steer(app);
        advance(app, STEP);
        steps += 1;
    }
    run(app, 0.25f);
    saved += save_png(app, argv[2], "05-snake-play.png");

    /* Keep playing until the greedy pilot crashes. */
    while (steps < 6000 && app_snake(app)->state == SNAKE_STATE_PLAYING) {
        steer(app);
        advance(app, STEP);
        steps += 1;
    }
    run(app, 0.8f);
    saved += save_png(app, argv[2], "06-snake-game-over.png");

    send_key(app, KEY_ESCAPE);
    run(app, 0.5f);
    saved += save_png(app, argv[2], "07-desktop-again.png");

    printf("render-frames: %d screenshots written to %s\n", saved, argv[2]);
    printf("render-frames: final score %d, best %d, length %d\n", app_snake(app)->score, app_snake(app)->best,
           app_snake(app)->length);
    app_destroy(app);
    free(font);
    return saved == 8 ? 0 : 1;
}
