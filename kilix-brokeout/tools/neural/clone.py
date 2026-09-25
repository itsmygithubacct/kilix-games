#!/usr/bin/env python3
"""Behaviour-clone a player from brokeout-lab demonstrations (the warm start for
evolution strategies; the demonstrations come from the lab's tunnel aimer).

    clone.py DEMO.bin [MORE.bin ...] --out OUT.raw [--hidden 32] [--epochs 8]
             [--stride N] [--seed 1]

DEMO.bin is `brokeout-lab --dump`: per tick, 38 float32 features then an int32
action. OUT.raw is the network's float32 parameters in KXPOLICY order (per
layer: weights [out][in], then biases), which brokeout-lab reads with
--weights/--init. Needs numpy.
"""
import argparse
import sys

import numpy as np

FEATURES, ACTIONS = 38, 9


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("demo", nargs="+", help="one or more demonstration files")
    ap.add_argument("--out", required=True)
    ap.add_argument("--hidden", type=int, default=32)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--batch", type=int, default=4096)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--stride", type=int, default=1, help="keep every Nth tick")
    a = ap.parse_args()

    rec = np.dtype([("x", "<f4", FEATURES), ("a", "<i4")])
    parts = [np.memmap(path, dtype=rec, mode="r")[::a.stride] for path in a.demo]
    x = np.concatenate([p["x"] for p in parts]).astype(np.float32)
    y = np.concatenate([p["a"] for p in parts]).astype(np.int64)
    print(f"{len(y)} samples; action shares {np.bincount(y, minlength=ACTIONS) / len(y)}",
          file=sys.stderr)
    rng = np.random.default_rng(a.seed)
    order = rng.permutation(len(y))
    x, y = x[order], y[order]
    split = len(y) * 9 // 10
    widths = [FEATURES, a.hidden, a.hidden, ACTIONS]
    params = []
    for fan_in, fan_out in zip(widths, widths[1:]):
        params.append([rng.normal(0, np.sqrt(2 / fan_in), (fan_out, fan_in)).astype(np.float32),
                       np.zeros(fan_out, np.float32)])
    m = [[np.zeros_like(p) for p in layer] for layer in params]
    v = [[np.zeros_like(p) for p in layer] for layer in params]
    step = 0

    def forward(xb):
        acts = [xb]
        h = xb
        for i, (w, b) in enumerate(params):
            h = h @ w.T + b
            if i < len(params) - 1:
                h = np.maximum(h, 0)
            acts.append(h)
        return h, acts

    for epoch in range(a.epochs):
        perm = rng.permutation(split)
        for start in range(0, split, a.batch):
            idx = perm[start:start + a.batch]
            out, acts = forward(x[idx])
            out -= out.max(axis=1, keepdims=True)
            p = np.exp(out)
            p /= p.sum(axis=1, keepdims=True)
            grad = p
            grad[np.arange(len(idx)), y[idx]] -= 1
            grad /= len(idx)
            step += 1
            for i in range(len(params) - 1, -1, -1):
                w = params[i][0]
                gw, gb = grad.T @ acts[i], grad.sum(axis=0)
                if i:
                    grad = (grad @ w) * (acts[i] > 0)
                for j, g in enumerate((gw, gb)):
                    m[i][j] = 0.9 * m[i][j] + 0.1 * g
                    v[i][j] = 0.999 * v[i][j] + 0.001 * g * g
                    params[i][j] -= (a.lr * (m[i][j] / (1 - 0.9 ** step)) /
                                     (np.sqrt(v[i][j] / (1 - 0.999 ** step)) + 1e-8)).astype(np.float32)
        out, _ = forward(x[split:])
        acc = float((out.argmax(axis=1) == y[split:]).mean())
        print(f"epoch {epoch + 1}: validation accuracy {acc:.4f}", file=sys.stderr)
    raw = b"".join(w.astype("<f4").tobytes() + b.astype("<f4").tobytes() for w, b in params)
    with open(a.out, "wb") as fh:
        fh.write(raw)


if __name__ == "__main__":
    main()
