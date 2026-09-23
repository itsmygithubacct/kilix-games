/* pong-lab: headless tuning, data generation and evaluation for kilix-pong's
 * CPU levels and neural player. Links the game's own game.o, so every label,
 * rollout and score comes from the shipped simulation.
 *
 *   pong-lab --left POLICY [--weights F.kxpol] [--beta B] [--level L]
 *            [--seed S] [--games N] [--dump F] [--max-ticks T] [--samples K]
 *
 * The left paddle is driven by POLICY (as a HUMAN controller, through the
 * same held-key path a player uses). The right paddle is the game's CPU at
 * --level easy|normal|hard|mix (mix: game k plays level k % 3). Policies:
 *
 *   intercept  meet every ball dead-centre: flawless defence, no attack
 *   reactive   a human proxy: intercept after a 12-16 tick reaction, aim
 *              error growing with speed and bounces, refined twice
 *   planner    the label source: exact lookahead over 11 strike offsets x K
 *              samples of the CPU's hidden randomness (see plan_target)
 *   neural     a policy blob via kilix-game-kit's loader and game.c's own
 *              game_policy_features()
 *   mix        DAgger: neural, overridden by the planner with probability B
 *   cpu        the game's CPU at the same level (a mirror-match reference)
 *
 * --dump writes one record per SERVE/PLAYING tick while the ball is live:
 * POLICY_FEATURES float32 features, then an int32 planner label (0 up,
 * 1 stay, 2 down). Matches are played to 11; a JSON summary goes to stdout.
 */
#include "kilix_pong.h"
#include "kilix_game_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sound_play(int id, float volume, float pitch) { (void)id; (void)volume; (void)pitch; }
void sound_set_enabled(bool enabled) { (void)enabled; }
bool term_has_key_release(void) { return true; }

enum { A_UP, A_STAY, A_DOWN };
enum { P_INTERCEPT, P_REACTIVE, P_PLANNER, P_NEURAL, P_MIX, P_CPU, P_COUNT };
static const char *policy_names[P_COUNT] = {
    "intercept", "reactive", "planner", "neural", "mix", "cpu"
};

#define FACE_X (PADDLE_INSET + PADDLE_W + BALL_RADIUS)
#define HALF (PADDLE_H * 0.5f)

static float left_center(void)
{
    return G.paddles[SIDE_LEFT].y + HALF;
}

static float fold(float y)
{
    float lo = BALL_RADIUS, span = LOGICAL_H - 2.0f * BALL_RADIUS;
    float t = fmodf(y - lo, 2.0f * span);
    if (t < 0.0f) t += 2.0f * span;
    return lo + (t > span ? 2.0f * span - t : t);
}

/* Height where the inbound ball meets the left face; false if not inbound. */
static bool intercept(float *y)
{
    const Ball *b = &G.ball;
    if (!b->active || b->vx >= 0.0f) return false;
    float t = (FACE_X - b->x) / b->vx;
    *y = fold(b->y + b->vy * (t < 0.0f ? 0.0f : t));
    return true;
}

static int steer(float target)
{
    float delta = target - left_center();
    if (fabsf(delta) < 3.0f) return A_STAY;
    return delta < 0.0f ? A_UP : A_DOWN;
}

static void press(int action)
{
    G.act_held[ACT_P1_UP] = action == A_UP;
    G.act_held[ACT_P1_DOWN] = action == A_DOWN;
    G.act_tick[ACT_P1_UP] = G.act_tick[ACT_P1_DOWN] = G.ticks;
}

/* Lab-private randomness, never the game's RNG, so a policy's own noise or
 * DAgger mixing cannot perturb the simulation it is measured on. */
static uint64_t lab_rng = 0x9E3779B97F4A7C15ULL;
static double lab_uniform(void)
{
    lab_rng ^= lab_rng << 13; lab_rng ^= lab_rng >> 7; lab_rng ^= lab_rng << 17;
    return (double)(lab_rng >> 11) * (1.0 / 9007199254740992.0);
}

/* ---------- reactive human proxy ----------
 * Reacts after 12-16 ticks (0.20-0.27 s), then aims with an error whose
 * sigma grows with ball speed and predicted wall bounces (8 + 10 per bounce
 * + 12 at the speed cap), x0.6 when the ball crosses mid-table and x0.7 at
 * the last quarter. At top speed it misses roughly 4% of flat balls and 18%
 * of one-bounce ones; slow balls are safe -- a good casual player. */
static struct { int timer, refines; float error; bool tracking; } reactive;

static int reactive_action(void)
{
    float y;
    if (!intercept(&y)) {
        reactive.tracking = false;
        reactive.timer = 12 + (int)(lab_uniform() * 5.0);
        return steer(LOGICAL_H * 0.5f);
    }
    if (!reactive.tracking) {
        if (--reactive.timer > 0) return A_STAY;
        float t = (FACE_X - G.ball.x) / G.ball.vx;
        float raw = G.ball.y + G.ball.vy * t;
        float bounces = fabsf(floorf((raw - BALL_RADIUS) / (LOGICAL_H - 2.0f * BALL_RADIUS)));
        float sigma = 8.0f + 10.0f * bounces + 12.0f * G.ball.speed / BALL_SPEED_MAX;
        double g = lab_uniform() + lab_uniform() + lab_uniform() + lab_uniform() - 2.0;
        reactive.error = (float)(g * 1.7320508) * sigma;
        reactive.refines = 2;
        reactive.tracking = true;
    }
    if (reactive.refines == 2 && G.ball.x < LOGICAL_W * 0.5f) reactive.error *= 0.6f, reactive.refines--;
    if (reactive.refines == 1 && G.ball.x < LOGICAL_W * 0.25f) reactive.error *= 0.7f, reactive.refines--;
    return steer(y + reactive.error);
}

/* ---------- planner (label source) ---------- */

static int samples = 6;
static const float offsets[] = {
    0.0f, 0.2f, -0.2f, 0.4f, -0.4f, 0.6f, -0.6f, 0.8f, -0.8f, 0.95f, -0.95f
};
#define N_OFFSETS (int)(sizeof offsets / sizeof offsets[0])
#define PLAN_EVERY 6
#define ROLLOUT_TICKS 900

/* Rolls the copied simulation forward with the left paddle steering to
 * `target` until it strikes, then recentring. +1 if the point is won, -1 if
 * lost, 0 if the CPU returns the ball. G is restored before returning. */
static int rollout(float target, uint32_t rng)
{
    GameState saved = G;
    int s0 = G.paddles[SIDE_LEFT].score, a0 = G.paddles[SIDE_RIGHT].score;
    int result = 0;
    bool struck = false;
    G.rng = rng ? rng : 1u;
    for (int i = 0; i < ROLLOUT_TICKS; i++) {
        if (G.state != GS_PLAYING && G.state != GS_SERVE) break;
        float vx = G.ball.vx;
        press(steer(struck ? LOGICAL_H * 0.5f : target));
        game_tick();
        if (G.paddles[SIDE_LEFT].score > s0) { result = 1; break; }
        if (G.paddles[SIDE_RIGHT].score > a0) { result = -1; break; }
        if (vx < 0.0f && G.ball.vx > 0.0f) struck = true;
        else if (struck && vx > 0.0f && G.ball.vx < 0.0f) break;
    }
    G = saved;
    return result;
}

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

/* Of the strike offsets, the one with the best mean outcome over `samples`
 * reseedings of the CPU's hidden randomness; ties keep the most central. The
 * reseeding is a pure function of the state, so the label is too. */
static float plan_target(float y)
{
    float best_target = clampf(y, HALF, LOGICAL_H - HALF);
    int best_value = -1000000;
    uint32_t base = mix32((uint32_t)(G.ball.x * 64.0f) ^ mix32((uint32_t)(G.ball.y * 64.0f)) ^
                          mix32((uint32_t)(G.paddles[SIDE_RIGHT].y * 64.0f) + 17u));
    for (int k = 0; k < N_OFFSETS; k++) {
        float target = clampf(y - offsets[k] * HALF, HALF, LOGICAL_H - HALF);
        int value = 0;
        for (int s = 0; s < samples; s++)
            value += rollout(target, mix32(base + (uint32_t)s * 0x9E3779B9U));
        if (value > best_value) { best_value = value; best_target = target; }
    }
    return best_target;
}

static struct { float target; long tick; } plan = { 0.0f, -1 };

static int planner_action(void)
{
    float y;
    if (!intercept(&y) || (G.state != GS_PLAYING && G.state != GS_SERVE)) {
        plan.tick = -1;
        return steer(LOGICAL_H * 0.5f);
    }
    if (plan.tick < 0 || (long)G.ticks - plan.tick >= PLAN_EVERY) {
        plan.target = plan_target(y);
        plan.tick = (long)G.ticks;
    }
    return steer(plan.target);
}

/* ---------- neural ---------- */

static kilix_policy policy;

static int neural_action(void)
{
    float features[POLICY_FEATURES], logits[POLICY_ACTIONS];
    game_policy_features(SIDE_LEFT, features);
    if (kilix_policy_forward(&policy, features, POLICY_FEATURES, logits,
                             POLICY_ACTIONS) != KILIX_POLICY_OK) {
        fprintf(stderr, "pong-lab: policy forward failed\n");
        exit(1);
    }
    return (int)kilix_policy_argmax(logits, POLICY_ACTIONS);
}

static bool load_blob(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return false; }
    static uint8_t blob[1u << 20];
    size_t size = fread(blob, 1, sizeof blob, fp);
    fclose(fp);
    kilix_policy_status status = kilix_policy_load(&policy, blob, size);
    if (status != KILIX_POLICY_OK) {
        fprintf(stderr, "%s: %s\n", path, kilix_policy_status_string(status));
        return false;
    }
    if (kilix_policy_input_count(&policy) != POLICY_FEATURES ||
        kilix_policy_output_count(&policy) != POLICY_ACTIONS) {
        fprintf(stderr, "%s: wrong shape for this game\n", path);
        return false;
    }
    return true;
}

static int parse_level(const char *text)
{
    if (!strcmp(text, "easy")) return LEVEL_EASY;
    if (!strcmp(text, "normal")) return LEVEL_NORMAL;
    if (!strcmp(text, "hard")) return LEVEL_HARD;
    if (!strcmp(text, "mix")) return -1;
    return -2;
}

int main(int argc, char **argv)
{
    const char *policy_name = "intercept", *weights = NULL, *dump = NULL, *level_name = "normal";
    double beta = 0.0;
    unsigned seed = 1;
    int games = 10;
    long max_ticks = 60L * 60 * 20;

    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) { fprintf(stderr, "missing value for %s\n", a); return 2; }
        if (!strcmp(a, "--left")) policy_name = v;
        else if (!strcmp(a, "--weights")) weights = v;
        else if (!strcmp(a, "--beta")) beta = atof(v);
        else if (!strcmp(a, "--level")) level_name = v;
        else if (!strcmp(a, "--seed")) seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--games")) games = atoi(v);
        else if (!strcmp(a, "--dump")) dump = v;
        else if (!strcmp(a, "--max-ticks")) max_ticks = atol(v);
        else if (!strcmp(a, "--samples")) samples = atoi(v);
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    int pol = 0;
    while (pol < P_COUNT && strcmp(policy_name, policy_names[pol])) pol++;
    int level = parse_level(level_name);
    if (pol == P_COUNT || level == -2 || games <= 0 || samples <= 0) {
        fprintf(stderr, "pong-lab: bad arguments (see the header comment)\n");
        return 2;
    }
    if ((pol == P_NEURAL || pol == P_MIX) && (!weights || !load_blob(weights))) return 2;
    lab_rng ^= (uint64_t)seed * 0xD1B54A32D192ED03ULL;

    FILE *out = NULL;
    if (dump && !(out = fopen(dump, "wb"))) { perror(dump); return 2; }

    long wins = 0, losses = 0, unfinished = 0, points_for = 0, points_against = 0;
    long records = 0, agree = 0, decided = 0;
    long long ticks = 0;
    for (int g = 0; g < games; g++) {
        int match_level = level >= 0 ? level : g % LEVEL_COUNT;
        G.headless = true;
        game_init(960, 540, seed + (unsigned)g);
        G.headless = true;
        game_configure(pol == P_CPU ? CTRL_CPU : CTRL_HUMAN, CTRL_CPU, match_level);
        game_start();
        plan.tick = -1;
        reactive.tracking = false;
        reactive.timer = 10;
        long t = 0;
        while (G.state != GS_GAMEOVER && t < max_ticks) {
            bool live = (G.state == GS_PLAYING || G.state == GS_SERVE) && G.ball.active;
            bool need_label = out || pol == P_PLANNER || pol == P_MIX;
            int label = need_label ? planner_action() : A_STAY;
            int action = A_STAY;
            switch (pol) {
            case P_INTERCEPT: { float y; action = steer(intercept(&y) ? y : LOGICAL_H * 0.5f); break; }
            case P_REACTIVE: action = reactive_action(); break;
            case P_PLANNER: action = label; break;
            case P_NEURAL:
            case P_MIX:
                action = neural_action();
                if (live && G.state == GS_PLAYING) { decided++; agree += action == label; }
                if (pol == P_MIX && lab_uniform() < beta) action = label;
                break;
            default: break;
            }
            if (out && live) {
                float features[POLICY_FEATURES];
                int32_t l = label;
                game_policy_features(SIDE_LEFT, features);
                fwrite(features, sizeof features, 1, out);
                fwrite(&l, sizeof l, 1, out);
                records++;
            }
            if (pol != P_CPU) press(action);
            game_tick();
            t++;
            char error[192];
            if (!game_validate(error, sizeof error)) {
                fprintf(stderr, "FAIL seed=%u tick=%ld: %s\n", seed + (unsigned)g, t, error);
                return 1;
            }
        }
        ticks += t;
        points_for += G.paddles[SIDE_LEFT].score;
        points_against += G.paddles[SIDE_RIGHT].score;
        if (G.state != GS_GAMEOVER) unfinished++;
        else if (G.winner == SIDE_LEFT) wins++;
        else losses++;
    }
    if (out) fclose(out);
    printf("{\"left\":\"%s\",\"level\":\"%s\",\"beta\":%.3f,\"seed\":%u,\"games\":%d,"
           "\"wins\":%ld,\"losses\":%ld,\"unfinished\":%ld,\"points_for\":%ld,"
           "\"points_against\":%ld,\"ticks\":%lld,\"records\":%ld",
           policy_name, level_name, beta, seed, games, wins, losses, unfinished,
           points_for, points_against, ticks, records);
    if (decided) printf(",\"planner_agreement\":%.5f", (double)agree / (double)decided);
    printf("}\n");
    kilix_policy_free(&policy);
    return 0;
}
