# Kilix JPAK: Deep Salvage

Kilix JPAK is a complete clean-room action-puzzle game for Kitty-protocol
terminals. Guide Kilix, an orange star-vault salvager, through 100 deterministic
vaults. Recover every star mote, manage jet fuel, erode phase foam, operate
shape-coded gates and shutters, evade eight machine families, and reach the
iris exit.

This is a new game. It is neither the original DOS game nor an audiovisual
parity reconstruction. It does not load, copy, decode, or require any original
executable, level, graphics, palette, demo, startup, configuration, score, or
sound file.

![Kilix exploring a code-drawn starvault](docs/screenshot.png)

## What is included

- A finite 100-level campaign with 100 unique structural signatures derived
  from ten named macro families and more than 30 entry-to-iris routes
- Mandatory phase, shutter, and ring-gate side vaults, plus reservation-aware
  placement that keeps objectives, hazards, machines, and route anchors
  purposeful and safely reachable
- Movement-driven Kilix animation: walking and climbing advance a persistent
  gait phase, while eased gait amount drives body bob, alternating arms and
  magnetic boots, and a responsive tail
- Gravity, exact held-key movement, rail climbing, limited-fuel jet flight,
  and a phase tool
- Star-mote objective and animated exit progression
- Fuel cells, charge coils, siphons, treasure, extra lives, EMP, and shields
- Ice, lichen, bidirectional conveyors, thorns, paired teleporters,
  shape-coded switches and shutters, and two erodible phase materials
- One to fifteen distributed machines per vault, drawn from eight enemy
  families with local activation, visible warning tells, and independent
  movement models
- Four lives, scoring, fuel/time clear bonuses, game over, final victory,
  high-score storage, and persistent level unlocks
- Three-page in-game field manual and level selector with a layered semantic
  mini-map, route direction, pressure rating, and present-system tags
- Native Kitty graphics with a distinct procedural motif for every chapter,
  exact press/release input, responsive resize, and silent audio fallback
- Fourteen deterministic procedural sound roles, including a held jet loop
- Headless rules, input, campaign, renderer, and sanitizer checks

All game-specific graphics are drawn at runtime from C primitives. There are
no bitmap or sound assets to install.

## Build and run

Linux needs a C11 compiler, zlib, libm, pthreads, and a Kitty graphics-protocol
terminal such as Kitty, Ghostty, or WezTerm.

```sh
git submodule update --init --recursive
make
./kilix-jpak
```

Start a particular level for development or testing:

```sh
./kilix-jpak --level 37
```

The title screen continues from the highest unlocked vault; the selector can
revisit any unlocked level. `--level` starts a practice session and never
changes unlock progress or the campaign high score.

## Controls

| Key | Action |
|---|---|
| Left / A | walk left |
| Right / D | walk right |
| Up / W | climb a magnetic rail |
| Down / S | descend a magnetic rail |
| Space / Z | fire the micro-thruster while fuel remains |
| X / E | erode nearby purple phase foam |
| P / Esc | pause or resume |
| R | spend one life and restart the current vault |
| M | toggle sound |
| H | open or close the field manual |
| Q | leave a menu or return to the title from pause |
| Ctrl-C | restore the terminal and quit |

Menu controls use arrows or WASD and Enter. The game uses Kitty keyboard
press/release events when available, with a 0.30-second press-only compatibility
latch for older terminals.

## Objective and rules

Every vault contains five to eight cyan star motes. The circular exit iris is
locked until all motes have been collected. Fuel powers only the thruster;
walking and magnetic rails always remain available. Charge coils refill fuel
while Kilix occupies them and purple siphons drain it. If the tank is emptied,
safe footing slowly restores an 18% emergency reserve, preventing fuel
softlocks without providing free charge in flight.

The named stages `FIRST LIGHT`, `CROSSLINK`, `SWITCHBACK`, `FLOATING KEYS`,
`PHASE LESSON`, `TWIN ASCENT`, `BROKEN ORBIT`, `SHUTTER TEST`, `FALSE FLOOR`,
and `DEEP GATE` define ten macro families. Chapter-specific mirroring, carved
gaps, added ledges, materials, and fixtures turn those families into 100 unique
structural signatures. The campaign uses more than 30 entry-to-iris route
pairs, alternating insertion sides and seeking a high supported iris toward
the opposite side.

Focus-mechanic levels build real side vaults around phase foam, linked
switches and shutters, or paired ring gates, then validate that the featured
system is required to traverse them. Reservation-aware placement protects
entry and iris anchors, rails, mandatory cells, objectives, and safe routes;
hazards and machines are added only after those purposeful spaces are secured.

Purple quantum foam can be dissolved temporarily by holding phase next to it.
Dense foam takes longer. Gray basalt phase locks cannot be dissolved. Ring
gates match by both color and center glyph. The same circle, triangle, and
square language connects switches to energy shutters, so the interactions do
not depend on color perception alone.

EMP freezes nearby enemies. Aegis lets Kilix survive contact and knock enemies
back. Either effect lasts ten seconds. Unprotected machine or thorn contact
costs one life and restarts the current vault. Machines remain dormant outside
their local detection range and display a nonlethal activation tell before
moving; retries restore the score banked at deployment while keeping elapsed
vault time.

## Data inspection

The campaign is deterministic and can be inspected without a terminal. Dump
the complete manifest or one level's annotated semantic grid:

```sh
./kilix-jpak --dump-campaign
./kilix-jpak --dump-level 37
```

User progress is a 24-byte, versioned, checksummed profile stored under:

```text
${XDG_DATA_HOME:-$HOME/.local/share}/kilix-jpak/profile.v1
```

`KILIX_JPAK_DATA_HOME` overrides the parent data directory for tests and
portable packaging. Profile writes use a mode-0600 temporary file, `fsync`,
atomic rename, and directory `fsync`. A corrupt or newer-format file is ignored
without partially applying it.

## Development and verification

```sh
make test                           # complete deterministic suite
make test-fast                      # shorter local loop
make sanitize                       # ASan + UBSan rules/campaign run
./kilix-jpak --rules-test
./kilix-jpak --input-test
./kilix-jpak --selftest 1337 12000
./kilix-jpak --render-test 7        # writes twenty-two PPM scenes
./kilix-jpak --sound-test
```

The render test checks that drawing a scene does not mutate `GameState`.
Simulation has no atlas pointer or graphics-derived values; changing a sprite
silhouette cannot change collision, AI, teleporter behavior, or gameplay RNG.
Its 22-scene corpus includes the title, layered selector mini-map, all three
manual pages, three macro-family showcases, three mandatory mechanic side
vaults, the same stage varied across three chapters, two opposite walking
strides, live play, pause, clear, life-lost, game-over, and victory
compositions.

## Architecture

| File | Responsibility |
|---|---|
| `src/data.c` | ten macro families, 100 unique vault structures, safe purposeful placement, titles, semantic names, reachability/mechanic audits |
| `src/game.c` | fixed-step physics, collisions, mechanics, enemies, progression, scoring, atomic profile |
| `src/render.c` | code-native game art, per-chapter motifs, layered selector/HUD scenes, logical-to-terminal scaling |
| `src/sound.c` | deterministic 44.1 kHz procedural synthesis and PCM mixing |
| `src/term.c` | shared Kitty session adapter, held input, presentation, resize, restoration |
| `src/main.c` | terminal loop, CLI, rules/input/campaign/render test modes |

Shared libraries are pinned as Git submodules:

| Library | Use |
|---|---|
| `kitty-terminal-session` | Kitty framebuffer and keyboard lifecycle |
| `soft-raster` | clipped RGBA primitives, nearest scaling, public-domain console font |
| `pcm-mixer` | mono PCM voices and CLI-sink transport |

## License and provenance

Kilix JPAK game code, generated graphics, generated sound definitions, level
grammar, names, and prose are MIT licensed. See [LICENSE](LICENSE).

No external bitmap, recording, screenshot, traced silhouette, generative-image
model output, or original-game binary asset is used. The embedded font comes
from `soft-raster` and carries its own public-domain provenance in that
submodule. The shared libraries retain their respective licenses.
