/*
 * DimOS workshop kernel.
 *
 * VGA mode 13h desktop: wooden cubes and warm pillows you can drag
 * with the arrow keys. Enter unfolds a cube into a window (terminal,
 * snake, files, about). Hardware I/O stays in kernel.asm.
 */

#include "font8.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef short i16;

extern u8 io_in8(u16 port);
extern void io_out8(u16 port, u8 value);

#define FB ((volatile u8 *)0xA0000u)
#define SW 320
#define SH 200

#define KEY_NONE 0u
#define KEY_UP 0x100u
#define KEY_RIGHT 0x101u
#define KEY_DOWN 0x102u
#define KEY_LEFT 0x103u
#define KEY_TAB 0x09u

#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u

#define PIT_CHANNEL_0 0x40u
#define PIT_COMMAND 0x43u
#define PIT_DIVISOR 11932u

#define FAT_ROOT ((volatile u8 *)0x10000u)
#define FAT_TABLE ((volatile u8 *)0x07E00u)
#define FILE_DATA ((volatile u8 *)0x30000u)
#define FAT_ROOT_ENTRIES 224u
#define FAT_FILE_LIMIT 32768u

#define APP_NONE 0u
#define APP_TERM 1u
#define APP_SNAKE 2u
#define APP_FILES 3u
#define APP_ABOUT 4u

#define KIND_CUBE 0u
#define KIND_PILLOW 1u

#define OBJ_COUNT 8u
#define TERM_COLS 36u
#define TERM_ROWS 14u
#define CMD_CAP 31u

#define SNAKE_MAX 128u
#define SNAKE_CW 8u
#define SNAKE_CH 8u
#define SNAKE_COLS 28u
#define SNAKE_ROWS 14u

#define DIR_UP 0u
#define DIR_RIGHT 1u
#define DIR_DOWN 2u
#define DIR_LEFT 3u

/* Warm workshop palette indices. */
#define C_NIGHT 0u
#define C_FLOOR 1u
#define C_WOOD 2u
#define C_SHAVE 3u
#define C_CREAM 4u
#define C_LINEN 5u
#define C_CLAY 6u
#define C_ROSE 7u
#define C_BLUSH 8u
#define C_AMBER 9u
#define C_MOSS 10u
#define C_INK 11u
#define C_BAR 12u
#define C_TEAL 13u
#define C_WHITE 14u
#define C_DUSK 15u

struct object {
    i16 x;
    i16 y;
    u8 kind;
    u8 app;
    u8 size;
    const char *name;
};

static u8 keyboard_modifiers;
static u8 keyboard_extended;
static u8 keyboard_pause_bytes;
static u8 keyboard_caps_lock;

static u16 pit_last;
static u32 pit_acc;
static u8 bounce;

static struct object objects[OBJ_COUNT];
static u8 selected;
static u8 app_open;

static char term_lines[TERM_ROWS][TERM_COLS + 1u];
static u8 term_row;
static u8 term_col;
static char cmd_buf[CMD_CAP + 1u];
static u8 cmd_len;
static u8 file_deleted[FAT_ROOT_ENTRIES];

static u16 snake_cells[SNAKE_MAX];
static u16 snake_len;
static u16 snake_score;
static u8 snake_dir;
static u8 snake_dead;
static u16 snake_food;
static u32 rng = 0xD14E05u;

static u8 heap[2048];
static u16 heap_used;

static void dac(u8 index, u8 r, u8 g, u8 b) {
    io_out8(0x3C8u, index);
    io_out8(0x3C9u, r);
    io_out8(0x3C9u, g);
    io_out8(0x3C9u, b);
}

static void palette_warm(void) {
    dac(C_NIGHT, 8, 6, 4);
    dac(C_FLOOR, 18, 12, 8);
    dac(C_WOOD, 28, 18, 10);
    dac(C_SHAVE, 42, 28, 14);
    dac(C_CREAM, 52, 40, 24);
    dac(C_LINEN, 58, 50, 38);
    dac(C_CLAY, 48, 22, 16);
    dac(C_ROSE, 54, 30, 26);
    dac(C_BLUSH, 60, 46, 40);
    dac(C_AMBER, 56, 40, 12);
    dac(C_MOSS, 22, 28, 16);
    dac(C_INK, 10, 8, 6);
    dac(C_BAR, 36, 22, 12);
    dac(C_TEAL, 18, 32, 28);
    dac(C_WHITE, 62, 58, 48);
    dac(C_DUSK, 12, 18, 24);
}

static void pixel(int x, int y, u8 color) {
    if (x >= 0 && x < SW && y >= 0 && y < SH) {
        FB[(u16)y * SW + (u16)x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, u8 color) {
    int row;
    int col;
    for (row = 0; row < h; ++row) {
        const int yy = y + row;
        if (yy < 0 || yy >= SH) {
            continue;
        }
        for (col = 0; col < w; ++col) {
            const int xx = x + col;
            if (xx >= 0 && xx < SW) {
                FB[(u16)yy * SW + (u16)xx] = color;
            }
        }
    }
}

static void hline(int x0, int x1, int y, u8 color) {
    int x;
    if (x0 > x1) {
        const int t = x0;
        x0 = x1;
        x1 = t;
    }
    for (x = x0; x <= x1; ++x) {
        pixel(x, y, color);
    }
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color) {
    int miny = y0;
    int maxy = y0;
    int y;
    if (y1 < miny) miny = y1;
    if (y2 < miny) miny = y2;
    if (y1 > maxy) maxy = y1;
    if (y2 > maxy) maxy = y2;
    if (miny < 0) miny = 0;
    if (maxy >= SH) maxy = SH - 1;
    for (y = miny; y <= maxy; ++y) {
        int nodes[3];
        int n = 0;
        int i;
        int xs[3];
        int ys[3];
        xs[0] = x0; ys[0] = y0;
        xs[1] = x1; ys[1] = y1;
        xs[2] = x2; ys[2] = y2;
        for (i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            if ((ys[i] < y && ys[j] >= y) || (ys[j] < y && ys[i] >= y)) {
                const int dy = ys[j] - ys[i];
                if (dy != 0) {
                    nodes[n++] = xs[i] + (xs[j] - xs[i]) * (y - ys[i]) / dy;
                }
            }
        }
        if (n >= 2) {
            hline(nodes[0], nodes[1], y, color);
        }
    }
}

static void fill_circle(int cx, int cy, int rx, int ry, u8 base, u8 hi) {
    int y;
    for (y = -ry; y <= ry; ++y) {
        int x;
        for (x = -rx; x <= rx; ++x) {
            const int nx = (x * 16) / (rx == 0 ? 1 : rx);
            const int ny = (y * 16) / (ry == 0 ? 1 : ry);
            if (nx * nx + ny * ny <= 16 * 16) {
                const int lx = nx + 6;
                const int ly = ny + 8;
                const int d = lx * lx + ly * ly;
                pixel(cx + x, cy + y, d < 140 ? hi : base);
            }
        }
    }
}

static void draw_char(int x, int y, char ch, u8 fg, u8 bg, u8 opaque) {
    const u8 *glyph;
    int row;
    u8 index;
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    index = (u8)(ch - 32);
    glyph = font8[index];
    for (row = 0; row < 8; ++row) {
        u8 bits = glyph[row];
        int col;
        for (col = 0; col < 8; ++col) {
            if ((bits & 0x80u) != 0u) {
                pixel(x + col, y + row, fg);
            } else if (opaque != 0u) {
                pixel(x + col, y + row, bg);
            }
            bits = (u8)(bits << 1);
        }
    }
}

static void draw_text(int x, int y, const char *text, u8 fg, u8 bg, u8 opaque) {
    while (*text != '\0') {
        draw_char(x, y, *text, fg, bg, opaque);
        x += 8;
        ++text;
    }
}

static void draw_cube(int cx, int cy, int s, u8 side, u8 top, u8 dark) {
    const int half = s / 2;
    const int lift = s / 2;
    const int x = cx;
    const int y = cy;
    fill_tri(x, y - s, x - s, y - lift, x, y, top);
    fill_tri(x, y - s, x + s, y - lift, x, y, top);
    fill_tri(x - s, y - lift, x, y, x - s, y + half, side);
    fill_tri(x, y, x - s, y + half, x, y + s - lift, side);
    fill_tri(x + s, y - lift, x, y, x + s, y + half, dark);
    fill_tri(x, y, x + s, y + half, x, y + s - lift, dark);
}

static char keyboard_ascii(u8 scan_code) {
    const u8 shifted = (u8)(keyboard_modifiers != 0u);
    char character = '\0';

    switch (scan_code) {
        case 0x01u: return (char)27;
        case 0x02u: return shifted != 0u ? '!' : '1';
        case 0x03u: return shifted != 0u ? '@' : '2';
        case 0x04u: return shifted != 0u ? '#' : '3';
        case 0x05u: return shifted != 0u ? '$' : '4';
        case 0x06u: return shifted != 0u ? '%' : '5';
        case 0x07u: return shifted != 0u ? '^' : '6';
        case 0x08u: return shifted != 0u ? '&' : '7';
        case 0x09u: return shifted != 0u ? '*' : '8';
        case 0x0Au: return shifted != 0u ? '(' : '9';
        case 0x0Bu: return shifted != 0u ? ')' : '0';
        case 0x0Cu: return shifted != 0u ? '_' : '-';
        case 0x0Du: return shifted != 0u ? '+' : '=';
        case 0x0Eu: return '\b';
        case 0x0Fu: return '\t';
        case 0x10u: character = 'q'; break;
        case 0x11u: character = 'w'; break;
        case 0x12u: character = 'e'; break;
        case 0x13u: character = 'r'; break;
        case 0x14u: character = 't'; break;
        case 0x15u: character = 'y'; break;
        case 0x16u: character = 'u'; break;
        case 0x17u: character = 'i'; break;
        case 0x18u: character = 'o'; break;
        case 0x19u: character = 'p'; break;
        case 0x1Au: return shifted != 0u ? '{' : '[';
        case 0x1Bu: return shifted != 0u ? '}' : ']';
        case 0x1Cu: return '\n';
        case 0x1Eu: character = 'a'; break;
        case 0x1Fu: character = 's'; break;
        case 0x20u: character = 'd'; break;
        case 0x21u: character = 'f'; break;
        case 0x22u: character = 'g'; break;
        case 0x23u: character = 'h'; break;
        case 0x24u: character = 'j'; break;
        case 0x25u: character = 'k'; break;
        case 0x26u: character = 'l'; break;
        case 0x27u: return shifted != 0u ? ':' : ';';
        case 0x28u: return shifted != 0u ? '"' : '\'';
        case 0x29u: return shifted != 0u ? '~' : '`';
        case 0x2Bu: return shifted != 0u ? '|' : '\\';
        case 0x2Cu: character = 'z'; break;
        case 0x2Du: character = 'x'; break;
        case 0x2Eu: character = 'c'; break;
        case 0x2Fu: character = 'v'; break;
        case 0x30u: character = 'b'; break;
        case 0x31u: character = 'n'; break;
        case 0x32u: character = 'm'; break;
        case 0x33u: return shifted != 0u ? '<' : ',';
        case 0x34u: return shifted != 0u ? '>' : '.';
        case 0x35u: return shifted != 0u ? '?' : '/';
        case 0x37u: return '*';
        case 0x39u: return ' ';
        default: return '\0';
    }

    if ((u8)(shifted ^ keyboard_caps_lock) != 0u) {
        character = (char)(character - ('a' - 'A'));
    }
    return character;
}

static u16 keyboard_poll(void) {
    while ((io_in8(PS2_STATUS) & 0x01u) != 0u) {
        const u8 status = io_in8(PS2_STATUS);
        const u8 scan_code = io_in8(PS2_DATA);
        u8 base_code;
        char character;

        if ((status & 0x20u) != 0u) {
            continue;
        }
        if (keyboard_pause_bytes != 0u) {
            --keyboard_pause_bytes;
            continue;
        }
        if (scan_code == 0xE1u) {
            keyboard_pause_bytes = 5u;
            continue;
        }
        if (scan_code == 0xE0u) {
            keyboard_extended = 1u;
            continue;
        }

        base_code = (u8)(scan_code & 0x7Fu);
        if ((scan_code & 0x80u) != 0u) {
            if (base_code == 0x2Au) {
                keyboard_modifiers = (u8)(keyboard_modifiers & (u8)~0x01u);
            } else if (base_code == 0x36u) {
                keyboard_modifiers = (u8)(keyboard_modifiers & (u8)~0x02u);
            }
            keyboard_extended = 0u;
            continue;
        }

        if (keyboard_extended != 0u) {
            keyboard_extended = 0u;
            switch (base_code) {
                case 0x48u: return KEY_UP;
                case 0x4Du: return KEY_RIGHT;
                case 0x50u: return KEY_DOWN;
                case 0x4Bu: return KEY_LEFT;
                case 0x1Cu: return (u16)'\n';
                case 0x0Fu: return KEY_TAB;
                default: {
                    const char ext = keyboard_ascii(base_code);
                    if (ext != '\0') {
                        return (u16)(u8)ext;
                    }
                    continue;
                }
            }
        }

        if (base_code == 0x2Au) {
            keyboard_modifiers = (u8)(keyboard_modifiers | 0x01u);
            continue;
        }
        if (base_code == 0x36u) {
            keyboard_modifiers = (u8)(keyboard_modifiers | 0x02u);
            continue;
        }
        if (base_code == 0x3Au) {
            keyboard_caps_lock = (u8)(keyboard_caps_lock ^ 1u);
            continue;
        }

        character = keyboard_ascii(base_code);
        if (character != '\0') {
            return (u16)(u8)character;
        }
    }
    return KEY_NONE;
}

static void keyboard_initialize(void) {
    keyboard_modifiers = 0u;
    keyboard_extended = 0u;
    keyboard_pause_bytes = 0u;
    keyboard_caps_lock = 0u;
    while ((io_in8(PS2_STATUS) & 0x01u) != 0u) {
        (void)io_in8(PS2_DATA);
    }
}

static u16 pit_read(void) {
    u8 low;
    u8 high;
    io_out8(PIT_COMMAND, 0x00u);
    low = io_in8(PIT_CHANNEL_0);
    high = io_in8(PIT_CHANNEL_0);
    return (u16)((u16)low | ((u16)high << 8u));
}

static void pit_initialize(void) {
    io_out8(PIT_COMMAND, 0x34u);
    io_out8(PIT_CHANNEL_0, (u8)PIT_DIVISOR);
    io_out8(PIT_CHANNEL_0, (u8)(PIT_DIVISOR >> 8u));
    pit_last = pit_read();
    pit_acc = 0u;
}

static u8 pit_tick(u32 need) {
    const u16 current = pit_read();
    u16 elapsed;
    if (pit_last >= current) {
        elapsed = (u16)(pit_last - current);
    } else {
        elapsed = (u16)(pit_last + (PIT_DIVISOR - current));
    }
    pit_last = current;
    pit_acc += elapsed;
    if (pit_acc >= need) {
        pit_acc -= need;
        return 1u;
    }
    return 0u;
}

static u8 ascii_upper(u8 character) {
    if (character >= (u8)'a' && character <= (u8)'z') {
        return (u8)(character - (u8)('a' - 'A'));
    }
    return character;
}

static u8 strings_equal(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (ascii_upper((u8)*left) != ascii_upper((u8)*right)) {
            return 0u;
        }
        ++left;
        ++right;
    }
    return (u8)(*left == '\0' && *right == '\0');
}

static void *kalloc(u16 bytes) {
    u8 *block;
    if ((u32)heap_used + (u32)bytes > sizeof(heap)) {
        return 0;
    }
    block = heap + heap_used;
    heap_used = (u16)(heap_used + bytes);
    return block;
}

static u16 fat12_next(u16 cluster) {
    const u16 offset = (u16)(cluster + cluster / 2u);
    u16 value = (u16)FAT_TABLE[offset] | ((u16)FAT_TABLE[offset + 1u] << 8u);
    return (cluster & 1u) != 0u ? (u16)(value >> 4u) : (u16)(value & 0x0FFFu);
}

static u8 file_name_matches(const volatile u8 *entry, const char *name) {
    u8 pos = 0u;
    u8 i = 0u;
    while (name[i] != '\0' && i < 12u) {
        char character = name[i++];
        if (character == '.') {
            while (pos < 8u) {
                if (entry[pos++] != ' ') return 0u;
            }
            pos = 8u;
        } else {
            if (pos >= 11u || (pos == 8u && entry[pos] == ' ')) return 0u;
            if (ascii_upper((u8)character) != ascii_upper(entry[pos])) return 0u;
            ++pos;
        }
    }
    if (i == 0u) return 0u;
    while (pos < 11u) {
        if (entry[pos++] != ' ') return 0u;
    }
    return 1u;
}

static u16 file_find(const char *name) {
    u16 index;
    for (index = 0u; index < FAT_ROOT_ENTRIES; ++index) {
        const volatile u8 *entry = FAT_ROOT + index * 32u;
        if (entry[0] == 0x00u) break;
        if (entry[0] == 0xE5u || entry[11] == 0x0Fu ||
            (entry[11] & 0x08u) != 0u || file_deleted[index] != 0u) continue;
        if (file_name_matches(entry, name) != 0u) return index;
    }
    return 0xFFFFu;
}

static void term_clear(void) {
    u8 row;
    u8 col;
    for (row = 0u; row < TERM_ROWS; ++row) {
        for (col = 0u; col < TERM_COLS; ++col) {
            term_lines[row][col] = ' ';
        }
        term_lines[row][TERM_COLS] = '\0';
    }
    term_row = 0u;
    term_col = 0u;
}

static void term_scroll(void) {
    u8 row;
    u8 col;
    if (term_row < TERM_ROWS) {
        return;
    }
    for (row = 1u; row < TERM_ROWS; ++row) {
        for (col = 0u; col <= TERM_COLS; ++col) {
            term_lines[row - 1u][col] = term_lines[row][col];
        }
    }
    for (col = 0u; col < TERM_COLS; ++col) {
        term_lines[TERM_ROWS - 1u][col] = ' ';
    }
    term_lines[TERM_ROWS - 1u][TERM_COLS] = '\0';
    term_row = (u8)(TERM_ROWS - 1u);
}

static void term_putc(char ch) {
    if (ch == '\n') {
        term_col = 0u;
        ++term_row;
    } else if (ch == '\r') {
        term_col = 0u;
    } else if (ch == '\b') {
        if (term_col != 0u) {
            --term_col;
            term_lines[term_row][term_col] = ' ';
        }
    } else {
        term_lines[term_row][term_col] = ch;
        ++term_col;
        if (term_col >= TERM_COLS) {
            term_col = 0u;
            ++term_row;
        }
    }
    term_scroll();
}

static void term_write(const char *text) {
    while (*text != '\0') {
        term_putc(*text);
        ++text;
    }
}

static void term_write_u(u32 value) {
    char digits[10];
    u8 length = 0u;
    if (value == 0u) {
        term_putc('0');
        return;
    }
    while (value != 0u) {
        digits[length] = (char)('0' + (char)(value % 10u));
        value /= 10u;
        ++length;
    }
    while (length != 0u) {
        --length;
        term_putc(digits[length]);
    }
}

static void file_print_name(const volatile u8 *entry) {
    u8 index;
    for (index = 0u; index < 8u && entry[index] != ' '; ++index) {
        term_putc((char)entry[index]);
    }
    if (entry[8] != ' ') {
        term_putc('.');
        for (index = 8u; index < 11u && entry[index] != ' '; ++index) {
            term_putc((char)entry[index]);
        }
    }
}

static void file_manager_dir(void) {
    u16 index;
    term_write("Disk files (FAT12):\n");
    for (index = 0u; index < FAT_ROOT_ENTRIES; ++index) {
        const volatile u8 *entry = FAT_ROOT + index * 32u;
        if (entry[0] == 0x00u) break;
        if (entry[0] == 0xE5u || entry[11] == 0x0Fu ||
            (entry[11] & 0x08u) != 0u || file_deleted[index] != 0u) continue;
        file_print_name(entry);
        term_write("  ");
        term_write_u((u32)entry[28] | ((u32)entry[29] << 8u) |
                     ((u32)entry[30] << 16u) | ((u32)entry[31] << 24u));
        term_write(" bytes\n");
    }
}

static void file_type(const char *name) {
    const u16 index = file_find(name);
    u16 cluster;
    u32 remaining;
    if (index == 0xFFFFu) {
        term_write("File not found.\n");
        return;
    }
    cluster = (u16)FAT_ROOT[index * 32u + 26u] |
              ((u16)FAT_ROOT[index * 32u + 27u] << 8u);
    remaining = (u32)FAT_ROOT[index * 32u + 28u] |
                ((u32)FAT_ROOT[index * 32u + 29u] << 8u) |
                ((u32)FAT_ROOT[index * 32u + 30u] << 16u) |
                ((u32)FAT_ROOT[index * 32u + 31u] << 24u);
    if (remaining > FAT_FILE_LIMIT) remaining = FAT_FILE_LIMIT;
    while (remaining != 0u && cluster >= 2u && cluster < 0xFF8u) {
        const u32 offset = (u32)(cluster - 2u) * 512u;
        u32 count = remaining < 512u ? remaining : 512u;
        u32 position;
        for (position = 0u; position < count; ++position) {
            term_putc((char)FILE_DATA[offset + position]);
        }
        remaining -= count;
        cluster = fat12_next(cluster);
    }
    term_putc('\n');
}

static void file_delete(const char *name) {
    const u16 index = file_find(name);
    const volatile u8 *entry;
    if (index == 0xFFFFu) {
        term_write("File not found.\n");
        return;
    }
    entry = FAT_ROOT + index * 32u;
    if (file_name_matches(entry, "KERNEL.BIN") != 0u) {
        term_write("KERNEL.BIN is protected.\n");
        return;
    }
    file_deleted[index] = 1u;
    term_write("Deleted (session only): ");
    file_print_name(entry);
    term_write("\n");
}

static char *trim_command(char *command) {
    char *start = command;
    char *end;
    while (*start == ' ') ++start;
    end = start;
    while (*end != '\0') ++end;
    while (end != start && end[-1] == ' ') --end;
    *end = '\0';
    return start;
}

static char *command_argument(char *command) {
    while (*command != ' ' && *command != '\0') ++command;
    if (*command == '\0') return command;
    *command++ = '\0';
    while (*command == ' ') ++command;
    return command;
}

static void print_help(void) {
    term_write("Commands:\n");
    term_write("  DIR   list FAT12 files\n");
    term_write("  TYPE  read a file\n");
    term_write("  DEL   hide a user file\n");
    term_write("  SNAKE start the game\n");
    term_write("  MEM   heap usage\n");
    term_write("  ABOUT system info\n");
    term_write("  DESK  back to workshop\n");
    term_write("  CLEAR HELP\n");
}

static void print_about(void) {
    term_write("DimOS workshop 0.2\n");
    term_write("32-bit protected mode, VGA 13h\n");
    term_write("Cubes smell of shavings.\n");
    term_write("Pillows stay warm.\n");
    term_write("Arrows move, Enter opens.\n");
    term_write("Heap used: ");
    term_write_u(heap_used);
    term_write("/");
    term_write_u((u32)sizeof(heap));
    term_write("\n");
}

static u32 next_random(void) {
    u32 value = rng;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    if (value == 0u) value = 0xA341316Cu;
    rng = value;
    return value;
}

static u16 snake_index(u8 row, u8 col) {
    return (u16)((u16)row * SNAKE_COLS + col);
}

static u8 snake_occupies(u16 cell, u16 ignore_tail) {
    u16 i;
    for (i = 0u; i < snake_len; ++i) {
        if (i == ignore_tail) continue;
        if (snake_cells[i] == cell) return 1u;
    }
    return 0u;
}

static void snake_place_food(void) {
    u16 guard = 0u;
    do {
        const u8 row = (u8)(next_random() % SNAKE_ROWS);
        const u8 col = (u8)(next_random() % SNAKE_COLS);
        snake_food = snake_index(row, col);
        ++guard;
    } while (snake_occupies(snake_food, 0xFFFFu) != 0u && guard < 200u);
}

static void snake_reset(void) {
    u8 i;
    snake_len = 4u;
    snake_score = 0u;
    snake_dir = DIR_RIGHT;
    snake_dead = 0u;
    rng ^= (u32)pit_read() | 1u;
    for (i = 0u; i < snake_len; ++i) {
        snake_cells[i] = snake_index(7u, (u8)(10u - i));
    }
    snake_place_food();
}

static void snake_step(void) {
    u16 head = snake_cells[0];
    u8 row = (u8)(head / SNAKE_COLS);
    u8 col = (u8)(head % SNAKE_COLS);
    u16 i;
    u16 next;
    if (snake_dir == DIR_UP) {
        if (row == 0u) { snake_dead = 1u; return; }
        --row;
    } else if (snake_dir == DIR_DOWN) {
        if (row + 1u >= SNAKE_ROWS) { snake_dead = 1u; return; }
        ++row;
    } else if (snake_dir == DIR_LEFT) {
        if (col == 0u) { snake_dead = 1u; return; }
        --col;
    } else {
        if (col + 1u >= SNAKE_COLS) { snake_dead = 1u; return; }
        ++col;
    }
    next = snake_index(row, col);
    if (snake_occupies(next, (u16)(snake_len - 1u)) != 0u) {
        snake_dead = 1u;
        return;
    }
    if (next == snake_food) {
        if (snake_len < SNAKE_MAX) ++snake_len;
        snake_score = (u16)(snake_score + 10u);
        snake_place_food();
    }
    i = (u16)(snake_len - 1u);
    while (i != 0u) {
        snake_cells[i] = snake_cells[i - 1u];
        --i;
    }
    snake_cells[0] = next;
}

static void snake_turn(u16 key) {
    if ((key == (u16)'w' || key == (u16)'W' || key == KEY_UP) &&
        snake_dir != DIR_DOWN) snake_dir = DIR_UP;
    else if ((key == (u16)'d' || key == (u16)'D' || key == KEY_RIGHT) &&
             snake_dir != DIR_LEFT) snake_dir = DIR_RIGHT;
    else if ((key == (u16)'s' || key == (u16)'S' || key == KEY_DOWN) &&
             snake_dir != DIR_UP) snake_dir = DIR_DOWN;
    else if ((key == (u16)'a' || key == (u16)'A' || key == KEY_LEFT) &&
             snake_dir != DIR_RIGHT) snake_dir = DIR_LEFT;
}

static void run_command(char *line) {
    char *command = trim_command(line);
    char *argument = command_argument(command);
    if (*command == '\0') return;
    if (strings_equal(command, "DIR") != 0u ||
        strings_equal(command, "FILES") != 0u) {
        file_manager_dir();
    } else if (strings_equal(command, "TYPE") != 0u) {
        if (*argument == '\0') term_write("Usage: TYPE filename\n");
        else file_type(argument);
    } else if (strings_equal(command, "DEL") != 0u) {
        if (*argument == '\0') term_write("Usage: DEL filename\n");
        else file_delete(argument);
    } else if (strings_equal(command, "SNAKE") != 0u) {
        app_open = APP_SNAKE;
        snake_reset();
    } else if (strings_equal(command, "MEM") != 0u) {
        (void)kalloc(16u);
        term_write("Heap ");
        term_write_u(heap_used);
        term_write(" / ");
        term_write_u((u32)sizeof(heap));
        term_write(" bytes\n");
    } else if (strings_equal(command, "ABOUT") != 0u ||
               strings_equal(command, "VER") != 0u) {
        print_about();
    } else if (strings_equal(command, "DESK") != 0u ||
               strings_equal(command, "DESKTOP") != 0u) {
        app_open = APP_NONE;
    } else if (strings_equal(command, "CLEAR") != 0u ||
               strings_equal(command, "CLS") != 0u) {
        term_clear();
    } else if (strings_equal(command, "HELP") != 0u) {
        print_help();
    } else if (strings_equal(command, "ECHO") != 0u) {
        term_write(argument);
        term_putc('\n');
    } else {
        term_write("Unknown. Type HELP.\n");
    }
}

static void term_open(void) {
    term_clear();
    cmd_len = 0u;
    term_write("DimOS workshop terminal\n");
    term_write("Cubes became windows. Type HELP.\n\n> ");
}

static void term_key(u16 key) {
    if (key == 27u) {
        app_open = APP_NONE;
        return;
    }
    if (key == (u16)'\n') {
        cmd_buf[cmd_len] = '\0';
        term_putc('\n');
        run_command(cmd_buf);
        if (app_open == APP_TERM) {
            cmd_len = 0u;
            term_write("> ");
        }
        return;
    }
    if (key == (u16)'\b') {
        if (cmd_len != 0u) {
            --cmd_len;
            term_putc('\b');
        }
        return;
    }
    if (key >= 32u && key <= 126u && cmd_len < CMD_CAP) {
        cmd_buf[cmd_len] = (char)key;
        ++cmd_len;
        term_putc((char)key);
    }
}

static void objects_init(void) {
    objects[0].x = 58;  objects[0].y = 118; objects[0].kind = KIND_CUBE;
    objects[0].app = APP_NONE; objects[0].size = 22; objects[0].name = "block";
    objects[1].x = 118; objects[1].y = 132; objects[1].kind = KIND_CUBE;
    objects[1].app = APP_NONE; objects[1].size = 16; objects[1].name = "shaving";
    objects[2].x = 250; objects[2].y = 128; objects[2].kind = KIND_PILLOW;
    objects[2].app = APP_NONE; objects[2].size = 22; objects[2].name = "pillow";
    objects[3].x = 198; objects[3].y = 146; objects[3].kind = KIND_PILLOW;
    objects[3].app = APP_NONE; objects[3].size = 18; objects[3].name = "warm";
    objects[4].x = 86;  objects[4].y = 78;  objects[4].kind = KIND_CUBE;
    objects[4].app = APP_TERM; objects[4].size = 20; objects[4].name = "term";
    objects[5].x = 156; objects[5].y = 86;  objects[5].kind = KIND_CUBE;
    objects[5].app = APP_SNAKE; objects[5].size = 20; objects[5].name = "snake";
    objects[6].x = 220; objects[6].y = 80;  objects[6].kind = KIND_CUBE;
    objects[6].app = APP_FILES; objects[6].size = 18; objects[6].name = "files";
    objects[7].x = 40;  objects[7].y = 168; objects[7].kind = KIND_CUBE;
    objects[7].app = APP_ABOUT; objects[7].size = 16; objects[7].name = "about";
    selected = 4u;
}

static void clamp_object(struct object *obj) {
    if (obj->x < 20) obj->x = 20;
    if (obj->x > 300) obj->x = 300;
    if (obj->y < 50) obj->y = 50;
    if (obj->y > 176) obj->y = 176;
}

static void draw_workshop(void) {
    u8 order[OBJ_COUNT];
    u8 i;
    u8 j;
    fill_rect(0, 0, SW, 36, C_DUSK);
    fill_rect(0, 36, SW, 110, C_FLOOR);
    fill_rect(0, 146, SW, 54, C_WOOD);
    for (i = 0u; i < 20u; ++i) {
        fill_rect(0, 146 + (int)i * 3, SW, 1, (i & 1u) != 0u ? C_BAR : C_WOOD);
    }
    fill_rect(0, 0, SW, 18, C_BAR);
    draw_text(6, 5, "DimOS workshop", C_CREAM, C_BAR, 0u);
    draw_text(200, 5, "v0.2", C_SHAVE, C_BAR, 0u);
    draw_text(48, 22, "shavings in the air", C_SHAVE, C_DUSK, 0u);

    for (i = 0u; i < OBJ_COUNT; ++i) order[i] = i;
    for (i = 0u; i < OBJ_COUNT; ++i) {
        for (j = (u8)(i + 1u); j < OBJ_COUNT; ++j) {
            if (objects[order[j]].y < objects[order[i]].y) {
                const u8 t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    for (i = 0u; i < OBJ_COUNT; ++i) {
        const u8 id = order[i];
        struct object *obj = &objects[id];
        const int bob = (id == selected) ? ((bounce & 1u) != 0u ? -1 : 0) : 0;
        const int x = obj->x;
        const int y = obj->y + bob;
        if (obj->kind == KIND_CUBE) {
            u8 top = C_SHAVE;
            u8 side = C_WOOD;
            u8 dark = C_BAR;
            if (obj->app == APP_TERM) { top = C_TEAL; side = C_MOSS; }
            if (obj->app == APP_SNAKE) { top = C_AMBER; side = C_CLAY; }
            if (obj->app == APP_FILES) { top = C_CREAM; side = C_SHAVE; }
            if (obj->app == APP_ABOUT) { top = C_LINEN; side = C_WOOD; }
            draw_cube(x, y, obj->size, side, top, dark);
        } else {
            fill_circle(x, y, obj->size, (obj->size * 3) / 4, C_ROSE, C_BLUSH);
        }
        if (id == selected) {
            fill_rect(x - obj->size - 2, y + obj->size - 4, obj->size * 2 + 4, 2, C_AMBER);
        }
        draw_text(x - 20, y + obj->size - 2, obj->name, C_INK, C_WOOD, 0u);
    }

    fill_rect(0, 186, SW, 14, C_INK);
    draw_text(4, 188, "Tab next  Arrows move  Enter open  Esc desk", C_LINEN, C_INK, 0u);
}

static void draw_window_frame(const char *title) {
    fill_rect(16, 22, 288, 160, C_BAR);
    fill_rect(18, 24, 284, 156, C_LINEN);
    fill_rect(18, 24, 284, 14, C_WOOD);
    draw_text(24, 26, title, C_CREAM, C_WOOD, 0u);
    draw_text(270, 26, "x", C_CLAY, C_WOOD, 0u);
}

static void draw_terminal(void) {
    u8 row;
    draw_window_frame("Terminal");
    fill_rect(20, 40, 280, 118, C_NIGHT);
    for (row = 0u; row < TERM_ROWS; ++row) {
        draw_text(24, 42 + (int)row * 8, term_lines[row], C_CREAM, C_NIGHT, 0u);
    }
    fill_rect(24 + (int)term_col * 8, 42 + (int)term_row * 8, 8, 8, C_AMBER);
}

static void draw_files_app(void) {
    u16 index;
    int y = 44;
    draw_window_frame("Files");
    draw_text(24, y, "FAT12 floppy", C_INK, C_LINEN, 0u);
    y += 12;
    for (index = 0u; index < FAT_ROOT_ENTRIES && y < 168; ++index) {
        const volatile u8 *entry = FAT_ROOT + index * 32u;
        char name[13];
        u8 n = 0u;
        u8 k;
        if (entry[0] == 0x00u) break;
        if (entry[0] == 0xE5u || entry[11] == 0x0Fu ||
            (entry[11] & 0x08u) != 0u || file_deleted[index] != 0u) continue;
        for (k = 0u; k < 8u && entry[k] != ' '; ++k) name[n++] = (char)entry[k];
        if (entry[8] != ' ') {
            name[n++] = '.';
            for (k = 8u; k < 11u && entry[k] != ' '; ++k) name[n++] = (char)entry[k];
        }
        name[n] = '\0';
        fill_rect(24, y, 10, 8, C_SHAVE);
        draw_text(38, y, name, C_INK, C_LINEN, 0u);
        y += 10;
    }
    draw_text(24, 168, "Esc closes", C_BAR, C_LINEN, 0u);
}

static void draw_about_app(void) {
    draw_window_frame("About DimOS");
    draw_text(24, 46, "A living workshop OS.", C_INK, C_LINEN, 0u);
    draw_text(24, 58, "Cubes keep the smell", C_INK, C_LINEN, 0u);
    draw_text(24, 70, "of fresh shavings.", C_INK, C_LINEN, 0u);
    draw_text(24, 86, "Pillows stay warm.", C_ROSE, C_LINEN, 0u);
    draw_text(24, 102, "Protected mode + FAT12", C_BAR, C_LINEN, 0u);
    draw_text(24, 114, "Heap", C_INK, C_LINEN, 0u);
    draw_text(64, 114, "ready", C_MOSS, C_LINEN, 0u);
    draw_text(24, 130, "Next: fonts, net, more", C_BAR, C_LINEN, 0u);
    draw_text(24, 168, "Esc back to workshop", C_BAR, C_LINEN, 0u);
}

static void draw_snake_app(void) {
    const int ox = 28;
    const int oy = 44;
    u16 i;
    u8 r;
    u8 c;
    draw_window_frame("Snake");
    fill_rect(ox - 2, oy - 2, SNAKE_COLS * SNAKE_CW + 4,
              SNAKE_ROWS * SNAKE_CH + 4, C_INK);
    fill_rect(ox, oy, SNAKE_COLS * SNAKE_CW, SNAKE_ROWS * SNAKE_CH, C_MOSS);
    for (i = 0u; i < snake_len; ++i) {
        r = (u8)(snake_cells[i] / SNAKE_COLS);
        c = (u8)(snake_cells[i] % SNAKE_COLS);
        fill_rect(ox + (int)c * SNAKE_CW + 1, oy + (int)r * SNAKE_CH + 1,
                  SNAKE_CW - 2, SNAKE_CH - 2, i == 0u ? C_AMBER : C_SHAVE);
    }
    r = (u8)(snake_food / SNAKE_COLS);
    c = (u8)(snake_food % SNAKE_COLS);
    fill_circle(ox + (int)c * SNAKE_CW + 4, oy + (int)r * SNAKE_CH + 4,
                3, 3, C_CLAY, C_ROSE);
    if (snake_dead != 0u) {
        draw_text(80, 100, "GAME OVER", C_WHITE, C_INK, 1u);
        draw_text(56, 112, "Enter retry  Esc desk", C_CREAM, C_INK, 1u);
    }
}

static void draw_frame(void) {
    if (app_open == APP_NONE) {
        draw_workshop();
    } else if (app_open == APP_TERM) {
        draw_workshop();
        draw_terminal();
    } else if (app_open == APP_SNAKE) {
        draw_workshop();
        draw_snake_app();
    } else if (app_open == APP_FILES) {
        draw_workshop();
        draw_files_app();
    } else {
        draw_workshop();
        draw_about_app();
    }
}

static void open_selected(void) {
    const u8 app = objects[selected].app;
    if (app == APP_NONE) return;
    app_open = app;
    if (app == APP_TERM) term_open();
    else if (app == APP_SNAKE) snake_reset();
    else if (app == APP_FILES) {
        term_clear();
        file_manager_dir();
    } else if (app == APP_ABOUT) {
        term_clear();
        print_about();
    }
}

static void move_selected(i16 dx, i16 dy) {
    objects[selected].x = (i16)(objects[selected].x + dx);
    objects[selected].y = (i16)(objects[selected].y + dy);
    clamp_object(&objects[selected]);
}

void kernel_main(void) {
    u8 dirty = 1u;

    palette_warm();
    keyboard_initialize();
    pit_initialize();
    objects_init();
    (void)kalloc(32u);
    app_open = APP_NONE;

    for (;;) {
        const u16 key = keyboard_poll();

        if (pit_tick(PIT_DIVISOR * 4u) != 0u) {
            bounce = (u8)(bounce ^ 1u);
            if (app_open == APP_SNAKE && snake_dead == 0u) {
                snake_step();
            }
            dirty = 1u;
        }

        if (key != KEY_NONE) {
            if (app_open == APP_NONE) {
                if (key == KEY_TAB || key == (u16)'\t') {
                    selected = (u8)((selected + 1u) % OBJ_COUNT);
                } else if (key == KEY_LEFT) {
                    move_selected(-6, 0);
                } else if (key == KEY_RIGHT) {
                    move_selected(6, 0);
                } else if (key == KEY_UP) {
                    move_selected(0, -6);
                } else if (key == KEY_DOWN) {
                    move_selected(0, 6);
                } else if (key == (u16)'\n') {
                    open_selected();
                } else if (key == (u16)'1') selected = 4u;
                else if (key == (u16)'2') selected = 5u;
                else if (key == (u16)'3') selected = 6u;
                else if (key == (u16)'4') selected = 7u;
            } else if (app_open == APP_TERM) {
                term_key(key);
            } else if (app_open == APP_SNAKE) {
                if (key == 27u) app_open = APP_NONE;
                else if (snake_dead != 0u &&
                         (key == (u16)'\n' || key == (u16)'n' || key == (u16)'N')) {
                    snake_reset();
                } else {
                    snake_turn(key);
                }
            } else {
                if (key == 27u) app_open = APP_NONE;
            }
            dirty = 1u;
        }

        if (dirty != 0u) {
            draw_frame();
            dirty = 0u;
        }
    }
}
