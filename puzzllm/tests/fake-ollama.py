"""A local Ollama stand-in for discovery and streaming completion tests."""
import http.server
import json
from pathlib import Path
import sys


class OllamaHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/ps":
            marker = Path(sys.argv[1]).with_suffix(".context")
            context = int(marker.read_text()) if marker.exists() else 32768
            vram_marker = marker.with_suffix(".vram")
            vram = int(vram_marker.read_text()) if vram_marker.exists() else 0
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps({"models": [
                {"name": name, "context_length": context, "size_vram": vram}
                for name in ("test/agent:7b", "test/always:latest", "test/other:latest")
            ]}).encode())
            return
        assert self.path == "/api/tags"
        self.send_response(200)
        self.end_headers()
        self.wfile.write(json.dumps({"models": [
            {"name": "test/agent:7b"},
            {"name": "test/always:latest"},
            {"name": "test/other:latest"},
            {"name": "test/remote:latest", "remote_host": "https://example.invalid"},
            {"name": "test/remote:cloud"},
        ]}).encode())

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        controls = {
            "test/agent:7b": [False, True],
            "test/always:latest": ["low", "medium", "high"],
            "test/other:latest": [False],
        }
        assert request["model"] in controls
        if self.path == "/api/show":
            values = controls[request["model"]]
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps({
                "thinking": {"values": values},
                "model_info": {"general.architecture": "test", "test.context_length": 65536},
            }).encode())
            return
        assert self.path == "/api/chat"
        assert request["options"]["num_ctx"] == 32768
        mode_marker = Path(sys.argv[1]).with_suffix(".compute")
        expected_gpu = int(mode_marker.read_text()) if mode_marker.exists() else None
        assert request["options"].get("num_gpu") == expected_gpu
        raw = request.get("think", "absent")
        effort = "none" if raw is False else "medium" if raw is True else raw
        message = {"role": "assistant", "content": f"Hello from the selected model ({effort})"}
        if request.get("tools"):
            if request["messages"][-1]["role"] == "tool":
                assert request["messages"][-1]["tool_name"] == "write"
                assert request["messages"][-2]["tool_calls"][0]["function"]["arguments"]["path"] == "hello_world.txt"
            else:
                message = {"role": "assistant", "tool_calls": [
                    {"function": {"name": "write", "arguments": {"path": "hello_world.txt"}}}
                ]}
        record = {"message": message, "done": True, "done_reason": "stop",
                  "prompt_eval_count": 11, "eval_count": 3}
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.end_headers()
        self.wfile.write(json.dumps(record).encode() + b"\n")


with http.server.ThreadingHTTPServer(("127.0.0.1", 0), OllamaHandler) as server:
    with open(sys.argv[1], "w") as port:
        print(server.server_port, file=port)
    server.serve_forever()
