/*
 * app_cheesy.c -- cheese balls with ketchup. Ам-ням-ням!
 *
 * The DimXfce mouse insisted: a shell with a rodent logo cannot ship
 * without cheese. So here is a full cheese ball kitchen: a bowl of freshly
 * fried balls with a ketchup drip, one hungry [NYAM!] button, a splash of
 * ketchup drops on every chomp, a burp-o-meter that proudly fills up, and
 * a cook-on-demand supply of more balls. All served in 320x200.
 */

#include "dimos.h"

#define ID_NOM 1u
#define ID_COOK 2u

#define BALL_ROWS 3u
#define BALLS_ON_PLATE 9u          /* 4 + 3 + 2, stacked in the bowl     */
#define BURP_AT 12u                /* balls between two mighty burps     */
#define DOUGH_LIMIT 240u           /* the kitchen is not infinite        */
#define DROP_COUNT 12u

/* Where the ball pile sits, in screen pixels: the bottom row of four,
 * then three, then two. Ball art is 13 pixels across. */
#define BALL_TOP_Y 118
static const s16 ball_x[BALLS_ON_PLATE] = {
    121, 135, 149, 163,
    128, 142, 156,
    135, 149
};
static const s16 ball_y[BALLS_ON_PLATE] = {
    104, 104, 104, 104,
    94, 94, 94,
    84, 84
};

static const char *const icon[16] = {
    "................",
    ".....XXXXXX.....",
    "...XXXXXXXXXX...",
    "..XXXXXXXXXXXX..",
    ".XXoXXXXXXXXXX..",
    ".XXXXXXXXXXXXX..",
    ".XXXXXXXXoXXXX..",
    ".XXXXXXXXXXXXXX.",
    ".XXXXXXXXXXXX.XX",
    "..XXXXXXXXXXXX..",
    "..XXXXXXXXXXXX..",
    "...XXXXXXXXXX...",
    ".....XXXXXX.....",
    "................",
    "................",
    "................",
};

/* A single crispy ball. */
static const char *const ball_art[12] = {
    ".....XXX.....",
    "...XXXXXXX...",
    "..XXXXXXXXX..",
    ".XXXXXXXXXXX.",
    "XXXXXXXXXXXXX",
    "XXXXXXXXXXXXX",
    "XXoXXXXXXXXXX",
    "XXXXXXXXXXXXX",
    ".XXXXXXXXXXX.",
    "..XXXXXXXXX..",
    "...XXXXXXX...",
    ".....XXX....."
};

/* The cast iron bowl with the golden rim every cheese kitchen deserves. */
static const char *const bowl_art[12] = {
    "XXooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooXX",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "...XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX...",
    "....XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX....",
    ".....XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.....",
    ".......XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.......",
    ".........XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.........",
    "...........XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX...........",
    ".............XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.............",
    "...............XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX...............",
    ".................XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.................",
    "...................XXXXXXXXXXXXXXXXXXXXXXXXXX..................."
};

/* A thin drip of ketchup over the top ball: the chef's signature. */
static const char *const ketchup_drip[6] = {
    "..XXXXX..",
    ".XXXXXXX.",
    "XXXX.XXXX",
    "XX....XXX",
    "X......XX",
    ".......XX"
};

/* The burp-o-meter: one lamp per ball since the last burp. */
static const char *const lamp_art[6] = {
    "..XXXX..",
    ".XXXXXX.",
    "XXXXXXXX",
    "XXXXXXXX",
    ".XXXXXX.",
    "..XXXX.."
};

static const char *const yum_phrases[] = {
    "Am-nyam-nyam!",
    "Crunch!",
    "So cheesy!",
    "Ketchup love!",
    "One more!",
    "Hot and golden!",
    "Chef is proud!",
    "Nyam. NYAM!"
};

static u16 plate;            /* balls still sitting in the bowl        */
static u32 eaten_total;
static u32 cooked_total;
static u16 streak;           /* progress towards the next burp         */
static u32 burps;
static u32 chew_until;       /* the nom-nom face shows until this time */
static u32 shake_until;      /* the burp shakes the bowl until then    */
static u16 phrase_index;
static u32 random_state;
static u8 drops_x[DROP_COUNT];
static u8 drops_y[DROP_COUNT];
static u8 drops_speed[DROP_COUNT];

static u32 next_random(void) {
    random_state = random_state * 1103515245u + 12345u;
    return (random_state >> 16u) & 0x7FFFu;
}

static void cheesy_open(void) {
    plate = BALLS_ON_PLATE;
    if (cooked_total == 0u) {
        cooked_total = BALLS_ON_PLATE;
    }
    streak = 0u;
    chew_until = 0u;
    shake_until = 0u;
    phrase_index = 0u;
    random_state = (time_milliseconds() | 1u);
    memory_zero(drops_x, (u32)sizeof(drops_x));
    memory_zero(drops_y, (u32)sizeof(drops_y));
    memory_zero(drops_speed, (u32)sizeof(drops_speed));
}

static void ketchup_splash(void) {
    u8 index;

    for (index = 0u; index < DROP_COUNT; ++index) {
        drops_x[index] = (u8)(130u + (next_random() % 52u));
        drops_y[index] = (u8)(74u + (next_random() % 20u));
        drops_speed[index] = (u8)(1u + (next_random() % 3u));
    }
}

static void cheesy_update(u16 elapsed_ms) {
    u8 index;
    u16 fall = (u16)((elapsed_ms + 20u) / 40u); /* pixels per frame, ~1.. */

    for (index = 0u; index < DROP_COUNT; ++index) {
        if (drops_speed[index] != 0u) {
            u16 y = (u16)(drops_y[index] + fall * drops_speed[index]);

            if (y > (u16)(BALL_TOP_Y + 8u)) {
                drops_speed[index] = 0u; /* a drop that lands in the bowl */
            } else {
                drops_y[index] = (u8)y;
            }
        }
    }
}

static void build_message(char *out, u16 capacity) {
    out[0] = '\0';
    text_append(out, "Eaten: ", capacity);
    text_append_number(out, eaten_total, capacity);
    text_append(out, "  burp in ", capacity);
    text_append_number(out, (u32)(BURP_AT - streak), capacity);
}

static void cheesy_draw(void) {
    const u32 now = time_milliseconds();
    char message[40];
    s16 shake = 0;
    u16 index;

    gfx_text((s16)(WINDOW_LEFT + 10), (s16)(WINDOW_TOP + 4),
             "Fresh cheese balls with ketchup!", COLOR_BLACK);

    /* The burp-o-meter: twelve lamps above the bowl. */
    for (index = 0u; index < BURP_AT; ++index) {
        gfx_picture((s16)(97 + (s16)index * 11), (s16)(WINDOW_TOP + 16),
                    lamp_art, 6u,
                    (index < streak) ? COLOR_ACCENT : COLOR_DARK_GRAY,
                    COLOR_DARK_GRAY);
    }

    if (now < shake_until) {
        shake = ((now / 50u) % 2u == 0u) ? 1 : -1;
        gfx_text((s16)(WINDOW_LEFT + 4), (s16)(WINDOW_TOP + 34),
                 "BURRRP!", COLOR_ALERT);
    }

    if (plate == 0u) {
        gfx_text((s16)(122), (s16)(WINDOW_TOP + 44), "the bowl is empty...",
                 COLOR_DARK_GRAY);
        gfx_text((s16)(114), (s16)(WINDOW_TOP + 56), "press [Cook more]!",
                 COLOR_BLACK);
    } else {
        /* The pile: every ball bobs gently, as every hot ball should. */
        for (index = 0u; index < plate; ++index) {
            const u16 phase = (u16)(((now / 170u) + (u32)index * 2u) % 6u);
            const s16 bob = (phase < 3u) ? (s16)phase : (s16)(5u - phase);

            gfx_picture((s16)(ball_x[index] + shake),
                        (s16)(ball_y[index] - 2 - bob),
                        ball_art, 12u, COLOR_ACCENT, COLOR_LIGHT_RED);
        }
        /* Ketchup drip on the top ball, and the falling drops. */
        gfx_picture((s16)(ball_x[plate - 1] + 3 + shake),
                    (s16)(ball_y[plate - 1] + 1),
                    ketchup_drip, 6u, COLOR_LIGHT_RED, COLOR_LIGHT_RED);
        for (index = 0u; index < DROP_COUNT; ++index) {
            if (drops_speed[index] != 0u) {
                gfx_fill((s16)drops_x[index], (s16)drops_y[index], 2, 3,
                         (index % 2u == 0u) ? COLOR_ALERT : COLOR_LIGHT_RED);
            }
        }
    }

    gfx_picture((s16)(121 + shake), (s16)(BALL_TOP_Y), bowl_art, 12u,
                COLOR_DARK_GRAY, COLOR_ACCENT);
    gfx_horizontal_line(101, (s16)(BALL_TOP_Y + 13), 104, COLOR_SHADOW);

    /* The nom-nom face: while the chew timer runs, big letters say it. */
    if (now < chew_until) {
        gfx_text(138, (s16)(WINDOW_TOP + 22), "OM NOM!", COLOR_ALERT);
        phrase_index = (u16)(phrase_index % 8u);
    } else {
        gfx_text_centered(110, (s16)(WINDOW_TOP + 22), 100,
                          yum_phrases[phrase_index % 8u], COLOR_DEEP);
    }

    gui_button((s16)(WINDOW_LEFT + 8), (s16)(WINDOW_TOP + 102), 92, 16,
               "NYAM!", ID_NOM);
    gui_button((s16)(WINDOW_LEFT + 202), (s16)(WINDOW_TOP + 102), 92, 16,
               "Cook more", ID_COOK);

    build_message(message, (u16)sizeof(message));
    gui_message_bar(message);
}

static void eat_ball(void) {
    if (plate == 0u) {
        /* An empty bowl only echoes. */
        sound_alert();
        return;
    }
    --plate;
    ++eaten_total;
    ++streak;
    ketchup_splash();
    phrase_index = (u16)((phrase_index + 1u) % 8u);
    chew_until = time_milliseconds() + 420u;
    sound_play_nom();

    if (streak == BURP_AT) {
        streak = 0u;
        ++burps;
        shake_until = time_milliseconds() + 900u;
        sound_play_burp();
    }
}

static void cook_batch(void) {
    if (plate != 0u) {
        /* Still balls left; the cook shrugs. */
        chew_until = time_milliseconds() + 300u;
        return;
    }
    if (cooked_total >= DOUGH_LIMIT) {
        /* The dough is gone. This kitchen lives dangerously but not
         * recklessly: the cook starts a new dough ball on the house. */
        cooked_total = 0u;
        eaten_total = 0u;
        burps = 0u;
    }
    cooked_total = (u32)(cooked_total + BALLS_ON_PLATE);
    plate = BALLS_ON_PLATE;
    chew_until = 0u;
    sound_beep();
}

static void cheesy_event(const Event *event) {
    if (event->type != EVENT_CLICK) {
        return;
    }
    if (event->key == ID_NOM) {
        eat_ball();
    } else if (event->key == ID_COOK) {
        cook_batch();
    }
}

const Application application_cheesy = {
    "Cheesy",
    "Cheesy Balls",
    icon,
    COLOR_ACCENT,
    cheesy_open,
    cheesy_draw,
    cheesy_event,
    cheesy_update
};
