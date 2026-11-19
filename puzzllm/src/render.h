/* render.h - compose one frame: room, door, screens, terminal text, Kilix.
 * Pictures come from art.h; this file owns text and layout. */
#ifndef PUZZLLM_RENDER_H
#define PUZZLLM_RENDER_H

#include "activity.h"
#include "levels.h"
#include "scene.h"
#include "soft_raster.h"
#include "neuroterm.h"

#include <stdbool.h>

typedef struct view {
    const scene *sc;
    const level *lv;
    int level_count;
    const neuroterm *omp;            /* may be NULL */
    const neuroterm *shell;          /* may be NULL */
    const activity *act;
    const char *agent_model;
    const char *omp_note;     /* shown over the omp screen when not NULL */
    const char *shell_note;
    const char *goal_why;     /* "hello_world.txt does not exist yet" */
    int hints_shown;          /* 0..nhints */
    bool blink;               /* cursor blink phase */
} view;

enum { MENU_RESUME, MENU_QUIT, MENU_RESET, MENU_LOCAL, MENU_FIRST_OLLAMA };

void render_menu(sr_canvas *c, char *const *models, int count, int selected,
                 const char *active_model, bool loading, bool failed,
                 bool started, int level, bool confirm_reset, const char *notice);
void render_frame(sr_canvas *c, const view *v);
#endif
