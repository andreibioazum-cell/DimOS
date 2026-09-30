/*
 * app_terminal.c -- the command line, for people who want it.
 *
 * DimOS never asks anybody to type, so this window brings its own keyboard:
 * four rows of keys that a finger can press. A real keyboard works too, if
 * one is plugged in.
 */

#include "dimos.h"

#define TERM_COLUMNS 37
#define TERM_VISIBLE_ROWS 6
#define HISTORY_ROWS 40
#define INPUT_LIMIT 38u

#define KEY_COLUMNS 11
#define KEY_ROWS 4
#define KEY_WIDTH 27
#define KEY_HEIGHT 15
#define KEY_STEP 16
#define KEY_LEFT (s16)(WINDOW_LEFT + 4)
#define KEY_TOP (s16)(WINDOW_TOP + 75)

#define ID_KEY_FIRST 1u                    /* 1 .. 44 */
#define ID_KEY_LAST (ID_KEY_FIRST + KEY_COLUMNS * KEY_ROWS - 1u)

#define SPECIAL_BACKSPACE 1u
#define SPECIAL_SPACE 2u
#define SPECIAL_ENTER 3u
#define SPECIAL_SHIFT 4u
#define SPECIAL_CLEAR 5u

static const char *const icon[16] = {
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "..XooooooooooX..",
    "..XooooooooooX..",
    "..X.XX.......X..",
    "..X..XX......X..",
    "..X...XX.....X..",
    "..X..XX......X..",
    "..X.XX.......X..",
    "..X....XXXX..X..",
    "..XooooooooooX..",
    "..XXXXXXXXXXXX..",
    "................",
    "................",
    "................",
    "................",
};

/* What each key prints, row by row. A zero means the key does something
 * instead of printing a character. */
static const char key_labels[KEY_ROWS][KEY_COLUMNS][4] = {
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "<-" },
    { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "." },
    { "A", "S", "D", "F", "G", "H", "J", "K", "L", "SP", "ENT" },
    { "SH", "Z", "X", "C", "V", "B", "N", "M", ":", "/", "CLS" }
};

static const char key_characters[KEY_ROWS][KEY_COLUMNS] = {
    { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\0' },
    { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '.' },
    { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '\0', '\0' },
    { '\0', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ':', '/', '\0' }
};

static const u8 key_special[KEY_ROWS][KEY_COLUMNS] = {
    { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, SPECIAL_BACKSPACE },
    { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u },
    { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, SPECIAL_SPACE, SPECIAL_ENTER },
    { SPECIAL_SHIFT, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, SPECIAL_CLEAR }
};

static char history[HISTORY_ROWS][TERM_COLUMNS + 1u];
static u16 history_count;
static u16 history_top;
static char input_line[INPUT_LIMIT];
static u8 shift_on;

static void terminal_print(const char *text);

static void history_add(const char *line) {
    u16 position = 0u;
    u16 length = text_length(line);

    if (length == 0u) {
        if (history_count < HISTORY_ROWS) {
            history[history_count][0] = '\0';
            ++history_count;
        }
        return;
    }

    /* Long lines are wrapped over as many rows as they need. */
    while (position < length) {
        u16 count = 0u;

        while (count < TERM_COLUMNS && (u16)(position + count) < length) {
            history[history_count % HISTORY_ROWS][count] = line[position + count];
            ++count;
        }
        history[history_count % HISTORY_ROWS][count] = '\0';
        ++history_count;
        position = (u16)(position + count);
        if (history_count > HISTORY_ROWS) {
            history_top = (u16)(history_count - HISTORY_ROWS);
        }
    }
}

static void terminal_print(const char *text) {
    history_add(text);
}

/* Appends a value as exactly two digits, "07" instead of "7". */
static void append_two_digits(char *out, u16 capacity, u8 value) {
    if (value < 10u) {
        text_append_character(out, '0', capacity);
    }
    text_append_number(out, value, capacity);
}

static void command_help(void) {
    terminal_print("Commands:");
    terminal_print("  DIR          list the files on the disk");
    terminal_print("  TYPE name    read a file");
    terminal_print("  DEL name     hide a file until reboot");
    terminal_print("  CLS          clear this window");
    terminal_print("  TIME         show the clock");
    terminal_print("  DATE         show the date");
    terminal_print("  VER          show the version");
    terminal_print("  THEME        switch the palette");
    terminal_print("  BEEP         test the speaker");
    terminal_print("  MENU         open the Whisker menu");
    terminal_print("  XFCE         about this desktop");
    terminal_print("  CHEESY       the cheese ball kitchen");
    terminal_print("  EXIT         back to the desktop");
}

static void command_directory(void) {
    u16 slot;
    char line[TERM_COLUMNS + 1u];

    for (slot = 0u; slot < file_system_visible_count(); ++slot) {
        const u16 index = file_system_visible(slot);

        if (index == FILE_NOT_FOUND) {
            break;
        }
        file_system_name(index, line);
        text_pad_right(line, 13u, (u16)sizeof(line));
        text_append_number(line, file_system_size(index), (u16)sizeof(line));
        text_append(line, " bytes", (u16)sizeof(line));
        terminal_print(line);
    }
}

static void command_type(const char *name) {
    char buffer[256];
    u32 bytes;
    u32 position = 0u;
    u16 column = 0u;
    char line[TERM_COLUMNS + 1u];

    if (*name == '\0') {
        terminal_print("Usage: TYPE filename");
        return;
    }
    {
        const u16 index = file_system_find(name);

        if (index == FILE_NOT_FOUND) {
            terminal_print("No such file");
            return;
        }
        bytes = file_system_read(index, 0u, buffer, (u32)sizeof(buffer) - 1u);
    }
    buffer[bytes] = '\0';

    line[0] = '\0';
    while (buffer[position] != '\0') {
        if (buffer[position] == '\n' || column == TERM_COLUMNS) {
            terminal_print(line);
            line[0] = '\0';
            column = 0u;
            if (buffer[position] == '\n') {
                ++position;
                continue;
            }
        }
        if (buffer[position] != '\r') {
            line[column] = buffer[position];
            ++column;
            line[column] = '\0';
        }
        ++position;
    }
    terminal_print(line);
}

static void command_delete(const char *name) {
    u16 index;

    if (*name == '\0') {
        terminal_print("Usage: DEL filename");
        return;
    }
    index = file_system_find(name);
    if (index == FILE_NOT_FOUND) {
        terminal_print("No such file");
        return;
    }
    if (file_system_is_protected(index) != 0u) {
        terminal_print("KERNEL.BIN is protected");
        return;
    }
    if (file_system_delete(index) != 0u) {
        terminal_print("Hidden until the next reboot");
    }
}

static void command_time(void) {
    char buffer[16];

    buffer[0] = '\0';
    text_append(buffer, "Time ", (u16)sizeof(buffer));
    append_two_digits(buffer, (u16)sizeof(buffer), clock_hours());
    text_append_character(buffer, ':', (u16)sizeof(buffer));
    append_two_digits(buffer, (u16)sizeof(buffer), clock_minutes());
    text_append_character(buffer, ':', (u16)sizeof(buffer));
    append_two_digits(buffer, (u16)sizeof(buffer), clock_seconds());
    terminal_print(buffer);
}

/* Splits "COMMAND argument" in place and returns the argument. */
static char *split_argument(char *command) {
    while (*command != ' ' && *command != '\0') {
        ++command;
    }
    if (*command == '\0') {
        return command;
    }
    *command = '\0';
    ++command;
    while (*command == ' ') {
        ++command;
    }
    return command;
}

static void run_command(char *command) {
    char *argument = split_argument(command);

    /* The typed line is already in the history from submit_input, so the
     * command only appears once. */
    if (*command == '\0') {
        return;
    }

    if (text_equal_ignore_case(command, "HELP") != 0u) {
        command_help();
    } else if (text_equal_ignore_case(command, "DIR") != 0u) {
        command_directory();
    } else if (text_equal_ignore_case(command, "TYPE") != 0u) {
        command_type(argument);
    } else if (text_equal_ignore_case(command, "DEL") != 0u) {
        command_delete(argument);
    } else if (text_equal_ignore_case(command, "CLS") != 0u ||
               text_equal_ignore_case(command, "CLEAR") != 0u) {
        history_count = 0u;
        history_top = 0u;
    } else if (text_equal_ignore_case(command, "TIME") != 0u) {
        command_time();
    } else if (text_equal_ignore_case(command, "DATE") != 0u) {
        char buffer[20];

        buffer[0] = '\0';
        text_append_number(buffer, clock_year(), (u16)sizeof(buffer));
        text_append_character(buffer, '-', (u16)sizeof(buffer));
        text_append_number(buffer, clock_month(), (u16)sizeof(buffer));
        text_append_character(buffer, '-', (u16)sizeof(buffer));
        text_append_number(buffer, clock_day(), (u16)sizeof(buffer));
        terminal_print(buffer);
    } else if (text_equal_ignore_case(command, "VER") != 0u) {
        terminal_print("DimOS 2.0 + DimXfce, x86-64");
    } else if (text_equal_ignore_case(command, "THEME") != 0u) {
        gfx_select_theme((u8)((gfx_current_theme() + 1u) % THEME_COUNT));
        terminal_print("Palette switched");
    } else if (text_equal_ignore_case(command, "BEEP") != 0u) {
        sound_play_startup();
    } else if (text_equal_ignore_case(command, "MENU") != 0u ||
               text_equal_ignore_case(command, "WHISKER") != 0u) {
        /* Super key of the budget: open the mouse menu from a shell. */
        gui_menu_toggle();
    } else if (text_equal_ignore_case(command, "XFCE") != 0u ||
               text_equal_ignore_case(command, "XFCE4-ABOUT") != 0u) {
        u16 index;

        terminal_print("DimXfce: Xfce in 320x200");
        terminal_print("panel+dock+whiskers, real cheese");
        for (index = 0u; index < gui_application_count(); ++index) {
            if (text_equal(gui_application(index)->title, "About") != 0u) {
                gui_open(index);
            }
        }
    } else if (text_equal_ignore_case(command, "CHEESY") != 0u ||
               text_equal_ignore_case(command, "NYAM") != 0u) {
        u16 index;

        for (index = 0u; index < gui_application_count(); ++index) {
            if (text_equal(gui_application(index)->title,
                           "Cheesy Balls") != 0u) {
                gui_open(index);
            }
        }
    } else if (text_equal_ignore_case(command, "SNAKE") != 0u) {
        gui_open(1u);
    } else if (text_equal_ignore_case(command, "MINES") != 0u) {
        gui_open(2u);
    } else if (text_equal_ignore_case(command, "EXIT") != 0u) {
        gui_go_home();
    } else {
        terminal_print("Unknown command - try HELP");
    }
}

static void submit_input(void) {
    char command[INPUT_LIMIT];

    text_copy(command, input_line, (u16)sizeof(command));
    text_trim(command);
    history_add(input_line);
    input_line[0] = '\0';
    run_command(command);
}

static void press_character(char character) {
    if (shift_on != 0u && character >= 'a' && character <= 'z') {
        character = (char)(character - ('a' - 'A'));
    }
    if (text_length(input_line) < (u16)(INPUT_LIMIT - 1u)) {
        text_append_character(input_line, character, (u16)sizeof(input_line));
    }
}

static void terminal_open(void) {
    if (history_count == 0u) {
        terminal_print("DimXfce terminal");
        terminal_print("Type HELP and press ENT");
        terminal_print("");
    }
    input_line[0] = '\0';
    /* Show the newest rows that fill the window. */
    history_top = (history_count > TERM_VISIBLE_ROWS)
                      ? (u16)(history_count - TERM_VISIBLE_ROWS)
                      : 0u;
}

static void terminal_draw(void) {
    u16 row;
    u16 column;
    const s16 text_top = (s16)(WINDOW_TOP + 2);
    char prompt[INPUT_LIMIT + 3u];

    gfx_fill((s16)(WINDOW_LEFT + 2), text_top, (s16)(TERM_COLUMNS * 8 + 4),
             (s16)(TERM_VISIBLE_ROWS * 9 + 4), COLOR_DEEP);

    /* Follow new output unless the user paged up into the history. */
    if ((u16)(history_top + TERM_VISIBLE_ROWS) < history_count) {
        history_top = (u16)(history_count - TERM_VISIBLE_ROWS);
    }

    for (row = 0u; row < TERM_VISIBLE_ROWS; ++row) {
        const u16 wanted = (u16)(history_top + row);

        if (wanted >= history_count) {
            break;
        }
        gfx_text((s16)(WINDOW_LEFT + 4), (s16)(text_top + 2 + (s16)(row * 9)),
                 history[wanted % HISTORY_ROWS], COLOR_LIGHT_GREEN);
    }
    gfx_outline((s16)(WINDOW_LEFT + 2), text_top, (s16)(TERM_COLUMNS * 8 + 4),
                (s16)(TERM_VISIBLE_ROWS * 9 + 4), COLOR_SHADOW);

    prompt[0] = '\0';
    text_append(prompt, "> ", (u16)sizeof(prompt));
    text_append(prompt, input_line, (u16)sizeof(prompt));
    text_append(prompt, "_", (u16)sizeof(prompt));
    gfx_fill((s16)(WINDOW_LEFT + 2), (s16)(text_top + TERM_VISIBLE_ROWS * 9 + 6),
             (s16)(TERM_COLUMNS * 8 + 4), 12, COLOR_TEXT_FIELD);
    gfx_outline((s16)(WINDOW_LEFT + 2), (s16)(text_top + TERM_VISIBLE_ROWS * 9 + 6),
                (s16)(TERM_COLUMNS * 8 + 4), 12, COLOR_SHADOW);
    gfx_text((s16)(WINDOW_LEFT + 4), (s16)(text_top + TERM_VISIBLE_ROWS * 9 + 8),
             prompt, COLOR_BLACK);

    for (row = 0u; row < KEY_ROWS; ++row) {
        for (column = 0u; column < KEY_COLUMNS; ++column) {
            const s16 x = (s16)(KEY_LEFT + (s16)(column * KEY_WIDTH));
            const s16 y = (s16)(KEY_TOP + (s16)(row * KEY_STEP));
            const u16 id = (u16)(ID_KEY_FIRST + (row * KEY_COLUMNS + column));
            const u8 selected =
                (u8)(key_special[row][column] == SPECIAL_SHIFT && shift_on != 0u);

            if (selected != 0u) {
                gui_button_colored(x, y, (s16)(KEY_WIDTH - 2), (s16)KEY_HEIGHT,
                                   key_labels[row][column], id,
                                   COLOR_SELECTION, COLOR_WHITE);
            } else {
                gui_button(x, y, (s16)(KEY_WIDTH - 2), (s16)KEY_HEIGHT,
                           key_labels[row][column], id);
            }
        }
    }
}

static void terminal_event(const Event *event) {
    if (event->type == EVENT_CLICK && event->key >= ID_KEY_FIRST &&
        event->key <= ID_KEY_LAST) {
        const u16 index = (u16)(event->key - ID_KEY_FIRST);
        const u16 row = (u16)(index / KEY_COLUMNS);
        const u16 column = (u16)(index % KEY_COLUMNS);
        const u8 special = key_special[row][column];

        if (special == SPECIAL_BACKSPACE) {
            const u16 length = text_length(input_line);

            if (length != 0u) {
                input_line[length - 1u] = '\0';
            }
        } else if (special == SPECIAL_SPACE) {
            press_character(' ');
        } else if (special == SPECIAL_ENTER) {
            submit_input();
        } else if (special == SPECIAL_SHIFT) {
            shift_on = (u8)(shift_on ^ 1u);
        } else if (special == SPECIAL_CLEAR) {
            history_count = 0u;
            history_top = 0u;
        } else {
            press_character(key_characters[row][column]);
        }
        sound_beep();
        return;
    }

    if (event->type != EVENT_KEY) {
        return;
    }
    if (event->key == KEY_ENTER) {
        submit_input();
    } else if (event->key == KEY_BACKSPACE) {
        const u16 length = text_length(input_line);

        if (length != 0u) {
            input_line[length - 1u] = '\0';
        }
    } else if (event->key == KEY_PAGE_UP && history_top != 0u) {
        --history_top;
    } else if (event->key == KEY_PAGE_DOWN &&
               (u16)(history_top + TERM_VISIBLE_ROWS) < history_count) {
        ++history_top;
    } else if (event->key >= 32u && event->key < 127u) {
        press_character((char)event->key);
    }
}

const Application application_terminal = {
    "Term",
    "Terminal",
    icon,
    COLOR_LIGHT_GRAY,
    terminal_open,
    terminal_draw,
    terminal_event,
    0
};
