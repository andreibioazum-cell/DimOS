/*
 * gfx.c -- everything that touches a pixel.
 *
 * DimOS composes a 320x200 indexed canvas in its own back buffer. The preferred
 * VBE path expands changed pixels into a full 1920x1080 XRGB8888 hardware
 * framebuffer; 640x480 RGB565 and VGA mode 13h remain fallbacks. A shadow
 * buffer keeps unchanged pixels off the slow video bus, so a static desktop
 * costs comparisons rather than multi-megabyte framebuffer writes.
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

/* The canvas we draw into and the last canvas successfully presented. */
u8 *const screen = (u8 *)BACK_BUFFER_ADDRESS;
static u8 *const presented = (u8 *)PRESENT_BUFFER_ADDRESS;
static u16 rgb565_color[256];
static u32 xrgb8888_color[256];
static u8 first_present = 1u;

/* The 320x200 canvas is deliberately kept for the tiny applications, but a
 * 1920x1080 VBE screen should not blow an 8x8 glyph up into a 48x48 block.
 * On the full-HD path text is queued and rasterized directly onto the real
 * framebuffer at a readable 3x glyph scale. Two command lists let us erase the previous
 * overlay without redrawing the whole multi-megapixel screen. */
#define HIGH_TEXT_LIMIT 160u
#define HIGH_TEXT_LENGTH 64u

typedef struct {
    s16 x;
    s16 y;
    u8 color;
    char text[HIGH_TEXT_LENGTH];
} HighText;

static HighText high_text_current[HIGH_TEXT_LIMIT];
static HighText high_text_previous[HIGH_TEXT_LIMIT];
static u16 high_text_current_count;
static u16 high_text_previous_count;

/* Software output window selected by the startup display wizard. The BIOS
 * mode remains the real framebuffer, while the smaller target lets v86 and
 * other emulators touch far fewer physical pixels per frame. */
static u16 render_width;
static u16 render_height;
static u16 render_left;
static u16 render_top;

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 red;
    u8 green;
    u8 blue;
} Color;

/* The sixteen classic DOS colours first, then the window manager roles.
 * The role values are the Greybird palette DimXfce borrows: warm grey
 * windows, a bright selection blue, a white canvas and a deep graphite
 * blue that gradients into both the wallpaper and the title bars. */
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
    { 238, 244, 250 },   /* COLOR_FACE        */
    { 150, 165, 190 },   /* COLOR_SHADOW      */
    { 255, 255, 255 },   /* COLOR_HILITE      */
    { 70, 99, 158 },     /* COLOR_TITLE_BAR   */
    { 155, 198, 231 },   /* COLOR_DESKTOP     */
    { 252, 254, 255 },   /* COLOR_TEXT_FIELD  */
    { 65, 126, 207 },    /* COLOR_SELECTION   */
    { 175, 190, 211 },   /* COLOR_DISABLED    */
    { 255, 132, 118 },   /* COLOR_ACCENT      */
    { 67, 190, 153 },    /* COLOR_GOOD        */
    { 232, 91, 112 },    /* COLOR_ALERT       */
    { 90, 124, 174 },    /* COLOR_PANEL       */
    { 255, 255, 255 },   /* COLOR_CANVAS      */
    { 201, 216, 232 },   /* COLOR_GRID        */
    { 41, 63, 105 },     /* COLOR_DEEP        */
    { 31, 47, 78 }       /* COLOR_CURSOR      */
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

/* Cache both direct-colour representations once per theme change. The hot
 * presenter then performs only table reads and naturally aligned stores. */
static void palette_write_entry(u8 index, Color value) {
    rgb565_color[index] = (u16)(((u16)(value.red >> 3u) << 11u) |
                                ((u16)(value.green >> 2u) << 5u) |
                                (u16)(value.blue >> 3u));
    xrgb8888_color[index] = ((u32)value.red << 16u) |
                            ((u32)value.green << 8u) |
                            (u32)value.blue;
    if (video_backend == VIDEO_BACKEND_VGA) {
        port_write_byte(PORT_VGA_DAC_WRITE_INDEX, index);
        port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.red >> 2));
        port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.green >> 2));
        port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.blue >> 2));
    }
}

/* The 6x6x6 colour cube that lives in slots 32..247. Six evenly spaced
 * levels per channel is the classic web-safe grid: coarse enough to fit
 * in the palette, fine enough that a blended edge is indistinguishable
 * from the exact colour at 320x200. */
static u8 cube_level(u8 step) {
    return (u8)(((u16)step * 255u) / (COLOR_CUBE_STEPS - 1u));
}

static u8 cube_step(u8 value) {
    return (u8)((((u16)value * (COLOR_CUBE_STEPS - 1u)) + 127u) / 255u);
}

static Color cube_color(u16 slot) {
    Color result;

    result.red = cube_level((u8)(slot / (COLOR_CUBE_STEPS * COLOR_CUBE_STEPS)));
    result.green = cube_level((u8)((slot / COLOR_CUBE_STEPS) % COLOR_CUBE_STEPS));
    result.blue = cube_level((u8)(slot % COLOR_CUBE_STEPS));
    return result;
}

/* The RGB behind any palette index, role or cube. */
static Color color_of(u8 index) {
    if (index < PALETTE_SIZE) {
        return palette[index];
    }
    if ((u16)index < COLOR_CUBE_BASE + COLOR_CUBE_COUNT) {
        return cube_color((u16)(index - COLOR_CUBE_BASE));
    }
    return palette[COLOR_BLACK];
}

/* The cube slot closest to a colour. */
static u8 nearest_cube(u16 red, u16 green, u16 blue) {
    return (u8)(COLOR_CUBE_BASE +
                (u16)cube_step((u8)red) * COLOR_CUBE_STEPS * COLOR_CUBE_STEPS +
                (u16)cube_step((u8)green) * COLOR_CUBE_STEPS +
                (u16)cube_step((u8)blue));
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
    /* The blending cube, themed like everything else so a green or amber
     * screen keeps its smooth edges. */
    for (index = 0u; index < COLOR_CUBE_COUNT; ++index) {
        palette_write_entry((u8)(COLOR_CUBE_BASE + index),
                            themed(cube_color(index), current_theme));
    }
    /* The slots we never draw with stay black instead of the rainbow the
     * video BIOS leaves behind after a mode change. */
    for (index = COLOR_CUBE_BASE + COLOR_CUBE_COUNT; index < 256u; ++index) {
        palette_write_entry((u8)index, black);
    }
}

/* ------------------------------------------------------------------ */
/* Blending                                                            */
/* ------------------------------------------------------------------ */

/* Mix a foreground colour into a background one. alpha runs from 0 (all
 * background) to FONT_ALPHA_MAX (all foreground); everything in between
 * lands on the nearest cube colour.
 *
 * Both ends stay exact, so solid areas keep their palette role and only
 * the soft edges spend cube slots. The answer for one pair of colours is
 * remembered, because text and icons ask for the same pair thousands of
 * times per frame and the division is the expensive part. */
static u8 shade_foreground = 0xFFu;
static u8 shade_background = 0xFFu;
static u8 shade_map[FONT_ALPHA_MAX + 1u];
static u8 shade_ready;

u8 gfx_shade(u8 foreground, u8 background, u8 alpha) {
    if (alpha >= (u8)FONT_ALPHA_MAX) {
        return foreground;
    }
    if (alpha == 0u) {
        return background;
    }
    if (foreground == background) {
        return foreground;
    }
    if (shade_ready == 0u || foreground != shade_foreground ||
        background != shade_background) {
        const Color front = color_of(foreground);
        const Color back = color_of(background);
        u8 level;

        for (level = 0u; level <= (u8)FONT_ALPHA_MAX; ++level) {
            const u16 red = (u16)(((u32)front.red * level +
                                   (u32)back.red * (FONT_ALPHA_MAX - level)) /
                                  FONT_ALPHA_MAX);
            const u16 green = (u16)(((u32)front.green * level +
                                     (u32)back.green * (FONT_ALPHA_MAX - level)) /
                                    FONT_ALPHA_MAX);
            const u16 blue = (u16)(((u32)front.blue * level +
                                    (u32)back.blue * (FONT_ALPHA_MAX - level)) /
                                   FONT_ALPHA_MAX);

            shade_map[level] = nearest_cube(red, green, blue);
        }
        shade_map[0] = background;
        shade_map[FONT_ALPHA_MAX] = foreground;
        shade_foreground = foreground;
        shade_background = background;
        shade_ready = 1u;
    }
    return shade_map[alpha];
}

void gfx_select_theme(u8 theme) {
    if (theme >= THEME_COUNT) {
        theme = THEME_COLOR;
    }
    current_theme = theme;
    shade_ready = 0u;
    palette_apply();
    /* Indexed canvas bytes do not change when a VBE theme changes, so force
     * one complete colour conversion instead of trusting the dirty shadow. */
    first_present = 1u;
}

u8 gfx_current_theme(void) {
    return current_theme;
}

/* ------------------------------------------------------------------ */
/* Font                                                                */
/* ------------------------------------------------------------------ */

static const u8 *font_glyphs;       /* one bit per pixel (BIOS layout)  */
static const u8 *font_levels;       /* 64 coverage levels per glyph     */
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
        font_levels = font_ttf_alpha_table();
        font_ready = 1u;
        return;
    }
    /* Otherwise: the 8x8 font of the video BIOS. */
    font_glyphs = (const u8 *)(u32)bios_font_address;
    font_ready = font_looks_valid(font_glyphs);
}

static u8 high_text_active(void) {
    return (u8)(video_backend == VIDEO_BACKEND_VBE &&
                video_bits_per_pixel == 32u && render_width >= 480u &&
                render_height >= 300u);
}

static u8 high_text_scale(void) {
    if (render_width >= 1280u) {
        return 3u;
    }
    if (render_width >= 640u) {
        return 2u;
    }
    return 1u;
}

static u8 high_text_alpha(char character, u8 row, u8 column) {
    if (font_ready == 0u) {
        return (u8)((row == 0u || row == 7u || column == 0u || column == 7u)
                        ? FONT_ALPHA_MAX : 0u);
    }
    if (character < 32 || character > 126) {
        character = '?';
    }
    if (font_levels != (const u8 *)0) {
        return font_levels[(u16)((u8)character * 64u + row * 8u + column)];
    }
    return (u8)((font_glyphs[(u16)(u8)character * 8u + row] &
                 (u8)(0x80u >> column)) != 0u ? FONT_ALPHA_MAX : 0u);
}

static u32 high_text_blend(u32 background, u8 color, u8 alpha) {
    const Color ink = color_of(color);
    const u8 back_red = (u8)(background >> 16u);
    const u8 back_green = (u8)(background >> 8u);
    const u8 back_blue = (u8)background;
    const u16 red = (u16)(((u32)ink.red * alpha +
                           (u32)back_red * (FONT_ALPHA_MAX - alpha)) /
                          FONT_ALPHA_MAX);
    const u16 green = (u16)(((u32)ink.green * alpha +
                             (u32)back_green * (FONT_ALPHA_MAX - alpha)) /
                            FONT_ALPHA_MAX);
    const u16 blue = (u16)(((u32)ink.blue * alpha +
                            (u32)back_blue * (FONT_ALPHA_MAX - alpha)) /
                           FONT_ALPHA_MAX);

    return ((u32)red << 16u) | ((u32)green << 8u) | (u32)blue;
}

static void high_text_restore_pixel(volatile u32 *framebuffer, u16 x, u16 y) {
    const u16 logical_x = (u16)(((u32)(x - render_left) * SCREEN_WIDTH) /
                               render_width);
    const u16 logical_y = (u16)(((u32)(y - render_top) * SCREEN_HEIGHT) /
                                render_height);
    const u8 color = screen[(u32)logical_y * SCREEN_WIDTH + logical_x];

    framebuffer[(u32)y * (video_pitch / 4u) + x] = xrgb8888_color[color];
}

static void high_text_restore(const HighText *commands, u16 count) {
    volatile u32 *framebuffer;
    u16 command;

    if (high_text_active() == 0u || video_framebuffer_address == 0u) {
        return;
    }
    framebuffer = (volatile u32 *)(u32)video_framebuffer_address;
    for (command = 0u; command < count; ++command) {
        const HighText *item = &commands[command];
        const u8 scale = high_text_scale();
        s32 base_x = (s32)render_left +
                     (s32)item->x * render_width / SCREEN_WIDTH;
        s32 base_y = (s32)render_top +
                     (s32)item->y * render_height / SCREEN_HEIGHT;
        u16 character;

        if (base_x < 0 || base_y < 0) {
            continue;
        }
        for (character = 0u; item->text[character] != '\0'; ++character) {
            u16 row;
            u16 column;

            for (row = 0u; row < 8u * scale; ++row) {
                const u16 y = (u16)(base_y + row);
                if (y >= video_height) {
                    continue;
                }
                for (column = 0u; column < 8u * scale; ++column) {
                    const u16 x = (u16)(base_x + character * 8u * scale + column);
                    if (x < video_width) {
                        high_text_restore_pixel(framebuffer, x, y);
                    }
                }
            }
        }
    }
}

static void high_text_queue(s16 x, s16 y, const char *text, u8 color) {
    HighText *item;
    u16 index;

    if (high_text_current_count >= HIGH_TEXT_LIMIT) {
        return;
    }
    item = &high_text_current[high_text_current_count++];
    item->x = x;
    item->y = y;
    item->color = color;
    for (index = 0u; index + 1u < HIGH_TEXT_LENGTH && text[index] != '\0'; ++index) {
        item->text[index] = text[index];
    }
    item->text[index] = '\0';
}

static void high_text_draw(void) {
    volatile u32 *framebuffer;
    u16 command;

    if (high_text_active() == 0u || video_framebuffer_address == 0u) {
        return;
    }
    framebuffer = (volatile u32 *)(u32)video_framebuffer_address;
    for (command = 0u; command < high_text_current_count; ++command) {
        const HighText *item = &high_text_current[command];
        const u8 scale = high_text_scale();
        const s32 base_x = (s32)render_left +
                           (s32)item->x * render_width / SCREEN_WIDTH;
        const s32 base_y = (s32)render_top +
                           (s32)item->y * render_height / SCREEN_HEIGHT;
        u16 character;

        if (base_x < 0 || base_y < 0) {
            continue;
        }
        for (character = 0u; item->text[character] != '\0'; ++character) {
            u8 row;
            u8 column;

            for (row = 0u; row < 8u; ++row) {
                for (column = 0u; column < 8u; ++column) {
                    const u8 alpha = high_text_alpha(item->text[character], row, column);
                    u8 dy;

                    if (alpha == 0u) {
                        continue;
                    }
                    for (dy = 0u; dy < scale; ++dy) {
                        const u16 y = (u16)(base_y + row * scale + dy);
                        u8 dx;

                        if (y >= video_height) {
                            continue;
                        }
                        for (dx = 0u; dx < scale; ++dx) {
                            const u16 x = (u16)(base_x +
                                character * 8u * scale +
                                column * scale + dx);
                            volatile u32 *pixel;

                            if (x >= video_width) {
                                continue;
                            }
                            pixel = framebuffer + (u32)y * (video_pitch / 4u) + x;
                            *pixel = high_text_blend(*pixel, item->color, alpha);
                        }
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Frame buffer                                                        */
/* ------------------------------------------------------------------ */

static void present_vga(void) {
    const u32 *source = (const u32 *)BACK_BUFFER_ADDRESS;
    u32 *shadow = (u32 *)PRESENT_BUFFER_ADDRESS;
    volatile u32 *target = (volatile u32 *)0x000A0000u;
    u32 index;

    for (index = 0u; index < SCREEN_BYTES / 4u; ++index) {
        const u32 pixels = source[index];

        if (first_present != 0u || shadow[index] != pixels) {
            target[index] = pixels;
            shadow[index] = pixels;
        }
    }
}

/* Logical-pixel boundaries in the physical mode. They are calculated once,
 * not divided in the hot path. The startup wizard can make this a small
 * centred viewport, so an emulator need not repaint a full 1920x1080 surface
 * on every changed logical pixel. */
static u16 vbe_x[SCREEN_WIDTH + 1u];
static u16 vbe_y[SCREEN_HEIGHT + 1u];
static u8 vbe_map_ready;

static void prepare_vbe_map(void) {
    u16 position;

    if (render_width == 0u || render_height == 0u) {
        render_width = video_width;
        render_height = video_height;
        render_left = 0u;
        render_top = 0u;
    }
    for (position = 0u; position <= SCREEN_WIDTH; ++position) {
        vbe_x[position] = (u16)(render_left +
            ((u32)position * render_width) / SCREEN_WIDTH);
    }
    for (position = 0u; position <= SCREEN_HEIGHT; ++position) {
        vbe_y[position] = (u16)(render_top +
            ((u32)position * render_height) / SCREEN_HEIGHT);
    }
    vbe_map_ready = 1u;
}

void gfx_set_output_resolution(u16 width, u16 height) {
    u32 color;

    if (video_backend != VIDEO_BACKEND_VBE || video_width == 0u ||
        video_height == 0u) {
        return;
    }
    if (width < SCREEN_WIDTH) {
        width = SCREEN_WIDTH;
    }
    if (height < SCREEN_HEIGHT) {
        height = SCREEN_HEIGHT;
    }
    if (width > video_width) {
        width = video_width;
    }
    if (height > video_height) {
        height = video_height;
    }
    render_width = width;
    render_height = height;
    render_left = (u16)((video_width - width) / 2u);
    render_top = (u16)((video_height - height) / 2u);
    vbe_map_ready = 0u;
    first_present = 1u;

    /* Clear the letterbox once. Subsequent frames only touch the selected
     * viewport, which is the useful optimization for v86's small profile. */
    color = xrgb8888_color[COLOR_DESKTOP];
    if (video_bits_per_pixel == 32u) {
        volatile u32 *framebuffer = (volatile u32 *)(u32)video_framebuffer_address;
        u16 y;

        for (y = 0u; y < video_height; ++y) {
            u32 x;
            volatile u32 *row = framebuffer + (u32)y * (video_pitch / 4u);

            for (x = 0u; x < video_width; ++x) {
                row[x] = color;
            }
        }
    }
}

static inline void present_vbe_pixel(volatile u8 *framebuffer, u16 x, u16 y,
                                     u8 color) {
    const u16 left = vbe_x[x];
    const u16 right = vbe_x[x + 1u];
    const u16 top = vbe_y[y];
    const u16 bottom = vbe_y[y + 1u];
    u16 output_y;

    if (video_bits_per_pixel == 32u) {
        const u32 direct_color = xrgb8888_color[color];

        for (output_y = top; output_y < bottom; ++output_y) {
            volatile u32 *pixel = (volatile u32 *)(framebuffer +
                (u32)output_y * video_pitch) + left;
            u16 output_x;

            for (output_x = left; output_x < right; ++output_x) {
                *pixel++ = direct_color;
            }
        }
    } else {
        const u16 direct_color = rgb565_color[color];

        for (output_y = top; output_y < bottom; ++output_y) {
            volatile u16 *pixel = (volatile u16 *)(framebuffer +
                (u32)output_y * video_pitch) + left;
            u16 output_x;

            for (output_x = left; output_x < right; ++output_x) {
                *pixel++ = direct_color;
            }
        }
    }
}

static void present_vbe(void) {
    volatile u8 *framebuffer =
        (volatile u8 *)(u32)video_framebuffer_address;
    const u32 *source_words = (const u32 *)BACK_BUFFER_ADDRESS;
    u32 *shadow_words = (u32 *)PRESENT_BUFFER_ADDRESS;
    u16 y;

    if (vbe_map_ready == 0u) {
        prepare_vbe_map();
    }
    for (y = 0u; y < SCREEN_HEIGHT; ++y) {
        const u32 word_base = (u32)y * (SCREEN_WIDTH / 4u);
        const u8 *source = screen + (u32)y * SCREEN_WIDTH;
        u8 *shadow = presented + (u32)y * SCREEN_WIDTH;
        u16 group;

        for (group = 0u; group < SCREEN_WIDTH / 4u; ++group) {
            const u32 word = source_words[word_base + group];
            u16 within;

            if (first_present == 0u && shadow_words[word_base + group] == word) {
                continue;
            }
            for (within = 0u; within < 4u; ++within) {
                const u16 x = (u16)(group * 4u + within);
                const u8 color = source[x];

                if (first_present != 0u || shadow[x] != color) {
                    present_vbe_pixel(framebuffer, x, y, color);
                    shadow[x] = color;
                }
            }
            shadow_words[word_base + group] = word;
        }
    }
}

void gfx_show(void) {
    if (video_backend == VIDEO_BACKEND_VBE &&
        video_framebuffer_address != 0u && video_width >= SCREEN_WIDTH &&
        video_height >= SCREEN_HEIGHT &&
        (video_bits_per_pixel == 16u || video_bits_per_pixel == 32u)) {
        present_vbe();
        high_text_draw();
        if (high_text_current_count != 0u) {
            u16 index;

            high_text_previous_count = high_text_current_count;
            for (index = 0u; index < high_text_current_count; ++index) {
                high_text_previous[index] = high_text_current[index];
            }
        }
    } else {
        present_vga();
    }
    first_present = 0u;
}

static void fill_bytes(u8 *target, u32 length, u8 color) {
    const u32 packed = (u32)color * 0x01010101u;

    while (length != 0u && ((u32)target & 3u) != 0u) {
        *target++ = color;
        --length;
    }
    while (length >= 4u) {
        *(u32 *)target = packed;
        target += 4;
        length -= 4u;
    }
    while (length != 0u) {
        *target++ = color;
        --length;
    }
}

void gfx_clear(u8 color) {
    /* Remove the previous high-resolution text before the logical canvas is
     * replaced with this frame's background. The presenter then only needs
     * to touch the small text rectangles, not all 2M framebuffer pixels. */
    high_text_restore(high_text_previous, high_text_previous_count);
    high_text_previous_count = 0u;
    high_text_current_count = 0u;
    fill_bytes(screen, SCREEN_BYTES, color);
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

/* Lay a colour over what is already on screen. alpha 0 changes nothing,
 * FONT_ALPHA_MAX paints the colour solid, and the levels in between pick
 * the cube colour that sits that far along. Every smooth edge in DimOS --
 * letters, icons, the pointer, circles -- is drawn with this. */
void gfx_pixel_blend(s16 x, s16 y, u8 color, u8 alpha) {
    u32 offset;

    if (alpha == 0u) {
        return;
    }
    if (x < 0 || y < 0) {
        return;
    }
    if ((u16)x >= SCREEN_WIDTH || (u16)y >= SCREEN_HEIGHT) {
        return;
    }
    if (alpha > (u8)FONT_ALPHA_MAX) {
        alpha = (u8)FONT_ALPHA_MAX;
    }
    offset = (u32)(u16)y * SCREEN_WIDTH + (u32)(u16)x;
    if (alpha == (u8)FONT_ALPHA_MAX) {
        screen[offset] = color;
        return;
    }
    screen[offset] = gfx_shade(color, screen[offset], alpha);
}

void gfx_horizontal_line(s16 x, s16 y, s16 length, u8 color) {
    s16 right;

    if (length <= 0 || y < 0 || (u16)y >= SCREEN_HEIGHT) {
        return;
    }
    right = (s16)(x + length);
    if (right <= 0 || x >= (s16)SCREEN_WIDTH) {
        return;
    }
    if (x < 0) {
        x = 0;
    }
    if (right > (s16)SCREEN_WIDTH) {
        right = (s16)SCREEN_WIDTH;
    }
    fill_bytes(screen + (u32)(u16)y * SCREEN_WIDTH + (u16)x,
               (u32)(u16)(right - x), color);
}

void gfx_vertical_line(s16 x, s16 y, s16 length, u8 color) {
    s16 bottom;
    u8 *pixel;

    if (length <= 0 || x < 0 || (u16)x >= SCREEN_WIDTH) {
        return;
    }
    bottom = (s16)(y + length);
    if (bottom <= 0 || y >= (s16)SCREEN_HEIGHT) {
        return;
    }
    if (y < 0) {
        y = 0;
    }
    if (bottom > (s16)SCREEN_HEIGHT) {
        bottom = (s16)SCREEN_HEIGHT;
    }
    pixel = screen + (u32)(u16)y * SCREEN_WIDTH + (u16)x;
    while (y < bottom) {
        *pixel = color;
        pixel += SCREEN_WIDTH;
        ++y;
    }
}

void gfx_fill(s16 x, s16 y, s16 width, s16 height, u8 color) {
    s16 right;
    s16 bottom;
    s16 row;

    if (width <= 0 || height <= 0) {
        return;
    }
    right = (s16)(x + width);
    bottom = (s16)(y + height);
    if (right <= 0 || bottom <= 0 || x >= (s16)SCREEN_WIDTH ||
        y >= (s16)SCREEN_HEIGHT) {
        return;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (right > (s16)SCREEN_WIDTH) {
        right = (s16)SCREEN_WIDTH;
    }
    if (bottom > (s16)SCREEN_HEIGHT) {
        bottom = (s16)SCREEN_HEIGHT;
    }
    for (row = y; row < bottom; ++row) {
        fill_bytes(screen + (u32)(u16)row * SCREEN_WIDTH + (u16)x,
                   (u32)(u16)(right - x), color);
    }
}

void gfx_outline(s16 x, s16 y, s16 width, s16 height, u8 color) {
    gfx_horizontal_line(x, y, width, color);
    gfx_horizontal_line(x, (s16)(y + height - 1), width, color);
    gfx_vertical_line(x, y, height, color);
    gfx_vertical_line((s16)(x + width - 1), y, height, color);
}

/* Bresenham's line for all octants: the doubled error of a step must be
 * evaluated ONCE, before either axis moves. Reading it again after the
 * first correction turns the walk into a drunkard's search for the end
 * point -- a diagonal in Paint once took millions of pixels to arrive. */
void gfx_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 color) {
    const s16 dx = (s16)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    const s16 dy = (s16)((y1 > y0) ? (y1 - y0) : (y0 - y1));
    const s16 step_x = (x0 < x1) ? 1 : -1;
    const s16 step_y = (y0 < y1) ? 1 : -1;
    s16 error = (s16)(dx - dy);

    for (;;) {
        const s16 twice_error = (s16)(2 * error);

        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        if (twice_error > (s16)-dy) {
            error = (s16)(error - dy);
            x0 = (s16)(x0 + step_x);
        }
        if (twice_error < dx) {
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

/* The integer square root of a small value: the classic bit walking
 * method, plenty fast for radii under a hundred. */
static u8 integer_sqrt(u16 value) {
    u8 root = 0u;
    u8 add = 128u;

    while ((u16)((u16)add * (u16)add) > value && add > 1u) {
        add = (u8)(add >> 1);
    }
    while (add != 0u) {
        const u8 trial = (u8)(root + add);

        if ((u16)((u16)trial * (u16)trial) <= value) {
            root = trial;
        }
        add = (u8)(add >> 1);
    }
    return root;
}

/* A filled ellipse: for every scanline the half width drops the way a
 * circle's would, just stretched differently in x and y. The wallpaper
 * builds its big mouse mascot out of these -- circles alone leave the
 * poor thing looking square. */
void gfx_ellipse_fill(s16 center_x, s16 center_y, s16 radius_x, s16 radius_y,
                      u8 color) {
    s16 row;

    for (row = (s16)-radius_y; row <= radius_y; ++row) {
        const u16 rest = (u16)((u16)radius_y * (u16)radius_y -
                               (u16)(row * row));
        const s16 half = (s16)(((u16)radius_x * integer_sqrt(rest)) /
                               (u16)radius_y);

        gfx_horizontal_line((s16)(center_x - half), (s16)(center_y + row),
                            (s16)(2 * half + 1), color);
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

/* A vertical gradient between two colours. There is no true colour in
 * mode 13h, so every band is the colour cube slot that sits that far
 * between the two ends -- the same trick anti-aliasing uses for soft
 * edges, stretched over a whole bar. The Xfce wallpaper, the window
 * title bars and the Whisker menu header are all painted with this. */
void gfx_gradient_vertical(s16 x, s16 y, s16 width, s16 height,
                           u8 top_color, u8 bottom_color) {
    s16 row;

    for (row = 0; row < height; ++row) {
        /* Band 0 is exactly the top colour, the last band is one step
         * short of the bottom one: a full bottom row belongs to whatever
         * is drawn next, not to this gradient. */
        u8 alpha = (u8)(((u32)row * (FONT_ALPHA_MAX - 1u)) /
                        (u32)((height > 1) ? (height - 1) : 1));
        u8 color = gfx_shade(bottom_color, top_color, alpha);

        gfx_horizontal_line(x, (s16)(y + row), width, color);
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

void gfx_character(s16 x, s16 y, char character, u8 color) {
    u8 row;
    u8 column;

    if (high_text_active() != 0u) {
        char one[2];

        one[0] = character;
        one[1] = '\0';
        high_text_queue(x, y, one, color);
        return;
    }

    if (character < 32 || character > 126) {
        character = '?';
    }
    if (font_ready == 0u) {
        /* No usable font at all: show a hollow box so text is still
         * visible instead of the screen going silently blank. */
        gfx_outline(x, y, 7, 8, color);
        return;
    }

    if (font_levels != (const u8 *)0) {
        /* The TrueType glyph, drawn with its coverage levels: a pixel the
         * outline fills completely gets the text colour, a pixel it only
         * clips gets a mixture with whatever lies underneath. That is what
         * removes the staircase from the letters. */
        const u8 *levels = font_levels + (u16)((u8)character * 64u);

        for (row = 0u; row < 8u; ++row) {
            for (column = 0u; column < 8u; ++column) {
                const u8 alpha = levels[(u16)row * 8u + column];

                if (alpha != 0u) {
                    gfx_pixel_blend((s16)(x + (s16)column), (s16)(y + (s16)row),
                                    color, alpha);
                }
            }
        }
        return;
    }

    {
        /* Fallback: the one-bit BIOS font. */
        const u8 *glyph = font_glyphs + (u16)((u8)character * 8u);

        for (row = 0u; row < 8u; ++row) {
            const u8 bits = glyph[row];
            for (column = 0u; column < 8u; ++column) {
                if ((u8)(bits & (u8)(0x80u >> column)) != 0u) {
                    gfx_pixel((s16)(x + (s16)column), (s16)(y + (s16)row), color);
                }
            }
        }
    }
}

void gfx_text(s16 x, s16 y, const char *text, u8 color) {
    if (high_text_active() != 0u) {
        high_text_queue(x, y, text, color);
        return;
    }
    while (*text != '\0') {
        gfx_character(x, y, *text, color);
        x = (s16)(x + 8);
        ++text;
    }
}

void gfx_text_filled(s16 x, s16 y, const char *text, u8 foreground, u8 background) {
    if (high_text_active() != 0u) {
        const s16 start = x;
        const char *start_text = text;

        while (*text != '\0') {
            gfx_fill(x, y, 8, 8, background);
            x = (s16)(x + 8);
            ++text;
        }
        high_text_queue(start, y, start_text, foreground);
        return;
    }
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

/* Art rows use '.' for "leave the pixel alone" and anything else for
 * "draw". Icons are written that way because a picture in the source is
 * far easier to read than a table of bytes.
 *
 * The art is a hard on/off mask, and drawn straight it looks exactly as
 * blocky as it is written. So every pixel the art lights up is drawn
 * solid, and the empty pixels around it are given a soft fringe: how
 * much of it they get depends on how many of their neighbours carry
 * ink. That keeps the icon itself crisp -- a two pixel wide stroke stays
 * a two pixel wide stroke -- while the staircase along a diagonal or a
 * curve is filled in with intermediate shades, which is what stops the
 * icons looking like blocks. Neighbours count 3 each side by side and 1
 * across a corner, out of the sixteen levels gfx_pixel_blend() takes.
 */

#define ART_MAX_WIDTH 64u

/* What one row of art says about a column: 0 = nothing, 1 = the main
 * colour, 2 = the secondary ("o") colour. */
static u8 art_cell(const char *const *art, u16 rows, s16 row, s16 column) {
    const char *line;
    s16 index;

    if (row < 0 || (u16)row >= rows || column < 0) {
        return 0u;
    }
    line = art[row];
    for (index = 0; index < column; ++index) {
        if (line[index] == '\0') {
            return 0u;
        }
    }
    if (line[column] == '\0' || line[column] == '.') {
        return 0u;
    }
    return (line[column] == 'o') ? 2u : 1u;
}

static u16 art_width(const char *const *art, u16 rows) {
    u16 row;
    u16 widest = 0u;

    for (row = 0u; row < rows; ++row) {
        u16 length = 0u;

        while (art[row][length] != '\0' && length < ART_MAX_WIDTH) {
            ++length;
        }
        if (length > widest) {
            widest = length;
        }
    }
    return widest;
}

/* One smoothed sample of the art: how much ink covers this pixel, and
 * which of the two colours that ink mostly is. */
static void art_sample(const char *const *art, u16 rows, s16 row, s16 column,
                       u8 *alpha, u8 *secondary) {
    static const s8 offset_row[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    static const s8 offset_column[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    static const u8 weight[8] = { 3u, 3u, 3u, 3u, 1u, 1u, 1u, 1u };
    const u8 centre = art_cell(art, rows, row, column);
    u16 main_ink = 0u;
    u16 other_ink = 0u;
    u8 index;

    if (centre != 0u) {
        /* A pixel the art fills is drawn solid, so shapes keep their
         * weight and small icons stay readable. */
        *alpha = (u8)FONT_ALPHA_MAX;
        *secondary = (u8)((centre == 2u) ? 1u : 0u);
        return;
    }

    for (index = 0u; index < 8u; ++index) {
        const u8 cell = art_cell(art, rows, (s16)(row + offset_row[index]),
                                 (s16)(column + offset_column[index]));

        if (cell == 1u) {
            main_ink = (u16)(main_ink + weight[index]);
        } else if (cell == 2u) {
            other_ink = (u16)(other_ink + weight[index]);
        }
    }
    /* An empty pixel only ever gets a fringe: enough to round off a
     * corner, never enough to smear the picture. */
    if (main_ink + other_ink > 8u) {
        const u16 total = (u16)(main_ink + other_ink);

        main_ink = (u16)((main_ink * 8u) / total);
        other_ink = (u16)((other_ink * 8u) / total);
    }
    *alpha = (u8)(main_ink + other_ink);
    *secondary = (u8)((other_ink > main_ink) ? 1u : 0u);
}

void gfx_picture(s16 x, s16 y, const char *const *art, u16 rows, u8 color,
                 u8 background) {
    const u16 width = art_width(art, rows);
    s16 row;
    s16 column;

    /* One pixel of bleed on every side, because a smoothed edge reaches
     * just past the pixels the art itself lights up. */
    for (row = -1; row <= (s16)rows; ++row) {
        for (column = -1; column <= (s16)width; ++column) {
            u8 alpha;
            u8 secondary;

            art_sample(art, rows, row, column, &alpha, &secondary);
            if (alpha == 0u) {
                continue;
            }
            gfx_pixel_blend((s16)(x + column), (s16)(y + row),
                            (secondary != 0u) ? background : color, alpha);
        }
    }
}

void gfx_picture_opaque(s16 x, s16 y, const char *const *art, u16 rows, u8 on,
                        u8 off) {
    const u16 width = art_width(art, rows);
    s16 row;
    s16 column;

    for (row = 0; row < (s16)rows; ++row) {
        for (column = 0; column < (s16)width; ++column) {
            u8 alpha;
            u8 secondary;

            /* The background is painted first, then the smoothed shape is
             * laid over it, so this stays opaque and still has soft edges. */
            gfx_pixel((s16)(x + column), (s16)(y + row), off);
            art_sample(art, rows, row, column, &alpha, &secondary);
            (void)secondary;
            gfx_pixel_blend((s16)(x + column), (s16)(y + row), on, alpha);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The mouse pointer                                                   */
/* ------------------------------------------------------------------ */

/* The pointer is not a picture at all: it is a shape, filled with real
 * area coverage. The arrow is described once as a polygon in sixteenths
 * of a pixel, rasterized once at boot into a coverage map, and only that
 * map is blended onto the screen every frame -- so it is smooth without
 * costing anything per frame.
 *
 * Two maps are built: the body of the arrow and a border one pixel wider
 * that sits behind it, which is what keeps the pointer visible on a dark
 * desktop as well as on a white page. */

#define POINTER_WIDTH 12u
#define POINTER_HEIGHT 19u
#define POINTER_SUBPIXEL 16 /* the polygon is written in 1/16 pixel */
#define POINTER_SAMPLES 4   /* 4x4 coverage samples per pixel       */
#define POINTER_POINTS 7

/* Tip at the top left, then down the left edge, into the notch, out
 * along the tail and back up the right edge. Coordinates are in
 * sixteenths of a pixel, so the arrow is about 9.4 x 16.6 pixels. */
static const s16 pointer_x[POINTER_POINTS] = { 0, 0, 48, 80, 115, 82, 150 };
static const s16 pointer_y[POINTER_POINTS] = { 0, 230, 178, 266, 250, 165, 162 };

static u8 pointer_body[POINTER_WIDTH * POINTER_HEIGHT];
static u8 pointer_border[POINTER_WIDTH * POINTER_HEIGHT];

/* Is this sample point inside the polygon? Even-odd crossing test, all
 * in sixteenths of a pixel so nothing needs floating point. */
static u8 pointer_inside(s32 sample_x, s32 sample_y) {
    u8 index;
    u8 inside = 0u;

    for (index = 0u; index < POINTER_POINTS; ++index) {
        const u8 next = (u8)((index + 1u) % POINTER_POINTS);
        const s32 x0 = pointer_x[index];
        const s32 y0 = pointer_y[index];
        const s32 x1 = pointer_x[next];
        const s32 y1 = pointer_y[next];

        if ((y0 > sample_y) != (y1 > sample_y)) {
            const s32 crossing = x0 + ((x1 - x0) * (sample_y - y0)) / (y1 - y0);

            if (sample_x < crossing) {
                inside = (u8)(inside ^ 1u);
            }
        }
    }
    return inside;
}

/* Coverage of the arrow over every pixel of the map: sixteen samples per
 * pixel, which is the same 0..16 scale the blending takes. */
static void pointer_build_body(void) {
    u16 row;
    u16 column;

    for (row = 0u; row < POINTER_HEIGHT; ++row) {
        for (column = 0u; column < POINTER_WIDTH; ++column) {
            u8 covered = 0u;
            u8 sub_row;

            for (sub_row = 0u; sub_row < POINTER_SAMPLES; ++sub_row) {
                const s32 sample_y =
                    (s32)row * POINTER_SUBPIXEL +
                    (s32)(2 * sub_row + 1) * POINTER_SUBPIXEL / (2 * POINTER_SAMPLES);
                u8 sub_column;

                for (sub_column = 0u; sub_column < POINTER_SAMPLES; ++sub_column) {
                    const s32 sample_x =
                        (s32)column * POINTER_SUBPIXEL +
                        (s32)(2 * sub_column + 1) * POINTER_SUBPIXEL /
                            (2 * POINTER_SAMPLES);

                    covered = (u8)(covered + pointer_inside(sample_x, sample_y));
                }
            }
            pointer_body[row * POINTER_WIDTH + column] = covered; /* 0..16 */
        }
    }
}

static void pointer_init(void) {
    s16 row;
    s16 column;

    pointer_build_body();

    /* The border is the arrow grown by one pixel in every direction: the
     * strongest coverage found in the nine pixels around this one. Drawn
     * first, in white, it keeps a black pointer readable over a dark
     * desktop and over a white text field alike. */
    for (row = 0; row < (s16)POINTER_HEIGHT; ++row) {
        for (column = 0; column < (s16)POINTER_WIDTH; ++column) {
            u8 strongest = 0u;
            s16 near_row;

            for (near_row = (s16)(row - 1); near_row <= (s16)(row + 1); ++near_row) {
                s16 near_column;

                if (near_row < 0 || near_row >= (s16)POINTER_HEIGHT) {
                    continue;
                }
                for (near_column = (s16)(column - 1);
                     near_column <= (s16)(column + 1); ++near_column) {
                    u8 value;

                    if (near_column < 0 || near_column >= (s16)POINTER_WIDTH) {
                        continue;
                    }
                    value = pointer_body[(u16)near_row * POINTER_WIDTH +
                                         (u16)near_column];
                    if (value > strongest) {
                        strongest = value;
                    }
                }
            }
            pointer_border[(u16)row * POINTER_WIDTH + (u16)column] = strongest;
        }
    }
}

void gfx_draw_pointer(s16 x, s16 y) {
    u16 row;
    u16 column;

    for (row = 0u; row < POINTER_HEIGHT; ++row) {
        for (column = 0u; column < POINTER_WIDTH; ++column) {
            const u16 cell = (u16)(row * POINTER_WIDTH + column);

            gfx_pixel_blend((s16)(x + (s16)column), (s16)(y + (s16)row),
                            COLOR_HILITE, pointer_border[cell]);
        }
    }
    for (row = 0u; row < POINTER_HEIGHT; ++row) {
        for (column = 0u; column < POINTER_WIDTH; ++column) {
            const u16 cell = (u16)(row * POINTER_WIDTH + column);

            gfx_pixel_blend((s16)(x + (s16)column), (s16)(y + (s16)row),
                            COLOR_CURSOR, pointer_body[cell]);
        }
    }
}

void gfx_init(void) {
    font_init();
    pointer_init();
    palette_apply();
    render_width = video_width;
    render_height = video_height;
    render_left = 0u;
    render_top = 0u;
    gfx_clear(COLOR_DESKTOP);
}
