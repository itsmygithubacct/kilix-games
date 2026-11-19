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
            {"name": "test/other:latest"},
            {"name": "test/remote:latest", "remote_host": "https://example.invalid"},
            {"name": "test/remote:cloud"},
        ]}).encode())

    def do_POST(self):
        assert self.path == "/v1/chat/completions"
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert request["model"] == "test/agent:7b"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(b'data: Hello from the selected model\n\ndata: [DONE]\n\n')


with http.server.HTTPServer(("127.0.0.1", 0), OllamaHandler) as server:
    with open(sys.argv[1], "w") as port:
        print(server.server_port, file=port)
    server.serve_forever()
