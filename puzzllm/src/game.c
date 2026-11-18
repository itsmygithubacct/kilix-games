/* game.c - see game.h. */
#include "game.h"
#include "render.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int game_load_progress(const char *state, int level_count)
{
    char path[4096];
    FILE *f;
    int n = 1;
    snprintf(path, sizeof path, "%s/progress", state);
    f = fopen(path, "r");
    if (f == NULL) return 0;
    if (fscanf(f, "level %d", &n) != 1) n = 1;
    fclose(f);
    if (n < 1) n = 1;
    if (n > level_count) n = level_count;
    return n - 1;
}

bool game_save_progress(const char *state, int level)
{
    char path[4096], tmp[4200];
    FILE *f;
    snprintf(path, sizeof path, "%s/progress", state);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL) return false;
    fprintf(f, "level %d\n", level + 1);
    if (fclose(f) != 0) return false;
    return rename(tmp, path) == 0;
}

/* mkdir -p, mode 0700 for anything it creates. */
static bool make_dirs(const char *path)
{
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return false;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
            buf[i] = c;
        }
    }
    return true;
}

static const level *cur_level(const game *g)
{
    return &g->levels.items[g->sc.level];
}

static void spawn_omp(game *g)
{
    char sandbox[4096], st_env[2100];
    char *argv[] = {sandbox, "agent", NULL};
    char *env[] = {st_env, "TERM=xterm-256color", NULL};

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    vt_feed(&g->omp_vt, "\033[2J\033[H", 7);
    g->omp_note[0] = '\0';
    if (!pty_spawn(&g->omp, argv, env, OMP_COLS, OMP_ROWS))
        snprintf(g->omp_note, sizeof g->omp_note, "could not start the agent sandbox");
    else
        activity_add(&g->act, ACT_INFO, "agent sandbox started");
}

static void spawn_shell(game *g)
{
    char sandbox[4096], st_env[2100];
    char *argv[] = {sandbox, "shell", NULL};
    char *env[] = {st_env, "TERM=xterm-256color", NULL};

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    vt_feed(&g->shell_vt, "\033[2J\033[H", 7);
    g->shell_note[0] = '\0';
    if (!pty_spawn(&g->shell, argv, env, SHELL_COLS, SHELL_ROWS))
        snprintf(g->shell_note, sizeof g->shell_note, "could not start the shell sandbox");
}

void game_enter_level(game *g, int level)
{
    scene_enter_room(&g->sc, level);
    g->hints_shown = cur_level(g)->nhints > 0 ? 1 : 0;
    g->level_time = 0;
    g->was_met = false;
    activity_add(&g->act, ACT_GOAL, "Room %d: %s", level + 1, cur_level(g)->title);
    game_poll_goal(g);
    if (g->sc.goal_met) g->sc.door = 1.0f;   /* already done earlier */
}

bool game_init(game *g, const char *root, const char *state, bool spawn,
               char *err, size_t errcap)
{
    char path[4096];

    memset(g, 0, sizeof *g);
    snprintf(g->root, sizeof g->root, "%s", root);
    snprintf(g->state, sizeof g->state, "%s", state);
    snprintf(g->workspace, sizeof g->workspace, "%s/workspace", state);
    g->spawn = spawn;
    pty_init(&g->omp);
    pty_init(&g->shell);
    activity_init(&g->act);

    snprintf(path, sizeof path, "%s/levels/levels.txt", root);
    if (!levels_load(&g->levels, path, err, errcap)) return false;
    if (!make_dirs(g->workspace)) {
        snprintf(err, errcap, "%s: %s", g->workspace, strerror(errno));
        return false;
    }
    if (!vt_init(&g->omp_vt, OMP_COLS, OMP_ROWS) ||
        !vt_init(&g->shell_vt, SHELL_COLS, SHELL_ROWS)) {
        snprintf(err, errcap, "out of memory");
        return false;
    }
    scene_init(&g->sc, 0, g->levels.count);
    activity_scan_workspace(&g->act, g->workspace);   /* prime: no "new" spam */
    game_enter_level(g, game_load_progress(state, g->levels.count));
    if (spawn) {
        spawn_omp(g);
        spawn_shell(g);
    }
    return true;
}

void game_shutdown(game *g)
{
    pty_stop(&g->omp);
    pty_stop(&g->shell);
    vt_free(&g->omp_vt);
    vt_free(&g->shell_vt);
}

void game_poll_goal(game *g)
{
    g->goal = levels_check(cur_level(g), g->workspace);
    g->sc.goal_met = g->goal.met;
    if (g->goal.met && !g->was_met)
        activity_add(&g->act, ACT_GOAL, "GOAL MET: %s. The door opens.", g->goal.why);
    else if (!g->goal.met && g->was_met)
        activity_add(&g->act, ACT_GOAL, "goal lost: %s", g->goal.why);
    g->was_met = g->goal.met;
}

static void next_hint(game *g)
{
    if (g->hints_shown < cur_level(g)->nhints) g->hints_shown++;
}

static void focus_screen(game *g, focus_id f)
{
    pty_proc *p = f == FOCUS_OMP ? &g->omp : f == FOCUS_SHELL ? &g->shell : NULL;
    g->sc.focus = f;
    for (int i = 0; i < 4; i++) g->held_until[i] = 0;
    if (p != NULL && g->spawn && (p->pid == 0 || pty_poll_exit(p))) {
        if (f == FOCUS_OMP) spawn_omp(g);
        else spawn_shell(g);
    }
    if (f == FOCUS_OMP) g->sc.dir = g->sc.x < AISLE_X0 ? 1 : 2;
    if (f == FOCUS_SHELL) g->sc.dir = g->sc.y < FLOOR_Y ? 3 : 1;
}

static int dir_index(uint32_t key)
{
    switch (key) {
    case KEY_LEFT: case 'a': return 0;
    case KEY_RIGHT: case 'd': return 1;
    case KEY_UP: case 'w': return 2;
    case KEY_DOWN: case 's': return 3;
    default: return -1;
    }
}

void game_key(game *g, const key_event *ev)
{
    bool press = ev->action != 3;
    bool ctrl = (ev->mods & MOD_CTRL) != 0;

    /* keys that work everywhere */
    if (press && (ev->key == KEY_F1 + 9 || (ctrl && ev->key == ']'))) {
        focus_screen(g, FOCUS_ROOM);
        return;
    }
    if (press && ev->key == KEY_F1) { focus_screen(g, FOCUS_OMP); return; }
    if (press && ev->key == KEY_F1 + 1) { focus_screen(g, FOCUS_SHELL); return; }

    if (g->sc.focus != FOCUS_ROOM) {
        pty_proc *p = g->sc.focus == FOCUS_OMP ? &g->omp : &g->shell;
        vt *t = g->sc.focus == FOCUS_OMP ? &g->omp_vt : &g->shell_vt;
        char buf[256];
        size_t n;
        if (!press) return;
        if (p->pid == 0 || pty_poll_exit(p)) {
            if (ev->key == KEY_ENTER && g->spawn) focus_screen(g, g->sc.focus);
            return;
        }
        n = keys_encode(ev, t->app_cursor, buf, sizeof buf);
        if (n > 0) pty_write(p, buf, n);
        return;
    }

    /* walking around the room */
    {
        uint32_t k = ev->key;
        int d;
        if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
        d = ctrl ? -1 : dir_index(k);
        if (d >= 0) {
            if (ev->action == 3) g->held_until[d] = 0;
            else g->held_until[d] = g->now + (g->release_events ? 1e9 : 0.2);
            return;
        }
        if (!press) return;
        if (ctrl && k == 'q') { g->quit = true; return; }
        if (k == 'h') { next_hint(g); return; }
        if (k == KEY_ENTER || k == ' ' || k == 'e') {
            screen_id near = scene_near_screen(&g->sc);
            if (near == SCREEN_OMP) focus_screen(g, FOCUS_OMP);
            else if (near == SCREEN_SHELL) focus_screen(g, FOCUS_SHELL);
            else if (near == SCREEN_ACT) next_hint(g);
        }
    }
}

static bool inside(rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void game_click(game *g, int x, int y)
{
    if (inside(layout_screen(SCREEN_OMP), x, y)) focus_screen(g, FOCUS_OMP);
    else if (inside(layout_screen(SCREEN_SHELL), x, y)) focus_screen(g, FOCUS_SHELL);
    else if (inside(layout_screen(SCREEN_ACT), x, y)) next_hint(g);
    else focus_screen(g, FOCUS_ROOM);
}

static void pump(pty_proc *p, vt *t, char *note, size_t notecap, const char *who)
{
    char buf[16384], reply[VT_REPLY_CAP];
    for (int i = 0; i < 16; i++) {   /* bounded per frame */
        long n = pty_read(p, buf, sizeof buf);
        if (n <= 0) break;
        vt_feed(t, buf, (size_t)n);
        {
            size_t r = vt_take_reply(t, reply, sizeof reply);
            if (r > 0) pty_write(p, reply, r);
        }
    }
    if (p->pid > 0 && pty_poll_exit(p) && note[0] == '\0')
        snprintf(note, notecap, "%s exited - press Enter here to restart", who);
}

void game_update(game *g, double dt)
{
    int dx = 0, dy = 0;
    scene_event ev;

    g->now += dt;
    g->level_time += dt;
    if (g->spawn) {
        pump(&g->omp, &g->omp_vt, g->omp_note, sizeof g->omp_note, "the agent");
        pump(&g->shell, &g->shell_vt, g->shell_note, sizeof g->shell_note, "the shell");
    }
    g->poll_timer -= dt;
    if (g->poll_timer <= 0) {
        g->poll_timer = 0.5;
        activity_poll(&g->act, g->state);
        game_poll_goal(g);
    }
    /* hints arrive on their own as time passes, never faster than H */
    if (!g->sc.goal_met && g->hints_shown < cur_level(g)->nhints &&
        g->level_time > (g->hints_shown + 1) * (double)HINT_SECONDS)
        g->hints_shown++;

    if (g->held_until[0] > g->now) dx -= 1;
    if (g->held_until[1] > g->now) dx += 1;
    if (g->held_until[2] > g->now) dy -= 1;
    if (g->held_until[3] > g->now) dy += 1;
    ev = scene_step(&g->sc, dx, dy, (float)dt);
    if (ev == SCENE_NEXT_ROOM) {
        int next = g->sc.level + 1;
        game_save_progress(g->state, next);
        game_enter_level(g, next);
    }
}

void game_render(game *g, sr_canvas *c)
{
    view v;
    memset(&v, 0, sizeof v);
    v.sc = &g->sc;
    v.lv = cur_level(g);
    v.level_count = g->levels.count;
    v.omp = &g->omp_vt;
    v.shell = &g->shell_vt;
    v.act = &g->act;
    v.omp_note = g->omp_note[0] ? g->omp_note : NULL;
    v.shell_note = g->shell_note[0] ? g->shell_note : NULL;
    v.goal_why = g->goal.why;
    v.hints_shown = g->hints_shown;
    v.blink = ((long)(g->now * 2.0)) % 2 == 0;
    render_frame(c, &v);
}
