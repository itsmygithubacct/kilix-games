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
    int half, gap = 48, top = 112, bottom, col_w, right_h, act_h;

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

    /* screens: omp on the left, goals and activity over the shell on the
     * right, between the wall and the floor, in whole cells */
    bottom = n.floor_y - 10;
    col_w = half - gap - 16;
    n.omp_x = 16;
    n.omp_y = top;
    n.omp_cols = clamp((col_w - 2 * SCREEN_PAD) / CELL_W, 20, SCREEN_MAX_COLS);
    n.omp_rows = clamp((bottom - top - SCREEN_TITLE_H - 2 * SCREEN_PAD) / CELL_H, 6, 80);

    right_h = (bottom - top - 22) / 2;
    act_h = right_h < SCREEN_H(8) ? SCREEN_H(8) : right_h;
    n.act_x = half + gap;
    n.act_y = top;
    n.act_cols = n.omp_cols;
    n.act_rows = clamp((act_h - SCREEN_TITLE_H - 2 * SCREEN_PAD) / CELL_H, 8, 40);
    n.shell_x = n.act_x;
    n.shell_y = top + act_h + 22;
    n.shell_cols = n.omp_cols;
    n.shell_rows = clamp((bottom - n.shell_y - SCREEN_TITLE_H - 2 * SCREEN_PAD) / CELL_H,
                         4, 40);

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
