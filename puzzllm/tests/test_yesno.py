#!/usr/bin/env python3
"""Exercise native provider commands, durable runs and exact reply scoring offline."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

WORKER = Path(__file__).resolve().parents[1] / "libexec/puzzllm-yesno"
loader = importlib.machinery.SourceFileLoader("yesno_worker", str(WORKER))
worker = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(worker)

QUESTIONS = [
    dict(id="first", band="maths", prompt="Is two even?", answer=0, rationale="Two is even."),
    dict(id="second", band="reading", prompt="Is the light on?", context="The light is off.",
         answer=1, rationale="The light is off."),
    dict(id="third", band="logic", prompt="Is none zero?", answer=0, rationale="None means zero."),
    dict(id="fourth", band="logic", prompt="Are there two?", answer=1, rationale="There are three."),
]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line]


class BenchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.log = self.root / "calls"
        for name in ("codex", "grok", "claude"):
            binary = self.root / name
            binary.write_text("""#!/usr/bin/env python3
import json, os, pathlib, sys, time
args = sys.argv[1:]
provider = pathlib.Path(sys.argv[0]).name
prompt = args[args.index('--single')+1] if provider == 'grok' else sys.stdin.read()
with open(os.environ['BENCH_CALLS'], 'a') as out:
    print(json.dumps([provider, args, prompt]), file=out)
time.sleep(float(os.environ.get('BENCH_DELAY', '0')))
if 'two even' in prompt: reply = 'YES'
elif 'light' in prompt: reply = 'YES'
elif 'none' in prompt: reply = 'NO, because...'
else: reply = 'NO'
reply = os.environ.get('BENCH_REPLY', reply)
if provider == 'codex':
    pathlib.Path(args[args.index('-o')+1]).write_text(reply)
    print('unrelated progress on stdout')
else:
    print(reply)
""")
            binary.chmod(0o700)
        package = self.root / "fixture/src/kilix_bench/onebit"
        (package / "backends").mkdir(parents=True)
        (package / "items.py").write_text('def load(split):\n assert split=="dev"\n return ' + repr(QUESTIONS))
        (package / "contract.py").write_text("def user_message(item):\n return item.get('context','')+'\\n'+item['prompt']\n")
        (package / "backends/chat.py").write_text("class ChatBackend: pass\n")
        self.env = {**os.environ, "PUZZLLM_YESNO_BENCH": str(self.root/"fixture"),
                    "BENCH_CALLS": str(self.log), "PYTHONDONTWRITEBYTECODE": "1"}
        for name in ("codex", "grok", "claude"):
            self.env["PUZZLLM_" + name.upper()] = str(self.root/name)

    def process(self, *args, delay=0):
        proc = subprocess.Popen([WORKER, *args], env={**self.env, "BENCH_DELAY": str(delay)},
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def close():
            if proc.poll() is None:
                proc.terminate()
            proc.communicate(timeout=8)
        self.addCleanup(close)
        return proc

    def wait_for(self, condition):
        deadline = time.monotonic()+8
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.025)
        self.fail("worker did not reach expected state")

    def reports(self):
        return list((self.state/"bench/yesno").glob("*.jsonl"))

    def test_all_hosted_adapters_use_one_fresh_prompt_and_strict_grades(self):
        for provider in ("codex", "grok", "claude"):
            with self.subTest(provider=provider):
                output = subprocess.run([WORKER, "run", self.state, provider, "chosen-model"],
                                        env=self.env, stdin=subprocess.DEVNULL, capture_output=True,
                                        text=True, check=True, timeout=12).stdout
                events = [json.loads(line) for line in output.splitlines()]
                self.assertEqual(events[-1]["event"], "end")
                self.assertEqual([e["verdict"] for e in events if e["event"] == "answer"],
                                 ["correct", "wrong", "invalid", "correct"])
                calls = [row for row in rows(self.log) if row[0] == provider]
                self.assertEqual(len(calls), len(QUESTIONS))
                for (_, args, prompt), item in zip(calls, QUESTIONS):
                    self.assertEqual(args[args.index("--model") + 1], "chosen-model")
                    self.assertIn("Respond with only YES or NO.", "\n".join([*args, prompt]))
                    self.assertIn(item["prompt"], prompt)
                    self.assertNotIn("rationale", prompt)
                    if "context" in item:
                        self.assertIn(item["context"], prompt)
        self.assertEqual(len(self.reports()), 3)
        self.assertTrue(all(not (p.stat().st_mode & 0o077) for p in self.reports()))
        saved = [r for r in rows(self.reports()[0]) if r["event"] == "answer"]
        self.assertEqual(saved[2]["reply"], "NO, because...")
        self.assertEqual(saved[1]["context"], QUESTIONS[1]["context"])

    def test_lowercase_reply_is_a_format_failure(self):
        self.env["BENCH_REPLY"] = "yes"
        result, _ = self.process("run", self.state, "claude").communicate(timeout=5)
        events = [json.loads(line) for line in result.splitlines()]
        self.assertEqual([r["verdict"] for r in events if r["event"] == "answer"], ["invalid"] * 4)

    def test_detaching_rejoining_and_concurrent_start_keep_one_run(self):
        watch = self.process("run", self.state, "grok", delay=0.45)
        self.wait_for(lambda: self.log.exists())
        watch.terminate()
        watch.communicate(timeout=4)
        join = self.process("latest", self.state)
        other = self.process("run", self.state, "codex", "another-model")
        for p in (join, other):
            out, _ = p.communicate(timeout=9)
            events = [json.loads(line) for line in out.splitlines()]
            self.assertEqual(events[-1]["event"], "end")
            self.assertEqual(sum(e["event"] == "answer" for e in events), 4)
        self.assertEqual(len(self.reports()), 1)
        self.assertEqual([provider for provider, _, _ in rows(self.log)], ["grok"] * 4)

    def test_stop_is_explicit_and_survives_replay(self):
        watch = self.process("run", self.state, "grok", delay=30)
        self.wait_for(self.log.exists)
        result, _ = watch.communicate("stop\n", timeout=7)
        self.assertEqual(json.loads(result.splitlines()[-1])["event"], "stopped")
        result, _ = self.process("latest", self.state).communicate(timeout=4)
        self.assertEqual(json.loads(result.splitlines()[-1])["event"], "stopped")
        self.assertEqual(len(rows(self.log)), 1)

    def test_provider_error_stops_without_scoring_the_item(self):
        self.env["PUZZLLM_CODEX"] = str(self.root / "missing")
        proc = self.process("run", self.state, "codex")
        result, _ = proc.communicate(timeout=5)
        events = [json.loads(line) for line in result.splitlines()]
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(events[-1]["event"], "error")
        self.assertFalse(any(e["event"] == "answer" for e in events))

    def test_local_catalogs_include_installed_tags_and_nested_ggufs(self):
        posts, prompts = [], []
        class Server(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"models":[{"name":"example:cloud"},{"name":"small:v2"}]}')
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                posts.append((self.path, data["model"]))
                missing = self.path == "/api/show" and data["model"] != "present:v1"
                self.send_response(404 if missing else 200)
                self.end_headers()
                if self.path == "/api/chat":
                    prompts.append(data)
                    self.wfile.write(b'{"message":{"thinking":"private reasoning","content":"NO"}}')
                else:
                    self.wfile.write(b'{"status":"success"}\n')
        with ThreadingHTTPServer(("127.0.0.1", 0), Server) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                with patch.dict(os.environ, OLLAMA_HOST="127.0.0.1:" + str(server.server_port)):
                    self.assertEqual(worker.local_models("ollama"), ["example:cloud", "small:v2"])
                    worker.ensure_ollama("someone/custom:v3", lambda _text: None)
                    with worker.answerer("ollama", "present:v1", self.root, None, lambda _text: None) as ask:
                        self.assertEqual(ask("Is two odd?"), "NO")
            finally:
                server.shutdown()
                thread.join()
        self.assertEqual(posts, [("/api/show", "someone/custom:v3"), ("/api/pull", "someone/custom:v3"),
                                 ("/api/show", "present:v1"), ("/api/chat", "present:v1")])
        self.assertIs(prompts[0]["think"], False)
        self.assertEqual(prompts[0]["messages"], [
            {"role": "system", "content": "Respond with only YES or NO."},
            {"role": "user", "content": "Is two odd?"}])
        folder = self.root / "models/subdir"
        folder.mkdir(parents=True)
        files = [folder/f"choice {i}.GGUF" for i in range(35)]
        for path in files:
            path.touch()
        with patch.dict(os.environ, KILIX_LLM_MODELS_DIR=str(folder.parent), PUZZLLM_MODEL=""):
            self.assertEqual(set(worker.local_models("llama.cpp")), set(map(str, files)))


if __name__ == "__main__":
    unittest.main()
