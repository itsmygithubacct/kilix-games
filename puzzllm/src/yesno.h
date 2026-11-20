/* The Yes/No Bench owns its model choice, answer history and child processes. */
#ifndef PUZZLLM_YESNO_H
#define PUZZLLM_YESNO_H
#include "keys.h"
#include "layout.h"
#include "pty.h"
#include "soft_raster.h"

typedef enum { BENCH_WATCH, BENCH_PROVIDERS, BENCH_MODELS, BENCH_ENTRY } bench_page;
typedef enum { BENCH_READY, BENCH_RUNNING, BENCH_COMPLETE, BENCH_STOPPED, BENCH_FAILED } bench_status;
typedef enum { BENCH_CORRECT, BENCH_WRONG, BENCH_INVALID } bench_verdict;
enum { BENCH_MODEL, BENCH_START, BENCH_FILTER, BENCH_LATEST, BENCH_BACK, BENCH_BUTTONS };

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

typedef struct yesno_bench {
    bool open, replay, follow, misses, standalone;
    bench_page page;
    bench_status status;
    int provider, picking, cursor;
    char model[4096], entry[4096], run_label[512], report[128];
    char notice[512], catalog_notice[320], question[1024], band[64];
    int total, asking, correct, wrong, invalid, selected;
    bench_answer *answers;
    int count, capacity;
    char **models;
    int model_count, model_capacity;
    bench_stream worker, catalog;
} yesno_bench;

struct game;
void yesno_init(yesno_bench *b);
void yesno_close(yesno_bench *b);
void yesno_open(struct game *g);
void yesno_poll(yesno_bench *b);
void yesno_key(struct game *g, const key_event *ev);
void yesno_click(struct game *g, int x, int y);
void yesno_scroll(yesno_bench *b, int rows);
void yesno_paste(yesno_bench *b, const char *text, size_t len);
void yesno_draw(sr_canvas *c, const yesno_bench *b);

const char *yesno_provider(int index);
const char *yesno_option(const yesno_bench *b, int row);
int yesno_options(const yesno_bench *b);
int yesno_history_row(const yesno_bench *b, int first, int offset);
int yesno_history_first(const yesno_bench *b, int rows);
int yesno_picker_first(const yesno_bench *b, int rows);
rect yesno_button_rect(int width, int height, int button);
rect yesno_history_rect(int width, int height);
rect yesno_picker_rect(int width, int height);
enum { BENCH_ROW_H = 25 };
#endif
