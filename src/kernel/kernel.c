/*
 * DimOS protected-mode kernel.
 *
 * Policy: hardware setup and port I/O stay in kernel.asm; the command shell,
 * PS/2 scan-code decoder, VGA console, timer handling, and Snake belong in C.
 * No hosted C library is used.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

extern u8 io_in8(u16 port);
extern void io_out8(u16 port, u8 value);

#define VGA_WIDTH 80u
#define VGA_HEIGHT 25u
#define VGA_MEMORY ((volatile u16 *)0xB8000u)
#define FRAMEBUFFER ((volatile u8 *)0xA0000u)
#define SCREEN_WIDTH 320u
#define SCREEN_HEIGHT 200u

#define KEY_NONE 0u
#define KEY_UP 0x100u
#define KEY_RIGHT 0x101u
#define KEY_DOWN 0x102u
#define KEY_LEFT 0x103u

#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u

#define PIT_CHANNEL_0 0x40u
#define PIT_COMMAND 0x43u
#define PIT_DIVISOR 11932u
#define PIT_MOVE_COUNTS (PIT_DIVISOR * 9u)

#define COMMAND_CAPACITY 31u

#define SNAKE_MAX_LENGTH 512u
#define SNAKE_START_ROW 12u
#define SNAKE_START_COLUMN 42u
#define SNAKE_TOP 3u
#define SNAKE_ROWS 20u
#define SNAKE_COLUMNS 78u

#define DIRECTION_UP 0u
#define DIRECTION_RIGHT 1u
#define DIRECTION_DOWN 2u
#define DIRECTION_LEFT 3u

static u8 console_row;
static u8 console_column;
static u8 console_attribute = 0x07u;

static u8 keyboard_modifiers;
static u8 keyboard_extended;
static u8 keyboard_pause_bytes;
static u8 keyboard_caps_lock;

static u16 pit_last_count;
static u32 pit_accumulated_counts;

static u16 snake_cells[SNAKE_MAX_LENGTH];
static u16 snake_length;
static u16 snake_score;
static u8 snake_direction;
static u32 random_state = 0xD14E05u;

static void cursor_update(void) {
    u16 position = (u16)((u16)console_row * VGA_WIDTH + console_column);

    io_out8(0x3D4u, 0x0Fu);
    io_out8(0x3D5u, (u8)position);
    io_out8(0x3D4u, 0x0Eu);
    io_out8(0x3D5u, (u8)(position >> 8u));
}

static void cursor_set_visible(u8 visible) {
    u8 start;

    io_out8(0x3D4u, 0x0Au);
    start = io_in8(0x3D5u);
    if (visible != 0u) {
        start = (u8)(start & (u8)~0x20u);
    } else {
        start = (u8)(start | 0x20u);
    }
    io_out8(0x3D5u, start);
}

static void console_scroll(void) {
    u16 row;
    u16 column;

    if (console_row < VGA_HEIGHT) {
        return;
    }

    for (row = 1u; row < VGA_HEIGHT; ++row) {
        for (column = 0u; column < VGA_WIDTH; ++column) {
            VGA_MEMORY[(row - 1u) * VGA_WIDTH + column] =
                VGA_MEMORY[row * VGA_WIDTH + column];
        }
    }

    for (column = 0u; column < VGA_WIDTH; ++column) {
        VGA_MEMORY[(VGA_HEIGHT - 1u) * VGA_WIDTH + column] =
            (u16)((u16)console_attribute << 8u) | (u16)' ';
    }
    console_row = (u8)(VGA_HEIGHT - 1u);
}

static void console_clear(void) {
    u16 cell;
    const u16 blank = (u16)((u16)console_attribute << 8u) | (u16)' ';

    for (cell = 0u; cell < VGA_WIDTH * VGA_HEIGHT; ++cell) {
        VGA_MEMORY[cell] = blank;
    }
    console_row = 0u;
    console_column = 0u;
    cursor_set_visible(1u);
    cursor_update();
}

static void console_put_character(char character) {
    if (character == '\n') {
        console_column = 0u;
        ++console_row;
    } else if (character == '\r') {
        console_column = 0u;
    } else {
        const u16 position =
            (u16)((u16)console_row * VGA_WIDTH + console_column);
        VGA_MEMORY[position] =
            (u16)((u16)console_attribute << 8u) | (u8)character;
        ++console_column;
        if (console_column >= VGA_WIDTH) {
            console_column = 0u;
            ++console_row;
        }
    }

    console_scroll();
    cursor_update();
}

static void console_backspace(void) {
    u16 position;

    if (console_column == 0u) {
        return;
    }

    --console_column;
    position = (u16)((u16)console_row * VGA_WIDTH + console_column);
    VGA_MEMORY[position] =
        (u16)((u16)console_attribute << 8u) | (u16)' ';
    cursor_update();
}

static void console_write(const char *text) {
    while (*text != '\0') {
        console_put_character(*text);
        ++text;
    }
}

static void pixel(u16 x, u16 y, u8 color) {
    if (x < SCREEN_WIDTH && y < SCREEN_HEIGHT) FRAMEBUFFER[y * SCREEN_WIDTH + x] = color;
}

static void fill_rect(u16 x, u16 y, u16 width, u16 height, u8 color) {
    u16 row;
    u16 column;
    for (row = 0u; row < height && y + row < SCREEN_HEIGHT; ++row) {
        for (column = 0u; column < width && x + column < SCREEN_WIDTH; ++column) {
            pixel((u16)(x + column), (u16)(y + row), color);
        }
    }
}

/* Compact built-in desktop: no images or external resources. */
static void desktop_draw(void) {
    u16 cell;
    fill_rect(0u, 0u, 320u, 200u, 1u);
    fill_rect(0u, 0u, 320u, 20u, 9u);
    fill_rect(0u, 178u, 320u, 22u, 8u);
    fill_rect(18u, 38u, 105u, 92u, 7u);
    fill_rect(28u, 48u, 85u, 20u, 15u);
    fill_rect(136u, 38u, 165u, 110u, 7u);
    fill_rect(146u, 48u, 145u, 20u, 3u);
    for (cell = 0u; cell < VGA_WIDTH * VGA_HEIGHT; ++cell) {
        VGA_MEMORY[cell] = (u16)(0x1Fu << 8u) | (u16)' ';
    }
    for (cell = 0u; cell < VGA_WIDTH; ++cell) {
        VGA_MEMORY[cell] = (u16)(0x17u << 8u) | (u16)' ';
    }
    VGA_MEMORY[0] = (u16)(0x1Fu << 8u) | (u16)'D';
    VGA_MEMORY[1] = (u16)(0x1Fu << 8u) | (u16)'i';
    VGA_MEMORY[2] = (u16)(0x1Fu << 8u) | (u16)'m';
    VGA_MEMORY[3] = (u16)(0x1Fu << 8u) | (u16)'O';
    VGA_MEMORY[4] = (u16)(0x1Fu << 8u) | (u16)'S';
    console_attribute = 0x1Fu;
    console_row = 2u;
    console_column = 3u;
    console_write("[ Files ]     [ Terminal ]     [ Settings ]\n\n");
    console_write("  DimOS desktop\n\n");
    console_write("  Terminal is ready\n\n");
    cursor_update();
}

static void console_write_unsigned(u32 value) {
    char digits[10];
    u8 length = 0u;

    if (value == 0u) {
        console_put_character('0');
        return;
    }

    while (value != 0u) {
        digits[length] = (char)('0' + (char)(value % 10u));
        value /= 10u;
        ++length;
    }

    while (length != 0u) {
        --length;
        console_put_character(digits[length]);
    }
}

static void console_set_position(u8 row, u8 column) {
    console_row = row;
    console_column = column;
    cursor_update();
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

/*
 * Read translated set-1 scan codes directly from the emulated PS/2
 * controller. This deliberately avoids BIOS int 16h, which is unreliable
 * after handoff in the v86 debug launcher.
 */
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
                case 0x35u: return (u16)'/';
                default: {
                    const char extended_char = keyboard_ascii(base_code);
                    if (extended_char != '\0') {
                        return (u16)(u8)extended_char;
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

static u16 keyboard_read(void) {
    u16 key;

    do {
        key = keyboard_poll();
    } while (key == KEY_NONE);
    return key;
}

static void keyboard_initialize(void) {
    keyboard_modifiers = 0u;
    keyboard_extended = 0u;
    keyboard_pause_bytes = 0u;
    keyboard_caps_lock = 0u;

    /* Drop stale BIOS handoff bytes, including command acknowledgements. */
    while ((io_in8(PS2_STATUS) & 0x01u) != 0u) {
        (void)io_in8(PS2_DATA);
    }
}

static u16 pit_read_counter(void) {
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
    pit_last_count = pit_read_counter();
    pit_accumulated_counts = 0u;
}

static void pit_reset_interval(void) {
    pit_last_count = pit_read_counter();
    pit_accumulated_counts = 0u;
}

static u8 pit_move_due(void) {
    const u16 current = pit_read_counter();
    u16 elapsed;

    if (pit_last_count >= current) {
        elapsed = (u16)(pit_last_count - current);
    } else {
        elapsed = (u16)(pit_last_count + (PIT_DIVISOR - current));
    }
    pit_last_count = current;
    pit_accumulated_counts += elapsed;

    if (pit_accumulated_counts >= PIT_MOVE_COUNTS) {
        pit_accumulated_counts -= PIT_MOVE_COUNTS;
        return 1u;
    }
    return 0u;
}

static void screen_cell(u16 position, char character, u8 attribute) {
    VGA_MEMORY[position] = (u16)((u16)attribute << 8u) | (u8)character;
}

static char screen_character(u16 position) {
    return (char)(u8)VGA_MEMORY[position];
}

static void snake_draw_score(void) {
    console_set_position(0u, 65u);
    console_write("Score: ");
    console_write_unsigned(snake_score);
}

static void snake_draw_arena(void) {
    u16 position;
    u16 row;

    console_set_position(0u, 0u);
    console_write("DimOS Snake");
    console_set_position(1u, 0u);
    console_write("WASD/arrows: move   N: new game   ESC: command line");
    snake_draw_score();

    for (position = 2u * VGA_WIDTH; position < 3u * VGA_WIDTH; ++position) {
        screen_cell(position, '#', 0x09u);
    }
    for (position = 23u * VGA_WIDTH; position < 24u * VGA_WIDTH; ++position) {
        screen_cell(position, '#', 0x09u);
    }
    for (row = SNAKE_TOP; row < SNAKE_TOP + SNAKE_ROWS; ++row) {
        screen_cell((u16)(row * VGA_WIDTH), '#', 0x09u);
        screen_cell((u16)(row * VGA_WIDTH + 79u), '#', 0x09u);
    }
}

static u32 next_random(void) {
    u32 value = random_state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    if (value == 0u) {
        value = 0xA341316Cu;
    }
    random_state = value;
    return value;
}

static void snake_place_food(void) {
    u16 position;

    do {
        const u16 row = (u16)(SNAKE_TOP + (next_random() % SNAKE_ROWS));
        const u16 column = (u16)(1u + (next_random() % SNAKE_COLUMNS));
        position = (u16)(row * VGA_WIDTH + column);
    } while (screen_character(position) != ' ');

    screen_cell(position, '@', 0x0Cu);
}

static void snake_initialize(void) {
    u16 index;
    u16 position = (u16)(SNAKE_START_ROW * VGA_WIDTH + SNAKE_START_COLUMN);

    console_clear();
    cursor_set_visible(0u);
    snake_length = 5u;
    snake_score = 0u;
    snake_direction = DIRECTION_RIGHT;
    random_state ^= (u32)pit_read_counter() | 1u;

    snake_draw_arena();
    for (index = 0u; index < snake_length; ++index) {
        snake_cells[index] = position;
        screen_cell(position, index == 0u ? 'O' : 'o',
                    index == 0u ? 0x0Fu : 0x0Au);
        --position;
    }
    snake_place_food();
    pit_reset_interval();
}

/* Return 1 after a collision and 0 after a successful move. */
static u8 snake_move(void) {
    u16 new_head = snake_cells[0];
    u16 index;
    u8 grew = 0u;
    char target;

    if (snake_direction == DIRECTION_UP) {
        new_head = (u16)(new_head - VGA_WIDTH);
    } else if (snake_direction == DIRECTION_RIGHT) {
        ++new_head;
    } else if (snake_direction == DIRECTION_DOWN) {
        new_head = (u16)(new_head + VGA_WIDTH);
    } else {
        --new_head;
    }

    target = screen_character(new_head);
    if (target == '@') {
        if (snake_length >= SNAKE_MAX_LENGTH) {
            return 1u;
        }
        grew = 1u;
        ++snake_length;
        snake_score = (u16)(snake_score + 10u);
    } else if (target != ' ') {
        return 1u;
    }

    if (grew == 0u) {
        screen_cell(snake_cells[snake_length - 1u], ' ', 0x07u);
    }

    index = (u16)(snake_length - 1u);
    while (index != 0u) {
        snake_cells[index] = snake_cells[index - 1u];
        --index;
    }

    screen_cell(snake_cells[0], 'o', 0x0Au);
    snake_cells[0] = new_head;
    screen_cell(new_head, 'O', 0x0Fu);

    if (grew != 0u) {
        snake_draw_score();
        snake_place_food();
    }
    return 0u;
}

static void snake_change_direction(u16 key) {
    if ((key == (u16)'w' || key == (u16)'W' || key == KEY_UP) &&
        snake_direction != DIRECTION_DOWN) {
        snake_direction = DIRECTION_UP;
    } else if ((key == (u16)'d' || key == (u16)'D' || key == KEY_RIGHT) &&
               snake_direction != DIRECTION_LEFT) {
        snake_direction = DIRECTION_RIGHT;
    } else if ((key == (u16)'s' || key == (u16)'S' || key == KEY_DOWN) &&
               snake_direction != DIRECTION_UP) {
        snake_direction = DIRECTION_DOWN;
    } else if ((key == (u16)'a' || key == (u16)'A' || key == KEY_LEFT) &&
               snake_direction != DIRECTION_RIGHT) {
        snake_direction = DIRECTION_LEFT;
    }
}

static void snake_game(void) {
    u8 restart = 1u;

    while (restart != 0u) {
        u8 collided = 0u;
        restart = 0u;
        snake_initialize();

        while (collided == 0u) {
            const u16 key = keyboard_poll();

            if (key == 27u) {
                cursor_set_visible(1u);
                return;
            }
            if (key == (u16)'n' || key == (u16)'N') {
                restart = 1u;
                break;
            }
            if (key != KEY_NONE) {
                snake_change_direction(key);
            }
            if (pit_move_due() != 0u) {
                collided = snake_move();
            }
        }

        if (restart == 0u && collided != 0u) {
            console_set_position(12u, 20u);
            console_write(" GAME OVER - ENTER/N: retry, ESC: exit ");
            for (;;) {
                const u16 key = keyboard_read();
                if (key == 27u) {
                    cursor_set_visible(1u);
                    return;
                }
                if (key == (u16)'\n' || key == (u16)'n' || key == (u16)'N') {
                    restart = 1u;
                    break;
                }
            }
        }
    }
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

static char *trim_command(char *command) {
    char *start = command;
    char *end;

    while (*start == ' ') {
        ++start;
    }
    end = start;
    while (*end != '\0') {
        ++end;
    }
    while (end != start && end[-1] == ' ') {
        --end;
    }
    *end = '\0';
    return start;
}

static void read_line(char *buffer, u8 capacity) {
    u8 length = 0u;

    for (;;) {
        const u16 key = keyboard_read();

        if (key == (u16)'\n') {
            buffer[length] = '\0';
            console_put_character('\n');
            return;
        }
        if (key == (u16)'\b') {
            if (length != 0u) {
                --length;
                console_backspace();
            }
            continue;
        }
        if (key >= 32u && key <= 126u && length < capacity) {
            buffer[length] = (char)key;
            ++length;
            console_put_character((char)key);
        }
    }
}

#define FAT_ROOT ((volatile u8 *)0x10000u)
#define FAT_TABLE ((volatile u8 *)0x07E00u)
#define FILE_DATA ((volatile u8 *)0x30000u)
#define FAT_ROOT_ENTRIES 224u
#define FAT_FILE_LIMIT 32768u

static u8 file_deleted[FAT_ROOT_ENTRIES];

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
            while (pos < 8u) { if (entry[pos++] != ' ') return 0u; }
            pos = 8u;
        } else {
            if (pos >= 11u || (pos == 8u && entry[pos] == ' ')) return 0u;
            if (ascii_upper((u8)character) != ascii_upper(entry[pos])) return 0u;
            ++pos;
        }
    }
    if (i == 0u) return 0u;
    while (pos < 11u) { if (entry[pos++] != ' ') return 0u; }
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

static void file_print_name(const volatile u8 *entry) {
    u8 index;
    for (index = 0u; index < 8u && entry[index] != ' '; ++index) console_put_character((char)entry[index]);
    if (entry[8] != ' ') {
        console_put_character('.');
        for (index = 8u; index < 11u && entry[index] != ' '; ++index) console_put_character((char)entry[index]);
    }
}

static void file_manager_dir(void) {
    u16 index;
    console_write("Disk files (FAT12):\n");
    for (index = 0u; index < FAT_ROOT_ENTRIES; ++index) {
        const volatile u8 *entry = FAT_ROOT + index * 32u;
        if (entry[0] == 0x00u) break;
        if (entry[0] == 0xE5u || entry[11] == 0x0Fu || (entry[11] & 0x08u) != 0u || file_deleted[index] != 0u) continue;
        file_print_name(entry);
        console_write("  ");
        console_write_unsigned((u32)entry[28] | ((u32)entry[29] << 8u) | ((u32)entry[30] << 16u) | ((u32)entry[31] << 24u));
        console_write(" bytes\n");
    }
}

static void file_type(const char *name) {
    const u16 index = file_find(name);
    u16 cluster;
    u32 remaining;
    if (index == 0xFFFFu) { console_write("File not found.\n"); return; }
    cluster = (u16)FAT_ROOT[index * 32u + 26u] | ((u16)FAT_ROOT[index * 32u + 27u] << 8u);
    remaining = (u32)FAT_ROOT[index * 32u + 28u] | ((u32)FAT_ROOT[index * 32u + 29u] << 8u) | ((u32)FAT_ROOT[index * 32u + 30u] << 16u) | ((u32)FAT_ROOT[index * 32u + 31u] << 24u);
    if (remaining > FAT_FILE_LIMIT) remaining = FAT_FILE_LIMIT;
    while (remaining != 0u && cluster >= 2u && cluster < 0xFF8u) {
        const u32 offset = (u32)(cluster - 2u) * 512u;
        u32 count = remaining < 512u ? remaining : 512u;
        u32 position;
        for (position = 0u; position < count; ++position) console_put_character((char)FILE_DATA[offset + position]);
        remaining -= count;
        cluster = fat12_next(cluster);
    }
    console_put_character('\n');
}

static u8 strings_equal(const char *left, const char *right);

static u8 is_protected_name(const char *name) {
    return strings_equal(name, "COMPUTER") != 0u ||
           strings_equal(name, "COMPUTER/") != 0u ||
           strings_equal(name, "DOWNLOADS") != 0u ||
           strings_equal(name, "DOWNLOADS/") != 0u;
}

static void clear_downloads(void) {
    u16 index;
    for (index = 0u; index < FAT_ROOT_ENTRIES; ++index) {
        const volatile u8 *entry = FAT_ROOT + index * 32u;
        if (entry[0] == 0xE5u || entry[11] == 0x0Fu || (entry[11] & 0x08u) != 0u) continue;
        if (entry[0] == 'D' && entry[1] == 'O' && entry[2] == 'W' && entry[3] == 'N') file_deleted[index] = 1u;
    }
    console_write("Downloads cleared (session only).\n");
}

static void file_delete(const char *name) {
    const u16 index = file_find(name);
    const volatile u8 *entry;
    if (index == 0xFFFFu) { console_write("File not found.\n"); return; }
    entry = FAT_ROOT + index * 32u;
    if (is_protected_name(name) != 0u || file_name_matches(entry, "KERNEL.BIN") != 0u) {
        console_write("KERNEL.BIN is protected.\n");
        return;
    }
    file_deleted[index] = 1u;
    console_write("Deleted (session only): "); file_print_name(entry); console_write("\n");
}

static char *command_argument(char *command) {
    while (*command != ' ' && *command != '\0') ++command;
    if (*command == '\0') return command;
    *command++ = '\0';
    while (*command == ' ') ++command;
    return command;
}

static void print_help(void) {
    console_write("Commands:\n");
    console_write("  DIR    open the computer files\n");
    console_write("  TYPE   read a file, for example TYPE README.TXT\n");
    console_write("  DEL    delete a file; COMPUTER and DOWNLOADS are protected\n");
    console_write("  DOWNLOADS  show downloads\n");
    console_write("  CLEAR DOWNLOADS  empty downloads\n");
    console_write("  SNAKE  start the built-in game\n");
    console_write("  CLEAR  clear the screen\n");
    console_write("  HELP   show this list\n");
}

void kernel_main(void) {
    char command_buffer[COMMAND_CAPACITY + 1u];

    console_clear();
    desktop_draw();
    keyboard_initialize();
    pit_initialize();

    console_write("DimOS protected-mode C kernel\n");
    console_write("PS/2 keyboard ready (v86 compatible). Type HELP.\n\n");

    for (;;) {
        char *command;
        char *argument;

        console_write("> ");
        read_line(command_buffer, COMMAND_CAPACITY);
        command = trim_command(command_buffer);
        argument = command_argument(command);

        if (*command == '\0') {
            continue;
        }
        if (strings_equal(command, "DIR") != 0u || strings_equal(command, "FILES") != 0u) {
            file_manager_dir();
        } else if (strings_equal(command, "TYPE") != 0u) {
            if (*argument == '\0') console_write("Usage: TYPE filename\n");
            else file_type(argument);
        } else if (strings_equal(command, "DEL") != 0u) {
            if (*argument == '\0') console_write("Usage: DEL filename\n");
            else file_delete(argument);
        } else if (strings_equal(command, "DOWNLOADS") != 0u) {
            console_write("Downloads (can only be cleared):\n");
            file_manager_dir();
        } else if (strings_equal(command, "SNAKE") != 0u) {
            snake_game();
            console_clear();
            console_write("Back at the command line. Type HELP.\n\n");
        } else if (strings_equal(command, "CLEAR") != 0u ||
                   strings_equal(command, "CLS") != 0u) {
            if (strings_equal(argument, "DOWNLOADS") != 0u) clear_downloads();
            else console_clear();
        } else if (strings_equal(command, "HELP") != 0u) {
            print_help();
        } else {
            console_write("Unknown command. Type HELP.\n");
        }
    }
}
