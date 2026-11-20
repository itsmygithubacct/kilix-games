/* main.c - puzzllm: Kilix in a room with a coding agent.
 *
 *   puzzllm                       play (needs a Kitty-graphics terminal)
 *   puzzllm --version
 *   puzzllm --render DIR          write the room as PPMs (no terminal needed)
 *   puzzllm --reset-progress      back up the workspace and start at level 1
 *   puzzllm --check-level N       print whether level N's goal is met now
 *   puzzllm --bench [WxH]         time frames headless (default 1908x972)
 *   puzzllm --renderer            print which renderer draws (neuronte/raster)
 *
 * PUZZLLM_ROOT is the puzzllm/ directory (bin/puzzllm sets it) and
 * PUZZLLM_STATE the persistent folder (default ~/.local/share/puzzllm).
 * PUZZLLM_SNAPSHOT=FILE writes the live frame to FILE (PPM) every 2 s, and
 * PUZZLLM_STATS=FILE appends the frame rate and frame times once a second.
 * PUZZLLM_NEURONTE_WEIGHTS (or ZELL_NEURONTE_WEIGHTS) names the Neuronte
 * weight file; PUZZLLM_RENDER=raster turns Neuronte off.
 */
#include "draw.h"
#include "game.h"
#include "layout.h"
#include "termio.h"

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PUZZLLM_VERSION
#define PUZZLLM_VERSION "0.2.0"
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

enum { FRAME_HZ = 60 };

static void start_renderer(const char *state)
{
    const char *r = getenv("PUZZLLM_RENDER");
    bool enabled = game_neuronte_preference(state);
    if (r && strcmp(r, "raster") == 0) enabled = false;
    if (r && strcmp(r, "neuronte") == 0) enabled = true;
    draw_set_neuronte(enabled);
}

static void on_key(void *ctx, const key_event *ev)
{
    game *g = ctx;
    unsigned long generation = g->clipboard_generation;
    game_key(g, ev);
    if (g->clipboard_generation != generation) termio_copy(g->clipboard, false);
}
static void on_pointer(void *ctx, int x, int y, pointer_action action)
{
    game *g = ctx;
    unsigned long generation = g->primary_generation;
    game_pointer(g, x, y, action);
    if (g->primary_generation != generation) termio_copy(g->primary, true);
}
static void on_paste(void *ctx, const unsigned char *text, size_t len, bool final)
{
    game_paste(ctx, text, len, final);
}
static void on_scroll(void *ctx, int x, int y, int rows) { game_scroll(ctx, x, y, rows); }

static int play(const char *root, const char *state)
{
    char err[512];
    sr_canvas canvas;
    termio_handlers h = {on_key, on_pointer, on_scroll, on_paste, &the_game};
    double last, next_snapshot = 0, next_frame, r0 = 0, r1 = 0;
    /* PUZZLLM_STATS=FILE: frame rate and timings, one line a second */
    FILE *stats = NULL;
    double st_start = 0, st_render = 0, st_present = 0;
    double st_render_max = 0, st_present_max = 0;
    int st_frames = 0;
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
    start_renderer(state);
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
    if (getenv("PUZZLLM_STATS") != NULL && getenv("PUZZLLM_STATS")[0] != '\0')
        stats = fopen(getenv("PUZZLLM_STATS"), "a");
    last = now_s();
    st_start = last;
    next_frame = last;
    while (!stop_requested && !the_game.quit) {
        struct pollfd fds[3];
        double t, wait;
        int nfds = 0, fw, fh;
        fds[nfds].fd = STDIN_FILENO; fds[nfds++].events = POLLIN;
        if (the_game.omp.fd >= 0) { fds[nfds].fd = the_game.omp.fd; fds[nfds++].events = POLLIN; }
        if (the_game.shell.fd >= 0) { fds[nfds].fd = the_game.shell.fd; fds[nfds++].events = POLLIN; }
        /* wake for input, terminal output or the next frame, whichever
         * comes first; frames come at most 60 times a second, as Zell's */
        wait = next_frame - now_s();
        poll(fds, (nfds_t)nfds, wait <= 0 ? 0 : (int)ceil(wait * 1000.0));

        if (termio_read(&h) < 0) break;
        t = now_s();
        if (t < next_frame) continue;
        next_frame = next_frame + 1.0 / FRAME_HZ < t ? t + 1.0 / FRAME_HZ
                                                   : next_frame + 1.0 / FRAME_HZ;
        the_game.release_events = termio_release_events();
        game_update(&the_game, t - last);
        last = t;
        termio_check_resize();
        termio_size(&fw, &fh);
        if (game_resize(&the_game, fw, fh)) {
            sr_canvas_free(&canvas);
            if (!sr_canvas_init(&canvas, CANVAS_W, CANVAS_H)) { rc = 1; break; }
        }
        r0 = now_s();
        game_render(&the_game, &canvas);
        r1 = now_s();
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
        if (stats != NULL) {
            double r2 = now_s();
            st_frames++;
            st_render += r1 - r0;
            st_present += r2 - r1;
            if (r1 - r0 > st_render_max) st_render_max = r1 - r0;
            if (r2 - r1 > st_present_max) st_present_max = r2 - r1;
            if (r2 - st_start >= 1.0) {
                fprintf(stats, "fps %.1f render %.2f/%.2f ms present %.2f/%.2f ms %s %s\n",
                        st_frames / (r2 - st_start), 1e3 * st_render / st_frames,
                        1e3 * st_render_max, 1e3 * st_present / st_frames,
                        1e3 * st_present_max, termio_describe(), draw_renderer());
                fflush(stats);
                st_frames = 0;
                st_render = st_present = st_render_max = st_present_max = 0;
                st_start = r2;
            }
        }
    }
    if (stats != NULL) fclose(stats);
    termio_stop();
    game_shutdown(&the_game);
    sr_canvas_free(&canvas);
    return rc;
}

static void feed(neuroterm *t, const char *s)
{
    neuroterm_feed(t, s, strlen(s));
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
    /* Preview rooms without replacing the saved room's guides or puzzles. */
    if (the_game.state_lock >= 0) {
        close(the_game.state_lock);
        the_game.state_lock = -1;
    }
    game_enter_level(&the_game, 0);
    start_renderer(state);
    if (!sr_canvas_init(&canvas, CANVAS_W, CANVAS_H)) return 1;
    feed(&the_game.omp_vt, "\033[1;36momp\033[0m  Qwen 3.5 (local)\r\n"
         "\xe2\x95\xad\xe2\x94\x80 you \xe2\x94\x80\xe2\x95\xae\r\n"
         "> create hello_world.txt saying Hello, World!\r\n");
    feed(&the_game.shell_vt,
         "\033[1;32mkilix@sandbox\033[0m:\033[1;34m~\033[0m$ ls\r\n");
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

/* Headless frame timing: the room at WxH with both terminals full of
 * text and Kilix walking, the way a busy frame looks. */
static int bench(const char *root, const char *state, const char *size)
{
    char err[512], line[512];
    sr_canvas canvas;
    int w = 1908, h = 972, frames = 240;
    double t0, worst = 0, total = 0;

    if (size != NULL && sscanf(size, "%dx%d", &w, &h) != 2) {
        fprintf(stderr, "puzzllm: --bench takes WxH, like 1908x972\n");
        return 2;
    }
    if (!game_init(&the_game, root, state, false, err, sizeof err)) {
        fprintf(stderr, "puzzllm: %s\n", err);
        return 1;
    }
    start_renderer(state);
    game_resize(&the_game, w, h);
    if (!sr_canvas_init(&canvas, CANVAS_W, CANVAS_H)) return 1;
    for (int row = 0; row < 80; row++) {
        snprintf(line, sizeof line, "\033[3%dm%3d \xe2\x94\x82 the quick brown fox jumps "
                 "over the lazy dog \xe2\x96\x88\xe2\xa0\xbf 0123456789 "
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\033[0m\r\n",
                 1 + row % 7, row);
        feed(&the_game.omp_vt, line);
        feed(&the_game.shell_vt, line);
    }
    the_game.held_until[1] = 1e9;   /* walk right... */
    for (int i = 0; i < frames; i++) {
        double t;
        if (i == frames / 2) { the_game.held_until[1] = 0; the_game.held_until[0] = 1e9; }
        game_update(&the_game, 1.0 / FRAME_HZ);
        t0 = now_s();
        game_render(&the_game, &canvas);
        t = now_s() - t0;
        total += t;
        if (t > worst) worst = t;
    }
    printf("renderer: %s\n", draw_renderer());
    printf("frame %dx%d: %.2f ms/frame average, %.2f ms worst, over %d frames\n",
           canvas.w, canvas.h, 1e3 * total / frames, 1e3 * worst, frames);
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
    r = game_check_level(&the_game, i - 1);
    printf("level %d: %s: %s\n", i, r.met ? "met" : "not met", r.why);
    game_shutdown(&the_game);
    return r.met ? 0 : 1;
}

int main(int argc, char **argv)
{
    char root[2048], state[2048];
    default_paths(root, sizeof root, state, sizeof state);

    if (argc == 2 && strcmp(argv[1], "--reset-progress") == 0) {
        if (!game_reset_progress(state)) {
            fprintf(stderr, "puzzllm: reset failed: %s\n", strerror(errno));
            return 1;
        }
        puts("Progress reset to level 1. Previous workspace files are kept in the state directory.");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("puzzllm %s\n", PUZZLLM_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--render") == 0)
        return render_to(root, state, argv[2]);
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--bench") == 0)
        return bench(root, state, argc == 3 ? argv[2] : NULL);
    if (argc == 2 && strcmp(argv[1], "--renderer") == 0) {
        start_renderer(state);
        printf("%s\n", draw_renderer());
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--check-level") == 0)
        return check_level(root, state, argv[2]);
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: puzzllm [--version | --render DIR | --check-level N | --reset-progress | --bench [WxH] | --renderer]\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: puzzllm [--version | --render DIR | --check-level N | --reset-progress | --bench [WxH] | --renderer]\n");
        return 2;
    }
    return play(root, state);
}
