/*
 * Kilix Fishtank - a software-rasterized virtual aquarium for
 * kitty-protocol terminals.
 */
#ifndef FISHTANK_H
#define FISHTANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TICK_DT  (1.0f / 60.0f)

#define MAX_FISH      42
#define MAX_SHARKS     4
#define MAX_SHIPS      4
#define MAX_BUBBLES  260
#define MAX_PLANTS    42
#define MAX_FOOD     180
#define MAX_ROCKS     14
#define MAX_PARTICLES 420
#define MAX_CANNONBALLS 18
#define MAX_SHARK_BITES 12
#define WATER_NODES   160
#define FISH_SPECIES_COUNT 8
#define ENV_ROCK_ASSET_COUNT 3
#define ENV_PLANT_ASSET_COUNT 4

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_MOUSE, KEY_MOUSE_MOVE
};

typedef struct {
    uint32_t body;
    uint32_t fin;
    uint32_t eye;
} FishPalette;

#define FISH_PALETTE_COUNT 8
extern const FishPalette FISH_PALETTES[FISH_PALETTE_COUNT];

typedef struct {
    bool active;
    float x, y;
    float size;
    float speed;
    int direction;
    float facing;
    float turnTimer;
    int species;
    int palette;
    float tailPhase;
    float targetX;
    float targetY;
    float wobble;
    float attackTimer;
    float biteTimer;
    float attackX, attackY;
} Fish;

typedef struct {
    bool active;
    float x, y;
    int direction;
    float facing;
    float turnTimer;
    float tailPhase;
    float jawPhase;
    float vx, vy;
    float targetX, targetY;
    float lastX, lastY;
    float repathTimer;
    float stuckTimer;
    float chargeTimer;
    float sweepTimer;
    float sweepCooldown;
    int sweepMode;
    int targetFish;
    bool hunting;
    float attackTimer;
} Shark;

typedef struct {
    bool active;
    float x, y;
    float mouthX, mouthY;
    float life, maxLife;
    float victimSize;
    float victimFacing;
    int direction;
    int species;
    int palette;
    int sharkIndex;
    uint32_t seed;
} SharkBite;

typedef struct {
    bool active;
    float x, y;
    int direction;
    float facing;
    float bobPhase;
    float wavePhase;
    int health;
    float reload;
    float cannonFlash;
    bool fishing;
    float hookX, hookY;
    float hookTargetX, hookTargetY;
    float hookVy;
    float fishingPulse;
    int hookedFish;
    int biteFish;
    float biteTimer;
    float catchMeter;
    float tension;
    float reelPower;
} Ship;

typedef struct {
    bool active;
    float x, y, vx, vy;
    float life;
    int owner;
} Cannonball;

typedef struct {
    bool active;
    float x, y;
    float size;
    float speed;
    float phase;
} Bubble;

typedef struct {
    bool active;
    float x;
    float height;
    float phase;
    float sway;
    float scale;
    int type;
    bool flip;
} Plant;

typedef struct {
    bool active;
    float x, y;
    float vy;
    float life;
} Food;

typedef struct {
    float x, w, h;
    int type;
    bool flip;
} Rock;

typedef struct {
    bool active;
    float x, y, vx, vy;
    float life, maxLife;
    float size;
    uint32_t color;
} Particle;

typedef struct {
    int W, H;
    int groundY;
    int waterY;
    float scale;
    bool quit;
    bool paused;
    bool showInfo;
    bool showHelp;
    bool headless;

    Fish fish[MAX_FISH];
    Shark sharks[MAX_SHARKS];
    Ship ships[MAX_SHIPS];
    SharkBite sharkBites[MAX_SHARK_BITES];
    Cannonball cannonballs[MAX_CANNONBALLS];
    Bubble bubbles[MAX_BUBBLES];
    Plant plants[MAX_PLANTS];
    Food food[MAX_FOOD];
    Rock rocks[MAX_ROCKS];
    Particle particles[MAX_PARTICLES];

    int numRocks;
    int frameCount;
    int worldVersion;
    int score;
    int combo;
    int bestCombo;
    int level;
    float bubbleTimer;
    float feedPulse;
    float comboTimer;
    float frenzyMeter;
    float frenzyTimer;
    float scorePulse;
    float dangerPulse;
    float waterLevel[WATER_NODES];
    float waterVel[WATER_NODES];
    float waterFoam[WATER_NODES];
    float currentPhase;

    int mouseX, mouseY;
    uint32_t rng;
} GameState;

extern GameState G;

/* ---------- game.c ---------- */
void frand_seed(uint32_t seed);
float frandf(void);
float clampf(float v, float lo, float hi);

void game_init(int w, int h, uint32_t seed);
void game_shutdown(void);
void game_reset_world(void);
void game_tick(void);
void game_handle_key(int key);
void game_add_food_at(float x, float y, int amount);
int game_count_fish(void);
int game_count_sharks(void);
int game_count_food(void);
int game_count_ships(void);
int game_count_cannonballs(void);

/* ---------- render.c ---------- */
void render_init(int w, int h);
void render_shutdown(void);
void render_frame(void);
uint8_t *render_fb(void);

/* ---------- audio.c ---------- */
typedef enum {
    AUDIO_FEED_SPLASH = 0,
    AUDIO_FISH_BITE,
    AUDIO_SHARK_BITE,
    AUDIO_FRENZY,
    AUDIO_CATCH_SPLASH,
    AUDIO_HOOK_BITE,
    AUDIO_HOOK_SET,
    AUDIO_FISH_ESCAPE,
    AUDIO_LINE_SNAP,
    AUDIO_SHARK_ALERT,
    AUDIO_EVENT_COUNT
} audio_event;

void audio_start(const char *argv0);
void audio_toggle(const char *argv0);
void audio_play(audio_event event, float volume, float pitch);
void audio_stop(void);
void audio_emergency_stop(void);
bool audio_validate_assets(const char *argv0);

/* ---------- term.c ---------- */
bool term_init(int *outW, int *outH);
void term_present(const uint8_t *rgba, int w, int h);
int term_poll_key(void);
void term_shutdown(void);
void term_emergency_restore(void);

#endif
