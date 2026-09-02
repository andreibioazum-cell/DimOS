/*
 * gfx.c -- everything that touches a pixel.
 *
 * DimOS draws in VGA mode 13h: 320x200, one byte per pixel, the picture lives
 * at 0xA0000. We compose each frame in our own buffer (BACK_BUFFER_ADDRESS)
 * and copy it to the video card once per frame, so nothing ever flickers.
 *
 * Text is drawn with 8x8 glyphs. When the boot disk carries FONT.TTF
 * (put any TrueType file into fonts/ before building), font_ttf.c
 * rasterizes it at boot and that table is used for every character.
 * Otherwise text falls back to the 8x8 font every VGA BIOS carries:
 * kernel.asm asks the video BIOS where that font is and stores the
 * address in bios_font_address.
 */

#include "dimos.h"

/* Filled by kernel.asm while the machine is still in real mode. */
extern u32 bios_font_address;

/* The buffer we draw into. screen[0] is the top left pixel. */
u8 *const screen = (u8 *)BACK_BUFFER_ADDRESS;

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 red;
    u8 green;
    u8 blue;
} Color;

/* The sixteen classic DOS colours first, then the window manager roles. */
static const Color palette[PALETTE_SIZE] = {
    { 0, 0, 0 },         /* COLOR_BLACK       */
    { 0, 0, 170 },       /* COLOR_BLUE        */
    { 0, 170, 0 },       /* COLOR_GREEN       */
    { 0, 170, 170 },     /* COLOR_CYAN        */
    { 170, 0, 0 },       /* COLOR_RED         */
    { 170, 0, 170 },     /* COLOR_MAGENTA     */
    { 170, 85, 0 },      /* COLOR_BROWN       */
    { 170, 170, 170 },   /* COLOR_LIGHT_GRAY  */
    { 85, 85, 85 },      /* COLOR_DARK_GRAY   */
    { 85, 85, 255 },     /* COLOR_LIGHT_BLUE  */
    { 85, 255, 85 },     /* COLOR_LIGHT_GREEN */
    { 85, 255, 255 },    /* COLOR_LIGHT_CYAN  */
    { 255, 85, 85 },     /* COLOR_LIGHT_RED   */
    { 255, 85, 255 },    /* COLOR_LIGHT_MAGENTA */
    { 255, 255, 85 },    /* COLOR_YELLOW      */
    { 255, 255, 255 },   /* COLOR_WHITE       */
    { 192, 192, 192 },   /* COLOR_FACE        */
    { 128, 128, 128 },   /* COLOR_SHADOW      */
    { 255, 255, 255 },   /* COLOR_HILITE      */
    { 0, 0, 128 },       /* COLOR_TITLE_BAR   */
    { 0, 128, 128 },     /* COLOR_DESKTOP     */
    { 255, 255, 255 },   /* COLOR_TEXT_FIELD  */
    { 0, 0, 170 },       /* COLOR_SELECTION   */
    { 128, 128, 128 },   /* COLOR_DISABLED    */
    { 255, 255, 0 },     /* COLOR_ACCENT      */
    { 0, 170, 0 },       /* COLOR_GOOD        */
    { 255, 85, 0 },      /* COLOR_ALERT       */
    { 160, 160, 160 },   /* COLOR_PANEL       */
    { 255, 255, 255 },   /* COLOR_CANVAS      */
    { 96, 96, 96 },      /* COLOR_GRID        */
    { 32, 32, 64 },      /* COLOR_DEEP        */
    { 0, 0, 0 }          /* COLOR_CURSOR      */
};

static u8 current_theme = THEME_COLOR;

/* Turn a colour into the shade a monochrome monitor would show. A green or
 * amber tube keeps the brightness of the original colour and drops the rest,
 * which is exactly how those phosphors behaved. */
static Color themed(Color original, u8 theme) {
    u16 brightness;

    if (theme == THEME_COLOR) {
        return original;
    }

    brightness = (u16)(((u32)original.red * 30u +
                        (u32)original.green * 59u +
                        (u32)original.blue * 11u) / 100u);

    if (theme == THEME_GREEN) {
        Color result;
        result.red = (u8)(brightness / 6u);
        result.green = (u8)brightness;
        result.blue = (u8)(brightness / 6u);
        return result;
    }
    if (theme == THEME_AMBER) {
        Color result;
        result.red = (u8)brightness;
        result.green = (u8)((brightness * 2u) / 3u);
        result.blue = 0u;
        return result;
    }

    {
        Color result;
        result.red = (u8)brightness;
        result.green = (u8)brightness;
        result.blue = (u8)brightness;
        return result;
    }
}

/* The video card stores six bits per channel, our table has eight. */
static void palette_write_entry(u8 index, Color value) {
    port_write_byte(PORT_VGA_DAC_WRITE_INDEX, index);
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.red >> 2));
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.green >> 2));
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.blue >> 2));
}

static void palette_apply(void) {
    Color black;
    u16 index;

    black.red = 0u;
    black.green = 0u;
    black.blue = 0u;

    for (index = 0u; index < PALETTE_SIZE; ++index) {
        palette_write_entry((u8)index, themed(palette[index], current_theme));
    }
    /* The slots we never draw with stay black instead of the rainbow the
     * video BIOS leaves behind after a mode change. */
    for (index = PALETTE_SIZE; index < 256u; ++index) {
        palette_write_entry((u8)index, black);
    }
}

void gfx_select_theme(u8 theme) {
    if (theme >= THEME_COUNT) {
        theme = THEME_COLOR;
    }
    current_theme = theme;
    palette_apply();
}

u8 gfx_current_theme(void) {
    return current_theme;
}

/* ------------------------------------------------------------------ */
/* Font                                                                */
/* ------------------------------------------------------------------ */

static const u8 *font_glyphs;
static u8 font_ready;

/* The BIOS font is only usable if it really looks like a font: the space is
 * empty, letters are not, and the table as a whole has a sane amount of ink.
 * Anything else means the video BIOS did not answer, and we draw boxes so the
 * screen still says something instead of showing random ROM bytes. */
static u8 font_looks_valid(const u8 *glyphs) {
    u16 index;
    u32 ink = 0u;
    u8 row;

    if (glyphs == (const u8 *)0) {
        return 0u;
    }
    for (row = 0u; row < 8u; ++row) {
        if (glyphs[(u16)' ' * 8u + row] != 0u) {
            return 0u;
        }
        if (glyphs[row] != 0u) { /* glyph 0 must be blank too */
            return 0u;
        }
    }
    /* Every letter must carry some ink (the bottom row is usually blank, so
     * look at the whole glyph, not at each row). */
    for (index = (u16)'A'; index <= (u16)'Z'; ++index) {
        u8 letter;
        u8 any = 0u;

        for (letter = 0u; letter < 8u; ++letter) {
            any = (u8)(any | glyphs[index * 8u + letter]);
        }
        if (any == 0u) {
            return 0u;
        }
    }
    for (index = 0u; index < BIOS_FONT_BYTES; ++index) {
        u8 bits = glyphs[index];
        while (bits != 0u) {
            ink += (u32)(bits & 1u);
            bits = (u8)(bits >> 1);
        }
    }
    return (ink > 800u && ink < 12000u) ? 1u : 0u;
}

static void font_init(void) {
    /* First choice: the real TrueType font from FONT.TTF on the boot
     * disk, rasterized by font_ttf.c. Drop any TTF into fonts/ and the
     * whole desktop is drawn with it. */
    if (font_ttf_load() != 0u) {
        font_glyphs = font_ttf_table();
        font_ready = 1u;
        return;
    }
    /* Otherwise: the 8x8 font of the video BIOS. */
    font_glyphs = (const u8 *)(u32)bios_font_address;
    font_ready = font_looks_valid(font_glyphs);
}

/* ------------------------------------------------------------------ */
/* Frame buffer                                                        */
/* ------------------------------------------------------------------ */

void gfx_show(void) {
    /* Both buffers start on a four byte boundary and SCREEN_BYTES is a
     * multiple of four, so copying whole words is safe and quick. */
    const u32 *source = (const u32 *)BACK_BUFFER_ADDRESS;
    volatile u32 *target = (volatile u32 *)0x000A0000u;
    u32 words = SCREEN_BYTES / 4u;
    u32 index;

    for (index = 0u; index < words; ++index) {
        target[index] = source[index];
    }
}

void gfx_clear(u8 color) {
    u32 index;

    for (index = 0u; index < SCREEN_BYTES; ++index) {
        screen[index] = color;
    }
}

void gfx_pixel(s16 x, s16 y, u8 color) {
    if (x < 0 || y < 0) {
        return;
    }
    if ((u16)x >= SCREEN_WIDTH || (u16)y >= SCREEN_HEIGHT) {
        return;
    }
    screen[(u16)y * SCREEN_WIDTH + (u16)x] = color;
}

void gfx_horizontal_line(s16 x, s16 y, s16 length, u8 color) {
    s16 step;

    if (y < 0 || (u16)y >= SCREEN_HEIGHT) {
        return;
    }
    for (step = 0; step < length; ++step) {
        gfx_pixel((s16)(x + step), y, color);
    }
}

void gfx_vertical_line(s16 x, s16 y, s16 length, u8 color) {
    s16 step;

    if (x < 0 || (u16)x >= SCREEN_WIDTH) {
        return;
    }
    for (step = 0; step < length; ++step) {
        gfx_pixel(x, (s16)(y + step), color);
    }
}

void gfx_fill(s16 x, s16 y, s16 width, s16 height, u8 color) {
    s16 row;

    for (row = 0; row < height; ++row) {
        gfx_horizontal_line(x, (s16)(y + row), width, color);
    }
}

void gfx_outline(s16 x, s16 y, s16 width, s16 height, u8 color) {
    gfx_horizontal_line(x, y, width, color);
    gfx_horizontal_line(x, (s16)(y + height - 1), width, color);
    gfx_vertical_line(x, y, height, color);
    gfx_vertical_line((s16)(x + width - 1), y, height, color);
}

/* Bresenham's line: step along the longer axis and nudge the other one. */
void gfx_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 color) {
    s16 dx = (s16)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    s16 dy = (s16)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    s16 step_x = (x0 < x1) ? 1 : -1;
    s16 step_y = (y0 < y1) ? 1 : -1;
    s16 error = (s16)(dx - dy);

    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        if ((s16)(2 * error) > -dy) {
            error = (s16)(error - dy);
            x0 = (s16)(x0 + step_x);
        }
        if ((s16)(2 * error) < dx) {
            error = (s16)(error + dx);
            y0 = (s16)(y0 + step_y);
        }
    }
}

/* Midpoint circle. Draws the outline, or every row when filled. */
void gfx_circle(s16 center_x, s16 center_y, s16 radius, u8 color, u8 filled) {
    s16 x = radius;
    s16 y = 0;
    s16 decision = (s16)(1 - radius);

    while (x >= y) {
        if (filled != 0u) {
            gfx_horizontal_line((s16)(center_x - x), (s16)(center_y + y),
                                (s16)(2 * x + 1), color);
            gfx_horizontal_line((s16)(center_x - y), (s16)(center_y + x),
                                (s16)(2 * y + 1), color);
            gfx_horizontal_line((s16)(center_x - x), (s16)(center_y - y),
                                (s16)(2 * x + 1), color);
            gfx_horizontal_line((s16)(center_x - y), (s16)(center_y - x),
                                (s16)(2 * y + 1), color);
        } else {
            gfx_pixel((s16)(center_x + x), (s16)(center_y + y), color);
            gfx_pixel((s16)(center_x - x), (s16)(center_y + y), color);
            gfx_pixel((s16)(center_x + x), (s16)(center_y - y), color);
            gfx_pixel((s16)(center_x - x), (s16)(center_y - y), color);
            gfx_pixel((s16)(center_x + y), (s16)(center_y + x), color);
            gfx_pixel((s16)(center_x - y), (s16)(center_y + x), color);
            gfx_pixel((s16)(center_x + y), (s16)(center_y - x), color);
            gfx_pixel((s16)(center_x - y), (s16)(center_y - x), color);
        }
        ++y;
        if (decision <= 0) {
            decision = (s16)(decision + 2 * y + 1);
        } else {
            --x;
            decision = (s16)(decision + 2 * y - 2 * x + 1);
        }
    }
}

/* Two colour checkerboard, the classic way to fake a third colour. */
void gfx_checker(s16 x, s16 y, s16 width, s16 height, u8 first, u8 second) {
    s16 row;
    s16 column;

    for (row = 0; row < height; ++row) {
        for (column = 0; column < width; ++column) {
            const u8 color = (((row + column) & 1) == 0) ? first : second;
            gfx_pixel((s16)(x + column), (s16)(y + row), color);
        }
    }
}

/* The 3D border every window and button of the era was built from: bright on
 * the top and left, dark on the bottom and right (or the other way round for
 * a button that is being pressed). */
void gfx_raised_box(s16 x, s16 y, s16 width, s16 height, u8 raised) {
    const u8 light = (raised != 0u) ? COLOR_HILITE : COLOR_SHADOW;
    const u8 dark = (raised != 0u) ? COLOR_SHADOW : COLOR_HILITE;

    gfx_horizontal_line(x, y, width, light);
    gfx_vertical_line(x, y, height, light);
    gfx_horizontal_line(x, (s16)(y + height - 1), width, dark);
    gfx_vertical_line((s16)(x + width - 1), y, height, dark);
}

/* A sunken panel: dark outside, light inside, content area in between. */
void gfx_panel(s16 x, s16 y, s16 width, s16 height) {
    gfx_outline(x, y, width, height, COLOR_SHADOW);
    gfx_horizontal_line((s16)(x + 1), (s16)(y + 1), (s16)(width - 2), COLOR_HILITE);
    gfx_vertical_line((s16)(x + 1), (s16)(y + 1), (s16)(height - 2), COLOR_HILITE);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

void gfx_character(s16 x, s16 y, char character, u8 color) {
    u8 row;
    u8 column;
    const u8 *glyph;

    if (character < 32 || character > 126) {
        character = '?';
    }
    if (font_ready == 0u) {
        /* No usable BIOS font: show a hollow box so text is still visible. */
        gfx_outline(x, y, 7, 8, color);
        return;
    }

    glyph = font_glyphs + (u16)((u8)character * 8u);
    for (row = 0u; row < 8u; ++row) {
        const u8 bits = glyph[row];
        for (column = 0u; column < 8u; ++column) {
            if ((u8)(bits & (u8)(0x80u >> column)) != 0u) {
                gfx_pixel((s16)(x + (s16)column), (s16)(y + (s16)row), color);
            }
        }
    }
}

void gfx_text(s16 x, s16 y, const char *text, u8 color) {
    while (*text != '\0') {
        gfx_character(x, y, *text, color);
        x = (s16)(x + 8);
        ++text;
    }
}

void gfx_text_filled(s16 x, s16 y, const char *text, u8 foreground, u8 background) {
    while (*text != '\0') {
        gfx_fill(x, y, 8, 8, background);
        gfx_character(x, y, *text, foreground);
        x = (s16)(x + 8);
        ++text;
    }
}

void gfx_text_centered(s16 x, s16 y, s16 width, const char *text, u8 color) {
    const u16 used = gfx_text_width(text);
    s16 start = x;

    if (used < (u16)width) {
        start = (s16)(x + (s16)((width - (s16)used) / 2));
    }
    gfx_text(start, y, text, color);
}

u16 gfx_text_width(const char *text) {
    u16 width = 0u;

    while (*text != '\0') {
        width = (u16)(width + 8u);
        ++text;
    }
    return width;
}

/* ------------------------------------------------------------------ */
/* Pictures written as ASCII art                                       */
/* ------------------------------------------------------------------ */

/* Art rows use '.' for "leave the pixel alone" and anything else for "draw".
 * Icons and the mouse pointer are written that way because a picture in the
 * source is far easier to read than a table of bytes. */
void gfx_picture(s16 x, s16 y, const char *const *art, u16 rows, u8 color, u8 background) {
    u16 row;

    for (row = 0u; row < rows; ++row) {
        const char *line = art[row];
        s16 column = 0;

        while (*line != '\0') {
            if (*line != '.') {
                gfx_pixel((s16)(x + column), (s16)(y + (s16)row),
                          (*line == 'o') ? background : color);
            }
            ++column;
            ++line;
        }
    }
}

void gfx_picture_opaque(s16 x, s16 y, const char *const *art, u16 rows, u8 on, u8 off) {
    u16 row;

    for (row = 0u; row < rows; ++row) {
        const char *line = art[row];
        s16 column = 0;

        while (*line != '\0') {
            gfx_pixel((s16)(x + column), (s16)(y + (s16)row),
                      (*line == '.') ? off : on);
            ++column;
            ++line;
        }
    }
}

/* The mouse pointer: a black arrow with a white edge, drawn last so it is
 * always on top. 'X' is the arrow, 'o' is the outline. */
static const char *const pointer_art[16] = {
    "Xo........",
    "XXo.......",
    "XXXo......",
    "XXXXo.....",
    "XXXXXo....",
    "XXXXXXo...",
    "XXXXXXXo..",
    "XXXXXXXXo.",
    "XXXXXXXXXo",
    "XXXXXXXooo",
    "XXXXoXXo..",
    "XXXo.XXXo.",
    "XXo...XXXo",
    "Xo.....XXo",
    "o.......oo",
    "..........",
};

void gfx_draw_pointer(s16 x, s16 y) {
    gfx_picture(x, y, pointer_art, 16u, COLOR_CURSOR, COLOR_HILITE);
}

void gfx_init(void) {
    font_init();
    palette_apply();
    gfx_clear(COLOR_DESKTOP);
}
