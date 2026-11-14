/*
 * draw.c -- software rasterizer for DimOS.
 *
 * Everything is anti-aliased by evaluating a signed distance per pixel
 * instead of drawing into a mask first: at 1280x720 the cost is small and
 * the code stays readable. Text is a real TrueType file (fonts/font.ttf,
 * embedded at build time) rasterized on demand and cached.
 */
#include "draw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* ------------------------------------------------------------------ */
/* Colours and blending                                                */
/* ------------------------------------------------------------------ */

static inline uint8_t clamp_u8(float v) {
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)(v + 0.5f);
}

static inline float clamp01(float v) {
    if (v <= 0.0f) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v;
}

uint32_t color_alpha(uint32_t color, uint8_t alpha) {
    const uint32_t a = PIXEL_A(color);
    const uint32_t scaled = (a * (uint32_t)alpha) / 255u;
    return (color & 0x00FFFFFFu) | (scaled << 24);
}

uint32_t color_lerp(uint32_t a, uint32_t b, float t) {
    t = clamp01(t);
    const float ar = (float)PIXEL_R(a), ag = (float)PIXEL_G(a), ab = (float)PIXEL_B(a), aa = (float)PIXEL_A(a);
    const float br = (float)PIXEL_R(b), bg = (float)PIXEL_G(b), bb = (float)PIXEL_B(b), ba = (float)PIXEL_A(b);
    const uint8_t r = clamp_u8(ar + (br - ar) * t);
    const uint8_t g = clamp_u8(ag + (bg - ag) * t);
    const uint8_t bl = clamp_u8(ab + (bb - ab) * t);
    const uint8_t al = clamp_u8(aa + (ba - aa) * t);
    return PIXEL(al, r, g, bl);
}

/* The canvas is always opaque, so a source-over blend is just a weighted
 * average of the three colour channels. */
static inline void blend_pixel(Canvas *c, int x, int y, uint32_t color, uint8_t alpha) {
    if (x < 0 || y < 0 || x >= c->width || y >= c->height || alpha == 0) {
        return;
    }
    uint32_t *p = &c->pixels[y * c->width + x];
    const uint32_t dst = *p;
    if (alpha == 255) {
        *p = color | 0xFF000000u;
        return;
    }
    const uint32_t sa = alpha;
    const uint32_t da = 255u - sa;
    const uint32_t r = (((uint32_t)PIXEL_R(color) * sa) + ((uint32_t)PIXEL_R(dst) * da)) / 255u;
    const uint32_t g = (((uint32_t)PIXEL_G(color) * sa) + ((uint32_t)PIXEL_G(dst) * da)) / 255u;
    const uint32_t b = (((uint32_t)PIXEL_B(color) * sa) + ((uint32_t)PIXEL_B(dst) * da)) / 255u;
    *p = PIXEL(0xFF, r, g, b);
}

/* ------------------------------------------------------------------ */
/* Rectangles and rounded rectangles                                   */
/* ------------------------------------------------------------------ */

void canvas_clear(Canvas *c, uint32_t color) {
    const uint32_t opaque = color | 0xFF000000u;
    for (int i = 0; i < c->width * c->height; ++i) {
        c->pixels[i] = opaque;
    }
}

void canvas_fill(Canvas *c, float x, float y, float w, float h, uint32_t color) {
    canvas_fill_alpha(c, x, y, w, h, color, 255);
}

void canvas_fill_alpha(Canvas *c, float x, float y, float w, float h, uint32_t color, uint8_t alpha) {
    const int x0 = (int)floorf(x);
    const int y0 = (int)floorf(y);
    const int x1 = (int)ceilf(x + w);
    const int y1 = (int)ceilf(y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            /* Coverage of a pixel centre inside an axis aligned box. */
            const float left = fmaxf((float)px, x);
            const float right = fminf((float)px + 1.0f, x + w);
            const float top = fmaxf((float)py, y);
            const float bottom = fminf((float)py + 1.0f, y + h);
            if (right <= left || bottom <= top) {
                continue;
            }
            const float coverage = (right - left) * (bottom - top);
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

/* Signed distance based coverage of a rounded rectangle. Positive inside. */
static inline float round_rect_coverage(float px, float py, float x, float y, float w, float h, float r) {
    const float half_w = w * 0.5f;
    const float half_h = h * 0.5f;
    if (r > half_w) {
        r = half_w;
    }
    if (r > half_h) {
        r = half_h;
    }
    const float cx = x + half_w;
    const float cy = y + half_h;
    const float qx = fabsf(px - cx) - (half_w - r);
    const float qy = fabsf(py - cy) - (half_h - r);
    const float ax = fmaxf(qx, 0.0f);
    const float ay = fmaxf(qy, 0.0f);
    const float outside = sqrtf(ax * ax + ay * ay);
    const float inside = fminf(fmaxf(qx, qy), 0.0f);
    const float distance = outside + inside - r;
    return clamp01(0.5f - distance);
}

static void round_rect_impl(Canvas *c, float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom, uint8_t alpha) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    const int x0 = (int)floorf(x) - 1;
    const int y0 = (int)floorf(y) - 1;
    const int x1 = (int)ceilf(x + w) + 1;
    const int y1 = (int)ceilf(y + h) + 1;
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        const float v = (h > 0.0f) ? clamp01((((float)py + 0.5f) - y) / h) : 0.0f;
        const uint32_t row = color_lerp(top, bottom, v);
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float coverage = round_rect_coverage((float)px + 0.5f, (float)py + 0.5f, x, y, w, h, radius);
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, row, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

void canvas_round_rect(Canvas *c, float x, float y, float w, float h, float radius, uint32_t color) {
    round_rect_impl(c, x, y, w, h, radius, color, color, 255);
}

void canvas_round_rect_alpha(Canvas *c, float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha) {
    round_rect_impl(c, x, y, w, h, radius, color, color, alpha);
}

void canvas_round_rect_gradient(Canvas *c, float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom) {
    round_rect_impl(c, x, y, w, h, radius, top, bottom, 255);
}

void canvas_round_rect_outline(Canvas *c, float x, float y, float w, float h, float radius, float thickness, uint32_t color, uint8_t alpha) {
    if (w <= 0.0f || h <= 0.0f || thickness <= 0.0f) {
        return;
    }
    const float half_w = w * 0.5f;
    const float half_h = h * 0.5f;
    float r = radius;
    if (r > half_w) {
        r = half_w;
    }
    if (r > half_h) {
        r = half_h;
    }
    const int x0 = (int)floorf(x) - 2;
    const int y0 = (int)floorf(y) - 2;
    const int x1 = (int)ceilf(x + w) + 2;
    const int y1 = (int)ceilf(y + h) + 2;
    const float cx = x + half_w;
    const float cy = y + half_h;
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float qx = fabsf(((float)px + 0.5f) - cx) - (half_w - r);
            const float qy = fabsf(((float)py + 0.5f) - cy) - (half_h - r);
            const float ax = fmaxf(qx, 0.0f);
            const float ay = fmaxf(qy, 0.0f);
            const float distance = sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0.0f) - r;
            const float coverage = clamp01(0.5f - (fabsf(distance) - thickness * 0.5f));
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

void canvas_shadow(Canvas *c, float x, float y, float w, float h, float radius, float spread, uint8_t alpha) {
    const int layers = 6;
    for (int i = layers; i >= 1; --i) {
        const float t = (float)i / (float)layers;
        const float grow = spread * t;
        const uint8_t a = (uint8_t)clamp_u8((float)alpha * (1.0f - t) * 0.55f);
        canvas_round_rect_alpha(c, x - grow, y - grow + spread * 0.35f, w + grow * 2.0f, h + grow * 2.0f, radius + grow, 0x00000000u, a);
    }
}

/* ------------------------------------------------------------------ */
/* Circles, rings, lines                                               */
/* ------------------------------------------------------------------ */

void canvas_circle(Canvas *c, float cx, float cy, float radius, uint32_t color) {
    canvas_circle_alpha(c, cx, cy, radius, color, 255);
}

void canvas_circle_alpha(Canvas *c, float cx, float cy, float radius, uint32_t color, uint8_t alpha) {
    if (radius <= 0.0f) {
        return;
    }
    const int x0 = (int)floorf(cx - radius) - 1;
    const int y0 = (int)floorf(cy - radius) - 1;
    const int x1 = (int)ceilf(cx + radius) + 1;
    const int y1 = (int)ceilf(cy + radius) + 1;
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            const float coverage = clamp01(radius + 0.5f - sqrtf(dx * dx + dy * dy));
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

void canvas_ring(Canvas *c, float cx, float cy, float radius, float thickness, uint32_t color, uint8_t alpha) {
    if (radius <= 0.0f || thickness <= 0.0f) {
        return;
    }
    const float outer = radius + thickness * 0.5f;
    const int x0 = (int)floorf(cx - outer) - 1;
    const int y0 = (int)floorf(cy - outer) - 1;
    const int x1 = (int)ceilf(cx + outer) + 1;
    const int y1 = (int)ceilf(cy + outer) + 1;
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            const float d = fabsf(sqrtf(dx * dx + dy * dy) - radius);
            const float coverage = clamp01(0.5f + thickness * 0.5f - d);
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

void canvas_line(Canvas *c, float x0, float y0, float x1, float y1, float thickness, uint32_t color, uint8_t alpha) {
    const float half = fmaxf(thickness * 0.5f, 0.5f);
    const int minx = (int)floorf(fminf(x0, x1) - half) - 1;
    const int miny = (int)floorf(fminf(y0, y1) - half) - 1;
    const int maxx = (int)ceilf(fmaxf(x0, x1) + half) + 1;
    const int maxy = (int)ceilf(fmaxf(y0, y1) + half) + 1;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len_sq = dx * dx + dy * dy;
    for (int py = miny; py < maxy; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = minx; px < maxx; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float sx = ((float)px + 0.5f) - x0;
            const float sy = ((float)py + 0.5f) - y0;
            float t = (len_sq > 0.0f) ? (sx * dx + sy * dy) / len_sq : 0.0f;
            t = clamp01(t);
            const float ddx = sx - dx * t;
            const float ddy = sy - dy * t;
            const float distance = sqrtf(ddx * ddx + ddy * ddy);
            const float coverage = clamp01(half + 0.5f - distance);
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

void canvas_radial_glow(Canvas *c, float cx, float cy, float radius, uint32_t color, uint8_t max_alpha) {
    if (radius <= 0.0f || max_alpha == 0) {
        return;
    }
    const int x0 = (int)floorf(cx - radius);
    const int y0 = (int)floorf(cy - radius);
    const int x1 = (int)ceilf(cx + radius);
    const int y1 = (int)ceilf(cy + radius);
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            const float d = sqrtf(dx * dx + dy * dy) / radius;
            if (d >= 1.0f) {
                continue;
            }
            const float falloff = (1.0f - d) * (1.0f - d);
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)max_alpha * falloff));
        }
    }
}

void canvas_ring_arc(Canvas *c, float cx, float cy, float radius, float thickness, float start_deg, float end_deg,
                     uint32_t color, uint8_t alpha) {
    if (radius <= 0.0f || thickness <= 0.0f) {
        return;
    }
    while (start_deg < 0.0f) {
        start_deg += 360.0f;
        end_deg += 360.0f;
    }
    while (end_deg < start_deg) {
        end_deg += 360.0f;
    }
    const float outer = radius + thickness * 0.5f;
    const int x0 = (int)floorf(cx - outer) - 1;
    const int y0 = (int)floorf(cy - outer) - 1;
    const int x1 = (int)ceilf(cx + outer) + 1;
    const int y1 = (int)ceilf(cy + outer) + 1;
    for (int py = y0; py < y1; ++py) {
        if (py < 0 || py >= c->height) {
            continue;
        }
        for (int px = x0; px < x1; ++px) {
            if (px < 0 || px >= c->width) {
                continue;
            }
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            float angle = atan2f(dy, dx) * 57.29577951f;
            if (angle < 0.0f) {
                angle += 360.0f;
            }
            if (angle < start_deg || angle > end_deg) {
                continue;
            }
            const float d = fabsf(sqrtf(dx * dx + dy * dy) - radius);
            const float coverage = clamp01(0.5f + thickness * 0.5f - d);
            if (coverage <= 0.0f) {
                continue;
            }
            blend_pixel(c, px, py, color, (uint8_t)clamp_u8((float)alpha * coverage));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Text: TrueType for the UI                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int used;
    int codepoint;
    int px;
    int w;
    int h;
    int xoff;
    int yoff;
    float advance;
    unsigned char *bitmap;
} Glyph;

#define GLYPH_CACHE_SIZE 2048

static struct {
    stbtt_fontinfo info;
    int ready;
    float ascent;
    float descent;
    float line_gap;
    Glyph cache[GLYPH_CACHE_SIZE];
    int count;
} g_text;

int text_init(const unsigned char *ttf, size_t ttf_size) {
    memset(&g_text, 0, sizeof(g_text));
    if (!ttf || ttf_size == 0) {
        return 0;
    }
    const int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (offset < 0 || !stbtt_InitFont(&g_text.info, ttf, offset)) {
        return 0;
    }
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&g_text.info, &ascent, &descent, &line_gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_text.info, 16.0f);
    g_text.ascent = (float)ascent * scale;
    g_text.descent = (float)descent * scale;
    g_text.line_gap = (float)line_gap * scale;
    g_text.ready = 1;
    return 1;
}

static const Glyph *glyph_get(int codepoint, int px) {
    for (int i = 0; i < g_text.count; ++i) {
        if (g_text.cache[i].used && g_text.cache[i].codepoint == codepoint && g_text.cache[i].px == px) {
            return &g_text.cache[i];
        }
    }
    if (!g_text.ready || g_text.count >= GLYPH_CACHE_SIZE) {
        return NULL;
    }
    Glyph *g = &g_text.cache[g_text.count++];
    memset(g, 0, sizeof(*g));
    g->used = 1;
    g->codepoint = codepoint;
    g->px = px;
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_text.info, codepoint, &advance, &lsb);
    const float scale = stbtt_ScaleForPixelHeight(&g_text.info, (float)px);
    g->advance = (float)advance * scale;
    int xoff = 0, yoff = 0;
    unsigned char *bitmap = stbtt_GetCodepointBitmap(&g_text.info, scale, scale, codepoint, &g->w, &g->h, &xoff, &yoff);
    g->bitmap = bitmap;
    g->xoff = xoff;
    g->yoff = yoff;
    return g;
}

/* Baseline of a line of text whose top-left corner is (x, y). */
static float baseline_offset(int px) {
    if (!g_text.ready) {
        return (float)px * 0.8f;
    }
    const float scale = stbtt_ScaleForPixelHeight(&g_text.info, (float)px);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&g_text.info, &ascent, &descent, &line_gap);
    return (float)ascent * scale;
}

static int utf8_next(const char **cursor) {
    const unsigned char *s = (const unsigned char *)*cursor;
    if (!*s) {
        return 0;
    }
    int cp = 0;
    int extra = 0;
    if (s[0] < 0x80) {
        cp = s[0];
        extra = 0;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1F;
        extra = 1;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0F;
        extra = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07;
        extra = 3;
    } else {
        *cursor += 1;
        return '?';
    }
    for (int i = 0; i < extra; ++i) {
        if (!s[1 + i]) {
            *cursor += 1 + i;
            return '?';
        }
        cp = (cp << 6) | (s[1 + i] & 0x3F);
    }
    *cursor += 1 + extra;
    return cp;
}

float text_ui_width(int px, const char *text) {
    if (!text) {
        return 0.0f;
    }
    float width = 0.0f;
    const char *cursor = text;
    for (;;) {
        const int cp = utf8_next(&cursor);
        if (cp == 0) {
            break;
        }
        const Glyph *g = glyph_get(cp, px);
        width += g ? g->advance : (float)px * 0.5f;
    }
    return width;
}

float text_ui_height(int px) {
    return (float)px;
}

void text_ui_alpha(Canvas *c, float x, float y, int px, uint32_t color, uint8_t alpha, const char *text) {
    if (!text) {
        return;
    }
    const float baseline = y + baseline_offset(px);
    float pen = x;
    const char *cursor = text;
    for (;;) {
        const int cp = utf8_next(&cursor);
        if (cp == 0) {
            break;
        }
        const Glyph *g = glyph_get(cp, px);
        if (!g) {
            continue;
        }
        if (g->bitmap && g->w > 0 && g->h > 0) {
            const int ox = (int)floorf(pen + (float)g->xoff);
            const int oy = (int)floorf(baseline + (float)g->yoff);
            for (int gy = 0; gy < g->h; ++gy) {
                const unsigned char *row = g->bitmap + (size_t)gy * (size_t)g->w;
                for (int gx = 0; gx < g->w; ++gx) {
                    const uint8_t coverage = row[gx];
                    if (!coverage) {
                        continue;
                    }
                    const uint8_t a = (uint8_t)(((uint32_t)coverage * (uint32_t)alpha) / 255u);
                    blend_pixel(c, ox + gx, oy + gy, color, a);
                }
            }
        }
        pen += g->advance;
    }
}

void text_ui(Canvas *c, float x, float y, int px, uint32_t color, const char *text) {
    text_ui_alpha(c, x, y, px, color, 255, text);
}

void text_ui_bold(Canvas *c, float x, float y, int px, uint32_t color, uint8_t alpha, const char *text) {
    /* There is a single weight in the bundled font: drawing the same glyphs
     * twice with a sub-pixel offset fakes a semibold cut convincingly. */
    text_ui_alpha(c, x, y, px, color, alpha, text);
    text_ui_alpha(c, x + fmaxf(1.0f, (float)px * 0.045f), y, px, color, (uint8_t)((uint32_t)alpha * 3u / 4u), text);
}

void text_ui_centered(Canvas *c, float cx, float y, int px, uint32_t color, uint8_t alpha, const char *text) {
    const float width = text_ui_width(px, text);
    text_ui_alpha(c, cx - width * 0.5f, y, px, color, alpha, text);
}
