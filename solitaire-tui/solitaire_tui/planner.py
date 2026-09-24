"""A determinized rollout planner: the teacher a learned policy imitates.

Klondike hides cards, so a planner that read the true face-down cards would
choose moves a player could not justify from what is visible. Its labels
would then not be a function of the observation, and a network could not
learn them. This planner only uses what a player sees. At each decision it
re-deals the hidden cards (the stock, face-down tableau cards and buried
waste) into `samples` arrangements consistent with the visible table,
plays each legal move in each arrangement, rolls the game on with a noisy
greedy player for up to `horizon` moves, and picks the move with the best
mean outcome. The re-deal is seeded from the visible position, so the
planner is deterministic.
"""
from __future__ import annotations

import hashlib
import random
from dataclasses import replace
from typing import Optional, Sequence

from .agents import GreedyAgent
from .engine import (DEAL, FOUND_TAB, TAB_TAB, TABLEAU, Move, State, apply,
                     can_found, legal_moves, rank_of)
from .sim import position_key


def _visible_key(state: State) -> int:
    """A stable seed from the visible position.

    Not hash(): before Python 3.12 hash(None) (an empty waste) depends on
    the object's address, which made the planner differ between processes.
    """
    top = state.waste_top()
    parts = (state.found, tuple(state.face_up(i) for i in range(TABLEAU)),
             state.down, -1 if top is None else top, len(state.stock),
             len(state.waste), state.draw3)
    digest = hashlib.blake2b(repr(parts).encode(), digest_size=4).digest()
    return int.from_bytes(digest, "big") & 0x7FFFFFFF


def redeal_hidden(state: State, rng: random.Random) -> State:
    """The same visible table with every hidden card identity shuffled."""
    hidden = list(state.stock) + list(state.waste[:-1])
    for i in range(TABLEAU):
        hidden += state.tableau[i][:state.down[i]]
    rng.shuffle(hidden)
    it = iter(hidden)
    stock = tuple(next(it) for _ in state.stock)
    waste = tuple(next(it) for _ in state.waste[:-1]) + state.waste[-1:]
    tableau = tuple(tuple(next(it) for _ in range(state.down[i])) + state.face_up(i)
                    for i in range(TABLEAU))
    return replace(state, stock=stock, waste=waste, tableau=tableau)


def _movable_king(state: State, exclude: int) -> bool:
    """A king that an empty column would help: the waste top, or a king
    heading a face-up run that sits on face-down cards."""
    top = state.waste_top()
    if top is not None and rank_of(top) == 13:
        return True
    for i in range(TABLEAU):
        if i != exclude and state.down[i] > 0:
            up = state.face_up(i)
            if up and rank_of(up[0]) == 13:
                return True
    return False


def sensible_moves(state: State, legal: Sequence[Move]) -> list:
    """The progress-making subset of the legal moves, which the search uses.

    Kept: every foundation play, waste play and deal. A tableau-to-tableau
    move is kept only when it turns up a face-down card, frees the card
    beneath it for its foundation, or empties a column that a waiting king
    can use. Taking a card off a foundation is dropped. Apart from dealing,
    every kept move is irreversible progress, so a game played from this
    set ends: without the filter the search wandered through fresh
    positions until the step limit. Empty only when no legal move is
    sensible, and then the caller falls back to the full list.
    """
    out = []
    for m in legal:
        if m.kind == FOUND_TAB:
            continue
        if m.kind == TAB_TAB:
            up = state.face_up(m.src)
            if m.count == len(up):
                if state.down[m.src] == 0 and not _movable_king(state, m.src):
                    continue
                if state.down[m.src] == 0 and rank_of(up[0]) == 13:
                    continue          # a king from one empty column to another
            elif not can_found(up[-m.count - 1], state):
                continue
        out.append(m)
    return out


def candidate_moves(state: State, legal: Sequence[Move], visited: dict) -> list:
    """The moves a planning or learned player chooses between.

    Sensible moves (above) that do not walk back into a position already
    reached this game; a deal may revisit once, for a full stock cycle.
    When every sensible move revisits, those are allowed, and the game's
    repeat rule ends it. Empty means nothing sensible is left: resign.
    The learned player uses this same function, so its training mask and
    its choices at play time agree.
    """
    sensible = sensible_moves(state, legal)
    fresh = [m for m in sensible
             if visited.get(position_key(apply(state, m, check=False)), 0)
             < (2 if m.kind == DEAL else 1)]
    return fresh or sensible


def _score(state: State) -> float:
    if state.won:
        return 100.0
    face_down = sum(state.down)
    return state.cards_home() + 0.5 * (21 - face_down)


def rollout(state: State, rng: random.Random, horizon: int,
            filtered: bool = False) -> float:
    """Play on with a noisy greedy player and score where it ends.

    filtered=True has the greedy player choose from sensible_moves (so it
    also makes the partial-run and column-clearing moves plain greedy
    never makes), taking the first best on a zero score."""
    greedy = GreedyAgent()
    seen = {position_key(state): 1}
    for _ in range(horizon):
        if state.won:
            break
        legal = legal_moves(state)
        if not legal:
            break
        if rng.random() < 0.15:
            candidates = sensible_moves(state, legal)
            move = rng.choice(candidates) if candidates else None
        elif filtered:
            pool = sensible_moves(state, legal)
            move = max(pool, key=lambda m: greedy.score(state, m)) if pool else None
        else:
            move = greedy.choose(state, legal)
        if move is None:
            break
        state = apply(state, move, check=False)
        key = position_key(state)
        seen[key] = seen.get(key, 0) + 1
        if seen[key] > 2:
            break
    return _score(state)


class PlannerAgent:
    """Determinized-rollout teacher. Deterministic for a given position."""
    name = "planner"

    def __init__(self, samples: int = 6, horizon: int = 80, filtered: bool = False):
        self.samples = samples
        self.horizon = horizon
        self.filtered = filtered     # rollouts choose from sensible_moves
        self.visited: dict = {}      # positions reached this game -> count
        self.last_values: dict = {}  # move -> value from the last choose()

    def choose(self, state: State, legal: Sequence[Move]) -> Optional[Move]:
        if not legal:
            return None
        here = position_key(state)
        self.visited[here] = self.visited.get(here, 0) + 1
        legal = candidate_moves(state, legal, self.visited)
        self.last_values = {}
        if not legal:
            return None
        if len(legal) == 1:
            return legal[0]
        base = _visible_key(state)
        worlds = [redeal_hidden(state, random.Random(base * 1000 + k))
                  for k in range(self.samples)]
        best, best_value = None, float("-inf")
        for index, move in enumerate(legal):
            total = 0.0
            for k, world in enumerate(worlds):
                after = apply(world, move, check=False)
                total += rollout(after, random.Random(base ^ (index * 7919 + k)),
                                 self.horizon, self.filtered)
            value = total / self.samples
            # Dealing only helps when nothing better exists; tiny tie-break.
            if move.kind == DEAL:
                value -= 0.01
            self.last_values[move] = value
            if value > best_value:
                best, best_value = move, value
        return best
