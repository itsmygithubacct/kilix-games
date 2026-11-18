/* kilix_art.h - Kilix as Zell draws him: standing and walking pictures,
 * 84 pixels tall, and which one to show. See kilix_art.c for provenance. */
#ifndef PUZZLLM_KILIX_ART_H
#define PUZZLLM_KILIX_ART_H

#include <stdint.h>

enum {
    KILIX_ART_STAND = 0,    /* + dir: 0 down, 1 up, 2 left, 3 right */
    KILIX_ART_WALK = 4,     /* + dir * 4 + frame */
    KILIX_ART_COUNT = 20,
    KILIX_ART_HEIGHT = 84
};

/* Premultiplied ARGB, width x height, row-major. */
typedef struct kilix_picture { int width, height; uint32_t *pixels; } kilix_picture;

/* Unpacked on first use; NULL for a bad id or if unpacking fails. */
const kilix_picture *kilix_art_get(int id);

typedef struct kilix_pose { int art, offset_y; } kilix_pose;
/* step_ticks: ticks walked since Kilix last stood still (0 = standing);
 * idle_ticks: a free-running tick count for the standing bob. */
kilix_pose kilix_pose_at(int dir, int step_ticks, int idle_ticks);

#endif
