#!/usr/bin/env python3
"""test-defects.py SCRATCH - planted-defect controls.

Each defect is one exact edit to a copy of the game. The edit must apply
exactly once (so a stale defect cannot pass silently), and the named test
target must then FAIL. A defect the tests do not notice is a hole in the
tests, and this script fails.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, file, old, new, make target)
DEFECTS = [
    # --- sandbox ---
    ("sandbox shares the network",
     "libexec/puzzllm-sandbox",
     "--unshare-all --die-with-parent",
     "--unshare-all --share-net --die-with-parent", "test-sandbox"),
    ("sandbox binds the host root read-write at /mnt/host",
     "libexec/puzzllm-sandbox",
     "    --ro-bind /usr /usr\n",
     "    --ro-bind /usr /usr --bind / /mnt/host\n", "test-sandbox"),
    ("sandbox root left writable",
     "libexec/puzzllm-sandbox",
     'exec bwrap "${args[@]}" --remount-ro / -- "$@"',
     'exec bwrap "${args[@]}" -- "$@"', "test-sandbox"),
    ("sandbox shows the host home",
     "libexec/puzzllm-sandbox",
     "    --tmpfs /tmp --tmpfs /home --tmpfs \"$box_home\"\n",
     "    --tmpfs /tmp --tmpfs /home --tmpfs \"$box_home\" --ro-bind \"$HOME\" \"$HOME\"\n",
     "test-sandbox"),
    ("shell gets the workspace read-only",
     "libexec/puzzllm-sandbox",
     '    args+=(--bind "$state/workspace" "$box_home/workspace"\n           --chdir',
     '    args+=(--ro-bind "$state/workspace" "$box_home/workspace"\n           --chdir',
     "test-sandbox"),
    ("omp pointed at the wrong port",
     "libexec/puzzllm-sandbox",
     "    baseUrl: http://127.0.0.1:$port/v1",
     "    baseUrl: http://127.0.0.1:1$port/v1", "test-sandbox"),
    # --- game ---
    ("level check follows symlinks",
     "src/levels.c", "    if (lstat(path, &st) != 0) {", "    if (stat(path, &st) != 0) {",
     "test-unit"),
    ("contains-ci is case-sensitive",
     "src/levels.c",
     "            if (ci) { a = (unsigned char)tolower(a); b = (unsigned char)tolower(b); }",
     "            (void)ci;", "test-unit"),
    ("cursor position off by one",
     "src/vt.c",
     "    case 'H': case 'f': move_to(t, param(t, 1, 1) - 1, param(t, 0, 1) - 1); break;",
     "    case 'H': case 'f': move_to(t, param(t, 1, 1), param(t, 0, 1) - 1); break;",
     "test-unit"),
    ("door passable while shut",
     "src/scene.c", "    return s->door >= 0.95f;", "    return s->door >= 0.0f;",
     "test-unit"),
    ("application cursor keys ignored",
     "src/keys.c",
     "        n = snprintf(b, sizeof b, \"\\033%c%c\", app ? 'O' : '[', letter);",
     "        n = snprintf(b, sizeof b, \"\\033%c%c\", app ? '[' : '[', letter);",
     "test-unit"),
    ("progress not saved",
     "src/game.c", "        game_save_progress(g->state, next);\n", "", "test-unit"),
    ("surrogate pairs decoded wrong",
     "src/json.c",
     "cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);",
     "cp = 0x10000 + ((cp - 0xd800) << 9) + (lo - 0xdc00);", "test-unit"),
    ("goal not re-checked while playing",
     "src/game.c", "        game_poll_goal(g);\n    }", "    }", "test-unit"),
]


def copy_tree(dst):
    """The game directory without test scratch; objects keep their times so
    only the edited file is rebuilt. The SDK link becomes absolute."""
    def ignore(path, names):
        if os.path.abspath(path) == os.path.join(HERE, "build"):
            return [n for n in names if n not in ("obj", "puzzllm", "test-puzzllm")]
        return []
    shutil.copytree(HERE, dst, symlinks=True, ignore=ignore)
    sdk = os.path.realpath(os.path.join(HERE, "third_party", "kilix-game-sdk"))
    os.remove(os.path.join(dst, "third_party", "kilix-game-sdk"))
    os.symlink(sdk, os.path.join(dst, "third_party", "kilix-game-sdk"))


def main():
    scratch = sys.argv[1]
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    kit = os.path.join(HERE, "build", "kilix-game-kit")
    holes = 0
    for i, (name, path, old, new, target) in enumerate(DEFECTS, 1):
        dst = os.path.join(scratch, "d%02d" % i)
        copy_tree(dst)
        f = os.path.join(dst, path)
        text = open(f).read()
        if text.count(old) != 1:
            print("BROKEN defect %d (%s): edit matches %d times in %s"
                  % (i, name, text.count(old), path))
            holes += 1
            continue
        open(f, "w").write(text.replace(old, new))
        log = os.path.join(scratch, "d%02d.log" % i)
        with open(log, "w") as out:
            rc = subprocess.call(
                ["make", "-C", dst, "KILIX_GAME_KIT_BUILD_DIR=" + kit, target],
                stdout=out, stderr=subprocess.STDOUT)
        if rc == 0:
            print("HOLE  defect %d (%s): %s still passes" % (i, name, target))
            holes += 1
        else:
            print("red   defect %d (%s): %s fails" % (i, name, target))
        shutil.rmtree(dst)
    print("test-defects: %d defects, %d not caught" % (len(DEFECTS), holes))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
