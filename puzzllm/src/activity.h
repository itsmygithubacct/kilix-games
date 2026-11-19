/* activity.h - what the coding agent is doing, for the upper-right screen.
 *
 * Three sources, all polled (no threads, no inotify):
 *   - omp's newest session file under STATE/agent/sessions/: tool calls,
 *     tool results, what the model said and what the player typed;
 *   - the workspace listing: files created, changed and removed, whoever
 *     did it (agent or player shell);
 *   - STATE/agent/puzzllm/status, written by the sandbox while Qwen loads.
 */
#ifndef PUZZLLM_ACTIVITY_H
#define PUZZLLM_ACTIVITY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum { ACT_LINES = 64, ACT_LINE_LEN = 160, ACT_FILES = 128 };

typedef enum act_kind {
    ACT_INFO, ACT_YOU, ACT_SAYS, ACT_TOOL, ACT_RESULT, ACT_ERROR, ACT_FILE,
    ACT_GOAL
} act_kind;

typedef struct act_line {
    act_kind kind;
    char text[ACT_LINE_LEN];
} act_line;

typedef struct act_file {
    char name[128];
    off_t size;
    long long mtime_ns;
    bool seen;
} act_file;

typedef struct activity {
    bool codex;
    act_line lines[ACT_LINES];
    int head, count;               /* ring buffer */
    char session_path[4096];
    long long session_offset;
    char partial[262144];          /* incomplete trailing session line */
    size_t partial_len;
    act_file files[ACT_FILES];
    int nfiles;
    bool files_primed;
    char status[ACT_LINE_LEN];
    unsigned long generation;
} activity;

void activity_init(activity *a);
void activity_begin_room(activity *a, const char *state, const char *workspace);
void activity_add(activity *a, act_kind kind, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* i = 0 is the oldest kept line. */
const act_line *activity_line(const activity *a, int i);
/* Poll all sources once. state is the game's persistent folder. */
void activity_poll(activity *a, const char *state);
/* One session JSONL line (exposed for tests). */
void activity_session_line(activity *a, const char *line, size_t len);
/* Diff the workspace listing (exposed for tests). */
void activity_scan_workspace(activity *a, const char *workspace);

#endif
