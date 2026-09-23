# Neural player and CPU-level lab

Kilix Pong's NEURAL controller is a small classifier trained here. It maps
11 features of the table (mirrored, so it can play either side) to up, stay
or down, 60 times a second. It has 5,123 parameters and ships as a
kilix-game-kit `KXPOLICY` blob. The blob is compiled into the binary from
`src/neural_policy_blob.h`, and its provenance is in
`docs/neural-policy-provenance.json`. `make check-policy` (part of
`make test`) proves the blob, the header and the manifest agree.

Nothing here is third-party: every label comes from the game's own
simulation.

## Pieces

| File | Role |
|---|---|
| `pong_lab.c` | Headless lab linked against the game's `game.o` (`make lab`). It plays scripted players or the network against the CPU levels, writes training dumps and prints JSON summaries. |
| `train.py` | PyTorch trainer (CPU is fine). Cross-entropy on planner labels; fits a dev temperature; writes the blob through the kit's `kilix_policy.py`. |
| `train-policy.sh` | The whole pipeline: planner demonstrations, DAgger rounds, dev tournaments and a fixed selection rule. |
| `install-policy.py` | Installs the selected blob, regenerates the header and writes the provenance manifest. |
| `check-policy.py` | The `make check-policy` gate. |

## How the labels are made

The `planner` looks for points it can win. While the ball is heading for
its paddle, every 6 ticks it tries 11 strike offsets on the paddle face. For
each offset it copies the whole `GameState` (everything, including the CPU's
state, lives in `G`) and reseeds the copy's RNG 6 times, sampling the CPU's
hidden reaction jitter, aim error and english. It plays each copy to the end
of the point and keeps the offset with the best mean outcome. The labels are
a pure function of the state. The network then learns to reproduce the
planner's up / stay / down choice from the features alone. After the first
round of demonstrations, DAgger rounds let the network play (with planner
takeovers at probability 0.5^k) and relabel every state it actually reaches.

## CPU levels

`cpu_levels` in `src/game.c` was tuned with the lab's `reactive` player, a
human proxy: it reacts after 0.20-0.27 s and its aim error grows with ball
speed and wall bounces. Match wins over 60 matches on seeds 1000..:

| Level | Proxy wins | Points | Planner wins |
|---|---:|---:|---:|
| EASY | 59/60 | 659-282 | - |
| NORMAL | 41/60 | 584-511 | 4/4 (44-0) |
| HARD | 17/60 | 480-593 | 4/4 (44-0) |

## Regenerate

```sh
make lab
WORK=$HOME/.local/share/kilix-pong-train PYTHON=/path/to/python-with-torch \
    tools/neural/train-policy.sh
python3 tools/neural/install-policy.py "$WORK" --test test.jsonl
make test
```

The procedure is reproducible but not bit-exact: PyTorch CPU kernels may
differ between builds. That is why the shipped bytes are pinned by sha256
instead of rebuilt.
