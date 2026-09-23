/* Entry point: terminal setup, fixed timestep, and headless checks. */
#include "fishtank.h"
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
    audio_emergency_stop();
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
    if (ms <= 0.0) return;
    struct timespec ts = { (time_t)(ms / 1000.0), (long)(fmod(ms, 1000.0) * 1e6) };
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

    int maxFish = 0;
    for (int i = 0; i < ticks; i++) {
        if (i % 400 == 30)
            game_add_food_at(500.0f, 40.0f, 12);
        if (i == 900 || i == 1080)
            game_handle_key('b');
        if (i == 1500 || i == 2200)
            game_handle_key('s');
        if (i == 3200 && game_count_ships() == 1)
            game_handle_key('m');
        if ((i == 3260 || i == 3380) && game_count_ships() == 1) {
            G.mouseX = 500;
            G.mouseY = G.waterY + (int)(185.0f * G.scale);
            game_handle_key(KEY_MOUSE);
        }
        game_tick();

        int fish = game_count_fish();
        if (fish > maxFish) maxFish = fish;
        if (fish < 0 || fish > MAX_FISH || game_count_food() > MAX_FOOD ||
            game_count_sharks() > MAX_SHARKS || game_count_ships() > MAX_SHIPS ||
            game_count_cannonballs() > MAX_CANNONBALLS) {
            printf("FAIL: invalid counts at tick %d fish=%d food=%d sharks=%d ships=%d shot=%d\n",
                   i, fish, game_count_food(), game_count_sharks(),
                   game_count_ships(), game_count_cannonballs());
            game_shutdown();
            return 1;
        }

        for (int f = 0; f < MAX_FISH; f++) {
            Fish *fishp = &G.fish[f];
            if (!fishp->active) continue;
            if (isnan(fishp->x) || isnan(fishp->y) ||
                fishp->x < -1.0f || fishp->x > G.W + 1.0f ||
                fishp->y < -1.0f || fishp->y > G.H + 1.0f) {
                printf("FAIL: bad fish at tick %d index=%d x=%f y=%f\n",
                       i, f, fishp->x, fishp->y);
                game_shutdown();
                return 1;
            }
        }
        for (int s = 0; s < MAX_SHIPS; s++) {
            Ship *ship = &G.ships[s];
            if (!ship->active) continue;
            if (isnan(ship->x) || isnan(ship->y) ||
                ship->x < -1.0f || ship->x > G.W + 1.0f ||
                ship->y < -1.0f || ship->y > G.H + 1.0f) {
                printf("FAIL: bad ship at tick %d index=%d x=%f y=%f\n",
                       i, s, ship->x, ship->y);
                game_shutdown();
                return 1;
            }
        }
        for (int s = 0; s < MAX_SHARKS; s++) {
            Shark *sh = &G.sharks[s];
            if (!sh->active) continue;
            float sharkOffscreen = 260.0f * G.scale;
            if (isnan(sh->x) || isnan(sh->y) || isnan(sh->vx) || isnan(sh->vy) ||
                sh->x < -sharkOffscreen || sh->x > G.W + sharkOffscreen ||
                sh->y < -1.0f || sh->y > G.H + 1.0f ||
                sh->targetFish >= MAX_FISH || sh->stuckTimer > 1.0f) {
                printf("FAIL: bad shark at tick %d index=%d x=%f y=%f vx=%f vy=%f target=%d stuck=%f sweep=%d\n",
                       i, s, sh->x, sh->y, sh->vx, sh->vy, sh->targetFish,
                       sh->stuckTimer, sh->sweepMode);
                game_shutdown();
                return 1;
            }
        }
        for (int c = 0; c < MAX_CANNONBALLS; c++) {
            Cannonball *ball = &G.cannonballs[c];
            if (!ball->active) continue;
            if (isnan(ball->x) || isnan(ball->y) ||
                ball->x < -80.0f || ball->x > G.W + 80.0f ||
                ball->y < -120.0f || ball->y > G.H + 80.0f) {
                printf("FAIL: bad cannonball at tick %d index=%d x=%f y=%f\n",
                       i, c, ball->x, ball->y);
                game_shutdown();
                return 1;
            }
        }
    }

    printf("PASS: seed=%u ticks=%d fish=%d maxFish=%d sharks=%d ships=%d shot=%d food=%d\n",
           seed, ticks, game_count_fish(), maxFish,
           game_count_sharks(), game_count_ships(), game_count_cannonballs(), game_count_food());
    game_shutdown();
    return 0;
}

static int render_test(unsigned seed)
{
    game_init(1000, 640, seed);
    G.headless = true;
    render_init(G.W, G.H);

    render_frame();
    dump_ppm("render_initial.ppm");

    game_add_food_at(500.0f, 42.0f, 24);
    for (int i = 0; i < 42; i++)
        game_tick();
    game_handle_key('s');
    if (G.sharks[0].active) {
        float s = G.scale;
        Shark *sh = &G.sharks[0];
        sh->x = 142.0f * s;
        sh->y = G.groundY - 126.0f * s;
        sh->direction = 1;
        sh->facing = 1.0f;
        sh->attackTimer = 0.82f;
        sh->chargeTimer = 0.28f;
        sh->hunting = true;
        G.sharkBites[0] = (SharkBite){
            .active = true,
            .x = sh->x + 54.0f * s,
            .y = sh->y + 4.0f * s,
            .mouthX = sh->x + 54.0f * s,
            .mouthY = sh->y + 3.0f * s,
            .life = 0.86f,
            .maxLife = 1.18f,
            .victimSize = 1.45f,
            .victimFacing = 1.0f,
            .direction = 1,
            .species = 0,
            .palette = 0,
            .sharkIndex = 0,
            .seed = 0x5eedb17eu
        };
    }
    render_frame();
    dump_ppm("render_attack.ppm");

    for (int i = 42; i < 360; i++)
        game_tick();
    render_frame();
    dump_ppm("render_fishtank.ppm");

    render_shutdown();
    game_shutdown();
    return 0;
}

static int run_interactive(const char *argv0)
{
    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "kilix-fishtank: needs an interactive kitty-protocol terminal\n");
        fprintf(stderr, "or run --selftest / --render-test.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(audio_stop);
    atexit(term_shutdown);

    game_init(w, h, (uint32_t)time(NULL));
    render_init(w, h);
    audio_start(argv0);

    const double frameMs = 1000.0 / 30.0;
    double next = now_ms();

    while (!G.quit) {
        int key;
        while ((key = term_poll_key()) != -1) {
            if (key == 'a' || key == 'A') {
                audio_toggle(argv0);
                continue;
            }
            game_handle_key(key);
        }

        game_tick();
        game_tick();

        render_frame();
        term_present(render_fb(), G.W, G.H);

        next += frameMs;
        double wait = next - now_ms();
        if (wait < -100.0) next = now_ms();
        sleep_ms(wait);
    }

    audio_stop();
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
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test")) {
        return audio_validate_assets(argv[0]) ? 0 : 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("kilix-fishtank 0.1.0\n");
        return 0;
    }
    return run_interactive(argv[0]);
}
