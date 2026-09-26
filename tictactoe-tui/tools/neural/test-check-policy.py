#!/usr/bin/env python3
"""Negative test for tools/neural/check-policy.py (run by `make check-policy`).

It writes mutated copies of the provenance manifest (sha256, fnv1a64, widths,
parameters each changed in turn) and of the embedded header (one byte
changed), runs the checker on each through --manifest/--header, and requires
it to reject every one while still accepting the unmodified files. It lives
outside the checker, so reverting the checker cannot take this test with it:
a checker that ignores the override options, or stops comparing a field,
fails here."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECK = ROOT / "tools" / "neural" / "check-policy.py"
MANIFEST = ROOT / "docs" / "neural-policy-provenance.json"
HEADER = ROOT / "src" / "neural_policy_blob.h"


def run(*args):
    return subprocess.run([sys.executable, "-B", str(CHECK), *args],
                          capture_output=True, text=True).returncode


def main():
    manifest = json.loads(MANIFEST.read_text())
    header = HEADER.read_text()
    failures = []
    if run() != 0:
        failures.append("the shipped files are rejected")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for field, value in (("sha256", "0" * 64), ("fnv1a64", "0" * 16),
                             ("widths", [1, 1]), ("parameters", 1)):
            bad = dict(manifest, **{field: value})
            path = tmp / f"manifest-{field}.json"
            path.write_text(json.dumps(bad))
            if run("--manifest", str(path)) == 0:
                failures.append(f"a wrong manifest {field} is accepted")
        last = header.rfind("0x")
        byte = header[last:last + 4]
        tampered = header[:last] + ("0x01" if byte != "0x01" else "0x02") + header[last + 4:]
        path = tmp / "header.h"
        path.write_text(tampered)
        if run("--header", str(path)) == 0:
            failures.append("a changed header byte is accepted")
    if failures:
        for line in failures:
            print(f"test-check-policy: FAIL: {line}", file=sys.stderr)
        return 1
    print("test-check-policy: every manifest field and the header bytes are checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
