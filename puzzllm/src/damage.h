/* damage.h - what changed between two frames, as a few rectangles.
 *
 * Most of the room stays still from one frame to the next: Kilix moves,
 * a cursor blinks, a terminal line changes. Sending only those parts keeps
 * the terminal from re-reading a full frame sixty times a second.
 */
#ifndef PUZZLLM_DAMAGE_H
#define PUZZLLM_DAMAGE_H

#include <stdint.h>

enum { DAMAGE_TILE_W = 64, DAMAGE_TILE_H = 32, DAMAGE_MAX_RECTS = 64 };

/* x1 and y1 are exclusive. */
typedef struct damage_rect { int x0, y0, x1, y1; } damage_rect;

/* Compare cur with prev (both w x h, row-major), copy what changed into
 * prev, and write at most max rectangles to out that together cover every
 * changed pixel. Returns how many; 0 when nothing changed. Tiles of
 * DAMAGE_TILE_W x DAMAGE_TILE_H are the unit; runs of changed tiles become
 * rectangles, and runs with the same columns in touching rows merge. When
 * that still needs more than max, one rectangle around all of it is
 * returned. */
int damage_update(uint32_t *prev, const uint32_t *cur, int w, int h,
                  damage_rect *out, int max);

#endif
