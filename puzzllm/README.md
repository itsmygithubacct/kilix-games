# puzzllm

Kilix, the firekitten from Zell, is locked in a room with a coding agent.
The agent is [omp](https://github.com/can1357/oh-my-pi) running a small local
model, Qwen 3.5, in a sandbox. Each room has a coding goal. Get the agent to
meet it and the door at the top of the room opens onto the next one. Part of
[kilix-games](../).

```sh
bin/puzzllm            # builds on first run, then plays (Kilix or Kitty tab)
make test              # unit, command-line, sandbox and planted-defect tests
make test-live         # level 1 with the real model (not part of make test)
```

## The room

| Screen | What it is |
|---|---|
| left, big | omp, the coding agent, talking to Qwen 3.5 |
| top right | a bash shell in the same sandbox and the same workspace |
| bottom right | the goal, its hints, and what the agent is doing: its tool calls and their results, what it said, files created, changed and removed |

Walk Kilix up to a screen and press Enter to type into it. The door's lamp
turns green and the door slides open when the goal is met. Walk up the
aisle through it to reach the next room.

| Key | Action |
|---|---|
| arrows, WASD | walk |
| Enter, Space, E | use the screen Kilix is next to (the activity screen gives the next hint) |
| F1 / F2 | type into omp / the shell from anywhere |
| F10 or Ctrl+] | stop typing and walk again |
| H | next hint |
| Ctrl+Q | quit (while walking) |
| mouse | click a screen to type into it, the floor to walk again |

Hints come one at a time, every 45 seconds or on H. Progress is kept in
`~/.local/share/puzzllm/progress`, so the game resumes in the room you
reached. If an omp or shell session ends, press Enter on its screen to
start a new one.

## Level 1: Hello, world

Get the agent to create `hello_world.txt`. The goal is met when the
workspace holds a regular file of that name (a symlink does not count), of
1 to 4096 bytes, containing "hello" and "world" in any case. `Hello, World!`
passes and an empty file does not. The player may also create the file by
hand in the shell; that counts the same. A 0.8B model often says it wrote
the file when it did not, so check with `ls` and `cat` in the shell.

Room 2 is a stub: its sign reads "coming soon" and its door stays shut.

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
`nonempty`, `contains TEXT`, `contains-ci TEXT` or `max-bytes N`. `room`
picks the room's look (`stone` or `brick`). A new level is a new block,
not new code. The full format is in [`src/levels.h`](src/levels.h).

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

`make test-sandbox` proves this from inside both sandboxes. Writes outside
the workspace fail. The host loopback and outside addresses are unreachable.
The host home is invisible. Every writable mount is one of those above.
Nothing appears in a host folder next to the state. It also runs the whole
agent chain with stand-ins for llama-server and omp.

### What it needs

| Part | Default | Override |
|---|---|---|
| bubblewrap | `bwrap` on `PATH` | |
| omp | `omp` on `PATH`, then `~/.local/bin/omp` | `PUZZLLM_OMP` |
| llama-server | `llama-server` on `PATH`, then a Kilix LLM bench build under `~/.local/share/kilix-llm/bench/engine/` | `PUZZLLM_LLAMA_SERVER` |
| Qwen 3.5 weights | `Qwen3.5-0.8B-Q8_0.gguf`, the model Zell routes to as `qwen3.5-0.8b`, in the Kilix LLM models folder | `PUZZLLM_MODEL` |

Nothing is downloaded. If a part is missing, the omp screen says which one.
`libexec/puzzllm-sandbox paths` prints what was found. `PUZZLLM_GPU=Vulkan1`
(any llama.cpp device name) runs the model on a GPU and mounts the GPU's
device nodes into the agent sandbox. The default is CPU. `PUZZLLM_THINK=1`
lets the model think, which is off by default because 0.8B tends to loop.

## Tests

`make test` runs four suites, all headless:

- `test-unit` covers the terminal emulator, key encoding, level parsing and
  checks, JSON, the activity feed, walking and the door, rendering, and the
  whole level-1 flow.
- `test-cli` covers `--version`, `--check-level`, `--render`, and bad
  input.
- `test-sandbox` covers confinement, persistence, the agent chain, and the
  goal met by hand from the shell.
- `test-defects` plants 14 defects: six in the sandbox, eight in the game.
  For each, it checks that the right suite turns red. A defect nobody
  notices fails the run.

Test scratch stays under `build/`.

`build/puzzllm --render DIR` draws the room as PPM files: door shut, door
open, and room 2. `build/puzzllm --check-level N` says whether level N's goal
is met in the workspace now.

## Art

The art is placeholder. The room, door and screen bezels are drawn from
primitives in [`src/art.c`](src/art.c). Kilix is Zell's walk cycle in
[`src/kilix_sprite.c`](src/kilix_sprite.c), whose header gives its
provenance. Replacing those two files behind [`src/art.h`](src/art.h) and
[`src/kilix_sprite.h`](src/kilix_sprite.h) swaps the graphics, and nothing
else needs to change.

## License

MIT. Kilix's frames come from Kilix Land through Zell, both MIT
(see [`src/kilix_sprite.c`](src/kilix_sprite.c)).
