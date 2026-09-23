import random
import unittest

from solitaire_tui import actions, agents, observe, sim
from solitaire_tui.engine import apply, deal, legal_moves


class ActionSpaceTest(unittest.TestCase):
    def test_bijection(self):
        self.assertEqual(actions.ACTION_COUNT, 681)
        for i in range(actions.ACTION_COUNT):
            self.assertEqual(actions.encode(actions.decode(i)), i)

    def test_mask_matches_rules(self):
        for seed in range(20):
            s, rng = deal(seed), random.Random(seed)
            for _ in range(100):
                legal = legal_moves(s)
                mask = actions.legal_mask(s)
                self.assertEqual(sum(mask), len(legal))
                for m in legal:
                    self.assertTrue(mask[actions.encode(m)])
                s = apply(s, rng.choice(legal))


class ObservationTest(unittest.TestCase):
    def test_size_and_no_hidden_leak(self):
        s = deal(5)
        obs = observe.observe(s)
        self.assertEqual(len(obs), observe.OBSERVATION_SIZE)
        hidden = [c for c in range(52) if obs[c * observe.PER_CARD] == 1.0]
        # 21 face-down tableau cards + 24 stock cards are hidden at the deal
        self.assertEqual(len(hidden), 45)
        # the observation must not depend on the order of hidden cards
        s2 = s.__class__(**{**s.__dict__, "stock": tuple(reversed(s.stock))})
        self.assertEqual(observe.observe(s2), obs)


class AgentTest(unittest.TestCase):
    def test_policy_agent_masks(self):
        first_legal = lambda obs, mask: mask.index(True)          # noqa: E731
        r = sim.play(agents.PolicyAgent(first_legal), seed=1, max_steps=200)
        self.assertIn(r.reason, ("won", "stuck", "step-limit", "resigned"))
        with self.assertRaises(ValueError):
            sim.play(agents.PolicyAgent(lambda obs, mask: mask.index(False)), seed=1)

    def test_greedy_baseline_and_determinism(self):
        a = sim.tournament(agents.GreedyAgent, range(1, 101))
        b = sim.tournament(agents.GreedyAgent, range(1, 101))
        self.assertEqual(a, b)
        self.assertGreater(a["wins"], 0)

    def test_trajectory_recording(self):
        r = sim.play(agents.GreedyAgent(), seed=2, record=True)
        self.assertEqual(len(r.trajectory), r.steps)
        obs, action = r.trajectory[0]
        self.assertEqual(len(obs), observe.OBSERVATION_SIZE)
        self.assertTrue(0 <= action < actions.ACTION_COUNT)


if __name__ == "__main__":
    unittest.main()
