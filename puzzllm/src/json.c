/* json.c - see json.h. */
#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct parser {
    const char *p, *end;
    json_doc *d;
    bool bad;
} parser;

static void ws(parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int new_node(parser *ps, json_type t)
{
    json_doc *d = ps->d;
    if (d->count == d->cap) {
        int ncap = d->cap ? d->cap * 2 : 64;
        json_node *n = realloc(d->nodes, (size_t)ncap * sizeof *n);
        if (n == NULL) { ps->bad = true; return -1; }
        d->nodes = n;
        d->cap = ncap;
    }
    memset(&d->nodes[d->count], 0, sizeof d->nodes[0]);
    d->nodes[d->count].type = t;
    d->nodes[d->count].first_child = -1;
    d->nodes[d->count].next = -1;
    return d->count++;
}

/* Strings are stored as offsets until parsing ends, because the buffer may
 * move when it grows; json_parse fixes the pointers up at the end. */
static bool put_byte(parser *ps, char c)
{
    json_doc *d = ps->d;
    if (d->slen + 1 >= d->scap) {
        size_t ncap = d->scap ? d->scap * 2 : 1024;
        char *n = realloc(d->strings, ncap);
        if (n == NULL) { ps->bad = true; return false; }
        d->strings = n;
        d->scap = ncap;
    }
    d->strings[d->slen++] = c;
    return true;
}

static void put_cp(parser *ps, unsigned long cp)
{
    if (cp < 0x80) put_byte(ps, (char)cp);
    else if (cp < 0x800) {
        put_byte(ps, (char)(0xc0 | (cp >> 6)));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        put_byte(ps, (char)(0xe0 | (cp >> 12)));
        put_byte(ps, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    } else {
        put_byte(ps, (char)(0xf0 | (cp >> 18)));
        put_byte(ps, (char)(0x80 | ((cp >> 12) & 0x3f)));
        put_byte(ps, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put_byte(ps, (char)(0x80 | (cp & 0x3f)));
    }
}

static int hex4(parser *ps, unsigned long *out)
{
    unsigned long v = 0;
    if (ps->end - ps->p < 4) return -1;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return -1;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* Returns the offset of the decoded string in d->strings, or -1. */
static long parse_string(parser *ps)
{
    long start = (long)ps->d->slen;
    if (ps->p >= ps->end || *ps->p != '"') return -1;
    ps->p++;
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char)c < 0x20) return -1;
        if (c != '\\') { put_byte(ps, c); continue; }
        if (ps->p >= ps->end) return -1;
        c = *ps->p++;
        switch (c) {
        case '"': put_byte(ps, '"'); break;
        case '\\': put_byte(ps, '\\'); break;
        case '/': put_byte(ps, '/'); break;
        case 'b': put_byte(ps, '\b'); break;
        case 'f': put_byte(ps, '\f'); break;
        case 'n': put_byte(ps, '\n'); break;
        case 'r': put_byte(ps, '\r'); break;
        case 't': put_byte(ps, '\t'); break;
        case 'u': {
            unsigned long cp, lo;
            if (hex4(ps, &cp) != 0) return -1;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (hex4(ps, &lo) != 0) return -1;
                    if (lo >= 0xdc00 && lo <= 0xdfff)
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    else
                        cp = 0xfffd;
                } else {
                    cp = 0xfffd;
                }
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                cp = 0xfffd;
            }
            put_cp(ps, cp);
            break;
        }
        default: return -1;
        }
    }
    if (ps->p >= ps->end) return -1;
    ps->p++;
    put_byte(ps, '\0');
    return ps->bad ? -1 : start;
}

static int parse_value(parser *ps, int depth);

static int parse_container(parser *ps, int depth, bool object)
{
    int node = new_node(ps, object ? JSON_OBJECT : JSON_ARRAY);
    int last = -1;
    if (node < 0) return -1;
    ps->p++;
    ws(ps);
    if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) {
        ps->p++;
        return node;
    }
    for (;;) {
        long key = -1;
        int child;
        ws(ps);
        if (object) {
            key = parse_string(ps);
            if (key < 0) return -1;
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') return -1;
            ps->p++;
        }
        child = parse_value(ps, depth + 1);
        if (child < 0) return -1;
        /* key holds an offset now; turned into a pointer in json_parse */
        ps->d->nodes[child].key = object ? (const char *)(key + 1) : NULL;
        if (last < 0) ps->d->nodes[node].first_child = child;
        else ps->d->nodes[last].next = child;
        last = child;
        ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) { ps->p++; return node; }
        return -1;
    }
}

static int parse_value(parser *ps, int depth)
{
    int node;
    if (depth > JSON_MAX_DEPTH) return -1;
    ws(ps);
    if (ps->p >= ps->end) return -1;
    switch (*ps->p) {
    case '{': return parse_container(ps, depth, true);
    case '[': return parse_container(ps, depth, false);
    case '"': {
        long off = parse_string(ps);
        if (off < 0) return -1;
        node = new_node(ps, JSON_STRING);
        if (node < 0) return -1;
        ps->d->nodes[node].str = (const char *)(off + 1);
        return node;
    }
    case 't':
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
            ps->p += 4;
            return new_node(ps, JSON_TRUE);
        }
        return -1;
    case 'f':
        if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
            ps->p += 5;
            return new_node(ps, JSON_FALSE);
        }
        return -1;
    case 'n':
        if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
            ps->p += 4;
            return new_node(ps, JSON_NULL);
        }
        return -1;
    default: {
        char buf[64];
        size_t n = 0;
        char *endp;
        while (ps->p < ps->end && n + 1 < sizeof buf &&
               (strchr("+-.eE", *ps->p) || (*ps->p >= '0' && *ps->p <= '9')))
            buf[n++] = *ps->p++;
        buf[n] = '\0';
        if (n == 0) return -1;
        node = new_node(ps, JSON_NUMBER);
        if (node < 0) return -1;
        ps->d->nodes[node].num = strtod(buf, &endp);
        if (*endp != '\0') return -1;
        return node;
    }
    }
}

int json_parse(json_doc *d, const char *text, size_t len)
{
    parser ps;
    int root;
    memset(d, 0, sizeof *d);
    ps.p = text;
    ps.end = text + len;
    ps.d = d;
    ps.bad = false;
    root = parse_value(&ps, 0);
    ws(&ps);
    if (root < 0 || ps.bad || ps.p != ps.end) return -1;
    /* Offsets were stored +1 so that 0 stays "no string". */
    for (int i = 0; i < d->count; i++) {
        if (d->nodes[i].key)
            d->nodes[i].key = d->strings + ((size_t)d->nodes[i].key - 1);
        if (d->nodes[i].str)
            d->nodes[i].str = d->strings + ((size_t)d->nodes[i].str - 1);
    }
    return root;
}

void json_free(json_doc *d)
{
    free(d->nodes);
    free(d->strings);
    memset(d, 0, sizeof *d);
}

int json_get(const json_doc *d, int obj, const char *key)
{
    if (obj < 0 || obj >= d->count || d->nodes[obj].type != JSON_OBJECT) return -1;
    for (int c = d->nodes[obj].first_child; c >= 0; c = d->nodes[c].next)
        if (d->nodes[c].key && strcmp(d->nodes[c].key, key) == 0) return c;
    return -1;
}

const char *json_str(const json_doc *d, int node)
{
    if (node < 0 || node >= d->count || d->nodes[node].type != JSON_STRING) return NULL;
    return d->nodes[node].str;
}
