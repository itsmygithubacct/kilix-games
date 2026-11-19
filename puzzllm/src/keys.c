/* keys.c - see keys.h. */
#include "keys.h"
#include "neuroterm_terminal.h"

#include <stdio.h>
#include <string.h>

static size_t utf8(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cp > 0x10ffff) return 0;
    o[0] = (char)(0xf0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static size_t put(char *out, size_t cap, const char *s, size_t n)
{
    if (n > cap) return 0;
    memcpy(out, s, n);
    return n;
}

/* xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl. */
static int mod_param(uint32_t mods)
{
    return 1 + (int)(mods & (MOD_SHIFT | MOD_ALT | MOD_CTRL));
}

static size_t cursor_key(char letter, uint32_t mods, bool app, char *out, size_t cap)
{
    char b[24];
    int n;
    if ((mods & (MOD_SHIFT | MOD_ALT | MOD_CTRL)) != 0)
        n = snprintf(b, sizeof b, "\033[1;%d%c", mod_param(mods), letter);
    else
        n = snprintf(b, sizeof b, "\033%c%c", app ? 'O' : '[', letter);
    return put(out, cap, b, (size_t)n);
}

static size_t tilde_key(int code, uint32_t mods, char *out, size_t cap)
{
    char b[24];
    int n;
    if ((mods & (MOD_SHIFT | MOD_ALT | MOD_CTRL)) != 0)
        n = snprintf(b, sizeof b, "\033[%d;%d~", code, mod_param(mods));
    else
        n = snprintf(b, sizeof b, "\033[%d~", code);
    return put(out, cap, b, (size_t)n);
}

size_t keys_encode(const key_event *ev, bool app_cursor, char *out, size_t cap)
{
    char b[8 * KEY_TEXT_MAX + 2];
    size_t n = 0;
    uint32_t k = ev->key;
    uint32_t m = ev->mods;
    bool alt = (m & MOD_ALT) != 0;
    bool ctrl = (m & MOD_CTRL) != 0;

    if (ev->action == 3) return 0;

    switch (k) {
    case KEY_ENTER: n = 0; if (alt) b[n++] = 0x1b; b[n++] = '\r'; return put(out, cap, b, n);
    case KEY_TAB:
        if (m & MOD_SHIFT) return put(out, cap, "\033[Z", 3);
        n = 0; if (alt) b[n++] = 0x1b; b[n++] = '\t'; return put(out, cap, b, n);
    case KEY_BACKSPACE:
        n = 0; if (alt) b[n++] = 0x1b; b[n++] = ctrl ? 0x08 : 0x7f;
        return put(out, cap, b, n);
    case KEY_ESCAPE: n = 0; if (alt) b[n++] = 0x1b; b[n++] = 0x1b; return put(out, cap, b, n);
    case KEY_UP: return cursor_key('A', m, app_cursor, out, cap);
    case KEY_DOWN: return cursor_key('B', m, app_cursor, out, cap);
    case KEY_RIGHT: return cursor_key('C', m, app_cursor, out, cap);
    case KEY_LEFT: return cursor_key('D', m, app_cursor, out, cap);
    case KEY_HOME: return cursor_key('H', m, app_cursor, out, cap);
    case KEY_END: return cursor_key('F', m, app_cursor, out, cap);
    case KEY_INSERT: return tilde_key(2, m, out, cap);
    case KEY_DELETE: return tilde_key(3, m, out, cap);
    case KEY_PAGE_UP: return tilde_key(5, m, out, cap);
    case KEY_PAGE_DOWN: return tilde_key(6, m, out, cap);
    default: break;
    }
    if (k >= KEY_F1 && k <= KEY_F12) {
        static const int codes[12] = {0, 0, 0, 0, 15, 17, 18, 19, 20, 21, 23, 24};
        int f = (int)(k - KEY_F1);
        if (f < 4) {
            if ((m & (MOD_SHIFT | MOD_ALT | MOD_CTRL)) != 0) {
                int len = snprintf(b, sizeof b, "\033[1;%d%c", mod_param(m), 'P' + f);
                return put(out, cap, b, (size_t)len);
            }
            b[0] = 0x1b; b[1] = 'O'; b[2] = (char)('P' + f);
            return put(out, cap, b, 3);
        }
        return tilde_key(codes[f], m, out, cap);
    }
    if (k >= 0xe000 && k <= 0xf8ff) return 0;   /* other functional keys */

    if (ctrl) {
        uint32_t c = k;
        unsigned char code;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c >= 'a' && c <= 'z') code = (unsigned char)(c - 'a' + 1);
        else if (c == ' ' || c == '@' || c == '2') code = 0;
        else if (c == '[' || c == '3') code = 0x1b;
        else if (c == '\\' || c == '4') code = 0x1c;
        else if (c == ']' || c == '5') code = 0x1d;
        else if (c == '^' || c == '6') code = 0x1e;
        else if (c == '_' || c == '-' || c == '7') code = 0x1f;
        else if (c == '?' || c == '8') code = 0x7f;
        else return 0;
        n = 0;
        if (alt) b[n++] = 0x1b;
        b[n++] = (char)code;
        return put(out, cap, b, n);
    }

    n = 0;
    if (alt) b[n++] = 0x1b;
    if (ev->text_len > 0) {
        for (int i = 0; i < ev->text_len && i < KEY_TEXT_MAX; i++)
            n += utf8(ev->text[i], b + n);
    } else if (k >= 0x20 && k != 0x7f) {
        uint32_t c = k;
        if ((m & MOD_SHIFT) && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        n += utf8(c, b + n);
    } else {
        return 0;
    }
    return put(out, cap, b, n);
}

size_t keys_paste(const char *text, bool bracketed, char *out, size_t cap)
{
    size_t n = 0, len = strlen(text);
    if (bracketed) {
        if (cap < 12 + len) return 0;
        memcpy(out, "\033[200~", 6);
        n = 6;
    } else if (cap < len) {
        return 0;
    }
    for (size_t i = 0; i < len; i++)
        out[n++] = text[i] == '\n' ? '\r' : text[i];
    if (bracketed) {
        memcpy(out + n, "\033[201~", 6);
        n += 6;
    }
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
