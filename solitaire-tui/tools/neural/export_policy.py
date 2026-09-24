#!/usr/bin/env python3
"""Pack weights written by `solnet.py train` into the game's policy blob.

  export_policy.py MODEL.npz OUT.kxpol

Uses kilix-game-kit's own packer (tools/kilix_policy.py from the monorepo's
SDK checkout), so the blob is exactly what the C runtime would load, then
reads it back with the game's pure-Python reader and checks it reproduces
the float32 numpy network on sample observations.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

GAME = Path(__file__).resolve().parents[2]
SDK_TOOLS = GAME.parent / "third_party" / "kilix-game-sdk" / "kilix-game-kit" / "tools"
sys.path.insert(0, str(GAME))
sys.path.insert(0, str(SDK_TOOLS))

import kilix_policy                                      # noqa: E402
from solitaire_tui import engine, features               # noqa: E402
from solitaire_tui.policy import Policy                  # noqa: E402


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    model, out = Path(sys.argv[1]), Path(sys.argv[2])
    data = np.load(model)
    if int(data["feature_version"]) != features.FEATURE_VERSION:
        raise SystemExit("model was trained on another feature version")
    count = int(data["layers"])
    weights = [data[f"w{i}"].astype("<f4") for i in range(count)]
    biases = [data[f"b{i}"].astype("<f4") for i in range(count)]
    widths = [weights[0].shape[1]] + [w.shape[0] for w in weights]
    raw = b"".join(w.tobytes() + b.tobytes() for w, b in zip(weights, biases))
    blob = kilix_policy.pack(widths, raw)
    out.write_bytes(blob)
    info = kilix_policy.inspect(blob)

    # The pure-Python reader must agree with float32 numpy on real candidates.
    policy = Policy(blob)
    worst = 0.0
    for seed in range(1, 21):
        state = engine.deal(seed)
        history = features.History()
        history.see(state)
        for move in engine.legal_moves(state):
            row = features.move_features(state, move, history)
            x = np.asarray(row, dtype=np.float32)
            for i, (w, b) in enumerate(zip(weights, biases)):
                x = x @ w.T + b
                if i < count - 1:
                    x = np.maximum(x, 0.0)
            worst = max(worst, abs(policy.forward(row)[0] - float(x[0])))
    if worst > 1e-3:
        raise SystemExit(f"python reader disagrees with numpy by {worst}")
    info["max_abs_score_difference"] = worst
    info["source"] = model.name
    print(json.dumps(info))
    return 0


if __name__ == "__main__":
    sys.exit(main())
