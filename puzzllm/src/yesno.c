#define _POSIX_C_SOURCE 200809L
#include "yesno.h"
#include "game.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const providers[] = {"Ollama", "llama.cpp", "Codex", "Grok", "Claude", "Kilix models"};
static const char *const commands[] = {"ollama", "llama.cpp", "codex", "grok", "claude", "kilix"};
enum { PROVIDERS = sizeof providers / sizeof providers[0] };

bool yesno_has_effort(int provider) { return provider >= 2 && provider <= 4; }

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
    b->provider = b->run_provider = b->selected = -1;
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
    if (b->archive) {
        yesno_close(b->archive);
        free(b->archive);
        b->archive = NULL;
    }
    stop_stream(&b->worker);
    clear_models(b);
    free(b->models);
    free(b->answers);
    free(b->standings);
    free(b->runs);
    free(b->machines);
}

static bool launch(game *g, bench_stream *stream, char *mode, char *arg, char *provider, char *model)
{
    char helper[4096];
    snprintf(helper, sizeof helper, "%s/libexec/puzzllm-yesno", g->root);
    char *argv[] = {helper, mode, arg, provider, model, !strcmp(mode, "run") ? g->bench.effort : NULL, NULL};
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

static double optional_number(const json_doc *doc, int root, const char *key)
{
    int i = json_get(doc, root, key);
    return i >= 0 && doc->nodes[i].type == JSON_NUMBER ? doc->nodes[i].num : -1;
}

static bool matches(const yesno_bench *b, int i)
{
    if (b->invalid_only) return b->answers[i].verdict == BENCH_INVALID;
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
    else {
        a->verdict = BENCH_INVALID;
        if (++b->invalid == 1 && b->page == BENCH_RANKS) b->refresh_rankings = true;
    }
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
        b->run_provider = -1;
        copy_field(b->run_effort, sizeof b->run_effort, field(doc, root, "effort"));
        copy_field(b->report, sizeof b->report, field(doc, root, "file"));
        for (int i = 0; i < PROVIDERS; i++) {
            if (!strcmp(commands[i], field(doc, root, "provider"))) {
                const char *model = field(doc, root, "model");
                b->run_provider = i;
                label(b->run_label, sizeof b->run_label, i, model);
                if (b->provider < 0) {
                    b->provider = i;
                    copy_field(b->model, sizeof b->model, model);
                    if (strcmp(b->run_effort, "default")) copy_field(b->effort, sizeof b->effort, b->run_effort);
                }
                break;
            }
        }
    } else if (!strcmp(event, "selection") || !strcmp(event, "identity")) {
        label(b->run_label, sizeof b->run_label, b->run_provider, field(doc, root, "model"));
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
        b->refresh_rankings = true;
    } else if (!strcmp(event, "storage_warning")) {
        copy_field(b->notice, sizeof b->notice, field(doc, root, "text"));
    } else if (!strcmp(event, "status") || !strcmp(event, "error") || !strcmp(event, "stopped")) {
        copy_field(b->notice, sizeof b->notice, field(doc, root, "text"));
        if (!strcmp(event, "error")) b->status = BENCH_FAILED;
        if (!strcmp(event, "stopped")) b->status = BENCH_STOPPED;
        if (b->status != BENCH_RUNNING) b->refresh_rankings = true;
    }
}

static void read_runner(bench_runner *r, const json_doc *doc, int root)
{
    copy_field(r->label, sizeof r->label, field(doc, root, "machine_label"));
    copy_field(r->cpu, sizeof r->cpu, field(doc, root, "cpu_label"));
    copy_field(r->gpu, sizeof r->gpu, field(doc, root, "gpu_label"));
    copy_field(r->ram, sizeof r->ram, field(doc, root, "ram_label"));
    copy_field(r->backend, sizeof r->backend, field(doc, root, "backend_scope"));
}

static void catalog_event(yesno_bench *b, const json_doc *doc, int root)
{
    if (b->catalog_notice[0]) return;
    const char *event = field(doc, root, "event");
    const char *model = field(doc, root, "model");
    if (!strcmp(event, "standing")) {
        if (b->standing_count == b->standing_capacity) {
            int capacity = b->standing_capacity ? 2*b->standing_capacity : 16;
            bench_standing *list = realloc(b->standings, (size_t)capacity * sizeof *list);
            if (!list) goto full;
            b->standings = list;
            b->standing_capacity = capacity;
        }
        bench_standing *s = &b->standings[b->standing_count++];
        copy_field(s->provider, sizeof s->provider, field(doc, root, "provider"));
        copy_field(s->model, sizeof s->model, model);
        const char *effort = field(doc, root, "effort");
        bool hosted = !strcmp(s->provider, "codex") || !strcmp(s->provider, "grok") || !strcmp(s->provider, "claude");
        copy_field(s->effort, sizeof s->effort, *effort ? effort : hosted ? "Unrecorded" : "-");
        s->rank = (int)number(doc, root, "rank");
        s->runs = (int)number(doc, root, "runs");
        s->finished = (int)number(doc, root, "finished");
        s->invalid = (int)optional_number(doc, root, "invalid_answers");
        s->invalid_counted_runs = (int)number(doc, root, "invalid_counted_runs");
        s->avg_correct = number(doc, root, "avg_correct");
        s->avg_percent = number(doc, root, "avg_percent");
        s->first_run = b->run_count;
        s->loaded = 0;
        s->first_machine = b->machine_count;
        s->machine_count = 0;
        return;
    }
    if (!strcmp(event, "machine_result") && b->standing_count) {
        if (b->machine_count == b->machine_capacity) {
            int capacity = b->machine_capacity ? 2*b->machine_capacity : 16;
            bench_machine_result *list = realloc(b->machines, (size_t)capacity * sizeof *list);
            if (!list) goto full;
            b->machines = list;
            b->machine_capacity = capacity;
        }
        bench_machine_result *m = &b->machines[b->machine_count++];
        read_runner(&m->runner, doc, root);
        m->runs = (int)number(doc, root, "runs");
        m->finished = (int)number(doc, root, "finished");
        m->invalid = (int)optional_number(doc, root, "invalid_answers");
        m->invalid_counted_runs = (int)number(doc, root, "invalid_counted_runs");
        m->wall_timed_runs = (int)number(doc, root, "wall_timed_runs");
        m->timed_answers = (int)number(doc, root, "timed_answers");
        m->avg_percent = optional_number(doc, root, "avg_percent");
        m->mean_wall_seconds = optional_number(doc, root, "mean_wall_seconds");
        m->mean_answer_seconds = optional_number(doc, root, "mean_answer_seconds");
        b->standings[b->standing_count-1].machine_count++;
        return;
    }
    if (!strcmp(event, "saved_run") && b->standing_count) {
        if (b->run_count == b->run_capacity) {
            int capacity = b->run_capacity ? 2*b->run_capacity : 32;
            bench_saved_run *list = realloc(b->runs, (size_t)capacity * sizeof *list);
            if (!list) goto full;
            b->runs = list;
            b->run_capacity = capacity;
        }
        bench_saved_run *r = &b->runs[b->run_count++];
        copy_field(r->file, sizeof r->file, field(doc, root, "file"));
        copy_field(r->started, sizeof r->started, field(doc, root, "started"));
        copy_field(r->status, sizeof r->status, field(doc, root, "status"));
        read_runner(&r->runner, doc, root);
        copy_field(r->usage, sizeof r->usage, field(doc, root, "usage_label"));
        r->wall_seconds = optional_number(doc, root, "wall_seconds");
        r->mean_answer_seconds = optional_number(doc, root, "mean_answer_seconds");
        r->correct = (int)number(doc, root, "correct");
        r->invalid = (int)optional_number(doc, root, "invalid");
        r->answered = (int)number(doc, root, "answered");
        r->total = (int)number(doc, root, "total");
        b->standings[b->standing_count-1].loaded++;
        return;
    }
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
    return;
full:
    copy_field(b->catalog_notice, sizeof b->catalog_notice, "Out of memory reading the benchmark history.");
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

static void rankings(game *g)
{
    yesno_bench *b = &g->bench;
    clear_models(b);
    b->standing_count = b->run_count = b->machine_count = b->cursor = 0;
    b->refresh_rankings = false;
    b->page = BENCH_RANKS;
    if (g->spawn && !launch(g, &b->catalog, "records", g->state, NULL, NULL))
        copy_field(b->catalog_notice, sizeof b->catalog_notice, "Could not read the benchmark history.");
}

void yesno_poll(game *g)
{
    yesno_bench *b = &g->bench;
    poll_stream(b, &b->worker, false);
    poll_stream(b, &b->catalog, true);
    if (b->archive) {
        poll_stream(b->archive, &b->archive->worker, false);
        if (b->archive->redraw) b->redraw = true;
        b->archive->redraw = false;
    }
    if (b->page == BENCH_RANKS && b->refresh_rankings && !b->catalog.process.pid)
        rankings(g);
}

static void choose(yesno_bench *b, const char *model)
{
    if (b->provider != b->picking || strcmp(b->model, model)) b->effort[0] = 0;
    b->provider = b->picking;
    copy_field(b->model, sizeof b->model, model);
    clear_models(b);
    b->page = BENCH_WATCH;
}

int yesno_options(const yesno_bench *b)
{
    if (b->page == BENCH_PROVIDERS) return PROVIDERS;
    if (b->page == BENCH_EFFORT) return b->model_count + 1;
    if (b->page == BENCH_RANKS) return b->standing_count;
    if (b->page == BENCH_RUNS) return b->standings[b->standing_index].loaded;
    if (b->page == BENCH_MACHINES) return b->standings[b->standing_index].machine_count;
    if (b->picking == 5) return b->model_count;
    return b->model_count + (b->picking < 2 ? 1 : 2);
}

const char *yesno_option(const yesno_bench *b, int row)
{
    if (b->page == BENCH_PROVIDERS) return yesno_provider(row);
    if (b->page == BENCH_EFFORT) return row ? b->models[row-1] : "Use configured default";
    if (b->picking == 5) return b->models[row];
    if (b->picking >= 2) {
        if (row < 2) return row ? "Enter model ID..." : "Use configured default";
        return b->models[row-2];
    }
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
        launch(g, &b->catalog, "models", (char *)commands[b->picking], NULL, NULL);
    } else if (b->page == BENCH_MODELS) {
        if (b->picking == 5) {
            if (b->cursor < b->model_count) choose(b, b->models[b->cursor]);
        } else if (b->picking >= 2 && !b->cursor) choose(b, "");
        else if (b->cursor >= (b->picking < 2 ? 1 : 2)) choose(b, yesno_option(b, b->cursor));
        else {
            b->entry[0] = 0;
            b->page = BENCH_ENTRY;
        }
    } else if (b->page == BENCH_EFFORT && b->cursor < yesno_options(b)) {
        copy_field(b->effort, sizeof b->effort, b->cursor ? yesno_option(b, b->cursor) : "");
        clear_models(b);
        b->page = BENCH_WATCH;
    } else if (b->page == BENCH_ENTRY && b->entry[0]) {
        choose(b, b->entry);
    } else if (b->page == BENCH_RANKS && b->cursor < b->standing_count) {
        b->standing_index = b->cursor;
        b->cursor = 0;
        b->page = BENCH_RUNS;
    } else if (b->page == BENCH_RUNS && b->cursor < yesno_options(b)) {
        bench_saved_run *r = &b->runs[b->standings[b->standing_index].first_run+b->cursor];
        yesno_bench *view = calloc(1, sizeof *view);
        if (!view) return;
        yesno_init(view);
        b->archive = view;
        b->page = view->page = BENCH_REPORT;
        view->replay = launch(g, &view->worker, "report", g->state, r->file, NULL);
        if (!view->replay)
            copy_field(view->notice, sizeof view->notice, "Could not open the saved answers.");
    }
}

static void back(yesno_bench *b)
{
    switch (b->page) {
    case BENCH_EFFORT: clear_models(b); b->page = BENCH_WATCH; break;
    case BENCH_ENTRY: b->page = BENCH_MODELS; break;
    case BENCH_MODELS: clear_models(b); b->cursor = b->picking; b->page = BENCH_PROVIDERS; break;
    case BENCH_PROVIDERS: b->page = BENCH_WATCH; break;
    case BENCH_REPORT:
        yesno_close(b->archive);
        free(b->archive);
        b->archive = NULL;
        b->page = BENCH_RUNS;
        break;
    case BENCH_RUNS: b->cursor = b->standing_index; b->page = BENCH_RANKS; break;
    case BENCH_MACHINES: b->cursor = b->history_cursor; b->page = BENCH_RUNS; break;
    case BENCH_RANKS: clear_models(b); b->page = BENCH_WATCH; break;
    case BENCH_WATCH: b->open = false; break;
    }
}

static void button(game *g, int id)
{
    yesno_bench *b = &g->bench;
    switch (id) {
    case BENCH_RANKINGS:
        if (b->page == BENCH_MACHINES) back(b);
        else if (b->page == BENCH_RUNS) {
            b->history_cursor = b->cursor;
            b->cursor = 0;
            b->page = BENCH_MACHINES;
        } else rankings(g);
        break;
    case BENCH_MODEL:
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
    case BENCH_FILTER:
        if (b->archive) b = b->archive;
        b->invalid_only = false;
        b->misses = !b->misses;
        latest_answer(b);
        break;
    case BENCH_LATEST: latest_answer(b->archive ? b->archive : b); break;
    case BENCH_BACK:
        if (b->standalone && b->page == BENCH_WATCH) g->quit = true;
        else back(b);
        break;
    }
}

static void toggle_invalid(yesno_bench *b)
{
    b->invalid_only = !b->invalid_only;
    b->misses = false;
    latest_answer(b);
}

static void choose_effort(game *g)
{
    yesno_bench *b = &g->bench;
    if (b->page != BENCH_WATCH || !yesno_has_effort(b->provider)) return;
    clear_models(b);
    b->cursor = 0;
    b->page = BENCH_EFFORT;
    launch(g, &b->catalog, "efforts", (char *)commands[b->provider], b->model, NULL);
}

void yesno_scroll(yesno_bench *b, int rows)
{
    b->redraw = true;
    if (b->archive) b = b->archive;
    if (b->page == BENCH_PROVIDERS || b->page == BENCH_MODELS || b->page == BENCH_EFFORT ||
        b->page == BENCH_RANKS || b->page == BENCH_RUNS || b->page == BENCH_MACHINES) {
        int n = yesno_options(b);
        if (!n) return;
        b->cursor += rows;
        if (b->cursor < 0) b->cursor = 0;
        if (b->cursor >= n) b->cursor = n - 1;
    } else if (b->page == BENCH_WATCH || b->page == BENCH_REPORT) {
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
    case KEY_HOME: yesno_scroll(b, -yesno_options(b)-(b->archive ? b->archive : b)->count); return;
    case KEY_END: yesno_scroll(b, yesno_options(b)+(b->archive ? b->archive : b)->count); return;
    }
    if (ev->action != 1) return;
    if (b->page != BENCH_WATCH && b->page != BENCH_REPORT) {
        if ((b->page == BENCH_RUNS || b->page == BENCH_MACHINES) && (ev->key == 'c' || ev->key == 'C')) {
            button(g, BENCH_RANKINGS);
            return;
        }
        if (ev->key == KEY_ENTER) accept(g);
        if (b->page == BENCH_RANKS && (ev->key == 'u' || ev->key == 'U')) rankings(g);
        return;
    }
    switch (ev->key) {
    case 'm': case 'M': if (!b->archive) button(g, BENCH_MODEL); break;
    case 'r': case 'R': if (!b->archive) button(g, BENCH_START); break;
    case 'b': case 'B': if (!b->archive) button(g, BENCH_RANKINGS); break;
    case 'f': case 'F': button(g, BENCH_FILTER); break;
    case 'i': case 'I': toggle_invalid(b->archive ? b->archive : b); break;
    case 'e': case 'E': choose_effort(g); break;
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
            if (b->page == BENCH_WATCH || i == BENCH_BACK ||
                (b->page == BENCH_REPORT && (i == BENCH_FILTER || i == BENCH_LATEST)) ||
                ((b->page == BENCH_RANKS || b->page == BENCH_RUNS || b->page == BENCH_MACHINES) &&
                 i == BENCH_RANKINGS)) button(g, i);
            else if (i == BENCH_START && b->page != BENCH_REPORT) accept(g);
            return;
        }
    }
    bool watch = b->page == BENCH_WATCH || b->page == BENCH_REPORT;
    if (b->archive) b = b->archive;
    if (watch && inside(yesno_invalid_rect(b), x, y)) { toggle_invalid(b); return; }
    if (watch && b->page == BENCH_WATCH && yesno_has_effort(b->provider) && inside(yesno_effort_rect(L.w), x, y)) {
        choose_effort(g);
        return;
    }
    rect area = watch ? yesno_history_rect(L.w, L.h) : yesno_picker_rect(L.w, L.h);
    if (b->page == BENCH_RUNS || b->page == BENCH_MACHINES) area = yesno_runs_rect(L.w, L.h);
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
