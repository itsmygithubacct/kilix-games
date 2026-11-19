# puzzllm

Kilix, the firekitten from Zell, is locked in a room with a coding agent.
The agent is [omp](https://github.com/can1357/oh-my-pi) running a small local
model, Qwen 3.5, in a sandbox. Each room has a coding goal. Get the agent to
meet it and the door at the top of the room opens onto the next one. Part of
[kilix-games](../).

```sh
bin/puzzllm            # builds on first run, then plays (Kilix or Kitty tab)
make test              # unit, command-line, sandbox and planted-defect tests
make test-live         # level 1 with the real model, not part of make test
                       # (PUZZLLM_LIVE_TIMEOUT: seconds per try, default 1200)
make bench             # how long a busy frame takes to draw, and which renderer
```

Kilix and the drawing come from zell-neuronte: his standing and walking
pictures, his pace and walk timing, and the Neuronte renderer, in which
Neuronte nets draw the text, the terminal characters and Kilix himself (see
[Drawing](#drawing)).

## The room

| Screen | What it is |
|---|---|
| left, big | omp, the coding agent, using the selected model |
| top right | the goal and its status in a dedicated block, above hints and agent activity |
| bottom right | a bash shell in the same sandbox and the same workspace |

The room is drawn at the pane's own size, so a bigger pane gets bigger
terminals. Walk Kilix up to a screen and press Enter to type into it. The door's lamp
turns green and the door slides open when the goal is met. Walk up the
aisle through it to reach the next room.

| Key | Action |
|---|---|
| arrows, WASD | walk |
| Enter, Space, E | use the screen Kilix is next to (the activity screen gives the next hint) |
| F1 / F2 | type into omp / the shell from anywhere |
| F10 or Ctrl+] | stop typing and walk again |
| H | next hint |
| Esc | open or close the game menu from any screen |
| Ctrl+Q | quit (while walking) |
| mouse | click a screen to type into it, the floor to walk again |

Hints come one at a time, every 45 seconds or on H. Progress is kept in
`~/.local/share/puzzllm/progress`, so the game resumes in the room you
reached. If an omp or shell session ends, press Enter on its screen to
start a new one.

The game opens on its menu. Use Up and Down to choose **Start game** or
continue your save, **Exit game**, **Reset progress**, or an agent model.
Press Enter to select. Reset asks for confirmation, backs up the current
workspace, and returns you to the menu at level 1. The list refreshes each time you open
the menu and includes the default llama.cpp model and the models installed
on the local Ollama service. Model changes restart the agent with a fresh
conversation in the same workspace. Choices apply until you quit.

Ollama must be running locally. PuzzLLM reads the local model list using
Python 3. It uses port 11434 by default and respects a loopback
`OLLAMA_HOST`. Ollama models vary in their support for chat and tool calls.

## Level 1: Hello, world

Get the agent to create `hello_world.txt`. The goal is met when the
workspace holds a regular file of that name (a symlink does not count), of
1 to 4096 bytes, containing "hello" and "world" in any case. `Hello, World!`
passes and an empty file does not. The player may also create the file by
hand in the shell; that counts the same. A 0.8B model often says it wrote
the file when it did not, so check with `ls` and `cat` in the shell.

The upper-right activity feed clears when you walk into a new room.
Completing a room moves its workspace files into `workspace/level_N/`.
With the game closed, run `bin/puzzllm --reset-progress` to start at level 1.
The previous workspace is kept under a `reset-*` directory in the game state.

## Levels are data

Levels live in [`levels/levels.txt`](levels/levels.txt), one block each:

```
level: 1
title: Hello, world
room: stone
goal: Get the agent to create hello_world.txt saying Hello, World!
hint: Walk to the big omp screen on the left and press Enter to talk to the agent.
check: file hello_world.txt
require: nonempty
require: contains-ci hello
require: contains-ci world
require: max-bytes 4096
```

`check: file NAME` takes a plain file name in the workspace. `check: none`
makes a room that is never finished, which is a stub. `require` can be
`nonempty`, `contains TEXT`, `contains-ci TEXT`, `contains-any-ci A | B`
(at least one of them, in any case), `lacks-ci A | B` (none of them),
`max-bytes N`, `bytes N` (exactly N bytes, up to 16 MiB) or `only CHARS`
(no other byte anywhere). `room` picks the room's look (`stone` or
`brick`). A new level is a new block, not new code. The full format is in
[`src/levels.h`](src/levels.h).

The levels so far:

1. **Hello, world:** `hello_world.txt` saying Hello, World!
2. **The carwash:** `carwash.txt` saying that to take the car to the
   carwash you must drive, because the car has to be at the carwash to be
   washed. Small models tend to say "walk, it's close". The check wants
   the carwash named, drive, and a reason, and refuses advice to walk:
   ```
   require: lacks-ci should walk | better to walk | recommend walking | walking is better | walk instead
   require: contains-any-ci carwash | car wash | car-wash
   require: contains-any-ci drive | driving | drove
   require: contains-any-ci because | since | so that | otherwise | has to be | must be | needs to be | need to be
   ```
   It matches words, not meaning, so the player decides whether the
   agent really understood.
3. **A million ones:** `1.txt` holding exactly 1,000,000 ones and nothing
   else, so no spaces and no line ending:
   ```
   check: file 1.txt
   require: bytes 1000000
   require: only 1
   ```
   A small model cannot type this. The player has to get it to use a
   command or a short program, and to leave off the line ending that
   `echo` and `print` add. When the check fails it says why, for example
   `1.txt has 1,000,001 bytes, one too many: often a line ending at the end`.
4. The treasure chest. Extract `key.txt` from the password-protected
   `chest.zip` to open the door. The game uses `zip` to create the chest,
   and you can open it with `unzip` in the shell. Resuming keeps the same chest.
5. A stub room for the next challenge.

## The sandbox

Both screens run in [bubblewrap](https://github.com/containers/bubblewrap)
sandboxes started by [`libexec/puzzllm-sandbox`](libexec/puzzllm-sandbox):

- Every namespace is unshared, network included. The only reachable address
  is the sandbox's own loopback, so nothing in it can reach the host or the
  internet.
- `/usr` and a short list of files from `/etc` are read-only. The sandbox
  root is read-only. `$HOME` and `/tmp` are fresh tmpfs. Nothing of the
  user's home is visible.
- Two folders are writable, both under `~/.local/share/puzzllm/` (or
  `$PUZZLLM_STATE`), and both survive restarts:
  - `workspace/`, mounted as `/kilix/workspace`. It is the working
    directory of both omp and the shell.
  - `agent/`, mounted as omp's `~/.omp/agent` in the agent sandbox only.
    It holds omp's config and sessions.
- In the agent sandbox, `puzzllm-agent-init` starts `llama-server` on the
  sandbox loopback with the Qwen weights mounted read-only. It waits for the
  server to answer, then starts omp. omp's `models.yml` is written fresh in
  the sandbox's own `agent/` folder, with one provider, `puzzllm-local`. The
  user's `~/.omp` is never mounted, so no other provider or key is visible.
  If you select Ollama, the agent keeps its private network namespace and
  reaches only the chosen model's chat endpoint through a local Unix socket.

`make test-sandbox` proves this from inside both sandboxes. Writes outside
the workspace fail. The host loopback and outside addresses are unreachable.
The host home is invisible. Every writable mount is one of those above.
Nothing appears in a host folder next to the state. It also runs the whole
agent chain with stand-ins for llama-server and omp.

Both embedded terminals use the NeuroTerm terminal core: the same patched
libvterm parser as the standalone NeuroTerm project, with styled Neuronte
glyphs on the game's canvas. The game supplies PTYs and keyboard events
through `src/neuroterm.h`. The parser source and its license are in
`third_party/libvterm/`.

### What it needs

| Part | Default | Override |
|---|---|---|
| bubblewrap | `bwrap` on `PATH` | |
| omp | `omp` on `PATH`, then `~/.local/bin/omp` | `PUZZLLM_OMP` |
| llama-server | `llama-server` on `PATH`, then a Kilix LLM bench build under `~/.local/share/kilix-llm/bench/engine/` | `PUZZLLM_LLAMA_SERVER` |
| Neuronte (drawing; optional) | a checkout at `~/projects/neuronte`, built from its two library sources | `make NEURONTE=DIR`, `RENDER=raster`; weights `PUZZLLM_NEURONTE_WEIGHTS` |
| Qwen 3.5 weights | `Qwen3.5-0.8B-Q8_0.gguf`, the model Zell routes to as `qwen3.5-0.8b`, in the Kilix LLM models folder | `PUZZLLM_MODEL` |

Nothing is downloaded. If a part is missing, the omp screen says which one.
`libexec/puzzllm-sandbox paths` prints what was found. `PUZZLLM_GPU=Vulkan1`
(any llama.cpp device name) runs the model on a GPU and mounts the GPU's
device nodes into the agent sandbox. The default is CPU. `PUZZLLM_THINK=1`
lets the model think, which is off by default because 0.8B tends to loop.

## Tests

`make test` runs four suites, all headless:

- `test-unit` covers the terminal emulator, key encoding, level parsing and
  checks (the carwash and the million ones included), JSON, the activity
  feed, Kilix's pictures and walk timing, the 60 Hz tick, the layout at ten
  pane sizes and resizing, both renderers, the frame-to-frame damage, rendering,
  and the whole level-1 flow. In a
  Neuronte build it insists that Neuronte draws, and runs the flow twice,
  once with each renderer.
- `test-cli` covers `--version`, `--check-level`, `--render`, `--bench`,
  `--renderer`, the raster fallback when the weights are missing, and bad
  input.
- `test-sandbox` covers confinement, persistence, the agent chain, and the
  goal met by hand from the shell.
- `test-defects` plants 29 defects: six in the sandbox, twenty-three in
  the game, two of them in the Neuronte renderer (skipped in a build without
  it). For each, it checks that the right suite turns red. A defect nobody
  notices fails the run.

Test scratch stays under `build/`.

`build/puzzllm --render DIR` draws the room as PPM files: door shut, door
open, and room 2. `build/puzzllm --check-level N` says whether level N's goal
is met in the workspace now.

## Drawing

Kilix is zell-neuronte's: his four standing and sixteen walking pictures,
84 pixels tall, copied from Zell with their provenance in
[`src/kilix_art.c`](src/kilix_art.c). He moves as Zell moves him: the room
runs at 60 ticks a second, he walks 3.5 pixels a tick, and each of his
four walk frames lasts seven ticks. Held keys are read from the terminal's
key releases, so he starts on the press and stops on the release.

[`src/draw.h`](src/draw.h) has two renderers behind one set of calls:

- **neuronte**, zell-neuronte's renderer, adapted from Zell's
  `draw_neuronte.c`. [Neuronte](https://github.com/itsmygithubacct/neuronte)
  nets draw every character on the screens, the bars and outlines are block
  glyphs, the shadow is the black circle, and Kilix goes through 2 x 4 octant
  cells whose masks come from the pixel net. It is built when the Makefile
  finds a Neuronte checkout at `NEURONTE` (default `~/projects/neuronte`), and
  its weights are `neuronte-split-v4.nrt`, the file zell-neuronte uses.
- **raster**, soft-raster's 8x16 font with box, block and braille characters
  drawn by hand. It is used in a build without Neuronte (`RENDER=raster`, or
  no checkout), when the weights do not open, or with `PUZZLLM_RENDER=raster`.

`build/puzzllm --renderer` prints which one is drawing. The floor, walls and
screen glass are drawn once and copied each frame. Each frame is compared
with the last one in 64 x 32 tiles, and only the tiles that changed are
sent to the terminal; a frame where nothing moved sends nothing.

`build/puzzllm --bench [WxH]` draws 240 busy frames headless (both terminals
full of coloured text, Kilix walking) and prints the average and worst
time. `PUZZLLM_STATS=FILE` makes a live game append its frame rate, frame
times and present counts to FILE once a second.

The room, door and screen bezels are placeholder primitives in
[`src/art.c`](src/art.c), behind [`src/art.h`](src/art.h). Replacing art.c
(and kilix_art.c) swaps the graphics, and nothing else needs to change.

## License

MIT. Kilix's pictures and the Neuronte renderer come from kilix-zell, which
is MIT (see [`src/kilix_art.c`](src/kilix_art.c) and
[`src/draw.c`](src/draw.c)). Neuronte is MIT and is compiled from its own
checkout, not copied here.
