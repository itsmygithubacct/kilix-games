"""Capture and route input for the two windows on the game's private display."""
import base64, binascii, json, os, re, select, signal, socket, stat, subprocess, sys, time
from pathlib import Path
from tempfile import NamedTemporaryFile
from puzzllm_ipc import Mailbox

# Read-only modules from the installed Kilix Pane Center and claudex.
sys.path[:0] = ["/opt/puzzllm/idle/center", "/opt/puzzllm/idle/claudex"]

root = "/opt/puzzllm/kx"
listen = "unix:/opt/puzzllm/kx/listen.sock"
fresh_agent = False


def opening_without_prompt():
    try:
        return Path(root, "agent-prompt").stat().st_size == 0
    except OSError:
        return False


def window_id(kind):
    cls = "puzzllm-agent" if kind == "agent" else "puzzllm-shell"
    try:
        out = subprocess.run(["xdotool", "search", "--class", cls],
                             capture_output=True, text=True, timeout=2).stdout.split()
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return out[0] if out else ""

def pane_match(role):
    return "env:^PUZZLLM_PANE$=^" + role + "$"

def restricted_control(kitten, args):
    """Expose pane inspection and focus to the network-isolated coding tool."""
    denied = (2, b"Restricted game controls support listing, reading and focusing the game panes.\n")
    if not isinstance(args, list) or not args or not all(isinstance(a, str) for a in args):
        return denied
    cmd, *options = args
    if cmd not in ("ls", "get-text", "focus-window", "focus-tab"):
        return denied
    match = None
    if len(options) >= 2 and options[0] == "--match":
        match, options = options[1], options[2:]
        if not re.fullmatch(r"id:[1-9][0-9]*", match) and match not in (pane_match("agent"), pane_match("shell")):
            return denied
    if cmd != "ls" and match is None:
        return denied
    if options and (cmd != "get-text" or any(o not in ("--extent=screen", "--extent=all",
                                                       "--ansi", "--add-cursor") for o in options)):
        return denied
    try:
        result = subprocess.run([kitten, "@", "--to", listen, *args], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=2)
        if result.returncode:
            return 1, result.stderr[:4096]
        if cmd == "ls":
            data = json.loads(result.stdout)
            for window in (w for oswin in data for tab in oswin["tabs"] for w in tab["windows"]):
                env = window.get("env", {})
                window["env"] = {k: env[k] for k in ("PUZZLLM_PANE",) if k in env}
            return 0, json.dumps(data).encode() + b"\n"
        if len(result.stdout) > 262144:
            return 2, b"Pane text exceeds the read limit. Request the visible screen.\n"
        return 0, result.stdout
    except (ValueError, KeyError, TypeError, OSError, subprocess.TimeoutExpired):
        return 2, b"The private game panes are unavailable.\n"

def launch_agent(kitten):
    global fresh_agent
    fresh_agent = opening_without_prompt()
    subprocess.run(
        [kitten, "@", "--to", listen, "launch", "--type=os-window",
         "--os-window-class=puzzllm-agent", "--os-window-title=agent",
         "--cwd=/kilix", "--env", "PUZZLLM_PANE=agent",
         "/opt/puzzllm/bin/puzzllm-kilix", "--pane", "agent"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, timeout=5, check=False)

def agent_window(kitten):
    args = [kitten, "@", "--to", listen, "ls", "--all-env-vars"]
    result = subprocess.run(args, capture_output=True, timeout=2, check=True)
    tree = json.loads(result.stdout)
    agents = [w for oswin in tree for tab in oswin["tabs"] for w in tab["windows"]
              if w.get("env", {}).get("PUZZLLM_PANE") == "agent"]
    if len(agents) != 1:
        return tree, None
    return tree, int(agents[0]["id"])


def ready_for_goal(kitten, tree, pane_id):
    from kilix_tui import kitty_rc, pane_center
    from idleness import assess_idle

    snapshot = pane_center.Inspector().snapshot(kitty_rc.parse(tree))
    pane = next((info.to_dict() for info in snapshot.panes if info.pane.id == pane_id), None)
    if not pane or (pane.get("coding_session") or {}).get("provider") != os.environ.get("PUZZLLM_AGENT"):
        return False
    screen = subprocess.run([kitten, "@", "--to", listen, "get-text", "--match", f"id:{pane_id}",
                             "--extent", "screen", "--ansi"], capture_output=True, text=True,
                            timeout=2, check=True)
    # A newly launched interactive Codex has no completed turn in its log.
    # Kilix supports its screen-only check for this first, untouched composer.
    first_prompt = (fresh_agent and pane["coding_session"]["provider"] == "codex"
                    and pane["coding_session"].get("session_id") == "unknown"
                    and not pane["coding_session"].get("last_turn_event"))
    return assess_idle(pane, screen.stdout, allow_screen_only=first_prompt)["ready"]


def send_goal(kitten, token=None):
    """An explicit click submits immediately. Auto claims one attempt per room."""
    global fresh_agent
    request = Path(root, "goals", "auto-request")
    receipt = Path(root, "auto-receipt")
    def current():
        try:
            return request.read_text().strip() == token and not (
                receipt.exists() and receipt.read_text().startswith(token + " "))
        except OSError:
            return False

    def record(status):
        with NamedTemporaryFile(mode="w", dir=root, delete=False) as temp:
            temp.write(f"{token} {status}\n")
        os.replace(temp.name, receipt)

    if token and not current():
        return
    try:
        tree, pane_id = agent_window(kitten)
        if pane_id is None:
            return
        if token:
            if not ready_for_goal(kitten, tree, pane_id) or not current():
                return
            record("attempted")
        fresh_agent = False
        target = ["--match", f"id:{pane_id}"]
        rc = [kitten, "@", "--to", listen]
        subprocess.run([*rc, "send-text", *target, "--stdin"], input=b"complete the goal in GOAL.md",
                       capture_output=True, timeout=2, check=True)
        subprocess.run([*rc, "send-key", *target, "enter"], stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=2, check=True)
        if token:
            record("sent")
    except ImportError:
        if token and current():
            record("unavailable")
    except (OSError, KeyError, RuntimeError, TypeError, ValueError, subprocess.SubprocessError) as error:
        # Keep the display bridge alive even when readiness cannot be read.
        print(f"Goal delivery unavailable: {error}", file=sys.stderr, flush=True)


def handle(line, kitten, sizes):
    global fresh_agent
    parts = line.split(" ", 3)
    if not parts or not parts[0]:
        return
    op = parts[0]
    if op == "goal" and parts == ["goal", "agent"]:
        send_goal(kitten)
        return
    if op == "auto-goal" and len(parts) == 2 and re.fullmatch(r"[0-9]{1,20}", parts[1]):
        send_goal(kitten, parts[1])
        return
    if op == "close-agent":
        subprocess.run([kitten, "@", "--to", listen, "close-window",
                        "--match", pane_match("agent")],
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=5, check=False)
        return
    if op == "restart-agent":
        handle("close-agent", kitten, sizes)
        launch_agent(kitten)
        return
    if len(parts) < 2 or parts[1] not in ("agent", "shell"):
        return
    pane = parts[1]
    if pane == "agent" and op in ("key", "type"):
        fresh_agent = False
    if op == "resize" and len(parts) == 4 and parts[2].isdigit() and parts[3].isdigit():
        sizes[pane] = (max(1, min(int(parts[2]), 1024)), max(1, min(int(parts[3]), 1280)))
        return
    wid = window_id(pane)
    if op == "focus":
        if not wid and pane == "agent":
            launch_agent(kitten)
        elif wid:
            subprocess.run(["xdotool", "windowfocus", wid],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=2, check=False)
        return
    if not wid:
        return
    if op == "click" and len(parts) >= 3:
        bits = parts[2].split(",")
        if len(bits) != 4 or not all(bit.isdigit() for bit in bits):
            return
        x, y, phase, button = bits
        phase = {"1": "mousedown", "2": "mouseup", "3": "click"}.get(phase, "mousemove")
        argv = ["xdotool", "mousemove", "--window", wid, x, y]
        if phase != "mousemove":
            argv += [phase, button]
        subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=2, check=False)
        return
    if op == "scroll" and len(parts) == 4 and parts[2].lstrip("-").isdigit():
        bits = parts[3].split(",")
        if len(bits) != 2 or not all(bit.isdigit() for bit in bits):
            return
        rows = int(parts[2])
        button = "5" if rows > 0 else "4"
        for _ in range(min(abs(rows), 12)):
            subprocess.run(["xdotool", "mousemove", "--window", wid, bits[0], bits[1],
                            "click", button],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=2, check=False)
        return
    if op == "key" and len(parts) == 3 and all(c.isalnum() or c in "+_-" for c in parts[2]):
        subprocess.run(["xdotool", "key", "--window", wid, "--", parts[2]],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=2, check=False)
        return
    if op == "type" and len(parts) == 3 and parts[1] in ("agent", "shell"):
        try:
            text = base64.b64decode(parts[2], validate=True).decode("utf-8", "replace")
        except (ValueError, binascii.Error):
            return
        # One write, ended with a carriage return. Per-key typing lets the
        # agent turn "goal" and GOAL.md into an unsubmitted slash command.
        submit = text.endswith("\n") or text.endswith("\r")
        body = text.rstrip("\r\n")
        payload = body.encode("utf-8") + (b"\r" if submit else b"")
        try:
            sent = subprocess.run(
                [kitten, "@", "--to", listen, "send-text",
                 "--match", pane_match(pane), "--stdin"],
                input=payload, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=3, check=False,
            ).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            sent = False
        if sent:
            return
        chunks = text.split("\n")
        for index, chunk in enumerate(chunks):
            if chunk:
                subprocess.run(["xdotool", "type", "--window", wid, "--delay", "0", "--", chunk],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=3, check=False)
            if index + 1 < len(chunks):
                subprocess.run(["xdotool", "key", "--window", wid, "Return"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=2, check=False)
        return

def capture(sizes, placed):
    # Xvfb has no window manager. Obscured pixels read as black. Keep each
    # window in its own half of the 2048-wide display before reading either.
    windows = {kind: window_id(kind) for kind in sizes}
    for kind, wid in windows.items():
        if not wid:
            placed.pop(kind, None)
            continue
        width, height = sizes[kind]
        geometry = (wid, width, height)
        if placed.get(kind) != geometry:
            x = "1024" if kind == "shell" else "0"
            moved = subprocess.run(
                ["xdotool", "windowmove", wid, x, "0", "windowsize", wid, str(width), str(height)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=2, check=False)
            if moved.returncode:
                return
            placed[kind] = geometry

    for kind, wid in windows.items():
        if not wid:
            continue
        name = kind + ".ppm"
        tmp = root + "/frames/" + name + ".tmp"
        dest = root + "/frames/" + name
        try:
            result = subprocess.run(["import", "-window", wid, "-depth", "8", "-silent", "ppm:" + tmp],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    timeout=2, check=False)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode == 0 and os.path.isfile(tmp) and os.path.getsize(tmp) > 16:
            os.replace(tmp, dest)


def main(kitty_bin, kitten, xvfb_pid):
    global fresh_agent
    fresh_agent = opening_without_prompt()
    config = "/opt/puzzllm/kilix-config/kitty.conf"
    os.environ["KITTY_LISTEN_ON"] = listen
    os.environ["PUZZLLM_KILIX_LISTEN"] = listen
    os.environ.pop("WAYLAND_DISPLAY", None)

    kitty = subprocess.Popen(
        [kitty_bin,
         "--config", config, "--config", "/opt/puzzllm/bin/puzzllm-kilix.conf",
         "--session", root + "/session.conf",
         "--listen-on", listen,
         "--override", "listen_on=" + listen,
         "--override", "allow_remote_control=yes"],
        stdin=subprocess.DEVNULL,
        stdout=open(root + "/logs/kitty.log", "ab"),
        stderr=subprocess.STDOUT,
        start_new_session=True)

    def stop(status=0):
        if kitty.poll() is None:
            kitty.terminate()
        try:
            os.kill(xvfb_pid, signal.SIGTERM)
        except OSError:
            pass
        raise SystemExit(status)

    signal.signal(signal.SIGTERM, lambda *_: stop(0))
    signal.signal(signal.SIGINT, lambda *_: stop(0))

    deadline = time.time() + 8
    while time.time() < deadline:
        if kitty.poll() is not None:
            open(root + "/error", "w").write(
                "Kilix exited %s before its listen socket\n" % (kitty.returncode,))
            stop(1)
        sock = root + "/listen.sock"
        if os.path.exists(sock) and stat.S_ISSOCK(os.stat(sock).st_mode):
            break
        time.sleep(0.05)
    else:
        open(root + "/error", "w").write("Kilix did not open its listen socket\n")
        stop(1)

    path = root + "/bridge.sock"
    try:
        os.unlink(path)
    except OSError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    os.chmod(path, 0o600)
    server.listen(4)
    server.setblocking(False)
    clients = {}
    controls = Mailbox(root + "/controls")

    sizes = {"agent": (960, 700), "shell": (640, 360)}
    placed = {}
    last = 0
    while True:
        if kitty.poll() is not None:
            open(root + "/error", "w").write(
                "Kilix exited %s\n" % (kitty.returncode,))
            stop(kitty.returncode or 1)
        readable, _, _ = select.select([server, controls.fd, *clients], [], [], 0.1)
        for sock in readable:
            if sock == controls.fd:
                controls.respond(lambda args: restricted_control(kitten, args))
                continue
            if sock is server:
                conn, _ = server.accept()
                conn.setblocking(False)
                clients[conn] = b""
                continue
            try:
                data = sock.recv(65536)
            except OSError:
                data = b""
            if not data:
                clients.pop(sock, None)
                sock.close()
                continue
            clients[sock] += data
            while b"\n" in clients[sock]:
                line, clients[sock] = clients[sock].split(b"\n", 1)
                try:
                    handle(line.decode("utf-8", "replace").strip(), kitten, sizes)
                except (OSError, subprocess.TimeoutExpired):
                    pass
        now = time.time()
        if now - last >= 0.1:
            last = now
            try:
                capture(sizes, placed)
            except (OSError, subprocess.TimeoutExpired):
                pass


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]))
