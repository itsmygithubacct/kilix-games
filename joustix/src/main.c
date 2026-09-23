#include "joustix.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char asset_root[512] = "assets";

void asset_paths_init(void)
{
    const char *override = getenv("JOUSTIX_ASSETS");
    if (override && *override) {
        snprintf(asset_root, sizeof asset_root, "%s", override);
        return;
    }
    char executable[400];
    ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (n <= 0) return;
    executable[n] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash) return;
    *slash = '\0';
    char candidate[512];
    snprintf(candidate, sizeof candidate, "%s/assets", executable);
    if (access(candidate, F_OK) == 0) {
        snprintf(asset_root, sizeof asset_root, "%s", candidate);
        return;
    }
    snprintf(candidate, sizeof candidate, "%s/../share/joustix/assets", executable);
    if (access(candidate, F_OK) == 0)
        snprintf(asset_root, sizeof asset_root, "%s", candidate);
}

const char *asset_path(const char *relative_path)
{
    static char paths[4][768];
    static int index;
    index = (index + 1) & 3;
    snprintf(paths[index], sizeof paths[index], "%s/%s", asset_root, relative_path);
    return paths[index];
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void sleep_ms(double milliseconds)
{
    if (milliseconds <= 0) return;
    struct timespec ts = { (time_t)(milliseconds / 1000.0),
                           (long)(fmod(milliseconds, 1000.0) * 1000000.0) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static bool event_matches_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static int game_key_from_event(const kittykb_event *event)
{
    static const char letters[] = "admpqrsw";
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
    return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP ||
           key == 'a' || key == 'd' || key == 'w' || key == ' ';
}

static bool interrupt_event(const kittykb_event *event)
{
    return event->key == 3u ||
           (event_matches_letter(event, 'c') &&
            (event->modifiers & KITTYKB_MOD_CTRL) != 0u);
}

static void on_signal(int sig)
{
    (void)sig;
    term_emergency_restore();
    _exit(1);
}

static int run_interactive(void)
{
    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "joustix: needs an interactive Kitty-graphics terminal\n"
                        "run --selftest or --render-test for headless checks\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGABRT, on_signal);
    atexit(term_shutdown);

    game_init(w, h, (uint32_t)time(NULL));
    render_init(w, h);
    char asset_error[192];
    if (!render_validate_assets(asset_error, sizeof asset_error)) {
        render_shutdown();
        term_shutdown();
        fprintf(stderr, "joustix: %s\n", asset_error);
        return 1;
    }
    sound_init();
    sound_set_enabled(G.sound_on);

    const double frame_ms = 1000.0 / 30.0;
    double next_frame = now_ms();
    while (!G.quit) {
        kittykb_event event;
        if (term_read_input() < 0) {
            G.quit = true;
            break;
        }
        bool held_input = term_has_release_events();
        while (term_next_key_event(&event)) {
            if (event.action != KITTYKB_ACTION_PRESS) continue;
            if (interrupt_event(&event)) {
                G.quit = true;
                continue;
            }
            int key = game_key_from_event(&event);
            if (key < 0 || (held_input && G.state == GS_PLAYING &&
                            gameplay_control_key(key))) continue;
            game_handle_key(key);
        }
        if (G.quit) break;
        game_set_held_controls(
            held_input,
            term_key_down('a') || term_key_down(KITTYKB_KEY_LEFT),
            term_key_down('d') || term_key_down(KITTYKB_KEY_RIGHT),
            term_key_down('w') || term_key_down(' ') ||
                term_key_down(KITTYKB_KEY_UP));
        int nw, nh;
        if (term_check_resize(&nw, &nh) && (nw != G.W || nh != G.H)) {
            G.W = nw; G.H = nh;
            render_resize(nw, nh);
        }
        game_tick();
        game_tick();
        render_frame();
        term_present(render_fb(), G.W, G.H);
        next_frame += frame_ms;
        double wait = next_frame - now_ms();
        if (wait < -100.0) next_frame = now_ms();
        else sleep_ms(wait);
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
    game_start();
    int max_wave = G.wave, max_score = 0, restarts = 0;
    bool saw_egg = false, saw_hatch = false;
    int previous_score = 0, previous_wave = G.wave;
    for (int i = 0; i < ticks; i++) {
        int eggs_before = game_active_eggs();
        int enemies_before = game_active_enemies();
        game_autopilot();
        game_tick();
        char error[160];
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL seed=%u tick=%d: %s\n", seed, i, error);
            return 1;
        }
        int eggs_after = game_active_eggs();
        int enemies_after = game_active_enemies();
        if (eggs_after > 0) saw_egg = true;
        if (eggs_before > eggs_after && enemies_after > enemies_before) saw_hatch = true;
        if (G.score > max_score) max_score = G.score;
        if (G.wave > max_wave) max_wave = G.wave;
        if (G.wave < previous_wave || G.score < previous_score) restarts++;
        previous_wave = G.wave;
        previous_score = G.score;
    }
    if (max_score <= 0 || !saw_egg) {
        fprintf(stderr, "FAIL seed=%u: simulation did not exercise a successful joust "
                        "(score=%d egg=%d)\n", seed, max_score, saw_egg);
        return 1;
    }
    printf("PASS seed=%u ticks=%d max_wave=%d max_score=%d eggs=%s hatches=%s restarts=%d\n",
           seed, ticks, max_wave, max_score, saw_egg ? "yes" : "no",
           saw_hatch ? "yes" : "no", restarts);
    return 0;
}

static int rules_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    game_init(960, 540, 123);
    G.headless = true;
    game_start();
    EXPECT(G.wave == 1 && G.player.active && G.player.on_platform &&
           G.player.x < 42 && G.player.dir == 1 &&
           G.platforms[0].w < 40 && G.platforms[1].w < 40,
           "first wave starts player one on the compact left pad");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    G.player = (Rider){ .x = 100, .y = 50, .prev_y = 50, .dir = 1, .active = true };
    G.enemies[0] = (Enemy){ .rider = { .x = 100, .y = 56, .prev_y = 56,
                                      .dir = -1, .active = true },
                               .type = EN_BOUNDER, .flap_timer = 99 };
    G.score = G.high_score = 0;
    game_tick();
    EXPECT(!G.enemies[0].rider.active && game_active_eggs() == 1 && G.score == 500,
           "higher player defeats a rider and creates an egg");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    G.player.x = 10; G.player.y = 100; G.player.prev_y = 100;
    G.player.vx = G.player.vy = 0; G.player.active = true; G.player.invuln = 0;
    G.eggs[0] = (Egg){ .x = 210, .y = 50, .hatch_timer = .001f,
                       .type = EN_HUNTER, .active = true, .grounded = true };
    game_tick();
    EXPECT(!G.eggs[0].active && game_active_enemies() == 1 &&
           G.enemies[0].type == EN_HUNTER, "uncollected egg hatches into its rider class");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    G.player = (Rider){ .x = 100, .y = 60, .prev_y = 60, .vx = 20,
                        .dir = 1, .active = true };
    G.enemies[0] = (Enemy){ .rider = { .x = 103, .y = 60, .prev_y = 60,
                                      .vx = -20, .dir = -1, .active = true },
                               .type = EN_BOUNDER, .flap_timer = 99 };
    game_tick();
    EXPECT(G.player.active && G.enemies[0].rider.active &&
           G.player.collide_cooldown > 0 && G.enemies[0].rider.collide_cooldown > 0,
           "equal-height joust rebounds without a kill");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    G.lives = 3;
    G.player = (Rider){ .x = 100, .y = 58, .prev_y = 58, .dir = 1, .active = true };
    G.enemies[0] = (Enemy){ .rider = { .x = 100, .y = 51, .prev_y = 51,
                                      .dir = -1, .active = true },
                               .type = EN_HUNTER, .flap_timer = 99 };
    game_tick();
    game_tick();
    EXPECT(!G.player.active && G.lives == 2,
           "higher enemy costs exactly one life before respawn");
    for (int i = 0; i < 64; i++) game_tick();
    EXPECT(G.player.active && G.player.on_platform && G.player.x < 42 &&
           G.player.dir == 1,
           "player one respawns on the left pad after losing a life");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.player.active = true; G.player.invuln = 10; G.player.y = 100;
    G.state = GS_PLAYING; G.score = 0; G.high_score = 0; G.wave = 1;
    game_tick();
    int bonus = G.score;
    game_tick();
    EXPECT(G.state == GS_WAVE && bonus == 1100 && G.score == bonus,
           "empty battlefield awards one wave bonus and enters intermission");
    int intermission_lives = G.lives;
    G.player.x = 100; G.player.y = 152; G.player.invuln = 0;
    G.lava_troll_x = 109;
    G.lava_troll_phase = 1.2f;
    game_tick();
    EXPECT(G.state == GS_WAVE && G.player.active && G.lives == intermission_lives &&
           G.lava_troll_phase == 1.2f,
           "wave intermission freezes hazards while player controls are frozen");
    G.wave_timer = 0;
    game_tick();
    EXPECT(G.state == GS_PLAYING && G.wave == 2 && G.player.active &&
           G.player.on_platform && G.player.x < 42 && G.player.dir == 1 &&
           G.lava_troll_phase == 0 && G.lava_troll_timer >= 4.0f,
           "every new wave resets player one on the left pad");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    game_set_held_controls(true, false, false, false);
    G.player = (Rider){ .x = 100, .y = 80, .prev_y = 80,
                        .dir = 1, .active = true, .invuln = 10 };
    G.enemies[0] = (Enemy){
        .rider = { .x = 250, .y = 30, .prev_y = 30, .active = true,
                   .spawn_timer = 99 },
        .type = EN_BOUNDER,
    };
    game_set_held_controls(true, false, true, false);
    for (int i = 0; i < 14; i++) game_tick();
    EXPECT(G.player.x > 106.5f && G.player.vx > 50.0f,
           "held direction produces decisive forward momentum");
    float before_cancel = G.player.vx;
    game_set_held_controls(true, true, true, false);
    game_tick();
    EXPECT(G.player.vx < before_cancel,
           "simultaneous opposite directions cancel acceleration");
    game_set_held_controls(true, true, false, false);
    game_tick();
    EXPECT(G.player.dir == -1 && G.player.vx < before_cancel,
           "releasing one direction preserves the other held direction");
    G.player.vy = 0;
    G.player.flap_cooldown = 0;
    game_set_held_controls(true, false, true, true);
    game_tick();
    EXPECT(G.player.vx > 0 && G.player.vy < 0,
           "direction and flap holds apply simultaneously");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    memset(G.particles, 0, sizeof G.particles);
    G.state = GS_PLAYING;
    game_set_held_controls(false, false, false, false);
    G.left_input = G.right_input = 0;
    G.player = (Rider){ .x = 130, .y = 85.5f, .prev_y = 85.5f, .vy = 35,
                        .dir = 1, .active = true, .invuln = 10 };
    G.enemies[0] = (Enemy){
        .rider = { .x = 250, .y = 30, .prev_y = 30, .active = true,
                   .spawn_timer = 99 },
        .type = EN_BOUNDER,
    };
    game_tick();
    int landing_particles = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) landing_particles += G.particles[i].active;
    EXPECT(G.player.on_platform && landing_particles >= 5,
           "a firm landing produces visible dust feedback");

    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    G.state = GS_PLAYING;
    G.lives = 1;
    G.player = (Rider){ .x = 100, .y = 58, .prev_y = 58,
                        .dir = 1, .active = true };
    G.enemies[0] = (Enemy){
        .rider = { .x = 100, .y = 51, .prev_y = 51,
                   .dir = -1, .active = true },
        .type = EN_HUNTER, .flap_timer = 99,
    };
    game_tick();
    game_tick();
    EXPECT(G.state == GS_GAMEOVER && G.lives == 0 && !G.player.active,
           "losing the final life enters the dedicated game-over state");
    game_handle_key(' ');
    game_handle_key('r');
    EXPECT(G.state == GS_GAMEOVER,
           "game over ignores restart shortcuts and waits for Enter");
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING && G.wave == 1 && G.lives == 3,
           "Enter confirms the default ride-again option");
    G.state = GS_GAMEOVER;
    G.gameover_choice = GAMEOVER_RESTART;
    game_handle_key(KEY_DOWN);
    game_handle_key(' ');
    EXPECT(G.state == GS_GAMEOVER && G.gameover_choice == GAMEOVER_MENU,
           "direction keys select main menu without leaving game over");
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_TITLE,
           "Enter confirms main menu and returns to the title screen");

    char error[160];
    EXPECT(game_validate(error, sizeof error), "game invariants hold after rule fixtures");
    printf("rules-test: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
#undef EXPECT
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return true;
    return false;
}

static bool ensure_directories(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!ensure_directory(path)) return false;
        *p = '/';
    }
    return ensure_directory(path);
}

static bool snapshot(const char *dir, const char *name)
{
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return false;
    render_frame();
    if (!render_dump_ppm(path)) {
        fprintf(stderr, "render-test: cannot write %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static int render_test(unsigned seed)
{
    const char *requested = getenv("JOUSTIX_RENDER_DIR");
    char dir[768];
    snprintf(dir, sizeof dir, "%s", requested && *requested ? requested : ".");
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = '\0';
    char make_dir[sizeof dir];
    snprintf(make_dir, sizeof make_dir, "%s", dir);
    if (!ensure_directories(make_dir)) {
        fprintf(stderr, "render-test: cannot create %s: %s\n", dir, strerror(errno));
        return 1;
    }
    game_init(960, 540, seed);
    G.headless = true;
    render_init(G.W, G.H);
    if (!render_fb()) return 1;
    char asset_error[192];
    if (!render_validate_assets(asset_error, sizeof asset_error)) {
        fprintf(stderr, "render-test: %s\n", asset_error);
        render_shutdown();
        return 1;
    }
    if (!snapshot(dir, "render_title.ppm")) return 1;
    game_start();
    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.eggs, 0, sizeof G.eggs);
    /* Keep the player grounded on the center platform so the render fixture
     * catches regressions between the physics baseline and the bird's feet. */
    G.player = (Rider){ .x = 143, .y = 86, .prev_y = 86, .vx = 34, .dir = 1,
                        .active = true, .on_platform = true };
    for (int type = 0; type < EN_TYPE_COUNT; type++) {
        G.enemies[type] = (Enemy){
            .rider = { .x = 115 + type * 62, .y = 72 + type * 18,
                       .prev_y = 72 + type * 18, .dir = type == 1 ? -1 : 1,
                       .active = true, .flap_anim = type == 0 ? .16f :
                                                            type == 1 ? .06f : 0 },
            .type = type,
        };
    }
    G.enemies[EN_HUNTER].rider.y = 86;
    G.enemies[EN_HUNTER].rider.prev_y = 86;
    G.enemies[EN_HUNTER].rider.vx = -43;
    G.enemies[EN_HUNTER].rider.on_platform = true;
    G.eggs[0] = (Egg){ .x = 154, .y = 119, .hatch_timer = 4,
                       .active = true, .grounded = true };
    G.eggs[1] = (Egg){ .x = 218, .y = 119, .hatch_timer = 1,
                       .active = true, .grounded = true };
    G.lava_troll_x = 280;
    G.lava_troll_phase = 1.2f;
    if (!snapshot(dir, "render_game.ppm")) return 1;
    G.state = GS_WAVE; G.wave_timer = 1.5f;
    snprintf(G.message, sizeof G.message, "WAVE CLEARED"); G.message_timer = 1.5f;
    if (!snapshot(dir, "render_wave.ppm")) return 1;
    G.state = GS_GAMEOVER; G.lives = 0; G.high_score = G.score;
    if (!snapshot(dir, "render_gameover.ppm")) return 1;
    render_shutdown();
    printf("wrote %s/render_title.ppm, render_game.ppm, render_wave.ppm, render_gameover.ppm\n", dir);
    return 0;
}

static int sound_test(void)
{
    if (!sound_init()) {
        printf("sound-test: no pacat, pw-play, aplay, or play sink; silent mode is available\n");
        sound_shutdown();
        return 0;
    }
    puts("sound-test: menu, flap, step, land, joust, hurt, egg, hatch, wave, lava");
    static const int delay_ms[SFX_COUNT] = {
        250, 320, 220, 350, 520, 700, 530, 580, 1280, 780
    };
    for (int i = 0; i < SFX_COUNT; i++) {
        sound_play(i, .8f, 1);
        sleep_ms(delay_ms[i]);
    }
    sound_shutdown();
    return 0;
}

static void usage(void)
{
    puts("joustix - flying-joust arcade game for Kitty terminals\n"
         "\nusage: joustix [option]\n"
         "  (no option)                 play interactively\n"
         "  --selftest [seed] [ticks]   deterministic headless simulation\n"
         "  --rules-test                deterministic gameplay regressions\n"
         "  --render-test [seed]        write four PPM scene snapshots\n"
         "  --sound-test                play every production sound bank\n"
         "  --version                   print version\n"
         "  --help                      show this help\n"
         "\nenvironment:\n"
         "  JOUSTIX_SKIP_PROBE=1        skip Kitty graphics capability probe\n"
         "  JOUSTIX_RENDER_DIR=/path    render-test output directory\n"
         "\nkeys: arrows/A/D fly, Up/W/Space flap, P/Esc pause, M sound, Q quit");
}

int main(int argc, char **argv)
{
    asset_paths_init();
    if (argc == 1) return run_interactive();
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return 0; }
    if (!strcmp(argv[1], "--version")) { puts("joustix 0.1.0"); return 0; }
    if (!strcmp(argv[1], "--rules-test")) return rules_test();
    if (!strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int ticks = argc > 3 ? atoi(argv[3]) : 12000;
        return selftest(seed, ticks);
    }
    if (!strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (!strcmp(argv[1], "--sound-test")) return sound_test();
    fprintf(stderr, "joustix: unknown option '%s'\n", argv[1]);
    usage();
    return 2;
}
