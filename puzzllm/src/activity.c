/* activity.c - see activity.h. */
#include "activity.h"
#include "json.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
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

static void codex_item(activity *a, const json_doc *d, int item)
{
    const char *kind = json_str(d, json_get(d, item, "type"));
    if (!kind) return;
    if (strcmp(kind, "message") == 0) {
        const char *role = json_str(d, json_get(d, item, "role"));
        if (!role || strcmp(role, "assistant") != 0) return;
        int parts = json_get(d, item, "content");
        if (parts < 0 || d->nodes[parts].type != JSON_ARRAY) return;
        for (int i = d->nodes[parts].first_child; i >= 0; i = d->nodes[i].next) {
            const char *text = json_str(d, json_get(d, i, "text"));
            if (text && *text) {
                char line[ACT_LINE_LEN];
                first_line(line, sizeof line, text);
                activity_add(a, ACT_SAYS, "agent: %s", line);
            }
        }
    } else if (strcmp(kind, "function_call") == 0 || strcmp(kind, "custom_tool_call") == 0) {
        const char *name = json_str(d, json_get(d, item, "name"));
        if (name) activity_add(a, ACT_TOOL, "tool %s", name);
    } else if (strcmp(kind, "function_call_output") == 0 ||
               strcmp(kind, "custom_tool_call_output") == 0) {
        activity_add(a, ACT_RESULT, "tool returned");
    }
}

static bool token_value(const json_doc *d, int usage, const char *key,
                        unsigned long long *value)
{
    int node = json_get(d, usage, key);
    if (node < 0 || d->nodes[node].type != JSON_NUMBER) return false;
    double num = d->nodes[node].num;
    if (!(num >= 0 && num <= 9007199254740991.0)) return false;
    *value = (unsigned long long)num;
    return num == (double)*value;
}

static void record_usage(activity *a, const json_doc *d, int usage, bool cumulative)
{
    unsigned long long total = 0;
    bool reported = token_value(d, usage, cumulative ? "total_tokens" : "totalTokens", &total);
    if (!reported && !cumulative) {
        static const char *const fields[] = {"input", "output", "cacheRead", "cacheWrite"};
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
            unsigned long long value;
            if (token_value(d, usage, fields[i], &value)) {
                total += value;
                reported = true;
            }
        }
    }
    if (!reported) return;
    a->tokens_reported = true;
    if (cumulative) a->tokens_used = total;
    else if (ULLONG_MAX - a->tokens_used >= total) a->tokens_used += total;
    else a->tokens_used = ULLONG_MAX;
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
    if (a->codex) {
        if (type && strcmp(type, "response_item") == 0)
            codex_item(a, &d, json_get(&d, root, "payload"));
        else if (type && strcmp(type, "turn_context") == 0) {
            int payload = json_get(&d, root, "payload");
            const char *model = json_str(&d, json_get(&d, payload, "model"));
            if (model) snprintf(a->codex_model, sizeof a->codex_model, "%s", model);
        }
        else if (type && strcmp(type, "event_msg") == 0) {
            int payload = json_get(&d, root, "payload");
            const char *event = json_str(&d, json_get(&d, payload, "type"));
            if (event && strcmp(event, "token_count") == 0) {
                int info = json_get(&d, payload, "info");
                record_usage(a, &d, json_get(&d, info, "total_token_usage"), true);
            }
        }
    } else if (type != NULL && strcmp(type, "message") == 0) {
        msg = json_get(&d, root, "message");
        role = json_str(&d, json_get(&d, msg, "role"));
        if (role && strcmp(role, "assistant") == 0)
            record_usage(a, &d, json_get(&d, msg, "usage"), false);
        if (role != NULL) content_blocks(a, &d, msg, role);
    } else if (type != NULL && strcmp(type, "model_change") == 0) {
        const char *m = json_str(&d, json_get(&d, root, "model"));
        if (m) activity_add(a, ACT_INFO, "model: %s", m);
    }
    json_free(&d);
}

static void find_session(const char *root, int depth, char *out, size_t cap, long long *best)
{
    DIR *dir = opendir(root);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[4096];
        struct stat st;
        const char *name = entry->d_name;
        if (name[0] == '.') continue;
        int length = snprintf(path, sizeof path, "%s/%s", root, name);
        if (length < 0 || (size_t)length >= sizeof path || lstat(path, &st) != 0) continue;
        if (depth > 0 && S_ISDIR(st.st_mode)) {
            find_session(path, depth - 1, out, cap, best);
            continue;
        }
        size_t n = strlen(name);
        if (!S_ISREG(st.st_mode) || n < 7 || strcmp(name + n - 6, ".jsonl") != 0) continue;
        long long time_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        if (time_ns > *best && (size_t)length < cap) {
            *best = time_ns;
            memcpy(out, path, (size_t)length + 1);
        }
    }
    closedir(dir);
}

static bool newest_session(const activity *a, const char *state, char *out, size_t cap)
{
    char root[4096];
    long long best = -1;
    snprintf(root, sizeof root, "%s/%s/sessions", state,
             a->codex ? "codex" : a->pi ? "pi" : "agent");
    find_session(root, a->codex ? 3 : 1, out, cap, &best);
    return best >= 0;
}

void activity_begin_session(activity *a, const char *state)
{
    struct stat st;
    a->tokens_used = 0;
    a->tokens_reported = false;
    a->codex_model[0] = 0;
    a->partial_len = 0;
    a->status[0] = 0;
    a->session_path[0] = 0;
    a->session_offset = 0;
    if (newest_session(a, state, a->session_path, sizeof a->session_path) &&
        stat(a->session_path, &st) == 0)
        a->session_offset = st.st_size;
}

void activity_begin_room(activity *a, const char *state, const char *workspace)
{
    a->head = a->count = 0;
    a->generation++;
    activity_begin_session(a, state);
    a->nfiles = 0;
    a->files_primed = false;
    activity_scan_workspace(a, workspace);
}

static void poll_session(activity *a, const char *state)
{
    char path[4096];
    char buf[16384];
    int fd;
    ssize_t n;

    if (!newest_session(a, state, path, sizeof path)) return;
    if (strcmp(path, a->session_path) != 0) {
        a->tokens_used = 0;
        a->tokens_reported = false;
        a->codex_model[0] = 0;
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
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, "shell.log") == 0) continue;
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
    snprintf(path, sizeof path, "%s/%s/puzzllm/status", state, a->pi ? "pi" : "agent");
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
    if (!a->codex) poll_status(a, state);
    poll_session(a, state);
    activity_scan_workspace(a, ws);
}
