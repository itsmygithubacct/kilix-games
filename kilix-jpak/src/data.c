/* Deterministic, reviewable campaign generation and semantic data tables. */
#include "kilix_jpak.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *const tile_names[TILE_KIND_COUNT] = {
    "empty", "ceramic panel", "carbon panel", "vault stone", "magnetic rail",
    "star mote", "comet cell", "charge coil", "energy siphon", "sun token",
    "vault relic", "nine-life sigil", "EMP bell", "aegis capsule", "crystal thorns",
    "crystal glaze", "star lichen", "left flux belt", "right flux belt",
    "circle gate", "triangle gate", "square gate", "circle switch",
    "triangle switch", "square switch", "circle shutter", "triangle shutter",
    "square shutter", "quantum foam", "dense quantum foam", "basalt phase lock",
    "starvault iris", "empty star socket"
};

static const char *const enemy_names[ENEMY_KIND_COUNT] = {
    "Needle Hound", "Gravity Seed", "Coil Mite", "Compass Dart",
    "Shardling", "Spark Wisp", "Phase Eye", "Void Ray"
};

static const char *const chapter_names[10] = {
    "AURORA INTAKE", "COPPER VAULT", "LICHEN DECK", "PRISM WORKS",
    "MAGNETIC DEEP", "EMBER ARRAY", "VIOLET LOCK", "NULL GARDEN",
    "CROWN CIRCUIT", "STARVAULT CORE"
};

static const char *const stage_names[10] = {
    "FIRST LIGHT", "CROSSLINK", "SWITCHBACK", "FLOATING KEYS", "PHASE LESSON",
    "TWIN ASCENT", "BROKEN ORBIT", "SHUTTER TEST", "FALSE FLOOR", "DEEP GATE"
};

const char *tile_name(int tile)
{
    return tile >= 0 && tile < TILE_KIND_COUNT ? tile_names[tile] : "invalid";
}

const char *enemy_name(int kind)
{
    return kind >= 0 && kind < ENEMY_KIND_COUNT ? enemy_names[kind] : "invalid";
}

int level_enemy_budget(int level_index)
{
    if (level_index < 0) level_index = 0;
    if (level_index >= CAMPAIGN_LEVELS) level_index = CAMPAIGN_LEVELS - 1;
    int chapter = level_index / 10;
    int stage = level_index % 10 + 1;
    int count = 1 + chapter + stage / 2;
    return count > 15 ? 15 : count;
}

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

static int irand(uint32_t *state, int limit)
{
    *state = mix32(*state + 0x9e3779b9u);
    return limit > 0 ? (int)(*state % (uint32_t)limit) : 0;
}

static int platform_tile(int level, int row, int theme)
{
    int selector = (level * 3 + row + theme) % 13;
    if (level >= 8 && selector == 0) return T_ICE;
    if (level >= 12 && selector == 1) return T_MOSS;
    if (level >= 16 && selector == 2) return T_CONVEYOR_LEFT;
    if (level >= 18 && selector == 3) return T_CONVEYOR_RIGHT;
    if ((theme + row) % 4 == 0) return T_PANEL_DARK;
    return T_PANEL;
}

static void add_platform(LevelData *out, int level, int theme,
                         int x0, int x1, int y, int variation)
{
    if (y <= 0 || y >= FIELD_ROWS || x0 > x1) return;
    if (x0 < 1) x0 = 1;
    if (x1 > FIELD_COLS - 2) x1 = FIELD_COLS - 2;
    int tile = platform_tile(level, y + variation * 5, theme);
    for (int x = x0; x <= x1; x++) out->tiles[y][x] = (uint8_t)tile;
}

static void add_rail(LevelData *out, int x, int top, int bottom)
{
    if (x <= 0 || x >= FIELD_COLS - 1) return;
    if (top < 0) top = 0;
    if (bottom > FIELD_ROWS - 1) bottom = FIELD_ROWS - 1;
    for (int y = top; y < bottom; y++) out->tiles[y][x] = T_LADDER;
}

static void build_topology(LevelData *out, int level, int theme, int topology,
                           uint32_t *random)
{
    for (int y = 0; y < FIELD_ROWS; y++) {
        out->tiles[y][0] = T_STONE;
        out->tiles[y][FIELD_COLS - 1] = T_STONE;
    }
    add_platform(out, level, theme, 0, FIELD_COLS - 1, 15, 0);

    switch (topology) {
    case 0: { /* terraces: generous tutorial lanes */
        static const int rows[4] = {12, 9, 6, 3};
        for (int i = 0; i < 4; i++) {
            add_platform(out, level, theme, 1, 24, rows[i], i);
            int gap = 5 + irand(random, 15);
            out->tiles[rows[i]][gap] = T_EMPTY;
            out->tiles[rows[i]][gap + 1] = T_EMPTY;
        }
        add_rail(out, 5, 12, 15); add_rail(out, 20, 9, 12);
        add_rail(out, 7, 6, 9); add_rail(out, 19, 3, 6);
        break;
    }
    case 1: /* staggered gantries */
        add_platform(out, level, theme, 1, 9, 13, 0);
        add_platform(out, level, theme, 13, 24, 13, 1);
        add_platform(out, level, theme, 4, 17, 10, 2);
        add_platform(out, level, theme, 21, 24, 10, 3);
        add_platform(out, level, theme, 1, 7, 7, 1);
        add_platform(out, level, theme, 11, 22, 7, 2);
        add_platform(out, level, theme, 5, 15, 4, 3);
        add_platform(out, level, theme, 19, 24, 4, 0);
        add_rail(out, 5, 13, 15); add_rail(out, 15, 10, 13);
        add_rail(out, 5, 7, 10); add_rail(out, 13, 4, 7);
        break;
    case 2: /* alternating switchback */
        add_platform(out, level, theme, 1, 19, 13, 0);
        add_platform(out, level, theme, 6, 24, 10, 1);
        add_platform(out, level, theme, 1, 19, 7, 2);
        add_platform(out, level, theme, 6, 24, 4, 3);
        add_rail(out, 6, 13, 15); add_rail(out, 18, 10, 13);
        add_rail(out, 7, 7, 10); add_rail(out, 18, 4, 7);
        break;
    case 3: /* floating keys: many small landing choices */
        add_platform(out, level, theme, 2, 8, 12, 0);
        add_platform(out, level, theme, 12, 17, 12, 1);
        add_platform(out, level, theme, 20, 24, 12, 2);
        add_platform(out, level, theme, 5, 12, 9, 2);
        add_platform(out, level, theme, 16, 22, 9, 3);
        add_platform(out, level, theme, 1, 6, 6, 1);
        add_platform(out, level, theme, 10, 16, 6, 2);
        add_platform(out, level, theme, 20, 24, 6, 0);
        add_platform(out, level, theme, 5, 11, 3, 3);
        add_platform(out, level, theme, 15, 23, 3, 1);
        add_rail(out, 5, 12, 15); add_rail(out, 16, 9, 12);
        add_rail(out, 5, 6, 9); add_rail(out, 16, 3, 6);
        break;
    case 4: /* split decks around a changing central shaft */
        add_platform(out, level, theme, 1, 10, 12, 0);
        add_platform(out, level, theme, 14, 24, 12, 1);
        add_platform(out, level, theme, 1, 15, 9, 2);
        add_platform(out, level, theme, 19, 24, 9, 3);
        add_platform(out, level, theme, 1, 7, 6, 1);
        add_platform(out, level, theme, 11, 24, 6, 2);
        add_platform(out, level, theme, 1, 18, 3, 3);
        add_platform(out, level, theme, 22, 24, 3, 0);
        add_rail(out, 5, 12, 15); add_rail(out, 15, 9, 12);
        add_rail(out, 5, 6, 9); add_rail(out, 15, 3, 6);
        break;
    case 5: /* twin towers with cross-vault jumps */
        add_platform(out, level, theme, 1, 10, 12, 0);
        add_platform(out, level, theme, 15, 24, 13, 1);
        add_platform(out, level, theme, 1, 10, 8, 2);
        add_platform(out, level, theme, 15, 24, 9, 3);
        add_platform(out, level, theme, 3, 11, 4, 1);
        add_platform(out, level, theme, 16, 24, 4, 2);
        add_platform(out, level, theme, 10, 17, 2, 3);
        add_rail(out, 4, 12, 15); add_rail(out, 21, 13, 15);
        add_rail(out, 5, 8, 12); add_rail(out, 20, 9, 13);
        add_rail(out, 7, 4, 8); add_rail(out, 20, 4, 9);
        break;
    case 6: /* broken orbit: offset arcs and long air lanes */
        add_platform(out, level, theme, 1, 6, 13, 0);
        add_platform(out, level, theme, 10, 18, 13, 1);
        add_platform(out, level, theme, 22, 24, 13, 2);
        add_platform(out, level, theme, 4, 12, 10, 2);
        add_platform(out, level, theme, 16, 24, 10, 3);
        add_platform(out, level, theme, 1, 8, 7, 1);
        add_platform(out, level, theme, 12, 20, 7, 2);
        add_platform(out, level, theme, 4, 15, 4, 3);
        add_platform(out, level, theme, 19, 24, 4, 0);
        add_rail(out, 4, 13, 15); add_rail(out, 17, 10, 13);
        add_rail(out, 5, 7, 10); add_rail(out, 14, 4, 7);
        break;
    case 7: /* nested shutter halls */
        add_platform(out, level, theme, 1, 7, 13, 0);
        add_platform(out, level, theme, 10, 16, 13, 1);
        add_platform(out, level, theme, 19, 24, 13, 2);
        add_platform(out, level, theme, 3, 12, 10, 2);
        add_platform(out, level, theme, 15, 22, 10, 3);
        add_platform(out, level, theme, 1, 7, 7, 1);
        add_platform(out, level, theme, 10, 16, 7, 2);
        add_platform(out, level, theme, 19, 24, 7, 0);
        add_platform(out, level, theme, 3, 12, 4, 3);
        add_platform(out, level, theme, 15, 22, 4, 1);
        add_rail(out, 4, 13, 15); add_rail(out, 11, 10, 13);
        add_rail(out, 20, 7, 10); add_rail(out, 11, 4, 7);
        break;
    case 8: /* vertical relay with uneven altitude changes */
        add_platform(out, level, theme, 1, 12, 13, 0);
        add_platform(out, level, theme, 16, 24, 13, 1);
        add_platform(out, level, theme, 6, 19, 11, 2);
        add_platform(out, level, theme, 1, 9, 8, 3);
        add_platform(out, level, theme, 13, 24, 8, 1);
        add_platform(out, level, theme, 5, 20, 5, 2);
        add_platform(out, level, theme, 1, 8, 2, 3);
        add_platform(out, level, theme, 15, 24, 2, 0);
        add_rail(out, 6, 13, 15); add_rail(out, 17, 11, 13);
        add_rail(out, 7, 8, 11); add_rail(out, 18, 5, 8);
        add_rail(out, 7, 2, 5);
        break;
    default: /* starvault core: dense three-gap decks */
        add_platform(out, level, theme, 1, 24, 13, 0);
        add_platform(out, level, theme, 1, 24, 10, 1);
        add_platform(out, level, theme, 1, 24, 7, 2);
        add_platform(out, level, theme, 1, 24, 4, 3);
        for (int row = 13; row >= 4; row -= 3) {
            int shift = (row + level) % 3;
            out->tiles[row][6 + shift] = T_EMPTY;
            out->tiles[row][13 - shift] = T_EMPTY;
            out->tiles[row][20 + shift / 2] = T_EMPTY;
        }
        add_rail(out, 6, 13, 15); add_rail(out, 19, 10, 13);
        add_rail(out, 7, 7, 10); add_rail(out, 18, 4, 7);
        break;
    }
}

static bool permanent_support_kind(int tile)
{
    switch (tile) {
    case T_PANEL: case T_PANEL_DARK: case T_STONE:
    case T_ICE: case T_MOSS: case T_CONVEYOR_LEFT: case T_CONVEYOR_RIGHT:
        return true;
    default:
        return false;
    }
}

static bool near_rail(const LevelData *out, int x, int y, int radius)
{
    for (int yy = y - radius; yy <= y + radius; yy++) {
        for (int xx = x - 1; xx <= x + 1; xx++) {
            if (xx >= 0 && xx < FIELD_COLS && yy >= 0 && yy < FIELD_ROWS &&
                out->tiles[yy][xx] == T_LADDER) return true;
        }
    }
    return false;
}

static void mirror_topology(LevelData *out)
{
    for (int y = 0; y < FIELD_ROWS; y++) {
        for (int x = 1; x < FIELD_COLS / 2; x++) {
            int opposite = FIELD_COLS - 1 - x;
            uint8_t swap = out->tiles[y][x];
            out->tiles[y][x] = out->tiles[y][opposite];
            out->tiles[y][opposite] = swap;
        }
    }
}

static bool can_carve_gap(const LevelData *out, int x, int y)
{
    if (x < 2 || x > FIELD_COLS - 3 || y < 3 || y >= FIELD_ROWS - 1)
        return false;
    if (!permanent_support_kind(out->tiles[y][x]) ||
        !permanent_support_kind(out->tiles[y][x - 1]) ||
        !permanent_support_kind(out->tiles[y][x + 1]) ||
        out->tiles[y - 1][x] != T_EMPTY || near_rail(out, x, y, 2)) return false;
    return true;
}

static void carve_variant_gap(LevelData *out, uint32_t *random)
{
    int candidates[FIELD_ROWS * FIELD_COLS][2];
    int count = 0;
    for (int y = 3; y < FIELD_ROWS - 1; y++) {
        for (int x = 2; x < FIELD_COLS - 2; x++) {
            if (!can_carve_gap(out, x, y)) continue;
            candidates[count][0] = x;
            candidates[count++][1] = y;
        }
    }
    if (count == 0) return;
    int chosen = irand(random, count);
    out->tiles[candidates[chosen][1]][candidates[chosen][0]] = T_EMPTY;
}

static bool add_variant_ledge(LevelData *out, int level, int theme,
                              int variation, uint32_t *random)
{
    int length = 2 + (variation % 3);
    for (int attempt = 0; attempt < 96; attempt++) {
        int x = 2 + irand(random, FIELD_COLS - length - 3);
        int y = 2 + irand(random, FIELD_ROWS - 5);
        bool clear = true;
        for (int xx = x; xx < x + length; xx++) {
            if (out->tiles[y][xx] != T_EMPTY ||
                out->tiles[y - 1][xx] != T_EMPTY ||
                out->tiles[y + 1][xx] != T_EMPTY || near_rail(out, xx, y, 1)) {
                clear = false;
                break;
            }
        }
        if (!clear) continue;
        int tile = platform_tile(level, y + variation, theme);
        for (int xx = x; xx < x + length; xx++) out->tiles[y][xx] = (uint8_t)tile;
        return true;
    }
    return false;
}

static void vary_topology(LevelData *out, int level, int chapter, int stage,
                          uint32_t *random)
{
    if (chapter & 1) mirror_topology(out);
    int gaps = chapter == 0 ? 0 : 1 + chapter / 4;
    for (int i = 0; i < gaps; i++) carve_variant_gap(out, random);
    int ledges = chapter < 2 ? 0 : 1 + (chapter >= 7);
    for (int i = 0; i < ledges; i++)
        (void)add_variant_ledge(out, level, chapter, stage + chapter + i, random);
}

typedef struct {
    bool cell[FIELD_ROWS][FIELD_COLS];
} ReservationGrid;

static bool supported_cell(const LevelData *out, const ReservationGrid *reserved,
                           int x, int y)
{
    return x > 0 && x < FIELD_COLS - 1 && y > 0 && y < FIELD_ROWS - 1 &&
           out->tiles[y][x] == T_EMPTY &&
           permanent_support_kind(out->tiles[y + 1][x]) &&
           (!reserved || !reserved->cell[y][x]);
}

static bool choose_supported(const LevelData *out, const ReservationGrid *reserved,
                             int preferred_x, int preferred_y, uint32_t salt,
                             int *out_x, int *out_y)
{
    int best_score = 1000000, best_x = -1, best_y = -1;
    for (int y = 1; y < FIELD_ROWS - 1; y++) for (int x = 1; x < FIELD_COLS - 1; x++) {
        if (!supported_cell(out, reserved, x, y)) continue;
        if (x == out->spawn_x && y == out->spawn_y) continue;
        if (out->tiles[y][x] == T_EXIT) continue;
        int score = abs(y - preferred_y) * 48 + abs(x - preferred_x) * 3 +
                    (int)(mix32(salt ^ (uint32_t)(x * 31 + y * 131)) & 3u);
        if (score < best_score) {
            best_score = score; best_x = x; best_y = y;
        }
    }
    if (best_x < 0) return false;
    *out_x = best_x; *out_y = best_y;
    return true;
}

static bool place_supported(LevelData *out, const ReservationGrid *reserved,
                            int preferred_x, int preferred_y, int tile,
                            uint32_t salt, int *placed_x, int *placed_y)
{
    int x, y;
    if (!choose_supported(out, reserved, preferred_x, preferred_y, salt, &x, &y))
        return false;
    out->tiles[y][x] = (uint8_t)tile;
    if (placed_x) *placed_x = x;
    if (placed_y) *placed_y = y;
    return true;
}

static bool enemy_cell_used(const LevelData *out, int x, int y)
{
    for (int i = 0; i < out->enemy_count; i++)
        if (out->enemies[i].x == x && out->enemies[i].y == y) return true;
    return false;
}

static int nearest_enemy_distance(const LevelData *out, int x, int y)
{
    int nearest = FIELD_COLS + FIELD_ROWS;
    for (int i = 0; i < out->enemy_count; i++) {
        int distance = abs(x - (int)out->enemies[i].x) +
                       abs(y - (int)out->enemies[i].y);
        if (distance < nearest) nearest = distance;
    }
    return nearest;
}

static int adjacent_enemy_count(const LevelData *out, int x, int y)
{
    int count = 0;
    for (int i = 0; i < out->enemy_count; i++)
        if (abs(x - (int)out->enemies[i].x) +
            abs(y - (int)out->enemies[i].y) == 1) count++;
    return count;
}

static bool safe_enemy_pair(const LevelData *out, int x, int y)
{
    int adjacent = 0;
    for (int i = 0; i < out->enemy_count; i++) {
        int ex = out->enemies[i].x, ey = out->enemies[i].y;
        if (abs(x - ex) + abs(y - ey) != 1) continue;
        adjacent++;
        if (adjacent > 1 || adjacent_enemy_count(out, ex, ey) != 0) return false;
    }
    return true;
}

static bool same_deck_segment(const LevelData *out, int ax, int ay, int bx, int by)
{
    if (ay != by) return false;
    int x0 = ax < bx ? ax : bx;
    int x1 = ax > bx ? ax : bx;
    for (int x = x0; x <= x1; x++) {
        int support = out->tiles[ay + 1][x];
        if (!permanent_support_kind(support) && support != T_LADDER) return false;
    }
    return true;
}

static int enemy_deck_load(const LevelData *out, int x, int y)
{
    int load = 0;
    for (int i = 0; i < out->enemy_count; i++)
        if (same_deck_segment(out, x, y, out->enemies[i].x, out->enemies[i].y))
            load++;
    return load;
}

typedef struct {
    bool phase_open;
    unsigned barrier_mask;
    bool teleports;
    bool spikes_block;
} NavigationRules;

static bool navigation_passable(int tile, NavigationRules rules)
{
    if (permanent_support_kind(tile) || tile == T_PHASE_STEEL) return false;
    if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) return rules.phase_open;
    if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER)
        return (rules.barrier_mask & (1u << (tile - T_BARRIER_CYAN))) != 0;
    if (tile == T_SPIKES && rules.spikes_block) return false;
    return true;
}

static void navigation_flood(const LevelData *level, NavigationRules rules,
                             bool seen[FIELD_ROWS][FIELD_COLS])
{
    int qx[FIELD_ROWS * FIELD_COLS], qy[FIELD_ROWS * FIELD_COLS];
    int head = 0, tail = 0;
    memset(seen, 0, sizeof(bool) * FIELD_ROWS * FIELD_COLS);
    if (!navigation_passable(level->tiles[level->spawn_y][level->spawn_x], rules))
        return;
    qx[tail] = level->spawn_x;
    qy[tail++] = level->spawn_y;
    seen[level->spawn_y][level->spawn_x] = true;
    while (head < tail) {
        int x = qx[head], y = qy[head++];
        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || nx >= FIELD_COLS || ny < 0 || ny >= FIELD_ROWS ||
                seen[ny][nx] || !navigation_passable(level->tiles[ny][nx], rules))
                continue;
            seen[ny][nx] = true;
            qx[tail] = nx;
            qy[tail++] = ny;
        }
        if (!rules.teleports) continue;
        int tile = level->tiles[y][x];
        if (tile < T_TELEPORT_CYAN || tile > T_TELEPORT_AMBER) continue;
        for (int ty = 1; ty < FIELD_ROWS - 1; ty++) {
            for (int tx = 1; tx < FIELD_COLS - 1; tx++) {
                if (!seen[ty][tx] && level->tiles[ty][tx] == tile) {
                    seen[ty][tx] = true;
                    qx[tail] = tx;
                    qy[tail++] = ty;
                }
            }
        }
    }
}

static bool all_required_seen(const LevelData *level,
                              bool seen[FIELD_ROWS][FIELD_COLS])
{
    if (!seen[level->exit_y][level->exit_x]) return false;
    for (int y = 0; y < FIELD_ROWS; y++) {
        for (int x = 0; x < FIELD_COLS; x++) {
            if (level->tiles[y][x] == T_CRYSTAL && !seen[y][x]) return false;
        }
    }
    return true;
}

static int tile_count(const LevelData *out, int tile)
{
    int count = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (out->tiles[y][x] == tile) count++;
    return count;
}

typedef enum {
    ROOM_PHASE,
    ROOM_SHUTTER,
    ROOM_TELEPORT
} RoomKind;

static int room_change_cost(const LevelData *out, int floor_y, int side)
{
    int x0 = side > 0 ? 13 : 1;
    int x1 = side > 0 ? 24 : 12;
    int ceiling_y = floor_y - 3;
    int shaft_x = side > 0 ? 14 : 11;
    int cost = 0;
    for (int y = ceiling_y; y <= floor_y; y++) {
        for (int x = x0; x <= x1; x++) {
            int tile = out->tiles[y][x];
            if ((x == out->exit_x && y == out->exit_y) ||
                (x == out->spawn_x && y == out->spawn_y)) return 1000000;
            if (tile == T_EXIT) return 1000000;
            if (tile == T_LADDER) cost += 80;
            else if (y == ceiling_y || y == floor_y) {
                if (!permanent_support_kind(tile)) cost += 2;
            } else if (tile != T_EMPTY) cost += 8;
        }
    }
    for (int y = ceiling_y - 1; y <= floor_y + 1; y++) {
        if (y < 0 || y >= FIELD_ROWS) continue;
        if ((shaft_x == out->exit_x && y == out->exit_y) ||
            (shaft_x == out->spawn_x && y == out->spawn_y) ||
            out->tiles[y][shaft_x] == T_EXIT) return 1000000;
    }
    return cost;
}

static bool install_side_room(LevelData *out, ReservationGrid *reserved,
                              int level, int theme, RoomKind kind, int family,
                              int preferred_band, int preferred_side,
                              unsigned *used_bands)
{
    static const int floors[4] = {13, 10, 7, 4};
    int best_band = -1, best_side = 0, best_cost = 1000000;
    for (int side_pass = 0; side_pass < 2; side_pass++) {
        int side = side_pass == 0 ? preferred_side : -preferred_side;
        for (int step = 0; step < 4; step++) {
            int band = (preferred_band + step) & 3;
            if (*used_bands & (1u << band)) continue;
            int cost = room_change_cost(out, floors[band], side) + step * 3 + side_pass;
            if (cost < best_cost) {
                best_cost = cost;
                best_band = band;
                best_side = side;
            }
        }
    }
    if (best_band < 0 || best_cost >= 1000000) return false;

    int floor_y = floors[best_band], ceiling_y = floor_y - 3;
    int x0 = best_side > 0 ? 13 : 1;
    int x1 = best_side > 0 ? 24 : 12;
    int gate_x = best_side > 0 ? 20 : 5;
    int inside_x = best_side > 0 ? 23 : 2;
    int outside_x = best_side > 0 ? 16 : 9;
    int structure = platform_tile(level, floor_y + best_band, theme);

    for (int x = x0; x <= x1; x++) {
        out->tiles[ceiling_y][x] = (uint8_t)structure;
        out->tiles[floor_y][x] = (uint8_t)structure;
        for (int y = ceiling_y + 1; y < floor_y; y++) {
            out->tiles[y][x] = T_EMPTY;
            reserved->cell[y][x] = true;
        }
    }

    /* Every inserted room carries its own central-side rail.  This preserves
     * vertical circulation even when the room's new floor completes what
     * would otherwise become an accidental full-width seal. */
    int shaft_x = best_side > 0 ? 14 : 11;
    int shaft_top = ceiling_y > 0 ? ceiling_y - 1 : ceiling_y;
    int shaft_bottom = floor_y < FIELD_ROWS - 1 ? floor_y + 1 : floor_y;
    for (int y = shaft_top; y <= shaft_bottom; y++)
        out->tiles[y][shaft_x] = T_LADDER;

    int upper = ceiling_y + 1, lower = floor_y - 1;
    if (kind == ROOM_PHASE) {
        int phase_tile = family ? T_PHASE_DENSE : T_PHASE_GLASS;
        out->tiles[upper][gate_x] = (uint8_t)phase_tile;
        out->tiles[lower][gate_x] = (uint8_t)phase_tile;
    } else if (kind == ROOM_SHUTTER) {
        out->tiles[upper][gate_x] = (uint8_t)(T_BARRIER_CYAN + family);
        out->tiles[lower][gate_x] = (uint8_t)(T_BARRIER_CYAN + family);
        out->tiles[lower][outside_x] = (uint8_t)(T_SWITCH_CYAN + family);
    } else {
        out->tiles[upper][gate_x] = T_STONE;
        out->tiles[lower][gate_x] = T_STONE;
        out->tiles[lower][outside_x] = (uint8_t)(T_TELEPORT_CYAN + family);
        out->tiles[lower][inside_x] = (uint8_t)(T_TELEPORT_CYAN + family);
    }
    out->tiles[lower][inside_x + (best_side > 0 ? -2 : 2)] = T_CRYSTAL;
    *used_bands |= 1u << best_band;
    return true;
}

static bool place_spaced_crystal(LevelData *out, const ReservationGrid *reserved,
                                 int preferred_x, int preferred_y, uint32_t salt)
{
    int best_x = -1, best_y = -1, best_score = 1000000;
    for (int y = 1; y < FIELD_ROWS - 1; y++) for (int x = 1; x < FIELD_COLS - 1; x++) {
        if (!supported_cell(out, reserved, x, y)) continue;
        if (abs(x - out->spawn_x) + abs(y - out->spawn_y) < 3) continue;
        bool close = false;
        for (int yy = 1; yy < FIELD_ROWS - 1 && !close; yy++)
            for (int xx = 1; xx < FIELD_COLS - 1; xx++)
                if (out->tiles[yy][xx] == T_CRYSTAL && abs(x - xx) + abs(y - yy) < 4)
                    close = true;
        if (close) continue;
        int score = abs(y - preferred_y) * 52 + abs(x - preferred_x) * 4 +
                    (int)(mix32(salt ^ (uint32_t)(x * 47 + y * 193)) & 7u);
        if (score < best_score) {
            best_score = score;
            best_x = x;
            best_y = y;
        }
    }
    if (best_x < 0)
        return place_supported(out, reserved, preferred_x, preferred_y, T_CRYSTAL,
                               salt, NULL, NULL);
    out->tiles[best_y][best_x] = T_CRYSTAL;
    return true;
}

static bool near_special(const LevelData *out, int x, int y, int radius)
{
    for (int yy = y - radius; yy <= y + radius; yy++) {
        for (int xx = x - radius; xx <= x + radius; xx++) {
            if (xx < 0 || xx >= FIELD_COLS || yy < 0 || yy >= FIELD_ROWS ||
                abs(xx - x) + abs(yy - y) > radius) continue;
            int tile = out->tiles[yy][xx];
            if (tile == T_CRYSTAL || tile == T_EXIT || tile == T_FUEL ||
                tile == T_CHARGER || tile == T_SWITCH_CYAN ||
                tile == T_SWITCH_MAGENTA || tile == T_SWITCH_AMBER ||
                (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER)) return true;
        }
    }
    return false;
}

static bool place_safe_hazard(LevelData *out, const ReservationGrid *reserved,
                              int preferred_x, int preferred_y, uint32_t salt)
{
    bool rejected[FIELD_ROWS][FIELD_COLS] = {{false}};
    for (int attempt = 0; attempt < FIELD_ROWS * FIELD_COLS; attempt++) {
        int best_x = -1, best_y = -1, best_score = 1000000;
        for (int y = 1; y < FIELD_ROWS - 1; y++) for (int x = 1; x < FIELD_COLS - 1; x++) {
            if (rejected[y][x] || !supported_cell(out, reserved, x, y) ||
                near_rail(out, x, y, 2) || near_special(out, x, y, 3) ||
                abs(x - out->spawn_x) + abs(y - out->spawn_y) < 5 ||
                abs(x - out->exit_x) + abs(y - out->exit_y) < 3) continue;
            int score = abs(y - preferred_y) * 31 + abs(x - preferred_x) * 3 +
                        (int)(mix32(salt ^ (uint32_t)(x * 61 + y * 211)) & 7u);
            if (score < best_score) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
        if (best_x < 0) return false;
        out->tiles[best_y][best_x] = T_SPIKES;
        bool seen[FIELD_ROWS][FIELD_COLS];
        NavigationRules solved = {true, 7u, true, true};
        navigation_flood(out, solved, seen);
        if (all_required_seen(out, seen)) return true;
        out->tiles[best_y][best_x] = T_EMPTY;
        rejected[best_y][best_x] = true;
    }
    return false;
}

static bool choose_safe_enemy_cell(const LevelData *out,
                                   const ReservationGrid *reserved,
                                   int preferred_x, int preferred_y, uint32_t salt,
                                   int rail_radius, int pair_distance,
                                   int required_parity, bool paired_fallback,
                                   int *out_x, int *out_y)
{
    int best_x = -1, best_y = -1, best_score = 1000000;
    for (int y = 1; y < FIELD_ROWS - 1; y++) {
        for (int x = 1; x < FIELD_COLS - 1; x++) {
            int nearest = nearest_enemy_distance(out, x, y);
            if (!supported_cell(out, reserved, x, y) ||
                enemy_cell_used(out, x, y) ||
                (rail_radius >= 0 && near_rail(out, x, y, rail_radius)) ||
                (required_parity >= 0 && ((x + y) & 1) != required_parity) ||
                (paired_fallback && !safe_enemy_pair(out, x, y)) ||
                nearest < pair_distance || near_special(out, x, y, 3) ||
                abs(x - out->spawn_x) + abs(y - out->spawn_y) < 7 ||
                abs(x - out->exit_x) + abs(y - out->exit_y) < 3) continue;

            /* Prefer deck ends and isolated landings.  Besides improving the
             * encounter silhouette, preserving flexible cells makes the
             * greedy packing far less likely to strand a late budget slot. */
            int conflicts = 0;
            for (int yy = y - pair_distance + 1; yy <= y + pair_distance - 1; yy++) {
                for (int xx = x - pair_distance + 1;
                     xx <= x + pair_distance - 1; xx++) {
                    if (xx == x && yy == y) continue;
                    if (abs(xx - x) + abs(yy - y) >= pair_distance) continue;
                    if (supported_cell(out, reserved, xx, yy) &&
                        !near_special(out, xx, yy, 3)) conflicts++;
                }
            }

            int deck_load = enemy_deck_load(out, x, y);
            int spread_penalty = nearest < 10 ? (10 - nearest) * 5 : 0;
            int score = abs(y - preferred_y) * 37 +
                        abs(x - preferred_x) * 3 + deck_load * 64 +
                        conflicts * 48 + spread_penalty +
                        (int)(mix32(salt ^ (uint32_t)(x * 73 + y * 239)) & 7u);
            if (score < best_score) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
    }
    if (best_x < 0) return false;
    *out_x = best_x;
    *out_y = best_y;
    return true;
}

void level_build(int level_index, LevelData *out)
{
    if (!out) return;
    if (level_index < 0) level_index = 0;
    if (level_index >= CAMPAIGN_LEVELS) level_index = CAMPAIGN_LEVELS - 1;
    memset(out, 0, sizeof *out);

    const int chapter = level_index / 10;
    const int stage = level_index % 10 + 1;
    out->theme = chapter;
    snprintf(out->title, sizeof out->title, "%s / %s",
             chapter_names[chapter], stage_names[stage - 1]);
    out->spawn_x = -1;
    out->spawn_y = -1;
    out->exit_x = -1;
    out->exit_y = -1;

    uint32_t random = 0x4b1d5a77u ^ (uint32_t)(level_index + 1) * 0x45d9f3bu;
    build_topology(out, level_index, chapter, stage - 1, &random);
    vary_topology(out, level_index, chapter, stage - 1, &random);

    /* Entry and iris anchors alternate sides.  Chapter mutation changes the
     * exact safe landing while keeping a readable cross-vault objective. */
    int spawn_preference = (stage & 1) ? 2 + chapter % 4 : 23 - chapter % 4;
    if (!choose_supported(out, NULL, spawn_preference, 14, random ^ 0x3366ccu,
                          &out->spawn_x, &out->spawn_y)) {
        out->spawn_x = 2;
        out->spawn_y = 14;
    }
    int exit_preference = (stage & 1) ? 23 - (chapter * 2) % 5
                                      : 2 + (chapter * 2) % 5;
    if (!place_supported(out, NULL, exit_preference, 1, T_EXIT,
                         random ^ 0x55aa11u,
                         &out->exit_x, &out->exit_y)) {
        out->exit_x = 22; out->exit_y = 14;
        out->tiles[out->exit_y][out->exit_x] = T_EXIT;
    }

    ReservationGrid reserved = {{{false}}};
    unsigned used_bands = 0;
    int far_side = out->spawn_x < FIELD_COLS / 2 ? 1 : -1;
    bool phase_focus = stage == 5 ||
                       (chapter >= 2 && (stage + chapter) % 4 == 0) ||
                       (chapter >= 7 && stage == 10);
    bool shutter_focus = stage == 8 ||
                         (chapter >= 1 && (stage + chapter * 2) % 5 == 0) ||
                         (chapter >= 7 && stage == 10);
    bool teleport_focus = level_index >= 11 &&
                          (stage == 2 ||
                           (chapter >= 3 && (stage + chapter) % 5 == 1) ||
                           (chapter >= 8 && stage == 10));

    /* Focus mechanics own real side vaults.  Phase foam and shutters fill a
     * two-cell floor-to-ceiling doorway; teleporters link a sealed pocket.
     * Each room contains one required mote, making the mechanic purposeful. */
    if (phase_focus)
        (void)install_side_room(out, &reserved, level_index, chapter, ROOM_PHASE,
                                chapter >= 3 && ((stage + chapter) & 1),
                                (stage + chapter) & 3, far_side, &used_bands);
    if (shutter_focus)
        (void)install_side_room(out, &reserved, level_index, chapter, ROOM_SHUTTER,
                                (chapter + stage / 3) % 3,
                                (stage + chapter + 1) & 3, far_side, &used_bands);
    if (teleport_focus)
        (void)install_side_room(out, &reserved, level_index, chapter, ROOM_TELEPORT,
                                (chapter + stage - 1) % 3,
                                (stage + chapter + 2) & 3, -far_side, &used_bands);

    /* Remaining objectives are farthest-biased, altitude-spread landing
     * choices.  Reversing preferences with entry side avoids one canonical
     * collection sweep across the whole campaign. */
    static const int crystal_y[8] = {14, 11, 8, 5, 2, 12, 6, 3};
    static const int crystal_x[8] = {6, 19, 4, 21, 12, 15, 8, 18};
    int crystal_goal = 5 + stage / 4 + chapter / 5;
    if (crystal_goal > 8) crystal_goal = 8;
    for (int attempt = 0; tile_count(out, T_CRYSTAL) < crystal_goal && attempt < 32;
         attempt++) {
        int i = attempt & 7;
        int preferred_x = out->spawn_x < FIELD_COLS / 2
                        ? crystal_x[i] : FIELD_COLS - 1 - crystal_x[i];
        (void)place_spaced_crystal(out, &reserved, preferred_x, crystal_y[i],
                                   random ^ (uint32_t)(attempt * 0x1021u));
    }

    int travel_direction = out->exit_x > out->spawn_x ? 1 : -1;
    (void)place_supported(out, &reserved, out->spawn_x + travel_direction * 3,
                          out->spawn_y, T_FUEL, random ^ 0x1001u, NULL, NULL);
    (void)place_supported(out, &reserved, (out->spawn_x + out->exit_x) / 2,
                          (out->spawn_y + out->exit_y) / 2, T_CHARGER,
                          random ^ 0x1002u, NULL, NULL);
    if (level_index >= 7)
        (void)place_supported(out, &reserved,
                              out->spawn_x < FIELD_COLS / 2 ? 20 : 5, 8,
                              T_DRAIN, random ^ 0x1003u, NULL, NULL);
    (void)place_supported(out, &reserved, 9 + stage, 14, T_COIN,
                          random ^ 0x1004u, NULL, NULL);
    if (stage == 5 || stage == 10)
        (void)place_supported(out, &reserved, 4, 5, T_RELIC,
                              random ^ 0x1005u, NULL, NULL);
    if (level_index % 12 == 11)
        (void)place_supported(out, &reserved, 22, 9, T_LIFE,
                              random ^ 0x1006u, NULL, NULL);
    if (level_index >= 11 && stage % 4 == 2)
        (void)place_supported(out, &reserved, 17, 5, T_STUN,
                              random ^ 0x1007u, NULL, NULL);
    if (level_index >= 14 && stage % 5 == 3)
        (void)place_supported(out, &reserved, 8, 8, T_SHIELD,
                              random ^ 0x1008u, NULL, NULL);

    /* Basalt phase locks are scenery/route shaping only and never support an
     * object, enemy, or hazard. */
    if (chapter >= 3 && stage % 3 == 0)
        (void)place_supported(out, &reserved, 3 + chapter, 2, T_PHASE_STEEL,
                              random ^ 0x2003u, NULL, NULL);

    /* Hazards keep a safety radius around objectives, fixtures, rail mouths,
     * entry, and iris.  A candidate is committed only if all required cells
     * remain reachable while thorns are treated as forbidden. */
    int hazards = chapter / 2 + (stage >= 7);
    if (hazards > 5) hazards = 5;
    for (int i = 0; i < hazards; i++) {
        (void)place_safe_hazard(out, &reserved, 5 + irand(&random, 17),
                                crystal_y[(i + stage) % 8],
                                random ^ (uint32_t)(0x5001u + i));
    }

    int enemy_count = level_enemy_budget(level_index);
    int available_kinds = 1 + level_index / 8;
    if (available_kinds > ENEMY_KIND_COUNT) available_kinds = ENEMY_KIND_COUNT;
    static const struct {
        int rail_radius;
        int pair_distance;
        int parity;
        bool paired_fallback;
    } placement_passes[] = {
        /* Rebuild the complete set at each tier so an early greedy choice
         * cannot permanently poison a safer late-vault packing.  Checkerboard
         * tiers cheaply find dense nonadjacent sets; the final tiers permit
         * only isolated two-machine pairs, never chains or crowds. */
        {2, 3, -1, false}, {1, 3, -1, false},
        {0, 3, -1, false}, {-1, 3, -1, false},
        {2, 2, 0, false}, {2, 2, 1, false},
        {1, 2, 0, false}, {1, 2, 1, false},
        {0, 2, 0, false}, {0, 2, 1, false},
        {-1, 2, 0, false}, {-1, 2, 1, false},
        {2, 2, -1, false}, {1, 2, -1, false},
        {0, 2, -1, false}, {-1, 2, -1, false},
        {2, 1, -1, true}, {1, 1, -1, true},
        {0, 1, -1, true}, {-1, 1, -1, true}
    };
    ReservationGrid before_enemies = reserved;
    for (size_t pass = 0;
         pass < sizeof placement_passes / sizeof placement_passes[0]; pass++) {
        reserved = before_enemies;
        out->enemy_count = 0;
        for (int i = 0; i < enemy_count && out->enemy_count < MAX_ENEMIES; i++) {
            int x = 0, y = 0;
            int preferred_x = 6 + (stage * 5 + i * 7) % 16;
            int preferred_y = crystal_y[(i + stage / 2) % 8];
            if (!choose_safe_enemy_cell(out, &reserved, preferred_x, preferred_y,
                                        random ^ (uint32_t)(i * 97),
                                        placement_passes[pass].rail_radius,
                                        placement_passes[pass].pair_distance,
                                        placement_passes[pass].parity,
                                        placement_passes[pass].paired_fallback,
                                        &x, &y)) break;
            out->enemies[out->enemy_count++] = (EnemySpawn){
                (uint8_t)((stage + i * 3 - 1) % available_kinds),
                (uint8_t)x, (uint8_t)y
            };
            reserved.cell[y][x] = true;
        }
        if (out->enemy_count == enemy_count) break;
    }
}

static bool fixture_needs_support(int tile)
{
    return tile == T_CRYSTAL || tile == T_FUEL || tile == T_CHARGER ||
           tile == T_DRAIN || tile == T_COIN || tile == T_RELIC ||
           tile == T_LIFE || tile == T_STUN || tile == T_SHIELD ||
           tile == T_SPIKES || tile == T_EXIT ||
           (tile >= T_TELEPORT_CYAN && tile <= T_SWITCH_AMBER);
}

bool level_validate(const LevelData *level, char *error, size_t error_len)
{
    if (!level) {
        snprintf(error, error_len, "null level");
        return false;
    }
    if (level->theme < 0 || level->theme >= 10 ||
        !memchr(level->title, '\0', sizeof level->title)) {
        snprintf(error, error_len, "invalid theme or unterminated title");
        return false;
    }
    if (level->spawn_x < 1 || level->spawn_x >= FIELD_COLS - 1 ||
        level->spawn_y < 0 || level->spawn_y >= FIELD_ROWS - 1 ||
        level->exit_x < 1 || level->exit_x >= FIELD_COLS - 1 ||
        level->exit_y < 0 || level->exit_y >= FIELD_ROWS - 1) {
        snprintf(error, error_len, "spawn or exit is out of bounds");
        return false;
    }
    if (level->tiles[level->spawn_y][level->spawn_x] != T_EMPTY ||
        level->tiles[level->exit_y][level->exit_x] != T_EXIT) {
        snprintf(error, error_len, "spawn or iris anchor has the wrong tile");
        return false;
    }
    int crystals = 0, exits = 0;
    int teleport_count[3] = {0, 0, 0};
    int switch_count[3] = {0, 0, 0};
    int barrier_count[3] = {0, 0, 0};
    int phase_count = 0;
    for (int y = 0; y < FIELD_ROWS; y++) {
        for (int x = 0; x < FIELD_COLS; x++) {
            int tile = level->tiles[y][x];
            if (tile < 0 || tile >= TILE_KIND_COUNT) {
                snprintf(error, error_len, "tile %d,%d has invalid kind %d", x, y, tile);
                return false;
            }
            if (tile == T_CRYSTAL) crystals++;
            if (tile == T_EXIT) exits++;
            if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER)
                teleport_count[tile - T_TELEPORT_CYAN]++;
            if (tile >= T_SWITCH_CYAN && tile <= T_SWITCH_AMBER)
                switch_count[tile - T_SWITCH_CYAN]++;
            if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER)
                barrier_count[tile - T_BARRIER_CYAN]++;
            if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) phase_count++;
            if (fixture_needs_support(tile) &&
                (y + 1 >= FIELD_ROWS ||
                 !permanent_support_kind(level->tiles[y + 1][x]))) {
                snprintf(error, error_len, "%s at %d,%d lacks permanent support",
                         tile_name(tile), x, y);
                return false;
            }
            if (tile == T_SPIKES &&
                (near_rail(level, x, y, 2) || near_special(level, x, y, 3) ||
                 abs(x - level->spawn_x) + abs(y - level->spawn_y) < 5 ||
                 abs(x - level->exit_x) + abs(y - level->exit_y) < 3)) {
                snprintf(error, error_len, "thorns at %d,%d violate a safety radius",
                         x, y);
                return false;
            }
        }
    }
    if (crystals < 5 || crystals > 8 || exits != 1) {
        snprintf(error, error_len, "needs 5-8 crystals and exactly one exit");
        return false;
    }
    if (!permanent_support_kind(level->tiles[level->spawn_y + 1][level->spawn_x])) {
        snprintf(error, error_len, "spawn lacks permanent support");
        return false;
    }
    for (int family = 0; family < 3; family++) {
        if (teleport_count[family] != 0 && teleport_count[family] != 2) {
            snprintf(error, error_len, "gate family %d has %d endpoints",
                     family, teleport_count[family]);
            return false;
        }
        if ((barrier_count[family] == 0) != (switch_count[family] == 0) ||
            (barrier_count[family] != 0 &&
             (barrier_count[family] != 2 || switch_count[family] != 1))) {
            snprintf(error, error_len, "shutter family %d is not a 2+1 set", family);
            return false;
        }
    }
    if (phase_count != 0 && phase_count != 2) {
        snprintf(error, error_len, "phase doorway has %d cells", phase_count);
        return false;
    }
    if (level->enemy_count < 0 || level->enemy_count > MAX_ENEMIES) {
        snprintf(error, error_len, "invalid enemy count");
        return false;
    }
    for (int i = 0; i < level->enemy_count; i++) {
        const EnemySpawn *s = &level->enemies[i];
        if (s->kind >= ENEMY_KIND_COUNT || s->x >= FIELD_COLS ||
            s->y >= FIELD_ROWS - 1) {
            snprintf(error, error_len, "enemy %d is invalid", i);
            return false;
        }
        if (!permanent_support_kind(level->tiles[s->y + 1][s->x]) ||
            abs((int)s->x - level->spawn_x) + abs((int)s->y - level->spawn_y) < 7 ||
            near_special(level, s->x, s->y, 3)) {
            snprintf(error, error_len, "enemy %d has an unsafe landing", i);
            return false;
        }
        int adjacent = 0;
        for (int other = 0; other < level->enemy_count; other++) {
            if (other == i) continue;
            int separation = abs((int)level->enemies[other].x - (int)s->x) +
                             abs((int)level->enemies[other].y - (int)s->y);
            if (separation == 1) adjacent++;
        }
        if (adjacent > 1) {
            snprintf(error, error_len,
                     "enemy %d has %d immediately adjacent neighbors", i, adjacent);
            return false;
        }
        for (int prior = 0; prior < i; prior++) {
            int separation = abs((int)level->enemies[prior].x - (int)s->x) +
                             abs((int)level->enemies[prior].y - (int)s->y);
            if (separation == 0) {
                snprintf(error, error_len,
                         "enemies %d and %d overlap", prior, i);
                return false;
            }
        }
    }

    /* Solved-state navigation includes ring-gate edges and treats hazards as
     * forbidden.  Each special room must also prove that its own mechanic is
     * useful, rather than decorative or bypassable. */
    bool seen[FIELD_ROWS][FIELD_COLS];
    NavigationRules rules = {true, 7u, true, true};
    navigation_flood(level, rules, seen);
    if (!all_required_seen(level, seen)) {
        if (!seen[level->exit_y][level->exit_x])
            snprintf(error, error_len, "iris %d,%d lacks a hazard-free route",
                     level->exit_x, level->exit_y);
        else {
            int missing_x = -1, missing_y = -1;
            for (int y = 0; y < FIELD_ROWS && missing_x < 0; y++)
                for (int x = 0; x < FIELD_COLS; x++)
                    if (level->tiles[y][x] == T_CRYSTAL && !seen[y][x]) {
                        missing_x = x;
                        missing_y = y;
                        break;
                    }
            snprintf(error, error_len, "mote %d,%d lacks a hazard-free route",
                     missing_x, missing_y);
        }
        return false;
    }
    if (phase_count) {
        rules.phase_open = false;
        navigation_flood(level, rules, seen);
        if (all_required_seen(level, seen)) {
            snprintf(error, error_len, "phase doorway does not gate a target");
            return false;
        }
        rules.phase_open = true;
    }
    for (int family = 0; family < 3; family++) if (barrier_count[family]) {
        rules.barrier_mask = 7u & ~(1u << family);
        navigation_flood(level, rules, seen);
        bool switch_seen = false;
        for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
            if (level->tiles[y][x] == T_SWITCH_CYAN + family && seen[y][x])
                switch_seen = true;
        if (!switch_seen || all_required_seen(level, seen)) {
            snprintf(error, error_len, "shutter family %d lacks causal ordering", family);
            return false;
        }
    }
    rules.barrier_mask = 7u;
    for (int family = 0; family < 3; family++) if (teleport_count[family]) {
        rules.teleports = false;
        navigation_flood(level, rules, seen);
        if (all_required_seen(level, seen)) {
            snprintf(error, error_len, "gate family %d does not unlock a route", family);
            return false;
        }
        rules.teleports = true;
    }
    if (!level->title[0]) {
        snprintf(error, error_len, "title is empty");
        return false;
    }
    return true;
}

bool level_validate_campaign(char *error, size_t error_len)
{
    LevelData level;
    char titles[CAMPAIGN_LEVELS][40];
    for (int i = 0; i < CAMPAIGN_LEVELS; i++) {
        level_build(i, &level);
        if (level.enemy_count != level_enemy_budget(i)) {
            snprintf(error, error_len, "level %d: machine budget %d != %d",
                     i + 1, level.enemy_count, level_enemy_budget(i));
            return false;
        }
        if (!level_validate(&level, error, error_len)) {
            char detail[160];
            snprintf(detail, sizeof detail, "level %d: %s", i + 1, error);
            snprintf(error, error_len, "%s", detail);
            return false;
        }
        for (int j = 0; j < i; j++) if (!strcmp(titles[j], level.title)) {
            snprintf(error, error_len, "duplicate title: %s", level.title);
            return false;
        }
        snprintf(titles[i], sizeof titles[i], "%s", level.title);
    }
    return true;
}
