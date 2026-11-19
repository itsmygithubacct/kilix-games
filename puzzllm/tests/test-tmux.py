"""Exercise the actual player-shell tmux server through the coding sandbox."""
import errno
import fcntl
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import termios
import time

sandbox = sys.argv[1]
log = Path(os.environ["PUZZLLM_STATE"]) / "workspace" / "shell.log"
env = os.environ | {"TERM": "xterm-256color"}
master, slave = os.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 20, 70, 0, 0))


def controlling_terminal():
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


proc = subprocess.Popen([sandbox, "shell"], env=env, stdin=slave, stdout=slave, stderr=slave,
                        start_new_session=True, preexec_fn=controlling_terminal)
os.close(slave)
terminal = bytearray()


def drain():
    if select.select([master], [], [], 0.05)[0]:
        try:
            terminal.extend(os.read(master, 65536))
        except OSError as error:
            if error.errno != errno.EIO:
                raise


def agent_tmux(*args):
    return subprocess.run([sandbox, "agent-exec", "--", "tmux", *args],
                          capture_output=True, text=True, timeout=2, env=env)


try:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and proc.poll() is None:
        drain()
        capture = agent_tmux("capture-pane", "-p", "-t", "shell")
        if capture.returncode == 0 and "kilix@sandbox" in capture.stdout:
            break
    else:
        raise AssertionError(f"shell did not attach: {terminal[-500:]!r}")

    os.write(master, b"printf 'puzzllm-tmux-%s\\n' ready\r")
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and proc.poll() is None:
        drain()
        capture = agent_tmux("capture-pane", "-p", "-t", "shell")
        if ("puzzllm-tmux-ready" in capture.stdout.splitlines() and log.exists() and
                b"puzzllm-tmux-ready" in log.read_bytes()):
            break
    else:
        raise AssertionError(f"capture or log did not get shell output: {capture.stderr}")

    mouse = agent_tmux("show-options", "-gv", "mouse")
    assert mouse.returncode == 0 and mouse.stdout.strip() == "on"
    os.write(master, b"exit\r")
    proc.wait(timeout=5)
    assert agent_tmux("has-session", "-t", "shell").returncode != 0
    assert b"puzzllm-tmux-ready" in log.read_bytes()
finally:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    os.close(master)
