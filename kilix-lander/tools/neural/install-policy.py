#!/usr/bin/env python3
"""Install a trained pilot into the game and record its provenance.

    install-policy.py MODEL.raw --hidden H --manifest-extra EXTRA.json

MODEL.raw is lander-lab's float32 parameter file (KXPOLICY order). This packs
it with kilix-game-kit's own tools/kilix_policy.py into
assets/policy/lander-neural.kxpol, regenerates src/neural_policy_blob.h with
the kit's embedder, and writes docs/neural-policy-provenance.json: the blob's
identity plus everything in EXTRA.json (training, selection and held-out
results). `make check-policy` later proves the three agree.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT / "third_party" / "kilix-game-kit" / "tools" / "kilix_policy.py"
BLOB = ROOT / "assets" / "policy" / "lander-neural.kxpol"
HEADER = ROOT / "src" / "neural_policy_blob.h"
MANIFEST = ROOT / "docs" / "neural-policy-provenance.json"
FEATURES, ACTIONS = 30, 6
sys.path.insert(0, str(KIT.parent))
import kilix_policy  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--hidden", type=int, required=True)
    ap.add_argument("--manifest-extra", help="JSON object merged into the manifest")
    a = ap.parse_args()
    widths = [FEATURES, a.hidden, a.hidden, ACTIONS]
    raw = Path(a.model).read_bytes()
    blob = kilix_policy.pack(widths, raw)
    BLOB.parent.mkdir(parents=True, exist_ok=True)
    BLOB.write_bytes(blob)
    subprocess.run([sys.executable, "-B", str(KIT), "embed", str(BLOB), "--symbol",
                    "neural_policy_blob", str(HEADER)], check=True)
    info = kilix_policy.inspect(blob)
    manifest = {
        "artifact": "assets/policy/lander-neural.kxpol",
        "embedded_header": "src/neural_policy_blob.h",
        "format": "kilix-game-kit KXPOLICY v1",
        "sha256": hashlib.sha256(blob).hexdigest(),
        "fnv1a64": info["fnv1a64"],
        "widths": widths,
        "parameters": info["parameters"],
        "license": "MIT, same as kilix-lander",
        "origin": ("Self-trained in kilix-lander's own simulation (tools/neural/lander_lab.c "
                   "linked against game.o). No third-party data or weights."),
        "inputs": "game_policy_features() in src/game.c: 30 scale-invariant floats",
        "outputs": "6 logits: main thrust {off,on} x side {none,left,right}; argmax each tick",
    }
    if a.manifest_extra:
        manifest.update(json.loads(Path(a.manifest_extra).read_text()))
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n")
    print(json.dumps({k: manifest[k] for k in ("sha256", "widths", "parameters")}))


if __name__ == "__main__":
    main()
