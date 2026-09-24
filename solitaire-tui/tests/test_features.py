import random
import unittest

from solitaire_tui import engine, features, planner
from solitaire_tui.engine import Move


class FeatureTest(unittest.TestCase):
    def test_no_hidden_card_leaks(self):
        # A player cannot tell two tables apart that differ only in hidden
        # cards, so neither may the features -- for every legal move.
        for seed in range(1, 25):
            rng = random.Random(seed)
            s = engine.deal(seed)
            for _ in range(rng.randrange(0, 40)):
                s = engine.apply(s, rng.choice(engine.legal_moves(s)))
            h = features.History()
            h.see(s)
            for k in range(4):
                w = planner.redeal_hidden(s, random.Random(k))
                hw = features.History()
                hw.see(w)
                for m in engine.legal_moves(s):
                    self.assertEqual(features.move_features(s, m, h),
                                     features.move_features(w, m, hw), (seed, m))

    def test_history_counts_idle_deals_and_repeats(self):
        s = engine.deal(4)
        h = features.History()
        h.see(s)
        for n in range(1, 4):
            s = engine.apply(s, Move(engine.DEAL))
            h.see(s)
            self.assertEqual(h.idle, n)
        s2 = engine.apply(s, Move(engine.DEAL))
        f = features.move_features(s, Move(engine.DEAL), h)
        self.assertEqual(f[features.NAMES.index("after_seen")], 0.0)
        h.see(s2)
        # any non-deal change resets the idle count
        other = [m for m in engine.legal_moves(s2) if m.kind != engine.DEAL]
        if other:
            h.see(engine.apply(s2, other[0]))
            self.assertEqual(h.idle, 0)

    def test_width_matches_names(self):
        s = engine.deal(1)
        h = features.History()
        h.see(s)
        for m in engine.legal_moves(s):
            self.assertEqual(len(features.move_features(s, m, h)), features.FEATURE_COUNT)
        self.assertEqual(features.FEATURE_COUNT, len(features.NAMES))


if __name__ == "__main__":
    unittest.main()
