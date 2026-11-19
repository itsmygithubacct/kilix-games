/* termio.c - see termio.h. */
#include "termio.h"

#include "damage.h"
#include "kitty_terminal_session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert((int)KEY_ESCAPE == (int)KITTYKB_KEY_ESCAPE, "key numbers");
_Static_assert((int)KEY_ENTER == (int)KITTYKB_KEY_ENTER, "key numbers");
_Static_assert((int)KEY_DELETE == (int)KITTYKB_KEY_DELETE, "key numbers");
_Static_assert((int)KEY_END == (int)KITTYKB_KEY_END, "key numbers");
_Static_assert((int)KEY_F1 == (int)KITTYKB_KEY_F1, "key numbers");
_Static_assert((int)KEY_F12 == (int)KITTYKB_KEY_F12, "key numbers");
_Static_assert((int)MOD_CTRL == (int)KITTYKB_MOD_CTRL, "modifier numbers");

static kittyts_session session;
static kittyts_options options;
static bool active;
static uint8_t *frame;
static int fb_w, fb_h;
/* canvas -> framebuffer placement: fb = off + canvas * num / den */
static int num = 1, den = 1, off_x, off_y;
static int canvas_w = 1, canvas_h = 1;

/* The last canvas presented 1:1, to find what changed since. */
static uint32_t *shown;
static size_t shown_cap;
static bool shown_valid;
static unsigned long presents_full, presents_patched, presents_skipped;

static bool alloc_frame(int w, int h)
{
    shown_valid = false;
    free(frame);
    frame = calloc((size_t)w * (size_t)h, 4);
    fb_w = w;
    fb_h = h;
    return frame != NULL;
}

static void place(void)
{
    /* Integer scale when it fits at 1x or more; otherwise shrink by an
     * exact fraction so text stays as legible as the pane allows. */
    if (fb_w >= canvas_w && fb_h >= canvas_h) {
        int s = fb_w / canvas_w < fb_h / canvas_h ? fb_w / canvas_w : fb_h / canvas_h;
        num = s;
        den = 1;
    } else {
        den = 1024;
        num = fb_w * 1024 / canvas_w < fb_h * 1024 / canvas_h ?
              fb_w * 1024 / canvas_w : fb_h * 1024 / canvas_h;
        if (num < 1) num = 1;
    }
    off_x = (fb_w - canvas_w * num / den) / 2;
    off_y = (fb_h - canvas_h * num / den) / 2;
}

int termio_start(void)
{
    kittyts_session_init(&session);
    kittyts_options_init(&options);
    options.keyboard.flags = KITTYKB_FLAGS_DEFAULT;   /* with key text */
    options.framebuffer.max_width = 2560;
    options.framebuffer.max_height = 1600;
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_BUTTON;
    options.pixel_mouse = true;
    options.focus_events = false;
    options.bracketed_paste = false;
    if (getenv("PUZZLLM_SKIP_PROBE")) options.framebuffer.probe_graphics = false;
    if (kittyts_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return -1;
    if (!alloc_frame(kittyts_width(&session), kittyts_height(&session))) {
        kittyts_stop(&session);
        errno = ENOMEM;
        return -1;
    }
    active = true;
    return 0;
}

void termio_stop(void)
{
    kittyts_stop(&session);
    active = false;
    free(frame);
    frame = NULL;
    free(shown);
    shown = NULL;
    shown_cap = 0;
    shown_valid = false;
}

void termio_emergency_restore(void)
{
    kittyts_emergency_restore(&session);
}

bool termio_release_events(void)
{
    return active && kittyts_has_release_events(&session);
}

static void to_rgba(const uint32_t *px, int w, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        const uint32_t *src = px + (size_t)y * (size_t)w;
        uint8_t *dst = frame + ((size_t)(off_y + y) * (size_t)fb_w + (size_t)(off_x + x0)) * 4;
        for (int x = x0; x < x1; x++) {
            uint32_t c = src[x];
            dst[0] = (uint8_t)(c >> 16);
            dst[1] = (uint8_t)(c >> 8);
            dst[2] = (uint8_t)c;
            dst[3] = 255;
            dst += 4;
        }
    }
}

bool termio_present(const uint32_t *px, int w, int h)
{
    if (!active || frame == NULL) return false;
    if (w != canvas_w || h != canvas_h) {
        canvas_w = w;
        canvas_h = h;
        shown_valid = false;
        place();
    }
    if (num == den) {
        /* 1:1: send only what changed since the last frame */
        size_t need = (size_t)w * (size_t)h;
        damage_rect d[DAMAGE_MAX_RECTS];
        kittyfb_rect r[DAMAGE_MAX_RECTS];
        int n;
        if (shown_cap < need) {
            uint32_t *grown = realloc(shown, need * sizeof *shown);
            if (grown == NULL) return false;
            shown = grown;
            shown_cap = need;
            shown_valid = false;
        }
        if (!shown_valid) {
            memcpy(shown, px, need * sizeof *px);
            to_rgba(px, w, 0, 0, w, h);
            shown_valid = true;
            presents_full++;
            return kittyts_present(&session, frame, fb_w, fb_h) && !kittyts_failed(&session);
        }
        n = damage_update(shown, px, w, h, d, DAMAGE_MAX_RECTS);
        if (n == 0) {
            presents_skipped++;
            return !kittyts_failed(&session);
        }
        for (int i = 0; i < n; i++) {
            to_rgba(px, w, d[i].x0, d[i].y0, d[i].x1, d[i].y1);
            r[i].x0 = d[i].x0 + off_x;
            r[i].y0 = d[i].y0 + off_y;
            r[i].x1 = d[i].x1 + off_x;
            r[i].y1 = d[i].y1 + off_y;
        }
        presents_patched++;
        return kittyts_present_damage(&session, frame, fb_w, fb_h, r, (size_t)n) &&
               !kittyts_failed(&session);
    }
    {
        int dw = canvas_w * num / den, dh = canvas_h * num / den;
        for (int y = 0; y < dh && off_y + y < fb_h; y++) {
            const uint32_t *src = px + (size_t)(y * den / num) * (size_t)w;
            uint8_t *dst = frame + ((size_t)(off_y + y) * (size_t)fb_w + (size_t)off_x) * 4;
            for (int x = 0; x < dw && off_x + x < fb_w; x++) {
                uint32_t c = src[x * den / num];
                dst[0] = (uint8_t)(c >> 16);
                dst[1] = (uint8_t)(c >> 8);
                dst[2] = (uint8_t)c;
                dst[3] = 255;
                dst += 4;
            }
        }
    }
    presents_full++;
    return kittyts_present(&session, frame, fb_w, fb_h) && !kittyts_failed(&session);
}

void termio_size(int *w, int *h)
{
    *w = fb_w;
    *h = fb_h;
}

const char *termio_describe(void)
{
    static char text[192];
    kittyfb_transport t = active ? kittyts_active_transport(&session) : KITTYFB_TRANSPORT_AUTO;
    snprintf(text, sizeof text, "fb %dx%d canvas %dx%d scale %d/%d %s presents full %lu "
             "patched %lu skipped %lu key-releases %s", fb_w, fb_h, canvas_w, canvas_h, num, den,
             t == KITTYFB_TRANSPORT_SHM ? "shm" : t == KITTYFB_TRANSPORT_INLINE ? "inline" : "auto",
             presents_full, presents_patched, presents_skipped,
             termio_release_events() ? "yes" : "no");
    return text;
}

void termio_check_resize(void)
{
    int nw, nh;
    if (!active) return;
    if (kittyts_check_resize(&session, &nw, &nh)) {
        (void)alloc_frame(nw, nh);
        place();
    }
}

static void dispatch_key(const termio_handlers *h, const kittykb_event *k)
{
    key_event ev;
    memset(&ev, 0, sizeof ev);
    ev.key = k->key;
    ev.mods = k->modifiers & (MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_SUPER);
    ev.action = (int)k->action;
    for (int i = 0; i < k->text_length && i < KEY_TEXT_MAX; i++)
        ev.text[ev.text_len++] = k->text[i];
    /* Layouts: with Ctrl held there is no text, so bindings use the
     * base-layout key; text keys without reported text use the shifted key. */
    if (ev.text_len == 0 && !(ev.mods & (MOD_CTRL | MOD_ALT)) &&
        (ev.mods & MOD_SHIFT) && k->shifted_key >= 0x20 && k->shifted_key < 0xe000)
        ev.text[ev.text_len++] = k->shifted_key;
    if (ev.key >= 0x80 && !(ev.key >= 0xe000 && ev.key <= 0xf8ff) &&
        k->base_layout_key != 0 && (ev.mods & MOD_CTRL))
        ev.key = k->base_layout_key;
    h->key(h->ctx, &ev);
}

int termio_read(const termio_handlers *h)
{
    kittyin_event e;
    if (!active) return 0;
    if (kittyts_read_input(&session) < 0) return -1;
    while (kittyts_next_event(&session, &e)) {
        if (e.kind == KITTYIN_EVENT_KEY) {
            dispatch_key(h, &e.data.key);
        } else if (e.kind == KITTYIN_EVENT_MOUSE) {
            const kittyin_mouse_event *m = &e.data.mouse;
            int px, py;
            bool wheel = m->action == KITTYIN_MOUSE_WHEEL && m->wheel_y != 0;
            bool click = m->action == KITTYIN_MOUSE_PRESS && m->button == 1;
            if (!wheel && !click) continue;
            if (m->pixel_coordinates) {
                px = m->x - kittyts_origin_x(&session);
                py = m->y - kittyts_origin_y(&session);
            } else {
                px = m->x * kittyts_cell_width(&session) - kittyts_origin_x(&session);
                py = m->y * kittyts_cell_height(&session) - kittyts_origin_y(&session);
            }
            if (px < off_x || py < off_y) continue;
            px = (px - off_x) * den / num;
            py = (py - off_y) * den / num;
            if (px >= canvas_w || py >= canvas_h) continue;
            if (wheel) h->scroll(h->ctx, px, py, 3 * m->wheel_y);
            else h->click(h->ctx, px, py);
        }
    }
    return 0;
}
