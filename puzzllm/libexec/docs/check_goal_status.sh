#!/bin/sh
# Query the running game's grader. Exit 0 for complete, 1 for work remaining, 2 for unavailable.
if [ "${PUZZLLM_FILE_CONTROLS:-}" = 1 ]; then
    exec python3 -I /opt/puzzllm/bin/puzzllm_ipc.py goals
fi
exec python3 -I -c '
import socket, sys
try:
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(12)
        connection.connect("/opt/puzzllm/kx/goals/status.sock")
        with connection.makefile("rb") as reply:
            code = reply.readline(8)
            if code not in (b"0\n", b"1\n", b"2\n"):
                raise OSError("incomplete response from the game")
            sys.stdout.buffer.write(reply.read())
    sys.exit(int(code))
except OSError as error:
    print("The game grader is unavailable:", error, file=sys.stderr)
    sys.exit(2)
'
