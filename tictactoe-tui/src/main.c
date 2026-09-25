/* Entry point: the interactive loop, remembered settings, and headless tests. */
#include "ttt.h"
#include "kilix_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define VERSION "0.1.0"

static long long now_ms(void)
{
    struct timespec t;
    (void)clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---------- remembered settings ----------
 *
 * The main-menu choice is kept in a kilix-state record under
 * $XDG_DATA_HOME/tictactoe-tui/, written only by the interactive game.
 * Payload v1: version, X player, O player, level, first move; one byte each.
 */
#define SETTINGS_VERSION 1u

static bool settings_open(kilixstate_store *store)
{
    kilixstate_options options;
    kilixstate_options_init(&options);
    options.app_id = "tictactoe-tui";
    options.filename = "settings.state";
    options.max_payload = 64u;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static void settings_load(Game *g)
{
    kilixstate_store store;
    if (!settings_open(&store)) return;
    uint8_t p[5];
    size_t size = 0;
    if (kilixstate_load(&store, p, sizeof p, &size) == KILIXSTATE_OK && size == sizeof p &&
        p[0] == SETTINGS_VERSION && p[1] < PLAYER_COUNT && p[2] < PLAYER_COUNT &&
        p[3] < LEVEL_COUNT && p[4] < STARTS_COUNT) {
        g->players[0] = p[1];
        g->players[1] = p[2];
        g->level = p[3];
        g->starts = p[4];
    }
    kilixstate_store_close(&store);
}

static void settings_save(const Game *g)
{
    kilixstate_store store;
    if (!settings_open(&store)) return;
    uint8_t p[5] = { SETTINGS_VERSION, (uint8_t)g->players[0], (uint8_t)g->players[1],
                     (uint8_t)g->level, (uint8_t)g->starts };
    kilixstate_result result = kilixstate_save(&store, p, sizeof p);
    if (result != KILIXSTATE_OK)
        fprintf(stderr, "tictactoe-tui: settings not saved: %s\n",
                kilixstate_result_name(result));
    kilixstate_store_close(&store);
}

/* ---------- interactive ---------- */

static int cli[4] = { -1, -1, -1, -1 };   /* X, O, level, first move */

/* A mouse press is turned into what it hit: a board cell (mouse_x) during
   play, or a menu row (mouse_y) on a menu. */
static void resolve_mouse(const Game *g, Input *in, const Screen *s)
{
    /* Hit-test against the layout actually drawn: render() clamps the screen
       to SCREEN_MAX_W x SCREEN_MAX_H, so the terminal's own size is wrong here. */
    int x = in->mouse_x, y = in->mouse_y, w = s->w, h = s->h;
    if (w < MIN_W || h < MIN_H) {            /* only the "enlarge" message is drawn */
        in->key = KEY_NONE;
        return;
    }
    if (g->screen == SCREEN_PLAY) {
        in->mouse_x = render_hit_cell(w, h, x, y);
        in->mouse_y = -1;
        if (in->mouse_x < 0) in->key = KEY_NONE;
    } else {
        in->mouse_y = render_hit_row(g, w, h, x, y);
        in->mouse_x = -1;
        if (in->mouse_y < 0) in->key = KEY_NONE;
    }
}

static int run_interactive(void)
{
    static Game game;
    static Screen screen;
    game_init(&game, (uint32_t)time(NULL) ^ (uint32_t)now_ms());
    settings_load(&game);
    if (cli[0] >= 0) game.players[0] = cli[0];
    if (cli[1] >= 0) game.players[1] = cli[1];
    if (cli[2] >= 0) game.level = cli[2];
    if (cli[3] >= 0) game.starts = cli[3];
    if (!term_init()) {
        fprintf(stderr, "tictactoe-tui: needs an interactive terminal (see --help)\n");
        return 1;
    }
    long long last = now_ms();
    while (!game.quit) {
        int w, h;
        term_size(&w, &h);
        render(&game, &screen, w, h);
        term_draw(&screen);
        Input in;
        int timeout = game_is_computer_turn(&game) ? 30 : 250;
        while (term_read(&in, timeout)) {
            timeout = 0;
            if (in.key == KEY_INTERRUPT) {
                game.quit = true;
                break;
            }
            if (in.key == KEY_RESIZE) continue;
            if (in.key == KEY_MOUSE) resolve_mouse(&game, &in, &screen);
            game_input(&game, &in);
            if (game.quit) break;
        }
        long long t = now_ms();
        game_update(&game, (int)(t - last));
        last = t;
    }
    term_shutdown();
    settings_save(&game);
    return 0;
}

/* ---------- headless tests ---------- */

static int failures;
#define EXPECT(cond, label) do { \
    if (cond) printf("PASS: %s\n", label); \
    else { fprintf(stderr, "FAIL: %s\n", label); failures++; } \
} while (0)

static void key(Game *g, int k)
{
    Input in = { k, -1, -1 };
    game_input(g, &in);
}

/* A Board whose side to move owns `own`: X holds own, and X is to move
   because it moved first on an even move count and second on an odd one. */
static Board mover_board(uint16_t own, uint16_t opp)
{
    int made = __builtin_popcount(own) + __builtin_popcount(opp);
    return (Board){ own, opp, made % 2 ? MARK_O : MARK_X };
}

/* Every position reachable from an empty board, as (own, opp) of the mover. */
static int count_reachable(uint16_t own, uint16_t opp, uint8_t *seen, int *nonterminal)
{
    int index = 0;
    for (int cell = 8; cell >= 0; cell--)
        index = index * 3 + (own >> cell & 1u ? 1 : (opp >> cell & 1u ? 2 : 0));
    if (seen[index]) return 0;
    seen[index] = 1;
    Board b = mover_board(own, opp);
    int total = 1;
    if (board_result(&b) != RESULT_NONE) return 1;
    (*nonterminal)++;
    for (int cell = 0; cell < 9; cell++)
        if (!((own | opp) >> cell & 1u))
            total += count_reachable(opp, (uint16_t)(own | 1u << cell), seen, nonterminal);
    return total;
}

static int rules_test(void)
{
    Board b;
    board_init(&b, MARK_X);
    EXPECT(board_to_move(&b) == MARK_X && board_legal(&b) == 0x1ff, "X opens on an empty board");
    int moves[] = { 0, 3, 1, 4, 2 };
    for (int i = 0; i < 5; i++) board_play(&b, moves[i]);
    EXPECT(board_result(&b) == RESULT_X && board_winning_line(&b) == 0x007,
           "a completed top row wins for X and is reported as the line");
    EXPECT(board_legal(&b) == 0 && !board_play(&b, 8), "no moves after a win");

    board_init(&b, MARK_X);
    board_play(&b, 4);
    EXPECT(!board_play(&b, 4) && board_to_move(&b) == MARK_O, "an occupied square is refused");
    int draw[] = { 0, 8, 2, 1, 7, 6, 3, 5 };   /* X4 O0 X8 O2 X1 O7 X6 O3 X5 */
    for (int i = 0; i < 8; i++) board_play(&b, draw[i]);
    EXPECT(board_result(&b) == RESULT_DRAW, "a full board with no line is a draw");

    board_init(&b, MARK_O);
    EXPECT(board_to_move(&b) == MARK_O, "O moves first when it starts");
    board_play(&b, 0);
    EXPECT(board_cell(&b, 0) == MARK_O && board_to_move(&b) == MARK_X,
           "turns alternate from the starting mark");

    EXPECT(solve_value(0, 0) == 0, "perfect play from the empty board is a draw");
    EXPECT(solve_optimal(0, 0) == 0x1ff, "every opening move keeps the draw");
    /* X corner, O edge next to it: X can force a win. */
    EXPECT(solve_value(0x001, 0x002) == 1, "a corner opening beats an adjacent edge reply");
    EXPECT(solve_distance(0x003, 0x018) == 1 && (solve_optimal(0x003, 0x018) & 0x004),
           "an immediate win is optimal and one ply away");
    EXPECT(solve_value(0x018, 0x003) == 1 && (solve_optimal(0x018, 0x003) & 0x020),
           "completing the middle row is found even with a block available");

    static uint8_t seen[19683];
    int nonterminal = 0;
    int total = count_reachable(0, 0, seen, &nonterminal);
    char label[96];
    (void)snprintf(label, sizeof label, "5478 reachable positions, 4520 with a move (%d, %d)",
                   total, nonterminal);
    EXPECT(total == 5478 && nonterminal == 4520, label);

    /* ---- menus ---- */
    Game g;
    game_init(&g, 7);
    EXPECT(g.screen == SCREEN_MENU && g.menu_row == MENU_PLAY, "the game opens on the menu");
    key(&g, 'q');
    EXPECT(!g.quit, "q does not quit");
    key(&g, KEY_ESC);
    EXPECT(!g.quit && g.menu_row == MENU_QUIT, "Esc on the menu selects QUIT without quitting");
    key(&g, KEY_DOWN);                          /* wraps to PLAY */
    key(&g, KEY_DOWN);                          /* X PLAYER */
    key(&g, KEY_RIGHT);                         /* HUMAN -> NEURAL */
    key(&g, KEY_DOWN);                          /* O PLAYER */
    key(&g, KEY_LEFT);                          /* NEURAL -> HUMAN */
    key(&g, KEY_DOWN);                          /* LEVEL */
    key(&g, KEY_ENTER);                         /* PERFECT -> EASY (wraps) */
    key(&g, KEY_DOWN);                          /* FIRST MOVE */
    key(&g, KEY_RIGHT);                         /* ALTERNATE */
    EXPECT(g.players[0] == PLAYER_NEURAL && g.players[1] == PLAYER_HUMAN &&
           g.level == LEVEL_EASY && g.starts == STARTS_ALTERNATE && g.screen == SCREEN_MENU,
           "menu rows change their settings with arrows and Enter");
    g.menu_row = MENU_PLAY;
    key(&g, KEY_ENTER);
    EXPECT(g.screen == SCREEN_PLAY && game_is_computer_turn(&g),
           "PLAY starts a game; a neural X moves first");
    game_update(&g, AI_DELAY_MS - 1);
    EXPECT(board_moves_made(&g.board) == 0, "the computer waits out its delay");
    game_update(&g, 1);
    EXPECT(board_moves_made(&g.board) == 1 && !game_is_computer_turn(&g),
           "then plays one move and hands the turn over");
    int before = board_moves_made(&g.board);
    int free_cell = -1;
    for (int c = 0; c < 9 && free_cell < 0; c++)
        if (board_cell(&g.board, c) == MARK_NONE) free_cell = c;
    int numpad[9] = { '7', '8', '9', '4', '5', '6', '1', '2', '3' };
    key(&g, numpad[free_cell]);
    EXPECT(board_moves_made(&g.board) == before + 1 &&
           board_cell(&g.board, free_cell) == MARK_O, "a numpad digit plays that square");

    key(&g, KEY_ESC);
    EXPECT(g.screen == SCREEN_PAUSE, "Esc during play opens the pause menu");
    key(&g, KEY_ESC);
    EXPECT(g.screen == SCREEN_PLAY, "Esc again resumes");
    key(&g, 'p');
    key(&g, KEY_DOWN);                          /* RESTART GAME */
    key(&g, KEY_ENTER);
    EXPECT(g.screen == SCREEN_PLAY && board_moves_made(&g.board) == 0 && g.games == 1,
           "RESTART GAME clears the board and keeps the game count");
    key(&g, 'p');
    key(&g, KEY_UP);                            /* wraps to QUIT */
    key(&g, KEY_ENTER);
    EXPECT(g.quit, "the pause menu's QUIT exits");

    /* ---- a finished game, the game-over menu, alternating starts ---- */
    game_init(&g, 3);
    g.players[0] = g.players[1] = PLAYER_HUMAN;
    g.starts = STARTS_ALTERNATE;
    key(&g, KEY_ENTER);                         /* PLAY */
    int win[] = { '7', '4', '8', '5', '9' };    /* X takes the top row */
    for (int i = 0; i < 5; i++) key(&g, win[i]);
    EXPECT(g.screen == SCREEN_OVER && g.last_result == RESULT_X && g.wins_x == 1,
           "three in a row ends the game and scores it");
    key(&g, KEY_ENTER);                         /* PLAY AGAIN */
    EXPECT(g.screen == SCREEN_PLAY && board_to_move(&g.board) == MARK_O,
           "ALTERNATE gives the second game's first move to O");
    key(&g, '5');
    key(&g, '5');
    EXPECT(board_moves_made(&g.board) == 1 && strstr(g.message, "taken"),
           "playing an occupied square is refused with a message");
    game_init(&g, 3);
    g.players[1] = PLAYER_HUMAN;
    key(&g, KEY_ENTER);
    for (int i = 0; i < 5; i++) key(&g, win[i]);
    key(&g, KEY_DOWN);
    key(&g, KEY_DOWN);                          /* QUIT */
    key(&g, KEY_ENTER);
    EXPECT(g.quit, "the game-over menu's QUIT exits");

    if (failures) {
        fprintf(stderr, "rules-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("rules-test: all rules passed");
    return 0;
}

/* The shipped network, checked exhaustively against the solver. */
static int optimal_checked, slow_wins, not_optimal;
static uint8_t visited[19683];

static void check_all(uint16_t own, uint16_t opp)
{
    int index = 0;
    for (int cell = 8; cell >= 0; cell--)
        index = index * 3 + (own >> cell & 1u ? 1 : (opp >> cell & 1u ? 2 : 0));
    if (visited[index]) return;
    visited[index] = 1;
    uint16_t optimal = solve_optimal(own, opp);
    if (!optimal) return;                        /* game over */
    Board b = mover_board(own, opp);
    int pick = choose_move(&b, PLAYER_NEURAL, LEVEL_PERFECT, &(uint32_t){ 1 });
    optimal_checked++;
    if (!(optimal >> pick & 1u)) not_optimal++;
    else if (solve_value(own, opp) > 0 &&
             1 + solve_distance(opp, (uint16_t)(own | 1u << pick)) != solve_distance(own, opp))
        slow_wins++;
    for (int cell = 0; cell < 9; cell++)
        if (!((own | opp) >> cell & 1u)) check_all(opp, (uint16_t)(own | 1u << cell));
}

/* The network (PERFECT) against every possible line of opponent play. */
static void never_loses(Board b, int neural_mark, long *games, long *losses, long *wins)
{
    int result = board_result(&b);
    if (result != RESULT_NONE) {
        (*games)++;
        if (result == (neural_mark == MARK_X ? RESULT_O : RESULT_X)) (*losses)++;
        else if (result != RESULT_DRAW) (*wins)++;
        return;
    }
    if (board_to_move(&b) == neural_mark) {
        Board next = b;
        board_play(&next, choose_move(&b, PLAYER_NEURAL, LEVEL_PERFECT, &(uint32_t){ 1 }));
        never_loses(next, neural_mark, games, losses, wins);
        return;
    }
    for (int cell = 0; cell < 9; cell++) {
        Board next = b;
        if (board_play(&next, cell)) never_loses(next, neural_mark, games, losses, wins);
    }
}

typedef struct { int wins, draws, losses; } Tally;

static Tally play_series(int level, int opponent, int games, uint32_t seed)
{
    Tally t = { 0, 0, 0 };
    uint32_t rng = seed;
    for (int i = 0; i < games; i++) {
        Board b;
        board_init(&b, i % 2 ? MARK_O : MARK_X);
        int neural_mark = i % 4 < 2 ? MARK_X : MARK_O;
        while (board_result(&b) == RESULT_NONE) {
            bool neural = board_to_move(&b) == neural_mark;
            int move = neural ? choose_move(&b, PLAYER_NEURAL, level, &rng)
                              : (opponent < 0 ? choose_move(&b, PLAYER_NEURAL, LEVEL_PERFECT, &rng)
                                              : choose_move(&b, opponent, 0, &rng));
            board_play(&b, move);
        }
        int result = board_result(&b);
        if (result == RESULT_DRAW) t.draws++;
        else if (result == (neural_mark == MARK_X ? RESULT_X : RESULT_O)) t.wins++;
        else t.losses++;
    }
    return t;
}

static int neural_test(void)
{
    if (!neural_ready()) {
        fprintf(stderr, "FAIL: neural policy: %s\n", neural_status());
        return 1;
    }
    printf("neural-test: %s\n", neural_status());
    check_all(0, 0);
    char label[128];
    (void)snprintf(label, sizeof label,
                   "PERFECT picks an optimal move in all %d positions (%d not optimal)",
                   optimal_checked, not_optimal);
    EXPECT(optimal_checked == 4520 && not_optimal == 0, label);
    (void)snprintf(label, sizeof label, "and a fastest win whenever it can win (%d slower)",
                   slow_wins);
    EXPECT(slow_wins == 0, label);

    for (int mark = MARK_X; mark <= MARK_O; mark++) {
        for (int first = MARK_X; first <= MARK_O; first++) {
            long games = 0, losses = 0, wins = 0;
            Board b;
            board_init(&b, first);
            never_loses(b, mark, &games, &losses, &wins);
            (void)snprintf(label, sizeof label,
                           "as %c, %c first: never loses to any of %ld opponent lines (%ld wins)",
                           mark == MARK_X ? 'X' : 'O', first == MARK_X ? 'X' : 'O',
                           games, wins);
            EXPECT(losses == 0 && games > 0, label);
        }
    }

    Tally level[LEVEL_COUNT];
    for (int l = 0; l < LEVEL_COUNT; l++) {
        level[l] = play_series(l, PLAYER_RANDOM, 2000, 99);
        printf("neural-test: %-7s vs RANDOM  %4d-%4d-%4d (W-D-L)\n", level_name(l),
               level[l].wins, level[l].draws, level[l].losses);
    }
    EXPECT(level[LEVEL_PERFECT].losses == 0, "PERFECT never loses to RANDOM");
    EXPECT(level[LEVEL_EASY].wins < level[LEVEL_NORMAL].wins &&
           level[LEVEL_NORMAL].wins <= level[LEVEL_PERFECT].wins &&
           level[LEVEL_EASY].losses > level[LEVEL_NORMAL].losses,
           "levels are ordered: EASY < NORMAL <= PERFECT against RANDOM");
    Tally easy = play_series(LEVEL_EASY, -1, 2000, 7);
    printf("neural-test: EASY    vs PERFECT %4d-%4d-%4d (W-D-L)\n",
           easy.wins, easy.draws, easy.losses);
    EXPECT(easy.losses > 0, "EASY can be beaten (it loses to PERFECT sometimes)");

    if (failures) {
        fprintf(stderr, "neural-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("neural-test: the network plays perfectly and the levels are ordered");
    return 0;
}

static int render_test(void)
{
    static Screen s;
    static Game g;
    char row[1024];
    game_init(&g, 1);
    render(&g, &s, 80, 24);
    bool menu_ok = false;
    for (int y = 0; y < s.h; y++) {
        screen_row_text(&s, y, row, sizeof row);
        if (strstr(row, "PLAY") && !strstr(row, "PLAYER")) menu_ok = true;
    }
    EXPECT(menu_ok && render_hit_row(&g, 80, 24, 40, 7) == MENU_PLAY,
           "the menu draws PLAY where a click selects it");

    g.players[1] = PLAYER_HUMAN;
    key(&g, KEY_ENTER);
    key(&g, '5');                               /* X centre */
    key(&g, '7');                               /* O top left */
    render(&g, &s, 80, 24);
    int x_rows = 0, o_rows = 0;
    for (int y = 0; y < s.h; y++) {
        screen_row_text(&s, y, row, sizeof row);
        if (strstr(row, "╳")) x_rows++;
        if (strstr(row, "╭───╮")) o_rows++;
    }
    EXPECT(x_rows == 1 && o_rows == 1, "marks are drawn on the board");
    EXPECT(render_hit_cell(80, 24, 40, 11) == 4 && render_hit_cell(80, 24, 27, 5) == 0 &&
           render_hit_cell(80, 24, 33, 11) == -1,
           "board clicks map to squares and grid lines hit nothing");
    /* wider than the drawing limit: clicks map through the clamped layout */
    render(&g, &s, 240, 40);
    {
        int cx = -1;
        for (int y = 0; y < s.h && cx < 0; y++) {
            screen_row_text(&s, y, row, sizeof row);
            if (strstr(row, "╳")) cx = y;
        }
        int hit = -1, drawn_x = -1;
        for (int x = 0; x < s.w && drawn_x < 0; x++)
            if (!strcmp(s.cells[cx][x].ch, "╳")) drawn_x = x;
        Input click = { KEY_MOUSE, drawn_x, cx };
        resolve_mouse(&g, &click, &s);         /* the interactive path */
        hit = click.key == KEY_MOUSE ? click.mouse_x : -1;
        EXPECT(s.w == SCREEN_MAX_W && drawn_x > 0 && hit == 4,
               "on a 240-column terminal a click on the drawn centre square hits it");
    }
    render(&g, &s, 30, 10);
    {
        Input click = { KEY_MOUSE, 15, 5 };
        resolve_mouse(&g, &click, &s);
        EXPECT(click.key == KEY_NONE, "clicks are ignored while the terminal is too small");
    }
    screen_row_text(&s, 5, row, sizeof row);
    EXPECT(strstr(row, "Enlarge") != NULL, "a small terminal asks to be enlarged");
    key(&g, KEY_ESC);
    render(&g, &s, 80, 24);
    int hit = -1;
    for (int x = 0; x < 80 && hit != PAUSE_QUIT; x++) hit = render_hit_row(&g, 80, 24, x, 22);
    EXPECT(hit == PAUSE_QUIT, "the pause menu's items are clickable");

    if (failures) {
        fprintf(stderr, "render-test: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("render-test: layout checks passed");
    return 0;
}

/* ---------- options ---------- */

static int parse_named(const char *text, const char *const *names, int count)
{
    for (int i = 0; text && i < count; i++)
        if (!strcasecmp(text, names[i])) return i;
    return -1;
}

static void usage(void)
{
    puts("tictactoe-tui - tic-tac-toe in this terminal, against a trained network\n"
         "\nusage: tictactoe-tui [--x P] [--o P] [--level L] [--first F]\n"
         "  P: human|neural|random   L: easy|normal|perfect   F: x|alternate\n"
         "\ntests:\n"
         "  --rules-test      rules, solver, menus and turn handling\n"
         "  --neural-test     prove the network plays perfectly; level ordering\n"
         "  --render-test     screen layout and click mapping\n"
         "  --version, --help\n"
         "\nkeys: arrows or WASD move, Enter/Space place, 1-9 numpad squares,\n"
         "      ? neural hint, Esc pause menu (quit from a menu), mouse clicks work");
}

int main(int argc, char **argv)
{
    static const char *const players[] = { "human", "neural", "random" };
    static const char *const levels[] = { "easy", "normal", "perfect" };
    static const char *const firsts[] = { "x", "alternate" };
    if (argc > 1) {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return 0; }
        if (!strcmp(argv[1], "--version")) { puts("tictactoe-tui " VERSION); return 0; }
        if (!strcmp(argv[1], "--rules-test")) return rules_test();
        if (!strcmp(argv[1], "--neural-test")) return neural_test();
        if (!strcmp(argv[1], "--render-test")) return render_test();
    }
    for (int i = 1; i < argc; i += 2) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        int slot = -1, parsed = -1;
        if (!strcmp(argv[i], "--x")) slot = 0, parsed = parse_named(value, players, 3);
        else if (!strcmp(argv[i], "--o")) slot = 1, parsed = parse_named(value, players, 3);
        else if (!strcmp(argv[i], "--level")) slot = 2, parsed = parse_named(value, levels, 3);
        else if (!strcmp(argv[i], "--first")) slot = 3, parsed = parse_named(value, firsts, 2);
        if (slot < 0 || parsed < 0) {
            fprintf(stderr, "tictactoe-tui: bad option '%s %s'\n", argv[i], value ? value : "");
            usage();
            return 2;
        }
        cli[slot] = parsed;
    }
    return run_interactive();
}
