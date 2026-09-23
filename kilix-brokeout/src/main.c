/* Entry point: terminal setup, fixed timestep, and headless checks. */
#include "kitty_brokeout.h"
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
    static const char letters[] = "acdmpqrsw";
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

static void dump_ppm(const char *path)
{
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

    for (int i = 0; i < 180; i++) {
        game_autopilot_tick();
        game_tick();
    }
    render_frame();
    dump_ppm("render_playing.ppm");

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
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
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
        game_set_held_controls(
            heldInput,
            term_key_down('a') || term_key_down(KITTYKB_KEY_LEFT),
            term_key_down('d') || term_key_down(KITTYKB_KEY_RIGHT));
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

        game_tick();
        game_tick();

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
        int ticks = argc > 3 ? atoi(argv[3]) : 7200;
        return selftest(seed, ticks);
    }
    if (argc > 1 && !strcmp(argv[1], "--input-test")) {
        return input_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test"))
        return sound_test();
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("kitty-brokeout 0.1.0\n");
        return 0;
    }
    return run_interactive();
}
