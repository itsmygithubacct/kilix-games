/* vt.h - a small xterm-compatible terminal emulator for the room's screens.
 *
 * Bytes from a pty go in with vt_feed(); the screen is a grid of cells the
 * renderer draws. It covers what omp's TUI and bash use: UTF-8, C0 controls,
 * CSI cursor/erase/insert/delete/scroll, scroll regions, SGR with 16, 256
 * and truecolour, the alternate screen, DECTCEM, DECCKM, DECAWM, bracketed
 * paste, and replies to DA1, DSR and CPR. OSC, DCS, APC (Kitty graphics),
 * PM and SOS strings are consumed and ignored. There is no scrollback.
 *
 * Pure C11, no I/O: replies to queries are queued and fetched with
 * vt_take_reply() so the caller writes them back to the pty.
 */
#ifndef PUZZLLM_VT_H
#define PUZZLLM_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VT_ATTR_BOLD = 1u << 0,
    VT_ATTR_DIM = 1u << 1,
    VT_ATTR_ITALIC = 1u << 2,
    VT_ATTR_UNDERLINE = 1u << 3,
    VT_ATTR_REVERSE = 1u << 4,
    VT_ATTR_STRIKE = 1u << 5,
    VT_ATTR_WIDE = 1u << 6,      /* left half of a double-width character */
    VT_ATTR_WIDE_TAIL = 1u << 7  /* right half; draw nothing */
};

/* Colours are 0x00RRGGBB, or VT_DEFAULT_COLOR for the terminal default. */
#define VT_DEFAULT_COLOR 0xff000000u

typedef struct vt_cell {
    uint32_t ch;   /* Unicode scalar; ' ' when blank */
    uint32_t fg;
    uint32_t bg;
    uint8_t attr;
} vt_cell;

enum { VT_REPLY_CAP = 256, VT_PARAM_MAX = 32, VT_STRING_CAP = 512 };

typedef struct vt {
    int cols, rows;
    vt_cell *main_grid, *alt_grid, *grid;   /* grid points at the active one */
    int cx, cy;
    bool wrap_pending;
    int top, bottom;                        /* scroll region, inclusive */
    vt_cell pen;                            /* attributes for new text */
    struct { int cx, cy; vt_cell pen; } saved, saved_main;
    bool alt_active;
    bool cursor_visible;
    bool app_cursor;
    bool autowrap;
    bool bracketed_paste;
    bool origin_mode;
    uint8_t tabs[512];                      /* 1 = tab stop, per column */

    /* parser */
    int state;
    uint32_t utf8_cp;
    int utf8_left;
    int params[VT_PARAM_MAX];
    bool sub[VT_PARAM_MAX];                 /* parameter followed a ':' */
    int nparams;
    bool param_started;
    char private_marker;                    /* '?', '>', '<', '=' or 0 */
    char intermediate;
    bool string_esc;                        /* ESC seen inside a string */
    char title[128];
    char osc[VT_STRING_CAP];
    size_t osc_len;

    char reply[VT_REPLY_CAP];
    size_t reply_len;
    uint64_t generation;                    /* bumps on every visible change */
} vt;

bool vt_init(vt *t, int cols, int rows);
void vt_free(vt *t);
bool vt_resize(vt *t, int cols, int rows);
void vt_feed(vt *t, const void *bytes, size_t len);
/* Copies queued replies into buf (at most cap bytes) and drops them. */
size_t vt_take_reply(vt *t, char *buf, size_t cap);
const vt_cell *vt_cell_at(const vt *t, int x, int y);
/* Writes row y as UTF-8 (trailing blanks trimmed) for tests and logs. */
size_t vt_row_text(const vt *t, int y, char *buf, size_t cap);
/* Display width of a scalar: 0, 1 or 2. */
int vt_char_width(uint32_t cp);
/* xterm 256-colour palette entry. */
uint32_t vt_palette(int index);

#endif
