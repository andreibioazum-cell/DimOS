/*
 * app_files.c -- the file manager.
 *
 * Shows the files that really are on the boot disk, opens them as text and
 * can hide one of them for the rest of the session. KERNEL.BIN cannot be
 * deleted, and nothing is ever written back to the disk.
 */

#include "dimos.h"

#define ROW_HEIGHT 14
#define VISIBLE_ROWS 8
#define LIST_WIDTH 216
/* 28 columns of 8 pixels plus the scroll buttons on the right just fit the
 * 302 pixel wide window interior. */
#define TEXT_COLUMNS 28
#define TEXT_ROWS 12
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
    "..XXXXXXXXX.....",
    "..XoooooooX.....",
    "..XoooooooXXXXXX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XooooooooooooX",
    "..XXXXXXXXXXXXXX",
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
static char message[40];

static void build_message(void) {
    message[0] = '\0';
    if (viewing != 0u) {
        text_append(message, "Reading a file - use the arrows", (u16)sizeof(message));
        return;
    }
    text_append_number(message, file_system_visible_count(), (u16)sizeof(message));
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

    if (viewing == 0u) {
        for (row = 0u; row < VISIBLE_ROWS; ++row) {
            const u16 slot = (u16)(page * VISIBLE_ROWS + row);
            const u16 index = file_system_visible(slot);
            const s16 y = (s16)(WINDOW_TOP + 2 + (s16)(row * ROW_HEIGHT));
            char label[32];

            if (index == FILE_NOT_FOUND) {
                continue;
            }
            file_system_name(index, label);
            text_pad_right(label, 13u, (u16)sizeof(label));
            text_append_number(label, file_system_size(index), (u16)sizeof(label));
            text_append(label, " bytes", (u16)sizeof(label));

            if (slot == selected_slot) {
                gui_button_colored((s16)(WINDOW_LEFT + 2), y, LIST_WIDTH,
                                   (s16)(ROW_HEIGHT - 2), label,
                                   (u16)(ID_FILE_FIRST + row),
                                   COLOR_SELECTION, COLOR_WHITE);
            } else {
                gui_button((s16)(WINDOW_LEFT + 2), y, LIST_WIDTH,
                           (s16)(ROW_HEIGHT - 2), label,
                           (u16)(ID_FILE_FIRST + row));
            }
        }

        gui_button((s16)(WINDOW_LEFT + LIST_WIDTH + 8), (s16)(WINDOW_TOP + 2), 74, 14,
                   "Open", ID_OPEN);
        gui_button((s16)(WINDOW_LEFT + LIST_WIDTH + 8), (s16)(WINDOW_TOP + 20), 74, 14,
                   "Delete", ID_DELETE);
        gui_button((s16)(WINDOW_LEFT + LIST_WIDTH + 8), (s16)(WINDOW_TOP + 44), 36, 14,
                   "Up", ID_PREVIOUS_PAGE);
        gui_button((s16)(WINDOW_LEFT + LIST_WIDTH + 46), (s16)(WINDOW_TOP + 44), 36, 14,
                   "Down", ID_NEXT_PAGE);
    } else {
        for (row = 0u; row < TEXT_ROWS; ++row) {
            const u16 wanted = (u16)(text_top + row);
            const s16 y = (s16)(WINDOW_TOP + 2 + (s16)(row * 10));

            fetch_line(wanted);
            gfx_text_filled((s16)(WINDOW_LEFT + 2), y, line, COLOR_BLACK,
                            COLOR_TEXT_FIELD);
        }
        gfx_outline((s16)(WINDOW_LEFT + 2), (s16)(WINDOW_TOP + 2),
                    (s16)(TEXT_COLUMNS * 8 + 2), (s16)(TEXT_ROWS * 10 + 2),
                    COLOR_SHADOW);

        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 2), 60, 14, "Up", ID_SCROLL_UP);
        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 20), 60, 14, "Down", ID_SCROLL_DOWN);
        /* The Thunar scrollbar jump: a whole page at once. */
        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 38), 60, 14, "[+]", ID_SCROLL_PAGE_DOWN);
        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 56), 60, 14, "[-]", ID_SCROLL_PAGE_UP);
        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 74), 60, 14, "Top", ID_SCROLL_TOP);
        gui_button((s16)(WINDOW_LEFT + TEXT_COLUMNS * 8 + 10),
                   (s16)(WINDOW_TOP + 94), 60, 16, "Close", ID_BACK);
    }

    gui_message_bar(message);
}

static void files_event(const Event *event) {
    u16 row;

    if (event->type != EVENT_CLICK) {
        return;
    }

    if (viewing != 0u) {
        if (event->key == ID_SCROLL_UP && text_top != 0u) {
            --text_top;
        } else if (event->key == ID_SCROLL_DOWN && text_top + 1u < text_lines) {
            ++text_top;
        } else if (event->key == ID_SCROLL_PAGE_UP) {
            /* A page up lands exactly a screen back, like Thunar's bar. */
            text_top = (text_top > (u16)(TEXT_ROWS - 1u))
                           ? (u16)(text_top - (TEXT_ROWS - 1u)) : 0u;
        } else if (event->key == ID_SCROLL_PAGE_DOWN) {
            u16 last = 0u;

            if (text_lines > (u16)TEXT_ROWS) {
                last = (u16)(text_lines - TEXT_ROWS);
            }
            text_top = (u16)(text_top + (TEXT_ROWS - 1u));
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
    COLOR_YELLOW,
    files_open,
    files_draw,
    files_event,
    (void (*)(u16))0
};
