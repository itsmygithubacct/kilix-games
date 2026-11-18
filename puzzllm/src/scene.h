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
    float walk_phase;
    bool moving;
    bool goal_met;
    float door;          /* 0 closed .. 1 open */
    focus_id focus;
} scene;

typedef enum scene_event {
    SCENE_NONE = 0,
    SCENE_DOOR_OPENED,   /* the door finished opening this step */
    SCENE_NEXT_ROOM      /* Kilix walked through the open door */
} scene_event;

enum { WALK_SPEED = 220 };   /* pixels per second */

void scene_init(scene *s, int level, int level_count);
/* Put Kilix back at the room's entrance with the door shut. */
void scene_enter_room(scene *s, int level);
/* dx, dy in -1..1; dt in seconds. */
scene_event scene_step(scene *s, int dx, int dy, float dt);
bool scene_walkable(const scene *s, float x, float y);
/* The screen Kilix is next to and facing, or SCREEN_NONE. */
screen_id scene_near_screen(const scene *s);
/* True once the door is open enough to walk through. */
bool scene_door_passable(const scene *s);

#endif
