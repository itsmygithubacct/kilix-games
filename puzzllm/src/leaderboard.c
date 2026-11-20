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

static FILE *open_score_file(const char *state, const char *name, bool write)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", state, name);
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

static bool load_score_file(const char *state, const char *name, score_entry **entries, int *count)
{
    *entries = NULL;
    *count = 0;
    FILE *file = open_score_file(state, name, false);
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

bool leaderboard_load(const char *state, score_entry **entries, int *count)
{
    return load_score_file(state, "leaderboard.tsv", entries, count);
}

static bool submit_score_file(const char *state, const char *name, const score_entry *score)
{
    FILE *file = open_score_file(state, name, true);
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

bool leaderboard_submit(const char *state, const score_entry *score)
{
    return submit_score_file(state, "leaderboard.tsv", score);
}

bool campaign_same_model(const char *lock, const char *model)
{
    size_t n;
    if (!lock || !model || !lock[0] || !model[0]) return false;
    if (!strcmp(lock, model)) return true;
    n = strlen(lock);
    return strncmp(model, lock, n) == 0 && model[n] == '/';
}

enum { CAMPAIGN_ROOM_MAX = 64 };

typedef struct campaign_run {
    unsigned long long id;
    bool submitted;
    char lock[SCORE_MODEL_CAP], model[SCORE_MODEL_CAP], player[SCORE_PLAYER_CAP];
    int count;
    score_entry rooms[CAMPAIGN_ROOM_MAX];
} campaign_run;

static bool read_campaign_run(const char *state, campaign_run *run)
{
    char path[4096], line[SCORE_LINE_CAP];
    snprintf(path, sizeof path, "%s/campaign-run", state);
    memset(run, 0, sizeof *run);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return false; }
    while (fgets(line, sizeof line, file)) {
        unsigned long long id;
        int submitted;
        char text[SCORE_MODEL_CAP], player[SCORE_PLAYER_CAP];
        if (sscanf(line, "id %llu", &id) == 1) run->id = id;
        else if (sscanf(line, "lock %271[^\r\n]", text) == 1)
            snprintf(run->lock, sizeof run->lock, "%s", text);
        else if (sscanf(line, "model %271[^\r\n]", text) == 1)
            snprintf(run->model, sizeof run->model, "%s", text);
        else if (sscanf(line, "player %63[^\r\n]", player) == 1)
            snprintf(run->player, sizeof run->player, "%s", player);
        else if (sscanf(line, "submitted %d", &submitted) == 1) run->submitted = submitted != 0;
        else {
            score_entry score = {0};
            if (score_parse(line, &score) && run->count < CAMPAIGN_ROOM_MAX)
                run->rooms[run->count++] = score;
        }
    }
    bool ok = !ferror(file);
    fclose(file);
    if (!run->lock[0]) snprintf(run->lock, sizeof run->lock, "%s", run->model);
    return ok && run->id > 0;
}

static bool write_campaign_run(const char *state, const campaign_run *run)
{
    int dir = open(state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return false;
    unlinkat(dir, ".campaign-run", 0);
    int fd = openat(dir, ".campaign-run", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { close(dir); return false; }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlinkat(dir, ".campaign-run", 0);
        close(dir);
        return false;
    }
    bool ok = fprintf(file, "id %llu\nlock %s\nmodel %s\nplayer %s\nsubmitted %d\n",
                      run->id, run->lock, run->model, run->player, run->submitted ? 1 : 0) > 0;
    for (int i = 0; ok && i < run->count; i++) {
        char line[SCORE_LINE_CAP];
        score_format(line, &run->rooms[i]);
        ok = fprintf(file, "%s\n", line) > 0;
    }
    if (fclose(file) != 0) ok = false;
    if (ok) ok = renameat(dir, ".campaign-run", dir, "campaign-run") == 0;
    if (!ok) unlinkat(dir, ".campaign-run", 0);
    close(dir);
    return ok;
}

bool campaign_active_model(const char *state, char model[SCORE_MODEL_CAP])
{
    campaign_run run;
    model[0] = 0;
    if (!read_campaign_run(state, &run) || !run.lock[0]) return false;
    snprintf(model, SCORE_MODEL_CAP, "%s", run.lock);
    return true;
}

bool campaign_restart(const char *state, const char *model, const char *player,
                      unsigned long long id)
{
    campaign_run run = {0};
    if (!id || !model || !model[0]) return false;
    run.id = id;
    leaderboard_label(run.lock, sizeof run.lock, model);
    leaderboard_label(run.model, sizeof run.model, model);
    leaderboard_label(run.player, sizeof run.player, player ? player : "");
    if (!run.player[0]) snprintf(run.player, sizeof run.player, "Player");
    return run.lock[0] && write_campaign_run(state, &run);
}

bool campaign_drop_level(const char *state, int level, unsigned long long new_id)
{
    campaign_run run;
    if (!read_campaign_run(state, &run)) return errno == ENOENT;
    int kept = 0;
    bool removed = false;
    for (int i = 0; i < run.count; i++) {
        if (run.rooms[i].level == level) {
            removed = true;
            continue;
        }
        run.rooms[kept++] = run.rooms[i];
    }
    run.count = kept;
    if (removed && run.submitted && new_id) {
        run.id = new_id;
        run.submitted = false;
    }
    return write_campaign_run(state, &run);
}

static bool campaign_rooms_ready(const campaign_run *run, const int *needed, int needed_count)
{
    if (needed_count <= 0 || run->count < needed_count) return false;
    for (int i = 0; i < needed_count; i++) {
        bool found = false;
        for (int j = 0; j < run->count; j++)
            if (run->rooms[j].level == needed[i] && run->rooms[j].measure[SCORE_TOKENS] > 0)
                found = true;
        if (!found) return false;
    }
    return true;
}

static bool campaign_submit(const char *state, const score_entry *score)
{
    return submit_score_file(state, "campaign.tsv", score);
}

bool campaign_note(const char *state, const score_entry *room, const int *needed,
                   int needed_count, bool *just_ranked)
{
    campaign_run run;
    if (just_ranked) *just_ranked = false;
    if (!room || !read_campaign_run(state, &run)) return false;
    if (room->measure[SCORE_TOKENS] <= 0 || !strcmp(room->model, "*") ||
        !campaign_same_model(run.lock, room->model))
        return true;
    bool found = false;
    for (int i = 0; i < run.count; i++)
        if (run.rooms[i].level == room->level) found = true;
    if (!found && run.count < CAMPAIGN_ROOM_MAX) run.rooms[run.count++] = *room;
    if (strlen(room->model) > strlen(run.model) && campaign_same_model(run.lock, room->model))
        snprintf(run.model, sizeof run.model, "%s", room->model);
    bool ready = campaign_rooms_ready(&run, needed, needed_count);
    if (ready && !run.submitted) {
        score_entry total = {.level = needed_count, .id = run.id};
        snprintf(total.model, sizeof total.model, "%s", run.model);
        snprintf(total.player, sizeof total.player, "%s", run.player);
        for (int metric = 0; metric < SCORE_METRICS; metric++)
            total.measure[metric] = metric < SCORE_CPU ? 0 : -1;
        for (int i = 0; i < needed_count; i++) {
            for (int j = 0; j < run.count; j++) {
                const score_entry *part = &run.rooms[j];
                if (part->level != needed[i]) continue;
                total.measure[SCORE_TOKENS] += part->measure[SCORE_TOKENS];
                total.measure[SCORE_TIME] += part->measure[SCORE_TIME];
                for (int metric = SCORE_CPU; metric < SCORE_METRICS; metric++) {
                    long long value = part->measure[metric];
                    if (value >= 0 && (total.measure[metric] < 0 || value > total.measure[metric]))
                        total.measure[metric] = value;
                }
                break;
            }
        }
        if (total.measure[SCORE_TOKENS] <= 0 || !campaign_submit(state, &total)) return false;
        run.submitted = true;
        if (just_ranked) *just_ranked = true;
    }
    return write_campaign_run(state, &run);
}

bool campaign_load(const char *state, score_entry **entries, int *count)
{
    return load_score_file(state, "campaign.tsv", entries, count);
}

bool campaign_board_clear(const char *state)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/campaign.tsv", state);
    return unlink(path) == 0 || errno == ENOENT;
}
