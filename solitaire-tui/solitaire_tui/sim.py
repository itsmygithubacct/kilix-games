"""Headless games: evaluation, and later, training data.

`play` runs one game with an agent and stops on a win, a resignation, the
step limit, or a stuck position (the same position repeated, which is how a
stock that cycles without progress shows up). With record=True it returns
the (observation, action index) pairs a policy would learn from.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

from . import actions, observe
from .engine import State, apply, deal, legal_moves


def position_key(state: State) -> tuple:
    """Everything that defines the position; not the move counter."""
    return (state.stock, state.waste, state.found, state.tableau, state.down)


@dataclass
class Result:
    seed: Optional[int]
    won: bool
    cards_home: int
    steps: int
    reason: str                       # won | resigned | stuck | step-limit
    trajectory: list = field(default_factory=list)


def play(agent, seed: Optional[int] = None, draw3: bool = False,
         max_steps: int = 2000, record: bool = False) -> Result:
    state = deal(seed, draw3)
    seen = {position_key(state): 1}
    trajectory = []
    for step in range(max_steps):
        if state.won:
            return Result(seed, True, 52, step, "won", trajectory)
        legal = legal_moves(state)
        move = agent.choose(state, legal)
        if move is None:
            return Result(seed, False, state.cards_home(), step, "resigned", trajectory)
        if record:
            trajectory.append((observe.observe(state), actions.encode(move)))
        state = apply(state, move)
        key = position_key(state)
        seen[key] = seen.get(key, 0) + 1
        if seen[key] > 2:
            return Result(seed, False, state.cards_home(), step + 1, "stuck", trajectory)
    return Result(seed, state.won, state.cards_home(), max_steps,
                  "won" if state.won else "step-limit", trajectory)


def tournament(agent_factory, seeds, draw3: bool = False, max_steps: int = 2000) -> dict:
    results = [play(agent_factory(), s, draw3, max_steps) for s in seeds]
    wins = sum(r.won for r in results)
    return {
        "games": len(results),
        "wins": wins,
        "win_rate": wins / len(results) if results else 0.0,
        "mean_cards_home": sum(r.cards_home for r in results) / max(len(results), 1),
        "reasons": {k: sum(r.reason == k for r in results)
                    for k in ("won", "resigned", "stuck", "step-limit")},
    }
