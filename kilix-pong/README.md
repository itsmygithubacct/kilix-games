# Kilix Pong

Kilix Pong is a fast, original paddle-ball arcade game for
[Kilix](https://github.com/itsmygithubacct/kilix), Kitty, and other Linux
terminals that implement the Kitty graphics protocol. It renders a software
RGBA framebuffer, compresses it with zlib, and streams real pixel frames to the
terminal—no SDL, X11, ncurses, or desktop window.

Either paddle can be a human, a CPU opponent at EASY, NORMAL or HARD, or a
trained neural player. Two humans get true local two-player controls through
the Kitty keyboard protocol's press/repeat/release events. Rallies accelerate
to a hard-capped top speed, paddle contact adds directional english, and the
first side to 11 wins.

## Features

- Luminous 320×180 logical arena scaled cleanly to the terminal's pixel grid
- Fixed 60 Hz deterministic simulation with a 30 fps asynchronous presenter
- Bounded-substep ball collision, contact separation, and anti-tunneling tests
- Human, CPU (three beatable levels) and neural controllers on either side
- A 5,123-parameter neural player trained from the game's own simulation
  (see [`tools/neural/`](tools/neural/README.md)), compiled in and verified
- Title-menu setup remembered between sessions through `kilix-state`
- Real held-key input on Kitty/Kilix, with a safe legacy key-repeat fallback
- Ball trails, impact particles, score flash, screen shake, and responsive UI
- Twenty-one original WAV effects generated deterministically by Python
- Resize handling, installed asset discovery, and careful terminal restoration
- Headless rules, replay-digest, render, audio, and install-layout checks

## Build

Kilix Pong needs a C11 compiler, zlib, libm, POSIX threads, Python 3 for asset
verification, and a Kitty-graphics terminal for interactive play.

```sh
make
./kilix-pong
```

On Debian or Ubuntu, the native build dependencies are:

```sh
sudo apt install build-essential zlib1g-dev python3
```

Audio is optional at runtime. The game tries `pacat`, `pw-play`, `aplay`, then
SoX `play`; if no sink opens, play continues silently. The checked-in WAV bank
means normal builds and gameplay do not invoke Python.

## Controls

| Key | Action |
|---|---|
| W / S | move player 1 up / down |
| Up / Down | move player 2 up / down in local 2P |
| Up / Down (title) | choose the LEFT, RIGHT or LEVEL row |
| Left / Right (title) | change that row: HUMAN / CPU / NEURAL, or EASY / NORMAL / HARD |
| Enter / Space | start or confirm |
| P / Esc | pause or resume |
| M | toggle sound |
| Q | quit |

With exactly one human, W/S and Up/Down both move that paddle, whichever side
it is on. The setup can also be given on the command line, which overrides
the remembered one for that session:

```sh
kilix-pong --left human --right cpu --level hard
kilix-pong --left neural --right cpu --level hard   # watch the neural player
```

## Opponents

The CPU reacts late, commits to a prediction whose error grows with ball speed
and wall bounces, and sharpens it only a bounded number of times. NORMAL and
HARD also aim their returns away from you. Every level is beatable. Against a
scripted human proxy (0.20-0.27 s reactions), a 100-match frozen test gave:

| Level | Proxy match wins | Neural match wins |
|---|---:|---:|
| EASY | 100% | 100% (1100-1 on points) |
| NORMAL | 69% | 100% (1100-1) |
| HARD | 36% | 100% (1100-1) |

The neural player is a classifier: 11 mirrored table features pass through
two 64-unit ReLU layers to up, stay or down. It learned from an exact
lookahead planner that finds shots the CPU cannot reach, so it wins by
angling returns rather than by defence alone: HARD misses 1.1 times per
minute against it, against 0.66 for a flawless centre-strike defender. Two
neural players never miss, so NEURAL vs NEURAL is an endless rally. Its
weights, digest and training record are in
[`docs/neural-policy-provenance.json`](docs/neural-policy-provenance.json).

Local 2P is best in Kilix or Kitty, where the extended keyboard protocol
reports independent key releases. A terminal that only sends legacy keypresses
still works, but held movement uses a short repeat-based timeout.

## Install

```sh
sudo make install
kilix-pong
```

`PREFIX` defaults to `/usr/local`, and `DESTDIR` is honored for packaging.
The install contains the binary, manual page, and production sound bank.

## Original Python sound bank

Every sound is synthesized from scratch by `tools/gen_sfx.py`; there are no
recordings, samples, model outputs, or third-party audio assets. The generator
uses fixed-point integer oscillators, polynomial envelopes, and fixed-seed
xorshift noise rather than platform math-library transcendentals, so it can
reproduce the exact checked-in bytes.

```sh
make sfx        # intentionally regenerate the canonical bank
make check-sfx  # non-writing byte, PCM-format, and SHA-256 verification
```

The artifact-level hashes and format metadata are recorded in
[`docs/audio-provenance.json`](docs/audio-provenance.json).

## Development and verification

```sh
make test
make sanitize
./kilix-pong --rules-test
./kilix-pong --ai-test
./kilix-pong --match neural cpu hard 1 10   # headless tournament, JSON
./kilix-pong --selftest 1337 12000
./kilix-pong --render-test 7 /tmp/kilix-pong-renders
./kilix-pong --sound-test
```

`make test` compares two identical seeded runs byte-for-byte, exercises a
second seed, checks collision, scoring, menu, CPU and neural-feature rules,
runs the CPU-level and neural tournaments, verifies the neural policy blob
against its header and manifest, validates all 21 regenerated WAVs, inspects
seven deterministic PPM scenes in a temporary directory, and tests the
installed asset layout. It leaves no test output in the repository.

## Architecture

| File | Responsibility |
|---|---|
| `src/game.c` | deterministic rules, CPU levels, neural controller, held input, collisions, particles |
| `src/render.c` | software rasterizer, arena, HUD, menus, render fixtures |
| `src/term.c` | Kitty keyboard parsing and resize around vendored `kitty-framebuffer` |
| `src/sound.c` | WAV banks routed through vendored `pcm-mixer` |
| `src/main.c` | interactive loop, asset discovery, CLI and headless checks |
| `tools/gen_sfx.py` | canonical deterministic Python audio source |
| `tools/neural/` | training lab, planner, trainer and policy gate for the neural player |
| `src/neural_policy_blob.h` | generated from `assets/policy/pong-neural.kxpol`; do not edit |

The shared runtime sources are kept under `third_party/`, so builds do not
depend on separately installed copies.

Research notes, rejected sound candidates, test renders, sanitizer logs, and
collaboration state are intentionally kept outside this release tree.

Kilix Pong is an independently authored paddle-ball game. It contains no code,
art, audio, tables, branding, or other assets from Atari's commercial Pong
releases.

## License

MIT. See [LICENSE](LICENSE).
