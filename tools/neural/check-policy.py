"""make check-policy: the shipped blob, its embedded header and the
provenance manifest must agree, and the blob must pass the kit's verifier."""
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KIT_TOOL = os.path.join(ROOT, "third_party", "kilix-game-kit", "tools", "kilix_policy.py")
BLOB = os.path.join(ROOT, "assets", "policy", "pong-neural.kxpol")
HEADER = os.path.join(ROOT, "src", "neural_policy_blob.h")
MANIFEST = os.path.join(ROOT, "docs", "neural-policy-provenance.json")


def fail(message):
    print(f"check-policy: {message}", file=sys.stderr)
    sys.exit(1)


def main():
    verify = subprocess.run([sys.executable, "-B", KIT_TOOL, "verify", BLOB],
                            capture_output=True, text=True)
    if verify.returncode:
        fail(verify.stderr.strip())
    info = json.loads(verify.stdout)
    with open(MANIFEST) as fh:
        manifest = json.load(fh)
    with open(BLOB, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    for key, actual in (("sha256", digest), ("fnv1a64", info["fnv1a64"]),
                        ("widths", info["widths"]), ("parameters", info["parameters"])):
        if manifest.get(key) != actual:
            fail(f"manifest {key} {manifest.get(key)!r} != blob {actual!r}")
    if info["widths"][0] != 11 or info["widths"][-1] != 3:
        fail(f"blob widths {info['widths']} do not match POLICY_FEATURES/POLICY_ACTIONS")
    embed = subprocess.run([sys.executable, "-B", KIT_TOOL, "embed", BLOB, "--symbol",
                            "neural_policy_blob", HEADER, "--check"], capture_output=True, text=True)
    if embed.returncode:
        fail(embed.stderr.strip())
    print(f"ok: neural policy {digest[:16]}... blob, header and manifest agree")


if __name__ == "__main__":
    main()
