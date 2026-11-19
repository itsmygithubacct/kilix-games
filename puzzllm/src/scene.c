/* scene.c - see scene.h. */
#include "scene.h"

#include <math.h>
#include <stddef.h>

void scene_enter_room(scene *s, int level)
{
    s->level = level;
    s->x = START_X;
    s->y = START_Y;
    s->dir = 1;
    s->step_ticks = 0;
    s->moving = false;
    s->goal_met = false;
    s->door = 0;
    s->focus = FOCUS_ROOM;
}

void scene_init(scene *s, int level, int level_count)
{
    if (L.w == 0) layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    s->idle_ticks = 0;
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

scene_event scene_tick(scene *s, int dx, int dy)
{
    scene_event ev = SCENE_NONE;
    float was_door = s->door;
    const float door_step = 1.0f / SCENE_DOOR_TICKS;

    s->idle_ticks++;
    /* nudged by half a step so float sums land on 0 and 1 exactly */
    if (s->goal_met && s->door < 1.0f) {
        s->door += door_step;
        if (s->door > 1.0f - door_step / 2) s->door = 1.0f;
    } else if (!s->goal_met && s->door > 0.0f) {
        s->door -= door_step;
        if (s->door < door_step / 2) s->door = 0.0f;
    }
    if (was_door < 1.0f && s->door >= 1.0f) ev = SCENE_DOOR_OPENED;

    s->moving = false;
    if (s->focus == FOCUS_ROOM && (dx != 0 || dy != 0)) {
        float len = sqrtf((float)(dx * dx + dy * dy));
        float nx = s->x + (float)dx / len * SCENE_WALK_SPEED;
        float ny = s->y + (float)dy / len * SCENE_WALK_SPEED;

        /* Zell's facing: sideways wins over up and down */
        s->dir = dx < 0 ? 2 : dx > 0 ? 3 : dy < 0 ? 1 : 0;
        /* slide along walls: try both axes, then each one */
        if (scene_walkable(s, nx, ny)) {
            s->x = nx; s->y = ny; s->moving = true;
        } else if (dx != 0 && scene_walkable(s, nx, s->y)) {
            s->x = nx; s->moving = true;
        } else if (dy != 0 && scene_walkable(s, s->x, ny)) {
            s->y = ny; s->moving = true;
        }
    }
    if (s->moving) s->step_ticks++;
    else s->step_ticks = 0;

    if (scene_door_passable(s) && s->y < DOOR_EXIT_Y &&
        s->level + 1 < s->level_count)
        return SCENE_NEXT_ROOM;
    return ev;
}

void scene_relayout(scene *s, const layout *old)
{
    float x = s->x, y = s->y;
    if (old != NULL && old->w > 0) {
        /* keep the offset from the aisle's centre line */
        x = (float)(L.w / 2) + (s->x - (float)(old->w / 2));
        /* on the floor, keep the height above the bottom; in the aisle the
         * top of the room has not moved */
        if (s->y >= (float)old->walk_y0) y = (float)L.h - ((float)old->h - s->y);
        else if (s->y >= (float)old->floor_y) y = (float)L.walk_y0;
    }
    if (scene_walkable(s, x, y)) {
        s->x = x;
        s->y = y;
    } else {
        s->x = (float)START_X;
        s->y = (float)START_Y;
    }
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
        if (i == SCREEN_ACT) continue;
        if (i == SCREEN_OMP && s->y >= FLOOR_Y && s->x >= AISLE_X0) continue;
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
