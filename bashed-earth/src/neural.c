/* The neural gunner: a kilix-game-kit policy compiled in from
 * src/neural_policy_blob.h (tools/neural/install-policy.py regenerates it).
 * Each turn it aims at ai_pick_target(), as the classic AIs do, and chooses a
 * weapon, a barrel angle and a power in one forward pass: no trajectory
 * search. tools/neural/bashed_earth_lab.c trains and evaluates it through
 * these same functions. */
#include "bashed_earth.h"
#include "kilix_game_policy.h"
#include "neural_policy_blob.h"

#include <math.h>
#include <stdio.h>

/* The fireable weapons, in the Tab-cycle order. */
const int NEURAL_WEAPONS[NEURAL_WEAPON_COUNT] = {
    W_NORMAL, W_MISSILE, W_BIG, W_TRIPLE, W_BOUNCY, W_ROLLER,
    W_DIGGER, W_DRILL, W_NAPALM, W_DIRT, W_MIRV, W_NUKE,
};

#define SCALE 1000.0f            /* pixels per feature unit */
#define PROFILE_SAMPLES 16       /* terrain samples between shooter and target */
#define BEYOND_SAMPLES 4         /* and past the target, 40 px apart */

static float pivot_y(const Tank *t) { return t->y - TANK_HEIGHT / 2.0f; }

/* How deep the tank's top sits under the surface, in 100 px, 0 when exposed. */
static float cover(const Tank *t)
{
    float top = t->y - TANK_HEIGHT / 2.0f;
    return clampf((top - terrain_get_height(t->x)) / 100.0f, 0.0f, 2.0f);
}

void neural_features(int shooter, int target, float *x)
{
    const Tank *me = &G.tanks[shooter], *tg = &G.tanks[target];
    float dir = tg->x >= me->x ? 1.0f : -1.0f;
    float sx = me->x, sy = pivot_y(me), dx = (tg->x - sx) * dir;
    int k = 0;
    x[k++] = dx / SCALE;
    x[k++] = (pivot_y(tg) - sy) / SCALE;
    x[k++] = G.wind * dir / 20.0f;
    x[k++] = me->maxPower / 100.0f;
    x[k++] = tg->hp / 100.0f;
    x[k++] = me->hp / 100.0f;
    x[k++] = tg->shield > 0 ? 1.0f : 0.0f;
    x[k++] = G.wallBounce ? 1.0f : 0.0f;
    x[k++] = (dir > 0 ? sx : G.W - sx) / SCALE;               /* wall behind */
    x[k++] = (dir > 0 ? G.W - tg->x : tg->x) / SCALE;         /* wall past the target */
    x[k++] = cover(tg);
    x[k++] = cover(me);
    for (int i = 0; i < PROFILE_SAMPLES; i++) {
        float px = sx + dir * dx * (float)(i + 1) / (PROFILE_SAMPLES + 1);
        x[k++] = (sy - terrain_get_height(px)) / SCALE;       /* > 0: ground above us */
    }
    for (int i = 0; i < BEYOND_SAMPLES; i++) {
        float px = clampf(tg->x + dir * 40.0f * (float)(i + 1), 0, (float)(G.W - 1));
        x[k++] = (sy - terrain_get_height(px)) / SCALE;
    }
    for (int i = 0; i < NEURAL_WEAPON_COUNT; i++)
        x[k++] = game_player_ammo(shooter, NEURAL_WEAPONS[i]) > 0 ? 1.0f : 0.0f;
}

static float squash(float v)
{
    if (!isfinite(v)) v = 0;
    return 1.0f / (1.0f + expf(-clampf(v, -30.0f, 30.0f)));
}

void neural_aim(int shooter, int target, const float *y)
{
    Tank *me = &G.tanks[shooter];
    const Tank *tg = &G.tanks[target];
    int best = 0;                                /* the Baby Missile never runs out */
    for (int i = 1; i < NEURAL_WEAPON_COUNT; i++)
        if (game_player_ammo(shooter, NEURAL_WEAPONS[i]) > 0 && isfinite(y[i]) &&
            (!isfinite(y[best]) || y[i] > y[best]))
            best = i;
    game_select_weapon(NEURAL_WEAPONS[best]);
    float a = 5.0f + 83.0f * squash(y[NEURAL_WEAPON_COUNT]);     /* 5..88 degrees */
    me->angle = tg->x >= me->x ? a : 180.0f - a;
    me->power = clampf(10.0f + 90.0f * squash(y[NEURAL_WEAPON_COUNT + 1]), 10.0f, me->maxPower);
}

/* ---------- the shipped policy ---------- */

static kilix_policy policy;
static int policy_state;                 /* 0 untried, 1 ready, -1 failed */
static const kilix_policy *candidate;    /* the lab's network, if any */

void neural_use_policy(const struct kilix_policy *p) { candidate = p; }
static char policy_status[96] = "not loaded";

static void load_once(void)
{
    if (policy_state) return;
    kilix_policy_status st = kilix_policy_load(&policy, neural_policy_blob, neural_policy_blob_size);
    if (st != KILIX_POLICY_OK) {
        snprintf(policy_status, sizeof policy_status, "policy rejected: %s",
                 kilix_policy_status_string(st));
        policy_state = -1;
    } else if (kilix_policy_input_count(&policy) != POLICY_FEATURES ||
               kilix_policy_output_count(&policy) != POLICY_OUTPUTS) {
        snprintf(policy_status, sizeof policy_status, "policy shape %zu->%zu, game needs %d->%d",
                 kilix_policy_input_count(&policy), kilix_policy_output_count(&policy),
                 POLICY_FEATURES, POLICY_OUTPUTS);
        kilix_policy_free(&policy);
        policy_state = -1;
    } else {
        snprintf(policy_status, sizeof policy_status, "ready: %zu parameters, digest %016llx",
                 policy.parameter_count, (unsigned long long)policy.digest);
        policy_state = 1;
    }
}

bool neural_ready(void)
{
    if (candidate) return true;
    load_once();
    return policy_state == 1;
}

const char *neural_status(void)
{
    load_once();
    return policy_status;
}

bool neural_do_turn(void)
{
    if (!neural_ready()) return false;
    int shooter = G.currentPlayer, target = ai_pick_target(shooter);
    if (target < 0) return false;
    float x[POLICY_FEATURES], y[POLICY_OUTPUTS];
    neural_features(shooter, target, x);
    if (kilix_policy_forward(candidate ? candidate : &policy, x, POLICY_FEATURES, y,
                             POLICY_OUTPUTS) != KILIX_POLICY_OK)
        return false;
    neural_aim(shooter, target, y);
    G.pendingAIFire = 500;               /* the same pause before firing as the classic AIs */
    return true;
}
