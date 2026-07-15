/* Entry point, headless checks, asset discovery, and fixed-step loop. */
#include "kilix_pong.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"

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
    game_start(MODE_AI);

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
    game_start(MODE_AI);
    prepare_ball(160, BALL_RADIUS + .1f, 130, -180);
    game_tick();
    EXPECT(G.ball.vy > 0 && G.ball.y >= BALL_RADIUS,
           "top wall reflects and separates the ball");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start(MODE_AI);
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
    game_start(MODE_AI);
    left = &G.paddles[SIDE_LEFT];
    prepare_ball(left->x + left->w + BALL_RADIUS + .4f,
                 left->y + .25f, -BALL_SPEED_MAX, 0);
    game_tick();
    EXPECT(G.ball.vx > 0 && G.rally == 1,
           "an inbound ball at the paddle corner is deflected");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start(MODE_AI);
    left = &G.paddles[SIDE_LEFT];
    prepare_ball(left->x - BALL_RADIUS - .25f,
                 left->y + left->h * .5f, -BALL_SPEED_MIN, 0);
    game_tick();
    EXPECT(G.ball.vx < 0 && G.rally == 0,
           "a ball fully behind the paddle cannot hit its back face");

    game_init(960, 540, 1234);
    G.headless = true;
    game_start(MODE_AI);
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
    game_start(MODE_AI);
    G.paddles[SIDE_RIGHT].score = WIN_SCORE - 1;
    prepare_ball(-BALL_RADIUS - 1, 90, -BALL_SPEED_MIN, 0);
    game_tick();
    EXPECT(G.state == GS_GAMEOVER && G.winner == SIDE_RIGHT &&
           G.paddles[SIDE_RIGHT].score == WIN_SCORE,
           "first side to eleven ends the match with the correct winner");

    game_init(960, 540, 4321);
    G.headless = true;
    game_start(MODE_AI);
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
    game_start(MODE_AI);
    okay = okay && snapshot(directory, "render_serve.ppm");

    G.state = GS_PLAYING;
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
    printf("render-test: wrote 6 deterministic 960x540 PPMs to %s\n", directory);
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
         "  --rules-test                        targeted gameplay regressions\n"
         "  --selftest [seed] [ticks]           deterministic simulation\n"
         "  --render-test [seed] [directory]    write six PPM fixtures\n"
         "  --asset-check                       validate runtime WAV discovery\n"
         "  --sound-test                        play every production cue variant\n"
         "  --version                           print version\n"
         "  --help                              show this help\n"
         "\nenvironment:\n"
         "  KILIX_PONG_ASSETS=/path             override asset root\n"
         "  KILIX_PONG_RENDER_DIR=/path         render-test output directory\n"
         "  KILIX_PONG_SKIP_PROBE=1             skip Kitty graphics query\n"
         "\ncontrols:\n"
         "  W/S player 1   Up/Down player 2   P/Esc pause   M sound   Q quit");
}

int main(int argc, char **argv)
{
    asset_paths_init();
    if (argc == 1) return run_interactive();
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
    if (!strcmp(argv[1], "--sound-test")) return sound_test();
    fprintf(stderr, "kilix-pong: unknown option '%s'\n", argv[1]);
    usage();
    return 2;
}
