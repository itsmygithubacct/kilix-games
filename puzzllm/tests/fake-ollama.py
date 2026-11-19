"""A local Ollama stand-in for discovery and streaming completion tests."""
import http.server
import json
import sys


class OllamaHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
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
            self.wfile.write(json.dumps({"thinking": {"values": values}}).encode())
            return
        assert self.path == "/v1/chat/completions"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        effort = request.get("reasoning_effort", "absent")
        self.wfile.write(f"data: Hello from the selected model ({effort})\n\ndata: [DONE]\n\n".encode())


with http.server.HTTPServer(("127.0.0.1", 0), OllamaHandler) as server:
    with open(sys.argv[1], "w") as port:
        print(server.server_port, file=port)
    server.serve_forever()
