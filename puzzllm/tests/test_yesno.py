#!/usr/bin/env python3
"""Exercise native provider commands, durable runs and exact reply scoring offline."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import fcntl
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

WORKER = Path(__file__).resolve().parents[1] / "libexec/puzzllm-yesno"
sys.path.insert(0, str(WORKER.parent))
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
if args[:1] == ['app-server']:
    for line in sys.stdin:
        call = json.loads(line)
        if call['method'] == 'model/list':
            print(json.dumps({'id': 3, 'result': {'data': [
                {'model': 'example-base', 'isDefault': True}, {'model': 'example-pro'},
                {'model': 'hidden-id', 'hidden': True}]}}), flush=True)
            print(json.dumps({'id': 2, 'result': {'config': {'model': 'example-pro'}}}), flush=True)
            break
    sys.exit()
if args == ['models']:
    print('Default model: example-fast')
    print('* example-fast (default)')
    print('- example-large')
    sys.exit()
if '--input-format' in args:
    call = json.loads(sys.stdin.readline())
    assert call['request'] == {'subtype': 'initialize'}
    print(json.dumps({'type': 'control_response', 'response': {'request_id': call['request_id'],
          'response': {'models': [{'value': 'default'}, {'value': 'opus'}, {'value': 'sonnet'}]}}}))
    sys.exit()
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
elif provider == 'claude' and os.environ.get('BENCH_RESOLVED_MODEL'):
    print(json.dumps({'result': reply, 'modelUsage': {os.environ['BENCH_RESOLVED_MODEL']: {}}}))
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
                    "BENCH_CALLS": str(self.log), "PYTHONDONTWRITEBYTECODE": "1",
                    "HOME": str(self.root/"home"), "CODEX_HOME": str(self.root/"home/.codex"),
                    "GROK_HOME": str(self.root/"home/.grok"), "CLAUDE_CONFIG_DIR": str(self.root/"home/.claude"),
                    "ANTHROPIC_MODEL": ""}
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

    def result(self, *args):
        result = subprocess.run([WORKER, *args], env=self.env, capture_output=True, text=True,
                                stdin=subprocess.DEVNULL, timeout=10, check=True)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_hosted_model_catalogs_use_native_ids_and_caches(self):
        def names(provider):
            return [r["model"] for r in self.result("models", provider)]
        self.assertEqual(names("codex"), ["example-pro", "example-base"])
        self.assertEqual(names("grok"), ["example-fast", "example-large"])
        settings = Path(self.env["CLAUDE_CONFIG_DIR"])
        settings.mkdir(parents=True)
        (settings/"settings.json").write_text(json.dumps({"model": "opus", "modelSettings": {"exact-version": {}}}))
        (settings/".claude.json").write_text(json.dumps({"additionalModelOptionsCache": [{"value": "long-context"}]}))
        self.assertEqual(names("claude"), ["opus", "sonnet", "long-context", "exact-version"])
        for provider, content, expected in (
                ("codex", [{"slug": "cache-pro", "visibility": "list"}, {"slug": "hidden", "visibility": "hide"}],
                 ["cache-pro"]),
                ("grok", {"cache-fast": {"info": {}}, "hidden": {"info": {"hidden": True}}}, ["cache-fast"])):
            home = Path(self.env[provider.upper()+"_HOME"])
            home.mkdir(parents=True)
            (home/"models_cache.json").write_text(json.dumps({"models": content, "unused": "private fixture"}))
            self.assertEqual(names(provider), expected)
        self.assertFalse(self.log.exists())

    def test_selected_and_configured_models_share_their_concrete_identity(self):
        for provider, concrete, alias in (("codex", "example-pro", "example-pro"),
                                          ("grok", "example-fast", "example-fast"),
                                          ("claude", "example-concrete", "opus")):
            self.env["BENCH_RESOLVED_MODEL"] = concrete
            for choice in ("", alias):
                self.result("run", self.state, provider, choice)
            board = self.result("records", self.state)
            model_rows = [r for r in board if r["event"] == "standing" and r["provider"] == provider]
            self.assertEqual(len(model_rows), 1)
            self.assertEqual((model_rows[0]["model"], model_rows[0]["runs"], model_rows[0]["avg_correct"]),
                             (concrete, 2, 2.0))
            calls = [args for name, args, _ in rows(self.log) if name == provider]
            if provider == "claude":
                self.assertNotIn("--model", calls[0])
                calls = calls[1:4]
            self.assertTrue(all(args[args.index("--model")+1] == concrete for args in calls))

    def test_rankings_rebuild_each_model_once_and_retain_every_attempt(self):
        folder = self.state / "bench/yesno"
        folder.mkdir(parents=True)
        def journal(n, provider, model, total, grades, end="end"):
            path = folder / f"20260101T0000{n:02}.000000Z-00000000.jsonl"
            events = [dict(event="begin", provider=provider, model=model, file=path.name, total=total)]
            events += [dict(event="answer", n=i, verdict=v) for i, v in enumerate(grades, 1)]
            if end:
                events.append(dict(event=end))
            path.write_text("".join(json.dumps(e)+"\n" for e in events))
            return path
        c, w = "correct", "wrong"
        first = journal(1, "ollama", "library/small", 4, [c,c,c,w])
        journal(2, "ollama", "small:latest", 2, [w,w])
        journal(3, "ollama", "small", 4, [c], "stopped")
        journal(4, "ollama", "small", 4, [], "error")
        journal(5, "ollama", "small", 4, [c,c], None)
        live = journal(6, "ollama", "small", 4, [c], None)
        journal(7, "codex", "tie", 8, [c,c,c,w,w,w,w,w])
        journal(8, "codex", "top", 1, [c])
        journal(9, "codex", "", 1, [c])
        journal(10, "ollama", "small", 4, [c])  # incomplete terminal record
        bad = journal(11, "grok", "damaged", 1, [], None)
        with bad.open("a") as out:
            out.write("invalid line\n")
        broken = journal(12, "grok", ["invalid id"], 0, [])
        self.assertTrue(broken.exists())
        with live.open("a") as writer:
            fcntl.flock(writer, fcntl.LOCK_EX)
            board = self.result("records", self.state)
            self.assertEqual(board, self.result("records", self.state))
        ranks = [r for r in board if r["event"] == "standing"]
        self.assertEqual([(r["rank"], r["model"]) for r in ranks if r["rank"]],
                         [(1, "top"), (2, "tie"), (2, "small:latest")])
        small = next(r for r in ranks if r["model"] == "small:latest")
        self.assertEqual((small["runs"], small["finished"], small["avg_correct"], small["avg_percent"]),
                         (7, 2, 1.5, 37.5))
        self.assertEqual(sum(r["runs"] for r in ranks), 12)
        self.assertEqual({r["status"] for r in board if r["event"] == "saved_run" and r["model"] == small["model"]},
                         {"Complete", "Failed", "Interrupted", "Running", "Stopped"})
        self.assertTrue(any(not r["model"] and r["rank"] == 0 and r["provider"] == "codex" for r in ranks))
        self.assertEqual(self.result("report", self.state, first.name), rows(first))
        path = self.root/"models"/"one.gguf"
        path.parent.mkdir()
        path.touch()
        link = self.root/"alias.gguf"
        link.symlink_to(path)
        for i, name in enumerate((link, path, self.root/"other"/path.name), 13):
            journal(i, "llama.cpp", str(name), 1, [c])
        gguf = [r for r in self.result("records", self.state) if r["event"] == "standing" and r["provider"] == "llama.cpp"]
        self.assertEqual({r["model"]: r["runs"] for r in gguf}, {str(path): 2, str(self.root/"other"/path.name): 1})

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

    def test_worker_saves_versioned_rankings_and_restores_without_journals(self):
        self.result("run", self.state, "codex", "chosen-model")
        path = self.state/"bench/yesno/rankings.json"
        original = path.read_bytes()
        board = json.loads(original)
        self.assertEqual((board["schema_version"], board["benchmark"], board["split"], board["total_runs"]),
                         (1, "OneBit", "dev", 1))
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertIn(b'\n  "models": [\n', original)
        group, = board["models"]
        self.assertEqual((group["provider"], group["model"], group["rank"], group["finished"], group["avg_percent"]),
                         ("codex", "chosen-model", 1, 1, 50.0))
        run, = group["history"]
        self.assertEqual(run["file"], self.reports()[0].name)
        self.assertRegex(run["started"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$")
        self.assertTrue(run["journal_available"])
        restored = self.root/"restored"
        recovered = restored/"bench/yesno/rankings.json"
        recovered.parent.mkdir(parents=True)
        recovered.write_bytes(original)
        self.result("run", restored, "codex", "chosen-model")
        group, = json.loads(recovered.read_bytes())["models"]
        self.assertEqual((group["runs"], group["finished"], group["avg_correct"]), (2, 2, 2.0))
        previous = next(r for r in group["history"] if r["file"] == run["file"])
        self.assertFalse(previous["journal_available"])
        self.assertEqual(previous["status"], "Complete")
        self.assertIn("Complete (summary only)", [r.get("status") for r in self.result("records", restored)])

    def test_snapshot_repair_and_failed_atomic_replace_preserve_previous_data(self):
        self.result("run", self.state, "codex", "one")
        folder = self.state/"bench/yesno"
        path = folder/"rankings.json"
        original = path.read_bytes()
        same = path.stat().st_ino
        self.result("records", self.state)
        self.assertEqual((path.stat().st_ino, path.read_bytes()), (same, original))
        self.reports()[0].rename(folder/"archived")
        with patch("puzzllm_yesno_records.os.replace", side_effect=OSError("fixture interrupted write")):
            with self.assertRaises(OSError):
                worker.save_rankings(folder)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(folder.glob(".rankings-*")), [])
        (folder/"archived").rename(folder/json.loads(original)["models"][0]["history"][0]["file"])
        path.write_bytes(b"part of an invalid file")
        self.result("latest", self.state)
        self.assertEqual(path.read_bytes(), original)
        backup, = folder.glob("rankings-damaged-*.json")
        self.assertEqual(backup.read_bytes(), b"part of an invalid file")
        newer = b'{"schema_version":22}\n'
        path.write_bytes(newer)
        events = self.result("records", self.state)
        self.assertEqual(path.read_bytes(), newer)
        self.assertEqual(events[0]["runs"], 1)
        self.assertIn("Unrecognized", events[-1]["text"])

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
        group, = json.loads((self.state/"bench/yesno/rankings.json").read_bytes())["models"]
        self.assertEqual((group["runs"], group["finished"], group["history"][0]["status"]), (1, 1, "Complete"))

    def test_rankings_finish_on_disk_with_every_monitor_closed(self):
        path = self.state/"bench/yesno/rankings.json"
        watch = self.process("run", self.state, "grok", "named", delay=0.2)
        self.wait_for(self.log.exists)
        watch.terminate()
        watch.communicate(timeout=4)
        self.wait_for(lambda: json.loads(path.read_bytes())["models"][0]["finished"] == 1)
        data = json.loads(path.read_bytes())
        self.assertEqual((data["total_runs"], data["models"][0]["model"], data["models"][0]["avg_correct"]),
                         (1, "named", 2.0))

    def test_stop_is_explicit_and_survives_replay(self):
        watch = self.process("run", self.state, "grok", delay=30)
        self.wait_for(self.log.exists)
        result, _ = watch.communicate("stop\n", timeout=7)
        self.assertEqual(json.loads(result.splitlines()[-1])["event"], "stopped")
        group, = json.loads((self.state/"bench/yesno/rankings.json").read_bytes())["models"]
        self.assertEqual((group["runs"], group["rank"], group["history"][0]["status"]), (1, None, "Stopped"))
        result, _ = self.process("latest", self.state).communicate(timeout=4)
        self.assertEqual(json.loads(result.splitlines()[-1])["event"], "stopped")
        self.assertEqual(len(rows(self.log)), 1)

    def test_game_launcher_finds_user_installed_clis_without_a_login_shell(self):
        home = self.root / "home"
        for provider, location in (("grok", ".grok/bin"), ("claude", ".local/bin")):
            folder = home / location
            folder.mkdir(parents=True)
            (folder/provider).symlink_to(self.root/provider)
            with patch.dict(os.environ, HOME=str(home), PATH=os.defpath,
                            BENCH_CALLS=str(self.log), **{"PUZZLLM_"+provider.upper(): ""}):
                answer = worker.hosted_answer(provider, "", "Is two even?", self.root)
                self.assertEqual(answer.strip(), "YES")

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
