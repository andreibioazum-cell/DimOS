/*
 * app.h -- DimOS itself: a boot sequence, a desktop and one application.
 *
 * The whole operating system is a single process with one window. The
 * platform layer (sokol_app + a two-triangle blit) only supplies a window,
 * input events and one texture; every pixel of the interface is produced by
 * draw.c on the CPU.
 */
#ifndef DIMOS_APP_H
#define DIMOS_APP_H

#include <stddef.h>
#include <stdint.h>

#include "draw.h"
#include "snake.h"

#define APP_WIDTH 1280
#define APP_HEIGHT 720

typedef enum {
    APP_SCREEN_BOOT = 0,
    APP_SCREEN_DESKTOP = 1,
    APP_SCREEN_SNAKE = 2
} AppScreen;

typedef enum {
    INPUT_MOUSE_MOVE = 0,
    INPUT_MOUSE_DOWN,
    INPUT_MOUSE_UP,
    INPUT_MOUSE_WHEEL,
    INPUT_KEY_DOWN
} InputType;

typedef enum {
    KEY_UNKNOWN = 0,
    KEY_ESCAPE,
    KEY_ENTER,
    KEY_SPACE,
    KEY_TAB,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_A,
    KEY_D,
    KEY_W,
    KEY_S,
    KEY_P,
    KEY_R,
    KEY_F1
} AppKey;

typedef struct {
    InputType type;
    float x;      /* canvas coordinates, already mapped by the platform layer */
    float y;
    AppKey key;
    float wheel;
} InputEvent;

typedef struct App App;

App *app_create(const unsigned char *ttf, size_t ttf_size);
void app_destroy(App *app);

/* Called once per frame before app_update(): false means nothing at all is
 * animating, so the platform layer can skip the whole frame (no redraw, no
 * texture upload, no GPU work). This is what makes an idle desktop free. */
int app_needs_frame(const App *app);
void app_update(App *app, float dt);
void app_draw_frame(App *app);
void app_input(App *app, const InputEvent *event);

Canvas *app_canvas(App *app);
AppScreen app_screen(const App *app);
void app_goto(App *app, AppScreen screen);
/* Skip the built-in console: used by the live ISO, which boots a real kernel. */
void app_skip_boot(App *app);
int app_quit_requested(const App *app);

#endif
