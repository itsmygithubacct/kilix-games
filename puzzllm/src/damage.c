/* damage.c - see damage.h. */
#include "damage.h"

#include <string.h>

static int tile_changed(const uint32_t *prev, const uint32_t *cur, int w,
                        int x0, int y0, int x1, int y1)
{
    size_t bytes = (size_t)(x1 - x0) * sizeof *cur;
    for (int y = y0; y < y1; y++) {
        size_t at = (size_t)y * (size_t)w + (size_t)x0;
        if (memcmp(prev + at, cur + at, bytes) != 0) return 1;
    }
    return 0;
}

static void copy_rect(uint32_t *prev, const uint32_t *cur, int w, damage_rect r)
{
    size_t bytes = (size_t)(r.x1 - r.x0) * sizeof *cur;
    for (int y = r.y0; y < r.y1; y++) {
        size_t at = (size_t)y * (size_t)w + (size_t)r.x0;
        memcpy(prev + at, cur + at, bytes);
    }
}

int damage_update(uint32_t *prev, const uint32_t *cur, int w, int h,
                  damage_rect *out, int max)
{
    int n = 0, overflow = 0;
    damage_rect all = {w, h, 0, 0};

    if (prev == NULL || cur == NULL || out == NULL || w <= 0 || h <= 0 || max <= 0)
        return 0;
    for (int ty = 0; ty < h; ty += DAMAGE_TILE_H) {
        int y1 = ty + DAMAGE_TILE_H < h ? ty + DAMAGE_TILE_H : h;
        int run = -1, tiles = (w + DAMAGE_TILE_W - 1) / DAMAGE_TILE_W;
        /* one step past the last tile closes a run that reaches the edge */
        for (int t = 0; t <= tiles; t++) {
            int tx = t * DAMAGE_TILE_W;
            int x1 = tx + DAMAGE_TILE_W < w ? tx + DAMAGE_TILE_W : w;
            int changed = t < tiles && tile_changed(prev, cur, w, tx, ty, x1, y1);
            if (changed && run < 0) run = tx;
            if (!changed && run >= 0) {
                damage_rect r = {run, ty, tx < w ? tx : w, y1};
                int merged = 0;
                /* the same columns just above: grow that one down */
                for (int i = n - 1; i >= 0 && !overflow; i--) {
                    if (out[i].x0 == r.x0 && out[i].x1 == r.x1 && out[i].y1 == ty) {
                        out[i].y1 = y1;
                        merged = 1;
                        break;
                    }
                }
                if (!merged) {
                    if (n < max) out[n++] = r;
                    else overflow = 1;
                }
                if (r.x0 < all.x0) all.x0 = r.x0;
                if (r.y0 < all.y0) all.y0 = r.y0;
                if (r.x1 > all.x1) all.x1 = r.x1;
                if (r.y1 > all.y1) all.y1 = r.y1;
                copy_rect(prev, cur, w, r);
                run = -1;
            }
        }
    }
    if (overflow) {
        out[0] = all;
        return 1;
    }
    return n;
}
