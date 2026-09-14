/*
 * dimos_commands.h -- what the kernel asked to be drawn.
 *
 * DimOS normally ends every drawing call in a byte written to a buffer:
 * gfx.c is a software rasterizer. For the Vulkan path that rasterizer is
 * removed entirely (tools/vkviewer/gfx_record.c replaces gfx.c) and each
 * call is recorded as one of these commands instead. Nothing is shaded,
 * blended, sampled or filled on the processor; the list below is the
 * whole output of a frame, and the GPU turns it into pixels.
 *
 * The same header is compiled into the guest (the recorder) and into the
 * host renderer, so both agree on the layout byte for byte.
 */

#ifndef DIMOS_COMMANDS_H
#define DIMOS_COMMANDS_H

/* Where the recorder leaves the list inside the emulated machine. Chosen
 * above the kernel and below the FONT.TTF work area, so it collides with
 * nothing the kernel already uses. */
#define COMMAND_AREA_ADDRESS 0x00200000u
#define COMMAND_AREA_BYTES (1024u * 1024u)
#define COMMAND_LIMIT 16384u
#define COMMAND_MAGIC 0x55504744u /* "DGPU" little endian */

/* A pixel the kernel never touched directly. gfx_clear() paints the
 * direct-write buffer with this, so the recorder can tell which pixels
 * an application wrote by hand (only Paint does) from the ones that were
 * left alone. No palette role uses index 255. */
#define BLIT_UNTOUCHED 0xFFu

/* The art atlas. Nine pieces of art exist in DimOS today (eight
 * application icons at 16x16 and the home button at 8x7), so sixteen
 * slots of 16x16 is room to spare and still small enough to sit in the
 * kernel's BSS. The renderer uses the same numbers. */
#define ART_SLOTS 16u
#define ART_MAX_ROWS 16u
#define ART_MAX_COLUMNS 16u
#define ART_SLOT_CELLS (ART_MAX_ROWS * ART_MAX_COLUMNS)

enum {
    COMMAND_CLEAR = 0u,   /* whole frame, one colour                      */
    COMMAND_RECT,         /* solid rectangle                              */
    COMMAND_GLYPH,        /* one character from the font atlas            */
    COMMAND_ART,          /* an icon, smoothed on the GPU                 */
    COMMAND_CIRCLE,       /* filled disc or ring                          */
    COMMAND_LINE,         /* straight line between two points             */
    COMMAND_CHECKER,      /* two colour chequerboard                      */
    COMMAND_POINTER,      /* the mouse arrow, an analytic polygon         */
    COMMAND_BLIT          /* raw pixels an application wrote itself       */
};

/* One entry, 24 bytes. The meaning of the spare fields depends on kind:
 *
 *   RECT      x,y,width,height, colour
 *   CLEAR     colour
 *   GLYPH     x,y (8x8 cell), colour, reference = character code
 *   ART       x,y,width,height cover the art plus one pixel of bleed,
 *             colour = ink, background = the colour of 'o' cells,
 *             a0 = 1 when both cell kinds use the ink colour
 *             (gfx_picture_opaque), reference = slot in the art atlas
 *   CIRCLE    x,y,width,height is the bounding box, a0 = radius,
 *             a1 = 1 when filled
 *   LINE      x,y,width,height is the bounding box, reference packs the
 *             start point (x << 16 | y) and a0,a1 hold the end point,
 *             all relative to the box
 *   CHECKER   x,y,width,height, colour = first, background = second
 *   POINTER   x,y, the box is the 12x19 the arrow needs
 *   BLIT      x,y,width,height of the region to take from the direct
 *             write buffer
 *
 * a0 also carries coverage 0..16 for a single blended pixel drawn as a
 * 1x1 RECT, which is how gfx_pixel_blend() is recorded.
 */
typedef struct {
    unsigned short kind;
    short x;
    short y;
    short width;
    short height;
    unsigned char color;
    unsigned char background;
    short a0;
    short a1;
    unsigned int reference;
    unsigned int spare;
} DimosCommand;

/* The head of the list, at COMMAND_AREA_ADDRESS. */
typedef struct {
    unsigned int magic;
    unsigned int count;       /* commands in this frame                  */
    unsigned int overflowed;  /* 1 if the frame needed more than the cap */
    unsigned int font_mode;   /* 0 = BIOS 8x8 bitmap, 1 = TrueType alpha */
    unsigned int frames;      /* frames recorded since boot              */
    unsigned int blit_used;   /* 1 when an application wrote pixels      */
    unsigned int spare[2];
} DimosCommandHeader;

#endif /* DIMOS_COMMANDS_H */
