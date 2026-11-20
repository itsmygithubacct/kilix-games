"""Small local requests for game clients that can use files but cannot open sockets."""
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import time
from collections import deque

TOKEN = re.compile(r"[0-9a-f]{32}", re.ASCII)
LIMIT = 4096  # one atomic Linux pipe write


class Mailbox:
    def __init__(self, directory):
        directory = Path(directory)
        directory.mkdir(mode=0o700, exist_ok=True)
        self.folder = os.open(directory, os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.mkfifo("requests", 0o600, dir_fd=self.folder)
        except FileExistsError:
            if not stat.S_ISFIFO(os.stat("requests", dir_fd=self.folder, follow_symlinks=False).st_mode):
                os.close(self.folder)
                raise ValueError("invalid request pipe")
        self.fd = os.open("requests", os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=self.folder)
        if not stat.S_ISFIFO(os.fstat(self.fd).st_mode):
            self.close()
            raise ValueError("invalid request pipe")
        self.replies = deque()
        self.pending = b""
        for name in os.listdir(self.folder):
            if name.startswith("reply-") and TOKEN.fullmatch(name[6:]):
                os.unlink(name, dir_fd=self.folder)

    def close(self):
        os.close(self.fd)
        os.close(self.folder)

    def respond(self, handler):
        self.pending += os.read(self.fd, LIMIT)
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            try:
                token, payload = json.loads(line)
                if not isinstance(token, str) or not TOKEN.fullmatch(token):
                    continue
            except (ValueError, TypeError, RecursionError):
                continue
            code, message = handler(payload)
            name = "reply-" + token
            tmp = ".new-" + secrets.token_hex(16)
            fd = os.open(tmp, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600, dir_fd=self.folder)
            try:
                with os.fdopen(fd, "wb") as out:
                    out.write(str(code).encode() + b"\n" + message)
                os.replace(tmp, name, src_dir_fd=self.folder, dst_dir_fd=self.folder)
            finally:
                try:
                    os.unlink(tmp, dir_fd=self.folder)
                except FileNotFoundError:
                    pass
            self.replies.append(name)
            if len(self.replies) > 32:
                try:
                    os.unlink(self.replies.popleft(), dir_fd=self.folder)
                except FileNotFoundError:
                    pass
        if len(self.pending) > LIMIT:
            self.pending = b""


def request(directory, payload):
    token = secrets.token_hex(16)
    packet = json.dumps([token, payload], separators=(",", ":")).encode() + b"\n"
    if len(packet) > LIMIT:
        raise ValueError("request exceeds the private game pipe limit")
    fd = os.open(directory / "requests", os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        os.write(fd, packet)
    finally:
        os.close(fd)
    path = directory / ("reply-" + token)
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        try:
            with path.open("rb") as reply:
                code = reply.readline(8)
                if code not in (b"0\n", b"1\n", b"2\n"):
                    raise ValueError("invalid game response")
                return int(code), reply.read()
        except FileNotFoundError:
            time.sleep(.03)
    raise TimeoutError("the game did not answer")


if __name__ == "__main__":
    name, *args = sys.argv[1:]
    if name not in ("goals", "controls"):
        sys.exit(2)
    try:
        code, message = request(Path("/opt/puzzllm/kx") / name, args)
        (sys.stdout.buffer if name == "goals" or code == 0 else sys.stderr.buffer).write(message)
        sys.exit(code)
    except (OSError, ValueError) as error:
        print("Game request failed:", error, file=sys.stderr)
        sys.exit(2)
