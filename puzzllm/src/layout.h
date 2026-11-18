/* layout.h - where things are in the 1280x800 room.
 *
 *   +------------------------- back wall ---[door]---------------------+
 *   | +-- omp (agent) ------------+  aisle  +-- shell -----------------+ |
 *   | |                           |         |                          | |
 *   | |                           |         +--------------------------+ |
 *   | |                           |         +-- agent / goal / hints --+ |
 *   | |                           |         |                          | |
 *   | +---------------------------+         +--------------------------+ |
 *   |                          floor                                      |
 *   +---------------------------------------------------------------------+
 *
 * Kilix walks the floor and the aisle; the door at the top of the aisle
 * opens when the level's goal is met.
 */
#ifndef PUZZLLM_LAYOUT_H
#define PUZZLLM_LAYOUT_H

enum {
    CANVAS_W = 1280,
    CANVAS_H = 800,
    CELL_W = 8,
    CELL_H = 16,

    WALL_H = 100,
    DOOR_X = 600, DOOR_Y = 14, DOOR_W = 80, DOOR_H = 86,

    SCREEN_TITLE_H = 22,
    SCREEN_PAD = 8,

    OMP_X = 16, OMP_Y = 112, OMP_COLS = 70, OMP_ROWS = 30,
    SHELL_X = 688, SHELL_Y = 112, SHELL_COLS = 70, SHELL_ROWS = 13,
    ACT_X = 688, ACT_Y = 380, ACT_COLS = 70, ACT_ROWS = 13,

    FLOOR_Y = 640,
    HUD_Y = 780,

    /* where Kilix's feet may go */
    WALK_X0 = 30, WALK_X1 = 1250, WALK_Y0 = 668, WALK_Y1 = 768,
    AISLE_X0 = 616, AISLE_X1 = 664, AISLE_Y0 = 118,
    DOOR_EXIT_Y = 84,

    START_X = 640, START_Y = 740
};

/* Outer rectangle of a screen with a terminal of cols x rows inside. */
#define SCREEN_W(cols) ((cols) * CELL_W + 2 * SCREEN_PAD)
#define SCREEN_H(rows) ((rows) * CELL_H + SCREEN_TITLE_H + 2 * SCREEN_PAD)

typedef enum screen_id {
    SCREEN_NONE = -1, SCREEN_OMP = 0, SCREEN_SHELL = 1, SCREEN_ACT = 2,
    SCREEN_COUNT = 3
} screen_id;

typedef struct rect { int x, y, w, h; } rect;

rect layout_screen(screen_id id);
/* Top-left pixel of the text area of a screen. */
void layout_text_origin(screen_id id, int *x, int *y);

#endif
