/*
 * app_music.c -- a tiny jukebox dressed as a phone music player.
 *
 * Three little tunes for the PC speaker, a large album cover made of
 * rounded shapes, playback controls and a progress bar that follows the
 * clock while something is playing. Everything the speaker can do goes
 * through sound.c.
 */

#include "dimos.h"

#define ID_PLAY 1u
#define ID_PREVIOUS 2u
#define ID_NEXT 3u
#define ID_CHIME 10u
#define ID_MARCH 11u
#define ID_WALTZ 12u
#define ID_STOP 13u

#define TRACKS 3u

static const char *const icon[16] = {
    "................",
    "..........XXX...",
    "..........X.X...",
    "..........X.XXX.",
    "..........X.X.X.",
    "..........XX..X.",
    "..........X...X.",
    "..........X...X.",
    "......XXXXX...X.",
    "....XXXXXXX...X.",
    "...XXXXXXX....X.",
    "...XXXXXXX...XX.",
    "...XXXXXXX..XXX.",
    "....XXXXX..XXX..",
    ".....XXX..XXX...",
    "................",
};

static const char *const track_names[TRACKS] = {
    "Chime", "March", "Waltz"
};

static const char *const track_notes[TRACKS] = {
    "A soft boot chime", "A tiny parade", "Three-four time"
};

static u8 track;
static u32 track_started;

static void music_open(void) {
    track = 0u;
    track_started = time_milliseconds();
}

static void play_track(u8 index) {
    track = (u8)(index % TRACKS);
    track_started = time_milliseconds();
    if (track == 0u) {
        sound_play_startup();
    } else if (track == 1u) {
        sound_play_march();
    } else {
        sound_play_waltz();
    }
}

/* The progress bar and the little level meters animate, so ask the shell
 * for a fresh frame while the song is playing. */
static void music_update(u16 elapsed_ms) {
    (void)elapsed_ms;
}

static void draw_album_art(void) {
    const s16 x = 40;
    const s16 y = 76;
    const s16 size = 200;

    /* A pink-to-indigo sleeve: each band is inset inside the rounded
     * corners, so the cover stays one clean shape. */
    gfx_round_fill(x, y, size, size, 22, COLOR_IOS_PINK);
    gfx_round_band_alpha(x, y, size, size, 22, (s16)(size / 3),
                         (s16)(size / 2), COLOR_IOS_PURPLE, 10u);
    gfx_round_band_alpha(x, y, size, size, 22, (s16)(size * 2 / 3),
                         (s16)(size / 2), COLOR_IOS_INDIGO, 9u);
    gfx_glyph((s16)(x + 50), (s16)(y + 50), icon, 16u, 6, COLOR_FACE);
}

static void music_draw(void) {
    const u32 elapsed = (time_milliseconds() - track_started) / 1000u;
    const u32 progress = elapsed % 24u;
    const u8 playing = sound_is_playing();
    char time_line[16];
    char right_time[16];
    u8 index;

    gui_window_frame_dark("Music", COLOR_WIN_BG);

    draw_album_art();

    gfx_text_size(272, 80, "NOW PLAYING", COLOR_DISABLED, 9);
    gfx_text_size(272, 96, track_names[track], COLOR_FACE, 26);
    gfx_text_size(272, 128, track_notes[track], COLOR_DISABLED, 11);

    /* Progress and times. */
    gfx_round_fill(272, 158, 380, 6, 3, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 4));
    gfx_round_fill(272, 158, (s16)(4 + (s32)progress * 372 / 24), 6, 3,
                   COLOR_IOS_PINK);

    time_line[0] = '\0';
    text_append_number(time_line, progress / 60u, (u16)sizeof(time_line));
    text_append_character(time_line, ':', (u16)sizeof(time_line));
    if ((progress % 60u) < 10u) {
        text_append_character(time_line, '0', (u16)sizeof(time_line));
    }
    text_append_number(time_line, progress % 60u, (u16)sizeof(time_line));
    text_copy(right_time, "-0:24", (u16)sizeof(right_time));
    gfx_text_size(272, 170, time_line, COLOR_DISABLED, 9);
    gfx_text_size_right((s16)(272 + 380), 170, right_time, COLOR_DISABLED, 9);

    /* Transport controls. */
    gfx_circle(312, 224, 22, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3), 1u);
    gfx_text_size_centered(290, 216, 44, "<<", COLOR_FACE, 11);
    gui_hotspot(290, 202, 44, 44, ID_PREVIOUS);

    gfx_circle(404, 224, 32, COLOR_IOS_PINK, 1u);
    if (playing != 0u) {
        gfx_fill(396, 214, 5, 20, COLOR_FACE);
        gfx_fill(407, 214, 5, 20, COLOR_FACE);
    } else {
        gfx_fill(397, 213, 4, 22, COLOR_FACE);
        gfx_fill(401, 215, 4, 18, COLOR_FACE);
        gfx_fill(405, 217, 4, 14, COLOR_FACE);
        gfx_fill(409, 219, 4, 10, COLOR_FACE);
        gfx_fill(413, 221, 4, 6, COLOR_FACE);
    }
    gui_hotspot(372, 192, 64, 64, ID_PLAY);

    gfx_circle(496, 224, 22, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3), 1u);
    gfx_text_size_centered(474, 216, 44, ">>", COLOR_FACE, 11);
    gui_hotspot(474, 202, 44, 44, ID_NEXT);

    /* The three tunes, as list rows. */
    for (index = 0u; index < TRACKS; ++index) {
        const s16 y = (s16)(268 + (s16)index * 30);
        const u8 chosen = (u8)(index == track);

        if (chosen != 0u) {
            gfx_round_fill(272, y, 300, 26, 8,
                           gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3));
        }
        gfx_text_size((s16)(286), (s16)(y + 7), track_names[index],
                      (chosen != 0u) ? COLOR_FACE : COLOR_DISABLED, 11);
        gui_hotspot(272, y, 300, 26, (u16)(ID_CHIME + index));
    }

    gui_button_colored(620, 268, 158, 30, "Stop", ID_STOP, COLOR_PANEL,
                       COLOR_DEEP);
    gfx_text_size(620, 308, "PC speaker only", COLOR_DISABLED, 9);
    gfx_text_size(620, 322, "Chime, march, waltz", COLOR_DISABLED, 9);

    gui_message_bar("Tap a tune, or use the big play button");
}

static void music_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }
    switch (event->key) {
        case ID_PLAY:
            if (sound_is_playing() != 0u) {
                sound_stop();
            } else {
                play_track(track);
            }
            break;
        case ID_PREVIOUS:
            play_track((u8)((track + TRACKS - 1u) % TRACKS));
            break;
        case ID_NEXT:
            play_track((u8)((track + 1u) % TRACKS));
            break;
        case ID_STOP:
            sound_stop();
            break;
        default:
            if (event->key >= ID_CHIME &&
                event->key < (u16)(ID_CHIME + TRACKS)) {
                play_track((u8)(event->key - ID_CHIME));
            }
            return;
    }
}

const Application application_music = {
    "Music",
    "Music",
    icon,
    COLOR_IOS_PINK,
    music_open,
    music_draw,
    music_event,
    music_update
};
