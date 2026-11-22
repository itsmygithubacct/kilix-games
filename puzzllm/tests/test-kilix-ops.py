import json
import sys
import unittest
from pathlib import Path
from collections import Counter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "libexec/tasks/kilix_ops"))
from kilix_tasks import ITEMS, commands, prompt, score


class KilixOperations(unittest.TestCase):
    def test_bank_and_keys(self):
        items = json.loads(ITEMS.read_text())
        self.assertEqual(Counter(i["kind"] for i in items), dict(command=8, readout=8, workflow=8))
        self.assertEqual(len({i["id"] for i in items}), len(items))
        for i in items:
            self.assertIn(i["question"], prompt(i))
            for key in i["answers"]:
                self.assertEqual(score(i, [key]), {"acc": 1}, i["id"])
                if i["kind"] != "readout":
                    rows = commands(key)
                    self.assertEqual(len(rows), 2 if i["kind"] == "workflow" else 1)
                    self.assertTrue(all(row[0] == "kilix" for row in rows))
            self.assertEqual(score(i, [""]), {"acc": 0})

    def test_command_scope_and_order(self):
        q = {"kind": "workflow", "answers": ["kilix pane send pane:12 'echo hi' --submit\nkilix pane read pane:12"]}
        first, second = q["answers"][0].splitlines()
        self.assertEqual(score(q, [first.replace("'echo hi'", '"echo hi"') + "\n" + second]), {"acc": 1})
        for wrong in (second + "\n" + first, first, first.replace("--submit", "") + "\n" + second,
                      first.replace("pane:12", "pane:21") + "\n" + second,
                      first + "\n" + second + "; pwd", first + "\n" + second + "\nDone"):
            self.assertEqual(score(q, [wrong]), {"acc": 0})

if __name__ == "__main__":
    unittest.main()
