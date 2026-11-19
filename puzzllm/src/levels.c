/* levels.c - see levels.h. */
#include "levels.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CHECK_FILE_CAP = 64 * 1024 };

static void set_err(char *err, size_t cap, int line, const char *why)
{
    if (err != NULL && cap > 0) snprintf(err, cap, "line %d: %s", line, why);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    return s;
}

static bool copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n == 0 || n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static bool safe_name(const char *name)
{
    if (name[0] == '\0' || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || (unsigned char)*p < 0x20) return false;
    }
    return true;
}

/* Split "a | b | c" into its trimmed parts. Returns how many, or 0 when
 * one is empty or there are too many. */
static int alternatives(const char *text, char out[][LEVEL_TEXT], int max)
{
    int n = 0;
    const char *p = text;
    for (;;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        char part[LEVEL_TEXT], *t;
        if (n >= max || len >= sizeof part) return 0;
        memcpy(part, p, len);
        part[len] = '\0';
        t = trim(part);
        if (*t == '\0') return 0;
        snprintf(out[n++], LEVEL_TEXT, "%s", t);
        if (bar == NULL) return n;
        p = bar + 1;
    }
}

static bool finish(level *lv, int line, char *err, size_t cap)
{
    if (lv->title[0] == '\0') { set_err(err, cap, line, "level has no title"); return false; }
    if (lv->goal[0] == '\0') { set_err(err, cap, line, "level has no goal"); return false; }
    if (lv->check == CHECK_FILE && lv->file[0] == '\0') {
        set_err(err, cap, line, "level has no check");
        return false;
    }
    if (lv->room[0] == '\0') snprintf(lv->room, sizeof lv->room, "stone");
    return true;
}

bool levels_parse(level_list *out, const char *text, char *err, size_t errcap)
{
    level *cur = NULL;
    int line_no = 0;
    bool have_check = false;
    const char *p = text;

    memset(out, 0, sizeof *out);
    while (*p) {
        char buf[1024];
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line, *colon, *key, *val;

        line_no++;
        if (len >= sizeof buf) { set_err(err, errcap, line_no, "line too long"); return false; }
        memcpy(buf, p, len);
        buf[len] = '\0';
        p = nl ? nl + 1 : p + len;

        line = trim(buf);
        if (line[0] == '\0' || line[0] == '#') continue;
        colon = strchr(line, ':');
        if (colon == NULL) { set_err(err, errcap, line_no, "expected 'key: value'"); return false; }
        *colon = '\0';
        key = trim(line);
        val = trim(colon + 1);

        if (strcmp(key, "level") == 0) {
            char *end;
            long n;
            if (cur != NULL) {
                if (!have_check) { set_err(err, errcap, line_no, "previous level has no check"); return false; }
                if (!finish(cur, line_no, err, errcap)) return false;
            }
            if (out->count >= LEVEL_MAX) { set_err(err, errcap, line_no, "too many levels"); return false; }
            n = strtol(val, &end, 10);
            if (*val == '\0' || *end != '\0' || n != out->count + 1) {
                set_err(err, errcap, line_no, "levels must be numbered 1, 2, 3 in order");
                return false;
            }
            cur = &out->items[out->count++];
            cur->number = (int)n;
            have_check = false;
            continue;
        }
        if (cur == NULL) { set_err(err, errcap, line_no, "field before the first 'level:'"); return false; }

        if (strcmp(key, "title") == 0) {
            if (!copy_field(cur->title, sizeof cur->title, val)) goto bad_value;
        } else if (strcmp(key, "goal") == 0) {
            if (!copy_field(cur->goal, sizeof cur->goal, val)) goto bad_value;
        } else if (strcmp(key, "room") == 0) {
            if (!copy_field(cur->room, sizeof cur->room, val)) goto bad_value;
        } else if (strcmp(key, "hint") == 0) {
            if (cur->nhints >= LEVEL_HINT_MAX) { set_err(err, errcap, line_no, "too many hints"); return false; }
            if (!copy_field(cur->hints[cur->nhints], LEVEL_TEXT, val)) goto bad_value;
            cur->nhints++;
        } else if (strcmp(key, "check") == 0) {
            if (have_check) { set_err(err, errcap, line_no, "second check in one level"); return false; }
            if (strcmp(val, "none") == 0) {
                cur->check = CHECK_NONE;
            } else if (strcmp(val, "chest") == 0) {
                cur->check = CHECK_CHEST;
            } else if (strncmp(val, "file ", 5) == 0) {
                char *name = trim(val + 5);
                if (!safe_name(name) || !copy_field(cur->file, sizeof cur->file, name)) {
                    set_err(err, errcap, line_no, "check file must be a plain, visible file name");
                    return false;
                }
                cur->check = CHECK_FILE;
            } else {
                set_err(err, errcap, line_no, "check must be 'file NAME', 'chest' or 'none'");
                return false;
            }
            have_check = true;
        } else if (strcmp(key, "require") == 0) {
            level_req *r;
            if (cur->nreqs >= LEVEL_REQ_MAX) { set_err(err, errcap, line_no, "too many requires"); return false; }
            r = &cur->reqs[cur->nreqs];
            if (strcmp(val, "nonempty") == 0) {
                r->kind = REQ_NONEMPTY;
            } else if (strncmp(val, "contains-any-ci ", 16) == 0 ||
                       strncmp(val, "lacks-ci ", 9) == 0) {
                bool any = val[0] == 'c';
                char parts[LEVEL_ALT_MAX][LEVEL_TEXT];
                r->kind = any ? REQ_CONTAINS_ANY_CI : REQ_LACKS_CI;
                if (!copy_field(r->text, sizeof r->text, trim(val + (any ? 16 : 9)))) goto bad_value;
                if (alternatives(r->text, parts, LEVEL_ALT_MAX) == 0) {
                    set_err(err, errcap, line_no, "expected 'TEXT | TEXT ...' with nothing empty");
                    return false;
                }
            } else if (strncmp(val, "contains-ci ", 12) == 0) {
                r->kind = REQ_CONTAINS_CI;
                if (!copy_field(r->text, sizeof r->text, trim(val + 12))) goto bad_value;
            } else if (strncmp(val, "equals ", 7) == 0) {
                r->kind = REQ_EQUALS;
                if (!copy_field(r->text, sizeof r->text, trim(val + 7))) goto bad_value;
            } else if (strncmp(val, "contains ", 9) == 0) {
                r->kind = REQ_CONTAINS;
                if (!copy_field(r->text, sizeof r->text, trim(val + 9))) goto bad_value;
            } else if (strncmp(val, "max-bytes ", 10) == 0 || strncmp(val, "bytes ", 6) == 0) {
                bool exact = val[0] == 'b';
                char *num = trim(val + (exact ? 6 : 10)), *end;
                r->kind = exact ? REQ_BYTES : REQ_MAX_BYTES;
                errno = 0;
                r->number = strtol(num, &end, 10);
                if (*num == '\0' || *end != '\0' || errno != 0 || r->number < (exact ? 0 : 1) ||
                    r->number > LEVEL_FILE_MAX) {
                    set_err(err, errcap, line_no, "byte count must be a whole number up to 16777216");
                    return false;
                }
            } else if (strncmp(val, "only ", 5) == 0) {
                r->kind = REQ_ONLY;
                if (!copy_field(r->text, sizeof r->text, trim(val + 5))) goto bad_value;
            } else {
                set_err(err, errcap, line_no, "unknown require");
                return false;
            }
            cur->nreqs++;
        } else {
            set_err(err, errcap, line_no, "unknown key");
            return false;
        }
        continue;
bad_value:
        set_err(err, errcap, line_no, "empty or too long value");
        return false;
    }
    if (cur == NULL) { set_err(err, errcap, line_no, "no levels"); return false; }
    if (!have_check) { set_err(err, errcap, line_no, "last level has no check"); return false; }
    return finish(cur, line_no, err, errcap);
}

bool levels_load(level_list *out, const char *path, char *err, size_t errcap)
{
    FILE *f = fopen(path, "rb");
    char *text;
    long n;
    bool ok;

    if (f == NULL) {
        if (err) snprintf(err, errcap, "%s: %s", path, strerror(errno));
        return false;
    }
    text = malloc(256 * 1024);
    if (text == NULL) { fclose(f); return false; }
    n = (long)fread(text, 1, 256 * 1024 - 1, f);
    fclose(f);
    text[n] = '\0';
    ok = levels_parse(out, text, err, errcap);
    free(text);
    return ok;
}

static bool contains(const char *hay, size_t n, const char *needle, bool ci)
{
    size_t m = strlen(needle);
    if (m == 0) return true;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        for (; j < m; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (ci) { a = (unsigned char)tolower(a); b = (unsigned char)tolower(b); }
            if (a != b) break;
        }
        if (j == m) return true;
    }
    return false;
}

/* 1000000 -> "1,000,000" */
static const char *grouped(long n, char *buf, size_t cap)
{
    char digits[32];
    int len = snprintf(digits, sizeof digits, "%ld", n < 0 ? -n : n), o = 0;
    if (n < 0 && (size_t)o + 1 < cap) buf[o++] = '-';
    for (int i = 0; i < len && (size_t)o + 2 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[o++] = ',';
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
    return buf;
}

/* A byte as a player would name it. */
static const char *byte_name(unsigned char b, char *buf, size_t cap)
{
    if (b == '\n') return "a line ending";
    if (b == '\r') return "a carriage return";
    if (b == ' ') return "a space";
    if (b == '\t') return "a tab";
    if (b > 0x20 && b < 0x7f) snprintf(buf, cap, "'%c'", b);
    else snprintf(buf, cap, "byte 0x%02x", b);
    return buf;
}

/* The most a level's file may hold before the check stops reading it. */
static long file_cap(const level *lv)
{
    long cap = CHECK_FILE_CAP;
    for (int i = 0; i < lv->nreqs; i++) {
        const level_req *q = &lv->reqs[i];
        if ((q->kind == REQ_BYTES || q->kind == REQ_MAX_BYTES) && q->number > cap)
            cap = q->number;
    }
    return cap;
}

goal_result levels_check(const level *lv, const char *workspace)
{
    goal_result r;
    char path[4096], a[32], b[32];
    char *data = NULL;
    struct stat st;
    long cap, n = 0;
    int fd;

    memset(&r, 0, sizeof r);
    if (lv->check == CHECK_NONE) {
        snprintf(r.why, sizeof r.why, "this room has no challenge yet");
        return r;
    }
    if (lv->check == CHECK_CHEST) {
        snprintf(r.why, sizeof r.why, "extract key.txt from chest.zip");
        return r;
    }
    snprintf(path, sizeof path, "%s/%s", workspace, lv->file);
    if (lstat(path, &st) != 0) {
        snprintf(r.why, sizeof r.why, "%.160s does not exist yet", lv->file);
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(r.why, sizeof r.why, "%.160s is not a regular file", lv->file);
        return r;
    }
    /* sizes first: they need no reading, and say the most */
    for (int i = 0; i < lv->nreqs; i++) {
        const level_req *q = &lv->reqs[i];
        if (q->kind == REQ_BYTES && st.st_size == q->number + 1) {
            snprintf(r.why, sizeof r.why, "%.120s has %s bytes, one too many: often a line "
                     "ending at the end", lv->file, grouped((long)st.st_size, a, sizeof a));
            return r;
        }
        if (q->kind == REQ_BYTES && st.st_size != q->number) {
            snprintf(r.why, sizeof r.why, "%.120s has %s bytes; it needs exactly %s", lv->file,
                     grouped((long)st.st_size, a, sizeof a), grouped(q->number, b, sizeof b));
            return r;
        }
        if (q->kind == REQ_MAX_BYTES && st.st_size > q->number) {
            snprintf(r.why, sizeof r.why, "%.160s is longer than %ld bytes", lv->file, q->number);
            return r;
        }
        if (q->kind == REQ_NONEMPTY && st.st_size == 0) {
            snprintf(r.why, sizeof r.why, "%.160s is empty", lv->file);
            return r;
        }
    }
    cap = file_cap(lv);
    if (st.st_size > cap) {
        snprintf(r.why, sizeof r.why, "%.160s is too big", lv->file);
        return r;
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        snprintf(r.why, sizeof r.why, "%.160s cannot be read", lv->file);
        return r;
    }
    data = malloc((size_t)cap + 1);
    if (data != NULL) {
        /* a file still being written may be longer by now: read at most cap */
        while (n < cap) {
            ssize_t got = read(fd, data + n, (size_t)(cap - n));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) { if (got < 0) n = -1; break; }
            n += got;
        }
    }
    close(fd);
    if (data == NULL || n < 0) {
        free(data);
        snprintf(r.why, sizeof r.why, "%.160s cannot be read", lv->file);
        return r;
    }
    for (int i = 0; i < lv->nreqs; i++) {
        const level_req *q = &lv->reqs[i];
        /* sizes again, on what was read: the file may have changed */
        if ((q->kind == REQ_BYTES && n != q->number) || (q->kind == REQ_MAX_BYTES && n > q->number) ||
            (q->kind == REQ_NONEMPTY && n == 0)) {
            snprintf(r.why, sizeof r.why, "%.160s changed while it was checked", lv->file);
            free(data);
            return r;
        }
        if ((q->kind == REQ_CONTAINS || q->kind == REQ_CONTAINS_CI) &&
            !contains(data, (size_t)n, q->text, q->kind == REQ_CONTAINS_CI)) {
            snprintf(r.why, sizeof r.why, "%.160s does not say \"%.60s\"", lv->file, q->text);
            free(data);
            return r;
        }
        if (q->kind == REQ_CONTAINS_ANY_CI || q->kind == REQ_LACKS_CI) {
            char parts[LEVEL_ALT_MAX][LEVEL_TEXT];
            int k = alternatives(q->text, parts, LEVEL_ALT_MAX), hit = -1;
            for (int j = 0; j < k && hit < 0; j++)
                if (contains(data, (size_t)n, parts[j], true)) hit = j;
            if (q->kind == REQ_CONTAINS_ANY_CI && hit < 0) {
                if (k == 1)
                    snprintf(r.why, sizeof r.why, "%.120s does not say \"%.100s\"", lv->file, parts[0]);
                else
                    snprintf(r.why, sizeof r.why, "%.120s says none of: %.120s", lv->file, q->text);
                free(data);
                return r;
            }
            if (q->kind == REQ_LACKS_CI && hit >= 0) {
                snprintf(r.why, sizeof r.why, "%.120s says \"%.100s\", which it must not",
                         lv->file, parts[hit]);
                free(data);
                return r;
            }
        }
        if (q->kind == REQ_EQUALS) {
            size_t first = 0, last = (size_t)n;
            while (first < last && isspace((unsigned char)data[first])) first++;
            while (last > first && isspace((unsigned char)data[last - 1])) last--;
            size_t length = last - first;
            if (length != strlen(q->text) || memcmp(data + first, q->text, length) != 0) {
                snprintf(r.why, sizeof r.why, "%.160s must contain only the correct answer", lv->file);
                free(data);
                return r;
            }
        }
        if (q->kind == REQ_ONLY) {
            for (long j = 0; j < n; j++) {
                if (memchr(q->text, data[j], strlen(q->text)) == NULL) {
                    char c[16];
                    snprintf(r.why, sizeof r.why, "%.80s has %s at byte %s; only %.40s may be in it",
                             lv->file, byte_name((unsigned char)data[j], c, sizeof c),
                             grouped(j + 1, a, sizeof a), q->text);
                    free(data);
                    return r;
                }
            }
        }
    }
    free(data);
    r.met = true;
    snprintf(r.why, sizeof r.why, "%.160s is there", lv->file);
    return r;
}
