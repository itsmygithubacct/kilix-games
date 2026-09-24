/* tictactoe-tui: tic-tac-toe in the terminal it is started from, with a
   trained neural opponent. */
#ifndef TTT_H
#define TTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- rules (engine.c) ----------
 *
 * Cells are numbered 0..8 row-major from the top left. A board is two
 * bitmasks plus the mark that moved first, so whose turn it is follows from
 * the counts. Either mark may start (the ALTERNATE option); everything the
 * solver and the network see is relative to the player to move, so the two
 * cases are the same game.
 */
enum { MARK_NONE, MARK_X, MARK_O };
enum { RESULT_NONE, RESULT_X, RESULT_O, RESULT_DRAW };

typedef struct {
    uint16_t x, o;   /* bit i set: that mark is on cell i */
    int first;       /* MARK_X or MARK_O */
} Board;

void board_init(Board *b, int first);
int  board_cell(const Board *b, int cell);
int  board_to_move(const Board *b);
int  board_result(const Board *b);
/* Bitmask of the winning line's cells, or 0. */
uint16_t board_winning_line(const Board *b);
/* Bitmask of the legal moves (empty once the game is over). */
uint16_t board_legal(const Board *b);
bool board_play(Board *b, int cell);
int  board_moves_made(const Board *b);

/* Exact minimax, memoized over every (own, opponent) position. value is
   for the player to move: +1 win, 0 draw, -1 loss. */
int  solve_value(uint16_t own, uint16_t opp);
/* Value of playing `cell` for the player to move. */
int  solve_move_value(uint16_t own, uint16_t opp, int cell);
/* Plies to the end when the winner wins fastest and the loser holds out. */
int  solve_distance(uint16_t own, uint16_t opp);
/* Bitmask of the moves that keep the best value. */
uint16_t solve_optimal(uint16_t own, uint16_t opp);
/* The mover's (own, opp) masks. */
void board_relative(const Board *b, uint16_t *own, uint16_t *opp);

/* ---------- players (engine.c) ---------- */

enum { PLAYER_HUMAN, PLAYER_NEURAL, PLAYER_RANDOM, PLAYER_COUNT };
enum { LEVEL_EASY, LEVEL_NORMAL, LEVEL_PERFECT, LEVEL_COUNT };

#define POLICY_FEATURES 18   /* own marks (9), then opponent marks (9) */
#define POLICY_ACTIONS  9    /* one score per cell */

void neural_features(uint16_t own, uint16_t opp, float out[POLICY_FEATURES]);
bool neural_ready(void);
const char *neural_status(void);
/* The network's scores for every cell; false if the policy is unusable. */
bool neural_scores(const Board *b, float out[POLICY_ACTIONS]);
/* A move for the side to play. PERFECT is the network's argmax; NORMAL and
   EASY sample from its scores at a temperature, so they make real mistakes.
   A missing policy falls back to the exact solver so the game still plays. */
int  choose_move(const Board *b, int player, int level, uint32_t *rng);

uint32_t rng_next(uint32_t *state);
const char *player_name(int player);
const char *level_name(int level);

/* ---------- the app (game.c) ---------- */

enum { SCREEN_MENU, SCREEN_PLAY, SCREEN_PAUSE, SCREEN_OVER };
enum {
    MENU_PLAY, MENU_X, MENU_O, MENU_LEVEL, MENU_STARTS, MENU_QUIT, MENU_ROWS
};
enum { PAUSE_RESUME, PAUSE_RESTART, PAUSE_MENU, PAUSE_QUIT, PAUSE_ROWS };
enum { OVER_AGAIN, OVER_MENU, OVER_QUIT, OVER_ROWS };
enum { STARTS_X, STARTS_ALTERNATE, STARTS_COUNT };

/* Input, already decoded from the terminal. */
enum {
    KEY_NONE = 0, KEY_ENTER = 1000, KEY_ESC, KEY_UP, KEY_DOWN, KEY_LEFT,
    KEY_RIGHT, KEY_MOUSE, KEY_RESIZE, KEY_INTERRUPT
};
typedef struct {
    int key;          /* a KEY_* or an ASCII character */
    int mouse_x;      /* KEY_MOUSE: 0-based cell of a left-button press */
    int mouse_y;
} Input;

#define AI_DELAY_MS 450   /* a computer player waits this long before moving */

typedef struct {
    int screen;
    int menu_row, pause_row, over_row;
    int players[2];        /* [0] plays X, [1] plays O: PLAYER_* */
    int level;             /* LEVEL_* for every neural side */
    int starts;            /* STARTS_* */
    Board board;
    int cursor;            /* cell under the keyboard cursor */
    int hint;              /* cell the network suggests, or -1 */
    int games;             /* games started this session */
    int wins_x, wins_o, draws;
    int last_result;       /* RESULT_* of the finished game */
    int ai_wait_ms;        /* countdown before a computer move */
    uint32_t rng;
    bool quit;
    char message[64];      /* one status line */
} Game;

void game_init(Game *g, uint32_t seed);
void game_new_round(Game *g);
/* Advances timers by elapsed_ms and lets a computer side move when due. */
void game_update(Game *g, int elapsed_ms);
void game_input(Game *g, const Input *in);
bool game_is_computer_turn(const Game *g);

/* ---------- drawing (render.c) ----------
 *
 * render() writes a whole frame into a Screen, a grid of cells, so layout is
 * testable without a terminal; term.c turns a Screen into escape sequences.
 */
#define SCREEN_MAX_W 200
#define SCREEN_MAX_H 80
#define MIN_W 44
#define MIN_H 24

enum {
    COLOR_DEFAULT, COLOR_DIM, COLOR_X, COLOR_O, COLOR_GRID, COLOR_TITLE,
    COLOR_SELECT, COLOR_WIN, COLOR_HINT, COLOR_ALERT
};

typedef struct {
    char ch[5];          /* one UTF-8 character, NUL-terminated */
    uint8_t color;       /* COLOR_* */
    bool bold, reverse;
} Cell;

typedef struct {
    int w, h;
    Cell cells[SCREEN_MAX_H][SCREEN_MAX_W];
} Screen;

void render(const Game *g, Screen *s, int w, int h);
/* The board cell drawn at screen position (x, y), or -1. */
int  render_hit_cell(int w, int h, int x, int y);
/* The menu row at (x, y) on the current screen, or -1 (mouse on menus). */
int  render_hit_row(const Game *g, int w, int h, int x, int y);
/* Plain text of one screen row, for tests. */
void screen_row_text(const Screen *s, int y, char *out, size_t out_len);

/* ---------- terminal (term.c) ---------- */
bool term_init(void);
void term_shutdown(void);
void term_size(int *w, int *h);
/* Waits up to timeout_ms for input; false when none arrived. */
bool term_read(Input *in, int timeout_ms);
void term_draw(const Screen *s);

#endif
