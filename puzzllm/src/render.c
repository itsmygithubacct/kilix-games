/* render.c - see render.h. */
#include "render.h"
#include "art.h"
#include "draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TERM_FG = 0xd0d4dc, TERM_BG = 0x101318, ACT_BG = 0x0d1014 };

/* ---- terminal cells ---- */

static void fill_cell_rect(sr_canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    uint32_t px = 0xff000000u | rgb;
    int x0 = x < 0 ? 0 : x, x1 = x + w > c->w ? c->w : x + w;
    for (int j = y < 0 ? 0 : y; j < y + h && j < c->h; j++) {
        uint32_t *row = c->px + (size_t)j * (size_t)c->w;
        for (int i = x0; i < x1; i++) row[i] = px;
    }
}

/* The screen behind the grid is already TERM_BG; a cell paints its own
 * background only when it differs. */
static void draw_cell(sr_canvas *c, int x, int y, const vt_cell *cell, bool cursor)
{
    uint32_t fg = cell->fg == VT_DEFAULT_COLOR ? TERM_FG : cell->fg;
    uint32_t bg = cell->bg == VT_DEFAULT_COLOR ? TERM_BG : cell->bg;
    uint32_t cp = cell->ch;
    int width = (cell->attr & VT_ATTR_WIDE) ? 2 * CELL_W : CELL_W;

    if (cell->attr & VT_ATTR_WIDE_TAIL) return;
    if (cell->attr & VT_ATTR_REVERSE) { uint32_t t = fg; fg = bg; bg = t; }
    if (cursor) { uint32_t t = fg; fg = bg; bg = t == bg ? 0xd0d0d0 : t; }
    if (cell->attr & VT_ATTR_DIM) fg = sr_mix(fg, bg, 0.45f);
    if (bg != TERM_BG) fill_cell_rect(c, x, y, width, CELL_H, bg);
    if (cp > 0x20) draw_glyph(c, x, y, width, CELL_H, cp, fg, (cell->attr & VT_ATTR_BOLD) != 0);
    if (cell->attr & VT_ATTR_UNDERLINE) fill_cell_rect(c, x, y + CELL_H - 2, width, 1, fg);
    if (cell->attr & VT_ATTR_STRIKE) fill_cell_rect(c, x, y + CELL_H / 2, width, 1, fg);
}

/* The cells over a text area that is already TERM_BG. */
static void vt_cells(sr_canvas *c, const vt *t, int x, int y, bool show_cursor)
{
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols; col++) {
            const vt_cell *cell = vt_cell_at(t, col, row);
            bool cur = show_cursor && t->cursor_visible && col == t->cx && row == t->cy;
            if (!cur && cell->ch <= 0x20 && cell->bg == VT_DEFAULT_COLOR &&
                !(cell->attr & (VT_ATTR_REVERSE | VT_ATTR_UNDERLINE | VT_ATTR_STRIKE)))
                continue;   /* a blank cell: the fill above drew it */
            draw_cell(c, x + col * CELL_W, y + row * CELL_H, cell, cur);
        }
    }
}

void render_vt(sr_canvas *c, const vt *t, int x, int y, bool show_cursor)
{
    fill_cell_rect(c, x, y, t->cols * CELL_W, t->rows * CELL_H, TERM_BG);
    vt_cells(c, t, x, y, show_cursor);
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
    char buf[ACT_LINE_LEN * 4];
    size_t n = 0;
    int cols = 0;
    /* at most max_cols characters, cut on a character boundary */
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < sizeof buf; p++) {
        if ((*p & 0xc0) != 0x80 && cols++ >= max_cols) break;
        buf[n++] = (char)*p;
    }
    buf[n] = '\0';
    draw_text(c, (float)x, (float)y, buf, rgb, 1.0f);
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
    default: return 0x8a93a3;
    }
}

static void render_activity(sr_canvas *c, const view *v, int x, int y)
{
    char lines[8][ACT_LINE_LEN];
    char buf[ACT_LINE_LEN + 32];
    int row = 0, n;
    const int cols = ACT_COLS < ACT_LINE_LEN - 1 ? ACT_COLS : ACT_LINE_LEN - 1;

    n = wrap(v->lv->goal, cols - 2, lines, 4);
    fill_cell_rect(c, x, y, cols * CELL_W, (n + 1) * CELL_H, 0x27323e);
    for (int i = 0; i < n; i++)
        text_line(c, x + CELL_W, y + (row++) * CELL_H, lines[i], 0xffffff, cols - 2);
    if (v->lv->check == CHECK_NONE)
        snprintf(buf, sizeof buf, "COMING SOON: the next challenge is not built yet.");
    else if (v->sc->goal_met)
        snprintf(buf, sizeof buf, "DONE: %.100s. The door is open: walk up the aisle.",
                 v->goal_why ? v->goal_why : "goal met");
    else
        snprintf(buf, sizeof buf, "NOT YET: %.150s", v->goal_why ? v->goal_why : "");
    text_line(c, x + CELL_W, y + (row++) * CELL_H, buf,
              v->lv->check == CHECK_NONE ? 0xffe27a : v->sc->goal_met ? 0x60f080 : 0xff9f80,
              cols - 2);
    if (v->hints_shown > 0 && v->lv->nhints > 0) {
        int h = v->hints_shown > v->lv->nhints ? v->lv->nhints : v->hints_shown;
        snprintf(buf, sizeof buf, "HINT %d/%d: %.160s", h, v->lv->nhints, v->lv->hints[h - 1]);
        n = wrap(buf, cols, lines, 2);
        for (int i = 0; i < n; i++)
            text_line(c, x, y + (row++) * CELL_H, lines[i], 0x9fd0ff, cols);
    }
    fill_cell_rect(c, x, y + row * CELL_H - 1, cols * CELL_W, 1, 0x3a404c);
    /* Goal updates are already visible in the goal block. */
    int total = v->act ? v->act->count : 0;
    int first = total;
    int visible = 0;
    while (first > 0 && visible < ACT_ROWS - row) {
        if (activity_line(v->act, --first)->kind != ACT_GOAL) visible++;
    }
    if (visible == 0 && row < ACT_ROWS)
        text_line(c, x, y + row * CELL_H, "Tool calls and file changes appear here.",
                  0x6a7383, cols);
    for (int i = first; i < total; i++) {
        const act_line *l = activity_line(v->act, i);
        if (l->kind != ACT_GOAL)
            text_line(c, x, y + (row++) * CELL_H, l->text, kind_color(l->kind), cols);
    }
}

static void note_over(sr_canvas *c, screen_id id, const char *note)
{
    rect r = layout_screen(id);
    int w = draw_text_width(note) + 24;
    float bx = r.x + (r.w - w) / 2.0f;
    float by = r.y + r.h - 44.0f;
    draw_fill(c, bx, by, (float)w, 26, 0x2a1d10, 0.92f);
    draw_stroke(c, bx, by, (float)w, 26, 1, 0xd8b440, 1.0f);
    draw_text(c, bx + 12, by + 5, note, 0xffe7a0, 1.0f);
}

/* The floor, walls and runner change only with the room's look, the frame
 * size and the renderer, so they are drawn once and copied each frame. */
static void room_background(sr_canvas *c, const char *style)
{
    static sr_canvas bg;
    static char bg_style[32];
    static char bg_renderer[16];
    char renderer[16];

    snprintf(renderer, sizeof renderer, "%.15s", draw_renderer());
    if (bg.px == NULL || bg.w != c->w || bg.h != c->h ||
        strcmp(bg_style, style ? style : "") != 0 || strcmp(bg_renderer, renderer) != 0) {
        sr_canvas_free(&bg);
        if (!sr_canvas_init(&bg, c->w, c->h)) {
            art_room(c, style);
            art_screen_glass(c, layout_screen(SCREEN_OMP), TERM_BG);
            art_screen_glass(c, layout_screen(SCREEN_SHELL), TERM_BG);
            art_screen_glass(c, layout_screen(SCREEN_ACT), ACT_BG);
            return;
        }
        art_room(&bg, style);
        art_screen_glass(&bg, layout_screen(SCREEN_OMP), TERM_BG);
        art_screen_glass(&bg, layout_screen(SCREEN_SHELL), TERM_BG);
        art_screen_glass(&bg, layout_screen(SCREEN_ACT), ACT_BG);
        snprintf(bg_style, sizeof bg_style, "%s", style ? style : "");
        snprintf(bg_renderer, sizeof bg_renderer, "%s", renderer);
    }
    memcpy(c->px, bg.px, (size_t)c->w * (size_t)c->h * sizeof *c->px);
}

void render_frame(sr_canvas *c, const view *v)
{
    const scene *s = v->sc;
    screen_id near = s->focus == FOCUS_ROOM ? scene_near_screen(s) : SCREEN_NONE;
    int tx, ty;
    char hud[256];
    int next_room = s->level + 2;

    room_background(c, v->lv->room);
    art_door(c, v->lv->room, s->door, next_room <= v->level_count ? next_room : 0);

    art_screen(c, layout_screen(SCREEN_OMP), "omp  -  coding agent (Qwen 3.5, local)",
               s->focus == FOCUS_OMP, near == SCREEN_OMP);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    if (v->omp) vt_cells(c, v->omp, tx, ty, s->focus == FOCUS_OMP && v->blink);
    if (v->omp_note) note_over(c, SCREEN_OMP, v->omp_note);

    art_screen(c, layout_screen(SCREEN_SHELL), "shell  -  sandbox workspace",
               s->focus == FOCUS_SHELL, near == SCREEN_SHELL);
    layout_text_origin(SCREEN_SHELL, &tx, &ty);
    if (v->shell) vt_cells(c, v->shell, tx, ty, s->focus == FOCUS_SHELL && v->blink);
    if (v->shell_note) note_over(c, SCREEN_SHELL, v->shell_note);

    snprintf(hud, sizeof hud, "Goal  |  Room %d: %.100s", v->lv->number, v->lv->title);
    art_screen(c, layout_screen(SCREEN_ACT), hud,
               false, near == SCREEN_ACT);
    layout_text_origin(SCREEN_ACT, &tx, &ty);
    render_activity(c, v, tx, ty);

    if (v->lv->check == CHECK_CHEST)
        art_chest(c, (AISLE_X0 + AISLE_X1) / 2 + 16, START_Y - 66, s->goal_met);
    art_kilix(c, s->x, s->y, s->dir, s->step_ticks, s->idle_ticks);
    if (s->focus == FOCUS_ROOM) {
        if (near == SCREEN_OMP) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: talk to the agent");
        else if (near == SCREEN_SHELL) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: use the shell");
        else if (near == SCREEN_ACT) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: next hint");
        else if (scene_door_passable(s) && s->x >= AISLE_X0 && s->x <= AISLE_X1)
            art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "The door is open: go up!");
    }

    if (s->focus == FOCUS_ROOM)
        snprintf(hud, sizeof hud,
                 "Arrows/WASD walk   Enter use screen   F1 agent  F2 shell   H hint   Ctrl+Q quit");
    else
        snprintf(hud, sizeof hud,
                 "Typing into the %s.   F10 or Ctrl+] back to the room   F1 agent  F2 shell",
                 s->focus == FOCUS_OMP ? "agent (omp)" : "shell");
    draw_text(c, 16, HUD_Y + 2, hud, 0xe6e6e6, 1.0f);
}
