/* main.c - puzzllm: Kilix in a room with a coding agent.
 *
 *   puzzllm                       play (needs a Kitty-graphics terminal)
 *   puzzllm --version
 *   puzzllm --render DIR          write the room as PPMs (no terminal needed)
 *   puzzllm --check-level N       print whether level N's goal is met now
 *
 * PUZZLLM_ROOT is the puzzllm/ directory (bin/puzzllm sets it) and
 * PUZZLLM_STATE the persistent folder (default ~/.local/share/puzzllm).
 * PUZZLLM_SNAPSHOT=FILE writes the live frame to FILE (PPM) every 2 s.
 */
#include "game.h"
#include "layout.h"
#include "termio.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PUZZLLM_VERSION
#define PUZZLLM_VERSION "0.1.0"
#endif

static game the_game;
static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void on_crash(int sig)
{
    termio_emergency_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void default_paths(char *root, size_t rcap, char *state, size_t scap)
{
    const char *r = getenv("PUZZLLM_ROOT");
    const char *s = getenv("PUZZLLM_STATE");
    if (r && r[0]) snprintf(root, rcap, "%s", r);
    else snprintf(root, rcap, ".");
    if (s && s[0]) snprintf(state, scap, "%s", s);
    else {
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (xdg && xdg[0]) snprintf(state, scap, "%s/puzzllm", xdg);
        else snprintf(state, scap, "%s/.local/share/puzzllm", home ? home : ".");
    }
}

static void on_key(void *ctx, const key_event *ev) { game_key(ctx, ev); }
static void on_click(void *ctx, int x, int y) { game_click(ctx, x, y); }

static int play(const char *root, const char *state)
{
    char err[512];
    sr_canvas canvas;
    termio_handlers h = {on_key, on_click, &the_game};
    double last, next_snapshot = 0;
    const char *snapshot;
    int rc = 0;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "puzzllm: run it in a terminal (Kilix or Kitty)\n");
        return 1;
    }
    if (!game_init(&the_game, root, state, true, err, sizeof err)) {
        fprintf(stderr, "puzzllm: %s\n", err);
        return 1;
    }
    if (!sr_canvas_init(&canvas, CANVAS_W, CANVAS_H)) {
        game_shutdown(&the_game);
        return 1;
    }
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGHUP, on_stop);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);
    if (termio_start() != 0) {
        int e = errno;
        game_shutdown(&the_game);
        sr_canvas_free(&canvas);
        if (e == ENOTSUP)
            fprintf(stderr, "puzzllm: this terminal does not show Kitty graphics; "
                            "run it in a Kilix or Kitty tab\n");
        else
            fprintf(stderr, "puzzllm: cannot start the terminal session: %s\n", strerror(e));
        return 1;
    }
    the_game.release_events = termio_release_events();
    snapshot = getenv("PUZZLLM_SNAPSHOT");
    last = now_s();
    while (!stop_requested && !the_game.quit) {
        struct pollfd fds[3];
        double t;
        int nfds = 0;
        fds[nfds].fd = STDIN_FILENO; fds[nfds++].events = POLLIN;
        if (the_game.omp.fd >= 0) { fds[nfds].fd = the_game.omp.fd; fds[nfds++].events = POLLIN; }
        if (the_game.shell.fd >= 0) { fds[nfds].fd = the_game.shell.fd; fds[nfds++].events = POLLIN; }
        poll(fds, (nfds_t)nfds, 33);

        if (termio_read(&h) < 0) break;
        the_game.release_events = termio_release_events();
        t = now_s();
        game_update(&the_game, t - last);
        last = t;
        termio_check_resize();
        game_render(&the_game, &canvas);
        if (snapshot != NULL && snapshot[0] != '\0' && t >= next_snapshot) {
            /* debugging aid: the frame as a PPM, replaced atomically */
            char tmp[4200];
            snprintf(tmp, sizeof tmp, "%s.tmp", snapshot);
            if (sr_write_ppm(&canvas, tmp)) rename(tmp, snapshot);
            next_snapshot = t + 2.0;
        }
        if (!termio_present(canvas.px, canvas.w, canvas.h)) {
            rc = 1;
            break;
        }
    }
    termio_stop();
    game_shutdown(&the_game);
    sr_canvas_free(&canvas);
    return rc;
}

static void feed(vt *t, const char *s)
{
    vt_feed(t, s, strlen(s));
}

/* Headless: draw three states of the room to DIR as PPM files. */
static int render_to(const char *root, const char *state, const char *dir)
{
    char err[512], path[4096];
    sr_canvas canvas;
    static const char *names[3] = {"room1-door-shut.ppm", "room1-door-open.ppm",
                                   "room2.ppm"};

    if (!game_init(&the_game, root, state, false, err, sizeof err)) {
        fprintf(stderr, "puzzllm: %s\n", err);
        return 1;
    }
    if (!sr_canvas_init(&canvas, CANVAS_W, CANVAS_H)) return 1;
    feed(&the_game.omp_vt, "\033[1;36momp\033[0m  Qwen 3.5 (local)\r\n"
         "\xe2\x95\xad\xe2\x94\x80 you \xe2\x94\x80\xe2\x95\xae\r\n"
         "> create hello_world.txt saying Hello, World!\r\n");
    feed(&the_game.shell_vt,
         "\033[1;32mkilix@sandbox\033[0m:\033[1;34m~/workspace\033[0m$ ls\r\n");
    for (int i = 0; i < 3; i++) {
        if (i == 1) {
            the_game.sc.goal_met = true;
            the_game.sc.door = 1.0f;
            the_game.sc.x = 640;
            the_game.sc.y = 300;
            the_game.goal.met = true;
            snprintf(the_game.goal.why, sizeof the_game.goal.why, "hello_world.txt is there");
        }
        if (i == 2 && the_game.levels.count > 1) game_enter_level(&the_game, 1);
        sr_clear(&canvas, 0x000000);
        game_render(&the_game, &canvas);
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (!sr_write_ppm(&canvas, path)) {
            fprintf(stderr, "puzzllm: cannot write %s\n", path);
            return 1;
        }
        printf("%s\n", path);
    }
    sr_canvas_free(&canvas);
    game_shutdown(&the_game);
    return 0;
}

static int check_level(const char *root, const char *state, const char *n)
{
    char err[512];
    int i = atoi(n);
    goal_result r;
    if (!game_init(&the_game, root, state, false, err, sizeof err)) {
        fprintf(stderr, "puzzllm: %s\n", err);
        return 2;
    }
    if (i < 1 || i > the_game.levels.count) {
        fprintf(stderr, "puzzllm: no level %s\n", n);
        return 2;
    }
    r = levels_check(&the_game.levels.items[i - 1], the_game.workspace);
    printf("level %d: %s: %s\n", i, r.met ? "met" : "not met", r.why);
    game_shutdown(&the_game);
    return r.met ? 0 : 1;
}

int main(int argc, char **argv)
{
    char root[2048], state[2048];
    default_paths(root, sizeof root, state, sizeof state);

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("puzzllm %s\n", PUZZLLM_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--render") == 0)
        return render_to(root, state, argv[2]);
    if (argc == 3 && strcmp(argv[1], "--check-level") == 0)
        return check_level(root, state, argv[2]);
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: puzzllm [--version | --render DIR | --check-level N]\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: puzzllm [--version | --render DIR | --check-level N]\n");
        return 2;
    }
    return play(root, state);
}
