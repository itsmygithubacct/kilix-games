/*
 * Kilix Brokeout - kitty-protocol brick breaker.
 */
#ifndef KITTY_BROKEOUT_H
#define KITTY_BROKEOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty_keyboard.h"

#define TICK_DT  (1.0f / 60.0f)
#define TICK_MS  16.666666f

#define MAX_BRICKS    176
#define MAX_BALLS     5
#define MAX_PARTICLES 720
#define MAX_POWERUPS  24
#define MAX_STARS     180

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

enum {
    GS_TITLE,
    GS_CONTROLS,
    GS_PLAYING,
    GS_PAUSED,
    GS_LEVEL_CLEAR,
    GS_BALL_LOST,
    GS_GAMEOVER
};

enum {
    BRICK_NORMAL,
    BRICK_METAL,
    BRICK_EXPLOSIVE,
    BRICK_SPEED,
    BRICK_COUNT
};

enum {
    PU_WIDE,
    PU_SLOW,
    PU_MULTI,
    PU_SHIELD,
    PU_COUNT
};

enum {
    SND_PADDLE,
    SND_BRICK,
    SND_METAL,
    SND_EXPLODE,
    SND_POWERUP,
    SND_LAUNCH,
    SND_LOSE,
    SND_CLEAR,
    SND_MENU,
    SOUND_COUNT
};

typedef struct {
    bool alive;
    int type;
    int hits, maxHits;
    float x, y, w, h;
    float pulse;
} Brick;

typedef struct {
    bool active;
    bool attached;
    float x, y, vx, vy, radius;
    float speed;
    float trailX[10], trailY[10];
    int trailHead;
} Ball;

typedef struct {
    float x, y, w, h;
    float moveAxis;
    float intentTimer;
    float wideTimer;
    float shieldTimer;
} Paddle;

typedef struct {
    bool active;
    int type;
    float x, y, vy;
    float phase;
} Powerup;

typedef struct {
    bool active;
    float x, y, vx, vy;
    float life, maxLife, size;
    uint32_t color;
    int kind;
} Particle;

typedef struct {
    float x, y;
    uint8_t brightness;
    float phase, speed;
    int size;
} Star;

typedef struct {
    int state;
    int W, H;
    bool quit, headless;

    float scale;
    float playX, playY, playW, playH;

    Paddle paddle;
    Ball balls[MAX_BALLS];
    Brick bricks[MAX_BRICKS];
    Powerup powerups[MAX_POWERUPS];
    Particle particles[MAX_PARTICLES];
    Star stars[MAX_STARS];
    int numBricks, numStars;

    int score, highScore, savedHighScore, level, lives;
    int combo;
    float comboTimer;
    float stateTimer;
    float levelTimer;
    float launchAngle;
    float speedBoostTimer;
    float screenFlash;
    float cameraShake;
    int frameCount;
    bool soundEnabled;
    bool heldControls, heldLeft, heldRight;

    uint32_t rng;
} GameState;

extern GameState G;
extern const char *POWERUP_NAMES[PU_COUNT];

/* ---------- utilities / game ---------- */
void frand_seed(uint32_t seed);
float frandf(void);
float clampf(float v, float lo, float hi);

void game_init(int w, int h, uint32_t seed);
void game_shutdown(void);
void game_reset_to_title(void);
void game_start_run(void);
void game_tick(void);
void game_handle_key(int key);
void game_set_held_controls(bool available, bool left, bool right);
void game_autopilot_tick(void);
void game_force_level_clear(void);
void game_force_gameover(void);

int game_active_ball_count(void);
int game_remaining_breakable_bricks(void);
float game_ball_speed_target(void);

/* ---------- render.c ---------- */
void render_init(int w, int h);
void render_shutdown(void);
void render_frame(void);
uint8_t *render_fb(void);

/* ---------- term.c ---------- */
bool term_init(int *outW, int *outH);
void term_present(const uint8_t *rgba, int w, int h);
int term_read_input(void);
bool term_next_key_event(kittykb_event *event);
bool term_key_down(uint32_t key);
bool term_has_release_events(void);
void term_shutdown(void);
void term_emergency_restore(void);

/* ---------- sound.c ---------- */
bool sound_init(void);
void sound_shutdown(void);
void sound_set_enabled(bool on);
bool sound_is_enabled(void);
void sound_play(int id, float vol, float pitch);
void sound_loop(int id, bool on, float vol, float pitch);

#endif
