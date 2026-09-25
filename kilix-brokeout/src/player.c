/* The neural player: a kilix-game-kit policy compiled in from
 * src/neural_policy_blob.h (tools/neural/install-policy.py regenerates it),
 * and the per-tick hand-off between the keyboard and a computer player. */
#include "kitty_brokeout.h"
#include "kilix_game_policy.h"
#include "neural_policy_blob.h"

#include <stdio.h>

static kilix_policy policy;
static int policy_state;                 /* 0 untried, 1 ready, -1 failed */
static char policy_status[96] = "not loaded";

static void load_once(void)
{
    if (policy_state) return;
    kilix_policy_status st = kilix_policy_load(&policy, neural_policy_blob, neural_policy_blob_size);
    if (st != KILIX_POLICY_OK) {
        snprintf(policy_status, sizeof policy_status, "policy rejected: %s",
                 kilix_policy_status_string(st));
        policy_state = -1;
    } else if (kilix_policy_input_count(&policy) != POLICY_FEATURES ||
               kilix_policy_output_count(&policy) != POLICY_ACTIONS) {
        snprintf(policy_status, sizeof policy_status, "policy shape %zu->%zu, game needs %d->%d",
                 kilix_policy_input_count(&policy), kilix_policy_output_count(&policy),
                 POLICY_FEATURES, POLICY_ACTIONS);
        kilix_policy_free(&policy);
        policy_state = -1;
    } else {
        snprintf(policy_status, sizeof policy_status, "ready: %zu parameters, digest %016llx",
                 policy.parameter_count, (unsigned long long)policy.digest);
        policy_state = 1;
    }
}

bool player_neural_ready(void)
{
    load_once();
    return policy_state == 1;
}

const char *player_neural_status(void)
{
    load_once();
    return policy_status;
}

int player_neural_action(void)
{
    float x[POLICY_FEATURES], y[POLICY_ACTIONS];
    if (!player_neural_ready()) return POLICY_ACTIONS / 2;
    game_policy_features(x);
    if (kilix_policy_forward(&policy, x, POLICY_FEATURES, y, POLICY_ACTIONS) != KILIX_POLICY_OK)
        return POLICY_ACTIONS / 2;                   /* centre strike */
    return (int)kilix_policy_argmax(y, POLICY_ACTIONS);
}

bool player_tick(void)
{
    if ((G.state != GS_PLAYING && G.state != GS_BALL_LOST) || G.controller == PLAYER_YOU)
        return false;
    if (G.controller == PLAYER_NEURAL && player_neural_ready()) {
        game_apply_action(player_neural_action());   /* every tick, as in training */
        return true;
    }
    /* AUTOPILOT, or NEURAL without a usable policy: the scripted player. */
    game_set_held_controls(false, false, false);
    game_autopilot_tick();
    return true;
}
