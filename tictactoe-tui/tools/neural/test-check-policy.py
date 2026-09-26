#!/usr/bin/env python3
"""Negative test for tools/neural/check-policy.py (run by `make check-policy`).

It writes mutated copies of the provenance manifest (sha256, fnv1a64, widths,
parameters each changed in turn) and of the embedded header (one byte
changed), runs the checker on each through --manifest/--header, and requires
it to reject every one while still accepting the unmodified files. It also
loads the checker and feeds its problems() function verifier results that
break one rule each (the verifier's sha256, the game's input width, its
output width) and a header whose sha256 annotation alone is wrong. It lives
outside the checker, so reverting the checker, or deleting any one of its
comparisons, fails here."""
import importlib.util
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


def direct_cases(manifest, header):
    """Rules the files alone cannot break: call the checker's problems()."""
    spec = importlib.util.spec_from_file_location("check_policy", CHECK)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        problems = module.problems
        blob = module.BLOB.read_bytes()
        inputs, outputs = module.INPUTS, module.OUTPUTS
    except Exception as error:                   # an old or broken checker
        return [f"the checker has no usable problems(): {error}"]
    good = {"sha256": manifest["sha256"], "fnv1a64": manifest["fnv1a64"],
            "widths": list(manifest["widths"]), "parameters": manifest["parameters"]}
    found = []
    if problems(blob, good, manifest, header):
        found.append("problems() rejects the shipped files")
    bad = dict(good, sha256="0" * 64)
    if not problems(blob, bad, manifest, header):
        found.append("a verifier sha256 that differs from the blob is accepted")
    for side, index, fit in (("input", 0, inputs), ("output", -1, outputs)):
        widths = list(good["widths"])
        widths[index] = fit + 1
        bad = dict(good, widths=widths)
        agreeing = dict(manifest, widths=widths)   # manifest agrees; only the contract breaks
        if not problems(blob, bad, agreeing, header):
            found.append(f"an {side} width the game cannot use is accepted")
    return found


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
        sha = json.loads(MANIFEST.read_text())["sha256"]
        annotated = header.replace(f"sha256 {sha}", "sha256 " + "0" * 64, 1)
        path = tmp / "header-annotation.h"
        path.write_text(annotated)
        if annotated == header or run("--header", str(path)) == 0:
            failures.append("a header with a wrong sha256 annotation is accepted")
    failures += direct_cases(manifest, header)
    if failures:
        for line in failures:
            print(f"test-check-policy: FAIL: {line}", file=sys.stderr)
        return 1
    print("test-check-policy: every manifest field and the header bytes are checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
