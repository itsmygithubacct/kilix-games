/* render.h - compose one frame: room, door, screens, terminal text, Kilix.
 * Pictures come from art.h; this file owns text and layout. */
#ifndef PUZZLLM_RENDER_H
#define PUZZLLM_RENDER_H

#include "activity.h"
#include "art.h"
#include "levels.h"
#include "history.h"
#include "scene.h"
#include "soft_raster.h"
#include "neuroterm_terminal.h"

#include <stdbool.h>

typedef struct pane_selection {
    screen_id pane;
    int start, end;          /* row-major cell indices, inclusive */
    bool active, dragging;
} pane_selection;

typedef struct view {
    const scene *sc;
    const level *lv;
    const room_art *room;
    int next_room;
    const neuroterm *omp;            /* may be NULL */
    const neuroterm *shell;          /* may be NULL */
    const history *omp_history, *shell_history;
    const activity *act;
    int goal_scroll, action_scroll;
    pane_selection selection;
    const char *agent_model;
    const char *resource_summary, *resource_devices;
    const char *hosted_agent;
    bool pi;
    const char *omp_note;     /* shown over the omp screen when not NULL */
    const char *shell_note;
    const char *goal_why;     /* "hello_world.txt does not exist yet" */
    int hints_shown;          /* 0..nhints */
    bool tooltips;
    bool blink;               /* cursor blink phase */
} view;

typedef enum compute_mode { COMPUTE_AUTO, COMPUTE_CPU, COMPUTE_GPU } compute_mode;

enum { MENU_RESUME, MENU_QUIT, MENU_RESET, MENU_RESET_LEVEL, MENU_THINKING, MENU_COMPUTE, MENU_TOOLTIPS, MENU_MODEL_DETAILS, MENU_LEADERBOARD, MENU_HARNESS, MENU_CODEX, MENU_GROK, MENU_LOCAL, MENU_OLLAMA, MENU_NEURONTE };

enum { MODEL_SPEC_CAP = 16384 };
typedef enum model_details_tab { DETAILS_OLLAMA, DETAILS_LLAMA, DETAILS_CODEX, DETAILS_PI, DETAILS_GROK, DETAILS_COUNT } model_details_tab;

int render_model_details(sr_canvas *c, model_details_tab tab, int scroll,
                         const char *prompt, const char *schema, const char *count);
void render_prompt_editor(sr_canvas *c, const char *draft, size_t cursor, const char *notice);

struct model_option;
void render_menu(sr_canvas *c, int selected, const char *active_model,
                 bool started, int level, bool confirm_reset, const char *notice, int thinking_support, bool thinking_enabled, const char *hosted_agent,
                 bool pi, compute_mode compute, bool tooltips);
void render_model_menu(sr_canvas *c, bool ollama, const struct model_option *models,
                       int count, int selected, bool loading, bool failed,
                       const char *llama_model);
struct score_entry;
int render_leaderboard(sr_canvas *c, const struct score_entry *scores, int count,
                       int scroll, int level, int metric, bool models, const char *notice,
                       const struct score_entry *attempt);
size_t render_copy_selection(const view *v, char *out, size_t cap);
int render_scroll_limit(const view *v, screen_id id);
void render_frame(sr_canvas *c, const view *v);
#endif
