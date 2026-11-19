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
static void vt_cells(sr_canvas *c, const neuroterm *t, const history *scroll, int x, int y, bool show_cursor)
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
            if (!(scroll ? history_cell(scroll, col, row, &cell) : neuroterm_cell(t, col, row, &cell))) {
                col++;
                continue;
            }
            int span = cell.width == 2 && col + 1 < t->cols ? 2 : 1;
            style.cursor = show_cursor && (!scroll || !scroll->offset) && t->cursor_visible && row == cursor.row &&
                           cursor.col >= col && cursor.col < col + span;
            neuroterm_paint_cell(t->screen, &cell, x + col * CELL_W, y + row * CELL_H,
                                 CELL_W, CELL_H, span, &style, &painter);
            col += span;
        }
    }
}

/* ---- the goal and system actions panes ---- */

/* Split on spaces where possible, preserving UTF-8 character boundaries. */
static const char *wrap_line(const char *s, int cols, char *line)
{
    const char *p = s;
    const char *space = NULL;
    for (int i = 0; *p && i < cols; i++) {
        if (*p == ' ' && i > cols / 3) space = p;
        p++;
        while (((unsigned char)*p & 0xc0) == 0x80) p++;
    }
    if (*p && *p != ' ' && space) p = space;
    memcpy(line, s, (size_t)(p - s));
    line[p - s] = 0;
    while (*p == ' ') p++;
    return p;
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

typedef struct pane_text {
    sr_canvas *canvas;
    char (*copy)[LEVEL_TEXT + 32];
    int x, y, cols, rows, first, count;
} pane_text;

static void pane_message(pane_text *pane, const char *text, uint32_t color)
{
    char line[LEVEL_TEXT + 32];
    while (*text) {
        text = wrap_line(text, pane->cols, line);
        int row = pane->count++ - pane->first;
        if (row < 0 || row >= pane->rows) continue;
        if (pane->copy) strcpy(pane->copy[row], line);
        if (pane->canvas)
            text_line(pane->canvas, pane->x, pane->y + row * CELL_H, line, color, pane->cols);
    }
}

static void goal_text(pane_text *pane, const view *v)
{
    char buf[LEVEL_TEXT + 32];
    pane_message(pane, v->lv->goal, 0xffffff);
    if (v->lv->check == CHECK_NONE)
        snprintf(buf, sizeof buf, "COMING SOON: the next challenge is not built yet.");
    else if (v->sc->goal_met)
        snprintf(buf, sizeof buf, "DONE: %.220s. The door is open: walk up the aisle.",
                 v->goal_why ? v->goal_why : "goal met");
    else
        snprintf(buf, sizeof buf, "NOT YET: %s", v->goal_why ? v->goal_why : "");
    uint32_t status = v->lv->check == CHECK_NONE ? 0xffe27a : v->sc->goal_met ? 0x60f080 : 0xff9f80;
    pane_message(pane, buf, status);
    if (v->hints_shown > 0 && v->lv->nhints > 0) {
        int h = v->hints_shown > v->lv->nhints ? v->lv->nhints : v->hints_shown;
        snprintf(buf, sizeof buf, "HINT %d/%d: %s", h, v->lv->nhints, v->lv->hints[h - 1]);
        pane_message(pane, buf, 0x9fd0ff);
    }
}

static void action_text(pane_text *pane, const view *v)
{
    for (int i = 0; v->act && i < v->act->count; i++) {
        const act_line *line = activity_line(v->act, i);
        if (line->kind != ACT_GOAL)
            pane_message(pane, line->text, kind_color(line->kind));
    }
    if (pane->count == 0)
        pane_message(pane, "Tool calls and file changes appear here.", 0x6a7383);
}

int render_scroll_limit(const view *v, screen_id id)
{
    pane_text pane = {
        .cols = id == SCREEN_GOAL ? GOAL_COLS : ACT_COLS,
        .rows = id == SCREEN_GOAL ? GOAL_ROWS : ACT_ROWS,
    };
    if (id == SCREEN_GOAL) goal_text(&pane, v);
    else action_text(&pane, v);
    return pane.count > pane.rows ? pane.count - pane.rows : 0;
}

static void render_pane_text(sr_canvas *c, const view *v, screen_id id, int x, int y,
                             char (*copy)[LEVEL_TEXT + 32])
{
    bool goal = id == SCREEN_GOAL;
    int limit = render_scroll_limit(v, id);
    int offset = goal ? v->goal_scroll : v->action_scroll;
    if (offset > limit) offset = limit;
    pane_text pane = {
        .canvas = c, .copy = copy, .x = x, .y = y,
        .cols = goal ? GOAL_COLS : ACT_COLS, .rows = goal ? GOAL_ROWS : ACT_ROWS,
        .first = goal ? offset : limit - offset,
    };
    if (goal) goal_text(&pane, v);
    else action_text(&pane, v);
}

static void selection_range(pane_selection selection, int *first, int *last)
{
    bool forward = selection.start <= selection.end;
    *first = forward ? selection.start : selection.end;
    *last = forward ? selection.end : selection.start;
}

static size_t copy_terminal_row(const history *h, int row, int start, int end, char *out)
{
    size_t n = 0;
    if (start > 0) {
        VTermScreenCell cell;
        if (history_cell(h, start - 1, row, &cell) && cell.width == 2) start--;
    }
    for (int col = start; col <= end;) {
        VTermScreenCell cell;
        if (!history_cell(h, col, row, &cell)) break;
        if (!cell.chars[0]) {
            out[n++] = ' ';
        } else {
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
                n += keys_utf8(cell.chars[i], out + n);
        }
        col += cell.width == 2 ? 2 : 1;
    }
    while (n && out[n - 1] == ' ') n--;
    return n;
}

static size_t copy_text_row(const char *text, int start, int end, char *out)
{
    const char *first = NULL;
    const char *p = text;
    for (int col = 0; *p && col <= end; col++) {
        if (col == start) first = p;
        p++;
        while (((unsigned char)*p & 0xc0) == 0x80) p++;
    }
    if (!first) return 0;
    memcpy(out, first, (size_t)(p - first));
    return (size_t)(p - first);
}

size_t render_copy_selection(const view *v, char *out, size_t cap)
{
    if (!v->selection.active || !cap) return 0;
    screen_id id = v->selection.pane;
    rect bounds = layout_text_bounds(id);
    int cols = bounds.w / CELL_W;
    int first, last;
    selection_range(v->selection, &first, &last);
    const history *terminal = id == SCREEN_OMP ? v->omp_history :
                              id == SCREEN_SHELL ? v->shell_history : NULL;
    char text[SCREEN_TEXT_ROWS_MAX][LEVEL_TEXT + 32] = {{0}};
    if (!terminal) render_pane_text(NULL, v, id, 0, 0, text);

    size_t used = 0;
    for (int row = first / cols; row <= last / cols; row++) {
        char line[SCREEN_MAX_COLS * VTERM_MAX_CHARS_PER_CELL * 4];
        int start = row == first / cols ? first % cols : 0;
        int end = row == last / cols ? last % cols : cols - 1;
        size_t n = terminal ? copy_terminal_row(terminal, row, start, end, line) :
                             copy_text_row(text[row], start, end, line);
        if (n + (row != first / cols) >= cap - used) break;
        if (row != first / cols) out[used++] = '\n';
        memcpy(out + used, line, n);
        used += n;
    }
    out[used] = 0;
    return used;
}

static void render_selection(sr_canvas *c, pane_selection selection)
{
    if (!selection.active) return;
    rect r = layout_text_bounds(selection.pane);
    int cols = r.w / CELL_W, first, last;
    selection_range(selection, &first, &last);
    for (int row = first / cols; row <= last / cols; row++) {
        int left = row == first / cols ? first % cols : 0;
        int right = row == last / cols ? last % cols : cols - 1;
        draw_fill(c, r.x + left * CELL_W, r.y + row * CELL_H,
                  (right - left + 1) * CELL_W, CELL_H, 0x68abef, 0.30f);
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

/* The floor, walls and runner change only with the room's art, the frame
 * size and the renderer, so they are drawn once and copied each frame. */
static void room_background(sr_canvas *c, const room_art *art, const char *style)
{
    static sr_canvas bg;
    static char bg_style[32];
    static char bg_renderer[16];
    static unsigned bg_revision;
    unsigned revision = art ? art->revision : 0;
    char renderer[16];

    snprintf(renderer, sizeof renderer, "%.15s", draw_renderer());
    if (bg.px == NULL || bg.w != c->w || bg.h != c->h ||
        bg_revision != revision || strcmp(bg_style, style ? style : "") != 0 || strcmp(bg_renderer, renderer) != 0) {
        sr_canvas_free(&bg);
        if (!sr_canvas_init(&bg, c->w, c->h)) {
            art_room(c, art, style);
            art_screen_glass(c, layout_screen(SCREEN_OMP), TERM_BG);
            art_screen_glass(c, layout_screen(SCREEN_SHELL), TERM_BG);
            art_screen_glass(c, layout_screen(SCREEN_ACT), ACT_BG);
            art_screen_glass(c, layout_screen(SCREEN_GOAL), ACT_BG);
            return;
        }
        art_room(&bg, art, style);
        art_screen_glass(&bg, layout_screen(SCREEN_OMP), TERM_BG);
        art_screen_glass(&bg, layout_screen(SCREEN_SHELL), TERM_BG);
        art_screen_glass(&bg, layout_screen(SCREEN_ACT), ACT_BG);
        art_screen_glass(&bg, layout_screen(SCREEN_GOAL), ACT_BG);
        bg_revision = revision;
        snprintf(bg_style, sizeof bg_style, "%s", style ? style : "");
        snprintf(bg_renderer, sizeof bg_renderer, "%s", renderer);
    }
    memcpy(c->px, bg.px, (size_t)c->w * (size_t)c->h * sizeof *c->px);
}

void render_menu(sr_canvas *c, const model_option *models, int count, int selected,
                 const char *active_model, bool loading, bool failed,
                 bool started, int level, bool confirm_reset, const char *notice, int support, bool thinking_enabled, bool codex,
                 compute_mode compute)
{
    static const char *const actions[] = {
        [MENU_QUIT] = "Exit game", [MENU_RESET] = "Reset progress",
        [MENU_MODEL_DETAILS] = "Model prompt and tools",
        [MENU_CODEX] = "Codex (OpenAI)",
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
            const char *status = codex ? "managed in Codex" : thinking_enabled ? "On" :
                support == THINK_UNAVAILABLE ? "Off (unavailable)" :
                support == THINK_HAS_MINIMUM ? "Off (model minimum)" : "Off";
            snprintf(label, sizeof label, "Thinking: %s", status);
        } else if (i == MENU_COMPUTE) {
            const char *mode = compute == COMPUTE_CPU ? "CPU" : compute == COMPUTE_GPU ? "GPU" : "Auto";
            snprintf(label, sizeof label, "Compute: %s", codex ? "managed in Codex" : mode);
        } else if (i < MENU_FIRST_OLLAMA)
            snprintf(label, sizeof label, "%s", actions[i]);
        else
            snprintf(label, sizeof label, "%s (Ollama)", models[i - MENU_FIRST_OLLAMA].name);
        bool disabled = (i == MENU_THINKING && support == THINK_UNAVAILABLE) ||
                        (i == MENU_COMPUTE && codex);
        uint32_t color = disabled ? 0xa6b5c6 : 0xf2efeb;
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

static void details_block(pane_text *pane, const char *text)
{
    while (*text) {
        char line[MODEL_SPEC_CAP];
        size_t length = strcspn(text, "\n");
        if (length) {
            memcpy(line, text, length);
            line[length] = 0;
            pane_message(pane, line, 0xd0d4dc);
        } else {
            pane->count++;
        }
        text += length;
        if (*text) text++;
    }
}

static void details_text(pane_text *pane, model_details_tab tab, const char *prompt, const char *schema)
{
    if (tab == DETAILS_OLLAMA) {
        pane_message(pane, "PuzzLLM system prompt", 0xffdf94);
        details_block(pane, *prompt ? prompt : "System prompt file unavailable.");
        pane->count++;
        pane_message(pane, "Ollama write-tool schema", 0xffdf94);
        details_block(pane, *schema ? schema : "Write-tool schema file unavailable.");
    } else if (tab == DETAILS_LLAMA) {
        pane_message(pane, "omp supplies its built-in system prompt and tool definitions to llama.cpp.", 0xd0d4dc);
        pane_message(pane, "The Ollama tab shows the prompt and write schema configured by PuzzLLM.", 0xd0d4dc);
    } else {
        pane_message(pane, "The Codex CLI supplies its own prompt and tools.", 0xd0d4dc);
    }
}

int render_model_details(sr_canvas *c, model_details_tab tab, int scroll,
                         const char *prompt, const char *schema)
{
    static const char *const tabs[] = {"Ollama", "llama.cpp", "Codex"};
    int width = CANVAS_W < 1000 ? CANVAS_W - 100 : 880;
    int height = CANVAS_H - 96;
    int x = (CANVAS_W - width) / 2, y = 48;
    pane_text pane = {.cols = (width - 40) / CELL_W, .rows = (height - 116) / CELL_H};
    details_text(&pane, tab, prompt, schema);
    int limit = pane.count > pane.rows ? pane.count - pane.rows : 0;
    if (!c) return limit;

    draw_fill(c, 0, 0, CANVAS_W, CANVAS_H, 0x0c1018, 0.72f);
    draw_fill(c, x, y, width, height, 0x151d29, 1);
    draw_stroke(c, x, y, width, height, 2, 0x826b47, 1);
    draw_text(c, x + 20, y + 14, "Model prompt and tools", 0xffde8c, 1);
    for (int i = 0; i < DETAILS_COUNT; i++) {
        int tx = x + 20 + i * 104;
        if (i == (int)tab) draw_fill(c, tx - 5, y + 39, 96, 22, 0x354356, 1);
        draw_text(c, tx, y + 42, tabs[i], i == (int)tab ? 0xffde8c : 0xa6b5c6, 1);
    }
    pane.canvas = c;
    pane.x = x + 20;
    pane.y = y + 72;
    pane.first = scroll > limit ? limit : scroll;
    pane.count = 0;
    details_text(&pane, tab, prompt, schema);
    draw_text(c, x + 20, y + height - 29,
              "Left/Right backend    Up/Down or wheel scroll    Esc back", 0xbccadb, 1);
    return limit;
}

void render_frame(sr_canvas *c, const view *v)
{
    const scene *s = v->sc;
    screen_id near = s->focus == FOCUS_ROOM ? scene_near_screen(s) : SCREEN_NONE;
    int tx, ty;
    char hud[256];

    room_background(c, v->room, v->lv->room);
    art_door(c, v->room, v->lv->room, s->door,
             v->next_level ? v->next_level->number : 0,
             v->next_level ? v->next_level->room : NULL);

    if (v->codex) snprintf(hud, sizeof hud, "Codex  -  OpenAI agent");
    else snprintf(hud, sizeof hud, "omp  -  %.36s",
                  v->agent_model && v->agent_model[0] ? v->agent_model : "Qwen 3.5 (llama.cpp)");
    art_screen(c, layout_screen(SCREEN_OMP), hud,
               s->focus == FOCUS_OMP, near == SCREEN_OMP);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    if (v->omp) vt_cells(c, v->omp, v->omp_history, tx, ty, s->focus == FOCUS_OMP && v->blink);
    if (v->omp_note) note_over(c, SCREEN_OMP, v->omp_note);

    art_screen(c, layout_screen(SCREEN_SHELL), "shell  -  sandbox workspace",
               s->focus == FOCUS_SHELL, near == SCREEN_SHELL);
    layout_text_origin(SCREEN_SHELL, &tx, &ty);
    if (v->shell) vt_cells(c, v->shell, v->shell_history, tx, ty, s->focus == FOCUS_SHELL && v->blink);
    if (v->shell_note) note_over(c, SCREEN_SHELL, v->shell_note);

    snprintf(hud, sizeof hud, "Goal  |  Room %d: %.100s", v->lv->number, v->lv->title);
    art_screen(c, layout_screen(SCREEN_GOAL), hud, false, near == SCREEN_GOAL);
    layout_text_origin(SCREEN_GOAL, &tx, &ty);
    render_pane_text(c, v, SCREEN_GOAL, tx, ty, NULL);

    art_screen(c, layout_screen(SCREEN_ACT), "System actions", false, false);
    layout_text_origin(SCREEN_ACT, &tx, &ty);
    render_pane_text(c, v, SCREEN_ACT, tx, ty, NULL);

    render_selection(c, v->selection);

    if (v->lv->check == CHECK_CHEST)
        art_chest(c, v->chest, (AISLE_X0 + AISLE_X1) / 2 + 48, START_Y - 34, s->goal_met);
    art_kilix(c, s->x, s->y, s->dir, s->step_ticks, s->idle_ticks);
    if (s->focus == FOCUS_ROOM) {
        if (near == SCREEN_OMP) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: talk to the agent");
        else if (near == SCREEN_SHELL) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: use the shell");
        else if (near == SCREEN_GOAL) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: next hint");
        else if (scene_door_passable(s) && s->x >= AISLE_X0 && s->x <= AISLE_X1)
            art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "The door is open: go up!");
    }

    if (s->focus == FOCUS_ROOM)
        snprintf(hud, sizeof hud,
                 "Arrows/WASD walk   Alt+Left agent   Alt+Right shell   H hint   Esc menu");
    else
        snprintf(hud, sizeof hud,
                 "%s focused   Alt+Left agent   Alt+Right shell   Alt+Up room   Esc menu",
                 s->focus == FOCUS_OMP ? "agent" : "shell");
    draw_text(c, 16, HUD_Y + 2, hud, 0xe6e6e6, 1.0f);
}
