#include "chess_bash.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void on_signal(int sig)
{
    (void)sig;
    term_emergency_restore();
    _exit(1);
}

/* ---------- asset root resolution ----------
 * Assets are found relative to the executable (repo checkout or installed
 * under PREFIX/share/chess-bash), overridable with CHESS_BASH_ASSETS. */
static char asset_root[512] = "assets";

void asset_paths_init(void)
{
    const char *env = getenv("CHESS_BASH_ASSETS");
    if (env && *env) {
        snprintf(asset_root, sizeof asset_root, "%s", env);
        return;
    }
    char exe[400];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    char cand[512];
    snprintf(cand, sizeof cand, "%s/assets", exe);
    if (access(cand, F_OK) == 0) {
        snprintf(asset_root, sizeof asset_root, "%s", cand);
        return;
    }
    snprintf(cand, sizeof cand, "%s/../share/chess-bash/assets", exe);
    if (access(cand, F_OK) == 0)
        snprintf(asset_root, sizeof asset_root, "%s", cand);
}

const char *asset_path(const char *rel)
{
    static char bufs[4][768];
    static int bi;
    bi = (bi + 1) & 3;
    snprintf(bufs[bi], sizeof bufs[bi], "%s/%s", asset_root, rel);
    return bufs[bi];
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

static char render_output_dir[768] = ".";
static bool render_output_ready;

static bool ensure_one_directory(const char *path)
{
    if (mkdir(path, 0777) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool render_output_init(void)
{
    if (render_output_ready) return true;
    const char *env = getenv("CHESS_BASH_RENDER_DIR");
    const char *dir = env && *env ? env : ".";
    size_t len = strlen(dir);
    if (len >= sizeof render_output_dir) {
        fprintf(stderr, "render-test: CHESS_BASH_RENDER_DIR is too long\n");
        return false;
    }
    memcpy(render_output_dir, dir, len + 1);
    while (len > 1 && render_output_dir[len - 1] == '/')
        render_output_dir[--len] = '\0';

    char tmp[sizeof render_output_dir];
    memcpy(tmp, render_output_dir, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!ensure_one_directory(tmp)) {
            fprintf(stderr, "render-test: cannot create output directory %s: %s\n",
                    tmp, strerror(errno));
            return false;
        }
        *p = '/';
    }
    if (!ensure_one_directory(tmp)) {
        fprintf(stderr, "render-test: cannot create output directory %s: %s\n",
                tmp, strerror(errno));
        return false;
    }
    render_output_ready = true;
    return true;
}

static bool dump_render(const char *name)
{
    if (!render_output_init()) return false;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", render_output_dir, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "render-test: output path is too long for %s\n", name);
        return false;
    }
    if (!render_dump_ppm(path)) {
        fprintf(stderr, "render-test: failed to write %s: %s\n",
                path, strerror(errno));
        return false;
    }
    return true;
}

static bool kings_present(void)
{
    return game_king_square(SIDE_WHITE) >= 0 && game_king_square(SIDE_BLACK) >= 0;
}

static int selftest(unsigned seed, int plies)
{
    if (plies <= 0) plies = 160;
    game_init(1000, 640, seed);
    game_start(MODE_HUMAN_HUMAN);
    for (int i = 0; i < plies && G.state != GS_GAMEOVER; i++) {
        game_generate_legal();
        if (G.legal_count <= 0) break;
        int idx = (int)((seed * 33u + (unsigned)i * 17u) % (unsigned)G.legal_count);
        char uci[8];
        game_move_to_uci(G.legal[idx], uci);
        if (!game_apply_uci_move(uci)) {
            printf("FAIL: rejected legal move %s at ply %d\n", uci, i);
            game_shutdown();
            return 1;
        }
        int guard = 0;
        while (G.state == GS_ANIMATING && guard++ < 400)
            game_tick();
        if (G.state == GS_ANIMATING) {
            printf("FAIL: animation for %s never finished\n", uci);
            game_shutdown();
            return 1;
        }
        if (!kings_present()) {
            printf("FAIL: missing king after %s at ply %d\n", uci, i);
            game_shutdown();
            return 1;
        }
    }
    printf("PASS: seed=%u plies=%d final_state=%d history_len=%zu legal=%d\n",
           seed, plies, G.state, strlen(G.uci_history), G.legal_count);
    game_shutdown();
    return 0;
}

static void anim_run_until(float t)
{
    int guard = 0;
    while (G.state == GS_ANIMATING && G.anim.t < t && guard++ < 500)
        game_tick();
}

static void anim_run_out(void)
{
    int guard = 0;
    while (G.state == GS_ANIMATING && guard++ < 500)
        game_tick();
}

static bool test_apply_moves(const char *moves)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", moves);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if (!game_apply_uci_move(tok)) return false;
        anim_run_out();
    }
    return true;
}

static int perft_test(void)
{
    static const struct {
        const char *name;
        const char *fen;
        int depth;
        uint64_t expected;
    } cases[] = {
        {
            "start position",
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            4, 197281,
        },
        {
            "Kiwipete",
            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
            3, 97862,
        },
    };

    int failures = 0;
    game_init(1000, 640, 123);
    game_start(MODE_HUMAN_HUMAN);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (!game_load_fen(cases[i].fen)) {
            printf("FAIL: perft %s FEN rejected\n", cases[i].name);
            failures++;
            continue;
        }
        uint64_t nodes = game_perft(cases[i].depth);
        if (nodes != cases[i].expected) {
            printf("FAIL: perft %s depth %d = %llu, expected %llu\n",
                   cases[i].name, cases[i].depth,
                   (unsigned long long)nodes,
                   (unsigned long long)cases[i].expected);
            failures++;
        } else {
            printf("PASS: perft %s depth %d = %llu\n",
                   cases[i].name, cases[i].depth,
                   (unsigned long long)nodes);
        }
    }
    game_shutdown();
    if (failures)
        printf("perft-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

static int perft_run(int depth, const char *fen)
{
    if (depth < 0 || depth > 8) {
        fprintf(stderr, "perft: depth must be between 0 and 8\n");
        return 2;
    }
    game_init(1000, 640, 1);
    game_start(MODE_HUMAN_HUMAN);
    if (fen && !game_load_fen(fen)) {
        fprintf(stderr, "perft: invalid FEN\n");
        game_shutdown();
        return 2;
    }
    uint64_t nodes = game_perft(depth);
    printf("perft depth=%d nodes=%llu\n", depth, (unsigned long long)nodes);
    game_shutdown();
    return 0;
}

static int rules_test(void)
{
    int failures = 0;
    game_init(1000, 640, 123);
    game_start(MODE_HUMAN_HUMAN);

#define EXPECT(cond, label) do { \
    if (!(cond)) { printf("FAIL: %s\n", label); failures++; } \
} while (0)

    EXPECT(G.engines[0].in_fd == -1 && G.engines[0].out_fd == -1 &&
           G.engines[1].in_fd == -1 && G.engines[1].out_fd == -1,
           "unused engine descriptors are safely initialized");
    EXPECT(!game_apply_uci_move("z9z1"), "malformed UCI is rejected");
    EXPECT(!game_apply_uci_move("e2e4queen"), "overlong UCI is rejected");
    EXPECT(!game_apply_uci_move("e2e4q"),
           "promotion suffix on an ordinary move is rejected");

    const char *promotion_run =
        "a2a4 h7h5 a4a5 h5h4 a5a6 h4h3 a6b7 h3g2";
    EXPECT(test_apply_moves(promotion_run), "promotion setup is legal");
    EXPECT(!game_apply_uci_move("b7a8"), "UCI promotion requires a suffix");
    EXPECT(game_apply_uci_move("b7a8n"), "suffixed UCI promotion is accepted");
    anim_run_out();

    /* Castling rights alone are not enough when a rook is absent. */
    memset(G.board, '.', sizeof G.board);
    G.board[make_sq(4, 0)] = 'K';
    G.board[make_sq(4, 7)] = 'k';
    G.side = SIDE_WHITE;
    G.castle_wk = G.castle_wq = true;
    G.castle_bk = G.castle_bq = false;
    G.ep_square = -1;
    game_generate_legal();
    bool found_castle = false;
    for (int i = 0; i < G.legal_count; i++)
        if (G.legal[i].castle) found_castle = true;
    EXPECT(!found_castle, "castling requires the rook");

    G.board[make_sq(4, 7)] = '.';
    G.board[make_sq(4, 1)] = 'k';
    game_generate_legal();
    bool captures_king = false;
    for (int i = 0; i < G.legal_count; i++)
        if (G.legal[i].to == make_sq(4, 1)) captures_king = true;
    EXPECT(!captures_king, "a king is checked, never captured");

    /* A pinned pawn has no en-passant right for repetition identity. */
    game_start(MODE_HUMAN_HUMAN);
    const char *pinned_ep_repetition =
        "d2d4 e7e5 e2e3 e5d4 g1f3 g8f6 e3e4 g7g6 "
        "e4e5 f8g7 b2b3 e8g8 c2c3 f8e8 a2a3 d7d5 "
        "f3g1 f6h5 g1f3 h5f6 f3g1 f6h5 g1f3 h5f6";
    EXPECT(test_apply_moves(pinned_ep_repetition), "pinned-EP sequence is legal");
    EXPECT(G.state == GS_GAMEOVER && G.result_kind == RESULT_REPETITION,
           "pinned en-passant position repeats three times");

    /* Check feedback must clear as soon as the checked side answers it. */
    game_start(MODE_HUMAN_HUMAN);
    EXPECT(test_apply_moves("e2e4 d7d5 f1b5 c7c6"), "check-answer sequence is legal");
    EXPECT(G.check_flash <= 0 && G.check_sq < 0 && !game_side_in_check(G.side),
           "answered check clears its warning");

    /* A reversible 100th halfmove is adjudicated as a fifty-move draw. */
    bool loaded = game_load_fen("8/8/8/8/8/8/6R1/K6k w - - 99 50");
    EXPECT(loaded, "fifty-move fixture FEN loads");
    if (loaded) {
        EXPECT(game_apply_uci_move("g2g3"), "fifty-move fixture move is legal");
        anim_run_out();
        EXPECT(G.state == GS_GAMEOVER && G.result_kind == RESULT_FIFTY_MOVE,
               "100th quiet halfmove triggers fifty-move draw");
    }

    /* Qg6 leaves the cornered king with no legal square but not in check. */
    loaded = game_load_fen("7k/5K2/8/5Q2/8/8/8/8 w - - 0 1");
    EXPECT(loaded, "stalemate fixture FEN loads");
    if (loaded) {
        EXPECT(game_apply_uci_move("f5g6"), "stalemate fixture move is legal");
        anim_run_out();
        EXPECT(G.state == GS_GAMEOVER && G.result_kind == RESULT_STALEMATE,
               "move with no legal reply and no check is stalemate");
    }

    loaded = game_load_fen("7k/8/8/8/8/8/6B1/K7 w - - 0 1");
    EXPECT(loaded, "insufficient-material fixture FEN loads");
    if (loaded) {
        EXPECT(game_apply_uci_move("g2f3"),
               "insufficient-material fixture move is legal");
        anim_run_out();
        EXPECT(G.state == GS_GAMEOVER && G.result_kind == RESULT_MATERIAL,
               "king and bishop versus king is insufficient material");
    }

    loaded = game_load_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    EXPECT(loaded, "en-passant fixture FEN loads");
    if (loaded) {
        EXPECT(game_apply_uci_move("e5d6"), "en-passant capture is legal");
        anim_run_out();
        EXPECT(G.board[make_sq(3, 5)] == 'P' &&
               G.board[make_sq(3, 4)] == '.' &&
               G.captured_count[SIDE_WHITE] == 1 &&
               G.captured[SIDE_WHITE][0] == 'p',
               "en-passant moves the pawn and removes the bypassed pawn");
    }

    /* The black rook attacks f1 only: White may castle long, not through f1. */
    loaded = game_load_fen("r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1");
    EXPECT(loaded, "castling-through-check fixture FEN loads");
    if (loaded) {
        EXPECT(!game_apply_uci_move("e1g1"),
               "castling through an attacked transit square is rejected");
        EXPECT(game_apply_uci_move("e1c1"),
               "castling on the unattacked side remains legal");
        anim_run_out();
        EXPECT(G.board[make_sq(2, 0)] == 'K' &&
               G.board[make_sq(3, 0)] == 'R',
               "legal castling moves both king and rook");
    }

    /* Blue-side controls follow the rotated view, not absolute coordinates. */
    G.human_white = false;
    game_start(MODE_HUMAN_AI);
    EXPECT(test_apply_moves("e2e4"), "computer opening move is legal");
    int cursor_before = G.cursor;
    game_handle_key(KEY_LEFT);
    EXPECT(G.cursor == make_sq(sq_file(cursor_before) + 1, sq_rank(cursor_before)),
           "Blue-side cursor follows the rotated board");

    game_handle_key('n');
    EXPECT(G.state == GS_PLAYING && G.leave_arm > 0,
           "first N asks before abandoning a game");
    game_handle_key(KEY_RIGHT);
    EXPECT(G.state == GS_PLAYING && G.leave_arm <= 0,
           "another action cancels leave confirmation");
    game_handle_key('n');
    game_handle_key('n');
    EXPECT(G.state == GS_TITLE, "second N confirms leaving the game");

    G.human_white = true;
    game_shutdown();
#undef EXPECT
    if (failures) {
        printf("rules-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("PASS: rules, adjudication, feedback, confirmation, and Blue-side view\n");
    return 0;
}

static int render_test(unsigned seed)
{
    bool ok = false;
    char asset_error[256];
    game_init(1000, 640, seed);
    render_init(G.W, G.H);
    if (!render_output_init()) goto done;
    if (!render_validate_assets(asset_error, sizeof asset_error)) {
        fprintf(stderr, "render-test: %s\n", asset_error);
        goto done;
    }

#define DUMP_RENDER(name) do { if (!dump_render(name)) goto done; } while (0)
#define APPLY_RENDER_MOVE(uci) do { \
    if (!game_apply_uci_move(uci)) { \
        fprintf(stderr, "render-test: expected legal move %s was rejected\n", uci); \
        goto done; \
    } \
} while (0)

    G.state = GS_INTRO;
    G.intro_t = 3.1f;
    render_frame();
    DUMP_RENDER("render_intro.ppm");

    game_reset_to_title();
    G.title_t = 1.4f;
    render_frame();
    DUMP_RENDER("render_title.ppm");

    game_start(MODE_HUMAN_HUMAN);
    render_frame();
    DUMP_RENDER("render_board.ppm");

    /* Every theme must load and frame the centered software board cleanly. */
    for (int theme = 0; theme < BOARD_THEME_COUNT; theme++) {
        G.board_theme = theme;
        render_set_theme(theme);
        render_frame();
        char path[64];
        snprintf(path, sizeof path, "render_theme_%d.ppm", theme);
        DUMP_RENDER(path);
    }
    G.board_theme = 0;
    render_set_theme(0);

    game_handle_key(KEY_ENTER);   /* e2 selected: distinct move/capture cues */
    render_frame();
    DUMP_RENDER("render_selected.ppm");

    G.W = 640;
    G.H = 414;                    /* typical 80x24 terminal geometry */
    render_resize(G.W, G.H);
    game_handle_key('n');         /* confirmation must remain visible compact */
    render_frame();
    DUMP_RENDER("render_compact.ppm");
    game_handle_key(KEY_ESC);
    G.W = 1000;
    G.H = 640;
    render_resize(G.W, G.H);

    APPLY_RENDER_MOVE("e2e4");
    anim_run_until(0.55f);
    render_frame();
    DUMP_RENDER("render_move.ppm");
    anim_run_out();

    APPLY_RENDER_MOVE("d7d5");
    anim_run_out();
    G.cursor = make_sq(4, 3);
    game_handle_key(KEY_ENTER);
    render_frame();
    DUMP_RENDER("render_capture_target.ppm");
    game_handle_key(KEY_ESC);
    APPLY_RENDER_MOVE("e4d5");
    anim_run_until(0.40f);
    render_frame();
    DUMP_RENDER("render_battle_windup.ppm");
    anim_run_until(0.52f);
    render_frame();
    DUMP_RENDER("render_battle_strike.ppm");
    anim_run_until(0.70f);
    render_frame();
    DUMP_RENDER("render_battle_fall.ppm");
    anim_run_out();

    /* castling: king and rook both animate */
    APPLY_RENDER_MOVE("g8f6");
    anim_run_out();
    APPLY_RENDER_MOVE("g1f3");
    anim_run_out();
    APPLY_RENDER_MOVE("g7g6");
    anim_run_out();
    APPLY_RENDER_MOVE("f1e2");
    anim_run_out();
    APPLY_RENDER_MOVE("f8g7");
    anim_run_out();
    APPLY_RENDER_MOVE("e1g1");
    anim_run_until(0.62f);
    render_frame();
    DUMP_RENDER("render_castle.ppm");
    anim_run_out();

    /* promotion chooser on a hand-built position */
    memset(G.board, '.', sizeof G.board);
    G.board[make_sq(4, 0)] = 'K';
    G.board[make_sq(4, 7)] = 'k';
    G.board[make_sq(0, 6)] = 'P';
    G.castle_wk = G.castle_wq = G.castle_bk = G.castle_bq = false;
    G.ep_square = -1;
    G.side = SIDE_WHITE;
    G.state = GS_PLAYING;
    G.anim.active = false;
    game_generate_legal();
    if (!game_apply_human_target(make_sq(0, 6), make_sq(0, 7))) {
        fprintf(stderr, "render-test: promotion chooser move was rejected\n");
        goto done;
    }
    render_frame();
    DUMP_RENDER("render_promo.ppm");
    game_handle_key(KEY_ENTER);
    anim_run_until(0.95f);
    render_frame();
    DUMP_RENDER("render_promo_sparkle.ppm");
    anim_run_out();

    /* a Blue human gets a rotated board with their army nearest them */
    G.human_white = false;
    game_start(MODE_HUMAN_AI);
    APPLY_RENDER_MOVE("e2e4");
    anim_run_out();
    render_frame();
    DUMP_RENDER("render_blue_side.ppm");
    G.human_white = true;

    /* fool's mate for the finale */
    game_start(MODE_HUMAN_HUMAN);
    const char *fools[] = { "f2f3", "e7e5", "g2g4", "d8h4" };
    for (int i = 0; i < 4; i++) {
        APPLY_RENDER_MOVE(fools[i]);
        anim_run_out();
    }
    if (G.state != GS_GAMEOVER || G.result_kind != RESULT_CHECKMATE) {
        fprintf(stderr, "render-test: Fool's Mate did not reach checkmate\n");
        goto done;
    }
    for (int i = 0; i < 70; i++)
        game_tick();
    render_frame();
    DUMP_RENDER("render_gameover.ppm");

    ok = true;

done:
    render_shutdown();
    game_shutdown();
    if (ok)
        printf("wrote 21 render-test PPMs to %s\n", render_output_dir);
#undef APPLY_RENDER_MOVE
#undef DUMP_RENDER
    return ok ? 0 : 1;
}

static int sound_test(void)
{
    bool ok = sound_init();
    if (!ok) {
        printf("sound-test: no supported audio sink found; game will run silent\n");
        sound_shutdown();
        return 0;
    }
    printf("sound-test: playing select, move, capture, fall, start trumpet, win trumpet, check\n");
    sound_play(SND_SELECT, 0.7f, 1.0f);
    sleep_ms(220);
    sound_play(SND_MOVE, 0.8f, 1.0f);
    sleep_ms(380);
    sound_play(SND_CAPTURE, 0.8f, 1.0f);
    sleep_ms(600);
    sound_play(SND_FALL, 0.8f, 1.0f);
    sleep_ms(900);
    sound_play(SND_START_TRUMPET, 0.8f, 1.0f);
    sleep_ms(1400);
    sound_play(SND_WIN_TRUMPET, 0.8f, 1.0f);
    sleep_ms(2600);
    sound_play(SND_CHECK, 0.8f, 1.0f);
    sleep_ms(600);
    sound_shutdown();
    return 0;
}

static int run_interactive(void)
{
    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "chess-bash: needs an interactive kitty-protocol terminal\n");
        fprintf(stderr, "or run --selftest / --render-test.\n");
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
    sound_init();
    if (G.music_on)
        sound_music_play(MUS_BATTLE, 0.38f, true);
    sound_play(SND_START_TRUMPET, 0.7f, 1.0f);

    const double frame_ms = 1000.0 / 30.0;
    double next = now_ms();
    while (!G.quit) {
        int key;
        while ((key = term_poll_key()) != -1)
            game_handle_key(key);

        int nw, nh;
        if (term_check_resize(&nw, &nh) && (nw != G.W || nh != G.H)) {
            G.W = nw;
            G.H = nh;
            render_resize(nw, nh);
        }

        game_tick();
        game_tick();
        render_frame();
        term_present(render_fb(), G.W, G.H);

        next += frame_ms;
        double wait = next - now_ms();
        if (wait < -100) next = now_ms();
        sleep_ms(wait);
    }

    render_shutdown();
    sound_shutdown();
    game_shutdown();
    return 0;
}

static void print_usage(void)
{
    printf("chess-bash - animated Battle-Chess-style chess for kitty terminals\n\n"
           "usage: chess-bash [option]\n"
           "  (no option)          play (needs a kitty-graphics terminal)\n"
           "  --selftest [seed] [plies]   headless rules exercise\n"
           "  --rules-test                run deterministic rule/UX regressions\n"
           "  --perft-test                run standard move-generator perft cases\n"
           "  --perft depth [fen]         count legal move-tree nodes (quote FEN)\n"
           "  --fallback-test             smoke-test the built-in AI without an engine\n"
           "  --render-test [seed]        dump screenshots of every game state\n"
           "  --sound-test                play every sound cue and exit\n"
           "  --version                   print version\n"
           "  --help                      this text\n\n"
           "environment:\n"
           "  STOCKFISH=/path/to/engine   UCI engine for the AI modes\n"
           "  CHESS_BASH_ASSETS=/dir      override the asset directory\n"
           "  CHESS_BASH_RENDER_DIR=/dir  render/battle-test output directory\n"
           "  CHESS_BASH_SKIP_PROBE=1     skip the kitty-graphics probe\n\n"
           "keys: arrows/WASD move - ENTER select - R resign - M music -\n"
           "      F fast battles - S skip battle - N twice to menu - Q quit\n");
}

/* apply a space-separated UCI move list headlessly and report the outcome */
static int script_test(const char *moves)
{
    game_init(1000, 640, 1);
    game_start(MODE_HUMAN_HUMAN);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", moves ? moves : "");
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if (G.state == GS_GAMEOVER) break;
        if (!game_apply_uci_move(tok)) {
            printf("FAIL: rejected %s\n", tok);
            game_shutdown();
            return 1;
        }
        int guard = 0;
        while (G.state == GS_ANIMATING && guard++ < 500)
            game_tick();
    }
    printf("state=%d result_kind=%d result=\"%s\" halfmove=%d\n",
           G.state, G.result_kind, G.result, G.halfmove_clock);
    game_shutdown();
    return 0;
}

/* apply a move list, then snapshot the final move's animation at several
 * points — the tuning loop for battle choreography */
static int battle_test(const char *moves)
{
    bool ok = false;
    char asset_error[256];
    game_init(1000, 640, 1);
    render_init(G.W, G.H);
    if (!render_output_init()) goto done;
    if (!render_validate_assets(asset_error, sizeof asset_error)) {
        fprintf(stderr, "battle-test: %s\n", asset_error);
        goto done;
    }
    game_start(MODE_HUMAN_HUMAN);
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", moves ? moves : "");
    char *toks[128];
    int n = 0;
    for (char *tok = strtok(buf, " "); tok && n < 128; tok = strtok(NULL, " "))
        toks[n++] = tok;
    for (int i = 0; i < n; i++) {
        if (!game_apply_uci_move(toks[i])) {
            printf("FAIL: rejected %s\n", toks[i]);
            goto done;
        }
        if (i < n - 1)
            anim_run_out();
    }
    const float snaps[] = { 0.15f, 0.30f, 0.42f, 0.52f, 0.66f, 0.90f };
    for (int i = 0; i < 6; i++) {
        anim_run_until(snaps[i]);
        render_frame();
        char path[64];
        snprintf(path, sizeof path, "battle_%02d.ppm", i);
        if (!dump_render(path)) goto done;
    }
    anim_run_out();
    ok = true;

done:
    render_shutdown();
    game_shutdown();
    if (ok)
        printf("wrote %s/battle_00..05.ppm\n", render_output_dir);
    return ok ? 0 : 1;
}

/* run AI vs AI headlessly against $STOCKFISH (or the fallback AI) */
static int ai_test(int max_ticks, int difficulty)
{
    if (max_ticks <= 0) max_ticks = 36000;
    game_init(1000, 640, 99);
    if (difficulty >= 0 && difficulty < DIFF_COUNT)
        G.difficulty = difficulty;
    game_start(MODE_AI_AI);
    int ticks = 0;
    while (G.state != GS_GAMEOVER && ticks++ < max_ticks) {
        game_tick();
        sleep_ms(1);   /* keep the poll loop from outrunning the engine */
    }
    engine_cancel_all();   /* stabilize engine state/name before reporting */
    G.ai_thinking = false;
    printf("%s: ticks=%d plies=%d state=%d result=\"%s\" engine0=%s engine1=%s\n",
           G.state == GS_GAMEOVER ? "DONE" : "TIMEOUT",
           ticks, G.ply_count, G.state, G.result,
           G.engines[0].ok ? G.engines[0].name : "fallback",
           G.engines[1].ok ? G.engines[1].name : "fallback");
    bool completed = G.state == GS_GAMEOVER;
    game_shutdown();
    return completed ? 0 : 1;
}

static int fallback_test(void)
{
    enum { TARGET_PLIES = 6, MAX_TICKS = 4000 };
    if (setenv("STOCKFISH", "/definitely/missing/chess-bash-stockfish", 1) != 0) {
        fprintf(stderr, "fallback-test: cannot set STOCKFISH: %s\n", strerror(errno));
        return 1;
    }

    game_init(1000, 640, 0x51a7u);
    G.difficulty = 0;
    G.fast_anim = true;
    game_start(MODE_AI_AI);
    int ticks = 0;
    while (G.ply_count < TARGET_PLIES && G.state != GS_GAMEOVER &&
           ticks++ < MAX_TICKS) {
        game_tick();
        if (G.ai_thinking)
            sleep_ms(1);   /* let the async missing-engine probe complete */
    }

    bool engines_inactive = !G.ai_thinking;
    for (int i = 0; i < 2; i++) {
        engines_inactive = engines_inactive && G.engine_attempted[i] &&
                           !G.engines[i].ok && G.engines[i].pid <= 0 &&
                           G.engines[i].in_fd == -1 && G.engines[i].out_fd == -1;
    }
    bool played = G.ply_count >= TARGET_PLIES && kings_present() &&
                  G.uci_history[0] != '\0';
    if (!played || !engines_inactive) {
        fprintf(stderr,
                "FAIL: fallback AI plies=%d ticks=%d state=%d thinking=%d "
                "engine0=%d engine1=%d\n",
                G.ply_count, ticks, G.state, G.ai_thinking,
                G.engines[0].ok, G.engines[1].ok);
        game_shutdown();
        return 1;
    }

    game_generate_legal();
    printf("PASS: built-in fallback AI completed %d legal plies with no external engine\n",
           G.ply_count);
    game_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    asset_paths_init();
    if (argc > 2 && !strcmp(argv[1], "--script")) {
        return script_test(argv[2]);
    }
    if (argc > 1 && !strcmp(argv[1], "--ai-test")) {
        return ai_test(argc > 2 ? atoi(argv[2]) : 0,
                       argc > 3 ? atoi(argv[3]) : -1);
    }
    if (argc > 1 && !strcmp(argv[1], "--fallback-test")) {
        return fallback_test();
    }
    if (argc > 2 && !strcmp(argv[1], "--battle-test")) {
        return battle_test(argv[2]);
    }
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        print_usage();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int plies = argc > 3 ? atoi(argv[3]) : 160;
        return selftest(seed, plies);
    }
    if (argc > 1 && !strcmp(argv[1], "--rules-test")) {
        return rules_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--perft-test")) {
        return perft_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--perft")) {
        if (argc < 3) {
            fprintf(stderr, "chess-bash: --perft requires a depth\n");
            return 2;
        }
        char *end;
        long depth = strtol(argv[2], &end, 10);
        if (*argv[2] == '\0' || *end || depth < 0 || depth > 8) {
            fprintf(stderr, "chess-bash: invalid perft depth '%s'\n", argv[2]);
            return 2;
        }
        return perft_run((int)depth, argc > 3 ? argv[3] : NULL);
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test")) {
        return sound_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("chess-bash 0.2.0\n");
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "chess-bash: unknown option '%s'\n", argv[1]);
        print_usage();
        return 2;
    }
    return run_interactive();
}
