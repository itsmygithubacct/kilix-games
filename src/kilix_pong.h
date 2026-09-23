/* Kilix Pong: a two-paddle rally game for kitty-protocol terminals. */
#ifndef KILIX_PONG_H
#define KILIX_PONG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGICAL_W 320.0f
#define LOGICAL_H 180.0f
#define TICK_DT   (1.0f / 60.0f)

#define PADDLE_W        4.0f
#define PADDLE_H        30.0f
#define PADDLE_INSET    10.0f
#define PADDLE_SPEED    150.0f
#define BALL_RADIUS     2.0f
#define BALL_SPEED_MIN  110.0f
#define BALL_SPEED_MAX  420.0f   /* hard cap: anti-tunneling bound */
#define BALL_SPEED_GAIN 8.0f     /* per paddle contact */
#define BALL_MIN_VX     0.35f    /* fraction of speed; forbids vertical stalls */
#define WIN_SCORE       11
#define BALL_TRAIL_LEN  12
#define MAX_PARTICLES   128

/* Substepping keeps the ball from tunneling a paddle at BALL_SPEED_MAX. The
   cap and this bound are a pair: raising one without the other reopens it. */
#define MAX_SUBSTEP     1.5f     /* logical units of travel per collision step */

/* Legacy terminals report no key release. game.c then expires a held key this
   many ticks after its last press/repeat. Ticks, never wall-clock: a
   wall-clock timeout would desynchronize --selftest across machines. */
#define LEGACY_HOLD_TICKS 8

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

/* Kitty CSI-u modifier bits, already decoded from the wire's (value - 1). */
enum {
    KEY_MOD_SHIFT = 1, KEY_MOD_ALT = 2, KEY_MOD_CTRL = 4, KEY_MOD_SUPER = 8
};

/* Values match the kitty keyboard protocol event types verbatim. */
typedef enum {
    KEY_ACTION_PRESS   = 1,
    KEY_ACTION_REPEAT  = 2,
    KEY_ACTION_RELEASE = 3
} KeyAction;

/* term.c emits these; game.c reduces them to held state. Under the legacy
   fallback term.c never emits KEY_ACTION_RELEASE — see term_has_key_release. */
typedef struct {
    int       key;   /* ASCII/unicode, or a KEY_* special */
    int       mods;  /* KEY_MOD_* bitmask */
    KeyAction action;
} KeyEvent;

enum { GS_TITLE, GS_SERVE, GS_PLAYING, GS_PAUSED, GS_POINT, GS_GAMEOVER };
enum { SIDE_LEFT, SIDE_RIGHT, SIDE_COUNT };

/* Who drives a paddle. Any combination is legal: two humans is local 2P,
   CPU vs NEURAL (or NEURAL vs NEURAL) is a watchable exhibition. */
enum { CTRL_HUMAN, CTRL_CPU, CTRL_NEURAL, CTRL_COUNT };

/* CPU skill. Every level is beatable; see cpu_levels in game.c. */
enum { LEVEL_EASY, LEVEL_NORMAL, LEVEL_HARD, LEVEL_COUNT };

/* Title-menu rows. */
enum { MENU_LEFT, MENU_RIGHT, MENU_LEVEL, MENU_ROWS };

/* Neural policy input width: mirrored per side, so one network plays both.
   game_policy_features() is the single definition the game and the training
   lab (tools/neural) share. */
#define POLICY_FEATURES 11
#define POLICY_ACTIONS  3     /* 0 up, 1 stay, 2 down */

/* Held-state slots. game.c owns these; term.c knows nothing about them. */
enum { ACT_P1_UP, ACT_P1_DOWN, ACT_P2_UP, ACT_P2_DOWN, ACT_COUNT };

/* The python-authored bank in assets/sfx/. There is deliberately no C-synth
   fallback: a missing bank degrades to silence with a diagnostic rather than
   quietly substituting sounds the generator did not produce. */
enum {
    SFX_PADDLE, SFX_WALL, SFX_SCORE, SFX_SERVE, SFX_MISS, SFX_MENU,
    SFX_GAMEOVER, SFX_COUNT
};
#define SFX_VARIANTS 3   /* per cue; rotated so rallies do not machine-gun one sample */

/* CPU opponent state. It lives in the paddle (and so in G) rather than in
   file statics, so copying G copies the whole simulation -- which is what
   the training lab's exact lookahead relies on. */
enum { CPU_IDLE, CPU_REACTING, CPU_TRACKING };
typedef struct {
    int   phase;    /* CPU_* */
    int   timer;    /* ticks left in the reaction delay / until the next replan */
    int   refines;  /* error refinements left for this shot */
    float error;    /* current aim error in logical units */
    float aim;      /* strike offset it plays for, -1 (top) .. +1 (bottom) */
    float target;   /* paddle-centre target */
} CpuBrain;

typedef struct {
    float x, y, w, h;
    float vy;
    float input;       /* -1..1, from keys, the CPU or the neural policy */
    int   score;
    int   controller;  /* CTRL_* */
    CpuBrain cpu;
} Paddle;

typedef struct {
    float x, y, vx, vy;
    float speed;
    float trail_x[BALL_TRAIL_LEN], trail_y[BALL_TRAIL_LEN];
    int   trail_head;
    bool  active;
} Ball;

typedef struct {
    float x, y, vx, vy, life, max_life;
    uint32_t color;
    bool active;
} Particle;

typedef struct {
    int  state;
    int  W, H;
    bool quit, headless, sound_on;

    /* Match setup, chosen on the title menu (or the command line). */
    int  setup[SIDE_COUNT];   /* CTRL_* per side */
    int  level;               /* LEVEL_*, used by every CPU side */
    int  menu_row;            /* MENU_* */
    int  paused_from;         /* state a pause resumes into */

    uint32_t rng;
    uint64_t ticks;

    Paddle   paddles[SIDE_COUNT];
    Ball     ball;
    Particle particles[MAX_PARTICLES];

    bool     act_held[ACT_COUNT];
    uint64_t act_tick[ACT_COUNT];   /* tick of last press/repeat; legacy expiry */

    /* SIDE_* receiving the next serve; serve_ball() aims toward this side.
       After a point, this is the side that conceded. Not the server. */
    int   serve_to;
    int   winner;         /* SIDE_* once GS_GAMEOVER, else -1 */
    int   rally;
    float state_timer, shake, flash;
} GameState;

extern GameState G;

/* ---------- game.c ---------- */
float clampf(float v, float lo, float hi);
float game_randf(void);
void  game_init(int w, int h, uint32_t seed);
/* Starts a match with the current G.setup and G.level. */
void  game_start(void);
/* Validated setters; out-of-range values are clamped to the defaults. */
void  game_configure(int left_controller, int right_controller, int level);
void  game_tick(void);
void  game_handle_event(const KeyEvent *ev);
void  game_autopilot(void);
/* Mirrored policy inputs for `side`: that side always sees itself on the
   left, so one network plays both paddles. */
void  game_policy_features(int side, float out[POLICY_FEATURES]);
/* True when the compiled-in neural policy loaded and matches the feature
   and action widths; otherwise NEURAL sides fall back to the CPU. */
bool  game_neural_ready(void);
const char *game_neural_status(void);
const char *game_controller_name(int controller);
const char *game_level_name(int level);
bool  game_validate(char *error, size_t error_len);
/* Order-independent digest of simulation state. Same seed + same tick count
   must yield an identical value on every run and platform; --selftest prints
   it and `make test` compares two runs of one seed. */
uint64_t game_state_digest(void);

/* ---------- render.c ---------- */
void     render_init(int w, int h);
void     render_resize(int w, int h);
void     render_shutdown(void);
void     render_frame(void);
uint8_t *render_fb(void);
bool     render_dump_ppm(const char *path);

/* ---------- term.c ---------- */
bool term_init(int *out_w, int *out_h);
bool term_check_resize(int *out_w, int *out_h);
void term_present(const uint8_t *rgba, int w, int h);
/* Dequeues one event; returns false when none is pending. */
bool term_poll_event(KeyEvent *out);
/* True only when the kitty keyboard mode is actually active, i.e. releases are
   real. game.c uses LEGACY_HOLD_TICKS expiry when this is false. Local 2P is
   only honest when this is true. */
bool term_has_key_release(void);
void term_shutdown(void);
void term_emergency_restore(void);

/* ---------- sound.c ---------- */
bool        sound_init(void);
void        sound_shutdown(void);
void        sound_play(int id, float volume, float pitch);
void        sound_set_enabled(bool enabled);
bool        sound_is_enabled(void);
bool        sound_bank_loaded(void);
/* Name of the sink actually opened, or NULL. --sound-test reports this rather
   than claiming a sink that was merely probed. */
const char *sound_sink_name(void);

/* ---------- main.c ---------- */
/* KILIX_PONG_ASSETS override, then checkout-adjacent, then installed
   ../share/kilix-pong/assets. sound.c consumes asset_path only. */
void        asset_paths_init(void);
const char *asset_path(const char *relative_path);

#endif
