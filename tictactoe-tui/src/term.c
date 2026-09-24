/* The terminal: raw input, SGR mouse, and drawing a Screen with ANSI escapes.
 *
 * The game runs in the terminal it was started from: it switches to the
 * alternate screen, hides the cursor and enables mouse reports, and puts all
 * of that back on exit, on SIGTERM/SIGHUP, and on a crash signal. Ctrl+C
 * arrives as a key (the terminal is raw) and quits cleanly.
 */
#include "ttt.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static bool active;
static volatile sig_atomic_t resized;
static Screen shown;            /* what the terminal currently displays */
static bool shown_valid;

static const char enter_seq[] = "\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[2J";
static const char leave_seq[] = "\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";

static void write_all(const char *data, size_t len)
{
    while (len) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

static void restore(void)
{
    if (!active) return;
    write_all(leave_seq, sizeof leave_seq - 1);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    active = false;
}

static void on_fatal(int sig)
{
    restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

bool term_init(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return false;
    struct termios raw = saved;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    active = true;
    (void)atexit(restore);
    signal(SIGTERM, on_fatal);
    signal(SIGHUP, on_fatal);
    signal(SIGSEGV, on_fatal);
    signal(SIGABRT, on_fatal);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    (void)sigaction(SIGWINCH, &sa, NULL);
    write_all(enter_seq, sizeof enter_seq - 1);
    shown_valid = false;
    return true;
}

void term_shutdown(void)
{
    restore();
}

void term_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

/* ---------- input ---------- */

static unsigned char pending[256];
static size_t pending_len;

static void fill_pending(int timeout_ms)
{
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0) return;
    ssize_t n = read(STDIN_FILENO, pending + pending_len, sizeof pending - pending_len);
    if (n > 0) pending_len += (size_t)n;
}

static void consume(size_t n)
{
    memmove(pending, pending + n, pending_len - n);
    pending_len -= n;
}

/* Decodes one input from the front of `pending`; 0 bytes means incomplete. */
static size_t decode(Input *in)
{
    unsigned char c = pending[0];
    in->mouse_x = in->mouse_y = -1;
    if (c != 0x1b) {
        in->key = c == '\r' || c == '\n' ? KEY_ENTER : c == 3 ? KEY_INTERRUPT : c;
        return 1;
    }
    if (pending_len == 1) return 0;
    if (pending[1] != '[' && pending[1] != 'O') {   /* Alt+key: take the Esc alone */
        in->key = KEY_ESC;
        return 1;
    }
    if (pending_len < 3) return 0;
    switch (pending[2]) {
    case 'A': in->key = KEY_UP; return 3;
    case 'B': in->key = KEY_DOWN; return 3;
    case 'C': in->key = KEY_RIGHT; return 3;
    case 'D': in->key = KEY_LEFT; return 3;
    case 'M':                                  /* keypad Enter in application mode */
        if (pending[1] == 'O') { in->key = KEY_ENTER; return 3; }
        break;
    default: break;
    }
    if (pending[1] == '[' && pending[2] == '<') {  /* SGR mouse: ESC [ < b ; x ; y M|m */
        size_t i = 3;
        int field[3] = { 0, 0, 0 }, f = 0;
        for (; i < pending_len; i++) {
            unsigned char d = pending[i];
            if (d >= '0' && d <= '9') field[f] = field[f] * 10 + (d - '0');
            else if (d == ';' && f < 2) f++;
            else if (d == 'M' || d == 'm') break;
            else { in->key = KEY_NONE; return i + 1; }
        }
        if (i >= pending_len) return 0;
        bool press = pending[i] == 'M';
        in->key = press && (field[0] & ~0x1c) == 0 ? KEY_MOUSE : KEY_NONE;  /* left button */
        in->mouse_x = field[1] - 1;
        in->mouse_y = field[2] - 1;
        return i + 1;
    }
    /* Any other CSI/SS3 sequence: skip to its final byte. */
    size_t i = 2;
    while (i < pending_len && (pending[i] < 0x40 || pending[i] > 0x7e)) i++;
    if (i >= pending_len) return 0;
    in->key = KEY_NONE;
    return i + 1;
}

bool term_read(Input *in, int timeout_ms)
{
    if (resized) {
        resized = 0;
        shown_valid = false;
        in->key = KEY_RESIZE;
        return true;
    }
    if (!pending_len) fill_pending(timeout_ms);
    if (!pending_len) return false;
    size_t used = decode(in);
    if (!used) {                 /* incomplete: wait briefly for the rest */
        fill_pending(40);
        used = decode(in);
        if (!used) {             /* a lone Esc (or a truncated sequence) */
            in->key = KEY_ESC;
            used = 1;
        }
    }
    consume(used);
    return in->key != KEY_NONE;
}

/* ---------- output ---------- */

typedef struct {
    char *data;
    size_t len, cap;
} Buffer;

static void append(Buffer *b, const char *s, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 16384;
        while (cap < b->len + n) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void append_str(Buffer *b, const char *s)
{
    append(b, s, strlen(s));
}

static const char *color_sgr(int color)
{
    switch (color) {
    case COLOR_DIM:    return "90";
    case COLOR_X:      return "96";
    case COLOR_O:      return "95";
    case COLOR_GRID:   return "34";
    case COLOR_TITLE:  return "93";
    case COLOR_SELECT: return "97;44";
    case COLOR_WIN:    return "30;43";
    case COLOR_HINT:   return "30;42";
    case COLOR_ALERT:  return "91";
    default:           return "39";
    }
}

static bool same_style(const Cell *a, const Cell *b)
{
    return a->color == b->color && a->bold == b->bold && a->reverse == b->reverse;
}

static void style(Buffer *b, const Cell *c)
{
    char sgr[48];
    /* Reverse cells use the colour pair as-is (the SELECT/WIN/HINT pairs are
       already foreground-on-background), so no SGR 7 is needed. */
    const char *pair = c->reverse ? color_sgr(c->color)
                       : (c->color == COLOR_SELECT ? "97" : c->color == COLOR_WIN ? "93"
                          : c->color == COLOR_HINT ? "92" : color_sgr(c->color));
    (void)snprintf(sgr, sizeof sgr, "\x1b[0;%s%sm", c->bold ? "1;" : "", pair);
    append_str(b, sgr);
}

void term_draw(const Screen *s)
{
    static Buffer out;
    out.len = 0;
    bool full = !shown_valid || shown.w != s->w || shown.h != s->h;
    if (full) append_str(&out, "\x1b[0m\x1b[2J");
    for (int y = 0; y < s->h; y++) {
        if (!full && !memcmp(shown.cells[y], s->cells[y], sizeof(Cell) * (size_t)s->w))
            continue;
        char move[24];
        (void)snprintf(move, sizeof move, "\x1b[%d;1H", y + 1);
        append_str(&out, move);
        const Cell *last = NULL;
        /* Leave the bottom-right cell alone: writing it scrolls some terminals. */
        int width = y == s->h - 1 ? s->w - 1 : s->w;
        for (int x = 0; x < width; x++) {
            const Cell *c = &s->cells[y][x];
            if (!last || !same_style(last, c)) style(&out, c);
            append_str(&out, c->ch);
            last = c;
        }
    }
    append_str(&out, "\x1b[0m");
    write_all(out.data, out.len);
    shown.w = s->w;
    shown.h = s->h;
    for (int y = 0; y < s->h; y++)
        memcpy(shown.cells[y], s->cells[y], sizeof(Cell) * (size_t)s->w);
    shown_valid = true;
}
