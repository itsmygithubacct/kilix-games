/* kilix_embed.c - bridge client for the private sandbox Kilix. */
#define _GNU_SOURCE
#include "kilix_embed.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void kilix_embed_init(kilix_embed *k)
{
    memset(k, 0, sizeof *k);
    pty_init(&k->proc);
    k->bridge = -1;
}

static void free_frames(kilix_embed *k)
{
    free(k->agent_px);
    free(k->shell_px);
    k->agent_px = k->shell_px = NULL;
    k->agent_w = k->agent_h = k->shell_w = k->shell_h = 0;
    k->agent_bytes = k->shell_bytes = 0;
    memset(&k->agent_mt, 0, sizeof k->agent_mt);
    memset(&k->shell_mt, 0, sizeof k->shell_mt);
}

void kilix_embed_stop(kilix_embed *k)
{
    if (k->bridge >= 0) close(k->bridge);
    k->bridge = -1;
    k->pending_len = 0;
    pty_stop(&k->proc);
    pty_init(&k->proc);
    free_frames(k);
    k->resize_sent = false;
    k->error[0] = 0;
}

bool kilix_embed_running(const kilix_embed *k)
{
    return k->proc.pid > 0 && !k->proc.exited;
}

bool kilix_embed_start(kilix_embed *k, const char *state,
                       char *const argv[], char *const env[])
{
    if (k->proc.pid > 0 && !k->proc.exited) return true;
    if (k->proc.pid > 0) kilix_embed_stop(k);
    if (!state || snprintf(k->state, sizeof k->state, "%s", state) >= (int)sizeof k->state)
        return false;
    k->bridge = -1;
    k->pending_len = 0;
    k->resize_sent = false;
    k->error[0] = 0;
    return pty_spawn(&k->proc, argv, env, 80, 24);
}

static bool write_all(kilix_embed *k, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(k->bridge, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(k->bridge);
            k->bridge = -1;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool write_line(kilix_embed *k, const char *line)
{
    size_t n = strlen(line);
    return write_all(k, line, n) && write_all(k, "\n", 1);
}

static bool flush_pending(kilix_embed *k)
{
    if (!k->pending_len || k->bridge < 0) return k->pending_len == 0;
    if (!write_all(k, k->pending, k->pending_len) || !write_all(k, "\n", 1))
        return false;
    k->pending_len = 0;
    return true;
}

static bool queue_line(kilix_embed *k, const char *line)
{
    size_t n = strlen(line);
    size_t extra = k->pending_len ? 1 : 0;
    if (k->pending_len + extra + n >= sizeof k->pending) return false;
    if (extra) k->pending[k->pending_len++] = '\n';
    memcpy(k->pending + k->pending_len, line, n);
    k->pending_len += n;
    k->pending[k->pending_len] = 0;
    return true;
}

bool kilix_embed_command(kilix_embed *k, const char *line)
{
    if (!line || !line[0]) return false;
    if (k->bridge >= 0 && flush_pending(k) && write_line(k, line)) return true;
    return queue_line(k, line);
}

static const char *pane_name(int pane)
{
    return pane ? "shell" : "agent";
}

void kilix_embed_focus(kilix_embed *k, int pane)
{
    char line[32];
    snprintf(line, sizeof line, "focus %s", pane_name(pane));
    kilix_embed_command(k, line);
}

void kilix_embed_close_agent(kilix_embed *k)
{
    kilix_embed_command(k, "close-agent");
}

static void map_point(const kilix_embed *k, int pane, int lx, int ly,
                      int rw, int rh, int *x, int *y)
{
    int fw = pane ? k->shell_w : k->agent_w;
    int fh = pane ? k->shell_h : k->agent_h;
    if (fw <= 0) fw = rw > 0 ? rw : 1;
    if (fh <= 0) fh = rh > 0 ? rh : 1;
    if (rw <= 0) rw = fw;
    if (rh <= 0) rh = fh;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    *x = lx * fw / rw;
    *y = ly * fh / rh;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= fw) *x = fw - 1;
    if (*y >= fh) *y = fh - 1;
}

void kilix_embed_click(kilix_embed *k, int pane, int lx, int ly,
                       int rw, int rh, int phase, int button)
{
    int x, y;
    char line[80];
    map_point(k, pane, lx, ly, rw, rh, &x, &y);
    if (button < 1) button = 1;
    snprintf(line, sizeof line, "click %s %d,%d,%d,%d", pane_name(pane), x, y, phase, button);
    kilix_embed_command(k, line);
}

void kilix_embed_scroll(kilix_embed *k, int pane, int rows, int lx, int ly,
                        int rw, int rh)
{
    int x, y;
    char line[80];
    if (!rows) return;
    map_point(k, pane, lx, ly, rw, rh, &x, &y);
    snprintf(line, sizeof line, "scroll %s %d %d,%d", pane_name(pane), rows, x, y);
    kilix_embed_command(k, line);
}

static char b64_char(int v)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return tab[v & 63];
}

void kilix_embed_type(kilix_embed *k, int pane, const char *text, size_t len)
{
    char prefix[32];
    size_t prefix_len, out_len, i, o;
    char *line;
    int n;
    if (!text || !len) return;
    n = snprintf(prefix, sizeof prefix, "type %s ", pane_name(pane));
    if (n <= 0 || (size_t)n >= sizeof prefix) return;
    prefix_len = (size_t)n;
    out_len = 4 * ((len + 2) / 3);
    if (prefix_len + out_len + 1 >= sizeof k->pending) {
        len = ((sizeof k->pending - prefix_len - 8) / 4) * 3;
        out_len = 4 * ((len + 2) / 3);
    }
    line = malloc(prefix_len + out_len + 1);
    if (!line) return;
    memcpy(line, prefix, prefix_len);
    o = prefix_len;
    for (i = 0; i < len; i += 3) {
        unsigned b0 = (unsigned char)text[i];
        unsigned b1 = i + 1 < len ? (unsigned char)text[i + 1] : 0;
        unsigned b2 = i + 2 < len ? (unsigned char)text[i + 2] : 0;
        unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        line[o++] = b64_char((int)(triple >> 18));
        line[o++] = b64_char((int)(triple >> 12));
        line[o++] = i + 1 < len ? b64_char((int)(triple >> 6)) : '=';
        line[o++] = i + 2 < len ? b64_char((int)triple) : '=';
    }
    line[o] = 0;
    kilix_embed_command(k, line);
    free(line);
}

static bool key_ok(const char *s)
{
    if (!s || !s[0]) return false;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

static const char *functional_name(uint32_t key, char *buf, size_t cap)
{
    const char *name = NULL;
    switch (key) {
    case KEY_ESCAPE: name = "Escape"; break;
    case KEY_ENTER: name = "Return"; break;
    case KEY_TAB: name = "Tab"; break;
    case KEY_BACKSPACE: name = "BackSpace"; break;
    case KEY_INSERT: name = "Insert"; break;
    case KEY_DELETE: name = "Delete"; break;
    case KEY_LEFT: name = "Left"; break;
    case KEY_RIGHT: name = "Right"; break;
    case KEY_UP: name = "Up"; break;
    case KEY_DOWN: name = "Down"; break;
    case KEY_PAGE_UP: name = "Prior"; break;
    case KEY_PAGE_DOWN: name = "Next"; break;
    case KEY_HOME: name = "Home"; break;
    case KEY_END: name = "End"; break;
    default:
        if (key >= KEY_F1 && key <= KEY_F12) {
            snprintf(buf, cap, "F%d", (int)(key - KEY_F1) + 1);
            return buf;
        }
        break;
    }
    return name;
}

void kilix_embed_key(kilix_embed *k, int pane, const key_event *ev)
{
    char token[32], chord[80], line[112];
    const char *name;
    size_t used = 0;
    bool chorded;
    if (!ev || ev->action == 3 || (ev->action != 1 && ev->action != 2)) return;
    chorded = (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) != 0;
    if (!chorded && ev->text_len > 0) {
        char text[64];
        size_t n = 0;
        int i;
        for (i = 0; i < ev->text_len && n + 4 < sizeof text; i++)
            n += keys_utf8(ev->text[i], text + n);
        if (n) kilix_embed_type(k, pane, text, n);
        return;
    }
    name = functional_name(ev->key, token, sizeof token);
    if (!name && ev->key >= 32 && ev->key < 127) {
        unsigned char ch = (unsigned char)ev->key;
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
        token[0] = (char)ch;
        token[1] = 0;
        name = token;
    }
    if (!name) return;
    chord[0] = 0;
    if (ev->mods & MOD_CTRL) used += (size_t)snprintf(chord + used, sizeof chord - used, "ctrl+");
    if (ev->mods & MOD_ALT) used += (size_t)snprintf(chord + used, sizeof chord - used, "alt+");
    if (ev->mods & MOD_SHIFT) used += (size_t)snprintf(chord + used, sizeof chord - used, "shift+");
    if (ev->mods & MOD_SUPER) used += (size_t)snprintf(chord + used, sizeof chord - used, "super+");
    snprintf(chord + used, sizeof chord - used, "%s", name);
    if (!key_ok(chord)) return;
    snprintf(line, sizeof line, "key %s %s", pane_name(pane), chord);
    kilix_embed_command(k, line);
}

void kilix_embed_set_sizes(kilix_embed *k, int aw, int ah, int sw, int sh)
{
    if (aw < 1) aw = 1;
    if (ah < 1) ah = 1;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    k->resize_w[0] = aw;
    k->resize_h[0] = ah;
    k->resize_w[1] = sw;
    k->resize_h[1] = sh;
    k->resize_sent = false;
}

static void try_connect(kilix_embed *k)
{
    char path[4096];
    struct sockaddr_un addr;
    int fd, n;
    if (k->bridge >= 0) return;
    n = snprintf(path, sizeof path, "%s/kx/bridge.sock", k->state);
    if (n <= 0 || (size_t)n >= sizeof path || (size_t)n >= sizeof addr.sun_path) return;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, (size_t)n + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return;
    }
    k->bridge = fd;
}

static int ppm_token(FILE *f, char *tok, size_t cap)
{
    int c;
    size_t n = 0;
    do {
        c = fgetc(f);
        if (c == '#') {
            while (c != EOF && c != '\n') c = fgetc(f);
        }
    } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (c == EOF) return 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '#') {
        if (n + 1 < cap) tok[n++] = (char)c;
        c = fgetc(f);
    }
    tok[n] = 0;
    return n > 0;
}

static bool decode_ppm(FILE *f, uint32_t **px, int *w, int *h)
{
    char tok[64];
    int width, height, maxval, count, i, depth;
    unsigned char *raw;
    uint32_t *next;
    size_t bytes, got;
    if (!ppm_token(f, tok, sizeof tok) || strcmp(tok, "P6") != 0 ||
        !ppm_token(f, tok, sizeof tok))
        return false;
    width = atoi(tok);
    if (!ppm_token(f, tok, sizeof tok)) return false;
    height = atoi(tok);
    if (!ppm_token(f, tok, sizeof tok)) return false;
    maxval = atoi(tok);
    /* ImageMagick's import writes 16-bit samples (maxval 65535) from this
     * display. The high byte is the 8-bit channel. */
    if (maxval == 255) depth = 1;
    else if (maxval == 65535) depth = 2;
    else return false;
    if (width < 1 || height < 1 || width > 2048 || height > 1280) return false;
    count = width * height;
    bytes = (size_t)count * 3u * (size_t)depth;
    raw = malloc(bytes);
    next = malloc((size_t)count * sizeof *next);
    if (!raw || !next) {
        free(raw);
        free(next);
        return false;
    }
    got = fread(raw, 1, bytes, f);
    if (got != bytes) {
        free(raw);
        free(next);
        return false;
    }
    for (i = 0; i < count; i++) {
        size_t o = (size_t)i * 3u * (size_t)depth;
        unsigned r = raw[o];
        unsigned g = raw[o + (size_t)depth];
        unsigned b = raw[o + 2u * (size_t)depth];
        next[i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
    free(raw);
    free(*px);
    *px = next;
    *w = width;
    *h = height;
    return true;
}

bool kilix_embed_load_ppm(const char *path, uint32_t **px, int *w, int *h)
{
    FILE *f;
    bool ok;
    if (!path || !px || !w || !h) return false;
    f = fopen(path, "rb");
    if (!f) return false;
    ok = decode_ppm(f, px, w, h);
    fclose(f);
    return ok;
}

static bool load_ppm(const char *path, uint32_t **px, int *w, int *h,
                     struct timespec *stamp, off_t *bytes)
{
    struct stat st;
    FILE *f;
    if (stat(path, &st) != 0 || st.st_size < 16) return false;
    if (*px && stamp->tv_sec == st.st_mtim.tv_sec && stamp->tv_nsec == st.st_mtim.tv_nsec &&
        *bytes == st.st_size)
        return true;
    f = fopen(path, "rb");
    if (!f) return false;
    if (!decode_ppm(f, px, w, h)) {
        fclose(f);
        return false;
    }
    fclose(f);
    *stamp = st.st_mtim;
    *bytes = st.st_size;
    return true;
}

static void read_error(kilix_embed *k)
{
    char path[4096];
    FILE *f;
    if (k->error[0]) return;
    if (snprintf(path, sizeof path, "%s/kx/error", k->state) >= (int)sizeof path) return;
    f = fopen(path, "r");
    if (!f) return;
    if (fgets(k->error, sizeof k->error, f)) {
        size_t n = strlen(k->error);
        if (n && k->error[n - 1] == '\n') k->error[n - 1] = 0;
    }
    fclose(f);
}

void kilix_embed_poll(kilix_embed *k)
{
    char buf[4096];
    char path[4096];
    if (k->proc.pid <= 0) return;
    while (pty_read(&k->proc, buf, sizeof buf) > 0) {}
    if (pty_poll_exit(&k->proc)) {
        read_error(k);
        if (k->bridge >= 0) close(k->bridge);
        k->bridge = -1;
        return;
    }
    try_connect(k);
    if (k->bridge >= 0) flush_pending(k);
    if (k->bridge >= 0 && !k->resize_sent) {
        char line[80];
        snprintf(line, sizeof line, "resize agent %d %d", k->resize_w[0], k->resize_h[0]);
        kilix_embed_command(k, line);
        snprintf(line, sizeof line, "resize shell %d %d", k->resize_w[1], k->resize_h[1]);
        kilix_embed_command(k, line);
        k->resize_sent = true;
    }
    if (snprintf(path, sizeof path, "%s/kx/frames/agent.ppm", k->state) < (int)sizeof path)
        load_ppm(path, &k->agent_px, &k->agent_w, &k->agent_h, &k->agent_mt, &k->agent_bytes);
    if (snprintf(path, sizeof path, "%s/kx/frames/shell.ppm", k->state) < (int)sizeof path)
        load_ppm(path, &k->shell_px, &k->shell_w, &k->shell_h, &k->shell_mt, &k->shell_bytes);
}
