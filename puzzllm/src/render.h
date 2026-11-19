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
    const level *next_level;
    const neuroterm *omp;            /* may be NULL */
    const neuroterm *shell;          /* may be NULL */
    const history *omp_history, *shell_history;
    const activity *act;
    int goal_scroll, action_scroll;
    pane_selection selection;
    const char *agent_model;
    const char *resource_summary, *resource_devices;
    bool codex;
    const char *omp_note;     /* shown over the omp screen when not NULL */
    const char *shell_note;
    const char *goal_why;     /* "hello_world.txt does not exist yet" */
    int hints_shown;          /* 0..nhints */
    bool blink;               /* cursor blink phase */
} view;

typedef enum compute_mode { COMPUTE_AUTO, COMPUTE_CPU, COMPUTE_GPU } compute_mode;

enum { MENU_RESUME, MENU_QUIT, MENU_RESET, MENU_THINKING, MENU_COMPUTE, MENU_MODEL_DETAILS, MENU_CODEX, MENU_LOCAL, MENU_FIRST_OLLAMA };

enum { MODEL_SPEC_CAP = 4096 };
typedef enum model_details_tab { DETAILS_OLLAMA, DETAILS_LLAMA, DETAILS_CODEX, DETAILS_COUNT } model_details_tab;

int render_model_details(sr_canvas *c, model_details_tab tab, int scroll,
                         const char *prompt, const char *schema);

struct model_option;
void render_menu(sr_canvas *c, const struct model_option *models, int count, int selected,
                 const char *active_model, bool loading, bool failed,
                 bool started, int level, bool confirm_reset, const char *notice, int thinking_support, bool thinking_enabled, bool codex,
                 compute_mode compute);
size_t render_copy_selection(const view *v, char *out, size_t cap);
int render_scroll_limit(const view *v, screen_id id);
void render_frame(sr_canvas *c, const view *v);
#endif
