/*
 * draw.h -- the tiny software renderer every DimOS screen is made of.
 *
 * The application never talks to the GPU directly: it paints 32-bit ARGB
 * pixels into one 1280x720 canvas, which the sokol backend uploads as a
 * texture and stretches over the window. That keeps the UI code identical
 * for the real window, for the headless PNG renderer and for the tests.
 */
#ifndef DIMOS_DRAW_H
#define DIMOS_DRAW_H

#include <stddef.h>
#include <stdint.h>

/* The canvas is stored in RGBA byte order -- red at the lowest address --
 * which is exactly what the GPU texture format wants, so presenting a frame
 * is a memcpy and never a per-pixel swizzle. */
#define PIXEL(a, r, g, b) ((uint32_t)((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(b) & 0xFFu) << 16) | (((uint32_t)(g) & 0xFFu) << 8) | ((uint32_t)(r) & 0xFFu)))
#define RGB(r, g, b) PIXEL(0xFF, r, g, b)
#define PIXEL_R(c) ((uint8_t)((c) & 0xFFu))
#define PIXEL_G(c) ((uint8_t)(((c) >> 8) & 0xFFu))
#define PIXEL_B(c) ((uint8_t)(((c) >> 16) & 0xFFu))
#define PIXEL_A(c) ((uint8_t)(((c) >> 24) & 0xFFu))

typedef struct Canvas {
    int width;
    int height;
    uint32_t *pixels; /* RGBA8 in memory: R,G,B,A bytes, alpha in the top byte */
} Canvas;

uint32_t color_alpha(uint32_t color, uint8_t alpha);
uint32_t color_lerp(uint32_t a, uint32_t b, float t);

void canvas_clear(Canvas *c, uint32_t color);
void canvas_fill(Canvas *c, float x, float y, float w, float h, uint32_t color);
void canvas_fill_alpha(Canvas *c, float x, float y, float w, float h, uint32_t color, uint8_t alpha);
void canvas_round_rect(Canvas *c, float x, float y, float w, float h, float radius, uint32_t color);
void canvas_round_rect_alpha(Canvas *c, float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha);
void canvas_round_rect_gradient(Canvas *c, float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom);
void canvas_round_rect_outline(Canvas *c, float x, float y, float w, float h, float radius, float thickness, uint32_t color, uint8_t alpha);
void canvas_shadow(Canvas *c, float x, float y, float w, float h, float radius, float spread, uint8_t alpha);
void canvas_circle(Canvas *c, float cx, float cy, float radius, uint32_t color);
void canvas_circle_alpha(Canvas *c, float cx, float cy, float radius, uint32_t color, uint8_t alpha);
void canvas_ring(Canvas *c, float cx, float cy, float radius, float thickness, uint32_t color, uint8_t alpha);
void canvas_line(Canvas *c, float x0, float y0, float x1, float y1, float thickness, uint32_t color, uint8_t alpha);
/* Smooth falloff glow: alpha fades from max_alpha in the centre to zero at
 * the radius, squared so there is no visible banding. */
void canvas_radial_glow(Canvas *c, float cx, float cy, float radius, uint32_t color, uint8_t max_alpha);
/* Ring segment; angles in degrees, 0 pointing right and growing clockwise. */
void canvas_ring_arc(Canvas *c, float cx, float cy, float radius, float thickness, float start_deg, float end_deg,
                     uint32_t color, uint8_t alpha);

/* Text: a real TrueType file, rasterized on demand and cached. */
int text_init(const unsigned char *ttf, size_t ttf_size);
void text_ui(Canvas *c, float x, float y, int px, uint32_t color, const char *text);
void text_ui_alpha(Canvas *c, float x, float y, int px, uint32_t color, uint8_t alpha, const char *text);
void text_ui_bold(Canvas *c, float x, float y, int px, uint32_t color, uint8_t alpha, const char *text);
void text_ui_centered(Canvas *c, float cx, float y, int px, uint32_t color, uint8_t alpha, const char *text);
float text_ui_width(int px, const char *text);
float text_ui_height(int px);

#endif
