"""Kilix Operations v1. Compare generated text to the requested result."""

import json
from pathlib import Path
import re
import shlex

ITEMS = Path(__file__).with_name("items.json")
GUIDE = """Kilix operator reference
kilix ls lists pages. kilix ls --panes lists individual panes.
kilix pane list --json returns the pane inventory.
kilix pane read pane:ID reads visible text. Append --extent all for scrollback.
kilix pane send pane:ID TEXT types one literal argument. Append --submit to press Enter.
kilix pane focus pane:ID focuses a pane. kilix focus tab:ID focuses a page.
kilix new-pane DIRECTION splits the caller's pane. Append -- PROGRAM to start a program there.
kilix new-tab --title NAME opens a named page.
A replacement pane can have a new ID. Use the latest inventory in the question.
"""


def dataset(**kwargs):
    from datasets import Dataset, DatasetDict
    return DatasetDict(test=Dataset.from_list(json.loads(ITEMS.read_text())))


def prompt(doc):
    form = ("Reply with only the requested value." if doc["kind"] == "readout" else
            "Reply with only the requested Kilix command lines, one per line in execution order.")
    return f"{GUIDE}\n{doc['question']}\n{form}\nAnswer:\n"


def target(doc):
    return doc["answers"][0]


def commands(text):
    # Every reference command uses literal arguments from this small alphabet.
    # Excluding shell metacharacters before tokenizing preserves operand meaning.
    if not re.fullmatch(r"""[A-Za-z0-9_./:'" =\-\t\n]+""", text):
        return None
    try:
        return [shlex.split(line) for line in text.splitlines() if line.strip()]
    except ValueError:
        return None


def score(doc, results):
    reply = results[0].strip()
    if doc["kind"] == "readout":
        correct = reply in doc["answers"]
    else:
        parsed = commands(reply)
        correct = parsed is not None and any(parsed == commands(key) for key in doc["answers"])
    return {"acc": int(correct)}
