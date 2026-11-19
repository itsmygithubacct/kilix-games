#include "history.h"

#include <stdlib.h>
#include <string.h>

typedef struct history_line {
    int cols;
    VTermScreenCell cells[];
} history_line;

static const VTermScreenCell blank = {
    .width = 1, .fg.type = VTERM_COLOR_DEFAULT_FG, .bg.type = VTERM_COLOR_DEFAULT_BG,
};

void history_clear(history *h)
{
    for (int i = 0; i < h->count; i++)
        free(h->lines[(h->head + i) % HISTORY_LINES]);
    h->head = h->count = h->offset = 0;
}

void history_scroll(history *h, int rows)
{
    if (h->alternate) return;
    int offset = h->offset + rows;
    h->offset = offset < 0 ? 0 : offset > h->count ? h->count : offset;
}

static int push(int cols, const VTermScreenCell *cells, void *user)
{
    history *h = user;
    history_line *line = malloc(sizeof *line + (size_t)cols * sizeof *cells);
    if (!line) return 0;
    line->cols = cols;
    memcpy(line->cells, cells, (size_t)cols * sizeof *cells);
    int slot = (h->head + h->count) % HISTORY_LINES;
    if (h->count == HISTORY_LINES) {
        free(h->lines[slot]);
        h->head = (h->head + 1) % HISTORY_LINES;
    } else {
        h->count++;
    }
    h->lines[slot] = line;
    if (h->offset > 0 && h->offset < h->count) h->offset++;
    return 1;
}

static VTermScreenCell line_cell(const history_line *line, int col, int width)
{
    if (col >= line->cols) return blank;
    VTermScreenCell cell = line->cells[col];
    if (cell.width == 2 && col + 1 >= width) return blank;
    return cell;
}

static int pop(int cols, VTermScreenCell *cells, void *user)
{
    history *h = user;
    if (!h->count) return 0;
    history_line *line = h->lines[(h->head + --h->count) % HISTORY_LINES];
    for (int i = 0; i < cols; i++) cells[i] = line_cell(line, i, cols);
    free(line);
    if (h->offset > h->count) h->offset = h->count;
    return 1;
}

static int clear(void *user)
{
    history_clear(user);
    return 1;
}

static int property(VTermProp prop, VTermValue *value, void *user)
{
    history *h = user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE: h->terminal->cursor_visible = value->boolean; break;
    case VTERM_PROP_CURSORSHAPE: h->terminal->cursor_shape = value->number; break;
    case VTERM_PROP_ALTSCREEN: h->alternate = value->boolean; h->offset = 0; break;
    case VTERM_PROP_MOUSE: h->mouse_mode = value->number; break;
    default: break;
    }
    return 1;
}

void history_attach(history *h, neuroterm *t)
{
    static const VTermScreenCallbacks callbacks = {
        .settermprop = property, .sb_pushline = push, .sb_popline = pop, .sb_clear = clear,
    };
    h->terminal = t;
    vterm_screen_set_callbacks(t->screen, &callbacks, h);
}

bool history_cell(const history *h, int col, int row, VTermScreenCell *cell)
{
    int index = row - h->offset;
    if (index >= 0) return neuroterm_cell(h->terminal, col, index, cell);
    const history_line *line = h->lines[(h->head + h->count + index) % HISTORY_LINES];
    *cell = line_cell(line, col, h->terminal->cols);
    return true;
}
