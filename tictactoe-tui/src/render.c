/* Frame layout into a Screen of cells. Pure: no terminal, no globals. */
#include "ttt.h"

#include <stdio.h>
#include <string.h>

#define CELL_W 11
#define CELL_H 5
#define BOARD_W (CELL_W * 3 + 2)
#define BOARD_H (CELL_H * 3 + 2)
#define BOARD_TOP 3
#define MENU_W 38

/* ---------- cell writing ---------- */

static void clear(Screen *s, int w, int h)
{
    s->w = w < SCREEN_MAX_W ? w : SCREEN_MAX_W;
    s->h = h < SCREEN_MAX_H ? h : SCREEN_MAX_H;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            s->cells[y][x] = (Cell){ " ", COLOR_DEFAULT, false, false };
}

static void put(Screen *s, int x, int y, const char *ch, int color, bool bold, bool reverse)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    Cell *c = &s->cells[y][x];
    (void)snprintf(c->ch, sizeof c->ch, "%s", ch);
    c->color = (uint8_t)color;
    c->bold = bold;
    c->reverse = reverse;
}

/* Bytes in the UTF-8 sequence starting with `lead`. */
static int utf8_len(unsigned char lead)
{
    return lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
}

static int text_width(const char *text)
{
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p += utf8_len(*p))
        width++;
    return width;
}

static void text(Screen *s, int x, int y, const char *str, int color, bool bold, bool reverse)
{
    for (const unsigned char *p = (const unsigned char *)str; *p; x++) {
        char ch[5] = { 0 };
        int n = utf8_len(*p);
        memcpy(ch, p, (size_t)n);
        put(s, x, y, ch, color, bold, reverse);
        p += n;
    }
}

static void centered(Screen *s, int y, const char *str, int color, bool bold)
{
    text(s, (s->w - text_width(str)) / 2, y, str, color, bold, false);
}

static void fill(Screen *s, int x, int y, int w, int h, int color, bool reverse)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            put(s, col, row, " ", color, false, reverse);
}

/* ---------- geometry ---------- */

static int board_left(int w)
{
    return (w - BOARD_W) / 2;
}

int render_hit_cell(int w, int h, int x, int y)
{
    (void)h;
    int bx = x - board_left(w), by = y - BOARD_TOP;
    if (bx < 0 || by < 0 || bx >= BOARD_W || by >= BOARD_H) return -1;
    if (bx % (CELL_W + 1) == CELL_W || by % (CELL_H + 1) == CELL_H) return -1;  /* grid line */
    return (by / (CELL_H + 1)) * 3 + bx / (CELL_W + 1);
}

/* The pause and game-over menus sit on one line under the board. */
static const char *const pause_items[PAUSE_ROWS] = {
    "RESUME", "RESTART GAME", "MAIN MENU", "QUIT"
};
static const char *const over_items[OVER_ROWS] = { "PLAY AGAIN", "MAIN MENU", "QUIT" };
#define ITEM_GAP 3
#define BAR_Y (BOARD_TOP + BOARD_H + 2)

static int bar_start(int w, const char *const *items, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++) total += text_width(items[i]) + 2;
    total += ITEM_GAP * (count - 1);
    return (w - total) / 2;
}

static int bar_hit(int w, const char *const *items, int count, int x)
{
    int at = bar_start(w, items, count);
    for (int i = 0; i < count; i++) {
        int width = text_width(items[i]) + 2;
        if (x >= at && x < at + width) return i;
        at += width + ITEM_GAP;
    }
    return -1;
}

#define MENU_TOP 7

static int menu_row_y(int row)
{
    /* PLAY, a gap, the four settings, a gap, QUIT. */
    return MENU_TOP + row * 2 + (row > MENU_PLAY ? 1 : 0) + (row == MENU_QUIT ? 1 : 0);
}

int render_hit_row(const Game *g, int w, int h, int x, int y)
{
    (void)h;
    if (g->screen == SCREEN_MENU) {
        int left = (w - MENU_W) / 2;
        if (x < left || x >= left + MENU_W) return -1;
        for (int row = 0; row < MENU_ROWS; row++)
            if (y == menu_row_y(row)) return row;
        return -1;
    }
    if (y != BAR_Y) return -1;
    if (g->screen == SCREEN_PAUSE) return bar_hit(w, pause_items, PAUSE_ROWS, x);
    if (g->screen == SCREEN_OVER) return bar_hit(w, over_items, OVER_ROWS, x);
    return -1;
}

/* ---------- pieces ---------- */

static const char *const x_art[3] = { " ╲   ╱ ", "   ╳   ", " ╱   ╲ " };
static const char *const o_art[3] = { " ╭───╮ ", " │   │ ", " ╰───╯ " };

static void draw_title(Screen *s)
{
    int x = (s->w - 21) / 2;
    text(s, x, 0, "T I C", COLOR_X, true, false);
    text(s, x + 6, 0, "·", COLOR_DIM, false, false);
    text(s, x + 8, 0, "T A C", COLOR_TITLE, true, false);
    text(s, x + 14, 0, "·", COLOR_DIM, false, false);
    text(s, x + 16, 0, "T O E", COLOR_O, true, false);
}

static void draw_scores(const Game *g, Screen *s)
{
    char left[40], mid[32], right[40];
    (void)snprintf(left, sizeof left, "X %s  %d", player_name(g->players[0]), g->wins_x);
    (void)snprintf(mid, sizeof mid, "draws %d", g->draws);
    (void)snprintf(right, sizeof right, "%d  %s O", g->wins_o, player_name(g->players[1]));
    int x = board_left(s->w);
    text(s, x, 1, left, COLOR_X, true, false);
    centered(s, 1, mid, COLOR_DIM, false);
    text(s, x + BOARD_W - text_width(right), 1, right, COLOR_O, true, false);
}

static void draw_board(const Game *g, Screen *s)
{
    int left = board_left(s->w);
    uint16_t win = board_winning_line(&g->board);
    bool playing = g->screen == SCREEN_PLAY;

    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            bool vline = x % (CELL_W + 1) == CELL_W, hline = y % (CELL_H + 1) == CELL_H;
            if (vline && hline) put(s, left + x, BOARD_TOP + y, "╋", COLOR_GRID, false, false);
            else if (vline) put(s, left + x, BOARD_TOP + y, "┃", COLOR_GRID, false, false);
            else if (hline) put(s, left + x, BOARD_TOP + y, "━", COLOR_GRID, false, false);
        }
    }
    for (int cell = 0; cell < 9; cell++) {
        int cx = left + (cell % 3) * (CELL_W + 1);
        int cy = BOARD_TOP + (cell / 3) * (CELL_H + 1);
        int mark = board_cell(&g->board, cell);
        bool winning = win >> cell & 1u;
        bool cursor = playing && cell == g->cursor && !game_is_computer_turn(g);
        bool hint = playing && cell == g->hint;
        if (cursor) fill(s, cx, cy, CELL_W, CELL_H, hint ? COLOR_HINT : COLOR_SELECT, true);
        else if (winning) fill(s, cx, cy, CELL_W, CELL_H, COLOR_WIN, true);
        if (mark != MARK_NONE) {
            const char *const *art = mark == MARK_X ? x_art : o_art;
            int color = winning ? COLOR_WIN : (mark == MARK_X ? COLOR_X : COLOR_O);
            for (int row = 0; row < 3; row++)
                text(s, cx + 2, cy + 1 + row, art[row], color, true, cursor || winning);
        } else {
            /* The numpad key for this square, faint, so keyboard play needs no chart. */
            static const char keys[9] = { '7', '8', '9', '4', '5', '6', '1', '2', '3' };
            char key[2] = { keys[cell], 0 };
            text(s, cx + CELL_W / 2, cy + CELL_H / 2, key,
                 hint ? COLOR_HINT : COLOR_DIM, hint, cursor);
        }
    }
}

static void draw_bar(Screen *s, const char *const *items, int count, int selected)
{
    int x = bar_start(s->w, items, count);
    for (int i = 0; i < count; i++) {
        char label[40];
        (void)snprintf(label, sizeof label, " %s ", items[i]);
        text(s, x, BAR_Y, label, i == selected ? COLOR_SELECT : COLOR_DEFAULT,
             i == selected, i == selected);
        x += text_width(label) + ITEM_GAP;
    }
}

static void draw_menu(const Game *g, Screen *s)
{
    static const char *const labels[MENU_ROWS] = {
        "PLAY", "X PLAYER", "O PLAYER", "NEURAL LEVEL", "FIRST MOVE", "QUIT"
    };
    int left = (s->w - MENU_W) / 2;
    centered(s, 2, "three in a row, against a trained network", COLOR_DIM, false);
    char status[96];
    (void)snprintf(status, sizeof status, "neural player %s", neural_status());
    centered(s, 4, status, neural_ready() ? COLOR_DIM : COLOR_ALERT, false);

    bool neural_used = g->players[0] == PLAYER_NEURAL || g->players[1] == PLAYER_NEURAL;
    for (int row = 0; row < MENU_ROWS; row++) {
        int y = menu_row_y(row);
        bool selected = row == g->menu_row;
        if (selected) fill(s, left, y, MENU_W, 1, COLOR_SELECT, true);
        if (row == MENU_PLAY || row == MENU_QUIT) {
            const char *label = labels[row];
            text(s, left + (MENU_W - text_width(label)) / 2, y, label,
                 selected ? COLOR_SELECT : (row == MENU_PLAY ? COLOR_TITLE : COLOR_ALERT),
                 true, selected);
            continue;
        }
        const char *value;
        switch (row) {
        case MENU_X: value = player_name(g->players[0]); break;
        case MENU_O: value = player_name(g->players[1]); break;
        case MENU_LEVEL: value = level_name(g->level); break;
        default: value = g->starts == STARTS_X ? "X ALWAYS" : "ALTERNATE"; break;
        }
        char shown[32];
        (void)snprintf(shown, sizeof shown, selected ? "< %s >" : "%s", value);
        int color = row == MENU_X ? COLOR_X : row == MENU_O ? COLOR_O : COLOR_DEFAULT;
        if (row == MENU_LEVEL && !neural_used && !selected) color = COLOR_DIM;
        text(s, left + 2, y, labels[row], selected ? COLOR_SELECT : COLOR_DIM, false, selected);
        text(s, left + MENU_W - 2 - text_width(shown), y, shown,
             selected ? COLOR_SELECT : color, true, selected);
    }
    centered(s, s->h - 1, "↑↓ select   ←→ change   enter choose", COLOR_DIM, false);
}

void render(const Game *g, Screen *s, int w, int h)
{
    clear(s, w, h);
    if (s->w < MIN_W || s->h < MIN_H) {
        char need[64];
        (void)snprintf(need, sizeof need, "Enlarge the terminal to %dx%d", MIN_W, MIN_H);
        centered(s, s->h / 2, need, COLOR_ALERT, true);
        return;
    }
    draw_title(s);
    if (g->screen == SCREEN_MENU) {
        draw_menu(g, s);
        return;
    }
    draw_scores(g, s);
    draw_board(g, s);

    int message_color = COLOR_DEFAULT;
    if (g->screen == SCREEN_OVER)
        message_color = g->last_result == RESULT_DRAW ? COLOR_TITLE :
                        g->last_result == RESULT_X ? COLOR_X : COLOR_O;
    centered(s, BOARD_TOP + BOARD_H, g->screen == SCREEN_PAUSE ? "Paused" : g->message,
             message_color, g->screen == SCREEN_OVER);

    if (g->screen == SCREEN_PAUSE) {
        draw_bar(s, pause_items, PAUSE_ROWS, g->pause_row);
        centered(s, s->h - 1, "↑↓ select   enter choose   esc resume", COLOR_DIM, false);
    } else if (g->screen == SCREEN_OVER) {
        draw_bar(s, over_items, OVER_ROWS, g->over_row);
        centered(s, s->h - 1, "↑↓ select   enter choose   esc main menu", COLOR_DIM, false);
    } else {
        centered(s, s->h - 1, "arrows move  enter place  1-9 numpad  ? hint  esc menu",
                 COLOR_DIM, false);
    }
}

void screen_row_text(const Screen *s, int y, char *out, size_t out_len)
{
    size_t at = 0;
    if (!out_len) return;
    for (int x = 0; y >= 0 && y < s->h && x < s->w; x++) {
        size_t n = strlen(s->cells[y][x].ch);
        if (at + n + 1 > out_len) break;
        memcpy(out + at, s->cells[y][x].ch, n);
        at += n;
    }
    out[at] = '\0';
}
