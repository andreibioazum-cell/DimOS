/*
 * font_preview.c -- see how a TTF will look in DimOS without booting it.
 *
 * Compiles the kernel's own TrueType reader (src/kernel/font_ttf.c) as a
 * normal program, feeds it a font file and prints every ASCII glyph as
 * a native 16x16 coverage raster -- exactly what the VBE overlay draws.
 *
 * Build and run:
 *     gcc -std=c11 -O2 -Wall -Wextra -DDIMOS_HOST_TEST \
 *         -I src/kernel tools/font_preview.c src/kernel/font_ttf.c \
 *         -o bin/font-preview
 *     bin/font-preview fonts/font.ttf            # the whole set
 *     bin/font-preview fonts/font.ttf "Hello"    # just these letters
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;

/* The kernel side of the interface (see src/kernel/font_ttf.c). */
extern u8 *font_ttf_work_area;
u8 font_ttf_build(const u8 *file, u32 size);
const u8 *font_ttf_table(void);
const u8 *font_ttf_alpha_table(void);
const u8 *font_ttf_native_alpha_table(void);

/* DimOS does not draw letters as bare on/off pixels: every pixel carries
 * a coverage level from 0 to 16 and is blended into the background, which
 * is what makes small text look smooth instead of blocky. These five
 * characters stand for that range, from empty to solid ink. */
static char shade_character(u8 level) {
    static const char shades[] = ".:+*#";

    if (level > 16u) {
        level = 16u;
    }
    return shades[level / 4u];
}

static void print_row_of_glyphs(const u8 *levels, const char *text, int pixels) {
    size_t count = strlen(text);
    size_t item;
    int row;

    for (row = 0; row < pixels; ++row) {
        for (item = 0; item < count; ++item) {
            const u8 *glyph = levels + (size_t)(u8)text[item] *
                               (size_t)pixels * (size_t)pixels;
            int column;

            for (column = 0; column < pixels; ++column) {
                putchar(shade_character(glyph[(size_t)row * (size_t)pixels +
                                            (size_t)column]));
            }
            putchar(' ');
        }
        putchar('\n');
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "fonts/font.ttf";
    FILE *handle;
    long size;
    u8 *file;

    handle = fopen(path, "rb");
    if (handle == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(handle, 0, SEEK_END);
    size = ftell(handle);
    fseek(handle, 0, SEEK_SET);
    if (size <= 0 || size > 16L * 1024L * 1024L) {
        fprintf(stderr, "%s has a silly size\n", path);
        fclose(handle);
        return 1;
    }
    file = (u8 *)malloc((size_t)size);
    font_ttf_work_area = (u8 *)calloc(1, 256u * 1024u);
    if (file == NULL || font_ttf_work_area == NULL ||
        fread(file, 1, (size_t)size, handle) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        fclose(handle);
        return 1;
    }
    fclose(handle);

    if (font_ttf_build(file, (u32)size) == 0u) {
        fprintf(stderr,
                "%s was rejected -- DimOS would fall back to the BIOS font\n"
                "(the file must be a TrueType font with glyf outlines,\n"
                "not an OpenType/CFF .otf)\n",
                path);
        return 1;
    }

    if (argc > 2) {
        int argument;
        for (argument = 2; argument < argc; ++argument) {
            print_row_of_glyphs(font_ttf_native_alpha_table(), argv[argument], 16);
        }
    } else {
        static const char *lines[] = {
            "ABCDEFGH", "IJKLMNOP", "QRSTUVWX", "YZ",
            "abcdefgh", "ijklmnop", "qrstuvwx", "yz",
            "01234567", "89",
            "!\"#$%&'()", "*+,-./:;", "<=>?@[\\]", "^_`{|}~",
        };
        size_t line;
        for (line = 0; line < sizeof(lines) / sizeof(lines[0]); ++line) {
            print_row_of_glyphs(font_ttf_native_alpha_table(), lines[line], 16);
        }
    }
    printf("OK: DimOS will boot with this font.\n");
    return 0;
}
