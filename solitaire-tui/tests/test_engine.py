import random
import unittest

from solitaire_tui import engine
from solitaire_tui.engine import (DEAL, TAB_TAB, WASTE_FOUND, IllegalMove, Move,
                                  apply, card, deal, legal_moves)


def kilix95_deal(seed):
    """sol.py's new_game, verbatim in behaviour: suit-major deck, shuffle, pop."""
    deck = [(r, s) for s in range(4) for r in range(1, 14)]
    random.Random(seed).shuffle(deck)
    tab = [[] for _ in range(7)]
    for i in range(7):
        for _ in range(i + 1):
            tab[i].append(deck.pop())
    return deck, tab


class DealTest(unittest.TestCase):
    def test_same_deal_as_kilix95(self):
        for seed in (0, 1, 7, 12345):
            stock, tab = kilix95_deal(seed)
            s = deal(seed)
            self.assertEqual(s.stock, tuple(card(r, u) for r, u in stock))
            for i in range(7):
                self.assertEqual(s.tableau[i], tuple(card(r, u) for r, u in tab[i]))
            self.assertEqual(s.down, tuple(range(7)))

    def test_invariants_under_random_play(self):
        for seed in range(30):
            s, rng = deal(seed), random.Random(seed)
            for _ in range(250):
                s = apply(s, rng.choice(legal_moves(s)))
                engine.check_invariants(s)


class RuleTest(unittest.TestCase):
    def test_deal_three_and_recycle(self):
        s = deal(3, draw3=True)
        top3 = s.stock[-3:]
        s = apply(s, Move(DEAL))
        self.assertEqual(s.waste, tuple(reversed(top3)))
        self.assertEqual(s.fan, 3)
        while s.stock:
            s = apply(s, Move(DEAL))
        before = s.waste
        s = apply(s, Move(DEAL))                      # recycle
        self.assertEqual(s.stock, tuple(reversed(before)))
        self.assertEqual(s.waste, ())

    def test_illegal_move_raises_and_state_is_immutable(self):
        s = deal(1)
        with self.assertRaises(IllegalMove):
            apply(s, Move(TAB_TAB, src=0, dst=0, count=1))
        t = apply(s, Move(DEAL))
        self.assertEqual(s.waste, ())
        self.assertEqual(len(t.waste), 1)

    def test_exposed_card_turns_up_and_win(self):
        # a hand-built near-win: only the K of clubs is left, on the tableau
        tableau = ((),) * 6 + ((card(13, 3),),)
        s = engine.State(found=(13, 13, 13, 12), tableau=tableau, down=(0,) * 7)
        engine.check_invariants(s)
        s = apply(s, Move(engine.TAB_FOUND, src=6))
        self.assertTrue(s.won)

    def test_king_only_on_empty_column_and_alternating_colours(self):
        self.assertTrue(engine.can_stack(card(13, 0), None))
        self.assertFalse(engine.can_stack(card(12, 0), None))
        self.assertTrue(engine.can_stack(card(6, 1), card(7, 0)))   # red 6 on black 7
        self.assertFalse(engine.can_stack(card(6, 1), card(7, 2)))  # red on red

    def test_waste_found_only_when_legal(self):
        s = deal(1)
        for m in legal_moves(s):
            if m.kind == WASTE_FOUND:
                self.fail("no waste before the first deal")


if __name__ == "__main__":
    unittest.main()
