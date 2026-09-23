/* Game state, physics, collisions, level generation, and input. */
#include "kitty_brokeout.h"
#include "kilix_state.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846f

GameState G;

const char *POWERUP_NAMES[PU_COUNT] = {
    "WIDE", "SLOW", "MULTI", "SHIELD"
};

static float vlen(float x, float y) { return sqrtf(x * x + y * y); }

float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void frand_seed(uint32_t seed)
{
    G.rng = seed ? seed : 0x8f7011edu;
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

static bool high_score_store_init(kilixstate_store *store)
{
    kilixstate_options options;

    kilixstate_options_init(&options);
    options.app_id = "kitty-brokeout";
    options.filename = "highscore";
    options.max_payload = 4u;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static uint32_t read_le32(const unsigned char bytes[4])
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void write_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

static void load_high_score(void)
{
    kilixstate_store store;
    unsigned char payload[4];
    size_t payload_size = 0u;
    kilixstate_result result;

    if (!high_score_store_init(&store)) return;
    result = kilixstate_load(&store, payload, sizeof payload, &payload_size);
    if (result == KILIXSTATE_OK && payload_size == sizeof payload &&
        read_le32(payload) <= (uint32_t)INT_MAX) {
        G.highScore = (int)read_le32(payload);
    } else if (result == KILIXSTATE_CORRUPT) {
        /* One-time upgrade from the original decimal text file. */
        char path[KILIXSTATE_PATH_CAPACITY];
        int score = 0;
        if (kilixstate_store_path(&store, path, sizeof path) == KILIXSTATE_OK) {
            FILE *file = fopen(path, "r");
            if (file != NULL) {
                if (fscanf(file, "%d", &score) == 1 && score > 0) {
                    G.highScore = score;
                    write_le32(payload, (uint32_t)score);
                    (void)kilixstate_save(&store, payload, sizeof payload);
                }
                (void)fclose(file);
            }
        }
    }
    G.savedHighScore = G.highScore;
    kilixstate_store_close(&store);
}

static void save_high_score(void)
{
    if (G.headless || G.highScore <= G.savedHighScore) return;
    kilixstate_store store;
    unsigned char payload[4];

    if (!high_score_store_init(&store)) return;
    write_le32(payload, (uint32_t)G.highScore);
    if (kilixstate_save(&store, payload, sizeof payload) == KILIXSTATE_OK)
        G.savedHighScore = G.highScore;
    kilixstate_store_close(&store);
}

static void update_high_score(void)
{
    if (G.score > G.highScore)
        G.highScore = G.score;
}

static void layout_playfield(void)
{
    float sx = G.W / 1000.0f;
    float sy = G.H / 640.0f;
    G.scale = clampf(fminf(sx, sy), 0.62f, 1.72f);
    float s = G.scale;

    G.playX = floorf(30.0f * s);
    G.playY = floorf(48.0f * s);
    G.playW = G.W - G.playX * 2.0f;
    G.playH = G.H - G.playY - 42.0f * s;
    if (G.playW < 560.0f) G.playW = G.W - G.playX * 2.0f;
    if (G.playH < 330.0f) G.playH = G.H - G.playY - 24.0f * s;
}

static void add_particle(float x, float y, float vx, float vy,
                         float life, float size, uint32_t color, int kind)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) {
            *p = (Particle){
                .active = true, .x = x, .y = y, .vx = vx, .vy = vy,
                .life = life, .maxLife = life, .size = size,
                .color = color, .kind = kind
            };
            return;
        }
    }
}

static void burst(float x, float y, uint32_t color, int count, float power)
{
    for (int i = 0; i < count; i++) {
        float a = frandr(0.0f, PI * 2.0f);
        float sp = frandr(power * 0.18f, power);
        float sz = frandr(1.5f, 4.5f) * G.scale;
        add_particle(x + frandr(-3, 3) * G.scale, y + frandr(-3, 3) * G.scale,
                     cosf(a) * sp, sinf(a) * sp,
                     frandr(0.22f, 0.82f), sz, color, 0);
    }
}

static void clear_dynamic(void)
{
    memset(G.balls, 0, sizeof G.balls);
    memset(G.powerups, 0, sizeof G.powerups);
    memset(G.particles, 0, sizeof G.particles);
    G.combo = 0;
    G.comboTimer = 0.0f;
    G.stateTimer = 0.0f;
    G.levelTimer = 0.0f;
    G.launchAngle = 0.0f;
    G.speedBoostTimer = 0.0f;
    G.screenFlash = 0.0f;
    G.cameraShake = 0.0f;
}

static void set_paddle_width(float width)
{
    float cx = G.paddle.x + G.paddle.w * 0.5f;
    G.paddle.w = width;
    G.paddle.x = clampf(cx - G.paddle.w * 0.5f, G.playX + 8 * G.scale,
                       G.playX + G.playW - G.paddle.w - 8 * G.scale);
}

static void reset_paddle(void)
{
    float s = G.scale;
    G.paddle.w = 116.0f * s;
    G.paddle.h = 11.0f * s;
    G.paddle.x = G.playX + (G.playW - G.paddle.w) * 0.5f;
    G.paddle.y = G.playY + G.playH - 32.0f * s;
    G.paddle.moveAxis = 0.0f;
    G.paddle.intentTimer = 0.0f;
    G.paddle.wideTimer = 0.0f;
    G.paddle.shieldTimer = 0.0f;
}

float game_ball_speed_target(void)
{
    float s = G.scale;
    float base = (292.0f + G.level * 18.0f + fminf(G.levelTimer * 4.2f, 120.0f)) * s;
    if (G.speedBoostTimer > 0.0f) base *= 1.23f;
    return fminf(base, 610.0f * s);
}

static void seed_ball_trail(Ball *b)
{
    for (int i = 0; i < 10; i++) {
        b->trailX[i] = b->x;
        b->trailY[i] = b->y;
    }
    b->trailHead = 0;
}

static void attach_new_ball(void)
{
    memset(G.balls, 0, sizeof G.balls);
    Ball *b = &G.balls[0];
    G.launchAngle = 0.0f;
    b->active = true;
    b->attached = true;
    b->radius = fmaxf(3.5f, 5.2f * G.scale);
    b->speed = game_ball_speed_target();
    b->x = G.paddle.x + G.paddle.w * 0.5f;
    b->y = G.paddle.y - b->radius - 2.0f * G.scale;
    seed_ball_trail(b);
}

static void launch_ball(Ball *b)
{
    if (!b || !b->active || !b->attached) return;
    float angle = clampf(G.launchAngle, -0.82f, 0.82f);
    float speed = game_ball_speed_target();
    b->attached = false;
    b->speed = speed;
    b->vx = sinf(angle) * speed;
    b->vy = -cosf(angle) * speed;
    sound_play(SND_LAUNCH, 0.55f, 0.95f + frandf() * 0.1f);
}

static bool has_attached_ball(void)
{
    for (int i = 0; i < MAX_BALLS; i++)
        if (G.balls[i].active && G.balls[i].attached)
            return true;
    return false;
}

static void nudge_launch_aim(float delta)
{
    if (!has_attached_ball()) return;
    G.launchAngle = clampf(G.launchAngle + delta, -0.82f, 0.82f);
}

static uint32_t brick_color(const Brick *b)
{
    if (b->type == BRICK_METAL) return 0x9ca3af;
    if (b->type == BRICK_EXPLOSIVE) return 0xd946ef;
    if (b->type == BRICK_SPEED) return 0x22d3ee;
    if (b->hits >= 3) return 0xef4444;
    if (b->hits == 2) return 0xf97316;
    return 0xfacc15;
}

static void spawn_powerup(float x, float y, int forced)
{
    int type = forced;
    if (type < 0) {
        float r = frandf();
        type = r < 0.30f ? PU_WIDE : r < 0.55f ? PU_SLOW : r < 0.78f ? PU_MULTI : PU_SHIELD;
    }
    for (int i = 0; i < MAX_POWERUPS; i++) {
        Powerup *p = &G.powerups[i];
        if (!p->active) {
            *p = (Powerup){
                .active = true, .type = type, .x = x, .y = y,
                .vy = (92.0f + 9.0f * G.level) * G.scale,
                .phase = frandf() * PI * 2.0f
            };
            return;
        }
    }
}

static void create_level(void)
{
    memset(G.bricks, 0, sizeof G.bricks);
    memset(G.powerups, 0, sizeof G.powerups);
    G.numBricks = 0;

    int cols = G.playW > 1180.0f ? 14 : 12;
    int rows = 5 + G.level;
    if (rows > 11) rows = 11;
    float s = G.scale;
    float gap = fmaxf(2.0f, 3.0f * s);
    float side = 24.0f * s;
    float bw = floorf((G.playW - side * 2.0f - gap * (cols - 1)) / cols);
    float bh = fmaxf(9.0f, floorf(16.0f * s));
    float startX = G.playX + (G.playW - (bw * cols + gap * (cols - 1))) * 0.5f;
    float startY = G.playY + 58.0f * s;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            if (G.numBricks >= MAX_BRICKS) return;
            float pattern = sinf((col + 1) * 1.7f + G.level * 0.43f) +
                            cosf((row + 2) * 1.23f);
            if (G.level >= 5 && row > 1 && ((col + row + G.level) % 11 == 0) &&
                pattern > 0.4f) {
                continue;
            }

            int type = BRICK_NORMAL;
            float r = frandf();
            if (G.level >= 4 && r < 0.070f) type = BRICK_METAL;
            else if (G.level >= 3 && r < 0.145f) type = BRICK_EXPLOSIVE;
            else if (G.level >= 2 && r < 0.215f) type = BRICK_SPEED;

            int hits = 1;
            if (type == BRICK_NORMAL) {
                hits = row < 2 ? 1 : row < 5 ? 2 : 3;
                if (hits > G.level) hits = G.level;
                if (hits < 1) hits = 1;
            }

            G.bricks[G.numBricks++] = (Brick){
                .alive = true,
                .type = type,
                .hits = type == BRICK_METAL ? 99 : hits,
                .maxHits = type == BRICK_METAL ? 1 : hits,
                .x = startX + col * (bw + gap),
                .y = startY + row * (bh + gap),
                .w = bw,
                .h = bh,
                .pulse = frandf() * 0.5f
            };
        }
    }
}

int game_remaining_breakable_bricks(void)
{
    int n = 0;
    for (int i = 0; i < G.numBricks; i++)
        if (G.bricks[i].alive && G.bricks[i].type != BRICK_METAL) n++;
    return n;
}

int game_active_ball_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_BALLS; i++)
        if (G.balls[i].active) n++;
    return n;
}

static void normalize_ball(Ball *b, float target)
{
    float speed = vlen(b->vx, b->vy);
    if (speed < 1.0f) {
        b->vx = 0.0f;
        b->vy = -target;
        b->speed = target;
        return;
    }
    float k = target / speed;
    b->vx *= k;
    b->vy *= k;
    b->speed = target;
}

static bool circle_rect(float cx, float cy, float r,
                        float x, float y, float w, float h)
{
    float qx = clampf(cx, x, x + w);
    float qy = clampf(cy, y, y + h);
    float dx = cx - qx;
    float dy = cy - qy;
    return dx * dx + dy * dy <= r * r;
}

static void reflect_from_rect(Ball *b, float px, float py, const Brick *br)
{
    float r = b->radius;
    if (py <= br->y - r) {
        b->y = br->y - r - 0.2f;
        b->vy = -fabsf(b->vy);
        return;
    }
    if (py >= br->y + br->h + r) {
        b->y = br->y + br->h + r + 0.2f;
        b->vy = fabsf(b->vy);
        return;
    }
    if (px <= br->x - r) {
        b->x = br->x - r - 0.2f;
        b->vx = -fabsf(b->vx);
        return;
    }
    if (px >= br->x + br->w + r) {
        b->x = br->x + br->w + r + 0.2f;
        b->vx = fabsf(b->vx);
        return;
    }

    float left = fabsf((b->x + r) - br->x);
    float right = fabsf((br->x + br->w) - (b->x - r));
    float top = fabsf((b->y + r) - br->y);
    float bottom = fabsf((br->y + br->h) - (b->y - r));
    float m = left;
    int side = 0;
    if (right < m) { m = right; side = 1; }
    if (top < m) { m = top; side = 2; }
    if (bottom < m) side = 3;

    if (side == 0) { b->x = br->x - r - 0.2f; b->vx = -fabsf(b->vx); }
    else if (side == 1) { b->x = br->x + br->w + r + 0.2f; b->vx = fabsf(b->vx); }
    else if (side == 2) { b->y = br->y - r - 0.2f; b->vy = -fabsf(b->vy); }
    else { b->y = br->y + br->h + r + 0.2f; b->vy = fabsf(b->vy); }
}

static void complete_level_if_needed(void)
{
    if (G.state == GS_PLAYING && game_remaining_breakable_bricks() == 0) {
        G.state = GS_LEVEL_CLEAR;
        G.stateTimer = 0.0f;
        G.score += 500 + G.level * 120 + G.lives * 75;
        update_high_score();
        save_high_score();
        G.screenFlash = fmaxf(G.screenFlash, 0.9f);
        G.cameraShake = fmaxf(G.cameraShake, 9.0f * G.scale);
        sound_play(SND_CLEAR, 0.8f, 1.0f);
    }
}

static void explode_at(float x, float y, float radius, int sourceIndex)
{
    G.screenFlash = fmaxf(G.screenFlash, 0.78f);
    G.cameraShake = fmaxf(G.cameraShake, 10.0f * G.scale);
    burst(x, y, 0xf0abfc, 58, 235.0f * G.scale);
    sound_play(SND_EXPLODE, 0.75f, 0.9f + frandf() * 0.16f);

    float r2 = radius * radius;
    for (int i = 0; i < G.numBricks; i++) {
        Brick *b = &G.bricks[i];
        if (!b->alive || i == sourceIndex) continue;
        float cx = b->x + b->w * 0.5f;
        float cy = b->y + b->h * 0.5f;
        float dx = cx - x;
        float dy = cy - y;
        if (dx * dx + dy * dy > r2) continue;
        if (b->type == BRICK_METAL) {
            b->pulse = 1.0f;
            burst(cx, cy, 0xcbd5e1, 8, 110.0f * G.scale);
            continue;
        }
        b->alive = false;
        G.score += 18 + G.level * 4;
        burst(cx, cy, brick_color(b), 15, 170.0f * G.scale);
        if (frandf() < 0.08f) spawn_powerup(cx, cy, -1);
    }
}

static void hit_brick(Ball *ball, int index)
{
    Brick *b = &G.bricks[index];
    b->pulse = 1.0f;

    if (b->type == BRICK_METAL) {
        sound_play(SND_METAL, 0.45f, 0.92f + frandf() * 0.18f);
        burst(ball->x, ball->y, 0xcbd5e1, 8, 115.0f * G.scale);
        return;
    }

    G.combo++;
    G.comboTimer = 2.1f;
    G.score += 8 + G.level * 3 + G.combo * 2;
    b->hits--;
    sound_play(SND_BRICK, 0.48f, 0.86f + 0.04f * G.combo);

    if (b->type == BRICK_SPEED) {
        G.speedBoostTimer = 4.0f;
        G.screenFlash = fmaxf(G.screenFlash, 0.25f);
        spawn_powerup(b->x + b->w * 0.5f, b->y + b->h * 0.5f, PU_SLOW);
    }

    if (b->hits > 0) {
        burst(ball->x, ball->y, brick_color(b), 6, 90.0f * G.scale);
        return;
    }

    b->alive = false;
    float cx = b->x + b->w * 0.5f;
    float cy = b->y + b->h * 0.5f;
    G.score += 22 + G.level * 7 + G.combo * 4;
    burst(cx, cy, brick_color(b), b->type == BRICK_EXPLOSIVE ? 26 : 16,
          b->type == BRICK_EXPLOSIVE ? 220.0f * G.scale : 145.0f * G.scale);

    if (b->type == BRICK_EXPLOSIVE)
        explode_at(cx, cy, b->w * 1.85f, index);
    else if (frandf() < 0.105f + fminf(G.level, 8) * 0.006f)
        spawn_powerup(cx, cy, -1);

    complete_level_if_needed();
}

static void collide_paddle(Ball *b)
{
    Paddle *p = &G.paddle;
    if (b->vy <= 0.0f) return;
    if (!circle_rect(b->x, b->y, b->radius, p->x, p->y, p->w, p->h)) return;

    b->y = p->y - b->radius - 0.3f;
    float hit = (b->x - (p->x + p->w * 0.5f)) / (p->w * 0.5f);
    hit = clampf(hit, -1.0f, 1.0f);
    float angle = hit * 1.12f;
    float speed = fmaxf(game_ball_speed_target() * 1.015f, vlen(b->vx, b->vy));
    b->vx = sinf(angle) * speed + p->moveAxis * 42.0f * G.scale;
    b->vy = -cosf(angle) * speed;
    normalize_ball(b, fminf(speed + 6.0f * G.scale, game_ball_speed_target() * 1.12f));
    G.combo = 0;
    G.comboTimer = 0.0f;
    sound_play(SND_PADDLE, 0.55f, 0.88f + fabsf(hit) * 0.22f);
    burst(b->x, p->y, 0x86efac, 7, 70.0f * G.scale);
}

static bool collide_bricks(Ball *b, float px, float py)
{
    for (int i = 0; i < G.numBricks; i++) {
        Brick *br = &G.bricks[i];
        if (!br->alive) continue;
        if (!circle_rect(b->x, b->y, b->radius, br->x, br->y, br->w, br->h))
            continue;
        reflect_from_rect(b, px, py, br);
        normalize_ball(b, game_ball_speed_target());
        hit_brick(b, i);
        return true;
    }
    return false;
}

static void tick_ball(Ball *b)
{
    if (!b->active) return;

    b->trailHead = (b->trailHead + 1) % 10;
    b->trailX[b->trailHead] = b->x;
    b->trailY[b->trailHead] = b->y;

    if (b->attached) {
        b->x = G.paddle.x + G.paddle.w * 0.5f;
        b->y = G.paddle.y - b->radius - 2.0f * G.scale;
        seed_ball_trail(b);
        return;
    }

    float speed = fmaxf(vlen(b->vx, b->vy), 1.0f);
    int steps = (int)ceilf((speed * TICK_DT) / fmaxf(2.0f, b->radius * 0.55f));
    if (steps < 1) steps = 1;
    if (steps > 14) steps = 14;
    float dt = TICK_DT / steps;

    for (int step = 0; step < steps && b->active; step++) {
        float px = b->x;
        float py = b->y;
        b->x += b->vx * dt;
        b->y += b->vy * dt;

        if (b->x - b->radius < G.playX) {
            b->x = G.playX + b->radius;
            b->vx = fabsf(b->vx);
            sound_play(SND_PADDLE, 0.25f, 0.74f);
        } else if (b->x + b->radius > G.playX + G.playW) {
            b->x = G.playX + G.playW - b->radius;
            b->vx = -fabsf(b->vx);
            sound_play(SND_PADDLE, 0.25f, 0.78f);
        }
        if (b->y - b->radius < G.playY) {
            b->y = G.playY + b->radius;
            b->vy = fabsf(b->vy);
            sound_play(SND_PADDLE, 0.25f, 0.82f);
        }

        float bottom = G.playY + G.playH;
        if (G.paddle.shieldTimer > 0.0f && b->vy > 0.0f &&
            b->y + b->radius >= bottom - 6.0f * G.scale) {
            b->y = bottom - 6.0f * G.scale - b->radius;
            b->vy = -fabsf(b->vy);
            G.paddle.shieldTimer *= 0.68f;
            G.cameraShake = fmaxf(G.cameraShake, 4.0f * G.scale);
            sound_play(SND_POWERUP, 0.35f, 0.62f);
        }

        collide_paddle(b);
        collide_bricks(b, px, py);

        if (b->y - b->radius > bottom + 54.0f * G.scale) {
            b->active = false;
            sound_play(SND_LOSE, 0.45f, 0.9f);
        }
    }
}

static void lose_life(void)
{
    if (G.state != GS_PLAYING) return;
    G.lives--;
    G.combo = 0;
    G.comboTimer = 0.0f;
    G.stateTimer = 0.0f;
    G.cameraShake = fmaxf(G.cameraShake, 5.0f * G.scale);
    if (G.lives <= 0) {
        G.state = GS_GAMEOVER;
        update_high_score();
        save_high_score();
    } else {
        G.state = GS_BALL_LOST;
        attach_new_ball();
    }
}

static void apply_powerup(int type)
{
    float s = G.scale;
    G.score += 55 + G.level * 5;
    G.screenFlash = fmaxf(G.screenFlash, 0.22f);
    sound_play(SND_POWERUP, 0.62f, 0.9f + type * 0.08f);

    if (type == PU_WIDE) {
        set_paddle_width(168.0f * s);
        G.paddle.wideTimer = 11.5f;
    } else if (type == PU_SLOW) {
        G.speedBoostTimer = 0.0f;
        for (int i = 0; i < MAX_BALLS; i++) {
            Ball *b = &G.balls[i];
            if (b->active && !b->attached) {
                b->vx *= 0.72f;
                b->vy *= 0.72f;
                normalize_ball(b, fmaxf(220.0f * s, vlen(b->vx, b->vy)));
            }
        }
    } else if (type == PU_MULTI) {
        int source = -1;
        for (int i = 0; i < MAX_BALLS; i++) {
            if (G.balls[i].active) { source = i; break; }
        }
        if (source >= 0 && G.balls[source].attached) launch_ball(&G.balls[source]);
        int made = 0;
        for (int i = 0; i < MAX_BALLS && made < 2 && source >= 0; i++) {
            if (G.balls[i].active) continue;
            Ball nb = G.balls[source];
            float speed = game_ball_speed_target() * (0.96f + made * 0.08f);
            float a = atan2f(nb.vy, nb.vx) + (made == 0 ? -0.48f : 0.48f);
            nb.vx = cosf(a) * speed;
            nb.vy = sinf(a) * speed;
            nb.attached = false;
            seed_ball_trail(&nb);
            G.balls[i] = nb;
            made++;
        }
    } else if (type == PU_SHIELD) {
        G.paddle.shieldTimer = 12.0f;
    }
}

static void tick_powerups(void)
{
    Paddle *p = &G.paddle;
    for (int i = 0; i < MAX_POWERUPS; i++) {
        Powerup *u = &G.powerups[i];
        if (!u->active) continue;
        u->phase += TICK_DT * 5.0f;
        u->y += u->vy * TICK_DT;
        float w = 38.0f * G.scale;
        float h = 16.0f * G.scale;
        if (u->x + w * 0.5f >= p->x && u->x - w * 0.5f <= p->x + p->w &&
            u->y + h * 0.5f >= p->y && u->y - h * 0.5f <= p->y + p->h + 8 * G.scale) {
            apply_powerup(u->type);
            burst(u->x, u->y, 0xffffff, 18, 120.0f * G.scale);
            u->active = false;
        } else if (u->y > G.playY + G.playH + 38.0f * G.scale) {
            u->active = false;
        }
    }
}

static void tick_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->life -= TICK_DT;
        if (p->life <= 0.0f) {
            p->active = false;
            continue;
        }
        p->x += p->vx * TICK_DT;
        p->y += p->vy * TICK_DT;
        p->vx *= 0.992f;
        p->vy = p->vy * 0.992f + 68.0f * G.scale * TICK_DT;
    }
}

void game_reset_to_title(void)
{
    G.state = GS_TITLE;
    G.stateTimer = 0.0f;
    G.level = 1;
    G.lives = 3;
    G.score = 0;
    clear_dynamic();
    reset_paddle();
    create_level();
    attach_new_ball();
}

void game_start_run(void)
{
    layout_playfield();
    G.state = GS_PLAYING;
    G.score = 0;
    G.level = 1;
    G.lives = 3;
    clear_dynamic();
    reset_paddle();
    create_level();
    attach_new_ball();
}

static void next_level(void)
{
    G.level++;
    G.state = GS_PLAYING;
    clear_dynamic();
    reset_paddle();
    create_level();
    attach_new_ball();
}

void game_init(int w, int h, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    G.W = w;
    G.H = h;
    G.soundEnabled = true;
    frand_seed(seed);
    layout_playfield();
    load_high_score();

    G.numStars = MAX_STARS;
    for (int i = 0; i < G.numStars; i++) {
        G.stars[i] = (Star){
            .x = frandf() * G.W,
            .y = frandf() * G.H,
            .brightness = (uint8_t)(70 + frandf() * 170),
            .phase = frandf() * PI * 2.0f,
            .speed = 0.7f + frandf() * 2.4f,
            .size = frandf() > 0.82f ? 2 : 1
        };
    }

    game_reset_to_title();
}

void game_shutdown(void)
{
    update_high_score();
    save_high_score();
}

void game_tick(void)
{
    G.frameCount++;
    G.stateTimer += TICK_DT;
    if (G.screenFlash > 0.0f) G.screenFlash = fmaxf(0.0f, G.screenFlash - TICK_DT * 1.9f);
    if (G.cameraShake > 0.0f) G.cameraShake = fmaxf(0.0f, G.cameraShake - 34.0f * TICK_DT);

    for (int i = 0; i < G.numBricks; i++)
        if (G.bricks[i].pulse > 0.0f)
            G.bricks[i].pulse = fmaxf(0.0f, G.bricks[i].pulse - TICK_DT * 2.8f);

    tick_particles();

    if (G.state == GS_PLAYING) {
        G.levelTimer += TICK_DT;
        if (G.speedBoostTimer > 0.0f) G.speedBoostTimer = fmaxf(0.0f, G.speedBoostTimer - TICK_DT);
        if (G.comboTimer > 0.0f) {
            G.comboTimer -= TICK_DT;
            if (G.comboTimer <= 0.0f) G.combo = 0;
        }

        float paddleAxis = 0.0f;
        if (G.heldControls) {
            paddleAxis = (G.heldRight ? 1.0f : 0.0f) -
                         (G.heldLeft ? 1.0f : 0.0f);
            G.paddle.moveAxis = paddleAxis;
            G.paddle.intentTimer = 0.0f;
        } else if (G.paddle.intentTimer > 0.0f) {
            paddleAxis = G.paddle.moveAxis;
            G.paddle.intentTimer -= TICK_DT;
        } else {
            G.paddle.moveAxis = 0.0f;
        }
        G.paddle.x += paddleAxis * 610.0f * G.scale * TICK_DT;

        if (G.paddle.wideTimer > 0.0f) {
            G.paddle.wideTimer -= TICK_DT;
            if (G.paddle.wideTimer <= 0.0f)
                set_paddle_width(116.0f * G.scale);
        }
        if (G.paddle.shieldTimer > 0.0f)
            G.paddle.shieldTimer = fmaxf(0.0f, G.paddle.shieldTimer - TICK_DT);

        G.paddle.x = clampf(G.paddle.x, G.playX + 8.0f * G.scale,
                            G.playX + G.playW - G.paddle.w - 8.0f * G.scale);

        for (int i = 0; i < MAX_BALLS; i++)
            tick_ball(&G.balls[i]);
        tick_powerups();
        update_high_score();

        if (game_active_ball_count() == 0)
            lose_life();
        complete_level_if_needed();
    } else if (G.state == GS_BALL_LOST) {
        tick_powerups();
        if (G.stateTimer > 0.90f) {
            G.state = GS_PLAYING;
            G.stateTimer = 0.0f;
        }
    } else if (G.state == GS_TITLE) {
        if ((G.frameCount % 11) == 0 && frandf() < 0.18f) {
            float x = frandr(G.playX + 80 * G.scale, G.playX + G.playW - 80 * G.scale);
            burst(x, G.playY + frandr(80, 180) * G.scale, 0x38bdf8, 2, 35.0f * G.scale);
        }
    }
}

static void move_intent(float axis)
{
    G.paddle.moveAxis = axis;
    G.paddle.intentTimer = 0.135f;
    if (G.state == GS_PLAYING) {
        nudge_launch_aim(axis * 0.075f);
        for (int i = 0; i < MAX_BALLS; i++) {
            if (G.balls[i].active && G.balls[i].attached) {
                G.balls[i].x = G.paddle.x + G.paddle.w * 0.5f;
                G.balls[i].y = G.paddle.y - G.balls[i].radius - 2.0f * G.scale;
            }
        }
    }
}

void game_set_held_controls(bool available, bool left, bool right)
{
    G.heldControls = available;
    G.heldLeft = available && left;
    G.heldRight = available && right;
    if (available) {
        G.paddle.moveAxis = (G.heldRight ? 1.0f : 0.0f) -
                            (G.heldLeft ? 1.0f : 0.0f);
        G.paddle.intentTimer = 0.0f;
    }
}

static void launch_all_attached(void)
{
    for (int i = 0; i < MAX_BALLS; i++)
        if (G.balls[i].active && G.balls[i].attached)
            launch_ball(&G.balls[i]);
}

void game_handle_key(int key)
{
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';

    if (key == 'q') {
        G.quit = true;
        return;
    }
    if (key == 'm') {
        G.soundEnabled = !G.soundEnabled;
        sound_set_enabled(G.soundEnabled);
        sound_play(SND_MENU, 0.4f, G.soundEnabled ? 1.15f : 0.75f);
        return;
    }

    if (G.state == GS_TITLE) {
        if (key == KEY_ENTER || key == ' ' || key == KEY_UP || key == 'w' || key == 'r') {
            sound_play(SND_MENU, 0.5f, 1.1f);
            game_start_run();
        } else if (key == 'c') {
            G.state = GS_CONTROLS;
            G.stateTimer = 0.0f;
            sound_play(SND_MENU, 0.4f, 1.0f);
        }
        return;
    }

    if (G.state == GS_CONTROLS) {
        if (key == KEY_ENTER || key == KEY_ESC || key == ' ' || key == 'c') {
            G.state = GS_TITLE;
            G.stateTimer = 0.0f;
            sound_play(SND_MENU, 0.35f, 0.88f);
        }
        return;
    }

    if (key == 'r') {
        sound_play(SND_MENU, 0.45f, 1.0f);
        game_start_run();
        return;
    }
    if (key == KEY_ESC) {
        if (G.state == GS_PAUSED) G.state = GS_PLAYING;
        else game_reset_to_title();
        sound_play(SND_MENU, 0.35f, 0.86f);
        return;
    }
    if (key == 'p') {
        if (G.state == GS_PLAYING) G.state = GS_PAUSED;
        else if (G.state == GS_PAUSED) G.state = GS_PLAYING;
        sound_play(SND_MENU, 0.35f, 1.0f);
        return;
    }

    if (G.state == GS_PLAYING || G.state == GS_BALL_LOST) {
        if (key == KEY_LEFT || key == 'a') move_intent(-1.0f);
        else if (key == KEY_RIGHT || key == 'd') move_intent(1.0f);
        else if (key == KEY_DOWN || key == 's') G.launchAngle = 0.0f;
        else if (key == KEY_ENTER || key == ' ' || key == KEY_UP || key == 'w')
            launch_all_attached();
        return;
    }

    if (G.state == GS_LEVEL_CLEAR) {
        if (key == KEY_ENTER || key == ' ' || key == KEY_UP || key == 'w') {
            sound_play(SND_MENU, 0.45f, 1.12f);
            next_level();
        }
        return;
    }

    if (G.state == GS_GAMEOVER) {
        if (key == KEY_ENTER || key == ' ' || key == KEY_UP || key == 'w') {
            sound_play(SND_MENU, 0.45f, 1.0f);
            game_start_run();
        }
    }
}

void game_autopilot_tick(void)
{
    if (G.state == GS_TITLE) {
        game_start_run();
        return;
    }
    if (G.state == GS_LEVEL_CLEAR) {
        next_level();
        return;
    }
    if (G.state == GS_GAMEOVER) {
        game_start_run();
        return;
    }

    if (G.state != GS_PLAYING && G.state != GS_BALL_LOST) return;

    Ball *target = NULL;
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!G.balls[i].active) continue;
        if (!target || G.balls[i].y > target->y) target = &G.balls[i];
    }
    float tx = target ? target->x : (G.playX + G.playW * 0.5f);
    float pc = G.paddle.x + G.paddle.w * 0.5f;
    if (tx < pc - 8.0f * G.scale) move_intent(-1.0f);
    else if (tx > pc + 8.0f * G.scale) move_intent(1.0f);
    if (target && target->attached) launch_ball(target);
}

void game_force_level_clear(void)
{
    if (G.state == GS_TITLE) game_start_run();
    for (int i = 0; i < G.numBricks; i++)
        if (G.bricks[i].type != BRICK_METAL)
            G.bricks[i].alive = false;
    G.state = GS_LEVEL_CLEAR;
    G.stateTimer = 0.0f;
    G.score += 1234;
}

void game_force_gameover(void)
{
    G.state = GS_GAMEOVER;
    G.lives = 0;
    G.stateTimer = 0.0f;
    update_high_score();
    save_high_score();
}
