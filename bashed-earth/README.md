# Bashed Earth

Turn-based artillery combat in your terminal, in the grand tradition of
Scorched Earth and Worms — rendered as real pixels via the **kitty
graphics protocol**. No SDL, no X11, no ncurses: the whole game is a
software-rasterized, antialiased framebuffer zlib-compressed and streamed
to the terminal as base64 APC escape sequences at a steady 30 fps
(double-buffered image ids + DEC 2026 synchronized updates, so no flicker;
encoding runs on its own thread, so no stutter).

Built for [kilix](https://github.com/itsmygithubacct/kilix) (and any
kitty-protocol terminal: kitty, ghostty, wezterm...). **Linux only.**

![Bashed Earth: a nuke detonates while napalm burns on the hillside](docs/screenshot.png)

## Features

- **Falling-sand destructible terrain** — 1px cellular automaton with
  grass/sand/ice worlds, lakes, flowing water, snow, and ice that shatters
  into avalanches when explosions cut it loose
- **15 weapons** — from the Baby Missile to the Nuke: Triple, Bouncy,
  Roller (rolls downhill into dug-in tanks), Drill, Digger, Napalm
  (spreads burning fire that creeps downhill and is doused by water),
  Dirt, MIRV, plus Raft / Parachute / Shield utility items
- **Weapon store & economy** — $10k per match, leftover cash and unused
  ammo carry across matches, winner bonus, multi-match scoreboard
- **5 AI personalities** — Aggressive, Defensive, Tactical, Balanced,
  Trickster; each with its own shopping list, weapon priorities, aim
  accuracy, taunts, and name pool
- **A neural gunner** — a sixth opponent, Neural, whose weapon, barrel angle
  and power come from a small trained network in one step, with no
  trajectory search. It won 93.7% of 2,000 held-out duels against the five
  classic personalities. Set Player 1 to Neural to watch it play
  ([below](#neural-gunner))
- **Weather** — wind that pushes shells, rain that fills craters, snow
  that buries ice worlds
- **Hazards** — fall damage (parachutes save you), buried-alive damage,
  self-hits, and a stalemate rule so dug-in wars end
- **Locally generated sound bank** — 15 reviewed shot, explosion, impact,
  splash, ricochet, drill, UI, and victory WAVs load at startup; any missing
  or invalid asset uses the built-in synthesized fallback. Everything is
  mixed live and streamed to whatever audio sink your system has
  (`pacat` / `pw-play` / `aplay` / sox `play`); silently disabled if none

## Build

Linux only. Needs gcc or clang, zlib, libm, and pthreads:

```sh
make
./bashed-earth
```

Sound plays through the first available CLI sink (`pacat`, `pw-play`,
`aplay`, or sox's `play`) — no sink, no sound, no problem.

## Controls

| Key | Action |
|-----|--------|
| Left / Right | aim barrel |
| Up / Down | power |
| Space / Enter | fire |
| 1-0 | weapon hotkeys |
| D / R | Drill / Roller |
| Tab | cycle weapons |
| M | toggle sound |
| Esc / P | pause menu: resume, main menu, quit |

Menus: arrows to navigate, Left/Right to change values, Enter to confirm.
The game is left through QUIT on the start, pause or match-over menu (Esc on
the start menu jumps to QUIT); Ctrl+C also exits and restores the terminal.
In the pause and match-over menus only Enter confirms, since Space fires.
When nobody human is playing (Player 1 set to Neural), the match-over menu
counts down six seconds and starts the next match; any key stops it.
Options persist to `~/.config/bashed-earth.conf` (`$XDG_CONFIG_HOME` if set).

## Neural gunner

Each opponent row cycles Off, AI: Random, the five classic personalities and
**Neural**; the Player 1 row switches between You and **Neural (watch)**,
which plays every seat by itself. Random still picks among the five classic
personalities only.

On its turn Neural picks its target by the classic rule (the weakest tank),
reads 44 numbers about the shot (the target's offset, the wind, 20 terrain
heights along and past the line of fire, both tanks' health and cover, its
ammo) and a 44-64-64-14 network answers with a weapon, an angle and a power.
It shops like Balanced. If its policy ever failed to load it would play as
Balanced. The network, how it was trained, and the full evaluation are in
[tools/neural/README.md](tools/neural/README.md).

## Development

```sh
make test                          # neural checks + headless AI-vs-AI selftests
./bashed-earth --menu-test         # menu-only exits, pause, match-over menu and countdown
./bashed-earth --neural-test       # the gunner loads, plays and replays; fixed duels
./bashed-earth --selftest 42 3     # specific seed, 3 matches
./bashed-earth --render-test 7     # dump render_*.ppm screenshots
BE_DEBUG=1 ./bashed-earth --selftest 1 1   # tick-by-tick state trace
```

The selftest plays full 4-AI matches headlessly (store, combat, economy,
carry-over) and checks invariants — no terminal needed, so it runs in CI.
Test modes keep their options file in a private temporary directory, and
`make test` runs them under a sentinel `HOME` and `XDG_CONFIG_HOME` and fails
if anything is written there. `make sanitize` repeats the checks under
ASan+UBSan.

## Architecture

| File | Role |
|------|------|
| `src/term.c` | key decoding around the vendored `kitty-framebuffer` session |
| `src/terrain.c` | falling-sand automaton with per-row active-span tracking |
| `src/game.c` | tanks, projectiles, flames, explosions, AI, store, turn flow |
| `src/neural.c` | the neural gunner: features, aim, and the embedded policy (`src/neural_policy_blob.h`) |
| `src/render.c` | scene, HUD, menus, and Scale2x/3x text over vendored `soft-raster` primitives |
| `src/sound.c` | reviewed WAV bank + procedural fallback routed through vendored `pcm-mixer` |
| `src/config.c` | weapon/AI/color data tables |
| `src/main.c` | 30 fps loop (60 Hz logic), selftest, render-test, neural-test |
| `tools/neural/` | the gunner's training and evaluation lab and policy tooling |

The four shared runtime libraries are pinned under `third_party/`, so a
normal checkout remains self-contained.

## License

Code is MIT; the shipped SFX bank is CC0-derived. See [LICENSE](LICENSE) and
[the per-file audio provenance](docs/audio-provenance.json). The embedded terminal font comes from
Debian console-setup's public-domain console fonts (details in
`third_party/soft-raster/src/font8x16.h`).
