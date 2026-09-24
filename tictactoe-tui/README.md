# tictactoe-tui

Tic-tac-toe for the terminal, part of [kilix-games](../). It runs inside the
terminal you start it from (Kilix, Kitty or any ANSI terminal), in text mode
with colour and mouse support, and puts the terminal back when you quit.
Either side can be a human, a trained neural player or a random mover.

```sh
make
./tictactoe-tui                          # the main menu
./tictactoe-tui --x human --o neural     # skip straight to your setup
./tictactoe-tui --o neural --level easy --first alternate
make test
```

## Playing

The main menu has **PLAY**, then **X PLAYER** and **O PLAYER** (HUMAN / NEURAL
/ RANDOM), **NEURAL LEVEL** (EASY / NORMAL / PERFECT), **FIRST MOVE** (X ALWAYS
or ALTERNATE between games) and **QUIT**. The score across a session's games is
shown above the board.

| Key | Action |
|---|---|
| arrows, WASD, hjkl | move the cursor; in a menu, move the selection |
| Enter / Space | place a mark; choose a menu item; step a setting |
| Left / Right | change a setting on the main menu |
| 1-9 | place on that square, numpad layout (7 8 9 is the top row) |
| ? | ask the network for its move (shown in green) |
| Esc / P | pause menu: RESUME, RESTART GAME, MAIN MENU, QUIT |
| mouse | click a square, a setting or a menu item |

When a game ends, a menu offers PLAY AGAIN, MAIN MENU and QUIT. There is no
quit key: the menus are the way out (Ctrl+C also exits cleanly). The last
setup is remembered in `$XDG_DATA_HOME/tictactoe-tui/settings.state`.

The terminal needs to be at least 44x24.

## The neural player

A 18-128-128-9 ReLU network (20,105 parameters) compiled into the binary,
run by kilix-game-kit's `kilix_game_policy`. Its input is the board from the
mover's side (its own marks, then the opponent's), so one network plays X
and O whoever moves first; its output is a value for every square.

It was trained by [`tools/neural/train.py`](tools/neural/train.py) on exact
minimax values of all 4,520 positions that can occur with a move to make,
with a cross-entropy term that makes its top choice exact. `make test`
proves in C, against the game's own solver, that at PERFECT it picks an
optimal move in every one of those positions, takes a fastest win whenever
it has one, and never loses to any line of play from either side.

The levels sample from the network's values instead of taking the best:

| Level | Temperature | vs RANDOM, 2000 games (W-D-L) |
|---|---|---|
| EASY | 0.55 | 1516-246-238 |
| NORMAL | 0.18 | 1756-228-16 |
| PERFECT | argmax | 1853-147-0 |

EASY against PERFECT: 0-675-1325, so EASY is beatable and PERFECT is not.

Provenance (data, seed, training curve, blob sha256) is in
[`docs/neural-policy-provenance.json`](docs/neural-policy-provenance.json);
`make check-policy` checks the blob, header and manifest agree. Retrain with
`make train` (needs numpy).

## Tests

```sh
make test        # rules, solver, menus, network proof, layout, install
make sanitize    # the same under ASan/UBSan
```

## Files

| Path | Contents |
|---|---|
| `src/engine.c` | rules, exact solver, neural and random players |
| `src/game.c` | menus, turns, computer move delay, scores |
| `src/render.c` | pure screen layout and click hit-testing |
| `src/term.c` | raw terminal input, SGR mouse, ANSI drawing |
| `src/main.c` | interactive loop, remembered settings, headless tests |
| `src/neural_policy_blob.h` | generated from `assets/policy/tictactoe-neural.kxpol`; do not edit |
| `tools/neural/` | trainer and policy check |

## License

MIT.
