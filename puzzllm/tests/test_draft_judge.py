"""The draft-and-judge Ollama choice: discovery, acceptance, extra drafts, the judge's pick, and fallbacks."""
import http.client
import http.server
import json
import os
import runpy
import socket
import tempfile
import threading
import unittest
from pathlib import Path

CHOICE = "qwen3.5-4b_draft_d3-4b_judge_ollama"


class Ollama(http.server.BaseHTTPRequestHandler):
    """Stands in for Ollama: the generator names its seed, the judge rates by a table."""
    installed = ["d3:4b", "qwen3.5:4b"]
    ratings = {}
    judge_works = True
    chats = []
    judged = []

    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        self.send_response(status)
        self.end_headers()
        self.wfile.write(json.dumps(body).encode())

    def do_GET(self):
        if self.path == "/api/ps":
            self.reply({"models": [{"name": "qwen3.5:4b", "context_length": 32768, "size_vram": 1}]})
        else:
            self.reply({"models": [{"name": name} for name in self.installed]})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/api/show":
            assert request["model"] == "qwen3.5:4b"
            self.reply({"thinking": {"values": [False, True]},
                        "model_info": {"general.architecture": "test", "test.context_length": 65536}})
        elif self.path == "/api/chat":
            assert request["model"] == "qwen3.5:4b" and request["stream"] is False
            Ollama.chats.append(request["options"])
            seed = request["options"]["seed"]
            text = "same" if seed in (3, 4) else f"draft {seed}"
            message = {"role": "assistant", "content": text}
            if seed == 2:
                message["tool_calls"] = [{"function": {"name": "write", "arguments": {"path": "a.txt", "content": "x"}}}]
            self.reply({"message": message, "done": True, "done_reason": "stop",
                        "prompt_eval_count": 10, "eval_count": 2})
        elif self.path == "/v1/systemone" and self.judge_works:
            assert request["model"] == "d3:4b" and "list the files" in request["state"]["request"]
            answers = {}
            for name, question in request["questions"].items():
                reply = question["instructions"].split("\n\n", 1)[1]
                Ollama.judged.append(reply)
                answers[name] = {"type": "noul", "noul": self.ratings.get(reply.split("\n")[0], 0.1)}
            self.reply({"answers": answers})
        else:
            self.reply({"error": "no judge"}, 500)


class DraftJudge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ollama = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Ollama)
        threading.Thread(target=cls.ollama.serve_forever, daemon=True).start()
        os.environ["OLLAMA_HOST"] = f"127.0.0.1:{cls.ollama.server_port}"
        cls.bridge = runpy.run_path(str(Path(__file__).parent.parent / "libexec/puzzllm-ollama"))
        cls.folder = tempfile.TemporaryDirectory()
        cls.socket = os.path.join(cls.folder.name, "s")
        cls.server = cls.bridge["HostServer"](cls.socket, cls.bridge["ChatHandler"])
        cls.server.model, cls.server.think, cls.server.context, cls.server.compute = CHOICE, False, 32768, "auto"
        cls.server.write_schema = {"name": "write", "parameters": {"properties": {}}}
        cls.server.journal = os.path.join(cls.folder.name, "journal")
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.ollama.shutdown()
        cls.ollama.server_close()
        cls.folder.cleanup()

    def setUp(self):
        Ollama.installed, Ollama.ratings, Ollama.judge_works = ["d3:4b", "qwen3.5:4b"], {}, True
        Ollama.chats, Ollama.judged = [], []

    def ask(self, stream=False):
        connection = http.client.HTTPConnection("localhost")
        connection.sock = socket.socket(socket.AF_UNIX)
        connection.sock.connect(self.socket)
        body = {"model": CHOICE, "stream": stream, "messages": [
            {"role": "system", "content": "be brief"}, {"role": "user", "content": "list the files"}]}
        if stream:
            body["stream_options"] = {"include_usage": True}
        connection.request("POST", "/v1/chat/completions", json.dumps(body), {"Content-Type": "application/json"})
        with connection.getresponse() as response:
            return response.status, response.read().decode()

    def test_offered_only_with_both_models(self):
        self.assertIn(CHOICE, self.bridge["offered_models"]())
        Ollama.installed = ["qwen3.5:4b"]
        self.assertNotIn(CHOICE, self.bridge["offered_models"]())
        self.assertEqual(self.ask()[0], 409)

    def test_accepted_first_draft_costs_one_draft(self):
        Ollama.ratings = {"draft 0": 0.95}
        status, text = self.ask()
        reply = json.loads(text)
        self.assertEqual((status, reply["choices"][0]["message"]["content"]), (200, "draft 0"))
        self.assertEqual([options["temperature"] for options in Ollama.chats], [0])
        self.assertEqual(reply["usage"]["total_tokens"], 12)

    def test_low_rating_buys_drafts_and_the_judge_picks(self):
        Ollama.ratings = {"draft 0": 0.2, "draft 2": 0.8, "same": 0.5}
        status, text = self.ask()
        reply = json.loads(text)
        message = reply["choices"][0]["message"]
        self.assertEqual((status, message["content"], reply["choices"][0]["finish_reason"]), (200, "draft 2", "tool_calls"))
        self.assertEqual(json.loads(message["tool_calls"][0]["function"]["arguments"])["path"], "a.txt")
        self.assertEqual([options["temperature"] for options in Ollama.chats], [0, 0.8, 0.8, 0.8, 0.8])
        self.assertEqual(Ollama.judged.count("same"), 1)             # a repeated draft is judged once
        self.assertTrue(any("Tool call: write" in reply for reply in Ollama.judged))
        self.assertEqual(reply["usage"]["total_tokens"], 60)         # all five drafts are counted
        entry = json.loads(Path(self.server.journal).read_text().splitlines()[-1])
        self.assertEqual((entry["drafts"], entry["picked"]), (4, 2))

    def test_first_draft_wins_a_tie_and_streams(self):
        Ollama.ratings = {}                                           # every draft rated 0.1
        status, text = self.ask(stream=True)
        events = [json.loads(line[6:]) for line in text.splitlines() if line.startswith("data: {")]
        self.assertEqual(status, 200)
        self.assertIn(": draft", text)
        self.assertEqual(events[1]["choices"][0]["delta"]["content"], "draft 0")
        self.assertEqual(events[-1]["usage"]["total_tokens"], 60)
        self.assertTrue(text.rstrip().endswith("data: [DONE]"))

    def test_without_a_judge_the_first_draft_is_returned(self):
        Ollama.judge_works = False
        status, text = self.ask()
        self.assertEqual((status, json.loads(text)["choices"][0]["message"]["content"]), (200, "draft 0"))
        self.assertEqual(len(Ollama.chats), 1)


if __name__ == "__main__":
    unittest.main()
