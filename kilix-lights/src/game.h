/* game.h - deterministic Lights Out rules and state. */
#ifndef KILIX_LIGHTS_GAME_H
#define KILIX_LIGHTS_GAME_H

#include "types.h"

typedef struct GameRect {
    int x, y, w, h;
} GameRect;

typedef struct Game {
    int size;
    int moves;
    int puzzle_number;
    int focus_cell;
    int hover_cell;
    int pressed_cell;
    int history_len;
    int best[3];
    uint32_t seed;
    uint64_t board;
    uint64_t initial;
    uint64_t generation_mask;
    uint64_t history[128];
    double pulse;
    double win_time;
    double cell_anim[MAX_CELLS];
    bool won;
    bool help;
    bool keyboard_focus;
} Game;

void game_init(Game *game);
void game_new(Game *game);
void game_reset(Game *game);
bool game_undo(Game *game);
void game_set_size(Game *game, int size);

uint64_t game_toggle_mask(int size, int cell);
bool game_activate(Game *game, int cell, bool *ended_on);
bool game_cell_on(const Game *game, int cell);
int game_lit_count(const Game *game);
int game_mode_index(int size);
const char *game_mode_name(int size);

GameRect game_cell_rect(const Game *game, int cell);
int game_cell_from_rect(const Game *game, int x, int y);
void game_update(Game *game, double dt);

/* Headless invariants used by the shipped binary's self-test. */
bool game_rules_selftest(void);

#endif
