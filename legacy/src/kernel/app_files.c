/*
 * app_files.c -- the file manager, styled like the iOS Files app.
 *
 * Shows the files that really are on the boot disk, opens them as text and
 * can hide one of them for the rest of the session. KERNEL.BIN cannot be
 * deleted, and nothing is ever written back to the disk.
 */

#include "dimos.h"

#define ROW_HEIGHT 32
#define VISIBLE_ROWS 8
#define LIST_X (s16)APP_LEFT
#define LIST_Y 74
#define LIST_WIDTH 520
#define LIST_HEIGHT (VISIBLE_ROWS * ROW_HEIGHT + 8)
#define SIDE_X (s16)(LIST_X + LIST_WIDTH + 16)
#define SIDE_WIDTH (s16)(APP_RIGHT - SIDE_X)
#define TEXT_COLUMNS 60
#define TEXT_ROWS 20
#define FILE_BUFFER_BYTES 2048u

#define ID_FILE_FIRST 1u
#define ID_OPEN 20u
#define ID_DELETE 21u
#define ID_PREVIOUS_PAGE 22u
#define ID_NEXT_PAGE 23u
#define ID_BACK 24u
#define ID_SCROLL_UP 25u
#define ID_SCROLL_DOWN 26u
#define ID_SCROLL_TOP 27u
#define ID_SCROLL_PAGE_UP 28u
#define ID_SCROLL_PAGE_DOWN 29u

static const char *const icon[16] = {
    "................",
    "................",
    "................",
    ".XXXX...........",
    ".XooX...........",
    ".XooXXXXXXXXXX..",
    ".XooooooooooXX..",
    ".XooooooooooXX..",
    ".XooooooooooXX..",
    ".XooooooooooXX..",
    ".XooooooooooXX..",
    ".XooooooooooXX..",
    ".XXXXXXXXXXXXX..",
    "................",
    "................",
    "................",
};

static u8 viewing;
static u16 page;
static u16 selected_slot;
static u16 selected_index = FILE_NOT_FOUND;
static u16 text_top;
static u16 text_lines;
static char file_text[FILE_BUFFER_BYTES];
static char line[TEXT_COLUMNS + 1u];
static char message[48];

static void build_message(void) {
    message[0] = '\0';
    if (viewing != 0u) {
        text_append(message, "Scroll the page with the buttons on the right",
                    (u16)sizeof(message));
        return;
    }
    text_append_number(message, file_system_visible_count(),
                       (u16)sizeof(message));
    text_append(message, " files on the boot disk", (u16)sizeof(message));
}

static void open_selected(void) {
    u32 bytes;

    if (selected_index == FILE_NOT_FOUND) {
        return;
    }
    bytes = file_system_read(selected_index, 0u, file_text,
                             FILE_BUFFER_BYTES - 1u);
    file_text[bytes] = '\0';

    /* DOS text files end their lines with CR LF; the viewer only knows LF,
     * so drop the carriage returns before counting and drawing lines. */
    {
        u32 from = 0u;
        u32 to = 0u;

        while (file_text[from] != '\0') {
            if (file_text[from] != '\r') {
                file_text[to] = file_text[from];
                ++to;
            }
            ++from;
        }
        file_text[to] = '\0';
    }

    /* Count the lines the viewer will show, wrapping long ones. */
    text_lines = 0u;
    {
        u32 position = 0u;
        u16 column = 0u;

        while (file_text[position] != '\0') {
            if (file_text[position] == '\n' || column == TEXT_COLUMNS) {
                ++text_lines;
                column = 0u;
            }
            if (file_text[position] != '\n') {
                ++column;
            }
            ++position;
        }
        ++text_lines;
    }
    text_top = 0u;
    viewing = 1u;
    build_message();
}

/* Writes one visible line of the file into line[], or an empty string when
 * the viewer has run past the end. */
static void fetch_line(u16 wanted) {
    u32 position = 0u;
    u16 current = 0u;
    u16 column = 0u;

    line[0] = '\0';
    while (file_text[position] != '\0') {
        if (current == wanted) {
            while (file_text[position] != '\0' && file_text[position] != '\n' &&
                   column < TEXT_COLUMNS) {
                line[column] = file_text[position];
                ++column;
                ++position;
            }
            line[column] = '\0';
            return;
        }
        if (file_text[position] == '\n' || column == TEXT_COLUMNS) {
            ++current;
            column = 0u;
            if (file_text[position] == '\n') {
                ++position;
            }
            continue;
        }
        ++column;
        ++position;
    }
}

static void files_open(void) {
    viewing = 0u;
    page = 0u;
    selected_slot = 0u;
    selected_index = FILE_NOT_FOUND;
    build_message();
}

static void files_draw(void) {
    u16 row;

    gui_window_frame("Files");

    if (viewing == 0u) {
        gui_section_label((s16)APP_LEFT, 64, "BOOT DISK");
        gui_card(LIST_X, LIST_Y, LIST_WIDTH, LIST_HEIGHT);

        for (row = 0u; row < VISIBLE_ROWS; ++row) {
            const u16 slot = (u16)(page * VISIBLE_ROWS + row);
            const u16 index = file_system_visible(slot);
            const s16 y = (s16)(LIST_Y + 4 + (s16)(row * ROW_HEIGHT));
            char label[13];
            char size[20];
            const u8 chosen = (u8)(slot == selected_slot);

            if (index == FILE_NOT_FOUND) {
                continue;
            }
            file_system_name(index, label);
            size[0] = '\0';
            text_append_number(size, file_system_size(index), (u16)sizeof(size));
            text_append(size, " B", (u16)sizeof(size));

            if (chosen != 0u) {
                gfx_round_fill((s16)(LIST_X + 6), y, (s16)(LIST_WIDTH - 12),
                               ROW_HEIGHT - 4, 9, COLOR_SELECTION);
            }
            gfx_text_size((s16)(LIST_X + 22), (s16)(y + 8), label,
                          (chosen != 0u) ? COLOR_FACE : COLOR_DEEP, 12);
            gfx_text_size_right((s16)(LIST_X + LIST_WIDTH - 20),
                                (s16)(y + 11), size,
                                (chosen != 0u) ? COLOR_FACE : COLOR_DISABLED,
                                9);
            gui_hotspot((s16)(LIST_X + 6), y, (s16)(LIST_WIDTH - 12),
                        ROW_HEIGHT - 4, (u16)(ID_FILE_FIRST + row));
        }

        gui_button(SIDE_X, 74, SIDE_WIDTH, 44, "Open", ID_OPEN);
        gui_button_colored(SIDE_X, 126, SIDE_WIDTH, 44, "Delete", ID_DELETE,
                           COLOR_ALERT, COLOR_FACE);
        gui_button_colored(SIDE_X, 184, SIDE_WIDTH, 40, "Previous page",
                           ID_PREVIOUS_PAGE, COLOR_PANEL, COLOR_DEEP);
        gui_button_colored(SIDE_X, 232, SIDE_WIDTH, 40, "Next page",
                           ID_NEXT_PAGE, COLOR_PANEL, COLOR_DEEP);
        gui_section_label(SIDE_X, 290, "TAP A FILE, THEN OPEN");
    } else {
        /* A plain white text pane, narrower than the list so the scroll
         * buttons to its right never disappear underneath it. */
        gfx_fill(LIST_X, 74, LIST_WIDTH, 280, COLOR_FACE);
        gfx_outline(LIST_X, 74, LIST_WIDTH, 280, COLOR_GRID);
        for (row = 0u; row < TEXT_ROWS; ++row) {
            const u16 wanted = (u16)(text_top + row);
            const s16 y = (s16)(80 + (s16)row * 12);

            fetch_line(wanted);
            gfx_text_size((s16)(LIST_X + 16), y, line, COLOR_DEEP, 10);
        }

        gui_button_colored(SIDE_X, 74, SIDE_WIDTH, 38, "Up", ID_SCROLL_UP,
                           COLOR_PANEL, COLOR_DEEP);
        gui_button_colored(SIDE_X, 118, SIDE_WIDTH, 38, "Down", ID_SCROLL_DOWN,
                           COLOR_PANEL, COLOR_DEEP);
        gui_button_colored(SIDE_X, 174, SIDE_WIDTH, 38, "Page down",
                           ID_SCROLL_PAGE_DOWN, COLOR_PANEL, COLOR_DEEP);
        gui_button_colored(SIDE_X, 218, SIDE_WIDTH, 38, "Page up",
                           ID_SCROLL_PAGE_UP, COLOR_PANEL, COLOR_DEEP);
        gui_button_colored(SIDE_X, 262, SIDE_WIDTH, 38, "Top", ID_SCROLL_TOP,
                           COLOR_PANEL, COLOR_DEEP);
        gui_button(SIDE_X, 306, SIDE_WIDTH, 38, "Close", ID_BACK);
    }

    gui_message_bar(message);
}

static void files_event(const Event *event) {
    u16 row;

    if (event->type == EVENT_WHEEL) {
        const s16 direction = (s16)event->key;

        if (direction > 0) {
            if (viewing != 0u && text_top != 0u) {
                --text_top;
            } else if (viewing == 0u && page != 0u) {
                --page;
            }
        } else if (direction < 0) {
            if (viewing != 0u && text_top + 1u < text_lines) {
                ++text_top;
            } else if (viewing == 0u &&
                       (u16)((page + 1u) * VISIBLE_ROWS) <
                           file_system_visible_count()) {
                ++page;
            }
        }
        return;
    }
    if (event->type != EVENT_CLICK) {
        return;
    }

    if (viewing != 0u) {
        if (event->key == ID_SCROLL_UP && text_top != 0u) {
            --text_top;
        } else if (event->key == ID_SCROLL_DOWN && text_top + 1u < text_lines) {
            ++text_top;
        } else if (event->key == ID_SCROLL_PAGE_UP) {
            text_top = (text_top > (u16)(TEXT_ROWS - 2u))
                           ? (u16)(text_top - (TEXT_ROWS - 2u)) : 0u;
        } else if (event->key == ID_SCROLL_PAGE_DOWN) {
            u16 last = 0u;

            if (text_lines > (u16)TEXT_ROWS) {
                last = (u16)(text_lines - TEXT_ROWS);
            }
            text_top = (u16)(text_top + (TEXT_ROWS - 2u));
            if (text_top > last) {
                text_top = last;
            }
        } else if (event->key == ID_SCROLL_TOP) {
            text_top = 0u;
        } else if (event->key == ID_BACK) {
            viewing = 0u;
            build_message();
        }
        return;
    }

    if (event->key >= ID_FILE_FIRST &&
        event->key < (u16)(ID_FILE_FIRST + VISIBLE_ROWS)) {
        row = (u16)(event->key - ID_FILE_FIRST);
        selected_slot = (u16)(page * VISIBLE_ROWS + row);
        selected_index = file_system_visible(selected_slot);
        sound_beep();
        return;
    }

    if (event->key == ID_OPEN) {
        if (selected_index == FILE_NOT_FOUND) {
            text_copy(message, "Pick a file first", (u16)sizeof(message));
        } else {
            open_selected();
        }
        return;
    }
    if (event->key == ID_DELETE) {
        char name[13];

        if (selected_index == FILE_NOT_FOUND) {
            text_copy(message, "Pick a file first", (u16)sizeof(message));
            return;
        }
        if (file_system_is_protected(selected_index) != 0u) {
            text_copy(message, "KERNEL.BIN is protected", (u16)sizeof(message));
            sound_alert();
            return;
        }
        file_system_name(selected_index, name);
        if (file_system_delete(selected_index) != 0u) {
            message[0] = '\0';
            text_append(message, name, (u16)sizeof(message));
            text_append(message, " hidden until reboot", (u16)sizeof(message));
            selected_index = FILE_NOT_FOUND;
        }
        return;
    }
    if (event->key == ID_PREVIOUS_PAGE && page != 0u) {
        --page;
        return;
    }
    if (event->key == ID_NEXT_PAGE &&
        (u16)((page + 1u) * VISIBLE_ROWS) < file_system_visible_count()) {
        ++page;
    }
}

const Application application_files = {
    "Files",
    "Files",
    icon,
    COLOR_SELECTION,
    files_open,
    files_draw,
    files_event,
    (void (*)(u16))0
};
