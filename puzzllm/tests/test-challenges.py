"""Independent solutions, witnesses and counterexamples for campaign rooms."""
from itertools import permutations, product
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "libexec"))
import puzzllm_challenges as puzzles


SOLUTIONS = {
    "lift.py": """from itertools import accumulate
import sys
for line in sys.stdin:
    floors = list(accumulate(1 if move == "U" else -1 for move in line.rstrip("\\n")))
    print(floors.index(-1) + 1 if -1 in floors else "OK")
""",
    "sort4.net": "1 2\n3 4\n1 3\n2 4\n2 3\n",
    "radio.py": """import sys
for line in sys.stdin:
    bits = list(map(int, line.strip()))
    error = 0
    for i, bit in enumerate(bits, 1):
        if bit:
            error ^= i
    if error:
        bits[error - 1] ^= 1
    print("".join(str(bits[i]) for i in (2, 4, 5, 6)))
""",
    "rename.sh": """set -eu
while read -r old new; do mv "$old" ".$new"; done
for destination in A B C D; do mv ".$destination" "$destination"; done
""",
    "cargo.py": """import itertools, json, sys
def pack(crates):
    if not crates:
        return []
    first, *rest = crates
    for size in range(len(rest) + 1):
        for others in itertools.combinations(rest, size):
            cart = (first, *others)
            if sum(crates[label] for label in cart) != 6:
                continue
            tail = pack({k:v for k,v in crates.items() if k not in cart})
            if tail is not None:
                return [cart] + tail
for line in sys.stdin:
    print(json.dumps(pack(json.loads(line)["crates"])))
""",
    "dispatch.py": """from itertools import permutations
import json, sys
def witness(jobs, edges):
    for order in permutations(reversed(jobs)):
        if all(order.index(a) < order.index(b) for a,b in edges):
            return {"order": order}
    for length in range(1, len(jobs) + 1):
        for path in permutations(jobs, length):
            ring = path + (path[0],)
            if all([a,b] in edges for a,b in zip(ring, ring[1:])):
                return {"cycle": ring}
for line in sys.stdin:
    print(json.dumps(witness(**json.loads(line))))
""",
    "escape.py": """import sys
def routes(board, cell=0, visited=frozenset(), path=""):
    if cell == 11:
        yield path
        return
    x, y = cell % 4, cell // 4
    for move, col, row in [("R",x+1,y), ("D",x,y+1), ("U",x,y-1), ("L",x-1,y)]:
        target = 4 * row + col
        if 0 <= col < 4 and 0 <= row < 3 and board[target] == "." and target not in visited:
            yield from routes(board, target, visited | {cell}, path + move)
for line in sys.stdin:
    print(min(routes(line.strip()), key=len, default="BLOCKED"))
""",
    "airlock.tbl": "start - left right start\nleft L left start start\nright R start right start\n",
}


class Campaign(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.state = Path(self.temp.name)
        self.workspace = self.state / "workspace"
        self.workspace.mkdir()
        self.env = dict(os.environ, PUZZLLM_ROOT=str(ROOT), PUZZLLM_STATE=str(self.state))

    def cli(self, *args):
        result = subprocess.run([ROOT / "build/puzzllm", *args], env=self.env,
                                capture_output=True, text=True, timeout=12)
        return result.returncode, result.stdout + result.stderr

    def grade(self, name, contents):
        (self.workspace / name).write_text(contents)
        result = subprocess.run([ROOT / "libexec/puzzllm-sandbox", "challenge-check", name],
                                env=self.env, capture_output=True, text=True, timeout=10)
        return result.returncode, result.stdout + result.stderr

    def test_campaign_through_the_game(self):
        for number, (name, source) in enumerate(SOLUTIONS.items(), 11):
            with self.subTest(name=name):
                (self.state / "progress").write_text(f"level {number}\n")
                code, result = self.cli("--check-level", str(number))
                self.assertEqual(code, 1, result)
                self.assertIn(name, result)
                goal = (self.workspace / "GOAL.md").read_text()
                self.assertIn("~/" + name, goal)
                guide = re.search(r"~/docs/(challenges/\w+\.md)", goal)
                self.assertIsNotNone(guide, goal)
                self.assertTrue((ROOT / "libexec/docs" / guide[1]).is_file())
                (self.workspace / name).write_text(source)
                code, status = self.cli("--goal-status")
                self.assertEqual(code, 0, status)
                self.assertIn("\nCOMPLETE\n", status)
                code, result = self.cli("--check-level", str(number))
                self.assertEqual(code, 0, result)
                self.assertIn(status.splitlines()[-1], result)
        (self.state / "progress").write_text("level 19\n")
        self.assertEqual(self.cli("--goal-status")[0], 2)

    def test_domains(self):
        self.assertEqual({word for word, _ in puzzles.lift_cases()},
                         {"".join(w) for n in range(9) for w in product("UD", repeat=n)})
        self.assertEqual(len(list(puzzles.lift_cases())), 511)
        self.assertEqual(dict(puzzles.lift_cases())["UUUDDDDU"], "7")
        radio = dict(puzzles.radio_cases())
        self.assertEqual(len(radio), 128)
        table = (ROOT / "libexec/docs/challenges/radio.md").read_text()
        pairs = re.findall(r"\| `([01]{4})` \| `([01]{7})` \|", table)
        self.assertEqual(len(pairs), 16)
        for message, sent in pairs:
            for received in (w for w in radio if sum(a != b for a, b in zip(w, sent)) <= 1):
                self.assertEqual(radio[received], message)
        exits = dict(puzzles.exit_cases())
        self.assertEqual(len(exits), 1024)
        self.assertEqual(exits["............"][1], 5)
        self.assertIsNone(exits[".#..#......."][1])
        boards = list(puzzles.dispatch_cases())
        self.assertEqual(len({line for line, _ in boards}), 514)
        self.assertEqual(len({frozenset(edges) for _, (jobs, edges) in boards if jobs == set("ABC")}), 512)
        manifests = list(puzzles.cargo_cases())
        self.assertEqual(len(set(line for line, _ in manifests)), 30)
        self.assertEqual({sum(crates.values()) for _, crates in manifests}, {12, 18, 24})

    def test_sorter(self):
        # A second five-comparator sorting network is equally valid.
        self.assertIn("16", puzzles.sort_four("1 3\n2 4\n1 2\n3 4\n2 3"))
        with self.assertRaisesRegex(puzzles.Incorrect, "Input [01]{4} leaves"):
            puzzles.sort_four(SOLUTIONS["sort4.net"].rsplit("2 3", 1)[0])
        for bad in ["2 1", "0 2", "1 3 4", SOLUTIONS["sort4.net"] + "1 2"]:
            with self.subTest(bad=bad), self.assertRaises(puzzles.Incorrect):
                puzzles.sort_four(bad)

    def test_certificate_validation(self):
        crates = {"A": 4, "B": 2, "C": 3, "D": 3}
        for valid in [[["A","B"],["C","D"]], [["D","C"],["B","A"]]]:
            puzzles.cargo_answer(crates, json.dumps(valid))
        for bad in [[["A","A"],["C","D"]], [["A","C"],["B","D"]],
                    [["B"],["A"],["C","D"]], [["X"],["B","C","D"]]]:
            with self.subTest(carts=bad), self.assertRaises(puzzles.Incorrect):
                puzzles.cargo_answer(crates, json.dumps(bad))
        graph = (set("ABC"), {("A","B")})
        for order in ("ABC", "ACB", "CAB"):
            puzzles.dispatch_answer(graph, json.dumps({"order": list(order)}))
        for bad in [{"order":list("BAC")}, {"order":list("AAB")}, {"order":list("AB")},
                    {"cycle":list("AB")}, {"cycle":list("ABA")}]:
            with self.subTest(bad=bad), self.assertRaises(puzzles.Incorrect):
                puzzles.dispatch_answer(graph, json.dumps(bad))
        ring = (set("ABC"), {("A","B"), ("B","C"), ("C","A")})
        for path in ("ABCA","BCAB","CABC"):
            puzzles.dispatch_answer(ring, json.dumps({"cycle": list(path)}))
        with self.assertRaises(puzzles.Incorrect):
            puzzles.dispatch_answer(ring, '{"cycle":["A","C","B","A"]}')

    def test_reachability_and_paths(self):
        base = SOLUTIONS["airlock.tbl"]
        self.assertIn("3 reachable", puzzles.airlock(base + "unused LR unused unused unused\n"))
        delayed = base.replace("left L left", "left L again") + "again L bad start start\nbad LR bad bad bad\n"
        with self.assertRaisesRegex(puzzles.Incorrect, "Events LLL .*both doors"):
            puzzles.airlock(delayed)
        for bad in ["start - start start start", base.replace("right R start", "right R left"),
                    base.replace("right R start right start","right R start right right"),
                    base.replace("left L left","left L missing")]:
            with self.subTest(bad=bad), self.assertRaises(puzzles.Incorrect):
                puzzles.airlock(bad)
        for path in ("DDRRR","RRRDD"):
            puzzles.exit_answer(("............", 5), path)
        for board, dist, bad in [("............",5,"RLRRRDD"), ("............",5,"RRRRD"),
                                 ("............",5,"RLRRD"), (".#..........",5,"RRRDD"),
                                 ("............",5,"BLOCKED"), (".#..#.......",None,"")]:
            with self.subTest(board=board,bad=bad), self.assertRaises(puzzles.Incorrect):
                puzzles.exit_answer((board,dist),bad)

    def test_late_failures_and_output(self):
        for name, late, message in [
            ("lift.py", 'print("WRONG") if line.strip() == "DDDDDDDD" else ', "Case 511"),
            ("radio.py", 'print("WRONG") if line.strip() == "1111110" else ', "Case 128"),
            ("escape.py", 'print("WRONG") if line.strip() == ".##########." else ', "Case 1024"),
        ]:
            source = SOLUTIONS[name]
            before, after = source.rsplit("    print(", 1)
            with self.subTest(name=name):
                code, result = self.grade(name, before + "    " + late + "print(" + after)
                self.assertEqual(code, 1, result)
                self.assertIn(message, result)
        for change, message in [("", "Missing output line"), ("print('extra')", "output lines")]:
            code, result = self.grade("lift.py", change and SOLUTIONS["lift.py"] + change)
            self.assertEqual(code, 1, result)
            self.assertIn(message, result)

    def test_rename_checks_all_permutations_and_leftovers(self):
        calls = []
        def reference(manifest, directory):
            pairs = [line.split() for line in manifest.decode().splitlines()]
            calls.append(tuple(b for a,b in pairs))
            payload = {old:(directory/old).read_bytes() for old,_ in pairs}
            for old,new in pairs:
                (directory/new).write_bytes(payload[old])
        puzzles.rename_carousel(reference, self.state)
        self.assertEqual(set(calls), set(permutations("ABCD")))
        self.assertEqual(len(calls), 24)
        for script, message in [("true", "original bytes"), (SOLUTIONS["rename.sh"] + "touch .temp", "exactly"),
                                (SOLUTIONS["rename.sh"] + 'for f in A B C D; do head -c "$(wc -c < "$f")" /dev/zero > ".$f"; mv ".$f" "$f"; done', "original bytes"),
                                ("rm D; ln -s A D", "original bytes")]:
            with self.subTest(script=script):
                code, result = self.grade("rename.sh", script)
                self.assertEqual(code, 1, result)
                self.assertIn(message, result)

    def test_unfinished_programs_fail(self):
        for source, message in [("import time; time.sleep(30)", "too long"),
                                ("raise SystemExit(3)", "status 3"),
                                ("import sys; sys.stdout.buffer.write(bytes([255]))", "ASCII")]:
            with self.subTest(message=message):
                code, result = self.grade("lift.py", source)
                self.assertEqual(code, 1, result)
                self.assertIn(message, result)

    def test_regular_file_and_isolation(self):
        # A solution cannot see a sibling workspace file or write into the live home.
        (self.workspace / "marker").write_text("original")
        source = """from pathlib import Path
assert not Path("marker").exists()
try:
    Path("/work/marker").write_text("changed")
except OSError:
    pass
else:
    raise RuntimeError("workspace writable")
""" + SOLUTIONS["lift.py"]
        self.assertEqual(self.grade("lift.py", source)[0], 0)
        self.assertEqual((self.workspace / "marker").read_text(), "original")
        (self.workspace / "sort4.net").symlink_to(self.workspace / "marker")
        self.assertEqual(self.cli("--check-level","12")[0], 1)
        (self.workspace / "sort4.net").unlink()
        code, result = self.grade("sort4.net", "x" * 65537)
        self.assertEqual(code, 1, result)
        self.assertIn("64 KiB", result)


if __name__ == "__main__":
    unittest.main()
