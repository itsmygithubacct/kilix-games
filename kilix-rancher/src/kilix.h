/*
 * Kilix Rancher - an original creature-raising game for Kitty terminals.
 *
 * The presentation layer is inspired by the software-framebuffer structure
 * used by Chess Bash.  Game rules, characters, writing and visual assets are
 * original to this project.
 */
#ifndef KILIX_H
#define KILIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define KILIX_RANCHER_VERSION "0.1.8"

#define TICK_DT (1.0f / 60.0f)
#define STAT_COUNT 6
#define DRILL_COUNT 6
#define ITEM_COUNT 4
#define MOVE_COUNT 4
#define OPPONENT_COUNT 6
#define RANK_COUNT 6
#define JOURNAL_PAGES 6   /* field-guide entries; matches JOURNAL_ENTRIES */

/* Memory drill (Bell-and-Ember) reveal cadence, shared by the tick that
 * advances the sequence and the renderer that lights the sparks. Each symbol
 * owns a MEMO_SLOT window but is only lit for MEMO_LIT of it, leaving a dark
 * gap so two identical symbols in a row read as two distinct flashes. */
#define MEMO_SLOT 0.62f
#define MEMO_LIT  0.40f

/* Heart drill (rhythm): a steady heartbeat the player taps along to. Beats fall
 * at a fixed tempo and the bell swells for RHY_APPROACH seconds beforehand, so
 * every beat is anticipated rather than a surprise. A tap within RHY_WINDOW of
 * the beat scores by closeness; taps outside are ignored (no penalty). Shared
 * by the tick, the input handler, and the renderer so all three agree. */
#define RHY_BEATS    5
#define RHY_FIRST    1.0f    /* first beat, seconds into the round */
#define RHY_PERIOD   0.80f   /* steady interval between beats */
#define RHY_WINDOW   0.28f   /* max tap offset that still scores */
#define RHY_APPROACH 0.80f   /* how long before a beat the bell swells */

/* Ranch economy. Rent is 1000 g every 4-week month, paid up front, first month
 * free; missing it is an eviction (game over). Hunger climbs every week and must
 * be fed down with food, or a starving Kilix loses stats and form. */
#define MONTH_WEEKS   4
#define RENT_AMOUNT   1000
#define BANK_ACTIONS  6     /* deposit 100/500/all, withdraw 100/500/all */
#define HUNGER_PER_WEEK 14
#define HUNGER_WARN   75    /* peckish: a small weekly toll */
#define HUNGER_STARVE 90    /* starving: stats and form decay each week */

/* Paid coaching (Academy): a guaranteed stat gain for gold and one week, shown
 * as a short training montage before the result. */
#define ACADEMY_COST  300
#define ACADEMY_GAIN  9
#define ACADEMY_ANIM  2.2f  /* seconds of training animation */

enum {
    KEY_ENTER = 1000,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESC,
    KEY_UP,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT
};

typedef enum {
    SCREEN_TITLE,
    SCREEN_NAMING,
    SCREEN_RANCH,
    SCREEN_TRAINING,
    SCREEN_CARE,
    SCREEN_ARENA,
    SCREEN_BATTLE,
    SCREEN_EVENT,
    SCREEN_JOURNAL,
    SCREEN_CREDITS,
    SCREEN_CHAMPION,
    SCREEN_DRILL,
    SCREEN_ACADEMY,   /* paid coaching: buy a guaranteed stat gain */
    SCREEN_DOJO,      /* learn battle moves for gold */
    SCREEN_BANK       /* set gold aside so rent is always covered */
} Screen;

/* One skill mini-game per drill, chosen by the drill's primary stat. */
typedef enum {
    MG_TIMING,     /* Power/Claw   — stop the sweep in the sweet spot */
    MG_REACTION,   /* Speed/Agility — press the instant the cue flashes */
    MG_MEMORY,     /* Skill/Focus  — repeat the ember sequence */
    MG_MASH,       /* Intellect/Flame — mash to fill the flame gauge */
    MG_HOLD,       /* Defense/Guard — keep the drifting marker centered */
    MG_RHYTHM,     /* Life/Heart   — tap each beat on time */
    MG_TYPE_COUNT
} MinigameType;

typedef struct {
    int drill;                 /* which DRILLS[] entry */
    MinigameType type;
    int phase;                 /* 0 intro, 1 playing, 2 finished */
    float clock;               /* seconds in the current phase */
    int round;                 /* current round (0-based) */
    int rounds;                /* rounds this game */
    float quality;             /* accumulated performance in [0,1] */
    float feedback;            /* hit/miss flash timer */

    float marker;              /* timing/hold sweep position [0,1] */
    float marker_vel;          /* sweep velocity/direction */
    float target;              /* sweet-spot / hold center [0,1] */
    float half;                /* sweet-spot / hold half-width */

    float cue_at;              /* reaction: when the cue fires */
    bool cue_live;             /* reaction/rhythm: window is open */

    int seq[10];               /* memory: the shown sequence */
    int seq_len;               /* memory: length this round */
    int seq_pos;               /* memory: playback/input index */
    bool showing;              /* memory: still displaying the sequence */

    int taps;                  /* mash presses / rhythm hits */
    int taps_target;           /* mash goal */

    char banner[48];           /* per-round feedback text */
} MinigameState;

typedef enum {
    STAT_LIFE,
    STAT_POWER,
    STAT_INTELLECT,
    STAT_DEFENSE,
    STAT_SPEED,
    STAT_SKILL
} StatKind;

typedef enum {
    SEASON_BLOOM,
    SEASON_SUN,
    SEASON_HARVEST,
    SEASON_FROST
} Season;

typedef enum {
    EVENT_NONE,
    EVENT_TRAIN,
    EVENT_REST,
    EVENT_FEED,
    EVENT_BATTLE_RESULT,
    EVENT_RANK_UP,
    EVENT_RENT,        /* the monthly rent collector calls */
    EVENT_EVICTION     /* rent went unpaid: the run is over */
} EventKind;

typedef enum {
    BATTLE_READY,
    BATTLE_ACTIVE,
    BATTLE_PLAYER_ATTACK,
    BATTLE_ENEMY_ATTACK,
    BATTLE_FINISHED
} BattlePhase;

typedef enum {
    SFX_MOVE,
    SFX_CONFIRM,
    SFX_TRAIN,
    SFX_HIT,
    SFX_WIN,
    SFX_LOSE,
    SFX_COUNT
} SoundId;

typedef struct {
    int value[STAT_COUNT];
} Stats;

typedef struct {
    char name[24];
    Stats stats;
    int fatigue;
    int stress;
    int bond;
    int form;
    int hunger;                /* 0 full .. 100 starving; climbs weekly */
    int age_weeks;
    int rank;
    int rank_wins;
    int total_wins;
    int total_losses;
    unsigned personality_seed;
} Monster;

typedef struct {
    const char *name;
    const char *subtitle;
    const char *description;
    StatKind primary;
    StatKind secondary;
    int min_gain;
    int max_gain;
    int fatigue;
    int stress;
    int success;
} DrillInfo;

typedef struct {
    const char *name;
    const char *description;
    int cost;
    int fatigue;
    int stress;
    int bond;
    int form;
    int satiety;               /* how much the food lowers hunger (0 = not food) */
} ItemInfo;

typedef struct {
    const char *name;
    const char *description;
    int cost;                  /* Will (guts) spent to use the move in battle */
    int power;
    int accuracy;
    float min_range;
    float max_range;
    StatKind scaling;
    int price;                 /* gold to learn it; 0 = known from the start */
} MoveInfo;

typedef struct {
    const char *name;
    const char *species;
    const char *epithet;
    uint32_t color;
    uint32_t accent;
    int rank;
    Stats stats;
    int guts_rate;
    int prize;
} OpponentInfo;

typedef struct {
    EventKind kind;
    float timer;
    float duration;
    int index;
    bool success;
    bool great;
    StatKind primary;
    StatKind secondary;
    int gain_primary;
    int gain_secondary;
    int money_delta;
    char title[64];
    char detail[192];
} EventState;

typedef struct {
    BattlePhase phase;
    int opponent;
    float timer;
    float intro_timer;
    float player_hp;
    float enemy_hp;
    float player_max_hp;
    float enemy_max_hp;
    float player_guts;
    float enemy_guts;
    float distance;
    float player_cooldown;
    float enemy_cooldown;
    float phase_timer;
    float shake;
    float flash;
    int selected_move;
    int active_move;
    int last_damage;
    int winner;             /* -1 enemy, 0 draw/unset, 1 player */
    bool hit;
    bool autopilot;         /* auto-battle: the Kilix fights on its own */
    char callout[96];
} BattleState;

typedef struct {
    int screen;
    int previous_screen;
    int W, H;
    bool quit;
    bool headless;

    int cursor;
    int title_cursor;
    int drill_cursor;
    int care_cursor;
    int arena_cursor;
    int academy_cursor;        /* Academy: which stat to coach */
    int dojo_cursor;           /* Dojo: which move to learn */
    int bank_cursor;           /* Bank: which deposit/withdraw action */
    int academy_phase;         /* 0 menu, 1 playing the training montage */
    float academy_clock;       /* montage timer */
    int academy_choice;        /* the stat being coached during the montage */
    int journal_page;

    float time;
    float screen_time;
    float toast_timer;
    float autosave_flash;
    float ambient_phase;
    unsigned rng;

    int total_weeks;
    int money;
    int bank;                  /* rent savings; rent is drawn from here first */
    uint32_t moves_known;      /* bitmask of learned MOVES[]; bit 0 always set */
    int rent_paid_weeks;       /* rent covers up to this week; first month free */
    Monster kilix;
    EventState event;
    BattleState battle;
    MinigameState minigame;

    char name_input[24];
    int name_len;
    char toast[160];
    char save_path[512];
    bool save_exists;
    bool first_visit;
    bool sound_on;
    bool pending_overwrite;   /* naming screen: awaiting confirm to replace a save */
    bool save_failed;         /* last save attempt failed: show a sticky warning */
} GameState;

typedef struct {
    int w, h;
    uint32_t *px;
    bool ok;
} Bitmap;

extern GameState G;
extern const DrillInfo DRILLS[DRILL_COUNT];
extern const ItemInfo ITEMS[ITEM_COUNT];
extern const MoveInfo MOVES[MOVE_COUNT];
extern const OpponentInfo OPPONENTS[OPPONENT_COUNT];
extern const char *RANK_NAMES[RANK_COUNT];
extern const char *STAT_NAMES[STAT_COUNT];

/* Utility and asset paths. */
float clampf(float value, float low, float high);
int clampi(int value, int low, int high);
unsigned rng_next(void);
int rng_range(int low, int high);
void asset_paths_init(void);
const char *asset_path(const char *relative);

/* Game simulation. */
void game_init(unsigned seed, bool fresh);
void game_tick(float dt);
void game_handle_key(int key);
void game_new_ranch(const char *name);
void game_go_ranch(void);
void game_start_battle(int opponent);
void game_use_move(int move_index);
void game_show_toast(const char *message);
void game_advance_week(void);
bool game_save(void);
bool game_load(void);
bool game_delete_save(void);
int game_selftest(unsigned seed, int weeks);
Season game_season(void);
const char *game_season_name(void);
const char *game_condition(void);
int game_year(void);
int game_week_of_year(void);
int game_overall_rating(void);
float game_drill_condition(int drill_index);

/* Software renderer. */
bool render_init(int width, int height, char *error, size_t error_len);
void render_resize(int width, int height);
void render_shutdown(void);
void render_frame(void);
const uint8_t *render_fb(void);
bool render_dump_ppm(const char *path);
bool render_validate_assets(char *error, size_t error_len);

/* Platform layer (main.c drives the shared kilix-game-kit host). Whether the
 * most recent key delivered to game_handle_key was keyboard auto-repeat
 * rather than a fresh press — the mash drill counts only genuine presses. */
bool input_key_repeated(void);

bool sound_init(void);
void sound_play(SoundId id);
void sound_shutdown(void);

#endif
