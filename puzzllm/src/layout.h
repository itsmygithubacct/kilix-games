/* layout.h - where things are in the room, for the pane's size.
 *
 *   +------------------------- back wall ---[door]---------------------+
 *   | +-- coding agent -----------+  aisle  +-- goal and hints --------+ |
 *   | |                           |         +--------------------------+ |
 *   | |                           |         +-- system actions -------+ |
 *   | |                           |         +--------------------------+ |
 *   | |                           |         | resources                 |
 *   | |                           |         +-- shell -----------------+ |
 *   | +---------------------------+         +--------------------------+ |
 *   |                          floor                                      |
 *   +---------------------------------------------------------------------+
 *
 * The room is drawn at the pane's own size, as Zell draws its rooms: the
 * screens take what the pane gives, in whole 8x16 cells, and the floor and
 * the aisle keep their size. Below LAYOUT_MIN_W x LAYOUT_MIN_H the room is
 * laid out at that minimum and the frame is shrunk to fit; above
 * LAYOUT_MAX_W x LAYOUT_MAX_H it is centred in the pane.
 *
 * Kilix walks the floor and the aisle; the door at the top of the aisle
 * opens when the level's goal is met.
 */
#ifndef PUZZLLM_LAYOUT_H
#define PUZZLLM_LAYOUT_H

enum {
    LAYOUT_MIN_W = 960, LAYOUT_MIN_H = 600,
    LAYOUT_MAX_W = 2048, LAYOUT_MAX_H = 1280,
    /* the size tests and --render use */
    LAYOUT_DEFAULT_W = 1280, LAYOUT_DEFAULT_H = 800,

    CELL_W = 8,
    CELL_H = 16,
    SCREEN_TITLE_H = 22,
    SCREEN_PAD = 8,
    SCREEN_BEZEL = 4,
    SCREEN_MAX_COLS = 150,
    SCREEN_TEXT_ROWS_MAX = 9,
    RESOURCE_H = 36,
    DOOR_W = 80, DOOR_H = 86, DOOR_Y = 43,
    WALL_H = DOOR_Y + DOOR_H,
    AISLE_Y0 = WALL_H + 18,
    DOOR_EXIT_Y = WALL_H - 16
};

typedef struct layout {
    int w, h;
    int door_x;
    int omp_x, omp_y, omp_cols, omp_rows;
    int shell_x, shell_y, shell_cols, shell_rows;
    int act_x, act_y, act_cols, act_rows;
    int goal_x, goal_y, goal_cols, goal_rows;
    int resource_y;
    int floor_y, hud_y;
    int walk_x0, walk_x1, walk_y0, walk_y1;   /* where Kilix's feet may go */
    int aisle_x0, aisle_x1;
    int start_x, start_y;
} layout;

/* The room's current layout; layout_set changes it. */
extern layout L;

/* Lay the room out for a w x h frame (clamped to the limits above).
 * Returns true when anything moved. */
int layout_set(int w, int h);

#define CANVAS_W (L.w)
#define CANVAS_H (L.h)
#define DOOR_X (L.door_x)
#define OMP_X (L.omp_x)
#define OMP_Y (L.omp_y)
#define OMP_COLS (L.omp_cols)
#define OMP_ROWS (L.omp_rows)
#define SHELL_X (L.shell_x)
#define SHELL_Y (L.shell_y)
#define SHELL_COLS (L.shell_cols)
#define SHELL_ROWS (L.shell_rows)
#define ACT_X (L.act_x)
#define ACT_Y (L.act_y)
#define ACT_COLS (L.act_cols)
#define ACT_ROWS (L.act_rows)
#define GOAL_COLS (L.goal_cols)
#define GOAL_ROWS (L.goal_rows)
#define FLOOR_Y (L.floor_y)
#define HUD_Y (L.hud_y)
#define WALK_X0 (L.walk_x0)
#define WALK_X1 (L.walk_x1)
#define WALK_Y0 (L.walk_y0)
#define WALK_Y1 (L.walk_y1)
#define AISLE_X0 (L.aisle_x0)
#define AISLE_X1 (L.aisle_x1)
#define START_X (L.start_x)
#define START_Y (L.start_y)

/* Outer rectangle of a screen with a terminal of cols x rows inside. */
#define SCREEN_W(cols) ((cols) * CELL_W + 2 * SCREEN_PAD)
#define SCREEN_H(rows) ((rows) * CELL_H + SCREEN_TITLE_H + 2 * SCREEN_PAD)

typedef enum screen_id {
    SCREEN_NONE = -1, SCREEN_OMP = 0, SCREEN_SHELL = 1, SCREEN_ACT = 2, SCREEN_GOAL = 3,
    SCREEN_COUNT = 4
} screen_id;

typedef struct rect { int x, y, w, h; } rect;

rect layout_screen(screen_id id);
enum { ROOM_PANE_COUNT = SCREEN_COUNT + 1 };
/* Pane frames that block walking. */
void layout_room_panes(rect panes[ROOM_PANE_COUNT]);
/* Top-left pixel of the text area of a screen. */
rect layout_resources(void);
typedef enum room_button {
    BUTTON_CLEAR_MODEL, BUTTON_CLEAR_SHELL, BUTTON_HARNESS, BUTTON_MODEL,
    BUTTON_SHELL_LOG, BUTTON_RESET_LEVEL, BUTTON_RESET_GAME, BUTTON_METRICS,
    BUTTON_AUTO, BUTTON_CLEAN_DISK,
    ROOM_BUTTON_COUNT, BUTTON_NONE = -1
} room_button;
rect layout_room_button(room_button button);
rect layout_text_bounds(screen_id id);
void layout_text_origin(screen_id id, int *x, int *y);

#endif
