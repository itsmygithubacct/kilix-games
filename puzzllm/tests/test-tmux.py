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


fake_grok = str(Path(sandbox).resolve().parent.parent / "tests" / "fake-grok")


def agent_tmux(*args):
    return subprocess.run([sandbox, "agent-exec", "--", "tmux", *args],
                          capture_output=True, text=True, timeout=2, env=env)


def grok_tmux(yolo):
    child_env = env | {"PUZZLLM_GROK": fake_grok}
    if yolo:
        child_env["PUZZLLM_YOLO"] = "1"
    return subprocess.run([sandbox, "grok", "--", "capture-pane", "-p", "-t", "shell"],
                          capture_output=True, text=True, timeout=15, env=child_env)


def wait_for_log(marker):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and proc.poll() is None:
        drain()
        if log.exists() and marker in log.read_bytes():
            return
    raise AssertionError(f"live transcript did not receive {marker!r}")


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

    os.write(master,
             b"server=$(tmux display-message -p '#{pid}'); client=$(tmux display-message -p '#{client_pid}'); "
             b"test \"$(awk '/^PPid:/ {print $2}' /proc/$server/status)\" = \"$client\" && "
             b"printf 'puzzllm-%s\\n' pane-owned-server\r")
    wait_for_log(b"puzzllm-pane-owned-server")

    for yolo in (False, True):
        capture = grok_tmux(yolo)
        assert capture.returncode == 0 and "kilix@sandbox" in capture.stdout, (
            f"grok yolo={yolo} missed the sandbox tmux: {capture.stderr!r} {capture.stdout!r}")

    old = log.with_suffix(".log.old")
    log.replace(old)
    log.write_bytes(b"replacement\n")
    os.write(master, b"\x0cprintf 'puzzllm-tmux-%s\\n' replaced\r")
    wait_for_log(b"puzzllm-tmux-replaced")
    assert log.read_bytes().startswith(b"replacement\n")
    assert b"puzzllm-tmux-replaced" not in old.read_bytes()
    assert b"puzzllm-tmux-ready" in old.read_bytes()
    log.unlink()
    os.write(master, b"printf 'puzzllm-tmux-%s\\n' recreated\r")
    wait_for_log(b"puzzllm-tmux-recreated")

    os.write(master, b"exit\r")
    proc.wait(timeout=5)
    assert agent_tmux("has-session", "-t", "shell").returncode != 0
    assert b"puzzllm-tmux-recreated" in log.read_bytes()
finally:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    os.close(master)
