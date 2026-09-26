#!/usr/bin/env python3
"""Coverage test for `make sentinel` (run by `make test`): the storage
sentinel must catch a headless mode that touches the high-score store.

It runs the sentinel target with GAME set to a wrapper around the real
binary. A clean wrapper must pass. A wrapper that appends to the store's
legacy high-score file after one mode must make the target fail and name that
mode; this is tried after the first mode (--input-test) and after the last
(--sound-test, which runs with an empty PATH). A recipe that stopped
checking after every mode, dropped a mode, or stopped honouring GAME fails
here. The wrapper only runs the real game for the tampered mode and for
--render-test, whose output the recipe checks, so this stays fast."""
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REAL = ROOT / "kitty-brokeout"

WRAPPER = """#!/bin/sh
mode="$1"
if [ "$mode" = "{mutate}" ]; then
    "{real}" "$@"; rc=$?
    printf 'x' >> "$XDG_DATA_HOME/kitty-brokeout/highscore"
    exit $rc
fi
if [ "$mode" = "--render-test" ]; then exec "{real}" "$@"; fi
exit 0
"""


def sentinel(wrapper):
    return subprocess.run(["make", "--no-print-directory", "-C", str(ROOT), "sentinel",
                           f"GAME={wrapper}"], capture_output=True, text=True)


def main():
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for mutate in ("none", "--input-test", "--sound-test"):
            wrapper = Path(tmp) / f"game-{mutate.strip('-') or 'none'}"
            wrapper.write_text(WRAPPER.format(mutate=mutate, real=REAL))
            wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
            r = sentinel(wrapper)
            if mutate == "none":
                if r.returncode != 0:
                    failures.append(f"a clean wrapper fails the sentinel: {r.stderr.strip()[-200:]}")
            elif r.returncode == 0:
                failures.append(f"a store change after {mutate} is not caught")
            elif f"FAIL: {mutate} changed" not in r.stderr:
                failures.append(f"a store change after {mutate} is not named: {r.stderr.strip()[-200:]}")
    if failures:
        for line in failures:
            print(f"sentinel-coverage: FAIL: {line}", file=sys.stderr)
        return 1
    print("sentinel-coverage: the sentinel catches and names a store change after any mode tried")
    return 0


if __name__ == "__main__":
    sys.exit(main())
