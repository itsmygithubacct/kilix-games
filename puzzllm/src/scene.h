/* scene.h - the room's rules: where Kilix can walk, which screen is near,
 * the door, and moving on to the next room. No drawing, no I/O.
 */
#ifndef PUZZLLM_SCENE_H
#define PUZZLLM_SCENE_H

#include "layout.h"

#include <stdbool.h>

typedef enum focus_id {
    FOCUS_ROOM = 0, FOCUS_OMP, FOCUS_SHELL
} focus_id;

typedef struct scene {
    int level;           /* index into the level list */
    int level_count;
    float x, y;          /* Kilix's feet */
    int dir;             /* 0 down, 1 up, 2 left, 3 right */
    int step_ticks;      /* ticks walked since standing still; 0 standing */
    int idle_ticks;      /* free-running, for the standing bob */
    bool moving;
    bool goal_met;
    float door;          /* 0 closed .. 1 open */
    focus_id focus;
} scene;

typedef enum scene_event {
    SCENE_NONE = 0,
    SCENE_DOOR_OPENED,   /* the door finished opening this tick */
    SCENE_NEXT_ROOM,     /* Kilix walked through the open door */
    SCENE_BOTTOM_EXIT
} scene_event;

/* Zell's clock and pace: 60 ticks a second, 3.5 pixels a tick walking. */
#define SCENE_TICK_HZ 60
#define SCENE_WALK_SPEED 3.5f
/* The door opens (or closes) over this many ticks. */
#define SCENE_DOOR_TICKS 75

void scene_init(scene *s, int level, int level_count);
/* Put Kilix back at the room's entrance with the door shut. */
void scene_enter_room(scene *s, int level);
/* One tick; dx, dy in -1..1 are the held direction keys. */
scene_event scene_tick(scene *s, int dx, int dy);
/* After layout_set: keep Kilix where he was relative to the floor and the
 * aisle, or put him at the entrance if that spot is gone. old is the
 * layout before the change. */
void scene_relayout(scene *s, const layout *old);
bool scene_walkable(const scene *s, float x, float y);
/* The screen Kilix is next to and facing, or SCREEN_NONE. */
screen_id scene_near_screen(const scene *s);
room_button scene_room_button_at(float x, float y);
/* True once the door is open enough to walk through. */
bool scene_door_passable(const scene *s);

#endif
