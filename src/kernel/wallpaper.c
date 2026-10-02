/*
 * wallpaper.c -- tiny native-resolution PNG reader.
 *
 * A general libpng/zlib port would be larger than the rest of this small OS.
 * DimOS therefore accepts a deliberately optimized, still standards-compliant
 * PNG profile: RGB8, non-interlaced, filter 0 and fixed-Huffman DEFLATE.  The
 * build-time generator emits exactly that profile.  Decoding is streaming:
 * only a 512-byte file cache and DEFLATE's required 32 KiB history exist in
 * kernel memory, while RGB565 pixels are written directly to the native-size
 * cache at 16 MiB.  No source or decoded scanline buffer is allocated.
 */

#include "dimos.h"

#define WALLPAPER_FILE "WALLPAPR.PNG"
#define WALLPAPER_MEMORY ((u16 *)0x01000000u)
#define INPUT_CACHE_BYTES 512u
#define DEFLATE_WINDOW_BYTES 32768u

static u8 input_cache[INPUT_CACHE_BYTES];
static u8 deflate_window[DEFLATE_WINDOW_BYTES];
static u16 wallpaper_width;
static u16 wallpaper_height;
static u8 wallpaper_loaded;

typedef struct {
    u16 file;
    u32 size;
    u32 position;
    u32 cache_start;
    u16 cache_length;
} PngInput;

typedef struct {
    PngInput *input;
    u32 remaining;
    u32 bits;
    u8 bit_count;
} BitInput;

typedef struct {
    u32 produced;
    u32 expected;
    u32 row_bytes;
    u32 row_position;
    u16 x;
    u16 y;
    u8 red;
    u8 green;
} PixelOutput;

static u8 input_byte(PngInput *input, u8 *value) {
    if (input->position >= input->size) {
        return 0u;
    }
    if (input->position < input->cache_start ||
        input->position >= input->cache_start + input->cache_length) {
        u32 wanted = input->size - input->position;
        if (wanted > INPUT_CACHE_BYTES) {
            wanted = INPUT_CACHE_BYTES;
        }
        input->cache_start = input->position;
        input->cache_length = (u16)file_system_read(input->file, input->position,
                                                    input_cache, wanted);
        if (input->cache_length == 0u) {
            return 0u;
        }
    }
    *value = input_cache[input->position - input->cache_start];
    ++input->position;
    return 1u;
}

static u8 input_skip(PngInput *input, u32 count) {
    if (count > input->size - input->position) {
        return 0u;
    }
    input->position += count;
    return 1u;
}

static u8 input_u32(PngInput *input, u32 *value) {
    u8 a;
    u8 b;
    u8 c;
    u8 d;
    if (input_byte(input, &a) == 0u || input_byte(input, &b) == 0u ||
        input_byte(input, &c) == 0u || input_byte(input, &d) == 0u) {
        return 0u;
    }
    *value = ((u32)a << 24u) | ((u32)b << 16u) | ((u32)c << 8u) | d;
    return 1u;
}

static u8 bits_read(BitInput *bits, u8 count, u32 *value) {
    while (bits->bit_count < count) {
        u8 next;
        if (bits->remaining == 0u || input_byte(bits->input, &next) == 0u) {
            return 0u;
        }
        bits->bits |= (u32)next << bits->bit_count;
        bits->bit_count = (u8)(bits->bit_count + 8u);
        --bits->remaining;
    }
    *value = bits->bits & ((1u << count) - 1u);
    bits->bits >>= count;
    bits->bit_count = (u8)(bits->bit_count - count);
    return 1u;
}

static u16 reverse_bits(u32 value, u8 count) {
    u16 reversed = 0u;
    while (count != 0u) {
        reversed = (u16)((reversed << 1u) | (u16)(value & 1u));
        value >>= 1u;
        --count;
    }
    return reversed;
}

/* Decode RFC 1951's fixed literal/length tree without storing a tree. */
static u8 fixed_symbol(BitInput *bits, u16 *symbol) {
    u32 code;
    u32 extra;
    u16 canonical;

    if (bits_read(bits, 7u, &code) == 0u) {
        return 0u;
    }
    canonical = reverse_bits(code, 7u);
    if (canonical <= 23u) {
        *symbol = (u16)(256u + canonical);
        return 1u;
    }
    if (bits_read(bits, 1u, &extra) == 0u) {
        return 0u;
    }
    code |= extra << 7u;
    canonical = reverse_bits(code, 8u);
    if (canonical >= 48u && canonical <= 191u) {
        *symbol = (u16)(canonical - 48u);
        return 1u;
    }
    if (canonical >= 192u && canonical <= 199u) {
        *symbol = (u16)(280u + canonical - 192u);
        return 1u;
    }
    if (bits_read(bits, 1u, &extra) == 0u) {
        return 0u;
    }
    code |= extra << 8u;
    canonical = reverse_bits(code, 9u);
    if (canonical >= 400u && canonical <= 511u) {
        *symbol = (u16)(144u + canonical - 400u);
        return 1u;
    }
    return 0u;
}

static u8 output_byte(PixelOutput *output, u8 value) {
    const u32 component = output->row_position;

    if (output->produced >= output->expected) {
        return 0u;
    }
    deflate_window[output->produced & (DEFLATE_WINDOW_BYTES - 1u)] = value;
    ++output->produced;

    if (component == 0u) {
        if (value != 0u) { /* only PNG filter None is needed */
            return 0u;
        }
    } else if ((component - 1u) % 3u == 0u) {
        output->red = value;
    } else if ((component - 1u) % 3u == 1u) {
        output->green = value;
    } else {
        WALLPAPER_MEMORY[(u32)output->y * wallpaper_width + output->x] =
            (u16)(((u16)(output->red >> 3u) << 11u) |
                  ((u16)(output->green >> 2u) << 5u) | (u16)(value >> 3u));
        ++output->x;
    }

    ++output->row_position;
    if (output->row_position == output->row_bytes) {
        output->row_position = 0u;
        output->x = 0u;
        ++output->y;
    }
    return 1u;
}

static u8 inflate_fixed(BitInput *bits, PixelOutput *output) {
    static const u16 length_base[29] = {
        3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 15u, 17u, 19u,
        23u, 27u, 31u, 35u, 43u, 51u, 59u, 67u, 83u, 99u, 115u, 131u,
        163u, 195u, 227u, 258u
    };
    static const u8 length_extra[29] = {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u,
        2u, 2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u
    };
    static const u16 distance_base[30] = {
        1u, 2u, 3u, 4u, 5u, 7u, 9u, 13u, 17u, 25u, 33u, 49u, 65u, 97u,
        129u, 193u, 257u, 385u, 513u, 769u, 1025u, 1537u, 2049u, 3073u,
        4097u, 6145u, 8193u, 12289u, 16385u, 24577u
    };
    static const u8 distance_extra[30] = {
        0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u,
        6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u
    };
    u8 final = 0u;

    while (final == 0u) {
        u32 value;
        u32 block_type;
        if (bits_read(bits, 1u, &value) == 0u) {
            return 0u;
        }
        final = (u8)value;
        if (bits_read(bits, 2u, &block_type) == 0u || block_type != 1u) {
            return 0u; /* generator always selects fixed Huffman blocks */
        }
        for (;;) {
            u16 symbol;
            if (fixed_symbol(bits, &symbol) == 0u) {
                return 0u;
            }
            if (symbol < 256u) {
                if (output_byte(output, (u8)symbol) == 0u) {
                    return 0u;
                }
            } else if (symbol == 256u) {
                break;
            } else {
                u16 length_index;
                u16 length;
                u16 distance_symbol;
                u16 distance;
                u32 extra;
                u16 index;

                if (symbol < 257u || symbol > 285u) {
                    return 0u;
                }
                length_index = (u16)(symbol - 257u);
                length = length_base[length_index];
                if (length_extra[length_index] != 0u) {
                    if (bits_read(bits, length_extra[length_index], &extra) == 0u) {
                        return 0u;
                    }
                    length = (u16)(length + extra);
                }
                if (bits_read(bits, 5u, &extra) == 0u) {
                    return 0u;
                }
                distance_symbol = reverse_bits(extra, 5u);
                if (distance_symbol >= 30u) {
                    return 0u;
                }
                distance = distance_base[distance_symbol];
                if (distance_extra[distance_symbol] != 0u) {
                    if (bits_read(bits, distance_extra[distance_symbol], &extra) == 0u) {
                        return 0u;
                    }
                    distance = (u16)(distance + extra);
                }
                if ((u32)distance > output->produced || distance > DEFLATE_WINDOW_BYTES) {
                    return 0u;
                }
                for (index = 0u; index < length; ++index) {
                    const u8 copied = deflate_window[
                        (output->produced - distance) & (DEFLATE_WINDOW_BYTES - 1u)];
                    if (output_byte(output, copied) == 0u) {
                        return 0u;
                    }
                }
            }
        }
    }
    return (u8)(output->produced == output->expected &&
                output->y == wallpaper_height);
}

u8 wallpaper_load(void) {
    static const u8 signature[8] = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    const u16 file = file_system_find(WALLPAPER_FILE);
    PngInput input;
    u8 index;
    u8 header[13];
    u32 length;
    u32 type;

    wallpaper_loaded = 0u;
    if (file == FILE_NOT_FOUND) {
        return 0u;
    }
    input.file = file;
    input.size = file_system_size(file);
    input.position = 0u;
    input.cache_start = input.size;
    input.cache_length = 0u;
    for (index = 0u; index < 8u; ++index) {
        u8 value;
        if (input_byte(&input, &value) == 0u || value != signature[index]) {
            return 0u;
        }
    }
    if (input_u32(&input, &length) == 0u || input_u32(&input, &type) == 0u ||
        length != 13u || type != 0x49484452u) { /* IHDR */
        return 0u;
    }
    for (index = 0u; index < 13u; ++index) {
        if (input_byte(&input, &header[index]) == 0u) {
            return 0u;
        }
    }
    wallpaper_width = (u16)(((u16)header[2] << 8u) | header[3]);
    wallpaper_height = (u16)(((u16)header[6] << 8u) | header[7]);
    if (header[0] != 0u || header[1] != 0u || header[4] != 0u || header[5] != 0u ||
        wallpaper_width != video_width || wallpaper_height != video_height ||
        header[8] != 8u || header[9] != 2u || header[10] != 0u ||
        header[11] != 0u || header[12] != 0u || input_skip(&input, 4u) == 0u) {
        return 0u;
    }

    if (input_u32(&input, &length) == 0u || input_u32(&input, &type) == 0u ||
        type != 0x49444154u || length < 6u) { /* one IDAT */
        return 0u;
    }
    {
        BitInput bits;
        PixelOutput output;
        u8 cmf;
        u8 flg;

        bits.input = &input;
        bits.remaining = length;
        bits.bits = 0u;
        bits.bit_count = 0u;
        if (input_byte(&input, &cmf) == 0u || input_byte(&input, &flg) == 0u ||
            cmf != 0x78u || (((u16)cmf << 8u) + flg) % 31u != 0u ||
            (flg & 0x20u) != 0u) {
            return 0u;
        }
        bits.remaining -= 2u;
        output.produced = 0u;
        output.row_bytes = (u32)wallpaper_width * 3u + 1u;
        output.expected = output.row_bytes * wallpaper_height;
        output.row_position = 0u;
        output.x = 0u;
        output.y = 0u;
        output.red = 0u;
        output.green = 0u;
        if (inflate_fixed(&bits, &output) == 0u) {
            return 0u;
        }
    }
    wallpaper_loaded = 1u;
    return 1u;
}

u8 wallpaper_ready(void) {
    return wallpaper_loaded;
}

u16 wallpaper_pixel(u16 x, u16 y) {
    if (wallpaper_loaded == 0u || x >= wallpaper_width || y >= wallpaper_height) {
        return 0u;
    }
    return WALLPAPER_MEMORY[(u32)y * wallpaper_width + x];
}
