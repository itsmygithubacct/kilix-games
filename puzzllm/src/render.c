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

int menu_page_rows(int page, bool neuronte, int *out, int cap)
{
    static const int main_rows[] = {
        MENU_HARNESS, MENU_MODEL, MENU_RESUME, MENU_CAMPAIGN, MENU_SCORES, MENU_DISPLAY, MENU_BENCH, MENU_AUTO, MENU_BETWEEN, MENU_RESETS, MENU_QUIT
    };
    static const int model_rows[] = {
        MENU_CODEX, MENU_GROK, MENU_CLAUDE, MENU_LOCAL, MENU_OLLAMA,
        MENU_MODEL_DETAILS, MENU_THINKING, MENU_COMPUTE, MENU_YOLO, MENU_DOWNLOADS
    };
    static const int score_rows[] = {
        MENU_LEADERBOARD, MENU_CAMPAIGN_BOARD, MENU_CLEAR_CAMPAIGN
    };
    static const int display_rows[] = {
        MENU_TOOLTIPS, MENU_ROOM_BUTTONS, MENU_HINTS_FILE, MENU_NEURONTE
    };
    static const int reset_rows[] = { MENU_RESET_LEVEL, MENU_RESET };
    static const int download_rows[] = {
        MENU_DL_GGUF_08, MENU_DL_GGUF_4B, MENU_DL_GEMMA_E2B, MENU_DL_OLLAMA_06, MENU_DL_OLLAMA_4B, MENU_DL_NAME
    };
    static const int between_rows[] = { MENU_LEVEL_SESSION, MENU_LEVEL_CLEANUP, MENU_CLEAN_DISK };
    static const int bench_rows[] = { MENU_YESNO, MENU_FREETEXT };
    static const int yesno_rows[] = { MENU_BENCH_K12, MENU_BENCH_BOOLQ };
    static const int text_rows[] = { MENU_IFEVAL, MENU_GSM8K, MENU_STOP_TEXT };
    const int *rows = main_rows;
    int n = (int)(sizeof main_rows / sizeof main_rows[0]);
    if (page == MENU_PAGE_MODEL) {
        rows = model_rows;
        n = (int)(sizeof model_rows / sizeof model_rows[0]);
    } else if (page == MENU_PAGE_SCORES) {
        rows = score_rows;
        n = (int)(sizeof score_rows / sizeof score_rows[0]);
    } else if (page == MENU_PAGE_DISPLAY) {
        rows = display_rows;
        n = (int)(sizeof display_rows / sizeof display_rows[0]);
        if (!neuronte) n--;
    } else if (page == MENU_PAGE_RESET) {
        rows = reset_rows;
        n = (int)(sizeof reset_rows / sizeof reset_rows[0]);
    } else if (page == MENU_PAGE_DOWNLOAD) {
        rows = download_rows;
        n = (int)(sizeof download_rows / sizeof download_rows[0]);
    } else if (page == MENU_PAGE_BETWEEN) {
        rows = between_rows;
        n = (int)(sizeof between_rows / sizeof between_rows[0]);
    } else if (page == MENU_PAGE_BENCH) {
        rows = bench_rows;
        n = (int)(sizeof bench_rows / sizeof bench_rows[0]);
    } else if (page == MENU_PAGE_YESNO) {
        rows = yesno_rows;
        n = sizeof yesno_rows / sizeof yesno_rows[0];
    } else if (page == MENU_PAGE_FREETEXT) {
        rows = text_rows;
        n = sizeof text_rows / sizeof text_rows[0];
    }
    if (n > cap) n = cap;
    if (out) for (int i = 0; i < n; i++) out[i] = rows[i];
    return n;
}

static rect menu_bounds(int w, int h, int rows, int *row_h)
{
    const int width = 560, chrome = 125;
    *row_h = 27;
    if (chrome + rows * *row_h > h - 8 && rows > 0) {
        *row_h = (h - 8 - chrome) / rows;
        if (*row_h < 16) *row_h = 16;
    }
    int height = chrome + rows * *row_h, top = (h - height) / 2;
    return (rect){(w - width) / 2, top < 0 ? 0 : top, width, height};
}

int menu_choice_at(int page, int x, int y)
{
    int ids[12], row_h;
    int rows = menu_page_rows(page, draw_neuronte_available(), ids, 12);
    rect panel = menu_bounds(L.w, L.h, rows, &row_h);
    int offset = y - panel.y - 62;
    if (x < panel.x + 11 || x >= panel.x + panel.w - 11 || offset < 0 ||
        offset >= rows * row_h || offset % row_h >= row_h - 2) return -1;
    return ids[offset / row_h];
}

void render_menu(sr_canvas *c, int selected, const char *active_model,
                 bool started, int level, bool confirm_reset, const char *notice, int support, bool thinking_enabled, const char *hosted_agent,
                 bool pi, compute_mode compute, bool tooltips, bool room_buttons,
                 bool campaign, const char *campaign_model, bool hints_file, bool yolo, int page,
                 bool naming, bool auto_play, level_session_mode session_mode, bool level_cleanup)
{
    static const char *const actions[] = {
        [MENU_QUIT] = "Exit game", [MENU_RESET] = "Reset Game",
        [MENU_RESET_LEVEL] = "Reset Level",
        [MENU_MODEL_DETAILS] = "Model prompt and tools",
        [MENU_LEADERBOARD] = "Leaderboard",
        [MENU_CAMPAIGN_BOARD] = "Campaign board",
        [MENU_CLEAR_CAMPAIGN] = "Clear campaign board",
        [MENU_CODEX] = "Codex (OpenAI)",
        [MENU_GROK] = "Grok Build (xAI)",
        [MENU_CLAUDE] = "Claude Code (Anthropic)",
        [MENU_LOCAL] = "llama.cpp models >",
        [MENU_OLLAMA] = "Ollama models >",
        [MENU_DOWNLOADS] = "Downloads >",
        [MENU_DL_GGUF_08] = "Download Qwen3.5-0.8B Q8_0",
        [MENU_DL_GGUF_4B] = "Download Qwen3.5-4B Q4_K_M",
        [MENU_DL_GEMMA_E2B] = "Download Gemma 4 E2B Q4_K_M",
        [MENU_DL_OLLAMA_06] = "Download Ollama qwen3:0.6b",
        [MENU_DL_OLLAMA_4B] = "Download Ollama qwen3:4b-instruct-2507",
        [MENU_DL_NAME] = "Pull another Ollama model",
        [MENU_BENCH] = "Bench >",
        [MENU_YESNO] = "Yes/No >", [MENU_FREETEXT] = "Free Text >",
        [MENU_IFEVAL] = "Run IFEval (instruction following)", [MENU_GSM8K] = "Run GSM8K (math)",
        [MENU_STOP_TEXT] = "Stop free-text run",
        [MENU_BENCH_K12] = BENCH_K12_NAME,
        [MENU_BENCH_BOOLQ] = BENCH_BOOLQ_NAME,
        [MENU_MODEL] = "Model >",
        [MENU_SCORES] = "Scores >",
        [MENU_DISPLAY] = "Display >",
        [MENU_RESETS] = "Reset >",
        [MENU_BETWEEN] = "Between levels >",
        [MENU_CLEAN_DISK] = "Clean up disk now",
    };
    int ids[12];
    int row_h;
    int rows = menu_page_rows(page, draw_neuronte_available(), ids, 12);
    rect panel = menu_bounds(c->w, c->h, rows, &row_h);
    int width = panel.w, height = panel.h, x = panel.x, y = panel.y;
    char label[320];

    draw_fill(c, 0, 0, c->w, c->h, 0x0c1018, 0.72f);
    if (confirm_reset) {
        int top = (c->h - 130) / 2;
        bool level_only = selected == MENU_RESET_LEVEL;
        bool clear_board = selected == MENU_CLEAR_CAMPAIGN;
        art_menu_panel(c, x, top, width, 130);
        draw_text(c, x + 18, top + 16, clear_board ? "Clear campaign board?" :
                  level_only ? "Reset Level?" : "Reset Game?", 0xffde8c, 1);
        draw_text(c, x + 18, top + 46, clear_board ?
                  "Remove ranked campaigns. This run and room scores stay." :
                  level_only ?
                  "Restart this level. The current workspace will be backed up." :
                  "Start at level 1. The current workspace will be backed up.",
                  0xf2efeb, 1);
        draw_text(c, x + 18, top + 93, clear_board ? "Enter clear    Esc cancel" :
                  "Enter reset    Esc cancel", 0xbccadb, 1);
        return;
    }
    art_menu_panel(c, x, y, width, height);
    const char *title = page == MENU_PAGE_MODEL ? "Model" :
                        page == MENU_PAGE_SCORES ? "Scores" :
                        page == MENU_PAGE_DISPLAY ? "Display" :
                        page == MENU_PAGE_RESET ? "Reset" :
                        page == MENU_PAGE_DOWNLOAD ? "Downloads" :
                        page == MENU_PAGE_BENCH ? "Bench" :
                        page == MENU_PAGE_YESNO ? "Yes/No" :
                        page == MENU_PAGE_FREETEXT ? "Free Text" :
                        page == MENU_PAGE_BETWEEN ? "Between levels" : "Game menu";
    draw_text(c, x + 18, y + 13, title, 0xffde8c, 1);
    if (page == MENU_PAGE_BENCH) snprintf(label, sizeof label, "Choose an answer format.");
    else if (page == MENU_PAGE_YESNO) snprintf(label, sizeof label, "Each benchmark has its own rankings.");
    else if (page == MENU_PAGE_FREETEXT) snprintf(label, sizeof label, "Grade generated answers on the local model.");
    else snprintf(label, sizeof label, "Agent model: %s", *active_model ? active_model : "Qwen 3.5 (llama.cpp)");
    text_line(c, x + 18, y + 39, label, 0xbccadb, (width - 36) / CELL_W);

    for (int row = 0; row < rows; row++) {
        int i = ids[row];
        int row_y = y + 62 + row * row_h;
        if (i == selected) {
            draw_fill(c, x + 11, row_y, width - 22, row_h - 2, 0x354356, 1);
            draw_fill(c, x + 11, row_y, 3, row_h - 2, 0xe0c070, 1);
        }
        if (i == MENU_RESUME) {
            if (started) snprintf(label, sizeof label, "Resume game");
            else if (level > 1) snprintf(label, sizeof label, "Continue from level %d", level);
            else snprintf(label, sizeof label, "Start game");
        } else if (i == MENU_THINKING) {
            const char *status = thinking_enabled ? "On" :
                support == THINK_UNAVAILABLE ? "Off (unavailable)" :
                support == THINK_HAS_MINIMUM ? "Off (model minimum)" : "Off";
            if (hosted_agent) snprintf(label, sizeof label, "Thinking: managed in %s", hosted_agent);
            else snprintf(label, sizeof label, "Thinking: %s", status);
        } else if (i == MENU_COMPUTE) {
            const char *mode = compute == COMPUTE_CPU ? "CPU" : compute == COMPUTE_GPU ? "GPU" : "Auto";
            if (hosted_agent) snprintf(label, sizeof label, "Compute: managed in %s", hosted_agent);
            else snprintf(label, sizeof label, "Compute: %s", mode);
        } else if (i == MENU_HARNESS)
            snprintf(label, sizeof label, "Harness: %s", hosted_agent ? hosted_agent : pi ? "Pi (pi.dev)" : "OMP");
        else if (i == MENU_TOOLTIPS)
            snprintf(label, sizeof label, "Tooltips: %s", tooltips ? "On" : "Off");
        else if (i == MENU_ROOM_BUTTONS)
            snprintf(label, sizeof label, "Level buttons: %s", room_buttons ? "On" : "Off");
        else if (i == MENU_HINTS_FILE)
            snprintf(label, sizeof label, "Hint file: %s", hints_file ? "On" : "Off");
        else if (i == MENU_YOLO)
            snprintf(label, sizeof label, "Yolo: %s", yolo ? "On" : "Off");
        else if (i == MENU_LEVEL_SESSION)
            snprintf(label, sizeof label, "Coding session: %s", session_mode == SESSION_CLEAR ? "/clear" :
                     session_mode == SESSION_RESTART ? "Restart" : "Keep");
        else if (i == MENU_LEVEL_CLEANUP)
            snprintf(label, sizeof label, "Clean up workspace each level: %s", level_cleanup ? "On" : "Off");
        else if (i == MENU_AUTO)
            snprintf(label, sizeof label, "Auto: %s", auto_play ? "On" : "Off");
        else if (i == MENU_CAMPAIGN)
            snprintf(label, sizeof label, "Campaign: %s",
                     campaign && campaign_model && *campaign_model ? campaign_model : "Off");
        else if (i == MENU_NEURONTE)
            snprintf(label, sizeof label, "Neuronte v2: %s", draw_neuronte_enabled() ? "On" : "Off");
        else
            snprintf(label, sizeof label, "%s", actions[i]);
        bool model_choice = i == MENU_CODEX || i == MENU_GROK || i == MENU_CLAUDE ||
                            i == MENU_LOCAL || i == MENU_OLLAMA;
        bool disabled = (i == MENU_THINKING && support == THINK_UNAVAILABLE) ||
                        ((i == MENU_COMPUTE || i == MENU_HARNESS) && hosted_agent) ||
                        (campaign && model_choice);
        uint32_t color = disabled ? 0xa6b5c6 : 0xf2efeb;
        text_line(c, x + 20, row_y + 5, label, color, (width - 40) / CELL_W);
    }
    if (*notice)
        text_line(c, x + 18, y + height - 48, notice, 0xa6b5c6, (width - 36) / CELL_W);
    draw_text(c, x + 18, y + height - 26,
              naming ? "Type an Ollama name    Enter pull    Esc cancel" :
              page != MENU_PAGE_MAIN ? "Up/Down move    Enter select    Esc back" :
              started ? "Up/Down move    Enter select    Esc close" : "Up/Down move    Enter select",
              0xbccadb, 1);
}

void render_model_menu(sr_canvas *c, bool ollama, const model_option *models,
                       int count, int selected, bool loading, bool failed,
                       const char *llama_model)
{
    int width = 560, row_h = 27;
    int options = count > 0 ? count : (ollama ? 0 : 1);
    int rows = (c->h - 180) / row_h;
    if (rows > 14) rows = 14;
    if (rows > options) rows = options;
    int first = rows > 0 && selected >= rows ? selected - rows + 1 : 0;
    int height = 110 + rows * row_h;
    int x = (c->w - width) / 2, y = (c->h - height) / 2;
    draw_fill(c, 0, 0, c->w, c->h, 0x0c1018, 0.72f);
    art_menu_panel(c, x, y, width, height);
    draw_text(c, x + 18, y + 13, ollama ? "Ollama models" : "llama.cpp models", 0xffde8c, 1);
    for (int i = first; i < first + rows; i++) {
        int row_y = y + 42 + (i - first) * row_h;
        if (i == selected) {
            draw_fill(c, x + 11, row_y, width - 22, row_h - 2, 0x354356, 1);
            draw_fill(c, x + 11, row_y, 3, row_h - 2, 0xe0c070, 1);
        }
        text_line(c, x + 20, row_y + 5, count > 0 ? models[i].name : llama_model,
                  0xf2efeb, (width - 40) / CELL_W);
    }
    if (ollama && !count) {
        const char *status = loading ? "Looking for installed Ollama models..." :
            failed ? "Could not reach the local Ollama service." : "No installed Ollama models found.";
        text_line(c, x + 18, y + 42, status, 0xa6b5c6, (width - 36) / CELL_W);
    }
    draw_text(c, x + 18, y + height - 26, "Up/Down move    Enter select    Esc back", 0xbccadb, 1);
}

static void score_value(char out[32], long long value, int metric)
{
    if (metric == SCORE_TIME)
        snprintf(out, 32, "%.2fs", (double)value / 1000.0);
    else if (metric == SCORE_CPU || metric == SCORE_GPU)
        snprintf(out, 32, "%lld%%", value);
    else if (metric == SCORE_RAM || metric == SCORE_VRAM || metric == SCORE_DISK)
        snprintf(out, 32, "%.2f MiB%s", (double)value / 1048576.0, metric == SCORE_DISK ? "/s" : "");
    else
        snprintf(out, 32, "%lld", value);
}

int render_leaderboard(sr_canvas *c, const score_entry *scores, int count,
                       int scroll, int level, int metric, bool models, const char *notice,
                       const score_entry *attempt, bool campaign)
{
    int width = CANVAS_W < 1000 ? CANVAS_W - 76 : 900;
    int height = CANVAS_H - 100, x = (CANVAS_W - width) / 2, y = 50;
    int rows = (height - 116) / CELL_H;
    int visible = 0;
    for (int i = 0; i < count; i++) if (scores[i].rank) visible++;
    int limit = visible > rows ? visible - rows : 0;
    if (!c) return limit;
    if (scroll > limit) scroll = limit;
    char text[512];
    int cols = (width - 40) / CELL_W;
    draw_fill(c, 0, 0, CANVAS_W, CANVAS_H, 0x0c1018, 0.72f);
    art_menu_panel(c, x, y, width, height);
    draw_text(c, x + 20, y + 13, campaign ? "Campaign board" :
              models ? "Model leaderboard" : "Player leaderboard", 0xffde8c, 1);
    int measure = models ? metric : SCORE_TOKENS;
    if (campaign)
        snprintf(text, sizeof text, "%d rooms   %s", level, leaderboard_metric[measure]);
    else if (attempt && attempt->id && attempt->level == level)
        snprintf(text, sizeof text, "Room %d   %s   |   Current run  %lld tokens  %.2fs",
                 level, leaderboard_metric[measure], attempt->measure[SCORE_TOKENS],
                 (double)attempt->measure[SCORE_TIME] / 1000.0);
    else snprintf(text, sizeof text, "Room %d   %s", level, leaderboard_metric[measure]);
    text_line(c, x + 20, y + 38, text, 0xbccadb, cols);
    if (*notice || !visible) {
        text_line(c, x + 20, y + 70, *notice ? notice : "No completed runs with this measurement.",
                  0xa6b5c6, cols);
    }
    int row_number = 0;
    for (int i = 0; i < count && row_number < scroll + rows; i++) {
        const score_entry *entry = &scores[i];
        if (!entry->rank) continue;
        int row = row_number++ - scroll;
        if (row < 0) continue;
        int row_y = y + 69 + row * CELL_H;
        char value[32];
        score_value(value, entry->measure[measure], measure);
        snprintf(text, sizeof text, "%3d  %-16.16s  %-51.51s  %s",
                 entry->rank, value, entry->model, entry->player);
        text_line(c, x + 20, row_y, text, entry->rank == 1 ? 0xffdf94 : 0xd0d4dc, cols);
    }
    draw_text(c, x + 20, y + height - 26, campaign ?
              "Left/Right length    M metric    Tab switch board    Up/Down scroll    Esc back" :
              "Left/Right room    M metric    Tab switch board    Up/Down scroll    Esc back",
              0xbccadb, 1);
    return limit;
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

static void details_text(pane_text *pane, model_details_tab tab, const char *prompt,
                         const char *schema, const char *count)
{
    if (tab != DETAILS_CODEX && tab != DETAILS_GROK && tab != DETAILS_CLAUDE) {
        pane_message(pane, "Default system message tokens (Qwen 3.5, text only)", 0xffdf94);
        pane_message(pane, "llama.cpp: OMP 2,460 / Pi 553    Ollama: OMP 256 / Pi 166", 0xd0d4dc);
        pane_message(pane, "Shipped Ollama prompt file: 132 tokens", 0xd0d4dc);
        pane->count++;
        if (*count) pane_message(pane, count, 0x9fd0ff);
    }
    if (tab == DETAILS_OLLAMA) {
        pane_message(pane, "~/.system/ollama-prompt.txt", 0xffdf94);
        details_block(pane, *prompt ? prompt : "Empty file. The game uses the shipped Ollama prompt.");
        pane->count++;
        pane_message(pane, "~/.system/write-tool.json (OMP Ollama write schema)", 0xffdf94);
        details_block(pane, *schema ? schema : "Write-tool schema file unavailable.");
    } else if (tab == DETAILS_LLAMA) {
        pane_message(pane, "~/.system/llama-prompt.txt", 0xffdf94);
        details_block(pane, *prompt ? prompt : "Empty file. The selected harness uses its built-in system prompt.");
    } else if (tab == DETAILS_PI) {
        pane_message(pane, "Pi provides its built-in read, bash, edit and write tools.", 0xd0d4dc);
        pane_message(pane, "Enter edits the prompt file for Pi's selected model backend.", 0xd0d4dc);
        details_block(pane, *prompt ? prompt : "Empty file. Pi uses its built-in system prompt.");
    } else if (tab == DETAILS_GROK) {
        pane_message(pane, "Grok Build supplies its own system prompt and tools.", 0xd0d4dc);
    } else if (tab == DETAILS_CLAUDE) {
        pane_message(pane, "Claude Code supplies its own system prompt and tools.", 0xd0d4dc);
    } else {
        pane_message(pane, "The Codex CLI supplies its own prompt and tools.", 0xd0d4dc);
    }
}

int render_model_details(sr_canvas *c, model_details_tab tab, int scroll,
                         const char *prompt, const char *schema, const char *count)
{
    static const char *const tabs[] = {"Ollama", "llama.cpp", "Codex", "Pi", "Grok", "Claude"};
    int width = CANVAS_W < 1000 ? CANVAS_W - 100 : 880;
    int height = CANVAS_H - 96;
    int x = (CANVAS_W - width) / 2, y = 48;
    pane_text pane = {.cols = (width - 40) / CELL_W, .rows = (height - 116) / CELL_H};
    details_text(&pane, tab, prompt, schema, count);
    int limit = pane.count > pane.rows ? pane.count - pane.rows : 0;
    if (!c) return limit;

    draw_fill(c, 0, 0, CANVAS_W, CANVAS_H, 0x0c1018, 0.72f);
    art_menu_panel(c, x, y, width, height);
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
    details_text(&pane, tab, prompt, schema, count);
    draw_text(c, x + 20, y + height - 29,
              (tab == DETAILS_CODEX || tab == DETAILS_GROK || tab == DETAILS_CLAUDE) ?
              "Left/Right backend    Esc back" :
              "Enter edit prompt    Left/Right tabs    Up/Down scroll    Esc back", 0xbccadb, 1);
    return limit;
}

void render_prompt_editor(sr_canvas *c, const char *draft, size_t cursor, const char *notice)
{
    int width = CANVAS_W < 1000 ? CANVAS_W - 100 : 880;
    int height = CANVAS_H - 96;
    int x = (CANVAS_W - width) / 2, y = 48;
    int cols = (width - 40) / CELL_W, rows = (height - 112) / CELL_H;
    int cursor_row = 0, cursor_col = 0;
    for (size_t i = 0; i < cursor; i++) {
        unsigned char ch = (unsigned char)draft[i];
        if (ch == '\n') { cursor_row++; cursor_col = 0; }
        else if ((ch & 0xc0) != 0x80 && ++cursor_col == cols) {
            cursor_row++;
            cursor_col = 0;
        }
    }
    int first = cursor_row >= rows ? cursor_row - rows + 1 : 0;
    draw_fill(c, 0, 0, CANVAS_W, CANVAS_H, 0x0c1018, 0.72f);
    art_menu_panel(c, x, y, width, height);
    draw_text(c, x + 20, y + 14, "Edit system prompt", 0xffde8c, 1);
    for (int line = 0; *draft && line < first + rows; line++) {
        const char *end = draft;
        int col = 0;
        for (; *end && *end != '\n' && col < cols; col++) {
            do end++; while (((unsigned char)*end & 0xc0) == 0x80);
        }
        if (line >= first) {
            char text[MODEL_SPEC_CAP];
            memcpy(text, draft, (size_t)(end - draft));
            text[end - draft] = 0;
            text_line(c, x + 20, y + 49 + (line - first) * CELL_H, text, 0xd0d4dc, cols);
        }
        draft = *end == '\n' && col < cols ? end + 1 : end;
    }
    draw_fill(c, x + 20 + cursor_col * CELL_W, y + 49 + (cursor_row - first) * CELL_H,
              2, CELL_H, 0xffdf94, 1);
    text_line(c, x + 20, y + height - 49, notice, 0xffdf94, cols);
    draw_text(c, x + 20, y + height - 27,
              "Ctrl+S save    Ctrl+U clear    Esc cancel    Paste supported", 0xbccadb, 1);
}

static void render_resources(sr_canvas *c, const view *v)
{
    rect r = layout_resources();
    int cols = (r.w - 2 * SCREEN_PAD) / CELL_W;
    char tokens[32] = "Tokens --";
    if (v->act && v->act->tokens_reported) {
        unsigned long long n = v->act->tokens_used;
        if (n < 1000000)
            snprintf(tokens, sizeof tokens, "Tokens %llu", n);
        else if (n < 1000000000)
            snprintf(tokens, sizeof tokens, "Tokens %.1fM", (double)n / 1000000.0);
        else
            snprintf(tokens, sizeof tokens, "Tokens %.1fB", (double)n / 1000000000.0);
    }
    int token_cols = (int)strlen(tokens);
    int metrics_cols = cols - token_cols - 2;
    const char *summary = v->resource_summary ? v->resource_summary : "Resources   Agent idle";
    if (strlen(summary) > (size_t)metrics_cols) {
        const char *cpu = strstr(summary, "CPU ");
        if (cpu) summary = cpu;
    }
    draw_fill(c, r.x, r.y, r.w, r.h, 0x171e27, 1);
    draw_stroke(c, r.x, r.y, r.w, r.h, 1, 0x3b4a5c, 1);
    text_line(c, r.x + SCREEN_PAD, r.y + 2, summary, 0xa8d7f2, metrics_cols);
    text_line(c, r.x + SCREEN_PAD + (cols - token_cols) * CELL_W, r.y + 2,
              tokens, 0xa8d7f2, token_cols);
    text_line(c, r.x + SCREEN_PAD, r.y + 18,
              v->resource_devices && *v->resource_devices ? v->resource_devices : "GPU --  VRAM --  Disk --/--",
              0xbdc8d7, cols);
}

static void render_room_buttons(sr_canvas *c, const scene *s, bool auto_on)
{
    static const char *const labels[ROOM_BUTTON_COUNT] = {
        "Clear model", "Clear shell", "Harness", "Model",
        "Shell log", "Reset level", "Reset game", "Metrics", "Auto", "Clean disk",
    };
    room_button aimed = s->focus == FOCUS_ROOM ? scene_room_button_at(s->x, s->y) : BUTTON_NONE;
    for (int i = 0; i < ROOM_BUTTON_COUNT; i++) {
        rect r = layout_room_button((room_button)i);
        bool active = (int)aimed == i;
        bool on = auto_on && i == BUTTON_AUTO;
        const char *label = on ? "Auto on" : labels[i];
        draw_fill(c, r.x + 2, r.y + 2, r.w, r.h, 0x10131e, 0.8f);
        draw_fill(c, r.x, r.y, r.w, r.h, active ? 0x5a4a24 : on ? 0x1d4a32 : 0x2a2f45, 1);
        draw_stroke(c, r.x, r.y, r.w, r.h, 1, active ? 0xd9ba79 : on ? 0x8fd0a8 : 0x8f8fd0, 1);
        int width = (int)strlen(label) * CELL_W;
        draw_text(c, r.x + (r.w - width) / 2, r.y + 2, label, 0xffffff, 1);
    }
}

void render_frame(sr_canvas *c, const view *v)
{
    const scene *s = v->sc;
    screen_id near = s->focus == FOCUS_ROOM ? scene_near_screen(s) : SCREEN_NONE;
    int tx, ty;
    char hud[256];

    room_background(c, v->room, v->lv->room);
    art_door(c, v->room, v->lv->room, s->door, v->next_room);

    if (v->hosted_agent) snprintf(hud, sizeof hud, "%s agent", v->hosted_agent);
    else snprintf(hud, sizeof hud, "%s  -  %.36s", v->pi ? "Pi" : "omp",
                  v->agent_model && v->agent_model[0] ? v->agent_model : "Qwen 3.5 (llama.cpp)");
    art_screen(c, layout_screen(SCREEN_OMP), hud,
               s->focus == FOCUS_OMP, near == SCREEN_OMP);
    layout_text_origin(SCREEN_OMP, &tx, &ty);
    if (v->agent_px) {
        rect r = layout_screen(SCREEN_OMP);
        draw_picture(c, v->agent_px, v->agent_pw, v->agent_ph, r.x, r.y, r.w, r.h, 1);
    } else if (v->omp) {
        vt_cells(c, v->omp, v->omp_history, tx, ty, s->focus == FOCUS_OMP && v->blink);
    }
    if (v->omp_note) note_over(c, SCREEN_OMP, v->omp_note);

    art_screen(c, layout_screen(SCREEN_SHELL), "shell  -  sandbox home",
               s->focus == FOCUS_SHELL, near == SCREEN_SHELL);
    layout_text_origin(SCREEN_SHELL, &tx, &ty);
    if (v->shell_px) {
        rect r = layout_screen(SCREEN_SHELL);
        draw_picture(c, v->shell_px, v->shell_pw, v->shell_ph, r.x, r.y, r.w, r.h, 1);
    } else if (v->shell) {
        vt_cells(c, v->shell, v->shell_history, tx, ty, s->focus == FOCUS_SHELL && v->blink);
    }
    if (v->shell_note) note_over(c, SCREEN_SHELL, v->shell_note);

    snprintf(hud, sizeof hud, "Goal  |  Room %d: %.100s", v->lv->number, v->lv->title);
    art_screen(c, layout_screen(SCREEN_GOAL), hud, false, near == SCREEN_GOAL);
    layout_text_origin(SCREEN_GOAL, &tx, &ty);
    render_pane_text(c, v, SCREEN_GOAL, tx, ty, NULL);

    art_screen(c, layout_screen(SCREEN_ACT), "System actions", false, false);
    layout_text_origin(SCREEN_ACT, &tx, &ty);
    render_pane_text(c, v, SCREEN_ACT, tx, ty, NULL);
    render_resources(c, v);

    render_selection(c, v->selection);
    if (v->room_buttons) render_room_buttons(c, s, v->auto_play);

    art_kilix(c, s->x, s->y, s->dir, s->step_ticks, s->idle_ticks);
    if (v->tooltips && s->focus == FOCUS_ROOM) {
        if (near == SCREEN_OMP) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: talk to the agent");
        else if (near == SCREEN_SHELL) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: use the shell");
        else if (near == SCREEN_GOAL) art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "Enter: next hint");
        else if (scene_door_passable(s) && s->x >= AISLE_X0 && s->x <= AISLE_X1)
            art_prompt(c, s->x, s->y - ART_KILIX_HEIGHT - 6, "The door is open: go up!");
    }

    if (s->focus == FOCUS_ROOM)
        snprintf(hud, sizeof hud,
                 "Arrows/WASD walk   Enter use   Alt+Left agent   Alt+Right shell   H hint   Esc menu");
    else
        snprintf(hud, sizeof hud,
                 "%s focused   Ctrl+Shift: F1 agent / F2 shell / F10 room / F12 menu",
                 s->focus == FOCUS_OMP ? "agent" : "shell");
    draw_text(c, 16, HUD_Y + 2, hud, 0xe6e6e6, 1.0f);
}
