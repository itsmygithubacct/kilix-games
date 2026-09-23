/* Simulation, rules, physics, and AI for kilix-pong.
 *
 * Everything here runs at a fixed 60Hz tick and is a pure function of (seed,
 * tick count, injected input) -- no wall-clock reads -- so a seed replays
 * identically. That is what game_state_digest and `make test`'s repeated-seed
 * comparison rely on.
 *
 * The math is restricted to operations IEEE-754 pins down exactly: +,-,*,/ and
 * sqrt are correctly rounded, and fmod is exact. Deflection therefore comes
 * from a normalized offset plus sqrtf rather than sinf/cosf, which are libm
 * territory and vary in the last place between implementations. Same-binary
 * replay is guaranteed; the stricter cross-toolchain claim additionally
 * assumes the build does not contract a*b+c into fma (-ffp-contract=off).
 */
#include "kilix_pong.h"
#include "kilix_game_policy.h"
#include "neural_policy_blob.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

GameState G;

/* ---------- CPU skill levels ----------
 *
 * The old tracker re-predicted the ball perfectly every tick, so with any
 * speed handicap it still could not be beaten: at the 420 cap the steepest
 * return leaves it >= 1.04 s (137 units of travel) for at most ~71 units of
 * need. These levels make it beatable the way people are: it reacts late,
 * commits to a prediction whose error grows with ball speed and with the
 * number of wall bounces it has to fold, and only sharpens that guess a few
 * times as the ball approaches. NORMAL and HARD also play english, aiming
 * the return away from the opponent -- which costs them some misses. Every
 * draw comes from the seeded RNG, so replays stay exact.
 *
 * Values were tuned headlessly against scripted players; the numbers and
 * the tournament are in tools/neural/README.md.
 */
typedef struct {
    float speed_scale;     /* x PADDLE_SPEED */
    int   react_ticks;     /* ticks before it starts tracking an inbound ball */
    int   react_jitter;    /* + uniform 0..jitter ticks */
    float err_base;        /* aim-error sigma terms, logical units: */
    float err_bounce;      /*   per predicted wall bounce */
    float err_speed;       /*   at the 420 speed cap, scaled linearly */
    float settle;          /* error multiplier at each refinement */
    int   refines;         /* how many times it sharpens its guess per shot */
    int   replan_ticks;
    float english;         /* strike offset it plays for; 0 = dead centre */
    float idle_speed;      /* fraction of speed used to recentre */
} CpuLevel;

static const CpuLevel cpu_levels[LEVEL_COUNT] = {
    /*            speed react jit  base bnc  spd  settle ref replan eng  idle */
    /* EASY   */ { 0.66f, 14, 8, 6.0f, 7.0f, 11.0f, 0.75f, 1, 12, 0.00f, 0.50f },
    /* NORMAL */ { 0.80f, 11, 6, 6.0f, 12.0f, 19.0f, 0.60f, 2, 10, 0.30f, 0.70f },
    /* HARD   */ { 0.92f, 8, 4, 4.0f, 9.0f, 15.0f, 0.60f, 2, 8, 0.55f, 0.90f },
};

#define CPU_DEADZONE 3.0f

/* The neural player: a kilix-game-kit policy compiled in from
   src/neural_policy_blob.h (tools/neural regenerates it). Read-only after
   the first game_init, so it is not simulation state. */
static kilix_policy neural_policy;
static int  neural_loaded;          /* 0 untried, 1 ready, -1 failed */
static char neural_status[96] = "not loaded";

static void neural_load_once(void)
{
    if (neural_loaded) return;
    kilix_policy_status status =
        kilix_policy_load(&neural_policy, neural_policy_blob, neural_policy_blob_size);
    if (status != KILIX_POLICY_OK) {
        (void)snprintf(neural_status, sizeof neural_status, "policy rejected: %s",
                       kilix_policy_status_string(status));
        neural_loaded = -1;
    } else if (kilix_policy_input_count(&neural_policy) != POLICY_FEATURES ||
               kilix_policy_output_count(&neural_policy) != POLICY_ACTIONS) {
        (void)snprintf(neural_status, sizeof neural_status,
                       "policy shape %zu->%zu, game needs %d->%d",
                       kilix_policy_input_count(&neural_policy),
                       kilix_policy_output_count(&neural_policy),
                       POLICY_FEATURES, POLICY_ACTIONS);
        kilix_policy_free(&neural_policy);
        neural_loaded = -1;
    } else {
        (void)snprintf(neural_status, sizeof neural_status,
                       "ready: %zu parameters, digest %016llx",
                       neural_policy.parameter_count,
                       (unsigned long long)neural_policy.digest);
        neural_loaded = 1;
    }
}

bool game_neural_ready(void)
{
    neural_load_once();
    return neural_loaded == 1;
}

const char *game_neural_status(void)
{
    neural_load_once();
    return neural_status;
}

const char *game_controller_name(int controller)
{
    static const char *names[CTRL_COUNT] = { "HUMAN", "CPU", "NEURAL" };
    return (controller >= 0 && controller < CTRL_COUNT) ? names[controller] : "?";
}

const char *game_level_name(int level)
{
    static const char *names[LEVEL_COUNT] = { "EASY", "NORMAL", "HARD" };
    return (level >= 0 && level < LEVEL_COUNT) ? names[level] : "?";
}

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* xorshift32; the only randomness in the sim, and it is seeded. */
static uint32_t rand_u32(void)
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return G.rng;
}

float game_randf(void)
{
    return (float)(rand_u32() >> 8) * (1.0f / 16777216.0f);
}

static float rand_range(float lo, float hi)
{
    return lo + (hi - lo) * game_randf();
}

/* Approximately standard normal from four uniforms (Irwin-Hall), so it
   uses only exactly-rounded arithmetic. */
static float rand_gauss(void)
{
    float sum = game_randf() + game_randf() + game_randf() + game_randf();
    return (sum - 2.0f) * 1.7320508f;
}

static void play(int id, float volume, float pitch)
{
    if (!G.headless && G.sound_on) sound_play(id, volume, pitch);
}

/* ---------- particles ---------- */

static void spawn_particles(float x, float y, int count, uint32_t color,
                            float speed_lo, float speed_hi)
{
    for (int i = 0; i < count; i++) {
        Particle *p = NULL;
        for (int j = 0; j < MAX_PARTICLES; j++) {
            if (!G.particles[j].active) { p = &G.particles[j]; break; }
        }
        if (!p) return;   /* budget spent this frame; drop the rest */

        /* Direction without trig: sample a vector in the unit square and
           normalize, rejecting the degenerate center. */
        float dx = rand_range(-1.0f, 1.0f);
        float dy = rand_range(-1.0f, 1.0f);
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.0001f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
        float speed = rand_range(speed_lo, speed_hi);

        p->x = x;
        p->y = y;
        p->vx = dx / len * speed;
        p->vy = dy / len * speed;
        p->max_life = rand_range(0.18f, 0.42f);
        p->life = p->max_life;
        p->color = color;
        p->active = true;
    }
}

static void update_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->x += p->vx * TICK_DT;
        p->y += p->vy * TICK_DT;
        p->vx *= 0.94f;
        p->vy *= 0.94f;
        p->life -= TICK_DT;
        if (p->life <= 0.0f) p->active = false;
    }
}

/* ---------- setup ---------- */

static void reset_paddles(void)
{
    Paddle *l = &G.paddles[SIDE_LEFT];
    Paddle *r = &G.paddles[SIDE_RIGHT];

    l->x = PADDLE_INSET;
    r->x = LOGICAL_W - PADDLE_INSET - PADDLE_W;
    for (int s = 0; s < SIDE_COUNT; s++) {
        G.paddles[s].y = (LOGICAL_H - PADDLE_H) * 0.5f;
        G.paddles[s].w = PADDLE_W;
        G.paddles[s].h = PADDLE_H;
        G.paddles[s].vy = 0.0f;
        G.paddles[s].input = 0.0f;
        G.paddles[s].controller = G.setup[s];
        memset(&G.paddles[s].cpu, 0, sizeof G.paddles[s].cpu);
        G.paddles[s].cpu.target = LOGICAL_H * 0.5f;
    }
}

/* Places the ball at center and aims it toward serve_to -- the receiver, per
 * the header contract, i.e. the side that just conceded (or a random side on
 * the opening serve). */
static void serve_ball(void)
{
    Ball *b = &G.ball;
    b->x = LOGICAL_W * 0.5f;
    b->y = LOGICAL_H * 0.5f;
    b->speed = BALL_SPEED_MIN;
    b->active = true;
    b->trail_head = 0;
    for (int i = 0; i < BALL_TRAIL_LEN; i++) {
        b->trail_x[i] = b->x;
        b->trail_y[i] = b->y;
    }

    /* Aim toward the receiver with a shallow random vertical component; the
       BALL_MIN_VX floor keeps a serve from crawling down the screen. */
    float vy = rand_range(-0.45f, 0.45f);
    float vx_mag = sqrtf(1.0f - vy * vy);
    float dir = (G.serve_to == SIDE_LEFT) ? -1.0f : 1.0f;
    b->vx = dir * vx_mag * b->speed;
    b->vy = vy * b->speed;
    G.rally = 0;
}

void game_init(int w, int h, uint32_t seed)
{
    bool headless = G.headless;
    memset(&G, 0, sizeof G);
    G.headless = headless;
    /* Audible by default: main.c mirrors this into sound_set_enabled right
       after sound_init, so leaving it zeroed mutes the whole bank even when
       the sink opened fine. Headless play() is gated separately. */
    G.sound_on = true;
    G.W = w;
    G.H = h;
    G.rng = seed ? seed : 0x1234567u;
    G.state = GS_TITLE;
    G.setup[SIDE_LEFT] = CTRL_HUMAN;
    G.setup[SIDE_RIGHT] = CTRL_CPU;
    G.level = LEVEL_NORMAL;
    G.menu_row = MENU_LEFT;
    G.winner = -1;
    G.serve_to = SIDE_RIGHT;
    G.paused_from = GS_PLAYING;
    neural_load_once();
    reset_paddles();
    serve_ball();
    G.ball.active = false;
}

void game_configure(int left_controller, int right_controller, int level)
{
    G.setup[SIDE_LEFT] = (left_controller >= 0 && left_controller < CTRL_COUNT)
                         ? left_controller : CTRL_HUMAN;
    G.setup[SIDE_RIGHT] = (right_controller >= 0 && right_controller < CTRL_COUNT)
                          ? right_controller : CTRL_CPU;
    G.level = (level >= 0 && level < LEVEL_COUNT) ? level : LEVEL_NORMAL;
}

void game_start(void)
{
    G.paddles[SIDE_LEFT].score = 0;
    G.paddles[SIDE_RIGHT].score = 0;
    G.winner = -1;
    G.rally = 0;
    G.shake = 0.0f;
    G.flash = 0.0f;
    G.serve_to = (rand_u32() & 1u) ? SIDE_LEFT : SIDE_RIGHT;
    memset(G.particles, 0, sizeof G.particles);
    memset(G.act_held, 0, sizeof G.act_held);
    memset(G.act_tick, 0, sizeof G.act_tick);
    reset_paddles();
    serve_ball();
    G.state = GS_SERVE;
    G.paused_from = GS_PLAYING;
    G.state_timer = 0.7f;
    play(SFX_SERVE, 0.8f, 1.0f);
}

/* ---------- input ---------- */

static void set_action(int act, bool down)
{
    if (act < 0 || act >= ACT_COUNT) return;
    if (down) {
        G.act_held[act] = true;
        G.act_tick[act] = G.ticks;
    } else {
        G.act_held[act] = false;
    }
}

static int action_for_key(int key)
{
    switch (key) {
    case 'w': case 'W': return ACT_P1_UP;
    case 's': case 'S': return ACT_P1_DOWN;
    case KEY_UP:        return ACT_P2_UP;
    case KEY_DOWN:      return ACT_P2_DOWN;
    default:            return -1;
    }
}

static void toggle_pause(void)
{
    if (G.state == GS_PLAYING || G.state == GS_SERVE || G.state == GS_POINT) {
        G.paused_from = G.state;
        G.state = GS_PAUSED;
        play(SFX_MENU, 0.7f, 1.0f);
    } else if (G.state == GS_PAUSED) {
        G.state = G.paused_from;
        play(SFX_MENU, 0.7f, 1.1f);
    }
}

/* Title menu: Up/Down (or W/S) pick a row, Left/Right change it. */
static bool title_menu_event(const KeyEvent *ev)
{
    if (G.state != GS_TITLE) return false;
    int step = 0;
    switch (ev->key) {
    case KEY_UP: case 'w': case 'W':
        if (ev->action == KEY_ACTION_PRESS)
            G.menu_row = (G.menu_row + MENU_ROWS - 1) % MENU_ROWS;
        break;
    case KEY_DOWN: case 's': case 'S':
        if (ev->action == KEY_ACTION_PRESS)
            G.menu_row = (G.menu_row + 1) % MENU_ROWS;
        break;
    case KEY_LEFT:  step = -1; break;
    case KEY_RIGHT: step = 1;  break;
    default:
        return false;
    }
    if (step && ev->action == KEY_ACTION_PRESS) {
        if (G.menu_row == MENU_LEVEL) {
            G.level = (G.level + LEVEL_COUNT + step) % LEVEL_COUNT;
        } else {
            int side = G.menu_row == MENU_LEFT ? SIDE_LEFT : SIDE_RIGHT;
            G.setup[side] = (G.setup[side] + CTRL_COUNT + step) % CTRL_COUNT;
        }
    }
    if (ev->action == KEY_ACTION_PRESS) play(SFX_MENU, 0.7f, step ? 1.15f : 1.0f);
    return true;
}

void game_handle_event(const KeyEvent *ev)
{
    if (!ev) return;
    if (title_menu_event(ev)) return;

    int act = action_for_key(ev->key);
    if (act >= 0) {
        if (ev->action == KEY_ACTION_RELEASE) set_action(act, false);
        else set_action(act, true);   /* press or repeat both refresh the hold */
        return;
    }

    /* Everything below is edge-triggered: repeats must not re-fire menus. */
    if (ev->action != KEY_ACTION_PRESS) return;

    switch (ev->key) {
    case 'q': case 'Q':          /* Q always quits; Esc never does mid-match */
        G.quit = true;
        break;

    case 'p': case 'P':          /* pause only; never a way out of the game */
        toggle_pause();
        break;

    case KEY_ESC:                /* pause in play, back out of menus */
        if (G.state == GS_TITLE) G.quit = true;
        else if (G.state == GS_GAMEOVER) {
            G.state = GS_TITLE;
            play(SFX_MENU, 0.7f, 1.0f);
        } else toggle_pause();
        break;

    case 'm': case 'M':          /* sound, per the house control scheme */
        G.sound_on = !G.sound_on;
        if (!G.headless) sound_set_enabled(G.sound_on);
        play(SFX_MENU, 0.7f, 1.2f);   /* only audible when switching on */
        break;

    case ' ': case KEY_ENTER:
        if (G.state == GS_TITLE) {
            game_start();
        } else if (G.state == GS_GAMEOVER) {
            game_start();             /* direct rematch; Esc goes to title */
        } else if (G.state == GS_PAUSED) {
            toggle_pause();
        }
        break;

    default:
        break;
    }
}

/* Reduces held state to a -1..1 axis per paddle.
 *
 * When the terminal cannot report releases, a hold is only believed for
 * LEGACY_HOLD_TICKS after its last press/repeat -- key autorepeat refreshes it.
 * The expiry is counted in ticks, never wall-clock, so headless replays stay
 * bit-identical. term_has_key_release() is sticky-true (codex_pong's design):
 * it flips once a real enhanced event is seen, and from then on releases are
 * authoritative and nothing expires.
 */
static float axis_from(int up_act, int down_act, bool trust_release)
{
    bool up = G.act_held[up_act];
    bool down = G.act_held[down_act];

    if (!trust_release) {
        if (up && G.ticks - G.act_tick[up_act] > LEGACY_HOLD_TICKS) up = false;
        if (down && G.ticks - G.act_tick[down_act] > LEGACY_HOLD_TICKS) down = false;
    }
    return (down ? 1.0f : 0.0f) - (up ? 1.0f : 0.0f);
}

/* Human paddles read their keys: W/S for the left, Up/Down for the right.
 * With exactly one human, either key pair drives that paddle, so a lone
 * player on the right side is not stuck with the arrow keys. */
static void apply_input(void)
{
    bool trust = G.headless ? true : term_has_key_release();
    bool left_human = G.paddles[SIDE_LEFT].controller == CTRL_HUMAN;
    bool right_human = G.paddles[SIDE_RIGHT].controller == CTRL_HUMAN;
    float p1 = axis_from(ACT_P1_UP, ACT_P1_DOWN, trust);
    float p2 = axis_from(ACT_P2_UP, ACT_P2_DOWN, trust);

    if (left_human && right_human) {
        G.paddles[SIDE_LEFT].input = p1;
        G.paddles[SIDE_RIGHT].input = p2;
    } else if (left_human || right_human) {
        G.paddles[left_human ? SIDE_LEFT : SIDE_RIGHT].input =
            clampf(p1 + p2, -1.0f, 1.0f);
    }
}

/* ---------- paddles ---------- */

/* Paddle.y is the TOP edge, matching x being the left edge -- Paddle is a rect
 * of {x, y, w, h}, as render.c and the rules fixtures read it. Anything that
 * wants the middle of the face asks for paddle_center(). */
static float paddle_center(const Paddle *p)
{
    return p->y + p->h * 0.5f;
}

static void move_paddle(Paddle *p, float dt)
{
    p->vy = p->input * PADDLE_SPEED;
    p->y = clampf(p->y + p->vy * dt, 0.0f, LOGICAL_H - p->h);
}

/* Ball-centre x where a strike on `side`'s face happens. */
static float face_x(int side)
{
    return side == SIDE_LEFT ? PADDLE_INSET + PADDLE_W + BALL_RADIUS
                             : LOGICAL_W - PADDLE_INSET - PADDLE_W - BALL_RADIUS;
}

/* Folds a free-flight height into the band the ball centre can occupy,
   [R, H-R]; *bounces receives the number of wall reflections. */
static float fold_height(float y, int *bounces)
{
    float lo = BALL_RADIUS, span = LOGICAL_H - 2.0f * BALL_RADIUS;
    float period = 2.0f * span;
    float t = fmodf(y - lo, period);
    if (t < 0.0f) t += period;
    if (bounces) {
        float laps = floorf((y - lo) / span);
        *bounces = (int)fabsf(laps);
    }
    return lo + (t > span ? period - t : t);
}

/* Seconds until the ball reaches `side`'s face, or -1 if it is not inbound. */
static float time_to_face(int side)
{
    const Ball *b = &G.ball;
    float dir = side == SIDE_LEFT ? -1.0f : 1.0f;
    if (!b->active || b->vx * dir <= 0.0f) return -1.0f;
    float t = (face_x(side) - b->x) / b->vx;
    return t < 0.0f ? 0.0f : t;
}

static void steer_toward(Paddle *p, float target, float speed)
{
    float delta = target - paddle_center(p);
    p->input = fabsf(delta) < CPU_DEADZONE ? 0.0f : (delta > 0.0f ? 1.0f : -1.0f);
    p->vy = p->input * speed;
    p->y = clampf(p->y + p->vy * TICK_DT, 0.0f, LOGICAL_H - p->h);
}

static void update_cpu(int side)
{
    Paddle *p = &G.paddles[side];
    CpuBrain *c = &p->cpu;
    const CpuLevel *L = &cpu_levels[G.level];
    float speed = PADDLE_SPEED * L->speed_scale;
    float t = (G.state == GS_PLAYING || G.state == GS_SERVE) ? time_to_face(side) : -1.0f;

    if (t < 0.0f) {                       /* ball away or dead: drift home */
        c->phase = CPU_IDLE;
        steer_toward(p, LOGICAL_H * 0.5f, speed * L->idle_speed);
        return;
    }
    if (c->phase == CPU_IDLE) {
        c->phase = CPU_REACTING;
        c->timer = L->react_ticks + (int)(game_randf() * (float)(L->react_jitter + 1));
    }
    if (c->phase == CPU_REACTING) {
        if (--c->timer > 0) {             /* still reading the shot */
            p->input = 0.0f;
            p->vy = 0.0f;
            return;
        }
        int bounces = 0;
        (void)fold_height(G.ball.y + G.ball.vy * t, &bounces);
        float sigma = L->err_base + L->err_bounce * (float)bounces +
                      L->err_speed * (G.ball.speed / BALL_SPEED_MAX);
        c->error = sigma * rand_gauss();
        /* English: strike so the return heads away from the opponent. */
        float opponent = paddle_center(&G.paddles[side == SIDE_LEFT ? SIDE_RIGHT : SIDE_LEFT]);
        float amount = L->english * (0.5f + 0.5f * game_randf());
        c->aim = opponent < LOGICAL_H * 0.5f ? amount : -amount;
        c->phase = CPU_TRACKING;
        c->refines = L->refines;
        c->timer = 0;
    }
    if (c->timer <= 0) {                  /* (re)plan; sharpen a bounded number of times */
        float crossing = fold_height(G.ball.y + G.ball.vy * t, NULL);
        c->target = crossing + c->error - c->aim * p->h * 0.5f;
        if (c->refines > 0) {
            c->error *= L->settle;
            c->refines--;
        }
        c->timer = L->replan_ticks;
    }
    c->timer--;
    steer_toward(p, c->target, speed);
}

/* ---------- neural player ---------- */

void game_policy_features(int side, float out[POLICY_FEATURES])
{
    const Ball *b = &G.ball;
    const Paddle *own = &G.paddles[side];
    const Paddle *opp = &G.paddles[side == SIDE_LEFT ? SIDE_RIGHT : SIDE_LEFT];
    bool mirror = side == SIDE_RIGHT;
    float x = mirror ? LOGICAL_W - b->x : b->x;
    float vx = mirror ? -b->vx : b->vx;
    bool active = b->active;
    float t = active ? time_to_face(side) : -1.0f;

    out[0] = x / LOGICAL_W * 2.0f - 1.0f;
    out[1] = b->y / LOGICAL_H * 2.0f - 1.0f;
    out[2] = active ? vx / BALL_SPEED_MAX : 0.0f;
    out[3] = active ? b->vy / BALL_SPEED_MAX : 0.0f;
    out[4] = paddle_center(own) / LOGICAL_H * 2.0f - 1.0f;
    out[5] = paddle_center(opp) / LOGICAL_H * 2.0f - 1.0f;
    out[6] = active ? 1.0f : 0.0f;
    /* Free-flight physics only: time to reach this face and the unfolded
       crossing height. Wall folding and strategy are left to the network. */
    out[7] = t >= 0.0f ? t / 3.0f : 0.0f;
    out[8] = t >= 0.0f ? (b->y + b->vy * t) / LOGICAL_H * 2.0f - 1.0f : 0.0f;
    out[9] = opp->vy / PADDLE_SPEED;
    out[10] = active ? b->speed / BALL_SPEED_MAX : 0.0f;
}

static void update_neural(int side)
{
    Paddle *p = &G.paddles[side];
    float features[POLICY_FEATURES], logits[POLICY_ACTIONS];
    game_policy_features(side, features);
    if (kilix_policy_forward(&neural_policy, features, POLICY_FEATURES,
                             logits, POLICY_ACTIONS) != KILIX_POLICY_OK) {
        update_cpu(side);
        return;
    }
    size_t action = kilix_policy_argmax(logits, POLICY_ACTIONS);
    p->input = action == 0 ? -1.0f : (action == 2 ? 1.0f : 0.0f);
    move_paddle(p, TICK_DT);
}

/* Moves both paddles by controller. Human input was applied just before. */
static void drive_paddles(void)
{
    for (int side = 0; side < SIDE_COUNT; side++) {
        switch (G.paddles[side].controller) {
        case CTRL_CPU:
            update_cpu(side);
            break;
        case CTRL_NEURAL:
            if (neural_loaded == 1) update_neural(side);
            else update_cpu(side);
            break;
        default:
            move_paddle(&G.paddles[side], TICK_DT);
            break;
        }
    }
}

/* ---------- ball ---------- */

static void push_trail(void)
{
    Ball *b = &G.ball;
    b->trail_head = (b->trail_head + 1) % BALL_TRAIL_LEN;
    b->trail_x[b->trail_head] = b->x;
    b->trail_y[b->trail_head] = b->y;
}

/* Recomputes velocity after a paddle contact.
 *
 * offset is where on the face the ball struck, -1 (top) .. +1 (bottom). The
 * result is renormalized to the new speed and then floored at BALL_MIN_VX of
 * horizontal component, so extreme english can never leave the ball crawling
 * vertically between the paddles forever.
 */
static void deflect(Ball *b, float offset, float dir)
{
    b->speed = clampf(b->speed + BALL_SPEED_GAIN, BALL_SPEED_MIN, BALL_SPEED_MAX);

    float vy_unit = clampf(offset, -1.0f, 1.0f) * 0.75f;
    float vx_unit = sqrtf(1.0f - vy_unit * vy_unit);

    if (vx_unit < BALL_MIN_VX) {
        vx_unit = BALL_MIN_VX;
        float room = 1.0f - vx_unit * vx_unit;
        float mag = sqrtf(room < 0.0f ? 0.0f : room);
        vy_unit = (vy_unit < 0.0f) ? -mag : mag;
    }

    b->vx = dir * vx_unit * b->speed;
    b->vy = vy_unit * b->speed;
}

/* Returns true if the ball was deflected by this paddle on this substep.
 * Direction-gated: a paddle can only hit a ball that is travelling toward it,
 * which is what stops a ball that is already past the face from being grabbed
 * back (the back-face double-hit). */
static bool paddle_hit(Paddle *p, int side)
{
    Ball *b = &G.ball;
    float half = p->h * 0.5f;
    float center = paddle_center(p);

    if (b->y + BALL_RADIUS < p->y || b->y - BALL_RADIUS > p->y + p->h) return false;

    if (side == SIDE_LEFT) {
        if (b->vx >= 0.0f) return false;                 /* moving away */
        float face = p->x + p->w;
        if (b->x - BALL_RADIUS > face) return false;      /* not there yet */
        if (b->x + BALL_RADIUS < p->x) return false;      /* fully behind */
        b->x = face + BALL_RADIUS;                        /* separate */
        deflect(b, (b->y - center) / half, 1.0f);
    } else {
        if (b->vx <= 0.0f) return false;
        float face = p->x;
        if (b->x + BALL_RADIUS < face) return false;
        if (b->x - BALL_RADIUS > p->x + p->w) return false;
        b->x = face - BALL_RADIUS;
        deflect(b, (b->y - center) / half, -1.0f);
    }
    return true;
}

static void award_point(int side)
{
    G.paddles[side].score++;
    G.ball.active = false;
    G.serve_to = (side == SIDE_LEFT) ? SIDE_RIGHT : SIDE_LEFT;
    G.flash = 0.5f;
    G.shake = 3.0f;

    float x = (side == SIDE_LEFT) ? LOGICAL_W - 6.0f : 6.0f;
    spawn_particles(x, G.ball.y, 22, 0xFFE8C0FFu, 30.0f, 130.0f);

    if (G.paddles[side].score >= WIN_SCORE) {
        G.winner = side;
        G.state = GS_GAMEOVER;
        G.state_timer = 0.0f;
        play(SFX_GAMEOVER, 0.9f, 1.0f);
    } else {
        G.state = GS_POINT;
        G.state_timer = 0.9f;
        play(SFX_SCORE, 0.85f, 1.0f);
        play(SFX_MISS, 0.7f, 1.0f);
    }
}

/* Advances the ball with bounded substeps.
 *
 * The substep count is derived from BALL_SPEED_MAX and MAX_SUBSTEP so that no
 * single step can move the ball further than MAX_SUBSTEP logical units -- less
 * than a paddle's width. That is what makes tunneling impossible rather than
 * merely unlikely; the two constants are a pair and the header says so.
 */
static void step_ball(float dt)
{
    Ball *b = &G.ball;
    if (!b->active) return;

    float travel = sqrtf(b->vx * b->vx + b->vy * b->vy) * dt;
    int steps = (int)(travel / MAX_SUBSTEP) + 1;
    if (steps > 32) steps = 32;
    float sub = dt / (float)steps;

    for (int i = 0; i < steps; i++) {
        b->x += b->vx * sub;
        b->y += b->vy * sub;

        if (b->y - BALL_RADIUS <= 0.0f && b->vy < 0.0f) {
            b->y = BALL_RADIUS;
            b->vy = -b->vy;
            play(SFX_WALL, 0.6f, 1.0f);
            spawn_particles(b->x, b->y, 4, 0x9FD8FFFFu, 20.0f, 70.0f);
        } else if (b->y + BALL_RADIUS >= LOGICAL_H && b->vy > 0.0f) {
            b->y = LOGICAL_H - BALL_RADIUS;
            b->vy = -b->vy;
            play(SFX_WALL, 0.6f, 1.0f);
            spawn_particles(b->x, b->y, 4, 0x9FD8FFFFu, 20.0f, 70.0f);
        }

        for (int s = 0; s < SIDE_COUNT; s++) {
            if (paddle_hit(&G.paddles[s], s)) {
                G.rally++;
                G.shake = 1.6f;
                float pitch = 1.0f + (float)G.rally * 0.012f;
                play(SFX_PADDLE, 0.85f, clampf(pitch, 1.0f, 1.6f));
                spawn_particles(b->x, b->y, 6, 0xFFFFFFFFu, 25.0f, 90.0f);
                break;   /* at most one paddle per substep */
            }
        }

        if (b->x < -BALL_RADIUS * 2.0f) { award_point(SIDE_RIGHT); return; }
        if (b->x > LOGICAL_W + BALL_RADIUS * 2.0f) { award_point(SIDE_LEFT); return; }
    }
    push_trail();
}

/* ---------- tick ---------- */

void game_tick(void)
{
    G.ticks++;

    if (G.shake > 0.0f) G.shake = (G.shake > 0.06f) ? G.shake * 0.86f : 0.0f;
    if (G.flash > 0.0f) G.flash = (G.flash > 0.01f) ? G.flash * 0.88f : 0.0f;
    update_particles();

    switch (G.state) {
    case GS_TITLE:
    case GS_GAMEOVER:
    case GS_PAUSED:
        return;

    case GS_SERVE:
        apply_input();
        drive_paddles();
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            G.ball.active = true;
            G.state = GS_PLAYING;
        }
        return;

    case GS_POINT:
        apply_input();
        drive_paddles();
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            serve_ball();
            G.state = GS_SERVE;
            G.state_timer = 0.7f;
            play(SFX_SERVE, 0.8f, 1.0f);
        }
        return;

    case GS_PLAYING:
    default:
        break;
    }

    apply_input();
    drive_paddles();
    step_ball(TICK_DT);
}

/* Drives every HUMAN paddle from the sim itself, for --selftest and
 * --render-test: it exercises real rallies without any terminal or input.
 * Each chases the inbound ball's current height with a small per-rally aim
 * error, so autopilot rallies end rather than run forever. */
static void autopilot_side(int side, int up_act, int down_act)
{
    const Ball *b = &G.ball;
    Paddle *p = &G.paddles[side];
    float dir = side == SIDE_LEFT ? -1.0f : 1.0f;
    float error = (float)((G.rally * 7 + side * 3) % 11) - 5.0f;
    float target = (b->active && b->vx * dir > 0.0f) ? b->y + error
                                                     : LOGICAL_H * 0.5f;
    float delta = target - paddle_center(p);
    bool up = delta < -3.0f, down = delta > 3.0f;
    set_action(up_act, up);
    set_action(down_act, down);
}

void game_autopilot(void)
{
    if (G.state == GS_TITLE || G.state == GS_GAMEOVER) {
        game_start();
        return;
    }
    if (G.state == GS_PAUSED) {
        G.state = G.ball.active ? GS_PLAYING : GS_SERVE;
        return;
    }
    bool left_human = G.paddles[SIDE_LEFT].controller == CTRL_HUMAN;
    bool right_human = G.paddles[SIDE_RIGHT].controller == CTRL_HUMAN;
    if (left_human) autopilot_side(SIDE_LEFT, ACT_P1_UP, ACT_P1_DOWN);
    if (right_human) {
        /* A lone right-side human hears both key pairs; drive one. */
        if (left_human) autopilot_side(SIDE_RIGHT, ACT_P2_UP, ACT_P2_DOWN);
        else autopilot_side(SIDE_RIGHT, ACT_P1_UP, ACT_P1_DOWN);
    }
}

/* ---------- introspection ---------- */

/* FNV-1a over quantized state. Floats are quantized to a fixed grid so the
 * digest is a statement about the simulation, not about float formatting. */
static void digest_add(uint64_t *h, int64_t v)
{
    /* Convert before shifting: right-shifting a negative signed value is
       implementation-defined in C, which would undercut the whole point of a
       digest meant to be comparable across toolchains. */
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        *h ^= (u >> (i * 8)) & 0xFFu;
        *h *= 1099511628211ULL;
    }
}

static void digest_addf(uint64_t *h, float v)
{
    digest_add(h, (int64_t)(v * 1000.0f));
}

uint64_t game_state_digest(void)
{
    uint64_t h = 1469598103934665603ULL;

    digest_add(&h, G.state);
    digest_add(&h, G.setup[SIDE_LEFT]);
    digest_add(&h, G.setup[SIDE_RIGHT]);
    digest_add(&h, G.level);
    digest_add(&h, (int64_t)G.ticks);
    digest_add(&h, G.rally);
    digest_add(&h, G.serve_to);
    digest_add(&h, G.winner);
    digest_add(&h, G.rng);

    digest_addf(&h, G.ball.x);
    digest_addf(&h, G.ball.y);
    digest_addf(&h, G.ball.vx);
    digest_addf(&h, G.ball.vy);
    digest_addf(&h, G.ball.speed);
    digest_add(&h, G.ball.active);

    for (int s = 0; s < SIDE_COUNT; s++) {
        digest_addf(&h, G.paddles[s].y);
        digest_add(&h, G.paddles[s].score);
        digest_add(&h, G.paddles[s].cpu.phase);
        digest_add(&h, G.paddles[s].cpu.timer);
        digest_addf(&h, G.paddles[s].cpu.error);
        digest_addf(&h, G.paddles[s].cpu.target);
    }
    return h;
}

bool game_validate(char *error, size_t error_len)
{
#define FAIL(...)                                              \
    do {                                                       \
        if (error && error_len) snprintf(error, error_len, __VA_ARGS__); \
        return false;                                          \
    } while (0)

    if (G.level < 0 || G.level >= LEVEL_COUNT)
        FAIL("level out of range: %d", G.level);
    for (int s = 0; s < SIDE_COUNT; s++) {
        const Paddle *p = &G.paddles[s];
        if (p->controller < 0 || p->controller >= CTRL_COUNT)
            FAIL("paddle %d controller out of range: %d", s, p->controller);
        if (!isfinite(p->cpu.error) || !isfinite(p->cpu.target))
            FAIL("paddle %d CPU state is not finite", s);
        if (p->y < -0.01f || p->y + p->h > LOGICAL_H + 0.01f)
            FAIL("paddle %d left the playfield: y=%.3f h=%.3f",
                 s, (double)p->y, (double)p->h);
        if (p->score < 0 || p->score > WIN_SCORE)
            FAIL("paddle %d score out of range: %d", s, p->score);
    }

    const Ball *b = &G.ball;
    if (b->active) {
        if (b->y < -1.0f || b->y > LOGICAL_H + 1.0f)
            FAIL("ball escaped vertically: y=%.3f", (double)b->y);
        float speed = sqrtf(b->vx * b->vx + b->vy * b->vy);
        if (speed > BALL_SPEED_MAX + 1.0f)
            FAIL("ball exceeded the speed cap: %.3f > %.3f",
                 (double)speed, (double)BALL_SPEED_MAX);
        if (speed > 0.1f && fabsf(b->vx) < speed * BALL_MIN_VX * 0.9f)
            FAIL("ball lost its horizontal component: vx=%.3f speed=%.3f",
                 (double)b->vx, (double)speed);
    }

    if (G.state == GS_GAMEOVER && G.winner < 0)
        FAIL("gameover with no winner");
    if (G.winner >= 0 && G.paddles[G.winner].score < WIN_SCORE)
        FAIL("winner %d has only %d points", G.winner, G.paddles[G.winner].score);
    return true;
#undef FAIL
}
