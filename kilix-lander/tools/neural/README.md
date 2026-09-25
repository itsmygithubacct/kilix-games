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

```sh
make lab
tools/neural/lander-lab --train WORK/run --hidden 32 --gens 400
tools/neural/lander-lab --pilot neural --weights WORK/run/best.raw --hidden 32 \
    --seed 9000000 --episodes 8000
tools/neural/lander-lab --pilot autopilot --seed 9000000 --episodes 8000
python3 tools/neural/install-policy.py WORK/run/best.raw --hidden 32 \
    --manifest-extra results.json
make test
```

One level of evaluation is episode k of a seed range: seed S+k, difficulty
k % 4, level 1 + (k/4) % 12, and one of ten fixed terminal sizes (k % 10).
Training, selection (5000000-5003999) and the one-shot held-out range
(9000000-9007999) never overlap. `make test`'s `--pilot-test` uses a fourth
range (8000000+).

## The shipped pilot

A 30-16-16-6 network (870 parameters). It was trained from scratch for 600
generations on levels 1-12 and selected at generation 420, then continued for
500 generations on levels 1-60 and selected at generation 440 (selection
98.5%). The single held-out run, 8000 levels 1-60: **98.4%**, against the
autopilot's 55.0% on the same levels. Easy and Medium land every time; Extra
Hard lands 93.6%. The full record, including the rejected 32- and 64-wide
runs, is in the manifest.
