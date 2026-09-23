/* input.h — key + mouse event queue in 640x400 canvas coordinates.
 *
 * kitty-input owns terminal sequence parsing and wire ordering. term.c maps
 * its typed events into this game's small canvas-space event model; UI code
 * consumes only input_next()/term_key_down(). This header stays vendor-free.
 *
 * Coarse-input note: terminals that ignore SGR-pixel
 * mode (1016) report cell-granular positions, so pointing precision is
 * +/- half a cell. Conventional controls retain 32x32 canvas-pixel targets.
 * Physical room objects deliberately use only their painted silhouettes, so
 * they are kept visually large enough to point at without target dilation.
 */
#ifndef KILIX_LIGHTS_INPUT_H
#define KILIX_LIGHTS_INPUT_H

#include "types.h"

/* ---- Event model ---- */
typedef enum input_kind {
    IN_NONE = 0,
    IN_KEY_DOWN, IN_KEY_REPEAT, IN_KEY_UP,   /* key = KEY_x or unicode, mods */
    IN_MOUSE_MOVE,                           /* mx,my updated; button=held|3 */
    IN_MOUSE_DOWN, IN_MOUSE_UP,              /* button 0=L 1=M 2=R           */
    IN_MOUSE_WHEEL,                          /* wheel = +1 up / -1 down      */
    IN_MOUSE_LEAVE                           /* pointer left the window      */
} input_kind;

typedef struct input_event {
    input_kind kind;
    uint32_t   key;      /* kitty key value (unicode scalar or KEY_* below) */
    uint32_t   mods;     /* MOD_* bitset (keys); shift/alt/ctrl (mouse)     */
    uint8_t    button;
    int8_t     wheel;
    int16_t    mx, my;   /* 640x400 canvas coords, clamped; valid on mouse evs */
    bool       in_view;  /* false when clamped from outside the viewport    */
} input_event;

/* ---- Key/mod constants ----
 * Values mirror the vendored kitty_keyboard.h canonical numbers (PUA plane);
 * src/input.c _Static_asserts them against the vendor header. Ordinary keys
 * are lowercase Unicode scalars ('a', '1', '+'). Bindings should match via
 * ev->key so the base-layout fallback handled in term.c keeps non-QWERTY
 * working.
 */
enum {
    KEY_ESCAPE    = 0xe000,
    KEY_ENTER     = 0xe001,
    KEY_TAB       = 0xe002,
    KEY_BACKSPACE = 0xe003,
    KEY_LEFT      = 0xe006,
    KEY_RIGHT     = 0xe007,
    KEY_UP        = 0xe008,
    KEY_DOWN      = 0xe009,
    KEY_PAGE_UP   = 0xe00a,
    KEY_PAGE_DOWN = 0xe00b,
    KEY_HOME      = 0xe00c,
    KEY_END       = 0xe00d
};
enum {
    MOD_SHIFT = 1u << 0,
    MOD_ALT   = 1u << 1,
    MOD_CTRL  = 1u << 2
};

/* ---- Consumer API (game code / main loop) ---- */
bool input_next(input_event *ev);            /* pop merged FIFO; false=empty */
void input_mouse_pos(int *gx, int *gy, bool *in_view);  /* latest, polled    */

/* ---- Producer API (called by term.c only) ---- */

/* Reset the queue, pointer position, and geometry.
 * term.c calls this before every terminal-session start; headless parser tests
 * use it to make fixtures independent. Geometry is reset to safe 1px cells. */
void input_reset(void);

/* Push one translated keyboard event (action: 1=press 2=repeat 3=release). */
void input_push_key(uint32_t key, uint32_t mods, int action);

/* Push one decoded kitty-input mouse event. x/y are zero-based terminal cell
 * or pixel coordinates; buttons are kitty-input's one-based button numbers;
 * actions are 1=press, 2=release, 3=move, 4=wheel. */
void input_push_mouse(int32_t x, int32_t y, uint32_t mods, uint8_t button,
                      int8_t wheel_x, int8_t wheel_y, int action,
                      bool pixel_coordinates);

/* A focus loss is exposed as IN_MOUSE_LEAVE so hover state cannot remain
 * visually stuck while another window owns the pointer. */
void input_push_focus(bool focused);

/* Geometry mirror for mouse coordinate mapping.
 * term.c calls this after start and after every resize. origin_col/row are
 * 1-based cell coords of the image top-left; scale/off_x/off_y are the
 * integer-upscale placement of the 640x400 canvas inside the framebuffer. */
void input_set_geometry(int term_cols, int term_rows,
                        int cell_w, int cell_h,
                        int origin_col, int origin_row,
                        int scale, int off_x, int off_y);

/* Queue is bounded and overwrite-oldest; adjacent mouse motion is coalesced. */
enum { INPUT_EVENT_RING = 128 };

#endif
