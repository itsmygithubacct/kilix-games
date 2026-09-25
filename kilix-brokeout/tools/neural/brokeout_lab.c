/* brokeout-lab: headless training and evaluation for Kilix Brokeout's neural
 * player. Links the game's own game.o, so every rally is the shipped
 * simulation, and runs networks through kilix-game-kit's kilix_policy_forward,
 * the same code the game uses.
 *
 * Evaluate:
 *   brokeout-lab --pilot autopilot|aimer|neural [--weights F.raw --hidden H |
 *                --blob F.kxpol] [--seed S] [--episodes N] [--workers W]
 *                [--jsonl F] [--seconds T] [--size WxH]
 * Train (evolution strategies, antithetic, rank-shaped, Adam):
 *   brokeout-lab --train OUTDIR [--hidden H] [--gens G] [--pop P] [--batch E]
 *                [--sigma S] [--lr L] [--workers W] [--init F.raw]
 *                [--train-seconds T] [--select-seed S]
 *
 * Episode k of a range starting at seed S: seed S+k, level 1 + (k/10) % 20,
 * terminal size k % 10 of `sizes` (training draws any landscape size and a
 * level 1..20 from the seed). It is one level played from its start and ends
 * when the level is cleared, a ball is lost, or after T seconds. A "clean
 * clear" is the first. The game runs headless, so nothing touches the real
 * high-score file.
 */
#include "kitty_brokeout.h"
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
void sound_set_enabled(bool on) { (void)on; }

#define SELECT_SEED 5000000u
#define SELECT_EPISODES 2000
#define TRAIN_SEED 1000000u
#define LEVELS 20

static const int sizes[][2] = {
    { 1000, 640 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 800, 500 }, { 2560, 1440 },
    { 3840, 2160 }, { 3440, 1440 }, { 1366, 768 }, { 2000, 600 }
};
#define SIZES (int)(sizeof sizes / sizeof sizes[0])

enum { P_AUTOPILOT, P_NEURAL, P_AIMER };
static int check_every = 25;
static bool aim_tunnel;                  /* --aim tunnel: the aimer digs the weakest column */
static bool label_aimer;                 /* --label aimer: dump the aimer's choice (DAgger) */             /* generations between selection checks */
static FILE *dump;                       /* --dump: features + action per tick */

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
    int outcome;           /* 0 cleared, 1 ball lost, 2 time up */
    int level, ticks;
    float reward, progress;
} Episode;

static int fixed_w, fixed_h;             /* --size WxH overrides the size cycle */
static bool training;                    /* training: size and level from the seed */
static float eval_seconds = 180.0f, train_seconds = 60.0f;
static float loss_penalty = 0.5f;        /* --loss-penalty: reward cost of a lost ball */

static uint32_t mix32(uint32_t h)
{
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}

static void start_episode(unsigned seed, int k)
{
    int w, h, level;
    if (training) {
        uint32_t r = mix32(seed * 2654435761u ^ 0x9e3779b9u);
        w = 640 + (int)(r % 3201u);
        h = 400 + (int)((r >> 12) % 1761u);
        if (w < h) w = h;
        level = 1 + (int)(mix32(r) % LEVELS);
    } else {
        w = sizes[k % SIZES][0];
        h = sizes[k % SIZES][1];
        level = 1 + (k / SIZES) % LEVELS;
    }
    if (fixed_w > 0) { w = fixed_w; h = fixed_h; }
    game_init(w, h, seed);
    G.headless = true;
    game_start_level(level);
}

static float hit_points(void)
{
    float total = 0.0f;
    for (int i = 0; i < G.numBricks; i++)
        if (G.bricks[i].alive && G.bricks[i].type != BRICK_METAL) total += (float)G.bricks[i].hits;
    return total;
}

/* The aimer (a lab heuristic, the warm-start teacher): strike the most urgent
   ball off-centre so the rebound heads for a goal: the remaining bricks' centre
   of mass, or with --aim tunnel the weakest non-empty column. Returns the
   action whose strike offset is nearest. */
static int aimer_action(void)
{
    float x[POLICY_FEATURES];
    game_policy_features(x);
    float hw = G.playW * 0.5f, cx0 = G.playX + hw;
    const Paddle *p = &G.paddle;
    float pc = p->x + p->w * 0.5f;
    if (x[2] < 0.5f || x[9] > 0.5f || x[8] >= 0.999f) return POLICY_ACTIONS / 2;
    float land = pc + x[7] * hw;
    float goal = cx0 + x[26] * hw;
    if (aim_tunnel) {
        int best = -1;
        for (int b = 0; b < 8; b++)
            if (x[18 + b] > 0.0f && (best < 0 || x[18 + b] < x[18 + best])) best = b;
        if (best >= 0) goal = G.playX + (best + 0.5f) / 8.0f * G.playW;
    }
    float want = clampf((goal - land) / hw * 1.4f, -NEURAL_MAX_OFFSET, NEURAL_MAX_OFFSET);
    int a = (int)lroundf((want / NEURAL_MAX_OFFSET + 1.0f) * 0.5f * (POLICY_ACTIONS - 1));
    return a < 0 ? 0 : a >= POLICY_ACTIONS ? POLICY_ACTIONS - 1 : a;
}

static int act(const Net *net)
{
    float x[POLICY_FEATURES], y[POLICY_ACTIONS];
    game_policy_features(x);
    if (kilix_policy_forward(&net->policy, x, POLICY_FEATURES, y, POLICY_ACTIONS) != KILIX_POLICY_OK)
        return POLICY_ACTIONS / 2;
    return (int)kilix_policy_argmax(y, POLICY_ACTIONS);
}

/* Reward: the share of the level's brick hit points removed, plus 1 and up
   to 0.5 more for clearing it sooner, minus loss_penalty for losing a ball. */
static Episode run_episode(int pilot, const Net *net, unsigned seed, int k, float seconds)
{
    Episode e = { 2, 0, 0, 0, 0 };
    start_episode(seed, k);
    e.level = G.level;
    float start = fmaxf(hit_points(), 1.0f);
    int lives = G.lives, limit = (int)(seconds * 60.0f), t = 0;
    for (; t < limit; t++) {
        if (G.state == GS_PLAYING || G.state == GS_BALL_LOST) {
            float x[POLICY_FEATURES];
            if (dump) game_policy_features(x);
            int teacher = dump && label_aimer ? aimer_action() : -1;   /* before acting */
            int a = POLICY_ACTIONS / 2;
            if (pilot == P_AUTOPILOT) {
                game_set_held_controls(false, false, false);
                game_autopilot_tick();
            } else {
                a = pilot == P_AIMER ? aimer_action() : act(net);
                game_apply_action(a);
            }
            if (dump) {
                int32_t label = teacher >= 0 ? teacher : a;
                fwrite(x, sizeof x, 1, dump);
                fwrite(&label, sizeof label, 1, dump);
            }
        }
        game_tick();
        if (G.state == GS_LEVEL_CLEAR) { e.outcome = 0; t++; break; }
        if (G.lives < lives || G.state == GS_GAMEOVER) { e.outcome = 1; t++; break; }
    }
    e.ticks = t;
    e.progress = 1.0f - hit_points() / start;
    if (e.outcome == 0) e.progress = 1.0f;
    e.reward = e.progress + (e.outcome == 0 ? 1.0f + 0.5f * (1.0f - (float)t / (float)limit) : 0.0f)
             - (e.outcome == 1 ? loss_penalty : 0.0f);
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
    Episode e = run_episode(c->pilot, c->net, c->seed + (unsigned)k, k, eval_seconds);
    out[0] = (float)e.outcome;
    out[1] = (float)e.level;
    out[2] = (float)e.ticks;
    out[3] = e.reward;
    out[4] = e.progress;
}

typedef struct {
    int n, cleared, lost, timeout;
    double progress, clear_seconds;
    int band_n[4], band_clear[4];          /* levels 1-5, 6-10, 11-15, 16-20 */
} Summary;

static Summary evaluate(int pilot, const Net *net, unsigned seed, int episodes, int workers,
                        FILE *jsonl)
{
    float *r = malloc(sizeof(float) * 5 * (size_t)episodes);
    EvalCtx c = { pilot, net, seed };
    training = false;
    parallel_map(episodes, 5, workers, eval_job, &c, r);
    Summary s = { 0 };
    for (int k = 0; k < episodes; k++) {
        float *e = r + 5 * k;
        int o = (int)e[0], level = (int)e[1], band = (level - 1) / 5;
        if (band > 3) band = 3;
        s.n++;
        s.cleared += o == 0;
        s.lost += o == 1;
        s.timeout += o == 2;
        s.progress += e[4];
        if (o == 0) s.clear_seconds += e[2] / 60.0;
        s.band_n[band]++;
        s.band_clear[band] += o == 0;
        if (jsonl)
            fprintf(jsonl, "{\"seed\":%u,\"k\":%d,\"level\":%d,\"outcome\":\"%s\",\"seconds\":%.2f,\"progress\":%.4f}\n",
                    seed + (unsigned)k, k, level, o == 0 ? "cleared" : o == 1 ? "ball_lost" : "time_up",
                    e[2] / 60.0, e[4]);
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
    double lo, hi, llo, lhi;
    wilson(s->cleared, s->n, &lo, &hi);
    wilson(s->lost, s->n, &llo, &lhi);
    fprintf(f, "{\"pilot\":\"%s\",\"seed\":%u,\"episodes\":%d,\"seconds\":%.0f,\"cleared\":%d,"
               "\"clear_rate\":%.4f,\"wilson95\":[%.4f,%.4f],\"ball_lost\":%d,\"loss_rate\":%.4f,"
               "\"loss_wilson95\":[%.4f,%.4f],\"time_up\":%d,\"mean_progress\":%.4f,"
               "\"mean_clear_seconds\":%.2f,\"by_levels\":{",
            pilot, seed, s->n, eval_seconds, s->cleared, s->n ? (double)s->cleared / s->n : 0.0,
            lo, hi, s->lost, s->n ? (double)s->lost / s->n : 0.0, llo, lhi, s->timeout,
            s->n ? s->progress / s->n : 0.0, s->cleared ? s->clear_seconds / s->cleared : 0.0);
    static const char *bands[4] = { "1-5", "6-10", "11-15", "16-20" };
    for (int b = 0; b < 4; b++) {
        wilson(s->band_clear[b], s->band_n[b], &lo, &hi);
        fprintf(f, "%s\"%s\":{\"n\":%d,\"cleared\":%d,\"rate\":%.4f,\"wilson95\":[%.4f,%.4f]}",
                b ? "," : "", bands[b], s->band_n[b], s->band_clear[b],
                s->band_n[b] ? (double)s->band_clear[b] / s->band_n[b] : 0.0, lo, hi);
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

static void es_job(int member, float *out, void *vctx)
{
    const EsCtx *c = vctx;
    Net n;
    training = true;
    net_alloc(&n, c->base->hidden);
    float *eps = malloc(sizeof(float) * n.count);
    noise(eps, n.count, c->gen, member / 2);
    float sign = member % 2 ? -1.0f : 1.0f;
    for (size_t i = 0; i < n.count; i++) n.params[i] = c->base->params[i] + sign * c->sigma * eps[i];
    double total = 0;
    int cleared = 0;
    for (int k = 0; k < c->batch; k++) {
        Episode e = run_episode(P_NEURAL, &n, c->seed + (unsigned)k, k, train_seconds);
        total += e.reward;
        cleared += e.outcome == 0;
    }
    out[0] = (float)(total / c->batch);
    out[1] = (float)cleared / (float)c->batch;
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
        double mean_fit = 0, mean_clear = 0;
        for (int i = 0; i < pop; i++) {
            rank[i].v = fit[2 * i];
            rank[i].i = i;
            mean_fit += fit[2 * i];
            mean_clear += fit[2 * i + 1];
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
        fprintf(log, "{\"gen\":%d,\"mean_fitness\":%.4f,\"mean_clear\":%.4f,\"seconds\":%ld",
                g, mean_fit / pop, mean_clear / pop, (long)(time(NULL) - t0));
        if (g % check_every == 0 || g == gens) {
            Summary s = evaluate(P_NEURAL, &base, select_seed, SELECT_EPISODES, workers, NULL);
            double rate = (double)s.cleared / s.n, loss = (double)s.lost / s.n;
            /* the selection score: clean clears first, mean progress breaks ties */
            double score = rate + 1e-3 * s.progress / s.n;
            fprintf(log, ",\"select_clear\":%.4f,\"select_loss\":%.4f,\"select_progress\":%.4f",
                    rate, loss, s.progress / s.n);
            snprintf(path, sizeof path, "%s/gen%05d.raw", outdir, g);
            net_write(&base, path);
            if (score > best) {
                best = score;
                snprintf(path, sizeof path, "%s/best.raw", outdir);
                net_write(&base, path);
                snprintf(path, sizeof path, "%s/best.txt", outdir);
                FILE *b = fopen(path, "w");
                if (b) {
                    fprintf(b, "gen %d hidden %d select_clear %.4f select_loss %.4f select_progress %.4f\n",
                            g, hidden, rate, loss, s.progress / s.n);
                    fclose(b);
                }
            }
            fprintf(stderr, "gen %d fitness %.3f train-clear %.3f select clear %.4f loss %.4f progress %.3f %lds\n",
                    g, mean_fit / pop, mean_clear / pop, rate, loss, s.progress / s.n,
                    (long)(time(NULL) - t0));
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
    int gens = 300, pop = 64, batch = 48;
    float sigma = 0.05f, lr = 0.02f;
    unsigned seed = 1;
    const char *weights = NULL, *blob = NULL, *train_dir = NULL, *jsonl = NULL, *init = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "--pilot")) pilot = !strcmp(v, "neural") ? P_NEURAL : !strcmp(v, "aimer") ? P_AIMER : P_AUTOPILOT;
        else if (!strcmp(a, "--label")) label_aimer = !strcmp(v, "aimer");
        else if (!strcmp(a, "--loss-penalty")) loss_penalty = strtof(v, NULL);
        else if (!strcmp(a, "--aim")) aim_tunnel = !strcmp(v, "tunnel");
        else if (!strcmp(a, "--check-every")) check_every = atoi(v) > 0 ? atoi(v) : 25;
        else if (!strcmp(a, "--dump")) { dump = fopen(v, "wb"); if (!dump) { perror(v); return 1; } workers = 1; }
        else if (!strcmp(a, "--seed")) seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--episodes")) episodes = atoi(v);
        else if (!strcmp(a, "--workers")) workers = atoi(v);
        else if (!strcmp(a, "--hidden")) hidden = atoi(v);
        else if (!strcmp(a, "--weights")) weights = v;
        else if (!strcmp(a, "--blob")) blob = v;
        else if (!strcmp(a, "--jsonl")) jsonl = v;
        else if (!strcmp(a, "--train")) train_dir = v;
        else if (!strcmp(a, "--init")) init = v;
        else if (!strcmp(a, "--gens")) gens = atoi(v);
        else if (!strcmp(a, "--pop")) pop = atoi(v) & ~1;
        else if (!strcmp(a, "--batch")) batch = atoi(v);
        else if (!strcmp(a, "--sigma")) sigma = strtof(v, NULL);
        else if (!strcmp(a, "--lr")) lr = strtof(v, NULL);
        else if (!strcmp(a, "--seconds")) eval_seconds = strtof(v, NULL);
        else if (!strcmp(a, "--train-seconds")) train_seconds = strtof(v, NULL);
        else if (!strcmp(a, "--select-seed")) select_seed = (unsigned)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--size")) {
            if (sscanf(v, "%dx%d", &fixed_w, &fixed_h) != 2 || fixed_w < 320 || fixed_h < 200) {
                fprintf(stderr, "bad --size %s\n", v);
                return 2;
            }
        }
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    /* The game reads its high-score store at init: keep the lab away from the
       player's real one. */
    char scratch[] = "/tmp/brokeout-lab-XXXXXX";
    if (!mkdtemp(scratch)) { perror("mkdtemp"); return 1; }
    setenv("XDG_DATA_HOME", scratch, 1);
    setenv("XDG_STATE_HOME", scratch, 1);
    if (train_dir) return train(train_dir, hidden, gens, pop, batch, sigma, lr, workers, init);

    Net net = { 0 };
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
    FILE *jf = jsonl ? fopen(jsonl, "w") : NULL;
    Summary s = evaluate(pilot, &net, seed, episodes, workers, jf);
    if (jf) fclose(jf);
    if (dump) fclose(dump);
    print_summary(stdout, pilot == P_NEURAL ? "neural" : pilot == P_AIMER ? "aimer" : "autopilot", seed, &s);
    return 0;
}
