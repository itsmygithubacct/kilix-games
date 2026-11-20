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

/* A campaign run is one model across every playable room. The board file
 * holds only finished campaigns. `needed` is the playable level numbers. */
bool campaign_same_model(const char *lock, const char *model);
bool campaign_active_model(const char *state, char model[SCORE_MODEL_CAP]);
bool campaign_restart(const char *state, const char *model, const char *player,
                      unsigned long long id);
bool campaign_drop_level(const char *state, int level, unsigned long long new_id);
bool campaign_note(const char *state, const score_entry *room, const int *needed,
                   int needed_count, bool *just_ranked);
bool campaign_load(const char *state, score_entry **entries, int *count);
bool campaign_board_clear(const char *state);

#endif
