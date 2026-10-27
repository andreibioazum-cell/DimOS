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
    const s16 y = 66;
    const s16 size = 150;

    /* A pink-to-indigo sleeve: each band is inset inside the rounded
     * corners, so the cover stays one clean shape. */
    gfx_round_fill(x, y, size, size, 22, COLOR_IOS_PINK);
    gfx_round_band_alpha(x, y, size, size, 22, (s16)(size / 3),
                         (s16)(size / 2), COLOR_IOS_PURPLE, 10u);
    gfx_round_band_alpha(x, y, size, size, 22, (s16)(size * 2 / 3),
                         (s16)(size / 2), COLOR_IOS_INDIGO, 9u);
    gfx_glyph((s16)(x + 38), (s16)(y + 38), icon, 16u, 5, COLOR_FACE);
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

    /* The sleeve on the left, the track and the transport on the right,
     * exactly the way a phone shows a song. */
    gfx_text_size(210, 78, "NOW PLAYING", COLOR_DISABLED, 9);
    gfx_text_size(210, 92, track_names[track], COLOR_FACE, 22);
    gfx_text_size(210, 120, track_notes[track], COLOR_DISABLED, 11);

    /* Progress and times. */
    gfx_round_fill(210, 148, 156, 6, 3, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 4));
    gfx_round_fill(210, 148, (s16)(4 + (s32)progress * 148 / 24), 6, 3,
                   COLOR_IOS_PINK);

    time_line[0] = '\0';
    text_append_number(time_line, progress / 60u, (u16)sizeof(time_line));
    text_append_character(time_line, ':', (u16)sizeof(time_line));
    if ((progress % 60u) < 10u) {
        text_append_character(time_line, '0', (u16)sizeof(time_line));
    }
    text_append_number(time_line, progress % 60u, (u16)sizeof(time_line));
    text_copy(right_time, "-0:24", (u16)sizeof(right_time));
    gfx_text_size(210, 160, time_line, COLOR_DISABLED, 9);
    gfx_text_size_right(366, 160, right_time, COLOR_DISABLED, 9);

    /* Transport controls, in a row under the progress bar. */
    gfx_circle(230, 202, 20, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3), 1u);
    gfx_text_size_centered(210, 195, 40, "<<", COLOR_FACE, 10);
    gui_hotspot(210, 182, 40, 40, ID_PREVIOUS);

    gfx_circle(288, 202, 26, COLOR_IOS_PINK, 1u);
    if (playing != 0u) {
        gfx_fill(282, 194, 4, 16, COLOR_FACE);
        gfx_fill(291, 194, 4, 16, COLOR_FACE);
    } else {
        gfx_fill(284, 193, 3, 18, COLOR_FACE);
        gfx_fill(287, 195, 3, 14, COLOR_FACE);
        gfx_fill(290, 197, 3, 10, COLOR_FACE);
        gfx_fill(293, 199, 3, 6, COLOR_FACE);
    }
    gui_hotspot(262, 176, 52, 52, ID_PLAY);

    gfx_circle(346, 202, 20, gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3), 1u);
    gfx_text_size_centered(326, 195, 40, ">>", COLOR_FACE, 10);
    gui_hotspot(326, 182, 40, 40, ID_NEXT);

    /* The three tunes, as full-width list rows under the sleeve. */
    gui_section_label((s16)APP_LEFT, 250, "TUNES");
    for (index = 0u; index < TRACKS; ++index) {
        const s16 y = (s16)(266 + (s16)index * 46);
        const u8 chosen = (u8)(index == track);

        gfx_round_fill((s16)APP_LEFT, y, (s16)APP_WIDTH, 40, 12,
                       (chosen != 0u)
                           ? gfx_shade(COLOR_FACE, COLOR_WIN_BG, 3)
                           : gfx_shade(COLOR_WIN_BG, COLOR_FACE, 2));
        gfx_text_size((s16)(APP_LEFT + 16), (s16)(y + 13), track_names[index],
                      (chosen != 0u) ? COLOR_FACE : COLOR_DISABLED, 12);
        gui_hotspot((s16)APP_LEFT, y, (s16)APP_WIDTH, 40,
                    (u16)(ID_CHIME + index));
    }

    {
        const s16 button_y = (s16)(266 + (s16)TRACKS * 46 + 6);
        const s16 half = (s16)((APP_WIDTH - 10) / 2);

        gui_button_colored((s16)APP_LEFT, button_y, half, 44, "Stop", ID_STOP,
                           COLOR_FACE, COLOR_DEEP);
        gui_section_label((s16)(APP_LEFT + half + 10), (s16)(button_y + 12),
                          "PC SPEAKER ONLY");
        gfx_text_size((s16)APP_LEFT, (s16)(button_y + 58),
                      "Chime, march and waltz are played on the PC speaker,",
                      COLOR_DISABLED, 9);
        gfx_text_size((s16)APP_LEFT, (s16)(button_y + 72),
                      "the way the first computers did it.", COLOR_DISABLED, 9);
    }

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
