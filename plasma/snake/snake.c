/*
 * Snake for the DimOS live system.
 * Plain C (C11, POSIX termios + ANSI escapes): no ncurses, no extra libraries.
 * Keys: arrows or W A S D to move, R to restart after game over, Q to quit.
 */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define W 40
#define H 20
#define MAXLEN (W * H)

static struct termios orig_term;
static int sx[MAXLEN], sy[MAXLEN];
static int len, dx, dy, fx, fy, score, over;

static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
    printf("\033[0m\033[?25h\033[2J\033[H");
    fflush(stdout);
}

static void enable_raw_terminal(void)
{
    struct termios t;

    if (tcgetattr(STDIN_FILENO, &orig_term) != 0)
        return;
    t = orig_term;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    printf("\033[?25l\033[2J");
    fflush(stdout);
    atexit(restore_terminal);
}

static int snake_at(int x, int y)
{
    for (int i = 0; i < len; i++)
        if (sx[i] == x && sy[i] == y)
            return 1;
    return 0;
}

static void place_food(void)
{
    do {
        fx = rand() % W;
        fy = rand() % H;
    } while (snake_at(fx, fy));
}

static void new_game(void)
{
    len = 3;
    dx = 1;
    dy = 0;
    score = 0;
    over = 0;
    for (int i = 0; i < len; i++) {
        sx[i] = W / 2 - i;
        sy[i] = H / 2;
    }
    place_food();
}

static void draw(void)
{
    printf("\033[H+");
    for (int x = 0; x < W; x++)
        putchar('-');
    printf("+\r\n");
    for (int y = 0; y < H; y++) {
        putchar('|');
        for (int x = 0; x < W; x++) {
            char c = ' ';
            if (x == fx && y == fy)
                c = '*';
            for (int i = 0; i < len; i++) {
                if (sx[i] == x && sy[i] == y) {
                    c = (i == 0) ? '@' : 'o';
                    break;
                }
            }
            putchar(c);
        }
        printf("|\r\n");
    }
    printf("+");
    for (int x = 0; x < W; x++)
        putchar('-');
    printf("+\r\n");
    printf("Score: %d   Arrows/WASD: move   Q: quit   %s\033[K\r\n",
           score, over ? "GAME OVER - press R" : "");
    fflush(stdout);
}

static void step(void)
{
    int nx = sx[0] + dx;
    int ny = sy[0] + dy;
    int eat;

    if (nx < 0 || nx >= W || ny < 0 || ny >= H) {
        over = 1;
        return;
    }
    eat = (nx == fx && ny == fy);
    /* The tail moves away on a normal step, so it is not a collision. */
    for (int i = 0; i < (eat ? len : len - 1); i++) {
        if (sx[i] == nx && sy[i] == ny) {
            over = 1;
            return;
        }
    }
    if (eat && len < MAXLEN)
        len++;
    for (int i = len - 1; i > 0; i--) {
        sx[i] = sx[i - 1];
        sy[i] = sy[i - 1];
    }
    sx[0] = nx;
    sy[0] = ny;
    if (eat) {
        score++;
        place_food();
    }
}

static void read_input(void)
{
    unsigned char c;
    unsigned char a, b;

    while (read(STDIN_FILENO, &c, 1) == 1) {
        int ndx = dx, ndy = dy;

        if (c == 27) {
            if (read(STDIN_FILENO, &a, 1) == 1 && read(STDIN_FILENO, &b, 1) == 1 && a == '[') {
                if (b == 'A') { ndx = 0; ndy = -1; }
                else if (b == 'B') { ndx = 0; ndy = 1; }
                else if (b == 'C') { ndx = 1; ndy = 0; }
                else if (b == 'D') { ndx = -1; ndy = 0; }
            }
        } else if (c == 'w' || c == 'W') {
            ndx = 0; ndy = -1;
        } else if (c == 's' || c == 'S') {
            ndx = 0; ndy = 1;
        } else if (c == 'a' || c == 'A') {
            ndx = -1; ndy = 0;
        } else if (c == 'd' || c == 'D') {
            ndx = 1; ndy = 0;
        } else if (c == 'q' || c == 'Q') {
            exit(0);
        } else if ((c == 'r' || c == 'R') && over) {
            new_game();
            continue;
        }
        /* Reversing straight into the body is never allowed. */
        if (!(len > 1 && ndx == -dx && ndy == -dy)) {
            dx = ndx;
            dy = ndy;
        }
    }
}

int main(void)
{
    srand((unsigned)time(NULL));
    enable_raw_terminal();
    new_game();
    for (;;) {
        read_input();
        if (!over)
            step();
        draw();
        usleep(120000);
    }
}
