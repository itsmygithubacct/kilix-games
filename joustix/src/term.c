/* Game-facing adapter over the shared terminal-session lifecycle. */
#include "joustix.h"
#include "kitty_terminal_session.h"

#include <stdlib.h>
#include <unistd.h>

static kittyts_session terminal;

bool term_init(int *out_w, int *out_h)
{
    kittyts_options options;
    kittyts_session_init(&terminal);
    kittyts_options_init(&options);
    options.framebuffer.min_width = 480;
    options.framebuffer.min_height = 270;
    options.framebuffer.max_width = 1440;
    options.framebuffer.max_height = 900;
    if (getenv("JOUSTIX_SKIP_PROBE"))
        options.framebuffer.probe_graphics = false;
    if (kittyts_start(&terminal, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;
    *out_w = kittyts_width(&terminal);
    *out_h = kittyts_height(&terminal);
    return true;
}

bool term_check_resize(int *out_w, int *out_h)
{
    return kittyts_check_resize(&terminal, out_w, out_h);
}

void term_present(const uint8_t *rgba, int w, int h)
{
    (void)kittyts_present(&terminal, rgba, w, h);
}

int term_read_input(void)
{
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
}

void term_emergency_restore(void)
{
    kittyts_emergency_restore(&terminal);
}
