/* vt.c - see vt.h. */
#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ST_GROUND,
    ST_ESC,
    ST_ESC_INTER,   /* ESC ( ) * + # %: one more byte to swallow */
    ST_CSI,
    ST_OSC,
    ST_STRING       /* DCS, APC, PM, SOS: ignored until ST */
};

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

uint32_t vt_palette(int i)
{
    static const uint32_t base[16] = {
        0x1d1f21, 0xcc6666, 0xb5bd68, 0xf0c674,
        0x81a2be, 0xb294bb, 0x8abeb7, 0xc5c8c6,
        0x666666, 0xff7b72, 0xc3e88d, 0xffe17a,
        0x8fbfff, 0xd7a6ff, 0x93e3db, 0xffffff
    };
    static const int steps[6] = {0, 95, 135, 175, 215, 255};

    if (i < 0) i = 0;
    if (i < 16) return base[i];
    if (i < 232) {
        i -= 16;
        return ((uint32_t)steps[i / 36] << 16) |
               ((uint32_t)steps[(i / 6) % 6] << 8) | (uint32_t)steps[i % 6];
    }
    if (i > 255) i = 255;
    {
        uint32_t g = (uint32_t)(8 + (i - 232) * 10);
        return (g << 16) | (g << 8) | g;
    }
}

int vt_char_width(uint32_t cp)
{
    if (cp == 0) return 0;
    if (cp < 0x300) return 1;
    /* combining marks and zero-width characters */
    if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) ||
        (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
        cp == 0x2060 || cp == 0xfeff)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) ||
        (cp >= 0x3041 && cp <= 0x33ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
        (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
        (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd))
        return 2;
    return 1;
}

static vt_cell blank_cell(const vt *t)
{
    vt_cell c;
    c.ch = ' ';
    c.fg = t->pen.fg;
    c.bg = t->pen.bg;
    c.attr = 0;
    return c;
}

static vt_cell *row_ptr(vt *t, int y)
{
    return t->grid + (size_t)y * (size_t)t->cols;
}

static void clear_cells(vt *t, int y, int x0, int x1)
{
    vt_cell b = blank_cell(t);
    vt_cell *r;

    if (y < 0 || y >= t->rows) return;
    x0 = clampi(x0, 0, t->cols);
    x1 = clampi(x1, 0, t->cols);
    r = row_ptr(t, y);
    for (int x = x0; x < x1; x++) r[x] = b;
}

static void fill_grid(vt *t, vt_cell *g)
{
    vt_cell b;
    b.ch = ' ';
    b.fg = VT_DEFAULT_COLOR;
    b.bg = VT_DEFAULT_COLOR;
    b.attr = 0;
    for (size_t i = 0; i < (size_t)t->cols * (size_t)t->rows; i++) g[i] = b;
}

static void reset_tabs(vt *t)
{
    memset(t->tabs, 0, sizeof t->tabs);
    for (int x = 8; x < (int)sizeof t->tabs; x += 8) t->tabs[x] = 1;
}

static void reset_pen(vt *t)
{
    t->pen.ch = ' ';
    t->pen.fg = VT_DEFAULT_COLOR;
    t->pen.bg = VT_DEFAULT_COLOR;
    t->pen.attr = 0;
}

static void full_reset(vt *t)
{
    t->grid = t->main_grid;
    fill_grid(t, t->main_grid);
    fill_grid(t, t->alt_grid);
    t->cx = t->cy = 0;
    t->wrap_pending = false;
    t->top = 0;
    t->bottom = t->rows - 1;
    reset_pen(t);
    t->saved.cx = t->saved.cy = 0;
    t->saved.pen = t->pen;
    t->saved_main = t->saved;
    t->alt_active = false;
    t->cursor_visible = true;
    t->app_cursor = false;
    t->autowrap = true;
    t->bracketed_paste = false;
    t->origin_mode = false;
    reset_tabs(t);
    t->state = ST_GROUND;
    t->utf8_left = 0;
    t->generation++;
}

bool vt_init(vt *t, int cols, int rows)
{
    memset(t, 0, sizeof *t);
    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return false;
    t->cols = cols;
    t->rows = rows;
    t->main_grid = calloc((size_t)cols * (size_t)rows, sizeof(vt_cell));
    t->alt_grid = calloc((size_t)cols * (size_t)rows, sizeof(vt_cell));
    if (t->main_grid == NULL || t->alt_grid == NULL) {
        vt_free(t);
        return false;
    }
    full_reset(t);
    return true;
}

void vt_free(vt *t)
{
    free(t->main_grid);
    free(t->alt_grid);
    t->main_grid = t->alt_grid = t->grid = NULL;
}

bool vt_resize(vt *t, int cols, int rows)
{
    vt_cell *m, *a;
    int old_cols = t->cols, old_rows = t->rows;
    vt_cell *om = t->main_grid, *oa = t->alt_grid;
    int shift;

    if (cols < 1 || rows < 1 || cols > 1000 || rows > 1000) return false;
    if (cols == old_cols && rows == old_rows) return true;
    m = calloc((size_t)cols * (size_t)rows, sizeof(vt_cell));
    a = calloc((size_t)cols * (size_t)rows, sizeof(vt_cell));
    if (m == NULL || a == NULL) {
        free(m);
        free(a);
        return false;
    }
    t->cols = cols;
    t->rows = rows;
    fill_grid(t, m);
    fill_grid(t, a);
    /* Keep the bottom of the screen when shrinking, as terminals do, so the
     * cursor line stays visible. */
    shift = t->cy - (rows - 1);
    if (shift < 0) shift = 0;
    for (int y = 0; y < rows; y++) {
        int sy = y + shift;
        if (sy >= old_rows) break;
        for (int x = 0; x < cols && x < old_cols; x++) {
            m[(size_t)y * cols + x] = om[(size_t)sy * old_cols + x];
            a[(size_t)y * cols + x] = oa[(size_t)sy * old_cols + x];
        }
    }
    free(om);
    free(oa);
    t->main_grid = m;
    t->alt_grid = a;
    t->grid = t->alt_active ? a : m;
    t->cy = clampi(t->cy - shift, 0, rows - 1);
    t->cx = clampi(t->cx, 0, cols - 1);
    t->top = 0;
    t->bottom = rows - 1;
    t->wrap_pending = false;
    t->generation++;
    return true;
}

static void reply(vt *t, const char *s)
{
    size_t n = strlen(s);
    if (t->reply_len + n > VT_REPLY_CAP) return;
    memcpy(t->reply + t->reply_len, s, n);
    t->reply_len += n;
}

size_t vt_take_reply(vt *t, char *buf, size_t cap)
{
    size_t n = t->reply_len < cap ? t->reply_len : cap;
    memcpy(buf, t->reply, n);
    memmove(t->reply, t->reply + n, t->reply_len - n);
    t->reply_len -= n;
    return n;
}

const vt_cell *vt_cell_at(const vt *t, int x, int y)
{
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return NULL;
    return t->grid + (size_t)y * (size_t)t->cols + (size_t)x;
}

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

size_t vt_row_text(const vt *t, int y, char *buf, size_t cap)
{
    size_t n = 0, keep = 0;
    if (cap == 0) return 0;
    if (y < 0 || y >= t->rows) { buf[0] = '\0'; return 0; }
    for (int x = 0; x < t->cols; x++) {
        const vt_cell *c = vt_cell_at(t, x, y);
        char tmp[4];
        size_t k;
        if (c->attr & VT_ATTR_WIDE_TAIL) continue;
        k = put_utf8(tmp, c->ch);
        if (n + k + 1 > cap) break;
        memcpy(buf + n, tmp, k);
        n += k;
        if (c->ch != ' ') keep = n;
    }
    buf[keep] = '\0';
    return keep;
}

/* ---- screen operations ---- */

static void scroll_up(vt *t, int top, int bottom, int n)
{
    int h = bottom - top + 1;
    if (n <= 0 || h <= 0) return;
    if (n > h) n = h;
    memmove(row_ptr(t, top), row_ptr(t, top + n),
            (size_t)(h - n) * (size_t)t->cols * sizeof(vt_cell));
    for (int y = bottom - n + 1; y <= bottom; y++) clear_cells(t, y, 0, t->cols);
}

static void scroll_down(vt *t, int top, int bottom, int n)
{
    int h = bottom - top + 1;
    if (n <= 0 || h <= 0) return;
    if (n > h) n = h;
    memmove(row_ptr(t, top + n), row_ptr(t, top),
            (size_t)(h - n) * (size_t)t->cols * sizeof(vt_cell));
    for (int y = top; y < top + n; y++) clear_cells(t, y, 0, t->cols);
}

static void linefeed(vt *t)
{
    t->wrap_pending = false;
    if (t->cy == t->bottom) scroll_up(t, t->top, t->bottom, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_index(vt *t)
{
    t->wrap_pending = false;
    if (t->cy == t->top) scroll_down(t, t->top, t->bottom, 1);
    else if (t->cy > 0) t->cy--;
}

static void put_char(vt *t, uint32_t cp)
{
    int w = vt_char_width(cp);
    vt_cell *r;

    if (w == 0) return;     /* combining marks are dropped */
    if (t->wrap_pending) {
        t->wrap_pending = false;
        if (t->autowrap) {
            t->cx = 0;
            linefeed(t);
        }
    }
    if (w == 2 && t->cx == t->cols - 1) {
        if (t->autowrap) {
            clear_cells(t, t->cy, t->cx, t->cols);
            t->cx = 0;
            linefeed(t);
        } else {
            w = 1;
        }
    }
    if (w == 2 && t->cols < 2) w = 1;
    r = row_ptr(t, t->cy);
    /* Writing over half of a wide character blanks its other half. */
    if (r[t->cx].attr & VT_ATTR_WIDE_TAIL && t->cx > 0)
        r[t->cx - 1] = blank_cell(t);
    if (r[t->cx].attr & VT_ATTR_WIDE && t->cx + 1 < t->cols)
        r[t->cx + 1] = blank_cell(t);
    r[t->cx] = t->pen;
    r[t->cx].ch = cp;
    r[t->cx].attr = (uint8_t)(t->pen.attr & ~(VT_ATTR_WIDE | VT_ATTR_WIDE_TAIL));
    if (w == 2) {
        r[t->cx].attr |= VT_ATTR_WIDE;
        r[t->cx + 1] = r[t->cx];
        r[t->cx + 1].ch = ' ';
        r[t->cx + 1].attr = (uint8_t)((r[t->cx].attr & ~VT_ATTR_WIDE) |
                                      VT_ATTR_WIDE_TAIL);
    }
    if (t->cx + w >= t->cols) {
        t->cx = t->cols - 1;
        t->wrap_pending = true;
    } else {
        t->cx += w;
    }
}

static void erase_display(vt *t, int mode)
{
    if (mode == 0) {
        clear_cells(t, t->cy, t->cx, t->cols);
        for (int y = t->cy + 1; y < t->rows; y++) clear_cells(t, y, 0, t->cols);
    } else if (mode == 1) {
        for (int y = 0; y < t->cy; y++) clear_cells(t, y, 0, t->cols);
        clear_cells(t, t->cy, 0, t->cx + 1);
    } else if (mode == 2 || mode == 3) {
        for (int y = 0; y < t->rows; y++) clear_cells(t, y, 0, t->cols);
    }
}

static void erase_line(vt *t, int mode)
{
    if (mode == 0) clear_cells(t, t->cy, t->cx, t->cols);
    else if (mode == 1) clear_cells(t, t->cy, 0, t->cx + 1);
    else if (mode == 2) clear_cells(t, t->cy, 0, t->cols);
}

static void insert_chars(vt *t, int n)
{
    vt_cell *r = row_ptr(t, t->cy);
    int keep;
    n = clampi(n, 1, t->cols - t->cx);
    keep = t->cols - t->cx - n;
    memmove(r + t->cx + n, r + t->cx, (size_t)keep * sizeof(vt_cell));
    clear_cells(t, t->cy, t->cx, t->cx + n);
}

static void delete_chars(vt *t, int n)
{
    vt_cell *r = row_ptr(t, t->cy);
    int keep;
    n = clampi(n, 1, t->cols - t->cx);
    keep = t->cols - t->cx - n;
    memmove(r + t->cx, r + t->cx + n, (size_t)keep * sizeof(vt_cell));
    clear_cells(t, t->cy, t->cols - n, t->cols);
}

static void move_to(vt *t, int x, int y)
{
    int lo = t->origin_mode ? t->top : 0;
    int hi = t->origin_mode ? t->bottom : t->rows - 1;
    t->cx = clampi(x, 0, t->cols - 1);
    t->cy = clampi(y + (t->origin_mode ? t->top : 0), lo, hi);
    t->wrap_pending = false;
}

static void save_cursor(vt *t)
{
    t->saved.cx = t->cx;
    t->saved.cy = t->cy;
    t->saved.pen = t->pen;
}

static void restore_cursor(vt *t)
{
    t->cx = clampi(t->saved.cx, 0, t->cols - 1);
    t->cy = clampi(t->saved.cy, 0, t->rows - 1);
    t->pen = t->saved.pen;
    t->wrap_pending = false;
}

static void set_alt(vt *t, bool on, bool save, bool clear)
{
    if (on == t->alt_active) return;
    if (on) {
        if (save) {
            save_cursor(t);
            t->saved_main = t->saved;
        }
        t->grid = t->alt_grid;
        t->alt_active = true;
        if (clear) erase_display(t, 2);
    } else {
        t->grid = t->main_grid;
        t->alt_active = false;
        if (save) {
            t->saved = t->saved_main;
            restore_cursor(t);
        }
    }
    t->top = 0;
    t->bottom = t->rows - 1;
}

static int param(const vt *t, int i, int dflt)
{
    if (i >= t->nparams || t->params[i] <= 0) return dflt;
    return t->params[i];
}

static void set_private_mode(vt *t, int mode, bool on)
{
    switch (mode) {
    case 1: t->app_cursor = on; break;
    case 6: t->origin_mode = on; move_to(t, 0, 0); break;
    case 7: t->autowrap = on; break;
    case 25: t->cursor_visible = on; break;
    case 47: set_alt(t, on, false, false); break;
    case 1047: set_alt(t, on, false, on); break;
    case 1049: set_alt(t, on, true, true); break;
    case 2004: t->bracketed_paste = on; break;
    default: break;   /* mouse, focus, sync output: nothing to do here */
    }
}

/* SGR. Parameters arrive flattened, sub-parameters (':') included, so
 * 38:2:r:g:b, 38;2;r;g;b and 38;5;n all parse the same way. */
static void sgr(vt *t)
{
    if (t->nparams == 0) {
        reset_pen(t);
        return;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i] < 0 ? 0 : t->params[i];
        if (p == 0) reset_pen(t);
        else if (p == 1) t->pen.attr |= VT_ATTR_BOLD;
        else if (p == 2) t->pen.attr |= VT_ATTR_DIM;
        else if (p == 3) t->pen.attr |= VT_ATTR_ITALIC;
        else if (p == 4) t->pen.attr |= VT_ATTR_UNDERLINE;
        else if (p == 7) t->pen.attr |= VT_ATTR_REVERSE;
        else if (p == 9) t->pen.attr |= VT_ATTR_STRIKE;
        else if (p == 22) t->pen.attr &= (uint8_t)~(VT_ATTR_BOLD | VT_ATTR_DIM);
        else if (p == 23) t->pen.attr &= (uint8_t)~VT_ATTR_ITALIC;
        else if (p == 24) t->pen.attr &= (uint8_t)~VT_ATTR_UNDERLINE;
        else if (p == 27) t->pen.attr &= (uint8_t)~VT_ATTR_REVERSE;
        else if (p == 29) t->pen.attr &= (uint8_t)~VT_ATTR_STRIKE;
        else if (p >= 30 && p <= 37) t->pen.fg = vt_palette(p - 30);
        else if (p == 39) t->pen.fg = VT_DEFAULT_COLOR;
        else if (p >= 40 && p <= 47) t->pen.bg = vt_palette(p - 40);
        else if (p == 49) t->pen.bg = VT_DEFAULT_COLOR;
        else if (p >= 90 && p <= 97) t->pen.fg = vt_palette(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->pen.bg = vt_palette(p - 100 + 8);
        else if (p == 38 || p == 48 || p == 58) {
            uint32_t c = VT_DEFAULT_COLOR;
            int kind = i + 1 < t->nparams ? t->params[i + 1] : -1;
            int run = 0;   /* colon sub-parameters after this one */
            while (i + 1 + run < t->nparams && t->sub[i + 1 + run]) run++;
            if (kind == 2 && run >= 5) {
                /* 38:2:CS:r:g:b - ISO 8613-6 form with a colour-space slot */
                c = ((uint32_t)clampi(t->params[i + 3], 0, 255) << 16) |
                    ((uint32_t)clampi(t->params[i + 4], 0, 255) << 8) |
                    (uint32_t)clampi(t->params[i + 5], 0, 255);
                i += run;
            } else if (kind == 5 && i + 2 < t->nparams) {
                c = vt_palette(clampi(t->params[i + 2], 0, 255));
                i += 2;
            } else if (kind == 2 && i + 4 < t->nparams) {
                c = ((uint32_t)clampi(t->params[i + 2], 0, 255) << 16) |
                    ((uint32_t)clampi(t->params[i + 3], 0, 255) << 8) |
                    (uint32_t)clampi(t->params[i + 4], 0, 255);
                i += 4;
            } else {
                i = t->nparams;   /* malformed: drop the rest */
                continue;
            }
            if (p == 38) t->pen.fg = c;
            else if (p == 48) t->pen.bg = c;
            /* 58 is the underline colour: parsed and ignored */
        }
    }
}

static void csi_dispatch(vt *t, unsigned char f)
{
    char buf[64];

    if (t->private_marker == '?') {
        if (f == 'h' || f == 'l') {
            for (int i = 0; i < t->nparams; i++)
                set_private_mode(t, t->params[i], f == 'h');
        }
        /* CSI ? u (Kitty keyboard query) is deliberately not answered: the
         * program then falls back to legacy keys, which keys.c produces. */
        return;
    }
    if (t->private_marker == '>' || t->private_marker == '<' ||
        t->private_marker == '=') {
        if (t->private_marker == '>' && f == 'c')
            reply(t, "\033[>1;10;0c");
        return;
    }
    if (t->intermediate != 0) return;   /* DECSCUSR and friends */

    switch (f) {
    case '@': insert_chars(t, param(t, 0, 1)); break;
    case 'A': move_to(t, t->cx, t->cy - (t->origin_mode ? t->top : 0) - param(t, 0, 1)); break;
    case 'B': case 'e':
        move_to(t, t->cx, t->cy - (t->origin_mode ? t->top : 0) + param(t, 0, 1)); break;
    case 'C': case 'a': move_to(t, t->cx + param(t, 0, 1), t->cy - (t->origin_mode ? t->top : 0)); break;
    case 'D': move_to(t, t->cx - param(t, 0, 1), t->cy - (t->origin_mode ? t->top : 0)); break;
    case 'E': move_to(t, 0, t->cy - (t->origin_mode ? t->top : 0) + param(t, 0, 1)); break;
    case 'F': move_to(t, 0, t->cy - (t->origin_mode ? t->top : 0) - param(t, 0, 1)); break;
    case 'G': case '`': move_to(t, param(t, 0, 1) - 1, t->cy - (t->origin_mode ? t->top : 0)); break;
    case 'H': case 'f': move_to(t, param(t, 1, 1) - 1, param(t, 0, 1) - 1); break;
    case 'd': move_to(t, t->cx, param(t, 0, 1) - 1); break;
    case 'I':
        for (int n = param(t, 0, 1); n > 0; n--) {
            int x = t->cx + 1;
            while (x < t->cols - 1 && !(x < (int)sizeof t->tabs && t->tabs[x])) x++;
            t->cx = clampi(x, 0, t->cols - 1);
        }
        break;
    case 'Z':
        for (int n = param(t, 0, 1); n > 0; n--) {
            int x = t->cx - 1;
            while (x > 0 && !(x < (int)sizeof t->tabs && t->tabs[x])) x--;
            t->cx = clampi(x, 0, t->cols - 1);
        }
        break;
    case 'J': erase_display(t, param(t, 0, 0)); break;
    case 'K': erase_line(t, param(t, 0, 0)); break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bottom)
            scroll_down(t, t->cy, t->bottom, param(t, 0, 1));
        t->cx = 0;
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bottom)
            scroll_up(t, t->cy, t->bottom, param(t, 0, 1));
        t->cx = 0;
        break;
    case 'P': delete_chars(t, param(t, 0, 1)); break;
    case 'S': scroll_up(t, t->top, t->bottom, param(t, 0, 1)); break;
    case 'T': scroll_down(t, t->top, t->bottom, param(t, 0, 1)); break;
    case 'X': clear_cells(t, t->cy, t->cx, t->cx + param(t, 0, 1)); break;
    case 'b': {
        const vt_cell *prev = t->cx > 0 ? vt_cell_at(t, t->cx - 1, t->cy) : NULL;
        uint32_t ch = prev ? prev->ch : ' ';
        for (int n = clampi(param(t, 0, 1), 1, t->cols); n > 0; n--) put_char(t, ch);
        break;
    }
    case 'c': reply(t, "\033[?62;22c"); break;
    case 'g':
        if (param(t, 0, 0) == 3) memset(t->tabs, 0, sizeof t->tabs);
        else if (t->cx < (int)sizeof t->tabs) t->tabs[t->cx] = 0;
        break;
    case 'm': sgr(t); break;
    case 'n':
        if (param(t, 0, 0) == 5) reply(t, "\033[0n");
        else if (param(t, 0, 0) == 6) {
            snprintf(buf, sizeof buf, "\033[%d;%dR",
                     t->cy + 1 - (t->origin_mode ? t->top : 0), t->cx + 1);
            reply(t, buf);
        }
        break;
    case 'r': {
        int top = param(t, 0, 1) - 1;
        int bottom = param(t, 1, t->rows) - 1;
        if (bottom > t->rows - 1) bottom = t->rows - 1;
        if (top < bottom) {
            t->top = top;
            t->bottom = bottom;
            move_to(t, 0, 0);
        }
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 't':
        if (param(t, 0, 0) == 18) {
            snprintf(buf, sizeof buf, "\033[8;%d;%dt", t->rows, t->cols);
            reply(t, buf);
        }
        break;
    default: break;
    }
}

static void esc_dispatch(vt *t, unsigned char c)
{
    switch (c) {
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M': reverse_index(t); break;
    case 'H': if (t->cx < (int)sizeof t->tabs) t->tabs[t->cx] = 1; break;
    case 'c': full_reset(t); break;
    default: break;   /* '=', '>' keypad modes and the rest: ignored */
    }
}

static void control(vt *t, unsigned char c)
{
    switch (c) {
    case 0x07: break;
    case 0x08:
        if (t->cx > 0) t->cx--;
        t->wrap_pending = false;
        break;
    case 0x09: {
        int x = t->cx + 1;
        while (x < t->cols - 1 && !(x < (int)sizeof t->tabs && t->tabs[x])) x++;
        t->cx = clampi(x, 0, t->cols - 1);
        break;
    }
    case 0x0a: case 0x0b: case 0x0c: linefeed(t); break;
    case 0x0d: t->cx = 0; t->wrap_pending = false; break;
    default: break;
    }
}

static void osc_end(vt *t)
{
    t->osc[t->osc_len] = '\0';
    if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') {
        snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
    }
    t->osc_len = 0;
}

static void begin_csi(vt *t)
{
    t->state = ST_CSI;
    t->nparams = 0;
    t->param_started = false;
    t->private_marker = 0;
    t->intermediate = 0;
}

static void ground_byte(vt *t, unsigned char b)
{
    if (t->utf8_left > 0) {
        if ((b & 0xc0) == 0x80) {
            t->utf8_cp = (t->utf8_cp << 6) | (b & 0x3f);
            if (--t->utf8_left == 0) {
                uint32_t cp = t->utf8_cp;
                if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
                put_char(t, cp);
            }
            return;
        }
        t->utf8_left = 0;
        put_char(t, 0xfffd);   /* truncated sequence; reprocess this byte */
    }
    if (b < 0x20 || b == 0x7f) {
        if (b == 0x1b) t->state = ST_ESC;
        else control(t, b);
    } else if (b < 0x80) {
        put_char(t, b);
    } else if ((b & 0xe0) == 0xc0) {
        t->utf8_cp = b & 0x1f;
        t->utf8_left = 1;
    } else if ((b & 0xf0) == 0xe0) {
        t->utf8_cp = b & 0x0f;
        t->utf8_left = 2;
    } else if ((b & 0xf8) == 0xf0) {
        t->utf8_cp = b & 0x07;
        t->utf8_left = 3;
    } else {
        put_char(t, 0xfffd);
    }
}

static void feed_byte(vt *t, unsigned char b)
{
    switch (t->state) {
    case ST_GROUND:
        ground_byte(t, b);
        break;
    case ST_ESC:
        t->state = ST_GROUND;
        if (b == '[') begin_csi(t);
        else if (b == ']') { t->state = ST_OSC; t->osc_len = 0; t->string_esc = false; }
        else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
            t->state = ST_STRING;
            t->string_esc = false;
        } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' ||
                   b == '%' || b == ' ') {
            t->state = ST_ESC_INTER;
        } else if (b == 0x1b) {
            t->state = ST_ESC;
        } else {
            esc_dispatch(t, b);
        }
        break;
    case ST_ESC_INTER:
        t->state = ST_GROUND;
        break;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            if (t->nparams == 0) { t->nparams = 1; t->sub[0] = false; }
            if (!t->param_started) {
                t->params[t->nparams - 1] = 0;
                t->param_started = true;
            }
            if (t->params[t->nparams - 1] < 100000)
                t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            if (t->nparams == 0) {
                t->nparams = 1;
                t->params[0] = -1;
                t->sub[0] = false;
            } else if (!t->param_started) {
                t->params[t->nparams - 1] = -1;
            }
            if (t->nparams < VT_PARAM_MAX) {
                t->nparams++;
                t->params[t->nparams - 1] = -1;
                t->sub[t->nparams - 1] = (b == ':');
            }
            t->param_started = false;
        } else if (b == '?' || b == '>' || b == '<' || b == '=') {
            t->private_marker = (char)b;
        } else if (b >= 0x20 && b <= 0x2f) {
            t->intermediate = (char)b;
        } else if (b >= 0x40 && b <= 0x7e) {
            if (t->nparams > 0 && !t->param_started &&
                t->params[t->nparams - 1] != -1)
                t->params[t->nparams - 1] = -1;
            t->state = ST_GROUND;
            csi_dispatch(t, b);
        } else if (b == 0x1b) {
            t->state = ST_ESC;
        } else if (b < 0x20) {
            control(t, b);
        } else {
            t->state = ST_GROUND;
        }
        break;
    case ST_OSC:
        if (b == 0x07) { osc_end(t); t->state = ST_GROUND; }
        else if (t->string_esc) {
            t->string_esc = false;
            if (b == '\\') { osc_end(t); t->state = ST_GROUND; }
        } else if (b == 0x1b) t->string_esc = true;
        else if (t->osc_len + 1 < VT_STRING_CAP) t->osc[t->osc_len++] = (char)b;
        break;
    case ST_STRING:
        if (t->string_esc) {
            t->string_esc = false;
            if (b == '\\') t->state = ST_GROUND;
        } else if (b == 0x1b) t->string_esc = true;
        else if (b == 0x07) t->state = ST_GROUND;
        break;
    default:
        t->state = ST_GROUND;
        break;
    }
}

void vt_feed(vt *t, const void *bytes, size_t len)
{
    const unsigned char *p = bytes;
    if (len == 0) return;
    for (size_t i = 0; i < len; i++) feed_byte(t, p[i]);
    t->generation++;
}
