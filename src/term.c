/* Terminal input plus presentation through kitty-framebuffer. */
#include "kilix_pong.h"
#include "kitty_framebuffer.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static bool framebuffer_active;
static volatile int shutdown_claimed;
static bool release_capability_seen;

static bool read_byte_timeout(unsigned char *out, int milliseconds)
{
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(STDIN_FILENO, &read_set);
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000
    };
    int selected = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
    return selected > 0 && read(STDIN_FILENO, out, 1) == 1;
}
bool term_init(int *out_width, int *out_height)
{
    kittyfb_options options;

    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&options);
    options.min_width = 320;
    options.min_height = 180;
    options.max_width = 1440;
    options.max_height = 900;
    options.enter_sequence = "\x1b[>15u";
    options.leave_sequence = "\x1b[<u";
    if (getenv("KILIX_PONG_SKIP_PROBE")) options.probe_graphics = false;
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;
    framebuffer_active = true;
    shutdown_claimed = 0;
    release_capability_seen = false;
    *out_width = kittyfb_width(&framebuffer);
    *out_height = kittyfb_height(&framebuffer);
    return true;
}

bool term_check_resize(int *out_width, int *out_height)
{
    return framebuffer_active &&
           kittyfb_check_resize(&framebuffer, out_width, out_height);
}

/* ------------------------------- input ---------------------------------- */




static bool decimal_range(const char *text, size_t begin, size_t end, int *out)
{
    unsigned value = 0;
    if (!out || begin >= end) return false;
    for (size_t index = begin; index < end; index++) {
        unsigned char byte = (unsigned char)text[index];
        if (byte < '0' || byte > '9') return false;
        unsigned digit = (unsigned)(byte - '0');
        if (value > ((unsigned)INT_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *out = (int)value;
    return true;
}

static bool parse_modifiers_action(const char *text, size_t begin, size_t end,
                                   int *mods, KeyAction *action,
                                   bool *explicit_action)
{
    if (!mods || !action || !explicit_action) return false;
    *mods = 0;
    *action = KEY_ACTION_PRESS;
    *explicit_action = false;
    if (begin >= end) return true;

    size_t colon = end;
    for (size_t index = begin; index < end; index++) {
        if (text[index] == ':') {
            colon = index;
            break;
        }
    }

    int encoded_modifiers;
    if (!decimal_range(text, begin, colon, &encoded_modifiers) ||
        encoded_modifiers < 1 || encoded_modifiers > 256)
        return false;
    *mods = encoded_modifiers - 1;

    if (colon < end) {
        int encoded_action;
        if (!decimal_range(text, colon + 1, end, &encoded_action) ||
            encoded_action < KEY_ACTION_PRESS ||
            encoded_action > KEY_ACTION_RELEASE)
            return false;
        *action = (KeyAction)encoded_action;
        *explicit_action = true;
    }
    return true;
}

static int normalize_key(int key)
{
    switch (key) {
    case 13: return KEY_ENTER;
    case 9: return KEY_TAB;
    case 27: return KEY_ESC;
    case 127: return KEY_BACKSPACE;
    /* Kitty's functional-key code points when a terminal elects CSI-u for
       arrows instead of the traditional A/B/C/D final bytes. */
    case 57352: return KEY_UP;
    case 57353: return KEY_DOWN;
    case 57354: return KEY_RIGHT;
    case 57355: return KEY_LEFT;
    default: return key;
    }
}

static int legacy_final_key(unsigned char final)
{
    switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'Z': return KEY_TAB;
    default: return 0;
    }
}

static bool parse_csi_u(const char *parameters, size_t length, KeyEvent *event)
{
    size_t first_semicolon = length;
    size_t second_semicolon = length;
    for (size_t index = 0; index < length; index++) {
        if (parameters[index] == ';') {
            if (first_semicolon == length) first_semicolon = index;
            else { second_semicolon = index; break; }
        }
    }

    size_t first_colon = first_semicolon;
    for (size_t index = 0; index < first_semicolon; index++) {
        if (parameters[index] == ':') { first_colon = index; break; }
    }

    int key;
    if (!decimal_range(parameters, 0, first_colon, &key) || key > 0x10ffff)
        return false;

    int modifiers = 0;
    KeyAction action = KEY_ACTION_PRESS;
    bool explicit_action = false;
    if (first_semicolon < length &&
        !parse_modifiers_action(parameters, first_semicolon + 1,
                                second_semicolon, &modifiers, &action,
                                &explicit_action))
        return false;

    event->key = normalize_key(key);
    event->mods = modifiers;
    event->action = action;
    if (explicit_action) release_capability_seen = true;
    return true;
}

static bool parse_legacy_csi(const char *parameters, size_t length,
                             unsigned char final, KeyEvent *event)
{
    int key = legacy_final_key(final);
    if (!key) return false;

    size_t semicolon = length;
    for (size_t index = 0; index < length; index++) {
        if (parameters[index] == ';') { semicolon = index; break; }
    }

    int modifiers = final == 'Z' ? KEY_MOD_SHIFT : 0;
    KeyAction action = KEY_ACTION_PRESS;
    bool explicit_action = false;
    if (semicolon < length &&
        !parse_modifiers_action(parameters, semicolon + 1, length,
                                &modifiers, &action, &explicit_action))
        return false;

    event->key = key;
    event->mods = modifiers;
    event->action = action;
    if (explicit_action) release_capability_seen = true;
    return true;
}

static bool parse_csi(KeyEvent *event)
{
    char parameters[96];
    size_t length = 0;
    bool overflow = false;
    unsigned char final = 0;

    for (;;) {
        unsigned char byte;
        if (!read_byte_timeout(&byte, 25)) return false;
        if (byte >= 0x40 && byte <= 0x7e) {
            final = byte;
            break;
        }
        if (length < sizeof parameters) parameters[length++] = (char)byte;
        else overflow = true;
    }
    if (overflow) return false;
    if (final == 'u') return parse_csi_u(parameters, length, event);
    return parse_legacy_csi(parameters, length, final, event);
}

bool term_poll_event(KeyEvent *out)
{
    if (!out) return false;
    unsigned char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1) return false;

    *out = (KeyEvent){ .action = KEY_ACTION_PRESS };
    if (byte == 3) {
        out->key = 'c';
        out->mods = KEY_MOD_CTRL;
        return true;
    }
    if (byte == '\r' || byte == '\n') { out->key = KEY_ENTER; return true; }
    if (byte == 127 || byte == 8) { out->key = KEY_BACKSPACE; return true; }
    if (byte == '\t') { out->key = KEY_TAB; return true; }
    if (byte != 0x1b) { out->key = byte; return true; }

    unsigned char next;
    if (!read_byte_timeout(&next, 25)) { out->key = KEY_ESC; return true; }
    if (next == '[') return parse_csi(out);
    if (next == 'O') {
        unsigned char final;
        if (!read_byte_timeout(&final, 25)) return false;
        int key = legacy_final_key(final);
        if (!key) return false;
        out->key = key;
        return true;
    }

    /* A legacy Alt chord is ESC followed by the literal key. */
    out->key = normalize_key(next);
    out->mods = KEY_MOD_ALT;
    return true;
}

bool term_has_key_release(void)
{
    return release_capability_seen;
}
void term_present(const uint8_t *rgba, int w, int h)
{
    if (framebuffer_active)
        (void)kittyfb_present(&framebuffer, rgba, w, h);
}

static bool claim_shutdown(void)
{
    if (!framebuffer_active) return false;
    return !__sync_lock_test_and_set(&shutdown_claimed, 1);
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    if (!claim_shutdown()) return;
    kittyfb_emergency_restore(&framebuffer);
}
