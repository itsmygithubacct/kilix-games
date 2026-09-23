/* Terminal layer: kitty-framebuffer presenter + kitty-keyboard input glue.
 *
 * Presentation (raw mode, alternate screen, zlib/base64 kitty graphics
 * escapes, double-buffered image ids, the presenter thread and the
 * async-signal-safe restore) lives in the vendored kitty-framebuffer
 * library; this file only wires the game's term_* API onto it and keeps
 * the kitty-keyboard input decoding that was always separate.
 */
#include "kitty_brokeout.h"
#include "kitty_framebuffer.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <unistd.h>

static kittyfb_session session;
static kittykb_terminal keyboard;
static bool sessionActive = false;
static bool keyboardActive = false;

/* one-shot guard shared by the normal and signal-handler exit paths */
static volatile int shutdownClaimed = 0;

static bool claim_shutdown(void)
{
    if (!sessionActive) return false;
    return !__sync_lock_test_and_set(&shutdownClaimed, 1);
}

bool term_init(int *outW, int *outH)
{
    kittyfb_options fbOptions;
    kittykb_terminal_options keyboardOptions;

    kittyfb_options_init(&fbOptions);
    /* The defaults reproduce the presenter this game shipped with: size
     * clamped to 640x400..1600x1000, snapped to whole cells and even
     * pixels, centered with one cell row left for the shell prompt.  The
     * library additionally probes for graphics support, so terminals
     * without the kitty graphics protocol now fail here (ENOTSUP)
     * instead of running blind. */
    fbOptions.install_winch_handler = false; /* fixed size; no resize path */

    kittyfb_session_init(&session);
    if (kittyfb_start(&session, STDIN_FILENO, STDOUT_FILENO, &fbOptions) != 0)
        return false;
    sessionActive = true;
    shutdownClaimed = 0;

    /* the library owns raw mode and its probe drained the query replies,
     * so start the keyboard decoder without touching termios */
    kittykb_terminal_init(&keyboard);
    kittykb_terminal_options_init(&keyboardOptions);
    keyboardOptions.flags = KITTYKB_FLAGS_KEY_STATE;
    keyboardOptions.make_raw = false;
    keyboardOptions.make_nonblocking = false;
    if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                               &keyboardOptions) != 0) {
        int error = errno;
        kittyfb_stop(&session);
        sessionActive = false;
        errno = error;
        return false;
    }
    keyboardActive = true;

    *outW = kittyfb_width(&session);
    *outH = kittyfb_height(&session);
    return true;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    if (!sessionActive) return;
    (void)kittyfb_present(&session, rgba, w, h);
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    /* pop the keyboard mode first, inside the alternate screen it was
     * pushed on, then let the library restore everything else */
    if (keyboardActive) {
        (void)kittykb_terminal_stop(&keyboard);
        keyboardActive = false;
    }
    kittyfb_stop(&session);
    sessionActive = false;
}

/* async-signal path: no locks, no pthread_join.  The keyboard pop is a
 * single best-effort write; kittyfb_emergency_restore() then fences the
 * presenter thread and writes one prebuilt restore sequence. */
void term_emergency_restore(void)
{
    if (!claim_shutdown()) return;
    if (keyboardActive)
        (void)write(STDOUT_FILENO, "\x1b[<u", 4);
    kittyfb_emergency_restore(&session);
}

int term_read_input(void)
{
    if (!keyboardActive) {
        errno = EINVAL;
        return -1;
    }
    return kittykb_terminal_read(&keyboard);
}

bool term_next_key_event(kittykb_event *event)
{
    return keyboardActive && kittykb_input_next(&keyboard.input, event);
}

bool term_key_down(uint32_t key)
{
    return keyboardActive && kittykb_input_key_down(&keyboard.input, key);
}

bool term_has_release_events(void)
{
    return keyboardActive && kittykb_input_has_release_events(&keyboard.input);
}
