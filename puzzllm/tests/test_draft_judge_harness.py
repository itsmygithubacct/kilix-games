#!/usr/bin/env python3
"""The draft-and-judge harness names: the picks the menu walks, and the command they start."""
import importlib.machinery
import importlib.util
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest

HELPER = pathlib.Path(__file__).resolve().parents[1] / "libexec" / "puzzllm-ollama"
loader = importlib.machinery.SourceFileLoader("puzzllm_ollama_harness", str(HELPER))
spec = importlib.util.spec_from_loader(loader.name, loader)
helper = importlib.util.module_from_spec(spec)
loader.exec_module(helper)

FOUND = {"workers": ["qwen3.5-0.8b", "qwen3.5-4b", "claude"], "judges": ["d3-2b", "qwen3.5-4b", "claude"]}
FULL = "draft_judge_harness/W:qwen3.5-0.8b:4/W:claude:1/D:10/J:d3-2b"


class HarnessTests(unittest.TestCase):
    def test_the_picks_go_worker_slots_drafts_judge(self):
        name = "draft_judge_harness"
        self.assertEqual(helper.harness_options(name, FOUND), [f"{name}/W:{m}" for m in FOUND["workers"]])
        name += "/W:qwen3.5-0.8b"
        self.assertEqual(helper.harness_options(name, FOUND), [f"{name}:{n}" for n in range(1, 9)])
        name += ":4"
        self.assertEqual(helper.harness_options(name, FOUND),
                         [f"{name}/D:{n}" for n in (4, 8, 12, 16)] + [f"{name}/W:qwen3.5-4b", f"{name}/W:claude"])
        name += "/W:claude:1/D:10"
        self.assertEqual(helper.harness_options(name, FOUND), [f"{name}/J:{m}" for m in FOUND["judges"]])
        self.assertEqual(helper.harness_options(FULL, FOUND), [])

    def test_bad_names_are_refused(self):
        for name in ("draft_judge_harness/D:4", "draft_judge_harness/W:a:9", "draft_judge_harness/W:a:1/J:x",
                     "draft_judge_harness/W:a/W:b:1", "draft_judge_harness/W:a:1/W:a:2/D:2/J:x",
                     "draft_judge_harness/W:a:1/D:0/J:x", "other/W:a:1"):
            with self.assertRaises(ValueError, msg=name):
                helper.parse_harness(name)

    def test_a_finished_name_starts_the_stack_and_the_agent_gets_its_socket(self):
        with tempfile.TemporaryDirectory() as state:
            launcher = os.path.join(state, "kilix-llm")
            with open(launcher, "w") as handle:  # records its arguments, then makes the socket
                handle.write('#!/bin/sh\nprintf "%s\\n" "$@" > "$(dirname "$0")/argv"\n'
                             'while [ "$1" != --socket ]; do shift; done\n: > "$2"\nexec sleep 30\n')
            os.chmod(launcher, os.stat(launcher).st_mode | stat.S_IXUSR)
            env = dict(os.environ, PUZZLLM_KILIX_LLM=launcher, PUZZLLM_CTX="16384")
            done = subprocess.run([sys.executable, str(HELPER), "host", state, FULL, "sh", "-c",
                                   'test -e "$0" && echo ok', "@OLLAMA_SOCKET@"],
                                  env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual((done.returncode, done.stdout), (0, "ok\n"), done.stderr)
            argv = pathlib.Path(state, "argv").read_text().split("\n")
            self.assertEqual(argv[:2], ["stack", "serve"])
            for flag, value in (("--ctx", "16384"), ("--drafts", "10"), ("--judge", "d3-2b"), ("--name", FULL)):
                self.assertEqual(argv[argv.index(flag) + 1], value)
            self.assertEqual([argv[i + 1] for i, a in enumerate(argv) if a == "--worker"],
                             ["qwen3.5-0.8b:4", "claude:1"])

    def test_an_unfinished_name_does_not_start(self):
        with self.assertRaises(ValueError):
            helper.harness_argv("draft_judge_harness/W:a:1/D:2", "sock", "journal")


if __name__ == "__main__":
    unittest.main()
