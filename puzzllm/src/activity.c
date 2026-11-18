/* activity.c - see activity.h. */
#include "activity.h"
#include "json.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void activity_init(activity *a)
{
    memset(a, 0, sizeof *a);
}

static void clean_line(char *s)
{
    /* One display line: newlines and tabs become spaces, controls go. */
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 0x20 || c == 0x7f) continue;
        *w++ = (char)c;
    }
    *w = '\0';
}

void activity_add(activity *a, act_kind kind, const char *fmt, ...)
{
    act_line *l;
    va_list ap;
    int slot;

    if (a->count < ACT_LINES) {
        slot = (a->head + a->count) % ACT_LINES;
        a->count++;
    } else {
        slot = a->head;
        a->head = (a->head + 1) % ACT_LINES;
    }
    l = &a->lines[slot];
    l->kind = kind;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof l->text, fmt, ap);
    va_end(ap);
    clean_line(l->text);
    a->generation++;
}

const act_line *activity_line(const activity *a, int i)
{
    if (i < 0 || i >= a->count) return NULL;
    return &a->lines[(a->head + i) % ACT_LINES];
}

/* First line of s, at most n bytes, cut on a UTF-8 boundary. */
static void first_line(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    while (s[n] && s[n] != '\n' && n + 1 < cap) n++;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80 && s[n] != '\0' &&
           s[n] != '\n')
        n--;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static const char *tool_target(const json_doc *d, int args)
{
    static const char *keys[] = {"path", "command", "file_path", "pattern",
                                 "i", "url", "query", NULL};
    for (int i = 0; keys[i]; i++) {
        const char *s = json_str(d, json_get(d, args, keys[i]));
        if (s && s[0]) return s;
    }
    return "";
}

static void content_blocks(activity *a, const json_doc *d, int msg,
                           const char *role)
{
    int content = json_get(d, msg, "content");
    char text[ACT_LINE_LEN];
    bool is_error = false;
    int err = json_get(d, msg, "isError");

    if (err >= 0 && d->nodes[err].type == JSON_TRUE) is_error = true;
    if (content >= 0 && d->nodes[content].type == JSON_STRING) {
        first_line(text, sizeof text, json_str(d, content));
        if (strcmp(role, "user") == 0) activity_add(a, ACT_YOU, "you: %s", text);
        return;
    }
    if (content < 0 || d->nodes[content].type != JSON_ARRAY) return;
    for (int b = d->nodes[content].first_child; b >= 0; b = d->nodes[b].next) {
        const char *type = json_str(d, json_get(d, b, "type"));
        if (type == NULL) continue;
        if (strcmp(type, "text") == 0) {
            const char *s = json_str(d, json_get(d, b, "text"));
            if (s == NULL || s[0] == '\0') continue;
            first_line(text, sizeof text, s);
            if (strcmp(role, "user") == 0)
                activity_add(a, ACT_YOU, "you: %s", text);
            else if (strcmp(role, "assistant") == 0)
                activity_add(a, ACT_SAYS, "agent: %s", text);
            else if (strcmp(role, "toolResult") == 0) {
                /* omp prefixes some results with a "[name#hash]" line */
                const char *body = s;
                if (body[0] == '[') {
                    const char *nl = strchr(body, '\n');
                    if (nl && nl[1]) body = nl + 1;
                }
                first_line(text, sizeof text, body);
                activity_add(a, is_error ? ACT_ERROR : ACT_RESULT, "  -> %s", text);
            }
        } else if (strcmp(type, "toolCall") == 0) {
            const char *name = json_str(d, json_get(d, b, "name"));
            int args = json_get(d, b, "arguments");
            first_line(text, sizeof text, tool_target(d, args));
            activity_add(a, ACT_TOOL, "tool %s %s", name ? name : "?", text);
        }
    }
}

void activity_session_line(activity *a, const char *line, size_t len)
{
    json_doc d;
    int root, msg;
    const char *type, *role;

    root = json_parse(&d, line, len);
    if (root < 0) {
        json_free(&d);
        return;
    }
    type = json_str(&d, json_get(&d, root, "type"));
    if (type != NULL && strcmp(type, "message") == 0) {
        msg = json_get(&d, root, "message");
        role = json_str(&d, json_get(&d, msg, "role"));
        if (role != NULL) content_blocks(a, &d, msg, role);
    } else if (type != NULL && strcmp(type, "model_change") == 0) {
        const char *m = json_str(&d, json_get(&d, root, "model"));
        if (m) activity_add(a, ACT_INFO, "model: %s", m);
    }
    json_free(&d);
}

static bool newest_session(const char *state, char *out, size_t cap)
{
    char root[3000];
    DIR *top;
    struct dirent *e;
    long long best = -1;

    snprintf(root, sizeof root, "%s/agent/sessions", state);
    top = opendir(root);
    if (top == NULL) return false;
    while ((e = readdir(top)) != NULL) {
        char sub[4096];
        DIR *dir;
        struct dirent *f;
        if (e->d_name[0] == '.') continue;
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", root, e->d_name) >= sizeof sub) continue;
        dir = opendir(sub);
        if (dir == NULL) continue;
        while ((f = readdir(dir)) != NULL) {
            char path[4096];
            struct stat st;
            size_t n = strlen(f->d_name);
            if (n < 7 || strcmp(f->d_name + n - 6, ".jsonl") != 0) continue;
            if ((size_t)snprintf(path, sizeof path, "%s/%s", sub, f->d_name) >= sizeof path)
                continue;
            if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            {
                long long t = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
                if (t > best && strlen(path) < cap) {
                    best = t;
                    memcpy(out, path, strlen(path) + 1);
                }
            }
        }
        closedir(dir);
    }
    closedir(top);
    return best >= 0;
}

static void poll_session(activity *a, const char *state)
{
    char path[4096];
    char buf[16384];
    int fd;
    ssize_t n;

    if (!newest_session(state, path, sizeof path)) return;
    if (strcmp(path, a->session_path) != 0) {
        snprintf(a->session_path, sizeof a->session_path, "%s", path);
        a->session_offset = 0;
        a->partial_len = 0;
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    if (lseek(fd, (off_t)a->session_offset, SEEK_SET) < 0) {
        close(fd);
        return;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        a->session_offset += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                activity_session_line(a, a->partial, a->partial_len);
                a->partial_len = 0;
            } else if (a->partial_len + 1 < sizeof a->partial) {
                a->partial[a->partial_len++] = buf[i];
            } else {
                a->partial_len = sizeof a->partial; /* too long: drop line */
            }
        }
        if (a->partial_len >= sizeof a->partial) a->partial_len = sizeof a->partial - 1;
    }
    close(fd);
}

void activity_scan_workspace(activity *a, const char *workspace)
{
    DIR *dir = opendir(workspace);
    struct dirent *e;

    if (dir == NULL) return;
    for (int i = 0; i < a->nfiles; i++) a->files[i].seen = false;
    while ((e = readdir(dir)) != NULL) {
        char path[4096];
        struct stat st;
        long long mt;
        int i;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strlen(e->d_name) >= sizeof a->files[0].name) continue;
        snprintf(path, sizeof path, "%s/%s", workspace, e->d_name);
        if (lstat(path, &st) != 0) continue;
        mt = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        for (i = 0; i < a->nfiles; i++)
            if (strcmp(a->files[i].name, e->d_name) == 0) break;
        if (i == a->nfiles) {
            if (a->nfiles >= ACT_FILES) continue;
            a->nfiles++;
            snprintf(a->files[i].name, sizeof a->files[i].name, "%s", e->d_name);
            if (a->files_primed)
                activity_add(a, ACT_FILE, "new %s%s (%lld bytes)", e->d_name,
                             S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
        } else if (a->files[i].mtime_ns != mt || a->files[i].size != st.st_size) {
            activity_add(a, ACT_FILE, "changed %s (%lld bytes)", e->d_name,
                         (long long)st.st_size);
        }
        a->files[i].size = st.st_size;
        a->files[i].mtime_ns = mt;
        a->files[i].seen = true;
    }
    closedir(dir);
    for (int i = 0; i < a->nfiles;) {
        if (!a->files[i].seen) {
            activity_add(a, ACT_FILE, "removed %s", a->files[i].name);
            a->files[i] = a->files[--a->nfiles];
        } else {
            i++;
        }
    }
    a->files_primed = true;
}

static void poll_status(activity *a, const char *state)
{
    char path[4096], buf[ACT_LINE_LEN];
    FILE *f;
    snprintf(path, sizeof path, "%s/agent/puzzllm/status", state);
    f = fopen(path, "r");
    if (f == NULL) return;
    if (fgets(buf, sizeof buf, f) != NULL) {
        buf[strcspn(buf, "\n")] = '\0';
        if (strcmp(buf, a->status) != 0) {
            snprintf(a->status, sizeof a->status, "%s", buf);
            activity_add(a, ACT_INFO, "%s", buf);
        }
    }
    fclose(f);
}

void activity_poll(activity *a, const char *state)
{
    char ws[4096];
    snprintf(ws, sizeof ws, "%s/workspace", state);
    poll_status(a, state);
    poll_session(a, state);
    activity_scan_workspace(a, ws);
}
