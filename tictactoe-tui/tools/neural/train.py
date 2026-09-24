#!/usr/bin/env python3
"""Train tictactoe-tui's neural opponent and install it into the game.

    python3 tools/neural/train.py [--seed N] [--hidden 128] [--max-epochs N]

Tic-tac-toe is small enough to label exactly. Every position reachable from
an empty board (4,520 with a move to make) is solved by minimax here, and
each legal move gets a target value for the player making it:

    win  in d plies:  1 - 0.08 * (d - 1)    (faster wins score higher)
    draw:             0
    loss in d plies: -(1 - 0.08 * (d - 1))  (slower losses score higher)

A small ReLU network (18 inputs: the mover's marks, then the opponent's;
9 outputs: one value per cell) is fit with full-batch Adam to two terms:
masked mean squared error against those values (so the outputs mean
something, which the game's EASY and NORMAL levels sample from), plus
cross-entropy of a softmax over the legal moves toward the best moves
(optimal, and fastest when winning), which is what makes the argmax exact. Features are relative to the mover, so one
network plays X and O, and games where O starts are the same positions.

Gate (all must hold, checked exhaustively before anything is written):
  1. at every position the network's best legal move is game-theoretically
     optimal (it never turns a win into a draw or a draw into a loss);
  2. when a win is available it picks a fastest win.
The game's `--neural-test` re-proves both in C against its own solver.

Writes assets/policy/tictactoe-neural.kxpol (kilix-game-kit KXPOLICY v1,
through the SDK's own packer), src/neural_policy_blob.h and
docs/neural-policy-provenance.json.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from functools import lru_cache
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT / "third_party" / "kilix-game-kit" / "tools" / "kilix_policy.py"
BLOB = ROOT / "assets" / "policy" / "tictactoe-neural.kxpol"
HEADER = ROOT / "src" / "neural_policy_blob.h"
MANIFEST = ROOT / "docs" / "neural-policy-provenance.json"
sys.path.insert(0, str(KIT.parent))
import kilix_policy  # noqa: E402

LINES = [(0, 1, 2), (3, 4, 5), (6, 7, 8), (0, 3, 6), (1, 4, 7), (2, 5, 8),
         (0, 4, 8), (2, 4, 6)]
FULL = 0x1FF
STEP = 0.08


def has_line(m: int) -> bool:
    return any(all(m >> c & 1 for c in line) for line in LINES)


def terminal(own: int, opp: int) -> bool:
    return has_line(own) or has_line(opp) or (own | opp) == FULL


@lru_cache(maxsize=None)
def value(own: int, opp: int) -> int:
    """For the mover: +1 win, 0 draw, -1 loss."""
    if has_line(opp):
        return -1
    if (own | opp) == FULL:
        return 0
    return max(-value(opp, own | 1 << c) for c in range(9) if not (own | opp) >> c & 1)


@lru_cache(maxsize=None)
def distance(own: int, opp: int) -> int:
    if terminal(own, opp):
        return 0
    best = value(own, opp)
    ds = [1 + distance(opp, own | 1 << c) for c in range(9)
          if not (own | opp) >> c & 1 and -value(opp, own | 1 << c) == best]
    return min(ds) if best > 0 else max(ds)


def positions():
    """(own, opp) for every reachable position with a move to make."""
    seen, stack, out = {(0, 0)}, [(0, 0)], []
    while stack:
        own, opp = stack.pop()
        if terminal(own, opp):
            continue
        out.append((own, opp))
        for c in range(9):
            if not (own | opp) >> c & 1:
                nxt = (opp, own | 1 << c)
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
    return sorted(out)


def dataset():
    rows = positions()
    x = np.zeros((len(rows), 18), np.float32)
    y = np.zeros((len(rows), 9), np.float32)
    mask = np.zeros((len(rows), 9), np.float32)
    best = np.zeros((len(rows), 9), np.float32)
    for i, (own, opp) in enumerate(rows):
        for c in range(9):
            x[i, c] = own >> c & 1
            x[i, 9 + c] = opp >> c & 1
            if (own | opp) >> c & 1:
                continue
            v = -value(opp, own | 1 << c)
            d = 1 + distance(opp, own | 1 << c)
            y[i, c] = v * (1.0 - STEP * (d - 1))
            mask[i, c] = 1.0
            if v == value(own, opp) and (v <= 0 or d == distance(own, opp)):
                best[i, c] = 1.0
    best /= best.sum(axis=1, keepdims=True)
    return rows, x, y, mask, best


def forward(params, x):
    h = x
    acts = [h]
    for i, (w, b) in enumerate(params):
        h = h @ w.T + b
        if i < len(params) - 1:
            h = np.maximum(h, 0.0)
        acts.append(h)
    return h, acts


def gate(params, rows, x, mask):
    out, _ = forward(params, x)
    out = np.where(mask > 0, out, -np.inf)
    picks = out.argmax(axis=1)
    not_optimal = slow_wins = 0
    for (own, opp), c in zip(rows, picks):
        best = value(own, opp)
        v = -value(opp, own | 1 << int(c))
        if v != best:
            not_optimal += 1
        elif best > 0 and 1 + distance(opp, own | 1 << int(c)) != distance(own, opp):
            slow_wins += 1
    return not_optimal, slow_wins


CE_WEIGHT = 0.1


def train(seed: int, hidden: int, max_epochs: int):
    rows, x, y, mask, best = dataset()
    rng = np.random.default_rng(seed)
    widths = [18, hidden, hidden, 9]
    params = []
    for fan_in, fan_out in zip(widths, widths[1:]):
        w = rng.normal(0, np.sqrt(2.0 / fan_in), (fan_out, fan_in)).astype(np.float32)
        params.append([w, np.zeros(fan_out, np.float32)])
    m = [[np.zeros_like(p) for p in layer] for layer in params]
    v = [[np.zeros_like(p) for p in layer] for layer in params]
    lr, b1, b2, eps = 3e-3, 0.9, 0.999, 1e-8
    count = mask.sum()
    history = []
    for epoch in range(1, max_epochs + 1):
        out, acts = forward(params, x)
        diff = (out - y) * mask
        logits = np.where(mask > 0, out, -1e9)
        logits -= logits.max(axis=1, keepdims=True)
        prob = np.exp(logits) * mask
        prob /= prob.sum(axis=1, keepdims=True)
        ce = float(-(best * np.log(prob + 1e-12)).sum() / len(rows))
        loss = float((diff ** 2).sum() / count)
        grad = 2.0 * diff / count + CE_WEIGHT * (prob - best) / len(rows)
        for i in range(len(params) - 1, -1, -1):
            w, _ = params[i]
            gw = grad.T @ acts[i]
            gb = grad.sum(axis=0)
            if i:
                grad = (grad @ w) * (acts[i] > 0)
            for j, g in enumerate((gw, gb)):
                m[i][j] = b1 * m[i][j] + (1 - b1) * g
                v[i][j] = b2 * v[i][j] + (1 - b2) * g * g
                mh = m[i][j] / (1 - b1 ** epoch)
                vh = v[i][j] / (1 - b2 ** epoch)
                params[i][j] -= (lr * mh / (np.sqrt(vh) + eps)).astype(np.float32)
        if epoch % 250 == 0:
            bad, slow = gate(params, rows, x, mask)
            history.append({"epoch": epoch, "mse": round(loss, 6), "ce": round(ce, 6),
                            "not_optimal": bad, "slow_wins": slow})
            print(f"epoch {epoch:5d} mse {loss:.5f} ce {ce:.4f} not-optimal {bad} "
                  f"slow-wins {slow}", flush=True)
            if bad == 0 and slow == 0 and epoch >= 2000:
                return params, widths, rows, loss, history
    raise SystemExit("gate not met within the epoch budget; nothing written")


def install(params, widths, rows, loss, history, seed, seconds):
    raw = b"".join(w.astype("<f4").tobytes() + b.astype("<f4").tobytes()
                   for w, b in params)
    blob = kilix_policy.pack(widths, raw)
    BLOB.parent.mkdir(parents=True, exist_ok=True)
    BLOB.write_bytes(blob)
    subprocess.run([sys.executable, "-B", str(KIT), "embed", str(BLOB), "--symbol",
                    "neural_policy_blob", str(HEADER)], check=True)
    info = kilix_policy.inspect(blob)
    manifest = {
        "artifact": "assets/policy/tictactoe-neural.kxpol",
        "embedded_header": "src/neural_policy_blob.h",
        "format": "kilix-game-kit KXPOLICY v1",
        "sha256": hashlib.sha256(blob).hexdigest(),
        "fnv1a64": info["fnv1a64"],
        "widths": widths,
        "parameters": info["parameters"],
        "license": "MIT, same as tictactoe-tui",
        "origin": ("Trained only on exact minimax values of tic-tac-toe computed by "
                   "tools/neural/train.py. No third-party data or weights."),
        "features": "18 floats: mover's marks on cells 0-8, then the opponent's",
        "outputs": "9 move values for the mover, about -1 (loss) .. +1 (win)",
        "targets": f"win/loss in d plies = +/-(1 - {STEP} * (d - 1)), draw = 0",
        "positions": len(rows),
        "training": {"seed": seed, "optimizer": "Adam lr 3e-3, full batch",
                     "loss": (f"masked MSE over legal moves + {CE_WEIGHT} x cross-entropy "
                              "toward the best moves"),
                     "final_mse": round(loss, 6), "hidden": widths[1],
                     "epochs": history[-1]["epoch"], "seconds": round(seconds, 1),
                     "numpy": np.__version__},
        "gate": {"positions_checked": len(rows), "not_optimal": 0, "slow_wins": 0,
                 "rule": ("best legal move optimal everywhere, fastest win when a win "
                          "exists; re-proved in C by tictactoe-tui --neural-test")},
        "history": history,
        "reproducibility": ("deterministic for one numpy build and seed; the shipped "
                            "bytes are pinned by sha256 and checked by make check-policy"),
    }
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n")
    print(json.dumps({k: manifest[k] for k in ("sha256", "widths", "parameters")}))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--hidden", type=int, default=128)
    ap.add_argument("--max-epochs", type=int, default=30000)
    a = ap.parse_args()
    start = time.time()
    params, widths, rows, loss, history = train(a.seed, a.hidden, a.max_epochs)
    install(params, widths, rows, loss, history, a.seed, time.time() - start)
    return 0


if __name__ == "__main__":
    sys.exit(main())
