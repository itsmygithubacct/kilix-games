"""The neural player: a trained network, read and run in pure Python.

The weights ship as a kilix-game-kit policy blob (format KXPOLICY v1, the
same bytes the C runtime `kilix_game_policy` loads; see that header for the
layout). This module is a standard-library reader for it, so the game keeps
no dependencies. Load refuses anything the C loader refuses: foreign magic,
another version, an impossible shape, a size that does not match, a digest
mismatch, a non-finite value.

The network scores one move at a time: its input is
`features.move_features(state, move)`, its output a single number. The
player scores each of `planner.candidate_moves` -- the same sensible,
non-revisiting moves its teacher chose from in training -- and plays the
best. The network is tiny (tens of inputs, one output), so a move costs well
under a millisecond.
"""
from __future__ import annotations

import math
import struct
from pathlib import Path
from typing import Optional, Sequence

from . import features
from .engine import Move, State
from .planner import candidate_moves

MAGIC = b"KXPOLICY"
VERSION = 1
MAX_LAYERS = 8
MAX_WIDTH = 256
BLOB = Path(__file__).resolve().parent / "data" / "solitaire-policy.kxpol"


class PolicyError(ValueError):
    pass


def fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


class Policy:
    """A dense ReLU network from a KXPOLICY v1 blob."""

    def __init__(self, blob: bytes):
        if len(blob) < 16 or blob[:8] != MAGIC:
            raise PolicyError("not a policy blob")
        version, layers = struct.unpack_from("<II", blob, 8)
        if version != VERSION:
            raise PolicyError(f"unsupported policy version {version}")
        if not 1 <= layers <= MAX_LAYERS:
            raise PolicyError("layer count out of bounds")
        if len(blob) < 16 + 4 * (layers + 1) + 4 + 8:
            raise PolicyError("truncated header")
        widths = list(struct.unpack_from(f"<{layers + 1}I", blob, 16))
        if any(not 1 <= w <= MAX_WIDTH for w in widths):
            raise PolicyError(f"shape out of bounds: {widths}")
        header = 16 + 4 * (layers + 1) + 4
        count = sum(widths[i] * widths[i + 1] + widths[i + 1] for i in range(layers))
        if len(blob) != header + 4 * count + 8:
            raise PolicyError("size does not match the shape")
        (stored,) = struct.unpack_from("<Q", blob, len(blob) - 8)
        if fnv1a64(blob[:-8]) != stored:
            raise PolicyError("digest mismatch")
        (self.temperature,) = struct.unpack_from("<f", blob, header - 4)
        values = struct.unpack_from(f"<{count}f", blob, header)
        if not all(map(math.isfinite, values)) or not (
                math.isfinite(self.temperature) and self.temperature > 0):
            raise PolicyError("non-finite value")
        self.widths = widths
        self.layers = []                       # (rows [out][in], biases [out])
        at = 0
        for i in range(layers):
            fan_in, fan_out = widths[i], widths[i + 1]
            rows = [values[at + r * fan_in: at + (r + 1) * fan_in] for r in range(fan_out)]
            at += fan_in * fan_out
            self.layers.append((rows, list(values[at: at + fan_out])))
            at += fan_out

    @classmethod
    def load(cls, path: Optional[Path] = None) -> "Policy":
        """Reads `path`, by default the shipped BLOB (looked up at call time)."""
        return cls(Path(path if path is not None else BLOB).read_bytes())

    def forward(self, inputs: Sequence[float]) -> list:
        if len(inputs) != self.widths[0]:
            raise PolicyError(f"expected {self.widths[0]} inputs, got {len(inputs)}")
        x = list(inputs)
        last = len(self.layers) - 1
        for i, (rows, biases) in enumerate(self.layers):
            x = [b + sum(map(float.__mul__, row, x)) for row, b in zip(rows, biases)]
            if i < last:
                x = [v if v > 0.0 else 0.0 for v in x]
        return x


class NeuralAgent:
    """Plays with the network. Draw one only: that is what it learned."""
    name = "neural"

    def __init__(self, policy: Optional[Policy] = None):
        self.policy = policy or Policy.load()
        self.history = features.History()

    def choose(self, state: State, legal: Sequence[Move]) -> Optional[Move]:
        if not legal:
            return None
        self.history.see(state)
        return best_move(self.policy, state, legal, self.history)


def best_move(policy: Policy, state: State, legal: Sequence[Move],
              history: "features.History") -> Optional[Move]:
    """The network's choice among the candidates. `history` must have seen
    every position of this game, the current one included."""
    pool = candidate_moves(state, legal, history.visited)
    if len(pool) <= 1:
        return pool[0] if pool else None
    scores = [policy.forward(features.move_features(state, m, history))[0] for m in pool]
    return pool[max(range(len(pool)), key=scores.__getitem__)]   # first best on a tie


_cached: Optional[Policy] = None


def default_policy() -> Optional[Policy]:
    """The shipped network, loaded once; None when it is missing or damaged."""
    global _cached
    if _cached is None:
        try:
            net = Policy.load()
        except (OSError, PolicyError):
            return None
        if net.widths[0] != features.FEATURE_COUNT or net.widths[-1] != 1:
            return None                      # built for other features
        _cached = net
    return _cached
