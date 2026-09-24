"""Move features: what a learned player scores, one candidate move at a time.

The fixed observation/action pair (`observe`, `actions`) makes a network
learn relations such as "this waste card is next on its foundation" from
raw card identities, and map them onto the right one of 681 action slots.
Imitating the planner that way failed: the network fell back on the most
common label (deal). Here each candidate move is described by what it does,
in terms a player can see, and a small network gives it one score; the
player takes the best-scoring candidate.

Only what a player knows is used: the visible table and the game so far.
Nothing describes the card a move would turn up or a deal would show, which
are hidden until the move is made. `History` carries the game so far: how
often each position has been reached, and how many deals in a row have
changed nothing but the stock and waste. Without them a learned player
cannot tell a useful deal from one that only cycles the stock, and a
consistent preference for dealing locks the game up.
"""
from __future__ import annotations

from .engine import (DEAL, FOUND_TAB, TAB_FOUND, TAB_TAB, TABLEAU, WASTE_FOUND,
                     WASTE_TAB, Move, State, apply, can_found, is_red, rank_of)
from .sim import position_key

FEATURE_VERSION = 2
KINDS = (DEAL, WASTE_FOUND, WASTE_TAB, TAB_FOUND, TAB_TAB, FOUND_TAB)
NAMES = (
    [f"kind_{k}" for k in KINDS]
    + ["count", "reveals", "src_down", "empties_column", "frees_home",
       "moved_rank", "moved_king", "moved_red", "dest_empty", "dest_rank",
       "home_after_is_safe", "recycle", "stock", "waste", "face_down",
       "cards_home", "empty_columns", "waste_top_rank", "waste_top_home",
       "movable_kings", "home_min", "home_max", "phase",
       "after_seen", "here_seen", "idle_deals"]
)
FEATURE_COUNT = len(NAMES)


def _moved_card(state: State, move: Move):
    if move.kind in (WASTE_FOUND, WASTE_TAB):
        return state.waste_top()
    if move.kind == TAB_FOUND:
        return state.tableau[move.src][-1]
    if move.kind == TAB_TAB:
        return state.face_up(move.src)[-move.count]       # the run's base card
    if move.kind == FOUND_TAB:
        rank = state.found[move.src]
        return move.src * 13 + rank - 1 if rank else None
    return None


def _safe_home(card: int, state: State) -> bool:
    """Sending this card home can never cost a move later: both
    opposite-colour cards one rank down are already home (aces and twos
    always)."""
    rank = rank_of(card)
    if rank <= 2:
        return True
    red = is_red(card)
    opposite = [s for s in range(4) if (s in (1, 2)) != red]
    return all(state.found[s] >= rank - 1 for s in opposite)


class History:
    """The game so far, as a player remembers it. Call `see(state)` once for
    every position the game reaches, the current one included."""

    def __init__(self):
        self.visited: dict = {}
        self.idle = 0                    # deals in a row with no other change
        self._table = None

    def see(self, state: State) -> None:
        key = position_key(state)
        self.visited[key] = self.visited.get(key, 0) + 1
        table = (state.found, state.tableau, state.down)
        self.idle = self.idle + 1 if table == self._table else 0
        self._table = table


def move_features(state: State, move: Move, history: History) -> list:
    out = [0.0] * FEATURE_COUNT
    out[KINDS.index(move.kind)] = 1.0
    f = len(KINDS)
    card = _moved_card(state, move)
    if move.kind == TAB_TAB:
        up = state.face_up(move.src)
        whole = move.count == len(up)
        out[f + 0] = move.count / 13.0
        out[f + 1] = 1.0 if whole and state.down[move.src] > 0 else 0.0
        out[f + 2] = state.down[move.src] / 6.0
        out[f + 3] = 1.0 if whole and state.down[move.src] == 0 else 0.0
        out[f + 4] = 1.0 if not whole and can_found(up[-move.count - 1], state) else 0.0
    elif move.kind == TAB_FOUND:
        up = state.face_up(move.src)
        out[f + 0] = 1.0 / 13.0
        out[f + 1] = 1.0 if len(up) == 1 and state.down[move.src] > 0 else 0.0
        out[f + 2] = state.down[move.src] / 6.0
        out[f + 3] = 1.0 if len(state.tableau[move.src]) == 1 else 0.0
        out[f + 4] = 1.0 if len(up) > 1 and can_found(up[-2], state) else 0.0
    if card is not None:
        out[f + 5] = rank_of(card) / 13.0
        out[f + 6] = 1.0 if rank_of(card) == 13 else 0.0
        out[f + 7] = 1.0 if is_red(card) else 0.0
        if move.kind in (WASTE_FOUND, TAB_FOUND):
            out[f + 10] = 1.0 if _safe_home(card, state) else 0.0
    if move.kind in (WASTE_TAB, TAB_TAB, FOUND_TAB):
        dest = state.tableau[move.dst]
        out[f + 8] = 0.0 if dest else 1.0
        out[f + 9] = rank_of(dest[-1]) / 13.0 if dest else 0.0
    if move.kind == DEAL:
        out[f + 11] = 0.0 if state.stock else 1.0
    g = f + 12
    out[g + 0] = len(state.stock) / 24.0
    out[g + 1] = len(state.waste) / 24.0
    out[g + 2] = sum(state.down) / 21.0
    out[g + 3] = state.cards_home() / 52.0
    out[g + 4] = sum(1 for i in range(TABLEAU) if not state.tableau[i]) / 7.0
    top = state.waste_top()
    if top is not None:
        out[g + 5] = rank_of(top) / 13.0
        out[g + 6] = 1.0 if can_found(top, state) else 0.0
    out[g + 7] = sum(1 for i in range(TABLEAU)
                     if state.down[i] > 0 and state.face_up(i)
                     and rank_of(state.face_up(i)[0]) == 13) / 4.0
    out[g + 8] = min(state.found) / 13.0
    out[g + 9] = max(state.found) / 13.0
    out[g + 10] = 1.0 if not state.stock and not state.waste else 0.0
    after = position_key(apply(state, move, check=False))
    out[g + 11] = min(history.visited.get(after, 0), 3) / 3.0
    out[g + 12] = min(history.visited.get(position_key(state), 0), 3) / 3.0
    out[g + 13] = min(history.idle, 48) / 24.0
    return out
