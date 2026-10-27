/*
 * ui.c -- turning the composited frame into something a browser or a file
 * can show.
 *
 * The kernel draws into an XRGB8888 framebuffer, exactly the format a VBE
 * mode 32 bits per pixel would hand it. Two consumers read that buffer:
 *
 *   - the web front end compares it with the last frame it sent and packs
 *     the changed 16x16 tiles into one binary message. Tiles are the reason
 *     a phone on the sofa can show the shell without streaming a megabyte
 *     per frame: DimOS only repaints what really changed.
 *   - the test runs write still frames as PNG, so a screenshot in the
 *     repository is the output of the same code path the user sees.
 *
 * Both read the framebuffer directly. It lives for the whole process, and
 * gfx_show() leaves untouched pixels alone, so the runtime never has to
 * copy a frame at all.
 */

#include "dimos_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MAGIC 0x31464D44u /* "DMF1" in memory order */

static u32 *previous_frame;
static u32 previous_width;
static u32 previous_height;
static u32 sequence;

static u8 *payload;
static u32 payload_capacity;

static u16 tile_columns(u32 width) {
    return (u16)((width + HOST_TILE - 1u) / HOST_TILE);
}

static u16 tile_rows(u32 height) {
    return (u16)((height + HOST_TILE - 1u) / HOST_TILE);
}

static u8 payload_reserve(u32 bytes) {
    if (bytes <= payload_capacity) {
        return 1u;
    }
    {
        u8 *bigger = (u8 *)realloc(payload, bytes);

        if (bigger == (u8 *)0) {
            return 0u;
        }
        payload = bigger;
        payload_capacity = bytes;
    }
    return 1u;
}

static void put_u32(u8 *target, u32 value) {
    target[0] = (u8)(value & 0xFFu);
    target[1] = (u8)((value >> 8u) & 0xFFu);
    target[2] = (u8)((value >> 16u) & 0xFFu);
    target[3] = (u8)((value >> 24u) & 0xFFu);
}

static void put_u16(u8 *target, u16 value) {
    target[0] = (u8)(value & 0xFFu);
    target[1] = (u8)((value >> 8u) & 0xFFu);
}

u32 host_frame_sequence(void) {
    return sequence;
}

void host_frame_forget(void) {
    if (previous_frame != (u32 *)0) {
        memory_zero(previous_frame,
                    (u32)previous_width * previous_height * sizeof(u32));
    }
}

u8 host_frame_payload(u8 full, const u8 **buffer, u32 *length) {
    const u32 width = host_display_width();
    const u32 height = host_display_height();
    const u32 *frame = (const u32 *)host_framebuffer_pointer();
    const u16 columns = tile_columns(width);
    const u16 rows = tile_rows(height);
    u32 used = HOST_FRAME_HEADER_BYTES;
    u16 tile_count = 0u;
    u16 tile_y;
    u8 *header;

    if (frame == (const u32 *)0 || width == 0u || height == 0u) {
        return 0u;
    }
    if (previous_frame == (u32 *)0 || previous_width != width ||
        previous_height != height) {
        free(previous_frame);
        previous_frame = (u32 *)calloc((size_t)width * height, sizeof(u32));
        previous_width = width;
        previous_height = height;
        full = 1u;
        if (previous_frame == (u32 *)0) {
            return 0u;
        }
    }

    /* Worst case: every tile is dirty. */
    if (payload_reserve(HOST_FRAME_HEADER_BYTES +
                        (u32)columns * rows *
                            (8u + HOST_TILE * HOST_TILE * 4u)) == 0u) {
        return 0u;
    }

    for (tile_y = 0u; tile_y < rows; ++tile_y) {
        u16 tile_x;

        for (tile_x = 0u; tile_x < columns; ++tile_x) {
            const u32 x = (u32)tile_x * HOST_TILE;
            const u32 y = (u32)tile_y * HOST_TILE;
            const u32 tile_width = (x + HOST_TILE <= width) ? HOST_TILE
                                                            : width - x;
            const u32 tile_height = (y + HOST_TILE <= height) ? HOST_TILE
                                                             : height - y;
            u32 row;
            u8 changed = full;

            if (changed == 0u) {
                for (row = 0u; row < tile_height && changed == 0u; ++row) {
                    const u32 *source = frame + (y + row) * width + x;
                    const u32 *shadow = previous_frame + (y + row) * width + x;

                    if (memcmp(source, shadow, tile_width * sizeof(u32)) != 0) {
                        changed = 1u;
                    }
                }
            }
            if (changed == 0u) {
                continue;
            }
            if (used + 8u + tile_width * tile_height * 4u > payload_capacity) {
                break;
            }
            put_u16(payload + used, (u16)x);
            put_u16(payload + used + 2u, (u16)y);
            put_u16(payload + used + 4u, (u16)tile_width);
            put_u16(payload + used + 6u, (u16)tile_height);
            used += 8u;
            for (row = 0u; row < tile_height; ++row) {
                const u32 *source = frame + (y + row) * width + x;
                u32 *shadow = previous_frame + (y + row) * width + x;

                memcpy(payload + used, source, tile_width * sizeof(u32));
                memcpy(shadow, source, tile_width * sizeof(u32));
                used += tile_width * 4u;
            }
            ++tile_count;
        }
    }

    if (tile_count == 0u && full == 0u) {
        return 0u;
    }

    header = payload;
    put_u32(header, FRAME_MAGIC);
    put_u32(header + 4u, sequence);
    put_u16(header + 8u, tile_count);
    put_u16(header + 10u, (u16)width);
    put_u16(header + 12u, (u16)height);
    put_u16(header + 14u, 0u);
    ++sequence;

    *buffer = payload;
    *length = used;
    return 1u;
}

void host_copy_rgb(u8 *target) {
    const u32 width = host_display_width();
    const u32 height = host_display_height();
    const u32 *frame = (const u32 *)host_framebuffer_pointer();
    u32 index;

    for (index = 0u; index < width * height; ++index) {
        const u32 pixel = frame[index];

        target[index * 3u] = (u8)((pixel >> 16u) & 0xFFu);
        target[index * 3u + 1u] = (u8)((pixel >> 8u) & 0xFFu);
        target[index * 3u + 2u] = (u8)(pixel & 0xFFu);
    }
}

/* ------------------------------------------------------------------ */
/* PNG output                                                          */
/* ------------------------------------------------------------------ */

/* A deliberately small encoder: no compression, which keeps this file
 * dependency-free and still produces a perfectly valid PNG. A 844x390
 * screenshot is about a megabyte; the test runs keep a handful of them. */

static u32 crc_table[256];
static u8 crc_ready;

static void crc_build(void) {
    u32 index;

    for (index = 0u; index < 256u; ++index) {
        u32 value = index;
        u8 bit;

        for (bit = 0u; bit < 8u; ++bit) {
            value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1u))
                                       : (value >> 1u);
        }
        crc_table[index] = value;
    }
    crc_ready = 1u;
}

static u32 crc_update(u32 value, const u8 *data, u32 length) {
    u32 index;

    if (crc_ready == 0u) {
        crc_build();
    }
    for (index = 0u; index < length; ++index) {
        value = crc_table[(value ^ data[index]) & 0xFFu] ^ (value >> 8u);
    }
    return value;
}

static void write_u32_be(u8 *target, u32 value) {
    target[0] = (u8)(value >> 24u);
    target[1] = (u8)((value >> 16u) & 0xFFu);
    target[2] = (u8)((value >> 8u) & 0xFFu);
    target[3] = (u8)(value & 0xFFu);
}

static u8 chunk_write(FILE *handle, const char *type, const u8 *data,
                      u32 length) {
    u8 header[8];
    u8 trailer[4];
    u32 crc = 0xFFFFFFFFu;

    write_u32_be(header, length);
    memcpy(header + 4u, type, 4u);
    if (fwrite(header, 1u, 8u, handle) != 8u) {
        return 0u;
    }
    if (length != 0u && fwrite(data, 1u, length, handle) != length) {
        return 0u;
    }
    crc = crc_update(crc, header + 4u, 4u);
    if (length != 0u) {
        crc = crc_update(crc, data, length);
    }
    write_u32_be(trailer, crc ^ 0xFFFFFFFFu);
    return (u8)(fwrite(trailer, 1u, 4u, handle) == 4u);
}

static u32 adler32_bytes(const u8 *data, u32 length) {
    u32 a = 1u;
    u32 b = 0u;
    u32 index;

    for (index = 0u; index < length; ++index) {
        a = (a + data[index]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16u) | a;
}

u8 host_write_png(const char *path) {
    const u32 width = host_display_width();
    const u32 height = host_display_height();
    const u32 stride = width * 3u;
    const u32 raw_length = height * (stride + 1u);
    u8 *rgb = (u8 *)malloc((size_t)width * height * 3u);
    u8 *raw = (u8 *)malloc(raw_length);
    u8 *zlib = (u8 *)malloc(raw_length + (raw_length / 65535u + 1u) * 5u + 6u);
    FILE *handle;
    u32 zlib_length = 0u;
    u32 position = 0u;
    u32 row;
    u8 header[13];
    u8 ok = 1u;

    if (rgb == (u8 *)0 || raw == (u8 *)0 || zlib == (u8 *)0) {
        free(rgb);
        free(raw);
        free(zlib);
        return 0u;
    }
    host_copy_rgb(rgb);

    for (row = 0u; row < height; ++row) {
        raw[row * (stride + 1u)] = 0u; /* filter: none */
        memcpy(raw + row * (stride + 1u) + 1u, rgb + row * stride, stride);
    }

    /* zlib stream with stored ("uncompressed") deflate blocks. */
    zlib[zlib_length++] = 0x78u;
    zlib[zlib_length++] = 0x01u;
    while (position < raw_length) {
        u32 step = raw_length - position;
        const u8 final = (u8)(step <= 65535u ? 1u : 0u);

        if (step > 65535u) {
            step = 65535u;
        }
        zlib[zlib_length++] = final;
        zlib[zlib_length++] = (u8)(step & 0xFFu);
        zlib[zlib_length++] = (u8)((step >> 8u) & 0xFFu);
        zlib[zlib_length++] = (u8)(~step & 0xFFu);
        zlib[zlib_length++] = (u8)((~step >> 8u) & 0xFFu);
        memcpy(zlib + zlib_length, raw + position, step);
        zlib_length += step;
        position += step;
    }
    write_u32_be(zlib + zlib_length, adler32_bytes(raw, raw_length));
    zlib_length += 4u;

    handle = fopen(path, "wb");
    if (handle == (FILE *)0) {
        free(rgb);
        free(raw);
        free(zlib);
        return 0u;
    }
    {
        static const u8 signature[8] = { 0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au,
                                         0x1Au, 0x0Au };

        ok = (u8)(fwrite(signature, 1u, 8u, handle) == 8u);
    }
    write_u32_be(header, width);
    write_u32_be(header + 4u, height);
    header[8] = 8u;  /* bit depth   */
    header[9] = 2u;  /* truecolour  */
    header[10] = 0u; /* deflate     */
    header[11] = 0u; /* no filter   */
    header[12] = 0u; /* no interlace*/
    if (ok != 0u) {
        ok = chunk_write(handle, "IHDR", header, 13u);
    }
    if (ok != 0u) {
        ok = chunk_write(handle, "IDAT", zlib, zlib_length);
    }
    if (ok != 0u) {
        ok = chunk_write(handle, "IEND", (const u8 *)0, 0u);
    }
    fclose(handle);
    free(rgb);
    free(raw);
    free(zlib);
    return ok;
}
