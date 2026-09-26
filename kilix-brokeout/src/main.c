/* Entry point: terminal setup, fixed timestep, and headless checks. */
#define _XOPEN_SOURCE 700              /* nftw(), for scratch-storage cleanup */
#include "kitty_brokeout.h"
#include <ftw.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void on_signal(int sig)
{
    (void)sig;
    term_emergency_restore();
    _exit(1);
}

static bool event_matches_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static int game_key_from_event(const kittykb_event *event)
{
    static const char letters[] = "acdmnpqrsw";
    for (size_t i = 0; i < sizeof letters - 1; i++)
        if (event_matches_letter(event, letters[i])) return letters[i];

    switch (event->key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    default:
        return event->key <= (uint32_t)INT_MAX ? (int)event->key : -1;
    }
}

static bool direction_key(int key)
{
    return key == KEY_LEFT || key == KEY_RIGHT || key == 'a' || key == 'd';
}

static bool interrupt_event(const kittykb_event *event)
{
    return event->key == 3u ||
           (event_matches_letter(event, 'c') &&
            (event->modifiers & KITTYKB_MOD_CTRL) != 0u);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms)
{
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(fmod(ms, 1000.0) * 1e6) };
    nanosleep(&ts, NULL);
}

static const char *render_dir = ".";

static void dump_ppm(const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", render_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", G.W, G.H);
    const uint8_t *p = render_fb();
    for (int i = 0; i < G.W * G.H; i++)
        fwrite(p + i * 4, 1, 3, f);
    fclose(f);
    printf("wrote %s\n", path);
}

static int selftest(unsigned seed, int ticks)
{
    if (ticks <= 0) ticks = 7200;
    game_init(1000, 640, seed);
    G.headless = true;

    int clears = 0;
    int lastLevel = G.level;
    for (int i = 0; i < ticks; i++) {
        game_autopilot_tick();
        game_tick();

        for (int b = 0; b < MAX_BALLS; b++) {
            Ball *ball = &G.balls[b];
            if (!ball->active) continue;
            if (isnan(ball->x) || isnan(ball->y) || isnan(ball->vx) || isnan(ball->vy)) {
                printf("FAIL: NaN ball at tick %d\n", i);
                game_shutdown();
                return 1;
            }
        }
        if (G.level != lastLevel) {
            clears++;
            lastLevel = G.level;
        }
        if (G.score < 0 || G.lives < 0 || G.numBricks < 0 || G.numBricks > MAX_BRICKS) {
            printf("FAIL: invalid state at tick %d score=%d lives=%d bricks=%d\n",
                   i, G.score, G.lives, G.numBricks);
            game_shutdown();
            return 1;
        }
    }

    game_start_run();
    int before = game_remaining_breakable_bricks();
    game_force_level_clear();
    if (before <= 0 || G.state != GS_LEVEL_CLEAR || game_remaining_breakable_bricks() != 0) {
        printf("FAIL: force clear failed before=%d state=%d remaining=%d\n",
               before, G.state, game_remaining_breakable_bricks());
        game_shutdown();
        return 1;
    }

    game_force_gameover();
    if (G.state != GS_GAMEOVER || G.lives != 0) {
        printf("FAIL: force gameover failed state=%d lives=%d\n", G.state, G.lives);
        game_shutdown();
        return 1;
    }

    printf("PASS: seed=%u ticks=%d score=%d level=%d clears=%d state=%d\n",
           seed, ticks, G.score, G.level, clears, G.state);
    game_shutdown();
    return 0;
}

static int input_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    game_init(1000, 640, 1337);
    G.headless = true;
    game_start_run();

    float before = G.paddle.x;
    game_set_held_controls(true, false, true);
    game_tick();
    EXPECT(G.paddle.x > before && G.paddle.moveAxis == 1.0f,
           "held right moves the paddle continuously");

    before = G.paddle.x;
    game_set_held_controls(true, true, true);
    game_tick();
    EXPECT(fabsf(G.paddle.x - before) < 0.001f && G.paddle.moveAxis == 0.0f,
           "simultaneous opposite directions cancel");

    game_set_held_controls(true, true, false);
    game_tick();
    EXPECT(G.paddle.x < before && G.paddle.moveAxis == -1.0f,
           "releasing right preserves held left");

    before = G.paddle.x;
    game_set_held_controls(true, false, false);
    game_tick();
    EXPECT(fabsf(G.paddle.x - before) < 0.001f && G.paddle.moveAxis == 0.0f,
           "release stops the paddle immediately");

    G.launchAngle = 0.0f;
    game_handle_key('a');
    EXPECT(G.launchAngle < 0.0f,
           "direction presses retain pre-launch aim adjustment");

    game_set_held_controls(false, false, false);
    before = G.paddle.x;
    game_handle_key('d');
    game_tick();
    EXPECT(G.paddle.x > before,
           "legacy press-only fallback retains paddle intent");
    for (int i = 0; i < 10; i++) game_tick();
    before = G.paddle.x;
    game_tick();
    EXPECT(fabsf(G.paddle.x - before) < 0.001f && G.paddle.moveAxis == 0.0f,
           "legacy paddle intent expires without release events");

    game_shutdown();
#undef EXPECT
    return failures ? 1 : 0;
}

/* ---------- player and menu tests ---------- */

static const int test_sizes[][2] = {
    { 1000, 640 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 800, 500 }, { 2560, 1440 },
    { 3840, 2160 }, { 3440, 1440 }, { 1366, 768 }, { 2000, 600 }
};

/* One level played from its start, as tools/neural/brokeout_lab.c plays it:
   0 cleared, 1 ball lost, 2 time up. */
static int play_level(int player, unsigned seed, int k, int seconds)
{
    game_init(test_sizes[k % 10][0], test_sizes[k % 10][1], seed);
    G.headless = true;
    G.player = player;
    game_start_level(1 + (k / 10) % 20);
    int lives = G.lives, outcome = 2;
    for (int t = 0; t < seconds * 60; t++) {
        player_tick();
        game_tick();
        if (G.state == GS_LEVEL_CLEAR) { outcome = 0; break; }
        if (G.lives < lives || G.state == GS_GAMEOVER) { outcome = 1; break; }
    }
    return outcome;
}

static int player_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    EXPECT(player_neural_ready(), "the compiled-in neural player loads");
    printf("player-test: %s\n", player_neural_status());
    /* Regression seeds 8000000.. (neither selection nor held-out). */
    enum { N = 400, SECONDS = 180 };
    int nc = 0, nl = 0, ac = 0, al = 0;
    for (int k = 0; k < N; k++) {
        int n = play_level(PLAYER_NEURAL, 8000000u + (unsigned)k, k, SECONDS);
        int a = play_level(PLAYER_AUTOPILOT, 8000000u + (unsigned)k, k, SECONDS);
        nc += n == 0; nl += n == 1;
        ac += a == 0; al += a == 1;
    }
    printf("player-test: %d levels, %d s each: neural cleared %d, lost a ball %d; "
           "autopilot cleared %d, lost a ball %d\n", N, SECONDS, nc, nl, ac, al);
    /* Shipped: neural cleared 59 and lost 22; the autopilot cleared 0 and lost 30.
       Games are deterministic; the margins catch a broken or foreign network. */
    EXPECT(nc >= ac + 30, "the neural player cleanly clears at least 30 more of 400 levels than the autopilot");
    EXPECT(nl <= al + 10, "without losing noticeably more balls");

    game_init(1280, 720, 99);
    G.headless = true;
    G.player = PLAYER_NEURAL;
    game_start_run();
    for (int t = 0; t < 900; t++) { player_tick(); game_tick(); }
    float x1 = G.paddle.x;
    int s1 = G.score;
    game_init(1280, 720, 99);
    G.headless = true;
    G.player = PLAYER_NEURAL;
    game_start_run();
    for (int t = 0; t < 900; t++) { player_tick(); game_tick(); }
    EXPECT(G.paddle.x == x1 && G.score == s1, "neural games replay exactly");
#undef EXPECT
    return failures ? 1 : 0;
}

static int menu_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    game_init(1000, 640, 5);
    G.headless = true;
    EXPECT(G.state == GS_TITLE && G.menuRow == MENU_START, "the game opens on the main menu");
    game_handle_key('q');
    EXPECT(!G.quit, "Q does not quit");
    game_handle_key(KEY_ESC);
    EXPECT(!G.quit && G.menuRow == MENU_QUIT, "Esc on the menu selects QUIT without quitting");
    game_handle_key(KEY_DOWN);                    /* wraps to START */
    game_handle_key(KEY_DOWN);                    /* PLAYER */
    game_handle_key(KEY_RIGHT);                   /* YOU -> NEURAL */
    EXPECT(G.player == PLAYER_NEURAL && G.state == GS_TITLE, "the PLAYER row changes the player");
    G.menuRow = MENU_START;
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING && G.controller == PLAYER_NEURAL, "START plays with the chosen player");
    for (int t = 0; t < 60; t++) { player_tick(); game_tick(); }
    EXPECT(game_active_ball_count() == 1 && !G.balls[0].attached, "the neural player launches the ball itself");
    game_handle_key('n');
    EXPECT(G.controller == PLAYER_YOU, "N takes the paddle");
    game_handle_key('n');
    EXPECT(G.controller == PLAYER_NEURAL, "N hands it back");
    game_handle_key('p');
    EXPECT(G.state == GS_PAUSED && G.pauseRow == PAUSE_RESUME, "P opens the pause menu");
    float bx = G.balls[0].x;
    game_tick();
    EXPECT(G.balls[0].x == bx, "nothing moves while paused");
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_PLAYING, "Esc resumes");
    game_handle_key(KEY_ESC);
    game_handle_key(KEY_DOWN);                    /* RESTART */
    game_handle_key(KEY_DOWN);                    /* MAIN MENU */
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_TITLE, "the pause menu's MAIN MENU returns to the main menu");
    G.menuRow = MENU_QUIT;
    game_handle_key(KEY_ENTER);
    EXPECT(G.quit, "the main menu's QUIT exits");

    game_init(1000, 640, 5);
    G.headless = true;
    game_start_run();
    game_force_gameover();
    EXPECT(G.state == GS_GAMEOVER, "a lost game shows the game-over menu");
    G.overRow = OVER_AGAIN;
    game_handle_key(KEY_UP);                      /* wraps to QUIT */
    game_handle_key(KEY_ENTER);
    EXPECT(G.quit, "the game-over menu's QUIT exits");

    game_init(1000, 640, 5);
    G.headless = true;
    game_start_run();
    game_force_level_clear();
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_PAUSED, "Esc on the level-clear screen opens the pause menu");
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_LEVEL_CLEAR, "and Resume returns to the level-clear screen");
    game_handle_key('p');
    game_handle_key(KEY_DOWN);
    game_handle_key(KEY_DOWN);                    /* MAIN MENU */
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_TITLE, "the main menu is reachable from a cleared level");

    game_init(1000, 640, 5);
    G.headless = true;
    G.player = PLAYER_NEURAL;
    game_start_run();
    game_force_level_clear();
    G.controller = PLAYER_NEURAL;
    for (int t = 0; t < 200 && G.state == GS_LEVEL_CLEAR; t++) game_tick();
    EXPECT(G.state == GS_PLAYING && G.level == 2, "a computer player moves on after a clear by itself");

    /* Pausing its clear screen must not use up that wait. */
    game_init(1000, 640, 5);
    G.headless = true;
    G.player = PLAYER_NEURAL;
    game_start_run();
    game_force_level_clear();
    G.controller = PLAYER_NEURAL;
    for (int t = 0; t < 60; t++) game_tick();         /* 1 s of the 2.2 s wait */
    game_handle_key('p');
    for (int t = 0; t < 600; t++) game_tick();        /* 10 s paused */
    game_handle_key(KEY_ESC);
    game_tick();
    EXPECT(G.state == GS_LEVEL_CLEAR, "a long pause on a computer player's clear screen keeps its wait");
    int waited = 1;
    while (G.state == GS_LEVEL_CLEAR && waited < 400) { game_tick(); waited++; }
    EXPECT(G.state == GS_PLAYING && waited >= 60 && waited <= 80,
           "and the rest of the wait (about 1.2 s) runs after Resume");
#undef EXPECT
    return failures ? 1 : 0;
}

static int render_test(unsigned seed)
{
    game_init(1000, 640, seed);
    G.headless = true;
    render_init(G.W, G.H);

    render_frame();
    dump_ppm("render_title.ppm");

    game_start_run();
    G.launchAngle = 0.48f;
    render_frame();
    dump_ppm("render_ready.ppm");

    G.player = G.controller = PLAYER_NEURAL;
    for (int i = 0; i < 180; i++) {
        player_tick();
        game_tick();
    }
    render_frame();
    dump_ppm("render_playing.ppm");
    G.state = GS_PAUSED;
    G.pauseRow = PAUSE_MENU;
    render_frame();
    dump_ppm("render_paused.ppm");
    G.state = GS_PLAYING;
    G.player = G.controller = PLAYER_YOU;

    game_force_level_clear();
    render_frame();
    dump_ppm("render_clear.ppm");

    game_force_gameover();
    render_frame();
    dump_ppm("render_gameover.ppm");

    render_shutdown();
    game_shutdown();
    return 0;
}

static int sound_test(void)
{
    bool ok = sound_init();
    if (!ok)
        printf("sound-test: no supported audio sink found; game will run silent\n");
    else
        printf("sound-test: playing procedural sounds\n");

    sound_play(SND_MENU, 0.5f, 1.0f);
    sleep_ms(140);
    sound_play(SND_LAUNCH, 0.55f, 1.0f);
    sleep_ms(160);
    sound_play(SND_PADDLE, 0.55f, 1.0f);
    sleep_ms(140);
    sound_play(SND_BRICK, 0.55f, 1.0f);
    sleep_ms(140);
    sound_play(SND_METAL, 0.55f, 1.0f);
    sleep_ms(160);
    sound_play(SND_EXPLODE, 0.75f, 1.0f);
    sleep_ms(700);
    sound_play(SND_POWERUP, 0.65f, 1.0f);
    sleep_ms(350);
    sound_play(SND_CLEAR, 0.75f, 1.0f);
    sleep_ms(900);
    sound_shutdown();
    return 0;
}

static int run_interactive(void)
{
    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "kitty-brokeout: needs an interactive kitty-protocol terminal\n");
        fprintf(stderr, "or run --selftest / --render-test.\n");
        return 1;
    }
    /* Restore the terminal on every signal whose default action ends the
       process, not only Ctrl+C and SIGTERM. */
    static const int fatal[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGSEGV, SIGBUS,
                                 SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof fatal / sizeof fatal[0]; i++) signal(fatal[i], on_signal);
    atexit(term_shutdown);

    game_init(w, h, (uint32_t)time(NULL));
    render_init(w, h);
    sound_init();

    const double frameMs = 1000.0 / 30.0;
    double next = now_ms();

    while (!G.quit) {
        kittykb_event event;
        if (term_read_input() < 0) {
            G.quit = true;
            break;
        }
        bool heldInput = term_has_release_events();
        bool left = term_key_down('a') || term_key_down(KITTYKB_KEY_LEFT);
        bool right = term_key_down('d') || term_key_down(KITTYKB_KEY_RIGHT);
        game_set_held_controls(heldInput, left, right);
        while (term_next_key_event(&event)) {
            if (event.action == KITTYKB_ACTION_RELEASE) continue;
            if (interrupt_event(&event)) {
                G.quit = true;
                continue;
            }
            int key = game_key_from_event(&event);
            if (key < 0 || (event.action == KITTYKB_ACTION_REPEAT &&
                            !direction_key(key))) continue;
            game_handle_key(key);
        }
        if (G.quit) break;

        /* Two simulation ticks per frame; a computer player decides on each
           one, exactly as it was trained. */
        for (int tick = 0; tick < 2; tick++) {
            if (!player_tick() && G.controller == PLAYER_YOU)
                game_set_held_controls(heldInput, left, right);
            game_tick();
        }

        render_frame();
        term_present(render_fb(), G.W, G.H);

        next += frameMs;
        double wait = next - now_ms();
        if (wait < -100) next = now_ms();
        sleep_ms(wait);
    }

    sound_shutdown();
    render_shutdown();
    game_shutdown();
    return 0;
}

/* Every non-interactive mode runs against scratch storage. game_init() opens
   the high-score store (and may migrate a legacy file in place) before any
   caller could mark the game headless, so the isolation has to happen before
   the first game_init(): point XDG data and state at a fresh directory. */
static char scratch_dir[] = "/tmp/kitty-brokeout-test-XXXXXX";

static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void remove_scratch(void)
{
    (void)nftw(scratch_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static bool isolate_storage(void)
{
    if (!mkdtemp(scratch_dir)) {
        perror("kitty-brokeout: cannot create scratch storage");
        return false;
    }
    (void)atexit(remove_scratch);
    return setenv("XDG_DATA_HOME", scratch_dir, 1) == 0 &&
           setenv("XDG_STATE_HOME", scratch_dir, 1) == 0;
}

static bool headless_mode(const char *arg)
{
    static const char *const modes[] = {
        "--selftest", "--input-test", "--render-test", "--sound-test", "--player-test",
        "--menu-test"
    };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (!strcmp(arg, modes[i])) return true;
    return false;
}

int main(int argc, char **argv)
{
    if (argc > 1 && headless_mode(argv[1]) && !isolate_storage()) return 1;
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int ticks = argc > 3 ? atoi(argv[3]) : 7200;
        return selftest(seed, ticks);
    }
    if (argc > 1 && !strcmp(argv[1], "--input-test")) {
        return input_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        if (argc > 3) render_dir = argv[3];   /* default: the current directory */
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test"))
        return sound_test();
    if (argc > 1 && !strcmp(argv[1], "--player-test")) return player_test();
    if (argc > 1 && !strcmp(argv[1], "--menu-test")) return menu_test();
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("kitty-brokeout 0.2.0\n");
        return 0;
    }
    return run_interactive();
}
