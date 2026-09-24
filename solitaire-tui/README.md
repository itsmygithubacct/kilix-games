# solitaire-tui

Klondike solitaire for the terminal, part of [kilix-games](../). It replaces kilix 95's Solitaire
(kilix `desktop/apps/sol.py`). Same rules, and the same seed deals the same
game. Pure Python 3 standard library: curses for the screen, no other
dependencies.

```sh
./bin/solitaire-tui                 # random deal, deal one
./bin/solitaire-tui --seed 7        # a specific deal
./bin/solitaire-tui --draw3         # deal three
make test
```

## Playing

**Mouse:** click a card to pick it up and click where it goes, or drag it.
Double-click or right-click a card to **auto-place** it: to its foundation
if it fits, otherwise to the best column (one that uncovers a face-down card
if possible). Click the stock to deal. The buttons along the bottom are
Deal, Auto, All home, Undo, Hint, Play, New, Help and Quit.

| Key | Action |
|---|---|
| arrows | move between piles; Up/Down on a column widens/narrows the run |
| space / enter | pick up, drop, or deal when on the stock; esc lets go |
| a | auto-place the selected card |
| A | send every playable card home |
| 1-7 | jump to a column, or drop what you hold on it |
| f | send the card under the cursor home |
| d / u / h | deal / undo / hint (the neural player's move in deal one) |
| p | let the neural player play this deal; any key or click stops it |
| r / n / 3 | restart this deal / new deal / deal one or three |
| ? / q | help / quit |

Cards are drawn as boxes on the terminal's own background. Face-up cards in
a column overlap by two rows, or by one on short terminals. The game reads
SGR mouse reports itself; set `SOLITAIRE_TUI_TRACE=/path/log` to log every
input event when diagnosing a terminal.

## Built for learned players

The game is layered, so a neural network can play it without touching the UI:

| Module | Contract |
|---|---|
| `engine` | pure rules: immutable `State`, `Move`, `deal`, `legal_moves`, `apply` |
| `actions` | a fixed 681-way action space (`encode`/`decode`) and a legality mask |
| `observe` | a fixed 690-float observation of what a player can see (hidden cards never leak), versioned |
| `agents` | `choose(state, legal) -> Move`: random, greedy, and `PolicyAgent(policy(obs, mask) -> index)` |
| `sim` | headless games, tournaments and recorded (observation, action) trajectories |
| `tui` | the curses game; `render()` is pure and tested without a terminal |

Baselines, draw one, seeds 1..200: random 0%, greedy 13%.

```sh
./bin/solitaire-tui --simulate greedy --games 200
./bin/solitaire-tui --simulate neural --seed 900000 --games 1000
```

## The neural player

Hint and Play use a small network (32-64-64-1, 6,337 parameters, pure
Python at play time) in deal one; deal three falls back to greedy. It scores
each sensible, non-revisiting candidate move from what a player can see plus
the game's own history, and plays the best. It was trained on 6,000 games of
the game's own rollout planner (`planner.py`; `--simulate planner`) with
`tools/neural/solnet.py` and packed by `tools/neural/export_policy.py`;
provenance is in `solitaire_tui/data/solitaire-policy.json`.

Held-out deals 900000-900999, draw one, evaluated once:

| Player | Wins | Wilson 95% |
|---|---|---|
| greedy | 11.6% | 9.8-13.7 |
| **neural** | **22.1%** | 19.6-24.8 |
| random non-revisiting candidate | 28.1% | 25.4-31.0 |
| greedy non-revisiting candidate | 29.9% | 27.1-32.8 |
| planner (the teacher, far slower) | 32.2% | 29.4-35.2 |

The network roughly doubles greedy, which is what it had to do to ship. It
does not yet beat simply filtering greedy's moves to non-revisiting ones,
so most of its gain comes from the candidate filter rather than the
network's ranking. The teacher's choices between near-equal moves are
noisy, which is the limit found so far.

## License

MIT
