/*
 * sound.c -- the PC speaker.
 *
 * The speaker is one bit of port 0x61. Feeding timer channel 2 with a square
 * wave and switching that bit on makes a tone, so a song is nothing more than
 * a list of "this frequency for this many ticks".
 */

#include "dimos.h"

#define SPEAKER_TIMER_GATE 0x01u
#define SPEAKER_ENABLE 0x02u
#define TIMER_CHANNEL_2_SQUARE_WAVE 0xB6u
#define SPEAKER_INPUT_FREQUENCY 1193182u

/* Note names, so a melody in the source reads like a melody. */
#define REST 0u
#define NOTE_C4 262u
#define NOTE_D4 294u
#define NOTE_E4 330u
#define NOTE_F4 349u
#define NOTE_G4 392u
#define NOTE_A4 440u
#define NOTE_B4 494u
#define NOTE_C5 523u
#define NOTE_D5 587u
#define NOTE_E5 659u
#define NOTE_F5 698u
#define NOTE_G5 784u
#define NOTE_A5 880u
#define NOTE_C6 1047u

static u8 speaker_state;
static const Note *current_song;
static u16 song_length;
static u16 song_position;
static u16 ticks_left_in_note;
static u8 song_repeats;
static u8 playing;

static void speaker_off(void) {
    speaker_state = (u8)(speaker_state & (u8)~(SPEAKER_TIMER_GATE | SPEAKER_ENABLE));
    port_write_byte(PORT_SPEAKER, speaker_state);
}

static void speaker_tone(u16 hertz) {
    u16 divisor;

    if (hertz < 20u) {
        speaker_off();
        return;
    }

    divisor = (u16)(SPEAKER_INPUT_FREQUENCY / hertz);
    port_write_byte(PORT_PIT_COMMAND, TIMER_CHANNEL_2_SQUARE_WAVE);
    port_write_byte(PORT_PIT_CHANNEL_2, (u8)(divisor & 0xFFu));
    port_write_byte(PORT_PIT_CHANNEL_2, (u8)(divisor >> 8u));

    /* Keep whatever else the system had set in this port. */
    speaker_state = (u8)(port_read_byte(PORT_SPEAKER) &
                         (u8)~(SPEAKER_TIMER_GATE | SPEAKER_ENABLE));
    speaker_state = (u8)(speaker_state | SPEAKER_TIMER_GATE | SPEAKER_ENABLE);
    port_write_byte(PORT_SPEAKER, speaker_state);
}

void sound_init(void) {
    speaker_state = (u8)(port_read_byte(PORT_SPEAKER) &
                         (u8)~(SPEAKER_TIMER_GATE | SPEAKER_ENABLE));
    port_write_byte(PORT_SPEAKER, speaker_state);
    current_song = (const Note *)0;
    song_length = 0u;
    song_position = 0u;
    ticks_left_in_note = 0u;
    song_repeats = 0u;
    playing = 0u;
}

void sound_play(const Note *notes, u16 count, u8 repeat) {
    if (notes == (const Note *)0 || count == 0u) {
        sound_stop();
        return;
    }
    current_song = notes;
    song_length = count;
    song_position = 0u;
    song_repeats = repeat;
    playing = 1u;
    speaker_tone(notes[0].hertz);
    ticks_left_in_note = notes[0].ticks;
}

void sound_stop(void) {
    playing = 0u;
    current_song = (const Note *)0;
    song_length = 0u;
    song_position = 0u;
    ticks_left_in_note = 0u;
    speaker_off();
}

u8 sound_is_playing(void) {
    return playing;
}

void sound_update(u16 ticks) {
    if (playing == 0u || current_song == (const Note *)0) {
        return;
    }
    if (ticks_left_in_note > ticks) {
        ticks_left_in_note = (u16)(ticks_left_in_note - ticks);
        return;
    }

    ++song_position;
    if (song_position >= song_length) {
        if (song_repeats == 0u) {
            sound_stop();
            return;
        }
        song_position = 0u;
    }
    speaker_tone(current_song[song_position].hertz);
    ticks_left_in_note = current_song[song_position].ticks;
}

/* ------------------------------------------------------------------ */
/* The tunes                                                           */
/* ------------------------------------------------------------------ */

static const Note startup_chime[] = {
    { NOTE_C5, 5u }, { NOTE_E5, 5u }, { NOTE_G5, 5u }, { NOTE_C6, 14u },
};

static const Note march[] = {
    { NOTE_G4, 6u }, { NOTE_G4, 3u }, { NOTE_G4, 6u }, { NOTE_D4, 4u },
    { NOTE_G4, 8u }, { NOTE_B4, 8u }, { NOTE_C5, 6u }, { NOTE_B4, 4u },
    { NOTE_A4, 8u }, { NOTE_G4, 8u }, { NOTE_E4, 6u }, { NOTE_G4, 10u },
    { REST, 4u },
};

static const Note waltz[] = {
    { NOTE_C5, 9u }, { NOTE_E5, 3u }, { NOTE_G5, 6u },
    { NOTE_A5, 9u }, { NOTE_G5, 3u }, { NOTE_E5, 6u },
    { NOTE_F5, 9u }, { NOTE_D5, 3u }, { NOTE_F5, 6u },
    { NOTE_E5, 12u }, { REST, 3u },
};

static const Note bleep[] = {
    { NOTE_A5, 3u },
};

static const Note two_tone[] = {
    { NOTE_E5, 4u }, { NOTE_A4, 6u },
};

void sound_play_startup(void) {
    sound_play(startup_chime, (u16)(sizeof(startup_chime) / sizeof(startup_chime[0])), 0u);
}

void sound_play_march(void) {
    sound_play(march, (u16)(sizeof(march) / sizeof(march[0])), 0u);
}

void sound_play_waltz(void) {
    sound_play(waltz, (u16)(sizeof(waltz) / sizeof(waltz[0])), 0u);
}

void sound_beep(void) {
    sound_play(bleep, 1u, 0u);
}

void sound_alert(void) {
    sound_play(two_tone, 2u, 0u);
}
