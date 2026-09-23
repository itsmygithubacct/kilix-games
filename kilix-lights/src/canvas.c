/* canvas.c — pixel buffer lifetime. Contract: src/canvas.h. */
#include "canvas.h"

#include <stdlib.h>

bool canvas_init(Canvas *c, int w, int h)
{
    if (c == NULL) return false;
    c->px = NULL;
    c->w = 0;
    c->h = 0;
    if (w <= 0 || h <= 0) return false;
    if (w > (1 << 15) || h > (1 << 15)) return false;

    c->px = calloc((size_t)w * (size_t)h, sizeof *c->px);
    if (c->px == NULL) return false;
    for (int i = 0, n = w * h; i < n; i++)
        c->px[i] = 0xff000000u;  /* opaque black, not transparent */
    c->w = w;
    c->h = h;
    return true;
}

void canvas_free(Canvas *c)
{
    if (c == NULL) return;
    free(c->px);
    c->px = NULL;
    c->w = 0;
    c->h = 0;
}
