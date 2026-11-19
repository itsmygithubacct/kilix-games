/* In-canvas NeuroTerm terminal core. The host supplies input, PTY transport,
 * and the renderer. See third_party/libvterm/UPSTREAM for parser provenance. */
#ifndef PUZZLLM_NEUROTERM_H
#define PUZZLLM_NEUROTERM_H

#include "keys.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vterm.h>

#define NEUROTERM_FG 0xd0d4dcu
#define NEUROTERM_BG 0x101318u

typedef struct neuroterm {
    int cols, rows;
    VTerm *term;
    VTermScreen *screen;
    bool cursor_visible;
} neuroterm;

bool neuroterm_init(neuroterm *t, int cols, int rows);
void neuroterm_free(neuroterm *t);
bool neuroterm_resize(neuroterm *t, int cols, int rows);
void neuroterm_feed(neuroterm *t, const void *bytes, size_t len);
size_t neuroterm_read(neuroterm *t, char *buf, size_t cap);
size_t neuroterm_key(neuroterm *t, const key_event *event, char *buf, size_t cap);
VTermPos neuroterm_cursor(const neuroterm *t);
bool neuroterm_cell(const neuroterm *t, int col, int row, VTermScreenCell *cell);
uint32_t neuroterm_color(const neuroterm *t, VTermColor color);
size_t neuroterm_row_text(const neuroterm *t, int row, char *buf, size_t cap);

#endif
