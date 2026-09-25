#!/usr/bin/env python3
"""Terminal lifecycle check (run by `make test`): the game must hand the
terminal back exactly as it found it however it ends.

For each way out (QUIT on the menu, Ctrl+C typed, and SIGINT, SIGQUIT,
SIGTERM, SIGHUP sent from outside) it starts ./tictactoe-tui on a fresh
pseudo-terminal, waits until the game is drawing, ends it, and checks that the
terminal's settings equal the ones it started with and that the alternate
screen was left.
"""
import os
import pty
import select
import signal
import sys
import tempfile
import termios
import time

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tictactoe-tui")
LEAVE = b"\x1b[?1049l"


def read_for(fd, seconds):
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
    return out


def run(case, data_home):
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.close(master)
        for fd in (0, 1, 2):
            os.dup2(slave, fd)
        if slave > 2:
            os.close(slave)
        env = {"PATH": "/usr/bin:/bin", "TERM": "xterm-256color", "XDG_DATA_HOME": data_home,
               "HOME": data_home}
        os.execve(BIN, [BIN], env)
    out = read_for(master, 1.0)
    if case == "menu-quit":
        os.write(master, b"\x1b")            # Esc selects QUIT
        out += read_for(master, 0.3)
        os.write(master, b"\r")
    elif case == "ctrl-c":
        os.write(master, b"\x03")
    else:
        os.kill(pid, getattr(signal, case))
    deadline = time.time() + 5
    status = None
    while time.time() < deadline:
        out += read_for(master, 0.1)
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            break
    else:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        return f"{case}: did not exit"
    out += read_for(master, 0.2)
    after = termios.tcgetattr(slave)
    os.close(master)
    os.close(slave)
    problems = []
    if after != before:
        problems.append("terminal settings not restored")
    if LEAVE not in out:
        problems.append("alternate screen not left")
    return f"{case}: " + ("; ".join(problems) if problems else "ok")


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as data_home:
        for case in ("menu-quit", "ctrl-c", "SIGINT", "SIGQUIT", "SIGTERM", "SIGHUP"):
            line = run(case, data_home)
            ok = line.endswith(": ok")
            failures += not ok
            print(("PASS: " if ok else "FAIL: ") + "terminal restored after " + line)
    if failures:
        print(f"pty-check: {failures} failure(s)", file=sys.stderr)
        return 1
    print("pty-check: the terminal is restored every way the game can end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
