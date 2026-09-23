# kilix-games

A monorepo of small games for Kilix and other terminals. Each game lives in
its own directory, builds and tests on its own, and has a launcher in
`<game>/bin/`.

| Game | What it is | Run |
|---|---|---|
| [solitaire-tui](solitaire-tui/) | Klondike solitaire in the terminal, with mouse, auto-place and a learning interface. It replaces kilix 95's desktop Solitaire. | `solitaire-tui/bin/solitaire-tui` |

```sh
make test                 # every game's tests
make -C solitaire-tui test
```

Conventions for a new game: a directory named after the game, a `Makefile`
with `test`, a launcher at `bin/<game>`, a README, and no dependencies beyond
what the README lists. Games share this repository's MIT licence.
