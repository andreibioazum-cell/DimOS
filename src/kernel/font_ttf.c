/*
 * font_ttf.c -- a real TrueType font, read from the boot disk at boot.
 *
 * Put any TrueType font on the volume as FONT.TTF (the build copies
 * fonts/font.ttf there) and DimOS renders every piece of text with it.
 * This file is a complete, freestanding TrueType reader:
 *
 *   1. the sfnt directory is walked to find head/maxp/cmap/loca/glyf;
 *   2. cmap (formats 4, 12, 6 and 0) maps ASCII to glyph numbers;
 *   3. glyf outlines are decoded -- simple glyphs point by point,
 *      composite glyphs by recursing into their parts;
 *   4. the quadratic bezier outlines are flattened into line segments
 *      and filled with the non-zero winding rule on a 4x4 supersampled
 *      grid, giving each of the 95 printable ASCII characters an 8x8
 *      bitmap in the exact format gfx.c draws with.
 *
 * Everything is 32 bit integer math (26.6 fixed point on the pixel
 * grid): no floats, no libc, no allocations. The file and the scratch
 * tables live in fixed memory below the RAM disk, so none of this
 * counts against the kernel's own 64 KiB window.
 *
 * If FONT.TTF is missing, truncated or not a TrueType file, the loader
 * reports failure and gfx.c falls back to the BIOS font, so a bad font
 * can never take the screen down.
 */

#include "dimos.h"

/* ------------------------------------------------------------------ */
/* Fixed memory layout (all below the RAM disk at 0x500000)            */
/* ------------------------------------------------------------------ */

#ifdef DIMOS_HOST_TEST
/* The host preview tool (tools/font_preview.c) supplies plain buffers. */
u8 *font_ttf_work_area;
#define WORK ((FontWork *)font_ttf_work_area)
#else
#define FONT_FILE_ADDRESS 0x00400000u /* the raw FONT.TTF bytes        */
#define FONT_FILE_MAX_BYTES (768u * 1024u)
#define FONT_WORK_ADDRESS 0x004C0000u /* outline points and edge lists */
#define WORK ((FontWork *)FONT_WORK_ADDRESS)
#endif

/* ------------------------------------------------------------------ */
/* Rasterizer geometry                                                 */
/* ------------------------------------------------------------------ */

/* Every glyph cell is 8x8 pixels, each pixel is judged from a 4x4 block
 * of samples, and sample coordinates carry 6 fraction bits (26.6). */
#define SAMPLES_PER_PIXEL 4
#define GRID (8 * SAMPLES_PER_PIXEL)
#define FP_SHIFT 6
#define FP_HALF (1 << (FP_SHIFT - 1))
#define GRID_FP (GRID << FP_SHIFT)

/* A pixel lights up when at least this many of its 16 samples fall
 * inside the outline. If a whole glyph would vanish that way (a tiny
 * dot, a thin quote), the threshold drops until ink appears. */
#define INK_THRESHOLD 5u

/* Grid coordinates are clamped into this range so that every product
 * in the scanline math stays far away from 32 bit overflow. */
#define COORD_LIMIT (GRID_FP * 8)

/* How many straight segments approximate one quadratic bezier. */
#define BEZIER_STEPS 8

#define MAX_POINTS 1024u
#define MAX_CONTOURS 96u
#define MAX_EDGES 8192u
#define MAX_COMPONENT_DEPTH 4u
#define MAX_CROSSINGS 96u

typedef struct {
    s32 x0, y0, x1, y1; /* 26.6 grid coordinates */
} Edge;

typedef struct {
    /* One glyph's outline in font units (already transformed when the
     * glyph is part of a composite). */
    s32 point_x[MAX_POINTS];
    s32 point_y[MAX_POINTS];
    u8 point_flags[MAX_POINTS]; /* bit 0: the point is on the curve */
    u16 contour_end[MAX_CONTOURS];
    u16 point_count;
    u16 contour_count;

    /* The same outline flattened into line segments on the pixel grid. */
    Edge edge[MAX_EDGES];
    u16 edge_count;
} FontWork;

/* ------------------------------------------------------------------ */
/* The parsed font                                                     */
/* ------------------------------------------------------------------ */

static const u8 *ttf;
static u32 ttf_size;
static u8 parse_error; /* set once by any out-of-bounds read */

static u32 table_cmap, table_cmap_size;
static u32 table_glyf, table_glyf_size;
static u32 table_head;
static u32 table_loca, table_loca_size;
static u32 table_maxp;
static u32 table_hhea;

static u16 units_per_em;
static u8 long_loca; /* head.indexToLocFormat: 0 = 16 bit, 1 = 32 bit */
static u16 glyph_count;
static s32 em_ascent;     /* baseline to the top of the design space  */
static s32 em_descent;    /* baseline to the bottom (a negative number) */
static u32 cmap_subtable; /* offset of the chosen encoding subtable   */

/* The finished product: 128 glyphs, 8 bytes each, bit 7 = left pixel.
 * The layout matches the BIOS font table exactly, so gfx.c can point
 * at either one with the same drawing code. */
static u8 glyph_table[128u * 8u];

/* ------------------------------------------------------------------ */
/* Bounds-checked big-endian readers                                   */
/* ------------------------------------------------------------------ */

static u8 read_u8(u32 offset) {
    if (offset >= ttf_size) {
        parse_error = 1u;
        return 0u;
    }
    return ttf[offset];
}

static u16 read_u16(u32 offset) {
    if (offset + 2u > ttf_size || offset + 2u < offset) {
        parse_error = 1u;
        return 0u;
    }
    return (u16)(((u16)ttf[offset] << 8) | (u16)ttf[offset + 1u]);
}

static s16 read_s16(u32 offset) {
    return (s16)read_u16(offset);
}

static u32 read_u32(u32 offset) {
    if (offset + 4u > ttf_size || offset + 4u < offset) {
        parse_error = 1u;
        return 0u;
    }
    return ((u32)ttf[offset] << 24) | ((u32)ttf[offset + 1u] << 16) |
           ((u32)ttf[offset + 2u] << 8) | (u32)ttf[offset + 3u];
}

/* ------------------------------------------------------------------ */
/* sfnt table directory                                                */
/* ------------------------------------------------------------------ */

static u32 tag_word(char a, char b, char c, char d) {
    return ((u32)(u8)a << 24) | ((u32)(u8)b << 16) | ((u32)(u8)c << 8) |
           (u32)(u8)d;
}

static u8 find_tables(void) {
    u32 version = read_u32(0u);
    u16 count;
    u16 index;

    table_cmap = 0u;
    table_glyf = 0u;
    table_head = 0u;
    table_loca = 0u;
    table_maxp = 0u;
    table_hhea = 0u;

    /* 0x00010000 is the classic TrueType version; 'true' is what old
     * Apple fonts carry. 'OTTO' would mean PostScript outlines, which
     * this reader does not speak. */
    if (version != 0x00010000u && version != tag_word('t', 'r', 'u', 'e')) {
        return 0u;
    }
    count = read_u16(4u);
    if (count == 0u || count > 64u) {
        return 0u;
    }

    for (index = 0u; index < count; ++index) {
        const u32 record = 12u + (u32)index * 16u;
        const u32 tag = read_u32(record);
        const u32 offset = read_u32(record + 8u);
        const u32 length = read_u32(record + 12u);

        if (offset >= ttf_size || length > ttf_size - offset) {
            return 0u; /* the directory promises bytes the file lacks */
        }
        if (tag == tag_word('c', 'm', 'a', 'p')) {
            table_cmap = offset;
            table_cmap_size = length;
        } else if (tag == tag_word('g', 'l', 'y', 'f')) {
            table_glyf = offset;
            table_glyf_size = length;
        } else if (tag == tag_word('h', 'e', 'a', 'd')) {
            table_head = offset;
        } else if (tag == tag_word('l', 'o', 'c', 'a')) {
            table_loca = offset;
            table_loca_size = length;
        } else if (tag == tag_word('m', 'a', 'x', 'p')) {
            table_maxp = offset;
        } else if (tag == tag_word('h', 'h', 'e', 'a')) {
            table_hhea = offset;
        }
    }
    return (table_cmap != 0u && table_glyf != 0u && table_head != 0u &&
            table_loca != 0u && table_maxp != 0u)
               ? 1u
               : 0u;
}

static u8 read_header_tables(void) {
    if (read_u32(table_head + 12u) != 0x5F0F3CF5u) {
        return 0u; /* head.magicNumber -- the fixed TrueType magic */
    }
    units_per_em = read_u16(table_head + 18u);
    long_loca = (read_s16(table_head + 50u) != 0) ? 1u : 0u;
    glyph_count = read_u16(table_maxp + 4u);
    if (units_per_em < 16u || glyph_count == 0u) {
        return 0u;
    }

    /* hhea gives the design-space height used to scale glyphs into the
     * 8 pixel cell. Fall back to head's global box if hhea is absent
     * or nonsense. */
    if (table_hhea != 0u) {
        em_ascent = (s32)read_s16(table_hhea + 4u);
        em_descent = (s32)read_s16(table_hhea + 6u);
    } else {
        em_ascent = (s32)read_s16(table_head + 42u);  /* head.yMax */
        em_descent = (s32)read_s16(table_head + 38u); /* head.yMin */
    }
    if (em_ascent <= 0 || em_descent > 0 ||
        (em_ascent - em_descent) < (s32)units_per_em / 2 ||
        (em_ascent - em_descent) > (s32)units_per_em * 4) {
        em_ascent = ((s32)units_per_em * 4) / 5;
        em_descent = em_ascent - (s32)units_per_em;
    }
    return (parse_error == 0u) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* cmap: character code -> glyph number                                */
/* ------------------------------------------------------------------ */

static u8 pick_cmap_subtable(void) {
    const u16 encoding_count = read_u16(table_cmap + 2u);
    u32 best_score = 0u;
    u16 index;

    cmap_subtable = 0u;
    if (encoding_count == 0u || encoding_count > 32u) {
        return 0u;
    }
    for (index = 0u; index < encoding_count; ++index) {
        const u32 record = table_cmap + 4u + (u32)index * 8u;
        const u16 platform = read_u16(record);
        const u16 encoding = read_u16(record + 2u);
        const u32 offset = read_u32(record + 4u);
        u32 score = 0u;

        if (offset >= table_cmap_size) {
            continue;
        }
        if (platform == 3u && encoding == 1u) {
            score = 4u; /* Windows Unicode BMP -- present in every font */
        } else if (platform == 0u) {
            score = 3u; /* any Unicode encoding */
        } else if (platform == 3u && encoding == 0u) {
            score = 2u; /* Windows symbol */
        } else if (platform == 1u && encoding == 0u) {
            score = 1u; /* old Macintosh Roman */
        }
        if (score > best_score) {
            best_score = score;
            cmap_subtable = table_cmap + offset;
        }
    }
    return (best_score != 0u) ? 1u : 0u;
}

static u16 glyph_number(u16 code) {
    const u16 format = read_u16(cmap_subtable);

    if (format == 4u) {
        const u16 segment_count_x2 = read_u16(cmap_subtable + 6u);
        const u32 ends = cmap_subtable + 14u;
        const u32 starts = ends + (u32)segment_count_x2 + 2u;
        const u32 deltas = starts + (u32)segment_count_x2;
        const u32 ranges = deltas + (u32)segment_count_x2;
        u16 segment;

        for (segment = 0u; segment + 2u <= segment_count_x2; segment += 2u) {
            if (read_u16(ends + segment) < code) {
                continue;
            }
            if (read_u16(starts + segment) > code) {
                return 0u; /* the covering segment starts past this code */
            }
            {
                const u16 range_offset = read_u16(ranges + segment);
                if (range_offset == 0u) {
                    return (u16)(code + read_u16(deltas + segment));
                }
                {
                    const u32 slot =
                        ranges + segment + range_offset +
                        (u32)(code - read_u16(starts + segment)) * 2u;
                    const u16 glyph = read_u16(slot);
                    if (glyph == 0u) {
                        return 0u;
                    }
                    return (u16)(glyph + read_u16(deltas + segment));
                }
            }
        }
        return 0u;
    }

    if (format == 12u) {
        const u32 group_count = read_u32(cmap_subtable + 12u);
        u32 group;

        if (group_count > 65536u) {
            return 0u;
        }
        for (group = 0u; group < group_count; ++group) {
            const u32 record = cmap_subtable + 16u + group * 12u;
            const u32 first = read_u32(record);
            const u32 last = read_u32(record + 4u);
            if ((u32)code >= first && (u32)code <= last) {
                return (u16)(read_u32(record + 8u) + ((u32)code - first));
            }
        }
        return 0u;
    }

    if (format == 6u) {
        const u16 first = read_u16(cmap_subtable + 6u);
        const u16 entry_count = read_u16(cmap_subtable + 8u);
        if (code < first || (u16)(code - first) >= entry_count) {
            return 0u;
        }
        return read_u16(cmap_subtable + 10u + (u32)(code - first) * 2u);
    }

    if (format == 0u) {
        if (code > 255u) {
            return 0u;
        }
        return read_u8(cmap_subtable + 6u + code);
    }

    return 0u;
}

/* ------------------------------------------------------------------ */
/* glyf: decoding outlines                                             */
/* ------------------------------------------------------------------ */

/* Where glyph number "glyph" starts inside glyf. Returns 0 for an
 * empty glyph (a space) and on any inconsistency. */
static u8 glyph_location(u16 glyph, u32 *offset) {
    u32 this_one;
    u32 next_one;

    if (glyph >= glyph_count) {
        return 0u;
    }
    if (long_loca != 0u) {
        if (((u32)glyph + 2u) * 4u > table_loca_size) {
            return 0u;
        }
        this_one = read_u32(table_loca + (u32)glyph * 4u);
        next_one = read_u32(table_loca + (u32)glyph * 4u + 4u);
    } else {
        if (((u32)glyph + 2u) * 2u > table_loca_size) {
            return 0u;
        }
        this_one = (u32)read_u16(table_loca + (u32)glyph * 2u) * 2u;
        next_one = (u32)read_u16(table_loca + (u32)glyph * 2u + 2u) * 2u;
    }
    if (next_one <= this_one || next_one > table_glyf_size) {
        return 0u; /* empty glyph, or loca points outside glyf */
    }
    *offset = table_glyf + this_one;
    return 1u;
}

/* Composite glyphs place their parts through a 2x2 matrix in F2Dot14
 * (14 fraction bits) plus an offset in font units. */
typedef struct {
    s32 m00, m01, m10, m11; /* F2Dot14 */
    s32 dx, dy;             /* font units */
} Transform;

static s32 transform_x(const Transform *t, s32 x, s32 y) {
    return ((t->m00 * x + t->m01 * y) >> 14) + t->dx;
}

static s32 transform_y(const Transform *t, s32 x, s32 y) {
    return ((t->m10 * x + t->m11 * y) >> 14) + t->dy;
}

static u8 decode_glyph(u16 glyph, const Transform *transform, u8 depth);

/* A simple glyph: contour end list, hinting bytes (skipped), flags with
 * run-length compression, then packed x and y deltas. */
static u8 decode_simple_glyph(u32 at, u16 contours, const Transform *transform) {
    const u16 first_point = WORK->point_count;
    u32 cursor = at + 10u;
    u16 point_total = 0u;
    u16 index;
    s32 value;

    if (contours > (u16)(MAX_CONTOURS - WORK->contour_count)) {
        return 0u;
    }
    for (index = 0u; index < contours; ++index) {
        const u16 end = read_u16(cursor);
        cursor += 2u;
        if ((index != 0u && end < point_total) || end == 0xFFFFu) {
            return 0u; /* contour ends must be increasing */
        }
        point_total = (u16)(end + 1u);
        WORK->contour_end[WORK->contour_count + index] =
            (u16)(first_point + end);
    }
    if (point_total == 0u ||
        point_total > (u16)(MAX_POINTS - WORK->point_count)) {
        return 0u;
    }

    cursor += 2u + (u32)read_u16(cursor); /* skip hinting instructions */

    /* Flags, with the "repeat previous flag" compression. */
    for (index = 0u; index < point_total;) {
        const u8 flag = read_u8(cursor);
        u16 repeat = 1u;

        ++cursor;
        if ((flag & 0x08u) != 0u) {
            repeat = (u16)(1u + read_u8(cursor));
            ++cursor;
        }
        while (repeat != 0u && index < point_total) {
            WORK->point_flags[first_point + index] = flag;
            ++index;
            --repeat;
        }
        if (parse_error != 0u) {
            return 0u;
        }
    }

    /* X coordinates: bit 1 = a one byte delta whose sign is bit 4;
     * otherwise bit 4 says "same as before", else a signed word. */
    value = 0;
    for (index = 0u; index < point_total; ++index) {
        const u8 flag = WORK->point_flags[first_point + index];
        if ((flag & 0x02u) != 0u) {
            const s32 step = (s32)read_u8(cursor);
            ++cursor;
            value += ((flag & 0x10u) != 0u) ? step : -step;
        } else if ((flag & 0x10u) == 0u) {
            value += (s32)read_s16(cursor);
            cursor += 2u;
        }
        WORK->point_x[first_point + index] = value;
    }

    /* Y coordinates: the same scheme with bits 2 and 5. */
    value = 0;
    for (index = 0u; index < point_total; ++index) {
        const u8 flag = WORK->point_flags[first_point + index];
        if ((flag & 0x04u) != 0u) {
            const s32 step = (s32)read_u8(cursor);
            ++cursor;
            value += ((flag & 0x20u) != 0u) ? step : -step;
        } else if ((flag & 0x20u) == 0u) {
            value += (s32)read_s16(cursor);
            cursor += 2u;
        }
        WORK->point_y[first_point + index] = value;
    }

    if (parse_error != 0u) {
        return 0u;
    }

    /* Apply the composite transform (the identity for plain glyphs). */
    for (index = 0u; index < point_total; ++index) {
        const s32 x = WORK->point_x[first_point + index];
        const s32 y = WORK->point_y[first_point + index];
        WORK->point_x[first_point + index] = transform_x(transform, x, y);
        WORK->point_y[first_point + index] = transform_y(transform, x, y);
    }

    WORK->point_count = (u16)(first_point + point_total);
    WORK->contour_count = (u16)(WORK->contour_count + contours);
    return 1u;
}

/* Composite glyph component flags. */
#define COMPONENT_WORD_ARGS 0x0001u
#define COMPONENT_XY_ARGS 0x0002u
#define COMPONENT_HAS_SCALE 0x0008u
#define COMPONENT_MORE 0x0020u
#define COMPONENT_XY_SCALE 0x0040u
#define COMPONENT_MATRIX 0x0080u

static u8 decode_composite_glyph(u32 at, const Transform *outer, u8 depth) {
    u32 cursor = at + 10u;
    u16 flags;
    u16 parts = 0u;

    do {
        u16 part_glyph;
        Transform local;
        Transform combined;
        s32 dx = 0;
        s32 dy = 0;

        flags = read_u16(cursor);
        part_glyph = read_u16(cursor + 2u);
        cursor += 4u;

        if ((flags & COMPONENT_WORD_ARGS) != 0u) {
            if ((flags & COMPONENT_XY_ARGS) != 0u) {
                dx = (s32)read_s16(cursor);
                dy = (s32)read_s16(cursor + 2u);
            }
            cursor += 4u;
        } else {
            if ((flags & COMPONENT_XY_ARGS) != 0u) {
                dx = (s32)(s8)read_u8(cursor);
                dy = (s32)(s8)read_u8(cursor + 1u);
            }
            cursor += 2u;
        }
        /* Arguments that are point numbers instead of offsets are so
         * rare in practice that such a part is simply skipped. */

        local.m00 = 1 << 14;
        local.m01 = 0;
        local.m10 = 0;
        local.m11 = 1 << 14;
        local.dx = dx;
        local.dy = dy;
        if ((flags & COMPONENT_HAS_SCALE) != 0u) {
            local.m00 = (s32)read_s16(cursor);
            local.m11 = local.m00;
            cursor += 2u;
        } else if ((flags & COMPONENT_XY_SCALE) != 0u) {
            local.m00 = (s32)read_s16(cursor);
            local.m11 = (s32)read_s16(cursor + 2u);
            cursor += 4u;
        } else if ((flags & COMPONENT_MATRIX) != 0u) {
            local.m00 = (s32)read_s16(cursor);
            local.m01 = (s32)read_s16(cursor + 2u);
            local.m10 = (s32)read_s16(cursor + 4u);
            local.m11 = (s32)read_s16(cursor + 6u);
            cursor += 8u;
        }

        /* combined = outer o local: first place the part inside this
         * glyph, then apply whatever placed this glyph itself. */
        combined.m00 = (outer->m00 * local.m00 + outer->m01 * local.m10) >> 14;
        combined.m01 = (outer->m00 * local.m01 + outer->m01 * local.m11) >> 14;
        combined.m10 = (outer->m10 * local.m00 + outer->m11 * local.m10) >> 14;
        combined.m11 = (outer->m10 * local.m01 + outer->m11 * local.m11) >> 14;
        combined.dx = transform_x(outer, local.dx, local.dy);
        combined.dy = transform_y(outer, local.dx, local.dy);

        if (parse_error != 0u) {
            return 0u;
        }
        if ((flags & COMPONENT_XY_ARGS) != 0u) {
            if (decode_glyph(part_glyph, &combined, (u8)(depth + 1u)) == 0u) {
                return 0u;
            }
        }
        if (++parts > 16u) {
            return 0u; /* no sane glyph has this many parts */
        }
    } while ((flags & COMPONENT_MORE) != 0u);

    return 1u;
}

static u8 decode_glyph(u16 glyph, const Transform *transform, u8 depth) {
    u32 at;
    s16 contours;

    if (depth >= MAX_COMPONENT_DEPTH) {
        return 0u;
    }
    if (glyph_location(glyph, &at) == 0u) {
        return 1u; /* an empty glyph is a valid glyph (space) */
    }
    contours = read_s16(at);
    if (contours >= 0) {
        if (contours == 0) {
            return 1u;
        }
        return decode_simple_glyph(at, (u16)contours, transform);
    }
    return decode_composite_glyph(at, transform, depth);
}

/* ------------------------------------------------------------------ */
/* Flattening: outlines -> straight edges on the sample grid           */
/* ------------------------------------------------------------------ */

/* The mapping from font units to 26.6 grid coordinates, chosen once
 * per font from real measurements (capital height, descender depth)
 * and adjusted per glyph so wide letters squeeze into their cell. */
static s32 scale_y_num, scale_y_den; /* grid y per font unit  */
static s32 scale_x_num, scale_x_den; /* grid x per font unit  */
static s32 glyph_min_x;              /* leftmost point, units */
static s32 glyph_x_shift;            /* centering offset, 26.6 */
static s32 glyph_baseline;           /* baseline, 26.6         */

static s32 clamp_coordinate(s32 value) {
    if (value > COORD_LIMIT) {
        return COORD_LIMIT;
    }
    if (value < -COORD_LIMIT) {
        return -COORD_LIMIT;
    }
    return value;
}

static s32 grid_x(s32 units) {
    return clamp_coordinate(((units - glyph_min_x) * scale_x_num) /
                                scale_x_den + glyph_x_shift);
}

static s32 grid_y(s32 units) {
    return clamp_coordinate(glyph_baseline -
                            (units * scale_y_num) / scale_y_den);
}


static void push_edge(s32 x0, s32 y0, s32 x1, s32 y1) {
    Edge *edge;

    if (y0 == y1 || WORK->edge_count >= MAX_EDGES) {
        return; /* horizontal edges never cross a scanline */
    }
    edge = &WORK->edge[WORK->edge_count++];
    edge->x0 = x0;
    edge->y0 = y0;
    edge->x1 = x1;
    edge->y1 = y1;
}

/* One quadratic bezier, flattened into BEZIER_STEPS straight pieces.
 * t runs 0..64 so (64-t) and t keep every product inside 32 bits. */
static void push_bezier(s32 x0, s32 y0, s32 cx, s32 cy, s32 x1, s32 y1) {
    s32 previous_x = x0;
    s32 previous_y = y0;
    s32 step;

    for (step = 1; step <= BEZIER_STEPS; ++step) {
        const s32 t = (step * 64) / BEZIER_STEPS;
        const s32 u = 64 - t;
        const s32 next_x = (u * u * x0 + 2 * u * t * cx + t * t * x1 + 2048) >> 12;
        const s32 next_y = (u * u * y0 + 2 * u * t * cy + t * t * y1 + 2048) >> 12;

        push_edge(previous_x, previous_y, next_x, next_y);
        previous_x = next_x;
        previous_y = next_y;
    }
}

/* Turn the decoded outline into edges. Off-curve points are bezier
 * controls; two off-curve points in a row imply an on-curve point
 * between them (the classic TrueType shorthand). */
static void flatten_outline(void) {
    u16 contour;
    u16 start = 0u;

    WORK->edge_count = 0u;
    for (contour = 0u; contour < WORK->contour_count; ++contour) {
        const u16 end = WORK->contour_end[contour];
        const u16 count = (u16)(end - start + 1u);
        u16 index;
        s32 start_x = 0, start_y = 0;     /* first on-curve point   */
        s32 current_x = 0, current_y = 0; /* pen position           */
        s32 pending_x = 0, pending_y = 0; /* waiting bezier control */
        u8 has_pending = 0u;
        u8 has_start = 0u;

        if (end >= WORK->point_count || count < 2u) {
            start = (u16)(end + 1u);
            continue;
        }

        /* Walk count+1 steps so the contour closes on its first point;
         * one extra control step handles a trailing off-curve point. */
        for (index = 0u; index <= count + 1u; ++index) {
            const u16 slot = (u16)(start + ((index >= count) ? (index - count)
                                                             : index));
            const s32 x = grid_x(WORK->point_x[slot]);
            const s32 y = grid_y(WORK->point_y[slot]);
            const u8 on_curve = (u8)(WORK->point_flags[slot] & 0x01u);

            if (has_start == 0u) {
                if (on_curve != 0u) {
                    start_x = x;
                    start_y = y;
                    current_x = x;
                    current_y = y;
                    has_start = 1u;
                    /* A control seen before the start belongs to the
                     * closing curve; the wrap-around visits it again. */
                    has_pending = 0u;
                } else if (has_pending != 0u) {
                    /* Two off-curve points open the contour: the curve
                     * really passes through their midpoint, so start
                     * there and keep this point as the next control. */
                    start_x = (pending_x + x) / 2;
                    start_y = (pending_y + y) / 2;
                    current_x = start_x;
                    current_y = start_y;
                    has_start = 1u;
                    pending_x = x;
                    pending_y = y;
                } else {
                    /* The contour opens off-curve: remember the control
                     * until an on-curve start shows up. */
                    pending_x = x;
                    pending_y = y;
                    has_pending = 1u;
                }
                continue;
            }

            if (on_curve != 0u) {
                if (has_pending != 0u) {
                    push_bezier(current_x, current_y, pending_x, pending_y,
                                x, y);
                    has_pending = 0u;
                } else {
                    push_edge(current_x, current_y, x, y);
                }
                current_x = x;
                current_y = y;
            } else {
                if (has_pending != 0u) {
                    /* Two controls in a row: the midpoint is a real
                     * point on the curve. */
                    const s32 mid_x = (pending_x + x) / 2;
                    const s32 mid_y = (pending_y + y) / 2;
                    push_bezier(current_x, current_y, pending_x, pending_y,
                                mid_x, mid_y);
                    current_x = mid_x;
                    current_y = mid_y;
                }
                pending_x = x;
                pending_y = y;
                has_pending = 1u;
            }

            if (index >= count) {
                break; /* the contour is closed */
            }
        }

        if (has_start != 0u) {
            if (has_pending != 0u) {
                push_bezier(current_x, current_y, pending_x, pending_y,
                            start_x, start_y);
            } else if (current_x != start_x || current_y != start_y) {
                push_edge(current_x, current_y, start_x, start_y);
            }
        }
        start = (u16)(end + 1u);
    }
}

/* ------------------------------------------------------------------ */
/* Filling: edges -> an 8x8 bitmap                                     */
/* ------------------------------------------------------------------ */

/* Count how many samples of each pixel are inside the outline, using
 * the non-zero winding rule on GRID x GRID sample points. */
static void count_coverage(u8 coverage[64]) {
    s32 row;

    for (row = 0; row < GRID; ++row) {
        const s32 sample_y = (row << FP_SHIFT) + FP_HALF;
        s32 crossing_x[MAX_CROSSINGS];
        s8 crossing_dir[MAX_CROSSINGS];
        u16 crossings = 0u;
        u16 index;
        s32 column;

        for (index = 0u; index < WORK->edge_count; ++index) {
            const Edge *edge = &WORK->edge[index];
            s32 top_y, bottom_y, top_x, bottom_x;
            s8 direction;

            if (edge->y0 < edge->y1) {
                top_y = edge->y0;
                top_x = edge->x0;
                bottom_y = edge->y1;
                bottom_x = edge->x1;
                direction = 1;
            } else {
                top_y = edge->y1;
                top_x = edge->x1;
                bottom_y = edge->y0;
                bottom_x = edge->x0;
                direction = -1;
            }
            if (sample_y < top_y || sample_y >= bottom_y) {
                continue;
            }
            if (crossings >= MAX_CROSSINGS) {
                break;
            }
            crossing_x[crossings] =
                top_x + ((bottom_x - top_x) * (sample_y - top_y)) /
                            (bottom_y - top_y);
            crossing_dir[crossings] = direction;
            ++crossings;
        }

        if (crossings == 0u) {
            continue;
        }
        for (column = 0; column < GRID; ++column) {
            const s32 sample_x = (column << FP_SHIFT) + FP_HALF;
            s32 winding = 0;

            for (index = 0u; index < crossings; ++index) {
                if (crossing_x[index] <= sample_x) {
                    winding += crossing_dir[index];
                }
            }
            if (winding != 0) {
                ++coverage[(row / SAMPLES_PER_PIXEL) * 8 +
                           (column / SAMPLES_PER_PIXEL)];
            }
        }
    }
}

/* Coverage counts -> 8 row bytes, using the highest threshold that
 * still leaves the glyph visible. */
static void coverage_to_rows(const u8 coverage[64], u8 *rows) {
    u8 threshold = (u8)INK_THRESHOLD;
    u8 best = 0u;
    u8 cell;

    for (cell = 0u; cell < 64u; ++cell) {
        if (coverage[cell] > best) {
            best = coverage[cell];
        }
    }
    if (best == 0u) {
        return; /* genuinely empty (space) */
    }
    if (best < threshold) {
        threshold = best; /* thin marks stay visible */
    }

    for (cell = 0u; cell < 64u; ++cell) {
        if (coverage[cell] >= threshold) {
            rows[cell / 8u] |= (u8)(0x80u >> (cell % 8u));
        }
    }
}

/* Decode one character's outline into WORK. Returns 0 on failure. */
static u8 decode_character(u16 code) {
    Transform identity;
    u16 glyph;

    parse_error = 0u;
    identity.m00 = 1 << 14;
    identity.m01 = 0;
    identity.m10 = 0;
    identity.m11 = 1 << 14;
    identity.dx = 0;
    identity.dy = 0;

    WORK->point_count = 0u;
    WORK->contour_count = 0u;
    glyph = glyph_number(code);
    if (glyph == 0u && code != 0x20u) {
        return 0u; /* the font does not map this character */
    }
    if (decode_glyph(glyph, &identity, 0u) == 0u || parse_error != 0u) {
        WORK->point_count = 0u;
        WORK->contour_count = 0u;
        return 0u;
    }
    return 1u;
}

/* Measure the tallest point of one character, in font units. */
static u8 measure_top(u16 code, s32 *top) {
    u16 index;

    if (decode_character(code) == 0u || WORK->point_count == 0u) {
        return 0u;
    }
    *top = WORK->point_y[0];
    for (index = 1u; index < WORK->point_count; ++index) {
        if (WORK->point_y[index] > *top) {
            *top = WORK->point_y[index];
        }
    }
    return 1u;
}

/* Measure the lowest point of one character, in font units. */
static u8 measure_bottom(u16 code, s32 *bottom) {
    u16 index;

    if (decode_character(code) == 0u || WORK->point_count == 0u) {
        return 0u;
    }
    *bottom = WORK->point_y[0];
    for (index = 1u; index < WORK->point_count; ++index) {
        if (WORK->point_y[index] < *bottom) {
            *bottom = WORK->point_y[index];
        }
    }
    return 1u;
}

/* Choose the font-units -> pixel-grid scale from real glyphs, the way
 * classic 8x8 fonts are drawn: capitals fill rows 0..6, the baseline
 * sits on the row 6/7 boundary and descenders dip into row 7. */
static void choose_scale(void) {
    static const char cap_samples[] = "HEZAMX0";
    static const char descender_samples[] = "gpqyj";
    s32 cap_top = 0;
    s32 descender = 0;
    s32 span;
    u16 index;

    for (index = 0u; cap_samples[index] != '\0'; ++index) {
        if (measure_top((u16)(u8)cap_samples[index], &cap_top) != 0u &&
            cap_top > (s32)units_per_em / 8) {
            break;
        }
        cap_top = 0;
    }
    if (cap_top == 0) {
        cap_top = em_ascent; /* caps-less font: use the design ascent */
    }
    for (index = 0u; descender_samples[index] != '\0'; ++index) {
        s32 bottom = 0;
        if (measure_bottom((u16)(u8)descender_samples[index], &bottom) != 0u &&
            bottom < descender) {
            descender = bottom;
        }
    }
    if (descender < -(s32)units_per_em) {
        descender = -(s32)units_per_em;
    }

    /* Capitals should span 7 pixels -- unless the descender would then
     * fall off the cell, in which case everything shrinks a little. */
    scale_y_num = 7 * SAMPLES_PER_PIXEL << FP_SHIFT;
    scale_y_den = cap_top;
    span = cap_top - descender;
    if (span * scale_y_num > GRID_FP * scale_y_den) {
        scale_y_num = GRID_FP;
        scale_y_den = span;
    }
    glyph_baseline = (cap_top * scale_y_num) / scale_y_den;
}

/* Rasterize one character into rows[8]. Failures leave it blank. */
static void rasterize_character(u16 code, u8 *rows) {
    s32 min_x, max_x, width_fp;
    u16 index;
    u8 coverage[64];

    for (index = 0u; index < 8u; ++index) {
        rows[index] = 0u;
    }
    if (decode_character(code) == 0u || WORK->point_count == 0u) {
        return; /* unmapped or empty: stays blank */
    }

    /* Center the glyph horizontally, and squeeze letters wider than
     * seven pixels so neighbouring characters never touch. */
    min_x = WORK->point_x[0];
    max_x = WORK->point_x[0];
    for (index = 1u; index < WORK->point_count; ++index) {
        if (WORK->point_x[index] < min_x) {
            min_x = WORK->point_x[index];
        }
        if (WORK->point_x[index] > max_x) {
            max_x = WORK->point_x[index];
        }
    }
    scale_x_num = scale_y_num;
    scale_x_den = scale_y_den;
    width_fp = ((max_x - min_x) * scale_x_num) / scale_x_den;
    if (width_fp > (7 * SAMPLES_PER_PIXEL << FP_SHIFT)) {
        scale_x_num = 7 * SAMPLES_PER_PIXEL << FP_SHIFT;
        scale_x_den = max_x - min_x;
        width_fp = 7 * SAMPLES_PER_PIXEL << FP_SHIFT;
    }
    glyph_min_x = min_x;
    glyph_x_shift = (GRID_FP - width_fp) / 2;

    flatten_outline();

    for (index = 0u; index < 64u; ++index) {
        coverage[index] = 0u;
    }
    count_coverage(coverage);
    coverage_to_rows(coverage, rows);
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

/* Parse the font in file[0..size) and rasterize ASCII 0x20..0x7E into
 * the internal glyph table. Returns 1 when the result looks like a
 * usable font, 0 when the caller should keep the BIOS font. */
u8 font_ttf_build(const u8 *file, u32 size) {
    u16 code;
    u32 ink = 0u;

    ttf = file;
    ttf_size = size;
    parse_error = 0u;

    for (code = 0u; code < 128u * 8u; ++code) {
        glyph_table[code] = 0u;
    }
    if (file == (const u8 *)0 || size < 12u) {
        return 0u;
    }
    if (find_tables() == 0u || parse_error != 0u) {
        return 0u;
    }
    if (read_header_tables() == 0u) {
        return 0u;
    }
    if (pick_cmap_subtable() == 0u || parse_error != 0u) {
        return 0u;
    }

    choose_scale();

    for (code = 0x20u; code <= 0x7Eu; ++code) {
        rasterize_character(code, &glyph_table[(u32)code * 8u]);
    }

    /* The same sanity rules gfx.c applies to the BIOS font: the space
     * stays empty and every capital letter carries some ink. */
    for (code = 0u; code < 8u; ++code) {
        if (glyph_table[(u32)' ' * 8u + code] != 0u) {
            return 0u;
        }
    }
    for (code = (u16)'A'; code <= (u16)'Z'; ++code) {
        u8 any = 0u;
        u16 row;
        for (row = 0u; row < 8u; ++row) {
            any = (u8)(any | glyph_table[(u32)code * 8u + row]);
        }
        if (any == 0u) {
            return 0u;
        }
    }
    for (code = 0u; code < 128u * 8u; ++code) {
        u8 bits = glyph_table[code];
        while (bits != 0u) {
            ink += (u32)(bits & 1u);
            bits = (u8)(bits >> 1);
        }
    }
    return (ink > 400u && ink < 24000u) ? 1u : 0u;
}

const u8 *font_ttf_table(void) {
    return glyph_table;
}

#ifndef DIMOS_HOST_TEST
/* Find FONT.TTF on the boot volume, copy it into the fixed buffer and
 * build the glyph table from it. Called once by gfx_init(). */
u8 font_ttf_load(void) {
    const u16 entry = file_system_find("FONT.TTF");
    u32 size;
    u32 copied;

    if (entry == FILE_NOT_FOUND) {
        return 0u;
    }
    size = file_system_size(entry);
    if (size < 12u || size > FONT_FILE_MAX_BYTES) {
        return 0u;
    }
    copied = file_system_read(entry, 0u, (void *)FONT_FILE_ADDRESS, size);
    if (copied != size) {
        return 0u; /* the file reaches past the preloaded disk window */
    }
    return font_ttf_build((const u8 *)FONT_FILE_ADDRESS, size);
}
#endif
