/* game.c - see game.h. */
#define _GNU_SOURCE
#include "game.h"
#include "render.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int load_level(const char *state, const char *name, int level_count)
{
    char path[4096];
    FILE *f;
    int n = 1;
    snprintf(path, sizeof path, "%s/%s", state, name);
    f = fopen(path, "r");
    if (f == NULL) return -1;
    if (fscanf(f, "level %d", &n) != 1) n = 1;
    fclose(f);
    if (n < 1) n = 1;
    if (n > level_count) n = level_count;
    return n - 1;
}

static bool save_level(const char *state, const char *name, int level)
{
    char path[4096], tmp[4200];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", state, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL) return false;
    fprintf(f, "level %d\n", level + 1);
    if (fclose(f) != 0) return false;
    return rename(tmp, path) == 0;
}

int game_load_progress(const char *state, int level_count)
{
    int level = load_level(state, "progress", level_count);
    return level < 0 ? 0 : level;
}

bool game_save_progress(const char *state, int level)
{
    return save_level(state, "progress", level);
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

/* Hold the lock for the lifetime of a live game or a reset. */
static int lock_state(const char *state)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/play.lock", state);
    int fd = open(path, O_CREAT | O_CLOEXEC | O_RDWR | O_NOFOLLOW, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

bool game_reset_progress(const char *state)
{
    char workspace[4096], backup[4096], path[4096];
    if (!make_dirs(state)) return false;
    int lock = lock_state(state);
    if (lock < 0) return false;
    snprintf(workspace, sizeof workspace, "%s/workspace", state);
    snprintf(backup, sizeof backup, "%s/reset-XXXXXX", state);
    bool made_backup = mkdtemp(backup) != NULL;
    bool ok = made_backup;
    if (ok) {
        int len = snprintf(path, sizeof path, "%s/workspace", backup);
        if (len < 0 || (size_t)len >= sizeof path) {
            errno = ENAMETOOLONG;
            ok = false;
        } else {
            ok = rename(workspace, path) == 0 || errno == ENOENT;
        }
    }
    if (ok) {
        snprintf(path, sizeof path, "%s/chest-key", state);
        ok = unlink(path) == 0 || errno == ENOENT;
    }
    if (ok) {
        snprintf(path, sizeof path, "%s/transition", state);
        ok = unlink(path) == 0 || errno == ENOENT;
    }
    if (ok) {
        snprintf(path, sizeof path, "%s/progress", state);
        ok = unlink(path) == 0 || errno == ENOENT;
    }
    if (ok) ok = make_dirs(workspace);
    int error = errno;
    if (made_backup) rmdir(backup);  /* only removes an empty backup */
    close(lock);
    errno = error;
    return ok;
}

static bool archive_workspace(const char *workspace, int number)
{
    char folder[32];
    snprintf(folder, sizeof folder, "level_%d", number);
    DIR *dir = opendir(workspace);
    if (!dir) return false;
    int source = dirfd(dir);
    int dest = -1;
    if (mkdirat(source, folder, 0700) == 0 || errno == EEXIST)
        dest = openat(source, folder, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    bool ok = dest >= 0;
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        const char *digits = name + (strncmp(name, "level_", 6) == 0 ? 6 : 0);
        if (digits != name && *digits && digits[strspn(digits, "0123456789")] == 0) {
            struct stat st;
            if (fstatat(source, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(st.st_mode)) continue;
        }
        char candidate[256];
        snprintf(candidate, sizeof candidate, "%s", name);
        for (unsigned suffix = 1; ; suffix++) {
            if (renameat2(source, name, dest, candidate, RENAME_NOREPLACE) == 0 ||
                errno == ENOENT) break;
            if (errno != EEXIST) {
                ok = false;
                break;
            }
            snprintf(candidate, sizeof candidate, "%.240s.%u", name, suffix);
        }
    }
    if (dest >= 0) close(dest);
    closedir(dir);
    return ok;
}

/* The marker lets a restart finish an interrupted move before starting play. */
static bool finish_transition(const game *g, int previous)
{
    char marker[4096];
    if (!archive_workspace(g->workspace, previous + 1) ||
        !game_save_progress(g->state, previous + 1)) return false;
    snprintf(marker, sizeof marker, "%s/transition", g->state);
    return unlink(marker) == 0;
}

enum { CHEST_KEY_BYTES = 33 };

static bool read_key(const char *path, char key[CHEST_KEY_BYTES])
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size == CHEST_KEY_BYTES && read(fd, key, CHEST_KEY_BYTES) == CHEST_KEY_BYTES;
    close(fd);
    return ok;
}

static bool prepare_chest(const game *g)
{
    static const char *const passwords[] = {"zell", "qwen", "puzl", "kilx", "pleb"};
    char chest[4096], secret[4096], staging[2200], keypath[2300], zipfile[2300];
    char key[CHEST_KEY_BYTES];
    unsigned char random[17];
    struct stat st;

    snprintf(chest, sizeof chest, "%s/chest.zip", g->workspace);
    snprintf(secret, sizeof secret, "%s/chest-key", g->state);
    if (lstat(chest, &st) == 0 && S_ISREG(st.st_mode) && read_key(secret, key))
        return true;

    do {
        if (getrandom(random, sizeof random, 0) != sizeof random) return false;
    } while (random[16] == 255);
    for (int i = 0; i < 16; i++) snprintf(key + 2 * i, 3, "%02x", random[i]);
    key[32] = '\n';

    snprintf(staging, sizeof staging, "%s/chest-XXXXXX", g->state);
    if (!mkdtemp(staging)) return false;
    snprintf(keypath, sizeof keypath, "%s/key.txt", staging);
    snprintf(zipfile, sizeof zipfile, "%s/chest.zip", staging);
    int fd = open(keypath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0 && write(fd, key, sizeof key) == sizeof key;
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok) {
        pid_t child = fork();
        if (child == 0) {
            if (chdir(staging) == 0)
                execl("/usr/bin/zip", "zip", "-q", "-P", passwords[random[16] % 5],
                      "chest.zip", "key.txt", (char *)NULL);
            _exit(127);
        }
        int status;
        pid_t done = -1;
        if (child > 0)
            do { done = waitpid(child, &status, 0); } while (done < 0 && errno == EINTR);
        ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (ok) ok = rename(keypath, secret) == 0 && rename(zipfile, chest) == 0;
    unlink(keypath);
    unlink(zipfile);
    rmdir(staging);
    return ok;
}

goal_result game_check_level(const game *g, int index)
{
    const level *lv = &g->levels.items[index];
    if (lv->check != CHECK_CHEST) return levels_check(lv, g->workspace);

    goal_result result = {false, "extract key.txt from chest.zip"};
    char path[4096], expected[CHEST_KEY_BYTES], actual[CHEST_KEY_BYTES];
    snprintf(path, sizeof path, "%s/chest-key", g->state);
    if (!read_key(path, expected)) return result;
    snprintf(path, sizeof path, "%s/key.txt", g->workspace);
    if (read_key(path, actual) && memcmp(expected, actual, sizeof actual) == 0)
        result = (goal_result){true, "the chest key fits"};
    return result;
}

static const level *cur_level(const game *g)
{
    return &g->levels.items[g->sc.level];
}

static void spawn_omp(game *g)
{
    char sandbox[4096], st_env[2100], model_env[OLLAMA_NAME_CAP + 32];
    char *argv[] = {sandbox, "agent", NULL};
    char *env[] = {st_env, model_env, "TERM=xterm-256color", NULL};

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    snprintf(model_env, sizeof model_env, "PUZZLLM_OLLAMA_MODEL=%s", g->ollama_model);
    vt_feed(&g->omp_vt, "\033c", 2);
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

static void clear_models(game_menu *menu)
{
    for (int i = 0; i < menu->count; i++) free(menu->models[i]);
    free(menu->models);
    menu->models = NULL;
    menu->count = 0;
}

static void open_menu(game *g)
{
    game_menu *menu = &g->menu;
    char helper[4096];
    char *argv[] = {helper, "list", NULL};
    menu->open = true;
    menu->selected = 0;
    memset(g->held_until, 0, sizeof g->held_until);
    if (!g->spawn || menu->loading) return;
    clear_models(menu);
    menu->line_length = 0;
    snprintf(helper, sizeof helper, "%s/libexec/puzzllm-ollama", g->root);
    menu->loading = pty_spawn(&menu->discovery, argv, NULL, 80, 24);
    menu->load_failed = !menu->loading;
}

static void add_model(game_menu *menu)
{
    size_t n = menu->line_length;
    if (n == 0 || n >= sizeof menu->line) return;
    const char *allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";
    if (strspn(menu->line, allowed) == 0 ||
        strspn(menu->line, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:/-") != n)
        return;
    char **list = realloc(menu->models, (size_t)(menu->count + 1) * sizeof *list);
    if (!list) return;
    menu->models = list;
    char *name = strdup(menu->line);
    if (name) menu->models[menu->count++] = name;
}

static void poll_models(game_menu *menu)
{
    char buf[4096];
    for (int reads = 0; menu->loading && reads < 8; reads++) {
        long bytes = pty_read(&menu->discovery, buf, sizeof buf);
        if (bytes > 0) {
            for (long i = 0; i < bytes; i++) {
                if (buf[i] == '\n') {
                    if (menu->line_length < sizeof menu->line) {
                        menu->line[menu->line_length] = 0;
                        add_model(menu);
                    }
                    menu->line_length = 0;
                } else if (buf[i] != '\r' && menu->line_length < sizeof menu->line) {
                    menu->line[menu->line_length++] = buf[i];
                }
            }
        } else {
            if (bytes < 0 && pty_poll_exit(&menu->discovery)) {
                menu->loading = false;
                menu->load_failed = menu->discovery.exit_status != 0;
                pty_stop(&menu->discovery);
            }
            break;
        }
    }
}

static void menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    int last = menu->count + MENU_LOCAL;
    switch (ev->key) {
    case KEY_UP:
        if (menu->selected > 0) menu->selected--;
        break;
    case KEY_DOWN:
        if (menu->selected < last) menu->selected++;
        break;
    case KEY_HOME:
        menu->selected = 0;
        break;
    case KEY_END:
        menu->selected = last;
        break;
    case KEY_ENTER:
        if (ev->action == 2) break;
        menu->open = false;
        if (menu->selected == MENU_QUIT) g->quit = true;
        if (menu->selected >= MENU_LOCAL) {
            const char *name = menu->selected == MENU_LOCAL ? "" :
                               menu->models[menu->selected - MENU_FIRST_OLLAMA];
            if (strcmp(name, g->ollama_model) != 0 || (g->spawn && pty_poll_exit(&g->omp))) {
                snprintf(g->ollama_model, sizeof g->ollama_model, "%s", name);
                if (g->spawn) {
                    pty_stop(&g->omp);
                    spawn_omp(g);
                }
            }
        }
        break;
    default:
        break;
    }
}

void game_enter_level(game *g, int level)
{
    scene_enter_room(&g->sc, level);
    g->hints_shown = cur_level(g)->nhints > 0 ? 1 : 0;
    g->level_time = 0;
    g->was_met = false;
    activity_begin_room(&g->act, g->state, g->workspace);
    if (cur_level(g)->check == CHECK_CHEST && g->state_lock >= 0 && !prepare_chest(g))
        activity_add(&g->act, ACT_ERROR, "Could not prepare chest.zip (requires zip).");
    activity_add(&g->act, ACT_GOAL, "Room %d: %s", level + 1, cur_level(g)->title);
    game_poll_goal(g);
    if (g->sc.goal_met) g->sc.door = 1.0f;   /* already done earlier */
}

bool game_init(game *g, const char *root, const char *state, bool spawn,
               char *err, size_t errcap)
{
    char path[4096];

    memset(g, 0, sizeof *g);
    if (L.w == 0) layout_set(LAYOUT_DEFAULT_W, LAYOUT_DEFAULT_H);
    snprintf(g->root, sizeof g->root, "%s", root);
    snprintf(g->state, sizeof g->state, "%s", state);
    snprintf(g->workspace, sizeof g->workspace, "%s/workspace", state);
    g->spawn = spawn;
    g->state_lock = -1;
    pty_init(&g->omp);
    pty_init(&g->shell);
    pty_init(&g->menu.discovery);
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
    g->state_lock = lock_state(state);
    if (g->state_lock < 0 && (spawn || errno != EWOULDBLOCK)) {
        snprintf(err, errcap, "cannot lock game state: %s", strerror(errno));
        game_shutdown(g);
        return false;
    }
    scene_init(&g->sc, 0, g->levels.count);

    int previous = load_level(state, "transition", g->levels.count);
    if (g->state_lock >= 0 && previous >= 0 && (previous + 1 >= g->levels.count ||
                          !finish_transition(g, previous))) {
        snprintf(err, errcap, "could not finish archiving room %d", previous + 1);
        game_shutdown(g);
        return false;
    }
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
    pty_stop(&g->menu.discovery);
    clear_models(&g->menu);
    vt_free(&g->omp_vt);
    vt_free(&g->shell_vt);
    if (g->state_lock >= 0) close(g->state_lock);
    g->state_lock = -1;
}

void game_poll_goal(game *g)
{
    g->goal = game_check_level(g, g->sc.level);
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

    if (ev->key == KEY_ESCAPE && ev->mods == 0) {
        if (press && ev->action != 2) {
            if (g->menu.open) g->menu.open = false;
            else open_menu(g);
        }
        return;
    }
    if (g->menu.open) {
        if (press) menu_key(g, ev);
        return;
    }

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
    if (g->menu.open) return;
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
    poll_models(&g->menu);
    if (g->spawn) {
        pump(&g->omp, &g->omp_vt, g->omp_note, sizeof g->omp_note, "the agent");
        pump(&g->shell, &g->shell_vt, g->shell_note, sizeof g->shell_note, "the shell");
    }
    if (g->menu.open) return;
    if (g->transition_pending) {
        g->poll_timer -= dt;
        if (g->poll_timer > 0) return;
        g->poll_timer = 0.5;
        if (finish_transition(g, g->sc.level)) {
            g->transition_pending = false;
            game_enter_level(g, g->sc.level + 1);
        }
        return;
    }
    g->now += dt;
    g->level_time += dt;
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

    /* fixed ticks; a stall runs at most a quarter second of them, so
     * Kilix never jumps */
    g->tick_debt += dt;
    if (g->tick_debt > 0.25) g->tick_debt = 0.25;
    while (g->tick_debt >= 1.0 / SCENE_TICK_HZ) {
        double t = g->now - g->tick_debt;
        int dx = 0, dy = 0;
        scene_event ev;
        g->tick_debt -= 1.0 / SCENE_TICK_HZ;
        if (g->held_until[0] > t) dx -= 1;
        if (g->held_until[1] > t) dx += 1;
        if (g->held_until[2] > t) dy -= 1;
        if (g->held_until[3] > t) dy += 1;
        ev = scene_tick(&g->sc, dx, dy);
        if (ev == SCENE_NEXT_ROOM) {
            if (!save_level(g->state, "transition", g->sc.level)) {
                activity_add(&g->act, ACT_ERROR, "Could not save room transition.");
            } else if (finish_transition(g, g->sc.level)) {
                game_enter_level(g, g->sc.level + 1);
            } else {
                activity_add(&g->act, ACT_ERROR, "Could not archive room files. Retrying.");
                g->transition_pending = true;
            }
            g->tick_debt = 0;
            break;
        }
    }
}

bool game_resize(game *g, int w, int h)
{
    layout old = L;
    if (!layout_set(w, h)) return false;
    if (vt_resize(&g->omp_vt, OMP_COLS, OMP_ROWS)) pty_resize(&g->omp, OMP_COLS, OMP_ROWS);
    if (vt_resize(&g->shell_vt, SHELL_COLS, SHELL_ROWS))
        pty_resize(&g->shell, SHELL_COLS, SHELL_ROWS);
    scene_relayout(&g->sc, &old);
    return true;
}

void game_render(game *g, sr_canvas *c)
{
    view v;
    memset(&v, 0, sizeof v);
    v.sc = &g->sc;
    v.lv = cur_level(g);
    v.level_count = g->levels.count;
    v.omp = &g->omp_vt;
    v.agent_model = g->ollama_model;
    v.shell = &g->shell_vt;
    v.act = &g->act;
    v.omp_note = g->omp_note[0] ? g->omp_note : NULL;
    v.shell_note = g->shell_note[0] ? g->shell_note : NULL;
    v.goal_why = g->goal.why;
    v.hints_shown = g->hints_shown;
    v.blink = ((long)(g->now * 2.0)) % 2 == 0;
    render_frame(c, &v);
    if (g->menu.open)
        render_menu(c, g->menu.models, g->menu.count, g->menu.selected,
                    g->ollama_model, g->menu.loading, g->menu.load_failed);
}
