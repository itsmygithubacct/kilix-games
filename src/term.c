/* Terminal input plus presentation through kitty-framebuffer. */
#include "kilix_pong.h"
#include "kitty_framebuffer.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static kittykb_terminal keyboard;
static bool framebuffer_active;
static bool keyboard_active;
static volatile int shutdown_claimed;
bool term_init(int *out_width, int *out_height)
{
    kittyfb_options options;
    kittykb_terminal_options key_options;

    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&options);
    options.min_width = 320;
    options.min_height = 180;
    options.max_width = 1440;
    options.max_height = 900;
    if (getenv("KILIX_PONG_SKIP_PROBE")) options.probe_graphics = false;
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;
    framebuffer_active = true;
    shutdown_claimed = 0;
    kittykb_terminal_init(&keyboard);
    kittykb_terminal_options_init(&key_options);
    key_options.flags = KITTYKB_FLAGS_KEY_STATE;
    key_options.make_raw = false;
    key_options.make_nonblocking = false;
    if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                               &key_options) != 0) {
        int error = errno;
        kittyfb_stop(&framebuffer);
        framebuffer_active = false;
        errno = error;
        return false;
    }
    keyboard_active = true;
    *out_width = kittyfb_width(&framebuffer);
    *out_height = kittyfb_height(&framebuffer);
    return true;
}

bool term_check_resize(int *out_width, int *out_height)
{
    return framebuffer_active &&
           kittyfb_check_resize(&framebuffer, out_width, out_height);
}

static int game_key(uint32_t key)
{
    switch (key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    default: return key <= 0x10ffffU ? (int)key : 0;
    }
}

bool term_poll_event(KeyEvent *out)
{
    kittykb_event event;
    if (!out || !keyboard_active || kittykb_terminal_read(&keyboard) < 0 ||
        !kittykb_input_next(&keyboard.input, &event))
        return false;
    out->key = game_key(event.key);
    out->mods = (int)event.modifiers;
    out->action = (KeyAction)event.action;
    return true;
}

bool term_has_key_release(void)
{
    return keyboard_active &&
           kittykb_input_has_release_events(&keyboard.input);
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
    if (keyboard_active) {
        (void)kittykb_terminal_stop(&keyboard);
        keyboard_active = false;
    }
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    static const char keyboard_pop[] = "\x1b\\\x1b[<u";

    if (!claim_shutdown()) return;
    if (keyboard_active)
        (void)write(STDOUT_FILENO, keyboard_pop, sizeof keyboard_pop - 1);
    kittyfb_emergency_restore(&framebuffer);
}
