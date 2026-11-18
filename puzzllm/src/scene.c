/* scene.c - see scene.h. */
#include "scene.h"

#include <math.h>

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

void scene_enter_room(scene *s, int level)
{
    s->level = level;
    s->x = START_X;
    s->y = START_Y;
    s->dir = 1;
    s->walk_phase = 0;
    s->moving = false;
    s->goal_met = false;
    s->door = 0;
    s->focus = FOCUS_ROOM;
}

void scene_init(scene *s, int level, int level_count)
{
    s->level_count = level_count;
    scene_enter_room(s, level);
}

bool scene_door_passable(const scene *s)
{
    return s->door >= 0.95f;
}

bool scene_walkable(const scene *s, float x, float y)
{
    if (x >= WALK_X0 && x <= WALK_X1 && y >= WALK_Y0 && y <= WALK_Y1) return true;
    if (x >= AISLE_X0 && x <= AISLE_X1 && y <= WALK_Y1) {
        if (y >= AISLE_Y0) return true;
        /* past the threshold only through an open door */
        if (scene_door_passable(s) && y >= DOOR_EXIT_Y - 4) return true;
    }
    return false;
}

scene_event scene_step(scene *s, int dx, int dy, float dt)
{
    scene_event ev = SCENE_NONE;
    float was_door = s->door;

    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;   /* a stall must not teleport Kilix */

    if (s->goal_met && s->door < 1.0f) {
        s->door += dt * 0.8f;
        if (s->door > 1.0f) s->door = 1.0f;
    } else if (!s->goal_met && s->door > 0.0f) {
        s->door -= dt * 0.8f;
        if (s->door < 0.0f) s->door = 0.0f;
    }
    if (was_door < 1.0f && s->door >= 1.0f) ev = SCENE_DOOR_OPENED;

    s->moving = false;
    if (s->focus == FOCUS_ROOM && (dx != 0 || dy != 0)) {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float step = WALK_SPEED * dt / len;
        float nx = s->x + dx * step;
        float ny = s->y + dy * step;

        if (dy > 0) s->dir = 0;
        else if (dy < 0) s->dir = 1;
        if (dx < 0 && dy == 0) s->dir = 2;
        else if (dx > 0 && dy == 0) s->dir = 3;

        /* slide along walls: try both axes, then each one */
        if (scene_walkable(s, nx, ny)) {
            s->x = nx; s->y = ny; s->moving = true;
        } else if (dx != 0 && scene_walkable(s, nx, s->y)) {
            s->x = nx; s->moving = true;
        } else if (dy != 0 && scene_walkable(s, s->x, ny)) {
            s->y = ny; s->moving = true;
        }
        if (s->moving) s->walk_phase += dt * 6.0f;
    }

    if (scene_door_passable(s) && s->y < DOOR_EXIT_Y &&
        s->level + 1 < s->level_count)
        return SCENE_NEXT_ROOM;
    return ev;
}

static float rect_distance(rect r, float x, float y)
{
    float cx = x < r.x ? r.x : x > r.x + r.w ? r.x + r.w : x;
    float cy = y < r.y ? r.y : y > r.y + r.h ? r.y + r.h : y;
    return sqrtf((cx - x) * (cx - x) + (cy - y) * (cy - y));
}

screen_id scene_near_screen(const scene *s)
{
    screen_id best = SCREEN_NONE;
    float best_d = 1e9f;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        rect r = layout_screen((screen_id)i);
        float d = rect_distance(r, s->x, s->y);
        float cx = r.x + r.w / 2.0f;
        float cy = r.y + r.h / 2.0f;
        /* facing it counts for a little */
        if ((s->dir == 2 && cx < s->x) || (s->dir == 3 && cx > s->x) ||
            (s->dir == 1 && cy < s->y))
            d -= 24.0f;
        if (d < best_d) {
            best_d = d;
            best = (screen_id)i;
        }
    }
    return best_d <= 72.0f ? best : SCREEN_NONE;
}
