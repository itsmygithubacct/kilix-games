/* json.h - a small bounded JSON reader for omp's session lines.
 *
 * Parses one document into an arena of nodes. Strings are decoded to UTF-8
 * (\uXXXX and surrogate pairs included). Numbers are kept as doubles. Depth
 * is capped so a hostile line cannot recurse without bound.
 */
#ifndef PUZZLLM_JSON_H
#define PUZZLLM_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum json_type {
    JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUMBER, JSON_STRING,
    JSON_ARRAY, JSON_OBJECT
} json_type;

typedef struct json_node {
    json_type type;
    const char *key;     /* member name inside an object, else NULL */
    const char *str;     /* JSON_STRING value */
    double num;
    int first_child;     /* index, -1 if none */
    int next;            /* next sibling index, -1 if none */
} json_node;

typedef struct json_doc {
    json_node *nodes;
    int count, cap;
    char *strings;       /* decoded strings, NUL-separated */
    size_t slen, scap;
} json_doc;

enum { JSON_MAX_DEPTH = 32 };

/* Returns the root index, or -1 on a parse error. */
int json_parse(json_doc *d, const char *text, size_t len);
void json_free(json_doc *d);
/* Member of an object by name, or -1. */
int json_get(const json_doc *d, int obj, const char *key);
/* String value of node, or NULL if it is not a string. */
const char *json_str(const json_doc *d, int node);

#endif
