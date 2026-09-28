/* bashed-earth-lab: headless training and evaluation for the neural gunner.
 * Links the game's own objects, so every shot is the shipped simulation, and
 * runs networks through the game's neural_do_turn() with the candidate
 * swapped in by neural_use_policy().
 *
 * Scenario banks: turns cut from real matches between the classic AIs.
 *   bashed-earth-lab --build-bank F --seed S --count N [--workers W]
 * Scenario k of a bank is match seed S+k, stopped where one AI is about to
 * aim on a turn drawn from the seed. An episode replays that turn with the
 * gunner under test choosing the shot, and ends when the turn passes.
 *   bashed-earth-lab --scenarios F --shooter classic|neural [NET] [--jsonl F]
 * Demonstrations: the classic shooter's weapon, angle and power per turn.
 *   bashed-earth-lab --dump F.bin --scenarios BANK
 * or every decision of N whole classic matches from seed S:
 *   bashed-earth-lab --dump F.bin --seed S --count N
 * Ballistic labels: per bank turn, variants with the wind redrawn and the
 * target moved, each labelled with the exact shot found by simulated search:
 *   bashed-earth-lab --oracle F.bin --scenarios BANK [--variants N]
 * Duels: whole one-on-one matches against the five classic personalities.
 *   bashed-earth-lab --duels N --seed S [--seat NAME | NET] [--jsonl F]
 * Duel k is match seed S+k against personality k % 5, the gunner seated
 * first when (k / 5) is even. NET is --weights F.raw --hidden H or --blob F.
 * Train (evolution strategies, antithetic, rank-shaped, Adam):
 *   bashed-earth-lab --train OUTDIR --bank F --select-bank F [--hidden H]
 *     [--gens G] [--pop P] [--batch B] [--sigma S] [--lr L] [--workers W]
 *     [--init F.raw] [--proximity L] [--check-every N]
 *
 * Match setup (both kinds) is drawn from the seed: a field 700-2000 px wide,
 * the game's own terrain, wind and precipitation choices, wall bounce on
 * three times in four, damage 1x. The lab points XDG_CONFIG_HOME and HOME at
 * a private scratch directory, so it never touches a player's saved setup.
 */
#include "bashed_earth.h"
#include "kilix_game_policy.h"

#include <errno.h>
#include <poll.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

/* The lab links game.o without the terminal, renderer or mixer. */
void sound_play(int id, float vol, float pitch) { (void)id; (void)vol; (void)pitch; }
void sound_set_enabled(bool on) { (void)on; }

#define TRAIN_SEED 1000000u
#define MAX_TURN_TICKS (60 * 60)          /* one turn may not take a minute */
#define MAX_MATCH_TICKS (60L * 60 * 30)   /* nor one match half an hour */
static int check_every = 25;
static float proximity_weight = 0.3f;

static const char *STRAT_NAMES[STRAT_COUNT] = {
    "aggressive", "defensive", "tactical", "balanced", "trickster", "neural",
};

static int strat_by_name(const char *name)
{
    for (int i = 0; i < STRAT_COUNT; i++)
        if (!strcmp(name, STRAT_NAMES[i])) return i;
    return -1;
}

static uint32_t mix32(uint32_t h)
{
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

/* ---------- networks ---------- */

typedef struct {
    int hidden;
    size_t count;
    float *params;
    kilix_policy policy;       /* a view over params, or a loaded blob */
} Net;

static size_t param_count(int hidden)
{
    int w[4] = { POLICY_FEATURES, hidden, hidden, POLICY_OUTPUTS };
    size_t n = 0;
    for (int i = 0; i < 3; i++) n += (size_t)w[i] * (size_t)w[i + 1] + (size_t)w[i + 1];
    return n;
}

static void net_view(Net *n)
{
    memset(&n->policy, 0, sizeof n->policy);
    n->policy.layer_count = 3;
    n->policy.widths[0] = POLICY_FEATURES;
    n->policy.widths[1] = (uint32_t)n->hidden;
    n->policy.widths[2] = (uint32_t)n->hidden;
    n->policy.widths[3] = POLICY_OUTPUTS;
    n->policy.temperature = 1.0f;
    n->policy.parameter_count = n->count;
    n->policy.parameters = n->params;
}

static void net_alloc(Net *n, int hidden)
{
    n->hidden = hidden;
    n->count = param_count(hidden);
    n->params = calloc(n->count, sizeof *n->params);
    if (!n->params) { perror("calloc"); exit(1); }
    net_view(n);
}

static bool net_read(Net *n, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(n->params, sizeof *n->params, n->count, f);
    int extra = fgetc(f);
    fclose(f);
    return got == n->count && extra == EOF;
}

static bool net_write(const Net *n, const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(n->params, sizeof *n->params, n->count, f) == n->count;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, path) == 0;
}

/* ---------- matches ---------- */

static void setup_match(unsigned seed, int players, const int *strategies)
{
    uint32_t r = mix32(seed * 2654435761u ^ 0x85ebca6bu);
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    memset(&G, 0, sizeof G);
    G.headless = true;
    G.soundOn = false;
    G.damageMultiplier = 1.0f;
    G.W = 700 + (int)(r % 1301u);                         /* 700..2000 */
    r = mix32(r);
    G.H = (int)((float)G.W * (0.50f + (float)(r % 1000u) / 1000.0f * 0.15f));
    if (G.H < 420) G.H = 420;
    if (G.H > 1100) G.H = 1100;
    r = mix32(r);
    G.terrainSetting = (int)(r % 4u);
    G.windSetting = (int)((r >> 8) % 5u);
    G.precipSetting = (int)((r >> 16) % 5u);
    G.wallBounce = ((r >> 24) & 3u) != 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        G.pEnabled[i] = i < players;
        G.pStrategy[i] = i < players ? strategies[i] : -1;
    }
    game_reset_to_start();
    game_start_from_menu();              /* headless: every seat shops and plays itself */
}

/* The tick on which game_tick() would call ai_do_turn(). */
static bool at_decision(void)
{
    return G.gameState == GS_PLAYING && G.pendingAIStart > 0 &&
           G.pendingAIStart - TICK_MS <= 0;
}

/* ---------- scenario banks ---------- */

typedef struct {
    uint32_t seed, frand, shooter;
    uint32_t packed;              /* compressed bytes */
    uint8_t *data;                /* zlib(GameState, terrain) */
} Scenario;

typedef struct { int count; Scenario *s; } Bank;

static uint8_t *capture(uLongf *packed)
{
    size_t tsize = terrain_serialized_size(), raw = sizeof G + tsize;
    uint8_t *buf = malloc(raw);
    memcpy(buf, &G, sizeof G);
    terrain_serialize(buf + sizeof G);
    uLongf cap = compressBound((uLong)raw);
    uint8_t *out = malloc(cap);
    if (compress2(out, &cap, buf, (uLong)raw, 1) != Z_OK) { fprintf(stderr, "compress failed\n"); exit(1); }
    free(buf);
    *packed = cap;
    return out;
}

static void restore(const Scenario *sc)
{
    static uint8_t *buf;
    static size_t cap;
    size_t need = sizeof G + 9 + 8192u * 8192u;       /* the largest field terrain accepts */
    if (!buf) { cap = need; buf = malloc(cap); if (!buf) { perror("malloc"); exit(1); } }
    uLongf raw = (uLongf)cap;
    if (uncompress(buf, &raw, sc->data, sc->packed) != Z_OK || raw < sizeof G) {
        fprintf(stderr, "scenario %u is damaged\n", sc->seed);
        exit(1);
    }
    memcpy(&G, buf, sizeof G);
    if (!terrain_deserialize(buf + sizeof G, raw - sizeof G)) {
        fprintf(stderr, "scenario %u terrain is damaged\n", sc->seed);
        exit(1);
    }
    frand_seed(sc->frand);
    srand(sc->seed);
}

/* Play match `seed` between classic AIs until the decision on turn T (drawn
 * from the seed; the last decision if the match ends first). */
static Scenario make_scenario(unsigned seed)
{
    uint32_t r = mix32(seed ^ 0x27d4eb2fu);
    int players = (r % 10u) < 5 ? 2 : (r % 10u) < 7 ? 3 : 4;
    int strategies[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) strategies[i] = (int)(mix32(r + (uint32_t)i + 1) % STRAT_CLASSIC_COUNT);
    int turn = (int)(mix32(r ^ 0x165667b1u) % 25u);
    setup_match(seed, players, strategies);
    Scenario sc = { seed, 0, 0, 0, NULL };
    int seen = 0;
    for (long t = 0; t < MAX_MATCH_TICKS && G.gameState != GS_GAMEOVER; t++) {
        if (at_decision()) {
            G.pendingAIStart = 0;
            if (G.tanks[G.currentPlayer].hp > 0) {
                free(sc.data);
                uLongf packed;
                sc.data = capture(&packed);
                sc.packed = (uint32_t)packed;
                sc.frand = frand_state();
                sc.shooter = (uint32_t)G.currentPlayer;
                if (seen++ == turn) break;
                ai_do_turn();
            }
        }
        game_tick();
    }
    return sc;
}

static bool bank_write_one(FILE *f, const Scenario *sc)
{
    uint32_t head[4] = { sc->seed, sc->frand, sc->shooter, sc->packed };
    return fwrite(head, sizeof head, 1, f) == 1 && fwrite(sc->data, 1, sc->packed, f) == sc->packed;
}

static Bank bank_read(const char *path)
{
    Bank b = { 0, NULL };
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint32_t magic[3];
    if (fread(magic, sizeof magic, 1, f) != 1 || magic[0] != 0x4b4e4142u || magic[1] != (uint32_t)sizeof G) {
        fprintf(stderr, "%s: not a bank for this build\n", path);
        exit(1);
    }
    b.count = (int)magic[2];
    b.s = calloc((size_t)b.count, sizeof *b.s);
    for (int i = 0; i < b.count; i++) {
        uint32_t head[4];
        if (fread(head, sizeof head, 1, f) != 1) { fprintf(stderr, "%s: truncated\n", path); exit(1); }
        Scenario *sc = &b.s[i];
        sc->seed = head[0]; sc->frand = head[1]; sc->shooter = head[2]; sc->packed = head[3];
        sc->data = malloc(sc->packed);
        if (fread(sc->data, 1, sc->packed, f) != sc->packed) { fprintf(stderr, "%s: truncated\n", path); exit(1); }
    }
    fclose(f);
    return b;
}

static void bank_free(Bank *b)
{
    for (int i = 0; i < b->count; i++) free(b->s[i].data);
    free(b->s);
    b->s = NULL;
    b->count = 0;
}

static int build_bank(const char *path, unsigned seed, int count, int workers)
{
    if (workers < 1) workers = 1;
    char part[1100];
    pid_t pids[64];
    if (workers > 64) workers = 64;
    for (int w = 0; w < workers; w++) {
        pids[w] = fork();
        if (pids[w] < 0) { perror("fork"); return 1; }
        if (pids[w] == 0) {
            snprintf(part, sizeof part, "%s.part%d", path, w);
            FILE *f = fopen(part, "wb");
            if (!f) _exit(1);
            for (int k = w; k < count; k += workers) {
                Scenario sc = make_scenario(seed + (unsigned)k);
                if (!sc.data || !bank_write_one(f, &sc)) _exit(1);
                free(sc.data);
            }
            _exit(fclose(f) == 0 ? 0 : 1);
        }
    }
    for (int w = 0; w < workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "bank worker %d failed\n", w); return 1; }
    }
    /* interleave the parts back into seed order */
    FILE *parts[64];
    for (int w = 0; w < workers; w++) {
        snprintf(part, sizeof part, "%s.part%d", path, w);
        parts[w] = fopen(part, "rb");
        if (!parts[w]) { perror(part); return 1; }
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) { perror(tmp); return 1; }
    uint32_t magic[3] = { 0x4b4e4142u, (uint32_t)sizeof G, (uint32_t)count };
    fwrite(magic, sizeof magic, 1, out);
    size_t total = 0;
    for (int k = 0; k < count; k++) {
        FILE *f = parts[k % workers];
        uint32_t head[4];
        if (fread(head, sizeof head, 1, f) != 1) { fprintf(stderr, "short part\n"); return 1; }
        uint8_t *data = malloc(head[3]);
        if (fread(data, 1, head[3], f) != head[3]) { fprintf(stderr, "short part\n"); return 1; }
        fwrite(head, sizeof head, 1, out);
        fwrite(data, 1, head[3], out);
        total += head[3];
        free(data);
    }
    for (int w = 0; w < workers; w++) {
        fclose(parts[w]);
        snprintf(part, sizeof part, "%s.part%d", path, w);
        unlink(part);
    }
    if (fclose(out) != 0 || rename(tmp, path) != 0) { perror(path); return 1; }
    printf("{\"bank\":\"%s\",\"seed\":%u,\"count\":%d,\"mean_packed_bytes\":%zu}\n",
           path, seed, count, count ? total / (size_t)count : 0);
    return 0;
}

/* ---------- one turn ---------- */

enum { SHOOT_CLASSIC, SHOOT_NEURAL };

typedef struct {
    float reward;          /* (damage dealt + 20 per shield broken - self damage + 25 per kill) / 100 */
    float proximity;       /* exp(-d/150), d the closest impact to the target */
    float dealt, self;
    int kills, weapon, hit;
} Turn;

static Turn play_turn(const Scenario *sc, int shooter_kind)
{
    Turn out = { 0, 0, 0, 0, 0, 0, 0 };
    restore(sc);
    int me = (int)sc->shooter;
    int hp0[MAX_PLAYERS], shield0[MAX_PLAYERS];
    for (int i = 0; i < G.numPlayers; i++) { hp0[i] = G.tanks[i].hp; shield0[i] = G.tanks[i].shield; }
    int target = neural_pick_target(me);
    if (target < 0) return out;
    float tx = G.tanks[target].x, ty = G.tanks[target].y - TANK_HEIGHT / 2.0f;
    if (shooter_kind == SHOOT_NEURAL) {
        int keep = G.tanks[me].strategy;
        G.tanks[me].strategy = STRAT_NEURAL;
        ai_do_turn();                                  /* the game's own path to neural_do_turn */
        G.tanks[me].strategy = keep;
    } else {
        ai_do_turn();
    }
    out.weapon = G.currentWeapon;
    /* follow every projectile to where it stops */
    float px[MAX_PROJECTILES], py[MAX_PROJECTILES], best = 1e9f;
    bool was[MAX_PROJECTILES] = { false };
    for (int t = 0; t < MAX_TURN_TICKS; t++) {
        game_tick();
        for (int i = 0; i < MAX_PROJECTILES; i++) {
            const Projectile *p = &G.projectiles[i];
            if (p->active) { px[i] = p->x; py[i] = p->y; was[i] = true; }
            else if (was[i]) {
                float d = hypotf(px[i] - tx, py[i] - ty);
                if (d < best) best = d;
                was[i] = false;
            }
        }
        if (G.gameState == GS_GAMEOVER || (G.gameState == GS_PLAYING && G.currentPlayer != me)) break;
    }
    for (int i = 0; i < G.numPlayers; i++) {
        const Tank *t = &G.tanks[i];
        float lost = (float)(hp0[i] - t->hp);
        if (i == me) { out.self = lost; continue; }
        out.dealt += lost;
        out.kills += hp0[i] > 0 && t->hp <= 0;
        out.reward += 20.0f * (float)(shield0[i] - t->shield);
    }
    out.hit = out.dealt > 0 || out.kills > 0;
    out.reward = (out.reward + out.dealt - out.self + 25.0f * (float)out.kills) / 100.0f;
    out.proximity = best < 1e8f ? expf(-best / 150.0f) : 0.0f;
    return out;
}

/* ---------- demonstrations ---------- */

typedef void (*JobFn)(int index, float *out, void *ctx);
static void parallel_map(int n, int width, int workers, JobFn job, void *ctx, float *results);

/* The classic AIs' own target rule: weakest living opponent, first seat on ties. */
static int classic_target(int shooter)
{
    int best = -1;
    for (int i = 0; i < G.numPlayers; i++)
        if (i != shooter && G.tanks[i].hp > 0 && (best < 0 || G.tanks[i].hp < G.tanks[best].hp))
            best = i;
    return best;
}

#define DEMO_WIDTH (POLICY_FEATURES + 5)   /* features, weapon slot, angle, power, dealt, self */

/* The classic shooter's choice on one scenario, with what it achieved. */
static void demo_job(int i, float *out, void *vctx)
{
    const Bank *b = vctx;
    const Scenario *sc = &b->s[i];
    restore(sc);
    int me = (int)sc->shooter, target = classic_target(me);
    memset(out, 0, sizeof(float) * DEMO_WIDTH);
    if (target < 0) { out[POLICY_FEATURES] = -1; return; }
    neural_features(me, target, out);
    Turn t = play_turn(sc, SHOOT_CLASSIC);
    /* play_turn replays from the same snapshot, so the choice it made is this one */
    restore(sc);
    ai_do_turn();
    const Tank *tank = &G.tanks[me];
    float dir = G.tanks[target].x >= tank->x ? 1.0f : -1.0f;
    int slot = 0;
    for (int k = 0; k < NEURAL_WEAPON_COUNT; k++) if (NEURAL_WEAPONS[k] == G.currentWeapon) slot = k;
    out[POLICY_FEATURES] = (float)slot;
    out[POLICY_FEATURES + 1] = dir > 0 ? tank->angle : 180.0f - tank->angle;
    out[POLICY_FEATURES + 2] = tank->power;
    out[POLICY_FEATURES + 3] = t.dealt;
    out[POLICY_FEATURES + 4] = t.self;
}

static int dump_demos(const Bank *b, const char *path, int workers)
{
    float *r = malloc(sizeof(float) * DEMO_WIDTH * (size_t)b->count);
    parallel_map(b->count, DEMO_WIDTH, workers, demo_job, (void *)b, r);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    int written = 0;
    for (int i = 0; i < b->count; i++) {
        float *e = r + (size_t)DEMO_WIDTH * (size_t)i;
        if (e[POLICY_FEATURES] < 0) continue;
        fwrite(e, sizeof(float), DEMO_WIDTH, f);
        written++;
    }
    fclose(f);
    free(r);
    printf("{\"demos\":\"%s\",\"rows\":%d,\"width\":%d}\n", path, written, DEMO_WIDTH);
    return 0;
}

/* Every classic decision in whole matches: match seed S+k, the same setup as
 * a bank scenario. Each decision's outcome is the damage done before the
 * next decision (or the end of the match). */
#define MATCH_DEMOS 64

static void match_demo_job(int k, float *out, void *vctx)
{
    unsigned seed = *(const unsigned *)vctx + (unsigned)k;
    uint32_t r = mix32(seed ^ 0x27d4eb2fu);
    int players = (r % 10u) < 5 ? 2 : (r % 10u) < 7 ? 3 : 4;
    int strategies[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) strategies[i] = (int)(mix32(r + (uint32_t)i + 1) % STRAT_CLASSIC_COUNT);
    setup_match(seed, players, strategies);
    memset(out, 0, sizeof(float) * (1 + MATCH_DEMOS * DEMO_WIDTH));
    int n = 0, me = -1, hp0[MAX_PLAYERS] = { 0 };
    float *row = NULL;
    for (long t = 0; t <= MAX_MATCH_TICKS; t++) {
        bool decide = at_decision() && G.tanks[G.currentPlayer].hp > 0;
        if (row && (decide || G.gameState == GS_GAMEOVER || t == MAX_MATCH_TICKS)) {
            for (int i = 0; i < G.numPlayers; i++) {
                float lost = (float)(hp0[i] - G.tanks[i].hp);
                if (i == me) row[POLICY_FEATURES + 4] = lost;
                else row[POLICY_FEATURES + 3] += lost;
            }
            row = NULL;
        }
        if (G.gameState == GS_GAMEOVER || t == MAX_MATCH_TICKS) break;
        if (decide && n < MATCH_DEMOS) {
            G.pendingAIStart = 0;
            me = G.currentPlayer;
            int target = classic_target(me);
            if (target >= 0) {
                row = out + 1 + (size_t)n * DEMO_WIDTH;
                neural_features(me, target, row);
                for (int i = 0; i < G.numPlayers; i++) hp0[i] = G.tanks[i].hp;
                ai_do_turn();
                const Tank *tank = &G.tanks[me];
                float dir = G.tanks[target].x >= tank->x ? 1.0f : -1.0f;
                int slot = 0;
                for (int w = 0; w < NEURAL_WEAPON_COUNT; w++) if (NEURAL_WEAPONS[w] == G.currentWeapon) slot = w;
                row[POLICY_FEATURES] = (float)slot;
                row[POLICY_FEATURES + 1] = dir > 0 ? tank->angle : 180.0f - tank->angle;
                row[POLICY_FEATURES + 2] = tank->power;
                n++;
            }
        }
        game_tick();
    }
    out[0] = (float)n;
}

static int dump_match_demos(const char *path, unsigned seed, int matches, int workers)
{
    int width = 1 + MATCH_DEMOS * DEMO_WIDTH;
    float *r = malloc(sizeof(float) * (size_t)width * (size_t)matches);
    if (!r) { perror("malloc"); return 1; }
    parallel_map(matches, width, workers, match_demo_job, &seed, r);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    long rows = 0;
    for (int k = 0; k < matches; k++) {
        const float *m = r + (size_t)width * (size_t)k;
        int n = (int)m[0];
        fwrite(m + 1, sizeof(float), (size_t)n * DEMO_WIDTH, f);
        rows += n;
    }
    fclose(f);
    free(r);
    printf("{\"demos\":\"%s\",\"seed\":%u,\"matches\":%d,\"rows\":%ld,\"width\":%d}\n",
           path, seed, matches, rows, DEMO_WIDTH);
    return 0;
}

/* ---------- ballistic labels ---------- */

/* The game's own flight rule, to the first ground or off the field. */
static bool fly(float x, float y, float angle, float power, float *ox, float *oy)
{
    float a = angle * (float)M_PI / 180, vx = cosf(a) * power * 0.15f, vy = -sinf(a) * power * 0.15f;
    for (int step = 0; step < 600; step++) {
        vy += GRAVITY;
        vx += G.wind * 0.01f;
        x += vx;
        y += vy;
        if (x < 0 || x >= G.W || y > G.H) return false;
        if (terrain_is_ground(x, y)) { *ox = x; *oy = y; return true; }
    }
    return false;
}

/* The exact shot at the target by simulated search: a 50 degree barrel when
 * it can land within 20 px (a hit on the tank lands on the ground about
 * 12 px below its centre), else the first of 60, 70, 80, 40, 30 that can,
 * else the closest landing overall. Power is scanned in 0.25 steps. */
static void ballistic_label(int me, int target, float *angle_m, float *power, float *miss)
{
    static const float angles[6] = { 50, 60, 70, 80, 40, 30 };
    const Tank *t = &G.tanks[me], *tg = &G.tanks[target];
    float dir = tg->x >= t->x ? 1.0f : -1.0f;
    float tx = tg->x, ty = tg->y - TANK_HEIGHT / 2.0f;
    *miss = 1e9f; *angle_m = 50; *power = t->maxPower;
    for (int i = 0; i < 6; i++) {
        float actual = dir > 0 ? angles[i] : 180.0f - angles[i], rad = actual * (float)M_PI / 180;
        float lx = t->x + cosf(rad) * BARREL_LENGTH;
        float ly = t->y - TANK_HEIGHT / 2.0f - sinf(rad) * BARREL_LENGTH;
        for (float p = 10; p <= t->maxPower + 1e-3f; p += 0.25f) {
            float ox, oy;
            if (!fly(lx, ly, actual, p, &ox, &oy)) continue;
            float d = hypotf(ox - tx, oy - ty);
            if (d < *miss) { *miss = d; *angle_m = angles[i]; *power = p; }
        }
        if (*miss < 20) break;
    }
}

/* Per bank turn, `count` variants: the wind redrawn in [-20, 20] and, four
 * times in five, the target moved to a random spot on the surface at least
 * 60 px away. Row: features, then -1 (no weapon label), angle, power, miss. */
static int oracle_count = 25;

static void oracle_job(int i, float *out, void *vctx)
{
    const Bank *b = vctx;
    const Scenario *sc = &b->s[i];
    memset(out, 0, sizeof(float) * (size_t)oracle_count * DEMO_WIDTH);
    for (int v = 0; v < oracle_count; v++) {
        float *row = out + (size_t)v * DEMO_WIDTH;
        restore(sc);
        uint32_t r = mix32(sc->seed * 747796405u + (uint32_t)v * 2891336453u + 1);
        int me = (int)sc->shooter, target = classic_target(me);
        row[POLICY_FEATURES] = -2;                       /* unusable unless filled below */
        if (target < 0) continue;
        G.wind = ((float)(r % 10001u) / 10000.0f - 0.5f) * 40.0f;
        r = mix32(r);
        if (r % 5u) {
            Tank *tg = &G.tanks[target];
            for (int tries = 0; tries < 20; tries++) {
                r = mix32(r);
                float x = 30.0f + (float)(r % 100000u) / 100000.0f * (float)(G.W - 60);
                if (fabsf(x - G.tanks[me].x) < 60) continue;
                tg->x = x;
                tg->y = terrain_get_height(x) - TANK_HEIGHT / 2.0f;
                break;
            }
        }
        neural_features(me, target, row);
        float angle, power, miss;
        ballistic_label(me, target, &angle, &power, &miss);
        row[POLICY_FEATURES] = -1;
        row[POLICY_FEATURES + 1] = angle;
        row[POLICY_FEATURES + 2] = power;
        row[POLICY_FEATURES + 3] = miss;
    }
}

static int dump_oracle(const Bank *b, const char *path, int workers)
{
    int width = oracle_count * DEMO_WIDTH;
    float *r = malloc(sizeof(float) * (size_t)width * (size_t)b->count);
    if (!r) { perror("malloc"); return 1; }
    parallel_map(b->count, width, workers, oracle_job, (void *)b, r);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    long rows = 0;
    for (long i = 0; i < (long)b->count * oracle_count; i++) {
        const float *row = r + (size_t)i * DEMO_WIDTH;
        if (row[POLICY_FEATURES] != -1) continue;
        fwrite(row, sizeof(float), DEMO_WIDTH, f);
        rows++;
    }
    fclose(f);
    free(r);
    printf("{\"oracle\":\"%s\",\"turns\":%d,\"variants\":%d,\"rows\":%ld,\"width\":%d}\n",
           path, b->count, oracle_count, rows, DEMO_WIDTH);
    return 0;
}

/* ---------- duels ---------- */

typedef struct {
    int opponent, seat, winner;      /* winner: 1 gunner, 0 opponent, -1 draw, -2 unfinished */
    int turns, gunner_hp, opponent_hp;
} Duel;

static int seat_strategy = STRAT_NEURAL;   /* --seat: a classic in the gunner's chair */

static Duel play_duel(unsigned seed, int k)
{
    Duel d = { k % STRAT_CLASSIC_COUNT, (k / STRAT_CLASSIC_COUNT) % 2, -2, 0, 0, 0 };
    int strategies[2];
    strategies[d.seat] = seat_strategy;
    strategies[1 - d.seat] = d.opponent;
    setup_match(seed, 2, strategies);
    for (long t = 0; t < MAX_MATCH_TICKS && G.gameState != GS_GAMEOVER; t++) {
        d.turns += at_decision();
        game_tick();
    }
    d.gunner_hp = G.tanks[d.seat].hp;
    d.opponent_hp = G.tanks[1 - d.seat].hp;
    if (G.gameState == GS_GAMEOVER)
        d.winner = G.lastWinnerId < 0 ? -1 : G.lastWinnerId == d.seat ? 1 : 0;
    return d;
}

/* ---------- parallel map over forked workers ----------
 * job(i) for i in [0, n) runs in one of `workers` children; each result is
 * `width` floats written back through a pipe. The game keeps its state in a
 * process-wide global, so processes, not threads. */

static void parallel_map(int n, int width, int workers, JobFn job, void *ctx, float *results)
{
    if (workers < 1) workers = 1;
    if (workers > n) workers = n;
    if (workers == 1) {
        for (int i = 0; i < n; i++) job(i, results + (size_t)i * (size_t)width, ctx);
        return;
    }
    int fds[64][2];
    pid_t pids[64];
    if (workers > 64) workers = 64;
    for (int w = 0; w < workers; w++) {
        if (pipe(fds[w]) != 0) { perror("pipe"); exit(1); }
        pids[w] = fork();
        if (pids[w] < 0) { perror("fork"); exit(1); }
        if (pids[w] == 0) {
            close(fds[w][0]);
            float *buf = malloc(sizeof(float) * (size_t)width);
            for (int i = w; i < n; i += workers) {
                job(i, buf, ctx);
                size_t bytes = sizeof(float) * (size_t)width;
                const char *p = (const char *)buf;
                while (bytes) {
                    ssize_t k = write(fds[w][1], p, bytes);
                    if (k < 0 && errno == EINTR) continue;
                    if (k <= 0) _exit(1);
                    p += k;
                    bytes -= (size_t)k;
                }
            }
            _exit(0);
        }
        close(fds[w][1]);
    }
    /* Drain every pipe as data arrives: a worker whose results outgrow the
       pipe buffer must never wait on the parent reading another worker. */
    size_t row = sizeof(float) * (size_t)width, got[64] = { 0 }, want[64];
    int open_pipes = workers;
    for (int w = 0; w < workers; w++) want[w] = row * (size_t)((n - w + workers - 1) / workers);
    while (open_pipes) {
        struct pollfd pfd[64];
        int map[64], m = 0;
        for (int w = 0; w < workers; w++)
            if (got[w] < want[w]) { pfd[m].fd = fds[w][0]; pfd[m].events = POLLIN; map[m++] = w; }
        if (poll(pfd, (nfds_t)m, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            exit(1);
        }
        for (int j = 0; j < m; j++) {
            if (!(pfd[j].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int w = map[j];
            size_t index = (size_t)w + (got[w] / row) * (size_t)workers, within = got[w] % row;
            char *dst = (char *)(results + index * (size_t)width) + within;
            ssize_t k = read(fds[w][0], dst, row - within);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { fprintf(stderr, "worker %d died\n", w); exit(1); }
            got[w] += (size_t)k;
            if (got[w] == want[w]) open_pipes--;
        }
    }
    for (int w = 0; w < workers; w++) {
        close(fds[w][0]);
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "worker %d failed\n", w);
            exit(1);
        }
    }
}

/* ---------- evaluation ---------- */

static void wilson(int k, int n, double *lo, double *hi)
{
    if (!n) { *lo = *hi = 0; return; }
    double z = 1.96, p = (double)k / n, d = 1 + z * z / n;
    double c = (p + z * z / (2 * n)) / d, h = z * sqrt(p * (1 - p) / n + z * z / (4.0 * n * n)) / d;
    *lo = c - h;
    *hi = c + h;
}

typedef struct { const Bank *bank; int kind; const Net *net; } TurnCtx;

static void turn_job(int i, float *out, void *vctx)
{
    const TurnCtx *c = vctx;
    neural_use_policy(c->net ? &c->net->policy : NULL);
    Turn t = play_turn(&c->bank->s[i], c->kind);
    out[0] = t.reward; out[1] = t.proximity; out[2] = t.dealt; out[3] = t.self;
    out[4] = (float)t.kills; out[5] = (float)t.weapon; out[6] = (float)t.hit;
}

typedef struct { double reward, proximity, dealt, self; int n, kills, hits; } TurnSummary;

static TurnSummary eval_turns(const Bank *b, int kind, const Net *net, int workers, FILE *jsonl)
{
    float *r = malloc(sizeof(float) * 7 * (size_t)b->count);
    TurnCtx c = { b, kind, net };
    parallel_map(b->count, 7, workers, turn_job, &c, r);
    TurnSummary s = { 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < b->count; i++) {
        float *e = r + 7 * i;
        s.n++;
        s.reward += e[0]; s.proximity += e[1]; s.dealt += e[2]; s.self += e[3];
        s.kills += (int)e[4]; s.hits += e[6] > 0;
        if (jsonl)
            fprintf(jsonl, "{\"seed\":%u,\"reward\":%.4f,\"proximity\":%.4f,\"dealt\":%.0f,\"self\":%.0f,"
                           "\"kills\":%d,\"weapon\":\"%s\"}\n",
                    b->s[i].seed, e[0], e[1], e[2], e[3], (int)e[4], WEAPONS[(int)e[5]].name);
    }
    free(r);
    return s;
}

static void print_turns(const char *who, const TurnSummary *s)
{
    double lo, hi;
    wilson(s->hits, s->n, &lo, &hi);
    printf("{\"shooter\":\"%s\",\"turns\":%d,\"mean_reward\":%.4f,\"mean_proximity\":%.4f,"
           "\"mean_dealt\":%.2f,\"mean_self\":%.2f,\"kills\":%d,\"hit_rate\":%.4f,\"hit_wilson95\":[%.4f,%.4f]}\n",
           who, s->n, s->reward / s->n, s->proximity / s->n, s->dealt / s->n, s->self / s->n,
           s->kills, (double)s->hits / s->n, lo, hi);
}

typedef struct { unsigned seed; const Net *net; } DuelCtx;

static void duel_job(int k, float *out, void *vctx)
{
    const DuelCtx *c = vctx;
    neural_use_policy(c->net ? &c->net->policy : NULL);
    Duel d = play_duel(c->seed + (unsigned)k, k);
    out[0] = (float)d.opponent; out[1] = (float)d.seat; out[2] = (float)d.winner;
    out[3] = (float)d.turns; out[4] = (float)d.gunner_hp; out[5] = (float)d.opponent_hp;
}

static int run_duels(unsigned seed, int n, const Net *net, int workers, FILE *jsonl)
{
    float *r = malloc(sizeof(float) * 6 * (size_t)n);
    DuelCtx c = { seed, net };
    parallel_map(n, 6, workers, duel_job, &c, r);
    int games[STRAT_CLASSIC_COUNT] = { 0 }, wins[STRAT_CLASSIC_COUNT] = { 0 };
    int draws = 0, unfinished = 0, total = 0, won = 0, seat_games[2] = { 0 }, seat_wins[2] = { 0 };
    long turns = 0;
    for (int k = 0; k < n; k++) {
        float *e = r + 6 * k;
        int o = (int)e[0], seat = (int)e[1], w = (int)e[2];
        games[o]++; total++; seat_games[seat]++;
        turns += (long)e[3];
        if (w == 1) { wins[o]++; won++; seat_wins[seat]++; }
        draws += w == -1;
        unfinished += w == -2;
        if (jsonl)
            fprintf(jsonl, "{\"seed\":%u,\"k\":%d,\"opponent\":\"%s\",\"seat\":%d,\"result\":\"%s\","
                           "\"turns\":%d,\"gunner_hp\":%d,\"opponent_hp\":%d}\n",
                    seed + (unsigned)k, k, STRAT_NAMES[o], seat,
                    w == 1 ? "win" : w == 0 ? "loss" : w == -1 ? "draw" : "unfinished",
                    (int)e[3], (int)e[4], (int)e[5]);
    }
    double lo, hi;
    wilson(won, total, &lo, &hi);
    printf("{\"gunner\":\"%s\",\"seed\":%u,\"duels\":%d,\"wins\":%d,\"win_rate\":%.4f,\"wilson95\":[%.4f,%.4f],"
           "\"draws\":%d,\"unfinished\":%d,\"mean_turns\":%.1f,",
           seat_strategy == STRAT_NEURAL ? "neural" : STRAT_NAMES[seat_strategy], seed, total, won,
           total ? (double)won / total : 0.0, lo, hi, draws, unfinished, total ? (double)turns / total : 0.0);
    printf("\"first_seat\":{\"duels\":%d,\"wins\":%d},\"second_seat\":{\"duels\":%d,\"wins\":%d},\"by_opponent\":{",
           seat_games[0], seat_wins[0], seat_games[1], seat_wins[1]);
    for (int o = 0; o < STRAT_CLASSIC_COUNT; o++) {
        wilson(wins[o], games[o], &lo, &hi);
        printf("%s\"%s\":{\"duels\":%d,\"wins\":%d,\"win_rate\":%.4f,\"wilson95\":[%.4f,%.4f]}",
               o ? "," : "", STRAT_NAMES[o], games[o], wins[o], games[o] ? (double)wins[o] / games[o] : 0.0, lo, hi);
    }
    printf("}}\n");
    free(r);
    return 0;
}

/* ---------- evolution strategies ---------- */

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Deterministic standard normals for perturbation `member` of generation `gen`. */
static void noise(float *eps, size_t n, unsigned gen, int member)
{
    uint64_t s = ((uint64_t)gen << 32) ^ (uint64_t)(member * 2654435761u) ^ 0x5eedull;
    for (size_t i = 0; i < n; i += 2) {
        double u1 = ((splitmix(&s) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        double u2 = ((splitmix(&s) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        double r = sqrt(-2.0 * log(u1));
        eps[i] = (float)(r * cos(6.283185307179586 * u2));
        if (i + 1 < n) eps[i + 1] = (float)(r * sin(6.283185307179586 * u2));
    }
}

typedef struct {
    const Net *base;
    const Bank *bank;
    const int *batch;       /* scenario indices shared by every member */
    int batch_size;
    float sigma;
    unsigned gen;
} EsCtx;

static void es_job(int member, float *out, void *vctx)
{
    const EsCtx *c = vctx;
    Net n;
    net_alloc(&n, c->base->hidden);
    float *eps = malloc(sizeof(float) * n.count);
    noise(eps, n.count, c->gen, member / 2);
    float sign = member % 2 ? -1.0f : 1.0f;
    for (size_t i = 0; i < n.count; i++) n.params[i] = c->base->params[i] + sign * c->sigma * eps[i];
    neural_use_policy(&n.policy);
    double reward = 0, prox = 0;
    for (int b = 0; b < c->batch_size; b++) {
        Turn t = play_turn(&c->bank->s[c->batch[b]], SHOOT_NEURAL);
        reward += t.reward;
        prox += t.proximity;
    }
    out[0] = (float)((reward + proximity_weight * prox) / c->batch_size);
    out[1] = (float)(reward / c->batch_size);
    free(eps);
    free(n.params);
}

typedef struct { float v; int i; } Ranked;
static int cmp_ranked(const void *a, const void *b)
{
    float x = ((const Ranked *)a)->v, y = ((const Ranked *)b)->v;
    return x < y ? -1 : x > y;
}

static void init_params(Net *n, uint64_t seed)
{
    int w[4] = { POLICY_FEATURES, n->hidden, n->hidden, POLICY_OUTPUTS };
    size_t at = 0;
    for (int l = 0; l < 3; l++) {
        float scale = sqrtf(2.0f / (float)w[l]) * (l == 2 ? 0.1f : 1.0f);
        size_t count = (size_t)w[l] * (size_t)w[l + 1];
        float *tmp = malloc(sizeof(float) * count);
        noise(tmp, count, (unsigned)seed, 1000000 + l);
        for (size_t i = 0; i < count; i++) n->params[at + i] = tmp[i] * scale;
        free(tmp);
        at += count;
        for (int b = 0; b < w[l + 1]; b++) n->params[at++] = 0.0f;
    }
}

static int train(const char *outdir, const Bank *bank, const Bank *select, int hidden, int gens,
                 int pop, int batch, float sigma, float lr, int workers, const char *init)
{
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) { perror(outdir); return 1; }
    char path[1024];
    Net base;
    net_alloc(&base, hidden);
    if (init) {
        if (!net_read(&base, init)) { fprintf(stderr, "cannot read %s\n", init); return 1; }
    } else {
        init_params(&base, 12345);
    }
    size_t n = base.count;
    float *m = calloc(n, sizeof(float)), *v = calloc(n, sizeof(float));
    float *grad = calloc(n, sizeof(float)), *eps = malloc(sizeof(float) * n);
    float *fit = malloc(sizeof(float) * 2 * (size_t)pop);
    Ranked *rank = malloc(sizeof(Ranked) * (size_t)pop);
    int *indices = malloc(sizeof(int) * (size_t)batch);
    snprintf(path, sizeof path, "%s/log.jsonl", outdir);
    FILE *log = fopen(path, "a");
    double best = -1e9;
    time_t t0 = time(NULL);
    for (int g = 1; g <= gens; g++) {
        uint64_t s = TRAIN_SEED + (uint64_t)g * 7919u;
        for (int b = 0; b < batch; b++) indices[b] = (int)(splitmix(&s) % (uint64_t)bank->count);
        EsCtx c = { &base, bank, indices, batch, sigma, (unsigned)g };
        parallel_map(pop, 2, workers, es_job, &c, fit);
        double mean_fit = 0, mean_reward = 0;
        for (int i = 0; i < pop; i++) {
            rank[i].v = fit[2 * i];
            rank[i].i = i;
            mean_fit += fit[2 * i];
            mean_reward += fit[2 * i + 1];
        }
        qsort(rank, (size_t)pop, sizeof *rank, cmp_ranked);
        float *shaped = malloc(sizeof(float) * (size_t)pop);
        for (int r = 0; r < pop; r++) shaped[rank[r].i] = (float)r / (float)(pop - 1) - 0.5f;
        memset(grad, 0, sizeof(float) * n);
        for (int p = 0; p < pop / 2; p++) {
            float w = shaped[2 * p] - shaped[2 * p + 1];
            if (w == 0) continue;
            noise(eps, n, (unsigned)g, p);
            for (size_t i = 0; i < n; i++) grad[i] += w * eps[i];
        }
        free(shaped);
        const float b1 = 0.9f, b2 = 0.999f, wd = 0.005f;
        for (size_t i = 0; i < n; i++) {
            float gi = -grad[i] / ((float)pop * sigma) + wd * base.params[i];
            m[i] = b1 * m[i] + (1 - b1) * gi;
            v[i] = b2 * v[i] + (1 - b2) * gi * gi;
            float mh = m[i] / (1 - powf(b1, (float)g)), vh = v[i] / (1 - powf(b2, (float)g));
            base.params[i] -= lr * mh / (sqrtf(vh) + 1e-8f);
        }
        fprintf(log, "{\"gen\":%d,\"mean_fitness\":%.4f,\"mean_reward\":%.4f,\"seconds\":%ld",
                g, mean_fit / pop, mean_reward / pop, (long)(time(NULL) - t0));
        if (g % check_every == 0 || g == gens) {
            TurnSummary s = eval_turns(select, SHOOT_NEURAL, &base, workers, NULL);
            double score = s.reward / s.n;
            fprintf(log, ",\"select_reward\":%.4f,\"select_hit\":%.4f,\"select_dealt\":%.2f,\"select_self\":%.2f",
                    score, (double)s.hits / s.n, s.dealt / s.n, s.self / s.n);
            snprintf(path, sizeof path, "%s/gen%05d.raw", outdir, g);
            net_write(&base, path);
            if (score > best) {
                best = score;
                snprintf(path, sizeof path, "%s/best.raw", outdir);
                net_write(&base, path);
                snprintf(path, sizeof path, "%s/best.txt", outdir);
                FILE *b = fopen(path, "w");
                if (b) {
                    fprintf(b, "gen %d hidden %d select_reward %.4f select_hit %.4f\n",
                            g, hidden, score, (double)s.hits / s.n);
                    fclose(b);
                }
            }
            fprintf(stderr, "gen %d fitness %.3f train-reward %.3f select reward %.4f hit %.4f dealt %.1f self %.1f %lds\n",
                    g, mean_fit / pop, mean_reward / pop, score, (double)s.hits / s.n,
                    s.dealt / s.n, s.self / s.n, (long)(time(NULL) - t0));
        }
        fprintf(log, "}\n");
        fflush(log);
    }
    fclose(log);
    free(m); free(v); free(grad); free(eps); free(fit); free(rank); free(indices); free(base.params);
    return 0;
}

static char scratch_dir[] = "/tmp/bashed-earth-lab-XXXXXX";

static void remove_scratch(void)
{
    char path[sizeof scratch_dir + 32];
    snprintf(path, sizeof path, "%s/bashed-earth.conf", scratch_dir);
    unlink(path);
    rmdir(scratch_dir);
}

int main(int argc, char **argv)
{
    int hidden = 32, workers = 12, gens = 300, pop = 64, batch = 64, count = 1000, duels = 0;
    float sigma = 0.05f, lr = 0.02f;
    unsigned seed = 1;
    const char *weights = NULL, *blob = NULL, *train_dir = NULL, *jsonl = NULL, *init = NULL;
    const char *bank_path = NULL, *select_path = NULL, *build = NULL, *scenarios = NULL, *dump = NULL, *oracle = NULL;
    int shooter = SHOOT_NEURAL;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "--build-bank")) build = v;
        else if (!strcmp(a, "--count")) count = atoi(v);
        else if (!strcmp(a, "--scenarios")) scenarios = v;
        else if (!strcmp(a, "--dump")) dump = v;
        else if (!strcmp(a, "--oracle")) oracle = v;
        else if (!strcmp(a, "--variants")) oracle_count = atoi(v) > 0 ? atoi(v) : 25;
        else if (!strcmp(a, "--shooter")) shooter = !strcmp(v, "classic") ? SHOOT_CLASSIC : SHOOT_NEURAL;
        else if (!strcmp(a, "--duels")) duels = atoi(v);
        else if (!strcmp(a, "--seat")) {
            seat_strategy = strat_by_name(v);
            if (seat_strategy < 0) { fprintf(stderr, "unknown personality %s\n", v); return 2; }
        }
        else if (!strcmp(a, "--seed")) seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--workers")) workers = atoi(v);
        else if (!strcmp(a, "--hidden")) hidden = atoi(v);
        else if (!strcmp(a, "--weights")) weights = v;
        else if (!strcmp(a, "--blob")) blob = v;
        else if (!strcmp(a, "--jsonl")) jsonl = v;
        else if (!strcmp(a, "--train")) train_dir = v;
        else if (!strcmp(a, "--bank")) bank_path = v;
        else if (!strcmp(a, "--select-bank")) select_path = v;
        else if (!strcmp(a, "--init")) init = v;
        else if (!strcmp(a, "--gens")) gens = atoi(v);
        else if (!strcmp(a, "--pop")) pop = atoi(v) & ~1;
        else if (!strcmp(a, "--batch")) batch = atoi(v);
        else if (!strcmp(a, "--sigma")) sigma = strtof(v, NULL);
        else if (!strcmp(a, "--lr")) lr = strtof(v, NULL);
        else if (!strcmp(a, "--proximity")) proximity_weight = strtof(v, NULL);
        else if (!strcmp(a, "--check-every")) check_every = atoi(v) > 0 ? atoi(v) : 25;
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    /* Keep the lab away from any real per-user state. */
    if (!mkdtemp(scratch_dir)) { perror("mkdtemp"); return 1; }
    setenv("XDG_CONFIG_HOME", scratch_dir, 1);
    setenv("HOME", scratch_dir, 1);
    atexit(remove_scratch);

    if (build) return build_bank(build, seed, count, workers);
    if (oracle) {
        if (!scenarios) { fprintf(stderr, "--oracle needs --scenarios BANK\n"); return 2; }
        Bank b = bank_read(scenarios);
        int rc = dump_oracle(&b, oracle, workers);
        bank_free(&b);
        return rc;
    }
    if (dump && !scenarios) return dump_match_demos(dump, seed, count, workers);
    if (dump) {
        Bank b = bank_read(scenarios);
        int rc = dump_demos(&b, dump, workers);
        bank_free(&b);
        return rc;
    }
    if (train_dir) {
        if (!bank_path || !select_path) { fprintf(stderr, "--train needs --bank and --select-bank\n"); return 2; }
        Bank b = bank_read(bank_path), s = bank_read(select_path);
        int rc = train(train_dir, &b, &s, hidden, gens, pop, batch, sigma, lr, workers, init);
        bank_free(&b);
        bank_free(&s);
        return rc;
    }

    Net net = { 0 };
    bool use_net = (scenarios && shooter == SHOOT_NEURAL) || (duels && seat_strategy == STRAT_NEURAL);
    if (use_net) {
        if (blob) {
            FILE *f = fopen(blob, "rb");
            if (!f) { perror(blob); return 1; }
            static uint8_t buf[1 << 20];
            size_t size = fread(buf, 1, sizeof buf, f);
            fclose(f);
            kilix_policy_status st = kilix_policy_load(&net.policy, buf, size);
            if (st != KILIX_POLICY_OK) { fprintf(stderr, "blob: %s\n", kilix_policy_status_string(st)); return 1; }
            if (kilix_policy_input_count(&net.policy) != POLICY_FEATURES ||
                kilix_policy_output_count(&net.policy) != POLICY_OUTPUTS) {
                fprintf(stderr, "blob shape does not match the game\n");
                return 1;
            }
        } else if (weights) {
            net_alloc(&net, hidden);
            if (!net_read(&net, weights)) { fprintf(stderr, "cannot read %s (hidden %d)\n", weights, hidden); return 1; }
        } else {
            fprintf(stderr, "need --weights F --hidden H or --blob F (or --seat NAME / --shooter classic)\n");
            return 2;
        }
    }
    FILE *jf = jsonl ? fopen(jsonl, "w") : NULL;
    int rc = 0;
    if (scenarios) {
        Bank b = bank_read(scenarios);
        TurnSummary s = eval_turns(&b, shooter, use_net ? &net : NULL, workers, jf);
        print_turns(shooter == SHOOT_NEURAL ? "neural" : "classic", &s);
        bank_free(&b);
    } else if (duels) {
        rc = run_duels(seed, duels, use_net ? &net : NULL, workers, jf);
    } else {
        fprintf(stderr, "nothing to do: --build-bank, --scenarios, --duels or --train\n");
        rc = 2;
    }
    if (jf) fclose(jf);
    if (blob && use_net) kilix_policy_free(&net.policy);
    free(net.params);
    return rc;
}
