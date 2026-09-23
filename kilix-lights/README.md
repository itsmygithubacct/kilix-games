# Kilix Lights

Kilix Lights is a standalone, full-color **Lights Out** puzzle for Kitty-graphics terminals. It opens directly into a solvable 5×5 board inside a generated retro electrical workshop. Every accepted switch press flips that switch and its orthogonal neighbors and plays a short mechanical light-switch sound.

The repository pins its C rendering, input, terminal-session, audio, and state dependencies through the recursive `third_party/kilix-game-kit` submodule. It does not use assets or source files from another game.

## Build and play

Clone recursively so the exact reviewed dependency versions are present:

```sh
git clone --recurse-submodules https://github.com/itsmygithubacct/kilix-lights.git
cd kilix-lights
make
./bin/kilix-lights
```

For an existing non-recursive clone, run `git submodule update --init --recursive` before building.

Kilix, kitty, Ghostty, and WezTerm are supported through the Kitty graphics protocol. Building requires GNU Make, a C11 compiler, zlib development headers, libm, and POSIX threads. The verification and asset-preparation targets additionally require Python 3 and Pillow. Binary packaging also uses `strip`, `strings`, and `tar`.

## Controls

- Click the visible switch plate to flip it. The transparent corners and spaces between plates are not clickable.
- Arrow keys or `WASD`: move keyboard focus.
- `Enter` or `Space`: flip the focused switch.
- `3`, `5`, `7`: choose Starter, Classic, or Expert.
- `N`: new solvable puzzle.
- `R`: reset the current puzzle exactly.
- `U` or `Backspace`: undo.
- `M`: mute or unmute the switch cue.
- `?` or `H`: help.
- `Q` or `Esc`: quit; `Esc` closes help first.

The objective is to turn every light off. Puzzle generation starts from the solved state and applies legal presses, so every supplied board has a solution.

## Assets and sound

The room and switch are original generated bitmap assets. Runtime copies are fixed-size P6 images so the C binary can load them without a large image library. The committed transparent switch mask is also the hit mask, keeping the interactive area aligned with the visible object.

The 155 ms switch cue is synthesized deterministically by `tools/generate_lightswitch.py` as mono PCM16 at 44.1 kHz. Live playback uses pcm-mixer through kilix-game-kit and gracefully falls back to silence if no `pw-play`, `pacat`, `aplay`, or `play` sink is available.

Set `KILIX_LIGHTS_AUDIO=off` to disable live audio. Set `KILIX_LIGHTS_ASSETS=/path/to/assets` to override the executable-relative asset directory.

## Verification and distribution

```sh
make test           # rules, input, interactions, audio, assets, and renders
make sanitize       # instrument the game and complete dependency stack
make release-check  # clean builds, tests, sanitizers, PTY restoration, package
make dist           # stripped binary, runtime assets, and all license notices
```

`make dist` writes a reproducible, platform-named archive under `dist/`. The binary remains beside an `assets/` directory so executable-relative resource discovery works after extraction. Useful headless commands include `--rules-test`, `--input-test`, `--interaction-test`, `--sound-test`, `--asset-test`, and `--render-test DIR`.

See [docs/ASSETS.md](docs/ASSETS.md) for asset provenance and [THIRD_PARTY.md](THIRD_PARTY.md) for dependency and license details.
