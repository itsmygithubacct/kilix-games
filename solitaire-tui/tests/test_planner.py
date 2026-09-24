import random
import unittest

from solitaire_tui import engine, planner
from solitaire_tui.engine import FOUND_TAB, TAB_TAB, Move, State, card
from solitaire_tui.sim import position_key


def hidden_cards(s: State) -> list:
    out = list(s.stock) + list(s.waste[:-1])
    for i in range(engine.TABLEAU):
        out += s.tableau[i][:s.down[i]]
    return sorted(out)


class RedealTest(unittest.TestCase):
    def test_keeps_the_visible_table_and_the_hidden_set(self):
        s = engine.deal(11)
        for _ in range(9):                         # some waste, some play
            s = engine.apply(s, Move(engine.DEAL))
        for k in range(20):
            w = planner.redeal_hidden(s, random.Random(k))
            self.assertEqual(w.found, s.found)
            self.assertEqual(w.down, s.down)
            self.assertEqual(w.waste_top(), s.waste_top())
            self.assertEqual([w.face_up(i) for i in range(7)], [s.face_up(i) for i in range(7)])
            self.assertEqual(hidden_cards(w), hidden_cards(s))
            engine.check_invariants(w)
            # the planner's seed reads only what a player can see
            self.assertEqual(planner._visible_key(w), planner._visible_key(s))

    def test_visible_key_is_stable_across_processes(self):
        # blake2b over the visible parts, never hash(): hash(None) varied
        # between processes before Python 3.12. A literal pins it.
        self.assertEqual(planner._visible_key(engine.deal(1)), 693816595)


class FilterTest(unittest.TestCase):
    def test_sensible_moves_drop_foundation_pulls_and_idle_shuffles(self):
        for seed in range(1, 40):
            s = engine.deal(seed)
            for _ in range(60):
                legal = engine.legal_moves(s)
                if not legal:
                    break
                for m in planner.sensible_moves(s, legal):
                    self.assertNotEqual(m.kind, FOUND_TAB)
                    if m.kind == TAB_TAB:
                        up = s.face_up(m.src)
                        whole = m.count == len(up)
                        self.assertTrue(
                            (whole and s.down[m.src] > 0)
                            or (whole and planner._movable_king(s, m.src))
                            or (not whole and engine.can_found(up[-m.count - 1], s)), m)
                s = engine.apply(s, random.Random(seed).choice(legal))

    def test_partial_run_that_frees_a_card_for_home_is_kept(self):
        ace, five, four = card(1, 0), card(5, 1), card(4, 0)
        six = card(6, 0)
        # column 0: A♠ then a red 5 / black 4 run; column 1: black 6
        s = State(stock=tuple(c for c in range(52) if c not in (ace, five, four, six)),
                  waste=(), found=(0, 0, 0, 0),
                  tableau=((ace, five, four), (six,), (), (), (), (), ()),
                  down=(0,) * 7)
        legal = engine.legal_moves(s)
        run = Move(TAB_TAB, src=0, dst=1, count=2)
        self.assertIn(run, legal)
        self.assertIn(run, planner.sensible_moves(s, legal))

    def test_candidates_skip_positions_already_reached(self):
        s = engine.deal(3)
        legal = engine.legal_moves(s)
        deal = Move(engine.DEAL)
        after = engine.apply(s, deal)
        seen = {position_key(s): 1, position_key(after): 1}
        # a deal may revisit once (a stock cycle), other moves never
        self.assertIn(deal, planner.candidate_moves(s, legal, seen))
        seen[position_key(after)] = 2
        self.assertNotIn(deal, planner.candidate_moves(s, legal, seen) or [None])


class PlannerTest(unittest.TestCase):
    def test_deterministic_and_legal(self):
        runs = []
        for _ in range(2):
            agent = planner.PlannerAgent(2, 20)
            s = engine.deal(5)
            moves = []
            for _ in range(25):
                legal = engine.legal_moves(s)
                m = agent.choose(s, legal)
                if m is None:
                    break
                self.assertIn(m, legal)
                moves.append(m)
                s = engine.apply(s, m)
            runs.append(moves)
        self.assertEqual(runs[0], runs[1])

    def test_unchecked_apply_matches_checked(self):
        s = engine.deal(9)
        for m in engine.legal_moves(s):
            self.assertEqual(engine.apply(s, m, check=False), engine.apply(s, m))


if __name__ == "__main__":
    unittest.main()
