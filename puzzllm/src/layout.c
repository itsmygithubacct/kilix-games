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
    int half, gap = 48, top = 16, panel_gap = 10, status_gap = 6, col_w;

    w = clamp(w, LAYOUT_MIN_W, LAYOUT_MAX_W);
    h = clamp(h, LAYOUT_MIN_H, LAYOUT_MAX_H);
    half = w / 2;

    n.w = w;
    n.h = h;
    n.door_x = half - DOOR_W / 2;
    n.hud_y = h - 20;
    n.walk_y1 = h - 32;
    n.walk_y0 = h - 120;
    n.floor_y = h - 160;
    n.walk_x0 = 30;
    n.walk_x1 = w - 30;
    n.aisle_x0 = half - 20;
    n.aisle_x1 = half + 20;
    n.start_x = half;

    int height = n.floor_y + 6 - top;
    col_w = half - gap - 16;
    n.omp_x = 16;
    n.omp_y = top;
    n.omp_cols = clamp((col_w - 2 * SCREEN_PAD) / CELL_W, 20, SCREEN_MAX_COLS);
    int terminal_bottom = n.hud_y - 28;
    n.omp_rows = (terminal_bottom - n.omp_y - SCREEN_H(0)) / CELL_H;

    int right_rows = (height - 3 * SCREEN_H(0) - panel_gap -
                      2 * status_gap - RESOURCE_H) / CELL_H;
    n.goal_x = half + gap;
    n.goal_y = top;
    n.goal_cols = n.omp_cols;
    n.goal_rows = clamp(right_rows / 4, 2, 6);
    n.act_x = n.goal_x;
    n.act_y = n.goal_y + SCREEN_H(n.goal_rows) + panel_gap;
    n.act_cols = n.goal_cols;
    int action_budget = right_rows - n.goal_rows - 6; /* reserve six shell rows */
    n.act_rows = clamp(clamp(right_rows / 4, 2, SCREEN_TEXT_ROWS_MAX), 2, action_budget);
    n.shell_x = n.goal_x;
    n.resource_y = n.act_y + SCREEN_H(n.act_rows) + status_gap;
    n.shell_y = n.resource_y + RESOURCE_H + status_gap;
    n.shell_cols = n.goal_cols;
    n.shell_rows = (n.hud_y - 52 - n.shell_y - SCREEN_H(0)) / CELL_H;

    int omp_bottom = n.omp_y + SCREEN_H(n.omp_rows);
    int shell_bottom = n.shell_y + SCREEN_H(n.shell_rows);
    int pane_bottom = omp_bottom > shell_bottom ? omp_bottom : shell_bottom;
    n.start_y = clamp(h - 60, pane_bottom + SCREEN_BEZEL, n.walk_y1);

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
    rect r = layout_text_bounds(id);
    *x = r.x;
    *y = r.y;
}

rect layout_text_bounds(screen_id id)
{
    rect r = layout_screen(id);
    return (rect){r.x + SCREEN_PAD, r.y + SCREEN_TITLE_H + SCREEN_PAD,
                  r.w - 2 * SCREEN_PAD, r.h - SCREEN_H(0)};
}

rect layout_resources(void)
{
    return (rect){L.goal_x, L.resource_y, SCREEN_W(GOAL_COLS), RESOURCE_H};
}

rect layout_room_button(room_button button)
{
    /* Two rows on the floor. A third row would sit on the HUD and fail the walk test. */
    int cols = (ROOM_BUTTON_COUNT + 1) / 2;
    int gap = 4;
    int width = (CANVAS_W - L.shell_x - 24 - (cols - 1) * gap) / cols;
    if (width > 116) width = 116;
    if (width < 8) width = 8;
    int x = CANVAS_W - 24 - cols * width - (cols - 1) * gap;
    return (rect){x + (button % cols) * (width + gap),
                  HUD_Y - 48 + (button / cols) * 24, width, 20};
}

void layout_room_panes(rect panes[ROOM_PANE_COUNT])
{
    for (int i = 0; i < SCREEN_COUNT; i++) {
        rect r = layout_screen((screen_id)i);
        panes[i] = (rect){r.x - SCREEN_BEZEL, r.y - SCREEN_BEZEL,
                          r.w + 2 * SCREEN_BEZEL, r.h + 2 * SCREEN_BEZEL};
    }
    panes[SCREEN_COUNT] = layout_resources();
}
