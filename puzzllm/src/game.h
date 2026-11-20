/* game.h - one play session: levels, the room, the two live screens, the
 * activity feed and saved progress. No terminal I/O of its own (termio.c
 * does that), so tests drive it headless with spawn disabled.
 */
#ifndef PUZZLLM_GAME_H
#define PUZZLLM_GAME_H

#include "activity.h"
#include "art.h"
#include "count_puzzle.h"
#include "keys.h"
#include "leaderboard.h"
#include "levels.h"
#include "pty.h"
#include "scene.h"
#include "soft_raster.h"
#include "render.h"

#include <stdbool.h>

enum { HINT_SECONDS = 45 };

enum { OLLAMA_NAME_CAP = 256 };

typedef enum thinking_support {
    THINK_UNAVAILABLE,
    THINK_SWITCHABLE,
    THINK_HAS_MINIMUM
} thinking_support;

typedef struct model_option {
    char name[OLLAMA_NAME_CAP];
    thinking_support thinking;
} model_option;

typedef struct game_menu {
    bool open;
    bool loading;
    bool load_failed;
    bool confirm_reset;
    bool details_open;
    bool leaderboard_open;
    bool campaign_board;
    score_entry *scores;
    int score_count, score_scroll, score_level, score_metric;
    bool model_scores;
    int model_submenu;          /* zero or the main menu's backend row */
    int model_selected;
    model_details_tab details_tab;
    int details_scroll;
    char prompt[MODEL_SPEC_CAP], write_schema[MODEL_SPEC_CAP];
    char prompt_draft[MODEL_SPEC_CAP];
    size_t prompt_cursor;
    bool prompt_edit;
    pty_proc prompt_counter;
    char prompt_count[48];
    char prompt_input[128];
    size_t prompt_input_len;
    char notice[320];
    int page;
    int selected;
    model_option *models;
    int count;
    pty_proc discovery;
    char line[OLLAMA_NAME_CAP + 1];
    size_t line_length;
} game_menu;

enum { CLIPBOARD_CAP = 16384 };

typedef struct agent_metrics {
    pty_proc worker;
    char partial[256], summary[128], devices[128];
    size_t length;
} agent_metrics;

typedef struct game {
    char root[2048];        /* the puzzllm/ directory */
    char state[2048];       /* persistent folder, e.g. ~/.local/share/puzzllm */
    char workspace[2100];
    level_list levels;
    room_art room;
    scene sc;
    int state_lock;
    bool started;
    bool spawn;             /* start the sandboxes (false in tests) */
    neuroterm omp_vt, shell_vt;
    history omp_history, shell_history;
    int goal_scroll, action_scroll;
    pane_selection selection;
    char clipboard[CLIPBOARD_CAP];
    unsigned long clipboard_generation;
    char primary[CLIPBOARD_CAP];
    unsigned long primary_generation;
    char paste[CLIPBOARD_CAP];
    size_t paste_len;
    bool paste_overflow;
    pty_proc omp, shell;
    agent_metrics metrics;
    activity act;
    score_entry ranked_attempt;
    unsigned long long ranked_session_tokens;
    char ranked_session_path[4096], ranked_session_model[SCORE_MODEL_CAP];
    double ranked_fraction_ms;
    goal_result goal;
    count_puzzle count_check;
    script_puzzle script_check;
    bool was_met;
    int hints_shown;
    double level_time;
    double poll_timer;
    double now;
    focus_id last_pane;
    unsigned pane_arrows;
    double held_until[4];   /* left, right, up, down */
    double tick_debt;       /* seconds not yet run as scene ticks */
    bool release_events;    /* the terminal reports key releases */
    bool quit;
    game_menu menu;
    char ollama_model[OLLAMA_NAME_CAP];
    bool use_codex;
    bool use_grok;
    bool use_claude;
    bool use_pi;
    bool yolo;
    thinking_support thinking_support;
    bool thinking_enabled;
    compute_mode compute;
    bool tooltips, room_buttons, hints_file, campaign;
    char campaign_model[SCORE_MODEL_CAP];
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
void game_pointer(game *g, int x, int y, pointer_action action);
void game_paste(game *g, const unsigned char *text, size_t len, bool final);
/* Wheel movement in rows, positive for down. */
void game_scroll(game *g, int x, int y, int rows);
/* Advance dt seconds: pump the terminals, poll the goal, and run the
 * room in fixed 1/60 s ticks, as Zell does. */
void game_update(game *g, double dt);
/* The frame is now w x h: lay the room out for it, and resize the two
 * terminals and their programs to match. True when the layout changed. */
bool game_resize(game *g, int w, int h);
void game_render(game *g, sr_canvas *c);
/* Progress file: STATE/progress, one line "level N". */
int game_load_progress(const char *state, int level_count);
bool game_neuronte_preference(const char *state);
bool game_reset_progress(const char *state);
goal_result game_check_level(const game *g, int level);
bool game_save_progress(const char *state, int level);

#endif
