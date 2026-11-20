#define _GNU_SOURCE
#include "leaderboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void leaderboard_label(char *out, int cap, const char *text)
{
    int n = 0;
    for (; *text && n < cap - 1; text++)
        if ((unsigned char)*text >= 32 && *text != 127) out[n++] = *text;
    out[n] = 0;
}

static FILE *open_scores(const char *state, bool write)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/leaderboard.tsv", state);
    int flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                (write ? O_RDWR | O_CREAT | O_APPEND : O_RDONLY);
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    FILE *file = fdopen(fd, write ? "a+" : "r");
    if (!file) close(fd);
    return file;
}

const char *const leaderboard_metric[SCORE_METRICS] = {
    "Tokens", "Time", "Peak CPU", "Peak RAM", "Peak GPU", "Peak VRAM", "Peak disk I/O",
};

bool score_parse(const char *line, score_entry *score)
{
    long long *m = score->measure;
    return sscanf(line, "%d\t%llu\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%271[^\t\n]\t%63[^\r\n]",
                  &score->level, &score->id, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6],
                  score->model, score->player) == 11 &&
           score->level > 0 && score->id > 0 && m[SCORE_TOKENS] >= 0;
}

void score_format(char line[SCORE_LINE_CAP], const score_entry *score)
{
    const long long *m = score->measure;
    snprintf(line, SCORE_LINE_CAP, "%d\t%llu\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%s\t%s",
             score->level, score->id, m[0], m[1], m[2], m[3], m[4], m[5], m[6], score->model, score->player);
}

typedef struct rank_order {
    int metric;
    bool group;
} rank_order;

static int compare_scores(const void *a, const void *b, void *data)
{
    const score_entry *x = a, *y = b;
    const rank_order *order = data;
    if (x->level != y->level) return x->level < y->level ? -1 : 1;
    if (order->group) {
        int model = strcmp(x->model, y->model);
        if (model) return model;
    } else if (!!x->rank != !!y->rank) {
        return x->rank ? -1 : 1;
    }
    long long xm = x->measure[order->metric], ym = y->measure[order->metric];
    if ((xm < 0) != (ym < 0)) return xm < 0 ? 1 : -1;
    if (xm != ym) return xm < ym ? -1 : 1;
    return strcmp(x->player, y->player);
}

void leaderboard_rank(score_entry *scores, int count, int level, int metric, bool models)
{
    rank_order order = {.metric = models ? metric : SCORE_TOKENS, .group = true};
    if (count > 1) qsort_r(scores, (size_t)count, sizeof *scores, compare_scores, &order);
    for (int i = 0; i < count; i++) {
        score_entry *row = &scores[i];
        bool same = i && row->level == row[-1].level && !strcmp(row->model, row[-1].model);
        row->rank = 0;
        if (row->level == level && row->measure[order.metric] >= 0 &&
            (!models || !same))
            row->rank = models || !same ? 1 : row[-1].rank + 1;
    }
    if (!models) return;
    order.group = false;
    if (count > 1) qsort_r(scores, (size_t)count, sizeof *scores, compare_scores, &order);
    int rank = 0;
    for (int i = 0; i < count; i++)
        if (scores[i].rank) scores[i].rank = ++rank;
}

bool leaderboard_load(const char *state, score_entry **entries, int *count)
{
    *entries = NULL;
    *count = 0;
    FILE *file = open_scores(state, false);
    if (!file) return errno == ENOENT;
    bool ok = true;
    int capacity = 0;
    char line[SCORE_LINE_CAP];
    while (fgets(line, sizeof line, file)) {
        score_entry score = {0};
        if (!score_parse(line, &score)) continue;
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            score_entry *more = realloc(*entries, (size_t)capacity * sizeof **entries);
            if (!more) { ok = false; break; }
            *entries = more;
        }
        (*entries)[(*count)++] = score;
    }
    if (ferror(file)) ok = false;
    fclose(file);
    if (!ok) {
        free(*entries);
        *entries = NULL;
        *count = 0;
        return false;
    }
    return true;
}

bool leaderboard_submit(const char *state, const score_entry *score)
{
    FILE *file = open_scores(state, true);
    if (!file) return false;
    rewind(file);
    char line[SCORE_LINE_CAP];
    while (fgets(line, sizeof line, file)) {
        score_entry old;
        if (score_parse(line, &old) && old.id == score->id) {
            fclose(file);
            return true;
        }
    }
    score_format(line, score);
    bool ok = !ferror(file) && fseek(file, 0, SEEK_END) == 0 && fprintf(file, "%s\n", line) > 0;
    if (fclose(file) != 0) ok = false;
    return ok;
}
