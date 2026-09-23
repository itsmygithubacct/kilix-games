# Kilix Brokeout

A native C Breakout/Arkanoid-style game rendered as real pixels through the
kitty graphics protocol. No SDL, no X11, no ncurses: the game renders a
software RGBA framebuffer, compresses it with zlib, and streams it to the
terminal as kitty image frames.

![Kilix Brokeout gameplay screenshot](docs/screenshot.png)

The executable remains `kitty-brokeout` for compatibility with existing Kilix
installations and scripts.

The game has a responsive playfield, multi-hit bricks, metal bricks, explosive
bricks, speed bricks, falling capsules, particles, ball trails, screen shake,
level progression, a locally generated sound bank with procedural fallback,
and headless test modes.

## Features

- Kitty-protocol pixel graphics with double-buffered terminal frames
- Exact held-key movement with independent press/release tracking and a
  press-only compatibility fallback
- Fixed-timestep gameplay with substepped ball collisions
- Multi-hit, metal, explosive, and speed bricks
- Wide paddle, slow, multiball, and shield capsules
- Aimed launch with a visible guide line
- Persistent local best score
- Particles, ball trails, screen flash, and screen shake
- Nine reviewed local SFX with a safe procedural fallback, played through
  `pacat`, `pw-play`, `aplay`, or sox `play`
- Deterministic selftests and render tests for CI

## Build

Linux only. Needs gcc or clang, zlib, libm, pthreads, and a terminal that
supports the kitty graphics protocol:

```sh
make
./kitty-brokeout
```

Sound uses the first available sink among `pacat`, `pw-play`, `aplay`, or
sox `play`. If none exists, the game runs silently.

## Controls

| Key | Action |
|-----|--------|
| Left / A | move paddle left; aim left before launch |
| Right / D | move paddle right; aim right before launch |
| Down / S | center launch aim |
| Space / Enter / Up / W | launch ball, advance screens |
| P | pause |
| M | toggle sound |
| R | restart run |
| C | controls screen |
| Esc | title / resume |
| Q | quit |

## Development

```sh
make test
./kitty-brokeout --selftest 42 7200
./kitty-brokeout --render-test 7
./kitty-brokeout --sound-test
```

`--render-test` writes PPM screenshots for title, ready/aim, gameplay, level
clear, and game-over states without needing an interactive terminal.

## Architecture

| File | Role |
|------|------|
| `src/term.c` | input glue over `kitty_keyboard`, presenter glue over `kitty-framebuffer` |
| `src/game.c` | breakout rules, physics, levels, particles, powerups |
| `src/render.c` | scene, HUD, and menu drawing over the `soft-raster` primitives |
| `src/sound.c` | reviewed WAV bank + procedural fallback, played through `pcm-mixer` |
| `src/main.c` | interactive loop, selftest, render-test, sound-test |

Generic terminal presentation, rasterization, and audio transport live in
vendored libraries under `third_party/`: `kitty-framebuffer` (kitty graphics
protocol frames, terminal setup/restore), `soft-raster` (anti-aliased
primitives and the 8x16 font), `pcm-mixer` (voice mixing and the audio sink
probe), and `kitty_keyboard` (keyboard protocol decoding).

## License

Code is MIT; the shipped SFX bank is CC0-derived. See [LICENSE](LICENSE) and
[the per-file audio provenance](docs/audio-provenance.json).
