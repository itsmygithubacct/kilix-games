#!/usr/bin/env python3
"""Fit a gunner network before evolution strategies (a warm start).

    fit-demos.py --aim AIM.bin --weapons DEMOS.bin --out MODEL.raw --hidden H
                 [--select-aim F] [--select-weapons F]

Both inputs are rows written by bashed-earth-lab: the 44 features, then a
weapon slot, an angle (mirrored), a power and two outcome columns.

- AIM.bin (`--oracle`): ballistic labels, the exact shot found by simulated
  search; column 47 is how close it lands. The network learns angle and power
  through the sigmoids the game applies; shots that land within 40 px count
  fully, the rest a tenth.
- DEMOS.bin (`--dump`): the classic AIs' own decisions; only their weapon
  choice is learned, by softmax over the weapons in stock.

MODEL.raw is float32 in KXPOLICY order (per layer: weights [out][in], then
biases), ready for bashed-earth-lab --init or install-policy.py. Needs numpy.
"""
import argparse
import json

import numpy as np

FEATURES, OUTPUTS, WEAPONS = 44, 14, 12
WIDTH = FEATURES + 5


def rows(path):
    return np.fromfile(path, dtype=np.float32).reshape(-1, WIDTH).astype(np.float64)


def aim_set(path):
    r = rows(path)
    a = np.clip((r[:, FEATURES + 1] - 5.0) / 83.0, 0.01, 0.99)
    p = np.clip((r[:, FEATURES + 2] - 10.0) / 90.0, 0.01, 0.99)
    target = np.stack([np.log(a / (1 - a)), np.log(p / (1 - p))], axis=1)
    weight = np.where(r[:, FEATURES + 3] < 40, 1.0, 0.1)
    return r[:, :FEATURES], target, weight, r[:, FEATURES + 1], r[:, FEATURES + 2]


def weapon_set(path):
    r = rows(path)
    r = r[r[:, FEATURES] >= 0]
    x = r[:, :FEATURES]
    return x, r[:, FEATURES].astype(np.int64), x[:, FEATURES - WEAPONS:] > 0.5


def forward(params, x):
    w1, b1, w2, b2, w3, b3 = params
    h1 = np.maximum(0, x @ w1.T + b1)
    h2 = np.maximum(0, h1 @ w2.T + b2)
    return h1, h2, h2 @ w3.T + b3


def backward(params, x, h1, h2, dy):
    w1, b1, w2, b2, w3, b3 = params
    dh2 = (dy @ w3) * (h2 > 0)
    dh1 = (dh2 @ w2) * (h1 > 0)
    return [dh1.T @ x, dh1.sum(0), dh2.T @ h1, dh2.sum(0), dy.T @ h2, dy.sum(0)]


def aim_grads(params, x, target, weight):
    h1, h2, y = forward(params, x)
    diff = y[:, WEAPONS:] - target
    loss = (weight[:, None] * diff ** 2).sum() / weight.sum()
    dy = np.zeros_like(y)
    dy[:, WEAPONS:] = 2 * weight[:, None] * diff / weight.sum()
    return loss, backward(params, x, h1, h2, dy)


def weapon_grads(params, x, slot, mask):
    h1, h2, y = forward(params, x)
    n = len(x)
    logits = np.where(mask, y[:, :WEAPONS], -1e9)
    logits -= logits.max(axis=1, keepdims=True)
    prob = np.exp(logits) * mask
    prob /= prob.sum(axis=1, keepdims=True)
    loss = -np.log(prob[np.arange(n), slot] + 1e-12).mean()
    grad = prob
    grad[np.arange(n), slot] -= 1
    dy = np.zeros_like(y)
    dy[:, :WEAPONS] = grad / n
    return loss, backward(params, x, h1, h2, dy)


def aim_report(params, data):
    x, _, weight, angle, power = data
    _, _, y = forward(params, x)
    sig = 1 / (1 + np.exp(-y[:, WEAPONS:]))
    ok = weight == 1.0
    return {"rows": int(ok.sum()),
            "angle_mae": round(float(np.abs(5 + 83 * sig[ok, 0] - angle[ok]).mean()), 2),
            "power_mae": round(float(np.abs(10 + 90 * sig[ok, 1] - power[ok]).mean()), 2)}


def weapon_report(params, data):
    x, slot, mask = data
    _, _, y = forward(params, x)
    return round(float((np.where(mask, y[:, :WEAPONS], -1e9).argmax(1) == slot).mean()), 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--aim", required=True)
    ap.add_argument("--weapons", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hidden", type=int, required=True)
    ap.add_argument("--select-aim")
    ap.add_argument("--select-weapons")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--weapon-weight", type=float, default=0.2)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    aim, weapons = aim_set(a.aim), weapon_set(a.weapons)
    sel_aim = aim_set(a.select_aim) if a.select_aim else None
    sel_weapons = weapon_set(a.select_weapons) if a.select_weapons else None
    widths = [FEATURES, a.hidden, a.hidden, OUTPUTS]
    params = []
    for i in range(3):
        params.append(rng.normal(0, np.sqrt(2.0 / widths[i]), (widths[i + 1], widths[i])))
        params.append(np.zeros(widths[i + 1]))
    m = [np.zeros_like(p) for p in params]
    v = [np.zeros_like(p) for p in params]
    n_aim, n_weapons, t = len(aim[0]), len(weapons[0]), 0
    steps = max(1, n_aim // a.batch)
    for epoch in range(1, a.epochs + 1):
        order = rng.permutation(n_aim)
        lr = a.lr * (0.5 * (1 + np.cos(np.pi * (epoch - 1) / a.epochs)))    # cosine decay
        for s in range(steps):
            t += 1
            idx = order[s * a.batch:(s + 1) * a.batch]
            _, g_aim = aim_grads(params, aim[0][idx], aim[1][idx], aim[2][idx])
            widx = rng.integers(0, n_weapons, a.batch)
            _, g_w = weapon_grads(params, weapons[0][widx], weapons[1][widx], weapons[2][widx])
            for i in range(6):
                g = g_aim[i] + a.weapon_weight * g_w[i] + 1e-5 * params[i]
                m[i] = 0.9 * m[i] + 0.1 * g
                v[i] = 0.999 * v[i] + 0.001 * g * g
                params[i] -= lr * (m[i] / (1 - 0.9 ** t)) / (np.sqrt(v[i] / (1 - 0.999 ** t)) + 1e-8)
        if epoch % 10 == 0 or epoch == a.epochs:
            line = {"epoch": epoch, "train_aim": aim_report(params, aim),
                    "train_weapon_match": weapon_report(params, weapons)}
            if sel_aim:
                line["select_aim"] = aim_report(params, sel_aim)
            if sel_weapons:
                line["select_weapon_match"] = weapon_report(params, sel_weapons)
            print(json.dumps(line), flush=True)
    flat = np.concatenate([p.ravel() for p in params]).astype("<f4")
    flat.tofile(a.out)
    print(json.dumps({"out": a.out, "parameters": int(flat.size), "widths": widths}))


if __name__ == "__main__":
    main()
