/* render.c - see render.h. */
#include "render.h"
#include "art.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TERM_FG = 0xd0d4dc, TERM_BG = 0x101318 };

/* ---- one cell ---- */

static void fill_cell_rect(sr_canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t px = 0xff000000u | rgb;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < c->w) c->px[j * c->w + i] = px;
    }
}

static void glyph(sr_canvas *c, int x, int y, unsigned char ch, uint32_t fg, bool bold)
{
    const uint8_t *rows = sr_font_glyph_in(SR_FONT_FIXED_8X16, ch);
    uint32_t px = 0xff000000u | fg;
    if (rows == NULL) return;
    for (int j = 0; j < CELL_H; j++) {
        uint8_t bits = rows[j];
        if (bold) bits |= (uint8_t)(bits >> 1);
        if (y + j < 0 || y + j >= c->h) continue;
        for (int i = 0; i < CELL_W; i++)
            if ((bits & (0x80 >> i)) && x + i >= 0 && x + i < c->w)
                c->px[(y + j) * c->w + x + i] = px;
    }
}

/* Box drawing U+2500..U+257F as arms: up, right, down, left; 0 none,
 * 1 light, 2 heavy, 3 double. Unlisted ones fall back to '+'. */
static bool box_arms(uint32_t cp, int arms[4])
{
    static const struct { uint16_t cp; uint8_t u, r, d, l; } table[] = {
        {0x2500, 0,1,0,1}, {0x2501, 0,2,0,2}, {0x2502, 1,0,1,0}, {0x2503, 2,0,2,0},
        {0x2504, 0,1,0,1}, {0x2505, 0,2,0,2}, {0x2506, 1,0,1,0}, {0x2507, 2,0,2,0},
        {0x2508, 0,1,0,1}, {0x2509, 0,2,0,2}, {0x250a, 1,0,1,0}, {0x250b, 2,0,2,0},
        {0x250c, 0,1,1,0}, {0x250f, 0,2,2,0}, {0x2510, 0,0,1,1}, {0x2513, 0,0,2,2},
        {0x2514, 1,1,0,0}, {0x2517, 2,2,0,0}, {0x2518, 1,0,0,1}, {0x251b, 2,0,0,2},
        {0x251c, 1,1,1,0}, {0x2523, 2,2,2,0}, {0x2524, 1,0,1,1}, {0x252b, 2,0,2,2},
        {0x252c, 0,1,1,1}, {0x2533, 0,2,2,2}, {0x2534, 1,1,0,1}, {0x253b, 2,2,0,2},
        {0x253c, 1,1,1,1}, {0x254b, 2,2,2,2}, {0x254c, 0,1,0,1}, {0x254e, 1,0,1,0},
        {0x2550, 0,3,0,3}, {0x2551, 3,0,3,0}, {0x2554, 0,3,3,0}, {0x2557, 0,0,3,3},
        {0x255a, 3,3,0,0}, {0x255d, 3,0,0,3}, {0x2560, 3,3,3,0}, {0x2563, 3,0,3,3},
        {0x2566, 0,3,3,3}, {0x2569, 3,3,0,3}, {0x256c, 3,3,3,3},
        {0x256d, 0,1,1,0}, {0x256e, 0,0,1,1}, {0x256f, 1,0,0,1}, {0x2570, 1,1,0,0},
        {0x2574, 0,0,0,1}, {0x2575, 1,0,0,0}, {0x2576, 0,1,0,0}, {0x2577, 0,0,1,0},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].cp == cp) {
            arms[0] = table[i].u; arms[1] = table[i].r;
            arms[2] = table[i].d; arms[3] = table[i].l;
            return true;
        }
    }
    return false;
}

static void box(sr_canvas *c, int x, int y, const int arms[4], uint32_t fg)
{
    int cx = x + CELL_W / 2 - 1, cy = y + CELL_H / 2 - 1;
    for (int a = 0; a < 4; a++) {
        int w = arms[a] == 2 ? 3 : arms[a] == 3 ? 1 : 1;
        if (arms[a] == 0) continue;
        for (int pass = 0; pass < (arms[a] == 3 ? 2 : 1); pass++) {
            int o = arms[a] == 3 ? (pass ? 2 : -2) : (w == 3 ? -1 : 0);
            if (a == 0) fill_cell_rect(c, cx + o, y, w, cy - y + 1, fg);
            if (a == 2) fill_cell_rect(c, cx + o, cy, w, y + CELL_H - cy, fg);
            if (a == 1) fill_cell_rect(c, cx, cy + o, x + CELL_W - cx, w, fg);
            if (a == 3) fill_cell_rect(c, x, cy + o, cx - x + 1, w, fg);
        }
    }
}

/* Non-ASCII characters the 8x16 font lacks, mapped to a close ASCII. */
static unsigned char ascii_stand_in(uint32_t cp)
{
    switch (cp) {
    case 0x2022: case 0x25cf: case 0x23fa: case 0x2219: case 0x00b7: return '*';
    case 0x25cb: case 0x25ef: case 0x25e6: return 'o';
    case 0x2026: return '.';
    case 0x2713: case 0x2714: case 0x221a: return 'v';
    case 0x2717: case 0x2718: case 0x00d7: return 'x';
    case 0x276f: case 0x203a: case 0x2192: case 0x25b6: case 0x25b8: case 0x21b3: return '>';
    case 0x2039: case 0x2190: case 0x25c0: return '<';
    case 0x2191: case 0x25b2: return '^';
    case 0x2193: case 0x25bc: return 'v';
    case 0x2018: case 0x2019: case 0x00b4: return '\'';
    case 0x201c: case 0x201d: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x00a0: return ' ';
    default: return '?';
    }
}

static void draw_cell(sr_canvas *c, int x, int y, const vt_cell *cell, bool cursor)
{
    uint32_t fg = cell->fg == VT_DEFAULT_COLOR ? TERM_FG : cell->fg;
    uint32_t bg = cell->bg == VT_DEFAULT_COLOR ? TERM_BG : cell->bg;
    uint32_t cp = cell->ch;
    int width = (cell->attr & VT_ATTR_WIDE) ? 2 * CELL_W : CELL_W;
    int arms[4];

    if (cell->attr & VT_ATTR_WIDE_TAIL) return;
    if (cell->attr & VT_ATTR_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
    if (cursor) { uint32_t t = fg; fg = bg; bg = t == bg ? 0xd0d0d0 : t; }
    if (cell->attr & VT_ATTR_DIM) fg = sr_mix(fg, bg, 0.45f);
    fill_cell_rect(c, x, y, width, CELL_H, bg);

    if (cp >= 0x21 && cp <= 0x7e) {
        glyph(c, x, y, (unsigned char)cp, fg, (cell->attr & VT_ATTR_BOLD) != 0);
    } else if (cp >= 0x2500 && cp <= 0x257f) {
        if (box_arms(cp, arms)) box(c, x, y, arms, fg);
        else glyph(c, x, y, '+', fg, false);
    } else if (cp >= 0x2580 && cp <= 0x259f) {
        /* block elements: full, upper/lower halves, left/right halves, shades */
        if (cp == 0x2588) fill_cell_rect(c, x, y, CELL_W, CELL_H, fg);
        else if (cp == 0x2580) fill_cell_rect(c, x, y, CELL_W, CELL_H / 2, fg);
        else if (cp >= 0x2581 && cp <= 0x2587) {
            int h = (int)(cp - 0x2580) * CELL_H / 8;
            fill_cell_rect(c, x, y + CELL_H - h, CELL_W, h, fg);
        } else if (cp >= 0x2589 && cp <= 0x258f) {
            int w = (int)(0x2590 - cp) * CELL_W / 8;
            fill_cell_rect(c, x, y, w, CELL_H, fg);
        } else if (cp == 0x2590) fill_cell_rect(c, x + CELL_W / 2, y, CELL_W / 2, CELL_H, fg);
        else if (cp >= 0x2591 && cp <= 0x2593) {
            uint32_t m = sr_mix(bg, fg, 0.25f * (float)(cp - 0x2590));
            fill_cell_rect(c, x, y, CELL_W, CELL_H, m);
        } else glyph(c, x, y, '#', fg, false);
    } else if (cp >= 0x2800 && cp <= 0x28ff) {
        /* braille: eight dots in a 2x4 grid (spinners) */
        static const int dx[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static const int dy[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        for (int b = 0; b < 8; b++)
            if ((cp - 0x2800) & (1u << b))
                fill_cell_rect(c, x + 1 + dx[b] * 4, y + 2 + dy[b] * 3, 2, 2, fg);
    } else if (cp > 0x7e) {
        glyph(c, x, y, ascii_stand_in(cp), fg, false);
    }
    if (cell->attr & VT_ATTR_UNDERLINE) fill_cell_rect(c, x, y + CELL_H - 2, width, 1, fg);
    if (cell->attr & VT_ATTR_STRIKE) fill_cell_rect(c, x, y + CELL_H / 2, width, 1, fg);
}

void render_vt(sr_canvas *c, const vt *t, int x, int y, bool show_cursor)
{
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            const vt_cell *cell = vt_cell_at(t, col, row);
            bool cur = show_cursor && t->cursor_visible && col == t->cx && row == t->cy;
            draw_cell(c, x + col * CELL_W, y + row * CELL_H, cell, cur);
        }
    }
}

/* ---- the agent / goal / hints screen ---- */

/* Word-wrap s into lines of at most cols characters; returns line count. */
static int wrap(const char *s, int cols, char out[][ACT_LINE_LEN], int max_lines)
{
    int n = 0;
    while (*s && n < max_lines) {
        int len = (int)strlen(s);
        int take = len < cols ? len : cols;
        if (take < len) {
            int sp = take;
            while (sp > 0 && s[sp] != ' ') sp--;
            if (sp > cols / 3) take = sp;
        }
        if (take >= ACT_LINE_LEN) take = ACT_LINE_LEN - 1;
        memcpy(out[n], s, (size_t)take);
        out[n][take] = '\0';
        n++;
        s += take;
        while (*s == ' ') s++;
    }
    return n;
}

static void text_line(sr_canvas *c, int x, int y, const char *s, uint32_t rgb, int max_cols)
{
    char buf[ACT_LINE_LEN];
    size_t n = 0;
    /* the font is ASCII; UTF-8 sequences become one '?' each */
    for (const unsigned char *p = (const unsigned char *)s; *p && (int)n < max_cols &&
         n + 1 < sizeof buf; p++) {
        if (*p < 0x80) buf[n++] = (char)*p;
        else if ((*p & 0xc0) != 0x80) buf[n++] = '?';
    }
    buf[n] = '\0';
    sr_text(c, (float)x, (float)y, buf, rgb, 1.0f, 1);
}

static uint32_t kind_color(act_kind k)
{
    switch (k) {
    case ACT_YOU: return 0x9fd0ff;
    case ACT_SAYS: return 0xe8e8e8;
    case ACT_TOOL: return 0xffd27a;
    case ACT_RESULT: return 0x9aa3b0;
    case ACT_ERROR: return 0xff8a80;
    case ACT_FILE: return 0x8ff0a4;
    case ACT_GOAL: return 0x60f080;
    default: return 0x8a93a3;
    }
}

static void render_activity(sr_canvas *c, const view *v, int x, int y)
{
    char lines[8][ACT_LINE_LEN];
    char buf[ACT_LINE_LEN + 32];
    int row = 0, n;
    const int cols = ACT_COLS;

    snprintf(buf, sizeof buf, "LEVEL %d: %.120s", v->lv->number, v->lv->title);
    text_line(c, x, y, buf, 0xffe27a, cols);
    row++;
    snprintf(buf, sizeof buf, "GOAL: %.160s", v->lv->goal);
    n = wrap(buf, cols, lines, 2);
    for (int i = 0; i < n; i++) text_line(c, x, y + (row++) * CELL_H, lines[i], 0xffffff, cols);
    if (v->lv->check == CHECK_NONE)
        snprintf(buf, sizeof buf, "COMING SOON: the next challenge is not built yet.");
    else if (v->sc->goal_met)
        snprintf(buf, sizeof buf, "DONE: %.100s. The door is open: walk up the aisle.",
                 v->goal_why ? v->goal_why : "goal met");
    else
        snprintf(buf, sizeof buf, "NOT YET: %.150s", v->goal_why ? v->goal_why : "");
    text_line(c, x, y + (row++) * CELL_H, buf,
              v->lv->check == CHECK_NONE ? 0xffe27a : v->sc->goal_met ? 0x60f080 : 0xff9f80, cols);
    if (v->hints_shown > 0 && v->lv->nhints > 0) {
        int h = v->hints_shown > v->lv->nhints ? v->lv->nhints : v->hints_shown;
        snprintf(buf, sizeof buf, "HINT %d/%d: %.160s", h, v->lv->nhints, v->lv->hints[h - 1]);
        n = wrap(buf, cols, lines, 2);
        for (int i = 0; i < n; i++)
            text_line(c, x, y + (row++) * CELL_H, lines[i], 0x9fd0ff, cols);
    }
    fill_cell_rect(c, x, y + row * CELL_H + 6, cols * CELL_W, 1, 0x3a404c);
    row++;
    /* newest activity at the bottom */
    {
        int space = ACT_ROWS - row;
        int total = v->act ? v->act->count : 0;
        int first = total > space ? total - space : 0;
        if (total == 0)
            text_line(c, x, y + row * CELL_H, "(the agent's tool calls and file changes show here)",
                      0x6a7383, cols);
        for (int i = first; i < total; i++) {
            const act_line *l = activity_line(v->act, i);
            text_line(c, x, y + (row + i - first) * CELL_H, l->text, kind_color(l->kind), cols);
        }
    }
}

static void note_over(sr_canvas *c, screen_id id, const char *note)
{
    rect r = layout_screen(id);
    int w = sr_text_width(note, 1) + 24;
    float bx = r.x + (r.w - w) / 2.0f;
    float by = r.y + r.h - 44.0f;
    sr_fill_rect(c, bx, by, (float)w, 26, 0x2a1d10, 0.92f);
    sr_stroke_rect(c, bx, by, (float)w, 26, 1, 0xd8b440, 1.0f);
    sr_text(c, bx + 12, by + 5, note, 0xffe7a0, 1.0f, 1);
}

void render_frame(sr_canvas *c, const view *v)
{
    const scene *s = v->sc;
    screen_id near = s->focus == FOCUS_ROOM ? scene_near_screen(s) : SCREEN_NONE;
    int tx, ty;
    char hud[256];
    int next_room = s->level + 2;

    art_room(c, v->lv->room);
    art_door(c, v->lv->room, s->door, next_room <= v->level_count ? next_room : 0);

    art_screen(c, layout_screen(SCREEN_OMP), "omp  -  coding agent (Qwen 3.5, local)",
               s->focus == FOCUS_OMP, near == SCREEN_OMP, TERM_BG);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    if (v->omp) render_vt(c, v->omp, tx, ty, s->focus == FOCUS_OMP && v->blink);
    if (v->omp_note) note_over(c, SCREEN_OMP, v->omp_note);

    art_screen(c, layout_screen(SCREEN_SHELL), "shell  -  sandbox workspace",
               s->focus == FOCUS_SHELL, near == SCREEN_SHELL, TERM_BG);
    layout_text_origin(SCREEN_SHELL, &tx, &ty);
    if (v->shell) render_vt(c, v->shell, tx, ty, s->focus == FOCUS_SHELL && v->blink);
    if (v->shell_note) note_over(c, SCREEN_SHELL, v->shell_note);

    art_screen(c, layout_screen(SCREEN_ACT), "agent  -  goal  -  hints",
               false, near == SCREEN_ACT, 0x0d1014);
    layout_text_origin(SCREEN_ACT, &tx, &ty);
    render_activity(c, v, tx, ty);

    art_kilix(c, s->x, s->y, s->dir, s->moving ? ((int)s->walk_phase & 1) : 0);
    if (s->focus == FOCUS_ROOM) {
        if (near == SCREEN_OMP) art_prompt(c, s->x, s->y - 64, "Enter: talk to the agent");
        else if (near == SCREEN_SHELL) art_prompt(c, s->x, s->y - 64, "Enter: use the shell");
        else if (near == SCREEN_ACT) art_prompt(c, s->x, s->y - 64, "Enter: next hint");
        else if (scene_door_passable(s) && s->x >= AISLE_X0 && s->x <= AISLE_X1)
            art_prompt(c, s->x, s->y - 64, "The door is open: go up!");
    }

    if (s->focus == FOCUS_ROOM)
        snprintf(hud, sizeof hud,
                 "Arrows/WASD walk   Enter use screen   F1 agent  F2 shell   H hint   Ctrl+Q quit");
    else
        snprintf(hud, sizeof hud,
                 "Typing into the %s.   F10 or Ctrl+] back to the room   F1 agent  F2 shell",
                 s->focus == FOCUS_OMP ? "agent (omp)" : "shell");
    sr_text(c, 16, HUD_Y + 2, hud, 0xe6e6e6, 1.0f, 1);
}
