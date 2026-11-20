"""Build benchmark standings from the original per-run journals."""
from collections import defaultdict
from datetime import datetime
import fcntl
from fractions import Fraction
import json
import os
from pathlib import Path
import tempfile


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
        return created.isoformat(timespec="microseconds") + "Z"
    except ValueError:
        return None


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


def archived_runs(previous):
    # A restored rankings file can contain scores whose question journals were
    # archived separately. Merge by the original run filename on every refresh.
    runs = {}
    if previous.get("benchmark") != "OneBit" or previous.get("split") != "dev":
        raise ValueError("Unknown benchmark")
    for group in previous["models"]:
        provider, model = group["provider"] or "", group["model"] or ""
        if not isinstance(provider, str) or not isinstance(model, str):
            raise ValueError("Invalid model identity")
        for entry in group["history"]:
            run = dict(entry, provider=provider, model=model, journal_available=False)
            name = run["file"]
            if Path(name).name != name or not name.endswith(".jsonl") or name in runs:
                raise ValueError("Invalid run filename")
            if run["status"] not in ("Running", "Complete", "Stopped", "Failed", "Interrupted", "Damaged", "Unreadable"):
                raise ValueError("Invalid run status")
            if any(type(run[key]) is not int for key in ("correct", "answered", "total")):
                raise ValueError("Invalid answer count")
            if not 0 <= run["correct"] <= run["answered"] or (
                    run["status"] == "Complete" and not 0 < run["answered"] == run["total"]):
                raise ValueError("Inconsistent score")
            if run["started"] is not None and not isinstance(run["started"], str):
                raise ValueError("Invalid start date")
            if run["status"] == "Running":
                run["status"] = "Interrupted"
            runs[name] = run
    if len(runs) != previous["total_runs"]:
        raise ValueError("Inconsistent run count")
    return runs


def rankings_document(folder, runs=None):
    if runs is None:
        runs = {}
    for path in folder.glob("*.jsonl"):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            run = read_run(path)
        except FileNotFoundError:
            continue
        except OSError:
            run = dict(provider="", model="", file=path.name, started=run_date(path),
                       status="Unreadable", total=0, answered=0, correct=0)
        if run is None:
            continue
        if run["status"] != "Complete" and path.name in runs and runs[path.name]["status"] == "Complete":
            continue
        runs[path.name] = dict(run, journal_available=run["status"] not in ("Damaged", "Unreadable"))
    by_model = defaultdict(list)
    for name in sorted(runs, reverse=True):
        run = runs[name]
        by_model[run.pop("provider"), run.pop("model")].append(run)
    groups = []
    for (provider, model), attempts in by_model.items():
        complete = [r for r in attempts if r["status"] == "Complete"]
        count = len(complete)
        mean = Fraction(sum(r["correct"] for r in complete), count) if count else None
        accuracy = sum((Fraction(r["correct"], r["total"]) for r in complete), Fraction()) / count if count else None
        groups.append((provider, model, attempts, count, mean, accuracy))
    groups.sort(key=lambda g: (not (g[1] and g[3]), -(g[5] or 0), g[0].casefold(), g[1].casefold(), g[1]))
    previous_accuracy = None
    rank = 0
    models = []
    for position, (provider, model, attempts, count, mean, accuracy) in enumerate(groups, 1):
        ranked = bool(model and count)
        if ranked and accuracy != previous_accuracy:
            rank, previous_accuracy = position, accuracy
        models.append(dict(provider=provider or None, model=model or None, rank=rank if ranked else None,
                           runs=len(attempts), finished=count,
                           avg_correct=float(mean) if count else None,
                           avg_percent=float(accuracy * 100) if count else None, history=attempts))
    return dict(schema_version=1, benchmark="OneBit", split="dev", total_runs=len(runs), models=models)


def ranking_events(document):
    for group in document["models"]:
        fields = dict(group)
        history = fields.pop("history")
        fields.update(provider=fields["provider"] or "", model=fields["model"] or "", rank=fields["rank"] or 0)
        yield dict(event="standing", **fields)
        for run in history:
            started = run["started"]
            date = started[:19].replace("T", " ") if started else Path(run["file"]).stem
            status = run["status"]
            if status == "Complete" and not run["journal_available"]:
                status = "Complete (summary only)"
            yield {**run, "event": "saved_run", "provider": fields["provider"], "model": fields["model"],
                   "started": date, "status": status}


def save_rankings(folder):
    """Publish one complete UTF-8 snapshot, serialized with other writers."""
    folder.mkdir(parents=True, exist_ok=True, mode=0o700)
    target = folder / "rankings.json"
    with (folder / ".rankings.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            old_bytes = target.read_bytes()
        except FileNotFoundError:
            old_bytes = None
        runs = None
        damaged = False
        if old_bytes is not None:
            try:
                previous = json.loads(old_bytes)
            except (UnicodeError, ValueError):
                damaged = True
            else:
                if not isinstance(previous, dict) or previous.get("schema_version") != 1:
                    raise ValueError("Unrecognized rankings.json format. Its original contents have been preserved.")
                try:
                    runs = archived_runs(previous)
                except (ValueError, KeyError, TypeError):
                    damaged = True
        document = rankings_document(folder, runs)
        if damaged:
            if not document["total_runs"]:
                raise ValueError("Cannot rebuild rankings.json without run journals. Its contents have been preserved.")
            # Retain the exact bytes for recovery before repairing from journals.
            with tempfile.NamedTemporaryFile(dir=folder, prefix="rankings-damaged-", suffix=".json", delete=False) as out:
                out.write(old_bytes)
                out.flush()
                os.fsync(out.fileno())
        encoded = (json.dumps(document, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")
        if encoded != old_bytes:
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=folder, prefix=".rankings-", delete=False) as out:
                    temporary = Path(out.name)
                    out.write(encoded)
                    out.flush()
                    os.fsync(out.fileno())
                os.replace(temporary, target)
                directory = os.open(folder, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
        return document
