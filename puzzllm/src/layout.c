/* layout.c - see layout.h. */
#include "layout.h"

layout L;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int layout_set(int w, int h)
{
    layout n;
    int half, gap = 48, top = 112, panel_gap = 12, bottom, col_w, height;

    w = clamp(w, LAYOUT_MIN_W, LAYOUT_MAX_W);
    h = clamp(h, LAYOUT_MIN_H, LAYOUT_MAX_H);
    half = w / 2;

    n.w = w;
    n.h = h;
    n.door_x = half - DOOR_W / 2;
    n.hud_y = h - 20;
    n.walk_y1 = h - 32;
    n.walk_y0 = h - 132;
    n.floor_y = h - 160;
    n.walk_x0 = 30;
    n.walk_x1 = w - 30;
    n.aisle_x0 = half - 24;
    n.aisle_x1 = half + 24;
    n.start_x = half;
    n.start_y = h - 60;

    bottom = n.floor_y - 10;
    height = bottom - top;
    col_w = half - gap - 16;
    n.omp_x = 16;
    n.omp_y = top;
    n.omp_cols = clamp((col_w - 2 * SCREEN_PAD) / CELL_W, 20, SCREEN_MAX_COLS);
    n.omp_rows = (height - SCREEN_H(0)) / CELL_H;

    int right_rows = (height - 3 * SCREEN_H(0) - 2 * panel_gap) / CELL_H;
    n.goal_x = half + gap;
    n.goal_y = top;
    n.goal_cols = n.omp_cols;
    n.goal_rows = clamp(right_rows / 3, 3, 8);
    n.act_x = n.goal_x;
    n.act_y = n.goal_y + SCREEN_H(n.goal_rows) + panel_gap;
    n.act_cols = n.goal_cols;
    int action_budget = right_rows - n.goal_rows - 6; /* reserve six shell rows */
    n.act_rows = clamp(clamp(right_rows / 4, 2, 9), 2, action_budget);
    n.shell_x = n.goal_x;
    n.shell_y = n.act_y + SCREEN_H(n.act_rows) + panel_gap;
    n.shell_cols = n.goal_cols;
    n.shell_rows = right_rows - n.goal_rows - n.act_rows;

    {
        int moved = n.w != L.w || n.h != L.h;
        L = n;
        return moved;
    }
}

rect layout_screen(screen_id id)
{
    rect r = {0, 0, 0, 0};
    switch (id) {
    case SCREEN_OMP:
        r.x = OMP_X; r.y = OMP_Y; r.w = SCREEN_W(OMP_COLS); r.h = SCREEN_H(OMP_ROWS);
        break;
    case SCREEN_SHELL:
        r.x = SHELL_X; r.y = SHELL_Y; r.w = SCREEN_W(SHELL_COLS); r.h = SCREEN_H(SHELL_ROWS);
        break;
    case SCREEN_ACT:
        r.x = ACT_X; r.y = ACT_Y; r.w = SCREEN_W(ACT_COLS); r.h = SCREEN_H(ACT_ROWS);
        break;
    case SCREEN_GOAL:
        r.x = L.goal_x; r.y = L.goal_y; r.w = SCREEN_W(GOAL_COLS); r.h = SCREEN_H(GOAL_ROWS);
        break;
    default:
        break;
    }
    return r;
}

void layout_text_origin(screen_id id, int *x, int *y)
{
    rect r = layout_screen(id);
    *x = r.x + SCREEN_PAD;
    *y = r.y + SCREEN_TITLE_H + SCREEN_PAD;
}
