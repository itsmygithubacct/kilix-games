/* art.h - every picture in the game, and nothing else.
 *
 * Room and chest sprites use PNG assets. Screen bezels and status lights
 * use drawing primitives, and Kilix uses Zell's desktop cast.
 */
#ifndef PUZZLLM_ART_H
#define PUZZLLM_ART_H

#include "layout.h"
#include "soft_raster.h"

#include <stdbool.h>

typedef struct art_sprite {
    uint32_t *pixels;
    int w, h;
} art_sprite;

typedef struct chest_art {
    art_sprite closed, open, key;
} chest_art;

typedef struct room_art {
    art_sprite stone, brick, floor, door, passage;
    unsigned revision;
} room_art;

void art_load_room(room_art *art, const char *root);
void art_free_room(room_art *art);
void art_load_chest(chest_art *art, const char *root);
void art_free_chest(chest_art *art);
/* x is the left edge and y is the base of both chest frames. */
void art_chest(sr_canvas *c, const chest_art *art, int x, int y, bool open);
/* Floor, walls and the aisle runner. style is the level's "room:" value. */
void art_room(sr_canvas *c, const room_art *art, const char *style);
/* The door at the top of the aisle. open: 0 shut .. 1 fully open.
 * next_room is the number on its sign, 0 when there is no next room yet. */
void art_door(sr_canvas *c, const room_art *art, const char *style, float open, int next_room);
/* A screen's unchanging parts: shadow, glass, title bar, and its text area
 * filled with background. Drawn once into the cached room. */
void art_screen_glass(sr_canvas *c, rect r, uint32_t background);
/* What changes with focus: the bezel ring, the title and the power light. */
void art_screen(sr_canvas *c, rect r, const char *title, bool focused, bool near);
/* Kilix with feet at (x, y). dir: 0 down, 1 up, 2 left, 3 right;
 * step_ticks and idle_ticks pick the walk frame and the standing bob. */
void art_kilix(sr_canvas *c, float x, float y, int dir, int step_ticks, int idle_ticks);
/* How far above his feet Kilix's head is, for prompts over him. */
enum { ART_KILIX_HEIGHT = 84 };
/* A small speech-bubble style prompt above Kilix ("Enter: use omp"). */
void art_prompt(sr_canvas *c, float x, float y, const char *text);

#endif
