/* term.c — kitty terminal session adapter: lifecycle, integer upscale,
 * present, resize, and ordered typed input dispatch.
 *
 * Headless subcommands never call term_init; every
 * function here is a safe no-op on an unstarted session, so atexit
 * (term_shutdown) and the signal handlers main.c installs can always fire.
 */
#include "term.h"
#include "input.h"

#include "kitty_terminal_session.h"
#include "kitty_framebuffer_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static kittyts_session session;
static kittyts_options options;
static bool active;

static uint8_t *present_buf;         /* fb_w * fb_h * 4 RGBA, borders black */
static int fb_w, fb_h;
static int scale = 1, off_x, off_y;

static struct winsize last_ws;
enum { MIN_PRESENT_SCALE = 2, CELL_SNAP_HEADROOM = 64 };

static bool alloc_present_buffer(int w, int h)
{
    free(present_buf);
    present_buf = calloc((size_t)w * (size_t)h, 4);  /* zero = black border */
    fb_w = w;
    fb_h = h;
    return present_buf != NULL;
}

/* scale = min(fbw/640, fbh/400); the requested minimum normally guarantees
 * scale >= 2 and the 2400x1600 maximum caps it at 3. */
static void compute_placement(void)
{
    scale = imini(fb_w / CANVAS_W, fb_h / CANVAS_H);
    if (scale < 1) scale = 1;
    off_x = (fb_w - CANVAS_W * scale) / 2;
    off_y = (fb_h - CANVAS_H * scale) / 2;
}

/* Mirror the image placement into input.c: rerun
 * the library's own geometry derivation on the live winsize; if it ever
 * disagrees with the session (never observed), re-center from the session's
 * cell metrics instead. */
static void mirror_geometry(void)
{
    struct winsize ws;
    kittyfb_geometry g;
    int cell_w, cell_h, origin_col, origin_row;

    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 ||
        ws.ws_row == 0) {
        ws.ws_col = 80;
        ws.ws_row = 24;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
    }
    last_ws = ws;
    if (kittyfb_derive_geometry(ws.ws_col, ws.ws_row, ws.ws_xpixel,
                                ws.ws_ypixel, &options.framebuffer, &g) &&
        g.width == fb_w && g.height == fb_h) {
        cell_w = g.cell_width;
        cell_h = g.cell_height;
        origin_col = g.origin_column;
        origin_row = g.origin_row;
    } else {
        cell_w = kittyts_cell_width(&session);
        cell_h = kittyts_cell_height(&session);
        if (cell_w < 1) cell_w = 9;
        if (cell_h < 1) cell_h = 18;
        origin_col = imaxi(1, 1 + (ws.ws_col - fb_w / cell_w) / 2);
        origin_row = imaxi(1, 1 + (ws.ws_row - 1 - fb_h / cell_h) / 2);
    }
    input_set_geometry(ws.ws_col, ws.ws_row, cell_w, cell_h,
                       origin_col, origin_row, scale, off_x, off_y);
}

int term_init(void)
{
    input_reset();
    kittyts_session_init(&session);
    kittyts_options_init(&options);
    /* kitty-framebuffer snaps its clamp down to whole terminal cells. Leave
     * enough headroom that ordinary (and unusually large) cells cannot turn
     * the requested 2x minimum into a 1x canvas. */
    options.framebuffer.min_width =
        CANVAS_W * MIN_PRESENT_SCALE + CELL_SNAP_HEADROOM - 1;
    options.framebuffer.min_height =
        CANVAS_H * MIN_PRESENT_SCALE + CELL_SNAP_HEADROOM - 1;
    options.framebuffer.max_width = 2400;
    options.framebuffer.max_height = 1600;
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    options.pixel_mouse = true;
    options.focus_events = true;
    options.bracketed_paste = false;
    if (getenv("KILIX_LIGHTS_SKIP_PROBE"))
        options.framebuffer.probe_graphics = false;
    if (kittyts_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return -1;
    if (!alloc_present_buffer(kittyts_width(&session),
                              kittyts_height(&session))) {
        kittyts_stop(&session);
        errno = ENOMEM;
        return -1;
    }
    active = true;
    compute_placement();
    mirror_geometry();
    return 0;
}

void term_shutdown(void)
{
    kittyts_stop(&session);      /* internally idempotent / no-op if unused */
    active = false;
    free(present_buf);
    present_buf = NULL;
}

void term_emergency_restore(void)
{
    /* Async-signal-safe: mouse off (leave sequence), kbd mode pop,
     * framebuffer restore. No locks, no frees. */
    kittyts_emergency_restore(&session);
}

/* One pass: 0xAARRGGBB -> RGBA bytes, replicated scale x scale. Canvas alpha
 * is ignored because the game canvas is opaque. */
static void upscale(const uint32_t *src)
{
    for (int y = 0; y < CANVAS_H; y++) {
        uint8_t *row0 = present_buf +
            ((size_t)(off_y + y * scale) * (size_t)fb_w + (size_t)off_x) * 4;
        uint8_t *p = row0;
        for (int x = 0; x < CANVAS_W; x++) {
            uint32_t c = src[y * CANVAS_W + x];
            uint8_t r = (uint8_t)(c >> 16);
            uint8_t g = (uint8_t)(c >> 8);
            uint8_t b = (uint8_t)c;
            for (int k = 0; k < scale; k++) {
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = 255;
                p += 4;
            }
        }
        for (int k = 1; k < scale; k++)
            memcpy(row0 + (size_t)k * (size_t)fb_w * 4, row0,
                   (size_t)CANVAS_W * (size_t)scale * 4);
    }
}

bool term_present_canvas(const Canvas *c)
{
    if (!active || present_buf == NULL || c == NULL || c->px == NULL ||
        c->w != CANVAS_W || c->h != CANVAS_H)
        return false;
    upscale(c->px);
    return kittyts_present(&session, present_buf, fb_w, fb_h) &&
           !kittyfb_failed(&session.framebuffer);
}

bool term_check_resize(void)
{
    int nw, nh;
    struct winsize ws;

    if (!active) return false;
    if (kittyts_check_resize(&session, &nw, &nh)) {
        /* On alloc failure present_buf stays NULL and term_present_canvas
         * returns false, which exits the main loop. */
        (void)alloc_present_buffer(nw, nh);
        compute_placement();
        mirror_geometry();
        return true;
    }
    /* Centering-only change (cols/rows moved but the clamped fb size did
     * not): re-mirror so the mouse map stays exact. */
    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        (ws.ws_col != last_ws.ws_col || ws.ws_row != last_ws.ws_row ||
         ws.ws_xpixel != last_ws.ws_xpixel ||
         ws.ws_ypixel != last_ws.ws_ypixel))
        mirror_geometry();
    return false;
}

/* Non-QWERTY fallback (input.h): keep functional (PUA) and ASCII keys as-is;
 * for other layouts report the PC-101 base-layout key so bindings on
 * 'a'..'z' keep working. */
static uint32_t translate_key(const kittykb_event *ev)
{
    /* Kitty's produced-text and shifted-key fields carry layout-resolved
     * punctuation (Shift+1 -> '!').  Bindings still fall back to the base
     * key below, but text editors must receive what the user actually typed. */
    if (ev->action != KITTYKB_ACTION_RELEASE) {
        if (ev->text_length == 1 && ev->text[0] >= 32 && ev->text[0] <= 126)
            return ev->text[0];
        if ((ev->modifiers & KITTYKB_MOD_SHIFT) != 0 &&
            ev->shifted_key >= 32 && ev->shifted_key <= 126)
            return ev->shifted_key;
    }
    if (ev->key < 0x80 || (ev->key >= 0xe000 && ev->key <= 0xe0ff))
        return ev->key;
    return ev->base_layout_key != 0 ? ev->base_layout_key : ev->key;
}

bool term_translation_test(void)
{
    static const char shifted_one[] = "\x1b[49:33;2u";
    static const char shifted_a[] = "\x1b[97:65;2u";
    const struct {
        const char *bytes;
        size_t length;
        uint32_t expected;
    } cases[] = {
        {shifted_one, sizeof shifted_one - 1u, '!'},
        {shifted_a, sizeof shifted_a - 1u, 'A'}
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        kittykb_input input;
        kittykb_event event;
        kittykb_input_init(&input);
        kittykb_input_feed(&input, cases[i].bytes, cases[i].length);
        if (!kittykb_input_next(&input, &event) ||
            translate_key(&event) != cases[i].expected)
            return false;
    }
    return true;
}

static void dispatch_input(const kittyin_event *event)
{
    if (event->kind == KITTYIN_EVENT_KEY) {
        const kittykb_event *key = &event->data.key;
        input_push_key(translate_key(key), key->modifiers, (int)key->action);
    } else if (event->kind == KITTYIN_EVENT_MOUSE) {
        const kittyin_mouse_event *mouse = &event->data.mouse;
        input_push_mouse(mouse->x, mouse->y, mouse->modifiers, mouse->button,
                         mouse->wheel_x, mouse->wheel_y, mouse->action,
                         mouse->pixel_coordinates);
    } else if (event->kind == KITTYIN_EVENT_FOCUS) {
        input_push_focus(event->data.focus.focused);
    }
}

int term_read_input(void)
{
    kittyin_event event;

    if (!active) return 0;
    if (kittyts_read_input(&session) < 0) return -1;
    while (kittyts_next_event(&session, &event)) dispatch_input(&event);
    return 0;
}

bool term_key_down(uint32_t key)
{
    return active && kittyts_key_down(&session, key);
}

bool term_has_release_events(void)
{
    return active && kittyts_has_release_events(&session);
}

int term_scale(void)
{
    return scale;
}

void term_offsets(int *out_x, int *out_y)
{
    if (out_x) *out_x = off_x;
    if (out_y) *out_y = off_y;
}
