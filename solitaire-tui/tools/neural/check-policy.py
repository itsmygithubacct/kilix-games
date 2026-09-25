#!/usr/bin/env python3
"""Check that the shipped policy blob is the one its provenance manifest
describes (run by `make test` and `make all`): the same sha256, shape and
parameter count, and a blob the game's own reader accepts."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from solitaire_tui import features, policy  # noqa: E402

DATA = ROOT / "solitaire_tui" / "data"


def fail(message):
    print(f"check-policy: {message}", file=sys.stderr)
    sys.exit(1)


blob = (DATA / "solitaire-policy.kxpol").read_bytes()
manifest = json.loads((DATA / "solitaire-policy.json").read_text())
sha = hashlib.sha256(blob).hexdigest()
if sha != manifest["sha256"]:
    fail(f"blob sha256 {sha[:16]} is not the manifest's {manifest['sha256'][:16]}")
try:
    net = policy.Policy(blob)
except policy.PolicyError as error:
    fail(f"the game's reader refuses the blob: {error}")
if net.widths != manifest["widths"]:
    fail(f"widths {net.widths} are not the manifest's {manifest['widths']}")
count = sum(net.widths[i] * net.widths[i + 1] + net.widths[i + 1] for i in range(len(net.widths) - 1))
if count != manifest["parameters"]:
    fail(f"{count} parameters, manifest says {manifest['parameters']}")
if net.widths[0] != features.FEATURE_COUNT or net.widths[-1] != 1:
    fail(f"shape {net.widths} does not fit FEATURE_VERSION {features.FEATURE_VERSION}")
print(f"check-policy: ok ({count} parameters, sha256 {sha[:16]})")
