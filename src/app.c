/*
 * app.c -- DimOS: the boot console, the desktop and the window manager.
 *
 * Three screens share one 1280x720 canvas:
 *
 *   1. BOOT     an Arch Linux style console: kernel messages, systemd's
 *               green [  OK  ] lines, then a login prompt that types itself.
 *   2. DESKTOP  a wallpaper, a top bar with a live clock, and exactly one
 *               application icon in the middle of the screen.
 *   3. SNAKE    the one installed application (see snake.c).
 *
 * There is no 3D here at all: no depth buffer, no matrices, no perspective.
 * Every pixel is painted on the CPU into one ARGB canvas, and the platform
 * layer blits that canvas with a single textured quad.
 */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "snake.h"

/* ------------------------------------------------------------------ */
/* Boot script                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    BOOT_STYLE_BANNER = 0,
    BOOT_STYLE_KERNEL,
    BOOT_STYLE_OK,
    BOOT_STYLE_INFO
} BootStyle;

typedef struct {
    float at;
    BootStyle style;
    const char *text;
} BootLine;

static const BootLine boot_lines[] = {
    {0.10f, BOOT_STYLE_BANNER, "Arch Linux 6.12.9-arch1-1 (tty1)"},
    {0.45f, BOOT_STYLE_KERNEL, "[    0.000000] Linux version 6.12.9-arch1-1 (linux@archlinux)"},
    {0.58f, BOOT_STYLE_KERNEL, "[    0.000000] Command line: BOOT_IMAGE=/vmlinuz-linux root=UUID=6f1c"},
    {0.71f, BOOT_STYLE_KERNEL, "[    0.000014] BIOS-provided physical RAM map:"},
    {0.84f, BOOT_STYLE_KERNEL, "[    0.000057] ACPI: RSDP 0x00000000000F05B0 000024 (v02 SOKOL )"},
    {0.97f, BOOT_STYLE_KERNEL, "[    0.000092] Memory: 3298612K/4194304K available"},
    {1.10f, BOOT_STYLE_KERNEL, "[    0.000131] smpboot: CPU0: AMD Ryzen 7 5800X 8-Core"},
    {1.24f, BOOT_STYLE_KERNEL, "[    0.024418] sokol: window 1280x720, 32-bit canvas, 60 Hz"},
    {1.37f, BOOT_STYLE_KERNEL, "[    0.024492] dimos: software renderer ready"},
    {1.50f, BOOT_STYLE_KERNEL, "[    0.024530] dimos: input devices: mouse, keyboard"},
    {1.68f, BOOT_STYLE_KERNEL, "[    0.061208] dimos: 964.3 MB/s canvas blit, 0 dropped"},
    {1.90f, BOOT_STYLE_INFO,   "systemd[1]: systemd 256.9-1-arch running in system mode"},
    {2.15f, BOOT_STYLE_OK,     " Created slice User and Session Slice."},
    {2.35f, BOOT_STYLE_OK,     " Started Journal Service."},
    {2.55f, BOOT_STYLE_OK,     " Mounted /boot."},
    {2.75f, BOOT_STYLE_OK,     " Started udev Kernel Device Manager."},
    {2.95f, BOOT_STYLE_OK,     " Reached target Local File Systems."},
    {3.15f, BOOT_STYLE_OK,     " Started Sokol Compositor."},
    {3.35f, BOOT_STYLE_OK,     " Started Snake."},
    {3.55f, BOOT_STYLE_OK,     " Reached target Graphical Interface."},
};

#define BOOT_LINE_COUNT ((int)(sizeof(boot_lines) / sizeof(boot_lines[0])))

static const char login_block[] =
    "Arch Linux 6.12.9-arch1-1 (tty1)\n"
    "\n"
    "archlinux login: root\n"
    "Password: ******\n"
    "\n"
    "Last login: Fri Nov 14 08:57:31 on tty1\n"
    "\n"
    "[root@archlinux ~]# snake --desktop\n"
    "\n";

typedef enum {
    BOOT_PHASE_LOG = 0,
    BOOT_PHASE_LOGIN
} BootPhase;

/* ------------------------------------------------------------------ */
/* Application state                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

struct App {
    Canvas canvas;
    uint32_t *wallpaper;
    /* Everything that does not move is painted once into this layer; a frame
     * is then a memcpy plus the handful of sprites that actually animate. */
    uint32_t *static_layer;
    int static_ready; /* AppScreen the static layer was built for, or -1 */

    AppScreen screen;
    float time;
    float fade;
    int first_frame;
    int input_pending;
    int quit_requested;

    float mouse_x;
    float mouse_y;

    /* boot */
    BootPhase boot_phase;
    float login_start;
    float login_chars;

    /* desktop */
    Rect icon_rect;
    int icon_hovered;
    int icon_pressed;
    float hover;
    int clock_minute;

    /* snake */
    Snake snake;
    Rect back_button;
    int back_hovered;
};

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    return color_lerp(a, b, t);
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The panel clock is the only part of the desktop that depends on the host
 * machine, so it is read through two tiny helpers. */
static void read_clock(int *hour, int *minute, int *day, int *weekday, int *month) {
    const time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (!local) {
        *hour = 0;
        *minute = 0;
        *day = 1;
        *weekday = 0;
        *month = 0;
        return;
    }
    *hour = local->tm_hour;
    *minute = local->tm_min;
    *day = local->tm_mday;
    *weekday = local->tm_wday % 7;
    *month = local->tm_mon % 12;
}

static void build_wallpaper(App *app) {
    const int w = APP_WIDTH;
    const int h = APP_HEIGHT;
    const uint32_t top = RGB(0x0E, 0x1B, 0x2D);
    const uint32_t bottom = RGB(0x04, 0x07, 0x0C);
    const uint32_t glow_blue = RGB(0x1D, 0x4E, 0xD8);
    const uint32_t glow_green = RGB(0x05, 0x96, 0x68);
    for (int y = 0; y < h; ++y) {
        const float v = (float)y / (float)(h - 1);
        const uint32_t base = mix(top, bottom, v);
        for (int x = 0; x < w; ++x) {
            uint32_t color = base;

            const float dx1 = ((float)x - 300.0f) / 780.0f;
            const float dy1 = ((float)y + 150.0f) / 620.0f;
            float d1 = 1.0f - sqrtf(dx1 * dx1 + dy1 * dy1);
            if (d1 > 0.0f) {
                color = mix(color, glow_blue, d1 * d1 * 0.55f);
            }

            const float dx2 = ((float)x - 1090.0f) / 560.0f;
            const float dy2 = ((float)y - 760.0f) / 460.0f;
            float d2 = 1.0f - sqrtf(dx2 * dx2 + dy2 * dy2);
            if (d2 > 0.0f) {
                color = mix(color, glow_green, d2 * d2 * 0.35f);
            }

            const float vx = ((float)x - (float)w * 0.5f) / ((float)w * 0.5f);
            const float vy = ((float)y - (float)h * 0.5f) / ((float)h * 0.5f);
            const float vignette = 1.0f - 0.30f * (vx * vx + vy * vy) * 0.5f;
            const uint32_t r = (uint32_t)((float)PIXEL_R(color) * vignette);
            const uint32_t g = (uint32_t)((float)PIXEL_G(color) * vignette);
            const uint32_t b = (uint32_t)((float)PIXEL_B(color) * vignette);
            app->wallpaper[y * w + x] = PIXEL(0xFF, r, g, b);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

App *app_create(const unsigned char *ttf, size_t ttf_size) {
    App *app = (App *)calloc(1, sizeof(App));
    if (!app) {
        return NULL;
    }
    app->canvas.width = APP_WIDTH;
    app->canvas.height = APP_HEIGHT;
    app->canvas.pixels = (uint32_t *)malloc((size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
    app->wallpaper = (uint32_t *)malloc((size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
    app->static_layer = (uint32_t *)malloc((size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
    app->static_ready = -1;
    if (!app->canvas.pixels || !app->wallpaper || !app->static_layer) {
        app_destroy(app);
        return NULL;
    }
    if (!text_init(ttf, ttf_size)) {
        fprintf(stderr, "dimos: warning: could not load the UI font\n");
    }
    build_wallpaper(app);

    app->icon_rect.x = APP_WIDTH / 2 - 52;
    app->icon_rect.y = 268;
    app->icon_rect.w = 104;
    app->icon_rect.h = 104;

    app->back_button.x = 170;
    app->back_button.y = 54;
    app->back_button.w = 40;
    app->back_button.h = 40;

    snake_reset(&app->snake, 0);
    app->screen = APP_SCREEN_BOOT;
    app->boot_phase = BOOT_PHASE_LOG;
    app->first_frame = 1;
    app->input_pending = 1;
    app->fade = 1.0f;
    app->clock_minute = -1;
    return app;
}

void app_destroy(App *app) {
    if (!app) {
        return;
    }
    free(app->canvas.pixels);
    free(app->wallpaper);
    free(app->static_layer);
    free(app);
}

Canvas *app_canvas(App *app) {
    return &app->canvas;
}

AppScreen app_screen(const App *app) {
    return app->screen;
}

int app_quit_requested(const App *app) {
    return app->quit_requested;
}

/* The live ISO boots a real kernel that prints a real log on tty1, so the
 * shell skips its own console and opens straight on the desktop. */
void app_skip_boot(App *app) {
    if (app->screen == APP_SCREEN_BOOT) {
        app_goto(app, APP_SCREEN_DESKTOP);
    }
}

void app_goto(App *app, AppScreen screen) {
    app->screen = screen;
    app->fade = 1.0f;
    app->input_pending = 1;
    app->static_ready = -1;
    if (screen == APP_SCREEN_SNAKE) {
        snake_reset(&app->snake, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Update                                                              */
/* ------------------------------------------------------------------ */

static int hover_in(const App *app, const Rect *r, float grow) {
    return app->mouse_x >= (float)r->x - grow && app->mouse_x <= (float)(r->x + r->w) + grow &&
           app->mouse_y >= (float)r->y - grow && app->mouse_y <= (float)(r->y + r->h) + grow;
}

void app_update(App *app, float dt) {
    if (dt <= 0.0f) {
        dt = 0.0f;
    }
    if (dt > 0.25f) {
        dt = 0.25f;
    }
    app->time += dt;

    if (app->fade > 0.0f) {
        app->fade -= dt * 3.2f;
        if (app->fade < 0.0f) {
            app->fade = 0.0f;
        }
    }

    switch (app->screen) {
    case APP_SCREEN_BOOT: {
        if (app->boot_phase == BOOT_PHASE_LOG) {
            const float last = boot_lines[BOOT_LINE_COUNT - 1].at;
            if (app->time > last + 1.1f) {
                app->boot_phase = BOOT_PHASE_LOGIN;
                app->login_start = app->time;
            }
        } else {
            const float revealed = (app->time - app->login_start) * 68.0f;
            app->login_chars = revealed;
            const float total = (float)strlen(login_block);
            if (revealed > total + 45.0f) {
                app_goto(app, APP_SCREEN_DESKTOP);
            }
        }
        break;
    }
    case APP_SCREEN_DESKTOP: {
        const int hovered = hover_in(app, &app->icon_rect, 10.0f);
        const float target = hovered ? 1.0f : 0.0f;
        app->icon_hovered = hovered;
        app->hover += (target - app->hover) * clampf(dt * 12.0f, 0.0f, 1.0f);
        if (fabsf(app->hover - target) < 0.002f) {
            app->hover = target;
        }
        break;
    }
    case APP_SCREEN_SNAKE: {
        snake_update(&app->snake, dt);
        app->back_hovered = hover_in(app, &app->back_button, 2.0f);
        break;
    }
    default:
        break;
    }
}

int app_needs_frame(const App *app) {
    if (app->first_frame || app->input_pending) {
        return 1;
    }
    if (app->fade > 0.0f) {
        return 1;
    }
    switch (app->screen) {
    case APP_SCREEN_BOOT:
        return 1; /* the console is always typing something */
    case APP_SCREEN_DESKTOP: {
        /* An idle desktop animates nothing: the platform may skip the whole
         * frame. Only the rising/settling hover animation and a new minute
         * on the clock wake it up again. */
        if (app->hover != (app->icon_hovered ? 1.0f : 0.0f)) {
            return 1;
        }
        int hour = 0, minute = 0, day = 0, weekday = 0, month = 0;
        read_clock(&hour, &minute, &day, &weekday, &month);
        return (hour * 60 + minute) != app->clock_minute;
    }
    case APP_SCREEN_SNAKE:
        return 1; /* a running snake is an animation */
    default:
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

void app_input(App *app, const InputEvent *event) {
    app->input_pending = 1;

    if (event->type == INPUT_MOUSE_MOVE) {
        app->mouse_x = event->x;
        app->mouse_y = event->y;
        return;
    }

    if (event->type == INPUT_MOUSE_DOWN) {
        app->mouse_x = event->x;
        app->mouse_y = event->y;
        if (app->screen == APP_SCREEN_SNAKE && hover_in(app, &app->back_button, 2.0f)) {
            app_goto(app, APP_SCREEN_DESKTOP);
            return;
        }
        if (app->screen == APP_SCREEN_BOOT) {
            app_goto(app, APP_SCREEN_DESKTOP);
            return;
        }
        if (app->screen == APP_SCREEN_DESKTOP && hover_in(app, &app->icon_rect, 10.0f)) {
            app->icon_pressed = 1;
        }
        return;
    }

    if (event->type == INPUT_MOUSE_UP) {
        app->mouse_x = event->x;
        app->mouse_y = event->y;
        if (app->screen == APP_SCREEN_DESKTOP && app->icon_pressed && hover_in(app, &app->icon_rect, 10.0f)) {
            app->icon_pressed = 0;
            app_goto(app, APP_SCREEN_SNAKE);
            return;
        }
        app->icon_pressed = 0;
        return;
    }

    if (event->type != INPUT_KEY_DOWN) {
        return;
    }

    switch (app->screen) {
    case APP_SCREEN_BOOT:
        app_goto(app, APP_SCREEN_DESKTOP);
        break;
    case APP_SCREEN_DESKTOP:
        if (event->key == KEY_ENTER || event->key == KEY_SPACE) {
            app_goto(app, APP_SCREEN_SNAKE);
        } else if (event->key == KEY_ESCAPE) {
            app->quit_requested = 1;
        }
        break;
    case APP_SCREEN_SNAKE: {
        if (event->key == KEY_ESCAPE) {
            app_goto(app, APP_SCREEN_DESKTOP);
            break;
        }
        SnakeInput input = SNAKE_INPUT_NONE;
        switch (event->key) {
        case KEY_UP:
        case KEY_W:
            input = SNAKE_INPUT_UP;
            break;
        case KEY_DOWN:
        case KEY_S:
            input = SNAKE_INPUT_DOWN;
            break;
        case KEY_LEFT:
        case KEY_A:
            input = SNAKE_INPUT_LEFT;
            break;
        case KEY_RIGHT:
        case KEY_D:
            input = SNAKE_INPUT_RIGHT;
            break;
        case KEY_P:
            input = SNAKE_INPUT_PAUSE;
            break;
        case KEY_SPACE:
            input = (app->snake.state == SNAKE_STATE_DEAD) ? SNAKE_INPUT_RESTART : SNAKE_INPUT_PAUSE;
            break;
        case KEY_ENTER:
        case KEY_R:
            input = SNAKE_INPUT_RESTART;
            break;
        default:
            break;
        }
        snake_handle_input(&app->snake, input);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Boot screen                                                         */
/* ------------------------------------------------------------------ */

static void draw_boot_log(App *app) {
    Canvas *c = &app->canvas;
    const float scale = 2.0f;
    const float line_height = 20.0f;
    const float top = 46.0f;

    int printed = 0;
    for (int i = 0; i < BOOT_LINE_COUNT; ++i) {
        if (app->time + 0.0001f < boot_lines[i].at) {
            break;
        }
        printed = i + 1;
    }

    for (int i = 0; i < printed; ++i) {
        const BootLine *line = &boot_lines[i];
        const float alpha = clampf((app->time - line->at) / 0.10f, 0.0f, 1.0f);
        const uint8_t a = (uint8_t)(alpha * 255.0f);
        const float y = top + (float)i * line_height;
        switch (line->style) {
        case BOOT_STYLE_BANNER:
            text_mono(c, 26.0f, y, scale, RGB(0xF2, 0xF4, 0xF8), a, line->text);
            break;
        case BOOT_STYLE_KERNEL: {
            /* Timestamp in dim grey, the message itself in near white. */
            char stamp[16];
            memcpy(stamp, line->text, 13);
            stamp[13] = '\0';
            text_mono(c, 26.0f, y, scale, RGB(0x5B, 0x66, 0x77), a, stamp);
            text_mono(c, 26.0f + 13.0f * 8.0f * scale, y, scale, RGB(0xC3, 0xCC, 0xD9), a, line->text + 13);
            break;
        }
        case BOOT_STYLE_INFO:
            text_mono(c, 26.0f, y, scale, RGB(0x56, 0xB6, 0xE0), a, line->text);
            break;
        case BOOT_STYLE_OK: {
            text_mono(c, 26.0f, y, scale, RGB(0x6B, 0x77, 0x88), a, "[  ");
            text_mono(c, 26.0f + 3.0f * 16.0f, y, scale, RGB(0x35, 0xD0, 0x5F), a, "OK");
            text_mono(c, 26.0f + 5.0f * 16.0f, y, scale, RGB(0x6B, 0x77, 0x88), a, "  ]");
            text_mono(c, 26.0f + 8.0f * 16.0f, y, scale, RGB(0xD7, 0xDE, 0xE8), a, line->text);
            break;
        }
        default:
            break;
        }
    }

    /* Blinking block cursor right after the newest line. */
    if (((int)(app->time * 2.0f) & 1) == 0 && printed > 0) {
        const float y = top + (float)printed * line_height;
        text_mono(c, 26.0f, y, scale, RGB(0xC3, 0xCC, 0xD9), 200, "_");
    }

    text_ui(c, 26.0f, (float)APP_HEIGHT - 34.0f, 13, RGB(0x64, 0x74, 0x8B), "any key or click skips the boot log");
}

static void draw_boot_login(App *app) {
    Canvas *c = &app->canvas;
    const float scale = 2.0f;
    const float line_height = 26.0f;
    const float left = 46.0f;
    const float top = 60.0f;

    const int len = (int)strlen(login_block);
    int visible = (int)app->login_chars;
    if (visible > len) {
        visible = len;
    }

    int col = 0;
    int row = 0;
    for (int i = 0; i < visible; ++i) {
        const char ch = login_block[i];
        if (ch == '\n') {
            col = 0;
            row += 1;
            continue;
        }
        char glyph[2] = {ch, '\0'};
        uint32_t color = RGB(0xC9, 0xD1, 0xDB);
        if (i < 13) {
            color = RGB(0xF2, 0xF4, 0xF8);
        } else if (ch == '*' || ch == '#') {
            color = RGB(0x86, 0xEF, 0xAC);
        }
        text_mono(c, left + (float)col * 16.0f, top + (float)row * line_height, scale, color, 255, glyph);
        col += 1;
    }

    if (((int)(app->time * 2.4f) & 1) == 0) {
        text_mono(c, left + (float)col * 16.0f, top + (float)row * line_height, scale, RGB(0xE8, 0xEE, 0xF6), 220, "_");
    }

    text_ui(c, 46.0f, (float)APP_HEIGHT - 34.0f, 13, RGB(0x64, 0x74, 0x8B), "starting the desktop...");
}

/* ------------------------------------------------------------------ */
/* Desktop                                                             */
/* ------------------------------------------------------------------ */

static void draw_status_icons(Canvas *c, float right) {
    const uint32_t ink = RGB(0xE4, 0xEB, 0xF4);

    /* battery: outline, 70% charge, terminal nub */
    const float bx = right - 118.0f;
    const float by = 15.0f;
    canvas_round_rect_outline(c, bx, by, 27.0f, 13.0f, 4.0f, 1.4f, ink, 150);
    canvas_round_rect(c, bx + 2.4f, by + 2.4f, 16.0f, 8.2f, 2.2f, RGB(0x4A, 0xDE, 0x80));
    canvas_round_rect(c, bx + 28.2f, by + 4.6f, 2.2f, 3.8f, 1.0f, ink);

    /* volume: a filled speaker cone plus two sound arcs */
    const float vx = right - 76.0f;
    const float vy = 21.5f;
    for (int i = 0; i < 6; ++i) {
        const float h = 5.0f + (float)i * 0.9f;
        canvas_fill(c, vx + 4.0f + (float)i, vy - h * 0.5f, 1.3f, h, ink);
    }
    canvas_fill(c, vx, vy - 2.6f, 4.2f, 5.2f, ink);
    canvas_ring_arc(c, vx + 5.0f, vy, 7.0f, 1.5f, -55.0f, 55.0f, ink, 190);
    canvas_ring_arc(c, vx + 5.0f, vy, 11.0f, 1.4f, -55.0f, 55.0f, ink, 110);

    /* wifi: three arcs and a dot */
    const float wx = right - 28.0f;
    const float wy = 24.5f;
    canvas_ring_arc(c, wx, wy, 4.5f, 1.6f, 225.0f, 315.0f, ink, 210);
    canvas_ring_arc(c, wx, wy, 8.5f, 1.6f, 225.0f, 315.0f, ink, 210);
    canvas_ring_arc(c, wx, wy, 12.5f, 1.6f, 225.0f, 315.0f, ink, 210);
    canvas_circle(c, wx, wy, 1.7f, ink);
}

static void draw_top_bar_base(Canvas *c) {
    canvas_fill_alpha(c, 0.0f, 0.0f, (float)APP_WIDTH, 40.0f, RGB(0x04, 0x07, 0x0C), 120);
    canvas_fill_alpha(c, 0.0f, 39.0f, (float)APP_WIDTH, 1.0f, RGB(0xFF, 0xFF, 0xFF), 18);
    text_ui(c, 26.0f, 11.0f, 16, RGB(0xF2, 0xF4, 0xF8), "DimOS");

    /* a small arch-shaped mark, drawn with three strokes */
    canvas_line(c, 106.0f, 30.0f, 112.0f, 12.0f, 2.0f, RGB(0x4A, 0xDE, 0x80), 230);
    canvas_line(c, 112.0f, 12.0f, 118.0f, 30.0f, 2.0f, RGB(0x4A, 0xDE, 0x80), 230);
    canvas_line(c, 109.0f, 24.0f, 115.0f, 24.0f, 1.8f, RGB(0x4A, 0xDE, 0x80), 160);

    draw_status_icons(c, (float)APP_WIDTH - 26.0f);
}

static void draw_top_bar_clock(App *app) {
    Canvas *c = &app->canvas;
    int hour = 0, minute = 0, day = 0, weekday = 0, month = 0;
    read_clock(&hour, &minute, &day, &weekday, &month);
    app->clock_minute = hour * 60 + minute;

    char clock[16];
    char date[32];
    snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
    static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(date, sizeof(date), "%s %d %s", weekdays[weekday], day, months[month]);

    /* The clock is only repainted when the minute really changed, so the
     * static layer underneath is not even touched in between. */
    const float clock_w = text_ui_width(16, clock);
    const float date_w = text_ui_width(13, date);
    const float total = clock_w + 14.0f + date_w;
    const float start = ((float)APP_WIDTH - total) * 0.5f;
    text_ui(c, start, 11.0f, 16, RGB(0xF2, 0xF4, 0xF8), clock);
    text_ui(c, start + clock_w + 14.0f, 13.0f, 13, RGB(0x94, 0xA3, 0xB8), date);
}

static void draw_snake_badge(Canvas *c, float cx, float cy, float size, float glow) {
    /* The icon: a rounded green tile with a smooth S shaped snake on it. */
    if (glow > 0.01f) {
        canvas_round_rect_alpha(c, cx - size * 0.5f - 5.0f, cy - size * 0.5f - 5.0f, size + 10.0f, size + 10.0f, 32.0f,
                                RGB(0x34, 0xD3, 0x99), (uint8_t)(60.0f * glow));
    }
    canvas_shadow(c, cx - size * 0.5f, cy - size * 0.5f, size, size, size * 0.26f, 18.0f, 110);
    canvas_round_rect_gradient(c, cx - size * 0.5f, cy - size * 0.5f, size, size, size * 0.26f,
                               RGB(0x3F, 0xD9, 0x8A), RGB(0x04, 0x6B, 0x4D));
    canvas_round_rect_outline(c, cx - size * 0.5f, cy - size * 0.5f, size, size, size * 0.26f, 1.2f,
                              RGB(0xFF, 0xFF, 0xFF), 60);

    /* Sample a horizontal S curve densely and stamp circles of varying
     * radius: a continuous tapered body without any polygon code. */
    const float unit = size / 104.0f;
    enum { SAMPLES = 72 };
    float px[SAMPLES + 1];
    float py[SAMPLES + 1];
    float pr[SAMPLES + 1];
    for (int i = 0; i <= SAMPLES; ++i) {
        const float t = (float)i / (float)SAMPLES;
        px[i] = cx + (t - 0.5f) * size * 0.70f;
        py[i] = cy + sinf(t * 6.2831853f * 1.05f - 0.35f) * size * 0.135f;
        pr[i] = (0.020f + 0.020f * t) * size;
    }

    /* dark contour first, then the body itself */
    for (int i = 0; i <= SAMPLES; ++i) {
        canvas_circle(c, px[i], py[i], pr[i] + 2.2f * unit, RGB(0x02, 0x2C, 0x1C));
    }
    for (int i = 0; i <= SAMPLES; ++i) {
        const float t = (float)i / (float)SAMPLES;
        canvas_circle(c, px[i], py[i], pr[i], mix(RGB(0x0F, 0x7A, 0x3F), RGB(0xD9, 0xFB, 0xE6), t));
    }

    /* head sits on the last sample, looking along the tail direction */
    const float hx = px[SAMPLES];
    const float hy = py[SAMPLES];
    const float heading = atan2f(py[SAMPLES] - py[SAMPLES - 1], px[SAMPLES] - px[SAMPLES - 1]);
    const float head_r = 0.062f * size;
    canvas_circle(c, hx, hy, head_r + 2.2f * unit, RGB(0x02, 0x2C, 0x1C));
    canvas_circle(c, hx, hy, head_r, RGB(0xEC, 0xFD, 0xF5));

    const float ex = cosf(heading), ey = sinf(heading);
    const float nx = -ey, ny = ex;
    const float eye_f = head_r * 0.35f;
    const float eye_s = head_r * 0.42f;
    canvas_circle(c, hx + ex * eye_f + nx * eye_s, hy + ey * eye_f + ny * eye_s, head_r * 0.22f, RGB(0x04, 0x33, 0x21));
    canvas_circle(c, hx + ex * eye_f - nx * eye_s, hy + ey * eye_f - ny * eye_s, head_r * 0.22f, RGB(0x04, 0x33, 0x21));

    /* forked tongue */
    const float tx = hx + ex * head_r * 1.15f;
    const float ty = hy + ey * head_r * 1.15f;
    canvas_line(c, tx, ty, tx + ex * head_r * 0.75f, ty + ey * head_r * 0.75f, 1.3f * unit, RGB(0xF8, 0x71, 0x71), 210);
    canvas_line(c, tx + ex * head_r * 0.75f, ty + ey * head_r * 0.75f, tx + ex * head_r * 1.15f + nx * head_r * 0.3f,
                ty + ey * head_r * 1.15f + ny * head_r * 0.3f, 1.1f * unit, RGB(0xF8, 0x71, 0x71), 210);
    canvas_line(c, tx + ex * head_r * 0.75f, ty + ey * head_r * 0.75f, tx + ex * head_r * 1.15f - nx * head_r * 0.3f,
                ty + ey * head_r * 1.15f - ny * head_r * 0.3f, 1.1f * unit, RGB(0xF8, 0x71, 0x71), 210);
}

static void snapshot_static(App *app) {
    memcpy(app->static_layer, app->canvas.pixels,
           (size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
    app->static_ready = (int)app->screen;
}

static void restore_static(App *app) {
    memcpy(app->canvas.pixels, app->static_layer,
           (size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
}

/* ------------------------------------------------------------------ */
/* Desktop                                                             */
/* ------------------------------------------------------------------ */

static void prepare_desktop(App *app) {
    Canvas *c = &app->canvas;
    memcpy(c->pixels, app->wallpaper, (size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));

    /* one smooth spotlight behind the icon, no banding */
    canvas_radial_glow(c, (float)APP_WIDTH * 0.5f, 330.0f, 430.0f, RGB(0x2B, 0x5C, 0xE8), 42);

    draw_top_bar_base(c);

    /* label with a soft drop shadow so it reads on any wallpaper */
    const float cx = (float)APP_WIDTH * 0.5f;
    const float label_y = 330.0f + 52.0f + 16.0f;
    const float label_w = text_ui_width(17, "Snake");
    text_ui_alpha(c, cx - label_w * 0.5f + 1.0f, label_y + 1.5f, 17, RGB(0x00, 0x00, 0x00), 130, "Snake");
    text_ui(c, cx - label_w * 0.5f, label_y, 17, RGB(0xF6, 0xF8, 0xFB), "Snake");

    text_ui_centered(c, cx, 604.0f, 14, RGB(0xE8, 0xEE, 0xF6), 130,
                     "one application installed  \xc2\xb7  click it or press Enter");

    text_ui_alpha(c, 26.0f, (float)APP_HEIGHT - 30.0f, 12, RGB(0xCB, 0xD5, 0xE1), 90, "DimOS 1.0");
    const char *right = "software renderer  \xc2\xb7  no 3D, no GPU pipeline";
    const float right_w = text_ui_width(12, right);
    text_ui_alpha(c, (float)APP_WIDTH - 26.0f - right_w, (float)APP_HEIGHT - 30.0f, 12, RGB(0xCB, 0xD5, 0xE1), 90, right);

    snapshot_static(app);
}

static void draw_desktop(App *app) {
    Canvas *c = &app->canvas;
    if (app->static_ready != (int)APP_SCREEN_DESKTOP) {
        prepare_desktop(app);
    } else {
        restore_static(app);
    }

    draw_top_bar_clock(app);

    const float scale = 1.0f + 0.035f * app->hover;
    const float size = 104.0f * scale;
    const float cx = (float)APP_WIDTH * 0.5f;
    const float cy = 330.0f;
    if (app->icon_pressed) {
        canvas_round_rect_alpha(c, cx - size * 0.5f, cy - size * 0.5f, size, size, 28.0f, RGB(0x00, 0x00, 0x00), 40);
    }
    draw_snake_badge(c, cx, cy, size, 0.35f + app->hover);

    if (app->hover > 0.02f) {
        text_ui_centered(c, cx, 330.0f + 52.0f + 46.0f, 13, RGB(0x94, 0xA3, 0xB8), (uint8_t)(200.0f * app->hover),
                         "click to open");
    }
}

/* ------------------------------------------------------------------ */
/* Snake screen                                                        */
/* ------------------------------------------------------------------ */

static void prepare_game(App *app) {
    Canvas *c = &app->canvas;
    memcpy(c->pixels, app->wallpaper, (size_t)APP_WIDTH * (size_t)APP_HEIGHT * sizeof(uint32_t));
    canvas_fill_alpha(c, 0.0f, 0.0f, (float)APP_WIDTH, (float)APP_HEIGHT, RGB(0x02, 0x05, 0x08), 165);
    draw_top_bar_base(c);

    snake_draw_background(c);

    const Rect *b = &app->back_button;
    canvas_round_rect_alpha(c, (float)b->x, (float)b->y, (float)b->w, (float)b->h, 12.0f, RGB(0xFF, 0xFF, 0xFF), 16);
    canvas_round_rect_outline(c, (float)b->x, (float)b->y, (float)b->w, (float)b->h, 12.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 40);

    text_ui_bold(c, 222.0f, 56.0f, 32, RGB(0xF8, 0xFA, 0xFC), 255, "Snake");

    /* chip frames; only the numbers inside are repainted per frame */
    float chip_x = (float)(SNAKE_BOARD_X + SNAKE_BOARD_W);
    static const char *labels[2] = {"BEST", "SCORE"};
    static const uint32_t accents[2] = {RGB(0x94, 0xA3, 0xB8), RGB(0x4A, 0xDE, 0x80)};
    for (int i = 1; i >= 0; --i) {
        const float w = 118.0f;
        chip_x -= w;
        canvas_round_rect_alpha(c, chip_x, 52.0f, w, 44.0f, 14.0f, RGB(0xFF, 0xFF, 0xFF), 14);
        canvas_round_rect_outline(c, chip_x, 52.0f, w, 44.0f, 14.0f, 1.0f, RGB(0xFF, 0xFF, 0xFF), 26);
        text_ui(c, chip_x + 14.0f, 58.0f, 10, accents[i], labels[i]);
        chip_x -= 12.0f;
    }

    text_ui_centered(c, (float)APP_WIDTH * 0.5f, (float)(SNAKE_BOARD_Y + SNAKE_BOARD_H) + 16.0f, 13,
                     RGB(0x94, 0xA3, 0xB8), 200, "arrows / WASD to steer  \xc2\xb7  P pause  \xc2\xb7  Esc desktop");

    snapshot_static(app);
}

static void draw_snake_screen(App *app) {
    Canvas *c = &app->canvas;
    if (app->static_ready != (int)APP_SCREEN_SNAKE) {
        prepare_game(app);
    } else {
        restore_static(app);
    }

    draw_top_bar_clock(app);

    /* back button highlight and chevron */
    const Rect *b = &app->back_button;
    if (app->back_hovered) {
        canvas_round_rect_alpha(c, (float)b->x, (float)b->y, (float)b->w, (float)b->h, 12.0f, RGB(0xFF, 0xFF, 0xFF), 14);
    }
    canvas_line(c, (float)b->x + 24.0f, (float)b->y + 12.0f, (float)b->x + 16.0f, (float)b->y + 20.0f, 2.0f,
                RGB(0xE8, 0xEE, 0xF6), 235);
    canvas_line(c, (float)b->x + 16.0f, (float)b->y + 20.0f, (float)b->x + 24.0f, (float)b->y + 28.0f, 2.0f,
                RGB(0xE8, 0xEE, 0xF6), 235);

    /* the two numbers in the chips */
    char buffer[32];
    float chip_x = (float)(SNAKE_BOARD_X + SNAKE_BOARD_W);
    snprintf(buffer, sizeof(buffer), "%d", app->snake.score);
    text_ui_bold(c, chip_x - 118.0f + 14.0f, 70.0f, 22, RGB(0xF8, 0xFA, 0xFC), 255, buffer);
    snprintf(buffer, sizeof(buffer), "%d", app->snake.best);
    text_ui_bold(c, chip_x - 130.0f - 118.0f + 14.0f, 70.0f, 22, RGB(0xF8, 0xFA, 0xFC), 255, buffer);

    snake_draw(c, &app->snake, app->time);

    /* status line under the board */
    snprintf(buffer, sizeof(buffer), "length %d", app->snake.length);
    text_ui(c, (float)SNAKE_BOARD_X, (float)(SNAKE_BOARD_Y + SNAKE_BOARD_H) + 16.0f, 13, RGB(0x64, 0x74, 0x8B), buffer);
    const float speed = 0.150f / (app->snake.interval > 0.0f ? app->snake.interval : 0.150f);
    snprintf(buffer, sizeof(buffer), "%.2fx speed", speed);
    const float speed_w = text_ui_width(13, buffer);
    text_ui(c, (float)(SNAKE_BOARD_X + SNAKE_BOARD_W) - speed_w, (float)(SNAKE_BOARD_Y + SNAKE_BOARD_H) + 16.0f, 13,
            RGB(0x64, 0x74, 0x8B), buffer);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

void app_draw_frame(App *app) {
    Canvas *c = &app->canvas;
    switch (app->screen) {
    case APP_SCREEN_BOOT:
        canvas_clear(c, RGB(0x04, 0x06, 0x09));
        if (app->boot_phase == BOOT_PHASE_LOG) {
            draw_boot_log(app);
        } else {
            draw_boot_login(app);
        }
        break;
    case APP_SCREEN_DESKTOP:
        draw_desktop(app);
        break;
    case APP_SCREEN_SNAKE:
        draw_snake_screen(app);
        break;
    default:
        canvas_clear(c, RGB(0x00, 0x00, 0x00));
        break;
    }

    if (app->fade > 0.0f) {
        canvas_fill_alpha(c, 0.0f, 0.0f, (float)APP_WIDTH, (float)APP_HEIGHT, RGB(0x00, 0x00, 0x00),
                          (uint8_t)(clampf(app->fade, 0.0f, 1.0f) * 255.0f));
    }

    app->first_frame = 0;
    app->input_pending = 0;
}
