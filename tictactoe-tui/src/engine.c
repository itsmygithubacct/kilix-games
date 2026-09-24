/* Rules, the exact solver, and the computer players. */
#include "ttt.h"
#include "kilix_game_policy.h"
#include "neural_policy_blob.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const uint16_t lines[8] = {
    0x007, 0x038, 0x1c0,          /* rows */
    0x049, 0x092, 0x124,          /* columns */
    0x111, 0x054                  /* diagonals */
};
#define FULL 0x1ffu

static bool has_line(uint16_t marks)
{
    for (int i = 0; i < 8; i++)
        if ((marks & lines[i]) == lines[i]) return true;
    return false;
}

static int popcount9(uint16_t v)
{
    int n = 0;
    for (; v; v &= (uint16_t)(v - 1)) n++;
    return n;
}

/* ---------- board ---------- */

void board_init(Board *b, int first)
{
    b->x = b->o = 0;
    b->first = first == MARK_O ? MARK_O : MARK_X;
}

int board_cell(const Board *b, int cell)
{
    if (cell < 0 || cell > 8) return MARK_NONE;
    if (b->x >> cell & 1u) return MARK_X;
    if (b->o >> cell & 1u) return MARK_O;
    return MARK_NONE;
}

int board_moves_made(const Board *b)
{
    return popcount9(b->x) + popcount9(b->o);
}

int board_to_move(const Board *b)
{
    int second = b->first == MARK_X ? MARK_O : MARK_X;
    return board_moves_made(b) % 2 == 0 ? b->first : second;
}

uint16_t board_winning_line(const Board *b)
{
    uint16_t found = 0;
    for (int i = 0; i < 8; i++)
        if ((b->x & lines[i]) == lines[i] || (b->o & lines[i]) == lines[i])
            found |= lines[i];      /* a double line lights both */
    return found;
}

int board_result(const Board *b)
{
    if (has_line(b->x)) return RESULT_X;
    if (has_line(b->o)) return RESULT_O;
    return ((b->x | b->o) & FULL) == FULL ? RESULT_DRAW : RESULT_NONE;
}

uint16_t board_legal(const Board *b)
{
    if (board_result(b) != RESULT_NONE) return 0;
    return (uint16_t)(~(b->x | b->o) & FULL);
}

bool board_play(Board *b, int cell)
{
    if (cell < 0 || cell > 8 || !(board_legal(b) >> cell & 1u)) return false;
    if (board_to_move(b) == MARK_X) b->x |= (uint16_t)(1u << cell);
    else b->o |= (uint16_t)(1u << cell);
    return true;
}

void board_relative(const Board *b, uint16_t *own, uint16_t *opp)
{
    bool x_moves = board_to_move(b) == MARK_X;
    *own = x_moves ? b->x : b->o;
    *opp = x_moves ? b->o : b->x;
}

/* ---------- solver ----------
 *
 * A position is keyed by its base-3 index (own = 1, opponent = 2), so the
 * whole game fits in two 3^9-entry tables filled on demand.
 */
#define POSITIONS 19683
static int8_t value_memo[POSITIONS];     /* 0 unknown, else value + 2 */
static int8_t distance_memo[POSITIONS];  /* 0 unknown, else distance + 1 */

static int position_index(uint16_t own, uint16_t opp)
{
    int index = 0;
    for (int cell = 8; cell >= 0; cell--)
        index = index * 3 + (own >> cell & 1u ? 1 : (opp >> cell & 1u ? 2 : 0));
    return index;
}

/* True when the position is over: the opponent just completed a line or the
   board is full. (The mover cannot already have a line: they did not move.) */
static bool terminal(uint16_t own, uint16_t opp)
{
    return has_line(opp) || has_line(own) || ((own | opp) & FULL) == FULL;
}

int solve_value(uint16_t own, uint16_t opp)
{
    if (has_line(opp)) return -1;
    if (has_line(own)) return 1;
    if (((own | opp) & FULL) == FULL) return 0;
    int index = position_index(own, opp);
    if (value_memo[index]) return value_memo[index] - 2;
    int best = -2;
    uint16_t empty = (uint16_t)(~(own | opp) & FULL);
    for (int cell = 0; cell < 9 && best < 1; cell++) {
        if (!(empty >> cell & 1u)) continue;
        int v = -solve_value(opp, (uint16_t)(own | 1u << cell));
        if (v > best) best = v;
    }
    value_memo[index] = (int8_t)(best + 2);
    return best;
}

int solve_move_value(uint16_t own, uint16_t opp, int cell)
{
    return -solve_value(opp, (uint16_t)(own | 1u << cell));
}

uint16_t solve_optimal(uint16_t own, uint16_t opp)
{
    if (terminal(own, opp)) return 0;
    int best = solve_value(own, opp);
    uint16_t empty = (uint16_t)(~(own | opp) & FULL), moves = 0;
    for (int cell = 0; cell < 9; cell++)
        if (empty >> cell & 1u && solve_move_value(own, opp, cell) == best)
            moves |= (uint16_t)(1u << cell);
    return moves;
}

int solve_distance(uint16_t own, uint16_t opp)
{
    if (terminal(own, opp)) return 0;
    int index = position_index(own, opp);
    if (distance_memo[index]) return distance_memo[index] - 1;
    int best = solve_value(own, opp);
    uint16_t optimal = solve_optimal(own, opp);
    int chosen = -1;
    for (int cell = 0; cell < 9; cell++) {
        if (!(optimal >> cell & 1u)) continue;
        int d = 1 + solve_distance(opp, (uint16_t)(own | 1u << cell));
        if (chosen < 0 || (best > 0 ? d < chosen : d > chosen)) chosen = d;
    }
    distance_memo[index] = (int8_t)(chosen + 1);
    return chosen;
}

/* ---------- neural player ---------- */

static kilix_policy policy;
static int policy_state;               /* 0 untried, 1 ready, -1 failed */
static char policy_status[96] = "not loaded";

static void policy_load_once(void)
{
    if (policy_state) return;
    kilix_policy_status status =
        kilix_policy_load(&policy, neural_policy_blob, neural_policy_blob_size);
    if (status != KILIX_POLICY_OK) {
        (void)snprintf(policy_status, sizeof policy_status, "policy rejected: %s",
                       kilix_policy_status_string(status));
        policy_state = -1;
    } else if (kilix_policy_input_count(&policy) != POLICY_FEATURES ||
               kilix_policy_output_count(&policy) != POLICY_ACTIONS) {
        (void)snprintf(policy_status, sizeof policy_status,
                       "policy shape %zu->%zu, game needs %d->%d",
                       kilix_policy_input_count(&policy),
                       kilix_policy_output_count(&policy),
                       POLICY_FEATURES, POLICY_ACTIONS);
        kilix_policy_free(&policy);
        policy_state = -1;
    } else {
        (void)snprintf(policy_status, sizeof policy_status,
                       "ready: %zu parameters, digest %016llx", policy.parameter_count,
                       (unsigned long long)policy.digest);
        policy_state = 1;
    }
}

bool neural_ready(void)
{
    policy_load_once();
    return policy_state == 1;
}

const char *neural_status(void)
{
    policy_load_once();
    return policy_status;
}

void neural_features(uint16_t own, uint16_t opp, float out[POLICY_FEATURES])
{
    for (int cell = 0; cell < 9; cell++) {
        out[cell] = own >> cell & 1u ? 1.0f : 0.0f;
        out[9 + cell] = opp >> cell & 1u ? 1.0f : 0.0f;
    }
}

bool neural_scores(const Board *b, float out[POLICY_ACTIONS])
{
    if (!neural_ready()) return false;
    uint16_t own, opp;
    float features[POLICY_FEATURES];
    board_relative(b, &own, &opp);
    neural_features(own, opp, features);
    return kilix_policy_forward(&policy, features, POLICY_FEATURES, out,
                                POLICY_ACTIONS) == KILIX_POLICY_OK;
}

uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static float rng_unit(uint32_t *state)
{
    return (float)(rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

static int random_legal(uint16_t legal, uint32_t *rng)
{
    int count = popcount9(legal);
    if (!count) return -1;
    int pick = (int)(rng_next(rng) % (uint32_t)count);
    for (int cell = 0; cell < 9; cell++)
        if (legal >> cell & 1u && pick-- == 0) return cell;
    return -1;
}

/* Scores are the network's estimate of each move's value to the mover,
   about -1 (loses) .. +1 (wins); the sampling temperatures are in those units. */
static const float level_temperature[LEVEL_COUNT] = { 0.55f, 0.18f, 0.0f };

int choose_move(const Board *b, int player, int level, uint32_t *rng)
{
    uint16_t legal = board_legal(b);
    if (!legal) return -1;
    if (player == PLAYER_RANDOM) return random_legal(legal, rng);

    float scores[POLICY_ACTIONS];
    if (!neural_scores(b, scores)) {       /* no policy: play the solver */
        uint16_t own, opp;
        board_relative(b, &own, &opp);
        return random_legal(solve_optimal(own, opp), rng);
    }
    if (level < 0 || level >= LEVEL_COUNT) level = LEVEL_PERFECT;
    float temperature = level_temperature[level];
    int best = -1;
    for (int cell = 0; cell < 9; cell++)
        if (legal >> cell & 1u && (best < 0 || scores[cell] > scores[best])) best = cell;
    if (temperature <= 0.0f) return best;

    /* softmax(score / T) over the legal moves, shifted by the best score. */
    float weights[9], total = 0.0f;
    for (int cell = 0; cell < 9; cell++) {
        weights[cell] = legal >> cell & 1u
                        ? expf((scores[cell] - scores[best]) / temperature) : 0.0f;
        total += weights[cell];
    }
    float pick = rng_unit(rng) * total;
    for (int cell = 0; cell < 9; cell++) {
        if (!(legal >> cell & 1u)) continue;
        if (pick < weights[cell]) return cell;
        pick -= weights[cell];
    }
    return best;
}

const char *player_name(int player)
{
    static const char *names[PLAYER_COUNT] = { "HUMAN", "NEURAL", "RANDOM" };
    return player >= 0 && player < PLAYER_COUNT ? names[player] : "?";
}

const char *level_name(int level)
{
    static const char *names[LEVEL_COUNT] = { "EASY", "NORMAL", "PERFECT" };
    return level >= 0 && level < LEVEL_COUNT ? names[level] : "?";
}
