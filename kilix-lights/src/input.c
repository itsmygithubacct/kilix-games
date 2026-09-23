/* input.c — canvas-space event queue and terminal coordinate mapping.
 *
 * kitty-input performs all terminal parsing and preserves wire order. This
 * module only translates its typed events into the game's stable input model.
 */
#include "input.h"

#include "kitty_keyboard.h"

#include <string.h>

/* KEY/MOD mirror (input.h promises these track kitty-keyboard). */
#define MIRROR(a, b) _Static_assert((uint32_t)(a) == (uint32_t)(b), #a " drifted")
MIRROR(KEY_ESCAPE, KITTYKB_KEY_ESCAPE);
MIRROR(KEY_ENTER, KITTYKB_KEY_ENTER);
MIRROR(KEY_TAB, KITTYKB_KEY_TAB);
MIRROR(KEY_BACKSPACE, KITTYKB_KEY_BACKSPACE);
MIRROR(KEY_LEFT, KITTYKB_KEY_LEFT);
MIRROR(KEY_RIGHT, KITTYKB_KEY_RIGHT);
MIRROR(KEY_UP, KITTYKB_KEY_UP);
MIRROR(KEY_DOWN, KITTYKB_KEY_DOWN);
MIRROR(KEY_PAGE_UP, KITTYKB_KEY_PAGE_UP);
MIRROR(KEY_PAGE_DOWN, KITTYKB_KEY_PAGE_DOWN);
MIRROR(KEY_HOME, KITTYKB_KEY_HOME);
MIRROR(KEY_END, KITTYKB_KEY_END);
MIRROR(MOD_SHIFT, KITTYKB_MOD_SHIFT);
MIRROR(MOD_ALT, KITTYKB_MOD_ALT);
MIRROR(MOD_CTRL, KITTYKB_MOD_CTRL);
#undef MIRROR

static struct {
    input_event queue[INPUT_EVENT_RING];
    size_t head;
    size_t count;

    int cell_w, cell_h;
    int origin_col, origin_row;
    int scale, off_x, off_y;

    int16_t last_mx, last_my;
    bool last_in_view;
} in;

static void push(const input_event *event)
{
    input_event *slot;

    if (event->kind == IN_MOUSE_MOVE && in.count > 0u) {
        slot = &in.queue[(in.head + in.count - 1u) % INPUT_EVENT_RING];
        if (slot->kind == IN_MOUSE_MOVE) {
            *slot = *event;
            return;
        }
    }
    if (in.count == INPUT_EVENT_RING) {
        in.head = (in.head + 1u) % INPUT_EVENT_RING;
        --in.count;
    }
    slot = &in.queue[(in.head + in.count) % INPUT_EVENT_RING];
    *slot = *event;
    ++in.count;
}

/* kitty-input has already converted both pixel and cell reports to zero-based
 * terminal coordinates. Cell reports map to the center of their cell. */
static void map_coords(int32_t x, int32_t y, bool pixels,
                       int *gx, int *gy, bool *view)
{
    const int sc = in.scale > 0 ? in.scale : 1;
    int64_t fx, fy;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (pixels) {
        fx = (int64_t)x - (int64_t)(in.origin_col - 1) * in.cell_w;
        fy = (int64_t)y - (int64_t)(in.origin_row - 1) * in.cell_h;
    } else {
        fx = ((int64_t)x - (in.origin_col - 1)) * in.cell_w + in.cell_w / 2;
        fy = ((int64_t)y - (in.origin_row - 1)) * in.cell_h + in.cell_h / 2;
    }
    left = in.off_x;
    top = in.off_y;
    right = left + (int64_t)CANVAS_W * sc;
    bottom = top + (int64_t)CANVAS_H * sc;
    *view = fx >= left && fx < right && fy >= top && fy < bottom;
    *gx = fx < left ? 0 : (fx >= right ? CANVAS_W - 1 :
          (int)((fx - left) / sc));
    *gy = fy < top ? 0 : (fy >= bottom ? CANVAS_H - 1 :
          (int)((fy - top) / sc));
}

void input_reset(void)
{
    memset(&in, 0, sizeof in);
    in.cell_w = 1;
    in.cell_h = 1;
    in.origin_col = 1;
    in.origin_row = 1;
    in.scale = 1;
}

void input_push_key(uint32_t key, uint32_t mods, int action)
{
    input_event event = {0};

    event.kind = action == 3 ? IN_KEY_UP :
                 action == 2 ? IN_KEY_REPEAT : IN_KEY_DOWN;
    event.key = key;
    event.mods = mods;
    event.mx = in.last_mx;
    event.my = in.last_my;
    event.in_view = in.last_in_view;
    push(&event);
}

void input_push_mouse(int32_t x, int32_t y, uint32_t mods, uint8_t button,
                      int8_t wheel_x, int8_t wheel_y, int action,
                      bool pixel_coordinates)
{
    input_event event = {0};
    int gx, gy;
    bool view;

    map_coords(x, y, pixel_coordinates, &gx, &gy, &view);
    in.last_mx = (int16_t)gx;
    in.last_my = (int16_t)gy;
    in.last_in_view = view;
    event.mods = mods;
    event.mx = (int16_t)gx;
    event.my = (int16_t)gy;
    event.in_view = view;

    if (action == 4) {
        if (wheel_y == 0 || wheel_x != 0) return;
        event.kind = IN_MOUSE_WHEEL;
        event.wheel = (int8_t)-wheel_y;
    } else if (action == 3) {
        event.kind = IN_MOUSE_MOVE;
        event.button = button >= 1u && button <= 3u ?
                       (uint8_t)(button - 1u) : 3u;
    } else {
        if (button < 1u || button > 3u) return;
        event.kind = action == 2 ? IN_MOUSE_UP : IN_MOUSE_DOWN;
        event.button = (uint8_t)(button - 1u);
    }
    push(&event);
}

void input_push_focus(bool focused)
{
    input_event event = {0};

    if (focused) return;
    event.kind = IN_MOUSE_LEAVE;
    event.mx = in.last_mx;
    event.my = in.last_my;
    event.in_view = false;
    in.last_in_view = false;
    push(&event);
}

void input_set_geometry(int term_cols, int term_rows,
                        int cell_w, int cell_h,
                        int origin_col, int origin_row,
                        int scale, int off_x, int off_y)
{
    (void)term_cols;
    (void)term_rows;
    in.cell_w = cell_w > 0 ? cell_w : 1;
    in.cell_h = cell_h > 0 ? cell_h : 1;
    in.origin_col = origin_col > 0 ? origin_col : 1;
    in.origin_row = origin_row > 0 ? origin_row : 1;
    in.scale = scale > 0 ? scale : 1;
    in.off_x = off_x;
    in.off_y = off_y;
}

bool input_next(input_event *event)
{
    if (event == NULL || in.count == 0u) return false;
    *event = in.queue[in.head];
    in.head = (in.head + 1u) % INPUT_EVENT_RING;
    --in.count;
    return true;
}

void input_mouse_pos(int *gx, int *gy, bool *in_view)
{
    if (gx != NULL) *gx = in.last_mx;
    if (gy != NULL) *gy = in.last_my;
    if (in_view != NULL) *in_view = in.last_in_view;
}
