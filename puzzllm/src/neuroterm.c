/* libvterm integration based on NeuroTerm's terminal setup, MIT.
 * Copyright (c) 2026 itsmygithubacct */
#include "neuroterm.h"

#include <string.h>

static int set_term_prop(VTermProp prop, VTermValue *value, void *user)
{
    neuroterm *t = user;
    if (prop == VTERM_PROP_CURSORVISIBLE) t->cursor_visible = value->boolean;
    return 1;
}

bool neuroterm_init(neuroterm *t, int cols, int rows)
{
    static const VTermScreenCallbacks callbacks = {.settermprop = set_term_prop};
    VTermColor fg, bg, blue;
    memset(t, 0, sizeof *t);
    t->term = vterm_new(rows, cols);
    if (!t->term) return false;
    t->screen = vterm_obtain_screen(t->term);
    if (!t->screen) {
        neuroterm_free(t);
        return false;
    }
    t->cols = cols;
    t->rows = rows;
    t->cursor_visible = true;
    vterm_set_utf8(t->term, 1);
    vterm_screen_set_callbacks(t->screen, &callbacks, t);
    vterm_screen_enable_altscreen(t->screen, 1);
    vterm_screen_set_damage_merge(t->screen, VTERM_DAMAGE_SCROLL);
    vterm_color_rgb(&fg, (NEUROTERM_FG >> 16) & 255, (NEUROTERM_FG >> 8) & 255, NEUROTERM_FG & 255);
    vterm_color_rgb(&bg, (NEUROTERM_BG >> 16) & 255, (NEUROTERM_BG >> 8) & 255, NEUROTERM_BG & 255);
    vterm_screen_set_default_colors(t->screen, &fg, &bg);
    vterm_screen_reset(t->screen, 1);
    vterm_color_rgb(&blue, 0x5c, 0x8c, 0xff);
    vterm_state_set_palette_color(vterm_obtain_state(t->term), 4, &blue);
    return true;
}

void neuroterm_free(neuroterm *t)
{
    if (t->term) vterm_free(t->term);
    memset(t, 0, sizeof *t);
}

bool neuroterm_resize(neuroterm *t, int cols, int rows)
{
    if (cols <= 0 || rows <= 0) return false;
    if (cols == t->cols && rows == t->rows) return true;
    vterm_set_size(t->term, rows, cols);
    vterm_screen_flush_damage(t->screen);
    vterm_get_size(t->term, &t->rows, &t->cols);
    return t->rows == rows && t->cols == cols;
}

void neuroterm_feed(neuroterm *t, const void *bytes, size_t len)
{
    vterm_input_write(t->term, bytes, len);
    vterm_screen_flush_damage(t->screen);
}

size_t neuroterm_read(neuroterm *t, char *buf, size_t cap)
{
    return vterm_output_read(t->term, buf, cap);
}

VTermPos neuroterm_cursor(const neuroterm *t)
{
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(t->term), &cursor);
    return cursor;
}

bool neuroterm_cell(const neuroterm *t, int col, int row, VTermScreenCell *cell)
{
    return vterm_screen_get_cell(t->screen, (VTermPos){.row = row, .col = col}, cell) != 0;
}

uint32_t neuroterm_color(const neuroterm *t, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(t->screen, &color);
    return (uint32_t)color.rgb.red << 16 | (uint32_t)color.rgb.green << 8 | color.rgb.blue;
}

size_t neuroterm_row_text(const neuroterm *t, int row, char *buf, size_t cap)
{
    if (!cap) return 0;
    VTermRect rect = {row, row + 1, 0, t->cols};
    size_t n = vterm_screen_get_text(t->screen, buf, cap - 1, rect);
    buf[n] = 0;
    return n;
}

size_t neuroterm_key(neuroterm *t, const key_event *event, char *buf, size_t cap)
{
    static const VTermKey navigation[] = {
        [KEY_UP - KEY_LEFT] = VTERM_KEY_UP,
        [KEY_DOWN - KEY_LEFT] = VTERM_KEY_DOWN,
        [KEY_LEFT - KEY_LEFT] = VTERM_KEY_LEFT,
        [KEY_RIGHT - KEY_LEFT] = VTERM_KEY_RIGHT,
        [KEY_PAGE_UP - KEY_LEFT] = VTERM_KEY_PAGEUP,
        [KEY_PAGE_DOWN - KEY_LEFT] = VTERM_KEY_PAGEDOWN,
        [KEY_HOME - KEY_LEFT] = VTERM_KEY_HOME,
        [KEY_END - KEY_LEFT] = VTERM_KEY_END,
    };
    if (event->action == 3) return 0;
    if (event->key < KEY_LEFT || event->key > KEY_END)
        return keys_encode(event, false, buf, cap);
    VTermModifier mods = VTERM_MOD_NONE;
    if (event->mods & MOD_CTRL) mods |= VTERM_MOD_CTRL;
    if (event->mods & MOD_ALT) mods |= VTERM_MOD_ALT;
    if (event->mods & MOD_SHIFT) mods |= VTERM_MOD_SHIFT;
    vterm_keyboard_key(t->term, navigation[event->key - KEY_LEFT], mods);
    return neuroterm_read(t, buf, cap);
}
