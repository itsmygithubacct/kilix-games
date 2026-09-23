#include "kilix.h"
#include "kilix_game_audio.h"     /* kilix_game_data_root_from_executable */
#include "kilix_game_runtime.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char asset_root[768] = "assets";

float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

int clampi(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

unsigned rng_next(void)
{
    unsigned value = G.rng;
    if (!value) value = 0xA341316Cu;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    G.rng = value;
    return value;
}

int rng_range(int low, int high)
{
    if (high <= low) return low;
    return low + (int)(rng_next() % (unsigned)(high - low + 1));
}

void asset_paths_init(void)
{
    (void)kilix_game_data_root_from_executable(
        "KILIX_RANCHER_ASSETS", "assets", "../share/kilix-rancher/assets",
        asset_root, sizeof asset_root);
}

const char *asset_path(const char *relative)
{
    static char buffers[6][1024];
    static int index;
    index = (index + 1) % 6;
    snprintf(buffers[index], sizeof buffers[index], "%s/%s",
             asset_root, relative);
    return buffers[index];
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool ensure_path(const char *path)
{
    char copy[768];
    if (strlen(path) >= sizeof copy) return false;
    snprintf(copy, sizeof copy, "%s", path);
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (char *cursor = copy + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (!ensure_directory(copy)) return false;
        *cursor = '/';
    }
    return ensure_directory(copy);
}

static bool dump_named(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", directory, name);
    render_frame();
    if (!render_dump_ppm(path)) {
        fprintf(stderr, "render-test: could not write %s\n", path);
        return false;
    }
    return true;
}

static int render_test(const char *directory, unsigned seed)
{
    if (!ensure_path(directory)) {
        fprintf(stderr, "render-test: could not create %s: %s\n",
                directory, strerror(errno));
        return 1;
    }
    game_init(seed, true);
    G.headless = true;
    char temporary_save[768];
    snprintf(temporary_save, sizeof temporary_save,
             "%s/.render-test-save.dat", directory);
    if (strlen(temporary_save) >= sizeof G.save_path) {
        fprintf(stderr, "render-test: output path is too long\n");
        return 1;
    }
    memcpy(G.save_path, temporary_save, strlen(temporary_save) + 1);

    char error[256];
    if (!render_init(960, 540, error, sizeof error)) {
        fprintf(stderr, "render-test: %s\n", error);
        return 1;
    }
    G.screen_time = 1.0f;
    int failures = 0;
    failures += !dump_named(directory, "render_01_title.ppm");
    G.screen = SCREEN_NAMING;
    G.screen_time = 1.0f;
    snprintf(G.name_input, sizeof G.name_input, "EMBER");
    G.name_len = 5;
    failures += !dump_named(directory, "render_02_naming.ppm");

    game_new_ranch("Ember");
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_03_ranch.ppm");
    G.toast[0] = '\0';
    G.toast_timer = 0.0f;
    G.screen = SCREEN_TRAINING;
    G.drill_cursor = 2;
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_04_training.ppm");
    G.screen = SCREEN_CARE;
    G.care_cursor = 1;
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_05_care.ppm");
    G.screen = SCREEN_ARENA;
    G.arena_cursor = G.kilix.rank;
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_06_arena.ppm");

    game_start_battle(G.kilix.rank);
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_07_battle_ready.ppm");
    G.battle.phase = BATTLE_ACTIVE;
    G.battle.player_guts = 82;
    G.battle.enemy_guts = 64;
    G.battle.distance = 0.38f;
    failures += !dump_named(directory, "render_08_battle_active.ppm");
    G.battle.phase = BATTLE_PLAYER_ATTACK;
    G.battle.phase_timer = 0.23f;
    G.battle.hit = true;
    G.battle.last_damage = 37;
    snprintf(G.battle.callout, sizeof G.battle.callout,
             "EMBER POUNCE!  37 HEART");
    failures += !dump_named(directory, "render_09_battle_impact.ppm");

    G.screen = SCREEN_EVENT;
    G.screen_time = 1.0f;
    G.event.kind = EVENT_TRAIN;
    G.event.index = 2;
    G.event.timer = 1.0f;
    G.event.duration = 1.5f;
    G.event.success = true;
    G.event.great = true;
    G.event.primary = DRILLS[2].primary;
    G.event.secondary = DRILLS[2].secondary;
    G.event.gain_primary = 9;
    G.event.gain_secondary = 4;
    snprintf(G.event.title, sizeof G.event.title, "BLAZING SUCCESS!");
    snprintf(G.event.detail, sizeof G.event.detail,
             "%s CHASED EVERY FIREFLY AND CAME BACK GLOWING.", G.kilix.name);
    failures += !dump_named(directory, "render_10_training_result.ppm");

    G.screen = SCREEN_JOURNAL;
    G.journal_page = 1;
    G.screen_time = 1.0f;
    failures += !dump_named(directory, "render_11_journal.ppm");
    G.screen = SCREEN_CHAMPION;
    G.screen_time = 1.0f;
    G.kilix.rank = RANK_COUNT - 1;
    G.kilix.total_wins = 12;
    failures += !dump_named(directory, "render_12_champion.ppm");

    /* One snapshot of each drill mini-game, mid-play. */
    G.screen = SCREEN_DRILL;
    G.screen_time = 1.0f;
    /* `drill` is the DRILLS[] index whose primary stat yields this minigame, so
     * each snapshot's signboard title matches the game shown. */
    struct { MinigameType type; int rounds; int drill; const char *file; } drills[] = {
        {MG_TIMING,   3, 1, "render_13_drill_timing.ppm"},
        {MG_REACTION, 3, 4, "render_14_drill_reaction.ppm"},
        {MG_MEMORY,   3, 5, "render_15_drill_memory.ppm"},
        {MG_MASH,     1, 2, "render_16_drill_mash.ppm"},
        {MG_HOLD,     1, 3, "render_17_drill_hold.ppm"},
        {MG_RHYTHM,   RHY_BEATS, 0, "render_18_drill_rhythm.ppm"},
    };
    for (size_t i = 0; i < sizeof drills / sizeof drills[0]; i++) {
        memset(&G.minigame, 0, sizeof G.minigame);
        G.minigame.phase = 1;
        G.minigame.type = drills[i].type;
        G.minigame.drill = drills[i].drill;
        G.minigame.rounds = drills[i].rounds;
        G.minigame.round = 1;
        G.minigame.clock = 0.3f;              /* within a memory reveal's lit window */
        G.minigame.marker = 0.56f;
        G.minigame.target = 0.5f;
        G.minigame.half = 0.14f;
        G.minigame.cue_live = true;
        G.minigame.showing = drills[i].type == MG_MEMORY;
        G.minigame.seq_len = 4;
        G.minigame.seq[0] = 0; G.minigame.seq[1] = 2;
        G.minigame.seq[2] = 1; G.minigame.seq[3] = 3;
        G.minigame.seq_pos = 2;
        G.minigame.taps = 13;
        G.minigame.taps_target = 26;
        G.minigame.feedback = 0.4f;
        snprintf(G.minigame.banner, sizeof G.minigame.banner, "GOOD");
        failures += !dump_named(directory, drills[i].file);
    }

    /* One snapshot of each drill's get-ready / instruction screen so the
     * signboard text-fit stays covered by the visual regression set. */
    struct { MinigameType type; int drill; const char *file; } info[] = {
        {MG_TIMING,   1, "render_19_info_timing.ppm"},
        {MG_REACTION, 4, "render_20_info_reaction.ppm"},
        {MG_MEMORY,   5, "render_21_info_memory.ppm"},
        {MG_MASH,     2, "render_22_info_mash.ppm"},
        {MG_HOLD,     3, "render_23_info_hold.ppm"},
        {MG_RHYTHM,   0, "render_24_info_rhythm.ppm"},
    };
    for (size_t i = 0; i < sizeof info / sizeof info[0]; i++) {
        memset(&G.minigame, 0, sizeof G.minigame);
        G.minigame.phase = 0;                 /* get-ready / instructions */
        G.minigame.type = info[i].type;
        G.minigame.drill = info[i].drill;
        G.minigame.clock = 0.3f;              /* countdown shows "3" */
        failures += !dump_named(directory, info[i].file);
    }

    /* Economy screens: Academy (menu + montage), Dojo, and the rent events. */
    G.money = 1500;
    G.screen = SCREEN_ACADEMY;
    G.academy_phase = 0;
    G.academy_cursor = 1;
    failures += !dump_named(directory, "render_25_academy.ppm");
    G.academy_phase = 1;
    G.academy_choice = 1;
    G.academy_clock = 0.9f;
    failures += !dump_named(directory, "render_26_academy_train.ppm");

    G.screen = SCREEN_DOJO;
    G.dojo_cursor = 2;
    G.moves_known = 0x3u;                     /* first extra move already learned */
    failures += !dump_named(directory, "render_27_dojo.ppm");

    G.screen = SCREEN_EVENT;
    G.event.kind = EVENT_RENT;
    G.event.success = true;
    G.event.money_delta = -RENT_AMOUNT;
    G.event.duration = 2.2f;
    G.event.timer = 2.2f;
    snprintf(G.event.title, sizeof G.event.title, "%s", "Rent Collected");
    snprintf(G.event.detail, sizeof G.event.detail, "%s",
             "The ranch collector calls. Rent of 1000 g is paid up front for "
             "the coming month.");
    failures += !dump_named(directory, "render_28_rent.ppm");

    G.event.kind = EVENT_EVICTION;
    G.event.success = false;
    G.event.money_delta = 0;
    snprintf(G.event.title, sizeof G.event.title, "%s", "Eviction Notice");
    snprintf(G.event.detail, sizeof G.event.detail, "%s",
             "Rent came due and the coffers ran dry. The run ends here.");
    failures += !dump_named(directory, "render_29_eviction.ppm");

    G.screen = SCREEN_BANK;
    G.money = 650;
    G.bank = 800;
    G.rent_paid_weeks = 6;
    G.total_weeks = 4;                        /* rent due in 2 weeks */
    G.bank_cursor = 1;
    failures += !dump_named(directory, "render_30_bank.ppm");

    render_shutdown();
    unlink(temporary_save);
    if (failures) {
        fprintf(stderr, "render-test: %d snapshot(s) failed\n", failures);
        return 1;
    }
    printf("PASS: wrote 30 visual snapshots to %s\n", directory);
    return 0;
}

static void print_help(void)
{
    printf("kilix-rancher " KILIX_RANCHER_VERSION
           " - original graphical fire-kitten raising game\n\n"
           "Usage:\n"
           "  ./kilix-rancher                    Play in a Kitty-graphics terminal\n"
           "  ./kilix-rancher --selftest [seed] [weeks]\n"
           "  ./kilix-rancher --render-test [directory] [seed]\n"
           "  ./kilix-rancher --validate-assets\n"
           "  ./kilix-rancher --version\n"
           "  ./kilix-rancher --help\n\n"
           "Controls:\n"
           "  Arrows / W,S     Move the cursor; up/down pick a battle move\n"
           "  Left / Right     Change range during a live battle\n"
           "  Enter / Space    Confirm; call selected move\n"
           "  1-4              Call a battle move directly\n"
           "  Esc              Back; matches and drills cancel freely before\n"
           "                   they begin, but are forfeited once underway\n"
           "  J                Journal (ranch/champion screen)\n"
           "  M                Toggle sound\n"
           "  Q                Quit from title/ranch/champion\n\n"
           "Environment:\n"
           "  KILIX_RANCHER_ASSETS=/path   Override asset directory\n"
           "  KILIX_RANCHER_SAVE=/path     Override save file\n"
           "  KILIX_RANCHER_SKIP_PROBE=1   Skip Kitty protocol probe\n");
}

/* ---- interactive session over the shared kilix-game-kit host ----------- */

typedef struct {
    unsigned seed;
    bool started;              /* app_start ran: the terminal came up */
    bool key_repeat;           /* latest key event was keyboard auto-repeat */
    int64_t next_present_ns;   /* 30 fps presentation pacing */
    char error[256];           /* start-up failure detail, printed after run */
} AppState;

/* game.c asks this while counting mash-drill taps; only genuine presses may
 * count there while auto-repeat stays welcome everywhere else. */
static bool app_key_repeat;

bool input_key_repeated(void)
{
    return app_key_repeat;
}

static int game_key_from_event(const kittykb_event *event)
{
    switch (event->key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    default: return event->key <= 0x7fU ? (int)event->key : -1;
    }
}

static bool app_start(kilix_game_host *host, void *user)
{
    AppState *app = user;
    kittyts_session *terminal = kilix_game_host_terminal(host);

    app->started = true;
    game_init(app->seed, false);
    if (!render_init(kittyts_width(terminal), kittyts_height(terminal),
                     app->error, sizeof app->error))
        return false;
    (void)sound_init();
    return true;
}

static void app_event(kilix_game_host *host, void *user,
                      const kittyin_event *event)
{
    (void)user;
    if (event->kind != KITTYIN_EVENT_KEY) return;
    const kittykb_event *key_event = &event->data.key;
    if (key_event->action == KITTYKB_ACTION_RELEASE) return;
    if ((key_event->modifiers & KITTYKB_MOD_CTRL) &&
        (key_event->key == 'c' || key_event->key == 'C')) {
        G.quit = true;
        kilix_game_host_request_stop(host);
        return;
    }
    int key = game_key_from_event(key_event);
    if (key < 0) return;
    app_key_repeat = key_event->action == KITTYKB_ACTION_REPEAT;
    game_handle_key(key);
}

static bool app_step(kilix_game_host *host, void *user, double step_seconds)
{
    (void)user;
    game_tick((float)step_seconds);
    if (G.quit) kilix_game_host_request_stop(host);
    return true;
}

static bool app_render(kilix_game_host *host, void *user, double alpha)
{
    AppState *app = user;
    kittyts_session *terminal = kilix_game_host_terminal(host);
    int width, height;

    (void)alpha;
    if (kittyts_check_resize(terminal, &width, &height))
        render_resize(width, height);
    /* Input and simulation run at the host's full rate; the expensive parts —
     * the full-screen illustrated repaint and the Kitty pixel upload — are
     * paced at 30 fps. */
    int64_t now = kilix_game_monotonic_ns();
    if (now < app->next_present_ns) return true;
    app->next_present_ns = now + KILIX_GAME_NANOSECONDS_PER_SECOND / 30;
    render_frame();
    (void)kittyts_present(terminal, render_fb(), G.W, G.H);
    return true;
}

static void app_stop(kilix_game_host *host, void *user)
{
    (void)host;
    (void)user;
    sound_shutdown();
    render_shutdown();
}

int main(int argc, char **argv)
{
    asset_paths_init();
    unsigned seed = (unsigned)time(NULL);

    if (argc > 1 && !strcmp(argv[1], "--help")) {
        print_help();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("kilix-rancher %s\n", KILIX_RANCHER_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--validate-assets")) {
        char error[256];
        if (!render_validate_assets(error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            return 1;
        }
        printf("PASS: all runtime image assets are present and valid\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        if (argc > 2) seed = (unsigned)strtoul(argv[2], NULL, 10);
        int weeks = argc > 3 ? atoi(argv[3]) : 240;
        return game_selftest(seed, weeks);
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        const char *directory = argc > 2 ? argv[2] : ".render-test";
        if (argc > 3) seed = (unsigned)strtoul(argv[3], NULL, 10);
        return render_test(directory, seed);
    }
    if (argc > 1) {
        fprintf(stderr, "kilix-rancher: unknown option %s\n", argv[1]);
        print_help();
        return 2;
    }

    /* The shared host owns raw mode, the graphics probe, fixed-step timing
     * (60 Hz simulation, 8-step cap with debt dropped, so slow frames never
     * replay as a fast-forward burst), orderly SIGINT/SIGTERM/SIGHUP stops,
     * Ctrl-Z terminal-safe suspension, and crash-signal terminal restore. */
    AppState app = {0};
    kilix_game_host host;
    kilix_game_host_options options;
    kilix_game_host_callbacks callbacks = {
        app_start, app_event, app_step, app_render, app_stop
    };

    app.seed = seed;
    kilix_game_host_options_init(&options);
    options.terminal.framebuffer.min_width = 640;
    options.terminal.framebuffer.min_height = 360;
    options.terminal.framebuffer.max_width = 1440;
    options.terminal.framebuffer.max_height = 900;
    options.terminal.framebuffer.probe_timeout_ms = 2000;
    if (getenv("KILIX_RANCHER_SKIP_PROBE"))
        options.terminal.framebuffer.probe_graphics = false;

    int result = kilix_game_host_run(&host, &options, &callbacks, &app);
    if (result != EXIT_SUCCESS) {
        if (app.error[0])
            fprintf(stderr, "kilix-rancher: %s\n", app.error);
        else if (!app.started)
            fprintf(stderr, "kilix-rancher: terminal setup failed: %s\n",
                    strerror(host.terminal_errno ? host.terminal_errno
                                                 : ENOTSUP));
        else
            fprintf(stderr, "kilix-rancher: the session ended unexpectedly\n");
    }
    return result;
}
