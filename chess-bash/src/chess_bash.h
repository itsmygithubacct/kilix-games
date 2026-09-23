/*
 * Chess Bash - animated Kitty-terminal chess in C.
 *
 * The terminal layer follows the Bashed Earth / Terminal Lander style:
 * software RGBA framebuffer, zlib/base64 kitty graphics, raw keyboard input.
 */
#ifndef CHESS_BASH_H
#define CHESS_BASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define TICK_DT  (1.0f / 60.0f)
#define MAX_MOVES 256
/* longest practical game with automatic draw rules is far below this */
#define UCI_HISTORY_MAX 32768
#define MAX_PLIES 1024
#define BOARD_THEME_COUNT 6
#define DIFF_COUNT 3

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

enum { SIDE_WHITE, SIDE_BLACK };
enum { PT_PAWN, PT_KNIGHT, PT_BISHOP, PT_ROOK, PT_QUEEN, PT_KING, PT_COUNT };

enum {
    GS_TITLE,
    GS_PLAYING,
    GS_ANIMATING,
    GS_GAMEOVER,
    GS_INTRO,
    GS_PROMOTING
};

enum {
    MODE_HUMAN_HUMAN,
    MODE_HUMAN_AI,
    MODE_AI_AI,
    MODE_COUNT
};

enum {
    SND_SELECT,
    SND_MOVE,
    SND_CAPTURE,
    SND_FALL,
    SND_START_TRUMPET,
    SND_WIN_TRUMPET,
    SND_CHECK,
    SOUND_COUNT
};

enum {
    MUS_THINKING,
    MUS_BATTLE,
    MUS_VICTORY,
    MUSIC_COUNT
};

/* title menu rows */
enum {
    MENU_MODE,
    MENU_SIDE,
    MENU_DIFF,
    MENU_BOARD,
    MENU_MUSIC,
    MENU_SPEED,
    MENU_START,
    MENU_COUNT
};

enum {
    RESULT_NONE,
    RESULT_CHECKMATE,
    RESULT_STALEMATE,
    RESULT_FIFTY_MOVE,
    RESULT_REPETITION,
    RESULT_MATERIAL,
    RESULT_RESIGN
};

typedef struct {
    int from, to;
    char promo;
    bool capture;
    bool castle;
    bool en_passant;
    bool double_push;
} Move;

typedef struct {
    pid_t pid;
    int in_fd;          /* nonblocking command pipe: cancellation-safe writes */
    int out_fd;         /* raw fd + own line buffer: stdio buffering would
                           strand lines already read past a select() wakeup */
    char rdbuf[4096];
    int rdlen;
    bool ok;
    char name[96];
} Engine;

typedef struct {
    int w, h;
    uint32_t *px;
    bool ok;
} Bitmap;

typedef struct {
    bool active;
    Move move;
    char attacker;
    char victim;
    int attacker_type;
    int victim_type;
    int victim_sq;          /* real square of the victim (en passant aware) */
    int rook_from, rook_to; /* castling companion move, -1 when unused */
    int effect;
    float t;
    float duration;
    uint32_t sound_flags;
    char caption[128];
} BattleAnim;

typedef struct {
    int state;
    int W, H;
    bool quit;
    bool headless;

    char board[64];
    int side;
    int cursor;
    int selected;
    int mode;
    bool human_white;
    bool fast_anim;
    bool music_on;
    int difficulty;         /* index into difficulty table */
    int board_theme;

    /* title menu */
    int title_row;
    float intro_t;
    float title_t;

    bool castle_wk, castle_wq, castle_bk, castle_bq;
    int ep_square;
    int halfmove_clock;
    int fullmove_number;

    Move legal[MAX_MOVES];
    int legal_count;
    char uci_history[UCI_HISTORY_MAX];
    int ply_count;
    char last_move[8];
    char status[160];
    char result[80];
    int result_kind;
    int winner_side;        /* side index or -1 for draw */
    int loser_king_sq;      /* fallen king shown on the gameover board */
    float over_t;           /* seconds since the game ended */

    /* check feedback */
    float check_flash;      /* seconds remaining on the king flash */
    int check_sq;

    /* pawn promotion chooser */
    int promo_from, promo_to;
    int promo_cursor;       /* 0=Q 1=R 2=B 3=N */

    /* resign confirmation */
    float resign_arm;
    float leave_arm;

    /* captured pieces trays */
    char captured[2][16];   /* [capturing side][n] */
    int captured_count[2];

    Engine engines[2];
    bool engine_attempted[2];
    int ai_movetime_ms;
    bool ai_thinking;       /* async request outstanding */
    int frame_count;
    float ai_delay;

    BattleAnim anim;
} GameState;

extern GameState G;

/* ---------- utilities / game ---------- */
void asset_paths_init(void);
const char *asset_path(const char *rel);
float clampf(float v, float lo, float hi);
int sq_file(int sq);
int sq_rank(int sq);
int make_sq(int file, int rank);
const char *side_name(int side);
const char *mode_name(int mode);
const char *difficulty_name(int diff);
int piece_color(char p);
int piece_type(char p);
char piece_upper(char p);
const char *piece_name(int type);

void game_init(int w, int h, uint32_t seed);
void game_shutdown(void);
void game_reset_to_title(void);
void game_start(int mode);
void game_tick(void);
void game_handle_key(int key);
void game_generate_legal(void);
bool game_apply_human_target(int from, int to);
bool game_apply_uci_move(const char *uci);
void game_move_to_uci(Move m, char out[8]);
bool game_is_human_turn(void);
bool game_view_flipped(void);
bool game_side_in_check(int side);
int game_king_square(int side);
bool game_load_fen(const char *fen);
uint64_t game_perft(int depth);

/* ---------- engine.c ---------- */
bool engine_start(Engine *e);
void engine_stop(Engine *e);
bool engine_bestmove(Engine *e, const char *history, int movetime_ms,
                     char out_move[8], char *status, size_t status_len);
/* async wrapper: start/search on a worker thread, poll for the reply */
bool engine_request(Engine *e, const char *history, int movetime_ms);
int engine_poll(Engine *e, char out_move[8], char *status, size_t status_len);
void engine_cancel_all(void);
enum { ENGINE_PENDING, ENGINE_DONE, ENGINE_FAILED };

/* ---------- render.c ---------- */
void render_init(int w, int h);
void render_resize(int w, int h);
void render_shutdown(void);
void render_frame(void);
uint8_t *render_fb(void);
bool render_dump_ppm(const char *path);
bool render_validate_assets(char *error, size_t error_len);
const char *board_theme_name(int i);
void render_set_theme(int i);

/* ---------- term.c ---------- */
bool term_init(int *outW, int *outH);
bool term_check_resize(int *outW, int *outH);
void term_present(const uint8_t *rgba, int w, int h);
int term_poll_key(void);
void term_shutdown(void);
void term_emergency_restore(void);

/* ---------- sound.c ---------- */
bool sound_init(void);
void sound_shutdown(void);
void sound_play(int id, float vol, float pitch);
void sound_music_play(int id, float vol, bool loop);
void sound_music_stop(float fade_seconds);
int sound_music_current(void);

#endif
