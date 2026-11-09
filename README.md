# kilix-games

A monorepo of small games for Kilix and other Kitty-graphics terminals. Each
game lives in its own directory and builds and tests on its own. The C games
share one [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk)
checkout at `third_party/kilix-game-sdk`.

Installable on demand from the Kilix content catalog: twelve catalog entries
pin this repository (`kilix games play <id>`, or `kilix install <id>`). The
catalog pins commit `668faacf`; `main` has six later commits that tighten
self-test argument parsing and the command-line tests.

| Game | What it is | Language |
|---|---|---|
| [solitaire-tui](solitaire-tui/) | Klondike solitaire in the terminal, with mouse play, auto-place and a learning interface | Python |
| [kilix-lander](kilix-lander/) | Lunar lander with a software-rendered framebuffer and a trained neural pilot | C |
| [joustix](joustix/) | Fast flying-joust arcade game with a trained neural rider | C |
| [kilix-brokeout](kilix-brokeout/) | Breakout/Arkanoid-style brick breaker with a trained neural player | C |
| [bashed-earth](bashed-earth/) | Turn-based artillery combat | C |
| [kilix-lights](kilix-lights/) | Full-colour Lights Out puzzle | C |
| [kilix-jpak](kilix-jpak/) | Clean-room action-puzzle game | C |
| [kilix-pong](kilix-pong/) | Paddle-ball with three beatable CPU levels and a trained neural player | C |
| [chess-bash](chess-bash/) | Animated isometric chess | C |
| [kilix-rancher](kilix-rancher/) | Creature-raising game | C |
| [kilix-fishtank](kilix-fishtank/) | Arcade-style virtual fishtank | C |
| [tictactoe-tui](tictactoe-tui/) | Tic-tac-toe in the terminal it runs from, with a trained neural opponent | C |

Ten of the C games moved here from their own repositories with their full
history; solitaire-tui and tictactoe-tui were started here. The old repositories
are archived and point here (see [Superseded repositories](#superseded-repositories)).

## Build and test

```sh
git clone --recurse-submodules https://github.com/itsmygithubacct/kilix-games.git
cd kilix-games
make test                 # every game
make -C kilix-pong test   # one game
```

`make all` builds every C game; `make -C <game> all` builds one, and the game's
README says how to run it. The C games need a C compiler, make and zlib, and
most also libm and pthreads; solitaire-tui needs only Python 3.

## Usage

Inside Kilix, start a game from the Kilix 95 Start menu's Games list, the TUI's
Programs → Games list, or by catalog id on the command line:

```sh
kilix games play kilix-pong               # install if needed, then start
kilix games play kilix-pong --setup-only  # install only
kilix games list                          # games available in Kilix 95
```

| Catalog id | Directory | Binary | Kilix catalog label |
|---|---|---|---|
| `bashed-earth` | [bashed-earth](bashed-earth/README.md) | `bashed-earth/bashed-earth` | Bashed Earth |
| `chess-bash` | [chess-bash](chess-bash/README.md) | `chess-bash/chess-bash` | Chess Bash |
| `joustix` | [joustix](joustix/README.md) | `joustix/joustix` | Joustix |
| `kilix-fishtank` | [kilix-fishtank](kilix-fishtank/README.md) | `kilix-fishtank/kilix-fishtank` | Kilix Fishtank |
| `kilix-jpak` | [kilix-jpak](kilix-jpak/README.md) | `kilix-jpak/kilix-jpak` | Kilix JPAK |
| `kilix-lights` | [kilix-lights](kilix-lights/README.md) | `kilix-lights/bin/kilix-lights` | Kilix Lights |
| `kilix-pong` | [kilix-pong](kilix-pong/README.md) | `kilix-pong/kilix-pong` | Kilix Pong |
| `kilix-rancher` | [kilix-rancher](kilix-rancher/README.md) | `kilix-rancher/kilix-rancher` | Kilix Rancher |
| `kitty-brokeout` | [kilix-brokeout](kilix-brokeout/README.md) | `kilix-brokeout/kitty-brokeout` | Kilix Brokeout |
| `solitaire` | [solitaire-tui](solitaire-tui/README.md) | `solitaire-tui/bin/solitaire-tui` (no build step) | Solitaire TUI |
| `terminal-lander` | [kilix-lander](kilix-lander/README.md) | `kilix-lander/terminal-lander` | Kilix Lander |
| `tictactoe-tui` | [tictactoe-tui](tictactoe-tui/README.md) | `tictactoe-tui/tictactoe-tui` | Tic-Tac-Toe |

Two ids keep their pre-monorepo names (`kitty-brokeout`, `terminal-lander`) so
existing installations and preferences keep working.

## How it works

Each C game reaches the shared SDK through
`<game>/third_party/kilix-game-sdk -> ../../third_party/kilix-game-sdk`, so its
own Makefile is unchanged. It builds its copy of the kit under
`<game>/build/kilix-game-kit`, so games never share objects compiled with
different flags. The SDK is pinned at one commit for every game; advancing it
is one reviewed submodule commit that `make test` checks across all of them.

Kilix installs these games from its content catalog, which pins a commit of
this repository and names each game's directory and binary.

```
 kilix-content catalog entry (id, repository, ref 668faacf, build, binary)
        │  kilix games play <id>
        ▼
 shallow fetch of this repository at the ref, submodules included
        │  make -C <game> all
        ▼
 <game>/third_party/kilix-game-sdk ──▶ third_party/kilix-game-sdk (one pin for all games)
        │                                kilix-game-kit: terminal session, soft-raster,
        │                                pcm-mixer, kilix-state
        ▼
 Kilix opens the binary in a terminal tab
```

The graphical games draw with soft-raster, present frames through the Kitty
graphics protocol and play sound through pcm-mixer, all supplied by
kilix-game-kit. solitaire-tui is pure Python with curses, and tictactoe-tui runs
in text mode in the terminal it starts from. Six of the C games (bashed-earth,
joustix, kilix-brokeout, kilix-lander, kilix-pong and tictactoe-tui) ship a small
trained neural player that runs inside the game through kilix-game-kit's
policy-network runtime, with its provenance in the game's
`docs/neural-policy-provenance.json`.

## Adding a game

Give it a directory named after the game, a `Makefile` with `all` and `test`,
a README, and nothing outside its directory except the shared SDK. Add it to
`GAMES` in the top-level `Makefile` and to the CI matrix.

## Documentation

Each game's README is its documentation: [solitaire-tui](solitaire-tui/README.md),
[kilix-lander](kilix-lander/README.md), [joustix](joustix/README.md),
[kilix-brokeout](kilix-brokeout/README.md), [bashed-earth](bashed-earth/README.md),
[kilix-lights](kilix-lights/README.md), [kilix-jpak](kilix-jpak/README.md),
[kilix-pong](kilix-pong/README.md), [chess-bash](chess-bash/README.md),
[kilix-rancher](kilix-rancher/README.md), [kilix-fishtank](kilix-fishtank/README.md)
and [tictactoe-tui](tictactoe-tui/README.md). Games with more depth keep it in
their own `docs/` directory, for example asset and audio provenance, neural
policy provenance, manual pages, and joustix's
[Kilix integration notes](joustix/docs/kilix-integration.md).

## Superseded repositories

These standalone repositories are archived on GitHub. Their history continues
in the directory named here; they stay readable for installs pinned to older
commits.

| Archived repository | Now |
|---|---|
| [bashed-earth](https://github.com/itsmygithubacct/bashed-earth) | [bashed-earth/](bashed-earth/) |
| [chess-bash](https://github.com/itsmygithubacct/chess-bash) | [chess-bash/](chess-bash/) |
| [joustix](https://github.com/itsmygithubacct/joustix) | [joustix/](joustix/) |
| [kilix-brokeout](https://github.com/itsmygithubacct/kilix-brokeout) | [kilix-brokeout/](kilix-brokeout/) |
| [kilix-fishtank](https://github.com/itsmygithubacct/kilix-fishtank) | [kilix-fishtank/](kilix-fishtank/) |
| [kilix-jpak](https://github.com/itsmygithubacct/kilix-jpak) | [kilix-jpak/](kilix-jpak/) |
| [kilix-lander](https://github.com/itsmygithubacct/kilix-lander) | [kilix-lander/](kilix-lander/) |
| [kilix-lights](https://github.com/itsmygithubacct/kilix-lights) | [kilix-lights/](kilix-lights/) |
| [kilix-pong](https://github.com/itsmygithubacct/kilix-pong) | [kilix-pong/](kilix-pong/) |
| [kilix-rancher](https://github.com/itsmygithubacct/kilix-rancher) | [kilix-rancher/](kilix-rancher/) |

## Related repositories

- [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme),
  pinned at `third_party/kilix-game-sdk`, and through its game kit
  [kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme),
  [kitty-framebuffer](https://github.com/itsmygithubacct/kitty-framebuffer#readme),
  [kitty-input](https://github.com/itsmygithubacct/kitty-input#readme),
  [soft-raster](https://github.com/itsmygithubacct/soft-raster#readme),
  [pcm-mixer](https://github.com/itsmygithubacct/pcm-mixer#readme) and
  [kilix-state](https://github.com/itsmygithubacct/kilix-state#readme).
- [kilix-content](https://github.com/itsmygithubacct/kilix-content#readme): the
  catalog that pins this repository for each game.
- Games kept in their own repositories:
  [pleb-tower](https://github.com/itsmygithubacct/pleb-tower#readme),
  [super-kilix](https://github.com/itsmygithubacct/super-kilix#readme),
  [kilix-land](https://github.com/itsmygithubacct/kilix-land#readme) and
  [pleb-plant-grower](https://github.com/itsmygithubacct/pleb-plant-grower#readme).

## Part of Plebian-OS

kilix-games is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
Its games reach a user as twelve entries of the Kilix content catalog, each
fetched and built on first launch. The map of every repository and how it ships
is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. Games carry their own LICENSE files where they have third-party notices.
