# Neural rider lab

Joustix's NEURAL rider is a small network trained here, entirely in the game's
own simulation. Every tick it reads 47 features and picks one of 6 actions:
direction (left, none, right) with or without a flap. It ships as a
kilix-game-kit `KXPOLICY` blob, `assets/policy/joustix-neural.kxpol`, compiled
in through `src/neural_policy_blob.h` and run by `src/pilot.c` with the kit's
`kilix_game_policy`. Provenance is in `docs/neural-policy-provenance.json`.
`make check-policy` (part of `make test`) proves the blob, header and manifest
agree, and `tools/neural/test-check-policy.py` proves each of those checks can
fail.

## Pieces

| File | Role |
|---|---|
| `joustix_lab.c` | Headless lab linked against the game's `game.o` (`make lab`). It evaluates the autopilot or a network, records demonstrations, and trains with evolution strategies. Networks run through `kilix_policy_forward`, as in the game. |
| `install-policy.py` | Packs raw weights with the kit's `kilix_policy.py`, regenerates the header and writes the manifest. |
| `check-policy.py`, `test-check-policy.py` | The `make check-policy` gate and its negative test. |

## Inputs

`game_policy_features()` in `src/game.c` is the single definition, in the
game's fixed 320x180 logical units. It covers:

- the rider: position, velocity, on a platform, can flap, invulnerable,
  respawning, height above the lava, distance to the surface below;
- the three nearest enemies: wrapped offset, velocity, type, still spawning;
- the two nearest eggs: offset, grounded, time to hatch;
- the lava troll's position and timing;
- the number of enemies and eggs left, and the difficulty.

## Training

Evolution strategies (antithetic pairs, rank-shaped fitness, Adam, sigma 0.05,
population 64). Each generation flies every perturbed network over the same 48
waves, with the difficulty and a wave from 1 to 12 drawn from the seed. The
reward is the share of the wave's points earned (each rider's joust value plus
a prompt egg), plus 1 and up to 0.5 more for clearing it sooner, minus a
penalty for losing the rider (the episode then ends).

Evaluation episode k of a range starting at seed S: seed S+k, difficulty
k % 3, wave 1 + (k/3) % 12. It ends on a clear, a lost rider or 120 s.
Training (1000000+), selection (5000000-5002399) and the one-shot held-out
range (9000000-9004799) never overlap; `make test`'s `--pilot-test` uses
8000000+.

## The shipped rider

A 47-32-32-6 network (2,790 parameters). It was trained from scratch for about
100 generations (60 s episodes, death penalty 1.0), plateauing at an 85%
selection clear rate, and selected at generation 75. A diagnosis showed every
lost rider was a joust lost to a higher enemy, mostly in waves 10-12. It was
then continued for 200 generations with 120 s episodes and the death penalty
tripled, and selected at generation 150 (selection 89.5% clear, 10.5% lost).
The one held-out run, 4800 waves:

| | Neural | Autopilot |
|---|---|---|
| clean clears | 4284 (89.25%) [88.3, 90.1] | 48 (1.0%) [0.8, 1.3] |
| rider lost | 515 (10.7%) [9.9, 11.6] | 4743 (98.8%) |
| mean share of points | 94.2% | 1.7% |

The shipped blob reproduces the run exactly:

```sh
tools/neural/joustix-lab --pilot neural --blob assets/policy/joustix-neural.kxpol \
    --seed 9000000 --episodes 4800        # 4284 cleared
```
