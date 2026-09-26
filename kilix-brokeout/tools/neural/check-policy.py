#!/usr/bin/env python3
"""Check that the shipped policy blob, its embedded header and its provenance
manifest describe the same network (run by `make test`): the blob's sha256,
FNV-1a-64 digest, widths and parameter count must equal the manifest's, the
widths must fit the game's contract, and the header must embed exactly these
bytes. `--manifest PATH` and `--header PATH` check other copies of those
files; tools/neural/test-check-policy.py uses them to prove each field is
checked."""
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
INPUTS, OUTPUTS = 38, 9


def problems(blob, info, manifest, header):
    found = []
    sha = hashlib.sha256(blob).hexdigest()
    if info["sha256"] != sha or manifest.get("sha256") != sha:
        found.append("blob sha256 does not match the manifest")
    if manifest.get("fnv1a64") != info["fnv1a64"]:
        found.append(f"manifest fnv1a64 {manifest.get('fnv1a64')} is not the blob's {info['fnv1a64']}")
    if manifest.get("widths") != info["widths"]:
        found.append(f"manifest widths {manifest.get('widths')} are not the blob's {info['widths']}")
    if info["widths"][0] != INPUTS or info["widths"][-1] != OUTPUTS:
        found.append(f"widths {info['widths']} do not fit the game ({INPUTS} in, {OUTPUTS} out)")
    if manifest.get("parameters") != info["parameters"]:
        found.append(f"manifest parameters {manifest.get('parameters')} are not the blob's {info['parameters']}")
    if f"sha256 {sha}" not in header:
        found.append("embedded header was not generated from this blob")
    else:
        body = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", header.split("= {", 1)[1]))
        if body != blob:
            found.append("embedded header bytes differ from the blob")
    return found


def main():
    import argparse
    ap = argparse.ArgumentParser(description="check the shipped policy against its manifest")
    ap.add_argument("--manifest", type=Path, default=MANIFEST)
    ap.add_argument("--header", type=Path, default=HEADER)
    a = ap.parse_args()
    blob = BLOB.read_bytes()
    info = json.loads(subprocess.run([sys.executable, "-B", str(KIT), "verify", str(BLOB)],
                                     check=True, capture_output=True, text=True).stdout)
    manifest = json.loads(a.manifest.read_text())
    header = a.header.read_text()
    found = problems(blob, info, manifest, header)
    if found:
        for line in found:
            print(f"check-policy: {line}", file=sys.stderr)
        return 1
    print(f"check-policy: ok ({info['parameters']} parameters, sha256 {info['sha256'][:16]}, "
          f"fnv1a64 {info['fnv1a64']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
