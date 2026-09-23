#define _POSIX_C_SOURCE 200809L

#include "kilix.h"
#include "kilix_state.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * This file owns all game rules.  The renderer is deliberately kept on the
 * other side of kilix.h: a headless run and an interactive run therefore use
 * exactly the same calendar, growth, battle and persistence code.
 */

#define WEEKS_PER_YEAR 48
#define WEEKS_PER_SEASON 12
#define BATTLE_SECONDS 60.0f
#define SAVE_VERSION 3u
#define SAVE_MONEY_MAX 99999999
#define SAVE_WEEKS_MAX 10000000

GameState G;

const char *STAT_NAMES[STAT_COUNT] = {
    "Heart", "Claw", "Flame", "Guard", "Agility", "Focus"
};

const char *RANK_NAMES[RANK_COUNT] = {
    "Kindling", "Ember", "Flame", "Blaze", "Wildfire", "Crown"
};

const DrillInfo DRILLS[DRILL_COUNT] = {
    {
        "Hearthstone Hike", "Heart + Flame",
        "Carry warm hearthstones up the long hill without losing heart.",
        STAT_LIFE, STAT_INTELLECT, 5, 9, 16, 5, 84
    },
    {
        "Pawstone Press", "Claw + Guard",
        "Bat smooth stone weights across the yard, then brace to catch them.",
        STAT_POWER, STAT_DEFENSE, 5, 10, 20, 7, 78
    },
    {
        "Lantern Lesson", "Flame + Focus",
        "Stoke the paper lanterns to a blaze with fast bursts of whisker-fire.",
        STAT_INTELLECT, STAT_SKILL, 5, 9, 17, 6, 81
    },
    {
        "Cinder Shelter", "Guard + Heart",
        "Hold a padded shelter steady through a shower of harmless cinders.",
        STAT_DEFENSE, STAT_LIFE, 5, 9, 19, 6, 80
    },
    {
        "Firefly Dash", "Agility + Focus",
        "Chase clockwork fireflies through gates before their lights go dark.",
        STAT_SPEED, STAT_SKILL, 5, 10, 21, 8, 77
    },
    {
        "Bell-and-Ember", "Focus + Agility",
        "Watch the embers' colors, then ring the bells back in the same order.",
        STAT_SKILL, STAT_SPEED, 5, 9, 18, 7, 82
    }
};

const ItemInfo ITEMS[ITEM_COUNT] = {
    {
        "Hearth Stew", "A filling root-and-fish stew that restores a tired Kilix.",
        90, -10, -5, 4, 8, 55
    },
    {
        "Moonmint Brush", "A cool grooming session that eases worry and builds trust.",
        70, -4, -18, 7, 4, 0
    },
    {
        "Coolleaf Tonic", "A ranch remedy for heavy paws and an overheated flame.",
        130, -25, -9, 2, 10, 18
    },
    {
        "Starcoal Treat", "A rare sparkling morsel that brightens form and friendship.",
        180, 2, -12, 10, 15, 35
    }
};

const MoveInfo MOVES[MOVE_COUNT] = {
    {
        "Coal Claw", "A quick close-range swipe with a glowing paw.",
        18, 22, 91, 0.0f, 34.0f, STAT_POWER, 0
    },
    {
        "Ember Pounce", "A springing tackle that crosses a short gap.",
        24, 31, 83, 18.0f, 58.0f, STAT_POWER, 500
    },
    {
        "Whisker Flare", "A curved tongue of fire cast across the arena.",
        31, 40, 77, 43.0f, 82.0f, STAT_INTELLECT, 1100
    },
    {
        "Little Sunrise", "Kilix gathers its courage into one far-flying spark.",
        40, 53, 69, 68.0f, 100.0f, STAT_INTELLECT, 2000
    }
};

const OpponentInfo OPPONENTS[OPPONENT_COUNT] = {
    {
        "Pip", "Mossnub", "the Dewy-Tusked", 0xff70a46bu, 0xffd8f0a1u,
        0, {{40, 35, 30, 34, 42, 34}}, 7, 450
    },
    {
        "Plip", "Dewdrop", "the Wobbly", 0xffa37c5bu, 0xffead09au,
        1, {{55, 57, 42, 61, 43, 50}}, 8, 700
    },
    {
        "Nim", "Mistwing", "the Vanishing", 0xff7aa9c9u, 0xffd5efffu,
        2, {{68, 62, 72, 57, 83, 78}}, 9, 1050
    },
    {
        "Cairnox", "Stonecalf", "the Noon Wall", 0xffd99a45u, 0xffffe09au,
        3, {{91, 89, 98, 86, 91, 92}}, 10, 1500
    },
    {
        "Lunabelle", "Moonmoth", "the Last Gleam", 0xff55485fu, 0xffdf6c44u,
        4, {{119, 127, 114, 121, 132, 118}}, 11, 2200
    },
    {
        "Umbraze", "Duskcub", "Keeper of the Crown", 0xffd85b38u, 0xffffd76au,
        5, {{158, 162, 174, 154, 166, 171}}, 13, 3500
    }
};

static bool suppress_autosave;

typedef struct {
    uint32_t version;
    uint32_t checksum;
    uint32_t rng;
    int32_t total_weeks;
    int32_t money;
    char name[24];
    int32_t stats[STAT_COUNT];
    int32_t fatigue;
    int32_t stress;
    int32_t bond;
    int32_t form;
    int32_t age_weeks;
    int32_t rank;
    int32_t rank_wins;
    int32_t total_wins;
    int32_t total_losses;
    uint32_t personality_seed;
    uint32_t sound_on;
    uint32_t reserved[4];
} SaveData;

static const unsigned char SAVE_MAGIC[8] = {
    'K', 'I', 'L', 'I', 'X', 'S', 'V', '2'
};

static void change_screen(Screen screen);
static void normalize_monster(Monster *monster);
static void normalize_game(void);
static void begin_event(EventKind kind, const char *title, const char *detail);
static void perform_drill(int index);
static void perform_rest(void);
static void use_care_item(int index);
static void academy_finish(void);
static bool present_rent_event(void);
static void battle_tick(float dt);
static void settle_battle(void);

static void play_sound(SoundId sound)
{
    if (G.sound_on && !G.headless)
        sound_play(sound);
}

static bool file_exists(const char *path)
{
    kilixstate_options options;
    kilixstate_store store;
    size_t required = 0u;
    kilixstate_result result;

    if (!path || !path[0])
        return false;
    kilixstate_options_init(&options);
    options.absolute_path = path;
    options.max_payload = sizeof(SAVE_MAGIC) + sizeof(SaveData);
    options.format = KILIXSTATE_FORMAT_RAW;
    if (kilixstate_store_init(&store, &options) != KILIXSTATE_OK)
        return false;
    result = kilixstate_load(&store, NULL, 0u, &required);
    kilixstate_store_close(&store);
    return result != KILIXSTATE_NOT_FOUND;
}

static bool save_store_init(kilixstate_store *store)
{
    kilixstate_options options;

    if (!G.save_path[0]) return false;
    kilixstate_options_init(&options);
    options.absolute_path = G.save_path;
    options.max_payload = sizeof(SAVE_MAGIC) + sizeof(SaveData);
    /* Preserve the KILIXSV2 + SaveData format for existing ranches. */
    options.format = KILIXSTATE_FORMAT_RAW;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static void choose_save_path(char *out, size_t out_size)
{
    const char *override_path = getenv("KILIX_RANCHER_SAVE");
    const char *home = getenv("HOME");

    if (!out || out_size == 0)
        return;
    if (override_path && override_path[0])
        snprintf(out, out_size, "%s", override_path);
    else if (home && home[0])
        snprintf(out, out_size, "%s/.kilix-rancher.save", home);
    else
        snprintf(out, out_size, "%s", "kilix-rancher.save");
    out[out_size - 1] = '\0';
}

static int wrap_index(int value, int count)
{
    if (count <= 0)
        return 0;
    while (value < 0)
        value += count;
    while (value >= count)
        value -= count;
    return value;
}

static bool is_confirm_key(int key)
{
    return key == KEY_ENTER || key == '\r' || key == '\n' || key == ' ';
}

/* Enter specifically (no Space). Used where a Space-driven screen hands off to
 * a result the player must actually see — accepting Space there would let a
 * still-pressed Space skip it unread. */
static bool is_enter_key(int key)
{
    return key == KEY_ENTER || key == '\r' || key == '\n';
}

static int lower_key(int key)
{
    if (key >= 0 && key <= UCHAR_MAX)
        return tolower((unsigned char)key);
    return key;
}

static void copy_text(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0)
        return;
    snprintf(out, out_size, "%s", text ? text : "");
    out[out_size - 1] = '\0';
}

static void clean_name(char out[24], const char *name)
{
    size_t used = 0;
    bool pending_space = false;
    const unsigned char *p = (const unsigned char *)(name ? name : "");

    while (*p && isspace(*p))
        ++p;
    while (*p && used < 23) {
        if (isspace(*p)) {
            pending_space = used > 0;
        } else if (isprint(*p)) {
            if (pending_space && used < 23)
                out[used++] = ' ';
            pending_space = false;
            if (used < 23)
                out[used++] = (char)*p;
        }
        ++p;
    }
    if (used == 0) {
        memcpy(out, "Ember", 6);
        return;
    }
    out[used] = '\0';
}

static void change_screen(Screen screen)
{
    if (G.screen != (int)screen) {
        G.previous_screen = G.screen;
        G.toast[0] = '\0';
        G.toast_timer = 0.0f;
    }
    G.screen = screen;
    G.screen_time = 0.0f;

    switch (screen) {
    case SCREEN_TITLE:
        G.title_cursor = clampi(G.title_cursor, 0, 3);
        G.cursor = G.title_cursor;
        break;
    case SCREEN_RANCH:
        G.cursor = 0;
        break;
    case SCREEN_TRAINING:
        G.drill_cursor = clampi(G.drill_cursor, 0, DRILL_COUNT - 1);
        G.cursor = G.drill_cursor;
        break;
    case SCREEN_CARE:
        G.care_cursor = clampi(G.care_cursor, 0, ITEM_COUNT - 1);
        G.cursor = G.care_cursor;
        break;
    case SCREEN_ARENA:
        G.arena_cursor = clampi(G.arena_cursor, 0, OPPONENT_COUNT - 1);
        G.cursor = G.arena_cursor;
        break;
    case SCREEN_ACADEMY:
        G.academy_cursor = clampi(G.academy_cursor, 0, STAT_COUNT - 1);
        G.cursor = G.academy_cursor;
        break;
    case SCREEN_DOJO:
        G.dojo_cursor = clampi(G.dojo_cursor, 0, MOVE_COUNT - 1);
        G.cursor = G.dojo_cursor;
        break;
    case SCREEN_BANK:
        G.bank_cursor = clampi(G.bank_cursor, 0, BANK_ACTIONS - 1);
        G.cursor = G.bank_cursor;
        break;
    default:
        break;
    }
}

void game_show_toast(const char *message)
{
    copy_text(G.toast, sizeof(G.toast), message);
    G.toast_timer = 3.0f;
}

static void normalize_monster(Monster *monster)
{
    int i;

    if (!monster)
        return;
    monster->name[sizeof(monster->name) - 1] = '\0';
    if (!monster->name[0])
        copy_text(monster->name, sizeof(monster->name), "Ember");
    for (i = 0; i < STAT_COUNT; ++i)
        monster->stats.value[i] = clampi(monster->stats.value[i], 1, 999);
    monster->fatigue = clampi(monster->fatigue, 0, 100);
    monster->stress = clampi(monster->stress, 0, 100);
    monster->bond = clampi(monster->bond, 0, 100);
    monster->form = clampi(monster->form, 0, 100);
    monster->hunger = clampi(monster->hunger, 0, 100);
    monster->age_weeks = clampi(monster->age_weeks, 0, SAVE_WEEKS_MAX);
    monster->rank = clampi(monster->rank, 0, RANK_COUNT - 1);
    monster->rank_wins = clampi(monster->rank_wins, 0, SAVE_WEEKS_MAX);
    monster->total_wins = clampi(monster->total_wins, 0, SAVE_WEEKS_MAX);
    monster->total_losses = clampi(monster->total_losses, 0, SAVE_WEEKS_MAX);
    if (monster->personality_seed == 0)
        monster->personality_seed = 0x51f15e5du;
}

static void normalize_game(void)
{
    normalize_monster(&G.kilix);
    G.total_weeks = clampi(G.total_weeks, 0, SAVE_WEEKS_MAX);
    G.money = clampi(G.money, 0, SAVE_MONEY_MAX);
    G.bank = clampi(G.bank, 0, SAVE_MONEY_MAX);
    if (G.kilix.age_weeks != G.total_weeks)
        G.kilix.age_weeks = G.total_weeks;
    if (G.rng == 0)
        G.rng = 0x6d2b79f5u;
}

void game_init(unsigned seed, bool fresh)
{
    memset(&G, 0, sizeof(G));
    G.W = 960;
    G.H = 540;
    G.screen = SCREEN_TITLE;
    G.previous_screen = SCREEN_TITLE;
    G.rng = seed ? seed : 0x6d2b79f5u;
    G.sound_on = true;
    G.first_visit = true;
    G.battle.opponent = -1;
    G.battle.active_move = -1;
    choose_save_path(G.save_path, sizeof(G.save_path));
    G.save_exists = fresh ? false : file_exists(G.save_path);
}

void game_new_ranch(const char *name)
{
    Monster monster;
    unsigned personality;

    memset(&monster, 0, sizeof(monster));
    clean_name(monster.name, name);
    monster.stats.value[STAT_LIFE] = 48;
    monster.stats.value[STAT_POWER] = 43;
    monster.stats.value[STAT_INTELLECT] = 52;
    monster.stats.value[STAT_DEFENSE] = 39;
    monster.stats.value[STAT_SPEED] = 47;
    monster.stats.value[STAT_SKILL] = 44;
    monster.fatigue = 8;
    monster.stress = 5;
    monster.bond = 45;
    monster.form = 72;
    monster.hunger = 20;
    monster.rank = 0;
    personality = rng_next();
    monster.personality_seed = personality ? personality : 0x51f15e5du;

    G.kilix = monster;
    G.total_weeks = 0;
    G.money = 1200;
    G.bank = 0;
    G.moves_known = 1u;                 /* only the basic Coal Claw to start */
    G.rent_paid_weeks = MONTH_WEEKS;    /* the first month is on the house */
    memset(&G.event, 0, sizeof(G.event));
    memset(&G.battle, 0, sizeof(G.battle));
    G.battle.opponent = -1;
    G.battle.active_move = -1;
    G.first_visit = true;
    G.drill_cursor = 0;
    G.care_cursor = 0;
    G.arena_cursor = 0;
    G.journal_page = 0;
    G.cursor = 0;
    change_screen(SCREEN_RANCH);
    game_show_toast("A little fire-kitten has arrived at your ranch.");
    if (!suppress_autosave)
        game_save();
}

Season game_season(void)
{
    int week = clampi(G.total_weeks, 0, INT_MAX) % WEEKS_PER_YEAR;
    return (Season)(week / WEEKS_PER_SEASON);
}

const char *game_season_name(void)
{
    static const char *names[] = {
        "Bloomtide", "Highsun", "Harvestglow", "Frostfall"
    };
    return names[(int)game_season()];
}

int game_year(void)
{
    return G.total_weeks / WEEKS_PER_YEAR + 1;
}

int game_week_of_year(void)
{
    return G.total_weeks % WEEKS_PER_YEAR + 1;
}

int game_overall_rating(void)
{
    int i;
    int total = 0;

    for (i = 0; i < STAT_COUNT; ++i)
        total += G.kilix.stats.value[i];
    return total / STAT_COUNT;
}

const char *game_condition(void)
{
    if (G.kilix.fatigue >= 88)
        return "Exhausted";
    if (G.kilix.stress >= 85)
        return "Overwhelmed";
    if (G.kilix.form <= 24)
        return "Flame-faint";
    if (G.kilix.fatigue >= 65)
        return "Tired";
    if (G.kilix.stress >= 60)
        return "Restless";
    if (G.kilix.form >= 88 && G.kilix.bond >= 75)
        return "Radiant";
    if (G.kilix.form >= 72)
        return "Bright-eyed";
    return "Steady";
}

void game_advance_week(void)
{
    if (G.total_weeks >= SAVE_WEEKS_MAX)
        return;

    ++G.total_weeks;
    G.kilix.age_weeks = G.total_weeks;

    /* Ordinary ranch life has a small cost even during a productive week. */
    G.kilix.stress += 1;
    if (G.kilix.fatigue >= 82)
        G.kilix.form -= 5;
    else if (G.kilix.stress >= 75)
        G.kilix.form -= 4;
    else if (G.kilix.fatigue <= 30 && G.kilix.stress <= 35)
        G.kilix.form += 1;

    /* Appetite grows every week. A starving Kilix (fed too little) actively
     * loses condition and stats; a merely peckish one just frets a little. */
    G.kilix.hunger += HUNGER_PER_WEEK;
    if (G.kilix.hunger >= HUNGER_STARVE) {
        int s;
        for (s = 0; s < STAT_COUNT; ++s)
            G.kilix.stats.value[s] -= 1;
        G.kilix.form -= 6;
        G.kilix.bond -= 3;
    } else if (G.kilix.hunger >= HUNGER_WARN) {
        G.kilix.form -= 2;
    }

    if (G.total_weeks % WEEKS_PER_SEASON == 0)
        G.kilix.bond += 1;
    if (G.total_weeks % WEEKS_PER_YEAR == 0)
        G.kilix.form += 3;

    normalize_game();
    /* Weekly autosave. game_save records G.save_failed on failure, which the
     * renderer surfaces as a sticky warning; a plain toast here would be wiped
     * by the screen change every caller performs right after. */
    if (!suppress_autosave)
        game_save();
}

static void begin_event(EventKind kind, const char *title, const char *detail)
{
    memset(&G.event, 0, sizeof(G.event));
    G.event.kind = kind;
    G.event.duration = 2.2f;
    copy_text(G.event.title, sizeof(G.event.title), title);
    copy_text(G.event.detail, sizeof(G.event.detail), detail);
    change_screen(SCREEN_EVENT);
}

/* ---- drill mini-games --------------------------------------------------- */

/* Each drill's primary stat picks its mini-game. */
static MinigameType drill_minigame(int index)
{
    switch (DRILLS[index].primary) {
    case STAT_POWER:     return MG_TIMING;
    case STAT_SPEED:     return MG_REACTION;
    case STAT_SKILL:     return MG_MEMORY;
    case STAT_INTELLECT: return MG_MASH;
    case STAT_DEFENSE:   return MG_HOLD;
    case STAT_LIFE:      return MG_RHYTHM;
    default:             return MG_TIMING;
    }
}

/* The condition-dependent part of a drill's outcome, shared by the headless
 * success roll and the interactive score multiplier (and the training screen,
 * which shows the multiplier). */
static int drill_chance(const DrillInfo *drill)
{
    int chance = drill->success;
    chance += G.kilix.stats.value[STAT_SKILL] / 35;
    chance += G.kilix.bond / 18;
    chance += (G.kilix.form - 50) / 9;
    chance -= G.kilix.fatigue / 4;
    chance -= G.kilix.stress / 5;
    return clampi(chance, 12, 96);
}

/* Interactive drills keep the player's mini-game score but scale it by
 * condition: a rested, bonded Kilix in form keeps (or slightly beats) its
 * earned score, an exhausted or stressed one can at worst halve it — so the
 * catnap/care loop matters in interactive play exactly as the training screen
 * promises. */
float game_drill_condition(int drill_index)
{
    const DrillInfo *drill = &DRILLS[clampi(drill_index, 0, DRILL_COUNT - 1)];
    return clampf((float)drill_chance(drill) / drill->success, 0.5f, 1.15f);
}

/* Apply a drill's outcome and open its result event. `quality` in [0,1] drives
 * the interactive path; the deterministic path (headless render-test and the
 * scripted selftest) draws the same three RNG rolls as before so growth stays
 * reproducible. */
static void resolve_drill(int index, float quality, bool use_rng)
{
    const DrillInfo *drill = &DRILLS[index];
    int primary_gain, secondary_gain, gain_roll;
    bool success, great;
    char detail[192];

    if (use_rng) {
        int success_roll = rng_range(1, 100);
        gain_roll = rng_range(drill->min_gain, drill->max_gain);
        int great_roll = rng_range(1, 100);
        success = success_roll <= drill_chance(drill);
        great = success && great_roll <= clampi(5 + G.kilix.bond / 5, 5, 30);
    } else {
        quality = clampf(clampf(quality, 0.0f, 1.0f) *
                         game_drill_condition(index), 0.0f, 1.0f);
        success = quality >= 0.30f;
        great = quality >= 0.82f;
        gain_roll = drill->min_gain +
            (int)lroundf((drill->max_gain - drill->min_gain) * quality);
    }

    if (success) {
        primary_gain = gain_roll + (great ? 3 : 0);
        secondary_gain = clampi((gain_roll + 1) / 2 + (great ? 1 : 0), 2, 7);
        G.kilix.bond += great ? 3 : 1;
        G.kilix.form += great ? 3 : 1;
    } else {
        primary_gain = 1;
        secondary_gain = 0;
        G.kilix.stress += 4;
        G.kilix.form -= 3;
    }

    G.kilix.stats.value[drill->primary] += primary_gain;
    G.kilix.stats.value[drill->secondary] += secondary_gain;
    G.kilix.fatigue += drill->fatigue;
    G.kilix.stress += drill->stress;
    normalize_game();
    game_advance_week();

    if (great)
        snprintf(detail, sizeof(detail),
                 "A brilliant run! %s +%d and %s +%d. The two of you found a new rhythm.",
                 STAT_NAMES[drill->primary], primary_gain,
                 STAT_NAMES[drill->secondary], secondary_gain);
    else if (success)
        snprintf(detail, sizeof(detail), "%s +%d and %s +%d. A good week's work.",
                 STAT_NAMES[drill->primary], primary_gain,
                 STAT_NAMES[drill->secondary], secondary_gain);
    else
        snprintf(detail, sizeof(detail),
                 "The lesson did not click this week, but %s still grew by %d.",
                 STAT_NAMES[drill->primary], primary_gain);

    begin_event(EVENT_TRAIN, drill->name, detail);
    G.event.index = index;
    G.event.success = success;
    G.event.great = great;
    G.event.primary = drill->primary;
    G.event.secondary = drill->secondary;
    G.event.gain_primary = primary_gain;
    G.event.gain_secondary = secondary_gain;
    play_sound(SFX_TRAIN);
}

/* Reset the per-round state for the round-based games (the continuous games —
 * mash, hold, rhythm — run off one continuous clock and ignore this). */
static void mg_begin_round(MinigameState *m)
{
    m->clock = 0.0f;
    m->cue_live = false;
    m->showing = false;
    m->seq_pos = 0;
    switch (m->type) {
    case MG_TIMING:
        m->marker = 0.0f;
        m->marker_vel = 0.85f + 0.28f * m->round;   /* faster each round */
        m->target = 0.5f;
        m->half = 0.12f;
        break;
    case MG_REACTION:
        /* Varied but input-independent delay so there is no fixed rhythm. */
        m->cue_at = 0.8f + 0.5f * ((m->round * 7 + 3) % 5) / 4.0f;
        break;
    case MG_MEMORY:
        m->seq_len = clampi(3 + m->round, 1, 9);
        for (int i = 0; i < m->seq_len; i++)
            m->seq[i] = rng_range(0, 3);
        m->showing = true;
        break;
    default:
        break;
    }
}

static void enter_drill(int index)
{
    MinigameState *m = &G.minigame;
    memset(m, 0, sizeof(*m));
    m->drill = index;
    m->type = drill_minigame(index);
    m->phase = 0;                 /* intro / get ready */
    m->clock = 0.0f;
    switch (m->type) {
    case MG_TIMING:   m->rounds = 3; break;
    case MG_REACTION: m->rounds = 3; break;
    case MG_MEMORY:   m->rounds = 3; break;
    /* 20 taps in 4 s: a full gauge takes brisk but honest mashing now that
     * auto-repeat no longer counts (26 assumed some repeat inflation). */
    case MG_MASH:     m->rounds = 1; m->taps_target = 20; break;
    case MG_HOLD:     m->rounds = 1; m->marker = 0.5f; m->target = 0.5f;
                      m->half = 0.15f; break;
    case MG_RHYTHM:   m->rounds = RHY_BEATS; break;
    default:          m->rounds = 1; break;
    }
    change_screen(SCREEN_DRILL);
    play_sound(SFX_CONFIRM);
}

static void perform_drill(int index)
{
    if (index < 0 || index >= DRILL_COUNT) {
        game_show_toast("That drill is not in the ranch ledger.");
        return;
    }
    /* Headless render-test and the scripted selftest resolve immediately and
     * deterministically; interactive play runs the mini-game. */
    if (G.headless || suppress_autosave)
        resolve_drill(index, 0.0f, true);
    else
        enter_drill(index);
}

static void mg_finish(MinigameState *m)
{
    m->quality = clampf(m->quality / (m->rounds > 0 ? m->rounds : 1), 0.0f, 1.0f);
    m->phase = 2;
    m->clock = 0.0f;
    if (m->quality >= 0.82f)      copy_text(m->banner, sizeof(m->banner), "BRILLIANT!");
    else if (m->quality >= 0.55f) copy_text(m->banner, sizeof(m->banner), "GREAT WORK");
    else if (m->quality >= 0.30f) copy_text(m->banner, sizeof(m->banner), "NICE TRY");
    else                          copy_text(m->banner, sizeof(m->banner), "KEEP AT IT");
    play_sound(m->quality >= 0.55f ? SFX_WIN : SFX_MOVE);
}

/* Advance the rhythm game one beat WITHOUT resetting the clock, so the tempo
 * stays steady across all beats (unlike mg_score_round, which restarts each
 * round). Quality accumulates and is averaged over the beats by mg_finish. */
static void rhythm_advance(MinigameState *m, float q, const char *tag)
{
    m->quality += clampf(q, 0.0f, 1.0f);
    m->feedback = 0.30f;
    m->cue_live = false;
    copy_text(m->banner, sizeof(m->banner), tag);
    play_sound(q >= 0.5f ? SFX_CONFIRM : SFX_MOVE);
    m->round++;
    if (m->round >= m->rounds)
        mg_finish(m);
}

/* Record a round's quality [0,1], flash feedback, and advance or finish. */
static void mg_score_round(MinigameState *m, float q, const char *tag)
{
    m->quality += clampf(q, 0.0f, 1.0f);
    m->feedback = 0.6f;
    copy_text(m->banner, sizeof(m->banner), tag);
    play_sound(q >= 0.6f ? SFX_CONFIRM : SFX_MOVE);
    m->round++;
    if (m->round >= m->rounds)
        mg_finish(m);
    else
        mg_begin_round(m);
}

static float triangle_wave(float t)   /* 0 -> 1 -> 0, period 2 */
{
    float p = fmodf(t, 2.0f);
    return p < 1.0f ? p : 2.0f - p;
}

/* Finish the two continuous, single-round games. Mash converts its tap count to
 * quality here; hold has already accumulated its in-zone fraction into
 * m->quality during the tick. */
static void mg_finish_mash_or_hold(MinigameState *m)
{
    if (m->type == MG_MASH)
        m->quality = clampf((float)m->taps /
                            (m->taps_target > 0 ? m->taps_target : 1),
                            0.0f, 1.0f);
    mg_finish(m);
}

static void minigame_tick(float dt)
{
    MinigameState *m = &G.minigame;
    m->clock += dt;
    if (m->feedback > 0.0f)
        m->feedback = clampf(m->feedback - dt, 0.0f, 1.0f);

    if (m->phase == 0) {                       /* get-ready: time to read */
        if (m->clock >= 2.4f) {
            m->phase = 1;
            m->clock = 0.0f;
            mg_begin_round(m);
        }
        return;
    }
    if (m->phase != 1)
        return;

    switch (m->type) {
    case MG_TIMING:
        m->marker = triangle_wave(m->clock * m->marker_vel);
        if (m->clock > 6.0f)                    /* dithered too long */
            mg_score_round(m, 0.15f, "TOO SLOW");
        break;
    case MG_REACTION:
        if (m->clock >= m->cue_at)
            m->cue_live = true;
        if (m->clock >= m->cue_at + 1.4f)       /* never pressed */
            mg_score_round(m, 0.1f, "TOO SLOW");
        break;
    case MG_MEMORY:
        if (m->showing) {
            /* reveal one symbol per slot (lit then a dark gap), then hand over */
            int shown = (int)(m->clock / MEMO_SLOT);
            if (shown >= m->seq_len) {
                m->showing = false;
                m->clock = 0.0f;
                m->seq_pos = 0;
            }
        } else if (m->clock > 6.0f) {           /* input timed out */
            mg_score_round(m, (float)m->seq_pos / m->seq_len, "TIME!");
        }
        break;
    case MG_MASH:
        if (m->clock >= 4.0f)
            mg_finish_mash_or_hold(m);          /* forward-declared below */
        break;
    case MG_HOLD: {
        float drift = sinf(m->clock * 1.7f) * 0.9f + sinf(m->clock * 0.55f) * 0.6f;
        m->marker = clampf(m->marker + drift * dt * 0.22f, 0.0f, 1.0f);
        if (fabsf(m->marker - m->target) < m->half)
            m->quality += dt / 5.0f;            /* fraction of 5 s in the zone */
        if (m->clock >= 5.0f)
            mg_finish_mash_or_hold(m);
        break;
    }
    case MG_RHYTHM: {
        float beat = RHY_FIRST + m->round * RHY_PERIOD;
        if (m->clock >= beat - RHY_WINDOW && m->clock <= beat + RHY_WINDOW)
            m->cue_live = true;                     /* window open — tap now */
        else if (m->clock > beat + RHY_WINDOW)      /* let this beat pass */
            rhythm_advance(m, 0.0f, "MISS");
        break;
    }
    default:
        break;
    }
}

static void minigame_key(int key)
{
    MinigameState *m = &G.minigame;
    if (key == KEY_ESC) {
        /* Mirrors the arena: backing out during the get-ready countdown is
         * free, but a drill already underway is forfeited — the week is spent
         * and the rounds played so far are the result. Anything else would
         * let the player discard a weak session (worst of all at the result
         * screen, where the exact score is visible) and reroll for free. */
        if (m->phase == 0) {
            game_go_ranch();
            change_screen(SCREEN_TRAINING);
            game_show_toast("Drill called off. No week was spent.");
        } else if (m->phase == 1) {
            if (m->type == MG_MASH)
                m->quality = (float)m->taps /
                             (m->taps_target > 0 ? m->taps_target : 1);
            else
                m->quality /= m->rounds > 0 ? m->rounds : 1;
            resolve_drill(m->drill, m->quality, false);
        } else {
            resolve_drill(m->drill, m->quality, false);
        }
        return;
    }
    if (m->phase == 0)                           /* let the countdown run */
        return;
    if (m->phase == 2) {                         /* result -> apply reward.
         * Enter only: the drills are played with SPACE, so accepting Space
         * here would let a held/mashed Space blow past the result unseen. */
        if (is_enter_key(key))
            resolve_drill(m->drill, m->quality, false);
        return;
    }

    switch (m->type) {
    case MG_TIMING:
        if (is_confirm_key(key)) {
            float off = fabsf(m->marker - m->target);
            float q = clampf(1.0f - off / 0.42f, 0.0f, 1.0f);
            mg_score_round(m, q, off < m->half ? "PERFECT!" : "GOOD");
        }
        break;
    case MG_REACTION:
        if (is_confirm_key(key)) {
            if (!m->cue_live)
                mg_score_round(m, 0.0f, "TOO EARLY");
            else {
                float rt = m->clock - m->cue_at;
                mg_score_round(m, clampf(1.0f - rt / 0.55f, 0.0f, 1.0f),
                               rt < 0.25f ? "SHARP!" : "OK");
            }
        }
        break;
    case MG_MEMORY:
        if (!m->showing && key >= '1' && key <= '4') {
            int val = key - '1';
            if (val == m->seq[m->seq_pos]) {
                m->seq_pos++;
                m->feedback = 0.3f;
                if (m->seq_pos >= m->seq_len)
                    mg_score_round(m, 1.0f, "PERFECT!");
            } else {
                mg_score_round(m, (float)m->seq_pos / m->seq_len, "MISSED");
            }
        }
        break;
    case MG_MASH:
        /* Physical presses only: holding the key down would otherwise pump
         * the gauge with auto-repeat events. */
        if (is_confirm_key(key) && !input_key_repeated()) {
            m->taps++;
            m->feedback = 0.12f;
        }
        break;
    case MG_HOLD:
        if (key == KEY_LEFT || lower_key(key) == 'a')
            m->marker = clampf(m->marker - 0.07f, 0.0f, 1.0f);
        else if (key == KEY_RIGHT || lower_key(key) == 'd')
            m->marker = clampf(m->marker + 0.07f, 0.0f, 1.0f);
        break;
    case MG_RHYTHM:
        if (is_confirm_key(key)) {
            float beat = RHY_FIRST + m->round * RHY_PERIOD;
            float off = fabsf(m->clock - beat);
            if (off <= RHY_WINDOW)                  /* score by closeness */
                rhythm_advance(m, clampf(1.0f - off / RHY_WINDOW, 0.0f, 1.0f),
                               off < 0.10f ? "ON BEAT!" : "GOOD");
            /* taps outside the window are ignored, not punished */
        }
        break;
    default:
        break;
    }
}

static void perform_rest(void)
{
    int old_fatigue = G.kilix.fatigue;
    int old_stress = G.kilix.stress;
    char detail[192];

    G.kilix.fatigue -= 36;
    G.kilix.stress -= 23;
    G.kilix.form += 8;
    G.kilix.bond += 2;
    normalize_game();
    game_advance_week();
    snprintf(detail, sizeof(detail),
             "%s curled beside the warm stove. Fatigue %d to %d; stress %d to %d.",
             G.kilix.name, old_fatigue, G.kilix.fatigue,
             old_stress, G.kilix.stress);
    begin_event(EVENT_REST, "A Long Catnap", detail);
    G.event.success = true;
    play_sound(SFX_CONFIRM);
}

static void use_care_item(int index)
{
    const ItemInfo *item;
    char detail[192];

    if (index < 0 || index >= ITEM_COUNT) {
        game_show_toast("That care item is not available.");
        return;
    }
    item = &ITEMS[index];
    if (G.money < item->cost) {
        game_show_toast("Not enough ranch funds for that care item.");
        play_sound(SFX_MOVE);
        return;
    }

    G.money -= item->cost;
    G.kilix.fatigue += item->fatigue;
    G.kilix.stress += item->stress;
    G.kilix.bond += item->bond;
    G.kilix.form += item->form;
    G.kilix.hunger -= item->satiety;    /* food fills the belly; a brush does not */
    normalize_game();
    game_advance_week();
    if (item->satiety > 0)
        snprintf(detail, sizeof(detail),
                 "%s devoured the %s. Hunger -%d, bond %+d, form %+d.",
                 G.kilix.name, item->name, item->satiety, item->bond, item->form);
    else
        snprintf(detail, sizeof(detail),
                 "%s enjoyed the %s. Bond %+d, form %+d, stress %+d.",
                 G.kilix.name, item->name, item->bond, item->form, item->stress);
    begin_event(EVENT_FEED, item->name, detail);
    G.event.index = index;
    G.event.success = true;
    G.event.money_delta = -item->cost;
    play_sound(SFX_CONFIRM);
}

/* ---- Dojo: learn battle moves for gold (instant, no week) --------------- */
static void learn_move(int index)
{
    char toast[160];

    if (index < 0 || index >= MOVE_COUNT) {
        game_show_toast("That move is not on the dojo's list.");
        return;
    }
    if (G.moves_known & (1u << index)) {       /* incl. MOVES[0], the free basic */
        game_show_toast("Kilix already knows that move.");
        play_sound(SFX_MOVE);
        return;
    }
    if (G.money < MOVES[index].price) {
        game_show_toast("Not enough gold to learn that move.");
        play_sound(SFX_MOVE);
        return;
    }
    G.money -= MOVES[index].price;
    G.moves_known |= (1u << index);
    normalize_game();
    if (!suppress_autosave)
        game_save();                           /* a purchase should persist */
    snprintf(toast, sizeof(toast), "%s learned %s!", G.kilix.name,
             MOVES[index].name);
    game_show_toast(toast);
    play_sound(SFX_CONFIRM);
}

/* ---- Academy: paid coaching — a guaranteed stat gain, costs a week ------- */
static void academy_begin(int stat)
{
    if (stat < 0 || stat >= STAT_COUNT)
        return;
    if (G.money < ACADEMY_COST) {
        game_show_toast("Not enough gold for a coaching session.");
        play_sound(SFX_MOVE);
        return;
    }
    G.money -= ACADEMY_COST;
    G.academy_choice = stat;
    G.academy_phase = 1;           /* run the montage; the gain lands on finish */
    G.academy_clock = 0.0f;
    play_sound(SFX_TRAIN);
}

static void academy_finish(void)
{
    int stat = clampi(G.academy_choice, 0, STAT_COUNT - 1);
    char detail[192];

    G.academy_phase = 0;
    G.kilix.stats.value[stat] += ACADEMY_GAIN;
    G.kilix.fatigue += 10;
    normalize_game();
    game_advance_week();
    snprintf(detail, sizeof(detail),
             "A focused coaching session pays off. %s %+d, fatigue +10.",
             STAT_NAMES[stat], ACADEMY_GAIN);
    begin_event(EVENT_TRAIN, "Coaching Session", detail);
    G.event.index = stat;          /* stat index == its drill emblem index */
    G.event.primary = (StatKind)stat;
    G.event.gain_primary = ACADEMY_GAIN;
    G.event.money_delta = -ACADEMY_COST;   /* paid at booking; shown here */
    G.event.success = true;
    play_sound(SFX_WIN);
}

/* ---- Bank: set gold aside so rent is always covered -------------------- */
static void bank_move(int amount)
{
    if (amount > 0) {                          /* deposit coffers -> vault */
        int m = amount > G.money ? G.money : amount;
        if (m > SAVE_MONEY_MAX - G.bank)        /* cap so nothing overflows away */
            m = SAVE_MONEY_MAX - G.bank;
        if (m <= 0) {
            game_show_toast("No gold in the coffers to deposit.");
            play_sound(SFX_MOVE);
            return;
        }
        G.money -= m;
        G.bank += m;
    } else if (amount < 0) {                    /* withdraw vault -> coffers */
        int w = -amount > G.bank ? G.bank : -amount;
        if (w > SAVE_MONEY_MAX - G.money)
            w = SAVE_MONEY_MAX - G.money;
        if (w <= 0) {
            game_show_toast("The rent vault is empty.");
            play_sound(SFX_MOVE);
            return;
        }
        G.bank -= w;
        G.money += w;
    } else {
        return;
    }
    normalize_game();
    if (!suppress_autosave)
        game_save();                            /* a transfer should persist */
    play_sound(SFX_CONFIRM);
}

static void bank_action(int which)
{
    switch (which) {
    case 0: bank_move(100); break;
    case 1: bank_move(500); break;
    /* "Everything" with an empty purse/vault passes 1 g instead of 0 so the
     * request still reaches bank_move's "nothing to move" toasts rather than
     * dying in its silent zero-amount branch. */
    case 2: bank_move(G.money > 0 ? G.money : 1); break;   /* deposit all */
    case 3: bank_move(-100); break;
    case 4: bank_move(-500); break;
    case 5: bank_move(G.bank > 0 ? -G.bank : -1); break;   /* withdraw all */
    default: break;
    }
}

void game_go_ranch(void)
{
    change_screen(SCREEN_RANCH);
    G.cursor = 0;
    G.first_visit = false;
}

static float player_will_rate(void)
{
    float rate = 5.4f;

    rate += (float)G.kilix.stats.value[STAT_SKILL] * 0.022f;
    rate += (float)G.kilix.bond * 0.012f;
    rate += (float)(G.kilix.form - 50) * 0.012f;
    rate -= (float)G.kilix.fatigue * 0.016f;
    rate -= (float)G.kilix.stress * 0.012f;
    return clampf(rate, 3.0f, 14.0f);
}

void game_start_battle(int opponent)
{
    BattleState *battle = &G.battle;
    const OpponentInfo *rival;
    int speed_difference;

    if (opponent < 0 || opponent >= OPPONENT_COUNT) {
        game_show_toast("No rival is registered in that arena slot.");
        return;
    }
    rival = &OPPONENTS[opponent];
    if (rival->rank > G.kilix.rank) {
        /* Cleared leagues stay open for exhibition rematches (half prize, no
         * promotion — see settle_battle), so a struggling ranch always has a
         * way to earn rent; only higher leagues are locked. */
        game_show_toast("Win your current league before challenging that rival.");
        play_sound(SFX_MOVE);
        return;
    }

    memset(battle, 0, sizeof(*battle));
    battle->phase = BATTLE_READY;
    battle->opponent = opponent;
    battle->timer = BATTLE_SECONDS;
    battle->intro_timer = 1.25f;
    battle->phase_timer = 0.0f;
    battle->player_max_hp = 115.0f
        + G.kilix.stats.value[STAT_LIFE] * 2.8f
        + G.kilix.stats.value[STAT_DEFENSE] * 0.45f;
    battle->enemy_max_hp = 115.0f
        + rival->stats.value[STAT_LIFE] * 2.8f
        + rival->stats.value[STAT_DEFENSE] * 0.45f;
    battle->player_hp = battle->player_max_hp;
    battle->enemy_hp = battle->enemy_max_hp;
    battle->player_guts = 48.0f;
    battle->enemy_guts = 42.0f;
    speed_difference = G.kilix.stats.value[STAT_SPEED]
        - rival->stats.value[STAT_SPEED];
    battle->distance = clampf(0.50f - speed_difference * 0.001f, 0.34f, 0.66f);
    battle->selected_move = 0;
    battle->active_move = -1;
    battle->winner = 0;
    snprintf(battle->callout, sizeof(battle->callout),
             "%s faces %s, %s!", G.kilix.name, rival->name, rival->epithet);
    change_screen(SCREEN_BATTLE);
    play_sound(SFX_CONFIRM);
}

static bool move_in_range(const MoveInfo *move, float distance)
{
    const float slop = 0.0001f;
    float range = distance * 100.0f;
    return range + slop >= move->min_range
        && range - slop <= move->max_range;
}

static int attack_accuracy(bool player, int move_index)
{
    const MoveInfo *move = &MOVES[move_index];
    const OpponentInfo *rival = &OPPONENTS[G.battle.opponent];
    int focus = player ? G.kilix.stats.value[STAT_SKILL]
                       : rival->stats.value[STAT_SKILL];
    int agility = player ? rival->stats.value[STAT_SPEED]
                         : G.kilix.stats.value[STAT_SPEED];
    float middle = (move->min_range + move->max_range) * 0.5f;
    float half_width = (move->max_range - move->min_range) * 0.5f;
    float edge_factor = 0.0f;
    float range = G.battle.distance * 100.0f;
    int accuracy;

    if (half_width > 0.001f)
        edge_factor = (range - middle) / half_width;
    if (edge_factor < 0.0f)
        edge_factor = -edge_factor;

    accuracy = move->accuracy + (focus - agility) / 7;
    accuracy += 5 - (int)(edge_factor * 8.0f);
    if (player) {
        accuracy += (G.kilix.form - 50) / 10;
        accuracy -= G.kilix.stress / 20;
    }
    return clampi(accuracy, 7, 97);
}

static int attack_damage(bool player, int move_index, int variance)
{
    const MoveInfo *move = &MOVES[move_index];
    const OpponentInfo *rival = &OPPONENTS[G.battle.opponent];
    int scaling = player ? G.kilix.stats.value[move->scaling]
                         : rival->stats.value[move->scaling];
    int guard = player ? rival->stats.value[STAT_DEFENSE]
                       : G.kilix.stats.value[STAT_DEFENSE];
    float raw = (float)move->power + scaling * 0.39f - guard * 0.17f;

    if (player) {
        raw *= 0.82f + G.kilix.form * 0.0036f;
        raw *= 1.0f - G.kilix.fatigue * 0.0023f;
    }
    raw *= (float)variance / 100.0f;
    return clampi((int)(raw + 0.5f), 2, 9999);
}

static void enter_attack(bool player, int move_index)
{
    BattleState *battle = &G.battle;
    const MoveInfo *move = &MOVES[move_index];
    const OpponentInfo *rival = &OPPONENTS[battle->opponent];
    int hit_roll = rng_range(1, 100);
    int variance = rng_range(88, 112);
    int accuracy = attack_accuracy(player, move_index);
    int damage = 0;

    battle->active_move = move_index;
    battle->hit = hit_roll <= accuracy;
    if (battle->hit)
        damage = attack_damage(player, move_index, variance);
    battle->last_damage = damage;
    battle->flash = battle->hit ? 0.20f : 0.08f;
    battle->shake = battle->hit ? clampf(damage / 160.0f, 0.08f, 0.36f) : 0.0f;

    if (player) {
        battle->player_guts = clampf(battle->player_guts - move->cost, 0.0f, 100.0f);
        battle->player_cooldown = 0.55f;
        battle->enemy_hp = clampf(battle->enemy_hp - damage, 0.0f,
                                  battle->enemy_max_hp);
        battle->phase = BATTLE_PLAYER_ATTACK;
        battle->phase_timer = 0.34f;
        if (battle->hit)
            snprintf(battle->callout, sizeof(battle->callout), "%s! %d damage!",
                     move->name, damage);
        else
            snprintf(battle->callout, sizeof(battle->callout), "%s missed!", move->name);
    } else {
        battle->enemy_guts = clampf(battle->enemy_guts - move->cost, 0.0f, 100.0f);
        battle->enemy_cooldown = clampf(0.95f
            - rival->stats.value[STAT_SPEED] * 0.0015f, 0.48f, 0.90f);
        battle->player_hp = clampf(battle->player_hp - damage, 0.0f,
                                   battle->player_max_hp);
        battle->phase = BATTLE_ENEMY_ATTACK;
        battle->phase_timer = 0.38f;
        if (battle->hit)
            snprintf(battle->callout, sizeof(battle->callout),
                     "%s uses %s! %d damage!", rival->name, move->name, damage);
        else
            snprintf(battle->callout, sizeof(battle->callout),
                     "%s's %s missed!", rival->name, move->name);
    }
    if (battle->hit)
        play_sound(SFX_HIT);
}

void game_use_move(int move_index)
{
    BattleState *battle = &G.battle;
    const MoveInfo *move;

    if (G.screen != SCREEN_BATTLE || battle->phase != BATTLE_ACTIVE)
        return;
    if (move_index < 0 || move_index >= MOVE_COUNT) {
        game_show_toast("That battle move is not known.");
        return;
    }
    battle->selected_move = move_index;
    move = &MOVES[move_index];
    if (!(G.moves_known & (1u << move_index))) {
        game_show_toast("Kilix has not learned that move. Visit the Dojo.");
        play_sound(SFX_MOVE);
        return;
    }
    if (battle->player_cooldown > 0.0f) {
        game_show_toast("Wait for an opening.");
        return;
    }
    if (battle->player_guts + 0.001f < (float)move->cost) {
        game_show_toast("Not enough Will yet.");
        return;
    }
    if (!move_in_range(move, battle->distance)) {
        game_show_toast("That move cannot reach from here.");
        return;
    }
    enter_attack(true, move_index);
}

static void finish_battle(int winner, const char *callout)
{
    BattleState *battle = &G.battle;

    if (battle->phase == BATTLE_FINISHED)
        return;
    battle->phase = BATTLE_FINISHED;
    battle->winner = clampi(winner, -1, 1);
    battle->phase_timer = 1.10f;
    if (callout)
        copy_text(battle->callout, sizeof(battle->callout), callout);
    if (winner > 0)
        play_sound(SFX_WIN);
    else if (winner < 0)
        play_sound(SFX_LOSE);
}

static void finish_on_time(void)
{
    BattleState *battle = &G.battle;
    float player_ratio = battle->player_hp / battle->player_max_hp;
    float enemy_ratio = battle->enemy_hp / battle->enemy_max_hp;

    if (player_ratio > enemy_ratio + 0.005f)
        finish_battle(1, "Time! Kilix wins on remaining Heart!");
    else if (enemy_ratio > player_ratio + 0.005f)
        finish_battle(-1, "Time! The rival wins on remaining Heart.");
    else
        finish_battle(0, "Time! The match ends in a draw.");
}

/* Auto-battle: pick a move the Kilix can afford and reach (highest first). */
static int player_preferred_move(void)
{
    BattleState *battle = &G.battle;
    int best = -1;
    for (int i = MOVE_COUNT - 1; i >= 0; --i) {
        if (!(G.moves_known & (1u << i)))          /* only moves Kilix has learned */
            continue;
        if (battle->player_guts + 0.001f < MOVES[i].cost)
            continue;
        if (move_in_range(&MOVES[i], battle->distance))
            return i;
        if (best < 0)
            best = i;
    }
    return best;
}

static void player_think(float dt)
{
    BattleState *battle = &G.battle;
    int move_index = player_preferred_move();
    if (battle->player_cooldown > 0.0f || move_index < 0)
        return;
    if (battle->player_guts + 0.001f >= MOVES[move_index].cost
        && move_in_range(&MOVES[move_index], battle->distance)) {
        battle->selected_move = move_index;
        enter_attack(true, move_index);
        return;
    }
    /* Close toward the chosen move's band. */
    float target = (MOVES[move_index].min_range + MOVES[move_index].max_range) * 0.005f;
    if (battle->distance < target - 0.015f)
        battle->distance += 0.34f * dt;
    else if (battle->distance > target + 0.015f)
        battle->distance -= 0.34f * dt;
    battle->distance = clampf(battle->distance, 0.0f, 1.0f);
    battle->selected_move = move_index;
}

static int enemy_preferred_move(void)
{
    BattleState *battle = &G.battle;
    int best = -1;
    int i;

    for (i = MOVE_COUNT - 1; i >= 0; --i) {
        if (battle->enemy_guts + 0.001f < MOVES[i].cost)
            continue;
        if (move_in_range(&MOVES[i], battle->distance))
            return i;
        if (best < 0)
            best = i;
    }
    return best;
}

static void update_battle_resources(float dt)
{
    BattleState *battle = &G.battle;
    const OpponentInfo *rival = &OPPONENTS[battle->opponent];

    battle->timer = clampf(battle->timer - dt, 0.0f, BATTLE_SECONDS);
    battle->player_guts = clampf(battle->player_guts + player_will_rate() * dt,
                                 0.0f, 100.0f);
    battle->enemy_guts = clampf(battle->enemy_guts + rival->guts_rate * dt,
                                0.0f, 100.0f);
    battle->player_cooldown = clampf(battle->player_cooldown - dt, 0.0f, 10.0f);
    battle->enemy_cooldown = clampf(battle->enemy_cooldown - dt, 0.0f, 10.0f);
}

static void enemy_think(float dt)
{
    BattleState *battle = &G.battle;
    const OpponentInfo *rival = &OPPONENTS[battle->opponent];
    int move_index = enemy_preferred_move();
    float target;
    float pace;

    if (battle->enemy_cooldown > 0.0f || move_index < 0)
        return;
    if (battle->enemy_guts + 0.001f >= MOVES[move_index].cost
        && move_in_range(&MOVES[move_index], battle->distance)) {
        enter_attack(false, move_index);
        return;
    }

    target = (MOVES[move_index].min_range + MOVES[move_index].max_range) * 0.005f;
    pace = clampf(0.16f + rival->stats.value[STAT_SPEED] * 0.0012f,
                  0.18f, 0.40f);
    if (battle->distance < target - 0.015f)
        battle->distance += pace * dt;
    else if (battle->distance > target + 0.015f)
        battle->distance -= pace * dt;
    battle->distance = clampf(battle->distance, 0.0f, 1.0f);
}

static void battle_tick(float dt)
{
    BattleState *battle = &G.battle;

    if (battle->opponent < 0 || battle->opponent >= OPPONENT_COUNT) {
        game_go_ranch();
        game_show_toast("The arena match could not be restored.");
        return;
    }

    battle->shake = clampf(battle->shake - dt, 0.0f, 1.0f);
    battle->flash = clampf(battle->flash - dt, 0.0f, 1.0f);

    if (battle->phase == BATTLE_READY) {
        battle->intro_timer = clampf(battle->intro_timer - dt, 0.0f, 10.0f);
        return;
    }

    if (battle->phase == BATTLE_ACTIVE
        || battle->phase == BATTLE_PLAYER_ATTACK
        || battle->phase == BATTLE_ENEMY_ATTACK) {
        update_battle_resources(dt);
    }

    if (battle->phase == BATTLE_PLAYER_ATTACK
        || battle->phase == BATTLE_ENEMY_ATTACK) {
        battle->phase_timer = clampf(battle->phase_timer - dt, 0.0f, 10.0f);
        if (battle->phase_timer <= 0.0f) {
            if (battle->enemy_hp <= 0.0f)
                finish_battle(1, "The rival is down! Kilix wins!");
            else if (battle->player_hp <= 0.0f)
                finish_battle(-1, "Kilix is down. The rival wins.");
            else if (battle->timer <= 0.0f)
                finish_on_time();
            else
                battle->phase = BATTLE_ACTIVE;
        }
        return;
    }

    if (battle->phase == BATTLE_ACTIVE) {
        if (battle->timer <= 0.0f) {
            finish_on_time();
            return;
        }
        if (battle->autopilot)
            player_think(dt);
        /* If the player just launched an attack this tick, its 0.34s
         * PLAYER_ATTACK window owns the turn — skip the enemy so it can't
         * clobber the player's strike (or land a posthumous free hit on a
         * creature the player just KO'd). In manual play autopilot is off,
         * so phase stays ACTIVE and the enemy thinks exactly as before. */
        if (battle->phase == BATTLE_ACTIVE)
            enemy_think(dt);
        return;
    }

    if (battle->phase == BATTLE_FINISHED) {
        battle->phase_timer = clampf(battle->phase_timer - dt, 0.0f, 10.0f);
    }
}

static void settle_battle(void)
{
    BattleState *battle = &G.battle;
    const OpponentInfo *rival;
    bool promoted = false;
    int prize = 0;
    int fought_rank;
    char title[64];
    char detail[192];

    if (G.screen != SCREEN_BATTLE || battle->phase != BATTLE_FINISHED)
        return;
    rival = &OPPONENTS[battle->opponent];
    fought_rank = rival->rank;
    /* A cleared league's rival fights again as an exhibition: half prize,
     * lighter glory, never a promotion. This keeps income possible in any
     * week — without it, pre-Crown prize money is finite while rent is not,
     * and a slow-growing ranch would face certain eviction. */
    bool exhibition = fought_rank < G.kilix.rank;

    if (battle->winner > 0) {
        ++G.kilix.total_wins;
        prize = exhibition ? rival->prize / 2 : rival->prize;
        G.money = clampi(G.money + prize, 0, SAVE_MONEY_MAX);
        G.kilix.fatigue += 18;
        G.kilix.stress -= 5;
        G.kilix.bond += exhibition ? 2 : 6;
        G.kilix.form += exhibition ? 3 : 8;
        if (!exhibition)
            ++G.kilix.rank_wins;
        if (fought_rank == G.kilix.rank && G.kilix.rank < RANK_COUNT - 1) {
            ++G.kilix.rank;
            G.kilix.rank_wins = 0;
            promoted = true;
        }
        if (exhibition) {
            snprintf(title, sizeof(title), "%s Exhibition", RANK_NAMES[fought_rank]);
            snprintf(detail, sizeof(detail),
                     "%s bested %s in a friendly rematch and earned a %d coin purse.",
                     G.kilix.name, rival->name, prize);
        } else {
            snprintf(title, sizeof(title), "%s Victory", RANK_NAMES[fought_rank]);
            snprintf(detail, sizeof(detail),
                     "%s defeated %s and earned %d coins.%s",
                     G.kilix.name, rival->name, prize,
                     promoted ? " A league promotion awaits!" : " The Crown crowd roars!");
        }
    } else if (battle->winner < 0) {
        ++G.kilix.total_losses;
        G.kilix.fatigue += 20;
        G.kilix.stress += 12;
        G.kilix.form -= 7;
        snprintf(title, sizeof(title), "%s Defeat", RANK_NAMES[fought_rank]);
        snprintf(detail, sizeof(detail),
                 "%s lost to %s, but every match leaves a lesson for next time.",
                 G.kilix.name, rival->name);
    } else {
        G.kilix.fatigue += 18;
        G.kilix.stress += 4;
        snprintf(title, sizeof(title), "%s Draw", RANK_NAMES[fought_rank]);
        snprintf(detail, sizeof(detail),
                 "%s and %s were still even when the arena bell rang.",
                 G.kilix.name, rival->name);
    }

    normalize_game();
    game_advance_week();
    begin_event(EVENT_BATTLE_RESULT, title, detail);
    G.event.index = fought_rank;
    G.event.success = battle->winner > 0;
    G.event.great = promoted;
    G.event.money_delta = prize;
}

static void forfeit_battle(void)
{
    if (G.screen != SCREEN_BATTLE)
        return;
    ++G.kilix.total_losses;
    G.kilix.fatigue += 10;
    G.kilix.stress += 10;
    G.kilix.form -= 5;
    normalize_game();
    game_advance_week();
    /* A forfeited match still spends the week, so it must face the rent check
     * like every other week-advancing action, or repeated forfeits would let a
     * broke player dodge rent and eviction forever. */
    if (present_rent_event())
        return;
    game_go_ranch();
    game_show_toast("The match was forfeited. Kilix returned safely to the ranch.");
    play_sound(SFX_LOSE);
}

/* Back out of the pre-fight Ready prompt before any blow is struck: no
 * recorded loss, no penalty, no consumed week. Only a match already underway
 * (BATTLE_ACTIVE and later) counts Esc as a forfeit. */
static void cancel_battle(void)
{
    if (G.screen != SCREEN_BATTLE)
        return;
    memset(&G.battle, 0, sizeof(G.battle));
    G.battle.opponent = -1;
    G.battle.active_move = -1;
    G.arena_cursor = clampi(G.kilix.rank, 0, OPPONENT_COUNT - 1);
    G.cursor = G.arena_cursor;
    change_screen(SCREEN_ARENA);
    game_show_toast("Kilix stepped back from the arena. No match was recorded.");
    play_sound(SFX_MOVE);
}

typedef enum { RENT_NOT_DUE, RENT_PAID, RENT_EVICTED } RentResult;

/* Pure rent bookkeeping: once the paid-through period has elapsed, charge one
 * month up front. No screen/UI side effects, so the selftest drives it directly.
 * On failure the paid-through week is left untouched (rent is still owed). */
static RentResult try_collect_rent(void)
{
    if (G.total_weeks < G.rent_paid_weeks)
        return RENT_NOT_DUE;
    if (G.bank + G.money >= RENT_AMOUNT) {
        /* Spend the rent savings first, then the coffers for any shortfall. */
        int from_bank = G.bank >= RENT_AMOUNT ? RENT_AMOUNT : G.bank;
        G.bank -= from_bank;
        G.money -= (RENT_AMOUNT - from_bank);
        G.rent_paid_weeks += MONTH_WEEKS;
        return RENT_PAID;
    }
    return RENT_EVICTED;
}

/* Resolve any rent due and raise the matching event. Returns true when an event
 * is now on screen, so the caller stops instead of falling through to the ranch. */
static bool present_rent_event(void)
{
    char detail[192];
    RentResult result = try_collect_rent();

    if (result == RENT_NOT_DUE)
        return false;
    if (result == RENT_PAID) {
        snprintf(detail, sizeof(detail),
                 "The ranch collector calls. Rent of %d g is paid up front for "
                 "the coming month. Vault holds %d g, coffers %d g.",
                 RENT_AMOUNT, G.bank, G.money);
        begin_event(EVENT_RENT, "Rent Collected", detail);
        G.event.money_delta = -RENT_AMOUNT;
        G.event.success = true;
        play_sound(SFX_CONFIRM);
    } else {
        snprintf(detail, sizeof(detail),
                 "Rent of %d g came due and the coffers ran dry. %s and you are "
                 "put out of the ranch - the run ends here.",
                 RENT_AMOUNT, G.kilix.name);
        begin_event(EVENT_EVICTION, "Eviction Notice", detail);
        play_sound(SFX_LOSE);
    }
    return true;
}

static void dismiss_event(void)
{
    EventState old_event;
    char detail[192];

    if (G.screen_time < 0.12f)
        return;
    old_event = G.event;
    if (old_event.kind == EVENT_EVICTION) {
        /* The run is over: wipe the ranch record and return to the title. */
        game_delete_save();
        G.save_exists = false;
        memset(&G.event, 0, sizeof(G.event));
        change_screen(SCREEN_TITLE);
        return;
    }
    if (old_event.kind == EVENT_BATTLE_RESULT && old_event.success
        && old_event.index == RANK_COUNT - 1) {
        change_screen(SCREEN_CHAMPION);
        return;
    }
    if (old_event.kind == EVENT_BATTLE_RESULT && old_event.great) {
        snprintf(detail, sizeof(detail),
                 "%s has advanced to the %s League! New rivals are waiting.",
                 G.kilix.name, RANK_NAMES[G.kilix.rank]);
        begin_event(EVENT_RANK_UP, "League Promotion", detail);
        G.event.index = G.kilix.rank;
        G.event.success = true;
        play_sound(SFX_WIN);
        return;
    }
    /* A concluded week may have crossed into a new month: collect the rent (or
     * evict) before handing control back to the ranch. */
    if (present_rent_event())
        return;
    game_go_ranch();
}

static void open_journal(void)
{
    G.journal_page = clampi(G.journal_page, 0, JOURNAL_PAGES - 1);
    change_screen(SCREEN_JOURNAL);
}

static void handle_title_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_UP || lower == 'w') {
        G.title_cursor = wrap_index(G.title_cursor - 1, 4);
        G.cursor = G.title_cursor;
        play_sound(SFX_MOVE);
        return;
    }
    if (key == KEY_DOWN || lower == 's') {
        G.title_cursor = wrap_index(G.title_cursor + 1, 4);
        G.cursor = G.title_cursor;
        play_sound(SFX_MOVE);
        return;
    }
    if (lower == 'c') {
        change_screen(SCREEN_CREDITS);
        return;
    }
    if (!is_confirm_key(key))
        return;

    play_sound(SFX_CONFIRM);
    switch (G.title_cursor) {
    case 0:
        if (!G.save_exists) {
            game_show_toast("No ranch record is available yet.");
        } else if (game_load()) {
            game_show_toast("Ranch record loaded.");
        } else {
            game_show_toast("That ranch record is damaged or unreadable.");
        }
        break;
    case 1:
        G.name_input[0] = '\0';
        G.name_len = 0;
        G.pending_overwrite = false;
        change_screen(SCREEN_NAMING);
        break;
    case 2:
        open_journal();
        break;
    case 3:
        G.quit = true;
        break;
    default:
        break;
    }
}

static void handle_naming_key(int key)
{
    if (key == KEY_ESC) {
        G.pending_overwrite = false;
        change_screen(SCREEN_TITLE);
        return;
    }
    if (key == KEY_BACKSPACE || key == '\b' || key == 127) {
        if (G.name_len > 0) {
            --G.name_len;
            G.name_input[G.name_len] = '\0';
        }
        G.pending_overwrite = false;
        return;
    }
    if (is_confirm_key(key) && key != ' ') {
        /* Replacing an existing ranch is irreversible, so require a second
         * confirming Enter first; Esc (or editing the name) backs out. */
        if (G.save_exists && !G.pending_overwrite) {
            G.pending_overwrite = true;
            game_show_toast("This replaces your saved ranch. Press Enter again "
                            "to confirm, Esc to cancel.");
            play_sound(SFX_MOVE);
            return;
        }
        G.pending_overwrite = false;
        game_new_ranch(G.name_len > 0 ? G.name_input : "Ember");
        play_sound(SFX_CONFIRM);
        return;
    }
    if (key >= 32 && key <= 126 && G.name_len < (int)sizeof(G.name_input) - 1) {
        G.name_input[G.name_len++] = (char)key;
        G.name_input[G.name_len] = '\0';
        G.pending_overwrite = false;   /* name changed: re-confirm before replacing */
    }
}

/* Ranch menu rows, in display order. Kept in one place so the handler, the
 * activator and the renderer agree. */
enum {
    RANCH_DRILLS, RANCH_ACADEMY, RANCH_DOJO, RANCH_BANK, RANCH_CATNAP,
    RANCH_CARE, RANCH_ARENA, RANCH_JOURNAL, RANCH_SAVE, RANCH_ROWS
};

static void activate_ranch_row(int row)
{
    switch (row) {
    case RANCH_DRILLS:
        G.drill_cursor = 0;
        change_screen(SCREEN_TRAINING);
        break;
    case RANCH_ACADEMY:
        G.academy_phase = 0;
        change_screen(SCREEN_ACADEMY);
        break;
    case RANCH_DOJO:
        G.dojo_cursor = 1;          /* open on the first purchasable move */
        change_screen(SCREEN_DOJO);
        break;
    case RANCH_BANK:
        change_screen(SCREEN_BANK);
        break;
    case RANCH_CATNAP:
        perform_rest();
        break;
    case RANCH_CARE:
        G.care_cursor = 0;
        change_screen(SCREEN_CARE);
        break;
    case RANCH_ARENA:
        G.arena_cursor = G.kilix.rank;
        change_screen(SCREEN_ARENA);
        break;
    case RANCH_JOURNAL:
        open_journal();
        break;
    case RANCH_SAVE:
        if (game_save())
            game_show_toast("Ranch record saved.");
        else
            game_show_toast("The ranch record could not be saved.");
        break;
    default:
        break;
    }
}

static void handle_ranch_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        change_screen(SCREEN_TITLE);
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.cursor = wrap_index(G.cursor - 1, RANCH_ROWS);
        G.first_visit = false;
        play_sound(SFX_MOVE);
        return;
    }
    if (key == KEY_DOWN || lower == 's') {
        G.cursor = wrap_index(G.cursor + 1, RANCH_ROWS);
        G.first_visit = false;
        play_sound(SFX_MOVE);
        return;
    }
    if (lower == 'd')
        G.cursor = RANCH_DRILLS;
    else if (lower == 't')
        G.cursor = RANCH_ACADEMY;
    else if (lower == 'o')          /* 'm' is the global mute toggle */
        G.cursor = RANCH_DOJO;
    else if (lower == 'b')
        G.cursor = RANCH_BANK;
    else if (lower == 'r')
        G.cursor = RANCH_CATNAP;
    else if (lower == 'c')
        G.cursor = RANCH_CARE;
    else if (lower == 'a')
        G.cursor = RANCH_ARENA;
    else if (lower == 'j')
        G.cursor = RANCH_JOURNAL;
    else if (lower == 'v')
        G.cursor = RANCH_SAVE;
    else if (!is_confirm_key(key))
        return;

    G.first_visit = false;
    play_sound(SFX_CONFIRM);
    activate_ranch_row(G.cursor);
}

static void handle_training_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.drill_cursor = wrap_index(G.drill_cursor - 1, DRILL_COUNT);
        G.cursor = G.drill_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || lower == 's') {
        G.drill_cursor = wrap_index(G.drill_cursor + 1, DRILL_COUNT);
        G.cursor = G.drill_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        perform_drill(G.drill_cursor);
    }
}

static void handle_care_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.care_cursor = wrap_index(G.care_cursor - 1, ITEM_COUNT);
        G.cursor = G.care_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || lower == 's') {
        G.care_cursor = wrap_index(G.care_cursor + 1, ITEM_COUNT);
        G.cursor = G.care_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        use_care_item(G.care_cursor);
    }
}

static void handle_academy_key(int key)
{
    int lower = lower_key(key);

    if (G.academy_phase != 0)          /* the montage plays out on its own */
        return;
    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.academy_cursor = wrap_index(G.academy_cursor - 1, STAT_COUNT);
        G.cursor = G.academy_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || lower == 's') {
        G.academy_cursor = wrap_index(G.academy_cursor + 1, STAT_COUNT);
        G.cursor = G.academy_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        academy_begin(G.academy_cursor);
    }
}

static void handle_dojo_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.dojo_cursor = wrap_index(G.dojo_cursor - 1, MOVE_COUNT);
        G.cursor = G.dojo_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || lower == 's') {
        G.dojo_cursor = wrap_index(G.dojo_cursor + 1, MOVE_COUNT);
        G.cursor = G.dojo_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        learn_move(G.dojo_cursor);
    }
}

static void handle_bank_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        G.bank_cursor = wrap_index(G.bank_cursor - 1, BANK_ACTIONS);
        G.cursor = G.bank_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || lower == 's') {
        G.bank_cursor = wrap_index(G.bank_cursor + 1, BANK_ACTIONS);
        G.cursor = G.bank_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        bank_action(G.bank_cursor);
    }
}

static void handle_arena_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        game_go_ranch();
        return;
    }
    if (key == KEY_UP || key == KEY_LEFT || lower == 'w' || lower == 'a') {
        G.arena_cursor = wrap_index(G.arena_cursor - 1, OPPONENT_COUNT);
        G.cursor = G.arena_cursor;
        play_sound(SFX_MOVE);
    } else if (key == KEY_DOWN || key == KEY_RIGHT || lower == 's' || lower == 'd') {
        G.arena_cursor = wrap_index(G.arena_cursor + 1, OPPONENT_COUNT);
        G.cursor = G.arena_cursor;
        play_sound(SFX_MOVE);
    } else if (is_confirm_key(key)) {
        game_start_battle(G.arena_cursor);
    }
}

static void handle_battle_key(int key)
{
    BattleState *battle = &G.battle;
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        if (battle->phase == BATTLE_FINISHED)
            settle_battle();
        else if (battle->phase == BATTLE_READY)
            cancel_battle();          /* pre-fight: back out with no penalty */
        else
            forfeit_battle();         /* mid-fight: Esc gives up the match */
        return;
    }
    if (lower == 'v') {                    /* toggle auto-battle any time */
        battle->autopilot = !battle->autopilot;
        game_show_toast(battle->autopilot ? "Auto-battle ON - Kilix fights on its own."
                                          : "Auto-battle OFF - you have the reins.");
        play_sound(SFX_MOVE);
        return;
    }
    if (is_confirm_key(key) && battle->phase == BATTLE_READY) {
        battle->phase = BATTLE_ACTIVE;
        battle->intro_timer = 0.0f;
        copy_text(battle->callout, sizeof(battle->callout), "Begin!");
        play_sound(SFX_CONFIRM);
        return;
    }
    if (is_confirm_key(key) && battle->phase == BATTLE_FINISHED) {
        settle_battle();
        return;
    }
    if (key >= '1' && key <= '4') {
        int move = key - '1';
        battle->selected_move = move;
        game_use_move(move);
        return;
    }
    if (key == KEY_UP || lower == 'w') {
        battle->selected_move = wrap_index(battle->selected_move - 1, MOVE_COUNT);
        play_sound(SFX_MOVE);
        return;
    }
    if (key == KEY_DOWN || lower == 's') {
        battle->selected_move = wrap_index(battle->selected_move + 1, MOVE_COUNT);
        play_sound(SFX_MOVE);
        return;
    }
    /* Repositioning is only meaningful once the match is live; allowing it on
     * the Ready prompt let the player zero out the speed-based starting
     * distance the fight was set up with. */
    if ((key == KEY_LEFT || lower == 'a') && battle->phase == BATTLE_ACTIVE) {
        battle->distance = clampf(battle->distance + 0.075f, 0.0f, 1.0f);
        return;
    }
    if ((key == KEY_RIGHT || lower == 'd') && battle->phase == BATTLE_ACTIVE) {
        battle->distance = clampf(battle->distance - 0.075f, 0.0f, 1.0f);
        return;
    }
    if (is_confirm_key(key))
        game_use_move(battle->selected_move);
}

static void handle_journal_key(int key)
{
    int lower = lower_key(key);

    if (key == KEY_ESC) {
        if (G.previous_screen == SCREEN_TITLE)
            change_screen(SCREEN_TITLE);
        else if (G.previous_screen == SCREEN_CHAMPION)
            change_screen(SCREEN_CHAMPION);
        else
            game_go_ranch();
        return;
    }
    if (key == KEY_LEFT || key == KEY_UP || lower == 'a' || lower == 'w') {
        G.journal_page = wrap_index(G.journal_page - 1, JOURNAL_PAGES);
        play_sound(SFX_MOVE);
    } else if (key == KEY_RIGHT || key == KEY_DOWN || lower == 'd' || lower == 's'
               || is_confirm_key(key)) {
        G.journal_page = wrap_index(G.journal_page + 1, JOURNAL_PAGES);
        play_sound(SFX_MOVE);
    }
}

void game_handle_key(int key)
{
    int lower = lower_key(key);

    if (lower == 'm' && G.screen != SCREEN_NAMING) {
        G.sound_on = !G.sound_on;
        game_show_toast(G.sound_on ? "Sound is on." : "Sound is off.");
        if (G.sound_on)
            play_sound(SFX_CONFIRM);
        /* Persist the preference immediately when a ranch exists, so it does
         * not silently revert if the player quits before the next autosave.
         * Skip ONLY an in-progress fight (the active/attack phases): those are
         * guaranteed to settle into a week-advance autosave that persists the
         * toggle anyway, and saving mid-combat would flash a misleading
         * "saved" indicator. The Ready prompt (which Esc can cancel with no
         * save), the result event, and every menu therefore save normally. */
        bool live_fight = G.screen == SCREEN_BATTLE
                          && G.battle.phase != BATTLE_READY
                          && G.battle.phase != BATTLE_FINISHED;
        if (!suppress_autosave && G.save_exists && G.kilix.name[0] && !live_fight)
            game_save();
        return;
    }
    if (lower == 'q' && (G.screen == SCREEN_TITLE || G.screen == SCREEN_RANCH
                         || G.screen == SCREEN_CHAMPION)) {
        G.quit = true;
        return;
    }

    switch ((Screen)G.screen) {
    case SCREEN_TITLE:
        handle_title_key(key);
        break;
    case SCREEN_NAMING:
        handle_naming_key(key);
        break;
    case SCREEN_RANCH:
        handle_ranch_key(key);
        break;
    case SCREEN_TRAINING:
        handle_training_key(key);
        break;
    case SCREEN_CARE:
        handle_care_key(key);
        break;
    case SCREEN_ARENA:
        handle_arena_key(key);
        break;
    case SCREEN_BATTLE:
        handle_battle_key(key);
        break;
    case SCREEN_DRILL:
        minigame_key(key);
        break;
    case SCREEN_ACADEMY:
        handle_academy_key(key);
        break;
    case SCREEN_DOJO:
        handle_dojo_key(key);
        break;
    case SCREEN_BANK:
        handle_bank_key(key);
        break;
    case SCREEN_EVENT:
        if (is_confirm_key(key) || key == KEY_ESC)
            dismiss_event();
        break;
    case SCREEN_JOURNAL:
        handle_journal_key(key);
        break;
    case SCREEN_CREDITS:
        if (is_confirm_key(key) || key == KEY_ESC)
            change_screen(SCREEN_TITLE);
        break;
    case SCREEN_CHAMPION:
        if (lower_key(key) == 'j') {
            open_journal();
        } else if (is_confirm_key(key) || key == KEY_ESC) {
            /* A Crown victory spends a week like any other match, but its
             * result event routes here instead of through the usual
             * dismissal path — so the deferred rent check must run now, or
             * champions could dodge rent (and eviction) with every title
             * defense. */
            if (!present_rent_event())
                game_go_ranch();
        }
        break;
    default:
        change_screen(SCREEN_TITLE);
        break;
    }
}

static void tick_step(float dt)
{
    G.time += dt;
    G.screen_time += dt;
    G.ambient_phase += dt * 0.22f;
    if (G.ambient_phase > 4096.0f)
        G.ambient_phase -= 4096.0f;
    G.toast_timer = clampf(G.toast_timer - dt, 0.0f, 30.0f);
    G.autosave_flash = clampf(G.autosave_flash - dt, 0.0f, 30.0f);
    if (G.screen == SCREEN_EVENT)
        G.event.timer = clampf(G.event.timer + dt, 0.0f, G.event.duration);
    else if (G.screen == SCREEN_BATTLE)
        battle_tick(dt);
    else if (G.screen == SCREEN_DRILL)
        minigame_tick(dt);
    else if (G.screen == SCREEN_ACADEMY && G.academy_phase == 1) {
        G.academy_clock += dt;
        if (G.academy_clock >= ACADEMY_ANIM)
            academy_finish();          /* montage done: apply the gain */
    }
}

void game_tick(float dt)
{
    float remaining;

    if (!isfinite(dt) || dt <= 0.0f)
        return;
    remaining = clampf(dt, 0.0f, 0.25f);
    while (remaining > 0.0f) {
        float step = remaining > TICK_DT ? TICK_DT : remaining;
        tick_step(step);
        remaining -= step;
        if (remaining < 0.000001f)
            remaining = 0.0f;
    }
}

static uint32_t hash_bytes(const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void fill_save_data(SaveData *save)
{
    int i;

    memset(save, 0, sizeof(*save));
    save->version = SAVE_VERSION;
    save->rng = G.rng ? G.rng : 0x6d2b79f5u;
    save->total_weeks = G.total_weeks;
    save->money = G.money;
    memcpy(save->name, G.kilix.name, sizeof(save->name));
    save->name[sizeof(save->name) - 1] = '\0';
    for (i = 0; i < STAT_COUNT; ++i)
        save->stats[i] = G.kilix.stats.value[i];
    save->fatigue = G.kilix.fatigue;
    save->stress = G.kilix.stress;
    save->bond = G.kilix.bond;
    save->form = G.kilix.form;
    save->age_weeks = G.kilix.age_weeks;
    save->rank = G.kilix.rank;
    save->rank_wins = G.kilix.rank_wins;
    save->total_wins = G.kilix.total_wins;
    save->total_losses = G.kilix.total_losses;
    save->personality_seed = G.kilix.personality_seed;
    save->sound_on = G.sound_on ? 1u : 0u;
    /* v3 economy state rides in the previously reserved slots. */
    save->reserved[0] = (uint32_t)G.kilix.hunger;
    save->reserved[1] = G.moves_known;
    save->reserved[2] = (uint32_t)G.rent_paid_weeks;
    save->reserved[3] = (uint32_t)G.bank;
    save->checksum = 0;
    save->checksum = hash_bytes(save, sizeof(*save));
}

static bool valid_save_data(const SaveData *save)
{
    SaveData copy;
    uint32_t expected;
    int i;

    if (!save || (save->version != SAVE_VERSION && save->version != 2u))
        return false;
    copy = *save;
    expected = copy.checksum;
    copy.checksum = 0;
    if (expected != hash_bytes(&copy, sizeof(copy)))
        return false;
    if (save->rng == 0 || save->total_weeks < 0
        || save->total_weeks > SAVE_WEEKS_MAX
        || save->money < 0 || save->money > SAVE_MONEY_MAX)
        return false;
    if (save->version == 2u) {
        /* Pre-economy (initial release) saves share this exact layout but
         * wrote every reserved slot as zero; anything else is corruption,
         * not a v2 ranch. game_load fills in the economy defaults. */
        for (i = 0; i < 4; ++i)
            if (save->reserved[i] != 0u)
                return false;
    } else if (save->reserved[0] > 100
        /* v3 economy fields: hunger in range, at least the basic move known
         * and no unknown move bits, rent covering a sane, non-negative span. */
        || (save->reserved[1] & 1u) == 0
        || save->reserved[1] >= (1u << MOVE_COUNT)
        || (int32_t)save->reserved[2] < 0
        || (int32_t)save->reserved[2] > SAVE_WEEKS_MAX + MONTH_WEEKS
        || (int32_t)save->reserved[3] < 0
        || (int32_t)save->reserved[3] > SAVE_MONEY_MAX)
        return false;
    /* Canonical name: a non-empty run of printable ASCII, then all-zero to the
     * end of the field. The keyboard path (clean_name) already produces this;
     * rejecting anything else stops a hand-edited save from smuggling control
     * bytes into the renderer/event text or post-terminator junk that would
     * round-trip back out and make two identical ranches hash differently. */
    {
        size_t n = 0;
        while (n < sizeof(save->name) && save->name[n] != '\0') {
            unsigned char c = (unsigned char)save->name[n];
            if (c < 0x20 || c > 0x7e)
                return false;
            ++n;
        }
        if (n == 0 || n >= sizeof(save->name))
            return false;
        for (size_t j = n; j < sizeof(save->name); ++j)
            if (save->name[j] != '\0')
                return false;
    }
    for (i = 0; i < STAT_COUNT; ++i) {
        if (save->stats[i] < 1 || save->stats[i] > 999)
            return false;
    }
    if (save->fatigue < 0 || save->fatigue > 100
        || save->stress < 0 || save->stress > 100
        || save->bond < 0 || save->bond > 100
        || save->form < 0 || save->form > 100)
        return false;
    if (save->age_weeks != save->total_weeks
        || save->rank < 0 || save->rank >= RANK_COUNT
        || save->rank_wins < 0 || save->rank_wins > SAVE_WEEKS_MAX
        || save->total_wins < 0 || save->total_wins > SAVE_WEEKS_MAX
        || save->total_losses < 0 || save->total_losses > SAVE_WEEKS_MAX
        || save->personality_seed == 0 || save->sound_on > 1u)
        return false;
    return true;
}

bool game_save(void)
{
    SaveData save;
    unsigned char payload[sizeof(SAVE_MAGIC) + sizeof save];
    kilixstate_store store;
    bool ok;

    if (!G.save_path[0] || !G.kilix.name[0])
        return false;
    normalize_game();
    fill_save_data(&save);
    memcpy(payload, SAVE_MAGIC, sizeof SAVE_MAGIC);
    memcpy(payload + sizeof SAVE_MAGIC, &save, sizeof save);
    if (!save_store_init(&store)) {
        G.save_failed = true;
        return false;
    }
    ok = kilixstate_save(&store, payload, sizeof payload) == KILIXSTATE_OK;
    kilixstate_store_close(&store);
    /* Track save health in a field the screen-change toast-clear cannot wipe,
     * so a persistent on-screen warning can surface a failing disk. */
    G.save_failed = !ok;
    if (ok) {
        G.save_exists = true;
        G.autosave_flash = 1.0f;
    }
    return ok;
}

bool game_load(void)
{
    SaveData save;
    unsigned char payload[sizeof(SAVE_MAGIC) + sizeof save];
    kilixstate_store store;
    size_t payload_size = 0u;
    int i;
    GameState loaded;

    if (!G.save_path[0])
        return false;
    if (!save_store_init(&store))
        return false;
    if (kilixstate_load(&store, payload, sizeof payload, &payload_size) !=
        KILIXSTATE_OK || payload_size != sizeof payload) {
        kilixstate_store_close(&store);
        return false;
    }
    kilixstate_store_close(&store);
    memcpy(&save, payload + sizeof SAVE_MAGIC, sizeof save);
    if (memcmp(payload, SAVE_MAGIC, sizeof SAVE_MAGIC) != 0 ||
        !valid_save_data(&save))
        return false;

    memset(&loaded, 0, sizeof(loaded));
    loaded.W = G.W;
    loaded.H = G.H;
    loaded.headless = G.headless;
    loaded.screen = SCREEN_RANCH;
    loaded.previous_screen = SCREEN_TITLE;
    loaded.rng = save.rng;
    loaded.total_weeks = save.total_weeks;
    loaded.money = save.money;
    memcpy(loaded.kilix.name, save.name, sizeof(loaded.kilix.name));
    loaded.kilix.name[sizeof(loaded.kilix.name) - 1] = '\0';
    for (i = 0; i < STAT_COUNT; ++i)
        loaded.kilix.stats.value[i] = save.stats[i];
    loaded.kilix.fatigue = save.fatigue;
    loaded.kilix.stress = save.stress;
    loaded.kilix.bond = save.bond;
    loaded.kilix.form = save.form;
    loaded.kilix.age_weeks = save.age_weeks;
    loaded.kilix.rank = save.rank;
    loaded.kilix.rank_wins = save.rank_wins;
    loaded.kilix.total_wins = save.total_wins;
    loaded.kilix.total_losses = save.total_losses;
    loaded.kilix.personality_seed = save.personality_seed;
    loaded.sound_on = save.sound_on != 0;
    loaded.kilix.hunger = (int)save.reserved[0];
    loaded.moves_known = save.reserved[1] | 1u;   /* basic move always known */
    loaded.rent_paid_weeks = (int)save.reserved[2];
    loaded.bank = (int)save.reserved[3];
    if (save.version == 2u) {
        /* Migrate a pre-economy ranch: new-ranch hunger, and the newly
         * introduced rent first falls due at the next month boundary (the
         * same on-the-house grace a fresh ranch gets). moves_known and bank
         * already read as their defaults from the zeroed reserved slots.
         * The next autosave rewrites the record as version 3. */
        loaded.kilix.hunger = 20;
        loaded.rent_paid_weeks =
            (save.total_weeks / MONTH_WEEKS + 1) * MONTH_WEEKS;
    }
    loaded.save_exists = true;
    loaded.first_visit = false;
    loaded.battle.opponent = -1;
    loaded.battle.active_move = -1;
    copy_text(loaded.save_path, sizeof(loaded.save_path), G.save_path);
    G = loaded;
    normalize_game();
    return true;
}

bool game_delete_save(void)
{
    kilixstate_store store;
    kilixstate_result result;

    if (!G.save_path[0])
        return false;
    if (!save_store_init(&store)) return false;
    result = kilixstate_remove(&store);
    kilixstate_store_close(&store);
    if (result != KILIXSTATE_OK && result != KILIXSTATE_NOT_FOUND)
        return false;
    G.save_exists = false;
    return true;
}

static bool state_invariants(void)
{
    int i;

    if (G.screen < SCREEN_TITLE || G.screen > SCREEN_BANK
        || G.rng == 0 || G.total_weeks < 0 || G.total_weeks > SAVE_WEEKS_MAX
        || G.money < 0 || G.money > SAVE_MONEY_MAX
        || G.kilix.age_weeks != G.total_weeks
        || G.kilix.rank < 0 || G.kilix.rank >= RANK_COUNT
        || !memchr(G.kilix.name, '\0', sizeof(G.kilix.name)))
        return false;
    for (i = 0; i < STAT_COUNT; ++i) {
        if (G.kilix.stats.value[i] < 1 || G.kilix.stats.value[i] > 999)
            return false;
    }
    if (G.kilix.fatigue < 0 || G.kilix.fatigue > 100
        || G.kilix.stress < 0 || G.kilix.stress > 100
        || G.kilix.bond < 0 || G.kilix.bond > 100
        || G.kilix.form < 0 || G.kilix.form > 100
        || G.kilix.total_wins < 0 || G.kilix.total_losses < 0)
        return false;
    if (!isfinite(G.time) || !isfinite(G.screen_time)
        || !isfinite(G.toast_timer) || !isfinite(G.ambient_phase))
        return false;
    if (G.screen == SCREEN_BATTLE) {
        if (G.battle.opponent < 0 || G.battle.opponent >= OPPONENT_COUNT
            || G.battle.phase < BATTLE_READY || G.battle.phase > BATTLE_FINISHED
            || G.battle.selected_move < 0 || G.battle.selected_move >= MOVE_COUNT
            || G.battle.active_move < -1 || G.battle.active_move >= MOVE_COUNT
            || !isfinite(G.battle.player_hp) || !isfinite(G.battle.enemy_hp)
            || !isfinite(G.battle.player_guts) || !isfinite(G.battle.enemy_guts)
            || G.battle.player_hp < 0.0f || G.battle.player_hp > G.battle.player_max_hp
            || G.battle.enemy_hp < 0.0f || G.battle.enemy_hp > G.battle.enemy_max_hp
            || G.battle.player_guts < 0.0f || G.battle.player_guts > 100.0f
            || G.battle.enemy_guts < 0.0f || G.battle.enemy_guts > 100.0f
            || G.battle.distance < 0.0f || G.battle.distance > 1.0f
            || G.battle.timer < 0.0f || G.battle.phase_timer < 0.0f)
            return false;
    }
    return true;
}

static uint32_t persistent_digest(void)
{
    SaveData save;
    fill_save_data(&save);
    return hash_bytes(&save, sizeof(save));
}

static uint32_t scripted_growth(unsigned seed, int weeks, int *failures)
{
    int i;

    game_init(seed, true);
    G.headless = true;
    game_new_ranch("Test Kilix");
    for (i = 0; i < weeks; ++i) {
        int before = G.total_weeks;
        if (G.kilix.fatigue >= 72 || G.kilix.stress >= 72)
            perform_rest();
        else if (i % 9 == 8 && G.money >= ITEMS[i % ITEM_COUNT].cost)
            use_care_item(i % ITEM_COUNT);
        else
            perform_drill(i % DRILL_COUNT);
        if (G.total_weeks != before + 1 || !state_invariants())
            ++*failures;
        game_go_ranch();
    }
    return persistent_digest();
}

/* Headlessly play a drill's mini-game with either skilled or sloppy input and
 * return the resulting quality [0,1]. Used by the selftest to prove every
 * mini-game rewards skill and terminates. */
static float drive_minigame(int drill, bool skilled)
{
    enter_drill(drill);
    MinigameState *m = &G.minigame;
    m->phase = 1;
    m->clock = 0.0f;
    m->round = 0;
    m->quality = 0.0f;
    mg_begin_round(m);
    const float dt = 1.0f / 60.0f;
    int guard = 0;
    while (m->phase == 1 && guard++ < 200000) {
        switch (m->type) {
        case MG_TIMING: {
            float mk = triangle_wave(m->clock * m->marker_vel);
            if (skilled ? fabsf(mk - m->target) < 0.02f : m->clock > 0.05f)
                minigame_key(KEY_ENTER);
            else
                minigame_tick(dt);
            break;
        }
        case MG_REACTION:
            if (skilled) {
                if (m->cue_live) minigame_key(KEY_ENTER); else minigame_tick(dt);
            } else {
                if (!m->cue_live && m->clock > 0.1f) minigame_key(KEY_ENTER);
                else minigame_tick(dt);
            }
            break;
        case MG_MEMORY:
            if (m->showing) minigame_tick(dt);
            else minigame_key('1' + (skilled ? m->seq[m->seq_pos]
                                             : ((m->seq[m->seq_pos] + 1) & 3)));
            break;
        case MG_MASH:
            if (skilled) minigame_key(KEY_ENTER);
            minigame_tick(dt);
            break;
        case MG_HOLD:
            if (skilled && fabsf(m->marker - m->target) > 0.03f)
                minigame_key(m->marker > m->target ? KEY_LEFT : KEY_RIGHT);
            minigame_tick(dt);
            break;
        case MG_RHYTHM: {
            /* Aim for the beat centre, not the moment the window opens. */
            float beat = RHY_FIRST + m->round * RHY_PERIOD;
            if (skilled && m->cue_live && fabsf(m->clock - beat) < 0.03f)
                minigame_key(KEY_ENTER);
            else
                minigame_tick(dt);
            break;
        }
        default:
            minigame_tick(dt);
            break;
        }
    }
    return m->phase == 2 ? m->quality : -1.0f;
}

int game_selftest(unsigned seed, int weeks)
{
    GameState original = G;
    bool old_suppression = suppress_autosave;
    uint32_t first_digest;
    uint32_t second_digest;
    uint32_t saved_digest;
    uint32_t unchanged_digest;
    int failures = 0;
    int before;
    int before_wins;
    int before_losses;
    int before_money;
    int frames;
    int i;
    FILE *bad;
    char test_dir[400];
    char test_path[512];

#define SELFTEST_CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "game selftest: %s\n", (message)); \
            ++failures; \
        } \
    } while (0)

    if (weeks < 0)
        weeks = 0;
    if (weeks > 20000)
        weeks = 20000;
    if (seed == 0)
        seed = 0x13579bdfu;
    /* Isolate the save fixture in a private 0700 directory instead of a
     * predictable /tmp/...-<pid>.save that a co-user could pre-plant as a
     * symlink (a classic /tmp TOCTOU). mkdtemp creates it atomically. */
    {
        const char *tmpbase = getenv("TMPDIR");
        if (!tmpbase || !*tmpbase)
            tmpbase = "/tmp";
        snprintf(test_dir, sizeof(test_dir),
                 "%s/kilix-rancher-selftest-XXXXXX", tmpbase);
        if (!mkdtemp(test_dir)) {
            fprintf(stderr, "game selftest: could not create a private temp "
                            "directory: %s\n", strerror(errno));
            return 1;
        }
    }
    snprintf(test_path, sizeof(test_path), "%s/ranch.save", test_dir);
    suppress_autosave = true;

    first_digest = scripted_growth(seed, weeks, &failures);
    second_digest = scripted_growth(seed, weeks, &failures);
    SELFTEST_CHECK(first_digest == second_digest,
                   "identical seeds and choices must produce identical growth");
    SELFTEST_CHECK(G.total_weeks == weeks, "scripted calendar lost or duplicated a week");
    SELFTEST_CHECK(game_year() == weeks / WEEKS_PER_YEAR + 1,
                   "year calculation is incorrect");
    SELFTEST_CHECK(game_week_of_year() == weeks % WEEKS_PER_YEAR + 1,
                   "week-of-year calculation is incorrect");

    /* Calendar boundaries and season boundaries are deliberately exact. */
    G.total_weeks = 11;
    G.kilix.age_weeks = 11;
    SELFTEST_CHECK(game_season() == SEASON_BLOOM && game_week_of_year() == 12,
                   "last Bloomtide week is incorrect");
    game_advance_week();
    SELFTEST_CHECK(game_season() == SEASON_SUN && game_week_of_year() == 13,
                   "Highsun boundary is incorrect");
    G.total_weeks = 47;
    G.kilix.age_weeks = 47;
    SELFTEST_CHECK(game_year() == 1 && game_week_of_year() == 48,
                   "last ranch-year week is incorrect");
    game_advance_week();
    SELFTEST_CHECK(game_year() == 2 && game_week_of_year() == 1
                   && game_season() == SEASON_BLOOM,
                   "ranch-year rollover is incorrect");

    /* Exercise title, naming, ranch, event gating and no-op transactions. */
    game_init(seed, true);
    G.headless = true;
    copy_text(G.save_path, sizeof(G.save_path), test_path);
    G.title_cursor = 1;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_NAMING, "title did not open naming");
    game_handle_key('A');
    game_handle_key('s');
    game_handle_key('h');
    game_handle_key(KEY_BACKSPACE);
    game_handle_key('h');
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_RANCH && strcmp(G.kilix.name, "Ash") == 0,
                   "naming did not create the requested Kilix");
    G.cursor = RANCH_CATNAP;
    before = G.total_weeks;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_EVENT && G.total_weeks == before + 1,
                   "catnap must consume exactly one week");
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_EVENT && G.total_weeks == before + 1,
                   "fresh event accepted key repeat or duplicated its week");
    game_tick(0.15f);
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_RANCH && G.total_weeks == before + 1,
                   "event did not return to ranch cleanly");
    G.cursor = RANCH_CARE;
    game_handle_key(KEY_ENTER);
    G.money = 0;
    before = G.total_weeks;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_CARE && G.total_weeks == before,
                   "unaffordable care item consumed a week");
    game_handle_key(KEY_ESC);
    G.cursor = RANCH_ARENA;
    game_handle_key(KEY_ENTER);
    G.arena_cursor = 1;
    before = G.total_weeks;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_ARENA && G.total_weeks == before,
                   "locked rival started a match or consumed a week");

    /* Win a real deterministic battle and verify one-time settlement/promotion. */
    G.arena_cursor = 0;
    for (i = 0; i < STAT_COUNT; ++i)
        G.kilix.stats.value[i] = 500;
    G.kilix.form = 100;
    G.kilix.bond = 100;
    G.kilix.fatigue = 0;
    G.kilix.stress = 0;
    G.money = 1200;
    /* Esc on the pre-fight Ready prompt must cancel with no loss and no week. */
    before = G.total_weeks;
    before_losses = G.kilix.total_losses;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_BATTLE, "arena did not open the Ready prompt");
    game_handle_key(KEY_ESC);
    SELFTEST_CHECK(G.screen == SCREEN_ARENA && G.total_weeks == before
                   && G.kilix.total_losses == before_losses,
                   "Esc on the battle Ready prompt must cancel with no penalty");
    before = G.total_weeks;
    before_wins = G.kilix.total_wins;
    before_money = G.money;
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_BATTLE, "current-rank rival did not start");
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.battle.phase == BATTLE_ACTIVE,
                   "battle Ready prompt did not begin the match");
    game_use_move(-1);
    for (frames = 0; frames < 12000 && G.screen == SCREEN_BATTLE; ++frames) {
        if (G.battle.phase == BATTLE_ACTIVE) {
            G.battle.enemy_hp = clampf(G.battle.enemy_hp, 0.1f, 1.0f);
            G.battle.distance = 0.17f;
            G.battle.player_guts = 100.0f;
            G.battle.player_cooldown = 0.0f;
            game_use_move(0);
        } else if (G.battle.phase == BATTLE_FINISHED) {
            game_handle_key(KEY_ENTER);
        }
        game_tick(TICK_DT);
        if (!state_invariants()) {
            ++failures;
            break;
        }
    }
    SELFTEST_CHECK(G.screen == SCREEN_EVENT && G.event.kind == EVENT_BATTLE_RESULT,
                   "battle failed to settle into a result event");
    SELFTEST_CHECK(G.total_weeks == before + 1
                   && G.kilix.total_wins == before_wins + 1,
                   "battle accounting was not exactly once");
    SELFTEST_CHECK(G.money == before_money + OPPONENTS[0].prize
                   && G.kilix.rank == 1,
                   "tournament prize or promotion is incorrect");
    before = G.total_weeks;
    before_wins = G.kilix.total_wins;
    for (i = 0; i < 120; ++i)
        game_tick(TICK_DT);
    SELFTEST_CHECK(G.total_weeks == before && G.kilix.total_wins == before_wins,
                   "settled battle was counted more than once");
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_EVENT && G.event.kind == EVENT_RANK_UP,
                   "promotion follow-up event is missing");
    game_tick(0.15f);
    game_handle_key(KEY_ENTER);
    SELFTEST_CHECK(G.screen == SCREEN_RANCH, "promotion did not return to ranch");

    /* Save roundtrip and rejection of corrupt input without partial mutation. */
    copy_text(G.save_path, sizeof(G.save_path), test_path);
    SELFTEST_CHECK(game_save(), "could not write isolated save fixture");
    saved_digest = persistent_digest();
    G.money = 1;
    G.kilix.stats.value[0] = 1;
    G.rng ^= 0xa5a5a5a5u;
    SELFTEST_CHECK(game_load(), "valid isolated save did not load");
    SELFTEST_CHECK(persistent_digest() == saved_digest,
                   "save roundtrip changed persistent state");
    unchanged_digest = persistent_digest();
    bad = fopen(test_path, "wb");
    if (bad) {
        fwrite("bad", 1, 3, bad);
        fclose(bad);
        SELFTEST_CHECK(!game_load(), "truncated save was accepted");
        SELFTEST_CHECK(persistent_digest() == unchanged_digest,
                       "failed load partially mutated the ranch");
    } else {
        SELFTEST_CHECK(false, "could not create corrupt save fixture");
    }

    /* A failed save must raise the sticky warning flag (surfaced on screen so
     * a failing disk is never silent); a subsequent good save clears it. */
    {
        char good_path[512];
        char bad_path[512];
        copy_text(good_path, sizeof(good_path), G.save_path);
        snprintf(bad_path, sizeof(bad_path), "%s/../escape/ranch.save",
                 test_dir);
        copy_text(G.save_path, sizeof(G.save_path), bad_path);
        SELFTEST_CHECK(!game_save(), "save through an unsafe path must fail");
        SELFTEST_CHECK(G.save_failed,
                       "a failed save must raise the sticky warning flag");
        copy_text(G.save_path, sizeof(G.save_path), good_path);
        SELFTEST_CHECK(game_save() && !G.save_failed,
                       "a successful save must clear the sticky warning flag");
    }

    /* A version-2 (pre-economy) ranch record must load with migrated economy
     * defaults, not be rejected as damaged: those saves share the layout with
     * every reserved slot zero. */
    {
        SaveData v2;
        FILE *out;

        fill_save_data(&v2);
        v2.version = 2u;
        memset(v2.reserved, 0, sizeof v2.reserved);
        v2.checksum = 0;
        v2.checksum = hash_bytes(&v2, sizeof v2);
        out = fopen(test_path, "wb");
        if (out) {
            fwrite(SAVE_MAGIC, 1, sizeof SAVE_MAGIC, out);
            fwrite(&v2, 1, sizeof v2, out);
            fclose(out);
            SELFTEST_CHECK(game_load(),
                           "a version-2 ranch record must still load");
            SELFTEST_CHECK(G.moves_known == 1u && G.bank == 0
                           && G.kilix.hunger == 20
                           && G.rent_paid_weeks ==
                              ((int)v2.total_weeks / MONTH_WEEKS + 1)
                                  * MONTH_WEEKS,
                           "v2 migration must apply new-ranch economy defaults");
        } else {
            SELFTEST_CHECK(false, "could not create v2 save fixture");
        }
    }

    SELFTEST_CHECK(game_delete_save(), "isolated save cleanup failed");
    SELFTEST_CHECK(game_delete_save(), "deleting a missing save should succeed");

    {
        char tmp_sibling[640];
        snprintf(tmp_sibling, sizeof(tmp_sibling), "%s.tmp", test_path);
        remove(tmp_sibling);
        remove(test_path);
        rmdir(test_dir);
    }

    /* Auto-battle fights and wins on its own with no manual input. */
    {
        for (i = 0; i < STAT_COUNT; ++i)
            G.kilix.stats.value[i] = 400;
        G.kilix.rank = 0;
        G.kilix.fatigue = G.kilix.stress = 0;
        G.moves_known = (1u << MOVE_COUNT) - 1;   /* full moveset for auto-battle */
        G.screen = SCREEN_ARENA;
        G.arena_cursor = 0;
        game_start_battle(0);
        SELFTEST_CHECK(G.screen == SCREEN_BATTLE, "auto-battle could not start");
        G.battle.phase = BATTLE_ACTIVE;
        G.battle.autopilot = true;
        int guard = 0;
        while (G.battle.phase != BATTLE_FINISHED && G.screen == SCREEN_BATTLE
               && guard++ < 40000)
            game_tick(TICK_DT);
        SELFTEST_CHECK(G.battle.phase == BATTLE_FINISHED,
                       "auto-battle did not resolve without input");
        SELFTEST_CHECK(G.battle.winner > 0,
                       "a strong Kilix on auto-battle should win");
    }

    /* Every drill mini-game must terminate and reward skill over sloppiness. */
    for (int d = 0; d < DRILL_COUNT; d++) {
        float skilled = drive_minigame(d, true);
        float sloppy = drive_minigame(d, false);
        SELFTEST_CHECK(skilled >= 0.0f && sloppy >= 0.0f,
                       "a drill mini-game failed to terminate");
        SELFTEST_CHECK(skilled > sloppy + 0.15f,
                       "skilled drill play must clearly beat sloppy play");
        SELFTEST_CHECK(skilled >= 0.5f,
                       "a well-played drill should score at least half");
    }

    /* ---- economy: rent, hunger, and move-learning ---------------------- */
    {
        int before_life, before_hunger, before_weeks;

        /* Rent: not due before the paid-through week; charged up front once the
         * month elapses; and eviction when bank + coffers can't cover it. */
        G.bank = 0;
        G.total_weeks = 3; G.rent_paid_weeks = 4; G.money = 1500;
        SELFTEST_CHECK(try_collect_rent() == RENT_NOT_DUE && G.money == 1500,
                       "rent charged before the paid-through week");
        G.total_weeks = 4;
        SELFTEST_CHECK(try_collect_rent() == RENT_PAID && G.money == 500
                       && G.rent_paid_weeks == 8,
                       "rent must be collected up front for the month");
        SELFTEST_CHECK(try_collect_rent() == RENT_NOT_DUE,
                       "rent must not double-charge within one month");
        G.total_weeks = 8; G.money = 200;
        SELFTEST_CHECK(try_collect_rent() == RENT_EVICTED
                       && G.rent_paid_weeks == 8 && G.money == 200,
                       "an unpayable rent must evict without touching the ledger");

        /* Bank: transfers move gold between coffers and vault, capped at what is
         * held; rent then draws from the vault first, then the coffers. */
        G.money = 1000; G.bank = 0;
        bank_move(600);
        SELFTEST_CHECK(G.bank == 600 && G.money == 400,
                       "a deposit must move gold into the vault");
        bank_move(-250);
        SELFTEST_CHECK(G.bank == 350 && G.money == 650,
                       "a withdrawal must return gold to the coffers");
        bank_move(9999);
        SELFTEST_CHECK(G.bank == 1000 && G.money == 0,
                       "a deposit must cap at the coffers on hand");
        G.bank = 700; G.money = 500; G.total_weeks = 20; G.rent_paid_weeks = 20;
        SELFTEST_CHECK(try_collect_rent() == RENT_PAID && G.bank == 0 && G.money == 200,
                       "rent must drain the vault first, then the coffers");
        G.bank = 1500; G.money = 50; G.total_weeks = 24; G.rent_paid_weeks = 24;
        SELFTEST_CHECK(try_collect_rent() == RENT_PAID && G.bank == 500 && G.money == 50,
                       "a full vault must pay rent without touching the coffers");
        /* A transfer at the money cap must conserve gold, not clamp it away. */
        G.money = SAVE_MONEY_MAX; G.bank = 500;
        {
            long total_before = (long)G.money + (long)G.bank;
            bank_move(G.money);                 /* deposit all: capped to vault room */
            SELFTEST_CHECK((long)G.money + (long)G.bank == total_before
                           && G.bank <= SAVE_MONEY_MAX,
                           "a capped transfer must not create or destroy gold");
        }

        /* Hunger: a week climbs it and starvation decays stats; food lowers it. */
        G.kilix.hunger = 82;
        G.kilix.stats.value[STAT_LIFE] = 60;
        before_life = G.kilix.stats.value[STAT_LIFE];
        before_hunger = G.kilix.hunger;
        game_advance_week();                         /* 82 -> 96: starving */
        SELFTEST_CHECK(G.kilix.hunger > before_hunger,
                       "hunger must climb every week");
        SELFTEST_CHECK(G.kilix.stats.value[STAT_LIFE] < before_life,
                       "a starving Kilix must lose stats");
        G.money = 2000; G.kilix.hunger = 90;
        before_hunger = G.kilix.hunger;
        use_care_item(0);                            /* Hearth Stew: high satiety */
        SELFTEST_CHECK(G.kilix.hunger < before_hunger,
                       "food must reduce hunger");

        /* Dojo: an advanced move is locked until bought; buying deducts gold and
         * sets exactly its bit with no week spent. */
        G.moves_known = 1u; G.money = 2000;
        before_weeks = G.total_weeks;
        learn_move(1);
        SELFTEST_CHECK((G.moves_known & 2u) && G.money == 2000 - MOVES[1].price
                       && G.total_weeks == before_weeks,
                       "learning a move must cost gold, spend no week, set its bit");
        G.money = 0;
        learn_move(2);
        SELFTEST_CHECK(!(G.moves_known & 4u),
                       "an unaffordable move must not be learned");

        /* A forfeited match spends a week, so at an unpayable rent boundary it
         * must still evict rather than let the player dodge rent by forfeiting. */
        G.moves_known = (1u << MOVE_COUNT) - 1;
        G.kilix.rank = 0;
        G.screen = SCREEN_ARENA;
        G.arena_cursor = 0;
        game_start_battle(0);
        G.battle.phase = BATTLE_ACTIVE;
        G.total_weeks = 12; G.rent_paid_weeks = 12; G.money = 0; G.bank = 0;
        game_handle_key(KEY_ESC);                    /* mid-fight Esc = forfeit */
        SELFTEST_CHECK(G.screen == SCREEN_EVENT && G.event.kind == EVENT_EVICTION,
                       "forfeiting at an unpayable rent boundary must still evict");
    }

    /* A cleared league's rival accepts exhibition rematches: half prize, a
     * week spent, and never a promotion or rank-win credit. */
    {
        int before_money, before_weeks;

        for (i = 0; i < STAT_COUNT; ++i)
            G.kilix.stats.value[i] = 400;
        G.kilix.fatigue = G.kilix.stress = 0;
        G.kilix.rank = 1;
        G.kilix.rank_wins = 0;
        G.total_weeks = 20; G.kilix.age_weeks = 20;
        G.rent_paid_weeks = 48;              /* rent far off: isolate the purse */
        G.money = 1000; G.bank = 0;
        before_money = G.money;
        before_weeks = G.total_weeks;
        G.screen = SCREEN_ARENA;
        G.arena_cursor = 0;
        game_start_battle(0);
        SELFTEST_CHECK(G.screen == SCREEN_BATTLE,
                       "a cleared rival must accept an exhibition rematch");
        G.battle.phase = BATTLE_FINISHED;
        G.battle.winner = 1;
        settle_battle();
        SELFTEST_CHECK(G.money == before_money + OPPONENTS[0].prize / 2,
                       "an exhibition win must pay half the prize");
        SELFTEST_CHECK(G.kilix.rank == 1 && G.kilix.rank_wins == 0,
                       "an exhibition win must not promote or credit rank wins");
        SELFTEST_CHECK(G.total_weeks == before_weeks + 1,
                       "an exhibition match must spend a week");
        game_tick(0.15f);
        game_handle_key(KEY_ENTER);          /* dismiss the result event */
    }

    /* Drill Esc semantics: free only during the get-ready countdown; a live
     * drill forfeits (week spent), and the result screen banks the reward —
     * a completed session can never be discarded and rerolled. */
    {
        int before_weeks, before_stat;

        G.rent_paid_weeks = 48;
        G.screen = SCREEN_DRILL;
        memset(&G.minigame, 0, sizeof G.minigame);
        G.minigame.drill = 0;
        G.minigame.type = drill_minigame(0);
        G.minigame.phase = 0;                /* countdown */
        before_weeks = G.total_weeks;
        game_handle_key(KEY_ESC);
        SELFTEST_CHECK(G.total_weeks == before_weeks
                       && G.screen == SCREEN_TRAINING,
                       "countdown Esc must stay free of charge");

        G.screen = SCREEN_DRILL;
        memset(&G.minigame, 0, sizeof G.minigame);
        G.minigame.drill = 0;
        G.minigame.type = drill_minigame(0);
        G.minigame.phase = 1;                /* live */
        G.minigame.rounds = 3;
        game_handle_key(KEY_ESC);
        SELFTEST_CHECK(G.total_weeks == before_weeks + 1,
                       "abandoning a live drill must still spend the week");
        game_tick(0.15f);
        game_handle_key(KEY_ENTER);

        G.screen = SCREEN_DRILL;
        memset(&G.minigame, 0, sizeof G.minigame);
        G.minigame.drill = 0;
        G.minigame.type = drill_minigame(0);
        G.minigame.phase = 2;                /* result screen */
        G.minigame.quality = 0.9f;
        G.kilix.fatigue = G.kilix.stress = 20;
        before_weeks = G.total_weeks;
        before_stat = G.kilix.stats.value[DRILLS[0].primary];
        game_handle_key(KEY_ESC);
        SELFTEST_CHECK(G.total_weeks == before_weeks + 1
                       && G.kilix.stats.value[DRILLS[0].primary] > before_stat,
                       "Esc on the drill result must bank the session");
        game_tick(0.15f);
        game_handle_key(KEY_ENTER);
    }

    /* Leaving the champion screen must run the deferred rent check: a broke
     * Crown champion cannot dodge eviction by winning title defenses. */
    {
        G.screen = SCREEN_CHAMPION;
        G.kilix.rank = RANK_COUNT - 1;
        G.total_weeks = 24; G.kilix.age_weeks = 24;
        G.rent_paid_weeks = 24;
        G.money = 0; G.bank = 0;
        game_handle_key(KEY_ENTER);
        SELFTEST_CHECK(G.screen == SCREEN_EVENT
                       && G.event.kind == EVENT_EVICTION,
                       "champion exit must still collect overdue rent");
    }

    suppress_autosave = old_suppression;
    G = original;
    if (failures == 0)
        printf("PASS: game selftest seed=%u weeks=%d\n", seed, weeks);
    else
        fprintf(stderr, "FAIL: game selftest seed=%u weeks=%d failures=%d\n",
                seed, weeks, failures);
#undef SELFTEST_CHECK
    return failures;
}
