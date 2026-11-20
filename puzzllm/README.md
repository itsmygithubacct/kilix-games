# puzzllm

Kilix, the firekitten from Zell, is locked in a room with a coding agent.
The default agent is [omp](https://github.com/can1357/oh-my-pi) running a small local
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
| left | the selected coding agent |
| top right | the goal, status, and hints |
| middle right | system actions, including tool calls and workspace changes |
| above the shell | CPU, RAM, GPU, VRAM, and disk read/write activity |
| bottom right | a bash shell in the shared game home, inside tmux |

The shell runs in a tmux session and window called `shell`, inside the
player sandbox. Grok, Claude, Codex, and the local agent connect to that
same tmux server. Inspect it with `tmux capture-pane -p -t shell` (add
`-S -` for scrollback). The shell appends its raw output to `~/shell.log`
across rooms and sessions. The sandbox includes instructions in
`~/docs/tmux.md`. `~` is `/opt/puzzllm/home`, a symlink to the shared
workspace at `/kilix`.

Both terminals are panes of a private Kilix instance with their tab and pane
bars hidden. Type `kilix` to list them or `kilix ask 'list panes'` to ask in
ordinary language. The shell and the coding agent can control the same panes.
The sandbox mounts [the Kilix guide](libexec/docs/kilix.md) and
[the Ask guide](libexec/docs/kilix-ask.md) under `~/docs/`. The player shell
also has a tmux session named `shell`. On the first launch the game fetches
the small, pinned Needle command interpreter into its private state. Ask
uses its local grammar for pane actions.

Codex can read the log from its command sandbox. Resetting progress
backs up the log with the other files.

The resource bar samples the coding harness and any model process it can
attribute to that session. CPU is shown as a percentage of one logical
core. RAM uses resident memory and disk shows storage reads/writes per
second. Ollama reports VRAM for the selected model. If its shared runner
cannot be identified, CPU, RAM, and disk cover the local agent only.
Codex shows its local CLI resources. A dash means the metric is unavailable.

The room is drawn at the pane's own size, so a bigger pane gets bigger
terminals. Walk Kilix up to a screen and press Enter to type into it. The door's lamp
turns green and the door slides open when the goal is met. Walk up the
aisle through it to reach the next room.

| Key | Action |
|---|---|
| arrows, WASD | walk |
| Enter, Space, E | use the screen Kilix is next to (the goal pane gives the next hint) |
| Alt+Left / Alt+Right | focus the coding agent / shell while walking |
| Alt+Down | refocus the last terminal while walking |
| Ctrl+Shift+F1 / Ctrl+Shift+F2 | focus the agent / shell from either pane |
| Ctrl+Shift+F10 | return to the room |
| H | next hint |
| Esc | open the menu while walking, or go back in the menu |
| Ctrl+Shift+F12 | open the game menu from a terminal pane |
| Ctrl+Q | quit (while walking) |
| mouse click | click a terminal to type into it, the goal for the next hint, or the floor to walk again |
| mouse wheel | scroll the pane under the pointer |
| Ctrl+Shift+Page Up / Ctrl+Shift+Page Down | scroll the focused terminal’s history |
| left drag | highlight text in any pane and copy it to the primary selection |
| middle click | paste the primary selection into the agent or shell under the pointer |
| Ctrl+Alt+C | copy the selection |
| Ctrl+Alt+V | paste the game clipboard into the focused terminal |

You can also use your host terminal’s paste shortcut to paste from the system
clipboard. Highlighting and explicit copy update the host’s primary selection
and clipboard, respectively, when the host accepts OSC 52.

The buttons below the shell are off by default. Enable them under
**Esc → Level buttons**. The game remembers the setting.
They work like the Zell pane-room controls.
Walk onto a button and press Enter, or click it. **Clear model** starts
a fresh coding session. **Clear shell** clears the shell display.
**Shell log** puts `~/shell.log` in the agent's prompt. If the agent is
already running, review the draft and press Enter to send it. **Harness**
and **Model** open their settings, both reset buttons ask for confirmation,
and **Metrics** opens the leaderboard with the current run's tokens and
time. **Auto** turns automatic play on or off, the same switch as the
menu row.

Hints come one at a time, every 45 seconds or on H. Progress is kept in
`~/.local/share/puzzllm/progress`, so the game resumes in the room you
reached. If an agent or shell session ends, press Enter on its screen to
start a new one.

The game opens on its menu in this order: **Harness**, **Model**, then
**Start game** (or resume, or continue a saved level). Below those are
**Campaign**, **Scores**, **Display**, **Yes/No Bench**, **Auto**, **Reset**, and **Exit game**.
Enter opens a group. Escape steps back one page, and closes the menu once
play has started.

**Auto** is off until you turn it on. While it is on, each playable room
starts an agent session and tells it to complete the goal in `GOAL.md`.
A session that is already open and quiet for a couple of seconds gets the
same sentence. When the door opens, Kilix walks into the next playable
room and that room starts again. Auto stops in the last playable room.
An eligible campaign is ranked there as soon as the door opens and the
agent reports token use. The setting is remembered.

**Yes/No Bench** opens a progress monitor. Press **M** or click **Model** to
choose an Ollama tag, a llama.cpp GGUF, Codex, Grok, or Claude. For a local
model, choose from the installed list or enter a tag or file path. Ollama
pulls a missing tag when you start. For Codex, Grok or Claude, select a
named model from the CLI's catalog and local settings, use the configured
default, or enter a model ID. You can choose the next model while a benchmark
is running. The bench has its own selection, independent of the coding session
and campaign.

Press **R** or click **Start** to run the OneBit development questions. Each
question is a fresh request with the instruction `Respond with only YES or NO.`
After trimming surrounding whitespace, only uppercase `YES` and `NO` count
as valid. The monitor shows correct, wrong and invalid answers separately.
Accuracy counts every answered question. A provider failure ends the run
and preserves its partial results.

The player stays beside the controls at the bottom. Click an answer or use
the arrow keys or wheel to inspect it, **F** to filter misses, and **L** to
follow the latest answer again. **Esc** returns to the game. The benchmark
continues even if you close the tab. Reopen **Yes/No Bench**, or launch
`bin/puzzllm --yesno` in a separate terminal, to watch the same run. Use
**Stop** to end it. Each run saves its questions, replies, keys, timings,
and outcomes in `bench/yesno/*.jsonl` under the game state directory.

Press **B** or click **Rankings** to see one row per provider and model.
**Runs** counts every attempt. **Avg correct** is the mean number of correct
answers per finished run. **Avg %** is the mean accuracy of those runs and
determines rank. Tied models share a rank. Select a model to see every run,
then select a run to inspect its answers. **U** refreshes the rankings.
Older journals that omitted the default model's ID appear under
**Unrecorded default** with their original results.

The bench uses the OneBit data from the installed `kilix-bench` package
or the checkout named by `PUZZLLM_YESNO_BENCH`. Ollama uses `OLLAMA_HOST`.
GGUF discovery uses `KILIX_LLM_MODELS_DIR` and `PUZZLLM_MODEL`. A selected
GGUF runs on a private llama-server for the duration of the benchmark.

**Harness** toggles between OMP and Pi for a local model. **Model** chooses
Codex, Grok, Claude, llama.cpp, or Ollama, and holds the prompt, thinking,
compute, and Yolo settings. The llama.cpp and Ollama rows open their model lists.
Confirming a model from the opening menu starts the game. Model changes
restart the agent with a fresh conversation in the same workspace. Model
choices apply until you quit.

**Reset** asks before **Reset Level** or **Reset Game**, and backs up the
current workspace. **Reset Game** returns to the menu at level 1.
**Reset Level** reopens the current level with a fresh workspace, coding
session, and shell.

The resource bar shows the running token total reported by the current
agent session. It resets when a new session starts. A dash means usage has
yet to be reported.

**Leaderboard** saves local results in
`~/.local/share/puzzllm/leaderboard.tsv` (or the configured game state
directory). A run is recorded once when the player walks through the
unlocked door. The boards use runs with reported token usage from one
model. Set `PUZZLLM_PLAYER` to enter a display name. It defaults to the
local account name. Resetting the game or a level preserves past scores.

Use Left and Right to change rooms. In the model leaderboard, press M
to compare each model's best result by tokens, elapsed time, or peak
CPU, RAM, GPU, VRAM, or disk throughput. Unavailable resource samples
are omitted for that metric. Tab switches to player runs ranked by tokens
within each model.

**Campaign** is on the main menu. It locks the current model for every
playable room. Turn it on there. The choice is saved, and the game reopens on that
model. While it is on, Codex, Grok, Claude, llama.cpp, and Ollama stay on the
locked model. Finish every playable room with reported token use from
that model and the campaign is ranked. The unfinished stub room is not
part of the campaign. Walking through a room again keeps that room's
first result. **Campaign board** ranks finished campaigns. Tokens and
time are totals. CPU, RAM, GPU, VRAM, and disk use the highest room
peak. **Clear campaign board** asks for confirmation, then removes those
rankings. The current run and the room leaderboard stay. Reset Game
keeps the locked model and starts its rooms over. Reset Level redoes
that one room.

Run `~/check_goal_status.sh` from the player shell or the agent to grade the
current room immediately. It prints the goal and the first unmet check. Its
exit status is 0 for a completed goal, 1 when work remains, and 2 when the
grader is unavailable or the room has no playable goal.

Each room writes its goal to `GOAL.md` in the sandbox home, so the agent
and the shell can read it at `~/GOAL.md`. Entering a room replaces the
file with that room's goal. The same entry writes every hint for the room
to `~/HINTS.md`. **Display** → **Hint file** turns that file off. It is on
by default. The game archives departing guides as `~/prior_levels/GOAL_level_1.md`
and `~/prior_levels/HINTS_level_1.md` for Room 1, and likewise for later rooms.
Turning hints off archives the current hint file. Replaced versions get a
numeric suffix when an earlier copy already exists.

**Harness** toggles between OMP and [Pi](https://pi.dev/) for the selected
Ollama or llama.cpp model. If Pi is missing, the game installs it on first
launch into `runtime/pi/` under the game state. That launch needs internet
access. The installer uses npm and supplies a private Node.js runtime on
x86-64 and ARM64 Linux when a compatible Node.js and npm are unavailable.
For Pi sessions, the game uses host `fd` or `fdfind` when available.
Otherwise it installs a private copy of `fd` before starting the offline sandbox.
Switching restarts the agent in the same workspace. The choice is saved,
and Pi stores its game configuration and session history under `pi/`.

Choose **Codex (OpenAI)** to run the Codex CLI in the agent pane with the
same game workspace. The option needs Codex CLI with permission-profile
support and an internet connection. On first use, follow the device
sign-in instructions in the pane. Credentials and sessions are stored
under the game's separate `codex/` state directory. Codex's command
profile grants workspace access, denies access to that directory, and
disables command networking. The Codex client uses OpenAI's hosted service
when selected. Set `PUZZLLM_CODEX` to override the executable found on PATH.

Choose **Grok Build (xAI)** to use the native Grok Build CLI with the same
workspace. Install the native `grok` executable or set `PUZZLLM_GROK` to
its path. On first use, follow the device sign-in instructions in the
pane. Grok stores its sign-in separately in the game's `grok/` state
directory and runs with a strict sandbox that can also use the player
tmux server. Its session token
usage is currently available in Grok's own `/usage` view.

Choose **Claude Code (Anthropic)** to run Claude Code in the agent pane
with the same workspace. Install the `claude` executable or set
`PUZZLLM_CLAUDE` to its path. On first use, follow the sign-in instructions
in the pane. Claude stores its sign-in separately in the game's `claude/`
state directory. It asks before using tools until Yolo is on.

**Yolo** is on the Model page and is off by default. The choice is saved.
Turning it on restarts the agent and skips that harness's approval prompts:
`--yolo` for Codex and Grok, `--dangerously-skip-permissions` for Claude,
`--approval-mode=yolo` for OMP, and `--no-approve` for Pi. While it is off,
Codex keeps its game permission profile, Grok keeps its strict sandbox
with access to the player tmux server,
Claude and OMP ask, and Pi asks instead of skipping approval.

**Downloads** is the last row on the Model page. It downloads on this computer, not inside the sandbox. Suggested llama.cpp files are saved in the Kilix LLM models folder (`KILIX_LLM_MODELS_DIR`, or `GPU_TERMINAL_HOME/kilix-llm/models`, or `~/.local/gpu_terminal/kilix-llm/models`), which is the folder the game already searches for `Qwen3.5-0.8B-Q8_0.gguf`. Suggested Ollama models, and any other Ollama name you type, are pulled with `ollama pull`. A GGUF is not imported into Ollama. llama.cpp stays selectable beside Ollama. The sandbox bind-mounts the chosen GGUF read-only.

PuzzLLM requests **32,768 context tokens** from Ollama for every turn.
`PUZZLLM_CTX` changes the requested limit. The game checks the model's
reported maximum and requires at least 16,384 tokens. If Ollama reports
that it started a runner with less context than requested, the agent pane
shows an error instead of using the truncated completion.

**Tooltips** controls the yellow prompts above Kilix. They start off and
the game remembers your choice.

**Model prompt and tools** shows reference system-message counts for Qwen
3.5 and counts the saved prompt text with the local GGUF tokenizer when
available. Use Enter to edit the selected prompt and Ctrl+S to save.
The Ollama and llama.cpp prompts live in `~/.system/ollama-prompt.txt` and
`~/.system/llama-prompt.txt` inside the sandbox home. An empty prompt
file uses that backend's defaults. The OMP Ollama write-tool schema is
editable from the shell at `~/.system/write-tool.json`. Restart the agent
to pick up edits made in the shell. Use Left and Right to see which settings
are supplied by the other backends.

**Thinking** is off at startup and after switching models. The switch is
unavailable for models without reported thinking support. Changing it
restarts the agent. For models that always reason, Ollama uses their minimum
effort when the switch is off. Codex manages its own reasoning settings.

Ollama must be running locally. PuzzLLM reads the local model list using
Python 3. It uses port 11434 by default and respects a loopback
`OLLAMA_HOST`. Ollama models vary in their support for chat and tool calls.

## Level 1: Hello, world

Get the agent to create `~/hello_world.txt`. The goal is met when the
workspace holds a regular file of that name (a symlink does not count), of
1 to 4096 bytes, containing "hello" and "world" in any case. `Hello, World!`
passes and an empty file does not. The player may also create the file by
hand in the shell; that counts the same. A 0.8B model often says it wrote
the file when it did not, so check with `ls` and `cat` in the shell.

Follow the carpet up through an unlocked door to advance. Walk down off
the bottom of the carpet to return to the previous room or exit the game
from Room 1. By default your coding conversation, shell, and workspace carry
over. In **Between levels**, **Coding session** cycles through **Keep**,
**/clear** (clear the conversation in the running agent, using `/new` in
Codex and Pi), and **Restart**
(start a fresh agent process). **Clean up workspace each level** defaults to
**Off**. Turn it on to move your files and folders to
`~/prior_levels/level_N/` when leaving Room N. Revisiting that room
restores its archived work. **Clean up disk now** performs the same move
for the current room at any time. It leaves the active guides, shell
transcript and game settings available. With level buttons enabled, the
**Clean disk** floor button also performs this action.
With the game closed, run `bin/puzzllm --reset-progress` to start at level 1.
The previous workspace is kept under a `reset-*` directory in the game state.

## Levels are data

Levels live in [`levels/levels.txt`](levels/levels.txt), one block each:

```
level: 1
title: Hello, world
room: stone
goal: Get the agent to write Hello, World! to ~/hello_world.txt.
hint: Walk to the agent screen on the left and press Enter to talk to the agent.
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
`equals TEXT` (ignoring surrounding whitespace), `max-bytes N`,
`bytes N` (exactly N bytes, up to 16 MiB) or `only CHARS`
(no other byte anywhere). `room` picks the room's look: `stone`, `brick`, `press`, `archive`,
`vault`, `dock`, or `soon`. A new level is a new block. `check: script NAME` grades
that Bash file on hidden inputs, and `check: stamped-log` matches the latest
shell failure plus a stamp counted from the filename. Those two checks are
code; further rooms that use them are blocks. The full format is in
[`src/levels.h`](src/levels.h).

The levels so far:

1. **Hello, world:** `hello_world.txt` saying Hello, World!
2. **Strawberry:** count the r's in strawberry and write the number by itself
   to `~/strawberry.txt`. Surrounding whitespace is optional.
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
4. Count the files. Fix `~/count_files.sh` so it counts regular
   files directly inside the supplied `sample files` folder. The script
   also takes a folder path as its first argument. The grader checks the
   supplied folder and additional cases in a read-only sandbox.
5. The treasure chest. Extract `key.txt` from the password-protected
   `chest.zip` to open the door. The game uses `zip` to create the chest,
   and you can open it with `unzip` in the shell. Run `man unzip` in the
   sandbox to see the command reference. Resuming keeps the same chest.
6. **Follow the log.** Run `./run_check.sh` in the shell. Ask the agent to
   find the latest run ID and failing filename in tmux or `~/shell.log`
   and write them to `~/report.txt`, separated by one space. Rerunning
   the check makes earlier reports stale.
7. **The seal.** Write `~/seal.txt` by repeating the lowercase letters whose
   alphabet positions are multiples of 3 until the file is exactly 32 bytes,
   with no line ending.
8. **The mirror.** Write `~/mirror.txt` as that 32-byte seal, one comma, then
   the seal reversed. The file is exactly 65 bytes, with no line ending.
9. **The keeper.** Write `~/keep.sh` to read integers from stdin and print
   each one that exceeds its immediate predecessor. The grader supplies
   several inputs, including ties and negative numbers.
10. **The stamp.** Run `./run_check.sh` and report the latest run ID and
    failing filename. Append the number of letters in that filename whose
    alphabet positions are multiples of three.
11. A stub room for the next challenge.

The generated pixel art in `assets/` includes blue stone and red brick walls,
slate floor tiles, and a sliding wooden door. The sliding leaf reveals a black opening.

## The sandbox

The shell and local OMP and Pi agents run in [bubblewrap](https://github.com/containers/bubblewrap)
sandboxes started by [`libexec/puzzllm-sandbox`](libexec/puzzllm-sandbox):

- Every namespace is unshared, network included. The only reachable address
  is the sandbox's own loopback, so nothing in it can reach the host or the
  internet.
- `/usr` and a short list of files from `/etc` are read-only. The sandbox
  root is read-only and `/tmp` is a fresh tmpfs. The sandbox has its own
  persistent home. Nothing of the host user's home is visible.
- Two folders are writable, both under `~/.local/share/puzzllm/` (or
  `$PUZZLLM_STATE`), and both survive restarts:
  - `workspace/`, mounted as `/kilix`, the working directory. `$HOME` is
    `/opt/puzzllm/home`, a symlink to `/kilix`.
  - `agent/` for OMP or `pi/` for Pi, mounted separately at
    `/opt/puzzllm/agent` in the agent sandbox only. It holds configuration and sessions.
- In the agent sandbox, `puzzllm-agent-init` starts `llama-server` on the
  sandbox loopback with the Qwen weights mounted read-only. It waits for the
  server to answer, then starts OMP or Pi. The launcher writes the harness's
  provider configuration (`models.yml` or `models.json`) for `puzzllm-local`.
  If you select Ollama, the agent keeps its private network namespace and
  reaches only the chosen model's chat endpoint through a local Unix socket.

`make test-sandbox` probes the local agent and shell sandboxes. Writes outside
the workspace fail. The host loopback and outside addresses are unreachable.
The host home is invisible. Every writable mount is one of those above.
Nothing appears in a host folder next to the state. It also runs the whole
agent chain with stand-ins for llama-server and omp, and checks the separate
Codex and Claude launch paths with stand-in CLIs.

Both panes link the [NeuroTerm](https://github.com/itsmygithubacct/neuroterm)
embedded core from `third_party/neuroterm`. It supplies terminal parsing and
cell painting, including colors, wide characters, and cursors. PuzzLLM
supplies the PTYs, keyboard events, and the glyph and canvas backend. See
NeuroTerm's `embed.mk` and `include/neuroterm_terminal.h`.

### What it needs

| Part | Default | Override |
|---|---|---|
| tmux | `tmux` on `PATH` for the player shell | |
| bubblewrap | `bwrap` on `PATH` | |
| omp | `omp` on `PATH`, then `~/.local/bin/omp` | `PUZZLLM_OMP` |
| Pi | `pi` on `PATH`, then `~/.local/bin/pi`, then the game's private install | `PUZZLLM_PI`, `PUZZLLM_PI_NODE` |
| Claude Code | `claude` on `PATH`, then `~/.local/bin/claude` | `PUZZLLM_CLAUDE` |
| llama-server | `llama-server` on `PATH`, then a Kilix LLM bench build under `~/.local/share/kilix-llm/bench/engine/` | `PUZZLLM_LLAMA_SERVER` |
| Neuronte (drawing; optional) | a checkout at `~/projects/neuronte`, built from its two library sources | `make NEURONTE=DIR`, `RENDER=raster`; weights `PUZZLLM_NEURONTE_WEIGHTS` |
| Qwen 3.5 weights | `Qwen3.5-0.8B-Q8_0.gguf`, the model Zell routes to as `qwen3.5-0.8b`, in the Kilix LLM models folder | `PUZZLLM_MODEL` |

Model weights are supplied separately. If a part is missing, the coding pane says which one.
`libexec/puzzllm-sandbox paths` prints what was found. In the game menu,
select **Compute** and press Enter to cycle **Auto**, **CPU**, and **GPU**.
The choice is saved and applies to Ollama and llama.cpp. Auto lets Ollama
pick the device and defaults llama.cpp to CPU. In Auto or GPU mode,
`PUZZLLM_GPU=Vulkan1` can select a specific llama.cpp device. GPU mode
reports when llama.cpp finds no device or Ollama reports CPU-only placement.
`PUZZLLM_THINK=1`
lets the model think, which is off by default because 0.8B tends to loop.

## Tests

`make test` runs the following headless suites.

- `test-unit` covers the terminal emulator, key encoding, level parsing and
  checks (strawberry and the million ones included), JSON, the activity
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
- `test-kilix-display` checks window placement before capture and goal
  submission across agent restarts.
- `test-defects` introduces one regression at a time in a scratch copy.
  Each regression must fail its assigned suite. A raster-only build skips
  the checks that require Neuronte.

Test scratch stays under `build/`.

`build/puzzllm --render DIR` draws the room as PPM files: door shut, door
open, and room 2. `build/puzzllm --check-level N` says whether level N's goal
is met in the workspace now.

## Drawing

Kilix is zell-neuronte's: his four standing and sixteen walking pictures,
84 pixels tall, copied from Zell with their provenance in
[`src/kilix_art.c`](src/kilix_art.c). PuzzLLM paints an original flame emblem
on the visible shield faces. Kilix moves as Zell moves him. The room
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
  Room art and room text stay on that file. The terminal panes also open
  newer `neuronte-split-vN` files, `neuronte-term-*` bundles, and
  `neuronte-emoji-vN` from the weight file's directory and the usual
  Neuronte weight directories, and draw each glyph from the first file
  trained on it. That opener is `libneuronte-sdk`. Bundles from split-v9
  onward need a Neuronte that opens tiled members. When
  `~/projects/neuronte` is older and a checkout of its `origin/main` is
  visible, the Makefile links that checkout's `libneuronte` instead of
  compiling `neuronte.c` into the game.
- **System font**, the default. Fontconfig selects the system monospace
  font and FreeType draws it in the game's terminal cells. Building needs
  the Fontconfig and FreeType development packages, and running needs an
  installed monospace font. The built-in bitmap handles missing glyphs.

When the build supports Neuronte v2 and its weights are available, the
game menu offers a **Neuronte v2** toggle. It starts off and saves to
`~/.local/share/puzzllm/neuronte` or the configured game state directory.
Missing or unreadable weights leave the system font active.

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
