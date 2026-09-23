#include "joustix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RIDER_W 18.0f
#define RIDER_H 13.0f
#define EGG_W 5.0f
#define EGG_H 6.0f
#define PLAYER_ACCEL 230.0f
#define PLAYER_DRAG 65.0f
#define PLAYER_MAX_SPEED 68.0f
#define DIRECTION_LATCH 0.26f

GameState G;

static const float enemy_speed[EN_TYPE_COUNT] = { 32.0f, 43.0f, 54.0f };
static const float enemy_flap_delay[EN_TYPE_COUNT] = { 0.62f, 0.45f, 0.30f };
static const int enemy_points[EN_TYPE_COUNT] = { 500, 750, 1500 };

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

float game_randf(void)
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return (G.rng >> 8) * (1.0f / 16777216.0f);
}

static void set_message(const char *text, float seconds)
{
    snprintf(G.message, sizeof G.message, "%s", text);
    G.message_timer = seconds;
}

static void init_platforms(void)
{
    const Platform p[PLATFORM_COUNT] = {
        /* Compact player pads: P1 uses the left; the right is reserved for P2. */
        {   8, 147, 34, 5 }, { 278, 147, 34, 5 },
        {  67, 125, 58, 4 }, { 195, 125, 58, 4 },
        { 124,  99, 72, 4 },
        {  18,  64, 72, 4 }, { 230,  64, 72, 4 },
        { 120,  35, 80, 4 },
    };
    memcpy(G.platforms, p, sizeof p);
}

static void particle(float x, float y, float vx, float vy, float life,
                     uint32_t color)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (p->active) continue;
        *p = (Particle){ x, y, vx, vy, life, life, color, true };
        return;
    }
}

static void burst(float x, float y, uint32_t color, int count, float speed)
{
    for (int i = 0; i < count; i++) {
        float a = game_randf() * 6.2831853f;
        float s = speed * (0.35f + game_randf() * 0.65f);
        particle(x, y, cosf(a) * s, sinf(a) * s,
                 0.35f + game_randf() * 0.45f, color);
    }
}

static void landing_dust(float x, float y, float impact)
{
    int count = impact > 46.0f ? 8 : 5;
    for (int i = 0; i < count; i++) {
        float side = i & 1 ? 1.0f : -1.0f;
        particle(x + side * (1.5f + i * .35f), y,
                 side * (7.0f + i * 1.6f),
                 -4.0f - (i % 3) * 2.2f,
                 .22f + (i % 4) * .035f, 0xc9a66b);
    }
}

static void spawn_player_at(int platform_index)
{
    Platform *p = &G.platforms[platform_index];
    G.player = (Rider){0};
    G.player.x = p->x + p->w * 0.5f - RIDER_W * 0.5f;
    G.player.y = p->y - RIDER_H;
    G.player.prev_y = G.player.y;
    G.player.dir = p->x < LOGICAL_W * 0.5f ? 1 : -1;
    G.player.active = true;
    G.player.on_platform = true;
    G.player.spawn_timer = 0.85f;
    G.player.invuln = 1.55f;
    G.respawn_timer = 0;
    G.left_input = G.right_input = 0;
    G.step_sound_timer = 0;
    burst(G.player.x + RIDER_W * 0.5f, G.player.y + RIDER_H * 0.5f,
          0xd9f6ff, 18, 28.0f);
}

static void respawn_player(void)
{
    spawn_player_at(0);
}

static Enemy *spawn_enemy(int type, float x, float y)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        if (e->rider.active) continue;
        memset(e, 0, sizeof *e);
        e->type = type;
        e->rider.x = x;
        e->rider.y = y;
        e->rider.prev_y = y;
        e->rider.dir = game_randf() < 0.5f ? -1 : 1;
        e->rider.active = true;
        e->rider.spawn_timer = 0.55f + game_randf() * 0.45f;
        e->rider.invuln = e->rider.spawn_timer;
        e->think_timer = game_randf() * 0.15f;
        e->flap_timer = game_randf() * enemy_flap_delay[type];
        burst(x + RIDER_W * 0.5f, y + RIDER_H * 0.5f,
              type == EN_SHADOW ? 0xc084fc : 0xb8f7b1, 10, 18.0f);
        return e;
    }
    return NULL;
}

static Egg *spawn_egg(float x, float y, int type)
{
    for (int i = 0; i < MAX_EGGS; i++) {
        Egg *e = &G.eggs[i];
        if (e->active) continue;
        *e = (Egg){0};
        e->x = x;
        e->y = y;
        e->vx = (game_randf() * 2.0f - 1.0f) * 18.0f;
        e->vy = -18.0f - game_randf() * 12.0f;
        e->hatch_timer = fmaxf(2.8f, 5.5f - G.wave * 0.13f);
        e->collect_delay = 0.32f;
        e->type = type;
        e->active = true;
        return e;
    }
    return NULL;
}

int game_active_enemies(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        n += G.enemies[i].rider.active;
    return n;
}

int game_active_eggs(void)
{
    int n = 0;
    for (int i = 0; i < MAX_EGGS; i++) n += G.eggs[i].active;
    return n;
}

static void begin_wave(void)
{
    static const int waves[5][EN_TYPE_COUNT] = {
        { 3, 0, 0 }, { 4, 1, 0 }, { 3, 2, 0 }, { 2, 3, 1 }, { 2, 4, 2 },
    };
    G.wave++;
    /* Player 1 owns the left pad; the matching right pad is reserved for P2. */
    spawn_player_at(0);
    /* Never carry an active troll attack across a wave boundary. */
    G.lava_troll_phase = 0;
    G.lava_troll_timer = fmaxf(G.lava_troll_timer, 4.0f);
    int counts[EN_TYPE_COUNT];
    if (G.wave <= 5) {
        memcpy(counts, waves[G.wave - 1], sizeof counts);
    } else {
        counts[EN_BOUNDER] = 2 + G.wave / 3;
        counts[EN_HUNTER] = 1 + G.wave / 2;
        counts[EN_SHADOW] = 1 + (G.wave - 4) / 2;
    }
    counts[EN_HUNTER] += G.difficulty >= 2 && G.wave > 1;
    int slot = 0;
    for (int type = 0; type < EN_TYPE_COUNT; type++) {
        for (int j = 0; j < counts[type] && game_active_enemies() < MAX_ENEMIES; j++) {
            const Platform *p = &G.platforms[5 + (slot % 3)];
            float x = p->x + 5.0f + fmodf(slot * 23.0f, fmaxf(1.0f, p->w - 26.0f));
            float y = p->y - RIDER_H - 3.0f - (slot % 2) * 10.0f;
            spawn_enemy(type, x, y);
            slot++;
        }
    }
    G.state = GS_PLAYING;
    char text[32];
    snprintf(text, sizeof text, "WAVE %d", G.wave);
    set_message(text, 1.5f);
    sound_play(SFX_WAVE, 0.8f, 1.0f + G.wave * 0.012f);
}

void game_init(int w, int h, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    G.W = w;
    G.H = h;
    G.rng = seed ? seed : 0x4a6f7573u;
    G.state = GS_TITLE;
    G.sound_on = true;
    G.difficulty = 1;
    G.lava_troll_timer = 8.0f + game_randf() * 8.0f;
    init_platforms();
}

void game_start(void)
{
    int high = G.high_score;
    int difficulty = G.difficulty;
    bool sound_on = G.sound_on;
    int w = G.W, h = G.H;
    uint32_t rng = G.rng;
    memset(&G, 0, sizeof G);
    G.W = w;
    G.H = h;
    G.rng = rng ? rng : 1;
    G.high_score = high;
    G.difficulty = difficulty;
    G.sound_on = sound_on;
    G.lives = 3;
    G.state = GS_PLAYING;
    G.lava_troll_timer = 7.0f + game_randf() * 7.0f;
    init_platforms();
    begin_wave();
}

static bool horizontal_overlap(float ax, float aw, float bx, float bw)
{
    return ax + aw > bx && ax < bx + bw;
}

static bool boxes_overlap(float ax, float ay, float aw, float ah,
                          float bx, float by, float bw, float bh)
{
    return horizontal_overlap(ax, aw, bx, bw) && ay + ah > by && ay < by + bh;
}

static bool update_rider_physics(Rider *r)
{
    if (!r->active) return false;
    if (r->spawn_timer > 0) {
        r->spawn_timer = fmaxf(0, r->spawn_timer - TICK_DT);
        r->invuln = fmaxf(r->invuln, r->spawn_timer);
        return false;
    }
    r->prev_y = r->y;
    r->vy = fminf(82.0f, r->vy + 98.0f * TICK_DT);
    r->x += r->vx * TICK_DT;
    r->y += r->vy * TICK_DT;
    if (r->x < -RIDER_W) r->x += LOGICAL_W + RIDER_W;
    if (r->x > LOGICAL_W) r->x -= LOGICAL_W + RIDER_W;

    r->on_platform = false;
    for (int i = 0; i < PLATFORM_COUNT; i++) {
        Platform *p = &G.platforms[i];
        if (!horizontal_overlap(r->x + 2, RIDER_W - 4, p->x, p->w)) continue;
        float old_bottom = r->prev_y + RIDER_H;
        float new_bottom = r->y + RIDER_H;
        if (r->vy >= 0 && old_bottom <= p->y + 0.8f && new_bottom >= p->y) {
            r->y = p->y - RIDER_H;
            r->vy = 0;
            r->on_platform = true;
            break;
        }
        float underside = p->y + p->h;
        if (r->vy < 0 && r->prev_y >= underside - 0.8f && r->y <= underside) {
            r->y = underside;
            r->vy = fmaxf(8.0f, -r->vy * 0.32f);
            break;
        }
    }
    if (r->y < 4.0f) {
        r->y = 4.0f;
        if (r->vy < 0) r->vy = -r->vy * 0.28f;
    }
    r->invuln = fmaxf(0, r->invuln - TICK_DT);
    r->flap_anim = fmaxf(0, r->flap_anim - TICK_DT);
    r->flap_cooldown = fmaxf(0, r->flap_cooldown - TICK_DT);
    r->collide_cooldown = fmaxf(0, r->collide_cooldown - TICK_DT);
    return r->y + RIDER_H > LAVA_TOP + 3.0f;
}

static void flap(Rider *r, float power)
{
    if (!r->active || r->spawn_timer > 0 || r->flap_cooldown > 0) return;
    r->vy = -power;
    r->flap_anim = 0.22f;
    r->flap_cooldown = 0.105f;
}

static void flap_player(void)
{
    if (!G.player.active || G.player.flap_cooldown > 0 ||
        G.player.spawn_timer > 0) return;
    flap(&G.player, 62.0f);
    sound_play(SFX_FLAP, 0.42f, 0.92f + game_randf() * 0.12f);
}

static void kill_player(void)
{
    if (!G.player.active || G.player.invuln > 0) return;
    float x = G.player.x + RIDER_W * 0.5f;
    float y = G.player.y + RIDER_H * 0.5f;
    G.player.active = false;
    G.lives--;
    G.shake = 0.38f;
    G.flash = 0.18f;
    burst(x, y, 0xfacc15, 26, 43.0f);
    sound_play(SFX_HURT, 0.9f, 1.0f);
    if (G.lives <= 0) {
        G.state = GS_GAMEOVER;
        G.gameover_choice = GAMEOVER_RESTART;
        if (G.score > G.high_score) G.high_score = G.score;
        set_message("GAME OVER", 99.0f);
    } else {
        G.respawn_timer = 1.05f;
        set_message("RIDER LOST", 1.0f);
    }
}

static void kill_enemy(Enemy *e, bool make_egg)
{
    if (!e->rider.active) return;
    float x = e->rider.x + RIDER_W * 0.5f;
    float y = e->rider.y + RIDER_H * 0.5f;
    int type = e->type;
    e->rider.active = false;
    G.shake = 0.16f;
    burst(x, y, type == EN_SHADOW ? 0xc084fc : 0xfde68a, 20, 36.0f);
    if (make_egg) {
        G.score += enemy_points[type];
        if (G.score > G.high_score) G.high_score = G.score;
        spawn_egg(x - EGG_W * 0.5f, y - EGG_H * 0.5f, type);
        sound_play(SFX_JOUST, 0.9f, 0.92f + type * 0.10f);
    } else {
        sound_play(SFX_LAVA, 0.55f, 0.8f);
    }
}

static void update_player(void)
{
    Rider *p = &G.player;
    if (!p->active) {
        if (G.lives > 0 && (G.respawn_timer -= TICK_DT) <= 0) respawn_player();
        return;
    }
    int axis;
    if (G.held_controls) {
        axis = (G.held_right ? 1 : 0) - (G.held_left ? 1 : 0);
    } else {
        G.left_input = fmaxf(0, G.left_input - TICK_DT);
        G.right_input = fmaxf(0, G.right_input - TICK_DT);
        axis = (G.right_input > 0) - (G.left_input > 0);
    }
    if (axis) {
        p->dir = axis;
        p->vx += axis * PLAYER_ACCEL * TICK_DT;
    } else {
        float drag = PLAYER_DRAG * TICK_DT;
        if (p->vx > 0) p->vx = fmaxf(0, p->vx - drag);
        else p->vx = fminf(0, p->vx + drag);
    }
    p->vx = clampf(p->vx, -PLAYER_MAX_SPEED, PLAYER_MAX_SPEED);
    if (G.held_controls && G.held_flap) flap_player();
    bool was_on_platform = p->on_platform;
    float landing_speed = p->vy;
    bool hit_lava = update_rider_physics(p);
    if (!was_on_platform && p->on_platform && landing_speed > 16.0f) {
        landing_dust(p->x + RIDER_W * .5f, p->y + RIDER_H, landing_speed);
        sound_play(SFX_LAND, clampf(.20f + landing_speed * .004f, .26f, .48f),
                   clampf(1.08f - landing_speed * .003f, .82f, 1.02f));
        if (landing_speed > 52.0f) G.shake = fmaxf(G.shake, .035f);
    }
    if (p->spawn_timer <= 0 && p->on_platform && fabsf(p->vx) > 12.0f) {
        G.step_sound_timer -= TICK_DT;
        if (G.step_sound_timer <= 0) {
            G.step_sound_side ^= 1;
            sound_play(SFX_STEP, 0.20f, G.step_sound_side ? 0.94f : 1.04f);
            G.step_sound_timer = clampf(.34f - fabsf(p->vx) * .0024f, .17f, .30f);
        }
    } else {
        G.step_sound_timer = 0;
    }
    if (hit_lava) kill_player();
}

void game_set_held_controls(bool available, bool left, bool right, bool flap_held)
{
    G.held_controls = available;
    G.held_left = available && left;
    G.held_right = available && right;
    G.held_flap = available && flap_held;
    if (available) G.left_input = G.right_input = 0;
}

static float wrapped_dx(float from, float to)
{
    float dx = to - from;
    if (dx > LOGICAL_W * 0.5f) dx -= LOGICAL_W;
    if (dx < -LOGICAL_W * 0.5f) dx += LOGICAL_W;
    return dx;
}

static void update_enemy(Enemy *e)
{
    Rider *r = &e->rider;
    if (!r->active) return;
    if (r->spawn_timer <= 0 && G.player.active) {
        e->think_timer -= TICK_DT;
        e->flap_timer -= TICK_DT;
        if (e->think_timer <= 0) {
            float dx = wrapped_dx(r->x, G.player.x);
            float reaction = e->type == EN_BOUNDER ? 0.14f :
                             e->type == EN_HUNTER ? 0.09f : 0.045f;
            e->think_timer = reaction + game_randf() * reaction;
            if (fabsf(dx) > 5.0f) r->dir = dx < 0 ? -1 : 1;
        }
        float speed = enemy_speed[e->type] * (0.90f + G.difficulty * 0.10f);
        float desired = r->dir * speed;
        r->vx += clampf(desired - r->vx, -110.0f * TICK_DT, 110.0f * TICK_DT);

        bool needs_height = r->y > G.player.y - (e->type * 3.0f + 4.0f);
        bool lava_panic = r->y > 135.0f;
        if (e->flap_timer <= 0 && (needs_height || lava_panic || game_randf() < 0.045f)) {
            flap(r, 54.0f + e->type * 4.0f);
            float delay = enemy_flap_delay[e->type] * (1.10f - G.difficulty * 0.10f);
            e->flap_timer = delay * (0.72f + game_randf() * 0.56f);
        }
    }
    if (update_rider_physics(r)) kill_enemy(e, false);
}

static void check_jousts(void)
{
    if (!G.player.active || G.player.spawn_timer > 0 || G.player.invuln > 0) return;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        Rider *r = &e->rider;
        if (!r->active || r->spawn_timer > 0 || r->collide_cooldown > 0 ||
            G.player.collide_cooldown > 0) continue;
        if (!boxes_overlap(G.player.x + 1, G.player.y + 1, RIDER_W - 2, RIDER_H - 1,
                           r->x + 1, r->y + 1, RIDER_W - 2, RIDER_H - 1)) continue;
        float py = G.player.y + RIDER_H * 0.42f;
        float ey = r->y + RIDER_H * 0.42f;
        if (py < ey - 1.65f) {
            G.player.vy = fminf(G.player.vy, -20.0f);
            kill_enemy(e, true);
        } else if (ey < py - 1.65f) {
            r->vy = fminf(r->vy, -18.0f);
            kill_player();
            return;
        } else {
            float pv = G.player.vx;
            G.player.vx = r->vx * 0.72f;
            r->vx = pv * 0.72f;
            G.player.vy -= 8.0f;
            r->vy -= 8.0f;
            G.player.collide_cooldown = r->collide_cooldown = 0.24f;
            sound_play(SFX_JOUST, 0.45f, 1.35f);
        }
    }
}

static void collect_egg(Egg *e)
{
    e->active = false;
    int quick_bonus = (int)(fmaxf(0, e->hatch_timer) * 20.0f);
    G.score += 250 + quick_bonus;
    if (G.score > G.high_score) G.high_score = G.score;
    burst(e->x + EGG_W * 0.5f, e->y + EGG_H * 0.5f,
          0xfff7c2, 14, 25.0f);
    sound_play(SFX_EGG, 0.8f, 1.0f);
}

static void update_eggs(void)
{
    for (int i = 0; i < MAX_EGGS; i++) {
        Egg *e = &G.eggs[i];
        if (!e->active) continue;
        e->hatch_timer -= TICK_DT;
        e->collect_delay = fmaxf(0, e->collect_delay - TICK_DT);
        if (!e->grounded) {
            float old_bottom = e->y + EGG_H;
            e->vy = fminf(72.0f, e->vy + 82.0f * TICK_DT);
            e->x += e->vx * TICK_DT;
            e->y += e->vy * TICK_DT;
            if (e->x < -EGG_W) e->x += LOGICAL_W + EGG_W;
            if (e->x > LOGICAL_W) e->x -= LOGICAL_W + EGG_W;
            for (int j = 0; j < PLATFORM_COUNT; j++) {
                Platform *p = &G.platforms[j];
                if (e->vy >= 0 && horizontal_overlap(e->x, EGG_W, p->x, p->w) &&
                    old_bottom <= p->y + 0.8f && e->y + EGG_H >= p->y) {
                    e->y = p->y - EGG_H;
                    e->vy = e->vx = 0;
                    e->grounded = true;
                    break;
                }
            }
        }
        if (e->y + EGG_H > LAVA_TOP + 2) {
            e->active = false;
            burst(e->x, LAVA_TOP, 0xff8a25, 8, 18.0f);
            continue;
        }
        if (e->collect_delay <= 0 && G.player.active && G.player.spawn_timer <= 0 &&
            boxes_overlap(G.player.x, G.player.y, RIDER_W, RIDER_H,
                          e->x, e->y, EGG_W, EGG_H)) {
            collect_egg(e);
            continue;
        }
        if (e->hatch_timer <= 0) {
            float x = e->x, y = e->y;
            int type = e->type;
            e->active = false;
            spawn_enemy(type, x - 5, y - RIDER_H + EGG_H);
            burst(x, y, 0xe9d5ff, 16, 27.0f);
            sound_play(SFX_HATCH, 0.8f, 0.9f + type * 0.1f);
        }
    }
}

static void update_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->life -= TICK_DT;
        if (p->life <= 0) { p->active = false; continue; }
        p->vy += 42.0f * TICK_DT;
        p->x += p->vx * TICK_DT;
        p->y += p->vy * TICK_DT;
    }
}

static void update_lava_troll(void)
{
    if (G.lava_troll_phase > 0) {
        G.lava_troll_phase += TICK_DT;
        if (G.lava_troll_phase > 2.8f) {
            G.lava_troll_phase = 0;
            G.lava_troll_timer = 8.0f + game_randf() * 10.0f;
        } else if (G.lava_troll_phase > 0.9f && G.lava_troll_phase < 1.75f &&
                   G.player.active && G.player.y + RIDER_H > LAVA_TOP - 13.0f) {
            if (fabsf(wrapped_dx(G.lava_troll_x,
                                 G.player.x + RIDER_W * 0.5f)) < 11.0f)
                kill_player();
        }
    } else if ((G.lava_troll_timer -= TICK_DT) <= 0) {
        G.lava_troll_phase = 0.001f;
        G.lava_troll_x = 34.0f + game_randf() * 252.0f;
    }
}

void game_tick(void)
{
    G.ticks++;
    G.scene_time += TICK_DT;
    G.shake = fmaxf(0, G.shake - TICK_DT);
    G.flash = fmaxf(0, G.flash - TICK_DT);
    G.message_timer = fmaxf(0, G.message_timer - TICK_DT);
    update_particles();
    if (G.state == GS_TITLE || G.state == GS_PAUSED || G.state == GS_GAMEOVER)
        return;
    if (G.state == GS_WAVE) {
        if ((G.wave_timer -= TICK_DT) <= 0) begin_wave();
        return;
    }
    update_lava_troll();
    update_player();
    for (int i = 0; i < MAX_ENEMIES; i++) update_enemy(&G.enemies[i]);
    check_jousts();
    update_eggs();
    if (G.state == GS_PLAYING && game_active_enemies() == 0 && game_active_eggs() == 0) {
        G.score += 1000 + G.wave * 100;
        if (G.score > G.high_score) G.high_score = G.score;
        G.state = GS_WAVE;
        G.wave_timer = 2.15f;
        set_message("WAVE CLEARED", 2.15f);
        sound_play(SFX_WAVE, 0.9f, 1.22f);
    }
}

void game_handle_key(int key)
{
    if (key == 'q' || key == 'Q') { G.quit = true; return; }
    if (key == 'm' || key == 'M') {
        G.sound_on = !G.sound_on;
        sound_set_enabled(G.sound_on);
        if (G.sound_on) sound_play(SFX_MENU, 0.65f, 1.2f);
        return;
    }
    if (G.state == GS_TITLE) {
        if (key == KEY_LEFT || key == 'a' || key == 'A') {
            G.difficulty = (G.difficulty + 2) % 3;
            sound_play(SFX_MENU, 0.5f, 0.9f);
        } else if (key == KEY_RIGHT || key == 'd' || key == 'D') {
            G.difficulty = (G.difficulty + 1) % 3;
            sound_play(SFX_MENU, 0.5f, 1.1f);
        } else if (key == KEY_ENTER || key == ' ') {
            sound_play(SFX_MENU, 0.7f, 1.35f);
            game_start();
        }
        return;
    }
    if (G.state == GS_GAMEOVER) {
        int previous = G.gameover_choice;
        if (key == KEY_UP || key == KEY_LEFT || key == 'w' || key == 'W' ||
            key == 'a' || key == 'A') {
            G.gameover_choice = GAMEOVER_RESTART;
        } else if (key == KEY_DOWN || key == KEY_RIGHT || key == 's' || key == 'S' ||
                   key == 'd' || key == 'D') {
            G.gameover_choice = GAMEOVER_MENU;
        } else if (key == KEY_ENTER) {
            sound_play(SFX_MENU, 0.7f, 1.25f);
            if (G.gameover_choice == GAMEOVER_RESTART) {
                game_start();
            } else {
                G.state = GS_TITLE;
                G.player.active = false;
                memset(G.enemies, 0, sizeof G.enemies);
                memset(G.eggs, 0, sizeof G.eggs);
                memset(G.particles, 0, sizeof G.particles);
                G.left_input = G.right_input = 0;
                G.wave_timer = G.respawn_timer = 0;
                set_message("", 0);
            }
            return;
        }
        if (G.gameover_choice != previous) sound_play(SFX_MENU, 0.5f, 1.0f);
        return;
    }
    if (key == 'p' || key == 'P' || key == KEY_ESC) {
        if (G.state == GS_PAUSED) {
            G.state = G.wave_timer > 0 ? GS_WAVE : GS_PLAYING;
            set_message("", 0);
        } else if (G.state == GS_PLAYING || G.state == GS_WAVE) {
            G.state = GS_PAUSED;
            set_message("PAUSED", 99.0f);
        }
        sound_play(SFX_MENU, 0.55f, 1.0f);
        return;
    }
    if (G.state != GS_PLAYING) return;
    if (key == KEY_LEFT || key == 'a' || key == 'A') {
        G.left_input = DIRECTION_LATCH;
        G.right_input = 0;
        G.player.dir = -1;
    } else if (key == KEY_RIGHT || key == 'd' || key == 'D') {
        G.right_input = DIRECTION_LATCH;
        G.left_input = 0;
        G.player.dir = 1;
    } else if (key == KEY_UP || key == 'w' || key == 'W' || key == ' ') {
        flap_player();
    }
}

void game_autopilot(void)
{
    if (G.state == GS_TITLE || G.state == GS_GAMEOVER) {
        game_start();
        return;
    }
    if (G.state != GS_PLAYING || !G.player.active || G.player.spawn_timer > 0) {
        game_set_held_controls(true, false, false, false);
        return;
    }
    float tx = LOGICAL_W * 0.5f, ty = 95.0f;
    float best = 1e9f;
    for (int i = 0; i < MAX_EGGS; i++) {
        Egg *e = &G.eggs[i];
        if (!e->active) continue;
        float d = fabsf(wrapped_dx(G.player.x, e->x)) + fabsf(G.player.y - e->y) * 0.55f;
        if (d < best) { best = d; tx = e->x; ty = e->y - 4; }
    }
    if (best == 1e9f) {
        for (int i = 0; i < MAX_ENEMIES; i++) {
            Enemy *e = &G.enemies[i];
            if (!e->rider.active) continue;
            float d = fabsf(wrapped_dx(G.player.x, e->rider.x)) +
                      fabsf(G.player.y - e->rider.y) * 0.5f;
            if (d < best) { best = d; tx = e->rider.x; ty = e->rider.y - 7; }
        }
    }
    float dx = wrapped_dx(G.player.x, tx);
    game_set_held_controls(true, dx < -2, dx > 2,
                           G.player.y > ty || G.player.y > 132.0f);
}

bool game_validate(char *error, size_t error_len)
{
#define BAD(...) do { snprintf(error, error_len, __VA_ARGS__); return false; } while (0)
    if (G.wave < 0 || G.lives < 0 || G.lives > 3) BAD("bad counters: wave=%d lives=%d", G.wave, G.lives);
    if (G.gameover_choice < 0 || G.gameover_choice >= GAMEOVER_OPTION_COUNT)
        BAD("bad game-over choice: %d", G.gameover_choice);
    if (G.score < 0 || G.high_score < G.score) BAD("bad score: %d high=%d", G.score, G.high_score);
    if (game_active_enemies() > MAX_ENEMIES || game_active_eggs() > MAX_EGGS) BAD("object count overflow");
    if (G.player.active && (!isfinite(G.player.x) || !isfinite(G.player.y) ||
                            !isfinite(G.player.vx) || !isfinite(G.player.vy))) BAD("non-finite player");
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Rider *r = &G.enemies[i].rider;
        if (r->active && (!isfinite(r->x) || !isfinite(r->y))) BAD("non-finite enemy %d", i);
    }
    if (!isfinite(G.lava_troll_x)) BAD("non-finite lava troll");
    error[0] = '\0';
    return true;
#undef BAD
}
