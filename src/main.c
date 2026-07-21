/* Entry point, fixed-step terminal loop, and deterministic validation modes. */
#include "kilix_jpak.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}

static void sleep_ms(double milliseconds)
{
    if (milliseconds <= 0) return;
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000.0);
    delay.tv_nsec = (long)((milliseconds - delay.tv_sec * 1000.0) * 1e6);
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) { }
}

static void on_signal(int signal_number)
{
    (void)signal_number;
    term_emergency_restore();
    _exit(1);
}

static void install_signals(void)
{
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGBUS,
                                  SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        signal(signals[i], on_signal);
}

static bool event_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static int game_key(const kittykb_event *event)
{
    static const char letters[] = "adehmpqrswxz";
    for (size_t i = 0; i < sizeof letters - 1; i++)
        if (event_letter(event, letters[i])) return letters[i];
    switch (event->key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    default: return event->key <= (uint32_t)INT_MAX ? (int)event->key : -1;
    }
}

static bool continuous_key(int key)
{
    return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
           key == 'a' || key == 'd' || key == 'w' || key == 's' ||
           key == ' ' || key == 'z' || key == 'x' || key == 'e';
}

static bool interrupt_event(const kittykb_event *event)
{
    return event->key == 3u ||
           (event_letter(event, 'c') && (event->modifiers & KITTYKB_MOD_CTRL));
}

static void headless_environment(void)
{
    setenv("KILIX_JPAK_NO_PROFILE", "1", 1);
    sound_set_enabled(false);
}

static int selftest(uint32_t seed, int ticks)
{
    headless_environment();
    char error[192];
    if (!level_validate_campaign(error, sizeof error)) {
        fprintf(stderr, "FAIL campaign: %s\n", error);
        return 1;
    }
    game_init(960, 600, seed);
    G.headless = true;
    G.sound_on = false;
    game_start(0);
    int deaths = 0;
    int previous_lives = G.lives;
    if (ticks <= 0) ticks = 12000;
    for (int i = 0; i < ticks; i++) {
        game_autopilot();
        game_tick();
        if (G.lives < previous_lives) { deaths++; previous_lives = G.lives; }
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL seed=%u tick=%d: %s\n", seed, i, error);
            game_shutdown(); return 1;
        }
        if (G.state == GS_GAMEOVER) game_start(G.level);
    }

    /* Every generated vault is loaded, simulated, validated, and cleared.
     * This catches bad entity spawns and campaign-end flow independently of
     * how lucky the free-running bot was. */
    for (int level = 0; level < CAMPAIGN_LEVELS; level++) {
        G.lives = 9;
        game_load_level(level, true);
        for (int i = 0; i < 90; i++) { game_autopilot(); game_tick(); }
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL level=%d: %s\n", level + 1, error);
            game_shutdown(); return 1;
        }
        if (G.state != GS_PLAYING) game_load_level(level, true);
        game_force_level_clear();
        if (G.state != GS_LEVEL_CLEAR) {
            fprintf(stderr, "FAIL level=%d did not clear\n", level + 1);
            game_shutdown(); return 1;
        }
    }
    printf("PASS selftest seed=%u ticks=%d levels=%d deaths=%d score=%d\n",
           seed, ticks, CAMPAIGN_LEVELS, deaths, G.score);
    game_shutdown();
    return 0;
}

static int failures;
#define EXPECT(condition, label) do { \
    if (condition) printf("PASS: %s\n", label); \
    else { printf("FAIL: %s\n", label); failures++; } \
} while (0)

static bool find_tile(int kind, int *out_x, int *out_y)
{
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++)
        if (G.level_data.tiles[y][x] == kind) {
            *out_x = x; *out_y = y; return true;
        }
    return false;
}

static bool topology_cell_solid(int tile)
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

static uint64_t topology_signature(const LevelData *level)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        int tile = level->tiles[y][x];
        uint8_t shape = tile == T_LADDER ? 2u : topology_cell_solid(tile) ? 1u : 0u;
        hash ^= shape;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int topology_difference(const LevelData *a, const LevelData *b)
{
    int difference = 0;
    for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
        int at = a->tiles[y][x], bt = b->tiles[y][x];
        int ashape = at == T_LADDER ? 2 : topology_cell_solid(at) ? 1 : 0;
        int bshape = bt == T_LADDER ? 2 : topology_cell_solid(bt) ? 1 : 0;
        if (ashape != bshape) difference++;
    }
    return difference;
}

static void place_player_in_cell(int x, int y)
{
    G.player.x = x * TILE_SIZE + 2;
    G.player.y = y * TILE_SIZE + 1;
    G.player.vx = G.player.vy = 0;
    G.player.invulnerable = 0;
}

static int rules_test(void)
{
    failures = 0;
    headless_environment();
    char error[192];
    EXPECT(level_validate_campaign(error, sizeof error), "all 100 clean-room levels validate");
    LevelData campaign[CAMPAIGN_LEVELS];
    uint64_t signatures[CAMPAIGN_LEVELS];
    int distinct_topologies = 0;
    int distinct_routes = 0;
    int route_keys[CAMPAIGN_LEVELS];
    int minimum_chapter_difference = FIELD_ROWS * FIELD_COLS;
    bool insertion_sides_alternate = true;
    bool deterministic_rebuilds = true;
    bool enemy_budgets_exact = true;
    bool content_budgets_exact = true;
    bool objectives_disperse = true;
    for (int level = 0; level < CAMPAIGN_LEVELS; level++) {
        LevelData rebuilt;
        level_build(level, &campaign[level]);
        level_build(level, &rebuilt);
        if (memcmp(&campaign[level], &rebuilt, sizeof rebuilt) != 0)
            deterministic_rebuilds = false;
        signatures[level] = topology_signature(&campaign[level]);
        bool duplicate = false;
        for (int prior = 0; prior < level; prior++)
            if (signatures[prior] == signatures[level]) duplicate = true;
        if (!duplicate) distinct_topologies++;
        int route_key = (campaign[level].spawn_y * FIELD_COLS + campaign[level].spawn_x) *
                        (FIELD_ROWS * FIELD_COLS) +
                        campaign[level].exit_y * FIELD_COLS + campaign[level].exit_x;
        bool duplicate_route = false;
        for (int prior = 0; prior < level; prior++)
            if (route_keys[prior] == route_key) duplicate_route = true;
        route_keys[level] = route_key;
        if (!duplicate_route) distinct_routes++;
        if (level < 10 && ((level & 1) ? campaign[level].spawn_x < FIELD_COLS / 2
                                      : campaign[level].spawn_x >= FIELD_COLS / 2))
            insertion_sides_alternate = false;
        if (level >= 10) {
            int difference = topology_difference(&campaign[level], &campaign[level - 10]);
            if (difference < minimum_chapter_difference) minimum_chapter_difference = difference;
        }
        int chapter = level / 10, stage = level % 10 + 1;
        int expected_enemies = level_enemy_budget(level);
        if (campaign[level].enemy_count != expected_enemies) enemy_budgets_exact = false;
        int crystals = 0, hazards = 0, phase_cells = 0;
        int barriers = 0, gates = 0;
        int crystal_x[8], crystal_y[8], crystal_entries = 0;
        unsigned altitude_bands = 0;
        for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
            int tile = campaign[level].tiles[y][x];
            if (tile == T_CRYSTAL) {
                crystals++;
                if (crystal_entries < 8) {
                    crystal_x[crystal_entries] = x;
                    crystal_y[crystal_entries++] = y;
                }
                altitude_bands |= 1u << (y / 3);
            }
            if (tile == T_SPIKES) hazards++;
            if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) phase_cells++;
            if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER) barriers++;
            if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER) gates++;
        }
        int expected_crystals = 5 + stage / 4 + chapter / 5;
        if (expected_crystals > 8) expected_crystals = 8;
        int expected_hazards = chapter / 2 + (stage >= 7);
        if (expected_hazards > 5) expected_hazards = 5;
        bool phase_focus = stage == 5 ||
                           (chapter >= 2 && (stage + chapter) % 4 == 0) ||
                           (chapter >= 7 && stage == 10);
        bool shutter_focus = stage == 8 ||
                             (chapter >= 1 && (stage + chapter * 2) % 5 == 0) ||
                             (chapter >= 7 && stage == 10);
        bool gate_focus = level >= 11 &&
                          (stage == 2 ||
                           (chapter >= 3 && (stage + chapter) % 5 == 1) ||
                           (chapter >= 8 && stage == 10));
        if (crystals != expected_crystals || hazards != expected_hazards ||
            phase_cells != (phase_focus ? 2 : 0) ||
            barriers != (shutter_focus ? 2 : 0) ||
            gates != (gate_focus ? 2 : 0)) content_budgets_exact = false;
        int band_count = 0;
        for (int band = 0; band < 6; band++)
            if (altitude_bands & (1u << band)) band_count++;
        if (band_count < 3 ||
            abs(campaign[level].spawn_x - campaign[level].exit_x) +
            abs(campaign[level].spawn_y - campaign[level].exit_y) < 12)
            objectives_disperse = false;
        for (int a = 0; a < crystal_entries; a++) for (int b = a + 1; b < crystal_entries; b++)
            if (abs(crystal_x[a] - crystal_x[b]) +
                abs(crystal_y[a] - crystal_y[b]) < 3) objectives_disperse = false;
    }
    EXPECT(deterministic_rebuilds, "campaign rebuilds byte-identically");
    EXPECT(distinct_topologies == CAMPAIGN_LEVELS,
           "all 100 vaults have distinct structural signatures");
    EXPECT(minimum_chapter_difference >= 12,
           "same-stage chapters differ by at least twelve structural cells");
    EXPECT(distinct_routes >= 30, "campaign uses at least thirty entry-to-iris routes");
    EXPECT(insertion_sides_alternate, "vault insertion alternates between approach sides");
    EXPECT(enemy_budgets_exact, "every vault receives its intended machine budget");
    EXPECT(level_enemy_budget(0) == 1 && level_enemy_budget(9) == 6 &&
           level_enemy_budget(90) == 10 && level_enemy_budget(99) == 15,
           "machine pressure hits independent early and late landmarks");
    EXPECT(content_budgets_exact,
           "every vault receives its intended objective, hazard, and mechanic budget");
    EXPECT(objectives_disperse,
           "motes span at least three altitudes with safe route separation");

    game_init(960, 600, 1337);
    G.headless = true; G.sound_on = false;
    game_start(0);
    int x = 0, y = 0;
    EXPECT(find_tile(T_CRYSTAL, &x, &y), "first vault contains a star mote");
    int before = G.crystals_remaining;
    place_player_in_cell(x, y); game_tick();
    EXPECT(G.crystals_remaining == before - 1 &&
           G.level_data.tiles[y][x] == T_CRYSTAL_EMPTY,
           "star mote collection updates the objective atomically");

    game_load_level(0, true);
    EXPECT(find_tile(T_FUEL, &x, &y), "first vault contains authored fuel");
    G.player.fuel = 2; place_player_in_cell(x, y); game_tick();
    EXPECT(G.player.fuel > 50 && G.level_data.tiles[y][x] == T_EMPTY,
           "comet cell restores fuel and is consumed");

    game_load_level(7, true);
    int switch_kind = -1;
    int barrier_x = -1, barrier_y = -1;
    for (int k = T_SWITCH_CYAN; k <= T_SWITCH_AMBER; k++)
        if (find_tile(k, &x, &y)) switch_kind = k;
    EXPECT(switch_kind >= 0, "shutter vault contains a shape-coded switch");
    if (switch_kind >= 0) {
        (void)find_tile(T_BARRIER_CYAN + switch_kind - T_SWITCH_CYAN,
                        &barrier_x, &barrier_y);
        EXPECT(barrier_x >= 0 && game_tile_solid(barrier_x, barrier_y),
               "authored shutter doorway begins physically closed");
        place_player_in_cell(x, y); game_tick();
        EXPECT(G.barriers_open[switch_kind - T_SWITCH_CYAN],
               "switch opens only its matching shutter family");
        EXPECT(barrier_x >= 0 && !game_tile_solid(barrier_x, barrier_y),
               "opened shutter doorway becomes traversable");
    }

    game_load_level(11, true);
    int gate_kind = -1;
    for (int k = T_TELEPORT_CYAN; k <= T_TELEPORT_AMBER; k++)
        if (find_tile(k, &x, &y)) gate_kind = k;
    EXPECT(gate_kind >= 0, "gate vault contains a paired ring gate");
    if (gate_kind >= 0) {
        int gate_x[2], gate_y[2], count = 0;
        for (int gy = 0; gy < FIELD_ROWS; gy++) for (int gx = 0; gx < FIELD_COLS; gx++)
            if (G.level_data.tiles[gy][gx] == gate_kind && count < 2) {
                gate_x[count] = gx; gate_y[count++] = gy;
            }
        float old_x = gate_x[0] * TILE_SIZE + 2;
        place_player_in_cell(gate_x[0], gate_y[0]); game_tick();
        EXPECT(fabsf(G.player.x - old_x) > TILE_SIZE && G.player.teleport_cooldown > 0,
               "ring gate moves Kilix to its same-glyph partner");
        G.player.teleport_cooldown = 0;
        place_player_in_cell(gate_x[1], gate_y[1]); game_tick();
        EXPECT(fabsf(G.player.x - old_x) < 1.0f,
               "ring gate pair provides a collision-safe return trip");
    }

    game_load_level(4, true);
    EXPECT(find_tile(T_PHASE_GLASS, &x, &y), "phase tutorial contains quantum foam");
    if (find_tile(T_PHASE_GLASS, &x, &y)) {
        G.player.x = x * TILE_SIZE - 10; G.player.y = y * TILE_SIZE + 1;
        G.player.vx = G.player.vy = 0;
        game_set_held_controls(true, false, false, false, false, false, true);
        for (int i = 0; i < 35; i++) game_tick();
        EXPECT(G.phase_time[y][x] >= 100 && !game_tile_solid(x, y),
               "held phase erodes nearby foam into a temporary passage");
        G.phase_time[y][x] = 100;
        place_player_in_cell(x, y);
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
        EXPECT(G.phase_time[y][x] >= 100 && !game_tile_solid(x, y),
               "phase doorway waits for an occupant before restoring");
        place_player_in_cell(G.level_data.spawn_x, G.level_data.spawn_y);
        game_tick();
        EXPECT(G.phase_time[y][x] == 0 && game_tile_solid(x, y),
               "vacated phase doorway restores collision safely");
    }

    game_load_level(0, true);
    game_force_level_clear();
    EXPECT(G.state == GS_LEVEL_CLEAR && G.unlocked_level >= 1,
           "securing a vault advances persistent unlock progress");
    EXPECT(G.clear_time_bonus == 2400 && G.clear_fuel_bonus == 34,
           "vault-clear bonus records its time and fuel components");

    game_load_level(1, true);
    int unlocked_before_practice = G.unlocked_level;
    G.practice_mode = true;
    game_force_level_clear();
    EXPECT(G.unlocked_level == unlocked_before_practice,
           "forced-level practice cannot mutate campaign unlocks");
    G.practice_mode = false;

    game_load_level(0, true);
    int lives = G.lives;
    game_force_life_lost();
    EXPECT(G.state == GS_LIFE_LOST && G.lives == lives - 1,
           "unshielded contact spends exactly one life");

    EXPECT(game_validate(error, sizeof error), "post-fixture game state validates");
    game_shutdown();

    /* The real persistence path gets an isolated round trip.  This exercises
     * permissions, checksum validation, and the nonmutating corrupt-file
     * fallback without touching the player's profile. */
    char temporary[] = "/tmp/kilix-jpak-profile-XXXXXX";
    char *profile_root = mkdtemp(temporary);
    EXPECT(profile_root != NULL, "isolated profile directory is created");
    if (profile_root) {
        unsetenv("KILIX_JPAK_NO_PROFILE");
        setenv("KILIX_JPAK_DATA_HOME", profile_root, 1);
        game_init(960, 600, 77);
        G.high_score = 543210;
        G.unlocked_level = 37;
        G.sound_on = false;
        game_shutdown();

        char profile_path[1024], app_directory[512];
        snprintf(app_directory, sizeof app_directory, "%s/kilix-jpak", profile_root);
        snprintf(profile_path, sizeof profile_path, "%s/profile.v1", app_directory);
        struct stat status;
        EXPECT(stat(profile_path, &status) == 0 && (status.st_mode & 0777) == 0600,
               "profile is atomically created with private permissions");

        game_init(960, 600, 88);
        EXPECT(G.high_score == 543210 && G.unlocked_level == 37 && !G.sound_on,
               "versioned profile round-trips score, unlock, and sound");
        G.headless = true;
        game_shutdown();

        int profile_fd = open(profile_path, O_WRONLY | O_TRUNC);
        if (profile_fd >= 0) {
            (void)write(profile_fd, "bad", 3);
            close(profile_fd);
        }
        game_init(960, 600, 99);
        EXPECT(G.high_score == 0 && G.unlocked_level == 0 && G.sound_on,
               "truncated profile is rejected without partial state");
        G.headless = true;
        game_shutdown();
        unlink(profile_path);
        rmdir(app_directory);
        rmdir(profile_root);
        unsetenv("KILIX_JPAK_DATA_HOME");
        setenv("KILIX_JPAK_NO_PROFILE", "1", 1);
    }
    return failures ? 1 : 0;
}

static int input_test(void)
{
    failures = 0;
    headless_environment();
    game_init(960, 600, 42);
    G.headless = true; G.sound_on = false;
    game_start(0);
    G.player.invulnerable = 0;
    float before = G.player.x;
    game_set_held_controls(true, false, true, false, false, false, false);
    for (int i = 0; i < 5; i++) game_tick();
    EXPECT(G.player.x > before, "held right produces continuous movement");
    EXPECT(G.player.gait_amount > 0 && G.player.gait_phase > 0,
           "ground movement advances the visible walking gait");

    before = G.player.vx;
    game_set_held_controls(true, true, true, false, false, false, false);
    game_tick();
    EXPECT(G.player.vx <= before + .01f, "simultaneous left and right cancel acceleration");

    float fuel = G.player.fuel;
    game_set_held_controls(true, false, false, false, false, true, false);
    for (int i = 0; i < 4; i++) game_tick();
    EXPECT(G.player.thrusting && G.player.fuel < fuel, "held jet consumes fuel and lifts");

    game_set_held_controls(false, false, false, false, false, false, false);
    game_handle_key('d'); before = G.player.x; game_tick();
    EXPECT(G.player.x > before, "press-only fallback retains movement intent");
    for (int i = 0; i < 20; i++) game_tick();
    EXPECT(G.right_latch == 0, "press-only movement intent expires");

    game_load_level(0, true);
    game_set_held_controls(true, false, false, false, false, false, false);
    G.player.fuel = 0;
    for (int i = 0; i < 120; i++) game_tick();
    EXPECT(G.player.fuel > 7.5f && G.player.fuel <= 18.01f,
           "grounded reserve recovers an empty fuel tank");

    Enemy *warning_enemy = &G.enemies[0];
    warning_enemy->x = G.player.x + 70.0f;
    warning_enemy->y = G.player.y + 2.0f;
    warning_enemy->home_x = warning_enemy->x;
    warning_enemy->home_y = warning_enemy->y;
    warning_enemy->vx = warning_enemy->vy = 0;
    warning_enemy->alert = warning_enemy->tell = 0;
    G.player.invulnerable = 0;
    float warning_x = warning_enemy->x;
    game_tick();
    EXPECT(warning_enemy->alert > 0 && warning_enemy->tell > .4f &&
           fabsf(warning_enemy->x - warning_x) < .01f,
           "nearby dormant machine telegraphs before moving");
    G.player.x = warning_enemy->x;
    G.player.y = warning_enemy->y;
    game_tick();
    EXPECT(G.state == GS_PLAYING,
           "machine activation tell is nonlethal even on contact");
    game_load_level(0, true);

    game_handle_key('h');
    EXPECT(G.state == GS_HELP && G.help_return_state == GS_PLAYING,
           "gameplay H opens the field manual without losing context");
    game_handle_key('h');
    EXPECT(G.state == GS_PLAYING, "closing the field manual resumes gameplay");
    game_handle_key('p');
    game_handle_key('h');
    EXPECT(G.state == GS_HELP && G.help_return_state == GS_PAUSED,
           "pause menu opens the field manual with a paused return state");
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_PAUSED, "closing pause help returns to the pause menu");
    game_handle_key('p');

    G.score = 900;
    game_load_level(0, true);
    G.score += 350;
    G.level_time = 12.0f;
    G.player.invulnerable = 1.0f;
    int lives_before_restart = G.lives;
    game_handle_key('r');
    EXPECT(G.state == GS_LIFE_LOST && G.lives == lives_before_restart - 1,
           "manual restart consistently spends one life despite spawn grace");
    EXPECT(G.score == 900, "retry rolls unbanked level score back to deployment");
    G.state_timer = 99;
    game_tick();
    EXPECT(G.state == GS_PLAYING && G.level_time >= 12.0f,
           "retry preserves elapsed vault time instead of resetting its bonus");

    G.state = GS_LEVEL_SELECT;
    G.unlocked_level = 99;
    G.selected_level = 55;
    game_handle_key(KEY_UP);
    EXPECT(G.selected_level == 45, "selector Up follows the displayed grid upward");
    game_handle_key(KEY_DOWN);
    EXPECT(G.selected_level == 55, "selector Down follows the displayed grid downward");

    G.state = GS_TITLE;
    G.unlocked_level = 37;
    G.menu_choice = 0;
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING && G.level == 37 && !G.practice_mode,
           "continue campaign resumes the highest unlocked vault");
    game_shutdown();
    return failures ? 1 : 0;
}

static bool ensure_directory(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static int write_scene(const char *directory, const char *name)
{
    char path[768];
    if (directory && *directory)
        snprintf(path, sizeof path, "%s/render_%s.ppm", directory, name);
    else snprintf(path, sizeof path, "render_%s.ppm", name);
    GameState before = G;
    render_frame();
    if (memcmp(&before, &G, sizeof G) != 0) {
        fprintf(stderr, "renderer mutated game state in scene %s\n", name);
        return 1;
    }
    if (!render_dump_ppm(path)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    printf("wrote %s\n", path);
    return 0;
}

static int render_test(uint32_t seed)
{
    headless_environment();
    const char *directory = getenv("KILIX_JPAK_RENDER_DIR");
    if (directory && *directory && !ensure_directory(directory)) {
        fprintf(stderr, "cannot create render directory: %s\n", directory); return 1;
    }
    game_init(960, 600, seed); G.headless = true; G.sound_on = false;
    if (!render_init(G.W, G.H)) return 1;
    int failed = 0, images = 0;

    failed |= write_scene(directory, "title"); images++;
    G.state = GS_LEVEL_SELECT; G.unlocked_level = 67; G.selected_level = 42;
    failed |= write_scene(directory, "level_select"); images++;
    for (int page = 0; page < 3; page++) {
        char name[32]; snprintf(name, sizeof name, "help_%d", page + 1);
        G.state = GS_HELP; G.help_page = page;
        failed |= write_scene(directory, name); images++;
    }
    static const struct { int level; const char *name; } showcases[] = {
        {23, "topology_floating_keys"},
        {55, "topology_twin_ascent"},
        {99, "topology_deep_gate"},
        {4, "mechanic_phase_vault"},
        {7, "mechanic_shutter_vault"},
        {11, "mechanic_gate_vault"},
        {2, "variant_aurora_switchback"},
        {42, "variant_magnetic_switchback"},
        {92, "variant_core_switchback"}
    };
    for (size_t i = 0; i < sizeof showcases / sizeof showcases[0]; i++) {
        game_start(showcases[i].level);
        G.banner_timer = 0;
        G.flash = 0;
        failed |= write_scene(directory, showcases[i].name); images++;
    }
    game_start(0);
    G.banner_timer = 0;
    G.flash = 0;
    G.player.invulnerable = 0;
    G.player.grounded = true;
    G.player.vx = 48;
    G.player.gait_amount = 1;
    G.player.gait_phase = 1.5707963f;
    failed |= write_scene(directory, "walk_stride_a"); images++;
    G.player.gait_phase = 4.7123890f;
    failed |= write_scene(directory, "walk_stride_b"); images++;
    game_start(68);
    for (int i = 0; i < 240; i++) { game_autopilot(); game_tick(); }
    failed |= write_scene(directory, "playing"); images++;
    G.state = GS_PAUSED; failed |= write_scene(directory, "paused"); images++;
    G.state = GS_PLAYING; game_force_level_clear();
    failed |= write_scene(directory, "clear"); images++;
    game_load_level(38, true); game_force_life_lost();
    failed |= write_scene(directory, "life_lost"); images++;
    G.state = GS_GAMEOVER; G.score = 123456;
    failed |= write_scene(directory, "gameover"); images++;
    G.state = GS_VICTORY; G.score = 987654; G.flash = 0;
    failed |= write_scene(directory, "victory"); images++;

    render_shutdown(); game_shutdown();
    if (!failed) printf("PASS render-test seed=%u images=%d\n", seed, images);
    return failed ? 1 : 0;
}

static int sound_test(void)
{
    static const char *const names[SFX_COUNT] = {
        "menu", "jet loop", "crystal", "pickup", "phase", "teleport",
        "switch", "exit", "hit", "stun", "clear", "life", "game over", "victory"
    };
    if (!sound_init()) {
        printf("sound-test: no audio sink; silent fallback is operational\n");
        return 0;
    }
    sound_set_enabled(true);
    for (int i = 0; i < SFX_COUNT; i++) {
        printf("%02d %s\n", i, names[i]); fflush(stdout);
        if (i == SFX_JET) { sound_jet(true, .7f); sleep_ms(550); sound_jet(false, 0); }
        else { sound_play(i, .58f, 1.0f); sleep_ms(i >= SFX_CLEAR ? 1050 : 520); }
    }
    sound_shutdown();
    return 0;
}

static int dump_campaign(void)
{
    char error[160];
    if (!level_validate_campaign(error, sizeof error)) {
        fprintf(stderr, "%s\n", error); return 1;
    }
    printf("level\ttheme\tmotes\thazards\tenemies\tentry\tiris\tsystems\ttitle\n");
    for (int i = 0; i < CAMPAIGN_LEVELS; i++) {
        LevelData level; int crystals = 0, hazards = 0;
        bool phase = false, shutter = false, gate = false;
        level_build(i, &level);
        for (int y = 0; y < FIELD_ROWS; y++) for (int x = 0; x < FIELD_COLS; x++) {
            int tile = level.tiles[y][x];
            if (tile == T_CRYSTAL) crystals++;
            if (tile == T_SPIKES) hazards++;
            if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) phase = true;
            if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER) shutter = true;
            if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER) gate = true;
        }
        char systems[32] = "open";
        if (phase || shutter || gate) {
            systems[0] = '\0';
            if (phase) strcat(systems, "phase+");
            if (shutter) strcat(systems, "shutter+");
            if (gate) strcat(systems, "gate+");
            systems[strlen(systems) - 1] = '\0';
        }
        printf("%03d\t%d\t%d\t%d\t%d\t%d,%d\t%d,%d\t%s\t%s\n",
               i + 1, level.theme, crystals, hazards, level.enemy_count,
               level.spawn_x, level.spawn_y, level.exit_x, level.exit_y,
               systems, level.title);
    }
    return 0;
}

static char dump_cell(const LevelData *level, int x, int y)
{
    if (x == level->spawn_x && y == level->spawn_y) return '@';
    for (int i = 0; i < level->enemy_count; i++)
        if (level->enemies[i].x == x && level->enemies[i].y == y) return '!';
    int tile = level->tiles[y][x];
    if (tile == T_EMPTY) return ' ';
    if (tile == T_PANEL || tile == T_PANEL_DARK || tile == T_STONE ||
        tile == T_ICE || tile == T_MOSS || tile == T_CONVEYOR_LEFT ||
        tile == T_CONVEYOR_RIGHT || tile == T_PHASE_STEEL) return '#';
    if (tile == T_LADDER) return 'H';
    if (tile == T_CRYSTAL) return '*';
    if (tile == T_EXIT) return 'I';
    if (tile == T_PHASE_GLASS || tile == T_PHASE_DENSE) return 'P';
    if (tile >= T_BARRIER_CYAN && tile <= T_BARRIER_AMBER) return 'B';
    if (tile >= T_SWITCH_CYAN && tile <= T_SWITCH_AMBER) return 'S';
    if (tile >= T_TELEPORT_CYAN && tile <= T_TELEPORT_AMBER) return 'O';
    if (tile == T_SPIKES) return '^';
    if (tile == T_FUEL) return 'F';
    if (tile == T_CHARGER) return 'C';
    return '+';
}

static int dump_level(int one_based)
{
    if (one_based < 1 || one_based > CAMPAIGN_LEVELS) {
        fprintf(stderr, "--dump-level needs 1..100\n");
        return 2;
    }
    LevelData level;
    level_build(one_based - 1, &level);
    printf("%03d  %s  spawn=%d,%d iris=%d,%d enemies=%d\n",
           one_based, level.title, level.spawn_x, level.spawn_y,
           level.exit_x, level.exit_y, level.enemy_count);
    for (int y = 0; y < FIELD_ROWS; y++) {
        for (int x = 0; x < FIELD_COLS; x++) putchar(dump_cell(&level, x, y));
        putchar('\n');
    }
    printf("@ entry  I iris  * mote  H rail  P phase  B shutter  S switch\n"
           "O ring gate  ^ thorns  F fuel  C charger  ! machine  # structure\n");
    return 0;
}

static int usage(void)
{
    printf("kilix-jpak %s\n"
           "usage: kilix-jpak [--level N] [--selftest [seed] [ticks]]\n"
           "                  [--rules-test] [--input-test]\n"
           "                  [--render-test [seed]] [--sound-test]\n"
           "                  [--dump-campaign] [--dump-level N]\n"
           "                  [--version] [--help]\n\n"
           "Run without arguments in a Kitty-protocol terminal to play.\n",
           KJ_VERSION);
    return 0;
}

static bool parse_u32_argument(const char *text, uint32_t *out)
{
    if (!text || !*text || !out) return false;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9') return false;

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > UINT32_MAX)
        return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_int_argument(const char *text, int minimum, int maximum,
                               int *out)
{
    uint32_t value;
    if (!out || minimum < 0 || maximum < minimum ||
        !parse_u32_argument(text, &value) || value > (uint32_t)maximum ||
        value < (uint32_t)minimum) return false;
    *out = (int)value;
    return true;
}

static int option_arity_error(const char *option, const char *expectation)
{
    fprintf(stderr, "%s %s\n", option, expectation);
    return 2;
}

static int play(int forced_level)
{
    int width = 0, height = 0;
    if (!term_init(&width, &height)) {
        fprintf(stderr, "kilix-jpak needs an interactive Kitty-protocol terminal\n");
        fprintf(stderr, "use --selftest or --render-test for headless operation\n");
        return 1;
    }
    install_signals();
    atexit(term_shutdown);
    game_init(width, height, (uint32_t)time(NULL));
    if (!render_init(width, height)) { term_shutdown(); return 1; }
    bool audio_available = sound_init();
    sound_set_enabled(audio_available && G.sound_on);
    if (forced_level >= 0) {
        G.practice_mode = true;
        game_start(forced_level);
    }

    const double tick_ms = 1000.0 / 60.0;
    const double frame_ms = 1000.0 / 30.0;
    const double maximum_elapsed_ms = 250.0;
    const int maximum_catchup_ticks = 8;
    double previous_time = now_ms();
    double next_frame = previous_time;
    double tick_accumulator = 0.0;
    while (!G.quit) {
        double current_time = now_ms();
        double elapsed = current_time - previous_time;
        previous_time = current_time;
        if (elapsed < 0.0) elapsed = 0.0;
        if (elapsed > maximum_elapsed_ms) elapsed = maximum_elapsed_ms;
        tick_accumulator += elapsed;

        if (term_read_input() < 0) { G.quit = true; break; }
        bool held = term_has_release_events();
        kittykb_event event;
        while (term_next_key_event(&event)) {
            if (event.action != KITTYKB_ACTION_PRESS) continue;
            if (interrupt_event(&event)) { G.quit = true; continue; }
            int key = game_key(&event);
            if (key < 0) continue;
            if (held && G.state == GS_PLAYING && continuous_key(key)) continue;
            game_handle_key(key);
        }
        if (G.quit) break;
        game_set_held_controls(
            held,
            term_key_down('a') || term_key_down('A') || term_key_down(KITTYKB_KEY_LEFT),
            term_key_down('d') || term_key_down('D') || term_key_down(KITTYKB_KEY_RIGHT),
            term_key_down('w') || term_key_down('W') || term_key_down(KITTYKB_KEY_UP),
            term_key_down('s') || term_key_down('S') || term_key_down(KITTYKB_KEY_DOWN),
            term_key_down(' ') || term_key_down('z') || term_key_down('Z'),
            term_key_down('x') || term_key_down('X') || term_key_down('e') || term_key_down('E'));

        int new_width, new_height;
        if (term_check_resize(&new_width, &new_height) &&
            (new_width != G.W || new_height != G.H)) {
            G.W = new_width; G.H = new_height;
            if (!render_resize(new_width, new_height)) { G.quit = true; break; }
        }
        int catchup_ticks = 0;
        while (tick_accumulator >= tick_ms &&
               catchup_ticks < maximum_catchup_ticks) {
            game_tick();
            tick_accumulator -= tick_ms;
            catchup_ticks++;
        }
        if (tick_accumulator >= tick_ms)
            tick_accumulator = fmod(tick_accumulator, tick_ms);
        render_frame();
        term_present(render_fb(), G.W, G.H);
        next_frame += frame_ms;
        double wait = next_frame - now_ms();
        if (wait < -100) next_frame = now_ms();
        else sleep_ms(wait);
    }
    game_shutdown(); sound_shutdown(); render_shutdown(); term_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        if (argc > 4)
            return option_arity_error("--selftest", "accepts only [seed] [ticks]");
        uint32_t seed = 1337u;
        int ticks = 12000;
        if (argc > 2 && !parse_u32_argument(argv[2], &seed)) {
            fprintf(stderr, "selftest seed must be an unsigned 32-bit integer\n");
            return 2;
        }
        if (argc > 3 && !parse_int_argument(argv[3], 1, INT_MAX, &ticks)) {
            fprintf(stderr, "selftest ticks must be an integer in 1..%d\n", INT_MAX);
            return 2;
        }
        return selftest(seed, ticks);
    }
    if (argc > 1 && !strcmp(argv[1], "--rules-test")) {
        if (argc != 2) return option_arity_error("--rules-test", "takes no arguments");
        return rules_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--input-test")) {
        if (argc != 2) return option_arity_error("--input-test", "takes no arguments");
        return input_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        if (argc > 3)
            return option_arity_error("--render-test", "accepts only [seed]");
        uint32_t seed = 42u;
        if (argc > 2 && !parse_u32_argument(argv[2], &seed)) {
            fprintf(stderr, "render-test seed must be an unsigned 32-bit integer\n");
            return 2;
        }
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test")) {
        if (argc != 2) return option_arity_error("--sound-test", "takes no arguments");
        return sound_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--dump-campaign")) {
        if (argc != 2)
            return option_arity_error("--dump-campaign", "takes no arguments");
        return dump_campaign();
    }
    if (argc > 1 && !strcmp(argv[1], "--dump-level")) {
        if (argc != 3)
            return option_arity_error("--dump-level", "needs exactly one level in 1..100");
        int level;
        if (!parse_int_argument(argv[2], 1, CAMPAIGN_LEVELS, &level)) {
            fprintf(stderr, "dump level must be an integer in 1..%d\n", CAMPAIGN_LEVELS);
            return 2;
        }
        return dump_level(level);
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        if (argc != 2) return option_arity_error("--version", "takes no arguments");
        printf("kilix-jpak %s\n", KJ_VERSION); return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        if (argc != 2) return option_arity_error(argv[1], "takes no arguments");
        return usage();
    }
    if (argc > 1 && !strcmp(argv[1], "--level")) {
        if (argc != 3)
            return option_arity_error("--level", "needs exactly one level in 1..100");
        int level;
        if (!parse_int_argument(argv[2], 1, CAMPAIGN_LEVELS, &level)) {
            fprintf(stderr, "level must be an integer in 1..%d\n", CAMPAIGN_LEVELS);
            return 2;
        }
        return play(level - 1);
    }
    if (argc > 1) { fprintf(stderr, "unknown option: %s\n", argv[1]); usage(); return 2; }
    return play(-1);
}
