/* game.h - one play session: levels, the room, the two live screens, the
 * activity feed and saved progress. No terminal I/O of its own (termio.c
 * does that), so tests drive it headless with spawn disabled.
 */
#ifndef PUZZLLM_GAME_H
#define PUZZLLM_GAME_H

#include "activity.h"
#include "keys.h"
#include "levels.h"
#include "pty.h"
#include "scene.h"
#include "soft_raster.h"
#include "vt.h"

#include <stdbool.h>

enum { HINT_SECONDS = 45 };

enum { OLLAMA_NAME_CAP = 256 };

typedef struct game_menu {
    bool open;
    bool loading;
    bool load_failed;
    int selected;
    char **models;
    int count;
    pty_proc discovery;
    char line[OLLAMA_NAME_CAP];
    size_t line_length;
} game_menu;

typedef struct game {
    char root[2048];        /* the puzzllm/ directory */
    char state[2048];       /* persistent folder, e.g. ~/.local/share/puzzllm */
    char workspace[2100];
    level_list levels;
    scene sc;
    int state_lock;
    bool spawn;             /* start the sandboxes (false in tests) */
    vt omp_vt, shell_vt;
    pty_proc omp, shell;
    activity act;
    goal_result goal;
    bool was_met;
    int hints_shown;
    double level_time;
    double poll_timer;
    double now;
    double held_until[4];   /* left, right, up, down */
    double tick_debt;       /* seconds not yet run as scene ticks */
    bool release_events;    /* the terminal reports key releases */
    bool quit;
    game_menu menu;
    char ollama_model[OLLAMA_NAME_CAP];
    bool transition_pending;
    char omp_note[96], shell_note[96];
} game;

bool game_init(game *g, const char *root, const char *state, bool spawn,
               char *err, size_t errcap);
void game_shutdown(game *g);
/* Enter room `level` (an index): Kilix at the entrance, first hint shown,
 * goal re-checked; a goal met earlier opens the door at once. */
void game_enter_level(game *g, int level);
/* Re-check the current level's goal now (also done every 0.5 s). */
void game_poll_goal(game *g);
void game_key(game *g, const key_event *ev);
/* Mouse press at canvas coordinates. */
void game_click(game *g, int x, int y);
/* Advance dt seconds: pump the terminals, poll the goal, and run the
 * room in fixed 1/60 s ticks, as Zell does. */
void game_update(game *g, double dt);
/* The frame is now w x h: lay the room out for it, and resize the two
 * terminals and their programs to match. True when the layout changed. */
bool game_resize(game *g, int w, int h);
void game_render(game *g, sr_canvas *c);
/* Progress file: STATE/progress, one line "level N". */
int game_load_progress(const char *state, int level_count);
bool game_reset_progress(const char *state);
goal_result game_check_level(const game *g, int level);
bool game_save_progress(const char *state, int level);

#endif
