/* pty.h - one child process on a pseudo-terminal.
 *
 * pty_spawn forks a child in a new session with the pty as its controlling
 * terminal. The master is non-blocking. pty_stop signals only the PID this
 * object started, then reaps it.
 */
#ifndef PUZZLLM_PTY_H
#define PUZZLLM_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct pty_proc {
    int fd;          /* master, -1 when not running */
    pid_t pid;       /* child, 0 when none */
    int exit_status; /* waitpid status once exited */
    bool exited;
} pty_proc;

void pty_init(pty_proc *p);
/* argv[0] is looked up on PATH. env entries are added to the inherited
 * environment (NULL-terminated, may be NULL). */
bool pty_spawn(pty_proc *p, char *const argv[], char *const env[],
               int cols, int rows);
/* Bytes available now: >0 read, 0 nothing (or not running), -1 closed. */
long pty_read(pty_proc *p, void *buf, size_t cap);
bool pty_write(pty_proc *p, const void *buf, size_t len);
void pty_resize(pty_proc *p, int cols, int rows);
/* Non-blocking reap; true once the child has exited. */
bool pty_poll_exit(pty_proc *p);
void pty_stop(pty_proc *p);

#endif
