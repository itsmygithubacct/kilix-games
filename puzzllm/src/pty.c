/* pty.c - see pty.h. */
#define _XOPEN_SOURCE 700
#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void pty_init(pty_proc *p)
{
    p->fd = -1;
    p->pid = 0;
    p->exit_status = 0;
    p->exited = false;
}

bool pty_spawn(pty_proc *p, char *const argv[], char *const env[],
               int cols, int rows)
{
    int master;
    char *slave_name;
    pid_t pid;
    struct winsize ws;

    pty_init(p);
    master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return false;
    if (grantpt(master) != 0 || unlockpt(master) != 0 ||
        (slave_name = ptsname(master)) == NULL) {
        close(master);
        return false;
    }
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * 8);
    ws.ws_ypixel = (unsigned short)(rows * 16);

    pid = fork();
    if (pid < 0) {
        close(master);
        return false;
    }
    if (pid == 0) {
        int slave;
        struct termios tio;
        sigset_t none;

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGWINCH, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setsid();
        slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        ioctl(slave, TIOCSWINSZ, &ws);
        if (tcgetattr(slave, &tio) == 0) {
            tio.c_iflag |= IUTF8;
            tcsetattr(slave, TCSANOW, &tio);
        }
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        for (int i = 0; env != NULL && env[i] != NULL; i++)
            putenv(env[i]);
        execvp(argv[0], argv);
        _exit(127);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    p->fd = master;
    p->pid = pid;
    return true;
}

long pty_read(pty_proc *p, void *buf, size_t cap)
{
    ssize_t n;
    if (p->fd < 0) return 0;
    n = read(p->fd, buf, cap);
    if (n > 0) return (long)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;   /* EIO: the slave side is gone */
}

bool pty_write(pty_proc *p, const void *buf, size_t len)
{
    const char *s = buf;
    int spins = 0;
    if (p->fd < 0) return false;
    while (len > 0) {
        ssize_t n = write(p->fd, s, len);
        if (n > 0) {
            s += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR) && spins++ < 200) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        } else {
            return false;
        }
    }
    return true;
}

void pty_resize(pty_proc *p, int cols, int rows)
{
    struct winsize ws;
    if (p->fd < 0) return;
    memset(&ws, 0, sizeof ws);
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * 8);
    ws.ws_ypixel = (unsigned short)(rows * 16);
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

bool pty_poll_exit(pty_proc *p)
{
    int status;
    pid_t r;
    if (p->pid <= 0) return true;
    if (p->exited) return true;
    r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid) {
        p->exited = true;
        p->exit_status = status;
        return true;
    }
    return false;
}

void pty_stop(pty_proc *p)
{
    if (p->pid > 0 && !pty_poll_exit(p)) {
        /* Only the PID we started. bwrap passes SIGTERM on to its child and
         * --die-with-parent covers the rest of the sandbox. */
        kill(p->pid, SIGTERM);
        for (int i = 0; i < 100 && !pty_poll_exit(p); i++) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
        }
        if (!pty_poll_exit(p)) {
            kill(p->pid, SIGKILL);
            waitpid(p->pid, &p->exit_status, 0);
            p->exited = true;
        }
    }
    if (p->fd >= 0) close(p->fd);
    p->fd = -1;
}
