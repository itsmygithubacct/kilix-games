"""What a player can see, as a fixed-length float vector for a network.

Only information a human player has is encoded: face-down tableau cards and
the undealt stock are "hidden" (their identities are never leaked), and so
are waste cards below the top that a player could not see. The layout is
fixed and documented so a trained model and this encoder cannot silently
drift apart; OBSERVATION_VERSION changes whenever the layout does.

Per card (52 cards x 13 features, in card-id order):
    0   hidden (face-down tableau, stock, or buried waste)
    1   waste top (playable)
    2   on its foundation
    3-9 face-up on tableau column 0..6
    10  depth from the top of its tableau column / 12 (0 = top card)
    11  is the top card of a tableau column
    12  rank / 13 (always set; lets the net generalise across suits)
Globals (14):
    face-down count per column / 6 (7), stock size / 24, waste size / 24,
    draw-three flag, foundation height per suit / 13 (4)
"""
from __future__ import annotations

from .engine import TABLEAU, State, rank_of

OBSERVATION_VERSION = 1
PER_CARD = 13
GLOBALS = 14
OBSERVATION_SIZE = 52 * PER_CARD + GLOBALS       # 690


def observe(state: State) -> list:
    out = [0.0] * OBSERVATION_SIZE
    for c in range(52):
        base = c * PER_CARD
        out[base + 0] = 1.0                       # hidden unless shown below
        out[base + 12] = rank_of(c) / 13.0

    def show(c: int) -> int:
        base = c * PER_CARD
        out[base + 0] = 0.0
        return base

    top = state.waste_top()
    if top is not None:
        out[show(top) + 1] = 1.0
    for s in range(4):
        for r in range(1, state.found[s] + 1):
            out[show(s * 13 + r - 1) + 2] = 1.0
    for i in range(TABLEAU):
        up = state.face_up(i)
        for depth, c in enumerate(reversed(up)):
            base = show(c)
            out[base + 3 + i] = 1.0
            out[base + 10] = depth / 12.0
            out[base + 11] = 1.0 if depth == 0 else 0.0

    g = 52 * PER_CARD
    for i in range(TABLEAU):
        out[g + i] = state.down[i] / 6.0
    out[g + 7] = len(state.stock) / 24.0
    out[g + 8] = len(state.waste) / 24.0
    out[g + 9] = 1.0 if state.draw3 else 0.0
    for s in range(4):
        out[g + 10 + s] = state.found[s] / 13.0
    return out
