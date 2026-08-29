/*
 * app_music.c -- the PC speaker jukebox.
 *
 * Three little tunes, a row of notes to poke, and bars that dance while
 * something is playing. Everything the speaker can do goes through sound.c.
 */

#include "dimos.h"

#define ID_CHIME 1u
#define ID_MARCH 2u
#define ID_WALTZ 3u
#define ID_STOP 4u
#define ID_NOTE_FIRST 10u /* 10 .. 17 */

#define NOTE_KEYS 8u
#define NOTE_KEY_WIDTH 20
#define NOTE_KEY_HEIGHT 58
#define NOTE_LEFT (s16)(WINDOW_LEFT + 136)
#define NOTE_TOP (s16)(WINDOW_TOP + 4)

static const char *const icon[16] = {
    "................",
    "........XXXXXX..",
    "........X...XXX.",
    "........X.....X.",
    "........X.......",
    "........X.......",
    "........X.......",
    "........X.......",
    "........X.......",
    "...XXXXXX.......",
    "..XXXXXXXX......",
    "..XXXXXXXX......",
    "...XXXXXX.......",
    "................",
    "................",
    "................",
};

/* One note, reused whenever a key is poked. */
static Note single_note;

static const u16 note_frequencies[NOTE_KEYS] = {
    262u, 294u, 330u, 349u, 392u, 440u, 494u, 523u
};
static const char *const note_names[NOTE_KEYS] = {
    "C", "D", "E", "F", "G", "A", "B", "C"
};

static char message[40];

static void music_open(void) {
    text_copy(message, "Pick a tune or poke a note", (u16)sizeof(message));
}

static void music_draw(void) {
    u8 index;
    const s16 bars_left = (s16)(WINDOW_LEFT + 4);
    const s16 bars_top = (s16)(WINDOW_TOP + 76);

    gui_button((s16)(WINDOW_LEFT + 4), (s16)(WINDOW_TOP + 4), 122, 18,
               "Chime", ID_CHIME);
    gui_button((s16)(WINDOW_LEFT + 4), (s16)(WINDOW_TOP + 26), 122, 18,
               "March", ID_MARCH);
    gui_button((s16)(WINDOW_LEFT + 4), (s16)(WINDOW_TOP + 48), 122, 18,
               "Waltz", ID_WALTZ);
    gui_button((s16)(WINDOW_LEFT + 4), (s16)(WINDOW_TOP + 70), 122, 18,
               "Stop", ID_STOP);

    for (index = 0u; index < NOTE_KEYS; ++index) {
        const s16 x = (s16)(NOTE_LEFT + (s16)(index * NOTE_KEY_WIDTH));

        gfx_fill(x, NOTE_TOP, (s16)(NOTE_KEY_WIDTH - 2), (s16)NOTE_KEY_HEIGHT,
                 COLOR_TEXT_FIELD);
        gfx_raised_box(x, NOTE_TOP, (s16)(NOTE_KEY_WIDTH - 2),
                       (s16)NOTE_KEY_HEIGHT, 1u);
        gfx_text((s16)(x + 6), (s16)(NOTE_TOP + NOTE_KEY_HEIGHT + 2),
                 note_names[index], COLOR_BLACK);
        gui_hotspot(x, NOTE_TOP, (s16)(NOTE_KEY_WIDTH - 2), (s16)NOTE_KEY_HEIGHT,
                    (u16)(ID_NOTE_FIRST + index));
    }

    /* The bars are driven by the clock, so they move even between tunes. */
    for (index = 0u; index < 24u; ++index) {
        const s16 x = (s16)(bars_left + (s16)(index * 12));
        const u32 phase = (u32)((time_milliseconds() / 90u) + index * 7u) % 23u;
        const u16 height = (sound_is_playing() != 0u)
                               ? (u16)(4u + (phase % 19u))
                               : 3u;

        gfx_fill(x, (s16)(bars_top + 34 - (s16)height), 10, (s16)height,
                 (index % 3u == 0u) ? COLOR_LIGHT_RED
                                    : ((index % 3u == 1u) ? COLOR_YELLOW
                                                          : COLOR_LIGHT_GREEN));
    }
    gfx_horizontal_line(bars_left, (s16)(bars_top + 34), 288, COLOR_SHADOW);

    gui_message_bar(message);
}

static void music_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }

    if (event->key >= ID_NOTE_FIRST &&
        event->key < (u16)(ID_NOTE_FIRST + NOTE_KEYS)) {
        single_note.hertz = note_frequencies[event->key - ID_NOTE_FIRST];
        single_note.ticks = 12u;
        sound_play(&single_note, 1u, 0u);
        text_copy(message, "Note played", (u16)sizeof(message));
        return;
    }

    switch (event->key) {
        case ID_CHIME:
            sound_play_startup();
            text_copy(message, "Playing: chime", (u16)sizeof(message));
            break;
        case ID_MARCH:
            sound_play_march();
            text_copy(message, "Playing: march", (u16)sizeof(message));
            break;
        case ID_WALTZ:
            sound_play_waltz();
            text_copy(message, "Playing: waltz", (u16)sizeof(message));
            break;
        case ID_STOP:
            sound_stop();
            text_copy(message, "Stopped", (u16)sizeof(message));
            break;
        default:
            return;
    }
}

const Application application_music = {
    "TUN",
    "Music",
    icon,
    COLOR_YELLOW,
    music_open,
    music_draw,
    music_event,
    0
};
