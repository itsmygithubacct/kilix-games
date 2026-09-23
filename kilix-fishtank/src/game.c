/* Fishtank simulation, entity steering, and input. */
#include "fishtank.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f

enum {
    SHARK_SWEEP_NONE = 0,
    SHARK_SWEEP_EXIT,
    SHARK_SWEEP_WAIT,
    SHARK_SWEEP_RETURN
};

GameState G;

const FishPalette FISH_PALETTES[FISH_PALETTE_COUNT] = {
    { 0xffa500, 0xffcc64, 0x050505 },
    { 0x1d74d8, 0xfacc15, 0x050505 },
    { 0xff6a00, 0xf8fafc, 0x050505 },
    { 0x22d3ee, 0xff4d6d, 0x050505 },
    { 0x74c476, 0xff79c6, 0x050505 },
    { 0x9333ea, 0xc4b5fd, 0x050505 },
    { 0xdc2626, 0xfca5a5, 0x050505 },
    { 0xfacc15, 0xfef08a, 0x050505 },
};

float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void frand_seed(uint32_t seed)
{
    G.rng = seed ? seed : 0x81f5u;
}

float frandf(void)
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return (G.rng >> 8) * (1.0f / 16777216.0f);
}

static float frandr(float lo, float hi)
{
    return lo + (hi - lo) * frandf();
}

static int irandr(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(frandf() * (float)(hi - lo + 1));
}

static float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx;
    float dy = ay - by;
    return dx * dx + dy * dy;
}

static int sign_dir(float v)
{
    return v >= 0.0f ? 1 : -1;
}

static void set_fish_direction(Fish *f, int dir)
{
    dir = dir >= 0 ? 1 : -1;
    if (f->direction != dir) {
        f->direction = dir;
        f->turnTimer = 0.62f;
    }
}

static void set_shark_direction(Shark *sh, int dir)
{
    dir = dir >= 0 ? 1 : -1;
    if (sh->direction != dir) {
        sh->direction = dir;
        sh->turnTimer = 0.82f;
    }
}

static void advance_facing(float *facing, int dir, float rate, float dt)
{
    float target = dir >= 0 ? 1.0f : -1.0f;
    float delta = target - *facing;
    float maxStep = rate * dt;
    if (fabsf(delta) <= maxStep)
        *facing = target;
    else
        *facing += delta > 0.0f ? maxStep : -maxStep;
}

static float turn_throttle(float timer, float fullTurnTime)
{
    return 1.0f - 0.42f * clampf(timer / fullTurnTime, 0.0f, 1.0f);
}

static float sim_water_surface_y(float x)
{
    if (G.W <= 1) return (float)G.waterY;
    float fx = clampf(x / (float)(G.W - 1), 0.0f, 1.0f) * (WATER_NODES - 1);
    int i = (int)floorf(fx);
    if (i < 0) i = 0;
    if (i >= WATER_NODES - 1)
        return G.waterY + G.waterLevel[WATER_NODES - 1];
    float t = fx - i;
    return G.waterY + G.waterLevel[i] * (1.0f - t) + G.waterLevel[i + 1] * t;
}

static int active_ship_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SHIPS; i++)
        if (G.ships[i].active) n++;
    return n;
}

static int sole_ship_index(void)
{
    int found = -1;
    for (int i = 0; i < MAX_SHIPS; i++) {
        if (!G.ships[i].active) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

static void add_particle(float x, float y, float vx, float vy,
                         float life, float size, uint32_t color)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) {
            *p = (Particle){
                .active = true, .x = x, .y = y, .vx = vx, .vy = vy,
                .life = life, .maxLife = life, .size = size, .color = color
            };
            return;
        }
    }
}

static void sparkle(float x, float y, uint32_t color, int count, float power)
{
    for (int i = 0; i < count; i++) {
        float a = frandr(0.0f, PI * 2.0f);
        float sp = frandr(power * 0.25f, power);
        add_particle(x + frandr(-2, 2) * G.scale, y + frandr(-2, 2) * G.scale,
                     cosf(a) * sp, sinf(a) * sp,
                     frandr(0.18f, 0.65f), frandr(1.4f, 3.8f) * G.scale, color);
    }
}

static void blood_spray(float x, float y, int dir, int count, float power)
{
    static const uint32_t reds[] = { 0x7f0612, 0xb80f1f, 0x4b0208, 0xdc2626 };
    float s = G.scale;
    dir = dir >= 0 ? 1 : -1;
    for (int i = 0; i < count; i++) {
        float a = frandr(-PI * 0.78f, PI * 0.78f);
        float back = frandr(power * 0.16f, power);
        float side = frandr(-power * 0.42f, power * 0.42f);
        add_particle(x + frandr(-4.0f, 4.0f) * s, y + frandr(-4.0f, 4.0f) * s,
                     -dir * back + cosf(a) * side,
                     sinf(a) * power * 0.36f + frandr(-18.0f, 24.0f) * s,
                     frandr(0.32f, 0.95f), frandr(2.0f, 6.2f) * s,
                     reds[irandr(0, 3)]);
    }
}

static void start_shark_bite(Shark *sh, Fish *f, float s)
{
    if (!sh || !f) return;

    int slot = -1;
    float lowestLife = 1e30f;
    for (int i = 0; i < MAX_SHARK_BITES; i++) {
        if (!G.sharkBites[i].active) {
            slot = i;
            break;
        }
        if (G.sharkBites[i].life < lowestLife) {
            lowestLife = G.sharkBites[i].life;
            slot = i;
        }
    }
    if (slot < 0) return;

    float face = fabsf(sh->facing) > 0.05f ? sh->facing : (float)sh->direction;
    int dir = face >= 0.0f ? 1 : -1;
    float mouthX = sh->x + dir * 54.0f * s;
    float mouthY = sh->y + 3.0f * s;
    int sharkIndex = (int)(sh - G.sharks);
    if (sharkIndex < 0 || sharkIndex >= MAX_SHARKS) sharkIndex = -1;

    G.sharkBites[slot] = (SharkBite){
        .active = true,
        .x = f->x,
        .y = f->y,
        .mouthX = mouthX,
        .mouthY = mouthY,
        .life = 1.18f,
        .maxLife = 1.18f,
        .victimSize = f->size,
        .victimFacing = fabsf(f->facing) > 0.05f ? f->facing : (float)f->direction,
        .direction = dir,
        .species = f->species,
        .palette = f->palette,
        .sharkIndex = sharkIndex,
        .seed = G.rng ^ ((uint32_t)G.frameCount * 2654435761u) ^ (uint32_t)(slot * 977u)
    };

    blood_spray(mouthX, mouthY, dir, 22, 116.0f * s);
}

static void award_food_score(const Fish *f)
{
    int sizeBonus = f ? (int)(f->size * 3.0f) : 0;
    int frenzyBonus = G.frenzyTimer > 0.0f ? 12 : 0;
    G.combo++;
    if (G.combo > G.bestCombo) G.bestCombo = G.combo;
    G.comboTimer = 2.8f;
    G.score += 10 + sizeBonus + frenzyBonus + G.combo * 2;
    G.level = 1 + G.score / 500;
    G.scorePulse = 0.38f;
    G.frenzyMeter = clampf(G.frenzyMeter + 7.5f + G.combo * 0.55f, 0.0f, 100.0f);
    audio_play(AUDIO_FISH_BITE, 0.22f,
               f ? clampf(1.18f - f->size * 0.06f, 0.86f, 1.18f) : 1.0f);
    if (G.frenzyMeter >= 100.0f) {
        G.frenzyMeter = 0.0f;
        G.frenzyTimer = 7.5f;
        G.scorePulse = 0.75f;
        audio_play(AUDIO_FRENZY, 0.62f, 1.0f);
    }
}

static void shark_penalty(void)
{
    G.combo = 0;
    G.comboTimer = 0.0f;
    G.dangerPulse = 0.72f;
    if (G.score > 0) {
        G.score -= 25;
        if (G.score < 0) G.score = 0;
    }
}

static void add_bubble_at(float x, float y, float size, float speed)
{
    for (int i = 0; i < MAX_BUBBLES; i++) {
        Bubble *b = &G.bubbles[i];
        if (!b->active) {
            *b = (Bubble){
                .active = true, .x = x, .y = y, .size = size,
                .speed = speed, .phase = frandr(0.0f, PI * 2.0f)
            };
            return;
        }
    }
}

static void disturb_water(float x, float impulse)
{
    if (G.W <= 1) return;
    float fx = clampf(x / (float)G.W, 0.0f, 0.999f) * (WATER_NODES - 1);
    int c = (int)fx;
    for (int i = c - 6; i <= c + 6; i++) {
        if (i < 0 || i >= WATER_NODES) continue;
        float d = fabsf((float)i - fx);
        float k = fmaxf(0.0f, 1.0f - d / 6.4f);
        k = k * k * (3.0f - 2.0f * k);
        G.waterVel[i] += impulse * k;
        G.waterLevel[i] += impulse * 0.020f * k;
        G.waterFoam[i] = clampf(G.waterFoam[i] + fabsf(impulse) * 0.030f * k,
                                0.0f, 1.35f);
    }
}

static void add_plant(float x)
{
    for (int i = 0; i < MAX_PLANTS; i++) {
        Plant *p = &G.plants[i];
        if (!p->active) {
            int type = irandr(0, ENV_PLANT_ASSET_COUNT - 1);
            static const float minH[ENV_PLANT_ASSET_COUNT] = { 74.0f, 48.0f, 30.0f, 42.0f };
            static const float maxH[ENV_PLANT_ASSET_COUNT] = { 126.0f, 78.0f, 58.0f, 82.0f };
            *p = (Plant){
                .active = true,
                .x = clampf(x, 12.0f * G.scale, G.W - 12.0f * G.scale),
                .height = frandr(minH[type], maxH[type]) * G.scale,
                .phase = frandr(0.0f, PI * 2.0f),
                .sway = frandr(0.45f, 1.35f),
                .scale = frandr(0.82f, 1.18f),
                .type = type,
                .flip = frandf() < 0.5f
            };
            return;
        }
    }
}

static void add_fish(void)
{
    float margin = 42.0f * G.scale;
    float top = G.waterY + 34.0f * G.scale;
    float bottom = G.groundY - 54.0f * G.scale;
    if (bottom < top + 20.0f) bottom = top + 20.0f;

    for (int i = 0; i < MAX_FISH; i++) {
        Fish *f = &G.fish[i];
        if (!f->active) {
            float y = frandr(top, bottom);
            float x = frandr(margin, G.W - margin);
            int species = irandr(0, FISH_SPECIES_COUNT - 1);
            *f = (Fish){
                .active = true,
                .x = x,
                .y = y,
                .size = frandr(0.70f, 2.25f),
                .speed = frandr(23.0f, 70.0f) * G.scale,
                .direction = frandf() < 0.5f ? -1 : 1,
                .facing = 0.0f,
                .turnTimer = 0.0f,
                .species = species,
                .palette = irandr(0, FISH_PALETTE_COUNT - 1),
                .tailPhase = frandr(0.0f, PI * 2.0f),
                .targetX = frandr(margin, G.W - margin),
                .targetY = y,
                .wobble = frandr(0.0f, PI * 2.0f),
                .attackTimer = 0.0f,
                .biteTimer = 0.0f,
                .attackX = 0.0f,
                .attackY = 0.0f
            };
            f->facing = (float)f->direction;
            return;
        }
    }
}

static void add_shark(void)
{
    float s = G.scale;
    for (int i = 0; i < MAX_SHARKS; i++) {
        Shark *sh = &G.sharks[i];
        if (!sh->active) {
            *sh = (Shark){
                .active = true,
                .x = frandf() < 0.5f ? 48.0f * s : G.W - 48.0f * s,
                .y = frandr(G.waterY + 72.0f * s, G.groundY - 84.0f * s),
                .direction = frandf() < 0.5f ? -1 : 1,
                .tailPhase = frandr(0.0f, PI * 2.0f),
                .jawPhase = frandr(0.0f, PI * 2.0f),
                .facing = 0.0f,
                .turnTimer = 0.0f,
                .vx = 0.0f,
                .vy = 0.0f,
                .targetX = frandr(70.0f * s, G.W - 70.0f * s),
                .targetY = frandr(G.waterY + 84.0f * s, G.groundY - 88.0f * s),
                .lastX = 0.0f,
                .lastY = 0.0f,
                .repathTimer = frandr(0.12f, 0.55f),
                .stuckTimer = 0.0f,
                .chargeTimer = 0.0f,
                .sweepTimer = 0.0f,
                .sweepCooldown = frandr(7.0f, 16.0f),
                .sweepMode = SHARK_SWEEP_NONE,
                .targetFish = -1,
                .attackTimer = 0.0f
            };
            sh->facing = (float)sh->direction;
            sh->vx = sh->direction * 44.0f * s;
            sh->lastX = sh->x;
            sh->lastY = sh->y;
            audio_play(AUDIO_SHARK_ALERT, 0.42f, 1.0f);
            return;
        }
    }
}

static void add_ship(void)
{
    float s = G.scale;
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *ship = &G.ships[i];
        if (!ship->active) {
            *ship = (Ship){
                .active = true,
                .x = frandr(80.0f * s, G.W - 80.0f * s),
                .y = G.waterY + 45.0f * s,
                .direction = frandf() < 0.5f ? -1 : 1,
                .facing = 0.0f,
                .bobPhase = frandr(0.0f, PI * 2.0f),
                .wavePhase = frandr(0.0f, PI * 2.0f),
                .health = 3,
                .reload = frandr(0.55f, 1.7f),
                .cannonFlash = 0.0f,
                .fishing = false,
                .hookX = 0.0f,
                .hookY = 0.0f,
                .hookTargetX = 0.0f,
                .hookTargetY = 0.0f,
                .hookVy = 0.0f,
                .fishingPulse = 0.0f,
                .hookedFish = -1,
                .biteFish = -1,
                .biteTimer = 0.0f,
                .catchMeter = 0.0f,
                .tension = 0.0f,
                .reelPower = 0.0f
            };
            ship->facing = (float)ship->direction;
            return;
        }
    }
}

void game_add_food_at(float x, float y, int amount)
{
    float s = G.scale;
    if (amount < 1) amount = 1;
    if (y < G.waterY + 5.0f * s) y = G.waterY + 5.0f * s;
    if (y > G.groundY - 8.0f * s) y = G.groundY - 8.0f * s;
    disturb_water(x, 7.0f * s);

    for (int n = 0; n < amount; n++) {
        for (int i = 0; i < MAX_FOOD; i++) {
            Food *f = &G.food[i];
            if (!f->active) {
                *f = (Food){
                    .active = true,
                    .x = clampf(x + frandr(-18.0f, 18.0f) * s, 8.0f * s, G.W - 8.0f * s),
                    .y = y + frandr(-3.0f, 10.0f) * s,
                    .vy = frandr(16.0f, 34.0f) * s,
                    .life = frandr(6.0f, 10.0f)
                };
                break;
            }
        }
    }
    G.feedPulse = 0.55f;
    audio_play(AUDIO_FEED_SPLASH, 0.34f,
               0.94f + 0.03f * (float)(amount > 4 ? 4 : amount));
}

static void clear_sharks(void)
{
    for (int i = 0; i < MAX_SHARKS; i++)
        G.sharks[i].active = false;
}

int game_count_fish(void)
{
    int n = 0;
    for (int i = 0; i < MAX_FISH; i++)
        if (G.fish[i].active) n++;
    return n;
}

int game_count_sharks(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SHARKS; i++)
        if (G.sharks[i].active) n++;
    return n;
}

int game_count_food(void)
{
    int n = 0;
    for (int i = 0; i < MAX_FOOD; i++)
        if (G.food[i].active) n++;
    return n;
}

int game_count_ships(void)
{
    return active_ship_count();
}

int game_count_cannonballs(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CANNONBALLS; i++)
        if (G.cannonballs[i].active) n++;
    return n;
}

void game_reset_world(void)
{
    float sx = G.W / 1000.0f;
    float sy = G.H / 640.0f;
    G.scale = clampf(fminf(sx, sy), 0.62f, 1.75f);
    float s = G.scale;

    G.groundY = G.H - (int)(58.0f * s);
    if (G.groundY < (int)(G.H * 0.68f)) G.groundY = (int)(G.H * 0.68f);
    if (G.groundY > G.H - 28) G.groundY = G.H - 28;
    /* Keep the simulated surface below the HUD so surface assets can ride it. */
    G.waterY = (int)(166.0f * s);
    if (G.waterY < (int)(68.0f * s)) G.waterY = (int)(68.0f * s);
    if (G.waterY > G.groundY - (int)(220.0f * s))
        G.waterY = G.groundY - (int)(220.0f * s);
    if (G.waterY < 36) G.waterY = 36;

    memset(G.fish, 0, sizeof G.fish);
    memset(G.sharks, 0, sizeof G.sharks);
    memset(G.ships, 0, sizeof G.ships);
    memset(G.sharkBites, 0, sizeof G.sharkBites);
    memset(G.cannonballs, 0, sizeof G.cannonballs);
    memset(G.bubbles, 0, sizeof G.bubbles);
    memset(G.plants, 0, sizeof G.plants);
    memset(G.food, 0, sizeof G.food);
    memset(G.rocks, 0, sizeof G.rocks);
    memset(G.particles, 0, sizeof G.particles);
    G.numRocks = 0;
    G.paused = false;
    G.bubbleTimer = 0.0f;
    G.feedPulse = 0.0f;
    G.score = 0;
    G.combo = 0;
    G.bestCombo = 0;
    G.level = 1;
    G.comboTimer = 0.0f;
    G.frenzyMeter = 0.0f;
    G.frenzyTimer = 0.0f;
    G.scorePulse = 0.0f;
    G.dangerPulse = 0.0f;
    G.currentPhase = frandr(0.0f, PI * 2.0f);
    memset(G.waterLevel, 0, sizeof G.waterLevel);
    memset(G.waterVel, 0, sizeof G.waterVel);
    memset(G.waterFoam, 0, sizeof G.waterFoam);

    G.numRocks = irandr(6, 9);
    if (G.W > 1100) G.numRocks += 2;
    if (G.numRocks > MAX_ROCKS) G.numRocks = MAX_ROCKS;
    for (int i = 0; i < G.numRocks; i++) {
        float w = frandr(36.0f, 96.0f) * s;
        G.rocks[i] = (Rock){
            .x = frandr(28.0f * s, G.W - 28.0f * s),
            .w = w,
            .h = frandr(22.0f, 54.0f) * s,
            .type = irandr(0, ENV_ROCK_ASSET_COUNT - 1),
            .flip = frandf() < 0.5f
        };
    }

    int plantCount = 12 + G.W / 90;
    if (plantCount > MAX_PLANTS) plantCount = MAX_PLANTS;
    for (int i = 0; i < plantCount; i++)
        add_plant(frandr(18.0f * s, G.W - 18.0f * s));

    int fishCount = 11 + G.W / 210;
    if (fishCount > 24) fishCount = 24;
    for (int i = 0; i < fishCount; i++)
        add_fish();

    G.mouseX = G.W / 2;
    G.mouseY = G.groundY / 3;
    G.worldVersion++;
}

void game_init(int w, int h, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    G.W = w;
    G.H = h;
    frand_seed(seed);
    game_reset_world();
}

void game_shutdown(void)
{
}

static int nearest_food(float x, float y, float *outDist2)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < MAX_FOOD; i++) {
        Food *f = &G.food[i];
        if (!f->active) continue;
        float d = dist2(x, y, f->x, f->y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (outDist2) *outDist2 = bd;
    return best;
}

static int nearest_shark(float x, float y, float *outDist2)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < MAX_SHARKS; i++) {
        Shark *s = &G.sharks[i];
        if (!s->active) continue;
        float d = dist2(x, y, s->x, s->y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (outDist2) *outDist2 = bd;
    return best;
}

static int nearest_fish(float x, float y, float *outDist2)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < MAX_FISH; i++) {
        Fish *f = &G.fish[i];
        if (!f->active) continue;
        float d = dist2(x, y, f->x, f->y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (outDist2) *outDist2 = bd;
    return best;
}

static int shark_choose_target(const Shark *sh, float *outDist2)
{
    int best = -1;
    float bestScore = 1e30f;
    float bestD2 = 1e30f;
    float s = G.scale;
    for (int i = 0; i < MAX_FISH; i++) {
        const Fish *f = &G.fish[i];
        if (!f->active) continue;
        float dx = f->x - sh->x;
        float dy = f->y - sh->y;
        float d2 = dx * dx + dy * dy;
        float d = sqrtf(fmaxf(d2, 1.0f));
        float edgeVulnerability = 0.0f;
        if (f->x < 95.0f * s || f->x > G.W - 95.0f * s)
            edgeVulnerability += 42.0f * s;
        if (f->y < G.waterY + 86.0f * s || f->y > G.groundY - 78.0f * s)
            edgeVulnerability += 24.0f * s;
        float facingBonus = (sh->direction > 0 && dx > 0.0f) || (sh->direction < 0 && dx < 0.0f)
                          ? 36.0f * s : 0.0f;
        float sizeBonus = f->size * 15.0f * s;
        float stickyBonus = i == sh->targetFish ? 58.0f * s : 0.0f;
        float score = d - edgeVulnerability - facingBonus - sizeBonus - stickyBonus;
        if (score < bestScore) {
            bestScore = score;
            bestD2 = d2;
            best = i;
        }
    }
    if (outDist2) *outDist2 = bestD2;
    return best;
}

static void update_food(float dt)
{
    float s = G.scale;
    for (int i = 0; i < MAX_FOOD; i++) {
        Food *f = &G.food[i];
        if (!f->active) continue;
        f->vy += 5.0f * s * dt;
        f->y += f->vy * dt;
        f->x += sinf(G.frameCount * 0.015f + f->x * 0.017f) * 0.08f * s;
        f->life -= dt;
        if (f->life <= 0.0f || f->y > G.groundY - 4.0f * s)
            f->active = false;
    }
}

static void update_water(float dt)
{
    float s = G.scale;
    float damping = powf(0.30f, dt);
    float tension = 92.0f;
    float restoring = 24.0f;
    float spread = 0.115f;
    float next[WATER_NODES];

    for (int i = 0; i < WATER_NODES; i++) {
        float left = i > 0 ? G.waterLevel[i - 1] : G.waterLevel[i];
        float right = i + 1 < WATER_NODES ? G.waterLevel[i + 1] : G.waterLevel[i];
        float curvature = left + right - G.waterLevel[i] * 2.0f;
        float wind = sinf(G.currentPhase * 1.75f + i * 0.43f) * 0.032f * s;
        G.waterVel[i] += (curvature * tension - G.waterLevel[i] * restoring) * dt + wind;
        G.waterVel[i] *= damping;
        next[i] = G.waterLevel[i] + G.waterVel[i] * dt;
    }

    for (int pass = 0; pass < 5; pass++) {
        float tmp[WATER_NODES];
        memcpy(tmp, next, sizeof tmp);
        for (int i = 1; i < WATER_NODES - 1; i++) {
            float lap = next[i - 1] + next[i + 1] - next[i] * 2.0f;
            tmp[i] += lap * spread;
        }
        memcpy(next, tmp, sizeof next);
    }

    float maxAmp = 24.0f * s;
    for (int i = 0; i < WATER_NODES; i++) {
        float left = i > 0 ? next[i - 1] : next[i];
        float right = i + 1 < WATER_NODES ? next[i + 1] : next[i];
        float slope = fabsf(right - left);
        G.waterLevel[i] = clampf(next[i], -maxAmp, maxAmp);
        if (slope > 4.7f * s || fabsf(G.waterVel[i]) > 44.0f * s)
            G.waterFoam[i] = clampf(G.waterFoam[i] + (slope / fmaxf(1.0f, 18.0f * s)) * dt,
                                    0.0f, 1.35f);
        G.waterFoam[i] *= powf(0.11f, dt);
        if (G.waterFoam[i] < 0.002f) G.waterFoam[i] = 0.0f;
    }
    G.currentPhase += dt * 0.82f;
}

static void update_fish(float dt)
{
    float s = G.scale;
    float top = G.waterY + 28.0f * s;
    float bottom = G.groundY - 30.0f * s;

    for (int i = 0; i < MAX_FISH; i++) {
        Fish *f = &G.fish[i];
        if (!f->active) continue;
        f->tailPhase += dt * (6.5f + f->speed * 0.07f);
        f->wobble += dt * 1.3f;
        advance_facing(&f->facing, f->direction, 3.8f, dt);
        if (f->turnTimer > 0.0f) {
            f->turnTimer -= dt;
            if (f->turnTimer < 0.0f) f->turnTimer = 0.0f;
        }
        if (f->attackTimer > 0.0f) {
            f->attackTimer -= dt;
            if (f->attackTimer < 0.0f) f->attackTimer = 0.0f;
        }
        if (f->biteTimer > 0.0f) {
            f->biteTimer -= dt;
            if (f->biteTimer < 0.0f) f->biteTimer = 0.0f;
        }

        if (frandf() < 0.0065f || f->targetY <= 0.0f)
            f->targetY = frandr(top + 12.0f * s, bottom - 12.0f * s);
        if (frandf() < 0.0024f)
            set_fish_direction(f, -f->direction);

        float sd2;
        int si = nearest_shark(f->x, f->y, &sd2);
        float fleeR = 245.0f * s;
        if (si >= 0 && sd2 < fleeR * fleeR && sd2 > 1.0f) {
            Shark *sh = &G.sharks[si];
            float px = sh->x + sh->vx * 0.34f;
            float py = sh->y + sh->vy * 0.34f;
            float dx = f->x - px;
            float dy = f->y - py;
            float d = sqrtf(fmaxf(dx * dx + dy * dy, 1.0f));
            dx /= d;
            dy /= d;
            float threat = clampf(1.0f - d / fleeR, 0.0f, 1.0f);
            float sv = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
            if (sv > 1.0f) {
                float side = sinf(f->wobble + i * 1.7f) >= 0.0f ? 1.0f : -1.0f;
                float pxv = -sh->vy / sv;
                float pyv = sh->vx / sv;
                dx += pxv * side * (0.34f + threat * 0.48f);
                dy += pyv * side * (0.22f + threat * 0.26f);
            }
            if (f->x < 92.0f * s) dx += (92.0f * s - f->x) / (92.0f * s);
            if (f->x > G.W - 92.0f * s) dx -= (f->x - (G.W - 92.0f * s)) / (92.0f * s);
            if (f->y < top + 38.0f * s) dy += (top + 38.0f * s - f->y) / (68.0f * s);
            if (f->y > bottom - 30.0f * s) dy -= (f->y - (bottom - 30.0f * s)) / (68.0f * s);
            float ed = sqrtf(fmaxf(dx * dx + dy * dy, 1.0f));
            dx /= ed;
            dy /= ed;
            float sp = f->speed * (2.45f + threat * 3.20f + (sh->hunting ? 0.95f : 0.0f));
            set_fish_direction(f, sign_dir(dx));
            float turnMove = turn_throttle(f->turnTimer, 0.62f);
            f->x += dx * sp * turnMove * dt;
            f->y += dy * sp * 0.82f * turnMove * dt;
            f->targetX = clampf(f->x + dx * (96.0f + 130.0f * threat) * s,
                                36.0f * s, G.W - 36.0f * s);
            f->targetY = clampf(f->y + dy * (62.0f + 92.0f * threat) * s,
                                top + 10.0f * s, bottom - 10.0f * s);
            if (threat > 0.72f && f->attackTimer <= 0.0f) {
                f->attackTimer = 0.22f;
                f->attackX = sh->x;
                f->attackY = sh->y;
            }
        } else {
            float fd2;
            int fi = nearest_food(f->x, f->y, &fd2);
            float seekR = 250.0f * s;
            if (fi >= 0 && fd2 < seekR * seekR && fd2 > 1.0f) {
                Food *food = &G.food[fi];
                float d = sqrtf(fd2);
                if (d < (9.0f + f->size * 2.0f) * s) {
                    food->active = false;
                    f->attackTimer = fmaxf(f->attackTimer, 0.16f);
                    f->biteTimer = 0.48f;
                    f->attackX = food->x;
                    f->attackY = food->y;
                    f->targetX = frandr(36.0f * s, G.W - 36.0f * s);
                    f->targetY = frandr(top + 12.0f * s, bottom - 12.0f * s);
                    award_food_score(f);
                    sparkle(food->x, food->y, 0xffe0a3, 5, 32.0f * s);
                    add_bubble_at(food->x, food->y, 2.2f * s, 34.0f * s);
                } else {
                    float dx = (food->x - f->x) / d;
                    float dy = (food->y - f->y) / d;
                    if (d < 92.0f * s && f->attackTimer <= 0.0f) {
                        f->attackTimer = 0.44f;
                        f->attackX = food->x;
                        f->attackY = food->y;
                    }
                    float frenzyBoost = G.frenzyTimer > 0.0f ? 1.35f : 1.0f;
                    float burst = (f->attackTimer > 0.0f ? 3.25f : 1.7f) * frenzyBoost;
                    set_fish_direction(f, sign_dir(dx));
                    float turnMove = turn_throttle(f->turnTimer, 0.62f);
                    f->x += dx * f->speed * burst * turnMove * dt;
                    f->y += dy * f->speed * (burst * 0.78f) * turnMove * dt;
                }
            } else {
                float dx = f->targetX - f->x;
                if (fabsf(dx) < 24.0f * s || frandf() < 0.0035f)
                    f->targetX = frandr(36.0f * s, G.W - 36.0f * s);
                float vx = dx >= 0.0f ? 1.0f : -1.0f;
                set_fish_direction(f, sign_dir(dx));
                float turnMove = turn_throttle(f->turnTimer, 0.62f);
                f->x += vx * f->speed * turnMove * dt * (0.72f + 0.28f * sinf(f->wobble * 0.7f));
                f->y += (f->targetY - f->y) * clampf(dt * 1.35f, 0.0f, 1.0f);
                f->y += sinf(f->wobble) * 0.08f * s;
            }
        }

        float sepX = 0.0f, sepY = 0.0f;
        for (int j = 0; j < MAX_FISH; j++) {
            if (j == i || !G.fish[j].active) continue;
            float dx = f->x - G.fish[j].x;
            float dy = f->y - G.fish[j].y;
            float d2 = dx * dx + dy * dy;
            float r = 32.0f * s;
            if (d2 > 0.01f && d2 < r * r) {
                float inv = 1.0f / sqrtf(d2);
                float k = (1.0f - sqrtf(d2) / r);
                sepX += dx * inv * k;
                sepY += dy * inv * k;
            }
        }
        f->x += sepX * 22.0f * s * dt;
        f->y += sepY * 16.0f * s * dt;

        float margin = (22.0f + f->size * 5.0f) * s;
        if (f->x > G.W - margin) {
            f->x = G.W - margin;
            set_fish_direction(f, -1);
            f->targetX = frandr(36.0f * s, G.W * 0.62f);
            f->targetY = frandr(top, bottom);
        } else if (f->x < margin) {
            f->x = margin;
            set_fish_direction(f, 1);
            f->targetX = frandr(G.W * 0.38f, G.W - 36.0f * s);
            f->targetY = frandr(top, bottom);
        }
        f->y = clampf(f->y, top, bottom);
    }
}

static void shark_consume_fish(Shark *sh, Fish *f, float s)
{
    if (!f || !f->active) return;
    const FishPalette *pal = &FISH_PALETTES[f->palette % FISH_PALETTE_COUNT];
    sh->attackTimer = 1.18f;
    sh->chargeTimer = 0.34f;
    start_shark_bite(sh, f, s);
    /* Presentation must not consume gameplay RNG; derive pitch from the
     * victim so audio cannot change the deterministic simulation. */
    audio_play(AUDIO_SHARK_BITE, 0.76f,
               clampf(1.04f - f->size * 0.04f, 0.90f, 1.02f));
    sparkle(f->x, f->y, pal->fin, 12, 74.0f * s);
    for (int b = 0; b < 9; b++)
        add_bubble_at(f->x + frandr(-9, 9) * s, f->y + frandr(-5, 6) * s,
                      frandr(1.6f, 4.0f) * s, frandr(45.0f, 88.0f) * s);
    f->active = false;
    sh->targetFish = -1;
    sh->repathTimer = 0.08f;
    shark_penalty();
}

static void shark_begin_return_sweep(Shark *sh, float top, float bottom, float s)
{
    float off = 182.0f * s;
    int dir = sh->direction >= 0 ? -1 : 1;
    sh->sweepMode = SHARK_SWEEP_RETURN;
    sh->sweepTimer = frandr(1.60f, 2.35f);
    sh->x = dir > 0 ? -off : G.W + off;
    sh->y = frandr(top + 12.0f * s, bottom - 12.0f * s);
    sh->vx = dir * frandr(330.0f, 470.0f) * s;
    sh->vy = frandr(-22.0f, 22.0f) * s;
    sh->targetX = dir > 0 ? G.W + off : -off;
    sh->targetY = sh->y + frandr(-42.0f, 42.0f) * s;
    sh->targetFish = -1;
    sh->hunting = true;
    sh->chargeTimer = 0.62f;
    sh->attackTimer = 0.92f;
    sh->stuckTimer = 0.0f;
    sh->facing = (float)dir;
    set_shark_direction(sh, dir);
}

static void shark_begin_exit_sweep(Shark *sh, float top, float bottom, float s)
{
    int dir = fabsf(sh->vx) > 4.0f * s ? sign_dir(sh->vx) : sh->direction;
    if (dir == 0) dir = frandf() < 0.5f ? -1 : 1;
    sh->sweepMode = SHARK_SWEEP_EXIT;
    sh->sweepTimer = frandr(0.50f, 0.95f);
    sh->targetFish = -1;
    sh->hunting = false;
    sh->targetX = dir > 0 ? G.W + 190.0f * s : -190.0f * s;
    sh->targetY = frandr(top + 18.0f * s, bottom - 18.0f * s);
    sh->vx = dir * frandr(210.0f, 285.0f) * s;
    sh->vy = (sh->targetY - sh->y) * frandr(0.85f, 1.35f);
    sh->attackTimer = fmaxf(sh->attackTimer, 0.44f);
    sh->chargeTimer = fmaxf(sh->chargeTimer, 0.35f);
    sh->stuckTimer = 0.0f;
    set_shark_direction(sh, dir);
}

static void update_shark_sweep(Shark *sh, float dt, float top, float bottom, float s)
{
    float off = 182.0f * s;

    if (sh->sweepMode == SHARK_SWEEP_WAIT) {
        sh->sweepTimer -= dt;
        if (sh->sweepTimer <= 0.0f)
            shark_begin_return_sweep(sh, top, bottom, s);
        return;
    }

    sh->x += sh->vx * dt;
    sh->y += sh->vy * dt;
    sh->y += sinf(G.currentPhase * 3.1f + sh->x * 0.018f) * 0.28f * s;
    sh->targetY += sinf(G.currentPhase * 1.7f + sh->x * 0.006f) * 5.0f * s * dt;
    sh->vy += (clampf(sh->targetY, top, bottom) - sh->y) * dt * 0.72f;
    sh->y = clampf(sh->y, top, bottom);
    sh->tailPhase += dt * 10.0f;
    sh->jawPhase += dt * 7.5f;

    for (int i = 0; i < MAX_FISH; i++) {
        Fish *f = &G.fish[i];
        if (!f->active) continue;
        float dx = fabsf(f->x - sh->x);
        float dy = fabsf(f->y - sh->y);
        if (dx < 42.0f * s && dy < 24.0f * s)
            shark_consume_fish(sh, f, s);
    }

    if ((G.frameCount % 4) == 0 && sh->x > -18.0f * s && sh->x < G.W + 18.0f * s)
        disturb_water(sh->x, -7.5f * s);

    bool left = sh->x < -off;
    bool right = sh->x > G.W + off;
    if (sh->sweepMode == SHARK_SWEEP_EXIT && (left || right)) {
        sh->sweepMode = SHARK_SWEEP_WAIT;
        sh->sweepTimer = frandr(0.36f, 1.05f);
        sh->vx = 0.0f;
        sh->vy = 0.0f;
        return;
    }

    if (sh->sweepMode == SHARK_SWEEP_RETURN && (left || right)) {
        int dir = sh->vx >= 0.0f ? 1 : -1;
        float margin = 54.0f * s;
        sh->sweepMode = SHARK_SWEEP_NONE;
        sh->sweepCooldown = frandr(8.0f, 18.0f);
        sh->x = dir > 0 ? G.W - margin : margin;
        sh->y = clampf(sh->y, top, bottom);
        sh->vx = -dir * 68.0f * s;
        sh->vy *= 0.35f;
        sh->targetX = dir > 0 ? frandr(margin, G.W * 0.55f)
                              : frandr(G.W * 0.45f, G.W - margin);
        sh->targetY = frandr(top + 20.0f * s, bottom - 20.0f * s);
        sh->targetFish = -1;
        sh->repathTimer = 0.12f;
        sh->hunting = false;
        set_shark_direction(sh, -dir);
    }
}

static void update_sharks(float dt)
{
    float s = G.scale;
    float top = G.waterY + 58.0f * s;
    float bottom = G.groundY - 58.0f * s;
    float margin = 54.0f * s;

    for (int i = 0; i < MAX_SHARKS; i++) {
        Shark *sh = &G.sharks[i];
        if (!sh->active) continue;
        sh->tailPhase += dt * 5.8f;
        sh->jawPhase += dt * 4.0f;
        advance_facing(&sh->facing, sh->direction, 2.4f, dt);
        if (sh->turnTimer > 0.0f) {
            sh->turnTimer -= dt;
            if (sh->turnTimer < 0.0f) sh->turnTimer = 0.0f;
        }
        if (sh->attackTimer > 0.0f) {
            sh->attackTimer -= dt;
            if (sh->attackTimer < 0.0f) sh->attackTimer = 0.0f;
        }
        sh->repathTimer -= dt;
        if (sh->chargeTimer > 0.0f) {
            sh->chargeTimer -= dt;
            if (sh->chargeTimer < 0.0f) sh->chargeTimer = 0.0f;
        }
        if (sh->sweepCooldown > 0.0f) {
            sh->sweepCooldown -= dt;
            if (sh->sweepCooldown < 0.0f) sh->sweepCooldown = 0.0f;
        }

        if (sh->sweepMode != SHARK_SWEEP_NONE) {
            update_shark_sweep(sh, dt, top, bottom, s);
            sh->lastX = sh->x;
            sh->lastY = sh->y;
            continue;
        }

        float fd2 = 1e30f;
        bool targetValid = sh->targetFish >= 0 && sh->targetFish < MAX_FISH &&
                           G.fish[sh->targetFish].active;
        if (!targetValid || sh->repathTimer <= 0.0f) {
            sh->targetFish = shark_choose_target(sh, &fd2);
            sh->repathTimer = sh->targetFish >= 0 ? frandr(0.24f, 0.52f)
                                                  : frandr(0.75f, 1.45f);
        } else {
            Fish *f = &G.fish[sh->targetFish];
            fd2 = dist2(sh->x, sh->y, f->x, f->y);
        }

        sh->hunting = sh->targetFish >= 0;
        if (sh->sweepCooldown <= 0.0f &&
            (sh->stuckTimer > 0.34f || frandf() < (sh->hunting ? 0.0011f : 0.0034f))) {
            shark_begin_exit_sweep(sh, top, bottom, s);
            update_shark_sweep(sh, dt, top, bottom, s);
            sh->lastX = sh->x;
            sh->lastY = sh->y;
            continue;
        }

        float desiredX = 0.0f;
        float desiredY = 0.0f;
        float desiredSpeed = 44.0f * s;
        float steerRate = 2.9f;

        if (sh->hunting) {
            Fish *f = &G.fish[sh->targetFish];
            float dist = sqrtf(fmaxf(fd2, 1.0f));
            float currentSpeed = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
            float lead = clampf(dist / fmaxf(1.0f, currentSpeed + 110.0f * s), 0.10f, 0.92f);
            float preyVx = f->direction * f->speed * (0.72f + 0.28f * sinf(f->wobble));
            float preyVy = sinf(f->tailPhase * 0.55f) * 18.0f * s;
            sh->targetX = clampf(f->x + preyVx * lead, margin, G.W - margin);
            sh->targetY = clampf(f->y + preyVy * lead, top, bottom);

            desiredX = sh->targetX - sh->x;
            desiredY = sh->targetY - sh->y;
            float close = clampf(1.0f - dist / (290.0f * s), 0.0f, 1.0f);
            if (dist < 260.0f * s && sh->chargeTimer <= 0.0f)
                sh->chargeTimer = frandr(0.58f, 0.95f);
            if (dist < 190.0f * s && sh->attackTimer <= 0.0f)
                sh->attackTimer = 0.82f;
            desiredSpeed = (66.0f + close * 72.0f) * s;
            if (sh->chargeTimer > 0.0f || sh->attackTimer > 0.0f) {
                desiredSpeed *= 1.42f;
                steerRate = 4.2f;
            }
        } else {
            float d2 = dist2(sh->x, sh->y, sh->targetX, sh->targetY);
            if (d2 < (42.0f * s) * (42.0f * s) || sh->repathTimer <= 0.0f) {
                float side = sh->direction >= 0 ? 0.72f : 0.28f;
                if (frandf() < 0.38f) side = 1.0f - side;
                sh->targetX = clampf(G.W * side + frandr(-130.0f, 130.0f) * s,
                                     margin, G.W - margin);
                sh->targetY = frandr(top + 12.0f * s, bottom - 12.0f * s);
                sh->repathTimer = frandr(0.90f, 1.75f);
            }
            desiredX = sh->targetX - sh->x;
            desiredY = sh->targetY - sh->y;
            desiredSpeed = 48.0f * s;
        }

        if (sh->x < margin) desiredX += (margin - sh->x) * 3.4f;
        if (sh->x > G.W - margin) desiredX -= (sh->x - (G.W - margin)) * 3.4f;
        if (sh->y < top) desiredY += (top - sh->y) * 3.8f;
        if (sh->y > bottom) desiredY -= (sh->y - bottom) * 3.8f;

        for (int j = 0; j < MAX_SHARKS; j++) {
            if (j == i || !G.sharks[j].active) continue;
            float dx = sh->x - G.sharks[j].x;
            float dy = sh->y - G.sharks[j].y;
            float d2 = dx * dx + dy * dy;
            float r = 105.0f * s;
            if (d2 > 0.01f && d2 < r * r) {
                float d = sqrtf(d2);
                float k = (1.0f - d / r) * 82.0f * s;
                desiredX += dx / d * k;
                desiredY += dy / d * k;
            }
        }

        float dd = sqrtf(fmaxf(desiredX * desiredX + desiredY * desiredY, 1.0f));
        desiredX /= dd;
        desiredY /= dd;
        if (fabsf(desiredX) > 0.08f)
            set_shark_direction(sh, sign_dir(desiredX));
        float turnMove = turn_throttle(sh->turnTimer, 0.82f);
        float wantVx = desiredX * desiredSpeed * turnMove;
        float wantVy = desiredY * desiredSpeed * turnMove;
        float steer = clampf(dt * (steerRate + sh->stuckTimer * 5.0f), 0.0f, 1.0f);
        sh->vx += (wantVx - sh->vx) * steer;
        sh->vy += (wantVy - sh->vy) * steer;

        float maxSpeed = (sh->hunting ? 176.0f : 74.0f) * s;
        float speed = sqrtf(sh->vx * sh->vx + sh->vy * sh->vy);
        if (speed > maxSpeed) {
            sh->vx = sh->vx / speed * maxSpeed;
            sh->vy = sh->vy / speed * maxSpeed;
            speed = maxSpeed;
        }

        sh->x += sh->vx * dt;
        sh->y += sh->vy * dt;

        bool bounced = false;
        if (sh->x > G.W - margin) {
            sh->x = G.W - margin;
            sh->vx = -fabsf(sh->vx) * 0.72f;
            sh->targetX = frandr(margin, G.W * 0.55f);
            bounced = true;
        } else if (sh->x < margin) {
            sh->x = margin;
            sh->vx = fabsf(sh->vx) * 0.72f;
            sh->targetX = frandr(G.W * 0.45f, G.W - margin);
            bounced = true;
        }
        if (sh->y > bottom) {
            sh->y = bottom;
            sh->vy = -fabsf(sh->vy) * 0.68f;
            sh->targetY = frandr(top, bottom - 35.0f * s);
            bounced = true;
        } else if (sh->y < top) {
            sh->y = top;
            sh->vy = fabsf(sh->vy) * 0.68f;
            sh->targetY = frandr(top + 35.0f * s, bottom);
            bounced = true;
        }
        if (bounced) {
            sh->repathTimer = 0.05f;
            sh->stuckTimer = 0.0f;
            set_shark_direction(sh, sign_dir(sh->vx));
        }

        float moved = sqrtf(dist2(sh->x, sh->y, sh->lastX, sh->lastY));
        if (moved < 0.10f * s && speed > 22.0f * s)
            sh->stuckTimer += dt;
        else
            sh->stuckTimer = fmaxf(0.0f, sh->stuckTimer - dt * 1.8f);
        sh->lastX = sh->x;
        sh->lastY = sh->y;
        if (sh->stuckTimer > 0.42f) {
            float push = sh->x < G.W * 0.5f ? 1.0f : -1.0f;
            sh->vx = push * (92.0f + frandr(0.0f, 42.0f)) * s;
            sh->vy = frandr(-36.0f, 36.0f) * s;
            sh->targetX = clampf(sh->x + push * frandr(170.0f, 310.0f) * s,
                                 margin, G.W - margin);
            sh->targetY = frandr(top + 20.0f * s, bottom - 20.0f * s);
            sh->targetFish = -1;
            sh->repathTimer = 0.12f;
            sh->stuckTimer = 0.0f;
            set_shark_direction(sh, sign_dir(sh->vx));
        }

        if (sh->hunting && sh->targetFish >= 0 && sh->targetFish < MAX_FISH &&
            G.fish[sh->targetFish].active) {
            Fish *f = &G.fish[sh->targetFish];
            float biteD2 = dist2(sh->x, sh->y, f->x, f->y);
            if ((biteD2 < (23.0f * s) * (23.0f * s)) ||
                (sh->attackTimer > 0.0f && biteD2 < (30.0f * s) * (30.0f * s)))
                shark_consume_fish(sh, f, s);
        }

        if (sh->hunting && (G.frameCount + i * 11) % 9 == 0)
            disturb_water(sh->x, (sh->y < top + 70.0f * s ? -5.2f : -2.2f) * s);

        sh->y = clampf(sh->y, top, bottom);
    }
}

static int nearest_enemy_ship(int owner, float *outDist2)
{
    int best = -1;
    float bd = 1e30f;
    if (owner < 0 || owner >= MAX_SHIPS || !G.ships[owner].active) {
        if (outDist2) *outDist2 = bd;
        return -1;
    }
    Ship *src = &G.ships[owner];
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *ship = &G.ships[i];
        if (i == owner || !ship->active) continue;
        float d = dist2(src->x, src->y, ship->x, ship->y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (outDist2) *outDist2 = bd;
    return best;
}

static void water_splash(float x, float y, uint32_t color, int count, float power)
{
    float s = G.scale;
    disturb_water(x, power * 0.16f);
    for (int i = 0; i < count; i++) {
        float a = frandr(-PI * 0.96f, -PI * 0.04f);
        float sp = frandr(power * 0.25f, power);
        add_particle(x, y, cosf(a) * sp, sinf(a) * sp,
                     frandr(0.22f, 0.70f), frandr(1.5f, 4.8f) * s, color);
    }
    for (int b = 0; b < 4; b++)
        add_bubble_at(x + frandr(-10.0f, 10.0f) * s, y + frandr(2.0f, 12.0f) * s,
                      frandr(1.6f, 3.8f) * s, frandr(34.0f, 68.0f) * s);
}

static void damage_ship(int idx, float x, float y)
{
    if (idx < 0 || idx >= MAX_SHIPS) return;
    Ship *ship = &G.ships[idx];
    if (!ship->active) return;
    ship->health--;
    ship->cannonFlash = fmaxf(ship->cannonFlash, 0.18f);
    G.dangerPulse = 0.45f;
    water_splash(x, y, 0xfff1a8, 18, 82.0f * G.scale);
    if (ship->health <= 0) {
        float surface = sim_water_surface_y(ship->x);
        water_splash(ship->x, surface, 0xf8fafc, 32, 118.0f * G.scale);
        ship->active = false;
        ship->fishing = false;
        G.score += 75;
        G.scorePulse = 0.42f;
    }
}

static void fire_cannon(int owner, int target)
{
    if (owner < 0 || owner >= MAX_SHIPS || target < 0 || target >= MAX_SHIPS)
        return;
    Ship *a = &G.ships[owner];
    Ship *b = &G.ships[target];
    if (!a->active || !b->active) return;

    float s = G.scale;
    float startSurface = sim_water_surface_y(a->x);
    float targetSurface = sim_water_surface_y(b->x);
    float sx = a->x + a->direction * 43.0f * s;
    float sy = startSurface - 24.0f * s;
    float tx = b->x;
    float ty = targetSurface - 26.0f * s;
    float dx = tx - sx;
    if (fabsf(dx) < 34.0f * s) return;

    for (int i = 0; i < MAX_CANNONBALLS; i++) {
        Cannonball *ball = &G.cannonballs[i];
        if (ball->active) continue;
        float g = 154.0f * s;
        float travel = clampf(fabsf(dx) / (210.0f * s), 0.58f, 1.32f);
        *ball = (Cannonball){
            .active = true,
            .x = sx,
            .y = sy,
            .vx = dx / travel,
            .vy = (ty - sy - 0.5f * g * travel * travel) / travel,
            .life = 2.2f,
            .owner = owner
        };
        a->reload = frandr(1.35f, 2.35f);
        a->cannonFlash = 0.22f;
        disturb_water(a->x, 2.2f * s);
        for (int p = 0; p < 5; p++)
            add_particle(sx - a->direction * 2.0f * s, sy,
                         -a->direction * frandr(12.0f, 42.0f) * s,
                         frandr(-16.0f, 8.0f) * s,
                         frandr(0.20f, 0.45f), frandr(1.8f, 4.0f) * s, 0xb7c0c8);
        return;
    }
}

static void update_cannonballs(float dt)
{
    float s = G.scale;
    float gravity = 154.0f * s;
    for (int i = 0; i < MAX_CANNONBALLS; i++) {
        Cannonball *ball = &G.cannonballs[i];
        if (!ball->active) continue;

        ball->vy += gravity * dt;
        ball->x += ball->vx * dt;
        ball->y += ball->vy * dt;
        ball->life -= dt;

        for (int j = 0; j < MAX_SHIPS; j++) {
            if (j == ball->owner || !G.ships[j].active) continue;
            float surface = sim_water_surface_y(G.ships[j].x);
            if (fabsf(ball->x - G.ships[j].x) < 50.0f * s &&
                ball->y > surface - 92.0f * s && ball->y < surface + 12.0f * s) {
                damage_ship(j, ball->x, ball->y);
                ball->active = false;
                break;
            }
        }
        if (!ball->active) continue;

        float surface = sim_water_surface_y(ball->x);
        if (ball->life <= 0.0f || ball->x < -24.0f * s || ball->x > G.W + 24.0f * s ||
            ball->y > G.groundY || (ball->vy > 0.0f && ball->y > surface + 7.0f * s)) {
            water_splash(ball->x, fminf(ball->y, surface + 4.0f * s),
                         0xdffbff, 10, 64.0f * s);
            ball->active = false;
        }
    }
}

static void init_ship_fishing(Ship *ship)
{
    float s = G.scale;
    float surface = sim_water_surface_y(ship->x);
    ship->fishing = true;
    ship->hookX = ship->x;
    ship->hookY = surface + 32.0f * s;
    ship->hookTargetX = ship->x;
    ship->hookTargetY = clampf(G.waterY + 190.0f * s, surface + 52.0f * s,
                               G.groundY - 78.0f * s);
    ship->hookVy = 0.0f;
    ship->fishingPulse = 0.45f;
    ship->hookedFish = -1;
    ship->biteFish = -1;
    ship->biteTimer = 0.0f;
    ship->catchMeter = 0.0f;
    ship->tension = 0.0f;
    ship->reelPower = 0.0f;
}

static bool valid_fish_index(int idx)
{
    return idx >= 0 && idx < MAX_FISH && G.fish[idx].active;
}

static void reset_fishing_state(Ship *ship)
{
    ship->hookedFish = -1;
    ship->biteFish = -1;
    ship->biteTimer = 0.0f;
    ship->catchMeter = 0.0f;
    ship->tension = 0.0f;
    ship->reelPower = 0.0f;
}

static void fishing_escape(Ship *ship, int idx, bool snapped)
{
    float s = G.scale;
    if (valid_fish_index(idx)) {
        Fish *f = &G.fish[idx];
        f->targetX = clampf(f->x + frandr(-170.0f, 170.0f) * s, 36.0f * s, G.W - 36.0f * s);
        f->targetY = clampf(f->y + frandr(55.0f, 145.0f) * s,
                            G.waterY + 42.0f * s, G.groundY - 38.0f * s);
        set_fish_direction(f, sign_dir(f->targetX - f->x));
    }
    if (snapped) {
        sparkle(ship->hookX, ship->hookY, 0xe0faff, 7, 38.0f * s);
        G.dangerPulse = fmaxf(G.dangerPulse, 0.20f);
    }
    audio_play(snapped ? AUDIO_LINE_SNAP : AUDIO_FISH_ESCAPE,
               snapped ? 0.48f : 0.34f, 1.0f);
    reset_fishing_state(ship);
    ship->fishingPulse = 0.42f;
}

static void fishing_land(Ship *ship, int idx)
{
    if (!valid_fish_index(idx)) {
        reset_fishing_state(ship);
        return;
    }
    float s = G.scale;
    Fish *f = &G.fish[idx];
    const FishPalette *pal = &FISH_PALETTES[f->palette % FISH_PALETTE_COUNT];
    G.score += 85 + (int)(f->size * 22.0f) + (int)(ship->tension * 18.0f);
    G.scorePulse = 0.50f;
    G.combo++;
    if (G.combo > G.bestCombo) G.bestCombo = G.combo;
    G.comboTimer = 2.4f;
    G.frenzyMeter = clampf(G.frenzyMeter + 8.0f, 0.0f, 100.0f);
    sparkle(ship->hookX, ship->hookY, pal->body, 18, 66.0f * s);
    water_splash(ship->hookX, sim_water_surface_y(ship->hookX), 0xdffbff, 14, 62.0f * s);
    audio_play(AUDIO_CATCH_SPLASH, 0.58f,
               clampf(1.10f - f->size * 0.04f, 0.88f, 1.08f));
    f->active = false;
    reset_fishing_state(ship);
    ship->hookTargetY = sim_water_surface_y(ship->x) + 54.0f * s;
    ship->hookVy = -128.0f * s;
    ship->fishingPulse = 0.70f;
    if (game_count_fish() < 18)
        add_fish();
}

static void fishing_reel_action(Ship *ship)
{
    if (!ship || !ship->fishing) return;
    float s = G.scale;
    float surface = sim_water_surface_y(ship->x);

    if (ship->biteTimer > 0.0f && valid_fish_index(ship->biteFish)) {
        ship->hookedFish = ship->biteFish;
        ship->biteFish = -1;
        ship->biteTimer = 0.0f;
        ship->catchMeter = 0.08f;
        ship->tension = 0.42f;
        ship->reelPower = 0.34f;
        ship->fishingPulse = 0.62f;
        G.fish[ship->hookedFish].attackTimer = 0.35f;
        G.fish[ship->hookedFish].attackX = ship->hookX;
        G.fish[ship->hookedFish].attackY = ship->hookY;
        sparkle(ship->hookX, ship->hookY, 0xfff1a8, 8, 42.0f * s);
        audio_play(AUDIO_HOOK_SET, 0.42f, 1.0f);
        return;
    }

    if (valid_fish_index(ship->hookedFish)) {
        ship->reelPower = clampf(ship->reelPower + 0.34f, 0.0f, 1.0f);
        ship->hookTargetY = clampf(ship->hookTargetY - 18.0f * s,
                                   surface + 44.0f * s, G.groundY - 54.0f * s);
        ship->hookVy -= 64.0f * s;
        ship->fishingPulse = 0.36f;
        return;
    }

    ship->hookTargetY = clampf(ship->hookTargetY - 42.0f * s,
                               surface + 44.0f * s, G.groundY - 54.0f * s);
    ship->hookVy -= 90.0f * s;
    ship->fishingPulse = 0.42f;
    disturb_water(ship->hookX, -3.2f * s);
}

static void update_ship_fishing(Ship *ship, float dt)
{
    if (!ship->fishing) return;
    if (active_ship_count() != 1) {
        ship->fishing = false;
        return;
    }

    float s = G.scale;
    float surface = sim_water_surface_y(ship->x);
    if (ship->hookX <= 0.0f || ship->hookY <= 0.0f)
        init_ship_fishing(ship);

    ship->hookTargetX = clampf(ship->hookTargetX, 24.0f * s, G.W - 24.0f * s);
    ship->hookTargetY = clampf(ship->hookTargetY, surface + 46.0f * s, G.groundY - 54.0f * s);
    ship->hookX += (ship->hookTargetX - ship->hookX) * clampf(dt * 2.8f, 0.0f, 1.0f);
    ship->hookX += sinf(G.currentPhase * 5.0f + ship->wavePhase) * 0.055f * s;
    ship->hookVy += (ship->hookTargetY - ship->hookY) * 18.0f * dt;
    ship->hookVy *= powf(0.035f, dt);
    ship->hookY += ship->hookVy * dt;
    ship->hookY = clampf(ship->hookY, surface + 34.0f * s, G.groundY - 44.0f * s);
    if (ship->fishingPulse > 0.0f) {
        ship->fishingPulse -= dt;
        if (ship->fishingPulse < 0.0f) ship->fishingPulse = 0.0f;
    }
    ship->reelPower = fmaxf(0.0f, ship->reelPower - dt * 1.25f);

    if (ship->biteTimer > 0.0f) {
        ship->biteTimer -= dt;
        if (ship->biteTimer <= 0.0f) {
            fishing_escape(ship, ship->biteFish, false);
            return;
        }
    }

    if (valid_fish_index(ship->hookedFish)) {
        Fish *f = &G.fish[ship->hookedFish];
        float dx = ship->hookX - f->x;
        float dy = ship->hookY - f->y;
        float d = sqrtf(fmaxf(dx * dx + dy * dy, 1.0f));
        float nx = dx / d;
        float ny = dy / d;
        float fight = sinf(G.currentPhase * 7.2f + f->wobble * 1.7f) * 0.13f +
                      sinf(G.currentPhase * 3.1f + f->x * 0.013f) * 0.07f;
        float tensionTarget = clampf(d / (116.0f * s) + ship->reelPower * 0.56f + fight,
                                     0.0f, 1.18f);
        ship->tension += (tensionTarget - ship->tension) * clampf(dt * 4.7f, 0.0f, 1.0f);

        float pull = (0.16f + ship->reelPower * 0.82f + ship->catchMeter * 0.15f) * dt;
        f->x += dx * pull;
        f->y += dy * pull;
        f->x += -ny * sinf(G.currentPhase * 9.0f + f->wobble) * 13.0f * s * dt;
        f->y += nx * sinf(G.currentPhase * 8.1f + f->wobble) * 7.0f * s * dt;
        f->targetX = ship->hookX;
        f->targetY = ship->hookY;
        set_fish_direction(f, sign_dir(ship->hookX - f->x));

        bool sweet = ship->tension >= 0.30f && ship->tension <= 0.78f;
        if (sweet)
            ship->catchMeter += dt * (0.16f + ship->reelPower * 0.30f +
                                      clampf(1.0f - d / (118.0f * s), 0.0f, 1.0f) * 0.13f);
        else
            ship->catchMeter -= dt * (ship->tension > 0.78f ? 0.18f : 0.10f);

        if (ship->tension > 1.02f) {
            fishing_escape(ship, ship->hookedFish, true);
            return;
        }
        if (ship->tension < 0.08f && ship->catchMeter < 0.05f && ship->reelPower <= 0.02f) {
            fishing_escape(ship, ship->hookedFish, false);
            return;
        }
        ship->catchMeter = clampf(ship->catchMeter, 0.0f, 1.0f);
        if (ship->catchMeter >= 1.0f)
            fishing_land(ship, ship->hookedFish);
        return;
    } else if (ship->hookedFish >= 0) {
        fishing_escape(ship, ship->hookedFish, false);
        return;
    }

    float fd2;
    int fi = nearest_fish(ship->hookX, ship->hookY, &fd2);
    if (fi >= 0) {
        Fish *f = &G.fish[fi];
        float pullR = 122.0f * s;
        if (fd2 < pullR * pullR) {
            float d = sqrtf(fmaxf(fd2, 1.0f));
            float lure = clampf(1.0f - d / pullR, 0.0f, 1.0f);
            f->targetX = ship->hookX;
            f->targetY = ship->hookY;
            set_fish_direction(f, sign_dir(ship->hookX - f->x));
            f->x += (ship->hookX - f->x) * lure * 0.018f;
            f->y += (ship->hookY - f->y) * lure * 0.014f;
            if (d < 24.0f * s && ship->biteTimer <= 0.0f &&
                frandf() < dt * (1.45f + lure * 2.2f)) {
                ship->biteFish = fi;
                ship->biteTimer = frandr(0.72f, 1.08f);
                ship->fishingPulse = 0.60f;
                f->attackTimer = fmaxf(f->attackTimer, 0.32f);
                f->attackX = ship->hookX;
                f->attackY = ship->hookY;
                audio_play(AUDIO_HOOK_BITE, 0.34f, 1.0f);
            }
        }
    }
}

static void update_ships(float dt)
{
    float s = G.scale;
    int shipCount = active_ship_count();
    for (int i = 0; i < MAX_SHIPS; i++) {
        Ship *ship = &G.ships[i];
        if (!ship->active) continue;
        ship->bobPhase += dt * 2.1f;
        ship->wavePhase += dt * 4.6f;
        advance_facing(&ship->facing, ship->direction, 2.8f, dt);
        if (ship->reload > 0.0f) {
            ship->reload -= dt;
            if (ship->reload < 0.0f) ship->reload = 0.0f;
        }
        if (ship->cannonFlash > 0.0f) {
            ship->cannonFlash -= dt;
            if (ship->cannonFlash < 0.0f) ship->cannonFlash = 0.0f;
        }

        if (shipCount > 1) {
            if (ship->fishing)
                reset_fishing_state(ship);
            ship->fishing = false;
            float td2;
            int target = nearest_enemy_ship(i, &td2);
            if (target >= 0) {
                float dx = G.ships[target].x - ship->x;
                int face = sign_dir(dx);
                ship->direction = face;
                float dist = sqrtf(fmaxf(td2, 1.0f));
                float ideal = 235.0f * s;
                float moveDir = dist < ideal * 0.72f ? -face : face;
                float move = dist > ideal * 1.12f || dist < ideal * 0.78f ? 13.0f * s : 4.0f * s;
                ship->x += moveDir * move * dt;
                if (ship->reload <= 0.0f && dist < 650.0f * s)
                    fire_cannon(i, target);
            } else {
                ship->x += ship->direction * 13.0f * s * dt;
            }
        } else {
            float patrol = ship->fishing ? 6.0f * s : 18.0f * s;
            ship->x += ship->direction * patrol * dt;
            update_ship_fishing(ship, dt);
        }

        if (ship->x > G.W - 72.0f * s) {
            ship->x = G.W - 72.0f * s;
            ship->direction = -1;
        } else if (ship->x < 72.0f * s) {
            ship->x = 72.0f * s;
            ship->direction = 1;
        }
        ship->y = G.waterY + 45.0f * s + sinf(ship->bobPhase) * 2.0f * s;
        if ((G.frameCount + i * 19) % 12 == 0)
            disturb_water(ship->x, sinf(ship->bobPhase) * 0.85f * s);
    }
}

static void update_bubbles(float dt)
{
    float s = G.scale;
    for (int i = 0; i < MAX_BUBBLES; i++) {
        Bubble *b = &G.bubbles[i];
        if (!b->active) continue;
        b->phase += dt * 4.8f;
        b->y -= b->speed * dt;
        b->x += sinf(b->phase) * b->speed * 0.035f * dt;
        float surface = sim_water_surface_y(b->x);
        if (b->y - b->size <= surface + 1.5f * s) {
            if (frandf() < 0.34f) {
                disturb_water(b->x, -0.34f * b->size);
                add_particle(b->x, surface + 1.5f * s,
                             frandr(-8.0f, 8.0f) * s, frandr(-20.0f, -4.0f) * s,
                             frandr(0.16f, 0.38f), fmaxf(1.0f, b->size * 0.42f),
                             0xdffbff);
            }
            b->active = false;
            continue;
        }
        if (b->y + b->size < 0.0f)
            b->active = false;
    }
}

static void update_plants(float dt)
{
    for (int i = 0; i < MAX_PLANTS; i++)
        if (G.plants[i].active)
            G.plants[i].phase += dt * G.plants[i].sway;
}

static void update_particles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += 14.0f * G.scale * dt;
        p->life -= dt;
        if (p->life <= 0.0f)
            p->active = false;
    }
}

static void update_shark_bites(float dt)
{
    float s = G.scale;
    for (int i = 0; i < MAX_SHARK_BITES; i++) {
        SharkBite *b = &G.sharkBites[i];
        if (!b->active) continue;

        float age = b->maxLife - b->life;
        if (age > 0.14f && age < 0.78f && ((G.frameCount + i) % 3) == 0) {
            float t = clampf(age / b->maxLife, 0.0f, 1.0f);
            float pull = t * t * (3.0f - 2.0f * t);
            float mx = b->mouthX;
            float my = b->mouthY;
            int dir = b->direction >= 0 ? 1 : -1;
            if (b->sharkIndex >= 0 && b->sharkIndex < MAX_SHARKS && G.sharks[b->sharkIndex].active) {
                Shark *sh = &G.sharks[b->sharkIndex];
                float face = fabsf(sh->facing) > 0.05f ? sh->facing : (float)sh->direction;
                dir = face >= 0.0f ? 1 : -1;
                mx = sh->x + dir * 54.0f * s;
                my = sh->y + 3.0f * s;
                b->mouthX = mx;
                b->mouthY = my;
                b->direction = dir;
            }
            float px = b->x + (mx - dir * 9.0f * s - b->x) * pull;
            float py = b->y + (my - b->y) * pull;
            blood_spray(px, py, dir, age < 0.42f ? 6 : 3, (70.0f + 55.0f * (1.0f - t)) * s);
        }

        b->life -= dt;
        if (b->life <= 0.0f)
            b->active = false;
    }
}

static void spawn_background_bubbles(float dt)
{
    G.bubbleTimer += dt;
    float interval = 0.085f;
    while (G.bubbleTimer >= interval) {
        G.bubbleTimer -= interval;
        float s = G.scale;
        float x;
        if (frandf() < 0.55f) {
            int tries = 0;
            do {
                int pi = irandr(0, MAX_PLANTS - 1);
                if (G.plants[pi].active) {
                    x = G.plants[pi].x + frandr(-6.0f, 6.0f) * s;
                    break;
                }
                x = frandr(15.0f * s, G.W - 15.0f * s);
            } while (++tries < 4);
        } else {
            x = frandr(15.0f * s, G.W - 15.0f * s);
        }
        add_bubble_at(x, G.groundY - frandr(2.0f, 12.0f) * s,
                      frandr(1.5f, 4.2f) * s, frandr(26.0f, 64.0f) * s);
    }
}

void game_tick(void)
{
    G.frameCount++;
    if (G.feedPulse > 0.0f) {
        G.feedPulse -= TICK_DT;
        if (G.feedPulse < 0.0f) G.feedPulse = 0.0f;
    }
    if (G.scorePulse > 0.0f) {
        G.scorePulse -= TICK_DT;
        if (G.scorePulse < 0.0f) G.scorePulse = 0.0f;
    }
    if (G.dangerPulse > 0.0f) {
        G.dangerPulse -= TICK_DT;
        if (G.dangerPulse < 0.0f) G.dangerPulse = 0.0f;
    }
    if (G.paused) return;

    update_water(TICK_DT);
    if (G.comboTimer > 0.0f) {
        G.comboTimer -= TICK_DT;
        if (G.comboTimer <= 0.0f) {
            G.comboTimer = 0.0f;
            G.combo = 0;
        }
    }
    if (G.frenzyTimer > 0.0f) {
        G.frenzyTimer -= TICK_DT;
        if (G.frenzyTimer < 0.0f) G.frenzyTimer = 0.0f;
    } else if (G.frenzyMeter > 0.0f) {
        G.frenzyMeter = fmaxf(0.0f, G.frenzyMeter - 2.5f * TICK_DT);
    }

    update_food(TICK_DT);
    update_fish(TICK_DT);
    update_sharks(TICK_DT);
    update_shark_bites(TICK_DT);
    update_ships(TICK_DT);
    update_cannonballs(TICK_DT);
    update_bubbles(TICK_DT);
    update_plants(TICK_DT);
    update_particles(TICK_DT);
    spawn_background_bubbles(TICK_DT);

    if (game_count_fish() < 4 && (G.frameCount % 360) == 0)
        add_fish();
}

static Ship *single_fishing_ship(void)
{
    int idx = sole_ship_index();
    if (idx < 0) return NULL;
    return &G.ships[idx];
}

void game_handle_key(int key)
{
    switch (key) {
    case KEY_ESC:
        if (G.showHelp) G.showHelp = false;
        else G.quit = true;
        break;
    case KEY_ENTER:
    case ' ':
    {
        Ship *ship = single_fishing_ship();
        if (ship && ship->fishing) {
            fishing_reel_action(ship);
            break;
        }
        game_add_food_at(frandr(60.0f * G.scale, G.W - 60.0f * G.scale),
                         24.0f * G.scale, 10);
        break;
    }
    case KEY_MOUSE:
    {
        Ship *ship = single_fishing_ship();
        if (ship && ship->fishing) {
            float surface = sim_water_surface_y(ship->x);
            ship->hookTargetX = clampf((float)G.mouseX, 24.0f * G.scale, G.W - 24.0f * G.scale);
            ship->hookTargetY = clampf((float)G.mouseY, surface + 44.0f * G.scale,
                                       G.groundY - 54.0f * G.scale);
            fishing_reel_action(ship);
            break;
        }
        game_add_food_at((float)G.mouseX, (float)G.mouseY, 4);
        break;
    }
    case KEY_MOUSE_MOVE:
    {
        Ship *ship = single_fishing_ship();
        if (ship && ship->fishing) {
            float surface = sim_water_surface_y(ship->x);
            ship->hookTargetX = clampf((float)G.mouseX, 24.0f * G.scale, G.W - 24.0f * G.scale);
            ship->hookTargetY = clampf((float)G.mouseY, surface + 44.0f * G.scale,
                                       G.groundY - 54.0f * G.scale);
        }
        break;
    }
    case 'f': case 'F':
        add_fish();
        break;
    case 's': case 'S':
        add_shark();
        break;
    case 'b': case 'B':
        add_ship();
        break;
    case 'm': case 'M':
    {
        Ship *ship = single_fishing_ship();
        if (ship) {
            if (ship->fishing) ship->fishing = false;
            else init_ship_fishing(ship);
        }
        break;
    }
    case 'c': case 'C':
        clear_sharks();
        break;
    case 'p': case 'P':
        G.paused = !G.paused;
        break;
    case 'h': case 'H': case '?':
        G.showHelp = !G.showHelp;
        break;
    case 'i': case 'I':
        G.showInfo = !G.showInfo;
        break;
    case 'r': case 'R':
        game_reset_world();
        break;
    case 'q': case 'Q':
        G.quit = true;
        break;
    default:
        break;
    }
}
