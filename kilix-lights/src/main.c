/* main.c - terminal lifecycle, frame loop, and headless release checks. */
#include "app.h"
#include "art.h"
#include "canvas.h"
#include "draw.h"
#include "game.h"
#include "input.h"
#include "sound.h"
#include "term.h"

#include "kitty_input.h"
#include "soft_raster.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef KILIX_LIGHTS_VERSION
#define KILIX_LIGHTS_VERSION "1.0.0"
#endif

enum { PRESENT_HZ = 30 };

static Canvas canvas;

static void on_fatal_signal(int sig)
{
    term_emergency_restore();
    _exit(128 + sig);
}

static void install_signal_handlers(void)
{
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGSEGV,
                                  SIGBUS, SIGFPE, SIGABRT};
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_fatal_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = (int)SA_RESETHAND;
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        sigaction(signals[i], &action, NULL);
}

static int64_t now_ms(void)
{
    struct timespec time_value;
    clock_gettime(CLOCK_MONOTONIC, &time_value);
    return (int64_t)time_value.tv_sec * 1000 + time_value.tv_nsec / 1000000;
}

static void sleep_ms(int64_t milliseconds)
{
    struct timespec duration;
    if (milliseconds <= 0) return;
    duration.tv_sec = (time_t)(milliseconds / 1000);
    duration.tv_nsec = (long)((milliseconds % 1000) * 1000000);
    nanosleep(&duration, NULL);
}

static bool write_frame(const char *directory, const char *name,
                        const App *app)
{
    char path[1024];
    sr_canvas output;
    int path_length;
    int bad_x = 0;
    int bad_y = 0;
    app_draw(app, &canvas);
    if (!draw_canvas_opaque(&canvas, &bad_x, &bad_y)) {
        fprintf(stderr, "render: transparent pixel at %d,%d\n", bad_x, bad_y);
        return false;
    }
    path_length = snprintf(path, sizeof path, "%s/%s.ppm", directory, name);
    if (path_length < 0 || (size_t)path_length >= sizeof path) {
        fprintf(stderr, "render: output path is too long\n");
        return false;
    }
    sr_canvas_wrap(&output, canvas.px, canvas.w, canvas.h);
    return sr_write_ppm(&output, path);
}

static int cmd_rules_test(void)
{
    if (!game_rules_selftest()) {
        fprintf(stderr, "rules-test: FAIL\n");
        return 1;
    }
    printf("rules-test: ok (3/4/5-neighbor masks, solvable generation, undo)\n");
    return 0;
}

static int cmd_input_test(void)
{
    static const uint8_t down[] = "\x1b[<0;11;6M";
    static const uint8_t up[] = "\x1b[<0;11;6m";
    static const uint8_t pixel_second[] = "\x1b[<0;2;2M";
    static const uint8_t pixel_last[] = "\x1b[<0;1280;800M";
    static const uint8_t pixel_outside[] = "\x1b[<0;1281;801M";
    kittyin_input parser;
    kittyin_event decoded;
    input_event event;

    input_reset();
    input_set_geometry(80, 24, 8, 16, 1, 1, 1, 0, 0);
    kittyin_input_init(&parser);
    kittyin_input_set_pixel_coordinates(&parser, false);
    kittyin_input_feed(&parser, down, sizeof down - 1u);
    if (!kittyin_input_next(&parser, &decoded) ||
        decoded.kind != KITTYIN_EVENT_MOUSE) {
        fprintf(stderr, "input-test: FAIL shared mouse parser\n");
        return 1;
    }
    input_push_mouse(decoded.data.mouse.x, decoded.data.mouse.y,
                     decoded.data.mouse.modifiers, decoded.data.mouse.button,
                     decoded.data.mouse.wheel_x, decoded.data.mouse.wheel_y,
                     decoded.data.mouse.action,
                     decoded.data.mouse.pixel_coordinates);
    if (!input_next(&event) || event.kind != IN_MOUSE_DOWN ||
        event.button != 0 || event.mx != 84 || event.my != 88 ||
        !event.in_view) {
        fprintf(stderr, "input-test: FAIL mouse down mapping\n");
        return 1;
    }
    kittyin_input_feed(&parser, up, sizeof up - 1u);
    if (!kittyin_input_next(&parser, &decoded) ||
        decoded.kind != KITTYIN_EVENT_MOUSE) {
        fprintf(stderr, "input-test: FAIL shared mouse release parser\n");
        return 1;
    }
    input_push_mouse(decoded.data.mouse.x, decoded.data.mouse.y,
                     decoded.data.mouse.modifiers, decoded.data.mouse.button,
                     decoded.data.mouse.wheel_x, decoded.data.mouse.wheel_y,
                     decoded.data.mouse.action,
                     decoded.data.mouse.pixel_coordinates);
    if (!input_next(&event) || event.kind != IN_MOUSE_UP ||
        event.mx != 84 || event.my != 88) {
        fprintf(stderr, "input-test: FAIL mouse release mapping\n");
        return 1;
    }
    input_push_key('n', 0u, 1);
    if (!input_next(&event) || event.kind != IN_KEY_DOWN || event.key != 'n') {
        fprintf(stderr, "input-test: FAIL keyboard queue\n");
        return 1;
    }
    if (!term_translation_test()) {
        fprintf(stderr, "input-test: FAIL shifted-key translation\n");
        return 1;
    }
    input_push_focus(false);
    if (!input_next(&event) || event.kind != IN_MOUSE_LEAVE || event.in_view) {
        fprintf(stderr, "input-test: FAIL focus-loss mouse leave\n");
        return 1;
    }

    /* kitty-input has already converted SGR's one-based reports to zero-based
     * pixels. At 2x, both physical pixels of each logical pixel must map to
     * the same canvas coordinate, including the final row and column. */
    input_reset();
    input_set_geometry(160, 48, 10, 20, 1, 1, 2, 0, 0);
    kittyin_input_init(&parser);
    kittyin_input_set_pixel_coordinates(&parser, true);
    kittyin_input_feed(&parser, pixel_second, sizeof pixel_second - 1u);
    if (!kittyin_input_next(&parser, &decoded) ||
        decoded.kind != KITTYIN_EVENT_MOUSE) {
        fprintf(stderr, "input-test: FAIL pixel parser\n");
        return 1;
    }
    input_push_mouse(decoded.data.mouse.x, decoded.data.mouse.y,
                     decoded.data.mouse.modifiers, decoded.data.mouse.button,
                     decoded.data.mouse.wheel_x, decoded.data.mouse.wheel_y,
                     decoded.data.mouse.action,
                     decoded.data.mouse.pixel_coordinates);
    if (!input_next(&event) || event.mx != 0 || event.my != 0 ||
        !event.in_view) {
        fprintf(stderr, "input-test: FAIL first scaled pixel mapping\n");
        return 1;
    }
    kittyin_input_feed(&parser, pixel_last, sizeof pixel_last - 1u);
    if (!kittyin_input_next(&parser, &decoded)) {
        fprintf(stderr, "input-test: FAIL final pixel parser\n");
        return 1;
    }
    input_push_mouse(decoded.data.mouse.x, decoded.data.mouse.y,
                     decoded.data.mouse.modifiers, decoded.data.mouse.button,
                     decoded.data.mouse.wheel_x, decoded.data.mouse.wheel_y,
                     decoded.data.mouse.action,
                     decoded.data.mouse.pixel_coordinates);
    if (!input_next(&event) || event.mx != CANVAS_W - 1 ||
        event.my != CANVAS_H - 1 || !event.in_view) {
        fprintf(stderr, "input-test: FAIL final scaled pixel mapping\n");
        return 1;
    }
    kittyin_input_feed(&parser, pixel_outside, sizeof pixel_outside - 1u);
    if (!kittyin_input_next(&parser, &decoded)) {
        fprintf(stderr, "input-test: FAIL outside pixel parser\n");
        return 1;
    }
    input_push_mouse(decoded.data.mouse.x, decoded.data.mouse.y,
                     decoded.data.mouse.modifiers, decoded.data.mouse.button,
                     decoded.data.mouse.wheel_x, decoded.data.mouse.wheel_y,
                     decoded.data.mouse.action,
                     decoded.data.mouse.pixel_coordinates);
    if (!input_next(&event) || event.in_view) {
        fprintf(stderr, "input-test: FAIL outside pixel boundary\n");
        return 1;
    }
    input_push_mouse(INT32_MAX, INT32_MAX, 0u, 1u, 0, 0, 1, false);
    if (!input_next(&event) || event.in_view || event.mx != CANVAS_W - 1 ||
        event.my != CANVAS_H - 1) {
        fprintf(stderr, "input-test: FAIL extreme cell-coordinate clamp\n");
        return 1;
    }
    printf("input-test: ok (mouse cells/pixels, focus, key translation)\n");
    return 0;
}

static int cmd_asset_test(const char *argv0)
{
    bool art_ok = art_validate(argv0, true);
    bool sound_ok = sound_validate(argv0, true);
    if (!art_ok || !sound_ok) {
        fprintf(stderr, "asset-test: FAIL\n");
        return 1;
    }
    printf("asset-test: ok (generated room, alpha switch, PCM16 cue)\n");
    return 0;
}

static int cmd_sound_test(const char *argv0)
{
    int16_t output[8192];
    bool nonzero = false;
    sound_reset_trace();
    if (!sound_init(argv0, true) || !sound_asset_loaded() ||
        !sound_mixer_running()) {
        fprintf(stderr, "sound-test: FAIL offline startup\n");
        sound_shutdown();
        return 1;
    }
    sound_play_switch(true);
    if (!sound_mix_offline(output, sizeof output / sizeof output[0])) {
        fprintf(stderr, "sound-test: FAIL offline mix\n");
        sound_shutdown();
        return 1;
    }
    for (size_t i = 0; i < sizeof output / sizeof output[0]; i++)
        if (output[i] != 0) nonzero = true;
    if (!nonzero || sound_trace_count() != 1u) {
        fprintf(stderr, "sound-test: FAIL silent output or trigger count\n");
        sound_shutdown();
        return 1;
    }
    sound_shutdown();
    printf("sound-test: ok (one accepted click, nonzero offline mix)\n");
    return 0;
}

static int cmd_interaction_test(const char *argv0)
{
    bool ok;
    if (!art_init(argv0, true)) {
        fprintf(stderr, "interaction-test: FAIL generated sprite unavailable\n");
        return 1;
    }
    ok = app_interaction_selftest();
    art_shutdown();
    if (!ok) {
        fprintf(stderr, "interaction-test: FAIL\n");
        return 1;
    }
    printf("interaction-test: ok (exact sprite hits, capture, controls, sound)\n");
    return 0;
}

static void send_hover(App *app, int x, int y)
{
    input_event event;
    memset(&event, 0, sizeof event);
    event.kind = IN_MOUSE_MOVE;
    event.mx = (int16_t)x;
    event.my = (int16_t)y;
    event.in_view = true;
    (void)app_handle(app, &event);
}

static int cmd_render_test(const char *argv0, const char *directory)
{
    App app;
    GameRect rect;
    int written = 0;
    if (!canvas_init(&canvas, CANVAS_W, CANVAS_H)) return 2;
    if (!art_init(argv0, true)) {
        canvas_free(&canvas);
        return 1;
    }
    app_init(&app);
    if (!write_frame(directory, "classic", &app)) goto fail;
    written++;
    rect = game_cell_rect(&app.game, 12);
    send_hover(&app, rect.x + rect.w / 2, rect.y + rect.h / 2);
    if (!write_frame(directory, "classic-hover", &app)) goto fail;
    written++;
    game_set_size(&app.game, 3);
    app.hover_target = -1;
    if (!write_frame(directory, "starter", &app)) goto fail;
    written++;
    game_set_size(&app.game, 7);
    app.hover_target = -1;
    if (!write_frame(directory, "expert", &app)) goto fail;
    written++;
    game_set_size(&app.game, 5);
    app.hover_target = -1;
    app.game.help = true;
    if (!write_frame(directory, "help", &app)) goto fail;
    written++;
    app.game.help = false;
    app.game.board = 0u;
    app.game.won = true;
    app.game.moves = 17;
    app.game.win_time = 1.25;
    if (!write_frame(directory, "victory", &app)) goto fail;
    written++;
    art_shutdown();
    canvas_free(&canvas);
    printf("render-test: wrote %d full-color frames to %s\n", written,
           directory);
    return 0;
fail:
    art_shutdown();
    canvas_free(&canvas);
    return 1;
}

static uint32_t parse_u32(const char *text, uint32_t fallback)
{
    char *end = NULL;
    unsigned long value;
    if (text == NULL || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX)
        return fallback;
    return (uint32_t)value;
}

static int cmd_selftest(uint32_t seed, int steps)
{
    Game game;
    uint32_t random = seed;
    uint32_t digest = 2166136261u;
    bool ended_on;
    game_init(&game);
    game.seed = seed;
    game_new(&game);
    for (int i = 0; i < steps; i++) {
        random = random * 1664525u + 1013904223u;
        if (game.won) game_new(&game);
        (void)game_activate(&game,
                            (int)(random % (uint32_t)(game.size * game.size)),
                            &ended_on);
        digest ^= (uint32_t)game.board;
        digest *= 16777619u;
        digest ^= (uint32_t)(game.board >> 32);
        digest *= 16777619u;
        digest ^= (uint32_t)game.moves;
        digest *= 16777619u;
    }
    printf("selftest: seed=%u steps=%d digest=%08x\n", seed, steps, digest);
    return 0;
}

static int run_interactive(const char *argv0)
{
    const double delta = 1.0 / PRESENT_HZ;
    const int64_t frame_ms = 1000 / PRESENT_HZ;
    App app;
    int64_t next_frame;
    bool running = true;

    if (!canvas_init(&canvas, CANVAS_W, CANVAS_H)) {
        fprintf(stderr, "kilix-lights: out of memory\n");
        return 1;
    }
    if (!art_init(argv0, true))
        fprintf(stderr, "kilix-lights: generated art unavailable; using fallback\n");
    if (term_init() != 0) {
        art_shutdown();
        canvas_free(&canvas);
        if (errno == ENOTSUP) {
            fprintf(stderr, "kilix-lights needs Kitty graphics (Kilix, kitty, "
                            "Ghostty, or WezTerm).\n");
        } else {
            fprintf(stderr, "kilix-lights: cannot start terminal: %s\n",
                    strerror(errno));
        }
        return 1;
    }
    atexit(term_shutdown);
    install_signal_handlers();
    if (!sound_init(argv0, false))
        fprintf(stderr, "kilix-lights: switch audio unavailable; continuing silently\n");
    app_init(&app);
    next_frame = now_ms();

    while (running) {
        input_event event;
        int mouse_x = 0;
        int mouse_y = 0;
        bool mouse_view = false;
        if (term_read_input() < 0) break;
        while (input_next(&event)) {
            if (app_handle(&app, &event) == APP_QUIT) {
                running = false;
                break;
            }
        }
        if (!running) break;
        app_update(&app, delta);
        (void)term_check_resize();
        app_draw(&app, &canvas);
        input_mouse_pos(&mouse_x, &mouse_y, &mouse_view);
        if (mouse_view) draw_cursor(&canvas, mouse_x, mouse_y);
        if (!term_present_canvas(&canvas)) break;
        next_frame += frame_ms;
        sleep_ms(next_frame - now_ms());
    }

    sound_shutdown();
    term_shutdown();
    art_shutdown();
    canvas_free(&canvas);
    return 0;
}

static void usage(void)
{
    printf("kilix-lights " KILIX_LIGHTS_VERSION "\n"
           "usage: kilix-lights [option]\n\n"
           "  (no option)           play\n"
           "  --rules-test          verify puzzle rules and generation\n"
           "  --input-test          verify SGR mouse/key parsing\n"
           "  --interaction-test    verify exact clicks and controls\n"
           "  --asset-test          validate graphical and audio assets\n"
           "  --sound-test          offline-mix the click cue\n"
           "  --render-test DIR     write visual review frames\n"
           "  --selftest [S] [N]    deterministic state soak\n"
           "  --version             print version\n");
}

int main(int argc, char **argv)
{
    if (argc == 1) return run_interactive(argv[0]);
    if (strcmp(argv[1], "--version") == 0) {
        printf("kilix-lights " KILIX_LIGHTS_VERSION "\n");
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "--rules-test") == 0) return cmd_rules_test();
    if (strcmp(argv[1], "--input-test") == 0) return cmd_input_test();
    if (strcmp(argv[1], "--interaction-test") == 0)
        return cmd_interaction_test(argv[0]);
    if (strcmp(argv[1], "--asset-test") == 0) return cmd_asset_test(argv[0]);
    if (strcmp(argv[1], "--sound-test") == 0) return cmd_sound_test(argv[0]);
    if (strcmp(argv[1], "--render-test") == 0)
        return cmd_render_test(argv[0], argc > 2 ? argv[2] : ".");
    if (strcmp(argv[1], "--selftest") == 0) {
        uint32_t seed = parse_u32(argc > 2 ? argv[2] : NULL, 1337u);
        uint32_t count = parse_u32(argc > 3 ? argv[3] : NULL, 500u);
        if (count > 1000000u) count = 1000000u;
        return cmd_selftest(seed, (int)count);
    }
    fprintf(stderr, "kilix-lights: unknown option '%s'\n", argv[1]);
    usage();
    return 2;
}
