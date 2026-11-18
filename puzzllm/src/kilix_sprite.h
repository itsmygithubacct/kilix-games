/* kilix_sprite.h - the player sprite (see kilix_sprite.c for provenance). */
#ifndef PUZZLLM_KILIX_SPRITE_H
#define PUZZLLM_KILIX_SPRITE_H

#include <stdint.h>

enum {
    KILIX_SPRITE_W = 64,
    KILIX_SPRITE_H = 64,
    KILIX_SPRITE_FOOT = 60  /* source row the feet stand on */
};

/* direction: 0 down, 1 up, 2 left, 3 right. step is 0 or 1. */
const uint32_t *kilix_sprite_frame(int direction, int step);

#endif
