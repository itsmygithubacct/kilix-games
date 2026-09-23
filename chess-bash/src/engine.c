#define _GNU_SOURCE   /* pipe2 */
#include "chess_bash.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

/* one async startup/search job; only one side thinks at a time */
static struct {
    pthread_t thread;
    Engine *engine;
    char history[UCI_HISTORY_MAX];
    int movetime_ms;
    char move[8];
    atomic_int state;            /* ENGINE_PENDING/DONE/FAILED, idle = -1 */
    atomic_bool abort;
} job = {
    .state = ATOMIC_VAR_INIT(-1),
    .abort = ATOMIC_VAR_INIT(false),
};
static _Thread_local bool in_engine_worker;

static void engine_cancel(Engine *e);

static bool executable_in_path(const char *name)
{
    if (!name || !*name) return false;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = strdup(path);
    if (!copy) return false;
    bool found = false;
    for (char *p = copy, *tok; (tok = strsep(&p, ":")) != NULL;) {
        if (!*tok) tok = ".";
        char full[512];
        snprintf(full, sizeof full, "%s/%s", tok, name);
        if (access(full, X_OK) == 0) { found = true; break; }
    }
    free(copy);
    return found;
}

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

enum { READ_EOF = -1, READ_TIMEOUT = 0, READ_LINE = 1 };

/* Pull one '\n'-terminated line out of the engine's own read buffer.
 * Timeout and EOF stay distinct so a dead engine fails immediately instead
 * of turning a readable EOF descriptor into a CPU-burning timeout loop. */
static int read_line_timeout(Engine *e, char *buf, size_t len, int timeout_ms)
{
    double deadline = mono_ms() + timeout_ms;
    for (;;) {
        char *nl = memchr(e->rdbuf, '\n', (size_t)e->rdlen);
        if (nl) {
            size_t n = (size_t)(nl - e->rdbuf) + 1;
            size_t out = n < len ? n : len - 1;
            memcpy(buf, e->rdbuf, out);
            buf[out] = '\0';
            memmove(e->rdbuf, e->rdbuf + n, (size_t)e->rdlen - n);
            e->rdlen -= (int)n;
            return READ_LINE;
        }
        if ((size_t)e->rdlen >= sizeof e->rdbuf - 1)
            e->rdlen = 0;   /* pathological line; drop it */

        double left = deadline - mono_ms();
        if (left <= 0) return READ_TIMEOUT;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(e->out_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)((left - tv.tv_sec * 1000) * 1000);
        int rc = select(e->out_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) {
            if (rc < 0 && errno == EINTR) continue;
            return rc == 0 ? READ_TIMEOUT : READ_EOF;
        }
        ssize_t got = read(e->out_fd, e->rdbuf + e->rdlen,
                           sizeof e->rdbuf - 1 - (size_t)e->rdlen);
        if (got <= 0) return READ_EOF;   /* engine died */
        e->rdlen += (int)got;
    }
}

static bool write_cmd_bytes(Engine *e, const char *buf, size_t len,
                            bool allow_during_abort, int timeout_ms)
{
    double deadline = mono_ms() + timeout_ms;
    while (len > 0) {
        if (!allow_during_abort &&
            atomic_load_explicit(&job.abort, memory_order_acquire) &&
            job.engine == e)
            return false;
        ssize_t n = write(e->in_fd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        double left = deadline - mono_ms();
        if (left <= 0) return false;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(e->in_fd, &wfds);
        struct timeval tv = { 0, (suseconds_t)(left < 100.0 ? left * 1000.0 : 100000.0) };
        int rc = select(e->in_fd + 1, NULL, &wfds, NULL, &tv);
        if (rc < 0 && errno != EINTR) return false;
    }
    return true;
}

static bool send_cmd(Engine *e, const char *cmd)
{
    if (!e || e->in_fd < 0 || !cmd || !*cmd) return false;
    size_t len = strlen(cmd);
    if (!write_cmd_bytes(e, cmd, len, false, 2000)) return false;
    return cmd[len - 1] == '\n' || write_cmd_bytes(e, "\n", 1, false, 2000);
}

static bool send_stop(Engine *e)
{
    return e && e->in_fd >= 0 &&
           write_cmd_bytes(e, "stop\n", 5, true, 100);
}

static bool wait_token(Engine *e, const char *token, int timeout_ms)
{
    char line[512];
    double deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (atomic_load_explicit(&job.abort, memory_order_acquire) &&
            job.engine == e)
            return false;
        double left = deadline - mono_ms();
        if (left <= 0) return false;
        int got = read_line_timeout(e, line, sizeof line,
                                    (int)(left < 250 ? left : 250));
        if (got == READ_EOF) return false;
        if (got == READ_TIMEOUT)
            continue;
        if (!strncmp(line, "id name ", 8)) {
            strncpy(e->name, line + 8, sizeof e->name - 1);
            e->name[sizeof e->name - 1] = '\0';
            e->name[strcspn(e->name, "\r\n")] = '\0';
        }
        if (strstr(line, token)) return true;
    }
}

bool engine_start(Engine *e)
{
    memset(e, 0, sizeof *e);
    e->in_fd = -1;
    e->out_fd = -1;
    signal(SIGPIPE, SIG_IGN);

    const char *path = getenv("STOCKFISH");
    if (!path || !*path) path = "stockfish";
    if (!executable_in_path(path))
        return false;

    int to_child[2], from_child[2];
    if (pipe2(to_child, O_CLOEXEC) != 0)
        return false;
    if (pipe2(from_child, O_CLOEXEC) != 0) {
        close(to_child[0]); close(to_child[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }
    if (pid == 0) {
        if (to_child[0] == STDIN_FILENO)
            fcntl(STDIN_FILENO, F_SETFD, 0);
        else
            dup2(to_child[0], STDIN_FILENO);  /* dup2 clears CLOEXEC */
        if (from_child[1] == STDOUT_FILENO)
            fcntl(STDOUT_FILENO, F_SETFD, 0);
        else
            dup2(from_child[1], STDOUT_FILENO);
        if (from_child[1] == STDERR_FILENO)
            fcntl(STDERR_FILENO, F_SETFD, 0);
        else
            dup2(from_child[1], STDERR_FILENO);
        execlp(path, path, (char *)NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    e->pid = pid;
    e->in_fd = to_child[1];
    e->out_fd = from_child[0];
    e->rdlen = 0;
    int flags = fcntl(e->in_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(e->in_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        engine_stop(e);
        return false;
    }

    if (!send_cmd(e, "uci") || !wait_token(e, "uciok", 3500)) {
        engine_stop(e);
        return false;
    }
    send_cmd(e, "setoption name Threads value 1");
    send_cmd(e, "setoption name Hash value 32");
    if (!send_cmd(e, "isready") || !wait_token(e, "readyok", 2500)) {
        engine_stop(e);
        return false;
    }
    e->ok = true;
    if (!e->name[0])
        snprintf(e->name, sizeof e->name, "Stockfish");
    return true;
}

void engine_stop(Engine *e)
{
    if (!e) return;
    /* Be safe for the conventional `Engine e = {0}` initialization too. */
    if (e->pid == 0 && !e->ok && e->in_fd == 0 && e->out_fd == 0) {
        e->in_fd = e->out_fd = -1;
        return;
    }
    engine_cancel(e);   /* never close pipes under a live worker thread */
    if (e->in_fd >= 0) {
        /* Best effort only: closing stdin is itself a reliable quit signal,
         * and shutdown must not wait for a wedged engine's input pipe. */
        ssize_t ignored = write(e->in_fd, "quit\n", 5);
        (void)ignored;
        close(e->in_fd);
        e->in_fd = -1;
    }
    if (e->out_fd >= 0) {
        close(e->out_fd);
        e->out_fd = -1;
    }
    if (e->pid > 0) {
        int status;
        /* give it a moment to honor "quit", then escalate */
        for (int i = 0; i < 20; i++) {
            if (waitpid(e->pid, &status, WNOHANG) == e->pid) {
                e->pid = 0;
                break;
            }
            usleep(25 * 1000);
            if (i == 7) kill(e->pid, SIGTERM);
            if (i == 18) kill(e->pid, SIGKILL);
        }
        if (e->pid > 0) {
            waitpid(e->pid, &status, 0);
            e->pid = 0;
        }
    }
    e->ok = false;
}

bool engine_bestmove(Engine *e, const char *history, int movetime_ms,
                     char out_move[8], char *status, size_t status_len)
{
    if (!e || !e->ok) return false;
    /* sync point: an aborted search that outlived its drain window may
     * still owe us a bestmove; readyok is guaranteed to come after it,
     * so waiting here consumes any stale line instead of the new search
     * mistaking it for its own answer */
    if (!send_cmd(e, "isready") || !wait_token(e, "readyok", 3000)) {
        e->ok = false;
        return false;
    }
    /* Stockfish accepts Skill Level; other UCI engines are required to
     * ignore options they do not recognize.  Movetime still controls pace,
     * while this keeps Squire from secretly playing at full strength. */
    int skill = movetime_ms <= 150 ? 3 : movetime_ms <= 600 ? 10 : 18;
    char skill_cmd[64];
    snprintf(skill_cmd, sizeof skill_cmd, "setoption name Skill Level value %d", skill);
    if (!send_cmd(e, skill_cmd)) {
        e->ok = false;
        return false;
    }
    char cmd[UCI_HISTORY_MAX + 64];
    if (history && *history)
        snprintf(cmd, sizeof cmd, "position startpos moves %s", history);
    else
        snprintf(cmd, sizeof cmd, "position startpos");
    if (!send_cmd(e, cmd)) {
        e->ok = false;
        return false;
    }
    snprintf(cmd, sizeof cmd, "go movetime %d", movetime_ms > 0 ? movetime_ms : 450);
    if (!send_cmd(e, cmd)) {
        e->ok = false;
        return false;
    }

    char line[1024];
    bool stop_sent = false;
    double deadline = mono_ms() + (movetime_ms > 0 ? movetime_ms : 450) + 5500;
    for (;;) {
        double left = deadline - mono_ms();
        if (left <= 0) break;
        if (atomic_load_explicit(&job.abort, memory_order_acquire) &&
            job.engine == e && !stop_sent) {
            send_stop(e);
            stop_sent = true;
            /* drain the forced bestmove, then bail */
            double drain = mono_ms() + 600;
            if (drain < deadline) deadline = drain;
        }
        int got = read_line_timeout(e, line, sizeof line,
                                    (int)(left < 250 ? left : 250));
        if (got == READ_EOF) {
            e->ok = false;
            return false;
        }
        if (got == READ_TIMEOUT)
            continue;
        if (!strncmp(line, "info ", 5) && status && status_len) {
            char *depth = strstr(line, " depth ");
            char *score = strstr(line, " score ");
            if (depth || score) {
                snprintf(status, status_len, "%s analyzing", e->name[0] ? e->name : "Stockfish");
            }
        }
        if (!strncmp(line, "bestmove ", 9)) {
            char mv[8] = {0};
            if (sscanf(line + 9, "%7s", mv) == 1) {
                size_t len = strlen(mv);
                if (len == 4 || len == 5) {
                    memcpy(out_move, mv, len + 1);
                    return true;
                }
            }
            return false;
        }
    }
    send_stop(e);
    /* No bestmove was drained.  Reusing this process could let its delayed
     * reply masquerade as the next search result, so force a clean restart. */
    e->ok = false;
    return false;
}

/* ---------- async wrapper ---------- */

static void *job_main(void *arg)
{
    (void)arg;
    in_engine_worker = true;
    char mv[8] = {0};
    bool aborted = atomic_load_explicit(&job.abort, memory_order_acquire);
    bool ok = !aborted && (job.engine->ok || engine_start(job.engine));
    if (ok)
        ok = engine_bestmove(job.engine, job.history, job.movetime_ms,
                             mv, NULL, 0);
    if (!ok && !job.engine->ok)
        engine_stop(job.engine);
    if (ok && !atomic_load_explicit(&job.abort, memory_order_acquire)) {
        snprintf(job.move, sizeof job.move, "%s", mv);
        atomic_store_explicit(&job.state, ENGINE_DONE, memory_order_release);
    } else {
        atomic_store_explicit(&job.state, ENGINE_FAILED, memory_order_release);
    }
    in_engine_worker = false;
    return NULL;
}

bool engine_request(Engine *e, const char *history, int movetime_ms)
{
    if (!e || atomic_load_explicit(&job.state, memory_order_acquire) != -1)
        return false;
    job.engine = e;
    snprintf(job.history, sizeof job.history, "%s", history ? history : "");
    job.movetime_ms = movetime_ms;
    job.move[0] = '\0';
    atomic_store_explicit(&job.abort, false, memory_order_release);
    atomic_store_explicit(&job.state, ENGINE_PENDING, memory_order_release);
    if (pthread_create(&job.thread, NULL, job_main, NULL) != 0) {
        atomic_store_explicit(&job.state, -1, memory_order_release);
        return false;
    }
    return true;
}

int engine_poll(Engine *e, char out_move[8], char *status, size_t status_len)
{
    (void)status;
    (void)status_len;
    int state = atomic_load_explicit(&job.state, memory_order_acquire);
    if (job.engine != e || state == -1) return ENGINE_FAILED;
    if (state == ENGINE_PENDING) return ENGINE_PENDING;
    pthread_join(job.thread, NULL);
    int result = atomic_load_explicit(&job.state, memory_order_acquire);
    if (result == ENGINE_DONE)
        snprintf(out_move, 8, "%s", job.move);
    atomic_store_explicit(&job.state, -1, memory_order_release);
    job.engine = NULL;
    return result;
}

/* abort an in-flight search (if any) for this engine and wait it out; a
 * job that already finished but was never polled still needs its join.
 * The engine process stays alive and usable. */
static void engine_cancel(Engine *e)
{
    if (atomic_load_explicit(&job.state, memory_order_acquire) == -1 ||
        job.engine != e)
        return;
    /* Startup/search failures clean their own process from the worker. */
    if (in_engine_worker)
        return;
    atomic_store_explicit(&job.abort, true, memory_order_release);
    pthread_join(job.thread, NULL);
    atomic_store_explicit(&job.state, -1, memory_order_release);
    job.engine = NULL;
}

void engine_cancel_all(void)
{
    if (atomic_load_explicit(&job.state, memory_order_acquire) != -1 &&
        job.engine)
        engine_cancel(job.engine);
}
