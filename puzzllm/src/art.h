/* art.h - every picture in the game, and nothing else.
 *
 * PLACEHOLDER ART. The room, door and screen bezels are drawn from
 * primitives; Kilix is Zell's sprite. The real graphics are to be made later:
 * replace art.c (and kilix_sprite.c) behind these functions and nothing else
 * in the game has to change. Text on the screens is drawn by render.c.
 */
#ifndef PUZZLLM_ART_H
#define PUZZLLM_ART_H

#include "layout.h"
#include "soft_raster.h"

#include <stdbool.h>

/* Floor, walls and the aisle runner. style is the level's "room:" value. */
void art_room(sr_canvas *c, const char *style);
/* The door at the top of the aisle. open: 0 shut .. 1 fully open.
 * next_room is the number on its sign, 0 when there is no next room yet. */
void art_door(sr_canvas *c, const char *style, float open, int next_room);
/* A screen's bezel and title bar; the text area inside is left to the
 * caller (filled with the screen's background colour). */
void art_screen(sr_canvas *c, rect r, const char *title, bool focused,
                bool near, uint32_t background);
/* Kilix with feet at (x, y). dir: 0 down, 1 up, 2 left, 3 right. */
void art_kilix(sr_canvas *c, float x, float y, int dir, int step);
/* A small speech-bubble style prompt above Kilix ("Enter: use omp"). */
void art_prompt(sr_canvas *c, float x, float y, const char *text);

#endif
