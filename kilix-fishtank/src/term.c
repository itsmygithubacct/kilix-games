/* Terminal, framebuffer, keyboard, and mouse glue over one shared session. */
#include "fishtank.h"
#include "kitty_terminal_session.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

static kittyts_session terminal;
static bool terminal_active;
static int cell_width = 9, cell_height = 18;
static int origin_column = 1, origin_row = 1;

static void update_mouse_geometry(int width, int height)
{
    struct winsize window;
    int columns = 80, rows = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_col > 0) columns = window.ws_col;
        if (window.ws_row > 0) rows = window.ws_row;
    }
    cell_width = kittyts_cell_width(&terminal);
    cell_height = kittyts_cell_height(&terminal);
    if (cell_width < 1) cell_width = 1;
    if (cell_height < 1) cell_height = 1;

    int image_columns = (width + cell_width - 1) / cell_width;
    int image_rows = (height + cell_height - 1) / cell_height;
    origin_column = 1 + (columns - image_columns) / 2;
    origin_row = 1 + ((rows - 1) - image_rows) / 2;
    if (origin_column < 1) origin_column = 1;
    if (origin_row < 1) origin_row = 1;
}

bool term_init(int *outW, int *outH)
{
    kittyts_options options;

    if (outW == NULL || outH == NULL) {
        errno = EINVAL;
        return false;
    }
    kittyts_session_init(&terminal);
    kittyts_options_init(&options);
    options.framebuffer.install_winch_handler = false;
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    /* Cell coordinates remain portable to terminals without SGR pixels. */
    options.pixel_mouse = false;
    options.focus_events = true;
    if (kittyts_start(&terminal, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;

    terminal_active = true;
    *outW = kittyts_width(&terminal);
    *outH = kittyts_height(&terminal);
    update_mouse_geometry(*outW, *outH);
    return true;
}

void term_present(const uint8_t *rgba, int width, int height)
{
    if (terminal_active)
        (void)kittyts_present(&terminal, rgba, width, height);
}

void term_shutdown(void)
{
    if (!terminal_active) return;
    kittyts_stop(&terminal);
    terminal_active = false;
}

void term_emergency_restore(void)
{
    if (!terminal_active) return;
    kittyts_emergency_restore(&terminal);
    terminal_active = false;
}

static void update_mouse(const kittyin_mouse_event *mouse)
{
    int x = (mouse->x - (origin_column - 1)) * cell_width;
    int y = (mouse->y - (origin_row - 1)) * cell_height;

    if (G.W > 0) x = (int)clampf((float)x, 0.0f, (float)(G.W - 1));
    if (G.H > 0) y = (int)clampf((float)y, 0.0f, (float)(G.H - 1));
    G.mouseX = x;
    G.mouseY = y;
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
    default: return key <= 0x7fU ? (int)key : -1;
    }
}

int term_poll_key(void)
{
    kittyin_event event;

    if (!terminal_active) return -1;
    if (kittyts_read_input(&terminal) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK)
        return -1;

    while (kittyts_next_event(&terminal, &event)) {
        if (event.kind == KITTYIN_EVENT_MOUSE) {
            const kittyin_mouse_event *mouse = &event.data.mouse;
            update_mouse(mouse);
            if (mouse->action == KITTYIN_MOUSE_PRESS && mouse->button == 1u)
                return KEY_MOUSE;
            return KEY_MOUSE_MOVE;
        }
        if (event.kind != KITTYIN_EVENT_KEY ||
            event.data.key.action == KITTYKB_ACTION_RELEASE)
            continue;
        if ((event.data.key.modifiers & KITTYKB_MOD_CTRL) &&
            (event.data.key.key == 'c' || event.data.key.key == 'C')) {
            G.quit = true;
            return -1;
        }
        int key = game_key(event.data.key.key);
        if (key >= 0) return key;
    }
    return -1;
}
