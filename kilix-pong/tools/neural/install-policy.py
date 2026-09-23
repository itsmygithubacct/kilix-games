"""Install a trained policy blob into the game and record its provenance.

    python3 tools/neural/install-policy.py WORK [--test TEST.jsonl]

Copies WORK/models/<SELECTED>.kxpol to assets/policy/pong-neural.kxpol,
regenerates src/neural_policy_blob.h through kilix-game-kit's embedder, and
writes docs/neural-policy-provenance.json from the training report, the dev
tournaments of every round and, if given, the frozen test tournament.
`make check-policy` later proves the three still agree.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KIT_TOOL = os.path.join(ROOT, "third_party", "kilix-game-kit", "tools", "kilix_policy.py")
BLOB = os.path.join(ROOT, "assets", "policy", "pong-neural.kxpol")
HEADER = os.path.join(ROOT, "src", "neural_policy_blob.h")
MANIFEST = os.path.join(ROOT, "docs", "neural-policy-provenance.json")


def jsonl(path):
    with open(path) as fh:
        return [json.loads(line) for line in fh if line.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--test", help="frozen test tournament JSON lines")
    a = ap.parse_args()
    models = os.path.join(a.work, "models")
    with open(os.path.join(models, "SELECTED")) as fh:
        selected = fh.read().strip()
    source = os.path.join(models, selected + ".kxpol")
    with open(source + ".json") as fh:
        report = json.load(fh)

    os.makedirs(os.path.dirname(BLOB), exist_ok=True)
    shutil.copyfile(source, BLOB)
    subprocess.run([sys.executable, "-B", KIT_TOOL, "embed", BLOB, "--symbol",
                    "neural_policy_blob", HEADER], check=True)
    info = json.loads(subprocess.run([sys.executable, "-B", KIT_TOOL, "verify", BLOB],
                                     check=True, capture_output=True, text=True).stdout)

    rounds = sorted(f[:-len("-dev.jsonl")] for f in os.listdir(models) if f.endswith("-dev.jsonl"))
    manifest = {
        "artifact": "assets/policy/pong-neural.kxpol",
        "embedded_header": "src/neural_policy_blob.h",
        "format": "kilix-game-kit KXPOLICY v1",
        "sha256": info["sha256"],
        "fnv1a64": info["fnv1a64"],
        "widths": info["widths"],
        "parameters": info["parameters"],
        "temperature": info["temperature"],
        "license": "MIT, same as kilix-pong",
        "origin": ("Trained only from kilix-pong's own simulation: labels come from "
                   "tools/neural/pong_lab.c's lookahead planner over game.o. No "
                   "third-party data, weights or recordings."),
        "procedure": "tools/neural/train-policy.sh, then tools/neural/install-policy.py",
        "selected_round": selected,
        "selection_rule": ("most dev match wins vs HARD, then higher dev point "
                           "difference vs HARD, then the earlier round"),
        "training": {k: report[k] for k in ("train_records", "dev_records", "label_share",
                                            "seconds", "temperature", "hidden", "epochs",
                                            "seed", "torch")},
        "dev_calibrated": report["dev_calibrated"],
        "dev_tournaments": {r: jsonl(os.path.join(models, r + "-dev.jsonl")) for r in rounds},
        "reproducibility": ("procedure-reproducible, not bit-exact: PyTorch CPU training "
                            "may differ across builds; the shipped bytes are pinned by "
                            "sha256 and verified by make check-policy"),
    }
    if a.test:
        manifest["frozen_test"] = jsonl(a.test)
    with open(MANIFEST, "w") as fh:
        json.dump(manifest, fh, indent=1)
        fh.write("\n")
    print(f"installed {selected}: sha256 {info['sha256']}")


if __name__ == "__main__":
    main()
