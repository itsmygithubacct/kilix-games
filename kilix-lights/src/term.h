/* term.h — terminal session lifecycle, integer upscaler, present, resize.
 *
 * Frame cadence lives in main.c but the present path is here; headless
 * subcommands never call term_init().
 *
 * The implementation consumes the ordered event stream supplied by
 * kitty-terminal-session. This header is vendor-free.
 *
 * Present path: 640x400 canvas -> integer upscale
 * scale = min(fbw/640, fbh/400) clamped >= 1, centered with off_x/off_y,
 * black letterbox -> kittyts_present. Framebuffer minima include one large
 * cell of snap headroom so the logical canvas normally lands in scale 2..5;
 * mouse/focus modes are owned by the session lifecycle;
 * KILIX_LIGHTS_SKIP_PROBE skips the graphics probe for controlled tests.
 */
#ifndef KILIX_LIGHTS_TERM_H
#define KILIX_LIGHTS_TERM_H

#include "canvas.h"

/* Start the kitty session (framebuffer probe -> raw + altscreen + mouse on
 * -> keyboard protocol push), mirror geometry into input.h, allocate the
 * present buffer. Returns 0, or -1 with errno set (ENOTSUP = not a
 * kitty-graphics terminal: print the friendly message and exit 1). */
int  term_init(void);

/* Normal teardown (idempotent; also registered via atexit). */
void term_shutdown(void);

/* Async-signal-safe restore for SIGINT/TERM/HUP/SEGV/BUS/FPE/ABRT handlers:
 * pops keyboard mode, mouse off, framebuffer emergency restore. Handlers
 * call this then _exit(128+sig). */
void term_emergency_restore(void);

/* Upscale + present the 640x400 canvas. Returns false once the presenter
 * has latched a failure (kittyfb_failed) — exit the main loop on that. */
bool term_present_canvas(const Canvas *c);

/* Cheap per-frame resize poll. On true, the present buffer has been
 * reallocated, scale/offsets recomputed, and input_set_geometry re-fed. */
bool term_check_resize(void);

/* Drain stdin through kitty-input and dispatch ordered keyboard, mouse, and
 * focus events. Call once per frame. Returns -1 if the fd died. */
int  term_read_input(void);

/* Headless parser/translation check for produced shifted ASCII text. */
bool term_translation_test(void);

/* Held-key state (kitty protocol only). term_key_down is only trustworthy
 * when term_has_release_events(); callers gate continuous actions (map
 * scroll) on that, falling back to press/repeat nudges. */
bool term_key_down(uint32_t key);
bool term_has_release_events(void);

/* Current upscale placement (debug/tests; input.c gets these via
 * input_set_geometry, not by polling here). */
int  term_scale(void);
void term_offsets(int *off_x, int *off_y);

#endif
