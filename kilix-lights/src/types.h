/* types.h - shared canvas geometry and integer helpers. */
#ifndef KILIX_LIGHTS_TYPES_H
#define KILIX_LIGHTS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CANVAS_W = 640,
    CANVAS_H = 400,
    MAX_BOARD = 7,
    MAX_CELLS = MAX_BOARD * MAX_BOARD
};

static inline int imini(int a, int b) { return a < b ? a : b; }
static inline int imaxi(int a, int b) { return a > b ? a : b; }
static inline int iclampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

#endif
