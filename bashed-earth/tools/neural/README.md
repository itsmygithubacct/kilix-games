# Neural gunner lab

Bashed Earth's **Neural** personality is a small network trained here,
entirely in the game's own simulation. On its turn it reads 44 features and
answers, in one forward pass, with a weapon, a barrel angle and a power: no
trajectory search. The classic personalities aim by searching simulated
trajectories and then add personality noise. The network ships as a
kilix-game-kit `KXPOLICY` blob, `assets/policy/bashed-earth-neural.kxpol`,
compiled in through `src/neural_policy_blob.h` and run by `src/neural.c`
with the kit's `kilix_game_policy`. Provenance is in
`docs/neural-policy-provenance.json`. `make check-policy` (part of
`make test`) proves the blob, header and manifest agree, and
`tools/neural/test-check-policy.py` proves each of those checks can fail.

## Pieces

| File | Role |
|---|---|
| `bashed_earth_lab.c` | Headless lab linked against the game's objects (`make lab`). It cuts scenario banks from real matches, evaluates shooters on them, records demonstrations and ballistic labels, plays duels, and trains with evolution strategies. Networks run through the game's own `neural_do_turn()` with the candidate swapped in by `neural_use_policy()`. |
| `fit-demos.py` | Fits a network to ballistic labels (aim) and the classic AIs' weapon choices (numpy). |
| `install-policy.py` | Packs raw weights with the kit's `kilix_policy.py`, regenerates the header and writes the manifest. |
| `check-policy.py`, `test-check-policy.py` | The `make check-policy` gate and its negative test. |

## Inputs and outputs

`neural_features()` in `src/neural.c` is the single definition. Everything is
seen from the shooter and mirrored so the target is to the right; pixels are
divided by 1000 (the field is the terminal's pixel size, so it varies):

- the target's offset, the wind, max power, both tanks' health, the target's
  shield, wall bounce, the distance to the wall behind and past the target,
  and how deeply each tank is buried;
- 16 terrain heights between shooter and target and 4 past it;
- 12 flags: which fireable weapons are in stock.

The 14 outputs are 12 weapon scores (argmax over the weapons in stock), the
barrel angle `5 + 83*sigmoid` degrees and the power `10 + 90*sigmoid`,
clamped to the tank's max power. The target is chosen by the classic rule
(weakest living opponent, ties to the nearest).

## Data

A **scenario** is a turn cut from a real match between classic AIs (2-4
tanks; personalities, a 700-2000 px field, terrain, wind, precipitation and
wall bounce all drawn from the seed), stopped where an AI is about to aim.
Replaying it with a different shooter gives a turn reward: damage dealt, plus
20 per enemy shield broken, minus damage taken, plus 25 per kill, over 100.

A **duel** is one whole match against one classic personality: duel k of a
range starting at seed S is match seed S+k against personality k % 5, the
gunner seated first when k/5 is even.

Seed ranges never overlap: training banks and labels 1,000,000+ and
2,000,000+, selection 5,000,000+ (bank) and 5,100,000+ (duels), `make test`'s
fixed duels 8,000,000+, held-out 9,000,000+ (duels) and 9,500,000+ (bank),
each held-out range evaluated once.

## How the shipped gunner was made

The protocol was frozen before training (with three logged amendments):

1. Evolution strategies from scratch (as in the Joustix lab) learned slowly:
   0.07 selection-bank reward after 30 generations, against 0.156 for the
   classic shooter.
2. Fitting the network to the classic AIs' own shots reached only 0.09: their
   labels carry personality aim noise, and the search lands on very
   different angles for similar shots, so a fit averages them.
3. Aim labels were then computed from the game's own flight rule: for each
   state, the exact shot found by simulated search (a 50 degree barrel when
   it lands within 20 px, otherwise the first of 60, 70, 80, 40 or 30 degrees
   that does). They came from 100,000 variants of 4,000 training turns, with
   the wind redrawn and the target moved (`--oracle`). Weapon labels are the
   classic AIs' choices over 72,939 decisions in 2,000 matches (`--dump`).
   `fit-demos.py` fits both, 60 epochs.
4. Evolution strategies continued from that fit only lowered the selection
   reward (0.274 to 0.231 in 20 generations), so it was stopped.

Selection: the three checkpoints with the best selection-bank reward played
1,000 selection duels each. The 44-64-64-14 fit won 943 (the 44-32-32-14 fit
934, the ES generation-10 network 903) and ships: 7,950 parameters.

## Held-out results

2,000 duels, seeds 9,000,000-9,001,999, evaluated once:

| Gunner seat | Wins | Win rate [Wilson 95%] |
|---|---|---|
| **Neural** | **1873** | **93.7% [92.5, 94.6]** |
| Balanced | 1153 | 57.7% [55.5, 59.8] |
| Aggressive | 1132 | 56.6% [54.4, 58.8] |
| Trickster | 1062 | 53.1% [50.9, 55.3] |
| Defensive | 884 | 44.2% [42.0, 46.4] |
| Tactical | 841 | 42.1% [39.9, 44.2] |

Neural by opponent: Aggressive 93.3%, Defensive 96.8%, Tactical 98.3%,
Balanced 88.0%, Trickster 92.0%; seated first 956/1000, second 917/1000;
mean 8.3 turns per duel. The classic rows are the same 2,000 duels with that
personality in the gunner's seat. They show what an even match looks like
on these seeds.

On 1,000 held-out turns (bank seeds 9,500,000+), replayed with each
shooter: Neural's mean turn reward was 0.262 and it damaged someone on 75.1%
of turns [72.3, 77.7]; the classic shooter's figures were 0.168 and 43.7%
[40.7, 46.8].

The shipped blob reproduces the duel run exactly:

```sh
make lab
tools/neural/bashed-earth-lab --duels 2000 --seed 9000000 \
    --blob assets/policy/bashed-earth-neural.kxpol      # 1873 wins
```

## Reproducing the training

```sh
L=tools/neural/bashed-earth-lab
$L --build-bank train.bank --seed 1000000 --count 4000
$L --build-bank select.bank --seed 5000000 --count 800
$L --oracle aim.bin --scenarios train.bank --variants 25
$L --oracle select-aim.bin --scenarios select.bank --variants 5
$L --dump weapons.bin --seed 2000000 --count 2000
$L --dump select-weapons.bin --scenarios select.bank
python3 tools/neural/fit-demos.py --aim aim.bin --weapons weapons.bin \
    --select-aim select-aim.bin --select-weapons select-weapons.bin \
    --hidden 64 --out gunner.raw
$L --duels 1000 --seed 5100000 --weights gunner.raw --hidden 64
python3 tools/neural/install-policy.py gunner.raw --hidden 64 --manifest-extra extra.json
```

Every lab mode points `HOME` and `XDG_CONFIG_HOME` at a private scratch
directory, so none can touch a player's saved setup.
