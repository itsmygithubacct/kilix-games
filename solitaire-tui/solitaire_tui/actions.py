"""A fixed, finite action space over Klondike moves, for learned policies.

A policy network needs a constant-size output. Every move the engine can
ever produce maps to one index in 0..ACTION_COUNT-1, and each index maps back
to exactly one Move:

    0              deal (or recycle the waste when the stock is empty)
    1              waste -> foundation
    2..8           waste -> tableau j
    9..15          tableau i -> foundation
    16..652        tableau i -> tableau j, moving the top n cards
                   (i, j in 0..6; n in 1..13; i == j is never legal)
    653..680       foundation suit s -> tableau j

`legal_mask(state)` gives a boolean per index, so a policy masks illegal
outputs before its argmax or sampling.
"""
from __future__ import annotations

from .engine import (DEAL, FOUND_TAB, TAB_FOUND, TAB_TAB, TABLEAU, WASTE_FOUND,
                     WASTE_TAB, Move, State, legal_moves)

MAX_RUN = 13
_WASTE_TAB = 2
_TAB_FOUND = _WASTE_TAB + TABLEAU
_TAB_TAB = _TAB_FOUND + TABLEAU
_FOUND_TAB = _TAB_TAB + TABLEAU * TABLEAU * MAX_RUN
ACTION_COUNT = _FOUND_TAB + 4 * TABLEAU          # 681


def encode(move: Move) -> int:
    if move.kind == DEAL:
        return 0
    if move.kind == WASTE_FOUND:
        return 1
    if move.kind == WASTE_TAB:
        return _WASTE_TAB + move.dst
    if move.kind == TAB_FOUND:
        return _TAB_FOUND + move.src
    if move.kind == TAB_TAB:
        if not 1 <= move.count <= MAX_RUN:
            raise ValueError(f"run length {move.count} out of range")
        return _TAB_TAB + (move.src * TABLEAU + move.dst) * MAX_RUN + (move.count - 1)
    if move.kind == FOUND_TAB:
        return _FOUND_TAB + move.src * TABLEAU + move.dst
    raise ValueError(f"unknown move kind {move.kind!r}")


def decode(index: int) -> Move:
    if not 0 <= index < ACTION_COUNT:
        raise ValueError(f"action {index} out of range")
    if index == 0:
        return Move(DEAL)
    if index == 1:
        return Move(WASTE_FOUND)
    if index < _TAB_FOUND:
        return Move(WASTE_TAB, dst=index - _WASTE_TAB)
    if index < _TAB_TAB:
        return Move(TAB_FOUND, src=index - _TAB_FOUND)
    if index < _FOUND_TAB:
        pair, n = divmod(index - _TAB_TAB, MAX_RUN)
        src, dst = divmod(pair, TABLEAU)
        return Move(TAB_TAB, src=src, dst=dst, count=n + 1)
    s, dst = divmod(index - _FOUND_TAB, TABLEAU)
    return Move(FOUND_TAB, src=s, dst=dst)


def legal_actions(state: State) -> list:
    return [encode(m) for m in legal_moves(state)]


def legal_mask(state: State) -> list:
    mask = [False] * ACTION_COUNT
    for index in legal_actions(state):
        mask[index] = True
    return mask
