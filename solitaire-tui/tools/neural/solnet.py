#!/usr/bin/env python3
"""Solitaire TUI neural player pipeline: teacher games, imitation, DAgger.

Run from the solitaire-tui directory (it imports solitaire_tui). Every
game-playing subcommand spreads games over a process pool, one game per task.

The learned player scores candidate moves: each of `planner.candidate_moves`
is described by `features.move_features` (visible table plus the game's
History) and a small MLP gives it one score. Training matches a softmax over
each decision's candidate scores to the teacher's: by default to
softmax(teacher values / tau), because the planner's own choice between
near-equal moves is noise; `--loss hard` uses its choice instead. Decisions
with a single candidate teach nothing and are not stored.

  bench    play an agent over seeds, one JSON line per game
  collect  play the teacher over seeds, record its decisions
  dagger   play the student, label every decision with the teacher
  train    fit the scorer to one or more decision datasets (needs torch)
  eval     = bench

Agents: greedy, random, filtered:greedy, filtered:random (the move filter
alone, as baselines), planner:SAMPLES:HORIZON[:f] (f: filtered rollouts),
student:PATH.npz (weights written by `train`).

Seed ranges are the protocol, fixed before any training (research PLAN):
teacher tuning 1-999, training data from 10000 up, checkpoint selection
800000-800499, held-out evaluation 900000-900999.
"""
from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import os
import random
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from solitaire_tui import features                               # noqa: E402
from solitaire_tui.agents import GreedyAgent, RandomAgent        # noqa: E402
from solitaire_tui.engine import apply, deal, legal_moves        # noqa: E402
from solitaire_tui.planner import PlannerAgent, candidate_moves  # noqa: E402
from solitaire_tui.sim import position_key                       # noqa: E402

F = features.FEATURE_COUNT


# ---------------------------------------------------------------- agents


class Scorer:
    """The trained MLP (F -> ... -> 1) in float32 numpy."""

    def __init__(self, path: str):
        data = np.load(path)
        if int(data["feature_version"]) != features.FEATURE_VERSION:
            raise SystemExit(f"{path}: trained on another feature version")
        self.layers = [(data[f"w{i}"].astype(np.float32), data[f"b{i}"].astype(np.float32))
                       for i in range(int(data["layers"]))]

    def scores(self, rows: np.ndarray) -> np.ndarray:
        x = rows
        for i, (w, b) in enumerate(self.layers):
            x = x @ w.T + b
            if i < len(self.layers) - 1:
                x = np.maximum(x, 0.0)
        return x[:, 0]


class Tracked:
    """Remembers the game (features.History) and filters to candidate_moves."""

    def __init__(self):
        self.history = features.History()

    @property
    def visited(self):
        return self.history.visited

    def pool(self, state, legal):
        self.history.see(state)
        return candidate_moves(state, legal, self.visited)


class StudentAgent(Tracked):
    name = "student"

    def __init__(self, path: str):
        super().__init__()
        self.net = Scorer(path)

    def choose(self, state, legal):
        if not legal:
            return None
        pool = self.pool(state, legal)
        if len(pool) <= 1:
            return pool[0] if pool else None
        rows = np.array([features.move_features(state, m, self.history) for m in pool],
                        dtype=np.float32)
        return pool[int(np.argmax(self.net.scores(rows)))]


class FilteredAgent(Tracked):
    """The move filter alone: candidate_moves chosen by greedy's score or at
    random. What the network adds is its win rate over these."""

    def __init__(self, how: str):
        super().__init__()
        self.how = how
        self.greedy = GreedyAgent()
        self.rng = random.Random(0)

    def choose(self, state, legal):
        if not legal:
            return None
        pool = self.pool(state, legal)
        if not pool:
            return None
        if self.how == "random":
            return self.rng.choice(pool)
        return max(pool, key=lambda m: self.greedy.score(state, m))


def make_agent(spec: str):
    kind, _, rest = spec.partition(":")
    if kind == "greedy":
        return GreedyAgent()
    if kind == "random":
        return RandomAgent(0)
    if kind == "filtered":
        return FilteredAgent(rest)
    if kind == "planner":
        parts = rest.split(":")
        return PlannerAgent(int(parts[0]), int(parts[1]), parts[2:] == ["f"])
    if kind == "student":
        return StudentAgent(rest)
    raise SystemExit(f"unknown agent {spec}")


def teacher_pool(agent, state, legal):
    """The candidates `agent` will choose from, before its choose() counts
    this position (no move returns to it, so the pool is the same)."""
    return candidate_moves(state, legal, agent.visited)


# ---------------------------------------------------------------- games


def play_game(job):
    """One game. job = (spec, seed, max_steps, record, label_spec).

    record: keep each decision with 2+ candidates as (feature rows, index of
    the label, the teacher's value for each candidate). label_spec: the label comes from that teacher while `spec`
    chooses the move played (DAgger); the teacher sees every position, so
    its revisit guard matches the game actually played.
    """
    spec, seed, max_steps, record, label_spec = job
    started = time.perf_counter()
    agent = make_agent(spec)
    teacher = make_agent(label_spec) if label_spec else None
    state = deal(seed, False)
    seen = {position_key(state): 1}
    history = features.History()          # the game as the teacher saw it
    decisions = []
    reason = "step-limit"
    steps = max_steps
    for step in range(max_steps):
        if state.won:
            reason, steps = "won", step
            break
        legal = legal_moves(state)
        if record:
            labeller = teacher or agent
            history.see(state)
            pool = teacher_pool(labeller, state, legal)
            label = labeller.choose(state, legal)
            move = label if teacher is None else agent.choose(state, legal)
            if label is not None and len(pool) > 1:
                rows = np.array([features.move_features(state, m, history) for m in pool],
                                dtype=np.float16)
                values = np.array([labeller.last_values[m] for m in pool], dtype=np.float32)
                decisions.append((rows, pool.index(label), values))
        else:
            move = agent.choose(state, legal)
        if move is None:
            reason, steps = "resigned", step
            break
        state = apply(state, move)
        key = position_key(state)
        seen[key] = seen.get(key, 0) + 1
        if seen[key] > 2:
            reason, steps = "stuck", step + 1
            break
    result = {"seed": seed, "agent": spec, "won": bool(state.won),
              "cards_home": state.cards_home(), "steps": steps, "reason": reason,
              "seconds": round(time.perf_counter() - started, 2)}
    return result, (decisions if record else None)


def seeds_from(text: str) -> list:
    out = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


def wilson(wins: int, n: int, z: float = 1.96):
    if n == 0:
        return (0.0, 0.0)
    p = wins / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * ((p * (1 - p) / n + z * z / (4 * n * n)) ** 0.5) / d
    return (round(c - h, 4), round(c + h, 4))


def run_games(args, record: bool, label_spec=None):
    seeds = seeds_from(args.seeds)
    jobs = [(args.agent, s, args.max_steps, record, label_spec) for s in seeds]
    out = open(args.out, "w") if args.out else None
    rows, decisions, wins, done = [], [], 0, 0
    started = time.time()
    with mp.Pool(args.workers) as pool:
        for result, data in pool.imap_unordered(play_game, jobs):
            done += 1
            wins += result["won"]
            rows.append(result)
            if data:
                decisions.extend(data)
            if out:
                out.write(json.dumps(result) + "\n")
                out.flush()
            if done % max(1, len(jobs) // 20) == 0 or done == len(jobs):
                print(f"{done}/{len(jobs)} games, {wins} wins "
                      f"({wins / done:.1%}), {time.time() - started:.0f}s", flush=True)
    summary = {"agent": args.agent, "label": label_spec, "games": done, "wins": wins,
               "win_rate": round(wins / max(1, done), 4), "wilson95": wilson(wins, done),
               "mean_cards_home": round(sum(r["cards_home"] for r in rows) / max(1, done), 2),
               "wall_seconds": round(time.time() - started, 1)}
    print(json.dumps(summary), flush=True)
    return summary, decisions


def save_decisions(path: str, decisions) -> None:
    """Flat candidate rows plus per-decision offsets and label positions."""
    counts = np.array([len(d[0]) for d in decisions], dtype=np.int32)
    offsets = np.concatenate([[0], np.cumsum(counts)]).astype(np.int64)
    rows = (np.concatenate([d[0] for d in decisions]).astype(np.float16)
            if decisions else np.zeros((0, F), np.float16))
    values = (np.concatenate([d[2] for d in decisions]).astype(np.float32)
              if decisions else np.zeros((0,), np.float32))
    label = np.array([d[1] for d in decisions], dtype=np.int16)
    np.savez_compressed(path, rows=rows, offsets=offsets, label=label, values=values,
                        feature_version=features.FEATURE_VERSION)
    print(f"wrote {path}: {len(label)} decisions, {len(rows)} candidate rows, "
          f"max {counts.max() if len(counts) else 0} candidates", flush=True)


def load_decisions(paths):
    rows, counts, label, values = [], [], [], []
    for p in paths:
        d = np.load(p)
        if int(d["feature_version"]) != features.FEATURE_VERSION:
            raise SystemExit(f"{p}: another feature version")
        rows.append(d["rows"])
        counts.append(np.diff(d["offsets"]))
        label.append(d["label"].astype(np.int64))
        values.append(d["values"])
    return (np.concatenate(rows), np.concatenate(counts), np.concatenate(label),
            np.concatenate(values))


# ---------------------------------------------------------------- commands


def cmd_bench(args):
    summary, _ = run_games(args, record=False)
    if args.summary:
        Path(args.summary).write_text(json.dumps(summary, indent=2) + "\n")


def cmd_collect(args):
    _, decisions = run_games(args, record=True)
    save_decisions(args.data, decisions)


def cmd_dagger(args):
    _, decisions = run_games(args, record=True, label_spec=args.teacher)
    save_decisions(args.data, decisions)


def cmd_train(args):
    import torch

    torch.manual_seed(args.seed)
    torch.set_num_threads(args.threads)
    rows, counts, label, values = load_decisions(args.data)
    n, k = len(label), int(counts.max())
    # Pad each decision's candidates to k rows; padding is masked out.
    offsets = np.concatenate([[0], np.cumsum(counts)])
    index = np.full((n, k), -1, dtype=np.int64)
    for j in range(k):
        has = counts > j
        index[has, j] = offsets[:-1][has] + j
    X = torch.tensor(rows.astype(np.float32))
    I = torch.tensor(index)
    Y = torch.tensor(label)
    V = torch.tensor(np.concatenate([values, [0.0]]).astype(np.float32))   # [-1] pads
    order = np.random.default_rng(args.seed).permutation(n)
    split = max(1, int(n * 0.05))
    val, train = torch.tensor(order[:split]), order[split:]
    widths = [F] + [int(w) for w in args.hidden.split(",")] + [1]
    layers = []
    for i in range(len(widths) - 1):
        layers.append(torch.nn.Linear(widths[i], widths[i + 1]))
        if i < len(widths) - 2:
            layers.append(torch.nn.ReLU())
    net = torch.nn.Sequential(*layers)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, args.epochs)

    def logits(batch):
        idx = I[batch]                                  # (b, k)
        valid = idx >= 0
        scores = net(X[idx.clamp(min=0)]).squeeze(-1)   # (b, k)
        return scores.masked_fill(~valid, -1e9)

    def loss_of(batch):
        out = logits(batch)
        if args.loss == "hard":
            return torch.nn.functional.cross_entropy(out, Y[batch])
        idx = I[batch]
        target = (V[idx] / args.tau).masked_fill(idx < 0, -1e9).softmax(-1)
        return -(target * out.log_softmax(-1)).sum(-1).mean()

    for epoch in range(args.epochs):
        net.train()
        perm = torch.tensor(np.random.default_rng(args.seed + epoch).permutation(train))
        total = 0.0
        for s in range(0, len(perm), args.batch):
            batch = perm[s:s + args.batch]
            loss = loss_of(batch)
            opt.zero_grad()
            loss.backward()
            opt.step()
            total += loss.item() * len(batch)
        sched.step()
        net.eval()
        with torch.no_grad():
            acc = (logits(val).argmax(1) == Y[val]).float().mean().item()
        print(f"epoch {epoch + 1}/{args.epochs} loss {total / len(train):.4f} "
              f"val_acc {acc:.4f}", flush=True)
    linear = [m for m in net if isinstance(m, torch.nn.Linear)]
    arrays = {"layers": np.array(len(linear)),
              "feature_version": np.array(features.FEATURE_VERSION)}
    for i, m in enumerate(linear):
        arrays[f"w{i}"] = m.weight.detach().numpy().astype(np.float32)
        arrays[f"b{i}"] = m.bias.detach().numpy().astype(np.float32)
    np.savez(args.out, **arrays)
    print(json.dumps({"out": args.out, "decisions": n, "max_candidates": k,
                      "val_acc": round(acc, 4), "hidden": args.hidden,
                      "epochs": args.epochs, "loss": args.loss, "tau": args.tau}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def games(p):
        p.add_argument("--agent", required=True)
        p.add_argument("--seeds", required=True, help="e.g. 1-200 or 1-50,900-910")
        p.add_argument("--workers", type=int, default=os.cpu_count())
        p.add_argument("--max-steps", type=int, default=1000)
        p.add_argument("--out", help="write one JSON line per game here (truncates)")

    for name, fn in (("bench", cmd_bench), ("eval", cmd_bench)):
        p = sub.add_parser(name)
        games(p)
        p.add_argument("--summary")
        p.set_defaults(fn=fn)
    p = sub.add_parser("collect")
    games(p)
    p.add_argument("--data", required=True)
    p.set_defaults(fn=cmd_collect)
    p = sub.add_parser("dagger")
    games(p)
    p.add_argument("--teacher", required=True)
    p.add_argument("--data", required=True)
    p.set_defaults(fn=cmd_dagger)
    p = sub.add_parser("train")
    p.add_argument("--data", nargs="+", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--hidden", default="64,64")
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--batch", type=int, default=1024)
    p.add_argument("--lr", type=float, default=2e-3)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--threads", type=int, default=min(32, os.cpu_count() or 1))
    p.add_argument("--loss", choices=("soft", "hard"), default="soft")
    p.add_argument("--tau", type=float, default=0.5,
                   help="soft targets: softmax(teacher values / tau); values are ~1 per card")
    p.set_defaults(fn=cmd_train)
    args = parser.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
