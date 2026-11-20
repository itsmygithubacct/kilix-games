"""Build benchmark standings from the original per-run journals."""
from collections import defaultdict
from datetime import datetime
import fcntl
from fractions import Fraction
import json
from pathlib import Path


def model_id(provider, model):
    if not model:
        return ""
    if provider == "llama.cpp":
        try:
            return str(Path(model).expanduser().resolve())
        except (OSError, RuntimeError, ValueError):
            return model
    if provider == "ollama":
        model = model.removeprefix("registry.ollama.ai/").removeprefix("library/")
        if ":" not in model.rsplit("/", 1)[-1]:
            model += ":latest"
    return model


def run_date(path):
    try:
        created = datetime.strptime(path.name.split("-", 1)[0], "%Y%m%dT%H%M%S.%fZ")
        return created.strftime("%Y-%m-%d %H:%M:%S")
    except ValueError:
        return path.stem


def read_run(path):
    provider = model = status = ""
    identities = set()
    answers = {}
    total = 0
    damaged = False
    with path.open("rb") as source:
        try:
            fcntl.flock(source, fcntl.LOCK_SH | fcntl.LOCK_NB)
            live = False
        except BlockingIOError:
            live = True
        for line in source:
            if not line.endswith(b"\n"):
                break
            try:
                event = json.loads(line)
                kind = event["event"]
                if kind == "begin":
                    backend, choice = event["provider"], event.get("model") or ""
                    if not isinstance(backend, str) or not isinstance(choice, str):
                        raise ValueError("invalid model")
                    provider, model = backend, choice
                    total = int(event.get("total", 0))
                elif kind == "selection" and isinstance(event.get("model"), str):
                    model = event["model"]
                elif kind == "identity":
                    name = event.get("model")
                    if isinstance(name, str) and name:
                        identities.add(model_id(provider, name))
                elif kind == "size":
                    total = int(event["total"])
                elif kind == "answer":
                    n, verdict = event["n"], event["verdict"]
                    if type(n) is not int or n < 1 or verdict not in ("correct", "wrong", "invalid"):
                        raise ValueError("invalid answer")
                    if n in answers and answers[n] != verdict:
                        damaged = True
                    answers[n] = verdict
                elif kind in ("end", "stopped", "error"):
                    status = {"end": "Complete", "stopped": "Stopped", "error": "Failed"}[kind]
            except (KeyError, TypeError, ValueError, UnicodeError):
                damaged = True
    if not provider and not damaged:
        # The worker can publish the locked file just before its first write.
        return None if live else dict(provider="", model="", status="Interrupted", file=path.name,
                                      started=run_date(path), answered=0, total=0, correct=0)
    if damaged:
        status = "Damaged"
    elif status == "Complete" and (total < 1 or len(answers) != total or max(answers, default=0) > total):
        status = "Interrupted"
    elif not status:
        status = "Running" if live else "Interrupted"
    identity = next(iter(identities)) if len(identities) == 1 else model_id(provider, model)
    if len(identities) > 1:
        identity = ""
    return dict(provider=provider, model=identity, file=path.name, started=run_date(path),
                status=status, total=total, answered=len(answers),
                correct=sum(verdict == "correct" for verdict in answers.values()))


def standings(folder):
    by_model = defaultdict(list)
    for path in sorted(folder.glob("*.jsonl"), reverse=True):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            run = read_run(path)
        except FileNotFoundError:
            continue
        except OSError:
            run = dict(provider="", model="", file=path.name, started=run_date(path),
                       status="Unreadable", total=0, answered=0, correct=0)
        if run is not None:
            by_model[run["provider"], run["model"]].append(run)
    groups = []
    for (provider, model), attempts in by_model.items():
        complete = [r for r in attempts if r["status"] == "Complete"]
        count = len(complete)
        mean = Fraction(sum(r["correct"] for r in complete), count) if count else None
        accuracy = sum((Fraction(r["correct"], r["total"]) for r in complete), Fraction()) / count if count else None
        groups.append((provider, model, attempts, count, mean, accuracy))
    groups.sort(key=lambda g: (not (g[1] and g[3]), -(g[5] or 0), g[0].casefold(), g[1].casefold(), g[1]))
    previous = None
    rank = 0
    for position, (provider, model, attempts, count, mean, accuracy) in enumerate(groups, 1):
        ranked = bool(model and count)
        if ranked and accuracy != previous:
            rank, previous = position, accuracy
        yield dict(event="standing", provider=provider, model=model, rank=rank if ranked else 0,
                   runs=len(attempts), finished=count,
                   avg_correct=float(mean) if count else None,
                   avg_percent=float(accuracy * 100) if count else None)
        for run in attempts:
            yield dict(event="saved_run", **run)
