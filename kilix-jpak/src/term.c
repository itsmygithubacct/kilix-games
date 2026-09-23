/* Adapter over the shared Kitty framebuffer/keyboard terminal session. */
#include "kilix_jpak.h"
#include "kitty_terminal_session.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static kittyts_session terminal;
static bool presentation_failed;

bool term_init(int *out_width, int *out_height)
{
    kittyts_options options;
    presentation_failed = false;
    kittyts_session_init(&terminal);
    kittyts_options_init(&options);
    options.framebuffer.min_width = 640;
    options.framebuffer.min_height = 400;
    options.framebuffer.max_width = 1600;
    options.framebuffer.max_height = 1000;
    if (getenv("KILIX_JPAK_SKIP_PROBE"))
        options.framebuffer.probe_graphics = false;
    if (kittyts_start(&terminal, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;
    *out_width = kittyts_width(&terminal);
    *out_height = kittyts_height(&terminal);
    return true;
}

bool term_check_resize(int *out_width, int *out_height)
{
    return kittyts_check_resize(&terminal, out_width, out_height);
}

void term_present(const uint8_t *rgba, int width, int height)
{
    if (!kittyts_present(&terminal, rgba, width, height))
        presentation_failed = true;
}

int term_read_input(void)
{
    if (presentation_failed) {
        errno = EIO;
        return -1;
    }
    return kittyts_read_input(&terminal);
}

bool term_next_key_event(kittykb_event *event)
{
    return kittyts_next_key_event(&terminal, event);
}

bool term_key_down(uint32_t key)
{
    return kittyts_key_down(&terminal, key);
}

bool term_has_release_events(void)
{
    return kittyts_has_release_events(&terminal);
}

void term_shutdown(void)
{
    kittyts_stop(&terminal);
    presentation_failed = false;
}

void term_emergency_restore(void)
{
    kittyts_emergency_restore(&terminal);
}
