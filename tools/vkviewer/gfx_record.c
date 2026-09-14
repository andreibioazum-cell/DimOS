/*
 * gfx_record.c -- gfx.c with the rasterizer taken out.
 *
 * This file provides exactly the interface src/kernel/dimos.h declares
 * for gfx.c, and is linked *instead of* src/kernel/gfx.c when the kernel
 * is built for the Vulkan path. Every entry point that used to compute
 * pixel values here now appends a command to a list and returns.
 *
 * What that removes, concretely:
 *
 *   - gfx_shade(): the palette cube search and the 17 entry blend table
 *     are gone. Mixing a colour with what lies underneath is a GPU
 *     operation now, so nothing on the processor ever computes a blend.
 *   - gfx_character(): no longer walks 64 coverage levels per glyph.
 *     The glyph number is recorded; the font atlas lives in GPU memory.
 *   - gfx_picture(): no longer samples eight neighbours per pixel to
 *     smooth an icon. The icon's identity is recorded; the smoothing is
 *     a texture fetch on the GPU.
 *   - gfx_draw_pointer(): no longer blends two 12x19 coverage maps.
 *     The arrow is a polygon the fragment shader evaluates.
 *   - gfx_fill() / lines / circles: no per-pixel loops at all.
 *
 * Two things still need real pixels, and both are honest about it:
 *
 *   - Paint keeps its own bitmap in scratch memory and copies it into
 *     the frame buffer by hand (memory_copy, not a gfx call). Those
 *     pixels are picked up by a COMMAND_BLIT over the direct-write
 *     buffer. The kernel wrote them, so they are uploaded as an image.
 *   - The palette still has to reach the host, because the DAC is what
 *     carries the themes. It is written through the same ports as always
 *     and read back by the capture script.
 *
 * The font atlas is not rasterized here either: font_ttf.c already
 * produces coverage levels at boot and this file simply hands the table
 * to the host, which uploads it as a texture.
 */

#include "dimos.h"
#include "dimos_commands.h"

extern u32 bios_font_address;

/* The direct-write buffer. Applications that build their own bitmap
 * (Paint) still write here through the `screen` pointer, exactly as they
 * did before; nothing else does. */
u8 *const screen = (u8 *)BACK_BUFFER_ADDRESS;

#define COMMANDS ((DimosCommand *)(COMMAND_AREA_ADDRESS + sizeof(DimosCommandHeader)))
#define HEADER ((volatile DimosCommandHeader *)COMMAND_AREA_ADDRESS)

static u8 current_theme = THEME_COLOR;
static u8 font_is_truetype;

/* ------------------------------------------------------------------ */
/* The palette                                                         */
/* ------------------------------------------------------------------ */

/* Unchanged from gfx.c: these are the colours the VGA DAC is programmed
 * with. The host reads the DAC back and uploads it as a uniform buffer,
 * so the shader resolves indices exactly like the card would. */
typedef struct {
    u8 red;
    u8 green;
    u8 blue;
} Color;

static const Color palette[PALETTE_SIZE] = {
    { 0, 0, 0 },         { 0, 0, 170 },       { 0, 170, 0 },     { 0, 170, 170 },
    { 170, 0, 0 },       { 170, 0, 170 },     { 170, 85, 0 },    { 170, 170, 170 },
    { 85, 85, 85 },      { 85, 85, 255 },     { 85, 255, 85 },   { 85, 255, 255 },
    { 255, 85, 85 },     { 255, 85, 255 },    { 255, 255, 85 },  { 255, 255, 255 },
    { 192, 192, 192 },   { 128, 128, 128 },   { 255, 255, 255 }, { 0, 0, 128 },
    { 0, 128, 128 },     { 255, 255, 255 },   { 96, 99, 104 },   { 128, 128, 128 },
    { 255, 255, 0 },     { 0, 170, 0 },       { 255, 85, 0 },    { 160, 160, 160 },
    { 255, 255, 255 },   { 96, 96, 96 },      { 32, 32, 64 },    { 0, 0, 0 }
};

static Color themed(Color original, u8 theme) {
    u16 brightness;

    if (theme == THEME_COLOR) {
        return original;
    }
    brightness = (u16)(((u32)original.red * 30u + (u32)original.green * 59u +
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

static void palette_write_entry(u8 index, Color value) {
    port_write_byte(PORT_VGA_DAC_WRITE_INDEX, index);
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.red >> 2));
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.green >> 2));
    port_write_byte(PORT_VGA_DAC_DATA, (u8)(value.blue >> 2));
}

static u8 cube_level(u8 step) {
    return (u8)(((u16)step * 255u) / (COLOR_CUBE_STEPS - 1u));
}

static Color cube_color(u16 slot) {
    Color result;

    result.red = cube_level((u8)(slot / (COLOR_CUBE_STEPS * COLOR_CUBE_STEPS)));
    result.green = cube_level((u8)((slot / COLOR_CUBE_STEPS) % COLOR_CUBE_STEPS));
    result.blue = cube_level((u8)(slot % COLOR_CUBE_STEPS));
    return result;
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
    for (index = 0u; index < COLOR_CUBE_COUNT; ++index) {
        palette_write_entry((u8)(COLOR_CUBE_BASE + index),
                            themed(cube_color(index), current_theme));
    }
    for (index = COLOR_CUBE_BASE + COLOR_CUBE_COUNT; index < 256u; ++index) {
        palette_write_entry((u8)index, black);
    }
}

/* Published for the host: the palette blending is done in.
 *
 * This is deliberately the *untinted* table. gfx.c mixes colours in
 * these values and then looks the result up in the 6x6x6 cube; the theme
 * is applied afterwards, by the DAC, which is why an amber screen still
 * has smooth edges. The GPU has to blend in the same space to land on
 * the same cube slots, so the kernel hands its own table over rather
 * than letting the host guess. */
u8 dimos_base_palette[256u * 3u];

static void publish_base_palette(void) {
    u16 index;

    for (index = 0u; index < 256u; ++index) {
        Color value;

        if (index < PALETTE_SIZE) {
            value = palette[index];
        } else if (index < COLOR_CUBE_BASE + COLOR_CUBE_COUNT) {
            value = cube_color((u16)(index - COLOR_CUBE_BASE));
        } else {
            value = palette[COLOR_BLACK];
        }
        dimos_base_palette[index * 3u + 0u] = value.red;
        dimos_base_palette[index * 3u + 1u] = value.green;
        dimos_base_palette[index * 3u + 2u] = value.blue;
    }
}

/* gfx_shade() survives only as an identity: the blend itself happens on
 * the GPU, but app_paint.c calls this to pick a colour to store in its
 * own bitmap, so it has to return something sensible. */
u8 gfx_shade(u8 foreground, u8 background, u8 alpha) {
    return (alpha >= (u8)(FONT_ALPHA_MAX / 2u)) ? foreground : background;
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
/* The command list                                                    */
/* ------------------------------------------------------------------ */

static DimosCommand *push(u16 kind) {
    const u32 count = HEADER->count;
    DimosCommand *command;

    if (count >= COMMAND_LIMIT) {
        HEADER->overflowed = 1u;
        return (DimosCommand *)0;
    }
    command = &COMMANDS[count];
    command->kind = kind;
    command->x = 0;
    command->y = 0;
    command->width = 0;
    command->height = 0;
    command->color = 0u;
    command->background = 0u;
    command->a0 = 0;
    command->a1 = 0;
    command->reference = 0u;
    command->spare = 0u;
    HEADER->count = count + 1u;
    return command;
}

/* Called by gui.c once per frame, before anything is drawn. */
static void frame_begin(void) {
    HEADER->magic = COMMAND_MAGIC;
    HEADER->count = 0u;
    HEADER->overflowed = 0u;
    HEADER->font_mode = font_is_truetype;
    HEADER->blit_used = 0u;
}

/* ------------------------------------------------------------------ */
/* The drawing interface                                               */
/* ------------------------------------------------------------------ */

void gfx_clear(u8 color) {
    DimosCommand *command;

    /* A new frame starts here: gui.c clears before it draws. */
    frame_begin();
    command = push(COMMAND_CLEAR);
    if (command != (DimosCommand *)0) {
        command->color = color;
        command->width = (s16)SCREEN_WIDTH;
        command->height = (s16)SCREEN_HEIGHT;
    }
}

void gfx_pixel(s16 x, s16 y, u8 color) {
    DimosCommand *command = push(COMMAND_RECT);

    if (command != (DimosCommand *)0) {
        command->x = x;
        command->y = y;
        command->width = 1;
        command->height = 1;
        command->color = color;
        command->a0 = (s16)FONT_ALPHA_MAX;
    }
}

void gfx_pixel_blend(s16 x, s16 y, u8 color, u8 alpha) {
    DimosCommand *command;

    if (alpha == 0u) {
        return;
    }
    command = push(COMMAND_RECT);
    if (command != (DimosCommand *)0) {
        command->x = x;
        command->y = y;
        command->width = 1;
        command->height = 1;
        command->color = color;
        command->a0 = (s16)((alpha > (u8)FONT_ALPHA_MAX) ? (u8)FONT_ALPHA_MAX : alpha);
    }
}

void gfx_horizontal_line(s16 x, s16 y, s16 length, u8 color) {
    gfx_fill(x, y, length, 1, color);
}

void gfx_vertical_line(s16 x, s16 y, s16 length, u8 color) {
    gfx_fill(x, y, 1, length, color);
}

void gfx_fill(s16 x, s16 y, s16 width, s16 height, u8 color) {
    DimosCommand *command;

    if (width <= 0 || height <= 0) {
        return;
    }
    command = push(COMMAND_RECT);
    if (command != (DimosCommand *)0) {
        command->x = x;
        command->y = y;
        command->width = width;
        command->height = height;
        command->color = color;
        command->a0 = (s16)FONT_ALPHA_MAX;
    }
}

void gfx_outline(s16 x, s16 y, s16 width, s16 height, u8 color) {
    gfx_fill(x, y, width, 1, color);
    gfx_fill(x, (s16)(y + height - 1), width, 1, color);
    gfx_fill(x, y, 1, height, color);
    gfx_fill((s16)(x + width - 1), y, 1, height, color);
}

void gfx_line(s16 x0, s16 y0, s16 x1, s16 y1, u8 color) {
    DimosCommand *command = push(COMMAND_LINE);
    s16 left = (x0 < x1) ? x0 : x1;
    s16 top = (y0 < y1) ? y0 : y1;
    s16 right = (x0 > x1) ? x0 : x1;
    s16 bottom = (y0 > y1) ? y0 : y1;

    if (command == (DimosCommand *)0) {
        return;
    }
    command->x = left;
    command->y = top;
    command->width = (s16)(right - left + 1);
    command->height = (s16)(bottom - top + 1);
    command->color = color;
    command->reference = (u32)(((u32)(u16)(x0 - left) << 16) | (u16)(y0 - top));
    command->a0 = (s16)(x1 - left);
    command->a1 = (s16)(y1 - top);
}

void gfx_circle(s16 center_x, s16 center_y, s16 radius, u8 color, u8 filled) {
    DimosCommand *command = push(COMMAND_CIRCLE);

    if (command == (DimosCommand *)0) {
        return;
    }
    command->x = (s16)(center_x - radius - 1);
    command->y = (s16)(center_y - radius - 1);
    command->width = (s16)(2 * radius + 3);
    command->height = (s16)(2 * radius + 3);
    command->color = color;
    command->a0 = radius;
    command->a1 = (s16)filled;
}

void gfx_checker(s16 x, s16 y, s16 width, s16 height, u8 first, u8 second) {
    DimosCommand *command = push(COMMAND_CHECKER);

    if (command == (DimosCommand *)0) {
        return;
    }
    command->x = x;
    command->y = y;
    command->width = width;
    command->height = height;
    command->color = first;
    command->background = second;
}

void gfx_raised_box(s16 x, s16 y, s16 width, s16 height, u8 raised) {
    const u8 light = (raised != 0u) ? COLOR_HILITE : COLOR_SHADOW;
    const u8 dark = (raised != 0u) ? COLOR_SHADOW : COLOR_HILITE;

    gfx_fill(x, y, width, 1, light);
    gfx_fill(x, y, 1, height, light);
    gfx_fill(x, (s16)(y + height - 1), width, 1, dark);
    gfx_fill((s16)(x + width - 1), y, 1, height, dark);
}

void gfx_panel(s16 x, s16 y, s16 width, s16 height) {
    gfx_outline(x, y, width, height, COLOR_SHADOW);
    gfx_fill((s16)(x + 1), (s16)(y + 1), (s16)(width - 2), 1, COLOR_HILITE);
    gfx_fill((s16)(x + 1), (s16)(y + 1), 1, (s16)(height - 2), COLOR_HILITE);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

void gfx_character(s16 x, s16 y, char character, u8 color) {
    DimosCommand *command;

    if (character < 32 || character > 126) {
        character = '?';
    }
    command = push(COMMAND_GLYPH);
    if (command != (DimosCommand *)0) {
        command->x = x;
        command->y = y;
        command->width = 8;
        command->height = 8;
        command->color = color;
        command->reference = (u32)(u8)character;
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
/* Icons                                                               */
/* ------------------------------------------------------------------ */

/* The art itself is ASCII in the kernel's rodata. The host cannot follow
 * a guest pointer cheaply, so each distinct piece of art is given a slot
 * the first time it is drawn and copied into a small atlas the host
 * reads once. The smoothing that gfx.c used to do per pixel, per frame,
 * is not done here at all -- the shader does it from the atlas. */

typedef struct {
    const char *const *art;
    u16 rows;
} ArtSlot;

static ArtSlot art_slots[ART_SLOTS];
static u16 art_slot_count;

/* Published for the host: one byte per cell, 0 = empty, 1 = ink,
 * 2 = the secondary ('o') colour. */
u8 dimos_art_atlas[ART_SLOTS][ART_MAX_ROWS][ART_MAX_COLUMNS];
u8 dimos_art_rows[ART_SLOTS];
u8 dimos_art_columns[ART_SLOTS];
u32 dimos_art_count;

static u16 art_slot_for(const char *const *art, u16 rows) {
    u16 index;
    u16 row;
    u16 widest = 0u;

    for (index = 0u; index < art_slot_count; ++index) {
        if (art_slots[index].art == art && art_slots[index].rows == rows) {
            return index;
        }
    }
    if (art_slot_count >= ART_SLOTS || rows > ART_MAX_ROWS) {
        return 0xFFFFu;
    }
    index = art_slot_count++;
    art_slots[index].art = art;
    art_slots[index].rows = rows;

    for (row = 0u; row < rows; ++row) {
        const char *line = art[row];
        u16 column = 0u;

        while (line[column] != '\0' && column < ART_MAX_COLUMNS) {
            u8 cell = 0u;

            if (line[column] != '.') {
                cell = (u8)((line[column] == 'o') ? 2u : 1u);
            }
            dimos_art_atlas[index][row][column] = cell;
            ++column;
        }
        if (column > widest) {
            widest = column;
        }
    }
    dimos_art_rows[index] = (u8)rows;
    dimos_art_columns[index] = (u8)widest;
    dimos_art_count = art_slot_count;
    return index;
}

static void record_art(s16 x, s16 y, const char *const *art, u16 rows, u8 color,
                       u8 background, u8 opaque) {
    const u16 slot = art_slot_for(art, rows);
    DimosCommand *command;

    if (slot == 0xFFFFu) {
        return;
    }
    command = push(COMMAND_ART);
    if (command == (DimosCommand *)0) {
        return;
    }
    /* One pixel of bleed on every side, the same margin gfx.c used. */
    command->x = (s16)(x - 1);
    command->y = (s16)(y - 1);
    command->width = (s16)(dimos_art_columns[slot] + 2u);
    command->height = (s16)(dimos_art_rows[slot] + 2u);
    command->color = color;
    command->background = background;
    command->a0 = (s16)opaque;
    command->reference = slot;
}

void gfx_picture(s16 x, s16 y, const char *const *art, u16 rows, u8 color,
                 u8 background) {
    record_art(x, y, art, rows, color, background, 0u);
}

void gfx_picture_opaque(s16 x, s16 y, const char *const *art, u16 rows, u8 on,
                        u8 off) {
    record_art(x, y, art, rows, on, off, 1u);
}

/* ------------------------------------------------------------------ */
/* The pointer                                                         */
/* ------------------------------------------------------------------ */

static void record_blit(void);

void gfx_draw_pointer(s16 x, s16 y) {
    DimosCommand *command;

    /* The pointer goes on top of everything, so any hand-written pixels
     * are complete by now: pick them up before the arrow is recorded. */
    record_blit();

    command = push(COMMAND_POINTER);
    if (command != (DimosCommand *)0) {
        command->x = x;
        command->y = y;
        command->width = 12;
        command->height = 19;
        command->color = COLOR_CURSOR;
        command->background = COLOR_HILITE;
    }
}

/* ------------------------------------------------------------------ */
/* Handing the frame over                                              */
/* ------------------------------------------------------------------ */

/* The pixels an application wrote by hand this frame, kept where the
 * host can read them.
 *
 * The direct-write buffer itself is reset at the end of every frame, so
 * that the next frame can tell fresh writes from stale ones. The host
 * reads memory at the top of gfx_show(), by which point that reset has
 * already happened -- so the bytes are copied here first and this copy
 * is what gets uploaded. 64000 bytes of scratch, above the command list
 * and clear of everything the kernel uses. */
u8 *const dimos_blit_snapshot = (u8 *)(COMMAND_AREA_ADDRESS + COMMAND_AREA_BYTES);

/* Published for the host: the anti-aliased font atlas font_ttf.c built
 * at boot, or the BIOS bitmap when there is no TTF. */
const u8 *dimos_font_alpha;
const u8 *dimos_font_bits;

/* The blit is recorded by gui.c's last drawing call rather than by
 * gfx_show(), because the sandbox stops the machine on entry to
 * gfx_show() -- so anything done there would never run. gfx_draw_pointer
 * is the last thing drawn in a frame, so the scan happens from there. */
static void record_blit(void) {
    u32 index;
    s16 min_x = (s16)SCREEN_WIDTH;
    s16 min_y = (s16)SCREEN_HEIGHT;
    s16 max_x = -1;
    s16 max_y = -1;

    /* Did an application write pixels by hand this frame? Paint does;
     * nothing else in DimOS does. Those bytes are real pixels the kernel
     * produced, so they are sent as an image rather than as geometry. */
    for (index = 0u; index < SCREEN_BYTES; ++index) {
        if (screen[index] != BLIT_UNTOUCHED) {
            const s16 x = (s16)(index % SCREEN_WIDTH);
            const s16 y = (s16)(index / SCREEN_WIDTH);

            if (x < min_x) { min_x = x; }
            if (x > max_x) { max_x = x; }
            if (y < min_y) { min_y = y; }
            if (y > max_y) { max_y = y; }
        }
    }
    if (max_x >= 0) {
        DimosCommand *command;

        for (index = 0u; index < SCREEN_BYTES; ++index) {
            dimos_blit_snapshot[index] = screen[index];
        }
        command = push(COMMAND_BLIT);
        if (command != (DimosCommand *)0) {
            command->x = min_x;
            command->y = min_y;
            command->width = (s16)(max_x - min_x + 1);
            command->height = (s16)(max_y - min_y + 1);
        }
        HEADER->blit_used = 1u;
    }

    /* Reset for the next frame: from here on, any byte that is not
     * BLIT_UNTOUCHED was written by an application during that frame. */
    for (index = 0u; index < SCREEN_BYTES; ++index) {
        screen[index] = BLIT_UNTOUCHED;
    }
}

void gfx_show(void) {
    HEADER->frames = HEADER->frames + 1u;
}

void gfx_init(void) {
    u32 index;

    HEADER->magic = COMMAND_MAGIC;
    HEADER->count = 0u;
    HEADER->frames = 0u;

    /* Nothing has been written by hand yet. */
    for (index = 0u; index < SCREEN_BYTES; ++index) {
        screen[index] = BLIT_UNTOUCHED;
    }

    if (font_ttf_load() != 0u) {
        font_is_truetype = 1u;
        dimos_font_alpha = font_ttf_alpha_table();
        dimos_font_bits = font_ttf_table();
    } else {
        font_is_truetype = 0u;
        dimos_font_alpha = (const u8 *)0;
        dimos_font_bits = (const u8 *)(u32)bios_font_address;
    }
    publish_base_palette();
    palette_apply();
    gfx_clear(COLOR_DESKTOP);
}
