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

static bool save_state_line(const char *state, const char *name, const char *line)
{
    char path[4096], tmp[4200];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", state, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL) return false;
    bool written = fprintf(f, "%s\n", line) >= 0;
    if (fclose(f) != 0 || !written) return false;
    return rename(tmp, path) == 0;
}

static bool save_level(const char *state, const char *name, int level)
{
    char line[32];
    snprintf(line, sizeof line, "level %d", level + 1);
    return save_state_line(state, name, line);
}

static const char *const compute_name[] = {"auto", "cpu", "gpu"};

static compute_mode load_compute(const char *state)
{
    char path[4096], value[8];
    snprintf(path, sizeof path, "%s/compute", state);
    FILE *f = fopen(path, "r");
    if (!f) return COMPUTE_AUTO;
    bool read = fscanf(f, "%7s", value) == 1;
    fclose(f);
    if (read && strcmp(value, compute_name[COMPUTE_CPU]) == 0) return COMPUTE_CPU;
    if (read && strcmp(value, compute_name[COMPUTE_GPU]) == 0) return COMPUTE_GPU;
    return COMPUTE_AUTO;
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

/* The caller holds the state lock. */
static bool reset_progress(const char *state)
{
    char workspace[4096], backup[4096], path[4096];
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
    errno = error;
    return ok;
}

bool game_reset_progress(const char *state)
{
    if (!make_dirs(state)) return false;
    int lock = lock_state(state);
    if (lock < 0) return false;
    bool ok = reset_progress(state);
    int error = errno;
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
    if (lv->check == CHECK_COUNT_FILES)
        return count_puzzle_check(g->root, g->state, g->workspace);
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

static void spawn_agent(game *g)
{
    char sandbox[4096], st_env[2100], model_env[OLLAMA_NAME_CAP + 32];
    char *argv[] = {sandbox, g->use_codex ? "codex" : "agent", NULL};
    char *think_env = g->thinking_enabled ? "PUZZLLM_THINK=1" : "PUZZLLM_THINK=0";
    char compute_env[32];
    snprintf(compute_env, sizeof compute_env, "PUZZLLM_COMPUTE=%s", compute_name[g->compute]);
    char *env[] = {st_env, model_env, think_env, compute_env, "TERM=xterm-256color", NULL};

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    snprintf(model_env, sizeof model_env, "PUZZLLM_OLLAMA_MODEL=%s", g->ollama_model);
    neuroterm_feed(&g->omp_vt, "\033c", 2);
    history_clear(&g->omp_history);
    g->omp_note[0] = '\0';
    if (!pty_spawn(&g->omp, argv, env, OMP_COLS, OMP_ROWS))
        snprintf(g->omp_note, sizeof g->omp_note, "could not start the agent sandbox");
    else
        activity_add(&g->act, ACT_INFO, "agent sandbox started");
}

static void stop_room_agent(game *g)
{
    pty_stop(&g->omp);
    pty_init(&g->omp);
    neuroterm_feed(&g->omp_vt, "\033c", 2);
    history_clear(&g->omp_history);
    snprintf(g->omp_note, sizeof g->omp_note, "Focus the agent pane to start a session.");
}

static void spawn_shell(game *g)
{
    char sandbox[4096], st_env[2100];
    char *argv[] = {sandbox, "shell", NULL};
    char *env[] = {st_env, "TERM=xterm-256color", NULL};

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    neuroterm_feed(&g->shell_vt, "\033[2J\033[H", 7);
    g->shell_note[0] = '\0';
    if (!pty_spawn(&g->shell, argv, env, SHELL_COLS, SHELL_ROWS))
        snprintf(g->shell_note, sizeof g->shell_note, "could not start the shell sandbox");
}

static void clear_models(game_menu *menu)
{
    free(menu->models);
    menu->models = NULL;
    menu->count = 0;
}

static void open_menu(game *g)
{
    game_menu *menu = &g->menu;
    char helper[4096];
    char *argv[] = {helper, "list-details", NULL};
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
    const char *name = menu->line + 1;
    const char *allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";
    if (menu->line[0] < '0' || menu->line[0] > '2' || strspn(name, allowed) == 0 ||
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:/-") != n - 1)
        return;
    model_option *list = realloc(menu->models, (size_t)(menu->count + 1) * sizeof *list);
    if (!list) return;
    menu->models = list;
    model_option *model = &list[menu->count++];
    snprintf(model->name, sizeof model->name, "%s", name);
    model->thinking = (thinking_support)(menu->line[0] - '0');
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

static void start_play(game *g)
{
    if (!g->started) {
        g->started = true;
        if (g->spawn) {
            spawn_agent(g);
            spawn_shell(g);
        }
    }
    g->menu.open = false;
}

static void reset_from_menu(game *g)
{
    g->menu.confirm_reset = false;
    if (g->state_lock < 0) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Could not lock the save for reset.");
        return;
    }
    pty_stop(&g->omp);
    pty_stop(&g->shell);
    pty_init(&g->omp);
    pty_init(&g->shell);
    g->started = false;
    if (!reset_progress(g->state)) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Reset failed: %s", strerror(errno));
        return;
    }
    neuroterm_feed(&g->omp_vt, "\033c", 2);
    history_clear(&g->omp_history);
    neuroterm_feed(&g->shell_vt, "\033c", 2);
    history_clear(&g->shell_history);
    g->omp_note[0] = g->shell_note[0] = 0;
    g->transition_pending = false;
    g->tick_debt = g->poll_timer = 0;
    game_enter_level(g, 0);
    g->menu.selected = MENU_RESUME;
    snprintf(g->menu.notice, sizeof g->menu.notice, "Progress reset. Previous files backed up.");
}

static thinking_support bundled_thinking(void)
{
    const char *weights = getenv("PUZZLLM_MODEL");
    return weights && *weights ? THINK_UNAVAILABLE : THINK_SWITCHABLE;
}

static void restart_agent(game *g)
{
    if (g->spawn && g->started && !g->transition_pending && g->omp.pid != 0) {
        pty_stop(&g->omp);
        spawn_agent(g);
    }
}

static void menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    int last = menu->count + MENU_LOCAL;
    if (menu->confirm_reset) {
        if (ev->key == KEY_ENTER && ev->action != 2) reset_from_menu(g);
        return;
    }
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
        menu->notice[0] = 0;
        if (menu->selected == MENU_RESUME) start_play(g);
        if (menu->selected == MENU_QUIT) g->quit = true;
        if (menu->selected == MENU_RESET) menu->confirm_reset = true;
        if (menu->selected == MENU_THINKING && g->thinking_support != THINK_UNAVAILABLE) {
            g->thinking_enabled = !g->thinking_enabled;
            restart_agent(g);
        }
        if (menu->selected == MENU_COMPUTE && !g->use_codex) {
            compute_mode compute = g->compute == COMPUTE_GPU ? COMPUTE_AUTO : g->compute + 1;
            if (g->state_lock >= 0 && save_state_line(g->state, "compute", compute_name[compute])) {
                g->compute = compute;
                restart_agent(g);
            } else {
                snprintf(menu->notice, sizeof menu->notice, "Could not save the compute setting.");
            }
        }
        if (menu->selected >= MENU_CODEX) {
            bool codex = menu->selected == MENU_CODEX;
            const model_option *model = menu->selected >= MENU_FIRST_OLLAMA ?
                                        &menu->models[menu->selected - MENU_FIRST_OLLAMA] : NULL;
            const char *name = model ? model->name : "";
            thinking_support support = codex ? THINK_UNAVAILABLE :
                                       model ? model->thinking : bundled_thinking();
            if (codex != g->use_codex || strcmp(name, g->ollama_model) != 0 ||
                support != g->thinking_support) {
                g->thinking_enabled = false;
                g->thinking_support = support;
                snprintf(g->ollama_model, sizeof g->ollama_model, "%s", name);
                if (g->use_codex != codex) {
                    g->use_codex = codex;
                    g->act.codex = codex;
                    activity_begin_room(&g->act, g->state, g->workspace);
                }
                restart_agent(g);
            } else if (g->spawn && g->started && pty_poll_exit(&g->omp)) {
                restart_agent(g);
            }
            if (g->started) menu->open = false;
        }
        break;
    default:
        break;
    }
}

void game_enter_level(game *g, int level)
{
    count_puzzle_stop(&g->count_check);
    scene_enter_room(&g->sc, level);
    g->last_pane = FOCUS_OMP;
    g->hints_shown = cur_level(g)->nhints > 0 ? 1 : 0;
    g->goal_scroll = g->action_scroll = 0;
    g->selection = (pane_selection){0};
    g->level_time = 0;
    g->was_met = false;
    activity_begin_room(&g->act, g->state, g->workspace);
    if (cur_level(g)->check == CHECK_CHEST && g->state_lock >= 0 && !prepare_chest(g))
        activity_add(&g->act, ACT_ERROR, "Could not prepare chest.zip (requires zip).");
    if (cur_level(g)->check == CHECK_COUNT_FILES && g->state_lock >= 0 &&
        !count_puzzle_prepare(g->root, g->state))
        activity_add(&g->act, ACT_ERROR, "Could not install the count_files.sh puzzle.");
    activity_add(&g->act, ACT_GOAL, "Room %d: %s", level + 1, cur_level(g)->title);
    if (!g->spawn && cur_level(g)->check == CHECK_COUNT_FILES)
        g->goal = levels_check(cur_level(g), g->workspace);
    else
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
    g->thinking_support = bundled_thinking();
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
    if (!neuroterm_init(&g->omp_vt, OMP_COLS, OMP_ROWS) ||
        !neuroterm_init(&g->shell_vt, SHELL_COLS, SHELL_ROWS)) {
        snprintf(err, errcap, "out of memory");
        return false;
    }
    history_attach(&g->omp_history, &g->omp_vt);
    history_attach(&g->shell_history, &g->shell_vt);
    g->state_lock = lock_state(state);
    if (g->state_lock < 0 && (spawn || errno != EWOULDBLOCK)) {
        snprintf(err, errcap, "cannot lock game state: %s", strerror(errno));
        game_shutdown(g);
        return false;
    }
    g->compute = load_compute(state);
    scene_init(&g->sc, 0, g->levels.count);

    int previous = load_level(state, "transition", g->levels.count);
    if (g->state_lock >= 0 && previous >= 0 && (previous + 1 >= g->levels.count ||
                          !finish_transition(g, previous))) {
        snprintf(err, errcap, "could not finish archiving room %d", previous + 1);
        game_shutdown(g);
        return false;
    }
    game_enter_level(g, game_load_progress(state, g->levels.count));
    art_load_chest(&g->chest, root);
    art_load_room(&g->room, root);
    g->started = !spawn;
    if (spawn) open_menu(g);
    return true;
}

void game_shutdown(game *g)
{
    count_puzzle_stop(&g->count_check);
    pty_stop(&g->omp);
    pty_stop(&g->shell);
    pty_stop(&g->menu.discovery);
    clear_models(&g->menu);
    art_free_chest(&g->chest);
    art_free_room(&g->room);
    history_clear(&g->omp_history);
    history_clear(&g->shell_history);
    neuroterm_free(&g->omp_vt);
    neuroterm_free(&g->shell_vt);
    if (g->state_lock >= 0) close(g->state_lock);
    g->state_lock = -1;
}

void game_poll_goal(game *g)
{
    g->goal = cur_level(g)->check == CHECK_COUNT_FILES ?
        count_puzzle_poll(&g->count_check, g->root, g->state, g->workspace) :
        game_check_level(g, g->sc.level);
    g->sc.goal_met = g->goal.met;
    if (g->goal.met && !g->was_met)
        activity_add(&g->act, ACT_GOAL, "GOAL MET: %s. The door opens.", g->goal.why);
    else if (!g->goal.met && g->was_met)
        activity_add(&g->act, ACT_GOAL, "goal lost: %s", g->goal.why);
    g->was_met = g->goal.met;
}

static view game_view(const game *g);

static void paste_into_terminal(game *g, const char *text, size_t len)
{
    if (!len || g->menu.open || g->sc.focus == FOCUS_ROOM) return;
    bool agent = g->sc.focus == FOCUS_OMP;
    history *h = agent ? &g->omp_history : &g->shell_history;
    pty_proc *p = agent ? &g->omp : &g->shell;
    if (p->pid <= 0 || pty_poll_exit(p)) return;

    char bytes[CLIPBOARD_CAP + 16];
    vterm_keyboard_start_paste(h->terminal->term);
    size_t n = neuroterm_read(h->terminal, bytes, 8);
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        /* Paste text, excluding embedded terminal escape sequences and C0 keys. */
        if (ch == '\r' && i + 1 < len && text[i + 1] == '\n') continue;
        if (ch == '\n' || ch == '\r') bytes[n++] = '\r';
        else if (ch == '\t' || (ch >= 0x20 && ch != 0x7f)) bytes[n++] = (char)ch;
    }
    vterm_keyboard_end_paste(h->terminal->term);
    n += neuroterm_read(h->terminal, bytes + n, sizeof bytes - n);
    h->offset = 0;
    g->selection.active = false;
    if (n > 0) pty_write(p, bytes, n);
}

void game_paste(game *g, const unsigned char *text, size_t len, bool final)
{
    if (len > sizeof g->paste - g->paste_len) g->paste_overflow = true;
    if (!g->paste_overflow) {
        memcpy(g->paste + g->paste_len, text, len);
        g->paste_len += len;
    }
    if (final) {
        if (!g->paste_overflow) paste_into_terminal(g, g->paste, g->paste_len);
        g->paste_len = 0;
        g->paste_overflow = false;
    }
}

static void scroll_text(game *g, screen_id id, int rows)
{
    view v = game_view(g);
    int *offset = id == SCREEN_GOAL ? &g->goal_scroll : &g->action_scroll;
    int limit = render_scroll_limit(&v, id);
    int next = (*offset < limit ? *offset : limit) + rows;
    *offset = next < 0 ? 0 : next > limit ? limit : next;
}

static void next_hint(game *g)
{
    if (g->hints_shown < cur_level(g)->nhints) {
        if (g->selection.pane == SCREEN_GOAL) g->selection.active = false;
        g->hints_shown++;
        g->goal_scroll = 0;
        scroll_text(g, SCREEN_GOAL, LEVEL_TEXT);
    }
}

static void focus_screen(game *g, focus_id f)
{
    pty_proc *p = f == FOCUS_OMP ? &g->omp : f == FOCUS_SHELL ? &g->shell : NULL;
    g->sc.focus = f;
    if (f != FOCUS_ROOM) g->last_pane = f;
    for (int i = 0; i < 4; i++) g->held_until[i] = 0;
    if (p != NULL && g->spawn && (f != FOCUS_OMP || !g->transition_pending) &&
        (p->pid == 0 || pty_poll_exit(p))) {
        if (f == FOCUS_OMP) spawn_agent(g);
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

    if (ev->key >= KEY_LEFT && ev->key <= KEY_DOWN) {
        unsigned arrow = 1u << (ev->key - KEY_LEFT);
        if (ev->action == 1) {
            g->pane_arrows &= ~arrow;
        } else if (g->pane_arrows & arrow) {
            if (!press) g->pane_arrows &= ~arrow;
            return;
        }
        if (ev->mods == MOD_ALT) {
            if (ev->action == 1 && !g->menu.open) {
                g->pane_arrows |= arrow;
                switch (ev->key) {
                case KEY_LEFT: focus_screen(g, FOCUS_OMP); break;
                case KEY_RIGHT: focus_screen(g, FOCUS_SHELL); break;
                case KEY_UP: focus_screen(g, FOCUS_ROOM); break;
                case KEY_DOWN: focus_screen(g, g->last_pane); break;
                }
            }
            return;
        }
    }

    if (ev->key == KEY_ESCAPE && ev->mods == 0) {
        if (press && ev->action != 2) {
            if (g->menu.confirm_reset) g->menu.confirm_reset = false;
            else if (!g->menu.open) open_menu(g);
            else if (g->started) g->menu.open = false;
        }
        return;
    }
    if (g->menu.open) {
        if (press) menu_key(g, ev);
        return;
    }

    if ((ev->mods == (MOD_CTRL | MOD_ALT) || ev->mods == (MOD_CTRL | MOD_SHIFT)) &&
        (ev->key == 'c' || ev->key == 'C' || ev->key == 'v' || ev->key == 'V')) {
        if (ev->action == 1) {
            if (ev->key == 'c' || ev->key == 'C') {
                view v = game_view(g);
                char copy[CLIPBOARD_CAP];
                size_t len = render_copy_selection(&v, copy, sizeof copy);
                if (len) {
                    memcpy(g->clipboard, copy, len + 1);
                    g->clipboard_generation++;
                }
            } else {
                paste_into_terminal(g, g->clipboard, strlen(g->clipboard));
            }
        }
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
        neuroterm *t = g->sc.focus == FOCUS_OMP ? &g->omp_vt : &g->shell_vt;
        char buf[256];
        size_t n;
        if (!press) return;
        history *scroll = g->sc.focus == FOCUS_OMP ? &g->omp_history : &g->shell_history;
        if (ev->mods == MOD_SHIFT && !scroll->alternate &&
            (ev->key == KEY_PAGE_UP || ev->key == KEY_PAGE_DOWN)) {
            g->selection.active = false;
            history_scroll(scroll, ev->key == KEY_PAGE_UP ? t->rows : -t->rows);
            return;
        }
        if (p->pid == 0 || pty_poll_exit(p)) {
            if (ev->key == KEY_ENTER && g->spawn) focus_screen(g, g->sc.focus);
            return;
        }
        n = neuroterm_key(t, ev, buf, sizeof buf);
        if (n > 0) {
            scroll->offset = 0;
            if (g->selection.pane == (g->sc.focus == FOCUS_OMP ? SCREEN_OMP : SCREEN_SHELL))
                g->selection.active = false;
            pty_write(p, buf, n);
        }
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
            else if (near == SCREEN_GOAL) next_hint(g);
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
    else if (inside(layout_screen(SCREEN_GOAL), x, y)) next_hint(g);
    else if (inside(layout_screen(SCREEN_ACT), x, y)) return;
    else focus_screen(g, FOCUS_ROOM);
}

static int pointer_cell(rect r, int x, int y)
{
    x = x < r.x ? 0 : x >= r.x + r.w ? r.w - 1 : x - r.x;
    y = y < r.y ? 0 : y >= r.y + r.h ? r.h - 1 : y - r.y;
    return (y / CELL_H) * (r.w / CELL_W) + x / CELL_W;
}

void game_pointer(game *g, int x, int y, pointer_action action)
{
    pane_selection *s = &g->selection;
    if (g->menu.open) {
        s->dragging = false;
        return;
    }
    if (action == POINTER_MIDDLE) {
        if (!g->primary[0]) return;
        focus_id target;
        if (inside(layout_screen(SCREEN_OMP), x, y)) target = FOCUS_OMP;
        else if (inside(layout_screen(SCREEN_SHELL), x, y)) target = FOCUS_SHELL;
        else return;
        s->dragging = false;
        focus_screen(g, target);
        paste_into_terminal(g, g->primary, strlen(g->primary));
        return;
    }
    if (action == POINTER_DOWN) {
        *s = (pane_selection){.pane = SCREEN_NONE, .dragging = true};
        for (int i = 0; i < SCREEN_COUNT; i++) {
            screen_id id = (screen_id)i;
            rect r = layout_text_bounds(id);
            if (!inside(r, x, y)) continue;
            s->pane = id;
            s->start = s->end = pointer_cell(r, x, y);
            break;
        }
        return;
    }
    if (!s->dragging) return;
    if (s->pane != SCREEN_NONE) {
        s->end = pointer_cell(layout_text_bounds(s->pane), x, y);
        if (s->end != s->start || action == POINTER_DRAG) s->active = true;
    }
    if (action == POINTER_UP) {
        s->dragging = false;
        if (s->active) {
            view v = game_view(g);
            char text[CLIPBOARD_CAP];
            size_t len = render_copy_selection(&v, text, sizeof text);
            if (len) {
                memcpy(g->primary, text, len + 1);
                g->primary_generation++;
            }
        } else {
            game_click(g, x, y);
        }
    }
}

void game_scroll(game *g, int x, int y, int rows)
{
    if (g->menu.open || !rows) return;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        screen_id id = (screen_id)i;
        if (!inside(layout_screen(id), x, y)) continue;
        if (g->selection.pane == id) g->selection = (pane_selection){0};
        if (id == SCREEN_GOAL || id == SCREEN_ACT) {
            scroll_text(g, id, id == SCREEN_GOAL ? rows : -rows);
            return;
        }

        history *h = id == SCREEN_OMP ? &g->omp_history : &g->shell_history;
        pty_proc *p = id == SCREEN_OMP ? &g->omp : &g->shell;
        if (!h->alternate && h->mouse_mode == VTERM_PROP_MOUSE_NONE) {
            history_scroll(h, -rows);
        } else if (p->pid > 0) {
            VTerm *term = h->terminal->term;
            if (h->mouse_mode != VTERM_PROP_MOUSE_NONE) {
                int tx, ty;
                layout_text_origin(id, &tx, &ty);
                if (x < tx || y < ty || x >= tx + h->terminal->cols * CELL_W ||
                    y >= ty + h->terminal->rows * CELL_H) return;
                vterm_mouse_move(term, (y - ty) / CELL_H, (x - tx) / CELL_W, VTERM_MOD_NONE);
                vterm_mouse_button(term, rows < 0 ? 4 : 5, true, VTERM_MOD_NONE);
            } else {
                int steps = rows > 0 ? rows : -rows;
                for (int j = 0; j < steps; j++)
                    vterm_keyboard_key(term, rows < 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN,
                                       VTERM_MOD_NONE);
            }
            char buf[256];
            size_t n;
            while ((n = neuroterm_read(h->terminal, buf, sizeof buf)) != 0)
                pty_write(p, buf, n);
        }
        return;
    }
}

static void pump(pty_proc *p, neuroterm *t, char *note, size_t notecap, const char *who)
{
    char buf[16384], reply[256];
    for (int i = 0; i < 16; i++) {   /* bounded per frame */
        long n = pty_read(p, buf, sizeof buf);
        if (n <= 0) break;
        neuroterm_feed(t, buf, (size_t)n);
        size_t bytes;
        while ((bytes = neuroterm_read(t, reply, sizeof reply)) != 0)
            pty_write(p, reply, bytes);
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
            } else {
                stop_room_agent(g);
                if (finish_transition(g, g->sc.level)) {
                    game_enter_level(g, g->sc.level + 1);
                } else {
                    activity_add(&g->act, ACT_ERROR, "Could not archive room files. Retrying.");
                    g->transition_pending = true;
                }
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
    g->selection = (pane_selection){0};
    if (neuroterm_resize(&g->omp_vt, OMP_COLS, OMP_ROWS)) pty_resize(&g->omp, OMP_COLS, OMP_ROWS);
    if (neuroterm_resize(&g->shell_vt, SHELL_COLS, SHELL_ROWS))
        pty_resize(&g->shell, SHELL_COLS, SHELL_ROWS);
    scene_relayout(&g->sc, &old);
    return true;
}

static view game_view(const game *g)
{
    view v;
    memset(&v, 0, sizeof v);
    v.sc = &g->sc;
    v.lv = cur_level(g);
    v.chest = &g->chest;
    v.room = &g->room;
    v.level_count = g->levels.count;
    v.omp = &g->omp_vt;
    v.agent_model = g->use_codex ? "Codex" : g->ollama_model;
    v.codex = g->use_codex;
    v.shell = &g->shell_vt;
    v.omp_history = &g->omp_history;
    v.shell_history = &g->shell_history;
    v.act = &g->act;
    v.goal_scroll = g->goal_scroll;
    v.action_scroll = g->action_scroll;
    v.selection = g->selection;
    v.omp_note = g->omp_note[0] ? g->omp_note : NULL;
    v.shell_note = g->shell_note[0] ? g->shell_note : NULL;
    v.goal_why = g->goal.why;
    v.hints_shown = g->hints_shown;
    v.blink = ((long)(g->now * 2.0)) % 2 == 0;
    return v;
}

void game_render(game *g, sr_canvas *c)
{
    view v = game_view(g);
    render_frame(c, &v);
    if (g->menu.open)
        render_menu(c, g->menu.models, g->menu.count, g->menu.selected,
                    v.agent_model, g->menu.loading, g->menu.load_failed,
                    g->started, g->sc.level + 1, g->menu.confirm_reset, g->menu.notice,
                    g->thinking_support, g->thinking_enabled, g->use_codex, g->compute);
}
