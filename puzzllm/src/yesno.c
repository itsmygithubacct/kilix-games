#define _POSIX_C_SOURCE 200809L
#include "yesno.h"
#include "game.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const providers[] = {"Ollama", "llama.cpp", "Codex", "Grok", "Claude"};
static const char *const commands[] = {"ollama", "llama.cpp", "codex", "grok", "claude"};
enum { PROVIDERS = sizeof providers / sizeof providers[0] };

const char *yesno_provider(int index)
{
    return index >= 0 && index < PROVIDERS ? providers[index] : "Choose a model";
}

static const char *model_label(int provider, const char *model)
{
    const char *leaf = provider == 1 ? strrchr(model, '/') : NULL;
    return leaf ? leaf + 1 : model;
}

static void label(char *out, size_t size, int provider, const char *model)
{
    snprintf(out, size, "%s%s%s", yesno_provider(provider), model[0] ? " / " : "",
             model_label(provider, model));
}

void yesno_init(yesno_bench *b)
{
    b->provider = b->selected = -1;
    b->follow = true;
    pty_init(&b->worker.process);
    pty_init(&b->catalog.process);
}

static void stop_stream(bench_stream *stream)
{
    pty_stop(&stream->process);
    pty_init(&stream->process);
    stream->len = 0;
}

static void clear_models(yesno_bench *b)
{
    stop_stream(&b->catalog);
    for (int i = 0; i < b->model_count; i++) free(b->models[i]);
    b->model_count = 0;
    b->catalog_notice[0] = 0;
}

void yesno_close(yesno_bench *b)
{
    stop_stream(&b->worker);
    clear_models(b);
    free(b->models);
    free(b->answers);
}

static bool launch(game *g, bench_stream *stream, char *mode, char *arg, char *provider, char *model)
{
    char helper[4096];
    snprintf(helper, sizeof helper, "%s/libexec/puzzllm-yesno", g->root);
    char *argv[] = {helper, mode, arg, provider, model, NULL};
    return g->spawn && pty_spawn(&stream->process, argv, NULL, 120, 24);
}

void yesno_open(game *g)
{
    yesno_bench *b = &g->bench;
    b->open = b->redraw = true;
    b->page = BENCH_WATCH;
    if (!b->worker.process.pid) {
        b->replay = launch(g, &b->worker, "latest", g->state, NULL, NULL);
    }
}

static void copy_field(char *out, size_t size, const char *value)
{
    size_t n = strlen(value);
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)value[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out, value, n);
    out[n] = 0;
}

static const char *field(const json_doc *doc, int root, const char *key)
{
    const char *s = json_str(doc, json_get(doc, root, key));
    return s ? s : "";
}

static double number(const json_doc *doc, int root, const char *key)
{
    int i = json_get(doc, root, key);
    return i >= 0 && doc->nodes[i].type == JSON_NUMBER ? doc->nodes[i].num : 0;
}

static bool matches(const yesno_bench *b, int i)
{
    return !b->misses || b->answers[i].verdict != BENCH_CORRECT;
}

static void latest_answer(yesno_bench *b)
{
    b->follow = true;
    b->selected = b->count - 1;
    while (b->selected >= 0 && !matches(b, b->selected)) b->selected--;
}

static void answer_event(yesno_bench *b, const json_doc *doc, int root)
{
    if (b->count == b->capacity) {
        int capacity = b->capacity ? b->capacity * 2 : 32;
        bench_answer *rows = realloc(b->answers, (size_t)capacity * sizeof *rows);
        if (!rows) {
            copy_field(b->notice, sizeof b->notice, "Out of memory for answer history. The report is on disk.");
            return;
        }
        b->answers = rows;
        b->capacity = capacity;
    }
    bench_answer *a = &b->answers[b->count++];
    a->number = (int)number(doc, root, "n");
    a->seconds = number(doc, root, "seconds");
    copy_field(a->question, sizeof a->question, field(doc, root, "question"));
    copy_field(a->band, sizeof a->band, field(doc, root, "band"));
    copy_field(a->context, sizeof a->context, field(doc, root, "context"));
    copy_field(a->rationale, sizeof a->rationale, field(doc, root, "rationale"));
    copy_field(a->reply, sizeof a->reply, field(doc, root, "reply"));
    copy_field(a->expected, sizeof a->expected, field(doc, root, "expected"));
    const char *result = field(doc, root, "verdict");
    if (!strcmp(result, "correct")) { a->verdict = BENCH_CORRECT; b->correct++; }
    else if (!strcmp(result, "wrong")) { a->verdict = BENCH_WRONG; b->wrong++; }
    else { a->verdict = BENCH_INVALID; b->invalid++; }
    if (b->follow && matches(b, b->count - 1)) b->selected = b->count - 1;
}

static void worker_event(yesno_bench *b, const json_doc *doc, int root)
{
    const char *event = field(doc, root, "event");
    if (!strcmp(event, "attached")) {
        b->replay = false;
    } else if (!strcmp(event, "begin")) {
        b->count = b->correct = b->wrong = b->invalid = b->asking = 0;
        b->selected = -1;
        b->follow = true;
        b->status = BENCH_RUNNING;
        b->total = (int)number(doc, root, "total");
        b->question[0] = b->band[0] = b->notice[0] = 0;
        copy_field(b->report, sizeof b->report, field(doc, root, "file"));
        for (int i = 0; i < PROVIDERS; i++) {
            if (!strcmp(commands[i], field(doc, root, "provider"))) {
                const char *model = field(doc, root, "model");
                label(b->run_label, sizeof b->run_label, i, model);
                if (b->provider < 0) {
                    b->provider = i;
                    copy_field(b->model, sizeof b->model, model);
                }
                break;
            }
        }
    } else if (!strcmp(event, "size")) {
        b->total = (int)number(doc, root, "total");
    } else if (!strcmp(event, "question")) {
        b->asking = (int)number(doc, root, "n");
        copy_field(b->question, sizeof b->question, field(doc, root, "question"));
        copy_field(b->band, sizeof b->band, field(doc, root, "band"));
        b->notice[0] = 0;
    } else if (!strcmp(event, "answer")) {
        answer_event(b, doc, root);
    } else if (!strcmp(event, "end")) {
        b->status = BENCH_COMPLETE;
    } else if (!strcmp(event, "status") || !strcmp(event, "error") || !strcmp(event, "stopped")) {
        copy_field(b->notice, sizeof b->notice, field(doc, root, "text"));
        if (!strcmp(event, "error")) b->status = BENCH_FAILED;
        if (!strcmp(event, "stopped")) b->status = BENCH_STOPPED;
    }
}

static void catalog_event(yesno_bench *b, const json_doc *doc, int root)
{
    const char *model = field(doc, root, "model");
    if (!*model) {
        copy_field(b->catalog_notice, sizeof b->catalog_notice, field(doc, root, "text"));
        return;
    }
    if (b->model_count == b->model_capacity) {
        int size = b->model_capacity ? b->model_capacity * 2 : 16;
        char **list = realloc(b->models, sizeof *list * (size_t)size);
        if (!list) return;
        b->models = list;
        b->model_capacity = size;
    }
    char *s = strdup(model);
    if (s) b->models[b->model_count++] = s;
}

static void poll_stream(yesno_bench *b, bench_stream *stream, bool catalog)
{
    if (stream->process.pid <= 0) return;
    char buf[4096];
    for (int reads = 0; reads < 32; reads++) {
        long n = pty_read(&stream->process, buf, sizeof buf);
        if (n != 0) b->redraw = true;
        if (n <= 0) {
            if (n < 0 && pty_poll_exit(&stream->process)) {
                if (!catalog && b->status == BENCH_RUNNING) {
                    b->status = BENCH_FAILED;
                    copy_field(b->notice, sizeof b->notice, "Worker exited before the run finished.");
                }
                if (!catalog) b->replay = false;
                stop_stream(stream);
            }
            return;
        }
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                json_doc doc = {0};
                int root = json_parse(&doc, stream->line, stream->len);
                if (root >= 0) {
                    if (catalog) catalog_event(b, &doc, root);
                    else worker_event(b, &doc, root);
                }
                json_free(&doc);
                stream->len = 0;
            } else if (buf[i] != '\r' && stream->len < sizeof stream->line) {
                stream->line[stream->len++] = buf[i];
            }
        }
    }
}

void yesno_poll(yesno_bench *b)
{
    poll_stream(b, &b->worker, false);
    poll_stream(b, &b->catalog, true);
}

static void choose(yesno_bench *b, const char *model)
{
    b->provider = b->picking;
    copy_field(b->model, sizeof b->model, model);
    clear_models(b);
    b->page = BENCH_WATCH;
}

int yesno_options(const yesno_bench *b)
{
    if (b->page == BENCH_PROVIDERS) return PROVIDERS;
    return b->picking < 2 ? 1 + b->model_count : 2;
}

const char *yesno_option(const yesno_bench *b, int row)
{
    if (b->page == BENCH_PROVIDERS) return yesno_provider(row);
    if (b->picking >= 2) return row ? "Enter model ID..." : "Use configured default";
    if (!row) return b->picking == 0 ? "Enter an Ollama tag..." : "Enter a GGUF file path...";
    return b->models[row-1];
}

static void accept(game *g)
{
    yesno_bench *b = &g->bench;
    if (b->page == BENCH_PROVIDERS) {
        b->picking = b->cursor;
        b->cursor = 0;
        b->page = BENCH_MODELS;
        clear_models(b);
        if (b->picking < 2) launch(g, &b->catalog, "models", (char *)commands[b->picking], NULL, NULL);
    } else if (b->page == BENCH_MODELS) {
        if (b->picking >= 2 && !b->cursor) choose(b, "");
        else if (b->picking < 2 && b->cursor) choose(b, b->models[b->cursor-1]);
        else {
            b->entry[0] = 0;
            b->page = BENCH_ENTRY;
        }
    } else if (b->page == BENCH_ENTRY && b->entry[0]) {
        choose(b, b->entry);
    }
}

static void back(yesno_bench *b)
{
    switch (b->page) {
    case BENCH_ENTRY: b->page = BENCH_MODELS; break;
    case BENCH_MODELS: clear_models(b); b->cursor = b->picking; b->page = BENCH_PROVIDERS; break;
    case BENCH_PROVIDERS: b->page = BENCH_WATCH; break;
    case BENCH_WATCH: b->open = false; break;
    }
}

static void button(game *g, int id)
{
    yesno_bench *b = &g->bench;
    switch (id) {
    case BENCH_MODEL:
        if (b->worker.process.pid > 0) break;
        b->page = BENCH_PROVIDERS;
        b->cursor = b->provider >= 0 ? b->provider : 0;
        break;
    case BENCH_START:
        if (b->replay) break;
        if (b->worker.process.pid > 0) {
            if (b->status != BENCH_RUNNING) break;
            if (pty_write(&b->worker.process, "stop\n", 5))
                copy_field(b->notice, sizeof b->notice, "Stopping benchmark...");
        } else if (b->provider < 0) {
            button(g, BENCH_MODEL);
        } else if (launch(g, &b->worker, "run", g->state, (char *)commands[b->provider], b->model)) {
            b->status = BENCH_RUNNING;
            copy_field(b->notice, sizeof b->notice, "Starting benchmark...");
        } else {
            b->status = BENCH_FAILED;
            copy_field(b->notice, sizeof b->notice, "Could not launch the benchmark worker.");
        }
        break;
    case BENCH_FILTER: b->misses = !b->misses; latest_answer(b); break;
    case BENCH_LATEST: latest_answer(b); break;
    case BENCH_BACK:
        if (b->standalone && b->page == BENCH_WATCH) g->quit = true;
        else back(b);
        break;
    }
}

void yesno_scroll(yesno_bench *b, int rows)
{
    b->redraw = true;
    if (b->page == BENCH_PROVIDERS || b->page == BENCH_MODELS) {
        int n = yesno_options(b);
        b->cursor += rows;
        if (b->cursor < 0) b->cursor = 0;
        if (b->cursor >= n) b->cursor = n - 1;
    } else if (b->page == BENCH_WATCH) {
        int step = rows < 0 ? -1 : 1;
        for (int i = b->selected + step; rows && i >= 0 && i < b->count; i += step) {
            if (matches(b, i)) { b->selected = i; b->follow = false; rows -= step; }
        }
    }
}

void yesno_paste(yesno_bench *b, const char *text, size_t len)
{
    if (b->page != BENCH_ENTRY) return;
    size_t n = strlen(b->entry);
    if (len >= sizeof b->entry - n) return;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)text[i] < 32 || text[i] == 127) return;
    b->redraw = true;
    memcpy(b->entry + n, text, len);
    b->entry[n + len] = 0;
}

void yesno_key(game *g, const key_event *ev)
{
    yesno_bench *b = &g->bench;
    if (ev->action == 3) return;
    b->redraw = true;
    if (ev->key == KEY_ESCAPE) { if (ev->action == 1) button(g, BENCH_BACK); return; }
    if (b->page == BENCH_ENTRY) {
        size_t n = strlen(b->entry);
        if (ev->key == KEY_ENTER) { if (ev->action == 1) accept(g); }
        else if (ev->key == KEY_BACKSPACE && n) {
            do { n--; } while (n && ((unsigned char)b->entry[n] & 0xc0) == 0x80);
            b->entry[n] = 0;
        } else if (!(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
            char bytes[4];
            if (ev->text_len) {
                for (int i = 0; i < ev->text_len; i++) yesno_paste(b, bytes, keys_utf8(ev->text[i], bytes));
            } else if (ev->key >= 32 && ev->key < 127) yesno_paste(b, bytes, keys_utf8(ev->key, bytes));
        }
        return;
    }
    switch (ev->key) {
    case KEY_UP: yesno_scroll(b, -1); return;
    case KEY_DOWN: yesno_scroll(b, 1); return;
    case KEY_PAGE_UP: yesno_scroll(b, -10); return;
    case KEY_PAGE_DOWN: yesno_scroll(b, 10); return;
    case KEY_HOME: yesno_scroll(b, -yesno_options(b)-b->count); return;
    case KEY_END: yesno_scroll(b, yesno_options(b)+b->count); return;
    }
    if (ev->action != 1) return;
    if (b->page != BENCH_WATCH) { if (ev->key == KEY_ENTER) accept(g); return; }
    switch (ev->key) {
    case 'm': case 'M': button(g, BENCH_MODEL); break;
    case 'r': case 'R': button(g, BENCH_START); break;
    case 'f': case 'F': button(g, BENCH_FILTER); break;
    case 'l': case 'L': button(g, BENCH_LATEST); break;
    }
}

int yesno_history_first(const yesno_bench *b, int rows)
{
    int at = b->selected;
    if (at < 0) return -1;
    int first = at;
    /* Live answers build down the table. Browsing keeps the selected row in view. */
    int before = b->follow ? rows - 1 : rows / 2;
    for (int i = at - 1; i >= 0 && before > 0; i--)
        if (matches(b, i)) { first = i; before--; }
    return first;
}

int yesno_history_row(const yesno_bench *b, int first, int offset)
{
    for (int i = first; i >= 0 && i < b->count; i++)
        if (matches(b, i) && offset-- == 0) return i;
    return -1;
}

int yesno_picker_first(const yesno_bench *b, int rows)
{
    int top = b->cursor - rows / 2, limit = yesno_options(b) - rows;
    if (top > limit) top = limit;
    return top < 0 ? 0 : top;
}

static bool inside(rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void yesno_click(game *g, int x, int y)
{
    yesno_bench *b = &g->bench;
    b->redraw = true;
    for (int i = 0; i < BENCH_BUTTONS; i++) {
        if (inside(yesno_button_rect(L.w, L.h, i), x, y)) {
            if (b->page == BENCH_WATCH || i == BENCH_BACK) button(g, i);
            else if (b->page == BENCH_ENTRY && i == BENCH_START) accept(g);
            return;
        }
    }
    bool watch = b->page == BENCH_WATCH;
    rect area = watch ? yesno_history_rect(L.w, L.h) : yesno_picker_rect(L.w, L.h);
    if (!inside(area, x, y) || b->page == BENCH_ENTRY) return;
    int row = (y - area.y) / BENCH_ROW_H, rows = area.h / BENCH_ROW_H;
    if (row >= rows) return;
    if (watch) {
        int index = yesno_history_row(b, yesno_history_first(b, rows), row);
        if (index >= 0) { b->selected = index; b->follow = false; }
    } else {
        int index = yesno_picker_first(b, rows) + row;
        if (index < yesno_options(b)) { b->cursor = index; accept(g); }
    }
}
