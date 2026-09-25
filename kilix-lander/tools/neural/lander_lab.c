/* lander-lab: headless training and evaluation for kilix-lander's neural
 * pilot. Links the game's own game.o, so every flight is the shipped
 * simulation, and runs networks through kilix-game-kit's kilix_policy_forward,
 * the same code the game uses.
 *
 * Evaluate:
 *   lander-lab --pilot autopilot|idle|neural [--weights F.raw --hidden H |
 *              --blob F.kxpol] [--seed S] [--episodes N] [--workers W]
 *              [--jsonl F] [--size WxH (instead of the size cycle)]
 *              [--max-level N (levels 1..N, default 12)]
 * Training also takes --train-max-level N (levels drawn 1..N, default 12)
 * and --select-seed S (checkpoint selection range, default 5000000); its
 * checkpoint evaluations use --max-level.
 * Record demonstrations (one process; the warm start for training):
 *   lander-lab --pilot autopilot --dump F.bin [--seed S] [--episodes N]
 *     F.bin: per flying tick, POLICY_FEATURES float32 then an int32 action
 *     (what the autopilot actually fired: main*3 + side).
 * Train (evolution strategies, antithetic, rank-shaped, Adam):
 *   lander-lab --train OUTDIR [--hidden H] [--gens G] [--pop P]
 *              [--batch E] [--sigma S] [--lr L] [--workers W] [--init F.raw]
 *
 * Episode k of a range starting at seed S: seed S+k, difficulty k % 4,
 * level 1 + (k/4) % 12, screen size k % 10 of `sizes` (training episodes
 * instead draw any landscape size from 640x400 to 3840x2160). It ends on a landing, a crash or
 * MAX_TICKS (a failure). See the research PLAN for the seed ranges.
 */
#include "terminal_lander.h"
#include "kilix_game_policy.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void sound_play(int id, float vol, float pitch) { (void)id; (void)vol; (void)pitch; }
void sound_loop(int id, bool on, float vol, float pitch) { (void)id; (void)on; (void)vol; (void)pitch; }

#define MAX_TICKS (60 * 90)
#define SELECT_SEED 5000000u
#define SELECT_EPISODES 4000
#define TRAIN_SEED 1000000u

static const int sizes[][2] = {
    { 1000, 640 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 800, 500 }, { 2560, 1440 },
    { 3840, 2160 }, { 3440, 1440 }, { 1366, 768 }, { 2000, 600 }
};
#define SIZES (int)(sizeof sizes / sizeof sizes[0])

enum { P_IDLE, P_AUTOPILOT, P_NEURAL };

/* ---------- networks ---------- */

typedef struct {
    int hidden;
    size_t count;
    float *params;
    kilix_policy policy;       /* a view over params, for kilix_policy_forward */
} Net;

static size_t param_count(int hidden)
{
    int w[4] = { POLICY_FEATURES, hidden, hidden, POLICY_ACTIONS };
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
    n->policy.widths[3] = POLICY_ACTIONS;
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

/* ---------- episodes ---------- */

typedef struct {
    int outcome;           /* 0 landed, 1 crashed, 2 timeout */
    int difficulty, ticks;
    float reward, fuel_frac;
} Episode;

static int fixed_w, fixed_h;             /* --size WxH overrides the size cycle */
static int max_level = 12;               /* --max-level: episodes cycle levels 1..N */
static int train_max_level = 12;         /* --train-max-level: training draws 1..N */
static bool random_sizes;                /* training: any terminal, from the seed */

static void start_episode(unsigned seed, int k)
{
    if (fixed_w > 0) {
        game_init(fixed_w, fixed_h, seed);
    } else if (random_sizes) {
        uint32_t h = seed * 2654435761u ^ 0x9e3779b9u;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
        int w = 640 + (int)(h % 3201u);                  /* 640..3840 */
        int hh = 400 + (int)((h >> 12) % 1761u);         /* 400..2160 */
        if (w < hh) w = hh;                              /* terminals are landscape */
        game_init(w, hh, seed);
    } else {
        game_init(sizes[k % SIZES][0], sizes[k % SIZES][1], seed);
    }
    G.difficulty = k % DIFF_COUNT;
    game_start_run();
    if (random_sizes)                                    /* training: level from the seed */
        G.level = 1 + (int)((seed * 2246822519u ^ (seed >> 13)) % (unsigned)train_max_level);
    else
        G.level = 1 + (k / DIFF_COUNT) % max_level;
    game_create_level();
    G.state = GS_PLAYING;
}

/* Landing is worth >= 1; a failure earns a shaped 0..0.8 for how close it
   came (position over the pad, then speed and tilt at the moment it ended),
   so early, all-crashing generations still have a gradient. */
static float failure_reward(bool timeout)
{
    const Lander *l = &G.lander;
    float half = G.pad.width * 0.5f + G.padGrace;
    float dx = fabsf(G.pad.x + G.pad.width * 0.5f - (l->x + l->w * 0.5f));
    float pos = 1.0f / (1.0f + fmaxf(0.0f, dx / half - 1.0f));
    if (timeout) return 0.3f * pos;
    float speed = sqrtf(l->vx * l->vx + l->vy * l->vy);
    float spd = 1.0f / (1.0f + fmaxf(0.0f, speed / G.maxSafeSpeed - 1.0f));
    float ang = 1.0f / (1.0f + fmaxf(0.0f, fabsf(l->angle) / G.maxLandingAngle - 1.0f));
    return 0.8f * (0.4f * pos + 0.3f * pos * spd + 0.3f * pos * ang);
}

static int act(const Net *net)
{
    float x[POLICY_FEATURES], y[POLICY_ACTIONS];
    game_policy_features(x);
    if (kilix_policy_forward(&net->policy, x, POLICY_FEATURES, y, POLICY_ACTIONS) != KILIX_POLICY_OK)
        return 0;
    return (int)kilix_policy_argmax(y, POLICY_ACTIONS);
}

static FILE *dump;

static Episode run_episode(int pilot, const Net *net, unsigned seed, int k)
{
    Episode e = { 2, 0, 0, 0, 0 };
    start_episode(seed, k);
    e.difficulty = G.difficulty;
    int t = 0;
    for (; t < MAX_TICKS && G.state == GS_PLAYING; t++) {
        float x[POLICY_FEATURES];
        if (dump) game_policy_features(x);
        if (pilot == P_AUTOPILOT) game_autopilot_tick();
        else if (pilot == P_NEURAL) game_apply_action(act(net));
        game_tick();
        if (dump) {
            const Lander *l = &G.lander;
            int32_t a = (l->mainThrust ? 3 : 0) + (l->leftThrust ? 1 : l->rightThrust ? 2 : 0);
            fwrite(x, sizeof x, 1, dump);
            fwrite(&a, sizeof a, 1, dump);
        }
    }
    e.ticks = t;
    if (G.state == GS_LEVEL_COMPLETE) {
        e.outcome = 0;
        e.fuel_frac = G.lander.fuel / G.lander.maxFuel;
        e.reward = 1.0f + 0.1f * e.fuel_frac;
    } else if (G.state == GS_CRASHING) {
        e.outcome = 1;
        e.reward = failure_reward(false);
    } else {
        e.reward = failure_reward(true);
    }
    game_shutdown();
    return e;
}

/* ---------- parallel map over forked workers ----------
 * job(i) for i in [0, n) runs in one of `workers` children; each result is
 * `width` floats written back through a pipe. The game keeps its state in a
 * process-wide global, so processes, not threads. */
typedef void (*JobFn)(int index, float *out, void *ctx);

static void parallel_map(int n, int width, int workers, JobFn job, void *ctx, float *results)
{
    if (workers < 1) workers = 1;
    if (workers > n) workers = n;
    if (workers == 1) {                  /* inline: keeps stdio (the dump) in this process */
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
    for (int w = 0; w < workers; w++) {
        for (int i = w; i < n; i += workers) {
            size_t bytes = sizeof(float) * (size_t)width;
            char *p = (char *)(results + (size_t)i * (size_t)width);
            while (bytes) {
                ssize_t k = read(fds[w][0], p, bytes);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) { fprintf(stderr, "worker %d died\n", w); exit(1); }
                p += k;
                bytes -= (size_t)k;
            }
        }
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

typedef struct { int pilot; const Net *net; unsigned seed; } EvalCtx;

static void eval_job(int k, float *out, void *vctx)
{
    const EvalCtx *c = vctx;
    Episode e = run_episode(c->pilot, c->net, c->seed + (unsigned)k, k);
    out[0] = (float)e.outcome;
    out[1] = (float)e.difficulty;
    out[2] = (float)e.ticks;
    out[3] = e.reward;
    out[4] = e.fuel_frac;
}

typedef struct { int n, landed, crashed, timeout; double reward; int dn[DIFF_COUNT], dl[DIFF_COUNT]; } Summary;

static Summary evaluate(int pilot, const Net *net, unsigned seed, int episodes, int workers,
                        FILE *jsonl)
{
    float *r = malloc(sizeof(float) * 5 * (size_t)episodes);
    EvalCtx c = { pilot, net, seed };
    parallel_map(episodes, 5, workers, eval_job, &c, r);
    Summary s = { 0 };
    for (int k = 0; k < episodes; k++) {
        float *e = r + 5 * k;
        int o = (int)e[0], d = (int)e[1];
        s.n++;
        s.landed += o == 0;
        s.crashed += o == 1;
        s.timeout += o == 2;
        s.reward += e[3];
        s.dn[d]++;
        s.dl[d] += o == 0;
        if (jsonl)
            fprintf(jsonl, "{\"seed\":%u,\"k\":%d,\"difficulty\":\"%s\",\"outcome\":\"%s\",\"ticks\":%d,\"fuel_left\":%.4f}\n",
                    seed + (unsigned)k, k, DIFFICULTY_NAMES[d],
                    o == 0 ? "landed" : o == 1 ? "crashed" : "timeout", (int)e[2], e[4]);
    }
    free(r);
    return s;
}

static void wilson(int k, int n, double *lo, double *hi)
{
    if (!n) { *lo = *hi = 0; return; }
    double z = 1.96, p = (double)k / n, d = 1 + z * z / n;
    double c = (p + z * z / (2 * n)) / d, h = z * sqrt(p * (1 - p) / n + z * z / (4.0 * n * n)) / d;
    *lo = c - h;
    *hi = c + h;
}

static void print_summary(FILE *f, const char *pilot, unsigned seed, const Summary *s)
{
    double lo, hi;
    wilson(s->landed, s->n, &lo, &hi);
    fprintf(f, "{\"pilot\":\"%s\",\"seed\":%u,\"episodes\":%d,\"landed\":%d,\"crashed\":%d,"
               "\"timeout\":%d,\"landing_rate\":%.4f,\"wilson95\":[%.4f,%.4f],\"by_difficulty\":{",
            pilot, seed, s->n, s->landed, s->crashed, s->timeout,
            s->n ? (double)s->landed / s->n : 0.0, lo, hi);
    for (int d = 0; d < DIFF_COUNT; d++) {
        wilson(s->dl[d], s->dn[d], &lo, &hi);
        fprintf(f, "%s\"%s\":{\"n\":%d,\"landed\":%d,\"rate\":%.4f,\"wilson95\":[%.4f,%.4f]}",
                d ? "," : "", DIFFICULTY_NAMES[d], s->dn[d], s->dl[d],
                s->dn[d] ? (double)s->dl[d] / s->dn[d] : 0.0, lo, hi);
    }
    fprintf(f, "}}\n");
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
    float sigma;
    unsigned gen;
    int batch;
    unsigned seed;
} EsCtx;

/* Member i: pair i/2, sign +/- (antithetic). Fitness = mean reward over the
   generation's shared batch of episodes (common random numbers). */
static void es_job(int member, float *out, void *vctx)
{
    const EsCtx *c = vctx;
    Net n;
    random_sizes = true;
    net_alloc(&n, c->base->hidden);
    float *eps = malloc(sizeof(float) * n.count);
    noise(eps, n.count, c->gen, member / 2);
    float sign = member % 2 ? -1.0f : 1.0f;
    for (size_t i = 0; i < n.count; i++) n.params[i] = c->base->params[i] + sign * c->sigma * eps[i];
    double total = 0;
    int landed = 0;
    for (int k = 0; k < c->batch; k++) {
        Episode e = run_episode(P_NEURAL, &n, c->seed + (unsigned)k, k);
        total += e.reward;
        landed += e.outcome == 0;
    }
    out[0] = (float)(total / c->batch);
    out[1] = (float)landed / (float)c->batch;
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
    int w[4] = { POLICY_FEATURES, n->hidden, n->hidden, POLICY_ACTIONS };
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

static unsigned select_seed = SELECT_SEED;

static int train(const char *outdir, int hidden, int gens, int pop, int batch, float sigma,
                 float lr, int workers, const char *init)
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
    snprintf(path, sizeof path, "%s/log.jsonl", outdir);
    FILE *log = fopen(path, "a");
    double best = -1;
    time_t t0 = time(NULL);
    for (int g = 1; g <= gens; g++) {
        EsCtx c = { &base, sigma, (unsigned)g, batch, TRAIN_SEED + (unsigned)g * 1000u };
        parallel_map(pop, 2, workers, es_job, &c, fit);
        double mean_fit = 0, mean_land = 0;
        for (int i = 0; i < pop; i++) {
            rank[i].v = fit[2 * i];
            rank[i].i = i;
            mean_fit += fit[2 * i];
            mean_land += fit[2 * i + 1];
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
            float gi = -grad[i] / ((float)pop * sigma) + wd * base.params[i];  /* minimise -fitness */
            m[i] = b1 * m[i] + (1 - b1) * gi;
            v[i] = b2 * v[i] + (1 - b2) * gi * gi;
            float mh = m[i] / (1 - powf(b1, (float)g)), vh = v[i] / (1 - powf(b2, (float)g));
            base.params[i] -= lr * mh / (sqrtf(vh) + 1e-8f);
        }
        fprintf(log, "{\"gen\":%d,\"mean_fitness\":%.4f,\"mean_landing\":%.4f,\"seconds\":%ld",
                g, mean_fit / pop, mean_land / pop, (long)(time(NULL) - t0));
        if (g % 10 == 0 || g == gens) {
            random_sizes = false;
            Summary s = evaluate(P_NEURAL, &base, select_seed, SELECT_EPISODES, workers, NULL);
            double rate = (double)s.landed / s.n;
            fprintf(log, ",\"select_landing\":%.4f", rate);
            for (int d = 0; d < DIFF_COUNT; d++)
                fprintf(log, ",\"select_%d\":%.4f", d, (double)s.dl[d] / s.dn[d]);
            snprintf(path, sizeof path, "%s/gen%05d.raw", outdir, g);
            net_write(&base, path);
            if (rate > best) {
                best = rate;
                snprintf(path, sizeof path, "%s/best.raw", outdir);
                net_write(&base, path);
                snprintf(path, sizeof path, "%s/best.txt", outdir);
                FILE *b = fopen(path, "w");
                if (b) { fprintf(b, "gen %d hidden %d select_landing %.4f\n", g, hidden, rate); fclose(b); }
            }
            fprintf(stderr, "gen %d fitness %.3f train-landing %.3f select %.4f (best %.4f) %lds\n",
                    g, mean_fit / pop, mean_land / pop, rate, best, (long)(time(NULL) - t0));
        }
        fprintf(log, "}\n");
        fflush(log);
    }
    fclose(log);
    return 0;
}

int main(int argc, char **argv)
{
    int pilot = P_AUTOPILOT, episodes = 400, hidden = 32, workers = 12;
    int gens = 300, pop = 96, batch = 96;
    float sigma = 0.05f, lr = 0.02f;
    unsigned seed = 1;
    const char *weights = NULL, *blob = NULL, *train_dir = NULL, *jsonl = NULL, *init = NULL;
    const char *dump_path = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "--pilot")) pilot = !strcmp(v, "idle") ? P_IDLE : !strcmp(v, "neural") ? P_NEURAL : P_AUTOPILOT;
        else if (!strcmp(a, "--seed")) seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--episodes")) episodes = atoi(v);
        else if (!strcmp(a, "--workers")) workers = atoi(v);
        else if (!strcmp(a, "--hidden")) hidden = atoi(v);
        else if (!strcmp(a, "--weights")) weights = v;
        else if (!strcmp(a, "--blob")) blob = v;
        else if (!strcmp(a, "--jsonl")) jsonl = v;
        else if (!strcmp(a, "--train")) train_dir = v;
        else if (!strcmp(a, "--init")) init = v;
        else if (!strcmp(a, "--select-seed")) select_seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--dump")) dump_path = v;
        else if (!strcmp(a, "--max-level")) max_level = atoi(v) > 0 ? atoi(v) : 12;
        else if (!strcmp(a, "--train-max-level")) train_max_level = atoi(v) > 0 ? atoi(v) : 12;
        else if (!strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &fixed_w, &fixed_h) != 2 || fixed_w < 320 || fixed_h < 200) {
                fprintf(stderr, "bad --size %s\n", v);
                return 2;
            }
        }
        else if (!strcmp(a, "--gens")) gens = atoi(v);
        else if (!strcmp(a, "--pop")) pop = atoi(v) & ~1;
        else if (!strcmp(a, "--batch")) batch = atoi(v);
        else if (!strcmp(a, "--sigma")) sigma = strtof(v, NULL);
        else if (!strcmp(a, "--lr")) lr = strtof(v, NULL);
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (train_dir) return train(train_dir, hidden, gens, pop, batch, sigma, lr, workers, init);

    Net net = { 0 };
    const char *name = pilot == P_IDLE ? "idle" : pilot == P_AUTOPILOT ? "autopilot" : "neural";
    if (pilot == P_NEURAL) {
        if (blob) {
            FILE *f = fopen(blob, "rb");
            if (!f) { perror(blob); return 1; }
            static uint8_t buf[1 << 20];
            size_t size = fread(buf, 1, sizeof buf, f);
            fclose(f);
            kilix_policy_status st = kilix_policy_load(&net.policy, buf, size);
            if (st != KILIX_POLICY_OK) { fprintf(stderr, "blob: %s\n", kilix_policy_status_string(st)); return 1; }
            if (kilix_policy_input_count(&net.policy) != POLICY_FEATURES ||
                kilix_policy_output_count(&net.policy) != POLICY_ACTIONS) {
                fprintf(stderr, "blob shape does not match the game\n");
                return 1;
            }
        } else {
            net_alloc(&net, hidden);
            if (!weights || !net_read(&net, weights)) { fprintf(stderr, "need --weights (hidden %d) or --blob\n", hidden); return 1; }
        }
    }
    if (dump_path) {
        dump = fopen(dump_path, "wb");
        if (!dump) { perror(dump_path); return 1; }
        workers = 1;                     /* the dump is written by this process */
    }
    FILE *jf = jsonl ? fopen(jsonl, "w") : NULL;
    Summary s = evaluate(pilot, &net, seed, episodes, workers, jf);
    if (jf) fclose(jf);
    if (dump) fclose(dump);
    print_summary(stdout, name, seed, &s);
    return 0;
}
