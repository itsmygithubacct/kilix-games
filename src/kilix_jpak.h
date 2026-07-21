/*
 * Kilix JPAK: Deep Salvage
 *
 * A clean-room, Kitty-protocol action-puzzle game.  Nothing in this header
 * depends on the historical game data that motivated the project.
 */
#ifndef KILIX_JPAK_H
#define KILIX_JPAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty_keyboard.h"

#define KJ_VERSION "0.1.0"

#define LOGICAL_W 512
#define LOGICAL_H 320
#define FIELD_COLS 26
#define FIELD_ROWS 16
#define TILE_SIZE 16
#define FIELD_X 8
#define FIELD_Y 42
#define FIELD_W (FIELD_COLS * TILE_SIZE)
#define FIELD_H (FIELD_ROWS * TILE_SIZE)
#define TICK_DT (1.0f / 60.0f)

#define CAMPAIGN_LEVELS 100
#define MAX_ENEMIES 20
#define MAX_PARTICLES 384

enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

enum {
    GS_TITLE,
    GS_LEVEL_SELECT,
    GS_HELP,
    GS_PLAYING,
    GS_PAUSED,
    GS_LEVEL_CLEAR,
    GS_LIFE_LOST,
    GS_GAMEOVER,
    GS_VICTORY
};

/* These are semantic cells, not atlas indices.  Each is drawn from native
 * primitives in render.c and has one explicit behavior in game.c. */
enum {
    T_EMPTY,
    T_PANEL,
    T_PANEL_DARK,
    T_STONE,
    T_LADDER,
    T_CRYSTAL,
    T_FUEL,
    T_CHARGER,
    T_DRAIN,
    T_COIN,
    T_RELIC,
    T_LIFE,
    T_STUN,
    T_SHIELD,
    T_SPIKES,
    T_ICE,
    T_MOSS,
    T_CONVEYOR_LEFT,
    T_CONVEYOR_RIGHT,
    T_TELEPORT_CYAN,
    T_TELEPORT_MAGENTA,
    T_TELEPORT_AMBER,
    T_SWITCH_CYAN,
    T_SWITCH_MAGENTA,
    T_SWITCH_AMBER,
    T_BARRIER_CYAN,
    T_BARRIER_MAGENTA,
    T_BARRIER_AMBER,
    T_PHASE_GLASS,
    T_PHASE_DENSE,
    T_PHASE_STEEL,
    T_EXIT,
    T_CRYSTAL_EMPTY,
    TILE_KIND_COUNT
};

enum {
    EN_TRACKER,
    EN_ROLLER,
    EN_POGO,
    EN_DART,
    EN_SHARD,
    EN_BLINKER,
    EN_WISP,
    EN_WING,
    ENEMY_KIND_COUNT
};

enum {
    SFX_MENU,
    SFX_JET,
    SFX_CRYSTAL,
    SFX_PICKUP,
    SFX_PHASE,
    SFX_TELEPORT,
    SFX_SWITCH,
    SFX_EXIT,
    SFX_HIT,
    SFX_STUN,
    SFX_CLEAR,
    SFX_LIFE,
    SFX_GAMEOVER,
    SFX_VICTORY,
    SFX_COUNT
};

enum {
    PARTICLE_SPARK,
    PARTICLE_THRUST,
    PARTICLE_DUST,
    PARTICLE_SHARD,
    PARTICLE_TEXT
};

typedef struct {
    uint8_t kind;
    uint8_t x, y;
} EnemySpawn;

typedef struct {
    uint8_t tiles[FIELD_ROWS][FIELD_COLS];
    EnemySpawn enemies[MAX_ENEMIES];
    int enemy_count;
    int spawn_x, spawn_y;
    int exit_x, exit_y;
    int theme;
    char title[40];
} LevelData;

typedef struct {
    float x, y, vx, vy;
    float fuel;
    float invulnerable;
    float shield;
    float stunner;
    float teleport_cooldown;
    float phase_pulse;
    float gait_phase;
    float gait_amount;
    int facing;
    int animation;
    bool grounded;
    bool on_ladder;
    bool thrusting;
    bool phasing;
} Player;

typedef struct {
    bool active;
    int kind;
    float x, y, vx, vy;
    float home_x, home_y;
    float timer;
    float stun;
    float phase;
    float alert;
    float tell;
    int direction;
} Enemy;

typedef struct {
    bool active;
    float x, y, vx, vy;
    float life, max_life, size;
    uint32_t color;
    int kind;
} Particle;

typedef struct {
    int state;
    int W, H;
    bool quit, headless, sound_on, practice_mode;
    uint32_t rng;
    uint64_t ticks;
    float scene_time;

    LevelData level_data;
    uint16_t phase_time[FIELD_ROWS][FIELD_COLS];
    Player player;
    Enemy enemies[MAX_ENEMIES];
    Particle particles[MAX_PARTICLES];

    int level;
    int selected_level;
    int unlocked_level;
    int score;
    int high_score;
    int saved_high_score;
    int lives;
    int crystals_remaining;
    int menu_choice;
    int help_page;
    int help_return_state;
    int level_start_score;
    int clear_time_bonus;
    int clear_fuel_bonus;
    bool barriers_open[3];
    bool exit_open;

    float state_timer;
    float level_time;
    float flash;
    float shake;
    float banner_timer;
    char banner[64];
    char death_reason[40];

    bool held_controls;
    bool held_left, held_right, held_up, held_down;
    bool held_jet, held_phase;
    float left_latch, right_latch, up_latch, down_latch;
    float jet_latch, phase_latch;
    bool scripted_input;
} GameState;

extern GameState G;

/* data.c */
void level_build(int level_index, LevelData *out);
int level_enemy_budget(int level_index);
bool level_validate(const LevelData *level, char *error, size_t error_len);
bool level_validate_campaign(char *error, size_t error_len);
const char *tile_name(int tile);
const char *enemy_name(int kind);

/* game.c */
float clampf(float value, float low, float high);
float game_randf(void);
void game_init(int width, int height, uint32_t seed);
void game_shutdown(void);
void game_start(int level);
void game_load_level(int level, bool fresh_life);
void game_tick(void);
void game_handle_key(int key);
void game_set_held_controls(bool available, bool left, bool right,
                            bool up, bool down, bool jet, bool phase);
void game_autopilot(void);
bool game_validate(char *error, size_t error_len);
void game_force_level_clear(void);
void game_force_life_lost(void);
bool game_tile_solid(int tx, int ty);

/* render.c */
bool render_init(int width, int height);
bool render_resize(int width, int height);
void render_shutdown(void);
void render_frame(void);
uint8_t *render_fb(void);
bool render_dump_ppm(const char *path);

/* term.c */
bool term_init(int *out_width, int *out_height);
bool term_check_resize(int *out_width, int *out_height);
void term_present(const uint8_t *rgba, int width, int height);
int term_read_input(void);
bool term_next_key_event(kittykb_event *event);
bool term_key_down(uint32_t key);
bool term_has_release_events(void);
void term_shutdown(void);
void term_emergency_restore(void);

/* sound.c */
bool sound_init(void);
void sound_shutdown(void);
void sound_set_enabled(bool enabled);
bool sound_is_enabled(void);
void sound_play(int id, float volume, float pitch);
void sound_jet(bool active, float intensity);

#endif
