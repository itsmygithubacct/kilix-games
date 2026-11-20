/* Install the file-count puzzle's starter files and run its Bash grader out of process. */
#ifndef PUZZLLM_COUNT_PUZZLE_H
#define PUZZLLM_COUNT_PUZZLE_H

#include "levels.h"
#include <sys/stat.h>
#include <sys/types.h>

typedef struct count_puzzle {
    pid_t grader;
    bool checked;
    struct stat script, folder;
    goal_result result;
} count_puzzle;

/* The script room grades one workspace file. It reruns only when that file changes. */
typedef struct script_puzzle {
    pid_t grader;
    bool checked;
    struct stat script;
    goal_result result;
} script_puzzle;

bool puzzle_prepare(const char *root, const char *state, const char *action);
goal_result count_puzzle_check(const char *root, const char *state, const char *workspace);
goal_result count_puzzle_poll(count_puzzle *puzzle, const char *root, const char *state,
                              const char *workspace);
void count_puzzle_stop(count_puzzle *puzzle);
goal_result script_puzzle_check(const char *root, const char *state, const char *workspace,
                                const char *name);
goal_result script_puzzle_poll(script_puzzle *puzzle, const char *root, const char *state,
                               const char *workspace, const char *name);
void script_puzzle_stop(script_puzzle *puzzle);

#endif
