#include "count_puzzle.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t launch(const char *root, const char *state, const char *action)
{
    char sandbox[4096];
    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", root);
    pid_t pid = fork();
    if (pid != 0) return pid;

    int quiet = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (quiet < 0 || dup2(quiet, STDIN_FILENO) < 0 ||
        dup2(quiet, STDOUT_FILENO) < 0 || dup2(quiet, STDERR_FILENO) < 0)
        _exit(127);
    if (quiet > STDERR_FILENO) close(quiet);
    if (setenv("PUZZLLM_STATE", state, 1) != 0) _exit(127);
    execl(sandbox, sandbox, action, (char *)NULL);
    _exit(127);
}

static pid_t reap(pid_t pid, int *status, int flags)
{
    if (pid <= 0) return -1;
    pid_t done;
    do { done = waitpid(pid, status, flags); } while (done < 0 && errno == EINTR);
    return done;
}

bool count_puzzle_prepare(const char *root, const char *state)
{
    int status;
    return reap(launch(root, state, "count-setup"), &status, 0) > 0 &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static goal_result outcome(int code)
{
    switch (code) {
    case 0: return (goal_result){true, "the script counts regular files correctly"};
    case 11: return (goal_result){false, "count_files.sh and sample files must be in the workspace"};
    case 12: return (goal_result){false, "count_files.sh did not print the correct count for sample files"};
    case 13: return (goal_result){false, "count_files.sh must also work with other folder paths"};
    case 124:
    case 137: return (goal_result){false, "count_files.sh took too long"};
    default: return (goal_result){false, "could not run the sandboxed Bash check"};
    }
}

static bool inputs(const char *workspace, struct stat *script, struct stat *folder)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/count_files.sh", workspace);
    if (lstat(path, script) != 0 || !S_ISREG(script->st_mode) || script->st_size > 65536)
        return false;
    snprintf(path, sizeof path, "%s/sample files", workspace);
    return lstat(path, folder) == 0 && S_ISDIR(folder->st_mode);
}

static bool same(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

goal_result count_puzzle_check(const char *root, const char *state, const char *workspace)
{
    struct stat script, folder;
    if (!inputs(workspace, &script, &folder)) return outcome(11);
    int status;
    if (reap(launch(root, state, "count-check"), &status, 0) <= 0)
        return (goal_result){false, "could not start the Bash check"};
    return outcome(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

goal_result count_puzzle_poll(count_puzzle *puzzle, const char *root, const char *state,
                              const char *workspace)
{
    struct stat script, folder;
    bool ready = inputs(workspace, &script, &folder);
    bool changed = !ready || !puzzle->checked ||
                   !same(&script, &puzzle->script) || !same(&folder, &puzzle->folder);
    if (puzzle->grader > 0) {
        int status;
        pid_t done = reap(puzzle->grader, &status, WNOHANG);
        if (done == 0)
            return ready ? (goal_result){false, "checking count_files.sh"} : outcome(11);
        puzzle->grader = 0;
        puzzle->result = outcome(done > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    if (!ready) {
        puzzle->checked = false;
        return outcome(11);
    }
    if (changed) {
        puzzle->grader = launch(root, state, "count-check");
        if (puzzle->grader <= 0) {
            puzzle->checked = false;
            return (goal_result){false, "could not start the Bash check"};
        }
        puzzle->checked = true;
        puzzle->script = script;
        puzzle->folder = folder;
        puzzle->result = (goal_result){false, "checking count_files.sh"};
    }
    return puzzle->result;
}

void count_puzzle_stop(count_puzzle *puzzle)
{
    if (puzzle->grader > 0) {
        int status;
        kill(puzzle->grader, SIGTERM);
        reap(puzzle->grader, &status, 0);
    }
    *puzzle = (count_puzzle){0};
}
