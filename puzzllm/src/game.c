/* game.c - see game.h. */
#define _GNU_SOURCE
#include "game.h"
#include "draw.h"
#include "render.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool save_hints_file(const game *g);
static bool remove_workspace_file(const game *g, const char *name);

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

static bool load_preference(const char *state, const char *name, char value[8])
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", state, name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool read = fscanf(f, "%7s", value) == 1;
    fclose(f);
    return read;
}

static compute_mode load_compute(const char *state)
{
    char value[8];
    bool read = load_preference(state, "compute", value);
    if (read && strcmp(value, compute_name[COMPUTE_CPU]) == 0) return COMPUTE_CPU;
    if (read && strcmp(value, compute_name[COMPUTE_GPU]) == 0) return COMPUTE_GPU;
    return COMPUTE_AUTO;
}

bool game_neuronte_preference(const char *state)
{
    char value[8];
    return load_preference(state, "neuronte", value) && strcmp(value, "on") == 0;
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

static void rank_attempt_name(char name[32], int level)
{
    snprintf(name, 32, "rank-attempt-%d", level + 1);
}

static bool clear_rank_attempt(const char *state, int level)
{
    char path[4096], name[32];
    rank_attempt_name(name, level);
    snprintf(path, sizeof path, "%s/%s", state, name);
    return unlink(path) == 0 || errno == ENOENT;
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
    for (int i = 0; ok && i < LEVEL_MAX; i++) ok = clear_rank_attempt(state, i);
    if (ok) ok = make_dirs(workspace);
    int error = errno;
    if (made_backup) rmdir(backup);  /* only removes an empty backup */
    errno = error;
    return ok;
}

/* Prepare the replacement before touching the active workspace. */
static bool reset_level_workspace(const game *g)
{
    char backup[2300], fresh[2400];
    snprintf(backup, sizeof backup, "%s/level-reset-%d-XXXXXX", g->state, g->sc.level + 1);
    if (!mkdtemp(backup)) return false;
    snprintf(fresh, sizeof fresh, "%s/workspace", backup);
    if (mkdir(fresh, 0700) == 0 &&
        renameat2(AT_FDCWD, g->workspace, AT_FDCWD, fresh, RENAME_EXCHANGE) == 0)
        return true;
    int error = errno;
    rmdir(fresh);
    rmdir(backup);
    errno = error;
    return false;
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

static bool move_room_entry(int source, const char *name, int dest)
{
    char candidate[256];
    snprintf(candidate, sizeof candidate, "%s", name);
    for (unsigned suffix = 1; ; suffix++) {
        if (renameat2(source, name, dest, candidate, RENAME_NOREPLACE) == 0 ||
            errno == ENOENT) return true;
        if (errno != EEXIST) return false;
        snprintf(candidate, sizeof candidate, "%.240s.%u", name, suffix);
    }
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
        /* The running shell keeps its log and the mounted help across rooms. */
        if (!strcmp(name, ".") || !strcmp(name, "..") ||
            !strcmp(name, "shell.log") || !strcmp(name, "docs") ||
            !strcmp(name, ".system")) continue;
        const char *digits = name + (strncmp(name, "level_", 6) == 0 ? 6 : 0);
        if (digits != name && *digits && digits[strspn(digits, "0123456789")] == 0) {
            struct stat st;
            if (fstatat(source, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(st.st_mode)) continue;
        }
        ok = move_room_entry(source, name, dest);
    }
    if (dest >= 0) close(dest);
    closedir(dir);
    return ok;
}

static bool restore_workspace(const char *workspace, int number)
{
    char folder[32];
    snprintf(folder, sizeof folder, "level_%d", number);
    int dest = open(workspace, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dest < 0) return false;
    int source = openat(dest, folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0) {
        bool absent = errno == ENOENT;
        close(dest);
        return absent;
    }
    DIR *dir = fdopendir(source);
    if (!dir) {
        close(source);
        close(dest);
        return false;
    }
    bool ok = true;
    while (ok) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            ok = errno == 0;
            break;
        }
        const char *name = entry->d_name;
        if (strcmp(name, ".") && strcmp(name, ".."))
            ok = move_room_entry(source, name, dest);
    }
    closedir(dir);
    close(dest);
    return ok;
}

typedef struct room_transition {
    int from, direction;
    bool restoring;
} room_transition;

static bool read_transition(const game *g, room_transition *move)
{
    char marker[4096];
    snprintf(marker, sizeof marker, "%s/transition", g->state);
    FILE *file = fopen(marker, "r");
    *move = (room_transition){0};
    if (!file) return errno == ENOENT;
    char phase[16];
    int room;
    int fields = fscanf(file, "%15s %d", phase, &room);
    fclose(file);
    if (fields != 2 || room < 1 || room > g->levels.count) return false;
    move->from = room - 1;
    if (!strcmp(phase, "level") || !strcmp(phase, "restore-next"))
        move->direction = 1;
    else if (!strcmp(phase, "back") || !strcmp(phase, "restore-back"))
        move->direction = -1;
    else return false;
    move->restoring = strncmp(phase, "restore-", 8) == 0;
    int target = move->from + move->direction;
    return target >= 0 && target < g->levels.count;
}

static bool save_transition(const game *g, room_transition move)
{
    char line[48];
    const char *phase = move.direction > 0 ? (move.restoring ? "restore-next" : "level") :
                                           (move.restoring ? "restore-back" : "back");
    snprintf(line, sizeof line, "%s %d", phase, move.from + 1);
    return save_state_line(g->state, "transition", line);
}

/* Record the restore phase before moving any files back into the live home. */
static bool finish_transition(const game *g, room_transition move)
{
    if (!move.restoring) {
        if (!archive_workspace(g->workspace, move.from + 1)) return false;
        move.restoring = true;
        if (!save_transition(g, move)) return false;
    }
    int target = move.from + move.direction;
    if (!restore_workspace(g->workspace, target + 1) ||
        !game_save_progress(g->state, target)) return false;
    char marker[4096];
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
    if (lv->check == CHECK_SCRIPT)
        return script_puzzle_check(g->root, g->state, g->workspace, lv->file);
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

static const char *hosted_agent(const game *g)
{
    return g->use_codex ? "Codex" : g->use_grok ? "Grok" : g->use_claude ? "Claude" : NULL;
}

static void models_directory(char *out, size_t cap)
{
    const char *dir = getenv("KILIX_LLM_MODELS_DIR");
    const char *gpu = getenv("GPU_TERMINAL_HOME");
    const char *home = getenv("HOME");
    int n;
    if (dir && *dir) n = snprintf(out, cap, "%s", dir);
    else if (gpu && *gpu) n = snprintf(out, cap, "%s/kilix-llm/models", gpu);
    else n = snprintf(out, cap, "%s/.local/gpu_terminal/kilix-llm/models", home && *home ? home : ".");
    if (n < 0 || (size_t)n >= cap) out[0] = 0;
}

static bool plain_gguf_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    unsigned char first;
    if (n < 6 || n >= OLLAMA_NAME_CAP || strstr(name, "..")) return false;
    first = (unsigned char)name[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
          (first >= '0' && first <= '9') || first == '_'))
        return false;
    if (strcmp(name + n - 5, ".gguf") != 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

static void model_label_for(bool codex, bool grok, const char *ollama, const char *gguf,
                            char out[SCORE_MODEL_CAP])
{
    char model[SCORE_MODEL_CAP];
    const char *weights = gguf && *gguf ? gguf : getenv("PUZZLLM_MODEL");
    const char *leaf = weights ? strrchr(weights, '/') : NULL;
    const char *hosted = codex ? "Codex" : grok ? "Grok" : NULL;
    snprintf(model, sizeof model, "%s%s", hosted ? "" : (ollama && *ollama ? "Ollama/" : "llama.cpp/"),
             hosted ? hosted : ollama && *ollama ? ollama :
             weights && *weights ? (leaf ? leaf + 1 : weights) : "Qwen3.5-0.8B-Q8_0.gguf");
    leaderboard_label(out, SCORE_MODEL_CAP, model);
}

static void current_model_label(const game *g, char out[SCORE_MODEL_CAP])
{
    if (g->use_claude) {
        leaderboard_label(out, SCORE_MODEL_CAP, "Claude");
        return;
    }
    model_label_for(g->use_codex, g->use_grok, g->ollama_model, g->llama_model, out);
}

static void player_name(char out[SCORE_PLAYER_CAP])
{
    const char *player = getenv("PUZZLLM_PLAYER");
    if (!player || !*player) {
        struct passwd *account = getpwuid(getuid());
        player = account ? account->pw_name : "Player";
    }
    leaderboard_label(out, SCORE_PLAYER_CAP, player);
    if (!out[0]) snprintf(out, SCORE_PLAYER_CAP, "Player");
}

static unsigned long long fresh_score_id(void)
{
    unsigned long long id = 0;
    if (getrandom(&id, sizeof id, 0) != (ssize_t)sizeof id || !id)
        id = ((unsigned long long)getpid() << 16) ^ 0x9e3779b97f4a7c15ULL;
    if (!id) id = 1;
    return id;
}

static void apply_locked_model(game *g, const char *label)
{
    bool codex = strncmp(label, "Codex", 5) == 0 && (label[5] == 0 || label[5] == '/');
    bool grok = !codex && strncmp(label, "Grok", 4) == 0 && (label[4] == 0 || label[4] == '/');
    bool claude = !codex && !grok && strncmp(label, "Claude", 6) == 0 &&
                  (label[6] == 0 || label[6] == '/');
    g->use_codex = g->act.codex = codex;
    g->use_grok = g->act.grok = grok;
    g->use_claude = g->act.claude = claude;
    g->ollama_model[0] = 0;
    if (!codex && !grok && !claude && strncmp(label, "Ollama/", 7) == 0)
        snprintf(g->ollama_model, sizeof g->ollama_model, "%s", label + 7);
    if (!codex && !grok && !claude && strncmp(label, "llama.cpp/", 10) == 0 &&
        plain_gguf_name(label + 10)) {
        char dir[4096];
        char path[sizeof g->llama_model];
        models_directory(dir, sizeof dir);
        int n = snprintf(path, sizeof path, "%s/%s", dir, label + 10);
        if (dir[0] && n > 0 && (size_t)n < sizeof path && access(path, R_OK) == 0)
            memcpy(g->llama_model, path, (size_t)n + 1);
    }
    if (codex || grok || claude) g->thinking_support = THINK_UNAVAILABLE;
}

static void stop_metrics(agent_metrics *metrics)
{
    pty_stop(&metrics->worker);
    pty_init(&metrics->worker);
    metrics->length = 0;
    metrics->summary[0] = metrics->devices[0] = 0;
}

static void start_metrics(game *g)
{
    char helper[4096], pid[32];
    const char *gpu = getenv("PUZZLLM_GPU");
    const char *backend = g->use_codex ? "codex" : g->use_grok ? "grok" :
                          g->use_claude ? "claude" : g->ollama_model[0] ? "ollama" : "llama";
    bool cpu = g->compute == COMPUTE_CPU || (g->compute == COMPUTE_AUTO && (!gpu || !*gpu));
    char *argv[] = {helper, pid, (char *)backend, g->ollama_model, cpu ? "cpu" : "auto", NULL};

    snprintf(helper, sizeof helper, "%s/libexec/puzzllm-metrics", g->root);
    snprintf(pid, sizeof pid, "%ld", (long)g->omp.pid);
    if (!pty_spawn(&g->metrics.worker, argv, NULL, 100, 8))
        snprintf(g->metrics.summary, sizeof g->metrics.summary, "Resource data unavailable");
}

static void pump_metrics(game *g)
{
    agent_metrics *metrics = &g->metrics;
    pty_proc *agent = &g->omp;
    if (metrics->worker.fd < 0) return;
    if (pty_poll_exit(agent)) {
        stop_metrics(metrics);
        return;
    }
    char buf[512];
    for (int attempt = 0; attempt < 4; attempt++) {
        long n = pty_read(&metrics->worker, buf, sizeof buf);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\r') continue;
            if (buf[i] != '\n') {
                if (metrics->length < sizeof metrics->partial - 1)
                    metrics->partial[metrics->length++] = buf[i];
                continue;
            }
            metrics->partial[metrics->length] = 0;
            metrics->length = 0;
            char *separator = strchr(metrics->partial, '|');
            if (!separator || strncmp(metrics->partial, "STAT ", 5) != 0) continue;
            *separator = 0;
            char *sample = strchr(separator + 1, '|');
            if (sample) {
                *sample++ = 0;
                long long values[5];
                if (g->ranked_attempt.id &&
                    sscanf(sample, "%lld %lld %lld %lld %lld",
                           &values[0], &values[1], &values[2], &values[3], &values[4]) == 5)
                    for (int j = 0; j < 5; j++)
                        if (values[j] > g->ranked_attempt.measure[SCORE_CPU + j])
                            g->ranked_attempt.measure[SCORE_CPU + j] = values[j];
            }
            snprintf(metrics->summary, sizeof metrics->summary, "%.127s", metrics->partial + 5);
            snprintf(metrics->devices, sizeof metrics->devices, "%.127s", separator + 1);
        }
    }
    if (pty_poll_exit(&metrics->worker)) {
        stop_metrics(metrics);
        snprintf(metrics->summary, sizeof metrics->summary, "Resource data unavailable");
    }
}

static void restore_ranked_attempt(game *g)
{
    g->ranked_attempt = (score_entry){0};
    g->ranked_session_tokens = 0;
    g->ranked_session_path[0] = g->ranked_session_model[0] = 0;
    g->ranked_fraction_ms = 0;
    char path[4096], name[32], line[SCORE_LINE_CAP];
    rank_attempt_name(name, g->sc.level);
    snprintf(path, sizeof path, "%s/%s", g->state, name);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return; }
    score_entry saved = {0};
    if (fgets(line, sizeof line, file) && score_parse(line, &saved) &&
        saved.level == cur_level(g)->number)
        g->ranked_attempt = saved;
    fclose(file);
}

static void checkpoint_ranked_attempt(game *g)
{
    score_entry *score = &g->ranked_attempt;
    if (!score->id || g->state_lock < 0) return;
    if (strcmp(g->ranked_session_path, g->act.session_path)) {
        snprintf(g->ranked_session_path, sizeof g->ranked_session_path, "%s", g->act.session_path);
        g->ranked_session_tokens = 0;
    }
    if (g->ranked_session_model[0] && g->act.tokens_reported &&
        g->act.tokens_used >= g->ranked_session_tokens) {
        unsigned long long delta = g->act.tokens_used - g->ranked_session_tokens;
        g->ranked_session_tokens = g->act.tokens_used;
        if (delta && g->act.codex && g->act.codex_model[0]) {
            char model[SCORE_MODEL_CAP], label[SCORE_MODEL_CAP];
            snprintf(model, sizeof model, "Codex/%s", g->act.codex_model);
            leaderboard_label(label, sizeof label, model);
            if (!strcmp(score->model, "Codex"))
                snprintf(score->model, sizeof score->model, "%s", label);
            else if (strcmp(score->model, label))
                snprintf(score->model, sizeof score->model, "*");
        }
        unsigned long long room = (unsigned long long)(LLONG_MAX - score->measure[SCORE_TOKENS]);
        score->measure[SCORE_TOKENS] += (long long)(delta < room ? delta : room);
    }
    char name[32], line[SCORE_LINE_CAP];
    rank_attempt_name(name, g->sc.level);
    score_format(line, score);
    save_state_line(g->state, name, line);
}

static void begin_ranked_agent(game *g)
{
    score_entry *score = &g->ranked_attempt;
    char clean[SCORE_MODEL_CAP];
    current_model_label(g, clean);
    bool same = !strcmp(score->model, clean) ||
                (g->use_codex && !strncmp(score->model, "Codex/", 6));
    if ((!score->id || (!score->measure[SCORE_TOKENS] && !same)) &&
        !g->sc.goal_met) {
        *score = (score_entry){.level = cur_level(g)->number};
        if (getrandom(&score->id, sizeof score->id, 0) != sizeof score->id) score->id = 0;
        player_name(score->player);
        snprintf(score->model, sizeof score->model, "%s", clean);
        for (int j = SCORE_CPU; j < SCORE_METRICS; j++) score->measure[j] = -1;
    } else if (score->id && !same) {
        snprintf(score->model, sizeof score->model, "*");
    }
    snprintf(g->ranked_session_model, sizeof g->ranked_session_model, "%s", clean);
    g->ranked_session_tokens = 0;
    g->ranked_session_path[0] = 0;
}

static void poll_ranked_attempt(game *g)
{
    if (g->ranked_session_model[0]) {
        activity_poll(&g->act, g->state);
        checkpoint_ranked_attempt(g);
    }
}

static void spawn_agent_prompt(game *g, const char *prompt)
{
    poll_ranked_attempt(g);
    activity_begin_session(&g->act, g->state);
    begin_ranked_agent(g);
    char sandbox[4096], st_env[2100], model_env[OLLAMA_NAME_CAP + 32], yolo_env[18];
    char *mode = g->use_codex ? "codex" : g->use_grok ? "grok" : g->use_claude ? "claude" : "agent";
    char *argv[] = {sandbox, mode, (char *)prompt, NULL};
    char *think_env = g->thinking_enabled ? "PUZZLLM_THINK=1" : "PUZZLLM_THINK=0";
    char compute_env[32];
    snprintf(compute_env, sizeof compute_env, "PUZZLLM_COMPUTE=%s", compute_name[g->compute]);
    snprintf(yolo_env, sizeof yolo_env, "PUZZLLM_YOLO=%d", g->yolo ? 1 : 0);
    char *env[9];
    int en = 0;
    env[en++] = st_env;
    env[en++] = model_env;
    env[en++] = think_env;
    env[en++] = compute_env;
    env[en++] = yolo_env;
    env[en++] = g->use_pi ? "PUZZLLM_HARNESS=pi" : "PUZZLLM_HARNESS=omp";
    env[en++] = "TERM=xterm-256color";
    if (!g->use_codex && !g->use_grok && !g->use_claude && !g->ollama_model[0] && g->llama_model[0]) {
        snprintf(g->gguf_env, sizeof g->gguf_env, "PUZZLLM_MODEL=%s", g->llama_model);
        env[en++] = g->gguf_env;
    }
    env[en] = NULL;

    snprintf(sandbox, sizeof sandbox, "%s/libexec/puzzllm-sandbox", g->root);
    snprintf(st_env, sizeof st_env, "PUZZLLM_STATE=%s", g->state);
    snprintf(model_env, sizeof model_env, "PUZZLLM_OLLAMA_MODEL=%s", g->ollama_model);
    stop_metrics(&g->metrics);
    neuroterm_feed(&g->omp_vt, "\033c", 2);
    history_clear(&g->omp_history);
    g->omp_note[0] = '\0';
    if (!pty_spawn(&g->omp, argv, env, OMP_COLS, OMP_ROWS))
        snprintf(g->omp_note, sizeof g->omp_note, "could not start the agent sandbox");
    else {
        start_metrics(g);
        activity_add(&g->act, ACT_INFO, "agent sandbox started");
    }
}

static void spawn_agent(game *g)
{
    spawn_agent_prompt(g, NULL);
}

static void stop_room_agent(game *g)
{
    stop_metrics(&g->metrics);
    pty_stop(&g->omp);
    poll_ranked_attempt(g);
    g->ranked_session_model[0] = 0;
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
    menu->page = MENU_PAGE_MAIN;
    menu->selected = MENU_HARNESS;
    menu->model_submenu = 0;
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
    g->menu.page = MENU_PAGE_MAIN;
}

static void reset_from_menu(game *g)
{
    if (g->menu.selected == MENU_CLEAR_CAMPAIGN) {
        g->menu.confirm_reset = false;
        if (campaign_board_clear(g->state))
            snprintf(g->menu.notice, sizeof g->menu.notice, "Campaign board cleared.");
        else
            snprintf(g->menu.notice, sizeof g->menu.notice, "Could not clear the campaign board.");
        return;
    }
    bool level_only = g->menu.selected == MENU_RESET_LEVEL;
    int level = level_only ? g->sc.level : 0;
    g->menu.confirm_reset = false;
    if (g->state_lock < 0) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Could not lock the save for reset.");
        return;
    }
    if (level_only && g->transition_pending) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Resume to finish entering the next level.");
        return;
    }
    bool was_started = g->started;
    stop_metrics(&g->metrics);
    pty_stop(&g->omp);
    poll_ranked_attempt(g);
    g->ranked_session_model[0] = 0;
    pty_stop(&g->shell);
    pty_init(&g->omp);
    pty_init(&g->shell);
    g->started = false;
    if (!(level_only ? reset_level_workspace(g) : reset_progress(g->state))) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Reset failed: %s", strerror(errno));
        if (level_only && was_started && g->spawn) {
            g->started = true;
            spawn_agent(g);
            spawn_shell(g);
        } else if (level_only) g->started = was_started;
        return;
    }
    neuroterm_feed(&g->omp_vt, "\033c", 2);
    history_clear(&g->omp_history);
    neuroterm_feed(&g->shell_vt, "\033c", 2);
    history_clear(&g->shell_history);
    g->omp_note[0] = g->shell_note[0] = 0;
    g->transition_pending = false;
    g->tick_debt = g->poll_timer = 0;
    if (level_only) clear_rank_attempt(g->state, level);
    g->menu.notice[0] = 0;
    if (g->campaign) {
        char player[SCORE_PLAYER_CAP];
        player_name(player);
        bool kept = level_only ?
            campaign_drop_level(g->state, g->levels.items[level].number, fresh_score_id()) :
            campaign_restart(g->state, g->campaign_model, player, fresh_score_id());
        if (!kept)
            snprintf(g->menu.notice, sizeof g->menu.notice, "Could not update the campaign run.");
    }
    game_enter_level(g, level);
    g->menu.page = MENU_PAGE_MAIN;
    g->menu.selected = MENU_RESUME;
    if (level_only) start_play(g);
    else if (!g->menu.notice[0])
        snprintf(g->menu.notice, sizeof g->menu.notice, "Game reset. Previous files backed up.");
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

static void model_details_scroll(game_menu *menu, int rows)
{
    int limit = render_model_details(NULL, menu->details_tab, 0,
                                     menu->prompt, menu->write_schema,
                                     menu->prompt_count);
    int next = menu->details_scroll + rows;
    menu->details_scroll = next < 0 ? 0 : next > limit ? limit : next;
}

static void refresh_model_details(game *g);

static void model_details_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    switch (ev->key) {
    case KEY_LEFT:
    case KEY_RIGHT:
        if (ev->action == 1) {
            int change = ev->key == KEY_LEFT ? DETAILS_COUNT - 1 : 1;
            menu->details_tab = (menu->details_tab + change) % DETAILS_COUNT;
            menu->details_scroll = 0;
            refresh_model_details(g);
        }
        break;
    case KEY_UP: model_details_scroll(menu, -1); break;
    case KEY_DOWN: model_details_scroll(menu, 1); break;
    case KEY_PAGE_UP: model_details_scroll(menu, -16); break;
    case KEY_PAGE_DOWN: model_details_scroll(menu, 16); break;
    case KEY_HOME: model_details_scroll(menu, -MODEL_SPEC_CAP); break;
    case KEY_END: model_details_scroll(menu, MODEL_SPEC_CAP); break;
    default: break;
    }
}

static void read_model_spec(const char *root, const char *name, char text[MODEL_SPEC_CAP])
{
    char path[4096];
    snprintf(path, sizeof path, "%s/libexec/%s", root, name);
    text[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    text[fread(text, 1, MODEL_SPEC_CAP - 1, f)] = '\0';
    fclose(f);
}

static const char *prompt_file(const game *g)
{
    return g->menu.details_tab == DETAILS_OLLAMA ||
           (g->menu.details_tab == DETAILS_PI && g->ollama_model[0]) ?
           "ollama-prompt.txt" : "llama-prompt.txt";
}

static int system_folder(const game *g, bool create)
{
    int workspace = open(g->workspace, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (workspace < 0) return -1;
    if (create) mkdirat(workspace, ".system", 0700);
    int folder = openat(workspace, ".system", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(workspace);
    return folder;
}

static void seed_system_files(const game *g)
{
    const char *names[] = {"ollama-prompt.txt", "llama-prompt.txt", "write-tool.json"};
    const char *sources[] = {"puzzllm-ollama-prompt.txt", NULL, "puzzllm-write-tool.json"};
    int folder = system_folder(g, true);
    if (folder < 0) return;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        int fd = openat(folder, names[i], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) continue;
        char text[MODEL_SPEC_CAP] = "";
        if (sources[i]) read_model_spec(g->root, sources[i], text);
        size_t len = strlen(text);
        bool ok = write(fd, text, len) == (ssize_t)len;
        if (close(fd) != 0) ok = false;
        if (!ok) unlinkat(folder, names[i], 0);
    }
    close(folder);
}

static void read_system_file(int folder, const char *name, char text[MODEL_SPEC_CAP])
{
    struct stat st;
    int fd = openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    text[0] = 0;
    if (fd < 0) return;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        ssize_t n = read(fd, text, MODEL_SPEC_CAP - 1);
        if (n > 0) {
            while (n > 0 && text[n - 1] == '\n') n--;
            text[n] = 0;
        }
    }
    close(fd);
}

static void refresh_model_details(game *g)
{
    game_menu *menu = &g->menu;
    int folder = system_folder(g, false);
    menu->prompt[0] = menu->write_schema[0] = 0;
    if (folder >= 0) {
        read_system_file(folder, prompt_file(g), menu->prompt);
        read_system_file(folder, "write-tool.json", menu->write_schema);
        close(folder);
    }
    pty_stop(&menu->prompt_counter);
    pty_init(&menu->prompt_counter);
    menu->prompt_count[0] = 0;
    menu->prompt_input_len = 0;
    if (!menu->prompt[0] || !g->spawn || menu->details_tab == DETAILS_CODEX ||
        menu->details_tab == DETAILS_GROK || menu->details_tab == DETAILS_CLAUDE) return;
    char helper[4096], input[MODEL_SPEC_CAP + 32];
    snprintf(helper, sizeof helper, "%s/libexec/puzzllm-prompt-tokens", g->root);
    snprintf(input, sizeof input, "PUZZLLM_PROMPT_TEXT=%s", menu->prompt);
    char *argv[] = {helper, NULL};
    char *env[] = {input, NULL};
    snprintf(menu->prompt_count, sizeof menu->prompt_count, "Counting file text...");
    if (!pty_spawn(&menu->prompt_counter, argv, env, 80, 24))
        snprintf(menu->prompt_count, sizeof menu->prompt_count, "Tokenizer unavailable");
}

static void poll_prompt_count(game_menu *menu)
{
    if (menu->prompt_counter.fd < 0) return;
    char buf[64];
    long n = pty_read(&menu->prompt_counter, buf, sizeof buf);
    if (n > 0) {
        size_t room = sizeof menu->prompt_input - 1 - menu->prompt_input_len;
        size_t copy = (size_t)n < room ? (size_t)n : room;
        memcpy(menu->prompt_input + menu->prompt_input_len, buf, copy);
        menu->prompt_input_len += copy;
        return;
    }
    if (!pty_poll_exit(&menu->prompt_counter)) return;
    menu->prompt_input[menu->prompt_input_len] = 0;
    char *end;
    long count = strtol(menu->prompt_input, &end, 10);
    if (menu->prompt_counter.exit_status == 0 && end != menu->prompt_input &&
        (*end == '\r' || *end == '\n' || !*end))
        snprintf(menu->prompt_count, sizeof menu->prompt_count, "File text: %ld tokens (local GGUF)", count);
    else
        snprintf(menu->prompt_count, sizeof menu->prompt_count, "Tokenizer unavailable");
    pty_stop(&menu->prompt_counter);
}

static void insert_prompt_text(game_menu *menu, const char *text, size_t len)
{
    size_t end = strlen(menu->prompt_draft);
    if (len > sizeof menu->prompt_draft - end - 1) {
        snprintf(menu->notice, sizeof menu->notice, "Prompt limit is %d bytes.", MODEL_SPEC_CAP - 1);
        return;
    }
    char *cursor = menu->prompt_draft + menu->prompt_cursor;
    memmove(cursor + len, cursor, end - menu->prompt_cursor + 1);
    memcpy(cursor, text, len);
    menu->prompt_cursor += len;
    menu->notice[0] = 0;
}

static bool save_system_prompt(const game *g)
{
    int folder = system_folder(g, true);
    if (folder < 0) return false;
    const char *temp = ".prompt-save";
    unlinkat(folder, temp, 0);
    int fd = openat(folder, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = false;
    if (fd >= 0) {
        const char *draft = g->menu.prompt_draft;
        size_t len = strlen(draft);
        ok = write(fd, draft, len) == (ssize_t)len;
        if (close(fd) != 0) ok = false;
        if (ok) ok = renameat(folder, temp, folder, prompt_file(g)) == 0;
        if (!ok) unlinkat(folder, temp, 0);
    }
    close(folder);
    return ok;
}

static size_t prompt_step(const char *text, size_t at, int delta)
{
    if (delta < 0 && at > 0) {
        do at--; while (at > 0 && ((unsigned char)text[at] & 0xc0) == 0x80);
    } else if (delta > 0 && text[at]) {
        do at++; while (((unsigned char)text[at] & 0xc0) == 0x80);
    }
    return at;
}

static void prompt_editor_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    char *text = menu->prompt_draft;
    size_t at = menu->prompt_cursor;
    if (ev->mods & MOD_CTRL) {
        if (ev->action != 1) return;
        if (ev->key == 's' || ev->key == 'S') {
            if (g->state_lock < 0 || !save_system_prompt(g)) {
                snprintf(menu->notice, sizeof menu->notice, "Could not save the prompt in ~/.system.");
                return;
            }
            menu->prompt_edit = false;
            menu->details_scroll = 0;
            refresh_model_details(g);
            bool editing_ollama = strcmp(prompt_file(g), "ollama-prompt.txt") == 0;
            if (!hosted_agent(g) && editing_ollama == (g->ollama_model[0] != 0))
                restart_agent(g);
        } else if (ev->key == 'u' || ev->key == 'U') {
            text[0] = 0;
            menu->prompt_cursor = 0;
        }
        return;
    }
    switch (ev->key) {
    case KEY_HOME:
        while (at && text[at - 1] != '\n') at--;
        menu->prompt_cursor = at;
        return;
    case KEY_END:
        menu->prompt_cursor = at + strcspn(text + at, "\n");
        return;
    case KEY_LEFT: case KEY_RIGHT:
        menu->prompt_cursor = prompt_step(text, at, ev->key == KEY_LEFT ? -1 : 1);
        return;
    case KEY_UP: case KEY_DOWN: {
        size_t begin = at, column = 0;
        while (begin && text[begin - 1] != '\n') {
            begin = prompt_step(text, begin, -1);
            column++;
        }
        if (ev->key == KEY_UP) {
            if (!begin) return;
            at = begin - 1;
            while (at && text[at - 1] != '\n') at--;
        } else {
            at += strcspn(text + at, "\n");
            if (!text[at]) return;
            at++;
        }
        while (column-- && text[at] && text[at] != '\n') at = prompt_step(text, at, 1);
        menu->prompt_cursor = at;
        return;
    }
    case KEY_BACKSPACE: case KEY_DELETE: {
        size_t first = ev->key == KEY_BACKSPACE ? prompt_step(text, at, -1) : at;
        size_t last = ev->key == KEY_DELETE ? prompt_step(text, at, 1) : at;
        memmove(text + first, text + last, strlen(text + last) + 1);
        menu->prompt_cursor = first;
        return;
    }
    case KEY_ENTER:
        insert_prompt_text(menu, "\n", 1);
        return;
    default:
        break;
    }
    if (ev->mods & (MOD_ALT | MOD_SUPER)) return;
    for (int i = 0; i < (ev->text_len ? ev->text_len : 1); i++) {
        uint32_t ch = ev->text_len ? ev->text[i] : ev->key;
        if (ch < 32 || ch == 127 || (!ev->text_len && ch >= 0xe000 && ch <= 0xf8ff)) continue;
        char encoded[4];
        size_t n = keys_utf8(ch, encoded);
        insert_prompt_text(menu, encoded, n);
    }
}

static bool campaign_choice_allowed(const game *g, bool codex, bool grok, const char *name)
{
    char label[SCORE_MODEL_CAP];
    if (!g->campaign) return true;
    model_label_for(codex, grok, name, g->llama_model, label);
    return campaign_same_model(g->campaign_model, label);
}

static void select_model(game *g, bool codex, bool grok, const model_option *model)
{
    const char *name = model ? model->name : "";
    if (g->campaign && !campaign_choice_allowed(g, codex, grok, name)) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Campaign is locked to %s.", g->campaign_model);
        return;
    }
    thinking_support support = (codex || grok) ? THINK_UNAVAILABLE :
                               model ? model->thinking : bundled_thinking();
    if (codex != g->use_codex || grok != g->use_grok || g->use_claude ||
        strcmp(name, g->ollama_model) != 0 || support != g->thinking_support) {
        g->thinking_enabled = false;
        g->thinking_support = support;
        snprintf(g->ollama_model, sizeof g->ollama_model, "%s", name);
        if (g->use_codex != codex || g->use_grok != grok || g->use_claude) {
            poll_ranked_attempt(g);
            g->ranked_session_model[0] = 0;
            g->use_codex = codex;
            g->act.codex = codex;
            g->use_grok = g->act.grok = grok;
            g->use_claude = g->act.claude = false;
            activity_begin_room(&g->act, g->state, g->workspace);
        }
        restart_agent(g);
    } else if (g->spawn && g->started && pty_poll_exit(&g->omp)) {
        restart_agent(g);
    }
    g->menu.model_submenu = 0;
    start_play(g);
}

static void select_claude(game *g)
{
    char label[SCORE_MODEL_CAP];
    leaderboard_label(label, SCORE_MODEL_CAP, "Claude");
    if (g->campaign && !campaign_same_model(g->campaign_model, label)) {
        snprintf(g->menu.notice, sizeof g->menu.notice, "Campaign is locked to %s.", g->campaign_model);
        return;
    }
    if (!g->use_claude) {
        g->thinking_support = THINK_UNAVAILABLE;
        g->thinking_enabled = g->thinking_support == THINK_SWITCHABLE;
        g->ollama_model[0] = 0;
        poll_ranked_attempt(g);
        g->ranked_session_model[0] = 0;
        g->use_codex = g->act.codex = false;
        g->use_grok = g->act.grok = false;
        g->use_claude = g->act.claude = true;
        activity_begin_room(&g->act, g->state, g->workspace);
        restart_agent(g);
    } else if (g->spawn && g->started && pty_poll_exit(&g->omp)) {
        restart_agent(g);
    }
    g->menu.model_submenu = 0;
    start_play(g);
}

static const struct {
    int id;
    const char *kind;
    const char *arg;
} download_catalog[] = {
    {MENU_DL_GGUF_08, "gguf", "qwen3.5-0.8b"},
    {MENU_DL_GGUF_4B, "gguf", "qwen3.5-4b-q4"},
    {MENU_DL_OLLAMA_06, "ollama", "qwen3:0.6b"},
    {MENU_DL_OLLAMA_4B, "ollama", "qwen3:4b-instruct-2507"},
};

static bool ollama_pull_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 128 || strstr(name, "..") || strstr(name, "//")) return false;
    unsigned char first = (unsigned char)name[0], last = (unsigned char)name[n - 1];
    if (first == '/' || first == ':' || first == '.' || first == '-' || last == '/' || last == ':')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == ':' || c == '/' || c == '-')
            continue;
        return false;
    }
    return true;
}

static void scan_ggufs(game_menu *menu)
{
    char dir[4096];
    menu->gguf_count = 0;
    models_directory(dir, sizeof dir);
    if (!dir[0]) return;
    DIR *folder = opendir(dir);
    if (!folder) return;
    struct dirent *ent;
    while ((ent = readdir(folder)) != NULL && menu->gguf_count < 32) {
        char path[4600];
        struct stat st;
        int n;
        if (!plain_gguf_name(ent->d_name)) continue;
        n = snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) continue;
        snprintf(menu->gguf[menu->gguf_count].name, sizeof menu->gguf[0].name, "%s", ent->d_name);
        menu->gguf[menu->gguf_count].thinking = THINK_UNAVAILABLE;
        menu->gguf_count++;
    }
    closedir(folder);
    for (int i = 1; i < menu->gguf_count; i++) {
        model_option item = menu->gguf[i];
        int j = i;
        bool item_default = strcmp(item.name, "Qwen3.5-0.8B-Q8_0.gguf") == 0;
        while (j > 0) {
            bool earlier_default = strcmp(menu->gguf[j - 1].name, "Qwen3.5-0.8B-Q8_0.gguf") == 0;
            if (earlier_default || (!item_default && strcmp(menu->gguf[j - 1].name, item.name) <= 0))
                break;
            menu->gguf[j] = menu->gguf[j - 1];
            j--;
        }
        menu->gguf[j] = item;
    }
}

static bool gguf_path(const game_menu *menu, int index, char *out, size_t cap)
{
    char dir[4096];
    int n;
    if (index < 0 || index >= menu->gguf_count) return false;
    models_directory(dir, sizeof dir);
    n = snprintf(out, cap, "%s/%s", dir, menu->gguf[index].name);
    return dir[0] && n > 0 && (size_t)n < cap && access(out, R_OK) == 0;
}

static void start_fetch(game *g, const char *kind, const char *arg)
{
    game_menu *menu = &g->menu;
    const char *override = getenv("PUZZLLM_FETCH");
    int n;
    char *argv[4];
    if (menu->fetching || menu->yesno_running) {
        snprintf(menu->notice, sizeof menu->notice, "A download is already running.");
        return;
    }
    if (override && *override)
        n = snprintf(menu->fetch_helper, sizeof menu->fetch_helper, "%s", override);
    else
        n = snprintf(menu->fetch_helper, sizeof menu->fetch_helper, "%s/libexec/puzzllm-fetch", g->root);
    if (n < 0 || (size_t)n >= sizeof menu->fetch_helper) {
        snprintf(menu->notice, sizeof menu->notice, "Could not start the download.");
        return;
    }
    snprintf(menu->fetch_kind, sizeof menu->fetch_kind, "%s", kind);
    snprintf(menu->fetch_arg, sizeof menu->fetch_arg, "%s", arg);
    menu->fetch_line[0] = 0;
    menu->fetch_line_len = 0;
    argv[0] = menu->fetch_helper;
    argv[1] = menu->fetch_kind;
    argv[2] = menu->fetch_arg;
    argv[3] = NULL;
    menu->fetching = pty_spawn(&menu->fetch, argv, NULL, 80, 24);
    snprintf(menu->notice, sizeof menu->notice, "%s", menu->fetching ?
             "Downloading on this computer..." : "Could not start the download.");
}

static void poll_fetch(game_menu *menu)
{
    char buf[1024];
    if (!menu->fetching) return;
    for (int reads = 0; reads < 8; reads++) {
        long n = pty_read(&menu->fetch, buf, sizeof buf);
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    menu->fetch_line[menu->fetch_line_len] = 0;
                    if (menu->fetch_line_len)
                        snprintf(menu->notice, sizeof menu->notice, "%s", menu->fetch_line);
                    menu->fetch_line_len = 0;
                } else if (buf[i] != '\r' && menu->fetch_line_len + 1 < sizeof menu->fetch_line) {
                    menu->fetch_line[menu->fetch_line_len++] = buf[i];
                }
            }
        } else {
            if (n < 0 && pty_poll_exit(&menu->fetch)) {
                if (menu->fetch_line_len) {
                    menu->fetch_line[menu->fetch_line_len] = 0;
                    snprintf(menu->notice, sizeof menu->notice, "%s", menu->fetch_line);
                }
                if (menu->fetch.exit_status != 0 &&
                    (!menu->notice[0] || strncmp(menu->notice, "Downloading", 11) == 0))
                    snprintf(menu->notice, sizeof menu->notice, "Download failed.");
                menu->fetching = false;
                pty_stop(&menu->fetch);
            }
            break;
        }
    }
}

static void poll_yesno(game_menu *menu)
{
    char buf[1024];
    if (!menu->yesno_running) return;
    for (int reads = 0; reads < 8; reads++) {
        long n = pty_read(&menu->yesno, buf, sizeof buf);
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    menu->yesno_line[menu->yesno_line_len] = 0;
                    if (menu->yesno_line_len)
                        snprintf(menu->notice, sizeof menu->notice, "%s", menu->yesno_line);
                    menu->yesno_line_len = 0;
                } else if (buf[i] != '\r' && menu->yesno_line_len + 1 < sizeof menu->yesno_line) {
                    menu->yesno_line[menu->yesno_line_len++] = buf[i];
                }
            }
        } else {
            if (n < 0 && pty_poll_exit(&menu->yesno)) {
                if (menu->yesno_line_len) {
                    menu->yesno_line[menu->yesno_line_len] = 0;
                    snprintf(menu->notice, sizeof menu->notice, "%s", menu->yesno_line);
                }
                if (menu->yesno.exit_status != 0 &&
                    (!menu->notice[0] || strncmp(menu->notice, "Starting", 8) == 0))
                    snprintf(menu->notice, sizeof menu->notice, "Kilix yes/no bench stopped.");
                menu->yesno_running = false;
                pty_stop(&menu->yesno);
            }
            break;
        }
    }
}

static void offer_autotrust(game *g)
{
    if (!g->spawn) {
        snprintf(g->menu.notice, sizeof g->menu.notice,
                 "autotrust-jev downloads on this computer when the game is running.");
        return;
    }
    start_fetch(g, "ollama", "autotrust-jev");
}

static void start_yesno(game *g)
{
    game_menu *menu = &g->menu;
    char helper[4096];
    char *argv[2];
    int n;
    if (!g->spawn) {
        snprintf(menu->notice, sizeof menu->notice,
                 "Kilix yes/no bench runs autotrust-jev and downloads it when it is missing.");
        return;
    }
    if (menu->fetching || menu->yesno_running) {
        snprintf(menu->notice, sizeof menu->notice, "Kilix yes/no bench is already running.");
        return;
    }
    n = snprintf(helper, sizeof helper, "%s/libexec/puzzllm-yesno", g->root);
    if (n < 0 || (size_t)n >= sizeof helper) {
        snprintf(menu->notice, sizeof menu->notice, "Could not start the Kilix yes/no bench.");
        return;
    }
    argv[0] = helper;
    argv[1] = NULL;
    menu->yesno_line_len = 0;
    menu->yesno_line[0] = 0;
    menu->yesno_running = pty_spawn(&menu->yesno, argv, NULL, 80, 24);
    snprintf(menu->notice, sizeof menu->notice, "%s", menu->yesno_running ?
             "Starting the Kilix yes/no bench with autotrust-jev." :
             "Could not start the Kilix yes/no bench.");
}

static void choose_llama(game *g, const char *path)
{
    char previous[sizeof g->llama_model];
    snprintf(previous, sizeof previous, "%s", g->llama_model);
    if (path) snprintf(g->llama_model, sizeof g->llama_model, "%s", path);
    else g->llama_model[0] = 0;
    if (g->campaign && !campaign_choice_allowed(g, false, false, "")) {
        snprintf(g->llama_model, sizeof g->llama_model, "%s", previous);
        snprintf(g->menu.notice, sizeof g->menu.notice, "Campaign is locked to %s.", g->campaign_model);
        return;
    }
    select_model(g, false, false, NULL);
}

static void append_pull_name(game_menu *menu, unsigned char c)
{
    size_t n = strlen(menu->pull_name);
    if (n + 1 >= sizeof menu->pull_name) return;
    if (c == '-' && n == 0) return;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '.' || c == ':' || c == '/' || c == '-') {
        menu->pull_name[n] = (char)c;
        menu->pull_name[n + 1] = 0;
    }
}

static void name_menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    if (ev->action != 1) return;
    if (ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE) {
        size_t n = strlen(menu->pull_name);
        if (n) menu->pull_name[n - 1] = 0;
    } else if (ev->key == KEY_ENTER) {
        if (!ollama_pull_name(menu->pull_name)) {
            snprintf(menu->notice, sizeof menu->notice,
                     "Ollama names use letters, digits, and . _ : / -.");
            return;
        }
        menu->name_edit = false;
        start_fetch(g, "ollama", menu->pull_name);
        return;
    } else if (ev->text_len) {
        for (int i = 0; i < ev->text_len; i++)
            if (ev->text[i] < 128) append_pull_name(menu, (unsigned char)ev->text[i]);
    } else if (ev->key >= 32 && ev->key < 127) {
        append_pull_name(menu, (unsigned char)ev->key);
    } else {
        return;
    }
    snprintf(menu->notice, sizeof menu->notice, "Name: %s", menu->pull_name);
}

static void model_menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    bool ollama = menu->model_submenu == MENU_OLLAMA;
    int count = ollama ? menu->count : (menu->gguf_count > 0 ? menu->gguf_count : 1);
    if (!count) return;
    switch (ev->key) {
    case KEY_UP:
        if (menu->model_selected > 0) menu->model_selected--;
        break;
    case KEY_DOWN:
        if (menu->model_selected < count - 1) menu->model_selected++;
        break;
    case KEY_HOME: menu->model_selected = 0; break;
    case KEY_END: menu->model_selected = count - 1; break;
    case KEY_ENTER:
        if (ev->action != 1) break;
        if (ollama) {
            if (menu->count > 0)
                select_model(g, false, false, &menu->models[menu->model_selected]);
        } else if (menu->gguf_count > 0) {
            char path[sizeof g->llama_model];
            if (gguf_path(menu, menu->model_selected, path, sizeof path))
                choose_llama(g, path);
            else
                snprintf(menu->notice, sizeof menu->notice,
                         "That GGUF is no longer in the models folder.");
        } else {
            choose_llama(g, NULL);
        }
        break;
    default: break;
    }
}

static void rank_menu_refresh(game_menu *menu)
{
    leaderboard_rank(menu->scores, menu->score_count, menu->score_level,
                     menu->score_metric, menu->model_scores);
    menu->score_scroll = 0;
}

static int playable_level_numbers(const game *g, int out[LEVEL_MAX])
{
    int count = 0;
    for (int i = 0; i < g->levels.count; i++)
        if (g->levels.items[i].check != CHECK_NONE) out[count++] = g->levels.items[i].number;
    return count;
}

static void open_score_board(game *g, bool campaign)
{
    game_menu *menu = &g->menu;
    free(menu->scores);
    bool loaded = campaign ? campaign_load(g->state, &menu->scores, &menu->score_count) :
                  leaderboard_load(g->state, &menu->scores, &menu->score_count);
    if (!loaded)
        snprintf(menu->notice, sizeof menu->notice, campaign ?
                 "Could not load the campaign board." : "Could not load the local leaderboard.");
    menu->leaderboard_open = true;
    menu->campaign_board = campaign;
    menu->model_scores = true;
    if (campaign) {
        int needed[LEVEL_MAX];
        menu->score_level = playable_level_numbers(g, needed);
    } else {
        menu->score_level = cur_level(g)->number;
    }
    menu->score_metric = SCORE_TOKENS;
    rank_menu_refresh(menu);
}

static void open_leaderboard(game *g)
{
    open_score_board(g, false);
}

static void rank_menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    int step = 0;
    switch (ev->key) {
    case KEY_LEFT:
    case KEY_RIGHT: {
        int room = menu->score_level + (ev->key == KEY_LEFT ? -1 : 1);
        if (room >= 1 && room <= g->levels.count) {
            menu->score_level = room;
            rank_menu_refresh(menu);
        }
        return;
    }
    case KEY_TAB:
        if (ev->action == 1) {
            menu->model_scores = !menu->model_scores;
            rank_menu_refresh(menu);
        }
        return;
    case 'm': case 'M':
        if (ev->action == 1 && menu->model_scores) {
            menu->score_metric = (menu->score_metric + 1) % SCORE_METRICS;
            rank_menu_refresh(menu);
        }
        return;
    case KEY_UP: step = -1; break;
    case KEY_DOWN: step = 1; break;
    case KEY_PAGE_UP: step = -12; break;
    case KEY_PAGE_DOWN: step = 12; break;
    default: return;
    }
    int limit = render_leaderboard(NULL, menu->scores, menu->score_count, 0, 0, 0, false, "", NULL, false);
    int next = menu->score_scroll + step;
    menu->score_scroll = next < 0 ? 0 : next > limit ? limit : next;
}

static int menu_row_index(const game *g)
{
    int rows[12];
    int n = menu_page_rows(g->menu.page, draw_neuronte_available(), rows, 12);
    for (int i = 0; i < n; i++) if (rows[i] == g->menu.selected) return i;
    return 0;
}

static void menu_move(game *g, int index)
{
    int rows[12];
    int n = menu_page_rows(g->menu.page, draw_neuronte_available(), rows, 12);
    if (n <= 0) return;
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;
    g->menu.selected = rows[index];
}

static void menu_key(game *g, const key_event *ev)
{
    game_menu *menu = &g->menu;
    if (menu->leaderboard_open) {
        rank_menu_key(g, ev);
        return;
    }
    if (menu->details_open) {
        if (menu->prompt_edit) prompt_editor_key(g, ev);
        else if (ev->key == KEY_ENTER && ev->action == 1 && menu->details_tab != DETAILS_CODEX &&
                 menu->details_tab != DETAILS_GROK && menu->details_tab != DETAILS_CLAUDE) {
            snprintf(menu->prompt_draft, sizeof menu->prompt_draft, "%s", menu->prompt);
            menu->prompt_cursor = strlen(menu->prompt_draft);
            menu->notice[0] = 0;
            menu->prompt_edit = true;
        } else model_details_key(g, ev);
        return;
    }
    if (menu->confirm_reset) {
        if (ev->key == KEY_ENTER && ev->action != 2) reset_from_menu(g);
        return;
    }
    if (menu->model_submenu) {
        model_menu_key(g, ev);
        return;
    }
    if (menu->name_edit) {
        name_menu_key(g, ev);
        return;
    }
    switch (ev->key) {
    case KEY_UP:
        menu_move(g, menu_row_index(g) - 1);
        break;
    case KEY_DOWN:
        menu_move(g, menu_row_index(g) + 1);
        break;
    case KEY_HOME:
        menu_move(g, 0);
        break;
    case KEY_END:
        menu_move(g, 100);
        break;
    case KEY_ENTER:
        if (ev->action == 2) break;
        menu->notice[0] = 0;
        if (menu->selected == MENU_MODEL || menu->selected == MENU_SCORES ||
            menu->selected == MENU_DISPLAY || menu->selected == MENU_BENCH ||
            menu->selected == MENU_RESETS) {
            menu->page = menu->selected == MENU_MODEL ? MENU_PAGE_MODEL :
                         menu->selected == MENU_SCORES ? MENU_PAGE_SCORES :
                         menu->selected == MENU_DISPLAY ? MENU_PAGE_DISPLAY :
                         menu->selected == MENU_BENCH ? MENU_PAGE_BENCH : MENU_PAGE_RESET;
            menu_move(g, 0);
            break;
        }
        if (menu->selected == MENU_RESUME) start_play(g);
        if (menu->selected == MENU_QUIT) g->quit = true;
        if (menu->selected == MENU_RESET || menu->selected == MENU_RESET_LEVEL ||
            menu->selected == MENU_CLEAR_CAMPAIGN)
            menu->confirm_reset = true;
        if (menu->selected == MENU_CAMPAIGN) {
            bool enabled = !g->campaign;
            char label[SCORE_MODEL_CAP], player[SCORE_PLAYER_CAP];
            current_model_label(g, label);
            if (enabled && !(g->campaign_model[0] && campaign_same_model(g->campaign_model, label))) {
                player_name(player);
                if (!campaign_restart(g->state, label, player, fresh_score_id())) {
                    snprintf(menu->notice, sizeof menu->notice, "Could not start the campaign.");
                    break;
                }
                snprintf(g->campaign_model, sizeof g->campaign_model, "%s", label);
            }
            if (g->state_lock < 0 || !save_state_line(g->state, "campaign", enabled ? "on" : "off"))
                snprintf(menu->notice, sizeof menu->notice, "Could not save the campaign setting.");
            else {
                g->campaign = enabled;
                if (enabled)
                    snprintf(menu->notice, sizeof menu->notice, "Campaign locked to %s.", g->campaign_model);
                else
                    snprintf(menu->notice, sizeof menu->notice, "Campaign is off.");
            }
        }
        if (menu->selected == MENU_CAMPAIGN_BOARD) open_score_board(g, true);
        if (menu->selected == MENU_HARNESS && !hosted_agent(g)) {
            bool pi = !g->use_pi;
            if (g->state_lock >= 0 && save_state_line(g->state, "harness", pi ? "pi" : "omp")) {
                pty_stop(&g->omp);
                poll_ranked_attempt(g);
                g->ranked_session_model[0] = 0;
                g->use_pi = g->act.pi = pi;
                activity_begin_room(&g->act, g->state, g->workspace);
                if (g->spawn && g->started && !g->transition_pending) spawn_agent(g);
            } else {
                snprintf(menu->notice, sizeof menu->notice, "Could not save the harness setting.");
            }
        }
        if (menu->selected == MENU_THINKING && g->thinking_support != THINK_UNAVAILABLE) {
            g->thinking_enabled = !g->thinking_enabled;
            restart_agent(g);
        }
        if (menu->selected == MENU_COMPUTE && !hosted_agent(g)) {
            compute_mode compute = g->compute == COMPUTE_GPU ? COMPUTE_AUTO : g->compute + 1;
            if (g->state_lock >= 0 && save_state_line(g->state, "compute", compute_name[compute])) {
                g->compute = compute;
                restart_agent(g);
            } else {
                snprintf(menu->notice, sizeof menu->notice, "Could not save the compute setting.");
            }
        }
        if (menu->selected == MENU_TOOLTIPS) {
            bool enabled = !g->tooltips;
            if (g->state_lock >= 0 && save_state_line(g->state, "tooltips", enabled ? "on" : "off"))
                g->tooltips = enabled;
            else
                snprintf(menu->notice, sizeof menu->notice, "Could not save the tooltip setting.");
        }
        if (menu->selected == MENU_ROOM_BUTTONS) {
            bool enabled = !g->room_buttons;
            if (g->state_lock >= 0 && save_state_line(g->state, "room-buttons", enabled ? "on" : "off"))
                g->room_buttons = enabled;
            else
                snprintf(menu->notice, sizeof menu->notice, "Could not save the level buttons setting.");
        }
        if (menu->selected == MENU_HINTS_FILE) {
            bool enabled = !g->hints_file;
            if (g->state_lock < 0 || !save_state_line(g->state, "hints-file", enabled ? "on" : "off"))
                snprintf(menu->notice, sizeof menu->notice, "Could not save the hint file setting.");
            else if (!(enabled ? save_hints_file(g) : remove_workspace_file(g, "HINTS.md"))) {
                save_state_line(g->state, "hints-file", g->hints_file ? "on" : "off");
                snprintf(menu->notice, sizeof menu->notice, "Could not save the hint file setting.");
            } else {
                g->hints_file = enabled;
            }
        }
        if (menu->selected == MENU_NEURONTE && draw_neuronte_available()) {
            bool enabled = !draw_neuronte_enabled();
            if (!draw_set_neuronte(enabled))
                snprintf(menu->notice, sizeof menu->notice, "Could not open Neuronte v2 weights.");
            else if (g->state_lock < 0 || !save_state_line(g->state, "neuronte", enabled ? "on" : "off")) {
                draw_set_neuronte(!enabled);
                snprintf(menu->notice, sizeof menu->notice, "Could not save the renderer setting.");
            }
        }
        if (menu->selected == MENU_YOLO) {
            bool enabled = !g->yolo;
            if (g->state_lock < 0 || !save_state_line(g->state, "yolo", enabled ? "on" : "off"))
                snprintf(menu->notice, sizeof menu->notice, "Could not save the yolo setting.");
            else {
                g->yolo = enabled;
                restart_agent(g);
            }
        }
        if (menu->selected == MENU_MODEL_DETAILS) {
            menu->details_open = true;
            menu->details_scroll = 0;
            menu->details_tab = g->use_codex ? DETAILS_CODEX : g->use_grok ? DETAILS_GROK :
                                g->use_claude ? DETAILS_CLAUDE : g->use_pi ? DETAILS_PI :
                                g->ollama_model[0] ? DETAILS_OLLAMA : DETAILS_LLAMA;
            refresh_model_details(g);
        }
        if (menu->selected == MENU_LEADERBOARD) open_leaderboard(g);
        if (menu->selected == MENU_YESNO) start_yesno(g);
        if (menu->selected == MENU_AUTOTRUST) offer_autotrust(g);
        if (menu->selected == MENU_CODEX) select_model(g, true, false, NULL);
        if (menu->selected == MENU_GROK) select_model(g, false, true, NULL);
        if (menu->selected == MENU_CLAUDE) select_claude(g);
        if (menu->selected == MENU_DOWNLOADS) {
            menu->page = MENU_PAGE_DOWNLOAD;
            menu->name_edit = false;
            menu_move(g, 0);
        }
        if (menu->selected == MENU_DL_NAME) {
            menu->name_edit = true;
            menu->pull_name[0] = 0;
            snprintf(menu->notice, sizeof menu->notice, "Name: ");
        }
        for (size_t i = 0; i < sizeof download_catalog / sizeof download_catalog[0]; i++) {
            if (menu->selected == download_catalog[i].id)
                start_fetch(g, download_catalog[i].kind, download_catalog[i].arg);
        }
        if (menu->selected == MENU_LOCAL || menu->selected == MENU_OLLAMA) {
            menu->model_submenu = menu->selected;
            menu->model_selected = 0;
            if (menu->selected == MENU_LOCAL) {
                scan_ggufs(menu);
                if (menu->gguf_count > 0) {
                    const char *leaf = strrchr(g->llama_model, '/');
                    leaf = leaf ? leaf + 1 : g->llama_model;
                    for (int i = 0; i < menu->gguf_count; i++)
                        if (g->llama_model[0] && strcmp(menu->gguf[i].name, leaf) == 0)
                            menu->model_selected = i;
                }
            }
        }
        break;
    default:
        break;
    }
}

static bool write_workspace_file(const game *g, const char *name, const char *temp,
                                 const char *body, size_t length)
{
    int dirfd = open(g->workspace, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) return false;
    unlinkat(dirfd, temp, 0);
    int fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool ok = false;
    if (fd >= 0) {
        ok = write(fd, body, length) == (ssize_t)length;
        if (close(fd) != 0) ok = false;
        if (ok) ok = renameat(dirfd, temp, dirfd, name) == 0;
        if (!ok) unlinkat(dirfd, temp, 0);
    }
    close(dirfd);
    return ok;
}

static bool remove_workspace_file(const game *g, const char *name)
{
    int dirfd = open(g->workspace, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) return false;
    bool ok = unlinkat(dirfd, name, 0) == 0 || errno == ENOENT;
    close(dirfd);
    return ok;
}

static bool save_goal_file(const game *g)
{
    const level *lv = cur_level(g);
    char body[LEVEL_TEXT + 32];
    int length = snprintf(body, sizeof body, "# Goal\n\n%s\n", lv->goal);
    if (length < 0 || (size_t)length >= sizeof body) return false;
    return write_workspace_file(g, "GOAL.md", ".goal-save", body, (size_t)length);
}

static bool save_hints_file(const game *g)
{
    const level *lv = cur_level(g);
    char body[LEVEL_HINT_MAX * (LEVEL_TEXT + 16) + 64];
    int length = snprintf(body, sizeof body, "# Hints\n");
    if (length < 0 || (size_t)length >= sizeof body) return false;
    if (lv->nhints == 0) {
        int extra = snprintf(body + length, sizeof body - (size_t)length, "\nThis room has no hints.\n");
        if (extra < 0 || (size_t)length + (size_t)extra >= sizeof body) return false;
        length += extra;
    }
    for (int i = 0; i < lv->nhints; i++) {
        int extra = snprintf(body + length, sizeof body - (size_t)length, "\n%d. %s\n", i + 1, lv->hints[i]);
        if (extra < 0 || (size_t)length + (size_t)extra >= sizeof body) return false;
        length += extra;
    }
    return write_workspace_file(g, "HINTS.md", ".hints-save", body, (size_t)length);
}

void game_enter_level(game *g, int level)
{
    if (g->state_lock >= 0) seed_system_files(g);
    count_puzzle_stop(&g->count_check);
    script_puzzle_stop(&g->script_check);
    scene_enter_room(&g->sc, level);
    restore_ranked_attempt(g);
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
        !puzzle_prepare(g->root, g->state, "count-setup"))
        activity_add(&g->act, ACT_ERROR, "Could not install the count_files.sh puzzle.");
    if ((cur_level(g)->check == CHECK_FOLLOW_LOG || cur_level(g)->check == CHECK_STAMPED_LOG) &&
        g->state_lock >= 0 && !puzzle_prepare(g->root, g->state, "log-setup"))
        activity_add(&g->act, ACT_ERROR, "Could not install run_check.sh.");
    activity_add(&g->act, ACT_GOAL, "Room %d: %s", level + 1, cur_level(g)->title);
    if (!g->spawn && cur_level(g)->check == CHECK_COUNT_FILES)
        g->goal = levels_check(cur_level(g), g->workspace);
    else
        game_poll_goal(g);
    if (g->sc.goal_met) g->sc.door = 1.0f;   /* already done earlier */
    if (g->state_lock >= 0) {
        if (!save_goal_file(g))
            activity_add(&g->act, ACT_ERROR, "Could not save ~/GOAL.md.");
        if (g->hints_file) {
            if (!save_hints_file(g))
                activity_add(&g->act, ACT_ERROR, "Could not save ~/HINTS.md.");
        } else if (!remove_workspace_file(g, "HINTS.md")) {
            activity_add(&g->act, ACT_ERROR, "Could not remove ~/HINTS.md.");
        }
    }
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
    pty_init(&g->menu.fetch);
    pty_init(&g->menu.yesno);
    pty_init(&g->menu.prompt_counter);
    pty_init(&g->metrics.worker);
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
    char harness[8];
    g->use_pi = g->act.pi = load_preference(state, "harness", harness) && strcmp(harness, "pi") == 0;
    char tips[8];
    g->tooltips = load_preference(state, "tooltips", tips) && strcmp(tips, "on") == 0;
    char buttons[8];
    g->room_buttons = load_preference(state, "room-buttons", buttons) && strcmp(buttons, "on") == 0;
    char hints_file[8];
    g->hints_file = !(load_preference(state, "hints-file", hints_file) && strcmp(hints_file, "off") == 0);
    char yolo[8];
    g->yolo = load_preference(state, "yolo", yolo) && strcmp(yolo, "on") == 0;
    char campaign[8];
    if (!campaign_active_model(state, g->campaign_model)) g->campaign_model[0] = 0;
    g->campaign = load_preference(state, "campaign", campaign) && strcmp(campaign, "on") == 0 &&
                  g->campaign_model[0];
    if (g->campaign) apply_locked_model(g, g->campaign_model);
    scene_init(&g->sc, 0, g->levels.count);

    room_transition move;
    if (g->state_lock >= 0 && (!read_transition(g, &move) ||
        (move.direction && !finish_transition(g, move)))) {
        snprintf(err, errcap, "could not recover the saved room transition");
        game_shutdown(g);
        return false;
    }
    game_enter_level(g, game_load_progress(state, g->levels.count));
    art_load_room(&g->room, root);
    g->started = !spawn;
    if (spawn) open_menu(g);
    return true;
}

void game_shutdown(game *g)
{
    stop_metrics(&g->metrics);
    count_puzzle_stop(&g->count_check);
    script_puzzle_stop(&g->script_check);
    pty_stop(&g->omp);
    poll_ranked_attempt(g);
    pty_stop(&g->shell);
    pty_stop(&g->menu.discovery);
    pty_stop(&g->menu.fetch);
    pty_stop(&g->menu.yesno);
    pty_stop(&g->menu.prompt_counter);
    clear_models(&g->menu);
    free(g->menu.scores);
    g->menu.scores = NULL;
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
    if (cur_level(g)->check == CHECK_COUNT_FILES)
        g->goal = count_puzzle_poll(&g->count_check, g->root, g->state, g->workspace);
    else if (cur_level(g)->check == CHECK_SCRIPT)
        g->goal = script_puzzle_poll(&g->script_check, g->root, g->state, g->workspace,
                                     cur_level(g)->file);
    else
        g->goal = game_check_level(g, g->sc.level);
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
        if (!g->paste_overflow && g->menu.prompt_edit) {
            char clean[CLIPBOARD_CAP];
            size_t n = 0;
            for (size_t i = 0; i < g->paste_len; i++) {
                unsigned char ch = (unsigned char)g->paste[i];
                if (ch == '\r' && i + 1 < g->paste_len && g->paste[i + 1] == '\n') continue;
                if (ch == '\r' || ch == '\n') clean[n++] = '\n';
                else if (ch == '\t') clean[n++] = ' ';
                else if (ch >= 32 && ch != 127) clean[n++] = (char)ch;
            }
            insert_prompt_text(&g->menu, clean, n);
        } else if (!g->paste_overflow) paste_into_terminal(g, g->paste, g->paste_len);
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

static void press_room_button(game *g, room_button button)
{
    if (g->transition_pending) {
        activity_add(&g->act, ACT_INFO, "Wait for the room change to finish.");
        return;
    }
    focus_screen(g, FOCUS_ROOM);
    switch (button) {
    case BUTTON_CLEAR_MODEL:
        stop_room_agent(g);
        if (g->spawn) spawn_agent(g);
        activity_add(&g->act, ACT_INFO, "Started a fresh coding session.");
        break;
    case BUTTON_CLEAR_SHELL:
        history_clear(&g->shell_history);
        if (g->shell.pid > 0 && !pty_poll_exit(&g->shell))
            pty_write(&g->shell, "\014", 1);
        else if (g->spawn) spawn_shell(g);
        activity_add(&g->act, ACT_INFO, "Cleared shell display.");
        break;
    case BUTTON_SHELL_LOG: {
        const char text[] = "The player shell transcript is at ~/shell.log.";
        bool ready = g->omp.pid > 0 && !pty_poll_exit(&g->omp);
        if (!ready && g->spawn) spawn_agent_prompt(g, text);
        focus_screen(g, FOCUS_OMP);
        if (ready) paste_into_terminal(g, text, sizeof text - 1);
        break;
    }
    case BUTTON_HARNESS:
    case BUTTON_MODEL:
    case BUTTON_RESET_LEVEL:
    case BUTTON_RESET_GAME:
    case BUTTON_METRICS:
        open_menu(g);
        g->menu.notice[0] = 0;
        if (button == BUTTON_HARNESS) {
            g->menu.page = MENU_PAGE_MAIN;
            g->menu.selected = MENU_HARNESS;
        }
        if (button == BUTTON_MODEL) {
            g->menu.page = MENU_PAGE_MODEL;
            g->menu.selected = g->use_codex ? MENU_CODEX : g->use_grok ? MENU_GROK :
                               g->use_claude ? MENU_CLAUDE : g->ollama_model[0] ? MENU_OLLAMA : MENU_LOCAL;
        }
        if (button == BUTTON_RESET_LEVEL || button == BUTTON_RESET_GAME) {
            g->menu.page = MENU_PAGE_RESET;
            g->menu.selected = button == BUTTON_RESET_LEVEL ? MENU_RESET_LEVEL : MENU_RESET;
            g->menu.confirm_reset = true;
        }
        if (button == BUTTON_METRICS) {
            g->menu.page = MENU_PAGE_SCORES;
            g->menu.selected = MENU_LEADERBOARD;
            open_leaderboard(g);
        }
        break;
    default: break;
    }
}

void game_key(game *g, const key_event *ev)
{
    bool press = ev->action != 3;
    const unsigned game_mod = MOD_CTRL | MOD_SHIFT;
    bool in_room = g->sc.focus == FOCUS_ROOM;

    if (ev->key >= KEY_LEFT && ev->key <= KEY_DOWN) {
        unsigned arrow = 1u << (ev->key - KEY_LEFT);
        if (ev->action == 1) {
            g->pane_arrows &= ~arrow;
        } else if (g->pane_arrows & arrow) {
            if (!press) g->pane_arrows &= ~arrow;
            return;
        }
        if (in_room && ev->mods == MOD_ALT) {
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

    if ((ev->key == KEY_ESCAPE && ev->mods == 0 && (in_room || g->menu.open)) ||
        (ev->key == KEY_F12 && ev->mods == game_mod)) {
        if (press && ev->action != 2) {
            if (g->menu.prompt_edit) g->menu.prompt_edit = false;
            else if (g->menu.leaderboard_open) {
                g->menu.leaderboard_open = false;
                g->menu.campaign_board = false;
            }
            else if (g->menu.details_open) {
                g->menu.details_open = false;
                pty_stop(&g->menu.prompt_counter);
            }
            else if (g->menu.confirm_reset) g->menu.confirm_reset = false;
            else if (g->menu.name_edit) {
                g->menu.name_edit = false;
                g->menu.pull_name[0] = 0;
                g->menu.notice[0] = 0;
            }
            else if (g->menu.model_submenu) g->menu.model_submenu = 0;
            else if (g->menu.open && g->menu.page == MENU_PAGE_DOWNLOAD) {
                g->menu.page = MENU_PAGE_MODEL;
                g->menu.selected = MENU_DOWNLOADS;
            }
            else if (g->menu.open && g->menu.page != MENU_PAGE_MAIN) {
                int page = g->menu.page;
                g->menu.page = MENU_PAGE_MAIN;
                g->menu.selected = page == MENU_PAGE_MODEL ? MENU_MODEL :
                                   page == MENU_PAGE_SCORES ? MENU_SCORES :
                                   page == MENU_PAGE_DISPLAY ? MENU_DISPLAY :
                                   page == MENU_PAGE_BENCH ? MENU_BENCH : MENU_RESETS;
            }
            else if (!g->menu.open) open_menu(g);
            else if (g->started) g->menu.open = false;
        }
        return;
    }
    if (g->menu.open) {
        if (press) menu_key(g, ev);
        return;
    }

    bool copy = (ev->key == 'c' || ev->key == 'C') && g->selection.active;
    bool paste = (ev->key == 'v' || ev->key == 'V') && g->clipboard[0] && !in_room;
    if (ev->mods == (MOD_CTRL | MOD_ALT) && (copy || paste)) {
        if (ev->action == 1) {
            if (copy) {
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

    if (ev->mods == game_mod) {
        focus_id target;
        switch (ev->key) {
        case KEY_F1: target = FOCUS_OMP; break;
        case KEY_F1 + 1: target = FOCUS_SHELL; break;
        case KEY_F1 + 9: target = FOCUS_ROOM; break;
        default: target = (focus_id)-1; break;
        }
        if ((int)target >= 0) {
            if (ev->action == 1) focus_screen(g, target);
            return;
        }
    }

    if (!in_room) {
        pty_proc *p = g->sc.focus == FOCUS_OMP ? &g->omp : &g->shell;
        neuroterm *t = g->sc.focus == FOCUS_OMP ? &g->omp_vt : &g->shell_vt;
        char buf[256];
        size_t n;
        if (!press) return;
        history *scroll = g->sc.focus == FOCUS_OMP ? &g->omp_history : &g->shell_history;
        if (ev->mods == game_mod && !scroll->alternate &&
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
        d = dir_index(k);
        if (d >= 0 && (!press || !(ev->mods & ~MOD_SHIFT))) {
            if (ev->action == 3) g->held_until[d] = 0;
            else g->held_until[d] = g->now + (g->release_events ? 1e9 : 0.2);
            return;
        }
        if (!press) return;
        if (ev->mods == MOD_CTRL && k == 'q') { g->quit = true; return; }
        if (ev->mods & ~MOD_SHIFT) return;
        if (k == 'h') { next_hint(g); return; }
        if (k == KEY_ENTER || k == ' ' || k == 'e') {
            room_button button = scene_room_button_at(g->sc.x, g->sc.y);
            if (g->room_buttons && button != BUTTON_NONE) {
                if (ev->action == 1) press_room_button(g, button);
                return;
            }
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
    room_button button = scene_room_button_at((float)x, (float)y);
    if (g->room_buttons && button != BUTTON_NONE) {
        press_room_button(g, button);
        return;
    }
    if (inside(layout_screen(SCREEN_OMP), x, y)) focus_screen(g, FOCUS_OMP);
    else if (inside(layout_screen(SCREEN_SHELL), x, y)) focus_screen(g, FOCUS_SHELL);
    else if (inside(layout_screen(SCREEN_GOAL), x, y)) next_hint(g);
    else if (inside(layout_screen(SCREEN_ACT), x, y) || inside(layout_resources(), x, y)) return;
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
    if (!rows) return;
    if (g->menu.details_open) {
        model_details_scroll(&g->menu, rows);
        return;
    }
    if (g->menu.open) return;
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

static void arrive_in_room(game *g, room_transition move)
{
    game_enter_level(g, move.from + move.direction);
    if (move.direction < 0) {
        g->sc.y = AISLE_Y0 + SCENE_WALK_SPEED;
        g->sc.dir = 0;
    }
    g->transition_pending = false;
    memset(g->held_until, 0, sizeof g->held_until);
}

void game_update(game *g, double dt)
{
    poll_prompt_count(&g->menu);
    poll_models(&g->menu);
    poll_fetch(&g->menu);
    poll_yesno(&g->menu);
    if (g->spawn) {
        pump(&g->omp, &g->omp_vt, g->omp_note, sizeof g->omp_note, "the agent");
        pump(&g->shell, &g->shell_vt, g->shell_note, sizeof g->shell_note, "the shell");
        pump_metrics(g);
    }
    if (g->started && g->ranked_attempt.id && g->ranked_session_model[0] && !g->transition_pending) {
        g->ranked_fraction_ms += dt * 1000.0;
        long long ms = (long long)g->ranked_fraction_ms;
        g->ranked_attempt.measure[SCORE_TIME] += ms;
        g->ranked_fraction_ms -= (double)ms;
    }
    if (g->menu.open) return;
    if (g->transition_pending) {
        g->poll_timer -= dt;
        if (g->poll_timer > 0) return;
        g->poll_timer = 0.5;
        room_transition move;
        if (read_transition(g, &move) && move.direction && finish_transition(g, move))
            arrive_in_room(g, move);
        return;
    }
    g->now += dt;
    g->level_time += dt;
    g->poll_timer -= dt;
    if (g->poll_timer <= 0) {
        g->poll_timer = 0.5;
        activity_poll(&g->act, g->state);
        checkpoint_ranked_attempt(g);
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
        if (ev == SCENE_BOTTOM_EXIT && g->sc.level == 0) {
            g->quit = true;
            break;
        }
        if (ev == SCENE_NEXT_ROOM || ev == SCENE_BOTTOM_EXIT) {
            room_transition move = {.from = g->sc.level,
                                    .direction = ev == SCENE_NEXT_ROOM ? 1 : -1};
            if (!save_transition(g, move)) {
                activity_add(&g->act, ACT_ERROR, "Could not save room transition.");
                if (move.direction < 0) g->sc.y = CANVAS_H - 2;
            } else {
                stop_room_agent(g);
                score_entry *score = &g->ranked_attempt;
                if (move.direction > 0 && g->sc.goal_met && score->id &&
                    score->measure[SCORE_TOKENS] > 0 && strcmp(score->model, "*") &&
                    !leaderboard_submit(g->state, score))
                    activity_add(&g->act, ACT_ERROR, "Could not save the leaderboard result.");
                if (move.direction > 0 && g->sc.goal_met && g->campaign) {
                    int needed[LEVEL_MAX];
                    int count = playable_level_numbers(g, needed);
                    bool ranked = false;
                    if (!campaign_note(g->state, score, needed, count, &ranked))
                        activity_add(&g->act, ACT_ERROR, "Could not save the campaign.");
                    else if (ranked)
                        activity_add(&g->act, ACT_INFO, "Campaign complete. It is on the campaign board.");
                }
                if (finish_transition(g, move)) {
                    arrive_in_room(g, move);
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
    v.room = &g->room;
    if (g->sc.level + 1 < g->levels.count)
        v.next_room = g->levels.items[g->sc.level + 1].number;
    v.omp = &g->omp_vt;
    v.hosted_agent = hosted_agent(g);
    const char *leaf = strrchr(g->llama_model, '/');
    leaf = g->llama_model[0] ? (leaf && leaf[1] ? leaf + 1 : g->llama_model) : "";
    v.agent_model = v.hosted_agent ? v.hosted_agent :
                    g->ollama_model[0] ? g->ollama_model : leaf;
    v.pi = g->use_pi;
    v.shell = &g->shell_vt;
    v.omp_history = &g->omp_history;
    v.shell_history = &g->shell_history;
    v.act = &g->act;
    v.resource_summary = g->metrics.summary[0] ? g->metrics.summary :
        g->omp.pid > 0 && !g->omp.exited ? "Sampling resources..." : "Resources   Agent idle";
    v.resource_devices = g->metrics.devices;
    v.goal_scroll = g->goal_scroll;
    v.action_scroll = g->action_scroll;
    v.selection = g->selection;
    v.omp_note = g->omp_note[0] ? g->omp_note : NULL;
    v.shell_note = g->shell_note[0] ? g->shell_note : NULL;
    v.goal_why = g->goal.why;
    v.hints_shown = g->hints_shown;
    v.tooltips = g->tooltips;
    v.room_buttons = g->room_buttons;
    v.blink = ((long)(g->now * 2.0)) % 2 == 0;
    return v;
}

static const char *configured_gguf_label(void)
{
    const char *weights = getenv("PUZZLLM_MODEL");
    if (!weights || !*weights) return "Qwen 3.5 (default GGUF)";
    const char *leaf = strrchr(weights, '/');
    return leaf && leaf[1] ? leaf + 1 : weights;
}

void game_render(game *g, sr_canvas *c)
{
    view v = game_view(g);
    render_frame(c, &v);
    if (g->menu.prompt_edit)
        render_prompt_editor(c, g->menu.prompt_draft, g->menu.prompt_cursor, g->menu.notice);
    else if (g->menu.details_open)
        render_model_details(c, g->menu.details_tab, g->menu.details_scroll,
                             g->menu.prompt, g->menu.write_schema,
                             g->menu.prompt_count);
    else if (g->menu.leaderboard_open)
        render_leaderboard(c, g->menu.scores, g->menu.score_count, g->menu.score_scroll,
                           g->menu.score_level, g->menu.score_metric, g->menu.model_scores,
                           g->menu.notice, &g->ranked_attempt, g->menu.campaign_board);
    else if (g->menu.model_submenu) {
        bool ollama_list = g->menu.model_submenu == MENU_OLLAMA;
        render_model_menu(c, ollama_list, ollama_list ? g->menu.models : g->menu.gguf,
                          ollama_list ? g->menu.count : g->menu.gguf_count,
                          g->menu.model_selected, g->menu.loading, g->menu.load_failed,
                          configured_gguf_label());
    } else if (g->menu.open)
        render_menu(c, g->menu.selected, v.agent_model,
                    g->started, g->sc.level + 1, g->menu.confirm_reset, g->menu.notice,
                    g->thinking_support, g->thinking_enabled, v.hosted_agent, g->use_pi,
                    g->compute, g->tooltips, g->room_buttons, g->campaign, g->campaign_model,
                    g->hints_file, g->yolo, g->menu.page, g->menu.name_edit);
}
