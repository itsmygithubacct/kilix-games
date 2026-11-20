/* kilix_embed.h - the private sandbox Kilix: one process, two panes.
 * Clicks, keys, and frames go through its bridge. Spawn-false tests never
 * start it. */
#ifndef PUZZLLM_KILIX_EMBED_H
#define PUZZLLM_KILIX_EMBED_H

#include "keys.h"
#include "pty.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

typedef struct kilix_embed {
    pty_proc proc;
    int bridge;                 /* -1 when not connected; never 0 */
    char state[2048];
    char pending[32768];
    size_t pending_len;
    uint32_t *agent_px, *shell_px;
    int agent_w, agent_h, shell_w, shell_h;
    struct timespec agent_mt, shell_mt;
    off_t agent_bytes, shell_bytes;
    int resize_w[2], resize_h[2];
    bool resize_sent;
    char error[160];
} kilix_embed;

void kilix_embed_init(kilix_embed *k);
void kilix_embed_stop(kilix_embed *k);
bool kilix_embed_running(const kilix_embed *k);
/* argv is puzzllm-kilix session [prompt]. env is NULL-terminated additions. */
bool kilix_embed_start(kilix_embed *k, const char *state,
                       char *const argv[], char *const env[]);
void kilix_embed_poll(kilix_embed *k);
bool kilix_embed_command(kilix_embed *k, const char *line);
void kilix_embed_focus(kilix_embed *k, int pane);
void kilix_embed_close_agent(kilix_embed *k);
void kilix_embed_click(kilix_embed *k, int pane, int lx, int ly,
                       int rw, int rh, int phase, int button);
void kilix_embed_scroll(kilix_embed *k, int pane, int rows, int lx, int ly,
                        int rw, int rh);
void kilix_embed_type(kilix_embed *k, int pane, const char *text, size_t len);
void kilix_embed_key(kilix_embed *k, int pane, const key_event *ev);
void kilix_embed_set_sizes(kilix_embed *k, int aw, int ah, int sw, int sh);
/* Replace *px with one P6 frame. 8-bit and 16-bit samples both load. */
bool kilix_embed_load_ppm(const char *path, uint32_t **px, int *w, int *h);

#endif
