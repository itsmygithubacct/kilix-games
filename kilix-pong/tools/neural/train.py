"""Train kilix-pong's neural player on pong-lab dumps.

    python train.py --train A.bin B.bin ... --dev DEV.bin --out POLICY.kxpol
                    [--hidden 64] [--epochs 8] [--seed 0] [--init POLICY.kxpol]

Records are POLICY_FEATURES float32 features plus an int32 planner label
(see pong_lab.c). The network is POLICY_FEATURES -> H -> H -> 3 with ReLU,
trained with cross-entropy (AdamW, one-cycle LR). A single temperature is
fitted on dev (calibration only; it never changes the argmax) and stored in
the blob. The output is a kilix-game-kit KXPOLICY blob written through
kilix_policy.py, plus OUT.json with the history and dev metrics.

Needs PyTorch and NumPy; the game itself needs neither.
"""
import argparse
import json
import os
import sys
import time

import numpy as np
import torch
from torch import nn

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "third_party", "kilix-game-kit", "tools"))
import kilix_policy  # noqa: E402

FEATURES = 11   # POLICY_FEATURES in src/kilix_pong.h
ACTIONS = 3


def load(paths):
    rec = np.dtype([("f", "<f4", FEATURES), ("y", "<i4")])
    arr = np.concatenate([np.fromfile(p, dtype=rec) for p in paths])
    return torch.from_numpy(arr["f"].copy()), torch.from_numpy(arr["y"].astype(np.int64))


def model(hidden):
    return nn.Sequential(nn.Linear(FEATURES, hidden), nn.ReLU(), nn.Linear(hidden, hidden),
                         nn.ReLU(), nn.Linear(hidden, ACTIONS))


def raw_parameters(net):
    return np.concatenate([t.detach().float().numpy().ravel()
                           for t in net.state_dict().values()]).astype("<f4").tobytes()


def load_blob(net, path):
    with open(path, "rb") as fh:
        blob = fh.read()
    info = kilix_policy.inspect(blob)
    header = 16 + 4 * len(info["widths"]) + 4
    flat = torch.from_numpy(np.frombuffer(blob[header:-8], dtype="<f4").copy())
    sd, i = net.state_dict(), 0
    for k, v in sd.items():
        sd[k] = flat[i:i + v.numel()].view_as(v).clone()
        i += v.numel()
    if i != flat.numel():
        raise SystemExit(f"{path}: shape does not match --hidden")
    net.load_state_dict(sd)


@torch.no_grad()
def evaluate(net, x, y, temp=1.0):
    logits = net(x) / temp
    pred = logits.argmax(1)
    correct = pred == y
    p = logits.softmax(1).max(1).values
    bins = torch.clamp((p * 10).long(), 0, 9)
    ece = sum(((bins == b).float().mean()
               * (p[bins == b].mean() - correct[bins == b].float().mean()).abs()).item()
              for b in range(10) if (bins == b).any())
    return {"accuracy": correct.float().mean().item(),
            "nll": nn.functional.cross_entropy(logits, y).item(), "ece": ece,
            "recall": {c: correct[y == i].float().mean().item()
                       for i, c in enumerate(("up", "stay", "down")) if (y == i).any()}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--dev", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--init")
    ap.add_argument("--hidden", type=int, default=64)
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--threads", type=int, default=4)
    a = ap.parse_args()

    torch.manual_seed(a.seed)
    torch.set_num_threads(a.threads)
    x, y = load(a.train)
    dx, dy = load([a.dev])
    net = model(a.hidden)
    if a.init:
        load_blob(net, a.init)
    opt = torch.optim.AdamW(net.parameters(), lr=3e-3, weight_decay=1e-4)
    batch = 4096
    steps = a.epochs * ((len(y) + batch - 1) // batch)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=3e-3, total_steps=steps)
    history, t0 = [], time.time()
    for epoch in range(a.epochs):
        net.train()
        perm = torch.randperm(len(y))
        for i in range(0, len(y), batch):
            idx = perm[i:i + batch]
            loss = nn.functional.cross_entropy(net(x[idx]), y[idx])
            opt.zero_grad()
            loss.backward()
            opt.step()
            sched.step()
        net.eval()
        history.append({"epoch": epoch + 1, "train_loss": loss.item(), **evaluate(net, dx, dy)})
        print(json.dumps({k: history[-1][k] for k in ("epoch", "train_loss", "accuracy")}), flush=True)

    with torch.no_grad():
        logits = net(dx)
    temperature = float(min(
        (nn.functional.cross_entropy(logits / t, dy).item(), t)
        for t in np.exp(np.linspace(np.log(0.05), np.log(5.0), 200)))[1])
    blob = kilix_policy.pack([FEATURES, a.hidden, a.hidden, ACTIONS], raw_parameters(net), temperature)
    with open(a.out, "wb") as fh:
        fh.write(blob)
    report = {
        "blob": kilix_policy.inspect(blob), "train_records": len(y), "dev_records": len(dy),
        "label_share": {c: (y == i).float().mean().item() for i, c in enumerate(("up", "stay", "down"))},
        "seconds": round(time.time() - t0, 1), "history": history, "temperature": temperature,
        "dev_calibrated": evaluate(net, dx, dy, temperature), "train_files": a.train,
        "dev_file": a.dev, "init": a.init, "seed": a.seed, "hidden": a.hidden,
        "epochs": a.epochs, "torch": torch.__version__,
    }
    with open(a.out + ".json", "w") as fh:
        json.dump(report, fh, indent=1)
    print(json.dumps({"parameters": report["blob"]["parameters"],
                      "dev": history[-1]["accuracy"], "temperature": temperature}))


if __name__ == "__main__":
    main()
