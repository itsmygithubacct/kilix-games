/* Joustix: a flying-joust arcade game for Kitty-protocol terminals. */
#ifndef JOUSTIX_H
#define JOUSTIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty_keyboard.h"

#define LOGICAL_W 320.0f
#define LOGICAL_H 180.0f
#define LAVA_TOP  164.0f
#define TICK_DT   (1.0f / 60.0f)
#define MAX_ENEMIES 40
#define MAX_EGGS 40
#define MAX_PARTICLES 160
#define PLATFORM_COUNT 8

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

enum { GS_TITLE, GS_PLAYING, GS_WAVE, GS_PAUSED, GS_GAMEOVER };
enum { EN_BOUNDER, EN_HUNTER, EN_SHADOW, EN_TYPE_COUNT };
enum { GAMEOVER_RESTART, GAMEOVER_MENU, GAMEOVER_OPTION_COUNT };
enum {
    SFX_MENU, SFX_FLAP, SFX_STEP, SFX_LAND, SFX_JOUST, SFX_HURT, SFX_EGG,
    SFX_HATCH, SFX_WAVE, SFX_LAVA, SFX_COUNT
};

typedef struct {
    float x, y, w, h;
} Platform;

typedef struct {
    int w, h;
    uint32_t *px;
    bool ok;
} Bitmap;

typedef struct {
    float x, y, prev_y;
    float vx, vy;
    float spawn_timer, invuln, flap_anim, flap_cooldown, collide_cooldown;
    int dir;
    bool active, on_platform;
} Rider;

typedef struct {
    Rider rider;
    int type;
    float think_timer, flap_timer;
} Enemy;

typedef struct {
    float x, y, vx, vy, hatch_timer, collect_delay;
    int type;
    bool active, grounded;
} Egg;

typedef struct {
    float x, y, vx, vy, life, max_life;
    uint32_t color;
    bool active;
} Particle;

typedef struct {
    int state;
    int W, H;
    bool quit, headless, sound_on;
    uint32_t rng;
    uint64_t ticks;
    float scene_time;

    Rider player;
    Enemy enemies[MAX_ENEMIES];
    Egg eggs[MAX_EGGS];
    Particle particles[MAX_PARTICLES];
    Platform platforms[PLATFORM_COUNT];

    int wave, score, high_score, lives;
    int difficulty, gameover_choice;
    float wave_timer, respawn_timer, left_input, right_input;
    float shake, flash, message_timer, lava_troll_timer, lava_troll_phase;
    float step_sound_timer;
    int step_sound_side;
    float lava_troll_x;
    bool held_controls, held_left, held_right, held_flap;
    char message[80];
} GameState;

extern GameState G;

float clampf(float v, float lo, float hi);
float game_randf(void);
void asset_paths_init(void);
const char *asset_path(const char *relative_path);
void game_init(int w, int h, uint32_t seed);
void game_start(void);
void game_tick(void);
void game_handle_key(int key);
void game_set_held_controls(bool available, bool left, bool right, bool flap);
void game_autopilot(void);
bool game_validate(char *error, size_t error_len);
int game_active_enemies(void);
int game_active_eggs(void);

void render_init(int w, int h);
void render_resize(int w, int h);
void render_shutdown(void);
void render_frame(void);
uint8_t *render_fb(void);
bool render_dump_ppm(const char *path);
bool render_validate_assets(char *error, size_t error_len);

bool term_init(int *out_w, int *out_h);
bool term_check_resize(int *out_w, int *out_h);
void term_present(const uint8_t *rgba, int w, int h);
int term_read_input(void);
bool term_next_key_event(kittykb_event *event);
bool term_key_down(uint32_t key);
bool term_has_release_events(void);
void term_shutdown(void);
void term_emergency_restore(void);

bool sound_init(void);
void sound_shutdown(void);
void sound_play(int id, float volume, float pitch);
void sound_set_enabled(bool enabled);
bool sound_is_enabled(void);

#endif
