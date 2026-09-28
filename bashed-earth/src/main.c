/* Entry point: terminal setup, 30 fps render loop with 2 logic ticks per
 * frame (physics runs at 60 Hz), and a headless selftest mode that plays
 * full AI-vs-AI matches to validate the game logic. */
#include "bashed_earth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>

static void on_signal(int sig)
{
    (void)sig;
    term_emergency_restore();
    _exit(1);
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

static void set_defaults(void)
{
    memset(&G, 0, sizeof G);
    G.damageMultiplier = 1.0f;
    G.wallBounce = true;
    G.soundOn = true;
    G.pEnabled[0] = true;
    G.pEnabled[1] = true;
    G.pStrategy[1] = G.pStrategy[2] = G.pStrategy[3] = -1;
}

/* ---------- selftest: headless AI-vs-AI matches ---------- */
extern const char *g_phase;

static void watchdog(int sig)
{
    (void)sig;
    printf("WATCHDOG: stuck in phase '%s' (state=%d frame=%d)\n",
           g_phase, G.gameState, G.frameCount);
    fflush(stdout);
    _exit(2);
}

static int selftest(unsigned seed, int matches)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    G.pEnabled[1] = G.pEnabled[2] = G.pEnabled[3] = true;

    printf("bashed-earth selftest: seed=%u matches=%d field=%dx%d\n",
           seed, matches, G.W, G.H);

    game_reset_to_start();
    game_start_from_menu();   /* headless => all-AI, store auto-runs, launches */

    if (G.gameState != GS_PLAYING && G.gameState != GS_ANIMATING) {
        printf("FAIL: expected match to launch, state=%d\n", G.gameState);
        return 1;
    }

    int match = 0;
    long ticks = 0;
    const long MAX_TICKS = 60L * 60 * 30;   /* 30 minutes of game time per run */
    int shotsFired = 0, lastAmmoSum = -1;

    while (match < matches && ticks < MAX_TICKS) {
        alarm(5);          /* any single tick taking 5s = wedged */
        game_tick();
        ticks++;

        if (getenv("BE_DEBUG") && ticks % 600 == 0) {
            int nproj = 0, alive = 0;
            for (int i = 0; i < MAX_PROJECTILES; i++) nproj += G.projectiles[i].active;
            for (int i = 0; i < G.numPlayers; i++) alive += G.tanks[i].hp > 0;
            printf("  t=%ld state=%d player=%d alive=%d proj=%d hp=[%d %d %d %d] timers=%.0f/%.0f/%.0f\n",
                   ticks, G.gameState, G.currentPlayer, alive, nproj,
                   G.tanks[0].hp, G.tanks[1].hp, G.tanks[2].hp, G.tanks[3].hp,
                   G.pendingNextTurn, G.pendingAIStart, G.pendingAIFire);
        }

        /* count shots by watching total fireable ammo (cheap heuristic) */
        int ammoSum = 0;
        for (int i = 0; i < G.numPlayers; i++)
            for (int w = 0; w < WEAPON_COUNT; w++)
                if (w != W_NORMAL) ammoSum += G.ammo[i][w];
        if (lastAmmoSum >= 0 && ammoSum < lastAmmoSum) shotsFired += lastAmmoSum - ammoSum;
        lastAmmoSum = ammoSum;

        /* invariants */
        for (int i = 0; i < G.numPlayers; i++) {
            Tank *t = &G.tanks[i];
            if (isnan(t->x) || isnan(t->y)) {
                printf("FAIL: tank %d position is NaN at tick %ld\n", i, ticks);
                return 1;
            }
            if (t->hp < 0 || t->hp > MAX_HP) {
                printf("FAIL: tank %d hp out of range: %d\n", i, t->hp);
                return 1;
            }
        }

        if (G.gameState == GS_GAMEOVER) {
            match++;
            printf("  match %d done: winner=%s rounds=%d ticks=%ld terrain=%s\n",
                   match,
                   G.lastWinnerId >= 0 ? G.tanks[G.lastWinnerId].name : "(draw)",
                   G.roundCount + 1, ticks,
                   G.terrainType == TERRAIN_GRASS ? "grass"
                   : G.terrainType == TERRAIN_SAND ? "sand" : "ice");
            if (match < matches) {
                game_next_round();
                lastAmmoSum = -1;
                if (G.gameState != GS_PLAYING && G.gameState != GS_ANIMATING) {
                    printf("FAIL: next round did not launch, state=%d\n", G.gameState);
                    return 1;
                }
            }
        }
    }

    if (match < matches) {
        printf("FAIL: only %d/%d matches finished in %ld ticks (state=%d, alive=",
               match, matches, ticks, G.gameState);
        for (int i = 0; i < G.numPlayers; i++) printf("%d ", G.tanks[i].hp);
        printf(")\n");
        return 1;
    }

    printf("PASS: %d matches, %ld ticks total, ~%d special shots consumed\n",
           matches, ticks, shotsFired);
    return 0;
}

/* ---------- neural test: the gunner loads, plays, and replays exactly ---------- */
/* One headless duel to the end: seat 0 plays strategy a, seat 1 strategy b.
 * Returns the winner's seat, -1 for a draw, -2 if it never finished. */
static int play_duel(unsigned seed, int a, int b, int *turns)
{
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    G.pStrategy[0] = a;
    G.pStrategy[1] = b;
    game_reset_to_start();
    game_start_from_menu();
    int shots = 0;
    for (long t = 0; t < 60L * 60 * 30 && G.gameState != GS_GAMEOVER; t++) {
        int before = G.gameState;
        game_tick();
        shots += before == GS_PLAYING && G.gameState == GS_ANIMATING;
    }
    if (turns) *turns = shots;
    return G.gameState == GS_GAMEOVER ? G.lastWinnerId : -2;
}

#define NEURAL_TEST_DUELS 10
#define NEURAL_TEST_EXPECTED 10   /* set when the policy was installed */

static int neural_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { printf("FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("neural gunner: %s\n", neural_status());
    EXPECT(neural_ready(), "the shipped policy loads with the game's shape");

    /* the start menu offers Neural for player 1 and for every opponent */
    set_defaults();
    G.headless = true;
    game_reset_to_start();
    G.startCursor = 0;
    game_handle_key(KEY_RIGHT);
    EXPECT(G.p1Neural, "the Player 1 row switches to the neural gunner");
    G.startCursor = 1;                            /* Player 2, on Random */
    for (int i = 0; i < STRAT_COUNT; i++) game_handle_key(KEY_RIGHT);
    EXPECT(G.pEnabled[1] && G.pStrategy[1] == STRAT_NEURAL, "an opponent row reaches Neural");
    game_handle_key(KEY_RIGHT);
    EXPECT(!G.pEnabled[1], "and wraps back to Off");
    G.pEnabled[1] = true;
    G.pStrategy[1] = -1;
    game_start_from_menu();
    EXPECT(G.tanks[0].isAI && G.tanks[0].strategy == STRAT_NEURAL,
           "a neural Player 1 takes its turns itself");
    EXPECT(G.tanks[1].strategy >= 0 && G.tanks[1].strategy < STRAT_CLASSIC_COUNT,
           "Random picks among the five classic personalities");

    /* every AI, neural or classic, aims at the weakest tank, lowest seat on ties */
    {
        GameState keep = G;
        G.numPlayers = 4;
        for (int i = 0; i < 4; i++) G.tanks[i].hp = 100;
        EXPECT(ai_pick_target(3) == 0, "tied health: the target is the lowest seat, not the nearest");
        G.tanks[2].hp = 40;
        EXPECT(ai_pick_target(3) == 2, "otherwise the weakest tank");
        G.tanks[0].hp = 0;
        G.tanks[2].hp = 100;
        EXPECT(ai_pick_target(3) == 1, "and never a dead one");
        G = keep;
    }

    /* one neural turn aims from the policy and fires after the usual pause */
    for (long t = 0; t < 600 && G.gameState == GS_PLAYING; t++) game_tick();
    EXPECT(G.gameState == GS_ANIMATING || G.gameState == GS_TURN_ENDING,
           "the neural gunner fires on its turn");
    const Tank *me = &G.tanks[0];
    EXPECT(me->angle >= 5 && me->angle <= 175 && me->power >= 10 && me->power <= me->maxPower,
           "its barrel stays inside the game's limits");

    int turns1 = 0, turns2 = 0;
    int w1 = play_duel(20260927u, STRAT_NEURAL, STRAT_BALANCED, &turns1);
    int w2 = play_duel(20260927u, STRAT_NEURAL, STRAT_BALANCED, &turns2);
    EXPECT(w1 != -2, "a duel against Balanced finishes");
    EXPECT(w1 == w2 && turns1 == turns2, "the same seed replays the same duel");

    /* Regression: fixed duels against every classic personality, the gunner
     * in each seat. The shipped policy won NEURAL_TEST_EXPECTED of these when
     * it was installed; a policy or rules change that costs more than two
     * of them fails here. */
    int won = 0;
    for (int k = 0; k < NEURAL_TEST_DUELS; k++) {
        int opponent = k % STRAT_CLASSIC_COUNT, seat = (k / STRAT_CLASSIC_COUNT) % 2;
        int winner = seat == 0 ? play_duel(8000000u + (unsigned)k, STRAT_NEURAL, opponent, NULL)
                               : play_duel(8000000u + (unsigned)k, opponent, STRAT_NEURAL, NULL);
        won += winner == seat;
    }
    char label[96];
    snprintf(label, sizeof label, "the gunner wins %d of %d fixed duels (at least %d)",
             won, NEURAL_TEST_DUELS, NEURAL_TEST_EXPECTED - 2);
    EXPECT(won >= NEURAL_TEST_EXPECTED - 2, label);
#undef EXPECT
    return failures ? 1 : 0;
}

/* ---------- menu test: exits only through menus; pause freezes play ---------- */
static int menu_test(void)
{
    int failures = 0;
#define EXPECT(condition, label) do { \
    if (!(condition)) { printf("FAIL: %s\n", label); failures++; } \
    else printf("PASS: %s\n", label); \
} while (0)
    setvbuf(stdout, NULL, _IONBF, 0);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    game_reset_to_start();
    game_handle_key('q');
    game_handle_key('Q');
    EXPECT(!G.quit && G.gameState == GS_START, "Q does not quit");
    game_handle_key(KEY_ESC);
    EXPECT(!G.quit && G.startCursor == QUIT_ROW, "Esc on the start menu selects QUIT without quitting");
    game_handle_key(KEY_LEFT);
    EXPECT(!G.quit && G.gameState == GS_START, "Left/Right on QUIT changes nothing");
    game_handle_key(KEY_ENTER);
    EXPECT(G.quit, "Enter on QUIT exits");

    /* a match between a human and one AI: pause in flight, resume, menus */
    set_defaults();
    G.headless = false;                 /* a human Player 1 */
    G.W = 1000;
    G.H = 640;
    G.pStrategy[1] = STRAT_BALANCED;
    game_reset_to_start();
    G.startCursor = START_ROW;
    game_handle_key(KEY_ENTER);
    EXPECT(G.gameState == GS_STORE, "START opens the store for the human");
    game_handle_key(KEY_ESC);
    EXPECT(G.gameState == GS_PAUSED && G.pauseFrom == GS_STORE, "Esc in the store pauses");
    game_handle_key(KEY_ESC);
    EXPECT(G.gameState == GS_STORE, "Esc resumes the store");
    game_handle_key(KEY_ENTER);         /* done shopping: the AI shops and the match starts */
    G.headless = true;                  /* keep the rest of the test silent */
    EXPECT(G.gameState == GS_PLAYING && G.currentPlayer == 0 && !G.tanks[0].isAI,
           "the match starts on the human's turn");
    game_handle_key(' ');
    for (int t = 0; t < 10; t++) game_tick();
    int live = -1;
    for (int i = 0; i < MAX_PROJECTILES; i++) if (G.projectiles[i].active) live = i;
    EXPECT(G.gameState == GS_ANIMATING && live >= 0, "Space fires");
    game_handle_key('p');
    float x = live >= 0 ? G.projectiles[live].x : 0, y = live >= 0 ? G.projectiles[live].y : 0;
    int frame = G.frameCount;
    for (int t = 0; t < 120; t++) game_tick();
    EXPECT(G.gameState == GS_PAUSED && G.frameCount == frame && live >= 0 &&
           G.projectiles[live].x == x && G.projectiles[live].y == y,
           "P pauses mid-flight and nothing moves while paused");
    game_handle_key(' ');
    game_handle_key('q');
    EXPECT(G.gameState == GS_PAUSED && !G.quit, "Space and Q do nothing in the pause menu");
    game_handle_key(KEY_ENTER);
    EXPECT(G.gameState == GS_ANIMATING, "Enter on RESUME returns to the flight");
    for (int t = 0; t < 5; t++) game_tick();
    EXPECT(live >= 0 && (G.projectiles[live].x != x || G.projectiles[live].y != y || !G.projectiles[live].active),
           "and the shell flies on");
    game_handle_key(KEY_ESC);
    game_handle_key(KEY_DOWN);                    /* MAIN MENU */
    game_handle_key(KEY_ENTER);
    EXPECT(G.gameState == GS_START && !G.quit, "the pause menu's MAIN MENU returns to the start menu");

    /* game over with a human: a menu that waits; its QUIT exits */
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    G.pStrategy[0] = STRAT_BALANCED;
    G.pStrategy[1] = STRAT_TACTICAL;
    game_reset_to_start();
    game_start_from_menu();
    G.tanks[0].isAI = false;                      /* pretend Player 1 is at the keyboard */
    for (long t = 0; t < 60L * 60 * 30 && G.gameState != GS_GAMEOVER; t++) {
        if (G.gameState == GS_PLAYING && G.currentPlayer == 0) game_fire();
        game_tick();
    }
    EXPECT(G.gameState == GS_GAMEOVER && G.autoplayTimer == 0 && G.gameoverCursor == GAMEOVER_NEXT,
           "a match with a human ends on a menu with no countdown");
    for (int t = 0; t < 600; t++) game_tick();
    game_handle_key(' ');
    EXPECT(G.gameState == GS_GAMEOVER, "it waits, and Space does not choose");
    game_handle_key(KEY_UP);
    EXPECT(G.gameoverCursor == GAMEOVER_QUIT, "Up wraps to QUIT");
    game_handle_key(KEY_ENTER);
    EXPECT(G.quit, "the game-over menu's QUIT exits");

    /* nobody human: the next match starts by itself after AUTOPLAY_MS */
    for (int round = 0; round < 2; round++) {
        set_defaults();
        G.headless = true;
        G.W = 1000;
        G.H = 640;
        G.pStrategy[0] = STRAT_NEURAL;
        G.pStrategy[1] = STRAT_BALANCED;
        game_reset_to_start();
        game_start_from_menu();
        for (long t = 0; t < 60L * 60 * 30 && G.gameState != GS_GAMEOVER; t++) game_tick();
        int match = G.matchNumber;
        if (round == 0) {
            EXPECT(G.gameState == GS_GAMEOVER && G.autoplayTimer > 0,
                   "with nobody human the game-over menu counts down");
            int almost = (int)(AUTOPLAY_MS / TICK_MS) - 2;
            for (int t = 0; t < almost; t++) game_tick();
            EXPECT(G.gameState == GS_GAMEOVER, "the menu stays up during the countdown");
            for (int t = 0; t < 4; t++) game_tick();
            EXPECT(G.matchNumber == match + 1 &&
                   (G.gameState == GS_PLAYING || G.gameState == GS_ANIMATING),
                   "then the next match starts by itself");
        } else {
            game_handle_key(KEY_DOWN);
            for (int t = 0; t < (int)(AUTOPLAY_MS / TICK_MS) * 2; t++) game_tick();
            EXPECT(G.gameState == GS_GAMEOVER && G.autoplayTimer == 0 &&
                   G.gameoverCursor == GAMEOVER_MENU && G.matchNumber == match,
                   "any key stops the countdown and keeps the menu");
        }
    }
#undef EXPECT
    return failures ? 1 : 0;
}

/* Test modes keep their options file in a private directory, so no test can
 * change the player's saved setup. */
static char config_scratch[] = "/tmp/bashed-earth-test-XXXXXX";

static void remove_config_scratch(void)
{
    char path[sizeof config_scratch + 32];
    snprintf(path, sizeof path, "%s/bashed-earth.conf", config_scratch);
    unlink(path);
    rmdir(config_scratch);
}

static void isolate_config(void)
{
    if (!mkdtemp(config_scratch)) { perror("mkdtemp"); exit(1); }
    setenv("XDG_CONFIG_HOME", config_scratch, 1);
    setenv("HOME", config_scratch, 1);
    atexit(remove_config_scratch);
}

/* ---------- render test: dump framebuffer screenshots as PPM ---------- */
static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", G.W, G.H);
    const uint8_t *fb = render_fb();
    for (int i = 0; i < G.W * G.H; i++)
        fwrite(fb + i * 4, 1, 3, f);
    fclose(f);
    printf("wrote %s\n", path);
}

static int render_test(unsigned seed)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    render_init(G.W, G.H);

    game_reset_to_start();
    render_frame();
    dump_ppm("render_start.ppm");

    G.pEnabled[1] = G.pEnabled[2] = G.pEnabled[3] = true;

    /* store screen: enter as a human shopper, screenshot, then hand the
     * reins back to the AI flow */
    G.headless = false;
    game_start_from_menu();
    if (G.gameState == GS_STORE) {
        render_frame();
        dump_ppm("render_store.ppm");
    }
    G.headless = true;
    G.tanks[0].isAI = true;
    G.tanks[0].strategy = rand() % STRAT_COUNT;
    game_store_confirm();
    for (int i = 0; i < 400; i++) game_tick();
    render_frame();
    dump_ppm("render_mid.ppm");

    /* run until we catch a frame with a projectile or explosion in flight */
    for (int tries = 0; tries < 20000; tries++) {
        game_tick();
        bool action = false;
        for (int i = 0; i < MAX_PROJECTILES; i++)
            if (G.projectiles[i].active) action = true;
        if (action && G.frameCount % 7 == 0) {
            for (int j = 0; j < 12; j++) game_tick();  /* mid-flight/explosion */
            break;
        }
    }
    render_frame();
    dump_ppm("render_action.ppm");

    game_handle_key('p');                  /* the pause menu over the flight */
    render_frame();
    dump_ppm("render_paused.ppm");
    game_handle_key('p');

    for (long i = 0; i < 200000 && G.gameState != GS_GAMEOVER; i++) game_tick();
    render_frame();
    dump_ppm("render_gameover.ppm");
    render_shutdown();
    return 0;
}

/* ---------- interactive ---------- */
static int run(void)
{
    set_defaults();
    options_load();

    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "bashed-earth: needs an interactive terminal with the kitty\n");
        fprintf(stderr, "graphics protocol (kitty/kilix). Or try --selftest.\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGQUIT, on_signal);
    atexit(term_shutdown);

    srand((unsigned)time(NULL));
    G.W = w;
    G.H = h;
    render_init(w, h);
    sound_init();
    sound_set_enabled(G.soundOn);
    game_reset_to_start();

    const double FRAME_MS = 1000.0 / 30;   /* 30 fps render, 60 Hz logic */
    double next = now_ms();

    while (!G.quit) {
        int key;
        while ((key = term_poll_key()) != -1)
            game_handle_key(key);

        game_tick();
        game_tick();

        render_frame();
        term_present(render_fb(), G.W, G.H);

        next += FRAME_MS;
        double wait = next - now_ms();
        if (wait < -100) next = now_ms();   /* fell behind badly; resync */
        sleep_ms(wait);
    }

    sound_shutdown();
    render_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") && !strncmp(argv[1], "--", 2))
        isolate_config();
    if (argc > 1 && !strcmp(argv[1], "--neural-test")) return neural_test();
    if (argc > 1 && !strcmp(argv[1], "--menu-test")) return menu_test();
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int matches = argc > 3 ? atoi(argv[3]) : 3;
        return selftest(seed, matches);
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("bashed-earth 1.0.0\n");
        return 0;
    }
    return run();
}
