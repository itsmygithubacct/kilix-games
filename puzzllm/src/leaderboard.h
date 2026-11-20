#ifndef PUZZLLM_LEADERBOARD_H
#define PUZZLLM_LEADERBOARD_H

#include <stdbool.h>

enum { SCORE_MODEL_CAP = 272, SCORE_PLAYER_CAP = 64, SCORE_LINE_CAP = 640 };
enum { SCORE_TOKENS, SCORE_TIME, SCORE_CPU, SCORE_RAM, SCORE_GPU, SCORE_VRAM, SCORE_DISK, SCORE_METRICS };

typedef struct score_entry {
    int level, rank;
    unsigned long long id;
    long long measure[SCORE_METRICS];
    char model[SCORE_MODEL_CAP], player[SCORE_PLAYER_CAP];
} score_entry;

bool leaderboard_load(const char *state, score_entry **entries, int *count);
bool leaderboard_submit(const char *state, const score_entry *score);
void leaderboard_label(char *out, int cap, const char *text);
bool score_parse(const char *line, score_entry *score);
void score_format(char line[SCORE_LINE_CAP], const score_entry *score);
void leaderboard_rank(score_entry *scores, int count, int level, int metric, bool models);
extern const char *const leaderboard_metric[SCORE_METRICS];

#endif
