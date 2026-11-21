"""Finite campaign problems. Only the runner executes player submissions."""
from collections import deque
from itertools import permutations, product
import json
import os
import random
import re
import stat


class Incorrect(ValueError):
    """A remaining requirement suitable for the player and the coding agent."""


def sort_four(source):
    pairs = []
    for line in source.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"\s*([1-4])\s+([1-4])\s*", line)
        if not match or match[1] >= match[2]:
            raise Incorrect("Each line needs two lane numbers, lower first, from 1 through 4.")
        pairs.append((int(match[1]) - 1, int(match[2]) - 1))
    if len(pairs) > 5:
        raise Incorrect(f"The network has {len(pairs)} comparators. The limit is five.")
    for bits in product("01", repeat=4):
        lanes = list(bits)
        for i, j in pairs:
            if lanes[i] > lanes[j]:
                lanes[i], lanes[j] = lanes[j], lanes[i]
        if lanes != sorted(bits):
            raise Incorrect(f"Input {''.join(bits)} leaves {''.join(lanes)}. Expected {''.join(sorted(bits))}.")
    return "All 16 binary inputs sort. The same network sorts arbitrary weights."


def airlock(source):
    states = {}
    for line in source.splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5:
            raise Incorrect("Each row needs a state name, door output and three targets for L R C.")
        name, doors, *targets = fields
        if any(not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,15}", s) for s in [name, *targets]):
            raise Incorrect("State names start with a letter and have at most 16 letters, digits or underscores.")
        if doors not in ("-", "L", "R", "LR") or name in states or len(states) == 32:
            raise Incorrect("Use at most 32 uniquely named states with door output -, L, R or LR.")
        states[name] = (doors, targets)
    if "start" not in states or states["start"][0] != "-":
        raise Incorrect("The initial state must be named start and have both doors sealed (-).")
    for name, (_, targets) in states.items():
        for target in targets:
            if target not in states:
                raise Incorrect(f"State {name} refers to missing state {target}.")
    pending = deque([("start", "")])
    visited = {"start"}
    while pending:
        name, trace = pending.popleft()
        doors, targets = states[name]
        for event, target in zip("LRC", targets):
            path = trace + event
            after = states[target][0]
            if after == "LR":
                raise Incorrect(f"Events {path} reach {target} with both doors open.")
            expected = event if event != "C" and doors in ("-", event) else "-"
            if after != expected:
                raise Incorrect(f"Events {path} reach {target} with output {after}. Expected {expected}.")
            if target not in visited:
                visited.add(target)
                pending.append((target, path))
    return f"Verified all three events from each of {len(visited)} reachable states."


def lift_cases():
    for length in range(9):
        for moves in product("UD", repeat=length):
            floor = 0
            expected = "OK"
            for step, move in enumerate(moves, 1):
                floor += 1 if move == "U" else -1
                if floor < 0:
                    expected = str(step)
                    break
            yield "".join(moves), expected


def radio_cases():
    for digits in product(range(2), repeat=4):
        a, b, c, d = digits
        word = [a ^ b ^ d, a ^ c ^ d, a, b ^ c ^ d, b, c, d]
        message = "".join(map(str, digits))
        yield "".join(map(str, word)), message
        for bit in range(7):
            damaged = word.copy()
            damaged[bit] ^= 1
            yield "".join(map(str, damaged)), message


def compact(value):
    return json.dumps(value, separators=(",", ":"))


def cargo_cases():
    rng = random.Random(4106)
    seen = set()
    while len(seen) < 30:
        count = 2 + len(seen) % 3
        weights = []
        for _ in range(count):
            cuts = sorted(rng.sample(range(1, 6), rng.randrange(4)))
            weights.extend(b - a for a, b in zip([0, *cuts], [*cuts, 6]))
        rng.shuffle(weights)
        sequence = tuple(weights)
        if sequence in seen:
            continue
        seen.add(sequence)
        crates = {chr(ord("A") + i): weight for i, weight in enumerate(weights)}
        yield compact({"capacity": 6, "crates": crates}), crates


def json_answer(answer):
    try:
        return json.loads(answer)
    except (ValueError, RecursionError):
        raise Incorrect("Expected a JSON answer on one line.") from None


def cargo_answer(crates, answer):
    carts = json_answer(answer)
    if not isinstance(carts, list) or any(not isinstance(cart, list) for cart in carts):
        raise Incorrect("Return a JSON array of carts, each an array of crate labels.")
    labels = [label for cart in carts for label in cart]
    if any(type(label) is not str for label in labels) or sorted(labels) != sorted(crates):
        raise Incorrect("Use each labeled crate exactly once.")
    if any(sum(crates[label] for label in cart) > 6 for cart in carts):
        raise Incorrect("A cart exceeds capacity six.")
    optimum = sum(crates.values()) // 6
    if len(carts) != optimum:
        raise Incorrect(f"The load fits in {optimum} carts. Your packing uses {len(carts)}.")


def dispatch_cases():
    def case(jobs, edges):
        return compact({"jobs": list(jobs), "edges": edges}), (set(jobs), set(edges))
    jobs = "ABC"
    possible = list(product(jobs, repeat=2))
    for mask in range(1 << len(possible)):
        yield case(jobs, [edge for i, edge in enumerate(possible) if mask & (1 << i)])
    yield case("ABCDEF", [("D", "B"), ("E", "B"), ("B", "F"), ("F", "A")])
    yield case("ABCDEF", [("B", "C"), ("C", "D"), ("D", "E"), ("E", "B"), ("E", "F")])


def dispatch_answer(graph, answer):
    jobs, edges = graph
    result = json_answer(answer)
    if not isinstance(result, dict) or set(result) not in ({"order"}, {"cycle"}):
        raise Incorrect('Return a JSON object with exactly one key, "order" or "cycle".')
    kind, path = next(iter(result.items()))
    if not isinstance(path, list) or any(type(job) is not str or job not in jobs for job in path):
        raise Incorrect("The witness must be an array of known job names.")
    if kind == "order":
        if len(path) != len(jobs) or set(path) != jobs:
            raise Incorrect("An order must contain every job exactly once.")
        for a, b in sorted(edges):
            if path.index(a) >= path.index(b):
                raise Incorrect(f"Job {a} must precede {b}.")
    else:
        if len(path) < 2 or path[0] != path[-1] or len(set(path[:-1])) != len(path) - 1:
            raise Incorrect("A cycle must return to its first job with distinct jobs before closure.")
        if any(edge not in edges for edge in zip(path, path[1:])):
            raise Incorrect("A step in the submitted cycle has no directed edge.")


STEPS = {"U": (0, -1), "D": (0, 1), "L": (-1, 0), "R": (1, 0)}


def neighbor(board, cell, move):
    dx, dy = STEPS[move]
    x, y = cell % 4 + dx, cell // 4 + dy
    if 0 <= x < 4 and 0 <= y < 3 and board[y * 4 + x] == ".":
        return y * 4 + x
    return None


def exit_cases():
    for middle in product(".#", repeat=10):
        board = "." + "".join(middle) + "."
        pending = deque([(0, 0)])
        visited = {0}
        distance = None
        while pending:
            cell, steps = pending.popleft()
            if cell == 11:
                distance = steps
                break
            for move in STEPS:
                other = neighbor(board, cell, move)
                if other is not None and other not in visited:
                    visited.add(other)
                    pending.append((other, steps + 1))
        yield board, (board, distance)


def exit_answer(problem, answer):
    board, distance = problem
    if distance is None:
        if answer != "BLOCKED":
            raise Incorrect("The exit is unreachable. Print BLOCKED.")
        return
    if answer == "BLOCKED" or len(answer) != distance:
        raise Incorrect(f"A shortest route has {distance} moves.")
    cell = 0
    for move in answer:
        cell = neighbor(board, cell, move) if move in STEPS else None
        if cell is None:
            raise Incorrect("The route crosses a wall or the grid boundary.")
    if cell != 11:
        raise Incorrect("The route misses the exit.")


def exact(expected, answer):
    if answer != expected:
        raise Incorrect(f"Expected {expected}, got {ascii(answer)[:50]}.")


# Each input and output is one line, including the lift's empty first input.
BATCH = {
    "lift.py": (lift_cases, exact, "lift command strings"),
    "radio.py": (radio_cases, exact, "radio words"),
    "cargo.py": (cargo_cases, cargo_answer, "optimal packings"),
    "dispatch.py": (dispatch_cases, dispatch_answer, "dependency boards"),
    "escape.py": (exit_cases, exit_answer, "grid layouts"),
}


def check_batch(name, run):
    generate, validate, noun = BATCH[name]
    cases = list(generate())
    stdin = "".join(line + "\n" for line, _ in cases).encode("ascii")
    output = run(stdin).decode("ascii").split("\n")
    if output[-1] == "":
        output.pop()
    for i, (line, expected) in enumerate(cases):
        try:
            if i == len(output):
                raise Incorrect("Missing output line.")
            validate(expected, output[i].removesuffix("\r"))
        except Incorrect as error:
            raise Incorrect(f"Case {i + 1}, input {line or '(empty)'}: {error}") from None
    if len(output) != len(cases):
        raise Incorrect(f"Expected {len(cases)} output lines, received {len(output)}.")
    return f"Passed all {len(cases)} {noun}."


def rename_carousel(run, scratch):
    labels = "ABCD"
    for case, destinations in enumerate(permutations(labels), 1):
        directory = scratch / str(case)
        directory.mkdir()
        contents = {label: b"\x00" + os.urandom(16 + i) + b"\n" for i, label in enumerate(labels)}
        for label, content in contents.items():
            (directory / label).write_bytes(content)
        manifest = "".join(f"{a} {b}\n" for a, b in zip(labels, destinations))
        try:
            run(manifest.encode("ascii"), directory)
            if {item.name for item in directory.iterdir()} != set(labels):
                raise Incorrect("The directory must contain exactly A B C D after renaming.")
            for old, new in zip(labels, destinations):
                path = directory / new
                info = path.lstat()
                if (not stat.S_ISREG(info.st_mode) or info.st_size != len(contents[old])
                        or path.read_bytes() != contents[old]):
                    raise Incorrect(f"{new} must contain the original bytes from {old}.")
        except Incorrect as error:
            raise Incorrect(f"Mapping ABCD to {''.join(destinations)}: {error}") from None
    return "All 24 rename permutations preserve every file and leave no temporary names."
