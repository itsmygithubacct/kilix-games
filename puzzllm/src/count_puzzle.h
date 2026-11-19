/* Install Room 5's starter files and run its Bash grader out of process. */
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

bool count_puzzle_prepare(const char *root, const char *state);
goal_result count_puzzle_check(const char *root, const char *state, const char *workspace);
goal_result count_puzzle_poll(count_puzzle *puzzle, const char *root, const char *state,
                              const char *workspace);
void count_puzzle_stop(count_puzzle *puzzle);

#endif
