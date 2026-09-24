import struct
import unittest

from solitaire_tui import engine, features, policy, sim, tui
from solitaire_tui.engine import Move


def pack(widths, values, temperature=1.0) -> bytes:
    """An independent KXPOLICY v1 writer (the SDK tool is the reference)."""
    body = (b"KXPOLICY" + struct.pack("<II", 1, len(widths) - 1)
            + struct.pack(f"<{len(widths)}I", *widths) + struct.pack("<f", temperature)
            + struct.pack(f"<{len(values)}f", *values))
    return body + struct.pack("<Q", policy.fnv1a64(body))


# 2 -> 3 -> 2, the kilix-game-kit reference network and digest
REF = [1, -1, 0.5, 0.5, -2, 1, 0, 0.25, -0.5, 1, 2, -1, -1, 0.5, 3, 0.1, -0.2]


class ReaderTest(unittest.TestCase):
    def test_reference_blob_digest_and_forward(self):
        blob = pack([2, 3, 2], REF, 1.5)
        # the digest the C and Python SDK tests assert for this blob
        self.assertEqual(struct.unpack_from("<Q", blob, len(blob) - 8)[0],
                         0xc08fcc42e5238118)
        net = policy.Policy(blob)
        # Layer 0 weights [[1,-1],[.5,.5],[-2,1]], biases [0,.25,-.5]:
        # hidden = relu([1, 1.75, -3.5]) = [1, 1.75, 0]. Layer 1 weights
        # [[1,2,-1],[-1,.5,3]], biases [.1,-.2]: out = [4.6, -0.325], the
        # values kilix-game-kit's C test asserts for this blob.
        out = net.forward([2.0, 1.0])
        self.assertAlmostEqual(out[0], 4.6, places=6)
        self.assertAlmostEqual(out[1], -0.325, places=6)
        self.assertAlmostEqual(net.temperature, 1.5)

    def test_refuses_damage(self):
        blob = pack([2, 3, 2], REF)
        cases = {
            "magic": b"KXPOLICZ" + blob[8:],
            "truncated": blob[:-1],
            "trailing": blob + b"\0",
            "flipped": blob[:40] + bytes([blob[40] ^ 1]) + blob[41:],
            "version": pack([2, 3, 2], REF)[:8] + struct.pack("<I", 2) + blob[12:],
            "nan": pack([2, 3, 2], [float("nan")] + REF[1:]),
            "too wide": pack([257, 1], [0.0] * 258),
        }
        for name, bad in cases.items():
            with self.subTest(name):
                with self.assertRaises(policy.PolicyError):
                    policy.Policy(bad)
        policy.Policy(pack([256, 1], [0.0] * 257))          # the SDK's bound itself loads


class ShippedPlayerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.net = policy.default_policy()
        if cls.net is None:
            raise AssertionError(f"shipped policy missing or damaged: {policy.BLOB}")

    def test_shape_matches_the_game(self):
        self.assertEqual(self.net.widths[0], features.FEATURE_COUNT)
        self.assertEqual(self.net.widths[-1], 1)

    def test_plays_legal_games(self):
        for seed in (1, 2):
            agent = policy.NeuralAgent(self.net)
            result = sim.play(agent, seed)                 # apply() checks legality
            self.assertIn(result.reason, ("won", "resigned", "stuck"))


class TuiPlayerTest(unittest.TestCase):
    def test_hint_is_neural_in_deal_one(self):
        g = tui.Game.new(7)
        g.show_hint()
        self.assertTrue(g.message.startswith("hint (neural): "), g.message)
        g3 = tui.Game.new(7, draw3=True)
        g3.show_hint()
        self.assertTrue(g3.message.startswith("hint (greedy): "), g3.message)

    def test_autoplay_moves_and_any_key_stops_it(self):
        g = tui.Game.new(7)
        g.key(ord("p"))
        self.assertTrue(g.autoplay)
        before = g.state
        g.autoplay_step()
        self.assertNotEqual(g.state, before)
        self.assertEqual(len(g.undo), 1)                   # undo still works
        g.key(ord("x"))
        self.assertFalse(g.autoplay)
        g.autoplay_step()                                  # stopped: no move
        self.assertEqual(len(g.undo), 1)

    def test_autoplay_refuses_deal_three(self):
        g = tui.Game.new(7, draw3=True)
        g.toggle_autoplay()
        self.assertFalse(g.autoplay)

    def test_neural_move_matches_the_agent(self):
        g = tui.Game.new(12)
        agent = policy.NeuralAgent(policy.default_policy())
        for _ in range(15):
            move, who = g.advice()
            self.assertEqual(who, "neural")
            expected = agent.choose(g.state, engine.legal_moves(g.state))
            self.assertEqual(move, expected)
            if move is None:
                break
            g.play(move)


if __name__ == "__main__":
    unittest.main()
