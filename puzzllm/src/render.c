/* render.c - see render.h. */
#include "render.h"
#include "game.h"
#include "art.h"
#include "draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TERM_BG = NEUROTERM_BG, ACT_BG = 0x0d1014 };

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

static void terminal_fill(void *user, int x, int y, int w, int h, uint32_t rgb)
{
    fill_cell_rect(user, x, y, w, h, rgb);
}

static void terminal_glyph(void *user, uint32_t scalar, unsigned flags, int x, int y,
                           int w, int h, uint32_t rgb)
{
    unsigned style = (flags & NEUROTERM_BOLD ? DRAW_GLYPH_BOLD : 0u) |
                     (flags & NEUROTERM_ITALIC ? DRAW_GLYPH_ITALIC : 0u);
    draw_glyph(user, x, y, w, h, scalar, rgb, style);
}

/* The screen bezel has already painted the default background. */
static void vt_cells(sr_canvas *c, const neuroterm *t, int x, int y, bool show_cursor)
{
    VTermPos cursor = neuroterm_cursor(t);
    const neuroterm_painter painter = {c, terminal_fill, terminal_glyph};
    neuroterm_cell_style style = {
        .foreground = NEUROTERM_FG, .background = NEUROTERM_BG,
        .focused = true, .cursor_shape = t->cursor_shape,
    };
    for (int row = 0; row < t->rows; row++) {
        for (int col = 0; col < t->cols;) {
            VTermScreenCell cell;
            if (!neuroterm_cell(t, col, row, &cell)) {
                col++;
                continue;
            }
            int span = cell.width == 2 && col + 1 < t->cols ? 2 : 1;
            style.cursor = show_cursor && t->cursor_visible && row == cursor.row &&
                           cursor.col >= col && cursor.col < col + span;
            neuroterm_paint_cell(t->screen, &cell, x + col * CELL_W, y + row * CELL_H,
                                 CELL_W, CELL_H, span, &style, &painter);
            col += span;
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

void render_menu(sr_canvas *c, const model_option *models, int count, int selected,
                 const char *active_model, bool loading, bool failed,
                 bool started, int level, bool confirm_reset, const char *notice, int support, bool thinking_enabled)
{
    static const char *const actions[] = {
        [MENU_QUIT] = "Exit game", [MENU_RESET] = "Reset progress",
        [MENU_LOCAL] = "Qwen 3.5 (llama.cpp)",
    };
    int width = 560, row_h = 27;
    int rows = (c->h - 180) / row_h;
    int options = count + MENU_FIRST_OLLAMA;
    if (rows > 14) rows = 14;
    if (rows > options) rows = options;
    int first = selected >= rows ? selected - rows + 1 : 0;
    int height = 125 + rows * row_h;
    int x = (c->w - width) / 2;
    int y = (c->h - height) / 2;
    char label[320];

    draw_fill(c, 0, 0, c->w, c->h, 0x0c1018, 0.72f);
    if (confirm_reset) {
        int top = (c->h - 130) / 2;
        draw_fill(c, x, top, width, 130, 0x151d29, 1);
        draw_stroke(c, x, top, width, 130, 2, 0x826b47, 1);
        draw_text(c, x + 18, top + 16, "Reset progress?", 0xffde8c, 1);
        draw_text(c, x + 18, top + 46, "Start at level 1. The current workspace will be backed up.",
                  0xf2efeb, 1);
        draw_text(c, x + 18, top + 93, "Enter reset    Esc cancel", 0xbccadb, 1);
        return;
    }
    draw_fill(c, x, y, width, height, 0x151d29, 1);
    draw_stroke(c, x, y, width, height, 2, 0x826b47, 1);
    draw_text(c, x + 18, y + 13, "Game menu", 0xffde8c, 1);
    snprintf(label, sizeof label, "Agent model: %s", *active_model ? active_model : actions[MENU_LOCAL]);
    text_line(c, x + 18, y + 39, label, 0xbccadb, (width - 36) / CELL_W);

    for (int i = first; i < first + rows; i++) {
        int row_y = y + 62 + (i - first) * row_h;
        if (i == selected)
            draw_fill(c, x + 11, row_y, width - 22, row_h - 2, 0x354356, 1);
        if (i == MENU_RESUME) {
            if (started) snprintf(label, sizeof label, "Resume game");
            else if (level > 1) snprintf(label, sizeof label, "Continue from level %d", level);
            else snprintf(label, sizeof label, "Start game");
        } else if (i == MENU_THINKING) {
            const char *status = thinking_enabled ? "On" :
                support == THINK_UNAVAILABLE ? "Off (unavailable)" :
                support == THINK_HAS_MINIMUM ? "Off (model minimum)" : "Off";
            snprintf(label, sizeof label, "Thinking: %s", status);
        } else if (i < MENU_FIRST_OLLAMA)
            snprintf(label, sizeof label, "%s", actions[i]);
        else
            snprintf(label, sizeof label, "%s (Ollama)", models[i - MENU_FIRST_OLLAMA].name);
        uint32_t color = i == MENU_THINKING && support == THINK_UNAVAILABLE ? 0xa6b5c6 : 0xf2efeb;
        text_line(c, x + 20, row_y + 5, label, color, (width - 40) / CELL_W);
    }
    if (*notice || loading || failed || count == 0 || options > rows) {
        if (*notice) snprintf(label, sizeof label, "%s", notice);
        else if (loading) snprintf(label, sizeof label, "Looking for installed Ollama models...");
        else if (failed) snprintf(label, sizeof label, "Could not reach the local Ollama service.");
        else if (count == 0) snprintf(label, sizeof label, "No installed Ollama models found.");
        else snprintf(label, sizeof label, "Options %d-%d of %d", first + 1, first + rows, options);
        text_line(c, x + 18, y + height - 48, label, 0xa6b5c6, (width - 36) / CELL_W);
    }
    draw_text(c, x + 18, y + height - 26,
              started ? "Up/Down move    Enter select    Esc close" : "Up/Down move    Enter select",
              0xbccadb, 1);
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

    snprintf(hud, sizeof hud, "omp  -  %.36s",
             v->agent_model && v->agent_model[0] ? v->agent_model : "Qwen 3.5 (llama.cpp)");
    art_screen(c, layout_screen(SCREEN_OMP), hud,
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
        art_chest(c, v->chest, (AISLE_X0 + AISLE_X1) / 2 + 16, START_Y - 34, s->goal_met);
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
                 "Arrows/WASD walk   Enter use screen   F1 agent  F2 shell   H hint   Esc menu");
    else
        snprintf(hud, sizeof hud,
                 "Typing into the %s.   F10 or Ctrl+] back to the room   Esc menu",
                 s->focus == FOCUS_OMP ? "agent (omp)" : "shell");
    draw_text(c, 16, HUD_Y + 2, hud, 0xe6e6e6, 1.0f);
}
