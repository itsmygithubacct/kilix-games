/* The Yes/No Bench owns its model choice, answer history and child processes. */
#ifndef PUZZLLM_YESNO_H
#define PUZZLLM_YESNO_H
#include "keys.h"
#include "layout.h"
#include "pty.h"
#include "soft_raster.h"

typedef enum { BENCH_WATCH, BENCH_PROVIDERS, BENCH_MODELS, BENCH_ENTRY,
               BENCH_RANKS, BENCH_RUNS, BENCH_REPORT, BENCH_MACHINES, BENCH_EFFORT } bench_page;
typedef enum { BENCH_READY, BENCH_RUNNING, BENCH_COMPLETE, BENCH_STOPPED, BENCH_FAILED } bench_status;
typedef enum { BENCH_CORRECT, BENCH_WRONG, BENCH_INVALID } bench_verdict;
typedef enum { BENCH_K12, BENCH_BOOLQ } bench_suite;
typedef enum { BENCH_IN_GAME, BENCH_DIRECT, BENCH_MENU_ONLY } bench_mode;
#define BENCH_K12_NAME "kilix-k12-computer use (yes/no)"
#define BENCH_BOOLQ_NAME "BoolQ (YES/NO)"
enum { BENCH_MODEL, BENCH_START, BENCH_FILTER, BENCH_LATEST, BENCH_RANKINGS, BENCH_BACK, BENCH_BUTTONS };

typedef struct bench_answer {
    int number;
    bench_verdict verdict;
    char band[64], question[1024], context[1024], rationale[768], reply[768], expected[8];
    double seconds;
} bench_answer;

typedef struct bench_stream {
    pty_proc process;
    char line[32768];
    size_t len;
} bench_stream;

typedef struct bench_standing {
    char provider[32], model[4096], effort[48];
    int rank, runs, finished, invalid, invalid_counted_runs, first_run, loaded, first_machine, machine_count;
    double avg_correct, avg_percent;
} bench_standing;

typedef struct bench_runner {
    char label[128], cpu[256], gpu[768], ram[64], backend[48];
} bench_runner;

typedef struct bench_machine_result {
    bench_runner runner;
    int runs, finished, invalid, invalid_counted_runs, wall_timed_runs, timed_answers;
    double avg_percent, mean_wall_seconds, mean_answer_seconds;
} bench_machine_result;

typedef struct bench_saved_run {
    char file[256], started[128], status[32], usage[512];
    bench_runner runner;
    double wall_seconds, mean_answer_seconds;
    int correct, answered, total, invalid;
} bench_saved_run;

typedef struct yesno_bench {
    bool open, replay, follow, misses, invalid_only, redraw;
    bench_mode mode;
    bench_page page;
    bench_suite suite;
    bench_status status;
    int provider, run_provider, picking, cursor;
    char model[4096], entry[4096], effort[48], run_effort[48], run_label[512], report[128];
    char notice[512], catalog_notice[320], question[1024], band[64];
    int total, asking, correct, wrong, invalid, selected;
    bench_answer *answers;
    int count, capacity;
    char **models;
    int model_count, model_capacity;
    bench_standing *standings;
    bench_saved_run *runs;
    bench_machine_result *machines;
    int machine_count, machine_capacity, history_cursor;
    int standing_count, standing_capacity, run_count, run_capacity, standing_index;
    bool refresh_rankings;
    struct yesno_bench *archive;
    bench_stream worker, catalog;
} yesno_bench;

struct game;
void yesno_init(yesno_bench *b);
void yesno_close(yesno_bench *b);
void yesno_open(struct game *g, bench_suite suite);
const char *yesno_suite_key(bench_suite suite);
void yesno_poll(struct game *g);
void yesno_key(struct game *g, const key_event *ev);
void yesno_click(struct game *g, int x, int y);
void yesno_scroll(yesno_bench *b, int rows);
void yesno_paste(yesno_bench *b, const char *text, size_t len);
void yesno_draw(sr_canvas *c, const yesno_bench *b);

const char *yesno_provider(int index);
bool yesno_has_effort(int provider);
bool yesno_gguf(int provider);
const char *yesno_option(const yesno_bench *b, int row);
int yesno_options(const yesno_bench *b);
int yesno_history_row(const yesno_bench *b, int first, int offset);
int yesno_history_first(const yesno_bench *b, int rows);
int yesno_picker_first(const yesno_bench *b, int rows);
rect yesno_button_rect(int width, int height, int button);
rect yesno_history_rect(int width, int height);
rect yesno_picker_rect(int width, int height);
rect yesno_runs_rect(int width, int height);
rect yesno_invalid_rect(const yesno_bench *b);
rect yesno_effort_rect(int width);
enum { BENCH_ROW_H = 25 };
#endif
