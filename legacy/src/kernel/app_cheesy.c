/*
 * app_cheesy.c -- cheese balls with ketchup. Ам-ням-ням!
 *
 * A full cheese ball kitchen in phone style: a big bowl of freshly fried
 * balls with a ketchup drip, one hungry NYAM button, a splash of ketchup
 * drops on every chomp, a burp-o-meter that proudly fills up, and a
 * cook-on-demand supply of more balls.
 */

#include "dimos.h"

#define ID_NOM 1u
#define ID_COOK 2u

#define BALLS_ON_PLATE 9u          /* 4 + 3 + 2, stacked in the bowl     */
#define BURP_AT 12u                /* balls between two mighty burps     */
#define DOUGH_LIMIT 240u           /* the kitchen is not infinite        */
#define DROP_COUNT 12u
#define BALL_TOP_Y 118
#define BALL_RADIUS 20
#define BOWL_CENTER_X 256
#define PANEL_X (s16)(APP_LEFT + 470)

static const s16 ball_x[BALLS_ON_PLATE] = {
    196, 236, 276, 316,
    216, 256, 296,
    236, 276
};
static const s16 ball_y[BALLS_ON_PLATE] = {
    206, 206, 206, 206,
    168, 168, 168,
    132, 132
};

static const char *const icon[16] = {
    "................",
    "................",
    ".....XXX.XXX....",
    "....XXXX.XXXX...",
    ".....XXX.XXX....",
    "................",
    "..XXXXXXXXXXXX..",
    "..XooooooooooX..",
    "..XooooooooooX..",
    "..XooooooooooX..",
    "..XooooooooooX..",
    "...XooooooooX...",
    "....XXXXXXXX....",
    "................",
    "................",
    "................",
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
        /* Drops start just above the bowl, so they never cross the caption
         * that sits along the top of the card. */
        drops_x[index] = (u8)(200u + (next_random() % 110u));
        drops_y[index] = (u8)(164u + (next_random() % 20u));
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

static void draw_ball(s16 x, s16 y, u8 hot) {
    gfx_circle(x, y, BALL_RADIUS, COLOR_ACCENT, 1u);
    gfx_circle((s16)(x - 6), (s16)(y - 7), 7,
               gfx_shade(COLOR_ACCENT, COLOR_FACE, 6), 1u);
    if (hot != 0u) {
        gfx_circle(x, (s16)(y + 2), (s16)(BALL_RADIUS - 4),
                   gfx_shade(COLOR_ACCENT, COLOR_ALERT, 4), 1u);
    }
    /* A dot of ketchup on top of every ball. */
    gfx_circle((s16)(x + 5), (s16)(y - 9), 5, COLOR_ALERT, 1u);
}

static void cheesy_draw(void) {
    const u32 now = time_milliseconds();
    char message[48];
    char buffer[16];
    s16 shake = 0;
    u16 index;

    gui_window_frame("Cheesy Balls");

    /* The counter: a warm rounded card, a bowl, and the pile of balls. */
    gfx_round_fill((s16)APP_LEFT, 74, 470, 274, 22, COLOR_PANEL);
    gfx_text_size((s16)(APP_LEFT + 24), 88,
                  "Fresh cheese balls with ketchup", COLOR_DEEP, 12);

    /* The burp-o-meter: twelve lamps above the bowl. */
    for (index = 0u; index < BURP_AT; ++index) {
        gfx_circle((s16)(APP_LEFT + 30 + (s16)index * 18), 114, 5,
                   (index < streak) ? COLOR_ACCENT : COLOR_GRID, 1u);
    }

    if (now < shake_until) {
        shake = ((now / 50u) % 2u == 0u) ? 1 : -1;
        gfx_text_size_right((s16)(APP_LEFT + 446), 110, "BURRRP!",
                            COLOR_ALERT, 13);
    }

    /* The cast iron bowl. */
    gfx_ellipse_fill((s16)(BOWL_CENTER_X + shake), 250, 196, 62, COLOR_DEEP);
    gfx_ellipse_fill((s16)(BOWL_CENTER_X + shake), 242, 190, 52,
                     gfx_shade(COLOR_DEEP, COLOR_FACE, 3));

    if (plate == 0u) {
        gfx_text_size_centered((s16)(BOWL_CENTER_X - 150), 170, 300,
                               "the bowl is empty...", COLOR_DISABLED, 14);
        gfx_text_size_centered((s16)(BOWL_CENTER_X - 150), 198, 300,
                               "press Cook more", COLOR_DEEP, 12);
    } else {
        for (index = 0u; index < plate; ++index) {
            const u16 phase = (u16)(((now / 170u) + (u32)index * 2u) % 6u);
            const s16 bob = (phase < 3u) ? (s16)phase : (s16)(5u - phase);

            draw_ball((s16)(ball_x[index] + shake),
                      (s16)(ball_y[index] - bob),
                      (u8)(index + 1u == plate));
        }
        /* The ketchup drip running down the top ball. */
        gfx_fill((s16)(ball_x[plate - 1u] + 2 + shake),
                 (s16)(ball_y[plate - 1u] - 10), 5, 9, COLOR_ALERT);
        gfx_circle((s16)(ball_x[plate - 1u] + 4 + shake),
                   (s16)(ball_y[plate - 1u] + 4), 4, COLOR_ALERT, 1u);
    }

    for (index = 0u; index < DROP_COUNT; ++index) {
        if (drops_speed[index] != 0u) {
            gfx_circle(drops_x[index], drops_y[index], 2,
                       (index % 2u == 0u) ? COLOR_ALERT : COLOR_IOS_PINK, 1u);
        }
    }

    if (now < chew_until) {
        gfx_text_size_right((s16)(APP_LEFT + 446), 132, "OM NOM!",
                            COLOR_ALERT, 13);
    } else {
        gfx_text_size((s16)(APP_LEFT + 24), 132,
                      yum_phrases[phrase_index % 8u], COLOR_ACCENT, 10);
    }

    /* The right-hand side: the big hungry controls. */
    gfx_text_size(PANEL_X, 74, "CHEESY BALLS", COLOR_DISABLED, 9);
    gfx_text_size(PANEL_X, 88, "Kitchen", COLOR_DEEP, 24);

    gfx_text_size(PANEL_X, 140, "BALLS IN BOWL", COLOR_DISABLED, 9);
    buffer[0] = '\0';
    text_append_number(buffer, plate, (u16)sizeof(buffer));
    gfx_text_size(PANEL_X, 152, buffer, COLOR_DEEP, 30);

    gfx_text_size((s16)(PANEL_X + 120), 140, "BURPS", COLOR_DISABLED, 9);
    buffer[0] = '\0';
    text_append_number(buffer, burps, (u16)sizeof(buffer));
    gfx_text_size((s16)(PANEL_X + 120), 152, buffer, COLOR_DEEP, 30);

    gui_button(PANEL_X, 200, 250, 56, "NYAM!", ID_NOM);
    gui_button_colored(PANEL_X, 266, 250, 44, "Cook more", ID_COOK,
                       COLOR_PANEL, COLOR_DEEP);

    gui_message_bar("Hold on: every chomp splashes ketchup");

    build_message(message, (u16)sizeof(message));
    (void)message;
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
    COLOR_YELLOW,
    cheesy_open,
    cheesy_draw,
    cheesy_event,
    cheesy_update
};
