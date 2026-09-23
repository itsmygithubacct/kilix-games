# kilix-games

A monorepo of small games for Kilix and other Kitty-graphics terminals. Each
game lives in its own directory and builds and tests on its own. The C games
share one [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk)
checkout at `third_party/kilix-game-sdk`.

| Game | What it is | Language |
|---|---|---|
| [solitaire-tui](solitaire-tui/) | Klondike solitaire in the terminal, with mouse play, auto-place and a learning interface | Python |
| [kilix-lander](kilix-lander/) | Lunar lander with a software-rendered framebuffer | C |
| [joustix](joustix/) | Fast flying-joust arcade game | C |
| [kilix-brokeout](kilix-brokeout/) | Breakout/Arkanoid-style brick breaker | C |
| [bashed-earth](bashed-earth/) | Turn-based artillery combat | C |
| [kilix-lights](kilix-lights/) | Full-colour Lights Out puzzle | C |
| [kilix-jpak](kilix-jpak/) | Clean-room action-puzzle game | C |
| [kilix-pong](kilix-pong/) | Paddle-ball with three beatable CPU levels and a trained neural player | C |
| [chess-bash](chess-bash/) | Animated isometric chess | C |
| [kilix-rancher](kilix-rancher/) | Creature-raising game | C |
| [kilix-fishtank](kilix-fishtank/) | Arcade-style virtual fishtank | C |

Each C game except solitaire-tui moved here from its own repository with its
full history. The old repositories are archived and point here.

## Build and test

```sh
git clone --recurse-submodules https://github.com/itsmygithubacct/kilix-games.git
cd kilix-games
make test                 # every game
make -C kilix-pong test   # one game
```

Each C game reaches the shared SDK through
`<game>/third_party/kilix-game-sdk -> ../../third_party/kilix-game-sdk`, so its
own Makefile is unchanged. It builds its copy of the kit under
`<game>/build/kilix-game-kit`, so games never share objects compiled with
different flags. The SDK is pinned at one commit for every game; advancing it
is one reviewed submodule commit that `make test` checks across all of them.

Kilix installs these games from its content catalog, which pins a commit of
this repository and names each game's directory and binary.

## Adding a game

Give it a directory named after the game, a `Makefile` with `all` and `test`,
a README, and nothing outside its directory except the shared SDK. Add it to
`GAMES` in the top-level `Makefile` and to the CI matrix.

## License

MIT. Games carry their own LICENSE files where they have third-party notices.
