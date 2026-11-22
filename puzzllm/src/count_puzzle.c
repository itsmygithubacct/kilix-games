#define _GNU_SOURCE
#include "count_puzzle.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t launch(const char *root, const char *state, const char *action, const char *extra, int *feedback)
{
    char sandbox[4096];
    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", root);
    int pipefd[2] = {-1, -1};
    if (feedback && pipe2(pipefd, O_CLOEXEC) != 0) return -1;
    pid_t pid = fork();
    if (pid != 0) {
        if (feedback) {
            close(pipefd[1]);
            if (pid < 0) close(pipefd[0]);
            else *feedback = pipefd[0];
        }
        return pid;
    }

    int quiet = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (quiet < 0 || dup2(quiet, STDIN_FILENO) < 0 ||
        dup2(feedback ? pipefd[1] : quiet, STDOUT_FILENO) < 0 || dup2(quiet, STDERR_FILENO) < 0)
        _exit(127);
    if (quiet > STDERR_FILENO) close(quiet);
    if (setenv("PUZZLLM_STATE", state, 1) != 0) _exit(127);
    if (extra != NULL && extra[0] != '\0')
        execl(sandbox, sandbox, action, extra, (char *)NULL);
    else
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

bool puzzle_prepare(const char *root, const char *state, const char *action)
{
    int status;
    return reap(launch(root, state, action, NULL, NULL), &status, 0) > 0 &&
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
    if (reap(launch(root, state, "count-check", NULL, NULL), &status, 0) <= 0)
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
        puzzle->grader = launch(root, state, "count-check", NULL, NULL);
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

static bool plain_script_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '/' || c == ' ') return false;
    }
    return true;
}

static bool script_file(const char *workspace, const char *name, struct stat *script)
{
    char path[4096];
    int n;
    if (!plain_script_name(name)) return false;
    n = snprintf(path, sizeof path, "%s/%s", workspace, name);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    return lstat(path, script) == 0 && S_ISREG(script->st_mode) && script->st_size <= 65536;
}

static goal_result script_outcome(int code)
{
    switch (code) {
    case 0: return (goal_result){true, "the script keeps strictly rising integers"};
    case 11: return (goal_result){false, "the script must be a regular file in the workspace"};
    case 12: return (goal_result){false, "the script fails an input that is not the sample"};
    case 13: return (goal_result){false, "the script fails negatives or a rise after a drop"};
    case 14: return (goal_result){false, "the script fails a longer rising run"};
    case 124:
    case 137: return (goal_result){false, "the script took too long"};
    default: return (goal_result){false, "could not run the sandboxed script check"};
    }
}

static goal_result script_checking(const char *name)
{
    goal_result r = {false, ""};
    snprintf(r.why, sizeof r.why, "checking %.200s", name != NULL ? name : "script");
    return r;
}

static goal_result missing_submission(const level *lv)
{
    if (lv->check == CHECK_SCRIPT) return script_outcome(11);
    goal_result r = {false, ""};
    snprintf(r.why, sizeof r.why, "write ~/%.160s as a regular file of at most 64 KiB", lv->file);
    return r;
}

static pid_t launch_submission(const char *root, const char *state, const level *lv, int *feedback)
{
    *feedback = -1;
    if (lv->check == CHECK_SCRIPT) return launch(root, state, "script-check", lv->file, NULL);
    return launch(root, state, "challenge-check", lv->file, feedback);
}

static goal_result submission_result(int code, int feedback)
{
    if (feedback < 0) return script_outcome(code);
    goal_result result = {false, ""};
    ssize_t n;
    do { n = read(feedback, result.why, sizeof result.why - 1); } while (n < 0 && errno == EINTR);
    close(feedback);
    if (n <= 0) return (goal_result){false, "could not run the campaign grader"};
    result.why[strcspn(result.why, "\r\n")] = 0;
    result.met = code == 0;
    return result;
}

goal_result script_puzzle_check(const char *root, const char *state, const char *workspace,
                                const level *lv)
{
    struct stat script;
    int status, feedback;
    if (!script_file(workspace, lv->file, &script)) return missing_submission(lv);
    pid_t pid = launch_submission(root, state, lv, &feedback);
    if (pid <= 0) return (goal_result){false, "could not start the grader"};
    pid_t done = reap(pid, &status, 0);
    return submission_result(done > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1, feedback);
}

goal_result script_puzzle_poll(script_puzzle *puzzle, const char *root, const char *state,
                               const char *workspace, const level *lv)
{
    struct stat script;
    bool ready = script_file(workspace, lv->file, &script);
    bool changed = !ready || !puzzle->checked || !same(&script, &puzzle->script);
    if (puzzle->grader > 0) {
        int status;
        pid_t done = reap(puzzle->grader, &status, WNOHANG);
        if (done == 0)
            return ready ? script_checking(lv->file) : missing_submission(lv);
        puzzle->grader = 0;
        int code = done > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        puzzle->result = submission_result(code, puzzle->feedback);
        if (code == 75) puzzle->checked = false;
    }
    if (!ready) {
        puzzle->checked = false;
        return missing_submission(lv);
    }
    if (changed) {
        puzzle->grader = launch_submission(root, state, lv, &puzzle->feedback);
        if (puzzle->grader <= 0) {
            puzzle->checked = false;
            return (goal_result){false, "could not start the grader"};
        }
        puzzle->checked = true;
        puzzle->script = script;
        puzzle->result = script_checking(lv->file);
    }
    return puzzle->result;
}

void script_puzzle_stop(script_puzzle *puzzle)
{
    if (puzzle->grader > 0) {
        int status;
        kill(puzzle->grader, SIGTERM);
        reap(puzzle->grader, &status, 0);
        if (puzzle->feedback >= 0) close(puzzle->feedback);
    }
    *puzzle = (script_puzzle){0};
}
