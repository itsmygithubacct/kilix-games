#!/usr/bin/env python3
"""Run the application in an isolated PTY and verify terminal cleanup."""

from __future__ import annotations

import argparse
import contextlib
import errno
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time


MOUSE_ENTER = b"\x1b[?1003h\x1b[?1006h\x1b[?1016h"
MOUSE_LEAVE = b"\x1b[?1016l\x1b[?1006l\x1b[?1003l"
KEYBOARD_ENTER = b"\x1b[>15u"
KEYBOARD_LEAVE = b"\x1b[<u"
CURSOR_SHOW = b"\x1b[?25h"
ALT_SCREEN_LEAVE = b"\x1b[?1049l"
SYNC_LEAVE = b"\x1b[?2026l"
KITTY_FRAME = re.compile(
    rb"\x1b_Ga=T,f=24,i=\d+,q=2,o=z,s=(\d+),v=(\d+),m=[01];"
)
DEFAULT_TIMEOUT = 8.0
DEFAULT_OUTPUT_CAP = 32 * 1024 * 1024
SKIP_ERRNOS = {
    errno.ENOSYS,
    errno.ENODEV,
    errno.ENOTSUP,
    getattr(errno, "EOPNOTSUPP", errno.ENOTSUP),
}


class SmokeFailure(Exception):
    """A concise, user-facing smoke-test failure."""


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Launch the application under an isolated PTY, observe its terminal "
            "protocol, quit with q, and verify clean restoration."
        )
    )
    parser.add_argument(
        "--binary",
        required=True,
        type=Path,
        help="path to the application binary",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        metavar="SECONDS",
        help=f"hard timeout for the entire run (default: {DEFAULT_TIMEOUT:g})",
    )
    parser.add_argument(
        "--output-cap",
        type=int,
        default=DEFAULT_OUTPUT_CAP,
        metavar="BYTES",
        help=f"maximum captured PTY output (default: {DEFAULT_OUTPUT_CAP})",
    )
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.output_cap <= 0:
        parser.error("--output-cap must be greater than zero")
    return args


def _skip(reason: str) -> int:
    print(f"pty_smoke: SKIP: {reason}")
    return 0


def _tail(data: bytearray, length: int = 512) -> str:
    return repr(bytes(data[-length:]))


def _kill_child(proc: subprocess.Popen[bytes]) -> None:
    if proc.poll() is not None:
        return
    with contextlib.suppress(ProcessLookupError):
        os.killpg(proc.pid, signal.SIGKILL)
    with contextlib.suppress(subprocess.TimeoutExpired):
        proc.wait(timeout=1.0)


def _read_available(master_fd: int, output: bytearray, output_cap: int) -> bool:
    """Read one nonblocking chunk; return False at PTY EOF."""
    try:
        chunk = os.read(master_fd, min(65536, output_cap - len(output) + 1))
    except BlockingIOError:
        return True
    except OSError as exc:
        if exc.errno == errno.EIO:
            return False
        raise
    if not chunk:
        return False
    output.extend(chunk)
    if len(output) > output_cap:
        raise SmokeFailure(f"PTY output exceeded --output-cap ({output_cap} bytes)")
    return True


def run_smoke(
    args: argparse.Namespace, pty_module: object, fcntl_module: object, termios_module: object
) -> int:
    binary = args.binary.expanduser()
    if not binary.exists():
        raise SmokeFailure(f"binary does not exist: {binary}")
    if not binary.is_file():
        raise SmokeFailure(f"binary is not a regular file: {binary}")
    if not os.access(binary, os.X_OK):
        raise SmokeFailure(f"binary is not executable: {binary}")
    binary = binary.resolve()

    if not hasattr(termios_module, "TIOCSWINSZ"):
        return _skip("termios.TIOCSWINSZ is unavailable")

    try:
        master_fd, slave_fd = pty_module.openpty()  # type: ignore[attr-defined]
    except OSError as exc:
        if exc.errno in SKIP_ERRNOS:
            return _skip(f"PTY allocation is unavailable: {exc}")
        raise SmokeFailure(f"cannot allocate PTY: {exc}") from exc

    check_fd = -1
    proc: subprocess.Popen[bytes] | None = None
    output = bytearray()
    frame_width = 0
    frame_height = 0
    frame_end = -1
    q_sent = False
    master_open = True

    try:
        try:
            fcntl_module.ioctl(  # type: ignore[attr-defined]
                slave_fd,
                termios_module.TIOCSWINSZ,  # type: ignore[attr-defined]
                struct.pack("HHHH", 48, 160, 1600, 960),
            )
            initial_termios = termios_module.tcgetattr(slave_fd)  # type: ignore[attr-defined]
            initial_flags = fcntl_module.fcntl(  # type: ignore[attr-defined]
                slave_fd, fcntl_module.F_GETFL  # type: ignore[attr-defined]
            )
            check_fd = os.dup(slave_fd)
        except OSError as exc:
            if exc.errno in SKIP_ERRNOS:
                return _skip(f"required PTY operations are unavailable: {exc}")
            raise SmokeFailure(f"cannot configure PTY: {exc}") from exc

        environment = os.environ.copy()
        environment.update(
            {
                "KILIX_LIGHTS_SKIP_PROBE": "1",
                "KILIX_LIGHTS_AUDIO": "off",
                "TERM": "xterm-kitty",
            }
        )
        try:
            proc = subprocess.Popen(
                [str(binary)],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                env=environment,
                close_fds=True,
                start_new_session=True,
            )
        except OSError as exc:
            raise SmokeFailure(f"cannot launch {binary}: {exc}") from exc
        finally:
            os.close(slave_fd)
            slave_fd = -1

        current_flags = fcntl_module.fcntl(  # type: ignore[attr-defined]
            master_fd, fcntl_module.F_GETFL  # type: ignore[attr-defined]
        )
        fcntl_module.fcntl(  # type: ignore[attr-defined]
            master_fd,
            fcntl_module.F_SETFL,  # type: ignore[attr-defined]
            current_flags | os.O_NONBLOCK,
        )

        import select

        deadline = time.monotonic() + args.timeout
        exited_at: float | None = None
        while True:
            now = time.monotonic()
            if now >= deadline:
                missing = []
                if MOUSE_ENTER not in output:
                    missing.append("mouse enter")
                if KEYBOARD_ENTER not in output:
                    missing.append("keyboard enter")
                if frame_end < 0:
                    missing.append("Kitty frame")
                detail = ", ".join(missing) if missing else "clean process exit"
                raise SmokeFailure(f"timed out after {args.timeout:g}s waiting for {detail}")

            if master_open:
                readable, _, _ = select.select([master_fd], [], [], min(0.05, deadline - now))
                if readable:
                    master_open = _read_available(master_fd, output, args.output_cap)
            else:
                time.sleep(min(0.01, max(0.0, deadline - now)))

            if frame_end < 0:
                match = KITTY_FRAME.search(output)
                if match is not None:
                    frame_width = int(match.group(1))
                    frame_height = int(match.group(2))
                    frame_end = match.end()

            if (
                not q_sent
                and MOUSE_ENTER in output
                and KEYBOARD_ENTER in output
                and frame_end >= 0
            ):
                try:
                    os.write(master_fd, b"q")
                except BlockingIOError:
                    continue
                except OSError as exc:
                    raise SmokeFailure(f"cannot send q through PTY: {exc}") from exc
                q_sent = True

            return_code = proc.poll()
            if return_code is not None:
                if exited_at is None:
                    exited_at = time.monotonic()
                # Drain output after exit. PTY EOF is definitive; otherwise a
                # short quiet interval is enough for already-buffered bytes.
                if not master_open or time.monotonic() - exited_at >= 0.15:
                    break

        return_code = proc.wait(timeout=max(0.01, deadline - time.monotonic()))
        if return_code != 0:
            raise SmokeFailure(
                f"process exited with status {return_code}; output tail: {_tail(output)}"
            )
        if not q_sent:
            raise SmokeFailure(
                "process exited before mouse/keyboard enter sequences and a Kitty "
                f"frame were observed; output tail: {_tail(output)}"
            )

        restoration_markers = (
            ("keyboard pop", KEYBOARD_LEAVE),
            ("mouse leave", MOUSE_LEAVE),
            ("cursor show", CURSOR_SHOW),
            ("alternate-screen leave", ALT_SCREEN_LEAVE),
            ("synchronized-output leave", SYNC_LEAVE),
        )
        missing_restoration = [
            label
            for label, marker in restoration_markers
            if output.find(marker, frame_end) < 0
        ]
        if missing_restoration:
            raise SmokeFailure(
                "missing restoration sequence(s): "
                + ", ".join(missing_restoration)
                + f"; output tail: {_tail(output)}"
            )

        restored_termios = termios_module.tcgetattr(check_fd)  # type: ignore[attr-defined]
        restored_flags = fcntl_module.fcntl(  # type: ignore[attr-defined]
            check_fd, fcntl_module.F_GETFL  # type: ignore[attr-defined]
        )
        if restored_termios != initial_termios:
            raise SmokeFailure("child did not restore the PTY termios attributes")
        if (restored_flags & os.O_NONBLOCK) != (initial_flags & os.O_NONBLOCK):
            raise SmokeFailure("child did not restore the PTY O_NONBLOCK state")

        print(
            f"pty_smoke: ok (Kitty frame {frame_width}x{frame_height}, "
            f"{len(output)} PTY bytes)"
        )
        return 0
    finally:
        if proc is not None:
            _kill_child(proc)
        if slave_fd >= 0:
            os.close(slave_fd)
        if check_fd >= 0:
            os.close(check_fd)
        os.close(master_fd)


def main() -> int:
    args = parse_args(sys.argv[1:])
    if os.name != "posix":
        return _skip("POSIX PTYs are unavailable on this platform")

    try:
        import fcntl
        import pty
        import termios
    except ImportError as exc:
        return _skip(f"required POSIX PTY module is unavailable: {exc.name}")

    try:
        return run_smoke(args, pty, fcntl, termios)
    except SmokeFailure as exc:
        print(f"pty_smoke: FAIL: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"pty_smoke: FAIL: PTY operation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
