/* termio.h - the outer terminal: Kitty graphics out, Kitty keyboard and
 * pixel mouse in. Adapted from kilix-lights' src/term.c (same monorepo).
 */
#ifndef PUZZLLM_TERMIO_H
#define PUZZLLM_TERMIO_H

#include "keys.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct termio_handlers {
    void (*key)(void *ctx, const key_event *ev);
    void (*pointer)(void *ctx, int canvas_x, int canvas_y, pointer_action action);
    void (*scroll)(void *ctx, int canvas_x, int canvas_y, int rows);
    void (*paste)(void *ctx, const unsigned char *text, size_t len, bool final);
    void *ctx;
} termio_handlers;

/* 0 on success; -1 with errno (ENOTSUP: not a Kitty-graphics terminal). */
int termio_start(void);
void termio_stop(void);
void termio_emergency_restore(void);
bool termio_release_events(void);
/* Present a w x h 0xAARRGGBB canvas: 1:1 and centred when it fits,
 * shrunk to fit when it does not. */
bool termio_present(const uint32_t *px, int w, int h);
void termio_check_resize(void);
/* The framebuffer's size in pixels. */
void termio_size(int *w, int *h);
/* One line about the session: sizes, scale, transport (for PUZZLLM_STATS). */
const char *termio_describe(void);
/* Drain input and dispatch it. -1 when stdin is gone. */
void termio_copy(const char *text, bool primary);
int termio_read(const termio_handlers *h);

#endif
