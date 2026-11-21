"""Discover the model IDs accepted by the installed headless CLIs."""
from contextlib import suppress
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import tempfile
import time
import tomllib


def executable(provider):
    home = Path.home()
    search = os.pathsep.join((os.environ.get("PATH", os.defpath), str(home / ".local/bin"),
                              str(home / ".grok/bin"), str(home / ".claude/local")))
    command = os.environ.get("PUZZLLM_" + provider.upper()) or provider
    binary = shutil.which(os.path.expanduser(command), path=search)
    if not binary:
        raise RuntimeError(f"Could not find {command}. Install the CLI or set PUZZLLM_{provider.upper()}.")
    return os.path.abspath(binary)


def mapping(path):
    try:
        with path.open("rb") as source:
            value = tomllib.load(source) if path.suffix == ".toml" else json.load(source)
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def identifiers(values):
    return list(dict.fromkeys(v for v in values if isinstance(v, str) and v and
                              len(v) < 512 and v.isprintable() and not v.isspace()))


def stop_child(process):
    # Headless agents may start helpers. Each invocation owns a process group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=0.35)
    except subprocess.TimeoutExpired:
        pass
    except ProcessLookupError:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def native_catalog(argv, requests, receive, directory):
    """Exchange only initialization/catalog messages. No conversation is started."""
    proc = subprocess.Popen(argv, cwd=directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        proc.stdin.write(b"".join(json.dumps(r).encode() + b"\n" for r in requests))
        proc.stdin.flush()
        pending = b""
        until = time.monotonic() + 8
        while (left := until - time.monotonic()) > 0 and select.select([proc.stdout], [], [], left)[0]:
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break
            lines = (pending + chunk).split(b"\n")
            pending = lines.pop()
            for line in lines:
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if isinstance(row, dict) and receive(row):
                    return
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        with suppress(BrokenPipeError, ConnectionResetError):
            proc.stdin.close()
        proc.stdout.close()
        stop_child(proc)


def codex_catalog(directory, need_default):
    home = Path(os.environ.get("CODEX_HOME") or Path.home() / ".codex")
    config = mapping(home / "config.toml")
    default = config.get("model") if isinstance(config.get("model"), str) else ""
    cached = mapping(home / "models_cache.json").get("models") or []
    names = identifiers(row.get("slug") for row in cached if isinstance(row, dict)
                        and row.get("visibility") != "hide")
    if names and (default or not need_default):
        return identifiers([default, *names]), default

    received = set()
    list_default = ""

    def read(row):
        nonlocal default, list_default
        received.add(row.get("id"))
        result = row.get("result") or {}
        if row.get("id") == 2:
            default = (result.get("config") or {}).get("model") or default
        if row.get("id") == 3:
            for item in result.get("data") or []:
                if not item.get("hidden"):
                    model = item.get("model") or item.get("id")
                    names.append(model)
                    if item.get("isDefault"):
                        list_default = model
        return 3 in received and 2 in received

    requests = [
        {"id": 1, "method": "initialize", "params": {"clientInfo": {"name": "puzzllm", "version": "1"}}},
        {"method": "initialized"}, {"id": 2, "method": "config/read", "params": {"includeLayers": False}},
        {"id": 3, "method": "model/list", "params": {"limit": 256}},
    ]
    native_catalog([executable("codex"), "app-server", "-c", "mcp_servers={}"], requests, read, directory)
    default = default or list_default
    return identifiers([default, *names]), default


def grok_catalog(directory, need_default):
    home = Path(os.environ.get("GROK_HOME") or Path.home() / ".grok")
    cached = mapping(home / "models_cache.json").get("models") or {}
    names = [key for key, row in cached.items() if isinstance(row, dict)
             and not (row.get("info") or {}).get("hidden")] if isinstance(cached, dict) else []
    configured = mapping(home / "config.toml").get("model")
    default = configured if isinstance(configured, str) else ""
    if not names or (need_default and not default):
        try:
            result = subprocess.run([executable("grok"), "models"], cwd=directory,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    text=True, errors="replace", timeout=8, env={**os.environ, "NO_COLOR": "1"})
            for line in result.stdout.splitlines():
                match = re.fullmatch(r"Default model:\s*(\S+)", line.strip(), re.I)
                if match and not default:
                    default = match[1]
                item = re.fullmatch(r"\s*[*-]\s+(\S+)(?:\s+.*)?", line)
                if item:
                    names.append(item[1])
        except (OSError, subprocess.TimeoutExpired, RuntimeError):
            if not names:
                raise
    return identifiers([default, *names]), default


def claude_catalog(directory, need_default):
    home = Path(os.environ.get("CLAUDE_CONFIG_DIR") or Path.home() / ".claude")
    account = mapping(home.parent / ".claude.json") if not os.environ.get("CLAUDE_CONFIG_DIR") else mapping(home / ".claude.json")
    settings = mapping(home / "settings.json")
    settings.update(mapping(home / "settings.local.json"))
    env = {**(settings.get("env") or {}), **os.environ}
    configured = env.get("ANTHROPIC_MODEL") or settings.get("model")
    default = configured if isinstance(configured, str) else ""
    if need_default:
        # The messages CLI reports the concrete deployment when it answers.
        return [], default
    names = [default]

    def read(row):
        if row.get("type") != "control_response" or row.get("response", {}).get("request_id") != "catalog":
            return False
        result = row["response"].get("response") or {}
        names.extend(m.get("value") for m in result.get("models", []) if m.get("value") != "default")
        return True

    argv = ["--print", "--verbose", "--input-format", "stream-json",
            "--output-format", "stream-json", "--strict-mcp-config", "--no-session-persistence", "--tools", ""]
    try:
        native_catalog([executable("claude"), *argv], [{"type": "control_request", "request_id": "catalog",
                       "request": {"subtype": "initialize"}}], read, directory)
    except (OSError, RuntimeError):
        if not any((default, settings.get("availableModels"), settings.get("modelSettings"),
                    account.get("additionalModelOptionsCache"))):
            raise
    names.extend(m.get("value") for m in account.get("additionalModelOptionsCache", []) if isinstance(m, dict))
    names.extend(settings.get("modelSettings") or {})
    names.extend(settings.get("availableModels") or [])
    names.extend(value for key, value in env.items() if key.startswith("ANTHROPIC_DEFAULT_") and key.endswith("_MODEL"))
    return identifiers(names), default


def hosted_catalog(provider, need_default=False):
    sources = {"codex": codex_catalog, "grok": grok_catalog, "claude": claude_catalog}
    cache = Path(os.environ.get("TMPDIR") or Path(os.environ.get("XDG_CACHE_HOME") or Path.home()/".cache")/"puzzllm")
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(dir=cache) as directory:
        return sources[provider](directory, need_default)


def gguf_snapshot(model):
    """Capture the actual file set before loading the model."""
    path = Path(model).expanduser().resolve(strict=True)
    parts = [path]
    split = re.fullmatch(r"(.+)-\d{5}-of-(\d{5})\.gguf", path.name, re.I)
    if split and int(split[2]) > 1:
        parts = [path.with_name(f"{split[1]}-{i:05}-of-{split[2]}{path.suffix}") for i in range(1, int(split[2])+1)]
    return parts, gguf_stamps(parts)


def gguf_stamps(parts):
    return [(s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for s in (p.stat() for p in parts)]


def gguf_artifact(snapshot, cache, runner_id):
    """Hash the weights outside the timed interval and cache by file identity."""
    parts, initial = snapshot
    if gguf_stamps(parts) != initial:
        return None
    key = hashlib.sha256(json.dumps([runner_id, str(parts[0]), initial]).encode()).hexdigest()
    entry = cache / key
    try:
        known = entry.read_text().strip()
        if re.fullmatch(r"sha256:[0-9a-f]{64}", known):
            return known
    except OSError:
        pass
    hashes = []
    for p in parts:
        with p.open("rb") as source:
            hashes.append(hashlib.file_digest(source, "sha256").digest())
    if gguf_stamps(parts) != initial:
        return None
    digest = hashes[0] if len(parts) == 1 else hashlib.sha256(b"GGUF shards\0" + b"".join(hashes)).digest()
    result = "sha256:" + digest.hex()
    try:
        cache.mkdir(exist_ok=True, mode=0o700)
        entry.write_text(result + "\n")
    except OSError:
        pass
    return result
