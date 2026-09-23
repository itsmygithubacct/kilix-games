/* Simulation, collision, campaign flow, input, and user profile. */
#include "kilix_jpak.h"
#include "kilix_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_W 11.0f
#define PLAYER_H 15.0f
#define ENEMY_W 12.0f
#define ENEMY_H 12.0f

GameState G;

float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

float game_randf(void)
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return (G.rng >> 8) * (1.0f / 16777216.0f);
}

static float visual_rand(uint32_t salt)
{
    return (hash32((uint32_t)G.ticks * 0x9e3779b9u ^ salt) >> 8) *
           (1.0f / 16777216.0f);
}

static bool overlap(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static int tile_at(float x, float y)
{
    int tx = (int)floorf(x / TILE_SIZE);
    int ty = (int)floorf(y / TILE_SIZE);
    if (tx < 0 || tx >= FIELD_COLS || ty < 0 || ty >= FIELD_ROWS)
        return T_STONE;
    return G.level_data.tiles[ty][tx];
}

static bool tile_kind_solid(int tile)
{
    switch (tile) {
    case T_PANEL: case T_PANEL_DARK: case T_STONE:
    case T_ICE: case T_MOSS: case T_CONVEYOR_LEFT: case T_CONVEYOR_RIGHT:
    case T_BARRIER_CYAN: case T_BARRIER_MAGENTA: case T_BARRIER_AMBER:
    case T_PHASE_GLASS: case T_PHASE_DENSE: case T_PHASE_STEEL:
        return true;
    default:
        return false;
    }
}

bool game_tile_solid(int tx, int ty)
{
    if (tx < 0 || tx >= FIELD_COLS || ty < 0 || ty >= FIELD_ROWS)
        return true;
    int tile = G.level_data.tiles[ty][tx];
    if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER &&
        G.barriers_open[tile - T_BARRIER_CYAN]) return false;
    if ((tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) &&
        G.phase_time[ty][tx] >= 100) return false;
    return tile_kind_solid(tile);
}

static bool rect_hits_solid(float x, float y, float w, float h)
{
    const float inset = 0.12f;
    int x0 = (int)floorf((x + inset) / TILE_SIZE);
    int x1 = (int)floorf((x + w - inset) / TILE_SIZE);
    int y0 = (int)floorf((y + inset) / TILE_SIZE);
    int y1 = (int)floorf((y + h - inset) / TILE_SIZE);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
            if (game_tile_solid(tx, ty)) return true;
    return false;
}

/* ---------- versioned, checksummed XDG profile ---------- */

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

static uint32_t checksum(const uint8_t *bytes, size_t count)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

static bool profile_store_init(kilixstate_store *store)
{
    kilixstate_options options;
    const char *override = getenv("KILIX_JPAK_DATA_HOME");

    kilixstate_options_init(&options);
    options.app_id = "kilix-jpak";
    options.filename = "profile.v1";
    options.base_directory = override && *override ? override : NULL;
    options.max_payload = 24u;
    /* The KJPAK01 payload already has a public version and checksum. */
    options.format = KILIXSTATE_FORMAT_RAW;
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

static void profile_load(void)
{
    if (getenv("KILIX_JPAK_NO_PROFILE")) return;
    kilixstate_store store;
    uint8_t bytes[24];
    size_t size = 0u;

    if (!profile_store_init(&store)) return;
    kilixstate_result result = kilixstate_load(&store, bytes, sizeof bytes,
                                               &size);
    kilixstate_store_close(&store);
    static const uint8_t magic[8] = {'K','J','P','A','K','0','1','\0'};
    if (result != KILIXSTATE_OK || size != sizeof bytes ||
        memcmp(bytes, magic, 8) != 0 ||
        read_le32(bytes + 8) != 1u ||
        read_le32(bytes + 20) != checksum(bytes, 20)) return;
    uint32_t high = read_le32(bytes + 12);
    int unlocked = bytes[16];
    if (high <= 999999999u) G.high_score = (int)high;
    if (unlocked >= 0 && unlocked < CAMPAIGN_LEVELS) {
        G.unlocked_level = unlocked;
        G.selected_level = unlocked;
    }
    G.sound_on = bytes[17] != 0;
    G.saved_high_score = G.high_score;
}

static void profile_save(void)
{
    if (G.headless || getenv("KILIX_JPAK_NO_PROFILE")) return;
    kilixstate_store store;
    uint8_t bytes[24] = {'K','J','P','A','K','0','1','\0'};
    write_le32(bytes + 8, 1u);
    write_le32(bytes + 12, (uint32_t)G.high_score);
    bytes[16] = (uint8_t)G.unlocked_level;
    bytes[17] = G.sound_on ? 1u : 0u;
    write_le32(bytes + 20, checksum(bytes, 20));

    if (!profile_store_init(&store)) return;
    if (kilixstate_save(&store, bytes, sizeof bytes) == KILIXSTATE_OK)
        G.saved_high_score = G.high_score;
    kilixstate_store_close(&store);
}

static void update_high_score(void)
{
    if (!G.practice_mode && G.score > G.high_score) G.high_score = G.score;
}

/* ---------- effects ---------- */

static void add_particle(float x, float y, float vx, float vy, float life,
                         float size, uint32_t color, int kind)
{
    for (int i = 0; i < MAX_PARTICLES; i++) if (!G.particles[i].active) {
        G.particles[i] = (Particle){true, x, y, vx, vy, life, life, size, color, kind};
        return;
    }
}

static void burst(float x, float y, uint32_t color, int count, float speed, int kind)
{
    const float tau = 6.28318530718f;
    for (int i = 0; i < count; i++) {
        float a = visual_rand((uint32_t)(i * 19 + count * 101)) * tau;
        float s = speed * (0.25f + visual_rand((uint32_t)(i * 67 + 11)) * 0.75f);
        add_particle(x, y, cosf(a) * s, sinf(a) * s,
                     0.25f + visual_rand((uint32_t)(i * 83 + 29)) * 0.55f,
                     1.0f + visual_rand((uint32_t)(i * 113 + 7)) * 2.0f,
                     color, kind);
    }
}

static void update_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->life -= TICK_DT;
        if (p->life <= 0) { p->active = false; continue; }
        p->x += p->vx * TICK_DT;
        p->y += p->vy * TICK_DT;
        if (p->kind == PARTICLE_THRUST || p->kind == PARTICLE_SHARD)
            p->vy += 55.0f * TICK_DT;
        p->vx *= 0.992f;
    }
}

static void set_banner(const char *text)
{
    snprintf(G.banner, sizeof G.banner, "%s", text);
    G.banner_timer = 2.0f;
}

/* ---------- level/session flow ---------- */

static void spawn_player(bool fresh_life)
{
    float retained_fuel = G.player.fuel;
    float retained_shield = G.player.shield;
    float retained_stunner = G.player.stunner;
    memset(&G.player, 0, sizeof G.player);
    G.player.x = G.level_data.spawn_x * TILE_SIZE + 2.0f;
    G.player.y = G.level_data.spawn_y * TILE_SIZE + 1.0f;
    G.player.facing = 1;
    G.player.invulnerable = 1.2f;
    if (fresh_life) {
        G.player.fuel = 34.0f;
    } else {
        G.player.fuel = fmaxf(18.0f, retained_fuel);
        G.player.shield = retained_shield;
        G.player.stunner = retained_stunner;
    }
}

void game_load_level(int level, bool fresh_life)
{
    if (level < 0) level = 0;
    if (level >= CAMPAIGN_LEVELS) level = CAMPAIGN_LEVELS - 1;
    G.level = level;
    level_build(level, &G.level_data);
    memset(G.phase_time, 0, sizeof G.phase_time);
    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.particles, 0, sizeof G.particles);
    memset(G.barriers_open, 0, sizeof G.barriers_open);
    G.level_start_score = G.score;
    G.clear_time_bonus = 0;
    G.clear_fuel_bonus = 0;
    G.death_reason[0] = '\0';
    G.crystals_remaining = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (G.level_data.tiles[y][x] == T_CRYSTAL) G.crystals_remaining++;
    G.exit_open = G.crystals_remaining == 0;
    for (int i = 0; i < G.level_data.enemy_count; i++) {
        EnemySpawn spawn = G.level_data.enemies[i];
        Enemy *e = &G.enemies[i];
        *e = (Enemy){
            .active = true, .kind = spawn.kind,
            .x = spawn.x * TILE_SIZE + 2.0f,
            .y = spawn.y * TILE_SIZE + 3.0f,
            .home_x = spawn.x * TILE_SIZE + 2.0f,
            .home_y = spawn.y * TILE_SIZE + 3.0f,
            .timer = 0.3f + i * 0.11f,
            .phase = visual_rand((uint32_t)(level * 41 + i * 17)) * 6.28f,
            .direction = (i + level) & 3
        };
        if (e->kind == EN_ROLLER) e->vx = (i & 1 ? -38.0f : 38.0f);
        if (e->kind == EN_DART) {
            static const float dx[4] = {50, 0, -50, 0};
            static const float dy[4] = {0, 50, 0, -50};
            e->vx = dx[e->direction]; e->vy = dy[e->direction];
        }
        if (e->kind == EN_SHARD) {
            e->vx = (i & 1 ? -34.0f : 34.0f); e->vy = -34.0f;
        }
    }
    spawn_player(fresh_life);
    G.state = GS_PLAYING;
    G.state_timer = 0;
    G.level_time = 0;
    G.flash = 0.35f;
    snprintf(G.banner, sizeof G.banner, "%sLEVEL %03d  %s",
             G.practice_mode ? "PRACTICE  " : "", level + 1,
             G.level_data.title);
    G.banner_timer = 2.4f;
}

void game_start(int level)
{
    G.score = 0;
    G.lives = 4;
    G.selected_level = level;
    game_load_level(level, true);
}

static void return_to_title(void)
{
    update_high_score();
    profile_save();
    sound_jet(false, 0);
    G.state = GS_TITLE;
    G.help_return_state = GS_TITLE;
    G.menu_choice = 0;
    G.state_timer = 0;
    memset(G.particles, 0, sizeof G.particles);
}

static void open_help(int return_state)
{
    G.help_return_state = return_state;
    G.help_page = 0;
    G.state = GS_HELP;
    G.state_timer = 0;
    sound_jet(false, 0);
}

static void close_help(void)
{
    int destination = G.help_return_state;
    if (destination != GS_PLAYING && destination != GS_PAUSED &&
        destination != GS_TITLE) destination = GS_TITLE;
    if (destination == GS_TITLE) {
        return_to_title();
        return;
    }
    G.state = destination;
    G.state_timer = 0;
}

static void complete_level(void)
{
    if (G.state != GS_PLAYING) return;
    G.clear_time_bonus = (int)fmaxf(0.0f, 2400.0f - G.level_time * 18.0f);
    G.clear_fuel_bonus = (int)G.player.fuel;
    G.score += 500 + G.clear_fuel_bonus + G.clear_time_bonus;
    update_high_score();
    G.state = GS_LEVEL_CLEAR;
    G.state_timer = 0;
    G.flash = 1.0f;
    G.shake = 5.0f;
    sound_jet(false, 0);
    sound_play(SFX_CLEAR, 0.7f, 1.0f);
    burst(G.player.x + 6, G.player.y + 7, 0xfcd34d, 38, 95, PARTICLE_SPARK);
    if (!G.practice_mode) {
        int unlocked = G.level + 1;
        if (unlocked >= CAMPAIGN_LEVELS) unlocked = CAMPAIGN_LEVELS - 1;
        if (unlocked > G.unlocked_level) G.unlocked_level = unlocked;
        profile_save();
    }
}

static void begin_life_loss(const char *reason, bool impact)
{
    if (G.state != GS_PLAYING && G.state != GS_PAUSED) return;
    sound_jet(false, 0);
    sound_play(SFX_HIT, 0.8f, 0.92f + visual_rand(414) * 0.18f);
    if (impact)
        burst(G.player.x + 6, G.player.y + 7, 0xfb7185, 32, 110,
              PARTICLE_SHARD);
    G.shake = impact ? 7.0f : 3.0f;
    G.flash = impact ? 0.8f : 0.35f;
    snprintf(G.death_reason, sizeof G.death_reason, "%s",
             reason && *reason ? reason : "SIGNAL LOSS");
    G.score = G.level_start_score;
    if (G.lives > 0) G.lives--;
    G.state_timer = 0;
    if (G.lives > 0) {
        G.state = GS_LIFE_LOST;
    } else {
        G.state = GS_GAMEOVER;
        update_high_score();
        sound_play(SFX_GAMEOVER, 0.75f, 1.0f);
        profile_save();
    }
}

static void kill_player(const char *reason)
{
    if (G.state != GS_PLAYING || G.player.invulnerable > 0) return;
    begin_life_loss(reason, true);
}

static void restart_current_life(void)
{
    begin_life_loss("MANUAL RECALL", false);
}

void game_force_level_clear(void)
{
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (G.level_data.tiles[y][x] == T_CRYSTAL)
            G.level_data.tiles[y][x] = T_CRYSTAL_EMPTY;
    G.crystals_remaining = 0;
    G.exit_open = true;
    complete_level();
}

void game_force_life_lost(void)
{
    G.player.invulnerable = 0;
    kill_player("TEST SIGNAL");
}

/* ---------- player mechanics ---------- */

static bool phase_cell_occupied(int tx, int ty)
{
    float x = tx * TILE_SIZE, y = ty * TILE_SIZE;
    if (overlap(G.player.x, G.player.y, PLAYER_W, PLAYER_H,
                x, y, TILE_SIZE, TILE_SIZE)) return true;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *enemy = &G.enemies[i];
        if (enemy->active && overlap(enemy->x, enemy->y, ENEMY_W, ENEMY_H,
                                     x, y, TILE_SIZE, TILE_SIZE)) return true;
    }
    return false;
}

static void update_phase_cells(void)
{
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        uint16_t *time = &G.phase_time[y][x];
        if (*time >= 100) {
            if (*time == 100 && phase_cell_occupied(x, y)) continue;
            (*time)--;
            if (*time == 99) *time = 0;
        } else if (*time > 0) {
            (*time)--;
        }
    }

    if (!G.player.phasing) return;
    int x0 = (int)floorf((G.player.x - 3) / TILE_SIZE);
    int x1 = (int)floorf((G.player.x + PLAYER_W + 3) / TILE_SIZE);
    int y0 = (int)floorf((G.player.y - 3) / TILE_SIZE);
    int y1 = (int)floorf((G.player.y + PLAYER_H + 3) / TILE_SIZE);
    bool working = false;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= FIELD_COLS || y < 0 || y >= FIELD_ROWS) continue;
        int tile = G.level_data.tiles[y][x];
        if (tile != T_PHASE_GLASS && tile != T_PHASE_DENSE) continue;
        uint16_t *time = &G.phase_time[y][x];
        if (*time >= 100) continue;
        int step = tile == T_PHASE_GLASS ? 4 : 2;
        int threshold = tile == T_PHASE_GLASS ? 28 : 64;
        *time = (uint16_t)(*time + step);
        working = true;
        if (*time >= threshold) {
            *time = 700;
            burst(x * TILE_SIZE + 8, y * TILE_SIZE + 8, 0xc084fc, 20, 65,
                  PARTICLE_SPARK);
        }
    }
    if (working && ((G.ticks % 12u) == 0u)) sound_play(SFX_PHASE, 0.24f, 1.0f);
}

static bool player_on_ladder(void)
{
    float cx = G.player.x + PLAYER_W * 0.5f;
    return tile_at(cx, G.player.y + 3) == T_LADDER ||
           tile_at(cx, G.player.y + PLAYER_H - 2) == T_LADDER;
}

static int surface_under_player(void)
{
    return tile_at(G.player.x + PLAYER_W * 0.5f, G.player.y + PLAYER_H + 0.5f);
}

static void move_player_axis(float amount, bool vertical)
{
    if (amount == 0) return;
    if (vertical) G.player.y += amount;
    else G.player.x += amount;
    if (!rect_hits_solid(G.player.x, G.player.y, PLAYER_W, PLAYER_H)) return;

    /* Subpixel bisection keeps landing edges stable without a tile-specific
     * resolver and works because no tick can cross a whole cell. */
    if (vertical) G.player.y -= amount;
    else G.player.x -= amount;
    float lo = 0, hi = 1;
    for (int i = 0; i < 8; i++) {
        float mid = (lo + hi) * 0.5f;
        if (vertical) G.player.y += amount * mid;
        else G.player.x += amount * mid;
        bool hit = rect_hits_solid(G.player.x, G.player.y, PLAYER_W, PLAYER_H);
        if (vertical) G.player.y -= amount * mid;
        else G.player.x -= amount * mid;
        if (hit) hi = mid; else lo = mid;
    }
    if (vertical) {
        G.player.y += amount * lo;
        if (amount > 0) G.player.grounded = true;
        G.player.vy = 0;
    } else {
        G.player.x += amount * lo;
        G.player.vx = 0;
    }
}

static void consume_tile(int tx, int ty)
{
    if (tx < 0 || tx >= FIELD_COLS || ty < 0 || ty >= FIELD_ROWS) return;
    int tile = G.level_data.tiles[ty][tx];
    switch (tile) {
    case T_CRYSTAL:
        G.level_data.tiles[ty][tx] = T_CRYSTAL_EMPTY;
        G.crystals_remaining--;
        G.score += 100;
        sound_play(SFX_CRYSTAL, 0.55f, 1.0f + (G.crystals_remaining % 4) * .08f);
        burst(tx * TILE_SIZE + 8, ty * TILE_SIZE + 8, 0x67e8f9, 18, 72,
              PARTICLE_SPARK);
        if (G.crystals_remaining <= 0) {
            G.crystals_remaining = 0;
            G.exit_open = true;
            sound_play(SFX_EXIT, 0.62f, 1.0f);
            set_banner("STARVAULT IRIS OPEN");
        }
        break;
    case T_FUEL:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.player.fuel = fminf(100.0f, G.player.fuel + 58.0f);
        G.score += 75;
        sound_play(SFX_PICKUP, 0.5f, .92f);
        set_banner("COMET CELL +58");
        break;
    case T_COIN:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.score += 150;
        sound_play(SFX_PICKUP, .46f, 1.12f);
        break;
    case T_RELIC:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.score += 400;
        sound_play(SFX_PICKUP, .58f, 1.32f);
        set_banner("STARVAULT RELIC +400");
        break;
    case T_LIFE:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.lives = G.lives < 9 ? G.lives + 1 : 9;
        G.score += 250;
        sound_play(SFX_LIFE, .65f, 1.0f);
        set_banner("NINE-LIFE SIGIL");
        break;
    case T_STUN:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.player.stunner = fmaxf(G.player.stunner, 10.0f);
        G.score += 100;
        sound_play(SFX_STUN, .55f, 1.0f);
        set_banner("EMP FIELD 10 SEC");
        break;
    case T_SHIELD:
        G.level_data.tiles[ty][tx] = T_EMPTY;
        G.player.shield = fmaxf(G.player.shield, 10.0f);
        G.score += 100;
        sound_play(SFX_PICKUP, .55f, 1.28f);
        set_banner("AEGIS FIELD 10 SEC");
        break;
    default:
        break;
    }
}

static void teleport_player(int tx, int ty, int tile)
{
    if (G.player.teleport_cooldown > 0) return;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        if ((x != tx || y != ty) && G.level_data.tiles[y][x] == tile) {
            burst(G.player.x + 6, G.player.y + 7, 0x22d3ee, 22, 82, PARTICLE_SPARK);
            G.player.x = x * TILE_SIZE + 2.0f;
            G.player.y = y * TILE_SIZE + 1.0f;
            G.player.vx *= .4f; G.player.vy = 0;
            G.player.teleport_cooldown = 1.0f;
            G.player.invulnerable = fmaxf(G.player.invulnerable, .45f);
            sound_play(SFX_TELEPORT, .62f, .92f + (tile - T_TELEPORT_CYAN) * .12f);
            burst(G.player.x + 6, G.player.y + 7, 0xc084fc, 22, 82, PARTICLE_SPARK);
            return;
        }
    }
}

static void process_player_cell(void)
{
    int tx = (int)((G.player.x + PLAYER_W * .5f) / TILE_SIZE);
    int ty = (int)((G.player.y + PLAYER_H * .5f) / TILE_SIZE);
    if (tx < 0 || tx >= FIELD_COLS || ty < 0 || ty >= FIELD_ROWS) return;
    consume_tile(tx, ty);
    int tile = G.level_data.tiles[ty][tx];
    if (tile == T_CHARGER) G.player.fuel = fminf(100.0f, G.player.fuel + 18.0f * TICK_DT);
    if (tile == T_DRAIN) G.player.fuel = fmaxf(0.0f, G.player.fuel - 14.0f * TICK_DT);
    if (tile == T_SPIKES && G.player.shield <= 0) kill_player("CRYSTAL THORNS");
    if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER)
        teleport_player(tx, ty, tile);
    if (tile >= T_SWITCH_CYAN && tile <= T_SWITCH_AMBER) {
        int family = tile - T_SWITCH_CYAN;
        if (!G.barriers_open[family]) {
            G.barriers_open[family] = true;
            G.score += 50;
            sound_play(SFX_SWITCH, .5f, 1.0f + family * .1f);
            set_banner(family == 0 ? "CIRCLE SHUTTERS OPEN" :
                       family == 1 ? "TRIANGLE SHUTTERS OPEN" :
                                     "SQUARE SHUTTERS OPEN");
        }
    }
    if (tile == T_EXIT && G.exit_open) complete_level();
}

static float active_axis(bool negative, bool positive)
{
    return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

static void update_player(void)
{
    bool left = G.held_controls ? G.held_left : G.left_latch > 0;
    bool right = G.held_controls ? G.held_right : G.right_latch > 0;
    bool up = G.held_controls ? G.held_up : G.up_latch > 0;
    bool down = G.held_controls ? G.held_down : G.down_latch > 0;
    bool jet = G.held_controls ? G.held_jet : G.jet_latch > 0;
    bool phase = G.held_controls ? G.held_phase : G.phase_latch > 0;
    float horizontal = active_axis(left, right);
    float vertical = active_axis(up, down);

    G.player.phasing = phase;
    G.player.phase_pulse += (phase ? 8.0f : 2.0f) * TICK_DT;
    G.player.on_ladder = player_on_ladder() && (vertical != 0 || G.player.on_ladder);
    G.player.grounded = false;

    int surface = surface_under_player();
    float max_speed = surface == T_MOSS ? 33.0f : 58.0f;
    float accel = G.player.on_ladder ? 170.0f : 120.0f;
    G.player.vx += horizontal * accel * TICK_DT;
    G.player.vx = clampf(G.player.vx, -max_speed, max_speed);
    if (horizontal != 0) G.player.facing = horizontal > 0 ? 1 : -1;

    if (G.player.on_ladder) {
        G.player.vy = vertical * 55.0f;
        if (horizontal != 0 && !player_on_ladder()) G.player.on_ladder = false;
    } else {
        G.player.vy += 168.0f * TICK_DT;
        if (jet && G.player.fuel > 0.0f) {
            G.player.vy -= 295.0f * TICK_DT;
            G.player.vy = fmaxf(G.player.vy, -92.0f);
            G.player.fuel = fmaxf(0.0f, G.player.fuel - 10.5f * TICK_DT);
            G.player.thrusting = true;
            if ((G.ticks & 1u) == 0u) {
                float px = G.player.x + (G.player.facing > 0 ? 2.0f : 9.0f);
                add_particle(px, G.player.y + 14, -G.player.vx * .08f,
                             30 + visual_rand((uint32_t)G.ticks) * 22, .28f, 1.6f,
                             0xfb923c, PARTICLE_THRUST);
            }
        } else G.player.thrusting = false;
        G.player.vy = fminf(G.player.vy, 105.0f);
    }
    sound_jet(G.player.thrusting, G.player.fuel / 100.0f);

    update_phase_cells();
    move_player_axis(G.player.vx * TICK_DT, false);
    move_player_axis(G.player.vy * TICK_DT, true);

    surface = surface_under_player();
    if (G.player.grounded || G.player.on_ladder) {
        float friction = surface == T_ICE ? .994f : surface == T_MOSS ? .82f : .87f;
        if (horizontal == 0) G.player.vx *= friction;
        if (surface == T_CONVEYOR_LEFT) G.player.vx -= 38.0f * TICK_DT;
        if (surface == T_CONVEYOR_RIGHT) G.player.vx += 38.0f * TICK_DT;
    } else if (horizontal == 0) G.player.vx *= .996f;

    /* A grounded reserve capacitor slowly restores enough charge to recover
     * from an empty tank.  It cannot be exploited in flight and stops at the
     * same minimum carried between vaults. */
    if (!G.player.thrusting && G.player.fuel < 18.0f &&
        (G.player.grounded || G.player.on_ladder))
        G.player.fuel = fminf(18.0f, G.player.fuel + 4.0f * TICK_DT);

    if (fabsf(G.player.vx) < .03f) G.player.vx = 0;
    bool walking = G.player.grounded && fabsf(G.player.vx) > 2.0f;
    bool climbing = G.player.on_ladder && fabsf(G.player.vy) > 2.0f;
    float gait_target = walking || climbing ? 1.0f : 0.0f;
    float gait_rate = gait_target > G.player.gait_amount ? 12.0f : 8.0f;
    G.player.gait_amount += (gait_target - G.player.gait_amount) *
                            fminf(1.0f, gait_rate * TICK_DT);
    if (walking)
        G.player.gait_phase += fabsf(G.player.vx) * .24f * TICK_DT;
    else if (climbing)
        G.player.gait_phase += fabsf(G.player.vy) * .20f * TICK_DT;
    G.player.animation = ((int)floorf(G.player.gait_phase * (2.0f / 3.14159265f))) & 3;
    process_player_cell();
}

/* ---------- enemies ---------- */

static bool enemy_hits_solid(const Enemy *e, float x, float y)
{
    (void)e;
    return rect_hits_solid(x, y, ENEMY_W, ENEMY_H);
}

static bool enemy_move(Enemy *e, float dx, float dy)
{
    bool hit = false;
    if (dx != 0) {
        e->x += dx;
        if (enemy_hits_solid(e, e->x, e->y)) { e->x -= dx; hit = true; }
    }
    if (dy != 0) {
        e->y += dy;
        if (enemy_hits_solid(e, e->x, e->y)) { e->y -= dy; hit = true; }
    }
    return hit;
}

static bool enemy_on_ground(const Enemy *e)
{
    return enemy_hits_solid(e, e->x, e->y + 1.0f);
}

static bool enemy_has_floor_ahead(const Enemy *e, float direction)
{
    float probe_x = direction >= 0 ? e->x + ENEMY_W + 1.0f : e->x - 1.0f;
    float probe_y = e->y + ENEMY_H + 2.0f;
    int tx = (int)floorf(probe_x / TILE_SIZE);
    int ty = (int)floorf(probe_y / TILE_SIZE);
    return game_tile_solid(tx, ty);
}

static void enemy_gravity_move(Enemy *e, float gravity)
{
    e->vy = fminf(e->vy + gravity * TICK_DT, 92.0f);
    if (enemy_move(e, e->vx * TICK_DT, 0)) e->vx = -e->vx;
    float dy = e->vy * TICK_DT;
    e->y += dy;
    if (enemy_hits_solid(e, e->x, e->y)) {
        e->y -= dy;
        if (e->vy > 0) e->timer = 0;
        e->vy = 0;
    }
}

static void update_enemy(Enemy *e, int index)
{
    if (!e->active) return;
    e->phase += TICK_DT * (2.0f + e->kind * .17f);
    e->timer -= TICK_DT;
    float px = G.player.x + PLAYER_W * .5f;
    float py = G.player.y + PLAYER_H * .5f;
    float ex = e->x + ENEMY_W * .5f;
    float ey = e->y + ENEMY_H * .5f;
    float wake_dx = px - ex, wake_dy = py - ey;
    float distance_squared = wake_dx * wake_dx + wake_dy * wake_dy;

    if (G.player.stunner > 0 && distance_squared < 38.0f * 38.0f)
        e->stun = fmaxf(e->stun, .18f);
    if (e->stun > 0) {
        e->stun -= TICK_DT;
        return;
    }

    /* Machines wake locally instead of converging from the entire vault.
     * The materialization tell is nonlethal and gives Kilix time to react. */
    if (e->alert <= 0.0f) {
        if (distance_squared >= 96.0f * 96.0f) return;
        e->alert = 3.0f;
        e->tell = .48f;
        e->vx *= .2f;
        e->vy *= .2f;
    } else if (distance_squared < 144.0f * 144.0f) {
        e->alert = 3.0f;
    } else {
        e->alert = fmaxf(0.0f, e->alert - TICK_DT);
        if (e->alert <= 0.0f) return;
    }
    if (e->tell > 0.0f) {
        e->tell = fmaxf(0.0f, e->tell - TICK_DT);
        return;
    }

    switch (e->kind) {
    case EN_TRACKER:
        e->vx += (px > ex ? 1 : -1) * 72.0f * TICK_DT;
        e->vx = clampf(e->vx, -34.0f, 34.0f);
        if (fabsf(px - ex) < 32 && py < ey - 18 && e->timer <= 0) {
            e->vy = -58.0f; e->timer = .7f;
        }
        enemy_gravity_move(e, 150.0f);
        break;
    case EN_ROLLER:
        if (fabsf(e->vx) < 12) e->vx = (index & 1) ? -42.0f : 42.0f;
        if (enemy_on_ground(e) && !enemy_has_floor_ahead(e, e->vx))
            e->vx = -e->vx;
        enemy_gravity_move(e, 175.0f);
        break;
    case EN_POGO:
        if (e->timer <= 0 && e->vy == 0) {
            e->vy = -72.0f;
            e->vx = clampf((px - ex) * .38f, -28.0f, 28.0f);
            e->timer = .55f;
        }
        enemy_gravity_move(e, 190.0f);
        break;
    case EN_DART:
        if (enemy_move(e, e->vx * TICK_DT, e->vy * TICK_DT)) {
            e->direction = (e->direction + 1 + (index & 1) * 2) & 3;
            static const float dx[4] = {58, 0, -58, 0};
            static const float dy[4] = {0, 58, 0, -58};
            e->vx = dx[e->direction]; e->vy = dy[e->direction];
        }
        break;
    case EN_SHARD:
        e->x += e->vx * TICK_DT;
        if (enemy_hits_solid(e, e->x, e->y)) { e->x -= e->vx * TICK_DT; e->vx = -e->vx; }
        e->y += e->vy * TICK_DT;
        if (enemy_hits_solid(e, e->x, e->y)) { e->y -= e->vy * TICK_DT; e->vy = -e->vy; }
        break;
    case EN_BLINKER:
        if (e->timer <= 0) {
            float angle = game_randf() * 6.2831853f;
            e->vx = cosf(angle) * 42.0f; e->vy = sinf(angle) * 42.0f;
            e->timer = .35f + game_randf() * .8f;
        }
        if (enemy_move(e, e->vx * TICK_DT, e->vy * TICK_DT)) e->timer = 0;
        break;
    case EN_WISP: {
        float dx = px - ex, dy = py - ey;
        float length = sqrtf(dx * dx + dy * dy) + .001f;
        e->vx += dx / length * 34.0f * TICK_DT;
        e->vy += dy / length * 34.0f * TICK_DT;
        float speed = sqrtf(e->vx * e->vx + e->vy * e->vy);
        if (speed > 34) { e->vx *= 34 / speed; e->vy *= 34 / speed; }
        e->x += e->vx * TICK_DT; e->y += e->vy * TICK_DT;
        e->x = clampf(e->x, 17, FIELD_W - 29); e->y = clampf(e->y, 1, FIELD_H - 29);
        break;
    }
    case EN_WING:
        e->vx += (px > ex ? 1 : -1) * 45.0f * TICK_DT;
        e->vx = clampf(e->vx, -42, 42);
        e->vy = sinf(e->phase * 2.1f) * 24.0f + clampf((py - ey) * .4f, -18, 18);
        if (enemy_move(e, e->vx * TICK_DT, e->vy * TICK_DT)) e->vx = -e->vx;
        break;
    }

    bool exposed = e->kind != EN_WISP ||
                   !enemy_hits_solid(e, e->x, e->y);
    if (exposed && overlap(G.player.x, G.player.y, PLAYER_W, PLAYER_H,
                           e->x, e->y, ENEMY_W, ENEMY_H)) {
        if (G.player.shield > 0 || G.player.stunner > 0) {
            e->stun = 1.1f;
            e->vx = (ex < px ? -55.0f : 55.0f);
            G.score += 5;
            if ((G.ticks % 18u) == 0u) sound_play(SFX_STUN, .22f, 1.1f);
        } else kill_player(enemy_name(e->kind));
    }
}

/* ---------- public lifecycle and tick ---------- */

void game_init(int width, int height, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    G.W = width; G.H = height;
    G.rng = seed ? seed : 0x6a09e667u;
    G.state = GS_TITLE;
    G.sound_on = true;
    G.unlocked_level = 0;
    G.selected_level = 0;
    G.help_return_state = GS_TITLE;
    profile_load();
}

void game_shutdown(void)
{
    update_high_score();
    profile_save();
}

static void decay_latches(void)
{
    float *values[] = {&G.left_latch, &G.right_latch, &G.up_latch,
                       &G.down_latch, &G.jet_latch, &G.phase_latch};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        *values[i] = fmaxf(0.0f, *values[i] - TICK_DT);
}

void game_tick(void)
{
    G.ticks++;
    G.scene_time += TICK_DT;
    G.state_timer += TICK_DT;
    if (G.flash > 0) G.flash = fmaxf(0.0f, G.flash - 1.8f * TICK_DT);
    if (G.shake > 0) G.shake = fmaxf(0.0f, G.shake - 15.0f * TICK_DT);
    if (G.banner_timer > 0) G.banner_timer -= TICK_DT;
    decay_latches();
    update_particles();

    if (G.state == GS_TITLE) {
        if ((G.ticks % 9u) == 0u && (G.ticks % 90u) < 36u)
            add_particle(145 + fmodf(G.scene_time * 47, 230), 220,
                         -7, -26, .55f, 1.5f, 0xfb923c, PARTICLE_THRUST);
        return;
    }
    if (G.state == GS_LEVEL_CLEAR && G.state_timer > 2.4f) {
        if (G.level + 1 >= CAMPAIGN_LEVELS) {
            G.state = GS_VICTORY; G.state_timer = 0;
            sound_play(SFX_VICTORY, .8f, 1.0f);
        } else game_load_level(G.level + 1, false);
        return;
    }
    if (G.state == GS_LIFE_LOST && G.state_timer > 1.65f) {
        float elapsed = G.level_time;
        game_load_level(G.level, true);
        G.level_time = elapsed;
        return;
    }
    if (G.state != GS_PLAYING) return;

    G.level_time += TICK_DT;
    if (G.player.invulnerable > 0) G.player.invulnerable -= TICK_DT;
    if (G.player.shield > 0) G.player.shield -= TICK_DT;
    if (G.player.stunner > 0) G.player.stunner -= TICK_DT;
    if (G.player.teleport_cooldown > 0) G.player.teleport_cooldown -= TICK_DT;
    update_player();
    if (G.state != GS_PLAYING) return;
    for (int i = 0; i < MAX_ENEMIES; i++) update_enemy(&G.enemies[i], i);
    if (G.player.y > FIELD_H + 24) kill_player("VOID FALL");
}

void game_set_held_controls(bool available, bool left, bool right,
                            bool up, bool down, bool jet, bool phase)
{
    G.held_controls = available;
    G.held_left = left; G.held_right = right;
    G.held_up = up; G.held_down = down;
    G.held_jet = jet; G.held_phase = phase;
}

static void set_press_latch(int key)
{
    const float duration = .30f;
    if (key == KEY_LEFT || key == 'a') G.left_latch = duration;
    if (key == KEY_RIGHT || key == 'd') G.right_latch = duration;
    if (key == KEY_UP || key == 'w') G.up_latch = duration;
    if (key == KEY_DOWN || key == 's') G.down_latch = duration;
    if (key == ' ' || key == 'z') G.jet_latch = duration;
    if (key == 'x' || key == 'e') G.phase_latch = duration;
}

void game_handle_key(int key)
{
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key == 'm') {
        G.sound_on = !G.sound_on;
        sound_set_enabled(G.sound_on);
        if (!G.headless) profile_save();
        return;
    }
    if (G.state == GS_TITLE) {
        if (key == KEY_UP || key == 'w') {
            G.menu_choice = (G.menu_choice + 3) % 4;
            sound_play(SFX_MENU, .35f, .9f);
        } else if (key == KEY_DOWN || key == 's') {
            G.menu_choice = (G.menu_choice + 1) % 4;
            sound_play(SFX_MENU, .35f, 1.05f);
        } else if (key == 'h') {
            open_help(GS_TITLE);
        } else if (key == KEY_ENTER || key == ' ') {
            sound_play(SFX_MENU, .5f, 1.18f);
            if (G.menu_choice == 0) {
                G.practice_mode = false;
                game_start(G.unlocked_level);
            }
            else if (G.menu_choice == 1) {
                G.selected_level = G.unlocked_level;
                G.state = GS_LEVEL_SELECT; G.state_timer = 0;
            } else if (G.menu_choice == 2) {
                open_help(GS_TITLE);
            } else G.quit = true;
        } else if (key == 'q' || key == KEY_ESC) G.quit = true;
        return;
    }
    if (G.state == GS_LEVEL_SELECT) {
        if (key == KEY_LEFT || key == 'a') G.selected_level--;
        if (key == KEY_RIGHT || key == 'd') G.selected_level++;
        if (key == KEY_UP || key == 'w') G.selected_level -= 10;
        if (key == KEY_DOWN || key == 's') G.selected_level += 10;
        G.selected_level = (int)clampf((float)G.selected_level, 0,
                                      (float)G.unlocked_level);
        if (key == KEY_ENTER || key == ' ') {
            G.practice_mode = false;
            game_start(G.selected_level);
        }
        if (key == KEY_ESC || key == 'q') return_to_title();
        return;
    }
    if (G.state == GS_HELP) {
        if (key == KEY_RIGHT || key == KEY_DOWN || key == 'd' || key == 's' ||
            key == KEY_ENTER || key == ' ') G.help_page = (G.help_page + 1) % 3;
        if (key == KEY_LEFT || key == KEY_UP || key == 'a' || key == 'w')
            G.help_page = (G.help_page + 2) % 3;
        if (key == KEY_ESC || key == 'q' || key == 'h') close_help();
        return;
    }
    if (G.state == GS_PLAYING) {
        if (key == KEY_ESC || key == 'p') {
            G.state = GS_PAUSED; G.state_timer = 0; sound_jet(false, 0); return;
        }
        if (key == 'h') { open_help(GS_PLAYING); return; }
        if (key == 'r') { restart_current_life(); return; }
        if (!G.held_controls) set_press_latch(key);
        return;
    }
    if (G.state == GS_PAUSED) {
        if (key == KEY_ESC || key == 'p' || key == KEY_ENTER) {
            G.state = GS_PLAYING; G.state_timer = 0;
        } else if (key == 'h') {
            open_help(GS_PAUSED);
        } else if (key == 'r') {
            restart_current_life();
        } else if (key == 'q') return_to_title();
        return;
    }
    if (G.state == GS_LEVEL_CLEAR) {
        if (key == KEY_ENTER || key == ' ') G.state_timer = 99;
        if (key == KEY_ESC) return_to_title();
        return;
    }
    if (G.state == GS_LIFE_LOST) {
        if (key == KEY_ENTER || key == ' ') G.state_timer = 99;
        if (key == KEY_ESC) return_to_title();
        return;
    }
    if (G.state == GS_GAMEOVER || G.state == GS_VICTORY) {
        if (key == KEY_ENTER || key == ' ' || key == KEY_ESC) return_to_title();
    }
}

void game_autopilot(void)
{
    if (G.state == GS_TITLE) { game_start(0); return; }
    if (G.state == GS_LIFE_LOST) { G.state_timer = 99; return; }
    if (G.state == GS_LEVEL_CLEAR) { G.state_timer = 99; return; }
    if (G.state != GS_PLAYING) return;
    float px = G.player.x + PLAYER_W * .5f;
    float py = G.player.y + PLAYER_H * .5f;
    float tx = G.level_data.exit_x * TILE_SIZE + 8.0f;
    float ty = G.level_data.exit_y * TILE_SIZE + 8.0f;
    float best = 1e30f;
    if (!G.exit_open) for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        if (G.level_data.tiles[y][x] != T_CRYSTAL) continue;
        float dx = x * TILE_SIZE + 8 - px, dy = y * TILE_SIZE + 8 - py;
        float distance = dx * dx + dy * dy;
        if (distance < best) { best = distance; tx = x * TILE_SIZE + 8; ty = y * TILE_SIZE + 8; }
    }
    G.scripted_input = true;
    G.held_controls = true;
    G.held_left = tx < px - 3; G.held_right = tx > px + 3;
    G.held_up = ty < py - 4; G.held_down = ty > py + 5;
    G.held_jet = ty < py - 12 || (G.player.vy > 65 && G.player.fuel > 4);
    G.held_phase = true;
}

bool game_validate(char *error, size_t error_len)
{
    if (G.state < GS_TITLE || G.state > GS_VICTORY) {
        snprintf(error, error_len, "invalid game state %d", G.state); return false;
    }
    if (G.level < 0 || G.level >= CAMPAIGN_LEVELS || G.selected_level < 0 ||
        G.selected_level >= CAMPAIGN_LEVELS || G.unlocked_level < 0 ||
        G.unlocked_level >= CAMPAIGN_LEVELS || G.lives < 0 || G.lives > 9 ||
        G.score < 0 || G.high_score < 0 || G.level_start_score < 0 ||
        G.crystals_remaining < 0 || G.crystals_remaining > FIELD_COLS * FIELD_ROWS) {
        snprintf(error, error_len, "invalid campaign counters level=%d lives=%d crystals=%d",
                 G.level, G.lives, G.crystals_remaining); return false;
    }
    if (G.level_data.theme < 0 || G.level_data.theme >= 10 ||
        !memchr(G.level_data.title, '\0', sizeof G.level_data.title) ||
        G.level_data.exit_x < 0 || G.level_data.exit_x >= FIELD_COLS ||
        G.level_data.exit_y < 0 || G.level_data.exit_y >= FIELD_ROWS ||
        G.level_data.tiles[G.level_data.exit_y][G.level_data.exit_x] != T_EXIT) {
        snprintf(error, error_len, "invalid loaded level metadata"); return false;
    }
    if (!isfinite(G.player.x) || !isfinite(G.player.y) ||
        !isfinite(G.player.vx) || !isfinite(G.player.vy) ||
        !isfinite(G.player.gait_phase) || !isfinite(G.player.gait_amount) ||
        !isfinite(G.player.invulnerable) || !isfinite(G.player.shield) ||
        !isfinite(G.player.stunner) || !isfinite(G.player.teleport_cooldown) ||
        G.player.fuel < -.01f || G.player.fuel > 100.01f ||
        G.player.x < -32.0f || G.player.x > FIELD_W + 32.0f ||
        G.player.y < -32.0f || G.player.y > FIELD_H + 48.0f) {
        snprintf(error, error_len, "invalid player state"); return false;
    }
    int counted = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (G.level_data.tiles[y][x] == T_CRYSTAL) counted++;
    if (counted != G.crystals_remaining) {
        snprintf(error, error_len, "crystal count %d != %d", counted, G.crystals_remaining);
        return false;
    }
    if (G.exit_open != (G.crystals_remaining == 0)) {
        snprintf(error, error_len, "iris state disagrees with remaining motes");
        return false;
    }
    if (G.state == GS_PLAYING &&
        rect_hits_solid(G.player.x, G.player.y, PLAYER_W, PLAYER_H)) {
        snprintf(error, error_len, "player is embedded in active collision");
        return false;
    }
    for (int i = 0; i < MAX_ENEMIES; i++) if (G.enemies[i].active) {
        Enemy *e = &G.enemies[i];
        if (e->kind < 0 || e->kind >= ENEMY_KIND_COUNT || !isfinite(e->x) ||
            !isfinite(e->y) || !isfinite(e->vx) || !isfinite(e->vy) ||
            !isfinite(e->timer) || !isfinite(e->stun) || !isfinite(e->phase) ||
            !isfinite(e->alert) || !isfinite(e->tell) ||
            e->alert < 0.0f || e->alert > 3.01f ||
            e->tell < 0.0f || e->tell > .51f ||
            e->x < -32.0f || e->x > FIELD_W + 32.0f ||
            e->y < -32.0f || e->y > FIELD_H + 32.0f) {
            snprintf(error, error_len, "invalid enemy %d", i); return false;
        }
        if (e->kind != EN_WISP && enemy_hits_solid(e, e->x, e->y)) {
            snprintf(error, error_len, "enemy %d is embedded in active collision", i);
            return false;
        }
    }
    for (int i = 0; i < MAX_PARTICLES; i++) if (G.particles[i].active) {
        Particle *p = &G.particles[i];
        if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->vx) ||
            !isfinite(p->vy) || !isfinite(p->life) || !isfinite(p->max_life) ||
            !isfinite(p->size) || p->max_life <= 0 || p->size < 0) {
            snprintf(error, error_len, "invalid particle %d", i); return false;
        }
    }
    return true;
}
