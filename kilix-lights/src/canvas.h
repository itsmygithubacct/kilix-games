/* canvas.h — the 0xAARRGGBB pixel buffer the whole UI draws into.
 *
 * Deliberately vendor-free so term.h stays vendor-free: only src/draw.c
 * bridges this buffer to soft-raster (via sr_canvas_wrap), and only
 * src/term.c and src/input.c touch the kitty_* APIs.
 */
#ifndef KILIX_LIGHTS_CANVAS_H
#define KILIX_LIGHTS_CANVAS_H

#include "types.h"

typedef struct Canvas {
    uint32_t *px;  /* row-major 0xAARRGGBB, px[y * w + x] */
    int w;
    int h;
} Canvas;

/* Allocates w*h pixels zeroed to opaque black. False on bad size or ENOMEM. */
bool canvas_init(Canvas *c, int w, int h);
void canvas_free(Canvas *c);

#endif
