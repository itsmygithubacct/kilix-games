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

#include <stdio.h>
#include <string.h>
#include <math.h>

GameState G;

/* AI/autopilot tuning lives here, not in the shared header: codex_pong's
 * modules never reference it, and the header is the cross-module contract
 * rather than a dumping ground for one module's internals. Both biases are
 * drawn from the seeded RNG and reset on init/start, so replays stay exact
 * without needing to appear in game_state_digest.
 */
#define AI_SPEED_SCALE 0.88f   /* handicap vs. a human's PADDLE_SPEED */

static float ai_bias;          /* AI aim error, re-rolled each point */
static float autopilot_bias;   /* autopilot aim error, for headless rallies */

/* Pause restores the exact state it interrupted rather than inferring one from
 * ball.active: pausing during GS_POINT and resuming into GS_SERVE would skip
 * the remainder of that point's dwell. */
static int paused_from = GS_PLAYING;

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
    }
    l->is_ai = false;
    r->is_ai = (G.mode == MODE_AI);
}

/* Places the ball at center and aims it at serve_to's opponent. */
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
    G.mode = MODE_AI;
    G.winner = -1;
    G.serve_to = SIDE_RIGHT;
    reset_paddles();
    serve_ball();
    G.ball.active = false;
}

void game_start(int mode)
{
    G.mode = (mode >= 0 && mode < MODE_COUNT) ? mode : MODE_AI;
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
    paused_from = GS_PLAYING;
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
        paused_from = G.state;
        G.state = GS_PAUSED;
        play(SFX_MENU, 0.7f, 1.0f);
    } else if (G.state == GS_PAUSED) {
        G.state = paused_from;
        play(SFX_MENU, 0.7f, 1.1f);
    }
}

void game_handle_event(const KeyEvent *ev)
{
    if (!ev) return;

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

    case KEY_LEFT:
    case KEY_RIGHT:              /* mode select, title only */
        if (G.state == GS_TITLE) {
            G.mode = (ev->key == KEY_LEFT) ? MODE_AI : MODE_2P;
            play(SFX_MENU, 0.7f, 1.15f);
        }
        break;

    case ' ': case KEY_ENTER:
        if (G.state == GS_TITLE) {
            game_start(G.mode);
        } else if (G.state == GS_GAMEOVER) {
            game_start(G.mode);       /* direct rematch; Esc goes to title */
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

static void apply_input(void)
{
    bool trust = G.headless ? true : term_has_key_release();

    G.paddles[SIDE_LEFT].input = axis_from(ACT_P1_UP, ACT_P1_DOWN, trust);
    if (!G.paddles[SIDE_RIGHT].is_ai)
        G.paddles[SIDE_RIGHT].input = axis_from(ACT_P2_UP, ACT_P2_DOWN, trust);
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

/* Imperfect tracking AI: it reacts to the ball's current position with a
 * deadzone and a speed handicap, and only commits once the ball is heading its
 * way. Beatable on purpose -- a perfect tracker is unplayable and boring. */
static void update_ai(float dt)
{
    Paddle *p = &G.paddles[SIDE_RIGHT];
    if (!p->is_ai) return;

    const Ball *b = &G.ball;
    float target;

    if (b->vx > 0.0f && b->active) {
        /* Predict where the ball crosses the paddle plane, folding wall
           bounces, then miss slightly so rallies stay alive. */
        float dx = (p->x - b->x);
        float t = (fabsf(b->vx) > 0.001f) ? dx / b->vx : 0.0f;
        float predicted = b->y + b->vy * t;

        /* Reflect the prediction into the playfield instead of iterating. */
        float span = LOGICAL_H * 2.0f;
        float wrapped = fmodf(fabsf(predicted), span);
        if (wrapped < 0.0f) wrapped += span;
        target = (wrapped > LOGICAL_H) ? (span - wrapped) : wrapped;
        target += ai_bias;
    } else {
        target = LOGICAL_H * 0.5f;   /* recenter while idle */
    }

    float delta = target - paddle_center(p);
    float deadzone = 3.0f;
    if (fabsf(delta) < deadzone) {
        p->input = 0.0f;
    } else {
        p->input = (delta > 0.0f) ? 1.0f : -1.0f;
    }

    p->vy = p->input * PADDLE_SPEED * AI_SPEED_SCALE;
    p->y = clampf(p->y + p->vy * dt, 0.0f, LOGICAL_H - p->h);
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
        move_paddle(&G.paddles[SIDE_LEFT], TICK_DT);
        if (!G.paddles[SIDE_RIGHT].is_ai) move_paddle(&G.paddles[SIDE_RIGHT], TICK_DT);
        else update_ai(TICK_DT);
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            G.ball.active = true;
            G.state = GS_PLAYING;
        }
        return;

    case GS_POINT:
        apply_input();
        move_paddle(&G.paddles[SIDE_LEFT], TICK_DT);
        if (!G.paddles[SIDE_RIGHT].is_ai) move_paddle(&G.paddles[SIDE_RIGHT], TICK_DT);
        else update_ai(TICK_DT);
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            ai_bias = rand_range(-6.0f, 6.0f);
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
    move_paddle(&G.paddles[SIDE_LEFT], TICK_DT);
    if (!G.paddles[SIDE_RIGHT].is_ai) move_paddle(&G.paddles[SIDE_RIGHT], TICK_DT);
    else update_ai(TICK_DT);
    step_ball(TICK_DT);
}

/* Drives both paddles from the sim itself, for --selftest and --render-test:
 * it exercises real rallies without any terminal or input. */
void game_autopilot(void)
{
    if (G.state == GS_TITLE) {
        game_start(G.mode);
        return;
    }
    if (G.state == GS_GAMEOVER) {
        game_start(G.mode);
        return;
    }
    if (G.state == GS_PAUSED) {
        G.state = G.ball.active ? GS_PLAYING : GS_SERVE;
        return;
    }

    const Ball *b = &G.ball;
    Paddle *l = &G.paddles[SIDE_LEFT];

    /* Left paddle chases the ball when it is inbound, with a small seeded
       error so autopilot rallies end rather than run forever. */
    float target = (b->vx < 0.0f && b->active) ? b->y + autopilot_bias
                                               : LOGICAL_H * 0.5f;
    float delta = target - paddle_center(l);
    if (fabsf(delta) < 3.0f) set_action(ACT_P1_UP, false), set_action(ACT_P1_DOWN, false);
    else if (delta < 0.0f)   set_action(ACT_P1_UP, true),  set_action(ACT_P1_DOWN, false);
    else                     set_action(ACT_P1_UP, false), set_action(ACT_P1_DOWN, true);

    if (!G.paddles[SIDE_RIGHT].is_ai) {
        Paddle *r = &G.paddles[SIDE_RIGHT];
        float rt = (b->vx > 0.0f && b->active) ? b->y : LOGICAL_H * 0.5f;
        float rd = rt - paddle_center(r);
        if (fabsf(rd) < 3.0f) set_action(ACT_P2_UP, false), set_action(ACT_P2_DOWN, false);
        else if (rd < 0.0f)   set_action(ACT_P2_UP, true),  set_action(ACT_P2_DOWN, false);
        else                  set_action(ACT_P2_UP, false), set_action(ACT_P2_DOWN, true);
    }
}

/* ---------- introspection ---------- */

/* FNV-1a over quantized state. Floats are quantized to a fixed grid so the
 * digest is a statement about the simulation, not about float formatting. */
static void digest_add(uint64_t *h, int64_t v)
{
    for (int i = 0; i < 8; i++) {
        *h ^= (uint64_t)((v >> (i * 8)) & 0xFF);
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
    digest_add(&h, G.mode);
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

    for (int s = 0; s < SIDE_COUNT; s++) {
        const Paddle *p = &G.paddles[s];
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
