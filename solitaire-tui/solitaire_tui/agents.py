"""Players that are not a person at the keyboard.

Every agent implements one method:

    choose(state, legal) -> Move | None     (None = resign)

`legal` is engine.legal_moves(state), passed in so agents never disagree with
the rules about what is allowed. The simulator and the UI's hint key only
know this interface. A neural policy plugs in as PolicyAgent with a function
over (observation, legal-action mask); see observe.py and actions.py for
the fixed layouts it consumes.
"""
from __future__ import annotations

import random
from typing import Callable, Optional, Sequence

from . import actions, observe
from .engine import (DEAL, FOUND_TAB, TAB_FOUND, TAB_TAB, WASTE_FOUND,
                     WASTE_TAB, Move, State)


class RandomAgent:
    """Uniform over legal moves; a floor for comparisons."""
    name = "random"

    def __init__(self, seed: Optional[int] = None):
        self.rng = random.Random(seed)

    def choose(self, state: State, legal: Sequence[Move]) -> Optional[Move]:
        return self.rng.choice(list(legal)) if legal else None


class GreedyAgent:
    """A simple, loop-free heuristic player.

    In priority order: send any card home; move a whole face-up run to
    reveal a face-down card (bigger hidden piles first); play the waste onto
    the tableau; deal. It never makes a tableau move that reveals nothing
    and never takes a card back off a foundation, so it cannot oscillate.
    The simulator ends the game when dealing only cycles the stock.
    """
    name = "greedy"

    def score(self, state: State, move: Move) -> float:
        if move.kind in (WASTE_FOUND, TAB_FOUND):
            return 100.0
        if move.kind == TAB_TAB:
            up = state.face_up(move.src)
            if move.count == len(up) and state.down[move.src] > 0:
                return 80.0 + state.down[move.src]
            return 0.0
        if move.kind == WASTE_TAB:
            return 50.0
        if move.kind == DEAL:
            return 1.0
        return 0.0                      # FOUND_TAB and anything new

    def choose(self, state: State, legal: Sequence[Move]) -> Optional[Move]:
        best, best_score = None, 0.0
        for move in legal:
            s = self.score(state, move)
            if s > best_score:
                best, best_score = move, s
        return best


class PolicyAgent:
    """The hook for a learned policy.

    `policy(observation, mask) -> index` receives observe.observe(state)
    (OBSERVATION_SIZE floats) and actions.legal_mask(state) (ACTION_COUNT
    booleans) and returns an action index. The chosen index must be legal;
    an illegal choice is an error, never silently replaced.
    """
    name = "policy"

    def __init__(self, policy: Callable[[list, list], int]):
        self.policy = policy

    def choose(self, state: State, legal: Sequence[Move]) -> Optional[Move]:
        if not legal:
            return None
        mask = actions.legal_mask(state)
        index = self.policy(observe.observe(state), mask)
        if not (0 <= index < actions.ACTION_COUNT and mask[index]):
            raise ValueError(f"policy chose illegal action {index}")
        return actions.decode(index)


AGENTS = {"random": RandomAgent, "greedy": GreedyAgent}
