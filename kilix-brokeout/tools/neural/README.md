# Neural player lab

Kilix Brokeout's NEURAL player is a small network trained here, entirely in the
game's own simulation. Every tick it reads 38 features of the table and
chooses **where on the paddle face to strike the most urgent ball**: one of 9
points from the far left edge through the centre to the far right edge.
`game_apply_action()` in `src/game.c` then moves the paddle to meet the ball
at that point, predicting where the ball will cross the paddle (wall bounces
included). It also launches a ball resting on the paddle after 0.35 s. The
network decides the shot, which is what clears a level: the angle a ball
leaves the paddle at depends on where it strikes.

It ships as a kilix-game-kit `KXPOLICY` blob, `assets/policy/brokeout-neural.kxpol`,
compiled in through `src/neural_policy_blob.h` and run by `src/player.c` with
the kit's `kilix_game_policy`. Provenance is in
`docs/neural-policy-provenance.json`, and `make check-policy` (part of
`make test`) proves the blob, header and manifest agree.

## Pieces

| File | Role |
|---|---|
| `brokeout_lab.c` | Headless lab linked against the game's `game.o` (`make lab`). It evaluates the autopilot, the lab's "aimer" heuristic or a network, records demonstrations, and trains with evolution strategies. Networks run through `kilix_policy_forward`, as in the game. Its XDG directories point at a scratch directory, so the player's high-score store is never touched. |
| `clone.py` | Behaviour cloning from `--dump` demonstrations (numpy); tried as a warm start, not used by the shipped player. |
| `install-policy.py` | Packs raw weights with the kit's `kilix_policy.py`, regenerates the header and writes the manifest. |
| `check-policy.py` | The `make check-policy` gate. |

## Inputs

`game_policy_features()` is the single definition, relative to the playfield so
any terminal size looks alike. It covers the paddle's position and width; the
two most urgent balls (position, velocity, predicted crossing relative to the
paddle, time to arrive, resting on the paddle); the remaining bricks' hit
points in eight columns, their centre and lowest row; the nearest falling
capsule; the wide, shield and overdrive timers; extra balls; and ball speed.

## What it had to learn

The game's scripted autopilot centres the paddle under the ball, so returns go
straight up and the ball loops. It rarely loses the ball, but it clean-clears
under 1% of levels in three minutes. Rallying brick by brick takes around a
thousand seconds per level. Fast clears come from tunnelling through to the
top, where the ball bounces among the bricks from above. The lab's aimer
heuristic, which strikes toward the weakest column, showed this (18% clean
clears) but lost more balls than the autopilot. The network learned
tunnelling and, with lost balls penalised hard, to keep the ball.

## Training

Evolution strategies (antithetic pairs, rank-shaped fitness, Adam, sigma 0.05,
population 64). Each generation plays every perturbed network on the same 48
levels: a random level 1-20 on a random landscape terminal from 640x400 to
3840x2160. The reward is the share of brick hit points removed, plus 1 and up
to 0.5 more for clearing the level sooner, minus a penalty for losing the ball
(the episode then ends).

Evaluation episode k of a range starting at seed S: seed S+k, level
1 + (k/10) % 20, one of ten fixed terminal sizes (k % 10). It ends when the
level is cleared, the ball is lost, or after 180 s. Training (1000000+),
selection (5000000-5001999) and the one-shot held-out range (9000000-9003999)
never overlap; `make test`'s `--player-test` uses 8000000+.

## The shipped player

A 38-32-32-9 network (2,601 parameters). It was trained from scratch for 50
generations (60 s episodes, lost-ball penalty 1.5), then continued for 200
generations with 120 s episodes and the penalty doubled to 3.0, after a
diagnosis showed its lost balls came almost all from extreme strike points on
fast balls. Selection (generation 200): clean clears 16.7%, ball lost 6.35%.
The one held-out run, 4000 levels:

| | Neural | Autopilot |
|---|---|---|
| clean clears | 596 (14.9%) [13.8, 16.0] | 17 (0.4%) [0.3, 0.7] |
| ball lost | 282 (7.05%) [6.3, 7.9] | 278 (6.95%) [6.2, 7.8] |
| mean share of bricks removed | 84.6% | 24.8% |

By level band, the neural player clears 20.4 / 11.4 / 14.3 / 13.5% of levels
1-5 / 6-10 / 11-15 / 16-20; the autopilot 0.9 / 0.3 / 0.3 / 0.2%. The shipped
blob reproduces the run exactly:

```sh
tools/neural/brokeout-lab --pilot neural --blob assets/policy/brokeout-neural.kxpol \
    --seed 9000000 --episodes 4000        # 596 cleared, 282 balls lost
```
