/* Scrollback for an embedded NeuroTerm pane. */
#ifndef PUZZLLM_HISTORY_H
#define PUZZLLM_HISTORY_H

#include "neuroterm_terminal.h"

enum { HISTORY_LINES = 1000 };

struct history_line;
typedef struct history {
    neuroterm *terminal;
    struct history_line *lines[HISTORY_LINES];
    int head, count, offset;
    int mouse_mode;
    bool alternate;
} history;

void history_attach(history *h, neuroterm *t);
void history_clear(history *h);
void history_scroll(history *h, int rows);
bool history_cell(const history *h, int col, int row, VTermScreenCell *cell);

#endif
