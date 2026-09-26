#!/usr/bin/env python3
"""Coverage test for `make sentinel` (run by `make test`): the storage
sentinel must catch a headless mode that touches the high-score store.

It runs the sentinel target with GAME set to a wrapper around the real
binary. A clean wrapper must pass. For each of the six headless modes, a
wrapper that overwrites the store's legacy high score with different bytes of
the same length (12345 -> 92345) during that mode must make the target fail
and name that mode. Only a content hash sees a same-length overwrite, so a
recipe that drops a mode, stops checking after every mode, stops hashing the
file, or stops honouring GAME fails here. The wrappers do not run the real
game except for --render-test, whose output the recipe checks, so this stays
fast."""
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REAL = ROOT / "kitty-brokeout"

MODES = ("--input-test", "--menu-test", "--player-test", "--selftest", "--render-test",
         "--sound-test")

WRAPPER = """#!/bin/sh
mode="$1"
rc=0
if [ "$mode" = "--render-test" ]; then "{real}" "$@"; rc=$?; fi
if [ "$mode" = "{mutate}" ]; then
    printf '92345\\n' > "$XDG_DATA_HOME/kitty-brokeout/highscore"
fi
exit $rc
"""


def sentinel(wrapper):
    return subprocess.run(["make", "--no-print-directory", "-C", str(ROOT), "sentinel",
                           f"GAME={wrapper}"], capture_output=True, text=True)


def main():
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for mutate in ("none",) + MODES:
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
