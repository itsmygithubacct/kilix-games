/* A full-canvas monitor with the same hit rectangles for drawing and input. */
#include "yesno.h"
#include "art.h"
#include "draw.h"
#include <stdio.h>
#include <string.h>

enum { BG=0x101821, PANEL=0x192530, LINE=0x344753, INK=0xebf1ef,
       MUTED=0xa4b8be, AQUA=0x70dfd3, GOLD=0xf6d388, RED=0xffa692, PAD=26 };

rect yesno_button_rect(int w, int h, int button)
{
    int slot = (w - 128) / BENCH_BUTTONS;
    if (slot > 186) slot = 186;
    return (rect){100 + button * slot, h - 60, slot - 8, 37};
}

rect yesno_history_rect(int w, int h)
{
    return (rect){PAD, 225, w / 2 - PAD - 8, h - 106 - 225};
}

rect yesno_picker_rect(int w, int h)
{
    return (rect){PAD, 133, w - 2 * PAD, h - 106 - 133};
}

/* One UTF-8 line, measured in cells as in the room renderer. */
static const char *line(sr_canvas *c, int x, int y, int width, const char *text, unsigned color, bool wrap)
{
    char buf[1024];
    int cols = width / CELL_W;
    const char *p = text, *space = NULL;
    while (*p && cols-- > 0) {
        if (*p == ' ') space = p;
        p++;
        while (((unsigned char)*p & 0xc0) == 0x80) p++;
        if ((size_t)(p-text) >= sizeof buf - 4) break;
    }
    if (wrap && *p && *p != ' ' && space && space > text) p = space;
    size_t len = (size_t)(p-text);
    memcpy(buf, text, len);
    buf[len] = 0;
    draw_text(c, x, y, buf, color, 1);
    while (*p == ' ') p++;
    return p;
}

static int paragraph(sr_canvas *c, int x, int y, int width, const char *text, int max_rows, unsigned color)
{
    for (int i = 0; i < max_rows && *text; i++, y += 20)
        text = line(c, x, y, width, text, color, true);
    return y;
}

static void dock(sr_canvas *c, const yesno_bench *b)
{
    bool active = b->status == BENCH_RUNNING && !b->replay;
    const char *buttons[BENCH_BUTTONS] = {active ? "M  Next model" : "M  Model", active ? "R  Stop" : "R  Start",
        b->misses ? "F  Show all" : "F  Misses only", "L  Latest", "B  Rankings",
        b->standalone && b->page == BENCH_WATCH ? "Esc  Close" : "Esc  Back"};
    if (b->page != BENCH_WATCH) {
        buttons[BENCH_MODEL] = NULL;
        if (b->page != BENCH_REPORT) buttons[BENCH_FILTER] = buttons[BENCH_LATEST] = NULL;
        buttons[BENCH_RANKINGS] = b->page == BENCH_RANKS ? "U  Refresh" : NULL;
        buttons[BENCH_START] = b->page == BENCH_RANKS ? "Enter  Runs" :
                               b->page == BENCH_RUNS ? "Enter  Answers" :
                               b->page == BENCH_REPORT ? NULL : "Enter  Select";
    }
    draw_fill(c, 0, c->h-95, c->w, 95, PANEL, 1);
    draw_fill(c, 0, c->h-95, c->w, 1, LINE, 1);
    art_kilix(c, 54, c->h - 9, 0, 0, 0);
    for (int i=0; i<BENCH_BUTTONS; i++) {
        if (!buttons[i]) continue;
        rect r = yesno_button_rect(c->w, c->h, i);
        bool primary = i == BENCH_START && !active;
        unsigned fill = primary ? AQUA : 0x263743;
        unsigned fg = primary ? BG : INK;
        draw_fill(c, r.x, r.y, r.w, r.h, fill, 1);
        line(c, r.x+11, r.y+10, r.w-16, buttons[i], fg, false);
    }
}

static void picker(sr_canvas *c, const yesno_bench *b)
{
    rect r = yesno_picker_rect(c->w, c->h);
    const char *title = b->page == BENCH_PROVIDERS ? "Choose a provider" : yesno_provider(b->picking);
    draw_text(c, PAD, 56, title, AQUA, 1);
    if (b->page == BENCH_ENTRY) {
        const char *caption = b->picking == 0 ? "Ollama model tag (missing models are pulled on Start)" :
                              b->picking == 1 ? "Path to a GGUF model file" : "Model ID";
        draw_text(c, PAD, 100, caption, MUTED, 1);
        draw_fill(c, PAD, 131, r.w, 46, PANEL, 1);
        draw_stroke(c, PAD, 131, r.w, 46, 1, AQUA, 1);
        const char *tail = b->entry;
        while (*tail && draw_text_width(tail) > r.w-48) {
            tail++;
            while (((unsigned char)*tail & 0xc0) == 0x80) tail++;
        }
        draw_text(c, PAD+15, 146, tail, INK, 1);
        draw_fill(c, PAD+16+draw_text_width(tail), 146, 2, 16, AQUA, 1);
        return;
    }
    const char *hint = "Select a row or use the arrow keys and Enter.";
    if (b->page == BENCH_MODELS && b->catalog.process.pid > 0) hint = "Loading available models...";
    if (b->page == BENCH_MODELS && b->catalog_notice[0]) hint = b->catalog_notice;
    line(c, PAD, 91, r.w, hint, MUTED, false);
    int rows = r.h / BENCH_ROW_H, first = yesno_picker_first(b, rows);
    for (int row=0; row<rows && first+row<yesno_options(b); row++) {
        int i = first+row, y = r.y + row*BENCH_ROW_H;
        bool selected = i == b->cursor;
        draw_fill(c, r.x, y, r.w, BENCH_ROW_H-1, selected ? 0x304853 : PANEL, 1);
        if (selected) draw_fill(c, r.x, y, 3, BENCH_ROW_H-1, AQUA, 1);
        const char *option = yesno_option(b, i);
        /* Keep the distinguishing filename visible for long absolute paths. */
        const char *leaf = b->page == BENCH_MODELS && b->picking == 1 && i ? strrchr(option, '/') : NULL;
        if (leaf && draw_text_width(option) > r.w-36) option = leaf+1;
        line(c, r.x+14, y+4, r.w-28, option, selected ? INK : MUTED, false);
    }
}

static void records(sr_canvas *c, const yesno_bench *b)
{
    rect r = yesno_picker_rect(c->w, c->h);
    bool ranks = b->page == BENCH_RANKS;
    const bench_standing *group = ranks ? NULL : &b->standings[b->standing_index];
    draw_text(c, PAD, 54, ranks ? "Model rankings" : "Run history", AQUA, 1);
    char text[4500];
    if (ranks) {
        snprintf(text, sizeof text, "%d model%s   /   %d run%s     Ranked by mean accuracy across finished runs.",
                 b->standing_count, b->standing_count == 1 ? "" : "s", b->run_count, b->run_count == 1 ? "" : "s");
    } else {
        snprintf(text, sizeof text, "%s / %s", group->provider, *group->model ? group->model : "Unrecorded default");
    }
    line(c, PAD, 81, r.w, b->catalog_notice[0] ? b->catalog_notice : text,
         b->catalog_notice[0] ? RED : MUTED, false);
    int cols[] = {r.x+10, r.x+76, r.x+r.w-365, r.x+r.w-294, r.x+r.w-207, r.x+r.w-89, r.x+r.w};
    int run_cols[] = {r.x+10, r.x+r.w/3, r.x+2*r.w/3, r.x+5*r.w/6, r.x+r.w};
    const char *headings[] = {"Rank", "Model", "Runs", "Finished", "Avg correct", "Avg %"};
    const char *run_headings[] = {"Started (UTC)", "Status", "Answered", "Correct"};
    for (int i=0; i<(ranks ? 6 : 4); i++)
        draw_text(c, ranks ? cols[i] : run_cols[i], r.y-23, ranks ? headings[i] : run_headings[i], MUTED, 1);
    int rows = r.h / BENCH_ROW_H, first = yesno_picker_first(b, rows);
    int count = yesno_options(b);
    if (!count) {
        const char *message = b->catalog.process.pid ? "Reading saved runs..." : "No benchmark runs recorded.";
        line(c, r.x+10, r.y+7, r.w-20, message, MUTED, false);
        return;
    }
    for (int row=0; row<rows && first+row<count; row++) {
        int i = first+row, y = r.y+row*BENCH_ROW_H;
        bool selected = i == b->cursor;
        draw_fill(c, r.x, y, r.w, BENCH_ROW_H-1, selected ? 0x304853 : PANEL, 1);
        if (selected) draw_fill(c, r.x, y, 3, BENCH_ROW_H-1, AQUA, 1);
        y += 4;
        if (ranks) {
            const bench_standing *s = &b->standings[i];
            if (s->rank) snprintf(text, sizeof text, "%d", s->rank);
            draw_text(c, cols[0], y, s->rank ? text : "-", GOLD, 1);
            const char *model = s->model;
            const char *leaf = !strcmp(s->provider, "llama.cpp") ? strrchr(model, '/') : NULL;
            snprintf(text, sizeof text, "%s / %s", *s->provider ? s->provider : "Unknown",
                     leaf ? leaf+1 : *model ? model : "Unrecorded default");
            line(c, cols[1], y, cols[2]-cols[1]-8, text, INK, false);
            snprintf(text, sizeof text, "%d", s->runs);
            draw_text(c, cols[2], y, text, INK, 1);
            snprintf(text, sizeof text, "%d", s->finished);
            draw_text(c, cols[3], y, text, MUTED, 1);
            snprintf(text, sizeof text, "%.1f", s->avg_correct);
            draw_text(c, cols[4], y, s->finished ? text : "-", INK, 1);
            snprintf(text, sizeof text, "%.1f%%", s->avg_percent);
            draw_text(c, cols[5], y, s->finished ? text : "-", AQUA, 1);
        } else {
            const bench_saved_run *run = &b->runs[group->first_run+i];
            line(c, run_cols[0], y, run_cols[1]-run_cols[0]-8, run->started, INK, false);
            draw_text(c, run_cols[1], y, run->status, AQUA, 1);
            snprintf(text, sizeof text, "%d / %d", run->answered, run->total);
            draw_text(c, run_cols[2], y, text, MUTED, 1);
            snprintf(text, sizeof text, "%d", run->correct);
            draw_text(c, run_cols[3], y, text, INK, 1);
        }
    }
}

static void detail(sr_canvas *c, const yesno_bench *b, rect r)
{
    int x = r.x + r.w + 16, width = c->w - PAD - x;
    draw_fill(c, x, r.y-31, width, r.h+31, PANEL, 1);
    int y = r.y-20;
    x += 15;
    width -= 30;
    if (b->selected < 0) {
        draw_text(c, x, y, b->misses ? "No misses to inspect" : "Answer detail", MUTED, 1);
        return;
    }
    const bench_answer *a = &b->answers[b->selected];
    char header[180];
    snprintf(header, sizeof header, "Question %d   /   %s", a->number, a->band);
    line(c, x, y, width, header, AQUA, false);
    y = paragraph(c, x, y+28, width, a->question, 3, INK);
    int bottom = r.y + r.h - 12;
    if (*a->context && y+106 < bottom)
        y = paragraph(c, x, y+5, width, a->context, (bottom-y > 230) ? 4 : 2, MUTED);
    snprintf(header, sizeof header, "Expected  %s       Time  %.1f s", a->expected, a->seconds);
    if (y+49 < bottom) {
        draw_text(c, x, y+8, header, GOLD, 1);
        y += 34;
        char reply[820];
        snprintf(reply, sizeof reply, "Reply     %s", a->reply[0] ? a->reply : "(empty)");
        int lines = (bottom-y)/20;
        y = paragraph(c, x, y, width, reply, lines < 3 ? lines : 3, INK);
    }
    if (y+48 < bottom && a->rationale[0]) {
        draw_text(c, x, y+10, "Answer key", MUTED, 1);
        paragraph(c, x, y+32, width, a->rationale, (bottom-y-32)/20, INK);
    }
}

static void history(sr_canvas *c, const yesno_bench *b)
{
    rect r = yesno_history_rect(c->w, c->h);
    draw_fill(c, r.x, r.y-31, r.w, r.h+31, PANEL, 1);
    draw_text(c, r.x+12, r.y-21, b->misses ? "Miss #" : "Item", MUTED, 1);
    draw_text(c, r.x+73, r.y-21, "Result", MUTED, 1);
    draw_text(c, r.x+165, r.y-21, "Reply", MUTED, 1);
    draw_text(c, r.x+240, r.y-21, "Band", MUTED, 1);
    int rows = r.h / BENCH_ROW_H;
    int index = yesno_history_first(b, rows);
    for (int row=0; row<rows && index>=0; row++) {
        const bench_answer *a = &b->answers[index];
        int y = r.y + row*BENCH_ROW_H;
        if (index == b->selected) {
            draw_fill(c, r.x, y, r.w, BENCH_ROW_H-1, 0x30434f, 1);
            draw_fill(c, r.x, y, 3, BENCH_ROW_H-1, AQUA, 1);
        }
        char n[20];
        snprintf(n, sizeof n, "%d", a->number);
        draw_text(c, r.x+12, y+4, n, INK, 1);
        const char *result = a->verdict == BENCH_CORRECT ? "Correct" : a->verdict == BENCH_WRONG ? "Wrong" : "Invalid";
        draw_text(c, r.x+73, y+4, result, a->verdict == BENCH_CORRECT ? AQUA : RED, 1);
        line(c, r.x+165, y+4, 65, a->reply, INK, false);
        line(c, r.x+240, y+4, r.w-255, a->band, MUTED, false);
        index = yesno_history_row(b, index+1, 0);
    }
    detail(c, b, r);
}

void yesno_draw(sr_canvas *c, const yesno_bench *b)
{
    if (b->archive) b = b->archive;
    draw_fill(c, 0, 0, c->w, c->h, BG, 1);
    draw_text(c, PAD, 20, b->page == BENCH_REPORT ? "YES / NO BENCH    SAVED ANSWERS" : "YES / NO BENCH", GOLD, 1);
    if (b->page != BENCH_WATCH && b->page != BENCH_REPORT) {
        if (b->page == BENCH_RANKS || b->page == BENCH_RUNS) records(c, b);
        else picker(c, b);
        dock(c, b);
        return;
    }
    static const char *const states[] = {"Ready", "Running", "Complete", "Stopped", "Failed"};
    const char *state = b->replay ? "Loading saved answers" : states[b->status];
    int width = draw_text_width(state);
    draw_disc(c, c->w-PAD-width-13, 28, 4, 4, b->status == BENCH_FAILED ? RED : AQUA, 1);
    draw_text(c, c->w-PAD-width, 20, state, INK, 1);
    char text[1100], choice[512];
    const char *model = b->provider == 1 ? strrchr(b->model, '/') : NULL;
    model = model ? model+1 : b->model;
    snprintf(choice, sizeof choice, "%s%s%.480s", yesno_provider(b->provider), *model ? " / " : "", model);
    bool current = b->page == BENCH_REPORT || b->status == BENCH_RUNNING;
    line(c, PAD, 54, c->w/2 - PAD, current && *b->run_label ? b->run_label : choice, INK, false);
    snprintf(text, sizeof text, "%d / %d answered", b->count, b->total);
    draw_text(c, c->w-PAD-draw_text_width(text), 54, text, INK, 1);
    int available = c->w - 2*PAD;
    draw_fill(c, PAD, 81, available, 6, LINE, 1);
    if (b->total > 0 && b->count) draw_fill(c, PAD, 81, available*(double)b->count/b->total, 6, AQUA, 1);
    snprintf(text, sizeof text, "Correct %d       Wrong %d       Invalid %d", b->correct, b->wrong, b->invalid);
    draw_text(c, PAD, 103, text, INK, 1);
    if (b->count) {
        snprintf(text, sizeof text, "%.1f%% accuracy", 100.0*b->correct/b->count);
        draw_text(c, c->w-PAD-draw_text_width(text), 103, text, AQUA, 1);
    }
    if (b->notice[0]) line(c, PAD, 140, available, b->notice, b->status == BENCH_FAILED ? RED : MUTED, false);
    else if (b->status == BENCH_RUNNING && b->asking) {
        snprintf(text, sizeof text, "Asking %d   /   %.60s   /   %.960s", b->asking, b->band, b->question);
        line(c, PAD, 140, available, text, AQUA, false);
    } else if (b->status == BENCH_READY) {
        draw_text(c, PAD, 140, b->provider < 0 ? "Choose a model to start a run." :
                  "Start runs the OneBit development questions unattended.", MUTED, 1);
    }
    if (b->report[0]) {
        if (b->page == BENCH_REPORT || !strcmp(choice, b->run_label))
            snprintf(text, sizeof text, "Report   bench/yesno/%.125s", b->report);
        else if (b->status == BENCH_RUNNING)
            snprintf(text, sizeof text, "Next run   %.420s     Report   bench/yesno/%.125s", choice, b->report);
        else
            snprintf(text, sizeof text, "Report for %.420s   bench/yesno/%.125s", b->run_label, b->report);
        line(c, PAD, 167, available, text, MUTED, false);
    }
    history(c, b);
    dock(c, b);
}
