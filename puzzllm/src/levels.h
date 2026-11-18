/* levels.h - the level list, read from levels/levels.txt.
 *
 * A level is data: a title, the goal text, hints, a room style and a check.
 * Adding a level means adding a block to the file, not code. Format: one
 * "key: value" per line, blocks separated by a "level:" line, '#' comments.
 *
 *   level: 1
 *   title: Hello, world
 *   room: stone
 *   goal: Get the agent to create hello_world.txt in the workspace.
 *   hint: ...                (repeatable, shown one at a time)
 *   check: file hello_world.txt
 *   require: nonempty        (repeatable; see below)
 *   require: contains-ci hello
 *
 * check: file NAME   NAME is a plain file name in the workspace: no '/', no
 *                    "..", not hidden. It must be a regular file (a symlink
 *                    does not count) no bigger than 64 KiB.
 * check: none        never met: a "coming soon" stub room.
 * require: nonempty | contains TEXT | contains-ci TEXT | max-bytes N
 */
#ifndef PUZZLLM_LEVELS_H
#define PUZZLLM_LEVELS_H

#include <stdbool.h>
#include <stddef.h>

enum {
    LEVEL_MAX = 64,
    LEVEL_HINT_MAX = 8,
    LEVEL_REQ_MAX = 8,
    LEVEL_TEXT = 256
};

typedef enum level_check_kind { CHECK_NONE = 0, CHECK_FILE } level_check_kind;
typedef enum level_req_kind {
    REQ_NONEMPTY = 1, REQ_CONTAINS, REQ_CONTAINS_CI, REQ_MAX_BYTES
} level_req_kind;

typedef struct level_req {
    level_req_kind kind;
    char text[LEVEL_TEXT];
    long number;
} level_req;

typedef struct level {
    int number;
    char title[LEVEL_TEXT];
    char goal[LEVEL_TEXT];
    char room[32];
    char hints[LEVEL_HINT_MAX][LEVEL_TEXT];
    int nhints;
    level_check_kind check;
    char file[LEVEL_TEXT];
    level_req reqs[LEVEL_REQ_MAX];
    int nreqs;
} level;

typedef struct level_list {
    level items[LEVEL_MAX];
    int count;
} level_list;

/* Parse text. On failure returns false and writes "line N: why" to err. */
bool levels_parse(level_list *out, const char *text, char *err, size_t errcap);
bool levels_load(level_list *out, const char *path, char *err, size_t errcap);

typedef struct goal_result {
    bool met;
    char why[LEVEL_TEXT];   /* what is missing, for the activity screen */
} goal_result;

/* Check a level against a workspace directory. */
goal_result levels_check(const level *lv, const char *workspace);

#endif
