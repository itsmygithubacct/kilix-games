/* Entry point: terminal setup, fixed timestep, and headless checks. */
#include "terminal_lander.h"
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
    static const char letters[] = "acdqw";
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

static bool gameplay_control_key(int key)
{
    return key == KEY_UP || key == KEY_LEFT || key == KEY_RIGHT ||
           key == 'w' || key == 'a' || key == 'd';
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
    if (ticks <= 0) ticks = 3600;
    game_init(1000, 640, seed);
    int stabilityState = GS_TITLE;
    for (int i = 0; i < ticks; i++) {
        game_autopilot_tick();
        game_tick();
        Lander *l = &G.lander;
        if (isnan(l->x) || isnan(l->y) || isnan(l->vx) || isnan(l->vy) ||
            isnan(l->angle)) {
            printf("FAIL: NaN at tick %d\n", i);
            game_shutdown();
            return 1;
        }
        if (G.state == GS_GAMEOVER) break;
    }
    stabilityState = G.state;

    game_start_run();
    G.lander.x = G.pad.x + G.pad.width / 2.0f - G.lander.w / 2.0f;
    G.lander.y = G.pad.y - G.lander.h - 5 * G.scale;
    G.lander.vx = 0;
    G.lander.vy = 18 * G.scale;
    G.lander.angle = 0;
    G.lander.angularVelocity = 0;
    for (int i = 0; i < 20 && G.state == GS_PLAYING; i++)
        game_tick();
    if (G.state != GS_LEVEL_COMPLETE || G.score <= 0) {
        printf("FAIL: safe landing did not score (state=%d score=%d)\n", G.state, G.score);
        game_shutdown();
        return 1;
    }
    int landingScore = G.score;

    for (int d = 0; d < DIFF_COUNT; d++) {
        G.difficulty = d;
        game_start_run();
        G.lander.x = G.pad.x + G.pad.width / 2.0f - G.lander.w / 2.0f;
        G.lander.y = G.pad.y - G.lander.h - 5 * G.scale;
        G.lander.vx = 0;
        G.lander.vy = 18 * G.scale;
        G.lander.angle = 0;
        G.lander.angularVelocity = 0;
        for (int i = 0; i < 20 && G.state == GS_PLAYING; i++)
            game_tick();
        if (G.state != GS_LEVEL_COMPLETE || G.score <= 0) {
            printf("FAIL: %s safe landing failed (state=%d score=%d)\n",
                   DIFFICULTY_NAMES[d], G.state, G.score);
            game_shutdown();
            return 1;
        }
    }

    printf("PASS: seed=%u ticks=%d stability_state=%d safe_landing_score=%d difficulties=%d\n",
           seed, ticks, stabilityState, landingScore, DIFF_COUNT);
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
    game_start_run();
    G.lander.x = G.W * 0.5f;
    G.lander.y = G.H * 0.1f;
    G.lander.vx = G.lander.vy = G.lander.angularVelocity = 0;
    G.lander.angle = 0;

    game_set_held_controls(true, true, true, false);
    game_tick();
    EXPECT(G.lander.mainThrust && G.lander.leftThrust && !G.lander.rightThrust,
           "main and side thrust apply simultaneously");

    game_set_held_controls(true, false, true, true);
    game_tick();
    EXPECT(!G.lander.leftThrust && !G.lander.rightThrust,
           "simultaneous opposite side controls cancel");

    game_set_held_controls(true, true, false, true);
    game_tick();
    EXPECT(G.lander.mainThrust && !G.lander.leftThrust && G.lander.rightThrust,
           "releasing one side preserves the other held controls");

    game_set_held_controls(true, false, false, true);
    game_tick();
    EXPECT(!G.lander.mainThrust && G.lander.rightThrust,
           "main thrust release does not release side thrust");

    game_set_held_controls(false, false, false, false);
    game_handle_key('w');
    game_handle_key('a');
    game_tick();
    EXPECT(G.lander.mainThrust && G.lander.leftThrust,
           "legacy press-only fallback retains simultaneous latch input");
    for (int i = 0; i < 6; i++) game_tick();
    EXPECT(!G.lander.mainThrust && !G.lander.leftThrust,
           "legacy input latches expire without release events");

    game_shutdown();
#undef EXPECT
    return failures ? 1 : 0;
}

/* ---------- pilot and menu tests ---------- */

static const int test_sizes[][2] = {
    { 1000, 640 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 800, 500 }, { 2560, 1440 },
    { 3840, 2160 }, { 3440, 1440 }, { 1366, 768 }, { 2000, 600 }
};

/* One level flown from its start by `pilot`, as tools/neural/lander_lab.c
   flies it (levels 1-60, ten terminal sizes): true on a landing. */
static bool fly_level(int pilot, unsigned seed, int k)
{
    game_init(test_sizes[k % 10][0], test_sizes[k % 10][1], seed);
    G.difficulty = k % DIFF_COUNT;
    G.pilot = pilot;
    game_start_run();
    G.level = 1 + (k / DIFF_COUNT) % 60;
    game_create_level();
    for (int t = 0; t < 60 * 90 && G.state == GS_PLAYING; t++) {
        pilot_tick();
        game_tick();
    }
    bool landed = G.state == GS_LEVEL_COMPLETE;
    game_shutdown();
    return landed;
}

static int pilot_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    EXPECT(pilot_neural_ready(), "the compiled-in neural pilot loads");
    printf("pilot-test: %s\n", pilot_neural_status());
    /* Regression seeds 8000000.. (neither selection nor held-out). */
    enum { N = 800 };
    int neural[DIFF_COUNT] = { 0 }, autop[DIFF_COUNT] = { 0 }, nt = 0, at = 0;
    for (int k = 0; k < N; k++) {
        bool n = fly_level(PILOT_NEURAL, 8000000u + (unsigned)k, k);
        bool a = fly_level(PILOT_AUTOPILOT, 8000000u + (unsigned)k, k);
        neural[k % DIFF_COUNT] += n;
        autop[k % DIFF_COUNT] += a;
        nt += n;
        at += a;
    }
    for (int d = 0; d < DIFF_COUNT; d++)
        printf("pilot-test: %-10s neural %3d/%d  autopilot %3d/%d\n", DIFFICULTY_NAMES[d],
               neural[d], N / DIFF_COUNT, autop[d], N / DIFF_COUNT);
    printf("pilot-test: overall    neural %3d/%d  autopilot %3d/%d\n", nt, N, at, N);
    EXPECT(nt >= at + N / 5, "the neural pilot lands at least 20 points more often than the autopilot");
    bool each = true;
    for (int d = 0; d < DIFF_COUNT; d++) each = each && neural[d] >= autop[d];
    EXPECT(each, "and at least as often on every difficulty");

    /* The same flight twice is the same flight. */
    game_init(1280, 720, 99);
    G.pilot = PILOT_NEURAL;
    game_start_run();
    for (int t = 0; t < 300 && G.state == GS_PLAYING; t++) { pilot_tick(); game_tick(); }
    float x1 = G.lander.x, y1 = G.lander.y;
    game_shutdown();
    game_init(1280, 720, 99);
    G.pilot = PILOT_NEURAL;
    game_start_run();
    for (int t = 0; t < 300 && G.state == GS_PLAYING; t++) { pilot_tick(); game_tick(); }
    EXPECT(G.lander.x == x1 && G.lander.y == y1, "neural flights replay exactly");
    game_shutdown();
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
    EXPECT(G.state == GS_TITLE && G.menuRow == MENU_START, "the game opens on the main menu");
    game_handle_key('q');
    EXPECT(!G.quit, "Q does not quit");
    game_handle_key(KEY_ESC);
    EXPECT(!G.quit && G.menuRow == MENU_QUIT, "Esc on the menu selects QUIT without quitting");
    game_handle_key(KEY_DOWN);                    /* wraps to START */
    game_handle_key(KEY_DOWN);                    /* DIFFICULTY */
    game_handle_key(KEY_RIGHT);                   /* Medium -> Hard */
    game_handle_key(KEY_DOWN);                    /* PILOT */
    game_handle_key(KEY_RIGHT);                   /* You -> Neural */
    EXPECT(G.difficulty == DIFF_HARD && G.pilot == PILOT_NEURAL && G.state == GS_TITLE,
           "menu rows change difficulty and pilot");
    G.menuRow = MENU_START;
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING && G.flying == PILOT_NEURAL, "START flies with the chosen pilot");
    game_handle_key('n');
    EXPECT(G.flying == PILOT_YOU, "N takes the controls");
    game_handle_key('n');
    EXPECT(G.flying == PILOT_NEURAL, "N hands them back");
    for (int t = 0; t < 30; t++) { pilot_tick(); game_tick(); }
    float padX = (float)G.pad.x, startY = G.lander.y;
    (void)startY;
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_PAUSED, "Esc in flight opens the pause menu");
    game_tick();
    EXPECT(G.state == GS_PAUSED, "the simulation stops while paused");
    game_handle_key(KEY_DOWN);                    /* RESTART LEVEL */
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING && (float)G.pad.x == padX && G.lander.vx == 0,
           "RESTART LEVEL replays the same terrain from the start");
    game_handle_key('p');
    game_handle_key(KEY_UP);                      /* wraps to QUIT */
    game_handle_key(KEY_ENTER);
    EXPECT(G.quit, "the pause menu's QUIT exits");
    game_shutdown();

    game_init(1000, 640, 5);
    game_start_run();
    G.lives = 1;
    G.lander.vy = 900;                            /* straight into the ground */
    for (int t = 0; t < 400 && G.state != GS_GAMEOVER; t++) game_tick();
    EXPECT(G.state == GS_GAMEOVER && G.overRow == OVER_AGAIN, "losing the last life shows the game-over menu");
    game_handle_key(KEY_DOWN);
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_TITLE, "game-over MAIN MENU returns to the main menu");
    game_shutdown();
#undef EXPECT
    return failures ? 1 : 0;
}

static int render_test(unsigned seed)
{
    game_init(1000, 640, seed);
    render_init(G.W, G.H);

    render_frame();
    dump_ppm("render_title.ppm");

    G.pilot = PILOT_NEURAL;
    game_start_run();
    for (int i = 0; i < 180; i++) {
        pilot_tick();
        game_tick();
    }
    render_frame();
    dump_ppm("render_playing.ppm");
    G.state = GS_PAUSED;
    G.pauseRow = PAUSE_RESTART;
    render_frame();
    dump_ppm("render_paused.ppm");
    G.state = GS_PLAYING;
    G.pilot = PILOT_YOU;

    G.lander.x = G.pad.x - G.lander.w * 2.2f;
    G.lander.y = terrain_height_at(G.lander.x) - G.lander.h - 2;
    G.lander.vy = 300 * G.scale;
    G.lander.angle = 0.7f;
    game_tick();
    for (int i = 0; i < 18; i++) game_tick();
    render_frame();
    dump_ppm("render_crash.ppm");

    game_start_run();
    G.lander.x = G.pad.x + G.pad.width / 2.0f - G.lander.w / 2.0f;
    G.lander.y = G.pad.y - G.lander.h;
    G.lander.vx = G.lander.vy = G.lander.angularVelocity = 0;
    G.lander.angle = 0;
    G.lander.landed = true;
    G.score += G.pad.points + 100;
    G.state = GS_LEVEL_COMPLETE;
    render_frame();
    dump_ppm("render_landed.ppm");

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
        printf("sound-test: playing every production sound bank\n");

    sound_play(SND_MENU, 0.6f, 1.0f);
    sleep_ms(180);
    sound_play(SND_BEEP, 0.65f, 1.0f);
    sleep_ms(200);
    sound_play(SND_WARNING, 0.65f, 1.0f);
    sleep_ms(330);
    sound_loop(SND_THRUST_MAIN, true, 0.55f, 1.0f);
    sleep_ms(700);
    sound_loop(SND_THRUST_SIDE, true, 0.35f, 1.1f);
    sleep_ms(500);
    sound_loop(SND_THRUST_SIDE, false, 0, 1);
    sound_loop(SND_THRUST_MAIN, false, 0, 1);
    sound_play(SND_LANDING, 0.8f, 1.0f);
    sleep_ms(800);
    sound_play(SND_CRASH, 0.7f, 0.9f);
    sleep_ms(1000);
    sound_shutdown();
    return ok ? 0 : 0;
}

static int run_interactive(void)
{
    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "terminal-lander: needs an interactive kitty-protocol terminal\n");
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
        while (term_next_key_event(&event)) {
            if (event.action != KITTYKB_ACTION_PRESS) continue;
            if (interrupt_event(&event)) {
                G.quit = true;
                continue;
            }
            int key = game_key_from_event(&event);
            if (key < 0 || (heldInput && G.state == GS_PLAYING &&
                            gameplay_control_key(key))) continue;
            game_handle_key(key);
        }
        if (G.quit) break;
        bool up = term_key_down('w') || term_key_down(KITTYKB_KEY_UP);
        bool left = term_key_down('a') || term_key_down(KITTYKB_KEY_LEFT);
        bool right = term_key_down('d') || term_key_down(KITTYKB_KEY_RIGHT);

        /* Two simulation ticks per frame; a computer pilot decides on each
           one, exactly as it was trained. */
        for (int tick = 0; tick < 2; tick++) {
            if (!pilot_tick()) game_set_held_controls(heldInput, up, left, right);
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int ticks = argc > 3 ? atoi(argv[3]) : 3600;
        return selftest(seed, ticks);
    }
    if (argc > 1 && !strcmp(argv[1], "--input-test")) {
        return input_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--pilot-test")) return pilot_test();
    if (argc > 1 && !strcmp(argv[1], "--menu-test")) return menu_test();
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        if (argc > 3) render_dir = argv[3];   /* default: the current directory */
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test")) {
        return sound_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("terminal-lander 0.2.0\n");
        return 0;
    }
    return run_interactive();
}
