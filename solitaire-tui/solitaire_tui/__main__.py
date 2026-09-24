"""Command line: play, simulate, or self-test."""
import argparse
import json
import sys

from . import __version__, agents, sim


def _planner():
    from .planner import PlannerAgent
    return PlannerAgent(16, 100)             # the neural player's teacher


def _neural():
    from .policy import NeuralAgent
    return NeuralAgent()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="solitaire-tui",
                                 description="Klondike solitaire for the terminal.")
    ap.add_argument("--seed", type=int, help="deal this game (same deals as kilix 95 Solitaire)")
    ap.add_argument("--draw3", action="store_true", help="deal three instead of one")
    players = dict(agents.AGENTS, planner=_planner, neural=_neural)
    ap.add_argument("--simulate", choices=sorted(players),
                    help="play headless games with an agent and print JSON")
    ap.add_argument("--games", type=int, default=100, help="games for --simulate")
    ap.add_argument("--version", action="version", version=f"solitaire-tui {__version__}")
    a = ap.parse_args(argv)
    if a.simulate:
        start = a.seed if a.seed is not None else 1
        summary = sim.tournament(players[a.simulate], range(start, start + a.games),
                                 draw3=a.draw3)
        print(json.dumps({"agent": a.simulate, "first_seed": start, "draw3": a.draw3,
                          **summary}))
        return 0
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("solitaire-tui: needs an interactive terminal (try --simulate)", file=sys.stderr)
        return 2
    from . import tui
    tui.run(a.seed, a.draw3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
