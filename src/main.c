/* Entry point, headless checks, asset discovery, and fixed-step loop. */
#include "kilix_pong.h"
#include "kilix_state.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.2.0"

static char asset_root[512] = "assets";

void asset_paths_init(void)
{
    const char *override = getenv("KILIX_PONG_ASSETS");
    if (override && *override) {
        (void)snprintf(asset_root, sizeof asset_root, "%s", override);
        return;
    }

    char executable[400];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash) return;
    *slash = '\0';

    char candidate[512];
    (void)snprintf(candidate, sizeof candidate, "%s/assets", executable);
    if (access(candidate, R_OK) == 0) {
        (void)snprintf(asset_root, sizeof asset_root, "%s", candidate);
        return;
    }
    (void)snprintf(candidate, sizeof candidate,
                   "%s/../share/kilix-pong/assets", executable);
    if (access(candidate, R_OK) == 0)
        (void)snprintf(asset_root, sizeof asset_root, "%s", candidate);
}

const char *asset_path(const char *relative_path)
{
    static char paths[8][768];
    static unsigned index;
    index = (index + 1U) % 8U;
    (void)snprintf(paths[index], sizeof paths[index], "%s/%s", asset_root,
                   relative_path ? relative_path : "");
    return paths[index];
}

static double monotonic_milliseconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static void sleep_milliseconds(double milliseconds)
{
    if (milliseconds <= 0.0) return;
    struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000.0),
        .tv_nsec = (long)(fmod(milliseconds, 1000.0) * 1000000.0)
    };
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {}
}

static void emergency_signal(int signal_number)
{
    term_emergency_restore();
    _exit(128 + signal_number);
}

static void install_signal_handlers(void)
{
    (void)signal(SIGINT, emergency_signal);
    (void)signal(SIGTERM, emergency_signal);
    (void)signal(SIGHUP, emergency_signal);
    (void)signal(SIGSEGV, emergency_signal);
    (void)signal(SIGBUS, emergency_signal);
    (void)signal(SIGFPE, emergency_signal);
    (void)signal(SIGABRT, emergency_signal);
}

static bool ctrl_c_event(const KeyEvent *event)
{
    return event && event->action != KEY_ACTION_RELEASE &&
           (event->mods & KEY_MOD_CTRL) &&
           (event->key == 'c' || event->key == 'C');
}

/* ---------- remembered setup ----------
 *
 * The title-menu choice (both controllers, level, sound) is kept in a
 * kilix-state record under $XDG_DATA_HOME/kilix-pong/, written only by the
 * interactive game -- never by tests. Payload v1: version, left, right,
 * level, sound, one byte each; anything else is ignored.
 */
#define SETTINGS_VERSION 1u

static bool settings_open(kilixstate_store *store)
{
    kilixstate_options options;
    kilixstate_options_init(&options);
    options.app_id = "kilix-pong";
    options.filename = "settings.state";
    options.max_payload = 64u;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static void settings_load(void)
{
    kilixstate_store store;
    if (!settings_open(&store)) return;
    uint8_t payload[5];
    size_t size = 0;
    if (kilixstate_load(&store, payload, sizeof payload, &size) == KILIXSTATE_OK &&
        size == sizeof payload && payload[0] == SETTINGS_VERSION &&
        payload[1] < CTRL_COUNT && payload[2] < CTRL_COUNT &&
        payload[3] < LEVEL_COUNT && payload[4] <= 1u) {
        game_configure(payload[1], payload[2], payload[3]);
        G.sound_on = payload[4] != 0u;
    }
    kilixstate_store_close(&store);
}

static void settings_save(void)
{
    kilixstate_store store;
    if (!settings_open(&store)) return;
    uint8_t payload[5] = {
        SETTINGS_VERSION, (uint8_t)G.setup[SIDE_LEFT], (uint8_t)G.setup[SIDE_RIGHT],
        (uint8_t)G.level, G.sound_on ? 1u : 0u
    };
    kilixstate_result result = kilixstate_save(&store, payload, sizeof payload);
    if (result != KILIXSTATE_OK)
        fprintf(stderr, "kilix-pong: settings not saved: %s\n",
                kilixstate_result_name(result));
    kilixstate_store_close(&store);
}

static int parse_controller(const char *text)
{
    if (!text) return -1;
    if (!strcmp(text, "human")) return CTRL_HUMAN;
    if (!strcmp(text, "cpu")) return CTRL_CPU;
    if (!strcmp(text, "neural")) return CTRL_NEURAL;
    return -1;
}

static int parse_level(const char *text)
{
    if (!text) return -1;
    if (!strcmp(text, "easy")) return LEVEL_EASY;
    if (!strcmp(text, "normal")) return LEVEL_NORMAL;
    if (!strcmp(text, "hard")) return LEVEL_HARD;
    return -1;
}

/* Command-line overrides for an interactive session; -1 keeps the saved or
   default value. */
static int cli_setup[3] = { -1, -1, -1 };

static int run_interactive(void)
{
    int width;
    int height;
    if (!term_init(&width, &height)) {
        fprintf(stderr,
                "kilix-pong: needs an interactive Kitty-graphics terminal\n"
                "with at least a 320x180-pixel cell grid; use --help for tests\n");
        return 1;
    }
    install_signal_handlers();
    (void)atexit(term_shutdown);

    game_init(width, height, (uint32_t)time(NULL));
    settings_load();
    game_configure(cli_setup[0] >= 0 ? cli_setup[0] : G.setup[SIDE_LEFT],
                   cli_setup[1] >= 0 ? cli_setup[1] : G.setup[SIDE_RIGHT],
                   cli_setup[2] >= 0 ? cli_setup[2] : G.level);
    render_init(width, height);
    if (!render_fb()) {
        fprintf(stderr, "kilix-pong: unable to allocate the framebuffer\n");
        term_shutdown();
        return 1;
    }
    bool audio_ready = sound_init();
    sound_set_enabled(G.sound_on);
    if (!sound_bank_loaded())
        fprintf(stderr, "kilix-pong: production sound bank unavailable; continuing silently\n");
    else if (!audio_ready)
        fprintf(stderr, "kilix-pong: no supported audio sink; continuing silently\n");

    const double frame_period = 1000.0 / 30.0;
    double next_frame = monotonic_milliseconds();
    while (!G.quit) {
        KeyEvent event;
        while (term_poll_event(&event)) {
            if (ctrl_c_event(&event)) G.quit = true;
            else game_handle_event(&event);
        }

        int resized_width;
        int resized_height;
        if (term_check_resize(&resized_width, &resized_height) &&
            (resized_width != G.W || resized_height != G.H)) {
            G.W = resized_width;
            G.H = resized_height;
            render_resize(resized_width, resized_height);
            if (!render_fb()) G.quit = true;
        }

        game_tick();
        game_tick();
        render_frame();
        term_present(render_fb(), G.W, G.H);

        next_frame += frame_period;
        double remaining = next_frame - monotonic_milliseconds();
        if (remaining < -100.0) next_frame = monotonic_milliseconds();
        else sleep_milliseconds(remaining);
    }

    settings_save();
    sound_shutdown();
    render_shutdown();
    term_shutdown();
    return 0;
}

static int selftest(unsigned seed, int ticks)
{
    if (ticks <= 0) ticks = 12000;
    game_init(960, 540, seed);
    G.headless = true;
    game_start();

    int highest_total_score = 0;
    int highest_rally = 0;
    for (int tick = 0; tick < ticks; tick++) {
        game_autopilot();
        game_tick();
        char error[192];
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL seed=%u tick=%d: %s\n", seed, tick, error);
            return 1;
        }
        int total_score = G.paddles[SIDE_LEFT].score +
                          G.paddles[SIDE_RIGHT].score;
        if (total_score > highest_total_score) highest_total_score = total_score;
        if (G.rally > highest_rally) highest_rally = G.rally;
    }

    if (highest_total_score <= 0 || highest_rally <= 0) {
        fprintf(stderr,
                "FAIL seed=%u: simulation did not exercise rally and scoring "
                "(score=%d rally=%d)\n",
                seed, highest_total_score, highest_rally);
        return 1;
    }
    uint64_t digest = game_state_digest();
    printf("PASS seed=%u ticks=%d score=%d:%d max-score=%d max-rally=%d "
           "digest=%016llx\n",
           seed, ticks, G.paddles[SIDE_LEFT].score,
           G.paddles[SIDE_RIGHT].score, highest_total_score, highest_rally,
           (unsigned long long)digest);
    return 0;
}

/* Headless tournament: `games` matches to 11 from seeds seed..seed+games-1.
 * Human sides are played by the autopilot. A match that has not finished
 * after max_ticks counts as unfinished. */
typedef struct {
    int wins[SIDE_COUNT], unfinished;
    long points[SIDE_COUNT];
    long long ticks;
} MatchTally;

static bool play_matches(int left, int right, int level, unsigned seed, int games,
                         long max_ticks, MatchTally *tally)
{
    memset(tally, 0, sizeof *tally);
    for (int game = 0; game < games; game++) {
        game_init(960, 540, seed + (unsigned)game);
        G.headless = true;
        game_configure(left, right, level);
        game_start();
        long tick = 0;
        while (G.state != GS_GAMEOVER && tick < max_ticks) {
            game_autopilot();
            game_tick();
            tick++;
            char error[192];
            if (!game_validate(error, sizeof error)) {
                fprintf(stderr, "FAIL seed=%u tick=%ld: %s\n",
                        seed + (unsigned)game, tick, error);
                return false;
            }
        }
        tally->ticks += tick;
        for (int side = 0; side < SIDE_COUNT; side++)
            tally->points[side] += G.paddles[side].score;
        if (G.state == GS_GAMEOVER) tally->wins[G.winner]++;
        else tally->unfinished++;
    }
    return true;
}

static int match_command(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: kilix-pong --match LEFT RIGHT LEVEL SEED GAMES\n"
                        "  LEFT/RIGHT: human|cpu|neural (human = autopilot)\n"
                        "  LEVEL: easy|normal|hard\n");
        return 2;
    }
    int left = parse_controller(argv[2]), right = parse_controller(argv[3]);
    int level = parse_level(argv[4]);
    int games = atoi(argv[6]);
    if (left < 0 || right < 0 || level < 0 || games <= 0) {
        fprintf(stderr, "kilix-pong: bad --match arguments\n");
        return 2;
    }
    unsigned seed = (unsigned)strtoul(argv[5], NULL, 10);
    MatchTally tally;
    if (!play_matches(left, right, level, seed, games, 60L * 60 * 20, &tally)) return 1;
    printf("{\"left\":\"%s\",\"right\":\"%s\",\"level\":\"%s\",\"seed\":%u,"
           "\"games\":%d,\"left_wins\":%d,\"right_wins\":%d,\"unfinished\":%d,"
           "\"left_points\":%ld,\"right_points\":%ld,\"ticks\":%lld,"
           "\"neural\":\"%s\"}\n",
           argv[2], argv[3], argv[4], seed, games, tally.wins[SIDE_LEFT],
           tally.wins[SIDE_RIGHT], tally.unfinished, tally.points[SIDE_LEFT],
           tally.points[SIDE_RIGHT], tally.ticks, game_neural_status());
    return 0;
}

/* Headless tournaments that pin the CPU levels and the neural player:
 * the scripted autopilot takes a strictly smaller share of the points at
 * each harder level but still scores at HARD (every level is beatable), and
 * the neural player beats HARD from either side. */
static int ai_test(void)
{
    int failures = 0;
    MatchTally tally;
    double share[LEVEL_COUNT];
    for (int level = 0; level < LEVEL_COUNT; level++) {
        if (!play_matches(CTRL_HUMAN, CTRL_CPU, level, 20, 10, 60L * 60 * 20, &tally))
            return 1;
        long total = tally.points[SIDE_LEFT] + tally.points[SIDE_RIGHT];
        share[level] = total ? (double)tally.points[SIDE_LEFT] / (double)total : 0.0;
        printf("ai-test: autopilot vs CPU %-6s wins %2d/10  points %3ld-%3ld\n",
               game_level_name(level), tally.wins[SIDE_LEFT],
               tally.points[SIDE_LEFT], tally.points[SIDE_RIGHT]);
    }
    if (!(share[LEVEL_EASY] > share[LEVEL_NORMAL] && share[LEVEL_NORMAL] > share[LEVEL_HARD])) {
        fprintf(stderr, "FAIL: CPU levels are not ordered by difficulty\n");
        failures++;
    }
    if (share[LEVEL_HARD] < 0.2) {
        fprintf(stderr, "FAIL: HARD is not beatable by the scripted player\n");
        failures++;
    }

    if (!game_neural_ready()) {
        fprintf(stderr, "FAIL: neural policy: %s\n", game_neural_status());
        return 1;
    }
    for (int side = 0; side < SIDE_COUNT; side++) {
        bool left = side == SIDE_LEFT;
        if (!play_matches(left ? CTRL_NEURAL : CTRL_CPU, left ? CTRL_CPU : CTRL_NEURAL,
                          LEVEL_HARD, 40, 6, 60L * 60 * 20, &tally))
            return 1;
        printf("ai-test: neural (%s) vs CPU HARD wins %d/6  points %ld-%ld\n",
               left ? "left" : "right", tally.wins[side],
               tally.points[side], tally.points[1 - side]);
        if (tally.wins[side] < 5) {
            fprintf(stderr, "FAIL: neural player on the %s did not beat HARD\n",
                    left ? "left" : "right");
            failures++;
        }
    }
    if (failures) {
        fprintf(stderr, "ai-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("ai-test: levels ordered and beatable; neural %s\n", game_neural_status());
    return 0;
}

static void prepare_ball(float x, float y, float vx, float vy)
{
    G.state = GS_PLAYING;
    G.ball.active = true;
    G.ball.x = x;
    G.ball.y = y;
    G.ball.vx = vx;
    G.ball.vy = vy;
    G.ball.speed = hypotf(vx, vy);
    G.rally = 0;
}

static int rules_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (condition) printf("PASS: %s\n", label); \
    else { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
} while (0)

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    prepare_ball(160, BALL_RADIUS + .1f, 130, -180);
    game_tick();
    EXPECT(G.ball.vy > 0 && G.ball.y >= BALL_RADIUS,
           "top wall reflects and separates the ball");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    Paddle *left = &G.paddles[SIDE_LEFT];
    prepare_ball(left->x + left->w + BALL_RADIUS + 2.2f,
                 left->y + left->h * .5f, -BALL_SPEED_MAX, 0);
    game_tick();
    EXPECT(G.ball.vx > 0 && G.rally == 1,
           "maximum-speed ball cannot tunnel through a paddle face");
    int rally_after_hit = G.rally;
    game_tick();
    EXPECT(G.ball.vx > 0 && G.rally == rally_after_hit,
           "post-contact separation prevents a double paddle hit");
    EXPECT(G.ball.speed <= BALL_SPEED_MAX + .01f,
           "paddle acceleration respects the hard speed cap");
    EXPECT(fabsf(G.ball.vx) >= G.ball.speed * BALL_MIN_VX - .05f,
           "paddle english preserves a minimum horizontal component");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    left = &G.paddles[SIDE_LEFT];
    prepare_ball(left->x + left->w + BALL_RADIUS + .4f,
                 left->y + .25f, -BALL_SPEED_MAX, 0);
    game_tick();
    EXPECT(G.ball.vx > 0 && G.rally == 1,
           "an inbound ball at the paddle corner is deflected");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    left = &G.paddles[SIDE_LEFT];
    prepare_ball(left->x - BALL_RADIUS - .25f,
                 left->y + left->h * .5f, -BALL_SPEED_MIN, 0);
    game_tick();
    EXPECT(G.ball.vx < 0 && G.rally == 0,
           "a ball fully behind the paddle cannot hit its back face");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    int right_before = G.paddles[SIDE_RIGHT].score;
    prepare_ball(-BALL_RADIUS - 1, 90, -BALL_SPEED_MIN, 0);
    game_tick();
    EXPECT(G.paddles[SIDE_RIGHT].score == right_before + 1,
           "a left-side miss awards exactly one right-side point");
    EXPECT(G.serve_to == SIDE_LEFT,
           "the side that conceded is selected as the next receiver");
    for (int tick = 0; tick < 120 && G.state == GS_POINT; tick++) game_tick();
    EXPECT(G.state == GS_SERVE && G.serve_to == SIDE_LEFT && G.ball.vx < 0,
           "the post-point serve travels toward its named receiver");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start();
    G.paddles[SIDE_RIGHT].score = WIN_SCORE - 1;
    prepare_ball(-BALL_RADIUS - 1, 90, -BALL_SPEED_MIN, 0);
    game_tick();
    EXPECT(G.state == GS_GAMEOVER && G.winner == SIDE_RIGHT &&
           G.paddles[SIDE_RIGHT].score == WIN_SCORE,
           "first side to eleven ends the match with the correct winner");

    game_init(960, 540, 4321);
    G.headless = true;
    game_start();
    for (int tick = 0; tick < 900; tick++) {
        game_autopilot();
        game_tick();
    }
    Paddle *right = &G.paddles[SIDE_RIGHT];
    EXPECT(right->y >= 0 && right->y + right->h <= LOGICAL_H + .01f,
           "AI paddle remains inside the logical playfield");
    char error[192];
    EXPECT(game_validate(error, sizeof error),
           "game_validate accepts the exercised deterministic state");

    /* ---- title menu ---- */
    game_init(960, 540, 99);
    G.headless = true;
    KeyEvent key = { KEY_DOWN, 0, KEY_ACTION_PRESS };
    game_handle_event(&key);                              /* row -> RIGHT */
    key.key = KEY_RIGHT;
    game_handle_event(&key);                              /* CPU -> NEURAL */
    key.key = 's';
    game_handle_event(&key);                              /* row -> LEVEL (W/S work too) */
    key.key = KEY_LEFT;
    game_handle_event(&key);                              /* NORMAL -> EASY */
    game_handle_event(&key);                              /* EASY -> HARD (wraps) */
    EXPECT(G.menu_row == MENU_LEVEL && G.setup[SIDE_RIGHT] == CTRL_NEURAL &&
           G.level == LEVEL_HARD && G.setup[SIDE_LEFT] == CTRL_HUMAN,
           "title menu rows and values respond to arrows and W/S");
    key.key = KEY_ENTER;
    game_handle_event(&key);
    EXPECT(G.state == GS_SERVE &&
           G.paddles[SIDE_RIGHT].controller == CTRL_NEURAL &&
           G.paddles[SIDE_LEFT].controller == CTRL_HUMAN,
           "Enter starts a match with the menu's controllers");
    key.key = KEY_DOWN;
    key.action = KEY_ACTION_PRESS;
    game_handle_event(&key);
    EXPECT(G.menu_row == MENU_LEVEL, "arrow keys leave the menu alone once playing");

    game_init(960, 540, 99);
    game_configure(99, -4, 12);
    EXPECT(G.setup[SIDE_LEFT] == CTRL_HUMAN && G.setup[SIDE_RIGHT] == CTRL_CPU &&
           G.level == LEVEL_NORMAL,
           "game_configure clamps out-of-range setup to the defaults");

    /* ---- a lone human on the right may use W/S ---- */
    game_init(960, 540, 5);
    G.headless = true;
    game_configure(CTRL_CPU, CTRL_HUMAN, LEVEL_EASY);
    game_start();
    float before = G.paddles[SIDE_RIGHT].y;
    KeyEvent w = { 'w', 0, KEY_ACTION_PRESS };
    game_handle_event(&w);
    for (int tick = 0; tick < 5; tick++) game_tick();
    EXPECT(G.paddles[SIDE_RIGHT].y < before,
           "a lone right-side human is driven by W/S");

    /* ---- CPU reaction delay ---- */
    game_init(960, 540, 1234);
    G.headless = true;
    game_configure(CTRL_HUMAN, CTRL_CPU, LEVEL_EASY);
    game_start();
    prepare_ball(100, 20, 300, 0);            /* inbound to the CPU, far off-centre */
    float cpu_y = G.paddles[SIDE_RIGHT].y;
    for (int tick = 0; tick < 10; tick++) game_tick();
    EXPECT(G.paddles[SIDE_RIGHT].y == cpu_y && G.paddles[SIDE_RIGHT].cpu.phase == CPU_REACTING,
           "EASY CPU holds still through its reaction delay");
    for (int tick = 0; tick < 25; tick++) game_tick();   /* reaction <= 22 ticks; arrival ~41 */
    EXPECT(G.ball.vx > 0.0f && G.paddles[SIDE_RIGHT].y < cpu_y &&
           G.paddles[SIDE_RIGHT].cpu.phase == CPU_TRACKING,
           "the CPU tracks toward the ball after reacting");

    /* ---- the whole simulation is in G: a copy replays exactly ---- */
    game_init(960, 540, 777);
    G.headless = true;
    game_configure(CTRL_CPU, CTRL_CPU, LEVEL_HARD);
    game_start();
    for (int tick = 0; tick < 3000; tick++) game_tick();
    GameState saved = G;
    for (int tick = 0; tick < 3000; tick++) game_tick();
    uint64_t first = game_state_digest();
    G = saved;
    for (int tick = 0; tick < 3000; tick++) game_tick();
    EXPECT(game_state_digest() == first,
           "restoring a copied GameState replays CPU play bit-identically");

    /* ---- neural features are mirrored per side ---- */
    game_init(960, 540, 42);
    G.headless = true;
    game_start();
    prepare_ball(100, 60, -250, 80);
    G.paddles[SIDE_LEFT].y = 30;
    G.paddles[SIDE_RIGHT].y = 110;
    G.paddles[SIDE_RIGHT].vy = -40;
    float left_view[POLICY_FEATURES], right_view[POLICY_FEATURES];
    game_policy_features(SIDE_LEFT, left_view);
    G.ball.x = LOGICAL_W - G.ball.x;
    G.ball.vx = -G.ball.vx;
    G.paddles[SIDE_LEFT].y = 110;
    G.paddles[SIDE_LEFT].vy = -40;
    G.paddles[SIDE_RIGHT].y = 30;
    G.paddles[SIDE_RIGHT].vy = 0;
    game_policy_features(SIDE_RIGHT, right_view);
    bool mirrored = true;
    for (int index = 0; index < POLICY_FEATURES; index++)
        if (fabsf(left_view[index] - right_view[index]) > 1e-5f) mirrored = false;
    EXPECT(mirrored && left_view[7] > 0.0f,
           "a mirrored table gives the right side the left side's features");
    EXPECT(game_neural_ready(), "the compiled-in neural policy loads");

#undef EXPECT
    if (failures) {
        fprintf(stderr, "rules-test: %d failure%s\n", failures,
                failures == 1 ? "" : "s");
        return 1;
    }
    puts("rules-test: all targeted rules passed");
    return 0;
}

static bool ensure_directory(char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    return false;
}

static bool ensure_directories(char *path)
{
    if (!path || !*path) return false;
    for (char *cursor = path + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (!ensure_directory(path)) return false;
        *cursor = '/';
    }
    return ensure_directory(path);
}

static bool snapshot(const char *directory, const char *filename)
{
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", directory, filename) >=
        (int)sizeof path)
        return false;
    render_frame();
    if (!render_dump_ppm(path)) {
        fprintf(stderr, "render-test: cannot write %s: %s\n", path,
                strerror(errno));
        return false;
    }
    return true;
}

static int render_test(unsigned seed, const char *output_directory)
{
    if (!output_directory || !*output_directory) {
        fprintf(stderr,
                "render-test: provide an output directory as the final argument "
                "or KILIX_PONG_RENDER_DIR\n");
        return 2;
    }
    char directory[768];
    (void)snprintf(directory, sizeof directory, "%s", output_directory);
    size_t length = strlen(directory);
    while (length > 1 && directory[length - 1] == '/') directory[--length] = '\0';
    char directory_copy[sizeof directory];
    (void)snprintf(directory_copy, sizeof directory_copy, "%s", directory);
    if (!ensure_directories(directory_copy)) {
        fprintf(stderr, "render-test: cannot create %s: %s\n", directory,
                strerror(errno));
        return 1;
    }

    game_init(960, 540, seed);
    G.headless = true;
    render_init(G.W, G.H);
    if (!render_fb()) return 1;

    bool okay = snapshot(directory, "render_title.ppm");
    game_configure(CTRL_NEURAL, CTRL_CPU, LEVEL_HARD);
    G.menu_row = MENU_LEVEL;
    okay = okay && snapshot(directory, "render_title_menu.ppm");
    game_configure(CTRL_HUMAN, CTRL_CPU, LEVEL_NORMAL);
    G.menu_row = MENU_LEFT;
    game_start();
    okay = okay && snapshot(directory, "render_serve.ppm");

    G.state = GS_PLAYING;
    G.paddles[SIDE_LEFT].controller = CTRL_NEURAL;   /* scoreboard label */
    G.ball.active = true;
    G.ball.x = 188;
    G.ball.y = 73;
    G.ball.vx = 210;
    G.ball.vy = -84;
    G.ball.speed = hypotf(G.ball.vx, G.ball.vy);
    G.paddles[SIDE_LEFT].score = 4;
    G.paddles[SIDE_RIGHT].score = 6;
    G.rally = 8;
    for (int index = 0; index < BALL_TRAIL_LEN; index++) {
        G.ball.trail_x[index] = G.ball.x - index * 3.2f;
        G.ball.trail_y[index] = G.ball.y + index * 1.2f;
    }
    G.ball.trail_head = 0;
    for (int index = 0; index < 18; index++) {
        G.particles[index] = (Particle){
            .x = 166 + (index % 6) * 3.2f,
            .y = 84 + (index / 6) * 3.0f,
            .vx = 0, .vy = 0, .life = .5f + index * .01f, .max_life = .8f,
            .color = index & 1 ? 0xf472b6ffU : 0x22d3eeffU, .active = true
        };
    }
    okay = okay && snapshot(directory, "render_playing.ppm");

    G.state = GS_POINT;
    G.flash = .55f;
    okay = okay && snapshot(directory, "render_point.ppm");
    G.state = GS_PAUSED;
    G.flash = 0;
    okay = okay && snapshot(directory, "render_paused.ppm");
    G.state = GS_GAMEOVER;
    G.winner = SIDE_RIGHT;
    G.paddles[SIDE_RIGHT].score = WIN_SCORE;
    okay = okay && snapshot(directory, "render_gameover.ppm");

    render_shutdown();
    if (!okay) return 1;
    printf("render-test: wrote 7 deterministic 960x540 PPMs to %s\n", directory);
    return 0;
}

static int asset_check(void)
{
    bool sink_ready = sound_init();
    bool bank_ready = sound_bank_loaded();
    const char *sink = sound_sink_name();
    printf("asset-check: bank=%s sink=%s sample=%s\n",
           bank_ready ? "loaded" : "missing",
           sink_ready && sink ? sink : "none",
           asset_path("sfx/paddle.wav"));
    sound_shutdown();
    return bank_ready ? 0 : 1;
}

static int sound_test(void)
{
    bool sink_ready = sound_init();
    bool bank_ready = sound_bank_loaded();
    const char *sink = sound_sink_name();
    if (!bank_ready) {
        fprintf(stderr, "sound-test: production WAV bank missing or invalid\n");
        sound_shutdown();
        return 1;
    }
    if (!sink_ready || !sink) {
        puts("sound-test: WAV bank loaded; no pacat, pw-play, aplay, or play sink opened");
        sound_shutdown();
        return 0;
    }

    printf("sound-test: bank loaded; playing 7 cues x 3 variants through %s\n",
           sink);
    static const int delays[SFX_COUNT] = {140, 120, 420, 220, 420, 150, 850};
    for (int cue = 0; cue < SFX_COUNT; cue++) {
        for (int variant = 0; variant < SFX_VARIANTS; variant++) {
            sound_play(cue, .75f, 1.0f);
            sleep_milliseconds(delays[cue]);
        }
    }
    sound_shutdown();
    return 0;
}

static void usage(void)
{
    puts("kilix-pong - luminous paddle-ball arcade for Kitty terminals\n"
         "\nusage: kilix-pong [option]\n"
         "  (no option)                         play interactively\n"
         "  --left C --right C --level L        play with this setup\n"
         "                                      C: human|cpu|neural  L: easy|normal|hard\n"
         "  --rules-test                        targeted gameplay regressions\n"
         "  --ai-test                           CPU-level and neural tournaments\n"
         "  --selftest [seed] [ticks]           deterministic simulation\n"
         "  --render-test [seed] [directory]    write seven PPM fixtures\n"
         "  --asset-check                       validate runtime WAV discovery\n"
         "  --match L R LEVEL SEED GAMES        headless tournament, JSON summary\n"
         "  --sound-test                        play every production cue variant\n"
         "  --version                           print version\n"
         "  --help                              show this help\n"
         "\nenvironment:\n"
         "  KILIX_PONG_ASSETS=/path             override asset root\n"
         "  KILIX_PONG_RENDER_DIR=/path         render-test output directory\n"
         "  KILIX_PONG_SKIP_PROBE=1             skip Kitty graphics query\n"
         "\ncontrols:\n"
         "  W/S player 1   Up/Down player 2   P/Esc pause   M sound   Q quit\n"
         "  title menu: Up/Down pick LEFT, RIGHT or LEVEL; Left/Right change it");
}

int main(int argc, char **argv)
{
    asset_paths_init();
    if (argc == 1) return run_interactive();
    if (!strcmp(argv[1], "--left") || !strcmp(argv[1], "--right") ||
        !strcmp(argv[1], "--level")) {
        for (int index = 1; index < argc; index += 2) {
            const char *value = index + 1 < argc ? argv[index + 1] : NULL;
            int parsed = -1, slot = -1;
            if (!strcmp(argv[index], "--left")) slot = 0, parsed = parse_controller(value);
            else if (!strcmp(argv[index], "--right")) slot = 1, parsed = parse_controller(value);
            else if (!strcmp(argv[index], "--level")) slot = 2, parsed = parse_level(value);
            if (slot < 0 || parsed < 0) {
                fprintf(stderr, "kilix-pong: bad setup option '%s %s'\n",
                        argv[index], value ? value : "");
                usage();
                return 2;
            }
            cli_setup[slot] = parsed;
        }
        return run_interactive();
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return 0;
    }
    if (!strcmp(argv[1], "--version")) {
        puts("kilix-pong " VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "--rules-test")) return rules_test();
    if (!strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int ticks = argc > 3 ? atoi(argv[3]) : 12000;
        return selftest(seed, ticks);
    }
    if (!strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        const char *directory = argc > 3 ? argv[3] :
                                getenv("KILIX_PONG_RENDER_DIR");
        return render_test(seed, directory);
    }
    if (!strcmp(argv[1], "--asset-check")) return asset_check();
    if (!strcmp(argv[1], "--match")) return match_command(argc, argv);
    if (!strcmp(argv[1], "--ai-test")) return ai_test();
    if (!strcmp(argv[1], "--sound-test")) return sound_test();
    fprintf(stderr, "kilix-pong: unknown option '%s'\n", argv[1]);
    usage();
    return 2;
}
