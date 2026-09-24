"""Klondike rules as pure data: no I/O, no randomness after the deal.

This engine is shared by the terminal UI, the headless simulator and any
agent (scripted or learned). A State is a plain value: `apply` returns a new
State and never mutates its input, so search, replay and training can copy,
branch and compare states freely.

Rules follow kilix 95 Solitaire (kilix desktop/apps/sol.py), from which this
game is derived:
  - deal one (default) or deal three, unlimited passes through the stock;
  - tableau builds down in alternating colours, only a King fills a gap;
  - any valid face-up run moves as a unit;
  - foundations build up by suit from the Ace;
  - a foundation's top card may come back down onto the tableau;
  - the newly exposed face-down tableau card turns up automatically.

The same seed deals the same layout as kilix 95 Solitaire:
random.Random(seed).shuffle over the deck in suit-major order, dealt by pop().

Cards are ints 0..51: suit * 13 + (rank - 1). Suits follow sol.py's order:
0 spade, 1 heart, 2 diamond, 3 club (hearts and diamonds are red).
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field, replace
from typing import Optional

SUIT_SYMBOLS = ("♠", "♥", "♦", "♣")   # ♠ ♥ ♦ ♣
SUIT_NAMES = ("spades", "hearts", "diamonds", "clubs")
RANK_NAMES = ("", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K")
TABLEAU = 7


def card(rank: int, suit: int) -> int:
    return suit * 13 + (rank - 1)


def rank_of(c: int) -> int:
    return c % 13 + 1


def suit_of(c: int) -> int:
    return c // 13


def is_red(c: int) -> bool:
    return suit_of(c) in (1, 2)


def card_name(c: int) -> str:
    return RANK_NAMES[rank_of(c)] + SUIT_SYMBOLS[suit_of(c)]


# ---------------------------------------------------------------- moves

DEAL = "deal"                # stock -> waste, or recycle waste -> stock
WASTE_FOUND = "waste_found"  # waste top -> its foundation
WASTE_TAB = "waste_tab"      # waste top -> tableau dst
TAB_FOUND = "tab_found"      # tableau src top -> its foundation
TAB_TAB = "tab_tab"          # top `count` cards of tableau src -> tableau dst
FOUND_TAB = "found_tab"      # foundation src (a suit) top -> tableau dst


@dataclass(frozen=True)
class Move:
    kind: str
    src: int = 0
    dst: int = 0
    count: int = 1

    def __str__(self) -> str:
        if self.kind == DEAL:
            return "deal"
        if self.kind == WASTE_FOUND:
            return "waste->foundation"
        if self.kind == WASTE_TAB:
            return f"waste->T{self.dst + 1}"
        if self.kind == TAB_FOUND:
            return f"T{self.src + 1}->foundation"
        if self.kind == TAB_TAB:
            return f"T{self.src + 1}->T{self.dst + 1} ({self.count})"
        return f"F{SUIT_SYMBOLS[self.src]}->T{self.dst + 1}"


# ---------------------------------------------------------------- state

@dataclass(frozen=True)
class State:
    """One Klondike position.

    stock/waste: bottom -> top (the top is the last element).
    tableau[i]:  bottom -> top; the first down[i] cards are face down.
    found[s]:    how many cards of suit s are on its foundation (0..13).
    fan:         waste cards shown fanned after a deal-three (display only).
    """
    stock: tuple = ()
    waste: tuple = ()
    found: tuple = (0, 0, 0, 0)
    tableau: tuple = ((),) * TABLEAU
    down: tuple = (0,) * TABLEAU
    draw3: bool = False
    fan: int = 0
    moves: int = 0
    seed: Optional[int] = field(default=None, compare=False)

    # -- queries
    @property
    def won(self) -> bool:
        return all(n == 13 for n in self.found)

    def face_up(self, i: int) -> tuple:
        return self.tableau[i][self.down[i]:]

    def waste_top(self) -> Optional[int]:
        return self.waste[-1] if self.waste else None

    def found_top(self, s: int) -> Optional[int]:
        return card(self.found[s], s) if self.found[s] else None

    def cards_home(self) -> int:
        return sum(self.found)


def deal(seed: Optional[int] = None, draw3: bool = False) -> State:
    """A new game; identical to kilix 95 Solitaire's new_game(seed)."""
    deck = [card(r, s) for s in range(4) for r in range(1, 14)]
    random.Random(seed).shuffle(deck)
    tableau = []
    for i in range(TABLEAU):
        pile = []
        for _ in range(i + 1):
            pile.append(deck.pop())
        tableau.append(tuple(pile))
    return State(stock=tuple(deck), tableau=tuple(tableau),
                 down=tuple(range(TABLEAU)), draw3=draw3, seed=seed)


# ---------------------------------------------------------------- rules

def can_found(c: int, state: State) -> bool:
    return state.found[suit_of(c)] == rank_of(c) - 1


def can_stack(c: int, onto: Optional[int]) -> bool:
    if onto is None:
        return rank_of(c) == 13
    return is_red(onto) != is_red(c) and rank_of(onto) == rank_of(c) + 1


def _valid_run(run) -> bool:
    return bool(run) and all(
        is_red(a) != is_red(b) and rank_of(a) == rank_of(b) + 1
        for a, b in zip(run, run[1:]))


def _tab_top(state: State, i: int) -> Optional[int]:
    return state.tableau[i][-1] if state.tableau[i] else None


def legal_moves(state: State) -> list:
    """Every legal move, in a stable order."""
    moves = []
    if state.stock or state.waste:
        moves.append(Move(DEAL))
    w = state.waste_top()
    if w is not None:
        if can_found(w, state):
            moves.append(Move(WASTE_FOUND))
        for j in range(TABLEAU):
            if can_stack(w, _tab_top(state, j)):
                moves.append(Move(WASTE_TAB, dst=j))
    for i in range(TABLEAU):
        up = state.face_up(i)
        if not up:
            continue
        if can_found(up[-1], state):
            moves.append(Move(TAB_FOUND, src=i))
        for n in range(1, len(up) + 1):
            run = up[-n:]
            if not _valid_run(run):
                break
            for j in range(TABLEAU):
                if j != i and can_stack(run[0], _tab_top(state, j)):
                    moves.append(Move(TAB_TAB, src=i, dst=j, count=n))
    for s in range(4):
        top = state.found_top(s)
        if top is None:
            continue
        for j in range(TABLEAU):
            if can_stack(top, _tab_top(state, j)):
                moves.append(Move(FOUND_TAB, src=s, dst=j))
    return moves


def is_legal(state: State, move: Move) -> bool:
    return move in legal_moves(state)


class IllegalMove(ValueError):
    pass


def _with_tab(state: State, i: int, pile: tuple) -> tuple:
    """(tableau, down) after replacing pile i, turning its new top up."""
    tableau = list(state.tableau)
    down = list(state.down)
    tableau[i] = pile
    down[i] = min(down[i], max(len(pile) - 1, 0))
    return tuple(tableau), tuple(down)


def apply(state: State, move: Move, check: bool = True) -> State:
    """The position after `move`. Raises IllegalMove if it is not legal.

    check=False skips the legality test, for search code that only ever
    applies moves taken from legal_moves() of this same state."""
    if check and not is_legal(state, move):
        raise IllegalMove(str(move))
    step = state.moves + 1
    if move.kind == DEAL:
        if state.stock:
            n = min(3 if state.draw3 else 1, len(state.stock))
            dealt = tuple(reversed(state.stock[-n:]))
            return replace(state, stock=state.stock[:-n], waste=state.waste + dealt,
                           fan=n, moves=step)
        return replace(state, stock=tuple(reversed(state.waste)), waste=(),
                       fan=0, moves=step)

    found = list(state.found)
    if move.kind in (WASTE_FOUND, WASTE_TAB):
        c = state.waste[-1]
        waste = state.waste[:-1]
        fan = state.fan - 1 if state.fan > 1 else state.fan
        if move.kind == WASTE_FOUND:
            found[suit_of(c)] += 1
            return replace(state, waste=waste, found=tuple(found), fan=fan, moves=step)
        tableau, down = _with_tab(state, move.dst, state.tableau[move.dst] + (c,))
        return replace(state, waste=waste, tableau=tableau, down=down, fan=fan,
                       moves=step)

    if move.kind == TAB_FOUND:
        pile = state.tableau[move.src]
        found[suit_of(pile[-1])] += 1
        tableau, down = _with_tab(state, move.src, pile[:-1])
        return replace(state, tableau=tableau, down=down, found=tuple(found),
                       moves=step)

    if move.kind == TAB_TAB:
        pile = state.tableau[move.src]
        run = pile[-move.count:]
        tableau, down = _with_tab(state, move.src, pile[:-move.count])
        interim = replace(state, tableau=tableau, down=down)
        tableau, down = _with_tab(interim, move.dst, interim.tableau[move.dst] + run)
        return replace(state, tableau=tableau, down=down, moves=step)

    # FOUND_TAB
    c = state.found_top(move.src)
    found[move.src] -= 1
    tableau, down = _with_tab(state, move.dst, state.tableau[move.dst] + (c,))
    return replace(state, tableau=tableau, down=down, found=tuple(found), moves=step)


def check_invariants(state: State) -> None:
    """Raise AssertionError if the position is not a legal Klondike state."""
    seen = list(state.stock) + list(state.waste)
    for i in range(TABLEAU):
        seen += list(state.tableau[i])
        assert 0 <= state.down[i] <= max(len(state.tableau[i]) - 1, 0), \
            f"T{i + 1} down count {state.down[i]} for {len(state.tableau[i])} cards"
        assert _valid_run(state.face_up(i)) or not state.face_up(i), \
            f"T{i + 1} face-up cards are not a valid run"
    for s in range(4):
        assert 0 <= state.found[s] <= 13
        seen += [card(r, s) for r in range(1, state.found[s] + 1)]
    assert sorted(seen) == list(range(52)), "every card exactly once"
