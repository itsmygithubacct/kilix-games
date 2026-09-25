# Neural pilot lab

Kilix Lander's NEURAL pilot is a small network trained here, entirely in the
game's own simulation. Each tick it maps 30 features of what a pilot can see
to one of 6 actions (main thrust off/on × side none/left/right). It ships as
a kilix-game-kit `KXPOLICY` blob, `assets/policy/lander-neural.kxpol`. The
blob is compiled in through `src/neural_policy_blob.h` and run by `src/pilot.c`
with the kit's `kilix_game_policy`. Provenance is in
`docs/neural-policy-provenance.json`, and `make check-policy` (part of
`make test`) proves the blob, the header and the manifest agree.

## Pieces

| File | Role |
|---|---|
| `lander_lab.c` | Headless lab linked against the game's `game.o` (`make lab`). It evaluates the autopilot or a network, records demonstrations and trains with evolution strategies. Networks run through `kilix_policy_forward`, the same call the game makes. |
| `clone.py` | Behaviour cloning from `--dump` demonstrations (numpy). It was an optional warm start; the shipped pilot did not need it. |
| `install-policy.py` | Packs raw weights with the kit's `kilix_policy.py`, regenerates the header and writes the manifest. |
| `check-policy.py` | The `make check-policy` gate. |

## Inputs

`game_policy_features()` in `src/game.c` is the single definition. Lengths and
speeds are divided by the game's screen scale, so any terminal size looks
alike. The inputs are: the offset to the pad, height over the pad, velocity,
tilt and spin, fuel, pad width, clearance below, six terrain probes around
the lander, distance to both screen edges, and the difficulty's physics
(gravity, thrust, safe speed and angle, stabiliser, damping, drag, fuel rates,
pad grace).

## Training

Evolution strategies (antithetic pairs, rank-shaped fitness, Adam, sigma 0.05)
optimise the landing outcome directly, so there is no teacher to imitate. Each
generation flies every perturbed network over the same batch of 96 levels:
all four difficulties, levels 1-12, and a random landscape terminal from
640x400 to 3840x2160. A landing scores 1 plus a little for fuel left. A crash
or a timeout earns a shaped 0-0.8 for how close it came (over the pad, then
speed and tilt), so a generation that lands nothing still has a direction.
Workers are forked processes, because the game keeps its state in one global.

The shipped pilot was made in two stages, then checked once on held-out
levels 1-60 (the release protocol):

```sh
make lab
# stage 1: from scratch, levels 1-12 (selection 5000000-5003999)
tools/neural/lander-lab --train WORK/s1 --hidden 16 --gens 600
# stage 2: continue on levels 1-60 (selection 5100000-5103999)
tools/neural/lander-lab --train WORK/s2 --hidden 16 --gens 500 --init WORK/s1/best.raw \
    --train-max-level 60 --max-level 60 --select-seed 5100000
# the one held-out run, levels 1-60, and the autopilot on the same levels
tools/neural/lander-lab --pilot neural --weights WORK/s2/best.raw --hidden 16 \
    --seed 9100000 --episodes 8000 --max-level 60
tools/neural/lander-lab --pilot autopilot --seed 9100000 --episodes 8000 --max-level 60
python3 tools/neural/install-policy.py WORK/s2/best.raw --hidden 16 \
    --manifest-extra results.json
make test
```

The shipped blob reproduces the held-out figures exactly:

```sh
tools/neural/lander-lab --pilot neural --blob assets/policy/lander-neural.kxpol \
    --seed 9100000 --episodes 8000 --max-level 60     # 7869/8000 landed
```

One level of evaluation is episode k of a seed range: seed S+k, difficulty
k % 4, level 1 + (k/4) % N (N = 60 for the release; the lab's default
`--max-level` is 12, stage 1's range), and one of ten fixed terminal sizes
(k % 10). Training (1000000+), selection and the one-shot held-out range
never overlap; stage 1 used held-out 9000000-9007999 once (99.99% on levels
1-12) before stage 2 moved to fresh ranges. `make test`'s `--pilot-test`
uses a fourth range (8000000+).

## The shipped pilot

A 30-16-16-6 network (870 parameters). It was trained from scratch for 600
generations on levels 1-12 and selected at generation 420, then continued for
500 generations on levels 1-60 and selected at generation 440 (selection
98.5%). The single held-out run, 8000 levels 1-60: **98.4%**, against the
autopilot's 55.0% on the same levels. Easy and Medium land every time; Extra
Hard lands 93.6%. The runs not selected: a 32-wide network reached 99.98%
selection in stage 1 and 96.2% in stage 2; a 64-wide network trained from
scratch with the same settings never took off (44.9% after 600 generations).
The manifest records the selected network's training, selection and held-out
results.
