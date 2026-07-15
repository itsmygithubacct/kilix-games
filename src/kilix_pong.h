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
enum { MODE_AI, MODE_2P, MODE_COUNT };
enum { SIDE_LEFT, SIDE_RIGHT, SIDE_COUNT };

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

typedef struct {
    float x, y, w, h;
    float vy;
    float input;    /* -1..1, reduced from held state by game.c */
    int   score;
    bool  is_ai;
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
    int  mode;

    uint32_t rng;
    uint64_t ticks;

    Paddle   paddles[SIDE_COUNT];
    Ball     ball;
    Particle particles[MAX_PARTICLES];

    bool     act_held[ACT_COUNT];
    uint64_t act_tick[ACT_COUNT];   /* tick of last press/repeat; legacy expiry */

    int   serve_to;       /* SIDE_* that serves next */
    int   winner;         /* SIDE_* once GS_GAMEOVER, else -1 */
    int   rally;
    float state_timer, shake, flash;
} GameState;

extern GameState G;

/* ---------- game.c ---------- */
float clampf(float v, float lo, float hi);
float game_randf(void);
void  game_init(int w, int h, uint32_t seed);
void  game_start(int mode);
void  game_tick(void);
void  game_handle_event(const KeyEvent *ev);
void  game_autopilot(void);
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
