"""Incident evidence, player work orders and the private VM verification receipt."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import random
import secrets
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from puzzllm_challenges import Incorrect

ACTIONS = {
    "ssh": ["restore-openssh-server", "restart-ssh"],
    "root": ["restore-sudo", "remove-unapproved-unit", "restore-dispatch", "reboot"],
}


def atomic(path, text):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as out:
        out.write(text)
    os.replace(out.name, path)


def evidence(kind, seed):
    rng = random.Random(seed)
    names = rng.sample(["alder", "briar", "cedar", "elm", "hazel", "rowan"], 4)
    subject = rng.randrange(1, 4)
    ids = rng.sample(range(100000, 999999), 20)
    accounts = ["directory " + str(i + 4100) + " " + user for i, user in enumerate(names)]
    events = []
    audit = []
    for i, name in enumerate(names):
        session = f"s-{ids[i]}"
        if kind == "ssh":
            first, second = f"G-{ids[4+i]}", f"S-{ids[8+i]}"
            events.append(f"{first} gateway authenticated uid={4100+i} peer=192.0.2.17 connection={session}")
            result = "sshd integrity alarm; incident confirmed" if i == subject else "routine engineer login"
            audit.append(f"{second} ssh-host connection={session} disposition={result}")
        else:
            first, second = f"L-{ids[4+i]}", f"A-{ids[8+i]}"
            events.append(f"{first} login session={session} uid={4100+i}")
            if i == subject:
                detail = "image=/usr/bin/sudo euid=0 authorization=absent change=dispatch-shadow.service"
            elif i == 0:
                detail = "image=/usr/bin/sudo euid=0 authorization=approved change=log-rotation"
            else:
                detail = "image=/usr/bin/ls euid=user authorization=ordinary"
            audit.append(f"{second} audit session={session} {detail}")
        if i == subject:
            report = dict(account=name)
            if kind == "ssh":
                report.update(gateway_event=first, server_event=second)
            else:
                report.update(login_event=first, root_event=second, component="sudo", advisory="USN-7604-1")
    rng.shuffle(events)
    rng.shuffle(audit)
    inventory = (["Archive: Noble OpenSSH incident / USN-6859-1; shared access gateway"]
                 if kind == "ssh" else
                 ["Archive: Noble package inventory at incident time",
                  "sudo: vendor security update USN-7604-1 outstanding",
                  "tar: unrelated maintenance update outstanding",
                  "Baseline: dispatch.service enabled; /srv/dispatch/manifest retained; no dispatch-shadow.service"])
    return "\n".join(inventory + accounts + events + audit) + "\n", report


def prepare(state, kind):
    folder = state / "incident-evidence"
    folder.mkdir(exist_ok=True, mode=0o700)
    ws = (state / "workspace").stat()
    case = state / "incidents" / f"{ws.st_dev:x}-{ws.st_ino:x}" / kind
    case.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (case / ".setup").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        key = case / "seed"
        if not key.exists():
            atomic(key, secrets.token_hex(24))
        text, fields = evidence(kind, key.read_text())
        log = folder / (kind + ".log")
        if not log.exists() or log.read_text() != text:
            atomic(log, text)
    return case, fields


def diagnose(kind, source, fields):
    try:
        report = json.loads(source)
    except (ValueError, RecursionError):
        raise Incorrect("Submit a JSON object using the fields in the room guide.") from None
    if not isinstance(report, dict):
        raise Incorrect("The incident report must be a JSON object.")
    for key, expected in fields.items():
        if report.get(key) != expected:
            raise Incorrect(f"Check {key} against this incident's off-host evidence.")
    actions = report.get("repair")
    if not isinstance(actions, list) or not all(isinstance(action, str) for action in actions):
        raise Incorrect("Supply the repair work order as a JSON array.")
    if actions != ACTIONS[kind]:
        missing = next((action for action in ACTIONS[kind] if action not in actions), None)
        raise Incorrect("Diagnosis verified. " + ("Missing repair: " + missing if missing else
                        "Check the order of the maintenance actions against the guide."))
    return actions


def assess(state, name, source):
    kind = name.split("-")[0]
    case, fields = prepare(state, kind)
    actions = diagnose(kind, source, fields)
    signature = hashlib.sha256(json.dumps([fields, actions], sort_keys=True).encode()).hexdigest()
    result = case / (signature + ".json")
    with (case / ".vm-lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise BlockingIOError("Diagnosis verified. Ubuntu is applying and checking the repair work order.") from None
        if result.exists():
            ok, msg = json.loads(result.read_text())
            if ok:
                return msg
            if time.time() - result.stat().st_mtime < 60:
                raise BlockingIOError(msg)
        with (case / "verification.log").open("w") as log:
            subprocess.Popen([sys.executable, "-IB", str(Path(__file__).with_name("puzzllm_incident_vm.py")),
                              str(result), kind, *actions], stdin=subprocess.DEVNULL,
                             stdout=log, stderr=log, pass_fds=(lock.fileno(),), start_new_session=True)
    raise BlockingIOError("Diagnosis verified. Starting the disposable Ubuntu repair check.")


if __name__ == "__main__":
    os.umask(0o077)
    prepare(Path(sys.argv[1]), {"prepare-ssh-incident": "ssh", "prepare-root-incident": "root"}[sys.argv[2]])
