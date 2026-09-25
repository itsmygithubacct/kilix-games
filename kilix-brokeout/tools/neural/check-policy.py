#!/usr/bin/env python3
"""Check that the shipped policy blob, its embedded header and its provenance
manifest describe the same bytes (run by `make test`)."""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = ROOT / "third_party" / "kilix-game-kit" / "tools" / "kilix_policy.py"
BLOB = ROOT / "assets" / "policy" / "brokeout-neural.kxpol"
HEADER = ROOT / "src" / "neural_policy_blob.h"
MANIFEST = ROOT / "docs" / "neural-policy-provenance.json"


def fail(message):
    print(f"check-policy: {message}", file=sys.stderr)
    sys.exit(1)


blob = BLOB.read_bytes()
sha = hashlib.sha256(blob).hexdigest()
info = json.loads(subprocess.run([sys.executable, "-B", str(KIT), "verify", str(BLOB)],
                                 check=True, capture_output=True, text=True).stdout)
manifest = json.loads(MANIFEST.read_text())
if manifest["sha256"] != sha or info["sha256"] != sha:
    fail("blob sha256 does not match the manifest")
if info["widths"] != manifest["widths"] or info["widths"][0] != 38 or info["widths"][-1] != 9:
    fail(f"unexpected widths {info['widths']}")
header = HEADER.read_text()
if f"sha256 {sha}" not in header:
    fail("embedded header was not generated from this blob")
body = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", header.split("= {", 1)[1]))
if body != blob:
    fail("embedded header bytes differ from the blob")
print(f"check-policy: ok ({info['parameters']} parameters, sha256 {sha[:16]})")
